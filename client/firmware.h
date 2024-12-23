// SPDX-FileCopyrightText: 2023-2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

enum firmware_type {
  FIRMWARE_CONTAINER,
  FIRMWARE_APP,
  FIRMWARE_ESP,
  FIRMWARE_LEAPFROG,
  FIRMWARE_TYPE_COUNT
};

extern const char *firmware_type_magic[FIRMWARE_TYPE_COUNT];

/* On-disk format of the firmware container header and the firmware
 * header. These are preceded by the 8-byte magic string identifying
 * the type of the following header.
 */
struct firmware_container_header_disk {
  uint16_t usb_vid;             // Big-endian
  uint16_t usb_pid;             // Big-endian
  uint32_t firmware_version[4]; // Big-endian
  uint32_t num_sections;        // Big-endian
} __attribute__((packed));

struct firmware_header_disk {
  uint16_t usb_vid;             // Big-endian
  uint16_t usb_pid;             // Big-endian
  uint32_t firmware_version[4]; // Big-endian
  uint32_t firmware_length;     // Big-endian
  uint8_t  sha256[32];
} __attribute__((packed));

/* In-memory representation of the firmware */
struct firmware {
  enum      firmware_type type;
  uint16_t  usb_vid;
  uint16_t  usb_pid;
  uint32_t  firmware_version[4];
  uint32_t  firmware_length;
  uint8_t   sha256[32];
  uint8_t   md5[16];
  uint8_t  *firmware_data;
};

/* In-memory representation of the firmware container */
struct firmware_container {
  uint16_t          usb_vid;
  uint16_t          usb_pid;
  uint32_t          firmware_version[4];
  uint32_t          num_sections;
  struct firmware **sections;
};

/* Read just the firmware container header from a file */
struct firmware_container *read_firmware_header(const char *fn);

/* Read all sections of a firmware container from a file */
struct firmware_container *read_firmware_file(const char *fn);

void free_firmware_container(struct firmware_container *container);

const char *firmware_type_to_string(enum firmware_type type);
