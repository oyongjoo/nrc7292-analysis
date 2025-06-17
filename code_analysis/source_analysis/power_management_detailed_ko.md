# NRC7292 전력 관리 상세 소스 코드 분석

## 개요

본 문서는 NRC7292 HaLow 드라이버의 전력 관리 구현에 대한 포괄적인 분석을 제공합니다. 소스 코드를 상세히 검토하여 작성되었으며, 드라이버는 다중 절전 모드, 호스트 측 전력 제어, IEEE 802.11 표준 준수를 갖춘 정교한 전력 관리 시스템을 구현합니다.

## 전력 관리 아키텍처

### 1. 전력 모드 정의

드라이버는 `nrc.h`에서 정의된 4가지 별개의 절전 모드를 지원합니다:

```c
enum NRC_PS_MODE {
    NRC_PS_NONE,              // 절전 없음
    NRC_PS_MODEMSLEEP,        // 모뎀 절전 모드
    NRC_PS_DEEPSLEEP_TIM,     // TIM 수신 기능이 있는 딥슬립
    NRC_PS_DEEPSLEEP_NONTIM   // TIM 수신 기능이 없는 딥슬립
};
```

### 2. 드라이버 상태 관리

드라이버는 여러 상태 변수를 통해 전력 상태를 관리합니다:

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
    NRC_DRV_PS,               // 절전 상태
};
```

## 핵심 전력 관리 구현

### 1. STA 절전 관리 (`nrc-pm.c`)

#### PS-Poll 메커니즘 구현

드라이버는 불완전한 서비스 기간에 대한 정교한 해결책을 포함한 PS-Poll 처리를 구현합니다:

```c
static int tx_h_sta_pm(struct nrc_trx_data *tx)
{
    struct ieee80211_hw *hw = tx->nw->hw;
    struct sk_buff *skb = tx->skb;
    struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
    struct ieee80211_hdr *mh = (void *) skb->data;
    __le16 fc = mh->frame_control;

    // 데이터 프레임에 대한 전력 관리 필드 설정
    if (!(txi->flags & IEEE80211_TX_CTL_NO_ACK) &&
        ieee80211_is_data(fc) && !ieee80211_has_pm(fc)) {
        mh->frame_control |= cpu_to_le16(IEEE80211_FCTL_PM);
        fc = mh->frame_control;
    }

    // PS-Poll 상태 추적
    if (ieee80211_is_pspoll(fc)) {
        struct nrc_vif *i_vif = to_i_vif(tx->vif);
        i_vif->ps_polling = true;
    }

    return 0;
}
```

#### PS-Poll 서비스 기간 수정

중요한 구현 세부사항은 PS-Poll 서비스 기간 완료 해결책입니다:

```c
static int rx_h_fixup_ps_poll_sp(struct nrc_trx_data *rx)
{
    struct ieee80211_hw *hw = rx->nw->hw;
    struct nrc_vif *i_vif = to_i_vif(rx->vif);
    struct ieee80211_hdr *mh = (void *) rx->skb->data;
    __le16 fc = mh->frame_control;

    // PS-Poll 서비스 기간 중인지 확인
    if (rx->vif->type != NL80211_IFTYPE_STATION || !i_vif->ps_polling)
        return 0;

    // 프레임이 서빙 AP에서 온 것인지 확인
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

### 2. BSS Max Idle Period 구현

#### Keep-Alive 타이머 관리

드라이버는 AP와 STA 모드용 별도 타이머로 BSS Max Idle Period를 구현합니다:

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

    // 상태 전환 검증
    if (state_changed(ASSOC, AUTHORIZED)) {
        max_idle_period = i_sta->max_idle.period;
        
        if (max_idle_period == 0) {
            nrc_mac_dbg("[%s] max_idle_period is 0", __func__);
            return 0;
        }

        // S1G용 확장 BSS Max Idle Period
        if (nrc_mac_is_s1g(hw->priv)) {
            u8 usf = (max_idle_period >> 14) & 0x3;
            max_idle_period &= ~0xc000;
            max_idle_period *= ieee80211_usf_to_sf(usf);
        }

        // STA 모드: AP 타이머 만료 전에 keep-alive 전송
        if (vif->type == NL80211_IFTYPE_STATION) {
            if (bss_max_idle_offset == 0) {
                if (max_idle_period > 2)
                    idle_offset = -768; // 768ms 여유
                else
                    idle_offset = -100; // 100ms 여유
            } else {
                idle_offset = bss_max_idle_offset;
            }
        }

        timeout_ms = (u64)max_idle_period * 1024 + (u64)idle_offset;
        if (timeout_ms < 924) {
            timeout_ms = 924; // 최소 안전 임계값
        }

        // 타이머 설정
        if (vif->type == NL80211_IFTYPE_STATION) {
            timer_setup(&i_vif->max_idle_timer, sta_max_idle_period_expire, 0);
            i_sta->max_idle.idle_period = msecs_to_jiffies(timeout_ms);
            mod_timer(&i_vif->max_idle_timer, jiffies + i_sta->max_idle.idle_period);
        } else {
            // AP 모드 타이머 설정
            if (!timer_pending(&i_vif->max_idle_timer)) {
                timer_setup(&i_vif->max_idle_timer, ap_max_idle_period_expire, 0);
                mod_timer(&i_vif->max_idle_timer, 
                         jiffies + msecs_to_jiffies(BSS_MAX_IDLE_TIMER_PERIOD_MS));
            }
            i_sta->max_idle.idle_period = timeout_ms >> 10; // 초 단위로 변환
        }
    }

    return 0;
}
```

#### QoS Null Keep-Alive 전송

STA 모드 keep-alive 구현은 QoS Null 프레임을 전송합니다:

```c
static void sta_max_idle_period_expire(struct timer_list *t)
{
    struct nrc_vif *i_vif = from_timer(i_vif, t, max_idle_timer);
    struct ieee80211_hw *hw = i_vif->nw->hw;
    struct sk_buff *skb;
    struct ieee80211_hdr_3addr_qos *qosnullfunc;

    // QoS Null 프레임 생성
    skb = ieee80211_nullfunc_get(hw, i_sta->vif, false);
    skb_put(skb, 2);
    qosnullfunc = (struct ieee80211_hdr_3addr_qos *) skb->data;
    qosnullfunc->frame_control |= cpu_to_le16(IEEE80211_STYPE_QOS_NULL);
    qosnullfunc->qc = cpu_to_le16(0);
    skb_set_queue_mapping(skb, IEEE80211_AC_VO);

    // keep-alive 프레임 전송
    nrc_mac_tx(hw, &control, skb);

    // 타이머 재설정
    mod_timer(&i_vif->max_idle_timer, jiffies + i_sta->max_idle.idle_period);
}
```

#### AP 모드 비활성 모니터링

AP 모드는 STA 활동을 모니터링하고 비활성 스테이션을 연결 해제합니다:

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
                    // 비활성 스테이션 연결 해제
                    ieee80211_disconnect_sta(vif, sta);
                } else {
                    // 백오프로 재설정
                    i_sta->max_idle.sta_idle_timer = i_sta->max_idle.idle_period;
                }
            }
        }
    }
    spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);
}
```

### 3. TX 경로 전력 관리 (`nrc-trx.c`)

#### 모뎀 절전 웨이크업 로직

TX 경로는 다양한 절전 모드에 대한 전력 관리 로직을 포함합니다:

```c
void nrc_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
        struct sk_buff *skb)
{
    // STA 모드용 절전 처리
    if (tx.nw->vif[vif_id]->type == NL80211_IFTYPE_STATION) {
        if (tx.nw->drv_state == NRC_DRV_PS)
            nrc_hif_wake_target(tx.nw->hif);

        // 모뎀 절전 웨이크업
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

        // 딥슬립 deauth 처리
        if (power_save >= NRC_PS_DEEPSLEEP_TIM) {
            if (tx.nw->drv_state == NRC_DRV_PS) {
                memset(&tx.nw->d_deauth, 0, sizeof(struct nrc_delayed_deauth));
                if (ieee80211_is_deauth(mh->frame_control)) {
                    // 딥슬립 중 deauth 처리
                }
            }
        }
    }
}
```

### 4. WIM 전력 관리 인터페이스

#### WIM 전력 관리 매개변수

WIM 프로토콜은 포괄적인 전력 관리 매개변수를 정의합니다:

```c
struct wim_pm_param {
    uint8_t ps_mode;                    // 절전 모드
    uint8_t ps_enable;                  // 활성화/비활성화 플래그
    uint16_t ps_wakeup_pin;            // GPIO 웨이크업 핀
    uint64_t ps_duration;              // 절전 지속시간
    uint32_t ps_timeout;               // 웨이크업 타임아웃
    uint8_t wowlan_wakeup_host_pin;    // WoWLAN 호스트 웨이크 핀
    uint8_t wowlan_enable_any;         // 모든 패킷 웨이크
    uint8_t wowlan_enable_magicpacket; // 매직 패킷 웨이크
    uint8_t wowlan_enable_disconnect;  // 연결 해제 웨이크
    uint8_t wowlan_n_patterns;         // 패턴 수
    struct wowlan_pattern wp[2];       // 웨이크 패턴
} __packed;
```

#### WoWLAN 패턴 정의

```c
#define WOWLAN_PATTER_SIZE 48

struct wowlan_pattern {
    uint16_t offset:6;        // 패킷 오프셋
    uint16_t mask_len:4;      // 마스크 길이
    uint16_t pattern_len:6;   // 패턴 길이
    uint8_t mask[WOWLAN_PATTER_SIZE/8];      // 비트마스크
    uint8_t pattern[WOWLAN_PATTER_SIZE];     // 패턴 데이터
} __packed;
```

### 5. HIF 계층 전력 제어 (`nrc-hif-cspi.c`)

#### GPIO 기반 웨이크 제어

HIF 계층은 GPIO 기반 타겟 웨이크업을 구현합니다:

```c
// 장치 상태 레지스터 정의
#define EIRQ_DEV_SLEEP  (1<<3)
#define EIRQ_DEV_READY  (1<<2)
#define EIRQ_RXQ        (1<<1)
#define EIRQ_TXQ        (1<<0)

// SPI 시스템 레지스터 구조
struct spi_sys_reg {
    u8 wakeup;      // 0x0 - 웨이크업 제어
    u8 status;      // 0x1 - 장치 상태
    u16 chip_id;    // 0x2-0x3 - 칩 식별
    u32 modem_id;   // 0x4-0x7 - 모뎀 식별
    u32 sw_id;      // 0x8-0xb - 소프트웨어 버전
    u32 board_id;   // 0xc-0xf - 보드 식별
} __packed;
```

## 고급 전력 관리 기능

### 1. 동적 절전 타이머

드라이버는 사용자 정의 가능한 타임아웃으로 mac80211 동적 절전을 구현합니다:

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

### 2. 절전 지속시간 구성

드라이버는 다양한 전력 모드에 대해 구성 가능한 절전 지속시간을 지원합니다:

```c
// 절전 지속시간 배열용 모듈 매개변수
extern int sleep_duration[];

struct wim_sleep_duration_param {
    uint32_t sleep_ms;
} __packed;
```

### 3. 전력 상태 동기화

드라이버는 전력 상태 전환을 위한 완료 메커니즘을 구현합니다:

```c
struct nrc {
    // 전력 관리 동기화
    struct completion hif_tx_stopped;
    struct completion hif_rx_stopped;
    struct completion hif_irq_stopped;
};
```

## IEEE 802.11 표준 준수

### 1. 전력 관리 필드 처리

드라이버는 현재 전력 상태에 따라 전송된 프레임에서 전력 관리 필드를 자동으로 설정합니다.

### 2. TIM 요소 처리

딥슬립 모드의 경우, 드라이버는 언제 깨어나서 버퍼된 프레임을 검색할지 결정하기 위해 TIM 요소 처리를 구현합니다.

### 3. Listen Interval 지원

드라이버는 협상된 listen interval을 준수하고 그에 따라 웨이크업 타이밍을 조정합니다.

## 오류 처리 및 복구

### 1. 전력 상태 중 펌웨어 복구

드라이버는 절전 상태 중 펌웨어 크래시를 처리하고 복구 절차를 시작하는 메커니즘을 포함합니다.

### 2. 지연된 Deauth 처리

딥슬립 모드의 경우, 적절한 상태 정리를 보장하기 위해 deauth 프레임이 특별히 처리됩니다:

```c
struct nrc_delayed_deauth {
    atomic_t delayed_deauth;
    s8 vif_index;
    u16 aid;
    bool removed;
    struct sk_buff *deauth_frm;
    // 상태 보존 구조체
    struct ieee80211_vif v;
    struct ieee80211_sta s;
    struct ieee80211_key_conf p;
    struct ieee80211_key_conf g;
    struct ieee80211_bss_conf b;
};
```

## 구성 및 튜닝

### 1. 모듈 매개변수

주요 전력 관리 매개변수는 모듈 매개변수를 통해 구성할 수 있습니다:

- `power_save`: 절전 모드 선택
- `sleep_duration[]`: 절전 지속시간 배열
- `bss_max_idle_offset`: BSS max idle 타이밍 오프셋
- `power_save_gpio[]`: 전력 제어용 GPIO 구성

### 2. 동적 구성

런타임 구성은 netlink 인터페이스 및 debugfs 항목을 통해 지원됩니다.

## 성능 고려사항

### 1. 웨이크업 지연시간

드라이버는 중요한 데이터 구조를 미리 로드하고 최소 웨이크업 경로를 유지하여 웨이크업 지연시간을 최적화합니다.

### 2. 전력 소비 최적화

다양한 절전 모드는 다양한 수준의 전력 절약 대 응답성 트레이드오프를 제공합니다:

- **MODEMSLEEP**: 빠른 웨이크업, 중간 전력 절약
- **DEEPSLEEP_TIM**: TIM 모니터링을 통한 균형 잡힌 전력 절약
- **DEEPSLEEP_NONTIM**: 최대 전력 절약, 수동 웨이크업 필요

### 3. 처리량 영향

드라이버는 성능을 유지하기 위해 고처리량 기간 동안 절전을 일시적으로 비활성화하는 메커니즘을 포함합니다.

## 결론

NRC7292 전력 관리 구현은 전력 효율성과 네트워크 성능의 균형을 맞추는 포괄적인 솔루션을 제공합니다. HIF 레벨 GPIO 제어부터 고레벨 IEEE 802.11 준수까지의 다층 접근 방식은 표준 무선 인프라와의 호환성을 유지하면서 다양한 사용 사례에서 견고한 작동을 보장합니다.

정교한 BSS Max Idle Period 구현은 유연한 절전 모드 및 WoWLAN 지원과 결합되어 이 드라이버를 연장된 배터리 수명이 중요한 IoT 및 저전력 애플리케이션에 적합하게 만듭니다.

## 주요 함수별 전력 관리 역할

### 1. 전력 상태 관리 함수

#### `tx_h_sta_pm()` - TX 전력 관리 핸들러
**목적**: 전송 프레임에 전력 관리 필드를 설정하고 PS-Poll 상태를 추적
**매개변수**: 
- `tx`: 전송 데이터 구조체
**절전 로직**:
1. 현재 전력 상태에 따라 PM 필드 설정
2. PS-Poll 프레임 감지 시 polling 상태 활성화
3. mac80211 동적 절전 지원

#### `rx_h_fixup_ps_poll_sp()` - PS-Poll 서비스 기간 수정
**목적**: 불완전한 PS-Poll 서비스 기간 해결
**웨이크업 조건**:
- 비콘 수신 시 가상 Null 프레임 응답 생성
- 데이터 프레임 수신 시 polling 상태 해제
**전력 상태 변화**: PS-Poll 상태에서 일반 수신 상태로 전환

### 2. Keep-Alive 메커니즘

#### `sta_max_idle_period_expire()` - STA Keep-Alive 타이머
**목적**: 주기적 QoS Null 프레임 전송으로 연결 유지
**절전 설정값**:
- 타이머 간격: BSS Max Idle Period에서 오프셋 적용
- 프레임 타입: QoS Null (우선순위 VO)
**사용 커널 PM API**: `timer_setup()`, `mod_timer()`

#### `ap_max_idle_period_expire()` - AP 비활성 모니터링
**목적**: 연결된 STA들의 활동 모니터링 및 비활성 STA 연결 해제
**절전 로직 단계**:
1. STA 리스트 순회하며 idle 타이머 감소
2. 타임아웃 카운트 증가 및 임계값 확인
3. 한계 초과 시 deauth 프레임 전송
**웨이크업 조건**: 데이터 프레임 수신 시 타이머 리셋

### 3. 전력 모드별 구현

#### 모뎀 절전 (`NRC_PS_MODEMSLEEP`)
**진입 조건**: 
- 데이터 비활성 감지
- 동적 절전 타이머 만료
**해제 조건**:
- 전송할 데이터 있음
- WIM 명령을 통한 수동 웨이크업
**상태 추적**: `ps_modem_enabled`, `ps_drv_state`

#### 딥슬립 TIM 모드 (`NRC_PS_DEEPSLEEP_TIM`)
**진입 조건**:
- 장기간 비활성
- TIM 수신 스케줄링 완료
**웨이크업 조건**:
- TIM에 AID 포함
- 중요한 관리 프레임 (deauth 등)
**특별 처리**: `d_deauth` 구조체를 통한 지연된 deauth 처리

#### 딥슬립 Non-TIM 모드 (`NRC_PS_DEEPSLEEP_NONTIM`)
**진입 조건**: 최대 절전 요구
**웨이크업 조건**:
- GPIO 인터럽트
- 수동 웨이크업 명령
- WoWLAN 패턴 매칭

### 4. WIM 전력 관리 명령

#### `nrc_wim_pm_req()` - WIM 절전 요청
**목적**: 펌웨어에 절전 모드 변경 지시
**매개변수**:
- `cmd`: WIM_CMD_SLEEP 등
- `arg`: 절전 지속시간, GPIO 설정 등
**WIM 프로토콜 통신**:
1. `wim_pm_param` 구조체 구성
2. TLV 형식으로 매개변수 패킹
3. HIF를 통한 펌웨어 전송

### 5. GPIO 기반 웨이크업

#### `nrc_hif_wake_target()` - 타겟 웨이크업
**목적**: GPIO를 통한 하드웨어 웨이크업
**웨이크업 시퀀스**:
1. SPI 웨이크업 레지스터 write
2. EIRQ 상태 확인
3. 장치 ready 신호 대기
**타임아웃 처리**: 웨이크업 실패 시 재시도 또는 오류 보고

### 6. 자동 절전 제어

#### 데이터 활동 모니터링
**모니터링 대상**:
- TX/RX 프레임 카운트
- 큐 상태 (empty/non-empty)
- EAPOL 프레임 활동
**자동 진입 로직**:
1. 설정된 비활성 기간 확인
2. 대기 중인 TX 데이터 없음 확인
3. 동적 절전 타이머 시작

#### 절전 조건 판단
**진입 차단 조건**:
- 활성 스캔 진행 중
- 연결 설정 진행 중
- 높은 우선순위 트래픽 존재
**강제 해제 조건**:
- 긴급 관리 프레임
- 사용자 데이터 전송 요청

### 7. IEEE 802.11 전력 관리 표준 연관성

#### 표준 준수 기능
**PM 필드 자동 관리**:
- 전송 프레임의 PM 비트 설정
- 수신 프레임의 PM 비트 해석
**Listen Interval 처리**:
- 연결 시 협상된 값 저장
- 비콘 수신 스케줄링에 적용

#### TIM 요소 처리
**TIM 비트맵 파싱**:
- AID 기반 비트 위치 계산
- 부분 가상 비트맵 처리
**DTIM 특별 처리**:
- 브로드캐스트/멀티캐스트 데이터
- DTIM 기간 동안 필수 웨이크업

## 전력 관리 상태 머신

### 상태 전환 다이어그램

```
[AWAKE] ←→ [MODEMSLEEP] ← [DEEPSLEEP_TIM] ← [DEEPSLEEP_NONTIM]
   ↑            ↑              ↑                    ↑
   │            │              │                    │
   └─── 데이터 ──┴── 짧은 비활성 ──┴─── 중간 비활성 ─────┴─── 장기 비활성
```

### 각 상태별 특성

1. **AWAKE**: 완전 활성, 즉시 응답
2. **MODEMSLEEP**: 빠른 웨이크업 (< 10ms), 중간 절전
3. **DEEPSLEEP_TIM**: TIM 기반 웨이크업, 높은 절전
4. **DEEPSLEEP_NONTIM**: 수동 웨이크업만, 최대 절전