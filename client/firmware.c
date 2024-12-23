// SPDX-FileCopyrightText: 2023-2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

const char *firmware_type_magic[] = {
  [FIRMWARE_CONTAINER] = "SCARLBOX",
  [FIRMWARE_APP]       = "SCARLET4",
  [FIRMWARE_ESP]       = "SCARLESP",
  [FIRMWARE_LEAPFROG]  = "SCARLEAP"
};

static int verify_sha256(
  const unsigned char *data,
  size_t length,
  const unsigned char *expected_hash
) {
  unsigned char computed_hash[SHA256_DIGEST_LENGTH];
  SHA256(data, length, computed_hash);
  return memcmp(computed_hash, expected_hash, SHA256_DIGEST_LENGTH) == 0;
}

// calculate MD5
static void md5(
  const unsigned char *data,
  size_t               length,
  unsigned char       *md5
) {
  EVP_MD_CTX   *mdctx;
  const EVP_MD *md;
  unsigned int  md_len;

  mdctx = EVP_MD_CTX_new();
  if (mdctx == NULL) {
    fprintf(stderr, "Failed to create MD5 context\n");
    exit(1);
  }
  md = EVP_md5();
  if (md == NULL) {
    fprintf(stderr, "Failed to get MD5 digest\n");
    EVP_MD_CTX_free(mdctx);
    exit(1);
  }

  EVP_DigestInit_ex(mdctx, md, NULL);
  EVP_DigestUpdate(mdctx, data, length);
  EVP_DigestFinal_ex(mdctx, md5, &md_len);

  EVP_MD_CTX_free(mdctx);
}

// Convert magic string to enum
static int firmware_type_from_magic(const char *magic) {
  for (int i = 0; i < FIRMWARE_TYPE_COUNT; i++)
    if (strncmp(magic, firmware_type_magic[i], 8) == 0)
      return i;
  return -1;
}

int read_magic(FILE *file, const char *fn) {
  char magic[8];
  if (fread(magic, sizeof(magic), 1, file) != 1) {
    perror("Failed to read magic");
    fprintf(stderr, "Error reading magic from %s\n", fn);
    return -1;
  }

  return firmware_type_from_magic(magic);
}

// Convert from disk format to memory format
static struct firmware *firmware_header_disk_to_mem(
  int type,
  const struct firmware_header_disk *disk
) {

  struct firmware *firmware = calloc(1, sizeof(struct firmware));
  if (!firmware) {
    perror("Failed to allocate memory for firmware");
    return NULL;
  }

  firmware->type = type;
  firmware->usb_vid = ntohs(disk->usb_vid);
  firmware->usb_pid = ntohs(disk->usb_pid);
  for (int i = 0; i < 4; i++)
    firmware->firmware_version[i] = ntohl(disk->firmware_version[i]);
  firmware->firmware_length = ntohl(disk->firmware_length);
  memcpy(firmware->sha256, disk->sha256, 32);

  return firmware;
}

static struct firmware_container *firmware_container_header_disk_to_mem(
  const struct firmware_container_header_disk *disk
) {
  struct firmware_container *container = calloc(
    1, sizeof(struct firmware_container)
  );
  if (!container) {
    perror("Failed to allocate memory for firmware container");
    return NULL;
  }

  container->usb_vid = ntohs(disk->usb_vid);
  container->usb_pid = ntohs(disk->usb_pid);
  for (int i = 0; i < 4; i++)
    container->firmware_version[i] = ntohl(disk->firmware_version[i]);
  container->num_sections = ntohl(disk->num_sections);

  return container;
}

static struct firmware *read_header(
  FILE       *file,
  const char *fn,
  int         type
) {
  struct firmware_header_disk disk_header;
  size_t read_count = fread(
    &disk_header, sizeof(struct firmware_header_disk), 1, file
  );

  if (read_count != 1) {
    fprintf(stderr, "Error reading firmware header\n");
    goto error;
  }

  struct firmware *firmware = firmware_header_disk_to_mem(type, &disk_header);

  if (!firmware) {
    fprintf(stderr, "Invalid firmware header\n");
    goto error;
  }

  return firmware;

error:
  free(firmware);
  return NULL;
}

static struct firmware *read_header_and_data(
  FILE       *file,
  const char *fn,
  int         type
) {
  struct firmware *firmware = read_header(file, fn, type);
  if (!firmware) {
    fprintf(stderr, "Error reading firmware header from %s\n", fn);
    goto error;
  }

  // Read firmware data
  firmware->firmware_data = malloc(firmware->firmware_length);
  if (!firmware->firmware_data) {
    perror("Failed to allocate memory for firmware data");
    goto error;
  }
  size_t read_count = fread(
    firmware->firmware_data, 1, firmware->firmware_length, file
  );
  if (read_count != firmware->firmware_length) {
    if (feof(file))
      fprintf(stderr, "Unexpected end of file\n");
    else
      perror("Failed to read firmware data");
    fprintf(stderr, "Error reading firmware data from %s\n", fn);
    goto error;
  }

  // Verify the firmware data
  if (!verify_sha256(
    firmware->firmware_data,
    firmware->firmware_length,
    firmware->sha256
  )) {
    fprintf(stderr, "Corrupt firmware (failed checksum) in %s\n", fn);
    goto error;
  }

  // add the MD5 for ESP firmware
  if (firmware->type == FIRMWARE_ESP) {
    md5(
      firmware->firmware_data,
      firmware->firmware_length,
      firmware->md5
    );
  }

  return firmware;

error:
  free(firmware->firmware_data);
  free(firmware);
  return NULL;
}

struct firmware *read_magic_and_header_and_data(
  FILE       *file,
  const char *fn,
  int         section
) {
  int type = read_magic(file, fn);

  if (type < 0 || type == FIRMWARE_CONTAINER) {
    fprintf(
      stderr,
      "Invalid firmware type %d in section %d of %s\n",
      type,
      section + 1,
      fn
    );
    return NULL;
  }

  return read_header_and_data(file, fn, type);
}

struct firmware_container *read_firmware_container_header(
  FILE       *file,
  const char *fn
) {
  struct firmware_container_header_disk disk_header;

  // Read header
  if (fread(
    &disk_header,
    sizeof(struct firmware_container_header_disk),
    1,
    file
  ) != 1) {
    perror("Failed to read container header");
    fprintf(stderr, "Error reading container header from %s\n", fn);
    return NULL;
  }

  return firmware_container_header_disk_to_mem(&disk_header);
}

struct firmware_container *read_firmware_container(
  FILE       *file,
  const char *fn
) {
  struct firmware_container *container = read_firmware_container_header(file, fn);
  if (!container) {
    fprintf(stderr, "Error reading container header from %s\n", fn);
    return NULL;
  }

  if (container->num_sections < 1 || container->num_sections > 3) {
    fprintf(
      stderr,
      "Invalid number of sections in %s: %d\n",
      fn,
      container->num_sections
    );
    goto error;
  }

  // Allocate memory for sections
  container->sections = calloc(
    container->num_sections, sizeof(struct firmware *)
  );
  if (!container->sections) {
    perror("Failed to allocate memory for firmware sections");
    goto error;
  }

  // Read sections
  for (int i = 0; i < container->num_sections; i++) {
    container->sections[i] = read_magic_and_header_and_data(file, fn, i);
    if (!container->sections[i]) {
      fprintf(stderr, "Error reading section %d from %s\n", i + 1, fn);
      goto error;
    }
  }

  return container;

error:
  free_firmware_container(container);
  return NULL;
}

struct firmware_container *read_firmware_header(
  const char *fn
) {
  FILE *file = fopen(fn, "rb");
  if (!file) {
    perror("fopen");
    fprintf(stderr, "Unable to open %s\n", fn);
    return NULL;
  }

  int type = read_magic(file, fn);

  struct firmware_container *container = NULL;

  if (type == FIRMWARE_CONTAINER) {
    container = read_firmware_container_header(file, fn);
    fclose(file);

    return container;
  }

  struct firmware *firmware = read_header(file, fn, type);
  if (!firmware) {
    fprintf(stderr, "Error reading firmware header from %s\n", fn);
    fclose(file);
    return NULL;
  }

  container = calloc(1, sizeof(struct firmware_container));
  if (!container) {
    perror("Failed to allocate memory for firmware container");
    fclose(file);
    return NULL;
  }

  container->num_sections = 1;
  container->sections = calloc(1, sizeof(struct firmware *));
  container->sections[0] = firmware;

  fclose(file);

  return container;
}

struct firmware_container *read_firmware_file(const char *fn) {

  // Open file
  FILE *file = fopen(fn, "rb");
  if (!file) {
    perror("fopen");
    fprintf(stderr, "Unable to open %s\n", fn);
    return NULL;
  }

  // Read magic string
  int type = read_magic(file, fn);

  if (type < 0) {
    fprintf(stderr, "Invalid firmware type\n");
    fclose(file);
    return NULL;
  }

  // Container?
  if (type == FIRMWARE_CONTAINER)
    return read_firmware_container(file, fn);

  // Not a container; read the firmware header and data
  struct firmware *firmware = read_header_and_data(file, fn, type);
  if (!firmware) {
    fprintf(stderr, "Error reading firmware from %s\n", fn);
    fclose(file);
    return NULL;
  }

  fclose(file);

  // Put the firmware header and data into a container
  struct firmware_container *container = calloc(
    1, sizeof(struct firmware_container)
  );
  if (!container) {
    perror("Failed to allocate memory for firmware container");
    return NULL;
  }
  container->num_sections = 1;
  container->sections = calloc(
    1, sizeof(struct firmware *)
  );
  container->sections[0] = firmware;

  return container;
}

void free_firmware_container(
  struct firmware_container *container
) {
  if (!container)
    return;

  if (container->sections) {
    for (int i = 0; i < container->num_sections; i++) {
      struct firmware *firmware = container->sections[i];
      if (!firmware)
        continue;

      free(firmware->firmware_data);
      free(firmware);
    }

    free(container->sections);
  }

  free(container);
}

const char *firmware_type_to_string(enum firmware_type type) {
  switch (type) {
    case FIRMWARE_CONTAINER: return "container";
    case FIRMWARE_APP:       return "App";
    case FIRMWARE_ESP:       return "ESP";
    case FIRMWARE_LEAPFROG:  return "Leapfrog";
    default:                 return "unknown";
  }
}
