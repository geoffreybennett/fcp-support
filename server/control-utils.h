// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "device.h"

/* Common control read/write functions used by all control types */

int read_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
);

int write_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                   value
);

int read_bitmap_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
);

int write_bitmap_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                   value
);

int devmap_type_to_data_type(const char *type);
int devmap_type_to_data_type_with_width(const char *type, int width);

int find_member_by_path(
  struct fcp_device   *device,
  const char          *path,
  struct json_object **found_member,
  const char         **member_type,
  int                 *total_offset,
  bool                 allow_missing
);

int find_member_by_path_with_notify(
  struct fcp_device   *device,
  const char          *path,
  struct json_object **found_member,
  const char         **member_type,
  int                 *total_offset,
  int                 *notify_device,
  int                 *notify_client,
  bool                 allow_missing
);

int read_bytes_control(
  struct fcp_device    *device,
  struct control_props *props,
  void                 *data,
  size_t                size
);

int write_bytes_control(
  struct fcp_device    *device,
  struct control_props *props,
  const void           *data,
  size_t                size
);
