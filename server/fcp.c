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

#define FCP_STEP0_SIZE 24
#define FCP_STEP2_SIZE 84

/* Initialise the device */
void fcp_init(snd_hwdep_t *hwdep) {
  int err;
  int total_size = sizeof(struct fcp_init) + FCP_STEP0_SIZE + FCP_STEP2_SIZE;
  struct fcp_init *init = calloc(1, total_size);

  if (!init) {
    log_error("Cannot allocate memory for FCP init");
    exit(1);
  }

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

  /* Initialise FCP */
  init->step0_resp_size = FCP_STEP0_SIZE;
  init->step2_resp_size = FCP_STEP2_SIZE;
  init->init1_opcode = FCP_OPCODE_INIT_1;
  init->init2_opcode = FCP_OPCODE_INIT_2;

  err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_INIT, init);
  if (err < 0) {
    if (err == -ENOTTY) {
      log_error(
        "FCP init failed: %s (check the kernel FCP driver version)",
        snd_strerror(err)
      );
    } else {
      log_error("FCP init failed: %s", snd_strerror(err));
    }
    exit(1);
  }

  /* dump step0_buf and step2_buf contained within init data */
  __u8 *step0_buf = (__u8 *)init + sizeof(struct fcp_init);
  __u8 *step2_buf = step0_buf + FCP_STEP0_SIZE;

  /* Extract firmware version from step2_buf[8] */
  uint32_t firmware_version;
  memcpy(&firmware_version, step2_buf + 8, sizeof(firmware_version));
  firmware_version = le32toh(firmware_version);
  log_debug("Firmware version: %d", firmware_version);
}

static int fcp_cmd(
  snd_hwdep_t *hwdep,
  uint32_t     opcode,
  const void  *req,
  size_t       req_size,
  void        *resp,
  size_t       resp_size
) {
  int buf_size = req_size > resp_size ? req_size : resp_size;
  int total_size = sizeof(struct fcp_cmd) + buf_size;
  struct fcp_cmd *cmd = calloc(1, total_size);

  if (!cmd) {
    log_error("Cannot allocate memory for FCP command");
    exit(1);
  }

  cmd->opcode = opcode;
  cmd->req_size = req_size;
  cmd->resp_size = resp_size;

  if (req)
    memcpy(cmd->data, req, req_size);

  int err = snd_hwdep_ioctl(hwdep, FCP_IOCTL_CMD, cmd);
  if (err >= 0 && resp)
    memcpy(resp, cmd->data, resp_size);

  free(cmd);

  return err;
}

int fcp_cap_read(snd_hwdep_t *hwdep, int opcode_category) {
  uint16_t req = htole16(opcode_category);
  uint8_t resp = 0;

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_CAP_READ,
    &req, sizeof(req),
    &resp, sizeof(resp)
  );

  if (err < 0) {
    log_error("Get capabilities failed: %s", snd_strerror(err));
    return err;
  }

  return resp;
}

/* Reboot the device */
int fcp_reboot(snd_hwdep_t *hwdep) {
  int err = fcp_cmd(hwdep, FCP_OPCODE_REBOOT, NULL, 0, NULL, 0);
  if (err < 0)
    log_error("Reboot failed: %s", snd_strerror(err));

  return err;
}

/* Return meter info */
int fcp_meter_info(snd_hwdep_t *hwdep, int *num_meter_slots) {
  uint8_t resp[4] = {0};

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_METER_INFO,
    NULL, 0,
    resp, sizeof(resp)
  );

  if (err < 0) {
    log_error("Get meter info failed: %s", snd_strerror(err));
    return err;
  }

  *num_meter_slots = resp[0];

  return 0;
}

int fcp_meter_read(snd_hwdep_t *hwdep, int count, int *value) {
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

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_METER_READ,
    &req, sizeof(req),
    resp, resp_size
  );

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
  uint8_t resp[8] = {0};

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_MIX_INFO,
    NULL, 0,
    resp, sizeof(resp)
  );

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

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_MIX_READ,
    &req, sizeof(req),
    resp, resp_size
  );

  if (err < 0) {
    log_error("Get mix failed: %s", snd_strerror(err));
    free(resp);
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

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_MIX_WRITE,
    req, req_size,
    NULL, 0
  );

  if (err < 0)
    log_error("Set mix failed: %s", snd_strerror(err));

  free(req);
  return err;
}

/* Retrieve mux info in 3-element array */
int fcp_mux_info(snd_hwdep_t *hwdep, int *values) {
  uint16_t resp[6] = {0};

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_MUX_INFO,
    NULL, 0,
    resp, sizeof(resp)
  );

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
  struct {
    uint8_t offset;
    uint8_t pad;
    uint8_t count;
    uint8_t mux_num;
  } __attribute__((packed)) req;

  /* Allocate response buffer */
  size_t resp_size = sizeof(uint32_t) * count;
  uint32_t *resp = calloc(count, sizeof(uint32_t));
  if (!resp) {
    log_error("Cannot allocate memory for mux read");
    return -ENOMEM;
  }

  /* Prepare request data */
  req.offset = 0;
  req.pad = 0;
  req.count = htole16(count);
  req.mux_num = htole16(mux_num);

  /* Send command */
  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_MUX_READ,
    &req, sizeof(req),
    resp, resp_size
  );
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

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_MUX_WRITE,
    req, req_size,
    NULL, 0
  );

  if (err < 0)
    log_error("Set mux failed: %s", snd_strerror(err));

  free(req);
  return err;
}

/* Read flash info */
int fcp_flash_info(snd_hwdep_t *hwdep, int *size, int *count) {
  struct {
    uint32_t size;
    uint32_t count;
    uint8_t  unknown[8];
  } __attribute__((packed)) resp;

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_FLASH_INFO,
    NULL, 0,
    &resp, sizeof(resp)
  );

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
  uint32_t req = htole32(segment_num);
  struct {
    uint32_t size;
    uint32_t flags;
    char     name[16];
  } __attribute__((packed)) resp;

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_FLASH_SEGMENT_INFO,
    &req, sizeof(req),
    &resp, sizeof(resp)
  );

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

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_FLASH_ERASE,
    &req, sizeof(req),
    NULL, 0
  );

  if (err < 0) {
    log_error("Flash erase failed: %s", snd_strerror(err));
    return err;
  }

  return 0;
}

/* Get flash erase progress */
int fcp_flash_erase_progress(snd_hwdep_t *hwdep, int segment_num) {
  struct {
    uint32_t segment_num;
    uint32_t pad;
  } __attribute__((packed)) req = {0};
  uint8_t resp;

  /* Prepare request data */
  req.segment_num = htole32(segment_num);

  /* Send command */
  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_FLASH_ERASE_PROGRESS,
    &req, sizeof(req),
    &resp, sizeof(resp));
  if (err < 0) {
    log_error("Get flash erase progress failed: %s", snd_strerror(err));
    return err;
  }

  return resp;
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

  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_FLASH_WRITE,
    req, req_size,
    NULL, 0
  );

  if (err < 0)
    log_error("Flash write failed: %s", snd_strerror(err));

  free(req);
  return err;
}

/* Read the sync status */
int fcp_sync_read(snd_hwdep_t *hwdep) {
  uint32_t buf = 0;

  /* Send command */
  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_SYNC_READ,
    NULL, 0,
    &buf, sizeof(buf)
  );
  if (err < 0) {
    log_error("Read sync failed: %s", snd_strerror(err));
    return err;
  }

  return !!le32toh(buf);
}

/* Start ESP DFU */
int fcp_esp_dfu_start(snd_hwdep_t *hwdep, uint32_t length, const uint8_t *md5_hash) {
  struct {
    uint32_t offset;
    uint32_t length;
    uint8_t  md5_hash[16];
  } __attribute__((packed)) req = {0};

  // Prepare request
  req.length = htole32(length);
  memcpy(req.md5_hash, md5_hash, 16);

  return fcp_cmd(
    hwdep,
    FCP_OPCODE_ESP_DFU_START,
    &req, sizeof(req),
    NULL, 0
  );
}

/* Write ESP DFU data */
int fcp_esp_dfu_write(snd_hwdep_t *hwdep, const void *data, size_t count) {
  return fcp_cmd(
    hwdep,
    FCP_OPCODE_ESP_DFU_WRITE,
    data, count,
    NULL, 0
  );
}

/* Read 1/2/4 data bytes */
int fcp_data_read(
  snd_hwdep_t *hwdep,
  int          offset,
  int          size,
  bool         is_signed,
  int          *value
) {
  struct {
    uint32_t offset;
    uint32_t size;
  } __attribute__((packed)) req;
  uint32_t resp = 0;

  /* Prepare request data */
  req.offset = htole32(offset);
  req.size = htole32(size);

  /* Send command */
  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_DATA_READ,
    &req, sizeof(req), &resp, size);
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
  struct {
    uint32_t offset;
    uint32_t size;
    uint32_t value;
  } __attribute__((packed)) req;

  /* Prepare request data */
  req.offset = htole32(offset);
  req.size = htole32(size);
  req.value = htole32(value);

  log_debug("Writing data: offset=%d size=%d value=%d", offset, size, value);

  /* Send command */
  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_DATA_WRITE,
    &req, sizeof(uint32_t) * 2 + size, NULL, 0);
  if (err < 0) {
    log_error("Set data failed at offset %d: %s", offset, snd_strerror(err));
    return err;
  }

  return 0;
}

int fcp_data_notify(snd_hwdep_t *hwdep, int event) {
  struct {
    uint32_t event;
  } __attribute__((packed)) req;

  /* Prepare request data */
  req.event = htole32(event);

  /* Send command */
  return fcp_cmd(
    hwdep,
    FCP_OPCODE_DATA_NOTIFY,
    &req, sizeof(req),
    NULL, 0
  );
}

/* Read the device map */
int fcp_devmap_read(snd_hwdep_t *hwdep, char **buf) {

  /* Get device map info */
  uint16_t info_resp[2] = {0};

  /* Send command */
  int err = fcp_cmd(
    hwdep,
    FCP_OPCODE_DEVMAP_INFO,
    NULL, 0,
    &info_resp, sizeof(info_resp)
  );
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
  uint32_t req_block_num;

  for (int offset = 0; offset < size; offset += FCP_DEVMAP_BLOCK_SIZE) {
    req_block_num = htole32(offset / FCP_DEVMAP_BLOCK_SIZE);
    size_t resp_size = FCP_DEVMAP_BLOCK_SIZE;
    if (offset + FCP_DEVMAP_BLOCK_SIZE > size)
      resp_size = size - offset;

    err = fcp_cmd(
      hwdep,
      FCP_OPCODE_DEVMAP_READ,
      &req_block_num, sizeof(req_block_num),
      data, resp_size
    );
    if (err < 0) {
      log_error("Read device map failed: %s", snd_strerror(err));
      free(*buf);
      *buf = NULL;
      return err;
    }

    memcpy(*buf + offset, data, resp_size);
  }

  return size;
}
