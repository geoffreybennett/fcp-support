// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string.h>
#include <openssl/sha.h>

#include "hash.h"

int verify_sha256(
  const unsigned char *data,
  size_t length,
  const unsigned char *expected_hash
) {
  unsigned char computed_hash[SHA256_DIGEST_LENGTH];
  SHA256(data, length, computed_hash);
  return memcmp(computed_hash, expected_hash, SHA256_DIGEST_LENGTH) == 0;
}
