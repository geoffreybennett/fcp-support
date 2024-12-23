// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <json-c/json.h>
#include <string.h>

#include "device.h"
#include "log.h"

void remove_all_user_controls(struct fcp_device *device) {
  snd_ctl_elem_list_t *list;
  snd_ctl_elem_id_t   *id;
  unsigned int         count, i;
  int                  err;

  snd_ctl_elem_list_alloca(&list);
  snd_ctl_elem_id_alloca(&id);

  /* Get count of controls */
  err = snd_ctl_elem_list(device->ctl, list);
  if (err < 0) {
    log_error("Cannot list controls: %s", snd_strerror(err));
    return;
  }

  count = snd_ctl_elem_list_get_count(list);

  /* Get control IDs */
  err = snd_ctl_elem_list_alloc_space(list, count);
  if (err < 0) {
    log_error("Cannot allocate space for control list: %s", snd_strerror(err));
    return;
  }

  err = snd_ctl_elem_list(device->ctl, list);
  if (err < 0) {
    log_error("Cannot get control list: %s", snd_strerror(err));
    snd_ctl_elem_list_free_space(list);
    return;
  }

  /* Remove each mixer control */
  for (i = 0; i < count; i++) {
    snd_ctl_elem_list_get_id(list, i, id);

    snd_ctl_elem_info_t *info;
    snd_ctl_elem_info_alloca(&info);

    snd_ctl_elem_info_set_id(info, id);

    err = snd_ctl_elem_info(device->ctl, info);
    if (err < 0) {
      log_error("Cannot get control info: %s", snd_strerror(err));
      continue;
    }

    if (snd_ctl_elem_info_is_user(info)) {
      err = snd_ctl_elem_remove(device->ctl, id);
      if (err < 0) {
        log_error("Cannot remove control '%s': %s",
                  snd_ctl_elem_id_get_name(id), snd_strerror(err));
      }
    }
  }

  snd_ctl_elem_list_free_space(list);
}

int add_user_control(struct fcp_device *device, struct control_props *props) {
  snd_ctl_t           *ctl = device->ctl;
  snd_ctl_elem_info_t *info;
  snd_ctl_elem_id_t   *id;
  int err;

  snd_ctl_elem_info_alloca(&info);
  snd_ctl_elem_id_alloca(&id);

  /* Set up control ID */
  snd_ctl_elem_id_set_interface(id, props->interface);
  snd_ctl_elem_id_set_name(id, props->name);

  /* Set up control info */
  snd_ctl_elem_info_set_id(info, id);

  /* Try to remove if exists */
  snd_ctl_elem_remove(ctl, id);

  if (props->component_count) {
    if (props->type != SND_CTL_ELEM_TYPE_INTEGER) {
      log_error(
        "Invalid control type %d for multi-component control %s "
          "(must be integer)",
        props->type, props->name
      );
      return -1;
    } else if (!props->read_only) {
      log_error(
        "Multi-component control %s must be read-only",
        props->name
      );
      return -1;
    }
  }

  /* Add the control */
  if (props->type == SND_CTL_ELEM_TYPE_INTEGER) {
    int count = props->component_count ? props->component_count : 1;
    err = snd_ctl_add_integer_elem_set(
      ctl, info, 1, count, props->min, props->max, props->step
    );
  } else if (props->type == SND_CTL_ELEM_TYPE_BOOLEAN) {
    err = snd_ctl_add_boolean_elem_set(ctl, info, 1, 1);
  } else if (props->type == SND_CTL_ELEM_TYPE_ENUMERATED) {
    props->min = 0;
    props->max = props->enum_count - 1;
    props->step = 1;
    err = snd_ctl_add_enumerated_elem_set(
      ctl, info, 1, 1,
      props->enum_count,
      (const char * const *)props->enum_names
    );
  } else {
    log_error("Invalid control type %d for %s", props->type, props->name);
    return -1;
  }
  if (err < 0) {
    log_error(
      "Cannot add control '%s' (type=%d, interface=%d): %s",
      props->name,
      props->type,
      props->interface,
      snd_strerror(err)
    );
    return err;
  }

  /* Set the TLV */
  if (props->tlv) {
    err = snd_ctl_elem_tlv_write(ctl, id, props->tlv);
    if (err < 0) {
      log_error(
        "Cannot set TLV for control '%s': %s",
        props->name,
        snd_strerror(err)
      );
      return err;
    }
  }

  /* Get the initial value */
  int count = props->component_count ? props->component_count : 1;
  int values[count];

  err = props->read_func(device, props, values);
  if (err < 0) {
    log_error(
      "Cannot get initial value for control '%s': %s",
      props->name,
      snd_strerror(err)
    );
    return err;
  }

  /* Validate values are in range */
  for (int i = 0; i < count; i++) {
    if (values[i] < props->min || values[i] > props->max) {
      log_error(
        "Initial value %d for %s is out of range [%d, %d]",
        values[i],
        props->name,
        props->min,
        props->max
      );
      values[i] = values[i] < props->min ? props->min : props->max;
    }
  }

  /* Save the initial value (writing is not supported for
   * multi-component controls so we don't need to keep the
   * value)
   */
  if (!props->component_count)
    props->value = values[0];

  snd_ctl_elem_value_t *elem_value;
  snd_ctl_elem_value_alloca(&elem_value);
  snd_ctl_elem_value_set_id(elem_value, id);

  for (int i = 0; i < count; i++)
    snd_ctl_elem_value_set_integer(elem_value, i, values[i]);

  err = snd_ctl_elem_write(ctl, elem_value);
  if (err < 0) {
    log_error(
      "Cannot set %s to %d: %s",
      props->name,
      props->value,
      snd_strerror(err)
    );
    return err;
  }

  /* Unlock the control if it's not read-only.
   * Also unlock the Firmware Version control; use the Firmware
   * Version SCKT TLV + lock state to indicate to users that the
   * server is running.
   */
  if (!props->read_only || !strcmp(props->name, "Firmware Version")) {
    err = snd_ctl_elem_unlock(ctl, id);
    if (err < 0) {
      log_error(
        "Cannot unlock control '%s': %s",
        props->name,
        snd_strerror(err)
      );
      return err;
    }
  }

  return 0;
}
