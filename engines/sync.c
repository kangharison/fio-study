/*
 * [한국어 설명] 동기식 POSIX I/O 엔진 구현 (sync.c) — fio의 기준 엔진 5종
 *
 * === 파일의 역할 ===
 * 이 파일은 fio가 제공하는 가장 기본적인 동기식(sync) I/O 엔진을 구현한다. 표준
 * POSIX 시스템 호출 read(2)/write(2), pread(2)/pwrite(2), readv(2)/writev(2),
 * preadv(2)/pwritev(2), preadv2(2)/pwritev2(2) 위에 **단일 소스 파일 하나로**
 * 5가지 서로 다른 ioengine ("sync", "psync", "vsync", "pvsync", "pvsync2")
 * 을 동시에 등록한다. 5개 엔진이 한 파일에 모여 있는 이유는 (1) 모두 동일한
 * syncio_data 구조체와 fio_io_end() 완료 환산 루틴·LAST_POS 캐시·do_io_u_sync
 * /do_io_u_trim 경로를 공유하며, (2) ddir 분기 구조가 동형이어서 시스템 호출만
 * 바꿔 끼우는 파생 관계이기 때문이다. 각 엔진은 동일한 io_u 해석 규약을 따르며
 * 서로 다른 시스템 호출을 통해 단일 버퍼(scalar) 또는 벡터(scatter/gather) I/O
 * 를 수행한다. 모든 I/O가 호출과 동시에 커널에서 완료되므로 fio_q_status의
 * FIO_Q_COMPLETED/FIO_Q_QUEUED 의미가 비동기 엔진과 다르게 해석된다 — sync/psync
 * /pvsync/pvsync2는 queue() 내부에서 곧장 커널 왕복을 마치고 COMPLETED를 반환하고,
 * vsync만 연속 I/O를 iovec에 누적하여 commit() 시점에 readv/writev로 일괄 flush
 * 한다. 이 엔진은 설치 필수 기능이 거의 없어 모든 POSIX 플랫폼에서 동작하며,
 * fio의 다른(비동기·커널 바이패스) 엔진 성능을 비교할 때의 **기준선(baseline)**
 * 역할을 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 실행 흐름(backend.c: thread_main → do_io)에서 잡 스레드는 매 iteration마다
 * 다음 순서로 ioengine 콜백을 호출한다:
 *   get_io_u() → td_io_prep() → td_io_queue() → td_io_commit() →
 *   td_io_getevents() → ioengine.event() → put_io_u().
 * 본 파일이 채우는 콜백은 엔진별로 다르다 (비어 있는 슬롯은 fio 코어의 기본
 * no-op/generic_* 경로로 대체된다):
 *   - "sync":     prep = fio_syncio_prep (필요 시 lseek으로 파일 포인터 이동)
 *                 queue = fio_syncio_queue (read/write — 파일 포인터 기반)
 *                 → FIO_Q_COMPLETED 즉시 반환. commit/event/getevents 불필요
 *                   (FIO_SYNCIO 플래그가 코어에게 "큐잉 없음"을 통지).
 *   - "psync":    queue = fio_psyncio_queue (pread/pwrite — offset 직접 전달)
 *                 → FIO_Q_COMPLETED. prep 불필요(파일 포인터에 의존하지 않음).
 *   - "vsync":    init/cleanup = fio_vsyncio_{init,cleanup} (syncio_data 할당·해제)
 *                 queue = fio_vsyncio_queue (iovec 배열에 누적, FIO_Q_QUEUED 반환)
 *                 commit = fio_vsyncio_commit (lseek + readv/writev 일괄 발행)
 *                 event/getevents = fio_vsyncio_{event,getevents} (완료 보고)
 *                 → 유일하게 비동기 엔진 계약(FIO_Q_QUEUED)을 따르지만 실제 커널
 *                   호출은 commit에서 동기적으로 수행. 연속 I/O 병합 최적화.
 *   - "pvsync":   init/cleanup은 vsync와 공유(syncio_data만 iovec[0] 슬롯 하나씩 사용)
 *                 queue = fio_pvsyncio_queue (preadv/pwritev로 **즉시** 동기 실행)
 *                 → FIO_Q_COMPLETED. vsync처럼 누적하지 않고, 벡터 I/O이지만 iovec
 *                   원소 1개만 써서 사실상 pread/pwrite와 동일한 오프셋 동작.
 *   - "pvsync2":  queue = fio_pvsyncio2_queue (preadv2/pwritev2 + RWF_* 플래그)
 *                 → FIO_Q_COMPLETED. pvsync 상위호환으로 RWF_HIPRI/DONTCACHE/
 *                   NOWAIT/ATOMIC 확장 플래그 제어 가능 (리눅스 4.6+).
 * 실행 컨텍스트: 모든 콜백은 fio **잡 스레드**(또는 잡 프로세스, 옵션에 따라)에서
 * 동기적으로 실행된다. 커널이 VFS→블록 레이어→DMA 순으로 실제 I/O를 완료할
 * 때까지 시스템 호출 내부에서 block되므로, getevents의 최소 min도 commit 시점에
 * 이미 모두 채워져 있다. SYNCIO 플래그가 세팅되면 fio 코어는 commit/getevents
 * 단계를 자체적으로 생략한다(ioengines.c의 td_io_queue 분기 참조).
 *
 * === 타 모듈과의 연결 ===
 * - 의존 모듈(include/linker): 표준 C 라이브러리(read/write/lseek/p*v*2 시스템 호출
 *   래퍼), sys/uio.h의 struct iovec, errno 전역, fio 코어(fio.h)의 thread_data/
 *   io_u/fio_file/ioengine_ops/FIO_Q_* 정의 및 do_io_u_trim·do_io_u_sync·
 *   io_u_log_error·td_verror·fio_ro_check·io_u_mark_submit 유틸, optgroup.h의
 *   FIO_OPT_C_ENGINE 카테고리와 FIO_OPT_G_INVALID 그룹 매크로, lib/rand.h의
 *   frand_state/init_rand/rand_between(pvsync2 hipri 확률 결정용 난수 생성).
 * - 이 파일에 의존하는 모듈: fio 코어 런타임 — 생성자(fio_init 속성)로 등록된
 *   5개 vtable이 전역 engine_list에 링크되어, ioengines.c의 load_ioengine()이
 *   잡 파일의 ioengine=<name> 옵션을 strcmp로 매칭한다. 기본 잡 파일 파싱에서
 *   ioengine 미지정 시 "sync"가 폴백으로 쓰이므로 fio 전체 회귀 테스트에서 가장
 *   많이 실행되는 엔진이다.
 * - 데이터 흐름: io_u->xfer_buf (사용자 데이터 버퍼) / io_u->xfer_buflen (전송 바이트)
 *   / io_u->offset (파일 내 오프셋) / io_u->ddir (READ|WRITE|TRIM|SYNC*) / io_u->file
 *   (fio_file 포인터, f->fd 커널 fd 포함)을 입력으로 받아 → read/pread/readv/preadv/
 *   preadv2 또는 write/pwrite/writev/pwritev/pwritev2 시스템 호출을 발행 → 커널 VFS
 *   → 파일시스템 → 블록 레이어 → 디바이스 드라이버로 데이터 이동. 반환값은 전송된
 *   바이트 수(성공) 또는 -1(errno 설정 시). 이 값을 fio_io_end() 또는 fio_vsyncio_end()
 *   에서 io_u->resid(미전송 바이트 수)와 io_u->error(errno 복사)로 환산하여 fio
 *   코어에 보고한다. 결과적으로 io_u는 in_flight → completed 상태로 전이하고
 *   put_io_u()에서 free 풀로 복귀한다.
 * - 공유 자료구조:
 *   · td->io_ops_data = struct syncio_data *  (vsync/pvsync/pvsync2만 사용.
 *     sync/psync는 자체 상태가 없어 NULL). fio_vsyncio_init/cleanup에서 생성/해제.
 *   · td->eo          = struct psyncv2_options *  (pvsync2 전용, 옵션 파서가 생성).
 *   · f->engine_pos   = uint64_t  (LAST_POS 매크로로 접근). sync 엔진이 "마지막
 *     R/W의 끝 오프셋"을 캐시해 다음 prep에서 lseek을 생략하는 최적화 슬롯.
 *     -1ULL(UINT64_MAX)이 "무효/초기" 센티널 값. vsync commit 경로에서도 갱신.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_syncio_prep(): sync 엔진의 prep 콜백. LAST_POS와 io_u->offset을 비교해
 *   일치하면 skip, 불일치하면 lseek(SEEK_SET)으로 파일 포인터 이동. 유일하게
 *   lseek을 쓰는 엔진이 sync — read/write가 파일 포인터 의존적이기 때문.
 * - fio_io_end(): 단일 syscall 반환값 → io_u->resid/error 공통 환산 헬퍼.
 *   sync/psync/pvsync/pvsync2가 모두 호출. vsync는 별도의 fio_vsyncio_end()
 *   (iovec 순회로 각 io_u에 바이트 분배)를 사용.
 * - fio_syncio_queue()/fio_psyncio_queue(): 각각 read·write / pread·pwrite 동기 발행.
 * - fio_pvsyncio_queue()/fio_pvsyncio2_queue(): preadv·pwritev / preadv2·pwritev2.
 *   pvsync2는 추가로 RWF_HIPRI/DONTCACHE/NOWAIT/ATOMIC 플래그를 조합.
 * - fio_vsyncio_{queue,append,set_iov,commit,end}(): 연속 I/O를 iovec에 병합,
 *   lseek 후 readv/writev로 일괄 실행, 부분 전송 시 각 io_u에 resid 분배.
 * - fio_vsyncio_{init,cleanup,event,getevents}(): vsync 계열(vsync/pvsync/pvsync2)
 *   공용의 상태 생성/파괴, 완료 이벤트 보고(commit에서 이미 완료된 것을 카운터로 소비).
 * - struct syncio_data: vsync 계열의 iovec 배열, 누적 상태, last_* 병합 캐시,
 *   pvsync2 hipri 확률 판정용 frand_state.
 * - struct psyncv2_options: pvsync2 전용 옵션(hipri/hipri_percentage/uncached/nowait).
 * - fio_syncio_register()/fio_syncio_unregister(): 5개 ioengine_ops를 한 번에
 *   등록/해제(빌드 매크로 CONFIG_PWRITEV, FIO_HAVE_PWRITEV2로 선택적 포함).
 */

/*
 * sync/psync engine
 *
 * IO engine that does regular read(2)/write(2) with lseek(2) to transfer
 * data and IO engine that does regular pread(2)/pwrite(2) to transfer data.
 *
 */
/* [한국어] <stdio.h>: printf/fprintf 계열 선언 — 본 파일은 fprintf를 직접 호출하지
 * 않지만 fio.h가 간접 포함하는 dprint()/log_err() 매크로 경로가 FILE* stderr/stdout
 * 심볼을 참조할 수 있어 방어적으로 포함. 시스템 헤더이므로 dependency는 libc 하나. */
#include <stdio.h>
/* [한국어] <stdlib.h>: calloc/malloc/free 프로토타입 — fio_vsyncio_init()에서
 * syncio_data 구조체(calloc)와 iovecs/io_us 배열(malloc)을 할당, cleanup에서 free로
 * 해제. 또한 EXIT_SUCCESS 등 표준 상수의 공급원. */
#include <stdlib.h>
/* [한국어] <unistd.h>: read/write/lseek/pread/pwrite 프로토타입과 SEEK_SET 상수의
 * 1차 공급원(POSIX). 이 파일의 sync/psync 엔진은 여기서 선언된 시스템 호출 래퍼를
 * 직접 사용한다. close/open 등 파일 오픈 시스템 호출은 generic_open_file에 위임되므로
 * 여기서 직접 호출하지 않는다. */
#include <unistd.h>
/* [한국어] <sys/uio.h>: struct iovec { iov_base, iov_len } 정의와 readv/writev/
 * preadv/pwritev/preadv2/pwritev2 프로토타입 공급 — vsync/pvsync/pvsync2 계열의
 * 벡터(scatter/gather) I/O에 필수. iovec는 커널 UAPI와 동일 레이아웃이라 사용자
 * 공간 포인터를 그대로 커널에 전달 가능. */
#include <sys/uio.h>
/* [한국어] <errno.h>: errno 전역 변수와 EAGAIN/EINVAL 등 에러 코드 매크로 공급.
 * 모든 시스템 호출 실패(ret=-1) 경로에서 errno를 fio_io_end()가 io_u->error로
 * 복사하여 fio 코어에 전파. pvsync2 nowait 경로에서 EAGAIN도 여기 포함됨. */
#include <errno.h>

/* [한국어] "../fio.h": fio 코어의 공용 타입/매크로 단일 진입점 헤더 —
 *   · struct thread_data (잡 컨텍스트), struct io_u (I/O 유닛),
 *     struct fio_file (대상 파일, f->fd 커널 fd·f->engine_pos), struct ioengine_ops.
 *   · FIO_Q_COMPLETED/QUEUED/BUSY 반환값 enum, fio_q_status 타입.
 *   · td_verror() (잡 전역 오류 보고), io_u_log_error() (I/O 디버그 로깅),
 *     fio_ro_check() (readonly 잡에서 DDIR_WRITE 차단), do_io_u_sync() (fsync/
 *     fdatasync/sync_file_range 디스패치), do_io_u_trim() (blkdev BLKDISCARD/
 *     파일시스템 fallocate PUNCH_HOLE 디스패치).
 *   · ddir_rw(ddir) 매크로 (READ|WRITE 판정), ddir_sync(ddir) 매크로 (SYNC*
 *     계열 판정), dprint() (FD_IO 디버그 로거).
 *   · FIO_IOOPS_VERSION (ioengine ABI 버전), FIO_SYNCIO/FIO_SYNCFS/
 *     FIO_ATOMICWRITES 등 엔진 특성 플래그 비트, generic_open_file/close_file/
 *     get_file_size 함수 포인터(이 엔진들은 특수 오픈 로직이 없어 그대로 재사용).
 *   · io_u_mark_submit() (commit에서 동시 발행 I/O 개수 통계 기록). */
#include "../fio.h"
/* [한국어] "../optgroup.h": 옵션 카테고리 분류 매크로 공급 —
 *   · FIO_OPT_C_ENGINE: 옵션이 "엔진 카테고리"에 속함(HOWTO 문서 자동 생성용).
 *   · FIO_OPT_G_INVALID: 옵션 그룹 미지정(pvsync2 옵션은 별도 그룹 없음). */
#include "../optgroup.h"
/* [한국어] "../lib/rand.h": fio 공용 난수 생성기 공급 —
 *   · struct frand_state: Marsaglia의 xorwow 또는 Tausworthe 상태(빌드 옵션 의존).
 *   · init_rand(state, use_os_rand): seed 초기화. 두 번째 인자 0이면 고정 seed.
 *   · rand_between(state, lo, hi): [lo, hi] 범위 균등 난수 → pvsync2의 hipri_percentage
 *     확률 판정("1..100 중 뽑은 값이 percentage 이하면 RWF_HIPRI 부여")에 사용. */
#include "../lib/rand.h"

/*
 * Sync engine uses engine_data to store last offset
 */
/* [한국어] LAST_POS(f): fio_file 구조체의 engine_pos 슬롯(uint64_t)을 "파일 포인터의
 * 마지막 위치 = 직전 I/O의 끝 오프셋"을 저장하는 캐시로 전용(專用) 사용하는 매크로.
 * 왜 필요한가: sync 엔진의 read(2)/write(2)는 파일 포인터(file description의 pos)에
 * 의존하며 호출 후 자동으로 전송 바이트만큼 전진한다. 따라서 다음 io_u가 직전
 * I/O의 끝 바로 다음 오프셋이면 lseek(2) syscall을 생략할 수 있다(순차 I/O 최적화,
 * syscall 횟수 절반으로 감소). 이 매크로는 f->engine_pos를 lvalue로 노출해 양방향
 * 접근을 단순화한다.
 * 설정자: fio_io_end()가 성공한 R/W 후 "offset + ret"로 갱신. fio_vsyncio_init()는
 * 초기에 -1ULL로 세팅하지 않고(잡 시작 시 fio_file이 0 초기화되므로 미정의일 수
 * 있음), fio_syncio_prep()가 -1ULL 체크로 방어한다.
 * 읽는 자: fio_syncio_prep()만 읽음(sync 엔진 전용 최적화). psync/pvsync/pvsync2는
 * 오프셋을 시스템 호출 인자로 직접 전달하므로 LAST_POS 읽을 필요가 없다.
 * 값 범위: 0..파일_크기 또는 -1ULL(무효 센티널). UINT64_MAX == -1ULL은 실제 파일에선
 * 도달할 수 없는 값이라 안전.
 * 동기화: fio_file은 잡 스레드 단위로 독립이거나(잡당 분리), 공유 시에도 sync 엔진은
 * 보통 per-job fd이므로 별도 락 불필요. */
#define LAST_POS(f)	((f)->engine_pos)

/*
 * [한국어] struct syncio_data — vsync/pvsync/pvsync2 계열 엔진의 잡별 내부 상태.
 *
 * 수명: fio_vsyncio_init()에서 calloc으로 할당되어 td->io_ops_data에 부착되고,
 *       fio_vsyncio_cleanup()에서 해제된다. 잡 스레드 1개에 1개.
 * 공유: 잡 스레드 단독 소유 — 다른 스레드 접근 없음. 따라서 모든 필드가 lock-free.
 * 역할:
 *   1) 벡터 I/O(readv/writev)를 위한 iovec 배열과 큐잉 상태(iovecs, io_us, queued,
 *      queued_bytes, events)를 보관.
 *   2) "이번 io_u가 직전에 큐잉된 것과 같은 파일·방향·연속 오프셋인가?" 를 빠르게
 *      판정하기 위한 캐시(last_offset/last_file/last_ddir)를 보관 — vsync만 사용.
 *   3) pvsync2의 확률적 RWF_HIPRI 결정에 쓰일 잡별 난수 상태(rand_state) 보관.
 * 엔진별 사용량:
 *   · vsync   — iovecs[0..iodepth), io_us[0..iodepth), queued/events/queued_bytes,
 *               last_offset/file/ddir 모두 사용.
 *   · pvsync  — iovecs[0]만 사용(단일 벡터), 나머지는 할당되지만 unused. rand_state도 unused.
 *   · pvsync2 — iovecs[0]만 사용, rand_state도 hipri 결정에 사용. queued* 필드 unused.
 */
struct syncio_data {
	struct iovec *iovecs;
	/* [한국어] scatter/gather I/O용 iovec 배열(길이 = td->o.iodepth).
	 * 설정자: fio_vsyncio_init()가 malloc으로 할당(초기화되지 않은 메모리),
	 *        fio_vsyncio_set_iov()가 각 슬롯의 iov_base/iov_len 기입(vsync),
	 *        fio_pvsyncio_queue()·fio_pvsyncio2_queue()는 [0] 슬롯만 재사용.
	 * 읽는 자: fio_vsyncio_commit()의 readv(2)/writev(2)에 sd->queued 길이로 전달,
	 *          pvsync는 preadv/pwritev에 길이 1로, pvsync2는 preadv2/pwritev2에 길이 1로.
	 * 값 범위: 각 원소의 iov_base = io_u->xfer_buf (사용자 버퍼 포인터, NULL 아님),
	 *          iov_len = io_u->xfer_buflen (>0, 보통 4KiB~1MiB). 커널이 직접 참조.
	 * 동기화: 잡 스레드 전용 — 다른 스레드 접근 없음. */

	struct io_u **io_us;
	/* [한국어] 큐잉된 io_u 포인터 배열(길이 = td->o.iodepth) — commit 후 event(idx)
	 * 콜백이 fio 코어에 완료된 io_u를 순서대로 돌려줄 때 사용.
	 * 설정자: fio_vsyncio_set_iov()가 큐잉 순서대로 io_us[queued]에 기록.
	 * 읽는 자: fio_vsyncio_event()가 인덱스 역참조하여 io_u 반환, fio_vsyncio_end()가
	 *          전송 바이트를 각 io_u에 resid로 분배할 때 순회.
	 * 값 범위: io_us[0..queued)는 유효 io_u 포인터, [queued..iodepth)는 stale(미정의).
	 * 동기화: 잡 스레드 전용. pvsync/pvsync2에서는 실질 unused. */

	unsigned int queued;
	/* [한국어] 현재 iovecs에 누적되어 commit을 기다리는 io_u 개수(vsync의 핵심 상태).
	 * 설정자: fio_vsyncio_set_iov()에서 queued++, fio_vsyncio_commit()에서 0으로 리셋.
	 *        fio_vsyncio_queue()의 "새 batch 시작" 경로에서도 방어적으로 0으로 리셋.
	 * 읽는 자: fio_vsyncio_queue() 병합 가능 여부 판단(큐가 비었는가, iodepth 도달했는가),
	 *          fio_vsyncio_commit()의 빈 큐 skip 판정과 lseek+readv/writev 벡터 길이 인자.
	 * 값 범위: 0..td->o.iodepth.
	 * 동기화: 잡 스레드 전용. */

	unsigned int events;
	/* [한국어] commit에서 이미 완료 처리되었으나 아직 fio 코어의 getevents가 소비하지
	 * 않은 "완료 이벤트" 수. 비동기 엔진의 completion queue와 같은 역할을 수행하지만
	 * 실제 I/O는 이미 동기 완료된 상태이므로 단순 카운터.
	 * 설정자: fio_vsyncio_commit()가 queued 값을 그대로 events에 이동,
	 *        fio_vsyncio_getevents()가 min>0일 때 0으로 소비.
	 * 읽는 자: fio_vsyncio_getevents()가 반환값으로 사용.
	 * 값 범위: 0..iodepth. 동기화: 잡 스레드 전용. */

	unsigned long queued_bytes;
	/* [한국어] 현재 누적 중인 iovecs 전체의 요청 바이트 합계. readv/writev 반환값과
	 * 비교해 "부분 전송"(short read/write)을 검출하는 데 쓰인다.
	 * 설정자: fio_vsyncio_set_iov()에서 += io_u->xfer_buflen, commit에서 리셋은 없지만
	 *        (end 완료 후 queued=0과 함께 효과적으로 0으로 리셋됨), 새 batch 시작 시
	 *        fio_vsyncio_queue()의 else 분기에서 명시적 0 리셋.
	 * 읽는 자: fio_vsyncio_end()의 "bytes == queued_bytes" 전체 전송 판정.
	 * 값 범위: 0..(iodepth × max_bs). */

	unsigned long long last_offset;
	/* [한국어] 마지막으로 누적된 io_u의 "끝 오프셋" = io_u->offset + io_u->xfer_buflen.
	 * 다음에 들어오는 io_u가 이 값과 같은 offset을 가지면 "연속 I/O"로 판정되어 iovec에
	 * append할 수 있다(vsync의 벡터 병합 트리거).
	 * 설정자: fio_vsyncio_set_iov()가 매 누적 시 갱신.
	 * 읽는 자: fio_vsyncio_append()가 io_u->offset == sd->last_offset 비교.
	 * 값 범위: 0..파일_크기 또는 -1ULL(무효, fio_vsyncio_init()의 초기값).
	 * 동기화: 잡 스레드 전용. */

	struct fio_file *last_file;
	/* [한국어] 마지막으로 큐에 누적된 io_u가 속한 fio_file 포인터. 새 io_u가 다른
	 * 파일을 대상으로 하면(다중 파일 잡) 연속 I/O로 병합할 수 없으므로 현재 batch를
	 * commit한 뒤 새 batch를 시작해야 한다.
	 * 설정자: fio_vsyncio_set_iov(). 읽는 자: fio_vsyncio_append()가 포인터 등가 비교.
	 * 값 범위: 유효한 fio_file 포인터 또는 NULL(init 직후). 동기화: 잡 스레드 전용. */

	enum fio_ddir last_ddir;
	/* [한국어] 마지막 I/O 방향(DDIR_READ 또는 DDIR_WRITE). 방향이 다르면 readv와
	 * writev를 섞을 수 없으므로(시스템 호출 자체가 다름) 벡터 병합 불가 → 별도 commit.
	 * 설정자: fio_vsyncio_set_iov(). 읽는 자: fio_vsyncio_append()와
	 * fio_vsyncio_commit()의 DDIR_READ ? readv : writev 분기.
	 * 값 범위: enum fio_ddir 중 DDIR_READ/DDIR_WRITE (SYNC/TRIM은 append 자체 거부).
	 * 동기화: 잡 스레드 전용. */

	struct frand_state rand_state;
	/* [한국어] pvsync2의 hipri_percentage 확률 판정용 잡별 난수 상태.
	 * 설정자: fio_vsyncio_init()가 init_rand(&sd->rand_state, 0)로 고정 seed 초기화
	 *        (잡별 결정적 난수를 원하면 추후 use_os_rand를 바꿀 수 있음).
	 * 읽는 자: fio_pvsyncio2_queue()가 rand_between(&sd->rand_state, 1, 100)으로 [1,100]
	 *          균등 난수를 뽑아 hipri_percentage와 비교.
	 * 값 범위: frand_state 내부는 불투명; 외부에서 접근하지 않음.
	 * 동기화: 잡 스레드 전용 — 락 불필요. */
};

/* [한국어] FIO_HAVE_PWRITEV2: configure 스크립트가 preadv2/pwritev2(Linux 4.6+ glibc
 * 2.26+)의 가용성을 런타임 컴파일 시점에 확인하면 config-host.h에 정의하는 매크로.
 * 이 빌드에서만 pvsync2 엔진과 관련 옵션 구조체/테이블이 생성된다(그 외 플랫폼에선
 * 이 블록 전체가 preprocessed-out). 해당 매크로가 없으면 아래 register 함수도
 * ioengine_pvrw2를 등록하지 않는다. */
#ifdef FIO_HAVE_PWRITEV2
/*
 * [한국어] struct psyncv2_options — pvsync2 엔진 전용 옵션 구조체.
 * preadv2(2)/pwritev2(2)의 마지막 flags 인자를 제어한다. RWF_* 플래그는 리눅스 커널의
 * per-I/O 요청 플래그로, 파일 O_DIRECT/O_SYNC 같은 파일 전역 속성과 독립적으로 개별
 * I/O 요청의 동작을 변경할 수 있다.
 *
 * 수명: 옵션 파서(options.c)가 잡 파일/CLI 파싱 시 calloc으로 할당, td->eo에 부착.
 *       잡 종료 시 fio 코어가 자동 해제.
 * 공유: 잡 스레드가 queue 콜백에서 읽기만 함 — 파서가 잡 시작 전 세팅 후 불변.
 */
struct psyncv2_options {
	void *pad;
	/* [한국어] fio 옵션 파서가 요구하는 구조체 선두 패딩(관례적 더미).
	 * 설정자: 아무도 쓰지 않음. 읽는 자: 없음.
	 * 값 범위: 미사용. 동기화: 불필요.
	 * 존재 이유: 레거시로 옵션 구조체 첫 필드가 포인터 크기 정렬되도록 예약한 슬롯. */

	unsigned int hipri;
	/* [한국어] RWF_HIPRI 요청 여부 — 리눅스 블록 레이어의 "polling" 경로를 활성화.
	 * 커널은 이 플래그가 set된 I/O를 io_submit 후 블록 드라이버의 polling 메서드로
	 * 완료를 busy-poll하여 IRQ 지연 없이 즉각 회수한다(주로 NVMe + poll 큐).
	 * 설정자: 옵션 파서가 잡 파일의 "hipri" 스위치(FIO_OPT_STR_SET — 값 없이 존재만으로 1).
	 * 읽는 자: fio_pvsyncio2_queue()가 hipri && 확률 추첨 통과 시 flags |= RWF_HIPRI.
	 * 값 범위: 0(비활성) | 1(활성). 부분 적용은 아래 hipri_percentage로 조절. */

	unsigned int hipri_percentage;
	/* [한국어] hipri=1일 때 실제 RWF_HIPRI를 붙일 확률(퍼센트). 폴링/IRQ 혼합 워크로드
	 * 시뮬레이션에 사용(예: 지연 민감 I/O만 hipri, 나머지는 IRQ).
	 * 설정자: 옵션 파서(FIO_OPT_INT, 기본값 def="100"). 읽는 자: fio_pvsyncio2_queue()의
	 * rand_between(&sd->rand_state, 1, 100) <= hipri_percentage 비교 — [1,100] 균등난수가
	 * 이 값 이하이면 실제로 플래그 set.
	 * 값 범위: 0..100 (파서가 minval/maxval로 강제). */

	unsigned int uncached;
	/* [한국어] RWF_DONTCACHE(=RWF_UNCACHED) — buffered I/O에서 페이지 캐시 우회 힌트.
	 * 커널은 이 플래그가 set된 read/write의 데이터 페이지를 I/O 완료 후 LRU에서 제거해
	 * 캐시 오염(cache pollution)을 줄인다. O_DIRECT와 달리 정렬 제약이 없어 임의 버퍼/
	 * 오프셋/크기에 사용 가능.
	 * 설정자: 옵션 파서(FIO_OPT_INT). 읽는 자: fio_pvsyncio2_queue().
	 * 값 범위: 0|1. 단, td->o.odirect=1(O_DIRECT)와 동시 사용 시 의미 없음(queue가 조건부로 skip). */

	unsigned int nowait;
	/* [한국어] RWF_NOWAIT — 요청이 블로킹되어야 할 상황(페이지 캐시 미스 후 디스크 I/O
	 * 필요, 락 대기, 파일 확장 필요 등)에서 EAGAIN을 즉시 반환하도록 하는 플래그.
	 * 비동기 엔진(io_uring 등)이 fallback 경로로 자주 사용; pvsync2는 EAGAIN을 에러로
	 * 받아 fio_io_end()에서 io_u->error에 저장 → 재시도는 fio 코어 정책에 위임.
	 * 설정자: 옵션 파서(FIO_OPT_BOOL). 읽는 자: fio_pvsyncio2_queue().
	 * 값 범위: 0|1. */
};

/* [한국어] pvsync2 엔진의 옵션 테이블 — fio_option 배열. NULL .name 센티널로 종료.
 * 각 엔트리가 잡 파일의 "key=value" 한 줄에 대응. ioengine_pvrw2.options에 연결되어
 * 파서가 옵션 이름을 매칭하면 off1 오프셋의 필드에 파싱된 값을 기입한다. */
static struct fio_option options[] = {
	{
		.name	= "hipri",
		/* [한국어] 잡 파일에서 사용자가 입력하는 옵션 키("hipri"). */
		.lname	= "RWF_HIPRI",
		/* [한국어] 긴 이름(long name) — --help 출력과 문서 자동 생성에 사용. 플래그의 정식 명칭 노출. */
		.type	= FIO_OPT_STR_SET,
		/* [한국어] "값 없이 존재만으로 1"인 boolean 스위치 타입. "hipri"만 쓰면 1, 생략하면 0. */
		.off1	= offsetof(struct psyncv2_options, hipri),
		/* [한국어] 파서가 값을 저장할 구조체 내 필드 오프셋(bytes) — psyncv2_options.hipri로 주소 계산. */
		.help	= "Set RWF_HIPRI for pwritev2/preadv2",
		/* [한국어] --help/--cmdhelp 에 노출되는 한 줄 도움말. */
		.category = FIO_OPT_C_ENGINE,
		/* [한국어] 옵션 카테고리: "I/O 엔진" — fio HOWTO 자동 분류 및 help 섹션 그룹핑. */
		.group	= FIO_OPT_G_INVALID,
		/* [한국어] 세부 그룹 미지정(이 엔진 옵션은 별도 그룹이 필요 없음). */
	},
	{
		.name	= "hipri_percentage",
		/* [한국어] "hipri_percentage=NN" 형태로 1..100 사이 정수 입력. */
		.lname	= "RWF_HIPRI_PERCENTAGE",
		/* [한국어] 긴 이름 — 기능 설명용. */
		.type	= FIO_OPT_INT,
		/* [한국어] 정수 타입(십진수 파싱). */
		.off1	= offsetof(struct psyncv2_options, hipri_percentage),
		/* [한국어] psyncv2_options.hipri_percentage에 저장. */
		.minval	= 0,
		/* [한국어] 허용 최솟값 0 — 파서가 범위 밖 입력을 거부. */
		.maxval	= 100,
		/* [한국어] 허용 최댓값 100 — 퍼센트 의미. */
		.def    = "100",
		/* [한국어] 기본값(문자열로 지정 후 파서가 정수로 변환) — hipri=1이면 기본 100%로 모든 I/O에 RWF_HIPRI. */
		.help	= "Probabilistically set RWF_HIPRI for pwritev2/preadv2",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "uncached",
		.lname	= "Uncached",
		.type	= FIO_OPT_INT,
		/* [한국어] 정수 타입 — 0|1 외에도 향후 확장 여지를 위한 INT로 선언(현재는 boolean처럼 사용). */
		.off1	= offsetof(struct psyncv2_options, uncached),
		.help	= "Use RWF_DONTCACHE for buffered read/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "nowait",
		.lname	= "RWF_NOWAIT",
		.type	= FIO_OPT_BOOL,
		/* [한국어] 엄밀한 boolean 타입("0"/"1"/"true"/"false"/"yes"/"no" 허용) — uncached와 달리 BOOL 사용. */
		.off1	= offsetof(struct psyncv2_options, nowait),
		.help	= "Set RWF_NOWAIT for pwritev2/preadv2",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= NULL,
		/* [한국어] 센티널 엔트리 — 파서가 이 .name==NULL을 보고 테이블 끝을 인식. */
	},
};
#endif

/*
 * [한국어]
 * fio_syncio_prep - "sync" 엔진의 prep 콜백: 필요한 경우 lseek으로 파일 포인터 이동.
 *
 * @td: 잡 컨텍스트. lseek 실패 시 td_verror로 잡 전역 오류 보고.
 * @io_u: 이번에 수행할 I/O 유닛. io_u->file(대상 파일), io_u->ddir(방향),
 *        io_u->offset(요청 오프셋) 필드가 유효해야 한다.
 * @return: 0 = 성공 또는 skip(lseek 불필요), 1 = lseek 시스템 호출 실패.
 *          fio 코어는 0이 아닌 값을 받으면 이 io_u를 에러로 처리해 queue 단계로
 *          진입하지 않는다.
 *
 * 왜 필요한가: read(2)/write(2)는 커널의 "file description" 내부 파일 포인터(f_pos)
 * 에 의존해 해당 위치에서 읽고 쓴 뒤 자동으로 전진시킨다. fio는 시퀀셜/랜덤 혼합
 * 패턴을 테스트하므로 io_u->offset이 파일 포인터의 현재 위치와 같다는 보장이 없다.
 * 따라서 매 I/O 전 lseek으로 포인터를 재배치해야 한다. 단, 직전 I/O가 정확히 이
 * io_u의 offset 직전에서 끝났다면(순차 I/O) lseek syscall을 생략해 시스템 호출
 * 절반(lseek 1회 + read/write 1회 → read/write 1회)을 아낄 수 있다. 이 최적화는
 * LAST_POS(f) 캐시로 구현된다.
 *
 * 대안과 비교:
 *   · psync: pread/pwrite가 오프셋을 인자로 받으므로 prep이 불필요(lseek 호출 0회).
 *   · pvsync/pvsync2: preadv/pwritev(2)도 오프셋 인자 지원 → 역시 prep 불필요.
 *   · vsync: readv/writev도 파일 포인터 기반 → commit 시점에 lseek 1회.
 * 즉 sync 엔진만 prep에서 lseek을 관리한다. 멀티스레드가 동일 fd를 공유하면
 * "lseek → read/write" 사이 레이스가 발생하므로 fio는 sync 엔진 잡을 fork하거나
 * 잡당 파일을 분리하는 방식으로 회피한다.
 *
 * 호출 체인: backend.c: thread_main → do_io → td_io_prep [ioengines.c] →
 *   [이 함수 (ioengine_rw.prep == fio_syncio_prep)] → lseek(2) 시스템 호출.
 * 실행 컨텍스트: 잡 스레드 단독. 재진입 없음.
 */
static int fio_syncio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	/* [한국어] 대상 파일 구조체 포인터 추출 — 이후 f->fd(커널 fd)와 f->engine_pos
	 * (LAST_POS 캐시)에 접근. io_u 자체는 파일을 간접 참조만 하므로 지역 변수로 캐시. */

	if (!ddir_rw(io_u->ddir))
		return 0;
	/* [한국어] ddir_rw 매크로: ddir == DDIR_READ || ddir == DDIR_WRITE 판정.
	 * DDIR_TRIM(디스카드), DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE 등은 파일 포인터와
	 * 무관하므로 lseek 불필요 — 즉시 성공 반환. queue 콜백이 do_io_u_trim/sync로
	 * 분기 처리한다. */

	if (LAST_POS(f) != -1ULL && LAST_POS(f) == io_u->offset)
		return 0;
	/* [한국어] LAST_POS 최적화: 직전 I/O 종료 시 갱신한 캐시가 (1) 유효(-1ULL 무효
	 * 센티널이 아님)하고 (2) 이번 요청 offset과 정확히 일치하면, 커널의 파일 포인터가
	 * 이미 거기 있다는 뜻 → lseek 생략. 첫 I/O이거나 랜덤 I/O 중 우연히 연속이
	 * 발생한 경우에도 이 최적화가 적용된다. */

	if (lseek(f->fd, io_u->offset, SEEK_SET) == -1) {
		td_verror(td, errno, "lseek");
		return 1;
	}
	/* [한국어] lseek(2) 시스템 호출: 커널에 "이 fd의 파일 포인터를 SEEK_SET 기준
	 * (파일 시작)에서 io_u->offset 바이트로 이동하라"고 요청. 성공 시 새 포인터
	 * 위치를 반환(여기선 무시), 실패 시 -1 반환하고 errno 설정. 실패 원인: ESPIPE
	 * (파이프/소켓은 seek 불가), EBADF(잘못된 fd), EINVAL(whence 오류) 등.
	 * td_verror는 잡 전역 에러 플래그 세팅 + 에러 메시지 누적 — 이후 해당 잡이 중단됨. */

	return 0;
	/* [한국어] lseek 성공 시 파일 포인터가 io_u->offset으로 이동 완료 — queue 단계에서
	 * read/write가 이 위치부터 전송을 시작한다. */
}

/*
 * [한국어]
 * fio_io_end - 단일 syscall 반환값을 fio의 완료 상태로 환산하는 공통 헬퍼.
 *
 * @td: 잡 컨텍스트 — 에러를 td_verror로 전파할 때 사용.
 * @io_u: 완료 처리할 I/O 유닛 — resid/error 필드를 설정한다.
 * @ret: 시스템 호출 반환값. 0 이상이면 실제 전송된 바이트 수, -1이면 errno 설정된 오류.
 * @return: 항상 FIO_Q_COMPLETED — 동기 엔진이므로 호출 시점에 I/O가 이미 완료된
 *          상태이며, fio 코어는 이 반환값을 받아 io_u를 in_flight → completed로
 *          즉시 전이시킨다(추가 getevents/event 호출 없이 put_io_u 진행).
 *
 * 공통 사용: 본 파일의 sync/psync/pvsync/pvsync2 queue 콜백이 모두 이 헬퍼로 마감한다.
 * vsync만 별도의 fio_vsyncio_end()를 쓰는데, 이는 iovec 벡터 전체를 다루고 반환값을
 * 여러 io_u에 분배해야 하기 때문이다.
 *
 * 처리 단계:
 *   1) 정상 R/W(ret >= 0 && ddir_rw)인 경우 LAST_POS(file)을 "offset + ret"로 전진시켜
 *      sync 엔진의 다음 prep에서 lseek 생략 가능성을 판단하도록 캐시한다. psync/pvsync*
 *      에서도 LAST_POS가 갱신되지만 이들은 prep에서 이 값을 읽지 않으므로 무해한 side effect.
 *   2) 전송 바이트(ret)가 요청 크기(xfer_buflen)에 미달하면 "short I/O" → io_u->resid에
 *      잔여 바이트를 기록. short I/O는 파이프/소켓/파일 끝 등에서 정상적으로 발생 가능.
 *   3) ret == -1이면 errno를 io_u->error에 복사하고 td_verror로 잡 전역 오류 전파.
 *
 * 호출 체인: fio_syncio_queue / fio_psyncio_queue / fio_pvsyncio_queue /
 *   fio_pvsyncio2_queue → [이 함수] → (에러 시) io_u_log_error + td_verror.
 * 실행 컨텍스트: 잡 스레드 단독.
 */
static int fio_io_end(struct thread_data *td, struct io_u *io_u, int ret)
{
	if (io_u->file && ret >= 0 && ddir_rw(io_u->ddir))
		LAST_POS(io_u->file) = io_u->offset + ret;
	/* [한국어] 세 조건을 모두 만족하는 "성공한 R/W"의 경우에만 LAST_POS 갱신:
	 *   · io_u->file != NULL: 파일이 연결된 실제 I/O(sync/flush ddir 일부는 file 없을 수 있음).
	 *   · ret >= 0: 시스템 호출 성공(short I/O 포함).
	 *   · ddir_rw: READ 또는 WRITE만 파일 포인터를 전진시킴(TRIM/SYNC는 무관).
	 * 부분 전송(short I/O)이어도 커널은 파일 포인터를 실제로 전진시킨 바이트만큼만
	 * 이동시키므로 "offset + ret"가 정확한 다음 위치이다. */

	if (ret != (int) io_u->xfer_buflen) {
		/* [한국어] 요청한 전체 바이트를 전송하지 못한 경우 — short I/O 또는 오류.
		 * (int) 캐스트: xfer_buflen은 unsigned이지만 ret은 signed int이므로 비교 전
		 * 부호를 맞춤(음수는 오류 의미, 양수는 signed 범위 내에서 비교). */
		if (ret >= 0) {
			io_u->resid = io_u->xfer_buflen - ret;
			/* [한국어] short read/write: 잔여 바이트를 resid에 기록.
			 * fio 코어는 resid를 보고 통계(완료 바이트)에서 차감하거나 재시도 정책을 적용. */
			io_u->error = 0;
			/* [한국어] short I/O는 오류가 아님 — error=0으로 명시. */
			return FIO_Q_COMPLETED;
			/* [한국어] 즉시 반환 — 아래 error 분기를 건너뛰어 td_verror 호출을 방지. */
		} else
			io_u->error = errno;
			/* [한국어] 오류 경로: ret == -1 → errno(스레드 지역 변수)를 io_u->error에 복사.
			 * errno는 직후 다른 syscall(심지어 log_err 같은 로깅 호출)로 덮일 수 있으므로
			 * 여기서 즉시 캡처하는 것이 중요. fio_vsyncio_end()도 같은 이유로 err 캐시. */
	}

	if (io_u->error) {
		io_u_log_error(td, io_u);
		/* [한국어] 디버그용 에러 로그(FD_IO 채널) — 실패한 I/O의 offset/buflen/ddir/file 등을 출력.
		 * dprint(FD_IO, ...)로 내부 진단 정보를 남김. */
		td_verror(td, io_u->error, "xfer");
		/* [한국어] 잡 전역 오류로 승격 — td->error 필드에 errno 기록, td->verror 문자열에
		 * "xfer" + errno 해석 메시지 누적. fio 코어가 do_io 루프 종료 후 해당 잡을 에러로 마감. */
	}

	return FIO_Q_COMPLETED;
	/* [한국어] 동기 엔진이므로 정상/부분/실패 모두 즉시 COMPLETED 반환.
	 * 코어가 io_u->error != 0이면 실패로 통계 집계, resid > 0이면 short로 집계. */
}

/*
 * [한국어]
 * fio_pvsyncio_queue - "pvsync" 엔진의 queue 콜백: preadv/pwritev로 **즉시** 동기 실행.
 *
 * @td: 잡 컨텍스트. @io_u: 이번 I/O 요청(file/ddir/offset/xfer_buf/xfer_buflen).
 * @return: FIO_Q_COMPLETED — fio_io_end()가 환산한 완료 상태를 그대로 반환.
 *
 * vsync와의 차이점: vsync는 연속 I/O를 iovec에 누적(FIO_Q_QUEUED)한 뒤 commit 단계에서
 * readv/writev를 한 번에 호출하여 scatter/gather를 실제로 활용한다. 반면 pvsync는 "벡터
 * I/O"라는 이름에도 불구하고 **iovec 원소 1개만** 사용해 매 요청을 즉시 발행한다.
 * 그럼 preadv/pwritev를 쓰는 이유는? (1) pread/pwrite 대비 동일한 오프셋 인자 지원 →
 * 파일 포인터 독립(psync와 동일), (2) 향후 io_u fragmentation 같은 확장 여지, (3) 일부
 * 커널 구현에서 pwritev가 pwrite보다 효율적일 수 있다는 마이크로 최적화 탐색 용도.
 *
 * 시스템 호출:
 *   · preadv(fd, iov, iovcnt, offset): 커널이 각 iovec[i]의 iov_base에 iov_len 바이트씩
 *     순서대로 읽어 담음. offset은 기준이며 파일 포인터는 변경 안 함. 성공 시 전체
 *     읽은 바이트 수 반환. POSIX 2008.
 *   · pwritev(...): 대응되는 쓰기. "모든 iov 처리 후" 한 번에 원자적 쓰기가 보장되지는
 *     않음(atomic은 RWF_ATOMIC이 필요, pvsync2만 사용).
 *
 * 호출 체인: backend.c do_io → td_io_queue → ioengine_pvrw.queue(=이 함수) →
 *   preadv(2)/pwritev(2) 또는 do_io_u_trim(TRIM) / do_io_u_sync(SYNC*) → fio_io_end().
 */
#ifdef CONFIG_PWRITEV
static enum fio_q_status fio_pvsyncio_queue(struct thread_data *td,
					    struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] fio_vsyncio_init()에서 잡 초기화 시 할당해둔 상태 구조체 포인터.
	 * pvsync는 iovecs[0]만 쓰고, 나머지 필드(io_us/queued/last_*/rand_state)는 unused. */
	struct iovec *iov = &sd->iovecs[0];
	/* [한국어] 단일 벡터 사용 — 인덱스 0만 씀. iovec 배열의 나머지는 pvsync에서 미사용
	 * 이지만 fio_vsyncio_init()가 iodepth만큼 malloc해둠(공용 init 재사용 때문). */
	struct fio_file *f = io_u->file;
	/* [한국어] 대상 파일(커널 fd 포함). preadv/pwritev 인자로 f->fd 전달. */
	int ret;
	/* [한국어] syscall 반환값 저장 — 전송 바이트 수(>=0) 또는 -1(errno 설정). */

	fio_ro_check(td, io_u);
	/* [한국어] fio_ro_check 매크로: readonly=1 잡인데 io_u->ddir == DDIR_WRITE면
	 * assert로 프로그램 중단. 테스트 안전망 — 설정 모순을 조기에 드러냄. */

	iov->iov_base = io_u->xfer_buf;
	/* [한국어] iovec 원소의 버퍼 포인터에 io_u의 사용자 공간 버퍼 주소를 연결(zero-copy).
	 * 커널은 이 주소를 get_user_pages로 pin한 뒤 DMA 소스/목적지로 사용. */
	iov->iov_len = io_u->xfer_buflen;
	/* [한국어] 전송할 바이트 수(xfer_buflen). 보통 4KiB ~ 1MiB 사이. */

	if (io_u->ddir == DDIR_READ)
		ret = preadv(f->fd, iov, 1, io_u->offset);
		/* [한국어] preadv(2) 시스템 호출 — 벡터 길이 1로 호출.
		 * 커널은 VFS → 파일시스템 → 페이지 캐시(있으면 hit, 없으면 블록 레이어로 I/O 발행).
		 * 성공 시 실제 읽은 바이트 수, EOF 시 0, 실패 시 -1 반환. */
	else if (io_u->ddir == DDIR_WRITE)
		ret = pwritev(f->fd, iov, 1, io_u->offset);
		/* [한국어] pwritev(2) — 벡터 길이 1. 커널은 페이지 캐시에 쓰고(버퍼드) 또는
		 * 블록 레이어로 직접 보냄(O_DIRECT). O_SYNC/O_DSYNC가 아니면 fsync 전까지 휘발성. */
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		/* [한국어] 블록 디바이스(BLKDISCARD ioctl) 또는 파일시스템(fallocate PUNCH_HOLE)
		 * 기반 디스카드 발행. fio 코어 헬퍼가 파일 유형별로 분기. */
		return FIO_Q_COMPLETED;
		/* [한국어] TRIM은 fio_io_end 경유하지 않고 즉시 완료 반환(오류는 do_io_u_trim 내부에서 처리). */
	} else
		ret = do_io_u_sync(td, io_u);
		/* [한국어] DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE 계열 — ddir 값에 따라 fsync(2)/
		 * fdatasync(2)/sync_file_range(2) 디스패치. 커널에 파일 메타데이터+데이터 영속화 요청. */

	return fio_io_end(td, io_u, ret);
	/* [한국어] 반환값 → resid/error 환산 후 FIO_Q_COMPLETED 반환. */
}
#endif

/*
 * [한국어]
 * fio_pvsyncio2_queue - "pvsync2" 엔진 queue: preadv2(2)/pwritev2(2) + RWF_* 플래그 조합.
 *
 * @td: 잡. @io_u: I/O 요청. @return: FIO_Q_COMPLETED.
 *
 * pvsync의 상위호환 — 리눅스 4.6+에서 도입된 preadv2/pwritev2 시스템 호출이 추가
 * 받는 마지막 인자 "flags"(per-I/O RWF_* 플래그)를 활용한다. 핵심 플래그:
 *   · RWF_HIPRI (Linux 4.6+): 블록 레이어 polling 경로 활성화. NVMe poll queue에
 *     I/O를 올리고 IRQ 대신 busy-poll로 완료 수확(지연↓, CPU↑). fio 옵션 hipri=1로
 *     활성, hipri_percentage로 적용 비율 조절.
 *   · RWF_DONTCACHE (Linux 6.14+, a.k.a. RWF_UNCACHED): 버퍼드 I/O인데 완료 후 해당
 *     페이지를 캐시에서 제거 → cache pollution 억제. O_DIRECT와 달리 정렬 제약 없음.
 *   · RWF_NOWAIT (Linux 4.14+): 블로킹 발생할 상황이면 EAGAIN 즉시 반환. 비동기 엔진의
 *     fallback 경로로 사용되지만 pvsync2에서는 단순히 EAGAIN을 io_u->error에 기록.
 *   · RWF_ATOMIC (Linux 6.11+, write only): torn-write 방지 — 전체 write가 중간 상태
 *     없이 atomic하게 반영되거나 실패. FS/블록 디바이스 지원 필요(보통 NVMe atomic
 *     write boundary와 정렬). fio 옵션 --atomic=1 + FIO_ATOMICWRITES 엔진 플래그.
 * 확률적 HIPRI: hipri=1일 때 매 I/O마다 [1,100] 균등 난수를 뽑아 hipri_percentage
 * 이하면 실제로 RWF_HIPRI를 부여. 예: hipri_percentage=30이면 약 30% I/O만 폴링 경로.
 *
 * 매핑: struct psyncv2_options 필드 → 런타임 flags 비트
 *   · o->hipri     → 확률 통과 시 RWF_HIPRI
 *   · o->uncached  → (O_DIRECT 아닐 때만) RWF_DONTCACHE
 *   · o->nowait    → RWF_NOWAIT
 *   · td->o.oatomic → (write일 때만) RWF_ATOMIC  (잡 전역 옵션)
 *
 * 호출 체인: td_io_queue → ioengine_pvrw2.queue(=이 함수) → preadv2(2)/pwritev2(2) 또는
 *   do_io_u_trim/sync → fio_io_end().
 */
#ifdef FIO_HAVE_PWRITEV2
static enum fio_q_status fio_pvsyncio2_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] 공용 init이 만든 상태(rand_state로 hipri 확률 판정, iovecs[0]로 벡터 전달). */
	struct psyncv2_options *o = td->eo;
	/* [한국어] 엔진 전용 옵션 구조체(td->eo = engine options). 파서가 잡 시작 전 세팅. */
	struct iovec *iov = &sd->iovecs[0];
	/* [한국어] 단일 벡터 사용 — pvsync와 동일한 iovec[0] 재활용 패턴. */
	struct fio_file *f = io_u->file;
	/* [한국어] 대상 파일(f->fd가 커널 fd). */
	int ret, flags = 0;
	/* [한국어] ret: syscall 반환값. flags: RWF_* 플래그 누적 비트마스크, 초기 0. */

	fio_ro_check(td, io_u);
	/* [한국어] readonly 잡에서 write 요청 차단 — 설정 모순 방어. */

	if (o->hipri &&
	    (rand_between(&sd->rand_state, 1, 100) <= o->hipri_percentage))
		flags |= RWF_HIPRI;
	/* [한국어] 확률적 HIPRI 부여 로직:
	 *   · 먼저 o->hipri=1(옵션 활성)인지 확인 — 단락 평가로 rand 호출 생략 가능.
	 *   · rand_between(&sd->rand_state, 1, 100): 잡별 난수 상태에서 [1,100] 균등 정수 추출.
	 *   · 결과가 o->hipri_percentage 이하이면 RWF_HIPRI 세팅.
	 * 예: hipri_percentage=100이면 항상 set(기본값), 50이면 절반만, 0이면 거의 set 안 됨. */
	if (!td->o.odirect && o->uncached)
		flags |= RWF_DONTCACHE;
	/* [한국어] RWF_DONTCACHE 조건부 세팅: td->o.odirect=0(버퍼드 I/O)이면서 o->uncached=1
	 * 일 때만. O_DIRECT는 이미 페이지 캐시를 우회하므로 DONTCACHE 힌트가 중복/무의미 —
	 * 이 경우 플래그를 안 붙여 커널의 불필요한 검증을 건너뛰게 한다. */
	if (o->nowait)
		flags |= RWF_NOWAIT;
	/* [한국어] RWF_NOWAIT 세팅: 블로킹 상황(페이지 캐시 미스, 락 대기, 파일 확장 등)이면
	 * 커널이 -EAGAIN 즉시 반환하도록 함. pvsync2는 이를 io_u->error에 기록해 fio 코어로 전파. */

	iov->iov_base = io_u->xfer_buf;
	/* [한국어] 사용자 버퍼 포인터 — preadv2/pwritev2에 전달되어 커널이 pin 후 DMA. */
	iov->iov_len = io_u->xfer_buflen;
	/* [한국어] 전송 바이트 수. */

	if (io_u->ddir == DDIR_READ)
		ret = preadv2(f->fd, iov, 1, io_u->offset, flags);
		/* [한국어] preadv2(2) — 벡터 길이 1 + 오프셋 + RWF_* 플래그.
		 * glibc wrapper가 syscall(__NR_preadv2, ...) 실행 → 커널 fs/read_write.c 진입.
		 * RWF_HIPRI면 blkdev iopoll 경로 사용, RWF_DONTCACHE면 완료 후 invalidate. */
	else if (io_u->ddir == DDIR_WRITE) {
		if (td->o.oatomic)
			flags |= RWF_ATOMIC;
		/* [한국어] torn-write 방지 atomic write. FIO_ATOMICWRITES 엔진 플래그가 선언되어
		 * 있어야 fio 코어가 --atomic 옵션을 허용. 커널/파일시스템/블록 디바이스 지원 필요 —
		 * 미지원 시 EINVAL. NVMe의 atomic write unit (AWUN/AWUPF) 경계 내에서만 보장. */
		ret = pwritev2(f->fd, iov, 1, io_u->offset, flags);
		/* [한국어] pwritev2(2) — write 경로. RWF_SYNC/RWF_DSYNC(O_SYNC/O_DSYNC 동등) 플래그는
		 * 여기선 사용하지 않지만 일반적으로 조합 가능. */
	} else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		/* [한국어] TRIM은 RWF_* 플래그 의미 없음 — 공용 discard 헬퍼로 바로 위임. */
		return FIO_Q_COMPLETED;
		/* [한국어] 조기 반환 — fio_io_end 우회. */
	} else
		ret = do_io_u_sync(td, io_u);
		/* [한국어] DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE — 플래그 무관, 공용 헬퍼로 위임. */

	return fio_io_end(td, io_u, ret);
	/* [한국어] 반환값 환산 — RWF_NOWAIT로 EAGAIN 반환된 경우에도 io_u->error=EAGAIN으로 기록됨. */
}
#endif

/*
 * [한국어]
 * fio_psyncio_queue - "psync" 엔진 queue: pread(2)/pwrite(2) 즉시 실행.
 *
 * @td: 잡. @io_u: I/O 요청. @return: FIO_Q_COMPLETED.
 *
 * psync는 sync의 변형으로, read/write 대신 "positional" pread/pwrite를 사용한다.
 * pread/pwrite는 파일 포인터에 의존하지 않고 offset 인자로 직접 위치를 지정하므로:
 *   · prep 단계가 불필요(lseek 생략) → ioengine_prw에서 .prep 필드 비어 있음.
 *   · 파일 포인터를 건드리지 않으므로 멀티스레드가 동일 fd를 공유해도 레이스 없음.
 * 리눅스에서는 pread/pwrite 내부적으로 __sys_preadv/__sys_pwritev가 1원소 iovec로
 * 실행되므로 pvsync와 거의 동일한 성능이지만, RWF_* 플래그를 붙일 수 없다.
 *
 * 호출 체인: td_io_queue → ioengine_prw.queue(=이 함수) → pread(2)/pwrite(2) 또는
 *   do_io_u_trim/sync → fio_io_end().
 */
static enum fio_q_status fio_psyncio_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	/* [한국어] 대상 파일(f->fd가 커널 fd). psync는 syncio_data를 쓰지 않음(td->io_ops_data=NULL). */
	int ret;
	/* [한국어] syscall 반환값. */

	fio_ro_check(td, io_u);
	/* [한국어] readonly 잡에서 write 차단. */

	if (io_u->ddir == DDIR_READ)
		ret = pread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
		/* [한국어] pread(2) — fd/buf/count/offset 4-인자 위치 지정 읽기. 파일 포인터 불변.
		 * 성공 시 실제 읽은 바이트 수(0=EOF), 실패 시 -1(errno 설정). 커널 경로는 preadv와
		 * 내부적으로 거의 동일(iov 1개 암시). */
	else if (io_u->ddir == DDIR_WRITE)
		ret = pwrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
		/* [한국어] pwrite(2) — 위치 지정 쓰기. 파일 포인터 불변. O_APPEND 파일에선 offset이
		 * 무시되고 항상 끝에 append(POSIX 명세). */
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		/* [한국어] 블록 디바이스/파일시스템 discard 발행 — 공용 헬퍼. */
		return FIO_Q_COMPLETED;
		/* [한국어] 조기 반환. */
	} else
		ret = do_io_u_sync(td, io_u);
		/* [한국어] DDIR_SYNC 계열 — fsync/fdatasync/sync_file_range. */

	return fio_io_end(td, io_u, ret);
	/* [한국어] 반환값 → resid/error 환산. */
}

/*
 * [한국어]
 * fio_syncio_queue - "sync"(기본) 엔진 queue: read(2)/write(2) 동기 호출.
 *
 * @td: 잡. @io_u: I/O 요청. @return: FIO_Q_COMPLETED.
 *
 * fio의 "기본 엔진" — ioengine 옵션을 생략하면 이 엔진이 선택된다. 다른 모든 엔진의
 * 성능 비교 baseline. read/write는 파일 포인터에 의존하므로 fio_syncio_prep()이 먼저
 * LAST_POS를 확인하고 필요시 lseek을 수행했다고 가정한다. 이 함수는 lseek 없이 순수
 * 전송만 수행 → prep과 queue로 분리된 구조 덕분에 lseek이 생략된 경우 오버헤드 최소.
 *
 * 주의: read/write는 POSIX 명세상 멀티스레드 공유 fd에서 "lseek 이후 read 사이" 레이스
 * 가능성이 있다(POSIX.1-2001은 단일 syscall 호출 자체는 atomic이지만 lseek+read 쌍은 아님).
 * fio는 sync 엔진 잡을 기본적으로 fork로 실행하거나 잡당 별도 파일을 열어 이 문제를 회피.
 *
 * 호출 체인: td_io_queue → ioengine_rw.queue(=이 함수) → read(2)/write(2) 또는
 *   do_io_u_trim/sync → fio_io_end().
 */
static enum fio_q_status fio_syncio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	/* [한국어] 대상 파일 — f->fd(커널 fd)와 f->engine_pos(LAST_POS)를 담음. */
	int ret;
	/* [한국어] syscall 반환값 저장. */

	fio_ro_check(td, io_u);
	/* [한국어] RO 잡 write 방어. */

	if (io_u->ddir == DDIR_READ)
		ret = read(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
		/* [한국어] read(2) — 파일 포인터 위치에서 xfer_buflen 바이트를 xfer_buf로 읽음.
		 * 성공 시 실제 읽은 바이트 수, EOF 시 0, 실패 시 -1(errno). 파일 포인터가 전송
		 * 바이트만큼 자동 전진 — 다음 prep에서 LAST_POS로 lseek 생략 판단. */
	else if (io_u->ddir == DDIR_WRITE)
		ret = write(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
		/* [한국어] write(2) — 파일 포인터 위치에 쓰기. 파일 포인터 자동 전진.
		 * 페이지 캐시로 들어가며 fsync 전까지 휘발성(O_SYNC/O_DSYNC가 아닐 때). */
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		/* [한국어] 공용 discard 헬퍼. */
		return FIO_Q_COMPLETED;
	} else
		ret = do_io_u_sync(td, io_u);
		/* [한국어] SYNC* 계열. */

	return fio_io_end(td, io_u, ret);
	/* [한국어] 반환값 환산. */
}

/*
 * [한국어]
 * fio_vsyncio_getevents - vsync 계열의 getevents 콜백: commit에서 이미 완료된 이벤트
 *                         카운터를 fio 코어에 보고.
 *
 * @td: 잡 컨텍스트.
 * @min: 최소 완료 이벤트 수(호출자 요구). 0이면 소비 없이 0 반환(polling 목적).
 *       >0이면 누적된 events를 한 번에 모두 드레인.
 * @max: (미사용) 최대 수집 수 — fio_unused 매크로로 컴파일러 경고 억제.
 * @t:   (미사용) 타임아웃 — vsync는 이미 동기 완료 상태라 대기 불필요.
 * @return: 보고할 이벤트 수(0..iodepth).
 *
 * vsync 엔진은 commit() 시점에 readv/writev로 모든 I/O를 동기 완료시킨 뒤 sd->events
 * 에 "커밋된 개수"를 기록해둔다. 따라서 이 getevents는 실제 대기 없이 단순히 카운터를
 * 반환하고 소비 표시만 한다(비동기 엔진의 completion queue와 외견상 동일한 계약 유지).
 *
 * 호출 체인: backend.c do_io → td_io_getevents → [이 함수].
 */
static int fio_vsyncio_getevents(struct thread_data *td, unsigned int min,
				 unsigned int max,
				 const struct timespec fio_unused *t)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] 잡별 vsync 상태 구조체 추출. */
	int ret;
	/* [한국어] 반환할 이벤트 수. */

	if (min) {
		ret = sd->events;
		/* [한국어] 커밋으로 쌓인 모든 완료 수를 한 번에 보고. */
		sd->events = 0;
		/* [한국어] 한 번에 모두 소비 — 다음 커밋 전까지 0. */
	} else
		ret = 0;
		/* [한국어] min=0(논블록 polling): 아무것도 소비하지 않고 0 반환. */

	dprint(FD_IO, "vsyncio_getevents: min=%d,max=%d: %d\n", min, max, ret);
	/* [한국어] FD_IO 채널 디버그 로그 — fio --debug=io로 활성화 시 출력. */
	return ret;
	/* [한국어] fio 코어에 완료 수 통지 — 코어는 이후 event(i)를 ret번 호출. */
}

/*
 * [한국어]
 * fio_vsyncio_event - getevents가 보고한 인덱스로 io_us 배열에서 io_u를 반환.
 *
 * @td: 잡 컨텍스트.
 * @event: 0..(직전 getevents 반환값 - 1) 범위의 인덱스. commit 시점의 큐 스냅샷에
 *         대응하며 FIFO 순서로 io_u를 반환한다.
 * @return: 해당 인덱스의 io_u 포인터(NULL 불가).
 *
 * 호출 체인: td_io_getevents → ... → td_io_event → [이 함수].
 */
static struct io_u *fio_vsyncio_event(struct thread_data *td, int event)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] 상태 구조체 포인터. */

	return sd->io_us[event];
	/* [한국어] 직전 커밋에서 io_us[0..queued-1]에 순서대로 저장된 io_u 반환.
	 * commit 후 getevents가 events를 소비할 때까지 io_us 배열 내용은 불변. */
}

/*
 * [한국어]
 * fio_vsyncio_append - 새 io_u가 기존 큐에 "연속 I/O"로 합쳐질 수 있는지 판정.
 *
 * @td: 잡. @io_u: 새로 들어온 요청.
 * @return: 1 = append 가능(같은 파일·같은 방향·연속 오프셋), 0 = 별도 커밋 필요.
 *
 * vsync의 벡터 병합 최적화 핵심 술어. 연속 I/O만 iovec 배열로 묶어 readv/writev
 * 한 번에 제출하면 syscall 오버헤드와 VFS/블록 레이어 왕복이 N→1로 감소한다.
 */
static int fio_vsyncio_append(struct thread_data *td, struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] 상태 — last_offset/last_file/last_ddir 읽음. */

	if (ddir_sync(io_u->ddir))
		return 0;
	/* [한국어] ddir_sync 매크로: SYNC/DATASYNC/SYNC_FILE_RANGE 판정. fsync 같은
	 * 동기화 요청은 의미상 "이 시점까지의 모든 쓰기 영속화"를 요구하므로 절대 벡터
	 * 병합 금지 — 병합하면 ordering이 깨진다. */

	if (io_u->offset == sd->last_offset && io_u->file == sd->last_file &&
	    io_u->ddir == sd->last_ddir)
		return 1;
	/* [한국어] 세 조건 AND: (1) 새 offset == 직전 끝 위치(연속), (2) 같은 파일(fd 공유),
	 * (3) 같은 방향(READ/WRITE 일치). 하나라도 어긋나면 벡터 병합 불가. */

	return 0;
	/* [한국어] 불연속/파일 변경/방향 변경 — 새 batch 시작 필요. */
}

/*
 * [한국어]
 * fio_vsyncio_set_iov - iovecs[idx]/io_us[idx]를 채우고 last_* 캐시 및 누적 카운터 갱신.
 *
 * @sd:    syncio_data.
 * @io_u:  새로 누적할 I/O 유닛.
 * @idx:   삽입 위치(0..queued). 보통 set_iov 호출 시 sd->queued 또는 0.
 *
 * append 가능한 I/O가 들어올 때마다 호출되어 iovec 배열과 병합 판정 캐시를 동시에
 * 업데이트한다. queued 카운터도 ++되어 다음 호출 시 같은 함수가 다음 슬롯에 쓰인다.
 */
static void fio_vsyncio_set_iov(struct syncio_data *sd, struct io_u *io_u,
				int idx)
{
	sd->io_us[idx] = io_u;
	/* [한국어] 이벤트 보고용 역링크 — event(idx) 콜백이 이 배열에서 io_u를 꺼낸다. */
	sd->iovecs[idx].iov_base = io_u->xfer_buf;
	/* [한국어] 사용자 버퍼 포인터 공유(zero-copy) — readv/writev가 이 주소로 직접 전송. */
	sd->iovecs[idx].iov_len = io_u->xfer_buflen;
	/* [한국어] 이 iovec 원소가 전송할 바이트 수. */
	sd->last_offset = io_u->offset + io_u->xfer_buflen;
	/* [한국어] 다음 io_u의 append 판정용 — 이번 I/O의 끝 오프셋을 기록. */
	sd->last_file = io_u->file;
	/* [한국어] 병합 판정용 파일 포인터. */
	sd->last_ddir = io_u->ddir;
	/* [한국어] 병합 판정용 I/O 방향. */
	sd->queued_bytes += io_u->xfer_buflen;
	/* [한국어] 전체 바이트 합계 누적 — fio_vsyncio_end()의 전체 전송 판정에 사용. */
	sd->queued++;
	/* [한국어] 큐 길이 +1 — commit 시 readv/writev의 iovcnt 인자. */
}

/*
 * [한국어]
 * fio_vsyncio_queue - "vsync" 엔진의 queue 콜백: 연속 I/O는 누적, 불연속이면
 *                     FIO_Q_BUSY로 commit을 유도.
 *
 * @td: 잡. @io_u: I/O 요청.
 * @return:
 *   · FIO_Q_QUEUED    — iovec 배열에 누적 성공. fio 코어는 이후 commit 호출.
 *   · FIO_Q_BUSY      — 큐가 포화되었거나 불연속이라 이번 io_u를 받을 수 없음.
 *                       fio 코어가 td_io_commit → 큐 비움 → 이 io_u 재시도.
 *   · FIO_Q_COMPLETED — sync ddir(fsync 등)이 빈 큐에 들어온 경우 즉시 처리하고 완료 반환.
 *
 * vsync만 이 FIO_Q_QUEUED/FIO_Q_BUSY 경로를 쓰며(비동기 엔진 계약 모방), 실제 커널
 * 호출은 모두 동기적으로 commit 시점에 일어난다. 이게 다른 sync 엔진과 vsync의
 * 결정적 차이점이다.
 *
 * 분기 흐름:
 *  1) append 불가(불연속/다른 파일/다른 방향/sync ddir) & 큐에 아무것도 없음:
 *     · sync ddir(fsync 등)이면 do_io_u_sync로 즉시 처리하고 fio_io_end로 완료 반환.
 *     · 그 외 새 io_u면 방어적으로 queued/queued_bytes를 0으로 리셋하고 첫 엔트리 삽입.
 *  2) append 불가 & 이미 큐에 뭔가 있음: FIO_Q_BUSY 반환 → 코어가 commit 후 재시도.
 *  3) append 가능 & 큐가 iodepth 꽉 참: FIO_Q_BUSY → commit 유도.
 *  4) append 가능 & 여유 있음: iovec에 추가 후 FIO_Q_QUEUED.
 *
 * 호출 체인: td_io_queue → [이 함수] (→ fio 코어가 필요 시 td_io_commit → fio_vsyncio_commit).
 */
static enum fio_q_status fio_vsyncio_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] vsync 상태 구조체. */

	fio_ro_check(td, io_u);
	/* [한국어] RO 잡 write 방어. */

	if (!fio_vsyncio_append(td, io_u)) {
		dprint(FD_IO, "vsyncio_queue: no append (%d)\n", sd->queued);
		/* [한국어] append 불가 — 병합할 수 없음. 이후 분기로 처리. */
		/*
		 * If we can't append and have stuff queued, tell fio to
		 * commit those first and then retry this io
		 */
		if (sd->queued)
			return FIO_Q_BUSY;
		/* [한국어] 큐에 다른 연속 batch가 대기 중 → 먼저 commit해서 비워야 함.
		 * fio 코어가 FIO_Q_BUSY를 보면 td_io_commit을 호출해 현재 batch를 flush하고,
		 * 다음 do_io 루프에서 이 io_u를 다시 제출한다. */
		if (ddir_sync(io_u->ddir)) {
			int ret = do_io_u_sync(td, io_u);
			/* [한국어] 빈 큐에 fsync 요청이 들어온 경우 — vsync의 병합과 무관하므로
			 * 즉시 do_io_u_sync로 처리하고 fio_io_end()로 완료. */

			return fio_io_end(td, io_u, ret);
		}

		sd->queued = 0;
		/* [한국어] 방어적 재설정 — 이론상 0이어야 하지만 안전장치. */
		sd->queued_bytes = 0;
		/* [한국어] 바이트 누적 카운터도 초기화. */
		fio_vsyncio_set_iov(sd, io_u, 0);
		/* [한국어] 새 batch 시작 — iovecs[0]부터 채움. last_* 캐시도 이 io_u로 갱신. */
	} else {
		if (sd->queued == td->o.iodepth) {
			dprint(FD_IO, "vsyncio_queue: max depth %d\n", sd->queued);
			return FIO_Q_BUSY;
			/* [한국어] 배열 포화 — iodepth 도달. commit을 통해 비운 뒤 재시도해야 함.
			 * iodepth는 잡 옵션(기본 1, 비동기처럼 벡터 병합을 원하면 설정). */
		}

		dprint(FD_IO, "vsyncio_queue: append\n");
		fio_vsyncio_set_iov(sd, io_u, sd->queued);
		/* [한국어] 여유 있음 — 현재 queued 위치에 append. set_iov가 queued도 ++. */
	}

	dprint(FD_IO, "vsyncio_queue: depth now %d\n", sd->queued);
	/* [한국어] 디버그: 현재 큐 깊이 출력. */
	return FIO_Q_QUEUED;
	/* [한국어] 누적 성공 — 아직 실제 I/O는 발행되지 않음. commit 시점에 실행됨. */
}

/*
 * Check that we transferred all bytes, or saw an error, etc
 */
/*
 * [한국어]
 * fio_vsyncio_end - readv/writev 반환값 bytes를 각 io_u에 개별 분배하는 완료 환산.
 *
 * @td: 잡 컨텍스트.
 * @bytes: readv/writev 시스템 호출의 반환값. 전송된 총 바이트 수(>=0) 또는 -1(errno).
 * @return: 0 = 성공(완전 전송/부분 전송 포함), 음수(-err) = 치명적 오류.
 *
 * fio_io_end()와 달리 "여러 io_u에 바이트를 분배"해야 하므로 별도 헬퍼가 필요하다.
 * 처리 단계:
 *   1) bytes == queued_bytes(전부 전송된 이상적인 경우) → 조기 반환 0.
 *   2) 부분 전송(0 <= bytes < queued_bytes): iovec 순서대로 각 io_u에 "이 io_u 몫" 만큼
 *      할당하고 남은 바이트를 차감. 한 io_u가 모두 채워지지 못하면 그 뒤 io_u들은
 *      this_io=0이 되어 resid=xfer_buflen(전체 실패)으로 기록됨.
 *   3) bytes == -1(오류): errno를 모든 io_u에 전파하고 td_verror로 잡 전역 실패 승격.
 *
 * 중요: err = errno를 loop 전에 한 번 캡처하는 것은, 루프 내부에서 다른 syscall이나
 * log 출력이 errno를 덮을 수 있기 때문이다.
 */
static int fio_vsyncio_end(struct thread_data *td, ssize_t bytes)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] vsync 상태. io_us 배열과 queued 카운터를 순회. */
	struct io_u *io_u;
	/* [한국어] 루프 내 임시 포인터. */
	unsigned int i;
	/* [한국어] 루프 인덱스. */
	int err;
	/* [한국어] errno 선캡처용. */

	/*
	 * transferred everything, perfect
	 */
	if (bytes == sd->queued_bytes)
		return 0;
	/* [한국어] 모든 iovec 원소의 바이트가 완전 전송됨 — 각 io_u는 기본값(resid=0)
	 * 상태로 남고 코어가 성공으로 집계. 조기 반환. */

	err = errno;
	/* [한국어] 반복 중 다른 syscall이 errno를 덮을 수 있어 선캡처. bytes=-1 경로에서
	 * 모든 io_u에 복사할 원본 오류값이다. 정상 부분 전송이어도 관성적으로 캡처. */
	for (i = 0; i < sd->queued; i++) {
		io_u = sd->io_us[i];
		/* [한국어] 현재 분배 중인 io_u. */

		if (bytes == -1) {
			io_u->error = err;
			/* [한국어] 전체 오류 경로: readv/writev가 -1 반환 → 전체 batch 실패.
			 * 모든 io_u에 동일 errno를 전파하여 코어가 일관된 실패 처리를 하도록 함. */
		} else {
			unsigned int this_io;
			/* [한국어] 이번 io_u가 실제 전송받은 바이트 수. */

			this_io = bytes;
			/* [한국어] 남은 전체 바이트를 후보값으로. */
			if (this_io > io_u->xfer_buflen)
				this_io = io_u->xfer_buflen;
			/* [한국어] 이 io_u의 요청 크기를 상한으로 cap. 초과분은 다음 io_u로 넘어감. */

			io_u->resid = io_u->xfer_buflen - this_io;
			/* [한국어] 잔여 바이트 = 요청 - 실제 수령. 완전 전송이면 0, 전혀 못 받았으면 xfer_buflen. */
			io_u->error = 0;
			/* [한국어] short I/O는 오류 아님. */
			bytes -= this_io;
			/* [한국어] 다음 io_u에 분배할 남은 바이트. 0에 도달하면 이후 io_u들은 this_io=0. */
		}
	}

	if (bytes == -1) {
		td_verror(td, err, "xfer vsync");
		/* [한국어] 잡 전역 오류 승격 — 치명적 실패는 잡을 중단시킨다. */
		return -err;
		/* [한국어] 음수 반환 — commit 호출자가 이를 에러로 식별. */
	}

	return 0;
	/* [한국어] 부분 전송은 오류 아님 — 0 반환. */
}

/*
 * [한국어]
 * fio_vsyncio_commit - 누적된 iovecs를 lseek 후 readv(2)/writev(2)로 일괄 제출.
 *
 * @td: 잡 컨텍스트.
 * @return: 0 = 성공(또는 빈 큐 skip), -err = lseek 실패.
 *
 * vsync 엔진의 "실제 I/O가 발생하는" 유일한 지점. queue()에서 누적된 iovecs 배열을
 * 한 번의 시스템 호출로 커널에 전달한다. 파일 포인터를 batch의 첫 io_u 오프셋으로
 * 이동(lseek) 후, last_ddir에 따라 readv 또는 writev 디스패치.
 *
 * 동기 호출이므로 반환 시점에 모든 io_u가 완료된 상태이다. events=queued로 이동시키고
 * queued=0으로 리셋하여 다음 라운드를 준비한다(다음 getevents가 events를 소비).
 *
 * 호출 체인: backend.c do_io → td_io_commit → [이 함수] → lseek + readv/writev →
 *   fio_vsyncio_end() → (부분 전송 시) 각 io_u에 resid 분배.
 */
static int fio_vsyncio_commit(struct thread_data *td)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] vsync 상태. */
	struct fio_file *f;
	/* [한국어] 이 batch의 대상 파일(모든 io_u가 같은 파일을 가리킴 — append 조건). */
	ssize_t ret;
	/* [한국어] readv/writev 반환값 — ssize_t는 -1 표현 가능. */

	if (!sd->queued)
		return 0;
	/* [한국어] 커밋할 것 없음(queue가 FIO_Q_BUSY를 자주 반환한 뒤 빈 상태로 commit되는 경우) — 조기 반환. */

	io_u_mark_submit(td, sd->queued);
	/* [한국어] 통계: 이번 batch가 동시 발행한 io 수를 "I/O depth" 히스토그램에 기록.
	 * fio --debug=io 로 볼 수 있는 submit/complete 분포에 기여. */
	f = sd->last_file;
	/* [한국어] batch 내 모든 io_u가 같은 파일이므로 last_file을 대표로 사용. */

	if (lseek(f->fd, sd->io_us[0]->offset, SEEK_SET) == -1) {
		int err = -errno;
		/* [한국어] errno 즉시 캡처 — td_verror가 덮을 수 있음. */

		td_verror(td, errno, "lseek");
		/* [한국어] 잡 전역 오류 승격. */
		return err;
		/* [한국어] 음수 에러 반환 — 코어가 commit 실패로 인식. */
	}
	/* [한국어] readv/writev는 파일 포인터를 시작점으로 쓴다(preadv/pwritev와 달리
	 * 오프셋 인자 없음). 따라서 batch 시작 오프셋으로 lseek하여 벡터 전체의 기준점 설정. */

	if (sd->last_ddir == DDIR_READ)
		ret = readv(f->fd, sd->iovecs, sd->queued);
		/* [한국어] readv(2) — iovcnt=sd->queued 개의 iovec 원소를 순서대로 파일 포인터 위치에서 읽음.
		 * 성공 시 총 읽은 바이트 수, EOF 시 0, 실패 시 -1. 각 iovec에 순차 채움. 파일 포인터
		 * 는 전송 바이트만큼 전진. */
	else
		ret = writev(f->fd, sd->iovecs, sd->queued);
		/* [한국어] writev(2) — iovcnt 개의 iovec를 순서대로 파일 포인터에 씀.
		 * atomic 보장: POSIX는 writev를 원자적으로 명세(다른 writev와 섞이지 않음). */

	dprint(FD_IO, "vsyncio_commit: %d\n", (int) ret);
	/* [한국어] 디버그 로그 — 실제 전송된 총 바이트 수 출력. */
	sd->events = sd->queued;
	/* [한국어] 커밋된 개수를 getevents가 보고할 이벤트 수로 저장. io_us 배열은 event(idx) 응답용으로 유지. */
	sd->queued = 0;
	/* [한국어] 큐 비움 — 다음 batch 시작을 위한 리셋. io_us/iovecs 내용은 events 소비 전까지 참조됨. */
	return fio_vsyncio_end(td, ret);
	/* [한국어] 반환값을 각 io_u에 분배하여 resid/error 설정. */
}

/*
 * [한국어]
 * fio_vsyncio_init - vsync/pvsync/pvsync2 엔진 공통 init 콜백.
 *
 * @td: 잡 컨텍스트.
 * @return: 0 = 성공.
 *
 * syncio_data 구조체를 calloc으로 할당, iovecs/io_us 배열을 td->o.iodepth 길이로
 * 확보, 난수 상태를 init_rand로 초기화. td->io_ops_data에 부착하여 모든 queue/
 * commit/event/getevents 콜백이 공유한다.
 *
 * pvsync는 iovecs[0]만, pvsync2는 iovecs[0] + rand_state를 쓰지만, 공용 init이라
 * 모두 iodepth 길이만큼 할당. vsync만 전체를 활용.
 *
 * 호출 체인: backend.c td_io_init → ioengine_{vrw,pvrw,pvrw2}.init(=이 함수).
 */
static int fio_vsyncio_init(struct thread_data *td)
{
	struct syncio_data *sd;
	/* [한국어] 잡별 상태 포인터. */

	sd = calloc(1, sizeof(*sd));
	/* [한국어] 0으로 초기화된 메모리 할당 — 모든 카운터/포인터가 0/NULL로 시작.
	 * 주의: fio 코드 관행상 calloc 실패(OOM) 시 NULL 반환은 체크하지 않음(후속 dereference로 즉시 세그폴트). */
	sd->last_offset = -1ULL;
	/* [한국어] 무효 센티널 값 — 첫 io_u의 append 판정에서 반드시 불일치하도록. */
	sd->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));
	/* [한국어] iovec 배열 할당 — iodepth 크기. 초기값은 정의되지 않음(set_iov에서 덮어씀). */
	sd->io_us = malloc(td->o.iodepth * sizeof(struct io_u *));
	/* [한국어] io_u 포인터 배열 할당. */
	init_rand(&sd->rand_state, 0);
	/* [한국어] pvsync2의 hipri 분포용 난수 seed 초기화. 두 번째 인자 0 = OS 난수 미사용(결정적 seed). */

	td->io_ops_data = sd;
	/* [한국어] fio 코어가 queue/commit 등 콜백에서 꺼내 쓸 수 있도록 td에 부착. */
	return 0;
	/* [한국어] 성공 보고 — 음수 반환하면 잡이 초기화 실패로 중단됨. */
}

/*
 * [한국어]
 * fio_vsyncio_cleanup - init에서 할당한 자원을 해제하는 cleanup 콜백.
 *
 * @td: 잡 컨텍스트.
 *
 * 잡 종료 시 1회 호출되어 syncio_data와 그 내부 배열을 free. free(NULL)은 안전하므로
 * init이 부분 실패한 경우에도 안전하게 동작한다.
 *
 * 호출 체인: backend.c 잡 정리 → td_io_cleanup → ioengine.cleanup(=이 함수).
 */
static void fio_vsyncio_cleanup(struct thread_data *td)
{
	struct syncio_data *sd = td->io_ops_data;
	/* [한국어] init이 부착한 상태 포인터. */

	if (sd) {
		/* [한국어] init 실패 경로 방어 — sd가 NULL이면 아무것도 하지 않음. */
		free(sd->iovecs);
		/* [한국어] iovec 배열 해제. */
		free(sd->io_us);
		/* [한국어] io_u 포인터 배열 해제. */
		free(sd);
		/* [한국어] 구조체 본체 해제. */
	}
}

/*
 * [한국어] ioengine_rw — "sync" 엔진 vtable. read(2)/write(2) + lseek(2) 기반.
 * fio의 **기본 엔진**(ioengine 옵션 미지정 시 폴백). prep에서 LAST_POS 최적화로 lseek
 * 절감, queue에서 즉시 완료 보고. commit/event/getevents 불필요(FIO_SYNCIO 덕분).
 */
static struct ioengine_ops ioengine_rw = {
	.name		= "sync",
	/* [한국어] 엔진 식별자 — 잡 파일 "ioengine=sync" 또는 미지정 시 매칭.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c의 load_ioengine() strcmp. 등록 후 불변. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전 상수(fio.h 정의). register_ioengine이 코어와
	 * 매칭 검증 — 버전 불일치 시 엔진 로드 거부(에러 출력). */

	.prep		= fio_syncio_prep,
	/* [한국어] prep 콜백 — 매 io_u 제출 전 LAST_POS 체크 후 필요 시 lseek(2) 호출.
	 * 5개 엔진 중 유일하게 prep이 필요한 엔진(read/write 파일 포인터 의존). */

	.queue		= fio_syncio_queue,
	/* [한국어] queue 콜백 — read/write 동기 실행. FIO_Q_COMPLETED 반환(FIO_SYNCIO 계약). */

	.open_file	= generic_open_file,
	/* [한국어] fio 코어가 제공하는 기본 open_file — O_RDONLY/O_WRONLY/O_RDWR/O_DIRECT/
	 * O_SYNC 등을 td->o 옵션에서 해석해 open(2) 호출. 특수 로직이 없어 재사용. */

	.close_file	= generic_close_file,
	/* [한국어] fio 코어 기본 close_file — close(2) 호출. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] fio 코어 기본 get_file_size — stat/fstat으로 파일 크기 획득. 블록
	 * 디바이스는 BLKGETSIZE64 ioctl. */

	.flags		= FIO_SYNCIO | FIO_SYNCFS,
	/* [한국어] 엔진 특성 플래그 비트마스크:
	 *   · FIO_SYNCIO (0x01): "동기 I/O 엔진" — queue()가 즉시 FIO_Q_COMPLETED를
	 *     반환함을 코어에 통지. 코어는 commit/getevents/event 호출을 건너뛰고
	 *     iodepth=1처럼 동작시킴(큐잉 없음). 매 io_u마다 io_u_submit_time 자동 기록.
	 *   · FIO_SYNCFS (0x8000): syncfs(2) 시스템 호출 지원 — fsync_on_close 등의
	 *     파일시스템 sync 기능이 필요한 잡 옵션과 호환. */
};

/*
 * [한국어] ioengine_prw — "psync" 엔진 vtable. pread(2)/pwrite(2) 기반.
 * prep 불필요(파일 포인터 독립). 멀티스레드 fd 공유 안전.
 */
static struct ioengine_ops ioengine_prw = {
	.name		= "psync",
	/* [한국어] "ioengine=psync" 매칭. 설정자: 이 초기화. 읽는 자: load_ioengine. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전. */

	.queue		= fio_psyncio_queue,
	/* [한국어] queue 콜백 — pread/pwrite 동기 실행. prep 없음(오프셋 인자로 직접 전달). */

	.open_file	= generic_open_file,
	/* [한국어] 기본 open. */

	.close_file	= generic_close_file,
	/* [한국어] 기본 close. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 기본 크기 조회. */

	.flags		= FIO_SYNCIO | FIO_SYNCFS,
	/* [한국어] sync와 동일한 플래그 조합 — 동기 엔진 + syncfs 지원.
	 * sync와 차이: prep 없음(콜백 테이블에 .prep 필드 미지정) → 코어가 건너뜀. */
};

/*
 * [한국어] ioengine_vrw — "vsync" 엔진 vtable. readv(2)/writev(2) 벡터 일괄 제출.
 * 5개 엔진 중 유일하게 queue에서 FIO_Q_QUEUED를 반환하고 commit에서 실제 I/O 발행.
 * init/cleanup/commit/event/getevents가 모두 필요(syncio_data 생명주기 관리).
 */
static struct ioengine_ops ioengine_vrw = {
	.name		= "vsync",
	/* [한국어] "ioengine=vsync" 매칭. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전. */

	.init		= fio_vsyncio_init,
	/* [한국어] init 콜백 — syncio_data calloc + iovecs/io_us 배열 malloc + 난수 초기화. */

	.cleanup	= fio_vsyncio_cleanup,
	/* [한국어] cleanup 콜백 — init에서 할당한 자원 free. */

	.queue		= fio_vsyncio_queue,
	/* [한국어] queue 콜백 — 연속 I/O는 iovec에 append(FIO_Q_QUEUED), 불연속/포화 시
	 * FIO_Q_BUSY로 코어에 commit 유도. sync ddir은 즉시 do_io_u_sync 후 COMPLETED. */

	.commit		= fio_vsyncio_commit,
	/* [한국어] commit 콜백 — lseek + readv/writev 일괄 호출, events=queued 이동. */

	.event		= fio_vsyncio_event,
	/* [한국어] event 콜백 — 인덱스로 io_us 배열에서 io_u 반환(완료 보고용). */

	.getevents	= fio_vsyncio_getevents,
	/* [한국어] getevents 콜백 — sd->events 카운터 소비 후 반환. */

	.open_file	= generic_open_file,
	/* [한국어] 기본 open. */

	.close_file	= generic_close_file,
	/* [한국어] 기본 close. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 기본 크기 조회. */

	.flags		= FIO_SYNCIO | FIO_SYNCFS,
	/* [한국어] FIO_SYNCIO 플래그가 있지만 실제로는 queue가 FIO_Q_QUEUED를 반환할 수 있어
	 * 비동기 성격이 섞여 있다. 코어는 SYNCIO를 보고도 FIO_Q_QUEUED를 받으면 정상적으로
	 * commit/getevents 경로를 태운다(SYNCIO는 "큐잉을 생략할 수 있음"이지 강제 아님).
	 * FIO_SYNCFS: syncfs 호환. */
};

/*
 * [한국어] ioengine_pvrw — "pvsync" 엔진 vtable. preadv(2)/pwritev(2) 기반.
 * vsync와 init/cleanup을 공유하지만 queue는 즉시 실행(iovec 1개만 사용).
 * CONFIG_PWRITEV 빌드에서만 컴파일(preadv/pwritev 가용 플랫폼).
 */
#ifdef CONFIG_PWRITEV
static struct ioengine_ops ioengine_pvrw = {
	.name		= "pvsync",
	/* [한국어] "ioengine=pvsync" 매칭. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전. */

	.init		= fio_vsyncio_init,
	/* [한국어] 공용 init(syncio_data 할당) — iovecs[0]만 사용하지만 iodepth 전체 할당. */

	.cleanup	= fio_vsyncio_cleanup,
	/* [한국어] 공용 cleanup. */

	.queue		= fio_pvsyncio_queue,
	/* [한국어] queue 콜백 — preadv/pwritev 즉시 동기 실행. FIO_Q_COMPLETED 반환. */

	.open_file	= generic_open_file,
	/* [한국어] 기본 open. */

	.close_file	= generic_close_file,
	/* [한국어] 기본 close. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 기본 크기 조회. */

	.flags		= FIO_SYNCIO | FIO_SYNCFS,
	/* [한국어] 동기 엔진 + syncfs 호환. commit/getevents/event 사용 안 함(SYNCIO 계약). */
};
#endif

/*
 * [한국어] ioengine_pvrw2 — "pvsync2" 엔진 vtable. preadv2(2)/pwritev2(2) + RWF_*.
 * pvsync의 상위호환 + RWF_HIPRI/DONTCACHE/NOWAIT/ATOMIC 플래그 제어.
 * FIO_HAVE_PWRITEV2 빌드(Linux 4.6+ + glibc 2.26+)에서만 컴파일.
 * options 테이블과 option_struct_size로 엔진 전용 옵션(psyncv2_options) 노출.
 */
#ifdef FIO_HAVE_PWRITEV2
static struct ioengine_ops ioengine_pvrw2 = {
	.name		= "pvsync2",
	/* [한국어] "ioengine=pvsync2" 매칭. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전. */

	.init		= fio_vsyncio_init,
	/* [한국어] 공용 init — rand_state도 함께 초기화되어 hipri 확률 판정에 사용. */

	.cleanup	= fio_vsyncio_cleanup,
	/* [한국어] 공용 cleanup. */

	.queue		= fio_pvsyncio2_queue,
	/* [한국어] queue 콜백 — RWF_* 플래그 조합 후 preadv2/pwritev2 즉시 실행. */

	.open_file	= generic_open_file,
	/* [한국어] 기본 open. */

	.close_file	= generic_close_file,
	/* [한국어] 기본 close. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 기본 크기 조회. */

	.flags		= FIO_SYNCIO | FIO_ATOMICWRITES | FIO_SYNCFS,
	/* [한국어] 플래그 조합:
	 *   · FIO_SYNCIO: 동기 엔진 — queue가 즉시 완료.
	 *   · FIO_ATOMICWRITES (0x40000): --atomic 옵션과 연동 — RWF_ATOMIC 경로를 이용한
	 *     torn-write 방지 쓰기 지원을 코어에 통지. 이 플래그가 없으면 oatomic=1이어도
	 *     fio 코어가 경고 후 비활성화.
	 *   · FIO_SYNCFS: syncfs 호환. */

	.options	= options,
	/* [한국어] 엔진 전용 옵션 테이블 포인터 — 위 static fio_option options[] 배열.
	 * fio 파서가 이 배열을 돌며 잡 파일의 옵션 키와 매칭. */

	.option_struct_size	= sizeof(struct psyncv2_options),
	/* [한국어] td->eo에 할당할 옵션 구조체 크기 — 파서가 이 크기만큼 calloc 후
	 * .off1 오프셋에 값 기입. */
};
#endif

/*
 * [한국어]
 * fio_syncio_register - 프로그램 시작 시 **5개 엔진을 한꺼번에** 코어 엔진 리스트에 등록.
 *
 * 특이 사항: 다른 엔진(null/libaio/io_uring 등)은 파일당 엔진 1개를 등록하지만 이 파일은
 * 한 constructor에서 5개 ioengine_ops vtable을 모두 등록한다. 이유는 (1) 다섯 엔진이
 * 동일한 syncio_data 구조체·fio_io_end 헬퍼·LAST_POS 매크로 등 공용 인프라를 사용하고
 * (2) ddir 분기 구조가 동형이어서 한 번역 단위(translation unit)로 관리하는 것이 빌드
 * 의존성과 유지보수 측면에서 유리하기 때문이다. 각 vtable은 전역 engine_list에 독립
 * 노드로 링크되므로, fio 코어의 load_ioengine()이 사용자 옵션 "ioengine=sync|psync|
 * vsync|pvsync|pvsync2" 중 하나를 strcmp 매칭해 별도로 꺼내 쓸 수 있다.
 *
 * fio_init 속성: GCC/Clang의 __attribute__((constructor))로 치환되는 매크로(fio.h).
 * main() 진입 전 libc 동적 로더가 자동 호출 → register_ioengine이 flist_add_tail로
 * 전역 engine_list에 각 vtable 노드를 추가. 프로세스당 단 1회 실행(메인 스레드).
 *
 * 조건부 등록:
 *   · pvsync: CONFIG_PWRITEV (configure가 preadv/pwritev 가용성 확인 시)
 *   · pvsync2: FIO_HAVE_PWRITEV2 (Linux 4.6+ 커널 헤더 + glibc 2.26+)
 * 미지원 빌드에선 해당 엔진이 컴파일/등록되지 않아 load_ioengine("pvsync2") 호출 시
 * 엔진을 찾지 못해 에러가 출력된다.
 */
static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine_rw);
	/* [한국어] "sync" 등록 — flist_add_tail로 전역 engine_list에 링크. */
	register_ioengine(&ioengine_prw);
	/* [한국어] "psync" 등록. */
	register_ioengine(&ioengine_vrw);
	/* [한국어] "vsync" 등록. */
#ifdef CONFIG_PWRITEV
	register_ioengine(&ioengine_pvrw);
	/* [한국어] "pvsync" 등록 — preadv/pwritev 빌드 가능한 플랫폼에서만. */
#endif
#ifdef FIO_HAVE_PWRITEV2
	register_ioengine(&ioengine_pvrw2);
	/* [한국어] "pvsync2" 등록 — Linux 4.6+ preadv2/pwritev2 가용 플랫폼. */
#endif
}

/*
 * [한국어]
 * fio_syncio_unregister - 프로세스 종료 시 5개 엔진을 모두 등록 해제(destructor).
 *
 * fio_exit 속성: __attribute__((destructor))로 치환 — main() 복귀 후 libc가 자동 호출.
 * 정적 바이너리에서는 프로세스 종료와 함께 메모리가 회수되므로 엄밀히 불필요하나,
 * 외부 엔진(.so) dlclose 경로나 메모리 누수 검사 도구에 대한 위생을 위해 대칭적으로
 * 작성한다. unregister_ioengine은 flist_del로 노드를 engine_list에서 제거.
 */
static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine_rw);
	/* [한국어] sync 해제. */
	unregister_ioengine(&ioengine_prw);
	/* [한국어] psync 해제. */
	unregister_ioengine(&ioengine_vrw);
	/* [한국어] vsync 해제. */
#ifdef CONFIG_PWRITEV
	unregister_ioengine(&ioengine_pvrw);
	/* [한국어] pvsync 해제(조건부 컴파일). */
#endif
#ifdef FIO_HAVE_PWRITEV2
	unregister_ioengine(&ioengine_pvrw2);
	/* [한국어] pvsync2 해제(조건부 컴파일). */
#endif
}
