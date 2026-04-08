/*
 * The io parts of the fio tool, includes workers for sync and mmap'ed
 * io, as well as both posix and linux libaio support.
 *
 * sync io is implemented on top of aio.
 *
 * This is not really specific to fio, if the get_io_u/put_io_u and
 * structures was pulled into this as well it would be a perfectly
 * generic io engine that could be used for other projects.
 *
 */
/*
 * [한국어 설명]
 * fio 도구의 I/O 엔진 프레임워크 핵심 파일이다.
 * 이 파일은 I/O 엔진의 로딩, 등록/해제, 초기화/종료,
 * 그리고 I/O 요청의 준비(prep) -> 큐잉(queue) -> 커밋(commit) -> 완료 수집(getevents)
 * 전체 생명주기를 관리하는 중간 계층(glue layer)을 제공한다.
 *
 * 각 I/O 엔진(libaio, io_uring, sync 등)은 ioengine_ops 구조체의 콜백 함수를
 * 구현하여 이 프레임워크에 등록한다. td_io_*() 함수들은 상위 계층(backend.c 등)에서
 * 호출되며, 내부적으로 등록된 엔진의 콜백을 호출한다.
 *
 * ioengine_ops 구조체의 주요 콜백 호출 시점:
 *   - init()       : td_io_init()에서 호출. 엔진별 초기화 (예: io_uring 링 설정)
 *   - prep()       : td_io_prep()에서 호출. I/O 요청 제출 전 준비 작업
 *   - queue()      : td_io_queue()에서 호출. 실제 I/O 요청을 엔진에 제출
 *   - commit()     : td_io_commit()에서 호출. 큐에 쌓인 요청들을 OS에 일괄 제출
 *   - getevents()  : td_io_getevents()에서 호출. 완료된 I/O 이벤트 수집
 *   - event()      : 개별 완료 이벤트에서 io_u를 가져올 때 호출
 *   - open_file()  : td_io_open_file()에서 호출. 파일 열기
 *   - close_file() : td_io_close_file()에서 호출. 파일 닫기
 *   - cleanup()    : close_ioengine()에서 호출. 엔진 자원 해제
 *   - get_file_size(): td_io_get_file_size()에서 호출. 파일 크기 조회
 *   - unlink_file(): td_io_unlink_file()에서 호출. 파일 삭제
 *
 * I/O 흐름 요약:
 *   load_ioengine() -> td_io_init() -> [td_io_prep() -> td_io_queue()
 *   -> td_io_commit() -> td_io_getevents()] (반복) -> close_ioengine()
 */

/* 표준 라이브러리 헤더 */
#include <stdlib.h>    /* malloc, free 등 메모리 관리 함수 */
#include <unistd.h>    /* unlink, close 등 POSIX 시스템 호출 */
#include <string.h>    /* strcmp, strncmp 등 문자열 처리 함수 */
#include <dlfcn.h>     /* dlopen, dlsym, dlclose 등 동적 라이브러리 로딩 함수 */
#include <fcntl.h>     /* fcntl, F_SET_RW_HINT 등 파일 제어 함수 */
#include <assert.h>    /* assert 매크로 - 디버그용 단언문 */
#include <sys/types.h> /* 시스템 타입 정의 */
#include <dirent.h>    /* opendir, readdir 등 디렉토리 탐색 함수 */
#include <errno.h>     /* errno 에러 코드 */

/* fio 내부 헤더 */
#include "fio.h"       /* fio 핵심 데이터 구조와 함수 선언 */
#include "diskutil.h"  /* 디스크 유틸리티 (사용량 추적 등) */
#include "zbd.h"       /* 존 블록 디바이스(ZBD) 지원 */

/*
 * engine_list: 등록된 모든 I/O 엔진을 보관하는 전역 연결 리스트의 헤드
 * [한국어] 각 I/O 엔진(libaio, sync, io_uring 등)이 register_ioengine()을 통해
 * 이 리스트에 추가된다. find_ioengine()은 이 리스트를 순회하여 이름으로 엔진을 찾는다.
 */
static FLIST_HEAD(engine_list);

/*
 * [한국어]
 * async_ioengine_sync_trim - 비동기 엔진에서 TRIM 작업을 동기적으로 처리해야 하는지 확인
 *
 * @td: 스레드 데이터 구조체 (현재 워커 스레드의 상태 정보)
 * @io_u: I/O 유닛 구조체 (개별 I/O 요청을 나타냄)
 * @return: TRIM을 동기적으로 처리해야 하면 true
 *
 * 일부 비동기 I/O 엔진(예: io_uring)은 TRIM(DISCARD) 명령을 비동기적으로
 * 처리하지 못한다. 이 경우 FIO_ASYNCIO_SYNC_TRIM 플래그가 설정되며,
 * TRIM 요청은 동기 경로를 통해 처리된다.
 */
static inline bool async_ioengine_sync_trim(struct thread_data *td,
					    struct io_u	*io_u)
{
	/* 엔진에 FIO_ASYNCIO_SYNC_TRIM 플래그가 있고, I/O 방향이 TRIM인지 확인 */
	return td_ioengine_flagged(td, FIO_ASYNCIO_SYNC_TRIM) &&
		io_u->ddir == DDIR_TRIM;
}

/*
 * [한국어]
 * async_ioengine_sync_syncfs - 비동기 엔진에서 SYNCFS 작업을 동기적으로 처리해야 하는지 확인
 *
 * @td: 스레드 데이터 구조체
 * @io_u: I/O 유닛 구조체
 * @return: SYNCFS를 동기적으로 처리해야 하면 true
 *
 * TRIM과 유사하게, SYNCFS(파일시스템 전체 동기화) 명령도 일부 비동기 엔진에서
 * 동기적으로 처리해야 할 수 있다.
 */
static inline bool async_ioengine_sync_syncfs(struct thread_data *td,
					      struct io_u *io_u)
{
	/* 엔진에 FIO_ASYNCIO_SYNC_SYNCFS 플래그가 있고, I/O 방향이 SYNCFS인지 확인 */
	return td_ioengine_flagged(td, FIO_ASYNCIO_SYNC_SYNCFS) &&
		io_u->ddir == DDIR_SYNCFS;
}

/*
 * [한국어]
 * check_engine_ops - I/O 엔진의 콜백 함수 구성이 올바른지 검증
 *
 * @td: 스레드 데이터 구조체
 * @ops: 검증할 I/O 엔진의 오퍼레이션 구조체
 * @return: 오류가 있으면 true, 정상이면 false
 *
 * I/O 엔진 로딩 시(load_ioengine 내부에서) 호출되어, 엔진이 필수 콜백을
 * 올바르게 구현했는지 확인한다.
 *
 * 검증 항목:
 *   1. 엔진의 API 버전이 현재 fio와 호환되는지 확인
 *   2. queue() 콜백이 반드시 존재해야 함 (모든 엔진의 필수 콜백)
 *   3. 동기(sync) 엔진은 queue()만 있으면 충분
 *   4. 비동기 엔진이 offload 모드와 호환되는지 확인
 *   5. 비동기 엔진은 event()와 getevents() 콜백도 반드시 필요
 */
static bool check_engine_ops(struct thread_data *td, struct ioengine_ops *ops)
{
	/* 엔진의 ioops 버전이 현재 fio가 기대하는 버전과 일치하는지 확인 */
	if (ops->version != FIO_IOOPS_VERSION) {
		log_err("bad ioops version %d (want %d)\n", ops->version,
							FIO_IOOPS_VERSION);
		return true; /* 버전 불일치: 오류 */
	}

	/* queue() 콜백은 모든 I/O 엔진에서 필수 - I/O 요청을 처리하는 핵심 함수 */
	if (!ops->queue) {
		log_err("%s: no queue handler\n", ops->name);
		return true; /* queue 핸들러 없음: 오류 */
	}

	/*
	 * sync engines only need a ->queue()
	 */
	/* [한국어] 동기 엔진(FIO_SYNCIO)은 queue()만 있으면 된다.
	 * 동기 엔진에서는 queue() 호출 시 I/O가 즉시 완료되므로
	 * event()/getevents() 같은 비동기 완료 수집 콜백이 불필요하다. */
	if (ops->flags & FIO_SYNCIO)
		return false; /* 동기 엔진은 추가 검증 불필요: 정상 */

	/*
	 * async engines aren't reliable with offload
	 */
	/* [한국어] 비동기 엔진 중 FIO_NO_OFFLOAD 플래그가 있는 엔진은
	 * offload 제출 모드(IO_MODE_OFFLOAD)와 함께 사용할 수 없다.
	 * offload 모드에서는 별도의 스레드가 I/O를 제출하는데,
	 * 일부 비동기 엔진은 이를 안정적으로 지원하지 못한다. */
	if ((td->o.io_submit_mode == IO_MODE_OFFLOAD) &&
	    (ops->flags & FIO_NO_OFFLOAD)) {
		log_err("%s: can't be used with offloaded submit. Use a sync "
			"engine\n", ops->name);
		return true; /* offload 모드 비호환: 오류 */
	}

	/* 비동기 엔진은 event()와 getevents() 콜백이 필수
	 * event(): 완료된 개별 이벤트에서 io_u를 추출
	 * getevents(): 완료된 이벤트 묶음을 OS로부터 가져옴 */
	if (!ops->event || !ops->getevents) {
		log_err("%s: no event/getevents handler\n", ops->name);
		return true; /* event/getevents 핸들러 없음: 오류 */
	}

	return false; /* 모든 검증 통과: 정상 */
}

/*
 * [한국어]
 * unregister_ioengine - I/O 엔진을 전역 엔진 리스트에서 제거
 *
 * @ops: 제거할 I/O 엔진의 오퍼레이션 구조체
 *
 * 엔진의 리스트 노드를 engine_list에서 분리하여 등록을 해제한다.
 * 주로 동적으로 로드된 외부 엔진이 언로드될 때 호출된다.
 */
void unregister_ioengine(struct ioengine_ops *ops)
{
	dprint(FD_IO, "ioengine %s unregistered\n", ops->name);
	/* flist_del_init: 리스트에서 노드를 제거하고, 자기 자신을 가리키도록 초기화 */
	flist_del_init(&ops->list);
}

/*
 * [한국어]
 * register_ioengine - I/O 엔진을 전역 엔진 리스트에 등록
 *
 * @ops: 등록할 I/O 엔진의 오퍼레이션 구조체
 *
 * 엔진의 리스트 노드를 engine_list의 끝(tail)에 추가한다.
 * 내장 엔진은 fio 시작 시 자동으로, 외부 엔진은 dlopen 후에 호출된다.
 * 각 I/O 엔진 소스 파일(예: engines/libaio.c)의 fio_init 속성 함수에서
 * 이 함수를 호출하여 자신을 등록한다.
 */
void register_ioengine(struct ioengine_ops *ops)
{
	dprint(FD_IO, "ioengine %s registered\n", ops->name);
	/* engine_list의 끝에 새 엔진을 추가 */
	flist_add_tail(&ops->list, &engine_list);
}

/*
 * [한국어]
 * find_ioengine - 이름으로 등록된 I/O 엔진을 검색
 *
 * @name: 찾고자 하는 엔진의 이름 (예: "libaio", "sync", "io_uring")
 * @return: 찾은 엔진의 ioengine_ops 포인터, 없으면 NULL
 *
 * engine_list를 순회하며 이름이 일치하는 엔진을 찾아 반환한다.
 * __load_ioengine()에서 호출되며, 이는 load_ioengine()의 내부 함수이다.
 */
static struct ioengine_ops *find_ioengine(const char *name)
{
	struct ioengine_ops *ops;       /* 현재 순회 중인 엔진의 ops 포인터 */
	struct flist_head *entry;       /* 리스트 순회용 포인터 */

	/* engine_list의 모든 항목을 순회 */
	flist_for_each(entry, &engine_list) {
		/* 리스트 엔트리에서 ioengine_ops 구조체 포인터를 추출
		 * (container_of 매크로와 유사한 역할) */
		ops = flist_entry(entry, struct ioengine_ops, list);
		/* 이름이 일치하면 해당 엔진을 반환 */
		if (!strcmp(name, ops->name))
			return ops;
	}

	return NULL; /* 이름이 일치하는 엔진을 찾지 못함 */
}

/* CONFIG_DYNAMIC_ENGINES가 정의된 경우에만 외부 엔진 동적 로딩 지원 */
#ifdef CONFIG_DYNAMIC_ENGINES
/*
 * [한국어]
 * dlopen_external - 외부 I/O 엔진 공유 라이브러리(.so)를 동적으로 로드
 *
 * @td: 스레드 데이터 구조체
 * @engine: 엔진 이름 (예: "myengine" -> FIO_EXT_ENG_DIR/fio-myengine.so를 로드)
 * @return: dlopen 핸들, 실패 시 NULL
 *
 * FIO_EXT_ENG_DIR 디렉토리에서 "fio-<engine>.so" 형식의 공유 라이브러리를 찾아
 * 동적으로 로드한다. 외부 엔진 패키지가 설치되어 있을 때 사용된다.
 */
static void *dlopen_external(struct thread_data *td, const char *engine)
{
	char engine_path[PATH_MAX]; /* 엔진 .so 파일의 전체 경로를 저장할 버퍼 */
	void *dlhandle;             /* dlopen이 반환하는 핸들 */

	/* 엔진 경로 생성: FIO_EXT_ENG_DIR/fio-<엔진이름>.so */
	sprintf(engine_path, "%s/fio-%s.so", FIO_EXT_ENG_DIR, engine);

	dprint(FD_IO, "dlopen external %s\n", engine_path);
	/* RTLD_LAZY: 심볼을 사용할 때 지연 로딩 (성능 최적화) */
	dlhandle = dlopen(engine_path, RTLD_LAZY);
	/* 로드 실패 시 안내 메시지 출력 */
	if (!dlhandle)
		log_info("Engine %s not found; Either name is invalid, was not built, or fio-engine-%s package is missing.\n",
			 engine, engine);

	return dlhandle;
}
#else
/* 동적 엔진이 비활성화된 경우, dlopen_external은 항상 NULL을 반환하는 매크로 */
#define dlopen_external(td, engine) (NULL)
#endif

/*
 * [한국어]
 * dlopen_ioengine - I/O 엔진을 동적 라이브러리(.so)로 로드하고 ops 구조체를 가져옴
 *
 * @td: 스레드 데이터 구조체
 * @engine_lib: 엔진 라이브러리 이름 또는 경로
 * @return: 로드된 엔진의 ioengine_ops 포인터, 실패 시 NULL
 *
 * 이 함수는 load_ioengine()에서 내장 엔진을 찾지 못했을 때 호출된다.
 * dlopen()으로 공유 라이브러리를 로드한 후, dlsym()으로 ioengine_ops 구조체를
 * 찾아 반환한다. 외부 엔진은 3가지 방법으로 ops를 노출할 수 있다:
 *   1. 엔진 이름과 동일한 전역 심볼
 *   2. "ioengine"이라는 전역 심볼
 *   3. get_ioengine() 함수를 통한 동적 할당 (C++ 엔진 등)
 */
static struct ioengine_ops *dlopen_ioengine(struct thread_data *td,
					    const char *engine_lib)
{
	struct ioengine_ops *ops; /* 로드된 엔진의 오퍼레이션 구조체 포인터 */
	void *dlhandle;           /* dlopen이 반환하는 공유 라이브러리 핸들 */

	/* "linuxaio"나 "aio"라는 이름은 "libaio"의 별칭으로 처리 */
	if (!strncmp(engine_lib, "linuxaio", 8) ||
	    !strncmp(engine_lib, "aio", 3))
		engine_lib = "libaio";

	dprint(FD_IO, "dlopen engine %s\n", engine_lib);

	/* 이전 dlerror 상태를 초기화 */
	dlerror();
	/* 먼저 직접 dlopen 시도 (시스템 라이브러리 경로에서 검색) */
	dlhandle = dlopen(engine_lib, RTLD_LAZY);
	if (!dlhandle) {
		/* 직접 로드 실패 시, 외부 엔진 디렉토리에서 시도 */
		dlhandle = dlopen_external(td, engine_lib);
		if (!dlhandle) {
			/* 외부 디렉토리에서도 실패하면 오류 반환 */
			td_vmsg(td, -1, dlerror(), "dlopen");
			return NULL;
		}
	}

	/*
	 * Unlike the included modules, external engines should have a
	 * non-static ioengine structure that we can reference.
	 */
	/* [한국어] 내장 모듈과 달리, 외부 엔진은 non-static ioengine 구조체를
	 * 제공해야 한다. 먼저 엔진 이름과 동일한 심볼을 찾아본다. */
	ops = dlsym(dlhandle, engine_lib);
	/* 엔진 이름 심볼이 없으면 "ioengine"이라는 범용 심볼 이름으로 시도 */
	if (!ops)
		ops = dlsym(dlhandle, "ioengine");

	/*
	 * For some external engines (like C++ ones) it is not that trivial
	 * to provide a non-static ionengine structure that we can reference.
	 * Instead we call a method which allocates the required ioengine
	 * structure.
	 */
	/* [한국어] C++ 등으로 작성된 외부 엔진은 전역 구조체를 제공하기 어려울 수 있다.
	 * 이 경우 get_ioengine() 함수를 통해 동적으로 ops 구조체를 할당받는다. */
	if (!ops) {
		/* get_ioengine 함수 심볼을 찾아 호출 */
		get_ioengine_t get_ioengine = dlsym(dlhandle, "get_ioengine");

		if (get_ioengine)
			get_ioengine(&ops); /* ops 포인터에 할당된 구조체 주소를 저장 */
	}

	/* 3가지 방법 모두 실패하면 오류 처리 */
	if (!ops) {
		td_vmsg(td, -1, dlerror(), "dlsym");
		dlclose(dlhandle); /* 로드한 라이브러리 핸들 해제 */
		return NULL;
	}

	/* dlhandle을 ops에 저장하여 나중에 dlclose()로 정리할 수 있게 함 */
	ops->dlhandle = dlhandle;
	return ops;
}

/*
 * [한국어]
 * __load_ioengine - 내장(정적 등록된) I/O 엔진을 이름으로 검색
 *
 * @engine: 엔진 이름 문자열
 * @return: 찾은 엔진의 ioengine_ops 포인터, 없으면 NULL
 *
 * load_ioengine()의 내부 헬퍼 함수로, 이미 등록된(engine_list에 있는) 엔진을
 * 이름으로 찾는다. linuxaio/aio 별칭을 libaio로 변환하는 처리도 포함한다.
 */
static struct ioengine_ops *__load_ioengine(const char *engine)
{
	/*
	 * linux libaio has alias names, so convert to what we want
	 */
	/* [한국어] "linuxaio"와 "aio"는 "libaio"의 별칭이므로 변환 */
	if (!strncmp(engine, "linuxaio", 8) || !strncmp(engine, "aio", 3)) {
		dprint(FD_IO, "converting ioengine name: %s -> libaio\n",
		       engine);
		engine = "libaio";
	}

	dprint(FD_IO, "load ioengine %s\n", engine);
	/* find_ioengine()을 호출하여 engine_list에서 이름으로 검색 */
	return find_ioengine(engine);
}

/*
 * [한국어]
 * load_ioengine - I/O 엔진을 로드하는 최상위 함수 (fio I/O 흐름의 첫 번째 단계)
 *
 * @td: 스레드 데이터 구조체 (td->o.ioengine에 엔진 이름이 들어 있음)
 * @return: 로드된 엔진의 ioengine_ops 포인터, 실패 시 NULL
 *
 * I/O 흐름에서의 위치:
 *   [1] load_ioengine() <-- 현재 함수 (엔진 로드)
 *   [2] td_io_init()           (엔진 초기화)
 *   [3] td_io_open_file()      (파일 열기)
 *   [4] td_io_prep()           (I/O 요청 준비)
 *   [5] td_io_queue()          (I/O 요청 큐잉)
 *   [6] td_io_commit()         (큐잉된 요청 일괄 제출)
 *   [7] td_io_getevents()      (완료된 이벤트 수집)
 *   [8] td_io_close_file()     (파일 닫기)
 *   [9] close_ioengine()       (엔진 종료 및 자원 해제)
 *
 * 로딩 전략:
 *   1. 먼저 __load_ioengine()으로 내장 엔진 리스트에서 검색
 *   2. 찾지 못하거나 동적 라이브러리 엔진이면 dlopen_ioengine()으로 .so 파일 로드
 *   3. 로드 후 check_engine_ops()로 필수 콜백 검증
 */
struct ioengine_ops *load_ioengine(struct thread_data *td)
{
	struct ioengine_ops *ops = NULL; /* 로드될 엔진의 ops 포인터 */
	const char *name;                /* 로드할 엔진의 이름 또는 경로 */

	/*
	 * Use ->ioengine_so_path if an external ioengine path is specified.
	 * In this case, ->ioengine is "external" which also means the prefix
	 * for external ioengines "external:" is properly used.
	 */
	/* [한국어] 외부 엔진 경로(ioengine_so_path)가 지정된 경우 그것을 사용하고,
	 * 아니면 엔진 이름(ioengine)을 사용한다. ?: 는 GNU 확장 삼항 연산자로,
	 * ioengine_so_path가 NULL이 아니면 그 값을, NULL이면 ioengine 값을 사용한다. */
	name = td->o.ioengine_so_path ?: td->o.ioengine;

	/*
	 * Try to load ->ioengine first, and if failed try to dlopen(3) either
	 * ->ioengine or ->ioengine_so_path.  This is redundant for an external
	 * ioengine with prefix, and also leaves the possibility of unexpected
	 * behavior (e.g. if the "external" ioengine exists), but we do this
	 * so as not to break job files not using the prefix.
	 */
	/* [한국어] 먼저 내장 엔진 리스트에서 검색 시도.
	 * 외부 엔진의 경우 중복 시도가 되지만, 기존 job 파일 호환성을 위해 이렇게 한다. */
	ops = __load_ioengine(td->o.ioengine);

	/* We do re-dlopen existing handles, for reference counting */
	/* [한국어] ops가 NULL이거나(내장 엔진에 없음), dlhandle이 설정되어 있으면
	 * (동적 라이브러리 엔진이면) dlopen으로 로드를 시도한다.
	 * 이미 로드된 핸들도 re-dlopen하여 참조 카운트를 증가시킨다. */
	if (!ops || ops->dlhandle)
		ops = dlopen_ioengine(td, name);

	/*
	 * If ops is NULL, we failed to load ->ioengine, and also failed to
	 * dlopen(3) either ->ioengine or ->ioengine_so_path as a path.
	 */
	/* [한국어] 내장 검색과 dlopen 모두 실패하면 엔진을 로드할 수 없음 */
	if (!ops) {
		log_err("fio: engine %s not loadable\n", name);
		return NULL;
	}

	/*
	 * Check that the required methods are there.
	 */
	/* [한국어] 엔진의 필수 콜백(queue, event, getevents 등)이 올바르게 구현되었는지 검증 */
	if (check_engine_ops(td, ops))
		return NULL; /* 검증 실패: 필수 콜백 누락 또는 버전 불일치 */

	return ops; /* 성공: 로드되고 검증된 엔진 ops를 반환 */
}

/*
 * For cleaning up an ioengine which never made it to init().
 */
/*
 * [한국어]
 * free_ioengine - 초기화(init)까지 도달하지 못한 I/O 엔진의 자원을 해제
 *
 * @td: 스레드 데이터 구조체
 *
 * init()이 호출되지 않은 엔진을 정리할 때 사용한다.
 * cleanup()은 호출하지 않는다 (init이 안 됐으므로 cleanup할 것이 없음).
 * 엔진 옵션(eo) 메모리 해제와, 동적 라이브러리인 경우 dlclose()를 수행한다.
 * close_ioengine()과의 차이: close_ioengine()은 cleanup() 호출 후 이 함수를 호출.
 */
void free_ioengine(struct thread_data *td)
{
	/* td와 td->io_ops가 NULL이 아닌지 단언 검사 */
	assert(td != NULL && td->io_ops != NULL);

	dprint(FD_IO, "free ioengine %s\n", td->io_ops->name);

	/* 엔진 옵션(eo)이 있고, 옵션 정의(options)가 있으면 옵션 메모리 해제 */
	if (td->eo && td->io_ops->options) {
		options_free(td->io_ops->options, td->eo); /* 옵션 구조체 내부 동적 할당 해제 */
		free(td->eo);   /* 옵션 구조체 자체 해제 */
		td->eo = NULL;   /* 댕글링 포인터 방지 */
	}

	/* 동적 라이브러리로 로드된 엔진이면 dlclose()로 라이브러리 해제 */
	if (td->io_ops->dlhandle) {
		dprint(FD_IO, "dlclose ioengine %s\n", td->io_ops->name);
		dlclose(td->io_ops->dlhandle);
	}

	/* 엔진 ops 포인터를 NULL로 설정하여 사용 불가 상태로 만듦 */
	td->io_ops = NULL;
}

/*
 * [한국어]
 * close_ioengine - I/O 엔진을 완전히 종료하고 자원을 해제
 *
 * @td: 스레드 데이터 구조체
 *
 * I/O 흐름의 마지막 단계에서 호출된다.
 * 1. 엔진의 cleanup() 콜백을 호출하여 엔진 내부 자원 해제
 *    (예: io_uring 링 해제, libaio의 io_context 해제 등)
 * 2. free_ioengine()을 호출하여 엔진 옵션과 동적 라이브러리 핸들 해제
 */
void close_ioengine(struct thread_data *td)
{
	dprint(FD_IO, "close ioengine %s\n", td->io_ops->name);

	/* cleanup 콜백이 정의되어 있으면 호출 (엔진별 자원 정리) */
	if (td->io_ops->cleanup) {
		td->io_ops->cleanup(td);    /* 엔진 내부 자원 해제 (예: 링 버퍼, 컨텍스트) */
		td->io_ops_data = NULL;     /* 엔진 전용 데이터 포인터 초기화 */
	}

	/* 나머지 자원(옵션 메모리, dlhandle 등) 해제 */
	free_ioengine(td);
}

/*
 * [한국어]
 * td_io_prep - I/O 요청을 제출하기 전에 준비 작업을 수행
 *
 * @td: 스레드 데이터 구조체
 * @io_u: 준비할 I/O 유닛 (개별 I/O 요청)
 * @return: 성공 시 0, 실패 시 음수 오류 코드
 *
 * I/O 흐름에서의 위치:
 *   load_ioengine() -> td_io_init() -> td_io_open_file()
 *   -> [td_io_prep() <-- 현재 함수] -> td_io_queue() -> td_io_commit()
 *   -> td_io_getevents() -> ...
 *
 * io_u를 엔진에 제출하기 전에 필요한 사전 준비를 한다.
 * 예를 들어, libaio 엔진은 여기서 iocb(I/O 제어 블록)를 설정한다.
 * prep() 콜백이 없는 엔진도 있으며, 이 경우 아무 작업 없이 성공(0)을 반환한다.
 */
int td_io_prep(struct thread_data *td, struct io_u *io_u)
{
	/* 디버그 출력: 현재 io_u의 prep 단계 시작 */
	dprint_io_u(io_u, "prep");
	/* 읽기 전용 모드에서 쓰기 요청이 아닌지 확인 */
	fio_ro_check(td, io_u);

	/* 해당 파일에 대한 파일 잠금 획득 (동시 접근 제어) */
	lock_file(td, io_u->file, io_u->ddir);

	/* prep 콜백이 정의되어 있으면 호출 */
	if (td->io_ops->prep) {
		int ret = td->io_ops->prep(td, io_u);

		dprint(FD_IO, "prep: io_u %p: ret=%d\n", io_u, ret);

		/* prep 실패 시 파일 잠금을 해제하고 오류 반환 */
		if (ret)
			unlock_file(td, io_u->file);
		return ret;
	}

	return 0; /* prep 콜백이 없으면 항상 성공 */
}

/*
 * [한국어]
 * td_io_getevents - 완료된 I/O 이벤트들을 수집
 *
 * @td: 스레드 데이터 구조체
 * @min: 최소한 이 개수만큼의 이벤트가 완료될 때까지 대기
 * @max: 최대로 가져올 이벤트 수
 * @t: 타임아웃 시간 (NULL이면 무한 대기)
 * @return: 수집된 이벤트 수, 오류 시 음수
 *
 * I/O 흐름에서의 위치:
 *   td_io_prep() -> td_io_queue() -> td_io_commit()
 *   -> [td_io_getevents() <-- 현재 함수] -> (io_u 재사용 또는 완료 처리)
 *
 * 이 함수는 비동기 I/O 엔진에서 핵심적인 역할을 한다.
 * 비동기 엔진(libaio, io_uring 등)에서 큐에 제출된 I/O 요청들이 완료되면,
 * 이 함수를 통해 완료된 이벤트를 수집한다.
 *
 * 동작 과정:
 *   1. td->done 플래그 확인 (RDMA 서버 측 특수 처리)
 *   2. min > 0이면 commit() 호출하여 미제출 요청을 먼저 플러시
 *   3. max를 현재 in-flight I/O 깊이에 맞게 조정
 *   4. 엔진의 getevents() 콜백 호출로 OS에서 완료 이벤트 수집
 *   5. io_u_in_flight 카운터 갱신 및 완료 통계 기록
 *
 * 동기(sync) 엔진에서는 queue()에서 I/O가 즉시 완료되므로
 * getevents()가 호출되지 않거나 의미가 없다.
 */
int td_io_getevents(struct thread_data *td, unsigned int min, unsigned int max,
		    const struct timespec *t)
{
	int r = 0; /* 수집된 이벤트 수 또는 오류 코드 */

	/*
	 * For ioengine=rdma one side operation RDMA_WRITE or RDMA_READ,
	 * server side gets a message from the client
	 * side that the task is finished, and
	 * td->done is set to 1 after td_io_commit(). In this case,
	 * there is no need to reap complete event in server side.
	 */
	/* [한국어] RDMA 엔진의 특수 케이스: 서버 측에서 클라이언트의 완료 메시지를 받으면
	 * td->done이 설정되며, 이 경우 이벤트 수집이 불필요하다. */
	if (td->done)
		return 0;

	/* min > 0이면 아직 제출되지 않은 요청을 먼저 commit하여 플러시 */
	if (min > 0 && td->io_ops->commit) {
		r = td->io_ops->commit(td);
		if (r < 0)
			goto out; /* commit 실패 시 오류 처리로 점프 */
	}
	/* max를 현재 미완료 I/O 깊이(cur_depth)로 제한 */
	if (max > td->cur_depth)
		max = td->cur_depth;
	/* min이 max보다 크면 max를 min에 맞춤 (최소 보장) */
	if (min > max)
		max = min;

	r = 0;
	/* max가 0보다 크고 getevents 콜백이 있으면 호출하여 이벤트 수집 */
	if (max && td->io_ops->getevents)
		r = td->io_ops->getevents(td, min, max, t);
out:
	if (r >= 0) {
		/*
		 * Reflect that our submitted requests were retrieved with
		 * whatever OS async calls are in the underlying engine.
		 */
		/* [한국어] 수집된 이벤트 수만큼 in-flight 카운터를 감소시킨다.
		 * io_u_in_flight는 제출되었지만 아직 완료되지 않은 I/O 요청 수를 추적한다. */
		td->io_u_in_flight -= r;
		/* 완료 통계를 기록 (깊이별 완료 카운트 등) */
		io_u_mark_complete(td, r);
	} else
		/* 오류 발생 시 스레드 에러 상태 설정 */
		td_verror(td, r, "get_events");

	dprint(FD_IO, "getevents: %d\n", r);
	return r;
}

/*
 * [한국어]
 * td_io_queue - I/O 요청(io_u)을 I/O 엔진의 큐에 제출
 *
 * @td: 스레드 데이터 구조체
 * @io_u: 제출할 I/O 유닛 (읽기/쓰기/TRIM/SYNC 등의 개별 요청)
 * @return: FIO_Q_COMPLETED(즉시 완료), FIO_Q_QUEUED(큐에 추가됨), FIO_Q_BUSY(엔진 바쁨)
 *
 * I/O 흐름에서의 위치 (핵심 데이터 경로):
 *   td_io_prep() -> [td_io_queue() <-- 현재 함수] -> td_io_commit()
 *   -> td_io_getevents()
 *
 * 이 함수는 fio I/O 데이터 경로의 핵심이다. 모든 I/O 요청은 이 함수를 통해
 * 실제 I/O 엔진에 전달된다.
 *
 * 반환값의 의미:
 *   - FIO_Q_COMPLETED: I/O가 즉시 완료됨 (동기 엔진의 일반적인 결과)
 *   - FIO_Q_QUEUED: I/O가 엔진 내부 큐에 추가됨 (비동기 엔진),
 *                   나중에 td_io_commit()으로 제출하고 td_io_getevents()로 완료 확인
 *   - FIO_Q_BUSY: 엔진이 더 이상 요청을 받을 수 없음,
 *                 io_u가 "되돌려지고" 나중에 재시도됨
 *
 * 전체 동작 과정:
 *   1. io_u에 IO_U_F_FLIGHT 플래그 설정 (제출 중 표시)
 *   2. overlap 체크 뮤텍스 해제 (offload 모드)
 *   3. iolog에 I/O 기록
 *   4. 동기 엔진이면 issue_time 기록
 *   5. I/O 통계 카운터 업데이트 (issue 수, 바이트 수)
 *   6. 엔진의 queue() 콜백 호출 (실제 I/O 제출)
 *   7. ZBD(존 블록 디바이스) 후처리
 *   8. 반환값에 따른 통계 처리 및 자동 commit
 */
enum fio_q_status td_io_queue(struct thread_data *td, struct io_u *io_u)
{
	/* I/O 방향을 통계 카운팅용으로 변환 (예: DDIR_READ, DDIR_WRITE) */
	const enum fio_ddir ddir = acct_ddir(io_u);
	/* 전송할 데이터 버퍼 크기 */
	unsigned long long buflen = io_u->xfer_buflen;
	enum fio_q_status ret; /* 엔진의 queue() 반환값 */

	/* 디버그 출력: queue 단계 시작 */
	dprint_io_u(io_u, "queue");
	/* 읽기 전용 모드에서 쓰기 요청 방지 체크 */
	fio_ro_check(td, io_u);

	/* io_u가 아직 FLIGHT 상태가 아닌지 단언 (이중 제출 방지) */
	assert((io_u->flags & IO_U_F_FLIGHT) == 0);
	/* IO_U_F_FLIGHT 플래그 설정: 이 io_u가 현재 엔진에 제출 중임을 표시 */
	io_u_set(td, io_u, IO_U_F_FLIGHT);

	/*
	 * If overlap checking was enabled in offload mode we
	 * can release this lock that was acquired when we
	 * started the overlap check because the IO_U_F_FLIGHT
	 * flag is now set
	 */
	/* [한국어] offload 모드에서 overlap 체크가 활성화된 경우,
	 * IO_U_F_FLIGHT가 설정되었으므로 overlap 체크 뮤텍스를 해제할 수 있다.
	 * 이 뮤텍스는 동일 오프셋에 대한 중복 I/O를 방지하기 위해 사용된다. */
	if (td_offload_overlap(td)) {
		int res;

		res = pthread_mutex_unlock(&overlap_check);
		if (fio_unlikely(res != 0)) {
			log_err("failed to unlock overlap check mutex, err: %i:%s", errno, strerror(errno));
			abort(); /* 뮤텍스 해제 실패는 심각한 오류이므로 프로세스 중단 */
		}
	}

	/* 파일이 열려 있는지 단언 검사 */
	assert(fio_file_open(io_u->file));

	/*
	 * If using a write iolog, store this entry.
	 */
	/* [한국어] 쓰기용 iolog가 활성화되어 있으면 이 I/O 요청을 로그에 기록.
	 * 나중에 이 로그를 재생(replay)하여 동일한 I/O 패턴을 재현할 수 있다. */
	log_io_u(td, io_u);

	/* I/O 에러와 잔여 데이터 카운터 초기화 */
	io_u->error = 0;  /* 에러 코드 초기화 */
	io_u->resid = 0;  /* 미전송 잔여 바이트 초기화 (short I/O 감지용) */

	/* 동기 엔진이거나, 비동기 엔진이지만 TRIM/SYNCFS를 동기 처리해야 하는 경우 */
	if (td_ioengine_flagged(td, FIO_SYNCIO) ||
	    async_ioengine_sync_trim(td, io_u) ||
	    async_ioengine_sync_syncfs(td, io_u)) {
		/* issue_time 기록이 필요한지 확인 (레이턴시 측정, iolog 재생 등) */
		if (fio_fill_issue_time(td)) {
			/* 현재 시간을 io_u의 issue_time에 기록 */
			fio_gettime(&io_u->issue_time, NULL);

			/*
			 * only used for iolog
			 */
			/* [한국어] iolog 재생 모드에서는 last_issue 타임스탬프도 갱신하여
			 * 다음 I/O의 제출 시점을 계산하는 데 사용한다. */
			if (td->o.read_iolog_file)
				memcpy(&td->last_issue, &io_u->issue_time,
						sizeof(io_u->issue_time));
		}
	}


	/* 읽기 또는 쓰기 방향의 I/O인 경우 통계 카운터 갱신 */
	if (ddir_rw(ddir)) {
		/* verification 리스트에 의한 I/O가 아닌 경우에만 issue 카운터 증가 */
		if (!(io_u->flags & IO_U_F_VER_LIST)) {
			td->io_issues[ddir]++;          /* 해당 방향의 I/O 발행 횟수 증가 */
			td->io_issue_bytes[ddir] += buflen; /* 해당 방향의 I/O 발행 바이트 증가 */
		}
		/* rate 제한용 발행 바이트 카운터 (항상 증가) */
		td->rate_io_issue_bytes[ddir] += buflen;
	}

	/* ===== 핵심: 엔진의 queue() 콜백 호출 =====
	 * 여기서 실제로 I/O 요청이 엔진에 전달된다.
	 * 동기 엔진: pread/pwrite 등을 직접 호출하여 즉시 완료
	 * 비동기 엔진: 내부 제출 큐에 추가 (나중에 commit으로 OS에 제출) */
	ret = td->io_ops->queue(td, io_u);
	/* ZBD(존 블록 디바이스) 후처리: 쓰기 포인터 갱신 등 */
	zbd_queue_io_u(td, io_u, &ret);

	/* I/O 제출 후 파일 잠금 해제 */
	unlock_file(td, io_u->file);

	/* FIO_Q_BUSY: 엔진이 더 이상 요청을 수용할 수 없음 */
	if (ret == FIO_Q_BUSY) {
		/* 통계 카운터를 되돌림 (요청이 실제로 제출되지 않았으므로) */
		if (ddir_rw(ddir)) {
			td->io_issues[ddir]--;
			td->io_issue_bytes[ddir] -= buflen;
			td->rate_io_issue_bytes[ddir] -= buflen;
		}
		/* FLIGHT 플래그 해제: 이 io_u는 다시 사용 가능 상태로 */
		io_u_clear(td, io_u, IO_U_F_FLIGHT);
	}

	/*
	 * If an error was seen and the io engine didn't propagate it
	 * back to 'td', do so.
	 */
	/* [한국어] io_u에 에러가 설정되었지만 스레드 레벨 에러(td->error)가
	 * 아직 설정되지 않았으면, io_u의 에러를 스레드로 전파한다. */
	if (io_u->error && !td->error)
		td_verror(td, io_u->error, "td_io_queue");

	/*
	 * Add warning for O_DIRECT so that users have an easier time
	 * spotting potentially bad alignment. If this triggers for the first
	 * IO, then it's likely an alignment problem or because the host fs
	 * does not support O_DIRECT
	 */
	/* [한국어] O_DIRECT 사용 시 첫 번째 I/O에서 EINVAL 에러가 발생하면,
	 * 정렬(alignment) 문제이거나 파일시스템이 O_DIRECT를 지원하지 않을 가능성이 높다.
	 * 사용자가 문제를 빠르게 파악할 수 있도록 경고 메시지를 출력한다. */
	if (io_u->error == EINVAL && td->io_issues[io_u->ddir & 1] == 1 &&
	    td->o.odirect) {

		log_info("fio: first direct IO errored. File system may not "
			 "support direct IO, or iomem_align= is bad, or "
			 "invalid block size. Try setting direct=0.\n");
	}

	/* ZBD 비정렬 쓰기 에러 처리: 존 블록 디바이스에서 정렬되지 않은 쓰기 시도 시
	 * 사용자에게 --zonemode=zbd 옵션 사용을 안내한다. */
	if (zbd_unaligned_write(io_u->error) &&
	    td->io_issues[io_u->ddir & 1] == 1 &&
	    td->o.zone_mode != ZONE_MODE_ZBD) {
		log_info("fio: first I/O failed. If %s is a zoned block device, consider --zonemode=zbd\n",
			 io_u->file->file_name);
	}

	/* commit 콜백이 없는 엔진 (주로 동기 엔진)은 queue()에서 즉시 제출+완료되므로
	 * 여기서 제출/완료 통계를 바로 기록한다. */
	if (!td->io_ops->commit) {
		io_u_mark_submit(td, 1);   /* 제출 통계 기록 */
		io_u_mark_complete(td, 1); /* 완료 통계 기록 */
	}

	/* ===== 반환값에 따른 후처리 ===== */

	if (ret == FIO_Q_COMPLETED) {
		/* FIO_Q_COMPLETED: I/O가 즉시 완료됨 (동기 엔진의 일반적인 경우) */
		/* 읽기/쓰기이거나, fsync인데 TD_FSYNCING 상태가 아닌 경우에 통계 기록 */
		if (ddir_rw(io_u->ddir) ||
		    (ddir_sync(io_u->ddir) && td->runstate != TD_FSYNCING)) {
			io_u_mark_depth(td, 1);             /* I/O 깊이 통계 기록 */
			td->ts.total_io_u[io_u->ddir]++;    /* 방향별 총 I/O 수 증가 */
		}

		/* 마지막으로 발행된 I/O 방향 기록 */
		td->last_ddir_issued = ddir;
	} else if (ret == FIO_Q_QUEUED) {
		/* FIO_Q_QUEUED: I/O가 엔진 내부 큐에 추가됨 (비동기 엔진) */
		td->io_u_queued++; /* 큐에 쌓인 미제출 I/O 수 증가 */

		/* 읽기/쓰기이거나 fsync(비 FSYNCING 상태)면 총 I/O 수 증가 */
		if (ddir_rw(io_u->ddir) ||
		    (ddir_sync(io_u->ddir) && td->runstate != TD_FSYNCING))
			td->ts.total_io_u[io_u->ddir]++;

		/* 큐에 쌓인 수가 iodepth_batch에 도달하면 자동으로 commit 호출
		 * iodepth_batch는 한번에 OS에 제출할 배치 크기를 결정한다. */
		if (td->io_u_queued >= td->o.iodepth_batch)
			td_io_commit(td);

		/* 마지막으로 발행된 I/O 방향 기록 */
		td->last_ddir_issued = ddir;
	}

	/* 비동기 엔진이면서 TRIM 동기 처리가 아닌 경우, 여기서 issue_time을 기록
	 * (동기 엔진은 위에서 이미 기록했으므로 여기서는 비동기 엔진만 처리) */
	if (!td_ioengine_flagged(td, FIO_SYNCIO) &&
		!async_ioengine_sync_trim(td, io_u)) {
		/* issue_time 기록이 필요하고, 엔진이 자체적으로 issue_time을 설정하지 않는 경우 */
		if (fio_fill_issue_time(td) &&
			!td_ioengine_flagged(td, FIO_ASYNCIO_SETS_ISSUE_TIME)) {
			fio_gettime(&io_u->issue_time, NULL);

			/*
			 * only used for iolog
			 */
			/* [한국어] iolog 재생 모드에서 마지막 발행 시간 갱신 */
			if (td->o.read_iolog_file)
				memcpy(&td->last_issue, &io_u->issue_time,
						sizeof(io_u->issue_time));
		}
	}

	return ret; /* 큐잉 결과 반환: COMPLETED, QUEUED, 또는 BUSY */
}

/*
 * [한국어]
 * td_io_init - I/O 엔진을 초기화
 *
 * @td: 스레드 데이터 구조체
 * @return: 성공 시 0, 실패 시 음수 오류 코드
 *
 * I/O 흐름에서의 위치:
 *   load_ioengine() -> [td_io_init() <-- 현재 함수] -> td_io_open_file()
 *   -> td_io_prep() -> td_io_queue() -> ...
 *
 * 엔진의 init() 콜백을 호출하여 엔진별 초기화를 수행한다.
 * 예시:
 *   - libaio: io_setup()으로 aio 컨텍스트 생성
 *   - io_uring: io_uring_setup()으로 SQ/CQ 링 생성
 *   - sync: 특별한 초기화 없음 (init 콜백이 NULL)
 *
 * 초기화 실패 시, iodepth가 1보다 큰 경우 깊이를 줄여보라는 안내를 출력한다.
 * (일부 엔진은 커널 자원 제한으로 큰 iodepth를 지원하지 못할 수 있음)
 */
int td_io_init(struct thread_data *td)
{
	int ret = 0; /* 초기화 결과 코드 */

	/* init 콜백이 정의되어 있으면 호출 */
	if (td->io_ops->init) {
		ret = td->io_ops->init(td);
		if (ret)
			/* 초기화 실패 메시지 출력.
			 * iodepth > 1이면 깊이를 줄여보라는 추가 안내를 표시 */
			log_err("fio: io engine %s init failed.%s\n",
				td->io_ops->name,
				td->o.iodepth > 1 ?
				" Perhaps try reducing io depth?" : "");
		else
			/* 초기화 성공: io_ops_init 플래그를 1로 설정하여
			 * 나중에 cleanup이 필요함을 표시 */
			td->io_ops_init = 1;
		/* td->error가 아직 설정되지 않았으면 ret 값으로 설정 */
		if (!td->error)
			td->error = ret;
	}

	return ret;
}

/*
 * [한국어]
 * td_io_commit - 큐에 쌓인 I/O 요청들을 OS에 일괄 제출 (flush)
 *
 * @td: 스레드 데이터 구조체
 *
 * I/O 흐름에서의 위치:
 *   td_io_prep() -> td_io_queue() -> [td_io_commit() <-- 현재 함수]
 *   -> td_io_getevents()
 *
 * 비동기 엔진에서 queue()로 쌓인 요청들을 한번에 OS에 제출한다.
 * 이를 "배치 제출"이라고 하며, 시스템 콜 오버헤드를 줄인다.
 *
 * 예시:
 *   - libaio: io_submit()으로 여러 iocb를 한번에 제출
 *   - io_uring: io_uring_enter()로 SQ 링의 항목들을 제출
 *   - sync 엔진: commit 콜백이 없음 (queue()에서 이미 완료됨)
 *
 * td_io_queue()에서 io_u_queued가 iodepth_batch에 도달하면 자동 호출되고,
 * td_io_getevents()에서 min > 0일 때도 호출되어 미제출 요청을 플러시한다.
 */
void td_io_commit(struct thread_data *td)
{
	int ret; /* commit 결과 */

	dprint(FD_IO, "calling ->commit(), depth %d\n", td->cur_depth);

	/* 현재 활성 I/O가 없거나 큐에 쌓인 요청이 없으면 아무것도 하지 않음 */
	if (!td->cur_depth || !td->io_u_queued)
		return;

	/* 이번 배치에서 제출하는 I/O 수를 깊이 통계에 기록 */
	io_u_mark_depth(td, td->io_u_queued);

	/* commit 콜백이 있으면 호출하여 큐에 쌓인 요청을 OS에 제출 */
	if (td->io_ops->commit) {
		ret = td->io_ops->commit(td);
		if (ret)
			td_verror(td, -ret, "io commit"); /* 오류 시 스레드 에러 설정 */
	}

	/*
	 * Reflect that events were submitted as async IO requests.
	 */
	/* [한국어] 큐에 쌓여있던 요청들이 OS에 제출되었으므로,
	 * in_flight 카운터에 추가하고 queued 카운터를 0으로 리셋한다.
	 * io_u_in_flight: 제출되었지만 아직 완료되지 않은 I/O 수
	 * io_u_queued: 엔진 내부 큐에 있지만 아직 OS에 제출되지 않은 I/O 수 */
	td->io_u_in_flight += td->io_u_queued;
	td->io_u_queued = 0;
}

/*
 * [한국어]
 * td_io_open_file - I/O 엔진을 통해 파일을 열기
 *
 * @td: 스레드 데이터 구조체
 * @f: 열 파일의 fio_file 구조체
 * @return: 성공 시 0, 실패 시 1
 *
 * I/O 흐름에서의 위치:
 *   load_ioengine() -> td_io_init() -> [td_io_open_file() <-- 현재 함수]
 *   -> td_io_prep() -> td_io_queue() -> ...
 *
 * 파일 열기뿐만 아니라 다양한 파일 관련 설정도 수행한다:
 *   1. 닫는 중(closing)인 파일의 재열기 처리
 *   2. 엔진의 open_file() 콜백 호출로 실제 파일 열기
 *   3. 파일 메타데이터 리셋 및 상태 플래그 설정
 *   4. 파이프 파일에 대한 랜덤 I/O 제한 검사
 *   5. 캐시 무효화 (invalidate_cache 옵션)
 *   6. fadvise 힌트 설정 (순차/랜덤/재사용 안함)
 *   7. 쓰기 힌트(write_hint) 설정 (커널 수명 힌트)
 *   8. O_DIRECT 설정 (OS_O_DIRECT가 없는 플랫폼)
 */
int td_io_open_file(struct thread_data *td, struct fio_file *f)
{
	/* 파일이 닫는 중(closing) 상태인지 확인 */
	if (fio_file_closing(f)) {
		/*
		 * Open translates to undo closing.
		 */
		/* [한국어] 닫는 중인 파일을 다시 열려는 경우, closing 상태를 해제하고
		 * 참조 카운트를 증가시키는 것으로 처리한다.
		 * 실제로 파일을 다시 open하지 않고, 닫기를 취소하는 것이다. */
		fio_file_clear_closing(f);
		get_file(f); /* 참조 카운트 증가 */
		return 0;
	}
	/* 파일이 이미 열려있지 않아야 함 (이중 열기 방지) */
	assert(!fio_file_open(f));
	/* fd가 -1이어야 함 (아직 열리지 않은 상태) */
	assert(f->fd == -1);
	/* open_file 콜백이 반드시 있어야 함 */
	assert(td->io_ops->open_file);

	/* 엔진의 open_file 콜백 호출 (실제 파일 열기) */
	if (td->io_ops->open_file(td, f)) {
		/* O_DIRECT 사용 중 EINVAL 에러: 대상이 O_DIRECT를 지원하지 않음 */
		if (td->error == EINVAL && td->o.odirect)
			log_err("fio: destination does not support O_DIRECT\n");
		/* EMFILE 에러: 파일 디스크립터 수 제한 초과 */
		if (td->error == EMFILE) {
			log_err("fio: try reducing/setting openfiles (failed"
				" at %u of %u)\n", td->nr_open_files,
							td->o.nr_files);
		}

		/* 열기 실패 시 fd는 -1이고 파일은 열린 상태가 아니어야 함 */
		assert(f->fd == -1);
		assert(!fio_file_open(f));
		return 1; /* 실패 */
	}

	/* 파일 메타데이터 리셋 (오프셋, 통계 등) */
	fio_file_reset(td, f);
	/* 파일 상태를 "열림"으로 설정 */
	fio_file_set_open(f);
	/* closing 상태 해제 */
	fio_file_clear_closing(f);
	/* 디스크 사용률 추적 카운터 증가 */
	disk_util_inc(f->du);

	/* 열린 파일 수 증가 */
	td->nr_open_files++;
	/* 파일 참조 카운트 증가 */
	get_file(f);

	/* 파이프 파일은 탐색(seek)이 불가능하므로 랜덤 I/O를 사용할 수 없음 */
	if (f->filetype == FIO_TYPE_PIPE) {
		if (td_random(td)) {
			log_err("fio: can't seek on pipes (no random io)\n");
			goto err; /* 오류 경로로 점프 */
		}
	}

	/* DISKLESSIO 플래그: 디스크 없는 엔진(예: null, net)은 파일 관련 설정 건너뜀 */
	if (td_ioengine_flagged(td, FIO_DISKLESSIO))
		goto done;

	/* invalidate_cache 옵션이 설정되어 있으면 파일의 페이지 캐시를 무효화
	 * 이렇게 하면 캐시 효과를 배제한 순수 디스크 성능을 측정할 수 있다. */
	if (td->o.invalidate_cache && file_invalidate_cache(td, f))
		goto err;

	/* fadvise 힌트 설정: 커널에 I/O 패턴에 대한 힌트를 제공하여
	 * 페이지 캐시 정책(미리 읽기 등)을 최적화한다. */
	if (td->o.fadvise_hint != F_ADV_NONE &&
	    (f->filetype == FIO_TYPE_BLOCK || f->filetype == FIO_TYPE_FILE)) {
		int flags; /* posix_fadvise 플래그 */

		/* fadvise 타입에 따라 적절한 POSIX 플래그 선택 */
		if (td->o.fadvise_hint == F_ADV_TYPE) {
			/* F_ADV_TYPE: I/O 패턴에 따라 자동 결정 */
			if (td_random(td))
				flags = POSIX_FADV_RANDOM;     /* 랜덤 접근: 미리 읽기 비활성화 */
			else
				flags = POSIX_FADV_SEQUENTIAL; /* 순차 접근: 적극적 미리 읽기 */
		} else if (td->o.fadvise_hint == F_ADV_RANDOM)
			flags = POSIX_FADV_RANDOM;     /* 강제 랜덤 */
		else if (td->o.fadvise_hint == F_ADV_SEQUENTIAL)
			flags = POSIX_FADV_SEQUENTIAL; /* 강제 순차 */
#ifdef POSIX_FADV_NOREUSE
		else if (td->o.fadvise_hint == F_ADV_NOREUSE)
			flags = POSIX_FADV_NOREUSE;    /* 한 번 사용 후 캐시에서 제거 */
#endif
		else {
			log_err("fio: unknown fadvise type %d\n",
							td->o.fadvise_hint);
			flags = POSIX_FADV_NORMAL; /* 알 수 없는 타입이면 기본값 사용 */
		}

		/* 파일의 I/O 영역에 대해 fadvise 힌트 적용 */
		if (posix_fadvise(f->fd, f->file_offset, f->io_size, flags) < 0) {
			if (!fio_did_warn(FIO_WARN_FADVISE))
				log_err("fio: fadvise hint failed\n");
		}
	}
#ifdef FIO_HAVE_WRITE_HINT
	/* 쓰기 힌트(write_hint) 설정: 커널에 데이터의 예상 수명을 알려준다.
	 * 이를 통해 커널/SSD 펌웨어가 데이터 배치를 최적화할 수 있다.
	 * (예: 수명이 짧은 데이터와 긴 데이터를 다른 영역에 배치) */
	if (fio_option_is_set(&td->o, write_hint) &&
	    (f->filetype == FIO_TYPE_BLOCK || f->filetype == FIO_TYPE_FILE)) {
		uint64_t hint = td->o.write_hint; /* 사용자가 설정한 쓰기 힌트 값 */
		int res;

		/*
		 * For direct IO, set the hint on the file descriptor if that is
		 * supported. Otherwise set it on the inode. For buffered IO, we
		 * need to set it on the inode.
		 */
		/* [한국어] Direct I/O의 경우 파일 디스크립터에 힌트를 설정하고,
		 * 실패하면 inode에 설정한다. Buffered I/O는 inode에 설정한다. */
		if (td->o.odirect) {
			res = fcntl(f->fd, F_SET_FILE_RW_HINT, &hint);
			if (res < 0)
				res = fcntl(f->fd, F_SET_RW_HINT, &hint);
		} else {
			res = fcntl(f->fd, F_SET_RW_HINT, &hint);
		}
		if (res < 0) {
			td_verror(td, errno, "fcntl write hint");
			goto err; /* 힌트 설정 실패 */
		}
	}
#endif

	/* OS_O_DIRECT가 정의되지 않은 플랫폼에서 O_DIRECT 에뮬레이션 설정 */
	if (td->o.odirect && !OS_O_DIRECT && fio_set_directio(td, f))
		goto err;

done:
	/* 파일 열기 완료를 로그에 기록 */
	log_file(td, f, FIO_LOG_OPEN_FILE);
	return 0; /* 성공 */
err:
	/* 오류 발생 시 정리 작업 */
	disk_util_dec(f->du);              /* 디스크 사용률 카운터 감소 */
	if (td->io_ops->close_file)
		td->io_ops->close_file(td, f); /* 엔진의 close_file 콜백으로 파일 닫기 */
	return 1; /* 실패 */
}

/*
 * [한국어]
 * td_io_close_file - 파일을 닫기 위해 "닫는 중" 상태로 표시
 *
 * @td: 스레드 데이터 구조체
 * @f: 닫을 파일의 fio_file 구조체
 * @return: put_file()의 반환값 (참조 카운트가 0이 되면 실제 닫기 수행)
 *
 * 즉시 파일을 닫지 않고, "closing" 상태로 표시한 후 참조 카운트를 감소시킨다.
 * 해당 파일에 대한 모든 I/O가 완료되어 참조 카운트가 0이 되면
 * put_file() 내부에서 실제로 엔진의 close_file() 콜백이 호출된다.
 * 이 지연 닫기(deferred close) 방식은 비동기 I/O에서 진행 중인
 * 요청이 완료되기 전에 파일이 닫히는 것을 방지한다.
 */
int td_io_close_file(struct thread_data *td, struct fio_file *f)
{
	/* 아직 closing 상태가 아닌 경우에만 닫기 로그 기록
	 * (이미 closing 상태이면 중복 로그 방지) */
	if (!fio_file_closing(f))
		log_file(td, f, FIO_LOG_CLOSE_FILE);

	/*
	 * mark as closing, do real close when last io on it has completed
	 */
	/* [한국어] closing 플래그를 설정하여 "닫는 중" 상태로 표시.
	 * 실제 닫기는 마지막 I/O가 완료되어 참조 카운트가 0이 될 때 수행된다. */
	fio_file_set_closing(f);

	/* 참조 카운트 감소. 0이 되면 실제 파일 닫기(close_file 콜백) 실행 */
	return put_file(td, f);
}

/*
 * [한국어]
 * td_io_unlink_file - 파일을 삭제(unlink)
 *
 * @td: 스레드 데이터 구조체
 * @f: 삭제할 파일의 fio_file 구조체
 * @return: 성공 시 0, 실패 시 errno
 *
 * 엔진에 unlink_file 콜백이 정의되어 있으면 그것을 사용하고,
 * 없으면 표준 unlink() 시스템 호출을 사용한다.
 * 벤치마크 완료 후 테스트 파일을 정리하거나,
 * 새로운 테스트를 위해 파일을 재생성할 때 호출된다.
 */
int td_io_unlink_file(struct thread_data *td, struct fio_file *f)
{
	/* 엔진에 unlink_file 콜백이 있으면 엔진별 삭제 로직 사용 */
	if (td->io_ops->unlink_file)
		return td->io_ops->unlink_file(td, f);
	else {
		int ret;

		/* 표준 unlink 시스템 호출로 파일 삭제 */
		ret = unlink(f->file_name);
		if (ret < 0)
			return errno; /* 실패 시 errno 반환 */

		return 0; /* 성공 */
	}
}

/*
 * [한국어]
 * td_io_get_file_size - 파일의 크기를 조회
 *
 * @td: 스레드 데이터 구조체
 * @f: 크기를 조회할 파일의 fio_file 구조체
 * @return: 성공 시 0, 실패 시 음수 오류 코드
 *
 * 엔진의 get_file_size() 콜백을 호출하여 파일 크기를 가져온다.
 * 콜백이 정의되지 않으면 0을 반환한다 (크기 조회 불필요).
 * 파일 크기는 I/O 범위 계산, 오프셋 결정 등에 사용된다.
 */
int td_io_get_file_size(struct thread_data *td, struct fio_file *f)
{
	/* get_file_size 콜백이 없으면 크기 조회를 건너뜀 (0 반환) */
	if (!td->io_ops->get_file_size)
		return 0;

	/* 엔진의 get_file_size 콜백 호출 */
	return td->io_ops->get_file_size(td, f);
}

/* CONFIG_DYNAMIC_ENGINES가 정의된 경우에만 동적 엔진 탐색 기능 제공 */
#ifdef CONFIG_DYNAMIC_ENGINES
/* Load all dynamic engines in FIO_EXT_ENG_DIR for enghelp command */
/*
 * [한국어]
 * fio_load_dynamic_engines - 외부 엔진 디렉토리의 모든 동적 엔진을 로드
 *
 * @td: 스레드 데이터 구조체
 *
 * FIO_EXT_ENG_DIR 디렉토리를 스캔하여 그 안의 모든 .so 파일을 dlopen한다.
 * fio_show_ioengine_help()에서 사용 가능한 모든 엔진 목록을 표시할 때
 * 호출되어, 외부 엔진들도 목록에 포함되도록 한다.
 */
static void
fio_load_dynamic_engines(struct thread_data *td)
{
	DIR *dirhandle = NULL;            /* 디렉토리 핸들 */
	struct dirent *dirent = NULL;     /* 디렉토리 엔트리 */
	char engine_path[PATH_MAX];       /* 엔진 파일의 전체 경로 */

	/* 외부 엔진 디렉토리 열기 */
	dirhandle = opendir(FIO_EXT_ENG_DIR);
	if (!dirhandle)
		return; /* 디렉토리가 없으면 조용히 반환 */

	/* 디렉토리의 모든 엔트리를 순회 */
	while ((dirent = readdir(dirhandle)) != NULL) {
		/* 현재 디렉토리(.)와 상위 디렉토리(..)는 건너뜀 */
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;

		/* 전체 경로를 구성하고 dlopen으로 로드 */
		sprintf(engine_path, "%s/%s", FIO_EXT_ENG_DIR, dirent->d_name);
		dlopen_ioengine(td, engine_path);
	}

	closedir(dirhandle); /* 디렉토리 핸들 닫기 */
}
#else
/* 동적 엔진이 비활성화된 경우, 아무 동작도 하지 않는 매크로 */
#define fio_load_dynamic_engines(td) do { } while (0)
#endif

/*
 * [한국어]
 * fio_show_ioengine_help - I/O 엔진의 도움말/옵션 정보를 표시
 *
 * @engine: 엔진 이름 (NULL이면 사용 가능한 모든 엔진 목록 표시)
 * @return: 성공 시 0, 실패 시 1
 *
 * fio --enghelp 명령으로 호출된다.
 * - engine이 NULL/빈 문자열: 등록된 모든 엔진의 이름 목록을 출력
 * - engine이 지정됨: 해당 엔진의 옵션(파라미터) 도움말을 출력
 * - "engine,option" 형식: 특정 옵션만 출력
 */
int fio_show_ioengine_help(const char *engine)
{
	struct flist_head *entry;       /* 엔진 리스트 순회용 포인터 */
	struct thread_data td;          /* 임시 스레드 데이터 (엔진 로딩에 필요) */
	struct ioengine_ops *io_ops;    /* 순회 중인 엔진의 ops 포인터 */
	char *sep;                      /* 콤마 구분자 위치 */
	int ret = 1;                    /* 반환값 (기본: 실패) */

	/* 임시 thread_data를 0으로 초기화 */
	memset(&td, 0, sizeof(struct thread_data));

	/* 엔진 이름이 지정되지 않은 경우: 모든 엔진 목록 출력 */
	if (!engine || !*engine) {
		log_info("Available IO engines:\n");
		/* 동적 엔진도 로드하여 목록에 포함 */
		fio_load_dynamic_engines(&td);
		/* engine_list를 순회하며 모든 엔진 이름 출력 */
		flist_for_each(entry, &engine_list) {
			io_ops = flist_entry(entry, struct ioengine_ops, list);
			log_info("\t%s\n", io_ops->name);
		}
		return 0; /* 성공 */
	}
	/* "engine,option" 형식인지 확인: 콤마로 엔진 이름과 옵션 분리 */
	sep = strchr(engine, ',');
	if (sep) {
		*sep = 0;  /* 콤마를 NULL로 치환하여 엔진 이름 문자열 분리 */
		sep++;     /* sep는 이제 옵션 이름을 가리킴 */
	}

	/* 임시 td에 엔진 이름을 설정하고 엔진 로드 */
	td.o.ioengine = (char *)engine;
	td.io_ops = load_ioengine(&td);

	/* 엔진 로드 실패 */
	if (!td.io_ops) {
		log_info("IO engine %s not found\n", engine);
		return 1;
	}

	/* 엔진에 옵션이 정의되어 있으면 옵션 도움말 표시 */
	if (td.io_ops->options)
		ret = show_cmd_help(td.io_ops->options, sep);
	else
		log_info("IO engine %s has no options\n", td.io_ops->name);

	/* 임시로 로드한 엔진 자원 해제 */
	free_ioengine(&td);
	return ret;
}
