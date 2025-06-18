# NRC7292 HaLow Driver Component Interactions

This document provides detailed component interaction diagrams for the NRC7292 HaLow driver, showing how major components communicate and data flows through the system.

## Architecture Overview

The NRC7292 driver implements a layered architecture with clear separation between kernel and userspace components:

```
 ┌─────────────────────────────────────────────────────────────────┐
 │                    USERSPACE APPLICATIONS                       │
 ├─────────────────────────────────────────────────────────────────┤
 │  hostapd/wpa_supplicant  │  CLI Application  │  Python Scripts  │
 ├─────────────────────────────────────────────────────────────────┤
 │              Linux Kernel Network Stack                         │
 ├─────────────────────────────────────────────────────────────────┤
 │                     mac80211 Subsystem                          │
 └─────────────────────────────────────────────────────────────────┘
 ┌─────────────────────────────────────────────────────────────────┐
 │                   NRC7292 KERNEL DRIVER                         │
 ├─────────────────────────────────────────────────────────────────┤
 │  mac80211 Interface  │  Netlink Interface  │  TRX Handler      │
 ├─────────────────────────────────────────────────────────────────┤
 │         WIM Protocol Layer (Wireless Interface Module)          │
 ├─────────────────────────────────────────────────────────────────┤
 │                 HIF Layer (Hardware Interface)                  │
 ├─────────────────────────────────────────────────────────────────┤
 │             CSPI Layer (Custom Serial Peripheral Interface)     │
 └─────────────────────────────────────────────────────────────────┘
 ┌─────────────────────────────────────────────────────────────────┐
 │                  NRC7292 HARDWARE CHIP                          │
 │                    (Firmware + Radio)                           │
 └─────────────────────────────────────────────────────────────────┘
```

## 1. Component Interaction Overview

### Core Component Structure

```
┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│   nrc-mac80211   │◄──►│  nrc-netlink     │◄──►│   CLI App        │
│   (mac80211      │    │  (Kernel-User    │    │   (Userspace     │
│    interface)    │    │   communication) │    │    commands)     │
└──────────────────┘    └──────────────────┘    └──────────────────┘
          │                        │                        │
          ▼                        ▼                        │
┌──────────────────┐    ┌──────────────────┐               │
│    nrc-trx       │◄──►│      wim.c       │               │
│  (TX/RX packet   │    │  (WIM Protocol   │               │
│    handling)     │    │    messages)     │               │
└──────────────────┘    └──────────────────┘               │
          │                        │                        │
          ▼                        ▼                        │
┌──────────────────┐    ┌──────────────────┐               │
│     nrc-hif      │◄──►│   nrc-fw         │               │
│ (Hardware        │    │  (Firmware       │               │
│  Interface)      │    │   management)    │               │
└──────────────────┘    └──────────────────┘               │
          │                                                 │
          ▼                                                 │
┌──────────────────┐                                       │
│  nrc-hif-cspi    │                                       │
│  (Custom SPI     │◄──────────────────────────────────────┘
│   hardware)      │
└──────────────────┘
          │
          ▼
┌──────────────────┐
│  NRC7292 Chip    │
│   (Hardware)     │
└──────────────────┘
```

## 2. Layer-by-Layer Interactions

### 2.1 mac80211 to HIF Layer Communication

```
┌─ mac80211 Layer ─────────────────────────────────────────────────────┐
│                                                                      │
│  nrc_mac_tx() ──────┐                                               │
│  nrc_mac_rx() ──────┤                                               │
│  nrc_mac_conf_tx()──┤                                               │
│  nrc_mac_sta_*() ───┘                                               │
└──────────────────────┼───────────────────────────────────────────────┘
                       │
┌─ TRX Layer ──────────▼───────────────────────────────────────────────┐
│                                                                      │
│  nrc_trx_handler structures:                                        │
│  • TX handlers: Authentication, encryption, queue management        │
│  • RX handlers: Decryption, packet processing, mac80211 delivery    │
│                                                                      │
│  Flow: TXH(handler_func, VIF_MASK) / RXH(handler_func, VIF_MASK)    │
└──────────────────────┼───────────────────────────────────────────────┘
                       │
┌─ WIM Layer ──────────▼───────────────────────────────────────────────┐
│                                                                      │
│  struct wim {                                                       │
│    u16 cmd/resp/event;  /* Command/Response/Event ID */             │
│    u8  seqno;          /* Sequence number for matching */           │
│    u8  n_tlvs;         /* Number of TLV parameters */               │
│    u8  payload[0];     /* TLV parameters */                         │
│  }                                                                   │
│                                                                      │
│  nrc_wim_alloc_skb() ──► nrc_wim_skb_add_tlv() ──► nrc_xmit_wim_*() │
└──────────────────────┼───────────────────────────────────────────────┘
                       │
┌─ HIF Layer ──────────▼───────────────────────────────────────────────┐
│                                                                      │
│  struct hif {                                                       │
│    u8  type;           /* HIF_TYPE_FRAME or HIF_TYPE_WIM */          │
│    u8  subtype;        /* Frame subtype or WIM subtype */           │
│    u8  flags;          /* Control flags */                          │
│    s8  vifindex;       /* Virtual interface index */                │
│    u16 len;            /* Payload length */                         │
│    u16 tlv_len;        /* TLV section length */                     │
│    u8  payload[0];     /* Actual data */                            │
│  }                                                                   │
└──────────────────────┼───────────────────────────────────────────────┘
                       │
┌─ CSPI Layer ─────────▼───────────────────────────────────────────────┐
│                                                                      │
│  Custom SPI Protocol:                                               │
│  • C_SPI_WAKE_UP (0x0)     • C_SPI_DEVICE_STATUS (0x1)             │
│  • C_SPI_EIRQ_STATUS (0x13) • C_SPI_QUEUE_STATUS (0x14)            │
│  • C_SPI_MESSAGE (0x20)     • C_SPI_TXQ_WINDOW (0x41)              │
│                                                                      │
│  IRQ-driven communication with target device                        │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow Sequence

```
TX Path (Host → Target):
========================

User Data
    │
    ▼ (via mac80211)
nrc_mac_tx()
    │
    ▼ (TX handlers)
nrc_trx_handler chain
    │ (process authentication, encryption, etc.)
    ▼
WIM message creation
    │ (nrc_wim_alloc_skb, add TLVs)
    ▼
HIF header addition
    │ (type=HIF_TYPE_FRAME, vifindex, etc.)
    ▼
CSPI transmission
    │ (nrc_hif_cspi write operations)
    ▼
NRC7292 Chip


RX Path (Target → Host):
========================

NRC7292 Chip
    │
    ▼ (CSPI IRQ)
CSPI reception
    │ (IRQ handler, queue status check)
    ▼
HIF processing
    │ (parse HIF header, route by type)
    ▼
WIM/Frame processing
    │ (nrc_wim_rx or frame handler)
    ▼
nrc_trx_handler chain
    │ (RX handlers: decrypt, validate, etc.)
    ▼
nrc_mac_rx()
    │
    ▼ (to mac80211)
Linux Network Stack
```

## 3. TX/RX Operation Details

### 3.1 TX Operation Flow

```
nrc_mac_tx()
    │
    ├─► Validate VIF and parameters
    │
    ├─► Determine TX handler chain based on:
    │   • Frame type (data/mgmt/ctrl)
    │   • VIF type (STA/AP/Mesh/Monitor)
    │   • Security requirements
    │
    ├─► Execute TX handlers in sequence:
    │   ┌────────────────────────────────────────┐
    │   │ TXH(nrc_tx_frame_check, NL80211_IFTYPE_ALL) │
    │   │ TXH(nrc_tx_8023_to_80211, BIT(NL80211_IFTYPE_AP)) │
    │   │ TXH(nrc_tx_mesh_control, BIT(NL80211_IFTYPE_MESH_POINT)) │
    │   │ TXH(nrc_tx_ampdu_session, NL80211_IFTYPE_ALL) │
    │   └────────────────────────────────────────┘
    │
    ├─► Convert to WIM frame message:
    │   • Allocate WIM SKB: nrc_wim_alloc_skb()
    │   • Add frame TLVs: channel, rates, power, etc.
    │   • Set frame sequence number
    │
    ├─► Queue management:
    │   • Check credit availability per AC queue
    │   • Update pending frame counters
    │   • Handle flow control
    │
    └─► Transmit via HIF:
        • Add HIF header (type=HIF_TYPE_FRAME)
        • Submit to CSPI layer
        • Handle completion/acknowledgment
```

### 3.2 RX Operation Flow

```
CSPI IRQ Handler
    │
    ├─► Read EIRQ status register
    ├─► Check RX queue status
    ├─► Read message from C_SPI_MESSAGE
    │
    ▼
nrc_hif_rx()
    │
    ├─► Parse HIF header
    ├─► Route by HIF type:
    │   ├─► HIF_TYPE_FRAME → Frame processing
    │   ├─► HIF_TYPE_WIM → WIM message processing  
    │   └─► HIF_TYPE_LOG → Debug log processing
    │
    ▼ (for frames)
Frame Processing
    │
    ├─► Extract radiotap header information
    ├─► Determine RX handler chain
    ├─► Execute RX handlers:
    │   ┌────────────────────────────────────────┐
    │   │ RXH(nrc_rx_monitor, BIT(NL80211_IFTYPE_MONITOR)) │
    │   │ RXH(nrc_rx_mesh_control, BIT(NL80211_IFTYPE_MESH_POINT)) │
    │   │ RXH(nrc_rx_sta_mgmt, NL80211_IFTYPE_ALL) │
    │   │ RXH(nrc_rx_defrag, NL80211_IFTYPE_ALL) │
    │   └────────────────────────────────────────┘
    │
    ├─► Decrypt if needed
    ├─► Update statistics
    │
    └─► Deliver to mac80211:
        nrc_mac_rx() → ieee80211_rx_irqsafe()
```

## 4. WIM Protocol Command/Response Flow

### 4.1 WIM Message Structure

```
WIM Message Format:
┌─────────────────────────────────────────────────────────────┐
│                    HIF Header                               │
├─────────────────────────────────────────────────────────────┤
│ type=HIF_TYPE_WIM │ subtype │ flags │ vifindex │ len │ tlv_len │
├─────────────────────────────────────────────────────────────┤
│                    WIM Header                               │
├─────────────────────────────────────────────────────────────┤
│    cmd/resp/event    │    seqno    │    n_tlvs    │         │
├─────────────────────────────────────────────────────────────┤
│                    TLV Parameters                           │
├─────────────────────────────────────────────────────────────┤
│  TLV1: │  Type  │ Length │         Value                    │
│  TLV2: │  Type  │ Length │         Value                    │
│   ...  │   ...  │   ...  │          ...                    │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 WIM Command/Response Mechanism

```
Command Initiation:
===================

Host Driver                           Target Firmware
     │                                        │
     ├─► nrc_wim_alloc_skb(WIM_CMD_SET)      │
     ├─► nrc_wim_skb_add_tlv(CHANNEL)        │
     ├─► nrc_wim_skb_add_tlv(TX_POWER)       │
     ├─► nrc_xmit_wim_request()              │
     │                                       │
     │   ──── WIM REQUEST (seqno=N) ────────►│
     │                                       │ Process command
     │                                       │ Update configuration
     │                                       │
     │◄──── WIM RESPONSE (seqno=N) ──────    │
     │                                       │
     ├─► Match sequence number               │
     ├─► Process response TLVs               │
     └─► Complete operation                  │

Synchronous Command (with wait):
===============================

nrc_xmit_wim_request_wait()
     │
     ├─► Send WIM request
     ├─► wait_for_completion_timeout(&nw->wim_responded)
     │   (blocks until response or timeout)
     │
     └─► Return response SKB or timeout error

Event Processing:
================

Target Firmware                       Host Driver
     │                                        │
     │   ──── WIM EVENT (SCAN_COMPLETED) ────►│
     │                                        │
     │                                        ├─► nrc_wim_rx()
     │                                        ├─► Process event type
     │                                        └─► Notify mac80211
```

### 4.3 Key WIM Commands and Their Flow

```
Station Association Flow:
========================

1. nrc_wim_set_sta_type()
   ├─► WIM_CMD_SET + WIM_TLV_STA_TYPE
   └─► Configure device as STA/AP/Mesh

2. nrc_wim_set_mac_addr()
   ├─► WIM_CMD_SET + WIM_TLV_MACADDR
   └─► Set interface MAC address

3. nrc_wim_install_key()
   ├─► WIM_CMD_SET_KEY + WIM_TLV_KEY_PARAM
   └─► Install encryption keys

4. nrc_wim_change_sta()
   ├─► WIM_CMD_STA_CMD + station parameters
   └─► Add/remove/modify station

Scanning Flow:
=============

1. nrc_wim_hw_scan()
   ├─► WIM_CMD_SCAN_START
   ├─► WIM_TLV_SCAN_PARAM (channels, SSIDs)
   ├─► WIM_TLV_SCAN_BAND_IE (per-band IEs)
   └─► WIM_TLV_SCAN_PROBE_REQ_IE (probe request IEs)

2. Target performs scan
   └─► Multiple WIM_EVENT_SCAN_COMPLETED events

3. Scan completion
   └─► Final event with scan results
```

## 5. Netlink Communication Architecture

### 5.1 Kernel-Userspace Communication

```
┌─────────────────────────────────────────────────────────────┐
│                    USERSPACE                                │
├─────────────────────────────────────────────────────────────┤
│  CLI Application        Python Test Scripts                 │
│  ├─ cli_netlink.c      ├─ nrcnetlink.py                     │
│  ├─ netlink_send_data  ├─ shell.py                          │
│  └─ Command parsing    └─ Test automation                   │
├─────────────────────────────────────────────────────────────┤
│                 Netlink Socket Layer                        │
├─────────────────────────────────────────────────────────────┤
│                   KERNEL SPACE                              │
├─────────────────────────────────────────────────────────────┤
│  nrc-netlink.c                                              │
│  ├─ Generic Netlink Family: "NRC-NL-FAM"                   │
│  ├─ Command handlers for each NL_* operation               │
│  ├─ Multicast groups: WFA_CAPI_RESPONSE, NRC_LOG           │
│  └─ Direct WIM command translation                         │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 Netlink Command Processing Flow

```
CLI Application Example:
=======================

User Command: cli_app set ampdu_mode 2
     │
     ▼
netlink_send_data(NL_SHELL_RUN, "set ampdu_mode 2")
     │
     ▼ (Netlink socket)
Kernel: nrc_netlink_shell_run()
     │
     ├─► Parse command string
     ├─► Validate parameters
     ├─► Convert to WIM command:
     │   └─► WIM_CMD_SET + WIM_TLV_AMPDU_MODE
     ├─► nrc_xmit_wim_request_wait()
     └─► Return response to userspace

Python Script Example:
=====================

halow_ampdu.py calls:
     │
     ▼
nrcnetlink.send_msg(NL_WFA_CAPI_SEND_ADDBA, params)
     │
     ▼ (Netlink socket)
Kernel: nrc_netlink_send_addba()
     │
     ├─► Extract TID, destination MAC
     ├─► Find target station
     ├─► Call nrc_wim_ampdu_action()
     │   └─► WIM_CMD_AMPDU_ACTION + parameters
     └─► Send Block ACK request to target
```

### 5.3 Netlink Message Categories

```
Testing/Debug Commands:
======================
• NL_SHELL_RUN → Direct firmware shell commands
• NL_SHELL_RUN_RAW → Raw command execution
• NL_MIC_SCAN → MIC failure testing
• NL_FRAME_INJECTION → Inject custom frames

WFA CAPI Commands:
==================
• NL_WFA_CAPI_STA_GET_INFO → Station information
• NL_WFA_CAPI_STA_SET_11N → 802.11n configuration
• NL_WFA_CAPI_SEND_ADDBA → Block ACK setup
• NL_WFA_CAPI_SEND_DELBA → Block ACK teardown
• NL_WFA_CAPI_BSS_MAX_IDLE → Keep-alive configuration

HaLow Specific Commands:
=======================
• NL_HALOW_SET_DUT → Device Under Test mode
• NL_CLI_APP_GET_INFO → Driver information
• NL_AUTO_BA_TOGGLE → Auto Block ACK control
```

## 6. Hardware Interface (CSPI) Details

### 6.1 CSPI Register Map and Operations

```
CSPI Register Layout:
====================

┌─ System Registers (0x0-0xF) ─────────────────────────────────┐
│ 0x0: C_SPI_WAKE_UP        │ 0x1: C_SPI_DEVICE_STATUS       │
│ 0x2: C_SPI_CHIP_ID_HIGH   │ 0x3: C_SPI_CHIP_ID_LOW         │
│ 0x4: C_SPI_MODEM_ID       │ 0x8: C_SPI_SOFTWARE_VERSION    │
│ 0xC: C_SPI_BOARD_ID       │                                 │
├─ IRQ Control (0x10-0x13) ───────────────────────────────────┤
│ 0x10: C_SPI_EIRQ_MODE     │ 0x11: C_SPI_EIRQ_ENABLE        │
│ 0x12: C_SPI_EIRQ_STATUS_LATCH │ 0x13: C_SPI_EIRQ_STATUS   │
├─ Queue Status (0x14-0x1F) ──────────────────────────────────┤
│ 0x14: C_SPI_QUEUE_STATUS  │ (contains TX/RX queue info)    │
├─ Message Interface (0x20-0x2F) ─────────────────────────────┤
│ 0x20: C_SPI_MESSAGE       │ (data transfer register)       │
├─ Queue Configuration (0x30-0x41) ───────────────────────────┤
│ 0x30: C_SPI_RXQ_THRESHOLD │ 0x31: C_SPI_RXQ_WINDOW         │
│ 0x40: C_SPI_TXQ_THRESHOLD │ 0x41: C_SPI_TXQ_WINDOW         │
└──────────────────────────────────────────────────────────────┘
```

### 6.2 CSPI Communication Flow

```
Initialization Sequence:
=======================

nrc_hif_cspi_init()
     │
     ├─► Configure SPI bus parameters
     ├─► Setup GPIO for interrupt
     ├─► Register IRQ handler
     │
     ├─► Device Detection:
     │   ├─► Read C_SPI_CHIP_ID → Verify NRC7292
     │   ├─► Read C_SPI_MODEM_ID → Check modem type
     │   └─► Read C_SPI_SOFTWARE_VERSION
     │
     ├─► Configure EIRQ:
     │   ├─► Set C_SPI_EIRQ_MODE = CSPI_EIRQ_MODE
     │   └─► Set C_SPI_EIRQ_ENABLE = CSPI_EIRQ_A_ENABLE
     │
     └─► Start RX thread for message processing

Data Transmission:
==================

nrc_hif_xmit(skb)
     │
     ├─► Check device ready state
     ├─► Acquire SPI lock
     │
     ├─► Write message:
     │   ├─► Calculate message length
     │   ├─► Write to C_SPI_MESSAGE register
     │   └─► Handle chunked transfer if needed
     │
     ├─► Update TX queue counters
     └─► Release SPI lock

IRQ-Driven Reception:
====================

GPIO IRQ → cspi_irq_handler()
     │
     ├─► Read C_SPI_EIRQ_STATUS
     ├─► Check for RX queue interrupt
     │
     ├─► If RX data available:
     │   ├─► Read C_SPI_QUEUE_STATUS
     │   ├─► Calculate message length
     │   ├─► Read from C_SPI_MESSAGE
     │   └─► Queue for processing
     │
     ├─► Check for device status changes
     └─► Clear interrupt status
```

### 6.3 Credit-Based Flow Control

```
TX Credit Management:
====================

Per-AC Credit Tracking:
┌─────────────────────────────────────────────────────────┐
│ atomic_t tx_credit[IEEE80211_NUM_ACS*3]:               │
│ • AC0 (BK): CREDIT_AC0 = 4 credits                     │
│ • AC1 (BE): CREDIT_AC1 = 40 credits (or 20/80 variant) │
│ • AC2 (VI): CREDIT_AC2 = 8 credits                     │
│ • AC3 (VO): CREDIT_AC3 = 8 credits                     │
└─────────────────────────────────────────────────────────┘

Credit Flow:
Host Driver                           Target Firmware
     │                                        │
     ├─► Check available credits              │
     ├─► If credits > 0:                      │
     │   ├─► Decrement credit counter         │
     │   └─► Transmit frame                   │
     │                                        │
     │   ──── Frame with HIF header ─────────►│
     │                                        │ Process frame
     │                                        │ Consume buffer
     │                                        │
     │◄──── WIM_EVENT_CREDIT_REPORT ─────────│
     │                                        │
     ├─► Parse credit report                  │
     └─► Restore credits per AC               │

Credit Report Event Structure:
┌─────────────────────────────────────────────────────────┐
│ WIM_EVENT_CREDIT_REPORT                                 │
│ ├─► WIM_TLV_AC_CREDIT_REPORT                           │
│     ├─► AC0 completed frames count                     │
│     ├─► AC1 completed frames count                     │
│     ├─► AC2 completed frames count                     │
│     └─► AC3 completed frames count                     │
└─────────────────────────────────────────────────────────┘
```

## 7. Error Handling and Recovery

### 7.1 Recovery Mechanisms

```
Firmware Recovery Flow:
======================

Detection:
├─► Watchdog timeout (TARGET_NOTI_WDT_EXPIRED)
├─► Communication failure
├─► Netlink recovery command (NL_CMD_RECOVERY)
└─► Manual recovery trigger

Recovery Process:
nrc_netlink_trigger_recovery()
     │
     ├─► Set driver state to recovery mode
     ├─► Stop all TX queues
     ├─► Flush pending operations
     │
     ├─► Reset hardware interface:
     │   ├─► nrc_hif_reset_device()
     │   ├─► Clear IRQ status
     │   └─► Reinitialize CSPI registers
     │
     ├─► Reload firmware:
     │   ├─► WIM_CMD_REQ_FW
     │   └─► Wait for WIM_EVENT_READY
     │
     ├─► Restore configuration:
     │   ├─► VIF settings
     │   ├─► Station parameters
     │   ├─► Security keys
     │   └─► Channel configuration
     │
     └─► Resume normal operations
```

### 7.2 Power Management Integration

```
Power Save Flow:
===============

Host-Initiated Sleep:
nrc_hif_sleep_target_prepare()
     │
     ├─► Stop TX tasklet
     ├─► Flush pending frames
     ├─► Send WIM_CMD_SLEEP
     │   └─► WIM_TLV_SLEEP_DURATION
     │
     ├─► Wait for WIM_EVENT_PS_READY
     ├─► Suspend CSPI interrupts
     └─► Enter low power state

Target-Initiated Wakeup:
GPIO IRQ (device ready signal)
     │
     ├─► nrc_hif_wake_target()
     ├─► Check C_SPI_DEVICE_STATUS
     ├─► Process TARGET_NOTI_FW_READY_FROM_PS
     ├─► Resume CSPI operations
     └─► Restart TX tasklet
```

## Conclusion

The NRC7292 driver implements a sophisticated layered architecture with clear separation of concerns:

1. **mac80211 Interface Layer**: Provides standard Linux wireless interface
2. **TRX Handler Layer**: Manages packet processing with configurable handler chains  
3. **WIM Protocol Layer**: Implements command/response/event communication
4. **HIF Layer**: Provides hardware abstraction with pluggable interfaces
5. **CSPI Layer**: Handles low-level SPI communication with the NRC7292 chip

The design enables:
- **Modularity**: Each layer has well-defined interfaces
- **Extensibility**: Handler chains allow feature addition
- **Reliability**: Credit-based flow control and recovery mechanisms
- **Testability**: Netlink interface enables comprehensive testing
- **Power Efficiency**: Integrated power management with firmware coordination

This architecture supports the full range of HaLow operations including STA/AP modes, mesh networking, security protocols, and power management while maintaining compatibility with standard Linux networking tools.