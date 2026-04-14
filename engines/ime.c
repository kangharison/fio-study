/*
 * FIO engines for DDN's Infinite Memory Engine.
 * This file defines 3 engines: ime_psync, ime_psyncv, and ime_aio
 *
 * Copyright (C) 2018      DataDirect Networks. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation..
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/*
 * [한국어 설명] DDN IME(Infinite Memory Engine) 버스트 버퍼 I/O 엔진 묶음 (ime.c)
 *
 * === 파일의 역할 ===
 * DDN의 버스트 버퍼 플랫폼 IME에 접근하기 위한 세 가지 fio I/O 엔진을 하나의 TU에서
 * 함께 구현한다: ime_psync(요청 단위 동기 호출), ime_psyncv(iovec 누적 후 단일 동기
 * preadv/pwritev), ime_aio(다수 요청을 비동기로 제출/수확). IME 네이티브 라이브러리의
 * ime_native_* 심볼을 직접 링크하여 사용하며, 각 엔진은 fio의 queue/commit/getevents/
 * event 계약을 IME API 특성에 맞게 다르게 채운다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=ime_psync / ime_psyncv / ime_aio 로 선택되는 세 플러그인이 모두 하나의
 * fio_init 생성자 경로(fio_ime_*_register)에서 등록된다. 실행 흐름은 backend.c →
 * td_io_init → fio_ime_*_setup → fio_ime_*_open_file → I/O 루프. ime_psync는 매번 바로
 * 완료를 반환하지만, ime_psyncv는 iodepth_batch만큼 iovec을 쌓은 뒤 commit에서
 * ime_native_preadv/pwritev를 호출한다. ime_aio는 여러 리퀘스트 큐를 병렬로 운용한다.
 * 실행 컨텍스트는 fio 잡 스레드 1개이며, aio 완료 대기는 pthread_mutex/cond로 동기화한다.
 *
 * === 타 모듈과의 연결 ===
 * 상단: fio 코어의 ioengine 플러그인 경로(ioengines.c)에서 호출된다.
 * 하단: libim_client(ime_native_init/finalize/pread/pwrite/preadv/pwritev/aio_read/
 *       aio_write/aio_wait/open/close/lstat/unlink)를 호출한다.
 * 데이터 흐름: io_u->xfer_buf ↔ ime_native_* ↔ IME 클라이언트 라이브러리 ↔ 버스트 버퍼.
 * 공유 상태: 각 엔진의 ime_data 구조체(iovecs, completed events, aio request lists)는
 * td->io_ops_data에 저장되어 잡 스레드 단독 소유이나, aio 완료 콜백은 IME 라이브러리의
 * 내부 스레드에서 호출되므로 pthread_mutex로 보호된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_ime_psync_queue(): ime_native_pread/pwrite 직접 호출 후 FIO_Q_COMPLETED 반환.
 * - fio_ime_psyncv_{queue,commit,getevents,event}(): iovec 누적 + 단일 preadv/pwritev.
 * - fio_ime_aio_{queue,commit,getevents,event}(): ime_native_aio_read/write 다중 제출.
 * - fio_ime_aio_complete_cb(): IME 콜백에서 완료 카운트 증가 + cond_signal.
 * - struct imesio_req / imeaio_req: iovec 누적 요청 기술자.
 * - 공통 open_file/close_file/get_file_size/unlink_file: ime_native 파일 관리 래핑.
 */

/*
 * Some details about the new engines are given below:
 *
 *
 * ime_psync:
 * Most basic engine that issues calls to ime_native whenever an IO is queued.
 *
 * ime_psyncv:
 * This engine tries to queue the IOs (by creating iovecs) if asked by FIO (via
 * iodepth_batch). It refuses to queue when the iovecs can't be appended, and
 * waits for FIO to issue a commit. After a call to commit and get_events, new
 * IOs can be queued.
 *
 * ime_aio:
 * This engine tries to queue the IOs (by creating iovecs) if asked by FIO (via
 * iodepth_batch). When the iovecs can't be appended to the current request, a
 * new request for IME is created. These requests will be issued to IME when
 * commit is called. Contrary to ime_psyncv, there can be several requests at
 * once. We don't need to wait for a request to terminate before creating a new
 * one.
 */

#include <stdio.h>       /* [한국어] snprintf/log_err 포맷 I/O를 위해 포함 - dprint 등 fio 로깅에서 내부적으로 사용. */
#include <stdlib.h>      /* [한국어] malloc/calloc/free 동적 메모리 관리 - ime_data, iovecs, io_us 배열 할당에 사용. */
#include <errno.h>       /* [한국어] 시스템 호출(unlink, ime_native_*)이 실패 시 전역 errno를 읽어 td_verror에 전달하기 위해 필요. */
#include <linux/limits.h>/* [한국어] PATH_MAX 상수 제공 - fio_set_ime_filename()이 thread-local 경로 버퍼 크기로 사용. */
#include <ime_native.h>  /* [한국어] DDN IME 네이티브 클라이언트 API 헤더 - ime_native_init/pread/pwrite/preadv/pwritev/aio_read/aio_write/stat/open/close/ftruncate/fsync와 struct ime_aiocb, DEFAULT_IME_FILE_PREFIX 제공. */

#include "../fio.h"      /* [한국어] fio 코어 API - struct thread_data, struct io_u, struct fio_file, ioengine_ops, dprint, td_verror, fio_ro_check, register_ioengine 등. */


/**************************************************************
 *              Types and constants definitions
 *
 **************************************************************/

/* define constants for async IOs */
/* [한국어] imeaio_req->status에 "아직 진행 중"임을 표시하는 sentinel 값.
 *   - 설정자: fio_ime_aio_enqueue()가 새 요청을 만들 때.
 *   - 읽는 자: fio_ime_aio_getevents()가 완료 여부 판정 시, 그리고 완료 콜백이 덮어씀.
 *   - 값 범위: 음수(-1) 하나만 이 의미로 사용. 0 이상은 전송 바이트 수로 해석됨. */
#define FIO_IME_IN_PROGRESS -1
/* [한국어] imeaio_req->status에 "IME 라이브러리가 에러를 돌려줬다"를 표시하는 sentinel.
 *   - 설정자: 완료 콜백 fio_ime_aio_complete_cb()가 err!=0일 때, 또는 commit에서 제출 실패 시.
 *   - 읽는 자: getevents 루프가 io_u->error = EIO로 매핑. */
#define FIO_IME_REQ_ERROR   -2

/* This flag is used when some jobs were created using threads. In that
   case, IME can't be finalized in the engine-specific cleanup function,
   because other threads might still use IME. Instead, IME is finalized
   in the destructor (see fio_ime_unregister), only when the flag
   fio_ime_is_initialized is true (which means at least one thread has
   initialized IME). */
/* [한국어] IME 라이브러리가 최소 한 스레드에 의해 초기화되었는지 여부의 전역 플래그.
 *   - 설정자: fio_ime_engine_init()이 ime_native_init() 성공 후 true로 설정.
 *   - 해제자: 프로세스 모드 finalize(fio_ime_engine_finalize) 또는 스레드 모드에서
 *     전체 언로드 시 fio_ime_unregister()에서 해제.
 *   - 값 범위: true/false. 스레드 모드에서는 여러 잡이 공유하지만, fio 초기화 순서상
 *     잡 스레드 생성 이전에 init이 호출되면 경쟁 없음(경고 로그만 출력).
 *   - 동기화: 잡 스레드 생성 전/후 순서 보장에 의존하며 별도 락 없음. */
static bool fio_ime_is_initialized = false;

/* [한국어] IME 동기 I/O 요청 구조체 (ime_psync/ime_psyncv 엔진용)
 *
 * ime_psyncv가 iovec을 누적할 때, 이 하나의 요청 기술자가 "현재 모아놓은 배치가 어느
 * 파일/오프셋/방향에 대한 것인지"를 기억한다. 배치가 비어 있으면(queued==0) 다음
 * enqueue가 필드를 초기화한다. */
struct imesio_req {
	int 			fd;
	/* [한국어] 이 배치의 대상 파일 디스크립터(ime_native_open 반환값).
	 * 설정자: fio_ime_psyncv_enqueue()에서 큐가 비어 있을 때 io_u->file->fd로 채움.
	 * 읽는 자: fio_ime_psyncv_can_queue()가 "새 io_u가 같은 fd인지" 비교,
	 *          fio_ime_psyncv_commit()이 preadv/pwritev 첫 인자로 사용.
	 * 값 범위: 유효한 IME fd (>=0). -1이면 의도치 않은 상태.
	 * 동기화: 잡 스레드 단독 소유(싱글 스레드 컨텍스트). */
	/* [한국어 원주석] 파일 디스크립터 */

	enum fio_ddir	ddir;
	/* [한국어] 이 배치의 I/O 방향(DDIR_READ/DDIR_WRITE). IME psyncv는 readv/writev가
	 *          분리되어 있어 한 배치는 동일 방향이어야 한다.
	 * 설정자: fio_ime_psyncv_enqueue()가 큐가 빌 때 io_u->ddir로 초기화.
	 * 읽는 자: can_queue()가 같은 방향인지 확인, commit()이 preadv/pwritev 선택 분기에 사용.
	 * 값 범위: DDIR_READ(0) 또는 DDIR_WRITE(1). DDIR_SYNC는 이 구조체를 거치지 않고
	 *          queue()에서 즉시 ime_native_fsync로 처리. */
	/* [한국어 원주석] I/O 방향 (READ 또는 WRITE) */

	off_t			offset;
	/* [한국어] 배치의 시작 파일 오프셋. preadv/pwritev 호출 시 네 번째 인자로 전달.
	 * 설정자: enqueue()가 큐가 빌 때 io_u->offset으로 초기화.
	 * 읽는 자: commit()이 ime_native_preadv/pwritev 인자로 사용.
	 * 값 범위: 0 이상의 파일 오프셋(바이트).
	 * 주의: ime_d->last_offset으로 "연속성"을 검증한 경우에만 iovec을 추가하므로
	 *       모든 iovec은 offset부터 last_offset까지 끊김 없이 연결된다. */
	/* [한국어 원주석] 파일 오프셋 */
};
/* [한국어] IME 비동기 I/O 요청 구조체 (ime_aio 엔진용)
 *
 * ime_aio는 iodepth만큼의 요청을 링으로 운용하며, 각 요청은 인접한 iovec 여러 개를
 * 하나의 ime_native_aio_{read,write} 호출로 묶는다. 완료 콜백은 IME 라이브러리의
 * 내부 스레드에서 호출되므로 status는 mutex/cond 쌍으로 동기화된다. */
struct imeaio_req {
	struct ime_aiocb 	iocb;
	/* [한국어] IME가 요구하는 비동기 I/O 제어 블록(ime_native.h 정의).
	 * 필드: fd/file_offset/iov/iovcnt/flags/complete_cb/user_context.
	 * 설정자: fio_ime_aio_enqueue()가 배치 시작 시 전부 채움. append 경로에서는
	 *          iovcnt만 증가시켜 기존 iocb를 확장.
	 * 읽는 자: commit()이 ime_native_aio_read/write()에 포인터 전달.
	 *          완료 콜백에서 user_context(→ 자기 자신)를 통해 역참조.
	 * 동기화: 제출 후 완료 콜백까지 IME 라이브러리가 소유. fio는 status로만 확인. */
	/* [한국어 원주석] IME AIO 제어 블록 */

	ssize_t      		status;
	/* [한국어] 요청 상태 머신.
	 *   FIO_IME_IN_PROGRESS(-1): enqueue 직후, 완료 콜백 전.
	 *   FIO_IME_REQ_ERROR(-2):   라이브러리가 err!=0을 보고 또는 제출 실패.
	 *   >=0:                     콜백이 전달한 실제 전송 바이트 수.
	 * 설정자: enqueue(IN_PROGRESS), commit 실패(REQ_ERROR),
	 *          완료 콜백 fio_ime_aio_complete_cb()(bytes 또는 REQ_ERROR).
	 * 읽는 자: fio_ime_aio_getevents()가 완료 여부 판정 및 io_u->resid 계산에 사용.
	 * 동기화: status_mutex로 보호. 콜백 기록 → signal → getevents가 wait에서 깨어남. */
	/* [한국어 원주석] 요청 상태 (IN_PROGRESS/완료 바이트/에러) */

	enum fio_ddir		ddir;
	/* [한국어] 요청의 I/O 방향. commit()이 aio_read vs aio_write 선택 분기에 사용.
	 * 설정자: enqueue()가 새 요청을 만들 때 io_u->ddir로 초기화.
	 * 읽는 자: can_append()가 연속 io_u와 같은 방향인지 검사, commit()의 분기. */
	/* [한국어 원주석] I/O 방향 */

	pthread_cond_t		cond_endio;
	/* [한국어] 완료 콜백 → getevents 스레드 간 wake-up을 위한 POSIX 조건변수.
	 * 설정자: init()이 pthread_cond_init으로 생성, clean()이 destroy.
	 * 시그널: 완료 콜백이 status 갱신 후 pthread_cond_signal.
	 * 대기: getevents()가 status==IN_PROGRESS인 동안 pthread_cond_wait 루프.
	 * 동기화: 반드시 status_mutex와 함께 사용(전형적 mutex+cond 패턴). */
	/* [한국어 원주석] 완료 통지용 조건 변수 */

	pthread_mutex_t		status_mutex;
	/* [한국어] status 필드의 원자적 기록/판독을 보호하는 POSIX 뮤텍스.
	 * 설정자/해제자: init/clean에서 init/destroy.
	 * 경쟁: 완료 콜백(IME 내부 스레드) vs getevents(잡 스레드) 동시 접근.
	 * 보호 범위: status 읽기/쓰기 및 cond_wait/signal 코드 경로. */
	/* [한국어 원주석] 상태 보호용 뮤텍스 */
};

/* This structure will be used for 2 engines: ime_psyncv and ime_aio */
/* [한국어] ime_psyncv와 ime_aio가 공유하는 엔진별 상태 컨테이너. td->io_ops_data에 저장되어
 *          잡 스레드 생명주기 동안 유지된다. ime_psync는 단순 동기 엔진이라 이 구조체를 쓰지 않음. */
struct ime_data {
	union {
		struct imeaio_req 	*aioreqs;
		/* [한국어] ime_aio: iodepth 크기의 요청 배열(링 버퍼).
		 * 설정자: fio_ime_aio_init()이 malloc, cond/mutex init.
		 * 해제자: fio_ime_aio_clean()이 destroy 후 free.
		 * 접근 인덱스: head(enqueue), tail(pop/getevents), cur_commit(commit). */
		struct imesio_req	*sioreq;
		/* [한국어] ime_psyncv: 단일 배치 요청 기술자(하나만 사용).
		 * psyncv는 하나의 preadv/pwritev 호출로 모든 iovec을 커밋하므로 요청 객체도 1개. */
		/* [한국어 원주석] array of aio requests / pointer to the only syncio request
		 * 두 엔진이 메모리를 공유하기 위해 union - 엔진 선택 시점에 한쪽만 유효. */
	};
	struct iovec 	*iovecs;
	/* [한국어] 제출 대기 중인 scatter/gather 버퍼 배열(크기=iodepth).
	 * 설정자: init()이 malloc, enqueue()가 io_u의 xfer_buf/xfer_buflen을 기록.
	 * 읽는 자: psyncv commit은 ime_native_preadv/pwritev에 전달, aio는 iocb->iov로 가리킴.
	 * 인덱스: head 위치에 append, iocb는 배열의 연속 구간을 iov로 묶어 참조. */
	/* [한국어 원주석] array of queued iovecs */

	struct io_u 	**io_us;
	/* [한국어] iovec과 1:1 대응되는 io_u 포인터 배열(크기=2*iodepth로 할당되어 앞 iodepth는
	 *          "queued" 용, 뒤 iodepth는 event_io_us 용으로 분할).
	 * 설정자: enqueue()가 head 위치에 io_u 저장.
	 * 읽는 자: getevents()가 완료 순서대로 event_io_us에 복사, psyncv_end가 io_u->error/resid 기록. */
	/* [한국어 원주석] array of queued io_u pointers */

	struct io_u 	**event_io_us;
	/* [한국어] get_events가 fio에 반환할 완료 io_u들의 임시 배열(io_us 뒤쪽 iodepth 구간).
	 * 설정자: getevents() 루프가 채움.
	 * 읽는 자: fio_ime_event(td, event_idx)가 event_io_us[event_idx]를 fio에 반환. */
	/* [한국어 원주석] array of the events retrieved after get_events */

	unsigned int 	queued;
	/* [한국어] 현재 큐에 적재된(iovec으로 등록된) io_u 개수.
	 * 설정자: fio_ime_queue_incr()가 증가, fio_ime_queue_red()가 감소, queue_reset()이 0으로.
	 * 읽는 자: queue()가 depth와 비교해 FIO_Q_BUSY 결정, commit()이 제출량 결정. */
	/* [한국어 원주석] iovecs/io_us in the queue */

	unsigned int 	events;
	/* [한국어] "커밋은 되었으나 아직 get_events로 수확되지 않은" io_u 개수.
	 * 설정자: commit()이 queue_commit()으로 증가, getevents가 queue_red/reset으로 감소.
	 * 의미: psyncv에서는 events>0이면 get_events 전까지 새 queue 금지(can_queue 검사). */
	/* [한국어 원주석] number of committed iovecs/io_us */

	/* variables used to implement a "ring" queue */
	unsigned int depth;
	/* [한국어] 링 버퍼 용량 = td->o.iodepth. init()에서 설정되고 이후 불변. */
	/* [한국어 원주석] max entries in the queue */

	unsigned int head;
	/* [한국어] 다음 enqueue 위치(write index). queue_incr에서 (head+1)%depth로 전진.
	 *          aio에서 can_append의 첫 조건이 head!=0 - 배열 경계에서는 새 요청을 만들도록 강제. */
	/* [한국어 원주석] index used to append */

	unsigned int tail;
	/* [한국어] 다음 pop 위치(read index). getevents가 완료된 iovec을 소비하면서 전진. */
	/* [한국어 원주석] index used to pop */

	unsigned int cur_commit;
	/* [한국어] 아직 commit되지 않은 첫 요청의 인덱스(aio 전용). queue_commit에서 iovcnt만큼 전진.
	 *          (queued - events) > 0이면 커밋할 요청이 남아 있음. */
	/* [한국어 원주석] index of the first uncommitted req */

	/* offset used by the last iovec (used to check if the iovecs can be appended)*/
	unsigned long long	last_offset;
	/* [한국어] 가장 최근 enqueue된 iovec의 "끝 오프셋"(offset + xfer_buflen).
	 * 사용 목적: 새 io_u가 이 위치와 정확히 인접해야만 배치에 append 가능(연속 I/O 조건).
	 * 설정자: psyncv_enqueue/aio_enqueue가 매번 갱신. */

	/* The variables below are used for aio only */
	struct imeaio_req	*last_req;
	/* [한국어] aio 전용: 현재 "확장 가능한" 마지막 요청(동일 fd/ddir/연속 오프셋의 배치).
	 * 설정자: enqueue에서 새 요청을 만들 때 자신을 가리키도록 갱신.
	 * 읽는 자: can_append()와 enqueue 내부에서 iovcnt 증가. */
	/* [한국어 원주석] last request awaiting committing */
};


/**************************************************************
 *         Private functions for queueing/unqueueing
 *
 **************************************************************/

/*
 * [한국어]
 * fio_ime_queue_incr - 링 큐에 하나를 추가했을 때 head/queued를 전진.
 *
 * @ime_d: 엔진별 상태(psyncv/aio 공용).
 *
 * enqueue 헬퍼. 호출 측은 "새 iovec/io_us[head]에 데이터를 이미 기록한" 상태.
 * 실행 컨텍스트: 잡 스레드(queue 콜백) 단독.
 * 호출 체인: fio_ime_psyncv_enqueue / fio_ime_aio_enqueue → [이 함수].
 */
static void fio_ime_queue_incr (struct ime_data *ime_d)
{
	ime_d->head = (ime_d->head + 1) % ime_d->depth;  /* [한국어] 링 버퍼 특성상 depth-1 다음은 0으로 순환. */
	ime_d->queued++;                                 /* [한국어] 큐 적재량 증가 - queue()의 depth 비교에 사용. */
}

/*
 * [한국어]
 * fio_ime_queue_red - 하나의 iovec이 완료되어 fio에 반환되었을 때 tail 전진 및 카운터 감소.
 *
 * @ime_d: 엔진별 상태.
 *
 * getevents의 완료 처리 내부에서 호출. queued와 events 양쪽을 동시에 감소시키는 이유는
 * "커밋된 요청"이 소비되면 큐에서도 빠져야 하기 때문.
 * 호출 체인: fio_ime_aio_getevents → [이 함수].
 */
static void fio_ime_queue_red (struct ime_data *ime_d)
{
	ime_d->tail = (ime_d->tail + 1) % ime_d->depth;  /* [한국어] 읽기 포인터 순환 전진. */
	ime_d->queued--;                                 /* [한국어] 큐에서 제거되었으므로 적재량 감소. */
	ime_d->events--;                                 /* [한국어] 커밋되어 반환된 항목이므로 완료 대기량도 감소. */
}

/*
 * [한국어]
 * fio_ime_queue_commit - aio의 한 요청이 IME에 제출되었을 때 cur_commit/events를 그 요청의
 *                        iovcnt만큼 전진.
 *
 * @iovcnt: 이 요청에 묶여 있던 iovec 개수(1 이상).
 *
 * commit 루프에서 매 요청 제출 후 호출.
 * 호출 체인: fio_ime_aio_commit → [이 함수].
 */
static void fio_ime_queue_commit (struct ime_data *ime_d, int iovcnt)
{
	ime_d->cur_commit = (ime_d->cur_commit + iovcnt) % ime_d->depth;  /* [한국어] 다음 미커밋 요청의 인덱스. */
	ime_d->events += iovcnt;                                          /* [한국어] 커밋되었지만 아직 수확되지 않은 항목 수 증가. */
}

/*
 * [한국어]
 * fio_ime_queue_reset - psyncv에서 한 배치가 완전히 소비된 뒤 모든 포인터를 0으로 리셋.
 *
 * psyncv는 매번 "append 시작 인덱스 = 0"을 가정하므로 getevents 종료 시점에 전체를 리셋.
 * aio와 달리 psyncv는 링 구조가 아닌 "한 배치 단일 윈도우"로 동작함에 유의.
 * 호출 체인: fio_ime_psyncv_getevents → [이 함수].
 */
static void fio_ime_queue_reset (struct ime_data *ime_d)
{
	ime_d->head = 0;         /* [한국어] 다음 enqueue는 배열 시작부터. */
	ime_d->tail = 0;         /* [한국어] pop 인덱스도 초기화. */
	ime_d->cur_commit = 0;   /* [한국어] 커밋 인덱스 초기화(psyncv는 사실상 사용 안 함). */
	ime_d->queued = 0;       /* [한국어] 큐에 남은 항목 없음. */
	ime_d->events = 0;       /* [한국어] 미수확 완료 없음. */
}

/**************************************************************
 *                   General IME functions
 *             (needed for both sync and async IOs)
 **************************************************************/

/*
 * [한국어]
 * fio_set_ime_filename - fio가 넘겨준 일반 파일 경로 앞에 IME 전용 prefix를 붙여
 *                         IME 네이티브 API가 인식할 수 있는 경로로 변환한다.
 *
 * @filename: fio가 보유한 원본 파일명(예: "/mnt/test/foo").
 * @return:   성공 시 thread-local 버퍼 포인터("ime://"+원본 등), 너무 길면 NULL.
 *
 * IME 경로 규약상 DEFAULT_IME_FILE_PREFIX(ime_native.h)가 앞에 있어야 한다. 반환 버퍼는
 * __thread 저장소에 있으므로 같은 스레드 내에서 다음 호출 전까지만 유효.
 * 실행 컨텍스트: 잡 스레드. 재진입 불가(같은 스레드 내 연속 호출은 이전 값 덮어씀).
 * 호출 체인: fio_ime_open_file / fio_ime_get_file_size / fio_ime_unlink_file → [이 함수].
 */
static char *fio_set_ime_filename(char* filename)
{
	static __thread char ime_filename[PATH_MAX];  /* [한국어] 스레드별 전용 버퍼 - 반환 포인터 유효 기간은 "다음 호출 전까지". */
	int ret;                                      /* [한국어] snprintf의 "필요 길이" 반환값을 받아 절단 여부 검사. */

	ret = snprintf(ime_filename, PATH_MAX, "%s%s", DEFAULT_IME_FILE_PREFIX, filename);  /* [한국어] prefix+원본 파일명 조합. snprintf는 버퍼 초과 시에도 "필요했을 길이"를 반환. */
	if (ret < PATH_MAX)                            /* [한국어] 버퍼 내에 완전히 들어간 경우에만 유효 - PATH_MAX 이상이면 절단된 것. */
		return ime_filename;                       /* [한국어] 성공: thread-local 포인터 반환. */

	return NULL;                                  /* [한국어] 경로가 너무 길어 생성 실패 - 호출자는 에러로 처리. */
}

/*
 * [한국어]
 * fio_ime_get_file_size - ioengine_ops.get_file_size 콜백. IME 상의 파일 크기를 질의하여
 *                          fio_file->real_file_size를 채운다.
 *
 * @td: 잡 스레드 컨텍스트(에러 보고용).
 * @f:  대상 파일 디스크립터 래퍼.
 * @return: 성공 0, 실패 1.
 *
 * fio 코어는 파일 레이아웃/verify 등에 실제 크기를 요구한다. POSIX stat이 아닌
 * ime_native_stat을 호출해야 IME 상의 메타데이터를 얻을 수 있다.
 * 호출 체인: fio backend → [이 함수] → fio_set_ime_filename → ime_native_stat.
 * 에러 경로: prefix 생성 실패 또는 ime_native_stat 실패 시 td_verror로 errno 전파.
 */
static int fio_ime_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct stat buf;   /* [한국어] ime_native_stat의 출력 버퍼(POSIX stat 레이아웃과 호환). */
	int ret;           /* [한국어] ime_native_stat 반환값(-1: 에러). */
	char *ime_filename;/* [한국어] prefix가 붙은 IME 경로(thread-local 버퍼 포인터). */

	dprint(FD_FILE, "get file size %s\n", f->file_name);  /* [한국어] FD_FILE 디버그 채널에 추적 로그. */

	ime_filename = fio_set_ime_filename(f->file_name);  /* [한국어] "ime://" prefix 부여하여 IME API용 경로 생성. */
	if (ime_filename == NULL)                            /* [한국어] 경로가 PATH_MAX 초과로 생성 불가. */
		return 1;                                         /* [한국어] fio 측에 실패 반환 - 잡이 에러로 종료. */
	ret = ime_native_stat(ime_filename, &buf);          /* [한국어] IME 네이티브 stat - 메타데이터 조회(네트워크 RPC 가능성). */
	if (ret == -1) {                                     /* [한국어] 파일 없음/권한 없음 등. errno가 설정됨. */
		td_verror(td, errno, "fstat");                  /* [한국어] fio 에러 기록 - 파일명 컨텍스트 "fstat"로 저장. */
		return 1;                                        /* [한국어] 실패 반환. */
	}

	f->real_file_size = buf.st_size;                    /* [한국어] fio가 사용할 실제 파일 크기 기록 - 이후 layout/verify가 참조. */
	return 0;                                           /* [한국어] 성공. */
}

/* This functions mimics the generic_file_open function, but issues
   IME native calls instead of POSIX calls. */
/*
 * [한국어]
 * fio_ime_open_file - ioengine_ops.open_file 콜백. fio의 generic_file_open을 IME API 버전으로 재구현.
 *
 * @td: 잡 스레드(옵션/에러 보고).
 * @f:  열어야 할 파일 기술자(fd는 여기서 채움).
 * @return: 성공 0, 실패 1.
 *
 * POSIX open() 대신 ime_native_open()을 쓰도록 플래그 조합을 수행하고, TRIM은 IME가
 * 지원하지 않으므로 거부한다. 또한 fio 코어가 POSIX stat으로 크기를 얻지 않도록
 * 여기서 직접 get_file_size와 ftruncate를 호출해 파일 크기를 맞춘다.
 * 실행 컨텍스트: 잡 스레드(각 파일당 1회). 호출 체인: backend → td_io_open_file → [이 함수].
 * 에러 경로: TRIM 요청/지원 불가 조합/open 실패/stat 실패/ftruncate 실패 모두 td_verror 후 1 반환.
 */
static int fio_ime_open_file(struct thread_data *td, struct fio_file *f)
{
	int flags = 0;         /* [한국어] ime_native_open 두 번째 인자로 전달할 열기 플래그 집합. 0부터 OR로 조립. */
	int ret;               /* [한국어] 내부 호출 반환값 저장. */
	uint64_t desired_fs;   /* [한국어] 이 잡이 요구하는 최소 파일 크기 = io_size + file_offset. */
	char *ime_filename;    /* [한국어] IME prefix가 붙은 경로 문자열. */

	dprint(FD_FILE, "fd open %s\n", f->file_name);  /* [한국어] FD_FILE 채널에 열기 시도 로그. */

	if (td_trim(td)) {                                  /* [한국어] 이 잡이 TRIM(discard)을 포함하는지 검사 - IME는 미지원. */
		td_verror(td, EINVAL, "IME does not support TRIM operation");  /* [한국어] 사용자에게 명확한 오류 메시지로 보고. */
		return 1;                                        /* [한국어] 즉시 실패 반환 - 파일 오픈 포기. */
	}

	if (td->o.odirect)                                  /* [한국어] --direct=1 요청 시 O_DIRECT 부여 - IME는 페이지 캐시 우회 경로. */
		flags |= O_DIRECT;
	flags |= td->o.sync_io;                             /* [한국어] O_SYNC 등 sync_io 옵션 비트를 그대로 OR - 동기 쓰기 보장. */
	if (td->o.create_on_open && td->o.allow_create)     /* [한국어] "오픈 시 생성" + "생성 허용" 동시에 true면 O_CREAT. */
		flags |= O_CREAT;

	if (td_write(td)) {                                 /* [한국어] 잡이 쓰기(verify 포함) 방향인 경우. */
		if (!read_only)                                  /* [한국어] 전역 read_only 플래그가 꺼져 있어야 실제 쓰기 가능. */
			flags |= O_RDWR;                              /* [한국어] 읽기/쓰기 모두 - verify read-back을 위해 RW 필요. */

		if (td->o.allow_create)                          /* [한국어] 부재 파일을 만들 수 있는 옵션이면 O_CREAT 보강. */
			flags |= O_CREAT;
	} else if (td_read(td)) {                           /* [한국어] 순수 읽기 잡. */
		flags |= O_RDONLY;                               /* [한국어] 읽기 전용 열기 - 데이터 변경 방지. */
	} else {
		/* We should never go here. */
		/* [한국어] read/write/trim 어디에도 속하지 않는 비정상 조합. 보통 옵션 파서가 막지만 방어적 처리. */
		td_verror(td, EINVAL, "Unsopported open mode");  /* [한국어] 잡 종료시킬 수준의 치명적 오류로 기록. */
		return 1;
	}

	ime_filename = fio_set_ime_filename(f->file_name);  /* [한국어] IME용 경로로 변환. */
	if (ime_filename == NULL)                            /* [한국어] PATH_MAX 초과 등으로 경로 생성 불가. */
		return 1;
	f->fd = ime_native_open(ime_filename, flags, 0600); /* [한국어] IME 네이티브 open - mode 0600: rw- 소유자만. RPC 가능성 있음. */
	if (f->fd == -1) {                                   /* [한국어] open 실패(접근/부재/생성 불가 등). */
		char buf[FIO_VERROR_SIZE];                        /* [한국어] fio 오류 메시지 포맷용 지역 버퍼. */
		int __e = errno;                                  /* [한국어] 후속 호출이 errno를 덮기 전에 스냅샷. */

		snprintf(buf, sizeof(buf), "open(%s)", f->file_name);  /* [한국어] "open(<파일명>)" 형태로 컨텍스트 기록. */
		td_verror(td, __e, buf);                          /* [한국어] fio에 에러 등록 - 잡 상태 전이. */
		return 1;
	}

	/* Now we need to make sure the real file size is sufficient for FIO
	   to do its things. This is normally done before the file open function
	   is called, but because FIO would use POSIX calls, we need to do it
	   ourselves */
	/* [한국어] fio 코어가 POSIX stat을 쓰지 못하므로 여기서 직접 IME stat으로 크기 획득. */
	ret = fio_ime_get_file_size(td, f);
	if (ret < 0) {                                       /* [한국어] 음수 반환은 내부적으로 에러 시그널(본 함수는 1을 돌려줌에 주의). */
		ime_native_close(f->fd);                          /* [한국어] 리소스 누수 방지 - 방금 연 fd 닫기. */
		td_verror(td, errno, "ime_get_file_size");       /* [한국어] errno 전파. */
		return 1;
	}

	desired_fs = f->io_size + f->file_offset;           /* [한국어] 잡 설정이 요구하는 최소 파일 크기 계산. */
	if (td_write(td)) {                                 /* [한국어] 쓰기 잡: 필요 시 파일을 확장(layout). */
		dprint(FD_FILE, "Laying out file %s%s\n",
			DEFAULT_IME_FILE_PREFIX, f->file_name);      /* [한국어] 레이아웃 단계 추적 로그. */
		if (!td->o.create_on_open &&                     /* [한국어] "오픈 시 생성" 옵션이 없을 때만 사전 확장 필요. */
				f->real_file_size < desired_fs &&         /* [한국어] 현재 크기가 요구 크기보다 작을 때만. */
				ime_native_ftruncate(f->fd, desired_fs) < 0) {  /* [한국어] IME 파일 크기를 확장 - 실패 시 음수. */
			ime_native_close(f->fd);                      /* [한국어] 확장 실패 시 fd 정리. */
			td_verror(td, errno, "ime_native_ftruncate");
			return 1;
		}
		if (f->real_file_size < desired_fs)              /* [한국어] truncate가 성공했다면 캐시된 크기를 갱신. */
			f->real_file_size = desired_fs;
	} else if (td_read(td) && f->real_file_size < desired_fs) {  /* [한국어] 읽기 잡인데 파일이 너무 작음 - 확장 불가능한 상황. */
		ime_native_close(f->fd);                          /* [한국어] 리소스 정리. */
		log_err("error: can't read %lu bytes from file with "
						"%lu bytes\n", desired_fs, f->real_file_size);  /* [한국어] 사용자에게 친절한 메시지 출력. */
		return 1;
	}

	return 0;                                           /* [한국어] 모든 준비 완료 - fd는 f->fd에 저장됨. */
}

/*
 * [한국어]
 * fio_ime_close_file - ioengine_ops.close_file 콜백. open_file에서 얻은 fd를 IME에 반환.
 *
 * @td: unused(fio_unused 매크로로 표식).
 * @f:  닫을 파일.
 * @return: 성공 0, 실패 errno.
 *
 * 호출 체인: backend → td_io_close_file → [이 함수] → ime_native_close.
 */
static int fio_ime_close_file(struct thread_data fio_unused *td, struct fio_file *f)
{
	int ret = 0;  /* [한국어] 기본 성공. 닫기 실패 시에만 errno로 덮어씀. */

	dprint(FD_FILE, "fd close %s\n", f->file_name);  /* [한국어] 디버그 추적 로그. */

	if (ime_native_close(f->fd) < 0)                  /* [한국어] IME 네이티브 close - 리소스/연결 해제(RPC 가능성). */
		ret = errno;                                   /* [한국어] 실패 시 errno 스냅샷하여 반환. */

	f->fd = -1;                                       /* [한국어] fd 무효화 - 재사용 시 잘못된 핸들 사용 방지. */
	return ret;
}

/*
 * [한국어]
 * fio_ime_unlink_file - ioengine_ops.unlink_file 콜백. IME prefix 경로를 POSIX unlink로 제거.
 *
 * 이 함수는 IME 네이티브 unlink가 아닌 POSIX unlink를 호출한다 - IME가 POSIX 마운트 지점에
 * 경로를 노출하므로 파일 제거는 OS 레이어에서 처리 가능함을 전제로 한다.
 * 호출 체인: fio cleanup → td_io_unlink_file → [이 함수].
 */
static int fio_ime_unlink_file(struct thread_data *td, struct fio_file *f)
{
	char *ime_filename = fio_set_ime_filename(f->file_name);  /* [한국어] IME prefix 경로 확보. */
	int ret;

	if (ime_filename == NULL)                                  /* [한국어] 경로 생성 실패. */
		return 1;

	ret = unlink(ime_filename);                                /* [한국어] POSIX unlink - 디렉토리 엔트리 제거. */
	return ret < 0 ? errno : 0;                                /* [한국어] 실패 시 errno, 성공 시 0. */
}

/*
 * [한국어]
 * fio_ime_event - ioengine_ops.event 콜백. getevents 이후 fio가 "n번째 완료 io_u"를 질의할 때 반환.
 *
 * @event: 0..(getevents 반환값-1) 범위의 인덱스.
 * @return: event_io_us[event]로 저장해둔 io_u 포인터.
 *
 * 두 비동기 엔진(psyncv/aio)이 모두 같은 구현을 쓰므로 공용화.
 */
static struct io_u *fio_ime_event(struct thread_data *td, int event)
{
	struct ime_data *ime_d = td->io_ops_data;  /* [한국어] 잡별 엔진 상태를 꺼냄. */

	return ime_d->event_io_us[event];          /* [한국어] getevents 루프가 채워둔 완료 io_u 배열에서 인덱스 참조. */
}

/* Setup file used to replace get_file_sizes when settin up the file.
   Instead we will set real_file_sie to 0 for each file. This way we
   can avoid calling ime_native_init before the forks are created. */
/*
 * [한국어]
 * fio_ime_setup - ioengine_ops.setup 콜백. ime_native_init()을 fork 이후로 늦추기 위한 트릭.
 *
 * fio 코어는 setup에서 get_file_size를 호출해 파일 크기를 얻으려 하지만, IME 라이브러리는
 * fork 전에 초기화되면 자식에서 문제를 일으킨다. 따라서 real_file_size를 0으로 세팅하여
 * "크기 미지정 상태"로 남기고, 실제 크기 획득은 open_file 시점으로 지연한다.
 * 호출 체인: fio_backend → td_io_setup → [이 함수].
 */
static int fio_ime_setup(struct thread_data *td)
{
	struct fio_file *f;   /* [한국어] 반복 포인터. */
	unsigned int i;       /* [한국어] for_each_file 매크로용 인덱스. */

	for_each_file(td, f, i) {  /* [한국어] td에 등록된 모든 파일을 순회. */
		dprint(FD_FILE, "setup: set file size to 0 for %p/%d/%s\n",
			f, i, f->file_name);       /* [한국어] 디버그 추적 - 지연 초기화 의도를 로그로 남김. */
		f->real_file_size = 0;          /* [한국어] 크기 0 = "아직 모름" - fio가 이후 단계에서 다시 질의하도록 유도. */
	}

	return 0;                          /* [한국어] setup 단계는 항상 성공. */
}

/*
 * [한국어]
 * fio_ime_engine_init - ioengine_ops.init 콜백. IME 라이브러리 초기화 및 파일 크기 임시 세팅.
 *
 * @td: 잡 스레드.
 * @return: 항상 0 (IME init은 본 함수에서 실패 반환을 하지 않음 - 실패 시 라이브러리가 abort).
 *
 * use_thread=0 모드(즉 fork 모드)에서 잡 스레드 생성 전에 init이 이미 true이면 순서 오류
 * 경고만 출력한다. use_thread=1 모드에서는 여러 잡이 하나의 프로세스를 공유하므로
 * IME 초기화를 한 번만 수행해도 OK.
 * 호출 체인: backend → td_io_init → [이 함수].
 */
static int fio_ime_engine_init(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_IO, "ime engine init\n");                  /* [한국어] FD_IO 채널 추적. */
	if (fio_ime_is_initialized && !td->o.use_thread) {   /* [한국어] fork 모드인데 이미 초기화되었다면 순서 이상 - 경고. */
		log_err("Warning: something might go wrong. Not all threads/forks were"
				" created before the FIO jobs were initialized.\n");
	}

	ime_native_init();                                    /* [한국어] IME 클라이언트 초기화 - 내부 스레드/연결 준비. */
	fio_ime_is_initialized = true;                        /* [한국어] 전역 플래그 세팅 - unregister에서 finalize 판단에 사용. */

	/* We have to temporarily set real_file_size so that
	   FIO can initialize properly. It will be corrected
	   on file open. */
	/* [한국어] init 단계에서 fio가 real_file_size를 참조할 수 있도록 임시 값 채움 - open에서 정확한 값으로 덮어씀. */
	for_each_file(td, f, i)
		f->real_file_size = f->io_size + f->file_offset;  /* [한국어] 최소 필요 크기로 세팅 - fio의 크기 관련 검증 통과용. */

	return 0;
}

/*
 * [한국어]
 * fio_ime_engine_finalize - ioengine_ops.cleanup 콜백(또는 공통 finalize 헬퍼).
 *
 * fork 모드(각 잡이 독립 프로세스)에서는 잡 종료 시 ime_native_finalize가 안전하다.
 * thread 모드에서는 다른 잡이 아직 IME를 사용 중일 수 있으므로 여기서 finalize하지 않고
 * fio_ime_unregister(프로세스 종료 시)에서 처리.
 */
static void fio_ime_engine_finalize(struct thread_data *td)
{
	/* Only finalize IME when using forks */
	if (!td->o.use_thread) {                              /* [한국어] fork 모드 판단 - 각 프로세스가 자기 IME를 가짐. */
		if (ime_native_finalize() < 0)                     /* [한국어] IME 종료 - 연결/스레드 해제. */
			log_err("error in ime_native_finalize\n");     /* [한국어] 실패해도 프로세스는 계속 종료 진행. */
		fio_ime_is_initialized = false;                    /* [한국어] 전역 플래그 리셋. */
	}
}


/**************************************************************
 *             Private functions for blocking IOs
 *                     (without iovecs)
 **************************************************************/

/* Notice: this function comes from the sync engine */
/* It is used by the commit function to return a proper code and fill
   some attributes in the io_u used for the IO. */
/*
 * [한국어]
 * fio_ime_psync_end - ime_native_p{read,write}/fsync 결과를 io_u에 반영하고 완료 상태 반환.
 *
 * @ret: 시스템 호출 반환값(성공 시 바이트 수, 부분 전송 시 작은 수, 실패 시 -1).
 * @return: 항상 FIO_Q_COMPLETED (psync는 동기 엔진).
 *
 * 부분 전송(short I/O)은 io_u->resid에 미전송 바이트를 기록하고 error=0으로 둔다.
 * 완전 실패는 io_u->error에 errno를 기록하고 td_verror로 fio에 보고.
 */
static int fio_ime_psync_end(struct thread_data *td, struct io_u *io_u, ssize_t ret)
{
	if (ret != (ssize_t) io_u->xfer_buflen) {           /* [한국어] 요청 바이트 전부가 전송되지 않았음. */
		if (ret >= 0) {                                   /* [한국어] 0 이상: 부분 전송(short I/O) - fio는 이를 에러가 아닌 resid로 처리. */
			io_u->resid = io_u->xfer_buflen - ret;         /* [한국어] 남은 바이트 = 요청 - 실제. */
			io_u->error = 0;                               /* [한국어] 부분 성공이므로 에러 없음. */
			return FIO_Q_COMPLETED;                        /* [한국어] 완료로 반환 - fio가 남은 부분 재시도할 수 있음. */
		} else
			io_u->error = errno;                           /* [한국어] -1: 시스템 호출 실패 - errno 기록. */
	}

	if (io_u->error) {                                   /* [한국어] 에러가 있으면 로그에 남기고 td 상태에도 반영. */
		io_u_log_error(td, io_u);                         /* [한국어] io_u 컨텍스트(오프셋/버퍼/방향)를 로그에 출력. */
		td_verror(td, io_u->error, "xfer");               /* [한국어] 잡에 치명 에러로 보고 - "xfer" 태그. */
	}

	return FIO_Q_COMPLETED;                              /* [한국어] psync는 반환 시 항상 완료 - fio는 별도 getevents 불필요. */
}

/*
 * [한국어]
 * fio_ime_psync_queue - ime_psync 엔진의 queue 콜백. 동기적으로 I/O를 수행하고 즉시 완료 반환.
 *
 * @io_u: 제출할 I/O 유닛(버퍼/오프셋/방향 포함).
 * @return: FIO_Q_COMPLETED - 이 엔진은 비동기 큐잉 없음.
 *
 * ddir에 따라 ime_native_pread/pwrite/fsync를 직접 호출. 그 외 방향(TRIM 등)은 EINVAL.
 * 실행 컨텍스트: 잡 스레드 동기. 호출 체인: backend → td_io_queue → [이 함수].
 */
static enum fio_q_status fio_ime_psync_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	struct fio_file *f = io_u->file;  /* [한국어] 대상 파일(오픈된 IME fd 포함). */
	ssize_t ret;                       /* [한국어] 전송 바이트 또는 -1. */

	fio_ro_check(td, io_u);            /* [한국어] read-only 잡이면 쓰기 시도를 assert로 잡음 - 방어적 점검. */

	if (io_u->ddir == DDIR_READ)
		ret = ime_native_pread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);    /* [한국어] IME pread - 주어진 오프셋에서 xfer_buflen 바이트 읽기. */
	else if (io_u->ddir == DDIR_WRITE)
		ret = ime_native_pwrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);   /* [한국어] IME pwrite - 주어진 오프셋에 쓰기. */
	else if (io_u->ddir == DDIR_SYNC)
		ret = ime_native_fsync(f->fd);                                                      /* [한국어] IME fsync - 캐시 플러시. */
	else {
		ret = io_u->xfer_buflen;                                                            /* [한국어] 알 수 없는 방향 - ret을 "전체 전송"으로 세팅해서 resid 0이 되도록. */
		io_u->error = EINVAL;                                                               /* [한국어] 그러나 에러 플래그는 명시. */
	}

	return fio_ime_psync_end(td, io_u, ret);                                              /* [한국어] 결과를 io_u에 반영하고 상태 반환. */
}


/**************************************************************
 *             Private functions for blocking IOs
 *                       (with iovecs)
 **************************************************************/

/*
 * [한국어]
 * fio_ime_psyncv_can_queue - psyncv 엔진에서 새 io_u를 현재 iovec 배치에 이어붙일 수 있는지 판정.
 *
 * psyncv는 하나의 preadv/pwritev 호출로 여러 iovec을 동시에 처리하므로, 배치는 동일 fd,
 * 동일 방향, 파일상 연속 오프셋을 요구한다. 또한 이미 커밋되어 getevents 대기 중인
 * 이벤트가 남아 있으면 새 배치를 시작할 수 없다.
 *
 * @return: true면 enqueue 허용, false면 FIO_Q_BUSY로 되돌려 잡이 commit/getevents 후 재시도.
 */
static bool fio_ime_psyncv_can_queue(struct ime_data *ime_d, struct io_u *io_u)
{
	/* We can only queue if:
	  - There are no queued iovecs
	  - Or if there is at least one:
		 - There must be no event waiting for retrieval
		 - The offsets must be contiguous
		 - The ddir and fd must be the same */
	return (ime_d->queued == 0 || (                     /* [한국어] 빈 큐면 언제나 OK. */
			ime_d->events == 0 &&                        /* [한국어] 커밋된 미수확 이벤트가 없어야 함. */
			ime_d->last_offset == io_u->offset &&        /* [한국어] 이전 iovec의 끝과 새 io_u 시작이 정확히 맞닿아야 함. */
			ime_d->sioreq->ddir == io_u->ddir &&         /* [한국어] 동일 방향 - preadv/pwritev는 한 방향만 처리. */
			ime_d->sioreq->fd == io_u->file->fd));       /* [한국어] 동일 fd에 대한 배치여야 함. */
}

/* Before using this function, we should have already
   ensured that the queue is not full */
/*
 * [한국어]
 * fio_ime_psyncv_enqueue - io_u를 psyncv 배치의 현재 head 위치에 iovec으로 기록.
 *
 * 사전조건: queued < depth 이고 can_queue()가 true. 큐가 비어 있으면 요청 메타(ioreq)를
 * 새로 초기화하고, 아니면 기존 메타에 이어붙인다.
 */
static void fio_ime_psyncv_enqueue(struct ime_data *ime_d, struct io_u *io_u)
{
	struct imesio_req *ioreq = ime_d->sioreq;        /* [한국어] psyncv의 유일한 요청 기술자. */
	struct iovec *iov = &ime_d->iovecs[ime_d->head]; /* [한국어] 새 iovec이 들어갈 슬롯. */

	iov->iov_base = io_u->xfer_buf;                  /* [한국어] 사용자 버퍼 주소. preadv/pwritev가 DMA 대상으로 사용. */
	iov->iov_len = io_u->xfer_buflen;                /* [한국어] 전송할 바이트 수. */

	if (ime_d->queued == 0) {                        /* [한국어] 배치의 첫 번째 iovec - 메타데이터 초기화. */
		ioreq->offset = io_u->offset;                 /* [한국어] 배치 시작 오프셋 기록. */
		ioreq->ddir = io_u->ddir;                     /* [한국어] 방향 확정. */
		ioreq->fd = io_u->file->fd;                   /* [한국어] 대상 fd. */
	}

	ime_d->io_us[ime_d->head] = io_u;                /* [한국어] iovec과 1:1로 대응되는 io_u 저장 - getevents에서 재활용. */
	ime_d->last_offset = io_u->offset + io_u->xfer_buflen;  /* [한국어] 다음 enqueue가 연속성 검사할 끝 오프셋 갱신. */
	fio_ime_queue_incr(ime_d);                       /* [한국어] head/queued 전진. */
}

/* Tries to queue an IO. It will fail if the IO can't be appended to the
   current request or if the current request has been committed but not
   yet retrieved by get_events. */
/*
 * [한국어]
 * fio_ime_psyncv_queue - ime_psyncv queue 콜백. iovec 누적 후 commit에서 단일 preadv/pwritev로 플러시.
 *
 * @return: FIO_Q_QUEUED(배치에 누적됨), FIO_Q_BUSY(큐가 꽉 찼거나 연속성 위반),
 *           FIO_Q_COMPLETED(fsync나 에러로 즉시 완료).
 *
 * 호출 체인: backend → td_io_queue → [이 함수]. fsync는 psyncv 배치를 거치지 않고 즉시 처리.
 */
static enum fio_q_status fio_ime_psyncv_queue(struct thread_data *td,
	struct io_u *io_u)
{
	struct ime_data *ime_d = td->io_ops_data;  /* [한국어] 엔진 상태 꺼냄. */

	fio_ro_check(td, io_u);                    /* [한국어] read-only 잡 보호. */

	if (ime_d->queued == ime_d->depth)         /* [한국어] 큐가 가득 - fio에 커밋/수확을 먼저 하라고 요청. */
		return FIO_Q_BUSY;

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {  /* [한국어] 일반 read/write 경로. */
		if (!fio_ime_psyncv_can_queue(ime_d, io_u))              /* [한국어] 연속성/동일 fd/동일 방향/미수확 이벤트 없음 확인. */
			return FIO_Q_BUSY;                                     /* [한국어] 배치 불가 - fio가 먼저 commit. */

		dprint(FD_IO, "queue: ddir=%d at %u commit=%u queued=%u events=%u\n",
			io_u->ddir, ime_d->head, ime_d->cur_commit,
			ime_d->queued, ime_d->events);                        /* [한국어] 큐 상태 디버그. */
		fio_ime_psyncv_enqueue(ime_d, io_u);                     /* [한국어] iovec 슬롯에 기록 + head 전진. */
		return FIO_Q_QUEUED;                                      /* [한국어] 비동기 성공 - 완료는 getevents에서. */
	} else if (io_u->ddir == DDIR_SYNC) {                       /* [한국어] fsync는 배치 대상이 아니므로 즉시 실행. */
		if (ime_native_fsync(io_u->file->fd) < 0) {              /* [한국어] IME fsync - 버스트 버퍼 캐시 플러시. */
			io_u->error = errno;                                   /* [한국어] 실패 시 errno 기록. */
			td_verror(td, io_u->error, "fsync");
		}
		return FIO_Q_COMPLETED;                                   /* [한국어] 동기 완료. */
	} else {
		io_u->error = EINVAL;                                     /* [한국어] TRIM 등 미지원 방향. */
		td_verror(td, io_u->error, "wrong ddir");
		return FIO_Q_COMPLETED;
	}
}

/* Notice: this function comes from the sync engine */
/* It is used by the commit function to return a proper code and fill
   some attributes in the io_us appended to the current request. */
/*
 * [한국어]
 * fio_ime_psyncv_end - preadv/pwritev 반환값을 배치에 속한 각 io_u에 분배.
 *
 * @bytes: preadv/pwritev 반환값(총 전송 바이트, 실패 시 -1).
 *
 * preadv/pwritev는 iovec 순서대로 채우므로 bytes를 iovec 크기 순으로 소진시킨다.
 * 실패 시 모든 io_u에 errno 기록.
 */
static int fio_ime_psyncv_end(struct thread_data *td, ssize_t bytes)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct io_u *io_u;
	unsigned int i;
	int err = errno;   /* [한국어] 후속 로깅이 errno를 덮기 전에 보존. */

	for (i = 0; i < ime_d->queued; i++) {  /* [한국어] 배치에 포함된 모든 io_u 순회. */
		io_u = ime_d->io_us[i];

		if (bytes == -1)                     /* [한국어] 전체 실패 - 모든 io_u에 같은 errno 기록. */
			io_u->error = err;
		else {
			unsigned int this_io;              /* [한국어] 이 io_u에 할당된 전송량. */

			this_io = bytes;                   /* [한국어] 남은 bytes를 가용 상한으로. */
			if (this_io > io_u->xfer_buflen)  /* [한국어] io_u 한도를 넘지 않도록 clamp. */
				this_io = io_u->xfer_buflen;

			io_u->resid = io_u->xfer_buflen - this_io;  /* [한국어] 미전송 바이트. */
			io_u->error = 0;                             /* [한국어] 부분 성공 - 에러 아님. */
			bytes -= this_io;                            /* [한국어] 남은 전체 바이트에서 소진. */
		}
	}

	if (bytes == -1) {                   /* [한국어] 전체 실패 경로 - fio에 보고. */
		td_verror(td, err, "xfer psyncv");
		return -err;                      /* [한국어] 음수 errno를 반환(commit 호출자가 에러로 판정). */
	}

	return 0;                            /* [한국어] 성공 또는 부분 성공 모두 0. */
}

/* Commits the current request by calling ime_native (with one or several
   iovecs). After this commit, the corresponding events (one per iovec)
   can be retrieved by get_events. */
/*
 * [한국어]
 * fio_ime_psyncv_commit - 누적된 iovec 배치를 단일 preadv/pwritev로 IME에 제출.
 *
 * 이 엔진은 동기 엔진이므로 commit 반환 시점에 모든 전송이 끝나 있다. getevents는
 * 단지 io_us → event_io_us로 복사만 한다.
 * 호출 체인: backend → td_io_commit → [이 함수].
 */
static int fio_ime_psyncv_commit(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imesio_req *ioreq;
	int ret = 0;

	/* Exit if there are no (new) events to commit
	   or if the previous committed event haven't been retrieved */
	if (!ime_d->queued || ime_d->events)  /* [한국어] 누적된 iovec이 없거나, 직전 배치가 아직 수확되지 않았으면 아무것도 안 함. */
		return 0;

	ioreq = ime_d->sioreq;                 /* [한국어] 유일 요청 기술자(fd/ddir/offset 포함). */
	ime_d->events = ime_d->queued;         /* [한국어] queued 전체를 "커밋됨" 상태로 이동 - getevents가 이만큼 돌려줌. */
	if (ioreq->ddir == DDIR_READ)
		ret = ime_native_preadv(ioreq->fd, ime_d->iovecs, ime_d->queued, ioreq->offset);   /* [한국어] 읽기 배치 제출 - 동기 호출. */
	else
		ret = ime_native_pwritev(ioreq->fd, ime_d->iovecs, ime_d->queued, ioreq->offset);  /* [한국어] 쓰기 배치 제출. */

	dprint(FD_IO, "committed %d iovecs\n", ime_d->queued);  /* [한국어] 배치 크기 로깅. */

	return fio_ime_psyncv_end(td, ret);                      /* [한국어] 결과를 각 io_u에 분배. */
}

/*
 * [한국어]
 * fio_ime_psyncv_getevents - commit에서 이미 완료된 전송을 fio에 알리는 단계.
 *
 * psyncv는 commit이 동기적으로 끝나므로 여기서는 io_us → event_io_us로 단순 복사만 하고
 * 큐 포인터를 0으로 리셋한다.
 */
static int fio_ime_psyncv_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct io_u *io_u;
	int events = 0;           /* [한국어] 반환할 완료 이벤트 수. */
	unsigned int count;

	if (ime_d->events) {      /* [한국어] 커밋되었으나 아직 수확 안 된 것이 있을 때만 처리. */
		for (count = 0; count < ime_d->events; count++) {
			io_u = ime_d->io_us[count];            /* [한국어] 커밋된 순서대로 io_u 추출. */
			ime_d->event_io_us[events] = io_u;     /* [한국어] fio가 fio_ime_event로 조회할 배열에 저장. */
			events++;
		}
		fio_ime_queue_reset(ime_d);              /* [한국어] psyncv는 한 배치가 끝나면 전체 포인터 리셋(링 동작 아님). */
	}

	dprint(FD_IO, "getevents(%u,%u) ret=%d queued=%u events=%u\n",
		min, max, events, ime_d->queued, ime_d->events);
	return events;
}

/*
 * [한국어]
 * fio_ime_psyncv_init - ime_psyncv의 init 콜백. IME 라이브러리 초기화 + ime_data 할당.
 *
 * ime_data 내부 필드:
 *   - sioreq: 단일 요청 기술자(imesio_req).
 *   - iovecs: iodepth 크기 iovec 배열.
 *   - io_us: 2*iodepth 크기(앞쪽 iodepth는 queued, 뒤쪽 iodepth는 event용으로 분할).
 */
static int fio_ime_psyncv_init(struct thread_data *td)
{
	struct ime_data *ime_d;

	if (fio_ime_engine_init(td) < 0)   /* [한국어] 공통 IME 초기화(라이브러리 init + real_file_size 임시 세팅). */
		return 1;

	ime_d = calloc(1, sizeof(*ime_d)); /* [한국어] 모든 필드 0 초기화 - head/tail/queued/events 모두 0부터. */

	ime_d->sioreq = malloc(sizeof(struct imesio_req));         /* [한국어] 단일 요청 구조체. */
	ime_d->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));  /* [한국어] iodepth만큼의 iovec. */
	ime_d->io_us = malloc(2 * td->o.iodepth * sizeof(struct io_u *));  /* [한국어] 2*iodepth: 앞 절반은 queued, 뒤 절반은 event용. */
	ime_d->event_io_us = ime_d->io_us + td->o.iodepth;          /* [한국어] 뒤 절반 시작 포인터. */

	ime_d->depth = td->o.iodepth;                               /* [한국어] 링 용량. */

	td->io_ops_data = ime_d;                                    /* [한국어] 엔진 상태를 잡에 부착. */
	return 0;
}

/*
 * [한국어]
 * fio_ime_psyncv_clean - ime_psyncv의 cleanup 콜백. init에서 할당한 자원 해제.
 */
static void fio_ime_psyncv_clean(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;

	if (ime_d) {                          /* [한국어] init이 성공했을 때만 해제. */
		free(ime_d->sioreq);              /* [한국어] 요청 기술자. */
		free(ime_d->iovecs);              /* [한국어] iovec 배열. */
		free(ime_d->io_us);               /* [한국어] io_us는 event_io_us까지 포함하는 단일 할당. */
		free(ime_d);                      /* [한국어] 상태 컨테이너 자체. */
		td->io_ops_data = NULL;           /* [한국어] 이후 오접근 방지. */
	}

	fio_ime_engine_finalize(td);          /* [한국어] 공통 finalize - fork 모드면 ime_native_finalize 호출. */
}


/**************************************************************
 *           Private functions for non-blocking IOs
 *
 **************************************************************/

/*
 * [한국어]
 * fio_ime_aio_complete_cb - IME 라이브러리가 AIO 완료 시 호출하는 콜백.
 *
 * @aiocb: 제출 시 넘긴 iocb 포인터. user_context에 struct imeaio_req* 들어 있음.
 * @err:   0이면 성공, 그 외는 라이브러리 에러 코드.
 * @bytes: 성공 시 실제 전송 바이트 수.
 *
 * 실행 컨텍스트: IME 라이브러리의 내부 스레드(잡 스레드가 아님). 따라서 status는
 * mutex로 보호하고, wait 중일 수 있는 getevents 스레드를 cond_signal로 깨운다.
 */
void fio_ime_aio_complete_cb  (struct ime_aiocb *aiocb, int err,
							   ssize_t bytes)
{
	struct imeaio_req *ioreq = (struct imeaio_req *) aiocb->user_context;  /* [한국어] 제출 시 저장한 self 포인터 복원. */

	pthread_mutex_lock(&ioreq->status_mutex);                              /* [한국어] status 기록 직전 락 획득 - getevents와 상호배제. */
	ioreq->status = err == 0 ? bytes : FIO_IME_REQ_ERROR;                  /* [한국어] 성공 시 바이트 수, 실패 시 에러 sentinel 기록. */
	pthread_mutex_unlock(&ioreq->status_mutex);                            /* [한국어] 락 해제 - 이후 signal은 락 없이 가능. */

	pthread_cond_signal(&ioreq->cond_endio);                               /* [한국어] getevents가 cond_wait 중이면 깨움. */
}

/*
 * [한국어]
 * fio_ime_aio_can_queue - aio 엔진에서 새 io_u를 큐에 받아들일지 판단하는 훅.
 *
 * 현재 구현은 depth 체크를 호출자에서 이미 수행하고 용량 외 추가 거부 조건이 없으므로
 * 항상 true. 미래에 backpressure 정책 삽입을 위한 확장점.
 */
static bool fio_ime_aio_can_queue (struct ime_data *ime_d, struct io_u *io_u)
{
	/* So far we can queue in any case. */
	return true;  /* [한국어] 무조건 허용. */
}
/*
 * [한국어]
 * fio_ime_aio_can_append - 새 io_u를 기존 "열린" AIO 요청에 iovec으로 붙일 수 있는지 판단.
 *
 * 기존 요청에 append하면 iocb 하나로 여러 iovec을 한 번에 제출할 수 있어 효율적이다.
 * 단, IME의 iov 포인터가 ime_d->iovecs 배열의 연속 구간을 가리키므로 head가 0으로
 * 감긴 상태면 연속성이 깨지므로 새 요청을 시작해야 한다.
 */
static bool fio_ime_aio_can_append (struct ime_data *ime_d, struct io_u *io_u)
{
	/* We can only append if:
		- The iovecs will be contiguous in the array
		- There is already a queued iovec
		- The offsets are contiguous
		- The ddir and fs are the same */
	return (ime_d->head != 0 &&                       /* [한국어] head==0은 링이 감겨 배열 시작 - 기존 iov 구간과 불연속. */
			ime_d->queued - ime_d->events > 0 &&       /* [한국어] 아직 미커밋된 요청이 큐에 있어야 append 대상 존재. */
			ime_d->last_offset == io_u->offset &&      /* [한국어] 파일 오프셋 연속성. */
			ime_d->last_req->ddir == io_u->ddir &&     /* [한국어] 방향 일치(aio_read vs aio_write). */
			ime_d->last_req->iocb.fd == io_u->file->fd); /* [한국어] fd 일치. */
}

/* Before using this function, we should have already
   ensured that the queue is not full */
/*
 * [한국어]
 * fio_ime_aio_enqueue - aio 큐에 io_u를 추가. 가능하면 기존 요청에 iovec append, 아니면 새 요청 생성.
 *
 * 사전조건: queued < depth. can_queue()가 true.
 * 새 요청 생성 경로에서는 iocb 전체 필드를 채우고 last_req를 이 요청으로 갱신한다.
 */
static void fio_ime_aio_enqueue(struct ime_data *ime_d, struct io_u *io_u)
{
	struct imeaio_req *ioreq = &ime_d->aioreqs[ime_d->head];  /* [한국어] head 슬롯의 요청 객체. */
	struct ime_aiocb *iocb = &ioreq->iocb;                     /* [한국어] 그 안의 iocb. */
	struct iovec *iov = &ime_d->iovecs[ime_d->head];           /* [한국어] 같은 head 위치의 iovec. iov 배열과 요청 배열이 head로 동기 진행. */

	iov->iov_base = io_u->xfer_buf;                            /* [한국어] 사용자 버퍼 등록. */
	iov->iov_len = io_u->xfer_buflen;                          /* [한국어] 바이트 수. */

	if (fio_ime_aio_can_append(ime_d, io_u))                   /* [한국어] 기존 요청과 연속이면 iovcnt만 증가시키는 경량 경로. */
		ime_d->last_req->iocb.iovcnt++;                        /* [한국어] 기존 iocb가 새 iov까지 포함하도록 확장 - 배열 연속성 덕. */
	else {
		ioreq->status = FIO_IME_IN_PROGRESS;                   /* [한국어] 새 요청은 기본 "진행 중" 상태로 시작. */
		ioreq->ddir = io_u->ddir;                              /* [한국어] 방향 기록. */
		ime_d->last_req = ioreq;                               /* [한국어] append 대상 포인터 갱신. */

		iocb->complete_cb = &fio_ime_aio_complete_cb;          /* [한국어] 완료 콜백 등록 - 라이브러리가 호출. */
		iocb->fd = io_u->file->fd;                             /* [한국어] 대상 IME fd. */
		iocb->file_offset = io_u->offset;                      /* [한국어] 이 요청 전체의 시작 오프셋. */
		iocb->iov = iov;                                       /* [한국어] iov 배열의 해당 위치를 가리키게 하여 이후 append 시 확장 가능. */
		iocb->iovcnt = 1;                                      /* [한국어] 초기 1개, append마다 증가. */
		iocb->flags = 0;                                       /* [한국어] 추가 플래그 없음. */
		iocb->user_context = (intptr_t) ioreq;                 /* [한국어] 완료 콜백이 역참조할 self 포인터 저장(intptr_t 캐스팅으로 IME ABI 맞춤). */
	}

	ime_d->io_us[ime_d->head] = io_u;                          /* [한국어] iov/iocb와 동기 인덱스로 io_u 저장. */
	ime_d->last_offset = io_u->offset + io_u->xfer_buflen;     /* [한국어] 다음 can_append 검사용. */
	fio_ime_queue_incr(ime_d);                                 /* [한국어] head/queued 전진. */
}

/* Tries to queue an IO. It will create a new request if the IO can't be
   appended to the current request. It will fail if the queue can't contain
   any more io_u/iovec. In this case, commit and then get_events need to be
   called. */
/*
 * [한국어]
 * fio_ime_aio_queue - ime_aio queue 콜백. iovec을 링 큐에 누적(가능하면 기존 요청에 확장).
 *
 * 실제 IME 제출은 commit 단계에서 이루어지며, 이 함수는 일단 누적만 한다. 큐가 꽉 차면
 * FIO_Q_BUSY로 fio에 "커밋/수확 먼저 하세요"를 알림.
 */
static enum fio_q_status fio_ime_aio_queue(struct thread_data *td,
		struct io_u *io_u)
{
	struct ime_data *ime_d = td->io_ops_data;

	fio_ro_check(td, io_u);

	dprint(FD_IO, "queue: ddir=%d at %u commit=%u queued=%u events=%u\n",
		io_u->ddir, ime_d->head, ime_d->cur_commit,
		ime_d->queued, ime_d->events);

	if (ime_d->queued == ime_d->depth)                   /* [한국어] 링 용량 초과 차단. */
		return FIO_Q_BUSY;

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {
		if (!fio_ime_aio_can_queue(ime_d, io_u))          /* [한국어] 확장 훅 - 현재는 항상 true. */
			return FIO_Q_BUSY;

		fio_ime_aio_enqueue(ime_d, io_u);                 /* [한국어] 실제 누적(append 또는 새 요청). */
		return FIO_Q_QUEUED;                              /* [한국어] 비동기 - 완료는 getevents에서. */
	} else if (io_u->ddir == DDIR_SYNC) {
		if (ime_native_fsync(io_u->file->fd) < 0) {       /* [한국어] fsync는 배치 대상 아니므로 즉시 동기 수행. */
			io_u->error = errno;
			td_verror(td, io_u->error, "fsync");
		}
		return FIO_Q_COMPLETED;
	} else {
		io_u->error = EINVAL;                             /* [한국어] 미지원 방향. */
		td_verror(td, io_u->error, "wrong ddir");
		return FIO_Q_COMPLETED;
	}
}

/*
 * [한국어]
 * fio_ime_aio_commit - 누적된 AIO 요청들을 ime_native_aio_read/write로 실제 제출.
 *
 * cur_commit에서 시작하여 (queued - events)가 0이 될 때까지 요청 단위로 제출한다.
 * 제출 실패 시 해당 요청에 REQ_ERROR를 기록하고 음수 errno를 반환.
 * 호출 체인: backend → td_io_commit → [이 함수].
 */
static int fio_ime_aio_commit(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imeaio_req *ioreq;
	int ret = 0;

	/* Loop while there are events to commit */
	while (ime_d->queued - ime_d->events) {              /* [한국어] 미커밋 요청이 남은 동안 반복. */
		ioreq = &ime_d->aioreqs[ime_d->cur_commit];       /* [한국어] 다음 제출 대상. */
		if (ioreq->ddir == DDIR_READ)
			ret = ime_native_aio_read(&ioreq->iocb);      /* [한국어] 비동기 읽기 제출 - 완료는 콜백으로. */
		else
			ret = ime_native_aio_write(&ioreq->iocb);     /* [한국어] 비동기 쓰기 제출. */

		fio_ime_queue_commit(ime_d, ioreq->iocb.iovcnt);  /* [한국어] cur_commit/events를 iovcnt만큼 전진. */

		/* fio needs a negative error code */
		if (ret < 0) {                                    /* [한국어] 제출 자체 실패. */
			ioreq->status = FIO_IME_REQ_ERROR;             /* [한국어] 콜백 없이 직접 에러 마크. */
			return -errno;                                 /* [한국어] fio는 음수 errno를 요구. */
		}

		io_u_mark_submit(td, ioreq->iocb.iovcnt);         /* [한국어] 통계: 제출된 io_u 수 누적(iovcnt = 이 배치의 io_u 수). */
		dprint(FD_IO, "committed %d iovecs commit=%u queued=%u events=%u\n",
			ioreq->iocb.iovcnt, ime_d->cur_commit,
			ime_d->queued, ime_d->events);
	}

	return 0;
}

/*
 * [한국어]
 * fio_ime_aio_getevents - 커밋된 AIO 요청들의 완료를 수집하여 fio에 보고.
 *
 * @min, @max: fio가 기대하는 최소/최대 완료 수. @t: 타임아웃(현재 구현은 무시).
 *
 * tail부터 커밋된 순서대로 확인. 요청이 이미 완료되어 있으면 속한 모든 iovec/io_u를 이벤트로
 * 변환하고, 아직 진행 중이면 mutex+cond로 완료 대기. 완료된 요청 하나가 iovcnt>1이면
 * 다음 요청을 받으면 max를 넘길 수 있어 break로 현재 라운드를 종료.
 */
static int fio_ime_aio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imeaio_req *ioreq;
	struct io_u *io_u;
	int events = 0;          /* [한국어] 이번 호출에서 반환할 이벤트 수. */
	unsigned int count;       /* [한국어] 요청 내 iovec 순회 인덱스. */
	ssize_t bytes;            /* [한국어] 요청이 전송한 총 바이트 - 각 io_u에 분배. */

	while (ime_d->events) {  /* [한국어] 커밋되었으나 수확 안 된 것이 남아 있는 동안. */
		ioreq = &ime_d->aioreqs[ime_d->tail];  /* [한국어] tail 위치의 요청. */

		/* Break if we already got events, and if we will
		   exceed max if we append the next events */
		if (events && events + ioreq->iocb.iovcnt > max)  /* [한국어] 이미 일부 반환했고 이 요청을 추가하면 max 초과 - 중단. */
			break;

		if (ioreq->status != FIO_IME_IN_PROGRESS) {       /* [한국어] 이미 완료된 요청(성공 바이트 수 또는 REQ_ERROR). */

			bytes = ioreq->status;                         /* [한국어] 총 전송 바이트(또는 에러 sentinel). */
			for (count = 0; count < ioreq->iocb.iovcnt; count++) {  /* [한국어] 이 요청에 묶인 iovec 수만큼 io_u 처리. */
				io_u = ime_d->io_us[ime_d->tail];          /* [한국어] tail 위치의 io_u. */
				ime_d->event_io_us[events] = io_u;         /* [한국어] fio가 볼 이벤트 배열에 기록. */
				events++;
				fio_ime_queue_red(ime_d);                  /* [한국어] tail/queued/events 감소. 다음 반복에서 tail이 이미 전진해 있음. */

				if (ioreq->status == FIO_IME_REQ_ERROR)    /* [한국어] 요청 전체가 에러였으면 개별 io_u에 EIO. */
					io_u->error = EIO;
				else {
					io_u->resid = bytes > io_u->xfer_buflen ?
									0 : io_u->xfer_buflen - bytes;  /* [한국어] 남은 전송량보다 요청 크기가 크면 resid=xfer-bytes, 아니면 0. */
					io_u->error = 0;                        /* [한국어] 성공(부분 포함). */
					bytes -= io_u->xfer_buflen - io_u->resid; /* [한국어] 이번 io_u가 소비한 실제 전송량을 총합에서 차감. */
				}
			}
		} else {                                           /* [한국어] 아직 진행 중 - 완료 콜백 대기. */
			pthread_mutex_lock(&ioreq->status_mutex);      /* [한국어] status 검사 전 락. */
			while (ioreq->status == FIO_IME_IN_PROGRESS)   /* [한국어] spurious wakeup 방어 루프. */
				pthread_cond_wait(&ioreq->cond_endio, &ioreq->status_mutex);  /* [한국어] 콜백이 signal할 때까지 대기. */
			pthread_mutex_unlock(&ioreq->status_mutex);    /* [한국어] 락 해제 후 다음 루프 반복이 status를 다시 읽음. */
		}

	}

	dprint(FD_IO, "getevents(%u,%u) ret=%d queued=%u events=%u\n", min, max,
		events, ime_d->queued, ime_d->events);
	return events;
}

/*
 * [한국어]
 * fio_ime_aio_init - ime_aio init 콜백. psyncv와 유사하지만 aioreqs 배열 할당 + 각 요청의
 *                     cond/mutex를 init해야 한다.
 */
static int fio_ime_aio_init(struct thread_data *td)
{
	struct ime_data *ime_d;
	struct imeaio_req *ioreq;
	unsigned int i;

	if (fio_ime_engine_init(td) < 0)  /* [한국어] 공통 IME 초기화. */
		return 1;

	ime_d = calloc(1, sizeof(*ime_d));  /* [한국어] 0으로 초기화된 상태 컨테이너. */

	ime_d->aioreqs = malloc(td->o.iodepth * sizeof(struct imeaio_req));  /* [한국어] iodepth 크기 요청 배열. */
	ime_d->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));        /* [한국어] 동일 크기 iovec 배열. */
	ime_d->io_us = malloc(2 * td->o.iodepth * sizeof(struct io_u *));    /* [한국어] queued+event 용 2배 크기. */
	ime_d->event_io_us = ime_d->io_us + td->o.iodepth;                    /* [한국어] 뒤 절반 시작. */

	ime_d->depth = td->o.iodepth;
	for (i = 0; i < ime_d->depth; i++) {        /* [한국어] 각 요청의 cond/mutex 초기화. */
		ioreq = &ime_d->aioreqs[i];
		pthread_cond_init(&ioreq->cond_endio, NULL);       /* [한국어] 기본 속성 cond 생성. */
		pthread_mutex_init(&ioreq->status_mutex, NULL);    /* [한국어] 기본 속성 mutex 생성. */
	}

	td->io_ops_data = ime_d;
	return 0;
}

/*
 * [한국어]
 * fio_ime_aio_clean - ime_aio cleanup 콜백. 각 요청의 cond/mutex destroy + 배열 해제.
 */
static void fio_ime_aio_clean(struct thread_data *td)
{
	struct ime_data *ime_d = td->io_ops_data;
	struct imeaio_req *ioreq;
	unsigned int i;

	if (ime_d) {
		for (i = 0; i < ime_d->depth; i++) {     /* [한국어] 각 요청의 동기화 객체 파괴. */
			ioreq = &ime_d->aioreqs[i];
			pthread_cond_destroy(&ioreq->cond_endio);
			pthread_mutex_destroy(&ioreq->status_mutex);
		}
		free(ime_d->aioreqs);                     /* [한국어] 요청 배열. */
		free(ime_d->iovecs);                      /* [한국어] iovec 배열. */
		free(ime_d->io_us);                       /* [한국어] io_us(+event_io_us) 통합 할당. */
		free(ime_d);
		td->io_ops_data = NULL;
	}

	fio_ime_engine_finalize(td);                  /* [한국어] fork 모드면 라이브러리도 종료. */
}


/**************************************************************
 *                   IO engines definitions
 *
 **************************************************************/

/* The FIO_DISKLESSIO flag used for these engines is necessary to prevent
   FIO from using POSIX calls. See fio_ime_open_file for more details. */
/* [한국어] FIO_DISKLESSIO는 "fio 코어가 POSIX 파일 계열 호출을 스스로 하지 않도록" 막는 플래그.
 *          IME 경로는 POSIX 네임스페이스에 있지 않을 수 있으므로 stat/open/close를 모두
 *          엔진이 인수해야 한다. FIO_SYNCIO는 잡 러너에게 "이 엔진은 동기 방식"임을 알려
 *          iodepth>1에서도 getevents를 기본 경로로 쓰지 않도록 한다(psyncv는 commit이 동기이므로 포함). */

/* [한국어] ime_psync 엔진 op 테이블 - 단순 동기 pread/pwrite/fsync. commit/getevents 없음. */
static struct ioengine_ops ioengine_prw = {
	.name		= "ime_psync",                    /* [한국어] --ioengine=ime_psync 로 선택되는 엔진 이름. */
	.version	= FIO_IOOPS_VERSION,              /* [한국어] 엔진 ABI 버전 매칭용. 불일치 시 fio 로드 거부. */
	.setup		= fio_ime_setup,                  /* [한국어] real_file_size=0으로 세팅하여 init을 늦춤. */
	.init		= fio_ime_engine_init,            /* [한국어] ime_native_init 호출. */
	.cleanup	= fio_ime_engine_finalize,        /* [한국어] fork 모드에서 ime_native_finalize. */
	.queue		= fio_ime_psync_queue,            /* [한국어] 동기 pread/pwrite/fsync 직접 호출. */
	.open_file	= fio_ime_open_file,              /* [한국어] IME prefix 붙여 ime_native_open. */
	.close_file	= fio_ime_close_file,             /* [한국어] ime_native_close. */
	.get_file_size	= fio_ime_get_file_size,      /* [한국어] ime_native_stat. */
	.unlink_file  	= fio_ime_unlink_file,         /* [한국어] POSIX unlink(IME도 POSIX 네임스페이스 노출). */
	.flags	    	= FIO_SYNCIO | FIO_DISKLESSIO, /* [한국어] 동기 + POSIX 우회. */
};

/* [한국어] ime_psyncv 엔진 op 테이블 - iovec 누적 + 단일 preadv/pwritev. */
static struct ioengine_ops ioengine_pvrw = {
	.name		= "ime_psyncv",                   /* [한국어] --ioengine=ime_psyncv. */
	.version	= FIO_IOOPS_VERSION,
	.setup		= fio_ime_setup,
	.init		= fio_ime_psyncv_init,            /* [한국어] 상태 구조체 + iovec 배열 할당. */
	.cleanup	= fio_ime_psyncv_clean,           /* [한국어] 할당 자원 해제. */
	.queue		= fio_ime_psyncv_queue,           /* [한국어] iovec에 누적(FIO_Q_QUEUED 반환). */
	.commit		= fio_ime_psyncv_commit,          /* [한국어] preadv/pwritev 단일 호출로 배치 플러시. */
	.getevents	= fio_ime_psyncv_getevents,       /* [한국어] io_us → event_io_us 복사 + 큐 리셋. */
	.event		= fio_ime_event,                  /* [한국어] event_io_us[event] 반환. */
	.open_file	= fio_ime_open_file,
	.close_file	= fio_ime_close_file,
	.get_file_size	= fio_ime_get_file_size,
	.unlink_file  	= fio_ime_unlink_file,
	.flags	    	= FIO_SYNCIO | FIO_DISKLESSIO, /* [한국어] commit이 동기이므로 SYNCIO 유지. */
};

/* [한국어] ime_aio 엔진 op 테이블 - 진짜 비동기 ime_native_aio_{read,write}. */
static struct ioengine_ops ioengine_aio = {
	.name		= "ime_aio",                      /* [한국어] --ioengine=ime_aio. */
	.version	= FIO_IOOPS_VERSION,
	.setup		= fio_ime_setup,
	.init		= fio_ime_aio_init,               /* [한국어] 요청 배열 + cond/mutex 초기화. */
	.cleanup	= fio_ime_aio_clean,              /* [한국어] destroy + free. */
	.queue		= fio_ime_aio_queue,              /* [한국어] 배치에 누적, 필요 시 새 요청 생성. */
	.commit		= fio_ime_aio_commit,             /* [한국어] aio_read/write 제출 루프. */
	.getevents	= fio_ime_aio_getevents,          /* [한국어] 콜백 대기 + 완료 iovec → io_u 변환. */
	.event		= fio_ime_event,
	.open_file	= fio_ime_open_file,
	.close_file	= fio_ime_close_file,
	.get_file_size	= fio_ime_get_file_size,
	.unlink_file  	= fio_ime_unlink_file,
	.flags       	= FIO_DISKLESSIO,             /* [한국어] 비동기이므로 SYNCIO 아님 - fio 러너가 queue-depth 기반 동작 수행. */
};

/*
 * [한국어]
 * fio_ime_register - 컴파일된 공유 객체가 로드될 때 fio 런타임에 세 엔진을 모두 등록.
 *                     fio_init 매크로로 .init_array에 등록되어 자동 호출됨.
 */
static void fio_init fio_ime_register(void)
{
	register_ioengine(&ioengine_prw);   /* [한국어] ime_psync 등록 - 전역 ioengine 리스트에 link. */
	register_ioengine(&ioengine_pvrw);  /* [한국어] ime_psyncv 등록. */
	register_ioengine(&ioengine_aio);   /* [한국어] ime_aio 등록. */
}

/*
 * [한국어]
 * fio_ime_unregister - 프로세스 종료 시 fio_exit(.fini_array) 훅에서 호출되어 엔진 해제 및
 *                       thread 모드에서 지연되었던 ime_native_finalize를 수행.
 */
static void fio_exit fio_ime_unregister(void)
{
	unregister_ioengine(&ioengine_prw);  /* [한국어] 전역 리스트에서 제거. */
	unregister_ioengine(&ioengine_pvrw);
	unregister_ioengine(&ioengine_aio);

	if (fio_ime_is_initialized && ime_native_finalize() < 0)  /* [한국어] thread 모드에서는 cleanup이 finalize를 안 했으므로 여기서 마지막에 수행. 실패해도 경고만. */
		log_err("Warning: IME did not finalize properly\n");
}
