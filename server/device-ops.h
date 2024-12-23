// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <alsa/asoundlib.h>
#include <json-c/json.h>

#include "device.h"

int device_init(int card_num, struct fcp_device *device);

int device_init_controls(struct fcp_device *device);

void device_handle_notification(struct fcp_device *device, uint32_t notification);

int device_handle_control_change(
  struct fcp_device          *device,
  const snd_ctl_elem_id_t    *control_id,
  const snd_ctl_elem_value_t *new_value
);

void device_get_fds(
  struct fcp_device *device,
  int               *ctl_fd,
  int               *hwdep_fd
);

int device_load_config(struct fcp_device *device);

int add_control(struct fcp_device *device, struct control_props *props);

struct control_props *find_control(
  struct fcp_device *device,
  const char        *name
);
