// SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "log.h"

// Global state
static bool use_systemd = false;
static log_level_t current_level = LOG_LEVEL_INFO;

static bool check_journal_stream(void) {
  const char *env = getenv("JOURNAL_STREAM");
  if (!env) return false;

  unsigned long long dev = 0, inode = 0;
  if (sscanf(env, "%llu:%llu", &dev, &inode) != 2) return false;

  struct stat statbuf;
  if (fstat(STDERR_FILENO, &statbuf) < 0) return false;

  return statbuf.st_dev == dev && statbuf.st_ino == inode;
}

void log_init(void) {
  use_systemd = check_journal_stream();

  const char *env = getenv("LOG_LEVEL");
  if (env) {
    if (!strcmp(env, "error")) current_level = LOG_LEVEL_ERROR;
    else if (!strcmp(env, "warning")) current_level = LOG_LEVEL_WARNING;
    else if (!strcmp(env, "info")) current_level = LOG_LEVEL_INFO;
    else if (!strcmp(env, "debug")) current_level = LOG_LEVEL_DEBUG;
  }
}

void log_msg(log_level_t level, const char *fmt, ...) {
  if (level > current_level) {
    return;
  }

  va_list args;
  va_start(args, fmt);

  if (use_systemd) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);

#ifdef HAVE_SYSTEMD
    int priority = LOG_INFO;
    switch (level) {
      case LOG_LEVEL_ERROR: priority = LOG_ERR; break;
      case LOG_LEVEL_WARNING: priority = LOG_WARNING; break;
      case LOG_LEVEL_INFO: priority = LOG_INFO; break;
      case LOG_LEVEL_DEBUG: priority = LOG_DEBUG; break;
    }

    sd_journal_print(priority, "%s", buf);
#endif
  } else {
    FILE *out = level <= LOG_LEVEL_WARNING ? stderr : stdout;
    vfprintf(out, fmt, args);
    fprintf(out, "\n");
  }

  va_end(args);
}
