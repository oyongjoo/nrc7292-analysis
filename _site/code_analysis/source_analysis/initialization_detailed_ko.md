# NRC7292 초기화 시퀀스 상세 소스 코드 분석

## 개요
본 문서는 NRC7292 HaLow 드라이버의 초기화 과정을 실제 소스 코드를 기반으로 상세히 분석합니다. 모듈 로딩부터 mac80211 등록까지의 전체 초기화 플로우를 단계별로 설명합니다.

## 1. 모듈 로딩 분석

### 1.1 nrc_cspi_init() 함수 분석

**위치**: `nrc-hif-cspi.c:2634`

```c
static int __init nrc_cspi_init (void)
{
#ifndef CONFIG_SPI_USE_DT
	struct spi_device *spi;
#endif
	int ret = 0;

#ifndef CONFIG_SPI_USE_DT
	spi = nrc_create_spi_device();
	if (IS_ERR(spi)) {
		pr_err("Failed to nrc_create_spi_dev\n");
		goto out;
	}
	g_spi_dev = spi;
#endif

	ret = spi_register_driver(&nrc_cspi_driver);
	if (ret) {
		pr_info("Failed to register spi driver(%s).",
					nrc_cspi_driver.driver.name);
		goto unregister_device;
	}

	pr_info("Succeed to register spi driver(%s).", nrc_cspi_driver.driver.name);
	return ret;

unregister_device:
#ifndef CONFIG_SPI_USE_DT
	spi_unregister_device(spi);
out:
#endif
	return ret;
}
```

**단계별 분석**:
1. **모듈 진입점**: `module_init(nrc_cspi_init)`로 지정된 커널 모듈 초기화 함수
2. **SPI 디바이스 생성**: Device Tree를 사용하지 않는 경우 수동으로 SPI 디바이스 생성
3. **SPI 드라이버 등록**: `spi_register_driver()`를 통해 커널에 SPI 드라이버 등록
4. **에러 처리**: 등록 실패 시 생성된 디바이스 정리

**커널 API 역할**:
- `spi_register_driver()`: SPI 서브시스템에 드라이버 등록, 매칭되는 디바이스 발견 시 probe 호출

### 1.2 SPI 디바이스 생성 (Device Tree 미사용 시)

**위치**: `nrc-hif-cspi.c:2597`

```c
static struct spi_device *nrc_create_spi_device (void)
{
	struct spi_master *master;
	struct spi_device *spi;

	/* Apply module parameters */
	bi.bus_num = spi_bus_num;
	bi.chip_select = spi_cs_num;
	bi.irq = spi_gpio_irq >= 0 ? gpio_to_irq(spi_gpio_irq) : -1;
	bi.max_speed_hz = hifspeed;

	/* Find the spi master that our device is attached to */
	master = spi_busnum_to_master(spi_bus_num);
	if (!master) {
		pr_err("Could not find spi master with the bus number %d.",
			spi_bus_num);
		return NULL;
	}

	/* Instantiate and add a spi device */
	spi = spi_new_device(master, &bi);
	if (!spi) {
		pr_err("Failed to instantiate a new spi device.");
		return NULL;
	}

	return spi;
}
```

**모듈 파라미터 초기화**:
- `spi_bus_num`: SPI 버스 번호 (기본값: 0)
- `spi_cs_num`: SPI 칩 선택 번호 (기본값: 0)  
- `spi_gpio_irq`: GPIO IRQ 번호 (기본값: 5)
- `hifspeed`: SPI 속도 (기본값: 20MHz)

## 2. 디바이스 프로브 분석

### 2.1 nrc_cspi_probe() 함수 단계별 분석

**위치**: `nrc-hif-cspi.c:2462`

```c
static int nrc_cspi_probe(struct spi_device *spi)
{
	struct nrc *nw;
	struct nrc_hif_device *hdev;
	struct nrc_spi_priv *priv;

	int ret = 0;
	int retry = 0;

	nrc_dbg_init(&spi->dev);

	priv = nrc_cspi_alloc(spi);
	if (IS_ERR(priv)) {
		dev_err(&spi->dev, "Failed to nrc_cspi_alloc\n");
		return PTR_ERR(priv);
	}

	ret = nrc_cspi_gpio_alloc(spi);
	if (ret) {
		dev_err(&spi->dev, "Failed to nrc_cspi_gpio_alloc\n");
		goto err_cspi_free;
	}

	hdev = nrc_hif_alloc(&spi->dev, (void *)priv,  &spi_ops);
	if (IS_ERR(hdev)) {
		dev_err(&spi->dev, "Failed to nrc_hif_alloc\n");
		ret = PTR_ERR(hdev);
		goto err_gpio_free;
	}

	priv->hdev = hdev;

try:
	if (fw_name) nrc_hif_reset_device(hdev);
	ret = nrc_hif_probe(hdev);
	if (ret && retry < MAX_RETRY_CNT) {
		retry++;
		goto try;
	}

	if (ret) {
		dev_err(&spi->dev, "Failed to nrc_hif_probe\n");
		goto err_hif_free;
	}

	nw = nrc_nw_alloc(&spi->dev, hdev);
	if (IS_ERR(nw)) {
		dev_err(&spi->dev, "Failed to nrc_nw_alloc\n");
		goto err_hif_free;
	}

	spi_set_drvdata(spi, nw);

	nrc_nw_set_model_conf(nw, priv->hw.sys.chip_id);

	ret = nrc_nw_start(nw);
	if (ret) {
		dev_err(&spi->dev, "Failed to nrc_nw_start (%d)\n", ret);
		goto err_nw_free;
	}

	return 0;

err_nw_free:
	nrc_nw_free(nw);
err_hif_free:
	nrc_hif_free(hdev);
err_gpio_free:
	nrc_cspi_gpio_free(spi);
err_cspi_free:
	nrc_cspi_free(priv);

	spi_set_drvdata(spi, NULL);
	return ret;
}
```

**초기화 단계**:

#### 2.1.1 디버그 초기화
```c
nrc_dbg_init(&spi->dev);
```
- 디버그 시스템 초기화
- SPI 디바이스와 연결된 디버그 인터페이스 설정

#### 2.1.2 CSPI 프라이빗 구조체 할당
```c
priv = nrc_cspi_alloc(spi);
```

**nrc_cspi_alloc() 함수**:
```c
static struct nrc_spi_priv *nrc_cspi_alloc (struct spi_device *dev)
{
	struct nrc_spi_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		return NULL;
	}

	dev->dev.platform_data = priv;
	priv->spi = dev;
	priv->ops = &cspi_ops;

	init_waitqueue_head(&priv->wait);
	spin_lock_init(&priv->lock);
	mutex_init(&priv->bus_lock_mutex);

#if !defined(CONFIG_SUPPORT_THREADED_IRQ)
	priv->irq_wq = create_singlethread_workqueue("nrc_cspi_irq");
	INIT_WORK(&priv->irq_work, irq_worker);
#endif
	INIT_DELAYED_WORK(&priv->work, spi_poll_status);

	priv->polling_interval = spi_polling_interval;

	return priv;
}
```

**동기화 객체 초기화**:
- `init_waitqueue_head()`: 대기 큐 초기화 (블로킹 I/O용)
- `spin_lock_init()`: 스핀락 초기화 (인터럽트 컨텍스트용)
- `mutex_init()`: 뮤텍스 초기화 (슬립 가능한 컨텍스트용)

#### 2.1.3 GPIO 할당
```c
ret = nrc_cspi_gpio_alloc(spi);
```
- SPI 통신용 GPIO 핀 할당
- IRQ GPIO, Reset GPIO 등 설정

#### 2.1.4 HIF 디바이스 할당
```c
hdev = nrc_hif_alloc(&spi->dev, (void *)priv, &spi_ops);
```
- Hardware Interface 추상화 계층 생성
- SPI 작업 큐 및 콜백 설정

#### 2.1.5 하드웨어 프로브 (재시도 로직)
```c
try:
	if (fw_name) nrc_hif_reset_device(hdev);
	ret = nrc_hif_probe(hdev);
	if (ret && retry < MAX_RETRY_CNT) {
		retry++;
		goto try;
	}
```

**재시도가 필요한 이유**:
- 하드웨어 초기화 타이밍 이슈
- SPI 통신 안정화 대기
- 칩 리셋 후 준비 시간 필요

### 2.2 하드웨어 검증 과정

#### 2.2.1 칩 ID 확인
하드웨어 프로브 과정에서 칩 ID를 읽어 지원 하드웨어인지 확인:

```c
nrc_nw_set_model_conf(nw, priv->hw.sys.chip_id);
```

**지원 칩 ID** (`nrc-init.c:597`):
```c
int nrc_nw_set_model_conf(struct nrc *nw, u16 chip_id)
{
	nw->chip_id = chip_id;
	dev_info(nw->dev, "Configuration of H/W Dependent Setting : %04x\n", nw->chip_id);

	switch (nw->chip_id) {
		case 0x7292:
			nw->hw_queues = 6;
			nw->wowlan_pattern_num = 1;
			break;
		case 0x7393:
		case 0x7394:
			nw->hw_queues = 11;
			nw->wowlan_pattern_num = 2;
			break;
		default:
			dev_err(nw->dev, "Unknown Newracom IEEE80211 chipset %04x", nw->chip_id);
			BUG();
	}

	dev_info(nw->dev, "- HW_QUEUES: %d\n", nw->hw_queues);
	dev_info(nw->dev, "- WoWLAN Pattern num: %d\n", nw->wowlan_pattern_num);

	return 0;
}
```

## 3. 네트워크 컨텍스트 생성

### 3.1 nrc_nw_alloc() 함수 분석

**위치**: `nrc-init.c:766`

```c
struct nrc *nrc_nw_alloc(struct device *dev, struct nrc_hif_device *hdev)
{
	struct ieee80211_hw *hw;
	struct nrc *nw;

	hw = nrc_mac_alloc_hw(sizeof(struct nrc), NRC_DRIVER_NAME);

	if (!hw) {
			return NULL;
	}

	nw = hw->priv;
	nw->hw = hw;
	nw->dev = dev;
	nw->hif = hdev;
	hdev->nw = nw;

	nw->loopback = loopback;
	nw->lb_count = lb_count;
	nw->drv_state = NRC_DRV_INIT;

	nw->vendor_skb_beacon = NULL;
	nw->vendor_skb_probe_req = NULL;
	nw->vendor_skb_probe_rsp = NULL;
	nw->vendor_skb_assoc_req = NULL;

	nrc_stats_init();
	nw->fw_priv = nrc_fw_init(nw);
	if (!nw->fw_priv) {
		dev_err(nw->dev, "Failed to initialize FW");
		goto err_hw_free;
	}

	mutex_init(&nw->target_mtx);
	mutex_init(&nw->state_mtx);

	spin_lock_init(&nw->vif_lock);

	init_completion(&nw->hif_tx_stopped);
	init_completion(&nw->hif_rx_stopped);
	init_completion(&nw->hif_irq_stopped);
	init_completion(&nw->wim_responded);

	nw->workqueue = create_singlethread_workqueue("nrc_wq");
	nw->ps_wq = create_singlethread_workqueue("nrc_ps_wq");

	INIT_DELAYED_WORK(&nw->roc_finish, nrc_mac_roc_finish);
	INIT_DELAYED_WORK(&nw->rm_vendor_ie_wowlan_pattern, nrc_rm_vendor_ie_wowlan_pattern);

#ifdef CONFIG_USE_TXQ
#ifdef CONFIG_NEW_TASKLET_API
	tasklet_setup(&nw->tx_tasklet, nrc_tx_tasklet);
#else
	tasklet_init(&nw->tx_tasklet, nrc_tx_tasklet, (unsigned long) nw);
#endif
#endif

	if (!disable_cqm) {
		nrc_mac_dbg("CQM is enabled");
		nw->beacon_timeout = 0;
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
		setup_timer(&nw->bcn_mon_timer,
					nrc_bcn_mon_timer, (unsigned long)nw);
#else
		timer_setup(&nw->bcn_mon_timer, nrc_bcn_mon_timer, 0);
#endif
	}

	return nw;

err_hw_free:
	nrc_mac_free_hw(hw);

	return NULL;
}
```

### 3.2 주요 구성 요소 초기화

#### 3.2.1 mac80211 하드웨어 할당
```c
hw = nrc_mac_alloc_hw(sizeof(struct nrc), NRC_DRIVER_NAME);
```
- `ieee80211_alloc_hw()`를 내부적으로 호출
- mac80211 프레임워크와 연결
- 프라이빗 데이터 영역 할당

#### 3.2.2 동기화 객체 초기화
```c
mutex_init(&nw->target_mtx);    // 타겟 디바이스 접근 보호
mutex_init(&nw->state_mtx);     // 드라이버 상태 보호
spin_lock_init(&nw->vif_lock);  // VIF 리스트 보호

init_completion(&nw->hif_tx_stopped);   // TX 정지 신호
init_completion(&nw->hif_rx_stopped);   // RX 정지 신호
init_completion(&nw->hif_irq_stopped);  // IRQ 정지 신호
init_completion(&nw->wim_responded);    // WIM 응답 신호
```

**completion 객체의 역할**:
- 비동기 작업의 완료를 대기
- `wait_for_completion()`: 완료 대기
- `complete()`: 완료 신호 전송

#### 3.2.3 워크 큐 생성
```c
nw->workqueue = create_singlethread_workqueue("nrc_wq");
nw->ps_wq = create_singlethread_workqueue("nrc_ps_wq");
```

**워크 큐 용도**:
- `nrc_wq`: 일반적인 비동기 작업
- `nrc_ps_wq`: 전력 관리 전용 작업

#### 3.2.4 지연 작업 초기화
```c
INIT_DELAYED_WORK(&nw->roc_finish, nrc_mac_roc_finish);
INIT_DELAYED_WORK(&nw->rm_vendor_ie_wowlan_pattern, nrc_rm_vendor_ie_wowlan_pattern);
```

**지연 작업 용도**:
- `roc_finish`: Remain on Channel 타임아웃
- `rm_vendor_ie_wowlan_pattern`: WoWLAN 패턴 제거

#### 3.2.5 태스크릿 초기화 (TX 처리용)
```c
#ifdef CONFIG_USE_TXQ
tasklet_setup(&nw->tx_tasklet, nrc_tx_tasklet);
#endif
```

**태스크릿의 역할**:
- 소프트웨어 인터럽트 컨텍스트에서 실행
- TX 패킷 처리 최적화
- 하드웨어 인터럽트보다 낮은 우선순위

## 4. 펌웨어 로딩 과정

### 4.1 펌웨어 파일 확인

**위치**: `nrc-fw.c:191`

```c
bool nrc_check_fw_file(struct nrc *nw)
{
	int status;

	if (fw_name == NULL)
		goto err_fw;

	if (nw->fw)
		return true;

	status = request_firmware((const struct firmware **)&nw->fw,
			fw_name, nw->dev);

	nrc_dbg(NRC_DBG_HIF, "[%s, %d] Checking firmware... (%s)",
			__func__, __LINE__, fw_name);

	if (status != 0) {
		nrc_dbg(NRC_DBG_HIF, "request_firmware() is failed, status = %d, fw = %p",
				status, nw->fw);
		goto err_fw;
	}

	nrc_dbg(NRC_DBG_HIF, "[%s, %d] OK...(%p, 0x%zx)",
			__func__, __LINE__, nw->fw->data, nw->fw->size);
	return true;

err_fw:
	nw->fw = NULL;
	return false;
}
```

### 4.2 펌웨어 다운로드

**위치**: `nrc-fw.c:156`

```c
void nrc_download_fw(struct nrc *nw)
{
	struct firmware *fw = nw->fw;
	struct nrc_fw_priv *priv = nw->fw_priv;
	struct nrc_hif_device *hdev = nw->hif;

	nrc_dbg(NRC_DBG_HIF, "FW download....%s", fw_name);
	priv->num_chunks = DIV_ROUND_UP(fw->size, FRAG_BYTES);
	priv->csum = true;
	priv->fw = fw;
	priv->fw_data_pos = fw->data;
	priv->remain_bytes = fw->size;
	priv->cur_chunk = 0;
	priv->index = 0;
	priv->index_fb = 0;
	priv->start_addr = FW_START_ADDR;
	priv->ack = true;

	nrc_hif_disable_irq(hdev);

	pr_err("start FW %d", priv->num_chunks);
	do {
		nrc_fw_send_frag(nw, priv);
	} while (nrc_fw_check_next_frag(nw, priv));
	pr_err("end FW");

	priv->fw_requested = false;
}
```

### 4.3 청크 기반 다운로드

#### 4.3.1 펌웨어 프래그먼트 구조
```c
/*
 * Firmware download packet format
 *
 * | eof (4B) | address (4B) | len (4B) | payload (1KB - 16) | checksum (4B) |
 *
 * Last packet with "eof" being 1 may have padding bytes in payload
 * but checksum will not include padding bytes
 */
```

#### 4.3.2 프래그먼트 업데이트
**위치**: `nrc-fw.c:54`

```c
void nrc_fw_update_frag(struct nrc_fw_priv *priv, struct fw_frag *frag)
{
	struct fw_frag_hdr *frag_hdr = &priv->frag_hdr;

	frag_hdr->eof = (priv->cur_chunk == (priv->num_chunks - 1));

	if (priv->cur_chunk == 0)
		frag_hdr->address = priv->start_addr;
	else
		frag_hdr->address += frag_hdr->len;

	frag_hdr->len = min_t(u32, FRAG_BYTES, priv->remain_bytes);
	frag->eof = frag_hdr->eof;
	frag->address = frag_hdr->address;
	frag->len = frag_hdr->len;
	memcpy(frag->payload, priv->fw_data_pos, frag_hdr->len);

	frag->checksum = 0;
	if (priv->csum)
		frag->checksum = checksum(frag->payload, frag_hdr->len);
}
```

#### 4.3.3 체크섬 검증
```c
static unsigned int checksum(unsigned char *buf, int size)
{
	int sum = 0;
	int i;

	for (i = 0; i < size; i++)
		sum += buf[i];
	return sum;
}
```

**체크섬의 중요성**:
- SPI 전송 중 데이터 무결성 확인
- 펌웨어 손상 감지
- 신뢰성 있는 부팅 보장

## 5. 네트워크 시작 과정

### 5.1 nrc_nw_start() 함수 분석

**위치**: `nrc-init.c:653`

```c
int nrc_nw_start(struct nrc *nw)
{
	int ret;
	int i;

	if (nw->drv_state != NRC_DRV_INIT) {
		dev_err(nw->dev, "Invalid NW state (%d)\n", nw->drv_state);
		return -EINVAL;
	}

	if (fw_name && !nrc_check_boot_ready(nw)) {
		dev_err(nw->dev, "Boot not ready\n");
		return -EINVAL;
	}

#if defined(CONFIG_SUPPORT_BD)
	ret = nrc_check_bd(nw);
	if (ret) {
		dev_err(nw->dev, "Failed to nrc_check_bd\n");
		return -EINVAL;
	}
#endif

	ret = nrc_check_fw_file(nw);
	if (ret == true) {
		nrc_download_fw(nw);
		nrc_release_fw(nw);
	}

	for (i = 0; i < MAX_FW_RETRY_CNT; i++) {
		if(nrc_check_fw_ready(nw)) {
			goto ready;
		}
		mdelay(100);
	}
	dev_err(nw->dev, "Failed to nrc_check_fw_ready\n");
	return -ETIMEDOUT;

ready:
	nw->drv_state = NRC_DRV_START;
	ret = nrc_hif_start(nw->hif);
	if (ret) {
		dev_err(nw->dev, "Failed to nrc_hif_start\n");
		goto err_return;
	}

	ret = nrc_fw_start(nw);
	if (ret) {
		dev_err(nw->dev, "Failed to nrc_fw_start\n");
		goto err_return;
	}

	ret = nrc_netlink_init(nw);
	if (ret) {
		dev_err(nw->dev, "Failed to nrc_netlink_init\n");
		goto err_return;
	}

	ret = nrc_register_hw(nw);
	if (ret) {
		dev_err(nw->dev, "Failed to nrc_register_hw\n");
		goto err_netlink_deinit;
	}

	nrc_init_debugfs(nw);

	return 0;

err_netlink_deinit:
	nrc_netlink_exit();
err_return:
	return ret;
}
```

### 5.2 펌웨어 시작

#### 5.2.1 nrc_fw_start() 함수
**위치**: `nrc-init.c:543`

```c
int nrc_fw_start(struct nrc *nw)
{
	struct sk_buff *skb_req, *skb_resp;
	struct wim_drv_info_param *p;

	skb_req = nrc_wim_alloc_skb(nw, WIM_CMD_START, tlv_len(sizeof(struct wim_drv_info_param)));
	if (!skb_req)
		return -ENOMEM;

	p = nrc_wim_skb_add_tlv(skb_req, WIM_TLV_DRV_INFO,
							sizeof(struct wim_drv_info_param), NULL);
	p->boot_mode = !!fw_name;
	p->cqm_off = disable_cqm;
	p->bitmap_encoding = bitmap_encoding;
	p->reverse_scrambler = reverse_scrambler;
	p->kern_ver = (NRC_TARGET_KERNEL_VERSION>>8)&0x0fff;
	p->ps_pretend_flag = ps_pretend;
	p->vendor_oui = VENDOR_OUI;
	if (nw->chip_id == 0x7292) {
		p->deepsleep_gpio_dir = TARGET_DEEP_SLEEP_GPIO_DIR_7292;
	} else {
		p->deepsleep_gpio_dir = TARGET_DEEP_SLEEP_GPIO_DIR_739X;
	}
	p->deepsleep_gpio_out = TARGET_DEEP_SLEEP_GPIO_OUT;
	p->deepsleep_gpio_pullup = TARGET_DEEP_SLEEP_GPIO_PULLUP;
	if(sw_enc < 0)
		sw_enc = 0;
	p->sw_enc = sw_enc;
	p->supported_ch_width = support_ch_width;
	skb_resp = nrc_xmit_wim_request_wait(nw, skb_req, (WIM_RESP_TIMEOUT * 30));
	if (skb_resp)
		nrc_on_fw_ready(skb_resp, nw);
	else
		return -ETIMEDOUT;
	return 0;
}
```

#### 5.2.2 펌웨어 준비 확인
**위치**: `nrc-init.c:444`

```c
static void nrc_on_fw_ready(struct sk_buff *skb, struct nrc *nw)
{
	struct ieee80211_hw *hw = nw->hw;
	struct wim_ready *ready;
	struct wim *wim = (struct wim *)skb->data;
	int i;

	nrc_dbg(NRC_DBG_HIF, "system ready");
	ready = (struct wim_ready *) (wim + 1);
	
	nw->fwinfo.ready = NRC_FW_ACTIVE;
	nw->fwinfo.version = ready->v.version;
	nw->fwinfo.rx_head_size = ready->v.rx_head_size;
	nw->fwinfo.tx_head_size = ready->v.tx_head_size;
	nw->fwinfo.payload_align = ready->v.payload_align;
	nw->fwinfo.buffer_size = ready->v.buffer_size;
	nw->fwinfo.hw_version = ready->v.hw_version;

	nw->cap.cap_mask = ready->v.cap.cap;
	nw->cap.listen_interval = ready->v.cap.listen_interval;
	nw->cap.bss_max_idle = ready->v.cap.bss_max_idle;
	nw->cap.max_vif = ready->v.cap.max_vif;

	if (has_macaddr_param(nw->mac_addr[0].addr)) {
		nw->has_macaddr[0] = true;
		nw->has_macaddr[1] = true;
		memcpy(nw->mac_addr[1].addr, nw->mac_addr[0].addr, ETH_ALEN);
		nw->mac_addr[1].addr[1]++;
		nw->mac_addr[1].addr[5]++;
	} else {
		nrc_set_macaddr_from_fw(nw, ready);
	}

	dev_kfree_skb(skb);
}
```

## 6. mac80211 등록

### 6.1 하드웨어 능력 설정

mac80211 등록 과정에서는 하드웨어의 능력을 정의하고 커널에 등록합니다:

- **지원 대역**: S1G (Sub-1GHz) 대역
- **채널 설정**: 지역별 규제 도메인에 따른 채널
- **암호화 지원**: WPA2/WPA3, HW/SW 암호화
- **전력 관리**: 파워 세이브 모드
- **집계 기능**: AMPDU 지원

### 6.2 에러 처리 및 정리 순서

초기화 실패 시 역순으로 정리:

1. **debugfs 정리**
2. **netlink 정리**: `nrc_netlink_exit()`
3. **HIF 정지**: `nrc_hif_stop()`
4. **네트워크 컨텍스트 해제**: `nrc_nw_free()`
5. **HIF 디바이스 해제**: `nrc_hif_free()`
6. **GPIO 해제**: `nrc_cspi_gpio_free()`
7. **CSPI 프라이빗 해제**: `nrc_cspi_free()`

## 7. 성공/실패 조건

### 7.1 성공 조건
- SPI 통신 정상 동작
- 지원 하드웨어 칩 ID 확인
- 펌웨어 파일 존재 및 정상 다운로드
- 펌웨어 응답 수신 (30초 타임아웃)
- mac80211 등록 성공

### 7.2 실패 조건
- SPI 마스터 찾기 실패
- GPIO 할당 실패
- 하드웨어 프로브 실패 (3회 재시도 후)
- 펌웨어 파일 로딩 실패
- 펌웨어 응답 타임아웃
- 메모리 할당 실패

## 8. 커널 버전 호환성

드라이버는 다양한 커널 버전을 지원하기 위해 조건부 컴파일을 사용:

```c
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
	setup_timer(&nw->bcn_mon_timer, nrc_bcn_mon_timer, (unsigned long)nw);
#else
	timer_setup(&nw->bcn_mon_timer, nrc_bcn_mon_timer, 0);
#endif
```

**주요 호환성 고려사항**:
- 타이머 API 변경 (4.15+)
- 태스크릿 API 변경 (5.17+)
- SPI 드라이버 remove 함수 반환값 (5.18+)

## 결론

NRC7292 드라이버의 초기화 과정은 커널 모듈 시스템, SPI 서브시스템, mac80211 프레임워크와의 긴밀한 통합을 보여줍니다. 각 단계는 명확한 책임을 가지며, 실패 시 적절한 정리 과정을 통해 시스템 안정성을 보장합니다. 특히 펌웨어 다운로드의 청크 기반 접근법과 체크섬 검증은 임베디드 시스템에서의 신뢰성을 높이는 중요한 요소입니다.