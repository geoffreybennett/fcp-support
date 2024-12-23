// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "device.h"
#include "../shared/fcp-shared.h"

int handle_esp_firmware_update(
  struct fcp_device                  *device,
  int                                 client_fd,
  const struct fcp_socket_msg_header *header
);
