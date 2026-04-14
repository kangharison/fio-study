/*
 * [한국어 설명] NFS I/O 엔진 구현 (nfs.c)
 *
 * === 파일의 역할 ===
 * libnfs(유저스페이스 NFS 클라이언트)를 이용해 커널 NFS 마운트 없이 NFS 서버로 직접
 * pread/pwrite를 비동기 발행하는 fio 엔진. libnfs의 콜백 모델(nfs_service)을 poll
 * 기반 이벤트 루프로 구동하여 완료 이벤트를 수집하고 원형 큐(events[])에 쌓는다.
 * 원형 큐 인덱스(next_buffered_event, free_event_buffer_index)로 생산자-소비자를 분리.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend → load_ioengine("nfs") → setup(use_thread=0 강제) → open_file(=do_mount + nfs_open)
 * → queue(nfs_pread/pwrite_async 제출) → getevents(nfs_event_loop: poll + nfs_service)
 * → event(큐에서 io_u 하나 pop). 실행 컨텍스트는 잡 프로세스(스레드는 libnfs와 상성 안좋음).
 *
 * === 타 모듈과의 연결 ===
 * - fio.h, optgroup.h: 공용 타입/옵션.
 * - libnfs (nfsc/*.h): NFS v3/v4 RPC 구현.
 * - poll(2): libnfs가 노출한 단일 소켓 FD에 대한 이벤트 대기.
 * - 공유 상태: td->eo = fio_libnfs_options(잡 전용, libnfs 컨텍스트 + events 큐);
 *   f->engine_data = nfs_data(파일 핸들 + options 역참조).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_libnfs_options: 컨텍스트/URL/iodepth + 원형 큐 상태.
 * - struct nfs_data:           파일 핸들(nfsfh) + options 포인터.
 * - fio_libnfs_setup():        use_thread=0 설정(데드락 회피).
 * - fio_libnfs_open():         do_mount 후 nfs_open.
 * - fio_libnfs_queue():        ddir별 async read/write 제출.
 * - nfs_callback():            libnfs 완료 콜백 — 결과 복사 + 큐 push.
 * - nfs_event_loop()/getevents/event: poll+service 루프와 event pop.
 */

#include <stdlib.h>                        /* [한국어] malloc/calloc/free/strcpy 등 */
#include <poll.h>                          /* [한국어] poll(2)로 NFS 소켓 이벤트 대기 */
#include <nfsc/libnfs.h>                   /* [한국어] libnfs 상위 API */
#include <nfsc/libnfs-raw.h>               /* [한국어] libnfs raw API(nfs_which_events 등) */
#include <nfsc/libnfs-raw-mount.h>         /* [한국어] NFS mount 프로토콜 타입 */

#include "../fio.h"
#include "../optgroup.h"

/*
 * [한국어] 메타데이터 벤치 확장용 op 분류. 현재 소스는 주로 READ/WRITE만 구현하며,
 * 나머지는 미래 확장용 enum 값으로 남아있다.
 */
enum nfs_op_type {
	NFS_READ_WRITE = 0,       /* [한국어] 일반 pread/pwrite 경로 */
	NFS_STAT_MKDIR_RMDIR,     /* [한국어] 디렉터리 stat/mkdir/rmdir 메타벤치(예약) */
	NFS_STAT_TOUCH_RM,        /* [한국어] 파일 stat/touch/rm 메타벤치(예약) */
};

/*
 * [한국어] NFS 엔진의 잡 전용 옵션/런타임 상태. td->eo가 이 구조체를 가리킨다.
 * 원형 큐(events)는 생산자=nfs_callback, 소비자=fio_libnfs_event.
 * 모두 같은 잡 프로세스 문맥에서 실행되어 락이 필요 없다.
 */
struct fio_libnfs_options {
	struct nfs_context *context;
	/* [한국어] libnfs의 NFS 컨텍스트(소켓 + RPC 상태머신).
	 * 설정자: do_mount()에서 nfs_init_context/nfs_mount 성공 시. 읽는 자: queue/loop/close.
	 * 값 범위: 유효 포인터(마운트 후) 또는 NULL. 동기화: 잡 전용. */

	char *nfs_url;
	/* [한국어] "nfs://server/export[/path]" 형식의 서버 URL.
	 * 설정자: 옵션 파서. 읽는 자: do_mount. */

	unsigned int queue_depth;
	/* [한국어] 원형 큐 크기(= td->o.iodepth).
	 * 설정자: do_mount. 읽는 자: nfs_callback의 모듈로 연산. */

	/* [한국어] 아래 필드들은 미완료/완료 I/O의 원형 큐를 구현한다. */

	int outstanding_events;
	/* [한국어] libnfs에 제출된 후 아직 콜백이 오지 않은 I/O 수.
	 * 설정자: queue()에서 ++; nfs_callback에서 --.
	 * 읽는 자: SHOULD_WAIT 매크로에서 poll 필요 여부 판단. */

	int prev_requested_event_index;
	/* [한국어] fio 코어가 직전에 꺼낸 event 인덱스 — 순차 호출 규약(assert) 검증용.
	 * 값 범위: -1(미수확) 또는 0..iodepth-1. */

	int next_buffered_event;
	/* [한국어] 소비자(event)가 다음에 읽을 원형 버퍼 위치. */

	int buffered_event_count;
	/* [한국어] libnfs 완료 콜백이 큐에 push했지만 fio가 아직 event()로 꺼내지 않은 수. */

	int free_event_buffer_index;
	/* [한국어] 다음 push 위치 — 생산자 포인터. */

	struct io_u**events;
	/* [한국어] 완료 io_u 포인터 원형 배열(크기 queue_depth).
	 * 설정자: do_mount calloc, nfs_callback에서 슬롯에 write.
	 * 읽는 자: fio_libnfs_event가 슬롯에서 pop하고 NULL 대입. */
};

/*
 * [한국어] 파일별 NFS 핸들 + 옵션 역참조.
 * f->engine_data가 이 포인터를 가짐.
 */
struct nfs_data {
	struct nfsfh *nfsfh;
	/* [한국어] libnfs가 nfs_open에서 반환한 파일 핸들(불투명).
	 * 설정자: fio_libnfs_open. 읽는 자: queue_read/queue_write/close. */

	struct fio_libnfs_options *options;
	/* [한국어] 잡 전역 옵션 구조체 역링크 — 콜백에서 컨텍스트 접근용. */
};

static struct fio_option options[] = {
	{
		.name	= "nfs_url",
		.lname	= "nfs_url",
		.type	= FIO_OPT_STR_STORE,
		.help	= "URL in libnfs format, eg nfs://<server|ipv4|"
			  "ipv6>/path[?arg=val[&arg=val]*]",
		.off1	= offsetof(struct fio_libnfs_options, nfs_url),
		.category = FIO_OPT_C_ENGINE,
		.group	= __FIO_OPT_G_NFS,
	},
	{
		.name     = NULL,
	},
};

/*
 * [한국어]
 * fio_libnfs_event - ioengine_ops.event. 원형 큐에서 io_u 하나를 pop.
 * fio 코어는 0..nr_events-1 인덱스로 순차 호출한다고 가정하며 assert로 검증.
 * 호출 체인: 코어(getevents 결과 소비) → [fio_libnfs_event].
 */
static struct io_u *fio_libnfs_event(struct thread_data *td, int event)
{
	struct fio_libnfs_options *o = td->eo;
	struct io_u *io_u = o->events[o->next_buffered_event];  /* [한국어] 소비자 포인터에서 pop */

	assert(o->events[o->next_buffered_event]);   /* [한국어] 슬롯이 비어있지 않아야 함 */
	o->events[o->next_buffered_event] = NULL;     /* [한국어] 슬롯 비우기(재사용 준비) */
	/* [한국어] 원형 버퍼 라운드 로빈 전진 */
	o->next_buffered_event = (o->next_buffered_event + 1) % td->o.iodepth;

	/* validate our state machine */
	assert(o->buffered_event_count);              /* [한국어] 수확 가능 건이 있어야 함 */
	o->buffered_event_count--;                     /* [한국어] 버퍼 카운터 감소 */
	assert(io_u);

	/* assert that fio_libnfs_event is being called in sequential fashion */
	/* [한국어] fio 코어가 이벤트 인덱스를 0부터 증가시키며 호출하는지 검증 */
	assert(event == 0 || o->prev_requested_event_index + 1 == event);
	if (o->buffered_event_count == 0)
		o->prev_requested_event_index = -1;    /* [한국어] 모두 소진 시 상태 리셋 */
	else
		o->prev_requested_event_index = event; /* [한국어] 마지막 인덱스 기록 */
	return io_u;
}

/*
 * fio core logic seems to stop calling this event-loop if we ever return with
 * 0 events
 */
/* [한국어] poll 대기 여부 결정 매크로:
 *  - 큐가 가득 찼을 때(outstanding == iodepth) 또는
 *  - flush 요청인데 아직 비행 중인 건이 있을 때 → timeout=-1(무한대기)
 *  - 그 외에는 timeout=0(논블록 진행)
 */
#define SHOULD_WAIT(td, o, flush)			\
 	((o)->outstanding_events == (td)->o.iodepth ||	\
		(flush && (o)->outstanding_events))

/*
 * [한국어]
 * nfs_event_loop - poll + nfs_service 조합으로 libnfs 상태머신을 진행시키고 완료 수집.
 * @flush: true면 모든 비행 I/O가 끝날 때까지 블록.
 * @return: buffered_event_count(fio가 수확 가능한 건수).
 * 호출 체인: fio_libnfs_getevents → [nfs_event_loop] → poll(2)/nfs_service → nfs_callback.
 */
static int nfs_event_loop(struct thread_data *td, bool flush)
{
	struct fio_libnfs_options *o = td->eo;
	struct pollfd pfds[1]; /* nfs:0 */  /* [한국어] libnfs는 단일 소켓 FD만 노출 */

	/* we already have stuff queued for fio, no need to waste cpu on poll() */
	/* [한국어] 이미 pop 가능 건이 있으면 poll 생략하고 바로 반환(빠른 경로) */
	if (o->buffered_event_count)
		return o->buffered_event_count;

	do {
		int timeout = SHOULD_WAIT(td, o, flush) ? -1 : 0;  /* [한국어] 블록/논블록 결정 */
		int ret = 0;

		pfds[0].fd = nfs_get_fd(o->context);                   /* [한국어] libnfs 소켓 FD */
		pfds[0].events = nfs_which_events(o->context);         /* [한국어] 현재 상태머신이 원하는 이벤트 마스크 */
		ret = poll(&pfds[0], 1, timeout);                       /* [한국어] 이벤트 대기 */
		if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;                                      /* [한국어] 시그널/임시 오류 — 재시도 */
			log_err("nfs: failed to poll events: %s\n", strerror(errno));
			break;
		}

		ret = nfs_service(o->context, pfds[0].revents);        /* [한국어] 상태머신 진행 + 완료 콜백 호출 */
		if (ret < 0) {
			log_err("nfs: socket is in an unrecoverable error state.\n");
			break;
		}
	} while (SHOULD_WAIT(td, o, flush));

	return o->buffered_event_count;  /* [한국어] 이번 루프로 누적된 수확 가능 건수 */
}

/*
 * [한국어]
 * fio_libnfs_getevents - ioengine_ops.getevents. 비 블로킹 이벤트 루프를 한 번 돌린다.
 */
static int fio_libnfs_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	return nfs_event_loop(td, false);
}

/*
 * [한국어]
 * nfs_callback - libnfs 완료 콜백. 결과를 io_u에 반영하고 원형 큐에 push.
 * @res:          >=0이면 전송 바이트, <0이면 음의 에러 코드.
 * @data:         READ의 경우 libnfs가 할당한 수신 버퍼(완료 후 내부 해제).
 * @private_data: queue 시 넘긴 io_u.
 * 호출 체인: nfs_service → libnfs 내부 → [nfs_callback] → events 큐 push.
 */
static void nfs_callback(int res, struct nfs_context *nfs, void *data,
			 void *private_data)
{
	struct io_u *io_u = private_data;                   /* [한국어] queue 시 심어둔 io_u */
	struct nfs_data *nfs_data = io_u->file->engine_data;
	struct fio_libnfs_options *o = nfs_data->options;
	if (res < 0) {
		log_err("Failed NFS operation(code:%d): %s\n", res,
						nfs_get_error(o->context));
		io_u->error = -res;                 /* [한국어] errno 양수로 저장 */
		/* res is used for read math below, don't want to pass negative
		 * there
		 */
		res = 0;   /* [한국어] resid 계산에 음수 들어가지 않도록 0으로 보정 */
	} else if (io_u->ddir == DDIR_READ) {
		/* [한국어] libnfs는 read 결과를 내부 버퍼에 주므로 사용자 버퍼로 복사 */
		memcpy(io_u->buf, data, res);
		if (res == 0)
			log_err("Got NFS EOF, this is probably not expected\n");
	}
	/* fio uses resid to track remaining data */
	io_u->resid = io_u->xfer_buflen - res;   /* [한국어] 미전송 잔여 바이트 기록 */

	assert(!o->events[o->free_event_buffer_index]);  /* [한국어] 생산자 슬롯이 비어있어야 함 */
	o->events[o->free_event_buffer_index] = io_u;    /* [한국어] 큐에 push */
	o->free_event_buffer_index = (o->free_event_buffer_index + 1) % o->queue_depth;  /* [한국어] 생산자 포인터 전진 */
	o->outstanding_events--;                           /* [한국어] 비행 카운터 감소 */
	o->buffered_event_count++;                         /* [한국어] 수확 가능 카운터 증가 */
}

/*
 * [한국어]
 * queue_write - ddir=WRITE 분기 헬퍼. libnfs 버전별 API 시그니처 차이를 #ifdef로 분기.
 * @return: 0 성공, <0 실패. 호출 체인: fio_libnfs_queue → [queue_write] → nfs_pwrite_async.
 */
static int queue_write(struct fio_libnfs_options *o, struct io_u *io_u)
{
	struct nfs_data *nfs_data = io_u->engine_data;  /* [한국어] 파일 핸들 획득 */

#ifdef LIBNFS_API_V2
	/* [한국어] libnfs v2: (buf, len, off, cb) 순서 */
	return nfs_pwrite_async(o->context, nfs_data->nfsfh,
				io_u->buf, io_u->buflen, io_u->offset,
				nfs_callback, io_u);
#else
	/* [한국어] libnfs v1: (off, len, buf, cb) 순서 */
	return nfs_pwrite_async(o->context, nfs_data->nfsfh, io_u->offset,
				io_u->buflen, io_u->buf, nfs_callback, io_u);
#endif
}

/*
 * [한국어]
 * queue_read - ddir=READ 분기 헬퍼. 버전별 시그니처 분기.
 */
static int queue_read(struct fio_libnfs_options *o, struct io_u *io_u)
{
	struct nfs_data *nfs_data = io_u->engine_data;

#ifdef LIBNFS_API_V2
	/* [한국어] v2는 쓰기와 동일한 인자 순서 */
	return nfs_pread_async(o->context, nfs_data->nfsfh,
				io_u->buf, io_u->buflen, io_u->offset,
				nfs_callback, io_u);
#else
	/* [한국어] v1: read는 유저 버퍼를 전달하지 않고 libnfs 내부 버퍼 사용 → 콜백에서 memcpy */
	return nfs_pread_async(o->context, nfs_data->nfsfh, io_u->offset,
				io_u->buflen, nfs_callback, io_u);
#endif
}

/*
 * [한국어]
 * fio_libnfs_queue - ioengine_ops.queue. 비동기 NFS read/write 제출.
 * 실패 시 td->error=1로 잡 종료 유도.
 */
static enum fio_q_status fio_libnfs_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct nfs_data *nfs_data = io_u->file->engine_data;   /* [한국어] 파일별 상태 */
	struct fio_libnfs_options *o = nfs_data->options;      /* [한국어] 잡 전역 옵션/큐 */
	struct nfs_context *nfs = o->context;                  /* [한국어] libnfs 컨텍스트 */
	enum fio_q_status ret = FIO_Q_QUEUED;
	int err;

	io_u->engine_data = nfs_data;   /* [한국어] queue_read/write가 참조 */
	switch (io_u->ddir) {
	case DDIR_WRITE:
		err = queue_write(o, io_u);
		break;
	case DDIR_READ:
		err = queue_read(o, io_u);
		break;
	case DDIR_TRIM:
		log_err("nfs: trim is not supported");   /* [한국어] NFS는 trim 미지원 */
		err = -1;
		break;
	default:
		log_err("nfs: unhandled io %d\n", io_u->ddir);
		err = -1;
	}
	if (err) {
		log_err("nfs: Failed to queue nfs op: %s\n", nfs_get_error(nfs));
		td->error = 1;   /* [한국어] 잡 중단 트리거 */
		return FIO_Q_COMPLETED;
	}
	o->outstanding_events++;   /* [한국어] 제출 성공 — 비행 카운터 증가 */
	return ret;
}

/*
 * Do a mount if one has not been done before
 */
/*
 * [한국어]
 * do_mount - 지정 URL로 nfs 마운트 + 이벤트 큐 초기화. 이미 컨텍스트가 있으면 no-op.
 * @return: 0 성공, <0 실패.
 * 호출 체인: fio_libnfs_open → [do_mount] → libnfs::nfs_init_context/nfs_parse_url_full/nfs_mount.
 */
static int do_mount(struct thread_data *td, const char *url)
{
	size_t event_size = sizeof(struct io_u **) * td->o.iodepth;  /* [한국어] 원형 큐 바이트 크기 */
	struct fio_libnfs_options *options = td->eo;
	struct nfs_url *nfs_url = NULL;
	int ret = 0;
	int path_len = 0;
	char *mnt_dir = NULL;

	if (options->context)
		return 0;   /* [한국어] 이미 마운트됨(다중 파일 시 재호출 대비) */

	options->context = nfs_init_context();   /* [한국어] libnfs 컨텍스트 생성 */
	if (!options->context) {
		log_err("nfs: failed to init nfs context\n");
		return -1;
	}

	options->events = calloc(1, event_size);   /* [한국어] 원형 큐 할당 */

	options->prev_requested_event_index = -1;   /* [한국어] 상태 초기화 */
	options->queue_depth = td->o.iodepth;

	nfs_url = nfs_parse_url_full(options->context, url);  /* [한국어] URL을 server/path/file로 분해 */
	path_len = strlen(nfs_url->path);
	/* [한국어] mount 대상 디렉터리 = path + file (예: /export/dir + subdir) */
	mnt_dir = malloc(path_len + strlen(nfs_url->file) + 1);
	strcpy(mnt_dir, nfs_url->path);
	strcpy(mnt_dir + strlen(nfs_url->path), nfs_url->file);
	ret = nfs_mount(options->context, nfs_url->server, mnt_dir);  /* [한국어] 실제 NFS 마운트 */
	free(mnt_dir);
	nfs_destroy_url(nfs_url);
	return ret;
}

/*
 * [한국어]
 * fio_libnfs_setup - ioengine_ops.setup. libnfs는 프로세스 종료 시 스레드 모드에서
 *                    hang을 일으키는 이슈가 있어 use_thread=0(프로세스 모드)로 강제.
 */
static int fio_libnfs_setup(struct thread_data *td)
{
	/* Using threads with libnfs causes fio to hang on exit, lower
	 * performance
	 */
	td->o.use_thread = 0;
	return 0;
}

/*
 * [한국어]
 * fio_libnfs_cleanup - 언마운트 + 컨텍스트 파괴 + 큐 해제.
 */
static void fio_libnfs_cleanup(struct thread_data *td)
{
	struct fio_libnfs_options *o = td->eo;

	nfs_umount(o->context);            /* [한국어] NFS 언마운트 RPC */
	nfs_destroy_context(o->context);   /* [한국어] 소켓/메모리 정리 */
	free(o->events);                     /* [한국어] 원형 큐 해제 */
}

/*
 * [한국어]
 * fio_libnfs_open - 대상 파일을 nfs_open으로 연다. 최초 호출 시 마운트 수행.
 * @return: 0 성공, 그 외 실패.
 */
static int fio_libnfs_open(struct thread_data *td, struct fio_file *f)
{
	struct fio_libnfs_options *options = td->eo;
	struct nfs_data *nfs_data = NULL;
	int flags = 0;
	int ret;

	if (!options->nfs_url) {
		log_err("nfs: nfs_url is a required parameter\n");
		return -1;
	}

	ret = do_mount(td, options->nfs_url);   /* [한국어] 필요 시 마운트 */

	if (ret) {
		log_err("nfs: Failed to mount %s with code %d: %s\n",
			options->nfs_url, ret, nfs_get_error(options->context));
		return ret;
	}
	nfs_data = calloc(1, sizeof(struct nfs_data));  /* [한국어] 파일별 상태 */
	nfs_data->options = options;                      /* [한국어] 역링크 저장 */

	/* [한국어] 쓰기 잡이면 필요시 파일 생성, 아니면 단순 RDWR */
	if (td_write(td))
		flags |= O_CREAT | O_RDWR;
	else
		flags |= O_RDWR;

	ret = nfs_open(options->context, f->file_name, flags, &nfs_data->nfsfh);  /* [한국어] 핸들 획득 */

	if (ret)
		log_err("Failed to open %s: %s\n", f->file_name,
					nfs_get_error(options->context));
	f->engine_data = nfs_data;   /* [한국어] 파일에 상태 부착 */
	return ret;
}

/*
 * [한국어]
 * fio_libnfs_close - nfs_close로 핸들 정리 후 상태 해제.
 */
static int fio_libnfs_close(struct thread_data *td, struct fio_file *f)
{
	struct nfs_data *nfs_data = f->engine_data;
	struct fio_libnfs_options *o = nfs_data->options;
	int ret = 0;

	if (nfs_data->nfsfh)
		ret = nfs_close(o->context, nfs_data->nfsfh);

	free(nfs_data);
	f->engine_data = NULL;
	return ret;
}

static struct ioengine_ops ioengine = {
	.name		= "nfs",
	.version	= FIO_IOOPS_VERSION,
	.setup		= fio_libnfs_setup,
	.queue		= fio_libnfs_queue,
	.getevents	= fio_libnfs_getevents,
	.event		= fio_libnfs_event,
	.cleanup	= fio_libnfs_cleanup,
	.open_file	= fio_libnfs_open,
	.close_file	= fio_libnfs_close,
	.flags		= FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL,
	.options	= options,
	.option_struct_size	= sizeof(struct fio_libnfs_options),
};

/* [한국어] 생성자/소멸자 — ioengine 레지스트리 등록/해제 */
static void fio_init fio_nfs_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_nfs_unregister(void)
{
	unregister_ioengine(&ioengine);
}
