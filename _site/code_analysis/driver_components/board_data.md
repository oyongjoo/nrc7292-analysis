# Board Data (nrc-bd.c) Analysis

## File Overview
Board data management is a core module for complying with wireless regulations in each country.

## Main Functions

### 1. Board Data File Processing
```c
// File path: /lib/firmware/{bd_name}
static void * nrc_dump_load(struct nrc *nw, int len)
```
- Kernel version-specific file system access handling
- Support for versions 5.0 ~ 5.18 compatibility
- Memory management and error handling

### 2. Country Code Mapping
```c
enum {
    CC_US=1, CC_JP, CC_K1, CC_TW, CC_EU, 
    CC_CN, CC_NZ, CC_AU, CC_K2, CC_MAX
} CC_TYPE;
```

### 3. Channel Table Management
Country-specific channel mapping definition:
- S1G frequency (actual HaLow frequency)
- Non-S1G frequency (WiFi compatible frequency)
- Channel index mapping

## Core Function Analysis

### `nrc_read_bd_tx_pwr()`
1. Country code identification
2. Hardware version matching
3. Board data parsing
4. Checksum verification
5. Supported channel list construction

### `nrc_check_bd()`
Board data file integrity check:
- File size verification
- Header information validation
- Checksum calculation and comparison

## Data Structures

### BDF (Board Data File) Structure
```c
struct BDF {
    uint8_t ver_major;
    uint8_t ver_minor;
    uint16_t total_len;
    uint16_t num_data_groups;
    uint16_t checksum_data;
    uint8_t data[];
};
```

### WIM BD Parameters
```c
struct wim_bd_param {
    uint16_t type;          // Country code
    uint16_t hw_version;    // Hardware version
    uint16_t length;        // Data length
    uint16_t checksum;      // Checksum
    uint8_t value[];        // Actual data
};
```

## Special Features

### Special Handling for Korea
- K1 (USN1): LBT requirements
- K2 (USN5): MIC detection requirements
- Dynamic selection via `kr_band` parameter

### Kernel Version Compatibility
Support for various kernel versions through complex conditional compilation:
- Handling `get_fs()`, `set_fs()` API changes
- Using `force_uaccess_begin()`
- Handling `kernel_read()` function signature changes