# CLAUDE.md — fio-study 프로젝트 지침

> 주석 작성 방법론(파일/함수/인라인/구조체 포맷, 핵심 원칙, 공통 작업 원칙, 커밋 규칙)은
> 상위 디렉토리의 [`../CLAUDE.md`](../CLAUDE.md)에서 공통으로 관리한다.
> 이 파일은 **fio 도메인에 특화된 지식과 작업 현황**만 정의한다.

## 프로젝트 개요

이 저장소는 [fio (Flexible I/O Tester)](https://github.com/axboe/fio) 소스 코드를 분석하고 학습하기 위한 스터디 프로젝트이다. 원본 fio 코드에 한국어 주석을 추가하여 코드 이해를 돕는 것이 목표이다.

- **원본**: fio v3.42 (Jens Axboe)
- **저장소**: https://github.com/kangharison/fio-study.git
- **브랜치**: master

## fio 핵심 I/O 흐름

```
main() [fio.c]
  → parse_options() [init.c]          # 명령줄/잡 파일 파싱
  → fio_backend() [backend.c]         # 메인 실행 루프
      → load_ioengine() [ioengines.c] # I/O 엔진 로드
      → td_io_init()                  # 엔진 초기화
      → 스레드/프로세스 생성
          → get_io_u() [io_u.c]       # I/O 유닛 할당
          → td_io_prep()              # I/O 준비
          → td_io_queue()             # I/O 제출
          → td_io_commit()            # 일괄 커밋
          → td_io_getevents()         # 완료 이벤트 수집
          → put_io_u()                # I/O 유닛 반환
      → show_run_stats() [stat.c]     # 결과 출력
```

## 핵심 자료구조 관계도

```
thread_data (잡 1개 = 스레드/프로세스 1개)
  ├── thread_options (잡 파일/CLI 옵션)
  ├── ioengine_ops (I/O 엔진 플러그인 콜백)
  │     ├── init / cleanup
  │     ├── queue / commit
  │     ├── getevents / event
  │     └── prep / open_file / close_file
  ├── io_u_queue (free / io_u_all / io_u_requeues)
  │     └── io_u (I/O 유닛 하나: offset, buflen, buf, ddir, ...)
  ├── fio_file[] (대상 파일/디바이스)
  └── thread_stat (누적 통계: lat, bw, iops)

io_log (io trace replay / write log)
verify_header (검증 데이터 메타)
```

## fio 특화 주석 요구사항

fio 코드의 특성상, 주석에 다음 사항을 반드시 포함한다:

1. **잡/스레드 모델**: 함수가 메인 프로세스, 잡 스레드, helper thread, verify 스레드 중 어디에서 실행되는지 명시
2. **I/O 엔진 플러그인 계약**: `struct ioengine_ops`의 콜백(queue/commit/getevents/event/init/cleanup)이 언제 어떤 순서로 호출되는지 설명
3. **io_u 생명주기**: free → prepped → in_flight → completed → free 상태 전이와 관련 함수(`get_io_u` / `put_io_u` / `td_io_queue`) 명시
4. **시스템 호출 맥락**: `preadv2`, `io_uring_enter`, `ioctl` 등의 호출 시 커널이 어떤 일을 수행하는지 간단히 설명
5. **OS/아키텍처 분기**: `#ifdef CONFIG_*`, `#ifdef __linux__` 분기가 있는 곳에서는 각 분기의 이유와 의미 설명
6. **verify/replay 경로**: 검증 대상 데이터(`verify_header`), io log replay 흐름이 나타나면 데이터 소스와 목적 설명
7. **통계 수집 경로**: `struct thread_stat`, `io_sample_data` 등 통계 누적 경로 설명
8. **플래그 비트 풀이**: `td->flags`, `io_u->flags`, `FIO_OPT_*` 등 비트 필드의 의미를 처음 등장 시 풀이
9. **NVMe/io_uring 스펙 참조**: 해당 코드가 NVMe 1.x 스펙 또는 io_uring UAPI의 어느 항목과 연관되는지 부연

## 디테일 수준 가이드 (fio 구체화)

| 파일 유형 | 상단 블록 | 함수 주석 | 인라인 주석 | 구조체 필드 |
|----------|----------|----------|------------|-----------|
| 핵심 코드 (fio.c, backend.c, ioengines.c, io_u.c, stat.c, init.c 등) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| I/O 엔진 (engines/*.c) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| 라이브러리 (lib/*.c) | 매우 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| 체크섬/해시 (crc/*.c) | 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| OS 추상화 (os/, oslib/) | 상세 | 모든 함수 | **모든 라인** | 모든 필드 |
| 아키텍처 헤더 (arch/) | 상세 | 모든 함수 | **모든 라인** | - |
| 테스트 (t/, unittests/, mock-tests/) | 상세 | 모든 함수 | **모든 라인** | 주요 구조체 |
| 프로파일 (profiles/) | 상세 | 모든 함수 | **모든 라인** | 주요 구조체 |

## 주요 용어 사전

- **td** / **thread_data**: fio의 잡 1개를 나타내는 실행 컨텍스트
- **io_u**: I/O 유닛 (하나의 read/write 요청)
- **ddir**: 데이터 방향 (`DDIR_READ`, `DDIR_WRITE`, `DDIR_TRIM`)
- **ioengine**: I/O 백엔드 플러그인 (sync, libaio, io_uring, net, rdma 등)
- **SQ/CQ**: io_uring의 Submission / Completion Queue (커널과 공유되는 링 버퍼)
- **SQE/CQE**: Submission/Completion Queue Entry
- **PRP**: Physical Region Page (NVMe의 DMA 물리 주소 리스트)
- **verify**: 쓰기 후 데이터 무결성 검증 (CRC/해시)
- **replay**: 기록된 io log를 재생하는 모드

## 주석 작업 진행 현황

> **재작업 확정 (신기준 `../CLAUDE.md` + 본 파일의 절대 규칙 적용)**
> - **기준**: "주석만으로 아키텍처 이해 가능" — 필수 4섹션 상단 블록, 모든 함수/필드/실행 라인.
> - **기존 주석 처리**: 삭제하지 않고 **그 자리에서 확장/보강**하여 신기준 충족.
> - **진행 방식**: 디렉토리 단위로 완료 후 본 섹션의 "작업 로그"와 "파일별 상태"에 즉시 기록한다.
>   새 세션이 시작되어도 **다음 착수 지점**과 **직전 파일의 품질 수준**을 이 파일만 보고 이어갈 수 있어야 한다.

### 세션 재개 프로토콜 (다음 세션에서 이 명령을 받으면 반드시 이 순서대로 수행)

1. 본 섹션 **"작업 로그"** 의 마지막 엔트리에서 **마지막으로 보강된 파일**과 **다음 착수 대상**을 확인한다.
2. 아래 **"파일별 신기준 상태"** 표에서 `◐`(부분) / `○`(미착수) 항목을 우선순위 순으로 선택한다.
3. 착수 전 해당 파일의 한국어 주석 밀도(한국어 라인 수 / 전체 라인 수)를 측정하여 현재 상태를 갱신한다.
4. 작업 완료 후 **반드시** 다음을 업데이트한다:
   - 파일별 상태 표의 해당 행 (`○`→`◐`→`●`, 한국어 라인 수 갱신)
   - "작업 로그"에 날짜/파일/요약 엔트리 추가
   - "다음 착수 대상" 포인터 갱신
5. 기존 상태 표시를 임의로 낙관적으로 올리지 않는다. 신기준 충족 판단 시 §0의 절대 규칙(상단 블록 4섹션, 모든 함수, 모든 필드 멀티라인, 모든 실행 라인 인라인)을 모두 만족해야 `●`.

### 상태 기호 정의

- `●` **신기준 완료**: 상단 블록 4섹션 + 모든 함수 주석 + 모든 필드 멀티라인 + 실행 라인 전수 인라인. 한국어/전체 비율은 파일 성격에 따라 다르나 통상 50%+.
- `◐` **부분 (구기준)**: 상단 블록·일부 함수 주석은 있으나 인라인/필드 주석이 얕음. 구기준에서 "완료"로 집계되었던 파일 대다수가 여기에 해당.
- `○` **미착수**: 한국어 주석 전무 또는 극히 일부.

### 우선순위 (신기준 재작업 순서)

1. `engines/*.c` (43) — 시작 지점. 내부 편차 큼.
2. 핵심 코어: `fio.c`, `backend.c`, `init.c`, `io_u.c`, `ioengines.c`, `stat.c`, `iolog.c`, `verify.c`, `options.c`, `server.c`
3. `lib/` (45)
4. `arch/` (18)
5. `t/` (19), `unittests/` (10)
6. `os/` (23), `crc/` (33), `oslib/` (26) — 구기준 미완료
7. 기타 최상위 헤더·유틸

### 파일별 신기준 상태 (스폿 체크 스냅샷, 2026-04-13 기준)

> 측정 방법: `rg -c '\[한국어\]' <file>` / `wc -l <file>`. 신기준 충족 여부는 비율만으로 단정할 수 없으며 실제 §0 절대 규칙 통과 여부로 판정한다. 아래 `◐`는 모두 "상단 블록은 있으나 인라인·필드 주석이 신기준 미달".

#### engines/ (43개, 모두 상단 블록 존재 — 신기준 미달)

| 파일 | 한국어/전체 | 상태 | 비고 |
|------|----------:|:---:|------|
| null.c | 신기준 | ● | **레퍼런스 템플릿** — ioengine_ops 계약 요약표 포함. 2026-04-13 보강 |
| libaio.c | 227/1222 (19%) | ◐ | 인라인 보강 필요 |
| sync.c | 93/809 (11%) | ◐ | 얕음 |
| io_uring.c | 60/? | ◐ | 측정 필요 |
| nvme.c | 298/? | ◐ | 큰 파일, 인라인 부족 |
| sg.c | 543/? | ◐ | 큰 파일 |
| net.c | 438/? | ◐ | |
| rdma.c | 280/? | ◐ | |
| ime.c | 372/? | ◐ | |
| xnvme.c | 301/? | ◐ | |
| rbd.c | 156/? | ◐ | |
| splice.c | 153/? | ◐ | |
| solarisaio.c | 118/? | ◐ | |
| mtd.c | 61/? | ◐ | |
| nbd.c | 66/? | ◐ | |
| mmap.c | 65/? | ◐ | |
| libpmem.c | 83/? | ◐ | |
| e4defrag.c | 83/? | ◐ | |
| rados.c | 234/? | ◐ | |
| posixaio.c | 83/? | ◐ | |
| nfs.c | 94/? | ◐ | |
| windowsaio.c | 71/? | ◐ | |
| libzbc.c | 78/? | ◐ | |
| libblkio.c | 20/? | ◐ | 얕음 |
| libiscsi.c | 16/? | ◐ | 얕음 |
| libhdfs.c | 12/? | ◐ | 얕음 |
| libcufile.c | 12/? | ◐ | 얕음 |
| http.c | 19/? | ◐ | 얕음 |
| dfs.c | 13/? | ◐ | 얕음 |
| cpu.c | 14/? | ◐ | |
| cmdprio.c | 17/? | ◐ | |
| exec.c | 443→690여 라인 | ● | fork/execvp/waitpid 커널 의미 명시. 2026-04-13 보강 |
| falloc.c | 51/? | ◐ | |
| ftruncate.c | 27/? | ◐ | |
| skeleton_external.c | 62/? | ◐ | |
| fileoperations.c | 신기준 | ● | 파일시스템 메타연산 측정 엔진. 2026-04-13 보강 |
| glusterfs.c | 신기준 | ● | sync/async 공유 공통 유틸. 2026-04-13 보강 |
| glusterfs_sync.c | 신기준 | ● | FIO_SYNCIO+FIO_DISKLESSIO, CONFIG_GF_NEW_API 분기 명시. 2026-04-13 보강 |
| glusterfs_async.c | 239→500여 라인 | ● | ioengine 비동기 계약(queue→FIO_Q_QUEUED/event/getevents). 2026-04-13 보강 |
| dev-dax.c | 신기준 | ● | DAX 페이지캐시 우회·CLWB/SFENCE 영속성 명시. 2026-04-13 보강 |
| nvme.h | 39/? | ◐ | 헤더 |
| cmdprio.h | 11/? | ◐ | 헤더 |
| gfapi.h | 9/? | ◐ | 헤더 |

#### 핵심 코어 (최상위)

| 파일 | 한국어/전체 | 상태 | 비고 |
|------|----------:|:---:|------|
| backend.c | 199/4082 (5%) | ◐ | **얕음, 최우선** |
| io_u.c | 433/3684 (12%) | ◐ | 얕음 |
| fio.c | 1/? | ◐ | **거의 없음** |
| init.c | — | ◐ | 측정 필요 |
| ioengines.c | 46/? | ◐ | |
| stat.c | 39/? | ◐ | 얕음 |
| iolog.c | 157/? | ◐ | |
| verify.c | 151/? | ◐ | |
| options.c | 97/? | ◐ | |
| server.c | 156/? | ◐ | |

#### 그 외 디렉토리 요약

| 디렉토리 | 파일 수 | 일괄 상태 | 비고 |
|---------|--------:|:---:|------|
| lib/ | 45 | ◐ | 구기준 완료, 인라인 보강 필요 |
| arch/ | 18 | ◐ | 구기준 완료 |
| t/ | 19 | ◐ | |
| unittests/ | 10 | ◐ | |
| profiles/ | 2 | ◐ | |
| mock-tests/ | 2 | ◐ | |
| compiler/ | 1 | ◐ | |
| exp/ | 1 | ◐ | |
| os/ | 23 | ○/◐ | 구기준 미완료 — 헤더 일부만 얕은 주석 |
| crc/ | 33 | ○/◐ | 구기준 미완료 |
| oslib/ | 26 | ○/◐ | 구기준 미완료 |

### 작업 로그

각 세션에서 신기준으로 보강한 파일을 **맨 위에** 추가한다. 형식: `- YYYY-MM-DD | <경로> | <요약> | 한국어 라인 <before>→<after>`.

- 2026-04-13 | engines/exec.c | fork/execvp/waitpid/SIGTERM·SIGKILL·dup2 커널 의미, exec_options 6필드 §4, 모든 함수 §2, 모든 실행 라인 인라인 | 9 → 443→690여 라인 확장
- 2026-04-13 | engines/dev-dax.c | DAX 페이지캐시 우회·pmem_memcpy_persist/CLWB/SFENCE 영속성, sysfs 파싱 인라인, ioengine_ops 필드별 설명 | 7 → 신기준
- 2026-04-13 | engines/null.c | **레퍼런스 템플릿**: 상단에 ioengine_ops 계약 요약표, C++/extern "C" 래퍼·constructor/destructor·NullData 메서드 전 라인 인라인, ioengine_ops 전 필드 §4 | 74 → 신기준
- 2026-04-13 | engines/fileoperations.c | "데이터 I/O 아닌 FS 메타연산 측정" 명시, FIO_FILESTAT_* 매크로, fc_data·filestat_options·fio_engine 필드 §4, 플래그 비트(FIO_DISKLESSIO/SYNCIO/FAKEIO/SYNCFS/NOSTATS/NOFILEHASH) 풀이 | 3 → 신기준
- 2026-04-13 | engines/glusterfs.c | sync/async 공유 공통 유틸, glfs_info 전 필드 §4, 9개 함수 §2 | 얕음 → 신기준
- 2026-04-13 | engines/glusterfs_sync.c | CONFIG_GF_NEW_API #ifdef 분기, FIO_SYNCIO+FIO_DISKLESSIO 플래그, LAST_POS 센티넬, FIO_Q_COMPLETED 계약 | 2 → 신기준
- 2026-04-13 | engines/glusterfs_async.c | 비동기 계약(queue→FIO_Q_QUEUED/event/getevents/io_u_init/io_u_free), fio_gf_iou 두 필드 §4, gf_async_cb 콜백 경로 | 4/239 → 500여 라인
- 2026-04-13 | (보강 작업 없음) | 현황 스냅샷 작성 — 본 섹션 재정비. 이전의 "267/368 구기준 완료" 표기를 파일별 ◐/●/○ 체계로 이관.

### 다음 착수 대상 (Next Up)

> 다음 세션에서 이 프로젝트에 동일 명령("주석 작업해줘")을 받으면 **우선 이 포인터부터 확인**한다.
> 포인터가 비었거나 방금 완료된 파일이면 "우선순위" 규칙에 따라 다음 파일을 선택하고 이 포인터를 갱신한다.

**현재 진행 상황**: engines/ 43개 중 **7개 ● 완료** (glusterfs_async/sync, glusterfs, fileoperations, exec, null, dev-dax). 나머지 36개 ◐.

**다음 세션 우선순위** — 얕은 것부터, 작고 독립적인 것부터:
- **1순위 (극도로 얕은 엔진)**: `engines/libblkio.c` (20), `engines/libiscsi.c` (16), `engines/libhdfs.c` (12), `engines/libcufile.c` (12), `engines/http.c` (19), `engines/dfs.c` (13), `engines/cpu.c` (14), `engines/cmdprio.c` (17), `engines/ftruncate.c` (27), `engines/falloc.c` (51)
- **2순위 (중간 깊이 엔진)**: `engines/skeleton_external.c` (62), `engines/mmap.c` (65), `engines/nbd.c` (66), `engines/windowsaio.c` (71), `engines/libzbc.c` (78), `engines/libpmem.c` (83), `engines/posixaio.c` (83), `engines/e4defrag.c` (83), `engines/sync.c` (93), `engines/nfs.c` (94), `engines/solarisaio.c` (118), `engines/splice.c` (153), `engines/rbd.c` (156)
- **3순위 (큰 엔진 — 분할 위임 권장)**: `engines/libaio.c` (227), `engines/rados.c` (234), `engines/rdma.c` (280), `engines/xnvme.c` (301), `engines/nvme.c` (298), `engines/ime.c` (372), `engines/net.c` (438), `engines/sg.c` (543), `engines/io_uring.c` (60+, 핵심 Linux 엔진 — 별도 주의)
- **헤더**: `engines/nvme.h` (39), `engines/cmdprio.h` (11), `engines/gfapi.h` (9) — 관련 .c와 함께 처리.

**코어 블록 착수 전제조건**: engines 43개 중 최소 10개가 `●` 된 뒤 `backend.c`/`io_u.c`로 이동 권장. 현재 7/43.

**신기준 레퍼런스 파일**: `engines/null.c` (ioengine_ops 콜백 계약 요약표 포함). 다른 엔진 작업 시 이 파일을 참고 예시로 제시할 것.

**세션별 권장 배치 크기**: 소형 파일(<100 라인 한국어 추가 전) 3~5개 병렬, 중형(100~300) 2~3개 병렬, 대형(300+) 1개씩 순차. 토큰 85% 상한을 넘지 않도록 웨이브 간 갱신 체크.

## 빌드 방법

```bash
./configure
make
```

## 참고

- fio 공식 문서: HOWTO.rst
- fio 옵션 목록: `fio --enghelp`, `fio --cmdhelp`
- gfio (GUI): GTK+ 기반 그래픽 프론트엔드 (gfio.c, gclient.c 등)
- NVMe 스펙: NVM Express Base Specification 1.x
- io_uring: Linux Kernel UAPI `linux/io_uring.h`
