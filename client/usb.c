// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>
#include <errno.h>

// Read the USB serial number via sysfs.
// controlC<N>/device points to the ALSA sound card, whose own
// device symlink points to the USB interface; the parent of
// that is the USB device which has the serial file.
char *get_device_serial(int card_num) {
  char path[128];
  snprintf(
    path, sizeof(path),
    "/sys/class/sound/controlC%d/device/device/../serial",
    card_num
  );

  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(
      stderr, "can't open %s: %s\n", path, strerror(errno)
    );
    return NULL;
  }

  char serial[40];
  int result = fscanf(f, "%39s", serial);
  fclose(f);

  if (result != 1) {
    fprintf(stderr, "can't read %s\n", path);
    return NULL;
  }

  return strdup(serial);
}
