// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <alsa/sound/tlv.h>
#include <json-c/json.h>

#include "mix.h"
#include "fcp-devmap.h"
#include "device-ops.h"
#include "log.h"

void invalidate_mix_cache(struct fcp_device *device) {
  struct mix_cache_entry *cache = device->mix_cache;

  if (!cache)
    return;

  for (int i = 0; i < device->mix_output_count; i++)
    cache[i].dirty = true;
}

static void init_mix_cache(struct fcp_device *device) {
  struct mix_cache_entry *cache = device->mix_cache = calloc(
    device->mix_output_count,
    sizeof(struct mix_cache_entry)
  );
  if (!cache) {
    log_error("Cannot allocate memory for mix cache");
    exit(1);
  }

  for (int i = 0; i < device->mix_output_count; i++) {
    cache[i].values = calloc(
      device->mix_input_count,
      sizeof(int)
    );
    if (!cache[i].values) {
      log_error("Cannot allocate memory for mix cache values");
      exit(1);
    }
  }

  invalidate_mix_cache(device);
}

void free_mix_cache(struct fcp_device *device) {
  struct mix_cache_entry *cache = device->mix_cache;

  if (!cache)
    return;

  for (int i = 0; i < device->mix_output_count; i++)
    free(cache[i].values);

  free(cache);
  device->mix_cache = NULL;
}

/* Get cached mix values, reading from the device first if necessary */
static int get_cached_mix_values(
  struct fcp_device  *device,
  int                 mix_output,
  int               **values
) {
  if (!device->mix_cache || mix_output < 0 || mix_output >= device->mix_output_count)
    return -EINVAL;

  struct mix_cache_entry *entry = &device->mix_cache[mix_output];

  if (entry->dirty) {
    int err = fcp_mix_read(
      device->hwdep, mix_output, device->mix_input_count, entry->values
    );
    if (err < 0)
      return err;
    entry->dirty = false;
  }

  *values = entry->values;
  return 0;
}

static int read_mix_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
) {
  int mix_output = props->offset / device->mix_input_count;
  int mix_input = props->offset % device->mix_input_count;

  int *values;
  int err = get_cached_mix_values(device, mix_output, &values);

  if (err < 0) {
    log_error(
      "Failed to read mix for output %d: %s",
      mix_output,
      snd_strerror(err)
    );
    return err;
  }

  *value = values[mix_input];

  return 0;
}

static int write_mix_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                   value
) {
  int mix_output = props->offset / device->mix_input_count;
  int mix_input = props->offset % device->mix_input_count;

  int *values;
  int err = get_cached_mix_values(device, mix_output, &values);
  if (err < 0) {
    log_error(
      "Failed to read mix for output %d: %s",
      mix_output,
      snd_strerror(err)
    );
    return err;
  }

  values[mix_input] = value;

  err = fcp_mix_write(
    device->hwdep, mix_output, device->mix_input_count, values
  );
  if (err < 0) {
    log_error(
      "Failed to write mix for output %d: %s",
      mix_output,
      snd_strerror(err)
    );
  }

  return err;
}

static const SNDRV_CTL_TLVD_DECLARE_DB_LINEAR(mix_tlv, SNDRV_CTL_TLVD_DB_GAIN_MUTE, 1200);

void add_mix_controls(struct fcp_device *device) {
  int num_outputs, num_inputs;

  int err = fcp_mix_info(device->hwdep, &num_outputs, &num_inputs);
  if (err < 0) {
    log_error("Cannot get mixer info: %s", snd_strerror(err));
    return;
  }

  device->mix_output_count = num_outputs;
  device->mix_input_count = num_inputs;

  init_mix_cache(device);

  for (int i = 0; i < num_outputs; i++) {
    for (int j = 0; j < num_inputs; j++) {
      char control_name[64];

      snprintf(
        control_name,
        sizeof(control_name),
        "Mix %c Input %02d Playback Volume",
        'A' + i,
        j + 1
      );

      struct control_props props = {
        .name          = strdup(control_name),
        .interface     = SND_CTL_ELEM_IFACE_MIXER,
        .type          = SND_CTL_ELEM_TYPE_INTEGER,
        .category      = CATEGORY_MIX,
        .min           = 0,
        .max           = 32613,
        .step          = 1,
        .tlv           = mix_tlv,
        .read_only     = 0,
        .notify_client = 0,
        .notify_device = 0,
        .offset        = i * num_inputs + j,
        .value         = 0,
        .read_func     = read_mix_control,
        .write_func    = write_mix_control
      };

      int err = add_control(device, &props);
      if (err < 0)
        return;
    }
  }
}

