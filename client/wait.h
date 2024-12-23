// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

struct device_wait {
  const char *serial;  // Expected serial number
  int         timeout; // How long to wait in seconds
};

int wait_for_device(struct device_wait *wait, struct found_card **found);
