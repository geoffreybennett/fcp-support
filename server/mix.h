// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include "device.h"
#include "fcp.h"

/* mixer range from -80dB to +12dB in 0.5dB steps */
#define FCP_MIXER_MIN_DB -80
#define FCP_MIXER_BIAS (-FCP_MIXER_MIN_DB * 2)
#define FCP_MIXER_MAX_DB 12
#define FCP_MIXER_MAX_VALUE \
        ((FCP_MIXER_MAX_DB - FCP_MIXER_MIN_DB) * 2)
#define FCP_MIXER_VALUE_COUNT (FCP_MIXER_MAX_VALUE + 1)

struct fcp_device;

/* Array of interface values (not ALSA dB values) for one mix output */
struct mix_cache_entry {
  int  *values;
  bool  dirty;
};

void free_mix_cache(struct fcp_device *device);
void invalidate_mix_cache(struct fcp_device *device);
void add_mix_controls(struct fcp_device *device);
