// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <json-c/json.h>

#include "device.h"
#include "../shared/fcp-shared.h"
#include "fcp-socket.h"
#include "esp-dfu.h"
#include "hash.h"
#include "log.h"

#define ESP_FLASH_BLOCK_SIZE 1024

/* Structure to hold all ESP DFU related values from devmap */
struct esp_dfu_config {
  /* State values from eSuperState enum */
  struct {
    uint8_t off;
    uint8_t dfu;
    uint8_t normal;
  } states;

  /* Notification values from eDFU_NOTIFICATION enum */
  struct {
    uint8_t clear;
    uint8_t next_block;
    uint8_t finish;
    uint8_t error;
  } dfu_notifications;

  /* Memory offsets */
  struct {
    int state;      /* ESP state offset */
    int esp_boot_mode;  /* Boot mode offset */
    int dfu_notify; /* DFU notification offset */
  } offsets;

  /* Client notification values */
  struct {
    uint32_t dfu_change;
  } notify_client;

  /* Device notification values */
  struct {
    uint32_t esp_boot_mode;
  } notify_device;
} config;

/* Get enum value from devmap */
static int get_enum_value(
  struct json_object *devmap,
  const char         *enum_name,
  const char         *value_name,
  uint8_t            *value
) {
  struct json_object *enums, *enum_obj, *enumerators, *enum_value;

  if (!json_object_object_get_ex(devmap, "enums", &enums)) {
    log_error("Failed to find 'enums' in devmap for enum '%s'", enum_name);
    return -1;
  }

  if (!json_object_object_get_ex(enums, enum_name, &enum_obj)) {
    log_error("Failed to find enum '%s' in devmap", enum_name);
    return -1;
  }

  if (!json_object_object_get_ex(enum_obj, "enumerators", &enumerators)) {
    log_error("Failed to find 'enumerators' in enum '%s'", enum_name);
    return -1;
  }

  if (!json_object_object_get_ex(enumerators, value_name, &enum_value)) {
    log_error("Failed to find value '%s' in enum '%s'", value_name, enum_name);
    return -1;
  }

  *value = json_object_get_int(enum_value);
  return 0;
}

/* Get all ESP DFU configuration from devmap
 * Returns 0 on success, -1 on failure
 */
static int get_esp_dfu_config(struct json_object *devmap) {
  int err;

  /* Get state values */
  err = get_enum_value(devmap, "eSuperState", "eSuperOff", &config.states.off) ||
        get_enum_value(devmap, "eSuperState", "eSuperDFU", &config.states.dfu) ||
        get_enum_value(devmap, "eSuperState", "eSuperNormal", &config.states.normal);
  if (err) {
    log_error("Failed to get state values from devmap");
    return -1;
  }

  /* Get notification values */
  err = get_enum_value(devmap, "eDFU_NOTIFICATION", "eClear", &config.dfu_notifications.clear) ||
        get_enum_value(devmap, "eDFU_NOTIFICATION", "eNextblock", &config.dfu_notifications.next_block) ||
        get_enum_value(devmap, "eDFU_NOTIFICATION", "eFinish", &config.dfu_notifications.finish) ||
        get_enum_value(devmap, "eDFU_NOTIFICATION", "eError", &config.dfu_notifications.error);
  if (err) {
    log_error("Failed to get notification values from devmap");
    return -1;
  }

  /* Get DFU notification mask */
  struct json_object *notify_types;
  struct json_object *dfu_change;
  if (!json_object_object_get_ex(devmap, "enums", &notify_types) ||
      !json_object_object_get_ex(notify_types, "eDEV_FCP_NOTIFY_MESSAGE_TYPE", &notify_types) ||
      !json_object_object_get_ex(notify_types, "enumerators", &notify_types) ||
      !json_object_object_get_ex(notify_types, "FCP_NOTIFY_DFU_CHANGE", &dfu_change)) {
    log_error("Cannot find DFU notification type");
    return -1;
  }
  config.notify_client.dfu_change = json_object_get_int(dfu_change);

  /* Get offsets */
  struct json_object *structs, *app_space, *members;
  struct json_object *esp_space, *esp_members;
  struct json_object *super_state;
  struct json_object *esp_boot_mode, *boot_mode_offset, *boot_mode_notify;
  struct json_object *dfu_notify;
  int esp_base;

  /* Get the APP_SPACE structure */
  if (!json_object_object_get_ex(devmap, "structs", &structs) ||
      !json_object_object_get_ex(structs, "APP_SPACE", &app_space) ||
      !json_object_object_get_ex(app_space, "members", &members)) {
    log_error("Cannot find APP_SPACE structure");
    return -1;
  }

  /* Get espSpace base offset and structure */
  struct json_object *esp_space_member;
  if (!json_object_object_get_ex(members, "espSpace", &esp_space_member) ||
      !json_object_object_get_ex(esp_space_member, "offset", &esp_space_member)) {
    log_error("Cannot find espSpace offset");
    return -1;
  }
  esp_base = json_object_get_int(esp_space_member);

  /* Get ESP_SPACE structure */
  if (!json_object_object_get_ex(structs, "ESP_SPACE", &esp_space) ||
      !json_object_object_get_ex(esp_space, "members", &esp_members)) {
    log_error("Cannot find ESP_SPACE structure");
    return -1;
  }

  /* Get member offsets */
  if (!json_object_object_get_ex(esp_members, "SuperState", &super_state) ||
      !json_object_object_get_ex(super_state, "offset", &super_state)) {
    log_error("Cannot find SuperState offset");
    return -1;
  }

  if (!json_object_object_get_ex(members, "ESPBootMode", &esp_boot_mode) ||
      !json_object_object_get_ex(esp_boot_mode, "offset", &boot_mode_offset) ||
      !json_object_object_get_ex(esp_boot_mode, "notify-device", &boot_mode_notify)) {
    log_error("Cannot find ESPBootMode offset/notify-device");
    return -1;
  }
  config.notify_device.esp_boot_mode = json_object_get_int(boot_mode_notify);

  if (!json_object_object_get_ex(esp_members, "DFU_NOTIFY", &dfu_notify) ||
      !json_object_object_get_ex(dfu_notify, "offset", &dfu_notify)) {
    log_error("Cannot find DFU_NOTIFY offset");
    return -1;
  }

  /* Calculate final offsets */
  config.offsets.state = esp_base + json_object_get_int(super_state);
  config.offsets.esp_boot_mode = json_object_get_int(boot_mode_offset);
  config.offsets.dfu_notify = esp_base + json_object_get_int(dfu_notify);

  return 0;
}

/* Read ESP state
 * Returns 0 on success, FCP_SOCKET_ERR_FCP on failure
 */
int fcp_esp_get_state(snd_hwdep_t *hwdep, int *state) {
  int err = fcp_data_read(hwdep, config.offsets.state, 1, false, state);

  if (err < 0)
    log_error("Cannot get ESP state: %s", snd_strerror(err));

  return err < 0 ? FCP_SOCKET_ERR_FCP : 0;
}

/* Set ESP boot mode
 * Returns 0 on success, FCP_SOCKET_ERR_FCP on failure
 */
int fcp_esp_set_boot_mode(snd_hwdep_t *hwdep, int mode) {
  int err = fcp_data_write(hwdep, config.offsets.esp_boot_mode, 1, mode);
  if (err < 0) {
    log_error("Cannot set ESP boot mode: %s", snd_strerror(err));
    return FCP_SOCKET_ERR_FCP;
  }

  err = fcp_data_notify(hwdep, config.notify_device.esp_boot_mode);
  if (err < 0) {
    log_error("Cannot notify ESP boot mode: %s", snd_strerror(err));
    return FCP_SOCKET_ERR_FCP;
  }

  return 0;
}

/* Get the ESP DFU notification
 * Returns 0 on success, FCP_SOCKET_ERR_FCP on failure
 */
int fcp_esp_get_dfu_notify(snd_hwdep_t *hwdep, int *notify) {
  int err = fcp_data_read(hwdep, config.offsets.dfu_notify, 1, false, notify);

  if (err < 0)
    log_error("Cannot get ESP DFU notify: %s", snd_strerror(err));

  return err < 0 ? FCP_SOCKET_ERR_FCP : 0;
}

/* Clear the ESP DFU notification
 * Returns 0 on success, FCP_SOCKET_ERR_FCP on failure
 */
int fcp_esp_clear_dfu_notify(snd_hwdep_t *hwdep) {
  int err = fcp_data_write(hwdep, config.offsets.dfu_notify, 1, config.dfu_notifications.clear);

  if (err < 0)
    log_error("Cannot clear ESP DFU notify: %s", snd_strerror(err));

  return err < 0 ? FCP_SOCKET_ERR_FCP : 0;
}

/* Wait for a DFU change notification from the ESP
 * Returns 0 on success (notification received), FCP_SOCKET_ERR_FCP or
 * FCP_SOCKET_ERR_TIMEOUT on failure
 */
static int wait_for_esp_notification(struct fcp_device *device, const char *msg) {
  int fd = device->hwdep_fd;
  fd_set rfds;
  struct timeval tv;
  time_t start_time = time(NULL);

  log_debug("Waiting for ESP notification: %s", msg);

  // Wait up to 10 seconds
  while (difftime(time(NULL), start_time) < 10) {
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    tv.tv_sec = 10 - difftime(time(NULL), start_time);
    tv.tv_usec = 0;

    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);

    // Timeout
    if (!ret)
      break;

    // Error
    if (ret < 0) {
      if (errno == EINTR) {
        log_debug("Select interrupted, retrying");
        continue;
      }
      log_error("Select failed waiting for %s: %s", msg, strerror(errno));
      return FCP_SOCKET_ERR_FCP;
    }

    // Check for data
    if (!FD_ISSET(fd, &rfds))
      continue;

    // Read notification
    uint32_t notification;
    int err = snd_hwdep_read(device->hwdep, &notification, sizeof(notification));
    if (err < 0) {
      log_error(
        "Failed to read notification for %s: %s",
        msg,
        snd_strerror(err)
      );
      return FCP_SOCKET_ERR_FCP;
    }
    log_debug(
      "Received notification 0x%08x while waiting for %s",
      notification,
      msg
    );

    // Check for DFU change notification and return if found
    if (notification & config.notify_client.dfu_change)
      return 0;
  }

  log_error("Timeout waiting for %s", msg);
  return FCP_SOCKET_ERR_TIMEOUT;
}

/* Check that the ESP state matches the expected state and wait up to
 * 500ms for it to change if it doesn't
 * Returns 0 on success, FCP_SOCKET_ERR_FCP or FCP_SOCKET_ERR_INVALID_STATE on failure
 */
static int check_esp_state(struct fcp_device *device, int expected_state) {
  int esp_state;

  for (int i = 0; i < 5; i++) {
    int err = fcp_esp_get_state(device->hwdep, &esp_state);
    if (err < 0)
      return err;

    log_debug("ESP state: %d, expected: %d", esp_state, expected_state);

    if (esp_state == expected_state)
      return 0;

    log_debug("Wait to see if ESP state changes");
    usleep(100 * 1000);
  }

  log_error(
    "ESP state change timeout; expected %d, got %d",
    expected_state,
    esp_state
  );

  return FCP_SOCKET_ERR_INVALID_STATE;
}

/* Wait for a particular ESP DFU notification and clear it
 * Returns 0 on success, FCP_SOCKET_ERR_FCP or FCP_SOCKET_ERR_TIMEOUT on failure
 */
static int wait_for_esp_dfu_notify(
  struct fcp_device *device,
  uint8_t            expected_notify,
  const char        *msg
) {
  int err;
  int dfu_notify;

  // Check up to 5 times
  for (int i = 0; i < 5; i++) {

    // Wait for notification; if it times out, return
    err = wait_for_esp_notification(device, msg);
    if (err < 0)
      return err;

    // Get the notification
    err = fcp_esp_get_dfu_notify(device->hwdep, &dfu_notify);
    if (err < 0)
      return FCP_SOCKET_ERR_FCP;

    // Clear the notification
    err = fcp_esp_clear_dfu_notify(device->hwdep);
    if (err < 0)
      return FCP_SOCKET_ERR_FCP;

    // Return if it's the expected notification
    if (dfu_notify == expected_notify)
      return 0;

    // Wait 100ms before retrying
    usleep(100 * 1000);
  }

  log_error("ESP DFU notify timeout waiting for %s", msg);
  return FCP_SOCKET_ERR_TIMEOUT;
}

/* Set the ESP state to the target state
 * Returns 0 on success, FCP_SOCKET_ERR_FCP or FCP_SOCKET_ERR_TIMEOUT on failure
 */
static int set_esp_state(struct fcp_device *device, uint8_t target_state) {
  log_debug("Setting ESP state to %d", target_state);

  int err = fcp_esp_set_boot_mode(device->hwdep, target_state);
  if (err < 0)
    return err;

  log_debug("Waiting for ESP boot mode change");

  // Wait for state change
  err = wait_for_esp_notification(device, "ESP state change");
  if (err < 0)
    return err;

  return check_esp_state(device, target_state);
}

/* Update ESP firmware
 * Returns 0 on success, FCP_SOCKET_ERR_* on failure
 */
int handle_esp_firmware_update(
  struct fcp_device                  *device,
  int                                 client_fd,
  const struct fcp_socket_msg_header *header
) {
  int err;
  int last_progress = -1;

  err = get_esp_dfu_config(device->devmap);
  if (err < 0)
    return FCP_SOCKET_ERR_CONFIG;

  struct firmware_payload *payload = (struct firmware_payload *)(header + 1);

  // Check USB ID
  if (payload->usb_vid != device->usb_vid ||
      payload->usb_pid != device->usb_pid) {
    log_error(
      "Invalid USB ID: expected %04x:%04x, got %04x:%04x",
      device->usb_vid,
      device->usb_pid,
      payload->usb_vid,
      payload->usb_pid
    );
    return FCP_SOCKET_ERR_INVALID_USB_ID;
  }

  // Verify SHA256
  if (!verify_sha256(payload->data, payload->size, payload->sha256)) {
    return FCP_SOCKET_ERR_INVALID_HASH;
  }

  // Send 0% progress
  send_progress(client_fd, 0);

  // Turn ESP off if it's on
  int esp_state;
  err = fcp_esp_get_state(device->hwdep, &esp_state);
  if (err < 0)
    return err;

  // ESP state zero is not listed in eSuperState; probably not running
  // leapfrog firmware
  if (!esp_state) {
    log_error("ESP state (0) invalid (not running leapfrog firmware?)");
    return FCP_SOCKET_ERR_NOT_LEAPFROG;
  }

  // Turn off ESP if it's on
  if (esp_state == config.states.normal) {
    err = set_esp_state(device, config.states.off);
    if (err < 0)
      return err;

  // If it's not off, we can't update the firmware
  } else if (esp_state != config.states.off) {
    log_error("ESP is not off (state is %d), cannot update firmware", esp_state);
    return FCP_SOCKET_ERR_INVALID_STATE;
  }

  // Start DFU
  err = fcp_esp_dfu_start(device->hwdep, payload->size, payload->md5);
  if (err < 0)
    return FCP_SOCKET_ERR_FCP;

  // Wait for ESP to enter DFU mode
  err = wait_for_esp_notification(device, "ESP to enter DFU mode");
  if (err < 0)
    return err;

  err = check_esp_state(device, config.states.dfu);
  if (err < 0)
    return err;

  // Wait for next block notification
  err = wait_for_esp_dfu_notify(device, config.dfu_notifications.next_block, "next block");
  if (err < 0)
    return err;

  // Write firmware blocks
  for (size_t offset = 0; offset < payload->size; offset += ESP_FLASH_BLOCK_SIZE) {
    size_t block_size = payload->size - offset;
    if (block_size > ESP_FLASH_BLOCK_SIZE)
      block_size = ESP_FLASH_BLOCK_SIZE;

    err = fcp_esp_dfu_write(device->hwdep, payload->data + offset, block_size);
    if (err < 0) {
      log_error("Error writing block at offset %zu", offset);
      return FCP_SOCKET_ERR_WRITE;
    }

    // Wait for next block notification
    err = wait_for_esp_dfu_notify(device, config.dfu_notifications.next_block, "next block");
    if (err < 0) {
      log_error("Error waiting for next block notification");
      return err;
    }

    // Send progress
    int progress = (offset + block_size) * 100 / payload->size;
    if (progress != last_progress) {
      last_progress = progress;
      send_progress(client_fd, progress);
    }
  }

  // Send final empty write to complete
  err = fcp_esp_dfu_write(device->hwdep, NULL, 0);
  if (err < 0) {
    log_error("Error writing final block");
    return FCP_SOCKET_ERR_WRITE;
  }

  // Wait for finish notification
  err = wait_for_esp_dfu_notify(device, config.dfu_notifications.finish, "finish");
  if (err < 0)
    return err;

  // Turn ESP off
  err = set_esp_state(device, config.states.off);
  if (err < 0)
    return err;

  // Turn ESP back on
  err = set_esp_state(device, config.states.normal);
  if (err < 0) {
    return err;
  }

  // Send 100% progress
  if (last_progress != 100)
    send_progress(client_fd, 100);

  return 0;
}
