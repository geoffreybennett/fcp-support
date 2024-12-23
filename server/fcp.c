// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <alsa/asoundlib.h>
#include <json-c/json.h>

#include "fcp.h"
#include "log.h"

#include "uapi-fcp.h"

#define FCP_OPCODE_INIT_1               (FCP_OPCODE_CATEGORY_INIT    << 12 | 0x000)
#define FCP_OPCODE_CAP_READ             (FCP_OPCODE_CATEGORY_INIT    << 12 | 0x001)
#define FCP_OPCODE_INIT_2               (FCP_OPCODE_CATEGORY_INIT    << 12 | 0x002)
#define FCP_OPCODE_REBOOT               (FCP_OPCODE_CATEGORY_INIT    << 12 | 0x003)
#define FCP_OPCODE_METER_INFO           (FCP_OPCODE_CATEGORY_METER   << 12 | 0x000)
#define FCP_OPCODE_METER_READ           (FCP_OPCODE_CATEGORY_METER   << 12 | 0x001)
#define FCP_OPCODE_MIX_INFO             (FCP_OPCODE_CATEGORY_MIX     << 12 | 0x000)
#define FCP_OPCODE_MIX_READ             (FCP_OPCODE_CATEGORY_MIX     << 12 | 0x001)
#define FCP_OPCODE_MIX_WRITE            (FCP_OPCODE_CATEGORY_MIX     << 12 | 0x002)
#define FCP_OPCODE_MUX_INFO             (FCP_OPCODE_CATEGORY_MUX     << 12 | 0x000)
#define FCP_OPCODE_MUX_READ             (FCP_OPCODE_CATEGORY_MUX     << 12 | 0x001)
#define FCP_OPCODE_MUX_WRITE            (FCP_OPCODE_CATEGORY_MUX     << 12 | 0x002)
#define FCP_OPCODE_FLASH_INFO           (FCP_OPCODE_CATEGORY_FLASH   << 12 | 0x000)
#define FCP_OPCODE_FLASH_SEGMENT_INFO   (FCP_OPCODE_CATEGORY_FLASH   << 12 | 0x001)
#define FCP_OPCODE_FLASH_ERASE          (FCP_OPCODE_CATEGORY_FLASH   << 12 | 0x002)
#define FCP_OPCODE_FLASH_ERASE_PROGRESS (FCP_OPCODE_CATEGORY_FLASH   << 12 | 0x003)
#define FCP_OPCODE_FLASH_WRITE          (FCP_OPCODE_CATEGORY_FLASH   << 12 | 0x004)
#define FCP_OPCODE_FLASH_READ           (FCP_OPCODE_CATEGORY_FLASH   << 12 | 0x005)
#define FCP_OPCODE_SYNC_READ            (FCP_OPCODE_CATEGORY_SYNC    << 12 | 0x004)
#define FCP_OPCODE_ESP_DFU_START        (FCP_OPCODE_CATEGORY_ESP_DFU << 12 | 0x000)
#define FCP_OPCODE_ESP_DFU_WRITE        (FCP_OPCODE_CATEGORY_ESP_DFU << 12 | 0x001)
#define FCP_OPCODE_DATA_READ            (FCP_OPCODE_CATEGORY_DATA    << 12 | 0x000)
#define FCP_OPCODE_DATA_WRITE           (FCP_OPCODE_CATEGORY_DATA    << 12 | 0x001)
#define FCP_OPCODE_DATA_NOTIFY          (FCP_OPCODE_CATEGORY_DATA    << 12 | 0x002)
#define FCP_OPCODE_DEVMAP_INFO          (FCP_OPCODE_CATEGORY_DATA    << 12 | 0x00c)
#define FCP_OPCODE_DEVMAP_READ          (FCP_OPCODE_CATEGORY_DATA    << 12 | 0x00d)

#define FCP_CMD_CONFIG_SAVE 6

/* Initialise the device */
void fcp_init(snd_hwdep_t *hwdep) {
  int err;
  struct fcp_step0 step0;
  struct fcp_cmd cmd;
  uint8_t step0_buf[24] = {0};
  uint8_t step2_buf[84] = {0};

  /* Check protocol version */
  int version;
  err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_PVERSION, &version);
  if (err < 0) {
    log_error("Cannot get protocol version: %s", snd_strerror(err));
    exit(1);
  }

  log_debug("Protocol version: %d.%d.%d",
           FCP_HWDEP_VERSION_MAJOR(version),
           FCP_HWDEP_VERSION_MINOR(version),
           FCP_HWDEP_VERSION_SUBMINOR(version));
  if (FCP_HWDEP_VERSION_MAJOR(version) != 2 ||
      FCP_HWDEP_VERSION_MINOR(version) != 0) {

    /* Exit quietly if the protocol version is 1.x (scarlett2 driver) */
    if (FCP_HWDEP_VERSION_MAJOR(version) == 1) {
      log_debug(
        "Protocol version 1.x is the ALSA scarlett2 driver "
        "which is supported by the scarlett2 utility."
      );
      log_debug("This daemon (fcp-server) is for the ALSA FCP driver.");
      exit(0);
    }

    log_error(
      "Unsupported protocol version (%d.%d.x expected, got %d.%d.%d)",
      2, 0,
      FCP_HWDEP_VERSION_MAJOR(version),
      FCP_HWDEP_VERSION_MINOR(version),
      FCP_HWDEP_VERSION_SUBMINOR(version)
    );

    exit(1);
  }

  /* Step 0: Get initial data */
  memset(&step0, 0, sizeof(step0));
  step0.data = step0_buf;
  step0.size = sizeof(step0_buf);

  err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_INIT, &step0);
  if (err < 0) {
    log_error("FCP step 0 failed: %s", snd_strerror(err));
    exit(1);
  }

  /* Step 1: Send INIT_1 command */
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = FCP_OPCODE_INIT_1;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = NULL;
  cmd.resp_size = 0;

  err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("FCP step 1 failed: %s", snd_strerror(err));
    exit(1);
  }

  /* Step 2: Send INIT_2 command and get firmware version */
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = FCP_OPCODE_INIT_2;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = step2_buf;
  cmd.resp_size = sizeof(step2_buf);

  err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("FCP step 2 failed: %s", snd_strerror(err));
    exit(1);
  }

  /* Extract firmware version from step2_buf[8] */
  uint32_t firmware_version;
  memcpy(&firmware_version, step2_buf + 8, sizeof(firmware_version));
  firmware_version = le32toh(firmware_version);
  log_debug("Firmware version: %d", firmware_version);
}

int fcp_cap_read(snd_hwdep_t *hwdep, int opcode_category) {
  struct fcp_cmd cmd;
  uint16_t opcode_category_req = htole16(opcode_category);
  uint8_t supported = 0;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_CAP_READ;
  cmd.req = &opcode_category_req;
  cmd.req_size = sizeof(opcode_category_req);
  cmd.resp = &supported;
  cmd.resp_size = sizeof(supported);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get capabilities failed: %s", snd_strerror(err));
    return err;
  }

  return supported;
}

/* Reboot the device */
int fcp_reboot(snd_hwdep_t *hwdep) {
  struct fcp_cmd cmd;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_REBOOT;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = NULL;
  cmd.resp_size = 0;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0)
    log_error("Reboot failed: %s", snd_strerror(err));

  return err;
}

/* Display meter info */
int fcp_meter_info(snd_hwdep_t *hwdep, int *num_meter_slots) {
  struct fcp_cmd cmd;
  uint8_t resp[4] = {0};

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_METER_INFO;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = resp;
  cmd.resp_size = sizeof(resp);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get meter info failed: %s", snd_strerror(err));
    return err;
  }

  *num_meter_slots = resp[0];

  return 0;
}

int fcp_meter_read(snd_hwdep_t *hwdep, int count, int *value) {
  struct fcp_cmd cmd;
  struct {
    uint16_t offset;
    uint16_t count;
    uint32_t pad;
  } __attribute__((packed)) req;
  int resp_size = sizeof(uint32_t) * count;
  uint32_t *resp = calloc(1, resp_size);

  if (!resp) {
    log_error("Cannot allocate memory for meter read");
    return -ENOMEM;
  }

  /* Prepare request data */
  req.offset = 0;
  req.count = htole16(count);
  req.pad = 0;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_METER_READ;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = resp;
  cmd.resp_size = resp_size;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get meter failed: %s", snd_strerror(err));
    goto done;
  }

  for (int i = 0; i < count; i++)
    value[i] = le32toh(resp[i]);

done:
  free(resp);
  return err;
}

/* Return mix info (I/O count) */
int fcp_mix_info(snd_hwdep_t *hwdep, int *num_outputs, int *num_inputs) {
  struct fcp_cmd cmd;
  uint8_t resp[8] = {0};

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_MIX_INFO;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = resp;
  cmd.resp_size = sizeof(resp);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get mix info failed: %s", snd_strerror(err));
    return err;
  }

  {
    char buf[256] = "Mix info:";
    char *p = buf + strlen(buf);
    for (int i = 0; i < sizeof(resp); i++)
      p += snprintf(p, sizeof(buf) - (p - buf), " %d", resp[i]);
    log_debug("%s", buf);
  }

  *num_outputs = resp[0];
  *num_inputs = resp[1];

  return 0;
}

/* Read mix data */
int fcp_mix_read(snd_hwdep_t *hwdep, int mix_num, int count, int *values) {
  struct fcp_cmd cmd;
  struct {
    uint16_t mix_num;
    uint16_t count;
  } __attribute__((packed)) req;
  int resp_size = sizeof(uint16_t) * count;
  uint16_t *resp = calloc(1, resp_size);

  if (!resp) {
    log_error("Cannot allocate memory for mix read");
    return -ENOMEM;
  }

  /* Prepare request data */
  req.mix_num = htole16(mix_num);
  req.count = htole16(count);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_MIX_READ;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = resp;
  cmd.resp_size = resp_size;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get mix failed: %s", snd_strerror(err));
    return err;
  }

  for (int i = 0; i < count; i++) {
    values[i] = le16toh(resp[i]);
  }

  free(resp);

  return 0;
}

/* Write mix data */
int fcp_mix_write(snd_hwdep_t *hwdep, int mix_num, int count, int *values) {
  struct fcp_cmd cmd;
  int req_size = sizeof(uint16_t) * (count + 1);
  struct {
    uint16_t mix_num;
    uint16_t values[];
  } __attribute__((packed)) *req = calloc(1, req_size);

  if (!req) {
    log_error("Cannot allocate memory for mix write");
    return -ENOMEM;
  }

  /* Prepare request data */
  req->mix_num = htole16(mix_num);
  for (int i = 0; i < count; i++)
    req->values[i] = htole16(values[i]);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_MIX_WRITE;
  cmd.req = req;
  cmd.req_size = req_size;
  cmd.resp = NULL;
  cmd.resp_size = 0;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0)
    log_error("Set mix failed: %s", snd_strerror(err));

  free(req);

  return err;
}

/* Retrieve mux info in 3-element array */
int fcp_mux_info(snd_hwdep_t *hwdep, int *values) {
  struct fcp_cmd cmd;
  uint16_t resp[6] = {0};

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_MUX_INFO;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = resp;
  cmd.resp_size = sizeof(resp);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get mux info failed: %s", snd_strerror(err));
    return err;
  }

  {
    char buf[256] = "Mux info:";
    char *p = buf + strlen(buf);
    for (int i = 0; i < sizeof(resp) / sizeof(resp[0]); i++)
      p += snprintf(p, sizeof(buf) - (p - buf), " %d", resp[i]);
    log_debug("%s", buf);
  }

  for (int i = 0; i < 3; i++)
    values[i] = le16toh(resp[i]);

  return 0;
}

/* Read mux data */
int fcp_mux_read(
  snd_hwdep_t *hwdep,
  int          mux_num,
  int          count,
  uint32_t    *values
) {
  struct fcp_cmd cmd;
  struct {
    uint8_t offset;
    uint8_t pad;
    uint8_t count;
    uint8_t mux_num;
  } __attribute__((packed)) req;
  int resp_size = sizeof(uint32_t) * count;
  uint32_t *resp = calloc(1, resp_size);

  if (!resp) {
    log_error("Cannot allocate memory for mux read");
    return -ENOMEM;
  }

  /* Prepare request data */
  req.offset = 0;
  req.pad = 0;
  req.count = htole16(count);
  req.mux_num = htole16(mux_num);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_MUX_READ;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = resp;
  cmd.resp_size = resp_size;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get mux failed: %s", snd_strerror(err));
    free(resp);
    return err;
  }

  /* Convert endianness */
  for (int i = 0; i < count; i++)
    values[i] = le32toh(resp[i]);

  free(resp);
  return 0;
}

/* Write mux data */
int fcp_mux_write(
  snd_hwdep_t *hwdep,
  int          mux_num,
  int          count,
  uint32_t    *values
) {
  struct fcp_cmd cmd;
  int req_size = sizeof(uint16_t) * 2 + sizeof(uint32_t) * count;
  struct {
    uint16_t pad;
    uint16_t mux_num;
    uint32_t values[];
  } __attribute__((packed)) *req = calloc(1, req_size);

  if (!req) {
    log_error("Cannot allocate memory for mux write");
    return -ENOMEM;
  }

  /* Prepare request data */
  req->pad = 0;
  req->mux_num = htole16(mux_num);
  for (int i = 0; i < count; i++) {
    req->values[i] = htole32(values[i]);
  }

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_MUX_WRITE;
  cmd.req = req;
  cmd.req_size = req_size;
  cmd.resp = NULL;
  cmd.resp_size = 0;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0)
    log_error("Set mux failed: %s", snd_strerror(err));

  free(req);
  return err;
}

/* Read flash info */
int fcp_flash_info(snd_hwdep_t *hwdep, int *size, int *count) {
  struct fcp_cmd cmd;
  struct {
    uint32_t size;
    uint32_t count;
    uint8_t  unknown[8];
  } __attribute__((packed)) resp;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_FLASH_INFO;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = &resp;
  cmd.resp_size = sizeof(resp);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get flash info failed: %s", snd_strerror(err));
    return err;
  }

  /* Convert from little-endian and return values */
  resp.size = le32toh(resp.size);
  resp.count = le32toh(resp.count);

  if (resp.size > 16 * 1024 * 1024) {
    log_error("Flash size too large: %d", resp.size);
    return -EOVERFLOW;
  }
  if (resp.count > 16) {
    log_error("Flash count too large: %d", resp.count);
    return -EOVERFLOW;
  }

  *size = resp.size;
  *count = resp.count;

  return 0;
}

/* Read flash segment info */
int fcp_flash_segment_info(
  snd_hwdep_t  *hwdep,
  int           segment_num,
  int          *size,
  uint32_t     *flags,
  char        **name
) {
  struct fcp_cmd cmd;
  uint32_t segment_num_req = htole32(segment_num);

  struct {
    uint32_t size;
    uint32_t flags;
    char     name[16];
  } __attribute__((packed)) resp;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_FLASH_SEGMENT_INFO;
  cmd.req = &segment_num_req;
  cmd.req_size = sizeof(segment_num_req);
  cmd.resp = &resp;
  cmd.resp_size = sizeof(resp);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get flash segment info failed: %s", snd_strerror(err));
    return err;
  }

  /* Ensure name is null-terminated */
  resp.name[15] = '\0';

  /* Convert from little-endian */
  resp.size = le32toh(resp.size);
  resp.flags = le32toh(resp.flags);

  if (resp.size > 16 * 1024 * 1024) {
    log_error("Flash segment size too large: %d", resp.size);
    return -EOVERFLOW;
  }

  /* Return values */
  *size = resp.size;
  *flags = resp.flags;
  *name = strdup(resp.name);

  if (!*name) {
    log_error("Cannot allocate memory for flash segment name");
    return -ENOMEM;
  }

  return 0;
}

/* Erase a flash segment */
int fcp_flash_erase(snd_hwdep_t *hwdep, int segment_num) {
  struct fcp_cmd cmd;
  struct {
    uint8_t  segment_num;
    uint8_t  pad[7];
  } __attribute__((packed)) req = {0};

  /* Prepare request data */
  req.segment_num = segment_num;

  if (segment_num < 1 || segment_num > 16) {
    log_error("Invalid segment number: %d", segment_num);
    return -EINVAL;
  }

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_FLASH_ERASE;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = NULL;
  cmd.resp_size = 0;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Flash erase failed: %s", snd_strerror(err));
    return err;
  }

  return 0;
}

/* Get flash erase progress */
int fcp_flash_erase_progress(snd_hwdep_t *hwdep, int segment_num) {
  struct fcp_cmd cmd;
  struct {
    uint32_t segment_num;
    uint32_t pad;
  } __attribute__((packed)) req;
  uint8_t progress;

  /* Prepare request data */
  req.segment_num = htole32(segment_num);
  req.pad = 0;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_FLASH_ERASE_PROGRESS;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = &progress;
  cmd.resp_size = sizeof(progress);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get flash erase progress failed: %s", snd_strerror(err));
    return err;
  }

  return progress;
}

/* Write data to flash */
int fcp_flash_write(
  snd_hwdep_t *hwdep,
  int          segment_num,
  int          offset,
  int          size,
  const void  *data
) {
  if (size > FCP_FLASH_WRITE_MAX) {
    log_error("Flash write size too large: %d", size);
    return -EINVAL;
  }

  if (segment_num < 1 || segment_num > 16) {
    log_error("Invalid segment number: %d", segment_num);
    return -EINVAL;
  }

  struct fcp_cmd cmd;
  int req_size = sizeof(uint32_t) * 3 + size;
  struct {
    uint32_t segment_num;
    uint32_t offset;
    uint32_t pad;
    uint8_t  data[];
  } __attribute__((packed)) *req = calloc(1, req_size);

  if (!req) {
    log_error("Cannot allocate memory for flash write");
    return -ENOMEM;
  }

  /* Prepare request data */
  req->segment_num = htole32(segment_num);
  req->offset = htole32(offset);
  memcpy(req->data, data, size);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_FLASH_WRITE;
  cmd.req = req;
  cmd.req_size = req_size;
  cmd.resp = NULL;
  cmd.resp_size = 0;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0)
    log_error("Flash write failed: %s", snd_strerror(err));

  free(req);
  return err;
}

/* Read the sync status */
int fcp_sync_read(snd_hwdep_t *hwdep) {
  struct fcp_cmd cmd;
  uint32_t buf = 0;

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_SYNC_READ;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = &buf;
  cmd.resp_size = sizeof(buf);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Read sync failed: %s", snd_strerror(err));
    return err;
  }

  return !!le32toh(buf);
}

/* Start ESP DFU */
int fcp_esp_dfu_start(snd_hwdep_t *hwdep, uint32_t length, const uint8_t *md5_hash) {
  struct fcp_cmd cmd;
  struct {
    uint32_t offset;
    uint32_t length;
    uint8_t  md5_hash[16];
  } __attribute__((packed)) req;

  // Prepare request
  req.offset = 0;
  req.length = htole32(length);
  memcpy(req.md5_hash, md5_hash, 16);

  // Set up command
  cmd.opcode = FCP_OPCODE_ESP_DFU_START;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = NULL;
  cmd.resp_size = 0;

  return snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
}

/* Write ESP DFU data */
int fcp_esp_dfu_write(snd_hwdep_t *hwdep, const void *data, size_t count) {
  struct fcp_cmd cmd;

  // Set up command
  cmd.opcode = FCP_OPCODE_ESP_DFU_WRITE;
  cmd.req = data;
  cmd.req_size = count;
  cmd.resp = NULL;
  cmd.resp_size = 0;

  return snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
}

/* Read 1/2/4 data bytes */
int fcp_data_read(
  snd_hwdep_t *hwdep,
  int          offset,
  int          size,
  bool         is_signed,
  int          *value
) {
  struct fcp_cmd cmd;
  struct {
    uint32_t offset;
    uint32_t size;
  } __attribute__((packed)) req;
  uint32_t resp = 0;

  /* Prepare request data */
  req.offset = htole32(offset);
  req.size = htole32(size);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_DATA_READ;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = &resp;
  cmd.resp_size = size;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get data failed: %s", snd_strerror(err));
    return err;
  }

  uint32_t raw_value = le32toh(resp);

  if (is_signed) {
    switch (size) {
      case 1:
        *value = (int8_t)raw_value;
        break;
      case 2:
        *value = (int16_t)raw_value;
        break;
      case 4:
        *value = (int32_t)raw_value;
        break;
      default:
        log_error("Invalid data size %d", size);
        return -EINVAL;
    }
  } else {
    *value = raw_value;
  }

  log_debug("Read data: offset=%d size=%d value=%d", offset, size, *value);
  return 0;
}

/* Write 1/2/4 data bytes */
int fcp_data_write(snd_hwdep_t *hwdep, int offset, int size, int value) {
  struct fcp_cmd cmd;
  struct {
    uint32_t offset;
    uint32_t size;
    uint32_t value;
  } __attribute__((packed)) req;

  /* Prepare request data */
  req.offset = htole32(offset);
  req.size = htole32(size);
  req.value = htole32(value);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_DATA_WRITE;
  cmd.req = &req;
  cmd.req_size = sizeof(uint32_t) * 2 + size;  /* offset + size + actual data */
  cmd.resp = NULL;
  cmd.resp_size = 0;

  log_debug("Writing data: offset=%d size=%d value=%d", offset, size, value);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Set data failed at offset %d: %s", offset, snd_strerror(err));
    return err;
  }

  return 0;
}

int fcp_data_notify(snd_hwdep_t *hwdep, int event) {
  struct fcp_cmd cmd;
  struct {
    uint32_t event;
  } __attribute__((packed)) req;

  /* Prepare request data */
  req.event = htole32(event);

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_DATA_NOTIFY;
  cmd.req = &req;
  cmd.req_size = sizeof(req);
  cmd.resp = NULL;
  cmd.resp_size = 0;

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Notify device failed: %s", snd_strerror(err));
    return err;
  }

  return 0;
}

/* Read the device map */
int fcp_devmap_read(snd_hwdep_t *hwdep, char **buf) {

  /* Get device map info */
  struct fcp_cmd cmd;
  uint16_t info_resp[2] = {0};

  /* Set up command structure */
  cmd.opcode = FCP_OPCODE_DEVMAP_INFO;
  cmd.req = NULL;
  cmd.req_size = 0;
  cmd.resp = &info_resp;
  cmd.resp_size = sizeof(info_resp);

  /* Send command */
  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
  if (err < 0) {
    log_error("Get device map info failed: %s", snd_strerror(err));
    return err;
  }

  int size = le16toh(info_resp[1]);

  /* Allocate buffer */
  *buf = calloc(size, 1);
  if (!*buf) {
    log_error("Cannot allocate memory for device map");
    return -ENOMEM;
  }

  /* Read device map */
  char data[FCP_DEVMAP_BLOCK_SIZE] = {0};
  cmd.opcode = FCP_OPCODE_DEVMAP_READ;
  uint32_t req_block_num;
  cmd.req = &req_block_num;
  cmd.req_size = sizeof(req_block_num);
  cmd.resp = data;

  for (int offset = 0; offset < size; offset += FCP_DEVMAP_BLOCK_SIZE) {
    req_block_num = htole32(offset / FCP_DEVMAP_BLOCK_SIZE);
    cmd.resp_size = FCP_DEVMAP_BLOCK_SIZE;
    if (offset + FCP_DEVMAP_BLOCK_SIZE > size)
      cmd.resp_size = size - offset;

    err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, &cmd);
    if (err < 0) {
      log_error("Read device map failed: %s", snd_strerror(err));
      free(*buf);
      *buf = NULL;
      return err;
    }

    memcpy(*buf + offset, data, cmd.resp_size);
  }

  return size;
}
