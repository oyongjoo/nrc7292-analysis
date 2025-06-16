# NRC7292 Driver Data Flow and Communication Paths Detailed Analysis

## Overview

This document provides a comprehensive analysis of the complete data flow and communication paths in the NRC7292 HaLow driver. Based on actual source code implementation, it covers TX/RX data processing, management frame handling, Netlink communication, WIM protocol, buffer management, queue management, and more.

## 1. Complete Data Flow Paths

### 1.1 TX Data Flow (Transmit Path)

```
User Application
    ↓ (socket write)
Network Stack (TCP/UDP/IP)
    ↓ (netdev_tx)
mac80211 subsystem
    ↓ (ieee80211_tx)
NRC Driver (nrc_mac_tx)
    ↓ (TX Handler Chain)
HIF Layer (nrc_xmit_frame)
    ↓ (CSPI Interface)
Hardware (NRC7292 Chipset)
    ↓ (Wireless Transmission)
Air (RF Signal)
```

#### 1.1.1 Detailed TX Flow (`nrc-trx.c::nrc_mac_tx`)

**Step 1: Entry Point and Initial Validation**
```c
void nrc_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
                struct sk_buff *skb)
{
    struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
    struct nrc_trx_data tx = {
        .nw = hw->priv,
        .vif = txi->control.vif,
        .sta = control->sta,
        .skb = skb,
        .result = 0,
    };
    
    // VIF validation
    if (!nrc_is_valid_vif(tx.nw, tx.vif))
        goto txh_out;
```

**Step 2: Power Management Handling**
```c
// Check power save state in STA mode
if (tx.nw->vif[vif_id]->type == NL80211_IFTYPE_STATION) {
    if (tx.nw->drv_state == NRC_DRV_PS)
        nrc_hif_wake_target(tx.nw->hif);
        
    // Handle MODEM SLEEP mode
    if (power_save == NRC_PS_MODEMSLEEP) {
        if (tx.nw->ps_modem_enabled) {
            // Wake up modem via WIM command
            skb1 = nrc_wim_alloc_skb(tx.nw, WIM_CMD_SET, ...);
            p = nrc_wim_skb_add_tlv(skb1, WIM_TLV_PS_ENABLE, ...);
            nrc_xmit_wim_request(tx.nw, skb1);
        }
    }
}
```

**Step 3: AMPDU Session Setup**
```c
// Set up BA session in auto AMPDU mode
if (ampdu_mode == NRC_AMPDU_AUTO) {
    if (ieee80211_is_data_qos(mh->frame_control) &&
        !is_multicast_ether_addr(mh->addr1) && !is_eapol(tx.skb)) {
        setup_ba_session(tx.nw, tx.vif, tx.skb);
    }
}
```

**Step 4: TX Handler Chain Execution**
```c
// Execute TX handlers sequentially
for (h = &__tx_h_start; h < &__tx_h_end; h++) {
    if (!(h->vif_types & BIT(tx.vif->type)))
        continue;
        
    res = h->handler(&tx);
    if (res < 0)
        goto txh_out;
}
```

**Key TX Handlers:**
- `tx_h_debug_print`: Debug frame printing
- `tx_h_debug_state`: STA state validation
- `tx_h_wfa_halow_filter`: HaLow frame filtering
- `tx_h_frame_filter`: General frame filtering
- `tx_h_put_iv`: IV header space allocation (for encryption)
- `tx_h_put_qos_control`: Convert non-QoS data to QoS data

**Step 5: Frame Transmission**
```c
if (!atomic_read(&tx.nw->d_deauth.delayed_deauth))
    nrc_xmit_frame(tx.nw, vif_id, (!!tx.sta ? tx.sta->aid : 0), tx.skb);
```

### 1.2 RX Data Flow (Receive Path)

```
Air (RF Signal)
    ↓ (Wireless Reception)
Hardware (NRC7292 Chipset)
    ↓ (CSPI Interface)
HIF Layer (nrc_hif_receive)
    ↓ (Frame Header Parsing)
NRC Driver (nrc_mac_rx)
    ↓ (RX Handler Chain)
mac80211 subsystem
    ↓ (ieee80211_rx_irqsafe)
Network Stack
    ↓ (Socket Buffer)
User Application
```

#### 1.2.1 Detailed RX Flow (`nrc-trx.c::nrc_mac_rx`)

**Step 1: Received Frame Validation**
```c
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    struct nrc_trx_data rx = { .nw = nw, .skb = skb, };
    struct ieee80211_hdr *mh;
    __le16 fc;
    
    // Check driver state
    if (!((nw->drv_state == NRC_DRV_RUNNING) ||
          (nw->drv_state == NRC_DRV_PS)) ||
          atomic_read(&nw->d_deauth.delayed_deauth)) {
        dev_kfree_skb(skb);
        return 0;
    }
```

**Step 2: RX Status Information Setup**
```c
// Extract receive status from frame header
nrc_mac_rx_h_status(nw, skb);

static void nrc_mac_rx_h_status(struct nrc *nw, struct sk_buff *skb)
{
    struct frame_hdr *fh = (void *)skb->data;
    struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    
    memset(status, 0, sizeof(*status));
    status->signal = fh->flags.rx.rssi;
    status->freq = fh->info.rx.frequency / 10;  // For S1G channels
    status->band = nw->band;
    
    if (fh->flags.rx.error_mic)
        status->flag |= RX_FLAG_MMIC_ERROR;
    if (fh->flags.rx.iv_stripped)
        status->flag |= RX_FLAG_IV_STRIPPED;
}
```

**Step 3: Monitor Mode Handling**
```c
if (nw->promisc) {
    ret = nrc_mac_s1g_monitor_rx(nw, skb);
    return ret;
}
```

**Step 4: Frame Header Removal and Interface Iteration**
```c
// Remove frame header
skb_pull(skb, nw->fwinfo.rx_head_size - sizeof(struct hif));
mh = (void*)skb->data;
fc = mh->frame_control;

// Iterate over active interfaces and execute RX handlers
ieee80211_iterate_interfaces(nw->hw, IEEE80211_IFACE_ITER_ACTIVE,
                             nrc_rx_handler, &rx);
```

**Step 5: RX Handler Chain Execution**
```c
static void nrc_rx_handler(void *data, u8 *mac, struct ieee80211_vif *vif)
{
    struct nrc_trx_data *rx = data;
    struct nrc_trx_handler *h;
    
    // Execute RX handlers sequentially
    for (h = &__rx_h_start; h < &__rx_h_end; h++) {
        if (!(h->vif_types & BIT(vif->type)))
            continue;
            
        res = h->handler(rx);
        if (res < 0)
            goto rxh_out;
    }
}
```

**Key RX Handlers:**
- `rx_h_vendor`: Vendor specific IE processing
- `rx_h_decrypt`: Hardware decryption post-processing
- `rx_h_check_sn`: Sequence number validation (for BA sessions)
- `rx_h_ibss_get_bssid_tsf`: IBSS mode TSF handling
- `rx_h_mesh`: Mesh network RSSI filtering

**Step 6: Frame Delivery to mac80211**
```c
if (!rx.result) {
    ieee80211_rx_irqsafe(nw->hw, rx.skb);
    
    // Update dynamic power save timer
    if (ieee80211_hw_check(nw->hw, SUPPORTS_DYNAMIC_PS)) {
        if (ieee80211_is_data(fc) && nw->ps_enabled &&
            nw->hw->conf.dynamic_ps_timeout > 0) {
            mod_timer(&nw->dynamic_ps_timer,
                     jiffies + msecs_to_jiffies(nw->hw->conf.dynamic_ps_timeout));
        }
    }
}
```

### 1.3 Management Frame Processing

#### 1.3.1 Beacon Frame Processing
```c
if (ieee80211_is_beacon(fc)) {
    if (!disable_cqm && rx->nw->associated_vif) {
        // Update beacon monitoring timer
        mod_timer(&rx->nw->bcn_mon_timer,
                 jiffies + msecs_to_jiffies(rx->nw->beacon_timeout));
    }
}
```

#### 1.3.2 Association Frame Processing
```c
if ((ieee80211_is_assoc_req(mh->frame_control) ||
     ieee80211_is_reassoc_req(mh->frame_control)) && rx.sta) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
    
    // Store listen interval
    i_sta->listen_interval = mgmt->u.assoc_req.listen_interval;
}
```

#### 1.3.3 Authentication Frame Special Handling
```c
// Handle Auth frame from already connected STA in AP mode
if (ieee80211_is_auth(mh->frame_control) && rx.sta && 
    rx.vif && rx.vif->type == NL80211_IFTYPE_AP) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    
    if(i_sta && i_sta->state > IEEE80211_STA_NOTEXIST){
        // Convert to Deauth frame for processing
        skb_deauth = ieee80211_deauth_get(nw->hw, mgmt->bssid, 
                                         mgmt->sa, mgmt->bssid, 
                                         WLAN_REASON_DEAUTH_LEAVING, NULL, false);
        ieee80211_rx_irqsafe(nw->hw, skb_deauth);
        dev_kfree_skb(skb);
        return 0;
    }
}
```

### 1.4 Control Frame Processing

#### 1.4.1 Block ACK Control
```c
static void setup_ba_session(struct nrc *nw, struct ieee80211_vif *vif, 
                             struct sk_buff *skb)
{
    struct ieee80211_sta *peer_sta = NULL;
    struct nrc_sta *i_sta = NULL;
    struct ieee80211_hdr *qmh = (struct ieee80211_hdr *) skb->data;
    int tid = *ieee80211_get_qos_ctl(qmh) & IEEE80211_QOS_CTL_TID_MASK;
    
    peer_sta = ieee80211_find_sta(vif, qmh->addr1);
    if (!peer_sta) return;
    
    i_sta = to_i_sta(peer_sta);
    
    switch (i_sta->tx_ba_session[tid]) {
        case IEEE80211_BA_NONE:
        case IEEE80211_BA_CLOSE:
            nw->ampdu_supported = true;
            nw->ampdu_reject = false;
            if((ret = ieee80211_start_tx_ba_session(peer_sta, tid, 0)) != 0) {
                // Error handling
            }
            break;
    }
}
```

#### 1.4.2 Sequence Number Validation
```c
static int rx_h_check_sn(struct nrc_trx_data *rx)
{
    // Detect and handle sequence number inversion in BA sessions
    if (ieee80211_sn_less(sn, i_sta->rx_ba_session[tid].sn) &&
        sn_diff <= IEEE80211_SN_MODULO - i_sta->rx_ba_session[tid].buf_size) {
        ieee80211_mark_rx_ba_filtered_frames(rx->sta, tid, sn, 0, 
                                            IEEE80211_SN_MODULO >> 1);
    }
    i_sta->rx_ba_session[tid].sn = sn;
}
```

## 2. Communication Path Analysis

### 2.1 Netlink Communication between Kernel Driver and CLI Application

#### 2.1.1 Netlink Architecture
```
CLI Application (User Space)
    ↓ Netlink Socket
Kernel Netlink Subsystem
    ↓ Generic Netlink
NRC Driver Netlink Handler
    ↓ Command Processing
Driver Internal Functions
```

#### 2.1.2 CLI Application Netlink Implementation
```c
// cli_netlink.c
int netlink_send_data(char cmd_type, char* param, char* response)
{
    int nl_fd;
    struct sockaddr_nl nl_address;
    int nl_family_id = 0;
    nl_msg_t nl_request_msg, nl_response_msg;
    
    // Create Netlink socket
    nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    
    // Compose request message
    nl_request_msg.n.nlmsg_type = nl_family_id;
    nl_request_msg.g.cmd = cmd_type;
    
    // Send data
    nl_rxtx_length = sendto(nl_fd, (char *)&nl_request_msg, 
                           nl_request_msg.n.nlmsg_len, 0,
                           (struct sockaddr *)&nl_address, 
                           sizeof(nl_address));
    
    // Receive response
    nl_rxtx_length = recv(nl_fd, &nl_response_msg, sizeof(nl_response_msg), 0);
}
```

#### 2.1.3 Driver Netlink Handler
```c
// nrc-netlink.c
static int nrc_netlink_shell(struct sk_buff *skb, struct genl_info *info)
{
    struct nrc *nw = nrc_wdev_to_nw(info->user_ptr[0]);
    int ret = 0;
    
    if (info->attrs[NL_SHELL_RUN_CMD]) {
        char *cmd = nla_data(info->attrs[NL_SHELL_RUN_CMD]);
        ret = nrc_netlink_shell_run(nw, info, cmd);
    }
    
    return ret;
}

static const struct genl_ops nrc_ops[] = {
    {
        .cmd = NL_SHELL_RUN,
        .flags = 0,
        .policy = nrc_genl_policy,
        .doit = nrc_netlink_shell,
    },
    // Other commands...
};
```

### 2.2 WIM Protocol Command/Response Flow

#### 2.2.1 WIM Protocol Structure
```c
struct wim {
    u16 cmd;        // Command code
    u16 seqno;      // Sequence number
    u8 payload[0];  // TLV payload
} __packed;

struct wim_tlv {
    u16 t;          // Type
    u16 l;          // Length
    u8 v[0];        // Value
} __packed;
```

#### 2.2.2 WIM Command Creation and Transmission
```c
// wim.c
struct sk_buff *nrc_wim_alloc_skb(struct nrc *nw, u16 cmd, int size)
{
    struct sk_buff *skb;
    struct wim *wim;
    
    skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
    if (!skb) return NULL;
    
    // Reserve space for HIF header
    skb_reserve(skb, sizeof(struct hif));
    
    // Create WIM header
    wim = (struct wim *)skb_put(skb, sizeof(*wim));
    memset(wim, 0, sizeof(*wim));
    wim->cmd = cmd;
    wim->seqno = nw->wim_seqno++;
    
    return skb;
}

void *nrc_wim_skb_add_tlv(struct sk_buff *skb, u16 T, u16 L, void *V)
{
    struct wim_tlv *tlv;
    
    tlv = (struct wim_tlv *)(skb_put(skb, tlv_len(L)));
    tlv->t = T;
    tlv->l = L;
    
    if (V) memcpy(tlv->v, V, L);
    
    return (void *)tlv->v;
}
```

#### 2.2.3 WIM Response Handling
```c
static int nrc_wim_response_handler(struct nrc *nw, struct sk_buff *skb)
{
    mutex_lock(&nw->target_mtx);
    nw->last_wim_responded = skb;
    mutex_unlock(&nw->target_mtx);
    
    if (completion_done(&nw->wim_responded)) {
        // No waiters, free SKB
        return 0;
    }
    
    complete(&nw->wim_responded);
    return 1;  // Free SKB later
}
```

### 2.3 Event Notification Paths

#### 2.3.1 WIM Event Processing
```c
static int nrc_wim_event_handler(struct nrc *nw, struct sk_buff *skb)
{
    struct ieee80211_vif *vif = NULL;
    struct wim *wim = (void *) skb->data;
    struct hif *hif = (void *)(skb->data - sizeof(*hif));
    
    // Find VIF from HIF VIF index
    if (hif->vifindex != -1)
        vif = nw->vif[hif->vifindex];
    
    switch (wim->event) {
        case WIM_EVENT_SCAN_COMPLETED:
            nrc_mac_cancel_hw_scan(nw->hw, vif);
            break;
            
        case WIM_EVENT_READY:
            nrc_wim_handle_fw_ready(nw);
            break;
            
        case WIM_EVENT_CREDIT_REPORT:
            nrc_wim_update_tx_credit(nw, wim);
            break;
            
        case WIM_EVENT_CSA:
            ieee80211_csa_finish(vif);
            break;
            
        case WIM_EVENT_CH_SWITCH:
            ieee80211_chswitch_done(vif, true);
            break;
    }
    
    return 0;
}
```

### 2.4 CSPI Register Access Patterns

#### 2.4.1 CSPI Command Structure
```c
// C-SPI command format
// [31:24]: start byte (0x50)
// [23:23]: burst (0: single, 1: burst)
// [22:22]: direction (0: read, 1: write)
// [21:21]: fixed (0: incremental, 1: fixed)
// [20:13]: address
// [12:0]: length (for multi-word transfer)

#define C_SPI_READ     0x50000000
#define C_SPI_WRITE    0x50400000
#define C_SPI_BURST    0x00800000
#define C_SPI_FIXED    0x00200000
#define C_SPI_ADDR(x)  (((x) & 0xff) << 13)
#define C_SPI_LEN(x)   ((x) & 0x1fff)
```

#### 2.4.2 Status Register Structure
```c
struct spi_status_reg {
    struct {
        u8 mode;
        u8 enable;
        u8 latched_status;
        u8 status;
    } eirq;
    u8 txq_status[6];    // TX queue status
    u8 rxq_status[6];    // RX queue status
    u32 msg[4];          // Messages
} __packed;
```

## 3. Buffer Management

### 3.1 SKB (Socket Buffer) Lifecycle

#### 3.1.1 TX Path SKB Management
```c
// 1. Receive SKB from mac80211
void nrc_mac_tx(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    // 2. SKB modification in TX handlers
    for (h = &__tx_h_start; h < &__tx_h_end; h++) {
        res = h->handler(&tx);  // SKB can be modified
    }
    
    // 3. Send to hardware
    nrc_xmit_frame(tx.nw, vif_id, aid, tx.skb);
    
    return;
    
txh_out:
    // 4. Free SKB on error
    if (tx.skb)
        dev_kfree_skb(tx.skb);
}
```

#### 3.1.2 RX Path SKB Management
```c
// 1. Receive SKB from hardware
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    // 2. Set status information
    nrc_mac_rx_h_status(nw, skb);
    
    // 3. Process SKB in RX handlers
    ieee80211_iterate_interfaces(nw->hw, IEEE80211_IFACE_ITER_ACTIVE,
                                 nrc_rx_handler, &rx);
    
    // 4. Hand over to mac80211 (transfer ownership)
    if (!rx.result) {
        ieee80211_rx_irqsafe(nw->hw, rx.skb);
    }
    
    return 0;
}
```

#### 3.1.3 WIM SKB Management
```c
// WIM command SKB allocation
struct sk_buff *nrc_wim_alloc_skb(struct nrc *nw, u16 cmd, int size)
{
    struct sk_buff *skb;
    
    // Allocate including HIF + WIM header size
    skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
    if (!skb) return NULL;
    
    // Reserve headroom for HIF header
    skb_reserve(skb, sizeof(struct hif));
    
    return skb;
}

// WIM response waiting
struct sk_buff *nrc_xmit_wim_request_wait(struct nrc *nw, struct sk_buff *skb, 
                                          int timeout)
{
    struct sk_buff *rsp;
    
    // Send request
    nrc_xmit_wim_request(nw, skb);
    
    // Wait for response
    if (wait_for_completion_timeout(&nw->wim_responded,
                                   msecs_to_jiffies(timeout)) == 0) {
        return NULL;  // Timeout
    }
    
    mutex_lock(&nw->target_mtx);
    rsp = nw->last_wim_responded;
    nw->last_wim_responded = NULL;
    mutex_unlock(&nw->target_mtx);
    
    return rsp;
}
```

### 3.2 DMA Buffer Management

NRC7292 uses SPI interface so there's no direct DMA, but SPI transfer buffer management is crucial.

#### 3.2.1 SPI Transfer Buffers
```c
#define SPI_BUFFER_SIZE (496-20)

struct nrc_spi_priv {
    struct spi_device *spi;
    
    // Slot management
    struct {
        u16 head;
        u16 tail;
        u16 size;
        u16 count;
    } slot[2];  // TX_SLOT(0), RX_SLOT(1)
    
    // Credit queue management
    int hw_queues;
    u8 front[CREDIT_QUEUE_MAX];
    u8 rear[CREDIT_QUEUE_MAX];
    u8 credit_max[CREDIT_QUEUE_MAX];
    
    struct mutex bus_lock_mutex;
};
```

#### 3.2.2 SPI Transfer Functions
```c
static inline void spi_set_transfer(struct spi_transfer *xfer,
                                   void *tx, void *rx, int len)
{
    xfer->tx_buf = tx;
    xfer->rx_buf = rx;
    xfer->len = len;
}

static ssize_t c_spi_write_data(struct spi_device *spi, u8 *buf, ssize_t size)
{
    struct spi_transfer xfer[2];
    struct spi_message msg;
    u32 cmd;
    ssize_t ret;
    
    cmd = C_SPI_WRITE | C_SPI_BURST | C_SPI_ADDR(C_SPI_TXQ_WINDOW) | 
          C_SPI_LEN(size);
    cmd = cpu_to_be32(cmd);
    
    // Send command
    spi_set_transfer(&xfer[0], &cmd, NULL, sizeof(cmd));
    // Send data
    spi_set_transfer(&xfer[1], buf, NULL, size);
    
    spi_message_init(&msg);
    spi_message_add_tail(&xfer[0], &msg);
    spi_message_add_tail(&xfer[1], &msg);
    
    ret = spi_sync(spi, &msg);
    
    return ret < 0 ? ret : size;
}
```

### 3.3 Memory Allocation Strategy

#### 3.3.1 SKB Allocation Priority
```c
// 1. Normal network traffic: GFP_ATOMIC (interrupt context)
skb = dev_alloc_skb(size);

// 2. WIM commands: GFP_KERNEL (process context allowed)
skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));

// 3. Emergency copy
skb_copy = skb_copy(skb, GFP_ATOMIC);
```

#### 3.3.2 Memory Free Patterns
```c
// TX error
txh_out:
    if (tx.skb)
        dev_kfree_skb(tx.skb);

// RX error  
if (nw->drv_state != NRC_DRV_RUNNING) {
    dev_kfree_skb(skb);
    return 0;
}

// After WIM response
if (ret != 1)  // Not freeing later
    dev_kfree_skb(skb);
```

## 4. Queue Management

### 4.1 AC (Access Category) Queue Processing

#### 4.1.1 AC Queue Definitions
```c
// Credit definitions
#define TCN  (2*1)
#define TCNE (0)
#define CREDIT_AC0    (TCN*2+TCNE)   /* BK (4) */
#define CREDIT_AC1    (TCN*20+TCNE)  /* BE (40) */
#define CREDIT_AC2    (TCN*4+TCNE)   /* VI (8) */
#define CREDIT_AC3    (TCN*4+TCNE)   /* VO(8) */

// Queue structure: VIF0(AC0~AC3), BCN, CONC, VIF1(AC0~AC3), padding
#define CREDIT_QUEUE_MAX (12)
```

#### 4.1.2 Queue State Management
```c
struct nrc {
    spinlock_t txq_lock;
    struct list_head txq;
    
    // Credit management
    atomic_t tx_credit[IEEE80211_NUM_ACS*3];
    atomic_t tx_pend[IEEE80211_NUM_ACS*3];
};

void nrc_mac_trx_init(struct nrc *nw)
{
    int i;
    
    spin_lock_init(&nw->txq_lock);
    INIT_LIST_HEAD(&nw->txq);
    
    for (i = 0; i < ARRAY_SIZE(nw->tx_credit); i++) {
        atomic_set(&nw->tx_credit[i], 0);
        atomic_set(&nw->tx_pend[i], 0);
    }
}
```

### 4.2 Credit-based Flow Control Mechanism

#### 4.2.1 Credit Update
```c
static int nrc_wim_update_tx_credit(struct nrc *nw, struct wim *wim)
{
    struct wim_credit_report *r = (void *)(wim + 1);
    int ac;
    
    // Update credits for all ACs
    for (ac = 0; ac < (IEEE80211_NUM_ACS*3); ac++)
        atomic_set(&nw->tx_credit[ac], r->v.ac[ac]);
    
    // Kick TX queue
    nrc_kick_txq(nw);
    
    return 0;
}
```

#### 4.2.2 Credit Consumption
```c
static bool nrc_check_credit(struct nrc *nw, int ac)
{
    return atomic_read(&nw->tx_credit[ac]) > 0;
}

static void nrc_consume_credit(struct nrc *nw, int ac)
{
    atomic_dec(&nw->tx_credit[ac]);
    atomic_inc(&nw->tx_pend[ac]);
}
```

#### 4.2.3 Credit Information Reading
```c
void nrc_hif_cspi_read_credit(struct nrc_hif_device *hdev, int q, 
                              int *p_front, int *p_rear, int *p_credit)
{
    struct nrc_spi_priv *priv = hdev->priv;
    *p_front = priv->front[q];
    *p_rear = priv->rear[q];
    *p_credit = priv->credit_max[q];
}
```

### 4.3 Congestion Control and Backpressure

#### 4.3.1 TXQ Backoff
```c
#ifdef CONFIG_TRX_BACKOFF
struct nrc_spi_priv {
    atomic_t trx_backoff;
};

static void nrc_apply_backoff(struct nrc_spi_priv *priv)
{
    if (atomic_read(&priv->trx_backoff) > 0) {
        msleep(1);  // Apply backoff
        atomic_dec(&priv->trx_backoff);
    }
}
#endif
```

#### 4.3.2 Queue Stop/Restart
```c
// Notify mac80211 to stop queues
ieee80211_stop_queues(nw->hw);

// Restart queues after credit recovery
ieee80211_wake_queues(nw->hw);

// Control specific AC queue only
ieee80211_stop_queue(nw->hw, ac);
ieee80211_wake_queue(nw->hw, ac);
```

## 5. Error and Exception Paths

### 5.1 TX/RX Flow Error Handling

#### 5.1.1 TX Error Handling
```c
void nrc_mac_tx(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    // VIF validation failure
    if (!nrc_is_valid_vif(tx.nw, tx.vif)) {
        nrc_mac_dbg("[%s] Invalid vif", __func__);
        goto txh_out;
    }
    
    // Driver state check
    if (tx.nw->drv_state != NRC_DRV_RUNNING) {
        if (ieee80211_is_deauth(mh->frame_control)) {
            nrc_ps_dbg("[%s] deauth in wrong state:%d", 
                      __func__, tx.nw->drv_state);
        }
        goto txh_out;
    }
    
    // TX handler error
    for (h = &__tx_h_start; h < &__tx_h_end; h++) {
        res = h->handler(&tx);
        if (res < 0) {
            nrc_mac_dbg("TX handler failed: %d", res);
            goto txh_out;
        }
    }
    
    return;
    
txh_out:
    // Free SKB on error
    if (tx.skb)
        dev_kfree_skb(tx.skb);
}
```

#### 5.1.2 RX Error Handling
```c
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    // Driver state check
    if (!((nw->drv_state == NRC_DRV_RUNNING) ||
          (nw->drv_state == NRC_DRV_PS))) {
        nrc_mac_dbg("Target not ready, discarding frame");
        dev_kfree_skb(skb);
        return 0;
    }
    
    // Delayed deauth state check
    if (atomic_read(&nw->d_deauth.delayed_deauth)) {
        dev_kfree_skb(skb);
        return 0;
    }
    
    // RX handler error
    if (rx.result) {
        // SKB already freed on error
        return 0;
    }
    
    // Normal processing: hand over to mac80211 (transfer ownership)
    ieee80211_rx_irqsafe(nw->hw, rx.skb);
    return 0;
}
```

#### 5.1.3 Encryption Error Handling
```c
static int rx_h_decrypt(struct nrc_trx_data *rx)
{
    struct ieee80211_key_conf *key;
    
    // No key case
    if (!key) {
        if (nw->cap.vif_caps[vif_id].cap_mask & WIM_SYSTEM_CAP_HYBRIDSEC) {
            // Fallback to SW decryption in HYBRID SEC mode
            return 0;
        }
        nrc_dbg(NRC_DBG_MAC, "key is NULL");
        return 0;
    }
    
    // Unsupported cipher
    switch (key->cipher) {
        case WLAN_CIPHER_SUITE_CCMP:
            // Supported
            break;
        default:
            nrc_dbg(NRC_DBG_MAC, "%s: unknown cipher (%d)", 
                   __func__, key->cipher);
            return 0;
    }
    
    // Mark as successfully decrypted
    status = IEEE80211_SKB_RXCB(rx->skb);
    status->flag |= RX_FLAG_DECRYPTED;
    status->flag |= RX_FLAG_MMIC_STRIPPED;
    
    return 0;
}
```

### 5.2 Recovery Mechanisms

#### 5.2.1 Firmware Reload
```c
void nrc_wim_handle_fw_reload(struct nrc *nw)
{
    nrc_ps_dbg("[%s] FW reload requested", __func__);
    
    // Set firmware state to loading
    atomic_set(&nw->fw_state, NRC_FW_LOADING);
    
    // Clean up HIF
    nrc_hif_cleanup(nw->hif);
    
    // Re-download firmware
    if (nrc_check_fw_file(nw)) {
        nrc_download_fw(nw);
        nw->hif->hif_ops->config(nw->hif);
        msleep(500);
        nrc_release_fw(nw);
    }
}
```

#### 5.2.2 STA Connection Recovery (AP mode)
```c
static void prepare_deauth_sta(void *data, struct ieee80211_sta *sta)
{
    struct nrc_sta *i_sta = to_i_sta(sta);
    struct ieee80211_hw *hw = i_sta->nw->hw;
    struct ieee80211_vif *vif = data;
    struct sk_buff *skb = NULL;
    
    if (!ieee80211_find_sta(vif, sta->addr))
        return;
    
    // Force create Deauth frame
    skb = ieee80211_deauth_get(hw, vif->addr, sta->addr, vif->addr,
                              WLAN_REASON_DEAUTH_LEAVING, sta, false);
    if (!skb) {
        nrc_dbg(NRC_DBG_STATE, "%s Fail to alloc skb", __func__);
        return;
    }
    
    nrc_dbg(NRC_DBG_STATE, "(AP Recovery) Disconnect STA(%pM) by force", 
           sta->addr);
    ieee80211_rx_irqsafe(hw, skb);
}
```

#### 5.2.3 Beacon Loss Handling
```c
static void nrc_send_beacon_loss(struct nrc *nw)
{
    if (nw->associated_vif) {
        nrc_dbg(NRC_DBG_STATE, "Sending beacon loss event");
        ieee80211_beacon_loss(nw->associated_vif);
    }
}

// When beacon loss detected in RX
if (nw->invoke_beacon_loss) {
    nw->invoke_beacon_loss = false;
    nrc_send_beacon_loss(nw);
}
```

### 5.3 Timeout Handling

#### 5.3.1 WIM Response Timeout
```c
struct sk_buff *nrc_xmit_wim_request_wait(struct nrc *nw, struct sk_buff *skb,
                                          int timeout)
{
    struct sk_buff *rsp;
    
    // Send request
    nrc_xmit_wim_request(nw, skb);
    
    // Wait for response (with timeout)
    if (wait_for_completion_timeout(&nw->wim_responded,
                                   msecs_to_jiffies(timeout)) == 0) {
        nrc_dbg(NRC_DBG_WIM, "WIM request timeout");
        return NULL;
    }
    
    // Return response SKB
    mutex_lock(&nw->target_mtx);
    rsp = nw->last_wim_responded;
    nw->last_wim_responded = NULL;
    mutex_unlock(&nw->target_mtx);
    
    return rsp;
}
```

#### 5.3.2 Beacon Monitoring Timeout
```c
// Set beacon monitoring timer
if (!disable_cqm && rx->nw->associated_vif) {
    mod_timer(&rx->nw->bcn_mon_timer,
             jiffies + msecs_to_jiffies(rx->nw->beacon_timeout));
}

// In timeout handler
static void nrc_beacon_timeout_handler(struct timer_list *t)
{
    struct nrc *nw = from_timer(nw, t, bcn_mon_timer);
    
    nrc_dbg(NRC_DBG_STATE, "Beacon timeout - connection lost");
    ieee80211_connection_loss(nw->associated_vif);
}
```

#### 5.3.3 Dynamic Power Save Timeout
```c
// Update timer on data TX/RX
if (ieee80211_hw_check(hw, SUPPORTS_DYNAMIC_PS) &&
    hw->conf.dynamic_ps_timeout > 0) {
    mod_timer(&tx.nw->dynamic_ps_timer,
             jiffies + msecs_to_jiffies(hw->conf.dynamic_ps_timeout));
}

// Enter power save mode on timeout
static void nrc_dynamic_ps_timeout_handler(struct timer_list *t)
{
    struct nrc *nw = from_timer(nw, t, dynamic_ps_timer);
    
    nrc_dbg(NRC_DBG_PS, "Entering dynamic PS mode");
    ieee80211_request_smps(nw->associated_vif, IEEE80211_SMPS_DYNAMIC);
}
```

## 6. Performance Optimization Considerations

### 6.1 Interrupt Processing Optimization
```c
// IRQ handler performs minimal work
static irqreturn_t nrc_spi_irq_handler(int irq, void *data)
{
    struct nrc_spi_priv *priv = data;
    
    // Disable IRQ
    disable_irq_nosync(irq);
    
    // Delegate actual processing to work queue
    queue_work(priv->irq_wq, &priv->irq_work);
    
    return IRQ_HANDLED;
}
```

### 6.2 Memory Pooling
```c
// Retry logic on SKB allocation failure
static struct sk_buff *nrc_alloc_skb_retry(int size, int max_retry)
{
    struct sk_buff *skb;
    int retry = 0;
    
    do {
        skb = dev_alloc_skb(size);
        if (skb) return skb;
        
        if (retry++ < max_retry) {
            msleep(1);  // Brief wait before retry
        }
    } while (retry < max_retry);
    
    return NULL;
}
```

### 6.3 Batch Processing
```c
// Process multiple frames at once
static int nrc_process_rx_batch(struct nrc *nw, struct sk_buff_head *skb_list)
{
    struct sk_buff *skb;
    
    while ((skb = skb_dequeue(skb_list)) != NULL) {
        nrc_mac_rx(nw, skb);
    }
    
    return 0;
}
```

## Conclusion

The NRC7292 driver implements HaLow networking through a complex multi-layer architecture. The core data flow serves as an efficient relay between the mac80211 subsystem and CSPI hardware interface, providing control plane via WIM protocol and user space communication via Netlink.

Key highlights include:

1. **Layered Error Handling**: Appropriate error handling and recovery mechanisms at each layer
2. **Efficient Buffer Management**: Zero-copy optimization through SKB lifecycle management
3. **Credit-based Flow Control**: Prevention of hardware queue overflow
4. **Asynchronous Processing**: Non-blocking architecture using IRQ and Work queues
5. **Power Management Integration**: Seamless integration with various power save modes

This design effectively supports IEEE 802.11ah HaLow's low-power, long-range communication requirements while ensuring compatibility with the Linux networking stack.