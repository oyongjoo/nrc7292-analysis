# NRC7292 드라이버 전력 관리 아키텍처

## HaLow IoT를 위한 전력 최적화

NRC7292 HaLow 드라이버는 IoT 디바이스의 배터리 수명 연장을 위한 포괄적인 전력 관리 시스템을 제공합니다.

## 1. 전력 관리 상태 및 모드

### A. 전력 절약 모드 정의
```c
// nrc.h에 정의된 파워 세이브 모드
enum NRC_PS_MODE {
    NRC_PS_NONE,              // 절전 모드 없음 (정상 동작)
    NRC_PS_MODEMSLEEP,        // 모뎀 슬립 모드
    NRC_PS_DEEPSLEEP_TIM,     // TIM 지원 딥 슬립
    NRC_PS_DEEPSLEEP_NONTIM   // TIM 미지원 딥 슬립
};
```

### B. 드라이버 상태 관리
```c
// 드라이버 동작 상태
enum NRC_DRV_STATE {
    NRC_DRV_INIT,      // 초기화 중
    NRC_DRV_RUNNING,   // 정상 동작
    NRC_DRV_PS,        // 전력 절약/딥 슬립 상태
    NRC_DRV_CLOSING    // 종료 중
};
```

### C. 모듈 매개변수
```c
// 전력 관리 설정 매개변수
static int power_save = 0;           // 절전 모드 활성화 (0~3)
static int bss_max_idle = 300;       // BSS 최대 유휴 시간 (초)
static int bss_max_idle_offset = 0;  // 유휴 시간 오프셋 (ms)
```

## 2. STA 모드 전력 관리

### A. PS-Poll 메커니즘 (nrc-pm.c)
```c
// STA 전력 관리 처리
static int tx_h_sta_pm(struct nrc_trx_data *tx)
{
    // 1. 데이터 프레임의 PM 비트 설정
    // 2. nrc_vif->ps_polling 상태 추적
    // 3. AP로부터 버퍼된 프레임 요청
    
    if (vif->ps_polling) {
        // PS-Poll 진행 중 상태 관리
        ieee80211_sta_ps_transition(vif, false);
    }
}
```

### B. Keep-Alive 메커니즘
```c
// STA 모드 Keep-Alive 타이머
static void sta_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, sta_max_idle_timer);
    
    // 1. QoS Null 프레임 전송으로 연결 유지
    // 2. AP 타임아웃 방지
    // 3. 주기적 전송 스케줄링
    
    if (i_vif->enable_qos_null) {
        // QoS Null 프레임을 주기적으로 전송
        queue_work(nrc_wq, &i_vif->qos_null_work);
    }
}
```

### C. 동적 절전 모드 (Dynamic PS)
```c
// mac80211과 연동된 동적 절전
static void nrc_ps_timeout_timer(struct timer_list *t)
{
    struct nrc *nw = from_timer(nw, t, ps_timer);
    
    // 1. 비활성 시간 경과 후 절전 모드 진입
    // 2. ps_work 큐잉으로 실제 절전 처리
    // 3. 트래픽 감지 시 자동 복귀
    
    queue_work(nrc_ps_wq, &nw->ps_work);
}
```

## 3. AP 모드 전력 관리

### A. 스테이션 모니터링
```c
// AP 모드에서 연결된 STA들의 활성도 모니터링
static void ap_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, ap_max_idle_timer);
    
    // 1. 각 STA의 sta_idle_timer 카운트 감소
    // 2. 3회 연속 타임아웃 시 deauth 전송
    // 3. 비활성 STA 연결 해제
    
    if (sta->sta_idle_timer <= 0) {
        // 비활성 STA 연결 해제
        ieee80211_ap_probereq_get(hw, vif);
    }
}
```

### B. BSS Max Idle 파라미터
```c
// HaLow 특화 BSS Max Idle 설정
#define BSS_MAX_IDLE_TIMER_PERIOD_MS 1000    // 1초 주기
#define BSS_MAX_ILDE_DEAUTH_LIMIT_COUNT 3    // 3회 타임아웃 허용
#define BSS_MAX_IDLE_DEFAULT_VALUE 300       // 기본 300초

// USF (Unit Scaling Factor) 지원
// HaLow에서 더 긴 유휴 시간 지원
```

## 4. 하드웨어 절전 제어

### A. GPIO 기반 Wake-up
```c
// 절전 모드 GPIO 제어
static int power_save_gpio[2] = {-1, -1};  // Wake-up GPIO 핀들
static int sleep_duration[2] = {0, 0};     // 슬립 지속 시간 [값, 단위]

// GPIO 핀 정의
#define RPI_GPIO_FOR_PS 18                 // 라즈베리파이 절전 GPIO
#define TARGET_GPIO_FOR_WAKEUP 1           // 타겟 웨이크업 GPIO
#define TARGET_GPIO_FOR_WAKEUP_HOST 2      // 호스트 웨이크업 GPIO
```

### B. CSPI 레벨 절전 (nrc-hif-cspi.c)
```c
// CSPI 웨이크업 프로토콜
static int spi_wakeup(struct nrc_hif_device *dev)
{
    // 1. 매직 넘버 0x79 전송으로 디바이스 깨우기
    c_spi_write_dummy(spi_priv, 0x79);
    
    // 2. 디바이스 상태 확인
    status = c_spi_read_reg(spi_priv, C_SPI_DEVICE_STATUS);
    
    // 3. 정상 동작 상태까지 대기
    return (status == C_SPI_STATUS_READY) ? 0 : -1;
}

// 시스템 서스펜드/리줌 지원
static int spi_suspend(struct nrc_hif_device *dev)
{
    // IRQ 비활성화 및 리소스 정리
}

static int spi_resume(struct nrc_hif_device *dev)  
{
    // 하드웨어 재초기화 및 IRQ 복원
}
```

## 5. WIM 프로토콜 전력 관리

### A. WIM 절전 명령어
```c
// WIM 전력 관리 명령들
#define WIM_CMD_SLEEP           0x10    // 슬립 모드 진입
#define WIM_TLV_PS_ENABLE       0x20    // 절전 모드 활성화
#define WIM_TLV_SLEEP_DURATION  0x21    // 슬립 지속 시간 설정

// WIM 전력 관리 이벤트
#define WIM_EVENT_PS_READY      0x30    // 절전 준비 완료
#define WIM_EVENT_PS_WAKEUP     0x31    // 웨이크업 알림
```

### B. 전력 관리 파라미터 구조체
```c
struct wim_pm_param {
    u8 ps_mode;              // 절전 모드 (NRC_PS_MODE)
    u8 ps_enable;            // 절전 활성화 플래그
    u16 ps_wakeup_pin;       // 웨이크업 GPIO 핀
    u64 ps_duration;         // 슬립 지속 시간 (마이크로초)
    u32 ps_timeout;          // 동적 절전 타임아웃
    u8 wowlan_enable;        // WoWLAN 지원
    u32 wowlan_pattern;      // 웨이크업 패턴
};
```

## 6. mac80211 통합

### A. mac80211 절전 콜백
```c
// mac80211 설정 변경 처리
static int nrc_mac_config(struct ieee80211_hw *hw, u32 changed)
{
    if (changed & IEEE80211_CONF_CHANGE_PS) {
        // 1. 절전 모드 상태 업데이트
        nw->ps_enabled = (hw->conf.flags & IEEE80211_CONF_PS);
        
        // 2. 펌웨어에 절전 설정 전송
        nrc_wim_set_power_save(nw, nw->ps_enabled);
        
        // 3. 동적 절전 타이머 설정
        if (hw->conf.dynamic_ps_timeout) {
            mod_timer(&nw->ps_timer, 
                     jiffies + msecs_to_jiffies(hw->conf.dynamic_ps_timeout));
        }
    }
}
```

### B. 하드웨어 능력 설정
```c
// 절전 관련 하드웨어 능력 광고
ieee80211_hw_set(hw, SUPPORTS_PS);            // 기본 절전 지원
ieee80211_hw_set(hw, SUPPORTS_DYNAMIC_PS);    // 동적 절전 지원
ieee80211_hw_set(hw, PS_NULLFUNC_STACK);      // Null 함수 프레임 처리
```

## 7. IoT 최적화 특징

### A. HaLow 특화 절전
```c
// Sub-1GHz 대역 특화 최적화
1. 긴 전송 거리로 인한 지연 시간 고려
2. 저전력 IoT 디바이스 배터리 수명 최적화
3. 간헐적 데이터 전송 패턴 지원
4. 확장된 BSS Max Idle 시간 (최대 수 시간)
```

### B. 유연한 웨이크업 메커니즘
```c
// 다중 웨이크업 트리거
1. GPIO 기반 하드웨어 웨이크업
2. 프레임 수신 기반 웨이크업  
3. 타이머 기반 주기적 웨이크업
4. 애플리케이션 트리거 웨이크업
```

### C. 효율적인 Keep-Alive
```c
// 최소 전력으로 연결 유지
1. QoS Null 프레임 사용 (오버헤드 최소)
2. 적응적 전송 주기 (트래픽 패턴 학습)
3. 배터리 상태 기반 절전 레벨 조정
4. 네트워크 조건 기반 최적화
```

## 8. 전력 관리 플로우

### A. STA 모드 절전 시퀀스
```
1. 절전 모드 설정 (power_save 매개변수)
2. mac80211 PS 이벤트 (IEEE80211_CONF_CHANGE_PS)
3. WIM 메시지 전송 (WIM_TLV_PS_ENABLE)
4. HIF 서스펜드 (딥 슬립용)
5. GPIO 웨이크업 설정
6. 주기적 Keep-Alive (QoS Null)
7. 웨이크업 (GPIO/프레임/타이머)
8. 정상 동작 복귀
```

### B. 동적 절전 동작
```
트래픽 감지 → 활성 모드 → 비활성 타임아웃 → 절전 진입 → 웨이크업 → 반복
```

## 9. 성능 및 배터리 최적화

### A. 전력 소모 최소화 전략
- **적극적 절전**: 트래픽 없을 때 즉시 슬립
- **지능형 웨이크업**: 필요시에만 깨어남
- **효율적 프로토콜**: 최소 오버헤드 keep-alive
- **하드웨어 최적화**: CSPI 레벨 절전 지원

### B. IoT 시나리오 지원
- **센서 네트워크**: 주기적 데이터 전송
- **스마트 미터**: 긴 유휴 시간 지원
- **웨어러블**: 동적 절전으로 반응성 유지
- **산업 IoT**: 안정적인 장거리 통신

이 포괄적인 전력 관리 아키텍처는 HaLow의 IoT 특성에 최적화되어, 배터리 수명을 극대화하면서도 필요한 연결성과 응답성을 보장합니다.