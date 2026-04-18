/*
 * [한국어 설명] DAOS File System (DFS) I/O 엔진 구현 (dfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Intel DAOS(Distributed Asynchronous Object Storage) 분산 스토리지의
 * POSIX-유사 파일 시스템 레이어인 libdfs 위에서 동작하는 fio I/O 엔진을 구현한다.
 * fio 잡은 DAOS 풀(pool: 스토리지 할당 단위) → 컨테이너(container: 네임스페이스
 * 단위) → DFS 마운트(dfs_t) → 파일 오브젝트(dfs_obj_t) 체인으로 접근 경로를 만들고,
 * dfs_read/dfs_write에 DAOS 비동기 이벤트(daos_event_t)를 붙여 I/O를 제출한다.
 * 완료는 프로세스 공유 이벤트 큐(EQ, daos_handle_t eqh)에서 daos_eq_poll로 수확한다.
 * 오브젝트 클래스(cid, 예: OC_SX/OC_RP_2G1)와 청크 크기(chunk_size)로 데이터
 * 분산/복제 정책을 지정할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 엔진 플러그인 계약에서 setup→init(풀/컨테이너/DFS 마운트)→open_file→
 * (prep→queue→commit→getevents→event 반복)→close_file→unlink_file→cleanup 순서로
 * 호출된다. 본 엔진은 queue()에서 dfs_read/dfs_write를 즉시 호출하되 daos_event_t로
 * 비동기 완료를 약속받고 FIO_Q_QUEUED를 반환하는 전형적 aio 패턴을 취한다.
 * 그 뒤 fio 코어가 getevents()를 호출해 daos_eq_poll()로 완료 개수를 받고,
 * event()로 개별 io_u를 fio에 되돌려 준다. 실행 컨텍스트는 각 잡 스레드(유저스페이스)
 * 이며, 클라이언트 측 libdaos가 RPC로 DAOS 서버(storage engine)와 통신한다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위: ioengines.c의 전체 콜백 세트(setup/init/prep/queue/commit 생략-
 *   getevents/event/cleanup/open_file/close_file/unlink_file/invalidate/
 *   get_file_size, io_u_init/io_u_free). fio 코어(backend.c)가 잡 스레드에서 호출.
 * - 하위: libdaos(daos_init/daos_pool_connect/daos_cont_open/daos_eq_create/
 *   daos_eq_poll/daos_event_init/daos_event_fini) 및 libdfs(dfs_mount/dfs_open/
 *   dfs_read/dfs_write/dfs_stat/dfs_release/dfs_remove/dfs_umount).
 * - 프로세스 전역 상태: daos_initialized, num_threads, daos_mutex, poh/coh/cid/
 *   daosfs를 여러 잡(같은 프로세스 내 thread_data)이 공유하여 DAOS 연결을 1회만
 *   수립·해제한다 — init/cleanup에서 뮤텍스로 동시 초기화 레이스를 방지한다.
 * - 공유 자료: thread_data::io_ops_data에 struct daos_data(잡별 EQ/dfs_obj/큐),
 *   io_u::engine_data에 struct daos_iou(개별 요청 컨텍스트) 저장. 완료 이벤트와
 *   io_u 매핑은 container_of(evp, struct daos_iou, ev) 패턴으로 복원한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - daos_fio_global_init/cleanup: DAOS 스택 초기화/종료, 풀 연결·컨테이너 열기·DFS 마운트.
 *   프로세스 최초 호출 잡 1회만 수행하며 daos_mutex+daos_initialized로 가드한다.
 * - daos_fio_init/cleanup: 잡별 EQ 생성/파기, 전역 초기화 호출자 제어(num_threads 카운팅).
 * - daos_fio_queue: dfs_read/dfs_write에 daos_event_t를 붙여 비동기 제출,
 *   FIO_Q_QUEUED 반환(즉시 실패는 FIO_Q_COMPLETED+io_u->error).
 * - daos_fio_getevents/event: daos_eq_poll로 완료 이벤트 수집, container_of로 io_u 복원.
 * - daos_fio_open/close/unlink: dfs_open/dfs_release/dfs_remove 래핑(파일 단위).
 * - 전역 poh/coh/cid/daosfs/daos_mutex: 잡 간 DAOS 핸들 공유 및 초기화 레이스 방지.
 * - struct daos_iou: io_u당 1개, daos_event_t + sgl/iov + 완료플래그.
 * - struct daos_data: 잡당 1개, EQ 핸들 + 오픈된 dfs_obj + 완료 io_u 배열.
 */

/**
 * FIO engine for DAOS File System (dfs).
 *
 * (C) Copyright 2020-2021 Intel Corporation.
 */

#include <fio.h>       /* [한국어] fio 코어 타입/매크로(thread_data, io_u, ioengine_ops, FIO_Q_*, dprint, td_verror 등) */
#include <optgroup.h>  /* [한국어] FIO_OPT_G_DFS 등 엔진별 옵션 그룹 정의 */

#include <daos.h>      /* [한국어] libdaos 코어 API — daos_init/daos_pool_connect/daos_cont_open/daos_eq_* 등 */
#include <daos_fs.h>   /* [한국어] libdfs API — dfs_mount/dfs_open/dfs_read/dfs_write/dfs_stat/dfs_remove 등 POSIX-유사 FS 레이어 */

/* [한국어] 프로세스 전역: DAOS 스택이 초기화되었는지 여부. 여러 잡이 같은 프로세스에서
 *          DAOS 연결을 재사용하므로, 최초 진입 잡만 daos_init()+풀/컨테이너 연결을
 *          수행하도록 가드한다. 설정자/리셋자 모두 daos_mutex 보호 하에 수정. */
static bool		daos_initialized;
/* [한국어] 현재 살아있는(해당 엔진을 사용 중인) 잡 스레드 수. init에서 증가, cleanup에서
 *          감소, 0이 되는 순간 마지막 잡이 전역 cleanup(DAOS finalize)까지 수행한다. */
static int		num_threads;
/* [한국어] 전역 상태(daos_initialized/num_threads/poh/coh/cid/daosfs) 보호용 프로세스 뮤텍스.
 *          여러 잡 스레드가 동시에 init/cleanup에 들어와 레이스를 일으키지 않도록 직렬화. */
static pthread_mutex_t	daos_mutex = PTHREAD_MUTEX_INITIALIZER;
daos_handle_t		poh;  /* pool handle */
/* [한국어] ↑ DAOS 풀 핸들(불투명 구조체). daos_pool_connect 성공 시 채워지며 전 잡이 공유.
 *          cleanup 시 daos_pool_disconnect로 반환. */
daos_handle_t		coh;  /* container handle */
/* [한국어] ↑ DAOS 컨테이너 핸들(불투명). daos_cont_open 성공 시 채워지며 전 잡이 공유.
 *          컨테이너는 DFS 네임스페이스의 루트를 담는 용기. */
daos_oclass_id_t	cid = OC_UNKNOWN;  /* object class */
/* [한국어] ↑ DAOS 오브젝트 클래스 ID. 옵션(oclass)이 주어지면 daos_oclass_name2id로
 *          설정, 미지정 시 OC_UNKNOWN(컨테이너 기본값 사용). 복제·EC 정책을 결정. */
dfs_t			*daosfs; /* dfs mount reference */
/* [한국어] ↑ DFS 마운트 핸들(dfs_mount 반환). 모든 파일 open/read/write/remove의
 *          첫 번째 인자로 전달되는 컨테이너 단위 파일시스템 컨텍스트. */

/* [한국어] DAOS I/O 유닛 확장 데이터 - io_u->engine_data에 저장 */
struct daos_iou {
	struct io_u	*io_u;		/* [한국어] 대응하는 fio io_u */
	/* [한국어] 이 DAOS I/O 요청과 짝이 되는 fio io_u 포인터.
	 * 설정자: daos_fio_io_u_init()에서 최초 설정(io_u마다 1회).
	 * 읽는 자: daos_fio_getevents()가 완료 이벤트를 io_u로 역매핑할 때 참조.
	 * 값 범위: 유효한 io_u 포인터(NULL 불가). io_u 해제 전까지 유효.
	 * 동기화: 한 io_u는 단일 잡 스레드에서만 다뤄지므로 별도 락 불필요. */
	daos_event_t	ev;		/* [한국어] DAOS 비동기 이벤트 핸들 */
	/* [한국어] DAOS 비동기 I/O의 완료 이벤트 핸들(libdaos 제공, 불투명 구조체).
	 * 설정자: daos_fio_queue()에서 daos_event_init(ev, eqh, NULL)로 EQ에 바인딩.
	 * 읽는 자: daos_eq_poll()이 반환한 daos_event_t* 포인터를 container_of로
	 *          struct daos_iou로 복원할 때 사용(멤버 오프셋 기준).
	 * 값 범위: libdaos 내부 관리 — 직접 필드 접근 금지. ev_error로 에러 전달.
	 * 동기화: 이벤트 큐(eqh)에 바인딩되어 있으며 eq 단위 락은 libdaos가 관리. */
	d_sg_list_t	sgl;		/* [한국어] scatter/gather 리스트 (DAOS I/O 버퍼 기술자) */
	/* [한국어] DAOS가 받는 scatter/gather 리스트. sg_nr(벡터 개수)=1, sg_iovs=&iov로
	 *          단일 버퍼만 사용. 설정자: queue() 직전. 읽는 자: libdfs(dfs_read/write). */
	d_iov_t		iov;		/* [한국어] I/O 벡터 (단일 버퍼 포인터+길이) */
	/* [한국어] 단일 DMA/메모리 버퍼 기술자. d_iov_set(&iov, buf, len)으로 io_u->xfer_buf/
	 *          xfer_buflen과 연결. sgl.sg_iovs가 이 필드를 가리킨다. */
	daos_size_t	size;		/* [한국어] 읽기 완료 시 실제 전송된 크기 */
	/* [한국어] 읽기(in/out) 파라미터: 호출 전에는 요청 크기, 호출 후에는 실제 읽힌 바이트.
	 *          쓰기는 사용하지 않음. resid 계산에는 사용하지 않고(코드상 resid=0으로
	 *          단순 처리) libdfs의 내부 상태 추적용. */
	bool		complete;	/* [한국어] I/O 완료 여부 */
	/* [한국어] 중복 완료 디버그 플래그. queue()에서 false, getevents()에서 true로 전이.
	 *          true인데 다시 완료 이벤트가 들어오면 libdaos/libdfs 버그 경고를 로그. */
};

/* [한국어] DAOS/DFS 엔진의 내부 상태 구조체 */
struct daos_data {
	daos_handle_t	eqh;		/* [한국어] DAOS 이벤트 큐 핸들 (비동기 I/O 완료 추적) */
	/* [한국어] 잡별 DAOS 이벤트 큐 핸들. daos_eq_create로 init에서 생성, cleanup에서
	 *          daos_eq_destroy(DESTROY_FORCE)로 파기. queue()에서 daos_event_init의
	 *          두 번째 인자로 사용되어 이벤트가 이 EQ에 등록되고, getevents()는
	 *          daos_eq_poll(eqh, ...)로 완료를 수확한다. 잡 스레드 단독 소유. */
	dfs_obj_t	*obj;		/* [한국어] DFS 파일 오브젝트 핸들 */
	/* [한국어] 현재 오픈된 DFS 파일 오브젝트 포인터. open_file에서 dfs_open으로 채워지고
	 *          close_file에서 dfs_release로 반환. dfs_read/dfs_write의 두 번째 인자.
	 *          단일 파일을 가정(한 잡이 여러 파일 동시 오픈 시 제약 있음). */
	struct io_u	**io_us;	/* [한국어] 완료된 io_u 배열 */
	/* [한국어] getevents()가 수집한 완료 io_u들을 순서대로 채워 넣는 배열(크기=iodepth).
	 *          event(i)가 이 배열의 i번째를 fio 코어에 반환. 새 폴링 라운드마다 덮어씀. */
	int		queued;		/* [한국어] 큐에 대기 중인 I/O 수 */
	/* [한국어] 현재 EQ에서 완료를 기다리는 in-flight I/O 수. queue()에서 ++,
	 *          getevents()에서 완료 1건당 --. queued==iodepth면 queue가 FIO_Q_BUSY 반환. */
	int		num_ios;	/* [한국어] 수집된 완료 이벤트 수 */
	/* [한국어] io_us 배열 용량(=iodepth). init에서 세팅되며 getevents의 max 인자로 사용. */
};

/* [한국어] dfs 엔진의 잡 옵션 저장 구조체. thread_data->eo에 할당되며 options[]의 off1으로 바인딩. */
struct daos_fio_options {
	void		*pad;
	/* [한국어] fio 옵션 매크로가 오프셋 0을 쓰지 않도록 두는 패딩(옵션 프레임워크 관례). */
	char		*pool;   /* Pool UUID */
	/* [한국어] DAOS 풀 UUID 또는 라벨 문자열. 필수 옵션. daos_pool_connect에 전달. */
	char		*cont;   /* Container UUID */
	/* [한국어] DAOS 컨테이너 UUID 또는 라벨. 필수 옵션. daos_cont_open에 전달. */
	daos_size_t	chsz;    /* Chunk size */
	/* [한국어] DFS 청크 크기(바이트). 0이면 컨테이너 기본값 사용. dfs_open 시 전달되어
	 *          스트라이프/분산 단위 결정. */
	char		*oclass; /* object class */
	/* [한국어] 오브젝트 클래스 이름 문자열(예: "SX", "RP_2G1"). daos_oclass_name2id로
	 *          cid(전역)에 저장되어 dfs_open의 파일 생성 시 복제/EC 정책 지정. */
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
	char		*svcl;   /* service replica list, deprecated */
	/* [한국어] DAOS API 1.0 이전: 풀 서비스 복제 랭크 리스트(":" 구분). 1.0+에서는
	 *          풀 라벨/UUID만으로 자동 조회되므로 제거됨. */
#endif
};

/* [한국어] fio 옵션 파서용 테이블. 사용자가 --pool=<> 등으로 지정하면 off1 오프셋에 저장됨. */
static struct fio_option options[] = {
	{
		.name		= "pool",                                           /* [한국어] 옵션 키 */
		.lname		= "pool uuid or label",                            /* [한국어] 긴 이름(도움말용) */
		.type		= FIO_OPT_STR_STORE,                               /* [한국어] 문자열 저장 타입 */
		.off1		= offsetof(struct daos_fio_options, pool),          /* [한국어] 저장 위치 */
		.help		= "DAOS pool uuid or label",
		.category	= FIO_OPT_C_ENGINE,                                 /* [한국어] 엔진 옵션 카테고리 */
		.group		= FIO_OPT_G_DFS,                                    /* [한국어] DFS 엔진 그룹 */
	},
	{
		.name           = "cont",                                           /* [한국어] 컨테이너 옵션 */
		.lname          = "container uuid or label",
		.type           = FIO_OPT_STR_STORE,
		.off1           = offsetof(struct daos_fio_options, cont),
		.help           = "DAOS container uuid or label",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_DFS,
	},
	{
		.name           = "chunk_size",                                     /* [한국어] DFS 청크 크기 */
		.lname          = "DFS chunk size",
		.type           = FIO_OPT_ULL,                                      /* [한국어] unsigned long long 파싱 */
		.off1           = offsetof(struct daos_fio_options, chsz),
		.help           = "DFS chunk size in bytes",
		.def		= "0", /* use container default */                      /* [한국어] 0=컨테이너 기본값 */
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_DFS,
	},
	{
		.name           = "object_class",                                   /* [한국어] 오브젝트 클래스 이름 */
		.lname          = "object class",
		.type           = FIO_OPT_STR_STORE,
		.off1           = offsetof(struct daos_fio_options, oclass),
		.help           = "DAOS object class",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_DFS,
	},
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
	{
		.name           = "svcl",                                           /* [한국어] 구버전 API: 서비스 랭크 리스트 */
		.lname          = "List of service ranks",
		.type           = FIO_OPT_STR_STORE,
		.off1           = offsetof(struct daos_fio_options, svcl),
		.help           = "List of pool replicated service ranks",
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_DFS,
	},
#endif
	{
		.name           = NULL,                                             /* [한국어] 옵션 배열 종단 마커 */
	},
};

/*
 * [한국어]
 * daos_fio_global_init - 프로세스 최초 잡에서 DAOS 스택 초기화 + 풀/컨테이너 연결 + DFS 마운트.
 *
 * @td: 잡 컨텍스트 (td->eo에서 daos_fio_options 얻어 pool/cont/oclass 확인).
 * @return: 0=성공, 양수 errno 또는 -DER_*=실패 (td_verror 기록).
 *
 * daos_initialized==false인 상태에서 daos_fio_init()이 daos_mutex 보호 하에
 * 호출한다. 단계: (1) 옵션 검증, (2) daos_init — DAOS 클라이언트 라이브러리 초기화
 * (RPC 핸들러, 네트워크 스택, 로그 초기화), (3) 구버전 API라면 UUID 파싱,
 * (4) daos_pool_connect — 풀 서비스와 RPC 수립 후 핸들 획득(DAOS_PC_RW: 읽기+쓰기),
 * (5) daos_cont_open — 컨테이너 열기(DAOS_COO_RW), (6) dfs_mount — 컨테이너 루트에
 * DFS 네임스페이스 마운트, (7) oclass 이름→cid 변환. 실패 시 역순 롤백.
 *
 * 호출 체인: daos_fio_init → [daos_fio_global_init] → daos_init/daos_pool_connect/
 *            daos_cont_open/dfs_mount.
 */
static int daos_fio_global_init(struct thread_data *td)
{
	struct daos_fio_options	*eo = td->eo;  /* [한국어] 잡 옵션 구조체 획득 */
	daos_pool_info_t	pool_info;         /* [한국어] 풀 메타(크기/상태) — 현재 로깅 생략 */
	daos_cont_info_t	co_info;           /* [한국어] 컨테이너 메타 — 현재 로깅 생략 */
	int			rc = 0;                     /* [한국어] DAOS 에러 코드(음수) 또는 errno */

#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
	if (!eo->pool || !eo->cont || !eo->svcl) {  /* [한국어] 구버전: svcl도 필수 */
#else
	if (!eo->pool || !eo->cont) {                /* [한국어] 신버전: pool/cont만 필수 */
#endif
		log_err("Missing required DAOS options\n");  /* [한국어] 사용자 오류 보고 */
		return EINVAL;                                /* [한국어] 잡 중단 유도 */
	}

	rc = daos_init();  /* [한국어] libdaos 클라이언트 초기화 — RPC/네트워크/로그 준비 */
	if (rc != -DER_ALREADY && rc) {  /* [한국어] -DER_ALREADY(이미 초기화)는 정상 취급 */
		log_err("Failed to initialize daos %d\n", rc);
		td_verror(td, rc, "daos_init");  /* [한국어] fio에 에러 기록(잡 리포트 반영) */
		return rc;
	}

#if !defined(DAOS_API_VERSION_MAJOR) || \
    (DAOS_API_VERSION_MAJOR == 1 && DAOS_API_VERSION_MINOR < 3)
	uuid_t pool_uuid, co_uuid;  /* [한국어] 1.3 미만: 문자열을 UUID 바이너리로 파싱 필요 */

	rc = uuid_parse(eo->pool, pool_uuid);  /* [한국어] 풀 UUID 문자열 → 16바이트 바이너리 */
	if (rc) {
		log_err("Failed to parse 'Pool uuid': %s\n", eo->pool);
		td_verror(td, EINVAL, "uuid_parse(eo->pool)");
		return EINVAL;
	}

	rc = uuid_parse(eo->cont, co_uuid);    /* [한국어] 컨테이너 UUID 파싱 */
	if (rc) {
		log_err("Failed to parse 'Cont uuid': %s\n", eo->cont);
		td_verror(td, EINVAL, "uuid_parse(eo->cont)");
		return EINVAL;
	}
#endif

	/* Connect to the DAOS pool */
#if !defined(DAOS_API_VERSION_MAJOR) || DAOS_API_VERSION_MAJOR < 1
	d_rank_list_t *svcl = NULL;  /* [한국어] 구버전: 서비스 랭크 리스트 필요 */

	svcl = daos_rank_list_parse(eo->svcl, ":");  /* [한국어] ":" 구분 문자열 → 랭크 배열 */
	if (svcl == NULL) {
		log_err("Failed to parse svcl\n");
		td_verror(td, EINVAL, "daos_rank_list_parse");
		return EINVAL;
	}

	rc = daos_pool_connect(pool_uuid, NULL, svcl, DAOS_PC_RW,
			&poh, &pool_info, NULL);  /* [한국어] 구버전: UUID+svcl로 풀 연결(RW) */
	d_rank_list_free(svcl);                       /* [한국어] 임시 랭크 리스트 해제 */
#elif (DAOS_API_VERSION_MAJOR == 1 && DAOS_API_VERSION_MINOR < 3)
	rc = daos_pool_connect(pool_uuid, NULL, DAOS_PC_RW, &poh, &pool_info,
			       NULL);                 /* [한국어] 1.0-1.2: UUID로 풀 연결(svcl 자동) */
#else
	rc = daos_pool_connect(eo->pool, NULL, DAOS_PC_RW, &poh, &pool_info,
			       NULL);                 /* [한국어] 1.3+: UUID 또는 라벨 문자열 직접 */
#endif
	if (rc) {
		log_err("Failed to connect to pool %d\n", rc);
		td_verror(td, rc, "daos_pool_connect");
		return rc;
	}

	/* Open the DAOS container */
#if !defined(DAOS_API_VERSION_MAJOR) || \
    (DAOS_API_VERSION_MAJOR == 1 && DAOS_API_VERSION_MINOR < 3)
	rc = daos_cont_open(poh, co_uuid, DAOS_COO_RW, &coh, &co_info, NULL);  /* [한국어] 구버전: UUID 바이너리 */
#else
	rc = daos_cont_open(poh, eo->cont, DAOS_COO_RW, &coh, &co_info, NULL); /* [한국어] 신버전: 문자열 허용 */
#endif
	if (rc) {
		log_err("Failed to open container: %d\n", rc);
		td_verror(td, rc, "daos_cont_open");
		(void)daos_pool_disconnect(poh, NULL);  /* [한국어] 롤백: 풀 연결 해제 */
		return rc;
	}

	/* Mount encapsulated filesystem */
	rc = dfs_mount(poh, coh, O_RDWR, &daosfs);  /* [한국어] 컨테이너에 DFS 네임스페이스 마운트 */
	if (rc) {
		log_err("Failed to mount DFS namespace: %d\n", rc);
		td_verror(td, rc, "dfs_mount");
		(void)daos_pool_disconnect(poh, NULL);  /* [한국어] 롤백(주의: 순서가 pool 먼저지만 문제 없음) */
		(void)daos_cont_close(coh, NULL);       /* [한국어] 컨테이너 닫기 */
		return rc;
	}

	/* Retrieve object class to use, if specified */
	if (eo->oclass)
		cid = daos_oclass_name2id(eo->oclass);  /* [한국어] 문자열 → 숫자 ID 변환 */

	return 0;  /* [한국어] 성공 — 전역 핸들 poh/coh/daosfs 모두 채워짐 */
}

/*
 * [한국어]
 * daos_fio_global_cleanup - 마지막 잡 종료 시 DAOS 스택 일괄 해제.
 *
 * @return: 0=모두 성공, 그 외=첫 실패 코드(나머지는 로그만).
 *
 * 역순으로 dfs_umount → daos_cont_close → daos_pool_disconnect → daos_fini를 호출.
 * 한 단계 실패해도 이후 단계는 계속 시도하여 자원 누수를 최소화한다. num_threads==0
 * 시점에 daos_fio_cleanup() 또는 실패 경로의 daos_fio_init()에서 호출된다.
 */
static int daos_fio_global_cleanup()
{
	int rc;         /* [한국어] 각 단계 임시 반환값 */
	int ret = 0;    /* [한국어] 최종 반환값(첫 실패 보존) */

	rc = dfs_umount(daosfs);                  /* [한국어] DFS 네임스페이스 언마운트 */
	if (rc) {
		log_err("failed to umount dfs: %d\n", rc);
		ret = rc;                             /* [한국어] 첫 실패 기록 */
	}
	rc = daos_cont_close(coh, NULL);          /* [한국어] 컨테이너 닫기 */
	if (rc) {
		log_err("failed to close container: %d\n", rc);
		if (ret == 0)
			ret = rc;                         /* [한국어] 기존 에러 덮어쓰지 않음 */
	}
	rc = daos_pool_disconnect(poh, NULL);     /* [한국어] 풀 연결 해제 */
	if (rc) {
		log_err("failed to disconnect pool: %d\n", rc);
		if (ret == 0)
			ret = rc;
	}
	rc = daos_fini();                         /* [한국어] libdaos 종료 */
	if (rc) {
		log_err("failed to finalize daos: %d\n", rc);
		if (ret == 0)
			ret = rc;
	}

	return ret;
}

/*
 * [한국어]
 * daos_fio_setup - 엔진 setup 콜백. DFS 엔진은 setup 단계에서 할 일이 없어 0만 반환.
 *
 * @td: 잡 컨텍스트(사용 안 함).
 * @return: 항상 0.
 *
 * fio 코어는 init 전에 setup을 호출하지만, DFS 초기화는 init에서 뮤텍스 보호 하에
 * 일괄 수행하므로 setup은 no-op로 둔다.
 */
static int daos_fio_setup(struct thread_data *td)
{
	return 0;  /* [한국어] no-op */
}

/*
 * [한국어]
 * daos_fio_init - 잡별 초기화. EQ 생성 + 최초 진입 시 전역 DAOS 스택 초기화.
 *
 * @td: 잡 컨텍스트. td->o.iodepth로 io_us 배열 크기 결정.
 * @return: 0=성공, 양수 errno 또는 음수 -DER_*=실패.
 *
 * daos_mutex로 직렬화하여 (1) 잡별 daos_data 할당, (2) daos_initialized==false면
 * daos_fio_global_init 1회 호출, (3) daos_eq_create로 잡 전용 이벤트 큐 생성을
 * 수행한다. 실패 경로에서 num_threads==0이면 전역 cleanup까지 되돌린다.
 *
 * 호출 체인: td_io_init → [daos_fio_init] → daos_fio_global_init/daos_eq_create.
 */
static int daos_fio_init(struct thread_data *td)
{
	struct daos_data	*dd;      /* [한국어] 잡별 상태 포인터 */
	int			rc = 0;          /* [한국어] 반환 코드 */

	pthread_mutex_lock(&daos_mutex);  /* [한국어] 전역 상태 보호: 동시 진입 직렬화 */

	dd = malloc(sizeof(*dd));          /* [한국어] 잡별 상태 할당 */
	if (dd == NULL) {
		log_err("Failed to allocate DAOS-private data\n");
		rc = ENOMEM;
		goto out;
	}

	dd->queued	= 0;                   /* [한국어] in-flight 카운터 초기화 */
	dd->num_ios	= td->o.iodepth;       /* [한국어] 이벤트 배열 용량=iodepth */
	dd->io_us	= calloc(dd->num_ios, sizeof(struct io_u *));  /* [한국어] 완료 io_u 배열(0초기화) */
	if (dd->io_us == NULL) {
		log_err("Failed to allocate IO queue\n");
		rc = ENOMEM;
		goto out;
	}

	/* initialize DAOS stack if not already up */
	if (!daos_initialized) {           /* [한국어] 프로세스 최초 잡만 전역 초기화 */
		rc = daos_fio_global_init(td);
		if (rc)
			goto out;
		daos_initialized = true;       /* [한국어] 이후 잡은 스킵 */
	}

	rc = daos_eq_create(&dd->eqh);     /* [한국어] 잡 전용 이벤트 큐 생성 (비동기 I/O 완료 수집용) */
	if (rc) {
		log_err("Failed to create event queue: %d\n", rc);
		td_verror(td, rc, "daos_eq_create");
		goto out;
	}

	td->io_ops_data = dd;              /* [한국어] 잡 컨텍스트에 엔진 상태 부착 */
	num_threads++;                     /* [한국어] 살아있는 잡 수 증가(cleanup에서 감소) */
out:
	if (rc) {                          /* [한국어] 실패 경로: 자원 롤백 */
		if (dd) {
			free(dd->io_us);
			free(dd);
		}
		if (num_threads == 0 && daos_initialized) {
			/* don't clobber error return value */
			(void)daos_fio_global_cleanup();  /* [한국어] 현재 잡이 첫 잡이었고 실패했으므로 전역 해제 */
			daos_initialized = false;
		}
	}
	pthread_mutex_unlock(&daos_mutex); /* [한국어] 뮤텍스 해제 */
	return rc;
}

/*
 * [한국어]
 * daos_fio_cleanup - 잡별 정리. EQ 파기 + 마지막 잡이면 전역 DAOS 해제.
 *
 * @td: 잡 컨텍스트.
 *
 * cleanup은 각 잡 스레드가 종료될 때 호출된다. daos_eq_destroy(DESTROY_FORCE)로
 * 미완료 이벤트까지 강제 정리하고, daos_mutex 하에 num_threads를 감소시켜
 * 0이 되면 daos_fio_global_cleanup()으로 DAOS 스택 전체를 내려 놓는다.
 */
static void daos_fio_cleanup(struct thread_data *td)
{
	struct daos_data	*dd = td->io_ops_data;  /* [한국어] init에서 부착된 상태 */
	int			rc;

	if (dd == NULL)                     /* [한국어] init 실패 시 dd 없음 — 조기 반환 */
		return;

	rc = daos_eq_destroy(dd->eqh, DAOS_EQ_DESTROY_FORCE);  /* [한국어] EQ 강제 파기(in-flight 포함) */
	if (rc < 0) {
		log_err("failed to destroy event queue: %d\n", rc);
		td_verror(td, rc, "daos_eq_destroy");
	}

	free(dd->io_us);                    /* [한국어] 완료 io_u 배열 해제 */
	free(dd);                           /* [한국어] 잡별 상태 해제 */

	pthread_mutex_lock(&daos_mutex);    /* [한국어] 전역 카운터 보호 */
	num_threads--;                      /* [한국어] 잡 수 감소 */
	if (daos_initialized && num_threads == 0) {  /* [한국어] 마지막 잡이면 전역 해제 */
		int ret;

		ret = daos_fio_global_cleanup();
		if (ret < 0 && rc == 0) {
			log_err("failed to clean up: %d\n", ret);
			td_verror(td, ret, "daos_fio_global_cleanup");
		}
		daos_initialized = false;       /* [한국어] 다음 라운드를 위해 리셋 */
	}
	pthread_mutex_unlock(&daos_mutex);
}

/*
 * [한국어]
 * daos_fio_get_file_size - dfs_stat으로 파일 크기를 조회하여 fio에 보고.
 *
 * @td: 잡 컨텍스트.
 * @f: 대상 파일(fio_file). 이름은 f->file_name.
 * @return: 0=성공, 그 외=에러.
 *
 * fio 코어가 파일 크기 자동 계산 시 호출(size=0 잡). daos_initialized==false(아직
 * init 전)면 스킵하고 0 반환 — 진짜 열기는 open_file에서 처리한다.
 */
static int daos_fio_get_file_size(struct thread_data *td, struct fio_file *f)
{
	char		*file_name = f->file_name;  /* [한국어] DFS 내 파일 경로 */
	struct stat	stbuf = {0};                 /* [한국어] 크기만 사용, 나머지 0 */
	int		rc;

	dprint(FD_FILE, "dfs stat %s\n", f->file_name);  /* [한국어] FD_FILE 카테고리 디버그 로그 */

	if (!daos_initialized)  /* [한국어] init 전 호출되는 경로(예: 크기 계산 먼저) → 스킵 */
		return 0;

	rc = dfs_stat(daosfs, NULL, file_name, &stbuf);  /* [한국어] DFS stat — parent=NULL=루트 기준 */
	if (rc) {
		log_err("Failed to stat %s: %d\n", f->file_name, rc);
		td_verror(td, rc, "dfs_stat");
		return rc;
	}

	f->real_file_size = stbuf.st_size;  /* [한국어] fio에 실제 크기 보고 */
	return 0;
}

/*
 * [한국어]
 * daos_fio_close - close_file 콜백. dfs_release로 파일 오브젝트 핸들 반환.
 *
 * @td: 잡 컨텍스트. td->io_ops_data에서 dd->obj 획득.
 * @f: 대상 파일.
 * @return: 0=성공, 그 외=에러.
 *
 * open_file의 짝. dfs_release는 파일 디스크립터를 닫는 POSIX close와 유사.
 */
static int daos_fio_close(struct thread_data *td, struct fio_file *f)
{
	struct daos_data	*dd = td->io_ops_data;
	int			rc;

	dprint(FD_FILE, "dfs release %s\n", f->file_name);

	rc = dfs_release(dd->obj);  /* [한국어] DFS 파일 핸들 해제 */
	if (rc) {
		log_err("Failed to release %s: %d\n", f->file_name, rc);
		td_verror(td, rc, "dfs_release");
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * daos_fio_open - open_file 콜백. 잡 방향(read/write)과 create 옵션에 따라 dfs_open 호출.
 *
 * @td: 잡 컨텍스트. td->o.create_on_open/allow_create 참조.
 * @f: 대상 파일(file_name).
 * @return: 0=성공, 그 외=에러(td_verror).
 *
 * POSIX open과 유사하게 O_RDONLY/O_RDWR/O_CREAT 플래그를 구성하고 dfs_open으로
 * 파일 오브젝트(dfs_obj_t*)를 얻어 dd->obj에 저장. cid(오브젝트 클래스)와 chsz(청크)
 * 는 파일 *생성* 시에만 의미가 있다(이미 존재하면 무시).
 */
static int daos_fio_open(struct thread_data *td, struct fio_file *f)
{
	struct daos_data	*dd = td->io_ops_data;
	struct daos_fio_options	*eo = td->eo;        /* [한국어] cid는 전역, chsz는 여기서 */
	int			flags = 0;                        /* [한국어] dfs_open 플래그 비트셋 */
	int			rc;

	dprint(FD_FILE, "dfs open %s (%s/%d/%d)\n",
	       f->file_name, td_write(td) & !read_only ? "rw" : "r",
	       td->o.create_on_open, td->o.allow_create);

	if (td->o.create_on_open && td->o.allow_create)
		flags |= O_CREAT;  /* [한국어] 잡 시작 시 생성 허용 */

	if (td_write(td)) {     /* [한국어] 쓰기 잡 */
		if (!read_only)
			flags |= O_RDWR;    /* [한국어] read_only 글로벌 모드면 RDWR 금지 */
		if (td->o.allow_create)
			flags |= O_CREAT;   /* [한국어] 없으면 생성 */
	} else if (td_read(td)) {
		flags |= O_RDONLY;      /* [한국어] 읽기 전용 */
	}

	rc = dfs_open(daosfs, NULL, f->file_name,              /* [한국어] parent=NULL=루트 상대 경로 */
		      S_IFREG | S_IRUSR | S_IWUSR,              /* [한국어] 정규 파일, rw-- 퍼미션 (생성 시) */
		      flags, cid, eo->chsz, NULL, &dd->obj);    /* [한국어] oclass/chunk_size/힌트(NULL)/결과 */
	if (rc) {
		log_err("Failed to open %s: %d\n", f->file_name, rc);
		td_verror(td, rc, "dfs_open");
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * daos_fio_unlink - unlink_file 콜백. dfs_remove로 파일 삭제.
 *
 * @td: 잡 컨텍스트.
 * @f: 대상 파일.
 * @return: 0=성공.
 *
 * unlink=1 옵션이 있을 때 fio가 잡 종료 후 호출.
 */
static int daos_fio_unlink(struct thread_data *td, struct fio_file *f)
{
	int rc;

	dprint(FD_FILE, "dfs remove %s\n", f->file_name);

	rc = dfs_remove(daosfs, NULL, f->file_name, false, NULL);  /* [한국어] 4번째 인자 false=비재귀(파일) */
	if (rc) {
		log_err("Failed to remove %s: %d\n", f->file_name, rc);
		td_verror(td, rc, "dfs_remove");
		return rc;
	}

	return 0;
}

/*
 * [한국어]
 * daos_fio_invalidate - invalidate 콜백. DFS는 클라이언트 캐시를 강제 무효화할 수단이
 *                       엔진 레벨엔 없으므로 no-op. 호출 사실만 디버그 로그.
 *
 * @td, @f: 잡/파일 컨텍스트(미사용).
 * @return: 항상 0.
 */
static int daos_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	dprint(FD_FILE, "dfs invalidate %s\n", f->file_name);
	return 0;
}

/*
 * [한국어]
 * daos_fio_io_u_free - io_u 해제 시 engine_data(daos_iou) 동반 해제.
 *
 * @td, @io_u: fio io_u 슬롯(풀 반환 직전).
 *
 * fio 코어가 io_u 풀을 내릴 때 각 io_u에 대해 호출. io_u_init과 대칭.
 */
static void daos_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct daos_iou *io = io_u->engine_data;  /* [한국어] io_u에 부착된 DAOS 컨텍스트 */

	if (io) {
		io_u->engine_data = NULL;              /* [한국어] dangling 방지 */
		free(io);                              /* [한국어] 동적 할당 해제 */
	}
}

/*
 * [한국어]
 * daos_fio_io_u_init - io_u 생성 시 DAOS 측 보조 구조 daos_iou 할당·연결.
 *
 * @td, @io_u: 풀에 등록되는 새 io_u.
 * @return: 0=성공, ENOMEM=실패.
 *
 * io_u 1개당 daos_iou 1개. 이후 queue()에서 같은 슬롯을 재사용한다.
 */
static int daos_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct daos_iou *io;

	io = malloc(sizeof(struct daos_iou));  /* [한국어] 요청 컨텍스트 할당 */
	if (!io) {
		td_verror(td, ENOMEM, "malloc");
		return ENOMEM;
	}
	io->io_u = io_u;               /* [한국어] 양방향 연결: daos_iou → io_u */
	io_u->engine_data = io;        /* [한국어] io_u → daos_iou */
	return 0;
}

/*
 * [한국어]
 * daos_fio_event - getevents가 수집한 완료 인덱스에 해당하는 io_u 반환.
 *
 * @td: 잡 컨텍스트.
 * @event: getevents가 보고한 0..N-1 인덱스.
 * @return: 해당 io_u 포인터(fio 코어가 put_io_u 처리).
 */
static struct io_u * daos_fio_event(struct thread_data *td, int event)
{
	struct daos_data *dd = td->io_ops_data;

	return dd->io_us[event];  /* [한국어] getevents가 채운 배열에서 조회 */
}

/*
 * [한국어]
 * daos_fio_getevents - DAOS 이벤트 큐를 폴링하여 min..max 범위의 완료를 수확.
 *
 * @td: 잡 컨텍스트.
 * @min: 최소 대기 완료 수(이만큼 모일 때까지 루프).
 * @max: 한 번에 가져올 최대 완료 수.
 * @t: 타임아웃 — 본 구현은 NOWAIT 사용으로 무시(busy-loop).
 * @return: 수집된 완료 수(폴 에러 시 현재까지 수집분 반환).
 *
 * daos_eq_poll(eqh, 0, DAOS_EQ_NOWAIT, max, evp)을 호출해 완료 이벤트 배열을 얻고,
 * container_of(ev, struct daos_iou, ev)로 각 이벤트를 소유한 daos_iou를 복원한다
 * (ev 필드 오프셋만 알면 바깥 구조체 포인터 역산 가능 — Linux 커널 스타일 패턴).
 * ev_error를 io_u->error에 전달하고 daos_event_fini로 이벤트를 정리한 뒤
 * dd->io_us[]에 쌓아 fio 코어가 event(i)로 꺼내가게 한다.
 *
 * 호출 체인: td_io_getevents → [daos_fio_getevents] → daos_eq_poll → daos_event_fini.
 */
static int daos_fio_getevents(struct thread_data *td, unsigned int min,
			      unsigned int max, const struct timespec *t)
{
	struct daos_data	*dd = td->io_ops_data;
	daos_event_t		*evp[max];      /* [한국어] VLA: 완료 이벤트 포인터 배열(스택) */
	unsigned int		events = 0;      /* [한국어] 지금까지 수집한 완료 수 */
	int			i;
	int			rc;

	while (events < min) {               /* [한국어] 최소 min개 모일 때까지 반복 */
		rc = daos_eq_poll(dd->eqh, 0, DAOS_EQ_NOWAIT, max, evp);  /* [한국어] 논블록 폴 — 즉시 반환 */
		if (rc < 0) {
			log_err("Event poll failed: %d\n", rc);
			td_verror(td, rc, "daos_eq_poll");
			return events;               /* [한국어] 에러 시 현재까지 수집분만 반환 */
		}

		for (i = 0; i < rc; i++) {       /* [한국어] rc = 이번 폴에서 건진 완료 수 */
			struct daos_iou	*io;
			struct io_u	*io_u;

			io = container_of(evp[i], struct daos_iou, ev);  /* [한국어] ev 포인터→바깥 구조체 역산 */
			if (io->complete)             /* [한국어] 이중 완료 방지 디버그 */
				log_err("Completion on already completed I/O\n");

			io_u = io->io_u;              /* [한국어] 대응 fio io_u 복원 */
			if (io->ev.ev_error)          /* [한국어] DAOS 에러 코드 존재 시 */
				io_u->error = io->ev.ev_error;  /* [한국어] fio에 전달 */
			else
				io_u->resid = 0;         /* [한국어] 부분 전송 없다고 가정 — 단순화 */

			dd->io_us[events] = io_u;    /* [한국어] 완료 배열에 등록 */
			dd->queued--;                /* [한국어] in-flight 감소 */
			daos_event_fini(&io->ev);    /* [한국어] 이벤트 자원 해제 */
			io->complete = true;         /* [한국어] 중복 완료 방지 플래그 */
			events++;                     /* [한국어] 누적 수 증가 */
		}
	}

	dprint(FD_IO, "dfs eq_pool returning %d (%u/%u)\n", events, min, max);

	return events;
}

/*
 * [한국어]
 * daos_fio_queue - io_u 하나를 DAOS EQ에 비동기 제출.
 *
 * @td: 잡 컨텍스트.
 * @io_u: 제출할 I/O 유닛(방향/오프셋/버퍼/길이 포함).
 * @return: FIO_Q_QUEUED(비동기 수락) / FIO_Q_BUSY(큐 만원) / FIO_Q_COMPLETED(즉시 완료·실패).
 *
 * 단계: (1) iodepth 초과면 BUSY, (2) sgl/iov를 io_u 버퍼로 구성,
 * (3) daos_event_init으로 ev를 EQ에 바인딩(이후 완료가 이 EQ로 들어옴),
 * (4) ddir에 따라 dfs_read/dfs_write 호출 — 둘 다 비동기로 즉시 반환,
 * (5) DDIR_SYNC는 no-op 동기 완료, 기타 방향은 에러. 성공 시 queued++ 후 FIO_Q_QUEUED.
 *
 * 호출 체인: td_io_queue → [daos_fio_queue] → daos_event_init → dfs_read/write.
 */
static enum fio_q_status daos_fio_queue(struct thread_data *td,
					struct io_u *io_u)
{
	struct daos_data	*dd = td->io_ops_data;
	struct daos_iou		*io = io_u->engine_data;  /* [한국어] io_u_init에서 붙인 컨텍스트 */
	daos_off_t		offset = io_u->offset;      /* [한국어] 파일 내 오프셋 */
	int			rc;

	if (dd->queued == td->o.iodepth)  /* [한국어] 큐 가득 — 코어에 재시도 유도 */
		return FIO_Q_BUSY;

	io->sgl.sg_nr = 1;                         /* [한국어] 단일 iov 사용 */
	io->sgl.sg_nr_out = 0;                     /* [한국어] 출력 필드는 libdaos가 설정 */
	d_iov_set(&io->iov, io_u->xfer_buf, io_u->xfer_buflen);  /* [한국어] 버퍼·길이 바인딩 */
	io->sgl.sg_iovs = &io->iov;                /* [한국어] sgl이 iov를 가리키게 함 */
	io->size = io_u->xfer_buflen;              /* [한국어] dfs_read의 in/out 크기 인자 */

	io->complete = false;                      /* [한국어] 새 요청 — 미완료 상태 시작 */
	rc = daos_event_init(&io->ev, dd->eqh, NULL);  /* [한국어] 이벤트를 EQ에 등록(부모=NULL) */
	if (rc) {
		log_err("Event init failed: %d\n", rc);
		io_u->error = rc;
		return FIO_Q_COMPLETED;                /* [한국어] 즉시 실패 완료 보고 */
	}

	switch (io_u->ddir) {                      /* [한국어] I/O 방향 분기 */
	case DDIR_WRITE:
		rc = dfs_write(daosfs, dd->obj, &io->sgl, offset, &io->ev);  /* [한국어] 비동기 쓰기 제출 */
		if (rc) {
			log_err("dfs_write failed: %d\n", rc);
			io_u->error = rc;
			return FIO_Q_COMPLETED;
		}
		break;
	case DDIR_READ:
		rc = dfs_read(daosfs, dd->obj, &io->sgl, offset, &io->size,
			      &io->ev);                /* [한국어] 비동기 읽기 제출(size는 in/out) */
		if (rc) {
			log_err("dfs_read failed: %d\n", rc);
			io_u->error = rc;
			return FIO_Q_COMPLETED;
		}
		break;
	case DDIR_SYNC:
		io_u->error = 0;                       /* [한국어] DFS는 잡 단위 sync 개념 없음 — no-op */
		return FIO_Q_COMPLETED;
	default:
		dprint(FD_IO, "Invalid IO type: %d\n", io_u->ddir);
		io_u->error = -DER_INVAL;              /* [한국어] 지원 안 되는 방향(TRIM 등) */
		return FIO_Q_COMPLETED;
	}

	dd->queued++;                              /* [한국어] in-flight 증가 */
	return FIO_Q_QUEUED;                       /* [한국어] 비동기 수락: getevents에서 완료 수확 */
}

/*
 * [한국어]
 * daos_fio_prep - prep 콜백. DFS 엔진은 queue()에서 한꺼번에 준비하므로 no-op.
 *
 * @td, @io_u: 미사용.
 * @return: 항상 0.
 */
static int daos_fio_prep(struct thread_data fio_unused *td, struct io_u *io_u)
{
	return 0;
}

/* ioengine_ops for get_ioengine() */
/* [한국어] fio 코어가 이 엔진을 등록/조회할 때 사용하는 콜백 테이블. */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "dfs",
	/* [한국어] 엔진 이름(ioengine=dfs 옵션으로 선택). */
	.version		= FIO_IOOPS_VERSION,
	/* [한국어] fio 코어와의 ABI 버전. 코어와 다르면 로드 거부. */
	.flags			= FIO_DISKLESSIO | FIO_NODISKUTIL,
	/* [한국어] FIO_DISKLESSIO: 로컬 파일시스템/블록디바이스가 아닌 네트워크 스토리지.
	 *          FIO_NODISKUTIL: /proc/diskstats 기반 disk util 출력 생략(DAOS는 해당 없음). */

	.setup			= daos_fio_setup,
	/* [한국어] 엔진 로드 직후 1회. DFS는 no-op. */
	.init			= daos_fio_init,
	/* [한국어] 잡 시작 — EQ 생성, 첫 잡은 전역 DAOS 초기화. */
	.prep			= daos_fio_prep,
	/* [한국어] 각 io_u 제출 전 훅(no-op). */
	.cleanup		= daos_fio_cleanup,
	/* [한국어] 잡 종료 — EQ 파기, 마지막 잡은 전역 DAOS 해제. */

	.open_file		= daos_fio_open,
	/* [한국어] 대상 파일 dfs_open. */
	.invalidate		= daos_fio_invalidate,
	/* [한국어] 캐시 무효화(no-op). */
	.get_file_size		= daos_fio_get_file_size,
	/* [한국어] dfs_stat으로 실제 크기 보고. */
	.close_file		= daos_fio_close,
	/* [한국어] dfs_release. */
	.unlink_file		= daos_fio_unlink,
	/* [한국어] dfs_remove. */

	.queue			= daos_fio_queue,
	/* [한국어] dfs_read/write 비동기 제출(FIO_Q_QUEUED). */
	.getevents		= daos_fio_getevents,
	/* [한국어] daos_eq_poll로 완료 수확. */
	.event			= daos_fio_event,
	/* [한국어] 인덱스→io_u 조회. */
	.io_u_init		= daos_fio_io_u_init,
	/* [한국어] io_u마다 daos_iou 할당·연결. */
	.io_u_free		= daos_fio_io_u_free,
	/* [한국어] io_u 해제 시 daos_iou 동반 해제. */

	.option_struct_size	= sizeof(struct daos_fio_options),
	/* [한국어] 옵션 구조체 크기(코어가 할당 시 사용). */
	.options		= options,
	/* [한국어] 옵션 정의 테이블. */
};

/*
 * [한국어]
 * fio_dfs_register - 공유 라이브러리 로드 시 자동 실행되는 엔진 등록자.
 *
 * fio_init 매크로(= __attribute__((constructor)))로 main 이전에 호출되어
 * register_ioengine()에 ioengine_ops를 등록. 이후 ioengine=dfs로 선택 가능.
 */
static void fio_init fio_dfs_register(void)
{
	register_ioengine(&ioengine);
}

/*
 * [한국어]
 * fio_dfs_unregister - 라이브러리 언로드 시 destructor. 엔진 테이블에서 제거.
 */
static void fio_exit fio_dfs_unregister(void)
{
	unregister_ioengine(&ioengine);
}
