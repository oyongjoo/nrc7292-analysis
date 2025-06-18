# NRC7292 Initialization Sequence Detailed Source Code Analysis

## Overview
This document provides a detailed analysis of the NRC7292 HaLow driver initialization process based on actual source code. It covers the complete initialization flow from module loading to mac80211 registration.

## 1. Module Loading Analysis

### 1.1 nrc_cspi_init() Function Analysis

**Location**: `nrc-hif-cspi.c:2634`

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

**Step-by-step Analysis**:
1. **Module Entry Point**: Kernel module initialization function specified by `module_init(nrc_cspi_init)`
2. **SPI Device Creation**: Manually create SPI device when Device Tree is not used
3. **SPI Driver Registration**: Register SPI driver with kernel via `spi_register_driver()`
4. **Error Handling**: Clean up created devices on registration failure

**Kernel API Roles**:
- `spi_register_driver()`: Register driver with SPI subsystem, probe called when matching device found

### 1.2 SPI Device Creation (Non-Device Tree)

**Location**: `nrc-hif-cspi.c:2597`

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

**Module Parameter Initialization**:
- `spi_bus_num`: SPI bus number (default: 0)
- `spi_cs_num`: SPI chip select number (default: 0)
- `spi_gpio_irq`: GPIO IRQ number (default: 5)
- `hifspeed`: SPI speed (default: 20MHz)

## 2. Device Probe Analysis

### 2.1 nrc_cspi_probe() Function Step-by-step Analysis

**Location**: `nrc-hif-cspi.c:2462`

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

**Initialization Steps**:

#### 2.1.1 Debug Initialization
```c
nrc_dbg_init(&spi->dev);
```
- Initialize debug system
- Set up debug interface linked to SPI device

#### 2.1.2 CSPI Private Structure Allocation
```c
priv = nrc_cspi_alloc(spi);
```

**nrc_cspi_alloc() Function**:
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

**Synchronization Object Initialization**:
- `init_waitqueue_head()`: Initialize wait queue (for blocking I/O)
- `spin_lock_init()`: Initialize spinlock (for interrupt context)
- `mutex_init()`: Initialize mutex (for sleepable context)

#### 2.1.3 GPIO Allocation
```c
ret = nrc_cspi_gpio_alloc(spi);
```
- Allocate GPIO pins for SPI communication
- Configure IRQ GPIO, Reset GPIO, etc.

#### 2.1.4 HIF Device Allocation
```c
hdev = nrc_hif_alloc(&spi->dev, (void *)priv, &spi_ops);
```
- Create Hardware Interface abstraction layer
- Set up SPI work queues and callbacks

#### 2.1.5 Hardware Probe (with Retry Logic)
```c
try:
	if (fw_name) nrc_hif_reset_device(hdev);
	ret = nrc_hif_probe(hdev);
	if (ret && retry < MAX_RETRY_CNT) {
		retry++;
		goto try;
	}
```

**Why Retry is Needed**:
- Hardware initialization timing issues
- SPI communication stabilization wait
- Required setup time after chip reset

### 2.2 Hardware Verification Process

#### 2.2.1 Chip ID Verification
During hardware probe, chip ID is read to verify supported hardware:

```c
nrc_nw_set_model_conf(nw, priv->hw.sys.chip_id);
```

**Supported Chip IDs** (`nrc-init.c:597`):
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

## 3. Network Context Creation

### 3.1 nrc_nw_alloc() Function Analysis

**Location**: `nrc-init.c:766`

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

### 3.2 Key Component Initialization

#### 3.2.1 mac80211 Hardware Allocation
```c
hw = nrc_mac_alloc_hw(sizeof(struct nrc), NRC_DRIVER_NAME);
```
- Internally calls `ieee80211_alloc_hw()`
- Connect with mac80211 framework
- Allocate private data area

#### 3.2.2 Synchronization Object Initialization
```c
mutex_init(&nw->target_mtx);    // Protect target device access
mutex_init(&nw->state_mtx);     // Protect driver state
spin_lock_init(&nw->vif_lock);  // Protect VIF list

init_completion(&nw->hif_tx_stopped);   // TX stop signal
init_completion(&nw->hif_rx_stopped);   // RX stop signal
init_completion(&nw->hif_irq_stopped);  // IRQ stop signal
init_completion(&nw->wim_responded);    // WIM response signal
```

**Completion Object Roles**:
- Wait for asynchronous operation completion
- `wait_for_completion()`: Wait for completion
- `complete()`: Send completion signal

#### 3.2.3 Work Queue Creation
```c
nw->workqueue = create_singlethread_workqueue("nrc_wq");
nw->ps_wq = create_singlethread_workqueue("nrc_ps_wq");
```

**Work Queue Usage**:
- `nrc_wq`: General asynchronous tasks
- `nrc_ps_wq`: Power management dedicated tasks

#### 3.2.4 Delayed Work Initialization
```c
INIT_DELAYED_WORK(&nw->roc_finish, nrc_mac_roc_finish);
INIT_DELAYED_WORK(&nw->rm_vendor_ie_wowlan_pattern, nrc_rm_vendor_ie_wowlan_pattern);
```

**Delayed Work Usage**:
- `roc_finish`: Remain on Channel timeout
- `rm_vendor_ie_wowlan_pattern`: WoWLAN pattern removal

#### 3.2.5 Tasklet Initialization (for TX processing)
```c
#ifdef CONFIG_USE_TXQ
tasklet_setup(&nw->tx_tasklet, nrc_tx_tasklet);
#endif
```

**Tasklet Role**:
- Execute in software interrupt context
- Optimize TX packet processing
- Lower priority than hardware interrupts

## 4. Firmware Loading Process

### 4.1 Firmware File Verification

**Location**: `nrc-fw.c:191`

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

### 4.2 Firmware Download

**Location**: `nrc-fw.c:156`

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

### 4.3 Chunk-based Download

#### 4.3.1 Firmware Fragment Structure
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

#### 4.3.2 Fragment Update
**Location**: `nrc-fw.c:54`

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

#### 4.3.3 Checksum Verification
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

**Checksum Importance**:
- Verify data integrity during SPI transmission
- Detect firmware corruption
- Ensure reliable boot

## 5. Network Start Process

### 5.1 nrc_nw_start() Function Analysis

**Location**: `nrc-init.c:653`

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

### 5.2 Firmware Start

#### 5.2.1 nrc_fw_start() Function
**Location**: `nrc-init.c:543`

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

#### 5.2.2 Firmware Ready Confirmation
**Location**: `nrc-init.c:444`

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

## 6. mac80211 Registration

### 6.1 Hardware Capability Configuration

During mac80211 registration, hardware capabilities are defined and registered with the kernel:

- **Supported Bands**: S1G (Sub-1GHz) band
- **Channel Configuration**: Channels according to regional regulatory domain
- **Encryption Support**: WPA2/WPA3, HW/SW encryption
- **Power Management**: Power save modes
- **Aggregation Features**: AMPDU support

### 6.2 Error Handling and Cleanup Order

On initialization failure, cleanup in reverse order:

1. **debugfs cleanup**
2. **netlink cleanup**: `nrc_netlink_exit()`
3. **HIF stop**: `nrc_hif_stop()`
4. **Network context free**: `nrc_nw_free()`
5. **HIF device free**: `nrc_hif_free()`
6. **GPIO free**: `nrc_cspi_gpio_free()`
7. **CSPI private free**: `nrc_cspi_free()`

## 7. Success/Failure Conditions

### 7.1 Success Conditions
- SPI communication working properly
- Supported hardware chip ID verified
- Firmware file exists and downloads successfully
- Firmware response received (30 second timeout)
- mac80211 registration successful

### 7.2 Failure Conditions
- SPI master not found
- GPIO allocation failure
- Hardware probe failure (after 3 retries)
- Firmware file loading failure
- Firmware response timeout
- Memory allocation failure

## 8. Kernel Version Compatibility

The driver supports various kernel versions using conditional compilation:

```c
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
	setup_timer(&nw->bcn_mon_timer, nrc_bcn_mon_timer, (unsigned long)nw);
#else
	timer_setup(&nw->bcn_mon_timer, nrc_bcn_mon_timer, 0);
#endif
```

**Key Compatibility Considerations**:
- Timer API changes (4.15+)
- Tasklet API changes (5.17+)
- SPI driver remove function return value (5.18+)

## Conclusion

The NRC7292 driver initialization process demonstrates tight integration with kernel module system, SPI subsystem, and mac80211 framework. Each step has clear responsibilities and appropriate cleanup procedures on failure to ensure system stability. The chunk-based firmware download approach with checksum verification is particularly important for reliability in embedded systems.