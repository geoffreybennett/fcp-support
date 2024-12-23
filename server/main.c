// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/param.h>
#include <alsa/asoundlib.h>

#include "device-ops.h"
#include "fcp-socket.h"
#include "log.h"

static void usage(const char *argv0) {
  log_error("Usage: %s <card-number>", argv0);
}

static int process_control_event(struct fcp_device *device) {
  snd_ctl_event_t *event;
  snd_ctl_elem_id_t *event_id;
  snd_ctl_elem_value_t *value;

  snd_ctl_event_alloca(&event);
  snd_ctl_elem_id_alloca(&event_id);
  snd_ctl_elem_value_alloca(&value);

  // Read the event
  int err = snd_ctl_read(device->ctl, event);
  if (err < 0)
    return err;

  // Only handle element events
  if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM)
    return 0;

  snd_ctl_event_elem_get_id(event, event_id);
  snd_ctl_elem_value_set_id(value, event_id);

  err = snd_ctl_elem_read(device->ctl, value);
  if (err < 0) {
    log_error("Cannot read control value: %s", snd_strerror(err));
    return err;
  }

  return device_handle_control_change(device, event_id, value);
}

static int run(struct fcp_device *device) {
  int ctl_fd, hwdep_fd;
  int err;

  // Get file descriptors for monitoring
  device_get_fds(device, &ctl_fd, &hwdep_fd);

  // Subscribe to control changes
  err = snd_ctl_subscribe_events(device->ctl, 1);
  if (err < 0) {
    log_error("Cannot subscribe to events: %s", snd_strerror(err));
    return err;
  }

  // Main event loop
  while (1) {
    fd_set rfds;
    int nfds = -1;

    FD_ZERO(&rfds);
    FD_SET(ctl_fd, &rfds);
    FD_SET(hwdep_fd, &rfds);
    nfds = MAX(nfds, MAX(ctl_fd, hwdep_fd));

    // Add socket fds
    fcp_socket_update_sets(&rfds, &nfds);
    nfds++;

    err = select(nfds, &rfds, NULL, NULL, NULL);
    if (err < 0) {
      if (errno == EINTR)
        continue;
      log_error("Select failed: %s", strerror(errno));
      break;
    }

    // Handle control events
    if (FD_ISSET(ctl_fd, &rfds)) {
      err = process_control_event(device);
      if (err == -ENODEV) {
        err = 0;
        log_debug("Control interface closed");
        break;
      }
      if (err < 0) {
        log_error("Control event processing failed: %s", snd_strerror(err));
        break;
      }
    }

    // Handle device notifications
    if (FD_ISSET(hwdep_fd, &rfds)) {
      uint32_t notification;
      err = snd_hwdep_read(device->hwdep, &notification,
                sizeof(notification));
      if (err < 0) {
        log_error("Cannot read notification: %s", snd_strerror(err));
        break;
      }
      device_handle_notification(device, notification);
    }

    // Handle socket events
    fcp_socket_handle_events(&rfds);
  }

  return err;
}

int main(int argc, char *argv[]) {
  struct fcp_device device;
  int card_num;
  int err;

  log_init();

  // Parse command line
  if (argc != 2) {
    usage(argv[0]);
    return 1;
  }

  errno = 0;
  card_num = strtol(argv[1], NULL, 10);
  if (errno || card_num < 0) {
    log_error("Invalid card number: %s", argv[1]);
    return 1;
  }

  // Initialise device
  err = device_init(card_num, &device);
  if (err < 0) {

    // Quietly ignore if FCP is not supported
    if (err == -ENOPROTOOPT)
      return 0;

    log_error("Device initialisation failed: %s", snd_strerror(err));
    return 1;
  }

  // Load device configuration
  err = device_load_config(&device);
  if (err < 0)
    return 1;

  // Initialise controls
  err = device_init_controls(&device);
  if (err < 0)
    return 1;

  // Initialise socket interface
  err = fcp_socket_init(&device);
  if (err < 0)
    return 1;

  log_info("fcp-server %s ready", VERSION);

  // Run main event loop
  err = run(&device);

  // Cleanup handled by OS
  return err < 0 ? 1 : 0;
}
