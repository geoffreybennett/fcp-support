// SPDX-FileCopyrightText: 2024-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <alsa/sound/tlv.h>
#include <json-c/json.h>

#include "input-controls.h"
#include "control-utils.h"
#include "device-ops.h"
#include "log.h"

/* Look up a control configuration property, preferring a per-member
 * value from the "member-config" object (keyed by devmap member name)
 * over the base configuration */
static struct json_object *get_config_prop(
  struct json_object *control_config,
  const char         *member_name,
  const char         *key
) {
  struct json_object *member_config, *member_props, *value;

  if (json_object_object_get_ex(
        control_config, "member-config", &member_config
      ) &&
      json_object_object_get_ex(member_config, member_name, &member_props) &&
      json_object_object_get_ex(member_props, key, &value))
    return value;

  if (json_object_object_get_ex(control_config, key, &value))
    return value;

  return NULL;
}

/* Get an integer control configuration property which may be either a
 * scalar (same value for all channels) or an array indexed by channel */
static int get_channel_int(
  struct json_object *obj,
  int                 channel,
  const char         *control_type,
  int                *value
) {
  if (json_object_is_type(obj, json_type_array)) {
    if (channel > (int)json_object_array_length(obj)) {
      log_error(
        "Channel %d out of range for %s per-channel array",
        channel, control_type
      );
      return -1;
    }
    *value = json_object_get_int(
      json_object_array_get_idx(obj, channel - 1)
    );
    return 0;
  }

  *value = json_object_get_int(obj);
  return 0;
}

static int create_input_control(
  struct fcp_device  *device,
  const char         *input_name,
  int                 channel,
  int                 array_index,
  struct json_object *member,
  const char         *member_name,
  struct json_object *control_config
) {
  char control_name[64];

  /* Extract control properties from app space */
  struct json_object *offset, *devmap_type;
  if (!json_object_object_get_ex(member, "offset", &offset) ||
      !json_object_object_get_ex(member, "type", &devmap_type)) {
    log_error(
      "Cannot find member properties (offset, type) for %s",
      member_name
    );
    return -1;
  }

  struct json_object *notify_device, *notify_client;
  json_object_object_get_ex(member, "notify-device", &notify_device);
  json_object_object_get_ex(member, "notify-client", &notify_client);

  /* Get control configuration */
  struct json_object *name_format, *type;
  if (!json_object_object_get_ex(control_config, "name", &name_format) ||
      !json_object_object_get_ex(control_config, "type", &type)) {
    log_error("Cannot find control properties for %s", member_name);
    return -1;
  }

  /* Format control name */
  snprintf(control_name, sizeof(control_name),
           json_object_get_string(name_format),
           channel);

  /* Create the control */
  struct control_props props = {
    .name          = control_name,
    .array_index   = array_index,
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .category      = CATEGORY_DATA,
    .data_type     = devmap_member_type_to_data_type(
                       device, json_object_get_string(devmap_type)
                     ),
    .step          = 1,
    .read_only     = 0,
    .notify_client = notify_client ? json_object_get_int(notify_client) : 0,
    .notify_device = notify_device ? json_object_get_int(notify_device) : 0,
    .offset        = json_object_get_int(offset),
    .value         = 0,
    .read_func     = read_data_control,
    .write_func    = write_data_control
  };

  /* Set type-specific properties */
  const char *type_str = json_object_get_string(type);

  if (!strcmp(type_str, "bool")) {
    props.type = SND_CTL_ELEM_TYPE_BOOLEAN;
    props.min = 0;
    props.max = 1;

  } else if (!strcmp(type_str, "int")) {
    struct json_object *min, *max;
    int min_val, max_val;

    if (!json_object_object_get_ex(control_config, "min", &min) ||
        !json_object_object_get_ex(control_config, "max", &max)) {
      log_error("Cannot find min/max for %s", member_name);
      return -1;
    }
    if (get_channel_int(min, channel, member_name, &min_val) < 0 ||
        get_channel_int(max, channel, member_name, &max_val) < 0)
      return -1;

    props.type = SND_CTL_ELEM_TYPE_INTEGER;
    props.min = min_val;
    props.max = max_val;

    struct json_object *db_min, *db_max;
    if (json_object_object_get_ex(control_config, "db-min", &db_min) &&
        json_object_object_get_ex(control_config, "db-max", &db_max)) {
      int db_min_val, db_max_val;

      if (get_channel_int(db_min, channel, member_name, &db_min_val) < 0 ||
          get_channel_int(db_max, channel, member_name, &db_max_val) < 0)
        return -1;

      unsigned int *tlv = calloc(4, sizeof(unsigned int));
      tlv[0] = SNDRV_CTL_TLVT_DB_MINMAX;
      tlv[1] = 2 * sizeof(unsigned int);
      tlv[2] = db_min_val * 100;
      tlv[3] = db_max_val * 100;

      props.tlv = tlv;
    }

  } else if (!strcmp(type_str, "enum")) {
    struct json_object *values =
      get_config_prop(control_config, member_name, "values");

    if (!values) {
      log_error("Cannot find values for %s", member_name);
      return -1;
    }

    int num_values = json_object_array_length(values);
    if (num_values <= 0) {
      log_error("Empty values array for enum %s", member_name);
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

/* Create input controls */
static int create_input_controls(
  struct fcp_device *device,
  struct json_object *inputs,
  struct json_object *members,
  struct json_object *input_controls
) {
  int err;

  /* Process each input */
  int array_index = json_object_array_length(inputs);
  for (int i = 0; i < array_index; i++) {
    struct json_object *input = json_object_array_get_idx(inputs, i);
    struct json_object *controls, *name;

    if (!json_object_object_get_ex(input, "controls", &controls) ||
        !json_object_object_get_ex(input, "name", &name)) {
      log_error("Cannot find controls/name in input %d", i);
      continue;
    }

    const char *input_name = json_object_get_string(name);

    /* Create each configured control type */
    json_object_object_foreach(input_controls, control_type, control_config) {
      struct json_object *control;
      if (json_object_object_get_ex(controls, control_type, &control)) {
        struct json_object *index;
        if (!json_object_object_get_ex(control, "index", &index)) {
          log_error("Cannot find %s index", control_type);
          continue;
        }

        struct json_object *member_name;
        if (!json_object_object_get_ex(control, "member", &member_name)) {
          log_error("Cannot find %s member", control_type);
          continue;
        }

        struct json_object *member;
        if (!json_object_object_get_ex(members, json_object_get_string(member_name), &member)) {
          log_error("Cannot find member %s", json_object_get_string(member_name));
          continue;
        }

        err = create_input_control(
          device,
          input_name,
          i + 1,
          json_object_get_int(index),
          member,
          json_object_get_string(member_name),
          control_config
        );
        if (err < 0)
          return err;
      }
    }
  }

  return 0;
}

/* Create controls for array-based global input members like inputMutes */
static int create_global_input_array_controls(
  struct fcp_device  *device,
  struct json_object *members,
  struct json_object *input_controls
) {
  /* List of global array members and their config keys */
  static const struct {
    const char *member_name;
    const char *config_key;
  } global_arrays[] = {
    { "inputMutes", "mute" },
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
    if (!json_object_object_get_ex(input_controls, config_key, &control_config))
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
      int err = create_input_control(
        device,
        NULL,
        i + 1,
        i,
        member,
        config_key,
        control_config
      );
      if (err < 0)
        return err;
    }
  }

  return 0;
}

int init_input_controls(struct fcp_device *device) {
  struct json_object *input_controls;

  /* Get input controls configuration */
  if (!json_object_object_get_ex(device->fam, "input-controls", &input_controls)) {
    log_error("Cannot find input-controls in configuration");
    return -1;
  }

  /* Get device specification */
  struct json_object *spec, *inputs;
  if (!json_object_object_get_ex(device->devmap, "device-specification", &spec) ||
      !json_object_object_get_ex(spec, "physical-inputs", &inputs)) {
    log_error("Cannot find device-specification/physical-inputs in device map");
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

  int err = create_input_controls(device, inputs, members, input_controls);
  if (err < 0)
    return err;

  return create_global_input_array_controls(device, members, input_controls);
}
