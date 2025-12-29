// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <json-c/json.h>

#include "device-ops.h"
#include "mix.h"
#include "fcp-devmap.h"
#include "log.h"

void invalidate_mux_cache(struct fcp_device *device) {
  struct mux_cache *cache = device->mux_cache;

  if (!cache)
    return;

  cache->dirty = true;
}

static void add_input_name(
  struct mux_cache *cache,
  const char       *name,
  int               router_pin
) {
  cache->input_names = realloc(
    cache->input_names,
    (cache->input_count + 1) * sizeof(const char *)
  );
  if (!cache->input_names) {
    log_error("Cannot allocate memory for input names");
    exit(1);
  }

  cache->input_router_pin = realloc(
    cache->input_router_pin,
    (cache->input_count + 1) * sizeof(uint16_t)
  );
  if (!cache->input_router_pin) {
    log_error("Cannot allocate memory for input router pins");
    exit(1);
  }

  cache->input_names[cache->input_count] = name;
  cache->input_router_pin[cache->input_count] = router_pin;
  cache->input_count++;
}

static void init_mux_cache(struct fcp_device *device) {
  struct mux_cache *cache = device->mux_cache = calloc(
    1,
    sizeof(struct mux_cache)
  );
  if (!cache) {
    log_error("Cannot allocate memory for mux cache");
    exit(1);
  }

  int err = fcp_mux_info(device->hwdep, cache->mux_size);
  if (err < 0) {
    log_error("Failed to get mux info: %s", snd_strerror(err));
    exit(1);
  }

  for (int i = 0; i < 3; i++) {
    log_debug("Mux %d: %d", i, cache->mux_size[i]);
    cache->values[i] = calloc(
      cache->mux_size[i],
      sizeof(uint32_t)
    );
  }

  invalidate_mux_cache(device);

  add_input_name(cache, "Off", 0);

  /* List of sources in the control config */
  struct json_object *control_sources;
  if (!json_object_object_get_ex(device->fam, "sources", &control_sources)) {
    log_error("Missing required 'sources' field in FCP ALSA map");
    return;
  }
  int num_control_sources = json_object_array_length(control_sources);

  /* List of sources in the devmap */
  struct json_object *spec, *devmap_sources;
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec) ||
      !json_object_object_get_ex(spec, "sources", &devmap_sources)) {
    log_error("Cannot find device-specification/sources in device map");
    return;
  }
  int num_devmap_sources = json_object_array_length(devmap_sources);

  /* Iterate over the controls sources */
  for (int i = 0; i < num_control_sources; i++) {

    struct json_object *control_source = json_object_array_get_idx(control_sources, i);

    struct json_object *device_name_json;
    struct json_object *alsa_name_json;

    if (!json_object_object_get_ex(control_source, "device_name", &device_name_json) ||
        !json_object_object_get_ex(control_source, "alsa_name", &alsa_name_json)) {
      log_error("Cannot find device_name or alsa_name in control source %d", i);
      return;
    }

    const char *device_name = json_object_get_string(device_name_json);
    const char *alsa_name = json_object_get_string(alsa_name_json);

    /* Find the devmap info for this control source */
    for (int j = 0; j < num_devmap_sources; j++) {
      struct json_object *devmap_source = json_object_array_get_idx(devmap_sources, j);

      struct json_object *source_name_json;
      if (!json_object_object_get_ex(devmap_source, "name", &source_name_json)) {
        log_error("Cannot find name in devmap source %d", j);
        return;
      }
      const char *source_name = json_object_get_string(source_name_json);

      if (strcmp(source_name, device_name))
        continue;

      struct json_object *router_pin_json;
      if (!json_object_object_get_ex(devmap_source, "router-pin", &router_pin_json)) {
        log_error("Cannot find router-pin in devmap source %d", j);
        return;
      }

      const char *router_pin_string = json_object_get_string(router_pin_json);
      int router_pin = atoi(router_pin_string);

      if (router_pin <= 0 || router_pin > 0xFFF) {
        log_error(
          "Invalid router pin 0x%s for control source %d devmap source %d",
          router_pin_string, i, j
        );
        return;
      }

      add_input_name(cache, alsa_name, router_pin);
    }
  }
}

void free_mux_cache(struct fcp_device *device) {
  struct mux_cache *cache = device->mux_cache;

  if (!cache)
    return;

  for (int i = 0; i < 3; i++)
    free(cache->values[i]);

  free(cache);

  device->mux_cache = NULL;
}

/* Get cached mux values, reading from the device first if necessary */
static int get_cached_mux_values(
  struct fcp_device  *device,
  int                 mux_num,
  uint32_t          **values
) {
  struct mux_cache *cache = device->mux_cache;

  if (!cache)
    return -EINVAL;

  if (cache->dirty) {
    for (int i = 0; i < 3; i++) {
      int err = fcp_mux_read(
        device->hwdep, i, cache->mux_size[i], cache->values[i]
      );
      if (err < 0)
        return err;
    }
    cache->dirty = false;
  }

  *values = cache->values[mux_num];
  return 0;
}

static int router_pin_to_input(
  struct fcp_device *device,
  int                router_pin
) {
  struct mux_cache *cache = device->mux_cache;

  for (int i = 0; i < cache->input_count; i++)
    if (cache->input_router_pin[i] == router_pin)
      return i;

  return 0;
}

static int read_mux_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
) {
  struct mux_cache *cache = device->mux_cache;

  uint32_t *values;
  int err = get_cached_mux_values(device, 0, &values);
  if (err < 0) {
    log_error("Failed to read mux 0: %s", snd_strerror(err));
    return err;
  }

  if (cache->output_fixed_input[props->offset] >= 0) {
    *value = cache->output_fixed_input[props->offset];
    return 0;
  }

  int router_pin = values[cache->output_router_slots[props->offset * 3]] >> 12;
  int input = router_pin_to_input(device, router_pin);

  *value = input;

  return 0;
}

static int write_mux_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                   value
) {
  struct mux_cache *cache = device->mux_cache;
  int router_pin = cache->input_router_pin[value];

  for (int rate = 0; rate < 3; rate++) {
    if (cache->output_fixed_input[props->offset] >= 0) {
      log_error("Cannot write to fixed input %s", props->name);
      return -EINVAL;
    }

    int slot_num = cache->output_router_slots[props->offset * 3 + rate];

    if (slot_num < 0 && rate == 0) {
      log_error("Missing router slot for %s", props->name);
      return -EINVAL;
    }

    uint32_t *values = cache->values[rate];
    values[slot_num] = (values[slot_num] & 0xFFF) | (router_pin << 12);

    int err = fcp_mux_write(
      device->hwdep, rate, cache->mux_size[rate], values
    );
    if (err < 0) {
      log_error("Failed to write mux %d: %s", rate, snd_strerror(err));
      return err;
    }
  }

  return 0;
}

static struct json_object *get_source_by_name(
  struct json_object *sources,
  const char         *name
) {
  int num_sources = json_object_array_length(sources);

  for (int i = 0; i < num_sources; i++) {
    struct json_object *source = json_object_array_get_idx(sources, i);

    struct json_object *source_name_json;
    if (!json_object_object_get_ex(source, "name", &source_name_json)) {
      log_error("Cannot find name in source %d", i);
      return NULL;
    }
    const char *source_name = json_object_get_string(source_name_json);

    if (!strcmp(source_name, name))
      return source;
  }

  return NULL;
}

void add_mux_controls(struct fcp_device *device) {
  init_mux_cache(device);

  struct mux_cache *cache = device->mux_cache;

  int *mux_size = cache->mux_size;
  uint32_t *values[3];

  for (int rate = 0; rate < 3; rate++) {
    int err = get_cached_mux_values(device, rate, &values[rate]);
    if (err < 0) {
      log_error("Failed to read mux %d: %s", rate, snd_strerror(err));
      return;
    }

    /* dump values src -> dst */
    {
      log_debug("Rate %d:", rate);
      char s[256] = "";
      char *p = s;
      for (int i = 0; i < mux_size[rate]; i++) {
        snprintf(p, sizeof(s) - (p - s), "  %03x %03x", values[rate][i] >> 12, values[rate][i] & 0xFFF);
        p += strlen(p);
        if (i % 8 == 7) {
          log_debug("%s", s);
          p = s;
        }
      }
      if (p > s)
        log_debug("%s", s);
    }
  }

  /* Control template */
  struct control_props props = {
    .type          = SND_CTL_ELEM_TYPE_ENUMERATED,
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .category      = CATEGORY_MUX,
    .enum_count    = cache->input_count,
    .enum_names    = (char **)cache->input_names,
    .step          = 1,
    .read_only     = 0,
    .notify_client = 0, /* TODO */
    .read_func     = read_mux_control,
    .write_func    = write_mux_control
  };

  /* List of sinks in the FCP ALSA map */
  struct json_object *sinks;
  if (!json_object_object_get_ex(device->fam, "sinks", &sinks)) {
    log_error("Cannot find sinks in FCP ALSA map");
    return;
  }
  int num_sinks = json_object_array_length(sinks);

  /* List of sources and destinations in the devmap */
  struct json_object *spec, *sources, *dests;
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec) ||
      !json_object_object_get_ex(spec, "sources", &sources) ||
      !json_object_object_get_ex(spec, "destinations", &dests)) {
    log_error("Cannot find device-specification/sources/destinations in device map");
    return;
  }
  int num_dests = json_object_array_length(dests);

  log_debug("num_sinks: %d, num_dests: %d", num_sinks, num_dests);

  cache->output_count = 0;
  cache->output_router_slots = calloc(num_sinks *  3, sizeof(int));
  if (!cache->output_router_slots) {
    log_error("Cannot allocate memory for output router slots");
    return;
  }

  cache->output_fixed_input = calloc(num_sinks, sizeof(int));
  if (!cache->output_fixed_input) {
    log_error("Cannot allocate memory for output fixed inputs");
    return;
  }

  /* Iterate over the sinks */
  for (int i = 0; i < num_sinks; i++) {

    struct json_object *sink = json_object_array_get_idx(sinks, i);

    struct json_object *device_name_json;
    struct json_object *alsa_name_json;

    if (!json_object_object_get_ex(sink, "device_name", &device_name_json) ||
        !json_object_object_get_ex(sink, "alsa_name", &alsa_name_json)) {
      log_error("Cannot find device_name or alsa_name in sink %d", i);
      return;
    }

    const char *device_name = json_object_get_string(device_name_json);
    const char *alsa_name = json_object_get_string(alsa_name_json);

    /* Find the destination info for this sink */
    for (int j = 0; j < num_dests; j++) {
      struct json_object *dest = json_object_array_get_idx(dests, j);

      struct json_object *dest_name_json;
      if (!json_object_object_get_ex(dest, "name", &dest_name_json)) {
        log_error("Cannot find name in destination %d", j);
        return;
      }
      const char *dest_name = json_object_get_string(dest_name_json);

      if (strcmp(dest_name, device_name))
        continue;

      struct json_object *router_pin_json;
      if (!json_object_object_get_ex(dest, "router-pin", &router_pin_json)) {
        log_error("Cannot find router-pin in destination %d", j);
        return;
      }

      const char *router_pin_string = json_object_get_string(router_pin_json);
      int router_pin = atoi(router_pin_string);

      if (router_pin <= 0 || router_pin > 0xFFF) {
        log_error(
          "Invalid router pin 0x%s for sink %d destination %d",
          router_pin_string, i, j
        );
        return;
      }

      struct json_object *static_source_json;

      /* Check for static-source mixer inputs */
      if (json_object_object_get_ex(
        dest, "static-source", &static_source_json
      )) {
        const char *static_source = json_object_get_string(static_source_json);

        struct json_object *source = get_source_by_name(sources, static_source);

        if (!source) {
          log_error(
            "Cannot find static source %s for %s",
            static_source,
            dest_name
          );
          return;
        }

        struct json_object *router_pin_json;
        if (!json_object_object_get_ex(
          source, "router-pin", &router_pin_json
        )) {
          log_error(
            "Cannot find router-pin for static source %s",
            static_source
          );
          return;
        }

        const char *router_pin_string = json_object_get_string(router_pin_json);
        int router_pin = atoi(router_pin_string);

        if (router_pin <= 0 || router_pin > 0xFFF) {
          log_error(
            "Invalid router pin 0x%s for static source %s",
            router_pin_string, static_source
          );
          return;
        }

        props.read_only = true;

        for (int rate = 0; rate < 3; rate++) {
          cache->output_router_slots[cache->output_count * 3 + rate] = -1;
        }
        cache->output_fixed_input[cache->output_count] =
          router_pin_to_input(device, router_pin);

      /* Not static; find the router slot */
      } else {

        for (int rate = 0; rate < 3; rate++) {
          int router_slot = -1;

          for (int k = 0; k < mux_size[rate]; k++) {
            if ((values[rate][k] & 0xFFF) == router_pin) {
              router_slot = k;
              break;
            }
          }

          /* Not expecting a router_slot to be found for rates > 0 */
          if (!rate && router_slot < 0) {
            log_error(
              "Cannot find router slot for %s pin 0x%03x",
              dest_name, router_pin
            );
            return;
          }

          cache->output_router_slots[cache->output_count * 3 + rate] = router_slot;
          cache->output_fixed_input[cache->output_count] = -1;
        }
      }

      char *control_name;
      if (!strncmp(alsa_name, "PCM", 3) ||
          !strncmp(alsa_name, "Mixer", 5)) {
        if (asprintf(&control_name, "%s Capture Enum", alsa_name) < 0) {
          log_error("Cannot allocate memory for control name");
          return;
        }
      } else {
        if (asprintf(&control_name, "%s Playback Enum", alsa_name) < 0) {
          log_error("Cannot allocate memory for control name");
          return;
        }
      }

      props.name = control_name;
      props.offset = cache->output_count;

      cache->output_count++;

      int err = add_control(device, &props);
      if (err < 0)
        return;

      free(control_name);
    }
  }
}
