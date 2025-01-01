# SPDX-FileCopyrightText: 2024 Geoffrey D. Bennett <g@b4.vu>
# SPDX-License-Identifier: GPL-3.0-or-later

# Credit to Tom Tromey and Paul D. Smith:
# http://make.mad-scientist.net/papers/advanced-auto-dependency-generation/

VERSION := $(shell \
  git describe --abbrev=4 --dirty --always --tags 2>/dev/null || \
  echo $${APP_VERSION:-Unknown} \
)

# Installation paths
ifeq ($(PREFIX),)
  PREFIX := /usr/local
endif

BINDIR := $(DESTDIR)$(PREFIX)/bin
SYSTEMD_DIR := $(DESTDIR)$(PREFIX)/lib/systemd/system
UDEV_DIR := $(DESTDIR)$(PREFIX)/lib/udev/rules.d
DATADIR_PATH := $(PREFIX)/share/fcp-server
DATADIR := $(DESTDIR)$(DATADIR_PATH)

DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$(*D)/$(*F).d

CFLAGS := -Wall -Werror -ggdb -fno-omit-frame-pointer -O2 -D_FORTIFY_SOURCE=2
CFLAGS += -DVERSION=\"$(VERSION)\"
CFLAGS += -DDATADIR=\"$(DATADIR_PATH)\"

PKG_CONFIG=pkg-config

CFLAGS += $(shell $(PKG_CONFIG) --cflags alsa)

LDFLAGS += $(shell $(PKG_CONFIG) --libs alsa)
LDFLAGS += $(shell $(PKG_CONFIG) --libs libcrypto)
LDFLAGS += $(shell $(PKG_CONFIG) --libs zlib)
LDFLAGS += $(shell $(PKG_CONFIG) --libs json-c)
LDFLAGS += -lm

SERVER_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsystemd)
SERVER_LDFLAGS := $(shell $(PKG_CONFIG) --libs libsystemd)

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) -c

# Define source files for each target
CLIENT_SRCS := $(sort $(wildcard client/*.c))
SERVER_SRCS := $(sort $(wildcard server/*.c))
SHARED_SRCS := $(sort $(wildcard shared/*.c))

# Define object files
CLIENT_OBJS := $(patsubst %.c,%.o,$(CLIENT_SRCS))
SERVER_OBJS := $(patsubst %.c,%.o,$(SERVER_SRCS))
SHARED_OBJS := $(patsubst %.c,%.o,$(SHARED_SRCS))

# Define dependency directories needed
CLIENT_DEPDIRS := $(addprefix $(DEPDIR)/,$(dir $(CLIENT_SRCS)))
SERVER_DEPDIRS := $(addprefix $(DEPDIR)/,$(dir $(SERVER_SRCS)))
SHARED_DEPDIRS := $(addprefix $(DEPDIR)/,$(dir $(SHARED_SRCS)))
DEPDIRS := $(sort $(CLIENT_DEPDIRS) $(SERVER_DEPDIRS) $(SHARED_DEPDIRS))

# Define targets
TARGETS := fcp-tool fcp-server systemd/fcp-server@.service

all: $(TARGETS)

# Create all dependency directories
$(DEPDIRS):
	mkdir -p $@

# Define dependency files
CLIENT_DEPS := $(CLIENT_SRCS:%.c=$(DEPDIR)/%.d)
SERVER_DEPS := $(SERVER_SRCS:%.c=$(DEPDIR)/%.d)
SHARED_DEPS := $(SHARED_SRCS:%.c=$(DEPDIR)/%.d)

# Update COMPILE.c for server files
$(SERVER_OBJS): COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(SERVER_CFLAGS) -c

# Pattern rule for object files
%.o: %.c | $(DEPDIRS)
	$(COMPILE.c) $(OUTPUT_OPTION) $<

$(CLIENT_DEPS):
$(SERVER_DEPS):
$(SHARED_DEPS):

-include $(wildcard $(CLIENT_DEPS))
-include $(wildcard $(SERVER_DEPS))
-include $(wildcard $(SHARED_DEPS))

fcp-tool: $(CLIENT_OBJS) $(SHARED_OBJS)
	cc -o $@ $(CLIENT_OBJS) $(SHARED_OBJS) ${LDFLAGS}

fcp-server: $(SERVER_OBJS)
	cc -o $@ $(SERVER_OBJS) ${LDFLAGS} ${SERVER_LDFLAGS}

clean: depclean
	rm -f $(TARGETS) $(CLIENT_OBJS) $(SERVER_OBJS) $(SHARED_OBJS) systemd/fcp-server@.service

depclean:
	rm -rf $(DEPDIR)

systemd/fcp-server@.service: systemd/fcp-server@.service.template
	sed 's|@PREFIX@|$(PREFIX)|g' $< > $@

install: all install-bin install-service install-rules install-data

install-bin:
	install -d $(BINDIR)
	install -m 755 fcp-tool $(BINDIR)
	install -m 755 fcp-server $(BINDIR)

install-service: systemd/fcp-server@.service
	install -D -m 644 $< $(SYSTEMD_DIR)/fcp-server@.service
	@echo "Run 'sudo systemctl daemon-reload' to reload systemd"

install-rules:
	install -D -m 644 udev/99-fcp.rules $(UDEV_DIR)/99-fcp.rules
	@echo "Run 'sudo udevadm control --reload-rules' to reload udev rules"

install-data:
	install -d $(DATADIR)
	install -m 644 data/fcp-alsa-map-*.json $(DATADIR)/

uninstall:
	rm -f $(BINDIR)/fcp-tool
	rm -f $(BINDIR)/fcp-server
	rm -f $(SYSTEMD_DIR)/fcp-server@.service
	rm -f $(UDEV_DIR)/99-fcp.rules
	rm -rf $(DATADIR)

help:
	@echo "fcp-support"
	@echo
	@echo "This Makefile knows about:"
	@echo "  make           - build fcp-server and fcp-tool"
	@echo "  make install   - install everything (binaries, service, rules, data)"
	@echo "  make uninstall - uninstall everything"
	@echo "  make clean     - remove build files"
	@echo "  make depclean  - remove dependency files"

.PHONY: all clean depclean install uninstall help install-bin install-service install-rules install-data
