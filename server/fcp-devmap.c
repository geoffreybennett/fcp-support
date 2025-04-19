// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <alsa/asoundlib.h>
#include <openssl/evp.h>
#include <zlib.h>

#include "fcp.h"
#include "fcp-devmap.h"
#include "log.h"

static json_object *try_load_devmap_json(const char *dir, const char *filename) {
  if (!dir)
    return json_object_from_file(filename);

  char *path;
  json_object *obj = NULL;
  if (asprintf(&path, "%s/%s", dir, filename) >= 0) {
    obj = json_object_from_file(path);
    free(path);
  }
  return obj;
}

static int fcp_devmap_read_from_file(struct fcp_device *device) {
  char *filename;
  if (asprintf(&filename, "fcp-devmap-%04x.json", device->usb_pid) < 0) {
    log_error("Failed to allocate memory for filename");
    exit(1);
  }

  // Try locations in order: env var, current dir, system dir
  const char *search_dirs[] = {
    getenv("FCP_SERVER_DATA_DIR"),
    NULL,
    DATADIR
  };

  for (size_t i = 0; i < sizeof(search_dirs) / sizeof(search_dirs[0]); i++) {
    device->devmap = try_load_devmap_json(search_dirs[i], filename);
    if (device->devmap) {
      free(filename);
      return 0;
    }
  }

  free(filename);
  return -ENOENT;
}

static int fcp_devmap_read_from_device(struct fcp_device *device) {
  snd_hwdep_t *hwdep = device->hwdep;
  char *encoded_buf;

  /* Read the device map */
  int encoded_size = fcp_devmap_read(hwdep, &encoded_buf);
  if (encoded_size < 0) {
    return encoded_size;
  }

  /* base64 decode */
  uint8_t *decoded_buf = malloc(EVP_DECODE_LENGTH(encoded_size));
  if (!decoded_buf) {
    free(encoded_buf);
    return -ENOMEM;
  }

  EVP_ENCODE_CTX *ctx = EVP_ENCODE_CTX_new();
  if (!ctx) {
    free(encoded_buf);
    free(decoded_buf);
    return -ENOMEM;
  }

  int outl;
  int decoded_size = 0;
  EVP_DecodeInit(ctx);
  EVP_DecodeUpdate(
    ctx,
    decoded_buf,
    &outl,
    (unsigned char *)encoded_buf,
    encoded_size
  );
  decoded_size += outl;
  EVP_DecodeFinal(ctx, decoded_buf + decoded_size, &outl);
  decoded_size += outl;
  EVP_ENCODE_CTX_free(ctx);
  free(encoded_buf);

  if (decoded_size <= 0) {
    free(decoded_buf);
    return -EINVAL;
  }

  /* zlib decode */
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  if (inflateInit(&strm) != Z_OK) {
    free(decoded_buf);
    return -EINVAL;
  }

  strm.next_in = decoded_buf;
  strm.avail_in = decoded_size;

  /* create a buffer big enough to hold the uncompressed data */
  size_t json_size = decoded_size * 16;
  uint8_t *json_buf = malloc(json_size);
  if (!json_buf) {
    free(decoded_buf);
    return -ENOMEM;
  }

  strm.next_out = json_buf;
  strm.avail_out = json_size;

  int ret = inflate(&strm, Z_FINISH);
  if (ret != Z_STREAM_END) {
    free(decoded_buf);
    free(json_buf);
    inflateEnd(&strm);
    return -EINVAL;
  }

  size_t json_len = json_size - strm.avail_out;

  inflateEnd(&strm);
  free(decoded_buf);

  json_buf[json_len] = '\0';

  /* write the json to a file for debugging */
  char *fn;
  if (asprintf(&fn, "/tmp/fcp-devmap-%04x.json", device->usb_pid) < 0) {
    log_error("Failed to allocate memory for filename");
    exit(1);
  }

  FILE *f = fopen(fn, "w");
  if (f) {
    fwrite(json_buf, 1, json_len, f);
    fclose(f);
  }

  /* parse json */
  device->devmap = json_tokener_parse((char *)json_buf);
  if (!device->devmap) {
    free(json_buf);
    return -EINVAL;
  }

  return 0;
}

int fcp_devmap_read_json(struct fcp_device *device) {
  int err = fcp_devmap_read_from_file(device);
  if (err == -ENOENT)
    err = fcp_devmap_read_from_device(device);

  return err;
}
