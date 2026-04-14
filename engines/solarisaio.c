/*
 * Native Solaris async IO engine
 *
 */

/*
 * [한국어 설명] Solaris 네이티브 AIO I/O 엔진 구현 (solarisaio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Solaris 계열 운영체제(및 파생)에서 제공하는 네이티브 비동기 I/O API
 * (<sys/asynch.h> 의 aioread(3AIO)/aiowrite(3AIO)/aiowait(3AIO)) 위에 fio 의 I/O
 * 엔진을 얹은 것이다. POSIX aio_* 계열(lio_listio 등)과는 별개의 Solaris 고유
 * 인터페이스이며, 완료 수확을 aiowait(3AIO) 로 한 건씩 가져오거나 SIGIO 시그널
 * 기반으로 비동기 콜백에서 가져오는 두 가지 모드를 지원한다. 최대 동시 요청 수가
 * 시스템 전역 상수 MAXASYNCHIO 로 제한되므로 엔진 init 에서 iodepth 를 그 값으로
 * 자동 하향 조정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: main → fio_backend → load_ioengine("solarisaio") 가 fio_solarisaio_register()
 * (constructor) 로 등록한 ioengine_ops 를 잡 스레드에 바인딩. 이후 잡 루프에서
 * get_io_u → td_io_prep(=fio_solarisaio_prep) → td_io_queue(=fio_solarisaio_queue) →
 * td_io_getevents(=fio_solarisaio_getevents) → td_io_event(=fio_solarisaio_event) →
 * put_io_u 순으로 반복한다. 실행 컨텍스트는 주로 잡 스레드이지만 SIGIO 사용 시
 * 신호 핸들러(비동기 시그널 컨텍스트) 에서도 완료 수집 경로(wait_for_event)가 실행된다.
 *
 * === 타 모듈과의 연결 ===
 * 상위: fio 코어(ioengines.c, io_u.c) 가 이 파일의 콜백들을 호출한다.
 * 하위: Solaris C 라이브러리(libaio/libc) 의 aioread/aiowrite/aiowait 및 fsync/fdatasync.
 * 공유 상태: struct solarisaio_data 를 td->io_ops_data 에 저장. io_u->resultp(aio_result_t)
 * 필드를 커널이 완료 시 갱신하며, fio 코어와 엔진이 이 멤버를 통해 결과를 공유한다.
 * 데이터 흐름(READ): 잡 스레드 queue → aioread 가 커널에 요청 → 커널이 io_u->resultp
 * (aio_return/aio_errno) 를 채움 → aiowait 가 해당 resultp 포인터를 반환 → container_of
 * 로 io_u 복원 → aio_events[] 로 fio 에 돌려줌. WRITE 경로도 대칭.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_solarisaio_init: aio_events 배열 할당, MAXASYNCHIO 기준으로 iodepth 하향, (옵션) SIGIO 설정.
 * - fio_solarisaio_prep: io_u 의 aio_result_t 를 AIO_INPROGRESS 로 초기화, engine_data 백포인터 세팅.
 * - fio_solarisaio_queue: ddir 분기로 aioread/aiowrite/fsync/fdatasync 호출, 깊이 도달 시 FIO_Q_BUSY.
 * - wait_for_event: aiowait 한 건 수확 → container_of 로 io_u 복원 → resid/error 계산 후 큐에 쌓음.
 * - fio_solarisaio_getevents/_event: 잡 스레드가 완료 배치 수확 및 개별 io_u 반환.
 * - struct solarisaio_data: 완료 배열, pending, nr, max_depth 를 묶은 잡 로컬 상태.
 */

#include <stdio.h>          /* [한국어] 표준 입출력. 간접 사용. */
#include <stdlib.h>         /* [한국어] calloc/free/exit. */
#include <unistd.h>         /* [한국어] fsync/fdatasync/close 등 POSIX. */
#include <signal.h>         /* [한국어] SIGIO 기반 완료 모드용 sigaction. */
#include <errno.h>          /* [한국어] aiowait 실패 원인 판별. */

#include "../fio.h"         /* [한국어] fio 공용 선언(thread_data/io_u/ioengine_ops/fio_ro_check 등). */

#include <sys/asynch.h>     /* [한국어] Solaris 네이티브 AIO: aio_result_t, aioread, aiowrite, aiowait, MAXASYNCHIO. */

/*
 * [한국어] struct solarisaio_data — Solaris AIO 엔진의 잡 로컬 상태.
 * td->io_ops_data 로 부착. 단일 잡 스레드가 소유하지만 SIGIO 핸들러도
 * 완료 경로에서 접근할 수 있으므로, 순서 보장을 위해 write_barrier 를 사용한다.
 */
/* [한국어] Solaris AIO 엔진의 내부 상태 구조체 */
struct solarisaio_data {
	struct io_u **aio_events;
	/* [한국어] 완료된 io_u 포인터를 쌓아두는 원형 외 배열(크기 max_depth).
	 * 설정자: wait_for_event 가 aio_pending 번째 자리에 저장.
	 * 읽는 자: fio_solarisaio_event(event 인덱스) 가 꺼냄.
	 * 동기화: 단일 생산자(wait_for_event) · 단일 소비자(getevents) 패턴.
	 *         SIGIO 사용 시에는 생산자가 시그널 핸들러가 되므로 write_barrier 로 순서 보장. */

	unsigned int aio_pending;
	/* [한국어] 현재까지 aio_events 에 쌓인, 아직 fio 에 돌려주지 않은 완료 건수.
	 * getevents 가 한꺼번에 차감하며 반환값으로 사용. */

	unsigned int nr;
	/* [한국어] 현재 커널에 '떠 있는' in-flight AIO 개수.
	 * queue 에서 제출 성공 시 ++, wait_for_event 에서 완료 수집 시 --.
	 * max_depth 와 비교해 백프레셔(FIO_Q_BUSY) 를 적용. */

	unsigned int max_depth;
	/* [한국어] 허용 최대 동시 AIO 개수. td->o.iodepth 와 MAXASYNCHIO 중 작은 값.
	 * init 에서 1회 설정 후 불변. */
};

/*
 * [한국어]
 * fio_solarisaio_prep - io_u 한 건을 AIO 제출 직전에 초기화.
 *
 * @td: 잡 스레드 thread_data (fio_unused 로 선언되어 있으나 실제로는 내부에서 참조).
 * @io_u: 준비할 I/O 유닛.
 * @return: 항상 0(성공).
 *
 * 왜 필요한가: aiowait 이후 container_of 로 io_u 를 복원하려면 io_u->resultp 가
 *              이 io_u 안에 들어 있어야 하며, aio_return 을 AIO_INPROGRESS 로 두어
 *              커널이 완료 후 갱신할 수 있게 한다. 또한 io_u->engine_data 에
 *              엔진 상태 백포인터를 걸어 wait_for_event 가 엔진 상태에 접근하게 한다.
 * 실행 컨텍스트: 잡 스레드(queue 이전).
 */
static int fio_solarisaio_prep(struct thread_data fio_unused *td,
			    struct io_u *io_u)
{
	struct solarisaio_data *sd = td->io_ops_data;                           /* [한국어] 엔진 상태 포인터 획득. */

	io_u->resultp.aio_return = AIO_INPROGRESS;                              /* [한국어] 커널이 완료 시 덮어쓸 '진행 중' 마커. */
	io_u->engine_data = sd;                                                 /* [한국어] wait_for_event 가 sd 에 접근할 수 있도록 백포인터 저장. */
	return 0;                                                               /* [한국어] 준비 성공. */
}

/*
 * [한국어]
 * wait_for_event - aiowait(3AIO) 로 한 건 수확하고 aio_events 배열에 쌓는다.
 *
 * @tv: 타임아웃(NULL 이면 무한 대기 — SIGIO 핸들러에서 그렇게 부름).
 *
 * 동작: aiowait 가 완료된 요청의 aio_result_t* 를 반환 → container_of 로 감싸는
 *       io_u 포인터 복원 → resid/error 계산 → aio_events[aio_pending] 에 저장 후
 *       write_barrier 로 순서 보장, aio_pending++ / nr-- 로 카운터 갱신.
 * 실행 컨텍스트: 잡 스레드(getevents 경유) 또는 SIGIO 핸들러.
 * 에러 처리: EINVAL 은 "대기할 요청 없음" 상황이라 조용히 반환, 그 외는 치명 → exit.
 */
static void wait_for_event(struct timeval *tv)
{
	struct solarisaio_data *sd;                                             /* [한국어] 완료 io_u 에서 역추적한 엔진 상태. */
	struct io_u *io_u;                                                      /* [한국어] 완료된 io_u. */
	aio_result_t *res;                                                      /* [한국어] aiowait 가 반환하는 결과 포인터(io_u->resultp 를 가리킴). */

	res = aiowait(tv);                                                      /* [한국어] 커널에서 하나 수확. tv=NULL 이면 무한 블록. */
	if (res == (aio_result_t *) -1) {                                       /* [한국어] 실패 반환 규약. */
		int err = errno;                                                /* [한국어] errno 즉시 캡처. */

		if (err != EINVAL) {                                            /* [한국어] EINVAL 이외는 복구 불가능한 것으로 간주. */
			log_err("fio: solarisaio got %d in aiowait\n", err);
			exit(err);                                              /* [한국어] 즉시 종료(예상치 못한 상태 전파 방지). */
		}
		return;                                                         /* [한국어] 대기할 요청이 없었음 → 정상 조기 반환. */
	} else if (!res)                                                        /* [한국어] NULL 반환 = 타임아웃(tv 가 0/지정된 경우). */
		return;

	io_u = container_of(res, struct io_u, resultp);                         /* [한국어] resultp 주소에서 감싸는 io_u 복원(offsetof 기반). */
	sd = io_u->engine_data;                                                 /* [한국어] prep 에서 저장해둔 엔진 상태 백포인터. */

	if (io_u->resultp.aio_return >= 0) {                                    /* [한국어] 성공 또는 부분전송. */
		io_u->resid = io_u->xfer_buflen - io_u->resultp.aio_return;     /* [한국어] 남은(미전송) 바이트 계산. */
		io_u->error = 0;                                                /* [한국어] 에러 없음. */
	} else
		io_u->error = io_u->resultp.aio_errno;                          /* [한국어] 실패: 커널이 기록한 errno 를 io_u 로 이관. */

	/*
	 * For SIGIO, we need a write barrier between the two, so that
	 * the ->aio_pending store is seen after the ->aio_events store
	 */
	sd->aio_events[sd->aio_pending] = io_u;                                 /* [한국어] 완료된 io_u 를 배열에 저장. */
	write_barrier();                                                        /* [한국어] 배열 스토어가 pending 카운터 스토어보다 먼저 보이도록 메모리 장벽. SIGIO 핸들러와 잡 스레드 간 관찰 순서 보장용. */
	sd->aio_pending++;                                                      /* [한국어] 수확분 증가(소비자가 이만큼 getevents 로 가져감). */
	sd->nr--;                                                               /* [한국어] in-flight 감소. */
}

/*
 * [한국어]
 * fio_solarisaio_getevents - 완료 배치 수확 콜백.
 *
 * @td: 잡 thread_data.
 * @min: 최소 수확 개수.
 * @max: 최대 수확 개수(미사용 — pending 전량 반환).
 * @t: 타임아웃(초+나노초). min==0 또는 NULL 이면 polling(0 대기).
 * @return: 이번 호출에서 잡이 가져가는 완료 개수.
 *
 * 동작: aio_pending 이 min 에 도달할 때까지 wait_for_event 를 반복.
 * 실행 컨텍스트: 잡 스레드. SIGIO 모드에서는 신호 핸들러도 pending 을 키움.
 */
static int fio_solarisaio_getevents(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct solarisaio_data *sd = td->io_ops_data;                           /* [한국어] 엔진 상태. */
	struct timeval tv;                                                      /* [한국어] aiowait 가 받는 타임아웃 구조체(초+마이크로초). */
	int ret;                                                                /* [한국어] 반환용 pending 스냅샷. */

	if (!min || !t) {                                                       /* [한국어] polling 요청(즉시 반환 허용). */
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	} else {                                                                /* [한국어] 블록 대기 타임아웃 지정. */
		tv.tv_sec = t->tv_sec;
		tv.tv_usec = t->tv_nsec / 1000;                                 /* [한국어] timespec(ns) → timeval(us) 변환. */
	}

	while (sd->aio_pending < min)                                           /* [한국어] 최소 수확 도달까지 반복 수확. */
		wait_for_event(&tv);

	/*
	 * should be OK without locking, as int operations should be atomic
	 */
	ret = sd->aio_pending;                                                  /* [한국어] 현재 보유한 완료 수 스냅샷. */
	sd->aio_pending -= ret;                                                 /* [한국어] 모두 소비한 것으로 차감(0으로). */
	return ret;                                                             /* [한국어] fio 코어에 수확 개수 알림. */
}

/*
 * [한국어]
 * fio_solarisaio_event - getevents 가 수확한 배열에서 event 번째 io_u 반환.
 *
 * @td: 잡 thread_data.
 * @event: 0 <= event < 방금 getevents 반환값.
 * @return: 해당 완료 io_u.
 * 실행 컨텍스트: 잡 스레드(getevents 직후 연속 호출).
 */
static struct io_u *fio_solarisaio_event(struct thread_data *td, int event)
{
	struct solarisaio_data *sd = td->io_ops_data;                           /* [한국어] 엔진 상태. */

	return sd->aio_events[event];                                           /* [한국어] 지정 인덱스의 io_u 반환. */
}

/*
 * [한국어]
 * fio_solarisaio_queue - Solaris AIO 엔진의 I/O 제출 콜백
 *
 * aioread()/aiowrite()로 Solaris 네이티브 비동기 I/O를 제출한다.
 * max_depth에 도달하면 FIO_Q_BUSY를 반환하여 백프레셔를 적용한다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → aioread()/aiowrite()
 *
 * 동기 계열(DDIR_SYNC/DATASYNC): in-flight(nr!=0) 이면 FIO_Q_BUSY 로 대기 요청하여
 *   이전 비동기 I/O 완료 후에만 fsync/fdatasync 수행(순서 보장).
 */
static enum fio_q_status fio_solarisaio_queue(struct thread_data fio_unused *td,
			      struct io_u *io_u)
{
	struct solarisaio_data *sd = td->io_ops_data;                           /* [한국어] 엔진 상태. */
	struct fio_file *f = io_u->file;                                        /* [한국어] 대상 파일. */
	off_t off;                                                              /* [한국어] 요청 오프셋. */
	int ret;                                                                /* [한국어] aio 제출 결과. */

	fio_ro_check(td, io_u);                                                 /* [한국어] readonly 잡에서 write 금지. */

	if (io_u->ddir == DDIR_SYNC) {                                          /* [한국어] 전체 동기화 요청. */
		if (sd->nr)                                                     /* [한국어] 아직 떠 있는 AIO 가 있으면. */
			return FIO_Q_BUSY;                                      /* [한국어] 백프레셔 — fio 가 나중에 재제출. */
		if (fsync(f->fd) < 0)                                           /* [한국어] 디스크 메타+데이터 동기화. */
			io_u->error = errno;

		return FIO_Q_COMPLETED;                                         /* [한국어] 동기 호출이라 즉시 완료. */
	}

	if (io_u->ddir == DDIR_DATASYNC) {                                      /* [한국어] 데이터만 동기화(메타 제외). */
		if (sd->nr)
			return FIO_Q_BUSY;
		if (fdatasync(f->fd) < 0)
			io_u->error = errno;

		return FIO_Q_COMPLETED;
	}

	if (sd->nr == sd->max_depth)                                            /* [한국어] MAXASYNCHIO 한도 도달 시 백프레셔. */
		return FIO_Q_BUSY;

	off = io_u->offset;                                                     /* [한국어] 오프셋 로드. */
	if (io_u->ddir == DDIR_READ)                                            /* [한국어] 읽기 제출. */
		ret = aioread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, off,
					SEEK_SET, &io_u->resultp);              /* [한국어] 네이티브 비동기 read. resultp 로 완료 통지. */
	else
		ret = aiowrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, off,
					SEEK_SET, &io_u->resultp);              /* [한국어] 네이티브 비동기 write. */
	if (ret) {                                                              /* [한국어] 제출 자체가 실패. */
		io_u->error = errno;                                            /* [한국어] errno 캡처. */
		td_verror(td, io_u->error, "xfer");                             /* [한국어] fio 에 에러 등록. */
		return FIO_Q_COMPLETED;                                         /* [한국어] 큐잉 실패 → 즉시 완료(에러)로 처리. */
	}

	sd->nr++;                                                               /* [한국어] in-flight 카운트 증가. */
	return FIO_Q_QUEUED;                                                    /* [한국어] 비동기 큐잉 성공. */
}

/*
 * [한국어]
 * fio_solarisaio_cleanup - 엔진 종료 시 상태 구조체·이벤트 배열 해제.
 * 실행 컨텍스트: 잡 스레드(잡 종료). sd NULL 안전.
 */
static void fio_solarisaio_cleanup(struct thread_data *td)
{
	struct solarisaio_data *sd = td->io_ops_data;                           /* [한국어] 엔진 상태. */

	if (sd) {                                                               /* [한국어] 초기화 실패 경로 대비. */
		free(sd->aio_events);                                           /* [한국어] 완료 배열 해제. */
		free(sd);                                                       /* [한국어] 상태 해제. */
	}
}

/*
 * Set USE_SIGNAL_COMPLETIONS to use SIGIO as completion events.
 */
#ifdef USE_SIGNAL_COMPLETIONS
/*
 * [한국어]
 * fio_solarisaio_sigio - SIGIO 시그널 핸들러.
 *
 * 왜 필요한가: 커널이 AIO 완료를 SIGIO 로 알려주도록 구성한 경우,
 * 잡 스레드가 명시적으로 aiowait 를 호출하지 않아도 핸들러에서 수확 가능.
 * 실행 컨텍스트: **비동기 시그널 컨텍스트**(재진입 주의).
 * wait_for_event(NULL) 로 무한대기 없이 즉시 한 건 수확 시도.
 */
static void fio_solarisaio_sigio(int sig)
{
	wait_for_event(NULL);                                                   /* [한국어] 시그널 도착 시 완료 1건 수확. NULL=블록 않음(사실상 대기=0 폴링 동치). */
}

/*
 * [한국어]
 * fio_solarisaio_init_sigio - SIGIO 핸들러 등록.
 * 실행 컨텍스트: 잡 스레드(init 단계 1회).
 */
static void fio_solarisaio_init_sigio(void)
{
	struct sigaction act;                                                   /* [한국어] 시그널 액션 구조체. */

	memset(&act, 0, sizeof(act));                                           /* [한국어] 필드 0으로 초기화(마스크/플래그 초기값). */
	act.sa_handler = fio_solarisaio_sigio;                                  /* [한국어] 핸들러 지정. */
	act.sa_flags = SA_RESTART;                                              /* [한국어] 시스템콜 자동 재시작 플래그(EINTR 최소화). */
	sigaction(SIGIO, &act, NULL);                                           /* [한국어] SIGIO 에 대해 액션 적용. */
}
#endif

/*
 * [한국어]
 * fio_solarisaio_init - 엔진 초기화 콜백.
 *
 * @td: 잡 thread_data.
 * @return: 0 성공(현 구현은 실패 경로 없음).
 *
 * 동작: 상태 할당 → iodepth 를 MAXASYNCHIO 로 상한 → aio_events 배열 할당 →
 *       (옵션) SIGIO 핸들러 설치 → io_ops_data 에 상태 등록.
 * 주의: calloc 실패 체크 없음(원 코드 보존).
 */
static int fio_solarisaio_init(struct thread_data *td)
{
	unsigned int max_depth;                                                 /* [한국어] 실제 사용할 최대 깊이. */
	struct solarisaio_data *sd;                                             /* [한국어] 엔진 상태. */
	sd = calloc(1, sizeof(*sd));                                            /* [한국어] 0 초기화 할당. */

	max_depth = td->o.iodepth;                                              /* [한국어] 잡이 요청한 깊이. */
	if (max_depth > MAXASYNCHIO) {                                          /* [한국어] OS 가 허용하는 상한을 넘으면. */
		max_depth = MAXASYNCHIO;                                        /* [한국어] 상한으로 클램프. */
		log_info("fio: lower depth to %d due to OS constraints\n",
							max_depth);             /* [한국어] 사용자에게 안내 로그. */
	}

	sd->aio_events = calloc(max_depth, sizeof(struct io_u *));              /* [한국어] 완료 버퍼 크기는 max_depth 로 고정. */
	sd->max_depth = max_depth;                                              /* [한국어] 백프레셔 비교용 저장. */

#ifdef USE_SIGNAL_COMPLETIONS
	fio_solarisaio_init_sigio();                                            /* [한국어] SIGIO 수확 모드 준비. */
#endif

	td->io_ops_data = sd;                                                   /* [한국어] fio 코어에 상태 등록. */
	return 0;                                                               /* [한국어] 성공. */
}

/*
 * [한국어] ioengine — Solaris AIO 엔진 콜백 테이블.
 * 파일 수명은 generic_* 공용 구현을 사용.
 */
static struct ioengine_ops ioengine = {
	.name		= "solarisaio",                                         /* [한국어] --ioengine=solarisaio 로 선택. */
	.version	= FIO_IOOPS_VERSION,                                    /* [한국어] ABI 버전. */
	.init		= fio_solarisaio_init,                                  /* [한국어] 초기화. */
	.prep		= fio_solarisaio_prep,                                  /* [한국어] 제출 직전 io_u 초기화. */
	.queue		= fio_solarisaio_queue,                                 /* [한국어] I/O 제출. */
	.getevents	= fio_solarisaio_getevents,                             /* [한국어] 완료 수확. */
	.event		= fio_solarisaio_event,                                 /* [한국어] 수확된 io_u 조회. */
	.cleanup	= fio_solarisaio_cleanup,                               /* [한국어] 종료 자원 해제. */
	.open_file	= generic_open_file,                                    /* [한국어] 공용 open. */
	.close_file	= generic_close_file,                                   /* [한국어] 공용 close. */
	.get_file_size	= generic_get_file_size,                                /* [한국어] 공용 크기 질의. */
};

/*
 * [한국어]
 * fio_solarisaio_register - 모듈 로드 시 엔진 등록(constructor).
 */
static void fio_init fio_solarisaio_register(void)
{
	register_ioengine(&ioengine);                                           /* [한국어] ioengines.c 전역 리스트에 추가. */
}

/*
 * [한국어]
 * fio_solarisaio_unregister - 모듈 언로드 시 엔진 해제(destructor).
 */
static void fio_exit fio_solarisaio_unregister(void)
{
	unregister_ioengine(&ioengine);                                         /* [한국어] 전역 리스트에서 제거. */
}
