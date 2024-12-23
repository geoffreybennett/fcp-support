// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <alsa/asoundlib.h>
#include <json-c/json.h>

#include "device.h"

int fcp_devmap_read_json(struct fcp_device *device);
