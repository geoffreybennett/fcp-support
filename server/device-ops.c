// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <json-c/json.h>

#include "device-ops.h"
#include "fcp.h"
#include "fcp-devmap.h"
#include "sync.h"
#include "input-controls.h"
#include "output-controls.h"
#include "global-controls.h"
#include "mix.h"
#include "mux.h"
#include "meter.h"
#include "log.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

// Get USB IDs from procfs - format is "VID:PID"
static void get_usb_ids(int card_num, uint16_t *vid, uint16_t *pid) {
  char *proc_path;
  char usbid[10]; // VID:PID + newline + null
  FILE *f;

  if (asprintf(&proc_path, "/proc/asound/card%d/usbid", card_num) < 0) {
    log_error("Cannot allocate memory for proc path");
    exit(1);
  }

  f = fopen(proc_path, "r");
  free(proc_path);
  if (!f) {
    log_error("Cannot open USB ID file for card %d", card_num);
    exit(1);
  }

  if (fread(usbid, 1, sizeof(usbid), f) != sizeof(usbid)) {
    log_error("Cannot read USB ID for card %d from %s", card_num, proc_path);
    exit(1);
  }
  fclose(f);

  usbid[9] = 0;  // Ensure null termination

  int scan_vid, scan_pid;
  if (sscanf(usbid, "%x:%x", &scan_vid, &scan_pid) != 2) {
    log_error(
      "Cannot parse USB ID '%s' for card %d in %s",
      usbid,
      card_num,
      proc_path
    );
    exit(1);
  }

  if (scan_vid < 0 || scan_pid < 0 || scan_vid > 0xffff || scan_pid > 0xffff) {
    log_error(
      "Invalid USB ID '%s' for card %d in %s",
      usbid,
      card_num,
      proc_path
    );
    exit(1);
  }

  *vid = scan_vid;
  *pid = scan_pid;
}

static int get_alsa_fd(const char *type, void *handle, int (*fd_count)(void*), int (*get_fds)(void*, struct pollfd*, unsigned int)) {
  struct pollfd pfds;
  int count = fd_count(handle);

  if (count != 1) {
    log_error("Invalid number of %s descriptors (%d)", type, count);
    exit(1);
  }

  int err = get_fds(handle, &pfds, 1);
  if (err < 0) {
    log_error("Cannot get %s descriptors: %s", type, snd_strerror(err));
    exit(1);
  }

  return pfds.fd;
}

static int open_alsa_device(struct fcp_device *device, const char *card_name) {
  int err;

  // Open ALSA control interface
  err = snd_ctl_open(&device->ctl, card_name, 0);
  if (err < 0) {
    log_error(
      "Cannot open control for card %s: %s",
      card_name,
      snd_strerror(err)
    );
    return err;
  }

  // Open ALSA hwdep interface
  err = snd_hwdep_open(&device->hwdep, card_name, 0);
  if (err < 0) {
    log_debug(
      "Cannot open hwdep for card %s: %s",
      card_name,
      snd_strerror(err)
    );
    snd_ctl_close(device->ctl);

    // Return ENOPROTOOPT if hwdep is not available (probably not an
    // FCP device or the driver is not loaded)
    if (err == -ENOENT)
      return -ENOPROTOOPT;

    return err;
  }

  return 0;
}

int device_init(int card_num, struct fcp_device *device) {
  char card_name[16];
  int err;

  // Clear device structure
  memset(device, 0, sizeof(*device));
  device->card_num = card_num;

  // Get USB IDs
  get_usb_ids(card_num, &device->usb_vid, &device->usb_pid);
  log_debug("USB ID: %04x:%04x", device->usb_vid, device->usb_pid);

  // Create ALSA device name and open interfaces
  snprintf(card_name, sizeof(card_name), "hw:%d", card_num);
  err = open_alsa_device(device, card_name);
  if (err < 0)
    return err;

  // Get file descriptors
  device->ctl_fd = get_alsa_fd("control", device->ctl,
                (void*)snd_ctl_poll_descriptors_count,
                (void*)snd_ctl_poll_descriptors);
  device->hwdep_fd = get_alsa_fd("hwdep", device->hwdep,
                  (void*)snd_hwdep_poll_descriptors_count,
                  (void*)snd_hwdep_poll_descriptors);

  // Initialise FCP protocol
  fcp_init(device->hwdep);

  return 0;
}

void init_control_manager(struct fcp_device *device) {
  const int initial_capacity = 8;

  struct control_manager *ctrl_mgr = &device->ctrl_mgr;

  ctrl_mgr->controls = calloc(initial_capacity, sizeof(struct control_props));
  if (!ctrl_mgr->controls) {
    log_error("Cannot allocate memory for control manager");
    exit(1);
  }
  ctrl_mgr->capacity = initial_capacity;
  ctrl_mgr->num_controls = 0;
}

int add_control(struct fcp_device *device, struct control_props *props) {
  struct control_manager *ctrl_mgr = &device->ctrl_mgr;

  if (ctrl_mgr->num_controls == ctrl_mgr->capacity) {
    int new_capacity = ctrl_mgr->capacity * 2;

    struct control_props *new_controls = realloc(
      ctrl_mgr->controls,
      new_capacity * sizeof(struct control_props)
    );
    if (!new_controls) {
      log_error("Cannot reallocate memory for control manager");
      exit(1);
    }
    ctrl_mgr->controls = new_controls;
    ctrl_mgr->capacity = new_capacity;
  }

  struct control_props *new_props = &ctrl_mgr->controls[ctrl_mgr->num_controls];
  *new_props = *props;
  new_props->name = strdup(props->name);
  if (!new_props->name) {
    log_error("Cannot allocate memory for control name");
    exit(1);
  }
  if (props->type == SND_CTL_ELEM_TYPE_ENUMERATED) {
    new_props->enum_names = malloc(props->enum_count * sizeof(char *));
    if (!new_props->enum_names) {
      log_error("Cannot allocate memory for enum names");
      exit(1);
    }
    for (int i = 0; i < props->enum_count; i++) {
      new_props->enum_names[i] = strdup(props->enum_names[i]);
      if (!new_props->enum_names[i]) {
        log_error("Cannot allocate memory for enum name");
        exit(1);
      }
    }
  }

  ctrl_mgr->num_controls++;

  add_user_control(device, new_props);

  return 0;
}

/* Find control by name */
struct control_props *find_control(
  struct fcp_device *device,
  const char        *name
) {
  struct control_manager *ctrl_mgr = &device->ctrl_mgr;

  for (int i = 0; i < ctrl_mgr->num_controls; i++)
    if (!strcmp(ctrl_mgr->controls[i].name, name))
      return &ctrl_mgr->controls[i];
  return NULL;
}

int device_init_controls(struct fcp_device *device) {
  // Remove any existing controls first
  remove_all_user_controls(device);

  init_control_manager(device);

  // Check and initialise subsystems based on capabilities
  int err;
  int supported;

  supported = fcp_cap_read(device->hwdep, FCP_OPCODE_CATEGORY_INIT);
  if (supported < 0 || !supported) {
    log_error("Device does not support required INIT category");
    return -EINVAL;
  }

  supported = fcp_cap_read(device->hwdep, FCP_OPCODE_CATEGORY_DATA);
  if (supported < 0 || !supported) {
    log_error("Device does not support required DATA category");
    return -EINVAL;
  }

  err = init_input_controls(device);
  if (err < 0)
    return err;

  err = init_output_controls(device);
  if (err < 0)
    return err;

  err = init_global_controls(device);
  if (err < 0)
    return err;

  // Initialise optional subsystems

  if (fcp_cap_read(device->hwdep, FCP_OPCODE_CATEGORY_SYNC) > 0)
    add_sync_control(device);

  if (fcp_cap_read(device->hwdep, FCP_OPCODE_CATEGORY_METER) > 0)
    add_meter_control(device);

  if (fcp_cap_read(device->hwdep, FCP_OPCODE_CATEGORY_MIX) > 0)
    add_mix_controls(device);

  if (fcp_cap_read(device->hwdep, FCP_OPCODE_CATEGORY_MUX) > 0)
    add_mux_controls(device);

  return 0;
}

void device_handle_notification(struct fcp_device *device, uint32_t notification) {

  log_debug("Notification: 0x%08x", notification);

  // Check each control to see if it needs updating
  for (int i = 0; i < device->ctrl_mgr.num_controls; i++) {
    struct control_props *props = &device->ctrl_mgr.controls[i];

    if (!(notification & props->notify_client))
      continue;

    // Allocate space for values
    int count = props->component_count ? props->component_count : 1;
    int values[count];

    // Get new value from device
    int err = props->read_func(device, props, values);
    if (err < 0) {
      log_error(
        "Cannot get data for control %s: %s",
        props->name,
        snd_strerror(err)
      );
      continue;
    }

    // Get current ALSA value
    snd_ctl_elem_value_t *alsa_value;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_alloca(&alsa_value);
    snd_ctl_elem_id_alloca(&id);

    snd_ctl_elem_id_set_interface(id, props->interface);
    snd_ctl_elem_id_set_name(id, props->name);
    snd_ctl_elem_value_set_id(alsa_value, id);

    err = snd_ctl_elem_read(device->ctl, alsa_value);
    if (err < 0) {
      log_error(
        "Failed to read value for control %s: %s",
        props->name,
        snd_strerror(err)
      );
      continue;
    }

    bool changed = false;
    for (int j = 0; j < count; j++) {
      int old_value = snd_ctl_elem_value_get_integer(alsa_value, j);
      if (values[j] != old_value) {
        changed = true;
        snd_ctl_elem_value_set_integer(alsa_value, j, values[j]);

        log_debug(
          "Control %s value changed at device from %d to %d",
          props->name,
          old_value,
          values[j]
        );
      }
    }

    if (!changed)
      continue;

    // Update if changed
    err = snd_ctl_elem_write(device->ctl, alsa_value);
    if (err < 0) {
      log_error(
        "Failed to set value for %s: %s",
        props->name,
        snd_strerror(err)
      );
    }
  }
}

int device_handle_control_change(
  struct fcp_device          *device,
  const snd_ctl_elem_id_t    *control_id,
  const snd_ctl_elem_value_t *new_value
) {
  const char *name = snd_ctl_elem_id_get_name(control_id);
  struct control_props *props = find_control(device, name);

  if (!props)
    return 0;  // Not one of our controls

  int new_val = snd_ctl_elem_value_get_integer(new_value, 0);

  if (new_val == props->value)
    return 0;  // No change

  log_debug(
    "Control %s value changed at ALSA from %d to %d",
    props->name,
    props->value,
    new_val
  );

  // Ignore read-only controls
  if (props->read_only)
    return 0;

  if (!props->write_func) {
    log_error("Control %s has no write function", props->name);
    return 0;
  }

  // Update value and notify device
  props->value = new_val;
  int err = props->write_func(device, props, new_val);
  if (err < 0) {
    log_error(
      "Cannot write data for control %s: %s",
      props->name,
      snd_strerror(err)
    );
    return err;
  }

  if (props->notify_device) {
    err = fcp_data_notify(device->hwdep, props->notify_device);
    if (err < 0) {
      log_error("Cannot notify device: %s", snd_strerror(err));
      return err;
    }
  }

  return 0;
}

void device_get_fds(struct fcp_device *device, int *ctl_fd, int *hwdep_fd) {
  *ctl_fd = device->ctl_fd;
  *hwdep_fd = device->hwdep_fd;
}

static json_object *try_load_json(const char *dir, const char *filename) {
  if (!dir)
    return json_object_from_file(filename);

  char *path;
  json_object *obj = NULL;
  if (asprintf(&path, "%s/%s", dir, filename) >= 0) {
    obj = json_object_from_file(path);
    free(path);
  }
  return obj;
}

int device_load_config(struct fcp_device *device) {
  int err;

  // Read device map
  err = fcp_devmap_read_json(device);
  if (err < 0) {
    log_error("Cannot read device map: %s", snd_strerror(err));
    return err;
  }

  // Read FCP ALSA map
  char *filename;
  if (asprintf(&filename, "fcp-alsa-map-%04x.json", device->usb_pid) < 0) {
    log_error("Cannot allocate memory for filename");
    return -ENOMEM;
  }

  // Try locations in order: env var, current dir, system dir
  const char *search_dirs[] = {
    getenv("FCP_SERVER_DATA_DIR"),
    NULL,
    DATADIR
  };

  for (size_t i = 0; i < ARRAY_SIZE(search_dirs); i++) {
    device->fam = try_load_json(search_dirs[i], filename);
    if (device->fam) {
      free(filename);
      return 0;
    }
  }

  free(filename);
  log_error(
    "Cannot read FCP ALSA map: %s",
    json_util_get_last_err()
  );
  return -ENOENT;
}
