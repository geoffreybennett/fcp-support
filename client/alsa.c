// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "alsa.h"
#include "devices.h"
#include "usb.h"

#define MAX_TLV_RANGE_SIZE 1024

static int get_usb_id(const char *card_name, int *out_vid, int *out_pid) {
  char proc_path[256];
  char usbid[10];
  int pid, vid;

  snprintf(proc_path, sizeof(proc_path), "/proc/asound/%s/usbid", card_name);

  FILE *f = fopen(proc_path, "r");
  if (!f)
    return 0;

  if (fread(usbid, 1, sizeof(usbid), f) != sizeof(usbid)) {
    fclose(f);
    return 0;
  }
  fclose(f);

  if (sscanf(usbid, "%x:%x", &vid, &pid) != 2)
    return 0;

  if (vid != VENDOR_VID)
    return 0;

  *out_vid = vid;
  *out_pid = pid;

  return 1;
}

static char *get_socket_path(snd_ctl_t *ctl, int card_num, bool quiet) {
  snd_ctl_elem_id_t *id;
  snd_ctl_elem_info_t *info;
  snd_ctl_elem_value_t *value;
  unsigned int tlv[MAX_TLV_RANGE_SIZE];
  int err;

  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_alloca(&value);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
  snd_ctl_elem_id_set_name(id, "Firmware Version");

  snd_ctl_elem_info_set_id(info, id);
  snd_ctl_elem_value_set_id(value, id);

  if ((err = snd_ctl_elem_info(ctl, info)) < 0) {
    if (!quiet)
      fprintf(
        stderr,
        "Firmware Version not found for card %d (is fcp-server running?)\n",
        card_num
      );
    return NULL;
  }

  // check if the element is a user control
  if (!snd_ctl_elem_info_is_user(info)) {
    if (!quiet)
      fprintf(
        stderr,
        "Firmware Version control for card %d is not a user control "
          "(use scarlett2, not fcp-tool for managing this card)\n",
        card_num
      );
    return NULL;
  }

  // check if the element is locked
  if (!snd_ctl_elem_info_is_locked(info)) {
    if (!quiet)
      fprintf(
        stderr,
        "Firmware Version control for card %d is not locked "
          "(is fcp-server running?)\n",
        card_num
      );
    return NULL;
  }

  if (!snd_ctl_elem_info_is_tlv_readable(info)) {
    if (!quiet)
      fprintf(stderr, "Firmware Version ctl element is not TLV readable\n");
    return NULL;
  }

  err = snd_ctl_elem_tlv_read(ctl, id, tlv, sizeof(tlv));
  if (err < 0) {
    if (!quiet)
      fprintf(
        stderr,
        "Error reading TLV data from Firmware Version ctl element: %s\n",
        snd_strerror(err)
      );
    return NULL;
  }

  // SCKT
  if (tlv[0] != 0x53434b54) {
    if (!quiet)
      fprintf(stderr, "Invalid TLV data in Firmware Version ctl element\n");
    return NULL;
  }

  return strdup((char *)&tlv[2]);
}

static void get_firmware_version(
  snd_ctl_t  *ctl,
  int         card_num,
  const char *name,
  uint32_t   *version
) {
  snd_ctl_elem_id_t *id;
  snd_ctl_elem_info_t *info;
  snd_ctl_elem_value_t *value;
  int err;

  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_value_alloca(&value);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
  snd_ctl_elem_id_set_name(id, name);

  snd_ctl_elem_info_set_id(info, id);
  snd_ctl_elem_value_set_id(value, id);

  // Set version to 0 if we can't read the control
  for (int i = 0; i < 4; i++)
    version[i] = 0;

  if ((err = snd_ctl_elem_info(ctl, info)) < 0)
    return;

  if (snd_ctl_elem_info_get_count(info) != 4) {
    fprintf(
      stderr,
      "Firmware Version control for card %d has wrong element count\n",
      card_num
    );
    return;
  }

  if (snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER) {
    fprintf(
      stderr,
      "Firmware Version control for card %d has wrong element type\n",
      card_num
    );
    return;
  }

  if ((err = snd_ctl_elem_read(ctl, value)) < 0) {
    fprintf(
      stderr,
      "Error reading Firmware Version control for card %d: %s\n",
      card_num,
      snd_strerror(err)
    );
    return;
  }

  for (int i = 0; i < 4; i++)
    version[i] = snd_ctl_elem_value_get_integer(value, i);
}

// Enumerate all cards and return an array of sound_card pointers
struct sound_card **enum_cards(int *count, bool quiet) {
  struct sound_card **cards = NULL;
  *count = 0;
  int card_num = -1;

  if (snd_card_next(&card_num) < 0 || card_num < 0)
    return NULL;

  while (card_num >= 0) {
    snd_ctl_t *ctl = NULL;
    char card_name[32];
    int vid, pid;
    snprintf(card_name, sizeof(card_name), "card%d", card_num);

    if (!get_usb_id(card_name, &vid, &pid))
      goto next;

    char *serial = get_device_serial(card_num);

    if (!serial) {
      fprintf(stderr, "Failed to get device serial number\n");
      return NULL;
    }

    char alsa_name[32];
    snprintf(alsa_name, sizeof(alsa_name), "hw:%d", card_num);

    struct supported_device *dev = get_supported_device_by_pid(pid);
    if (!dev)
      goto next;

    if (snd_ctl_open(&ctl, alsa_name, 0) < 0) {
      fprintf(
        stderr,
        "Cannot open control for card %d (%s)\n",
        card_num,
        alsa_name
      );
      goto next;
    }

    char *socket_path = get_socket_path(ctl, card_num, quiet);
    if (!socket_path)
      goto next;

    uint32_t firmware_version[4];
    uint32_t esp_firmware_version[4];

    get_firmware_version(
      ctl, card_num, "Firmware Version", firmware_version
    );
    get_firmware_version(
      ctl, card_num, "ESP Firmware Version", esp_firmware_version
    );

    *count += 1;
    cards = realloc(cards, sizeof(*cards) * *count);
    if (!cards) {
      perror("realloc");
      exit(1);
    }

    struct sound_card *card = calloc(1, sizeof(*card));
    cards[*count - 1] = card;

    card->card_num = card_num;
    card->usb_vid = vid;
    card->usb_pid = pid;
    card->card_name = strdup(card_name);
    card->serial = serial;
    card->product_name = strdup(dev->name);
    card->alsa_name = strdup(alsa_name);
    card->socket_path = socket_path;
    card->socket_fd = -1;
    memcpy(
      card->firmware_version,
      firmware_version,
      sizeof(firmware_version)
    );
    memcpy(
      card->esp_firmware_version,
      esp_firmware_version,
      sizeof(esp_firmware_version)
    );

next:
    if (ctl) {
      snd_ctl_close(ctl);
      ctl = NULL;
    }
    if (snd_card_next(&card_num) < 0)
      break;
  }

  return cards;
}

int connect_to_server(struct sound_card *card) {
  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("Cannot create socket");
    return -1;
  }

  struct sockaddr_un addr = {
    .sun_family = AF_UNIX
  };
  strncpy(addr.sun_path, card->socket_path, sizeof(addr.sun_path) - 1);

  if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    char *s;
    if (asprintf(&s, "Cannot connect to server at %s", addr.sun_path) < 0)
      perror("Cannot connect to server");
    else
      perror(s);
    return -1;
  }

  card->socket_fd = sock_fd;

  return 0;
}

// Wait for server to disconnect after sending a reboot command
// (should happen in <1ms)
int wait_for_disconnect(struct sound_card *card) {
  fd_set rfds;
  struct timeval tv, start_time, now;
  const int TIMEOUT_SECS = 1;
  char buf[1];

  gettimeofday(&start_time, NULL);

  while (1) {
    FD_ZERO(&rfds);
    FD_SET(card->socket_fd, &rfds);

    gettimeofday(&now, NULL);
    int elapsed = now.tv_sec - start_time.tv_sec;
    if (elapsed >= TIMEOUT_SECS) {
      fprintf(stderr, "Timeout waiting for server disconnect\n");
      return -1;
    }

    tv.tv_sec = TIMEOUT_SECS - elapsed;
    tv.tv_usec = 0;

    int ret = select(card->socket_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      return -1;
    }

    if (ret > 0) {
      // Try to read one byte
      ssize_t n = read(card->socket_fd, buf, 1);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        perror("read");
        return -1;
      }
      if (n == 0) {
        // EOF received - server has disconnected
        return 0;
      }
      // Ignore any data received, just keep waiting for EOF
    }
  }
}

void free_sound_card(struct sound_card *card) {
  if (!card)
    return;
  free(card->card_name);
  free(card->serial);
  free(card->product_name);
  free(card->alsa_name);
  free(card->socket_path);
  if (card->socket_fd >= 0)
    close(card->socket_fd);
  free(card);
}
