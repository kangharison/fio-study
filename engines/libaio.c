/*
 * libaio engine
 *
 * IO engine using the Linux native aio interface.
 *
 */
/*
 * [한국어 설명] Linux 네이티브 AIO(libaio) 기반 I/O 엔진 (libaio.c)
 *
 * === 파일의 역할 ===
 * 리눅스 커널의 AIO 시스콜(io_setup(2)/io_submit(2)/io_getevents(2)/io_cancel(2)/
 * io_destroy(2))을 얇게 래핑한 유저스페이스 라이브러리 libaio(3)를 이용해 fio의
 * --ioengine=libaio 엔진을 구현한다. fio의 I/O 경로는 "prep → queue → commit →
 * getevents → event" 5단계로 분리되는데, 본 파일은 다음과 같이 매핑한다:
 *   - prep()     : io_u를 io_prep_pread/pwrite/preadv/pwritev/fsync 매크로로 iocb에 채움.
 *   - queue()    : libaio_data의 iocbs[]/io_us[] 링 버퍼에 포인터만 적재(아직 커널 미제출).
 *   - commit()   : io_submit(aio_ctx, nr, iocbs+tail)로 일괄 제출 → 단일 syscall로 수십 개
 *                  iocb를 내려 syscall 엔트리 비용을 분산시키는 것이 libaio의 핵심 이점.
 *   - getevents(): io_getevents(2) 블로킹 대기(또는 userspace_reap=1일 때 aio_ring
 *                  유저 매핑을 직접 polling)로 완료 이벤트를 수확.
 *   - event()    : 완료된 io_event의 obj(=iocb*)에서 container_of로 io_u를 역추적해
 *                  error/resid로 환산한 뒤 fio 코어에 반환.
 * 이 엔진은 RWF_NOWAIT, RWF_ATOMIC(oatomic), I/O 우선순위(cmdprio: percentage/bssplit),
 * 사용자 공간 링 직접 reap, libaio_vectored(preadv/pwritev), libaio <0.3.111 호환 매핑 등
 * libaio가 제공하는 대부분의 옵션을 지원한다. DDIR_TRIM과 DDIR_SYNCFS는 libaio에
 * 비동기 opcode가 없어 동기 폴백(BLKDISCARD ioctl / fsync)으로 처리되고, 플래그로
 * FIO_ASYNCIO_SYNC_TRIM·FIO_ASYNCIO_SYNC_SYNCFS를 등록해 fio 코어에 이를 알린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=libaio로 선택되면 fio_init 생성자(__attribute__((constructor))로 확장)가
 * main() 진입 전에 register_ioengine(&ioengine)을 호출해 전역 engine_list에 삽입한다.
 * 실행 흐름:
 *   backend.c(fio_backend/thread_main)
 *      → td_io_init(ioengines.c)
 *         → fio_libaio_init: libaio_data와 iocbs[]/io_us[]/aio_events[]/iovecs[] 할당,
 *                            fio_cmdprio_init으로 ioprio 정책 구성.
 *         → fio_libaio_post_init: io_queue_init(iodepth, &aio_ctx) → 내부 io_setup(2)으로
 *                                 커널에 iodepth만큼 aio-nr을 차감하고 AIO 링 페이지를 mmap.
 *      → 메인 I/O 루프: get_io_u → td_io_prep(=fio_libaio_prep) → td_io_queue(=fio_libaio_queue)
 *        → (링 포화/배치) td_io_commit(=fio_libaio_commit)
 *        → td_io_getevents(=fio_libaio_getevents) → per-event td_io_event(=fio_libaio_event)
 *        → account_io_completion (stat.c).
 *      → td_io_cleanup(=fio_libaio_cleanup) → io_destroy(2)로 링 해제.
 *   실행 컨텍스트: 단일 잡 스레드(유저스페이스). 부모/자식 프로세스 분기(TD_F_CHILD)에서는
 *   cleanup 경로에 특수 처리(parent가 exit_aio로 병렬 io_destroy → RCU stall 회피).
 *
 * === 타 모듈과의 연결 ===
 * 상단 의존자: fio 코어 — backend.c(잡 스케줄), ioengines.c(플러그인 디스패치),
 *              io_u.c(io_u 생명주기), stat.c(lat/bw/iops 누적), verify.c(데이터 검증),
 *              cmdprio.c(본 엔진에 ioprio를 주입하는 공유 유틸 — libaio/io_uring/sg 공용).
 * 하단 의존자: libaio.so 3종 심볼군 —
 *              (a) 컨텍스트 관리: io_queue_init/io_destroy(각각 io_setup(2)/io_destroy(2) 래퍼).
 *              (b) iocb 헬퍼: io_prep_pread/pwrite/preadv/pwritev/fsync/fdsync — 모두 매크로로
 *                  iocb의 aio_fildes/aio_lio_opcode/aio_buf/aio_nbytes/aio_offset 필드 채움.
 *              (c) 제출/수확: io_submit/io_getevents/io_cancel — 각각 동명의 syscall 번호
 *                  (__NR_io_submit/__NR_io_getevents/__NR_io_cancel) 래핑.
 *              커널 측: fs/aio.c의 ioctx → aio_ring mmap, __io_submit_one, aio_complete.
 *              RWF_* UAPI(linux/fs.h): NOWAIT, HIPRI, APPEND, UNCACHED, ATOMIC.
 *              IOCB_FLAG_IOPRIO로 aio_reqprio 적용 활성화.
 * 데이터 흐름:
 *   io_u (fio)
 *     └ io_u->iocb (embed)  ──prep()──▶  io_prep_* 채움
 *                           ──queue()─▶  libaio_data.iocbs[head] 적재
 *                           ──commit()▶  io_submit(ctx, n, iocbs+tail)
 *                                        │
 *                                        ▼
 *                                      커널 VFS → 블록 계층 → 디바이스 드라이버
 *                                        │
 *                                      AIO 링(events[tail]) 기록 + tail 전진
 *                                        │
 *                           ◀─getevents()─  io_getevents 또는 user_io_getevents
 *                           ◀─event()────   ev->obj (iocb*) → container_of → io_u
 *                                           ev->res → io_u->error / io_u->resid
 * 공유 상태:
 *   io_context_t(aio_ctx): 커널 fs/aio.c의 kioctx와 1:1 매핑. 유저 쪽에서는
 *                          struct aio_ring*으로 reinterpret 가능(userspace_reap 경로).
 *   AIO 링 매직: AIO_RING_MAGIC = 0xa10a10a1. 일치해야 userspace reap 안전.
 *   cmdprio: libaio/io_uring/sg가 공유하는 외부 유틸. mode=NONE/PERC/BSSPLIT.
 *
 * === 주요 함수/구조체 요약 ===
 * - ring_inc(): 링 인덱스 래핑 (is_pow2면 AND 마스크, 아니면 모듈로).
 * - fio_libaio_prep(): io_u의 ddir/vectored/nowait/oatomic을 보고 iocb 채움.
 * - fio_libaio_cmdprio_prep(): cmdprio 정책에 따라 io_u->ioprio → iocb.aio_reqprio.
 * - fio_libaio_queue(): DDIR_TRIM/SYNCFS는 동기 폴백, 나머지는 링에 적재 후 FIO_Q_QUEUED.
 * - fio_libaio_queued(): io_submit 성공분에 issue_time 기록(slat/clat 분리 계산 기반).
 * - fio_libaio_commit(): io_submit 반복 호출, EAGAIN/ENOMEM/EINTR 재시도 전략 포함.
 * - fio_libaio_getevents(): io_getevents 또는 user_io_getevents로 min..max 수확.
 * - fio_libaio_event(): event[i].obj → io_u 역추적, ev->res 해석(부분/에러/성공).
 * - user_io_getevents(): aio_ring을 memory-read로 수확(시스콜 생략, non-blocking only).
 * - fio_libaio_init()/post_init()/cleanup(): 자원 수명 관리.
 * - struct libaio_data: aio_ctx + iocbs/io_us/aio_events/iovecs 링 + head/tail/queued +
 *                       cmdprio 상태. 잡 스레드 전용(락 불필요).
 * - struct libaio_options: userspace_reap, nowait, vectored, cmdprio_*.
 * - struct aio_ring: 커널 AIO 완료 링의 유저 매핑 뷰 — magic/head/tail/events[].
 * - struct ioengine_ops ioengine: init/post_init/prep/queue/commit/getevents/event/cleanup.
 */

/* 표준 라이브러리 헤더 */
#include <stdlib.h>
/* [한국어] C 표준 라이브러리: calloc/free 공급.
 * - calloc(n, sz): 0-초기화 연속 메모리 — libaio_data, iocbs[], io_us[], aio_events[], iovecs[]에 사용.
 * - free(ptr):      cleanup 경로에서 위 포인터들 역순 해제.
 * fio_libaio_init/cleanup 경로의 메모리 수명 관리 핵심 의존. */
#include <unistd.h>
/* [한국어] POSIX 기본 API: usleep 공급.
 * - usleep(us): commit의 EAGAIN 재시도에서 1us busy-wait, getevents의 재시도 전 10us sleep.
 *   커널 AIO 슬롯이 회복되기를 기다리는 짧은 백오프로만 사용(30s 상한으로 stall 감지). */
#include <errno.h>
/* [한국어] errno 심볼 카탈로그. libaio 함수들은 음수 -errno를 직접 반환하므로
 * EAGAIN(큐 포화), EINTR(시그널), ENOMEM(메모리 부족), EINVAL(잘못된 파라미터) 상수가 필요.
 * 본 파일에서는 io_getevents/io_submit 반환값을 이 상수들과 직접 비교. */
#include <libaio.h>
/* [한국어] Linux AIO 유저스페이스 래퍼 라이브러리 헤더(libaio-dev).
 * 공급 심볼:
 *   - 타입: io_context_t(불투명 포인터), struct iocb(aio_fildes/aio_lio_opcode/aio_buf/
 *           aio_nbytes/aio_offset/aio_reqprio/aio_rw_flags/u.c.flags/__pad2),
 *           struct io_event(data/obj/res/res2), IO_CMD_PREAD/PWRITE/PREADV/PWRITEV/FSYNC/FDSYNC.
 *   - 컨텍스트: io_queue_init/io_destroy(io_setup(2)/io_destroy(2) 래퍼).
 *   - 준비 매크로(iocb 필드 채움): io_prep_pread/pwrite/preadv/pwritev/fsync/fdsync.
 *   - 제출/수확: io_submit/io_getevents/io_cancel — 동명의 syscall 직행.
 * libaio는 커널 AIO 인터페이스의 얇은 래퍼라, 본 파일은 거의 대부분을 직접 호출한다. */
#include <sys/time.h>
/* [한국어] struct timeval/timespec 타입 및 gettimeofday류 헤더.
 * fio_gettime이 내부에서 사용하는 시간 구조체 정의 경로로 포함.
 * 본 파일에서 직접 호출하는 API는 없지만 타임아웃 인자(struct timespec *)가
 * io_getevents 시그니처에 나타나 필요. */
#include <sys/resource.h>
/* [한국어] rlimit 관련 헤더(RLIMIT_NOFILE/RLIMIT_MEMLOCK 등 상수 공급).
 * 본 파일은 직접 setrlimit을 호출하지 않지만, fio 코어가 post_init 이전에 rlimit을
 * 조정해 io_setup(2)의 aio-max-nr 충돌을 줄이는 배경이므로 관련 타입이 노출되도록 포함. */

/* fio 내부 헤더 */
#include "../fio.h"
/* [한국어] fio 핵심 헤더 — thread_data(td), io_u, fio_file, ioengine_ops, DDIR_*,
 * FIO_Q_* 반환 매크로, ddir_rw/ddir_sync/fio_ro_check/io_u_mark_submit/io_u_mark_complete,
 * do_io_u_trim/do_io_u_sync, container_of, fio_init/fio_exit GCC 속성 매크로,
 * fio_gettime/mtime_since_now, generic_open_file/generic_close_file/generic_get_file_size
 * 등 본 엔진이 참조하는 거의 모든 fio 기반 심볼 공급. */
#include "../lib/pow2.h"
/* [한국어] is_power_of_2(x) — 2의 거듭제곱 판별 인라인.
 * 본 파일에선 ld->entries(=iodepth)에 대해 1회 평가하여 is_pow2에 캐시 →
 * 이후 ring_inc가 AND 마스크(빠른 경로)와 모듈로(안전 경로)를 분기. */
#include "../optgroup.h"
/* [한국어] fio 옵션 그룹 매크로(FIO_OPT_C_ENGINE, FIO_OPT_G_LIBAIO 등) 공급.
 * options[] 각 엔트리의 .category/.group 필드가 이 헤더의 상수에 의존.
 * --enghelp/--cmdhelp의 도움말 그룹핑 기준이 된다. */
#include "../lib/memalign.h"
/* [한국어] 정렬된 메모리 할당 유틸.
 * 본 파일은 직접 호출하지 않지만, fio 코어가 io_u->xfer_buf를 블록 디바이스의 logical_block_size에
 * 맞춰 정렬 할당하도록 유지하는 맥락(O_DIRECT 요구)을 명시적으로 공유하기 위해 포함. */
#include "cmdprio.h"
/* [한국어] libaio/io_uring/sg 공유의 명령 우선순위(cmdprio) 유틸.
 * 공급 심볼:
 *   - struct cmdprio, struct cmdprio_options, enum cmdprio_mode(NONE/PERC/BSSPLIT 등).
 *   - fio_cmdprio_init/cleanup/set_ioprio, CMDPRIO_OPTIONS 매크로.
 * libaio_queue에서 mode != NONE일 때 prep hook로 호출해 iocb.aio_reqprio + IOCB_FLAG_IOPRIO 세팅. */

/* Should be defined in newest aio_abi.h */
/* [한국어] 최신 linux/aio_abi.h에 정의되어야 할 플래그의 호환성 폴백.
 * IOCB_FLAG_IOPRIO = (1<<1): iocb.u.c.flags에 세팅하면 커널이 aio_reqprio 필드를 읽어
 *   BIO의 ioprio에 전파한다. 설정하지 않으면 aio_reqprio는 무시되고 프로세스 기본 prio 사용.
 * 구형 커널 헤더에 이 매크로가 없을 수 있어 직접 정의한다(ABI 값 (1<<1)은 고정). */
#ifndef IOCB_FLAG_IOPRIO
#define IOCB_FLAG_IOPRIO    (1 << 1)
#endif

/* Hack for libaio < 0.3.111 */
/* [한국어] libaio 0.3.111 미만 호환 처리.
 * 구버전 헤더는 iocb 구조체에 aio_rw_flags 필드 대신 __pad2 패딩만 존재.
 * 이 #define으로 "aio_rw_flags" 식별자를 __pad2로 텍스트 치환하여 빌드 가능하게 한다.
 * CONFIG_LIBAIO_RW_FLAGS는 configure 스크립트가 헤더 피처 검사 후 정의 — 신버전 빌드에서는
 * 이 블록이 비활성화되고 진짜 aio_rw_flags 필드가 사용된다. RWF_NOWAIT/ATOMIC 등 UAPI 플래그는
 * __pad2 자리와 같은 워드에 배치되므로 ABI 상 안전. */
#ifndef CONFIG_LIBAIO_RW_FLAGS
#define aio_rw_flags __pad2
#endif

/* [한국어] 함수 전방 선언 (forward declaration).
 * fio_libaio_commit: fio_libaio_getevents()가 재시도 경로에서 "링에 남은 큐를 먼저 제출"
 *                    하기 위해 호출 — 정의가 뒤에 있어 전방선언 필요.
 * fio_libaio_init:   ioengine_ops 초기화에 앞서 포인터 지정용. */
static int fio_libaio_commit(struct thread_data *td);
static int fio_libaio_init(struct thread_data *td);

/*
 * [한국어] libaio 엔진의 핵심 데이터 구조체
 *
 * 이 구조체는 fio의 thread_data->io_ops_data에 저장되어
 * libaio 엔진의 모든 상태를 관리한다.
 */
struct libaio_data {
	io_context_t aio_ctx;
	/* [한국어] 커널이 발급한 AIO 컨텍스트 핸들 (불투명 포인터 타입).
	 * 설정자: fio_libaio_post_init()의 io_queue_init() → 내부적으로 io_setup(2)
	 *         시스콜이 호출되어 커널 내 AIO 링 페이지가 mmap 매핑되고 그 주소가 반환된다.
	 * 읽는 자: io_submit(2), io_getevents(2), io_destroy(2), io_cancel(2), 그리고
	 *          userspace reap 모드에서는 struct aio_ring*로 캐스팅되어 직접 접근됨.
	 * 값 범위: 성공 시 커널이 매핑한 유저 주소(0이 아님). 실패 시 설정되지 않음.
	 * 동기화: 한 잡 스레드가 전용으로 소유 — 다른 스레드와 공유하지 않음. 커널 측
	 *         aio_ring의 head/tail은 atomic_store_release/load_acquire로만 접근한다. */

	struct io_event *aio_events;
	/* [한국어] io_getevents(2) 또는 user_io_getevents()가 채워넣는 완료 이벤트 배열.
	 * 설정자: fio_libaio_init()에서 calloc(entries, sizeof(io_event))로 할당.
	 *         fio_libaio_getevents()가 호출할 때마다 커널/링으로부터 덮어쓰여짐.
	 * 읽는 자: fio_libaio_event(td, idx) — aio_events[idx]의 res/res2/obj를 참조해
	 *          대응 io_u의 error/resid를 설정하고 fio 코어에 반환.
	 * 값 범위: 배열 크기는 정확히 entries(=iodepth). 각 요소는 {data, obj, res, res2}.
	 *          res>=0이면 전송 바이트 수, res<0이면 -errno.
	 * 동기화: 단일 잡 스레드에서만 접근되므로 락 불필요. */

	struct iocb **iocbs;
	/* [한국어] io_submit(2)에 넘겨줄 iocb 포인터들의 링 버퍼(제출 대기 큐).
	 * 설정자: fio_libaio_queue()가 iocbs[head] = &io_u->iocb로 포인터를 등록하며 head++.
	 * 읽는 자: fio_libaio_commit()가 iocbs + tail부터 연속 구간을 io_submit에 전달.
	 * 값 범위: 배열 크기 entries. 원소는 해당 슬롯이 대기 중일 때만 유효한 iocb 포인터.
	 * 동기화: 단일 잡 스레드 전용. head는 queue()에서만, tail은 commit()에서만 갱신. */

	struct io_u **io_us;
	/* [한국어] iocbs[]와 1:1 인덱스 대응하는 io_u 포인터 링 버퍼.
	 * 설정자: fio_libaio_queue()에서 io_us[head] = io_u로 기록.
	 * 읽는 자: fio_libaio_queued()가 io_submit 성공 후 [tail, tail+ret) 구간을 돌며
	 *          issue_time(제출 시각)을 찍는다. 수확은 iocb→container_of로 역추적.
	 * 값 범위: 배열 크기 entries. 유효 구간은 [tail, head) (링 래핑 적용).
	 * 동기화: 단일 잡 스레드 전용. */

	struct io_u **io_u_index;
	/* [한국어] 현재 libaio 엔진에서는 사용되지 않는 예비/레거시 포인터.
	 * 설정자/읽는 자: 없음 (calloc조차 하지 않음).
	 * 값 범위: 항상 NULL로 남아있는 0-초기화 필드.
	 * 동기화: 접근 없음. */

	struct iovec *iovecs;		/* for vectored requests */
	/* [한국어] libaio_vectored=1일 때 preadv/pwritev에 전달할 iovec 배열.
	 * 설정자: fio_libaio_init()에서 calloc(entries, sizeof(iovec))로 확보.
	 *         fio_libaio_prep()에서 io_u->index를 인덱스로 iov_base/iov_len을 채움.
	 * 읽는 자: io_prep_preadv/pwritev가 iocb 내부에 이 iovec의 주소와 개수를 저장 →
	 *          io_submit 시 커널이 이를 참조하여 scatter/gather I/O를 수행.
	 * 값 범위: 배열 크기 entries, 각 원소는 {iov_base=xfer_buf, iov_len=xfer_buflen}.
	 * 동기화: io_u.index가 전역적으로 유일하므로 잡 스레드 내 경합 없음. */

	/*
	 * Basic ring buffer. 'head' is incremented in _queue(), and
	 * 'tail' is incremented in _commit(). We keep 'queued' so
	 * that we know if the ring is full or empty, when
	 * 'head' == 'tail'. 'entries' is the ring size, and
	 * 'is_pow2' is just an optimization to use AND instead of
	 * modulus to get the remainder on ring increment.
	 */
	/*
	 * [한국어] 기본 링 버퍼 구조:
	 * - 'head': _queue()에서 증가 - 새 I/O 요청이 추가되는 위치
	 * - 'tail': _commit()에서 증가 - 커널에 제출된 I/O의 위치
	 * - 'queued': head == tail일 때 링이 꽉 찼는지 비었는지 구별하기 위한 카운터
	 * - 'entries': 링 버퍼의 크기 (= iodepth 값)
	 * - 'is_pow2': entries가 2의 거듭제곱이면 true.
	 *   모듈로(%) 연산 대신 AND(&) 비트 연산으로 링 인덱스를 계산하여 성능 최적화.
	 *
	 * 링 버퍼 동작 예시 (entries=4):
	 *   queue() 호출 → iocbs[head]에 저장, head++, queued++
	 *   commit() 호출 → iocbs[tail]부터 io_submit(), tail++, queued--
	 */
	int is_pow2;
	/* [한국어] entries가 2의 거듭제곱인지 판별한 캐시 값.
	 * 설정자: fio_libaio_init()에서 is_power_of_2(entries) 결과로 1회 고정.
	 * 읽는 자: ring_inc() — true면 AND 마스크(& entries-1) 경로, false면 모듈로 경로.
	 * 값 범위: 0 또는 1. 1일 때 링 인덱스 갱신이 한 사이클에 끝나는 빠른 경로.
	 * 동기화: 초기화 이후 read-only. */

	unsigned int entries;
	/* [한국어] 링 버퍼 용량(슬롯 개수) = td->o.iodepth (사용자 지정 큐 깊이).
	 * 설정자: fio_libaio_init()에서 td->o.iodepth 복사.
	 * 읽는 자: ring_inc(), fio_libaio_commit()의 wrap 보호(entries - tail).
	 * 값 범위: 1 이상 정수. 너무 크면 io_setup 단계에서
	 *          /proc/sys/fs/aio-max-nr 한도에 걸려 실패할 수 있음.
	 * 동기화: 초기화 후 read-only. */

	unsigned int queued;
	/* [한국어] "queue()에 쌓였지만 아직 io_submit이 끝나지 않은" 요청 수.
	 * 설정자: fio_libaio_queue()에서 ++, fio_libaio_commit()에서 성공분만큼 --.
	 * 읽는 자: queue()가 iodepth 포화 여부 판정, commit() 루프 지속 조건,
	 *          EAGAIN/ENOMEM 시 "다른 대기 요청 존재 여부"로 에러 처리 분기.
	 * 값 범위: [0, entries]. head == tail 조건과 결합해 빈/참 상태 구분.
	 * 동기화: 단일 잡 스레드 전용이므로 원자성 불필요. */

	unsigned int head;
	/* [한국어] 링 버퍼의 생산자(producer) 인덱스 — 다음에 iocb를 넣을 슬롯.
	 * 설정자: fio_libaio_queue() 말미의 ring_inc(ld, &head, 1).
	 * 읽는 자: queue()에서 iocbs[head]/io_us[head] 접근 시 사용.
	 * 값 범위: [0, entries-1]. 래핑 방식은 is_pow2에 따라 AND 또는 MOD.
	 * 동기화: 단일 잡 스레드 전용. */

	unsigned int tail;
	/* [한국어] 링 버퍼의 소비자(consumer) 인덱스 — 다음에 io_submit으로 내보낼 슬롯.
	 * 설정자: fio_libaio_commit()가 io_submit 성공 반환 수(ret)만큼 ring_inc.
	 * 읽는 자: commit()가 iocbs+tail, io_us+tail로 제출 구간 기준점을 잡음.
	 * 값 범위: [0, entries-1]. tail ≤ head (링에서 모듈로 거리).
	 * 동기화: 단일 잡 스레드 전용. */

	struct cmdprio cmdprio;
	/* [한국어] 요청별 I/O 우선순위(cmdprio) 정책 상태.
	 * 설정자: fio_libaio_init()의 fio_cmdprio_init()이 옵션 파싱 결과로 초기화.
	 * 읽는 자: fio_libaio_cmdprio_prep()가 mode/percentage를 보고 ioprio 결정 후
	 *          iocb.aio_reqprio 및 IOCB_FLAG_IOPRIO 세팅.
	 * 값 범위: mode ∈ {NONE, PERCENT, BSSPLIT, ...}. 상세는 cmdprio.h 참고.
	 * 동기화: 잡 스레드 내 단독 소유. 내부 난수 소비에 별도 락 불필요. */
};

/*
 * [한국어] libaio 엔진 옵션 구조체
 *
 * fio 설정 파일에서 사용자가 지정할 수 있는 libaio 엔진 전용 옵션들.
 * 예: userspace_reap=1, nowait=1, libaio_vectored=1
 */
struct libaio_options {
	struct thread_data *td;
	/* [한국어] 옵션 구조체에서 thread_data로 되돌아가는 역포인터.
	 * 설정자: fio 옵션 파서(parse.c)가 옵션 구조체를 td마다 할당할 때 세팅.
	 * 읽는 자: fio_option 콜백(cb)이 옵션 변경 시점에 td 컨텍스트에 접근해야 할 때.
	 * 값 범위: 유효한 thread_data* (NULL 아님).
	 * 동기화: 옵션은 잡 시작 전에 파싱되어 이후 read-only. */

	unsigned int userspace_reap;
	/* [한국어] 1이면 io_getevents(2) 대신 aio_ring을 유저스페이스에서 직접 polling.
	 * 설정자: FIO_OPT_STR_SET 파서가 "userspace_reap" 옵션 등장 시 1로 세팅.
	 * 읽는 자: fio_libaio_getevents()가 actual_min==0일 때 분기 선택에 사용.
	 * 값 범위: 0 또는 1. 1일 때 시스콜 진입 비용이 사라지지만, 블로킹 대기는 불가.
	 * 동기화: read-only. */

	struct cmdprio_options cmdprio_options;
	/* [한국어] cmdprio 하위 옵션(class, percentage, bssplit 등)을 담는 집합 구조체.
	 * 설정자: CMDPRIO_OPTIONS() 매크로가 확장한 개별 옵션들을 fio 파서가 채움.
	 * 읽는 자: fio_cmdprio_init()가 ld->cmdprio로 변환/검증하는 입력.
	 * 값 범위: cmdprio.h 정의에 의존.
	 * 동기화: read-only. */

	unsigned int nowait;
	/* [한국어] 1이면 각 iocb에 RWF_NOWAIT 플래그를 세팅해 비차단 I/O를 요청.
	 * 설정자: FIO_OPT_BOOL 파서가 "nowait=1"을 인식할 때 1로 세팅.
	 * 읽는 자: fio_libaio_prep()가 DDIR_READ/WRITE 분기 내에서 iocb->aio_rw_flags에 OR.
	 * 값 범위: 0 또는 1. 1이면 페이지 캐시 미스/잠금 경합 시 -EAGAIN 반환.
	 * 동기화: read-only. */

	unsigned int vectored;
	/* [한국어] 1이면 io_prep_preadv/pwritev 경로(벡터 I/O)를 선택.
	 * 설정자: FIO_OPT_BOOL 파서가 "libaio_vectored=1" 인식 시 1.
	 * 읽는 자: fio_libaio_prep()가 분기에서 확인, iovecs[io_u->index] 사용.
	 * 값 범위: 0 또는 1. iov_count는 1로 고정이라 성능 이득보다 API 테스트 용도.
	 * 동기화: read-only. */
};

/*
 * [한국어] fio 옵션 배열 정의
 *
 * fio의 옵션 파싱 시스템에 libaio 엔진 전용 옵션들을 등록.
 * 각 옵션은 parse.c의 fio_options_parse에서 순회되어 --name=value 또는 잡 파일의
 * "name=value" 라인과 매칭된다. .off1이 가리키는 libaio_options 필드에 값을 저장.
 *
 * 공통 필드 규약:
 *   .name      : 옵션 식별자(명령줄/잡 파일에서 사용).
 *   .lname     : "긴 이름" — fio --enghelp 출력/문서화용.
 *   .type      : FIO_OPT_STR_SET(값 없이 이름만으로 1), FIO_OPT_BOOL(0/1/true/false), FIO_OPT_INT 등.
 *   .off1      : libaio_options 구조체 내 바이트 오프셋(offsetof).
 *   .help      : --cmdhelp 출력에 쓰이는 한 줄 설명.
 *   .category  : FIO_OPT_C_ENGINE — 엔진 전용 옵션 카테고리.
 *   .group     : FIO_OPT_G_LIBAIO — "libaio 엔진 그룹" 분류(--enghelp 그룹핑).
 *   cmdprio_*는 CMDPRIO_OPTIONS 매크로로 일괄 확장 — cmdprio.h에 정의.
 */
static struct fio_option options[] = {
	{
		/* [한국어] userspace_reap 옵션:
		 * 사용자 공간 이벤트 수확(reap) 활성화.
		 * 커널의 AIO 완료 링 버퍼(aio_ring)를 직접 memory-read로 소비하여
		 * io_getevents(2) 시스템 콜 엔트리/메모리 배리어 비용을 제거한다.
		 * 단, 유저스페이스에서 "새 이벤트 도착 대기"를 할 수 없으므로
		 * 논블로킹(actual_min==0) 경로에서만 사용. */
		.name	= "userspace_reap",
		/* [한국어] 명령줄/잡 파일에서 사용하는 식별자. */
		.lname	= "Libaio userspace reaping",
		/* [한국어] 긴 이름(도움말/문서 출력용). */
		.type	= FIO_OPT_STR_SET,
		/* [한국어] STR_SET = 값 없이 이름 등장만으로 1 세팅. */
		.off1	= offsetof(struct libaio_options, userspace_reap),
		/* [한국어] 파싱 결과가 기록될 libaio_options.userspace_reap 오프셋. */
		.help	= "Use alternative user-space reap implementation",
		/* [한국어] fio --cmdhelp userspace_reap에 출력되는 한 줄 도움말. */
		.category = FIO_OPT_C_ENGINE,
		/* [한국어] 엔진 전용 옵션 카테고리 — --enghelp에 그룹핑. */
		.group	= FIO_OPT_G_LIBAIO,
		/* [한국어] libaio 엔진 서브그룹 식별자. */
	},
	{
		/* [한국어] nowait 옵션:
		 * RWF_NOWAIT 플래그를 모든 read/write 요청의 iocb->aio_rw_flags에 OR.
		 * I/O가 페이지 캐시 미스, 블록 계층 잠금 경합, 큐 포화 등으로 즉시
		 * 처리할 수 없을 때 블로킹 대기 대신 -EAGAIN을 반환시켜 지연 시간에
		 * 민감한 워크로드에서 재시도/백오프 로직을 구현 가능하게 함.
		 * 커널 지원: Linux 4.14+ (block), 4.19+ (일부 파일시스템). */
		.name	= "nowait",
		/* [한국어] 옵션 식별자. */
		.lname	= "RWF_NOWAIT",
		/* [한국어] 대응하는 UAPI 플래그 상수 이름. */
		.type	= FIO_OPT_BOOL,
		/* [한국어] BOOL = 0/1/true/false/yes/no. */
		.off1	= offsetof(struct libaio_options, nowait),
		/* [한국어] libaio_options.nowait 오프셋. */
		.help	= "Set RWF_NOWAIT for reads/writes",
		/* [한국어] 도움말 텍스트. */
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		/* [한국어] libaio_vectored 옵션:
		 * 각 요청을 io_prep_pread/pwrite 대신 io_prep_preadv/pwritev로 준비.
		 * iocb.aio_lio_opcode가 IO_CMD_PREADV/PWRITEV로 설정되고 aio_buf가
		 * iovec 배열 포인터로 대체됨. 본 엔진은 iov_count=1로 고정 — 벡터 I/O
		 * 코드 경로 자체를 테스트/벤치마크하기 위한 옵션. */
		.name	= "libaio_vectored",
		/* [한국어] 옵션 식별자(엔진 접두어 사용 — 네임스페이스 충돌 회피). */
		.lname	= "Use libaio preadv,pwritev",
		/* [한국어] 긴 이름. */
		.type	= FIO_OPT_BOOL,
		/* [한국어] BOOL. */
		.off1	= offsetof(struct libaio_options, vectored),
		/* [한국어] libaio_options.vectored 오프셋. */
		.help	= "Use libaio {preadv,pwritev} instead of libaio {pread,pwrite}",
		/* [한국어] 도움말. */
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},

	/* [한국어] 명령 우선순위(cmdprio) 관련 옵션 일괄 등록 매크로.
	 * CMDPRIO_OPTIONS(struct libaio_options, FIO_OPT_G_LIBAIO)는 cmdprio.h에서
	 * cmdprio, cmdprio_class, cmdprio_percentage, cmdprio_hint, cmdprio_bssplit 등의
	 * 엔트리를 이 배열 가운데에 전개한다. 각 엔트리는 libaio_options.cmdprio_options의
	 * 해당 필드(offsetof 내부 매크로)에 값을 기록. libaio/io_uring/sg가 동일 매크로를
	 * 공유해 코드 중복 없이 우선순위 옵션을 제공. */
	CMDPRIO_OPTIONS(struct libaio_options, FIO_OPT_G_LIBAIO),
	{
		.name	= NULL,
		/* [한국어] NULL 이름 = 배열 종료 sentinel. parse.c 순회가 여기서 멈춘다. */
	},
};

/*
 * [한국어] ring_inc - 링 버퍼 인덱스 증가(래핑 포함)
 *
 * @param ld:  libaio_data 포인터. entries/is_pow2 메타데이터 소스.
 * @param val: &ld->head 또는 &ld->tail — in-place로 갱신될 대상 인덱스.
 * @param add: 증가량(보통 1 또는 io_submit 성공 개수).
 * @return: 없음(val이 직접 수정됨).
 *
 * 실행 컨텍스트: 단일 잡 스레드(유저스페이스). 동기화 불필요.
 * 호출 체인:
 *   fio_libaio_queue() → [ring_inc] (head 전진)
 *   fio_libaio_commit() → [ring_inc] (tail 전진)
 *
 * 성능 최적화:
 *   entries가 2의 거듭제곱일 때 (*val + add) & (entries - 1)로 모듈로 회피.
 *   예: entries=8, mask=7=0b0111, (5+4)&7 = 1.
 * 에러 경로: 없음 (인자가 잘못되면 상위에서 필터링됨).
 */
static inline void ring_inc(struct libaio_data *ld, unsigned int *val,
			    unsigned int add)
{
	if (ld->is_pow2)
		/* [한국어] 2의 거듭제곱: AND 비트 마스크로 빠르게 래핑 */
		*val = (*val + add) & (ld->entries - 1);
	else
		/* [한국어] 2의 거듭제곱이 아닌 경우: 모듈로 연산으로 래핑 */
		*val = (*val + add) % ld->entries;
}

/*
 * [한국어] fio_libaio_prep - io_u를 Linux AIO용 iocb로 변환하는 ioengine_ops.prep 콜백
 *
 * @param td:   fio 잡의 thread_data. td->eo로 libaio_options, td->io_ops_data로 libaio_data 접근.
 * @param io_u: 준비할 I/O 유닛. ddir/offset/xfer_buf/xfer_buflen/file 필드가 확정된 상태.
 * @return: 항상 0 (현재 구현상 실패 경로 없음).
 *
 * 배경: libaio는 io_submit(2)에 iocb 배열을 넘겨야 하므로, 각 io_u에 내장된
 * iocb 필드(opcode, fd, buf, nbytes, offset, aio_rw_flags, aio_reqprio)를 채워야 한다.
 * 이 작업을 queue() 전에 완료해 커널 제출 경로를 가볍게 유지한다.
 *
 * 동작:
 *   - DDIR_READ: vectored 옵션에 따라 io_prep_preadv 또는 io_prep_pread.
 *   - DDIR_WRITE: 동일 분기. RWF_ATOMIC(+oatomic) 지원 빌드에선 원자 쓰기 플래그.
 *   - ddir_sync: io_prep_fsync (비동기 fsync).
 *   - nowait 옵션: aio_rw_flags |= RWF_NOWAIT (블로킹 대기 대신 -EAGAIN 반환 유도).
 *
 * 실행 컨텍스트: 잡 스레드(유저스페이스), td_io_prep 경로에서 한 io_u당 1회.
 * 호출 체인:
 *   backend.c 루프 → td_io_prep(ioengines.c) → [fio_libaio_prep]
 *                                              → io_prep_pread/pwrite/preadv/pwritev/fsync (libaio 헬퍼)
 * 에러 경로: 없음. 잘못된 ddir은 상위에서 필터링.
 *
 * Linux AIO 헬퍼 요약:
 *   io_prep_pread/pwrite/preadv/pwritev/fsync — aio_abi.h의 iocb 필드 초기화.
 */
static int fio_libaio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_options *o = td->eo;         /* [한국어] 엔진 옵션 가져오기 */
	struct fio_file *f = io_u->file;           /* [한국어] I/O 대상 파일 */
	struct iocb *iocb = &io_u->iocb;           /* [한국어] io_u에 내장된 iocb 구조체 포인터 */
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 (iovecs 접근용) */

	/* [한국어] I/O 방향에 따라 iocb를 적절히 준비 */
	if (io_u->ddir == DDIR_READ) {
		/* [한국어] === 읽기 요청 === */
		if (o->vectored) {
			/* [한국어] 벡터 I/O 모드: preadv 사용 */
			struct iovec *iov = &ld->iovecs[io_u->index];
			/* [한국어] iovec에 버퍼 주소와 크기 설정 */
			iov->iov_base = io_u->xfer_buf;
			iov->iov_len = (size_t)io_u->xfer_buflen;
			/* [한국어] iocb를 벡터 읽기(preadv)용으로 설정.
			 * 파라미터: iocb, fd, iov배열, iov개수, 오프셋 */
			io_prep_preadv(iocb, f->fd, iov, 1, io_u->offset);
		} else {
			/* [한국어] 일반 I/O 모드: pread 사용 */
			/* [한국어] iocb를 일반 읽기(pread)용으로 설정.
			 * 파라미터: iocb, fd, 버퍼, 크기, 오프셋 */
			io_prep_pread(iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen,
						  io_u->offset);
		}
		if (o->nowait)
			/* [한국어] nowait 옵션이 설정되어 있으면 RWF_NOWAIT 플래그 추가.
			 * I/O가 즉시 처리될 수 없으면 블로킹 대신 EAGAIN 반환. */
			iocb->aio_rw_flags |= RWF_NOWAIT;
	} else if (io_u->ddir == DDIR_WRITE) {
		/* [한국어] === 쓰기 요청 === */
		if (o->vectored) {
			/* [한국어] 벡터 I/O 모드: pwritev 사용 */
			struct iovec *iov = &ld->iovecs[io_u->index];

			iov->iov_base = io_u->xfer_buf;
			iov->iov_len = (size_t)io_u->xfer_buflen;
			/* [한국어] iocb를 벡터 쓰기(pwritev)용으로 설정 */
			io_prep_pwritev(iocb, f->fd, iov, 1, io_u->offset);
		} else {
			/* [한국어] 일반 I/O 모드: pwrite 사용 */
			io_prep_pwrite(iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen,
						   io_u->offset);
		}
		if (o->nowait)
			/* [한국어] 쓰기에도 RWF_NOWAIT 적용 가능 */
			iocb->aio_rw_flags |= RWF_NOWAIT;
#ifdef FIO_HAVE_RWF_ATOMIC
		/* [한국어] RWF_ATOMIC 지원 시, oatomic 옵션이 설정되어 있으면
		 * 원자적(atomic) 쓰기 플래그를 추가.
		 * 이는 쓰기가 원자적으로 수행되어 부분 쓰기(partial write)가 없음을 보장. */
		if (td->o.oatomic)
			iocb->aio_rw_flags |= RWF_ATOMIC;
#endif
	} else if (ddir_sync(io_u->ddir))
		/* [한국어] === 동기화 요청 (fsync/fdatasync) ===
		 * iocb를 비동기 fsync용으로 설정. 파일의 데이터를 디스크에 플러시. */
		io_prep_fsync(iocb, f->fd);

	return 0;  /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어] fio_libaio_cmdprio_prep - I/O별 ioprio를 iocb에 주입(인라인 헬퍼)
 *
 * @param td:   잡 thread_data (td->io_ops_data에서 libaio_data 획득).
 * @param io_u: 우선순위 적용 대상 I/O 유닛.
 * @return: 없음 (io_u->iocb 필드만 수정).
 *
 * 배경: Linux 블록 계층의 ioprio를 AIO 경로에서도 사용하려면 iocb.aio_reqprio에
 * 우선순위 값을 넣고 IOCB_FLAG_IOPRIO를 set해야 커널이 이를 BIO에 전파한다.
 *
 * 동작: fio_cmdprio_set_ioprio()가 cmdprio 정책(percentage/bssplit 등)에 따라
 *       이 io_u에 우선순위를 적용할지 결정하고 io_u->ioprio를 채우면,
 *       iocb 필드 두 개(aio_reqprio, u.c.flags)에 복사.
 *
 * 실행 컨텍스트: 잡 스레드, fio_libaio_queue()에서 cmdprio.mode != NONE일 때만 호출.
 * 호출 체인: fio_libaio_queue() → [fio_libaio_cmdprio_prep] → fio_cmdprio_set_ioprio()
 * 에러 경로: 없음.
 */
static inline void fio_libaio_cmdprio_prep(struct thread_data *td,
					   struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;   /* [한국어] 엔진 데이터 가져오기 */
	struct cmdprio *cmdprio = &ld->cmdprio;     /* [한국어] cmdprio 설정 가져오기 */

	/* [한국어] 이 io_u에 대해 우선순위를 적용할지 판단하고, 적용할 경우 설정 */
	if (fio_cmdprio_set_ioprio(td, cmdprio, io_u)) {
		io_u->iocb.aio_reqprio = io_u->ioprio;          /* [한국어] iocb에 I/O 우선순위 값 설정 */
		io_u->iocb.u.c.flags |= IOCB_FLAG_IOPRIO;       /* [한국어] 우선순위 사용 플래그 활성화 */
	}
}

/*
 * [한국어] fio_libaio_event - ioengine_ops.event 콜백 (완료 이벤트 인덱스 → io_u)
 *
 * @param td:    잡 thread_data.
 * @param event: fio_libaio_getevents()가 이전 호출에서 수확한 aio_events[] 인덱스 (0-base).
 * @return: 해당 완료 이벤트에 대응하는 io_u 포인터. error/resid는 res 기반으로 갱신됨.
 *
 * 배경: fio 코어는 getevents()가 N개를 수확했다고 보고받으면 i=0..N-1로 event(td, i)를
 * 호출해 각 io_u를 확인한다. 이 함수는 커널이 기록한 완료 정보(io_event)를
 * fio가 이해하는 io_u 에러/잔여바이트 표현으로 변환한다.
 *
 * 동작:
 *   1) ev = &ld->aio_events[event]
 *   2) io_u = container_of(ev->obj, struct io_u, iocb) — iocb는 io_u 내 임베디드.
 *   3) ev->res가 요청 길이와 다르면:
 *       - res > xfer_buflen: 사실상 음수(-errno)가 unsigned로 해석된 것 → io_u->error = -ev->res
 *       - res < xfer_buflen: 부분 완료(short I/O) → resid에 미전송 바이트 수
 *      같으면 error=0.
 *
 * 실행 컨텍스트: 잡 스레드, getevents() 직후.
 * 호출 체인: backend → td_io_getevents 루프 → [fio_libaio_event] → io_u 상태 갱신.
 * 에러 경로: ev->obj가 유효하지 않으면 container_of가 잘못된 포인터 반환(발생해선 안 됨).
 *
 * io_event 필드: {data, obj(=iocb*), res, res2}. POSIX AIO와 달리 유저가 명시 제공.
 */
static struct io_u *fio_libaio_event(struct thread_data *td, int event)
{
	struct libaio_data *ld = td->io_ops_data;   /* [한국어] 엔진 데이터 가져오기 */
	struct io_event *ev;                         /* [한국어] 완료 이벤트 포인터 */
	struct io_u *io_u;                           /* [한국어] 반환할 io_u 포인터 */

	/* [한국어] 이벤트 배열에서 해당 인덱스의 이벤트 가져오기 */
	ev = ld->aio_events + event;
	/* [한국어] iocb 포인터(ev->obj)에서 io_u 포인터를 역추적.
	 * container_of(ptr, type, member): ptr이 type 구조체의 member 필드를 가리킬 때,
	 * type 구조체의 시작 주소를 계산하는 매크로. */
	io_u = container_of(ev->obj, struct io_u, iocb);

	/* [한국어] I/O 결과 확인 */
	if (ev->res != io_u->xfer_buflen) {
		/* [한국어] 전송된 바이트 수가 요청한 크기와 다른 경우 */
		if (ev->res > io_u->xfer_buflen)
			/* [한국어] res가 요청 크기보다 크면 에러 코드임 (음수 에러가 unsigned로 표현됨).
			 * 예: -EIO가 매우 큰 양수로 나타남. 부호를 반전하여 에러 코드로 저장. */
			io_u->error = -ev->res;
		else
			/* [한국어] res가 요청 크기보다 작으면 부분 완료(short I/O).
			 * resid에 남은(미전송) 바이트 수를 기록. */
			io_u->resid = io_u->xfer_buflen - ev->res;
	} else
		/* [한국어] 전송된 바이트 수가 요청한 크기와 정확히 일치 → 성공 */
		io_u->error = 0;

	return io_u;  /* [한국어] 처리된 io_u 반환 */
}

/*
 * [한국어] 커널 AIO 링 버퍼 구조체 (사용자 공간 접근용)
 *
 * 커널의 AIO 구현은 내부적으로 링 버퍼를 사용하여 완료 이벤트를 관리한다.
 * 이 링 버퍼는 io_context_t가 가리키는 메모리 영역에 매핑되어 있으며,
 * userspace_reap 옵션이 활성화되면 io_getevents() 시스템 콜 없이
 * 이 구조체를 직접 읽어서 완료 이벤트를 수확할 수 있다.
 *
 * 이는 커널 내부 구현에 의존하는 것으로, 공식 API가 아님에 주의.
 * AIO_RING_MAGIC 값으로 이 구조체가 유효한지 검증한다.
 */
struct aio_ring {
	unsigned id;		 /** kernel internal index number */
	/* [한국어] 커널 AIO 컨텍스트의 내부 식별자 (fs/aio.c에서 부여).
	 * 설정자: io_setup(2) 성공 시 커널이 AIO 링 첫 페이지에 기록.
	 * 읽는 자: 유저스페이스에서는 디버깅 외에는 직접 사용하지 않음.
	 * 값 범위: 커널 내부 의미. 유저는 해석하지 않는다.
	 * 동기화: 커널이 쓰고 유저가 읽는 read-only 메타 필드. */

	unsigned nr;		 /** number of io_events */
	/* [한국어] 링이 보관할 수 있는 io_event 슬롯 수 (io_setup의 maxevents 기반).
	 * 설정자: 커널의 aio_setup_ring()이 페이지 크기에 맞춰 계산 후 기록.
	 * 읽는 자: user_io_getevents()의 (head + 1) % ring->nr — 링 래핑 계산에 필수.
	 * 값 범위: maxevents 이상(정렬/헤더 때문에 약간 더 클 수 있음).
	 * 동기화: read-only. */

	unsigned head;
	/* [한국어] 소비자(유저스페이스) 측 인덱스 — 다음에 읽을 이벤트 위치.
	 * 설정자: user_io_getevents()가 atomic_store_release로 (head+1)%nr 진행.
	 * 읽는 자: 커널이 "링에 빈 공간이 있는지" 판단할 때 읽음. 유저는 루프 조건에 사용.
	 * 값 범위: [0, nr-1].
	 * 동기화: release 스토어로 업데이트 — 직전 events[head] 읽기가 완료된 후에만
	 *         커널이 관측하도록 메모리 순서를 강제. */

	unsigned tail;
	/* [한국어] 생산자(커널) 측 인덱스 — 커널이 다음에 완료 이벤트를 써넣을 위치.
	 * 설정자: 커널이 I/O 완료 시 events[tail]에 기록하고 tail을 증가.
	 * 읽는 자: user_io_getevents()가 head == tail로 "빈 링" 여부를 확인.
	 * 값 범위: [0, nr-1].
	 * 동기화: 커널은 acquire로 head를 읽고 release로 tail을 증가시킨다. */

	unsigned magic;
	/* [한국어] 링 유효성 검증용 매직 넘버 (AIO_RING_MAGIC = 0xa10a10a1).
	 * 설정자: io_setup(2) 시 커널이 기록.
	 * 읽는 자: fio_libaio_getevents()가 userspace reap 분기 진입 전 검사.
	 * 값 범위: 정상일 때 0xa10a10a1 고정. 불일치면 userspace reap 불가(시스콜 폴백).
	 * 동기화: read-only. */

	unsigned compat_features;
	/* [한국어] 무시해도 되는 호환 기능 비트 필드.
	 * 설정자: 커널이 설정. 읽는 자: 특정 기능 감지 시 사용(여기선 미사용).
	 * 값 범위: 기능 비트마스크. 동기화: read-only. */

	unsigned incompat_features;
	/* [한국어] 인식하지 못하면 링을 사용하면 안 되는 필수 기능 비트 필드.
	 * 설정자: 커널. 읽는 자: 안전한 링 접근을 요구하는 라이브러리. 여기선 미검사.
	 * 값 범위: 기능 비트마스크. 동기화: read-only. */

	unsigned header_length;	/** size of aio_ring */
	/* [한국어] aio_ring 헤더(이 구조체의 고정부) 크기(바이트).
	 * 설정자: 커널. 읽는 자: events[] 실제 시작 오프셋 계산이 필요할 때.
	 * 값 범위: 통상 >= sizeof(struct aio_ring). 동기화: read-only. */

	struct io_event events[0];
	/* [한국어] 유연한 배열 멤버(C99 FAM) — 실제 완료 이벤트들이 연속 배치되는 구간.
	 * 설정자: 커널이 I/O 완료 시 events[tail] 슬롯에 {data, obj, res, res2} 기록.
	 * 읽는 자: user_io_getevents()가 events[head]를 복사.
	 * 값 범위: 인덱스 [0, nr-1]. 각 원소는 struct io_event.
	 * 동기화: tail/head의 release/acquire 쌍으로 순서 보호. */
};

/* [한국어] AIO 링 버퍼 유효성 검증을 위한 매직 넘버.
 * "a10a10a1" - "AIO AIO A1"을 연상시키는 값. */
#define AIO_RING_MAGIC	0xa10a10a1

/*
 * [한국어] user_io_getevents - io_getevents 시스콜을 우회한 유저스페이스 링 폴링
 *
 * @param aio_ctx: io_setup으로 받은 컨텍스트. 실상은 aio_ring 첫 페이지의 유저 매핑 주소.
 * @param max:     이번 호출에서 최대 수확할 이벤트 수 (ld->aio_events 잔여 공간).
 * @param events:  수확 결과를 기록할 호출자 소유 io_event 배열.
 * @return: 실제로 링에서 복사한 이벤트 개수 (0..max).
 *
 * 배경: AIO 링 페이지는 io_setup 시 커널이 프로세스 주소 공간에 매핑해주므로,
 * head == tail 체크와 events[head] 복사만으로 시스콜 없이 완료를 수확할 수 있다.
 * 고 IOPS(수백만 IOPS급) 워크로드에서 시스콜 엔트리/메모리 배리어 비용을 제거해 유용.
 *
 * 제약: 이 경로는 "대기"를 할 수 없다 — tail이 진전되지 않으면 즉시 0 반환.
 *       따라서 호출 측은 actual_min==0(논블로킹)일 때만 이 경로를 선택한다.
 *
 * 실행 컨텍스트: 잡 스레드. 링은 커널-유저 양쪽이 touch하므로 release/acquire 필수.
 * 호출 체인: fio_libaio_getevents() (조건: userspace_reap=1 && magic 매치) → [user_io_getevents]
 * 에러 경로: 자체 실패 없음. aio_ctx가 잘못되면 상위의 magic 검사에서 걸린다.
 *
 * 링 동작:
 *   커널: 완료 시 events[tail] 기록 → tail을 release 스토어로 증가.
 *   유저: events[head] 복사 → head를 release 스토어로 증가(커널이 다음 write 가능하도록).
 */
static int user_io_getevents(io_context_t aio_ctx, unsigned int max,
			     struct io_event *events)
{
	long i = 0;                                              /* [한국어] 수확한 이벤트 카운터 */
	unsigned head;                                           /* [한국어] 현재 head 위치 */
	struct aio_ring *ring = (struct aio_ring*) aio_ctx;      /* [한국어] AIO 컨텍스트를
	                                                          * aio_ring 구조체로 캐스팅.
	                                                          * io_context_t는 실제로 이 링 버퍼의
	                                                          * 메모리 주소를 가리킴. */

	/* [한국어] 최대 max개까지 완료 이벤트를 수확하는 루프 */
	while (i < max) {
		head = ring->head;  /* [한국어] 현재 소비 위치(head) 읽기 */

		if (head == ring->tail) {
			/* There are no more completions */
			/* [한국어] head == tail: 더 이상 완료된 이벤트가 없음. 루프 종료. */
			break;
		} else {
			/* There is another completion to reap */
			/* [한국어] 완료된 이벤트가 있으므로 수확 */
			events[i] = ring->events[head];  /* [한국어] 링에서 이벤트를 출력 배열로 복사 */
			atomic_store_release(&ring->head,
					     (head + 1) % ring->nr);
			/* [한국어] head를 다음 위치로 이동.
			 * atomic_store_release: 메모리 배리어를 포함하여
			 * 이벤트 데이터 읽기가 head 업데이트 전에 완료됨을 보장.
			 * 이는 커널이 head를 읽고 새 이벤트를 추가할 때
			 * 데이터 경쟁(race condition)을 방지. */
			i++;
		}
	}

	return i;  /* [한국어] 실제 수확한 이벤트 수 반환 */
}

/*
 * [한국어] fio_libaio_getevents - ioengine_ops.getevents 콜백 (완료 이벤트 수집 루프)
 *
 * @param td:  잡 thread_data.
 * @param min: 반드시 수확해야 할 최소 이벤트 수. 0이면 논블로킹.
 * @param max: 이번 호출에서 수확 가능한 최대 이벤트 수(=td->cur_depth 이하).
 * @param t:   io_getevents 타임아웃 timespec. NULL이면 min개 도달까지 무한 대기.
 * @return: 수확한 이벤트 수(>=0) 또는 음수 -errno.
 *
 * 배경: 커널 AIO는 완료가 비동기로 발생하므로 최소/최대 제약을 지키며 대기/폴링한다.
 * userspace_reap 옵션이 켜지고 논블로킹 모드이며 링 매직이 유효하면 시스콜 없이 수확.
 *
 * 핵심 동작:
 *   1) 타임아웃 원본 보호를 위해 지역 복사본 lt 사용.
 *   2) 루프:
 *      a) 조건 충족 시 user_io_getevents()로 링 직접 읽기.
 *      b) 아니면 io_getevents(ctx, actual_min, max-events, buf+events, lt).
 *      c) r>0이면 events에 누적, actual_min 감소.
 *      d) r==0 또는 -EAGAIN이면 fio_libaio_commit()으로 대기 요청 추가 제출하고
 *         (actual_min>0일 때) 10us sleep 후 재시도.
 *      e) -EINTR면 재시도, 그 외 에러는 루프 탈출.
 *   3) events >= min이 될 때까지 반복.
 *
 * 실행 컨텍스트: 잡 스레드(유저스페이스). 시그널 수신 시 EINTR 경로 존재.
 * 호출 체인: backend → td_io_getevents → [fio_libaio_getevents]
 *              → io_getevents(2) 또는 user_io_getevents → (필요 시) fio_libaio_commit
 * 에러 경로: io_getevents의 -errno(예: -EFAULT, -EINVAL)를 그대로 상향 반환.
 *
 * fio 흐름 상 위치: queue() → commit() → [getevents] → event() (per-iter).
 */
static int fio_libaio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct libaio_data *ld = td->io_ops_data;    /* [한국어] 엔진 데이터 가져오기 */
	struct libaio_options *o = td->eo;           /* [한국어] 엔진 옵션 가져오기 */
	/* [한국어] actual_min: 실제 최소 수확 수.
	 * iodepth_batch_complete_min이 0이면 논블로킹 모드(actual_min=0),
	 * 아니면 min 값 사용 (최소 min개 수확할 때까지 대기). */
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct timespec __lt, *lt = NULL;            /* [한국어] 로컬 타임아웃 변수 */
	int r, events = 0;                           /* [한국어] r: 각 호출의 반환값, events: 누적 수확 수 */

	/* [한국어] 타임아웃이 지정되었으면 로컬 복사본 생성.
	 * io_getevents()가 t를 수정할 수 있으므로 원본을 보호. */
	if (t) {
		__lt = *t;
		lt = &__lt;
	}

	/* [한국어] 최소 min개의 이벤트를 수확할 때까지 반복하는 메인 루프 */
	do {
		if (o->userspace_reap == 1
		    && actual_min == 0
		    && ((struct aio_ring *)(ld->aio_ctx))->magic
				== AIO_RING_MAGIC) {
			/* [한국어] === 사용자 공간 수확 모드 ===
			 * 조건: userspace_reap 활성화 AND 논블로킹 모드(actual_min==0)
			 *       AND AIO 링 매직 넘버 유효
			 * io_getevents() 시스템 콜 없이 링 버퍼를 직접 읽음.
			 * 주의: actual_min > 0 (블로킹)일 때는 시스템 콜을 사용해야 함.
			 *       사용자 공간에서는 새 이벤트를 기다리며 블로킹할 수 없으므로. */
			r = user_io_getevents(ld->aio_ctx, max - events,
				ld->aio_events + events);
		} else {
			/* [한국어] === 커널 시스템 콜 모드 ===
			 * io_getevents(ctx, min_nr, max_nr, events, timeout)
			 * - 최소 actual_min개, 최대 (max-events)개의 이벤트를 기다림
			 * - 이벤트는 aio_events + events 위치에 저장 (이전 수확분 뒤에 추가)
			 * - lt: 타임아웃 (NULL이면 min_nr개 도착까지 무한 대기) */
			r = io_getevents(ld->aio_ctx, actual_min,
				max - events, ld->aio_events + events, lt);
		}
		if (r > 0) {
			/* [한국어] 이벤트를 성공적으로 수확한 경우 */
			events += r;  /* [한국어] 누적 이벤트 수 증가 */
			/* [한국어] 남은 최소 필요량을 감소.
			 * min()으로 actual_min이 음수(언더플로)가 되는 것을 방지. */
			actual_min -= min((unsigned int)events, actual_min);
		}
		else if ((min && r == 0) || r == -EAGAIN) {
			/* [한국어] 이벤트 없음(0) 또는 EAGAIN 발생.
			 * 아직 대기 중인 요청이 있으면 commit()을 호출하여
			 * 큐에 남아있는 요청을 커널에 제출.
			 * 이후 짧은 대기(10us) 후 재시도. */
			fio_libaio_commit(td);
			if (actual_min)
				usleep(10);  /* [한국어] 10마이크로초 대기 후 재시도 */
		} else if (r != -EINTR)
			/* [한국어] EINTR(시그널 인터럽트)이 아닌 다른 에러면 루프 탈출.
			 * EINTR은 시그널에 의한 일시적 중단이므로 재시도 가능. */
			break;
	} while (events < min);  /* [한국어] min개 이상 수확할 때까지 반복 */

	/* [한국어] 에러 발생 시 에러 코드 반환, 성공 시 수확한 이벤트 수 반환 */
	return r < 0 ? r : events;
}

/*
 * [한국어] fio_libaio_queue - ioengine_ops.queue 콜백 (io_u를 제출 대기 링에 적재)
 *
 * @param td:   잡 thread_data.
 * @param io_u: 이미 prep된 I/O 유닛.
 * @return:
 *   - FIO_Q_QUEUED:    링에 들어갔고 commit() 시 io_submit으로 전달될 예정.
 *   - FIO_Q_BUSY:      링/iodepth 포화 또는 TRIM/SYNCFS 직전 비동기 대기분 잔존 — 호출측이 먼저 reap 필요.
 *   - FIO_Q_COMPLETED: TRIM/SYNCFS를 동기 실행으로 즉시 완료.
 *
 * 배경: 큐잉-배치-제출 분리로 io_submit(2) 호출 횟수를 줄이고, iodepth 만큼의
 * 요청을 한 번에 커널에 내려 throughput을 끌어올리는 libaio 전형 패턴.
 *
 * 동작:
 *   1) fio_ro_check: read-only 위반 체크(쓰기 시 abort).
 *   2) queued == iodepth → FIO_Q_BUSY 반환(완료 먼저 수확 유도).
 *   3) DDIR_TRIM/SYNCFS: libaio 백엔드가 지원하지 않아 동기 호출 후 즉시 완료 마크.
 *      단, 이전에 비동기 요청이 대기 중이면 순서 보호를 위해 FIO_Q_BUSY.
 *   4) cmdprio 활성이면 fio_libaio_cmdprio_prep으로 ioprio 주입.
 *   5) iocbs[head]/io_us[head]에 포인터 저장, head 전진, queued++ → FIO_Q_QUEUED.
 *
 * 실행 컨텍스트: 잡 스레드(유저스페이스).
 * 호출 체인: backend → td_io_queue → [fio_libaio_queue]
 *              → (sync 분기) do_io_u_trim / do_io_u_sync
 *              → (async 분기) ring_inc / fio_libaio_cmdprio_prep
 * 에러 경로: ro 위반은 fio_ro_check가 td 상태를 ERROR로 전환. 링 적재 자체는 실패 없음.
 */
static enum fio_q_status fio_libaio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */

	/* [한국어] 읽기 전용 모드에서 쓰기 요청이 오면 에러 처리 */
	fio_ro_check(td, io_u);

	/* [한국어] 큐가 꽉 찼는지 확인 (현재 대기 수 == iodepth).
	 * 큐가 꽉 차면 FIO_Q_BUSY를 반환하여 fio가 먼저 완료 이벤트를
	 * 수확하도록 유도. */
	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/* [한국어] TRIM(discard)과 SYNCFS는 비동기로 처리할 수 없으므로
	 * 동기적으로 즉시 처리. 단, 이미 큐에 다른 요청이 있으면
	 * 먼저 그것들을 처리해야 하므로 BUSY 반환. */
	if (io_u->ddir == DDIR_TRIM || io_u->ddir == DDIR_SYNCFS) {
		if (ld->queued)
			return FIO_Q_BUSY;  /* [한국어] 대기 중인 비동기 요청이 있으면 먼저 처리 필요 */

		/* [한국어] TRIM 또는 SYNCFS를 동기적으로 즉시 실행 */
		if (io_u->ddir == DDIR_TRIM)
			do_io_u_trim(td, io_u);    /* [한국어] TRIM(discard) 수행 */
		else
			do_io_u_sync(td, io_u);    /* [한국어] SYNCFS 수행 */
		io_u_mark_submit(td, 1);           /* [한국어] 제출 통계 1 증가 */
		io_u_mark_complete(td, 1);         /* [한국어] 완료 통계 1 증가 */
		return FIO_Q_COMPLETED;            /* [한국어] 즉시 완료됨을 반환 */
	}

	/* [한국어] cmdprio(명령 우선순위) 모드가 활성화되어 있으면
	 * 이 io_u에 대해 우선순위를 설정 */
	if (ld->cmdprio.mode != CMDPRIO_MODE_NONE)
		fio_libaio_cmdprio_prep(td, io_u);

	/* [한국어] === 링 버퍼에 요청 추가 ===
	 * iocbs[head]에 iocb 포인터 저장 (io_submit()에 전달할 배열)
	 * io_us[head]에 io_u 포인터 저장 (나중에 타임스탬프 기록 등에 사용)
	 * head를 1 증가 (링 래핑 적용)
	 * queued 카운터 증가 */
	ld->iocbs[ld->head] = &io_u->iocb;  /* [한국어] iocb 포인터를 head 위치에 저장 */
	ld->io_us[ld->head] = io_u;         /* [한국어] io_u 포인터를 head 위치에 저장 */
	ring_inc(ld, &ld->head, 1);         /* [한국어] head 인덱스를 1 증가 (링 래핑) */
	ld->queued++;                        /* [한국어] 대기 중인 요청 수 증가 */
	return FIO_Q_QUEUED;                 /* [한국어] 큐에 추가됨을 반환 (아직 미제출) */
}

/*
 * [한국어] fio_libaio_queued - io_submit 성공분에 issue_time 찍기(지연 시간 측정용)
 *
 * @param td:    잡 thread_data.
 * @param io_us: 이번 io_submit에서 커널이 받아들인 io_u 포인터 배열의 시작(=io_us+tail).
 * @param nr:    성공적으로 제출된 io_u 개수(io_submit의 양수 반환값).
 * @return: 없음.
 *
 * 배경: fio의 submission latency/completion latency를 분리 측정하려면 "커널에 실제 넘긴
 * 시각"이 필요하다. FIO_ASYNCIO_SETS_ISSUE_TIME 플래그가 설정된 엔진은 이 값을 직접 기록한다.
 *
 * 동작:
 *   1) fio_fill_issue_time(td) 체크 — 이 잡이 issue_time 기록을 원하는지 확인.
 *   2) fio_gettime으로 nr개 모두에 동일 시각을 찍어 gettime 호출 비용 최소화.
 *   3) 각 io_u->issue_time을 채우고 io_u_queued()로 fio 코어 통계 훅 실행.
 *   4) read_iolog_file 모드면 td->last_issue도 갱신(iolog 재생 페이싱 기준).
 *
 * 실행 컨텍스트: 잡 스레드, fio_libaio_commit 내부.
 * 호출 체인: fio_libaio_commit → [fio_libaio_queued] → fio_gettime / io_u_queued.
 * 에러 경로: 없음.
 */
static void fio_libaio_queued(struct thread_data *td, struct io_u **io_us,
			      unsigned int nr)
{
	struct timespec now;   /* [한국어] 현재 시각 */
	unsigned int i;        /* [한국어] 루프 카운터 */

	/* [한국어] issue_time 기록이 필요 없으면 조기 반환 */
	if (!fio_fill_issue_time(td))
		return;

	/* [한국어] 현재 시각을 한 번만 가져옴 (모든 io_u에 동일 시각 적용) */
	fio_gettime(&now, NULL);

	/* [한국어] 제출된 각 io_u에 제출 시각을 기록 */
	for (i = 0; i < nr; i++) {
		struct io_u *io_u = io_us[i];

		/* [한국어] issue_time에 현재 시각 복사 */
		memcpy(&io_u->issue_time, &now, sizeof(now));
		/* [한국어] io_u의 큐잉 완료 처리 (지연 시간 계산 등에 활용) */
		io_u_queued(td, io_u);
	}

	/*
	 * only used for iolog
	 */
	/* [한국어] iolog(I/O 로그) 파일을 사용하는 경우에만
	 * 마지막 제출 시각을 thread_data에도 기록 */
	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &now, sizeof(now));
}

/*
 * [한국어] fio_libaio_commit - ioengine_ops.commit 콜백 (링 → 커널 일괄 제출)
 *
 * @param td: 잡 thread_data.
 * @return: 0 (더 이상 제출할 게 없거나 부분 성공으로 상위가 reap해야 함),
 *          또는 음수 -errno (치명적 실패).
 *
 * 배경: queue()는 iocb를 링에만 쌓아두므로, 실제 커널 진입은 이 함수가 담당한다.
 * 한 번의 io_submit(2)로 다수의 iocb를 내려 syscall 오버헤드를 분산시키는 것이 핵심.
 *
 * 핵심 동작:
 *   1) queued==0이면 즉시 0 반환.
 *   2) 루프:
 *      nr = min(queued, entries - tail)  // 링 끝 래핑 구간만큼만 한 번에
 *      ret = io_submit(aio_ctx, nr, iocbs + tail)
 *      ret>0: queued()로 issue_time 기록, io_u_mark_submit, queued-=ret, tail 전진.
 *      ret==-EINTR 또는 0: 재시도.
 *      ret==-EAGAIN: 커널 AIO 슬롯 포화 →
 *         다른 제출 성공분 있으면 break(상위가 reap 후 재시도),
 *         없으면 30초까지 1us busy-wait, 초과 시 에러로 포기.
 *      ret==-ENOMEM: queued 잔존 시 상위 reap 유도(메모리 회수), 아니면 치명.
 *      그 외 에러: break.
 *   3) queued>0 동안 반복.
 *
 * 실행 컨텍스트: 잡 스레드. getevents()에서도 내부적으로 호출됨.
 * 호출 체인: backend → td_io_commit → [fio_libaio_commit] → io_submit(2)
 *              → (간접) 커널 AIO → VFS → 블록 계층 → 드라이버.
 * 에러 경로: 30초 stall 감지 시 "aio appears to be stalled" 로그 후 ret 반환.
 *
 * io_submit 반환 관례:
 *   >0: 제출된 iocb 수 (nr 이하일 수 있음 — partial submit).
 *   0: 아무것도 제출 안 됨(드묾, EINTR과 유사 처리).
 *   <0: -errno.
 */
static int fio_libaio_commit(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */
	struct iocb **iocbs;        /* [한국어] 이번에 제출할 iocb 배열의 시작 포인터 */
	struct io_u **io_us;        /* [한국어] 이번에 제출할 io_u 배열의 시작 포인터 */
	struct timespec ts;         /* [한국어] EAGAIN 대기 시작 시각 (30초 타임아웃용) */
	int ret, wait_start = 0;    /* [한국어] ret: io_submit 반환값, wait_start: 대기 시작 여부 */

	/* [한국어] 큐에 대기 중인 요청이 없으면 아무것도 하지 않음 */
	if (!ld->queued)
		return 0;

	/* [한국어] 모든 대기 요청을 제출할 때까지 반복 */
	do {
		long nr = ld->queued;  /* [한국어] 제출할 요청 수 */

		/* [한국어] 링 버퍼의 tail부터 끝(entries)까지의 연속 공간만 제출 가능.
		 * 링 버퍼가 끝에서 래핑되면, 끝까지의 요청만 먼저 제출하고
		 * 다음 루프 반복에서 시작 부분의 나머지를 제출. */
		nr = min((unsigned int) nr, ld->entries - ld->tail);
		/* [한국어] tail 위치부터의 io_us와 iocbs 포인터 설정 */
		io_us = ld->io_us + ld->tail;
		iocbs = ld->iocbs + ld->tail;

		/* [한국어] === 핵심: io_submit()으로 커널에 I/O 요청 일괄 제출 ===
		 * io_submit(aio_ctx, 제출할_개수, iocb_포인터_배열)
		 * 성공하면 실제 제출된 개수를 반환 (nr 이하일 수 있음) */
		ret = io_submit(ld->aio_ctx, nr, iocbs);
		if (ret > 0) {
			/* [한국어] 제출 성공: ret개의 요청이 커널에 제출됨 */
			fio_libaio_queued(td, io_us, ret);  /* [한국어] 제출된 io_u에 타임스탬프 기록 */
			io_u_mark_submit(td, ret);          /* [한국어] 제출 통계 업데이트 */

			ld->queued -= ret;                  /* [한국어] 대기 수에서 제출된 수 차감 */
			ring_inc(ld, &ld->tail, ret);       /* [한국어] tail을 제출된 수만큼 전진 */
			ret = 0;                            /* [한국어] 성공 상태로 설정 */
			wait_start = 0;                     /* [한국어] 대기 타이머 리셋 */
		} else if (ret == -EINTR || !ret) {
			/* [한국어] EINTR: 시그널에 의해 중단됨 → 재시도
			 * ret==0: 아무것도 제출되지 않음 → 재시도 */
			if (!ret)
				io_u_mark_submit(td, ret);  /* [한국어] 0개 제출 통계 기록 */
			wait_start = 0;
			continue;  /* [한국어] 루프 계속 (재시도) */
		} else if (ret == -EAGAIN) {
			/*
			 * If we get EAGAIN, we should break out without
			 * error and let the upper layer reap some
			 * events for us. If we have no queued IO, we
			 * must loop here. If we loop for more than 30s,
			 * just error out, something must be buggy in the
			 * IO path.
			 */
			/* [한국어] EAGAIN: 커널 AIO 큐가 꽉 참.
			 * - 큐에 다른 요청이 있으면: 에러 없이 탈출하여 상위 레이어가
			 *   완료 이벤트를 수확하도록 함 (수확 후 큐에 공간이 생김).
			 * - 큐에 다른 요청이 없으면: 여기서 대기하며 재시도해야 함.
			 *   30초 이상 대기하면 I/O 경로에 버그가 있다고 판단하여 에러 반환. */
			if (ld->queued) {
				ret = 0;   /* [한국어] 에러가 아닌 정상 반환 */
				break;     /* [한국어] 루프 탈출 → 상위에서 이벤트 수확 */
			}
			if (!wait_start) {
				/* [한국어] 대기 시작 시각 기록 */
				fio_gettime(&ts, NULL);
				wait_start = 1;
			} else if (mtime_since_now(&ts) > 30000) {
				/* [한국어] 30초(30000ms) 이상 대기: 에러로 판단하고 탈출 */
				log_err("fio: aio appears to be stalled, giving up\n");
				break;
			}
			usleep(1);   /* [한국어] 1마이크로초 대기 후 재시도 */
			continue;
		} else if (ret == -ENOMEM) {
			/*
			 * If we get -ENOMEM, reap events if we can. If
			 * we cannot, treat it as a fatal event since there's
			 * nothing we can do about it.
			 */
			/* [한국어] ENOMEM: 메모리 부족으로 제출 실패.
			 * - 큐에 다른 요청이 있으면: 먼저 이벤트를 수확하여 메모리를 해제.
			 * - 큐가 비어있으면: 치명적 에러로 처리 (복구 불가). */
			if (ld->queued)
				ret = 0;   /* [한국어] 에러가 아닌 정상 반환 */
			break;
		} else
			/* [한국어] 기타 에러 (예: -EBADF, -EFAULT 등): 루프 탈출 */
			break;
	} while (ld->queued);  /* [한국어] 대기 중인 요청이 남아있으면 계속 제출 */

	return ret;  /* [한국어] 0이면 성공, 음수면 에러 코드 */
}

/*
 * [한국어] fio_libaio_cleanup - ioengine_ops.cleanup 콜백 (잡 종료 시 자원 해제)
 *
 * @param td: 잡 thread_data.
 * @return: 없음.
 *
 * 배경: io_destroy(2)는 커널이 내부적으로 AIO 링을 RCU로 유예-해제하기 때문에
 * 많은 잡에서 동시에 호출되면 rcu_sched stall이 발생하는 것으로 알려져 있다.
 * 이를 우회하기 위해 자식 프로세스 경로(TD_F_CHILD)는 이 cleanup에서 destroy를 하지 않고,
 * 부모의 exit_aio()가 병렬로 처리한다.
 *
 * 동작:
 *   1) ld이 NULL이 아니면:
 *      - TD_F_CHILD가 꺼져 있으면 io_destroy(aio_ctx) 직접 호출.
 *      - cmdprio / iovecs / aio_events / iocbs / io_us / ld 자체를 free.
 *
 * 실행 컨텍스트: 잡 스레드(또는 부모) 종료 시.
 * 호출 체인: backend → td_io_cleanup → [fio_libaio_cleanup] → io_destroy(2) / free.
 * 에러 경로: io_destroy 실패는 무시(종료 경로라 복구 의미 없음).
 */
static void fio_libaio_cleanup(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */

	if (ld) {
		/*
		 * Work-around to avoid huge RCU stalls at exit time. If we
		 * don't do this here, then it'll be torn down by exit_aio().
		 * But for that case we can parallellize the freeing, thus
		 * speeding it up a lot.
		 */
		/* [한국어] 종료 시 거대한 RCU 지연(stall)을 피하기 위한 우회 방법.
		 * 여기서 AIO 컨텍스트를 파괴하지 않으면 exit_aio()에서 처리됨.
		 * 그러나 exit_aio()에서는 해제를 병렬화할 수 있어서 더 빠름.
		 *
		 * TD_F_CHILD가 아닌 경우(부모 프로세스)에만 여기서 io_destroy() 호출.
		 * 자식 프로세스들은 exit_aio()에서 병렬로 처리됨. */
		if (!(td->flags & TD_F_CHILD))
			io_destroy(ld->aio_ctx);  /* [한국어] 커널 AIO 컨텍스트 파괴 */

		fio_cmdprio_cleanup(&ld->cmdprio);  /* [한국어] cmdprio 자원 해제 */
		free(ld->iovecs);        /* [한국어] iovec 배열 해제 */
		free(ld->aio_events);    /* [한국어] io_event 배열 해제 */
		free(ld->iocbs);         /* [한국어] iocb 포인터 배열 해제 */
		free(ld->io_us);         /* [한국어] io_u 포인터 배열 해제 */
		free(ld);                /* [한국어] libaio_data 구조체 자체 해제 */
	}
}

/*
 * [한국어] fio_libaio_post_init - ioengine_ops.post_init 콜백 (AIO 컨텍스트 생성)
 *
 * @param td: 잡 thread_data.
 * @return: 0 성공, 1 실패(td_verror로 에러가 기록됨).
 *
 * 배경: init()과 분리된 이유는 (a) 메모리/옵션 유효성 검증을 먼저 끝내고
 * (b) fork 이전에 AIO 컨텍스트가 자식에 상속되도록 해야 하며 (c) nofile/memlock
 * rlimit을 먼저 올린 뒤 io_setup을 해야 aio-max-nr 실패를 줄일 수 있기 때문.
 *
 * 동작: io_queue_init(iodepth, &ld->aio_ctx) → 내부적으로 io_setup(iodepth, &ctx).
 *       커널은 이 요청에 대해 aio-nr을 iodepth만큼 차감하고 AIO 링 페이지를 mmap.
 *
 * 실행 컨텍스트: 잡 스레드(유저스페이스). 한 번 성공하면 이후 재호출 없음.
 * 호출 체인: backend → td_io_init 이후 → [fio_libaio_post_init] → io_queue_init → io_setup(2)
 * 에러 경로:
 *   - -EAGAIN: /proc/sys/fs/aio-max-nr 초과(시스템 전역 한도).
 *   - -ENOMEM: 커널 메모리 부족.
 *   실패 시 td_verror 호출로 잡 상태 ERROR 전환.
 */
static int fio_libaio_post_init(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */
	int err;

	/* [한국어] 커널 AIO 컨텍스트 생성.
	 * iodepth 크기의 AIO 큐를 커널에 요청.
	 * 내부적으로 io_setup() 시스템 콜을 호출함. */
	err = io_queue_init(td->o.iodepth, &ld->aio_ctx);
	if (err) {
		/* [한국어] 실패 시 에러 보고.
		 * 일반적 원인: /proc/sys/fs/aio-max-nr 한도 초과,
		 * 또는 메모리 부족. */
		td_verror(td, -err, "io_queue_init");
		return 1;  /* [한국어] 실패 */
	}

	return 0;  /* [한국어] 성공 */
}

/*
 * [한국어] fio_libaio_init - ioengine_ops.init 콜백 (엔진 상태 할당)
 *
 * @param td: 잡 thread_data. td->eo는 파싱 완료된 libaio_options.
 * @return: 0 성공, 1 실패.
 *
 * 배경: AIO 컨텍스트 생성은 자원 제한 설정 이후(post_init)로 미루되, 링 배열/
 * 옵션 의존 상태(cmdprio)는 fork 전에 준비해둬야 하므로 여기서 일괄 할당한다.
 *
 * 할당 목록(크기 모두 entries = iodepth):
 *   - libaio_data (자체)
 *   - aio_events[] : io_event 완료 슬롯
 *   - iocbs[]      : iocb 포인터 링
 *   - io_us[]      : io_u 포인터 링
 *   - iovecs[]     : vectored 경로용 iovec
 * 메타: is_pow2 = is_power_of_2(iodepth)로 ring_inc 경로 결정.
 *
 * 실행 컨텍스트: 잡 스레드.
 * 호출 체인: backend → td_io_init → [fio_libaio_init] → calloc / fio_cmdprio_init.
 * 에러 경로: calloc 실패는 현재 미검사(레거시). cmdprio_init 실패 시 td_verror 후 1 반환.
 */
static int fio_libaio_init(struct thread_data *td)
{
	struct libaio_data *ld;                     /* [한국어] 엔진 데이터 포인터 */
	struct libaio_options *o = td->eo;          /* [한국어] 엔진 옵션 가져오기 */
	int ret;

	/* [한국어] libaio_data 구조체를 0으로 초기화하여 할당 */
	ld = calloc(1, sizeof(*ld));

	/* [한국어] 링 버퍼 크기를 iodepth로 설정 */
	ld->entries = td->o.iodepth;
	/* [한국어] entries가 2의 거듭제곱인지 확인 (ring_inc 최적화용) */
	ld->is_pow2 = is_power_of_2(ld->entries);
	/* [한국어] 완료 이벤트 저장용 io_event 배열 할당 */
	ld->aio_events = calloc(ld->entries, sizeof(struct io_event));
	/* [한국어] iocb 포인터 배열 할당 (링 버퍼, io_submit에 전달) */
	ld->iocbs = calloc(ld->entries, sizeof(struct iocb *));
	/* [한국어] io_u 포인터 배열 할당 (링 버퍼, 타임스탬프 기록 등에 사용) */
	ld->io_us = calloc(ld->entries, sizeof(struct io_u *));
	/* [한국어] 벡터 I/O용 iovec 배열 할당 */
	ld->iovecs = calloc(ld->entries, sizeof(ld->iovecs[0]));

	/* [한국어] 엔진 데이터를 thread_data에 저장.
	 * 이후 다른 콜백 함수들이 td->io_ops_data로 접근. */
	td->io_ops_data = ld;

	/* [한국어] 명령 우선순위(cmdprio) 기능 초기화 */
	ret = fio_cmdprio_init(td, &ld->cmdprio, &o->cmdprio_options);
	if (ret) {
		/* [한국어] cmdprio 초기화 실패 시 에러 보고 */
		td_verror(td, EINVAL, "fio_libaio_init");
		return 1;  /* [한국어] 실패 */
	}

	return 0;  /* [한국어] 성공 */
}

/*
 * [한국어] I/O 엔진 오퍼레이션 구조체 정의
 *
 * fio의 I/O 엔진 플러그인 계약(contract). ioengines.c가 이 테이블을 통해
 * 엔진 함수들을 디스패치한다. 각 필드는 NULL 허용/불허, 호출 시점, 반환값 규약이
 * ioengines.c에 의해 엄격히 정의되어 있다.
 *
 * 전체 호출 흐름(엔진 수명):
 *   [fio_init 생성자 시점] register_ioengine(&ioengine)
 *     → parse_options가 --ioengine=libaio 매칭 후 td->io_ops = &ioengine 연결
 *     → td_io_init(ioengines.c) → .init=fio_libaio_init (자원 할당)
 *     → .post_init=fio_libaio_post_init (io_queue_init)
 *     → 메인 I/O 루프 반복:
 *         .prep=fio_libaio_prep   (iocb 필드 채움)
 *         .queue=fio_libaio_queue (링에 적재 → FIO_Q_QUEUED)
 *         .commit=fio_libaio_commit (io_submit 일괄 제출)
 *         .getevents=fio_libaio_getevents (io_getevents/usr_reap)
 *         .event=fio_libaio_event  (ev → io_u 변환)
 *     → .cleanup=fio_libaio_cleanup (io_destroy + free)
 *     → [fio_exit 소멸자 시점] unregister_ioengine(&ioengine)
 *
 * open_file/close_file/get_file_size는 generic_* 구현을 그대로 사용해
 * 표준 open(2)/close(2)/fstat(2) 경로에 위임 — libaio 엔진은 파일 핸들 생성에
 * 특별 처리가 없다.
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "libaio",
	/* [한국어] 엔진 식별자. --ioengine=libaio 매칭 및 /proc 통계/로그에 표시.
	 * 설정자: 여기서 컴파일타임 정적. 읽는 자: load_ioengine의 strcmp 매칭(ioengines.c),
	 *          show_job의 엔진명 출력, dlsym 외부 엔진과 구분되는 built-in 식별. */
	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine_ops ABI 버전. fio 헤더에서 정의된 상수와 동일해야 로드 허용.
	 * 설정자: fio.h/ioengines.h. 읽는 자: check_engine_ops(ioengines.c)가 버전 mismatch
	 *          시 엔진 로딩 거부. ABI 안정성 가드레일. */
	.flags			= FIO_ASYNCIO_SYNC_TRIM |
					FIO_ASYNCIO_SYNC_SYNCFS |
					FIO_ASYNCIO_SETS_ISSUE_TIME |
					FIO_ATOMICWRITES,
	/* [한국어] 엔진 동작 특성 플래그 비트마스크.
	 * - FIO_ASYNCIO_SYNC_TRIM:  DDIR_TRIM을 엔진이 동기 폴백(BLKDISCARD ioctl/fallocate)으로
	 *                           처리한다고 코어에 통지. 코어는 TRIM 요청 시 inflight 카운터를
	 *                           증가시키지 않고 queue()가 즉시 FIO_Q_COMPLETED를 반환할 것으로 가정.
	 * - FIO_ASYNCIO_SYNC_SYNCFS: DDIR_SYNCFS도 동기 폴백(fsync/syncfs)으로 처리한다고 통지.
	 * - FIO_ASYNCIO_SETS_ISSUE_TIME: 엔진이 io_u->issue_time을 스스로 기록 — 코어는
	 *                                queue()/commit() 경계에서 자동 기록을 생략. 본 엔진은
	 *                                fio_libaio_queued()에서 io_submit 성공분에 한꺼번에 기록.
	 * - FIO_ATOMICWRITES:       RWF_ATOMIC(torn-write 방지) 지원 — oatomic 옵션 활성화 조건.
	 * 미설정 비트 의미: SYNCIO(동기 엔진 아님), DISKLESSIO(실제 파일 I/O), RAWIO(블록디바이스 전용
	 *                   아님), MEMALIGN(버퍼 정렬은 O_DIRECT 사용 여부로 결정), BARRIER(순서 강제 없음),
	 *                   UNIDIR(읽기·쓰기 모두 지원), NODISKUTIL(디스크 통계 활성). */
	.init			= fio_libaio_init,
	/* [한국어] ioengine_ops.init 콜백 — 엔진 상태 할당.
	 * 호출 시점: td_io_init(ioengines.c) 내부, post_init 직전. 한 번만.
	 * 반환 규약: 0 성공, 1 실패(td->error 세팅 필요).
	 * 본 엔진: libaio_data, iocbs/io_us/aio_events/iovecs calloc, cmdprio 초기화. */
	.post_init		= fio_libaio_post_init,
	/* [한국어] .post_init 콜백 — init 이후, fork/세팅 완료 뒤 호출.
	 * 호출 시점: td_io_post_init(ioengines.c). rlimit 조정·시그널 세팅이 끝난 뒤라
	 *            io_setup(2)의 aio-max-nr 실패 가능성이 최소화되는 시점.
	 * 본 엔진: io_queue_init으로 io_context_t 생성(커널 링 mmap). */
	.prep			= fio_libaio_prep,
	/* [한국어] .prep 콜백 — 각 io_u를 엔진이 이해하는 포맷으로 준비.
	 * 호출 시점: td_io_prep, queue() 직전. per-io_u 1회.
	 * 반환 규약: 0 성공, 음수 실패(-errno).
	 * 본 엔진: io_u->iocb를 io_prep_pread/pwrite/preadv/pwritev/fsync로 채움. */
	.queue			= fio_libaio_queue,
	/* [한국어] .queue 콜백 — 준비된 io_u를 엔진 제출 경로에 투입.
	 * 호출 시점: td_io_queue, prep 직후.
	 * 반환 규약: FIO_Q_QUEUED(비동기 적재 완료, commit 필요), FIO_Q_COMPLETED(동기 완료,
	 *            추가 getevents 불필요), FIO_Q_BUSY(자원 포화, 호출측이 reap 후 재시도).
	 * 본 엔진: TRIM/SYNCFS는 동기 폴백 → FIO_Q_COMPLETED, 나머지는 링 적재 → FIO_Q_QUEUED. */
	.commit			= fio_libaio_commit,
	/* [한국어] .commit 콜백 — queue()로 쌓인 요청을 커널에 실제 내림(배치 제출).
	 * 호출 시점: 링 포화, iodepth_batch 도달, 또는 getevents 재시도 경로.
	 * 반환 규약: 0 성공(부분 성공 포함, queued 잔존 허용), 음수 -errno 치명.
	 * 본 엔진: io_submit 루프 + EAGAIN/ENOMEM/EINTR 재시도 전략. */
	.getevents		= fio_libaio_getevents,
	/* [한국어] .getevents 콜백 — 완료 이벤트 수확.
	 * 호출 시점: td_io_getevents. 내부 min/max/timespec 인자로 대기·수확 규모 제어.
	 * 반환 규약: 실제 수확 개수(>=0) 또는 음수 -errno.
	 * 본 엔진: userspace_reap=1 & actual_min==0 & magic 매치면 user_io_getevents,
	 *          아니면 io_getevents(2). EAGAIN/EINTR 시 commit/usleep 재시도. */
	.event			= fio_libaio_event,
	/* [한국어] .event 콜백 — getevents가 수확한 i번째 이벤트를 io_u로 디코딩.
	 * 호출 시점: getevents 반환값 N에 대해 i=0..N-1 반복 호출.
	 * 반환 규약: 해당 io_u* 포인터. io_u->error/resid는 엔진이 세팅.
	 * 본 엔진: container_of로 ev->obj(=iocb*) → io_u 역추적, ev->res 해석. */
	.cleanup		= fio_libaio_cleanup,
	/* [한국어] .cleanup 콜백 — 잡 종료 시 자원 해제.
	 * 호출 시점: td_io_cleanup, 잡 스레드 종료 경로.
	 * 본 엔진: TD_F_CHILD 분기로 io_destroy 호출 여부 결정(RCU stall 우회),
	 *          모든 calloc 블록 free + cmdprio cleanup. */
	.open_file		= generic_open_file,
	/* [한국어] .open_file 콜백 — 파일/디바이스 열기.
	 * 호출 시점: 잡 시작 시 각 fio_file에 대해 1회.
	 * 본 엔진: fio 코어의 범용 구현(filehash/open(2)/O_DIRECT 등) 그대로 재사용. */
	.close_file		= generic_close_file,
	/* [한국어] .close_file 콜백 — 파일 닫기.
	 * 본 엔진: 범용 close(2) 호출 + filehash 제거에 위임. */
	.get_file_size		= generic_get_file_size,
	/* [한국어] .get_file_size 콜백 — 실제 파일 크기 조회.
	 * 본 엔진: fstat(2)/BLKGETSIZE64 ioctl 등 범용 구현 재사용. */
	.options		= options,
	/* [한국어] .options — 엔진 전용 fio_option 배열(종료 sentinel 포함).
	 * 읽는 자: parse_options가 --name=val/잡파일 라인 매칭 시 순회. */
	.option_struct_size	= sizeof(struct libaio_options),
	/* [한국어] .option_struct_size — libaio_options 구조체 크기.
	 * 설정자: compile-time sizeof. 읽는 자: fio 코어가 td->eo 할당 시 이 크기만큼 malloc. */
};

/*
 * [한국어] fio_libaio_register - fio_init 생성자 (engine 레지스트리 등록)
 *
 * @param: 없음.
 * @return: 없음.
 *
 * 배경: fio_init 매크로는 gcc의 __attribute__((constructor))로 확장되어 main() 진입
 * 전에 자동 실행된다. 각 엔진 .c는 이 패턴으로 자신을 런타임 플러그인 테이블에 등록한다.
 *
 * 실행 컨텍스트: 프로세스 초기화 단계(main 이전). 단일 스레드.
 * 호출 체인: 프로그램 로더/CRT → [fio_libaio_register] → register_ioengine(ioengines.c).
 * 에러 경로: register_ioengine 실패는 이름 중복 등 극히 예외적 상황.
 */
static void fio_init fio_libaio_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] ioengine 구조체를 fio의 엔진 목록에 등록 */
}

/*
 * [한국어] fio_libaio_unregister - fio_exit 소멸자 (engine 레지스트리 해제)
 *
 * @param: 없음.
 * @return: 없음.
 *
 * 배경: fio_exit는 __attribute__((destructor)) 확장. 프로세스 정상 종료 시 자동 호출되어
 * 전역 ioengine_list에서 엔트리를 떼어낸다.
 *
 * 실행 컨텍스트: 프로세스 종료(atexit 경로). 단일 스레드.
 * 호출 체인: _exit/atexit 체인 → [fio_libaio_unregister] → unregister_ioengine.
 * 에러 경로: 없음.
 */
static void fio_exit fio_libaio_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] fio의 엔진 목록에서 libaio 엔진 제거 */
}
