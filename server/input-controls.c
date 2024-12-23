// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
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

static int create_input_control(
  struct fcp_device  *device,
  const char         *input_name,
  int                 array_index,
  struct json_object *member,
  const char         *control_type,
  struct json_object *control_config
) {
  char control_name[64];

  /* Extract control properties from app space */
  struct json_object *offset, *devmap_type;
  if (!json_object_object_get_ex(member, "offset", &offset) ||
      !json_object_object_get_ex(member, "type", &devmap_type)) {
    log_error(
      "Cannot find member properties (offset, type) for %s",
      control_type
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
    log_error("Cannot find control properties for %s", control_type);
    return -1;
  }

  /* Format control name */
  snprintf(control_name, sizeof(control_name),
           json_object_get_string(name_format),
           array_index + 1);

  /* Create the control */
  struct control_props props = {
    .name          = control_name,
    .array_index   = array_index,
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .category      = CATEGORY_DATA,
    .data_type     = devmap_type_to_data_type(
                       json_object_get_string(devmap_type)
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

    if (!json_object_object_get_ex(control_config, "min", &min) ||
        !json_object_object_get_ex(control_config, "max", &max)) {
      log_error("Cannot find min/max for %s", control_type);
      return -1;
    }

    props.type = SND_CTL_ELEM_TYPE_INTEGER;
    props.data_type = devmap_type_to_data_type(json_object_get_string(devmap_type));
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
      log_error("Empty values array for enum %s", control_type);
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

  return create_input_controls(device, inputs, members, input_controls);
}
