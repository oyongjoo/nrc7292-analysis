# WIM Protocol Detailed Source Code Analysis

This document provides a comprehensive analysis of the Wireless Interface Module (WIM) protocol implementation in the NRC7292 HaLow driver package.

## Overview

The WIM protocol is the core communication interface between the Linux kernel driver and the NRC7292 firmware. It implements a TLV (Type-Length-Value) based messaging system for commands, responses, and events.

## Table of Contents

1. [WIM Protocol Structure](#wim-protocol-structure)
2. [WIM Message Creation](#wim-message-creation)
3. [WIM Command Processing](#wim-command-processing)
4. [WIM Response Handling](#wim-response-handling)
5. [WIM Event Processing](#wim-event-processing)
6. [Key WIM Commands](#key-wim-commands)
7. [TLV System Implementation](#tlv-system-implementation)
8. [Synchronization Mechanisms](#synchronization-mechanisms)
9. [Error Handling](#error-handling)
10. [Firmware Interaction](#firmware-interaction)

## WIM Protocol Structure

### WIM Header Structure (`wim.h` lines 33-42)

```c
struct wim {
    union {
        u16 cmd;
        u16 resp;
        u16 event;
    };
    u8 seqno;
    u8 n_tlvs;
    u8 payload[0];
} __packed;
```

**Analysis:**
- **Union design**: Commands, responses, and events share the same header structure but use different semantic meanings for the first field
- **Sequence number**: `seqno` provides request/response matching capability
- **TLV count**: `n_tlvs` indicates the number of TLV elements in the payload
- **Variable payload**: Zero-length array for flexible payload sizes

### TLV Structure (`wim.h` lines 44-48)

```c
struct wim_tlv {
    u16 t;      // Type
    u16 l;      // Length
    u8  v[0];   // Value (variable length)
} __packed;
```

**Purpose:**
- Provides extensible parameter passing mechanism
- Enables backward compatibility through optional TLVs
- Supports complex data structures through nested TLVs

## WIM Message Creation

### Core Allocation Function (`wim.c` lines 39-60)

```c
struct sk_buff *nrc_wim_alloc_skb(struct nrc *nw, u16 cmd, int size)
{
    struct sk_buff *skb;
    struct wim *wim;

    skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
    if (!skb)
        return NULL;

    /* Increase the headroom */
    skb_reserve(skb, sizeof(struct hif));

    /* Put wim header */
    wim = (struct wim *)skb_put(skb, sizeof(*wim));
    memset(wim, 0, sizeof(*wim));
    wim->cmd = cmd;
    wim->seqno = nw->wim_seqno++;

    nrc_wim_skb_bind_vif(skb, NULL);

    return skb;
}
```

**Key Features:**
1. **Memory layout planning**: Reserves space for HIF header to avoid future reallocations
2. **Sequence number management**: Auto-incrementing sequence numbers for request tracking
3. **VIF binding**: Associates the message with a virtual interface context
4. **Zero initialization**: Ensures clean header state

### TLV Addition Function (`wim.c` lines 97-114)

```c
void *nrc_wim_skb_add_tlv(struct sk_buff *skb, u16 T, u16 L, void *V)
{
    struct wim_tlv *tlv;

    if (L == 0) {
        tlv = (struct wim_tlv *)(skb_put(skb, sizeof(struct wim_tlv)));
        wim_set_tlv(tlv, T, L);
        return (void *)skb->data;
    }

    tlv = (struct wim_tlv *)(skb_put(skb, tlv_len(L)));
    wim_set_tlv(tlv, T, L);

    if (V)
        memcpy(tlv->v, V, L);

    return (void *)tlv->v;
}
```

**Analysis:**
- **Dynamic sizing**: Handles both zero-length and variable-length TLVs
- **Conditional copying**: Only copies data if source pointer is provided
- **Return value**: Points to the value field for further manipulation
- **Memory safety**: Uses `skb_put()` for proper SKB size management

## WIM Command Processing

### Simple Command Transmission (`wim.c` lines 76-81)

```c
int nrc_xmit_wim_simple_request(struct nrc *nw, int cmd)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(nw, cmd, 0);
    return nrc_xmit_wim_request(nw, skb);
}
```

### Synchronous Command with Response (`wim.c` lines 83-89)

```c
struct sk_buff *nrc_xmit_wim_simple_request_wait(struct nrc *nw, int cmd,
        int timeout)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(nw, cmd, 0);
    return nrc_xmit_wim_request_wait(nw, skb, timeout);
}
```

### Request/Response Synchronization (`hif.c` lines 583-602)

```c
struct sk_buff *nrc_xmit_wim_request_wait(struct nrc *nw,
        struct sk_buff *skb, int timeout)
{
    mutex_lock(&nw->target_mtx);
    nw->last_wim_responded = NULL;

    if (nrc_xmit_wim(nw, skb, HIF_WIM_SUB_REQUEST) < 0) {
        mutex_unlock(&nw->target_mtx);
        return NULL;
    }

    mutex_unlock(&nw->target_mtx);
    
    reinit_completion(&nw->wim_responded);
    if (wait_for_completion_timeout(&nw->wim_responded,
            timeout) == 0)
        return NULL;

    return nw->last_wim_responded;
}
```

**Synchronization Analysis:**
1. **Mutex protection**: Prevents race conditions during request setup
2. **Completion mechanism**: Uses Linux completion API for efficient waiting
3. **Timeout handling**: Prevents indefinite blocking on unresponsive firmware
4. **Response retrieval**: Returns the actual response SKB for processing

## WIM Response Handling

### Response Handler (`wim.c` lines 679-692)

```c
static int nrc_wim_response_handler(struct nrc *nw,
                    struct sk_buff *skb)
{
    mutex_lock(&nw->target_mtx);
    nw->last_wim_responded = skb;
    mutex_unlock(&nw->target_mtx);
    if (completion_done(&nw->wim_responded)) {
    /* No completion waiters, free the SKB */
        pr_err("no completion");
        return 0;
    }
    complete(&nw->wim_responded);
    return 1;
}
```

**Key Mechanisms:**
- **Thread safety**: Mutex protects response storage
- **Completion signaling**: Wakes up waiting threads
- **Memory management**: Returns `1` to prevent automatic SKB freeing
- **Error detection**: Handles cases where no thread is waiting

## WIM Event Processing

### Event Handler (`wim.c` lines 724-822)

```c
static int nrc_wim_event_handler(struct nrc *nw,
                 struct sk_buff *skb)
{
    struct ieee80211_vif *vif = NULL;
    struct wim *wim = (void *) skb->data;
    struct hif *hif = (void *)(skb->data - sizeof(*hif));
    
    /* hif->vifindex to vif */
    if (hif->vifindex != -1)
        vif = nw->vif[hif->vifindex];

    switch (wim->event) {
    case WIM_EVENT_SCAN_COMPLETED:
        nrc_mac_cancel_hw_scan(nw->hw, vif);
        // Timer management for beacon monitoring and power save
        break;
    case WIM_EVENT_READY:
        nrc_wim_handle_fw_ready(nw);
        break;
    case WIM_EVENT_CREDIT_REPORT:
        nrc_wim_update_tx_credit(nw, wim);
        break;
    // ... other events
    }
    
    return 0;
}
```

### Key Event Types Analysis:

1. **WIM_EVENT_SCAN_COMPLETED**: 
   - Notifies scan completion
   - Re-enables beacon monitoring and power save timers

2. **WIM_EVENT_READY**: 
   - Firmware initialization complete
   - Triggers driver state transition to active

3. **WIM_EVENT_CREDIT_REPORT**: 
   - Flow control mechanism for TX queues
   - Updates per-AC credit counters

## Key WIM Commands

### Station Management (`wim.c` lines 153-175)

```c
int nrc_wim_change_sta(struct nrc *nw, struct ieee80211_vif *vif,
               struct ieee80211_sta *sta, u8 cmd, bool sleep)
{
    struct sk_buff *skb;
    struct wim_sta_param *p;

    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_STA_CMD,
                    tlv_len(sizeof(*p)));

    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_STA_PARAM, sizeof(*p), NULL);
    memset(p, 0, sizeof(*p));

    p->cmd = cmd;
    p->flags = 0;
    p->sleep = sleep;
    ether_addr_copy(p->addr, sta->addr);
    p->aid = sta->aid;

    return nrc_xmit_wim_request(nw, skb);
}
```

### Hardware Scan Command (`wim.c` lines 177-309)

```c
int nrc_wim_hw_scan(struct nrc *nw, struct ieee80211_vif *vif,
            struct cfg80211_scan_request *req,
            struct ieee80211_scan_ies *ies)
{
    struct sk_buff *skb;
    struct wim_scan_param *p;
    int i, size = tlv_len(sizeof(struct wim_scan_param));

    // Calculate required size for IEs
    if (ies) {
        size += tlv_len(ies->common_ie_len);
        size += sizeof(struct wim_tlv);
        size += ies->len[NL80211_BAND_S1GHZ];
    }

    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_SCAN_START, size);

    /* WIM_TLV_SCAN_PARAM */
    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_SCAN_PARAM, sizeof(*p), NULL);
    
    // Populate scan parameters
    p->n_channels = req->n_channels;
    for (i = 0; i < req->n_channels; i++) {
        p->channel[i] = FREQ_TO_100KHZ(req->channels[i]->center_freq, 
                                       req->channels[i]->freq_offset);
    }
    
    p->n_ssids = req->n_ssids;
    for (i = 0; i < req->n_ssids; i++) {
        p->ssid[i].ssid_len = req->ssids[i].ssid_len;
        memcpy(p->ssid[i].ssid, req->ssids[i].ssid, req->ssids[i].ssid_len);
    }

    // Add IEs if present
    if (ies) {
        // Band-specific IEs
        nrc_wim_skb_add_tlv(skb, WIM_TLV_SCAN_BAND_IE, 
                           ies->len[NL80211_BAND_S1GHZ], ies->ies[NL80211_BAND_S1GHZ]);
        
        // Common IEs
        nrc_wim_skb_add_tlv(skb, WIM_TLV_SCAN_COMMON_IE, 
                           ies->common_ie_len, (void *)ies->common_ies);
    }

    return nrc_xmit_wim_request(nw, skb);
}
```

### Key Installation (`wim.c` lines 363-455)

```c
int nrc_wim_install_key(struct nrc *nw, enum set_key_cmd cmd,
            struct ieee80211_vif *vif,
            struct ieee80211_sta *sta,
            struct ieee80211_key_conf *key)
{
    struct sk_buff *skb;
    struct wim_key_param *p;
    u8 cipher;
    const u8 *addr;
    u16 aid = 0;

    cipher = nrc_to_wim_cipher_type(key->cipher);
    if (cipher == -1)
        return -ENOTSUPP;

    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_SET_KEY + (cmd - SET_KEY),
                tlv_len(sizeof(*p)));

    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_KEY_PARAM, sizeof(*p), NULL);

    // Determine target address and AID based on context
    if (sta) {
        addr = sta->addr;
    } else if (vif->type == NL80211_IFTYPE_AP) {
        addr = vif->addr;
    } else {
        addr = vif->bss_conf.bssid;
    }

    ether_addr_copy(p->mac_addr, addr);
    p->aid = aid;
    memcpy(p->key, key->key, key->keylen);
    p->cipher_type = cipher;
    p->key_index = key->keyidx;
    p->key_len = key->keylen;
    p->key_flags = (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ?
        WIM_KEY_FLAG_PAIRWISE : WIM_KEY_FLAG_GROUP;

    return nrc_xmit_wim_request(nw, skb);
}
```

## TLV System Implementation

### TLV Types (`nrc-wim-types.h` lines 146-230)

The WIM protocol defines extensive TLV types for different parameter categories:

```c
enum WIM_TLV_ID {
    WIM_TLV_SSID = 0,
    WIM_TLV_BSSID = 1,
    WIM_TLV_MACADDR = 2,
    WIM_TLV_AID = 3,
    WIM_TLV_STA_TYPE = 4,
    WIM_TLV_SCAN_PARAM = 5,
    WIM_TLV_KEY_PARAM = 6,
    // ... extensive list continues
    WIM_TLV_DEFAULT_MCS = 80,
    WIM_TLV_MAX,
};
```

### TLV Helper Functions

**AID Setting** (`wim.c` lines 116-119):
```c
void nrc_wim_set_aid(struct nrc *nw, struct sk_buff *skb, u16 aid)
{
    nrc_wim_skb_add_tlv(skb, WIM_TLV_AID, sizeof(u16), &aid);
}
```

**MAC Address Setting** (`wim.c` lines 121-124):
```c
void nrc_wim_add_mac_addr(struct nrc *nw, struct sk_buff *skb, u8 *addr)
{
    nrc_wim_skb_add_tlv(skb, WIM_TLV_MACADDR, ETH_ALEN, addr);
}
```

## Synchronization Mechanisms

### Request/Response Matching

The WIM protocol implements sophisticated synchronization through:

1. **Sequence Numbers**: Each request gets a unique sequence number
2. **Completion Objects**: Linux completion API for efficient waiting
3. **Mutex Protection**: Prevents race conditions in multi-threaded access
4. **Timeout Handling**: Prevents indefinite blocking

### Flow Control (`wim.c` lines 695-722)

```c
static int nrc_wim_update_tx_credit(struct nrc *nw, struct wim *wim)
{
    struct wim_credit_report *r = (void *)(wim + 1);
    int ac;

    for (ac = 0; ac < (IEEE80211_NUM_ACS*3); ac++)
        atomic_set(&nw->tx_credit[ac], r->v.ac[ac]);

    nrc_kick_txq(nw);
    return 0;
}
```

## Error Handling

### Cipher Type Validation (`wim.c` lines 327-345)

```c
enum wim_cipher_type nrc_to_wim_cipher_type(u32 cipher)
{
    switch (cipher) {
    case WLAN_CIPHER_SUITE_WEP40:
        return WIM_CIPHER_TYPE_WEP40;
    case WLAN_CIPHER_SUITE_WEP104:
        return WIM_CIPHER_TYPE_WEP104;
    case WLAN_CIPHER_SUITE_TKIP:
        return WIM_CIPHER_TYPE_TKIP;
    case WLAN_CIPHER_SUITE_CCMP:
        return WIM_CIPHER_TYPE_CCMP;
    case WLAN_CIPHER_SUITE_AES_CMAC:
    case WLAN_CIPHER_SUITE_BIP_GMAC_128:
    case WLAN_CIPHER_SUITE_BIP_GMAC_256:
        return WIM_CIPHER_TYPE_NONE;
    default:
        return WIM_CIPHER_TYPE_INVALID;
    }
}
```

### Response Timeout Handling

The synchronous WIM request mechanism includes comprehensive timeout handling to prevent system hangs when firmware becomes unresponsive.

## Firmware Interaction

### Message Routing (`wim.c` lines 824-850)

```c
int nrc_wim_rx(struct nrc *nw, struct sk_buff *skb, u8 subtype)
{
    int ret;

    ret = wim_rx_handler[subtype](nw, skb);

    if (ret != 1)
        /* Free the skb later (e.g. on Response WIM handler) */
        dev_kfree_skb(skb);

    return 0;
}
```

### Handler Dispatch Table (`wim.c` lines 826-830)

```c
static wim_rx_func wim_rx_handler[] = {
    [HIF_WIM_SUB_REQUEST] = nrc_wim_request_handler,
    [HIF_WIM_SUB_RESPONSE] = nrc_wim_response_handler,
    [HIF_WIM_SUB_EVENT] = nrc_wim_event_handler,
};
```

## Message Flow Analysis

### Command Flow:
1. Driver creates WIM message using `nrc_wim_alloc_skb()`
2. Parameters added via TLV system using `nrc_wim_skb_add_tlv()`
3. Message queued through HIF layer via `nrc_xmit_wim_request()`
4. Firmware processes command and generates response
5. Response received through `nrc_wim_response_handler()`
6. Waiting thread awakened via completion mechanism

### Event Flow:
1. Firmware generates asynchronous event
2. Event received through `nrc_wim_event_handler()`
3. Event type determines specific handling logic
4. Driver state updated based on event type
5. MAC80211 layer notified if required

## Performance Considerations

### Memory Management:
- Pre-allocation of SKB space to avoid reallocations
- Efficient TLV packing to minimize message sizes
- Proper SKB lifecycle management to prevent leaks

### Concurrency:
- Lock-free atomic operations for credit management
- Minimal critical sections in mutex-protected code
- Efficient completion-based synchronization

### Scalability:
- Support for multiple VIFs through indexing
- Per-AC credit tracking for QoS support
- Extensible TLV system for future enhancements

## Conclusion

The WIM protocol implementation demonstrates sophisticated firmware-driver communication through:

1. **Robust synchronization** mechanisms for request/response matching
2. **Flexible TLV system** enabling extensible parameter passing
3. **Comprehensive event handling** for asynchronous firmware notifications
4. **Efficient flow control** through credit-based transmission management
5. **Thread-safe design** supporting concurrent operations

The implementation successfully abstracts complex firmware interactions behind a clean, maintainable interface while providing the performance and reliability required for production wireless networking.