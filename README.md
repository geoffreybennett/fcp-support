# Linux FCP (Focusrite Control Protocol) Support

This repository provides the user-space components required for full
functionality of the Focusrite USB audio interfaces that use the Linux
FCP driver: the Scarlett 4th Gen 16i16, 18i16 and 18i20, and the ISA
C8X.

## Quick Start

If you don't have to build from source:

1. Install the RPM or deb from the
   [Releases](https://github.com/geoffreybennett/fcp-support/releases)
   page, and put your user in the `audio` group:

   ```bash
   sudo usermod -a -G audio $USER
   ```

   Log out and back in for the group change to take effect.

   If you have installed from source before, run `make uninstall` in
   that checkout first. Source builds install to `/usr/local/bin`,
   which comes before `/usr/bin` in `PATH`, so the old binaries shadow
   the packaged ones. `which -a fcp-tool` shows whether this has
   happened.

2. The FCP driver comes with the kernel. The Scarlett 4th Gen 16i16,
   18i16 and 18i20 need Linux 6.14 or later; the ISA C8X needs 6.18.39,
   7.1.4, or 7.2+. Check with `uname -r`.

   Debian 13 (trixie) ships 6.12, so a newer kernel is required from
   backports:

   ```bash
   echo 'deb http://deb.debian.org/debian trixie-backports main' |
     sudo tee /etc/apt/sources.list.d/trixie-backports.list
   sudo apt update
   sudo apt install -t trixie-backports linux-image-amd64
   ```

   Ubuntu 24.04 ships 6.8, so it needs the hardware enablement kernel:

   ```bash
   sudo apt install linux-generic-hwe-24.04
   ```

   Reboot into the new kernel.

3. Plug in the device and check that it was detected:

   ```bash
   fcp-tool
   ```

   It should report your interface, its card number, and the
   installed firmware version.

4. Install and run
   [alsa-scarlett-gui](https://github.com/geoffreybennett/alsa-scarlett-gui).
   It controls the device and offers firmware updates when one is
   available.

The rest of this page covers building from source, the command-line
tools, and troubleshooting.

## System Overview

FCP support in Linux consists of several components that work together:

1. **Kernel Driver** ([linux-fcp](https://github.com/geoffreybennett/linux-fcp))
   - Allows `fcp-server` to communicate with the device; comes with
     the kernel, so there is nothing to install

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
- ISA C8X

The FCP driver itself supports all Focusrite USB audio interfaces
since the 2nd Gen Scarletts, so support for the other devices will be
added here later. Until then, the Linux Scarlett2 driver supports all
the other 2nd Gen and later Scarlett, Clarett USB, Clarett+, and
Vocaster devices.

## Prerequisites

1. A kernel with the FCP driver for your device, as listed in the
   Quick Start above. Check with `uname -r`.

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

## Installation

Build from source:

1. Download, build and install:

```bash
git clone https://github.com/geoffreybennett/fcp-support.git
cd fcp-support
make
sudo make install
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

Firmware is distributed separately, from
[scarlett4-firmware](https://github.com/geoffreybennett/scarlett4-firmware).
RPM and deb packages are on its
[Releases](https://github.com/geoffreybennett/scarlett4-firmware/releases)
page, and Arch users can install `scarlett4-firmware` from the AUR. To
install the files by hand instead, put the `.bin` files in
`/usr/lib/firmware/scarlett4`, which is where `fcp-tool` and
`alsa-scarlett-gui` look for them. `fcp-tool list-all` reports the
versions it finds.

`fcp-tool` manages firmware from the command line:

```bash
# View all commands
fcp-tool help

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
