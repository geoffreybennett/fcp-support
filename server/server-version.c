// SPDX-FileCopyrightText: 2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string.h>

#include "device-ops.h"
#include "server-version.h"

#define SERVER_VERSION_SIZE 32

static int read_server_version(
  struct fcp_device    *device,
  struct control_props *props,
  void                 *buf,
  size_t                size
) {
  strncpy(buf, VERSION, size - 1);
  ((char *)buf)[size - 1] = '\0';
  return 0;
}

void add_server_version_control(struct fcp_device *device) {
  struct control_props props = {
    .name            = "FCP Server Version",
    .interface       = SND_CTL_ELEM_IFACE_CARD,
    .type            = SND_CTL_ELEM_TYPE_BYTES,
    .category        = CATEGORY_DATA,
    .size            = SERVER_VERSION_SIZE,
    .read_only       = 1,
    .read_bytes_func = read_server_version,
    .write_bytes_func = NULL
  };

  add_control(device, &props);
}
