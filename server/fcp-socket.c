// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/limits.h>

#include "../shared/fcp-shared.h"
#include "fcp-socket.h"
#include "fcp.h"
#include "esp-dfu.h"
#include "hash.h"
#include "log.h"

#define FLASH_BLOCK_SIZE 4096

static int server_sock = -1;
static struct fcp_device *device = NULL;

static int have_flash_info = false;
static int upgrade_segment_num = -1;
static int upgrade_segment_size = -1;
static int settings_segment_num = -1;
static int settings_segment_size = -1;
static int disk_segment_num = -1;
static int disk_segment_size = -1;
static int env_segment_num = -1;
static int env_segment_size = -1;

// Track current client state
struct client_state {
  int     fd;            // Client socket fd, -1 if none
  void   *buffer;        // Current message buffer
  size_t  size;          // Current buffer size
  size_t  bytes_read;    // How much we've read so far
  size_t  total_size;    // Total message size (once known)
};

static struct client_state client = {
  .fd = -1,
  .buffer = NULL,
  .size = 0,
  .bytes_read = 0,
  .total_size = 0
};

static void cleanup_client(void) {
  if (client.fd >= 0) {
    close(client.fd);
    client.fd = -1;
  }
  free(client.buffer);
  client.buffer = NULL;
  client.size = 0;
  client.bytes_read = 0;
  client.total_size = 0;
}

// Reject any pending connections in the accept queue. Call this
// periodically during long-running operations to prevent new clients
// from blocking.
void drain_pending_connections(void) {
  if (server_sock < 0)
    return;

  while (1) {
    int tmp_fd = accept(server_sock, NULL, NULL);
    if (tmp_fd < 0)
      break;
    log_debug("Rejected additional client connection");
    close(tmp_fd);
  }
}

static void send_response(int client_fd, uint8_t response_type, const void *payload, size_t payload_length) {
  int ret;
  struct fcp_socket_msg_header header = {
    .magic          = FCP_SOCKET_MAGIC_RESPONSE,
    .msg_type       = response_type,
    .payload_length = payload_length
  };

  // Send header
  ret = send(client_fd, &header, sizeof(header), MSG_NOSIGNAL);
  if (ret < 0) {
    log_error("Error sending fcp-socket response header: %s", strerror(errno));
    return;
  }
  if (ret != sizeof(header)) {
    log_error("Error sending fcp-socket response header");
    return;
  }

  // Send payload if present
  if (payload && payload_length > 0) {
    ret = send(client_fd, payload, payload_length, MSG_NOSIGNAL);
    if (ret < 0) {
      log_error("Error sending fcp-socket response payload: %s", strerror(errno));
      return;
    }
    if (ret != payload_length) {
      log_error("Error sending fcp-socket response payload");
      return;
    }
  }
}

static void send_error(int client_fd, int16_t code) {
  send_response(client_fd, FCP_SOCKET_RESPONSE_ERROR, &code, sizeof(code));
}

void send_progress(int client_fd, uint8_t percent) {
  send_response(client_fd, FCP_SOCKET_RESPONSE_PROGRESS, &percent, sizeof(percent));
}

static int get_segment_nums(snd_hwdep_t *device) {

  if (have_flash_info) {
    return 0;
  }

  int size, count;
  int ret = fcp_flash_info(device, &size, &count);
  if (ret != 0) {
    log_error("Failed to get flash info from device");
    return -1;
  }

  log_debug("Flash size: %d", size);
  log_debug("Segment count: %d", count);

  if (count < 1 || count > 15) {
    log_error("Invalid segment count: %d (expected 1-15)", count);
    return -1;
  }

  for (int i = 0; i < count; i++) {
    log_debug("Segment %d", i);

    int segment_size;
    uint32_t flags;
    char *name;

    ret = fcp_flash_segment_info(device, i, &segment_size, &flags, &name);
    if (ret != 0) {
      log_error("Failed to get segment info for segment %d", i);
      return -1;
    }

    log_debug("  Size: %d", segment_size);
    log_debug("  Flags: 0x%08x", flags);
    log_debug("  Name: %s", name);

    if (!strcmp(name, "App_Upgrade")) {
      upgrade_segment_num = i;
      upgrade_segment_size = segment_size;
    } else if (!strcmp(name, "App_Settings")) {
      settings_segment_num = i;
      settings_segment_size = segment_size;
    } else if (!strcmp(name, "App_Disk")) {
      disk_segment_num = i;
      disk_segment_size = segment_size;
    } else if (!strcmp(name, "App_Env")) {
      env_segment_num = i;
      env_segment_size = segment_size;
    }
  }

  if (upgrade_segment_num == 0) {
    log_error("Invalid upgrade segment number %d", upgrade_segment_num);
    return -1;
  } else if (settings_segment_num == 0) {
    log_error("Invalid settings segment number %d", settings_segment_num);
    return -1;
  } else if (disk_segment_num == 0) {
    log_error("Invalid disk segment number %d", disk_segment_num);
    return -1;
  } else if (env_segment_num == 0) {
    log_error("Invalid env segment number %d", env_segment_num);
    return -1;
  }

  log_debug("Flash info:");
  log_debug("  Upgrade segment: %d", upgrade_segment_num);
  log_debug("  Settings segment: %d", settings_segment_num);
  log_debug("  Disk segment: %d", disk_segment_num);
  log_debug("  Env segment: %d", env_segment_num);

  have_flash_info = true;

  return 0;
}

static int erase_flash_segment(int client_fd, int segment_num, int num_blocks) {
  static int last_progress = -1;

  if (segment_num < 1 || segment_num > 15) {
    log_error("Invalid segment number %d for erase", segment_num);
    return FCP_SOCKET_ERR_READ;
  }

  if (num_blocks < 1 || num_blocks > 255) {
    log_error("Invalid number of blocks %d for erase", num_blocks);
    return FCP_SOCKET_ERR_READ;
  }

  log_debug("Erasing segment %d", segment_num);

  int ret = fcp_flash_erase(device->hwdep, segment_num);
  if (ret != 0) {
    log_error("Error erasing flash segment %d: %d", segment_num, ret);
    return FCP_SOCKET_ERR_WRITE;
  }

  while (1) {
    int ret = fcp_flash_erase_progress(device->hwdep, segment_num);
    if (ret < 0) {
      log_error("Error getting flash erase progress: %d", ret);
      return FCP_SOCKET_ERR_READ;
    }

    if (ret == 255)
      break;

    int progress = ret * 100 / num_blocks;

    if (progress != last_progress) {
      send_progress(client_fd, progress);
      last_progress = progress;
    }

    // wait 50ms
    usleep(50000);

    drain_pending_connections();
  }

  if (last_progress != 100)
    send_progress(client_fd, 100);

  return 0;
}

static int erase_config(int client_fd) {
  int ret = get_segment_nums(device->hwdep);
  if (ret < 0) {
    log_error("Error getting segment numbers");
    return FCP_SOCKET_ERR_READ;
  }

  return erase_flash_segment(
    client_fd,
    settings_segment_num,
    settings_segment_size / FLASH_BLOCK_SIZE
  );
}

static int erase_app_firmware(int client_fd) {
  int ret = get_segment_nums(device->hwdep);
  if (ret < 0) {
    log_error("Error getting segment numbers");
    return FCP_SOCKET_ERR_READ;
  }

  return erase_flash_segment(
    client_fd,
    upgrade_segment_num,
    upgrade_segment_size / FLASH_BLOCK_SIZE
  );
}

static int handle_app_firmware_update(
  int client_fd, const struct fcp_socket_msg_header *header
) {
  struct firmware_payload *payload = (struct firmware_payload *) (header + 1);
  int last_progress = -1;

  int ret = get_segment_nums(device->hwdep);
  if (ret < 0) {
    log_error("Error getting segment numbers");
    return FCP_SOCKET_ERR_READ;
  }

  if (payload->size < 65536) {
    log_error("Firmware data too small: %d", payload->size);
    send_error(client_fd, FCP_SOCKET_ERR_INVALID_LENGTH);
    return FCP_SOCKET_ERR_INVALID_LENGTH;
  } else if (payload->size > upgrade_segment_size) {
    log_error(
      "Firmware data too large: %d > %d", payload->size, upgrade_segment_size
    );
    send_error(client_fd, FCP_SOCKET_ERR_INVALID_LENGTH);
    return FCP_SOCKET_ERR_INVALID_LENGTH;
  }

  // Verify hash
  if (!verify_sha256(payload->data, payload->size, payload->sha256)) {
    send_error(client_fd, FCP_SOCKET_ERR_INVALID_HASH);
    return FCP_SOCKET_ERR_INVALID_HASH;
  }

  // Verify PID
  if (payload->usb_vid != device->usb_vid ||
      payload->usb_pid != device->usb_pid) {
    log_error("Expected VID:PID %04x:%04x, got %04x:%04x",
              device->usb_vid, device->usb_pid,
              payload->usb_vid, payload->usb_pid);
    send_error(client_fd, FCP_SOCKET_ERR_INVALID_USB_ID);
    return FCP_SOCKET_ERR_INVALID_USB_ID;
  }

  // display first/last 16 bytes of firmware data
  {
    char s[256] = "";
    char *p = s;

    p += snprintf(
      p,
      sizeof(s) - (p - s),
      "Firmware data (length %d):",
      payload->size
    );
    for (int i = 0; i < 16; i++)
      p += snprintf(p, sizeof(s) - (p - s), " %02x", payload->data[i]);
    p += snprintf(p, sizeof(s) - (p - s), " ...");
    for (int i = payload->size - 16; i < payload->size; i++)
      p += snprintf(p, sizeof(s) - (p - s), " %02x", payload->data[i]);
    log_debug("%s", s);
  }

  for (int i = 0; i < payload->size; i += FCP_FLASH_WRITE_MAX) {
    int size = payload->size - i;
    if (size > FCP_FLASH_WRITE_MAX) {
      size = FCP_FLASH_WRITE_MAX;
    }
    int ret = fcp_flash_write(
      device->hwdep,
      upgrade_segment_num,
      i,
      size,
      payload->data + i
    );
    if (ret != 0) {
      log_error("Error writing flash segment");
      return FCP_SOCKET_ERR_WRITE;
    }

    int progress = i * 100 / payload->size;
    if (progress != last_progress) {
      send_progress(client_fd, progress);
      last_progress = progress;
    }

    drain_pending_connections();
  }

  if (last_progress != 100)
    send_progress(client_fd, 100);

  return 0;
}

static void handle_client_command(
  int client_fd,
  const struct fcp_socket_msg_header *header
) {
  int ret = 0;

  switch (header->msg_type) {
    case FCP_SOCKET_REQUEST_REBOOT:
      ret = fcp_reboot(device->hwdep);
      break;

    case FCP_SOCKET_REQUEST_CONFIG_ERASE:
      ret = erase_config(client_fd);
      break;

    case FCP_SOCKET_REQUEST_APP_FIRMWARE_ERASE:
      ret = erase_app_firmware(client_fd);
      break;

    case FCP_SOCKET_REQUEST_APP_FIRMWARE_UPDATE:
      ret = handle_app_firmware_update(client_fd, header);
      break;

    case FCP_SOCKET_REQUEST_ESP_FIRMWARE_UPDATE:
      ret = handle_esp_firmware_update(device, client_fd, header);
      break;

    default:
      send_error(client_fd, FCP_SOCKET_ERR_INVALID_COMMAND);
      return;
  }

  if (ret != 0) {
    send_error(client_fd, ret);
  } else {
    send_response(client_fd, FCP_SOCKET_RESPONSE_SUCCESS, NULL, 0);
  }
}

// Returns:
//  0 on sucess
// -1 on error
static int process_client_data(void) {
  ssize_t n;

  // First allocation if needed
  if (!client.buffer) {
    client.size = 4096;  // Initial buffer size
    client.buffer = malloc(client.size);
    if (!client.buffer) {
      log_error("Cannot allocate client buffer: %s", strerror(errno));
      return -1;
    }
  }

  // Read what we can
  n = read(
    client.fd,
    client.buffer + client.bytes_read,
    client.size - client.bytes_read
  );

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;  // Try again later
    }
    return -1;   // Error
  }
  if (n == 0) {
    return -1;   // EOF
  }

  client.bytes_read += n;

  // If we don't have the header yet, wait for more
  if (client.bytes_read < sizeof(struct fcp_socket_msg_header)) {
    return 0;
  }

  // Once we have the header, calculate total size
  if (client.total_size == 0) {
    struct fcp_socket_msg_header *header = client.buffer;
    client.total_size = sizeof(struct fcp_socket_msg_header) + header->payload_length;

    // Validate magic number
    if (header->magic != FCP_SOCKET_MAGIC_REQUEST) {
      send_error(client.fd, FCP_SOCKET_ERR_INVALID_MAGIC);
      return -1;
    }

    // Validate size
    if (header->payload_length > MAX_PAYLOAD_LENGTH) {
      send_error(client.fd, FCP_SOCKET_ERR_INVALID_LENGTH);
      return -1;
    }

    // Resize buffer if needed
    if (client.total_size > client.size) {
      void *new_buf = realloc(client.buffer, client.total_size);
      if (!new_buf) {
        log_error("Cannot reallocate client buffer: %s", strerror(errno));
        return -1;
      }
      client.buffer = new_buf;
      client.size = client.total_size;
    }
  }

  // Return 1 if we have the complete message
  if (client.bytes_read >= client.total_size) {
    struct fcp_socket_msg_header *header = client.buffer;

    // Process message
    handle_client_command(client.fd, header);

    // Reset state
    client.bytes_read = 0;
    client.total_size = 0;

    return 1;  // Message complete
  }

  return 0;  // Need more data
}

void fcp_socket_handle_client(void) {
  if (client.fd < 0) {
    // Accept new client if we don't have one
    client.fd = accept(server_sock, NULL, NULL);
    if (client.fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        log_error("Error accepting client connection: %s", strerror(errno));
      return;
    }
    fcntl(client.fd, F_SETFL, O_NONBLOCK);
    return;
  }

  // Process data
  int ret = process_client_data();

  // Error or complete message
  if (ret != 0) {
    log_debug("Client connection closed");
    cleanup_client();
  }
}

static int set_socket_path_tlv(struct fcp_device *device, const char *path) {
  snd_ctl_elem_id_t *id;
  int err;
  snd_ctl_elem_id_alloca(&id);

  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
  snd_ctl_elem_id_set_name(id, "Firmware Version");
  snd_ctl_elem_id_set_index(id, 0);

  // Size needs to align to sizeof(unsigned int)
  size_t path_len = strlen(path) + 1;
  int align = sizeof(unsigned int) - 1;
  size_t total_size = (path_len + align) & ~align;

  unsigned int *tlv = calloc(2 + total_size / 4, sizeof(unsigned int));
  if (!tlv) {
    log_error("Cannot allocate TLV memory");
    return -ENOMEM;
  }

  // Use unique identifier for socket path TLV
  tlv[0] = 0x53434B54; // "SCKT"
  tlv[1] = total_size;

  memcpy(&tlv[2], path, path_len);

  err = snd_ctl_elem_tlv_write(device->ctl, id, tlv);
  if (err < 0) {
    log_error("Cannot write socket path TLV: %s", snd_strerror(err));
  }

  free(tlv);

  // Lock the control element so users know we're now running
  err = snd_ctl_elem_lock(device->ctl, id);
  if (err < 0) {
    log_error("Cannot lock control element: %s", snd_strerror(err));
    return err;
  }

  return err;
}

int fcp_socket_init(struct fcp_device *dev) {
  struct sockaddr_un addr;

  device = dev;

  // Choose socket path
  const char *runtime_dir = getenv("RUNTIME_DIRECTORY");

  if (!runtime_dir) {
    runtime_dir = getenv("XDG_RUNTIME_DIR");

    if (!runtime_dir)
      runtime_dir = "/tmp";
  }

  char socket_path[PATH_MAX];
  snprintf(
    socket_path,
    sizeof(socket_path),
    "%s/fcp-%d.sock",
    runtime_dir,
    device->card_num
  );

  log_debug("Using socket path: %s", socket_path);

  // Create socket
  server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_sock < 0) {
    log_error("Cannot create socket: %s", strerror(errno));
    return -errno;
  }

  // Set socket to non-blocking
  if (fcntl(server_sock, F_SETFL, O_NONBLOCK) < 0) {
    log_error("Cannot set socket to non-blocking: %s", strerror(errno));
    close(server_sock);
    return -errno;
  }

  // Bind socket
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(socket_path) >= sizeof(addr.sun_path)) {
    log_error("Socket path too long: %s", socket_path);
    close(server_sock);
    return -ENAMETOOLONG;
  }
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  unlink(socket_path);

  if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    log_error("Cannot bind to %s: %s", socket_path, strerror(errno));
    close(server_sock);
    return -errno;
  }

  // Listen for connections
  if (listen(server_sock, 1) < 0) {
    log_error("Cannot listen on socket %s: %s", socket_path, strerror(errno));
    close(server_sock);
    return -errno;
  }

  // Set socket path TLV
  int ret = set_socket_path_tlv(device, socket_path);
  if (ret == 0) {
    log_debug("Socket path TLV set to %s", socket_path);
  }

  return 0;
}

// Add client fd to select() set
void fcp_socket_update_sets(fd_set *rfds, int *max_fd) {
  // Always add listening fd
  if (server_sock >= 0) {
    FD_SET(server_sock, rfds);
    if (server_sock > *max_fd) {
      *max_fd = server_sock;
    }
  }

  // Add client fd if we have one
  if (client.fd >= 0) {
    FD_SET(client.fd, rfds);
    if (client.fd > *max_fd) {
      *max_fd = client.fd;
    }
  }
}

void fcp_socket_handle_events(fd_set *rfds) {

  // Check for new connections
  if (server_sock >= 0 && FD_ISSET(server_sock, rfds)) {

    // Only accept if we don't have a client
    if (client.fd < 0) {

      // Accept new client
      client.fd = accept(server_sock, NULL, NULL);
      if (client.fd < 0) {
        log_error("Error accepting client connection: %s", strerror(errno));
      } else {
        if (fcntl(client.fd, F_SETFL, O_NONBLOCK) < 0) {
          log_error("Cannot set client socket to non-blocking: %s", strerror(errno));
          close(client.fd);
          client.fd = -1;
        } else {
          log_debug("Client connected");
        }
      }
    } else {
      drain_pending_connections();
    }
  }

  // Process client data
  if (client.fd >= 0 && FD_ISSET(client.fd, rfds)) {
    int result = process_client_data();
    if (result < 0) {
      log_debug("Client connection closed");
      cleanup_client();
    }
  }
}
