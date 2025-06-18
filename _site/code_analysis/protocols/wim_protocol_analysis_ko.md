# WIM 프로토콜 상세 분석

## WIM (Wireless Interface Module) 개요

WIM은 NRC7292 HaLow 드라이버에서 호스트와 펌웨어 간 통신을 담당하는 핵심 프로토콜입니다. 모든 무선 동작, 설정, 상태 관리가 WIM을 통해 이루어집니다.

## 1. WIM 프로토콜 구조

### A. WIM 메시지 헤더
```c
// wim.h에 정의된 WIM 헤더 구조
struct wim_header {
    union {
        struct {
            uint8_t  command;     // 명령 타입 (0-26)
            uint8_t  subcommand;  // 하위 명령 (필요시)
        } cmd;
        struct {
            uint8_t  result;      // 응답 결과 코드
            uint8_t  reserved;    // 예약 필드
        } resp;
        struct {
            uint8_t  event;       // 이벤트 타입 (0-11)
            uint8_t  reserved;    // 예약 필드
        } evt;
    };
    uint8_t  sequence;           // 시퀀스 번호 (요청/응답 매칭)
    uint8_t  tlv_count;          // TLV 파라미터 개수
};
```

### B. WIM 메시지 타입
```c
// 메시지 방향에 따른 분류
1. Request (요청): 호스트 → 펌웨어
2. Response (응답): 펌웨어 → 호스트  
3. Event (이벤트): 펌웨어 → 호스트 (비동기)
```

## 2. WIM 명령어 체계

### A. 주요 WIM 명령어 (27개)
```c
// wim-types.h에 정의된 명령어들
enum WIM_CMD_TYPE {
    WIM_CMD_INIT = 0,           // 초기화
    WIM_CMD_START,              // 시작
    WIM_CMD_STOP,               // 정지
    WIM_CMD_SET_STA_TYPE,       // 스테이션 타입 설정
    WIM_CMD_SET_SECURITY_MODE,  // 보안 모드 설정
    WIM_CMD_SET_BSS_INFO,       // BSS 정보 설정
    WIM_CMD_CONNECT,            // 연결
    WIM_CMD_DISCONNECT,         // 연결 해제
    WIM_CMD_SCAN,               // 스캔
    WIM_CMD_SET_CHANNEL,        // 채널 설정
    WIM_CMD_SET_TXPOWER,        // 송신 전력 설정
    WIM_CMD_INSTALL_KEY,        // 보안 키 설치
    WIM_CMD_AMPDU_ACTION,       // AMPDU 제어
    WIM_CMD_BA_ACTION,          // Block ACK 제어
    WIM_CMD_SET_LISTEN_INTERVAL, // Listen Interval 설정
    WIM_CMD_SET_BEACON_INTERVAL, // Beacon Interval 설정
    WIM_CMD_SET_RTS_THRESHOLD,  // RTS 임계값
    WIM_CMD_SET_FRAG_THRESHOLD, // 단편화 임계값
    WIM_CMD_SET_BG_SCAN_INTERVAL, // 백그라운드 스캔
    WIM_CMD_SET_SHORT_GI,       // Short Guard Interval
    WIM_CMD_SET_11N_INFO,       // 802.11n 정보
    WIM_CMD_SET_S1G_INFO,       // S1G 정보
    WIM_CMD_GET_VERSION,        // 버전 조회
    WIM_CMD_GET_STATS,          // 통계 조회
    WIM_CMD_SLEEP,              // 절전 모드
    WIM_CMD_WAKEUP,             // 웨이크업
    WIM_CMD_MAX
};
```

### B. 명령어 분류별 기능

#### 시스템 제어 명령
```c
- WIM_CMD_INIT: 펌웨어 초기화
- WIM_CMD_START: 무선 기능 시작
- WIM_CMD_STOP: 무선 기능 정지
- WIM_CMD_GET_VERSION: 펌웨어 버전 확인
- WIM_CMD_GET_STATS: 성능 통계 조회
```

#### 네트워크 설정 명령
```c
- WIM_CMD_SET_STA_TYPE: AP/STA/Monitor/Mesh 모드 설정
- WIM_CMD_SET_BSS_INFO: SSID, BSSID 등 BSS 정보
- WIM_CMD_CONNECT: 네트워크 연결
- WIM_CMD_DISCONNECT: 네트워크 연결 해제
- WIM_CMD_SCAN: 채널 스캔
```

#### RF 및 PHY 제어 명령
```c
- WIM_CMD_SET_CHANNEL: 동작 채널 설정
- WIM_CMD_SET_TXPOWER: 송신 전력 제어
- WIM_CMD_SET_RTS_THRESHOLD: RTS/CTS 임계값
- WIM_CMD_SET_FRAG_THRESHOLD: 패킷 단편화 임계값
- WIM_CMD_SET_SHORT_GI: Guard Interval 설정
```

#### 보안 관련 명령
```c
- WIM_CMD_SET_SECURITY_MODE: WPA/WPA2/WPA3 모드
- WIM_CMD_INSTALL_KEY: 암호화 키 설치
```

#### 고급 기능 명령
```c
- WIM_CMD_AMPDU_ACTION: AMPDU 집성 제어
- WIM_CMD_BA_ACTION: Block ACK 세션 관리
- WIM_CMD_SET_11N_INFO: 802.11n 기능 설정
- WIM_CMD_SET_S1G_INFO: HaLow S1G 기능 설정
```

#### 전력 관리 명령
```c
- WIM_CMD_SLEEP: 절전 모드 진입
- WIM_CMD_WAKEUP: 절전 모드 해제
- WIM_CMD_SET_LISTEN_INTERVAL: STA 절전 주기
```

## 3. WIM 이벤트 시스템

### A. WIM 이벤트 타입 (12개)
```c
enum WIM_EVENT_TYPE {
    WIM_EVENT_SCAN_COMPLETED = 0,    // 스캔 완료
    WIM_EVENT_CONNECTED,             // 연결 완료
    WIM_EVENT_DISCONNECTED,          // 연결 해제
    WIM_EVENT_AUTH_STATE_CHANGED,    // 인증 상태 변경
    WIM_EVENT_ASSOC_STATE_CHANGED,   // 연결 상태 변경
    WIM_EVENT_KEY_INSTALLED,         // 키 설치 완료
    WIM_EVENT_BA_SESSION_STARTED,    // BA 세션 시작
    WIM_EVENT_BA_SESSION_STOPPED,    // BA 세션 종료
    WIM_EVENT_BEACON_LOSS,           // 비콘 손실
    WIM_EVENT_SIGNAL_MONITOR,        // 신호 강도 모니터링
    WIM_EVENT_SLEEP_NOTIFY,          // 절전 상태 알림
    WIM_EVENT_MAX
};
```

### B. 이벤트 처리 메커니즘
```c
// 이벤트 수신 및 처리 (wim.c)
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
        // ... 기타 이벤트 처리
    }
}
```

## 4. TLV (Type-Length-Value) 시스템

### A. TLV 구조
```c
struct wim_tlv {
    uint8_t  type;     // TLV 타입 (0-80+)
    uint8_t  length;   // 데이터 길이
    uint8_t  value[];  // 실제 데이터
} __packed;
```

### B. 주요 TLV 타입들 (80개 이상)
```c
// 기본 네트워크 정보
#define WIM_TLV_SSID               1
#define WIM_TLV_BSSID              2
#define WIM_TLV_CHANNEL            3
#define WIM_TLV_TXPOWER            4

// 보안 관련
#define WIM_TLV_SECURITY_MODE      10
#define WIM_TLV_KEY_INDEX          11
#define WIM_TLV_KEY_TYPE           12
#define WIM_TLV_KEY_VALUE          13

// PHY/RF 설정
#define WIM_TLV_RTS_THRESHOLD      20
#define WIM_TLV_FRAG_THRESHOLD     21
#define WIM_TLV_SHORT_GI           22
#define WIM_TLV_GUARD_INTERVAL     23

// HaLow S1G 특화
#define WIM_TLV_S1G_CHANNEL        30
#define WIM_TLV_S1G_MCS            31
#define WIM_TLV_S1G_BW             32

// AMPDU 관련
#define WIM_TLV_AMPDU_ENABLE       40
#define WIM_TLV_AMPDU_SIZE         41
#define WIM_TLV_BA_WIN_SIZE        42

// 전력 관리
#define WIM_TLV_PS_MODE            50
#define WIM_TLV_LISTEN_INTERVAL    51
#define WIM_TLV_SLEEP_DURATION     52

// 스캔 관련
#define WIM_TLV_SCAN_SSID          60
#define WIM_TLV_SCAN_CHANNEL       61
#define WIM_TLV_SCAN_TYPE          62

// 통계 및 모니터링
#define WIM_TLV_STATS_TYPE         70
#define WIM_TLV_RSSI               71
#define WIM_TLV_SNR                72
```

### C. TLV 인코딩/디코딩 함수들
```c
// TLV 추가 함수들 (wim.c)
int nrc_wim_add_tlv_u8(struct sk_buff *skb, u8 type, u8 value);
int nrc_wim_add_tlv_u16(struct sk_buff *skb, u8 type, u16 value);
int nrc_wim_add_tlv_u32(struct sk_buff *skb, u8 type, u32 value);
int nrc_wim_add_tlv_data(struct sk_buff *skb, u8 type, const void *data, u8 len);

// TLV 파싱 함수들
u8 *nrc_wim_get_tlv(struct sk_buff *skb, u8 type, u8 *length);
u8 nrc_wim_get_tlv_u8(struct sk_buff *skb, u8 type);
u16 nrc_wim_get_tlv_u16(struct sk_buff *skb, u8 type);
u32 nrc_wim_get_tlv_u32(struct sk_buff *skb, u8 type);
```

## 5. WIM 통신 패턴

### A. 비동기 명령 (Fire-and-Forget)
```c
// 단순 설정 명령 - 응답 대기 없음
int nrc_wim_set_channel(struct nrc *nw, int channel)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(WIM_CMD_SET_CHANNEL, 1);
    
    nrc_wim_add_tlv_u8(skb, WIM_TLV_CHANNEL, channel);
    
    return nrc_wim_send(nw, skb);  // 전송 후 즉시 반환
}
```

### B. 동기 명령 (요청-응답)
```c
// 응답이 필요한 명령 - 완료 대기
int nrc_wim_connect(struct nrc *nw, struct ieee80211_bss_conf *bss_conf)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(WIM_CMD_CONNECT, 3);
    
    nrc_wim_add_tlv_data(skb, WIM_TLV_SSID, bss_conf->ssid, bss_conf->ssid_len);
    nrc_wim_add_tlv_data(skb, WIM_TLV_BSSID, bss_conf->bssid, ETH_ALEN);
    nrc_wim_add_tlv_u8(skb, WIM_TLV_CHANNEL, bss_conf->channel);
    
    return nrc_wim_send_request_wait(nw, skb, 5000);  // 5초 타임아웃
}
```

### C. 이벤트 처리 (펌웨어 → 호스트)
```c
// 펌웨어에서 발생한 이벤트 처리
static void nrc_wim_event_handler(struct nrc *nw, struct sk_buff *skb)
{
    struct wim_header *wh = (struct wim_header *)skb->data;
    
    // 이벤트 타입에 따른 콜백 호출
    switch (wh->evt.event) {
        case WIM_EVENT_SCAN_COMPLETED:
            complete(&nw->scan_done);  // 스캔 완료 신호
            break;
        case WIM_EVENT_CONNECTED:
            netif_carrier_on(nw->netdev);  // 네트워크 활성화
            break;
    }
}
```

## 6. WIM 메시지 생명주기

### A. 명령 생성 및 전송
```
1. nrc_wim_alloc_skb() - 메시지 버퍼 할당
2. nrc_wim_add_tlv_*() - 파라미터 추가
3. nrc_wim_send() - HIF를 통한 전송
4. 시퀀스 번호 할당 및 추적
```

### B. 응답 수신 및 처리
```
1. HIF에서 WIM 메시지 수신
2. 시퀀스 번호로 요청 매칭
3. completion 객체로 대기 중인 스레드 깨우기
4. TLV 파라미터 파싱 및 반환
```

### C. 메모리 관리
```c
// 자동 메모리 정리
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

### C. 에러 응답 처리
```c
// WIM 응답 에러 코드
enum WIM_RESULT_CODE {
    WIM_RESULT_SUCCESS = 0,
    WIM_RESULT_INVALID_PARAM,
    WIM_RESULT_NOT_SUPPORTED,
    WIM_RESULT_BUSY,
    WIM_RESULT_TIMEOUT,
    WIM_RESULT_FAILED
};
```

## 8. WIM과 HIF 통합

### A. 이중 큐 시스템
```c
// HIF 레벨에서 두 가지 큐 운영
1. Frame Queue: 데이터 프레임 전송
2. WIM Queue: 제어 명령 전송 (우선순위 높음)
```

### B. 플로우 제어
```c
// 크레딧 기반 전송 제어
- WIM 명령은 별도 크레딧 풀 사용
- 프레임과 독립적인 흐름 제어
- 펌웨어 과부하 방지
```

## 9. WIM 프로토콜의 장점

### A. 확장성
- TLV 구조로 새로운 파라미터 추가 용이
- 하위 호환성 유지하면서 기능 확장
- 펌웨어 업데이트 시 프로토콜 호환성

### B. 안정성  
- 시퀀스 번호로 메시지 추적
- 타임아웃과 재시도 메커니즘
- 메모리 누수 방지 자동 정리

### C. 성능
- 비동기 처리로 블로킹 최소화
- 우선순위 기반 큐 관리
- 효율적인 바이너리 인코딩

WIM 프로토콜은 NRC7292 HaLow 드라이버의 핵심으로, 복잡한 802.11ah 기능들을 체계적이고 안정적으로 제어할 수 있는 강력한 통신 인터페이스를 제공합니다.