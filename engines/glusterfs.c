/*
 * glusterfs engine
 *
 * common Glusterfs's gfapi interface
 *
 */

/*
 * [한국어 설명] GlusterFS gfapi 공통 I/O 엔진 헬퍼 (glusterfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 GlusterFS 분산 파일시스템의 유저스페이스 라이브러리 gfapi(libgfapi)를 사용해
 * fio가 볼륨에 접근할 때 필요한 공통 부트스트랩/티어다운 로직을 모아 둔 헬퍼이다.
 * 독립적인 I/O 엔진이 아니며, glusterfs_sync.c(gfapi 엔진)와 glusterfs_async.c(gfapi_async
 * 엔진)가 공유하는 setup/cleanup/open_file/close_file/get_file_size/unlink_file 콜백을
 * 제공한다. single-instance 모드에서는 동일 (volume, brick) 쌍으로 이미 만들어진 glfs_t
 * 인스턴스를 참조 카운트 기반으로 재사용하여, 잡이 여러 스레드를 생성할 때 볼륨을 중복으로
 * 초기화하지 않도록 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 백엔드 실행 경로 backend.c → td_io_init() → ioengine_ops.init(fio_gf_setup) 단계에서
 * 진입한다. 그 후 각 fio_file에 대해 ioengines.c가 open_file(fio_gf_open_file)을 부르고,
 * I/O 루프에서 gfapi 동기/비동기 엔진의 queue/getevents가 이 파일이 저장해 둔 gf_data
 * (td->io_ops_data)의 glfs_fd_t를 사용한다. 잡 종료 시 cleanup(fio_gf_cleanup) 경로로
 * 돌아와 single-instance 참조 카운트가 0이 되는 순간 glfs_fini()로 연결을 끊는다.
 * 실행 컨텍스트는 fio 잡 스레드(유저스페이스) 1개이며, glfs_list_head/glfs_lock은
 * 프로세스 내 모든 잡 스레드가 공유하므로 pthread_mutex로 보호한다.
 *
 * === 타 모듈과의 연결 ===
 * 상단: glusterfs_sync.c(gfapi), glusterfs_async.c(gfapi_async) 엔진이 이 파일의 심볼
 * (fio_gf_setup, fio_gf_cleanup, fio_gf_open_file, fio_gf_close_file, fio_gf_get_file_size,
 *  fio_gf_unlink_file, gfapi_options)을 직접 참조한다.
 * 하단: libgfapi의 glfs_new/glfs_init/glfs_creat/glfs_ftruncate/glfs_fsync/glfs_unlink/
 *       glfs_lseek/glfs_fadvise를 호출해 GlusterFS translator 스택에 I/O를 전달한다.
 * 데이터 흐름: fio CLI/잡파일의 volume=/brick=/single-instance= 옵션 → struct gf_options
 * (td->eo) → fio_gf_get_glfs()가 glfs_t 인스턴스를 획득 → struct gf_data에 저장 →
 * 엔진별 queue()에서 glfs_read/glfs_write로 데이터가 흐름.
 * 공유 자료구조: glfs_list_head(프로세스 전역 리스트), glfs_lock(pthread_mutex_t)로
 * single-instance 참조 카운트를 보호한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_gf_setup(): td->io_ops_data(gf_data)를 할당하고 fio_gf_get_glfs()로 glfs_t 획득.
 * - fio_gf_cleanup(): 열린 fd, 이벤트 버퍼, glfs_t 참조를 정리한다.
 * - fio_gf_open_file(): glfs_creat로 파일을 생성하고 read 테스트면 필요 크기까지 확장.
 * - fio_gf_close_file() / fio_gf_unlink_file(): 파일 해제 및 삭제.
 * - fio_gf_get_file_size(): glfs_lstat으로 real_file_size 채우기.
 * - struct glfs_info: single-instance 참조 카운트 항목(volume/brick/fs/refcount).
 */

/* [한국어] gfapi.h: 이 파일과 glusterfs_sync.c/glusterfs_async.c가 공유하는 공통 헤더.
 *  struct gf_options(volume/brick/single-instance 옵션), struct gf_data(잡별 엔진 상태),
 *  libgfapi(glfs.h)의 glfs_t/glfs_fd_t 타입과 glfs_* 함수 선언을 끌어온다. 이 헤더를
 *  통해서만 GlusterFS 라이브러리와 fio 내부 타입(thread_data, fio_file)을 연결한다. */
#include "gfapi.h"
/* [한국어] ../optgroup.h: fio의 옵션 그룹/카테고리 식별자 정의 헤더.
 *  FIO_OPT_C_ENGINE(엔진 카테고리)와 FIO_OPT_G_GFAPI(gfapi 그룹) 상수를 사용하여
 *  아래 gfapi_options 배열을 "엔진 > gfapi" 분류에 등록한다. fio --cmdhelp 출력과
 *  HOWTO 문서 정렬에 쓰인다. */
#include "../optgroup.h"

/*
 * [한국어]
 * gfapi_options[] - gfapi/gfapi_async 엔진이 공유하는 fio 옵션 테이블.
 *
 * 이 전역 배열은 engines/glusterfs_sync.c와 engines/glusterfs_async.c의 ioengine_ops.options
 * 필드가 공통으로 참조한다. init.c/options.c의 fio 옵션 파서가 잡 파일/CLI에서 "volume=",
 * "brick=", "single-instance=" 키를 만나면 offsetof로 지정된 struct gf_options 필드에
 * 값을 저장한다. FIO_OPT_C_ENGINE 카테고리 + FIO_OPT_G_GFAPI 그룹으로 묶여 `fio --enghelp`
 * 의 그룹별 출력을 가능케 한다. 마지막 엔트리 .name=NULL은 테이블 종료 마커로, fio 파서가
 * 배열 순회 종료를 판단하는 관례이다.
 */
struct fio_option gfapi_options[] = {
	{
	 /* [한국어] 옵션 키 (CLI/잡파일에서 "volume=" 형태로 사용). */
	 .name = "volume",
	 /* [한국어] 긴 이름 (help/문서 출력용). */
	 .lname = "Glusterfs volume",
	 /* [한국어] FIO_OPT_STR_STORE: 문자열을 파싱하여 strdup 후 off1 위치에 저장하는 타입. */
	 .type = FIO_OPT_STR_STORE,
	 /* [한국어] `fio --cmdhelp`에서 보이는 한 줄 설명. */
	 .help = "Name of the Glusterfs volume",
	 /* [한국어] td->eo(= struct gf_options 포인터) 기준 gf_vol 멤버의 바이트 오프셋.
	  *  fio 옵션 파서가 이 오프셋 위치에 strdup한 문자열 포인터를 써 넣는다. */
	 .off1 = offsetof(struct gf_options, gf_vol),
	 /* [한국어] 옵션 카테고리: I/O 엔진 전용 (파일/보안/전역과 구분). */
	 .category = FIO_OPT_C_ENGINE,
	 /* [한국어] 세부 그룹: GFAPI — 도움말에서 gfapi 항목으로 함께 묶여 노출된다. */
	 .group = FIO_OPT_G_GFAPI,
	 },
	{
	 /* [한국어] brick: GlusterFS 볼륨의 접속 대상 서버 호스트(볼륨 서버) 지정. */
	 .name = "brick",
	 .lname = "Glusterfs brick name",
	 .type = FIO_OPT_STR_STORE,
	 .help = "Name of the Glusterfs brick to connect",
	 /* [한국어] struct gf_options::gf_brick에 호스트명 문자열 저장. */
	 .off1 = offsetof(struct gf_options, gf_brick),
	 .category = FIO_OPT_C_ENGINE,
	 .group = FIO_OPT_G_GFAPI,
	 },
	{
	 /* [한국어] single-instance: 여러 잡이 동일 (volume,brick)을 쓸 때 glfs_t를 공유할지 여부.
	  *  true면 fio_gf_get_glfs()가 참조카운트 기반 재사용 경로로 진입한다. */
	 .name = "single-instance",
	 .lname = "Single glusterfs instance",
	 /* [한국어] BOOL 타입: 0/1로 파싱되어 gf_single_instance에 저장. */
	 .type = FIO_OPT_BOOL,
	 .help = "Only one glusterfs instance",
	 .off1 = offsetof(struct gf_options, gf_single_instance),
	 .category = FIO_OPT_C_ENGINE,
	 .group = FIO_OPT_G_GFAPI,
	 },
	{
	 /* [한국어] 종료 마커: .name=NULL에서 fio 옵션 파서가 배열 순회를 멈춘다. */
	 .name = NULL,
	 },
};

/*
 * [한국어]
 * struct glfs_info - single-instance 모드에서 (volume,brick) 쌍으로 공유되는 glfs_t 래퍼.
 *
 * fio는 한 프로세스 내에 여러 잡 스레드를 돌릴 수 있는데, 같은 GlusterFS 볼륨을 여러 잡이
 * 사용한다면 매번 glfs_new/glfs_init을 호출하는 것은 낭비이다. 이 구조체는 glfs_list_head에
 * 연결되어 참조 카운트로 생명을 관리하고, refcount가 0이 되면 glfs_fini()로 연결을 끊는다.
 * glfs_lock(pthread_mutex)으로 다중 잡 스레드 진입을 직렬화한다.
 */
struct glfs_info {
	struct flist_head	list;
	/* [한국어] glfs_list_head에 연결되는 이중 연결 리스트 노드.
	 *  설정자: fio_gf_get_glfs()가 새 인스턴스 생성 시 flist_add_tail로 추가.
	 *  읽는 자: fio_gf_get_glfs()/fio_gf_put_glfs()가 flist_for_each로 순회 검색.
	 *  동기화: glfs_lock을 잡은 상태에서만 접근. */

	char			*volume;
	/* [한국어] 이 인스턴스가 바인딩된 GlusterFS 볼륨 이름(strdup 사본).
	 *  설정자: fio_gf_get_glfs()의 malloc 경로에서 strdup(volume).
	 *  읽는 자: 동일 볼륨 검색을 위해 strcmp 비교에 사용.
	 *  해제자: fio_gf_put_glfs()에서 refcount==0일 때 free.
	 *  값 범위: NULL이 아닌 유효 C 문자열. */

	char			*brick;
	/* [한국어] 이 인스턴스의 brick(서버 호스트) 이름(strdup 사본).
	 *  설정자/읽는 자/해제자: volume과 동일 규칙.
	 *  값 범위: glfs_set_volfile_server("tcp", brick, 0)에 전달된 값. */

	glfs_t			*fs;
	/* [한국어] libgfapi가 반환한 불투명 파일시스템 핸들.
	 *  설정자: fio_gf_new_fs()의 glfs_new → glfs_init 성공 결과.
	 *  읽는 자: 잡 스레드들이 glfs_creat/glfs_read/glfs_write 등에 전달.
	 *  해제자: refcount==0 시 glfs_fini(fs).
	 *  동기화: libgfapi 내부가 멀티스레드 안전(공식 문서 기준) — 다중 잡에서 공유 가능. */

	int			refcount;
	/* [한국어] 이 glfs 인스턴스를 참조하는 잡의 수.
	 *  설정자: fio_gf_get_glfs()에서 증가, fio_gf_put_glfs()에서 감소.
	 *  읽는 자: put 경로에서 0이 되는지 확인 후 glfs_fini 호출 여부 결정.
	 *  값 범위: 0 이상의 정수. 0에 도달하면 리스트에서 제거되고 free.
	 *  동기화: glfs_lock으로 보호되는 임계 영역 안에서만 변경. */
};

/* [한국어] single-instance 모드의 전역 공유 상태를 보호하는 뮤텍스.
 *  PTHREAD_MUTEX_INITIALIZER로 정적 초기화되어 생성자 코드가 필요 없다.
 *  보호 대상: glfs_list_head 리스트 구조 + 각 glfs_info의 refcount/volume/brick/fs 포인터.
 *  잠금 위치: fio_gf_get_glfs(), fio_gf_put_glfs() 두 곳. */
static pthread_mutex_t glfs_lock = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] single-instance로 공유되는 glfs_info들의 전역 헤드(빈 더미 노드).
 *  FLIST_HEAD 매크로는 list.next/prev를 자기 자신으로 초기화하는 빈 리스트를 만든다.
 *  프로세스 전체에 하나만 존재하며, fio 프로세스 수명 동안 유지된다. */
static FLIST_HEAD(glfs_list_head);

/*
 * [한국어]
 * fio_gf_new_fs - GlusterFS 볼륨에 대한 새 libgfapi 연결(glfs_t)을 생성한다.
 *
 * @volume: GlusterFS 볼륨 이름. gfapi_options의 "volume=" 값이 잡 옵션을 통해 전달됨.
 * @brick:  접속할 서버 호스트명. "brick=" 옵션 값. TCP 2007(기본) 포트로 볼륨파일을 수신.
 * @return: 초기화에 성공한 glfs_t 포인터. 실패 시 NULL.
 *
 * libgfapi 초기화는 다음 4단계: glfs_new(컨텍스트 할당) → glfs_set_volfile_server
 * (볼륨파일을 가져올 서버 등록) → glfs_init(볼륨파일 수신 및 translator 스택 조립) →
 * 간단한 glfs_lstat(".")으로 마운트 가용성 확인. glusterd가 내려가 있거나 브릭 연결이
 * 실패하면 glfs_init이 에러를 반환한다. 중간에 sleep(2)가 있는 이유는 일부 GlusterFS
 * 버전에서 init 직후 즉시 I/O를 날리면 lookup 레이스가 발생하기 때문이다(원본 코드 의도).
 * 실행 컨텍스트: fio 잡 스레드(유저스페이스). fio_gf_get_glfs()에서 직접 호출되거나
 * single-instance 모드의 첫 잡에서만 호출된다.
 *
 * 호출 체인:
 *   fio_gf_setup → fio_gf_get_glfs → [fio_gf_new_fs] → glfs_new/glfs_init (libgfapi)
 */
static glfs_t *fio_gf_new_fs(char *volume, char *brick)
{
	int r = 0;                         /* [한국어] libgfapi 호출 반환값 누적용. 0이면 성공, 비0이면 아래 out에서 glfs_fini로 롤백. */
	glfs_t *fs;                        /* [한국어] 생성할 glfs_t 핸들. 성공 시 호출자에게 반환, 실패 시 NULL 반환. */
	struct stat sb = { 0, };           /* [한국어] 초기 glfs_lstat 검증용 임시 stat 버퍼. 결과값은 사용하지 않고 접근성만 본다. */

	fs = glfs_new(volume);             /* [한국어] libgfapi 컨텍스트 할당. 내부적으로 볼륨 이름만 기록하고 네트워크는 아직 열지 않는다. */
	if (!fs) {                         /* [한국어] 할당 실패(주로 ENOMEM)는 드물지만 가능. */
		log_err("glfs_new failed.\n");  /* [한국어] fio의 표준 에러 로그로 출력(stderr + log file). */
		goto out;                  /* [한국어] fs가 NULL이므로 out에서 glfs_fini 호출 금지 — 아래 r==0 경로로 NULL 반환. */
	}
	glfs_set_logging(fs, "/tmp/fio_gfapi.log", 7); /* [한국어] libgfapi 자체 로그를 /tmp로 빼고 레벨 7(TRACE)로. 디버그용 고정 경로. */
	/* default to tcp */
	/* [한국어] 볼륨파일 수신 전송 방식 지정. GlusterFS는 tcp/rdma/socket을 지원하나 이 엔진은 tcp 고정.
	 *  마지막 0은 포트 번호로, 0이면 libgfapi 기본값(24007 glusterd 포트) 사용. */
	r = glfs_set_volfile_server(fs, "tcp", brick, 0);
	if (r) {                           /* [한국어] 파라미터 검증 실패(호스트명 공백 등). */
		log_err("glfs_set_volfile_server failed.\n");
		goto out;                  /* [한국어] r!=0 이므로 out에서 glfs_fini로 정리. */
	}
	r = glfs_init(fs);                 /* [한국어] 실제 연결 수립: 볼륨파일 fetch → translator 그래프 구축 → 마운트 준비 완료. */
	if (r) {                           /* [한국어] glusterd 다운, 방화벽, 인증 실패 시 여기로. */
		log_err("glfs_init failed. Is glusterd running on brick?\n");
		goto out;
	}
	sleep(2);                          /* [한국어] 일부 GlusterFS 버전에서 init 직후 lookup 레이스 방지를 위한 보수적 대기. */
	r = glfs_lstat(fs, ".", &sb);      /* [한국어] 루트("."")에 대한 lstat 호출로 마운트가 정말 사용 가능한지 한 번 검증. */
	if (r) {                           /* [한국어] 볼륨이 마운트는 됐지만 접근 불가한 극히 드문 상태를 걸러낸다. */
		log_err("glfs_lstat failed.\n");
		goto out;
	}

out:
	if (r) {                           /* [한국어] 어떤 단계든 실패했다면(r!=0) 자원 해제. */
		glfs_fini(fs);             /* [한국어] libgfapi 컨텍스트 파괴 — 내부 소켓, translator 등 모두 정리. */
		fs = NULL;                 /* [한국어] 호출자에게 실패를 알리기 위해 NULL로 덮어쓴다. */
	}
	return fs;                         /* [한국어] 성공 시 유효 포인터, 실패 시 NULL. */
}

/*
 * [한국어]
 * fio_gf_get_glfs - single-instance 옵션을 고려해 glfs_t 인스턴스를 획득한다.
 *
 * @opt:    잡의 gfapi 옵션(gf_single_instance 플래그 보유).
 * @volume: 볼륨 이름 — 캐시 검색 키.
 * @brick:  브릭(서버) 이름 — 캐시 검색 키.
 * @return: 사용 가능한 glfs_t 포인터, 실패 시 NULL.
 *
 * single-instance가 꺼져 있으면 매번 새 fs를 만든다(단순 경로). 켜져 있으면 glfs_lock을
 * 잡고 glfs_list_head를 선형 탐색해 기존 (volume,brick) 엔트리를 찾는다. 있으면 refcount++,
 * 없으면 struct glfs_info를 새로 할당하고 fio_gf_new_fs()로 라이브러리 연결을 만든 뒤
 * 리스트에 추가한다. 잡 시작 시(fio_gf_setup 경로)에만 호출되므로 지연이 긴 glfs_init이
 * I/O 경로 바깥에 있다.
 *
 * 실행 컨텍스트: fio 잡 스레드. 여러 잡이 동시에 호출할 수 있으므로 glfs_lock 필수.
 *
 * 호출 체인:
 *   fio_gf_setup → [fio_gf_get_glfs] → fio_gf_new_fs → libgfapi
 */
static glfs_t *fio_gf_get_glfs(struct gf_options *opt,
			       char *volume, char *brick)
{
	struct glfs_info *glfs = NULL;     /* [한국어] 찾았거나 새로 만든 엔트리. 성공 시 이 포인터의 ->fs를 반환. */
	struct glfs_info *tmp;             /* [한국어] 리스트 순회용 임시 포인터(flist_entry로 컨테이너 복원). */
	struct flist_head *entry;          /* [한국어] flist_for_each 반복자 포인터. */

	if (!opt->gf_single_instance)      /* [한국어] 공유 안 함 — 단순히 새 fs를 만들어 곧장 반환(뮤텍스도 타지 않음). */
		return fio_gf_new_fs(volume, brick);

	pthread_mutex_lock (&glfs_lock);   /* [한국어] 리스트/refcount 보호 — 다른 잡 스레드가 동시 진입 시 직렬화. */

	flist_for_each(entry, &glfs_list_head) { /* [한국어] 전역 캐시 리스트를 head부터 순회. */
		tmp = flist_entry(entry, struct glfs_info, list); /* [한국어] list 노드에서 컨테이너 구조체로 역참조(offset 계산). */
		if (!strcmp(volume, tmp->volume) &&           /* [한국어] 볼륨 일치 && */
		    !strcmp(brick, tmp->brick)) {             /* [한국어] 브릭 일치 → 동일 공유 대상. */
			glfs = tmp;                           /* [한국어] 발견: 이 엔트리를 재사용한다. */
			break;                                /* [한국어] 선형 탐색 조기 종료. */
		}
	}

	if (glfs) {                        /* [한국어] 캐시 적중 — 참조만 늘린다. */
		glfs->refcount++;          /* [한국어] put 시 대응하는 감소가 이뤄지며 0이 되면 진짜 파괴된다. */
	} else {                           /* [한국어] 캐시 미스 — 새 엔트리 생성 + 라이브러리 초기화. */
		glfs = malloc(sizeof(*glfs));                 /* [한국어] 구조체 자체 할당. libgfapi가 아닌 fio 측 메타데이터. */
		if (!glfs)                                     /* [한국어] OOM 시 NULL 반환 경로로. */
			goto out;
		INIT_FLIST_HEAD(&glfs->list);                  /* [한국어] 리스트 노드 자기 참조 초기화 — 아직 어디에도 연결 안 됨. */
		glfs->refcount = 0;                            /* [한국어] 생성 실패 시 0인 채로 free할 수 있도록 보수적 초기화. */
		glfs->volume = strdup(volume);                 /* [한국어] 옵션 문자열의 수명과 독립적인 사본 보관(잡 종료 후에도 안전). */
		glfs->brick = strdup(brick);
		glfs->fs = fio_gf_new_fs(volume, brick);       /* [한국어] 실제 libgfapi 연결 생성(느린 경로 — 네트워크 포함). */
		if (!glfs->fs) {                               /* [한국어] 연결 실패 시 이미 할당한 구조체를 롤백. */
			free(glfs);                            /* [한국어] NOTE: strdup한 volume/brick은 원본 코드가 해제하지 않음(경미한 누수 — 원본 동작 유지). */
			glfs = NULL;
			goto out;
		}

		flist_add_tail(&glfs->list, &glfs_list_head); /* [한국어] 전역 캐시 꼬리에 등록 — 다음 잡이 발견할 수 있게. */
		glfs->refcount = 1;                            /* [한국어] 첫 참조를 1로 설정. 이 호출이 첫 소유자. */
	}

out:
	pthread_mutex_unlock (&glfs_lock); /* [한국어] 임계 영역 종료 — 이후 로컬 변수만 사용. */

	if (glfs)                          /* [한국어] 성공 경로: 엔트리 내부의 libgfapi 핸들만 노출. 래퍼 구조체는 감춘다. */
		return glfs->fs;
	return NULL;                       /* [한국어] 어떤 단계든 실패 시 NULL로 상위에 알림. */
}

/*
 * [한국어]
 * fio_gf_put_glfs - fio_gf_get_glfs로 얻은 glfs_t의 참조를 반납한다.
 *
 * @opt: 잡의 gfapi 옵션. gf_single_instance에 따라 파괴 경로가 갈린다.
 * @fs:  반납할 libgfapi 핸들.
 * @return: 없음.
 *
 * single-instance가 꺼져 있으면 get에서 매번 새로 만들었으므로 무조건 glfs_fini()로 즉시
 * 파괴한다. 켜져 있으면 리스트에서 일치하는 엔트리를 찾아 refcount를 줄이고, 마지막 참조가
 * 사라질 때만 실제 glfs_fini + 문자열 해제 + 리스트 제거를 수행한다. 동시성이 있으므로
 * glfs_lock 필수. 호출 시점은 잡 종료의 fio_gf_cleanup() 경로.
 *
 * 호출 체인:
 *   fio_gf_cleanup → [fio_gf_put_glfs] → glfs_fini (libgfapi)
 */
static void fio_gf_put_glfs(struct gf_options *opt, glfs_t *fs)
{
	struct glfs_info *glfs = NULL;     /* [한국어] 리스트에서 일치시킨 엔트리. 찾지 못하면 로그만 남기고 종료. */
	struct glfs_info *tmp;             /* [한국어] 순회용 컨테이너 포인터. */
	struct flist_head *entry;          /* [한국어] 순회용 리스트 노드 포인터. */

	if (!opt->gf_single_instance) {    /* [한국어] 공유 모드가 아니면 단독 파괴. */
		glfs_fini(fs);             /* [한국어] libgfapi 연결 즉시 해제. */
		return;
	}

	pthread_mutex_lock (&glfs_lock);   /* [한국어] refcount 감소/리스트 제거 보호. */

	flist_for_each(entry, &glfs_list_head) {            /* [한국어] 전역 캐시 순회. */
		tmp = flist_entry(entry, struct glfs_info, list); /* [한국어] 컨테이너 구조체로 복원. */
		if (tmp->fs == fs) {       /* [한국어] 포인터 동일성으로 매칭(문자열 비교 불필요). */
			glfs = tmp;
			break;
		}
	}

	if (!glfs) {                       /* [한국어] 이론상 있을 수 없는 경로 — 버그 감지용 로그. */
		log_err("glfs not found to fini.\n");
	} else {
		glfs->refcount--;          /* [한국어] 참조 1 감소. */

		if (glfs->refcount == 0) { /* [한국어] 마지막 소유자였다면 실제 파괴. */
			glfs_fini(glfs->fs);   /* [한국어] libgfapi 연결 종료. */
			free(glfs->volume);    /* [한국어] strdup 사본 반환. */
			free(glfs->brick);     /* [한국어] strdup 사본 반환. */
			flist_del(&glfs->list);/* [한국어] 캐시에서 제거(다른 잡이 다시 이 포인터를 만나지 않도록). NOTE: free(glfs) 자체는 원본 코드가 빠져 있다(사소한 누수 — 원본 유지). */
		}
	}

	pthread_mutex_unlock (&glfs_lock); /* [한국어] 임계 영역 해제. */
}

/*
 * [한국어]
 * fio_gf_setup - gfapi/gfapi_async 엔진의 ioengine_ops.init 콜백(공통).
 *
 * @td: 잡 컨텍스트. td->eo는 struct gf_options, td->io_ops_data는 이 함수가 채울 엔진 상태.
 * @return: 0 성공, -ENOMEM/-EIO 실패.
 *
 * fio 백엔드가 잡 스레드에서 td_io_init()을 호출할 때 진입한다. struct gf_data를 할당해
 * fd/aio_events/fs를 초기화하고, single-instance 옵션에 따라 공유 또는 전용 glfs_t를 얻는다.
 * 이미 io_ops_data가 있다면(재초기화 호출 등) 무시하고 0 반환. 실패 시 td->io_ops_data=NULL을
 * 보장하여 cleanup 쪽에서 이중 해제하지 않도록 한다.
 *
 * 실행 컨텍스트: 잡 스레드. I/O 루프 시작 전 1회 호출.
 *
 * 호출 체인:
 *   backend.c:thread_main → td_io_init → ioengine_ops.init(=fio_gf_setup)
 *     → fio_gf_get_glfs → fio_gf_new_fs
 */
int fio_gf_setup(struct thread_data *td)
{
	struct gf_data *g = NULL;          /* [한국어] 새로 할당할 엔진별 상태 구조체. 성공 시 td->io_ops_data에 설치. */
	struct gf_options *opt = td->eo;   /* [한국어] 옵션 구조체는 fio 파서가 이미 채워 둔 상태. volume/brick/single-instance를 읽는다. */

	dprint(FD_IO, "fio setup\n");      /* [한국어] FD_IO 카테고리 디버그 — `fio --debug=io` 활성 시에만 출력. */

	if (td->io_ops_data)               /* [한국어] 이미 설치되어 있으면 재초기화를 회피(이상 경로 방어). */
		return 0;

	g = malloc(sizeof(struct gf_data)); /* [한국어] gf_data 할당: fd/aio_events/fs/aio_events_count 등을 담는 잡별 스토리지. */
	if (!g) {                          /* [한국어] OOM 처리: fio는 -ENOMEM을 이 콜백의 실패 코드로 인식. */
		log_err("malloc failed.\n");
		return -ENOMEM;
	}
	g->fd = NULL;                      /* [한국어] open_file 전이라 아직 glfs_fd_t 없음. cleanup이 NULL을 안전히 판별하도록 초기화. */
	g->aio_events = NULL;              /* [한국어] 비동기 엔진(glusterfs_async.c)이 나중에 calloc으로 채움. 동기 엔진은 사용하지 않음. */

	g->fs = fio_gf_get_glfs(opt, opt->gf_vol, opt->gf_brick); /* [한국어] libgfapi 핸들 획득(공유 또는 신규). */
	if (!g->fs)                        /* [한국어] 연결 실패 — cleanup 경로로. */
		goto cleanup;

	dprint(FD_FILE, "fio setup %p\n", g->fs); /* [한국어] FD_FILE 카테고리로 디버그: 성공 포인터를 남겨 여러 잡의 공유를 확인. */
	td->io_ops_data = g;               /* [한국어] fio 엔진 계약: 이 포인터로 queue/getevents/event 등이 상태를 얻는다. */
	return 0;
cleanup:
	free(g);                           /* [한국어] 반쯤 만든 상태 롤백. g->fs는 NULL이라 glfs_fini 불필요. */
	td->io_ops_data = NULL;            /* [한국어] cleanup 콜백 재진입 방어. */
	return -EIO;                       /* [한국어] 일반 I/O 초기화 실패로 신호. */
}

/*
 * [한국어]
 * fio_gf_cleanup - gfapi/gfapi_async 엔진의 ioengine_ops.cleanup 콜백(공통).
 *
 * @td: 잡 컨텍스트.
 * @return: 없음.
 *
 * 잡 스레드가 I/O 루프를 종료한 뒤 td_io_close_file → cleanup 순서로 진입한다.
 * 비동기 엔진이 남긴 aio_events 버퍼와, close_file이 놓친 열린 fd(이상 종료 시), 그리고
 * single-instance 참조 카운트를 차례로 정리한다. 마지막에 gf_data 자체를 free하고
 * td->io_ops_data를 NULL로 리셋하여 잔존 참조를 차단한다.
 *
 * 실행 컨텍스트: 잡 스레드. I/O 루프 종료 후 1회.
 */
void fio_gf_cleanup(struct thread_data *td)
{
	struct gf_data *g = td->io_ops_data; /* [한국어] setup에서 설치된 상태 가져오기. NULL이면 이미 정리됨. */

	if (g) {                           /* [한국어] setup 실패 등으로 NULL인 경우엔 건드릴 것 없음. */
		if (g->aio_events)         /* [한국어] 비동기 엔진이 할당했을 수 있음. 동기 엔진에선 항상 NULL. */
			free(g->aio_events);
		if (g->fd)                 /* [한국어] close_file이 호출 안 된 비정상 종료 방어. */
			glfs_close(g->fd); /* [한국어] libgfapi fd 닫기 — 서버 측 핸들 해제. */
		if (g->fs)                 /* [한국어] fs가 있으면 참조 카운트 감소 또는 파괴. */
			fio_gf_put_glfs(td->eo, g->fs);
		free(g);                   /* [한국어] gf_data 본체 해제. */
		td->io_ops_data = NULL;    /* [한국어] 이중 해제 방지. */
	}
}

/*
 * [한국어]
 * fio_gf_get_file_size - ioengine_ops.get_file_size 콜백(공통).
 *
 * @td: 잡 컨텍스트.
 * @f:  크기를 얻을 대상 파일. 성공 시 f->real_file_size가 채워지고 FIO_FILE_size_known 세팅.
 * @return: 0 성공. 음수 errno 실패.
 *
 * fio는 잡 시작 전 get_file_size로 각 파일의 실제 크기를 확보해 I/O 옵셋 계산에 활용한다.
 * 여기서는 libgfapi의 glfs_lstat(심볼릭 링크 자체의 stat)으로 GlusterFS 상의 크기를 얻는다.
 * g/g->fs가 아직 없으면(드물게 cleanup 후 재호출 등) 조용히 0을 반환한다. 이미 size_known이
 * 세팅된 경우(동일 파일 재조회)도 중복 lstat을 피한다.
 *
 * 호출 체인:
 *   backend.c:setup_files → td_io_get_file_size → [fio_gf_get_file_size] → glfs_lstat
 */
int fio_gf_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct stat buf;                   /* [한국어] glfs_lstat 결과 버퍼. st_size만 사용. */
	int ret;                           /* [한국어] 호출 반환값 보관. */
	struct gf_data *g = td->io_ops_data; /* [한국어] setup이 설치한 상태. */

	dprint(FD_FILE, "get file size %s\n", f->file_name); /* [한국어] 디버그 추적. */

	if (!g || !g->fs) {                /* [한국어] 아직/이미 정리된 상태 — 오류 아님으로 취급. */
		return 0;
	}
	if (fio_file_size_known(f))        /* [한국어] FIO_FILE_size_known 플래그 확인 — 중복 stat 회피. */
		return 0;

	ret = glfs_lstat(g->fs, f->file_name, &buf); /* [한국어] libgfapi lstat — 서버 메타데이터 조회. */
	if (ret < 0) {                     /* [한국어] 권한/경로 오류 등. */
		log_err("glfs_lstat failed.\n");
		return ret;                /* [한국어] 음수 errno 그대로 상위 전달. */
	}

	f->real_file_size = buf.st_size;   /* [한국어] I/O 범위 계산에 쓰일 실제 크기 저장. */
	fio_file_set_size_known(f);        /* [한국어] FIO_FILE_size_known 플래그 세팅 — 이후 호출은 빠르게 스킵. */

	return 0;

}

/*
 * [한국어]
 * fio_gf_open_file - ioengine_ops.open_file 콜백(공통).
 *
 * @td: 잡 컨텍스트. td->o에 read_iolog/write 여부, odirect/sync_io/max_bs 등이 담김.
 * @f:  대상 파일. f->real_file_size는 get_file_size가 이미 채워 둔 상태.
 * @return: 0 성공, 비0 실패(errno 계열).
 *
 * 각 fio_file에 대해 한 번씩 호출된다. 잡의 rw 방향과 read_only 전역에 따라 플래그를 조립해
 * glfs_creat(없으면 생성, 있으면 열기)로 glfs_fd_t를 얻고 g->fd에 저장한다. 읽기 잡이면서
 * 파일이 없거나 too small이면 glfs_ftruncate로 확장한 뒤 fill_io_buffer/glfs_write로
 * 내용을 채워 실제 read I/O가 유효한 데이터를 보도록 만든다(verify 없이도 의미 있는 read
 * 성능 측정). fadvise 지원 빌드(GFAPI_USE_FADVISE)에선 random/sequential 힌트를 준다.
 * f->fd/shadow_fd를 -1로 남겨 두는 이유는 fio 상위가 이 값을 POSIX fd로 오해하지 않도록
 * 하기 위함이다 — 실제 핸들은 g->fd(glfs_fd_t)에 있다.
 *
 * 실행 컨텍스트: 잡 스레드. setup 이후 I/O 루프 진입 직전에 파일당 1회.
 *
 * 호출 체인:
 *   backend.c:setup_files → td_io_open_file → [fio_gf_open_file]
 *     → glfs_creat/glfs_ftruncate/glfs_write/glfs_fadvise
 */
int fio_gf_open_file(struct thread_data *td, struct fio_file *f)
{

	int flags = 0;                     /* [한국어] glfs_creat에 넘길 open flags. 쓰기/읽기/direct/sync 조합. */
	int ret = 0;                       /* [한국어] 반환값 누적. */
	struct gf_data *g = td->io_ops_data; /* [한국어] setup이 설치한 잡 상태. */
	struct stat sb = { 0, };           /* [한국어] 기존 파일 크기 확인용. 없으면 lstat이 실패하여 확장 경로로. */

	if (td_write(td)) {                /* [한국어] 쓰기가 포함된 잡인지 확인(verify 쓰기 포함). */
		if (!read_only)            /* [한국어] 전역 read_only가 아니면 RDWR로 열어 read-modify-write 대비. */
			flags = O_RDWR;
	} else if (td_read(td)) {          /* [한국어] 순수 읽기 잡. */
		if (!read_only)            /* [한국어] 파일이 없으면 창고 채우기 위해 RDWR 필요(아래 확장 로직). */
			flags = O_RDWR;
		else
			flags = O_RDONLY;  /* [한국어] 전역 read_only에선 절대 쓸 수 없음 — 확장 불가. */
	}

	if (td->o.odirect)                 /* [한국어] direct I/O 요청 시 플래그 추가. OS별 정의에 맞춰 OS_O_DIRECT. */
		flags |= OS_O_DIRECT;      /* [한국어] GlusterFS는 translator 레벨에서 O_DIRECT를 인식(지원 여부는 볼륨 설정). */
	flags |= td->o.sync_io;            /* [한국어] sync_io 옵션(O_SYNC/O_DSYNC 등)을 그대로 OR. */

	dprint(FD_FILE, "fio file %s open mode %s td rw %s\n", f->file_name,
	       flags & O_RDONLY ? "ro" : "rw", td_read(td) ? "read" : "write"); /* [한국어] 열기 직전 상태 추적. */
	/* [한국어] glfs_creat: POSIX creat 의미. 없으면 0644로 생성, 있으면 flags로 열기.
	 *  반환은 glfs_fd_t(불투명). POSIX int fd가 아님. */
	g->fd = glfs_creat(g->fs, f->file_name, flags, 0644);
	if (!g->fd) {                      /* [한국어] 생성/열기 실패 — 권한, 공간, 네트워크 등. */
		ret = errno;               /* [한국어] libgfapi는 errno 관행 따름. */
		log_err("glfs_creat failed.\n");
		return ret;
	}
	/* file for read doesn't exist or shorter than required, create/extend it */
	/* [한국어] 읽기 잡에서 파일이 없거나 크기가 부족하면 필요한 크기까지 확장한다. */
	if (td_read(td)) {
		if (glfs_lstat(g->fs, f->file_name, &sb) /* [한국어] 실패(없음) 또는 */
		    || sb.st_size < f->real_file_size) { /* [한국어] 요구 크기 미만이면 확장 필요. */
			dprint(FD_FILE, "fio extend file %s from %jd to %" PRIu64 "\n",
			       f->file_name, (intmax_t) sb.st_size, f->real_file_size); /* [한국어] 확장 전/후 크기 디버그. */
#if defined(CONFIG_GF_NEW_API)
			/* [한국어] 신규 API: 확장 시 pre/post stat을 받아올 수 있는 4인자 변형.
			 *  여기서는 둘 다 NULL로 생략. configure 단계에서 감지. */
			ret = glfs_ftruncate(g->fd, f->real_file_size, NULL, NULL);
#else
			/* [한국어] 레거시 API: 2인자 버전. GlusterFS 구버전 호환. */
			ret = glfs_ftruncate(g->fd, f->real_file_size);
#endif
			if (ret) {         /* [한국어] 확장 실패 — 로그만 남기고 계속(치명적 중단 아님). */
				log_err("failed fio extend file %s to %" PRIu64 "\n",
					f->file_name, f->real_file_size);
			} else {
				unsigned long long left;     /* [한국어] 아직 채워야 할 바이트 수(카운트다운). */
				unsigned int bs;             /* [한국어] 이번 루프의 쓰기 크기. */
				char *b;                     /* [한국어] 쓰기 버퍼. */
				int r;                       /* [한국어] glfs_write 반환값. */

				/* fill the file, copied from extend_file */
				/* [한국어] ftruncate만 쓰면 sparse 파일이 되어 read 성능 측정이 왜곡됨.
				 *  따라서 fio 공통 유틸 extend_file()의 로직을 복사해 실제 데이터로 채운다. */
				b = malloc(td->o.max_bs[DDIR_WRITE]); /* [한국어] 최대 쓰기 블록 크기만큼 버퍼 확보. */

				left = f->real_file_size;    /* [한국어] 처음엔 전체 크기만큼 남음. */
				while (left && !td->terminate) { /* [한국어] 종료 신호가 오면 즉시 중단. */
					bs = td->o.max_bs[DDIR_WRITE]; /* [한국어] 기본은 최대치. */
					if (bs > left)       /* [한국어] 마지막 잔여가 더 작으면 그만큼만. */
						bs = left;

					fill_io_buffer(td, b, bs, bs); /* [한국어] fio 공통: verify 패턴/난수/제로 등 옵션에 따른 채움. */

					r = glfs_write(g->fd, b, bs, 0); /* [한국어] libgfapi 동기 쓰기. 마지막 0은 flags. */
					dprint(FD_IO,
					       "fio write %d of %" PRIu64 " file %s\n",
					       r, f->real_file_size,
					       f->file_name);

					if (r > 0) {          /* [한국어] 부분 성공 포함 — 쓴 만큼 차감하고 계속. */
						left -= r;
						continue;
					} else {
						if (r < 0) {  /* [한국어] 에러 경로 — errno 확인. */
							int __e = errno;

							if (__e == ENOSPC) { /* [한국어] 공간 부족: fill_device면 의도적 계속, 아니면 중단. */
								if (td->o.
								    fill_device)
									break;
								log_info
								    ("fio: ENOSPC on laying out "
								     "file, stopping\n");
								break;
							}
							td_verror(td, errno,
								  "write"); /* [한국어] fio에 에러 기록 — 최종 통계에 반영. */
						} else
							td_verror(td, EIO, /* [한국어] r==0 short-write는 보통 발생 X. 방어적 EIO 기록. */
								  "write");

						break;
					}
				}

				if (b)                       /* [한국어] 성공/실패 무관하게 버퍼 해제. */
					free(b);
				glfs_lseek(g->fd, 0, SEEK_SET); /* [한국어] read I/O가 파일 시작부터 읽도록 오프셋 리셋. */

				if (td->terminate && td->o.unlink) { /* [한국어] 종료 + unlink 옵션: 채우다 중단된 임시 파일 제거. */
					dprint(FD_FILE, "terminate unlink %s\n",
					       f->file_name);
					glfs_unlink(g->fs, f->file_name);
				} else if (td->o.create_fsync) {     /* [한국어] 채운 뒤 fsync 옵션 지정 시. */
#if defined(CONFIG_GF_NEW_API)
					/* [한국어] 신규 API: pre/post stat 포인터 포함. */
					if (glfs_fsync(g->fd, NULL, NULL) < 0) {
#else
					/* [한국어] 레거시 API: fd만 받는 버전. */
					if (glfs_fsync(g->fd) < 0) {
#endif
						dprint(FD_FILE,
						       "failed to sync, close %s\n",
						       f->file_name);
						td_verror(td, errno, "fsync");    /* [한국어] 통계에 fsync 실패 기록. */
						glfs_close(g->fd);                /* [한국어] 실패 시 롤백: fd 닫고 NULL로. */
						g->fd = NULL;
						return 1;                          /* [한국어] open_file 실패로 상위 보고(0 아님). */
					}
				}
			}
		}
	}
#if defined(GFAPI_USE_FADVISE)
	/* [한국어] GFAPI_USE_FADVISE 빌드: glfs_fadvise로 random/sequential 힌트 전달.
	 *  GlusterFS translator가 read-ahead 전략을 조정하도록 유도. */
	{
		int r = 0;                 /* [한국어] fadvise 반환값. */
		if (td_random(td)) {       /* [한국어] 랜덤 잡 — read-ahead 억제 힌트. */
			r = glfs_fadvise(g->fd, 0, f->real_file_size,
					 POSIX_FADV_RANDOM);
		} else {                   /* [한국어] 순차 잡 — read-ahead 활성화 힌트. */
			r = glfs_fadvise(g->fd, 0, f->real_file_size,
					 POSIX_FADV_SEQUENTIAL);
		}
		if (r) {                   /* [한국어] 실패는 디버그로만 기록(비치명). */
			dprint(FD_FILE, "fio %p fadvise %s status %d\n", g->fs,
			       f->file_name, r);
		}
	}
#endif
	dprint(FD_FILE, "fio %p created %s\n", g->fs, f->file_name); /* [한국어] open 완료 트레이스. */
	f->fd = -1;                        /* [한국어] fio 상위가 POSIX fd로 오해하지 않도록 -1. 실제 핸들은 g->fd. */
	f->shadow_fd = -1;                 /* [한국어] shadow(병렬 복제 파일) fd도 사용 안 함 — -1. */
	td->o.open_files ++;               /* [한국어] fio의 열린 파일 카운터 증가 — 통계/관리 용. */
	return ret;                        /* [한국어] 여기까지 도달하면 보통 0. ftruncate 실패 시 비0일 수 있음. */
}

/*
 * [한국어]
 * fio_gf_close_file - ioengine_ops.close_file 콜백(공통).
 *
 * @td: 잡 컨텍스트.
 * @f:  닫을 파일.
 * @return: 0 성공, 양수 errno 실패.
 *
 * I/O 루프 종료 후 파일당 1회 호출. g->fd가 있으면 glfs_close로 서버 측 핸들을 반납한다.
 * 실패 시 errno를 그대로 반환. g->fd는 NULL로 리셋하여 cleanup 단계의 중복 close를 막는다.
 *
 * 호출 체인:
 *   backend.c:close_files → td_io_close_file → [fio_gf_close_file] → glfs_close
 */
int fio_gf_close_file(struct thread_data *td, struct fio_file *f)
{
	int ret = 0;                       /* [한국어] 반환 코드. */
	struct gf_data *g = td->io_ops_data; /* [한국어] 잡별 상태. */

	dprint(FD_FILE, "fd close %s\n", f->file_name); /* [한국어] 디버그 추적. */

	if (g) {                           /* [한국어] cleanup 이후 재진입 방어. */
		if (g->fd && glfs_close(g->fd) < 0) /* [한국어] 열려 있으면 닫기. 실패 시 errno 수집. */
			ret = errno;
		g->fd = NULL;              /* [한국어] 이중 close 방지. */
	}

	return ret;
}

/*
 * [한국어]
 * fio_gf_unlink_file - ioengine_ops.unlink_file 콜백(공통).
 *
 * @td: 잡 컨텍스트.
 * @f:  삭제할 파일.
 * @return: 0 성공, 양수 errno(close 실패 시).
 *
 * 잡 완료 후 unlink 옵션이 지정된 경우 호출된다. fd가 열려 있으면 먼저 닫고, glfs_unlink로
 * 파일을 제거한 뒤 glfs_fini로 라이브러리 연결까지 종료하고 gf_data를 해제한다. 주의:
 * 이 함수는 single-instance 참조 카운트를 거치지 않고 즉시 glfs_fini를 호출하므로 공유
 * 모드에서는 다른 잡의 fs 포인터를 끊을 수 있는 위험이 있다(원본 동작 그대로, 버그 가능성).
 *
 * 호출 체인:
 *   backend.c:unlink_all_files → td_io_unlink_file → [fio_gf_unlink_file]
 *     → glfs_close/glfs_unlink/glfs_fini
 */
int fio_gf_unlink_file(struct thread_data *td, struct fio_file *f)
{
	int ret = 0;                       /* [한국어] 반환값. close 실패 외에는 성공 처리. */
	struct gf_data *g = td->io_ops_data; /* [한국어] 잡별 상태. */

	dprint(FD_FILE, "fd unlink %s\n", f->file_name); /* [한국어] 디버그. */

	if (g) {
		if (g->fd && glfs_close(g->fd) < 0) /* [한국어] 열려 있으면 먼저 닫기 — unlink 전에 핸들 해제 필요. */
			ret = errno;

		glfs_unlink(g->fs, f->file_name);   /* [한국어] GlusterFS 상의 파일 삭제. */

		if (g->fs)                 /* [한국어] fs 포인터가 살아 있으면 즉시 종료.
						 *  주의: single-instance 공유 중이라면 참조 카운트를 우회하여 파괴하는 셈. */
			glfs_fini(g->fs);

		g->fd = NULL;              /* [한국어] 사후 재참조 방지. */
		free(g);                   /* [한국어] 상태 본체 해제. */
	}
	td->io_ops_data = NULL;            /* [한국어] cleanup 중복 진입 시 NULL 가드 발동. */

	return ret;
}
