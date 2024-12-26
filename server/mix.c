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

static struct json_object *find_destination_by_name(
  struct json_object *destinations,
  const char *name
) {
  int count = json_object_array_length(destinations);

  for (int i = 0; i < count; i++) {
    struct json_object *dest = json_object_array_get_idx(destinations, i);
    struct json_object *dest_name;

    if (!json_object_object_get_ex(dest, "name", &dest_name))
      continue;

    if (!strcmp(json_object_get_string(dest_name), name))
      return dest;
  }

  return NULL;
}

static int count_mixer_inputs(struct fcp_device *device) {
  struct json_object *sinks, *spec, *destinations;

  if (!json_object_object_get_ex(device->fam, "sinks", &sinks)) {
    log_error("Cannot find sinks in ALSA map");
    return -1;
  }

  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec) ||
      !json_object_object_get_ex(spec, "destinations", &destinations)) {
    log_error("Cannot find device-specification/destinations in device map");
    return -1;
  }

  int count = 0;
  int sink_count = json_object_array_length(sinks);

  for (int i = 0; i < sink_count; i++) {
    struct json_object *sink = json_object_array_get_idx(sinks, i);
    struct json_object *device_name;

    if (!json_object_object_get_ex(sink, "device_name", &device_name))
      continue;

    // Look up this sink in the destinations
    struct json_object *dest = find_destination_by_name(
      destinations,
      json_object_get_string(device_name)
    );

    if (!dest)
      continue;

    // Check if it's a mixer input
    struct json_object *mixer_index;
    if (json_object_object_get_ex(dest, "mixer-input-index", &mixer_index))
      count++;
  }

  return count;
}

void add_mix_controls(struct fcp_device *device) {
  int num_outputs, num_inputs;

  int err = fcp_mix_info(device->hwdep, &num_outputs, &num_inputs);
  if (err < 0) {
    log_error("Cannot get mixer info: %s", snd_strerror(err));
    return;
  }

  device->mix_output_count = num_outputs;
  device->mix_input_count = num_inputs;

  device->mix_input_control_count = count_mixer_inputs(device);
  if (device->mix_input_control_count < 1) {
    log_error("Cannot find any mixer inputs in ALSA map/device map");
    return;
  }

  init_mix_cache(device);

  struct json_object *sinks, *spec, *destinations;
  if (!json_object_object_get_ex(device->fam, "sinks", &sinks)) {
    log_error("Cannot find sinks in ALSA map");
    return;
  }
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec) ||
      !json_object_object_get_ex(spec, "destinations", &destinations)) {
    log_error("Cannot find device-specification/destinations in device map");
    return;
  }

  int sink_count = json_object_array_length(sinks);

  /* Create controls for each mix output */
  for (int i = 0; i < num_outputs; i++) {

    /* For each sink in the ALSA map, check if it's a mixer input and
     * create a control if it is
     */
    for (int j = 0; j < sink_count; j++) {
      struct json_object *sink = json_object_array_get_idx(sinks, j);
      struct json_object *device_name, *alsa_name;

      if (!json_object_object_get_ex(sink, "device_name", &device_name) ||
          !json_object_object_get_ex(sink, "alsa_name", &alsa_name))
        continue;

      /* Look up this sink in the destinations */
      struct json_object *dest = find_destination_by_name(
        destinations,
        json_object_get_string(device_name)
      );

      if (!dest)
        continue;

      /* Check if it's a mixer input */
      struct json_object *mixer_index;
      if (!json_object_object_get_ex(dest, "mixer-input-index", &mixer_index))
        continue;
      int mix_index = json_object_get_int(mixer_index);

      /* Get the number from alsa_name */
      const char *alsa_name_str = json_object_get_string(alsa_name);
      const char *num_str = alsa_name_str + strcspn(alsa_name_str, "0123456789");
      int num = atoi(num_str);
      if (num <= 0 || num >  num_inputs) {
        log_error("Invalid mixer input number %d", num);
        continue;
      }

      /* Create a control for this mixer input */
      char control_name[64];
      snprintf(
        control_name,
        sizeof(control_name),
        "Mix %c Input %02d Playback Volume",
        'A' + i,
        num
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
        .offset        = i * num_inputs + mix_index,
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

