# NRC7292 HaLow 메시 네트워킹 구현 분석

## 메시 네트워킹 개요

NRC7292 HaLow 드라이버는 IEEE 802.11s 표준 기반의 메시 네트워킹을 완전히 지원하며, Sub-1GHz 대역의 장점을 활용해 IoT 환경에 최적화된 메시 솔루션을 제공합니다.

## 1. 메시 인터페이스 지원

### A. mac80211 통합
```c
// nrc-mac80211.c에서 메시 인터페이스 지원
static const struct ieee80211_iface_limit if_limits_multi[] = {
    {
        .max = 1,
        .types = BIT(NL80211_IFTYPE_STATION) |
                 BIT(NL80211_IFTYPE_AP) |
                 BIT(NL80211_IFTYPE_MESH_POINT),  // 메시 포인트 지원
    },
    {
        .max = 1,
        .types = BIT(NL80211_IFTYPE_AP) |
                 BIT(NL80211_IFTYPE_MESH_POINT),
    },
};

// 지원되는 인터페이스 모드
hw->wiphy->interface_modes = 
    BIT(NL80211_IFTYPE_STATION) |
    BIT(NL80211_IFTYPE_AP) |
    BIT(NL80211_IFTYPE_MESH_POINT) |    // 메시 포인트
    BIT(NL80211_IFTYPE_MONITOR);
```

### B. WIM 프로토콜 메시 지원
```c
// nrc-wim-types.h에 정의된 메시 스테이션 타입
enum WIM_STA_TYPE {
    WIM_STA_TYPE_STA = 0,
    WIM_STA_TYPE_AP,
    WIM_STA_TYPE_MONITOR,
    WIM_STA_TYPE_MESH_POINT,     // 메시 포인트 타입
    WIM_STA_TYPE_MAX
};
```

### C. 메시 인터페이스 설정
```c
// 메시 인터페이스 추가 시 자동 신호 모니터링 활성화
static int nrc_mac_add_interface(struct ieee80211_hw *hw,
                                struct ieee80211_vif *vif)
{
    if (vif->type == NL80211_IFTYPE_MESH_POINT) {
        // 메시 네트워크에서 신호 강도 모니터링 자동 활성화
        set_bit(NRC_VIF_SIGNAL_MONITOR, &i_vif->flags);
    }
}
```

## 2. HaLow 메시 기능 및 IEEE 802.11s 지원

### A. 메시 노드 타입
```c
// 지원되는 메시 노드 구성
1. MP (Mesh Point): 기본 메시 노드
2. MPP (Mesh Portal Point): 인터넷 게이트웨이 노드
3. MAP (Mesh Access Point): 하이브리드 AP + 메시 노드
```

### B. 메시 보안 지원
```bash
# Open 메시 (보안 없음)
mode=5
mesh_fwding=1

# WPA3-SAE 보안 메시
mode=5
sae=1
mesh_fwding=1
ieee80211w=2                # 관리 프레임 보호 (필수)
```

### C. 메시 설정 매개변수
```conf
# 메시 네트워크 기본 설정 (mp_halow_*.conf)
mode=5                           # 메시 모드
beacon_int=100                   # 100ms 비콘 간격
dot11MeshRetryTimeout=1000       # 메시 재시도 타임아웃
dot11MeshHoldingTimeout=400      # 메시 홀딩 타임아웃
dot11MeshMaxRetries=4            # 최대 재시도 횟수
mesh_rssi_threshold=-90          # 피어링 RSSI 임계값
mesh_basic_rates=60 120 240      # 기본 전송률 (6, 12, 24 Mbps)
mesh_max_inactivity=-1           # 비활성 타임아웃 비활성화
```

## 3. 메시 경로 선택 및 라우팅

### A. HWMP (Hybrid Wireless Mesh Protocol)
```bash
# 루트 모드 설정
iw dev wlan0 set mesh_param mesh_hwmp_rootmode 2
iw dev wlan0 set mesh_param mesh_hwmp_root_interval 1000

# MPP 노드용 게이트웨이 알림
iw dev wlan0 set mesh_param mesh_gate_announcements 1

# 피어 링크 타임아웃 (0 = 무한대)
iw dev wlan0 set mesh_param mesh_plink_timeout 0

# 경로 새로 고침 시간
iw dev wlan0 set mesh_param mesh_hwmp_path_refresh_time 1000
```

### B. 수동 피어 관리
```python
# mesh_add_peer.py를 통한 피어 관리
def add_mesh_peer(interface, peer_mac):
    # 자동 피어링 비활성화 시 수동 피어 추가
    cmd = f"wpa_cli -i {interface} mesh_peer_add {peer_mac}"
    subprocess.run(cmd, shell=True)

def monitor_mesh_peers(interface):
    # 피어 연결 상태 모니터링 및 재연결
    while True:
        check_peer_connectivity()
        time.sleep(10)
```

### C. Batman-adv 통합
```bash
# 고급 메시 라우팅을 위한 batman-adv 지원
echo 'batman-adv' >> /etc/modules
modprobe batman-adv

# 커널 메시 포워딩 비활성화
iw dev wlan0 set mesh_param mesh_fwding 0

# batman-adv 인터페이스 추가
batctl if add wlan0
ifconfig bat0 up
```

## 4. Sub-1GHz 메시 최적화

### A. 확장된 통신 거리
```c
// HaLow 메시의 장거리 통신 장점
- 기존 WiFi 메시 대비 10배 확장된 통신 거리
- 건물 침투력 향상으로 실내외 연결성 개선
- 장애물 회피 능력 향상
```

### B. 저전력 메시 동작
```conf
# 배터리 구동 메시 노드용 전력 최적화
power_save=2                     # 절전 모드 활성화
beacon_int=200                   # 긴 비콘 간격 (200ms)
dtim_period=3                    # DTIM 주기 연장
mesh_max_inactivity=300000       # 5분 비활성 타임아웃
```

### C. S1G 채널 설정
```python
# 메시 네트워크용 채널 설정
def setup_mesh_channel():
    """
    메시 네트워크용 최적 채널 설정
    - 1MHz: 최대 거리, 최소 전력 소모
    - 2MHz: 거리와 처리량의 균형
    - 4MHz: 높은 처리량 애플리케이션
    """
    channels = {
        'max_range': {'freq': 9025, 'bw': 1},      # 1MHz 대역폭
        'balanced': {'freq': 9035, 'bw': 2},       # 2MHz 대역폭  
        'high_throughput': {'freq': 9215, 'bw': 4} # 4MHz 대역폭
    }
```

## 5. 메시 프레임 처리

### A. IEEE 802.11s 메시 헤더
```c
// 표준 메시 헤더 처리
struct ieee80211s_hdr {
    u8 flags;                    // 메시 플래그
    u8 ttl;                      // Time To Live
    __le32 seqnum;               // 시퀀스 번호
    u8 eaddr1[ETH_ALEN];         // 확장 주소 1
    u8 eaddr2[ETH_ALEN];         // 확장 주소 2 (선택적)
    u8 eaddr3[ETH_ALEN];         // 확장 주소 3 (선택적)
} __packed;
```

### B. 메시 데이터 포워딩
```c
// 펌웨어 레벨 메시 프레임 포워딩
- 하드웨어/펌웨어 레벨에서 메시 프레임 포워딩 처리
- 경로 선택 및 프레임 중복 제거
- 브로드캐스트/멀티캐스트 플러딩 제어
```

### C. 경로 선택 프로토콜 메시지
```c
// HWMP 프로토콜 메시지 타입
- PREQ (Path Request): 경로 요청
- PREP (Path Reply): 경로 응답
- PERR (Path Error): 경로 오류
- RANN (Root Announcement): 루트 알림
```

## 6. 메시 네트워크 토폴로지

### A. 트리 기반 메시
```bash
# 루트 노드 설정 (인터넷 게이트웨이)
mesh_hwmp_rootmode=2
mesh_gate_announcements=1
mesh_hwmp_root_interval=1000

# 리프 노드 설정
mesh_hwmp_rootmode=0
mesh_gate_announcements=0
```

### B. 완전 연결 메시
```bash
# 모든 노드가 라우팅 가능한 설정
mesh_hwmp_rootmode=0
mesh_fwding=1
no_auto_peer=0                   # 자동 피어링 활성화
```

### C. 하이브리드 네트워크
```python
# 메시 백본 + AP 액세스 포인트
def setup_hybrid_network():
    """
    메시 백본 네트워크와 일반 클라이언트용 AP 동시 운영
    """
    # 메시 인터페이스 (wlan0)
    setup_mesh_interface('wlan0', mesh_id='IoT_Backbone')
    
    # AP 인터페이스 (wlan1) 
    setup_ap_interface('wlan1', ssid='IoT_Access')
    
    # 브리지 연결
    setup_bridge(['wlan0', 'wlan1', 'eth0'])
```

## 7. 메시 설정 및 배포

### A. 자동 메시 설정 스크립트
```python
# mesh.py - 메시 네트워크 자동 설정
class MeshConfigurator:
    def setup_mesh_network(self, config):
        """
        메시 네트워크 자동 설정
        - 채널 및 전력 설정
        - 보안 설정 (WPA3-SAE)
        - 피어 검색 및 연결
        - 라우팅 프로토콜 활성화
        """
        self.configure_radio(config['channel'], config['power'])
        self.setup_security(config['security'])
        self.start_mesh_interface(config['mesh_id'])
        self.configure_routing(config['routing'])
```

### B. 브리지 및 NAT 지원
```bash
# 인터넷 게이트웨이 설정 (MPP 노드)
# 브리지 모드 - 메시와 이더넷 연결
brctl addbr br0
brctl addif br0 wlan0
brctl addif br0 eth0
ifconfig br0 up

# NAT 모드 - 인터넷 공유
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
iptables -A FORWARD -i wlan0 -o eth0 -j ACCEPT
iptables -A FORWARD -i eth0 -o wlan0 -j ACCEPT
echo 1 > /proc/sys/net/ipv4/ip_forward
```

### C. DHCP 통합
```conf
# 메시 네트워크용 DHCP 서버 설정
interface=br0
dhcp-range=192.168.100.10,192.168.100.200,12h
dhcp-option=3,192.168.100.1        # 기본 게이트웨이
dhcp-option=6,8.8.8.8,8.8.4.4      # DNS 서버
```

## 8. 성능 특성 및 확장성

### A. 확장성 기능
```c
// IEEE 802.11s 표준 지원 범위
- 다중 홉 지원: 최대 32홉까지
- 경로 선택: 에어타임 링크 메트릭 사용
- 부하 분산: 다중 경로 지원으로 트래픽 분산
- 자동 복구: 경로 장애 시 자동 우회 경로 설정
```

### B. IoT 최적화
```python
# IoT 애플리케이션용 메시 최적화
class IoTMeshOptimizer:
    def optimize_for_sensors(self):
        """
        센서 네트워크용 최적화
        - 낮은 지연 시간
        - 높은 노드 밀도 지원
        - 안정적인 경로 유지
        - 전력 효율성
        """
        self.set_beacon_interval(100)      # 빠른 네이버 검색
        self.set_path_refresh(30000)       # 경로 새로 고침 30초
        self.enable_power_save(True)       # 절전 모드 활성화
        self.set_retry_limit(2)            # 빠른 실패/복구
```

## 9. IoT 애플리케이션 장점

### A. 기존 WiFi 메시 대비 HaLow 메시 장점

#### 커버리지 영역
```c
// 커버리지 비교
기존 WiFi 메시:  ~100m 노드 간 거리
HaLow 메시:     ~1km 노드 간 거리 (10배 확장)

결과:
- 인프라 요구사항 대폭 감소
- 실외 및 농촌 지역 배포 개선
- 사각지대 현상 감소
```

#### 배터리 수명
```c
// 전력 소모 비교
기존 WiFi 메시:  수일~수주 배터리 수명
HaLow 메시:     수개월~수년 배터리 수명 (10배 향상)

최적화 요소:
- 효율적인 절전 프로토콜
- 낮은 송신 전력
- 간헐적 데이터 전송 최적화
```

#### 침투력
```c
// 신호 침투 능력
기존 WiFi (2.4/5GHz): 벽 2-3개 통과 제한
HaLow (Sub-1GHz):    벽 5-10개 통과 가능

장점:
- 건물 내부 완전 커버리지
- 지하 시설 연결 가능
- 산업 환경 내 안정적 통신
```

#### 디바이스 밀도
```c
// 지원 디바이스 수
기존 WiFi 메시:   ~50-100 디바이스/노드
HaLow 메시:      ~1000-8000 디바이스/노드

적용 분야:
- 대규모 센서 네트워크
- 스마트 시티 인프라
- 산업 IoT 모니터링
```

### B. 실제 배포 시나리오

#### 스마트 팜
```python
# 농업용 메시 네트워크
- 토양 센서: 수분, 온도, pH 모니터링
- 기상 스테이션: 강우량, 바람, 습도
- 관개 제어: 원격 밸브 및 펌프 제어
- 커버리지: 10-50 헥타르 단일 메시 네트워크
```

#### 스마트 시티
```python
# 도시 인프라 모니터링
- 대기질 센서: PM2.5, 오존, NO2
- 교통 모니터링: 차량 카운터, 주차 센서
- 스트리트 라이팅: 지능형 가로등 제어
- 커버리지: 도시 전체 단일 메시 백본
```

#### 산업 자동화
```python
# 공장 자동화 메시 네트워크
- 기계 모니터링: 진동, 온도, 압력 센서
- 자산 추적: RFID 및 위치 태그
- 안전 시스템: 가스 검출, 화재 경보
- 커버리지: 대형 공장 건물 전체
```

## 10. 결론

NRC7292 HaLow 메시 구현은 IEEE 802.11s 표준을 완벽히 준수하면서도 Sub-1GHz 대역의 고유한 장점을 최대한 활용한 IoT 특화 솔루션입니다. 

**핵심 장점:**
- **확장된 커버리지**: 기존 WiFi 대비 10배 향상된 통신 거리
- **향상된 배터리 수명**: 수개월~수년 간 배터리 동작 
- **높은 침투력**: 건물 및 장애물 통과 능력
- **대규모 확장성**: 수천 개 노드 지원
- **표준 호환성**: IEEE 802.11s 완전 준수

이러한 특징들로 인해 NRC7292 HaLow 메시는 기존 WiFi 메시로는 구현이 어려운 대규모 IoT 네트워크 배포에 이상적인 솔루션을 제공합니다.