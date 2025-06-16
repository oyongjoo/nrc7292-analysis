# NRC7292 규제 준수 메커니즘 분석

## 규제 준수 개요

NRC7292 HaLow 드라이버는 전 세계 다양한 국가의 무선 규제를 준수하기 위한 포괄적인 다층 접근 방식을 구현합니다. 국가별 주파수 할당, 전력 제한, 특수 요구사항을 체계적으로 관리합니다.

## 1. 규제 프레임워크

### A. 지원 국가/지역 코드 아키텍처
```c
// nrc-bd.c에 정의된 국가 코드 매핑
enum CC_TYPE {
    CC_US = 1,    // 미국 - 가장 포괄적인 주파수 할당 (902.5-927.5MHz)
    CC_JP,        // 일본 - 제한적 할당 (917-927MHz)
    CC_K1,        // 한국 USN1 - 비표준 대역 (921-923MHz) + LBT 요구
    CC_K2,        // 한국 USN5 - MIC 대역 (925-931MHz) + MIC 검출 요구
    CC_TW,        // 대만 - ISM 대역 (839-851MHz)
    CC_EU,        // 유럽연합 - SRD 대역 (863.5-867.5MHz)
    CC_CN,        // 중국 - 다중 서브밴드 (755.5-756.5, 779.5-786.5MHz)
    CC_NZ,        // 뉴질랜드 - 미국과 유사하나 제한된 채널
    CC_AU,        // 호주 - 뉴질랜드와 유사하나 지역별 제한
    CC_MAX
};
```

### B. 국가 코드 검증 및 매핑
```c
// nrc-bd.c:424-453 - 국가 코드 검증 로직
if (country_code[0] == 'U' && country_code[1] == 'S')
    cc_index = CC_US;
else if (country_code[0] == 'J' && country_code[1] == 'P')
    cc_index = CC_JP;
else if (country_code[0] == 'K' && country_code[1] == 'R') {
    // 한국의 특별 처리 - USN1과 USN5 대역 분리
    if (kr_band == 1) {
        cc_index = CC_K1;  // USN1: LBT 요구사항
        country_code[1] = '1';
    } else {
        cc_index = CC_K2;  // USN5: MIC 검출 요구사항
        country_code[1] = '2';
    }
}
else if (country_match(eu_countries_cc, country_code)) {
    // 27개 EU 회원국을 통합 EU 규제 도메인으로 매핑
    cc_index = CC_EU;
    country_code[0] = 'E';
    country_code[1] = 'U';
}
```

### C. EU 규제 통합
```c
// nrc-init.c:632-636 - EU 27개국 통합 관리
const char *const eu_countries_cc[] = {
    "AT", "BE", "BG", "CY", "CZ", "DE", "DK", "EE", "ES", "FI",
    "FR", "GR", "HR", "HU", "IE", "IT", "LT", "LU", "LV", "MT", 
    "NL", "PL", "PT", "RO", "SE", "SI", "SK", NULL
};

// 모든 EU 국가는 통합된 규제 도메인 사용
// ETSI EN 300 220 표준 준수 (SRD 대역 863-870MHz)
```

### D. 동적 국가 전환
```c
// nrc-s1g.c - 런타임 국가 전환 지원
void nrc_set_s1g_country(char* country_code)
{
    uint8_t cc_index = nrc_get_current_ccid_by_country(country_code);
    
    // 유효하지 않은 국가 코드의 경우 미국을 기본값으로 설정
    if (cc_index >= MAX_COUNTRY_CODE) {
        cc_index = US;
        s1g_alpha2[0] = 'U';
        s1g_alpha2[1] = 'S';
        nrc_dbg(NRC_DBG_MAC, "Country index %d out of range. Setting default (US)!", cc_index);
    }
    
    // S1G 채널 테이블 포인터 업데이트
    ptr_nrc_s1g_ch_table = &s1g_ch_table_set[cc_index][0];
}
```

## 2. 보드 데이터 (BD) 시스템

### A. 보드 데이터 파일 구조
```c
// nrc-bd.h에 정의된 BDF 구조
struct BDF {
    uint8_t  ver_major;        // 주요 버전 번호
    uint8_t  ver_minor;        // 부 버전 번호
    uint16_t total_len;        // 전체 데이터 길이
    uint16_t num_data_groups;  // 국가별 데이터 그룹 수
    uint16_t reserved[4];      // 예약 필드
    uint16_t checksum_data;    // 데이터 무결성 체크섬
    uint8_t data[0];           // 가변 길이 국가별 데이터
};

struct wim_bd_param {
    uint16_t type;             // 국가 코드 인덱스
    uint16_t length;           // 데이터 길이
    uint16_t checksum;         // 섹션 체크섬
    uint16_t hw_version;       // 하드웨어 버전 호환성
    uint8_t value[WIM_MAX_BD_DATA_LEN]; // 전력 제한 및 교정 데이터
};
```

### B. 하드웨어 버전 매칭
```c
// nrc-bd.c:483-523 - 하드웨어 호환성 검사
target_version = nw->fwinfo.hw_version;

// 하드웨어 버전이 유효하지 않으면 0으로 설정
if (target_version > 0x7FF)
    target_version = 0;

for (i = 0; i < bd->num_data_groups; i++) {
    if (type == cc_index) {
        bd_sel->hw_version = (uint16_t)(bd->data[6 + len + 4*i] + 
                (bd->data[7 + len + 4*i]<<8));
        
        if (target_version == bd_sel->hw_version) {
            // 일치하는 하드웨어 버전 데이터 로드
            nrc_dbg(NRC_DBG_STATE, "target version matched(%u : %u)",
                    target_version, bd_sel->hw_version);
            // 전력 제한 및 채널 데이터 로드
            check_bd_flag = true;
            break;
        }
    }
}
```

### C. 데이터 무결성 검증
```c
// 16비트 체크섬 검증 알고리즘
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

// 보드 데이터 파일 검증
int nrc_check_bd(struct nrc *nw)
{
    struct BDF *bd = (struct BDF *)g_bd;
    uint16_t ret;
    
    // 파일 크기 검증
    if(g_bd_size < NRC_BD_HEADER_LENGTH) {
        dev_err(nw->dev, "Invalid data size(%d)", g_bd_size);
        return -EINVAL;
    }
    
    // 체크섬 검증
    ret = nrc_checksum_16(bd->total_len, (uint8_t *)&bd->data[0]);
    if(bd->checksum_data != ret) {
        dev_err(nw->dev, "Invalid checksum(%u : %u)", bd->checksum_data, ret);
        return -EINVAL;
    }
    
    return 0;
}
```

## 3. 채널 및 전력 관리

### A. 국가별 채널 계획

#### 미국 (CC_US) - 가장 포괄적
```c
// 902.5-927.5MHz 대역에서 최대 채널 지원
static const struct s1g_channel_table s1g_ch_table_us[] = {
    //cc, freq,channel, bw, cca, oper,offset,pri_loc
    {"US", 9025, 1,  BW_1M, 1, 1,  5, 0},   // 902.5MHz → 2412MHz
    {"US", 9035, 3,  BW_1M, 1, 1, -5, 1},   // 903.5MHz → 2422MHz
    {"US", 9045, 5,  BW_1M, 2, 1,  5, 0},   // 904.5MHz → 2432MHz
    // ... 최대 50개 이상 채널 지원
    {"US", 9825, 165, BW_1M, 1, 1, -5, 1},  // 982.5MHz → 5825MHz
};
```

#### 한국 K1 (USN1 - LBT 요구)
```c
// 921-923MHz 비표준 대역
static const struct s1g_channel_table s1g_ch_table_k1[] = {
    {"K1", 9215, 36, BW_1M, 1, 1,  5, 0},   // 921.5MHz
    {"K1", 9225, 37, BW_1M, 1, 1, -5, 1},   // 922.5MHz
    // LBT (Listen Before Talk) 프로토콜 필수
};
```

#### 한국 K2 (USN5 - MIC 검출 요구)
```c
// 925-931MHz MIC 대역
static const struct s1g_channel_table s1g_ch_table_k2[] = {
    {"K2", 9255, 36, BW_1M, 1, 1,  5, 0},   // 925.5MHz
    {"K2", 9265, 37, BW_1M, 1, 1, -5, 1},   // 926.5MHz
    {"K2", 9275, 38, BW_1M, 2, 1,  5, 0},   // 927.5MHz
    // ... 925.5-930.5MHz 범위
    {"K2", 9270, 42, BW_2M, 1, 1,  0, 0},   // 927.0MHz (2MHz)
    {"K2", 9290, 43, BW_2M, 1, 1,  0, 0},   // 929.0MHz (2MHz)
    // MIC (Mutual Interference Cancellation) 검출 필수
};
```

#### 유럽연합 (CC_EU) - SRD 대역
```c
// 863.5-867.5MHz SRD 대역 (Short Range Device)
static const struct s1g_channel_table s1g_ch_table_eu[] = {
    {"EU", 8635, 1, BW_1M, 1, 6,  5, 0},    // 863.5MHz
    {"EU", 8645, 3, BW_1M, 1, 6, -5, 1},    // 864.5MHz
    {"EU", 8655, 5, BW_1M, 1, 6,  5, 0},    // 865.5MHz
    {"EU", 8665, 7, BW_1M, 1, 6, -5, 1},    // 866.5MHz
    {"EU", 8675, 9, BW_1M, 1, 6, -5, 1},    // 867.5MHz
    // ETSI EN 300 220 표준 준수
};
```

#### 일본 (CC_JP) - 제한적 할당
```c
// 917-927MHz 제한된 주파수 범위
static const struct s1g_channel_table s1g_ch_table_jp[] = {
    {"JP", 9170, 1,  BW_1M, 1, 8,  5, 0},   // 917.0MHz
    {"JP", 9180, 3,  BW_1M, 1, 8, -5, 1},   // 918.0MHz
    {"JP", 9190, 5,  BW_1M, 2, 8,  5, 0},   // 919.0MHz
    // ... 제한된 채널 할당
    // MIC (Ministry of Internal Affairs and Communications) 규제 준수
};
```

### B. S1G 주파수 매핑
```c
// S1G 주파수를 기존 WiFi 채널에 매핑
// 기존 WiFi 도구와의 호환성 보장
struct s1g_channel_table {
    char cc[3];           // 국가 코드
    int s1g_freq;         // 실제 S1G 주파수 (단위: 0.1MHz)
    int non_s1g_ch;       // 매핑된 WiFi 채널 번호
    int bw;               // 대역폭 (1, 2, 4, 8, 16MHz)
    int cca;              // Clear Channel Assessment 타입
    int oper;             // 동작 클래스
    int offset;           // 주파수 오프셋
    int pri_loc;          // Primary 채널 위치
};

예시:
9025 (902.5MHz) → 2412MHz (WiFi 채널 1)
9035 (903.5MHz) → 2422MHz (WiFi 채널 3)
```

### C. 전력 제한 시행
```c
// WIM 프로토콜을 통한 전력 제한 적용
enum WIM_TXPWR_TYPE {
    TXPWR_AUTO = 0,   // 자동 전력 제어
    TXPWR_LIMIT,      // 규제 제한 적용
    TXPWR_FIXED,      // 고정 전력 모드
};

// 국가별 최대 전력 제한 적용
int nrc_wim_set_txpower(struct nrc *nw, int power_dbm)
{
    struct sk_buff *skb;
    int max_power = get_country_max_power(nw->country_code);
    
    // 규제 제한 확인
    if (power_dbm > max_power) {
        power_dbm = max_power;
        nrc_dbg(NRC_DBG_WIM, "Power limited to %d dBm", max_power);
    }
    
    skb = nrc_wim_alloc_skb(WIM_CMD_SET_TXPOWER, 1);
    nrc_wim_add_tlv_u8(skb, WIM_TLV_TXPOWER, power_dbm);
    
    return nrc_wim_send(nw, skb);
}
```

## 4. 특수 지역 요구사항

### A. 한국의 LBT (Listen Before Talk) 구현
```c
// USN1 대역 (921-923MHz)에서 LBT 프로토콜 필수
case WIM_EVENT_LBT_ENABLED:
    nrc_dbg(NRC_DBG_HIF, "LBT enabled for USN1 band");
    // 송신 전 캐리어 센싱 활성화
    break;
    
case WIM_EVENT_LBT_DISABLED:
    nrc_dbg(NRC_DBG_HIF, "LBT disabled");
    break;

// LBT 요구사항 (한국 USN1):
// 1. 송신 전 캐리어 센싱 필수
// 2. 채널 점유 상태 확인
// 3. 동적 채널 접근 관리
// 4. 백오프 알고리즘 구현
```

### B. 한국의 MIC 검출 구현
```c
// USN5 대역 (925-931MHz)에서 MIC 검출 필수
static int nrc_mic_scan(struct sk_buff *skb, struct genl_info *info)
{
    struct wim_channel_1m_param channel;
    struct sk_buff *wim_skb;
    
    // 스캔 채널 범위 설정
    channel.channel_start = nla_get_s32(info->attrs[NL_MIC_SCAN_CHANNEL_START]);
    channel.channel_end = nla_get_s32(info->attrs[NL_MIC_SCAN_CHANNEL_END]);
    
    // MIC 스캔 명령 생성
    wim_skb = nrc_wim_alloc_skb(nrc_nw, WIM_CMD_MIC_SCAN,
                               sizeof(struct wim_channel_1m_param));
    if (!wim_skb) {
        nrc_dbg(NRC_DBG_HIF, "%s: failed to allocate skb\n", __func__);
        return -ENOMEM;
    }
    
    // MIC 검출 실행
    return nrc_wim_send_request(nrc_nw, wim_skb);
}

// MIC 검출 요구사항 (한국 USN5):
// 1. 지속적인 스펙트럼 모니터링
// 2. 간섭 검출 및 완화
// 3. 동적 채널 품질 평가
// 4. 실시간 신호 분석
```

### C. 일본의 특수 규제
```c
// 일본 MIC (총무성) 규제 준수
// 917-927MHz 제한된 주파수 할당
static const struct regulatory_rules jp_rules = {
    .max_power_dbm = 10,        // 최대 10dBm
    .max_antenna_gain = 6,      // 최대 안테나 이득 6dBi
    .duty_cycle_limit = 10,     // 10% 듀티 사이클 제한
    .listen_before_talk = false, // LBT 불필요
    .frequency_hopping = false,  // 주파수 호핑 불필요
};
```

### D. EU SRD 규제 준수
```c
// ETSI EN 300 220 표준 준수 (863-870MHz SRD 대역)
static const struct regulatory_rules eu_rules = {
    .max_power_dbm = 14,        // 최대 14dBm (25mW)
    .max_antenna_gain = 0,      // 안테나 이득 포함된 EIRP
    .duty_cycle_limit = 100,    // 제한 없음 (일부 채널)
    .listen_before_talk = false, // 일반적으로 불필요
    .adaptive_frequency_agility = false, // AFA 불필요
};
```

### E. 미국 FCC 요구사항
```c
// FCC Part 15.247 규제 준수 (902-928MHz ISM 대역)
static const struct regulatory_rules us_rules = {
    .max_power_dbm = 30,        // 최대 30dBm (1W)
    .max_antenna_gain = 6,      // 최대 안테나 이득 6dBi
    .spread_spectrum = true,    // 대역 확산 필수
    .frequency_hopping = false, // 주파수 호핑 선택적
    .power_spectral_density = -8, // -8 dBm/3kHz
};
```

## 5. 런타임 준수 관리

### A. 실시간 준수 검증
```c
// 지원 채널 목록 관리
struct bd_supp_param g_supp_ch_list;  // 국가별 지원 채널 목록
bool g_bd_valid = false;              // 보드 데이터 유효성 플래그

bool nrc_set_supp_ch_list(struct wim_bd_param *bd)
{
    uint8_t *pos = &bd->value[0];
    uint8_t length = bd->length;
    int i, j = 0;
    uint8_t cc_idx, s1g_ch_idx;
    
    // 현재 국가의 지원 채널 설정
    for(i=0; i < NRC_BD_MAX_CH_LIST; i++) {
        if((*pos) && (length > 0)) {
            g_supp_ch_list.num_ch++;
            g_supp_ch_list.s1g_ch_index[i] = *pos;
            
            // S1G를 non-S1G 주파수로 매핑
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

### B. 채널 설정 검증
```c
// 채널 설정 시 규제 준수 확인
int nrc_set_channel(struct nrc *nw, int channel)
{
    bool channel_allowed = false;
    int i;
    
    // 보드 데이터 유효성 확인
    if (!g_bd_valid) {
        nrc_dbg(NRC_DBG_MAC, "Board data not valid");
        return -EINVAL;
    }
    
    // 요청된 채널이 허용된 채널인지 확인
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
    
    // WIM 명령으로 채널 설정
    return nrc_wim_set_channel(nw, channel);
}
```

### C. 전력 제한 시행
```c
// 전력 설정 시 국가별 제한 적용
int nrc_set_txpower(struct nrc *nw, int power_dbm)
{
    int max_power = 0;
    
    // 국가별 최대 전력 제한 확인
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
            max_power = 20;  // 기본값 20dBm
            break;
    }
    
    // 전력 제한 적용
    if (power_dbm > max_power) {
        nrc_dbg(NRC_DBG_MAC, "Power limited from %d to %d dBm", 
                power_dbm, max_power);
        power_dbm = max_power;
    }
    
    return nrc_wim_set_txpower(nw, power_dbm);
}
```

### D. 규제 위반 에러 처리
```c
// 규제 위반 시 에러 처리 및 복구
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
            // 기본 국가 코드로 복구
            nrc_set_s1g_country("US");
            dev_warn(nw->dev, "Invalid country, reverted to US");
            break;
            
        case REG_ERROR_INVALID_CHANNEL:
            // 기본 채널로 전환
            nrc_set_channel(nw, 1);
            dev_warn(nw->dev, "Invalid channel, reverted to channel 1");
            break;
            
        case REG_ERROR_POWER_EXCEEDED:
            // 최대 허용 전력으로 제한
            nrc_set_txpower(nw, get_max_power(nw->country_code));
            dev_warn(nw->dev, "Power exceeded, limited to maximum allowed");
            break;
            
        case REG_ERROR_BD_CHECKSUM:
            // 보드 데이터 무효화
            g_bd_valid = false;
            dev_err(nw->dev, "Board data checksum error, disabling");
            break;
    }
}
```

## 6. 설정 파일 및 배포

### A. 국가별 설정 파일 구조
```bash
# evk/sw_pkg/nrc_pkg/script/conf/ 디렉토리 구조
conf/
├── US/          # 미국 설정
│   ├── ap_halow_open.conf
│   ├── sta_halow_open.conf
│   └── mp_halow_open.conf
├── EU/          # 유럽연합 설정
├── JP/          # 일본 설정
├── K1/          # 한국 USN1 설정
├── K2/          # 한국 USN5 설정
├── TW/          # 대만 설정
├── CN/          # 중국 설정
├── NZ/          # 뉴질랜드 설정
└── AU/          # 호주 설정
```

### B. 자동 국가 감지 및 설정
```python
# start.py에서 국가 자동 감지
def auto_detect_country():
    """
    시스템 로케일 또는 GPS 정보를 통한 국가 자동 감지
    """
    try:
        # 시스템 로케일 확인
        locale_country = get_system_locale_country()
        
        # GPS 기반 위치 확인 (선택적)
        if has_gps_capability():
            gps_country = get_gps_country()
            if gps_country:
                return gps_country
                
        return locale_country or "US"  # 기본값: 미국
    except:
        return "US"  # 오류 시 미국으로 기본 설정
```

### C. 규제 준수 검증 스크립트
```python
# regulatory_check.py - 배포 전 규제 준수 검증
def validate_regulatory_compliance(country_code, config_file):
    """
    배포 전 규제 준수 상태 검증
    """
    rules = load_regulatory_rules(country_code)
    config = load_config(config_file)
    
    errors = []
    
    # 주파수 검증
    if not is_frequency_allowed(config['frequency'], rules):
        errors.append(f"Frequency {config['frequency']} not allowed in {country_code}")
    
    # 전력 검증
    if config['txpower'] > rules['max_power']:
        errors.append(f"TX power {config['txpower']} exceeds limit {rules['max_power']}")
    
    # 대역폭 검증
    if not is_bandwidth_allowed(config['bandwidth'], rules):
        errors.append(f"Bandwidth {config['bandwidth']} not allowed")
    
    return errors
```

## 7. 주요 파일 위치

### A. 규제 핵심 파일들
```bash
# 드라이버 코어 파일
package/src/nrc/nrc-bd.c/h         # 보드 데이터 관리
package/src/nrc/nrc-s1g.c/h        # S1G 채널 관리
package/src/nrc/nrc-init.c         # 국가 코드 검증
package/src/nrc/nrc-netlink.c      # MIC 스캔 인터페이스

# 설정 파일들
evk/sw_pkg/nrc_pkg/script/conf/*/  # 국가별 설정
evk/binary/nrc7292_bd.dat          # 바이너리 보드 데이터
```

### B. 런타임 설정 도구
```bash
# 국가 설정 변경
./start.py 0 0 US              # 미국 규제로 STA 모드 시작
./start.py 1 1 EU              # EU 규제로 AP 모드 시작
./start.py 2 0 JP 1 0          # 일본 규제로 스니퍼 모드

# 수동 채널/전력 설정
iw dev wlan0 set freq 902500   # 902.5MHz로 설정
iw dev wlan0 set txpower fixed 2000  # 20dBm으로 설정
```

## 8. 결론

NRC7292 HaLow 드라이버의 규제 준수 시스템은 다음과 같은 특징을 제공합니다:

### A. 포괄적 글로벌 지원
- **9개 주요 국가/지역** 규제 도메인 지원
- **하드웨어 검증된 보드 데이터**로 지역별 정확한 교정
- **동적 국가 전환** 지원으로 국제 기기 배포 가능

### B. 견고한 준수 메커니즘
- **실시간 규제 검증** 및 체크섬 확인
- **특수 지역 요구사항** (한국 LBT/MIC, 일본 제한사항)
- **자동 에러 처리** 및 복구 시스템

### C. 표준 준수
- **FCC** (미국), **IC** (캐나다), **ETSI** (EU)
- **MIC** (일본), **KCC** (한국) 등 주요 규제 기관
- **IEEE 802.11ah** 표준 완전 준수

이러한 포괄적인 규제 준수 프레임워크를 통해 NRC7292 HaLow 드라이버는 전 세계 어디서나 현지 무선 규제를 준수하면서 안정적으로 동작할 수 있습니다.