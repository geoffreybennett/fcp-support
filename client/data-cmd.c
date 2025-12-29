// SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <endian.h>

#include "data-cmd.h"

static void data_usage(void) {
  fprintf(stderr,
    "Usage: %s -c <card> data <subcommand> [args...]\n"
    "\n"
    "Subcommands:\n"
    "  read <offset> <length>         Read <length> bytes from <offset>\n"
    "  write <offset> <length> <val>  Write <length> (1/2/4) byte value\n"
    "  notify <value>                 Send notify event <value>\n"
    "\n"
    "Values can be decimal, hex (0x prefix), or negative.\n"
    "Hex writes are raw bytes; decimal writes are little-endian.\n"
    "\n"
    "Examples:\n"
    "  %s -c 0 data read 442 1          Read 1 byte at offset 442\n"
    "  %s -c 0 data write 442 1 35      Write 1-byte value 35\n"
    "  %s -c 0 data write 442 4 0x12345678  Write raw bytes 12 34 56 78\n"
    "  %s -c 0 data write 442 4 -1      Write 4-byte value -1 (ff ff ff ff)\n"
    "  %s -c 0 data notify 35           Send notify event 35\n"
    "\n"
    "Note: Requires FCP_DEBUG=1 when starting fcp-server.\n",
    program_name, program_name, program_name, program_name, program_name,
    program_name
  );
  exit(EXIT_FAILURE);
}

static long parse_number(const char *str) {
  char *endptr;
  errno = 0;
  long val = strtol(str, &endptr, 0);
  if (errno || *endptr) {
    fprintf(stderr, "Invalid number: %s\n", str);
    exit(EXIT_FAILURE);
  }
  return val;
}

static int data_read(void) {
  if (cmd_argc < 2) {
    fprintf(stderr, "data read: requires <offset> <size>\n");
    data_usage();
  }

  uint32_t offset = parse_number(cmd_argv[0]);
  uint32_t size = parse_number(cmd_argv[1]);

  if (size < 1 || size > 1024) {
    fprintf(stderr, "data read: size must be 1-1024\n");
    return -1;
  }

  struct {
    uint32_t offset;
    uint32_t size;
  } __attribute__((packed)) req = {
    .offset = htole32(offset),
    .size = htole32(size)
  };

  int ret = send_fcp_cmd(FCP_OPCODE_DATA_READ, &req, sizeof(req), size);
  if (ret != 0)
    return ret;

  uint8_t *data = data_response;

  if (data_response_size >= 16) {
    // Hexdump style
    for (size_t off = 0; off < data_response_size; off += 16) {
      printf("%08zx ", (size_t)offset + off);

      // Hex bytes
      for (size_t i = 0; i < 16; i++) {
        if (i == 8)
          printf(" ");
        if (off + i < data_response_size)
          printf(" %02x", data[off + i]);
        else
          printf("   ");
      }

      // ASCII
      printf("  |");
      for (size_t i = 0; i < 16 && off + i < data_response_size; i++) {
        char c = data[off + i];
        putchar((c >= 32 && c < 127) ? c : '.');
      }
      printf("|\n");
    }
  } else if (data_response_size <= 4) {
    // Show as little-endian integer with 0x prefix
    uint32_t val = 0;
    for (size_t i = 0; i < data_response_size; i++)
      val |= (uint32_t)data[i] << (i * 8);

    // Check if MSB is set
    int msb_set = data[data_response_size - 1] & 0x80;

    // Format string based on size
    const char *hex_fmt = data_response_size == 1 ? "0x%02X" :
                          data_response_size == 2 ? "0x%04X" : "0x%08X";
    printf(hex_fmt, val);

    if (msb_set) {
      // Show both signed and unsigned
      int32_t sval;
      switch (data_response_size) {
        case 1: sval = (int8_t)val; break;
        case 2: sval = (int16_t)val; break;
        default: sval = (int32_t)val; break;
      }
      printf(" (%d / %u)", sval, val);
    } else {
      printf(" (%u)", val);
    }
    printf("\n");
  } else {
    // 5-15 bytes: hex + ASCII
    for (size_t i = 0; i < data_response_size; i++) {
      printf("%02x", data[i]);
      if (i < data_response_size - 1)
        printf(" ");
    }
    printf(" \"");
    for (size_t i = 0; i < data_response_size; i++) {
      char c = data[i];
      if (c >= 32 && c < 127)
        putchar(c);
      else
        putchar('.');
    }
    printf("\"\n");
  }

  free(data_response);
  data_response = NULL;
  data_response_size = 0;

  return 0;
}

static int data_write(void) {
  if (cmd_argc != 3) {
    fprintf(stderr, "data write: requires <offset> <length> <value>\n");
    data_usage();
  }

  uint32_t offset = parse_number(cmd_argv[0]);
  int length = parse_number(cmd_argv[1]);
  const char *val_str = cmd_argv[2];

  if (length < 1 || length > 4) {
    fprintf(stderr, "data write: length must be 1, 2, or 4\n");
    return -1;
  }

  size_t req_size = sizeof(uint32_t) * 2 + length;
  uint8_t *req = malloc(req_size);
  if (!req) {
    fprintf(stderr, "Failed to allocate request buffer\n");
    return -1;
  }

  *(uint32_t *)req = htole32(offset);
  *(uint32_t *)(req + 4) = htole32(length);

  if (strncmp(val_str, "0x", 2) == 0 || strncmp(val_str, "0X", 2) == 0) {
    // Hex: raw bytes, no endian swap, must be exact length
    const char *hex = val_str + 2;
    size_t hex_len = strlen(hex);
    if (hex_len != (size_t)length * 2) {
      fprintf(stderr,
        "data write: hex value must have exactly %d hex digits for length %d\n",
        length * 2, length);
      free(req);
      return -1;
    }
    for (int i = 0; i < length; i++) {
      unsigned int byte;
      if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
        fprintf(stderr, "data write: invalid hex digit\n");
        free(req);
        return -1;
      }
      req[8 + i] = byte;
    }
  } else {
    // Decimal (possibly negative): little-endian byte order
    long val = parse_number(val_str);
    for (int i = 0; i < length; i++)
      req[8 + i] = (val >> (i * 8)) & 0xff;
  }

  int ret = send_fcp_cmd(FCP_OPCODE_DATA_WRITE, req, req_size, 0);
  free(req);

  if (ret == 0)
    printf("OK\n");

  return ret;
}

static int data_notify(void) {
  if (cmd_argc < 1) {
    fprintf(stderr, "data notify: requires <value>\n");
    data_usage();
  }

  uint32_t event = parse_number(cmd_argv[0]);

  struct {
    uint32_t event;
  } __attribute__((packed)) req = {
    .event = htole32(event)
  };

  int ret = send_fcp_cmd(FCP_OPCODE_DATA_NOTIFY, &req, sizeof(req), 0);
  if (ret == 0)
    printf("OK\n");

  return ret;
}

int data_cmd(void) {
  if (cmd_argc < 1)
    data_usage();

  const char *subcmd = cmd_argv[0];
  cmd_argc--;
  cmd_argv++;

  if (!strcmp(subcmd, "read"))
    return data_read();
  if (!strcmp(subcmd, "write"))
    return data_write();
  if (!strcmp(subcmd, "notify"))
    return data_notify();

  fprintf(stderr, "Unknown data subcommand: %s\n", subcmd);
  data_usage();
  return -1;
}
