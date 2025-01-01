// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "alsa.h"

int wait_for_device(
  const char         *serial,  // Expected serial number
  int                 timeout, // How long to wait in seconds
  struct sound_card **card     // Output: sound card
);
