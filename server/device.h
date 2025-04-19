// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <alsa/asoundlib.h>
#include <json-c/json.h>

#include "mix.h"
#include "mux.h"

#define CATEGORY_DATA  0x01
#define CATEGORY_SYNC  0x02
#define CATEGORY_MIX   0x03
#define CATEGORY_MUX   0x04

// bit 0 = signed
#define DATA_TYPE_UINT8  0x02
#define DATA_TYPE_INT8   0x03
#define DATA_TYPE_UINT16 0x04
#define DATA_TYPE_INT16  0x05
#define DATA_TYPE_UINT32 0x08

struct control_manager {
  struct control_props *controls;
  int                  num_controls;
  int                  capacity;
};

struct fcp_device {
  int                     card_num;
  uint16_t                usb_vid;
  uint16_t                usb_pid;
  snd_ctl_t              *ctl;
  snd_hwdep_t            *hwdep;
  json_object            *devmap;
  json_object            *fam;
  int                     ctl_fd;
  int                     hwdep_fd;
  fd_set                  rfds;
  int                     mix_input_count;
  int                     mix_output_count;
  int                     mix_input_control_count;
  struct mix_cache_entry *mix_cache;
  struct mux_cache       *mux_cache;
  struct control_manager  ctrl_mgr;
};

struct control_props {
  char  *name;
  int    array_index;
  int    interface;
  int    type;
  int    data_type;
  int    category;
  int    min;
  int    max;
  int    step;
  int    link;
  const unsigned int *tlv;
  char **enum_names;
  int   *enum_values;
  int    enum_count;
  int    read_only;
  int    notify_client;
  int    notify_device;
  int    offset;
  int    component_count;  // >0 for multi-component controls
  int   *offsets;          // offsets of each component
  int   *data_types;       // types of each component
  int    size;             // for BYTES controls
  int    value;
  void  *bytes_value;      // for BYTES controls - stores current value
  int    (*read_func)(struct fcp_device *, struct control_props *, int *);
  int    (*write_func)(struct fcp_device *, struct control_props *, int);
  int    (*read_bytes_func)(struct fcp_device *, struct control_props *, void *, size_t);
  int    (*write_bytes_func)(struct fcp_device *, struct control_props *, const void *, size_t);
};

void remove_all_user_controls(struct fcp_device *device);
int add_user_control(struct fcp_device *device, struct control_props *props);
