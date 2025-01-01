// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#define VENDOR_VID 0x1235

// Supported devices
struct supported_device {
  int         pid;
  const char *name;
};

extern struct supported_device supported_devices[];

struct supported_device *get_supported_device_by_pid(uint16_t pid);
