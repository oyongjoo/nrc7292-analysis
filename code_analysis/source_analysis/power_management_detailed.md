# NRC7292 Power Management Detailed Source Code Analysis

## Overview

This document provides a comprehensive analysis of the power management implementation in the NRC7292 HaLow driver, based on detailed examination of the source code. The driver implements a sophisticated power management system with multiple sleep modes, host-side power control, and IEEE 802.11 standard compliance.

## Power Management Architecture

### 1. Power Mode Definitions

The driver supports four distinct power saving modes defined in `nrc.h`:

```c
enum NRC_PS_MODE {
    NRC_PS_NONE,              // No power saving
    NRC_PS_MODEMSLEEP,        // Modem sleep mode
    NRC_PS_DEEPSLEEP_TIM,     // Deep sleep with TIM listening
    NRC_PS_DEEPSLEEP_NONTIM   // Deep sleep without TIM
};
```

### 2. Driver State Management

The driver maintains power state through multiple state variables:

```c
enum NRC_DRV_STATE {
    NRC_DRV_REBOOT = -2,
    NRC_DRV_BOOT = -1,
    NRC_DRV_INIT = 0,
    NRC_DRV_CLOSED,
    NRC_DRV_CLOSING,
    NRC_DRV_STOP,
    NRC_DRV_START,
    NRC_DRV_RUNNING,
    NRC_DRV_PS,               // Power save state
};
```

## Core Power Management Implementation

### 1. STA Power Save Management (`nrc-pm.c`)

#### PS-Poll Mechanism Implementation

The driver implements PS-Poll handling with sophisticated workarounds for incomplete service periods:

```c
static int tx_h_sta_pm(struct nrc_trx_data *tx)
{
    struct ieee80211_hw *hw = tx->nw->hw;
    struct sk_buff *skb = tx->skb;
    struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
    struct ieee80211_hdr *mh = (void *) skb->data;
    __le16 fc = mh->frame_control;

    // Power management field setting for data frames
    if (!(txi->flags & IEEE80211_TX_CTL_NO_ACK) &&
        ieee80211_is_data(fc) && !ieee80211_has_pm(fc)) {
        mh->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);
        fc = mh->frame_control;
    }

    // PS-Poll state tracking
    if (ieee80211_is_pspoll(fc)) {
        struct nrc_vif *i_vif = to_i_vif(tx->vif);
        i_vif->ps_polling = true;
    }

    return 0;
}
```

#### PS-Poll Service Period Fix

A critical implementation detail is the PS-Poll service period completion workaround:

```c
static int rx_h_fixup_ps_poll_sp(struct nrc_trx_data *rx)
{
    struct ieee80211_hw *hw = rx->nw->hw;
    struct nrc_vif *i_vif = to_i_vif(rx->vif);
    struct ieee80211_hdr *mh = (void *) rx->skb->data;
    __le16 fc = mh->frame_control;

    // Check if in PS-Poll service period
    if (rx->vif->type != NL80211_IFTYPE_STATION || !i_vif->ps_polling)
        return 0;

    // Verify frame is from serving AP
    if (!ether_addr_equal(ieee80211_get_SA(mh), rx->vif->bss_conf.bssid))
        return 0;

    if (ieee80211_is_beacon(fc)) {
        i_vif->ps_polling = false;
        nrc_mac_rx_fictitious_ps_poll_response(rx->vif);
    } else if (ieee80211_is_data(fc))
        i_vif->ps_polling = false;

    return 0;
}
```

### 2. BSS Max Idle Period Implementation

#### Keep-Alive Timer Management

The driver implements BSS Max Idle Period with separate timers for AP and STA modes:

```c
static int sta_h_bss_max_idle_period(struct ieee80211_hw *hw,
                     struct ieee80211_vif *vif,
                     struct ieee80211_sta *sta,
                     enum ieee80211_sta_state old_state,
                     enum ieee80211_sta_state new_state)
{
    struct nrc_vif *i_vif = NULL;
    struct nrc_sta *i_sta = NULL;
    u64 timeout_ms = 0; 
    u32 max_idle_period = 0;
    int idle_offset = 0;

    // State transition validation
    if (state_changed(ASSOC, AUTHORIZED)) {
        max_idle_period = i_sta->max_idle.period;
        
        if (max_idle_period == 0) {
            nrc_mac_dbg("[%s] max_idle_period is 0", __func__);
            return 0;
        }

        // Extended BSS Max Idle Period for S1G
        if (nrc_mac_is_s1g(hw->priv)) {
            u8 usf = (max_idle_period >> 14) & 0x3;
            max_idle_period &= ~0xc000;
            max_idle_period *= ieee80211_usf_to_sf(usf);
        }

        // STA mode: send keep-alive before AP timer expires
        if (vif->type == NL80211_IFTYPE_STATION) {
            if (bss_max_idle_offset == 0) {
                if (max_idle_period > 2)
                    idle_offset = -768; // 768ms margin
                else
                    idle_offset = -100; // 100ms margin
            } else {
                idle_offset = bss_max_idle_offset;
            }
        }

        timeout_ms = (u64)max_idle_period * 1024 + (u64)idle_offset;
        if (timeout_ms < 924) {
            timeout_ms = 924; // Minimum safety threshold
        }

        // Timer setup
        if (vif->type == NL80211_IFTYPE_STATION) {
            timer_setup(&i_vif->max_idle_timer, sta_max_idle_period_expire, 0);
            i_sta->max_idle.idle_period = msecs_to_jiffies(timeout_ms);
            mod_timer(&i_vif->max_idle_timer, jiffies + i_sta->max_idle.idle_period);
        } else {
            // AP mode timer setup
            if (!timer_pending(&i_vif->max_idle_timer)) {
                timer_setup(&i_vif->max_idle_timer, ap_max_idle_period_expire, 0);
                mod_timer(&i_vif->max_idle_timer, 
                         jiffies + msecs_to_jiffies(BSS_MAX_IDLE_TIMER_PERIOD_MS));
            }
            i_sta->max_idle.idle_period = timeout_ms >> 10; // Convert to seconds
        }
    }

    return 0;
}
```

#### QoS Null Keep-Alive Transmission

The STA mode keep-alive implementation sends QoS Null frames:

```c
static void sta_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
    struct ieee80211_hw *hw = i_vif->nw->hw;
    struct sk_buff *skb;
    struct ieee80211_hdr_3addr_qos *qosnullfunc;

    // Create QoS Null frame
    skb = ieee80211_nullfunc_get(hw, i_sta->vif, false);
    skb_put(skb, 2);
    qosnullfunc = (struct ieee80211_hdr_3addr_qos *) skb->data;
    qosnullfunc->frame_control |= cpu_to_le16(IEEE80211_STYPE_QOS_NULL);
    qosnullfunc->qc = cpu_to_le16(0);
    skb_set_queue_mapping(skb, IEEE80211_AC_VO);

    // Transmit keep-alive frame
    nrc_mac_tx(hw, &control, skb);

    // Re-arm timer
    mod_timer(&i_vif->max_idle_timer, jiffies + i_sta->max_idle.idle_period);
}
```

#### AP Mode Inactivity Monitoring

AP mode monitors STA activity and disconnects inactive stations:

```c
static void ap_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
    struct nrc_sta *i_sta = NULL, *tmp = NULL;
    
    spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
    list_for_each_entry_safe(i_sta, tmp, &i_vif->preassoc_sta_list, list) {
        if (i_sta->max_idle.sta_idle_timer) {
            if (--i_sta->max_idle.sta_idle_timer == 0) {
                if (++i_sta->max_idle.timeout_cnt >= BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT) {
                    // Disconnect inactive station
                    ieee80211_disconnect_sta(vif, sta);
                } else {
                    // Re-arm with backoff
                    i_sta->max_idle.sta_idle_timer = i_sta->max_idle.idle_period;
                }
            }
        }
    }
    spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);
}
```

### 3. TX Path Power Management (`nrc-trx.c`)

#### Modem Sleep Wake-up Logic

The TX path includes power management logic for different sleep modes:

```c
void nrc_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
        struct sk_buff *skb)
{
    // Power save handling for STA mode
    if (tx.nw->vif[vif_id]->type == NL80211_IFTYPE_STATION) {
        if (tx.nw->drv_state == NRC_DRV_PS)
            nrc_hif_wake_target(tx.nw->hif);

        // Modem sleep wake-up
        if (power_save == NRC_PS_MODEMSLEEP) {
            if (tx.nw->ps_modem_enabled) {
                struct sk_buff *skb1;
                struct wim_pm_param *p;

                tx.nw->ps_drv_state = false;
                skb1 = nrc_wim_alloc_skb(tx.nw, WIM_CMD_SET,
                    tlv_len(sizeof(struct wim_pm_param)));
                p = nrc_wim_skb_add_tlv(skb1, WIM_TLV_PS_ENABLE,
                    sizeof(struct wim_pm_param), NULL);
                memset(p, 0, sizeof(struct wim_pm_param));
                p->ps_mode = power_save;
                p->ps_enable = tx.nw->ps_drv_state;
                nrc_xmit_wim_request(tx.nw, skb1);
                tx.nw->ps_modem_enabled = false;
            }
        }

        // Deep sleep deauth handling
        if (power_save >= NRC_PS_DEEPSLEEP_TIM) {
            if (tx.nw->drv_state == NRC_DRV_PS) {
                memset(&tx.nw->d_deauth, 0, sizeof(struct nrc_delayed_deauth));
                if (ieee80211_is_deauth(mh->frame_control)) {
                    // Handle deauth during deep sleep
                }
            }
        }
    }
}
```

### 4. WIM Power Management Interface

#### WIM Power Management Parameters

The WIM protocol defines comprehensive power management parameters:

```c
struct wim_pm_param {
    uint8_t ps_mode;                    // Power save mode
    uint8_t ps_enable;                  // Enable/disable flag
    uint16_t ps_wakeup_pin;            // GPIO wake-up pin
    uint64_t ps_duration;              // Sleep duration
    uint32_t ps_timeout;               // Wake-up timeout
    uint8_t wowlan_wakeup_host_pin;    // WoWLAN host wake pin
    uint8_t wowlan_enable_any;         // Any packet wake
    uint8_t wowlan_enable_magicpacket; // Magic packet wake
    uint8_t wowlan_enable_disconnect;  // Disconnect wake
    uint8_t wowlan_n_patterns;         // Number of patterns
    struct wowlan_pattern wp[2];       // Wake patterns
} __packed;
```

#### WoWLAN Pattern Definition

```c
#define WOWLAN_PATTER_SIZE 48

struct wowlan_pattern {
    uint16_t offset:6;        // Packet offset
    uint16_t mask_len:4;      // Mask length
    uint16_t pattern_len:6;   // Pattern length
    uint8_t mask[WOWLAN_PATTER_SIZE/8];      // Bitmask
    uint8_t pattern[WOWLAN_PATTER_SIZE];     // Pattern data
} __packed;
```

### 5. HIF Layer Power Control (`nrc-hif-cspi.c`)

#### GPIO-Based Wake Control

The HIF layer implements GPIO-based target wake-up:

```c
// Device status register definitions
#define EIRQ_DEV_SLEEP  (1<<3)
#define EIRQ_DEV_READY  (1<<2)
#define EIRQ_RXQ        (1<<1)
#define EIRQ_TXQ        (1<<0)

// SPI system register structure
struct spi_sys_reg {
    u8 wakeup;      // 0x0 - Wake-up control
    u8 status;      // 0x1 - Device status
    u16 chip_id;    // 0x2-0x3 - Chip identification
    u32 modem_id;   // 0x4-0x7 - Modem identification
    u32 sw_id;      // 0x8-0xb - Software version
    u32 board_id;   // 0xc-0xf - Board identification
} __packed;
```

## Advanced Power Management Features

### 1. Dynamic Power Save Timer

The driver implements mac80211 dynamic power save with customizable timeout:

```c
struct nrc {
    enum ps_mode {
        PS_DISABLED,
        PS_ENABLED,
        PS_AUTO_POLL,
        PS_MANUAL_POLL
    } ps;
    bool ps_poll_pending;
    bool ps_enabled;
    bool ps_drv_state;
    bool ps_modem_enabled;
    struct timer_list dynamic_ps_timer;
    struct workqueue_struct *ps_wq;
};
```

### 2. Sleep Duration Configuration

The driver supports configurable sleep durations for different power modes:

```c
// Module parameter for sleep duration array
extern int sleep_duration[];

struct wim_sleep_duration_param {
    uint32_t sleep_ms;
} __packed;
```

### 3. Power State Synchronization

The driver implements completion mechanisms for power state transitions:

```c
struct nrc {
    // Power management synchronization
    struct completion hif_tx_stopped;
    struct completion hif_rx_stopped;
    struct completion hif_irq_stopped;
};
```

## IEEE 802.11 Standard Compliance

### 1. Power Management Field Handling

The driver automatically sets the power management field in transmitted frames according to the current power state.

### 2. TIM Element Processing

For deep sleep modes, the driver implements TIM element processing to determine when to wake up and retrieve buffered frames.

### 3. Listen Interval Support

The driver respects the negotiated listen interval and adjusts wake-up timing accordingly.

## Error Handling and Recovery

### 1. Firmware Recovery During Power States

The driver includes mechanisms to handle firmware crashes during power save states and initiate recovery procedures.

### 2. Delayed Deauth Handling

For deep sleep modes, deauthentication frames are handled specially to ensure proper state cleanup:

```c
struct nrc_delayed_deauth {
    atomic_t delayed_deauth;
    s8 vif_index;
    u16 aid;
    bool removed;
    struct sk_buff *deauth_frm;
    // State preservation structures
    struct ieee80211_vif v;
    struct ieee80211_sta s;
    struct ieee80211_key_conf p;
    struct ieee80211_key_conf g;
    struct ieee80211_bss_conf b;
};
```

## Configuration and Tuning

### 1. Module Parameters

Key power management parameters can be configured via module parameters:

- `power_save`: Power save mode selection
- `sleep_duration[]`: Sleep duration array
- `bss_max_idle_offset`: BSS max idle timing offset
- `power_save_gpio[]`: GPIO configuration for power control

### 2. Dynamic Configuration

Runtime configuration is supported through netlink interface and debugfs entries.

## Performance Considerations

### 1. Wake-up Latency

The driver optimizes wake-up latency by pre-loading critical data structures and maintaining minimal wake-up paths.

### 2. Power Consumption Optimization

Different sleep modes provide varying levels of power savings vs. responsiveness trade-offs:

- **MODEMSLEEP**: Fast wake-up, moderate power savings
- **DEEPSLEEP_TIM**: Balanced power savings with TIM monitoring
- **DEEPSLEEP_NONTIM**: Maximum power savings, manual wake-up required

### 3. Throughput Impact

The driver includes mechanisms to temporarily disable power save during high-throughput periods to maintain performance.

## Conclusion

The NRC7292 power management implementation provides a comprehensive solution that balances power efficiency with network performance. The multi-layered approach, from HIF-level GPIO control to high-level IEEE 802.11 compliance, ensures robust operation across various use cases while maintaining compatibility with standard wireless infrastructure.

The sophisticated BSS Max Idle Period implementation, combined with flexible sleep modes and WoWLAN support, makes this driver suitable for IoT and low-power applications where extended battery life is critical.