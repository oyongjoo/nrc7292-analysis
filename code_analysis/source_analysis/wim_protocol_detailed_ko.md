# WIM 프로토콜 상세 소스 코드 분석

본 문서는 NRC7292 HaLow 드라이버 패키지의 WIM(Wireless Interface Module) 프로토콜 구현에 대한 포괄적인 분석을 제공합니다.

## 개요

WIM 프로토콜은 Linux 커널 드라이버와 NRC7292 펌웨어 간의 핵심 통신 인터페이스입니다. 명령, 응답, 이벤트를 위한 TLV(Type-Length-Value) 기반 메시징 시스템을 구현합니다.

## 목차

1. [WIM 프로토콜 구조](#wim-프로토콜-구조)
2. [WIM 메시지 생성](#wim-메시지-생성)
3. [WIM 명령 처리](#wim-명령-처리)
4. [WIM 응답 처리](#wim-응답-처리)
5. [WIM 이벤트 처리](#wim-이벤트-처리)
6. [주요 WIM 명령들](#주요-wim-명령들)
7. [TLV 시스템 구현](#tlv-시스템-구현)
8. [동기화 메커니즘](#동기화-메커니즘)
9. [에러 처리](#에러-처리)
10. [펌웨어와의 상호작용](#펌웨어와의-상호작용)

## WIM 프로토콜 구조

### WIM 헤더 구조 (`wim.h` 33-42줄)

```c
struct wim {
    union {
        u16 cmd;
        u16 resp;
        u16 event;
    };
    u8 seqno;
    u8 n_tlvs;
    u8 payload[0];
} __packed;
```

**분석:**
- **Union 설계**: 명령, 응답, 이벤트가 동일한 헤더 구조를 공유하지만 첫 번째 필드에 대해 다른 의미를 사용
- **시퀀스 번호**: `seqno`는 요청/응답 매칭 기능 제공
- **TLV 개수**: `n_tlvs`는 페이로드의 TLV 요소 개수를 나타냄
- **가변 페이로드**: 유연한 페이로드 크기를 위한 길이가 0인 배열

### TLV 구조 (`wim.h` 44-48줄)

```c
struct wim_tlv {
    u16 t;      // Type
    u16 l;      // Length
    u8  v[0];   // Value (가변 길이)
} __packed;
```

**목적:**
- 확장 가능한 매개변수 전달 메커니즘 제공
- 선택적 TLV를 통한 하위 호환성 지원
- 중첩된 TLV를 통한 복잡한 데이터 구조 지원

## WIM 메시지 생성

### 핵심 할당 함수 (`wim.c` 39-60줄)

```c
struct sk_buff *nrc_wim_alloc_skb(struct nrc *nw, u16 cmd, int size)
{
    struct sk_buff *skb;
    struct wim *wim;

    skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
    if (!skb)
        return NULL;

    /* 헤드룸 증가 */
    skb_reserve(skb, sizeof(struct hif));

    /* WIM 헤더 추가 */
    wim = (struct wim *)skb_put(skb, sizeof(*wim));
    memset(wim, 0, sizeof(*wim));
    wim->cmd = cmd;
    wim->seqno = nw->wim_seqno++;

    nrc_wim_skb_bind_vif(skb, NULL);

    return skb;
}
```

**주요 특징:**
1. **메모리 레이아웃 계획**: 향후 재할당을 피하기 위해 HIF 헤더 공간을 예약
2. **시퀀스 번호 관리**: 요청 추적을 위한 자동 증가 시퀀스 번호
3. **VIF 바인딩**: 메시지를 가상 인터페이스 컨텍스트와 연결
4. **제로 초기화**: 깨끗한 헤더 상태 보장

### TLV 추가 함수 (`wim.c` 97-114줄)

```c
void *nrc_wim_skb_add_tlv(struct sk_buff *skb, u16 T, u16 L, void *V)
{
    struct wim_tlv *tlv;

    if (L == 0) {
        tlv = (struct wim_tlv *)(skb_put(skb, sizeof(struct wim_tlv)));
        wim_set_tlv(tlv, T, L);
        return (void *)skb->data;
    }

    tlv = (struct wim_tlv *)(skb_put(skb, tlv_len(L)));
    wim_set_tlv(tlv, T, L);

    if (V)
        memcpy(tlv->v, V, L);

    return (void *)tlv->v;
}
```

**분석:**
- **동적 크기 조정**: 길이가 0인 TLV와 가변 길이 TLV 모두 처리
- **조건부 복사**: 소스 포인터가 제공된 경우에만 데이터 복사
- **반환값**: 추가 조작을 위해 값 필드를 가리킴
- **메모리 안전성**: 적절한 SKB 크기 관리를 위해 `skb_put()` 사용

## WIM 명령 처리

### 단순 명령 전송 (`wim.c` 76-81줄)

```c
int nrc_xmit_wim_simple_request(struct nrc *nw, int cmd)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(nw, cmd, 0);
    return nrc_xmit_wim_request(nw, skb);
}
```

### 응답 대기하는 동기 명령 (`wim.c` 83-89줄)

```c
struct sk_buff *nrc_xmit_wim_simple_request_wait(struct nrc *nw, int cmd,
        int timeout)
{
    struct sk_buff *skb = nrc_wim_alloc_skb(nw, cmd, 0);
    return nrc_xmit_wim_request_wait(nw, skb, timeout);
}
```

### 요청/응답 동기화 (`hif.c` 583-602줄)

```c
struct sk_buff *nrc_xmit_wim_request_wait(struct nrc *nw,
        struct sk_buff *skb, int timeout)
{
    mutex_lock(&nw->target_mtx);
    nw->last_wim_responded = NULL;

    if (nrc_xmit_wim(nw, skb, HIF_WIM_SUB_REQUEST) < 0) {
        mutex_unlock(&nw->target_mtx);
        return NULL;
    }

    mutex_unlock(&nw->target_mtx);
    
    reinit_completion(&nw->wim_responded);
    if (wait_for_completion_timeout(&nw->wim_responded,
            timeout) == 0)
        return NULL;

    return nw->last_wim_responded;
}
```

**동기화 분석:**
1. **뮤텍스 보호**: 요청 설정 중 경쟁 조건 방지
2. **완료 메커니즘**: 효율적인 대기를 위해 Linux completion API 사용
3. **타임아웃 처리**: 응답하지 않는 펌웨어에 대한 무한 블로킹 방지
4. **응답 검색**: 처리를 위해 실제 응답 SKB 반환

## WIM 응답 처리

### 응답 핸들러 (`wim.c` 679-692줄)

```c
static int nrc_wim_response_handler(struct nrc *nw,
                    struct sk_buff *skb)
{
    mutex_lock(&nw->target_mtx);
    nw->last_wim_responded = skb;
    mutex_unlock(&nw->target_mtx);
    if (completion_done(&nw->wim_responded)) {
    /* 완료 대기자가 없음, SKB 해제 */
        pr_err("no completion");
        return 0;
    }
    complete(&nw->wim_responded);
    return 1;
}
```

**핵심 메커니즘:**
- **스레드 안전성**: 뮤텍스가 응답 저장을 보호
- **완료 신호**: 대기 중인 스레드를 깨움
- **메모리 관리**: 자동 SKB 해제를 방지하기 위해 `1` 반환
- **에러 감지**: 대기 중인 스레드가 없는 경우 처리

## WIM 이벤트 처리

### 이벤트 핸들러 (`wim.c` 724-822줄)

```c
static int nrc_wim_event_handler(struct nrc *nw,
                 struct sk_buff *skb)
{
    struct ieee80211_vif *vif = NULL;
    struct wim *wim = (void *) skb->data;
    struct hif *hif = (void *)(skb->data - sizeof(*hif));
    
    /* hif->vifindex to vif */
    if (hif->vifindex != -1)
        vif = nw->vif[hif->vifindex];

    switch (wim->event) {
    case WIM_EVENT_SCAN_COMPLETED:
        nrc_mac_cancel_hw_scan(nw->hw, vif);
        // 비콘 모니터링 및 절전 모드를 위한 타이머 관리
        break;
    case WIM_EVENT_READY:
        nrc_wim_handle_fw_ready(nw);
        break;
    case WIM_EVENT_CREDIT_REPORT:
        nrc_wim_update_tx_credit(nw, wim);
        break;
    // ... 기타 이벤트들
    }
    
    return 0;
}
```

### 주요 이벤트 유형 분석:

1. **WIM_EVENT_SCAN_COMPLETED**: 
   - 스캔 완료 알림
   - 비콘 모니터링 및 절전 타이머 재활성화

2. **WIM_EVENT_READY**: 
   - 펌웨어 초기화 완료
   - 드라이버 상태를 활성으로 전환 트리거

3. **WIM_EVENT_CREDIT_REPORT**: 
   - TX 큐를 위한 플로우 제어 메커니즘
   - AC별 크레딧 카운터 업데이트

## 주요 WIM 명령들

### 스테이션 관리 (`wim.c` 153-175줄)

```c
int nrc_wim_change_sta(struct nrc *nw, struct ieee80211_vif *vif,
               struct ieee80211_sta *sta, u8 cmd, bool sleep)
{
    struct sk_buff *skb;
    struct wim_sta_param *p;

    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_STA_CMD,
                    tlv_len(sizeof(*p)));

    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_STA_PARAM, sizeof(*p), NULL);
    memset(p, 0, sizeof(*p));

    p->cmd = cmd;
    p->flags = 0;
    p->sleep = sleep;
    ether_addr_copy(p->addr, sta->addr);
    p->aid = sta->aid;

    return nrc_xmit_wim_request(nw, skb);
}
```

### 하드웨어 스캔 명령 (`wim.c` 177-309줄)

```c
int nrc_wim_hw_scan(struct nrc *nw, struct ieee80211_vif *vif,
            struct cfg80211_scan_request *req,
            struct ieee80211_scan_ies *ies)
{
    struct sk_buff *skb;
    struct wim_scan_param *p;
    int i, size = tlv_len(sizeof(struct wim_scan_param));

    // IE를 위한 필요한 크기 계산
    if (ies) {
        size += tlv_len(ies->common_ie_len);
        size += sizeof(struct wim_tlv);
        size += ies->len[NL80211_BAND_S1GHZ];
    }

    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_SCAN_START, size);

    /* WIM_TLV_SCAN_PARAM */
    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_SCAN_PARAM, sizeof(*p), NULL);
    
    // 스캔 매개변수 채우기
    p->n_channels = req->n_channels;
    for (i = 0; i < req->n_channels; i++) {
        p->channel[i] = FREQ_TO_100KHZ(req->channels[i]->center_freq, 
                                       req->channels[i]->freq_offset);
    }
    
    p->n_ssids = req->n_ssids;
    for (i = 0; i < req->n_ssids; i++) {
        p->ssid[i].ssid_len = req->ssids[i].ssid_len;
        memcpy(p->ssid[i].ssid, req->ssids[i].ssid, req->ssids[i].ssid_len);
    }

    // IE가 있는 경우 추가
    if (ies) {
        // 밴드별 IE
        nrc_wim_skb_add_tlv(skb, WIM_TLV_SCAN_BAND_IE, 
                           ies->len[NL80211_BAND_S1GHZ], ies->ies[NL80211_BAND_S1GHZ]);
        
        // 공통 IE
        nrc_wim_skb_add_tlv(skb, WIM_TLV_SCAN_COMMON_IE, 
                           ies->common_ie_len, (void *)ies->common_ies);
    }

    return nrc_xmit_wim_request(nw, skb);
}
```

### 키 설치 (`wim.c` 363-455줄)

```c
int nrc_wim_install_key(struct nrc *nw, enum set_key_cmd cmd,
            struct ieee80211_vif *vif,
            struct ieee80211_sta *sta,
            struct ieee80211_key_conf *key)
{
    struct sk_buff *skb;
    struct wim_key_param *p;
    u8 cipher;
    const u8 *addr;
    u16 aid = 0;

    cipher = nrc_to_wim_cipher_type(key->cipher);
    if (cipher == -1)
        return -ENOTSUPP;

    skb = nrc_wim_alloc_skb_vif(nw, vif, WIM_CMD_SET_KEY + (cmd - SET_KEY),
                tlv_len(sizeof(*p)));

    p = nrc_wim_skb_add_tlv(skb, WIM_TLV_KEY_PARAM, sizeof(*p), NULL);

    // 컨텍스트에 따라 대상 주소와 AID 결정
    if (sta) {
        addr = sta->addr;
    } else if (vif->type == NL80211_IFTYPE_AP) {
        addr = vif->addr;
    } else {
        addr = vif->bss_conf.bssid;
    }

    ether_addr_copy(p->mac_addr, addr);
    p->aid = aid;
    memcpy(p->key, key->key, key->keylen);
    p->cipher_type = cipher;
    p->key_index = key->keyidx;
    p->key_len = key->keylen;
    p->key_flags = (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) ?
        WIM_KEY_FLAG_PAIRWISE : WIM_KEY_FLAG_GROUP;

    return nrc_xmit_wim_request(nw, skb);
}
```

## TLV 시스템 구현

### TLV 유형 (`nrc-wim-types.h` 146-230줄)

WIM 프로토콜은 다양한 매개변수 카테고리를 위한 광범위한 TLV 유형을 정의합니다:

```c
enum WIM_TLV_ID {
    WIM_TLV_SSID = 0,
    WIM_TLV_BSSID = 1,
    WIM_TLV_MACADDR = 2,
    WIM_TLV_AID = 3,
    WIM_TLV_STA_TYPE = 4,
    WIM_TLV_SCAN_PARAM = 5,
    WIM_TLV_KEY_PARAM = 6,
    // ... 광범위한 목록이 계속됨
    WIM_TLV_DEFAULT_MCS = 80,
    WIM_TLV_MAX,
};
```

### TLV 헬퍼 함수

**AID 설정** (`wim.c` 116-119줄):
```c
void nrc_wim_set_aid(struct nrc *nw, struct sk_buff *skb, u16 aid)
{
    nrc_wim_skb_add_tlv(skb, WIM_TLV_AID, sizeof(u16), &aid);
}
```

**MAC 주소 설정** (`wim.c` 121-124줄):
```c
void nrc_wim_add_mac_addr(struct nrc *nw, struct sk_buff *skb, u8 *addr)
{
    nrc_wim_skb_add_tlv(skb, WIM_TLV_MACADDR, ETH_ALEN, addr);
}
```

## 동기화 메커니즘

### 요청/응답 매칭

WIM 프로토콜은 다음을 통해 정교한 동기화를 구현합니다:

1. **시퀀스 번호**: 각 요청은 고유한 시퀀스 번호를 가짐
2. **완료 객체**: 효율적인 대기를 위한 Linux completion API
3. **뮤텍스 보호**: 멀티스레드 접근에서 경쟁 조건 방지
4. **타임아웃 처리**: 무한 블로킹 방지

### 플로우 제어 (`wim.c` 695-722줄)

```c
static int nrc_wim_update_tx_credit(struct nrc *nw, struct wim *wim)
{
    struct wim_credit_report *r = (void *)(wim + 1);
    int ac;

    for (ac = 0; ac < (IEEE80211_NUM_ACS*3); ac++)
        atomic_set(&nw->tx_credit[ac], r->v.ac[ac]);

    nrc_kick_txq(nw);
    return 0;
}
```

## 에러 처리

### 암호화 유형 검증 (`wim.c` 327-345줄)

```c
enum wim_cipher_type nrc_to_wim_cipher_type(u32 cipher)
{
    switch (cipher) {
    case WLAN_CIPHER_SUITE_WEP40:
        return WIM_CIPHER_TYPE_WEP40;
    case WLAN_CIPHER_SUITE_WEP104:
        return WIM_CIPHER_TYPE_WEP104;
    case WLAN_CIPHER_SUITE_TKIP:
        return WIM_CIPHER_TYPE_TKIP;
    case WLAN_CIPHER_SUITE_CCMP:
        return WIM_CIPHER_TYPE_CCMP;
    case WLAN_CIPHER_SUITE_AES_CMAC:
    case WLAN_CIPHER_SUITE_BIP_GMAC_128:
    case WLAN_CIPHER_SUITE_BIP_GMAC_256:
        return WIM_CIPHER_TYPE_NONE;
    default:
        return WIM_CIPHER_TYPE_INVALID;
    }
}
```

### 응답 타임아웃 처리

동기식 WIM 요청 메커니즘은 펌웨어가 응답하지 않을 때 시스템 행을 방지하기 위한 포괄적인 타임아웃 처리를 포함합니다.

## 펌웨어와의 상호작용

### 메시지 라우팅 (`wim.c` 824-850줄)

```c
int nrc_wim_rx(struct nrc *nw, struct sk_buff *skb, u8 subtype)
{
    int ret;

    ret = wim_rx_handler[subtype](nw, skb);

    if (ret != 1)
        /* 나중에 SKB 해제 (예: 응답 WIM 핸들러에서) */
        dev_kfree_skb(skb);

    return 0;
}
```

### 핸들러 디스패치 테이블 (`wim.c` 826-830줄)

```c
static wim_rx_func wim_rx_handler[] = {
    [HIF_WIM_SUB_REQUEST] = nrc_wim_request_handler,
    [HIF_WIM_SUB_RESPONSE] = nrc_wim_response_handler,
    [HIF_WIM_SUB_EVENT] = nrc_wim_event_handler,
};
```

## 메시지 플로우 분석

### 명령 플로우:
1. 드라이버가 `nrc_wim_alloc_skb()`를 사용하여 WIM 메시지 생성
2. `nrc_wim_skb_add_tlv()`를 사용하여 TLV 시스템을 통해 매개변수 추가
3. `nrc_xmit_wim_request()`를 통해 HIF 레이어로 메시지 큐잉
4. 펌웨어가 명령을 처리하고 응답 생성
5. `nrc_wim_response_handler()`를 통해 응답 수신
6. 완료 메커니즘을 통해 대기 중인 스레드 깨움

### 이벤트 플로우:
1. 펌웨어가 비동기 이벤트 생성
2. `nrc_wim_event_handler()`를 통해 이벤트 수신
3. 이벤트 유형이 특정 처리 로직 결정
4. 이벤트 유형에 따라 드라이버 상태 업데이트
5. 필요한 경우 MAC80211 레이어에 알림

## 성능 고려사항

### 메모리 관리:
- 재할당을 피하기 위한 SKB 공간 사전 할당
- 메시지 크기를 최소화하기 위한 효율적인 TLV 패킹
- 누수를 방지하기 위한 적절한 SKB 생명주기 관리

### 동시성:
- 크레딧 관리를 위한 락 프리 원자적 연산
- 뮤텍스로 보호되는 코드에서 최소한의 크리티컬 섹션
- 완료 기반의 효율적인 동기화

### 확장성:
- 인덱싱을 통한 다중 VIF 지원
- QoS 지원을 위한 AC별 크레딧 추적
- 향후 개선을 위한 확장 가능한 TLV 시스템

## 결론

WIM 프로토콜 구현은 다음을 통해 정교한 펌웨어-드라이버 통신을 보여줍니다:

1. **견고한 동기화** 메커니즘으로 요청/응답 매칭
2. **유연한 TLV 시스템**으로 확장 가능한 매개변수 전달 지원
3. **포괄적인 이벤트 처리**로 비동기 펌웨어 알림 지원
4. **효율적인 플로우 제어**로 크레딧 기반 전송 관리
5. **스레드 안전 설계**로 동시 작업 지원

이 구현은 프로덕션 무선 네트워킹에 필요한 성능과 신뢰성을 제공하면서 복잡한 펌웨어 상호작용을 깨끗하고 유지보수 가능한 인터페이스 뒤에 성공적으로 추상화합니다.