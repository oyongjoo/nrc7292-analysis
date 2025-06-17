# NRC7292 Analysis Work Log

## Project Overview
- **Project**: NRC7292 HaLow Driver Code Analysis
- **Repository**: https://github.com/oyongjoo/nrc7292-analysis
- **Purpose**: Learn and document NRC7292 Linux kernel driver architecture

## Git Configuration
- **User**: Liam Lee <oyongjoo@gmail.com>
- **Repository**: oyongjoo/nrc7292-analysis
- **Branch**: master

## Completed Tasks

### 2025-06-15 - Initial Setup
1. ✅ **Git Repository Setup**
   - Created GitHub repository: https://github.com/oyongjoo/nrc7292-analysis
   - Configured git user: Liam Lee <oyongjoo@gmail.com>
   - Initial commit with code analysis documentation

2. ✅ **Initial Documentation Structure**
   - Created `code_analysis/README.md` - Main documentation overview
   - Created `code_analysis/architecture/overview.md` - Basic architecture overview
   - Created `code_analysis/driver_components/board_data.md` - Board data analysis
   - Created `code_analysis/notes/initial_findings.md` - Initial analysis findings
   - Created `code_analysis/regulatory/country_codes.md` - Regulatory compliance analysis

3. ✅ **Project Structure Analysis**
   - Analyzed NRC7292 software package structure
   - Identified main components: kernel driver, CLI app, EVK scripts
   - Created CLAUDE.md with project guidance

## Current Session Tasks

### Architecture Analysis (Completed)
1. 🔄 **Detailed Architecture Documentation**
   - Analyzing driver source files in `nrc7292_sw_pkg/package/src/nrc/`
   - Mapping component interactions and data flow
   - Documenting layer-by-layer architecture

2. 📋 **Pending Architecture Tasks**
   - [ ] Create detailed component interaction diagrams
   - [ ] Document data flow and communication paths  
   - [ ] Analyze driver initialization sequence
   - [ ] Document power management architecture

## Completed Analysis
1. ✅ Complete detailed architecture analysis
2. ✅ WIM protocol implementation analysis
3. ✅ Power management mechanisms study  
4. ✅ Mesh networking support examination
5. ✅ Testing framework architecture review
6. ✅ Regulatory compliance mechanisms
7. ✅ Source code accuracy verification

## Future Analysis Opportunities
1. Performance optimization techniques
2. Real-world deployment case studies
3. Integration with other IoT protocols
4. Advanced debugging techniques
5. Custom application development guide

## Key Files and Locations
- **Main Driver Source**: `/home/liam/work/nrc7292_sw_pkg/package/src/nrc/`
- **CLI Application**: `/home/liam/work/nrc7292_sw_pkg/package/src/cli_app/`
- **Documentation**: `/home/liam/work/code_analysis/`
- **Project Instructions**: `/home/liam/work/CLAUDE.md`

## Build Commands Reference
```bash
# Kernel Driver (Code style check in WSL2)
cd package/src/nrc && make clean
perl ./checkpatch.pl --file --terse --ignore=LINUX_VERSION_CODE --no-tree *.c *.h

# CLI Application  
cd package/src/cli_app && make clean && make

# Testing
cd package/src/nrc/test/block_ack && python testsuit.py
cd package/src/nrc/test/netlink && python3 -c "from nrcnetlink import NrcNetlink; print('OK')"
```

## Important Notes
- Focus on learning NRC7292 HaLow driver architecture
- Maintain detailed documentation for future reference
- Prefer editing existing files over creating new ones

## Git Commit Rules
- **Author**: Always use "Liam Lee <oyongjoo@gmail.com>" as the commit author
- **Co-Author**: Always include "Co-Authored-By: Liam Lee <oyongjoo@gmail.com>" in commit message
- **Claude Attribution**: Do NOT include "Generated with Claude Code" or similar Claude references
- **Commit Message Format**: Clear, descriptive commit messages without AI attribution

## Documentation Guidelines
- **Bilingual Documentation**: Create both English and Korean versions for all documents
  - English version: `filename.md` (original format)
  - Korean version: `filename_ko.md` (Korean translation)
- **Default Language**: Show Korean version (`_ko.md`) to user by default
- **Auto-generation**: Automatically create both versions when generating new documentation
- **Existing Documents**: Update all existing documentation to follow this bilingual approach

## Session Context
- Completed comprehensive analysis of NRC7292 HaLow driver with 7 major documentation areas
- All documentation verified to be based on actual source code implementations
- User emphasized importance of source code accuracy for future reference and understanding
- Created comprehensive analysis covering architecture, protocols, networking, regulatory, and testing
- Set up build environment in WSL2: make, build-essential, checkpatch.pl for code style validation
- Successfully built CLI application and verified test framework functionality
- Ready for advanced topics, implementation guidance, or practical development work

## Technical Q&A Sessions

### Session 1: TX Data Handling and Architecture Analysis

#### Q1: TX Data Workqueue and Threading
**Question**: TX data는 workqueue나 thread를 돌려서 차례대로 target으로 보내는게 아닌거야?
**Answer**: NRC7292는 TX tasklet 기반의 처리 방식을 사용합니다. workqueue/thread가 아닌 tasklet을 통한 bottom-half 처리로 interrupt context에서 빠른 실행이 가능합니다. TX tasklet은 round-robin 방식으로 여러 TXQ를 순차 처리하며, credit이 부족할 때 다음 TXQ로 이동하여 공정성을 보장합니다.

#### Q2: Data Loss Prevention Mechanism
**Question**: 어떤 방식으로 iperf 같은 동작에서 데이터를 손실없이 보낼수 있는거야?
**Answer**: Credit-based flow control 시스템을 사용합니다. 하드웨어 버퍼 상태를 credit으로 관리하여 오버플로우를 방지하고, per-AC credit 할당으로 QoS를 보장합니다. 또한 AMPDU Block ACK 세션을 통해 자동 재전송 처리를 하고, firmware에서 credit report를 통해 실시간 버퍼 상태를 업데이트합니다.

#### Q3: Beacon Loss Function Scope
**Question**: nrc_send_beacon_loss 이 함수는 ap 모드에서 실행되는건가?
**Answer**: 아닙니다. STA 모드에서 AP로부터 beacon을 수신하지 못할 때 실행되는 함수입니다. STA가 AP와의 연결을 모니터링하고 beacon loss를 감지하면 해당 함수가 호출됩니다.

#### Q4: Power Save Mode Decision Criteria
**Question**: tx data가 없는 경우에 대한 power save mode는 어떤 기준에 따라서 power save 를 enable/disable을 결정하는거야?
**Answer**: NRC7292는 여러 power save 모드(NRC_PS_MODEMSLEEP, NRC_PS_DEEPSLEEP_TIM)를 지원하며, TX queue 상태, driver state, 그리고 설정된 power_save 파라미터에 따라 결정됩니다. TX 데이터가 있을 때는 모뎀을 깨우고, 없을 때는 설정된 모드에 따라 sleep 상태로 전환합니다.

#### Q5: TX Tasklet Mechanism
**Question**: 이 파일을 보면 tx tasklet에 대한 설명은 전혀 없는데..
**Answer**: TX tasklet 메커니즘을 상세히 분석하여 문서에 추가했습니다. tasklet_setup()/tasklet_init() API 차이, round-robin 스케줄링, spinlock 보호, 그리고 성능 최적화 요소들을 포함한 완전한 설명을 제공했습니다.

#### Q6: NRC Function Documentation
**Question**: kernel 함수에 대한 설명은 어느정도 제공되고 있는데 nrc에서 생성한 함수 예를 들면 setup_ba_session 이와 같은 함수에 대해서는 설명이 빠져있네
**Answer**: setup_ba_session() 함수의 상세한 분석을 추가했습니다. 6단계 BA 세션 설정 과정, IEEE80211_BA_NONE/CLOSE/REJECT 상태 관리, 재시도 로직, S1G 호환성, 그리고 에러 처리 메커니즘을 포함한 완전한 함수별 분석을 제공했습니다.

#### Q7: LSB (Least Significant Bit) Checking
**Question**: unicast를 확인하는 조건이 LSB라고 하면 binary 형태에서 가장 오른쪽에서 있는 bit를 확인하는게 아닌가?
**Answer**: 맞습니다. `is_multicast_ether_addr()` 함수는 ethernet 주소의 첫 번째 바이트에서 LSB (가장 오른쪽 비트)를 확인합니다. IEEE 802 표준에서 이 비트를 I/G (Individual/Group) 비트라고 하며, 0이면 unicast, 1이면 multicast/broadcast를 의미합니다.

**Binary Examples**:
- Unicast: `02:00:00:00:00:01` → 첫 바이트 `0x02` → `00000010` → LSB=0 (unicast)
- Multicast: `03:00:00:00:00:01` → 첫 바이트 `0x03` → `00000011` → LSB=1 (multicast)

### Documentation Language Updates
- Fixed Korean content in English-only files (5 files converted to English)
- Added bilingual documentation guidelines to support both English and Korean versions
- Ensured technical accuracy in all translations while preserving source code references

### Session 2: Work Log Management and Conversation Tracking

#### Context Continuation
**Background**: 이전 conversation에서 나온 context가 요약되어 새로운 세션이 시작됨. 사용자가 Q&A 내용이 work_log.md에 추가되는지 확인함.

#### Q1: Work Log Updates
**Question**: 분석된 문서에 내가 추가로 질문하고 니가 답하는 내용들도 모두 work_log.md에 추가되고 있는거지?
**Answer**: 네, 맞습니다. 이전 세션의 모든 기술적 Q&A 내용을 work_log.md에 추가했습니다. TX 데이터 처리, credit system, power save, tasklet 메커니즘, NRC 함수 분석, LSB 검사 등 모든 질문과 답변이 기록되었습니다.

#### Q2: Complete Conversation Tracking
**Question**: 우리가 나누는 conversation의 모든 내용이 work_log.md에 추가되어야 해. 그래야 다음에 claude cli 를 재 실행하더라도 우리가 진행한 마지막 작업이 항상 유지되니까.
**Answer**: 완전히 동의합니다. 세션 연속성을 위해 모든 conversation 내용을 work_log.md에 기록하겠습니다. 이렇게 하면 Claude CLI 재실행 시에도 이전 작업 맥락이 완전히 유지됩니다.

#### Action Taken
- work_log.md에 현재 conversation 내용 추가
- 향후 모든 대화 내용을 실시간으로 work_log.md에 기록하는 방식으로 변경
- 세션 간 연속성 보장을 위한 완전한 conversation tracking 시스템 구축

#### Documentation Strategy Update
- **실시간 기록**: 모든 질문, 답변, 작업 내용을 즉시 work_log.md에 추가
- **세션 연속성**: Claude CLI 재시작 시 완전한 컨텍스트 유지
- **작업 맥락 보존**: 마지막 작업 상태와 진행 방향 명확히 기록

#### Q3: drv_priv Structure Analysis
**Question**: i_sta = to_i_sta(peer_sta); i_sta가 할당받는 drv_priv 에대해서 상세히 설명해줘
**Answer**: 

**drv_priv는 mac80211 프레임워크에서 제공하는 driver-private 데이터 공간입니다:**

**1. 매크로 정의 (nrc.h:396)**
```c
#define to_i_sta(s) ((struct nrc_sta *) (s)->drv_priv)
```

**2. struct nrc_sta 구조체 (nrc.h:371-391)**
```c
struct nrc_sta {
    struct nrc *nw;                              // 드라이버 메인 컨텍스트
    struct ieee80211_vif *vif;                   // 연결된 VIF
    
    enum ieee80211_sta_state state;              // STA 상태 (NONE, AUTH, ASSOC, AUTHORIZED)
    struct list_head list;                       // preassoc_sta_list 연결용
    
    /* 암호화 키 */
    struct ieee80211_key_conf *ptk;              // Pairwise Temporal Key
    struct ieee80211_key_conf *gtk;              // Group Temporal Key
    
    /* 전력 관리 */
    uint16_t listen_interval;                    // Listen interval
    struct nrc_max_idle max_idle;                // BSS max idle 설정
    
    /* Block ACK 세션 관리 (TID별) */
    enum ieee80211_tx_ba_state tx_ba_session[NRC_MAX_TID];  // TX BA 상태 (8개 TID)
    uint32_t ba_req_last_jiffies[NRC_MAX_TID];              // BA 요청 마지막 시간
    struct rx_ba_session rx_ba_session[NRC_MAX_TID];        // RX BA 세션
};
```

**3. drv_priv 할당 및 초기화 과정 (nrc-mac80211.c:2116-2138)**

**NOTEXIST → NONE 상태 전환 시:**
```c
if (state_changed(NOTEXIST, NONE)) {
    memset(i_sta, 0, sizeof(*i_sta));           // 전체 구조체 초기화
    i_sta->nw = nw;                             // 드라이버 컨텍스트 설정
    i_sta->vif = vif;                           // VIF 연결
    
    INIT_LIST_HEAD(&i_sta->list);               // 리스트 헤드 초기화
    
    spin_lock_irqsave(&i_vif->preassoc_sta_lock, flags);
    list_add_tail(&i_sta->list, &i_vif->preassoc_sta_list);  // preassoc 리스트에 추가
    spin_unlock_irqrestore(&i_vif->preassoc_sta_lock, flags);
    
    // BSS max idle 기본값 설정
    i_sta->max_idle.period = i_vif->max_idle_period;
    i_sta->max_idle.options = 0;
}
```

**4. drv_priv 메모리 할당 메커니즘**

**mac80211에서 sta 할당 시:**
- mac80211 프레임워크가 `sizeof(struct ieee80211_sta) + driver_priv_size` 만큼 메모리 할당
- `ieee80211_sta` 구조체 뒤에 드라이버별 private 데이터 공간이 연속으로 할당됨
- NRC 드라이버는 이 공간을 `struct nrc_sta`로 사용

**메모리 레이아웃:**
```
[ieee80211_sta 구조체][struct nrc_sta (drv_priv)]
                      ↑
                      sta->drv_priv가 가리키는 위치
```

**5. Block ACK 세션에서의 활용**

**setup_ba_session()에서 사용:**
```c
peer_sta = ieee80211_find_sta(vif, qmh->addr1);  // mac80211 STA 찾기
i_sta = to_i_sta(peer_sta);                      // drv_priv → nrc_sta 변환

// TID별 BA 세션 상태 확인 및 관리
switch (i_sta->tx_ba_session[tid]) {
    case IEEE80211_BA_NONE:
    case IEEE80211_BA_CLOSE:
        // BA 세션 설정 시도
        break;
    case IEEE80211_BA_REJECT:
        // 재시도 타이머 확인
        break;
}
```

**6. 핵심 특징**

- **Zero-copy 접근**: 포인터 캐스팅으로 즉시 접근 가능
- **Per-STA 상태 관리**: 각 연결된 스테이션별 독립적 상태 유지
- **TID별 세밀 제어**: 8개 TID별로 개별 BA 세션 상태 관리
- **전력 관리 통합**: BSS max idle과 listen interval 관리
- **암호화 키 저장**: PTK/GTK 키 정보 보관
- **리스트 관리**: preassoc/assoc 상태별 리스트 연결

**drv_priv는 mac80211과 NRC 드라이버 간의 브릿지 역할을 하는 핵심 데이터 구조입니다.**

#### Q4: Conversation Documentation Confirmation
**Question**: Did you add the current conversion?
**Answer**: Yes, I added the current conversation to work_log.md. Session 2 now includes all Q&A content: work log updates, complete conversation tracking requirement, and detailed drv_priv structure analysis. All technical details about drv_priv memory layout, initialization process, and Block ACK session usage are fully documented.

---
*Last Updated: 2025-06-17*
*Current Status: Implementing complete conversation tracking in work_log.md*
*Next Session: Continue from last conversation context with full history*