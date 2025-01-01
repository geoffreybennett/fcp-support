// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <fcntl.h>

// Include the shared header for protocol constants and structures
#include "../shared/fcp-shared.h"

#include "firmware.h"
#include "alsa.h"
#include "wait.h"
#include "devices.h"

// System-wide firmware directory
#define SYSTEM_FIRMWARE_DIR "/usr/lib/firmware/scarlett4"

#define GITHUB_URL "https://github.com/geoffreybennett"
#define FCP_DRIVER_URL  GITHUB_URL "/linux-fcp"
#define FCP_SUPPORT_URL GITHUB_URL "/fcp-support"
#define ASG_URL         GITHUB_URL "/alsa-scarlett-gui"
#define FIRMWARE_URL    GITHUB_URL "/fcp-firmware"

// List of found cards
int card_count = 0;
struct sound_card **cards = NULL;

// List of found firmware
struct found_firmware {
  const char                *fn;
  struct firmware_container *firmware;
};

int found_firmwares_count = 0;
struct found_firmware *found_firmwares = NULL;

// The name we were called as
const char *program_name;

// Command-line parameters
const char *command = NULL;
int selected_card_num = -1;
struct sound_card *selected_card = NULL;
const char *selected_firmware_file = NULL;
struct firmware_container *selected_firmware = NULL;
char *card_serial = NULL;

static int showing_progress = false;

// Array of supported commands and requirements
struct command {
  const char *name;
  int (*handler)(void);
  bool requires_cards;
  bool requires_card_selection;
  bool requires_firmwares;
  bool requires_firmware_selection;
};

// Firmware Helper Functions

static char *get_fw_version_string(
  const uint32_t *app_ver,
  const uint32_t *esp_ver
) {
  char *app_str;
  if (asprintf(
        &app_str,
        "%d.%d.%d.%d",
        app_ver[0],
        app_ver[1],
        app_ver[2],
        app_ver[3]
      ) < 0) {
    perror("asprintf");
    return NULL;
  }

  if (!esp_ver)
    return app_str;

  char *esp_str;
  if (asprintf(
        &esp_str,
        "%d.%d.%d.%d",
        esp_ver[0],
        esp_ver[1],
        esp_ver[2],
        esp_ver[3]
      ) < 0) {
    perror("asprintf");
    return NULL;
  }

  char *fw_str;
  if (asprintf(
        &fw_str,
        "App %s, ESP %s",
        app_str,
        esp_str
      ) < 0) {
    perror("asprintf");
    return NULL;
  }

  free(app_str);
  free(esp_str);

  return fw_str;
}

static int firmware_cmp(const uint32_t *v1, const uint32_t *v2) {
  for (int i = 0; i < 4; i++) {
    if (v1[i] < v2[i])
      return -1;
    if (v1[i] > v2[i])
      return 1;
  }
  return 0;
}

static void add_found_firmware(
  char *fn,
  struct firmware_container *firmware
) {
  if (firmware->usb_vid != VENDOR_VID)
    return;

  for (int i = 0; i < found_firmwares_count; i++) {
    struct found_firmware *found_firmware = &found_firmwares[i];

    // already have this firmware under a different name?
    if (found_firmware->firmware->usb_vid == firmware->usb_vid &&
        found_firmware->firmware->usb_pid == firmware->usb_pid &&
        found_firmware->firmware->firmware_version ==
          firmware->firmware_version)
      return;
  }

  found_firmwares_count++;
  found_firmwares = realloc(
    found_firmwares,
    sizeof(*found_firmwares) * found_firmwares_count
  );
  if (!found_firmwares) {
    perror("realloc");
    exit(EXIT_FAILURE);
  }

  struct found_firmware *fw = &found_firmwares[found_firmwares_count - 1];
  fw->fn = fn;
  fw->firmware = firmware;
}

static void enum_firmware_dir(const char *dirname) {
  DIR *dir;
  struct dirent *entry;

  if ((dir = opendir(dirname)) == NULL) {
    if (errno == ENOENT) {
      fprintf(stderr, "Firmware directory %s not found\n", dirname);
      fprintf(stderr, "Please install the firmware package from:\n");
      fprintf(stderr, "  %s\n\n", FIRMWARE_URL);
      return;
    }
    fprintf(
      stderr,
      "Unable to open directory %s: %s\n",
      dirname,
      strerror(errno)
    );
    return;
  }

  while ((entry = readdir(dir)) != NULL) {

    // Check if the file is a .bin file
    if (!strstr(entry->d_name, ".bin"))
      continue;

    // Construct full path
    size_t path_len = strlen(dirname) + strlen(entry->d_name) + 2;
    char *full_path = malloc(path_len);

    if (!full_path) {
      perror("malloc");
      return;
    }

    snprintf(full_path, path_len, "%s/%s", dirname, entry->d_name);

    // Parse the firmware file
    struct firmware_container *firmware = read_firmware_header(full_path);
    if (firmware) {
      add_found_firmware(full_path, firmware);
    } else {
      fprintf(stderr, "Failed to read firmware file: %s\n", full_path);
      free(full_path);
    }
  }

  closedir(dir);
}

static int found_firmware_cmp(const void *p1, const void *p2) {
  const struct found_firmware *ff1 = p1;
  const struct found_firmware *ff2 = p2;
  struct firmware_container *f1 = ff1->firmware;
  struct firmware_container *f2 = ff2->firmware;
  struct supported_device *d1 = get_supported_device_by_pid(f1->usb_pid);
  struct supported_device *d2 = get_supported_device_by_pid(f2->usb_pid);

  // compare location in supported_devices array
  if (d1 < d2)
    return -1;
  if (d1 > d2)
    return 1;

  // compare firmware version, newest first
  if (f1->firmware_version < f2->firmware_version)
    return 1;
  if (f1->firmware_version > f2->firmware_version)
    return -1;

  return 0;
}

static void enum_firmwares(void) {

  /* look for firmware files in the system firmware directory */
  enum_firmware_dir(SYSTEM_FIRMWARE_DIR);

  qsort(
    found_firmwares,
    found_firmwares_count,
    sizeof(*found_firmwares),
    found_firmware_cmp
  );
}

static struct found_firmware *get_latest_firmware(int pid) {
  for (int i = 0; i < found_firmwares_count; i++) {
    struct found_firmware *found_firmware = &found_firmwares[i];

    // found_firmwares is sorted by version descending so first is
    // latest
    if (found_firmware->firmware->usb_pid == pid)
      return found_firmware;
  }

  return NULL;
}

// Server Communication Helpers

// Helper function to read exact number of bytes
static ssize_t read_exact(int fd, void *buf, size_t count) {
  size_t total = 0;
  while (total < count) {
    ssize_t n = read(fd, (char*)buf + total, count - total);
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        continue;
      }
      return n; // Error or EOF
    }
    total += n;
  }
  return total;
}

static void show_progress(int percent) {
  char progress[51] = {0};
  int filled = percent / 2;
  int half = percent % 2;

  showing_progress = true;

  for (int i = 0; i < 50; i++) {
    progress[i] = i < filled ? '#' : i == filled ? half ? '>' : '-' : '.';
  }

  printf("\r[%s] %3d%%", progress, percent);
}

static void handle_progress_message(const void *payload, size_t length) {
  if (length != sizeof(uint8_t)) {
    fprintf(stderr, "Invalid progress message size\n");
    return;
  }

  uint8_t percent = *(const uint8_t*)payload;

  show_progress(percent);
}

static void handle_error_message(const void *payload, size_t length) {
  if (length != sizeof(int16_t)) {
    fprintf(stderr, "\nInvalid error message size\n");
    return;
  }

  int16_t error_code = *(const int16_t*)payload;

  if (error_code < 0 || error_code > FCP_SOCKET_ERR_MAX) {
    fprintf(stderr, "\nInvalid error code: %d\n", error_code);
    return;
  }

  fprintf(stderr, "\nError: %s\n", fcp_socket_error_messages[error_code]);
}

static void handle_success_message(void) {
  if (showing_progress) {
    show_progress(100);
    printf("\n");
    showing_progress = false;
  } else {
    printf("Done!\n");
  }
}

static int handle_server_message(int sock_fd, bool quiet) {
  struct fcp_socket_msg_header header;
  ssize_t n = read_exact(sock_fd, &header, sizeof(header));

  if (n <= 0) {
    if (n < 0) {
      perror("Error reading response header");
    }
    return n;
  }

  if (header.magic != FCP_SOCKET_MAGIC_RESPONSE) {
    fprintf(stderr, "Invalid response magic: 0x%02x\n", header.magic);
    return -1;
  }

  // Read payload if present
  void *payload = NULL;
  if (header.payload_length > 0) {
    payload = malloc(header.payload_length);
    if (!payload) {
      fprintf(stderr, "Failed to allocate payload buffer\n");
      return -1;
    }

    n = read_exact(sock_fd, payload, header.payload_length);
    if (n <= 0) {
      free(payload);
      if (n < 0) {
        perror("Error reading payload");
      }
      return n;
    }
  }

  int result;

  // Handle message based on type
  switch (header.msg_type) {
    case FCP_SOCKET_RESPONSE_PROGRESS:
      handle_progress_message(payload, header.payload_length);
      result = 1;
      break;

    case FCP_SOCKET_RESPONSE_ERROR:
      handle_error_message(payload, header.payload_length);
      result = -1;
      break;

    case FCP_SOCKET_RESPONSE_SUCCESS:
      if (!quiet)
        handle_success_message();
      result = 0;
      break;

    default:
      fprintf(stderr, "Unknown response type: %d\n", header.msg_type);
      result = -1;
  }

  free(payload);
  return result;
}

static int handle_server_responses(int sock_fd, bool quiet) {
  fd_set rfds;
  struct timeval tv, last_progress, now;
  const int TIMEOUT_SECS = 15;

  gettimeofday(&last_progress, NULL);

  while (1) {
    FD_ZERO(&rfds);
    FD_SET(sock_fd, &rfds);

    gettimeofday(&now, NULL);
    int elapsed = now.tv_sec - last_progress.tv_sec;
    if (elapsed >= TIMEOUT_SECS) {
      fprintf(stderr, "Operation timed out\n");
      return -1;
    }

    tv.tv_sec = TIMEOUT_SECS - elapsed;
    tv.tv_usec = 0;

    int ret = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      return -1;
    }
    if (ret > 0) {
      ret = handle_server_message(sock_fd, quiet);

      // Success or error
      if (ret <= 0)
        return ret;

      // Progress message
      gettimeofday(&last_progress, NULL);
    }
  }
}

static int send_simple_command(uint8_t command, int quiet) {
  int sock_fd = selected_card->socket_fd;

  struct fcp_socket_msg_header header = {
    .magic = FCP_SOCKET_MAGIC_REQUEST,
    .msg_type = command,
    .payload_length = 0
  };

  if (write(sock_fd, &header, sizeof(header)) != sizeof(header)) {
    perror("Error sending command");
    return -1;
  }

  return handle_server_responses(sock_fd, quiet);
}

static int send_firmware(struct firmware *fw) {
  int sock_fd = selected_card->socket_fd;
  int command;

  if (fw->type == FIRMWARE_LEAPFROG ||
      fw->type == FIRMWARE_APP) {
    command = FCP_SOCKET_REQUEST_APP_FIRMWARE_UPDATE;
  } else if (fw->type == FIRMWARE_ESP) {
    command = FCP_SOCKET_REQUEST_ESP_FIRMWARE_UPDATE;
  } else {
    fprintf(stderr, "Invalid firmware type\n");
    exit(1);
  }

  // Prepare and send header
  struct fcp_socket_msg_header header = {
    .magic          = FCP_SOCKET_MAGIC_REQUEST,
    .msg_type       = command,
    .payload_length = sizeof(struct firmware_payload) + fw->firmware_length
  };

  if (write(sock_fd, &header, sizeof(header)) != sizeof(header)) {
    perror("Error sending header");
    return -1;
  }

  // Send firmware payload
  struct firmware_payload payload = {
    .size    = fw->firmware_length,
    .usb_vid = fw->usb_vid,
    .usb_pid = fw->usb_pid
  };
  memcpy(payload.sha256, fw->sha256, SHA256_DIGEST_LENGTH);
  memcpy(payload.md5, fw->md5, MD5_DIGEST_LENGTH);

  if (write(sock_fd, &payload, sizeof(payload)) != sizeof(payload)) {
    perror("Error sending payload header");
    return -1;
  }

  // Send firmware data
  int result = write(sock_fd, fw->firmware_data, fw->firmware_length);
  if (result != fw->firmware_length) {
    perror("Error sending firmware data");
    return -1;
  }

  return handle_server_responses(sock_fd, false);
}

static struct firmware* find_firmware_by_type(enum firmware_type type) {
  for (int i = 0; i < selected_firmware->num_sections; i++)
    if (selected_firmware->sections[i]->type == type)
      return selected_firmware->sections[i];

  return NULL;
}

static int send_firmware_of_type(enum firmware_type required_type) {
  struct firmware *fw = find_firmware_by_type(required_type);

  if (!fw) {
    fprintf(stderr, "No matching firmware type found\n");
    return -1;
  }

  return send_firmware(fw);
}

// Device Helpers

static void check_card_selection(void) {
  if (!card_count) {
    fprintf(stderr, "No supported devices found\n");
    exit(EXIT_FAILURE);
  }
  if (selected_card_num == -1) {
    if (card_count > 1) {
      fprintf(stderr, "Error: more than one supported device found\n");
      fprintf(
        stderr,
        "Use '%s list' and '%s -c <card_num> ...' to select a device\n",
        program_name,
        program_name
      );
      exit(EXIT_FAILURE);
    }
    selected_card_num = cards[0]->card_num;
  }

  for (int i = 0; i < card_count; i++) {
    if (cards[i]->card_num == selected_card_num) {
      selected_card = cards[i];
      break;
    }
  }

  if (!selected_card) {
    fprintf(stderr, "Error: selected card %d not found\n", selected_card_num);
    fprintf(
      stderr,
      "Use '%s list' to list supported devices\n",
      program_name
    );
    exit(EXIT_FAILURE);
  }

  printf(
    "Selected device %s (%s)\n",
    selected_card->product_name,
    selected_card->serial
  );

  if (!selected_card->socket_path) {
    fprintf(
      stderr,
      "fcp-server not running for card %d\n",
      selected_card->card_num
    );
    exit(EXIT_FAILURE);
  }

  int err = connect_to_server(selected_card);

  if (err < 0) {
    fprintf(
      stderr,
      "Failed to connect to fcp-server for card %d\n",
      selected_card->card_num
    );
    exit(EXIT_FAILURE);
  }
}

static void check_firmware_selection(void) {

  struct found_firmware *ff;

  // no firmware version specified, use latest
  if (!selected_firmware_file) {
    ff = get_latest_firmware(selected_card->usb_pid);

    if (!ff) {
      fprintf(
        stderr,
        "No firmware available for %s\n",
        selected_card->product_name
      );
      exit(EXIT_FAILURE);
    }

    // check if latest firmware is newer
    if (firmware_cmp(selected_card->firmware_version,
                     ff->firmware->firmware_version) >= 0) {
      fprintf(
        stderr,
        "Firmware %d.%d.%d.%d for %s is already up to date\n",
        selected_card->firmware_version[0],
        selected_card->firmware_version[1],
        selected_card->firmware_version[2],
        selected_card->firmware_version[3],
        selected_card->product_name
      );
      exit(EXIT_FAILURE);
    }

  // firmware version specified, check if it's available
  } else {
    ff = calloc(1, sizeof(*ff));
    ff->fn = selected_firmware_file;
    ff->firmware = read_firmware_header(selected_firmware_file);
    if (!ff->firmware) {
      fprintf(stderr, "Failed to read firmware file: %s\n", ff->fn);
      exit(EXIT_FAILURE);
    }
  }

  // read the firmware file
  selected_firmware = read_firmware_file(ff->fn);

  if (!selected_firmware) {
    fprintf(stderr, "Unable to load firmware\n");
    exit(EXIT_FAILURE);
  }

  // double-check the PID
  if (selected_firmware->usb_pid != selected_card->usb_pid) {
    fprintf(
      stderr,
      "Firmware file is for a different device (PID %04x != %04x)\n",
      selected_firmware->usb_pid,
      selected_card->usb_pid
    );
    exit(EXIT_FAILURE);
  }

  // display the firmware version and filename
  printf(
    "Found firmware version %d.%d.%d.%d for %s\n"
    "  %s\n",
    selected_firmware->firmware_version[0],
    selected_firmware->firmware_version[1],
    selected_firmware->firmware_version[2],
    selected_firmware->firmware_version[3],
    selected_card->product_name,
    ff->fn
  );
}

static int is_connected(int pid) {
  for (int i = 0; i < card_count; i++)
    if (cards[i]->usb_pid == pid)
      return 1;

  return 0;
}

static int reboot_and_wait(void) {
  char *card_serial = strdup(selected_card->serial);
  int err;

  err = send_simple_command(FCP_SOCKET_REQUEST_REBOOT, true);

  if (err != 0)
    return err;

  printf("Rebooting");

  err = wait_for_disconnect(selected_card);
  if (err != 0) {
    fprintf(stderr, "fcp-server did not disconnect after reboot request\n");
    return -1;
  }

  close(selected_card->socket_fd);
  free_sound_card(selected_card);
  selected_card = NULL;

  if (wait_for_device(card_serial, 20, &selected_card) != 0) {
    printf("\n");
    fprintf(stderr, "Device did not reappear after reboot\n");
    return -1;
  }

  printf("\n");

  err = connect_to_server(selected_card);

  if (err < 0)
    return -1;

  return 0;
}

// Command Handlers

static int usage(void) {
  printf(
    "FCP Tool Version %s\n"
    "\n"
    "Usage: %s [options] [command]\n"
    "\n"
    "Commonly-used commands:\n"
    "  -h, help              Display this information\n"
    "  -l, list              List currently connected devices and\n"
    "                        if a firmware update is available\n"
    "  -u, update            Update firmware on the device\n"
    "  about                 Display more information\n"
    "\n"
    "Lesser-used commands:\n"
    "  list-all              List all supported products\n"
    "                        and available firmware versions\n"
    "  reboot                Reboot the device\n"
    "  erase-config          Reset to default configuration\n"
    "  erase-app             Erase the App firmware\n"
    "  upload-leapfrog       Upload Leapfrog firmware\n"
    "  upload-esp            Upload ESP firmware\n"
    "  upload-app            Upload App firmware\n"
    "\n"
    "Lesser-used options:\n"
    "  -c, --card <num>      Select a specific card number\n"
    "  -f, --firmware <file> Specify a firmware file\n"
    "\n"
    "Support: %s\n"
    "Configuration GUI: %s\n"
    "Firmware: %s\n"
    "\n",
    VERSION,
    program_name,
    FCP_SUPPORT_URL,
    ASG_URL,
    FIRMWARE_URL
  );

  return EXIT_SUCCESS;
}

static int about(void) {
  printf(
    "FCP Tool Version %s\n"
    "\n"
    "ABOUT\n"
    "-----\n"
    "\n"
    "The FCP Tool provides firmware management for Focusrite(R) USB audio\n"
    "interfaces using the Linux FCP driver.\n"
    "\n"
    "REQUIREMENTS\n"
    "------------\n"
    "\n"
    "Requires Linux kernel 6.TBA or later, or a backported version of the\n"
    "FCP USB protocol driver from\n"
    "  %s\n"
    "\n"
    "Requires device firmware to be placed in:\n"
    "  %s\n"
    "\n"
    "Obtain firmware from:\n"
    "  %s\n"
    "\n"
    "COPYRIGHT AND LEGAL INFORMATION\n"
    "-------------------------------\n"
    "\n"
    "Copyright 2024 Geoffrey D. Bennett <g@b4.vu>\n"
    "License: GPL-3.0-or-later\n"
    "\n"
    "This program is free software: you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation, either version 3 of the License, or (at\n"
    "your option) any later version.\n"
    "\n"
    "This program is distributed in the hope that it will be useful, but\n"
    "WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU\n"
    "General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with this program. If not, see https://www.gnu.org/licenses/\n"
    "\n"
    "Focusrite, Scarlett, Clarett, and Vocaster are trademarks or\n"
    "registered trademarks of Focusrite Audio Engineering Limited in\n"
    "England, USA, and/or other countries. Use of these trademarks does not\n"
    "imply any affiliation or endorsement of this software.\n"
    "\n"
    "SUPPORT AND ADDITIONAL SOFTWARE\n"
    "-------------------------------\n"
    "\n"
    "For support, please open an issue on GitHub:\n"
    "  %s\n"
    "\n"
    "GUI control panel available at:\n"
    "  %s\n"
    "\n"
    "CONTACT\n"
    "-------\n"
    "\n"
    "- Author: Geoffrey D. Bennett\n"
    "- Email: g@b4.vu\n"
    "- GitHub: %s\n"
    "\n"
    "DONATIONS\n"
    "---------\n"
    "\n"
    "This software, including the driver, tools, and GUI is Free Software\n"
    "that I’ve independently developed using my own resources. It\n"
    "represents hundreds of hours of development work.\n"
    "\n"
    "If you find this software valuable, please consider making a donation.\n"
    "Your show of appreciation, more than the amount itself, motivates me\n"
    "to continue improving these tools.\n"
    "\n"
    "You can donate via:\n"
    "\n"
    "- LiberaPay: https://liberapay.com/gdb\n"
    "- PayPal: https://paypal.me/gdbau\n"
    "- Zelle: g@b4.vu\n"
    "\n",
    VERSION,
    FCP_DRIVER_URL,
    SYSTEM_FIRMWARE_DIR,
    FIRMWARE_URL,
    FCP_SUPPORT_URL,
    ASG_URL,
    GITHUB_URL
  );

  return EXIT_SUCCESS;
}

static int reboot(void) {
  printf("Rebooting...");
  return send_simple_command(FCP_SOCKET_REQUEST_REBOOT, false);
}

static int erase_config(void) {
  printf("Erasing configuration...\n");
  return send_simple_command(FCP_SOCKET_REQUEST_CONFIG_ERASE, false);
}

static int erase_app(void) {
  printf("Erasing App firmware...\n");
  return send_simple_command(FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE, false);
}

static int erase_and_upload(enum firmware_type type) {
  if (type != FIRMWARE_ESP) {
    printf("Erasing App firmware...\n");
    int result = send_simple_command(
      FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE,
      false
    );
    if (result != 0)
      return result;
  }

  printf("Uploading %s firmware...\n", firmware_type_to_string(type));
  return send_firmware_of_type(type);
}

static int upload_leapfrog(void) {
  return erase_and_upload(FIRMWARE_LEAPFROG);
}

static int upload_esp(void) {
  printf("Uploading ESP firmware...\n");
  return send_firmware_of_type(FIRMWARE_ESP);
}

static int upload_app(void) {
  return erase_and_upload(FIRMWARE_APP);
}

static int list_cards(void) {
  if (!card_count) {
    fprintf(stderr, "No supported devices found\n");
    return EXIT_FAILURE;
  }

  printf(
    "Found %d supported device%s\n\n",
    card_count,
    card_count == 1 ? "" : "s"
  );
  for (int i = 0; i < card_count; i++) {
    struct sound_card *card = cards[i];

    char *fw_str = get_fw_version_string(
      card->firmware_version,
      card->esp_firmware_version
    );

    printf(
      "ALSA Card %d:\n"
      "  USB ID: %04x:%04x\n"
      "  Product: %s\n"
      "  Serial: %s\n"
      "  Firmware: %s\n",
      card->card_num,
      card->usb_vid,
      card->usb_pid,
      card->product_name,
      card->serial,
      fw_str
    );

    struct found_firmware *ff = get_latest_firmware(card->usb_pid);

    char *latest_fw_str = NULL;

    if (ff) {
      latest_fw_str = get_fw_version_string(
        ff->firmware->firmware_version,
        NULL
      );

      int cmp = firmware_cmp(
        card->firmware_version,
        ff->firmware->firmware_version
      );

      printf(
        "  (%s: %s)\n",
        cmp < 0 ? "update available" : cmp == 0 ? "up to date" : "newer than",
        latest_fw_str
      );
    } else {
      printf("  (no update firmware available)\n");
    }
    printf("\n");

    free(fw_str);
    free(latest_fw_str);
  }

  return EXIT_SUCCESS;
}

static void print_fw_version(const uint32_t *version) {
  printf(
    "%d.%d.%d.%d",
    version[0],
    version[1],
    version[2],
    version[3]
  );
}

// List all supported devices and available firmware versions
static int list_all(void) {
  if (!found_firmwares_count) {
    printf("No firmware found.\n\n");
    printf(
      "Firmware files should be placed in:\n"
      "  %s\n\n"
      "Obtain firmware from:\n"
      "  https://github.com/geoffreybennett/fcp-firmware\n",
      "\n"
      SYSTEM_FIRMWARE_DIR
    );
  }

  printf(
    "USB Product ID, Product Name, and Firmware versions available "
    "(* = connected)\n"
  );

  struct supported_device *dev;

  for (dev = supported_devices; dev->pid; dev++) {
    printf(
      "%c%04x %-25s ",
      is_connected(dev->pid) ? '*' : ' ',
      dev->pid,
      dev->name
    );

    int first = 1;

    for (int i = 0; i < found_firmwares_count; i++) {
      struct found_firmware *fw = &found_firmwares[i];

      if (fw->firmware->usb_pid != dev->pid)
        continue;

      if (!first)
        printf(", ");

      print_fw_version(fw->firmware->firmware_version);

      first = 0;
    }

    if (is_connected(dev->pid)) {
      printf(" (running: ");

      int first = 1;
      for (int i = 0; i < card_count; i++) {
        struct sound_card *card = cards[i];

        if (card->usb_pid != dev->pid)
          continue;

        if (!first)
          printf(", ");

        print_fw_version(card->firmware_version);

        first = 0;
      }

      printf(")");
    }

    printf("\n");
  }

  return EXIT_SUCCESS;
}

static int update(void) {
  bool need_leapfrog = false;
  bool need_esp = false;

  /* Check if ESP is up to date */
  struct firmware *esp_fw = find_firmware_by_type(FIRMWARE_ESP);
  if (esp_fw) {
    if (memcmp(selected_card->esp_firmware_version,
               esp_fw->firmware_version,
               sizeof(selected_card->esp_firmware_version)) != 0)
      need_esp = true;
  }

  /* If ESP isn't up to date, check if Leapfrog is already loaded */
  if (need_esp) {
    struct firmware *leapfrog_fw = find_firmware_by_type(FIRMWARE_LEAPFROG);
    if (leapfrog_fw) {
      if (memcmp(selected_card->firmware_version,
                 leapfrog_fw->firmware_version,
                 sizeof(selected_card->firmware_version)) != 0)
        need_leapfrog = true;
    }
  }

  for (int i = 0; i < selected_firmware->num_sections; i++) {
    struct firmware *fw = selected_firmware->sections[i];

    if (fw->type == FIRMWARE_LEAPFROG && !need_leapfrog)
      continue;

    if (fw->type == FIRMWARE_ESP && !need_esp)
      continue;

    int result = erase_and_upload(fw->type);
    if (result != 0)
      return result;

    if (fw->type != FIRMWARE_ESP) {
      result = reboot_and_wait();
      if (result != 0)
        return result;
    }
  }

  return 0;
}

// Main Helper Functions

static void short_help(void) {
  fprintf(stderr, "Use '%s help' for help\n", program_name);
  exit(EXIT_FAILURE);
}

static void parse_args(int argc, char *argv[]) {
  program_name = argv[0];

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    // -c, --card
    if (!strncmp(arg, "-c", 2) ||
        !strcmp(arg, "--card") ||
        !strncmp(arg, "--card=", 7)) {

      const char *card_num_str;

      // support -cN
      if (strncmp(arg, "-c", 2) == 0 && strlen(arg) > 2) {
        card_num_str = arg + 2;

      // and --card=N
      } else if (strncmp(arg, "--card=", 7) == 0) {
        card_num_str = arg + 7;

      // and -c N, --card N
      } else {
        if (i + 1 >= argc) {
          fprintf(
            stderr, "Missing argument for %s (requires a card number)\n", arg
          );
          short_help();
        }

        card_num_str = argv[++i];
      }

      if (selected_card_num != -1) {
        fprintf(stderr, "Error: multiple cards specified\n");
        short_help();
      }

      // Parse card number
      char *endptr;
      errno = 0;
      long card_num = strtol(card_num_str, &endptr, 10);
      if (errno || *endptr || card_num < 0) {
        fprintf(stderr, "Invalid card number: %s\n", card_num_str);
        short_help();
      }

      selected_card_num = card_num;

    // --firmware
    } else if (!strncmp(arg, "-f", 2) ||
               !strcmp(arg, "--firmware") ||
               !strncmp(arg, "--firmware=", 11)) {

      const char *firmware_file;

      // support -f file
      if (strncmp(arg, "-f", 2) == 0 && strlen(arg) > 2) {
        firmware_file = arg + 2;

      // and --firmware=file
      } else if (strncmp(arg, "--firmware=", 11) == 0) {
        firmware_file = arg + 11;

      // and --firmware file
      } else {
        if (i + 1 >= argc) {
          fprintf(
            stderr, "Missing argument for %s (requires a firmware file)\n", arg
          );
          short_help();
        }

        firmware_file = argv[++i];
      }

      if (selected_firmware_file) {
        fprintf(stderr, "Error: multiple firmware files specified\n");
        short_help();
      }

      selected_firmware_file = firmware_file;

    // short-form commands
    } else if (arg[0] == '-') {
      char *short_command = NULL;

      if (strlen(arg) == 2)
        switch (arg[1]) {
          case 'h': short_command = "help"; break;
          case 'l': short_command = "list"; break;
          case 'u': short_command = "update"; break;
        }

      if (!short_command) {
        fprintf(stderr, "Unknown option: %s\n", arg);
        short_help();
      }

      if (command) {
        fprintf(stderr, "Error: multiple commands specified\n");
        short_help();
      }

      command = short_command;

    // commands
    } else if (!command) {
      command = arg;

    } else {
      fprintf(stderr, "Error: multiple commands specified\n");
      short_help();
    }
  }

  // check if a card was specified but no command
  if (!command && selected_card_num != -1) {
    fprintf(stderr, "Error: card specified but no command\n");
    short_help();
  }
}

// Main

static struct command commands[] = {
  { "help",            usage,           false, false, false, false },
  { "about",           about,           false, false, false, false },
  { "reboot",          reboot,          true,  true,  false, false },
  { "erase-config",    erase_config,    true,  true,  false, false },
  { "erase-app",       erase_app,       true,  true,  false, false },
  { "upload-leapfrog", upload_leapfrog, true,  true,  true,  true  },
  { "upload-esp",      upload_esp,      true,  true,  true,  true  },
  { "upload-app",      upload_app,      true,  true,  true,  true  },
  { "list",            list_cards,      true,  false, true,  false },
  { "list-all",        list_all,        true,  false, true,  false },
  { "update",          update,          true,  true,  true,  true  },
  { 0 }
};

static struct command *find_command(const char *name) {
  struct command *command;

  for (command = commands; command->name; command++)
    if (!strcmp(command->name, name))
      return command;

  return NULL;
}

int main(int argc, char *argv[]) {
  setvbuf(stdout, NULL, _IONBF, 0);

  parse_args(argc, argv);

  if (!command)
    command = "list";

  struct command *cmd = find_command(command);

  if (!cmd) {
    fprintf(stderr, "Unknown command: %s\n", command);
    short_help();
  }

  if (cmd->requires_cards)
    cards = enum_cards(&card_count, false);

  if (cmd->requires_card_selection)
    check_card_selection();

  if (cmd->requires_firmwares)
    enum_firmwares();

  if (cmd->requires_firmware_selection)
    check_firmware_selection();

  return !!cmd->handler();
}
