---
layout: post
title: "NRC7292 TX Path Detailed Analysis"
date: 2025-06-17
category: Architecture
tags: [tx-path, tasklet, credit-system, ampdu, source-code]
excerpt: "Comprehensive analysis of the NRC7292 HaLow driver TX transmission path, including tasklet mechanism, credit-based flow control, and AMPDU Block ACK session management."
---

# NRC7292 TX Path Detailed Analysis

This post provides a comprehensive analysis of the TX (transmission) path in the NRC7292 HaLow (IEEE 802.11ah) driver, covering the entire data flow from mac80211 kernel framework to hardware transmission.

## TX Entry Point: `nrc_mac_tx` Function

The TX path begins at `nrc_mac_tx` function in `nrc-trx.c`:

```c
#ifdef CONFIG_SUPPORT_NEW_MAC_TX
void nrc_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
        struct sk_buff *skb)
#else
void nrc_mac_tx(struct ieee80211_hw *hw,
        struct sk_buff *skb)
#endif
```

### Parameter Analysis

- **`ieee80211_hw *hw`**: mac80211 hardware abstraction providing driver context via `hw->priv`
- **`ieee80211_tx_control *control`**: TX control information for newer kernel versions
- **`sk_buff *skb`**: Packet data containing IEEE 802.11 frame with metadata via `IEEE80211_SKB_CB(skb)`

## TX Tasklet Mechanism

### Initialization

The TX tasklet is configured during driver initialization (`nrc-init.c:816-822`):

```c
#ifdef CONFIG_USE_TXQ
#ifdef CONFIG_NEW_TASKLET_API
    tasklet_setup(&nw->tx_tasklet, nrc_tx_tasklet);
#else
    tasklet_init(&nw->tx_tasklet, nrc_tx_tasklet, (unsigned long) nw);
#endif
#endif
```

**Kernel API Compatibility:**
- `tasklet_setup()`: New kernel API (5.0+) with type safety
- `tasklet_init()`: Legacy API using unsigned long parameter

### Implementation

The TX tasklet provides bottom-half processing for sequential packet transmission:

```c
void nrc_tx_tasklet(struct tasklet_struct *t)
{
    struct nrc *nw = from_tasklet(nw, t, tx_tasklet);
    struct nrc_txq *ntxq, *tmp;
    int ret;

    spin_lock_bh(&nw->txq_lock);

    list_for_each_entry_safe(ntxq, tmp, &nw->txq, list) {
        ret = nrc_push_txq(nw, ntxq);
        if (ret == 0) {
            list_del_init(&ntxq->list);  // All packets sent
        } else {
            list_move_tail(&ntxq->list, &nw->txq);  // Round-robin
            break;
        }
    }

    spin_unlock_bh(&nw->txq_lock);
}
```

**Key Features:**
1. **Round-robin Scheduling**: Fair processing between TXQs
2. **Spinlock Protection**: Concurrency control with `txq_lock`
3. **Credit-aware Processing**: Stops when hardware buffers full

## Credit-Based Flow Control

### Credit Calculation

NRC7292 uses a sophisticated credit system for hardware buffer management:

```c
credit = DIV_ROUND_UP(skb->len, nw->fwinfo.buffer_size);
```

### Per-AC Credit Allocation

```c
#define CREDIT_AC0      (TCN*2+TCNE)    /* BK (4) */
#define CREDIT_AC1      (TCN*20+TCNE)   /* BE (40) */
#define CREDIT_AC2      (TCN*4+TCNE)    /* VI (8) */
#define CREDIT_AC3      (TCN*4+TCNE)    /* VO (8) */
```

### Credit Update Mechanism

Firmware reports credit availability via WIM messages:

```c
static int nrc_wim_update_tx_credit(struct nrc *nw, struct wim *wim)
{
    struct wim_credit_report *r = (void *)(wim + 1);
    int ac;
    
    for (ac = 0; ac < (IEEE80211_NUM_ACS*3); ac++)
        atomic_set(&nw->tx_credit[ac], r->v.ac[ac]);
    
    nrc_kick_txq(nw);  // Schedule tasklet
    return 0;
}
```

## AMPDU Block ACK Session Management

### Automatic BA Session Setup

The `setup_ba_session()` function manages automatic AMPDU establishment:

```c
static void setup_ba_session(struct nrc *nw, struct ieee80211_vif *vif, struct sk_buff *skb)
{
    struct ieee80211_sta *peer_sta = NULL;
    struct nrc_sta *i_sta = NULL;
    struct ieee80211_hdr *qmh = (struct ieee80211_hdr *) skb->data;
    int tid = *ieee80211_get_qos_ctl(qmh) & IEEE80211_QOS_CTL_TID_MASK;
    
    // 1. Fragmentation check
    if (nw->frag_threshold != -1) return;
    
    // 2. TID validation
    if (tid < 0 || tid >= NRC_MAX_TID) return;
    
    // 3. Find destination station
    peer_sta = ieee80211_find_sta(vif, qmh->addr1);
    if (!peer_sta) return;
    
    // 4. Get NRC station context
    i_sta = to_i_sta(peer_sta);
    if (!i_sta) return;
    
    // 5. State machine handling
    switch (i_sta->tx_ba_session[tid]) {
        case IEEE80211_BA_NONE:
        case IEEE80211_BA_CLOSE:
            ret = ieee80211_start_tx_ba_session(peer_sta, tid, 0);
            break;
        case IEEE80211_BA_REJECT:
            // Retry after 5 seconds
            if (jiffies_to_msecs(jiffies - i_sta->ba_req_last_jiffies[tid]) > 5000) {
                i_sta->tx_ba_session[tid] = IEEE80211_BA_NONE;
            }
            break;
    }
}
```

## drv_priv Structure Analysis

### The `to_i_sta()` Macro

```c
#define to_i_sta(s) ((struct nrc_sta *) (s)->drv_priv)
```

### NRC Station Structure

```c
struct nrc_sta {
    struct nrc *nw;                              // Driver context
    struct ieee80211_vif *vif;                   // Connected VIF
    
    enum ieee80211_sta_state state;              // STA state
    struct list_head list;                       // List connection
    
    /* Security keys */
    struct ieee80211_key_conf *ptk;              // Pairwise key
    struct ieee80211_key_conf *gtk;              // Group key
    
    /* Power management */
    uint16_t listen_interval;
    struct nrc_max_idle max_idle;
    
    /* Per-TID Block ACK sessions */
    enum ieee80211_tx_ba_state tx_ba_session[NRC_MAX_TID];
    uint32_t ba_req_last_jiffies[NRC_MAX_TID];
    struct rx_ba_session rx_ba_session[NRC_MAX_TID];
};
```

### Memory Layout

```
[ieee80211_sta structure][struct nrc_sta (drv_priv)]
                         ↑
                         sta->drv_priv points here
```

## TX Handler Chain

The TX path uses a modular handler chain defined at compile time:

```c
#define TXH(fn, mask)                   \
    static struct nrc_trx_handler __txh_ ## fn \
    __attribute((__section__("nrc.txh"))) = {  \
        .handler = fn,              \
        .vif_types = mask,          \
    }
```

### Major TX Handlers

1. **`tx_h_debug_print`**: Debug output (conditional)
2. **`tx_h_debug_state`**: Station state verification  
3. **`tx_h_frame_filter`**: Frame type filtering
4. **`tx_h_put_iv`**: Security IV header addition
5. **`tx_h_put_qos_control`**: QoS control field management

## Hardware Transmission

### HIF Header Construction

```c
hif = (void *)skb_push(skb, nw->fwinfo.tx_head_size);
memset(hif, 0, nw->fwinfo.tx_head_size);
hif->type = HIF_TYPE_FRAME;
hif->len = skb->len - sizeof(*hif);
hif->vifindex = vif_index;
```

### CSPI Interface

The Custom SPI (CSPI) protocol handles actual hardware transmission:

```c
/*
 * [31:24]: start byte (0x50)
 * [23:23]: burst (0: single, 1: burst)  
 * [22:22]: direction (0: read, 1: write)
 * [21:21]: fixed (0: incremental, 1: fixed)
 * [20:13]: address
 * [12:0]: length
 */
#define C_SPI_WRITE     0x50400000
#define C_SPI_BURST     0x00800000
```

## Performance Optimizations

### Zero-Copy Processing
- Efficient header addition using `skb_push()` and `skb_put()`
- Minimal memory copying through SKB headroom utilization

### Batch Processing
- Workqueue-based sequential transmission
- Priority-based queue processing for QoS

### Hardware Acceleration
- Firmware-level encryption support
- Automatic AMPDU aggregation management

## Conclusion

The NRC7292 TX path demonstrates a sophisticated architecture optimized for HaLow's IoT requirements:

1. **Efficient Tasklet Processing**: Bottom-half processing with round-robin fairness
2. **Credit-Based Flow Control**: Hardware buffer overflow prevention
3. **Automatic AMPDU Management**: Intelligent Block ACK session handling
4. **Modular Handler Chain**: Flexible processing pipeline
5. **Zero-Copy Optimization**: High-performance packet processing

This architecture effectively supports IEEE 802.11ah characteristics of low power, long range, and high device density while maintaining stability and performance across various network scenarios.

---

*This analysis is based on comprehensive source code review of the NRC7292 Linux kernel driver. For complete technical details, refer to the [source code analysis documentation](https://github.com/oyongjoo/nrc7292-analysis).*