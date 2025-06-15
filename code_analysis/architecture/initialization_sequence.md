# NRC7292 드라이버 초기화 시퀀스 분석

## 전체 초기화 흐름

```
모듈 로딩 → 하드웨어 감지 → 펌웨어 로딩 → 통신 초기화 → mac80211 등록 → 네트워크 준비
```

## 1. 모듈 초기화 단계

### A. 메인 모듈 진입점
```c
// nrc-hif-cspi.c:2634
static int __init nrc_cspi_init(void)
{
    // SPI 드라이버를 커널에 등록
    return spi_register_driver(&nrc_cspi_driver);
}
module_init(nrc_cspi_init);
```

### B. SPI 드라이버 등록
```c
static struct spi_driver nrc_cspi_driver = {
    .driver = {
        .name = "nrc-cspi",
        .of_match_table = nrc_cspi_dt_ids, // Device Tree 지원
    },
    .probe = nrc_cspi_probe,    // 하드웨어 감지 시 호출
    .remove = nrc_cspi_remove,  // 하드웨어 제거 시 호출
};
```

## 2. 하드웨어 프로브 단계

### A. SPI 디바이스 프로브 (nrc_cspi_probe)
```c
// nrc-hif-cspi.c:2462
static int nrc_cspi_probe(struct spi_device *spi)
{
    // 1. CSPI 전용 구조체 할당
    priv = nrc_cspi_alloc(spi);
    
    // 2. GPIO 설정 (인터럽트, 제어 핀)
    ret = nrc_cspi_gpio_alloc(spi);
    
    // 3. HIF 디바이스 생성
    hdev = nrc_hif_alloc(&spi->dev, priv, &spi_ops);
    
    // 4. 하드웨어 감지 및 검증
    ret = nrc_hif_probe(hdev);
    
    // 5. 네트워크 구조체 할당
    nw = nrc_nw_alloc(&spi->dev, hdev);
    
    // 6. 칩별 설정 적용
    nrc_nw_set_model_conf(nw, chip_id);
    
    // 7. 네트워크 스택 시작
    ret = nrc_nw_start(nw);
}
```

### B. 하드웨어 검증 (spi_probe)
```c
// nrc-hif-cspi.c:1704
static int spi_probe(struct nrc_hif_device *dev)
{
    // 1. 칩 ID 읽기 (최대 MAX_PROBE_CNT 시도)
    for (cnt = 0; cnt < MAX_PROBE_CNT; cnt++) {
        chip_id = c_spi_read_reg(spi_priv, C_SPI_EIRQ_MODE);
        if (chip_id == 0x7292 || chip_id == 0x7393 || chip_id == 0x7394)
            break;
    }
    
    // 2. CSPI 인터페이스 설정
    c_spi_config(spi_priv);
    
    // 3. 기본 크레딧 설정
    spi_set_default_credit(spi_priv);
}
```

## 3. 네트워크 구조체 초기화

### A. 주요 구조체 할당 (nrc_nw_alloc)
```c
// nrc-init.c:766
struct nrc *nrc_nw_alloc(struct device *dev, struct nrc_hif_device *hdev)
{
    // 1. mac80211 하드웨어 할당
    hw = nrc_mac_alloc_hw();
    
    // 2. 드라이버 주요 구조체 초기화
    nw = hw->priv;
    nw->hif = hdev;
    nw->dev = dev;
    
    // 3. 펌웨어 관리 초기화
    nrc_fw_init(nw);
    
    // 4. 통계 시스템 초기화
    nrc_stats_init();
    
    // 5. 워크 큐 생성
    nrc_wq = create_singlethread_workqueue("nrc_wq");
    nrc_ps_wq = create_singlethread_workqueue("nrc_ps_wq");
    
    // 6. 동기화 객체 초기화
    mutex_init(&nw->mutex);
    spin_lock_init(&nw->d_lock);
    
    // 7. TX Tasklet 설정
    tasklet_init(&nw->tx_tasklet, nrc_tx_tasklet, (unsigned long)nw);
}
```

### B. mac80211 하드웨어 설정 (nrc_mac_alloc_hw)
```c
// nrc-mac80211.c:4475
static struct ieee80211_hw *nrc_mac_alloc_hw(void)
{
    // 1. ieee80211_hw 구조체 할당
    hw = ieee80211_alloc_hw(sizeof(struct nrc), &nrc_mac80211_ops);
    
    // 2. 하드웨어 기능 설정
    ieee80211_hw_set(hw, SIGNAL_DBM);
    ieee80211_hw_set(hw, SUPPORTS_PS);
    ieee80211_hw_set(hw, AMPDU_AGGREGATION);
    
    // 3. 지원 인터페이스 타입 설정
    hw->wiphy->interface_modes = 
        BIT(NL80211_IFTYPE_STATION) |
        BIT(NL80211_IFTYPE_AP) |
        BIT(NL80211_IFTYPE_MESH_POINT) |
        BIT(NL80211_IFTYPE_MONITOR);
    
    // 4. 큐 및 채널 설정
    hw->queues = 4; // AC_BE, AC_BK, AC_VI, AC_VO
    hw->max_rates = 1;
    hw->max_rate_tries = 11;
}
```

## 4. 네트워크 스택 시작 단계

### A. 네트워크 시작 (nrc_nw_start)
```c
// nrc-init.c:653
int nrc_nw_start(struct nrc *nw)
{
    // 1. 부팅 준비 상태 확인
    if (fw_name && !nrc_check_boot_ready(nw)) {
        nrc_dbg(NRC_DBG_INIT, "Boot not ready\n");
        return -ENODEV;
    }
    
    // 2. 보드 데이터 검증
    #if defined(CONFIG_SUPPORT_BD)
    ret = nrc_check_bd(nw);
    if (ret < 0) return ret;
    #endif
    
    // 3. 펌웨어 다운로드
    ret = nrc_check_fw_file(nw);
    if (ret == true) {
        nrc_download_fw(nw);
        nrc_release_fw(nw);
    }
    
    // 4. 펌웨어 준비 대기
    for (i = 0; i < MAX_FW_RETRY_CNT; i++) {
        if(nrc_check_fw_ready(nw)) {
            goto ready;
        }
        mdelay(100);
    }
    
    ready:
    // 5. HIF 레이어 시작
    ret = nrc_hif_start(nw->hif);
    
    // 6. 펌웨어 통신 시작
    ret = nrc_fw_start(nw);
    
    // 7. Netlink 인터페이스 초기화
    ret = nrc_netlink_init(nw);
    
    // 8. mac80211 등록
    ret = nrc_register_hw(nw);
    
    // 9. 디버그 인터페이스 초기화
    nrc_init_debugfs(nw);
}
```

## 5. 펌웨어 관리 단계

### A. 펌웨어 다운로드 (nrc_download_fw)
```c
// nrc-fw.c
int nrc_download_fw(struct nrc *nw)
{
    // 1. 펌웨어 파일 로드
    ret = request_firmware(&fw, fw_name, nw->dev);
    
    // 2. 펌웨어 검증
    if (!nrc_check_fw_file_header(fw)) {
        return -EINVAL;
    }
    
    // 3. 청크 단위로 다운로드
    for (offset = 0; offset < fw->size; offset += chunk_size) {
        chunk_size = min_t(size_t, MAX_CHUNK_SIZE, fw->size - offset);
        ret = nrc_hif_write_chunks(nw->hif, fw->data + offset, chunk_size);
    }
    
    // 4. 다운로드 완료 신호
    nrc_hif_write_reg(nw->hif, FW_DOWNLOAD_COMPLETE_REG, 1);
}
```

### B. 펌웨어 시작 (nrc_fw_start)
```c
// nrc-init.c:543
static int nrc_fw_start(struct nrc *nw)
{
    // 1. WIM_CMD_START 명령 전송
    ret = nrc_wim_send_cmd(nw, WIM_CMD_START, NULL, 0);
    
    // 2. 드라이버 파라미터 설정
    nrc_wim_set_boot_mode(nw, boot_mode);
    nrc_wim_set_cqm(nw, cqm);
    nrc_wim_set_power_save(nw, power_save);
    
    // 3. 펌웨어 응답 대기
    ret = wait_for_completion_timeout(&nw->fw_ready, 
                                      msecs_to_jiffies(5000));
    
    // 4. MAC 주소 설정
    nrc_set_mac_addresses(nw);
}
```

## 6. HIF 레이어 시작

### A. SPI 인터페이스 시작 (spi_start)
```c
// nrc-hif-cspi.c:1749
static int spi_start(struct nrc_hif_device *dev)
{
    // 1. RX 처리 스레드 생성
    spi_priv->rx_thread = kthread_run(spi_rx_thread, dev, "nrc-spi-rx");
    
    // 2. 인터럽트 요청
    if (spi_gpio_irq >= 0) {
        ret = request_threaded_irq(gpio_to_irq(spi_gpio_irq),
                                   nrc_hif_cspi_irq_handler,
                                   NULL, IRQF_TRIGGER_FALLING,
                                   "nrc-cspi", dev);
    }
    
    // 3. 인터럽트 활성화
    c_spi_enable_irq(spi_priv, C_SPI_EIRQ_ENABLE);
    
    // 4. 상태 업데이트
    spi_priv->hif_ops->update_status(dev, NRC_HIF_OPENED);
}
```

## 7. mac80211 등록

### A. 하드웨어 등록 (nrc_register_hw)
```c
// nrc-mac80211.c:4569
int nrc_register_hw(struct nrc *nw)
{
    // 1. MAC 주소 설정
    nrc_set_mac_addresses(nw);
    
    // 2. 하드웨어 능력 설정
    nrc_set_hw_capabilities(nw);
    
    // 3. 지원 채널 설정
    nrc_setup_channels_rates(nw);
    
    // 4. mac80211에 등록
    ret = ieee80211_register_hw(nw->hw);
    
    // 5. 디버그 정보 출력
    nrc_dbg(NRC_DBG_INIT, "Registered NRC wireless device\n");
}
```

## 8. 주요 초기화 매개변수

### A. 모듈 매개변수
```c
// nrc-init.c
static char* fw_name = NULL;           // 펌웨어 파일 이름
static int hifspeed = 20000000;        // SPI 클럭 속도 (20MHz)
static int spi_gpio_irq = 5;           // SPI 인터럽트 GPIO
static int power_save = 0;             // 절전 모드
static int ampdu_mode = 2;             // AMPDU 집성 모드
static int bss_max_idle = 300;         // BSS 최대 유휴 시간
```

### B. 하드웨어 감지
```c
// 지원 칩 ID들
- 0x7292: NRC7292 (기본)
- 0x7393: NRC7393 
- 0x7394: NRC7394
```

## 9. 초기화 완료 조건

```c
// 드라이버가 완전히 초기화되었을 때의 상태:
1. ✅ 하드웨어 감지 및 검증 완료
2. ✅ 펌웨어 다운로드 및 시작 완료
3. ✅ WIM 프로토콜 통신 활성화
4. ✅ HIF 레이어 RX/TX 준비 완료
5. ✅ mac80211 등록 완료
6. ✅ Netlink 인터페이스 활성화
7. ✅ 디버그 인터페이스 준비
```

이 시점에서 드라이버는 네트워크 트래픽을 처리할 준비가 완료되며, 사용자 공간의 설정 도구(CLI, hostapd, wpa_supplicant 등)와 상호작용할 수 있습니다.

## 에러 처리 및 복구

초기화 과정에서 실패할 경우:
- 각 단계마다 에러 코드 반환
- 실패 시 이전 단계까지의 자원 해제
- 모듈 언로드 시 역순으로 정리
- 재초기화 가능한 설계