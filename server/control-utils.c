// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "control-utils.h"
#include "fcp.h"
#include "log.h"

int find_member_by_path(
  struct fcp_device   *device,
  const char          *path,
  struct json_object **found_member,
  const char         **member_type,
  int                 *total_offset,
  bool                 allow_missing
) {
  return find_member_by_path_with_notify(
    device, path, found_member, member_type, total_offset,
    NULL, NULL, allow_missing
  );
}

int find_member_by_path_with_notify(
  struct fcp_device   *device,
  const char          *path,
  struct json_object **found_member,
  const char         **member_type,
  int                 *total_offset,
  int                 *notify_device,
  int                 *notify_client,
  bool                 allow_missing
) {
  struct json_object *structs, *current_struct, *current_members;
  char *path_copy = strdup(path);
  char *token, *saveptr;
  const char *current_type = "APP_SPACE";
  *total_offset = 0;

  /* Track last non-null notify values */
  int last_notify_device = 0;
  int last_notify_client = 0;

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

    /* Track notify values - use this member's value if non-null */
    struct json_object *nd = json_object_object_get(member, "notify-device");
    struct json_object *nc = json_object_object_get(member, "notify-client");
    if (nd && !json_object_is_type(nd, json_type_null))
      last_notify_device = json_object_get_int(nd);
    if (nc && !json_object_is_type(nc, json_type_null))
      last_notify_client = json_object_get_int(nc);

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
  if (notify_device)
    *notify_device = last_notify_device;
  if (notify_client)
    *notify_client = last_notify_client;
  return 0;
}

int devmap_type_to_data_type(const char *type) {
  if (!strcmp(type, "bool"))
    return DATA_TYPE_UINT8;

  if (!strcmp(type, "uint8"))
    return DATA_TYPE_UINT8;
  if (!strcmp(type, "uint16"))
    return DATA_TYPE_UINT16;
  if (!strcmp(type, "uint32"))
    return DATA_TYPE_UINT32;

  if (!strcmp(type, "int8"))
    return DATA_TYPE_INT8;
  if (!strcmp(type, "int16"))
    return DATA_TYPE_INT16;

  log_error("Unhandled data type %s", type);
  exit(1);
}

/* Convert a devmap type to a data type, overriding the width */
int devmap_type_to_data_type_with_width(const char *type, int width) {
  int ret = 0;
  if (width == 1)
    ret = DATA_TYPE_UINT8;
  else if (width == 2)
    ret = DATA_TYPE_UINT16;
  else if (width == 4)
    ret = DATA_TYPE_UINT32;
  else {
    log_error("Unhandled width %d", width);
    exit(1);
  }
  if (strncmp(type, "int", 3) == 0)
    ret |= 1;
  return ret;
}

static int read_single_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                   data_type,
  int                   offset,
  int                   array_index,
  int                  *value
) {
  int width;

  if (data_type == DATA_TYPE_UINT8 ||
      data_type == DATA_TYPE_INT8) {
    width = 1;
  } else if (data_type == DATA_TYPE_UINT16 ||
             data_type == DATA_TYPE_INT16) {
    width = 2;
  } else if (data_type == DATA_TYPE_UINT32) {
    width = 4;
  } else {
    log_error("Invalid data type %d for control %s", data_type, props->name);
    return -1;
  }
  bool is_signed = data_type & 1;

  return fcp_data_read(
    device->hwdep,
    offset + props->array_index * width,
    width,
    is_signed,
    value
  );
}

int read_data_control(struct fcp_device *device, struct control_props *props, int *value) {
  if (!props->component_count) {
    int read_value, err;

    err = read_single_data_control(
      device, props,
      props->data_type, props->offset, props->array_index,
      &read_value
    );

    if (err < 0)
      return err;

    /* For enumerated controls with explicit values, map the value
     * back to the index
     */
    if (props->type == SND_CTL_ELEM_TYPE_ENUMERATED && props->enum_values) {
      for (int i = 0; i < props->enum_count; i++) {
        if (props->enum_values[i] == read_value) {
          log_debug("Read %s as %s (%d)", props->name, props->enum_names[i], i);
          *value = i;
          return 0;
        }
      }
      log_error(
        "Invalid enumerated value %d for control %s",
        read_value, props->name
      );
      return -1;
    }

    *value = read_value;
    return 0;
  }

  for (int i = 0; i < props->component_count; i++) {
    int offset = props->offsets[i];
    int data_type = props->data_types[i];

    int err = read_single_data_control(
      device, props,
      data_type, offset, props->array_index,
      &value[i]
    );
    if (err < 0)
      return err;
  }

  return 0;
}

int write_data_control(struct fcp_device *device, struct control_props *props, int value) {
  int width = 0;

  if (props->read_only) {
    log_error("Read-only control %s cannot be written", props->name);
    return -1;
  }

  if (!props->offset) {
    log_error("Control %s has no offset", props->name);
    return -1;
  }

  if (props->component_count) {
    log_error("Multi-component control %s cannot be written", props->name);
    return -1;
  }

  /* For enumerated controls with explicit values, map the index to
   * the value
   */
  if (props->type == SND_CTL_ELEM_TYPE_ENUMERATED && props->enum_values) {
    if (value < 0 || value >= props->enum_count) {
      log_error("Invalid enumerated value %d for control %s",
                value, props->name);
      return -1;
    }
    value = props->enum_values[value];
  }

  if (props->data_type == DATA_TYPE_UINT8 ||
      props->data_type == DATA_TYPE_INT8) {
    width = 1;
  } else if (props->data_type == DATA_TYPE_UINT16 ||
             props->data_type == DATA_TYPE_INT16) {
    width = 2;
  } else if (props->data_type == DATA_TYPE_UINT32) {
    width = 4;
  } else {
    log_error("Invalid data type %d for control %s",
              props->data_type, props->name);
    return -1;
  }

  int offset = props->offset + props->array_index * width;
  return fcp_data_write(device->hwdep, offset, width, value);
}

int read_bitmap_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                  *value
) {
  if (!props->offset) {
    log_error("Control %s has no offset", props->name);
    return -1;
  }

  int width;
  if (props->data_type == DATA_TYPE_UINT8 ||
      props->data_type == DATA_TYPE_INT8) {
    width = 1;
  } else if (props->data_type == DATA_TYPE_UINT16 ||
             props->data_type == DATA_TYPE_INT16) {
    width = 2;
  } else if (props->data_type == DATA_TYPE_UINT32) {
    width = 4;
  } else {
    log_error("Invalid data type %d for control %s",
              props->data_type, props->name);
    return -1;
  }

  int read_value;

  int err = fcp_data_read(device->hwdep, props->offset, width, false, &read_value);
  if (err < 0)
    return err;

  *value = (read_value >> props->array_index) & 1;

  return 0;
}

int write_bitmap_data_control(
  struct fcp_device    *device,
  struct control_props *props,
  int                   value
) {
  if (props->read_only) {
    log_error("Read-only control %s cannot be written", props->name);
    return -1;
  }

  if (!props->offset) {
    log_error("Control %s has no offset", props->name);
    return -1;
  }

  int width;
  if (props->data_type == DATA_TYPE_UINT8 ||
      props->data_type == DATA_TYPE_INT8) {
    width = 1;
  } else if (props->data_type == DATA_TYPE_UINT16 ||
             props->data_type == DATA_TYPE_INT16) {
    width = 2;
  } else if (props->data_type == DATA_TYPE_UINT32) {
    width = 4;
  } else {
    log_error("Invalid data type %d for control %s",
              props->data_type, props->name);
    return -1;
  }

  int read_value;

  int err = fcp_data_read(device->hwdep, props->offset, width, false, &read_value);
  if (err < 0)
    return err;

  int mask = 1 << props->array_index;
  if (value)
    read_value |= mask;
  else
    read_value &= ~mask;

  return fcp_data_write(device->hwdep, props->offset, width, read_value);
}

int read_bytes_control(
  struct fcp_device    *device,
  struct control_props *props,
  void                 *data,
  size_t                size
) {
  if (!props->offset) {
    log_error("Control %s has no offset", props->name);
    return -1;
  }

  if (size != props->size) {
    log_error(
      "Size mismatch for control %s: expected %d, got %zu",
      props->name, props->size, size
    );
    return -1;
  }

  return fcp_data_read_buf(device->hwdep, props->offset, size, data);
}

int write_bytes_control(
  struct fcp_device    *device,
  struct control_props *props,
  const void           *data,
  size_t                size
) {
  if (props->read_only) {
    log_error("Read-only control %s cannot be written", props->name);
    return -1;
  }

  if (!props->offset) {
    log_error("Control %s has no offset", props->name);
    return -1;
  }

  if (size != props->size) {
    log_error(
      "Size mismatch for control %s: expected %d, got %zu",
      props->name, props->size, size
    );
    return -1;
  }

  return fcp_data_write_buf(device->hwdep, props->offset, size, data);
}
