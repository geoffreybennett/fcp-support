// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <alsa/asoundlib.h>

struct found_card {
  int       card_num;
  int       usb_vid;
  int       usb_pid;
  char     *card_name;
  char     *alsa_name;
  char     *socket_path;
  uint32_t  firmware_version[4];
  uint32_t  esp_firmware_version[4];
};

// Returns array of found cards, caller must free
struct found_card **enumerate_cards(int *count, bool quiet);

// Clean up a found_card struct
void free_found_card(struct found_card *card);
