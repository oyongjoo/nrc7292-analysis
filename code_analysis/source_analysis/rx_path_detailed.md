# NRC7292 Receive (RX) Path Detailed Source Code Analysis

This document provides a comprehensive source code analysis of the NRC7292 HaLow driver's receive (RX) path. It covers the complete reception flow from CSPI interrupts to frame delivery to mac80211.

## Table of Contents

1. [Overview](#overview)
2. [RX Path Architecture](#rx-path-architecture)
3. [CSPI Interrupt Handling](#cspi-interrupt-handling)
4. [RX Thread Processing](#rx-thread-processing)
5. [HIF Layer Processing](#hif-layer-processing)
6. [RX Handler Chain](#rx-handler-chain)
7. [mac80211 Delivery](#mac80211-delivery)
8. [Special Frame Processing](#special-frame-processing)
9. [Conclusion](#conclusion)

## Overview

The NRC7292 receive path is a complex processing pipeline that begins with hardware interrupts and culminates in frame delivery to the Linux mac80211 subsystem. The main components include:

- **CSPI Interrupt Handler**: Hardware interrupt processing
- **RX Thread**: Asynchronous frame reception processing
- **HIF Layer**: Frame type-based routing
- **RX Handler Chain**: VIF-specific frame processing
- **mac80211 Interface**: Delivery to kernel networking stack

## RX Path Architecture

```
Hardware Interrupt
         ↓
   CSPI IRQ Handler
         ↓
    spi_update_status()
         ↓
    wake_up_interruptible()
         ↓
     spi_rx_thread()
         ↓
      spi_rx_skb()
         ↓
   HIF Layer (nrc_hif_rx)
         ↓
    Type-based Routing
         ↓
    nrc_mac_rx() / nrc_wim_rx()
         ↓
   ieee80211_rx_irqsafe()
```

## CSPI Interrupt Handling

### IRQ Handler Function

CSPI interrupts are processed by the `spi_irq()` function in `nrc-hif-cspi.c`:

#### When CONFIG_SUPPORT_THREADED_IRQ is Enabled

```c
static irqreturn_t spi_irq(int irq, void *data)
{
    struct nrc_hif_device *hdev = data;
    struct nrc_spi_priv *priv = hdev->priv;
    struct spi_device *spi = priv->spi;

#ifdef CONFIG_NRC_HIF_PRINT_FLOW_CONTROL
    nrc_dbg(NRC_DBG_HIF, "%s", __func__);
#endif

    spi_update_status(spi);
    wake_up_interruptible(&priv->wait);

    return IRQ_HANDLED;
}
```

**Key Functions:**
- Calls `spi_update_status()` to update hardware status
- Calls `wake_up_interruptible()` to wake up waiting RX thread
- Returns IRQ_HANDLED to signal interrupt completion

#### When CONFIG_SUPPORT_THREADED_IRQ is Disabled

```c
static irqreturn_t spi_irq(int irq, void *data)
{
    struct nrc_hif_device *hdev = data;
    struct nrc_spi_priv *priv = hdev->priv;

    queue_work(priv->irq_wq, &priv->irq_work);

    return IRQ_HANDLED;
}

static void irq_worker(struct work_struct *work)
{
    struct nrc_spi_priv *priv = container_of(work,
            struct nrc_spi_priv, irq_work);
    struct spi_device *spi = priv->spi;

    spi_update_status(spi);
    wake_up_interruptible(&priv->wait);
}
```

**Key Functions:**
- Uses workqueue for bottom-half processing
- Performs minimal work in interrupt context
- Actual status updates handled in `irq_worker()`

### Status Update Function

The `spi_update_status()` function reads hardware status and updates slot information:

```c
static int spi_update_status(struct spi_device *spi)
{
    struct nrc *nw = spi_get_drvdata(spi);
    struct nrc_hif_device *hdev = nw->hif;
    struct nrc_spi_priv *priv = hdev->priv;
    struct spi_status_reg *status = &priv->hw.status;

    int ret, ac, i, is_relay, retry_cnt=0;
    u32 rear;
    u16 cleared_sta=0;

    if (cspi_suspend) {
        return 0;
    }

    SYNC_LOCK(hdev);
    ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)status, sizeof(*status));
    SYNC_UNLOCK(hdev);
    
    if (ret < 0) {
        return ret;
    }

    // Update RX queue status
    priv->slot[RX_SLOT].head = status->rxq_status[0] & RXQ_SLOT_COUNT;
    
    // Credit handling logic
    for (ac = 0; ac < priv->hw_queues; ac++) {
        rear = status->txq_status[ac] & TXQ_SLOT_COUNT;
        priv->rear[ac] = rear;
    }

    return 0;
}
```

**Key Functions:**
- Reads queue status from CSPI registers
- Updates RX slot head pointer
- Updates TX credit status
- Synchronization through mutex

## RX Thread Processing

### spi_rx_thread Function

The RX thread is implemented in the `spi_rx_thread()` function, which continuously processes received data:

```c
static int spi_rx_thread(void *data)
{
    struct nrc_hif_device *hdev = data;
    struct nrc_spi_priv *priv = hdev->priv;
    struct spi_device *spi = priv->spi;
    struct sk_buff *skb;
    struct hif *hif;
    struct nrc *nw = hdev->nw;
    int ret;

    while (!kthread_should_stop()) {
        if (nw->loopback) {
            ret = spi_loopback(spi, priv, nw->lb_count);
            if (ret <= 0)
                nrc_dbg(NRC_DBG_HIF, "loopback (%d) error.", ret);
            continue;
        }

        if (!kthread_should_park()) {
            skb = spi_rx_skb(spi, priv);
            if (!skb) continue;

            hif = (void *)skb->data;
            if (hif->type != HIF_TYPE_LOOPBACK && cspi_suspend) {
                dev_kfree_skb(skb);
            } else {
                hdev->hif_ops->receive(hdev, skb);
            }
        } else {
            nrc_dbg(NRC_DBG_HIF, "spi_rx_thread parked.");
            kthread_parkme();
        }
    }
    return 0;
}
```

**Key Functions:**
- Infinite loop for continuous reception processing
- Loopback mode support
- Thread stop/park condition checking
- Frame reception through `spi_rx_skb()`
- HIF ops receive callback invocation

### spi_rx_skb Function

Actual frame reception is handled by the `spi_rx_skb()` function:

```c
static struct sk_buff *spi_rx_skb(struct spi_device *spi,
                struct nrc_spi_priv *priv)
{
    struct sk_buff *skb;
    struct hif *hif;
    ssize_t size;
    u32 nr_slot;
    int ret;
    u32 second_length = 0;
    struct nrc *nw = spi_get_drvdata(spi);
    struct nrc_hif_device *hdev = nw->hif;
    static const int def_slot = 4;

    // Allocate SKB
    skb = dev_alloc_skb(priv->slot[RX_SLOT].size * def_slot);
    if (!skb)
        goto fail;

    // Update status if RX slot is empty
    if (c_spi_num_slots(priv, RX_SLOT) == 0)
        spi_update_status(priv->spi);

    // Wait until at least one RX slot is ready
    ret = wait_event_interruptible(priv->wait,
            ((c_spi_num_slots(priv, RX_SLOT) > 0) ||
             kthread_should_stop() || kthread_should_park()));
    if (ret < 0)
        goto fail;

    if (kthread_should_stop() || kthread_should_park())
        goto fail;

    SYNC_LOCK(hdev);
    
    // Read first slot (including HIF header)
    priv->slot[RX_SLOT].tail++;
    size = c_spi_read(spi, skb->data, priv->slot[RX_SLOT].size);
    SYNC_UNLOCK(hdev);
    
    if (size < 0) {
        priv->slot[RX_SLOT].tail--;
        goto fail;
    }

    // Validate HIF header
    hif = (void *)skb->data;
    
    if (hif->type >= HIF_TYPE_MAX || hif->len == 0) {
        nrc_dbg(NRC_DBG_HIF, "rxslot:(h=%d,t=%d)",
                priv->slot[RX_SLOT].head, priv->slot[RX_SLOT].tail);
        spi_reset_rx(hdev);
        goto fail;
    }

    // Calculate additional slots needed
    nr_slot = DIV_ROUND_UP(sizeof(*hif) + hif->len, priv->slot[RX_SLOT].size);
    nr_slot--;

    if (nr_slot == 0)
        goto out;

    // Wait until additional slots are ready
    ret = wait_event_interruptible(priv->wait,
                (c_spi_num_slots(priv, RX_SLOT) >= nr_slot) ||
                kthread_should_stop() || kthread_should_park());
    if (ret < 0)
        goto fail;

    // Read additional data
    priv->slot[RX_SLOT].tail += nr_slot;
    second_length = hif->len + sizeof(*hif) - priv->slot[RX_SLOT].size;
    
    // 4-byte alignment
    if (second_length & 0x3) {
        second_length = (second_length + 4) & 0xFFFFFFFC;
    }

    SYNC_LOCK(hdev);
    size = c_spi_read(spi, skb->data + priv->slot[RX_SLOT].size,
            second_length);
    SYNC_UNLOCK(hdev);

    if (size < 0)
        goto fail;

out:
    skb_put(skb, sizeof(*hif) + hif->len);
    return skb;

fail:
    if (skb)
        dev_kfree_skb(skb);
    return NULL;
}
```

**Key Functions:**
- Multi-slot reception handling
- HIF header-based data length calculation
- Efficient reception through asynchronous waiting
- 4-byte alignment processing
- Error handling and recovery

## HIF Layer Processing

### hif_receive_skb Function

Received SKBs are routed by type in the `hif_receive_skb()` function in `hif.c`:

```c
static int hif_receive_skb(struct nrc_hif_device *dev, struct sk_buff *skb)
{
    struct nrc *nw = to_nw(dev);
    struct hif *hif = (void *)skb->data;

    WARN_ON(skb->len != hif->len + sizeof(*hif));

    if (nw->drv_state < NRC_DRV_START) {
        dev_kfree_skb(skb);
        return -EIO;
    }
    
    skb_pull(skb, sizeof(*hif));

    switch (hif->type) {
    case HIF_TYPE_FRAME:
        nrc_mac_rx(nw, skb);
        break;
    case HIF_TYPE_WIM:
        nrc_wim_rx(nw, skb, hif->subtype);
        break;
    case HIF_TYPE_LOG:
        nrc_netlink_rx(nw, skb, hif->subtype);
        break;
    case HIF_TYPE_DUMP:
        nrc_dbg(NRC_DBG_HIF, "HIF_TYPE_DUMP - length: %d", hif->len);
        nrc_dump_store((char *)skb->data, hif->len);
        dev_kfree_skb(skb);
        break;
    case HIF_TYPE_LOOPBACK:
        // Loopback processing logic
        dev_kfree_skb(skb);
        break;
    default:
        print_hex_dump(KERN_DEBUG, "hif type err ", DUMP_PREFIX_NONE,
                16, 1, skb->data, skb->len > 32 ? 32 : skb->len, false);
        dev_kfree_skb(skb);
    }
    return 0;
}
```

**Key Functions:**
- HIF header removal
- Driver state verification
- HIF type-based routing:
  - `HIF_TYPE_FRAME`: 802.11 frames → `nrc_mac_rx()`
  - `HIF_TYPE_WIM`: WIM commands/responses → `nrc_wim_rx()`
  - `HIF_TYPE_LOG`: Log data → `nrc_netlink_rx()`
  - `HIF_TYPE_DUMP`: Dump data → file storage

### nrc_hif_rx Function (Alternative Path)

In some cases, the `nrc_hif_rx()` function may be called directly:

```c
int nrc_hif_rx(struct nrc_hif_device *dev, const u8 *data, const u32 len)
{
    struct nrc *nw = to_nw(dev);
    struct sk_buff *skb;
    struct hif *hif = (void *)data;

    WARN_ON(len < sizeof(struct hif));

    if (nw->loopback) {
        // Loopback mode processing
        skb = dev_alloc_skb(len);
        if (!skb)
            return 0;

        ptr = (uint8_t *)skb_put(skb, len);
        memcpy(ptr, data, len);
        nrc_hif_debug_send(nw, skb);
        return 0;
    }

    skb = dev_alloc_skb(hif->len);
    memcpy(skb_put(skb, hif->len), data + sizeof(*hif), hif->len);

    switch (hif->type) {
    case HIF_TYPE_FRAME:
        nrc_mac_rx(nw, skb);
        break;
    case HIF_TYPE_WIM:
        nrc_wim_rx(nw, skb, hif->subtype);
        break;
    default:
        dev_kfree_skb(skb);
        BUG();
    }
    return 0;
}
```

## RX Handler Chain

### nrc_mac_rx Function

802.11 frames are processed by the `nrc_mac_rx()` function in `nrc-trx.c`:

```c
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    struct nrc_trx_data rx = { .nw = nw, .skb = skb, };
    struct ieee80211_hdr *mh;
    __le16 fc;
    int ret = 0;
    u64 now = 0, diff = 0;

    // Driver state verification
    if (!((nw->drv_state == NRC_DRV_RUNNING) ||
        (nw->drv_state == NRC_DRV_PS)) ||
        atomic_read(&nw->d_deauth.delayed_deauth)) {
        nrc_mac_dbg("Target not ready, discarding frame)");
        dev_kfree_skb(skb);
        return 0;
    }

    // RX status header processing
    nrc_mac_rx_h_status(nw, skb);

    // Promiscuous mode handling
    if (nw->promisc) {
        ret = nrc_mac_s1g_monitor_rx(nw, skb);
        return ret;
    }

    // Remove frame header
    skb_pull(skb, nw->fwinfo.rx_head_size - sizeof(struct hif));
    mh = (void*)skb->data;
    fc = mh->frame_control;
    now = ktime_to_us(ktime_get_real());

    // Iterate over active interfaces
    ieee80211_iterate_active_interfaces(nw->hw, nrc_rx_handler, &rx);

    // Scan mode handling
    if (nw->scan_mode == NRC_SCAN_MODE_ACTIVE_SCANNING) {
        if (nw->associated_vif != NULL &&
            nw->associated_vif->type == NL80211_IFTYPE_STATION &&
            ieee80211_is_beacon(fc)) {
            dev_kfree_skb(skb);
            return 0;
        }
    }

    diff = ktime_to_us(ktime_get_real()) - now;
    if (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD)
        nrc_mac_dbg("%s, diff=%lu", __func__, (unsigned long)diff);

    if (!rx.result) {
        // Beacon monitoring timer update
        if (!disable_cqm) {
            if (nw->associated_vif &&
                ieee80211_is_probe_resp(fc) &&
                nw->scan_mode == NRC_SCAN_MODE_IDLE) {
                mod_timer(&nw->bcn_mon_timer,
                    jiffies + msecs_to_jiffies(nw->beacon_timeout));
            }
        }

        // Association request handling
        if ((ieee80211_is_assoc_req(mh->frame_control) ||
            ieee80211_is_reassoc_req(mh->frame_control)) &&
            rx.sta) {
            struct nrc_sta *i_sta = to_i_sta(rx.sta);
            struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
            i_sta->listen_interval = mgmt->u.assoc_req.listen_interval;
        }

        // ARP frame debugging
#if NRC_DBG_PRINT_ARP_FRAME
        if (ieee80211_is_data_qos(fc)) {
            uint8_t *ptr;
            uint16_t llc_type;
            uint8_t ccmp_hdr = 0;
            if (ieee80211_has_protected(fc)) {
                ccmp_hdr = 8;
            }
            ptr = (uint8_t *)mh + (ieee80211_hdrlen(fc) + ccmp_hdr + 6);
            llc_type = *((uint16_t *)ptr);
            if (llc_type == htons(ETH_P_ARP)) {
                nrc_ps_dbg("[%s] RX ARP [type:%d stype:%d, protected:%d, len:%d]",
                    __func__, WLAN_FC_GET_TYPE(fc), WLAN_FC_GET_STYPE(fc),
                    ieee80211_has_protected(fc), rx.skb->len);
            }
        }
#endif
        // Frame delivery to mac80211
        ieee80211_rx_irqsafe(nw->hw, rx.skb);

        // Power management handling
        if (ieee80211_hw_check(nw->hw, SUPPORTS_DYNAMIC_PS)) {
            if (ieee80211_is_data(fc) && nw->ps_enabled &&
                nw->hw->conf.dynamic_ps_timeout > 0) {
                mod_timer(&nw->dynamic_ps_timer,
                    jiffies + msecs_to_jiffies(nw->hw->conf.dynamic_ps_timeout));
            }
        }
    }

    return 0;
}
```

**Key Functions:**
- Driver state validation
- RX status information processing
- VIF-specific frame processing
- Scan mode considerations
- Power management timer updates
- Safe frame delivery to mac80211

### nrc_mac_rx_h_status Function

RX status headers are processed by the `nrc_mac_rx_h_status()` function:

```c
static void nrc_mac_rx_h_status(struct nrc *nw, struct sk_buff *skb)
{
    struct frame_hdr *fh = (void *)skb->data;
    struct ieee80211_rx_status *status;
    struct ieee80211_hdr *mh;
    u8 nss, mcs, gi;
    u32 rate;

    // Initialize mac80211 RX status structure
    status = IEEE80211_SKB_RXCB(skb);
    memset(status, 0, sizeof(*status));

    // Extract status information from frame header
    status->freq = ieee80211_channel_to_frequency(fh->info.rx.channel,
                                                  NL80211_BAND_2GHZ);
    status->band = NL80211_BAND_2GHZ;
    status->signal = fh->info.rx.rssi;

    // Set MCS information
    if (fh->info.rx.mcs_index != 0xff) {
        status->encoding = RX_ENC_HT;
        status->rate_idx = fh->info.rx.mcs_index;
        
        if (fh->info.rx.gi)
            status->enc_flags |= RX_ENC_FLAG_SHORT_GI;
    }

    // Set bandwidth information
    switch (fh->info.rx.bw) {
    case 0:
        status->bw = RATE_INFO_BW_20;
        break;
    case 1:
        status->bw = RATE_INFO_BW_40;
        break;
    default:
        status->bw = RATE_INFO_BW_20;
        break;
    }

    // Set timestamp
    status->mactime = fh->info.rx.timestamp;
    status->flag |= RX_FLAG_MACTIME_START;
}
```

## mac80211 Delivery

### ieee80211_rx_irqsafe Function

Finally, frames are delivered to the mac80211 subsystem through the kernel's `ieee80211_rx_irqsafe()` function:

```c
// Within nrc_mac_rx() function
ieee80211_rx_irqsafe(nw->hw, rx.skb);
```

**Characteristics of ieee80211_rx_irqsafe():**
- **Interrupt Safe**: Can be safely called from interrupt context
- **Asynchronous Processing**: Actual processing occurs in softirq context
- **Queueing**: Stores frames in internal queue for later processing
- **RCU Protection**: Safe processing under RCU read lock

### Radiotap Header Generation

mac80211 automatically generates radiotap headers for monitor interfaces. This information is extracted from the previously set `ieee80211_rx_status` structure.

## Special Frame Processing

### WIM Frame Processing

WIM (Wireless Interface Module) frames are processed by the `nrc_wim_rx()` function in `wim.c`:

```c
int nrc_wim_rx(struct nrc *nw, struct sk_buff *skb, u8 subtype)
{
    int ret;

#if defined(CONFIG_NRC_HIF_PRINT_RX_DATA)
    skb_push(skb, sizeof(struct hif));
    nrc_dump_wim(skb);
    skb_pull(skb, sizeof(struct hif));
#endif

    ret = wim_rx_handler[subtype](nw, skb);

    if (ret != 1)
        /* Free the skb later (e.g. on Response WIM handler) */
        dev_kfree_skb(skb);

    return 0;
}
```

**WIM Subtype Handlers:**
- `HIF_WIM_SUB_REQUEST`: Request message processing
- `HIF_WIM_SUB_RESPONSE`: Response message processing
- `HIF_WIM_SUB_EVENT`: Event message processing

### Management Frame Special Processing

Some management frames receive special treatment in the `nrc_mac_rx()` function:

#### Authentication Frame Processing
```c
// Handle re-authentication from already connected STA in AP mode
if (ieee80211_is_auth(mh->frame_control) && rx.sta && rx.vif && 
    rx.vif->type == NL80211_IFTYPE_AP) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    if(i_sta && i_sta->state > IEEE80211_STA_NOTEXIST){
        struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
        struct sk_buff *skb_deauth;
        
        // Generate virtual deauth frame
        skb_deauth = ieee80211_deauth_get(nw->hw, mgmt->bssid, mgmt->sa, 
                                         mgmt->bssid, WLAN_REASON_DEAUTH_LEAVING, 
                                         NULL, false);
        if (skb_deauth) {
            ieee80211_rx_irqsafe(nw->hw, skb_deauth);
            dev_kfree_skb(skb);
            return 0;
        }
    }
}
```

#### Association Request Frame Processing
```c
if ((ieee80211_is_assoc_req(mh->frame_control) ||
    ieee80211_is_reassoc_req(mh->frame_control)) && rx.sta) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
    i_sta->listen_interval = mgmt->u.assoc_req.listen_interval;
}
```

### Encrypted Frame Processing

Decryption of encrypted frames is handled in firmware, and the driver receives already decrypted frames. The `rx_h_decrypt()` function performs additional processing when needed:

```c
static int rx_h_decrypt(struct nrc_trx_data *rx)
{
    struct ieee80211_hdr *mh = (void *) rx->skb->data;
    struct nrc *nw = rx->nw;
    
    // Check protected frame bit
    if (ieee80211_has_protected(mh->frame_control)) {
        // Additional encryption-related processing
        // (mostly handled in firmware)
    }
    
    return 0;
}
```

## Error Handling and Recovery

### RX Reset Mechanism

When data integrity issues occur, the `spi_reset_rx()` function is called:

```c
void spi_reset_rx(struct nrc_hif_device *hdev)
{
    struct nrc_spi_priv *priv = hdev->priv;
    
    // Reset RX slot pointers
    priv->slot[RX_SLOT].head = 0;
    priv->slot[RX_SLOT].tail = 0;
    
    // Re-synchronize hardware status
    spi_update_status(priv->spi);
}
```

### Garbage Data Detection

```c
if (c_spi_num_slots(priv, RX_SLOT) > 32) {
    if (cnt1++ < 10) {
        pr_err("!!!!! garbage rx data");
        spi_reset_rx(hdev);
    }
    goto fail;
}
```

## Performance Optimization

### TRX Backoff Mechanism

When AMPDU is not supported, traffic backoff is used:

```c
#ifdef CONFIG_TRX_BACKOFF
if (!nw->ampdu_supported) {
    backoff = atomic_inc_return(&priv->trx_backoff);
    
    if ((backoff % 3) != 0) {
        usleep_range(800, 1000);
    }
}
#endif
```

### Slot Management Optimization

```c
// Efficient slot calculation
static inline u16 c_spi_num_slots(struct nrc_spi_priv *priv, int dir)
{
    return (priv->slot[dir].head - priv->slot[dir].tail);
}
```

## Conclusion

The NRC7292 RX path from hardware interrupts to mac80211 delivery exhibits the following characteristics:

### Key Advantages

1. **Asynchronous Processing**: Efficient interrupt and thread-based processing
2. **Type-based Routing**: Clean frame classification through HIF layer
3. **Error Recovery**: Robust error detection and recovery mechanisms
4. **Performance Optimization**: Performance tuning through backoff and slot management

### Core Design Principles

1. **Layering**: Each layer has clear responsibilities
2. **Synchronization**: Thread safety ensured through proper locking
3. **Extensibility**: VIF-specific and type-specific processing supports various scenarios
4. **Stability**: Multi-level error handling and status validation

Through this design, the NRC7292 driver provides stable and efficient HaLow frame reception.