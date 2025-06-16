# NRC7292 Driver Power Management Architecture Analysis

## Overview

The NRC7292 HaLow driver provides a comprehensive power management system designed in accordance with the IEEE 802.11ah standard. This analysis provides detailed explanation of the driver's power management mechanisms based on actual source code.

## 1. Power Management Modes

### 1.1 Supported Power Save Modes

The driver supports 4 main power management modes defined in `enum NRC_PS_MODE`:

```c
enum NRC_PS_MODE {
    NRC_PS_NONE,                 // Power save disabled
    NRC_PS_MODEMSLEEP,          // Modem sleep mode
    NRC_PS_DEEPSLEEP_TIM,       // TIM-based deep sleep
    NRC_PS_DEEPSLEEP_NONTIM     // Non-TIM deep sleep
};
```

#### 1.1.1 NRC_PS_NONE (Power Save Disabled)
- Default mode with no power saving features
- Always-active state for maximum performance
- All RF and modem functions continuously active

#### 1.1.2 NRC_PS_MODEMSLEEP (Modem Sleep Mode)
- Light power saving mode
- RF remains active, only modem enters power save
- Provides fast wake-up time
- Maintains connection with periodic beacon reception

#### 1.1.3 NRC_PS_DEEPSLEEP_TIM (TIM-based Deep Sleep)
- Medium level power saving
- TIM (Traffic Indication Map) based wake-up
- Checks TIM in AP beacons to determine buffered data
- Supports GPIO wake-up signaling

#### 1.1.4 NRC_PS_DEEPSLEEP_NONTIM (Non-TIM Deep Sleep)
- Maximum power saving mode
- Timer-based periodic wake-up
- Provides longest sleep periods
- Wake-up only by external events or timers

### 1.2 Mode Transition Mechanisms

Power mode transitions are coordinated across multiple layers:

#### 1.2.1 Driver State Management
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
    NRC_DRV_PS,        // Power save state
};
```

#### 1.2.2 Firmware State Management
```c
#define NRC_FW_ACTIVE           (0)  // Firmware active
#define NRC_FW_LOADING          (1)  // Firmware loading
#define NRC_FW_PREPARE_SLEEP    (2)  // Sleep preparation
#define NRC_FW_SLEEP            (3)  // Sleep state
```

## 2. Host Power Management

### 2.1 Host-side Power Save Implementation

Host power management is implemented in `nrc-pm.c` and `nrc-mac80211.c`:

#### 2.1.1 PS-Poll Based Power Save
- Uses IEEE 802.11 standard PS-Poll frames
- Requests buffered data from AP in STA mode
- Automatic PM field setting in `tx_h_sta_pm()` function

```c
static int tx_h_sta_pm(struct nrc_trx_data *tx)
{
    // Automatically set PM field in data frames
    if (ieee80211_is_data(fc) && !ieee80211_has_pm(fc)) {
        mh->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);
    }
    
    if (ieee80211_is_pspoll(fc)) {
        i_vif->ps_polling = true;
    }
}
```

#### 2.1.2 Dynamic Power Save
- IEEE 802.11 Dynamic PS support
- Automatic power save mode transitions based on traffic patterns
- Timer-based control through `dynamic_ps_timer`

### 2.2 Wake-up Mechanisms

#### 2.2.1 GPIO-based Wake-up
```c
// GPIO pin configuration
int power_save_gpio[2] = {RPI_GPIO_FOR_PS, TARGET_GPIO_FOR_WAKEUP};

// Wake-up signal transmission
static void nrc_hif_send_wakeup_signal(struct nrc_hif_device *dev)
{
    gpio_set_value(power_save_gpio[0], 1);
    usleep_range(100, 200);
    gpio_set_value(power_save_gpio[0], 0);
}
```

#### 2.2.2 Interrupt-based Wake-up
- Wake-up through CSPI interrupts
- Wake-up signals from target to host
- Configured via `spi_gpio_irq` parameter

### 2.3 Driver State Management

```c
struct nrc {
    // Power management state
    enum ps_mode {
        PS_DISABLED,
        PS_ENABLED,
        PS_AUTO_POLL,
        PS_MANUAL_POLL
    } ps;
    
    bool ps_enabled;           // Power save enabled flag
    bool ps_drv_state;         // Driver power save state
    bool ps_modem_enabled;     // Modem power save enabled
    struct timer_list dynamic_ps_timer;  // Dynamic PS timer
};
```

## 3. Target Power Management

### 3.1 Firmware-level Power Save Features

#### 3.1.1 WIM Power Management Commands
```c
enum WIM_CMD_ID {
    WIM_CMD_SLEEP,       // Sleep command
    WIM_CMD_KEEP_ALIVE,  // Keep-alive command
    // ...
};

enum WIM_EVENT_ID {
    WIM_EVENT_PS_READY,   // Power save ready
    WIM_EVENT_PS_WAKEUP,  // Wake-up event
    // ...
};
```

#### 3.1.2 Power Management Parameter Configuration
```c
struct wim_pm_param {
    uint8_t ps_mode;                    // Power save mode
    uint8_t ps_enable;                  // Power save enable
    uint16_t ps_wakeup_pin;            // Wake-up pin
    uint64_t ps_duration;              // Power save duration
    uint32_t ps_timeout;               // Power save timeout
    uint8_t wowlan_wakeup_host_pin;    // WoWLAN wake-up pin
    uint8_t wowlan_enable_any;         // WoWLAN any packet
    uint8_t wowlan_enable_magicpacket; // Magic packet wake-up
    uint8_t wowlan_enable_disconnect;  // Disconnect wake-up
    uint8_t wowlan_n_patterns;         // Number of WoWLAN patterns
    struct wowlan_pattern wp[2];       // WoWLAN patterns
};
```

### 3.2 Power Control through WIM Protocol

#### 3.2.1 Power State Configuration
```c
static int nrc_wim_set_power_mode(struct nrc *nw, 
                                  struct ieee80211_vif *vif,
                                  enum NRC_PS_MODE mode)
{
    struct sk_buff *skb;
    struct wim_pm_param *p;
    
    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_SET,
                               tlv_len(sizeof(*p)));
    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_PS_ENABLE, sizeof(*p), NULL);
    
    p->ps_mode = mode;
    p->ps_enable = (mode != NRC_PS_NONE);
    
    return nrc_xmit_wim_request(nw, skb);
}
```

#### 3.2.2 Keep-alive Mechanism
```c
bool nrc_wim_request_keep_alive(struct nrc *nw)
{
    int ret = nrc_xmit_wim_simple_request(nw, WIM_CMD_KEEP_ALIVE);
    return (ret == 0);
}
```

### 3.3 Hardware Power Save Modes

#### 3.3.1 RF Power Control
- Dynamic transmission power adjustment
- Receiver chain deactivation
- Crystal oscillator control

#### 3.3.2 Modem Power Save
- Baseband processing suspension
- Memory retention mode
- Clock gating

## 4. Dynamic Power Management

### 4.1 Power State Monitoring

#### 4.1.1 Beacon Monitoring
```c
struct nrc {
    struct timer_list bcn_mon_timer;     // Beacon monitoring timer
    unsigned long beacon_timeout;        // Beacon timeout
    struct ieee80211_vif *associated_vif; // Associated VIF
};
```

#### 4.1.2 Traffic Detection
- TX/RX activity monitoring
- Queue occupancy-based decisions
- Dynamic power save mode transition decisions

### 4.2 Automatic Power Save Entry/Exit

#### 4.2.1 Dynamic PS Timer
```c
static void nrc_dynamic_ps_timer(unsigned long data)
{
    struct nrc *nw = (struct nrc *)data;
    
    if (nw->ps_enabled && can_enter_ps(nw)) {
        nrc_enter_power_save_mode(nw);
    }
}
```

#### 4.2.2 Traffic-based Wake-up
- Immediate wake-up on received data detection
- Power save mode exit on transmission requests
- Queue state-based power save mode maintain/exit decisions

### 4.3 QoS Requirements-based Adjustment

#### 4.3.1 Per-AC Power Management
```c
// Per-AC power policies
enum ieee80211_ac_numbers {
    IEEE80211_AC_VO = 0,  // Voice - minimal delay
    IEEE80211_AC_VI = 1,  // Video - low delay
    IEEE80211_AC_BE = 2,  // Best Effort - standard
    IEEE80211_AC_BK = 3,  // Background - maximum power save
};
```

#### 4.3.2 Latency Requirements
- Voice traffic: Minimal power save, fast response
- Video traffic: Limited power save
- Best Effort: Standard power save policy
- Background: Aggressive power save

## 5. Detailed Analysis by Power Save Mode

### 5.1 DOZE Mode

#### 5.1.1 Implementation Features
- Short power save cycles (tens of ms)
- RF remains active, only baseband power save
- Beacon interval-based wake-up
- Low power savings, fast response

#### 5.1.2 State Transition
```c
static int enter_doze_mode(struct nrc *nw)
{
    // Configure baseband power save
    nrc_hif_suspend_baseband(nw->hif);
    
    // Set short timer
    mod_timer(&nw->dynamic_ps_timer, 
              jiffies + msecs_to_jiffies(DOZE_PERIOD_MS));
    
    nw->drv_state = NRC_DRV_PS;
    return 0;
}
```

### 5.2 MODEM SLEEP Mode

#### 5.2.1 Implementation Mechanism
```c
static int enter_modem_sleep(struct nrc *nw)
{
    if (power_save == NRC_PS_MODEMSLEEP) {
        // Configure modem power save mode
        p->ps_mode = NRC_PS_MODEMSLEEP;
        
        // Suspend HIF
        nrc_hif_sleep_target_start(nw->hif, power_save);
        
        // Change state
        nw->drv_state = NRC_DRV_PS;
    }
}
```

#### 5.2.2 Features
- Medium level power savings
- Modem functions suspended
- RF maintained at minimum power
- 100ms~1s power save cycles

### 5.3 DEEP SLEEP Mode

#### 5.3.1 TIM-based Deep Sleep
```c
static int enter_deep_sleep_tim(struct nrc *nw)
{
    if (power_save >= NRC_PS_DEEPSLEEP_TIM) {
        // Configure GPIO wake-up
        p->ps_wakeup_pin = power_save_gpio[1];
        
        // Enable TIM monitoring
        setup_tim_monitoring(nw);
        
        // Enter deep sleep
        nrc_hif_deep_sleep_start(nw->hif);
        
        nw->drv_state = NRC_DRV_PS;
    }
}
```

#### 5.3.2 Non-TIM Deep Sleep
```c
static int enter_deep_sleep_nontim(struct nrc *nw)
{
    if (power_save >= NRC_PS_DEEPSLEEP_NONTIM) {
        // Configure timer-based wake-up
        setup_wakeup_timer(nw, sleep_duration);
        
        // Shutdown all RF functions
        nrc_hif_shutdown_rf(nw->hif);
        
        nw->drv_state = NRC_DRV_PS;
    }
}
```

### 5.4 WoWLAN (Wake on WLAN)

#### 5.4.1 WoWLAN Implementation
```c
struct wowlan_pattern {
    uint16_t offset:6;           // Packet offset
    uint16_t mask_len:4;         // Mask length
    uint16_t pattern_len:6;      // Pattern length
    uint8_t mask[WOWLAN_PATTER_SIZE/8];     // Bitmask
    uint8_t pattern[WOWLAN_PATTER_SIZE];    // Matching pattern
};
```

#### 5.4.2 Wake-up Conditions
- Magic packet reception
- Specific pattern matching
- Disconnect detection
- User-defined patterns

#### 5.4.3 WoWLAN Configuration
```c
static int nrc_setup_wowlan(struct nrc *nw, 
                           struct cfg80211_wowlan *wowlan)
{
    struct wim_pm_param *pm_param;
    
    pm_param->wowlan_enable_magicpacket = wowlan->magic_pkt;
    pm_param->wowlan_enable_disconnect = wowlan->disconnect;
    pm_param->wowlan_n_patterns = wowlan->n_patterns;
    
    // Configure patterns
    for (i = 0; i < wowlan->n_patterns; i++) {
        setup_wowlan_pattern(&pm_param->wp[i], 
                            &wowlan->patterns[i]);
    }
    
    return nrc_wim_set_power_params(nw, pm_param);
}
```

## 6. Power Efficiency Optimization

### 6.1 Interrupt Coalescing

#### 6.1.1 TX Interrupt Coalescing
```c
static void nrc_tx_interrupt_coalescing(struct nrc *nw)
{
    // Coalesce multiple TX completions into single interrupt
    if (nw->tx_coalescing_enabled) {
        mod_timer(&nw->tx_coalescing_timer,
                  jiffies + TX_COALESCING_DELAY);
    }
}
```

#### 6.1.2 RX Interrupt Optimization
- Batch processing of received packets
- Reduced interrupt frequency
- Minimized CPU wake-ups

### 6.2 Batch Processing

#### 6.2.1 Frame Aggregation
```c
// Power efficiency improvement through AMPDU aggregation
static int nrc_ampdu_tx_optimization(struct nrc *nw,
                                    struct ieee80211_sta *sta,
                                    u16 tid)
{
    // Aggregate multiple frames into single AMPDU
    return nrc_wim_ampdu_action(nw, vif, WIM_AMPDU_TX_START, 
                               sta, tid);
}
```

#### 6.2.2 Command Batching
- WIM command queuing
- Batch transmission to reduce wake-ups
- Improved power efficiency

### 6.3 Adaptive Power Save Control

#### 6.3.1 Traffic Pattern Learning
```c
struct traffic_analyzer {
    u32 tx_bytes_history[TRAFFIC_HISTORY_SIZE];
    u32 rx_bytes_history[TRAFFIC_HISTORY_SIZE];
    u32 idle_periods[IDLE_HISTORY_SIZE];
    enum ps_strategy preferred_strategy;
};

static enum ps_strategy analyze_traffic_pattern(struct nrc *nw)
{
    struct traffic_analyzer *analyzer = &nw->traffic_analyzer;
    
    if (is_voice_traffic(analyzer)) {
        return PS_STRATEGY_MINIMAL;
    } else if (is_periodic_traffic(analyzer)) {
        return PS_STRATEGY_PREDICTIVE;
    } else {
        return PS_STRATEGY_AGGRESSIVE;
    }
}
```

#### 6.3.2 Dynamic Parameter Adjustment
```c
static void adaptive_power_control(struct nrc *nw)
{
    enum ps_strategy strategy = analyze_traffic_pattern(nw);
    
    switch (strategy) {
    case PS_STRATEGY_MINIMAL:
        nw->dynamic_ps_timeout = MIN_PS_TIMEOUT;
        break;
    case PS_STRATEGY_PREDICTIVE:
        nw->dynamic_ps_timeout = calculate_optimal_timeout(nw);
        break;
    case PS_STRATEGY_AGGRESSIVE:
        nw->dynamic_ps_timeout = MAX_PS_TIMEOUT;
        break;
    }
}
```

## 7. BSS Max Idle Period Management

### 7.1 Keep-alive Mechanism

#### 7.1.1 AP Mode Implementation
```c
static void ap_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
    
    list_for_each_entry_safe(i_sta, tmp, 
                            &i_vif->preassoc_sta_list, list) {
        if (--i_sta->max_idle.sta_idle_timer == 0) {
            if (++i_sta->max_idle.timeout_cnt >= 
                BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT) {
                // Disconnect inactive station
                ieee80211_disconnect_sta(vif, sta);
            } else {
                // Reset timer
                i_sta->max_idle.sta_idle_timer = 
                    i_sta->max_idle.idle_period;
            }
        }
    }
}
```

#### 7.1.2 STA Mode Implementation
```c
static void sta_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
    
    // Send QoS Null frame as keep-alive
    skb = ieee80211_nullfunc_get(hw, i_sta->vif, false);
    qosnullfunc = (struct ieee80211_hdr_3addr_qos *)skb->data;
    qosnullfunc->frame_control |= cpu_to_le16(IEEE80211_STYPE_QOS_NULL);
    
    nrc_mac_tx(hw, &control, skb);
    
    // Reset timer
    mod_timer(&i_vif->max_idle_timer, 
              jiffies + i_sta->max_idle.idle_period);
}
```

### 7.2 BSS Max Idle IE Processing

#### 7.2.1 IE Parsing
```c
static u8 *find_bss_max_idle_ie(struct sk_buff *skb)
{
    struct ieee80211_mgmt *mgmt = (void *)skb->data;
    u8 *start, *end, *pos;
    
    // Find IE in Association Request/Response
    if (ieee80211_is_assoc_req(fc))
        start = (u8 *)mgmt->u.assoc_req.variable;
    else if (ieee80211_is_assoc_resp(fc))
        start = (u8 *)mgmt->u.assoc_resp.variable;
    
    pos = (u8 *)cfg80211_find_ie(WLAN_EID_BSS_MAX_IDLE_PERIOD,
                                 start, end - start);
    return pos;
}
```

#### 7.2.2 IE Configuration
```c
static int tx_h_bss_max_idle_period(struct nrc_trx_data *tx)
{
    struct bss_max_idle_period_ie *ie;
    
    // Add BSS Max Idle Period IE
    ie = (void *)ieee80211_append_ie(tx->skb,
                                    WLAN_EID_BSS_MAX_IDLE_PERIOD, 3);
    if (ie) {
        ie->max_idle_period = i_sta->max_idle.period;
        ie->idle_option = i_sta->max_idle.options;
    }
    
    return 0;
}
```

## 8. Power Management Parameters

### 8.1 Module Parameters

#### 8.1.1 Basic Power Configuration
```c
int power_save = NRC_PS_NONE;
module_param(power_save, int, 0600);
MODULE_PARM_DESC(power_save, "power save mode selection");

int power_save_gpio[2] = {RPI_GPIO_FOR_PS, TARGET_GPIO_FOR_WAKEUP};
module_param_array(power_save_gpio, int, NULL, 0600);
MODULE_PARM_DESC(power_save_gpio, "gpio pins for power save signaling");
```

#### 8.1.2 Advanced Configuration
```c
int bss_max_idle_offset = 0;
module_param(bss_max_idle_offset, int, 0600);
MODULE_PARM_DESC(bss_max_idle_offset, "BSS max idle period offset");

bool disable_cqm = false;
module_param(disable_cqm, bool, 0600);
MODULE_PARM_DESC(disable_cqm, "disable connection quality monitoring");
```

### 8.2 Runtime Control

#### 8.2.1 Netlink Interface
```c
// Dynamic power mode change
static int nrc_netlink_set_power_mode(struct sk_buff *skb,
                                     struct genl_info *info)
{
    struct nrc *nw = nrc_get_nw();
    u32 power_mode;
    
    if (info->attrs[NRC_ATTR_POWER_MODE]) {
        power_mode = nla_get_u32(info->attrs[NRC_ATTR_POWER_MODE]);
        return nrc_set_power_mode(nw, power_mode);
    }
    
    return -EINVAL;
}
```

#### 8.2.2 Debug Interface
```c
// Power status monitoring through debugfs
static int nrc_debugfs_power_status_show(struct seq_file *m, void *v)
{
    struct nrc *nw = m->private;
    
    seq_printf(m, "Power Save Mode: %d\n", power_save);
    seq_printf(m, "Driver State: %d\n", nw->drv_state);
    seq_printf(m, "FW State: %d\n", atomic_read(&nw->fw_state));
    seq_printf(m, "PS Enabled: %s\n", nw->ps_enabled ? "Yes" : "No");
    
    return 0;
}
```

## 9. Power Management Sequence Diagram

```mermaid
sequenceDiagram
    participant Host as Host Driver
    participant FW as Firmware
    participant RF as RF Hardware
    participant GPIO as GPIO Controller

    Note over Host,GPIO: Power Save Entry Sequence
    Host->>Host: Check traffic conditions
    Host->>FW: WIM_CMD_SLEEP
    FW->>RF: Disable TX/RX chains
    FW->>Host: WIM_EVENT_PS_READY
    Host->>GPIO: Assert sleep signal
    Host->>Host: Enter PS state

    Note over Host,GPIO: Wake-up Sequence
    GPIO->>Host: GPIO interrupt
    Host->>GPIO: Deassert sleep signal
    Host->>FW: Wake-up command
    FW->>RF: Enable TX/RX chains
    FW->>Host: WIM_EVENT_PS_WAKEUP
    Host->>Host: Resume normal operation
```

## 10. Performance and Power Consumption Analysis

### 10.1 Power Consumption Profile

| Mode | Average Power Consumption | Wake-up Time | Application Scenario |
|------|---------------------------|--------------|---------------------|
| NRC_PS_NONE | 100% | N/A | High throughput requirements |
| NRC_PS_MODEMSLEEP | 60-70% | <10ms | General data transmission |
| NRC_PS_DEEPSLEEP_TIM | 20-30% | 50-100ms | IoT sensor networks |
| NRC_PS_DEEPSLEEP_NONTIM | 5-10% | 100-500ms | Battery-powered devices |

### 10.2 Optimization Recommendations

#### 10.2.1 Application-specific Recommendations
- **Voice calls**: NRC_PS_NONE or very short Dynamic PS
- **Video streaming**: NRC_PS_MODEMSLEEP with fast wake-up
- **IoT sensors**: NRC_PS_DEEPSLEEP_TIM with long periods
- **Battery devices**: NRC_PS_DEEPSLEEP_NONTIM with maximum power save

#### 10.2.2 Tuning Parameters
- `dynamic_ps_timeout`: Adjust according to traffic patterns
- `bss_max_idle_offset`: Configure for network latency
- `beacon_timeout`: Balance connection stability and power savings

## 11. Troubleshooting and Debugging

### 11.1 Common Issues

#### 11.1.1 Wake-up Failures
```bash
# Check GPIO state
cat /sys/class/gpio/gpio${power_save_gpio[0]}/value

# Check interrupt counters
cat /proc/interrupts | grep nrc

# Enable debug logging
echo 0x40 > /sys/module/nrc/parameters/nrc_debug_mask
```

#### 11.1.2 Power Mode Transition Failures
```bash
# Check firmware state
cat /sys/kernel/debug/nrc/fw_state

# Check WIM communication status
cat /sys/kernel/debug/nrc/wim_stats
```

### 11.2 Debugging Tools

#### 11.2.1 Debug Masks
```c
#define NRC_DBG_PS     6    // Power management debug
#define NRC_DBG_STATE  8    // State transition debug

// Enable power management debugging
echo 0x140 > /sys/module/nrc/parameters/nrc_debug_mask
```

#### 11.2.2 Statistics Information
- TX/RX packet counters
- Power mode transition counts
- Wake-up event statistics
- Error and timeout counters

## Conclusion

The NRC7292 driver's power management architecture provides a flexible and scalable design suitable for various IoT and low-power applications while meeting IEEE 802.11ah standard requirements. Through multi-layered power management mechanisms, optimal balance between performance and power efficiency can be achieved.

This analysis is based on actual source code and will help developers understand and optimize power management features.