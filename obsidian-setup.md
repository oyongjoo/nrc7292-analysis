# Obsidian으로 개인 분석 블로그 만들기

## Obsidian 장점
- **완전 무료** (개인 사용)
- **로컬 저장** (완전 비공개)
- **마크다운 지원** (기존 블로그 포스트 그대로 사용 가능)
- **그래프 뷰** (문서 간 연결 시각화)
- **플러그인 생태계** (무료)
- **빠른 검색** 및 **백링크**

## 설정 방법

### 1. Obsidian 다운로드
- https://obsidian.md/ 에서 무료 다운로드
- Windows, Mac, Linux 모두 지원

### 2. Vault 생성
- 새 Vault 생성: "NRC7292 Analysis"
- 위치: 원하는 로컬 폴더 선택

### 3. 기존 블로그 포스트 이동
현재 작성된 블로그 포스트들을 Obsidian으로 복사:

```bash
# Obsidian vault 폴더에 복사
cp /home/liam/work/_posts/*.md [Obsidian Vault 경로]/Posts/
cp -r /home/liam/work/code_analysis/ [Obsidian Vault 경로]/Analysis/
```

### 4. 폴더 구조 제안
```
NRC7292 Analysis/
├── 📁 Posts/                    # 블로그 포스트
│   ├── TX Path Analysis.md
│   ├── Architecture Overview.md
│   └── Mesh Networking.md
├── 📁 Code Analysis/            # 상세 분석
│   ├── TX-RX Processing/
│   ├── Hardware Interface/
│   └── Protocol Implementation/
├── 📁 Notes/                    # 개인 노트
├── 📁 Images/                   # 이미지 파일
└── 📁 Templates/                # 템플릿
```

### 5. 유용한 플러그인 (무료)
- **Dataview**: 동적 쿼리
- **Templater**: 템플릿 자동화  
- **Advanced Tables**: 테이블 편집
- **Calendar**: 일자별 노트
- **Graph Analysis**: 고급 그래프

## 장점 요약
✅ 완전 무료 (개인 사용)
✅ 로컬 저장 (완전 비공개)
✅ 마크다운 호환
✅ 빠른 검색
✅ 그래프 뷰로 연결 관계 시각화
✅ 플러그인으로 기능 확장
✅ 동기화 불필요 (로컬 파일)