# Linux FCP (Focusrite Control Protocol) Support Tools

This repository provides the user-space components required for full
functionality of Focusrite USB audio interfaces that use the Linux FCP
driver.

## System Overview

FCP support in Linux consists of several components that work together:

1. **Kernel Driver** ([linux-fcp](https://github.com/geoffreybennett/linux-fcp))
   - Allows `fcp-server` to communicate with the device — install this
     first (not necessary if your kernel version is 6.14 or later)

2. **User-space Server** (this repo: `fcp-server`)
   - Communicates with the device via the kernel driver
   - Creates ALSA controls and transfers control changes to and from
     the device
   - Allows `alsa-scarlett-gui` and `fcp-tool` to update the
     device firmware, reset the configuration, and reboot

3. **Firmware Update Tool** (this repo: `fcp-tool`)
   - CLI utility for updating device firmware
   - Uploads firmware to the device via `fcp-server`

4. **GUI Application** ([alsa-scarlett-gui](https://github.com/geoffreybennett/alsa-scarlett-gui))
   - Provides a graphical interface for device control and
     firmware updates
   - Uses the ALSA controls created by `fcp-server`

## Supported Devices

The currently supported devices are:

- Scarlett 4th Gen 16i16, 18i16, and 18i20

The FCP driver itself supports all Focusrite USB audio interfaces
since the 2nd Gen Scarletts, so support for the other devices will be
added here later. Until then, the Linux Scarlett2 driver supports all
the other 2nd Gen and later Scarlett, Clarett USB, Clarett+, and
Vocaster devices.

## Prerequisites

1. Linux kernel 6.14 or later (the FCP kernel driver is included
   from this version). Check with `uname -r`.

2. Required packages for building:

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libasound2-dev libsystemd-dev \
  libssl-dev zlib1g-dev libjson-c-dev pkg-config

# Fedora
sudo dnf install make gcc alsa-lib-devel systemd-devel openssl-devel \
  zlib-devel json-c-devel pkgconfig
```

3. Audio group membership is required to use `fcp-tool`:

```bash
sudo usermod -a -G audio $USER
```

Log out and back in for the group membership change to take effect.

### Installation

RPM and deb packages are available from the
[GitHub Releases](https://github.com/geoffreybennett/fcp-support/releases)
page.

Alternatively, build from source:

1. Download, build and install:

```bash
git clone https://github.com/geoffreybennett/fcp-support.git
cd fcp-support
make
sudo make install
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
```

This will:
- Build `fcp-server` and `fcp-tool`
- Install binaries to `/usr/local/bin/`
- Install systemd service file to `/usr/local/lib/systemd/system/`
- Install udev rule to `/usr/local/lib/udev/rules.d/`
- Install data files to `/usr/local/share/fcp-server/`

2. Reload systemd and udev:

```bash
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
```

## Configuration

The installation process sets up:

1. **Systemd Service**: `fcp-server@.service`
   - Started automatically when a compatible device is detected
   - One instance per device (e.g., `fcp-server@1.service`)

2. **Udev Rules**:
   - Start systemd service when device is connected

## Usage

### Device Management

1. Normal operation:

  - Plug in the device
  - Server starts automatically
  - Check status with `systemctl status fcp-server@*`

2. Troubleshooting:

  - Watch the system log when connecting the device: `journalctl -f`
  - Stop the FCP server: `sudo systemctl stop fcp-server@*`
  - Get the card number with `aplay -l`
  - Start the FCP server manually with debug logging: `LOG_LEVEL=debug fcp-server <card-number>`

### Firmware Management

`alsa-scarlett-gui` will prompt you to update the firmware
automatically when a newer version is available. `fcp-tool`
provides equivalent command-line capabilities:

```bash
# View all commands
fcp-tool

# Update firmware (takes 1-2 minutes)
fcp-tool update

# Update firmware to a specific version
fcp-tool update -f /path/to/firmware.bin

# Maintenance commands
fcp-tool erase-config   # Reset device configuration to firmware defaults
fcp-tool reboot         # Reboot device
```

## Support

Report issues at: https://github.com/geoffreybennett/fcp-support/issues

Please include:

- Device model
- Linux distribution and version
- Output of `dmesg` and `lsusb`
- Output as per troubleshooting steps above

## Contact

- Author: Geoffrey D. Bennett
- Email: g@b4.vu
- GitHub: https://github.com/geoffreybennett

## Donations

This software, including the driver, tools, and GUI is Free Software
that I’ve independently developed using my own resources. It
represents hundreds of hours of development work.

If you find this software valuable, please consider making a donation.
Your show of appreciation, more than the amount itself, motivates me
to continue improving these tools.

You can donate via:

- LiberaPay: https://liberapay.com/gdb
- PayPal: https://paypal.me/gdbau
- Zelle: g@b4.vu
