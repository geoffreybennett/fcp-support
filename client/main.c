// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
#include <fcntl.h>

// Include the shared header for protocol constants and structures
#include "../shared/fcp-shared.h"

#include "firmware.h"
#include "usb.h"
#include "alsa.h"
#include "wait.h"

static int showing_progress = false;

struct command_context {
  struct found_card         *card;
  const char                *serial;
  struct firmware_container *container;
  int                        sock_fd;
};

static void print_usage(const char *prog) {
  fprintf(stderr, "fcp-firmware %s\n", VERSION);
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage: %s <command> [args...]\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Commonly-used commands:\n");
  fprintf(stderr, "  update <firmware-file>          Update firmware\n");
  fprintf(stderr, "  erase-config                    Erase configuration\n");
  fprintf(stderr, "  reboot                          Reboot device\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Advanced commands:\n");
  fprintf(stderr, "  upload-leapfrog <firmware-file> Upload Leapfrog firmware\n");
  fprintf(stderr, "  upload-esp <firmware-file>      Upload ESP firmware\n");
  fprintf(stderr, "  upload-app <firmware-file>      Upload App firmware\n");
  fprintf(stderr, "  erase-app                       Erase App firmware\n");
  fprintf(stderr, "  reboot                          Reboot device\n");
}

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

static int send_simple_command(int sock_fd, uint8_t command, int quiet) {
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

static struct firmware* find_firmware_by_type(
  struct firmware_container *container,
  enum firmware_type         type
) {
  for (int i = 0; i < container->num_sections; i++) {
    if (container->sections[i]->type == type) {
      return container->sections[i];
    }
  }
  return NULL;
}

static int send_firmware(
  int              sock_fd,
  struct firmware *fw
) {
  if (!fw) {
    fprintf(stderr, "No matching firmware type found\n");
    return -1;
  }

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

static int send_firmware_of_type(
  int                        sock_fd,
  struct firmware_container *container,
  enum firmware_type         required_type
) {
  struct firmware *fw = find_firmware_by_type(container, required_type);
  return send_firmware(sock_fd, fw);
}

static int connect_to_server(const char *socket_path) {
  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("Cannot create socket");
    return -1;
  }

  struct sockaddr_un addr = {
    .sun_family = AF_UNIX
  };
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    char *s;
    if (asprintf(&s, "Cannot connect to server at %s", addr.sun_path) < 0)
      perror("Cannot connect to server");
    else
      perror(s);
    return -1;
  }

  return sock_fd;
}

// Wait for server to disconnect after sending a reboot command
// (should happen in <1ms)
static int wait_for_disconnect(int sock_fd) {
  fd_set rfds;
  struct timeval tv, start_time, now;
  const int TIMEOUT_SECS = 1;
  char buf[1];

  gettimeofday(&start_time, NULL);

  while (1) {
    FD_ZERO(&rfds);
    FD_SET(sock_fd, &rfds);

    gettimeofday(&now, NULL);
    int elapsed = now.tv_sec - start_time.tv_sec;
    if (elapsed >= TIMEOUT_SECS) {
      fprintf(stderr, "Timeout waiting for server disconnect\n");
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
      // Try to read one byte
      ssize_t n = read(sock_fd, buf, 1);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        perror("read");
        return -1;
      }
      if (n == 0) {
        // EOF received - server has disconnected
        return 0;
      }
      // Ignore any data received, just keep waiting for EOF
    }
  }
}

static int reboot_and_wait(struct command_context *ctx) {
  int result = send_simple_command(ctx->sock_fd, FCP_SOCKET_REQUEST_REBOOT, true);

  if (result != 0)
    return result;

  printf("Rebooting");

  int err = wait_for_disconnect(ctx->sock_fd);
  if (err != 0) {
    fprintf(stderr, "fcp-server did not disconnect after reboot request\n");
    return -1;
  }

  struct device_wait wait = {
    .serial  = ctx->serial,
    .timeout = 20
  };

  struct found_card *found;
  if (wait_for_device(&wait, &found) != 0) {
    printf("\n");
    fprintf(stderr, "Device did not reappear after reboot\n");
    return -1;
  }

  close(ctx->sock_fd);
  ctx->sock_fd = connect_to_server(found->socket_path);
  free_found_card(found);

  if (ctx->sock_fd < 0)
    return -1;

  printf("\n");
  return 0;
}

static int perform_update(struct command_context *ctx) {
  struct firmware_container *container = ctx->container;
  struct found_card *card = ctx->card;
  bool need_leapfrog = false;
  bool need_esp = false;

  /* Check if ESP is up to date */
  struct firmware *esp_fw = find_firmware_by_type(container, FIRMWARE_ESP);
  if (esp_fw) {
    if (memcmp(card->esp_firmware_version,
               esp_fw->firmware_version,
               sizeof(card->esp_firmware_version)) != 0)
      need_esp = true;
  }

  /* If ESP isn't up to date, check if Leapfrog is already loaded */
  if (need_esp) {
    struct firmware *leapfrog_fw = find_firmware_by_type(container, FIRMWARE_LEAPFROG);
    if (leapfrog_fw) {
      if (memcmp(card->firmware_version,
                 leapfrog_fw->firmware_version,
                 sizeof(card->firmware_version)) != 0)
        need_leapfrog = true;
    }
  }

  for (int i = 0; i < container->num_sections; i++) {
    struct firmware *fw = container->sections[i];

    if (fw->type == FIRMWARE_LEAPFROG && !need_leapfrog)
      continue;

    if (fw->type == FIRMWARE_ESP && !need_esp)
      continue;

    if (fw->type == FIRMWARE_LEAPFROG ||
        fw->type == FIRMWARE_APP) {
      printf("Erasing App firmware...\n");
      int result = send_simple_command(
        ctx->sock_fd,
        FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE,
        false
      );
      if (result != 0)
        return result;
    }

    const char *type_str = firmware_type_to_string(fw->type);
    printf("Uploading %s firmware...\n", type_str);

    int result = send_firmware(ctx->sock_fd, fw);
    if (result != 0)
      return result;

    if (fw->type == FIRMWARE_LEAPFROG ||
        fw->type == FIRMWARE_APP) {
      result = reboot_and_wait(ctx);
      if (result != 0)
        return result;
    }
  }

  return 0;
}

static int execute_command(struct command_context *ctx, const char *cmd) {
  int result;
  int sock_fd = ctx->sock_fd;

  if (strcmp(cmd, "update") == 0) {
    printf("Updating firmware...\n");
    result = perform_update(ctx);
  } else if (strcmp(cmd, "upload-leapfrog") == 0) {
    printf("Erasing App firmware...\n");
    result = send_simple_command(
      sock_fd,
      FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE,
      false
    );
    if (result != 0)
      return result;

    printf("Uploading Leapfrog firmware...\n");
    result = send_firmware_of_type(sock_fd, ctx->container, FIRMWARE_LEAPFROG);

  } else if (strcmp(cmd, "upload-app") == 0) {
    printf("Erasing App firmware...\n");
    result = send_simple_command(
      sock_fd,
      FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE,
      false
    );
    if (result != 0)
      return result;

    printf("Uploading App firmware...\n");
    result = send_firmware_of_type(sock_fd, ctx->container, FIRMWARE_APP);

  } else if (strcmp(cmd, "upload-esp") == 0) {
    printf("Uploading ESP firmware...\n");
    result = send_firmware_of_type(sock_fd, ctx->container, FIRMWARE_ESP);

  } else if (strcmp(cmd, "erase-app") == 0) {
    printf("Erasing App firmware...\n");
    result = send_simple_command(
      sock_fd, FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE, false
    );
  } else if (strcmp(cmd, "erase-config") == 0) {
    printf("Erasing configuration...\n");
    result = send_simple_command(
      sock_fd, FCP_SOCKET_REQUEST_CONFIG_ERASE, false
    );
  } else if (strcmp(cmd, "reboot") == 0) {
    printf("Rebooting...");
    result = send_simple_command(
      sock_fd, FCP_SOCKET_REQUEST_REBOOT, false
    );
  } else {
    fprintf(stderr, "Unknown command: %s\n", cmd);
    result = -1;
  }

  return result;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  setvbuf(stdout, NULL, _IONBF, 0);

  int card_count;
  struct found_card **cards = enumerate_cards(&card_count, false);

  if (!cards || card_count == 0) {
    fprintf(stderr, "No supported devices found\n");
    return 1;
  }

  if (card_count > 1) {
    fprintf(stderr, "Multiple supported devices found\n");
    return 1;
  }

  struct found_card *card = cards[0];

  if (!card->socket_path) {
    fprintf(stderr, "fcp-server not running for card %d\n", card->card_num);
    return 1;
  }

  const char *socket_path = strdup(card->socket_path);
  const char *serial = get_device_serial(card->card_num);

  if (!serial) {
    fprintf(stderr, "Failed to get device serial number\n");
    return 1;
  }

  struct command_context ctx = {
    .card   = card,
    .serial = serial
  };

  // Display device serial number
  printf("Serial number: %s\n", serial);

//  free_found_card(cards[0]);
//  free(cards);

  int sock_fd = connect_to_server(socket_path);
  if (sock_fd < 0)
    return 1;

  ctx.sock_fd = sock_fd;

  // Parse command
  const char *cmd = argv[1];
  int result = 0;

  if (!strcmp(cmd, "update") ||
      !strcmp(cmd, "upload-leapfrog") ||
      !strcmp(cmd, "upload-app") ||
      !strcmp(cmd, "upload-esp")) {
    if (argc != 3) {
      fprintf(stderr, "Missing firmware file argument\n");
      result = 1;
    } else {
      ctx.container = read_firmware_file(argv[2]);
      if (!ctx.container) {
        fprintf(stderr, "Failed to load firmware file\n");
        result = 1;
      } else {
        result = execute_command(&ctx, cmd);
      }
    }
  } else {
    result = execute_command(&ctx, cmd);
  }

  close(sock_fd);
  return result;
}
