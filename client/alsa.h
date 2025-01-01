// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <alsa/asoundlib.h>

struct sound_card {
  int       card_num;
  int       usb_vid;
  int       usb_pid;
  char     *card_name;
  char     *serial;
  char     *product_name;
  char     *alsa_name;
  char     *socket_path;
  int       socket_fd;
  uint32_t  firmware_version[4];
  uint32_t  esp_firmware_version[4];
};

// Returns array of found cards, caller must free
struct sound_card **enum_cards(int *count, bool quiet);

// Connect to the fcp server for the sound card
int connect_to_server(struct sound_card *card);
int wait_for_disconnect(struct sound_card *card);

// Clean up a found_card struct
void free_sound_card(struct sound_card *card);
