// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <alsa/asoundlib.h>
#include <stdbool.h>

#define FCP_OPCODE_CATEGORY_INIT    0x000
#define FCP_OPCODE_CATEGORY_METER   0x001
#define FCP_OPCODE_CATEGORY_MIX     0x002
#define FCP_OPCODE_CATEGORY_MUX     0x003
#define FCP_OPCODE_CATEGORY_FLASH   0x004
#define FCP_OPCODE_CATEGORY_SYNC    0x006
#define FCP_OPCODE_CATEGORY_ESP_DFU 0x009
#define FCP_OPCODE_CATEGORY_COUNT   10

#define FCP_OPCODE_CATEGORY_DATA    0x800

#define FCP_DEVMAP_BLOCK_SIZE 1024
#define FCP_FLASH_WRITE_MAX (1024 - 3 * sizeof(uint32_t))
#define FCP_FLASH_SEGMENT_SIZE 0x10000

/* FCP commands */
void fcp_init(snd_hwdep_t *hwdep);
int fcp_cap_read(snd_hwdep_t *hwdep, int opcode_category);
int fcp_reboot(snd_hwdep_t *hwdep);
int fcp_meter_info(snd_hwdep_t *hwdep, int *num_meter_slots);
int fcp_meter_read(snd_hwdep_t *hwdep, int count, int *value);
int fcp_mix_info(snd_hwdep_t *hwdep, int *num_outputs, int *num_inputs);
int fcp_mix_read(snd_hwdep_t *hwdep, int mix_num, int size, int *values);
int fcp_mix_write(snd_hwdep_t *hwdep, int mix_num, int size, int *values);
int fcp_mux_info(snd_hwdep_t *hwdep, int *values);
int fcp_mux_read(snd_hwdep_t *hwdep, int mux_num, int size, uint32_t *values);
int fcp_mux_write(snd_hwdep_t *hwdep, int mux_num, int size, uint32_t *values);
int fcp_flash_info(snd_hwdep_t *hwdep, int *size, int *count);
int fcp_flash_segment_info(snd_hwdep_t *hwdep, int segment_num, int *size, uint32_t *flags, char **name);
int fcp_flash_erase(snd_hwdep_t *hwdep, int segment_num);
int fcp_flash_erase_progress(snd_hwdep_t *hwdep, int segment_num);
int fcp_flash_write(snd_hwdep_t *hwdep, int segment_num, int offset, int size, const void *data);
int fcp_flash_read(snd_hwdep_t *hwdep, int segment_num, int offset, int size, void *data);
int fcp_sync_read(snd_hwdep_t *hwdep);
int fcp_esp_dfu_start(snd_hwdep_t *hwdep, uint32_t length, const uint8_t *md5_hash);
int fcp_esp_dfu_write(snd_hwdep_t *hwdep, const void *data, size_t count);
int fcp_data_read(snd_hwdep_t *hwdep, int offset, int size, bool is_signed, int *value);
int fcp_data_write(snd_hwdep_t *hwdep, int offset, int size, int value);
int fcp_data_read_buf(snd_hwdep_t *hwdep, int offset, int size, void *buf);
int fcp_data_write_buf(snd_hwdep_t *hwdep, int offset, int size, const void *buf);
int fcp_data_notify(snd_hwdep_t *hwdep, int event);
int fcp_devmap_info(snd_hwdep_t *hwdep);
int fcp_devmap_read(snd_hwdep_t *hwdep, char **buf);
