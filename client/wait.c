// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include "wait.h"
#include "usb.h"
#include "alsa.h"

// Find device with matching serial number
static struct found_card* find_by_serial(const char *serial, bool quiet) {
  int count;
  struct found_card **cards = enumerate_cards(&count, quiet);
  if (!cards)
    return NULL;

  struct found_card *match = NULL;
  for (int i = 0; i < count; i++) {
    char *card_serial = get_device_serial(cards[i]->card_num);
    if (card_serial && strcmp(card_serial, serial) == 0) {
      // Found match - move to separate variable
      match = cards[i];
      cards[i] = NULL;
      free(card_serial);
      break;
    }
    free(card_serial);
  }

  // Cleanup unmatched cards
  for (int i = 0; i < count; i++) {
    if (cards[i])
      free_found_card(cards[i]);
  }
  free(cards);

  return match;
}

int wait_for_device(struct device_wait *wait, struct found_card **found) {
  long deadline = time(NULL) + wait->timeout;

  while (time(NULL) < deadline) {
    struct found_card *match = find_by_serial(wait->serial, true);
    if (match) {
      *found = match;
      return 0;
    }

    // Wait a bit before retry
    sleep(1);

    printf(".");
  }

  // Try one last time, but print error message if it fails
  struct found_card *match = find_by_serial(wait->serial, false);
  if (match) {
    *found = match;
    return 0;
  }

  return -1; // Timeout
}
