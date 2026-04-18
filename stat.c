/*
 * [한국어 설명] fio 통계 수집·계산·출력 엔진 (stat.c)
 *
 * === 파일의 역할 ===
 * fio의 성능 측정 결과 수집부터 최종 보고서 출력까지의 전 과정을 담당하는 통계 모듈이다.
 * I/O 완료 시점마다 io_u.c 로부터 호출되는 add_clat_sample / add_slat_sample /
 * add_lat_sample / add_bw_sample / add_iops_sample 이 "샘플 입력 통로"가 되고,
 * 이를 로그 스케일 히스토그램 버킷(thread_stat::io_u_plat) + Welford 평균/분산 누적기
 * (struct io_stat) + 실시간 로그(struct io_log) 세 갈래로 분산 저장한다.
 * 잡 종료 후 backend.c 가 show_run_stats → __show_run_stats 를 호출하면
 * calc_clat_percentiles / sum_thread_stats / show_group_stats / show_ddir_status /
 * show_thread_status_normal|terse|json 이 4가지 출력 포맷 중 선택된 형식으로 최종 보고서를 생성한다.
 * steadystate 판정(std dev / linear regression)과 iodepth·submit·complete 퍼센타일,
 * disk util, CPU 사용률(getrusage), cgroup stats, latency_percentile 분포까지 한 파일에서 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전체 실행 흐름에서 "측정/보고" 계층을 담당한다. 다음 세 경로로 진입한다.
 *
 *   [샘플 수집 경로]
 *     io_u_complete/io_u_sync_complete [io_u.c]
 *       → account_io_completion [io_u.c]
 *         → add_clat_sample/add_slat_sample/add_lat_sample [이 파일]
 *           → add_stat_sample       (Welford 평균/분산)
 *           → io_u_plat[bucket]++   (로그 스케일 히스토그램 버킷)
 *           → add_lat_percentile_sample (ns/us/ms 분포 카운터)
 *           → add_log_sample        (iolog write 경로, 선택적)
 *
 *   [최종 보고서 경로]
 *     fio_backend [backend.c]
 *       → show_run_stats [이 파일]
 *         → __show_run_stats
 *           → sum_thread_stats (그룹·mixed 집계)
 *           → calc_lat / calc_clat_percentiles / calc_steady_state_process
 *           → show_thread_status_{normal|terse|json} (output_format bit mask 분기)
 *           → show_group_stats / show_disk_util / show_idle_prof_stats
 *
 *   [실시간 ETA 경로]
 *     helper_thread [helper_thread.c]
 *       → check_status_file / __show_running_run_stats [이 파일]
 *         → calc_thread_status / eta_to_str (eta.c 와 연계)
 *
 * 실행 컨텍스트: 대부분의 add_*_sample 은 잡 스레드(단일 잡=단일 스레드) 에서 호출되어 락이
 * 필요 없으나, __show_running_run_stats 는 helper_thread 에서도 호출 가능하므로
 * stat_sem(struct fio_sem) 으로 보호한다. __show_run_stats 는 메인 프로세스(부모) 에서
 * 모든 잡이 reap 된 뒤 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - io_u.c: I/O 완료 시 add_clat_sample/add_slat_sample/add_lat_sample 을 직접 호출한다.
 *   완료 시각(nsec) 과 io_u->offset/bs/priority 를 전달받아 샘플 누적.
 * - backend.c: fio_backend() 의 main loop 말미에 show_run_stats() 를 호출한다.
 *   또한 check_for_running_stats() / __show_running_run_stats() 를 ETA/중간 보고에 사용.
 * - server.c: fio_server_send_ts() 로 thread_stat 을 직렬화해 원격 클라이언트에 전송한다.
 *   CPU 아키텍처/엔디안 차이를 convert_thread_stats 로 처리.
 * - iolog.c: __add_log_sample / add_log_sample 이 io_log 의 원형 버퍼에 누적하고,
 *   write_iolog_close / calc_log_samples 가 주기적으로 파일에 flush 한다.
 * - diskutil.c: show_disk_util / aggregate_slaves_stats / json_add_disk_utils 연계로
 *   iostat 유사 정보(util/await/svctm/rrqm 등) 를 함께 출력한다.
 * - steadystate.c: calc_steady_state / show_ss_normal 이 변동계수·기울기 모드로
 *   정상 상태 판정을 수행한다.
 * - helper_thread.c: check_for_running_stats 가 status 파일 변경을 감지해 중간 보고 트리거.
 * - idletime.c: show_idle_prof_stats 로 유휴 시간 프로파일 출력.
 * - client.c: handle_show_running_run_stats / handle_text 로 서버측 전송 데이터 재구성.
 * - 핵심 자료구조: thread_stat(스레드 단위 누적), group_run_stats(그룹 집계),
 *   io_stat(평균/분산 Welford 누적기), io_log(시계열 샘플 버퍼).
 *
 * === 주요 함수/구조체 요약 ===
 * - plat_val_to_idx / plat_idx_to_val : 레이턴시 값 ↔ 로그 스케일 버킷 인덱스 변환 (역함수).
 * - calc_clat_percentiles : 히스토그램에서 p-퍼센타일 추출 (50/90/99/99.9/99.99 등).
 * - calc_lat : min/max/mean/stddev 계산 (Welford 로 누적된 M,S → stddev = sqrt(S/(n-1))).
 * - sum_thread_stats : 두 thread_stat 합치기 (그룹·mixed·report 집계).
 * - sum_stat / __sum_stat : Chan 병렬 합성 공식으로 두 io_stat 의 Welford 상태 병합.
 * - show_run_stats / __show_run_stats : 최종 보고서 총괄.
 * - show_thread_status_normal / _terse / _json : 3가지 출력 형식.
 * - add_clat_sample / add_slat_sample / add_lat_sample : 완료/제출/총 레이턴시 입력 통로.
 * - add_bw_sample / add_iops_sample : 주기적 대역폭·IOPS 샘플.
 * - add_log_sample / __add_log_sample / get_cur_log / get_new_log / regrow_log : iolog 버퍼 관리.
 * - calc_log_samples : 헬퍼 스레드에서 주기적 로그 flush 트리거.
 * - __show_running_run_stats / check_for_running_stats / show_running_run_stats : ETA/중간 통계.
 * - reset_io_stats / finalize_logs : 잡 재시작 / 종료 시 통계 초기화·마감.
 * - thread_stat : 잡 단위 누적 통계(clat/slat/lat/bw/iops stat + 버킷 + 퍼센타일 리스트 + CPU/rusage/ss).
 * - group_run_stats : 그룹 단위 aggregate (min/max/sum_bw, total io bytes, runtime).
 * - io_stat : Welford 온라인 평균/분산 누적기 (mean, S, min, max, samples).
 * - io_sample_data / io_sample / log_sample : 시계열 로그 엔트리.
 *
 * === 샘플 수집 경로 ASCII ===
 *
 *   io_u completion
 *     │
 *     ▼
 *   account_io_completion (io_u.c)
 *     ├─ add_slat_sample(td, io_u)                   [제출 레이턴시]
 *     │     ├─ add_stat_sample(&ts->slat_stat[ddir], nsec)  (Welford)
 *     │     └─ __add_log_sample(td->slat_log, ...)          (iolog, optional)
 *     │
 *     ├─ add_clat_sample(td, ddir, nsec, bs, offset, prio)  [완료 레이턴시]
 *     │     ├─ add_stat_sample(&ts->clat_stat[ddir], nsec)
 *     │     ├─ add_lat_percentile_sample                    (ns/us/ms 3종 분포)
 *     │     ├─ ts->io_u_plat[ddir][plat_val_to_idx(nsec)]++ (히스토그램 버킷)
 *     │     ├─ add_clat_prio_stats (cmdprio 별)
 *     │     └─ __add_log_sample(td->clat_log, ...)
 *     │
 *     └─ add_lat_sample(td, ddir, nsec, ...)                 [총 레이턴시 = slat+clat]
 *           └─ add_stat_sample + iolog
 *
 *   (별도 주기적 샘플, 헬퍼 스레드 틱)
 *     add_bw_samples → add_bw_sample → add_stat_sample(&ts->bw_stat) + iolog
 *     add_iops_samples → add_iops_sample → add_stat_sample(&ts->iops_stat) + iolog
 *
 * === 퍼센타일 히스토그램 로그 스케일 설명 ===
 * FIO_IO_U_PLAT_BITS = 6, FIO_IO_U_PLAT_VAL = 64, FIO_IO_U_PLAT_GROUP_NR = 29, FIO_IO_U_PLAT_NR = 64*29 = 1856
 *   - val ≤ 2*64 (=128) : idx = val (1ns 정밀도, 첫 번째 선형 구간).
 *   - val > 128        : MSB 위치로 group 결정, 그룹 내 상위 6비트(PLAT_BITS)로 bucket offset 결정.
 *                         group g 의 대표값 = 2^(g+6) 근처, 총 29 그룹 ≈ 2^35 ≈ ~17 초 커버.
 *   - 상대 오차 상한 = 1/64 ≈ 1.56% (각 그룹 내 인덱스 6비트로 분해능 유지).
 *   - 총 메모리 = 1856 * 8 bytes = 14.5 KiB per ddir → 3 ddir = ~43 KiB per thread_stat.
 *   - 대표값 계산: plat_idx_to_val 이 버킷 중앙값 base + (k+0.5)*2^err_bits 를 반환해 오차 최소화.
 *   - 퍼센타일 추출: sum 누적이 n*(p/100) 을 초과하는 최초 버킷의 대표값을 반환.
 *
 * === 4가지 출력 포맷 매트릭스 ===
 *   Normal (사람 가독)    : show_thread_status_normal + show_ddir_status + show_clat_percentiles
 *   Terse v2..v5 (CSV)   : show_thread_status_terse[_all] + show_ddir_status_terse
 *                           (v5 는 cmdprio 별 통계와 더 많은 필드 포함)
 *   JSON                : show_thread_status_json + add_ddir_status_json (json.c API 사용)
 *   JSON+hist            : JSON 에 io_u_plat[] 원시 버킷 배열까지 포함 (ploting 도구용)
 *   선택 기준: output_format bit mask (FIO_OUTPUT_NORMAL/TERSE/JSON/JSON_PLUS) 로
 *             여러 포맷 동시 출력 가능. __show_run_stats 에서 각 format bit 마다 buf_output 을
 *             별도로 할당해 루프한다.
 *
 * === thread_stat 구조 핵심 ===
 *   struct thread_stat {
 *     io_stat    clat_stat[DDIR_RWDIR_CNT];   // 완료 레이턴시 Welford
 *     io_stat    slat_stat[DDIR_RWDIR_CNT];   // 제출 레이턴시 Welford
 *     io_stat    lat_stat[DDIR_RWDIR_CNT];    // 총 레이턴시 (slat+clat)
 *     io_stat    bw_stat[DDIR_RWDIR_CNT];     // 대역폭
 *     io_stat    iops_stat[DDIR_RWDIR_CNT];   // IOPS
 *     uint64_t   io_u_plat[DDIR_RWDIR_CNT_MAX][FIO_IO_U_PLAT_NR]; // 히스토그램 버킷
 *     uint64_t   io_u_lat_n/u/m [FIO_IO_U_LAT_*_NR];              // ns/us/ms 분포
 *     uint64_t   io_u_submit/complete [FIO_IO_U_PLAT_NR];         // 제출/완료 큐 깊이 분포
 *     clat_prio_stat *clat_prio[DDIR_RWDIR_CNT];                  // cmdprio 별 별도 통계
 *     percentile_list fio_fp64_t[FIO_IO_U_LIST_MAX_LEN];
 *     steady_state ss;                                            // 정상 상태 판정
 *     usr_time/sys_time/ctx/minf/majf                             // getrusage 결과
 *     cgroup stats, disk_util_stat 연계 포인터
 *   };
 *
 * === 주의사항 ===
 * - stat_sem : __show_running_run_stats 의 동시 호출 차단용 전역 세마포어.
 * - Welford 누적은 오버플로 방지 + 한 번에 평균/분산 양쪽 구함 (McCabe/Knuth AoCP vol2).
 * - 로그 sample 은 공유 메모리(smalloc) 에 있어야 다중 프로세스/포크 모드에서 공유됨.
 * - nsec_to_msec/usec 은 스케일 다운 시 나누기·반올림 수행 후 단위 문자열 반환.
 * - sum_thread_stats 의 first 인자: 첫 합산인지 여부 — min_val/max_val 초기화 경로 결정.
 */
/* [한국어] 표준 I/O — stdout/stderr 출력 외에도 buf_output/log_buf 가 간접 사용 */
#include <stdio.h>
/* [한국어] 문자열 복사/비교 — memcpy/memset/strcmp 전반 */
#include <string.h>
/* [한국어] malloc/free/qsort — qsort 는 calc_clat_percentiles 의 퍼센타일 리스트 정렬에 쓰임 */
#include <stdlib.h>
/* [한국어] timeval 구조체와 시각 변환 — rusage 의 ru_utime/ru_stime 처리 */
#include <sys/time.h>
/* [한국어] stat(2) 구조체 — check_status_file 의 파일 mtime 감지에 사용 */
#include <sys/stat.h>
/* [한국어] 수학 함수 — sqrt(표준편차), log10(자릿수 계산), scale_down_ns 등에 사용 */
#include <math.h>

#include "fio.h"               /* [한국어] thread_data/thread_stat/group_run_stats/io_stat/io_u/ddir_rw_sum 등 핵심 심볼 카탈로그 */
#include "diskutil.h"          /* [한국어] struct disk_util/disk_util_stat/disk_util_agg — show_disk_util/json_array_add_disk_util 연계 */
#include "lib/ieee754.h"       /* [한국어] fio_fp64_t 고정 엔디안 부동소수점 래퍼 — percentile_list 및 io_stat::mean/S 값을 네트워크로 보낼 때 필요 */
#include "json.h"              /* [한국어] json_object/json_array/json_object_add_value_* API — JSON 출력 형식에 사용 */
#include "lib/getrusage.h"     /* [한국어] fio_getrusage() — Linux/Win 추상화된 getrusage(2) 래퍼 (CPU 시간, 컨텍스트 스위치, 페이지 폴트) */
#include "idletime.h"          /* [한국어] show_idle_prof_stats / struct idle_prof — 유휴 시간 프로파일 통계 */
#include "lib/pow2.h"          /* [한국어] is_power_of_2() — kb_base 1000/1024 판정에 사용 (num2str 단위 분기) */
#include "lib/output_buffer.h" /* [한국어] struct buf_output / buf_output_init/free/dup / log_buf — 포맷된 출력 버퍼링 */
#include "helper_thread.h"     /* [한국어] 헬퍼 스레드 상태 (helper_do_stat/check_for_running_stats 연계) */
#include "smalloc.h"           /* [한국어] 공유 메모리 할당기 — io_log 버퍼가 포크된 잡 간 공유되도록 smalloc 사용 */
#include "zbd.h"               /* [한국어] Zoned Block Device — zbd_* 통계(reset count, wp max 등) 연계 */
#include "oslib/asprintf.h"    /* [한국어] GNU asprintf 호환 (Windows 등) — 동적 문자열 포맷 */

#ifdef WIN32
/* [한국어] Windows 의 타이머 해상도가 낮아(보통 ~15.6ms) log_avg_msec 창을 벗어난 샘플도 허용해야 하므로 2ms 슬랙 */
#define LOG_MSEC_SLACK	2
#else
/* [한국어] 리눅스 등 고해상도 타이머 환경에서는 1ms 슬랙이면 충분 */
#define LOG_MSEC_SLACK	1
#endif

/*
 * [한국어] 로그 샘플 구조체 — 하나의 I/O 이벤트에 대한 통계 데이터 단위.
 * __add_log_sample() 의 인자 다발을 묶은 내부용 임시 구조이며, 이 구조체 자체가
 * io_log 에 저장되는 것은 아니다 (io_log 는 더 작은 struct io_sample 을 저장).
 * log_sample 은 호출 인자 전달용으로만 쓰인다.
 */
struct log_sample {
	union io_sample_data data;
	/* [한국어] 샘플 값 — 용도에 따라 clat/slat/lat 레이턴시(ns) 또는 bw/iops 스칼라.
	 * 설정자: add_clat_sample 계열 각 함수에서 sample_val(nsec) 혹은 sample_plat() 으로 초기화.
	 * 읽는 자: __add_log_sample → io_sample 에 복사되어 iolog 파일 / 메모리 버퍼에 저장.
	 * 값 범위: ns 는 1..수십억 범위, bw 는 bytes/sec, iops 는 ops/sec (64비트 언사인드).
	 * 동기화: 잡 스레드 단독 접근 → 락 불필요. */

	uint32_t ddir;
	/* [한국어] I/O 방향 — DDIR_READ=0 / DDIR_WRITE=1 / DDIR_TRIM=2 / DDIR_SYNC=3 ...
	 * 설정자: 호출자에서 io_u->ddir 그대로 전달.
	 * 읽는 자: io_log 분기(로그 분리 시) / terse·JSON 출력 필드 라벨.
	 * 값 범위: enum fio_ddir — 0..DDIR_LAST-1. trim/sync 는 통계 분리 대상. */

	uint64_t bs;
	/* [한국어] 블록 크기(bytes) — 이 샘플이 발생한 I/O 의 크기.
	 * 설정자: io_u->buflen 또는 io_u->xfer_buflen 그대로 전달.
	 * 읽는 자: iolog 기록 시 크기 계산, 대역폭 환산(bw = bs/시간).
	 * 값 범위: 1..td->o.max_bs 까지 (블록사이즈 옵션 범위). */

	uint64_t offset;
	/* [한국어] I/O 오프셋(bytes) — 이 샘플이 발생한 파일 내 위치.
	 * 설정자: io_u->offset.
	 * 읽는 자: iolog 기록 (per-IO 분석에 유용). per-offset 히스토그램 옵션 시 활용.
	 * 값 범위: 0..real_file_size. */

	uint16_t priority;
	/* [한국어] I/O 우선순위(IOPRIO) — cmdprio 옵션으로 분리 통계 집계에 사용.
	 * 설정자: io_u->ioprio (libaio/io_uring/sg 에서 iocb.aio_reqprio/sqe.ioprio 로 전파됨).
	 * 읽는 자: add_clat_prio_stats 에서 prio 별 버킷 인덱스 검색.
	 * 값 범위: Linux ioprio 16비트 인코딩 — (class<<13)|level|hint. 값 0 = 기본. */

	uint64_t issue_time;
	/* [한국어] I/O 발행 시각(nsec 기준, fio_gettime) — 제출 시각을 로그에 기록할 때 사용.
	 * 설정자: io_u->issue_time.
	 * 읽는 자: iolog 의 완료 기록 시 issue_time 과 함께 저장되어 slat/clat 재구성 가능.
	 * 값 범위: 잡 시작 이후 상대 시각(nsec). 0 = 미기록. */
};

/*
 * [한국어] 통계 출력 동기화 세마포어 — 여러 스레드가 동시에 통계를 출력하지 않도록.
 * 설정자: stat_init() 에서 fio_sem_init(FIO_SEM_UNLOCKED) 로 생성.
 * 읽는 자: __show_running_run_stats() / show_thread_status() 등 외부 진입점에서 fio_sem_down/up 로 상호 배제.
 * 값 범위: 이진 세마포어 (0/1).
 * 동기화: stat_sem 자체가 동기화 프리미티브. helper_thread 와 메인 프로세스의 경합 방지. */
struct fio_sem *stat_sem;

/*
 * [한국어]
 * clear_rusage_stat - 잡 시작 시점의 rusage 스냅샷을 기록하고 누적 카운터를 0으로 초기화
 *
 * @td: 대상 잡의 thread_data — td->ru_start(getrusage 스냅샷), td->ts(누적 통계) 사용.
 *
 * 잡이 시작될 때 또는 ramp_time 종료 후 실제 측정 구간 진입 시 호출되어,
 * 이후 update_rusage_stat() 이 delta 를 계산할 기준점을 잡는다.
 * 누적 usr/sys/ctx/minf/majf 를 0으로 리셋해 이전 구간의 값을 폐기한다.
 *
 * 실행 컨텍스트: 잡 스레드 (본인 td 만 접근) — 동기화 불필요.
 *
 * 호출 체인:
 *   backend.c thread_main() 잡 시작부 → [clear_rusage_stat]
 *   reset_io_stats() → [clear_rusage_stat] (통계 리셋 시)
 */
void clear_rusage_stat(struct thread_data *td)
{
	struct thread_stat *ts = &td->ts;  /* [한국어] 짧은 별칭 — ts 로 잡 누적 통계 참조 */

	fio_getrusage(&td->ru_start);       /* [한국어] getrusage(RUSAGE_SELF) 로 스냅샷 수집 (OS 추상화 래퍼) */
	ts->usr_time = ts->sys_time = 0;    /* [한국어] 유저/커널 모드 CPU 시간(msec) 누적 초기화 */
	ts->ctx = 0;                        /* [한국어] 자발·비자발 컨텍스트 스위치 카운터 초기화 */
	ts->minf = ts->majf = 0;            /* [한국어] 마이너/메이저 페이지 폴트 카운터 초기화 */
}

/*
 * [한국어]
 * update_rusage_stat - 현재 rusage 를 다시 읽어 시작 시점과의 차이를 누적
 *
 * @td: 대상 잡 — td->ru_start/ru_end 스냅샷과 td->ts 누적기에 접근.
 *
 * 잡 실행 중 주기적으로 또는 종료 시점에 호출되어 마지막 스냅샷(ru_start) 이후의
 * 리소스 사용량 delta 를 ts 에 더한다. 호출 후 ru_start 를 ru_end 로 덮어써
 * 다음 구간의 기준점으로 삼는다 (슬라이딩 윈도우 누적).
 *
 * 수집 항목:
 *   usr_time/sys_time : ru_utime/ru_stime 의 차이(msec) — 유저/커널 모드 CPU 사용 시간.
 *   ctx               : 자발+비자발 컨텍스트 스위치 수.
 *   minf/majf         : 마이너/메이저 페이지 폴트 수 (메이저 = 디스크 I/O 동반).
 *
 * 실행 컨텍스트: 잡 스레드 — 단독 접근이라 동기화 불필요.
 *
 * 호출 체인:
 *   thread_main() 종료부 / 주기 체크 → [update_rusage_stat]
 */
void update_rusage_stat(struct thread_data *td)
{
	struct thread_stat *ts = &td->ts;  /* [한국어] 누적 통계 별칭 */

	fio_getrusage(&td->ru_end);         /* [한국어] 현재 rusage 스냅샷 — ru_end 에 저장 */
	ts->usr_time += mtime_since_tv(&td->ru_start.ru_utime,
					&td->ru_end.ru_utime);  /* [한국어] delta(ru_start.utime, ru_end.utime) msec 단위 누적 */
	ts->sys_time += mtime_since_tv(&td->ru_start.ru_stime,
					&td->ru_end.ru_stime);  /* [한국어] 커널 모드 시간도 동일하게 delta 누적 */
	ts->ctx += td->ru_end.ru_nvcsw + td->ru_end.ru_nivcsw
			- (td->ru_start.ru_nvcsw + td->ru_start.ru_nivcsw);  /* [한국어] (자발+비자발) 컨텍스트 스위치 delta */
	ts->minf += td->ru_end.ru_minflt - td->ru_start.ru_minflt;    /* [한국어] 마이너 폴트(디스크 I/O 없이 해결된 페이지 결함) delta */
	ts->majf += td->ru_end.ru_majflt - td->ru_start.ru_majflt;    /* [한국어] 메이저 폴트(디스크 I/O 발생) delta */

	memcpy(&td->ru_start, &td->ru_end, sizeof(td->ru_end));  /* [한국어] 다음 호출의 기준점으로 ru_end 를 ru_start 에 복사 (슬라이딩 윈도우) */
}

/*
 * Given a latency, return the index of the corresponding bucket in
 * the structure tracking percentiles.
 *
 * (1) find the group (and error bits) that the value (latency)
 * belongs to by looking at its MSB. (2) find the bucket number in the
 * group by looking at the index bits.
 *
 */
/*
 * [한국어]
 * plat_val_to_idx - 레이턴시 값 → 로그 스케일 히스토그램 버킷 인덱스 변환
 *
 * @val: 레이턴시 값(ns) 또는 임의의 정수 측정량. 0 허용.
 * @return: 0 ≤ idx < FIO_IO_U_PLAT_NR(=1856) 인 버킷 인덱스.
 *
 * 로그 스케일 버킷 시스템의 핵심 함수. MSB(최상위 1비트 위치)로 "그룹" 을 결정하고,
 * 그 아래 PLAT_BITS(=6) 비트로 "그룹 내 버킷 오프셋" 을 결정한다. 상대 오차 한계는
 * 1/2^PLAT_BITS = 1/64 ≈ 1.56% 로 유지된다. 이 함수와 plat_idx_to_val() 은 역함수 관계.
 *
 * 동작 단계:
 *   1. __builtin_clzll(count leading zeros) 로 MSB 위치 계산. val=0 은 msb=0 으로 처리.
 *   2. msb ≤ PLAT_BITS 이면 선형 구간 — val 자체를 인덱스로 반환(정밀도 손실 없음).
 *   3. 그 외는 error_bits = msb - PLAT_BITS 만큼 하위 비트를 버려 그룹 내 위치 결정.
 *   4. base = (error_bits+1) << PLAT_BITS 로 그룹 시작 인덱스 계산.
 *   5. offset = (val >> error_bits) & (PLAT_VAL-1) 로 그룹 내 6비트 오프셋 추출.
 *   6. base+offset 이 PLAT_NR-1 을 넘지 않도록 포화 클램프.
 *
 * 예시 (PLAT_BITS=6):
 *   val=100  (msb=6)  → linear 구간, idx=100
 *   val=1000 (msb=9)  → error_bits=3, base=4*64=256, offset=(1000>>3)&63=61, idx=317
 *   val=1M  (msb=19) → error_bits=13, base=14*64=896, offset=(1e6>>13)&63=58, idx=954
 *
 * 실행 컨텍스트: 잡 스레드 — add_clat_sample 등에서 호출, 재진입 안전(순수 함수).
 *
 * 호출 체인:
 *   add_clat_sample/add_lat_percentile_sample → [plat_val_to_idx] → io_u_plat[idx]++
 */
static unsigned int plat_val_to_idx(unsigned long long val)
{
	unsigned int msb, error_bits, base, offset, idx;

	/* Find MSB starting from bit 0 */
	if (val == 0)
		msb = 0;                                           /* [한국어] val=0 은 msb 정의 불가 — 편의상 0 으로 취급해 idx=0 반환 유도 */
	else
		msb = (sizeof(val)*8) - __builtin_clzll(val) - 1;  /* [한국어] MSB 비트 위치 = 64 - clz(val) - 1 (GCC/Clang intrinsic — x86 BSR / ARMv8 CLZ 명령 사용) */

	/*
	 * MSB <= (FIO_IO_U_PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index
	 */
	if (msb <= FIO_IO_U_PLAT_BITS)
		return val;                                        /* [한국어] 선형 구간 — val < 2^(PLAT_BITS+1) = 128 이면 val 자체를 인덱스로 사용 (무손실) */

	/* Compute the number of error bits to discard*/
	error_bits = msb - FIO_IO_U_PLAT_BITS;                 /* [한국어] 버릴 하위 비트 수 — 각 그룹 내 6비트 분해능 유지 */

	/* Compute the number of buckets before the group */
	base = (error_bits + 1) << FIO_IO_U_PLAT_BITS;         /* [한국어] 이 그룹 이전 누적 버킷 수 = (err_bits+1) * 64 */

	/*
	 * Discard the error bits and apply the mask to find the
	 * index for the buckets in the group
	 */
	offset = (FIO_IO_U_PLAT_VAL - 1) & (val >> error_bits);  /* [한국어] val 의 상위 MSB 다음 6비트를 그룹 내 오프셋으로 사용 (마스크 63 = 6비트) */

	/* Make sure the index does not exceed (array size - 1) */
	idx = (base + offset) < (FIO_IO_U_PLAT_NR - 1) ?
		(base + offset) : (FIO_IO_U_PLAT_NR - 1);          /* [한국어] 배열 out-of-bounds 방지 포화 클램프 — 극단값도 마지막 버킷에 누적 */

	return idx;
}

/*
 * Convert the given index of the bucket array to the value
 * represented by the bucket
 */
/*
 * [한국어]
 * plat_idx_to_val - 버킷 인덱스 → 대표 레이턴시 값 (버킷 중앙값)
 *
 * @idx: 0 ≤ idx < FIO_IO_U_PLAT_NR 버킷 인덱스.
 * @return: 해당 버킷이 나타내는 값의 "중앙값" (오차 최소화 목적).
 *
 * plat_val_to_idx() 의 역함수. 버킷 최솟값(base) 을 기준으로 버킷 폭 2^error_bits 의
 * 절반(0.5 * 2^err) 을 더해 중앙값을 반환한다. 이렇게 하면 퍼센타일 추출 시 이산화 오차가
 * 최대 버킷 폭의 1/2 로 제한된다.
 *
 * 실행 컨텍스트: 순수 함수 — 어디서든 호출 안전.
 *
 * 호출 체인:
 *   calc_clat_percentiles → [plat_idx_to_val] → 출력 퍼센타일 값
 */
static unsigned long long plat_idx_to_val(unsigned int idx)
{
	unsigned int error_bits;
	unsigned long long k, base;

	assert(idx < FIO_IO_U_PLAT_NR);                        /* [한국어] 배열 범위 벗어나는 인덱스는 버그 — 즉시 abort */

	/* MSB <= (FIO_IO_U_PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index */
	if (idx < (FIO_IO_U_PLAT_VAL << 1))
		return idx;                                        /* [한국어] 선형 구간 — idx < 128 은 val=idx 로 무손실 환원 */

	/* Find the group and compute the minimum value of that group */
	error_bits = (idx >> FIO_IO_U_PLAT_BITS) - 1;          /* [한국어] 그룹 번호 = idx/64 - 1, error_bits = 해당 그룹의 분해능 비트 */
	base = ((unsigned long long) 1) << (error_bits + FIO_IO_U_PLAT_BITS);  /* [한국어] 그룹 최솟값 = 2^(err_bits + 6) */

	/* Find its bucket number of the group */
	k = idx % FIO_IO_U_PLAT_VAL;                           /* [한국어] 그룹 내 오프셋(0..63) */

	/* Return the mean of the range of the bucket */
	return base + ((k + 0.5) * (1 << error_bits));         /* [한국어] 버킷 중앙값 = base + (k+0.5) * 2^err_bits — 버킷 폭의 절반 가산 */
}

/*
 * [한국어]
 * double_cmp - qsort() 용 비교 함수 (fio_fp64_t 오름차순)
 *
 * @a, @b: fio_fp64_t 포인터 (void* 로 전달받아 캐스팅).
 * @return: -1/0/+1 (a<b / a==b / a>b).
 *
 * calc_clat_percentiles 가 플롯리스트 plist[] 를 정렬할 때 qsort 의 comparator 로 사용.
 * fio_fp64_t 는 네트워크 엔디안 고정 float64 래퍼라 u.f 필드로 IEEE754 값 접근.
 * NaN 값이 포함되면 비교가 정의되지 않으므로 호출자가 NaN 을 미리 배제해야 함.
 */
static int double_cmp(const void *a, const void *b)
{
	const fio_fp64_t fa = *(const fio_fp64_t *) a;         /* [한국어] void* → fio_fp64_t 로 복사 (작은 구조체라 비용 미미) */
	const fio_fp64_t fb = *(const fio_fp64_t *) b;
	int cmp = 0;

	if (fa.u.f > fb.u.f)
		cmp = 1;                                           /* [한국어] a > b: 뒤로 정렬 */
	else if (fa.u.f < fb.u.f)
		cmp = -1;                                          /* [한국어] a < b: 앞으로 정렬 */

	return cmp;                                            /* [한국어] 동률(0) 의 경우 qsort 가 안정 정렬을 보장하지 않는다는 점 유의 */
}

/*
 * [한국어]
 * calc_clat_percentiles - 히스토그램 버킷에서 지정된 퍼센타일들의 대표값을 추출
 *
 * @io_u_plat: 각 버킷의 샘플 카운트 배열 (길이 FIO_IO_U_PLAT_NR).
 * @nr: 총 샘플 수 (sum of io_u_plat[]).
 * @plist: 요청 퍼센타일 리스트 (예: {50.0, 90.0, 99.0, 99.9, 0.0}) — 0.0 으로 끝남. 정렬됨(내부 qsort).
 * @output: 출력 — 요청 퍼센타일별 레이턴시 값 배열 (malloc 된다, 호출자가 free 해야 함).
 * @maxv, @minv: 출력 — 반환된 값들의 최대/최소.
 * @return: 실제로 계산된 퍼센타일 개수 (plist 의 유효 길이).
 *
 * 알고리즘:
 *   1. plist 끝(0.0 센티넬) 까지의 길이 len 계산.
 *   2. qsort 로 plist 오름차순 정렬 (일반적으로 이미 정렬되어 있음).
 *   3. io_u_plat[] 을 0..PLAT_NR-1 순회하며 누적 합 sum 구함.
 *   4. sum ≥ (plist[j]/100) * nr 가 되는 최초 버킷 i 에서 plat_idx_to_val(i) 를 ovals[j] 에 기록.
 *   5. 다음 퍼센타일 j+1 로 진행. 한 번의 스캔에 여러 퍼센타일을 동시에 뽑아내므로 O(PLAT_NR + len).
 *   6. minv/maxv 갱신.
 *
 * 에러: ovals 할당 실패 시 0 반환 (호출자가 확인).
 * 실행 컨텍스트: 보고서 생성 경로 — 메인 프로세스에서 호출되며 lock-free.
 *
 * 호출 체인:
 *   show_clat_percentiles / show_thread_status_json / show_ddir_status_terse
 *     → [calc_clat_percentiles] → plat_idx_to_val
 */
unsigned int calc_clat_percentiles(const uint64_t *io_u_plat, unsigned long long nr,
				   fio_fp64_t *plist, unsigned long long **output,
				   unsigned long long *maxv, unsigned long long *minv)
{
	unsigned long long sum = 0;        /* [한국어] 버킷 누적 합 — 현재까지 본 샘플 수 */
	unsigned int len, i, j = 0;        /* [한국어] len=plist 유효 길이, i=버킷 인덱스, j=현재 처리 중인 퍼센타일 인덱스 */
	unsigned long long *ovals = NULL;  /* [한국어] 출력 배열 — 퍼센타일별 대표값 저장 */
	bool is_last;

	*minv = -1ULL;                     /* [한국어] 부호 없는 정수 최댓값 초기화 — min 비교 시 어떤 값도 이보다 작음 */
	*maxv = 0;                         /* [한국어] max 초기화 */

	len = 0;
	while (len < FIO_IO_U_LIST_MAX_LEN && plist[len].u.f != 0.0)
		len++;                         /* [한국어] 0.0 센티넬 전까지 유효 길이 계산 — plist 는 보통 {50,95,99,99.5,99.9,99.95,99.99,0.0} 형태 */

	if (!len)
		return 0;                      /* [한국어] 요청된 퍼센타일이 없으면 즉시 종료 */

	/*
	 * Sort the percentile list. Note that it may already be sorted if
	 * we are using the default values, but since it's a short list this
	 * isn't a worry. Also note that this does not work for NaN values.
	 */
	if (len > 1)
		qsort(plist, len, sizeof(plist[0]), double_cmp);  /* [한국어] 오름차순 정렬 필수 — 아래 루프가 단조 증가 sum 가정 */

	ovals = malloc(len * sizeof(*ovals));
	if (!ovals)
		return 0;                      /* [한국어] 메모리 부족 시 조용히 실패 (호출자가 0 반환 검사) */

	/*
	 * Calculate bucket values, note down max and min values
	 */
	is_last = false;
	for (i = 0; i < FIO_IO_U_PLAT_NR && !is_last; i++) {
		sum += io_u_plat[i];           /* [한국어] i번째 버킷의 카운트를 누적 */
		while (sum >= ((long double) plist[j].u.f / 100.0 * nr)) {
			/* [한국어] sum/nr >= p/100 인 최초 i 에서 해당 퍼센타일의 경계 도달 판정
			 *          long double 캐스팅으로 부동소수 정밀도 확보 (nr 이 매우 클 때 오차 최소화) */
			assert(plist[j].u.f <= 100.0);

			ovals[j] = plat_idx_to_val(i);  /* [한국어] 버킷 중앙값을 퍼센타일 대표값으로 저장 */
			if (ovals[j] < *minv)
				*minv = ovals[j];       /* [한국어] 전체 minv 갱신 — 출력 포맷 스케일링(ns/us/ms) 결정에 사용 */
			if (ovals[j] > *maxv)
				*maxv = ovals[j];

			is_last = (j == len - 1) != 0;  /* [한국어] 마지막 퍼센타일 처리 여부 — 바깥 for 탈출 조건 */
			if (is_last)
				break;

			j++;                        /* [한국어] 다음 퍼센타일로 진행 — 같은 i 에서 여러 퍼센타일 동시 추출 가능 */
		}
	}

	if (!is_last)
		log_err("fio: error calculating latency percentiles\n");  /* [한국어] 샘플 부족/버그 — 이론상 도달 불가 */

	*output = ovals;                   /* [한국어] 출력 파라미터에 ovals 포인터 전달 (호출자 소유권) */
	return len;
}

/*
 * Find and display the p-th percentile of clat
 */
/*
 * [한국어]
 * show_clat_percentiles - 퍼센타일 결과를 Normal 포맷 텍스트로 출력
 *
 * @io_u_plat: 히스토그램 버킷 카운트 배열.
 * @nr: 총 샘플 수.
 * @plist: 퍼센타일 요청 리스트.
 * @precision: 소수점 이하 자릿수 (예: percentile_precision 옵션).
 * @pre: 출력 레이블 (예: "clat", "slat").
 * @out: 출력 버퍼 (buf_output).
 *
 * 단위 자동 스케일링:
 *   - minv>2ms 이고 maxv>99ms → msec 단위 표시
 *   - minv>2us 이고 maxv>99us → usec 단위 표시
 *   - 그 외 → nsec 단위 표시
 * 출력 예:
 *   clat percentiles (usec):
 *    |  50.00th=[  123], 90.00th=[  200], 99.00th=[ 1024], 99.90th=[ 4096],
 *    |  99.99th=[16384]
 *
 * 실행 컨텍스트: 최종 보고서 생성 경로 — 메인 프로세스.
 *
 * 호출 체인:
 *   show_ddir_status → [show_clat_percentiles] → calc_clat_percentiles
 */
static void show_clat_percentiles(const uint64_t *io_u_plat, unsigned long long nr,
				  fio_fp64_t *plist, unsigned int precision,
				  const char *pre, struct buf_output *out)
{
	unsigned int divisor, len, i, j = 0;
	unsigned long long minv, maxv;
	unsigned long long *ovals;
	int per_line, scale_down, time_width;
	bool is_last;
	char fmt[32];

	len = calc_clat_percentiles(io_u_plat, nr, plist, &ovals, &maxv, &minv);
	if (!len || !ovals)
		return;

	/*
	 * We default to nsecs, but if the value range is such that we
	 * should scale down to usecs or msecs, do that.
	 */
	if (minv > 2000000 && maxv > 99999999ULL) {
		scale_down = 2;
		divisor = 1000000;
		log_buf(out, "    %s percentiles (msec):\n     |", pre);
	} else if (minv > 2000 && maxv > 99999) {
		scale_down = 1;
		divisor = 1000;
		log_buf(out, "    %s percentiles (usec):\n     |", pre);
	} else {
		scale_down = 0;
		divisor = 1;
		log_buf(out, "    %s percentiles (nsec):\n     |", pre);
	}


	time_width = max(5, (int) (log10(maxv / divisor) + 1));
	snprintf(fmt, sizeof(fmt), " %%%u.%ufth=[%%%dllu]%%c", precision + 3,
			precision, time_width);
	/* fmt will be something like " %5.2fth=[%4llu]%c" */
	per_line = (80 - 7) / (precision + 10 + time_width);

	for (j = 0; j < len; j++) {
		/* for formatting */
		if (j != 0 && (j % per_line) == 0)
			log_buf(out, "     |");

		/* end of the list */
		is_last = (j == len - 1) != 0;

		for (i = 0; i < scale_down; i++)
			ovals[j] = (ovals[j] + 999) / 1000;

		log_buf(out, fmt, plist[j].u.f, ovals[j], is_last ? '\n' : ',');

		if (is_last)
			break;

		if ((j % per_line) == per_line - 1)	/* for formatting */
			log_buf(out, "\n");
	}

	free(ovals);
}

/*
 * [한국어]
 * get_nr_prios_with_samples - 해당 ddir 에서 실제 샘플이 있는 우선순위(priority) 개수 반환
 *
 * @ts: 대상 thread_stat.
 * @ddir: 방향(READ/WRITE/TRIM).
 * @return: samples > 0 인 cmdprio 버킷 개수.
 *
 * cmdprio(percentage 또는 bssplit 기반 I/O 우선순위 믹스) 가 설정된 잡에서,
 * 실제로 I/O 가 수행된 우선순위만 카운트해 출력 섹션을 결정한다.
 */
static int get_nr_prios_with_samples(struct thread_stat *ts, enum fio_ddir ddir)
{
	int i, nr_prios_with_samples = 0;

	for (i = 0; i < ts->nr_clat_prio[ddir]; i++) {
		if (ts->clat_prio[ddir][i].clat_stat.samples)
			nr_prios_with_samples++;  /* [한국어] 샘플이 있는 prio 만 카운트 — 유령 버킷 배제 */
	}

	return nr_prios_with_samples;
}

/*
 * [한국어]
 * calc_lat - io_stat 에서 min/max/mean/stddev 를 도출
 *
 * @is: Welford 누적기가 들어 있는 io_stat.
 * @min, @max: 출력 — 최소/최대값 (ns 또는 bw/iops 단위).
 * @mean: 출력 — 평균.
 * @dev:  출력 — 표준편차.
 * @return: true = 유효한 통계, false = 샘플 없음.
 *
 * Welford 온라인 알고리즘(Knuth AoCP vol2 4.2.2):
 *   샘플 x_i 마다 delta = x_i - mean, mean += delta/n, S += delta*(x_i - new_mean)
 *   이 누적된 S 로부터 분산 = S/(n-1), 표준편차 = sqrt(분산)
 * 한 번의 스캔으로 mean/variance 양쪽을 O(1) 공간에 구할 수 있고
 * 큰 n 에서도 수치 안정성이 우수 (순수 합산보다 오차 작음).
 */
bool calc_lat(const struct io_stat *is, unsigned long long *min,
	      unsigned long long *max, double *mean, double *dev)
{
	double n = (double) is->samples;   /* [한국어] 샘플 수를 double 로 변환 — 나눗셈 정밀도 보장 */

	if (n == 0)
		return false;                  /* [한국어] 샘플 없음 — 유효 통계 없음 */

	*min = is->min_val;                /* [한국어] Welford 와 별도로 add_stat_sample 이 min/max 갱신 */
	*max = is->max_val;
	*mean = is->mean.u.f;              /* [한국어] fio_fp64_t.u.f 는 IEEE754 double 접근 */

	if (n > 1.0)
		*dev = sqrt(is->S.u.f / (n - 1.0));  /* [한국어] 불편추정 분산(Bessel 보정) → 표준편차 */
	else
		*dev = 0;                      /* [한국어] n=1 이면 표준편차 정의 불가 (분모 0) */

	return true;
}

/*
 * [한국어]
 * show_mixed_group_stats - unified_rw_rep=both 모드에서 MIXED 집계를 추가 출력
 *
 * @rs: 그룹 run stats.
 * @out: 출력 버퍼.
 *
 * Normal 포맷에서 read/write/trim 각 방향을 출력한 뒤,
 * 세 방향을 하나로 합친 "MIXED" 라인을 추가로 출력한다.
 * 집계 방식: iobytes/agg 는 합, min_bw/max_bw 는 전 방향의 극값, min_run/max_run 도 동일.
 */
static void show_mixed_group_stats(const struct group_run_stats *rs, struct buf_output *out)
{
	char *io, *agg, *min, *max;
	char *ioalt, *aggalt, *minalt, *maxalt;
	uint64_t io_mix = 0, agg_mix = 0, min_mix = -1, max_mix = 0;
	uint64_t min_run = -1, max_run = 0;
	const int i2p = is_power_of_2(rs->kb_base);
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		if (!rs->max_run[i])
			continue;
		io_mix += rs->iobytes[i];
		agg_mix += rs->agg[i];
		min_mix = min_mix < rs->min_bw[i] ? min_mix : rs->min_bw[i];
		max_mix = max_mix > rs->max_bw[i] ? max_mix : rs->max_bw[i];
		min_run = min_run < rs->min_run[i] ? min_run : rs->min_run[i];
		max_run = max_run > rs->max_run[i] ? max_run : rs->max_run[i];
	}
	io = num2str(io_mix, rs->sig_figs, 1, i2p, N2S_BYTE);
	ioalt = num2str(io_mix, rs->sig_figs, 1, !i2p, N2S_BYTE);
	agg = num2str(agg_mix, rs->sig_figs, 1, i2p, rs->unit_base);
	aggalt = num2str(agg_mix, rs->sig_figs, 1, !i2p, rs->unit_base);
	min = num2str(min_mix, rs->sig_figs, 1, i2p, rs->unit_base);
	minalt = num2str(min_mix, rs->sig_figs, 1, !i2p, rs->unit_base);
	max = num2str(max_mix, rs->sig_figs, 1, i2p, rs->unit_base);
	maxalt = num2str(max_mix, rs->sig_figs, 1, !i2p, rs->unit_base);
	log_buf(out, "  MIXED: bw=%s (%s), %s-%s (%s-%s), io=%s (%s), run=%llu-%llumsec\n",
			agg, aggalt, min, max, minalt, maxalt, io, ioalt,
			(unsigned long long) min_run,
			(unsigned long long) max_run);
	free(io);
	free(agg);
	free(min);
	free(max);
	free(ioalt);
	free(aggalt);
	free(minalt);
	free(maxalt);
}

/*
 * [한국어]
 * show_group_stats - 그룹 집계 통계를 Normal 포맷으로 출력
 *
 * @rs: 그룹 run stats (DDIR 별 집계).
 * @out: 출력 버퍼.
 *
 * 각 ddir (READ/WRITE/TRIM) 별로 bw/min-max/io/run 라인을 출력한다.
 * unified_rw_rep=UNIFIED_BOTH 이면 MIXED 라인도 추가.
 * num2str() 은 i2p(=1024 기반) 와 !i2p(=1000 기반) 두 표기를 병기한다.
 * 예: "READ: bw=512MiB/s (537MB/s), 256KiB-1GiB (262KB-1.1GB), io=10GiB (10.7GB), run=10-20msec"
 *
 * 호출 체인:
 *   __show_run_stats → [show_group_stats] → show_mixed_group_stats
 */
void show_group_stats(const struct group_run_stats *rs, struct buf_output *out)
{
	char *io, *agg, *min, *max;
	char *ioalt, *aggalt, *minalt, *maxalt;
	const char *str[] = { "   READ", "  WRITE" , "   TRIM"};
	int i;

	log_buf(out, "\nRun status group %d (all jobs):\n", rs->groupid);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		const int i2p = is_power_of_2(rs->kb_base);

		if (!rs->max_run[i])
			continue;

		io = num2str(rs->iobytes[i], rs->sig_figs, 1, i2p, N2S_BYTE);
		ioalt = num2str(rs->iobytes[i], rs->sig_figs, 1, !i2p, N2S_BYTE);
		agg = num2str(rs->agg[i], rs->sig_figs, 1, i2p, rs->unit_base);
		aggalt = num2str(rs->agg[i], rs->sig_figs, 1, !i2p, rs->unit_base);
		min = num2str(rs->min_bw[i], rs->sig_figs, 1, i2p, rs->unit_base);
		minalt = num2str(rs->min_bw[i], rs->sig_figs, 1, !i2p, rs->unit_base);
		max = num2str(rs->max_bw[i], rs->sig_figs, 1, i2p, rs->unit_base);
		maxalt = num2str(rs->max_bw[i], rs->sig_figs, 1, !i2p, rs->unit_base);
		log_buf(out, "%s: bw=%s (%s), %s-%s (%s-%s), io=%s (%s), run=%llu-%llumsec\n",
				(rs->unified_rw_rep == UNIFIED_MIXED) ? "  MIXED" : str[i],
				agg, aggalt, min, max, minalt, maxalt, io, ioalt,
				(unsigned long long) rs->min_run[i],
				(unsigned long long) rs->max_run[i]);

		free(io);
		free(agg);
		free(min);
		free(max);
		free(ioalt);
		free(aggalt);
		free(minalt);
		free(maxalt);
	}

	/* Need to aggregate statistics to show mixed values */
	if (rs->unified_rw_rep == UNIFIED_BOTH)
		show_mixed_group_stats(rs, out);
}

/*
 * [한국어]
 * stat_calc_dist - 큐 깊이(depth) 분포 계산 — 각 버킷 카운트를 전체 대비 퍼센트로 변환
 *
 * @map: FIO_IO_U_MAP_NR 길이의 버킷 배열 (0/1/2/4/8/16/32/64/>=64 등 큐 깊이별).
 * @total: 전체 카운트.
 * @io_u_dist: 출력 — 각 버킷의 퍼센트.
 *
 * io_u_submit/complete 큐 깊이 분포 출력에 사용된다. 0.1% 미만이지만 0 이 아닌 값은
 * "0.1" 로 올려 표시해 "있지만 극소"함을 시각적으로 표현한다.
 */
void stat_calc_dist(const uint64_t *map, unsigned long total, double *io_u_dist)
{
	int i;

	/*
	 * Do depth distribution calculations
	 */
	for (i = 0; i < FIO_IO_U_MAP_NR; i++) {
		if (total) {
			io_u_dist[i] = (double) map[i] / (double) total;
			io_u_dist[i] *= 100.0;
			if (io_u_dist[i] < 0.1 && map[i])
				io_u_dist[i] = 0.1;
		} else
			io_u_dist[i] = 0.0;
	}
}

/*
 * [한국어]
 * stat_calc_lat - 레이턴시 분포 버킷 카운트를 전체 대비 퍼센트로 변환 (내부 헬퍼)
 *
 * @ts: thread_stat (전체 샘플 수 total_io_u 참조).
 * @dst: 출력 — 퍼센트 배열.
 * @src: 입력 — 버킷 카운트 배열 (io_u_lat_n/u/m).
 * @nr: 버킷 개수.
 *
 * stat_calc_lat_n/u/m 의 공통 로직. 매우 작은 비율이지만 0 이 아닌 값은 0.01% 로 표시.
 */
static void stat_calc_lat(const struct thread_stat *ts, double *dst,
			  const uint64_t *src, int nr)
{
	unsigned long total = ddir_rw_sum(ts->total_io_u);
	int i;

	/*
	 * Do latency distribution calculations
	 */
	for (i = 0; i < nr; i++) {
		if (total) {
			dst[i] = (double) src[i] / (double) total;
			dst[i] *= 100.0;
			if (dst[i] < 0.01 && src[i])
				dst[i] = 0.01;
		} else
			dst[i] = 0.0;
	}
}

/*
 * To keep the terse format unaltered, add all of the ns latency
 * buckets to the first us latency bucket
 */
/*
 * [한국어]
 * stat_calc_lat_nu - terse 포맷용: ns 버킷을 모두 us[0] 에 합쳐 하위 호환 유지
 *
 * @ts: thread_stat.
 * @io_u_lat_u: 출력 us 분포.
 *
 * terse v3/v4 는 원래 us/ms 두 종만 출력했으나 fio 에 ns 버킷이 추가되면서
 * 기존 파서를 깨뜨리지 않기 위해 ns 버킷 합계를 us[0] 에 흡수시킨다.
 */
static void stat_calc_lat_nu(const struct thread_stat *ts, double *io_u_lat_u)
{
	unsigned long ntotal = 0, total = ddir_rw_sum(ts->total_io_u);
	int i;

	stat_calc_lat(ts, io_u_lat_u, ts->io_u_lat_u, FIO_IO_U_LAT_U_NR);

	for (i = 0; i < FIO_IO_U_LAT_N_NR; i++)
		ntotal += ts->io_u_lat_n[i];

	io_u_lat_u[0] += 100.0 * (double) ntotal / (double) total;
}

/* [한국어] ns 레이턴시 분포 퍼센트 계산 (io_u_lat_n 버킷 → 퍼센트 배열) */
void stat_calc_lat_n(const struct thread_stat *ts, double *io_u_lat)
{
	stat_calc_lat(ts, io_u_lat, ts->io_u_lat_n, FIO_IO_U_LAT_N_NR);
}

/* [한국어] us 레이턴시 분포 퍼센트 계산 (io_u_lat_u 버킷 → 퍼센트 배열) */
void stat_calc_lat_u(const struct thread_stat *ts, double *io_u_lat)
{
	stat_calc_lat(ts, io_u_lat, ts->io_u_lat_u, FIO_IO_U_LAT_U_NR);
}

/* [한국어] ms 레이턴시 분포 퍼센트 계산 (io_u_lat_m 버킷 → 퍼센트 배열) */
void stat_calc_lat_m(const struct thread_stat *ts, double *io_u_lat)
{
	stat_calc_lat(ts, io_u_lat, ts->io_u_lat_m, FIO_IO_U_LAT_M_NR);
}

/*
 * [한국어]
 * display_lat - 레이턴시 통계(min/max/avg/stdev) 를 사람 가독 한 줄로 출력
 *
 * @name: 레이블 ("slat", "clat", "lat").
 * @min/@max: nsec 단위 min/max.
 * @mean/@dev: 평균/표준편차 (nsec).
 * @out: 출력 버퍼.
 *
 * nsec_to_msec/usec 을 이용해 자동 단위 변환. 출력 후 단위가 바뀌었다면 base 문자열 교체.
 */
static void display_lat(const char *name, unsigned long long min,
			unsigned long long max, double mean, double dev,
			struct buf_output *out)
{
	const char *base = "(nsec)";
	char *minp, *maxp;

	if (nsec_to_msec(&min, &max, &mean, &dev))
		base = "(msec)";
	else if (nsec_to_usec(&min, &max, &mean, &dev))
		base = "(usec)";

	minp = num2str(min, 6, 1, 0, N2S_NONE);
	maxp = num2str(max, 6, 1, 0, N2S_NONE);

	log_buf(out, "    %s %s: min=%s, max=%s, avg=%5.02f,"
		 " stdev=%5.02f\n", name, base, minp, maxp, mean, dev);

	free(minp);
	free(maxp);
}

/*
 * [한국어]
 * gen_mixed_ddir_stats_from_ts - READ+WRITE+TRIM 을 하나의 MIXED 통계로 집계
 *
 * @ts: 원본 thread_stat.
 * @return: 새로 할당된 mixed 통계 (호출자가 free 해야 함). 실패 시 NULL.
 *
 * unified_rw_rep=UNIFIED_BOTH 일 때 원본은 방향별, 별도로 합친 MIXED 도 출력하기 위해
 * 임시 thread_stat 을 만들어 sum_thread_stats 로 세 방향을 합성한다.
 * percentile 옵션과 정밀도는 원본에서 복사.
 */
static struct thread_stat *gen_mixed_ddir_stats_from_ts(const struct thread_stat *ts)
{
	struct thread_stat *ts_lcl;

	/*
	 * Handle aggregation of Reads (ddir = 0), Writes (ddir = 1), and
	 * Trims (ddir = 2)
	 */
	ts_lcl = malloc(sizeof(struct thread_stat));
	if (!ts_lcl) {
		log_err("fio: failed to allocate local thread stat\n");
		return NULL;
	}

	init_thread_stat(ts_lcl);

	/* calculate mixed stats  */
	ts_lcl->unified_rw_rep = UNIFIED_MIXED;
	ts_lcl->lat_percentiles = ts->lat_percentiles;
	ts_lcl->clat_percentiles = ts->clat_percentiles;
	ts_lcl->slat_percentiles = ts->slat_percentiles;
	ts_lcl->percentile_precision = ts->percentile_precision;
	memcpy(ts_lcl->percentile_list, ts->percentile_list, sizeof(ts->percentile_list));
	ts_lcl->sig_figs = ts->sig_figs;

	sum_thread_stats(ts_lcl, ts);

	return ts_lcl;
}

/*
 * [한국어]
 * convert_agg_kbytes_percent - 잡의 평균 대역폭을 그룹 집계 대비 퍼센트로 환산
 *
 * @rs: 그룹 run stats (agg[ddir] 는 KiB/s 가정 — *1024 단위 변환).
 * @ddir: 방향.
 * @mean: 잡의 평균 대역폭 (KiB/s).
 * @return: 잡이 그룹 총 대역폭의 몇 % 를 차지하는지 (0.0~100.0).
 *
 * 다중 잡 출력에서 "bw=512KiB/s (45.2%)" 처럼 점유율 표기에 사용.
 */
static double convert_agg_kbytes_percent(const struct group_run_stats *rs,
					 enum fio_ddir ddir, int mean)
{
	double p_of_agg = 100.0;
	if (rs && rs->agg[ddir] > 1024) {
		p_of_agg = mean * 100.0 / (double) (rs->agg[ddir] / 1024.0);

		if (p_of_agg > 100.0)
			p_of_agg = 100.0;
	}
	return p_of_agg;
}

/*
 * [한국어]
 * show_ddir_status - 방향별(READ/WRITE/TRIM/SYNC) 상세 통계를 Normal 포맷으로 출력
 *
 * @rs: 그룹 run stats.
 * @ts: 잡 thread_stat.
 * @ddir: 출력 대상 방향.
 * @out: 출력 버퍼.
 *
 * 출력 항목 (ddir_rw 기준):
 *   - IOPS, BW(두 단위 병기), 총 io bytes, runtime
 *   - slat/clat/lat 각각 min/max/avg/stdev (display_lat)
 *   - cmdprio 가 2개 이상 샘플 있는 prio 있을 때: 각 prio 별 동일 항목
 *   - slat/clat/lat percentiles (옵션 활성 시)
 *   - per-prio 퍼센타일 (샘플 있는 prio)
 *   - bw/iops 통계 (min/max/avg/stdev/% of agg)
 *
 * ddir_sync(DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE/SYNCFS) 이면 sync_stat 만 출력.
 * ZBD(count_zone_resets) 있으면 zbd_write_status 추가, cachehit/cachemiss 있으면 캐시 히트율.
 *
 * 호출 체인:
 *   show_thread_status_normal → [show_ddir_status] → display_lat/show_clat_percentiles
 */
static void show_ddir_status(const struct group_run_stats *rs, struct thread_stat *ts,
			     enum fio_ddir ddir, struct buf_output *out)
{
	unsigned long runt;
	unsigned long long min, max, bw, iops;
	double mean, dev;
	char *io_p, *bw_p, *bw_p_alt, *iops_p, *post_st = NULL;
	int i2p, i;
	const char *clat_type = ts->lat_percentiles ? "lat" : "clat";

	if (ddir_sync(ddir)) {
		if (calc_lat(&ts->sync_stat, &min, &max, &mean, &dev)) {
			log_buf(out, "  %s:\n",
				"fsync/fdatasync/sync_file_range/syncfs");
			display_lat(io_ddir_name(ddir), min, max, mean, dev, out);
			show_clat_percentiles(ts->io_u_sync_plat,
						ts->sync_stat.samples,
						ts->percentile_list,
						ts->percentile_precision,
						io_ddir_name(ddir), out);
		}
		return;
	}

	assert(ddir_rw(ddir));

	if (!ts->runtime[ddir])
		return;

	i2p = is_power_of_2(rs->kb_base);
	runt = ts->runtime[ddir];

	bw = (1000 * ts->io_bytes[ddir]) / runt;
	io_p = num2str(ts->io_bytes[ddir], ts->sig_figs, 1, i2p, N2S_BYTE);
	bw_p = num2str(bw, ts->sig_figs, 1, i2p, ts->unit_base);
	bw_p_alt = num2str(bw, ts->sig_figs, 1, !i2p, ts->unit_base);

	iops = (1000 * (uint64_t)ts->total_io_u[ddir]) / runt;
	iops_p = num2str(iops, ts->sig_figs, 1, 0, N2S_NONE);
	if (ts->count_zone_resets)
		post_st = zbd_write_status(ts);
	else if (ddir == DDIR_READ && ts->cachehit && ts->cachemiss) {
		uint64_t total;
		double hit;

		total = ts->cachehit + ts->cachemiss;
		hit = (double) ts->cachehit / (double) total;
		hit *= 100.0;
		if (asprintf(&post_st, "; Cachehit=%0.2f%%", hit) < 0)
			post_st = NULL;
	}

	log_buf(out, "  %s: IOPS=%s, BW=%s (%s)(%s/%llumsec)%s\n",
			(ts->unified_rw_rep == UNIFIED_MIXED) ? "mixed" : io_ddir_name(ddir),
			iops_p, bw_p, bw_p_alt, io_p,
			(unsigned long long) ts->runtime[ddir],
			post_st ? : "");

	free(post_st);
	free(io_p);
	free(bw_p);
	free(bw_p_alt);
	free(iops_p);

	if (calc_lat(&ts->slat_stat[ddir], &min, &max, &mean, &dev))
		display_lat("slat", min, max, mean, dev, out);
	if (calc_lat(&ts->clat_stat[ddir], &min, &max, &mean, &dev))
		display_lat("clat", min, max, mean, dev, out);
	if (calc_lat(&ts->lat_stat[ddir], &min, &max, &mean, &dev))
		display_lat(" lat", min, max, mean, dev, out);

	/* Only print per prio stats if there are >= 2 prios with samples */
	if (get_nr_prios_with_samples(ts, ddir) >= 2) {
		for (i = 0; i < ts->nr_clat_prio[ddir]; i++) {
			char buf[64];

			if (!calc_lat(&ts->clat_prio[ddir][i].clat_stat, &min,
				      &max, &mean, &dev))
				continue;

			snprintf(buf, sizeof(buf),
				 "%s prio %u/%u/%u",
				 clat_type,
				 ioprio_class(ts->clat_prio[ddir][i].ioprio),
				 ioprio(ts->clat_prio[ddir][i].ioprio),
				 ioprio_hint(ts->clat_prio[ddir][i].ioprio));
			display_lat(buf, min, max, mean, dev, out);
		}
	}

	if (ts->slat_percentiles && ts->slat_stat[ddir].samples > 0)
		show_clat_percentiles(ts->io_u_plat[FIO_SLAT][ddir],
					ts->slat_stat[ddir].samples,
					ts->percentile_list,
					ts->percentile_precision, "slat", out);
	if (ts->clat_percentiles && ts->clat_stat[ddir].samples > 0)
		show_clat_percentiles(ts->io_u_plat[FIO_CLAT][ddir],
					ts->clat_stat[ddir].samples,
					ts->percentile_list,
					ts->percentile_precision, "clat", out);
	if (ts->lat_percentiles && ts->lat_stat[ddir].samples > 0)
		show_clat_percentiles(ts->io_u_plat[FIO_LAT][ddir],
					ts->lat_stat[ddir].samples,
					ts->percentile_list,
					ts->percentile_precision, "lat", out);

	if (ts->clat_percentiles || ts->lat_percentiles) {
		char prio_name[64];
		uint64_t samples;

		if (ts->lat_percentiles)
			samples = ts->lat_stat[ddir].samples;
		else
			samples = ts->clat_stat[ddir].samples;

		/* Only print per prio stats if there are >= 2 prios with samples */
		if (get_nr_prios_with_samples(ts, ddir) >= 2) {
			for (i = 0; i < ts->nr_clat_prio[ddir]; i++) {
				uint64_t prio_samples =
					ts->clat_prio[ddir][i].clat_stat.samples;

				if (!prio_samples)
					continue;

				snprintf(prio_name, sizeof(prio_name),
					 "%s prio %u/%u/%u (%.2f%% of IOs)",
					 clat_type,
					 ioprio_class(ts->clat_prio[ddir][i].ioprio),
					 ioprio(ts->clat_prio[ddir][i].ioprio),
					 ioprio_hint(ts->clat_prio[ddir][i].ioprio),
					 100. * (double) prio_samples / (double) samples);
				show_clat_percentiles(ts->clat_prio[ddir][i].io_u_plat,
						prio_samples, ts->percentile_list,
						ts->percentile_precision,
						prio_name, out);
			}
		}
	}

	if (calc_lat(&ts->bw_stat[ddir], &min, &max, &mean, &dev)) {
		double p_of_agg = 100.0, fkb_base = (double)rs->kb_base;
		const char *bw_str;

		if ((rs->unit_base == 1) && i2p)
			bw_str = "Kibit";
		else if (rs->unit_base == 1)
			bw_str = "kbit";
		else if (i2p)
			bw_str = "KiB";
		else
			bw_str = "kB";

		p_of_agg = convert_agg_kbytes_percent(rs, ddir, mean);

		if (rs->unit_base == 1) {
			min *= 8.0;
			max *= 8.0;
			mean *= 8.0;
			dev *= 8.0;
		}

		if (mean > fkb_base * fkb_base) {
			min /= fkb_base;
			max /= fkb_base;
			mean /= fkb_base;
			dev /= fkb_base;
			bw_str = (rs->unit_base == 1 ? "Mibit" : "MiB");
		}

		log_buf(out, "   bw (%5s/s): min=%5llu, max=%5llu, per=%3.2f%%, "
			"avg=%5.02f, stdev=%5.02f, samples=%" PRIu64 "\n",
			bw_str, min, max, p_of_agg, mean, dev,
			(&ts->bw_stat[ddir])->samples);
	}
	if (calc_lat(&ts->iops_stat[ddir], &min, &max, &mean, &dev)) {
		log_buf(out, "   iops        : min=%5llu, max=%5llu, "
			"avg=%5.02f, stdev=%5.02f, samples=%" PRIu64 "\n",
			min, max, mean, dev, (&ts->iops_stat[ddir])->samples);
	}
}

/*
 * [한국어]
 * show_mixed_ddir_status - MIXED(세 방향 합산) 통계 라인 출력
 *
 * gen_mixed_ddir_stats_from_ts 로 임시 thread_stat 을 만들고 show_ddir_status 호출 후 해제.
 */
static void show_mixed_ddir_status(const struct group_run_stats *rs,
				   const struct thread_stat *ts,
				   struct buf_output *out)
{
	struct thread_stat *ts_lcl = gen_mixed_ddir_stats_from_ts(ts);

	if (ts_lcl)
		show_ddir_status(rs, ts_lcl, DDIR_READ, out);

	free_clat_prio_stats(ts_lcl);
	free(ts_lcl);
}

/*
 * [한국어]
 * show_lat - ns/us/ms 레이턴시 구간별 분포를 한 줄당 최대 5항목씩 출력 (내부 공통)
 *
 * @io_u_lat: 각 구간의 퍼센트 배열.
 * @nr: 구간 개수.
 * @ranges: 구간 라벨 배열 (예: {"2=","4=","10=",...}).
 * @msg: 단위 이름 ("nsec"/"usec"/"msec").
 * @out: 출력 버퍼.
 * @return: 항상 true (호환성 유지용).
 *
 * 0 이하 항목은 건너뛰고, 줄당 5개 항목씩 묶어 "lat (usec): 2=1.23%, 4=4.56%, ..." 형식 출력.
 */
static bool show_lat(const double *io_u_lat, int nr, const char **ranges,
		     const char *msg, struct buf_output *out)
{
	bool new_line = true, shown = false;
	int i, line = 0;

	for (i = 0; i < nr; i++) {
		if (io_u_lat[i] <= 0.0)
			continue;
		shown = true;
		if (new_line) {
			if (line)
				log_buf(out, "\n");
			log_buf(out, "  lat (%s)   : ", msg);
			new_line = false;
			line = 0;
		}
		if (line)
			log_buf(out, ", ");
		log_buf(out, "%s%3.2f%%", ranges[i], io_u_lat[i]);
		line++;
		if (line == 5)
			new_line = true;
	}

	if (shown)
		log_buf(out, "\n");

	return true;
}

/* [한국어] show_lat_n - ns 분포 10개 버킷(2..1000 ns) 출력 */
static void show_lat_n(const double *io_u_lat_n, struct buf_output *out)
{
	const char *ranges[] = { "2=", "4=", "10=", "20=", "50=", "100=",
				 "250=", "500=", "750=", "1000=", };

	show_lat(io_u_lat_n, FIO_IO_U_LAT_N_NR, ranges, "nsec", out);
}

/* [한국어] show_lat_u - us 분포 10개 버킷(2..1000 us) 출력 */
static void show_lat_u(const double *io_u_lat_u, struct buf_output *out)
{
	const char *ranges[] = { "2=", "4=", "10=", "20=", "50=", "100=",
				 "250=", "500=", "750=", "1000=", };

	show_lat(io_u_lat_u, FIO_IO_U_LAT_U_NR, ranges, "usec", out);
}

/* [한국어] show_lat_m - ms 분포 12개 버킷(2..>=2000 ms) 출력 */
static void show_lat_m(const double *io_u_lat_m, struct buf_output *out)
{
	const char *ranges[] = { "2=", "4=", "10=", "20=", "50=", "100=",
				 "250=", "500=", "750=", "1000=", "2000=",
				 ">=2000=", };

	show_lat(io_u_lat_m, FIO_IO_U_LAT_M_NR, ranges, "msec", out);
}

/*
 * [한국어]
 * show_latencies - 레이턴시 분포 3종(ns/us/ms) 을 한 번에 출력
 *
 * @ts: thread_stat.
 * @out: 출력 버퍼.
 *
 * stat_calc_lat_{n,u,m} 으로 버킷을 퍼센트로 변환 후 show_lat_{n,u,m} 호출.
 */
static void show_latencies(const struct thread_stat *ts, struct buf_output *out)
{
	double io_u_lat_n[FIO_IO_U_LAT_N_NR];
	double io_u_lat_u[FIO_IO_U_LAT_U_NR];
	double io_u_lat_m[FIO_IO_U_LAT_M_NR];

	stat_calc_lat_n(ts, io_u_lat_n);
	stat_calc_lat_u(ts, io_u_lat_u);
	stat_calc_lat_m(ts, io_u_lat_m);

	show_lat_n(io_u_lat_n, out);
	show_lat_u(io_u_lat_u, out);
	show_lat_m(io_u_lat_m, out);
}

/*
 * [한국어]
 * block_state_category - BLOCK_STATE_* 값을 3개 카테고리로 분류
 *
 * @block_state: BLOCK_STATE_UNINIT/WRITTEN/TRIMMED/WRITE_FAILURE/TRIM_FAILURE.
 * @return: 0=uninit, 1=written/trimmed(성공), 2=실패. 알 수 없는 값은 assert 로 abort.
 *
 * block_info (--experimental_verify 등) 의 lifetime 퍼센타일 정렬 키의 primary key 로 사용.
 */
static int block_state_category(int block_state)
{
	switch (block_state) {
	case BLOCK_STATE_UNINIT:
		return 0;
	case BLOCK_STATE_TRIMMED:
	case BLOCK_STATE_WRITTEN:
		return 1;
	case BLOCK_STATE_WRITE_FAILURE:
	case BLOCK_STATE_TRIM_FAILURE:
		return 2;
	default:
		/* Silence compile warning on some BSDs and have a return */
		assert(0);
		return -1;
	}
}

/*
 * [한국어]
 * compare_block_infos - qsort 비교 함수: category → cycles(트림 횟수) → state 순 정렬
 *
 * block_infos 배열을 lifetime 순으로 정렬해 퍼센타일 계산에 사용한다.
 */
static int compare_block_infos(const void *bs1, const void *bs2)
{
	uint64_t block1 = *(uint64_t *)bs1;
	uint64_t block2 = *(uint64_t *)bs2;
	int state1 = BLOCK_INFO_STATE(block1);
	int state2 = BLOCK_INFO_STATE(block2);
	int bscat1 = block_state_category(state1);
	int bscat2 = block_state_category(state2);
	int cycles1 = BLOCK_INFO_TRIMS(block1);
	int cycles2 = BLOCK_INFO_TRIMS(block2);

	if (bscat1 < bscat2)
		return -1;
	if (bscat1 > bscat2)
		return 1;

	if (cycles1 < cycles2)
		return -1;
	if (cycles1 > cycles2)
		return 1;

	if (state1 < state2)
		return -1;
	if (state1 > state2)
		return 1;

	assert(block1 == block2);
	return 0;
}

/*
 * [한국어]
 * calc_block_percentiles - block_info 의 lifetime 퍼센타일 계산
 *
 * @nr_block_infos: 블록 수.
 * @block_infos: 정렬 대상 배열 (in-place).
 * @plist: 퍼센타일 리스트.
 * @percentiles: 출력 — 각 퍼센타일의 트림 횟수.
 * @types: 출력 — BLOCK_STATE_* 별 블록 수.
 * @return: 퍼센타일 수.
 *
 * uninit 블록은 퍼센타일 계산에서 제외(실제 써진 블록만 대상).
 */
static int calc_block_percentiles(int nr_block_infos, uint32_t *block_infos,
				  fio_fp64_t *plist, unsigned int **percentiles,
				  unsigned int *types)
{
	int len = 0;
	int i, nr_uninit;

	qsort(block_infos, nr_block_infos, sizeof(uint32_t), compare_block_infos);

	while (len < FIO_IO_U_LIST_MAX_LEN && plist[len].u.f != 0.0)
		len++;

	if (!len)
		return 0;

	/*
	 * Sort the percentile list. Note that it may already be sorted if
	 * we are using the default values, but since it's a short list this
	 * isn't a worry. Also note that this does not work for NaN values.
	 */
	if (len > 1)
		qsort(plist, len, sizeof(plist[0]), double_cmp);

	/* Start only after the uninit entries end */
	for (nr_uninit = 0;
	     nr_uninit < nr_block_infos
		&& BLOCK_INFO_STATE(block_infos[nr_uninit]) == BLOCK_STATE_UNINIT;
	     nr_uninit ++)
		;

	if (nr_uninit == nr_block_infos)
		return 0;

	*percentiles = calloc(len, sizeof(**percentiles));

	for (i = 0; i < len; i++) {
		int idx = (plist[i].u.f * (nr_block_infos - nr_uninit) / 100)
				+ nr_uninit;
		(*percentiles)[i] = BLOCK_INFO_TRIMS(block_infos[idx]);
	}

	memset(types, 0, sizeof(*types) * BLOCK_STATE_COUNT);
	for (i = 0; i < nr_block_infos; i++)
		types[BLOCK_INFO_STATE(block_infos[i])]++;

	return len;
}

static const char *block_state_names[] = {
	[BLOCK_STATE_UNINIT] = "unwritten",
	[BLOCK_STATE_TRIMMED] = "trimmed",
	[BLOCK_STATE_WRITTEN] = "written",
	[BLOCK_STATE_TRIM_FAILURE] = "trim failure",
	[BLOCK_STATE_WRITE_FAILURE] = "write failure",
};

/*
 * [한국어]
 * show_block_infos - 블록 lifetime 퍼센타일 및 상태별 카운트 출력
 *
 * experimental_verify 등에서 블록별 트림/쓰기 이력 추적 시 출력되는 부가 정보.
 */
static void show_block_infos(int nr_block_infos, uint32_t *block_infos,
			     fio_fp64_t *plist, struct buf_output *out)
{
	int len, pos, i;
	unsigned int *percentiles = NULL;
	unsigned int block_state_counts[BLOCK_STATE_COUNT];

	len = calc_block_percentiles(nr_block_infos, block_infos, plist,
				     &percentiles, block_state_counts);

	log_buf(out, "  block lifetime percentiles :\n   |");
	pos = 0;
	for (i = 0; i < len; i++) {
		uint32_t block_info = percentiles[i];
#define LINE_LENGTH	75
		char str[LINE_LENGTH];
		int strln = snprintf(str, LINE_LENGTH, " %3.2fth=%u%c",
				     plist[i].u.f, block_info,
				     i == len - 1 ? '\n' : ',');
		assert(strln < LINE_LENGTH);
		if (pos + strln > LINE_LENGTH) {
			pos = 0;
			log_buf(out, "\n   |");
		}
		log_buf(out, "%s", str);
		pos += strln;
#undef LINE_LENGTH
	}
	if (percentiles)
		free(percentiles);

	log_buf(out, "        states               :");
	for (i = 0; i < BLOCK_STATE_COUNT; i++)
		log_buf(out, " %s=%u%c",
			 block_state_names[i], block_state_counts[i],
			 i == BLOCK_STATE_COUNT - 1 ? '\n' : ',');
}

/*
 * [한국어]
 * show_ss_normal - Steady State 판정 결과를 Normal 포맷으로 출력
 *
 * @ts: thread_stat — ss_dur/ss_state/ss_criterion/ss_deviation 필드 사용.
 * @out: 출력 버퍼.
 *
 * Steady State 모드: 일정 기간(ss_dur) 동안 측정치의 기울기(--slope) 또는 평균편차
 * (--mean_dev) 가 기준(ss_criterion) 이하이면 "attained" 로 판정. 판정 후 잡이
 * 일정 시간(ss_ramp_time) 후 종료되도록 하여 "정상 상태" 측정을 보장한다.
 * 출력 예: "steadystate : attained=yes, bw=512KiB/s (524.3KB/s), iops=128, iops slope=0.123%"
 */
static void show_ss_normal(const struct thread_stat *ts, struct buf_output *out)
{
	char *p1, *p1alt, *p2, *p3 = NULL;
	unsigned long long bw_mean, iops_mean, lat_mean;
	const int i2p = is_power_of_2(ts->kb_base);

	if (!ts->ss_dur)
		return;

	bw_mean = steadystate_bw_mean(ts);
	iops_mean = steadystate_iops_mean(ts);
	lat_mean = steadystate_lat_mean(ts);

	p1 = num2str(bw_mean / ts->kb_base, ts->sig_figs, ts->kb_base, i2p, ts->unit_base);
	p1alt = num2str(bw_mean / ts->kb_base, ts->sig_figs, ts->kb_base, !i2p, ts->unit_base);
	p2 = num2str(iops_mean, ts->sig_figs, 1, 0, N2S_NONE);
	if (ts->ss_state & FIO_SS_LAT) {
		const char *lat_unit = "nsec";
		unsigned long long lat_val = lat_mean;
		double lat_mean_d = lat_mean, lat_dev_d = 0.0;
		char *lat_num;

		if (nsec_to_msec(&lat_val, &lat_val, &lat_mean_d, &lat_dev_d))
			lat_unit = "msec";
		else if (nsec_to_usec(&lat_val, &lat_val, &lat_mean_d, &lat_dev_d))
			lat_unit = "usec";

		lat_num = num2str((unsigned long long)lat_mean_d, ts->sig_figs, 1, 0, N2S_NONE);
		if (asprintf(&p3, "%s%s", lat_num, lat_unit) < 0)
			p3 = NULL;
		free(lat_num);
	}

	log_buf(out, "  steadystate  : attained=%s, bw=%s (%s), iops=%s%s%s, %s%s=%.3f%s\n",
		ts->ss_state & FIO_SS_ATTAINED ? "yes" : "no",
		p1, p1alt, p2,
		p3 ? ", lat=" : "",
		p3 ? p3 : "",
		ts->ss_state & FIO_SS_IOPS ? "iops" : (ts->ss_state & FIO_SS_LAT ? "lat" : "bw"),
		ts->ss_state & FIO_SS_SLOPE ? " slope": " mean dev",
		ts->ss_criterion.u.f,
		ts->ss_state & FIO_SS_PCT ? "%" : "");

	free(p1);
	free(p1alt);
	free(p2);
	free(p3);
}

/*
 * [한국어]
 * show_agg_stats - md/dm 등 슬레이브 장치 집계 통계 출력 (normal/terse 두 모드)
 *
 * slavecount 로 나눠 평균 산출. terse 는 세미콜론 구분 CSV, 아니면 사람 가독 문자열.
 */
static void show_agg_stats(const struct disk_util_agg *agg, int terse,
			   struct buf_output *out)
{
	if (!agg->slavecount)
		return;

	if (!terse) {
		log_buf(out, ", aggrios=%llu/%llu, aggsectors=%llu/%llu, "
			 "aggrmerge=%llu/%llu, aggrticks=%llu/%llu, "
			 "aggrin_queue=%llu, aggrutil=%3.2f%%",
			(unsigned long long) agg->ios[0] / agg->slavecount,
			(unsigned long long) agg->ios[1] / agg->slavecount,
			(unsigned long long) agg->sectors[0] / agg->slavecount,
			(unsigned long long) agg->sectors[1] / agg->slavecount,
			(unsigned long long) agg->merges[0] / agg->slavecount,
			(unsigned long long) agg->merges[1] / agg->slavecount,
			(unsigned long long) agg->ticks[0] / agg->slavecount,
			(unsigned long long) agg->ticks[1] / agg->slavecount,
			(unsigned long long) agg->time_in_queue / agg->slavecount,
			agg->max_util.u.f);
	} else {
		log_buf(out, ";slaves;%llu;%llu;%llu;%llu;%llu;%llu;%llu;%3.2f%%",
			(unsigned long long) agg->ios[0] / agg->slavecount,
			(unsigned long long) agg->ios[1] / agg->slavecount,
			(unsigned long long) agg->merges[0] / agg->slavecount,
			(unsigned long long) agg->merges[1] / agg->slavecount,
			(unsigned long long) agg->ticks[0] / agg->slavecount,
			(unsigned long long) agg->ticks[1] / agg->slavecount,
			(unsigned long long) agg->time_in_queue / agg->slavecount,
			agg->max_util.u.f);
	}
}

/*
 * [한국어]
 * aggregate_slaves_stats - md/dm 등 마스터 장치의 하위 슬레이브 디스크 util 를 합산
 *
 * @masterdu: 마스터 disk_util (sliest 가 슬레이브 disk_util 리스트).
 *
 * max_util 은 슬레이브 중 가장 높은 활용률 → 시스템 병목 판정에 사용.
 * (평균 아닌 최대 활용률이 의미 있는 것은 가장 바쁜 구성 요소가 병목이기 때문)
 */
static void aggregate_slaves_stats(struct disk_util *masterdu)
{
	struct disk_util_agg *agg = &masterdu->agg;
	struct disk_util_stat *dus;
	struct flist_head *entry;
	struct disk_util *slavedu;
	double util;

	flist_for_each(entry, &masterdu->slaves) {
		slavedu = flist_entry(entry, struct disk_util, slavelist);
		dus = &slavedu->dus;
		agg->ios[0] += dus->s.ios[0];
		agg->ios[1] += dus->s.ios[1];
		agg->merges[0] += dus->s.merges[0];
		agg->merges[1] += dus->s.merges[1];
		agg->sectors[0] += dus->s.sectors[0];
		agg->sectors[1] += dus->s.sectors[1];
		agg->ticks[0] += dus->s.ticks[0];
		agg->ticks[1] += dus->s.ticks[1];
		agg->time_in_queue += dus->s.time_in_queue;
		agg->slavecount++;

		util = (double) (100 * dus->s.io_ticks / (double) slavedu->dus.s.msec);
		/* System utilization is the utilization of the
		 * component with the highest utilization.
		 */
		if (util > agg->max_util.u.f)
			agg->max_util.u.f = util;

	}

	if (agg->max_util.u.f > 100.0)
		agg->max_util.u.f = 100.0;
}

/*
 * [한국어]
 * print_disk_util - 디스크 유틸리티(iostat-like) 통계 출력 (normal/terse)
 *
 * @dus: disk_util_stat — /proc/diskstats 1 라인의 파싱 결과 (io/merge/tick/queue_time).
 * @agg: 슬레이브 집계.
 * @terse: 1=terse CSV, 0=사람 가독.
 * @out: 출력 버퍼.
 *
 * util = 100 * io_ticks / msec — 디바이스가 I/O 를 처리하느라 바빴던 시간 비율.
 * ios[0]=read count, ios[1]=write count, sectors/merges/ticks 도 R/W 분리.
 * time_in_queue 는 /proc/diskstats 의 11번 필드(weighted IO time).
 */
void print_disk_util(const struct disk_util_stat *dus, const struct disk_util_agg *agg,
		     int terse, struct buf_output *out)
{
	double util = 0;

	if (dus->s.msec)
		util = (double) 100 * dus->s.io_ticks / (double) dus->s.msec;
	if (util > 100.0)
		util = 100.0;

	if (!terse) {
		if (agg->slavecount)
			log_buf(out, "  ");

		log_buf(out, "  %s: ios=%llu/%llu, sectors=%llu/%llu, "
			"merge=%llu/%llu, ticks=%llu/%llu, in_queue=%llu, "
			"util=%3.2f%%",
				dus->name,
				(unsigned long long) dus->s.ios[0],
				(unsigned long long) dus->s.ios[1],
				(unsigned long long) dus->s.sectors[0],
				(unsigned long long) dus->s.sectors[1],
				(unsigned long long) dus->s.merges[0],
				(unsigned long long) dus->s.merges[1],
				(unsigned long long) dus->s.ticks[0],
				(unsigned long long) dus->s.ticks[1],
				(unsigned long long) dus->s.time_in_queue,
				util);
	} else {
		log_buf(out, ";%s;%llu;%llu;%llu;%llu;%llu;%llu;%llu;%3.2f%%",
				dus->name,
				(unsigned long long) dus->s.ios[0],
				(unsigned long long) dus->s.ios[1],
				(unsigned long long) dus->s.merges[0],
				(unsigned long long) dus->s.merges[1],
				(unsigned long long) dus->s.ticks[0],
				(unsigned long long) dus->s.ticks[1],
				(unsigned long long) dus->s.time_in_queue,
				util);
	}

	/*
	 * If the device has slaves, aggregate the stats for
	 * those slave devices also.
	 */
	show_agg_stats(agg, terse, out);

	if (!terse)
		log_buf(out, "\n");
}

/*
 * [한국어]
 * json_array_add_disk_util - 디스크 유틸을 JSON 배열 엔트리로 추가
 *
 * @dus: disk_util_stat.
 * @agg: 슬레이브 집계 (0 이면 생략).
 * @array: 추가 대상 JSON 배열.
 *
 * JSON 출력 모드에서 "disk_util": [ { ... }, ... ] 섹션 구성에 사용.
 */
void json_array_add_disk_util(const struct disk_util_stat *dus,
			      const struct disk_util_agg *agg, struct json_array *array)
{
	struct json_object *obj;
	double util = 0;

	if (dus->s.msec)
		util = (double) 100 * dus->s.io_ticks / (double) dus->s.msec;
	if (util > 100.0)
		util = 100.0;

	obj = json_create_object();
	json_array_add_value_object(array, obj);

	json_object_add_value_string(obj, "name", (const char *)dus->name);
	json_object_add_value_int(obj, "read_ios", dus->s.ios[0]);
	json_object_add_value_int(obj, "write_ios", dus->s.ios[1]);
	json_object_add_value_int(obj, "read_sectors", dus->s.sectors[0]);
	json_object_add_value_int(obj, "write_sectors", dus->s.sectors[1]);
	json_object_add_value_int(obj, "read_merges", dus->s.merges[0]);
	json_object_add_value_int(obj, "write_merges", dus->s.merges[1]);
	json_object_add_value_int(obj, "read_ticks", dus->s.ticks[0]);
	json_object_add_value_int(obj, "write_ticks", dus->s.ticks[1]);
	json_object_add_value_int(obj, "in_queue", dus->s.time_in_queue);
	json_object_add_value_float(obj, "util", util);

	/*
	 * If the device has slaves, aggregate the stats for
	 * those slave devices also.
	 */
	if (!agg->slavecount)
		return;
	json_object_add_value_int(obj, "aggr_read_ios",
				agg->ios[0] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_write_ios",
				agg->ios[1] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_read_sectors",
				agg->sectors[0] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_write_sectors",
				agg->sectors[1] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_read_merges",
				agg->merges[0] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_write_merge",
				agg->merges[1] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_read_ticks",
				agg->ticks[0] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_write_ticks",
				agg->ticks[1] / agg->slavecount);
	json_object_add_value_int(obj, "aggr_in_queue",
				agg->time_in_queue / agg->slavecount);
	json_object_add_value_float(obj, "aggr_util", agg->max_util.u.f);
}

/*
 * [한국어]
 * json_object_add_disk_utils - JSON obj 에 "disk_util" 배열을 추가하고 모든 disk_util 을 순회
 */
static void json_object_add_disk_utils(struct json_object *obj,
				       struct flist_head *head)
{
	struct json_array *array = json_create_array();
	struct flist_head *entry;
	struct disk_util *du;

	json_object_add_value_array(obj, "disk_util", array);

	flist_for_each(entry, head) {
		du = flist_entry(entry, struct disk_util, list);

		aggregate_slaves_stats(du);
		json_array_add_disk_util(&du->dus, &du->agg, array);
	}
}

/*
 * [한국어]
 * show_disk_util - 디스크 유틸 섹션 전체 출력 디스패처
 *
 * @terse: 1=terse, 0=normal.
 * @parent: JSON parent obj (JSON 출력 시).
 * @out: 텍스트 출력 버퍼.
 *
 * output_format 비트에 따라 JSON 경로 / print_disk_util 루프 중 하나로 분기.
 */
static void show_disk_util(int terse, struct json_object *parent,
			   struct buf_output *out)
{
	struct flist_head *entry;
	struct disk_util *du;
	bool do_json;

	if (!is_running_backend())
		return;

	if (flist_empty(&disk_list))
		return;

	if ((output_format & FIO_OUTPUT_JSON) && parent)
		do_json = true;
	else
		do_json = false;

	if (!terse && !do_json)
		log_buf(out, "\nDisk stats (read/write):\n");

	if (do_json) {
		json_object_add_disk_utils(parent, &disk_list);
	} else if (output_format & ~(FIO_OUTPUT_JSON | FIO_OUTPUT_JSON_PLUS)) {
		flist_for_each(entry, &disk_list) {
			du = flist_entry(entry, struct disk_util, list);

			aggregate_slaves_stats(du);
			print_disk_util(&du->dus, &du->agg, terse, out);
		}
	}
}

/*
 * [한국어]
 * scale_down_ns - nsec 값을 자동 단위 변환 (ns/us/ms/s)
 *
 * @val: 입력 ns.
 * @unit: 출력 단위 문자열 포인터 ("ns"/"us"/"ms"/"s").
 * @return: 변환된 값 (double).
 *
 * 임계값 2000/2000000/2000000000 — 가독성을 위해 최소 2 단위 이상 크기일 때만 upscale.
 */
static double scale_down_ns(unsigned long long val, const char **unit)
{
	double retval;

	if (val >= 2000000000) {
		retval = (double) val / 1000000000.0;
		*unit = "s";
	} else if (val >= 2000000) {
		retval = (double) val / 1000000.0;
		*unit = "ms";
	} else if (val >= 2000) {
		retval = (double) val / 1000.0;
		*unit = "us";
	} else {
		retval = (double) val;
		*unit = "ns";
	}

	return retval;
}

/*
 * [한국어]
 * show_thread_status_normal - 잡 하나의 Normal 포맷 전체 보고서 생성
 *
 * @ts: 잡 thread_stat.
 * @rs: 그룹 run stats.
 * @out: 출력 버퍼.
 *
 * 출력 구성:
 *   1. 헤더: jobname, groupid, jobs, err, pid, 시각, description
 *   2. for_each_rw_ddir: show_ddir_status 로 방향별 통계
 *   3. unified_rw_rep=both: show_mixed_ddir_status
 *   4. show_latencies: ns/us/ms 분포
 *   5. sync_stat 이 있으면 sync 통계
 *   6. cpu 사용률 (usr%, sys%, ctx, majf, minf)
 *   7. IO depths 분포 (io_u_map 기반)
 *   8. submit/complete 분포
 *   9. issued rwts / short / dropped
 *  10. (continue_on_error) errors
 *  11. (latency_depth) latency target / window
 *  12. (nr_block_infos) block lifetime percentiles
 *  13. (ss_dur) steady state 결과
 *
 * 실행 컨텍스트: 최종 보고서 생성 — 메인 프로세스.
 *
 * 호출 체인:
 *   __show_run_stats → [show_thread_status_normal] → show_ddir_status/show_latencies/...
 */
static void show_thread_status_normal(struct thread_stat *ts,
				      const struct group_run_stats *rs,
				      struct buf_output *out)
{
	double usr_cpu, sys_cpu;
	unsigned long runtime;
	double io_u_dist[FIO_IO_U_MAP_NR];
	time_t time_p;
	char time_buf[32];

	if (!ddir_rw_sum(ts->io_bytes) && !ddir_rw_sum(ts->total_io_u))
		return;

	memset(time_buf, 0, sizeof(time_buf));

	time(&time_p);
	os_ctime_r((const time_t *) &time_p, time_buf, sizeof(time_buf));

	if (!ts->error) {
		log_buf(out, "%s: (groupid=%d, jobs=%d): err=%2d: pid=%d: %s",
					ts->name, ts->groupid, ts->members,
					ts->error, (int) ts->pid, time_buf);
	} else {
		log_buf(out, "%s: (groupid=%d, jobs=%d): err=%2d (%s): pid=%d: %s",
					ts->name, ts->groupid, ts->members,
					ts->error, ts->verror, (int) ts->pid,
					time_buf);
	}

	if (strlen(ts->description))
		log_buf(out, "  Description  : [%s]\n", ts->description);

	for_each_rw_ddir(ddir) {
		if (ts->io_bytes[ddir])
			show_ddir_status(rs, ts, ddir, out);
	}

	if (ts->unified_rw_rep == UNIFIED_BOTH)
		show_mixed_ddir_status(rs, ts, out);

	show_latencies(ts, out);

	if (ts->sync_stat.samples)
		show_ddir_status(rs, ts, DDIR_SYNC, out);

	runtime = ts->total_run_time;
	if (runtime) {
		double runt = (double) runtime;

		usr_cpu = (double) ts->usr_time * 100 / runt;
		sys_cpu = (double) ts->sys_time * 100 / runt;
	} else {
		usr_cpu = 0;
		sys_cpu = 0;
	}

	log_buf(out, "  cpu          : usr=%3.2f%%, sys=%3.2f%%, ctx=%llu,"
		 " majf=%llu, minf=%llu\n", usr_cpu, sys_cpu,
			(unsigned long long) ts->ctx,
			(unsigned long long) ts->majf,
			(unsigned long long) ts->minf);

	stat_calc_dist(ts->io_u_map, ddir_rw_sum(ts->total_io_u), io_u_dist);
	log_buf(out, "  IO depths    : 1=%3.1f%%, 2=%3.1f%%, 4=%3.1f%%, 8=%3.1f%%,"
		 " 16=%3.1f%%, 32=%3.1f%%, >=64=%3.1f%%\n", io_u_dist[0],
					io_u_dist[1], io_u_dist[2],
					io_u_dist[3], io_u_dist[4],
					io_u_dist[5], io_u_dist[6]);

	stat_calc_dist(ts->io_u_submit, ts->total_submit, io_u_dist);
	log_buf(out, "     submit    : 0=%3.1f%%, 4=%3.1f%%, 8=%3.1f%%, 16=%3.1f%%,"
		 " 32=%3.1f%%, 64=%3.1f%%, >=64=%3.1f%%\n", io_u_dist[0],
					io_u_dist[1], io_u_dist[2],
					io_u_dist[3], io_u_dist[4],
					io_u_dist[5], io_u_dist[6]);
	stat_calc_dist(ts->io_u_complete, ts->total_complete, io_u_dist);
	log_buf(out, "     complete  : 0=%3.1f%%, 4=%3.1f%%, 8=%3.1f%%, 16=%3.1f%%,"
		 " 32=%3.1f%%, 64=%3.1f%%, >=64=%3.1f%%\n", io_u_dist[0],
					io_u_dist[1], io_u_dist[2],
					io_u_dist[3], io_u_dist[4],
					io_u_dist[5], io_u_dist[6]);
	log_buf(out, "     issued rwts: total=%llu,%llu,%llu,%llu"
				 " short=%llu,%llu,%llu,0"
				 " dropped=%llu,%llu,%llu,0\n",
					(unsigned long long) ts->total_io_u[0],
					(unsigned long long) ts->total_io_u[1],
					(unsigned long long) ts->total_io_u[2],
					(unsigned long long) ts->total_io_u[3],
					(unsigned long long) ts->short_io_u[0],
					(unsigned long long) ts->short_io_u[1],
					(unsigned long long) ts->short_io_u[2],
					(unsigned long long) ts->drop_io_u[0],
					(unsigned long long) ts->drop_io_u[1],
					(unsigned long long) ts->drop_io_u[2]);
	if (ts->continue_on_error) {
		log_buf(out, "     errors    : total=%llu, first_error=%d/<%s>\n",
					(unsigned long long)ts->total_err_count,
					ts->first_error,
					strerror(ts->first_error));
	}
	if (ts->latency_depth) {
		double target, window;
		const char *target_unit, *window_unit;

		target = scale_down_ns(ts->latency_target, &target_unit);
		window = scale_down_ns(ts->latency_window*1000, &window_unit);
		log_buf(out, "     latency   : target=%.2f%s, window=%.2f%s, percentile=%.2f%%, depth=%u\n",
					target, target_unit,
					window, window_unit,
					ts->latency_percentile.u.f,
					ts->latency_depth);
	}

	if (ts->nr_block_infos)
		show_block_infos(ts->nr_block_infos, ts->block_infos,
				  ts->percentile_list, out);

	if (ts->ss_dur)
		show_ss_normal(ts, out);
}

/*
 * [한국어]
 * show_ddir_status_terse - 방향별 통계를 terse(CSV-like) 형식으로 출력
 *
 * @ts: thread_stat.
 * @rs: 그룹 run stats.
 * @ddir: 방향.
 * @ver: terse 버전 (2/3/4/5) — 필드 레이아웃 차이.
 * @out: 출력 버퍼.
 *
 * Terse 포맷은 세미콜론(;) 구분 CSV 로, 각 필드는 고정 순서:
 *   total_ios;total_MB;bw;iops;runtime;slat_min;slat_max;slat_mean;slat_dev;
 *   clat_min;clat_max;clat_mean;clat_dev;clat_pct_list;lat_min;lat_max;lat_mean;lat_dev;
 *   bw_min;bw_max;bw_agg_pct;bw_mean;bw_dev;iops_min;iops_max;iops_mean;iops_dev;iops_samples;
 *   (v5) lat_percentile_stats
 *
 * 호출 체인:
 *   show_thread_status_terse_all → [show_ddir_status_terse]
 */
static void show_ddir_status_terse(struct thread_stat *ts,
				   const struct group_run_stats *rs,
				   enum fio_ddir ddir, int ver,
				   struct buf_output *out)
{
	unsigned long long min, max, minv, maxv, bw, iops;
	unsigned long long *ovals = NULL;
	double mean, dev;
	unsigned int len;
	int i, bw_stat;

	assert(ddir_rw(ddir));

	iops = bw = 0;
	if (ts->runtime[ddir]) {
		uint64_t runt = ts->runtime[ddir];

		bw = ((1000 * ts->io_bytes[ddir]) / runt) / 1024; /* KiB/s */
		iops = (1000 * (uint64_t) ts->total_io_u[ddir]) / runt;
	}

	log_buf(out, ";%llu;%llu;%llu;%llu",
		(unsigned long long) ts->io_bytes[ddir] >> 10, bw, iops,
					(unsigned long long) ts->runtime[ddir]);

	if (calc_lat(&ts->slat_stat[ddir], &min, &max, &mean, &dev))
		log_buf(out, ";%llu;%llu;%f;%f", min/1000, max/1000, mean/1000, dev/1000);
	else
		log_buf(out, ";%llu;%llu;%f;%f", 0ULL, 0ULL, 0.0, 0.0);

	if (calc_lat(&ts->clat_stat[ddir], &min, &max, &mean, &dev))
		log_buf(out, ";%llu;%llu;%f;%f", min/1000, max/1000, mean/1000, dev/1000);
	else
		log_buf(out, ";%llu;%llu;%f;%f", 0ULL, 0ULL, 0.0, 0.0);

	if (ts->lat_percentiles) {
		len = calc_clat_percentiles(ts->io_u_plat[FIO_LAT][ddir],
					ts->lat_stat[ddir].samples,
					ts->percentile_list, &ovals, &maxv,
					&minv);
	} else if (ts->clat_percentiles) {
		len = calc_clat_percentiles(ts->io_u_plat[FIO_CLAT][ddir],
					ts->clat_stat[ddir].samples,
					ts->percentile_list, &ovals, &maxv,
					&minv);
	} else {
		len = 0;
	}

	for (i = 0; i < FIO_IO_U_LIST_MAX_LEN; i++) {
		if (i >= len) {
			log_buf(out, ";0%%=0");
			continue;
		}
		log_buf(out, ";%f%%=%llu", ts->percentile_list[i].u.f, ovals[i]/1000);
	}

	if (calc_lat(&ts->lat_stat[ddir], &min, &max, &mean, &dev))
		log_buf(out, ";%llu;%llu;%f;%f", min/1000, max/1000, mean/1000, dev/1000);
	else
		log_buf(out, ";%llu;%llu;%f;%f", 0ULL, 0ULL, 0.0, 0.0);

	free(ovals);

	bw_stat = calc_lat(&ts->bw_stat[ddir], &min, &max, &mean, &dev);
	if (bw_stat) {
		double p_of_agg = 100.0;

		if (rs->agg[ddir]) {
			p_of_agg = mean * 100 / (double) (rs->agg[ddir] / 1024);
			if (p_of_agg > 100.0)
				p_of_agg = 100.0;
		}

		log_buf(out, ";%llu;%llu;%f%%;%f;%f", min, max, p_of_agg, mean, dev);
	} else {
		log_buf(out, ";%llu;%llu;%f%%;%f;%f", 0ULL, 0ULL, 0.0, 0.0, 0.0);
	}

	if (ver == 5) {
		if (bw_stat)
			log_buf(out, ";%" PRIu64, (&ts->bw_stat[ddir])->samples);
		else
			log_buf(out, ";%lu", 0UL);

		if (calc_lat(&ts->iops_stat[ddir], &min, &max, &mean, &dev))
			log_buf(out, ";%llu;%llu;%f;%f;%" PRIu64, min, max,
				mean, dev, (&ts->iops_stat[ddir])->samples);
		else
			log_buf(out, ";%llu;%llu;%f;%f;%lu", 0ULL, 0ULL, 0.0, 0.0, 0UL);
	}
}

/*
 * [한국어]
 * show_mixed_ddir_status_terse - MIXED 통계를 terse 형식으로 출력
 *
 * gen_mixed_ddir_stats_from_ts 로 임시 ts 를 만든 뒤 show_ddir_status_terse 호출.
 */
static void show_mixed_ddir_status_terse(const struct thread_stat *ts,
					 const struct group_run_stats *rs,
					 int ver, struct buf_output *out)
{
	struct thread_stat *ts_lcl = gen_mixed_ddir_stats_from_ts(ts);

	if (ts_lcl)
		show_ddir_status_terse(ts_lcl, rs, DDIR_READ, ver, out);

	free_clat_prio_stats(ts_lcl);
	free(ts_lcl);
}

/*
 * [한국어]
 * add_ddir_lat_json - 레이턴시 통계(slat/clat/lat) 하나를 JSON 객체로 변환
 *
 * @ts: thread_stat.
 * @percentiles: 퍼센타일 출력 on/off 비트.
 * @lat_stat: Welford 누적기.
 * @io_u_plat: 히스토그램 버킷.
 * @return: JSON 객체 ({min, max, mean, stddev, N, percentile: {...}, bins: {...}}).
 *
 * JSON_PLUS 옵션 시 bins 원시 버킷 카운트 배열도 포함 (플로팅 도구용).
 */
static struct json_object *add_ddir_lat_json(struct thread_stat *ts,
					     uint32_t percentiles,
					     const struct io_stat *lat_stat,
					     const uint64_t *io_u_plat)
{
	char buf[120];
	double mean, dev;
	unsigned int i, len;
	struct json_object *lat_object, *percentile_object, *clat_bins_object;
	unsigned long long min, max, maxv, minv, *ovals = NULL;

	if (!calc_lat(lat_stat, &min, &max, &mean, &dev)) {
		min = max = 0;
		mean = dev = 0.0;
	}
	lat_object = json_create_object();
	json_object_add_value_int(lat_object, "min", min);
	json_object_add_value_int(lat_object, "max", max);
	json_object_add_value_float(lat_object, "mean", mean);
	json_object_add_value_float(lat_object, "stddev", dev);
	json_object_add_value_int(lat_object, "N", lat_stat->samples);

	if (percentiles && lat_stat->samples) {
		len = calc_clat_percentiles(io_u_plat, lat_stat->samples,
				ts->percentile_list, &ovals, &maxv, &minv);

		if (len > FIO_IO_U_LIST_MAX_LEN)
			len = FIO_IO_U_LIST_MAX_LEN;

		percentile_object = json_create_object();
		json_object_add_value_object(lat_object, "percentile", percentile_object);
		for (i = 0; i < len; i++) {
			snprintf(buf, sizeof(buf), "%f", ts->percentile_list[i].u.f);
			json_object_add_value_int(percentile_object, buf, ovals[i]);
		}
		free(ovals);

		if (output_format & FIO_OUTPUT_JSON_PLUS) {
			clat_bins_object = json_create_object();
			json_object_add_value_object(lat_object, "bins", clat_bins_object);

			for(i = 0; i < FIO_IO_U_PLAT_NR; i++)
				if (io_u_plat[i]) {
					snprintf(buf, sizeof(buf), "%llu", plat_idx_to_val(i));
					json_object_add_value_int(clat_bins_object, buf, io_u_plat[i]);
				}
		}
	}

	return lat_object;
}

/*
 * [한국어]
 * add_ddir_status_json - 방향별 통계를 JSON 중첩 객체로 추가
 *
 * @ts: thread_stat.
 * @rs: 그룹 run stats.
 * @ddir: 방향.
 * @parent: 상위 JSON 객체.
 *
 * 생성 구조: parent["read"|"write"|"trim"|"sync"] = { io_bytes, io_kbytes, bw_bytes, bw, iops,
 * runtime, total_ios, short_ios, drop_ios, slat_ns, clat_ns, lat_ns, prios[], bw_min/max/agg/mean/dev,
 * iops_min/max/mean/stddev/samples, cachehit }.
 */
static void add_ddir_status_json(struct thread_stat *ts,
				 const struct group_run_stats *rs, enum fio_ddir ddir,
				 struct json_object *parent)
{
	unsigned long long min, max;
	unsigned long long bw_bytes, bw;
	double mean, dev, iops;
	struct json_object *dir_object, *tmp_object;
	double p_of_agg = 100.0;

	assert(ddir_rw(ddir) || ddir_sync(ddir));

	if ((ts->unified_rw_rep == UNIFIED_MIXED) && ddir != DDIR_READ)
		return;

	dir_object = json_create_object();
	json_object_add_value_object(parent,
		(ts->unified_rw_rep == UNIFIED_MIXED) ? "mixed" : io_ddir_name(ddir), dir_object);

	if (ddir_rw(ddir)) {
		bw_bytes = 0;
		bw = 0;
		iops = 0.0;
		if (ts->runtime[ddir]) {
			uint64_t runt = ts->runtime[ddir];

			bw_bytes = ((1000 * ts->io_bytes[ddir]) / runt); /* Bytes/s */
			bw = bw_bytes / 1024; /* KiB/s */
			iops = (1000.0 * (uint64_t) ts->total_io_u[ddir]) / runt;
		}

		json_object_add_value_int(dir_object, "io_bytes", ts->io_bytes[ddir]);
		json_object_add_value_int(dir_object, "io_kbytes", ts->io_bytes[ddir] >> 10);
		json_object_add_value_int(dir_object, "bw_bytes", bw_bytes);
		json_object_add_value_int(dir_object, "bw", bw);
		json_object_add_value_float(dir_object, "iops", iops);
		json_object_add_value_int(dir_object, "runtime", ts->runtime[ddir]);
		json_object_add_value_int(dir_object, "total_ios", ts->total_io_u[ddir]);
		json_object_add_value_int(dir_object, "short_ios", ts->short_io_u[ddir]);
		json_object_add_value_int(dir_object, "drop_ios", ts->drop_io_u[ddir]);

		tmp_object = add_ddir_lat_json(ts, ts->slat_percentiles,
				&ts->slat_stat[ddir], ts->io_u_plat[FIO_SLAT][ddir]);
		json_object_add_value_object(dir_object, "slat_ns", tmp_object);

		tmp_object = add_ddir_lat_json(ts, ts->clat_percentiles,
				&ts->clat_stat[ddir], ts->io_u_plat[FIO_CLAT][ddir]);
		json_object_add_value_object(dir_object, "clat_ns", tmp_object);

		tmp_object = add_ddir_lat_json(ts, ts->lat_percentiles,
				&ts->lat_stat[ddir], ts->io_u_plat[FIO_LAT][ddir]);
		json_object_add_value_object(dir_object, "lat_ns", tmp_object);
	} else {
		json_object_add_value_int(dir_object, "total_ios", ts->total_io_u[DDIR_SYNC]);
		tmp_object = add_ddir_lat_json(ts, ts->lat_percentiles | ts->clat_percentiles,
				&ts->sync_stat, ts->io_u_sync_plat);
		json_object_add_value_object(dir_object, "lat_ns", tmp_object);
	}

	if (!ddir_rw(ddir))
		return;

	/* Only include per prio stats if there are >= 2 prios with samples */
	if (get_nr_prios_with_samples(ts, ddir) >= 2) {
		struct json_array *array = json_create_array();
		const char *obj_name;
		int i;

		if (ts->lat_percentiles)
			obj_name = "lat_ns";
		else
			obj_name = "clat_ns";

		json_object_add_value_array(dir_object, "prios", array);

		for (i = 0; i < ts->nr_clat_prio[ddir]; i++) {
			struct json_object *obj;

			if (!ts->clat_prio[ddir][i].clat_stat.samples)
				continue;

			obj = json_create_object();

			json_object_add_value_int(obj, "prioclass",
				ioprio_class(ts->clat_prio[ddir][i].ioprio));
			json_object_add_value_int(obj, "prio",
				ioprio(ts->clat_prio[ddir][i].ioprio));
			json_object_add_value_int(obj, "priohint",
				ioprio_hint(ts->clat_prio[ddir][i].ioprio));

			tmp_object = add_ddir_lat_json(ts,
					ts->clat_percentiles | ts->lat_percentiles,
					&ts->clat_prio[ddir][i].clat_stat,
					ts->clat_prio[ddir][i].io_u_plat);
			json_object_add_value_object(obj, obj_name, tmp_object);
			json_array_add_value_object(array, obj);
		}
	}

	if (calc_lat(&ts->bw_stat[ddir], &min, &max, &mean, &dev)) {
		p_of_agg = convert_agg_kbytes_percent(rs, ddir, mean);
	} else {
		min = max = 0;
		p_of_agg = mean = dev = 0.0;
	}

	json_object_add_value_int(dir_object, "bw_min", min);
	json_object_add_value_int(dir_object, "bw_max", max);
	json_object_add_value_float(dir_object, "bw_agg", p_of_agg);
	json_object_add_value_float(dir_object, "bw_mean", mean);
	json_object_add_value_float(dir_object, "bw_dev", dev);
	json_object_add_value_int(dir_object, "bw_samples",
				(&ts->bw_stat[ddir])->samples);

	if (!calc_lat(&ts->iops_stat[ddir], &min, &max, &mean, &dev)) {
		min = max = 0;
		mean = dev = 0.0;
	}
	json_object_add_value_int(dir_object, "iops_min", min);
	json_object_add_value_int(dir_object, "iops_max", max);
	json_object_add_value_float(dir_object, "iops_mean", mean);
	json_object_add_value_float(dir_object, "iops_stddev", dev);
	json_object_add_value_int(dir_object, "iops_samples",
				(&ts->iops_stat[ddir])->samples);

	if (ts->cachehit + ts->cachemiss) {
		uint64_t total;
		double hit;

		total = ts->cachehit + ts->cachemiss;
		hit = (double) ts->cachehit / (double) total;
		hit *= 100.0;
		json_object_add_value_float(dir_object, "cachehit", hit);
	}
}

/*
 * [한국어]
 * add_mixed_ddir_status_json - MIXED 집계 JSON 객체 추가
 */
static void add_mixed_ddir_status_json(struct thread_stat *ts,
		struct group_run_stats *rs, struct json_object *parent)
{
	struct thread_stat *ts_lcl = gen_mixed_ddir_stats_from_ts(ts);

	/* add the aggregated stats to json parent */
	if (ts_lcl)
		add_ddir_status_json(ts_lcl, rs, DDIR_READ, parent);

	free_clat_prio_stats(ts_lcl);
	free(ts_lcl);
}

/*
 * [한국어]
 * show_thread_status_terse_all - Terse v2..v5 전체 한 줄 출력
 *
 * @ts: thread_stat.
 * @rs: 그룹 run stats.
 * @ver: 2/3/4/5 (버전별 필드 레이아웃).
 * @out: 출력 버퍼.
 *
 * v2: version;jobname;groupid;error;read;write;trim;cpu;depth;lat_u;lat_m
 * v3~: +fio_version;disk_util(ver>=3)
 * v4: +trim 필드 포함
 * v5: +per-ddir lat_percentile_stats
 *
 * 호출 체인:
 *   __show_run_stats → show_thread_status_terse → [show_thread_status_terse_all]
 */
static void show_thread_status_terse_all(struct thread_stat *ts,
					 struct group_run_stats *rs, int ver,
					 struct buf_output *out)
{
	double io_u_dist[FIO_IO_U_MAP_NR];
	double io_u_lat_u[FIO_IO_U_LAT_U_NR];
	double io_u_lat_m[FIO_IO_U_LAT_M_NR];
	double usr_cpu, sys_cpu;
	int i;

	/* General Info */
	if (ver == 2)
		log_buf(out, "2;%s;%d;%d", ts->name, ts->groupid, ts->error);
	else
		log_buf(out, "%d;%s;%s;%d;%d", ver, fio_version_string,
			ts->name, ts->groupid, ts->error);

	/* Log Read Status, or mixed if unified_rw_rep = 1 */
	show_ddir_status_terse(ts, rs, DDIR_READ, ver, out);
	if (ts->unified_rw_rep != UNIFIED_MIXED) {
		/* Log Write Status */
		show_ddir_status_terse(ts, rs, DDIR_WRITE, ver, out);
		/* Log Trim Status */
		if (ver == 2 || ver == 4 || ver == 5)
			show_ddir_status_terse(ts, rs, DDIR_TRIM, ver, out);
	}
	if (ts->unified_rw_rep == UNIFIED_BOTH)
		show_mixed_ddir_status_terse(ts, rs, ver, out);
	/* CPU Usage */
	if (ts->total_run_time) {
		double runt = (double) ts->total_run_time;

		usr_cpu = (double) ts->usr_time * 100 / runt;
		sys_cpu = (double) ts->sys_time * 100 / runt;
	} else {
		usr_cpu = 0;
		sys_cpu = 0;
	}

	log_buf(out, ";%f%%;%f%%;%llu;%llu;%llu", usr_cpu, sys_cpu,
						(unsigned long long) ts->ctx,
						(unsigned long long) ts->majf,
						(unsigned long long) ts->minf);

	/* Calc % distribution of IO depths, usecond, msecond latency */
	stat_calc_dist(ts->io_u_map, ddir_rw_sum(ts->total_io_u), io_u_dist);
	stat_calc_lat_nu(ts, io_u_lat_u);
	stat_calc_lat_m(ts, io_u_lat_m);

	/* Only show fixed 7 I/O depth levels*/
	log_buf(out, ";%3.1f%%;%3.1f%%;%3.1f%%;%3.1f%%;%3.1f%%;%3.1f%%;%3.1f%%",
			io_u_dist[0], io_u_dist[1], io_u_dist[2], io_u_dist[3],
			io_u_dist[4], io_u_dist[5], io_u_dist[6]);

	/* Microsecond latency */
	for (i = 0; i < FIO_IO_U_LAT_U_NR; i++)
		log_buf(out, ";%3.2f%%", io_u_lat_u[i]);
	/* Millisecond latency */
	for (i = 0; i < FIO_IO_U_LAT_M_NR; i++)
		log_buf(out, ";%3.2f%%", io_u_lat_m[i]);

	/* disk util stats, if any */
	if (ver >= 3 && is_running_backend())
		show_disk_util(1, NULL, out);

	/* Additional output if continue_on_error set - default off*/
	if (ts->continue_on_error)
		log_buf(out, ";%llu;%d", (unsigned long long) ts->total_err_count, ts->first_error);

	/* Additional output if description is set */
	if (strlen(ts->description)) {
		if (ver == 2)
			log_buf(out, "\n");
		log_buf(out, ";%s", ts->description);
	}

	log_buf(out, "\n");
}

/*
 * [한국어]
 * json_add_job_opts - 잡 옵션 리스트를 JSON 서브객체로 추가
 *
 * "job options": { "rw": "randread", "bs": "4K", ... } 형태로 저장.
 */
static void json_add_job_opts(struct json_object *root, const char *name,
			      struct flist_head *opt_list)
{
	struct json_object *dir_object;
	struct flist_head *entry;
	struct print_option *p;

	if (flist_empty(opt_list))
		return;

	dir_object = json_create_object();
	json_object_add_value_object(root, name, dir_object);

	flist_for_each(entry, opt_list) {
		p = flist_entry(entry, struct print_option, list);
		json_object_add_value_string(dir_object, p->name, p->value);
	}
}

/*
 * [한국어]
 * show_thread_status_json - 잡 하나의 JSON 형식 전체 보고서 생성
 *
 * @ts: thread_stat.
 * @rs: 그룹 run stats.
 * @opt_list: 옵션 리스트 (파싱된 잡 옵션 — json 에 "job options" 로 포함).
 * @return: 완성된 JSON root 객체.
 *
 * 반환 구조: {jobname, groupid, job_start, error, eta, elapsed, job options,
 *             read, write, trim, sync, mixed?, job_runtime, usr_cpu, sys_cpu,
 *             ctx, majf, minf, iodepth_level, iodepth_submit, iodepth_complete,
 *             latency_ns, latency_us, latency_ms, total_err?, first_error?,
 *             latency_depth?, block?, steadystate?, zone_resets?}
 *
 * JSON_PLUS 옵션 시 add_ddir_lat_json 내부에서 io_u_plat 의 원시 버킷도 포함.
 */
static struct json_object *show_thread_status_json(struct thread_stat *ts,
						   struct group_run_stats *rs,
						   struct flist_head *opt_list)
{
	struct json_object *root, *tmp;
	struct jobs_eta *je;
	double io_u_dist[FIO_IO_U_MAP_NR];
	double io_u_lat_n[FIO_IO_U_LAT_N_NR];
	double io_u_lat_u[FIO_IO_U_LAT_U_NR];
	double io_u_lat_m[FIO_IO_U_LAT_M_NR];
	double usr_cpu, sys_cpu;
	int i;
	size_t size;

	root = json_create_object();
	json_object_add_value_string(root, "jobname", ts->name);
	json_object_add_value_int(root, "groupid", ts->groupid);
	json_object_add_value_int(root, "job_start", ts->job_start);
	json_object_add_value_int(root, "error", ts->error);

	/* ETA Info */
	je = get_jobs_eta(true, &size);
	if (je) {
		json_object_add_value_int(root, "eta", je->eta_sec);
		json_object_add_value_int(root, "elapsed", je->elapsed_sec);
		free(je);
	}

	if (opt_list)
		json_add_job_opts(root, "job options", opt_list);

	add_ddir_status_json(ts, rs, DDIR_READ, root);
	add_ddir_status_json(ts, rs, DDIR_WRITE, root);
	add_ddir_status_json(ts, rs, DDIR_TRIM, root);
	add_ddir_status_json(ts, rs, DDIR_SYNC, root);

	if (ts->unified_rw_rep == UNIFIED_BOTH)
		add_mixed_ddir_status_json(ts, rs, root);

	/* CPU Usage */
	if (ts->total_run_time) {
		double runt = (double) ts->total_run_time;

		usr_cpu = (double) ts->usr_time * 100 / runt;
		sys_cpu = (double) ts->sys_time * 100 / runt;
	} else {
		usr_cpu = 0;
		sys_cpu = 0;
	}
	json_object_add_value_int(root, "job_runtime", ts->total_run_time);
	json_object_add_value_float(root, "usr_cpu", usr_cpu);
	json_object_add_value_float(root, "sys_cpu", sys_cpu);
	json_object_add_value_int(root, "ctx", ts->ctx);
	json_object_add_value_int(root, "majf", ts->majf);
	json_object_add_value_int(root, "minf", ts->minf);

	/* Calc % distribution of IO depths */
	stat_calc_dist(ts->io_u_map, ddir_rw_sum(ts->total_io_u), io_u_dist);
	tmp = json_create_object();
	json_object_add_value_object(root, "iodepth_level", tmp);
	/* Only show fixed 7 I/O depth levels*/
	for (i = 0; i < 7; i++) {
		char name[20];
		if (i < 6)
			snprintf(name, 20, "%d", 1 << i);
		else
			snprintf(name, 20, ">=%d", 1 << i);
		json_object_add_value_float(tmp, (const char *)name, io_u_dist[i]);
	}

	/* Calc % distribution of submit IO depths */
	stat_calc_dist(ts->io_u_submit, ts->total_submit, io_u_dist);
	tmp = json_create_object();
	json_object_add_value_object(root, "iodepth_submit", tmp);
	/* Only show fixed 7 I/O depth levels*/
	for (i = 0; i < 7; i++) {
		char name[20];
		if (i == 0)
			snprintf(name, 20, "0");
		else if (i < 6)
			snprintf(name, 20, "%d", 1 << (i+1));
		else
			snprintf(name, 20, ">=%d", 1 << i);
		json_object_add_value_float(tmp, (const char *)name, io_u_dist[i]);
	}

	/* Calc % distribution of completion IO depths */
	stat_calc_dist(ts->io_u_complete, ts->total_complete, io_u_dist);
	tmp = json_create_object();
	json_object_add_value_object(root, "iodepth_complete", tmp);
	/* Only show fixed 7 I/O depth levels*/
	for (i = 0; i < 7; i++) {
		char name[20];
		if (i == 0)
			snprintf(name, 20, "0");
		else if (i < 6)
			snprintf(name, 20, "%d", 1 << (i+1));
		else
			snprintf(name, 20, ">=%d", 1 << i);
		json_object_add_value_float(tmp, (const char *)name, io_u_dist[i]);
	}

	/* Calc % distribution of nsecond, usecond, msecond latency */
	stat_calc_dist(ts->io_u_map, ddir_rw_sum(ts->total_io_u), io_u_dist);
	stat_calc_lat_n(ts, io_u_lat_n);
	stat_calc_lat_u(ts, io_u_lat_u);
	stat_calc_lat_m(ts, io_u_lat_m);

	/* Nanosecond latency */
	tmp = json_create_object();
	json_object_add_value_object(root, "latency_ns", tmp);
	for (i = 0; i < FIO_IO_U_LAT_N_NR; i++) {
		const char *ranges[] = { "2", "4", "10", "20", "50", "100",
				 "250", "500", "750", "1000", };
		json_object_add_value_float(tmp, ranges[i], io_u_lat_n[i]);
	}
	/* Microsecond latency */
	tmp = json_create_object();
	json_object_add_value_object(root, "latency_us", tmp);
	for (i = 0; i < FIO_IO_U_LAT_U_NR; i++) {
		const char *ranges[] = { "2", "4", "10", "20", "50", "100",
				 "250", "500", "750", "1000", };
		json_object_add_value_float(tmp, ranges[i], io_u_lat_u[i]);
	}
	/* Millisecond latency */
	tmp = json_create_object();
	json_object_add_value_object(root, "latency_ms", tmp);
	for (i = 0; i < FIO_IO_U_LAT_M_NR; i++) {
		const char *ranges[] = { "2", "4", "10", "20", "50", "100",
				 "250", "500", "750", "1000", "2000",
				 ">=2000", };
		json_object_add_value_float(tmp, ranges[i], io_u_lat_m[i]);
	}

	/* Additional output if continue_on_error set - default off*/
	if (ts->continue_on_error) {
		json_object_add_value_int(root, "total_err", ts->total_err_count);
		json_object_add_value_int(root, "first_error", ts->first_error);
	}

	if (ts->latency_depth) {
		json_object_add_value_int(root, "latency_depth", ts->latency_depth);
		json_object_add_value_int(root, "latency_target", ts->latency_target);
		json_object_add_value_int(root, "latency_target_ns", ts->latency_target);
		json_object_add_value_float(root, "latency_percentile", ts->latency_percentile.u.f);
		json_object_add_value_int(root, "latency_window", ts->latency_window);
		json_object_add_value_int(root, "latency_window_us", ts->latency_window);
	}

	/* Additional output if description is set */
	if (strlen(ts->description))
		json_object_add_value_string(root, "desc", ts->description);

	if (ts->nr_block_infos) {
		/* Block error histogram and types */
		int len;
		unsigned int *percentiles = NULL;
		unsigned int block_state_counts[BLOCK_STATE_COUNT];

		len = calc_block_percentiles(ts->nr_block_infos, ts->block_infos,
					     ts->percentile_list,
					     &percentiles, block_state_counts);

		if (len) {
			struct json_object *block, *percentile_object, *states;
			int state;
			block = json_create_object();
			json_object_add_value_object(root, "block", block);

			percentile_object = json_create_object();
			json_object_add_value_object(block, "percentiles",
						     percentile_object);
			for (i = 0; i < len; i++) {
				char buf[20];
				snprintf(buf, sizeof(buf), "%f",
					 ts->percentile_list[i].u.f);
				json_object_add_value_int(percentile_object,
							  buf,
							  percentiles[i]);
			}

			states = json_create_object();
			json_object_add_value_object(block, "states", states);
			for (state = 0; state < BLOCK_STATE_COUNT; state++) {
				json_object_add_value_int(states,
					block_state_names[state],
					block_state_counts[state]);
			}
			free(percentiles);
		}
	}

	if (ts->ss_dur) {
		struct json_object *data;
		struct json_array *iops, *bw;
		int j, k, l;
		char ss_buf[64];
		int intervals = ts->ss_dur / (ss_check_interval / 1000L);

		snprintf(ss_buf, sizeof(ss_buf), "%s%s:%f%s",
			ts->ss_state & FIO_SS_IOPS ? "iops" : (ts->ss_state & FIO_SS_LAT ? "lat" : "bw"),
			ts->ss_state & FIO_SS_SLOPE ? "_slope" : "",
			(float) ts->ss_limit.u.f,
			ts->ss_state & FIO_SS_PCT ? "%" : "");

		tmp = json_create_object();
		json_object_add_value_object(root, "steadystate", tmp);
		json_object_add_value_string(tmp, "ss", ss_buf);
		json_object_add_value_int(tmp, "duration", (int)ts->ss_dur);
		json_object_add_value_int(tmp, "attained", (ts->ss_state & FIO_SS_ATTAINED) > 0);

		snprintf(ss_buf, sizeof(ss_buf), "%f%s", (float) ts->ss_criterion.u.f,
			ts->ss_state & FIO_SS_PCT ? "%" : "");
		json_object_add_value_string(tmp, "criterion", ss_buf);
		json_object_add_value_float(tmp, "max_deviation", ts->ss_deviation.u.f);
		json_object_add_value_float(tmp, "slope", ts->ss_slope.u.f);

		data = json_create_object();
		json_object_add_value_object(tmp, "data", data);
		bw = json_create_array();
		iops = json_create_array();

		/*
		** if ss was attained or the buffer is not full,
		** ss->head points to the first element in the list.
		** otherwise it actually points to the second element
		** in the list
		*/
		if ((ts->ss_state & FIO_SS_ATTAINED) || !(ts->ss_state & FIO_SS_BUFFER_FULL))
			j = ts->ss_head;
		else
			j = ts->ss_head == 0 ? intervals - 1 : ts->ss_head - 1;
		for (l = 0; l < intervals; l++) {
			k = (j + l) % intervals;
			json_array_add_value_int(bw, ts->ss_bw_data[k]);
			json_array_add_value_int(iops, ts->ss_iops_data[k]);
		}
		json_object_add_value_int(data, "bw_mean", steadystate_bw_mean(ts));
		json_object_add_value_int(data, "iops_mean", steadystate_iops_mean(ts));
		if (ts->ss_state & FIO_SS_LAT) {
			struct json_array *lat;
			lat = json_create_array();
			for (l = 0; l < intervals; l++) {
				k = (j + l) % intervals;
				json_array_add_value_int(lat, ts->ss_lat_data[k]);
			}
			json_object_add_value_int(data, "lat_mean", steadystate_lat_mean(ts));
			json_object_add_value_array(data, "lat_ns", lat);
		}
		json_object_add_value_array(data, "iops", iops);
		json_object_add_value_array(data, "bw", bw);
	}

	if (ts->count_zone_resets)
		json_object_add_value_int(root, "zone_resets",
					  ts->nr_zone_resets);

	return root;
}

/*
 * [한국어]
 * show_thread_status_terse - terse 포맷 디스패처 (v2..v5 지원)
 *
 * terse_version 전역에 따라 show_thread_status_terse_all 호출.
 * 지원 밖 버전은 log_err 로 경고.
 */
static void show_thread_status_terse(struct thread_stat *ts,
				     struct group_run_stats *rs,
				     struct buf_output *out)
{
	if (terse_version >= 2 && terse_version <= 5)
		show_thread_status_terse_all(ts, rs, terse_version, out);
	else
		log_err("fio: bad terse version!? %d\n", terse_version);
}

/*
 * [한국어]
 * show_thread_status - 출력 포맷 최상위 디스패처 (normal/terse/json 동시 호출 가능)
 *
 * @ts: 잡 thread_stat.
 * @rs: 그룹 run stats.
 * @opt_list: 잡 옵션 리스트 (JSON 에 포함).
 * @out: 출력 버퍼 (normal/terse 대상).
 * @return: JSON 객체 (JSON 출력이 활성화된 경우만, 아니면 NULL).
 *
 * output_format 전역 비트마스크(FIO_OUTPUT_TERSE/JSON/NORMAL)로 3가지 포맷 동시 출력 가능.
 * JSON 반환값은 __show_run_stats 가 최종 집계하여 단일 JSON 문서로 조립.
 */
struct json_object *show_thread_status(struct thread_stat *ts,
				       struct group_run_stats *rs,
				       struct flist_head *opt_list,
				       struct buf_output *out)
{
	struct json_object *ret = NULL;

	if (output_format & FIO_OUTPUT_TERSE)
		show_thread_status_terse(ts, rs,  out);
	if (output_format & FIO_OUTPUT_JSON)
		ret = show_thread_status_json(ts, rs, opt_list);
	if (output_format & FIO_OUTPUT_NORMAL)
		show_thread_status_normal(ts, rs,  out);

	return ret;
}

/*
 * [한국어]
 * __sum_stat - 두 io_stat 의 Welford 누적기(mean/S) 를 정확히 병합
 *
 * @dst: 누적 대상 (병합 결과가 들어감).
 * @src: 소스.
 * @first: true=첫 합산(dst 비어 있음), false=추가 합산.
 *
 * Chan et al. "Algorithms for computing the sample variance" (1979) 의 병렬 알고리즘:
 *   mean_new = (n1*m1 + n2*m2) / (n1+n2)
 *   M2_new   = M2_1 + M2_2 + (m2-m1)^2 * n1*n2 / (n1+n2)
 * 여기서 fio 의 S = M2 (Welford 의 온라인 정의: Σ(x_i - mean)^2).
 *
 * 단순히 S 를 더하면 두 그룹의 평균이 다를 때 편차가 누락된다.
 * (m2-m1)^2 * n1*n2/(n1+n2) 항이 그룹 간 평균 차이로 인한 분산을 보정한다.
 *
 * min/max 는 단순 min/max 병합.
 */
static void __sum_stat(struct io_stat *dst, const struct io_stat *src, bool first)
{
	double mean, S;

	dst->min_val = min(dst->min_val, src->min_val);    /* [한국어] 전 구간 최솟값 갱신 */
	dst->max_val = max(dst->max_val, src->max_val);    /* [한국어] 전 구간 최댓값 갱신 */

	/*
	 * Compute new mean and S after the merge
	 * <http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
	 *  #Parallel_algorithm>
	 */
	if (first) {
		mean = src->mean.u.f;
		S = src->S.u.f;
	} else {
		double delta = src->mean.u.f - dst->mean.u.f;

		mean = ((src->mean.u.f * src->samples) +
			(dst->mean.u.f * dst->samples)) /
			(dst->samples + src->samples);

		S =  src->S.u.f + dst->S.u.f + pow(delta, 2.0) *
			(dst->samples * src->samples) /
			(dst->samples + src->samples);
	}

	dst->samples += src->samples;
	dst->mean.u.f = mean;
	dst->S.u.f = S;

}

/*
 * We sum two kinds of stats - one that is time based, in which case we
 * apply the proper summing technique, and then one that is iops/bw
 * numbers. For group_reporting, we should just add those up, not make
 * them the mean of everything.
 */
/*
 * [한국어]
 * sum_stat - io_stat 합산의 두 가지 모드 분기
 *
 * @dst: 누적기.
 * @src: 소스.
 * @pure_sum: true=IOPS/BW 처럼 절대량 합산, false=레이턴시처럼 Welford 병합.
 *
 * - 레이턴시: Welford 합산 (__sum_stat) — 그룹 전체의 통일된 mean/stddev 도출.
 * - BW/IOPS 그룹 보고: 각 잡의 값을 더해 총량으로 표기 (평균이 아닌 합).
 */
static void sum_stat(struct io_stat *dst, const struct io_stat *src, bool pure_sum)
{
	bool first = dst->samples == 0;

	if (src->samples == 0)
		return;

	if (!pure_sum) {
		__sum_stat(dst, src, first);
		return;
	}

	if (first) {
		dst->min_val = src->min_val;
		dst->max_val = src->max_val;
		dst->samples = src->samples;
		dst->mean.u.f = src->mean.u.f;
		dst->S.u.f = src->S.u.f;
	} else {
		dst->min_val += src->min_val;
		dst->max_val += src->max_val;
		dst->samples += src->samples;
		dst->mean.u.f += src->mean.u.f;
		dst->S.u.f += src->S.u.f;
	}
}

/*
 * [한국어]
 * sum_group_stats - 그룹 run stats 병합 (다중 그룹 집계에 사용)
 *
 * min/max 는 극값, iobytes/agg 는 합산.
 * kb_base/unit_base/sig_figs 는 0 이면 src 값 채택.
 */
void sum_group_stats(struct group_run_stats *dst, const struct group_run_stats *src)
{
	unsigned int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		if (dst->max_run[i] < src->max_run[i])
			dst->max_run[i] = src->max_run[i];
		if (dst->min_run[i] && dst->min_run[i] > src->min_run[i])
			dst->min_run[i] = src->min_run[i];
		if (dst->max_bw[i] < src->max_bw[i])
			dst->max_bw[i] = src->max_bw[i];
		if (dst->min_bw[i] && dst->min_bw[i] > src->min_bw[i])
			dst->min_bw[i] = src->min_bw[i];

		dst->iobytes[i] += src->iobytes[i];
		dst->agg[i] += src->agg[i];
	}

	if (!dst->kb_base)
		dst->kb_base = src->kb_base;
	if (!dst->unit_base)
		dst->unit_base = src->unit_base;
	if (!dst->sig_figs)
		dst->sig_figs = src->sig_figs;
}

/*
 * Free the clat_prio_stat arrays allocated by alloc_clat_prio_stat_ddir().
 */
/*
 * [한국어]
 * free_clat_prio_stats - cmdprio 별 per-priority 통계 배열 해제
 *
 * smalloc/sfree 로 공유 메모리에 할당된 배열을 방향별로 해제.
 */
void free_clat_prio_stats(struct thread_stat *ts)
{
	enum fio_ddir ddir;

	if (!ts)
		return;

	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
		sfree(ts->clat_prio[ddir]);
		ts->clat_prio[ddir] = NULL;
		ts->nr_clat_prio[ddir] = 0;
	}
}

/*
 * Allocate a clat_prio_stat array. The array has to be allocated/freed using
 * smalloc/sfree, so that it is accessible by the process/thread summing the
 * thread_stats.
 */
/*
 * [한국어]
 * alloc_clat_prio_stat_ddir - cmdprio 별 per-priority 통계 배열 할당
 *
 * @ts: 대상 thread_stat.
 * @ddir: 방향.
 * @nr_prios: 할당할 우선순위 슬롯 수.
 * @return: 0=성공, 1=메모리 실패.
 *
 * smalloc(공유 메모리) 를 사용해 sum_thread_stats 가 부모 프로세스에서도 접근 가능.
 * 초기화 min_val=ULONG_MAX 로 하여 min 비교 시 첫 샘플이 갱신 트리거.
 */
int alloc_clat_prio_stat_ddir(struct thread_stat *ts, enum fio_ddir ddir,
			      int nr_prios)
{
	struct clat_prio_stat *clat_prio;
	int i;

	clat_prio = scalloc(nr_prios, sizeof(*ts->clat_prio[ddir]));
	if (!clat_prio) {
		log_err("fio: failed to allocate ts clat data\n");
		return 1;
	}

	for (i = 0; i < nr_prios; i++)
		clat_prio[i].clat_stat.min_val = ULONG_MAX;

	ts->clat_prio[ddir] = clat_prio;
	ts->nr_clat_prio[ddir] = nr_prios;

	return 0;
}

/*
 * [한국어]
 * grow_clat_prio_stat - clat_prio 배열에 슬롯 1개 추가 (동적 확장)
 *
 * sum_thread_stats 중 새로운 ioprio 값을 만나면 배열을 한 칸 늘려 추가.
 * 공유 메모리는 realloc 불가라 새로 할당 후 memcpy 하는 방식.
 */
static int grow_clat_prio_stat(struct thread_stat *dst, enum fio_ddir ddir)
{
	int curr_len = dst->nr_clat_prio[ddir];
	void *new_arr;

	new_arr = scalloc(curr_len + 1, sizeof(*dst->clat_prio[ddir]));
	if (!new_arr) {
		log_err("fio: failed to grow clat prio array\n");
		return 1;
	}

	memcpy(new_arr, dst->clat_prio[ddir],
	       curr_len * sizeof(*dst->clat_prio[ddir]));
	sfree(dst->clat_prio[ddir]);

	dst->clat_prio[ddir] = new_arr;
	dst->clat_prio[ddir][curr_len].clat_stat.min_val = ULONG_MAX;
	dst->nr_clat_prio[ddir]++;

	return 0;
}

/*
 * [한국어]
 * find_clat_prio_index - ioprio 값으로 clat_prio 배열에서 인덱스 찾기
 *
 * @return: -1 = 없음, 그 외 = 인덱스.
 * 선형 탐색 (prio 배열은 보통 소수 개수).
 */
static int find_clat_prio_index(struct thread_stat *dst, enum fio_ddir ddir,
				uint32_t ioprio)
{
	int i, nr_prios = dst->nr_clat_prio[ddir];

	for (i = 0; i < nr_prios; i++) {
		if (dst->clat_prio[ddir][i].ioprio == ioprio)
			return i;
	}

	return -1;
}

/*
 * [한국어]
 * alloc_or_get_clat_prio_index - ioprio 가 있으면 그 인덱스 반환, 없으면 새로 할당
 *
 * sum_thread_stats 에서 ddir 별 ioprio 별 집계 시, dst 에 해당 prio 가 없으면 grow 호출.
 */
static int alloc_or_get_clat_prio_index(struct thread_stat *dst,
					enum fio_ddir ddir, uint32_t ioprio,
					int *idx)
{
	int index = find_clat_prio_index(dst, ddir, ioprio);

	if (index == -1) {
		index = dst->nr_clat_prio[ddir];

		if (grow_clat_prio_stat(dst, ddir))
			return 1;

		dst->clat_prio[ddir][index].ioprio = ioprio;
	}

	*idx = index;

	return 0;
}

/*
 * [한국어]
 * clat_prio_stats_copy - src 의 clat_prio 배열을 dst 로 그대로 복사 (첫 합산)
 *
 * dst 가 비어있을 때 src 배열을 통째로 smalloc+memcpy.
 */
static int clat_prio_stats_copy(struct thread_stat *dst, const struct thread_stat *src,
				enum fio_ddir dst_ddir, enum fio_ddir src_ddir)
{
	size_t sz = sizeof(*src->clat_prio[src_ddir]) *
		src->nr_clat_prio[src_ddir];

	dst->clat_prio[dst_ddir] = smalloc(sz);
	if (!dst->clat_prio[dst_ddir]) {
		log_err("fio: failed to alloc clat prio array\n");
		return 1;
	}

	memcpy(dst->clat_prio[dst_ddir], src->clat_prio[src_ddir], sz);
	dst->nr_clat_prio[dst_ddir] = src->nr_clat_prio[src_ddir];

	return 0;
}

/*
 * [한국어]
 * clat_prio_stat_add_samples - 특정 ioprio 의 Welford 통계+버킷을 dst 에 병합
 *
 * alloc_or_get_clat_prio_index 로 슬롯 확보 후 sum_stat + io_u_plat 가산.
 */
static int clat_prio_stat_add_samples(struct thread_stat *dst,
				      enum fio_ddir dst_ddir, uint32_t ioprio,
				      const struct io_stat *io_stat,
				      const uint64_t *io_u_plat)
{
	int i, dst_index;

	if (!io_stat->samples)
		return 0;

	if (alloc_or_get_clat_prio_index(dst, dst_ddir, ioprio, &dst_index))
		return 1;

	sum_stat(&dst->clat_prio[dst_ddir][dst_index].clat_stat, io_stat,
		 false);

	for (i = 0; i < FIO_IO_U_PLAT_NR; i++)
		dst->clat_prio[dst_ddir][dst_index].io_u_plat[i] += io_u_plat[i];

	return 0;
}

/*
 * [한국어]
 * sum_clat_prio_stats_src_single_prio - src 가 cmdprio 미사용일 때의 합산 경로
 *
 * src->clat_prio 배열이 없으면 모든 I/O 가 src->ioprio 하나로 제출된 것이므로,
 * 전체 clat_stat/lat_stat 을 src->ioprio 슬롯의 샘플로 취급해 병합한다.
 */
static int sum_clat_prio_stats_src_single_prio(struct thread_stat *dst,
					       const struct thread_stat *src,
					       enum fio_ddir dst_ddir,
					       enum fio_ddir src_ddir)
{
	const struct io_stat *io_stat;
	const uint64_t *io_u_plat;

	/*
	 * If src ts has no clat_prio_stat array, then all I/Os were submitted
	 * using src->ioprio. Thus, the global samples in src->clat_stat (or
	 * src->lat_stat) can be used as the 'per prio' samples for src->ioprio.
	 */
	assert(!src->clat_prio[src_ddir]);
	assert(src->nr_clat_prio[src_ddir] == 0);

	if (src->lat_percentiles) {
		io_u_plat = src->io_u_plat[FIO_LAT][src_ddir];
		io_stat = &src->lat_stat[src_ddir];
	} else {
		io_u_plat = src->io_u_plat[FIO_CLAT][src_ddir];
		io_stat = &src->clat_stat[src_ddir];
	}

	return clat_prio_stat_add_samples(dst, dst_ddir, src->ioprio, io_stat,
					  io_u_plat);
}

/*
 * [한국어]
 * sum_clat_prio_stats_src_multi_prio - src 에 cmdprio 배열이 이미 존재할 때의 합산 경로
 *
 * src->clat_prio 배열이 있으면 cmdprio_percentage/bssplit 로 여러 prio 가 섞여 있으며,
 * 기본 prio 의 샘플도 이 배열에 포함되어 있다. dst 가 비어있으면 통째로 복사,
 * 아니면 각 prio 별로 add_samples 호출.
 */
static int sum_clat_prio_stats_src_multi_prio(struct thread_stat *dst,
					      const struct thread_stat *src,
					      enum fio_ddir dst_ddir,
					      enum fio_ddir src_ddir)
{
	int i;

	/*
	 * If src ts has a clat_prio_stat array, then there are multiple prios
	 * in use (i.e. src ts had cmdprio_percentage or cmdprio_bssplit set).
	 * The samples for the default prio will exist in the src->clat_prio
	 * array, just like the samples for any other prio.
	 */
	assert(src->clat_prio[src_ddir]);
	assert(src->nr_clat_prio[src_ddir]);

	/* If the dst ts doesn't yet have a clat_prio array, simply memcpy. */
	if (!dst->clat_prio[dst_ddir])
		return clat_prio_stats_copy(dst, src, dst_ddir, src_ddir);

	/* The dst ts already has a clat_prio_array, add src stats into it. */
	for (i = 0; i < src->nr_clat_prio[src_ddir]; i++) {
		struct io_stat *io_stat = &src->clat_prio[src_ddir][i].clat_stat;
		uint64_t *io_u_plat = src->clat_prio[src_ddir][i].io_u_plat;
		uint32_t ioprio = src->clat_prio[src_ddir][i].ioprio;

		if (clat_prio_stat_add_samples(dst, dst_ddir, ioprio, io_stat, io_u_plat))
			return 1;
	}

	return 0;
}

/*
 * [한국어]
 * sum_clat_prio_stats - cmdprio 별 통계 병합 디스패처
 *
 * src->clat_prio 존재 여부에 따라 single/multi prio 경로로 분기.
 * disable_prio_stat(init_per_prio_stats 가 세팅) 가 true 면 스킵.
 */
static int sum_clat_prio_stats(struct thread_stat *dst, const struct thread_stat *src,
			       enum fio_ddir dst_ddir, enum fio_ddir src_ddir)
{
	if (dst->disable_prio_stat)
		return 0;

	if (!src->clat_prio[src_ddir])
		return sum_clat_prio_stats_src_single_prio(dst, src, dst_ddir,
							   src_ddir);

	return sum_clat_prio_stats_src_multi_prio(dst, src, dst_ddir, src_ddir);
}

/*
 * [한국어]
 * sum_thread_stats - 두 thread_stat 병합 (group_reporting / MIXED 집계 핵심)
 *
 * @dst: 누적 대상.
 * @src: 소스.
 *
 * 처리:
 *   - ddir 별 clat/slat/lat 는 sum_stat(..., false) 로 Welford 병합 — 평균/분산 유지.
 *   - bw/iops 는 sum_stat(..., true) 로 절대량 합산 — "그룹 총 대역폭" 의미.
 *   - io_u_plat 히스토그램: 각 버킷 카운트 단순 합산.
 *   - io_u_lat_n/u/m (ns/us/ms 분포): 단순 합산.
 *   - total_io_u/short_io_u/drop_io_u/io_u_map/io_u_submit/io_u_complete: 단순 합산.
 *   - sync_stat: Welford 병합.
 *   - CPU 시간/컨텍스트/페이지폴트/런타임/캐시/존 리셋: 단순 합산.
 *   - cmdprio: sum_clat_prio_stats 로 prio 별 통합.
 *
 * MIXED 모드 (dst->unified_rw_rep == UNIFIED_MIXED):
 *   세 방향(READ/WRITE/TRIM) 을 모두 index 0 으로 병합해 단일 통계로 표시.
 *
 * 실행 컨텍스트: 메인 프로세스 보고서 생성 단계.
 *
 * 호출 체인:
 *   __show_run_stats → [sum_thread_stats] → sum_stat / sum_clat_prio_stats
 */
void sum_thread_stats(struct thread_stat *dst, const struct thread_stat *src)
{
	int k, l, m;

	for (l = 0; l < DDIR_RWDIR_CNT; l++) {
		if (dst->unified_rw_rep != UNIFIED_MIXED) {
			sum_stat(&dst->clat_stat[l], &src->clat_stat[l], false);
			sum_stat(&dst->slat_stat[l], &src->slat_stat[l], false);
			sum_stat(&dst->lat_stat[l], &src->lat_stat[l], false);
			sum_stat(&dst->bw_stat[l], &src->bw_stat[l], true);
			sum_stat(&dst->iops_stat[l], &src->iops_stat[l], true);
			sum_clat_prio_stats(dst, src, l, l);

			dst->io_bytes[l] += src->io_bytes[l];

			if (dst->runtime[l] < src->runtime[l])
				dst->runtime[l] = src->runtime[l];
		} else {
			sum_stat(&dst->clat_stat[0], &src->clat_stat[l], false);
			sum_stat(&dst->slat_stat[0], &src->slat_stat[l], false);
			sum_stat(&dst->lat_stat[0], &src->lat_stat[l], false);
			sum_stat(&dst->bw_stat[0], &src->bw_stat[l], true);
			sum_stat(&dst->iops_stat[0], &src->iops_stat[l], true);
			sum_clat_prio_stats(dst, src, 0, l);

			dst->io_bytes[0] += src->io_bytes[l];

			if (dst->runtime[0] < src->runtime[l])
				dst->runtime[0] = src->runtime[l];
		}
	}

	sum_stat(&dst->sync_stat, &src->sync_stat, false);
	dst->usr_time += src->usr_time;
	dst->sys_time += src->sys_time;
	dst->ctx += src->ctx;
	dst->majf += src->majf;
	dst->minf += src->minf;

	for (k = 0; k < FIO_IO_U_MAP_NR; k++) {
		dst->io_u_map[k] += src->io_u_map[k];
		dst->io_u_submit[k] += src->io_u_submit[k];
		dst->io_u_complete[k] += src->io_u_complete[k];
	}

	for (k = 0; k < FIO_IO_U_LAT_N_NR; k++)
		dst->io_u_lat_n[k] += src->io_u_lat_n[k];
	for (k = 0; k < FIO_IO_U_LAT_U_NR; k++)
		dst->io_u_lat_u[k] += src->io_u_lat_u[k];
	for (k = 0; k < FIO_IO_U_LAT_M_NR; k++)
		dst->io_u_lat_m[k] += src->io_u_lat_m[k];

	for (k = 0; k < DDIR_RWDIR_CNT; k++) {
		if (dst->unified_rw_rep != UNIFIED_MIXED) {
			dst->total_io_u[k] += src->total_io_u[k];
			dst->short_io_u[k] += src->short_io_u[k];
			dst->drop_io_u[k] += src->drop_io_u[k];
		} else {
			dst->total_io_u[0] += src->total_io_u[k];
			dst->short_io_u[0] += src->short_io_u[k];
			dst->drop_io_u[0] += src->drop_io_u[k];
		}
	}

	dst->total_io_u[DDIR_SYNC] += src->total_io_u[DDIR_SYNC];

	for (k = 0; k < FIO_LAT_CNT; k++)
		for (l = 0; l < DDIR_RWDIR_CNT; l++)
			for (m = 0; m < FIO_IO_U_PLAT_NR; m++)
				if (dst->unified_rw_rep != UNIFIED_MIXED)
					dst->io_u_plat[k][l][m] += src->io_u_plat[k][l][m];
				else
					dst->io_u_plat[k][0][m] += src->io_u_plat[k][l][m];

	for (k = 0; k < FIO_IO_U_PLAT_NR; k++)
		dst->io_u_sync_plat[k] += src->io_u_sync_plat[k];

	dst->total_run_time += src->total_run_time;
	dst->total_submit += src->total_submit;
	dst->total_complete += src->total_complete;
	if (src->count_zone_resets) {
		dst->count_zone_resets = 1;
		dst->nr_zone_resets += src->nr_zone_resets;
	}
	dst->cachehit += src->cachehit;
	dst->cachemiss += src->cachemiss;
}

/*
 * [한국어]
 * init_group_run_stat - 그룹 run stats 초기화 (min 을 ~0UL 로 설정해 첫 비교에 갱신)
 */
void init_group_run_stat(struct group_run_stats *gs)
{
	int i;
	memset(gs, 0, sizeof(*gs));

	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		gs->min_bw[i] = gs->min_run[i] = ~0UL;
}

/*
 * [한국어]
 * init_thread_stat_min_vals - 모든 io_stat 의 min_val 을 ULONG_MAX 로 초기화
 *
 * add_stat_sample 에서 min 비교 로직이 첫 샘플을 기록할 수 있도록 무한대로 초기화.
 */
void init_thread_stat_min_vals(struct thread_stat *ts)
{
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		ts->clat_stat[i].min_val = ULONG_MAX;
		ts->slat_stat[i].min_val = ULONG_MAX;
		ts->lat_stat[i].min_val = ULONG_MAX;
		ts->bw_stat[i].min_val = ULONG_MAX;
		ts->iops_stat[i].min_val = ULONG_MAX;
	}
	ts->sync_stat.min_val = ULONG_MAX;
}

/*
 * [한국어]
 * init_thread_stat - thread_stat 전체 초기화 (0 클리어 + min_val=MAX + groupid=-1)
 */
void init_thread_stat(struct thread_stat *ts)
{
	memset(ts, 0, sizeof(*ts));

	init_thread_stat_min_vals(ts);
	ts->groupid = -1;
}

/*
 * [한국어]
 * init_per_prio_stats - 그룹 내 prio 집계가 가능한지 판정해 disable_prio_stat 비트 설정
 *
 * @threadstats: 집계 결과가 들어갈 thread_stat 배열.
 * @nr_ts: 배열 길이.
 *
 * 로직:
 *   - 그룹 내 모든 td 가 같은 ioprio 를 가지고 cmdprio 배열이 없으면 prio 통계 활성화.
 *   - 그 외(다른 ioprio 혼재 / cmdprio 사용 중) 는 disable_prio_stat=1.
 * 최종 비트는 끝에서 반전되므로, 루프 중에는 "disable 조건 발견" 을 1 로 기록한 뒤
 * 마지막에 !disable 로 뒤집어 "활성화 여부" 로 변환.
 */
static void init_per_prio_stats(struct thread_stat *threadstats, int nr_ts)
{
	struct thread_stat *ts;
	int i, j, last_ts, idx;
	enum fio_ddir ddir;

	j = 0;
	last_ts = -1;
	idx = 0;

	/*
	 * Loop through all tds, if a td requires per prio stats, temporarily
	 * store a 1 in ts->disable_prio_stat, and then do an additional
	 * loop at the end where we invert the ts->disable_prio_stat values.
	 */
	for_each_td(td) {
		if (!td->o.stats)
			continue;
		if (idx &&
		    (!td->o.group_reporting ||
		     (td->o.group_reporting && last_ts != td->groupid))) {
			idx = 0;
			j++;
		}

		last_ts = td->groupid;
		ts = &threadstats[j];

		/* idx == 0 means first td in group, or td is not in a group. */
		if (idx == 0)
			ts->ioprio = td->ioprio;
		else if (td->ioprio != ts->ioprio)
			ts->disable_prio_stat = 1;

		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			if (td->ts.clat_prio[ddir]) {
				ts->disable_prio_stat = 1;
				break;
			}
		}

		idx++;
	} end_for_each();

	/* Loop through all dst threadstats and fixup the values. */
	for (i = 0; i < nr_ts; i++) {
		ts = &threadstats[i];
		ts->disable_prio_stat = !ts->disable_prio_stat;
	}
}

/*
 * [한국어]
 * __show_run_stats - 최종 실행 통계 보고서를 생성하는 중앙 함수
 *
 * 호출 시점: fio 실행 완료 후 backend.c 의 fio_backend() 말미에서 show_run_stats() 를 거쳐 진입.
 *
 * 처리 단계:
 *   1) 전체 잡 개수와 그룹 개수 기반으로 runstats[0..groupid] / threadstats[0..nr_ts-1] 할당.
 *   2) init_group_run_stat / init_thread_stat 로 초기화.
 *   3) 모든 td 순회하며:
 *      - td->ts 를 threadstats[j] 에 sum_thread_stats() 로 병합 (group_reporting 이면 동일 j).
 *      - kb_base/unit_base 불일치 경고.
 *   4) 각 그룹별 runstats[group] 에 대역폭 집계(agg/min_bw/max_bw/io bytes/run) 계산.
 *   5) 각 threadstats[i] 에 대해 출력 형식별로 show_thread_status_{normal|terse|json} 호출.
 *   6) 그룹 집계 show_group_stats, 디스크 유틸 show_disk_util, idle prof show_idle_prof_stats.
 *   7) 서버 모드(is_backend)면 server.c 가 결과를 클라이언트에 전송.
 *   8) 자원 해제.
 *
 * 실행 컨텍스트: 메인 프로세스 단독 (모든 잡 reap 완료 후).
 *
 * 호출 체인:
 *   show_run_stats → [__show_run_stats] → sum_thread_stats/show_thread_status/show_group_stats
 */
void __show_run_stats(void)
{
	struct group_run_stats *runstats, *rs;
	struct thread_stat *threadstats, *ts;
	int i, j, k, nr_ts, last_ts, idx;
	bool kb_base_warned = false;
	bool unit_base_warned = false;
	struct json_object *root = NULL;
	struct json_array *array = NULL;
	struct buf_output output[FIO_OUTPUT_NR];
	struct flist_head **opt_lists;

	runstats = malloc(sizeof(struct group_run_stats) * (groupid + 1));

	for (i = 0; i < groupid + 1; i++)
		init_group_run_stat(&runstats[i]);

	/*
	 * find out how many threads stats we need. if group reporting isn't
	 * enabled, it's one-per-td.
	 */
	nr_ts = 0;
	last_ts = -1;
	for_each_td(td) {
		if (!td->o.group_reporting) {
			nr_ts++;
			continue;
		}
		if (last_ts == td->groupid)
			continue;
		if (!td->o.stats)
			continue;

		last_ts = td->groupid;
		nr_ts++;
	} end_for_each();

	threadstats = malloc(nr_ts * sizeof(struct thread_stat));
	opt_lists = malloc(nr_ts * sizeof(struct flist_head *));

	for (i = 0; i < nr_ts; i++) {
		init_thread_stat(&threadstats[i]);
		opt_lists[i] = NULL;
	}

	init_per_prio_stats(threadstats, nr_ts);

	j = 0;
	last_ts = -1;
	idx = 0;
	for_each_td(td) {
		if (!td->o.stats)
			continue;
		if (idx && (!td->o.group_reporting ||
		    (td->o.group_reporting && last_ts != td->groupid))) {
			idx = 0;
			j++;
		}

		last_ts = td->groupid;

		ts = &threadstats[j];

		ts->clat_percentiles = td->o.clat_percentiles;
		ts->lat_percentiles = td->o.lat_percentiles;
		ts->slat_percentiles = td->o.slat_percentiles;
		ts->percentile_precision = td->o.percentile_precision;
		memcpy(ts->percentile_list, td->o.percentile_list, sizeof(td->o.percentile_list));
		opt_lists[j] = &td->opt_list;

		idx++;

		if (ts->groupid == -1) {
			/*
			 * These are per-group shared already
			 */
			snprintf(ts->name, sizeof(ts->name), "%s", td->o.name);
			if (td->o.description)
				snprintf(ts->description,
					 sizeof(ts->description), "%s",
					 td->o.description);
			else
				memset(ts->description, 0, FIO_JOBDESC_SIZE);

			/*
			 * If multiple entries in this group, this is
			 * the first member.
			 */
			ts->thread_number = td->thread_number;
			ts->groupid = td->groupid;
			ts->job_start = td->job_start;

			/*
			 * first pid in group, not very useful...
			 */
			ts->pid = td->pid;

			ts->kb_base = td->o.kb_base;
			ts->unit_base = td->o.unit_base;
			ts->sig_figs = td->o.sig_figs;
			ts->unified_rw_rep = td->o.unified_rw_rep;
		} else if (ts->kb_base != td->o.kb_base && !kb_base_warned) {
			log_info("fio: kb_base differs for jobs in group, using"
				 " %u as the base\n", ts->kb_base);
			kb_base_warned = true;
		} else if (ts->unit_base != td->o.unit_base && !unit_base_warned) {
			log_info("fio: unit_base differs for jobs in group, using"
				 " %u as the base\n", ts->unit_base);
			unit_base_warned = true;
		}

		ts->continue_on_error = td->o.continue_on_error;
		ts->total_err_count += td->total_err_count;
		ts->first_error = td->first_error;
		if (!ts->error) {
			if (!td->error && td->o.continue_on_error &&
			    td->first_error) {
				ts->error = td->first_error;
				snprintf(ts->verror, sizeof(ts->verror), "%s",
					 td->verror);
			} else if (td->error) {
				ts->error = td->error;
				snprintf(ts->verror, sizeof(ts->verror), "%s",
					 td->verror);
			}
		}

		ts->latency_depth = td->latency_qd;
		ts->latency_target = td->o.latency_target;
		ts->latency_percentile = td->o.latency_percentile;
		ts->latency_window = td->o.latency_window;

		ts->nr_block_infos = td->ts.nr_block_infos;
		for (k = 0; k < ts->nr_block_infos; k++)
			ts->block_infos[k] = td->ts.block_infos[k];

		sum_thread_stats(ts, &td->ts);

		ts->members++;

		if (td->o.ss_dur) {
			ts->ss_state = td->ss.state;
			ts->ss_dur = td->ss.dur;
			ts->ss_head = td->ss.head;
			ts->ss_bw_data = td->ss.bw_data;
			ts->ss_iops_data = td->ss.iops_data;
			ts->ss_lat_data = td->ss.lat_data;
			ts->ss_limit.u.f = td->ss.limit;
			ts->ss_slope.u.f = td->ss.slope;
			ts->ss_deviation.u.f = td->ss.deviation;
			ts->ss_criterion.u.f = td->ss.criterion;
		}
		else
			ts->ss_dur = ts->ss_state = 0;
	} end_for_each();

	for (i = 0; i < nr_ts; i++) {
		unsigned long long bw;

		ts = &threadstats[i];
		if (ts->groupid == -1)
			continue;
		rs = &runstats[ts->groupid];
		rs->kb_base = ts->kb_base;
		rs->unit_base = ts->unit_base;
		rs->sig_figs = ts->sig_figs;
		rs->unified_rw_rep |= ts->unified_rw_rep;

		for (j = 0; j < DDIR_RWDIR_CNT; j++) {
			if (!ts->runtime[j])
				continue;
			if (ts->runtime[j] < rs->min_run[j] || !rs->min_run[j])
				rs->min_run[j] = ts->runtime[j];
			if (ts->runtime[j] > rs->max_run[j])
				rs->max_run[j] = ts->runtime[j];

			bw = 0;
			if (ts->runtime[j])
				bw = ts->io_bytes[j] * 1000 / ts->runtime[j];
			if (bw < rs->min_bw[j])
				rs->min_bw[j] = bw;
			if (bw > rs->max_bw[j])
				rs->max_bw[j] = bw;

			rs->iobytes[j] += ts->io_bytes[j];
		}
	}

	for (i = 0; i < groupid + 1; i++) {
		enum fio_ddir ddir;

		rs = &runstats[i];

		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			if (rs->max_run[ddir])
				rs->agg[ddir] = (rs->iobytes[ddir] * 1000) /
						rs->max_run[ddir];
		}
	}

	for (i = 0; i < FIO_OUTPUT_NR; i++)
		buf_output_init(&output[i]);

	/*
	 * don't overwrite last signal output
	 */
	if (output_format & FIO_OUTPUT_NORMAL)
		log_buf(&output[__FIO_OUTPUT_NORMAL], "\n");
	if (output_format & FIO_OUTPUT_JSON) {
		struct thread_data *global;
		char time_buf[32];
		struct timeval now;
		unsigned long long ms_since_epoch;
		time_t tv_sec;

		gettimeofday(&now, NULL);
		ms_since_epoch = (unsigned long long)(now.tv_sec) * 1000 +
		                 (unsigned long long)(now.tv_usec) / 1000;

		tv_sec = now.tv_sec;
		os_ctime_r(&tv_sec, time_buf, sizeof(time_buf));
		if (time_buf[strlen(time_buf) - 1] == '\n')
			time_buf[strlen(time_buf) - 1] = '\0';

		root = json_create_object();
		json_object_add_value_string(root, "fio version", fio_version_string);
		json_object_add_value_int(root, "timestamp", now.tv_sec);
		json_object_add_value_int(root, "timestamp_ms", ms_since_epoch);
		json_object_add_value_string(root, "time", time_buf);
		global = get_global_options();
		json_add_job_opts(root, "global options", &global->opt_list);
		array = json_create_array();
		json_object_add_value_array(root, "jobs", array);
	}

	if (is_backend)
		fio_server_send_job_options(&get_global_options()->opt_list, -1U);

	for (i = 0; i < nr_ts; i++) {
		ts = &threadstats[i];
		rs = &runstats[ts->groupid];

		if (is_backend) {
			fio_server_send_job_options(opt_lists[i], i);
			fio_server_send_ts(ts, rs);
		} else {
			if (output_format & FIO_OUTPUT_TERSE)
				show_thread_status_terse(ts, rs, &output[__FIO_OUTPUT_TERSE]);
			if (output_format & FIO_OUTPUT_JSON) {
				struct json_object *tmp = show_thread_status_json(ts, rs, opt_lists[i]);
				json_array_add_value_object(array, tmp);
			}
			if (output_format & FIO_OUTPUT_NORMAL)
				show_thread_status_normal(ts, rs, &output[__FIO_OUTPUT_NORMAL]);
		}
	}
	if (!is_backend && (output_format & FIO_OUTPUT_JSON)) {
		/* disk util stats, if any */
		show_disk_util(1, root, &output[__FIO_OUTPUT_JSON]);

		show_idle_prof_stats(FIO_OUTPUT_JSON, root, &output[__FIO_OUTPUT_JSON]);

		json_print_object(root, &output[__FIO_OUTPUT_JSON]);
		log_buf(&output[__FIO_OUTPUT_JSON], "\n");
		json_free_object(root);
	}

	for (i = 0; i < groupid + 1; i++) {
		rs = &runstats[i];

		rs->groupid = i;
		if (is_backend)
			fio_server_send_gs(rs);
		else if (output_format & FIO_OUTPUT_NORMAL)
			show_group_stats(rs, &output[__FIO_OUTPUT_NORMAL]);
	}

	if (is_backend)
		fio_server_send_du();
	else if (output_format & FIO_OUTPUT_NORMAL) {
		show_disk_util(0, NULL, &output[__FIO_OUTPUT_NORMAL]);
		show_idle_prof_stats(FIO_OUTPUT_NORMAL, NULL, &output[__FIO_OUTPUT_NORMAL]);
	}

	for (i = 0; i < FIO_OUTPUT_NR; i++) {
		struct buf_output *out = &output[i];

		log_info_buf(out->buf, out->buflen);
		buf_output_free(out);
	}

	log_info_flush();
	free(runstats);

	/* free arrays allocated by sum_thread_stats(), if any */
	for (i = 0; i < nr_ts; i++) {
		ts = &threadstats[i];
		free_clat_prio_stats(ts);
	}
	free(threadstats);
	free(opt_lists);
}

/*
 * [한국어]
 * __show_running_run_stats - 실행 중 중간 통계 스냅샷 출력 (ETA/USR1 시그널/status 파일 트리거)
 *
 * @return: 항상 0.
 *
 * 처리:
 *   1) 각 td 에 update_rusage=1 설정하고 rusage_sem 다운 (워커가 rusage 를 업데이트한 뒤 신호).
 *      — stat_sem 밖에서 수행해야 워커-stat 스레드 간 데드락 방지.
 *   2) stat_sem 다운 — __show_run_stats 와 상호 배제.
 *   3) 현재까지의 runtime 을 io_bytes 와 함께 ts 에 임시 주입 (rt[] 에 저장).
 *   4) __show_run_stats 호출.
 *   5) 임시 주입한 runtime 을 원복 (원본 상태 보존).
 *   6) stat_sem 업.
 *
 * 실행 컨텍스트: 헬퍼 스레드 또는 시그널 전달 경로. stat_sem 과 rusage_sem 으로 동기화.
 */
int __show_running_run_stats(void)
{
	unsigned long long *rt;
	struct timespec ts;

	rt = malloc(thread_number * sizeof(unsigned long long));
	fio_gettime(&ts, NULL);

	/*
	 * Collect rusage from workers outside stat_sem to prevent deadlock caused
	 * by semaphore contention between the stat thread and the worker threads.
	 */
	for_each_td(td) {
		if (td->runstate >= TD_EXITED)
			continue;

		if (td->rusage_sem) {
			td->update_rusage = 1;
			/* Prevent deadlock if worker exits between first check and sem_down */
			if (td->runstate >= TD_EXITED) {
				td->update_rusage = 0;
				continue;
			}
			fio_sem_down(td->rusage_sem);
		}
		td->update_rusage = 0;
	} end_for_each();

	fio_sem_down(stat_sem);

	for_each_td(td) {
		if (td->runstate >= TD_EXITED)
			continue;
	
		for_each_rw_ddir(ddir) {
			td->ts.io_bytes[ddir] = td->io_bytes[ddir];
		}
		td->ts.total_run_time = mtime_since(&td->epoch, &ts);

		rt[__td_index] = mtime_since(&td->start, &ts);
		if (td_read(td) && td->ts.io_bytes[DDIR_READ])
			td->ts.runtime[DDIR_READ] += rt[__td_index];
		if (td_write(td) && td->ts.io_bytes[DDIR_WRITE])
			td->ts.runtime[DDIR_WRITE] += rt[__td_index];
		if (td_trim(td) && td->ts.io_bytes[DDIR_TRIM])
			td->ts.runtime[DDIR_TRIM] += rt[__td_index];
	} end_for_each();

	__show_run_stats();

	for_each_td(td) {
		if (td->runstate >= TD_EXITED)
			continue;

		if (td_read(td) && td->ts.io_bytes[DDIR_READ])
			td->ts.runtime[DDIR_READ] -= rt[__td_index];
		if (td_write(td) && td->ts.io_bytes[DDIR_WRITE])
			td->ts.runtime[DDIR_WRITE] -= rt[__td_index];
		if (td_trim(td) && td->ts.io_bytes[DDIR_TRIM])
			td->ts.runtime[DDIR_TRIM] -= rt[__td_index];
	} end_for_each();

	free(rt);
	fio_sem_up(stat_sem);

	return 0;
}

static bool status_file_disabled;

#define FIO_STATUS_FILE		"fio-dump-status"

/*
 * [한국어]
 * check_status_file - /tmp/fio-dump-status 파일 존재 여부 확인 및 삭제
 *
 * @return: 1 = 파일 발견하고 삭제 성공 (통계 덤프 트리거), 0 = 파일 없음.
 *
 * 사용자가 외부에서 "touch /tmp/fio-dump-status" 로 fio 에 중간 통계 덤프를 요청하는
 * 인터페이스. stat(2) 로 파일 존재 확인 후 unlink(2) 로 삭제.
 * TMPDIR/TEMP 환경변수를 먼저 확인하고 기본값 "/tmp".
 * 삭제 실패 시 status_file_disabled=true 로 이후 비활성화 (무한 덤프 방지).
 */
static int check_status_file(void)
{
	struct stat sb;
	const char *temp_dir;
	char fio_status_file_path[PATH_MAX];

	if (status_file_disabled)
		return 0;

	temp_dir = getenv("TMPDIR");
	if (temp_dir == NULL) {
		temp_dir = getenv("TEMP");
		if (temp_dir && strlen(temp_dir) >= PATH_MAX)
			temp_dir = NULL;
	}
	if (temp_dir == NULL)
		temp_dir = "/tmp";
#ifdef __COVERITY__
	__coverity_tainted_data_sanitize__(temp_dir);
#endif

	snprintf(fio_status_file_path, sizeof(fio_status_file_path), "%s/%s", temp_dir, FIO_STATUS_FILE);

	if (stat(fio_status_file_path, &sb))
		return 0;

	if (unlink(fio_status_file_path) < 0) {
		log_err("fio: failed to unlink %s: %s\n", fio_status_file_path,
							strerror(errno));
		log_err("fio: disabling status file updates\n");
		status_file_disabled = true;
	}

	return 1;
}

/*
 * [한국어]
 * check_for_running_stats - 중간 통계 덤프 트리거 파일 감지 시 show_running_run_stats 호출
 *
 * 헬퍼 스레드(helper_thread.c) 의 주기 루프에서 호출되어 fio-dump-status 파일을 체크.
 */
void check_for_running_stats(void)
{
	if (check_status_file()) {
		show_running_run_stats();
		return;
	}
}

/*
 * [한국어]
 * add_stat_sample - Welford 온라인 알고리즘으로 io_stat 에 샘플 1개 추가
 *
 * @is: 누적기.
 * @data: 샘플 값.
 *
 * Welford 알고리즘 (Knuth AoCP vol2 4.2.2.B, Welford 1962):
 *   delta = x - mean
 *   mean += delta / n_new
 *   S    += delta * (x - new_mean)
 * 이 방식은 한 번의 스캔으로 평균과 분산을 O(1) 공간에 구할 수 있고,
 * naive sum 방식에 비해 수치 안정성(특히 catastrophic cancellation 회피) 이 우수.
 * 최종 분산 = S/(n-1) 로 얻어진다.
 *
 * min/max 는 단순 비교 갱신.
 * delta 가 0 이면 mean/S 갱신 생략 — 소수점 연산 회피 최적화.
 *
 * 실행 컨텍스트: 잡 스레드에서 add_clat/slat/lat_sample 을 통해 호출 — 락 불필요.
 */
static inline void add_stat_sample(struct io_stat *is, unsigned long long data)
{
	double val = data;                 /* [한국어] 정수→double 변환 (Welford 연산용) */
	double delta;

	if (data > is->max_val)
		is->max_val = data;            /* [한국어] 최대값 갱신 */
	if (data < is->min_val)
		is->min_val = data;            /* [한국어] 최소값 갱신 (ULONG_MAX 로 초기화되어 있어 첫 샘플이 갱신) */

	delta = val - is->mean.u.f;        /* [한국어] x - mean (fio_fp64_t.u.f 는 double 값) */
	if (delta) {
		is->mean.u.f += delta / (is->samples + 1.0);   /* [한국어] mean_new = mean_old + delta/n_new */
		is->S.u.f += delta * (val - is->mean.u.f);     /* [한국어] S += delta * (x - mean_new) — Welford 핵심 */
	}

	is->samples++;                     /* [한국어] 샘플 카운트 증가 */
}

/*
 * [한국어]
 * add_stat_prio_sample - cmdprio 별 per-priority 통계에 샘플 추가
 *
 * @clat_prio: per-prio 배열 (NULL 이면 조용히 스킵).
 * @clat_prio_index: 배열 내 인덱스 (이미 매핑되어 있어야 함).
 * @nsec: 레이턴시 샘플.
 */
static inline void add_stat_prio_sample(struct clat_prio_stat *clat_prio,
					unsigned short clat_prio_index,
					unsigned long long nsec)
{
	if (clat_prio)
		add_stat_sample(&clat_prio[clat_prio_index].clat_stat, nsec);
}

/*
 * Return a struct io_logs, which is added to the tail of the log
 * list for 'iolog'.
 */
/*
 * [한국어]
 * get_new_log - 새로운 io_logs 청크를 할당해 iolog 의 tail 에 추가
 *
 * @iolog: 대상 io_log.
 * @return: 새 io_logs, 실패 시 NULL.
 *
 * 청크 크기 정책:
 *   - 첫 청크: td->o.log_entries 또는 DEF_LOG_ENTRIES (기본값).
 *   - 이후: 지수 두 배 증가(amortized O(1) append), MAX_LOG_ENTRIES 상한.
 *   - 청크 자체는 smalloc(공유 메모리), 배열은 calloc (per-thread 로컬).
 */
static struct io_logs *get_new_log(struct io_log *iolog)
{
	size_t new_samples;
	struct io_logs *cur_log;

	/*
	 * Cap the size at MAX_LOG_ENTRIES, so we don't keep doubling
	 * forever
	 */
	if (!iolog->cur_log_max) {
		if (iolog->td)
			new_samples = iolog->td->o.log_entries;
		else
			new_samples = DEF_LOG_ENTRIES;
	} else {
		new_samples = iolog->cur_log_max * 2;
		if (new_samples > MAX_LOG_ENTRIES)
			new_samples = MAX_LOG_ENTRIES;
	}

	cur_log = smalloc(sizeof(*cur_log));
	if (cur_log) {
		INIT_FLIST_HEAD(&cur_log->list);
		cur_log->log = calloc(new_samples, log_entry_sz(iolog));
		if (cur_log->log) {
			cur_log->nr_samples = 0;
			cur_log->max_samples = new_samples;
			flist_add_tail(&cur_log->list, &iolog->io_logs);
			iolog->cur_log_max = new_samples;
			return cur_log;
		}
		sfree(cur_log);
	}

	return NULL;
}

/*
 * Add and return a new log chunk, or return current log if big enough
 */
/*
 * [한국어]
 * regrow_log - 현재 청크가 가득 찼으면 새 청크 추가 (필요 시 pending 미리 flush)
 *
 * @iolog: 대상 io_log (disabled=true 면 조기 종료).
 * @return: 사용 가능한 io_logs, 실패 시 NULL.
 *
 * log_gz 가 활성화되어 있으면 가득 찬 청크를 gzip 압축 스레드에 넘긴다(iolog_cur_flush).
 * pending 버퍼에 임시 저장된 샘플이 있으면 새 청크로 이동시켜 순서 보존.
 */
static struct io_logs *regrow_log(struct io_log *iolog)
{
	struct io_logs *cur_log;
	int i;

	if (!iolog || iolog->disabled)
		goto disable;

	cur_log = iolog_cur_log(iolog);
	if (!cur_log) {
		cur_log = get_new_log(iolog);
		if (!cur_log)
			return NULL;
	}

	if (cur_log->nr_samples < cur_log->max_samples)
		return cur_log;

	/*
	 * No room for a new sample. If we're compressing on the fly, flush
	 * out the current chunk
	 */
	if (iolog->log_gz) {
		if (iolog_cur_flush(iolog, cur_log)) {
			log_err("fio: failed flushing iolog! Will stop logging.\n");
			return NULL;
		}
	}

	/*
	 * Get a new log array, and add to our list
	 */
	cur_log = get_new_log(iolog);
	if (!cur_log) {
		log_err("fio: failed extending iolog! Will stop logging.\n");
		return NULL;
	}

	if (!iolog->pending || !iolog->pending->nr_samples)
		return cur_log;

	/*
	 * Flush pending items to new log
	 */
	for (i = 0; i < iolog->pending->nr_samples; i++) {
		struct io_sample *src, *dst;

		src = get_sample(iolog, iolog->pending, i);
		dst = get_sample(iolog, cur_log, i);
		memcpy(dst, src, log_entry_sz(iolog));
	}
	cur_log->nr_samples = iolog->pending->nr_samples;

	iolog->pending->nr_samples = 0;
	return cur_log;
disable:
	if (iolog)
		iolog->disabled = true;
	return NULL;
}

/*
 * [한국어]
 * regrow_logs - 잡의 모든 iolog (slat/clat/clat_hist/lat/bw/iops) 확장
 *
 * TD_F_REGROW_LOGS 플래그가 세팅된 경우 백엔드 루프에서 호출되어 일괄 확장.
 */
void regrow_logs(struct thread_data *td)
{
	regrow_log(td->slat_log);
	regrow_log(td->clat_log);
	regrow_log(td->clat_hist_log);
	regrow_log(td->lat_log);
	regrow_log(td->bw_log);
	regrow_log(td->iops_log);
	td->flags &= ~TD_F_REGROW_LOGS;
}

/*
 * [한국어]
 * regrow_agg_logs - 전역 aggregate iolog (agg_io_log[]) 확장
 *
 * group_reporting/unified 잡이 공유하는 aggregate 로그의 확장 경로.
 */
void regrow_agg_logs(void)
{
	enum fio_ddir ddir;

	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
		regrow_log(agg_io_log[ddir]);
}

/*
 * [한국어]
 * get_cur_log - 쓰기 가능한 iolog 청크 반환 (가득 차면 regrow 또는 pending 반환)
 *
 * IO_MODE_OFFLOAD 또는 per_unit_log 가 아닐 때는 즉시 regrow_log 호출.
 * 그 외(잡 스레드가 직접 제출 중) 는 TD_F_REGROW_LOGS 플래그만 세팅하고 pending 에 임시 저장.
 * 이 분리는 제출 핫패스에서 malloc 호출을 피하기 위함.
 */
static struct io_logs *get_cur_log(struct io_log *iolog)
{
	struct io_logs *cur_log;

	cur_log = iolog_cur_log(iolog);
	if (!cur_log) {
		cur_log = get_new_log(iolog);
		if (!cur_log)
			return NULL;
	}

	if (cur_log->nr_samples < cur_log->max_samples)
		return cur_log;

	/*
	 * Out of space. If we're in IO offload mode, or we're not doing
	 * per unit logging (hence logging happens outside of the IO thread
	 * as well), add a new log chunk inline. If we're doing inline
	 * submissions, flag 'td' as needing a log regrow and we'll take
	 * care of it on the submission side.
	 */
	if ((iolog->td && iolog->td->o.io_submit_mode == IO_MODE_OFFLOAD) ||
	    !per_unit_log(iolog))
		return regrow_log(iolog);

	if (iolog->td)
		iolog->td->flags |= TD_F_REGROW_LOGS;
	if (iolog->pending)
		assert(iolog->pending->nr_samples < iolog->pending->max_samples);
	return iolog->pending;
}

/*
 * [한국어]
 * __add_log_sample - iolog 에 샘플 1개 기록 (핵심 내부 함수)
 *
 * @iolog: 대상 io_log.
 * @t: 샘플 시각(msec 또는 epoch 기준).
 * @sample: 입력 샘플 데이터.
 *
 * get_cur_log 로 쓸 수 있는 청크를 얻어 io_sample 항목을 append.
 * log_offset / log_issue_time / log_alternate_epoch 옵션에 따라 aux 필드 기록.
 * 공간 부족으로 청크 확장도 실패하면 iolog->disabled=true 로 이후 비활성화.
 */
static void __add_log_sample(struct io_log *iolog, unsigned long t,
			     struct log_sample *sample)
{
	struct io_logs *cur_log;

	if (iolog->disabled)
		return;
	if (flist_empty(&iolog->io_logs))
		iolog->avg_last[sample->ddir] = t;

	cur_log = get_cur_log(iolog);
	if (cur_log) {
		struct io_sample *s;

		s = get_sample(iolog, cur_log, cur_log->nr_samples);

		s->data = sample->data;
		s->time = t;
		if (iolog->td && iolog->td->o.log_alternate_epoch)
			s->time += iolog->td->alternate_epoch;
		io_sample_set_ddir(iolog, s, sample->ddir);
		s->bs = sample->bs;
		s->priority = sample->priority;

		if (iolog->log_offset)
			s->aux[IOS_AUX_OFFSET_INDEX] = sample->offset;

		if (iolog->log_issue_time)
			s->aux[IOS_AUX_ISSUE_TIME_INDEX] = sample->issue_time;

		cur_log->nr_samples++;
		return;
	}

	iolog->disabled = true;
}

/*
 * [한국어]
 * reset_io_stat - io_stat Welford 누적기를 초기 상태로 되돌림
 *
 * min_val = ULONG_MAX (첫 샘플이 무조건 갱신되도록), 나머지는 0.
 */
static inline void reset_io_stat(struct io_stat *ios)
{
	ios->min_val = -1ULL;                        /* [한국어] 부호없는 최댓값 = 0xFFFFFFFFFFFFFFFF */
	ios->max_val = ios->samples = 0;
	ios->mean.u.f = ios->S.u.f = 0;
}

/*
 * [한국어]
 * reset_io_u_plat - 히스토그램 버킷 배열 전체 0 초기화
 */
static inline void reset_io_u_plat(uint64_t *io_u_plat)
{
	int i;

	for (i = 0; i < FIO_IO_U_PLAT_NR; i++)
		io_u_plat[i] = 0;
}

/*
 * [한국어]
 * reset_clat_prio_stats - cmdprio 별 per-priority 누적 초기화
 *
 * 각 방향의 clat_prio 배열 모든 슬롯을 순회해 clat_stat 과 io_u_plat 리셋.
 */
static inline void reset_clat_prio_stats(struct thread_stat *ts)
{
	enum fio_ddir ddir;
	int i;

	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
		if (!ts->clat_prio[ddir])
			continue;

		for (i = 0; i < ts->nr_clat_prio[ddir]; i++) {
			reset_io_stat(&ts->clat_prio[ddir][i].clat_stat);
			reset_io_u_plat(ts->clat_prio[ddir][i].io_u_plat);
		}
	}
}

/*
 * [한국어]
 * reset_io_stats - 잡의 모든 누적 통계를 0 으로 리셋
 *
 * 용도:
 *   - latency_target 자동 탐색에서 이전 시도 결과 폐기
 *   - ramp_time 종료 후 "실제 측정" 구간 진입 시 워밍업 샘플 배제
 *   - 중간 보고 후 윈도우 리셋
 *
 * 리셋 항목:
 *   - clat/slat/lat/bw/iops_stat (ddir 별)
 *   - io_bytes/runtime/total_io_u/short_io_u/drop_io_u
 *   - io_u_plat 히스토그램 버킷 (FIO_LAT_CNT x DDIR x PLAT_NR)
 *   - cmdprio 별 per-priority 통계
 *   - sync_stat / io_u_sync_plat
 *   - io_u_map / io_u_submit / io_u_complete (큐 깊이 분포)
 *   - io_u_lat_n/u/m (ns/us/ms 분포)
 *   - total_submit / total_complete / zone resets / cachehit
 */
void reset_io_stats(struct thread_data *td)
{
	struct thread_stat *ts = &td->ts;
	int i, j;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		reset_io_stat(&ts->clat_stat[i]);
		reset_io_stat(&ts->slat_stat[i]);
		reset_io_stat(&ts->lat_stat[i]);
		reset_io_stat(&ts->bw_stat[i]);
		reset_io_stat(&ts->iops_stat[i]);

		ts->io_bytes[i] = 0;
		ts->runtime[i] = 0;
		ts->total_io_u[i] = 0;
		ts->short_io_u[i] = 0;
		ts->drop_io_u[i] = 0;
	}

	for (i = 0; i < FIO_LAT_CNT; i++)
		for (j = 0; j < DDIR_RWDIR_CNT; j++)
			reset_io_u_plat(ts->io_u_plat[i][j]);

	reset_clat_prio_stats(ts);

	ts->total_io_u[DDIR_SYNC] = 0;
	reset_io_u_plat(ts->io_u_sync_plat);

	for (i = 0; i < FIO_IO_U_MAP_NR; i++) {
		ts->io_u_map[i] = 0;
		ts->io_u_submit[i] = 0;
		ts->io_u_complete[i] = 0;
	}

	for (i = 0; i < FIO_IO_U_LAT_N_NR; i++)
		ts->io_u_lat_n[i] = 0;
	for (i = 0; i < FIO_IO_U_LAT_U_NR; i++)
		ts->io_u_lat_u[i] = 0;
	for (i = 0; i < FIO_IO_U_LAT_M_NR; i++)
		ts->io_u_lat_m[i] = 0;

	ts->total_submit = 0;
	ts->total_complete = 0;
	ts->nr_zone_resets = 0;
	ts->cachehit = ts->cachemiss = 0;
}

/*
 * [한국어]
 * __add_stat_to_log - 윈도우 평균/최대를 로그 샘플로 기록 (log_avg_msec 경계)
 *
 * @iolog: 대상 io_log.
 * @ddir: 방향.
 * @elapsed: 잡 시작 후 경과 ms.
 * @log_max: IO_LOG_SAMPLE_AVG(평균만) / IO_LOG_SAMPLE_MAX(최대만) / 그 외(둘 다).
 *
 * avg_window 에 누적된 Welford 통계의 mean(0.5 반올림)/max 를 io_sample 로 기록.
 * 기록 후 reset_io_stat 으로 윈도우 리셋.
 */
static void __add_stat_to_log(struct io_log *iolog, enum fio_ddir ddir,
			      unsigned long elapsed, int log_max)
{
	/*
	 * Note an entry in the log. Use the mean from the logged samples,
	 * making sure to properly round up. Only write a log entry if we
	 * had actual samples done.
	 */
	if (iolog->avg_window[ddir].samples) {
		struct log_sample sample = { {{ 0, 0 }}, ddir, 0, 0, 0, 0 };
		union io_sample_data *d = &sample.data;

		if (log_max == IO_LOG_SAMPLE_AVG) {
			d->val.val0 = iolog->avg_window[ddir].mean.u.f + 0.50;
			d->val.val1 = 0;
		} else if (log_max == IO_LOG_SAMPLE_MAX) {
			d->val.val0 = iolog->avg_window[ddir].max_val;
			d->val.val1 = 0;
		} else {
			d->val.val0 = iolog->avg_window[ddir].mean.u.f + 0.50;
			d->val.val1 = iolog->avg_window[ddir].max_val;
		}

		__add_log_sample(iolog, elapsed, &sample);
	}

	reset_io_stat(&iolog->avg_window[ddir]);
}

/* [한국어] _add_stat_to_log - 모든 ddir 에 대해 __add_stat_to_log 호출 (윈도우 flush) */
static void _add_stat_to_log(struct io_log *iolog, unsigned long elapsed,
			     int log_max)
{
	enum fio_ddir ddir;

	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
		__add_stat_to_log(iolog, ddir, elapsed, log_max);
}

/*
 * [한국어]
 * add_log_sample - 로그 샘플 추가 디스패처 (평균 윈도우 모드 / 즉시 기록 모드)
 *
 * @td: 잡.
 * @iolog: 대상 io_log.
 * @sample: 샘플.
 * @return: 다음 윈도우까지 대기할 ms (0 = 즉시).
 *
 * log_avg_msec=0 이면 매 샘플 즉시 __add_log_sample 호출.
 * log_avg_msec>0 이면:
 *   - avg_window[ddir] 에 add_stat_sample 로 Welford 누적.
 *   - 마지막 기록 시각(avg_last[ddir]) 이후 avg_msec 이 경과하면 __add_stat_to_log 로 평균 기록.
 *   - LOG_MSEC_SLACK 보정으로 타이머 해상도 이슈 완화.
 */
static unsigned long add_log_sample(struct thread_data *td,
				    struct io_log *iolog,
				    struct log_sample *sample)
{
	unsigned long elapsed, this_window;
	enum fio_ddir ddir = sample->ddir;

	if (!ddir_rw(ddir))
		return 0;

	elapsed = mtime_since_now(&td->epoch);

	/*
	 * If no time averaging, just add the log sample.
	 */
	if (!iolog->avg_msec) {
		__add_log_sample(iolog, elapsed, sample);
		return 0;
	}

	/*
	 * Add the sample. If the time period has passed, then
	 * add that entry to the log and clear.
	 */
	add_stat_sample(&iolog->avg_window[ddir], sample->data.val.val0);

	/*
	 * If period hasn't passed, adding the above sample is all we
	 * need to do.
	 */
	this_window = elapsed - iolog->avg_last[ddir];
	if (elapsed < iolog->avg_last[ddir])
		return iolog->avg_last[ddir] - elapsed;
	else if (this_window < iolog->avg_msec) {
		unsigned long diff = iolog->avg_msec - this_window;

		if (inline_log(iolog) || diff > LOG_MSEC_SLACK)
			return diff;
	}

	__add_stat_to_log(iolog, ddir, elapsed, td->o.log_max);

	iolog->avg_last[ddir] = elapsed - (elapsed % iolog->avg_msec);

	return iolog->avg_msec;
}

/*
 * [한국어]
 * finalize_logs - 잡 종료 시 남은 log_avg_msec 윈도우 샘플을 강제 flush
 *
 * @td: 잡.
 * @unit_logs: true = per-unit (clat/slat/lat) 대상, false = aggregate 대상.
 *
 * 종료 순간 미기록된 부분 윈도우의 평균값을 마지막 한 줄로 기록하여 데이터 유실 방지.
 */
void finalize_logs(struct thread_data *td, bool unit_logs)
{
	unsigned long elapsed;

	elapsed = mtime_since_now(&td->epoch);

	if (td->clat_log && unit_logs)
		_add_stat_to_log(td->clat_log, elapsed, td->o.log_max);
	if (td->slat_log && unit_logs)
		_add_stat_to_log(td->slat_log, elapsed, td->o.log_max);
	if (td->lat_log && unit_logs)
		_add_stat_to_log(td->lat_log, elapsed, td->o.log_max);
	if (td->bw_log && (unit_logs == per_unit_log(td->bw_log)))
		_add_stat_to_log(td->bw_log, elapsed, td->o.log_max);
	if (td->iops_log && (unit_logs == per_unit_log(td->iops_log)))
		_add_stat_to_log(td->iops_log, elapsed, td->o.log_max);
}

/*
 * [한국어]
 * add_agg_sample - aggregate io_log 에 샘플 추가 (group_reporting 공유 로그)
 *
 * @data: 샘플 값 (bw/iops).
 * @ddir: 방향.
 * @bs: 블록 크기.
 *
 * 시각은 genesis(전역 시작 시각) 기준 — 여러 잡이 하나의 로그에 기록하므로 공통 시간축 필요.
 */
void add_agg_sample(union io_sample_data data, enum fio_ddir ddir,
		    unsigned long long bs)
{
	struct io_log *iolog;
	struct log_sample sample = { data, ddir, bs, 0, 0, 0 };

	if (!ddir_rw(ddir))
		return;

	iolog = agg_io_log[ddir];
	__add_log_sample(iolog, mtime_since_genesis(), &sample);
}

/*
 * [한국어]
 * add_sync_clat_sample - fsync/fdatasync/sync_file_range/syncfs 의 완료 레이턴시 샘플 추가
 *
 * @ts: thread_stat.
 * @nsec: 완료 레이턴시 (ns).
 *
 * 일반 read/write 와 분리된 io_u_sync_plat 히스토그램과 sync_stat Welford 누적기에 기록.
 */
void add_sync_clat_sample(struct thread_stat *ts, unsigned long long nsec)
{
	unsigned int idx = plat_val_to_idx(nsec);
	assert(idx < FIO_IO_U_PLAT_NR);

	ts->io_u_sync_plat[idx]++;
	add_stat_sample(&ts->sync_stat, nsec);
}

/*
 * [한국어]
 * add_lat_percentile_sample - 퍼센타일 히스토그램 버킷에 샘플 추가
 *
 * @ts: thread_stat.
 * @nsec: 레이턴시 샘플.
 * @ddir: 방향.
 * @lat: FIO_SLAT / FIO_CLAT / FIO_LAT (세 가지 레이턴시 종류 분리 버킷).
 *
 * plat_val_to_idx 로 버킷 인덱스 계산 후 카운터 증가. 이 배열이 calc_clat_percentiles 의 입력.
 */
static inline void add_lat_percentile_sample(struct thread_stat *ts,
					     unsigned long long nsec,
					     enum fio_ddir ddir,
					     enum fio_lat lat)
{
	unsigned int idx = plat_val_to_idx(nsec);
	assert(idx < FIO_IO_U_PLAT_NR);

	ts->io_u_plat[lat][ddir][idx]++;
}

/*
 * [한국어]
 * add_lat_percentile_prio_sample - cmdprio 별 퍼센타일 히스토그램에 샘플 추가
 *
 * clat_prio 가 설정된 경우에만 해당 prio 슬롯의 io_u_plat 에 카운터 추가.
 */
static inline void
add_lat_percentile_prio_sample(struct thread_stat *ts, unsigned long long nsec,
			       enum fio_ddir ddir,
			       unsigned short clat_prio_index)
{
	unsigned int idx = plat_val_to_idx(nsec);

	if (ts->clat_prio[ddir])
		ts->clat_prio[ddir][clat_prio_index].io_u_plat[idx]++;
}

/*
 * [한국어]
 * add_clat_sample - 완료 레이턴시(clat) 샘플을 모든 경로에 분배 (fio 의 중심 훅)
 *
 * @td: 잡.
 * @ddir: 방향.
 * @nsec: 완료 레이턴시 (ns).
 * @bs: I/O 블록 크기.
 * @io_u: 완료된 io_u (optional — offset/ioprio/clat_prio_index 참조).
 *
 * 분배 경로:
 *   1) add_stat_sample → clat_stat[ddir] Welford 누적
 *   2) (lat_percentiles=0 일 때) add_stat_prio_sample → cmdprio 별 누적
 *   3) td->clat_log 활성 시 add_log_sample → iolog 기록
 *   4) clat_percentiles=1 일 때 add_lat_percentile_sample → 히스토그램 버킷
 *      (lat_percentiles=0 인 경우에만 add_lat_percentile_prio_sample 도 호출)
 *   5) clat_hist_log 활성 시 hist_msec 윈도우가 경과하면 현재 io_u_plat 전체 복사본을
 *      플로팅용 로그 샘플로 기록 (log_hist_msec 옵션, 시간별 분포 변화 추적)
 *
 * 동기화: td_async_processing 이면 __td_io_u_lock/unlock 사용 (offload 모드 등 멀티스레드 td).
 *
 * lat_percentiles vs clat_percentiles:
 *   - 기본 clat_percentiles=1 : 완료 레이턴시로 퍼센타일 계산
 *   - lat_percentiles=1       : 총 레이턴시로 퍼센타일 계산 (cmdprio 통계도 lat 기반)
 *
 * 호출 체인:
 *   account_io_completion [io_u.c] → [add_clat_sample]
 */
void add_clat_sample(struct thread_data *td, enum fio_ddir ddir,
		     unsigned long long nsec, unsigned long long bs,
		     struct io_u *io_u)
{
	const bool needs_lock = td_async_processing(td);
	unsigned long elapsed, this_window;
	struct thread_stat *ts = &td->ts;
	struct io_log *iolog = td->clat_hist_log;
	uint64_t offset = 0;
	unsigned int ioprio = 0;
	unsigned short clat_prio_index = 0;

	if (needs_lock)
		__td_io_u_lock(td);

	if (io_u) {
		offset = io_u->offset;
		ioprio = io_u->ioprio;
		clat_prio_index = io_u->clat_prio_index;
	}

	add_stat_sample(&ts->clat_stat[ddir], nsec);

	/*
	 * When lat_percentiles=1 (default 0), the reported per priority
	 * percentiles and stats are used for describing total latency values,
	 * even though the variable names themselves start with clat_.
	 *
	 * Because of the above definition, add a prio stat sample only when
	 * lat_percentiles=0. add_lat_sample() will add the prio stat sample
	 * when lat_percentiles=1.
	 */
	if (!ts->lat_percentiles)
		add_stat_prio_sample(ts->clat_prio[ddir], clat_prio_index,
				     nsec);

	if (td->clat_log) {
		struct log_sample sample = { sample_val(nsec), ddir, bs,
			offset, ioprio, 0 };

		if (io_u)
			sample.issue_time =
				ntime_since(&td->epoch, &io_u->issue_time);

		add_log_sample(td, td->clat_log, &sample);
	}

	if (ts->clat_percentiles) {
		/*
		 * Because of the above definition, add a prio lat percentile
		 * sample only when lat_percentiles=0. add_lat_sample() will add
		 * the prio lat percentile sample when lat_percentiles=1.
		 */
		add_lat_percentile_sample(ts, nsec, ddir, FIO_CLAT);
		if (!ts->lat_percentiles)
			add_lat_percentile_prio_sample(ts, nsec, ddir,
						       clat_prio_index);
	}

	if (iolog && iolog->hist_msec) {
		struct io_hist *hw = &iolog->hist_window[ddir];

		hw->samples++;
		elapsed = mtime_since_now(&td->epoch);
		if (!hw->hist_last)
			hw->hist_last = elapsed;
		this_window = elapsed - hw->hist_last;

		if (this_window >= iolog->hist_msec) {
			uint64_t *io_u_plat;
			struct io_u_plat_entry *dst;
			struct log_sample sample = { {{ 0, 0 }}, ddir, bs,
				offset, ioprio, 0 };

			/*
			 * Make a byte-for-byte copy of the latency histogram
			 * stored in td->ts.io_u_plat[ddir], recording it in a
			 * log sample. Note that the matching call to free() is
			 * located in iolog.c after printing this sample to the
			 * log file.
			 */
			io_u_plat = (uint64_t *) td->ts.io_u_plat[FIO_CLAT][ddir];
			dst = malloc(sizeof(struct io_u_plat_entry));
			memcpy(&(dst->io_u_plat), io_u_plat,
				FIO_IO_U_PLAT_NR * sizeof(uint64_t));
			flist_add(&dst->list, &hw->list);

			sample.data = sample_plat(dst);
			__add_log_sample(iolog, elapsed, &sample);

			/*
			 * Update the last time we recorded as being now, minus
			 * any drift in time we encountered before actually
			 * making the record.
			 */
			hw->hist_last = elapsed - (this_window - iolog->hist_msec);
			hw->samples = 0;
		}
	}

	if (needs_lock)
		__td_io_u_unlock(td);
}

/*
 * [한국어]
 * add_slat_sample - 제출 레이턴시(slat = queue()→completion 시작) 샘플 추가
 *
 * @td: 잡.
 * @io_u: io_u — start_time/issue_time 필드에서 계산.
 *
 * slat = issue_time - start_time (잡이 io_u 를 준비해 엔진에 제출하기까지 걸린 시간).
 * clat_stat 과 별도의 slat_stat 에 누적되며, slat_percentiles=1 이면 히스토그램도 기록.
 */
void add_slat_sample(struct thread_data *td, struct io_u *io_u)
{
	const bool needs_lock = td_async_processing(td);
	struct thread_stat *ts = &td->ts;
	enum fio_ddir ddir;
	unsigned long long nsec;

	ddir = io_u->ddir;
	if (!ddir_rw(ddir))
		return;

	if (needs_lock)
		__td_io_u_lock(td);

	nsec = ntime_since(&io_u->start_time, &io_u->issue_time);

	add_stat_sample(&ts->slat_stat[ddir], nsec);

	if (td->slat_log) {
		struct log_sample sample = { sample_val(nsec), ddir,
			io_u->xfer_buflen, io_u->offset, io_u->ioprio,
			ntime_since(&td->epoch, &io_u->issue_time) };

		add_log_sample(td, td->slat_log, &sample);
	}

	if (ts->slat_percentiles)
		add_lat_percentile_sample(ts, nsec, ddir, FIO_SLAT);

	if (needs_lock)
		__td_io_u_unlock(td);
}

/*
 * [한국어]
 * add_lat_sample - 총 레이턴시(lat = slat + clat) 샘플 추가
 *
 * @td: 잡.
 * @ddir: 방향.
 * @nsec: 총 레이턴시 (ns).
 * @bs: 블록 크기.
 * @io_u: 완료 io_u.
 *
 * lat_percentiles=1 일 때만 cmdprio 통계와 히스토그램을 lat 기반으로 기록한다.
 * (lat_percentiles=0 이면 add_clat_sample 에서 clat 기반으로 기록한 것으로 충분)
 */
void add_lat_sample(struct thread_data *td, enum fio_ddir ddir,
		    unsigned long long nsec, unsigned long long bs,
		    struct io_u * io_u)
{
	const bool needs_lock = td_async_processing(td);
	struct thread_stat *ts = &td->ts;

	if (!ddir_rw(ddir))
		return;

	if (needs_lock)
		__td_io_u_lock(td);

	add_stat_sample(&ts->lat_stat[ddir], nsec);

	if (td->lat_log) {
		struct log_sample sample = { sample_val(nsec), ddir, bs,
			io_u->offset, io_u->ioprio, 0 };

		add_log_sample(td, td->lat_log, &sample);
	}

	/*
	 * When lat_percentiles=1 (default 0), the reported per priority
	 * percentiles and stats are used for describing total latency values,
	 * even though the variable names themselves start with clat_.
	 *
	 * Because of the above definition, add a prio stat and prio lat
	 * percentile sample only when lat_percentiles=1. add_clat_sample() will
	 * add the prio stat and prio lat percentile sample when
	 * lat_percentiles=0.
	 */
	if (ts->lat_percentiles) {
		add_lat_percentile_sample(ts, nsec, ddir, FIO_LAT);
		add_lat_percentile_prio_sample(ts, nsec, ddir,
					       io_u->clat_prio_index);
		add_stat_prio_sample(ts->clat_prio[ddir], io_u->clat_prio_index,
				     nsec);
	}
	if (needs_lock)
		__td_io_u_unlock(td);
}

/*
 * [한국어]
 * add_bw_sample - 대역폭 샘플(bytes/sec) 1개 추가
 *
 * @td: 잡.
 * @io_u: io_u (ddir/offset/ioprio).
 * @bytes: 이 샘플이 측정한 전송 바이트.
 * @spent: 소요 시간 (us 단위 정수).
 *
 * rate = bytes * 1e6 / spent → bytes/sec (지난 구간의 평균).
 * bw_stat 에 Welford 누적 + bw_log 에 기록.
 * stat_io_bytes 는 다음 샘플 기준점 업데이트.
 */
void add_bw_sample(struct thread_data *td, struct io_u *io_u,
		   unsigned int bytes, unsigned long long spent)
{
	const bool needs_lock = td_async_processing(td);
	struct thread_stat *ts = &td->ts;
	unsigned long rate;

	if (spent)
		rate = (unsigned long) (bytes * 1000000ULL / spent);
	else
		rate = 0;

	if (needs_lock)
		__td_io_u_lock(td);

	add_stat_sample(&ts->bw_stat[io_u->ddir], rate);

	if (td->bw_log) {
		struct log_sample sample = { sample_val(rate), io_u->ddir,
			bytes, io_u->offset, io_u->ioprio, 0 };

		add_log_sample(td, td->bw_log, &sample);
	}

	td->stat_io_bytes[io_u->ddir] = td->this_io_bytes[io_u->ddir];

	if (needs_lock)
		__td_io_u_unlock(td);
}

/*
 * [한국어]
 * __add_samples - 주기적 bw/iops 샘플 계산의 공통 구현
 *
 * @td: 잡.
 * @parent_tv: 마지막 샘플 시각 (갱신됨).
 * @t: 현재 시각.
 * @avg_time: 평균 구간 (ms).
 * @this_io_bytes: 현재 누적 바이트/블록 수.
 * @stat_io_bytes: 마지막 샘플 시점의 누적 (delta 계산용).
 * @stat: io_stat 배열 (bw_stat 또는 iops_stat).
 * @log: io_log (bw_log 또는 iops_log).
 * @is_kb: true=KiB/s 단위, false=원 단위.
 * @return: 다음 샘플까지 대기할 ms.
 *
 * 헬퍼 스레드가 avg_time 주기로 호출. delta 기반으로 이 구간의 평균 rate 계산 후 샘플 추가.
 * LOG_MSEC_SLACK 보정으로 타이머 해상도 이슈 완화.
 */
static int __add_samples(struct thread_data *td, struct timespec *parent_tv,
			 struct timespec *t, unsigned int avg_time,
			 uint64_t *this_io_bytes, uint64_t *stat_io_bytes,
			 struct io_stat *stat, struct io_log *log,
			 bool is_kb)
{
	const bool needs_lock = td_async_processing(td);
	unsigned long spent, rate;
	enum fio_ddir ddir;
	unsigned long next, next_log;

	next_log = avg_time;

	spent = mtime_since(parent_tv, t);
	if (spent < avg_time && avg_time - spent > LOG_MSEC_SLACK)
		return avg_time - spent;

	if (needs_lock)
		__td_io_u_lock(td);

	/*
	 * Compute both read and write rates for the interval.
	 */
	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
		uint64_t delta;

		delta = this_io_bytes[ddir] - stat_io_bytes[ddir];
		if (!delta)
			continue; /* No entries for interval */

		if (spent) {
			if (is_kb)
				rate = delta * 1000 / spent / 1024; /* KiB/s */
			else
				rate = (delta * 1000) / spent;
		} else
			rate = 0;

		add_stat_sample(&stat[ddir], rate);

		if (log) {
			struct log_sample sample = {
				sample_val(rate), ddir, 0, 0, 0, 0 };

			if (td->o.min_bs[ddir] == td->o.max_bs[ddir])
				sample.bs = td->o.min_bs[ddir];
			next = add_log_sample(td, log, &sample);
			next_log = min(next_log, next);
		}

		stat_io_bytes[ddir] = this_io_bytes[ddir];
	}

	*parent_tv = *t;

	if (needs_lock)
		__td_io_u_unlock(td);

	if (spent <= avg_time)
		next = avg_time;
	else
		next = avg_time - (1 + spent - avg_time);

	return min(next, next_log);
}

/*
 * [한국어]
 * add_bw_samples - 헬퍼 스레드의 bw 샘플 트리거 (bw_avg_time 주기)
 */
static int add_bw_samples(struct thread_data *td, struct timespec *t)
{
	return __add_samples(td, &td->bw_sample_time, t, td->o.bw_avg_time,
				td->this_io_bytes, td->stat_io_bytes,
				td->ts.bw_stat, td->bw_log, true);
}

/*
 * [한국어]
 * add_iops_sample - IOPS 샘플 1개 추가 (I/O 완료 시 호출)
 *
 * @td: 잡.
 * @io_u: 완료 io_u.
 * @bytes: 이 I/O 의 바이트 수.
 *
 * iops_stat 에 "1" 을 추가 (샘플 개수가 곧 IOPS 의 원시 단위).
 * bw_avg_time 과 별개로 iops_avg_time 주기로 __add_samples 가 집계.
 */
void add_iops_sample(struct thread_data *td, struct io_u *io_u,
		     unsigned int bytes)
{
	const bool needs_lock = td_async_processing(td);
	struct thread_stat *ts = &td->ts;

	if (needs_lock)
		__td_io_u_lock(td);

	add_stat_sample(&ts->iops_stat[io_u->ddir], 1);

	if (td->iops_log) {
		struct log_sample sample = { sample_val(1), io_u->ddir, bytes,
			io_u->offset, io_u->ioprio, 0 };

		add_log_sample(td, td->iops_log, &sample);
	}

	td->stat_io_blocks[io_u->ddir] = td->this_io_blocks[io_u->ddir];

	if (needs_lock)
		__td_io_u_unlock(td);
}

/*
 * [한국어]
 * add_iops_samples - 헬퍼 스레드의 iops 샘플 트리거 (iops_avg_time 주기)
 */
static int add_iops_samples(struct thread_data *td, struct timespec *t)
{
	return __add_samples(td, &td->iops_sample_time, t, td->o.iops_avg_time,
				td->this_io_blocks, td->stat_io_blocks,
				td->ts.iops_stat, td->iops_log, false);
}

/*
 * [한국어]
 * td_in_logging_state - 잡이 로깅 가능한 상태인지 판정
 *
 * ramp_time 구간이면 false, runstate 가 RUNNING/VERIFYING/FINISHING/EXITED 면 true.
 */
static bool td_in_logging_state(struct thread_data *td)
{
	if (in_ramp_period(td))
		return false;

	switch(td->runstate) {
	case TD_RUNNING:
	case TD_VERIFYING:
	case TD_FINISHING:
	case TD_EXITED:
		return true;
	default:
		return false;
	}
}

/*
 * Returns msecs to next event
 */
/*
 * [한국어]
 * calc_log_samples - 주기적 bw/iops 샘플 계산 (헬퍼 스레드 메인 루프)
 *
 * @return: 다음 이벤트까지 대기할 ms.
 *
 * 헬퍼 스레드가 주기적으로 호출. 모든 활성 td 를 순회하며:
 *   - td_in_logging_state 가 true 이면 add_bw_samples / add_iops_samples 호출.
 *   - per_unit_log 로그는 제외 (해당 로그는 각 I/O 완료 시점에 기록됨).
 *   - avg_msec_min 으로 가장 빠른 다음 이벤트 시각 계산.
 *
 * 호출 체인:
 *   helper_thread_main [helper_thread.c] → [calc_log_samples] → add_bw_samples/add_iops_samples
 */
int calc_log_samples(void)
{
	unsigned int next = ~0U, tmp = 0, next_mod = 0, log_avg_msec_min = -1U;
	struct timespec now;
	long elapsed_time = 0;

	for_each_td(td) {
		fio_gettime(&now, NULL);
		elapsed_time = mtime_since(&td->epoch, &now);

		if (!td->o.stats)
			continue;
		if (!td_in_logging_state(td)) {
			next = min(td->o.iops_avg_time, td->o.bw_avg_time);
			continue;
		}
		if (!td->bw_log ||
			(td->bw_log && !per_unit_log(td->bw_log))) {
			tmp = add_bw_samples(td, &now);

			if (td->bw_log)
				log_avg_msec_min = min(log_avg_msec_min, (unsigned int)td->bw_log->avg_msec);
		}
		if (!td->iops_log ||
			(td->iops_log && !per_unit_log(td->iops_log))) {
			tmp = add_iops_samples(td, &now);

			if (td->iops_log)
				log_avg_msec_min = min(log_avg_msec_min, (unsigned int)td->iops_log->avg_msec);
		}

		if (tmp < next)
			next = tmp;
	} end_for_each();

	/* if log_avg_msec_min has not been changed, set it to 0 */
	if (log_avg_msec_min == -1U)
		log_avg_msec_min = 0;

	if (log_avg_msec_min == 0)
		next_mod = elapsed_time;
	else
		next_mod = elapsed_time % log_avg_msec_min;

	/* correction to keep the time on the log avg msec boundary */
	next = min(next, (log_avg_msec_min - next_mod));

	return next == ~0U ? 0 : next;
}

/*
 * [한국어]
 * stat_init - 통계 시스템 초기화 (프로세스 수명)
 *
 * 공유 세마포어 stat_sem 을 UNLOCKED 초기 상태로 생성.
 * fio.c 의 initialize_fio() 에서 호출되어 모든 잡이 접근 가능한 동기화 프리미티브 제공.
 */
void stat_init(void)
{
	stat_sem = fio_shared_sem_init(FIO_SEM_UNLOCKED);  /* [한국어] 공유 메모리 상의 세마포어 — fork 된 잡도 접근 가능 */
}

/*
 * [한국어]
 * stat_exit - 통계 시스템 정리
 *
 * stat_sem 다운으로 진행 중인 out-of-band 접근이 모두 완료됐음을 보장 후 세마포어 제거.
 */
void stat_exit(void)
{
	/*
	 * When we have the mutex, we know out-of-band access to it
	 * have ended.
	 */
	fio_sem_down(stat_sem);
	fio_shared_sem_remove(stat_sem);
}

/*
 * Called from signal handler. Wake up status thread.
 */
/*
 * [한국어]
 * show_running_run_stats - 중간 통계 출력 트리거 (시그널 핸들러 등에서 호출)
 *
 * 시그널 핸들러 내에서 직접 통계 출력은 비동기-안전하지 않으므로,
 * 헬퍼 스레드를 깨워(helper_do_stat) 안전한 컨텍스트에서 __show_running_run_stats 실행하게 위임.
 *
 * 호출 체인:
 *   sig_handler(SIGUSR1) → [show_running_run_stats] → helper_do_stat → __show_running_run_stats
 */
void show_running_run_stats(void)
{
	helper_do_stat();
}

/*
 * [한국어]
 * io_u_block_info - io_u 의 오프셋이 속한 블록의 block_info 포인터 반환
 *
 * @td: 잡.
 * @io_u: 대상 I/O 유닛.
 * @return: ts->block_infos[idx] 포인터.
 *
 * idx = (io_u->offset - file_offset) / bs[DDIR_TRIM]
 * experimental_verify / block lifetime 추적 시 블록별 상태(BLOCK_STATE_*) 갱신에 사용.
 * 여러 블록에 걸친 io_u 는 정확한 카운트를 보장할 수 없어 무시 대상 (주석의 "Ignore" 참조).
 */
uint32_t *io_u_block_info(struct thread_data *td, struct io_u *io_u)
{
	/* Ignore io_u's which span multiple blocks--they will just get
	 * inaccurate counts. */
	int idx = (io_u->offset - io_u->file->file_offset)
			/ td->o.bs[DDIR_TRIM];
	uint32_t *info = &td->ts.block_infos[idx];
	assert(idx < td->ts.nr_block_infos);
	return info;
}

