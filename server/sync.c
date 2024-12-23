// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "device-ops.h"
#include "log.h"

static const char *sync_enum_names[] = { "Unlocked", "Locked" };
static const int sync_enum_count = 2;

static int read_sync_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
) {
  int new_value = fcp_sync_read(device->hwdep);

  if (new_value < 0) {
    log_error("Failed to read sync status: %s", strerror(-new_value));
    return new_value;
  }

  *value = new_value;
  return 0;
}

void add_sync_control(struct fcp_device *device) {
  struct control_props props = {
    .name          = "Sync Status",
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .type          = SND_CTL_ELEM_TYPE_ENUMERATED,
    .category      = CATEGORY_SYNC,
    .enum_names    = (char **)sync_enum_names,
    .enum_count    = sync_enum_count,
    .read_only     = 1,
    .notify_client = 8,
    .notify_device = 0,
    .offset        = 0,
    .value         = 0,
    .read_func     = read_sync_control,
    .write_func    = NULL
  };

  add_control(device, &props);
}
