# NRC7292 드라이버 데이터 플로우 및 통신 경로 상세 분석

## 개요

이 문서는 NRC7292 HaLow 드라이버의 완전한 데이터 플로우와 통신 경로를 상세히 분석합니다. 소스 코드 구현에 기반하여 TX/RX 데이터 처리, 관리 프레임 처리, Netlink 통신, WIM 프로토콜, 버퍼 관리, 큐 관리 등을 포괄적으로 다룹니다.

## 1. 완전한 데이터 플로우 경로

### 1.1 TX 데이터 플로우 (송신 경로)

```
사용자 애플리케이션
    ↓ (socket write)
네트워크 스택 (TCP/UDP/IP)
    ↓ (netdev_tx)
mac80211 subsystem
    ↓ (ieee80211_tx)
NRC 드라이버 (nrc_mac_tx)
    ↓ (TX 핸들러 체인)
HIF 레이어 (nrc_xmit_frame)
    ↓ (CSPI 인터페이스)
하드웨어 (NRC7292 칩셋)
    ↓ (무선 전송)
공중 (RF 신호)
```

#### 1.1.1 상세 TX 플로우 (`nrc-trx.c::nrc_mac_tx`)

**1단계: 진입점 및 초기 검증**
```c
void nrc_mac_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
                struct sk_buff *skb)
{
    struct ieee80211_tx_info *txi = IEEE80211_SKB_CB(skb);
    struct nrc_trx_data tx = {
        .nw = hw->priv,
        .vif = txi->control.vif,
        .sta = control->sta,
        .skb = skb,
        .result = 0,
    };
    
    // VIF 유효성 검증
    if (!nrc_is_valid_vif(tx.nw, tx.vif))
        goto txh_out;
```

**2단계: 전력 관리 처리**
```c
// STA 모드에서 전력 절약 상태 확인
if (tx.nw->vif[vif_id]->type == NL80211_IFTYPE_STATION) {
    if (tx.nw->drv_state == NRC_DRV_PS)
        nrc_hif_wake_target(tx.nw->hif);
        
    // MODEM SLEEP 모드 처리
    if (power_save == NRC_PS_MODEMSLEEP) {
        if (tx.nw->ps_modem_enabled) {
            // WIM 명령으로 모뎀 깨우기
            skb1 = nrc_wim_alloc_skb(tx.nw, WIM_CMD_SET, ...);
            p = nrc_wim_skb_add_tlv(skb1, WIM_TLV_PS_ENABLE, ...);
            nrc_xmit_wim_request(tx.nw, skb1);
        }
    }
}
```

**3단계: AMPDU 세션 설정**
```c
// 자동 AMPDU 모드에서 BA 세션 설정
if (ampdu_mode == NRC_AMPDU_AUTO) {
    if (ieee80211_is_data_qos(mh->frame_control) &&
        !is_multicast_ether_addr(mh->addr1) && !is_eapol(tx.skb)) {
        setup_ba_session(tx.nw, tx.vif, tx.skb);
    }
}
```

**4단계: TX 핸들러 체인 실행**
```c
// TX 핸들러들을 순차적으로 실행
for (h = &__tx_h_start; h < &__tx_h_end; h++) {
    if (!(h->vif_types & BIT(tx.vif->type)))
        continue;
        
    res = h->handler(&tx);
    if (res < 0)
        goto txh_out;
}
```

**주요 TX 핸들러들:**
- `tx_h_debug_print`: 디버깅용 프레임 출력
- `tx_h_debug_state`: STA 상태 검증
- `tx_h_wfa_halow_filter`: HaLow 프레임 필터링
- `tx_h_frame_filter`: 일반 프레임 필터링
- `tx_h_put_iv`: IV 헤더 공간 확보 (암호화용)
- `tx_h_put_qos_control`: non-QoS 데이터를 QoS 데이터로 변환

**5단계: 프레임 전송**
```c
if (!atomic_read(&tx.nw->d_deauth.delayed_deauth))
    nrc_xmit_frame(tx.nw, vif_id, (!!tx.sta ? tx.sta->aid : 0), tx.skb);
```

### 1.2 RX 데이터 플로우 (수신 경로)

```
공중 (RF 신호)
    ↓ (무선 수신)
하드웨어 (NRC7292 칩셋)
    ↓ (CSPI 인터페이스)
HIF 레이어 (nrc_hif_receive)
    ↓ (프레임 헤더 파싱)
NRC 드라이버 (nrc_mac_rx)
    ↓ (RX 핸들러 체인)
mac80211 subsystem
    ↓ (ieee80211_rx_irqsafe)
네트워크 스택
    ↓ (소켓 버퍼)
사용자 애플리케이션
```

#### 1.2.1 상세 RX 플로우 (`nrc-trx.c::nrc_mac_rx`)

**1단계: 수신 프레임 검증**
```c
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    struct nrc_trx_data rx = { .nw = nw, .skb = skb, };
    struct ieee80211_hdr *mh;
    __le16 fc;
    
    // 드라이버 상태 확인
    if (!((nw->drv_state == NRC_DRV_RUNNING) ||
          (nw->drv_state == NRC_DRV_PS)) ||
          atomic_read(&nw->d_deauth.delayed_deauth)) {
        dev_kfree_skb(skb);
        return 0;
    }
```

**2단계: RX 상태 정보 설정**
```c
// 프레임 헤더에서 수신 상태 정보 추출
nrc_mac_rx_h_status(nw, skb);

static void nrc_mac_rx_h_status(struct nrc *nw, struct sk_buff *skb)
{
    struct frame_hdr *fh = (void *)skb->data;
    struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    
    memset(status, 0, sizeof(*status));
    status->signal = fh->flags.rx.rssi;
    status->freq = fh->info.rx.frequency / 10;  // S1G 채널용
    status->band = nw->band;
    
    if (fh->flags.rx.error_mic)
        status->flag |= RX_FLAG_MMIC_ERROR;
    if (fh->flags.rx.iv_stripped)
        status->flag |= RX_FLAG_IV_STRIPPED;
}
```

**3단계: 모니터 모드 처리**
```c
if (nw->promisc) {
    ret = nrc_mac_s1g_monitor_rx(nw, skb);
    return ret;
}
```

**4단계: 프레임 헤더 제거 및 인터페이스 순회**
```c
// 프레임 헤더 제거
skb_pull(skb, nw->fwinfo.rx_head_size - sizeof(struct hif));
mh = (void*)skb->data;
fc = mh->frame_control;

// 활성 인터페이스들을 순회하며 RX 핸들러 실행
ieee80211_iterate_interfaces(nw->hw, IEEE80211_IFACE_ITER_ACTIVE,
                             nrc_rx_handler, &rx);
```

**5단계: RX 핸들러 체인 실행**
```c
static void nrc_rx_handler(void *data, u8 *mac, struct ieee80211_vif *vif)
{
    struct nrc_trx_data *rx = data;
    struct nrc_trx_handler *h;
    
    // RX 핸들러들을 순차적으로 실행
    for (h = &__rx_h_start; h < &__rx_h_end; h++) {
        if (!(h->vif_types & BIT(vif->type)))
            continue;
            
        res = h->handler(rx);
        if (res < 0)
            goto rxh_out;
    }
}
```

**주요 RX 핸들러들:**
- `rx_h_vendor`: Vendor specific IE 처리
- `rx_h_decrypt`: 하드웨어 복호화 후처리
- `rx_h_check_sn`: 시퀀스 번호 검증 (BA 세션용)
- `rx_h_ibss_get_bssid_tsf`: IBSS 모드 TSF 처리
- `rx_h_mesh`: 메시 네트워크 RSSI 필터링

**6단계: mac80211로 프레임 전달**
```c
if (!rx.result) {
    ieee80211_rx_irqsafe(nw->hw, rx.skb);
    
    // 동적 전력 절약 타이머 갱신
    if (ieee80211_hw_check(nw->hw, SUPPORTS_DYNAMIC_PS)) {
        if (ieee80211_is_data(fc) && nw->ps_enabled &&
            nw->hw->conf.dynamic_ps_timeout > 0) {
            mod_timer(&nw->dynamic_ps_timer,
                     jiffies + msecs_to_jiffies(nw->hw->conf.dynamic_ps_timeout));
        }
    }
}
```

### 1.3 관리 프레임 처리

#### 1.3.1 Beacon 프레임 처리
```c
if (ieee80211_is_beacon(fc)) {
    if (!disable_cqm && rx->nw->associated_vif) {
        // Beacon 모니터링 타이머 갱신
        mod_timer(&rx->nw->bcn_mon_timer,
                 jiffies + msecs_to_jiffies(rx->nw->beacon_timeout));
    }
}
```

#### 1.3.2 Association 프레임 처리
```c
if ((ieee80211_is_assoc_req(mh->frame_control) ||
     ieee80211_is_reassoc_req(mh->frame_control)) && rx.sta) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
    
    // Listen interval 저장
    i_sta->listen_interval = mgmt->u.assoc_req.listen_interval;
}
```

#### 1.3.3 Authentication 프레임 특수 처리
```c
// AP 모드에서 이미 연결된 STA의 Auth 프레임 처리
if (ieee80211_is_auth(mh->frame_control) && rx.sta && 
    rx.vif && rx.vif->type == NL80211_IFTYPE_AP) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    
    if(i_sta && i_sta->state > IEEE80211_STA_NOTEXIST){
        // Deauth 프레임으로 변환하여 처리
        skb_deauth = ieee80211_deauth_get(nw->hw, mgmt->bssid, 
                                         mgmt->sa, mgmt->bssid, 
                                         WLAN_REASON_DEAUTH_LEAVING, NULL, false);
        ieee80211_rx_irqsafe(nw->hw, skb_deauth);
        dev_kfree_skb(skb);
        return 0;
    }
}
```

### 1.4 제어 프레임 처리

#### 1.4.1 Block ACK 제어
```c
static void setup_ba_session(struct nrc *nw, struct ieee80211_vif *vif, 
                             struct sk_buff *skb)
{
    struct ieee80211_sta *peer_sta = NULL;
    struct nrc_sta *i_sta = NULL;
    struct ieee80211_hdr *qmh = (struct ieee80211_hdr *) skb->data;
    int tid = *ieee80211_get_qos_ctl(qmh) & IEEE80211_QOS_CTL_TID_MASK;
    
    peer_sta = ieee80211_find_sta(vif, qmh->addr1);
    if (!peer_sta) return;
    
    i_sta = to_i_sta(peer_sta);
    
    switch (i_sta->tx_ba_session[tid]) {
        case IEEE80211_BA_NONE:
        case IEEE80211_BA_CLOSE:
            nw->ampdu_supported = true;
            nw->ampdu_reject = false;
            if((ret = ieee80211_start_tx_ba_session(peer_sta, tid, 0)) != 0) {
                // 에러 처리
            }
            break;
    }
}
```

#### 1.4.2 시퀀스 번호 검증
```c
static int rx_h_check_sn(struct nrc_trx_data *rx)
{
    // BA 세션에서 시퀀스 번호 역전 감지 및 처리
    if (ieee80211_sn_less(sn, i_sta->rx_ba_session[tid].sn) &&
        sn_diff <= IEEE80211_SN_MODULO - i_sta->rx_ba_session[tid].buf_size) {
        ieee80211_mark_rx_ba_filtered_frames(rx->sta, tid, sn, 0, 
                                            IEEE80211_SN_MODULO >> 1);
    }
    i_sta->rx_ba_session[tid].sn = sn;
}
```

## 2. 통신 경로 분석

### 2.1 커널 드라이버와 CLI 애플리케이션 간 Netlink 통신

#### 2.1.1 Netlink 아키텍처
```
CLI 애플리케이션 (사용자 공간)
    ↓ Netlink 소켓
커널 Netlink 서브시스템
    ↓ Generic Netlink
NRC 드라이버 Netlink 핸들러
    ↓ 명령 처리
드라이버 내부 함수들
```

#### 2.1.2 CLI 애플리케이션 Netlink 구현
```c
// cli_netlink.c
int netlink_send_data(char cmd_type, char* param, char* response)
{
    int nl_fd;
    struct sockaddr_nl nl_address;
    int nl_family_id = 0;
    nl_msg_t nl_request_msg, nl_response_msg;
    
    // Netlink 소켓 생성
    nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    
    // 요청 메시지 구성
    nl_request_msg.n.nlmsg_type = nl_family_id;
    nl_request_msg.g.cmd = cmd_type;
    
    // 데이터 전송
    nl_rxtx_length = sendto(nl_fd, (char *)&nl_request_msg, 
                           nl_request_msg.n.nlmsg_len, 0,
                           (struct sockaddr *)&nl_address, 
                           sizeof(nl_address));
    
    // 응답 수신
    nl_rxtx_length = recv(nl_fd, &nl_response_msg, sizeof(nl_response_msg), 0);
}
```

#### 2.1.3 드라이버 Netlink 핸들러
```c
// nrc-netlink.c
static int nrc_netlink_shell(struct sk_buff *skb, struct genl_info *info)
{
    struct nrc *nw = nrc_wdev_to_nw(info->user_ptr[0]);
    int ret = 0;
    
    if (info->attrs[NL_SHELL_RUN_CMD]) {
        char *cmd = nla_data(info->attrs[NL_SHELL_RUN_CMD]);
        ret = nrc_netlink_shell_run(nw, info, cmd);
    }
    
    return ret;
}

static const struct genl_ops nrc_ops[] = {
    {
        .cmd = NL_SHELL_RUN,
        .flags = 0,
        .policy = nrc_genl_policy,
        .doit = nrc_netlink_shell,
    },
    // 기타 명령들...
};
```

### 2.2 WIM 프로토콜 명령/응답 플로우

#### 2.2.1 WIM 프로토콜 구조
```c
struct wim {
    u16 cmd;        // 명령 코드
    u16 seqno;      // 시퀀스 번호
    u8 payload[0];  // TLV 페이로드
} __packed;

struct wim_tlv {
    u16 t;          // Type
    u16 l;          // Length
    u8 v[0];        // Value
} __packed;
```

#### 2.2.2 WIM 명령 생성 및 전송
```c
// wim.c
struct sk_buff *nrc_wim_alloc_skb(struct nrc *nw, u16 cmd, int size)
{
    struct sk_buff *skb;
    struct wim *wim;
    
    skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
    if (!skb) return NULL;
    
    // HIF 헤더용 공간 확보
    skb_reserve(skb, sizeof(struct hif));
    
    // WIM 헤더 생성
    wim = (struct wim *)skb_put(skb, sizeof(*wim));
    memset(wim, 0, sizeof(*wim));
    wim->cmd = cmd;
    wim->seqno = nw->wim_seqno++;
    
    return skb;
}

void *nrc_wim_skb_add_tlv(struct sk_buff *skb, u16 T, u16 L, void *V)
{
    struct wim_tlv *tlv;
    
    tlv = (struct wim_tlv *)(skb_put(skb, tlv_len(L)));
    tlv->t = T;
    tlv->l = L;
    
    if (V) memcpy(tlv->v, V, L);
    
    return (void *)tlv->v;
}
```

#### 2.2.3 WIM 응답 처리
```c
static int nrc_wim_response_handler(struct nrc *nw, struct sk_buff *skb)
{
    mutex_lock(&nw->target_mtx);
    nw->last_wim_responded = skb;
    mutex_unlock(&nw->target_mtx);
    
    if (completion_done(&nw->wim_responded)) {
        // 대기자가 없으면 SKB 해제
        return 0;
    }
    
    complete(&nw->wim_responded);
    return 1;  // SKB를 나중에 해제
}
```

### 2.3 이벤트 알림 경로

#### 2.3.1 WIM 이벤트 처리
```c
static int nrc_wim_event_handler(struct nrc *nw, struct sk_buff *skb)
{
    struct ieee80211_vif *vif = NULL;
    struct wim *wim = (void *) skb->data;
    struct hif *hif = (void *)(skb->data - sizeof(*hif));
    
    // HIF VIF 인덱스로부터 VIF 찾기
    if (hif->vifindex != -1)
        vif = nw->vif[hif->vifindex];
    
    switch (wim->event) {
        case WIM_EVENT_SCAN_COMPLETED:
            nrc_mac_cancel_hw_scan(nw->hw, vif);
            break;
            
        case WIM_EVENT_READY:
            nrc_wim_handle_fw_ready(nw);
            break;
            
        case WIM_EVENT_CREDIT_REPORT:
            nrc_wim_update_tx_credit(nw, wim);
            break;
            
        case WIM_EVENT_CSA:
            ieee80211_csa_finish(vif);
            break;
            
        case WIM_EVENT_CH_SWITCH:
            ieee80211_chswitch_done(vif, true);
            break;
    }
    
    return 0;
}
```

### 2.4 CSPI 레지스터 접근 패턴

#### 2.4.1 CSPI 명령 구조
```c
// C-SPI 명령 포맷
// [31:24]: start byte (0x50)
// [23:23]: burst (0: single, 1: burst)
// [22:22]: direction (0: read, 1: write)
// [21:21]: fixed (0: incremental, 1: fixed)
// [20:13]: address
// [12:0]: length (for multi-word transfer)

#define C_SPI_READ     0x50000000
#define C_SPI_WRITE    0x50400000
#define C_SPI_BURST    0x00800000
#define C_SPI_FIXED    0x00200000
#define C_SPI_ADDR(x)  (((x) & 0xff) << 13)
#define C_SPI_LEN(x)   ((x) & 0x1fff)
```

#### 2.4.2 상태 레지스터 구조
```c
struct spi_status_reg {
    struct {
        u8 mode;
        u8 enable;
        u8 latched_status;
        u8 status;
    } eirq;
    u8 txq_status[6];    // TX 큐 상태
    u8 rxq_status[6];    // RX 큐 상태
    u32 msg[4];          // 메시지
} __packed;
```

## 3. 버퍼 관리

### 3.1 SKB (Socket Buffer) 라이프사이클

#### 3.1.1 TX 경로 SKB 관리
```c
// 1. mac80211에서 SKB 수신
void nrc_mac_tx(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    // 2. TX 핸들러들에서 SKB 수정
    for (h = &__tx_h_start; h < &__tx_h_end; h++) {
        res = h->handler(&tx);  // SKB 수정 가능
    }
    
    // 3. 하드웨어로 전송
    nrc_xmit_frame(tx.nw, vif_id, aid, tx.skb);
    
    return;
    
txh_out:
    // 4. 에러 시 SKB 해제
    if (tx.skb)
        dev_kfree_skb(tx.skb);
}
```

#### 3.1.2 RX 경로 SKB 관리
```c
// 1. 하드웨어에서 SKB 수신
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    // 2. 상태 정보 설정
    nrc_mac_rx_h_status(nw, skb);
    
    // 3. RX 핸들러들에서 SKB 처리
    ieee80211_iterate_interfaces(nw->hw, IEEE80211_IFACE_ITER_ACTIVE,
                                 nrc_rx_handler, &rx);
    
    // 4. mac80211로 전달 (SKB 소유권 이전)
    if (!rx.result) {
        ieee80211_rx_irqsafe(nw->hw, rx.skb);
    }
    
    return 0;
}
```

#### 3.1.3 WIM SKB 관리
```c
// WIM 명령용 SKB 할당
struct sk_buff *nrc_wim_alloc_skb(struct nrc *nw, u16 cmd, int size)
{
    struct sk_buff *skb;
    
    // HIF + WIM 헤더 크기 포함하여 할당
    skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));
    if (!skb) return NULL;
    
    // HIF 헤더용 headroom 확보
    skb_reserve(skb, sizeof(struct hif));
    
    return skb;
}

// WIM 응답 대기
struct sk_buff *nrc_xmit_wim_request_wait(struct nrc *nw, struct sk_buff *skb, 
                                          int timeout)
{
    struct sk_buff *rsp;
    
    // 요청 전송
    nrc_xmit_wim_request(nw, skb);
    
    // 응답 대기
    if (wait_for_completion_timeout(&nw->wim_responded,
                                   msecs_to_jiffies(timeout)) == 0) {
        return NULL;  // 타임아웃
    }
    
    mutex_lock(&nw->target_mtx);
    rsp = nw->last_wim_responded;
    nw->last_wim_responded = NULL;
    mutex_unlock(&nw->target_mtx);
    
    return rsp;
}
```

### 3.2 DMA 버퍼 관리

NRC7292는 SPI 인터페이스를 사용하므로 직접적인 DMA는 없지만, SPI 전송을 위한 버퍼 관리가 중요합니다.

#### 3.2.1 SPI 전송 버퍼
```c
#define SPI_BUFFER_SIZE (496-20)

struct nrc_spi_priv {
    struct spi_device *spi;
    
    // 슬롯 관리
    struct {
        u16 head;
        u16 tail;
        u16 size;
        u16 count;
    } slot[2];  // TX_SLOT(0), RX_SLOT(1)
    
    // 크레딧 큐 관리
    int hw_queues;
    u8 front[CREDIT_QUEUE_MAX];
    u8 rear[CREDIT_QUEUE_MAX];
    u8 credit_max[CREDIT_QUEUE_MAX];
    
    struct mutex bus_lock_mutex;
};
```

#### 3.2.2 SPI 전송 함수
```c
static inline void spi_set_transfer(struct spi_transfer *xfer,
                                   void *tx, void *rx, int len)
{
    xfer->tx_buf = tx;
    xfer->rx_buf = rx;
    xfer->len = len;
}

static ssize_t c_spi_write_data(struct spi_device *spi, u8 *buf, ssize_t size)
{
    struct spi_transfer xfer[2];
    struct spi_message msg;
    u32 cmd;
    ssize_t ret;
    
    cmd = C_SPI_WRITE | C_SPI_BURST | C_SPI_ADDR(C_SPI_TXQ_WINDOW) | 
          C_SPI_LEN(size);
    cmd = cpu_to_be32(cmd);
    
    // 명령 전송
    spi_set_transfer(&xfer[0], &cmd, NULL, sizeof(cmd));
    // 데이터 전송
    spi_set_transfer(&xfer[1], buf, NULL, size);
    
    spi_message_init(&msg);
    spi_message_add_tail(&xfer[0], &msg);
    spi_message_add_tail(&xfer[1], &msg);
    
    ret = spi_sync(spi, &msg);
    
    return ret < 0 ? ret : size;
}
```

### 3.3 메모리 할당 전략

#### 3.3.1 SKB 할당 우선순위
```c
// 1. 일반 네트워크 트래픽: GFP_ATOMIC (인터럽트 컨텍스트)
skb = dev_alloc_skb(size);

// 2. WIM 명령: GFP_KERNEL (프로세스 컨텍스트 가능)
skb = dev_alloc_skb(size + sizeof(struct hif) + sizeof(struct wim));

// 3. 응급 상황용 복사
skb_copy = skb_copy(skb, GFP_ATOMIC);
```

#### 3.3.2 메모리 해제 패턴
```c
// TX 에러 시
txh_out:
    if (tx.skb)
        dev_kfree_skb(tx.skb);

// RX 에러 시  
if (nw->drv_state != NRC_DRV_RUNNING) {
    dev_kfree_skb(skb);
    return 0;
}

// WIM 응답 후
if (ret != 1)  // 나중에 해제하지 않는 경우
    dev_kfree_skb(skb);
```

## 4. 큐 관리

### 4.1 AC (Access Category) 큐 처리

#### 4.1.1 AC 큐 정의
```c
// 크레딧 정의
#define TCN  (2*1)
#define TCNE (0)
#define CREDIT_AC0    (TCN*2+TCNE)   /* BK (4) */
#define CREDIT_AC1    (TCN*20+TCNE)  /* BE (40) */
#define CREDIT_AC2    (TCN*4+TCNE)   /* VI (8) */
#define CREDIT_AC3    (TCN*4+TCNE)   /* VO(8) */

// 큐 구조: VIF0(AC0~AC3), BCN, CONC, VIF1(AC0~AC3), padding
#define CREDIT_QUEUE_MAX (12)
```

#### 4.1.2 큐 상태 관리
```c
struct nrc {
    spinlock_t txq_lock;
    struct list_head txq;
    
    // 크레딧 관리
    atomic_t tx_credit[IEEE80211_NUM_ACS*3];
    atomic_t tx_pend[IEEE80211_NUM_ACS*3];
};

void nrc_mac_trx_init(struct nrc *nw)
{
    int i;
    
    spin_lock_init(&nw->txq_lock);
    INIT_LIST_HEAD(&nw->txq);
    
    for (i = 0; i < ARRAY_SIZE(nw->tx_credit); i++) {
        atomic_set(&nw->tx_credit[i], 0);
        atomic_set(&nw->tx_pend[i], 0);
    }
}
```

### 4.2 크레딧 기반 플로우 제어 메커니즘

#### 4.2.1 크레딧 업데이트
```c
static int nrc_wim_update_tx_credit(struct nrc *nw, struct wim *wim)
{
    struct wim_credit_report *r = (void *)(wim + 1);
    int ac;
    
    // 모든 AC에 대해 크레딧 업데이트
    for (ac = 0; ac < (IEEE80211_NUM_ACS*3); ac++)
        atomic_set(&nw->tx_credit[ac], r->v.ac[ac]);
    
    // TX 큐 활성화
    nrc_kick_txq(nw);
    
    return 0;
}
```

#### 4.2.2 크레딧 소비
```c
static bool nrc_check_credit(struct nrc *nw, int ac)
{
    return atomic_read(&nw->tx_credit[ac]) > 0;
}

static void nrc_consume_credit(struct nrc *nw, int ac)
{
    atomic_dec(&nw->tx_credit[ac]);
    atomic_inc(&nw->tx_pend[ac]);
}
```

#### 4.2.3 크레딧 정보 읽기
```c
void nrc_hif_cspi_read_credit(struct nrc_hif_device *hdev, int q, 
                              int *p_front, int *p_rear, int *p_credit)
{
    struct nrc_spi_priv *priv = hdev->priv;
    *p_front = priv->front[q];
    *p_rear = priv->rear[q];
    *p_credit = priv->credit_max[q];
}
```

### 4.3 혼잡 제어 및 백프레셔

#### 4.3.1 TXQ 백오프
```c
#ifdef CONFIG_TRX_BACKOFF
struct nrc_spi_priv {
    atomic_t trx_backoff;
};

static void nrc_apply_backoff(struct nrc_spi_priv *priv)
{
    if (atomic_read(&priv->trx_backoff) > 0) {
        msleep(1);  // 백오프 적용
        atomic_dec(&priv->trx_backoff);
    }
}
#endif
```

#### 4.3.2 큐 전체 중지/재시작
```c
// mac80211에 큐 중지 알림
ieee80211_stop_queues(nw->hw);

// 크레딧 복구 후 큐 재시작
ieee80211_wake_queues(nw->hw);

// 특정 AC 큐만 제어
ieee80211_stop_queue(nw->hw, ac);
ieee80211_wake_queue(nw->hw, ac);
```

## 5. 에러 및 예외 경로

### 5.1 TX/RX 플로우의 에러 처리

#### 5.1.1 TX 에러 처리
```c
void nrc_mac_tx(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    // VIF 검증 실패
    if (!nrc_is_valid_vif(tx.nw, tx.vif)) {
        nrc_mac_dbg("[%s] Invalid vif", __func__);
        goto txh_out;
    }
    
    // 드라이버 상태 확인
    if (tx.nw->drv_state != NRC_DRV_RUNNING) {
        if (ieee80211_is_deauth(mh->frame_control)) {
            nrc_ps_dbg("[%s] deauth in wrong state:%d", 
                      __func__, tx.nw->drv_state);
        }
        goto txh_out;
    }
    
    // TX 핸들러 에러
    for (h = &__tx_h_start; h < &__tx_h_end; h++) {
        res = h->handler(&tx);
        if (res < 0) {
            nrc_mac_dbg("TX handler failed: %d", res);
            goto txh_out;
        }
    }
    
    return;
    
txh_out:
    // 에러 시 SKB 해제
    if (tx.skb)
        dev_kfree_skb(tx.skb);
}
```

#### 5.1.2 RX 에러 처리
```c
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    // 드라이버 상태 확인
    if (!((nw->drv_state == NRC_DRV_RUNNING) ||
          (nw->drv_state == NRC_DRV_PS))) {
        nrc_mac_dbg("Target not ready, discarding frame");
        dev_kfree_skb(skb);
        return 0;
    }
    
    // Delayed deauth 상태 확인
    if (atomic_read(&nw->d_deauth.delayed_deauth)) {
        dev_kfree_skb(skb);
        return 0;
    }
    
    // RX 핸들러 에러
    if (rx.result) {
        // 에러가 있으면 SKB는 이미 해제됨
        return 0;
    }
    
    // 정상 처리: mac80211에 전달 (SKB 소유권 이전)
    ieee80211_rx_irqsafe(nw->hw, rx.skb);
    return 0;
}
```

#### 5.1.3 암호화 에러 처리
```c
static int rx_h_decrypt(struct nrc_trx_data *rx)
{
    struct ieee80211_key_conf *key;
    
    // 키가 없는 경우
    if (!key) {
        if (nw->cap.vif_caps[vif_id].cap_mask & WIM_SYSTEM_CAP_HYBRIDSEC) {
            // HYBRID SEC 모드에서는 SW 복호화로 fallback
            return 0;
        }
        nrc_dbg(NRC_DBG_MAC, "key is NULL");
        return 0;
    }
    
    // 지원하지 않는 암호화 방식
    switch (key->cipher) {
        case WLAN_CIPHER_SUITE_CCMP:
            // 지원됨
            break;
        default:
            nrc_dbg(NRC_DBG_MAC, "%s: unknown cipher (%d)", 
                   __func__, key->cipher);
            return 0;
    }
    
    // 성공적으로 복호화됨을 표시
    status = IEEE80211_SKB_RXCB(rx->skb);
    status->flag |= RX_FLAG_DECRYPTED;
    status->flag |= RX_FLAG_MMIC_STRIPPED;
    
    return 0;
}
```

### 5.2 복구 메커니즘

#### 5.2.1 펌웨어 재로드
```c
void nrc_wim_handle_fw_reload(struct nrc *nw)
{
    nrc_ps_dbg("[%s] FW reload requested", __func__);
    
    // 펌웨어 상태를 로딩 중으로 설정
    atomic_set(&nw->fw_state, NRC_FW_LOADING);
    
    // HIF 정리
    nrc_hif_cleanup(nw->hif);
    
    // 펌웨어 재다운로드
    if (nrc_check_fw_file(nw)) {
        nrc_download_fw(nw);
        nw->hif->hif_ops->config(nw->hif);
        msleep(500);
        nrc_release_fw(nw);
    }
}
```

#### 5.2.2 STA 연결 복구 (AP 모드)
```c
static void prepare_deauth_sta(void *data, struct ieee80211_sta *sta)
{
    struct nrc_sta *i_sta = to_i_sta(sta);
    struct ieee80211_hw *hw = i_sta->nw->hw;
    struct ieee80211_vif *vif = data;
    struct sk_buff *skb = NULL;
    
    if (!ieee80211_find_sta(vif, sta->addr))
        return;
    
    // 강제로 Deauth 프레임 생성
    skb = ieee80211_deauth_get(hw, vif->addr, sta->addr, vif->addr,
                              WLAN_REASON_DEAUTH_LEAVING, sta, false);
    if (!skb) {
        nrc_dbg(NRC_DBG_STATE, "%s Fail to alloc skb", __func__);
        return;
    }
    
    nrc_dbg(NRC_DBG_STATE, "(AP Recovery) Disconnect STA(%pM) by force", 
           sta->addr);
    ieee80211_rx_irqsafe(hw, skb);
}
```

#### 5.2.3 Beacon Loss 처리
```c
static void nrc_send_beacon_loss(struct nrc *nw)
{
    if (nw->associated_vif) {
        nrc_dbg(NRC_DBG_STATE, "Sending beacon loss event");
        ieee80211_beacon_loss(nw->associated_vif);
    }
}

// RX에서 beacon loss 감지 시
if (nw->invoke_beacon_loss) {
    nw->invoke_beacon_loss = false;
    nrc_send_beacon_loss(nw);
}
```

### 5.3 타임아웃 처리

#### 5.3.1 WIM 응답 타임아웃
```c
struct sk_buff *nrc_xmit_wim_request_wait(struct nrc *nw, struct sk_buff *skb,
                                          int timeout)
{
    struct sk_buff *rsp;
    
    // 요청 전송
    nrc_xmit_wim_request(nw, skb);
    
    // 응답 대기 (타임아웃 포함)
    if (wait_for_completion_timeout(&nw->wim_responded,
                                   msecs_to_jiffies(timeout)) == 0) {
        nrc_dbg(NRC_DBG_WIM, "WIM request timeout");
        return NULL;
    }
    
    // 응답 SKB 반환
    mutex_lock(&nw->target_mtx);
    rsp = nw->last_wim_responded;
    nw->last_wim_responded = NULL;
    mutex_unlock(&nw->target_mtx);
    
    return rsp;
}
```

#### 5.3.2 Beacon 모니터링 타임아웃
```c
// Beacon 모니터링 타이머 설정
if (!disable_cqm && rx->nw->associated_vif) {
    mod_timer(&rx->nw->bcn_mon_timer,
             jiffies + msecs_to_jiffies(rx->nw->beacon_timeout));
}

// 타임아웃 핸들러에서
static void nrc_beacon_timeout_handler(struct timer_list *t)
{
    struct nrc *nw = from_timer(nw, t, bcn_mon_timer);
    
    nrc_dbg(NRC_DBG_STATE, "Beacon timeout - connection lost");
    ieee80211_connection_loss(nw->associated_vif);
}
```

#### 5.3.3 동적 전력 절약 타임아웃
```c
// 데이터 전송/수신 시 타이머 갱신
if (ieee80211_hw_check(hw, SUPPORTS_DYNAMIC_PS) &&
    hw->conf.dynamic_ps_timeout > 0) {
    mod_timer(&tx.nw->dynamic_ps_timer,
             jiffies + msecs_to_jiffies(hw->conf.dynamic_ps_timeout));
}

// 타임아웃 시 전력 절약 모드 진입
static void nrc_dynamic_ps_timeout_handler(struct timer_list *t)
{
    struct nrc *nw = from_timer(nw, t, dynamic_ps_timer);
    
    nrc_dbg(NRC_DBG_PS, "Entering dynamic PS mode");
    ieee80211_request_smps(nw->associated_vif, IEEE80211_SMPS_DYNAMIC);
}
```

## 6. 성능 최적화 고려사항

### 6.1 인터럽트 처리 최적화
```c
// IRQ 핸들러는 최소한의 작업만 수행
static irqreturn_t nrc_spi_irq_handler(int irq, void *data)
{
    struct nrc_spi_priv *priv = data;
    
    // IRQ 비활성화
    disable_irq_nosync(irq);
    
    // Work queue에 실제 처리 위임
    queue_work(priv->irq_wq, &priv->irq_work);
    
    return IRQ_HANDLED;
}
```

### 6.2 메모리 풀링
```c
// SKB 할당 실패 시 재시도 로직
static struct sk_buff *nrc_alloc_skb_retry(int size, int max_retry)
{
    struct sk_buff *skb;
    int retry = 0;
    
    do {
        skb = dev_alloc_skb(size);
        if (skb) return skb;
        
        if (retry++ < max_retry) {
            msleep(1);  // 잠시 대기 후 재시도
        }
    } while (retry < max_retry);
    
    return NULL;
}
```

### 6.3 배치 처리
```c
// 여러 프레임을 한 번에 처리
static int nrc_process_rx_batch(struct nrc *nw, struct sk_buff_head *skb_list)
{
    struct sk_buff *skb;
    
    while ((skb = skb_dequeue(skb_list)) != NULL) {
        nrc_mac_rx(nw, skb);
    }
    
    return 0;
}
```

## 결론

NRC7292 드라이버는 복잡한 다계층 아키텍처를 통해 HaLow 네트워킹을 구현합니다. 핵심 데이터 플로우는 mac80211 subsystem과 CSPI 하드웨어 인터페이스 사이의 효율적인 중계 역할을 수행하며, WIM 프로토콜을 통한 제어 평면과 Netlink를 통한 사용자 공간 통신을 제공합니다.

특히 주목할 점은:

1. **계층화된 에러 처리**: 각 계층에서 적절한 에러 처리와 복구 메커니즘을 제공
2. **효율적인 버퍼 관리**: SKB 라이프사이클을 통한 Zero-copy 최적화
3. **크레딧 기반 플로우 제어**: 하드웨어 큐 오버플로우 방지
4. **비동기 처리**: IRQ와 Work queue를 통한 논블로킹 아키텍처
5. **전력 관리 통합**: 다양한 전력 절약 모드와 seamless한 통합

이러한 설계는 IEEE 802.11ah HaLow의 저전력, 장거리 통신 요구사항을 효과적으로 지원하면서도 Linux 네트워킹 스택과의 호환성을 보장합니다.