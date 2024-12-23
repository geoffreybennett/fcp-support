// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "device.h"

int fcp_socket_init(struct fcp_device *device);
void fcp_socket_cleanup(void);

void fcp_socket_update_sets(fd_set *rfds, int *max_fd);
void fcp_socket_handle_events(fd_set *rfds);

void send_progress(int client_fd, uint8_t percent);
