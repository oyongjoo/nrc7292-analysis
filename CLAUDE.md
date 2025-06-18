# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the NRC7292 Software Package for Host mode (Linux OS) - a Linux kernel driver package for the NRC7292 HaLow (IEEE 802.11ah) chipset. The package provides a complete wireless networking solution including kernel driver, firmware, CLI application, and configuration scripts.

## Git Configuration and Commit Guidelines

**CRITICAL**: Always follow these Git settings when making commits:

### Git User Configuration
- **Author**: Liam Lee <oyongjoo@gmail.com>
- **Repository**: oyongjoo/nrc7292-analysis
- **Branch**: main

### Commit Message Guidelines
- **DO NOT** include "Generated with Claude Code" or similar AI attribution in commit messages
- **DO NOT** include "Co-Authored-By: Claude" in commit messages
- Keep commit messages professional and focused on the actual changes
- Use conventional commit format when appropriate

### Example of CORRECT commit message:
```
Add iperf TCP/UDP TX flow analysis

- Updated tx_path_detailed.md and tx_path_detailed_ko.md with comprehensive iperf scenarios
- Added detailed sequence diagrams for TCP and UDP transmission flows
- Created new blog post: NRC7292 iperf TCP/UDP TX Flow Analysis
```

### Example of INCORRECT commit message:
```
🤖 Generated with [Claude Code](https://claude.ai/code)
Co-Authored-By: Claude <noreply@anthropic.com>
```

## Build Commands

### Kernel Driver
```bash
cd package/src/nrc
make clean
make
# Install driver module
cp nrc.ko ~/nrc_pkg/sw/driver/
```

### CLI Application
```bash
cd package/src/cli_app
make clean
make
# Install CLI app
cp cli_app ~/nrc_pkg/script/
```

### FT232H USB-SPI Bridge (optional)
```bash
cd package/src/ft232h-usb-spi
make clean
make
```

## Testing

### Run Test Suite
```bash
# Block ACK tests
cd package/src/nrc/test/block_ack
python testsuit.py

# Network link tests
cd package/src/nrc/test/netlink
python test_ampdu.py
python test_bss_max_idle.py
python testsuit_sec.py
```

### Driver Module Tests
```bash
cd package/src/nrc/test
python insmod_rmmod_test.py
python channel_sweep_test.py
```

## Code Quality

### Kernel Code Style Check
```bash
cd package/src/nrc
make checkstyle
make checkstyle2  # Strict error checking
```

## Architecture

### Core Components

**Kernel Driver (`package/src/nrc/`):**
- `nrc-mac80211.c/h`: mac80211 interface layer
- `nrc-hif-cspi.c/h`: Custom SPI (CSPI) hardware interface
- `nrc-trx.c`: TX/RX packet handling
- `nrc-fw.c/h`: Firmware management
- `nrc-netlink.c/h`: Netlink interface for userspace communication
- `wim.c/h`: Wireless Interface Module protocol
- `nrc-s1g.c/h`: S1G (Sub-1GHz) specific functionality

**CLI Application (`package/src/cli_app/`):**
- `main.c`: CLI entry point
- `cli_cmd.c/h`: Command processing
- `cli_netlink.c/h`: Netlink communication with kernel driver
- `cli_util.c/h`: Utility functions

**EVK Scripts (`package/evk/sw_pkg/nrc_pkg/script/`):**
- `start.py`: Main configuration and startup script
- `stop.py`: Shutdown script
- `conf/`: Region-specific configuration files (US, EU, JP, etc.)

### Hardware Interface Architecture

The driver uses a custom SPI protocol (CSPI) to communicate with the NRC7292 chip:
- **CSPI Layer**: Handles low-level SPI communication
- **WIM Protocol**: Wireless Interface Module protocol for command/response
- **HIF (Hardware Interface)**: Abstraction layer between mac80211 and hardware

### Network Architecture

**Supported Modes:**
- STA (Station): Client mode
- AP (Access Point): Host mode
- MESH: Mesh networking with 802.11s support
- SNIFFER: Packet capture
- IBSS: Ad-hoc networking

**Security Support:**
- Open networks
- WPA2-PSK
- WPA3-SAE (with H2E support)
- WPA3-OWE (Opportunistic Wireless Encryption)
- WPS-PBC (Push Button Configuration)

## Configuration

### EVK Package Installation
```bash
cd package/evk/sw_pkg
chmod +x ./update.sh
./update.sh
```

### Running Network Modes
```bash
cd ~/nrc_pkg/script

# STA mode, Open security, US region
./start.py 0 0 US

# AP mode, WPA2 security, US region  
./start.py 1 1 US

# Sniffer mode, channel 159, US region
./start.py 2 0 US 159 0

# Stop all services
./stop.py
```

### Key Configuration Parameters

**RF Configuration:**
- `max_txpwr`: Maximum TX power in dBm
- `guard_int`: Guard interval ('long' or 'short')

**MAC Configuration:**
- `ampdu_enable`: AMPDU aggregation (0=disable, 1=manual, 2=auto)
- `power_save`: Power save mode for STA (0-3)
- `bss_max_idle`: Keep-alive timeout for AP mode

**Hardware Configuration:**
- `spi_clock`: SPI master clock frequency
- `spi_gpio_irq`: GPIO number for CSPI interrupt

## Important Notes

### SAE Hash-to-Element (H2E)
- H2E is supported but disabled by default for compatibility
- For HaLow certification, add `sae_pwe=1` to hostapd/wpa_supplicant config files
- Requires kernel 5.10.x or later for STA mode

### Regional Compliance
- Board data files in `package/evk/binary/` are specific to NRC7292 EVK
- For custom hardware, use vendor-provided board data files
- Configuration files organized by regulatory domain (US, EU, JP, etc.)

### Development Platform
- Primary target: Raspberry Pi 3 Model B+
- Requires Device Tree overlay installation
- Custom kernel module compilation needed for target kernel version