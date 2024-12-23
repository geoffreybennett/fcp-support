// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include "device.h"
#include "fcp.h"

/* Mux cache, one table per rate
 * 0 = 44.1/48kHz, 1 = 88.2/96kHz, 2 = 176.4/192kHz
 */
struct mux_cache {
  /* Mux values for each rate */
  int       mux_size[3];
  uint32_t *values[3];

  /* List of inputs, their names, and router pin for that input */
  int          input_count;
  const char **input_names;
  uint16_t    *input_router_pin;

  /* Number of outputs */
  int output_count;

  /* Array size output_count * 3 storing the router slot for each
   * output at each rate (output_num * 3 + rate).
   *
   * If the output is not available at a rate, or the output is fixed,
   * the value is -1.
   */
  int *output_router_slots;

  /* Array size output_count storing the fixed input index for each
   * output (if the output is fixed, otherwise -1).
   */
  int *output_fixed_input;

  bool dirty;
};

struct fcp_device;

void free_mux_cache(struct fcp_device *device);
void invalidate_mux_cache(struct fcp_device *device);
void add_mux_controls(struct fcp_device *device);
