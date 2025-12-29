// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <alsa/sound/tlv.h>
#include <json-c/json.h>

#include "output-controls.h"
#include "control-utils.h"
#include "device-ops.h"
#include "log.h"

/* Special handling for volume controls as we don't always get a
 * notify that the device has changed the volume of linked outputs
 */
int write_data_control_vol(struct fcp_device *device, struct control_props *props, int value) {
  int err = write_data_control(device, props, value);
  if (err < 0)
    return err;

  /* If this is a linked output, update the volume of the other output */
  if (props->link) {
    struct control_props *controls = device->ctrl_mgr.controls;
    int idx = props - controls;

    if (props->array_index & 1) {
      idx--;
    } else {
      idx++;
    }

    if (idx >= 0 && idx < device->ctrl_mgr.num_controls) {
      struct control_props *link = &controls[idx];
      err = write_data_control(device, link, value);
      if (err < 0)
        return err;
    }
  }

  /* Re-read from the device */
  device_handle_notification(device, props->notify_client);

  return 0;
}

static int create_output_control(
  struct fcp_device  *device,
  const char         *output_name,
  int                 array_index,
  struct json_object *member,
  const char         *control_type,
  struct json_object *control_config,
  struct json_object *output_link
) {
  char control_name[64];

  /* Extract control properties from app space */
  struct json_object *offset, *devmap_type, *notify_device, *notify_client;
  if (!json_object_object_get_ex(member, "offset", &offset) ||
      !json_object_object_get_ex(member, "type", &devmap_type) ||
      !json_object_object_get_ex(member, "notify-device", &notify_device) ||
      !json_object_object_get_ex(member, "notify-client", &notify_client)) {
    log_error(
      "Cannot find offset/type/notify-device/notify-client in member %s",
      control_type
    );
    return -1;
  }

  /* Get control configuration */
  struct json_object *name_format, *type;
  if (!json_object_object_get_ex(control_config, "name", &name_format) ||
      !json_object_object_get_ex(control_config, "type", &type)) {
    log_error(
      "Cannot find name/type in control configuration for %s",
      control_type
    );
    return -1;
  }

  /* Format control name */
  snprintf(control_name, sizeof(control_name),
           json_object_get_string(name_format),
           array_index + 1);

  /* Create the control */
  struct control_props props = {
    .name          = strdup(control_name),
    .array_index   = array_index,
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .category      = CATEGORY_DATA,
    .data_type     = devmap_type_to_data_type(
                       json_object_get_string(devmap_type)
                     ),
    .step          = 1,
    .read_only     = 0,
    .notify_client = json_object_get_int(notify_client),
    .notify_device = json_object_get_int(notify_device),
    .offset        = json_object_get_int(offset),
    .value         = 0,
    .read_func     = read_data_control,
    .write_func    = write_data_control_vol
  };

  if (output_link) {
    int array_len = json_object_array_length(output_link);
    for (int i = 0; i < array_len; i++) {
      struct json_object *link = json_object_array_get_idx(output_link, i);

      if (json_object_get_int(link) == array_index) {
        props.link = 1;
        break;
      }
    }
  }

  const char *type_str = json_object_get_string(type);

  if (!strcmp(type_str, "bool")) {
    props.type = SND_CTL_ELEM_TYPE_BOOLEAN;
    props.min = 0;
    props.max = 1;

  } else if (!strcmp(type_str, "int")) {
    struct json_object *min, *max;

    if (!json_object_object_get_ex(control_config, "min", &min) ||
        !json_object_object_get_ex(control_config, "max", &max)) {
      log_error("Cannot find min/max for %s", control_type);
      return -1;
    }

    props.type = SND_CTL_ELEM_TYPE_INTEGER;
    props.min = json_object_get_int(min);
    props.max = json_object_get_int(max);

    struct json_object *db_min, *db_max;
    if (json_object_object_get_ex(control_config, "db-min", &db_min) &&
        json_object_object_get_ex(control_config, "db-max", &db_max)) {

      unsigned int *tlv = calloc(4, sizeof(unsigned int));
      tlv[0] = SNDRV_CTL_TLVT_DB_MINMAX;
      tlv[1] = 2 * sizeof(unsigned int);
      tlv[2] = json_object_get_int(db_min) * 100;
      tlv[3] = json_object_get_int(db_max) * 100;
      props.tlv = tlv;
    }

  } else if (!strcmp(type_str, "enum")) {
    struct json_object *values;

    if (!json_object_object_get_ex(control_config, "values", &values)) {
      log_error("Cannot find values for %s", control_type);
      return -1;
    }

    int num_values = json_object_array_length(values);
    if (num_values <= 0) {
      log_error("Empty values array for %s", control_type);
      return -1;
    }

    props.type = SND_CTL_ELEM_TYPE_ENUMERATED;
    props.enum_count = num_values;
    props.enum_names = calloc(num_values, sizeof(char *));

    if (!props.enum_names) {
      log_error("Cannot allocate memory for enum names");
      return -1;
    }

    for (int i = 0; i < num_values; i++) {
      struct json_object *value = json_object_array_get_idx(values, i);
      props.enum_names[i] = strdup(json_object_get_string(value));
      if (!props.enum_names[i]) {
        log_error("Cannot allocate memory for enum name");
        for (int j = 0; j < i; j++)
          free(props.enum_names[j]);
        free(props.enum_names);
        return -1;
      }
    }

  } else {
    log_error("Invalid control type: %s", type_str);
    return -1;
  }

  return add_control(device, &props);
}

static int create_output_controls(
  struct fcp_device   *device,
  struct json_object  *outputs,
  struct json_object  *members,
  struct json_object  *output_controls,
  struct json_object  *output_link
) {
  /* Process each output */
  int array_index = json_object_array_length(outputs);
  for (int i = 0; i < array_index; i++) {
    struct json_object *output = json_object_array_get_idx(outputs, i);
    struct json_object *controls, *name;

    if (!json_object_object_get_ex(output, "controls", &controls) ||
        !json_object_object_get_ex(output, "name", &name)) {
      log_error("Cannot find controls/name in output %d", i);
      continue;
    }

    const char *output_name = json_object_get_string(name);

    /* Create each configured control type */
    json_object_object_foreach(output_controls, control_type, control_config) {
      struct json_object *control;

      if (json_object_object_get_ex(controls, control_type, &control)) {
        struct json_object *index;

        if (!json_object_object_get_ex(control, "index", &index)) {
          log_error(
            "Cannot find %s index in output %s",
            control_type,
            output_name
          );
          continue;
        }

        struct json_object *member_name;
        if (!json_object_object_get_ex(control, "member", &member_name)) {
          log_error(
            "Cannot find %s member in output %s",
            control_type,
            output_name
          );
          continue;
        }

        struct json_object *member;
        if (!json_object_object_get_ex(members, json_object_get_string(member_name), &member)) {
          log_error(
            "Cannot find member %s in device map",
            json_object_get_string(member_name)
          );
          continue;
        }

        int err = create_output_control(
          device,
          output_name,
          json_object_get_int(index),
          member,
          json_object_get_string(member_name),
          control_config,
          output_link
        );
        if (err < 0)
          return err;
      }
    }
  }

  return 0;
}

/* Build source enum list from output-group-sources in ALSA map */
static int build_source_enum(
  struct fcp_device  *device,
  char             ***enum_names,
  int               **enum_values,
  int                *enum_count
) {
  struct json_object *sources_array;

  if (!json_object_object_get_ex(device->fam, "output-group-sources", &sources_array)) {
    log_error("Cannot find output-group-sources in ALSA map");
    return -1;
  }

  int num_sources = json_object_array_length(sources_array);

  /* Count valid (non-null, non-empty) entries */
  int valid_count = 0;
  for (int i = 0; i < num_sources; i++) {
    struct json_object *entry = json_object_array_get_idx(sources_array, i);
    if (entry && json_object_get_type(entry) == json_type_string) {
      const char *name = json_object_get_string(entry);
      if (name && name[0] != '\0')
        valid_count++;
    }
  }

  /* Allocate arrays for valid entries only */
  *enum_count = valid_count;
  *enum_names = calloc(valid_count, sizeof(char *));
  *enum_values = calloc(valid_count, sizeof(int));

  if (!*enum_names || !*enum_values) {
    log_error("Cannot allocate memory for source enum");
    return -1;
  }

  /* Populate from the array, skipping null/empty entries */
  int enum_idx = 0;
  for (int i = 0; i < num_sources; i++) {
    struct json_object *entry = json_object_array_get_idx(sources_array, i);
    if (entry && json_object_get_type(entry) == json_type_string) {
      const char *name = json_object_get_string(entry);
      if (name && name[0] != '\0') {
        (*enum_names)[enum_idx] = strdup(name);
        (*enum_values)[enum_idx] = i;
        enum_idx++;
      }
    }
  }

  return 0;
}

static void free_source_enum(char **enum_names, int *enum_values, int enum_count) {
  if (enum_names) {
    for (int i = 0; i < enum_count; i++)
      free(enum_names[i]);
    free(enum_names);
  }
  free(enum_values);
}

/* Create output group controls (map, sources, trims) */
static int create_output_group_controls(
  struct fcp_device  *device,
  struct json_object *output_controls,
  struct json_object *enums
) {
  char **source_enum_names = NULL;
  int *source_enum_values = NULL;
  int source_enum_count = 0;
  int err;

  /* Get output count from enums */
  struct json_object *max_sizes, *enumerators, *output_count_obj;
  if (!json_object_object_get_ex(enums, "maximum_array_sizes", &max_sizes) ||
      !json_object_object_get_ex(max_sizes, "enumerators", &enumerators) ||
      !json_object_object_get_ex(enumerators, "kMAX_NUMBER_OUTPUTS", &output_count_obj)) {
    log_debug("No kMAX_NUMBER_OUTPUTS - skipping output group controls");
    return 0;
  }
  int output_count = json_object_get_int(output_count_obj);

  /* Iterate over output-controls looking for outputGroup entries */
  json_object_object_foreach(output_controls, control_path, control_config) {
    /* Check if this is an output group control (starts with "outputGroup") */
    if (strncmp(control_path, "outputGroup", 11) != 0)
      continue;

    struct json_object *name_obj, *type_obj;
    if (!json_object_object_get_ex(control_config, "name", &name_obj) ||
        !json_object_object_get_ex(control_config, "type", &type_obj)) {
      log_error("Missing name/type in output control %s", control_path);
      continue;
    }

    const char *name_template = json_object_get_string(name_obj);
    const char *type_str = json_object_get_string(type_obj);

    /* Find the member using dot notation */
    struct json_object *member;
    const char *member_type;
    int offset;
    int notify_device, notify_client;

    err = find_member_by_path_with_notify(
      device, control_path, &member, &member_type, &offset,
      &notify_device, &notify_client, true
    );
    if (err < 0) {
      log_debug("Output group member %s not found, skipping", control_path);
      continue;
    }

    /* Create controls for each output */
    for (int i = 0; i < output_count; i++) {
      char *name;
      if (asprintf(&name, name_template, i + 1) < 0) {
        log_error("Cannot allocate memory for control name");
        return -1;
      }

      struct control_props props = {
        .name          = name,
        .array_index   = i,
        .interface     = SND_CTL_ELEM_IFACE_MIXER,
        .category      = CATEGORY_DATA,
        .step          = 1,
        .read_only     = 0,
        .value         = 0,
        .offset        = offset,
        .data_type     = devmap_type_to_data_type(member_type),
        .notify_client = notify_client,
        .notify_device = notify_device
      };

      if (!strcmp(type_str, "bool-bitmap")) {
        /* Bitmap control - uses special read/write functions */
        props.type = SND_CTL_ELEM_TYPE_BOOLEAN;
        props.min = 0;
        props.max = 1;
        props.read_func = read_bitmap_data_control;
        props.write_func = write_bitmap_data_control;

      } else if (!strcmp(type_str, "enum")) {
        /* Enum control - build source list if not already done */
        struct json_object *values_from;
        if (json_object_object_get_ex(control_config, "values-from", &values_from) &&
            !strcmp(json_object_get_string(values_from), "output-group-sources")) {

          if (!source_enum_names) {
            err = build_source_enum(device, &source_enum_names, &source_enum_values, &source_enum_count);
            if (err < 0) {
              free(name);
              return err;
            }
          }

          props.type = SND_CTL_ELEM_TYPE_ENUMERATED;
          props.enum_count = source_enum_count;
          props.enum_names = calloc(source_enum_count, sizeof(char *));
          props.enum_values = calloc(source_enum_count, sizeof(int));
          for (int j = 0; j < source_enum_count; j++) {
            props.enum_names[j] = strdup(source_enum_names[j]);
            props.enum_values[j] = source_enum_values[j];
          }
          props.read_func = read_data_control;
          props.write_func = write_data_control;
        } else {
          log_error("Unsupported enum values-from for %s", control_path);
          free(name);
          continue;
        }

      } else if (!strcmp(type_str, "int")) {
        /* Integer control */
        struct json_object *min_obj, *max_obj;
        if (!json_object_object_get_ex(control_config, "min", &min_obj) ||
            !json_object_object_get_ex(control_config, "max", &max_obj)) {
          log_error("Missing min/max for int control %s", control_path);
          free(name);
          continue;
        }

        props.type = SND_CTL_ELEM_TYPE_INTEGER;
        props.min = json_object_get_int(min_obj);
        props.max = json_object_get_int(max_obj);
        props.read_func = read_data_control;
        props.write_func = write_data_control;

        struct json_object *db_min_obj, *db_max_obj;
        if (json_object_object_get_ex(control_config, "db-min", &db_min_obj) &&
            json_object_object_get_ex(control_config, "db-max", &db_max_obj)) {
          unsigned int *tlv = calloc(4, sizeof(unsigned int));
          tlv[0] = SNDRV_CTL_TLVT_DB_MINMAX;
          tlv[1] = 2 * sizeof(unsigned int);
          tlv[2] = json_object_get_int(db_min_obj) * 100;
          tlv[3] = json_object_get_int(db_max_obj) * 100;
          props.tlv = tlv;
        }

      } else {
        log_error("Unsupported control type %s for %s", type_str, control_path);
        free(name);
        continue;
      }

      err = add_control(device, &props);
      if (err < 0) {
        free(name);
        free_source_enum(source_enum_names, source_enum_values, source_enum_count);
        return err;
      }
    }
  }

  free_source_enum(source_enum_names, source_enum_values, source_enum_count);
  return 0;
}

/* Create controls for array-based global output members like outputMute */
static int create_global_output_array_controls(
  struct fcp_device  *device,
  struct json_object *members,
  struct json_object *output_controls
) {
  /* List of global array members and their config keys */
  static const struct {
    const char *member_name;
    const char *config_key;
  } global_arrays[] = {
    { "outputMute", "mute" },
    { NULL, NULL }
  };

  for (int g = 0; global_arrays[g].member_name; g++) {
    const char *member_name = global_arrays[g].member_name;
    const char *config_key = global_arrays[g].config_key;

    /* Check if this member exists in the device map */
    struct json_object *member;
    if (!json_object_object_get_ex(members, member_name, &member))
      continue;

    /* Check if we have config for this control type */
    struct json_object *control_config;
    if (!json_object_object_get_ex(output_controls, config_key, &control_config))
      continue;

    /* Get array size */
    struct json_object *array_shape;
    if (!json_object_object_get_ex(member, "array-shape", &array_shape))
      continue;

    int array_size = json_object_get_int(
      json_object_array_get_idx(array_shape, 0)
    );

    /* Create a control for each array element */
    for (int i = 0; i < array_size; i++) {
      int err = create_output_control(
        device,
        NULL,
        i,
        member,
        config_key,
        control_config,
        NULL
      );
      if (err < 0)
        return err;
    }
  }

  return 0;
}

int init_output_controls(struct fcp_device *device) {
  struct json_object *output_controls, *output_link;

  /* Get output controls configuration */
  if (!json_object_object_get_ex(device->fam, "output-controls", &output_controls)) {
    log_error("Cannot find output-controls/output-link in configuration");
    return -1;
  }
  json_object_object_get_ex(device->fam, "output-link", &output_link);

  /* Get device specification */
  struct json_object *spec, *outputs;
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec) ||
      !json_object_object_get_ex(spec, "physical-outputs", &outputs)) {
    log_error("Cannot find device-specification/physical-outputs in device map");
    return -1;
  }

  /* Get APP_SPACE members */
  struct json_object *structs, *app_space, *members;
  if (!json_object_object_get_ex(device->devmap, "structs", &structs) ||
      !json_object_object_get_ex(structs, "APP_SPACE", &app_space) ||
      !json_object_object_get_ex(app_space, "members", &members)) {
    log_error("Cannot find structs/APP_SPACE/members in device map");
    return -1;
  }

  int err = create_output_controls(
    device,
    outputs,
    members,
    output_controls,
    output_link
  );
  if (err < 0)
    return err;

  err = create_global_output_array_controls(device, members, output_controls);
  if (err < 0)
    return err;

  /* Get enums for output group controls */
  struct json_object *enums;
  if (!json_object_object_get_ex(device->devmap, "enums", &enums)) {
    log_debug("No enums in device map - skipping output group controls");
    return 0;
  }

  return create_output_group_controls(device, output_controls, enums);
}
