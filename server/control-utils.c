// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>
#include "control-utils.h"
#include "fcp.h"
#include "log.h"

int devmap_type_to_data_type(const char *type) {
  if (!strcmp(type, "bool"))
    return DATA_TYPE_UINT8;

  if (!strcmp(type, "uint8"))
    return DATA_TYPE_UINT8;
  if (!strcmp(type, "uint16"))
    return DATA_TYPE_UINT16;
  if (!strcmp(type, "uint32"))
    return DATA_TYPE_UINT32;

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

  if (data_type == DATA_TYPE_UINT8) {
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
  if (!props->component_count)
    return read_single_data_control(
      device, props,
      props->data_type, props->offset, props->array_index,
      value
    );

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

  if (props->data_type == DATA_TYPE_UINT8) {
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