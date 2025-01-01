// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include "wait.h"
#include "usb.h"
#include "alsa.h"

// Find device with matching serial number
static struct sound_card *find_by_serial(const char *serial, bool quiet) {
  int count;
  struct sound_card **cards = enum_cards(&count, quiet);
  if (!cards)
    return NULL;

  struct sound_card *match = NULL;
  for (int i = 0; i < count; i++) {
    char *card_serial = get_device_serial(cards[i]->card_num);

    if (card_serial && strcmp(card_serial, serial) == 0) {
      match = cards[i];
      cards[i] = NULL;
      free(card_serial);
      break;
    }
    free(card_serial);
  }

  // Cleanup unmatched cards
  for (int i = 0; i < count; i++)
    if (cards[i])
      free_sound_card(cards[i]);
  free(cards);

  return match;
}

int wait_for_device(
  const char         *serial,  // Expected serial number
  int                 timeout, // How long to wait in seconds
  struct sound_card **card     // Output: sound card
) {
  long deadline = time(NULL) + timeout;

  while (time(NULL) < deadline) {
    struct sound_card *match = find_by_serial(serial, true);
    if (match) {
      *card = match;
      return 0;
    }

    // Wait a bit before retry
    sleep(1);

    printf(".");
  }

  // Try one last time, but print error message if it fails
  struct sound_card *match = find_by_serial(serial, false);
  if (match) {
    *card = match;
    return 0;
  }

  return -1; // Timeout
}
