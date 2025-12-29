// SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>
#include <stddef.h>

// FCP opcodes for data commands
#define FCP_OPCODE_CATEGORY_DATA  0x800
#define FCP_OPCODE_DATA_READ      (FCP_OPCODE_CATEGORY_DATA << 12 | 0x000)
#define FCP_OPCODE_DATA_WRITE     (FCP_OPCODE_CATEGORY_DATA << 12 | 0x001)
#define FCP_OPCODE_DATA_NOTIFY    (FCP_OPCODE_CATEGORY_DATA << 12 | 0x002)

// Data command handler
int data_cmd(void);

// Send an FCP command (implemented in main.c)
int send_fcp_cmd(
  uint32_t opcode,
  const void *req_data,
  size_t req_size,
  size_t resp_size
);

// Response storage (in main.c)
extern void *data_response;
extern size_t data_response_size;

// Command arguments (in main.c)
extern int cmd_argc;
extern char **cmd_argv;

// Program name (in main.c)
extern const char *program_name;
