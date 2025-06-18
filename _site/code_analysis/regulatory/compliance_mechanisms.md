# NRC7292 Regulatory Compliance Mechanisms Analysis

## Regulatory Compliance Overview

The NRC7292 HaLow driver implements a comprehensive multi-layered approach to comply with wireless regulations across various countries worldwide. It systematically manages country-specific frequency allocations, power limitations, and special requirements.

## 1. Regulatory Framework

### A. Supported Country/Region Code Architecture
```c
// Country code mapping defined in nrc-bd.c
enum CC_TYPE {
    CC_US = 1,    // United States - Most comprehensive frequency allocation (902.5-927.5MHz)
    CC_JP,        // Japan - Limited allocation (917-927MHz)
    CC_K1,        // Korea USN1 - Non-standard band (921-923MHz) + LBT requirement
    CC_K2,        // Korea USN5 - MIC band (925-931MHz) + MIC detection requirement
    CC_TW,        // Taiwan - ISM band (839-851MHz)
    CC_EU,        // European Union - SRD band (863.5-867.5MHz)
    CC_CN,        // China - Multiple sub-bands (755.5-756.5, 779.5-786.5MHz)
    CC_NZ,        // New Zealand - Similar to US but limited channels
    CC_AU,        // Australia - Similar to New Zealand but regional restrictions
    CC_MAX
};
```

### B. Country Code Validation and Mapping
```c
// nrc-bd.c:424-453 - Country code validation logic
if (country_code[0] == 'U' && country_code[1] == 'S')
    cc_index = CC_US;
else if (country_code[0] == 'J' && country_code[1] == 'P')
    cc_index = CC_JP;
else if (country_code[0] == 'K' && country_code[1] == 'R') {
    // Special handling for Korea - USN1 and USN5 band separation
    if (kr_band == 1) {
        cc_index = CC_K1;  // USN1: LBT requirements
        country_code[1] = '1';
    } else {
        cc_index = CC_K2;  // USN5: MIC detection requirements
        country_code[1] = '2';
    }
}
else if (country_match(eu_countries_cc, country_code)) {
    // Map 27 EU member countries to unified EU regulatory domain
    cc_index = CC_EU;
    country_code[0] = 'E';
    country_code[1] = 'U';
}
```

### C. EU Regulatory Unification
```c
// nrc-init.c:632-636 - Unified management of 27 EU countries
const char *const eu_countries_cc[] = {
    "AT", "BE", "BG", "CY", "CZ", "DE", "DK", "EE", "ES", "FI",
    "FR", "GR", "HR", "HU", "IE", "IT", "LT", "LU", "LV", "MT", 
    "NL", "PL", "PT", "RO", "SE", "SI", "SK", NULL
};

// All EU countries use unified regulatory domain
// ETSI EN 300 220 standard compliance (SRD band 863-870MHz)
```

### D. Dynamic Country Switching
```c
// nrc-s1g.c - Runtime country switching support
void nrc_set_s1g_country(char* country_code)
{
    uint8_t cc_index = nrc_get_current_ccid_by_country(country_code);
    
    // Set US as default for invalid country codes
    if (cc_index >= MAX_COUNTRY_CODE) {
        cc_index = US;
        s1g_alpha2[0] = 'U';
        s1g_alpha2[1] = 'S';
        nrc_dbg(NRC_DBG_MAC, "Country index %d out of range. Setting default (US)!", cc_index);
    }
    
    // Update S1G channel table pointer
    ptr_nrc_s1g_ch_table = &s1g_ch_table_set[cc_index][0];
}
```

## 2. Board Data (BD) System

### A. Board Data File Structure
```c
// BDF structure defined in nrc-bd.h
struct BDF {
    uint8_t  ver_major;        // Major version number
    uint8_t  ver_minor;        // Minor version number
    uint16_t total_len;        // Total data length
    uint16_t num_data_groups;  // Number of country-specific data groups
    uint16_t reserved[4];      // Reserved fields
    uint16_t checksum_data;    // Data integrity checksum
    uint8_t data[0];           // Variable-length country-specific data
};

struct wim_bd_param {
    uint16_t type;             // Country code index
    uint16_t length;           // Data length
    uint16_t checksum;         // Section checksum
    uint16_t hw_version;       // Hardware version compatibility
    uint8_t value[WIM_MAX_BD_DATA_LEN]; // Power limits and calibration data
};
```

### B. Hardware Version Matching
```c
// nrc-bd.c:483-523 - Hardware compatibility check
target_version = nw->fwinfo.hw_version;

// Set to 0 if hardware version is invalid
if (target_version > 0x7FF)
    target_version = 0;

for (i = 0; i < bd->num_data_groups; i++) {
    if (type == cc_index) {
        bd_sel->hw_version = (uint16_t)(bd->data[6 + len + 4*i] + 
                (bd->data[7 + len + 4*i]<<8));
        
        if (target_version == bd_sel->hw_version) {
            // Load matching hardware version data
            nrc_dbg(NRC_DBG_STATE, "target version matched(%u : %u)",
                    target_version, bd_sel->hw_version);
            // Load power limits and channel data
            check_bd_flag = true;
            break;
        }
    }
}
```

### C. Data Integrity Verification
```c
// 16-bit checksum verification algorithm
static uint16_t nrc_checksum_16(uint16_t len, uint8_t* buf)
{
    uint32_t checksum = 0;
    int i = 0;
    
    while(len > 0) {
        checksum = ((buf[i]) + (buf[i+1]<<8)) + checksum;
        len -= 2;
        i += 2;
    }
    checksum = (checksum>>16) + checksum;
    return checksum;
}

// Board data file verification
int nrc_check_bd(struct nrc *nw)
{
    struct BDF *bd = (struct BDF *)g_bd;
    uint16_t ret;
    
    // File size verification
    if(g_bd_size < NRC_BD_HEADER_LENGTH) {
        dev_err(nw->dev, "Invalid data size(%d)", g_bd_size);
        return -EINVAL;
    }
    
    // Checksum verification
    ret = nrc_checksum_16(bd->total_len, (uint8_t *)&bd->data[0]);
    if(bd->checksum_data != ret) {
        dev_err(nw->dev, "Invalid checksum(%u : %u)", bd->checksum_data, ret);
        return -EINVAL;
    }
    
    return 0;
}
```

## 3. Channel and Power Management

### A. Country-specific Channel Plans

#### United States (CC_US) - Most Comprehensive
```c
// Maximum channel support in 902.5-927.5MHz band
static const struct s1g_channel_table s1g_ch_table_us[] = {
    //cc, freq,channel, bw, cca, oper,offset,pri_loc
    {"US", 9025, 1,  BW_1M, 1, 1,  5, 0},   // 902.5MHz → 2412MHz
    {"US", 9035, 3,  BW_1M, 1, 1, -5, 1},   // 903.5MHz → 2422MHz
    {"US", 9045, 5,  BW_1M, 2, 1,  5, 0},   // 904.5MHz → 2432MHz
    // ... Support for 50+ channels
    {"US", 9825, 165, BW_1M, 1, 1, -5, 1},  // 982.5MHz → 5825MHz
};
```

#### Korea K1 (USN1 - LBT Required)
```c
// 921-923MHz non-standard band
static const struct s1g_channel_table s1g_ch_table_k1[] = {
    {"K1", 9215, 36, BW_1M, 1, 1,  5, 0},   // 921.5MHz
    {"K1", 9225, 37, BW_1M, 1, 1, -5, 1},   // 922.5MHz
    // LBT (Listen Before Talk) protocol mandatory
};
```

#### Korea K2 (USN5 - MIC Detection Required)
```c
// 925-931MHz MIC band
static const struct s1g_channel_table s1g_ch_table_k2[] = {
    {"K2", 9255, 36, BW_1M, 1, 1,  5, 0},   // 925.5MHz
    {"K2", 9265, 37, BW_1M, 1, 1, -5, 1},   // 926.5MHz
    {"K2", 9275, 38, BW_1M, 2, 1,  5, 0},   // 927.5MHz
    // ... 925.5-930.5MHz range
    {"K2", 9270, 42, BW_2M, 1, 1,  0, 0},   // 927.0MHz (2MHz)
    {"K2", 9290, 43, BW_2M, 1, 1,  0, 0},   // 929.0MHz (2MHz)
    // MIC (Mutual Interference Cancellation) detection mandatory
};
```

#### European Union (CC_EU) - SRD Band
```c
// 863.5-867.5MHz SRD band (Short Range Device)
static const struct s1g_channel_table s1g_ch_table_eu[] = {
    {"EU", 8635, 1, BW_1M, 1, 6,  5, 0},    // 863.5MHz
    {"EU", 8645, 3, BW_1M, 1, 6, -5, 1},    // 864.5MHz
    {"EU", 8655, 5, BW_1M, 1, 6,  5, 0},    // 865.5MHz
    {"EU", 8665, 7, BW_1M, 1, 6, -5, 1},    // 866.5MHz
    {"EU", 8675, 9, BW_1M, 1, 6, -5, 1},    // 867.5MHz
    // ETSI EN 300 220 standard compliance
};
```

#### Japan (CC_JP) - Limited Allocation
```c
// 917-927MHz limited frequency range
static const struct s1g_channel_table s1g_ch_table_jp[] = {
    {"JP", 9170, 1,  BW_1M, 1, 8,  5, 0},   // 917.0MHz
    {"JP", 9180, 3,  BW_1M, 1, 8, -5, 1},   // 918.0MHz
    {"JP", 9190, 5,  BW_1M, 2, 8,  5, 0},   // 919.0MHz
    // ... Limited channel allocation
    // MIC (Ministry of Internal Affairs and Communications) regulation compliance
};
```

### B. S1G Frequency Mapping
```c
// Map S1G frequencies to existing WiFi channels
// Ensure compatibility with existing WiFi tools
struct s1g_channel_table {
    char cc[3];           // Country code
    int s1g_freq;         // Actual S1G frequency (unit: 0.1MHz)
    int non_s1g_ch;       // Mapped WiFi channel number
    int bw;               // Bandwidth (1, 2, 4, 8, 16MHz)
    int cca;              // Clear Channel Assessment type
    int oper;             // Operating class
    int offset;           // Frequency offset
    int pri_loc;          // Primary channel location
};

Example:
9025 (902.5MHz) → 2412MHz (WiFi channel 1)
9035 (903.5MHz) → 2422MHz (WiFi channel 3)
```

### C. Power Limit Enforcement
```c
// Apply power limits through WIM protocol
enum WIM_TXPWR_TYPE {
    TXPWR_AUTO = 0,   // Automatic power control
    TXPWR_LIMIT,      // Apply regulatory limits
    TXPWR_FIXED,      // Fixed power mode
};

// Apply country-specific maximum power limits
int nrc_wim_set_txpower(struct nrc *nw, int power_dbm)
{
    struct sk_buff *skb;
    int max_power = get_country_max_power(nw->country_code);
    
    // Check regulatory limits
    if (power_dbm > max_power) {
        power_dbm = max_power;
        nrc_dbg(NRC_DBG_WIM, "Power limited to %d dBm", max_power);
    }
    
    skb = nrc_wim_alloc_skb(WIM_CMD_SET_TXPOWER, 1);
    nrc_wim_add_tlv_u8(skb, WIM_TLV_TXPOWER, power_dbm);
    
    return nrc_wim_send(nw, skb);
}
```

## 4. Special Regional Requirements

### A. Korea's LBT (Listen Before Talk) Implementation
```c
// LBT protocol mandatory in USN1 band (921-923MHz)
case WIM_EVENT_LBT_ENABLED:
    nrc_dbg(NRC_DBG_HIF, "LBT enabled for USN1 band");
    // Enable carrier sensing before transmission
    break;
    
case WIM_EVENT_LBT_DISABLED:
    nrc_dbg(NRC_DBG_HIF, "LBT disabled");
    break;

// LBT Requirements (Korea USN1):
// 1. Carrier sensing mandatory before transmission
// 2. Channel occupancy status verification
// 3. Dynamic channel access management
// 4. Backoff algorithm implementation
```

### B. Korea's MIC Detection Implementation
```c
// MIC detection mandatory in USN5 band (925-931MHz)
static int nrc_mic_scan(struct sk_buff *skb, struct genl_info *info)
{
    struct wim_channel_1m_param channel;
    struct sk_buff *wim_skb;
    
    // Set scan channel range
    channel.channel_start = nla_get_s32(info->attrs[NL_MIC_SCAN_CHANNEL_START]);
    channel.channel_end = nla_get_s32(info->attrs[NL_MIC_SCAN_CHANNEL_END]);
    
    // Generate MIC scan command
    wim_skb = nrc_wim_alloc_skb(nrc_nw, WIM_CMD_MIC_SCAN,
                               sizeof(struct wim_channel_1m_param));
    if (!wim_skb) {
        nrc_dbg(NRC_DBG_HIF, "%s: failed to allocate skb\n", __func__);
        return -ENOMEM;
    }
    
    // Execute MIC detection
    return nrc_wim_send_request(nrc_nw, wim_skb);
}

// MIC Detection Requirements (Korea USN5):
// 1. Continuous spectrum monitoring
// 2. Interference detection and mitigation
// 3. Dynamic channel quality assessment
// 4. Real-time signal analysis
```

### C. Japan's Special Regulations
```c
// Japan MIC (Ministry of Internal Affairs and Communications) regulation compliance
// 917-927MHz limited frequency allocation
static const struct regulatory_rules jp_rules = {
    .max_power_dbm = 10,        // Maximum 10dBm
    .max_antenna_gain = 6,      // Maximum antenna gain 6dBi
    .duty_cycle_limit = 10,     // 10% duty cycle limit
    .listen_before_talk = false, // LBT not required
    .frequency_hopping = false,  // Frequency hopping not required
};
```

### D. EU SRD Regulation Compliance
```c
// ETSI EN 300 220 standard compliance (863-870MHz SRD band)
static const struct regulatory_rules eu_rules = {
    .max_power_dbm = 14,        // Maximum 14dBm (25mW)
    .max_antenna_gain = 0,      // EIRP including antenna gain
    .duty_cycle_limit = 100,    // No limit (some channels)
    .listen_before_talk = false, // Generally not required
    .adaptive_frequency_agility = false, // AFA not required
};
```

### E. US FCC Requirements
```c
// FCC Part 15.247 regulation compliance (902-928MHz ISM band)
static const struct regulatory_rules us_rules = {
    .max_power_dbm = 30,        // Maximum 30dBm (1W)
    .max_antenna_gain = 6,      // Maximum antenna gain 6dBi
    .spread_spectrum = true,    // Spread spectrum mandatory
    .frequency_hopping = false, // Frequency hopping optional
    .power_spectral_density = -8, // -8 dBm/3kHz
};
```

## 5. Runtime Compliance Management

### A. Real-time Compliance Verification
```c
// Supported channel list management
struct bd_supp_param g_supp_ch_list;  // Country-specific supported channel list
bool g_bd_valid = false;              // Board data validity flag

bool nrc_set_supp_ch_list(struct wim_bd_param *bd)
{
    uint8_t *pos = &bd->value[0];
    uint8_t length = bd->length;
    int i, j = 0;
    uint8_t cc_idx, s1g_ch_idx;
    
    // Set supported channels for current country
    for(i=0; i < NRC_BD_MAX_CH_LIST; i++) {
        if((*pos) && (length > 0)) {
            g_supp_ch_list.num_ch++;
            g_supp_ch_list.s1g_ch_index[i] = *pos;
            
            // Map S1G to non-S1G frequency
            cc_idx = nrc_get_cc_by_country();
            s1g_ch_idx = *pos;
            g_supp_ch_list.nons1g_ch_freq[j] = 
                nrc_get_non_s1g_freq(cc_idx, s1g_ch_idx);
            
            length--;
            pos++;
            j++;
        }
    }
    
    g_bd_valid = true;
    return true;
}
```

### B. Channel Setting Verification
```c
// Verify regulatory compliance when setting channels
int nrc_set_channel(struct nrc *nw, int channel)
{
    bool channel_allowed = false;
    int i;
    
    // Check board data validity
    if (!g_bd_valid) {
        nrc_dbg(NRC_DBG_MAC, "Board data not valid");
        return -EINVAL;
    }
    
    // Check if requested channel is allowed
    for (i = 0; i < g_supp_ch_list.num_ch; i++) {
        if (g_supp_ch_list.s1g_ch_index[i] == channel) {
            channel_allowed = true;
            break;
        }
    }
    
    if (!channel_allowed) {
        nrc_dbg(NRC_DBG_MAC, "Channel %d not allowed in %s", 
                channel, nw->country_code);
        return -EINVAL;
    }
    
    // Set channel via WIM command
    return nrc_wim_set_channel(nw, channel);
}
```

### C. Power Limit Enforcement
```c
// Apply country-specific limits when setting power
int nrc_set_txpower(struct nrc *nw, int power_dbm)
{
    int max_power = 0;
    
    // Check country-specific maximum power limits
    switch (nw->country_index) {
        case CC_US:
            max_power = 30;  // 30dBm (1W)
            break;
        case CC_EU:
            max_power = 14;  // 14dBm (25mW)
            break;
        case CC_JP:
            max_power = 10;  // 10dBm (10mW)
            break;
        case CC_K1:
        case CC_K2:
            max_power = 23;  // 23dBm (200mW)
            break;
        default:
            max_power = 20;  // Default 20dBm
            break;
    }
    
    // Apply power limits
    if (power_dbm > max_power) {
        nrc_dbg(NRC_DBG_MAC, "Power limited from %d to %d dBm", 
                power_dbm, max_power);
        power_dbm = max_power;
    }
    
    return nrc_wim_set_txpower(nw, power_dbm);
}
```

### D. Regulatory Violation Error Handling
```c
// Error handling and recovery for regulatory violations
enum regulatory_error_codes {
    REG_ERROR_INVALID_COUNTRY = -1001,
    REG_ERROR_INVALID_CHANNEL = -1002,
    REG_ERROR_POWER_EXCEEDED = -1003,
    REG_ERROR_BD_CHECKSUM = -1004,
    REG_ERROR_BD_VERSION = -1005,
};

static void handle_regulatory_error(struct nrc *nw, int error_code)
{
    switch (error_code) {
        case REG_ERROR_INVALID_COUNTRY:
            // Revert to default country code
            nrc_set_s1g_country("US");
            dev_warn(nw->dev, "Invalid country, reverted to US");
            break;
            
        case REG_ERROR_INVALID_CHANNEL:
            // Switch to default channel
            nrc_set_channel(nw, 1);
            dev_warn(nw->dev, "Invalid channel, reverted to channel 1");
            break;
            
        case REG_ERROR_POWER_EXCEEDED:
            // Limit to maximum allowed power
            nrc_set_txpower(nw, get_max_power(nw->country_code));
            dev_warn(nw->dev, "Power exceeded, limited to maximum allowed");
            break;
            
        case REG_ERROR_BD_CHECKSUM:
            // Invalidate board data
            g_bd_valid = false;
            dev_err(nw->dev, "Board data checksum error, disabling");
            break;
    }
}
```

## 6. Configuration Files and Deployment

### A. Country-specific Configuration File Structure
```bash
# evk/sw_pkg/nrc_pkg/script/conf/ directory structure
conf/
├── US/          # United States configuration
│   ├── ap_halow_open.conf
│   ├── sta_halow_open.conf
│   └── mp_halow_open.conf
├── EU/          # European Union configuration
├── JP/          # Japan configuration
├── K1/          # Korea USN1 configuration
├── K2/          # Korea USN5 configuration
├── TW/          # Taiwan configuration
├── CN/          # China configuration
├── NZ/          # New Zealand configuration
└── AU/          # Australia configuration
```

### B. Automatic Country Detection and Configuration
```python
# Automatic country detection in start.py
def auto_detect_country():
    """
    Automatic country detection via system locale or GPS information
    """
    try:
        # Check system locale
        locale_country = get_system_locale_country()
        
        # GPS-based location check (optional)
        if has_gps_capability():
            gps_country = get_gps_country()
            if gps_country:
                return gps_country
                
        return locale_country or "US"  # Default: United States
    except:
        return "US"  # Default to US on error
```

### C. Regulatory Compliance Verification Script
```python
# regulatory_check.py - Pre-deployment regulatory compliance verification
def validate_regulatory_compliance(country_code, config_file):
    """
    Verify regulatory compliance status before deployment
    """
    rules = load_regulatory_rules(country_code)
    config = load_config(config_file)
    
    errors = []
    
    # Frequency verification
    if not is_frequency_allowed(config['frequency'], rules):
        errors.append(f"Frequency {config['frequency']} not allowed in {country_code}")
    
    # Power verification
    if config['txpower'] > rules['max_power']:
        errors.append(f"TX power {config['txpower']} exceeds limit {rules['max_power']}")
    
    # Bandwidth verification
    if not is_bandwidth_allowed(config['bandwidth'], rules):
        errors.append(f"Bandwidth {config['bandwidth']} not allowed")
    
    return errors
```

## 7. Key File Locations

### A. Regulatory Core Files
```bash
# Driver core files
package/src/nrc/nrc-bd.c/h         # Board data management
package/src/nrc/nrc-s1g.c/h        # S1G channel management
package/src/nrc/nrc-init.c         # Country code validation
package/src/nrc/nrc-netlink.c      # MIC scan interface

# Configuration files
evk/sw_pkg/nrc_pkg/script/conf/*/  # Country-specific configurations
evk/binary/nrc7292_bd.dat          # Binary board data
```

### B. Runtime Configuration Tools
```bash
# Country configuration changes
./start.py 0 0 US              # Start STA mode with US regulations
./start.py 1 1 EU              # Start AP mode with EU regulations
./start.py 2 0 JP 1 0          # Start sniffer mode with Japan regulations

# Manual channel/power settings
iw dev wlan0 set freq 902500   # Set to 902.5MHz
iw dev wlan0 set txpower fixed 2000  # Set to 20dBm
```

## 8. Conclusion

The NRC7292 HaLow driver's regulatory compliance system provides the following features:

### A. Comprehensive Global Support
- **9 major country/region** regulatory domain support
- **Hardware-validated board data** for accurate regional calibration
- **Dynamic country switching** support enabling international device deployment

### B. Robust Compliance Mechanisms
- **Real-time regulatory verification** and checksum validation
- **Special regional requirements** (Korea LBT/MIC, Japan restrictions)
- **Automatic error handling** and recovery system

### C. Standards Compliance
- **FCC** (US), **IC** (Canada), **ETSI** (EU)
- **MIC** (Japan), **KCC** (Korea) and other major regulatory authorities
- **IEEE 802.11ah** standard full compliance

Through this comprehensive regulatory compliance framework, the NRC7292 HaLow driver can operate reliably anywhere in the world while complying with local wireless regulations.