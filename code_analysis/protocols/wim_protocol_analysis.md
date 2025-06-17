# Detailed WIM Protocol Analysis

## WIM (Wireless Interface Module) Overview

WIM is the core protocol responsible for communication between the host and firmware in the NRC7292 HaLow driver. All wireless operations, configurations, and state management are performed through WIM.

## 1. WIM Protocol Structure

### A. WIM Message Header
```c
// WIM header structure defined in wim.h
struct wim_header {
    union {
        struct {
            uint8_t  command;     // Command type (0-26)
            uint8_t  subcommand;  // Subcommand (when needed)
        } cmd;
        struct {
            uint8_t  result;      // Response result code
            uint8_t  reserved;    // Reserved field
        } resp;
        struct {
            uint8_t  event;       // Event type (0-11)
            uint8_t  reserved;    // Reserved field
        } evt;
    };
    uint8_t  sequence;           // Sequence number (request/response matching)
    uint8_t  tlv_count;          // Number of TLV parameters
};
```

### B. WIM Message Types
```c
// Classification by message direction
1. Request: Host → Firmware
2. Response: Firmware → Host  
3. Event: Firmware → Host (asynchronous)
```

## 2. WIM Command System

### A. Main WIM Commands (27 commands)
```c
// Commands defined in wim-types.h
enum WIM_CMD_TYPE {
    WIM_CMD_INIT = 0,           // Initialize
    WIM_CMD_START,              // Start
    WIM_CMD_STOP,               // Stop
    WIM_CMD_SET_STA_TYPE,       // Set station type
    WIM_CMD_SET_SECURITY_MODE,  // Set security mode
    WIM_CMD_SET_BSS_INFO,       // Set BSS information
    WIM_CMD_CONNECT,            // Connect
    WIM_CMD_DISCONNECT,         // Disconnect
    WIM_CMD_SCAN,               // Scan
    WIM_CMD_SET_CHANNEL,        // Set channel
    WIM_CMD_SET_TXPOWER,        // Set transmit power
    WIM_CMD_INSTALL_KEY,        // Install security key
    WIM_CMD_AMPDU_ACTION,       // AMPDU control
    WIM_CMD_BA_ACTION,          // Block ACK control
    WIM_CMD_SET_LISTEN_INTERVAL, // Set Listen Interval
    WIM_CMD_SET_BEACON_INTERVAL, // Set Beacon Interval
    WIM_CMD_SET_RTS_THRESHOLD,  // RTS threshold
    WIM_CMD_SET_FRAG_THRESHOLD, // Fragmentation threshold
    WIM_CMD_SET_BG_SCAN_INTERVAL, // Background scan
    WIM_CMD_SET_SHORT_GI,       // Short Guard Interval
    WIM_CMD_SET_11N_INFO,       // 802.11n information
    WIM_CMD_SET_S1G_INFO,       // S1G information
    WIM_CMD_GET_VERSION,        // Get version
    WIM_CMD_GET_STATS,          // Get statistics
    WIM_CMD_SLEEP,              // Sleep mode
    WIM_CMD_WAKEUP,             // Wake up
    WIM_CMD_MAX
};
```

### B. Command Functions by Category

#### System Control Commands
```c
- WIM_CMD_INIT: Firmware initialization
- WIM_CMD_START: Start wireless functions
- WIM_CMD_STOP: Stop wireless functions
- WIM_CMD_GET_VERSION: Check firmware version
- WIM_CMD_GET_STATS: Query performance statistics
```

#### Network Configuration Commands
```c
- WIM_CMD_SET_STA_TYPE: Set AP/STA/Monitor/Mesh mode
- WIM_CMD_SET_BSS_INFO: SSID, BSSID and other BSS information
- WIM_CMD_CONNECT: Network connection
- WIM_CMD_DISCONNECT: Network disconnection
- WIM_CMD_SCAN: Channel scan
```

#### RF and PHY Control Commands
```c
- WIM_CMD_SET_CHANNEL: Set operating channel
- WIM_CMD_SET_TXPOWER: Transmit power control
- WIM_CMD_SET_RTS_THRESHOLD: RTS/CTS threshold
- WIM_CMD_SET_FRAG_THRESHOLD: Packet fragmentation threshold
- WIM_CMD_SET_SHORT_GI: Guard Interval setting
```

#### Security-related Commands
```c
- WIM_CMD_SET_SECURITY_MODE: WPA/WPA2/WPA3 mode
- WIM_CMD_INSTALL_KEY: Install encryption key
```

#### Advanced Feature Commands
```c
- WIM_CMD_AMPDU_ACTION: AMPDU aggregation control
- WIM_CMD_BA_ACTION: Block ACK session management
- WIM_CMD_SET_11N_INFO: 802.11n feature settings
- WIM_CMD_SET_S1G_INFO: HaLow S1G feature settings
```

#### Power Management Commands
```c
- WIM_CMD_SLEEP: Enter sleep mode
- WIM_CMD_WAKEUP: Exit sleep mode
- WIM_CMD_SET_LISTEN_INTERVAL: STA power save cycle
```

## 3. WIM Event System

### A. WIM Event Types (12 events)
```c
enum WIM_EVENT_TYPE {
    WIM_EVENT_SCAN_COMPLETED = 0,    // Scan completed
    WIM_EVENT_CONNECTED,             // Connection completed
    WIM_EVENT_DISCONNECTED,          // Disconnected
    WIM_EVENT_AUTH_STATE_CHANGED,    // Authentication state changed
    WIM_EVENT_ASSOC_STATE_CHANGED,   // Association state changed
    WIM_EVENT_KEY_INSTALLED,         // Key installation completed
    WIM_EVENT_BA_SESSION_STARTED,    // BA session started
    WIM_EVENT_BA_SESSION_STOPPED,    // BA session stopped
    WIM_EVENT_BEACON_LOSS,           // Beacon loss
    WIM_EVENT_SIGNAL_MONITOR,        // Signal strength monitoring
    WIM_EVENT_SLEEP_NOTIFY,          // Sleep state notification
    WIM_EVENT_MAX
};
```

### B. Event Processing Mechanism
```c
// Event reception and processing (wim.c)
static void nrc_wim_process_event(struct nrc *nw, struct sk_buff *skb)
{
    struct wim_header *wh = (struct wim_header *)skb->data;
    
    switch (wh->evt.event) {
        case WIM_EVENT_SCAN_COMPLETED:
            nrc_mac_scan_complete(nw, skb);
            break;
        case WIM_EVENT_CONNECTED:
            nrc_mac_connected(nw, skb);
            break;
        case WIM_EVENT_DISCONNECTED:
            nrc_mac_disconnected(nw, skb);
            break;
        // ... Other event processing
    }
}
```

## 4. TLV (Type-Length-Value) System

### A. TLV Structure
```c
struct wim_tlv {
    uint8_t  type;     // TLV type (0-80+)
    uint8_t  length;   // Data length
    uint8_t  value[];  // Actual data
} __packed;
```

### B. Major TLV Types (80+ types)
```c
// Basic network information
#define WIM_TLV_SSID               1
#define WIM_TLV_BSSID              2
#define WIM_TLV_CHANNEL            3
#define WIM_TLV_TXPOWER            4

// Security-related
#define WIM_TLV_SECURITY_MODE      10
#define WIM_TLV_KEY_INDEX          11
#define WIM_TLV_KEY_TYPE           12
#define WIM_TLV_KEY_VALUE          13

// PHY/RF settings
#define WIM_TLV_RTS_THRESHOLD      20
#define WIM_TLV_FRAG_THRESHOLD     21
#define WIM_TLV_SHORT_GI           22
#define WIM_TLV_GUARD_INTERVAL     23

// HaLow S1G specific
#define WIM_TLV_S1G_CHANNEL        30
#define WIM_TLV_S1G_MCS            31
#define WIM_TLV_S1G_BW             32

// AMPDU-related
#define WIM_TLV_AMPDU_ENABLE       40
#define WIM_TLV_AMPDU_SIZE         41
#define WIM_TLV_BA_WIN_SIZE        42

// Power management
#define WIM_TLV_PS_MODE            50
#define WIM_TLV_LISTEN_INTERVAL    51
#define WIM_TLV_SLEEP_DURATION     52

// Scan-related
#define WIM_TLV_SCAN_SSID          60
#define WIM_TLV_SCAN_CHANNEL       61
#define WIM_TLV_SCAN_TYPE          62

// Statistics and monitoring
#define WIM_TLV_STATS_TYPE         70
#define WIM_TLV_RSSI               71
#define WIM_TLV_SNR                72
```

### C. TLV Encoding/Decoding Functions
```c
// TLV addition functions (wim.c)
int nrc_wim_add_tlv_u8(struct sk_buff *skb, u8 type, u8 value);
int nrc_wim_add_tlv_u16(struct sk_buff *skb, u8 type, u16 value);
int nrc_wim_add_tlv_u32(struct sk_buff *skb, u8 type, u32 value);
int nrc_wim_add_tlv_data(struct sk_buff *skb, u8 type, const void *data, u8 len);

// TLV parsing functions
u8 *nrc_wim_get_tlv(struct sk_buff *skb, u8 type, u8 *length);
u8 nrc_wim_get_tlv_u8(struct sk_buff *skb, u8 type);
u16 nrc_wim_get_tlv_u16(struct sk_buff *skb, u8 type);
u32 nrc_wim_get_tlv_u32(struct sk_buff *skb, u8 type);
```

## 5. WIM Communication Patterns

### A. Asynchronous Commands (Fire-and-Forget)
```c
// Simple configuration command - no response wait
int nrc_wim_set_channel(struct nrc *nw, int channel)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(WIM_CMD_SET_CHANNEL, 1);
    
    nrc_wim_add_tlv_u8(skb, WIM_TLV_CHANNEL, channel);
    
    return nrc_wim_send(nw, skb);  // Send and return immediately
}
```

### B. Synchronous Commands (Request-Response)
```c
// Command requiring response - wait for completion
int nrc_wim_connect(struct nrc *nw, struct ieee80211_bss_conf *bss_conf)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(WIM_CMD_CONNECT, 3);
    
    nrc_wim_add_tlv_data(skb, WIM_TLV_SSID, bss_conf->ssid, bss_conf->ssid_len);
    nrc_wim_add_tlv_data(skb, WIM_TLV_BSSID, bss_conf->bssid, ETH_ALEN);
    nrc_wim_add_tlv_u8(skb, WIM_TLV_CHANNEL, bss_conf->channel);
    
    return nrc_wim_send_request_wait(nw, skb, 5000);  // 5 second timeout
}
```

### C. Event Processing (Firmware → Host)
```c
// Processing events generated by firmware
static void nrc_wim_event_handler(struct nrc *nw, struct sk_buff *skb)
{
    struct wim_header *wh = (struct wim_header *)skb->data;
    
    // Callback invocation based on event type
    switch (wh->evt.event) {
        case WIM_EVENT_SCAN_COMPLETED:
            complete(&nw->scan_done);  // Scan completion signal
            break;
        case WIM_EVENT_CONNECTED:
            netif_carrier_on(nw->netdev);  // Network activation
            break;
    }
}
```

## 6. WIM Message Lifecycle

### A. Command Creation and Transmission
```
1. nrc_wim_alloc_skb() - Allocate message buffer
2. nrc_wim_add_tlv_*() - Add parameters
3. nrc_wim_send() - Transmission through HIF
4. Sequence number assignment and tracking
```

### B. Response Reception and Processing
```
1. Receive WIM message from HIF
2. Match request by sequence number
3. Wake up waiting thread using completion object
4. Parse TLV parameters and return
```

### C. Memory Management
```c
// Automatic memory cleanup
static void nrc_wim_cleanup_request(struct nrc_wim_request *req)
{
    if (req->response_skb) {
        dev_kfree_skb(req->response_skb);
    }
    kfree(req);
}
```

## 7. 에러 처리 및 복구

### A. 타임아웃 처리
```c
// 동기 명령 타임아웃 (기본 5초)
#define WIM_REQUEST_TIMEOUT_MS 5000

static int nrc_wim_wait_response(struct nrc *nw, u8 sequence, int timeout_ms)
{
    int ret = wait_for_completion_timeout(&req->completion, 
                                          msecs_to_jiffies(timeout_ms));
    if (ret == 0) {
        nrc_dbg(NRC_DBG_WIM, "WIM request timeout (seq=%d)\n", sequence);
        return -ETIMEDOUT;
    }
    return 0;
}
```

### B. 펌웨어 상태 검증
```c
// 명령 전송 전 펌웨어 상태 확인
static bool nrc_wim_ready(struct nrc *nw)
{
    return (nw->drv_state == NRC_DRV_RUNNING) && 
           (nw->fw_state == NRC_FW_ACTIVE);
}
```

### C. Error Response Handling
```c
// WIM response error codes
enum WIM_RESULT_CODE {
    WIM_RESULT_SUCCESS = 0,
    WIM_RESULT_INVALID_PARAM,
    WIM_RESULT_NOT_SUPPORTED,
    WIM_RESULT_BUSY,
    WIM_RESULT_TIMEOUT,
    WIM_RESULT_FAILED
};
```

## 8. WIM and HIF Integration

### A. Dual Queue System
```c
// Two queues operated at HIF level
1. Frame Queue: Data frame transmission
2. WIM Queue: Control command transmission (higher priority)
```

### B. Flow Control
```c
// Credit-based transmission control
- WIM commands use separate credit pool
- Independent flow control from frames
- Prevents firmware overload
```

## 9. Advantages of WIM Protocol

### A. Extensibility
- Easy addition of new parameters with TLV structure
- Function expansion while maintaining backward compatibility
- Protocol compatibility during firmware updates

### B. Stability  
- Message tracking with sequence numbers
- Timeout and retry mechanisms
- Automatic cleanup to prevent memory leaks

### C. Performance
- Minimized blocking with asynchronous processing
- Priority-based queue management
- Efficient binary encoding

The WIM protocol is the core of the NRC7292 HaLow driver, providing a powerful communication interface that enables systematic and stable control of complex 802.11ah features.