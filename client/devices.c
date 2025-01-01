// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdint.h>

#include "devices.h"

struct supported_device supported_devices[] = {
  { 0x821b, "Scarlett 4th Gen 16i16" },
  { 0x821c, "Scarlett 4th Gen 18i16" },
  { 0x821d, "Scarlett 4th Gen 18i20" },
  { 0 },
};

struct supported_device *get_supported_device_by_pid(uint16_t pid) {
  for (int i = 0; supported_devices[i].pid; i++)
    if (supported_devices[i].pid == pid)
      return &supported_devices[i];

  return NULL;
}
