# 보드 데이터 (nrc-bd.c) 분석

## 파일 개요
보드 데이터 관리는 각 국가의 무선 규제를 준수하기 위한 핵심 모듈입니다.

## 주요 기능

### 1. 보드 데이터 파일 처리
```c
// 파일 경로: /lib/firmware/{bd_name}
static void * nrc_dump_load(struct nrc *nw, int len)
```
- 커널 버전별 파일 시스템 접근 처리
- 5.0 ~ 5.18 버전 호환성 지원
- 메모리 관리 및 에러 처리

### 2. 국가 코드 매핑
```c
enum {
    CC_US=1, CC_JP, CC_K1, CC_TW, CC_EU, 
    CC_CN, CC_NZ, CC_AU, CC_K2, CC_MAX
} CC_TYPE;
```

### 3. 채널 테이블 관리
각 국가별로 정의된 채널 매핑:
- S1G 주파수 (실제 HaLow 주파수)
- Non-S1G 주파수 (WiFi 호환 주파수)
- 채널 인덱스 매핑

## 핵심 함수 분석

### `nrc_read_bd_tx_pwr()`
1. 국가 코드 식별
2. 하드웨어 버전 매칭
3. 보드 데이터 파싱
4. 체크섬 검증
5. 지원 채널 목록 구성

### `nrc_check_bd()`
보드 데이터 파일의 무결성 검사:
- 파일 크기 검증
- 헤더 정보 확인
- 체크섬 계산 및 비교

## 데이터 구조

### BDF (Board Data File) 구조
```c
struct BDF {
    uint8_t ver_major;
    uint8_t ver_minor;
    uint16_t total_len;
    uint16_t num_data_groups;
    uint16_t checksum_data;
    uint8_t data[];
};
```

### WIM BD 파라미터
```c
struct wim_bd_param {
    uint16_t type;          // 국가 코드
    uint16_t hw_version;    // 하드웨어 버전
    uint16_t length;        // 데이터 길이
    uint16_t checksum;      // 체크섬
    uint8_t value[];        // 실제 데이터
};
```

## 특이사항

### 한국의 특별 처리
- K1 (USN1): LBT 요구사항
- K2 (USN5): MIC 검출 요구사항
- `kr_band` 파라미터로 동적 선택

### 커널 버전 호환성
복잡한 조건부 컴파일로 다양한 커널 버전 지원:
- `get_fs()`, `set_fs()` API 변화 대응
- `force_uaccess_begin()` 사용
- `kernel_read()` 함수 시그니처 변화 처리