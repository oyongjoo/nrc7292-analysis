---
layout: post
title: "NRC7292 HaLow Driver Architecture Overview"
date: 2025-06-17
category: Architecture  
tags: [architecture, halow, 802.11ah, kernel-driver, overview]
excerpt: "Comprehensive overview of the NRC7292 HaLow driver architecture, including layer structure, component interactions, and IEEE 802.11ah implementation details."
---

# NRC7292 HaLow Driver Architecture Overview

The NRC7292 is a comprehensive Linux kernel driver package for the IEEE 802.11ah HaLow chipset, designed specifically for IoT applications requiring long-range, low-power wireless connectivity.

## Overall Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    User Space Applications                   │
├─────────────────────────────────────────────────────────────┤
│                     mac80211 Framework                      │
├─────────────────────────────────────────────────────────────┤
│                    NRC7292 Driver Core                      │
│  ┌───────────────┬─────────────────┬─────────────────────┐  │
│  │  mac80211     │      WIM        │    Power Mgmt       │  │
│  │  Interface    │    Protocol     │    & Control        │  │
│  └───────────────┼─────────────────┼─────────────────────┤  │
│  │      TX/RX Processing Path      │   Hardware Abstraction│  │
│  └─────────────────────────────────┼─────────────────────┘  │
├─────────────────────────────────────┼─────────────────────────┤
│                HIF (Hardware Interface)                     │
├─────────────────────────────────────────────────────────────┤
│                  CSPI (Custom SPI)                          │
├─────────────────────────────────────────────────────────────┤
│                    NRC7292 Firmware                        │
└─────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. mac80211 Integration Layer

**File**: `nrc-mac80211.c/h`

The mac80211 interface layer bridges the Linux wireless framework with NRC-specific functionality:

```c
static const struct ieee80211_ops nrc_ops = {
    .tx                 = nrc_mac_tx,
    .start              = nrc_mac_start,
    .stop               = nrc_mac_stop,
    .add_interface      = nrc_mac_add_interface,
    .remove_interface   = nrc_mac_remove_interface,
    .config             = nrc_mac_config,
    .bss_info_changed   = nrc_mac_bss_info_changed,
    .sta_state          = nrc_mac_sta_state,
    .conf_tx            = nrc_mac_conf_tx,
    .set_key            = nrc_mac_set_key,
    // ... additional operations
};
```

**Key Functions:**
- Interface management (STA, AP, Mesh, Monitor modes)
- TX/RX packet handling integration
- Configuration and state management
- Security key management

### 2. WIM (Wireless Interface Module) Protocol

**Files**: `wim.c/h`, `nrc-wim-types.h`

WIM provides the communication protocol between the driver and firmware:

```c
struct wim {
    u8 cmd;           // Command type
    u8 seqno;         // Sequence number
    u16 tlv_len;      // TLV payload length
    u32 flags;        // Control flags
} __packed;
```

**WIM Command Categories:**
- **Configuration**: Channel, power, regulatory settings
- **Station Management**: Association, authentication, key exchange
- **Data Control**: TX/RX parameters, AMPDU settings
- **Debug/Statistics**: Performance monitoring, diagnostics

### 3. Hardware Interface (HIF) Layer

**Files**: `hif.c/h`, `nrc-hif-*.c`

The HIF layer abstracts hardware communication:

```c
struct hif {
    u8 type;        // HIF_TYPE_FRAME, HIF_TYPE_WIM
    u8 subtype;     // Frame subtype
    u16 len;        // Payload length
    s8 vifindex;    // Virtual interface index
} __packed;
```

**Supported Interfaces:**
- **CSPI (Custom SPI)**: Primary interface for NRC7292
- **UART**: Alternative serial interface
- **USB**: Development and testing interface

### 4. TX/RX Processing Engine

**Files**: `nrc-trx.c/h`

Handles packet transmission and reception with advanced features:

#### TX Path Features:
- **Credit-based flow control**: Hardware buffer management
- **TX tasklet processing**: Bottom-half sequential transmission
- **AMPDU Block ACK**: Automatic aggregation management
- **Handler chain**: Modular processing pipeline

#### RX Path Features:
- **Multi-threaded processing**: Workqueue-based RX handling
- **Frame filtering**: Protocol-specific filtering
- **Statistics collection**: Performance monitoring

### 5. Power Management

**Files**: `nrc-pm.c/h`

Comprehensive power management for IoT applications:

```c
enum NRC_PS_MODE {
    NRC_PS_NONE,               // No power save
    NRC_PS_MODEMSLEEP,         // Modem sleep mode
    NRC_PS_DEEPSLEEP_TIM,      // Deep sleep with TIM
    NRC_PS_DEEPSLEEP_NONTIM    // Deep sleep without TIM
};
```

**Power Save Features:**
- **Dynamic power scaling**: Adaptive power based on traffic
- **BSS max idle**: Keep-alive mechanism for AP mode
- **Target wake time**: Scheduled wake-up coordination
- **Listen interval**: STA sleep scheduling

## Data Flow Architecture

### TX Data Flow

```
mac80211 → nrc_mac_tx() → TX handlers → Credit check → 
WIM encapsulation → HIF framing → CSPI transmission → Firmware
```

### RX Data Flow

```
Firmware → CSPI reception → HIF parsing → WIM processing → 
RX handlers → Frame validation → mac80211 indication
```

## Memory Management

### SKB (Socket Buffer) Processing

The driver uses Linux SKB structures for efficient packet handling:

```c
// TX: Add headers using headroom
hif = (void *)skb_push(skb, sizeof(struct hif));
wim = (void *)skb_push(skb, sizeof(struct wim));

// RX: Remove headers
skb_pull(skb, sizeof(struct hif));
skb_pull(skb, sizeof(struct wim));
```

### Driver Private Structures

Each mac80211 structure has associated driver-private data:

```c
#define to_i_vif(v)  ((struct nrc_vif *) (v)->drv_priv)
#define to_i_sta(s)  ((struct nrc_sta *) (s)->drv_priv)
```

## Hardware Abstraction

### CSPI (Custom SPI) Protocol

```c
/*
 * CSPI Command Format:
 * [31:24]: Start byte (0x50)
 * [23:23]: Burst mode
 * [22:22]: Direction (read/write)
 * [21:21]: Address mode (fixed/incremental)
 * [20:13]: Register address
 * [12:0]:  Transfer length
 */
#define C_SPI_WRITE     0x50400000
#define C_SPI_READ      0x50000000
#define C_SPI_BURST     0x00800000
```

### Interrupt Handling

```c
// GPIO-based interrupt for CSPI
static irqreturn_t nrc_hif_isr(int irq, void *dev_id)
{
    struct nrc_hif_device *hdev = dev_id;
    
    // Disable interrupt
    disable_irq_nosync(hdev->irq);
    
    // Schedule bottom-half processing
    schedule_work(&hdev->work);
    
    return IRQ_HANDLED;
}
```

## Supported Network Modes

### 1. Station (STA) Mode
- **Infrastructure connectivity**: Connect to existing APs
- **Power management**: Advanced sleep scheduling
- **Roaming support**: Seamless AP transitions

### 2. Access Point (AP) Mode  
- **Client support**: Up to 8192 associated clients
- **Security**: WPA2/WPA3 with hardware acceleration
- **QoS management**: 4-AC traffic prioritization

### 3. Mesh Networking
- **IEEE 802.11s compliance**: Standard mesh protocols
- **HWMP routing**: Hybrid wireless mesh protocol
- **Self-healing**: Automatic path recovery

### 4. Monitor Mode
- **Packet capture**: All frame types
- **Radiotap headers**: Detailed RF information
- **Real-time analysis**: Low-latency monitoring

## IEEE 802.11ah Specific Features

### S1G (Sub-1GHz) Support

```c
// S1G channel configuration
struct s1g_channel_info {
    u16 center_freq;     // Center frequency in MHz
    u8 bandwidth;        // 1, 2, 4, 8, 16 MHz
    u8 primary_channel;  // Primary channel number
    u8 channel_width;    // Channel width index
};
```

### Extended Range Features
- **1km+ communication range**: 10x improvement over 2.4GHz
- **Building penetration**: Enhanced signal propagation
- **Interference resistance**: Sub-1GHz band advantages

### IoT Optimizations
- **Massive device support**: 8192 devices per AP
- **Ultra-low power**: Years of battery operation
- **Small data efficiency**: Optimized for sensor data

## Configuration and Management

### Module Parameters

```c
// Key module parameters
module_param(fw_name, charp, 0444);           // Firmware file
module_param(bd_name, charp, 0600);           // Board data file  
module_param(hifspeed, int, 0600);            // Interface speed
module_param(spi_gpio_irq, int, 0600);        // GPIO interrupt pin
module_param(power_save, int, 0600);          // Power save mode
```

### Netlink Interface

**File**: `nrc-netlink.c/h`

Provides userspace control interface:

```c
// Netlink command types
enum NRC_NL_COMMANDS {
    NRC_NL_CMD_SET_CONFIG,
    NRC_NL_CMD_GET_STATUS,
    NRC_NL_CMD_TRIGGER_SCAN,
    NRC_NL_CMD_SET_AMPDU,
    // ... additional commands
};
```

## Testing and Validation

### Test Framework Structure

```
test/
├── block_ack/          # AMPDU and BA testing
├── netlink/            # Netlink interface tests  
├── vendor/             # Vendor-specific tests
├── insmod_rmmod_test.py    # Module lifecycle testing
└── channel_sweep_test.py   # Channel performance testing
```

### Comprehensive Testing Coverage
- **Protocol compliance**: IEEE 802.11ah standard verification
- **Performance testing**: Throughput, latency, range testing
- **Stress testing**: Long-duration stability validation
- **Interoperability**: Multi-vendor device testing

## Conclusion

The NRC7292 driver architecture demonstrates a well-designed, layered approach optimized for IEEE 802.11ah HaLow applications. Key architectural strengths include:

1. **Clean Layer Separation**: Clear interfaces between components
2. **mac80211 Integration**: Full compliance with Linux wireless framework
3. **Efficient Data Flow**: Zero-copy optimization and hardware acceleration
4. **Comprehensive Power Management**: IoT-focused energy efficiency
5. **Extensive Testing**: Production-quality validation framework

This architecture effectively bridges the gap between Linux networking stack and specialized HaLow hardware, providing a robust foundation for IoT deployment scenarios requiring long-range, low-power wireless connectivity.

---

*For detailed component analysis and source code insights, explore the complete [NRC7292 analysis documentation](https://github.com/oyongjoo/nrc7292-analysis).*