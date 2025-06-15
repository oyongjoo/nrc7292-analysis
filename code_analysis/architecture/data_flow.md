# NRC7292 드라이버 데이터 흐름 분석

## 전체 데이터 흐름 아키텍처

```
사용자 공간                     커널 공간                        하드웨어
┌─────────────┐              ┌──────────────────────────┐        ┌─────────────┐
│ CLI App     │              │                          │        │             │
│ start.py    │◄────────────►│    nrc-netlink.c/.h     │        │   NRC7292   │
│ stop.py     │              │                          │        │   Chipset   │
└─────────────┘              └──────────┬───────────────┘        │             │
                                        │                        │             │
┌─────────────┐              ┌──────────▼───────────────┐        │             │
│ hostapd/    │              │                          │        │             │
│ wpa_        │◄────────────►│    nrc-mac80211.c/.h    │        │             │
│ supplicant  │              │  (ieee80211_ops 구현)   │        │             │
└─────────────┘              └──────────┬───────────────┘        │             │
                                        │                        │             │
                             ┌──────────▼───────────────┐        │             │
                             │      nrc-trx.c           │        │             │
                             │  (TX/RX 패킷 처리)      │        │             │
                             └──────────┬───────────────┘        │             │
                                        │                        │             │
                             ┌──────────▼───────────────┐        │             │
                             │       wim.c/.h           │        │             │
                             │ (WIM 프로토콜 레이어)    │        │             │
                             └──────────┬───────────────┘        │             │
                                        │                        │             │
                             ┌──────────▼───────────────┐        │             │
                             │   nrc-hif-cspi.c/.h     │◄──────►│             │
                             │  (CSPI 하드웨어 IF)     │        │             │
                             └──────────────────────────┘        └─────────────┘
```

## 송신 데이터 경로 (TX Path)

### 1. 패킷 진입점
```c
// mac80211에서 패킷 전송 요청 (실제 함수명)
static void nrc_mac_tx(struct ieee80211_hw *hw, 
                      struct ieee80211_tx_control *control,
                      struct sk_buff *skb)
{
    // 패킷 검증 및 전처리
    // 큐 선택 및 우선순위 처리
    // HIF 레이어로 전달
}
```

### 2. TX 처리 파이프라인
```
네트워크 스택 패킷
       ↓
nrc_mac_tx() [nrc-trx.c]
       ↓
nrc_xmit_frame() [hif.c]
       ↓ 
패킷 헤더 추가/변환
       ↓
AMPDU 집성 처리 (필요시)
       ↓
nrc_xmit_wim() [wim.c/hif.c]
       ↓
WIM 프로토콜 래핑
       ↓
HIF ops->xmit() [nrc-hif-cspi.c]
       ↓
SPI 전송
       ↓
NRC7292 하드웨어
```

### 3. TX 상세 처리 단계

#### A. TX 진입점 처리 (nrc-trx.c)
```c
// nrc_mac_tx() 함수에서 처리되는 단계들:
1. ieee80211_tx_info 검증
2. 대상 VIF 및 스테이션 확인
3. 암호화 키 설정
4. QoS 및 우선순위 처리
5. TX tasklet 스케줄링 또는 직접 전송
```

#### B. HIF 레이어 처리 (hif.c)
```c
// nrc_xmit_frame() 함수에서 처리되는 단계들:
1. HIF 헤더 추가
2. VIF 인덱스 및 AID 설정
3. 패킷 세그멘테이션 (필요시)
4. AMPDU 집성 판단
5. WIM 레이어로 전달
```

#### C. WIM 프로토콜 처리 (wim.c/hif.c)
```c
// nrc_xmit_wim() 함수에서 처리되는 단계들:
1. WIM 헤더 생성
2. HIF_SUBTYPE 설정 (DATA, MGMT, etc.)
3. 시퀀스 번호 할당
4. TLV 포맷 변환
5. HIF 레이어로 전달
```

#### D. CSPI 하드웨어 인터페이스 (nrc-hif-cspi.c)
```c
// HIF ops->xmit() 구현에서 처리되는 단계들:
1. DMA 버퍼 할당
2. CSPI 프로토콜 헤더 추가
3. SPI 전송 큐잉
4. 인터럽트 기반 전송
5. 전송 완료 확인
```

## 수신 데이터 경로 (RX Path)

### 1. 인터럽트 처리
```c
// CSPI 인터럽트 핸들러 (실제 구현)
static irqreturn_t nrc_hif_cspi_irq_handler(int irq, void *dev_id)
{
    // 인터럽트 소스 확인
    // RX 데이터 유무 체크
    // Tasklet 또는 Work queue 스케줄링
    return IRQ_HANDLED;
}
```

### 2. RX 처리 파이프라인
```
NRC7292 하드웨어
       ↓
인터럽트 발생
       ↓
nrc_hif_cspi_irq_handler() [nrc-hif-cspi.c]
       ↓
HIF ops->read() - SPI 읽기
       ↓
nrc_hif_rx() [hif.c]
       ↓
WIM 프로토콜 파싱
       ↓
nrc_rx_complete() [nrc-trx.c]
       ↓
패킷 검증 및 변환
       ↓
ieee80211_rx_ni() [mac80211]
       ↓
네트워크 스택으로 전달
```

### 3. RX 상세 처리 단계

#### A. HIF 인터럽트 처리 (nrc-hif-cspi.c)
```c
1. GPIO 인터럽트 감지
2. 인터럽트 상태 레지스터 읽기
3. RX FIFO 상태 확인
4. Work queue 스케줄링
5. 인터럽트 클리어
```

#### B. WIM 프로토콜 파싱 (wim.c)
```c
1. WIM 헤더 검증
2. 메시지 타입 확인
3. 페이로드 길이 검증
4. 체크섬 확인
5. 응답/이벤트 분류
```

#### C. TRX 레이어 처리 (nrc-trx.c)
```c
1. HIF 헤더 제거
2. 패킷 리어셈블리 (세그멘테이션된 경우)
3. AMPDU 리오더링
4. 중복 패킷 필터링
5. 통계 업데이트
```

#### D. MAC 레이어 전달 (nrc-mac80211.c)
```c
1. ieee80211_rx_status 구성
2. RSSI/SNR 정보 설정
3. 채널 정보 매핑
4. 암호화 상태 설정
5. mac80211으로 전달
```

## 제어 명령 경로 (Control Path)

### 1. 사용자 공간에서 커널로 (Netlink)
```
CLI App → netlink socket → nrc-netlink.c → 해당 처리 함수
```

### 2. WIM 명령 처리 경로
```
설정 요청
    ↓
nrc_wim_set_param()
    ↓  
WIM_CMD_SET 메시지 생성
    ↓
cspi_write() 전송
    ↓
응답 대기 및 처리
```

## AMPDU 집성 데이터 흐름

### TX AMPDU
```
개별 패킷들 → 집성 버퍼 → Block ACK 설정 → 일괄 전송 → BA 응답 처리
```

### RX AMPDU
```
집성된 패킷 수신 → 리오더링 버퍼 → 순서 재배열 → 개별 패킷 전달 → BA 응답 전송
```

## 전력 관리 데이터 흐름 

### 절전 모드 진입
```
유휴 상태 감지 → PM 결정 → WIM_CMD_SLEEP → 하드웨어 절전 → 인터럽트 대기
```

### 절전 모드 해제  
```
Wake-up 인터럽트 → cspi_resume() → WIM_CMD_WAKE → 정상 동작 복구
```

## 성능 최적화 포인트

1. **Zero-copy 경로**: DMA 직접 전송으로 메모리 복사 최소화
2. **인터럽트 집합**: 다중 패킷을 한 번의 인터럽트로 처리
3. **큐 관리**: 우선순위별 TX 큐 분리 관리
4. **배치 처리**: 작은 패킷들의 배치 전송
5. **캐시 최적화**: 패킷 버퍼의 캐시 정렬

이 데이터 흐름 구조는 NRC7292 HaLow 드라이버의 효율적인 패킷 처리와 저지연 통신을 보장합니다.