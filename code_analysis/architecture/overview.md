# NRC7292 Driver Architecture Overview

## Overall Structure

```
Linux Kernel Space
├── mac80211 subsystem
│   └── nrc-mac80211.c/.h     # mac80211 interface layer
├── Network stack
│   └── cfg80211              # Wireless configuration framework
└── Hardware abstraction
    ├── nrc-hif-cspi.c/.h     # CSPI hardware interface
    ├── nrc-trx.c             # TX/RX packet processing
    └── wim.c/.h              # WIM protocol implementation

User Space
├── CLI application
│   └── cli_app/              # Configuration and management tools
└── Configuration scripts
    └── start.py/stop.py      # Network mode control
```

## Main Components

### 1. MAC Layer (nrc-mac80211.c)
- Interface with Linux mac80211
- Virtual interface management (up to 2)
- Station/AP/Mesh mode support

### 2. Hardware Interface (HIF)
- CSPI (Custom SPI) protocol
- Interrupt and polling mode support
- Communication with firmware

### 3. WIM Protocol
- Command/response based communication
- Firmware control and configuration
- State management and synchronization

### 4. Power Management
- Various power saving modes
- Dynamic power scaling
- Deep sleep support

## Data Flow

1. **Transmit**: User data → mac80211 → nrc-trx → HIF → firmware
2. **Receive**: Firmware → HIF → nrc-trx → mac80211 → network stack