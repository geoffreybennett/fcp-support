// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <openssl/sha.h>

int verify_sha256(
  const unsigned char *data,
  size_t               length,
  const unsigned char *expected_hash
);
