/*
 * [한국어 설명] fio I/O 유닛(io_u) 생명주기·오프셋/길이/방향 결정·완료 회계 핵심 파일 (io_u.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio에서 단일 I/O 요청을 표현하는 io_u 구조체의 "전 생애(end-to-end
 * lifecycle)"를 단독으로 책임진다. 상태 전이는 다음과 같다:
 *
 *   [free (io_u_freelist)] ─ get_io_u() ─→ [prepped (offset/buflen/ddir 결정 완료)]
 *        ↑                                            │
 *        │                                            │ td_io_queue() [ioengines.c]
 *        │                                            ▼
 *        │                                  [in_flight (IO_U_F_FLIGHT)]
 *        │                                            │
 *        │                          ┌─────────────────┴─────────────────┐
 *        │                          │ 동기(SYNCIO)                       │ 비동기
 *        │                          ▼                                    ▼
 *        │             io_u_sync_complete()                  td_io_getevents() →
 *        │             ─→ io_completed() ─→ account_io_completion()      │
 *        │                          │                                    │
 *        │                          ▼                                    ▼
 *        │                  [completed]                       io_u_queued_complete()
 *        │                          │                                    │
 *        │                          │ verify 활성?                       │
 *        │                          ├─ Yes → put_io_u_done() →           │
 *        │                          │        verify_io_u()로 큐잉        │
 *        │                          │        (verify_list)               │
 *        │                          └─ No  ─────────────────────────────┘
 *        │                                            │
 *        └──────────── put_io_u() ◀───────────────────┘
 *                                                  │
 *                                  (또는) requeue_io_u(): 부분완료/EAGAIN 시
 *                                          io_u_requeues 큐로 되돌려 다음 do_io()
 *                                          이터레이션이 td_io_queue를 재시도.
 *
 * 핵심 큐 3종(td 안의 struct io_u_queue):
 *   - td->io_u_freelist: 사용 가능한 io_u (get_io_u 소스)
 *   - td->io_u_all     : 모든 할당된 io_u 추적용 (init/cleanup·verify 검색용)
 *   - td->io_u_requeues: requeue된 io_u (다음 do_io 이터레이션에서 꺼냄)
 *
 * io_u 본체에는 ① 어디에(offset, file) ② 얼마나(buflen, xfer_buflen) ③ 무엇을
 * (ddir: READ/WRITE/TRIM/SYNC/DATASYNC/SYNC_FILE_RANGE/WAIT/INVAL) ④ 어떤 상태로
 * (flags: IO_U_F_FREE/FLIGHT/NO_UNACCOUNT/TRIMMED/BARRIER/VER_LIST/BUSY_OK)
 * ⑤ 결과는 어떻게(resid, error, issue_time/start_time) 같은 속성이 채워진다.
 *
 * 오프셋/길이/방향 결정 알고리즘:
 *   ▸ 방향(ddir): get_rw_ddir() → set_rw_ddir()
 *       - 단일 방향이면 그대로, 혼합이면 rate_ddir()/get_rand_ddir()로 rwmix 비율
 *         (rwmix_bytes / rand_seed) 기반 추첨. read_iolog_avail()이면 iolog 따른다.
 *       - SYNC/DATASYNC/SYNC_FILE_RANGE/TRIM은 별도 분기.
 *   ▸ 오프셋: get_next_offset() → 분기:
 *       - 순차: get_next_seq_offset() — file->last_pos[ddir] 갱신, ba/bs 정렬,
 *         존(zone_range) 경계와 io_size 한계 검사.
 *       - 랜덤: get_next_block() → get_next_rand_block() →
 *         __get_next_rand_offset 변종 디스패치(td->o.random_distribution):
 *           · DEFAULT(uniform):  __get_next_rand_offset() — LFSR(linear feedback
 *             shift register, td->use_lfsr) 또는 일반 frand → axmap_isset 통과까지 반복
 *           · ZIPF:  __get_next_rand_offset_zipf() (zipf_next, theta 파라미터)
 *           · PARETO:__get_next_rand_offset_pareto() (pareto h)
 *           · GAUSS: __get_next_rand_offset_gauss() (정규분포 dev/100)
 *           · ZONED/ZONED_ABS: __get_next_rand_offset_zoned[_abs]() —
 *             zone 비율(zone_split) 따른 다단계 파티션 추첨
 *           · SPRANDOM(WRITE 전용): __get_next_rand_offset_sprandom() — 모든
 *             주소를 정확히 한 번씩 기록(소진 시 td->done=1).
 *         결과 블록은 axmap(io_axmap) 미접근 비트와 매칭되어야 통과(소진 시 0 반환).
 *   ▸ 길이(buflen): get_next_buflen() — bsrange면 FIO_RAND_BS frand로 추첨,
 *         bssplit이면 누적 분포 함수(CDF)로, 단일 bs면 그대로. min_bs/max_bs/
 *         bs_unaligned/io_size·zone 경계 클램프.
 *
 * trimwrite/verify 디스패치:
 *   ▸ check_get_trim() : trim_backlog 도달 시 다음 io_u를 TRIM으로 강제 변환.
 *   ▸ check_get_verify(): verify_backlog 도달 시 verify_list에서 검증 io_u 발급.
 *   ▸ trimwrite 모드: io_u_sync_complete가 같은 영역을 WRITE 후 즉시 TRIM으로
 *     반복 발급, save_buf_random_state로 패턴 시드를 보존하여 verify가 재현.
 *
 * 완료 회계(account):
 *   ▸ io_completed(): io_u 1개 완료 처리. resid 반영, ddir별 bytes_done[]
 *     누적, lastfile/lastrate/last_issue/issue_time 갱신, ramp_time 통과 후
 *     account_io_completion() 호출.
 *   ▸ account_io_completion(): clat(완료 지연)/slat(제출 지연)/lat(전체) 샘플을
 *     stat.c의 add_*_sample()로 추가, bw 윈도(rate_bytes/iops)에 누적.
 *   ▸ trim_block_info(): TRIM 완료 시 io log에 해당 블록 무효화 기록.
 *   ▸ io_u_mark_lat_*sec / io_u_mark_latency: 분포 히스토그램(td->ts.io_u_lat_*)
 *     버킷에 카운트.
 *   ▸ io_u_mark_submit/complete/depth: 큐 깊이/제출/완료 분포를 멱승 버킷으로 집계.
 *
 * verify 경로:
 *   ▸ get_buf_state()/save_buf_state(): WRITE 시 사용한 frand 시드를 저장하여
 *     verify가 같은 패턴을 재생성(verify_pattern_check)할 수 있게 한다. WRITE 후
 *     verify_list에 io_u가 들어가고 do_verify가 다시 같은 offset/buflen으로 READ를
 *     발급, 패턴 비교.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 잡 실행 루프는 backend.c의 do_io()에 있다. 그 루프 한 사이클은
 * "get_io_u → td_io_prep → td_io_queue → (commit) → td_io_getevents →
 * io_u_*_complete → put_io_u"이며, 이 파일은 그 중 get/put 양 끝과 *_complete
 * 회계, 그리고 get_io_u 내부의 fill_io_u(오프셋/길이/방향 결정) 전체를 담당한다.
 * 즉 do_io 루프 한 사이클의 "결정·통계" 절반은 io_u.c, "엔진 경유 제출" 절반은
 * ioengines.c가 처리한다고 볼 수 있다. 실행 컨텍스트는 잡 스레드(td 단독 소유)이며
 * helper/verify 스레드는 별도의 td 인스턴스를 소유한다.
 *
 * 호출 체인 요약:
 *   do_io() [backend.c]
 *     ├─ get_io_u() [io_u.c]
 *     │    ├─ __get_io_u(): freelist에서 pop, IO_U_F_FREE clear, IO_U_F_FLIGHT는
 *     │    │   아직 미설정(prep 단계)
 *     │    ├─ set_io_u_file(): get_next_file()로 fio_file 선택(rr/random)
 *     │    ├─ fill_io_u(): set_rw_ddir + get_next_offset + get_next_buflen
 *     │    │    └─ check_get_trim()/check_get_verify()로 ddir 오버라이드
 *     │    └─ small_content_scramble(): write 패턴 식별자 삽입(옵션)
 *     ├─ td_io_prep() [ioengines.c]: 엔진별 sqe/iocb 등 사전 구성
 *     ├─ td_io_queue() [ioengines.c]: 엔진의 .queue 콜백 호출, IO_U_F_FLIGHT set
 *     │    → FIO_Q_COMPLETED면 즉시 io_u_sync_complete()
 *     │    → FIO_Q_QUEUED면 비동기 — 추후 td_io_getevents/io_u_queued_complete
 *     │    → FIO_Q_BUSY면 requeue_io_u()로 io_u_requeues에 push, IO_U_F_FLIGHT clear
 *     ├─ td_io_commit() [ioengines.c]: 누적 sqe 일괄 제출
 *     ├─ td_io_getevents() [ioengines.c]: 완료 폴링/대기
 *     ├─ io_u_queued_complete() [io_u.c]
 *     │    └─ ios_completed() → io_completed() → account_io_completion()
 *     └─ put_io_u() [io_u.c]: IO_U_F_FREE set, freelist에 push, file usage--
 *
 * === 타 모듈과의 연결 ===
 * - backend.c   : do_io()/do_verify() 잡 루프의 양쪽 끝에서 get_io_u/put_io_u/
 *                 io_u_*_complete를 호출. ramp_time/runtime 종료, terminate_threads
 *                 신호 등 잡 흐름은 backend가 주관.
 * - ioengines.c : td_io_prep/queue/commit/getevents/event 디스패치. 엔진 .queue가
 *                 FIO_Q_COMPLETED를 돌려주면 io_u_sync_complete가 즉시 호출됨.
 * - stat.c      : add_clat_sample/add_slat_sample/add_lat_sample/add_bw_sample 등을
 *                 account_io_completion에서 호출, thread_stat에 누적.
 * - iolog.c     : log_io_u(), trim_io_log(), io_u_mark_* 기반의 percentile/disk_util
 *                 윈도 윈도잉. trimwrite 모드에서 trim_block_info가 io log를 갱신.
 * - verify.c    : do_verify가 io_u_queued_complete/get_io_u를 별도 verify 경로로
 *                 호출. WRITE 시 save_buf_state로 보존된 시드를 verify가 재생성.
 * - lib/rand.c  : __rand/__rand64/frand_state — 오프셋/길이/패턴 모든 난수 소스.
 * - lib/axmap.c : 랜덤 맵 — axmap_isset/axmap_set_nr로 중복 접근 방지.
 * - lib/lfsr.c  : LFSR(td->use_lfsr=1일 때) — 메모리 효율적인 균등 비복원 추첨.
 * - zbd.c       : Zoned Block Device 모드 활성 시 zbd_adjust_block 등으로 오프셋 보정.
 * - sprandom.c  : SP RANDOM 모드(WRITE 전용 비복원 분포)의 상태기.
 * - trim.c      : trim_io_u_free, get_trim_io_u 등 trim 백로그/리스트.
 *
 * 공유 자료구조:
 *   - struct thread_data (td) : 잡 1개의 모든 상태(옵션, 통계, 큐 3종, 파일 배열,
 *                               엔진 ops, 난수 상태). 잡 스레드 단독 소유.
 *   - struct io_u            : 본 파일의 주인공. 위 생명주기 다이어그램 참조.
 *   - struct io_u_queue      : freelist/all/requeues 공통 컨테이너.
 *   - struct io_completion_data : 완료 1배치의 결과(nr/error/bytes_done/time)
 *                               집계용 임시 구조체(이 파일에 정의).
 *   - struct fio_file        : 대상 파일/디바이스. last_pos/io_size/io_axmap/spr_info.
 *
 * === 주요 함수/구조체 요약 ===
 * 큐 전이:
 *   - get_io_u()           : freelist → prepped. 파일/오프셋/길이/방향 결정.
 *   - put_io_u()           : in_flight/completed → freelist. file usage--.
 *   - requeue_io_u()       : in_flight → io_u_requeues. EAGAIN/BUSY 시.
 *   - clear_io_u()         : in_flight 플래그 정리(엔진 측 에러 회수 경로).
 *
 * 오프셋/길이/방향 결정:
 *   - fill_io_u()          : set_rw_ddir + get_next_offset + get_next_buflen 통합.
 *   - get_next_offset()    : 순차/랜덤 분기 → get_next_seq_offset / get_next_block.
 *   - get_next_buflen()    : bsrange/bssplit/단일 bs 분기.
 *   - __get_next_rand_offset[_zipf/_pareto/_gauss/_zoned/_zoned_abs/_sprandom]
 *                          : 분포별 랜덤 오프셋 생성기.
 *   - mark_random_map()    : axmap에 사용 표시(중복 방지).
 *   - get_rw_ddir() / rate_ddir() / get_rand_ddir() : 방향 결정.
 *
 * 완료 회계:
 *   - io_u_sync_complete() : 동기 완료 1건 처리.
 *   - io_u_queued_complete(): 비동기 완료 N건 일괄 처리(getevents 후).
 *   - io_completed()       : 단건 완료의 핵심 경로(resid/bytes_done/account).
 *   - account_io_completion(): clat/slat/lat/bw 샘플 기록.
 *   - io_u_mark_latency()/io_u_mark_lat_{n,u,m}sec : 지연 분포 버킷.
 *   - io_u_mark_submit/complete/depth : 큐 깊이/제출/완료 분포 버킷.
 *   - trim_block_info()    : TRIM 완료 시 io log 무효화.
 *
 * 보조:
 *   - get_buf_state()/save_buf_state() : verify를 위한 패턴 시드 보존.
 *   - fill_io_buffer()/io_u_fill_buffer() : 쓰기 버퍼 패턴 채움.
 *   - lat_target_*()       : 지연 목표(latency_target) 기반 적응적 iodepth 제어.
 *   - do_io_u_sync()/do_io_u_trim()/do_sync_file_range()
 *                          : 메타 ddir(SYNC/DATASYNC/SYNC_FILE_RANGE/TRIM) 즉시 실행.
 *
 * io_u 플래그 비트(io_u->flags) 풀이:
 *   - IO_U_F_FREE          : freelist 소속. get_io_u에서 clear, put_io_u에서 set.
 *   - IO_U_F_FLIGHT        : 엔진에 제출되어 완료 대기 중. td_io_queue에서 set,
 *                            완료/clear/requeue 경로에서 clear.
 *   - IO_U_F_NO_UNACCOUNT  : trimwrite 등 "이 io_u는 in_flight 카운트 차감 금지".
 *   - IO_U_F_TRIMMED       : 본 io_u가 TRIM 완료된 상태(verify 경로 분기 힌트).
 *   - IO_U_F_BARRIER       : 쓰기 배리어(BSG/SCSI 등 일부 엔진에서 의미).
 *   - IO_U_F_VER_LIST      : verify_list 소속(verify 경로의 io_u).
 *   - IO_U_F_BUSY_OK       : 랜덤 맵 충돌이어도 진행(중복 접근 허용 모드).
 *
 * 데이터 방향(ddir) 풀이:
 *   - DDIR_READ/WRITE/TRIM         : 데이터 I/O 3종.
 *   - DDIR_SYNC                    : fsync(2) 메타 동기.
 *   - DDIR_DATASYNC                : fdatasync(2).
 *   - DDIR_SYNC_FILE_RANGE         : sync_file_range(2) — 부분 동기.
 *   - DDIR_WAIT                    : 시간 대기(생성된 가짜 ddir).
 *   - DDIR_INVAL                   : 무효(에러/미설정).
 *
 * ※ 본 파일은 신기준 재작업 진행 중이다. 현재 패스에서는 상단 4섹션 블록을
 *    "주석만으로 io_u 생명주기 전체를 이해 가능"한 수준으로 전면 확장하였고,
 *    함수/필드 단위 §2/§4 주석과 모든 실행 라인 인라인은 후속 패스에서 점진
 *    완성한다. 기존 얕은 한국어 주석은 삭제하지 않고 보강 대상으로 남긴다.
 */
#include <unistd.h>       /* [한국어] POSIX 기본(파일 디스크립터 관련 프로토타입) — 엔진 경로에서 간접 사용 */
#include <string.h>       /* [한국어] memset/memcpy — io_u 버퍼 채움·구조체 초기화 */
#include <assert.h>       /* [한국어] assert — 랜덤 맵/블록 수/상태 전이 전제 방어 */

#include "fio.h"          /* [한국어] fio 코어 타입(thread_data/io_u/ioengine_ops/FIO_Q_*/dprint/fio_gettime 등) */
#include "verify.h"       /* [한국어] verify 경로 훅(verify_io_u_async/get_next_verify) — verify 모드 분기 */
#include "trim.h"         /* [한국어] trim 백로그 유틸(get_trim_io_u/trim_io_u_free) — TRIM ddir 강제 변환 경로 */
#include "lib/rand.h"     /* [한국어] frand_state 및 __rand/__rand64 — 오프셋/길이/패턴 난수 소스 */
#include "lib/axmap.h"    /* [한국어] 비트맵 기반 랜덤 맵 — 중복 접근 방지(공평성 커버리지) */
#include "err.h"          /* [한국어] IS_ERR/PTR_ERR/ERR_PTR — 엔진 에러 포인터 관례 */
#include "lib/pow2.h"     /* [한국어] is_power_of_2/roundup_pow2 — 블록 정렬/분포 버킷 산정 */
#include "minmax.h"       /* [한국어] min/max 매크로 — 경계 클램프 */
#include "zbd.h"          /* [한국어] Zoned Block Device 어댑터 — zbd_adjust_block/zbd_put_io */
#include "sprandom.h"     /* [한국어] SP RANDOM 상태기 — 모든 주소 1회 쓰기 분포 */

/* [한국어] I/O 완료 데이터 구조체 — 완료 1배치(N건)에 대한 집계 컨테이너.
 *
 * 수명: io_u_sync_complete()/io_u_queued_complete()가 스택에 잡고 init_icd()로
 * 초기화 → ios_completed()가 엔진의 .event() 콜백으로 각 완료 io_u를 순회하며
 * io_completed()를 호출, 결과를 이 구조체에 누적 → 호출자가 td->bytes_done에 합산.
 * 잡 스레드 스택 상에만 존재하므로 동기화 불필요.
 */
struct io_completion_data {
	int nr;				/* input */
	/* [한국어] 이 배치에서 처리할 완료 I/O 개수(입력).
	 * 설정자: init_icd()에서 getevents()가 반환한 완료 수 또는 sync 경우 1.
	 * 읽는 자: ios_completed() 루프 상한.
	 * 값 범위: 1..iodepth. 동기화: 스택 지역변수 — 없음. */

	int error;			/* output */
	/* [한국어] 배치 처리 중 마지막으로 기록된 에러 코드(출력).
	 * 설정자: io_completed()가 io_u->error를 여기로 전파.
	 * 읽는 자: io_u_sync_complete/io_u_queued_complete 반환값 분기.
	 * 값 범위: 0=성공, 양수=errno. 동기화: 스택 지역변수 — 없음. */

	uint64_t bytes_done[DDIR_RWDIR_CNT];	/* output */
	/* [한국어] 각 데이터 방향(READ/WRITE/TRIM)별로 이 배치에서 완료된 바이트 누적(출력).
	 * 설정자: io_completed()가 io_u->xfer_buflen - resid를 해당 ddir 슬롯에 더함.
	 * 읽는 자: 호출자가 td->bytes_done[]에 합산, do_io 루프의 진행률 판정에 사용.
	 * 값 범위: 0..Σ xfer_buflen. 동기화: 스택 지역변수 — 없음. */

	struct timespec time;		/* output */
	/* [한국어] 이 배치의 공통 완료 시각(fio_gettime 기준, account 지연 계산 기준점).
	 * 설정자: init_icd()에서 fio_gettime()로 1회 캡처.
	 * 읽는 자: account_io_completion()이 io_u->issue_time/start_time과의 차이로
	 *          clat/slat/lat 산출.
	 * 값 범위: CLOCK_MONOTONIC/TSC 기반 nsec. 동기화: 스택 지역변수 — 없음. */
};

/*
 * The ->io_axmap contains a map of blocks we have or have not done io
 * to yet. Used to make sure we cover the entire range in a fair fashion.
 */
/* [한국어]
 * random_map_free - 랜덤 맵에서 특정 블록이 아직 I/O되지 않았는지 확인.
 *
 * @f: 파일 구조체 포인터. io_axmap 필드는 axmap_new()로 fill_io_u 첫 호출 전에
 *     할당되며, min_bs 단위 블록 하나당 1비트를 가진다.
 * @block: 확인할 블록 번호 (min_bs 단위의 상대 블록 인덱스, file_offset 기준).
 * @return: 해당 블록이 비어있으면 true(=아직 I/O 안 함), 이미 접근했으면 false.
 *
 * io_axmap은 비트맵 기반의 자료구조(lib/axmap.c)로, 랜덤 I/O 시 전체 파일 범위를
 * "공평 커버리지(fair coverage)"로 한 번씩만 접근하기 위해 이미 건드린 블록을
 * 추적한다. norandommap=1 이면 이 맵이 비활성화되어 함수가 호출되지 않는다.
 *
 * 실행 컨텍스트: 잡 스레드 단독. axmap_isset은 비트 연산만 수행(락 없음) —
 * 맵 자체가 td 단독 소유라 경쟁 조건 없음.
 *
 * 호출 체인:
 *   fill_io_u → get_next_offset → get_next_block → get_next_rand_block →
 *   get_next_rand_offset → __get_next_rand_offset → [이 함수]
 */
static bool random_map_free(struct fio_file *f, const uint64_t block)
{
	/* [한국어] axmap_isset이 true이면 이미 접근함 → 여유 없음(false 반환). */
	return !axmap_isset(f->io_axmap, block);
}

/*
 * Mark a given offset as used in the map.
 */
/* [한국어]
 * mark_random_map - 주어진 I/O 영역을 랜덤 맵에서 "사용됨"으로 표시.
 *
 * @td: 잡 스레드 데이터. td->o.min_bs[ddir]로 블록 크기 스케일을 얻는다.
 * @io_u: 방향/플래그/파일 포인터를 가진 I/O 유닛. io_u->flags의 IO_U_F_BUSY_OK가
 *        세트면 "이미 사용 중인 블록과 충돌해도 그냥 진행" → 맵 업데이트 생략.
 * @offset: 표시할 시작 오프셋 (바이트, file_offset 기준 절대 오프셋).
 * @buflen: I/O 크기(바이트). 맵 제약으로 축소될 수 있으므로 반환값 중요.
 * @return: 실제 표시·발행 가능한 바이트 수. 맵에서 연속된 빈 블록이 buflen
 *          미만이면 이 값이 buflen보다 작다(호출자가 io_u->buflen을 축소).
 *
 * 랜덤 I/O에서 같은 위치를 중복 접근하지 않도록 맵에 기록한다(공평 커버리지).
 * min_bs 단위 블록 번호를 계산하고 axmap_set_nr()로 해당 블록들을 **원자적으로**
 * 비트 OR 연산 — lib/axmap.c는 level-compressed trie 기반으로 세트 실패(이미 채워진
 * 블록)를 만나면 거기까지만 세트하고 그 개수를 돌려준다.
 *
 * 실행 컨텍스트: 잡 스레드(td) 단독. io_axmap은 td 소유라 락 없이 접근.
 *
 * 호출 체인:
 *   fill_io_u → [이 함수] (norandommap=0 + td_random 경로)
 *   fill_multi_range_io_u → [이 함수] (다중 범위 trim 경로)
 *
 * 에러 경로: assert(nr_blocks > 0)이 실패하면 0 길이 I/O 요청으로 상위 버그.
 */
static uint64_t mark_random_map(struct thread_data *td, struct io_u *io_u,
				uint64_t offset, uint64_t buflen)
{
	/* [한국어] 이 방향의 최소 블록 크기 — 랜덤 맵의 한 비트가 표현하는 바이트 수. */
	unsigned long long min_bs = td->o.min_bs[io_u->ddir];
	struct fio_file *f = io_u->file;
	unsigned long long nr_blocks;
	uint64_t block;

	/* [한국어] 파일 시작(file_offset) 기준 상대 오프셋을 min_bs 단위 블록 번호로 변환.
	 * io_axmap은 파일의 상대 주소 공간(0..last_block)을 커버하므로 반드시 뺄셈. */
	block = (offset - f->file_offset) / (uint64_t) min_bs;
	/* [한국어] 이 I/O가 덮는 블록 수(올림). max_bs 가 min_bs의 정수배가 아닐 수도
	 * 있어 "+min_bs-1" 천장 올림 이디엄으로 안전하게 계산. */
	nr_blocks = (buflen + min_bs - 1) / min_bs;
	/* [한국어] 0블록 I/O는 상위 계산 버그 → 디버그 빌드에서 즉시 중단. */
	assert(nr_blocks > 0);

	/* [한국어] BUSY_OK 플래그가 없을 때만 맵에 마킹. BUSY_OK는 rw_seq 모드의
	 * 재탐색 경로, randtrimwrite의 write 단계 등 "이미 맵에 있어도 진행" 케이스. */
	if (!(io_u->flags & IO_U_F_BUSY_OK)) {
		/* [한국어] axmap_set_nr: [block..block+nr_blocks) 를 1로 세트.
		 * 중간에 이미 세트된 비트를 만나면 거기까지만 진행하고 실제 세트한 수 반환 —
		 * 이 메커니즘이 "빈 연속 블록이 원하는 만큼 안 될 때" 부분 매칭의 근거. */
		nr_blocks = axmap_set_nr(f->io_axmap, block, nr_blocks);
		assert(nr_blocks > 0);
	}

	/* [한국어] 실제 세트된 블록 수가 요청보다 적으면 buflen을 그만큼 축소 —
	 * 호출자는 이 값으로 io_u->buflen을 덮어써 IO 범위를 실제 맵 세트 범위로 한정. */
	if ((nr_blocks * min_bs) < buflen)
		buflen = nr_blocks * min_bs;

	return buflen;
}

/* [한국어]
 * last_block - 특정 방향(ddir)에 대해 이 파일의 유효 블록 수(상한)를 계산.
 *
 * @td: 잡 스레드. td->o.zone_mode/zone_range/min_bs/ba로 상한 계산 근거를 얻는다.
 * @f: 대상 파일. f->io_size(fio 옵션 io_size) vs f->real_file_size(실제 크기) 중
 *     작은 쪽을 사용해 오버플로 방지.
 * @ddir: READ/WRITE/TRIM 중 하나(ddir_rw로 검증). SYNC/DATASYNC 등은 금지.
 * @return: ba(block alignment) 단위로 측정한 접근 가능 블록 수 (0이면 파일 크기 부족).
 *
 * 파일의 io_size, real_file_size, zone_range 등을 모두 고려해 "이 방향으로 이번 잡에서
 * 만질 수 있는 블록 수"를 산출한다. ba(블록 정렬 단위)는 기본 min_bs와 같지만
 * ba_unaligned 시 별도 값. min_bs > ba 인 경우 마지막 블록이 파일 끝을 넘지 않도록
 * "여유(min_bs - ba)"만큼 max_size 에서 빼준다.
 *
 * 실행 컨텍스트: 잡 스레드. 파라미터 모두 td 단독 소유이므로 동기화 불필요.
 *
 * 호출 체인:
 *   __get_next_rand_offset[_zoned[_abs]] → [이 함수]
 */
static uint64_t last_block(struct thread_data *td, struct fio_file *f,
			   enum fio_ddir ddir)
{
	uint64_t max_blocks;
	uint64_t max_size;

	/* [한국어] SYNC/DATASYNC/INVAL 등 비데이터 방향은 "블록 수" 개념 없음 →
	 * 디버그 빌드에서 조기 탐지(상위 디스패처 버그). */
	assert(ddir_rw(ddir));

	/*
	 * Hmm, should we make sure that ->io_size <= ->real_file_size?
	 * -> not for now since there is code assuming it could go either.
	 */
	/* [한국어] io_size = 잡 옵션(size/io_size)으로 설정된 논리 상한,
	 * real_file_size = open 시 실제 fstat 된 바이트 크기. 둘 중 작은 쪽이
	 * 실제로 접근 가능한 한계 — io_size 가 더 클 수 있는 이유는 파일이 미확장
	 * 상태(write extend 전)일 수 있기 때문. */
	max_size = f->io_size;
	if (max_size > f->real_file_size)
		max_size = f->real_file_size;

	/* [한국어] ZONE_MODE_STRIDED: fio 가 관리하는 "가상 존" 범위 내에서만 I/O.
	 * 한 존(zone_range 바이트) 내에서만 블록 선택하도록 상한을 줄인다.
	 * ZBD 모드(ZNS 장치)와는 다름 — 여기선 fio 자체가 가짜 존 경계를 그어 패턴만 재현. */
	if (td->o.zone_mode == ZONE_MODE_STRIDED && td->o.zone_range)
		max_size = td->o.zone_range;

	/* [한국어] ba(블록 정렬) 는 I/O 시작 오프셋 격자이고, min_bs 는 실제 쓰는
	 * 바이트. min_bs > ba 면 마지막 ba 격자에서 시작 시 끝이 파일 상한을 넘을 수
	 * 있어 max_size 를 (min_bs - ba) 만큼 미리 빼서 안전 여백 확보. */
	if (td->o.min_bs[ddir] > td->o.ba[ddir])
		max_size -= td->o.min_bs[ddir] - td->o.ba[ddir];

	/* [한국어] 블록 수 = max_size / ba — 랜덤 분포에서 [0..max_blocks) 추첨 후
	 * ba 곱해 오프셋으로 변환. uint64_t 캐스트는 32비트 플랫폼에서 오버플로 방지. */
	max_blocks = max_size / (uint64_t) td->o.ba[ddir];
	/* [한국어] max_size < ba (파일이 블록보다 작음) → 접근 불가, 0 반환 →
	 * 상위 __get_next_rand_offset_* 가 1(실패) 전파 후 잡 종료 또는 다음 파일. */
	if (!max_blocks)
		return 0;

	return max_blocks;
}


/* [한국어]
 * __get_next_rand_offset_sprandom - SP RANDOM 분포(WRITE 전용 1회 커버리지)의
 *                                   다음 오프셋 생성기.
 *
 * @td: 잡 스레드. 종료 시 td->done=1로 마킹.
 * @f: 대상 파일. f->spr_info는 sprandom.c가 관리하는 비복원 상태 머신.
 * @ddir: WRITE만 허용(assert). READ/TRIM은 해당 분포 의미 없음.
 * @b: [출력] 다음 블록 번호가 저장될 포인터.
 * @lastb: 미사용(다른 변종과 시그니처 일치용). SP RANDOM은 spr_info가 상한을 내장.
 * @return: 0=성공, 1=모든 주소 1회 기록 완료(잡 종료 신호).
 *
 * SP RANDOM은 "모든 주소를 정확히 한 번씩 순회하며 WRITE"하는 분포로,
 * 균등 랜덤 + axmap 조합보다 효율적인 비복원 랜덤 패턴을 제공한다(내부는
 * sprandom.c의 LFSR 유사 상태 머신). 소진 시 td->done=1로 잡 루프(do_io)가
 * 자연 종료되도록 신호한다.
 *
 * 실행 컨텍스트: 잡 스레드. spr_info는 f 당 단독 소유.
 *
 * 호출 체인:
 *   fill_io_u → get_next_offset → get_next_block → get_next_rand_block →
 *   get_next_rand_offset → [이 함수] (td->o.sprandom && ddir==DDIR_WRITE 분기)
 */
static int __get_next_rand_offset_sprandom(struct thread_data *td, struct fio_file *f,
					   enum fio_ddir ddir, uint64_t *b,
					   uint64_t lastb)
{
	/* [한국어] SP RANDOM은 쓰기 전용 — READ/TRIM 호출은 상위 디스패처 버그. */
	assert(ddir == DDIR_WRITE);

	/* SP RANDOM writes all addresses once */
	/* [한국어] sprandom 상태 머신에서 다음 오프셋 획득 — 실패면 전체 주소 공간 소진.
	 * dprint로 종료 지점 로깅, td->done=1로 do_io 루프 다음 이터레이션에서 종료. */
	if (sprandom_get_next_offset(f->spr_info, f, b)) {
		dprint(FD_SPRANDOM, "sprandom is done\n");
		td->done = 1;
		return 1;
	}
	return 0;
}

/* [한국어]
 * __get_next_rand_offset - 균등(uniform) 랜덤 분포의 오프셋 생성 (기본 분포).
 *
 * @td: 잡 스레드. td->o.random_generator가 Tausworthe/LFSR 중 어느 경로 선택,
 *      td->offset_state가 Tausworthe 상태 머신.
 * @f: 대상 파일. LFSR 모드에서 f->lfsr에 잡 별 상태 저장.
 * @ddir: 미사용(시그니처 일치). 이 분포는 방향 무관.
 * @b: [출력] 선택된 블록 번호(min_bs 단위).
 * @lastb: 유효 블록 수(상한). last_block() 결과.
 * @return: 0=성공, 1=빈 블록 없음(소진).
 *
 * 알고리즘:
 *  1) Tausworthe(기본): r = __rand(offset_state), b = lastb * (r / (rand_max+1.0))
 *     부동소수점 곱으로 [0..lastb) 균등 사상.
 *  2) LFSR(td->o.random_generator == FIO_RAND_GEN_LFSR): lfsr_next()가 주기 2^n-1
 *     내에서 각 값을 정확히 한 번씩 순회 — 비복원 균등 분포의 메모리 효율적 구현
 *     (axmap 없이도 커버리지 보장). 소진 시 1 반환.
 *  3) 랜덤 맵(io_axmap)이 활성화되었으면 해당 블록이 비었는지(random_map_free) 확인,
 *     점유되어 있으면 axmap_next_free로 다음 빈 블록으로 점프(포워드 탐색) →
 *     빈 블록이 없으면(-1ULL) 실패.
 *
 * 실행 컨텍스트: 잡 스레드 단독. offset_state/lfsr/io_axmap 모두 td 소유.
 *
 * 호출 체인:
 *   get_next_rand_offset → [이 함수] (DEFAULT uniform 분기)
 *   __get_next_rand_offset_zoned/_zoned_abs 도 zone 내 오프셋 추첨에 재사용.
 */
static int __get_next_rand_offset(struct thread_data *td, struct fio_file *f,
				  enum fio_ddir ddir, uint64_t *b,
				  uint64_t lastb)
{
	uint64_t r;

	/* [한국어] Tausworthe 계열 (기본) — 32/64비트 변종 모두 __rand() 통해 추상화.
	 * Tausworthe는 주기 > 2^88의 고품질 PRNG(lib/rand.c), 통계적 균등성 우수. */
	if (td->o.random_generator == FIO_RAND_GEN_TAUSWORTHE ||
	    td->o.random_generator == FIO_RAND_GEN_TAUSWORTHE64) {

		/* [한국어] offset 전용 난수 상태에서 1회 추출 — 다른 용도(bsrange 등)와
		 * 시드/상태가 분리되어 있어 결과 재현성 보장(random_seed 고정 시). */
		r = __rand(&td->offset_state);

		dprint(FD_RANDOM, "off rand %llu\n", (unsigned long long) r);

		/* [한국어] r/(rand_max+1.0) ∈ [0,1) 비율로 변환 후 lastb 곱 → [0,lastb).
		 * +1.0은 상한 포함 방지, double 나눗셈으로 bias 최소화. */
		*b = lastb * (r / (rand_max(&td->offset_state) + 1.0));
	} else {
		/* [한국어] LFSR(선형 피드백 시프트 레지스터) — 비복원 균등 분포.
		 * 주기 내 모든 값이 정확히 1회 나타나 axmap 없이도 파일 전 영역 커버.
		 * 메모리 비용 O(1) vs axmap O(N/8). use_lfsr=1 옵션 시 활성화. */
		uint64_t off = 0;

		/* [한국어] lfsr_init이 성공한 파일에서만 호출되는지 검증.
		 * 파일 크기가 너무 작아 lfsr_init 실패 시 fio가 잡 시작 전 에러. */
		assert(fio_file_lfsr(f));

		/* [한국어] LFSR 상태 전진 후 다음 값 획득. 전체 공간 소진 시 non-zero 반환 — 종료 신호. */
		if (lfsr_next(&f->lfsr, &off))
			return 1;

		*b = off;
	}

	/*
	 * if we are not maintaining a random map, we are done.
	 */
	/* [한국어] norandommap=1 또는 LFSR 사용 시 → 랜덤 맵 불필요, 바로 반환.
	 * file_randommap()은 (td->o.norandommap==0 && !td->o.lfsr) 판정. */
	if (!file_randommap(td, f))
		goto ret;

	/*
	 * calculate map offset and check if it's free
	 */
	/* [한국어] 방금 뽑은 b 블록이 axmap에서 비어있으면 그대로 사용. */
	if (random_map_free(f, *b))
		goto ret;

	dprint(FD_RANDOM, "get_next_rand_offset: offset %llu busy\n",
						(unsigned long long) *b);

	/* [한국어] 점유된 블록이면 맵에서 다음 빈 블록 선형 탐색(forward, wrap 없음).
	 * axmap_next_free는 level-compressed trie 로 O(log N) 점프 가능. */
	*b = axmap_next_free(f->io_axmap, *b);
	/* [한국어] 전 영역 소진 시 sentinel -1ULL 반환 → 실패(1) 전파해
	 * 상위 get_next_rand_block이 file_reset 재시도 또는 잡 종료 판단. */
	if (*b == (uint64_t) -1ULL)
		return 1;
ret:
	return 0;
}

/* [한국어]
 * __get_next_rand_offset_zipf - Zipf 분포 오프셋 생성기.
 *
 * @td: 잡 스레드(미사용 — zipf 상태는 파일 단위).
 * @f: 대상 파일. f->zipf은 init_rand_file_service에서 zipf_init(range, theta, seed)로
 *    초기화된 Zipf 상태 머신(lib/zipf.c).
 * @ddir: 미사용(시그니처 일치).
 * @b: [출력] 블록 번호.
 * @return: 항상 0 (Zipf은 분석적이라 실패 없음).
 *
 * Zipf 분포: P(k) ∝ 1/k^theta — 소수의 "핫" 블록에 접근 집중 (웹 캐시 hit,
 * 데이터베이스 hot rows 모델). theta=1이 전통 Zipf, theta 증가할수록 더 편향.
 * 파라미터는 td->o.zipf_theta(또는 random_distribution=zipf:<theta>).
 *
 * 랜덤 맵 사용 안 함 — Zipf은 중복 접근 허용(핫 블록 집중이 의도).
 *
 * 호출 체인: get_next_rand_offset → [이 함수] (FIO_RAND_DIST_ZIPF).
 */
static int __get_next_rand_offset_zipf(struct thread_data *td,
				       struct fio_file *f, enum fio_ddir ddir,
				       uint64_t *b)
{
	/* [한국어] zipf_next: 내부 상태에서 역 CDF 기반으로 Zipf 샘플 1개 추출. */
	*b = zipf_next(&f->zipf);
	return 0;
}

/* [한국어]
 * __get_next_rand_offset_pareto - Pareto 분포 오프셋 생성기.
 *
 * @td/f/ddir/b/@return: zipf 변종과 동일 인터페이스.
 *
 * Pareto 분포("80/20 법칙"의 수학적 모델): 누적 CDF가 F(x) = 1 - (x_m/x)^alpha.
 * Zipf과 유사하게 긴 꼬리를 가져 상위 일부 블록이 대부분의 접근을 차지한다.
 * fio에서는 f->zipf 상태를 Pareto 모드로 재사용한다(pareto_next 호출).
 * 파라미터: random_distribution=pareto:<h> (h는 형태 파라미터, 보통 0.2~1.2).
 *
 * 호출 체인: get_next_rand_offset → [이 함수] (FIO_RAND_DIST_PARETO).
 */
static int __get_next_rand_offset_pareto(struct thread_data *td,
					 struct fio_file *f, enum fio_ddir ddir,
					 uint64_t *b)
{
	/* [한국어] pareto_next: f->zipf 상태(Pareto 계수 내장)에서 샘플 추출. */
	*b = pareto_next(&f->zipf);
	return 0;
}

/* [한국어]
 * __get_next_rand_offset_gauss - 가우시안(정규) 분포 오프셋 생성기.
 *
 * @td/f/ddir/b/@return: 다른 분포 변종과 동일.
 *
 * 정규 분포 N(mu=lastb/2, sigma=lastb*dev/100): 파일 중앙에 접근 집중 (뱅크 계좌의
 * 최근 활동, 로그 파일의 tail 접근 모델). 파라미터:
 *   random_distribution=normal:<dev>  — dev는 표준편차의 파일 비율(%).
 * dev=1 → sigma = lastb*0.01 (매우 좁음), dev=100 → sigma = lastb (균등에 가까움).
 * f->gauss는 init_rand_file_service에서 gauss_init(range, dev)로 초기화.
 *
 * 호출 체인: get_next_rand_offset → [이 함수] (FIO_RAND_DIST_GAUSS).
 */
static int __get_next_rand_offset_gauss(struct thread_data *td,
					struct fio_file *f, enum fio_ddir ddir,
					uint64_t *b)
{
	/* [한국어] gauss_next: Box-Muller/polar 변형으로 정규 분포 샘플 반환. */
	*b = gauss_next(&f->gauss);
	return 0;
}

/* [한국어]
 * __get_next_rand_offset_zoned_abs - 절대 바이트 경계 기반 존별 가중 랜덤 분포.
 *
 * @td: 잡 스레드. td->zone_state(rand_state), td->zone_state_index[ddir][0..99]
 *      (100 버킷 사전 전개된 zone→확률 매핑).
 * @f: 대상 파일(존 기반 접근 대상).
 * @ddir: READ/WRITE/TRIM.
 * @b: [출력] 선택된 블록 번호.
 * @return: 0=성공, 1=실패(last_block=0 또는 존 범위 초과).
 *
 * zoned_abs 분포: random_distribution=zoned_abs:percentage/size[,...]
 * 예) "70/1g:30/2g" = [0,1GB) 영역에 70%, [1GB,3GB)에 30% 가중치.
 *     zone_split_index[] 는 1~100 범위를 소진하는 누적 테이블로, v=1~100 난수를
 *     뽑아 O(1)에 존 선택 가능.
 *
 * 처리 단계:
 *   1. lastb = last_block (파일 상한).
 *   2. zone_split_nr=0(zone 미정의) → 폴백 bail → __get_next_rand_offset (기본 균등).
 *   3. v = rand_between(1,100) 으로 존 버킷 선택.
 *   4. zsi = zone_state_index[ddir][v-1]; stotal(이전 존까지 누적 블록),
 *      send(현재 존 끝 블록). 모두 ba 단위로 변환.
 *   5. 유효성 검사(send == -1U 는 버그, send > lastb 는 사용자 지정 범위가
 *      파일 크기 초과 → log_err 후 실패).
 *   6. __get_next_rand_offset 으로 [0, send-stotal) 범위 추첨 → stotal 더해 절대화.
 *
 * 실행 컨텍스트: 잡 스레드.
 *
 * 호출 체인: get_next_rand_offset → [이 함수] (FIO_RAND_DIST_ZONED_ABS).
 */
static int __get_next_rand_offset_zoned_abs(struct thread_data *td,
					    struct fio_file *f,
					    enum fio_ddir ddir, uint64_t *b)
{
	struct zone_split_index *zsi;
	uint64_t lastb, send, stotal;
	unsigned int v;

	/* [한국어] 파일의 유효 상한 블록 수. 0 이면 max_size < ba → 접근 불가 →
	 * 이 분포로도 오프셋 만들 수 없으므로 실패 전파. */
	lastb = last_block(td, f, ddir);
	if (!lastb)
		return 1;

	/* [한국어] 사용자가 random_distribution=zoned_abs 를 지정했더라도 해당
	 * 방향(ddir)에 대한 zone_split 이 없다면 기본 균등으로 폴백 — bail 라벨은
	 * send==-1U 버그 경로와 공유해 동일 폴백. */
	if (!td->o.zone_split_nr[ddir]) {
bail:
		return __get_next_rand_offset(td, f, ddir, b, lastb);
	}

	/*
	 * Generate a value, v, between 1 and 100, both inclusive
	 */
	/* [한국어] 1..100 난수로 존 버킷 선택. init_random_file_service 가 사용자
	 * zone_split 비율(예 70/30)을 100 슬롯 zone_state_index[ddir][0..99] 로
	 * 사전 전개해 둔 덕에 O(1) 테이블 조회만으로 확률적 존 결정이 가능. */
	v = rand_between(&td->zone_state, 1, 100);

	/*
	 * Find our generated table. 'send' is the end block of this zone,
	 * 'stotal' is our start offset.
	 */
	/* [한국어] v-1 인덱스로 해당 존의 (size_prev, size) 바이트 쌍 획득. */
	zsi = &td->zone_state_index[ddir][v - 1];
	/* [한국어] stotal = 이전 존들까지 누적 바이트 / ba → 블록 단위 시작점.
	 * __get_next_rand_offset 이 "블록 번호" 단위로 동작하므로 ba 로 정규화 필수. */
	stotal = zsi->size_prev / td->o.ba[ddir];
	/* [한국어] send = 현재 존 끝 바이트 / ba → 반개구간 [stotal, send) 블록 집합. */
	send = zsi->size / td->o.ba[ddir];

	/*
	 * Should never happen
	 */
	/* [한국어] send == -1U(unsigned sentinel)는 init 단계에서 이 슬롯이 채워지지
	 * 않은 상태 — fio 버그. fio_did_warn 로 1회만 출력 후 bail(균등 폴백)로 계속. */
	if (send == -1U) {
		if (!fio_did_warn(FIO_WARN_ZONED_BUG))
			log_err("fio: bug in zoned generation\n");
		goto bail;
	} else if (send > lastb) {
		/*
		 * This happens if the user specifies ranges that exceed
		 * the file/device size. We can't handle that gracefully,
		 * so error and exit.
		 */
		/* [한국어] 사용자가 zoned_abs=".../2g" 등 파일보다 큰 구간을 지정한
		 * 경우. 잘못된 테스트 설정이므로 폴백 없이 즉시 실패(잡 종료 유도). */
		log_err("fio: zoned_abs sizes exceed file size\n");
		return 1;
	}

	/*
	 * Generate index from 0..send-stotal
	 */
	/* [한국어] 존 내부 상대 블록 번호를 기본 균등으로 추첨. 존 내부에는 axmap
	 * 조건이 그대로 적용 가능(file_randommap=true 시). 실패면 그대로 전파. */
	if (__get_next_rand_offset(td, f, ddir, b, send - stotal) == 1)
		return 1;

	/* [한국어] 존 상대 번호에 stotal(존 시작) 을 더해 파일 전역 블록 번호로 승격. */
	*b += stotal;
	return 0;
}

/* [한국어]
 * __get_next_rand_offset_zoned - 퍼센트 비율 기반 존별 가중 랜덤 분포.
 *
 * @td/f/ddir/b/@return: zoned_abs 변종과 동일 인터페이스.
 *
 * zoned 분포: random_distribution=zoned:percentage/size_perc[,...]
 * 예) "70/50:30/50" = 파일의 앞 50%에 70% 접근, 뒤 50%에 30% 접근.
 * zoned_abs 와의 차이: 존 경계를 절대 바이트가 아니라 파일 비율(%)로 지정 —
 * 파일 크기가 달라도 같은 "상대적 hot/cold" 패턴 유지.
 *
 * 내부 계산: zsi->size_perc_prev/size_perc(이전 누적/현재 끝 %) 를 사용하여
 * offset = stotal * lastb / 100, 존 크기 = lastb * (send - stotal) / 100 로
 * 블록 수 변환.
 *
 * 호출 체인: get_next_rand_offset → [이 함수] (FIO_RAND_DIST_ZONED).
 */
static int __get_next_rand_offset_zoned(struct thread_data *td,
					struct fio_file *f, enum fio_ddir ddir,
					uint64_t *b)
{
	unsigned int v, send, stotal;
	uint64_t offset, lastb;
	struct zone_split_index *zsi;

	/* [한국어] 파일 유효 블록 상한. 0이면 파일 크기 < ba → 이 분포 적용 불가. */
	lastb = last_block(td, f, ddir);
	if (!lastb)
		return 1;

	/* [한국어] zone_split_nr[ddir]==0 이면 이 방향에 zoned 설정이 없는 것 →
	 * 기본 균등으로 폴백. bail 라벨은 버그(send==-1U) 경로와 공유. */
	if (!td->o.zone_split_nr[ddir]) {
bail:
		return __get_next_rand_offset(td, f, ddir, b, lastb);
	}

	/*
	 * Generate a value, v, between 1 and 100, both inclusive
	 */
	/* [한국어] 1..100 난수로 확률 테이블 조회(zoned 의 100 슬롯 전개된 룩업). */
	v = rand_between(&td->zone_state, 1, 100);

	/* [한국어] 해당 버킷의 zone_split_index 획득 — zoned_abs 와 달리 여기선
	 * size_perc(%) 필드를 사용. init 시 바이트 아닌 백분율 그대로 저장. */
	zsi = &td->zone_state_index[ddir][v - 1];
	/* [한국어] stotal: 이전 존들까지 누적 백분율(0~100). zoned_abs 와 달리 아직
	 * 바이트로 전환 안 된 상태 — 파일 크기와 독립적인 "상대 비율" 표현. */
	stotal = zsi->size_perc_prev;
	/* [한국어] send: 현재 존 끝 백분율. 구간 [stotal, send)%의 파일 영역 표현. */
	send = zsi->size_perc;

	/*
	 * Should never happen
	 */
	/* [한국어] send==-1U 는 init 단계 누락 슬롯 — fio 버그. 경고 1회 후 폴백. */
	if (send == -1U) {
		if (!fio_did_warn(FIO_WARN_ZONED_BUG))
			log_err("fio: bug in zoned generation\n");
		goto bail;
	}

	/*
	 * 'send' is some percentage below or equal to 100 that
	 * marks the end of the current IO range. 'stotal' marks
	 * the start, in percent.
	 */
	/* [한국어] 백분율 → 실제 블록 오프셋 변환. stotal==0 이면 offset=0 지름길 —
	 * 0 곱셈 회피 + stotal*lastb 오버플로 예방(lastb 가 테라바이트 스케일일 때). */
	if (stotal)
		offset = stotal * lastb / 100ULL;
	else
		offset = 0;

	/* [한국어] 존 폭(블록 수) = 전체블록 * (send-stotal) / 100.
	 * 여기서 lastb 를 덮어써 다음 __get_next_rand_offset 의 상한으로 재사용 —
	 * 원래 lastb 값은 이 시점 이후 필요 없음. */
	lastb = lastb * (send - stotal) / 100ULL;

	/*
	 * Generate index from 0..send-of-lastb
	 */
	/* [한국어] 존 내부 [0..존폭) 블록 추첨. axmap 이 있으면 그대로 연동
	 * (file_randommap true 시). 실패(소진) 면 1 전파. */
	if (__get_next_rand_offset(td, f, ddir, b, lastb) == 1)
		return 1;

	/*
	 * Add our start offset, if any
	 */
	/* [한국어] 존 상대 블록 → 파일 전역 블록으로 승격(offset 더하기).
	 * stotal==0 이면 offset==0 이라 이 분기는 생략 가능(미세 최적화). */
	if (offset)
		*b += offset;

	return 0;
}

/* [한국어]
 * get_next_rand_offset - 랜덤 분포 디스패처 (td->o.random_distribution 에 따라 분기).
 *
 * @td: 잡 스레드.
 * @f: 대상 파일.
 * @ddir: READ/WRITE/TRIM.
 * @b: [출력] 블록 번호.
 * @return: 0=성공, 1=실패(분포 소진 또는 알 수 없는 분포).
 *
 * 분기표:
 *   sprandom + DDIR_WRITE → __get_next_rand_offset_sprandom (비복원 1회 커버리지)
 *   FIO_RAND_DIST_RANDOM  → __get_next_rand_offset (기본 균등 + axmap/LFSR)
 *   FIO_RAND_DIST_ZIPF    → __get_next_rand_offset_zipf
 *   FIO_RAND_DIST_PARETO  → __get_next_rand_offset_pareto
 *   FIO_RAND_DIST_GAUSS   → __get_next_rand_offset_gauss
 *   FIO_RAND_DIST_ZONED   → __get_next_rand_offset_zoned  (퍼센트 가중)
 *   FIO_RAND_DIST_ZONED_ABS → __get_next_rand_offset_zoned_abs (절대 가중)
 *
 * 실행 컨텍스트: 잡 스레드.
 *
 * 호출 체인:
 *   get_next_rand_block → [이 함수] → 분포별 변종 함수.
 */
static int get_next_rand_offset(struct thread_data *td, struct fio_file *f,
				enum fio_ddir ddir, uint64_t *b)
{
	/* [한국어] sprandom(WRITE 비복원 1회 커버리지) 옵션 + WRITE 분기. 다른 분포를
	 * 지정해도 sprandom 이 true 이면 쓰기만 가로채서 "모든 주소 정확히 한 번 쓰기"
	 * 를 강제. 읽기/트림에는 해당 없어 아래 random_distribution 대로 흘러감. */
	if (td->o.sprandom && ddir == DDIR_WRITE) {
		return __get_next_rand_offset_sprandom(td, f, ddir, b, 0);
	} else if (td->o.random_distribution == FIO_RAND_DIST_RANDOM) {
		/* [한국어] 기본 균일 랜덤 — 이 분기만 lastb 를 호출자에서 미리 계산해
		 * 전달하는 구조(다른 분포는 내부에서 lastb 직접 계산). */
		uint64_t lastb;

		lastb = last_block(td, f, ddir);
		if (!lastb)
			return 1;

		return __get_next_rand_offset(td, f, ddir, b, lastb);
	} else if (td->o.random_distribution == FIO_RAND_DIST_ZIPF)
		/* [한국어] Zipf(1/k^theta) — 핫 블록 집중. axmap 없음(중복 접근 허용). */
		return __get_next_rand_offset_zipf(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_PARETO)
		/* [한국어] Pareto(80/20 모델) — Zipf 과 유사 긴 꼬리. 내부에서 f->zipf 재사용. */
		return __get_next_rand_offset_pareto(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_GAUSS)
		/* [한국어] 정규분포(N(mu=lastb/2, sigma=lastb*dev/100)) — 파일 중앙 집중. */
		return __get_next_rand_offset_gauss(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_ZONED)
		/* [한국어] 퍼센트 기반 zoned — "앞 50%에 70%, 뒤 50%에 30%" 상대 비율. */
		return __get_next_rand_offset_zoned(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_ZONED_ABS)
		/* [한국어] 절대 바이트 기반 zoned_abs — "[0,1g)에 70%, [1g,3g)에 30%". */
		return __get_next_rand_offset_zoned_abs(td, f, ddir, b);

	log_err("fio: unknown random distribution: %d\n", td->o.random_distribution);
	return 1;
}

/* [한국어]
 * should_do_random - 이번 I/O 를 랜덤으로 낼지 순차로 낼지 확률적 결정.
 *
 * @td: 잡 스레드. td->seq_rand_state[ddir] 이 난수 상태(방향별 분리).
 * @ddir: 데이터 방향(READ/WRITE/TRIM).
 * @return: true=랜덤 오프셋 사용, false=순차 오프셋 사용.
 *
 * percentage_random=<r,w,t> 옵션으로 방향별 비율 설정(예: 70,30,0 = 읽기 70%/쓰기 30%).
 * 100이면 early-return true(rand_between 호출 생략).
 *
 * 호출 체인: get_next_block (rw_seq 새 시퀀스) → [이 함수] → 반환에 따라
 * get_next_rand_block vs get_next_seq_offset 선택.
 */
static bool should_do_random(struct thread_data *td, enum fio_ddir ddir)
{
	unsigned int v;

	/* [한국어] perc_rand==100 이면 rand_between 호출 자체를 생략 —
	 * IOPs 집약 워크로드에서 난수 생성 비용을 피하는 fast path. */
	if (td->o.perc_rand[ddir] == 100)
		return true;

	/* [한국어] seq_rand_state[ddir] 는 방향별 독립 시드를 유지 — READ 경로의
	 * 난수 흐름이 WRITE 경로에 영향을 주지 않도록 분리(재현성 보장). */
	v = rand_between(&td->seq_rand_state[ddir], 1, 100);

	/* [한국어] v ∈ [1,100], perc_rand ∈ [0,100]. v <= perc_rand 확률은 perc_rand/100. */
	return v <= td->o.perc_rand[ddir];
}

/* [한국어]
 * loop_cache_invalidate - time_based 실행 중 파일 끝 도달 시 페이지캐시 무효화.
 *
 * @td: 잡 스레드.
 * @f: 대상 파일.
 *
 * time_based=1 + 랜덤 분포 소진 시 fio_file_reset으로 다시 처음부터 순회하는데,
 * 직전 사이클에서 페이지캐시에 데이터가 남아있으면 캐시 hit으로 성능이 왜곡된다.
 * 이를 방지하기 위해 file_invalidate_cache(→ posix_fadvise DONTNEED/drop_caches)
 * 를 호출. odirect=1 이면 캐시 자체를 우회하므로 이 호출은 no-op.
 *
 * 호출 체인:
 *   get_next_seq_offset (파일 끝 wrap) → [이 함수]
 *   get_next_rand_block (분포 소진 후 reset) → [이 함수]
 */
static void loop_cache_invalidate(struct thread_data *td, struct fio_file *f)
{
	struct thread_options *o = &td->o;

	/* [한국어] invalidate_cache=1 + !odirect 조합만 의미가 있음:
	 *  - odirect=1 은 이미 페이지캐시를 우회하므로 무효화 불필요.
	 *  - invalidate_cache=0 이면 사용자가 재사용 캐시 효과를 의도한 경우이므로 건드리지 않음.
	 * file_invalidate_cache 는 내부적으로 posix_fadvise(POSIX_FADV_DONTNEED) 또는
	 * POSIX_FADV_DONTNEED 지원 안 되는 경우 msync/munmap 경로로 캐시 드랍. */
	if (o->invalidate_cache && !o->odirect) {
		/* [한국어] ret 는 에러 무시(fio_unused 속성). file_invalidate 실패가
		 * 잡 진행을 막아선 안 됨 — 캐시 드랍은 성능 측정 보조 수단일 뿐. */
		int fio_unused ret;

		ret = file_invalidate_cache(td, f);
	}
}

/* [한국어]
 * get_next_rand_block - 랜덤 블록 선택 + 소진 시 time_based 리셋 재시도 래퍼.
 *
 * @td: 잡 스레드.
 * @f: 대상 파일. 소진 시 fio_file_reset(td, f) 로 axmap/LFSR/Zipf 상태 초기화.
 * @ddir: 방향.
 * @b: [출력] 블록 번호.
 * @return: 0=성공, 1=실패(리셋 후에도 소진).
 *
 * time_based=1 또는 file_service_type에 __FIO_FSERVICE_NONUNIFORM 비트(Zipf/Pareto/
 * Gauss) 가 설정되어 있으면 파일을 리셋하고 loop_cache_invalidate 후 1회 재시도.
 * 그 외에는 단순히 실패 전파.
 *
 * 호출 체인:
 *   get_next_block → [이 함수] → get_next_rand_offset → 분포별 변종.
 */
static int get_next_rand_block(struct thread_data *td, struct fio_file *f,
			       enum fio_ddir ddir, uint64_t *b)
{
	/* [한국어] 1차 시도 — 분포의 상태 기기에서 다음 블록 추출. 성공(0)이면 즉시 반환. */
	if (!get_next_rand_offset(td, f, ddir, b))
		return 0;

	/* [한국어] 분포 소진 시 2차 시도: 파일 상태(axmap/LFSR/Zipf 내부 카운터)를 리셋.
	 *  - time_based=1: 지정된 시간 동안 계속 돌아야 하므로 공간 소진 시 재사용 필수.
	 *  - __FIO_FSERVICE_NONUNIFORM (Zipf/Pareto/Gauss): 비균등 분포는 특성상 소진
	 *    개념이 "접근 완료"가 아니라 "상태 재초기화"로 해결.
	 * fio_file_reset 이 axmap 을 모두 0으로 되돌리고, loop_cache_invalidate 로
	 * 직전 사이클의 페이지 캐시를 드랍해 캐시 hit 으로 성능 왜곡 방지. */
	if (td->o.time_based ||
	    (td->o.file_service_type & __FIO_FSERVICE_NONUNIFORM)) {
		fio_file_reset(td, f);
		loop_cache_invalidate(td, f);
		/* [한국어] 리셋 후 재시도 — 성공이면 그대로 반환. 실패면 아래 로깅 후 1 반환. */
		if (!get_next_rand_offset(td, f, ddir, b))
			return 0;
	}

	dprint(FD_IO, "%s: rand offset failed, last=%llu, size=%llu\n",
			f->file_name, (unsigned long long) f->last_pos[ddir],
			(unsigned long long) f->real_file_size);
	return 1;
}

/* [한국어]
 * get_next_seq_offset - 순차(sequential) I/O 의 다음 오프셋(파일 상대) 계산.
 *
 * @td: 잡 스레드.
 * @f: 대상 파일. f->last_pos[ddir] 는 방향별 순차 "다음에 읽을" 위치.
 * @ddir: READ/WRITE/TRIM (assert ddir_rw).
 * @offset: [출력] file_offset 기준 상대 오프셋.
 * @return: 0=성공, 1=파일 끝 도달 및 wrap 불가.
 *
 * 알고리즘:
 *  1) time_based + 단일 파일 + last_pos가 io_size+start_offset 이상이면
 *     last_pos = file_offset 으로 wrap + cache invalidate.
 *  2) td_rw + io_size > size (읽기+쓰기 따로 잡힌 경우) 유사 wrap 처리.
 *  3) last_pos < real_file_size 이면:
 *     - 역방향(ddir_seq_add < 0) 이고 시작점 도달: 끝으로 jump (io_size 또는
 *       real_file_size 중 작은 쪽).
 *     - pos = last_pos - file_offset 계산 후 ddir_seq_add 적용(홀/역방향).
 *     - pos가 파일 끝을 넘으면 순방향은 0, 역방향은 끝으로 wrap.
 *  4) 그 외에는 실패(1).
 *
 * ddir_seq_add 의미: 양수=홀 I/O 간격(blockalign + add), 음수=역방향 step,
 * 0=연속.
 *
 * 호출 체인:
 *   get_next_block → [이 함수] (td_random 아닐 때 또는 should_do_random false).
 */
static int get_next_seq_offset(struct thread_data *td, struct fio_file *f,
			       enum fio_ddir ddir, uint64_t *offset)
{
	struct thread_options *o = &td->o;

	assert(ddir_rw(ddir));

	/*
	 * If we reach the end for a time based run, reset us back to 0
	 * and invalidate the cache, if we need to.
	 */
	/* [한국어] time_based 잡은 "runtime 초 동안 반복" 의미 → 파일 끝 도달 시
	 * file_offset 으로 wrap 하여 계속 I/O. nr_files==1 제한은 다중 파일 시 파일
	 * 간 전환으로 wrap 을 대신하도록 하기 위함(파일별 편향 방지). */
	if (f->last_pos[ddir] >= f->io_size + get_start_offset(td, f) &&
	    o->time_based && o->nr_files == 1) {
		f->last_pos[ddir] = f->file_offset;
		loop_cache_invalidate(td, f);
	}

	/*
	 * If we reach the end for a rw-io-size based run, reset us back to 0
	 * and invalidate the cache, if we need to.
	 */
	/* [한국어] td_rw + io_size > size 케이스 — 사용자가 파일 크기보다 큰 총
	 * I/O 양을 요청했을 때(예 100GB 파일에 300GB I/O). 이 방향에서 파일 끝에
	 * 닿으면 순환 재시작 필요 — time_based 조건 없음에 주의(바이트 기반 루프). */
	if (td_rw(td) && o->io_size > o->size) {
		if (f->last_pos[ddir] >= f->io_size + get_start_offset(td, f)) {
			f->last_pos[ddir] = f->file_offset;
			loop_cache_invalidate(td, f);
		}
        }

	/* [한국어] last_pos 가 아직 파일 크기 내 — 정상 순차 진행 또는 wrap 후 상태. */
	if (f->last_pos[ddir] < f->real_file_size) {
		uint64_t pos;

		/*
		 * Only rewind if we already hit the end
		 */
		/* [한국어] ddir_seq_add<0 (역방향 시퀀스) + last_pos==file_offset 은
		 * "직전에 시작점 도달 → 다시 rewind 필요" 상태. file_offset 조건은
		 * "처음 잡 시작 시의 동일 상태(시작점==file_offset)" 와 구분하기 위함. */
		if (f->last_pos[ddir] == f->file_offset &&
		    f->file_offset && o->ddir_seq_add < 0) {
			/* [한국어] 점프할 끝점: io_size vs real_file_size 중 작은 쪽.
			 * io_size(사용자 요청)가 파일 크기보다 작으면 그 상한까지만 역주행. */
			if (f->real_file_size > f->io_size)
				f->last_pos[ddir] = f->io_size;
			else
				f->last_pos[ddir] = f->real_file_size;
		}

		/* [한국어] pos 는 "파일 시작(file_offset) 상대" 오프셋 — 아래 ddir_seq_add
		 * 연산을 절대 주소가 아닌 상대 주소 공간에서 처리하기 위한 정규화. */
		pos = f->last_pos[ddir] - f->file_offset;
		/* [한국어] ddir_seq_add 는 "홀 I/O 간격(양수)" 또는 "역주행 스텝(음수)".
		 * pos==0 이면 아직 첫 I/O 전 — add 적용 없이 0부터 시작 유지(초기 케이스 보호). */
		if (pos && o->ddir_seq_add) {
			pos += o->ddir_seq_add;

			/*
			 * If we reach beyond the end of the file
			 * with holed IO, wrap around to the
			 * beginning again. If we're doing backwards IO,
			 * wrap to the end.
			 */
			/* [한국어] add 적용 후 pos >= real_file_size:
			 *  - 순방향(add > 0): 파일 처음으로 wrap (홀 I/O 가 끝을 넘어감).
			 *  - 역방향(add < 0): 끝(io_size 또는 real_file_size)으로 wrap 후
			 *    add 한 번 더 적용(역방향 스텝 유지). */
			if (pos >= f->real_file_size) {
				if (o->ddir_seq_add > 0)
					pos = f->file_offset;
				else {
					if (f->real_file_size > f->io_size)
						pos = f->io_size;
					else
						pos = f->real_file_size;

					pos += o->ddir_seq_add;
				}
			}
		}

		*offset = pos;
		return 0;
	}

	/* [한국어] last_pos >= real_file_size — 순차 I/O 공간 완전 소진. time_based
	 * 나 td_rw wrap 조건이 위에서 처리 안 됐으므로 여기선 진짜 실패(1). */
	return 1;
}

/* [한국어]
 * get_next_block - 순차/랜덤 혼합 규칙에 따라 다음 I/O 위치 결정.
 *
 * @td: 잡 스레드.
 * @io_u: 결정 결과를 저장할 I/O 유닛(io_u->file 이미 세트).
 * @ddir: READ/WRITE/TRIM.
 * @rw_seq: 1=ddir_seq_nr 주기 히트로 새 시퀀스 결정 타이밍, 0=시퀀스 진행 중.
 * @is_random: [출력] 결과가 랜덤이면 true (mark_random_map 호출 조건).
 * @return: 0=성공, 1=실패.
 *
 * 분기 트리:
 *   randtrimwrite + WRITE → 직전 TRIM의 last_start 재사용 (trim+write 쌍).
 *   rw_seq 히트 + td_random:
 *     should_do_random=true → get_next_rand_block
 *     should_do_random=false → get_next_seq_offset → 실패 시 rand 폴백.
 *   rw_seq 히트 + 순차: get_next_seq_offset.
 *   rw_seq 미히트 + RW_SEQ_SEQ: seq → 실패 시 rand 폴백.
 *   rw_seq 미히트 + RW_SEQ_IDENT: last_start 재사용(같은 위치 반복).
 *
 * 결과 정규화: offset 또는 b 중 하나만 유효 → offset 은 그대로, b 는 *ba[ddir].
 * io_u->verify_offset 도 동기 세팅.
 *
 * 호출 체인: get_next_offset → [이 함수].
 */
static int get_next_block(struct thread_data *td, struct io_u *io_u,
			  enum fio_ddir ddir, int rw_seq,
			  bool *is_random)
{
	struct fio_file *f = io_u->file;
	uint64_t b, offset;
	int ret;

	assert(ddir_rw(ddir));

	/* [한국어] sentinel -1ULL 로 초기화 — 결정 분기 이후 어느 쪽이 세트되었는지
	 * 구분 (offset != -1 이면 순차 경로, b != -1 이면 랜덤 경로). */
	b = offset = -1ULL;

	/* [한국어] randtrimwrite: 같은 영역에 TRIM → WRITE 를 쌍으로 내는 모드.
	 * WRITE 발행 시 직전 TRIM 의 last_start 를 재사용해 "방금 trim 한 자리에
	 * 바로 쓰기" 패턴을 만든다. BUSY_OK 는 axmap 이 이 쓰기를 충돌 처리하지
	 * 않도록 한다(이미 TRIM 이 axmap 을 마킹했을 수 있음). */
	if (td_randtrimwrite(td) && ddir == DDIR_WRITE) {
		/* don't mark randommap for these writes */
		io_u_set(td, io_u, IO_U_F_BUSY_OK);
		/* [한국어] file_offset 을 빼서 "파일 상대" 오프셋으로 정규화. 상위에서
		 * 다시 file_offset 을 더해 절대 주소로 복원. */
		offset = f->last_start[DDIR_TRIM] - f->file_offset;
		*is_random = true;
		ret = 0;
	} else if (rw_seq) {
		/* [한국어] rw_seq==1: ddir_seq_nr 주기가 끝나 "새 시퀀스" 를 시작하는 타이밍.
		 * 이때만 랜덤/순차 결정을 다시 하고, 이후 ddir_seq_nr 회는 같은 모드로 진행. */
		if (td_random(td)) {
			/* [한국어] 잡이 random 이어도 percentage_random<100 이면 일부는 순차.
			 * should_do_random 으로 이번 새 시퀀스를 어느 쪽으로 갈지 추첨. */
			if (should_do_random(td, ddir)) {
				ret = get_next_rand_block(td, f, ddir, &b);
				*is_random = true;
			} else {
				/* [한국어] 순차로 결정. BUSY_OK 설정 이유: 이번 순차 I/O 가
				 * 이전 axmap 히트(동일 블록)여도 그대로 진행 — 순차의 연속성
				 * 우선. 순차 실패(파일 끝) 시 랜덤 폴백으로 진행 보장. */
				*is_random = false;
				io_u_set(td, io_u, IO_U_F_BUSY_OK);
				ret = get_next_seq_offset(td, f, ddir, &offset);
				if (ret)
					/* [한국어] 순차 실패(파일 끝) → 랜덤으로 폴백해 잡 진행 유지. */
					ret = get_next_rand_block(td, f, ddir, &b);
			}
		} else {
			/* [한국어] td_random 이 false → 순수 순차(읽기/쓰기/트림 중 하나의 sequential). */
			*is_random = false;
			ret = get_next_seq_offset(td, f, ddir, &offset);
		}
	} else {
		/* [한국어] rw_seq_hit=0: 이미 시작된 시퀀스의 중간 — 모드 판정 없이 계속.
		 * BUSY_OK 는 순차 연속성 보장(axmap 충돌 우회). */
		io_u_set(td, io_u, IO_U_F_BUSY_OK);
		*is_random = false;

		if (td->o.rw_seq == RW_SEQ_SEQ) {
			/* [한국어] 일반 순차 진행 — 파일 끝 도달 시 랜덤 폴백. */
			ret = get_next_seq_offset(td, f, ddir, &offset);
			if (ret) {
				ret = get_next_rand_block(td, f, ddir, &b);
				*is_random = false;
			}
		} else if (td->o.rw_seq == RW_SEQ_IDENT) {
			/* [한국어] rw_seq=identical 모드 — 매번 "같은 위치" 반복(스트레스 테스트).
			 * last_start[ddir]==-1ULL 은 "아직 한 번도 발행 안 함" — 이때만 0 부터 시작. */
			if (f->last_start[ddir] != -1ULL)
				offset = f->last_start[ddir] - f->file_offset;
			else
				offset = 0;
			ret = 0;
		} else {
			log_err("fio: unknown rw_seq=%d\n", td->o.rw_seq);
			ret = 1;
		}
	}

	/* [한국어] 결정 성공 시 offset 또는 b 중 세트된 쪽을 io_u->offset 으로 승격. */
	if (!ret) {
		if (offset != -1ULL)
			/* [한국어] 순차/IDENT/randtrimwrite 경로 — offset 은 이미 바이트 단위. */
			io_u->offset = offset;
		else if (b != -1ULL)
			/* [한국어] 랜덤 경로 — b 는 ba 블록 번호. ba 곱해 바이트 오프셋으로 변환. */
			io_u->offset = b * td->o.ba[ddir];
		else {
			log_err("fio: bug in offset generation: offset=%llu, b=%llu\n", (unsigned long long) offset, (unsigned long long) b);
			ret = 1;
		}
		/* [한국어] verify_offset 을 offset 과 동기 — verify 경로가 이 필드를 기준
		 * 으로 패턴 재생성. WRITE 시점의 오프셋 그대로 저장해 READ-back 매칭 보장. */
		io_u->verify_offset = io_u->offset;
	}

	return ret;
}

/*
 * For random io, generate a random new block and see if it's used. Repeat
 * until we find a free one. For sequential io, just return the end of
 * the last io issued.
 */
/* [한국어]
 * get_next_offset - io_u->offset 결정 공용 진입점 + 파일 범위/ZBD 후처리.
 *
 * @td: 잡 스레드. td->ddir_seq_nr 카운터가 0 도달 시 새 시퀀스 → rw_seq_hit=1.
 * @io_u: 대상. io_u->file/ddir 는 이미 세트. 출력은 io_u->offset, verify_offset.
 * @is_random: [출력] 결과가 랜덤으로 결정되었는지(상위에서 axmap 세트 필요 판단).
 * @return: 0=성공, 1=실패(소진 또는 파일 범위 초과).
 *
 * 처리 순서:
 *   1. ddir_seq_nr 카운터 관리: 설정되어 있고 감소 후 0이면 rw_seq_hit=1 → 다음
 *      ddir_seq_nr 로 재충전 (순차/랜덤 교대 주기).
 *   2. get_next_block → offset 결정.
 *   3. offset >= io_size → 실패 (잡 io_size 초과).
 *   4. offset += file_offset → 절대 주소로 변환.
 *   5. offset >= real_file_size → 실패 (물리 크기 초과).
 *   6. randtrimwrite + TRIM + last_start 일치 보정 로직 (위 함수 주석 참조).
 *   7. verify_offset 동기 갱신.
 *
 * 호출 체인:
 *   fill_io_u → [이 함수]
 *   fill_multi_range_io_u → [이 함수]
 */
static int get_next_offset(struct thread_data *td, struct io_u *io_u,
			   bool *is_random)
{
	struct fio_file *f = io_u->file;
	enum fio_ddir ddir = io_u->ddir;
	int rw_seq_hit = 0;

	assert(ddir_rw(ddir));

	/* [한국어] ddir_seq_nr>0 옵션은 "매 N회마다 순차↔랜덤 재결정" 제어. 0이 되면
	 * rw_seq_hit 으로 표시 후 카운터 재충전. 예) ddir_seq_nr=10 → 10개마다 새 시퀀스. */
	if (td->o.ddir_seq_nr && !--td->ddir_seq_nr) {
		rw_seq_hit = 1;
		td->ddir_seq_nr = td->o.ddir_seq_nr;
	}

	/* [한국어] rw_seq_hit 값을 get_next_block 에 전달해 새 시퀀스면 모드 재추첨. */
	if (get_next_block(td, io_u, ddir, rw_seq_hit, is_random))
		return 1;

	/* [한국어] io_u->offset 은 아직 "파일 상대" (file_offset 안 더한 상태).
	 * io_size 는 잡 옵션의 논리 상한이므로 상대 오프셋과 직접 비교 가능. */
	if (io_u->offset >= f->io_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= io_size %llu\n",
					(unsigned long long) io_u->offset,
					(unsigned long long) f->io_size);
		return 1;
	}

	/* [한국어] file_offset(start_offset + 파일 선두 스킵) 을 더해 "파일 내 절대
	 * 바이트 오프셋" 으로 승격 — 이후 엔진 경로는 모두 절대 오프셋 사용. */
	io_u->offset += f->file_offset;
	/* [한국어] 절대 오프셋이 real_file_size 를 넘으면 I/O 불가능(파일 크기
	 * 변경된 경우 등). io_size < real_file_size 인 경우 위 검사가 더 엄격했지만
	 * 반대인 경우 이 검사가 실제 안전 가드. */
	if (io_u->offset >= f->real_file_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= size %llu\n",
					(unsigned long long) io_u->offset,
					(unsigned long long) f->real_file_size);
		return 1;
	}

	/*
	 * For randtrimwrite, we decide whether to issue a trim or a write
	 * based on whether the offsets for the most recent trim and write
	 * operations match. If they don't match that means we just issued a
	 * new trim and the next operation should be a write. If they *do*
	 * match that means we just completed a trim+write pair and the next
	 * command should be a trim.
	 *
	 * This works fine for sequential workloads but for random workloads
	 * it's possible to complete a trim+write pair and then have the next
	 * randomly generated offset match the previous offset. If that happens
	 * we need to alter the offset for the last write operation in order
	 * to ensure that we issue a write operation the next time through.
	 */
	/* [한국어] randtrimwrite에서 trim 후 write를 보장하기 위한 보정 로직.
	 * trim과 write의 마지막 시작 오프셋이 같으면 trim+write 쌍이 완료된 것이므로
	 * 다음은 trim이어야 함. 하지만 랜덤 오프셋이 이전과 같아지면 잘못 판단할 수 있어
	 * write의 마지막 오프셋을 1 줄여서 다음에 write가 발행되도록 함. */
	if (td_randtrimwrite(td) && ddir == DDIR_TRIM &&
	    f->last_start[DDIR_TRIM] == io_u->offset)
		f->last_start[DDIR_WRITE]--;

	io_u->verify_offset = io_u->offset;
	return 0;
}

/* [한국어]
 * io_u_fits - 제안된 I/O 가 잡의 유효 파일 범위(io_size + start_offset) 안에 드는지 판정.
 *
 * @td: 잡 스레드 (start_offset 계산).
 * @io_u: io_u->offset 기준.
 * @buflen: 시험할 길이.
 * @return: true=적합, false=오버런.
 *
 * 호출 체인: get_next_buflen 내부 재추첨 루프, fill_io_u 의 최종 검증.
 */
static inline bool io_u_fits(struct thread_data *td, struct io_u *io_u,
			     unsigned long long buflen)
{
	struct fio_file *f = io_u->file;

	/* [한국어] io_size 는 잡 옵션 상한, start_offset 은 그 시작 오프셋.
	 * f->file_offset 이 start_offset 을 반영하고 있으므로 io_size+start_offset 이
	 * 절대 주소 기준 "잡이 건드릴 수 있는 상한". */
	return io_u->offset + buflen <= f->io_size + get_start_offset(td, f);
}

/* [한국어]
 * get_next_buflen - I/O 블록 크기(바이트) 결정: 고정/bsrange/bssplit 분기.
 *
 * @td: 잡 스레드. td->o.min_bs[ddir]/max_bs[ddir]/bssplit 테이블 참조.
 *      td->bsrange_state[ddir] 가 bs 추첨 난수 상태.
 * @io_u: io_u->ddir(+ bs_is_seq_rand 시 is_random 으로 DDIR_READ/WRITE 치환).
 * @is_random: 이 I/O가 랜덤인지(순차 vs 랜덤에 다른 bs 적용 가능).
 * @return: 선택된 buflen(바이트). 0이면 파일 공간 부족으로 실패.
 *
 * 분기:
 *  1) randtrimwrite + WRITE: 직전 TRIM 의 버퍼 길이를 그대로 사용(쌍 매칭).
 *  2) min_bs == max_bs: 고정 bs 즉시 반환(가장 일반적인 FIO 설정).
 *  3) 일반: __rand 로 난수 뽑아
 *     - bssplit_nr=0 → 선형 보간 minbs + maxbs*(r/(frand_max+1)).
 *     - bssplit 있음 → 누적 확률 테이블 순회, (r/perc <= frand_max/100) 충족 시 선택.
 *  4) bs_unaligned=0 이면 minbs 단위 정렬 — minbs 가 2의 거듭제곱이면 비트마스크
 *     (buflen &= ~(minbs-1)), 아니면 나머지 연산(buflen -= buflen % minbs).
 *  5) buflen > maxbs 클램프. io_u_fits(파일 끝까지 남은 공간)에 맞을 때까지 재추첨.
 *
 * 호출 체인:
 *   fill_io_u → [이 함수]
 *   fill_multi_range_io_u → [이 함수] (범위별 반복 호출).
 */
static unsigned long long get_next_buflen(struct thread_data *td, struct io_u *io_u,
				    bool is_random)
{
	int ddir = io_u->ddir;
	unsigned long long buflen = 0;
	unsigned long long minbs, maxbs;
	uint64_t frand_max, r;
	bool power_2;

	assert(ddir_rw(ddir));

	/* [한국어] randtrimwrite 의 WRITE 는 직전 TRIM 과 같은 바이트 길이 재사용 —
	 * "trim 한 영역 그대로 다시 쓰기" 쌍 매칭. last_pos - last_start 가 trim 길이. */
	if (td_randtrimwrite(td) && ddir == DDIR_WRITE) {
		struct fio_file *f = io_u->file;

		return f->last_pos[DDIR_TRIM] - f->last_start[DDIR_TRIM];
	}

	/* [한국어] bs_is_seq_rand 옵션은 bs_[READ]/bs_[WRITE] 를 "실제 방향" 대신
	 * "순차=READ / 랜덤=WRITE" 의미로 재해석 — 읽기/쓰기 혼합이 아니면서
	 * 순차/랜덤을 다른 bs 로 테스트하려는 사용자를 위한 편의 스위치. */
	if (td->o.bs_is_seq_rand)
		ddir = is_random ? DDIR_WRITE : DDIR_READ;

	/* [한국어] 방향별 min/max bs 획득. 이후 분기 기준. */
	minbs = td->o.min_bs[ddir];
	maxbs = td->o.max_bs[ddir];

	/* [한국어] 고정 bs(블록 크기)면 추첨 전 즉시 반환 — IOPs 집약 워크로드의 핫패스. */
	if (minbs == maxbs)
		return minbs;

	/*
	 * If we can't satisfy the min block size from here, then fail
	 */
	/* [한국어] 최소 bs 조차 파일 남은 공간에 안 들어가면 실패 — 상위가 파일 끝 처리. */
	if (!io_u_fits(td, io_u, minbs))
		return 0;

	/* [한국어] bs 추첨 전용 난수 상태에서 상한값 조회 — Tausworthe 는 2^64-1 등. */
	frand_max = rand_max(&td->bsrange_state[ddir]);
	do {
		/* [한국어] 매 이터레이션마다 새 난수(재추첨) — 파일 끝 안 맞을 때 재시도. */
		r = __rand(&td->bsrange_state[ddir]);

		if (!td->o.bssplit_nr[ddir]) {
			/* [한국어] bssplit 없음 → [minbs, minbs+maxbs) 선형 보간.
			 * r/(frand_max+1.0) ∈ [0,1) 비율로 maxbs 곱한 뒤 minbs 더하기. */
			buflen = minbs + (unsigned long long) ((double) maxbs *
					(r / (frand_max + 1.0)));
		} else {
			/* [한국어] bssplit 설정: 누적 확률 분포(CDF) 기반 선택.
			 * 예) "4k/70:8k/20:16k/10" — i=0 시 perc=70, 여기에 난수가
			 * 해당하면 4k 확정. 식 (r / perc <= frand_max / 100) 은
			 * "r 이 perc 백분율 누적 내에 들어가는가" 판정(정수 버림 주의). */
			long long perc = 0;
			unsigned int i;

			for (i = 0; i < td->o.bssplit_nr[ddir]; i++) {
				struct bssplit *bsp = &td->o.bssplit[ddir][i];

				if (!bsp->perc)
					continue;
				buflen = bsp->bs;
				perc += bsp->perc;
				/* [한국어] 누적 비율에 적중 + 파일에 맞으면 이 bs 로 확정. */
				if ((r / perc <= frand_max / 100ULL) &&
				    io_u_fits(td, io_u, buflen))
					break;
			}
		}

		/* [한국어] bs 정렬: minbs 단위 경계로 내림. 2의 거듭제곱이면 AND 마스크
		 * (buflen & ~(minbs-1)) 로 1 cycle, 아니면 modulo 로 계산. bs_unaligned=1
		 * 이면 이 정렬 자체를 생략(비정렬 I/O 스트레스 테스트용). */
		power_2 = is_power_of_2(minbs);
		if (!td->o.bs_unaligned && power_2)
			buflen &= ~(minbs - 1);
		else if (!td->o.bs_unaligned && !power_2)
			buflen -= buflen % minbs;
		/* [한국어] 보간 결과가 maxbs 초과하면 클램프(부동소수점 반올림 여유 흡수). */
		if (buflen > maxbs)
			buflen = maxbs;
	} while (!io_u_fits(td, io_u, buflen));
	/* [한국어] 파일 남은 공간이 buflen 에 맞을 때까지 재추첨 루프. 이론상
	 * 무한 루프 가능하나 io_u_fits(minbs) 가 위에서 보장되어 있어 최소 bs 까지는
	 * 축소되며 결국 수렴. */

	return buflen;
}

/* [한국어]
 * set_rwmix_bytes - rwmix 방향 전환 임계점(td->rwmix_issues) 재계산.
 *
 * @td: 잡 스레드.
 *
 * 호출 시점: get_rw_ddir 이 rwmix 방향을 바꿀 때.
 *
 * 계산식: rwmix_issues = io_issues[rwmix_ddir] * rwmix[rwmix_ddir^1] / 100
 *   (반대 방향 비율만큼 현재 방향을 더 발행하면 누적 비율이 rwmix 에 수렴).
 *   바이트 기반이 아니라 발행 횟수 기반인 이유: 버퍼드 쓰기는 빠르게 submit 되지만
 *   reads 는 실질 완료까지 걸려 바이트 기준이면 왜곡됨(주석 원문 참조).
 */
static void set_rwmix_bytes(struct thread_data *td)
{
	unsigned int diff;

	/*
	 * we do time or byte based switch. this is needed because
	 * buffered writes may issue a lot quicker than they complete,
	 * whereas reads do not.
	 */
	/* [한국어] 반대 방향의 비율을 가져와 전환 임계치 계산 */
	diff = td->o.rwmix[td->rwmix_ddir ^ 1];
	td->rwmix_issues = (td->io_issues[td->rwmix_ddir] * diff) / 100;
}

/* [한국어]
 * get_rand_ddir - rwmix 확률 추첨으로 DDIR_READ/WRITE 선택.
 *
 * @td: 잡 스레드. td->rwmix_state 가 추첨 난수 상태.
 * @return: DDIR_READ 또는 DDIR_WRITE.
 *
 * 1~100 균등 추첨, v <= rwmix[READ] 면 READ 아니면 WRITE.
 * 호출 체인: get_rw_ddir → [이 함수] (rwmix 재전환 시점).
 */
static inline enum fio_ddir get_rand_ddir(struct thread_data *td)
{
	unsigned int v;

	/* [한국어] 1~100 사이 난수 생성 */
	v = rand_between(&td->rwmix_state, 1, 100);

	/* [한국어] rwmix[READ] 이하이면 읽기, 초과하면 쓰기 */
	if (v <= td->o.rwmix[DDIR_READ])
		return DDIR_READ;

	return DDIR_WRITE;
}

/* [한국어]
 * io_u_quiesce - 진행 중 I/O 모두 완료 대기(drain) — 슬립/QD조정 전 안정화.
 *
 * @td: 잡 스레드.
 * @return: 처리한 완료 수(>=0) 또는 마지막 에러(음수).
 *
 * 용도:
 *  - usec_sleep 등 blocking 전: 이미 발행된 I/O 의 latency 측정 왜곡 방지.
 *  - lat_target 이 QD 를 낮출 때: 상위 QD 시절 in-flight 가 축적되어 다음 사이클
 *    latency 가 스파이크나는 "ramp-down 폭풍" 방지.
 *  - verify 상태 전이 전.
 *  - regrow_logs: TD_F_REGROW_LOGS 세트 시 iops/bw/clat 로그 배열 확장.
 *
 * 내부 흐름:
 *   1. io_u_queued (SQE 빌드만 된 상태) 또는 cur_depth 양수 → td_io_commit (batch submit).
 *   2. io_u_in_flight > 0 인 동안 io_u_queued_complete(1) 루프 → 모두 완료.
 *   3. TD_F_REGROW_LOGS 설정되어 있으면 regrow_logs.
 *
 * 실행 컨텍스트: 잡 스레드.
 */
int io_u_quiesce(struct thread_data *td)
{
	int ret = 0, completed = 0, err = 0;

	/*
	 * We are going to sleep, ensure that we flush anything pending as
	 * not to skew our latency numbers.
	 *
	 * Changed to only monitor 'in flight' requests here instead of the
	 * td->cur_depth, b/c td->cur_depth does not accurately represent
	 * io's that have been actually submitted to an async engine,
	 * and cur_depth is meaningless for sync engines.
	 */
	/* [한국어] io_u_queued 는 SQE 빌드만 되고 아직 commit 안 된 io_u 수. cur_depth 는
	 * 잡이 점유한 io_u 수. 둘 중 하나라도 있으면 flush 해야 in-flight 가 정확 — 그
	 * 후 getevents 로 완료 수확. td_io_commit 은 엔진의 .commit 콜백 호출 = 실제
	 * 커널 submit(io_uring_enter, io_submit). */
	if (td->io_u_queued || td->cur_depth)
		td_io_commit(td);

	/* [한국어] io_u_in_flight 는 submit 후 완료 안 된 io_u 수 — 0 될 때까지 반복
	 * getevents(1). 1개씩 완료하는 이유: 각 완료 시 io_completed 가 다른 io_u 를
	 * 재큐잉할 수 있어 배치 경계 관리가 단순. */
	while (td->io_u_in_flight) {
		ret = io_u_queued_complete(td, 1);
		if (ret > 0)
			completed += ret;
		else if (ret < 0)
			err = ret;
	}

	/* [한국어] TD_F_REGROW_LOGS 는 "로그 배열 확장 필요" 플래그. quiesce 시점이
	 * 자연스러운 확장 지점(다른 io_u 진행 중이 아님) — 확장 중 메모리 이동이
	 * 있으면 안전. */
	if (td->flags & TD_F_REGROW_LOGS)
		regrow_logs(td);

	/* [한국어] 반환값 해석: completed 가 있으면 성공 우선(>0). 없으면 err(음수) 전파. */
	if (completed)
		return completed;

	return err;
}

/* [한국어]
 * rate_ddir - rate 옵션 준수를 위한 방향/타이밍 조정 (전환·슬립).
 *
 * @td: 잡 스레드. td->rate_next_io_time[ddir] 가 방향별 "다음 발행 허용 시각".
 * @ddir: 상위(get_rw_ddir)에서 결정한 원 방향.
 * @return: 조정된 방향(원 방향 유지/반대 방향 전환/DDIR_TIMEOUT).
 *
 * rate=<r,w,t> / rate_iops=<r,w,t> 옵션은 방향별 목표 속도 지정. fio 는 발행 시점에
 * rate_next_io_time 을 체크하여:
 *  - 이미 지났음(now >= next_time) → 지체 없이 발행.
 *  - 아직 미래임 + td_rw + 반대 방향이 이미 지체 → 반대 방향으로 전환.
 *  - 둘 다 미래 → 더 가까운 쪽으로 슬립. 더 가까운 쪽이 반대 방향이면 방향 전환.
 *  - io_submit_mode=INLINE 이면 슬립 전 io_u_quiesce 로 in-flight 청산.
 *  - 슬립 후 timeout 초과 → DDIR_TIMEOUT 반환(상위에서 잡 종료).
 *
 * 호출 체인:
 *   get_rw_ddir → (should_check_rate true 면) [이 함수].
 */
static enum fio_ddir rate_ddir(struct thread_data *td, enum fio_ddir ddir)
{
	/* [한국어] XOR 1 트릭: READ(0)↔WRITE(1) 토글. ddir 이 TRIM 이면 사용 안 됨(위에서 가드). */
	enum fio_ddir odir = ddir ^ 1;
	uint64_t usec;
	uint64_t now;

	assert(ddir_rw(ddir));
	/* [한국어] epoch(잡 시작 시각) 대비 경과 usec — rate_next_io_time 과 동일 좌표계. */
	now = utime_since_now(&td->epoch);

	/*
	 * if rate_next_io_time is in the past, need to catch up to rate
	 */
	/* [한국어] rate_next_io_time <= now: 목표 속도 대비 이미 뒤처져 있음 → 슬립
	 * 없이 즉시 발행해 따라잡기. */
	if (td->rate_next_io_time[ddir] <= now)
		return ddir;

	/*
	 * We are ahead of rate in this direction. See if we
	 * should switch.
	 */
	/* [한국어] 이 방향은 앞서 있음. 혼합 잡이면 반대 방향으로 전환 가능성 검토. */
	if (td_rw(td) && td->o.rwmix[odir]) {
		/*
		 * Other direction is behind rate, switch
		 */
		/* [한국어] 반대 방향이 뒤처져 있으면 즉시 그쪽으로 전환 — 슬립 없음. */
		if (td->rate_next_io_time[odir] <= now)
			return odir;

		/*
		 * Both directions are ahead of rate. sleep the min,
		 * switch if necessary
		 */
		/* [한국어] 양쪽 모두 앞서 있음 → 더 빨리 허용되는 쪽으로 슬립. 반대
		 * 방향이 더 빠르면 ddir 자체를 odir 로 바꿔 슬립 후 반대 방향 발행. */
		if (td->rate_next_io_time[ddir] <=
		    td->rate_next_io_time[odir]) {
			usec = td->rate_next_io_time[ddir] - now;
		} else {
			usec = td->rate_next_io_time[odir] - now;
			ddir = odir;
		}
	} else
		/* [한국어] 단일 방향 잡: 그냥 이 방향의 "다음 허용 시각"까지 기다림. */
		usec = td->rate_next_io_time[ddir] - now;

	/* [한국어] io_submit_mode=INLINE(기본) 은 잡 스레드가 직접 submit — 슬립 전에
	 * in-flight 을 청산해야 ramp-down 폭풍(슬립 도중 완료 누적이 다음 사이클에
	 * 한꺼번에 터져 latency 왜곡) 방지. OFFLOAD 모드는 별도 스레드가 처리해 필요 없음. */
	if (td->o.io_submit_mode == IO_MODE_INLINE)
		io_u_quiesce(td);

	/* [한국어] 슬립 이후가 timeout(잡 제한시간)을 초과하면 슬립 최소화 + DDIR_TIMEOUT
	 * 으로 잡 종료. usec 이 음수가 될 수 있는 케이스(now > timeout)는 별도 처리. */
	if (td->o.timeout && ((usec + now) > td->o.timeout)) {
		/*
		 * check if the usec is capable of taking negative values
		 */
		if (now > td->o.timeout) {
			ddir = DDIR_TIMEOUT;
			return ddir;
		}
		usec = td->o.timeout - now;
	}
	/* [한국어] usec_sleep: 짧으면 spin, 길면 nanosleep. td->terminate 폴링 가능. */
	usec_sleep(td, usec);

	/* [한국어] 슬립에서 깨어나면 상황 재평가 — timeout 경과 또는 terminate 신호
	 * 받았으면 잡 종료 유도(DDIR_TIMEOUT). */
	now = utime_since_now(&td->epoch);
	if ((td->o.timeout && (now > td->o.timeout)) || td->terminate)
		ddir = DDIR_TIMEOUT;

	return ddir;
}

/*
 * Return the data direction for the next io_u. If the job is a
 * mixed read/write workload, check the rwmix cycle and switch if
 * necessary.
 */
/* [한국어]
 * get_rw_ddir - 다음 io_u 의 데이터 방향 결정 (SYNC 메타 우선 + rwmix + rate).
 *
 * @td: 잡 스레드. 내부 상태: td->rwmix_ddir(현재 rwmix 고정 방향),
 *      td->rwmix_issues(방향 전환 임계), td->last_ddir_issued, td->io_issues[].
 * @return: fio_ddir enum(READ/WRITE/TRIM/SYNC/DATASYNC/SYNC_FILE_RANGE/INVAL/TIMEOUT).
 *
 * 결정 순서:
 *   1. 마지막 발행이 WRITE 였으면 sync 주기 3종 체크:
 *      - fsync_blocks 주기 → DDIR_SYNC
 *      - fdatasync_blocks 주기 → DDIR_DATASYNC
 *      - sync_file_range_nr 주기 → DDIR_SYNC_FILE_RANGE
 *   2. td_rw(읽기+쓰기 혼합):
 *      - rwmix_ddir 방향으로 rwmix_issues 만큼 발행했으면 → get_rand_ddir 로
 *        rwmix[READ] 확률 추첨 → 전환 시 set_rwmix_bytes 로 다음 전환 임계 재계산.
 *   3. 단일 방향: td_read/td_write/td_trim → 해당 ddir.
 *   4. should_check_rate(rate 옵션 없음)이면 시각 조회 생략(50% IOPs 최적화),
 *      있으면 rate_ddir 로 타이밍/전환 조정.
 *
 * 호출 체인: set_rw_ddir → [이 함수].
 */
static enum fio_ddir get_rw_ddir(struct thread_data *td)
{
	enum fio_ddir ddir;

	/*
	 * See if it's time to fsync/fdatasync/sync_file_range first,
	 * and if not then move on to check regular I/Os.
	 */
	/* [한국어] sync 는 쓰기 누적 후에만 의미 있음(last_ddir_issued == WRITE 가드).
	 * should_fsync: 전역 옵션/런타임 조건(예: 종료 직전 aux sync). */
	if (should_fsync(td) && td->last_ddir_issued == DDIR_WRITE) {
		/* [한국어] 쓰기 io_issues 가 fsync_blocks 배수일 때 fsync 삽입 —
		 * io_issues>0 가드는 "0번째 IO 에 바로 fsync" 오판 방지. */
		if (td->o.fsync_blocks && td->io_issues[DDIR_WRITE] &&
		    !(td->io_issues[DDIR_WRITE] % td->o.fsync_blocks))
			return DDIR_SYNC;

		/* [한국어] fdatasync 주기 체크(메타 제외 데이터 동기). */
		if (td->o.fdatasync_blocks && td->io_issues[DDIR_WRITE] &&
		    !(td->io_issues[DDIR_WRITE] % td->o.fdatasync_blocks))
			return DDIR_DATASYNC;

		/* [한국어] sync_file_range 주기 — 부분 sync. first_write/last_write
		 * 범위에 기반해 실행. */
		if (td->sync_file_range_nr && td->io_issues[DDIR_WRITE] &&
		    !(td->io_issues[DDIR_WRITE] % td->sync_file_range_nr))
			return DDIR_SYNC_FILE_RANGE;
	}

	if (td_rw(td)) {
		/*
		 * Check if it's time to seed a new data direction.
		 */
		/* [한국어] 현재 rwmix_ddir 방향으로 이미 rwmix_issues 개 발행했으면
		 * 비율 전환 타이밍 — 다른 방향으로 넘어갈지 추첨. */
		if (td->io_issues[td->rwmix_ddir] >= td->rwmix_issues) {
			/*
			 * Put a top limit on how many bytes we do for
			 * one data direction, to avoid overflowing the
			 * ranges too much
			 */
			/* [한국어] rwmix[READ]% 확률로 READ, 나머지는 WRITE — 원본 비율 유지. */
			ddir = get_rand_ddir(td);

			/* [한국어] 실제로 방향 전환되면 새 목표 임계값(rwmix_issues) 재계산.
			 * 같은 방향이면 기존 임계값 유지. */
			if (ddir != td->rwmix_ddir)
				set_rwmix_bytes(td);

			td->rwmix_ddir = ddir;
		}
		ddir = td->rwmix_ddir;
	} else if (td_read(td))
		/* [한국어] rw=read 옵션 — 전용 READ. */
		ddir = DDIR_READ;
	else if (td_write(td))
		/* [한국어] rw=write — 전용 WRITE. */
		ddir = DDIR_WRITE;
	else if (td_trim(td))
		/* [한국어] rw=trim — 전용 TRIM (trimwrite 는 set_rw_ddir 의 토글로 처리). */
		ddir = DDIR_TRIM;
	else
		/* [한국어] 옵션 조합 오류 — 상위에서 INVAL 감지 후 잡 종료. */
		ddir = DDIR_INVAL;

	if (!should_check_rate(td)) {
		/*
		 * avoid time-consuming call to utime_since_now() if rate checking
		 * isn't being used. this imrpoves IOPs 50%. See:
		 * https://github.com/axboe/fio/issues/1501#issuecomment-1418327049
		 */
		/* [한국어] rate 옵션 미설정 → rate_ddir 호출(=utime_since_now gettime) 생략.
		 * 높은 IOPs 워크로드(수백만 IOPs)에서 gettime 오버헤드가 50% 까지 차지할 수 있음. */
		td->rwmix_ddir = ddir;
	} else
		/* [한국어] rate 지정 시 타이밍/전환 조정(슬립, 방향 스왑, timeout 처리). */
		td->rwmix_ddir = rate_ddir(td, ddir);
	return td->rwmix_ddir;
}

/* [한국어]
 * set_rw_ddir - io_u->ddir 및 acct_ddir 설정 + ZBD/trimwrite/barrier 후처리.
 *
 * @td: 잡 스레드.
 * @io_u: 방향 결정 대상.
 *
 * 단계:
 *   1. get_rw_ddir: 순수 방향 결정.
 *   2. ZBD: zbd_adjust_ddir 로 write pointer 상태에 따라 READ→WRITE 등 재할당 가능.
 *   3. trimwrite (td_trimwrite): last_start[WRITE]==last_start[TRIM] → 다음은 TRIM,
 *      다르면 WRITE. 이 토글이 "쓰기/트림 쌍" 교대 발행의 핵심.
 *   4. io_u->ddir = io_u->acct_ddir = ddir (통계도 같은 방향으로 기록).
 *   5. WRITE + FIO_BARRIER 엔진 플래그 + barrier_blocks 주기 일치 → IO_U_F_BARRIER
 *      set (BSG/SCSI 일부 엔진에서 의미, 엔진이 FUA 또는 cache flush 유발).
 *
 * 호출 체인: fill_io_u → [이 함수].
 */
static void set_rw_ddir(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] get_rw_ddir: rwmix/rate/sync 주기 등 모든 방향 결정 요인 통합. */
	enum fio_ddir ddir = get_rw_ddir(td);

	/* [한국어] ZBD(ZNS) 모드: write pointer 가 전체 존을 다 채운 상태면 READ 로,
	 * 존이 비어 있는 상태에서 READ 요청이면 대체 존 선택 등 상태 기반 재할당. */
	if (td->o.zone_mode == ZONE_MODE_ZBD)
		ddir = zbd_adjust_ddir(td, io_u, ddir);

	/* [한국어] trimwrite(rw=trimwrite) 모드: 각 영역에 WRITE → TRIM 쌍으로 발행.
	 * last_start[WRITE]==last_start[TRIM] 은 "방금 쌍 완성" 상태 → 다음 위치에
	 * TRIM 시작. 다르면 "TRIM 은 발행했고 WRITE 가 따라올 차례". */
	if (td_trimwrite(td) && !ddir_sync(ddir)) {
		struct fio_file *f = io_u->file;
		if (f->last_start[DDIR_WRITE] == f->last_start[DDIR_TRIM])
			ddir = DDIR_TRIM;
		else
			ddir = DDIR_WRITE;
	}

	/* [한국어] ddir: 실제 I/O 방향. acct_ddir: 통계 기록 방향 — 같게 두지만 trim
	 * 등이 별도 통계로 잡힐 때 달라질 수 있음. */
	io_u->ddir = io_u->acct_ddir = ddir;

	/* [한국어] BARRIER 플래그 (FIO_BARRIER 엔진만 의미 있음 — BSG/SCSI 일부). 첫
	 * 쓰기(!io_issues==0) 는 제외하여 "주기 0 에 무조건 세트" 버그 방지. 엔진이
	 * 이 플래그 보고 FUA 또는 cache flush 삽입. */
	if (io_u->ddir == DDIR_WRITE && td_ioengine_flagged(td, FIO_BARRIER) &&
	    td->o.barrier_blocks &&
	   !(td->io_issues[DDIR_WRITE] % td->o.barrier_blocks) &&
	     td->io_issues[DDIR_WRITE])
		io_u_set(td, io_u, IO_U_F_BARRIER);
}

/* [한국어]
 * put_file_log - fio_file 참조 카운트 감소 + 실패 시 td_verror 로 에러 기록.
 *
 * @td: 잡 스레드.
 * @f: 해제 대상 fio_file. f->num_ref가 0이 되면 filesetup.c 가 실제 close 예약.
 *
 * 호출 체인:
 *   put_io_u → [이 함수] (io_u 가 파일 참조를 가진 경우)
 *   set_io_u_file 에러 회수 경로 → [이 함수]
 *   __get_io_u 재큐잉 경로 (runstate == TD_FSYNCING) → [이 함수]
 *
 * 에러 경로: put_file 반환값(ENOSPC/EIO 등)을 td_verror("file close") 로 기록 —
 * 잡 상태에 에러 마킹만 하고 계속 진행(파일 개별 실패가 잡 전체 중단 아님).
 */
void put_file_log(struct thread_data *td, struct fio_file *f)
{
	unsigned int ret = put_file(td, f);

	if (ret)
		td_verror(td, ret, "file close");
}

/* [한국어]
 * put_io_u - ★ 핵심 ★ io_u 생명주기 종료 단계: 완료된 io_u를 freelist로 반환.
 *
 * @td: 잡 스레드 데이터(자식이면 td->parent로 스위치).
 * @io_u: 반환할 I/O 유닛. IO_U_F_FREE 이 아니어야 하며, FLIGHT 플래그는
 *        io_completed/requeue_io_u에서 이미 clear되어 있다고 가정.
 *
 * io_u 상태 전이:
 *   [completed or cancelled]
 *         │ put_io_u()
 *         ▼
 *   1) zbd_put_io_u: ZBD 모드 시 open zone 참조 카운트 감소(경쟁: zbd.c lock).
 *   2) needs_lock = td_async_processing(td): verify 스레드/offload 모드에서는
 *      freelist/free_cond를 backend·verify 스레드가 공유 → mutex 필요.
 *   3) td = td->parent (자식이면): io_u 풀은 부모 소유.
 *   4) put_file_log (파일 참조 카운트--): fio_file::num_ref 감소. 0이 되면
 *      close_file_timer 가 td_io_close_file 트리거.
 *   5) io_u->file = NULL; IO_U_F_FREE 세트.
 *   6) IN_CUR_DEPTH 플래그가 있으면 td->cur_depth--. 자식 스레드는 이 경로
 *      불가(assert TD_F_CHILD 아님).
 *   7) io_u_qpush(&td->io_u_freelist, io_u): 프리리스트 테일에 재삽입 — 이후
 *      __get_io_u가 이 io_u를 popback으로 꺼냄.
 *   8) td_io_u_free_notify: free_cond 브로드캐스트로 대기 중인 async processing
 *      스레드에 "io_u 하나 풀렸음" 신호.
 *
 * 실행 컨텍스트: 잡 스레드(대부분) 또는 verify/offload 스레드. 후자에서는 needs_lock
 * 가 true가 되어 __td_io_u_lock/unlock으로 freelist 접근 직렬화.
 *
 * 호출 체인 (caller):
 *   io_u_sync_complete → [이 함수]
 *   ios_completed → [이 함수] (requeue가 아닌 경우)
 *   set_io_u_file 에러 경로 → [이 함수] (fill 실패 회수)
 *   clear_io_u → [이 함수]
 *   verify 경로: verify_io_u → [이 함수]
 *   helper thread의 청소 경로에서도 호출 가능.
 *
 * 에러 경로: 이 함수 자체는 실패하지 않음(반환 void). put_file_log 실패는
 * td_verror로 스레드 에러 마킹되지만 io_u 회수는 계속.
 */
void put_io_u(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 비동기 처리 활성 시에만 락 — verify_async(별도 스레드가 verify)
	 * 또는 io_submit_mode=offload(별도 스레드가 submit) 모드에서 freelist 를
	 * 잡 스레드와 그 헬퍼가 공유 → pthread_mutex 필요. 일반 모드(단일 스레드)는
	 * 락 자체가 캐시 오염을 일으켜 회피. */
	const bool needs_lock = td_async_processing(td);

	/* [한국어] ZBD(Zoned Block Device) 가 활성이면 open_zone 참조 카운트 감소.
	 * 존 내부 동시성 제어용. 일반 장치는 no-op. */
	zbd_put_io_u(td, io_u);

	/* [한국어] verify 스레드 등 자식 스레드에서 호출한 경우 io_u 풀은 부모 소유
	 * → 모든 갱신이 부모 td 기준으로 이뤄져야 cur_depth/freelist 일관성 유지. */
	if (td->parent)
		td = td->parent;

	if (needs_lock)
		__td_io_u_lock(td);

	/* [한국어] IO_U_F_NO_FILE_PUT: verify 경로 등에서 파일 참조 해제를 명시적으로
	 * 보류할 때 사용. 일반 경로는 put_file_log 로 file->num_ref 감소 → 0 되면
	 * 파일 close 스케줄. */
	if (io_u->file && !(io_u->flags & IO_U_F_NO_FILE_PUT))
		put_file_log(td, io_u->file);

	/* [한국어] 파일 바인딩 해제. 다음 __get_io_u 에서 꺼내질 때 file==NULL 이
	 * "빈 슬롯" 마커 — set_io_u_file 재수행 필요 판단. */
	io_u->file = NULL;
	/* [한국어] IO_U_F_FREE 세트 — queue_full/__get_io_u 의 assert 가 이 비트 확인. */
	io_u_set(td, io_u, IO_U_F_FREE);

	/* [한국어] IO_U_F_IN_CUR_DEPTH 는 __get_io_u 에서 cur_depth++ 할 때 세트됨 —
	 * 해제 시 cur_depth--. 자식 스레드는 이 경로 불가(부모 cur_depth 직접 조작
	 * 금지, TD_F_CHILD assert 로 탐지). */
	if (io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		assert(!(td->flags & TD_F_CHILD));
	}
	/* [한국어] io_u_qpush: freelist 테일에 재삽입(FIFO 순환). __get_io_u 는
	 * io_u_qpop(헤드) 로 꺼냄 → LRU 성 보장(가장 오래전 해제된 것부터 재사용). */
	io_u_qpush(&td->io_u_freelist, io_u);
	/* [한국어] free_cond 브로드캐스트로 __get_io_u 에서 cond_wait 중이던
	 * verify_async/offload 스레드 깨움 — "io_u 하나 반납됨" 신호. */
	td_io_u_free_notify(td);

	if (needs_lock)
		__td_io_u_unlock(td);
}

/* [한국어]
 * io_u_clear_inflight_flags - in-flight 관련 3비트(FLIGHT|BUSY_OK|PATTERN_DONE) clear.
 *
 * 분리된 이유: io_completed 와 clear_io_u 에서 동일 3비트를 한 번에 지우기 위함.
 * PATTERN_DONE: fill_buffer_pattern 단계 완료 플래그 (다음 WRITE 재사용 차단).
 * BUSY_OK: mark_random_map 스킵 허용 (중복 접근 허용). FLIGHT: 엔진 제출 상태.
 */
static inline void io_u_clear_inflight_flags(struct thread_data *td,
					      struct io_u *io_u)
{
	io_u_clear(td, io_u, IO_U_F_FLIGHT | IO_U_F_BUSY_OK |
		   IO_U_F_PATTERN_DONE);
}

/* [한국어]
 * clear_io_u - 엔진 에러 회수 경로에서 in-flight io_u 를 취소 처리 + freelist 복귀.
 *
 * @td: 잡 스레드.
 * @io_u: 취소할 I/O 유닛.
 *
 * 용도: 엔진 queue/prep 이 실패한 경우, 또는 잡 종료 시 정리 단계에서
 * in-flight 상태로 남은 io_u 를 정상 경로(io_completed)를 거치지 않고 직접 회수.
 * 통계/회계는 기록하지 않음(실패 I/O 는 account 대상 아님).
 *
 * 호출 체인:
 *   engines/*.c의 에러 경로 → [이 함수]
 *   backend.c cleanup_io_u → [이 함수]
 */
void clear_io_u(struct thread_data *td, struct io_u *io_u)
{
	io_u_clear_inflight_flags(td, io_u);
	put_io_u(td, io_u);
}

/* [한국어]
 * requeue_io_u - io_u 를 io_u_requeues 큐로 되돌려 다음 이터레이션에 재시도.
 *
 * @td: 잡 스레드(자식이면 부모 td 사용).
 * @io_u: 이중 포인터. 함수 종료 시 *io_u=NULL 로 설정해 호출자가 put_io_u를
 *        중복 호출하지 않도록 함.
 *
 * 사용 케이스:
 *  1) short I/O (io_completed에서 bytes < xfer_buflen): 이미 보낸 앞부분은 완료로
 *     처리, 남은 xfer_buflen=resid, xfer_buf+=bytes, offset+=bytes 로 갱신하여
 *     requeue_io_u → 다음 do_io 사이클이 __get_io_u 의 requeues 우선 경로로 발행.
 *  2) FIO_Q_BUSY (td_io_queue 반환): 엔진 큐 포화. io_issues[ddir]-- 로 카운터
 *     원복, IO_U_F_FLIGHT clear, cur_depth-- 후 재큐잉.
 *
 * io_u 상태 전이:
 *   [in_flight or partial] → requeue_io_u → [io_u_requeues]
 *     (IO_U_F_FLIGHT clear, IO_U_F_FREE set, cur_depth--, io_issues[ddir]--)
 *                           → 다음 __get_io_u 에서 우선 재꺼냄
 *
 * 동기화: needs_lock(async processing) 시 td->io_u_lock 보호.
 *
 * 실행 컨텍스트: 잡 스레드 또는 async verify 스레드.
 *
 * 호출 체인:
 *   io_completed (short I/O) → [이 함수]
 *   backend.c do_io (FIO_Q_BUSY 경로) → [이 함수]
 *
 * 에러 경로: 없음(void). 가정: io_u->flags 의 FLIGHT 비트는 함수 진입 시에만 유효.
 */
void requeue_io_u(struct thread_data *td, struct io_u **io_u)
{
	const bool needs_lock = td_async_processing(td);
	struct io_u *__io_u = *io_u;
	/* [한국어] acct_ddir 는 io_u->acct_ddir(통계 기록용 방향). TRIM/SYNC 등의
	 * 실제 ddir 과 다를 수 있어(예: trimwrite 의 write 는 ddir=WRITE, acct=WRITE). */
	enum fio_ddir ddir = acct_ddir(__io_u);

	dprint(FD_IO, "requeue %p\n", __io_u);

	if (td->parent)
		td = td->parent;

	if (needs_lock)
		__td_io_u_lock(td);

	/* [한국어] FREE 비트 세트 — freelist/requeues 둘 다 "발급 가능" 상태 의미. */
	io_u_set(td, __io_u, IO_U_F_FREE);
	/* [한국어] FIO_Q_BUSY 반환 경로: td_io_queue 가 io_issues[ddir]++ 를 이미 수행
	 * 했는데 엔진이 실제론 submit 하지 못했으므로 카운터 원복 필요. short I/O
	 * 경로에선 FLIGHT 인 상태로 들어오므로 조건 일치. */
	if ((__io_u->flags & IO_U_F_FLIGHT) && ddir_rw(ddir))
		td->io_issues[ddir]--;

	/* [한국어] FLIGHT clear — 더 이상 엔진에 점유되어 있지 않음. */
	io_u_clear(td, __io_u, IO_U_F_FLIGHT);
	/* [한국어] put_io_u 와 동일 — IN_CUR_DEPTH 였다면 cur_depth-- 로 부채 해소. */
	if (__io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		assert(!(td->flags & TD_F_CHILD));
	}

	/* [한국어] io_u_rpush: requeues 헤드에 push → __get_io_u 의 io_u_rpop 이
	 * 이 io_u 를 freelist 보다 먼저 꺼내 재시도. offset/buflen/file 보존. */
	io_u_rpush(&td->io_u_requeues, __io_u);
	/* [한국어] 대기 중 스레드에게 "재큐잉된 io_u 있음" 신호(free_cond 와 공유). */
	td_io_u_free_notify(td);

	if (needs_lock)
		__td_io_u_unlock(td);

	/* [한국어] *io_u=NULL: 호출자(io_completed 등)가 이 io_u 에 대해 put_io_u 를
	 * 이중 호출하지 않도록 마커 리셋. 이중 해제 방지. */
	*io_u = NULL;
}

/* [한국어]
 * setup_strided_zone_mode - zone_mode=strided 에서 존 간 이동/순환 관리.
 *
 * @td: 잡 스레드. td->zone_bytes 가 현재 존에서 누적 I/O 바이트, td->o.zone_size/
 *      zone_range/zone_skip 옵션으로 존 레이아웃 결정.
 * @io_u: 대상 I/O 유닛.
 *
 * 동작: zone_bytes >= zone_size 가 되면
 *   - file_offset += zone_range + zone_skip (다음 존 시작점).
 *   - 파일 끝 넘으면 wrap to start_offset.
 *   - zone_bytes 리셋.
 *   - io_skip_bytes 누적(통계에서 제외).
 * zone_size > zone_range 인 경우 같은 존 반복: last_pos 를 file_offset 으로 리셋.
 * zone_bytes % zone_range == 0 일 때 fio_file_reset 으로 랜덤맵 재사용 가능하게.
 *
 * ZBD 모드와의 차이: strided 는 zone_range 로 간격 이동만, 실제 존 상태 추적 안 함.
 * 호출 체인: fill_io_u → [이 함수] (zone_mode == STRIDED 분기).
 */
static void setup_strided_zone_mode(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;

	assert(td->o.zone_mode == ZONE_MODE_STRIDED);
	assert(td->o.zone_size);
	assert(td->o.zone_range);

	/*
	 * See if it's time to switch to a new zone
	 */
	/* [한국어] 현재 존 누적 바이트가 zone_size 도달 → 다음 존으로 이동.
	 * zone_bytes 리셋, file_offset 이 (zone_range + zone_skip) 만큼 전진.
	 * zone_skip 은 존 사이에 건너뛰는 공간(핫/콜드 블록 분리 시뮬레이션). */
	if (td->zone_bytes >= td->o.zone_size) {
		td->zone_bytes = 0;
		f->file_offset += td->o.zone_range + td->o.zone_skip;

		/*
		 * Wrap from the beginning, if we exceed the file size
		 */
		/* [한국어] 파일 끝 넘으면 start_offset 으로 wrap — 존 주기가 파일 전체를 순회. */
		if (f->file_offset >= f->real_file_size)
			f->file_offset = get_start_offset(td, f);

		/* [한국어] last_pos 도 새 file_offset 으로 동기 — 순차 I/O 가 새 존 시작점부터 재개. */
		f->last_pos[io_u->ddir] = f->file_offset;
		/* [한국어] io_skip_bytes 누적 — 전체 I/O 통계에서 제외되는 "건너뛴" 공간 기록. */
		td->io_skip_bytes += td->o.zone_skip;
	}

	/*
	 * If zone_size > zone_range, then maintain the same zone until
	 * zone_bytes >= zone_size.
	 */
	/* [한국어] zone_size > zone_range 구성에서 순차 진행이 zone_range 경계를
	 * 넘으면 "존을 다 덮음" → 같은 존의 시작으로 되돌려 동일 존 반복 I/O.
	 * zone_bytes 가 zone_size 에 도달할 때까지 이 재반복. */
	if (f->last_pos[io_u->ddir] >= (f->file_offset + td->o.zone_range)) {
		dprint(FD_IO, "io_u maintain zone offset=%" PRIu64 "/last_pos=%" PRIu64 "\n",
				f->file_offset, f->last_pos[io_u->ddir]);
		f->last_pos[io_u->ddir] = f->file_offset;
	}

	/*
	 * For random: if 'norandommap' is not set and zone_size > zone_range,
	 * map needs to be reset as it's done with zone_range everytime.
	 */
	/* [한국어] zone_range 바이트 주기마다 axmap 리셋 — 존 내 재반복 시 랜덤맵 재사용.
	 * zone_bytes % zone_range == 0 은 "존 경계 정확히 도달" 조건. */
	if ((td->zone_bytes % td->o.zone_range) == 0)
		fio_file_reset(td, f);
}

/* [한국어]
 * fill_multi_range_io_u - 다중 범위 TRIM I/O 생성 (num_range > 1 인 DSM discard).
 *
 * @td: 잡 스레드. td->o.num_range 가 한 DSM 명령에 실을 범위 수.
 * @io_u: io_u->buf 에 struct trim_range 배열을 쌓는다.
 * @return: 0=성공(io_u->buflen=전체 trim 길이, number_trim=실제 범위 수),
 *          1=첫 범위조차 못 만든 실패.
 *
 * NVMe Dataset Management(DSM) 명령은 한 번에 여러 LBA 범위를 discard 가능 →
 * fio 는 num_range 개 범위를 한 번에 생성해 엔진(NVMe passthru)에 전달.
 *
 * 처리: num_range 회 루프 — get_next_offset/get_next_buflen/파일 범위 검증 후
 * struct trim_range { start, len } 를 io_u->buf 에 차례로 쌓음.
 * 각 범위마다 f->last_start/last_pos 갱신 및 axmap 마킹(td_random 시).
 * 중간 실패 시 break 후 누적된 buflen 이 양수면 성공(부분 생성도 유효).
 *
 * 호출 체인: fill_io_u → [이 함수] (multi_range_trim 분기).
 */
static int fill_multi_range_io_u(struct thread_data *td, struct io_u *io_u)
{
	bool is_random;
	uint64_t buflen, i = 0;
	struct trim_range *range;
	struct fio_file *f = io_u->file;
	uint8_t *buf;

	/* [한국어] io_u->buf 는 잡 초기화 시 할당된 버퍼 — 여기에 struct trim_range 를
	 * 연속 배치. 엔진(nvme passthru)이 이 배열 주소를 DSM 명령의 range descriptor
	 * 로 전달해 한 번에 여러 LBA 범위를 discard. */
	buf = io_u->buf;
	buflen = 0;

	/* [한국어] num_range 개의 trim 범위를 생성. 중간 실패는 break 후 부분 성공 판정. */
	while (i < td->o.num_range) {
		range = (struct trim_range *)buf;
		/* [한국어] 각 범위마다 독립적으로 오프셋 선택 (get_next_offset 호출). */
		if (get_next_offset(td, io_u, &is_random)) {
			dprint(FD_IO, "io_u %p, failed getting offset\n",
			       io_u);
			break;
		}

		/* [한국어] 이 범위의 길이(바이트). bsrange/bssplit 분포 그대로 적용. */
		io_u->buflen = get_next_buflen(td, io_u, is_random);
		if (!io_u->buflen) {
			dprint(FD_IO, "io_u %p, failed getting buflen\n", io_u);
			break;
		}

		/* [한국어] offset+buflen 이 파일 크기를 넘으면 해당 범위는 포기 (앞까지는 유효). */
		if (io_u->offset + io_u->buflen > io_u->file->real_file_size) {
			dprint(FD_IO, "io_u %p, off=0x%llx + len=0x%llx exceeds file size=0x%llx\n",
			       io_u,
			       (unsigned long long) io_u->offset, io_u->buflen,
			       (unsigned long long) io_u->file->real_file_size);
			break;
		}

		/* [한국어] trim_range 구조체(start, len) 를 io_u->buf 에 기록. */
		range->start = io_u->offset;
		range->len = io_u->buflen;
		/* [한국어] 누적 buflen — 모든 범위의 총 바이트 수(통계용). */
		buflen += io_u->buflen;
		/* [한국어] 다음 범위 계산 및 통계/트리거에 쓰일 last_start/pos 업데이트. */
		f->last_start[io_u->ddir] = io_u->offset;
		f->last_pos[io_u->ddir] = io_u->offset + range->len;

		/* [한국어] 버퍼 포인터를 다음 trim_range 위치로 전진. */
		buf += sizeof(struct trim_range);
		i++;

		/* [한국어] 각 범위도 단일 trim 처럼 axmap 에 마킹(랜덤 모드). */
		if (td_random(td) && file_randommap(td, io_u->file))
			mark_random_map(td, io_u, io_u->offset, io_u->buflen);
		dprint_io_u(io_u, "fill");
	}
	if (buflen) {
		/*
		 * Set buffer length as overall trim length for this IO, and
		 * tell the ioengine about the number of ranges to be trimmed.
		 */
		/* [한국어] io_u->buflen 을 전체 범위 합계로, number_trim 을 실제 생성된
		 * 범위 수로 설정 — 엔진이 DSM 명령에 number_trim 을 개수 필드로 사용. */
		io_u->buflen = buflen;
		io_u->number_trim = i;
		return 0;
	}

	/* [한국어] 첫 범위도 생성 못했으면 실패 — 상위가 파일 전환/잡 종료 처리. */
	return 1;
}

/* [한국어]
 * fill_io_u - I/O 유닛에 "어디/얼마나/무엇을" 값을 채워넣는 설정 함수.
 *
 * @td: 잡 스레드.
 * @io_u: 설정할 I/O 유닛(이미 __get_io_u로 할당된 상태, file 세트 전제).
 * @return: 0=성공, 1=실패(호출자가 put_file_log + td_io_close_file로 정리).
 *
 * 채우는 필드: io_u->ddir, io_u->acct_ddir, io_u->offset, io_u->verify_offset,
 *              io_u->buflen, io_u->flags(IO_U_F_BARRIER 등), io_u->number_trim
 *              (다중 범위 trim).
 *
 * 처리 단계:
 *   1. NOIO 엔진 바이패스 (cpu/net 엔진 등 디스크 I/O 없는 경우 out).
 *   2. set_rw_ddir: get_rw_ddir (rwmix/fsync/datasync/sync_file_range 선택) +
 *      ZBD 조정(zbd_adjust_ddir) + trimwrite 토글 + IO_U_F_BARRIER 마킹.
 *   3. ddir == INVAL/TIMEOUT 이면 실패.
 *   4. ddir_sync(DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE) 는 offset/buflen 불필요 → out.
 *   5. zone_mode 분기: STRIDED (정해진 주기로 존 이동) 또는 ZBD (진짜 ZNS 존 관리).
 *   6. 다중 범위 trim (num_range > 1 + DDIR_TRIM): fill_multi_range_io_u 로 범위
 *      num_range개 연쇄 채움 → io_u->buf에 trim_range 배열 저장.
 *   7. 단일 I/O: get_next_offset (순차/랜덤/LFSR/Zipf/...) + get_next_buflen
 *      (bssplit CDF / bsrange / 단일 bs).
 *   8. ZBD: zbd_adjust_block로 WRITE는 존 write pointer에 맞춰 정렬, 읽기는
 *      유효 범위 클램프. io_u_eof 반환 시 실패(존 모두 닫힘).
 *   9. Data Placement(FDP/Streams): dp_fill_dspec_data로 io_u->dtype/dspec 채움.
 *  10. 파일 오버플로 최종 방어 검사(offset+buflen > real_file_size).
 *  11. mark_random_map (norandommap=0 + td_random + axmap 있음): axmap에 세트,
 *      세트 실패 시 buflen 축소 반영.
 *
 * 실행 컨텍스트: 잡 스레드 단독. 파일 통계/랜덤 상태 모두 td/f 소유라 락 없음.
 *
 * 호출 체인:
 *   set_io_u_file → [이 함수] (파일 선택 직후)
 *   fill_multi_range_io_u 는 같은 체인의 서브루틴.
 *
 * 에러 경로: 실패 시 호출자(set_io_u_file) 가 같은 파일로 재시도하거나
 * 파일을 close하고 다음 파일 시도(file_service_type에 따라).
 */
static int fill_io_u(struct thread_data *td, struct io_u *io_u)
{
	bool is_random;
	uint64_t offset;
	enum io_u_action ret;

	/* [한국어] FIO_NOIO 엔진(cpu/net 등)은 실제 디스크 I/O 없음 — 오프셋/버퍼 설정
	 * 전부 skip, ddir 만 결정되면 됨. 바로 out 으로 점프. */
	if (td_ioengine_flagged(td, FIO_NOIO))
		goto out;

	/* [한국어] ① ddir 결정: rwmix / fsync / datasync / sync_file_range / rate 조정 /
	 * ZBD 조정 / trimwrite 토글 / barrier_blocks 판정 일체 포함. */
	set_rw_ddir(td, io_u);

	/* [한국어] rate_ddir 가 타임아웃 반환하거나 잘못된 방향일 때. 잡 종료 유도. */
	if (io_u->ddir == DDIR_INVAL || io_u->ddir == DDIR_TIMEOUT) {
		dprint(FD_IO, "invalid direction received ddir = %d", io_u->ddir);
		return 1;
	}
	/*
	 * fsync() or fdatasync() or trim etc, we are done
	 */
	/* [한국어] ddir_sync(DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE): 오프셋/길이 개념 없음.
	 * out 에서 verify_offset=offset 은 수행되지만 의미 있는 값 아님(초기값 0). */
	if (!ddir_rw(io_u->ddir))
		goto out;

	/* [한국어] ② 존 모드 보정. STRIDED 는 fio 가 만든 가짜 존, ZBD 는 실제 ZNS 장치. */
	if (td->o.zone_mode == ZONE_MODE_STRIDED)
		setup_strided_zone_mode(td, io_u);
	else if (td->o.zone_mode == ZONE_MODE_ZBD)
		setup_zbd_zone_mode(td, io_u);

	/* [한국어] ③ 다중 범위 DSM trim (num_range>1 + DDIR_TRIM): 여러 범위를 한 개
	 * io_u 에 쌓아 NVMe passthru 가 한 번에 discard. fill_multi_range_io_u 이 전담. */
	if (multi_range_trim(td, io_u)) {
		if (fill_multi_range_io_u(td, io_u))
			return 1;
	} else {
		/*
		 * No log, let the seq/rand engine retrieve the next buflen and
		 * position.
		 */
		/* [한국어] ④-1 오프셋: 순차/랜덤(+ 분포) 분기 디스패치. is_random 출력으로
		 * 아래 mark_random_map 호출 여부 결정. */
		if (get_next_offset(td, io_u, &is_random)) {
			dprint(FD_IO, "io_u %p, failed getting offset\n", io_u);
			return 1;
		}

		/* [한국어] ④-2 블록 크기: bsrange 선형/bssplit CDF/고정 bs 분기. */
		io_u->buflen = get_next_buflen(td, io_u, is_random);
		if (!io_u->buflen) {
			dprint(FD_IO, "io_u %p, failed getting buflen\n", io_u);
			return 1;
		}
	}
	/* [한국어] offset 백업 — ZBD adjust 가 io_u->offset 을 변경할 수 있어
	 * 원본(랜덤맵 세트 기준) 을 따로 보관. */
	offset = io_u->offset;

	/* [한국어] ⑤ ZBD: write pointer 위치에 맞춰 offset/buflen 재정렬. 존이 모두
	 * 닫혔으면 io_u_eof → 이번 오프셋으로는 I/O 불가 → 실패 전파(다음 파일 시도). */
	if (td->o.zone_mode == ZONE_MODE_ZBD) {
		ret = zbd_adjust_block(td, io_u);
		if (ret == io_u_eof) {
			dprint(FD_IO, "zbd_adjust_block() returned io_u_eof\n");
			return 1;
		}
	}

	/* [한국어] ⑥ Data Placement(NVMe FDP/Streams/Placement ID): io_u 에 dtype/dspec
	 * 메타 채움. 엔진(nvme passthru)이 제출 시 명령 필드에 삽입. */
	if (td->o.dp_type != FIO_DP_NONE)
		dp_fill_dspec_data(td, io_u);

	/* [한국어] 최종 안전가드 — ZBD adjust 후에도 offset+buflen 이 파일 크기를
	 * 넘으면 버그. 방어적 체크. */
	if (io_u->offset + io_u->buflen > io_u->file->real_file_size) {
		dprint(FD_IO, "io_u %p, off=0x%llx + len=0x%llx exceeds file size=0x%llx\n",
			io_u,
			(unsigned long long) io_u->offset, io_u->buflen,
			(unsigned long long) io_u->file->real_file_size);
		return 1;
	}

	/*
	 * mark entry before potentially trimming io_u
	 */
	/* [한국어] ⑦ axmap 마킹: norandommap=0 + td_random + 랜덤맵 활성 + 다중범위 아님.
	 * 반환값이 원 buflen 보다 작으면 이미 점유된 블록에 걸렸다는 뜻 → buflen 축소
	 * 반영(이 io_u 는 축소된 영역만 커버). 다음 io_u 는 다른 블록으로. */
	if (!multi_range_trim(td, io_u) && td_random(td) && file_randommap(td, io_u->file))
		io_u->buflen = mark_random_map(td, io_u, offset, io_u->buflen);

out:
	if (!multi_range_trim(td, io_u))
		dprint_io_u(io_u, "fill");
	/* [한국어] verify_offset 은 verify 경로가 이 io_u 의 원본 오프셋을 재현할 때
	 * 사용 — rand 재추첨이나 ZBD adjust 로 offset 이 변해도 verify 가 원본 기준
	 * 패턴을 복원. */
	io_u->verify_offset = io_u->offset;
	/* [한국어] zone_bytes 는 STRIDED zone 모드의 현재 존 사용량 추적 — setup_strided_
	 * zone_mode 에서 zone_size 도달 판정에 사용. ZBD 모드에선 zbd.c 가 별도 관리. */
	td->zone_bytes += io_u->buflen;
	return 0;
}

/* [한국어]
 * __io_u_mark_map - 버킷 히스토그램 공용 인크리멘터 (7 버킷).
 *
 * @map: 대상 배열(ts.io_u_submit 또는 io_u_complete, 7 원소).
 * @nr: 이번 호출에 들어온 I/O 수.
 *
 * 버킷 경계: {0} {1..4} {5..8} {9..16} {17..32} {33..64} {65+}.
 * nr=0 도 맵에 기록 — "getevents 로 0건 반환"의 빈도 추적용.
 * fio_fallthrough: C fallthrough attribute (GCC warning 억제).
 *
 * 호출 체인: io_u_mark_submit/io_u_mark_complete → [이 함수].
 */
static void __io_u_mark_map(uint64_t *map, unsigned int nr)
{
	int idx = 0;

	/* [한국어] 7 버킷 (멱승 로그 스케일 근사): 0 | 1-4 | 5-8 | 9-16 | 17-32 | 33-64 | 65+
	 * nr 값에 따라 idx 결정 후 아래에서 ++. switch/case range (GNU 확장) 사용. */
	switch (nr) {
	default:
		/* [한국어] 65+ 대형 배치 — 마지막 버킷. */
		idx = 6;
		break;
	case 33 ... 64:
		idx = 5;
		break;
	case 17 ... 32:
		idx = 4;
		break;
	case 9 ... 16:
		idx = 3;
		break;
	case 5 ... 8:
		idx = 2;
		break;
	case 1 ... 4:
		idx = 1;
		/* [한국어] fio_fallthrough: GCC fallthrough attribute 로 의도적 관통.
		 * idx=1 세트 후 case 0 으로 관통해 break 하지만 추가 로직 실행 안 함 —
		 * "1..4 와 0 은 같은 처리 스킴" 표현(배열 인덱스만 다름). */
		fio_fallthrough;
	case 0:
		/* [한국어] nr=0 (getevents 가 0건 반환) 도 기록 — 폴링 실패 빈도 추적. */
		break;
	}

	/* [한국어] 해당 버킷 카운터 증가 — 락 없음(잡 스레드 단독 소유 통계). */
	map[idx]++;
}

/* [한국어]
 * io_u_mark_submit - 제출(submit) 배치 크기 히스토그램 기록.
 *
 * @td: 잡 스레드. td->ts.io_u_submit 배열 + total_submit 카운터.
 * @nr: 이번 submit 에 들어간 I/O 수.
 *
 * 호출 체인:
 *   td_io_commit (ioengines.c) → [이 함수]
 *   동기 엔진의 td_io_queue → [이 함수] (nr=1)
 */
void io_u_mark_submit(struct thread_data *td, unsigned int nr)
{
	__io_u_mark_map(td->ts.io_u_submit, nr);
	td->ts.total_submit++;
}

/* [한국어]
 * io_u_mark_complete - 완료(complete) 배치 크기 히스토그램 기록.
 *
 * @td: 잡 스레드.
 * @nr: 이번 getevents 로 회수된 I/O 수.
 *
 * 호출 체인:
 *   ioengines.c 의 완료 경로 → [이 함수]
 */
void io_u_mark_complete(struct thread_data *td, unsigned int nr)
{
	__io_u_mark_map(td->ts.io_u_complete, nr);
	td->ts.total_complete++;
}

/* [한국어]
 * io_u_mark_depth - 현재 큐 깊이(cur_depth) 히스토그램에 nr 가산.
 *
 * @td: 잡 스레드.
 * @nr: 이번 배치의 I/O 수(버킷 카운터에 곱해 가산).
 *
 * 버킷(멱승 기반): {1} {2..3} {4..7} {8..15} {16..31} {32..63} {64+}.
 * cur_depth 기반 버킷 선택 후 ts.io_u_map[idx] += nr.
 *
 * 호출 체인: ioengines.c 엔진 별 commit 후 → [이 함수].
 */
void io_u_mark_depth(struct thread_data *td, unsigned int nr)
{
	int idx = 0;

	/* [한국어] 7 버킷 (2^n 경계): 1 | 2-3 | 4-7 | 8-15 | 16-31 | 32-63 | 64+
	 * cur_depth 기준 버킷 선택 후 += nr — nr 은 이번 배치의 io_u 수. */
	switch (td->cur_depth) {
	default:
		/* [한국어] 64 이상 — 큰 큐 깊이. */
		idx = 6;
		break;
	case 32 ... 63:
		idx = 5;
		break;
	case 16 ... 31:
		idx = 4;
		break;
	case 8 ... 15:
		idx = 3;
		break;
	case 4 ... 7:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		/* [한국어] fallthrough: cur_depth=1 과 [2,3] 이 같은 break 공유. */
		fio_fallthrough;
	case 1:
		/* [한국어] cur_depth=1 (깊이 1 동기 제출) — idx=0. */
		break;
	}

	/* [한국어] io_u_map 에 nr 누적 — __io_u_mark_map 과 달리 "개수 가중치" 가산. */
	td->ts.io_u_map[idx] += nr;
}

/* [한국어]
 * io_u_mark_lat_nsec - 나노초 레이턴시 히스토그램 버킷 증가 (< 1000ns 전용).
 *
 * @td: 잡 스레드. td->ts.io_u_lat_n[10] 배열에 누적.
 * @nsec: 레이턴시(0~999ns). 1000ns 이상은 assert 실패 → 상위 io_u_mark_latency 가 분기.
 *
 * 버킷: {0..1} {2..3} {4..9} {10..19} {20..49} {50..99} {100..249} {250..499}
 *       {500..749} {750..999} (10 버킷).
 *
 * 호출 체인: io_u_mark_latency(nsec<1000) → [이 함수].
 */
static void io_u_mark_lat_nsec(struct thread_data *td, unsigned long long nsec)
{
	int idx = 0;

	assert(nsec < 1000);

	switch (nsec) {
	case 750 ... 999:
		idx = 9;
		break;
	case 500 ... 749:
		idx = 8;
		break;
	case 250 ... 499:
		idx = 7;
		break;
	case 100 ... 249:
		idx = 6;
		break;
	case 50 ... 99:
		idx = 5;
		break;
	case 20 ... 49:
		idx = 4;
		break;
	case 10 ... 19:
		idx = 3;
		break;
	case 4 ... 9:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 0 ... 1:
		break;
	}

	assert(idx < FIO_IO_U_LAT_N_NR);
	td->ts.io_u_lat_n[idx]++;
}

/* [한국어]
 * io_u_mark_lat_usec - 마이크로초 레이턴시 히스토그램 (1us~999us).
 *
 * @td: 잡 스레드. ts.io_u_lat_u[10] 배열.
 * @usec: 레이턴시(1..999).
 *
 * 호출 체인: io_u_mark_latency(1000 <= nsec < 1000000) → [이 함수] (nsec/1000 전달).
 */
static void io_u_mark_lat_usec(struct thread_data *td, unsigned long long usec)
{
	int idx = 0;

	assert(usec < 1000 && usec >= 1);

	switch (usec) {
	case 750 ... 999:
		idx = 9;
		break;
	case 500 ... 749:
		idx = 8;
		break;
	case 250 ... 499:
		idx = 7;
		break;
	case 100 ... 249:
		idx = 6;
		break;
	case 50 ... 99:
		idx = 5;
		break;
	case 20 ... 49:
		idx = 4;
		break;
	case 10 ... 19:
		idx = 3;
		break;
	case 4 ... 9:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 0 ... 1:
		break;
	}

	assert(idx < FIO_IO_U_LAT_U_NR);
	td->ts.io_u_lat_u[idx]++;
}

/* [한국어]
 * io_u_mark_lat_msec - 밀리초 레이턴시 히스토그램 (1ms 이상 전 구간).
 *
 * @td: 잡 스레드. ts.io_u_lat_m[12] 배열.
 * @msec: 레이턴시(ms, 1 이상).
 *
 * 버킷: {0..1}..{1000..1999} {2000+} (12 버킷). default 분기가 2000+ 처리.
 * 호출 체인: io_u_mark_latency(nsec >= 1000000) → [이 함수] (nsec/1000000 전달).
 */
static void io_u_mark_lat_msec(struct thread_data *td, unsigned long long msec)
{
	int idx = 0;

	assert(msec >= 1);

	switch (msec) {
	default:
		idx = 11;	/* [한국어] 2000ms 이상 */
		break;
	case 1000 ... 1999:
		idx = 10;
		break;
	case 750 ... 999:
		idx = 9;
		break;
	case 500 ... 749:
		idx = 8;
		break;
	case 250 ... 499:
		idx = 7;
		break;
	case 100 ... 249:
		idx = 6;
		break;
	case 50 ... 99:
		idx = 5;
		break;
	case 20 ... 49:
		idx = 4;
		break;
	case 10 ... 19:
		idx = 3;
		break;
	case 4 ... 9:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 0 ... 1:
		break;
	}

	assert(idx < FIO_IO_U_LAT_M_NR);
	td->ts.io_u_lat_m[idx]++;
}

/* [한국어]
 * io_u_mark_latency - 단위 자동 분기 (ns/us/ms) 레이턴시 히스토그램 갱신.
 *
 * @td: 잡 스레드.
 * @nsec: 완료 레이턴시(나노초).
 *
 * 3단계 분기:
 *   nsec < 1000           → io_u_mark_lat_nsec (ns 버킷)
 *   1000 <= nsec < 10^6   → io_u_mark_lat_usec(nsec/1000)
 *   10^6 <= nsec          → io_u_mark_lat_msec(nsec/10^6)
 *
 * 호출 체인: account_io_completion (disable_clat=0) → [이 함수].
 */
static void io_u_mark_latency(struct thread_data *td, unsigned long long nsec)
{
	/* [한국어] 3 자리수 스케일 분기 — 각 스케일마다 10개 버킷으로 해상도 유지.
	 * 단일 1000 버킷 대신 ns/us/ms 분리로 dynamic range 수억 배 커버. */
	if (nsec < 1000)
		/* [한국어] 0..999ns — NVMe/Optane 수준 초저지연. */
		io_u_mark_lat_nsec(td, nsec);
	else if (nsec < 1000000)
		/* [한국어] 1us..999us — SSD 일반 지연. nsec/1000 으로 usec 단위 정수화. */
		io_u_mark_lat_usec(td, nsec / 1000);
	else
		/* [한국어] 1ms 이상 — HDD 또는 네트워크 스토리지. nsec/1e6 으로 msec 정수화. */
		io_u_mark_lat_msec(td, nsec / 1000000);
}

/* [한국어]
 * __get_next_fileno_rand - 파일 번호 랜덤 선택 (file_service_type 에 따라 분포 분기).
 *
 * @td: 잡 스레드.
 * @return: td->files[] 배열 인덱스 (0..nr_files-1).
 *
 * 분기:
 *   FSERVICE_RANDOM: Tausworthe 균등.
 *   FSERVICE_ZIPF/PARETO: td->next_file_zipf 상태 (zipf/pareto_next). FIO_FSERVICE_SHIFT
 *     로 과생성 정밀도 내림 조정.
 *   FSERVICE_GAUSS: td->next_file_gauss.
 *
 * 호출 체인: get_next_file_rand → [이 함수].
 */
static unsigned int __get_next_fileno_rand(struct thread_data *td)
{
	unsigned long fileno;

	/* [한국어] 균등 분포 — next_file_state Tausworthe 난수에서 [0, nr_files) 사상.
	 * 다른 분포들과 달리 여기선 shift 없이 바로 반환. */
	if (td->o.file_service_type == FIO_FSERVICE_RANDOM) {
		uint64_t frand_max = rand_max(&td->next_file_state);
		unsigned long r;

		r = __rand(&td->next_file_state);
		return (unsigned int) ((double) td->o.nr_files
				* (r / (frand_max + 1.0)));
	}

	/* [한국어] 비균등 분포들 — 상태 머신의 next 함수 호출. FIO_FSERVICE_SHIFT 는
	 * "over-provision bits" — 분포 정밀도를 위해 (nr_files << SHIFT) 범위로 생성
	 * 후 shift 로 실제 인덱스 추출(소수점 정밀도 회수 효과). */
	if (td->o.file_service_type == FIO_FSERVICE_ZIPF)
		fileno = zipf_next(&td->next_file_zipf);
	else if (td->o.file_service_type == FIO_FSERVICE_PARETO)
		fileno = pareto_next(&td->next_file_zipf);
	else if (td->o.file_service_type == FIO_FSERVICE_GAUSS)
		fileno = gauss_next(&td->next_file_gauss);
	else {
		log_err("fio: bad file service type: %d\n", td->o.file_service_type);
		assert(0);
		return 0;
	}

	/* [한국어] over-provision 한 bits 제거 → [0, nr_files) 인덱스 도출. */
	return fileno >> FIO_FSERVICE_SHIFT;
}

/*
 * Get next file to service by choosing one at random
 */
/* [한국어]
 * get_next_file_rand - 랜덤 분포로 파일 선택 + 필요 시 open(open_files 상한 내).
 *
 * @td: 잡 스레드.
 * @goodf: 이 플래그가 세트된 파일만 선택(예: FIO_FILE_open). 0이면 조건 없음.
 * @badf: 이 플래그가 세트된 파일은 제외(예: FIO_FILE_closing).
 * @return: fio_file* (성공), ERR_PTR(-EBUSY) (open_files 상한 초과).
 *
 * 루프: __get_next_fileno_rand 로 번호 추첨 → done 상태면 continue →
 * 닫혀 있으면 td_io_open_file 시도 → 플래그 조건 충족 시 반환. 조건 미충족이면
 * 방금 연 파일 닫고 다시 추첨. 무한 루프 가능성 있지만 상위가 nr_done_files
 * 체크로 보호.
 *
 * 호출 체인: __get_next_file (FSERVICE_RANDOM/ZIPF/PARETO/GAUSS) → [이 함수].
 */
static struct fio_file *get_next_file_rand(struct thread_data *td,
					   enum fio_file_flags goodf,
					   enum fio_file_flags badf)
{
	struct fio_file *f;
	int fno;

	do {
		/* [한국어] 이 반복에서 새로 open 한 파일인지 마커 — 플래그 불일치시
		 * close 해야 할지 판단. 기존에 열려 있던 파일은 close 안 함. */
		int opened = 0;

		/* [한국어] 분포별 파일 인덱스 추첨. */
		fno = __get_next_fileno_rand(td);

		f = td->files[fno];
		/* [한국어] 이미 완료(I/O 소진)된 파일 skip — 루프 재추첨. */
		if (fio_file_done(f))
			continue;

		/* [한국어] 파일 아직 open 전이면 open 시도. */
		if (!fio_file_open(f)) {
			int err;

			/* [한국어] open_files 상한 초과 — 현재는 못 연다는 의미. -EBUSY 로
			 * 상위에 힌트(다른 파일 close 후 재시도). */
			if (td->nr_open_files >= td->o.open_files)
				return ERR_PTR(-EBUSY);

			/* [한국어] td_io_open_file: 엔진의 .open_file 콜백 호출. 실패 시
			 * 다음 파일 추첨으로 넘어감. */
			err = td_io_open_file(td, f);
			if (err)
				continue;
			opened = 1;
		}

		/* [한국어] 플래그 조건 평가 — goodf 가 필수 비트 세트, badf 는 제외할 비트. */
		if ((!goodf || (f->flags & goodf)) && !(f->flags & badf)) {
			dprint(FD_FILE, "get_next_file_rand: %p\n", f);
			return f;
		}
		/* [한국어] 조건 불일치 + 방금 열린 파일이면 원상복구하기 위해 close.
		 * 기존에 열려 있던 파일은 그대로 두고 다음 추첨 — 일관된 파일 상태 유지. */
		if (opened)
			td_io_close_file(td, f);
	} while (1);
}

/*
 * Get next file to service by doing round robin between all available ones
 */
/* [한국어]
 * get_next_file_rr - 라운드 로빈으로 파일 선택 + 필요 시 open.
 *
 * @td: 잡 스레드. td->next_file 인덱스가 RR 커서.
 * @goodf/badf: get_next_file_rand 와 동일.
 * @return: fio_file* 또는 NULL(한 바퀴 돌아도 후보 없음). open_files 상한이면 ERR_PTR.
 *
 * 호출 체인: __get_next_file (FSERVICE_RR/SEQ) → [이 함수].
 * SEQ 모드에서도 RR 로 첫 후보 선택 후 file_service_file 로 고정 사용.
 */
static struct fio_file *get_next_file_rr(struct thread_data *td, int goodf,
					 int badf)
{
	/* [한국어] 루프 시작 시점의 next_file 저장 — 한 바퀴 순회 종료 감지 (모든 파일
	 * 검사 완료 시 탈출). */
	unsigned int old_next_file = td->next_file;
	struct fio_file *f;

	do {
		int opened = 0;

		f = td->files[td->next_file];

		/* [한국어] RR 커서 전진 + nr_files 모듈로 wrap — 다음 호출에서 그 다음 파일 검사. */
		td->next_file++;
		if (td->next_file >= td->o.nr_files)
			td->next_file = 0;

		dprint(FD_FILE, "trying file %s %x\n", f->file_name, f->flags);
		/* [한국어] 완료 파일 skip — f=NULL 세트해 루프 종료 후 리턴값 판정에 사용. */
		if (fio_file_done(f)) {
			f = NULL;
			continue;
		}

		/* [한국어] 파일 닫혀 있으면 open. open_files 상한 검사 동일. */
		if (!fio_file_open(f)) {
			int err;

			if (td->nr_open_files >= td->o.open_files)
				return ERR_PTR(-EBUSY);

			err = td_io_open_file(td, f);
			if (err) {
				dprint(FD_FILE, "error %d on open of %s\n",
					err, f->file_name);
				f = NULL;
				continue;
			}
			opened = 1;
		}

		dprint(FD_FILE, "goodf=%x, badf=%x, ff=%x\n", goodf, badf,
								f->flags);
		/* [한국어] 플래그 조건 충족 → break(탈출, f 반환). */
		if ((!goodf || (f->flags & goodf)) && !(f->flags & badf))
			break;

		/* [한국어] 불일치 + 방금 열렸으면 close. */
		if (opened)
			td_io_close_file(td, f);

		/* [한국어] f=NULL 로 다시 리셋 — "이 파일은 선택 불가" 마커. 반복 지속. */
		f = NULL;
	} while (td->next_file != old_next_file);
	/* [한국어] 한 바퀴 순회 완료 — 후보 없으면 f=NULL 반환. */

	dprint(FD_FILE, "get_next_file_rr: %p\n", f);
	return f;
}

/* [한국어]
 * __get_next_file - 파일 서비스 정책(SEQ/RR/RANDOM/분포) 디스패처.
 *
 * @td: 잡 스레드.
 * @return: 선택된 fio_file*, NULL(모든 파일 완료), ERR_PTR(상한 초과).
 *
 * 우선순위:
 *  1) 현재 file_service_file 이 살아있고(열림+닫힘 중 아님):
 *     - SEQ 모드 → 그 파일 계속 사용.
 *     - file_service_left 남아 있음 → 카운터 차감 후 그 파일 사용.
 *  2) 새로 선택: RR/SEQ → get_next_file_rr, 그 외 → get_next_file_rand.
 *  3) 새 파일 확정: file_service_file 갱신 + file_service_left = file_service_nr - 1
 *     (한 파일에 연속 N개 I/O 후 전환).
 *
 * 실행 컨텍스트: 잡 스레드.
 * 호출 체인: get_next_file → [이 함수].
 */
static struct fio_file *__get_next_file(struct thread_data *td)
{
	struct fio_file *f;

	assert(td->o.nr_files <= td->files_index);

	/* [한국어] nr_done_files: 영구 완료 파일 수. nr_files 도달하면 잡 종료 신호. */
	if (td->nr_done_files >= td->o.nr_files) {
		dprint(FD_FILE, "get_next_file: nr_open=%d, nr_done=%d,"
				" nr_files=%d\n", td->nr_open_files,
						  td->nr_done_files,
						  td->o.nr_files);
		return NULL;
	}

	/* [한국어] 이전 get 에서 선정해 둔 file_service_file 이 여전히 사용 가능하면 재사용. */
	f = td->file_service_file;
	if (f && fio_file_open(f) && !fio_file_closing(f)) {
		/* [한국어] SEQ 모드는 이 파일이 완전히 끝날 때까지 계속 사용 — 전환 없음. */
		if (td->o.file_service_type == FIO_FSERVICE_SEQ)
			goto out;
		/* [한국어] file_service_nr 옵션은 "한 파일당 연속 I/O 수" — left 카운터
		 * 감소시키며 0 도달하면 아래에서 새 파일 선정. */
		if (td->file_service_left) {
			td->file_service_left--;
			goto out;
		}
	}

	/* [한국어] 새 파일 선정: RR/SEQ 는 라운드 로빈, 나머지(RANDOM/ZIPF/PARETO/GAUSS) 는 분포. */
	if (td->o.file_service_type == FIO_FSERVICE_RR ||
	    td->o.file_service_type == FIO_FSERVICE_SEQ)
		f = get_next_file_rr(td, FIO_FILE_open, FIO_FILE_closing);
	else
		f = get_next_file_rand(td, FIO_FILE_open, FIO_FILE_closing);

	if (IS_ERR(f))
		return f;

	/* [한국어] 현재 서비스 파일로 갱신 — 다음 get 때 위 "재사용" 분기 활용. */
	td->file_service_file = f;
	/* [한국어] file_service_nr - 1: 이미 지금 1회 사용으로 카운트되므로 하나 덜. */
	td->file_service_left = td->file_service_nr - 1;
out:
	if (f)
		dprint(FD_FILE, "get_next_file: %p [%s]\n", f, f->file_name);
	else
		dprint(FD_FILE, "get_next_file: NULL\n");
	return f;
}

/* [한국어]
 * get_next_file - 파일 선택 래퍼(확장 여지를 위해 분리).
 *
 * @td: 잡 스레드.
 * @return: __get_next_file 결과 그대로.
 *
 * 호출 체인: set_io_u_file → [이 함수] → __get_next_file.
 */
static struct fio_file *get_next_file(struct thread_data *td)
{
	return __get_next_file(td);
}

/* [한국어]
 * set_io_u_file - io_u 에 파일을 선택·바인딩하고 fill_io_u 로 I/O 정보 채움.
 *
 * @td: 잡 스레드.
 * @io_u: 대상. 성공 시 io_u->file + offset/buflen/ddir 세트.
 * @return: 0=성공, 양수=파일 소진 실패(호출자는 ERR_PTR 등 변환),
 *          음수=에러 포인터(-EBUSY 등 ERR_PTR).
 *
 * 루프 구조 — fill_io_u 실패 시 파일을 정리하고 다음 파일 시도:
 *   1. get_next_file 로 파일 획득 (IS_ERR/NULL 처리).
 *   2. io_u->file = f; get_file(f) 로 참조 카운트++.
 *   3. fill_io_u 성공이면 break.
 *   4. 실패 시: zbd_put_io_u + put_file_log + td_io_close_file + io_u->file=NULL.
 *   5. ddir==TIMEOUT 이면 즉시 종료.
 *   6. 비균일 분포이면 fio_file_reset 으로 재사용, 아니면 fio_file_set_done +
 *      nr_done_files++ 로 영구 완료.
 *   7. 다음 루프.
 *
 * 호출 체인: get_io_u → [이 함수] → get_next_file + fill_io_u.
 */
static long set_io_u_file(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f;

	do {
		/* [한국어] 파일 서비스 정책(SEQ/RR/RANDOM/분포)에 따라 후보 파일 선택.
		 * NULL = 모든 파일 완료, ERR_PTR = open_files 상한 초과 등 에러. */
		f = get_next_file(td);
		if (IS_ERR_OR_NULL(f))
			return PTR_ERR(f);

		/* [한국어] io_u 와 파일을 양방향 바인딩 + num_ref 증가. put_io_u 에서
		 * put_file_log 로 참조 해제 대칭. */
		io_u->file = f;
		get_file(f);

		/* [한국어] fill_io_u 성공 시 루프 탈출 — offset/buflen/ddir 세팅 완료. */
		if (!fill_io_u(td, io_u))
			break;

		/* [한국어] 실패 경로 정리: zbd_put_io_u 로 존 참조 해제, put_file_log +
		 * td_io_close_file 로 파일 닫음. io_u->file=NULL 마커. */
		zbd_put_io_u(td, io_u);

		put_file_log(td, f);
		td_io_close_file(td, f);
		io_u->file = NULL;

		/* [한국어] set_rw_ddir → rate_ddir 이 DDIR_TIMEOUT 반환했을 수 있음 —
		 * 잡 종료가 의도이므로 여기서도 즉시 실패 전파. */
		if (io_u->ddir == DDIR_TIMEOUT)
			return 1;

		/* [한국어] Zipf/Pareto/Gauss(NONUNIFORM): 파일 소진 개념이 아닌 "상태
		 * 재초기화" 전략 — fio_file_reset 후 다음 루프에서 이 파일 재시도.
		 * 균등/SEQ/RR 인 경우 "이 파일 완전 완료" → set_done + 카운터 증가로 다음
		 * 파일로 넘어감. */
		if (td->o.file_service_type & __FIO_FSERVICE_NONUNIFORM)
			fio_file_reset(td, f);
		else {
			fio_file_set_done(f);
			td->nr_done_files++;
			dprint(FD_FILE, "%s: is done (%d of %d)\n", f->file_name,
					td->nr_done_files, td->o.nr_files);
		}
	} while (1);

	return 0;
}

/* [한국어]
 * lat_fatal - max_latency 초과 시 치명적 에러(ETIMEDOUT) 마킹.
 *
 * @td: 잡 스레드.
 * @io_u: 문제의 io_u (파일/오프셋/길이/ddir 로그 출력).
 * @icd: 배치 완료 컨텍스트. icd->error = ETIMEDOUT 로 마크.
 * @tnsec: 실측 레이턴시.
 * @max_nsec: 허용 상한.
 *
 * td->error 가 아직 세트 안 된 경우에만 log_err 출력 (첫 발생 1회).
 * td_verror 로 에러 등록 → 상위 io_completed/io_u_*_complete 경로가 -1 반환 →
 * 잡 종료. icd->error 설정으로 배치 내 후속 io_u 도 처리 후 종료 판단.
 *
 * 호출 체인: account_io_completion (max_latency 초과 또는 lat_target_failed
 *             최종 실패) → [이 함수].
 */
static void lat_fatal(struct thread_data *td, struct io_u *io_u, struct io_completion_data *icd,
		      unsigned long long tnsec, unsigned long long max_nsec)
{
	if (!td->error) {
		log_err("fio: latency of %llu nsec exceeds specified max (%llu nsec): %s %s %llu %llu\n",
					tnsec, max_nsec,
					io_u->file->file_name,
					io_ddir_name(io_u->ddir),
					io_u->offset, io_u->buflen);
	}
	td_verror(td, ETIMEDOUT, "max latency exceeded");
	icd->error = ETIMEDOUT;
}

/* [한국어]
 * lat_new_cycle - latency_target 이진 탐색의 한 사이클 재시작.
 *
 * @td: 잡 스레드. latency_ts=기준 시각, latency_ios=기준 I/O 수, latency_failed=0.
 *
 * 호출 체인:
 *   __lat_target_failed (QD 낮춤 후) → [이 함수]
 *   lat_target_success → [이 함수]
 */
static void lat_new_cycle(struct thread_data *td)
{
	/* [한국어] 현재 시각 기록 — latency_window 경과 판정 기준점. */
	fio_gettime(&td->latency_ts, NULL);
	/* [한국어] 현재 총 I/O 블록 수 기록 — 이번 사이클 시작 시점. */
	td->latency_ios = ddir_rw_sum(td->io_blocks);
	/* [한국어] 이번 사이클의 타겟 초과 카운터 리셋. */
	td->latency_failed = 0;
}

/*
 * We had an IO outside the latency target. Reduce the queue depth. If we
 * are at QD=1, then it's time to give up.
 */
/* [한국어]
 * __lat_target_failed - latency_target 위반 시 QD 이진 탐색으로 낮춤.
 *
 * @td: 잡 스레드. latency_qd/latency_qd_low/latency_qd_high 삼자로 이진 탐색.
 * @return: true=QD=1 에서도 실패(잡 종료 신호), false=QD 낮춤 성공(계속).
 *
 * 알고리즘:
 *   - QD=1 이면 이미 최소 → 실패 반환(lat_fatal 트리거).
 *   - qd_high = 현재 qd (이 위는 확실히 실패로 표시).
 *   - qd = (qd + qd_low) / 2 (이진 하강).
 *   - io_u_quiesce 로 상위 QD 시절 in-flight 드레인 → ramp-down 폭풍 방지.
 *   - lat_new_cycle 로 기준점 리셋.
 *
 * 호출 체인:
 *   lat_target_failed (percentile=100%) → [이 함수]
 *   lat_target_check (percentile 미만 성공률) → [이 함수]
 */
static bool __lat_target_failed(struct thread_data *td)
{
	/* [한국어] latency_qd=1 에서도 latency_target 을 만족 못 함 → 디스크가 본질적으로
	 * 목표 latency 를 충족 불가 → lat_fatal 트리거 필요(반환 true). */
	if (td->latency_qd == 1)
		return true;

	/* [한국어] 현재 qd 는 "실패로 확인된 값" → qd_high 로 갱신해 이 위는 더 이상 탐색 안 함. */
	td->latency_qd_high = td->latency_qd;

	/* [한국어] qd==qd_low 인 경계 상태에서 추가 하강이 필요하면 low 를 한 단 더
	 * 내려 탐색 여지 확보 — 그렇지 않으면 이진 탐색이 동일 값에 수렴해 버림. */
	if (td->latency_qd == td->latency_qd_low)
		td->latency_qd_low--;

	/* [한국어] 이진 하강: 새 qd = (현재 + low) / 2. 예) 32 → 16 → 8 ... */
	td->latency_qd = (td->latency_qd + td->latency_qd_low) / 2;
	/* [한국어] 안정 카운터 리셋 — 이번 사이클은 실패이므로 연속 성공 카운트 재시작. */
	td->latency_stable_count = 0;

	dprint(FD_RATE, "Ramped down: %d %d %d\n", td->latency_qd_low, td->latency_qd, td->latency_qd_high);

	/*
	 * When we ramp QD down, quiesce existing IO to prevent
	 * a storm of ramp downs due to pending higher depth.
	 */
	/* [한국어] 이전 QD 시절의 in-flight 가 남아 있으면 그 완료 latency 는 "하강 후
	 * 발행" 이 아님에도 새 사이클 판단에 섞여 또 실패 판정을 유발해 폭주. quiesce
	 * 로 모두 비운 뒤 새 사이클 진입. */
	io_u_quiesce(td);
	lat_new_cycle(td);
	return false;
}

/* [한국어]
 * lat_target_failed - 즉시 실패 처리 (percentile=100%) vs 실패 카운트 누적만.
 *
 * @td: 잡 스레드.
 * @return: true=포기 확정, false=계속(카운트만 증가).
 *
 * percentile < 100 이면 latency_window 동안 실패 비율로 판단해야 하므로
 * 개별 초과만으로 즉시 포기하지 않음 → latency_failed++ 만.
 *
 * 호출 체인: account_io_completion → [이 함수].
 */
static bool lat_target_failed(struct thread_data *td)
{
	if (td->o.latency_percentile.u.f == 100.0)
		return __lat_target_failed(td);

	/* [한국어] 실패 횟수 증가 (사이클 끝에서 percentile로 판단) */
	td->latency_failed++;
	return false;
}

/* [한국어]
 * lat_target_init - latency_target 이진 탐색 초기화 (QD=1 부터).
 *
 * @td: 잡 스레드.
 *
 * 설정되어 있으면: latency_qd=1, latency_qd_high=iodepth, latency_qd_low=1,
 * latency_ts=현재 시각, latency_ios=현재 io_blocks 합.
 * 미설정이면: latency_qd=iodepth (전체 사용).
 *
 * 호출 체인:
 *   backend.c init_io_u → [이 함수] (잡 시작 시)
 *   lat_target_reset → [이 함수] (재초기화).
 */
void lat_target_init(struct thread_data *td)
{
	td->latency_end_run = 0;

	if (td->o.latency_target) {
		dprint(FD_RATE, "Latency target=%llu\n", td->o.latency_target);
		fio_gettime(&td->latency_ts, NULL);
		/* [한국어] QD=1부터 시작 */
		td->latency_qd = 1;
		/* [한국어] 상한은 사용자 지정 iodepth */
		td->latency_qd_high = td->o.iodepth;
		td->latency_qd_low = 1;
		td->latency_ios = ddir_rw_sum(td->io_blocks);
	} else
		/* [한국어] 타겟 없으면 전체 iodepth 사용 */
		td->latency_qd = td->o.iodepth;
}

/* [한국어]
 * lat_target_reset - latency_end_run 아니면 init 재호출(파일 루프 경계 등).
 *
 * @td: 잡 스레드.
 *
 * latency_end_run=1 이면 최종 측정 사이클 진행 중이므로 건드리지 않음.
 */
void lat_target_reset(struct thread_data *td)
{
	if (!td->latency_end_run)
		lat_target_init(td);
}

/* [한국어]
 * lat_target_success - latency_target 만족 시 QD 이진 탐색으로 상향.
 *
 * @td: 잡 스레드.
 *
 * 알고리즘:
 *   qd_low = 현재 qd.
 *   qd_high 가 iodepth(미탐색)면 qd *= 2, 아니면 (qd + qd_high) / 2.
 *   qd 상한 iodepth 클램프.
 *   qd+1 == qd_high 인 안정 상태에서 3회 연속 성공 시 qd_high 를 1 올려
 *   탐색 해상도 보존 (heuristic).
 *   qd 변화 없음 + latency_end_run=0 → "최적 QD 발견" → 최종 측정 사이클 진입:
 *     io_u_quiesce + reset_all_stats + reset_io_stats. latency_end_run=1.
 *   이미 end_run 이었으면 td->done=1 로 잡 종료.
 *
 * 호출 체인: lat_target_check (percentile 만족) → [이 함수].
 */
static void lat_target_success(struct thread_data *td)
{
	const unsigned int qd = td->latency_qd;
	struct thread_options *o = &td->o;

	/* [한국어] 이번 qd 는 "성공 확인" → qd_low 로 갱신(이 밑은 더 이상 탐색 안 함). */
	td->latency_qd_low = td->latency_qd;

	if (td->latency_qd + 1 == td->latency_qd_high) {
		/*
		 * latency_qd will not incease on lat_target_success(), so
		 * called stable. If we stick with this queue depth, the
		 * final latency is likely lower than latency_target. Fix
		 * this by increasing latency_qd_high slowly. Use a naive
		 * heuristic here. If we get lat_target_success() 3 times
		 * in a row, increase latency_qd_high by 1.
		 */
		/* [한국어] qd+1 == qd_high 는 이진 탐색이 멈춘 경계 상태. 3회 연속
		 * 성공해도 qd 는 그대로인 "고정 상태" — 실제로는 qd+1 도 가능할 수
		 * 있으므로 qd_high 를 1 올려 탐색 공간 재개. 휴리스틱 단순 카운터. */
		if (++td->latency_stable_count >= 3) {
			td->latency_qd_high++;
			td->latency_stable_count = 0;
		}
	}

	/*
	 * If we haven't failed yet, we double up to a failing value instead
	 * of bisecting from highest possible queue depth. If we have set
	 * a limit other than td->o.iodepth, bisect between that.
	 */
	/* [한국어] qd_high == iodepth 는 "아직 한 번도 실패 안 함" 상태 — 상한 탐색
	 * 이 안 끝났으므로 2배로 빠르게 상승. 이미 실패(qd_high < iodepth)면 이진
	 * 탐색으로 정밀 조정. */
	if (td->latency_qd_high != o->iodepth)
		td->latency_qd = (td->latency_qd + td->latency_qd_high) / 2;
	else
		td->latency_qd *= 2;

	/* [한국어] 사용자 지정 iodepth 를 넘지 않도록 상한 클램프. */
	if (td->latency_qd > o->iodepth)
		td->latency_qd = o->iodepth;

	dprint(FD_RATE, "Ramped up: %d %d %d\n", td->latency_qd_low, td->latency_qd, td->latency_qd_high);

	/*
	 * Same as last one, we are done. Let it run a latency cycle, so
	 * we get only the results from the targeted depth.
	 */
	/* [한국어] qd 가 이전 qd 와 동일 = 이진 탐색 수렴(더 이상 변할 곳 없음).
	 * latency_run=0 기본 모드는 여기서 최종 측정 사이클에 진입 또는 종료. */
	if (!o->latency_run && td->latency_qd == qd) {
		if (td->latency_end_run) {
			/* [한국어] 이미 최종 측정 사이클 중이었음 → 두 번째 수렴 = 확정 종료. */
			dprint(FD_RATE, "We are done\n");
			td->done = 1;
		} else {
			/* [한국어] 첫 수렴 — 최적 qd 확정. 기존 통계를 리셋하고 이 qd 만으로
			 * 최종 측정 윈도우 시작(순수한 타겟 QD 성능 보고서 생성). */
			dprint(FD_RATE, "Quiesce and final run\n");
			io_u_quiesce(td);
			td->latency_end_run = 1;
			reset_all_stats(td);
			reset_io_stats(td);
		}
	}

	lat_new_cycle(td);
}

/*
 * Check if we can bump the queue depth
 */
/* [한국어]
 * lat_target_check - latency_window 종료 시 성공률 판정 후 QD 조정.
 *
 * @td: 잡 스레드.
 *
 * utime_since_now(latency_ts) < latency_window → 조기 리턴.
 * 도달 시: 이번 윈도우에 발행된 I/O 수 = io_blocks 합 - latency_ios,
 *         성공률 = (ios - latency_failed) / ios * 100.
 * 성공률 >= latency_percentile → lat_target_success,
 * 미만 → __lat_target_failed.
 *
 * 호출 체인: backend.c do_io 루프 → [이 함수].
 */
void lat_target_check(struct thread_data *td)
{
	uint64_t usec_window;
	uint64_t ios;
	double success_ios;

	/* [한국어] latency_window 옵션(usec) 경과 여부 확인. 미경과면 아직 표본 부족 → 조기 리턴. */
	usec_window = utime_since_now(&td->latency_ts);
	if (usec_window < td->o.latency_window)
		return;

	/* [한국어] 이번 윈도우 발행 I/O 수 = 현재 누적 - latency_ios(윈도우 시작 시점 스냅샷).
	 * 성공률 = (ios - failed) / ios * 100%. */
	ios = ddir_rw_sum(td->io_blocks) - td->latency_ios;
	success_ios = (double) (ios - td->latency_failed) / (double) ios;
	success_ios *= 100.0;

	dprint(FD_RATE, "Success rate: %.2f%% (target %.2f%%)\n", success_ios, td->o.latency_percentile.u.f);

	/* [한국어] percentile 임계(기본 100%) 이상이면 이 qd 에서 목표 달성 → 상향.
	 * 미만이면 qd 를 낮춰 부하 감소. 이진 탐색 기본 스텝. */
	if (success_ios >= td->o.latency_percentile.u.f)
		lat_target_success(td);
	else
		__lat_target_failed(td);
}

/*
 * If latency target is enabled, we might be ramping up or down and not
 * using the full queue depth available.
 */
/* [한국어]
 * queue_full - freelist 고갈 또는 latency_target 가상 상한 도달 판정.
 *
 * @td: 잡 스레드(const).
 * @return: true=추가 io_u 할당 불가, false=여유 있음.
 *
 * 두 조건 중 하나:
 *  1) io_u_qempty(freelist): 모든 io_u in-flight 상태.
 *  2) latency_target 활성 + cur_depth >= latency_qd: QD 이진 탐색이 현재 허용한
 *     가상 상한 도달 (실제 iodepth 옵션보다 낮을 수 있음).
 *
 * 호출 체인: __get_io_u → [이 함수], backend.c do_io → [이 함수].
 */
bool queue_full(const struct thread_data *td)
{
	/* [한국어] io_u_qempty: freelist 헤드/테일 동일 검사 → O(1). 모든 io_u 가
	 * in_flight 상태임을 의미. */
	const int qempty = io_u_qempty(&td->io_u_freelist);

	if (qempty)
		return true;
	/* [한국어] latency_target 미설정이면 실제 물리 iodepth 까지 여유 있음 — 위
	 * qempty 가 유일한 가드. */
	if (!td->o.latency_target)
		return false;

	/* [한국어] latency_target 활성 시 cur_depth >= latency_qd 면 이진 탐색이 허용한
	 * 가상 상한 도달 — 실제 io_u 는 남아 있어도 지금은 발급 금지. */
	return td->cur_depth >= td->latency_qd;
}

/* [한국어]
 * __get_io_u - ★ 핵심 ★ io_u 생명주기 시작의 "풀에서 꺼내기" 단계.
 *
 * @td: 잡 스레드.
 * @return: IO_U_F_FREE 해제된 깨끗한 io_u, 또는 NULL(stop_io 또는 락/대기 포기).
 *
 * 상태 전이:
 *   [io_u_requeues] (우선)        [io_u_freelist]
 *         │                              │
 *         └──────────┬───────────────────┘
 *                    ▼
 *        [이 함수가 꺼냄] → io_u (FREE clear, BARRIER/TRIMMED/VER_LIST/NO_FILE_PUT clear,
 *                               cur_depth++, IN_CUR_DEPTH set, error=0, acct_ddir=-1)
 *                    ▼
 *            get_io_u의 후속 단계(check_get_trim/verify, set_io_u_file, fill_io_u, td_io_prep)
 *
 * 우선순위:
 *  1) td->io_u_requeues(재큐잉 큐): short I/O 재시도 또는 FIO_Q_BUSY 반환 재시도.
 *     이미 offset/buflen/file이 설정되어 있을 수 있으므로 → 아래 get_io_u가
 *     "io_u->file != NULL이면 fill 생략" 분기에서 이 상태를 활용.
 *  2) td->io_u_freelist(프리리스트): queue_full()이 false일 때만 꺼낸다.
 *     queue_full은 latency_target 모드에서는 td->latency_qd 상한을 적용.
 *  3) 둘 다 없으면:
 *     - async processing(verify 스레드 분리): pthread_cond_wait(&free_cond)로
 *       io_u 회수 대기. 깨어나면 다시 시도(goto again).
 *     - 그 외: NULL 반환 → get_io_u의 상위에서 td_io_getevents/io_u_quiesce 경로.
 *
 * 동기화: needs_lock 시 td->io_u_lock (pthread_mutex) 보호.
 *
 * 실행 컨텍스트: 잡 스레드(verify 스레드는 자체 td에서 동일 흐름).
 *
 * 호출 체인: get_io_u → [이 함수].
 *
 * 에러 경로: pthread_cond_wait 실패(신호/ENOMEM) 시 td->error=errno 설정 후
 * io_u=NULL 반환.
 */
struct io_u *__get_io_u(struct thread_data *td)
{
	const bool needs_lock = td_async_processing(td);
	struct io_u *io_u = NULL;

	/* [한국어] td->stop_io 는 runtime/runt 만료 등 외부에서 "이번 잡 더 이상 발행
	 * 금지" 신호. 새 io_u 뽑지 않고 NULL 로 상위에 종료 유도. */
	if (td->stop_io)
		return NULL;

	if (needs_lock)
		__td_io_u_lock(td);

again:
	/* [한국어] 1순위: io_u_requeues(재큐잉 리스트). FIO_Q_BUSY 재시도 또는
	 * short I/O 나머지 처리 등 "이어서 발행할 io_u" 가 여기 들어 있음.
	 * offset/buflen/file 보존된 상태 — set_io_u_file/fill_io_u 건너뜀. */
	if (!io_u_rempty(&td->io_u_requeues)) {
		io_u = io_u_rpop(&td->io_u_requeues);
		/* [한국어] resid 는 short I/O 재발행 시 "남은 바이트" 의미인데, 이번
		 * 재시도는 0 부터 새로 카운트해야 하므로 리셋. */
		io_u->resid = 0;
		/* [한국어] TD_FSYNCING 단계(잡 종료 전 최종 fsync)에서는 파일 레퍼런스
		 * 를 유지할 필요 없음 — fsync 는 fd 만 있으면 수행. put_file_log 로
		 * num_ref 감소 → 조기 close 스케줄. */
		if (io_u->file && td->runstate == TD_FSYNCING) {
			put_file_log(td, io_u->file);
			io_u->file = NULL;
		}
	} else if (!queue_full(td)) {
		/* [한국어] 2순위: freelist. queue_full 이 false 일 때만 — latency_target
		 * 이 활성이면 latency_qd 가상 상한 적용. */
		io_u = io_u_qpop(&td->io_u_freelist);

		/* [한국어] freelist 에서 꺼낸 io_u 는 "완전 초기화" 필요 — 이전 잡 사이클의
		 * 잔여 상태(file/buflen/resid/end_io) 가 남아 있을 수 있기 때문. */
		io_u->file = NULL;
		io_u->buflen = 0;
		io_u->resid = 0;
		io_u->end_io = NULL;
	}

	if (io_u) {
		/* [한국어] freelist/requeues 모두 FREE 비트 세트 상태로 저장되어 있어야
		 * 함 — 생명주기 반상 보장을 위한 불변. */
		assert(io_u->flags & IO_U_F_FREE);
		/* [한국어] "회수-후-재사용" 시점에 누적된 플래그를 일괄 클리어.
		 *  FREE: 더 이상 사용 가능 상태 아님.
		 *  NO_FILE_PUT: 이전 사용 시 세트된 특수 경로 마커.
		 *  TRIMMED: 이전에 TRIM 된 io_u (verify 연계).
		 *  BARRIER: 이전 WRITE 가 배리어였을 수 있음.
		 *  VER_LIST: verify_list 에서 가져왔던 상태. */
		io_u_clear(td, io_u, IO_U_F_FREE | IO_U_F_NO_FILE_PUT |
				 IO_U_F_TRIMMED | IO_U_F_BARRIER |
				 IO_U_F_VER_LIST);

		io_u->error = 0;
		io_u->acct_ddir = -1;
		/* [한국어] cur_depth 증가와 IN_CUR_DEPTH 세트는 반드시 쌍 — put/requeue
		 * 경로에서 이 비트 확인 후 cur_depth-- 수행. 자식 스레드는 부모 cur_depth
		 * 직접 조작 금지(TD_F_CHILD assert). */
		td->cur_depth++;
		assert(!(td->flags & TD_F_CHILD));
		io_u_set(td, io_u, IO_U_F_IN_CUR_DEPTH);
		io_u->ipo = NULL;
	} else if (td_async_processing(td)) {
		int ret;
		/*
		 * We ran out, wait for async verify threads to finish and
		 * return one
		 */
		/* [한국어] 둘 다 비었을 때: verify_async/offload 스레드만 남아 있을 수
		 * 있음 → free_cond 에 블로킹 대기. put_io_u/requeue 가 브로드캐스트하면
		 * 깨어나 again 으로 재시도. 단일 스레드 모드면 여기 진입 안 하고
		 * 그냥 NULL 반환(상위가 getevents 경로로 완료 수확). */
		assert(!(td->flags & TD_F_CHILD));
		ret = pthread_cond_wait(&td->free_cond, &td->io_u_lock);
		if (fio_unlikely(ret != 0)) {
			td->error = errno;
		} else if (!td->error)
			goto again;
	}

	if (needs_lock)
		__td_io_u_unlock(td);

	return io_u;
}

/* [한국어]
 * check_get_trim - trim 백로그 주기 도달 시 io_u 를 TRIM 으로 강제 변환.
 *
 * @td: 잡 스레드. TD_F_TRIM_BACKLOG 플래그, trim_entries/trim_batch/trim_backlog.
 * @io_u: 이번에 할당된 io_u (ddir 재할당 대상).
 * @return: true=trim 으로 세트됨(상위는 out 건너뜀), false=trim 미발생.
 *
 * 흐름:
 *  - trim_batch 남아 있으면 계속 trim(연속 소비).
 *  - io_hist_len % trim_backlog == 0 + last_ddir_completed != TRIM 이면 새 배치 시작.
 *  - get_next_trim (trim.c): 이전 쓰기 이력(io_hist)에서 trim 대상 영역 획득 → io_u 재설정.
 *
 * 호출 체인: get_io_u → [이 함수].
 */
static bool check_get_trim(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] TD_F_TRIM_BACKLOG 는 trim_backlog>0 옵션 활성 표식 — 미설정이면
	 * 이 경로 자체 skip. 핫패스 체크. */
	if (!(td->flags & TD_F_TRIM_BACKLOG))
		return false;
	/* [한국어] trim_entries 는 "trim 가능한 io_piece 수". 0 이면 이전 쓰기 이력
	 * 자체가 없어 trim 대상 없음. trim_batch 카운터도 리셋해 다음 쓰기까지 대기. */
	if (!td->trim_entries) {
		td->trim_batch = 0;
		return false;
	}

	/* [한국어] 진행 중인 배치 우선 — trim_batch>0 이면 계속 소비. */
	if (td->trim_batch) {
		td->trim_batch--;
		/* [한국어] get_next_trim: trim.c 가 io_hist 에서 다음 trim 대상 꺼내
		 * io_u 를 재설정(ddir=TRIM, offset/buflen 설정). 실패 시 배치 종료. */
		if (get_next_trim(td, io_u))
			return true;
		else
			td->trim_batch = 0;
	} else if (!(td->io_hist_len % td->o.trim_backlog) &&
		     td->last_ddir_completed != DDIR_TRIM) {
		/* [한국어] 새 배치 진입 조건: io_hist_len(누적 쓰기 기록)이 trim_backlog
		 * 배수 + 직전 완료가 TRIM 아닐 때. 후자는 연속 trim 루프 방지. */
		if (get_next_trim(td, io_u)) {
			/* [한국어] 배치 크기 결정: trim_batch 옵션이 있으면 그 값, 없으면
			 * trim_backlog 와 동일 — "backlog 전체를 한 번에 trim". */
			td->trim_batch = td->o.trim_batch;
			if (!td->trim_batch)
				td->trim_batch = td->o.trim_backlog;
			td->trim_batch--;
			return true;
		}
	}

	return false;
}

/* [한국어]
 * check_get_verify - verify 백로그 주기 도달 시 io_u 를 verify READ 로 강제 변환.
 *
 * @td: 잡 스레드. TD_F_VER_BACKLOG 플래그, io_hist_len, verify_batch/verify_backlog.
 * @io_u: 재설정 대상.
 * @return: true=verify 로 세트됨, false=미발생.
 *
 * 흐름:
 *  - verify_batch 남아 있으면 계속 verify.
 *  - io_hist_len % verify_backlog == 0 + last_ddir_completed != READ 이면 새 배치.
 *  - get_next_verify (verify.c): verify_list 에서 검증 대상 io_piece 꺼내
 *    io_u->offset/buflen/ddir=READ 로 세트, io_u->ipo 에 piece 포인터 연결.
 *
 * 호출 체인: get_io_u → [이 함수] (trim 체크 보다 먼저).
 */
static bool check_get_verify(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] verify_backlog 옵션 활성 여부. 미설정이면 즉시 false 로 일반 경로 진행. */
	if (!(td->flags & TD_F_VER_BACKLOG))
		return false;

	/* [한국어] io_hist_len: 누적된 (verify 가능) 쓰기 기록 수. 0이면 아직 verify
	 * 대상 없음. */
	if (td->io_hist_len) {
		int get_verify = 0;

		/* [한국어] 진행 중 verify 배치 소비 — trim 과 동일 패턴. */
		if (td->verify_batch)
			get_verify = 1;
		else if (!(td->io_hist_len % td->o.verify_backlog) &&
			 td->last_ddir_completed != DDIR_READ) {
			/* [한국어] 새 verify 배치 진입: io_hist_len 이 verify_backlog 배수 +
			 * 직전 완료가 READ 아닐 때. last=READ 조건은 verify 루프 폭주 방지. */
			td->verify_batch = td->o.verify_batch;
			if (!td->verify_batch)
				td->verify_batch = td->o.verify_backlog;
			get_verify = 1;
		}

		/* [한국어] get_next_verify 성공 = verify_list 에서 io_piece 꺼내 io_u 재설정.
		 * io_u->ddir=READ, offset/buflen/ipo 세트 — 후속 td_io_queue 가 verify 읽기 발행. */
		if (get_verify && !get_next_verify(td, io_u)) {
			td->verify_batch--;
			return true;
		}
	}

	return false;
}

/*
 * Fill offset and start time into the buffer content, to prevent too
 * easy compressible data for simple de-dupe attempts. Do this for every
 * 512b block in the range, since that should be the smallest block size
 * we can expect from a device.
 */
/* [한국어]
 * small_content_scramble - 쓰기 버퍼에 offset+timestamp 마커를 끼워 dedupe 회피.
 *
 * @io_u: 쓰기 io_u (xfer_buf 에 스크램블).
 *
 * 목적: 간단한 디덥(dedupe) 최적화를 속이지 않도록 각 512B 블록이 고유해지게
 * offset과 start_time 을 삽입. FIO의 dedupe_percentage 와 별개(dedupe 제어는
 * fill_io_buffer 에서, 이 함수는 매 write 마다 호출).
 *
 * 배치: 512B 블록마다 캐시라인(64B) 내 8가지 위치 중 하나에 offset(8B), 마지막
 * 16B 에 start_time.tv_sec/tv_nsec. 오프셋 선택에 (tv_nsec ^ boffset) & 7 사용.
 *
 * 호출 체인: get_io_u → [이 함수] (TD_F_SCRAMBLE_BUFFERS + !COMPRESS + !DO_VERIFY).
 */
static void small_content_scramble(struct io_u *io_u)
{
	/* [한국어] buflen >> 9 == buflen / 512 — 512B 는 대부분 디바이스의 최소
	 * 논리 블록. 이보다 세분화해도 디덥 관점에서 의미 없음. */
	unsigned long long i, nr_blocks = io_u->buflen >> 9;
	unsigned int offset;
	uint64_t boffset, *iptr;
	char *p;

	/* [한국어] buflen < 512 면 스크램블 대상 없음(잠재적 디버그 케이스). */
	if (!nr_blocks)
		return;

	p = io_u->xfer_buf;
	boffset = io_u->offset;

	/* [한국어] 이전에 채워진 상태 리셋 — scramble 후 이 버퍼는 오직 지금 시점의
	 * offset/time 에만 유효. 다음 쓰기에 재사용 시 재채움 유도. */
	if (io_u->buf_filled_len)
		io_u->buf_filled_len = 0;

	/*
	 * Generate random index between 0..7. We do chunks of 512b, if
	 * we assume a cacheline is 64 bytes, then we have 8 of those.
	 * Scramble content within the blocks in the same cacheline to
	 * speed things up.
	 */
	/* [한국어] 캐시라인 내 삽입 위치(0~7) 결정. tv_nsec ^ boffset 해시 후 & 7 로
	 * 저비트 3개 사용 — 시간/위치에 따라 고루 분산. 512B 가 8개 캐시라인을
	 * 포함하므로 8 중 하나 선택. */
	offset = (io_u->start_time.tv_nsec ^ boffset) & 7;

	for (i = 0; i < nr_blocks; i++) {
		/*
		 * Fill offset into start of cacheline, time into end
		 * of cacheline
		 */
		/* [한국어] 해당 캐시라인(offset 번째 64B 블록) 시작에 현재 바이트 오프셋
		 * 을 8B 값으로 삽입 — 각 512B 블록이 고유 id 를 갖게 됨(dedupe 회피). */
		iptr = (void *) p + (offset << 6);
		*iptr = boffset;

		/* [한국어] 마지막 캐시라인의 끝 16B 에 tv_sec/tv_nsec 타임스탬프 —
		 * 같은 offset 을 시간 다르게 쓰면 버퍼도 달라지므로 세션 간 dedupe 차단. */
		iptr = (void *) p + 64 - 2 * sizeof(uint64_t);
		iptr[0] = io_u->start_time.tv_sec;
		iptr[1] = io_u->start_time.tv_nsec;

		/* [한국어] 다음 512B 블록으로 전진 — boffset 도 512 증가시켜 각 블록이 고유 id. */
		p += 512;
		boffset += 512;
	}
}

/*
 * Return an io_u to be processed. Gets a buflen and offset, sets direction,
 * etc. The returned io_u is fully ready to be prepped, populated and submitted.
 */
/* [한국어]
 * get_io_u - ★ 핵심 ★ io_u 생명주기의 "할당+설정" 최상위 진입점.
 *
 * @td: 잡 스레드.
 * @return: 완전히 준비된 io_u (td_io_prep 까지 완료, 제출 즉시 가능),
 *          NULL (stop_io/재큐잉 없음+프리리스트 가득), 또는 ERR_PTR(-EBUSY) 등
 *          에러 포인터(IS_ERR로 검사).
 *
 * io_u 상태 전이:
 *   [freelist or requeues]
 *        │ __get_io_u (우선순위: requeues → freelist)
 *        ▼
 *   [allocated, plain] (IO_U_F_FREE clear, cur_depth++)
 *        │
 *        │ verify 백로그? (check_get_verify → get_next_verify → verify_list에서 pop)
 *        ├── Yes (DDIR_READ로 fill_io_u 대체, offset/buflen은 verify_header에서)
 *        │
 *        │ trim 백로그? (check_get_trim → get_next_trim)
 *        ├── Yes (DDIR_TRIM으로 강제 변환)
 *        │
 *        │ io_u->file 이미 세트됨? (requeue에서 온 io_u)
 *        ├── Yes (그대로 사용 — offset/buflen/ddir 모두 유지)
 *        │
 *        │ iolog replay 모드?
 *        ├── Yes (read_iolog_get이 로그에서 ddir/offset/buflen/buf 직접 채움)
 *        │
 *        │ 일반 경로
 *        └── set_io_u_file → get_next_file → fill_io_u (set_rw_ddir + get_next_offset +
 *                                                        get_next_buflen)
 *                    ▼
 *   [prepped: ddir, offset, buflen, file, buf, xfer_*, ioprio, start_time 채움]
 *                    ▼
 *            td_io_prep (엔진별 iocb/sqe/cdb 빌드)
 *                    ▼
 *              (do_io → td_io_queue)
 *
 * 쓰기 버퍼 처리:
 *  - TD_F_REFILL_BUFFERS: io_u_fill_buffer로 매 write마다 새 패턴 생성(dedupe 방지).
 *  - TD_F_SCRAMBLE_BUFFERS: small_content_scramble 마커 삽입 (start_time 기록 후).
 *  - TD_F_COMPRESS/TD_F_DO_VERIFY: scramble 금지 (패턴 보존).
 *
 * 실행 컨텍스트: 잡 스레드. get_io_u 호출 시점에 cur_depth++ 로 이 io_u 점유 기록.
 *
 * 호출 체인:
 *   backend.c: do_io → [이 함수] → td_io_queue → ... → io_completed → put_io_u
 *   verify 경로: do_verify → [이 함수] (verify_list 우선) → td_io_queue (READ) →
 *                 verify_io_u (패턴 비교)
 *
 * 에러 경로: fill/iolog/prep 실패 시 err_put 레이블 → put_io_u로 freelist 복귀
 * 후 ERR_PTR(ret) 반환 — 상위(do_io)는 errno 기반으로 재시도·종료 판단.
 */
struct io_u *get_io_u(struct thread_data *td)
{
	struct fio_file *f;
	struct io_u *io_u;
	int do_scramble = 0;
	long ret = 0;

	/* [한국어] ① io_u 풀에서 할당. requeues 우선, 그 다음 freelist. */
	io_u = __get_io_u(td);
	if (!io_u) {
		dprint(FD_IO, "__get_io_u failed\n");
		return NULL;
	}

	/* [한국어] ② verify 백로그 주기 도달이면 verify 경로로 강제 변환 — verify_list
	 * 의 io_piece 를 가져와 io_u->ddir=READ 로 세트, offset/buflen 도 piece 에서. */
	if (check_get_verify(td, io_u))
		goto out;
	/* [한국어] ③ trim 백로그 주기 도달이면 TRIM 으로 변환 — 이전 쓰기 이력에서
	 * trim 대상 추출. */
	if (check_get_trim(td, io_u))
		goto out;

	/*
	 * from a requeue, io_u already setup
	 */
	/* [한국어] ④ __get_io_u 가 requeues 에서 꺼낸 경우 io_u->file 이 이미 세트되어
	 * 있음(offset/buflen/ddir 도) — 다시 fill_io_u 할 필요 없이 바로 out. */
	if (io_u->file)
		goto out;

	/*
	 * If using an iolog, grab next piece if any available.
	 */
	/* [한국어] ⑤ iolog replay 모드: 로그 파일에서 ddir/offset/buflen 을 읽어와 그대로
	 * 적용. 기록-재생(replay) 워크로드용. */
	if (td->flags & TD_F_READ_IOLOG) {
		if (read_iolog_get(td, io_u))
			goto err_put;
	} else if (set_io_u_file(td, io_u)) {
		/* [한국어] ⑥ 일반 경로: 파일 선택 + fill_io_u 통합 루프. 실패 시 -EBUSY
		 * 로 상위에 "잠시 후 재시도" 힌트(getevents 후 다시). */
		ret = -EBUSY;
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	f = io_u->file;
	if (!f) {
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	assert(fio_file_open(f));

	/* [한국어] read/write/trim 분기 + 다중 범위 trim 아닌 단일 I/O 만 여기서 처리
	 * (다중 범위 trim 은 fill_multi_range_io_u 가 내부적으로 last_start/pos 관리). */
	if (ddir_rw(io_u->ddir) && !multi_range_trim(td, io_u)) {
		/* [한국어] buflen=0 은 fill_io_u 버그. NOIO 엔진은 I/O 를 안 보내므로 예외. */
		if (!io_u->buflen && !td_ioengine_flagged(td, FIO_NOIO)) {
			dprint(FD_IO, "get_io_u: zero buflen on %p\n", io_u);
			goto err_put;
		}

		/* [한국어] last_start/last_pos: 다음 순차 I/O 계산의 기준점. last_start 는
		 * rw_seq=IDENT / trimwrite 토글에도 사용. 반드시 여기서 갱신. */
		f->last_start[io_u->ddir] = io_u->offset;
		f->last_pos[io_u->ddir] = io_u->offset + io_u->buflen;

		if (io_u->ddir == DDIR_WRITE) {
			/* [한국어] WRITE: 사용자 의도에 따른 버퍼 처리 분기. */
			if (td->flags & TD_F_REFILL_BUFFERS) {
				/* [한국어] 매 쓰기마다 새 패턴 생성 — dedupe/compress 회피. */
				io_u_fill_buffer(td, io_u,
					td->o.min_bs[DDIR_WRITE],
					io_u->buflen);
			} else if ((td->flags & TD_F_SCRAMBLE_BUFFERS) &&
				   !(td->flags & TD_F_COMPRESS) &&
				   !(td->flags & TD_F_DO_VERIFY)) {
				/* [한국어] scramble: 512B 마다 offset+timestamp 마커 삽입.
				 * start_time 이 필요하므로 아래 td_io_prep 후 수행으로 지연.
				 * compress/verify 모드에서는 패턴 보존 필요 → scramble 금지. */
				do_scramble = 1;
			}
		} else if (io_u->ddir == DDIR_READ) {
			/*
			 * Reset the buf_filled parameters so next time if the
			 * buffer is used for writes it is refilled.
			 */
			/* [한국어] 읽기 후 이 io_u 가 다시 WRITE 로 쓰일 때 버퍼 재채움 유도 —
			 * buf_filled_len==0 이 "채움 필요" 마커. */
			io_u->buf_filled_len = 0;
		}
	}

	/*
	 * Set io data pointers.
	 */
	/* [한국어] xfer_buf/xfer_buflen 은 "엔진이 실제 읽고 쓰는" 포인터/길이. short I/O
	 * 재큐잉 시 xfer_buf 가 전진하고 xfer_buflen 이 줄지만, buf/buflen 은 원본 유지 —
	 * verify 등이 원본 범위 재현 가능. */
	io_u->xfer_buf = io_u->buf;
	io_u->xfer_buflen = io_u->buflen;

	/*
	 * Remember the issuing context priority. The IO engine may change this.
	 */
	/* [한국어] 잡 스레드 기본 ioprio 복사. cmdprio 등으로 per-I/O 변경할 수 있으며,
	 * 엔진(libaio/io_uring)이 이 값을 ioprio_set/IORING_SETUP_IOPRIO 로 전달. */
	io_u->ioprio = td->ioprio;
	io_u->clat_prio_index = 0;
out:
	assert(io_u->file);
	/* [한국어] ⑦ 엔진별 사전 준비: iocb/sqe/cdb 필드 빌드. 실패하면 err_put 경로. */
	if (!td_io_prep(td, io_u)) {
		/* [한국어] ⑧ lat 측정 기준 시각 기록 — td_io_prep 후, queue 전.
		 * disable_lat=1 이면 overhead 제거 목적. */
		if (!td->o.disable_lat)
			fio_gettime(&io_u->start_time, NULL);

		/* [한국어] scramble 은 start_time 의 tv_sec/tv_nsec 을 버퍼에 삽입하므로
		 * 반드시 gettime 이후에 수행. */
		if (do_scramble)
			small_content_scramble(io_u);

		return io_u;
	}
err_put:
	/* [한국어] 실패 경로: io_u 를 freelist 로 회수 후 에러 포인터로 반환. 상위
	 * do_io 는 IS_ERR 로 감지해 -EBUSY 면 getevents 후 재시도. */
	dprint(FD_IO, "get_io_u failed\n");
	put_io_u(td, io_u);
	return ERR_PTR(ret);
}

/* [한국어]
 * __io_u_log_error - I/O 에러 로그 출력 + 치명 여부에 따라 td->error 마킹.
 *
 * @td: 잡 스레드.
 * @io_u: 에러 발생 io_u (error/ddir/file/offset/xfer_buflen 참조).
 *
 * td_non_fatal_error 판정(ignore_error 옵션, continue_on_error 비트):
 *  - non-fatal + error_dump=0 → 조용히 리턴.
 *  - 그 외 → log_err + (io_ops->errdetails 콜백 → 엔진별 상세 — SCSI sense data,
 *    NVMe cpl_status 등).
 *  - zbd_log_err: ZBD 존 오류 별도 포맷.
 *  - 치명적 + td->error 아직 없음 → td_verror(io_u->error) 마킹.
 *
 * 호출 체인: io_u_log_error → [이 함수] (자신 + 부모).
 */
static void __io_u_log_error(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 에러 유형 판별 */
	enum error_type_bit eb = td_error_type(io_u->ddir, io_u->error);
	bool non_fatal_error = td_non_fatal_error(td, eb, io_u->error);

	/*
	 * Non-fatal errors (errors that should be ignored), are normally not
	 * dumped to the log, unless td->o.error_dump. Regardless, non-fatal
	 * errors should never call td_verror() to set td->error.
	 */
	/* [한국어] 비치명적 에러이고 error_dump가 꺼져 있으면 로그 생략 */
	if (non_fatal_error && !td->o.error_dump)
		return;

	log_err("fio: io_u error%s%s: %s: %s offset=%llu, buflen=%llu\n",
		io_u->file ? " on file " : "",
		io_u->file ? io_u->file->file_name : "",
		(io_u->flags & IO_U_F_DEVICE_ERROR) ?
			"Device-specific error" : strerror(io_u->error),
		io_ddir_name(io_u->ddir),
		io_u->offset, io_u->xfer_buflen);

	/* [한국어] ZBD 관련 에러 로그 */
	zbd_log_err(td, io_u);

	/* [한국어] 엔진별 상세 에러 정보 출력 */
	if (td->io_ops->errdetails) {
		char *err = td->io_ops->errdetails(td, io_u);

		if (err) {
			log_err("fio: %s\n", err);
			free(err);
		}
	}

	/* [한국어] 치명적 에러이면 스레드 에러 상태 설정 */
	if (!td->error && !non_fatal_error)
		td_verror(td, io_u->error, "io_u error");
}

/* [한국어]
 * io_u_log_error - I/O 에러 외부 API: 자신과 부모 스레드 양쪽에 로깅.
 *
 * @td: 잡 스레드. 자식이면 td->parent 로도 중복 로깅(verify async 스레드 등).
 * @io_u: 에러 io_u.
 *
 * 호출 체인: io_completed (error 경로) → [이 함수].
 */
void io_u_log_error(struct thread_data *td, struct io_u *io_u)
{
	__io_u_log_error(td, io_u);
	/* [한국어] 자식 스레드면 부모에도 에러 기록. async verify 경로에서
	 * 부모(잡) 스레드 집계에도 반영. */
	if (td->parent)
		__io_u_log_error(td->parent, io_u);
}

/* [한국어]
 * gtod_reduce - 시간 측정(fio_gettime) 호출을 최소화해도 되는지 판정.
 *
 * @td: 잡 스레드.
 * @return: true=gtod 전부 생략 가능(slat/clat/bw 모두 disabled 또는 gtod_reduce 옵션),
 *          false=최소 하나 이상 활성 → fio_gettime 필요.
 *
 * 용도: init_icd 가 icd.time 기록 여부 판단, account_io_completion 이 clat 계산
 * 생략 여부 판단 등.
 */
static inline bool gtod_reduce(struct thread_data *td)
{
	return (td->o.disable_clat && td->o.disable_slat && td->o.disable_bw)
			|| td->o.gtod_reduce;
}

/* [한국어]
 * trim_block_info - TRIM 완료 시 io log 의 해당 블록 상태를 TRIMMED 로 전이.
 *
 * @td: 잡 스레드. td->ts.block_infos[] 는 iolog.c 가 관리하는 블록별 상태 추적.
 * @io_u: TRIM 완료 io_u.
 *
 * 블록 상태 머신(iolog.c):
 *   UNINIT → 쓰기 후 WRITTEN → trim 후 TRIMMED → 재쓰기 후 WRITTEN_TRIMMED ...
 * 이 함수는 TRIMMED 방향 전이만 처리. BLOCK_STATE_TRIM_FAILURE 이상이면 회수.
 * 파라미터의 BLOCK_INFO_TRIMS 비트는 누적 trim 횟수.
 *
 * 호출 체인: account_io_completion (ddir==TRIM) → [이 함수].
 */
static void trim_block_info(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] io_u_block_info: io_u->offset/buflen 기준 블록의 block_info 슬롯 주소.
	 * 비트 필드: [31..16]=trim_count, [15..0]=state enum. */
	uint32_t *info = io_u_block_info(td, io_u);

	/* [한국어] BLOCK_STATE_TRIM_FAILURE 이상은 최종 상태(실패 보존) — 덮어쓰기 금지. */
	if (BLOCK_INFO_STATE(*info) >= BLOCK_STATE_TRIM_FAILURE)
		return;

	/* [한국어] state=TRIMMED 로 전이 + trim 횟수 1 증가. 하나의 원자 store 로 업데이트. */
	*info = BLOCK_INFO(BLOCK_STATE_TRIMMED, BLOCK_INFO_TRIMS(*info) + 1);
}

/* [한국어]
 * account_io_completion - 완료된 io_u 의 lat/clat/bw/iops 샘플을 stat.c 에 전달.
 *
 * @td: 잡 스레드(자식이면 부모로 전환 — 통계는 부모 집계).
 * @io_u: 완료된 io_u.
 * @icd: 배치 컨텍스트 (icd->time 이 완료 시각 기준점).
 * @idx: 통계 방향 인덱스(READ/WRITE/TRIM/SYNC).
 * @bytes: 실제 전송 바이트(xfer_buflen - resid).
 *
 * 샘플 산출:
 *   tnsec = ntime_since(start_time, icd->time)   → 총 lat (잡이 io_u 획득~완료)
 *   llnsec = ntime_since(issue_time, icd->time)  → clat (제출~완료)
 *   slat 는 별도로 io_u_queued() 에서 기록됨.
 *
 * 전파:
 *   - add_lat_sample: ts.lat_* 히스토그램 누적.
 *   - add_clat_sample: ts.clat_* 히스토그램.
 *   - io_u_mark_latency: ts.io_u_lat_{n,u,m}sec 버킷.
 *   - add_bw_sample/add_iops_sample: 윈도우 기반 rate 샘플(per_unit_log 조건).
 *   - add_sync_clat_sample: ddir_sync 전용.
 *   - trim_block_info: TRIM 전용.
 *
 * latency_target / max_latency 위반 검사:
 *   - max_latency[idx] 초과 → lat_fatal → ETIMEDOUT 치명 에러.
 *   - latency_target 초과 → lat_target_failed → percentile 100% 면 즉시 fatal.
 *
 * 통계 비활성(td->o.stats=0 또는 FIO_NOSTATS 엔진)이면 조기 리턴.
 *
 * 호출 체인: io_completed → [이 함수] (should_account 통과 시).
 */
static void account_io_completion(struct thread_data *td, struct io_u *io_u,
				  struct io_completion_data *icd,
				  const enum fio_ddir idx, unsigned int bytes)
{
	/* [한국어] gtod_reduce=1 또는 slat/clat/bw 전부 disable 이면 fio_gettime 호출을
	 * 생략 가능 — no_reduce 가 false 면 clat 계산 자체가 의미 없음(시간 안 찍힘). */
	const int no_reduce = !gtod_reduce(td);
	unsigned long long llnsec = 0;

	/* [한국어] 자식(verify async/offload) 스레드의 완료는 부모 td 의 통계에 집계 —
	 * 사용자 관점에서 잡 단위 통계가 단일 뷰. */
	if (td->parent)
		td = td->parent;

	/* [한국어] stats=0 옵션 또는 FIO_NOSTATS 엔진(예: cpu 엔진처럼 I/O 없는 모드)은
	 * 통계 수집 skip — 의미 없는 샘플로 리소스 낭비 방지. */
	if (!td->o.stats || td_ioengine_flagged(td, FIO_NOSTATS))
		return;

	/* [한국어] clat(completion latency) = 제출~완료 시간. issue_time 은 td_io_queue
	 * 에서 fio_fill_issue_time 으로 세트. icd->time 은 배치 완료 시각(init_icd). */
	if (no_reduce)
		llnsec = ntime_since(&io_u->issue_time, &icd->time);

	/* [한국어] lat(total latency) = io_u 획득 시점(start_time) ~ 완료. fill_io_u
	 * 후 td_io_prep 전에 fio_gettime 으로 세트. clat 보다 크며 큐잉 대기까지 포함. */
	if (!td->o.disable_lat) {
		unsigned long long tnsec;

		tnsec = ntime_since(&io_u->start_time, &icd->time);
		/* [한국어] ts.lat_* 히스토그램에 샘플 추가 — bytes 가중으로 대역폭 환산도 가능. */
		add_lat_sample(td, idx, tnsec, bytes, io_u);

		/* [한국어] profile(예 tiobench/act) 에 등록된 io_u_lat 콜백 호출. 프로파일
		 * 이 자체 조건 판정 후 icd->error 를 세트해 잡 조기 종료 유도 가능. */
		if (td->flags & TD_F_PROFILE_OPS) {
			struct prof_io_ops *ops = &td->prof_io_ops;

			if (ops->io_u_lat)
				icd->error = ops->io_u_lat(td, tnsec);
		}

		/* [한국어] read/write 방향만 레이턴시 목표/한계 체크(TRIM/SYNC 은 성능
		 * 측정 중심 밖). max_latency[idx] 는 방향별 독립. */
		if (ddir_rw(idx)) {
			/* [한국어] max_latency 는 절대 상한 — 1회 초과로 즉시 ETIMEDOUT fatal. */
			if (td->o.max_latency[idx] && tnsec > td->o.max_latency[idx])
				lat_fatal(td, io_u, icd, tnsec, td->o.max_latency[idx]);
			/* [한국어] latency_target 은 이진 탐색 기준 — 초과시 lat_target_failed
			 * 에서 percentile 정책에 따라 즉시 fatal 또는 카운터 증가. */
			if (td->o.latency_target && tnsec > td->o.latency_target) {
				if (lat_target_failed(td))
					lat_fatal(td, io_u, icd, tnsec, td->o.latency_target);
			}
		}
	}

	if (ddir_rw(idx)) {
		/* [한국어] clat 샘플 추가 + 지연 히스토그램 버킷(ns/us/ms 3단계) 갱신. */
		if (!td->o.disable_clat) {
			add_clat_sample(td, idx, llnsec, bytes, io_u);
			io_u_mark_latency(td, llnsec);
		}

		/* [한국어] bw 로그: per_unit_log 가 true 면 이번 완료를 로그 윈도우에 기록.
		 * 로그 간격 설정 시에만 활성화 — 없으면 집계는 io_bytes 누적만으로 계산. */
		if (!td->o.disable_bw && per_unit_log(td->bw_log))
			add_bw_sample(td, io_u, bytes, llnsec);

		/* [한국어] IOPS 로그 — no_reduce 조건은 llnsec 유효성 보장 + 로그 활성화 검사. */
		if (no_reduce && per_unit_log(td->iops_log))
			add_iops_sample(td, io_u, bytes);
	} else if (ddir_sync(idx) && !td->o.disable_clat)
		/* [한국어] sync 는 읽기/쓰기 회계와 별도 — ts.sync_stat 에 clat 만 누적. */
		add_sync_clat_sample(&td->ts, llnsec);

	/* [한국어] TRIM 완료 시 block_info 상태를 TRIMMED 로 전이 — iolog 가 블록별
	 * 상태를 쓰기/트림 횟수로 추적해 verify/trimwrite 정합성 판단. */
	if (td->ts.nr_block_infos && io_u->ddir == DDIR_TRIM)
		trim_block_info(td, io_u);
}

/* [한국어]
 * file_log_write_comp - 쓰기 완료 시 파일의 dirty 범위(first_write..last_write) 확장.
 *
 * @td: 잡 스레드(const).
 * @f: 대상 파일.
 * @offset/bytes: 방금 완료된 쓰기 범위.
 *
 * 용도: 다음 sync_file_range 호출(DDIR_SYNC_FILE_RANGE) 시 이 범위만 flush.
 * 성공한 SYNC 후에는 first_write=last_write=-1ULL 로 리셋(io_completed 의 sync 경로).
 *
 * 호출 체인: io_completed (DDIR_WRITE 성공) → [이 함수].
 */
static void file_log_write_comp(const struct thread_data *td, struct fio_file *f,
				uint64_t offset, unsigned int bytes)
{
	if (!f)
		return;

	/* [한국어] first_write = 이번 sync 주기 이후 쓰기 중 "가장 작은 offset".
	 * -1ULL sentinel 은 "아직 이 주기에 쓰기 없음" — 첫 쓰기 시 offset 로 초기화. */
	if (f->first_write == -1ULL || offset < f->first_write)
		f->first_write = offset;
	/* [한국어] last_write = 가장 큰 끝 바이트(= offset + bytes). 다음 sync_file_range
	 * 호출 시 [first_write, last_write) 영역만 flush 대상. */
	if (f->last_write == -1ULL || ((offset + bytes) > f->last_write))
		f->last_write = offset + bytes;
}

/* [한국어]
 * should_account - 현재 통계 기록 조건 판정(ramp 완료 + RUNNING/VERIFYING 상태).
 *
 * @td: 잡 스레드.
 * @return: true=account_io_completion 호출 허용, false=무시(ramp 중 또는 잘못된 상태).
 *
 * ramp_time 옵션이 활성이면 잡 시작 후 ramp_period 동안 통계 수집 안 함(워밍업).
 * runstate 가 TD_RUNNING/TD_VERIFYING 이 아닌 상태(초기화/종료 중)도 무시.
 *
 * 호출 체인: io_completed → [이 함수] → account_io_completion 가드.
 */
static bool should_account(struct thread_data *td)
{
	return ramp_period_over(td) && (td->runstate == TD_RUNNING ||
					   td->runstate == TD_VERIFYING);
}

/* [한국어]
 * io_completed - ★ 핵심 ★ 단일 io_u 완료 처리: 상태 전이, 회계, verify 연계.
 *
 * @td: 잡 스레드 데이터.
 * @io_u_ptr: io_u 이중 포인터. short I/O 재큐잉 시 requeue_io_u 가 *io_u_ptr=NULL로
 *            바꿔 호출자(ios_completed/*_sync_complete)가 put_io_u를 건너뛰도록 한다.
 *            end_io 콜백이 io_u를 교체할 수도 있어 이중 포인터.
 * @icd: 배치 완료 데이터(nr/error/bytes_done[]/time). 이 함수는 icd->bytes_done[ddir]
 *       에 완료 바이트 누적 + icd->error에 최초 에러 저장.
 *
 * io_u 상태 전이:
 *   [in_flight (IO_U_F_FLIGHT set)] → io_completed →
 *       성공+ddir_rw → [io_blocks++, io_bytes+=, ipo.flags clear(IP_F_IN_FLIGHT),
 *                       account_io_completion, end_io 콜백, icd.bytes_done+=]
 *                   → (상위에서 put_io_u → freelist)
 *       성공+short → [xfer_buflen=resid, xfer_buf+=, offset+=, requeue_io_u]
 *                   → (다음 do_io 이터레이션에서 나머지 발행)
 *       성공+ddir_sync → [first_write=last_write=-1ULL, account_io_completion]
 *       에러 → [td_non_fatal_error? 계속 : td_verror, icd.error=io_u->error]
 *
 * 주요 단계:
 *   1. FLIGHT 상태 검증 후 IO_U_F_FLIGHT|BUSY_OK|PATTERN_DONE 3비트 동시 clear.
 *   2. invalidate_inflight: offload overlap 추적 해제.
 *   3. ZBD 쓰기 에러 복구(recover_zbd_write_error=1 + WRITE + !SYNCIO 조건).
 *   4. verify 연계: io_u->ipo(io_piece) 가 있으면 에러는 unlog, 성공은
 *      atomic_store_release(ipo->flags, ... & ~IP_F_IN_FLIGHT) — 메모리 배리어로
 *      verify 스레드가 본 flags가 이전 쓰기 내용 "이후"를 보장.
 *   5. ddir_sync 처리: dirty 범위 리셋 + account 후 조기 리턴.
 *   6. read/write: resid != 0 면 short I/O → requeue(나머지 바이트 재발행).
 *      bytes=xfer_buflen-resid 로 실제 전송량 계산.
 *      io_blocks/io_bytes/this_io_blocks/this_io_bytes 각 업데이트 — this_io_*
 *      는 현재 "런"(ramp 이후) 통계용, verify_list 플래그이면 this_io_* 누적 안 함.
 *   7. DDIR_WRITE 시 file_log_write_comp로 first_write/last_write 범위 추적
 *      (후속 sync_file_range 타겟용).
 *   8. account_io_completion: lat/slat/clat/bw/iops 샘플 → stat.c.
 *   9. io_u->end_io 콜백: verify_io_u_async 등. ret 값이 icd.error로 승격.
 *  10. icd.error 처리: fatal이면 리턴, non-fatal이면 update_error_count+clear+continue.
 *
 * 실행 컨텍스트: 잡 스레드(sync 완료) 또는 잡 스레드(async 완료 수집).
 * verify 스레드는 자체 io_completed를 호출.
 *
 * 호출 체인:
 *   io_u_sync_complete → [이 함수] (1건 동기 완료)
 *   ios_completed → [이 함수] (N건 비동기 완료 반복)
 *   상위는 put_io_u로 freelist 반환.
 *
 * 에러 경로: icd->error가 세트되면 위의 update_error_count 호출 후 비치명이면
 * 0으로 clear하여 루프 계속. 치명적이면 상위에서 td_verror → 잡 중단.
 */
static void io_completed(struct thread_data *td, struct io_u **io_u_ptr,
			 struct io_completion_data *icd)
{
	struct io_u *io_u = *io_u_ptr;
	enum fio_ddir ddir = io_u->ddir;
	struct fio_file *f = io_u->file;

	dprint_io_u(io_u, "complete");

	/* [한국어] FLIGHT 비트 확인 — in-flight 아닌 io_u 를 완료하는 것은 상위 버그.
	 * io_u_clear_inflight_flags 는 FLIGHT/BUSY_OK/PATTERN_DONE 3비트 동시 제거. */
	assert(io_u->flags & IO_U_F_FLIGHT);
	io_u_clear_inflight_flags(td, io_u);
	/* [한국어] offload submit 모드에서 동일 오프셋 중복 제출 방지용 in-flight
	 * 맵에서 이 io_u 제거. 일반 모드는 no-op. */
	invalidate_inflight(td, io_u);

	/* [한국어] ZBD + recover_zbd_write_error=1 + WRITE 에러 + 비동기 엔진 조건:
	 * ZNS 장치는 write pointer 불일치 등으로 쓰기 실패 가능 — 복구 시도(write
	 * pointer 재조회 후 offset 정렬). SYNCIO 엔진은 이미 엔진 레벨에서 동기
	 * 처리되므로 제외. */
	if (td->o.zone_mode == ZONE_MODE_ZBD && td->o.recover_zbd_write_error &&
	    io_u->error && io_u->ddir == DDIR_WRITE &&
	    !td_ioengine_flagged(td, FIO_SYNCIO))
		zbd_recover_write_error(td, io_u);

	/*
	 * Mark IO ok to verify
	 */
	/* [한국어] io_u->ipo: verify 메타데이터 (io_piece). 쓰기 발행 시 log_io_piece
	 * 로 verify_list 에 추가되고 io_u->ipo 가 그 piece 를 가리킴. */
	if (io_u->ipo) {
		/*
		 * Remove errored entry from the verification list
		 */
		/* [한국어] 에러면 이 영역은 신뢰할 수 없으므로 verify_list 에서 제거 —
		 * 후속 do_verify 가 이 영역을 건드리지 않도록. */
		if (io_u->error)
			unlog_io_piece(td, io_u);
		else {
			/* [한국어] 성공: ipo->flags 에서 IP_F_IN_FLIGHT 원자적 해제.
			 * atomic_store_release: 이 스토어 이전의 모든 메모리 쓰기(io_u->buf
			 * 의 내용 등)가 verify 스레드에 반드시 보이도록 release 배리어. */
			atomic_store_release(&io_u->ipo->flags,
					io_u->ipo->flags & ~IP_F_IN_FLIGHT);
		}
	}

	/* [한국어] sync 계열(fsync/fdatasync/sync_file_range): offset/buflen 이 무의미. */
	if (ddir_sync(ddir)) {
		if (io_u->error)
			goto error;
		/* [한국어] sync 성공 = 모든 pending write 가 영속화됨 → dirty 범위 리셋.
		 * 다음 sync 주기까지 file_log_write_comp 가 범위 재누적. */
		if (f) {
			f->first_write = -1ULL;
			f->last_write = -1ULL;
		}
		/* [한국어] sync clat 은 특수 경로(add_sync_clat_sample) — buflen 을 바이트
		 * 대신 전달하는데 통계적으로는 "이 sync 에 관여한 메타데이터 크기" 근사. */
		if (should_account(td))
			account_io_completion(td, io_u, icd, ddir, io_u->buflen);
		return;
	}

	/* [한국어] last_ddir_completed 는 check_get_trim/verify 에서 "최근 완료
	 * 방향이 아니면 새 배치 진입" 조건에 쓰임. 루프 발산 방지. */
	td->last_ddir_completed = ddir;

	/* [한국어] 성공 + read/write/trim 분기 — 실제 데이터 처리 회계 경로. */
	if (!io_u->error && ddir_rw(ddir)) {
		/* [한국어] 실제 전송 바이트 = 요청량 - 잔여량(엔진이 부분만 처리 가능). */
		unsigned long long bytes = io_u->xfer_buflen - io_u->resid;
		int ret;

		/*
		 * Make sure we notice short IO from here, and requeue them
		 * appropriately!
		 */
		/* [한국어] short I/O: 요청 < 완료. 일부는 성공, 나머지 resid 는 다시
		 * 발행 필요. xfer_buf/offset 을 성공분만큼 전진시키고 buflen 을 잔여로
		 * 줄여 requeue_io_u → 다음 do_io 이터레이션에서 이어서 발행. */
		if (bytes && io_u->resid) {
			io_u->xfer_buflen = io_u->resid;
			io_u->xfer_buf += bytes;
			io_u->offset += bytes;
			td->ts.short_io_u[io_u->ddir]++;
			/* [한국어] 새 offset 이 여전히 파일 내면 재발행. 파일 끝 넘어가면
			 * 그대로 정상 완료로 처리(아래로 흘러감). */
			if (io_u->offset < io_u->file->real_file_size) {
				requeue_io_u(td, io_u_ptr);
				return;
			}
		}

		/* [한국어] 누적 통계 갱신 — io_blocks 는 완료 블록 수(IOPs 기반),
		 * io_bytes 는 총 바이트(대역폭 기반). 잡 전체 수명 누계. */
		td->io_blocks[ddir]++;
		td->io_bytes[ddir] += bytes;

		/* [한국어] this_io_* 는 "현재 런"(ramp 이후) 집계로 verify 경로와 분리.
		 * VER_LIST 플래그는 이 io_u 가 verify 재발행이므로 실제 워크로드 통계에
		 * 섞이면 안 됨(verify 는 별도 카운터). */
		if (!(io_u->flags & IO_U_F_VER_LIST)) {
			td->this_io_blocks[ddir]++;
			td->this_io_bytes[ddir] += bytes;
		}

		/* [한국어] 쓰기 성공 후 first_write/last_write 범위 확장 — 이후
		 * DDIR_SYNC_FILE_RANGE 시 이 범위만 부분 sync 대상으로 사용. */
		if (ddir == DDIR_WRITE)
			file_log_write_comp(td, f, io_u->offset, bytes);

		/* [한국어] stats 활성 + ramp 통과 시에만 샘플 기록 — ramp 워밍업
		 * 기간에 수집하면 대표성 훼손. */
		if (should_account(td))
			account_io_completion(td, io_u, icd, ddir, bytes);

		/* [한국어] icd.bytes_done[ddir] 은 배치 합산 — 호출자가 td->bytes_done 에
		 * 1회만 더하도록 집계 분리. */
		icd->bytes_done[ddir] += bytes;

		/* [한국어] end_io 콜백(verify_io_u_async 등): 완료 직후 추가 처리가 필요한
		 * 경로용. 반환값 != 0 이면 icd.error 로 승격(단, 이미 에러면 덮어쓰지 않음). */
		if (io_u->end_io) {
			ret = io_u->end_io(td, io_u_ptr);
			io_u = *io_u_ptr;
			if (ret && !icd->error)
				icd->error = ret;
		}
	} else if (io_u->error) {
error:
		/* [한국어] 에러 경로 — icd.error 에 기록 + io_u_log_error 로 상세 출력. */
		icd->error = io_u->error;
		io_u_log_error(td, io_u);
	}
	if (icd->error) {
		enum error_type_bit eb = td_error_type(ddir, icd->error);

		/* [한국어] ignore_error 로 비치명 판정이면 그대로 리턴 — 상위 update_error
		 * 없이 fatal 처리(상위에서 break_on_this_error 결정). */
		if (!td_non_fatal_error(td, eb, icd->error))
			return;

		/*
		 * If there is a non_fatal error, then add to the error count
		 * and clear all the errors.
		 */
		/* [한국어] continue_on_error 등으로 비치명이면 에러 카운터만 증가시키고
		 * td->error/icd->error/io_u->error 를 리셋해 잡 계속 진행. */
		update_error_count(td, icd->error);
		td_clear_error(td);
		icd->error = 0;
		if (io_u)
			io_u->error = 0;
	}
}

/* [한국어]
 * init_icd - io_completion_data(배치 컨텍스트) 필드 초기화.
 *
 * @td: 잡 스레드.
 * @icd: 초기화할 완료 컨텍스트(스택 할당이 일반적).
 * @nr: 이 배치에서 처리할 완료 수.
 *
 * 설정:
 *   icd->time = 현재 시각 (gtod_reduce 미활성 시에만).
 *   icd->nr = nr.
 *   icd->error = 0.
 *   icd->bytes_done[READ/WRITE/TRIM] = 0.
 *
 * icd->time 은 배치 내 모든 io_u 에 대해 같은 기준점을 사용 — 개별 io_u 마다
 * gettime 호출 비용 회피.
 *
 * 호출 체인:
 *   io_u_sync_complete → [이 함수] (nr=1)
 *   io_u_queued_complete → [이 함수] (nr=getevents 반환값)
 */
static void init_icd(struct thread_data *td, struct io_completion_data *icd,
		     int nr)
{
	int ddir;

	/* [한국어] gtod_reduce 모드면 fio_gettime 자체를 생략(clat 측정 불가해도 수용).
	 * 정상 모드에서는 이 시각이 icd 내 모든 io_u 의 공통 완료 기준점. */
	if (!gtod_reduce(td))
		fio_gettime(&icd->time, NULL);

	icd->nr = nr;

	icd->error = 0;
	/* [한국어] DDIR_RWDIR_CNT 만큼 루프 — READ/WRITE/TRIM 각 슬롯 0 초기화. */
	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
		icd->bytes_done[ddir] = 0;
}

/* [한국어]
 * ios_completed - 비동기 완료 배치 N건을 엔진 event() → io_completed → put_io_u 순회.
 *
 * @td: 잡 스레드.
 * @icd: init_icd 된 배치 컨텍스트.
 *
 * 루프: for i in 0..icd->nr-1 do
 *   io_u = td->io_ops->event(td, i)  — 엔진별 i번째 완료 io_u 반환(libaio 는
 *     aio_events[] 에서 꺼냄, io_uring 은 CQE 순회, posixaio 는 aio_error 확인 등).
 *   io_completed(&io_u) — short I/O 면 io_u=NULL 로 세트될 수 있음.
 *   io_u 유효하면 put_io_u 로 freelist 반환.
 *
 * 호출 체인: io_u_queued_complete → [이 함수].
 */
static void ios_completed(struct thread_data *td,
			  struct io_completion_data *icd)
{
	struct io_u *io_u;
	int i;

	for (i = 0; i < icd->nr; i++) {
		/* [한국어] 엔진별 .event(td, i) 콜백: i번째 완료된 io_u* 반환.
		 * libaio=aio_events[i].data, io_uring=CQE user_data 역참조,
		 * posixaio=aiocb 리스트에서 완료된 것. 순서는 엔진 정의. */
		io_u = td->io_ops->event(td, i);

		/* [한국어] short I/O 재큐잉 시 io_u 가 NULL 로 세트되어 나옴. */
		io_completed(td, &io_u, icd);

		/* [한국어] 정상 완료(NULL 아님)만 freelist 로. NULL = requeue 경로. */
		if (io_u)
			put_io_u(td, io_u);
	}
}

/* [한국어]
 * io_u_update_bytes_done - 배치 완료 바이트를 td->bytes_done[] 에 누적.
 *
 * @td: 잡 스레드.
 * @icd: 방금 처리한 배치 컨텍스트(bytes_done[ddir] 채워짐).
 *
 * TD_VERIFYING 상태이면 READ 바이트를 bytes_verified 에 추가(verify 전용 집계).
 * td_write 이면 그 시점에 조기 리턴(write 섞인 잡은 verify 중에도 write 바이트
 * 누적을 bytes_done 에 반영 — 아래 루프 실행).
 * 그 외에는 READ/WRITE/TRIM 각각 bytes_done 에 누적 — do_io 진행률 판정에 사용.
 *
 * 호출 체인: io_u_sync_complete/io_u_queued_complete → [이 함수].
 */
static void io_u_update_bytes_done(struct thread_data *td,
				   struct io_completion_data *icd)
{
	int ddir;

	/* [한국어] TD_VERIFYING: 쓰기 완료 후 검증 단계 (do_verify). 이때 읽어들이는
	 * 바이트는 "이미 bytes_done 에 계산된 쓰기"의 재-읽기이므로 bytes_done 에
	 * 또 더하면 이중 계산. 대신 bytes_verified 별도 카운터에 누적. td_write 이면
	 * 같이 쓰기도 있는 잡(쓰기+verify)이므로 아래 루프로 계속. */
	if (td->runstate == TD_VERIFYING) {
		td->bytes_verified += icd->bytes_done[DDIR_READ];
		if (td_write(td))
			return;
	}

	/* [한국어] 일반 경로 — READ/WRITE/TRIM 각 방향 바이트를 누적 진행률에 합산. */
	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
		td->bytes_done[ddir] += icd->bytes_done[ddir];
}

/*
 * Complete a single io_u for the sync engines.
 */
/* [한국어]
 * io_u_sync_complete - ★ 핵심 ★ 동기 엔진 1건 완료 처리.
 *
 * @td: 잡 스레드.
 * @io_u: 방금 제출되자마자 완료된 io_u (FIO_Q_COMPLETED 반환 경로).
 * @return: 0=성공, -1=치명적 에러(td_verror 로 에러 기록).
 *
 * 호출 시점: td_io_queue 가 FIO_Q_COMPLETED 를 반환한 직후 또는 SYNCIO 엔진의
 * td_io_commit 내부에서. 완료된 io_u 가 1개 뿐이므로 icd.nr=1.
 *
 * 내부 흐름:
 *   [FLIGHT io_u] → init_icd(1) → io_completed(큰 완료 로직 전체) → put_io_u →
 *   bytes_done[ddir] 누적 → icd.error 면 -1.
 *
 * 호출 체인:
 *   backend.c: do_io 루프에서 td_io_queue 반환값이 FIO_Q_COMPLETED 또는
 *   ioengine에서 동기 완료 시 → [이 함수].
 *
 * 실행 컨텍스트: 잡 스레드.
 *
 * 에러 경로: icd.error 전파 → -1 반환 → do_io 루프에서 break_on_this_error
 * 판정 후 잡 종료 여부 결정.
 */
int io_u_sync_complete(struct thread_data *td, struct io_u *io_u)
{
	struct io_completion_data icd;

	/* [한국어] nr=1 로 icd 초기화 — 동기 완료는 항상 단건. icd.time 은 완료 시각
	 * 기준점으로 account_io_completion 이 latency 계산에 사용. */
	init_icd(td, &icd, 1);
	/* [한국어] 이중 포인터 전달 — short I/O 재큐잉 시 io_u=NULL 로 세트되어
	 * 아래 put_io_u 가 스킵됨(requeue 경로는 이미 freelist 아닌 requeues 에 들어감). */
	io_completed(td, &io_u, &icd);

	/* [한국어] io_u != NULL 이면 정상 완료 → freelist 반납. NULL 이면 requeued 상태. */
	if (io_u)
		put_io_u(td, io_u);

	/* [한국어] icd.error 는 io_u->error 또는 end_io 콜백 에러가 전파된 값. td_verror
	 * 로 잡 에러 마킹 후 -1 반환해 상위 do_io 루프가 break_on_this_error 판정. */
	if (icd.error) {
		td_verror(td, icd.error, "io_u_sync_complete");
		return -1;
	}

	/* [한국어] bytes_done[ddir] 누적 — 잡 진행률(done/size) 판정에 사용. */
	io_u_update_bytes_done(td, &icd);

	return 0;
}

/*
 * Called to complete min_events number of io for the async engines.
 */
/* [한국어]
 * io_u_queued_complete - ★ 핵심 ★ 비동기 엔진 N건 완료 처리.
 *
 * @td: 잡 스레드.
 * @min_evts: 최소 대기 완료 수. 0이면 논블로킹(timespec={0,0}).
 * @return: 처리한 완료 수(>=0) 또는 -1(td_io_getevents 음수 반환 시).
 *
 * 비동기 엔진(libaio/io_uring/posixaio/rdma/ime 등)에서 getevents 로 완료 배치를
 * 한 번에 수확하고 io_completed 를 N회 돌려 "완료 회계"를 수행한다.
 *
 * 처리 순서:
 *   1. min_evts 조정: 0 → 논블로킹 포인터(tvp). min_evts > cur_depth 면 클램프.
 *   2. td_io_getevents(min=..., max=iodepth_batch_complete_max, tvp):
 *      엔진별 syscall (io_getevents/io_uring_enter/aio_suspend/poll).
 *      ret<0 → td_verror("td_io_getevents") 후 -1.
 *      ret==0 → 즉시 0 반환 (논블로킹 경로에서 완료 없음).
 *   3. init_icd(ret): icd.time=현재 시각(clat 기준점) 기록.
 *   4. ios_completed: icd.nr 만큼 td->io_ops->event(i) 로 io_u 가져와
 *      io_completed 반복, put_io_u 로 freelist 반환.
 *   5. icd.error → td_verror → -1.
 *   6. io_u_update_bytes_done: td->bytes_done[ddir] += icd.bytes_done[ddir].
 *      verify 모드이면 bytes_verified 에 READ 누적.
 *
 * 실행 컨텍스트: 잡 스레드. 단, async verify 모드에서는 verify 스레드가 자체
 * td로 이 함수 호출.
 *
 * 호출 체인:
 *   backend.c: wait_for_completions → [이 함수]
 *   do_io의 io_in_polling 경로, io_u_quiesce 경로도 사용.
 *
 * 에러 경로: ret<0(ENOMEM/EINVAL/EINTR 등) → td_verror. icd.error(I/O 자체 실패)
 * → td_verror + -1. 둘 다 상위에서 break_on_this_error 판단.
 */
int io_u_queued_complete(struct thread_data *td, int min_evts)
{
	struct io_completion_data icd;
	struct timespec *tvp = NULL;
	int ret;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 0, };

	dprint(FD_IO, "io_u_queued_complete: min=%d\n", min_evts);

	/* [한국어] min_evts=0 은 논블로킹 폴링 — ts={0,0} 포인터를 tvp 로 넘겨
	 * 엔진 getevents 가 블로킹하지 않게 한다(io_getevents 의 timeout 인자). */
	if (!min_evts)
		tvp = &ts;
	/* [한국어] min_evts > cur_depth 는 논리적 오류(없는 io_u 를 기다림 → 영원히
	 * 블로킹). cur_depth 로 클램프해 deadlock 회피. */
	else if (min_evts > td->cur_depth)
		min_evts = td->cur_depth;

	/* No worries, td_io_getevents fixes min and max if they are
	 * set incorrectly */
	/* [한국어] td_io_getevents: 엔진의 .getevents 콜백 호출. libaio=io_getevents,
	 * io_uring=io_uring_enter(GETEVENTS), posixaio=aio_suspend/aio_error 등.
	 * 반환값: 완료 수, 음수=errno 음수화. */
	ret = td_io_getevents(td, min_evts, td->o.iodepth_batch_complete_max, tvp);
	if (ret < 0) {
		td_verror(td, -ret, "td_io_getevents");
		return ret;
	} else if (!ret)
		/* [한국어] 논블로킹 폴링에서 완료 없음 — 정상. */
		return ret;

	/* [한국어] ret 건을 한꺼번에 icd 에 잡고 icd.time 을 배치 기준 시각으로 설정 —
	 * 모든 io_u 의 clat 이 동일 시각 기준(개별 gettime 호출 회피). */
	init_icd(td, &icd, ret);
	ios_completed(td, &icd);
	if (icd.error) {
		td_verror(td, icd.error, "io_u_queued_complete");
		return -1;
	}

	/* [한국어] 배치 바이트 합계를 td->bytes_done 에 누적. */
	io_u_update_bytes_done(td, &icd);

	return ret;
}

/*
 * Call when io_u is really queued, to update the submission latency.
 */
/* [한국어]
 * io_u_queued - ★ 핵심 ★ td_io_queue 성공 시점에 slat(제출 지연) 샘플 기록.
 *
 * @td: 잡 스레드(자식이면 부모로).
 * @io_u: 방금 큐잉된 io_u (issue_time 이 td_io_queue 에서 fio_fill_issue_time 으로 세트됨).
 *
 * slat = issue_time - start_time (io_u 획득 ~ 엔진 제출 직전).
 * clat = completion_time - issue_time.
 * lat  = completion_time - start_time.
 *
 * 조건: !disable_slat && ramp_period_over && td->o.stats.
 *
 * 호출 체인: ioengines.c td_io_queue → [이 함수] (FIO_Q_QUEUED 반환 직후).
 */
void io_u_queued(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 3중 조건: slat 활성 + ramp 통과 + stats 켜짐. 셋 중 하나라도 false
	 * 면 fio_gettime/통계 기록 자체를 skip (IOPs 집약 경로 보호). */
	if (!td->o.disable_slat && ramp_period_over(td) && td->o.stats) {
		/* [한국어] 자식 스레드(verify async 등)의 slat 은 부모 td 의 ts 에 집계 —
		 * 잡 단위 단일 뷰 유지. */
		if (td->parent)
			td = td->parent;
		/* [한국어] add_slat_sample: issue_time - start_time 을 ts.slat_* 에 추가.
		 * start_time 은 get_io_u 에서 찍음, issue_time 은 td_io_queue 에서 찍음. */
		add_slat_sample(td, io_u);
	}
}

/*
 * See if we should reuse the last seed, if dedupe is enabled
 */
/* [한국어]
 * get_buf_state - dedupe_percentage/mode 에 따라 사용할 frand_state 결정 (중복 데이터 생성).
 *
 * @td: 잡 스레드. buf_state/buf_state_prev/buf_state_ret/dedupe_working_set_states[].
 * @return: 이번 채움에 사용할 난수 상태 포인터.
 *
 * dedupe 개념: 같은 시드로 같은 데이터를 만들면 SSD/스토리지의 중복 제거 엔진이
 * 한 번만 저장 → 실제 쓰기 바이트가 줄어 성능/공간 효과 관찰. FIO 는 dedupe
 * 원본 비율을 사용자가 지정하면 그에 맞는 시드 패턴 재사용 전략 적용.
 *
 * 분기:
 *   dedupe_percentage=0 → buf_state (항상 새 데이터).
 *   dedupe_percentage=100 → buf_state_prev 로 복사 후 buf_state 반환 (항상 동일).
 *   1~99% + REPEAT 모드: 확률 적중 시 buf_state_prev 복사본 반환.
 *   1~99% + WORKING_SET 모드: 사전 초기화된 working set 에서 랜덤 페이지 시드 선택.
 *   확률 불적중 → buf_state (새 데이터).
 *
 * 호출 체인: fill_io_buffer → [이 함수] (compress_percentage || dedupe_percentage).
 */
static struct frand_state *get_buf_state(struct thread_data *td)
{
	unsigned int v;
	unsigned long long i;

	/* [한국어] dedupe 비활성 → 항상 buf_state(전진하는 고유 시드) 사용. 매 호출
	 * 마다 다른 데이터 — SSD dedupe 엔진이 중복 감지 안 함. */
	if (!td->o.dedupe_percentage)
		return &td->buf_state;
	else if (td->o.dedupe_percentage == 100) {
		/* [한국어] 100% dedupe: 모든 버퍼가 동일해야 함. buf_state 가 전진해
		 * 버리면 다음엔 다른 데이터가 되어 dedupe 실패 → prev 에 현재를 백업한
		 * 뒤 save_buf_state 에서 다시 prev 로 복원해 사실상 고정. */
		frand_copy(&td->buf_state_prev, &td->buf_state);
		return &td->buf_state;
	}

	/* [한국어] 1~100 난수 v: v ≤ dedupe_percentage 이면 이 버퍼는 "중복" 대상. */
	v = rand_between(&td->dedupe_state, 1, 100);

	if (v <= td->o.dedupe_percentage)
		switch (td->o.dedupe_mode) {
		case DEDUPE_MODE_REPEAT:
			/*
			* The caller advances the returned frand_state.
			* A copy of prev should be returned instead since
			* a subsequent intention to generate a deduped buffer
			* might result in generating a unique one
			*/
			/* [한국어] REPEAT: 직전 고유 시드(buf_state_prev)를 _ret 에 복사해
			 * 반환. 호출자가 이 시드를 전진시키더라도 원본 prev 는 유지 —
			 * 이후 dedupe 버퍼 생성 시 또다시 prev 재사용 가능. */
			frand_copy(&td->buf_state_ret, &td->buf_state_prev);
			return &td->buf_state_ret;
		case DEDUPE_MODE_WORKING_SET:
			/* [한국어] WORKING_SET: 사전 초기화된 num_unique_pages 개 고유
			 * 시드 풀에서 랜덤 선택 — 여러 개 중복 쌍이 공존하는 현실적
			 * dedupe 패턴(예: 같은 파일의 여러 복사본) 재현. */
			i = rand_between(&td->dedupe_working_set_index_state, 0, td->num_unique_pages - 1);
			frand_copy(&td->buf_state_ret, &td->dedupe_working_set_states[i]);
			return &td->buf_state_ret;
		default:
			log_err("unexpected dedupe mode %u\n", td->o.dedupe_mode);
			assert(0);
		}

	/* [한국어] 확률 불적중 → 새 고유 데이터. buf_state 가 매번 전진해 유일성 확보. */
	return &td->buf_state;
}

/* [한국어]
 * save_buf_state - 버퍼 채움 후 frand 상태를 보존 — verify 패턴 재현 근거.
 *
 * @td: 잡 스레드.
 * @rs: 방금 fill_random_buf 에 사용된 상태 포인터.
 *
 * 로직:
 *   dedupe_percentage=100 → rs 자리를 buf_state_prev 로 되돌림(REPEAT 보장).
 *   그 외 rs == &td->buf_state → buf_state_prev 에 복사 (다음 REPEAT 분기 준비).
 *   이 메커니즘이 verify 경로에서도 "같은 시드로 같은 패턴" 재생성하도록 한다.
 *
 * 호출 체인: fill_io_buffer → [이 함수].
 */
static void save_buf_state(struct thread_data *td, struct frand_state *rs)
{
	/* [한국어] 100% dedupe: 이전 상태를 현재 상태에서 복원 */
	if (td->o.dedupe_percentage == 100)
		frand_copy(rs, &td->buf_state_prev);
	/* [한국어] 새 데이터를 생성한 경우: 현재 상태를 이전 상태로 저장 */
	else if (rs == &td->buf_state)
		frand_copy(&td->buf_state_prev, rs);
}

/* [한국어]
 * fill_io_buffer - 쓰기 버퍼를 정책에 맞춰 채움(compress/dedupe/pattern/zero/random).
 *
 * @td: 잡 스레드.
 * @buf: 채울 버퍼 시작 주소.
 * @min_write: 최소 쓰기 단위(compress_chunk 경계 계산).
 * @max_bs: 채울 전체 크기(바이트).
 *
 * 우선순위(가장 먼저 매칭되는 정책 사용):
 *  1) compress_percentage || dedupe_percentage → 청크 루프 +
 *     fill_random_buf_percentage (perc% 는 압축 가능 패턴, 나머지는 랜덤).
 *     get_buf_state/save_buf_state 로 dedupe 시드 재사용.
 *  2) buffer_pattern_bytes > 0 → fill_buffer_pattern (사용자 지정 바이트열 반복).
 *  3) zero_buffers → memset(0).
 *  4) 기본 → fill_random_buf (Tausworthe 기반 랜덤).
 *
 * 예외: mem_type=CUDA_MALLOC 이면 호스트에서 채울 수 없어 no-op (libcufile 엔진이
 * GPU 쪽에서 별도 초기화).
 *
 * 호출 체인:
 *   io_u_fill_buffer → [이 함수] (REFILL_BUFFERS 세트 시 get_io_u 경로)
 *   init_io_u_buffers (backend.c 초기화) → [이 함수]
 */
void fill_io_buffer(struct thread_data *td, void *buf, unsigned long long min_write,
		    unsigned long long max_bs)
{
	struct thread_options *o = &td->o;

	/* [한국어] MEM_CUDA_MALLOC: libcufile 엔진 전용 GPU 메모리. 호스트에서
	 * memcpy/patterned fill 불가(CUDA 커널 경로 필요) → 엔진 자체에서 별도
	 * 초기화하도록 no-op. */
	if (o->mem_type == MEM_CUDA_MALLOC)
		return;

	if (o->compress_percentage || o->dedupe_percentage) {
		/* [한국어] 압축/dedupe 혼합 모드 — compress_chunk 단위로 루프. 각 청크마다
		 * perc% 비율만큼 "압축 가능한 패턴", 나머지는 랜덤을 혼합. dedupe 시드
		 * 재사용을 위해 get_buf_state/save_buf_state 쌍 사용. */
		unsigned int perc = td->o.compress_percentage;
		struct frand_state *rs = NULL;
		unsigned long long left = max_bs;
		unsigned long long this_write;

		do {
			/*
			 * Buffers are either entirely dedupe-able or not.
			 * If we choose to dedup, the buffer should undergo
			 * the same manipulation as the original write. Which
			 * means we should retrack the steps we took for compression
			 * as well.
			 */
			/* [한국어] rs 는 첫 청크에서만 결정 — 같은 io_u 안의 모든 청크는
			 * 동일 dedupe 결정을 공유해야 "전체 버퍼가 dedupe 인가/아닌가"
			 * 이분법 유지. 중간에 바뀌면 SSD dedupe 엔진이 부분만 인식. */
			if (!rs)
				rs = get_buf_state(td);

			min_write = min(min_write, left);

			/* [한국어] 이번 청크 길이 = min(min_write, compress_chunk).
			 * compress_chunk=0(미설정) 이면 min_not_zero 가 min_write 그대로 사용. */
			this_write = min_not_zero(min_write,
						(unsigned long long) td->o.compress_chunk);

			/* [한국어] fill_random_buf_percentage: 내부에서 perc% 의 바이트를
			 * buffer_pattern(고정 반복 패턴) 으로 채우고 나머지는 랜덤 —
			 * 적당한 압축률 달성. dedupe 동일 시드 재사용으로 같은 버퍼
			 * 만들면 SSD dedupe 엔진이 중복 감지. */
			fill_random_buf_percentage(rs, buf, perc,
				this_write, this_write,
				o->buffer_pattern,
				o->buffer_pattern_bytes);

			buf += this_write;
			left -= this_write;
			/* [한국어] 청크마다 시드 상태 저장 — 다음 쓰기 dedupe 시 이 시드 재사용 근거. */
			save_buf_state(td, rs);
		} while (left);
	} else if (o->buffer_pattern_bytes)
		/* [한국어] 고정 바이트 패턴 반복 — 사용자가 "0xdeadbeef" 등 지정.
		 * 압축률은 100% 근접(완전 반복). verify 와 자연 호환. */
		fill_buffer_pattern(td, buf, max_bs);
	else if (o->zero_buffers)
		/* [한국어] 전부 0 — 일부 SSD가 zero 는 실제 쓰기 없이 처리(TRIM 유사 효과). */
		memset(buf, 0, max_bs);
	else
		/* [한국어] 기본 — Tausworthe 랜덤. 비압축/비dedupe 일반 워크로드 재현. */
		fill_random_buf(get_buf_state(td), buf, max_bs);
}

/*
 * "randomly" fill the buffer contents
 */
/* [한국어]
 * io_u_fill_buffer - io_u 별 버퍼 채움 래퍼 + buf_filled_len 리셋.
 *
 * @td: 잡 스레드.
 * @io_u: 대상 I/O 유닛 (io_u->buf 에 채움).
 * @min_write/max_bs: fill_io_buffer 로 전달.
 *
 * buf_filled_len=0 리셋: 이 필드는 "현재 버퍼에 채워진 바이트 수"를 기억해
 * 다음 write 에서 같은 크기면 재채움 생략하는 최적화용. 여기서 리셋하면
 * 호출 직후 fill_io_buffer 가 전체 채움을 수행한다.
 *
 * 호출 체인:
 *   get_io_u (TD_F_REFILL_BUFFERS) → [이 함수]
 *   backend.c init_io_u_buffers (잡 초기화) → [이 함수]
 *   verify.c 의 verify 버퍼 준비 경로.
 */
void io_u_fill_buffer(struct thread_data *td, struct io_u *io_u,
		      unsigned long long min_write, unsigned long long max_bs)
{
	io_u->buf_filled_len = 0;
	fill_io_buffer(td, io_u->buf, min_write, max_bs);
}

/* [한국어]
 * do_sync_file_range - file_log_write_comp 가 추적한 dirty 범위만 부분 sync.
 *
 * @td: 잡 스레드(const). td->o.sync_file_range 가 SYNC_FILE_RANGE_* 플래그 비트.
 * @f: 대상 파일. first_write..last_write 가 전 마지막 SYNC 이후 쓴 범위.
 * @return: sync_file_range(2) 반환값 (성공 0 / 실패 -1/errno).
 *
 * Linux sync_file_range(fd, offset, nbytes, flags) 플래그:
 *   SYNC_FILE_RANGE_WAIT_BEFORE (1): 시작 전 기존 쓰기 완료 대기.
 *   SYNC_FILE_RANGE_WRITE (2): 쓰기 시작.
 *   SYNC_FILE_RANGE_WAIT_AFTER (4): 완료 대기.
 * fsync 와 달리 inode 메타데이터는 동기화 안 함.
 *
 * 호출 체인: do_io_u_sync (DDIR_SYNC_FILE_RANGE) → [이 함수].
 */
static int do_sync_file_range(const struct thread_data *td,
			      struct fio_file *f)
{
	uint64_t offset, nbytes;

	offset = f->first_write;
	nbytes = f->last_write - f->first_write;

	if (!nbytes)
		return 0;

	return sync_file_range(f->fd, offset, nbytes, td->o.sync_file_range);
}

/* [한국어]
 * do_io_u_sync - ddir 에 따라 적절한 sync 계열 syscall 디스패치.
 *
 * @td: 잡 스레드(const).
 * @io_u: DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE/SYNCFS 중 하나.
 * @return: syscall 반환값(0 또는 바이트 수). 음수면 io_u->error=errno 세트.
 *
 * 분기:
 *   DDIR_SYNC:
 *     CONFIG_FCNTL_SYNC → fcntl(fd, F_FULLFSYNC) [macOS: 실제 디스크 물리 플러시]
 *     그 외 → fsync(fd) [Linux: VFS 레이어까지 완전 동기]
 *   DDIR_DATASYNC:
 *     CONFIG_FDATASYNC → fdatasync(fd) [메타데이터 제외 데이터만 동기]
 *     아니면 xfer_buflen 돌려주고 EINVAL(실제 I/O 는 수행 안 함, 통계상 건너뜀).
 *   DDIR_SYNC_FILE_RANGE → do_sync_file_range(부분 동기).
 *   DDIR_SYNCFS → syncfs(fd) [파일시스템 전체 동기 — Linux only].
 *
 * 호출 체인: engines/sync.c, engines/libaio.c (SYNCIO 폴백) 등 → [이 함수].
 */
int do_io_u_sync(const struct thread_data *td, struct io_u *io_u)
{
	int ret;

	if (io_u->ddir == DDIR_SYNC) {
#ifdef CONFIG_FCNTL_SYNC
		/* [한국어] macOS: F_FULLFSYNC 는 실제 물리 디스크 플러시 요청.
		 * 일반 fsync 는 FS 캐시까지만 flush 하고 장치 write cache 는 안 비움. */
		ret = fcntl(io_u->file->fd, F_FULLFSYNC);
#else
		/* [한국어] Linux: fsync(2) — 데이터+메타데이터 모두 디스크에 동기. */
		ret = fsync(io_u->file->fd);
#endif
	} else if (io_u->ddir == DDIR_DATASYNC) {
#ifdef CONFIG_FDATASYNC
		/* [한국어] fdatasync(2): 메타데이터(mtime 등) 제외, 데이터 블록만 동기.
		 * 파일 크기 변경 등 꼭 필요한 메타는 여전히 포함됨. */
		ret = fdatasync(io_u->file->fd);
#else
		/* [한국어] 플랫폼 미지원 — "성공한 것처럼" xfer_buflen 반환 + EINVAL 표기.
		 * 잡이 멈추지 않도록 완료 처리 흉내, 하지만 에러로 기록. */
		ret = io_u->xfer_buflen;
		io_u->error = EINVAL;
#endif
	} else if (io_u->ddir == DDIR_SYNC_FILE_RANGE) {
		/* [한국어] sync_file_range(2): first_write..last_write 부분 범위만 flush —
		 * fsync 보다 비용 낮음, 메타데이터 미동기. */
		ret = do_sync_file_range(td, io_u->file);
	} else if (io_u->ddir == DDIR_SYNCFS) {
		/* [한국어] syncfs(2) [Linux 2.6.39+]: 해당 fd 가 속한 파일시스템 전체
		 * 동기 — 같은 FS 의 다른 파일도 flush. 테스트에서 전역 상태 리셋에 유용. */
		ret = syncfs(io_u->file->fd);
	} else {
		/* [한국어] 알 수 없는 ddir — 상위 로직 버그. 통계용으로 xfer_buflen 반환. */
		ret = io_u->xfer_buflen;
		io_u->error = EINVAL;
	}

	/* [한국어] syscall 음수 반환 → errno 에 실제 에러. io_u->error 에 전파해 상위
	 * 에러 핸들링 경로 활성화. */
	if (ret < 0)
		io_u->error = errno;

	return ret;
}

/* [한국어]
 * do_io_u_trim - TRIM(BLKDISCARD/ FALLOC_PUNCH_HOLE) I/O 수행.
 *
 * @td: 잡 스레드.
 * @io_u: DDIR_TRIM io_u (offset/xfer_buflen 이 trim 범위).
 * @return: 성공 시 trim 바이트 수(io_u->xfer_buflen), 실패 시 0 + io_u->error.
 *
 * ZBD 모드: zbd_do_io_u_trim 이 먼저 처리 — ZNS 의 reset_wp 를 trim 의미로 사용.
 *   io_u_completed 반환 시 즉시 완료로 처리(xfer_buflen 반환).
 * 일반 모드: os_trim (os/os-linux.h 의 BLKDISCARD ioctl 또는 fallocate
 *   PUNCH_HOLE|KEEP_SIZE — 파일 종류에 따라 os.c 에서 자동 분기).
 *
 * FIO_HAVE_TRIM 미정의 플랫폼(windows/solaris 일부) → EINVAL.
 *
 * 호출 체인: engines/sync.c 의 fio_io_end, libaio 의 DDIR_TRIM 동기 폴백 등.
 */
int do_io_u_trim(struct thread_data *td, struct io_u *io_u)
{
#ifndef FIO_HAVE_TRIM
	/* [한국어] FIO_HAVE_TRIM 미정의 플랫폼(Windows/Solaris 등) — trim 지원 없음.
	 * io_u->error=EINVAL 표기 + 0 반환(상위에서 에러 경로). */
	io_u->error = EINVAL;
	return 0;
#else
	struct fio_file *f = io_u->file;
	int ret;

	/* [한국어] ZBD(ZNS): trim 의 의미가 "reset_wp" (zone write pointer 초기화) —
	 * 일반 BLKDISCARD 가 아닌 존 리셋 명령. zbd_do_io_u_trim 이 존 상태 기반 처리. */
	if (td->o.zone_mode == ZONE_MODE_ZBD) {
		ret = zbd_do_io_u_trim(td, io_u);
		/* [한국어] io_u_completed: ZBD 쪽에서 완전히 처리됨 (OS 레벨 trim 스킵). */
		if (ret == io_u_completed)
			return io_u->xfer_buflen;
		if (ret)
			goto err;
	}

	/* [한국어] os_trim: 플랫폼별 trim 구현 래퍼(os/os-linux.h 등).
	 *  - 블록 디바이스: BLKDISCARD ioctl (NVMe 는 커널이 DSM 명령 발행).
	 *  - 일반 파일: fallocate(FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE).
	 * 반환 0 = 성공, 양수 = errno. */
	ret = os_trim(f, io_u->offset, io_u->xfer_buflen);
	if (!ret)
		return io_u->xfer_buflen;

err:
	/* [한국어] 실패 경로 공용: io_u->error 에 errno 기록 후 0(바이트 0 처리) 반환. */
	io_u->error = ret;
	return 0;
#endif
}
