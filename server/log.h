// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Log levels matching syslog/systemd
typedef enum {
  LOG_LEVEL_ERROR = 3,   // Error conditions
  LOG_LEVEL_WARNING = 4, // Warning conditions
  LOG_LEVEL_INFO = 6,    // Informational
  LOG_LEVEL_DEBUG = 7    // Debug messages
} log_level_t;

void log_init(void);
void log_msg(log_level_t level, const char *fmt, ...);

// Convenience macros
#define log_error(...) log_msg(LOG_LEVEL_ERROR, __VA_ARGS__)
#define log_warning(...) log_msg(LOG_LEVEL_WARNING, __VA_ARGS__)
#define log_info(...) log_msg(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_LEVEL_DEBUG, __VA_ARGS__)
