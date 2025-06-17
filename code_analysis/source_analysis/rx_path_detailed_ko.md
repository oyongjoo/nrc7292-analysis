# NRC7292 수신(RX) 경로 상세 소스 코드 분석

본 문서는 NRC7292 HaLow 드라이버의 수신(RX) 경로에 대한 포괄적인 소스 코드 분석을 제공합니다. CSPI 인터럽트부터 mac80211으로의 프레임 전달까지 전체 수신 흐름을 다룹니다.

## 목차

1. [개요](#개요)
2. [RX 경로 아키텍처](#rx-경로-아키텍처)
3. [CSPI 인터럽트 처리](#cspi-인터럽트-처리)
4. [RX 스레드 처리](#rx-스레드-처리)
5. [HIF 레이어 처리](#hif-레이어-처리)
6. [RX 핸들러 체인](#rx-핸들러-체인)
7. [mac80211 전달](#mac80211-전달)
8. [특수 프레임 처리](#특수-프레임-처리)
9. [결론](#결론)

## 개요

NRC7292 수신 경로는 하드웨어 인터럽트로 시작하여 Linux mac80211 서브시스템으로의 프레임 전달로 끝나는 복잡한 처리 파이프라인입니다. 주요 구성 요소는:

- **CSPI 인터럽트 핸들러**: 하드웨어 인터럽트 처리
- **RX 스레드**: 비동기 프레임 수신 처리
- **HIF 레이어**: 프레임 타입별 라우팅 
- **RX 핸들러 체인**: VIF별 프레임 처리
- **mac80211 인터페이스**: 커널 네트워킹 스택으로 전달

## RX 경로 아키텍처

```
하드웨어 인터럽트
         ↓
   CSPI IRQ 핸들러
         ↓
    spi_update_status()
         ↓
    wake_up_interruptible()
         ↓
     spi_rx_thread()
         ↓
      spi_rx_skb()
         ↓
   HIF 레이어 (nrc_hif_rx)
         ↓
    타입별 라우팅
         ↓
    nrc_mac_rx() / nrc_wim_rx()
         ↓
   ieee80211_rx_irqsafe()
```

## CSPI 인터럽트 처리

### IRQ 핸들러 함수

CSPI 인터럽트는 `nrc-hif-cspi.c`의 `spi_irq()` 함수에서 처리됩니다:

#### CONFIG_SUPPORT_THREADED_IRQ 활성화 시

```c
static irqreturn_t spi_irq(int irq, void *data)
{
    struct nrc_hif_device *hdev = data;
    struct nrc_spi_priv *priv = hdev->priv;
    struct spi_device *spi = priv->spi;

#ifdef CONFIG_NRC_HIF_PRINT_FLOW_CONTROL
    nrc_dbg(NRC_DBG_HIF, "%s", __func__);
#endif

    spi_update_status(spi);
    wake_up_interruptible(&priv->wait);

    return IRQ_HANDLED;
}
```

**주요 기능:**
- `spi_update_status()` 호출로 하드웨어 상태 업데이트
- `wake_up_interruptible()` 호출로 대기 중인 RX 스레드 깨우기
- IRQ_HANDLED 반환으로 인터럽트 완료 신호

#### CONFIG_SUPPORT_THREADED_IRQ 비활성화 시

```c
static irqreturn_t spi_irq(int irq, void *data)
{
    struct nrc_hif_device *hdev = data;
    struct nrc_spi_priv *priv = hdev->priv;

    queue_work(priv->irq_wq, &priv->irq_work);

    return IRQ_HANDLED;
}

static void irq_worker(struct work_struct *work)
{
    struct nrc_spi_priv *priv = container_of(work,
            struct nrc_spi_priv, irq_work);
    struct spi_device *spi = priv->spi;

    spi_update_status(spi);
    wake_up_interruptible(&priv->wait);
}
```

**주요 기능:**
- 워크큐를 사용한 bottom-half 처리
- 인터럽트 컨텍스트에서 최소한의 작업만 수행
- `irq_worker()`에서 실제 상태 업데이트 처리

### 상태 업데이트 함수

`spi_update_status()` 함수는 하드웨어 상태를 읽고 슬롯 정보를 업데이트합니다:

```c
static int spi_update_status(struct spi_device *spi)
{
    struct nrc *nw = spi_get_drvdata(spi);
    struct nrc_hif_device *hdev = nw->hif;
    struct nrc_spi_priv *priv = hdev->priv;
    struct spi_status_reg *status = &priv->hw.status;

    int ret, ac, i, is_relay, retry_cnt=0;
    u32 rear;
    u16 cleared_sta=0;

    if (cspi_suspend) {
        return 0;
    }

    SYNC_LOCK(hdev);
    ret = c_spi_read_regs(spi, C_SPI_EIRQ_MODE, (void *)status, sizeof(*status));
    SYNC_UNLOCK(hdev);
    
    if (ret < 0) {
        return ret;
    }

    // RX 큐 상태 업데이트
    priv->slot[RX_SLOT].head = status->rxq_status[0] & RXQ_SLOT_COUNT;
    
    // 크레딧 처리 로직
    for (ac = 0; ac < priv->hw_queues; ac++) {
        rear = status->txq_status[ac] & TXQ_SLOT_COUNT;
        priv->rear[ac] = rear;
    }

    return 0;
}
```

**주요 기능:**
- CSPI 레지스터에서 큐 상태 읽기
- RX 슬롯 헤드 포인터 업데이트
- TX 크레딧 상태 업데이트
- 뮤텍스를 통한 동기화

## RX 스레드 처리

### spi_rx_thread 함수

RX 스레드는 `spi_rx_thread()` 함수에서 구현되며, 지속적으로 수신 데이터를 처리합니다:

```c
static int spi_rx_thread(void *data)
{
    struct nrc_hif_device *hdev = data;
    struct nrc_spi_priv *priv = hdev->priv;
    struct spi_device *spi = priv->spi;
    struct sk_buff *skb;
    struct hif *hif;
    struct nrc *nw = hdev->nw;
    int ret;

    while (!kthread_should_stop()) {
        if (nw->loopback) {
            ret = spi_loopback(spi, priv, nw->lb_count);
            if (ret <= 0)
                nrc_dbg(NRC_DBG_HIF, "loopback (%d) error.", ret);
            continue;
        }

        if (!kthread_should_park()) {
            skb = spi_rx_skb(spi, priv);
            if (!skb) continue;

            hif = (void *)skb->data;
            if (hif->type != HIF_TYPE_LOOPBACK && cspi_suspend) {
                dev_kfree_skb(skb);
            } else {
                hdev->hif_ops->receive(hdev, skb);
            }
        } else {
            nrc_dbg(NRC_DBG_HIF, "spi_rx_thread parked.");
            kthread_parkme();
        }
    }
    return 0;
}
```

**주요 기능:**
- 무한 루프로 지속적인 수신 처리
- 루프백 모드 지원
- 스레드 정지/일시정지 조건 확인
- `spi_rx_skb()`를 통한 프레임 수신
- HIF ops의 receive 콜백 호출

### spi_rx_skb 함수

실제 프레임 수신은 `spi_rx_skb()` 함수에서 처리됩니다:

```c
static struct sk_buff *spi_rx_skb(struct spi_device *spi,
                struct nrc_spi_priv *priv)
{
    struct sk_buff *skb;
    struct hif *hif;
    ssize_t size;
    u32 nr_slot;
    int ret;
    u32 second_length = 0;
    struct nrc *nw = spi_get_drvdata(spi);
    struct nrc_hif_device *hdev = nw->hif;
    static const int def_slot = 4;

    // SKB 할당
    skb = dev_alloc_skb(priv->slot[RX_SLOT].size * def_slot);
    if (!skb)
        goto fail;

    // RX 슬롯이 비어있으면 상태 업데이트
    if (c_spi_num_slots(priv, RX_SLOT) == 0)
        spi_update_status(priv->spi);

    // 최소 하나의 RX 슬롯이 준비될 때까지 대기
    ret = wait_event_interruptible(priv->wait,
            ((c_spi_num_slots(priv, RX_SLOT) > 0) ||
             kthread_should_stop() || kthread_should_park()));
    if (ret < 0)
        goto fail;

    if (kthread_should_stop() || kthread_should_park())
        goto fail;

    SYNC_LOCK(hdev);
    
    // 첫 번째 슬롯 읽기 (HIF 헤더 포함)
    priv->slot[RX_SLOT].tail++;
    size = c_spi_read(spi, skb->data, priv->slot[RX_SLOT].size);
    SYNC_UNLOCK(hdev);
    
    if (size < 0) {
        priv->slot[RX_SLOT].tail--;
        goto fail;
    }

    // HIF 헤더 검증
    hif = (void *)skb->data;
    
    if (hif->type >= HIF_TYPE_MAX || hif->len == 0) {
        nrc_dbg(NRC_DBG_HIF, "rxslot:(h=%d,t=%d)",
                priv->slot[RX_SLOT].head, priv->slot[RX_SLOT].tail);
        spi_reset_rx(hdev);
        goto fail;
    }

    // 필요한 추가 슬롯 계산
    nr_slot = DIV_ROUND_UP(sizeof(*hif) + hif->len, priv->slot[RX_SLOT].size);
    nr_slot--;

    if (nr_slot == 0)
        goto out;

    // 추가 슬롯이 준비될 때까지 대기
    ret = wait_event_interruptible(priv->wait,
                (c_spi_num_slots(priv, RX_SLOT) >= nr_slot) ||
                kthread_should_stop() || kthread_should_park());
    if (ret < 0)
        goto fail;

    // 추가 데이터 읽기
    priv->slot[RX_SLOT].tail += nr_slot;
    second_length = hif->len + sizeof(*hif) - priv->slot[RX_SLOT].size;
    
    // 4바이트 정렬
    if (second_length & 0x3) {
        second_length = (second_length + 4) & 0xFFFFFFFC;
    }

    SYNC_LOCK(hdev);
    size = c_spi_read(spi, skb->data + priv->slot[RX_SLOT].size,
            second_length);
    SYNC_UNLOCK(hdev);

    if (size < 0)
        goto fail;

out:
    skb_put(skb, sizeof(*hif) + hif->len);
    return skb;

fail:
    if (skb)
        dev_kfree_skb(skb);
    return NULL;
}
```

**주요 기능:**
- 다중 슬롯 수신 처리
- HIF 헤더 기반 데이터 길이 계산
- 비동기 대기를 통한 효율적인 수신
- 4바이트 정렬 처리
- 에러 처리 및 복구

## HIF 레이어 처리

### hif_receive_skb 함수

수신된 SKB는 `hif.c`의 `hif_receive_skb()` 함수에서 타입별로 라우팅됩니다:

```c
static int hif_receive_skb(struct nrc_hif_device *dev, struct sk_buff *skb)
{
    struct nrc *nw = to_nw(dev);
    struct hif *hif = (void *)skb->data;

    WARN_ON(skb->len != hif->len + sizeof(*hif));

    if (nw->drv_state < NRC_DRV_START) {
        dev_kfree_skb(skb);
        return -EIO;
    }
    
    skb_pull(skb, sizeof(*hif));

    switch (hif->type) {
    case HIF_TYPE_FRAME:
        nrc_mac_rx(nw, skb);
        break;
    case HIF_TYPE_WIM:
        nrc_wim_rx(nw, skb, hif->subtype);
        break;
    case HIF_TYPE_LOG:
        nrc_netlink_rx(nw, skb, hif->subtype);
        break;
    case HIF_TYPE_DUMP:
        nrc_dbg(NRC_DBG_HIF, "HIF_TYPE_DUMP - length: %d", hif->len);
        nrc_dump_store((char *)skb->data, hif->len);
        dev_kfree_skb(skb);
        break;
    case HIF_TYPE_LOOPBACK:
        // 루프백 처리 로직
        dev_kfree_skb(skb);
        break;
    default:
        print_hex_dump(KERN_DEBUG, "hif type err ", DUMP_PREFIX_NONE,
                16, 1, skb->data, skb->len > 32 ? 32 : skb->len, false);
        dev_kfree_skb(skb);
    }
    return 0;
}
```

**주요 기능:**
- HIF 헤더 제거
- 드라이버 상태 확인
- HIF 타입별 라우팅:
  - `HIF_TYPE_FRAME`: 802.11 프레임 → `nrc_mac_rx()`
  - `HIF_TYPE_WIM`: WIM 명령/응답 → `nrc_wim_rx()`
  - `HIF_TYPE_LOG`: 로그 데이터 → `nrc_netlink_rx()`
  - `HIF_TYPE_DUMP`: 덤프 데이터 → 파일 저장

### nrc_hif_rx 함수 (대체 경로)

일부 경우에는 `nrc_hif_rx()` 함수가 직접 호출될 수 있습니다:

```c
int nrc_hif_rx(struct nrc_hif_device *dev, const u8 *data, const u32 len)
{
    struct nrc *nw = to_nw(dev);
    struct sk_buff *skb;
    struct hif *hif = (void *)data;

    WARN_ON(len < sizeof(struct hif));

    if (nw->loopback) {
        // 루프백 모드 처리
        skb = dev_alloc_skb(len);
        if (!skb)
            return 0;

        ptr = (uint8_t *)skb_put(skb, len);
        memcpy(ptr, data, len);
        nrc_hif_debug_send(nw, skb);
        return 0;
    }

    skb = dev_alloc_skb(hif->len);
    memcpy(skb_put(skb, hif->len), data + sizeof(*hif), hif->len);

    switch (hif->type) {
    case HIF_TYPE_FRAME:
        nrc_mac_rx(nw, skb);
        break;
    case HIF_TYPE_WIM:
        nrc_wim_rx(nw, skb, hif->subtype);
        break;
    default:
        dev_kfree_skb(skb);
        BUG();
    }
    return 0;
}
```

## RX 핸들러 체인

### nrc_mac_rx 함수

802.11 프레임은 `nrc-trx.c`의 `nrc_mac_rx()` 함수에서 처리됩니다:

```c
int nrc_mac_rx(struct nrc *nw, struct sk_buff *skb)
{
    struct nrc_trx_data rx = { .nw = nw, .skb = skb, };
    struct ieee80211_hdr *mh;
    __le16 fc;
    int ret = 0;
    u64 now = 0, diff = 0;

    // 드라이버 상태 확인
    if (!((nw->drv_state == NRC_DRV_RUNNING) ||
        (nw->drv_state == NRC_DRV_PS)) ||
        atomic_read(&nw->d_deauth.delayed_deauth)) {
        nrc_mac_dbg("Target not ready, discarding frame)");
        dev_kfree_skb(skb);
        return 0;
    }

    // RX 상태 헤더 처리
    nrc_mac_rx_h_status(nw, skb);

    // 프로미스큐어스 모드 처리
    if (nw->promisc) {
        ret = nrc_mac_s1g_monitor_rx(nw, skb);
        return ret;
    }

    // 프레임 헤더 제거
    skb_pull(skb, nw->fwinfo.rx_head_size - sizeof(struct hif));
    mh = (void*)skb->data;
    fc = mh->frame_control;
    now = ktime_to_us(ktime_get_real());

    // 활성 인터페이스 순회
    ieee80211_iterate_active_interfaces(nw->hw, nrc_rx_handler, &rx);

    // 스캔 모드 처리
    if (nw->scan_mode == NRC_SCAN_MODE_ACTIVE_SCANNING) {
        if (nw->associated_vif != NULL &&
            nw->associated_vif->type == NL80211_IFTYPE_STATION &&
            ieee80211_is_beacon(fc)) {
            dev_kfree_skb(skb);
            return 0;
        }
    }

    diff = ktime_to_us(ktime_get_real()) - now;
    if (diff > NRC_MAC80211_RCU_LOCK_THRESHOLD)
        nrc_mac_dbg("%s, diff=%lu", __func__, (unsigned long)diff);

    if (!rx.result) {
        // 비콘 모니터링 타이머 업데이트
        if (!disable_cqm) {
            if (nw->associated_vif &&
                ieee80211_is_probe_resp(fc) &&
                nw->scan_mode == NRC_SCAN_MODE_IDLE) {
                mod_timer(&nw->bcn_mon_timer,
                    jiffies + msecs_to_jiffies(nw->beacon_timeout));
            }
        }

        // 연결 요청 처리
        if ((ieee80211_is_assoc_req(mh->frame_control) ||
            ieee80211_is_reassoc_req(mh->frame_control)) &&
            rx.sta) {
            struct nrc_sta *i_sta = to_i_sta(rx.sta);
            struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
            i_sta->listen_interval = mgmt->u.assoc_req.listen_interval;
        }

        // ARP 프레임 디버깅
#if NRC_DBG_PRINT_ARP_FRAME
        if (ieee80211_is_data_qos(fc)) {
            uint8_t *ptr;
            uint16_t llc_type;
            uint8_t ccmp_hdr = 0;
            if (ieee80211_has_protected(fc)) {
                ccmp_hdr = 8;
            }
            ptr = (uint8_t *)mh + (ieee80211_hdrlen(fc) + ccmp_hdr + 6);
            llc_type = *((uint16_t *)ptr);
            if (llc_type == htons(ETH_P_ARP)) {
                nrc_ps_dbg("[%s] RX ARP [type:%d stype:%d, protected:%d, len:%d]",
                    __func__, WLAN_FC_GET_TYPE(fc), WLAN_FC_GET_STYPE(fc),
                    ieee80211_has_protected(fc), rx.skb->len);
            }
        }
#endif
        // mac80211으로 프레임 전달
        ieee80211_rx_irqsafe(nw->hw, rx.skb);

        // 전력 관리 처리
        if (ieee80211_hw_check(nw->hw, SUPPORTS_DYNAMIC_PS)) {
            if (ieee80211_is_data(fc) && nw->ps_enabled &&
                nw->hw->conf.dynamic_ps_timeout > 0) {
                mod_timer(&nw->dynamic_ps_timer,
                    jiffies + msecs_to_jiffies(nw->hw->conf.dynamic_ps_timeout));
            }
        }
    }

    return 0;
}
```

**주요 기능:**
- 드라이버 상태 검증
- RX 상태 정보 처리
- VIF별 프레임 처리
- 스캔 모드 고려사항
- 전력 관리 타이머 업데이트
- mac80211으로 안전한 프레임 전달

### nrc_mac_rx_h_status 함수

RX 상태 헤더는 `nrc_mac_rx_h_status()` 함수에서 처리됩니다:

```c
static void nrc_mac_rx_h_status(struct nrc *nw, struct sk_buff *skb)
{
    struct frame_hdr *fh = (void *)skb->data;
    struct ieee80211_rx_status *status;
    struct ieee80211_hdr *mh;
    u8 nss, mcs, gi;
    u32 rate;

    // mac80211 RX 상태 구조체 초기화
    status = IEEE80211_SKB_RXCB(skb);
    memset(status, 0, sizeof(*status));

    // 프레임 헤더에서 상태 정보 추출
    status->freq = ieee80211_channel_to_frequency(fh->info.rx.channel,
                                                  NL80211_BAND_2GHZ);
    status->band = NL80211_BAND_2GHZ;
    status->signal = fh->info.rx.rssi;

    // MCS 정보 설정
    if (fh->info.rx.mcs_index != 0xff) {
        status->encoding = RX_ENC_HT;
        status->rate_idx = fh->info.rx.mcs_index;
        
        if (fh->info.rx.gi)
            status->enc_flags |= RX_ENC_FLAG_SHORT_GI;
    }

    // 대역폭 정보 설정
    switch (fh->info.rx.bw) {
    case 0:
        status->bw = RATE_INFO_BW_20;
        break;
    case 1:
        status->bw = RATE_INFO_BW_40;
        break;
    default:
        status->bw = RATE_INFO_BW_20;
        break;
    }

    // 타임스탬프 설정
    status->mactime = fh->info.rx.timestamp;
    status->flag |= RX_FLAG_MACTIME_START;
}
```

## mac80211 전달

### ieee80211_rx_irqsafe 함수

최종적으로 프레임은 커널의 `ieee80211_rx_irqsafe()` 함수를 통해 mac80211 서브시스템으로 전달됩니다:

```c
// nrc_mac_rx() 함수 내에서
ieee80211_rx_irqsafe(nw->hw, rx.skb);
```

**ieee80211_rx_irqsafe() 함수의 특징:**
- **인터럽트 안전**: 인터럽트 컨텍스트에서 안전하게 호출 가능
- **비동기 처리**: 실제 처리는 softirq 컨텍스트에서 수행
- **큐잉**: 프레임을 내부 큐에 저장 후 나중에 처리
- **RCU 보호**: RCU 읽기 잠금 하에서 안전한 처리

### Radiotap 헤더 생성

mac80211은 모니터 인터페이스를 위해 radiotap 헤더를 자동으로 생성합니다. 이 정보는 앞서 설정한 `ieee80211_rx_status` 구조체에서 추출됩니다.

## 특수 프레임 처리

### WIM 프레임 처리

WIM (Wireless Interface Module) 프레임은 `wim.c`의 `nrc_wim_rx()` 함수에서 처리됩니다:

```c
int nrc_wim_rx(struct nrc *nw, struct sk_buff *skb, u8 subtype)
{
    int ret;

#if defined(CONFIG_NRC_HIF_PRINT_RX_DATA)
    skb_push(skb, sizeof(struct hif));
    nrc_dump_wim(skb);
    skb_pull(skb, sizeof(struct hif));
#endif

    ret = wim_rx_handler[subtype](nw, skb);

    if (ret != 1)
        /* Free the skb later (e.g. on Response WIM handler) */
        dev_kfree_skb(skb);

    return 0;
}
```

**WIM 서브타입별 핸들러:**
- `HIF_WIM_SUB_REQUEST`: 요청 메시지 처리
- `HIF_WIM_SUB_RESPONSE`: 응답 메시지 처리  
- `HIF_WIM_SUB_EVENT`: 이벤트 메시지 처리

### 관리 프레임 특수 처리

`nrc_mac_rx()` 함수에서 일부 관리 프레임은 특별히 처리됩니다:

#### 인증 프레임 처리
```c
// AP 모드에서 이미 연결된 STA의 재인증 처리
if (ieee80211_is_auth(mh->frame_control) && rx.sta && rx.vif && 
    rx.vif->type == NL80211_IFTYPE_AP) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    if(i_sta && i_sta->state > IEEE80211_STA_NOTEXIST){
        struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
        struct sk_buff *skb_deauth;
        
        // 가상의 deauth 프레임 생성
        skb_deauth = ieee80211_deauth_get(nw->hw, mgmt->bssid, mgmt->sa, 
                                         mgmt->bssid, WLAN_REASON_DEAUTH_LEAVING, 
                                         NULL, false);
        if (skb_deauth) {
            ieee80211_rx_irqsafe(nw->hw, skb_deauth);
            dev_kfree_skb(skb);
            return 0;
        }
    }
}
```

#### 연결 요청 프레임 처리
```c
if ((ieee80211_is_assoc_req(mh->frame_control) ||
    ieee80211_is_reassoc_req(mh->frame_control)) && rx.sta) {
    struct nrc_sta *i_sta = to_i_sta(rx.sta);
    struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
    i_sta->listen_interval = mgmt->u.assoc_req.listen_interval;
}
```

### 암호화된 프레임 처리

암호화된 프레임의 복호화는 펌웨어에서 처리되며, 드라이버는 이미 복호화된 프레임을 수신합니다. `rx_h_decrypt()` 함수는 필요 시 추가적인 처리를 수행합니다:

```c
static int rx_h_decrypt(struct nrc_trx_data *rx)
{
    struct ieee80211_hdr *mh = (void *) rx->skb->data;
    struct nrc *nw = rx->nw;
    
    // 보호된 프레임 비트 확인
    if (ieee80211_has_protected(mh->frame_control)) {
        // 암호화 관련 추가 처리
        // (대부분 펌웨어에서 처리됨)
    }
    
    return 0;
}
```

## 에러 처리 및 복구

### RX 리셋 메커니즘

데이터 무결성 문제가 발생하면 `spi_reset_rx()` 함수가 호출됩니다:

```c
void spi_reset_rx(struct nrc_hif_device *hdev)
{
    struct nrc_spi_priv *priv = hdev->priv;
    
    // RX 슬롯 포인터 리셋
    priv->slot[RX_SLOT].head = 0;
    priv->slot[RX_SLOT].tail = 0;
    
    // 하드웨어 상태 재동기화
    spi_update_status(priv->spi);
}
```

### 가비지 데이터 감지

```c
if (c_spi_num_slots(priv, RX_SLOT) > 32) {
    if (cnt1++ < 10) {
        pr_err("!!!!! garbage rx data");
        spi_reset_rx(hdev);
    }
    goto fail;
}
```

## 성능 최적화

### TRX Backoff 메커니즘

AMPDU가 지원되지 않는 경우 트래픽 백오프를 사용합니다:

```c
#ifdef CONFIG_TRX_BACKOFF
if (!nw->ampdu_supported) {
    backoff = atomic_inc_return(&priv->trx_backoff);
    
    if ((backoff % 3) != 0) {
        usleep_range(800, 1000);
    }
}
#endif
```

### 슬롯 관리 최적화

```c
// 효율적인 슬롯 계산
static inline u16 c_spi_num_slots(struct nrc_spi_priv *priv, int dir)
{
    return (priv->slot[dir].head - priv->slot[dir].tail);
}
```

## 결론

NRC7292의 RX 경로는 하드웨어 인터럽트부터 mac80211 전달까지 다음과 같은 특징을 가집니다:

### 주요 장점

1. **비동기 처리**: 인터럽트와 스레드 기반의 효율적인 처리
2. **타입별 라우팅**: HIF 레이어를 통한 깔끔한 프레임 분류
3. **에러 복구**: 강력한 에러 감지 및 복구 메커니즘
4. **성능 최적화**: 백오프 및 슬롯 관리를 통한 성능 튜닝

### 핵심 설계 원칙

1. **계층화**: 각 레이어가 명확한 책임을 가짐
2. **동기화**: 적절한 락킹을 통한 스레드 안전성 보장
3. **확장성**: VIF별, 타입별 처리로 다양한 시나리오 지원
4. **안정성**: 다중 레벨의 에러 처리 및 상태 검증

이러한 설계를 통해 NRC7292 드라이버는 안정적이고 효율적인 HaLow 프레임 수신을 제공합니다.