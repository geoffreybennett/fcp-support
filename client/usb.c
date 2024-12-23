// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

// Get the bus and device numbers from /proc/asound/cardxx/usbbus
static int get_usbbus(int card_num, int *bus, int *dev) {
  char path[256];
  snprintf(path, 256, "/proc/asound/card%d/usbbus", card_num);
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "can't open %s\n", path);
    return 0;
  }

  int result = fscanf(f, "%d/%d", bus, dev);
  fclose(f);

  if (result != 2) {
    fprintf(stderr, "can't read %s\n", path);
    return 0;
  }

  return 1;
}

// Read devnum from a USB device path
static int get_devnum(const char *bus_path) {
  char devnum_path[512];
  snprintf(devnum_path, 512, "%s/devnum", bus_path);

  FILE *f = fopen(devnum_path, "r");
  if (!f) {
    if (errno == ENOENT)
      return -1;
    fprintf(stderr, "can't open %s: %s\n", devnum_path, strerror(errno));
    return -1;
  }

  int devnum;
  int result = fscanf(f, "%d", &devnum);
  int err = errno;
  fclose(f);

  if (result != 1) {
    fprintf(stderr, "can't read %s: %s\n", devnum_path, strerror(err));
    return -1;
  }

  return devnum;
}

// Find USB device path by searching recursively
static int find_device_port(
  const char *bus_path,
  int bus,
  int dev,
  char *port_path,
  size_t port_path_size
) {
  if (get_devnum(bus_path) == dev) {
    snprintf(port_path, port_path_size, "%s", bus_path);
    return 1;
  }

  DIR *dir = opendir(bus_path);
  if (!dir) {
    fprintf(stderr, "can't open %s: %s\n", bus_path, strerror(errno));
    return 0;
  }

  char prefix[20];
  snprintf(prefix, 20, "%d-", bus);

  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (entry->d_type != DT_DIR)
      continue;

    if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
      continue;

    char next_path[512];
    snprintf(next_path, 512, "%s/%s", bus_path, entry->d_name);

    if (find_device_port(next_path, bus, dev, port_path, port_path_size)) {
      closedir(dir);
      return 1;
    }
  }

  closedir(dir);
  return 0;
}

// Get device serial number
char *get_device_serial(int card_num) {
  int bus, dev;
  if (!get_usbbus(card_num, &bus, &dev))
    return NULL;

  char bus_path[80];
  snprintf(bus_path, 80, "/sys/bus/usb/devices/usb%d", bus);

  char port_path[512];
  if (!find_device_port(bus_path, bus, dev, port_path, sizeof(port_path))) {
    fprintf(stderr, "can't find port path in %s for dev %d\n", bus_path, dev);
    return NULL;
  }

  char serial_path[520];
  snprintf(serial_path, 520, "%s/serial", port_path);

  FILE *f = fopen(serial_path, "r");
  if (!f) {
    fprintf(stderr, "can't open %s\n", serial_path);
    return NULL;
  }

  char serial[40];
  int result = fscanf(f, "%39s", serial);
  fclose(f);

  if (result != 1) {
    fprintf(stderr, "can't read %s\n", serial_path);
    return NULL;
  }

  return strdup(serial);
}
