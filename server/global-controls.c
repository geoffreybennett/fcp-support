// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <json-c/json.h>

#include "global-controls.h"
#include "control-utils.h"
#include "device-ops.h"
#include "log.h"

static int find_member_by_path(
  struct fcp_device   *device,
  const char          *path,
  struct json_object **found_member,
  const char         **member_type,
  int                 *total_offset,
  bool                 allow_missing
) {
  struct json_object *structs, *current_struct, *current_members;
  char *path_copy = strdup(path);
  char *token, *saveptr;
  const char *current_type = "APP_SPACE";
  *total_offset = 0;

  if (!json_object_object_get_ex(device->devmap, "structs", &structs)) {
    log_error("Cannot find structs in device map");
    free(path_copy);
    return -1;
  }

  /* Start with APP_SPACE */
  if (!json_object_object_get_ex(structs, "APP_SPACE", &current_struct) ||
      !json_object_object_get_ex(current_struct, "members", &current_members)) {
    log_error("Cannot find APP_SPACE members");
    free(path_copy);
    return -1;
  }

  struct json_object *member = NULL;

  /* Walk the dot-separated path */
  token = strtok_r(path_copy, ".", &saveptr);
  while (token != NULL) {
    if (!json_object_object_get_ex(current_members, token, &member)) {
      if (!allow_missing)
        log_error("Cannot find member %s", token);
      free(path_copy);
      return -1;
    }

    /* Add this member's offset */
    *total_offset += json_object_get_int(json_object_object_get(member, "offset"));

    /* Get the member's type */
    current_type = json_object_get_string(json_object_object_get(member, "type"));

    /* More path components? Look up next struct */
    token = strtok_r(NULL, ".", &saveptr);
    if (token != NULL) {
      if (!json_object_object_get_ex(structs, current_type, &current_struct) ||
          !json_object_object_get_ex(current_struct, "members", &current_members)) {
        log_error("Cannot find struct '%s' members", current_type);
        free(path_copy);
        return -1;
      }
    }
  }

  free(path_copy);
  *found_member = member;
  *member_type = current_type;
  return 0;
}

static int parse_component_path(
  const char  *component_spec,
  char       **path,
  int         *offset_adjust,
  int         *width
) {
  char *colon1 = strchr(component_spec, ':');

  if (!colon1) {
    *path = strdup(component_spec);
    *offset_adjust = 0;
    *width = 0;  // Use natural width from devmap
    return 0;
  }

  char *colon2 = strchr(colon1 + 1, ':');
  if (!colon2) {
    log_error("Invalid component spec: %s", component_spec);
    return -1;
  }

  *path = strndup(component_spec, colon1 - component_spec);
  *offset_adjust = atoi(colon1 + 1);
  *width = atoi(colon2 + 1);

  return 0;
}

static int get_component_info(
  struct fcp_device   *device,
  const char          *component_spec,
  struct json_object **member,
  const char         **member_type,
  int                 *offset,
  int                 *width
) {
  char *path;
  int offset_adjust;

  int ret = parse_component_path(component_spec, &path, &offset_adjust, width);
  if (ret < 0)
    return ret;

  ret = find_member_by_path(device, path, member, member_type, offset, true);
  free(path);

  // Component doesn't exist in this devmap version? Ignore it
  if (ret < 0)
    return 1;

  *offset += offset_adjust;

  // If width wasn't specified in component_spec, get it from devmap
  if (*width == 0)
    *width = json_object_get_int(json_object_object_get(*member, "size"));

  return 0;
}

static int create_bool_mixer_outputs_controls(
  const char          *control_name_template,
  struct fcp_device   *device,
  const char          *member_path,
  struct json_object  *control_config
) {
  struct json_object *member;
  const char *member_type;
  int offset;

  int err = find_member_by_path(
    device, member_path, &member, &member_type, &offset, false
  );
  if (err < 0) {
    log_error("Cannot find member %s", member_path);
    return -1;
  }

  for (int i = 0; i < device->mix_output_count; i++) {
    char *name;

    if (asprintf(&name, control_name_template, 'A' + i) < 0) {
      log_error("Cannot allocate memory for control name");
      exit(1);
    }

    struct control_props props = {
      .name          = name,
      .array_index   = i,
      .interface     = SND_CTL_ELEM_IFACE_MIXER,
      .category      = CATEGORY_DATA,
      .step          = 1,
      .read_only     = 0,
      .value         = 0,
      .read_func     = read_bitmap_data_control,
      .write_func    = write_bitmap_data_control,
      .offset        = offset,
      .data_type     = devmap_type_to_data_type(member_type),
      .data_types    = NULL,
      .component_count = 0,
      .notify_client = json_object_get_int(json_object_object_get(member, "notify-client")),
      .notify_device = json_object_get_int(json_object_object_get(member, "notify-device")),
      .type          = SND_CTL_ELEM_TYPE_BOOLEAN,
      .min           = 0,
      .max           = 1
    };

    err = add_control(device, &props);
    free(name);

    if (err < 0)
      return err;
  }

  return 0;
}


/* Create a global control */
static int create_global_control(
  struct fcp_device  *device,
  const char         *member_path,
  struct json_object *control_config,
  struct json_object *enums
) {
  struct json_object *fcp_notify, *fcp_notify_enums;
  struct json_object *name, *type, *components, *member;
  const char *member_type;

  if (!json_object_object_get_ex(
        enums, "eDEV_FCP_USER_MESSAGE_TYPE", &fcp_notify
      ) ||
      !json_object_object_get_ex(
        fcp_notify, "enumerators", &fcp_notify_enums
      )) {
    log_error(
      "Cannot find eDEV_FCP_USER_MESSAGE_TYPE/enumerators in device map"
    );
    return -1;
  }

  if (!json_object_object_get_ex(control_config, "name", &name) ||
      !json_object_object_get_ex(control_config, "type", &type)) {
    log_error("Invalid control configuration for %s", member_path);
    return -1;
  }

  const char *name_str = json_object_get_string(name);
  const char *type_str = json_object_get_string(type);

  if (!strcmp(type_str, "bool-mixer-outputs")) {
    return create_bool_mixer_outputs_controls(
      name_str, device, member_path, control_config
    );
  }

  struct control_props props = {
    .name          = strdup(name_str),
    .array_index   = 0,
    .interface     = SND_CTL_ELEM_IFACE_MIXER,
    .category      = CATEGORY_DATA,
    .step          = 1,
    .read_only     = 0,
    .value         = 0,
    .read_func     = read_data_control,
    .write_func    = write_data_control
  };

  /* Check for multi-component control */
  if (json_object_object_get_ex(control_config, "components", &components)) {
    int expected_count = json_object_get_int(
      json_object_object_get(control_config, "component-count")
    );
    int max_count = json_object_array_length(components);
    if (max_count < 1) {
      log_error("Invalid components for %s", member_path);
      return -1;
    }

    props.offsets = calloc(max_count, sizeof(int));
    if (!props.offsets) {
      log_error("Cannot allocate memory for component offsets");
      exit(1);
    }
    props.data_types = calloc(max_count, sizeof(int));
    if (!props.data_types) {
      log_error("Cannot allocate memory for component data types");
      exit(1);
    }

    int valid_count = 0;

    for (int i = 0; i < max_count; i++) {
      struct json_object *component_member;
      const char *component_member_type;
      int offset, width;
      const char *spec = json_object_get_string(json_object_array_get_idx(components, i));

      int err = get_component_info(
        device, spec, &component_member, &component_member_type, &offset, &width
      );

      // Skip components that don't exist
      if (err)
        continue;

      props.offsets[valid_count] = offset;
      props.data_types[valid_count] = devmap_type_to_data_type_with_width(
        component_member_type, width
      );
      valid_count++;

      if (valid_count == 1) {
        member = component_member;
        member_type = component_member_type;
      }
    }

    if (!valid_count) {
      log_error("No valid components for %s", member_path);
      return -1;
    }

    if (expected_count && valid_count != expected_count) {
      log_error(
        "Invalid component count %d for %s (expected %d)",
        valid_count, member_path, expected_count
      );
      return -1;
    }
    props.component_count = valid_count;

  /* Single-component control */
  } else {
    int err = find_member_by_path(
      device, member_path, &member, &member_type, &props.offset, false
    );
    if (err < 0) {
      log_error("Cannot find member %s", member_path);
      return -1;
    }
  }

  props.data_type = devmap_type_to_data_type(member_type);
  props.notify_client = json_object_get_int(
    json_object_object_get(member, "notify-client")
  );
  props.notify_device = json_object_get_int(
    json_object_object_get(member, "notify-device")
  );

  struct json_object *save;
  if (json_object_object_get_ex(control_config, "save", &save) &&
      json_object_get_boolean(save)) {
    if (props.notify_device) {
      log_error("Control %s has both save and notify-device", member_path);
    } else {
      struct json_object *flash_ctrl;

      if (!json_object_object_get_ex(
           fcp_notify_enums, "eMSG_FLASH_CTRL", &flash_ctrl
         )) {
        log_error(
          "Cannot find eMSG_FLASH_CTRL in eDEV_FCP_USER_MESSAGE_TYPE"
        );
      } else {
        props.notify_device = json_object_get_int(flash_ctrl);
      }
    }
  }

  if (!strcmp(type_str, "enum")) {

    props.type = SND_CTL_ELEM_TYPE_ENUMERATED;

    struct json_object *max_from, *values;

    /* Directly-specified enum values */
    if (json_object_object_get_ex(control_config, "values", &values)) {

      /* Handle direct enum values */
      props.type = SND_CTL_ELEM_TYPE_ENUMERATED;
      props.enum_count = json_object_array_length(values);
      props.enum_names = calloc(props.enum_count, sizeof(char *));
      if (!props.enum_names) {
        log_error("Cannot allocate memory for enum names");
        exit(1);
      }

      /* Check first element to determine format */
      struct json_object *first = json_object_array_get_idx(values, 0);
      if (json_object_get_type(first) == json_type_string) {
        /* Simple string array */
        for (int i = 0; i < props.enum_count; i++) {
          props.enum_names[i] = strdup(
            json_object_get_string(json_object_array_get_idx(values, i))
          );
          if (!props.enum_names[i]) {
            log_error("Cannot allocate memory for enum name");
            exit(1);
          }
        }
      } else {
        /* Array of objects with name/value pairs */
        props.enum_values = calloc(props.enum_count, sizeof(int));

        for (int i = 0; i < props.enum_count; i++) {
          struct json_object *value = json_object_array_get_idx(values, i);
          struct json_object *name, *val;
          if (!json_object_object_get_ex(value, "name", &name)) {
            log_error("Cannot find name in enum value %d", i);
            exit(1);
          }
          props.enum_names[i] = strdup(json_object_get_string(name));
          if (!props.enum_names[i]) {
            log_error("Cannot allocate memory for enum name");
            exit(1);
          }

          if (json_object_object_get_ex(value, "value", &val)) {
            props.enum_values[i] = json_object_get_int(val);
          } else {
            props.enum_values[i] = i;
          }
        }
      }

    /* Numbered enum values */
    } else if (json_object_object_get_ex(control_config, "max-from", &max_from)) {

      /* Look up max value from enums */
      struct json_object *max_sizes, *enumerators, *count;
      if (!json_object_object_get_ex(enums, "maximum_array_sizes", &max_sizes) ||
          !json_object_object_get_ex(max_sizes, "enumerators", &enumerators) ||
          !json_object_object_get_ex(enumerators, json_object_get_string(max_from), &count)) {
        log_error("Cannot find enum value for %s", json_object_get_string(max_from));
        return -1;
      }

      props.enum_count = json_object_get_int(count);

      struct json_object *value_format;
      if (!json_object_object_get_ex(control_config, "label-format", &value_format)) {
        log_error("Cannot find label-format for %s", member_path);
        return -1;
      }

      const char *format = json_object_get_string(value_format);
      props.enum_names = calloc(props.enum_count, sizeof(char *));
      if (!props.enum_names) {
        log_error("Cannot allocate memory for enum names");
        exit(1);
      }

      for (int i = 0; i < props.enum_count; i++) {
        if (asprintf(&props.enum_names[i], format, i + 1) < 0) {
          log_error("Cannot allocate memory for enum name");
          exit(1);
        }
      }

    } else {
      log_error("Cannot find max-from for %s", member_path);
      return -1;
    }

  } else if (!strcmp(type_str, "bool")) {
    props.type = SND_CTL_ELEM_TYPE_BOOLEAN;
    props.min = 0;
    props.max = 1;

  } else if (!strcmp(type_str, "bytes")) {
    props.type = SND_CTL_ELEM_TYPE_BYTES;
    props.size = json_object_get_int(json_object_object_get(member, "size"));
    props.read_bytes_func = read_bytes_control;
    props.write_bytes_func = write_bytes_control;

  } else if (!strcmp(type_str, "int")) {
    struct json_object *min, *max, *interface, *access;

    props.type = SND_CTL_ELEM_TYPE_INTEGER;

    if (props.data_type == DATA_TYPE_UINT8) {
      props.min = 0;
      props.max = 255;
    } else if (props.data_type == DATA_TYPE_UINT16) {
      props.min = 0;
      props.max = 65535;
    } else if (props.data_type == DATA_TYPE_UINT32) {
      props.min = 0;
      props.max = 2147483647; /* not supporting unsigned int32 yet */
    } else {
      log_error("Invalid data type %d for global control: %s", props.data_type, member_path);
      return -1;
    }

    if (json_object_object_get_ex(control_config, "min", &min)) {
      props.min = json_object_get_int(min);
    }
    if (json_object_object_get_ex(control_config, "max", &max)) {
      props.max = json_object_get_int(max);
    }
    if (json_object_object_get_ex(control_config, "interface", &interface)) {
      const char *interface_str = json_object_get_string(interface);
      if (!strcmp(interface_str, "mixer")) {
        props.interface = SND_CTL_ELEM_IFACE_MIXER;
      } else if (!strcmp(interface_str, "card")) {
        props.interface = SND_CTL_ELEM_IFACE_CARD;
      } else {
        log_error("Unsupported interface for global control: %s", interface_str);
        return -1;
      }
    }
    if (json_object_object_get_ex(control_config, "access", &access)) {
      const char *access_str = json_object_get_string(access);
      if (!strcmp(access_str, "readonly")) {
        props.read_only = 1;
      } else if (!strcmp(access_str, "readwrite")) {
        props.read_only = 0;
      } else {
        log_error("Unsupported access for global control: %s", access_str);
        return -1;
      }
    }

  } else {
    log_error("Unsupported control type for global control: %s", type_str);
    return -1;
  }

  return add_control(device, &props);
}

int init_global_controls(struct fcp_device *device) {

  /* Get global control configuration */
  struct json_object *global_controls;
  if (!json_object_object_get_ex(device->fam, "global-controls", &global_controls)) {
    log_error("Cannot find global-controls in configuration");
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

  /* Get enums */
  struct json_object *enums;
  if (!json_object_object_get_ex(device->devmap, "enums", &enums)) {
    log_error("Cannot find enums in device map");
    return -1;
  }

  /* Process each global control */
  json_object_object_foreach(global_controls, member_path, control_config) {
    create_global_control(device, member_path, control_config, enums);
  }

  return 0;
}
