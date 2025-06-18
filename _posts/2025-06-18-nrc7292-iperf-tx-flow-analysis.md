---
layout: post
title: "NRC7292 iperf TCP/UDP TX Flow Analysis"
date: 2025-06-18 14:00:00 +0900
categories: [Protocol, Performance]
excerpt: "Detailed analysis of NRC7292 HaLow driver TX path with iperf TCP/UDP transmission scenarios, including sequence diagrams and function-level flow tracking."
---

# NRC7292 iperf TCP/UDP TX Flow Analysis

## Overview

This post provides a comprehensive analysis of the NRC7292 HaLow driver's TX (transmission) path when handling iperf TCP and UDP traffic. We'll examine the complete data flow from application-level iperf commands down to the CSPI hardware interface, tracking each function call and transmission completion mechanism.

## iperf Test Commands

```bash
# TCP transmission test
iperf3 -c 192.168.1.100 -t 30 -i 1

# UDP transmission test  
iperf3 -c 192.168.1.100 -u -b 10M -t 30 -i 1
```

## TCP Data Transmission Flow

The following sequence diagram shows the complete TCP transmission flow through the NRC7292 driver:

### TCP Transmission Flow Steps:

1. **iperf3 Application** → `write()` system call
2. **Network Stack** → TCP segmentation and IP header addition → `dev_queue_xmit()`
3. **mac80211** → `ieee80211_tx()` call to NRC driver
4. **NRC Driver** → `nrc_mac_tx()` [nrc-trx.c:75] - TX entry point
5. **NRC Driver** → `setup_ba_session()` [nrc-trx.c:229] - AMPDU BA session setup
6. **NRC Driver** → TX handler chain execution
7. **NRC Driver** → `tx_h_put_qos_control()` [nrc-trx.c:579] - Convert to QoS data
8. **Credit Check** → If sufficient:
   - `nrc_xmit_frame()` [hif.c:711]
   - `nrc_hif_enqueue()` [hif.c:98] 
   - `nrc_hif_work()` [hif.c:177]
   - `nrc_hif_xmit()` [nrc-hif-cspi.c:403]
9. **HIF Layer** → C-SPI command transmission to firmware
10. **Firmware** → IEEE 802.11ah frame transmission to hardware
11. **Hardware** → TX completion interrupt back to firmware
12. **Firmware** → WIM Credit Report back to HIF
13. **HIF Layer** → `nrc_kick_txq()` [nrc-mac80211.c:587]
14. **NRC Driver** → `nrc_tx_tasklet()` [nrc-mac80211.c:545] - Process pending packets

## UDP Data Transmission Flow

UDP transmission follows a similar path but with optimizations for high-throughput scenarios:

### UDP Transmission Flow Steps:

1. **iperf3 Application** → `sendto()` system call
2. **Network Stack** → UDP header and IP header addition → `dev_queue_xmit()`
3. **mac80211** → `ieee80211_tx()` call to NRC driver
4. **NRC Driver** → `nrc_mac_tx()` [nrc-trx.c:75]
5. **NRC Driver** → `tx_h_put_qos_control()` [nrc-trx.c:579] - Convert UDP to QoS UDP
6. **High-speed Loop** → Continuous transmission while credit sufficient:
   - `nrc_xmit_frame()` [hif.c:711]
   - C-SPI burst transmission to firmware
   - Continuous wireless frame transmission
7. **Batch Completion** → After multiple frames:
   - Hardware sends batch TX completion interrupt
   - Firmware sends WIM Credit Report (batch)
   - `nrc_kick_txq()` to process waiting packets

## Key Differences: TCP vs UDP

### Packet Size Analysis

**TCP Characteristics:**
- Maximum Segment Size: 1460 bytes (MTU 1500 - IP 20 - TCP 20)
- Total frame size: ~1498 bytes (+ 802.11 headers)
- Credit consumption: 6 credits (DIV_ROUND_UP(1498, 256))

**UDP Characteristics:**
- Payload size: 1472 bytes (MTU 1500 - IP 20 - UDP 8)
- Total frame size: ~1510 bytes (+ 802.11 headers)  
- Credit consumption: 6 credits (DIV_ROUND_UP(1510, 256))

### QoS Processing

Both TCP and UDP data frames undergo the same QoS conversion:

```c
// tx_h_put_qos_control() function
if (ieee80211_is_data(fc) && !ieee80211_is_data_qos(fc)) {
    hdr->frame_control |= cpu_to_le16(IEEE80211_STYPE_QOS_DATA);
    *((u16 *)(skb->data + 24)) = 0;  // TID=0 (Best Effort)
}
```

### AMPDU Block ACK Sessions

**TCP Benefits:**
- Order guarantee requirements make AMPDU highly effective
- Built-in retransmission complements Block ACK mechanism
- High aggregation efficiency for continuous streams

**UDP Benefits:**
- No ordering requirements allow for optimized processing
- Higher throughput potential due to no retransmission overhead
- Efficient for real-time applications

## Transmission Completion Mechanism

The driver employs a multi-stage completion confirmation system:

### 1. Hardware Level (C-SPI)
```c
// nrc_hif_xmit() function
ret = nrc_spi_write(hdev, skb->data, skb->len);
if (ret < 0) {
    nrc_hif_free_skb(nw, skb);  // Credit rollback
    return ret;
}
```

### 2. Firmware Level (WIM Credit Report)
```c
// nrc_wim_update_tx_credit() [wim.c:695]
static int nrc_wim_update_tx_credit(struct nrc *nw, struct wim *wim)
{
    struct wim_credit_report *r = (void *)(wim + 1);
    
    // Update credits for each AC
    for (ac = 0; ac < (IEEE80211_NUM_ACS*3); ac++)
        atomic_set(&nw->tx_credit[ac], r->v.ac[ac]);
    
    nrc_kick_txq(nw);  // Resume pending packet processing
    return 0;
}
```

### 3. Completion Steps
1. **SPI Transmission Complete**: Hardware queue arrival
2. **Firmware Processing Complete**: Frame generation done
3. **Wireless Transmission Complete**: Air interface transmission
4. **Credit Return**: Buffer space release for new transmissions

## Performance Optimization Elements

### Credit-Based Flow Control
- **BE (Best Effort)**: 40 credits (largest allocation for data)
- **VI (Video)**: 8 credits
- **VO (Voice)**: 8 credits
- **BK (Background)**: 4 credits

### Batch Processing
```c
// nrc_hif_work() - Sequential queue processing
for (i = ARRAY_SIZE(hdev->queue)-1; i >= 0; i--) {
    for (;;) {
        skb = skb_dequeue(&hdev->queue[i]);
        if (!skb) break;
        ret = nrc_hif_xmit(hdev, skb);
    }
}
```

### AMPDU Aggregation
Automatic Block ACK session setup enhances throughput:
```c
// setup_ba_session()
ret = ieee80211_start_tx_ba_session(peer_sta, tid, 0);
```

## Performance Monitoring

### Real-time Statistics
```bash
# Monitor credit status
cat /proc/nrc/stats
# TX Credit: AC0=4, AC1=35, AC2=8, AC3=8
# TX Pending: AC0=0, AC1=5, AC2=0, AC3=0
```

### Debug Information
```c
// Per-frame debugging
nrc_dbg(NRC_DBG_TX, "TX: Data frame to %pM, len=%d, AC=%d", 
        hdr->addr1, skb->len, ac);
```

## Conclusion

The NRC7292 HaLow driver's TX path demonstrates sophisticated handling of both TCP and UDP traffic through iperf scenarios. Key architectural strengths include:

1. **Unified QoS Processing**: Both protocols benefit from automatic QoS conversion
2. **Credit-Based Flow Control**: Prevents buffer overflow while maintaining high throughput
3. **AMPDU Optimization**: Automatic aggregation improves efficiency for both protocols
4. **Multi-Stage Completion**: Reliable transmission confirmation through hardware and firmware layers
5. **Batch Processing**: Optimized for high-performance scenarios like iperf testing

This design effectively supports IEEE 802.11ah HaLow's low-power, long-range characteristics while delivering the performance needed for throughput measurement tools and real-world applications.

For detailed function-level analysis and complete source code references, see the [full TX path documentation](/code_analysis/source_analysis/tx_path_detailed.md) in our analysis repository.