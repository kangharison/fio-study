/*
 * [한국어 설명] 동기식 POSIX I/O 엔진 구현 (sync.c) — fio의 기준 엔진
 *
 * === 파일의 역할 ===
 * 이 파일은 fio가 제공하는 가장 기본적인 동기식 I/O 엔진을 구현한다. 표준
 * POSIX 시스템 호출 read(2)/write(2), pread(2)/pwrite(2), readv(2)/writev(2),
 * preadv(2)/pwritev(2), preadv2(2)/pwritev2(2) 위에 하나의 소스 파일로
 * 5가지 서로 다른 ioengine ("sync", "psync", "vsync", "pvsync", "pvsync2")
 * 을 동시에 등록한다. 각 엔진은 동일한 syncio_data 구조체와 fio_io_end()
 * 완료 처리 루틴을 공유하면서 서로 다른 시스템 호출을 통해 단일 버퍼 또는
 * 벡터(scatter/gather) I/O를 수행한다. 모든 I/O는 호출과 동시에 완료되므로
 * fio_q_status의 FIO_Q_COMPLETED/FIO_Q_QUEUED 의미가 비동기 엔진과 다르게
 * 해석된다(vsync 계열은 큐에 쌓은 뒤 commit에서 일괄 전송). 이 엔진은
 * 설치 필수 기능이 없어 거의 모든 플랫폼에서 동작하며, fio의 다른 엔진
 * 성능을 비교할 때의 기준선(baseline) 역할을 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 실행 흐름(`backend.c: thread_main → do_io`)에서 잡 스레드는 매
 * iteration마다 다음의 ioengine 콜백을 호출한다:
 *   get_io_u() → td_io_prep() → td_io_queue() → td_io_commit() →
 *   td_io_getevents() → ioengine.event() → put_io_u().
 * 본 파일이 채우는 콜백은 엔진별로 다르다:
 *   - "sync":     prep=fio_syncio_prep(lseek)  queue=fio_syncio_queue(read/write)
 *   - "psync":    queue=fio_psyncio_queue(pread/pwrite)
 *   - "vsync":    init=fio_vsyncio_init, queue=fio_vsyncio_queue(큐에 누적),
 *                 commit=fio_vsyncio_commit(readv/writev), event/getevents.
 *   - "pvsync":   vsync와 동일한 init/cleanup을 쓰되, queue=fio_pvsyncio_queue
 *                 (preadv/pwritev로 즉시 동기 실행 — 큐잉 없음).
 *   - "pvsync2":  queue=fio_pvsyncio2_queue(preadv2/pwritev2 + RWF_* flags).
 * 실행 컨텍스트: 모든 콜백은 fio 잡 스레드(또는 잡 프로세스)에서 동기적으로
 * 실행된다. 커널이 block/DMA 레벨에서 실제 I/O를 완료할 때까지 시스템 호출
 * 내부에서 대기하며, 따라서 getevents의 최소 min도 commit 시점에 모두 채워진다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존 모듈: 표준 C 라이브러리(read/write/lseek/p*v*2), sys/uio.h의 iovec,
 *              fio 코어(fio.h)의 thread_data/io_u/fio_file/ioengine_ops 정의,
 *              optgroup.h의 FIO_OPT_C_ENGINE 카테고리, lib/rand.h의 frand_state.
 * - 이 파일에 의존하는 모듈: fio 코어 런타임이 엔진 이름으로 조회(register_ioengine
 *   으로 등록된 다섯 vtable). test 바이너리에서 기본 엔진으로 암묵 참조되기도 한다.
 * - 데이터 흐름: io_u->xfer_buf/xfer_buflen/offset → read/pread/readv/preadv/preadv2
 *               또는 write/pwrite/writev/pwritev/pwritev2 → 커널 VFS → 블록 레이어.
 *   반환값은 fio_io_end() 또는 fio_vsyncio_end()에서 io_u->resid/error로 환산.
 * - 공유 자료구조: td->io_ops_data = struct syncio_data* (vsync 계열만),
 *                 td->eo         = struct psyncv2_options* (pvsync2 전용),
 *                 f->engine_pos  = 마지막 I/O 종료 오프셋(LAST_POS 매크로 래핑).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_syncio_prep(): sync 엔진에서 lseek으로 파일 포인터를 io_u->offset으로 이동.
 * - fio_io_end(): 단일 syscall 반환값 → io_u->resid/error 공통 환산.
 * - fio_syncio_queue()/fio_psyncio_queue(): 각각 read·write / pread·pwrite 동기 발행.
 * - fio_pvsyncio_queue()/fio_pvsyncio2_queue(): preadv·pwritev / preadv2·pwritev2 분기.
 * - fio_vsyncio_{queue,commit,end}(): 연속 I/O를 iovec에 병합 → readv/writev 일괄 실행.
 * - fio_vsyncio_{init,cleanup,event,getevents}(): vsync 계열 상태 관리 & 완료 보고.
 * - struct syncio_data: vsync/pvsync 계열의 iovec, 큐잉 상태, 난수 상태.
 * - struct psyncv2_options: pvsync2 전용 옵션(hipri/nowait/uncached).
 * - fio_syncio_register(): 5개 ioengine_ops를 한 번에 등록(빌드 매크로로 선택적).
 */

/*
 * sync/psync engine
 *
 * IO engine that does regular read(2)/write(2) with lseek(2) to transfer
 * data and IO engine that does regular pread(2)/pwrite(2) to transfer data.
 *
 */
/* [한국어] 표준 입출력 선언(실제로는 dprint 매크로 경로 등에서 간접 참조). */
#include <stdio.h>
/* [한국어] calloc/malloc/free — syncio_data 및 내부 배열 할당. */
#include <stdlib.h>
/* [한국어] read/write/lseek/pread/pwrite 등 POSIX I/O syscall 프로토타입. */
#include <unistd.h>
/* [한국어] struct iovec 정의 — readv/writev/preadv/pwritev 벡터 I/O에 필수. */
#include <sys/uio.h>
/* [한국어] errno 전역 — syscall 실패 시 fio_io_end에서 io_u->error에 복사. */
#include <errno.h>

/* [한국어] fio 핵심 타입/매크로 선언: thread_data, io_u, fio_file, ioengine_ops,
 * FIO_Q_* 반환값, td_verror, io_u_log_error, fio_ro_check, do_io_u_sync 등. */
#include "../fio.h"
/* [한국어] 옵션 카테고리/그룹 매크로(FIO_OPT_C_ENGINE, FIO_OPT_G_INVALID). */
#include "../optgroup.h"
/* [한국어] frand_state, init_rand, rand_between — pvsync2 hipri 확률 결정용 난수. */
#include "../lib/rand.h"

/*
 * Sync engine uses engine_data to store last offset
 */
/* [한국어] LAST_POS(f): fio_file의 engine_pos 슬롯을 "파일 포인터 마지막 위치" 저장소로
 * 전용 사용하는 매크로. sync 엔진은 read/write가 파일 포인터를 자동으로 전진시키므로,
 * 다음 io_u의 offset이 LAST_POS와 같으면 lseek을 생략해 syscall 한 번을 아낀다.
 * "-1ULL"을 "초기/무효" 센티널 값으로 사용한다. */
#define LAST_POS(f)	((f)->engine_pos)

/*
 * [한국어] vsync/pvsync/pvsync2 계열 엔진의 내부 상태 — td->io_ops_data에 매달린다.
 * 벡터 I/O(readv/writev)를 위한 iovec 배열과 "같은 파일·방향·연속 오프셋"
 * 판별을 위한 last_* 캐시, 그리고 pvsync2의 확률적 hipri 결정용 난수 상태를 모두 담는다.
 */
struct syncio_data {
	struct iovec *iovecs;
	/* [한국어] scatter/gather I/O용 iovec 배열(길이 = td->o.iodepth).
	 * 설정자: fio_vsyncio_init()가 malloc, fio_vsyncio_set_iov()가 각 슬롯 기입.
	 * 읽는 자: fio_vsyncio_commit()의 readv()/writev() 인자로 전달.
	 * 값 범위: iov_base=io_u->xfer_buf, iov_len=io_u->xfer_buflen.
	 * 동기화: 잡 스레드 전용 — 다른 스레드 접근 없음. */

	struct io_u **io_us;
	/* [한국어] 큐잉된 io_u 포인터 배열 — commit 후 event(idx) 응답에 사용.
	 * 설정자: fio_vsyncio_set_iov()가 큐잉 순서대로 기록.
	 * 읽는 자: fio_vsyncio_event()가 인덱스 역참조, fio_vsyncio_end()가 전송 결과 분배.
	 * 값 범위: iodepth 길이. 유효 범위는 [0, queued).
	 * 동기화: 잡 스레드 전용. */

	unsigned int queued;
	/* [한국어] 현재 iovecs에 누적되어 commit을 기다리는 io_u 개수.
	 * 설정자: fio_vsyncio_set_iov() 증가, fio_vsyncio_commit()에서 0으로 리셋.
	 * 읽는 자: fio_vsyncio_queue() 병합 가능 여부 판단, commit() skip 여부 판정.
	 * 값 범위: 0..iodepth. */

	unsigned int events;
	/* [한국어] getevents가 아직 fio에 보고하지 않은 완료 이벤트 수.
	 * 설정자: fio_vsyncio_commit()에서 queued를 이동, getevents에서 0으로 소비.
	 * 읽는 자: fio_vsyncio_getevents(). 값 범위: 0..iodepth. */

	unsigned long queued_bytes;
	/* [한국어] 누적 중인 iovecs의 전체 바이트 합 — readv/writev 반환값과 비교해 부분 전송 검출.
	 * 설정자: set_iov에서 +=, commit에서 0으로 리셋.
	 * 읽는 자: fio_vsyncio_end()의 bytes==queued_bytes 비교. */

	unsigned long long last_offset;
	/* [한국어] 마지막 누적 io_u의 끝 오프셋(= offset + xfer_buflen).
	 * 설정자/읽는 자: 자신(연속 오프셋 판정 캐시). 초기값 -1ULL(무효). */

	struct fio_file *last_file;
	/* [한국어] 마지막으로 큐에 누적된 io_u가 속한 파일. 다른 파일이면 병합 중단.
	 * 설정자: set_iov(). 읽는 자: fio_vsyncio_append(). */

	enum fio_ddir last_ddir;
	/* [한국어] 마지막 I/O 방향(DDIR_READ/WRITE). 방향이 다르면 벡터 병합 불가.
	 * 설정자: set_iov(). 읽는 자: append(). */

	struct frand_state rand_state;
	/* [한국어] pvsync2의 hipri_percentage 확률 판정용 난수 상태.
	 * 설정자: fio_vsyncio_init()가 init_rand로 초기화.
	 * 읽는 자: fio_pvsyncio2_queue()의 rand_between(1,100).
	 * 동기화: 잡 스레드 전용. */
};

/* [한국어] FIO_HAVE_PWRITEV2: configure가 preadv2/pwritev2(Linux 4.6+) 가용성을 확인한 경우에만 정의.
 * 이 빌드에서만 pvsync2 엔진과 관련 옵션 구조체가 생성된다. */
#ifdef FIO_HAVE_PWRITEV2
/*
 * [한국어] pvsync2 엔진 전용 옵션 구조체 — preadv2/pwritev2의 flags 인자를 제어.
 */
struct psyncv2_options {
	void *pad;
	/* [한국어] fio 옵션 파서가 요구하는 구조체 선두 패딩(실제 미사용). */

	unsigned int hipri;
	/* [한국어] RWF_HIPRI 요청 여부(block-layer polling 경로).
	 * 설정자: 옵션 파서(FIO_OPT_STR_SET). 읽는 자: fio_pvsyncio2_queue().
	 * 값 범위: 0|1. 부분적용은 hipri_percentage로 조절. */

	unsigned int hipri_percentage;
	/* [한국어] hipri=1일 때 실제 RWF_HIPRI를 붙일 확률(%).
	 * 설정자: 옵션 파서(기본 100). 읽는 자: rand_between과 비교.
	 * 값 범위: 0..100. */

	unsigned int uncached;
	/* [한국어] RWF_DONTCACHE — buffered I/O에서 페이지 캐시 우회 힌트.
	 * 설정자: 파서. 읽는 자: queue(). 단 td->o.odirect와 동시 사용 시 무시. */

	unsigned int nowait;
	/* [한국어] RWF_NOWAIT — 블로킹 대신 EAGAIN 반환을 허용.
	 * 설정자: 파서. 읽는 자: queue(). 값 범위: 0|1. */
};

/* [한국어] pvsync2 엔진의 옵션 테이블. .name=NULL 센티널 종료. */
static struct fio_option options[] = {
	{
		.name	= "hipri",
		.lname	= "RWF_HIPRI",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct psyncv2_options, hipri),
		.help	= "Set RWF_HIPRI for pwritev2/preadv2",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "hipri_percentage",
		.lname	= "RWF_HIPRI_PERCENTAGE",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct psyncv2_options, hipri_percentage),
		.minval	= 0,
		.maxval	= 100,
		.def    = "100",
		.help	= "Probabilistically set RWF_HIPRI for pwritev2/preadv2",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "uncached",
		.lname	= "Uncached",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct psyncv2_options, uncached),
		.help	= "Use RWF_DONTCACHE for buffered read/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "nowait",
		.lname	= "RWF_NOWAIT",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct psyncv2_options, nowait),
		.help	= "Set RWF_NOWAIT for pwritev2/preadv2",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= NULL,
	},
};
#endif

/*
 * [한국어]
 * fio_syncio_prep - sync 엔진의 prep 콜백: 필요한 경우 lseek으로 파일 포인터 이동.
 *
 * @td: 잡 컨텍스트(td_verror로 에러 보고할 때 사용).
 * @io_u: 이번에 수행할 I/O 유닛. file/ddir/offset 필드가 유효해야 한다.
 * @return: 0=성공(또는 불필요), 1=lseek 실패.
 *
 * read(2)/write(2)는 파일 포인터에 의존하므로 이전 io_u의 끝 오프셋이 새 요청
 * offset과 같으면 seek을 생략할 수 있다(순차 I/O 최적화). pread/pwrite 계열은
 * 오프셋을 직접 주므로 이 prep이 필요 없다. sync 모델은 멀티스레드가 같은 fd를
 * 공유하면 레이스가 발생하므로, fio는 sync 엔진을 쓸 때 보통 잡당 fd가 분리된다.
 *
 * 호출 체인: td_io_prep() [ioengines.c] → [이 함수] → lseek(2)
 */
static int fio_syncio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;	/* [한국어] 대상 파일 핸들. */

	if (!ddir_rw(io_u->ddir))
		return 0;
	/* [한국어] TRIM/SYNC 등 비-R/W 요청엔 lseek 불필요. */

	if (LAST_POS(f) != -1ULL && LAST_POS(f) == io_u->offset)
		return 0;
	/* [한국어] 직전 I/O 끝과 같은 오프셋 → 파일 포인터가 이미 거기 있음. */

	if (lseek(f->fd, io_u->offset, SEEK_SET) == -1) {
		td_verror(td, errno, "lseek");
		return 1;
	}
	/* [한국어] 커널에 파일 포인터 이동을 요청. 실패 시 에러 전파. */

	return 0;
}

/*
 * [한국어]
 * fio_io_end - 단일 syscall 반환값을 fio의 완료 상태로 환산하는 공통 헬퍼.
 *
 * @td: 잡. @io_u: 완료 처리할 I/O 유닛. @ret: syscall 반환값(바이트 수 또는 -1).
 * @return: 항상 FIO_Q_COMPLETED — 동기 엔진이므로 즉시 완료 상태로 보고.
 *
 * 1) 정상 R/W인 경우 LAST_POS를 전진시켜 다음 prep에서 lseek 생략 가능 여부 판단.
 * 2) 전송 바이트가 xfer_buflen 미만이면 short read/write → resid에 잔여 기록.
 * 3) ret=-1이면 errno를 io_u->error에 복사하고 td_verror로 상위 레이어에 전파.
 *
 * 호출 체인: 모든 sync*_queue() → [이 함수] → td_verror(에러 시)
 */
static int fio_io_end(struct thread_data *td, struct io_u *io_u, int ret)
{
	if (io_u->file && ret >= 0 && ddir_rw(io_u->ddir))
		LAST_POS(io_u->file) = io_u->offset + ret;
	/* [한국어] 성공한 R/W의 경우 마지막 위치를 전진시켜 캐시. */

	if (ret != (int) io_u->xfer_buflen) {
		/* [한국어] 전체 바이트를 전송하지 못한 경우. */
		if (ret >= 0) {
			io_u->resid = io_u->xfer_buflen - ret;	/* [한국어] short I/O 잔여. */
			io_u->error = 0;
			return FIO_Q_COMPLETED;
		} else
			io_u->error = errno;			/* [한국어] 오류 경로: errno 복사. */
	}

	if (io_u->error) {
		io_u_log_error(td, io_u);			/* [한국어] 디버그 로그 출력. */
		td_verror(td, io_u->error, "xfer");		/* [한국어] 잡 전역 오류로 승격. */
	}

	return FIO_Q_COMPLETED;
}

/*
 * [한국어]
 * fio_pvsyncio_queue - "pvsync" 엔진의 queue 콜백: preadv/pwritev로 즉시 실행.
 *
 * @td: 잡. @io_u: 요청. @return: FIO_Q_COMPLETED (동기 완료).
 *
 * vsync와 달리 큐잉 없이 iovec 하나짜리 벡터 I/O를 즉시 발행한다. 즉
 * preadv/pwritev의 scatter/gather 기능을 쓰지 않지만, 오프셋 인자가 있어 lseek
 * 없이 위치 지정이 가능하다는 점이 sync 대비 장점이다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → preadv(2)/pwritev(2) | do_io_u_trim/sync
 */
#ifdef CONFIG_PWRITEV
static enum fio_q_status fio_pvsyncio_queue(struct thread_data *td,
					    struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;
	struct iovec *iov = &sd->iovecs[0];	/* [한국어] 단일 벡터 사용 — 인덱스 0만 씀. */
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);			/* [한국어] RO 잡에서 write 차단. */

	iov->iov_base = io_u->xfer_buf;		/* [한국어] 사용자 버퍼 포인터 공유. */
	iov->iov_len = io_u->xfer_buflen;

	if (io_u->ddir == DDIR_READ)
		ret = preadv(f->fd, iov, 1, io_u->offset);
	else if (io_u->ddir == DDIR_WRITE)
		ret = pwritev(f->fd, iov, 1, io_u->offset);
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);		/* [한국어] 파일시스템/장치별 discard 경로. */
		return FIO_Q_COMPLETED;
	} else
		ret = do_io_u_sync(td, io_u);	/* [한국어] fsync/fdatasync 계열. */

	return fio_io_end(td, io_u, ret);
}
#endif

/*
 * [한국어]
 * fio_pvsyncio2_queue - "pvsync2" 엔진 queue: preadv2/pwritev2 + RWF_* 플래그.
 *
 * RWF_HIPRI(블록 폴링), RWF_DONTCACHE(페이지 캐시 우회 힌트), RWF_NOWAIT,
 * RWF_ATOMIC(td->o.oatomic 시, write 한정) 등 확장 플래그를 조합한다.
 * hipri는 hipri_percentage 확률로만 적용되어, 폴링/IRQ 혼합 워크로드 시뮬레이션이 가능하다.
 *
 * 호출 체인: td_io_queue → [이 함수] → preadv2(2)/pwritev2(2)
 */
#ifdef FIO_HAVE_PWRITEV2
static enum fio_q_status fio_pvsyncio2_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;
	struct psyncv2_options *o = td->eo;	/* [한국어] 엔진 전용 옵션. */
	struct iovec *iov = &sd->iovecs[0];
	struct fio_file *f = io_u->file;
	int ret, flags = 0;			/* [한국어] RWF_* 플래그 누적. */

	fio_ro_check(td, io_u);

	if (o->hipri &&
	    (rand_between(&sd->rand_state, 1, 100) <= o->hipri_percentage))
		flags |= RWF_HIPRI;
	/* [한국어] 확률적 HIPRI 부여 — 1..100 중 hipri_percentage 이하면 set. */
	if (!td->o.odirect && o->uncached)
		flags |= RWF_DONTCACHE;
	/* [한국어] O_DIRECT와 공존 의미 없음 → 버퍼드일 때만 활성. */
	if (o->nowait)
		flags |= RWF_NOWAIT;

	iov->iov_base = io_u->xfer_buf;
	iov->iov_len = io_u->xfer_buflen;

	if (io_u->ddir == DDIR_READ)
		ret = preadv2(f->fd, iov, 1, io_u->offset, flags);
	else if (io_u->ddir == DDIR_WRITE) {
		if (td->o.oatomic)
			flags |= RWF_ATOMIC;
		/* [한국어] torn-write 방지 atomic write — 커널/FS 지원 필요. */
		ret = pwritev2(f->fd, iov, 1, io_u->offset, flags);
	} else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		return FIO_Q_COMPLETED;
	} else
		ret = do_io_u_sync(td, io_u);

	return fio_io_end(td, io_u, ret);
}
#endif

/*
 * [한국어]
 * fio_psyncio_queue - "psync" 엔진 queue: pread/pwrite 호출.
 * lseek이 불필요하므로 sync 대비 prep가 비어 있고, 멀티스레드가 한 fd를 공유해도 안전하다.
 * 호출 체인: td_io_queue → [이 함수] → pread(2)/pwrite(2)
 */
static enum fio_q_status fio_psyncio_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_READ)
		ret = pread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
	else if (io_u->ddir == DDIR_WRITE)
		ret = pwrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		return FIO_Q_COMPLETED;
	} else
		ret = do_io_u_sync(td, io_u);

	return fio_io_end(td, io_u, ret);
}

/*
 * [한국어]
 * fio_syncio_queue - "sync"(기본) 엔진 queue: read/write 동기 호출.
 *
 * fio_syncio_prep()가 먼저 LAST_POS를 보고 필요시 lseek을 수행했다고 가정한다.
 * fio의 "기본 엔진"으로서 다른 모든 엔진의 성능 기준점이다.
 *
 * 호출 체인: td_io_queue → [이 함수] → read(2)/write(2)
 */
static enum fio_q_status fio_syncio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_READ)
		ret = read(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_WRITE)
		ret = write(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_TRIM) {
		do_io_u_trim(td, io_u);
		return FIO_Q_COMPLETED;
	} else
		ret = do_io_u_sync(td, io_u);

	return fio_io_end(td, io_u, ret);
}

/*
 * [한국어]
 * fio_vsyncio_getevents - vsync 계열 getevents: commit에서 이미 완료된 이벤트 카운터 반환.
 * @min: 0이면 소비 없이 0 반환(polling 목적), >0이면 누적된 events를 모두 드레인.
 * @return: 보고할 이벤트 수.
 * 호출 체인: td_io_getevents → [이 함수]
 */
static int fio_vsyncio_getevents(struct thread_data *td, unsigned int min,
				 unsigned int max,
				 const struct timespec fio_unused *t)
{
	struct syncio_data *sd = td->io_ops_data;
	int ret;

	if (min) {
		ret = sd->events;
		sd->events = 0;			/* [한국어] 한 번에 모두 소비. */
	} else
		ret = 0;

	dprint(FD_IO, "vsyncio_getevents: min=%d,max=%d: %d\n", min, max, ret);
	return ret;
}

/*
 * [한국어]
 * fio_vsyncio_event - event 인덱스로 io_us 배열에서 io_u 반환.
 * @event: 0..queued-1 범위(직전 커밋 시점의 큐 스냅샷).
 */
static struct io_u *fio_vsyncio_event(struct thread_data *td, int event)
{
	struct syncio_data *sd = td->io_ops_data;

	return sd->io_us[event];
}

/*
 * [한국어]
 * fio_vsyncio_append - 새 io_u가 기존 큐에 "연속 I/O"로 합쳐질 수 있는지 판정.
 * @return: 1=append 가능(같은 파일/방향/연속 오프셋), 0=별도 커밋 필요.
 */
static int fio_vsyncio_append(struct thread_data *td, struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;

	if (ddir_sync(io_u->ddir))
		return 0;		/* [한국어] sync/flush류는 절대 병합 금지. */

	if (io_u->offset == sd->last_offset && io_u->file == sd->last_file &&
	    io_u->ddir == sd->last_ddir)
		return 1;

	return 0;
}

/*
 * [한국어]
 * fio_vsyncio_set_iov - io_us[idx]/iovecs[idx]를 채우고 last_* 캐시 갱신.
 * @idx: 삽입 위치(0..queued).
 */
static void fio_vsyncio_set_iov(struct syncio_data *sd, struct io_u *io_u,
				int idx)
{
	sd->io_us[idx] = io_u;				/* [한국어] 이벤트 보고용 역링크. */
	sd->iovecs[idx].iov_base = io_u->xfer_buf;	/* [한국어] 사용자 버퍼 공유. */
	sd->iovecs[idx].iov_len = io_u->xfer_buflen;
	sd->last_offset = io_u->offset + io_u->xfer_buflen;	/* [한국어] 다음 병합 판정용. */
	sd->last_file = io_u->file;
	sd->last_ddir = io_u->ddir;
	sd->queued_bytes += io_u->xfer_buflen;		/* [한국어] 전체 바이트 합 누적. */
	sd->queued++;					/* [한국어] 큐 길이 +1. */
}

/*
 * [한국어]
 * fio_vsyncio_queue - "vsync" queue: 연속 I/O는 누적, 불연속이면 FIO_Q_BUSY로 커밋 유도.
 *
 * @return: FIO_Q_QUEUED(누적 성공) | FIO_Q_BUSY(커밋 필요) | FIO_Q_COMPLETED(sync ddir 즉시 처리).
 *
 * 흐름:
 *  1) append 불가 & 큐가 비어있음 → sync ddir이면 즉시 do_io_u_sync, 아니면 첫 엔트리로 시작.
 *  2) append 불가 & 큐 비어있지 않음 → FIO_Q_BUSY → fio가 commit 후 재시도.
 *  3) append 가능 & 큐 꽉참(iodepth 도달) → FIO_Q_BUSY.
 *  4) append 가능 & 여유 있음 → 큐에 추가 후 FIO_Q_QUEUED.
 *
 * 호출 체인: td_io_queue → [이 함수] (→ 이후 td_io_commit → fio_vsyncio_commit)
 */
static enum fio_q_status fio_vsyncio_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops_data;

	fio_ro_check(td, io_u);

	if (!fio_vsyncio_append(td, io_u)) {
		dprint(FD_IO, "vsyncio_queue: no append (%d)\n", sd->queued);
		/*
		 * If we can't append and have stuff queued, tell fio to
		 * commit those first and then retry this io
		 */
		if (sd->queued)
			return FIO_Q_BUSY;
		if (ddir_sync(io_u->ddir)) {
			int ret = do_io_u_sync(td, io_u);

			return fio_io_end(td, io_u, ret);
		}

		sd->queued = 0;			/* [한국어] 방어적 재설정. */
		sd->queued_bytes = 0;
		fio_vsyncio_set_iov(sd, io_u, 0);
	} else {
		if (sd->queued == td->o.iodepth) {
			dprint(FD_IO, "vsyncio_queue: max depth %d\n", sd->queued);
			return FIO_Q_BUSY;	/* [한국어] 배열 포화 → commit 유도. */
		}

		dprint(FD_IO, "vsyncio_queue: append\n");
		fio_vsyncio_set_iov(sd, io_u, sd->queued);
	}

	dprint(FD_IO, "vsyncio_queue: depth now %d\n", sd->queued);
	return FIO_Q_QUEUED;
}

/*
 * Check that we transferred all bytes, or saw an error, etc
 */
/*
 * [한국어]
 * fio_vsyncio_end - readv/writev 반환값 bytes를 각 io_u에 분배.
 *
 * @bytes: 실제 전송된 총 바이트(-1=errno 오류).
 * @return: 0=성공, -err=오류.
 *
 * 전부 전송되면 조기 반환. 부분 전송이면 iovec 순서대로 각 io_u->resid에 할당.
 * 오류 시 전체 io_u에 errno를 전파해 fio가 전체 실패로 기록하도록 한다.
 */
static int fio_vsyncio_end(struct thread_data *td, ssize_t bytes)
{
	struct syncio_data *sd = td->io_ops_data;
	struct io_u *io_u;
	unsigned int i;
	int err;

	/*
	 * transferred everything, perfect
	 */
	if (bytes == sd->queued_bytes)
		return 0;

	err = errno;		/* [한국어] 반복 중 다른 syscall이 errno를 덮을 수 있어 선캡처. */
	for (i = 0; i < sd->queued; i++) {
		io_u = sd->io_us[i];

		if (bytes == -1) {
			io_u->error = err;		/* [한국어] 전체 오류 전파. */
		} else {
			unsigned int this_io;

			this_io = bytes;
			if (this_io > io_u->xfer_buflen)
				this_io = io_u->xfer_buflen;	/* [한국어] 이 io_u 몫을 버퍼 크기로 캡. */

			io_u->resid = io_u->xfer_buflen - this_io;
			io_u->error = 0;
			bytes -= this_io;	/* [한국어] 남은 바이트에서 해당 몫 차감. */
		}
	}

	if (bytes == -1) {
		td_verror(td, err, "xfer vsync");
		return -err;
	}

	return 0;
}

/*
 * [한국어]
 * fio_vsyncio_commit - 누적된 iovecs를 lseek 후 readv/writev로 일괄 제출.
 *
 * @return: 0=성공 또는 빈 큐, -err=lseek/IO 실패.
 *
 * 동기 호출이므로 반환 시점에 모든 io_u가 완료된 상태이다. events=queued로
 * 이동시키고 queued=0으로 리셋하여 다음 라운드를 준비한다.
 *
 * 호출 체인: td_io_commit → [이 함수] → lseek + readv/writev → fio_vsyncio_end
 */
static int fio_vsyncio_commit(struct thread_data *td)
{
	struct syncio_data *sd = td->io_ops_data;
	struct fio_file *f;
	ssize_t ret;

	if (!sd->queued)
		return 0;		/* [한국어] 커밋할 것 없음. */

	io_u_mark_submit(td, sd->queued);	/* [한국어] 통계: 동시 발행 io 개수 기록. */
	f = sd->last_file;

	if (lseek(f->fd, sd->io_us[0]->offset, SEEK_SET) == -1) {
		int err = -errno;

		td_verror(td, errno, "lseek");
		return err;
	}
	/* [한국어] 연속 I/O의 시작 오프셋으로 파일 포인터 이동. */

	if (sd->last_ddir == DDIR_READ)
		ret = readv(f->fd, sd->iovecs, sd->queued);
	else
		ret = writev(f->fd, sd->iovecs, sd->queued);

	dprint(FD_IO, "vsyncio_commit: %d\n", (int) ret);
	sd->events = sd->queued;	/* [한국어] getevents가 보고할 완료 수. */
	sd->queued = 0;
	return fio_vsyncio_end(td, ret);
}

/*
 * [한국어]
 * fio_vsyncio_init - vsync/pvsync/pvsync2 공통 init.
 * syncio_data 할당, iovecs/io_us 배열(iodepth 크기) 확보, 난수 상태 초기화.
 * 호출 체인: td_io_init → [이 함수]
 */
static int fio_vsyncio_init(struct thread_data *td)
{
	struct syncio_data *sd;

	sd = calloc(1, sizeof(*sd));
	sd->last_offset = -1ULL;			/* [한국어] 무효 센티널 값. */
	sd->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));
	sd->io_us = malloc(td->o.iodepth * sizeof(struct io_u *));
	init_rand(&sd->rand_state, 0);			/* [한국어] pvsync2 hipri 분포용. */

	td->io_ops_data = sd;
	return 0;
}

/*
 * [한국어]
 * fio_vsyncio_cleanup - init에서 할당한 자원을 해제.
 */
static void fio_vsyncio_cleanup(struct thread_data *td)
{
	struct syncio_data *sd = td->io_ops_data;

	if (sd) {
		free(sd->iovecs);
		free(sd->io_us);
		free(sd);
	}
}

/* [한국어] "sync" 엔진 vtable — read/write + lseek. fio의 기본 엔진.
 * FIO_SYNCIO 플래그: fio 코어에 동기 엔진임을 알려 iodepth를 무시하도록.
 * FIO_SYNCFS 플래그: sync_file_range() 등 파일시스템 sync 기능 지원 가능. */
static struct ioengine_ops ioengine_rw = {
	.name		= "sync",
	.version	= FIO_IOOPS_VERSION,
	.prep		= fio_syncio_prep,
	.queue		= fio_syncio_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_SYNCFS,
};

/* [한국어] "psync" 엔진 vtable — pread/pwrite. prep 불필요. */
static struct ioengine_ops ioengine_prw = {
	.name		= "psync",
	.version	= FIO_IOOPS_VERSION,
	.queue		= fio_psyncio_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_SYNCFS,
};

/* [한국어] "vsync" 엔진 vtable — readv/writev 일괄. init/cleanup/commit/event/getevents 필요. */
static struct ioengine_ops ioengine_vrw = {
	.name		= "vsync",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_vsyncio_init,
	.cleanup	= fio_vsyncio_cleanup,
	.queue		= fio_vsyncio_queue,
	.commit		= fio_vsyncio_commit,
	.event		= fio_vsyncio_event,
	.getevents	= fio_vsyncio_getevents,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_SYNCFS,
};

/* [한국어] "pvsync" 엔진 vtable — preadv/pwritev. CONFIG_PWRITEV 빌드에서만 컴파일. */
#ifdef CONFIG_PWRITEV
static struct ioengine_ops ioengine_pvrw = {
	.name		= "pvsync",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_vsyncio_init,
	.cleanup	= fio_vsyncio_cleanup,
	.queue		= fio_pvsyncio_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_SYNCFS,
};
#endif

/* [한국어] "pvsync2" 엔진 vtable — preadv2/pwritev2 + RWF_*.
 * FIO_ATOMICWRITES 플래그: --atomic 옵션과 연동해 RWF_ATOMIC 경로 사용. */
#ifdef FIO_HAVE_PWRITEV2
static struct ioengine_ops ioengine_pvrw2 = {
	.name		= "pvsync2",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_vsyncio_init,
	.cleanup	= fio_vsyncio_cleanup,
	.queue		= fio_pvsyncio2_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_ATOMICWRITES | FIO_SYNCFS,
	.options	= options,
	.option_struct_size	= sizeof(struct psyncv2_options),
};
#endif

/*
 * [한국어]
 * fio_syncio_register - 프로그램 시작 시 5개 엔진을 한꺼번에 코어에 등록.
 * fio_init == __attribute__((constructor))로 main 이전에 자동 호출된다.
 */
static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine_rw);	/* [한국어] sync 등록. */
	register_ioengine(&ioengine_prw);	/* [한국어] psync 등록. */
	register_ioengine(&ioengine_vrw);	/* [한국어] vsync 등록. */
#ifdef CONFIG_PWRITEV
	register_ioengine(&ioengine_pvrw);	/* [한국어] pvsync(빌드 가능 시). */
#endif
#ifdef FIO_HAVE_PWRITEV2
	register_ioengine(&ioengine_pvrw2);	/* [한국어] pvsync2(커널 4.6+). */
#endif
}

/*
 * [한국어]
 * fio_syncio_unregister - 프로세스 종료 시 등록 해제(destructor).
 */
static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine_rw);
	unregister_ioengine(&ioengine_prw);
	unregister_ioengine(&ioengine_vrw);
#ifdef CONFIG_PWRITEV
	unregister_ioengine(&ioengine_pvrw);
#endif
#ifdef FIO_HAVE_PWRITEV2
	unregister_ioengine(&ioengine_pvrw2);
#endif
}
