/*
 * [한국어 설명] posixaio I/O 엔진 구현 (posixaio.c)
 *
 * === 파일의 역할 ===
 * POSIX 표준 비동기 I/O 라이브러리(aio.h; librt)를 사용하는 fio I/O 엔진. aio_read()/
 * aio_write()로 백그라운드에 I/O를 걸어두고, aio_error()/aio_return()으로 상태를 회수한다.
 * 리눅스의 libaio/io_uring 대비 성능은 낮지만 POSIX 표준이라 macOS/BSD 등 이식성이 좋다.
 * FIO_ASYNCIO_SYNC_TRIM/SYNC_SYNCFS 플래그가 설정되어 TRIM과 syncfs는 동기 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend 잡 루프가 load_ioengine("posixaio") 뒤 init→prep→queue→getevents→event
 * 순으로 호출한다. 이 엔진은 비동기 엔진이므로 queue()에서 FIO_Q_QUEUED를 반환한 뒤
 * getevents()에서 완료를 폴링/대기하고, event()로 완료된 io_u를 한 건씩 꺼내준다.
 * 실행 컨텍스트는 각 잡 스레드. librt가 내부적으로 헬퍼 스레드풀을 돌리는 구현이 많다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h: thread_data, io_u, ioengine_ops, generic_open_file 등.
 * - os/*: os_aiocb_t 타입 정의(플랫폼별 typedef of aiocb).
 * - librt: aio_* 시스템 라이브러리 실구현.
 * - 데이터 흐름: io_u → aiocb(fildes/buf/nbytes/offset) → aio_read/write → aio_error/return → io_u->resid/error.
 * - 공유 상태: td->io_ops_data 에 struct posixaio_data; td->io_u_all 순회로 비행 io_u 스캔.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct posixaio_data: 비행 큐 카운터 + 완료 이벤트 포인터 배열.
 * - fio_posixaio_prep(): aiocb 필드(fd/buf/len/off/SIGEV_NONE) 설정.
 * - fio_posixaio_queue(): aio_read/aio_write/aio_fsync 제출, EAGAIN 시 BUSY.
 * - fio_posixaio_getevents(): aio_error 폴링 + aio_suspend 대기 루프.
 * - fio_posixaio_event(): 이벤트 배열에서 io_u 반환.
 * - ts_utime_since_now(): 타임아웃 경과 usec 계산 헬퍼.
 */

/*
 * posixaio engine
 *
 * IO engine that uses the posix defined aio interface.
 *
 */
#include <stdio.h>    /* [한국어] 디버그 로그 포매팅 */
#include <stdlib.h>   /* [한국어] calloc/free — 상태/이벤트 배열 할당 */
#include <unistd.h>   /* [한국어] 표준 POSIX API */
#include <errno.h>    /* [한국어] errno 및 에러 코드(EINPROGRESS/ECANCELED/EAGAIN 등) */
#include <fcntl.h>    /* [한국어] O_SYNC 플래그 — aio_fsync 호출에 사용 */

#include "../fio.h"   /* [한국어] fio 코어 공용 타입/매크로 */

/*
 * [한국어] POSIX AIO 엔진의 잡별 상태.
 * td->io_ops_data에 저장되며 잡 스레드 단독 소유.
 */
struct posixaio_data {
	struct io_u **aio_events;
	/* [한국어] getevents에서 완료된 io_u들을 모아두는 배열(iodepth 크기).
	 * 설정자: fio_posixaio_getevents()가 인덱스 r에 push.
	 * 읽는 자: fio_posixaio_event()가 event 인덱스로 조회해 반환.
	 * 값 범위: 유효한 io_u 포인터 (완료 이벤트 구간).
	 * 동기화: 잡 스레드 단일 소유. */

	unsigned int queued;
	/* [한국어] 현재 librt에 발행되어 완료되지 않은 AIO 요청 수(in-flight).
	 * 설정자: queue()에서 aio_read/write 성공 시 ++; getevents가 완료 회수 시 --.
	 * 읽는 자: TRIM/sync 분기에서 드레인 판단(0이어야 동기 수행).
	 * 값 범위: 0..iodepth. 동기화: 잡 스레드 단일. */
};

/*
 * [한국어]
 * ts_utime_since_now - start 시각 이후 현재까지 경과 시간을 마이크로초로 반환.
 * @start: 단조 시계 기준 시작 시각.
 * @return: 경과 usec, 시계 실패 시 0.
 * 호출 체인: fio_posixaio_getevents() → [ts_utime_since_now].
 */
static unsigned long long ts_utime_since_now(const struct timespec *start)
{
	struct timespec now;  /* [한국어] 현재 시각 수신 버퍼 */

	if (fio_get_mono_time(&now) < 0)   /* [한국어] 단조 시계 조회 실패 시 0 반환(타임아웃 무시) */
		return 0;

	return utime_since(start, &now);   /* [한국어] start→now 경과를 usec로 환산 */
}

/*
 * [한국어]
 * fio_posixaio_prep - POSIX AIO의 aiocb 구조체를 io_u 정보로 채움
 *
 * aio_read()/aio_write()에 전달할 aiocb에 파일 디스크립터, 버퍼,
 * 크기, 오프셋을 설정한다. SIGEV_NONE으로 시그널 알림을 비활성화한다.
 *
 * 호출 체인: td_io_prep() → [이 함수]
 */
static int fio_posixaio_prep(struct thread_data fio_unused *td,
			     struct io_u *io_u)
{
	os_aiocb_t *aiocb = &io_u->aiocb;        /* [한국어] io_u에 임베드된 aiocb 참조 획득 */
	struct fio_file *f = io_u->file;          /* [한국어] 대상 파일 */

	aiocb->aio_fildes = f->fd;                 /* [한국어] POSIX AIO 대상 FD */
	aiocb->aio_buf = io_u->xfer_buf;           /* [한국어] 사용자 버퍼 포인터 */
	aiocb->aio_nbytes = io_u->xfer_buflen;     /* [한국어] 전송 바이트 수 */
	aiocb->aio_offset = io_u->offset;          /* [한국어] 파일 내 오프셋 */
	/* [한국어] SIGEV_NONE: 완료 시 시그널/스레드 알림 없음. fio가 aio_error로 폴링한다 */
	aiocb->aio_sigevent.sigev_notify = SIGEV_NONE;

	io_u->seen = 0;   /* [한국어] getevents가 이 io_u를 아직 수확하지 않았음을 표시 */
	return 0;         /* [한국어] 성공 */
}

#define SUSPEND_ENTRIES	8   /* [한국어] aio_suspend에 한 번에 넘길 aiocb 최대 개수(스택 버퍼 크기) */

/*
 * [한국어]
 * fio_posixaio_getevents - 완료된 AIO 이벤트 수집
 *
 * 비행 중인 모든 io_u를 순회하며 aio_error()로 완료 여부를 확인한다.
 * 완료된 것은 aio_return()으로 결과를 수집하고, 미완료는 suspend_list에 추가한다.
 * min개 이상 수집될 때까지 aio_suspend()로 대기하며 반복한다.
 * 타임아웃이 지정되면 경과 시간을 확인하여 조기 반환한다.
 *
 * 호출 체인: td_io_getevents() → [이 함수] → aio_error(3)/aio_suspend(3)
 */
static int fio_posixaio_getevents(struct thread_data *td, unsigned int min,
				  unsigned int max, const struct timespec *t)
{
	struct posixaio_data *pd = td->io_ops_data;         /* [한국어] 엔진 상태 */
	os_aiocb_t *suspend_list[SUSPEND_ENTRIES];           /* [한국어] aio_suspend 인자용 포인터 배열 */
	struct timespec start;                                /* [한국어] 타임아웃 기준 시작 시각 */
	int have_timeout = 0;                                  /* [한국어] t가 유효하고 시각 획득 성공 여부 */
	int suspend_entries;                                  /* [한국어] suspend_list에 채워진 항목 수 */
	struct io_u *io_u;                                     /* [한국어] 순회 중인 io_u */
	unsigned int r;                                        /* [한국어] 수확한 완료 이벤트 수 */
	int i;                                                 /* [한국어] io_u_qiter 인덱스 */

	/* [한국어] 타임아웃이 주어졌고 시각 획득이 성공하면 플래그 세팅, 아니면 start 0-init */
	if (t && fio_get_mono_time(&start) == 0)
		have_timeout = 1;
	else
		memset(&start, 0, sizeof(start));

	r = 0;  /* [한국어] 수확 카운터 초기화 */
restart:
	memset(suspend_list, 0, sizeof(suspend_list));   /* [한국어] 이번 라운드 suspend 목록 리셋 */
	suspend_entries = 0;
	/* [한국어] 이 잡의 모든 io_u를 순회하며 미수확 비행 상태인 것만 검사 */
	io_u_qiter(&td->io_u_all, io_u, i) {
		int err;

		/* [한국어] 이미 완료 보고 or 비행 상태가 아닌 io_u는 건너뛴다 */
		if (io_u->seen || !(io_u->flags & IO_U_F_FLIGHT))
			continue;

		err = aio_error(&io_u->aiocb);   /* [한국어] AIO 상태 조회 */
		if (err == EINPROGRESS) {
			/* [한국어] 아직 진행 중 — 아래 aio_suspend 대상 목록에 수집 */
			if (suspend_entries < SUSPEND_ENTRIES) {
				suspend_list[suspend_entries] = &io_u->aiocb;
				suspend_entries++;
			}
			continue;
		}

		io_u->seen = 1;                 /* [한국어] 이번 수확에서 집계 완료 표시 */
		pd->queued--;                    /* [한국어] 비행 카운터 감소 */
		pd->aio_events[r++] = io_u;     /* [한국어] 완료 이벤트 배열에 등록 */

		if (err == ECANCELED)
			/* [한국어] 커널이 취소한 경우 — 남은(resid) = 전체 크기로 처리 */
			io_u->resid = io_u->xfer_buflen;
		else if (!err) {
			/* [한국어] 성공 경로: aio_return으로 실제 전송 바이트 회수 */
			ssize_t retval = aio_return(&io_u->aiocb);

			io_u->resid = io_u->xfer_buflen - retval;  /* [한국어] 미전송 잔여분 기록 */
		} else
			io_u->error = err;  /* [한국어] 일반 에러 — io_u에 기록, 코어가 해석 */
	}

	if (r >= min)
		return r;  /* [한국어] 최소 수확 수 달성 — 반환 */

	if (have_timeout) {
		unsigned long long usec;
		/* [한국어] timespec t → usec 환산 */
		usec = (t->tv_sec * 1000000) + (t->tv_nsec / 1000);
		if (ts_utime_since_now(&start) > usec)
			return r;  /* [한국어] 타임아웃 초과 → 부분 결과로 반환 */
	}

	/*
	 * must have some in-flight, wait for at least one
	 */
	/* [한국어] 수집한 미완료 aiocb들에 대해 하나 이상 완료될 때까지 대기(또는 타임아웃) */
	aio_suspend((const os_aiocb_t * const *)suspend_list,
							suspend_entries, t);
	goto restart;  /* [한국어] 재스캔 */
}

/*
 * [한국어]
 * fio_posixaio_event - ioengine_ops.event. 수확된 인덱스의 io_u 반환.
 * 호출 체인: fio 코어(getevents 결과 소비) → [fio_posixaio_event].
 */
static struct io_u *fio_posixaio_event(struct thread_data *td, int event)
{
	struct posixaio_data *pd = td->io_ops_data;  /* [한국어] 엔진 상태 획득 */

	return pd->aio_events[event];                 /* [한국어] 배열에서 직접 조회 */
}

/*
 * [한국어]
 * fio_posixaio_queue - POSIX AIO 엔진의 I/O 제출 콜백
 *
 * aio_read()/aio_write()로 비동기 I/O를 제출한다.
 * EAGAIN 반환 시 FIO_Q_BUSY로 백프레셔를 적용한다(OSX에서 특히 빈번).
 * TRIM/sync는 큐가 비어있을 때만 동기적으로 처리한다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → aio_read(3)/aio_write(3)
 */
static enum fio_q_status fio_posixaio_queue(struct thread_data *td,
					    struct io_u *io_u)
{
	struct posixaio_data *pd = td->io_ops_data;   /* [한국어] 엔진 상태 */
	os_aiocb_t *aiocb = &io_u->aiocb;              /* [한국어] prep에서 채워진 aiocb */
	int ret;

	fio_ro_check(td, io_u);   /* [한국어] readonly 잡 WRITE 단속 */

	if (io_u->ddir == DDIR_READ)
		ret = aio_read(aiocb);     /* [한국어] 비동기 읽기 제출 */
	else if (io_u->ddir == DDIR_WRITE)
		ret = aio_write(aiocb);    /* [한국어] 비동기 쓰기 제출 */
	else if (io_u->ddir == DDIR_TRIM) {
		/* [한국어] TRIM은 POSIX AIO에 없어 동기 처리.
		 * 비행 중인 건이 있으면 드레인 유도(BUSY)하여 완료 후 재시도 */
		if (pd->queued)
			return FIO_Q_BUSY;

		do_io_u_trim(td, io_u);    /* [한국어] 동기 TRIM 실행 */
		return FIO_Q_COMPLETED;
	} else {
#ifdef CONFIG_POSIXAIO_FSYNC
		/* [한국어] aio_fsync 지원 플랫폼: SYNCFS가 아니면 O_SYNC 스타일로 비동기 fsync */
		if (io_u->ddir != DDIR_SYNCFS)
			ret = aio_fsync(O_SYNC, aiocb);
		else
			ret = 0;  /* [한국어] SYNCFS는 별도 동기 경로로 이후 처리 */
#else
		/* [한국어] aio_fsync 미지원 플랫폼: 동기 sync로 대체(드레인 후 실행) */
		if (pd->queued)
			return FIO_Q_BUSY;

		do_io_u_sync(td, io_u);
		return FIO_Q_COMPLETED;
#endif
	}

	if (ret) {
		int aio_err = errno;   /* [한국어] 실패 — errno 스냅샷 */

		/*
		 * At least OSX has a very low limit on the number of pending
		 * IOs, so if it returns EAGAIN, we are out of resources
		 * to queue more. Just return FIO_Q_BUSY to naturally
		 * drop off at this depth.
		 */
		/* [한국어] EAGAIN: 커널/librt 큐 포화. BUSY 반환 → 실효 iodepth 자연 감쇠 */
		if (aio_err == EAGAIN)
			return FIO_Q_BUSY;

		io_u->error = aio_err;                      /* [한국어] 영구 에러 기록 */
		td_verror(td, io_u->error, "xfer");         /* [한국어] 잡 단위 에러 로깅 */
		return FIO_Q_COMPLETED;                      /* [한국어] 완료(=에러 동반)로 처리 */
	}

	pd->queued++;             /* [한국어] 비행 카운터 증가 */
	return FIO_Q_QUEUED;     /* [한국어] 정상 비동기 큐잉 */
}

/*
 * [한국어]
 * fio_posixaio_cleanup - 잡 종료 시 상태 해제.
 * 호출 체인: td_io_cleanup → [fio_posixaio_cleanup].
 */
static void fio_posixaio_cleanup(struct thread_data *td)
{
	struct posixaio_data *pd = td->io_ops_data;

	if (pd) {
		free(pd->aio_events);   /* [한국어] 이벤트 배열 해제 */
		free(pd);                /* [한국어] 상태 구조체 해제 */
	}
}

/*
 * [한국어]
 * fio_posixaio_init - POSIX AIO 엔진 초기화 콜백
 * posixaio_data를 할당하고, iodepth 크기의 이벤트 배열을 생성한다.
 */
static int fio_posixaio_init(struct thread_data *td)
{
	struct posixaio_data *pd;
	pd = calloc(1, sizeof(*pd));                                  /* [한국어] 상태 0-할당 */
	pd->aio_events = calloc(td->o.iodepth, sizeof(struct io_u *)); /* [한국어] 이벤트 배열 */

	td->io_ops_data = pd;  /* [한국어] 이후 모든 콜백이 이 포인터로 상태 접근 */
	return 0;
}

/*
 * [한국어]
 * ioengine (posixaio) - ioengine_ops 정의.
 * flags:
 *  - FIO_ASYNCIO_SYNC_TRIM: TRIM은 동기 경로로 처리(코어에 힌트)
 *  - FIO_ASYNCIO_SYNC_SYNCFS: syncfs도 동기 처리
 */
static struct ioengine_ops ioengine = {
	.name		= "posixaio",
	.version	= FIO_IOOPS_VERSION,
	.flags		= FIO_ASYNCIO_SYNC_TRIM |
				FIO_ASYNCIO_SYNC_SYNCFS,
	.init		= fio_posixaio_init,
	.prep		= fio_posixaio_prep,
	.queue		= fio_posixaio_queue,
	.getevents	= fio_posixaio_getevents,
	.event		= fio_posixaio_event,
	.cleanup	= fio_posixaio_cleanup,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
};

/* [한국어] 프로세스 로드시 생성자 — ioengine 자동 등록 */
static void fio_init fio_posixaio_register(void)
{
	register_ioengine(&ioengine);
}

/* [한국어] 프로세스 종료시 소멸자 — 등록 해제 */
static void fio_exit fio_posixaio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
