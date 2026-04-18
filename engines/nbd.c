/*
 * [한국어 설명] NBD (Network Block Device) I/O 엔진 구현 (nbd.c)
 *
 * === 파일의 역할 ===
 * libnbd 기반 NBD 클라이언트 엔진. nbd:// 혹은 nbd+unix:// URI로 지정된 원격 NBD 서버에
 * 붙어서 pread/pwrite/trim/flush를 모두 비동기(aio) API로 제출하고, nbd_poll로 완료를
 * 진행시키며 nbd_aio_peek_command_completed 루프로 수확한다. 파일이 없어도(diskless)
 * 동작하도록 FIO_DISKLESSIO 플래그로 "nbd" 가상 파일을 내부에서 add_file 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend → load_ioengine("nbd") → setup(서버 사이즈 프로브) → init(잡 스레드별 연결)
 * → queue/getevents/event → cleanup. 비동기 엔진이라 queue()가 FIO_Q_QUEUED 반환 후
 * getevents/event가 완료를 분리 수확한다. 실행 컨텍스트는 잡 스레드이며 libnbd 내부가
 * 커넥션 소켓/상태머신을 관리한다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h: thread_data/io_u/ioengine_ops 등.
 * - optgroup.h + FIO_OPT_G_NBD: 엔진 옵션 그룹.
 * - libnbd: NBD URI 연결/AIO API/에러 보고.
 * - 데이터 흐름: io_u.xfer_buf ↔ nbd_aio_pread/pwrite ↔ NBD 서버 블록.
 * - 공유 상태: td->io_ops_data = nbd_data (잡당); io_u->engine_data는 완료 콜백에서
 *   nbd_data로 역조회하는 용도로 세팅.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nbd_data:      libnbd 핸들 + 완료된 io_u 동적 배열.
 * - struct nbd_options:   uri 문자열 옵션.
 * - nbd_setup():          1회성 가상 파일 등록 + 서버 접속해 size 프로브.
 * - nbd_init():           각 잡 스레드별 실제 커넥션 수립.
 * - nbd_queue():          ddir에 따라 aio_pread/pwrite/trim/flush 발행.
 * - cmd_completed():      libnbd 완료 콜백 — io_u를 nbd_data->completed에 push.
 * - nbd_getevents()/nbd_event(): nbd_poll + retire_commands로 완료 개수 수확.
 */

/*
 * NBD engine
 *
 * IO engine that talks to an NBD server.
 *
 * Copyright (C) 2019 Red Hat Inc.
 * Written by Richard W.M. Jones <rjones@redhat.com>
 *
 */

#include <stdio.h>     /* [한국어] log_err/log_info 포매팅 — printf 계열 매크로 공급. */
#include <stdlib.h>    /* [한국어] calloc(nbd_data 0-할당), realloc(completed 배열 확장), free(cleanup). */
#include <stdint.h>    /* [한국어] int64_t — NBD cookie와 서버 size 반환 타입. */
#include <errno.h>     /* [한국어] errno 전역, EIO/EINVAL — libnbd 실패 경로의 폴백 에러 코드. */

#include <libnbd.h>    /* [한국어] NBD 클라이언트 라이브러리 API:
                          · nbd_create/nbd_connect_uri — 서버 접속/핸드쉐이크
                          · nbd_aio_pread/pwrite/trim/flush — 비동기 I/O 제출
                          · nbd_poll — 상태머신 진행 + 완료 콜백 실행
                          · nbd_aio_peek_command_completed / nbd_aio_command_completed — 완료 회수
                          · nbd_get_error/nbd_get_errno — 에러 진단 */

#include "../fio.h"       /* [한국어] thread_data/io_u/ioengine_ops/fio_file, add_file, fio_ro_check 등 코어. */
#include "../optgroup.h"  /* [한국어] FIO_OPT_C_ENGINE/FIO_OPT_G_NBD — 옵션 카테고리/그룹 상수. */

/* Actually this differs across servers, but for nbdkit ... */
/* [한국어] NBD 서버별 요청 최대 크기는 상이하나, nbdkit 표준 한계를 가정한다(64MB).
 * queue()에서 assert로 xfer_buflen 초과 여부를 체크. */
#define NBD_MAX_REQUEST_SIZE (64 * 1024 * 1024)

/*
 * [한국어] NBD 엔진의 잡별 상태. td->io_ops_data가 가리킴.
 */
struct nbd_data {
	struct nbd_handle *nbd;
	/* [한국어] libnbd 커넥션 핸들. 서버 상태머신/소켓/AIO 큐를 라이브러리가 관리.
	 * 설정자: nbd_setup/nbd_init의 nbd_create+nbd_connect_uri. 읽는 자: queue/getevents 등 전 경로.
	 * 값 범위: 유효 핸들 또는 NULL(setup 종료 후/cleanup 후).
	 * 동기화: 잡 스레드 단독 소유. */

	int debug;
	/* [한국어] libnbd 디버그 로그 활성화 플래그(LIBNBD_DEBUG=1 환경변수).
	 * 설정자: nbd_setup에서 nbd_get_debug 결과 저장.
	 * 읽는 자: 부가 log_info 분기. */

	/* [한국어] 완료된 io_u 구조체 리스트 — 완료 콜백이 push, event()가 pop. */
	struct io_u **completed;
	/* [한국어] 완료 io_u 동적 배열(realloc으로 확장).
	 * 설정자: cmd_completed()가 realloc+append. 읽는 자: nbd_event가 역순 pop. */
	size_t nr_completed;
	/* [한국어] completed 배열 유효 길이.
	 * 값 범위: 0..iodepth. 동기화: libnbd 콜백은 nbd_poll 컨텍스트에서 호출되므로
	 * getevents 스레드와 동일 스레드에서 실행되어 락 불필요. */
};

/*
 * [한국어] NBD 엔진 옵션 구조체. option_struct_size로 코어가 잡마다 이 크기만큼
 * calloc하여 보관하고, options[] 테이블의 off1로 각 필드가 채워진다.
 */
struct nbd_options {
	void *padding;
	/* [한국어] off1==0 회피용 더미 포인터.
	 * 설정자: 없음(옵션 파서는 이 필드에 기록하지 않음).
	 * 읽는 자: 없음. uri 필드의 offsetof()를 non-zero로 만드는 구조적 역할.
	 * 값 범위: 미정의(calloc 0).
	 * 동기화: 읽히지 않음. */

	char *uri;
	/* [한국어] NBD 서버 접속 URI 문자열. 예: "nbd://host:10809", "nbd+unix:///path/sock",
	 *         "nbds://host"(TLS). libnbd가 스킴별로 TCP/Unix 소켓/TLS 협상을 판별.
	 * 설정자: fio 옵션 파서가 "uri=..." 값을 FIO_OPT_STR_STORE로 할당(strdup 내부).
	 * 읽는 자: nbd_setup() 프로브 연결, nbd_init() 잡당 본 연결.
	 * 값 범위: NULL(미지정 시 에러 처리) 또는 유효 NBD URI.
	 * 동기화: 잡 시작 이후 불변, 다중 잡이 같은 URI를 읽을 수 있으나 libnbd 핸들은 잡당 분리. */
};

static struct fio_option options[] = {
	{
		.name	= "uri",                                  /* [한국어] 옵션 이름 — 잡 파일/CLI에서 "uri=..." 형태로 지정. */
		.lname	= "NBD URI",                              /* [한국어] help 출력용 긴 이름. */
		.help	= "Name of NBD URI",                       /* [한국어] --enghelp=nbd 도움말 문구. */
		.category = FIO_OPT_C_ENGINE,                      /* [한국어] 엔진 전용 옵션 카테고리. */
		.group	= FIO_OPT_G_NBD,                          /* [한국어] NBD 엔진 옵션 그룹(optgroup.h). */
		.type	= FIO_OPT_STR_STORE,                       /* [한국어] 문자열 저장 타입 — 파서가 strdup 후 포인터 기록. */
		.off1	= offsetof(struct nbd_options, uri),      /* [한국어] nbd_options.uri 필드 오프셋 — padding 덕에 non-zero. */
	},
	{
		.name	= NULL,                                    /* [한국어] 옵션 테이블 종료 센티널. */
	},
};

/*
 * [한국어]
 * nbd_setup - ioengine_ops.setup 콜백. 잡 파싱 직후 1회 호출되어 nbd_data 할당과
 *            서버 크기 프로브(가짜 파일 등록 포함)를 수행한다. 실제 잡 스레드별 연결은
 *            nbd_init에서 다시 수행.
 * @return: 0 성공, 1 실패.
 * 호출 체인: fio_backend → td_io_init(초기 단계) → ioengine_ops.setup → [nbd_setup].
 */
/* Allocates nbd_data. */
static int nbd_setup(struct thread_data *td)
{
	struct nbd_data *nbd_data;
	struct nbd_options *o = td->eo;
	struct fio_file *f;
	int r;
	int64_t size;

	nbd_data = calloc(1, sizeof(*nbd_data));   /* [한국어] 잡별 상태 0-할당 */
	if (!nbd_data) {
		td_verror(td, errno, "calloc");
		return 1;
	}
	td->io_ops_data = nbd_data;                  /* [한국어] td에 부착 */

	/* Pretend to deal with files.	See engines/rbd.c */
	/* [한국어] NBD는 POSIX 파일이 없지만, fio 코어는 최소 1개 파일을 요구한다.
	 * rbd.c의 트릭을 따라 "nbd"라는 가상 파일을 추가한다 */
	if (!td->files_index) {
		add_file(td, "nbd", 0, 0);
		td->o.nr_files = td->o.nr_files ? : 1;   /* [한국어] 미지정이면 1로 */
		td->o.open_files++;
	}
	f = td->files[0];

	nbd_data->nbd = nbd_create();                /* [한국어] libnbd 핸들 생성 */
	if (!nbd_data->nbd) {
		log_err("fio: nbd_create: %s\n", nbd_get_error());
		return 1;
	}

	/* Get the debug flag which can be set through LIBNBD_DEBUG=1. */
	nbd_data->debug = nbd_get_debug(nbd_data->nbd);   /* [한국어] 디버그 모드 캐시 */

	/* Connect synchronously here so we can check for the size and
	 * in future other properties of the server.
	 */
	/* [한국어] setup 단계는 동기 연결 후 size만 얻고 즉시 close. 실제 I/O 연결은 init에서 */
	if (!o->uri) {
		log_err("fio: nbd: uri parameter was not specified\n");
		return 1;
	}
	r = nbd_connect_uri(nbd_data->nbd, o->uri);  /* [한국어] URI 기반 동기 연결 + NBD 핸드쉐이크 */
	if (r == -1) {
		log_err("fio: nbd_connect_uri: %s\n", nbd_get_error());
		return 1;
	}
	size = nbd_get_size(nbd_data->nbd);          /* [한국어] 서버에서 내보낸 블록 장치 크기 */
	if (size == -1) {
		log_err("fio: nbd_get_size: %s\n", nbd_get_error());
		return 1;
	}

	f->real_file_size = size;                     /* [한국어] 가짜 파일의 실제 크기 필드에 반영 */

	nbd_close (nbd_data->nbd);                    /* [한국어] 프로브용 연결 종료 — init에서 재연결 */
	nbd_data->nbd = NULL;

	return 0;
}

/*
 * [한국어]
 * nbd_cleanup - 잡 종료 시 소켓을 닫고 nbd_data 해제. setup의 역.
 * 호출 체인: td_io_cleanup → [nbd_cleanup].
 */
/* Closes socket and frees nbd_data -- the opposite of nbd_setup. */
static void nbd_cleanup(struct thread_data *td)
{
	struct nbd_data *nbd_data = td->io_ops_data;

	if (nbd_data) {
		if (nbd_data->nbd)
			nbd_close(nbd_data->nbd);   /* [한국어] 라이브 연결 존재 시 graceful close */
		free(nbd_data);
	}
}

/*
 * [한국어]
 * nbd_init - 각 잡 스레드가 실제 NBD 연결을 수립하는 콜백.
 * 호출 체인: td_io_init → [nbd_init] → libnbd::nbd_connect_uri.
 */
/* Connect to the server from each thread. */
static int nbd_init(struct thread_data *td)
{
	struct nbd_options *o = td->eo;                /* [한국어] uri 옵션 */
	struct nbd_data *nbd_data = td->io_ops_data;   /* [한국어] setup에서 할당된 상태 */
	int r;

	if (!o->uri) {
		log_err("fio: nbd: uri parameter was not specified\n");
		return 1;
	}

	nbd_data->nbd = nbd_create();   /* [한국어] 이 잡 스레드 전용 핸들 새로 생성 */
	if (!nbd_data->nbd) {
		log_err("fio: nbd_create: %s\n", nbd_get_error());
		return 1;
	}
	/* This is actually a synchronous connect and handshake. */
	/* [한국어] URI 기반 동기 연결 — 이후 모든 aio 호출은 이 핸들을 사용 */
	r = nbd_connect_uri(nbd_data->nbd, o->uri);
	if (r == -1) {
		log_err("fio: nbd_connect_uri: %s\n", nbd_get_error());
		return 1;
	}

	log_info("fio: connected to NBD server\n");
	return 0;
}

/*
 * [한국어]
 * cmd_completed - libnbd가 비동기 요청 완료 시 콜백으로 호출. 이 콜백은 nbd_poll
 *                컨텍스트(동일 잡 스레드) 내부에서 동기적으로 실행되므로 락이 필요 없다.
 * @vp:    io_u (제출 시 completion.user_data로 넘긴 값).
 * @error: libnbd가 보고하는 에러 코드 포인터.
 * @return: 0 — libnbd에 "콜백 유지"를 뜻함(재사용 방지).
 * 호출 체인: nbd_poll → libnbd 내부 → [cmd_completed] → nbd_data->completed 배열에 push.
 */
/* A command in flight has been completed. */
static int cmd_completed (void *vp, int *error)
{
	struct io_u *io_u;
	struct nbd_data *nbd_data;
	struct io_u **completed;

	io_u = vp;                           /* [한국어] user_data 캐스팅 */
	nbd_data = io_u->engine_data;        /* [한국어] queue()에서 심어둔 역링크 */

	if (nbd_data->debug)
		log_info("fio: nbd: command completed\n");

	if (*error != 0)
		io_u->error = *error;         /* [한국어] NBD 서버/네트워크 에러 전달 */
	else
		io_u->error = 0;               /* [한국어] 성공 */

	/* Add this completion to the list so it can be picked up
	 * later by ->event.
	 */
	/* [한국어] completed 배열 확장(realloc). 실패 시 io_u에 errno 기록하고 유실 — 희귀 경로 */
	completed = realloc(nbd_data->completed,
			    sizeof(struct io_u *) *
			    (nbd_data->nr_completed+1));
	if (completed == NULL) {
		io_u->error = errno;
		return 0;
	}

	nbd_data->completed = completed;
	nbd_data->completed[nbd_data->nr_completed] = io_u;  /* [한국어] tail append */
	nbd_data->nr_completed++;

	return 0;
}

/*
 * [한국어]
 * nbd_queue - 비동기 I/O 발행. ddir에 따라 aio_pread/pwrite/trim/flush 중 하나를 호출.
 * @return: FIO_Q_QUEUED(성공), FIO_Q_COMPLETED(에러/즉시 완료).
 * 호출 체인: td_io_queue → [nbd_queue] → libnbd::nbd_aio_*.
 */
/* Begin read or write request. */
static enum fio_q_status nbd_queue(struct thread_data *td,
				   struct io_u *io_u)
{
	struct nbd_data *nbd_data = td->io_ops_data;
	/* [한국어] libnbd 완료 콜백 바인딩 구조체. 콜백은 cmd_completed, user_data는 io_u */
	nbd_completion_callback completion = { .callback = cmd_completed,
					       .user_data = io_u };
	int r;

	fio_ro_check(td, io_u);                /* [한국어] readonly 잡 단속 */

	io_u->engine_data = nbd_data;           /* [한국어] cmd_completed에서 역참조용 링크 저장 */

	/* [한국어] 서버(NBDkit 기본 64MB) 제한 초과 방지 — 잡 구성 오류 방어 */
	if (io_u->ddir == DDIR_WRITE || io_u->ddir == DDIR_READ)
		assert(io_u->xfer_buflen <= NBD_MAX_REQUEST_SIZE);

	switch (io_u->ddir) {
	case DDIR_READ:
		/* [한국어] 비동기 읽기 — 버퍼/길이/오프셋과 완료 콜백 등록 */
		r = nbd_aio_pread(nbd_data->nbd,
				  io_u->xfer_buf, io_u->xfer_buflen,
				  io_u->offset, completion, 0);
		break;
	case DDIR_WRITE:
		/* [한국어] 비동기 쓰기 */
		r = nbd_aio_pwrite(nbd_data->nbd,
				   io_u->xfer_buf, io_u->xfer_buflen,
				   io_u->offset, completion, 0);
		break;
	case DDIR_TRIM:
		/* [한국어] 비동기 trim(discard) — 버퍼 불필요 */
		r = nbd_aio_trim(nbd_data->nbd, io_u->xfer_buflen,
				 io_u->offset, completion, 0);
		break;
	case DDIR_SYNC:
		/* XXX We could probably also handle
		 * DDIR_SYNC_FILE_RANGE with a bit of effort.
		 */
		/* [한국어] NBD flush 명령 */
		r = nbd_aio_flush(nbd_data->nbd, completion, 0);
		break;
	default:
		io_u->error = EINVAL;   /* [한국어] 미지원 방향 — 즉시 에러 완료 */
		return FIO_Q_COMPLETED;
	}

	if (r == -1) {
		/* errno is optional information on libnbd error path;
		 * if it's 0, set it to a default value
		 */
		/* [한국어] libnbd 제출 단계에서 바로 실패 — errno 추출, 0이면 EIO 대체 */
		io_u->error = nbd_get_errno();
		if (io_u->error == 0)
			io_u->error = EIO;
		return FIO_Q_COMPLETED;
	}

	if (nbd_data->debug)
		log_info("fio: nbd: command issued\n");
	io_u->error = 0;               /* [한국어] 제출 성공 — 실제 결과는 콜백에서 결정 */
	return FIO_Q_QUEUED;            /* [한국어] 비동기 큐잉 상태 통지 */
}

/*
 * [한국어]
 * retire_commands - libnbd 내부 완료 큐에서 cookie들을 꺼내 회수(retire)하고 개수 반환.
 *                  실제 io_u 상태는 cmd_completed에서 이미 세팅됐으므로 여기선 카운트만.
 * @return: 회수된 명령 수.
 */
static unsigned retire_commands(struct nbd_handle *nbd)
{
	int64_t cookie;
	unsigned r = 0;

	while ((cookie = nbd_aio_peek_command_completed(nbd)) > 0) {
		/* Ignore the return value.  cmd_completed has already
		 * checked for an error and set io_u->error.  We only
		 * have to call this to retire the command.
		 */
		nbd_aio_command_completed(nbd, cookie);   /* [한국어] 내부 완료 슬롯 해제 */
		r++;
	}

	if (nbd_get_debug(nbd))
		log_info("fio: nbd: %u commands retired\n", r);
	return r;
}

/*
 * [한국어]
 * nbd_getevents - 완료 이벤트를 min 이상 될 때까지 nbd_poll로 진행시킨 뒤 회수.
 * @t: 타임아웃(ms 환산). NULL이면 무한 대기(-1).
 * 호출 체인: td_io_getevents → [nbd_getevents] → libnbd::nbd_poll → cmd_completed.
 */
static int nbd_getevents(struct thread_data *td, unsigned int min,
			 unsigned int max, const struct timespec *t)
{
	struct nbd_data *nbd_data = td->io_ops_data;
	int r;
	unsigned events = 0;
	int timeout;

	/* XXX This handling of timeout is wrong because it will wait
	 * for up to loop iterations * timeout.
	 */
	/* [한국어] 타임아웃을 ms로 변환(nbd_poll 규약). NULL이면 -1(무한). */
	timeout = !t ? -1 : t->tv_sec * 1000 + t->tv_nsec / 1000000;

	while (events < min) {
		r = nbd_poll(nbd_data->nbd, timeout);   /* [한국어] I/O 상태머신 진행(+완료 콜백 실행) */
		if (r == -1) {
			/* error in poll */
			log_err("fio: nbd_poll: %s\n", nbd_get_error());
			return -1;
		}
		else {
			/* poll made progress */
			events += retire_commands(nbd_data->nbd);   /* [한국어] 완료된 커맨드 회수 */
		}
	}

	return events;   /* [한국어] 실제 누적 완료 수 반환 */
}

/*
 * [한국어]
 * nbd_event - 수확된 완료 중 하나를 반환. fio 코어가 getevents 반환값 N에 대해
 *            0..N-1 인덱스로 1회씩 호출한다. 내부 구현은 인덱스를 무시하고 LIFO pop
 *            — nr_completed가 제출 순서와 맞지 않아도 단지 "완료 io_u 하나"를 주면 된다.
 * @td: 잡 컨텍스트.
 * @event: fio 코어가 전달하는 인덱스(무시).
 * @return: 완료된 io_u 포인터 또는 NULL.
 * 호출 체인: do_io → io_u_queued_complete → td_io_event → [nbd_event].
 */
static struct io_u *nbd_event(struct thread_data *td, int event)
{
	struct nbd_data *nbd_data = td->io_ops_data; /* [한국어] 잡 상태 역참조. */

	if (nbd_data->nr_completed == 0)
		return NULL; /* [한국어] 완료 큐 비어 있음 — 방어적 반환(정상 경로에선 getevents 이후 N>0). */

	/* XXX We ignore the event number and assume fio calls us
	 * exactly once for [0..nr_events-1].
	 */
	nbd_data->nr_completed--;                      /* [한국어] LIFO pop — 말단부터 꺼냄. */
	return nbd_data->completed[nbd_data->nr_completed]; /* [한국어] 해당 슬롯의 io_u 포인터 반환. */
}

/*
 * [한국어]
 * nbd_io_u_init - io_u 생성 시 엔진 고유 필드 초기화.
 * @return: 0 항상 성공.
 * engine_data는 queue()에서 nbd_data 포인터로 덮어쓰고, cmd_completed가 역참조에 사용.
 * 호출 체인: io_u 할당 경로(backend.c) → td_io_ops->io_u_init → [nbd_io_u_init].
 */
static int nbd_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	io_u->engine_data = NULL; /* [한국어] 안전한 초기값 — queue() 진입 전 잘못된 포인터 참조 방지. */
	return 0;
}

/*
 * [한국어] nbd_io_u_free - io_u 해제 시 호출되는 훅.
 * 엔진이 per-io_u로 할당한 리소스가 없으므로 본문 비어 있음.
 * 호출 체인: io_u_free 경로 → td_io_ops->io_u_free → [nbd_io_u_free].
 */
static void nbd_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	/* Nothing needs to be done. */ /* [한국어] 해제할 엔진 자원 없음 — no-op. */
}

/*
 * [한국어] nbd_open_file - fio 코어가 가상 파일에 대해 open을 호출할 때의 훅.
 * FIO_DISKLESSIO 엔진은 실제 파일이 없으므로 no-op 성공을 반환.
 * 호출 체인: td_io_open_file → [nbd_open_file].
 */
static int nbd_open_file(struct thread_data *td, struct fio_file *f)
{
	return 0; /* [한국어] 연결은 이미 nbd_init에서 수립했으므로 추가 작업 없음. */
}

/*
 * [한국어] nbd_invalidate - 로컬 페이지 캐시 무효화 훅.
 * NBD는 서버 측 블록 장치이며 로컬 페이지 캐시를 사용하지 않으므로 no-op.
 * 호출 체인: td_io_setup 경로에서 invalidate=1 옵션 시 호출.
 */
static int nbd_invalidate(struct thread_data *td, struct fio_file *f)
{
	return 0; /* [한국어] NBD는 로컬 캐시 없음 — no-op. */
}

/*
 * [한국어] NBD 엔진의 ioengine_ops 테이블.
 * 비동기 엔진 계약을 가진 구조체로, queue()가 FIO_Q_QUEUED 반환 후 getevents+event로
 * 완료 분리 수확을 지원한다. FIO_DISKLESSIO로 파일 경로 없이도 동작.
 * ioengines.c::load_ioengine("nbd")가 이 구조체를 td->io_ops에 바인딩.
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "nbd",
	/* [한국어] --ioengine=nbd에서 사용될 식별자. engine_list 검색 키. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] fio 코어↔엔진 ABI 버전. 불일치 시 로드 거부. */

	.options		= options,
	/* [한국어] 위의 uri 옵션 테이블 — 코어 옵션 파서가 참조. */

	.option_struct_size	= sizeof(struct nbd_options),
	/* [한국어] 잡당 옵션 저장용 메모리 크기 — 코어가 calloc하여 off1로 필드 기록. */

	.flags			= FIO_DISKLESSIO | FIO_NOEXTEND,
	/* [한국어] 엔진 속성 비트:
	 *   FIO_DISKLESSIO — 로컬 파일이 없어도 동작하는 엔진(네트워크 백엔드).
	 *                   fio 코어의 파일 통계/filesetup 경로에서 파일 크기·존재 확인을 생략하도록 표시.
	 *   FIO_NOEXTEND   — 서버가 내보낸 export 크기를 넘어서 확장 불가(NBD는 고정 용량).
	 * getevents/event를 구현하므로 FIO_SYNCIO는 없음 → 비동기 계약. */

	.setup			= nbd_setup,
	/* [한국어] 잡 파싱 직후 1회 호출 — nbd_data 할당 + "nbd" 가상 파일 add_file + 동기 프로브 연결로
	 *          export size 취득 후 close. 호출자: td_io_init 초기 단계. */

	.init			= nbd_init,
	/* [한국어] 각 잡 스레드가 본 AIO 연결을 수립(nbd_create + nbd_connect_uri). */

	.cleanup		= nbd_cleanup,
	/* [한국어] 잡 종료 시 nbd_close + free(nbd_data). init/setup과 대칭. */

	.queue			= nbd_queue,
	/* [한국어] ddir에 따라 aio_pread/pwrite/trim/flush 발행. 성공 시 FIO_Q_QUEUED.
	 *          XFER 상한은 NBD_MAX_REQUEST_SIZE(64MiB) — assert로 강제. */

	.getevents		= nbd_getevents,
	/* [한국어] nbd_poll로 상태머신을 진행시키고 retire_commands로 완료 cookie 회수 후 개수 반환.
	 *          min까지 블로킹. timeout은 ms 단위로 변환. */

	.event			= nbd_event,
	/* [한국어] nr_completed에서 LIFO pop하여 완료 io_u 반환. 인덱스는 무시(코어 규약상 1회씩 호출). */

	.io_u_init		= nbd_io_u_init,
	/* [한국어] io_u 생성 시 engine_data=NULL 초기화. queue()에서 nbd_data 역링크로 덮어씀. */

	.io_u_free		= nbd_io_u_free,
	/* [한국어] io_u 해제 시 — 엔진이 malloc한 per-iou 상태 없음, no-op. */

	.open_file		= nbd_open_file,
	/* [한국어] FIO_DISKLESSIO이므로 실제 open 불필요 — no-op 성공. */

	.invalidate		= nbd_invalidate,
	/* [한국어] 로컬 페이지 캐시 무효화 개념 없음 — no-op. */
};

/*
 * [한국어] constructor 속성 — fio 바이너리 링크 시 자동 호출되어 NBD 엔진을
 * engine_list에 등록. 이후 --ioengine=nbd가 find_ioengine으로 조회 가능해짐.
 */
static void fio_init fio_nbd_register(void)
{
	register_ioengine(&ioengine); /* [한국어] engine_list에 tail-add — 초기화 단계라 락 불필요. */
}

/*
 * [한국어] destructor 속성 — 프로세스 종료 직전 engine_list에서 제거.
 */
static void fio_exit fio_nbd_unregister(void)
{
	unregister_ioengine(&ioengine); /* [한국어] flist_del_init 해제. */
}
