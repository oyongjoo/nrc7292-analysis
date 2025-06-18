# NRC7292 Driver Data Flow Analysis

## Overall Data Flow Architecture

```
User Space                    Kernel Space                      Hardware
┌─────────────┐              ┌──────────────────────────┐        ┌─────────────┐
│ CLI App     │              │                          │        │             │
│ start.py    │◄────────────►│    nrc-netlink.c/.h     │        │   NRC7292   │
│ stop.py     │              │                          │        │   Chipset   │
└─────────────┘              └──────────┬───────────────┘        │             │
                                        │                        │             │
┌─────────────┐              ┌──────────▼───────────────┐        │             │
│ hostapd/    │              │                          │        │             │
│ wpa_        │◄────────────►│    nrc-mac80211.c/.h    │        │             │
│ supplicant  │              │  (ieee80211_ops impl)   │        │             │
└─────────────┘              └──────────┬───────────────┘        │             │
                                        │                        │             │
                             ┌──────────▼───────────────┐        │             │
                             │      nrc-trx.c           │        │             │
                             │  (TX/RX packet handling) │        │             │
                             └──────────┬───────────────┘        │             │
                                        │                        │             │
                             ┌──────────▼───────────────┐        │             │
                             │       wim.c/.h           │        │             │
                             │ (WIM protocol layer)     │        │             │
                             └──────────┬───────────────┘        │             │
                                        │                        │             │
                             ┌──────────▼───────────────┐        │             │
                             │   nrc-hif-cspi.c/.h     │◄──────►│             │
                             │  (CSPI hardware IF)      │        │             │
                             └──────────────────────────┘        └─────────────┘
```

## Transmit Data Path (TX Path)

### 1. Packet Entry Point
```c
// Packet transmission request from mac80211 (actual function name)
static void nrc_mac_tx(struct ieee80211_hw *hw, 
                      struct ieee80211_tx_control *control,
                      struct sk_buff *skb)
{
    // Packet validation and preprocessing
    // Queue selection and priority handling
    // Forward to HIF layer
}
```

### 2. TX Processing Pipeline
```
Network stack packet
       ↓
nrc_mac_tx() [nrc-trx.c]
       ↓
nrc_xmit_frame() [hif.c]
       ↓ 
Packet header addition/conversion
       ↓
AMPDU aggregation (if needed)
       ↓
nrc_xmit_wim() [wim.c/hif.c]
       ↓
WIM protocol wrapping
       ↓
HIF ops->xmit() [nrc-hif-cspi.c]
       ↓
SPI transmission
       ↓
NRC7292 hardware
```

### 3. TX Detailed Processing Steps

#### A. TX Entry Point Processing (nrc-trx.c)
```c
// Steps handled in nrc_mac_tx() function:
1. ieee80211_tx_info validation
2. Target VIF and station verification
3. Encryption key setup
4. QoS and priority handling
5. TX tasklet scheduling or direct transmission
```

#### B. HIF Layer Processing (hif.c)
```c
// Steps handled in nrc_xmit_frame() function:
1. HIF header addition
2. VIF index and AID setup
3. Packet segmentation (if needed)
4. AMPDU aggregation decision
5. Forward to WIM layer
```

#### C. WIM Protocol Processing (wim.c/hif.c)
```c
// Steps handled in nrc_xmit_wim() function:
1. WIM header generation
2. HIF_SUBTYPE setup (DATA, MGMT, etc.)
3. Sequence number assignment
4. TLV format conversion
5. Forward to HIF layer
```

#### D. CSPI Hardware Interface (nrc-hif-cspi.c)
```c
// Steps handled in HIF ops->xmit() implementation:
1. DMA buffer allocation
2. CSPI protocol header addition
3. SPI transmission queuing
4. Interrupt-based transmission
5. Transmission completion confirmation
```

## Receive Data Path (RX Path)

### 1. Interrupt Processing
```c
// CSPI interrupt handler (actual implementation)
static irqreturn_t nrc_hif_cspi_irq_handler(int irq, void *dev_id)
{
    // Check interrupt source
    // Check for RX data availability
    // Schedule tasklet or work queue
    return IRQ_HANDLED;
}
```

### 2. RX Processing Pipeline
```
NRC7292 hardware
       ↓
Interrupt occurs
       ↓
nrc_hif_cspi_irq_handler() [nrc-hif-cspi.c]
       ↓
HIF ops->read() - SPI read
       ↓
nrc_hif_rx() [hif.c]
       ↓
WIM protocol parsing
       ↓
nrc_rx_complete() [nrc-trx.c]
       ↓
Packet validation and conversion
       ↓
ieee80211_rx_ni() [mac80211]
       ↓
Forward to network stack
```

### 3. RX Detailed Processing Steps

#### A. HIF Interrupt Processing (nrc-hif-cspi.c)
```c
1. GPIO interrupt detection
2. Interrupt status register read
3. RX FIFO status check
4. Work queue scheduling
5. Interrupt clear
```

#### B. WIM Protocol Parsing (wim.c)
```c
1. WIM header validation
2. Message type verification
3. Payload length validation
4. Checksum verification
5. Response/event classification
```

#### C. TRX Layer Processing (nrc-trx.c)
```c
1. HIF header removal
2. Packet reassembly (if segmented)
3. AMPDU reordering
4. Duplicate packet filtering
5. Statistics update
```

#### D. MAC Layer Forwarding (nrc-mac80211.c)
```c
1. ieee80211_rx_status construction
2. RSSI/SNR information setup
3. Channel information mapping
4. Encryption status setup
5. Forward to mac80211
```

## Control Command Path

### 1. User Space to Kernel (Netlink)
```
CLI App → netlink socket → nrc-netlink.c → corresponding handler function
```

### 2. WIM Command Processing Path
```
Configuration request
    ↓
nrc_wim_set_param()
    ↓  
WIM_CMD_SET message generation
    ↓
cspi_write() transmission
    ↓
Response waiting and processing
```

## AMPDU Aggregation Data Flow

### TX AMPDU
```
Individual packets → Aggregation buffer → Block ACK setup → Batch transmission → BA response handling
```

### RX AMPDU
```
Aggregated packet reception → Reordering buffer → Sequence rearrangement → Individual packet forwarding → BA response transmission
```

## Power Management Data Flow 

### Sleep Mode Entry
```
Idle state detection → PM decision → WIM_CMD_SLEEP → Hardware sleep → Interrupt waiting
```

### Sleep Mode Exit  
```
Wake-up interrupt → cspi_resume() → WIM_CMD_WAKE → Normal operation recovery
```

## Performance Optimization Points

1. **Zero-copy path**: Minimize memory copying with direct DMA transmission
2. **Interrupt coalescing**: Handle multiple packets with single interrupt
3. **Queue management**: Separate priority-based TX queue management
4. **Batch processing**: Batch transmission of small packets
5. **Cache optimization**: Cache-aligned packet buffer management

This data flow structure ensures efficient packet processing and low-latency communication for the NRC7292 HaLow driver.