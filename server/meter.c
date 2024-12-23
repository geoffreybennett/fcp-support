// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <alsa/sound/tlv.h>

#include "uapi-fcp.h"
#include "fcp.h"
#include "meter.h"
#include "log.h"

static int add_meter_mapping_info(struct fcp_device *device, int map_size, char **labels) {
  snd_ctl_elem_id_t *id;
  snd_ctl_elem_info_t *info;
  int err;

  snd_ctl_elem_id_alloca(&id);
  snd_ctl_elem_info_alloca(&info);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
  snd_ctl_elem_id_set_name(id, "Level Meter");
  snd_ctl_elem_id_set_index(id, 0);

  snd_ctl_elem_info_set_id(info, id);
  err = snd_ctl_elem_info(device->ctl, info);
  if (err < 0) {
    log_error(
      "Failed to get element info for %s: %s",
      snd_ctl_elem_id_get_name(id),
      snd_strerror(err)
    );
    return err;
  }

  // Create TLV structure
  unsigned int total_size = 0;
  for (int i = 0; i < map_size; i++) {
    total_size += strlen(labels[i]) + 1;
  }

  // Bump up to sizeof(unsigned int) boundary
  int align = sizeof(unsigned int) - 1;
  total_size = (total_size + align) & ~align;

  unsigned int *tlv = calloc(2 + total_size / 4, sizeof(unsigned int));
  if (!tlv) {
    log_error("Cannot allocate TLV memory");
    return -ENOMEM;
  }

  tlv[0] = 72275388;
  tlv[1] = total_size;

  char *data = (char *)&tlv[2];
  for (int i = 0; i < map_size; i++) {
    strcpy(data, labels[i]);
    data += strlen(labels[i]) + 1;
  }

  // Set the TLV
  err = snd_ctl_elem_tlv_write(device->ctl, id, tlv);
  if (err < 0) {
    log_error(
      "Failed to write TLV for %s: %s",
      snd_ctl_elem_id_get_name(id),
      snd_strerror(err)
    );
  }

  free(tlv);
  return err;
}

void add_meter_control(struct fcp_device *device) {
  struct json_object *spec, *sources, *sinks;
  struct json_object *control_sources, *control_sinks;
  int num_meter_slots;
  struct fcp_meter_map map = {0};
  char **labels = NULL;
  int meter_idx = 0;
  int err;

  fcp_meter_info(device->hwdep, &num_meter_slots);
  map.meter_slots = num_meter_slots;

  /* Get device specification */
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec)) {
    log_error("Cannot find device specification");
    return;
  }

  /* Get sources and sinks arrays */
  if (!json_object_object_get_ex(spec, "sources", &sources) ||
      !json_object_object_get_ex(spec, "destinations", &sinks)) {
    log_error("Cannot find sources/destinations arrays");
    return;
  }

  /* Get FCP ALSA map sources/sinks */
  if (!json_object_object_get_ex(device->fam, "sources", &control_sources) ||
      !json_object_object_get_ex(device->fam, "sinks", &control_sinks)) {
    log_error("Cannot find sources/sinks in fcp-alsa-map");
    return;
  }

  /* Allocate maximum possible size */
  int map_size = json_object_array_length(control_sources) +
                 json_object_array_length(control_sinks);

  int16_t *meter_map = calloc(map_size, sizeof(int16_t));
  labels = calloc(map_size, sizeof(char *));
  if (!meter_map || !labels) {
    log_error("Cannot allocate meter map/label map");
    err = -ENOMEM;
    goto done;
  }

  /* Map sources */
  for (int i = 0; i < json_object_array_length(control_sources); i++) {
    struct json_object *control_source = json_object_array_get_idx(control_sources, i);
    struct json_object *device_name, *alsa_name;

    if (!json_object_object_get_ex(control_source, "device_name", &device_name) ||
        !json_object_object_get_ex(control_source, "alsa_name", &alsa_name)) {
      log_error("Control source missing device_name/alsa_name");
      err = -1;
      goto done;
    }

    /* Find matching source in device map */
    const char *name = json_object_get_string(device_name);
    const char *alsa = json_object_get_string(alsa_name);

    for (int j = 0; j < json_object_array_length(sources); j++) {
      struct json_object *source = json_object_array_get_idx(sources, j);
      struct json_object *source_name, *peak_index;

      if (!json_object_object_get_ex(source, "name", &source_name))
        continue;

      if (strcmp(json_object_get_string(source_name), name))
        continue;

      if (json_object_object_get_ex(source, "peak-index", &peak_index)) {
        int idx = json_object_get_int(peak_index);

        if (idx < 0 || idx >= map.meter_slots) {
          log_error("Invalid peak index %d", idx);
          err = -1;
          goto done;
        }

        meter_map[meter_idx] = idx;

        if (asprintf(&labels[meter_idx], "Source %s", alsa) < 0) {
          log_error("Cannot allocate label");
          err = -ENOMEM;
          goto done;
        }

        meter_idx++;
      }
      break;
    }
  }

  /* Map sinks */
  for (int i = 0; i < json_object_array_length(control_sinks); i++) {
    struct json_object *control_sink = json_object_array_get_idx(control_sinks, i);
    struct json_object *device_name, *alsa_name;

    if (!json_object_object_get_ex(control_sink, "device_name", &device_name) ||
        !json_object_object_get_ex(control_sink, "alsa_name", &alsa_name)) {
      log_error("Control sink missing device_name/alsa_name");
      err = -1;
      goto done;
    }

    /* Find matching sink in device map */
    const char *name = json_object_get_string(device_name);
    const char *alsa = json_object_get_string(alsa_name);

    for (int j = 0; j < json_object_array_length(sinks); j++) {
      struct json_object *sink = json_object_array_get_idx(sinks, j);
      struct json_object *sink_name, *peak_index;

      if (!json_object_object_get_ex(sink, "name", &sink_name))
        continue;

      if (strcmp(json_object_get_string(sink_name), name))
        continue;

      if (json_object_object_get_ex(sink, "peak-index", &peak_index)) {
        int idx = json_object_get_int(peak_index);

        if (idx < 0 || idx >= map.meter_slots) {
          log_error("Invalid peak index %d", idx);
          err = -1;
          goto done;
        }

        meter_map[meter_idx] = idx;

        if (asprintf(&labels[meter_idx], "Sink %s", alsa) < 0) {
          log_error("Cannot allocate label");
          err = -ENOMEM;
          goto done;
        }

        meter_idx++;
      }

      break;
    }
  }

  if (meter_idx == 0) {
    log_error("No meters found");
    err = -1;
    goto done;
  }

  /* Configure meter mapping */
  map.map = meter_map;
  map.map_size = meter_idx;

  {
    char s[1024] = "Meter map:";
    char *p = s + strlen(s);
    for (int i = 0; i < meter_idx; i++)
      p += sprintf(p, " %d", meter_map[i]);
    log_debug("%s", s);
    log_debug("Meter slots: %d", map.meter_slots);
    log_debug("Map size: %d", map.map_size);
  }

  /* Send meter map to driver */
  err = snd_hwdep_ioctl(device->hwdep, FCP_IOCTL_SET_METER_MAP, &map);
  if (err < 0)
    log_error("Cannot set meter map: %s", snd_strerror(err));

  /* Add mapping info control */
  err = add_meter_mapping_info(device, meter_idx, labels);
  if (err < 0)
    goto done;

done:
  if (labels) {
    for (int i = 0; i < meter_idx; i++)
      free(labels[i]);
    free(labels);
  }
  free(meter_map);
}
