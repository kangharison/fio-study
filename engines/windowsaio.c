/*
 * windowsaio engine
 *
 * IO engine using Windows IO Completion Ports.
 */

/*
 * [한국어 설명] Windows IOCP 기반 비동기 I/O 엔진 구현 (windowsaio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Microsoft Windows의 I/O Completion Ports(IOCP) 메커니즘을
 * 이용하여 fio가 Windows 환경에서 비동기 디스크 I/O를 수행할 수 있게 하는
 * 엔진이다. POSIX aio/io_uring에 해당하는 Windows 네이티브 비동기 API로
 * FILE_FLAG_OVERLAPPED로 연 HANDLE에 ReadFile/WriteFile을 OVERLAPPED
 * 구조체와 함께 전달하면, 완료 시 커널이 관련 IOCP에 완료 패킷을 큐잉한다.
 * 엔진은 두 가지 완료 수집 모드를 지원한다 — 기본 모드는 별도
 * IoCompletionRoutine 스레드가 GetQueuedCompletionStatus()로 완료 패킷을
 * 상시 수거하고 io_complete 플래그를 set하며, no_completion_thread=1
 * 모드에서는 잡 스레드가 getevents 호출 내부에서 직접
 * GetQueuedCompletionStatusEx()로 배치 수거를 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   backend.c:do_io() → td_io_queue() [ioengines.c] → fio_windowsaio_queue()
 *     → ReadFile()/WriteFile()/FlushFileBuffers() (Windows kernel NT I/O)
 *   완료 경로 1 (완료 스레드 모드):
 *     NT I/O Manager → IOCP → IoCompletionRoutine() 스레드
 *       → fov->io_complete=TRUE + SetEvent(iocomplete_event)
 *       → 잡 스레드 fio_windowaio_getevents_thread() (WaitForSingleObject)
 *   완료 경로 2 (no_completion_thread):
 *     잡 스레드 fio_windowsaio_getevents_nothread()
 *       → GetQueuedCompletionStatusEx() 직접 호출.
 * 실행 컨텍스트: Windows 잡은 프로세스/스레드 모델 모두 지원하나 IOCP는
 * 프로세스 내 공유 핸들이므로 실질적으로 스레드 모드로 동작한다. 완료 스레드는
 * CreateThread()로 생성되며, 필요 시 fio의 cpumask를 fio_setaffinity로 바인딩한다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존 모듈: Win32 API(CreateFile, ReadFile, WriteFile, FlushFileBuffers,
 *              CreateIoCompletionPort, GetQueuedCompletionStatus[Ex],
 *              CreateEvent/SetEvent/WaitForSingleObject, CreateThread,
 *              GetLastError, GetTickCount), fio 코어(fio.h, optgroup.h),
 *              os/windows 경유의 win_to_posix_error/geterrno_from_win_error.
 * - 이 파일에 의존하는 모듈: fio 코어 런타임이 이름 "windowsaio"로 조회.
 *              Windows 빌드에서 실질적 기본 엔진으로 사용된다.
 * - 데이터 흐름: io_u->xfer_buf/offset → OVERLAPPED.Offset/OffsetHigh →
 *              ReadFile/WriteFile → 커널 I/O → 완료 시 OVERLAPPED.Internal
 *              (NTSTATUS)/InternalHigh(전송 바이트) → io_u->error/resid.
 * - 공유 자료구조: td->io_ops_data=struct windowsaio_data* (IOCP/완료스레드/이벤트),
 *                 io_u->engine_data=struct fio_overlapped* (OVERLAPPED+io_u 링크).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_windowsaio_init(): windowsaio_data 할당, IOCP와 (옵션) 완료 스레드 생성.
 * - fio_windowsaio_open_file(): FILE_FLAG_OVERLAPPED로 CreateFile, IOCP에 연결.
 * - fio_windowsaio_queue(): OVERLAPPED에 오프셋 주입 후 ReadFile/WriteFile 발행.
 * - IoCompletionRoutine(): 별도 스레드 — IOCP에서 완료 패킷을 수거해 io_complete set.
 * - fio_windowsaio_getevents(): 옵션에 따라 thread 모드/nothread 모드 분기.
 * - fio_windowsaio_cleanup(): 완료 스레드 종료, 핸들/메모리 해제.
 * - struct fio_overlapped: OVERLAPPED + io_u + 완료 플래그.
 * - struct windowsaio_data: 잡 단위 IOCP/스레드/이벤트 상태.
 * - struct windowsaio_options: 엔진 옵션(no_completion_thread 플래그).
 */

/* [한국어] stdio/stdlib/unistd: printf/malloc/free/close 등 표준 유틸리티 프로토타입. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
/* [한국어] signal(2) 프로토타입(현 파일에선 직접 사용은 드묾, 호환성 목적). */
#include <signal.h>
/* [한국어] errno 전역 — Win32 에러를 POSIX errno로 변환한 값이 저장될 수 있음. */
#include <errno.h>

/* [한국어] fio 핵심 구조체/매크로(thread_data, io_u, ioengine_ops, FIO_Q_*, dprint 등). */
#include "../fio.h"
/* [한국어] 옵션 카테고리(FIO_OPT_C_ENGINE, FIO_OPT_G_WINDOWSAIO). */
#include "../optgroup.h"

/* [한국어] Win32 CancelIoEx 함수 포인터 타입 — 일부 빌드에서 동적 로드에 사용.
 * 현재 파일에서는 선언만 있고 직접 호출하진 않는다. */
typedef BOOL (WINAPI *CANCELIOEX)(HANDLE hFile, LPOVERLAPPED lpOverlapped);

/* [한국어] os/windows 계열에서 제공하는 헬퍼 — Win32 DWORD 에러코드를 POSIX errno로 변환.
 * 여기서는 전방 선언만 두고 정의는 다른 파일(os/windows/*)에 있다. */
int geterrno_from_win_error (DWORD code, int deferrno);

/* [한국어] io_u 한 개에 해당하는 확장 OVERLAPPED 구조체 — engine_data 슬롯에 저장.
 * Win32의 OVERLAPPED는 반드시 I/O가 pending 중일 때까지 유효한 메모리에 있어야 하므로
 * io_u별로 1:1 할당한다. CONTAINING_RECORD 매크로로 OVERLAPPED → fio_overlapped 복원. */
struct fio_overlapped {
	OVERLAPPED o;
	/* [한국어] Win32 비동기 I/O 기술자. Offset/OffsetHigh에 64비트 오프셋 분할 저장.
	 * 설정자: fio_windowsaio_queue()가 Internal/InternalHigh/Offset을 초기화.
	 * 읽는 자: 커널이 완료 시 Internal(= NTSTATUS)과 InternalHigh(전송 바이트)를 기록,
	 *          완료 수거 경로가 이를 읽어 io_u->error/resid로 환산.
	 * 값 범위: Windows SDK 정의대로. hEvent=NULL 권장(IOCP 사용 시).
	 * 동기화: 커널 writer ↔ 완료 수거 스레드 reader. I/O pending 중 이 메모리를
	 *         재사용하면 안 됨(OVERLAPPED lifetime rule). */

	struct io_u *io_u;
	/* [한국어] 대응 fio io_u 역링크. 설정자: io_u_init. 읽는 자: 완료 루틴. */

	BOOL io_complete;
	/* [한국어] IoCompletionRoutine이 "완료 도착"을 잡 스레드에 알리는 플래그.
	 * 설정자: IoCompletionRoutine() → TRUE; getevents 수확 시 FALSE로 리셋.
	 * 읽는 자: fio_windowaio_getevents_thread()의 io_u_qiter 루프.
	 * 값 범위: TRUE | FALSE.
	 * 동기화: 완료 스레드 writer ↔ 잡 스레드 reader — SetEvent/Wait 경계가 fence 역할. */
};

/* [한국어] 잡 단위 IOCP 엔진 상태 — td->io_ops_data에 매달림. */
struct windowsaio_data {
	struct io_u **aio_events;
	/* [한국어] getevents가 fio에 반환할 완료 io_u 순서 배열(iodepth 크기).
	 * 설정자: getevents 경로. 읽는 자: fio_windowsaio_event(). */

	HANDLE iocp;
	/* [한국어] I/O Completion Port 핸들. 설정자: init에서 CreateIoCompletionPort.
	 * 읽는 자: open_file(파일 바인딩), GetQueuedCompletionStatus[Ex]. */

	HANDLE iothread;
	/* [한국어] 완료 처리 스레드 핸들(no_completion_thread=0일 때만 유효).
	 * 설정자: init에서 CreateThread. 읽는 자: cleanup에서 WaitForSingleObject/CloseHandle. */

	HANDLE iocomplete_event;
	/* [한국어] auto-reset 이벤트 — 완료 스레드가 SetEvent로 signal, 잡 스레드가 Wait.
	 * 설정자: CreateEvent(NULL, FALSE, FALSE, NULL). FALSE=auto-reset.
	 * 값 범위: signaled | non-signaled. */

	BOOL iothread_running;
	/* [한국어] 완료 스레드 실행 루프의 종료 플래그.
	 * 설정자: init=TRUE, cleanup=FALSE. 읽는 자: IoCompletionRoutine 루프 조건.
	 * 동기화: single-writer(잡 스레드) / single-reader(완료 스레드). 플래그는 한 방향으로만 전이. */
};

/* [한국어] IoCompletionRoutine 스레드 시작 시 전달되는 컨텍스트 — iocp와 wd 쌍을 묶어 전달. */
struct thread_ctx {
	HANDLE iocp;
	/* [한국어] 수거할 IOCP 핸들(windowsaio_data::iocp 복사본). */

	struct windowsaio_data *wd;
	/* [한국어] 완료 처리에 필요한 엔진 상태 포인터(iocomplete_event 시그널링용). */
};

/* [한국어] 엔진 옵션 구조체 — no_completion_thread 플래그로 완료 수거 모드 선택. */
struct windowsaio_options {
	struct thread_data *td;
	/* [한국어] 옵션 파서가 필요로 하는 역참조(상위 td). */

	unsigned int no_completion_thread;
	/* [한국어] 1이면 별도 완료 스레드 없이 getevents 내부에서 직접 수거.
	 * 값 범위: 0|1. 설정자: 파서. 읽는 자: init/getevents 분기. */
};

/* [한국어] 옵션 테이블(.name=NULL 센티널). */
static struct fio_option options[] = {
	{
		.name	= "no_completion_thread",
		.lname	= "No completion polling thread",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct windowsaio_options, no_completion_thread),
		.help	= "Use to avoid separate completion polling thread",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_WINDOWSAIO,
	},
	{
		.name	= NULL,
	},
};

/* [한국어] 완료 스레드 엔트리 포인트 전방 선언(init에서 참조). */
static DWORD WINAPI IoCompletionRoutine(LPVOID lpParameter);

/*
 * [한국어]
 * fio_windowsaio_init - 잡 초기화 시 IOCP, 이벤트, 완료 스레드를 생성한다.
 *
 * @td: 잡. @return: 0=성공, 1=어느 단계 실패.
 *
 * 단계: (1) windowsaio_data 할당, (2) aio_events 배열 할당,
 *       (3) auto-reset iocomplete_event 생성, (4) IOCP 생성,
 *       (5) (옵션) IoCompletionRoutine 스레드 생성 + 선택적 CPU affinity 지정.
 *
 * 호출 체인: td_io_init → ioengine_ops.init (= 이 함수) → CreateIoCompletionPort/CreateEvent/CreateThread.
 */
static int fio_windowsaio_init(struct thread_data *td)
{
	struct windowsaio_data *wd;
	int rc = 0;

	wd = calloc(1, sizeof(struct windowsaio_data));
	if (wd == NULL) {
		 log_err("windowsaio: failed to allocate memory for engine data\n");
		rc = 1;
	}

	if (!rc) {
		wd->aio_events = malloc(td->o.iodepth * sizeof(struct io_u*));
		if (wd->aio_events == NULL) {
			log_err("windowsaio: failed to allocate memory for aio events list\n");
			rc = 1;
		}
	}

	if (!rc) {
		/* Create an auto-reset event */
		wd->iocomplete_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		/* [한국어] auto-reset(FALSE): Wait이 해제되는 순간 자동으로 non-signaled로. */
		if (wd->iocomplete_event == NULL) {
			log_err("windowsaio: failed to create io complete event handle\n");
			rc = 1;
		}
	}

	if (rc) {
		if (wd != NULL) {
			if (wd->aio_events != NULL)
				free(wd->aio_events);

			free(wd);
		}
	}

	td->io_ops_data = wd;

	if (!rc) {
		struct thread_ctx *ctx;
		struct windowsaio_data *wd;
		HANDLE hFile;
		struct windowsaio_options *o = td->eo;

		hFile = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
		/* [한국어] 빈 IOCP 생성: INVALID_HANDLE_VALUE는 "파일 미바인딩"을 의미.
		 * 이후 open_file에서 각 파일 핸들을 이 IOCP에 연결한다. */
		if (hFile == INVALID_HANDLE_VALUE) {
			log_err("windowsaio: failed to create io completion port\n");
			rc = 1;
		}

		wd = td->io_ops_data;
		wd->iothread_running = TRUE;
		wd->iocp = hFile;

		if (o->no_completion_thread == 0) {
			/* [한국어] 기본 모드: 별도 완료 스레드를 돌려 완료 패킷을 상시 수거. */
			if (!rc)
				ctx = malloc(sizeof(struct thread_ctx));

			if (!rc && ctx == NULL) {
				log_err("windowsaio: failed to allocate memory for thread context structure\n");
				CloseHandle(hFile);
				rc = 1;
			}

			if (!rc) {
				DWORD threadid;

				ctx->iocp = hFile;
				ctx->wd = wd;
				wd->iothread = CreateThread(NULL, 0, IoCompletionRoutine, ctx, 0, &threadid);
				/* [한국어] 기본 스택, 기본 보안, 즉시 실행, OUT threadid. */
				if (!wd->iothread)
					log_err("windowsaio: failed to create io completion thread\n");
				else if (fio_option_is_set(&td->o, cpumask))
					fio_setaffinity(threadid, td->o.cpumask);
				/* [한국어] 잡의 cpumask 옵션이 지정된 경우 완료 스레드도 동일 마스크에 바인딩. */
			}
			if (rc || wd->iothread == NULL)
				rc = 1;
		}
	}

	return rc;
}

/*
 * [한국어]
 * fio_windowsaio_cleanup - 잡 종료 시 완료 스레드 중단, 모든 핸들/메모리 해제.
 * 순서: iothread_running=FALSE → WaitForSingleObject → CloseHandle 2개 → free 2개.
 */
static void fio_windowsaio_cleanup(struct thread_data *td)
{
	struct windowsaio_data *wd;

	wd = td->io_ops_data;

	if (wd != NULL) {
		wd->iothread_running = FALSE;
		/* [한국어] 완료 스레드 루프에 "종료하라" 신호. 다음 GQCS 타임아웃(250ms) 이내 확인됨. */
		WaitForSingleObject(wd->iothread, INFINITE);
		/* [한국어] 스레드가 실제로 종료될 때까지 블로킹 — race-free 해제 보장. */

		CloseHandle(wd->iothread);
		CloseHandle(wd->iocomplete_event);

		free(wd->aio_events);
		free(wd);

		td->io_ops_data = NULL;
	}
}

/*
 * [한국어]
 * windowsaio_invalidate_cache - 지정 파일을 FILE_FLAG_NO_BUFFERING로 잠시 열어 페이지 캐시 무효화 유도.
 * @return: 0=정상 진행(파일 없음 포함), 1=실패.
 *
 * Windows에는 POSIX posix_fadvise(DONTNEED) 대응이 없어, 동일 파일을 non-buffered로
 * 열었다가 즉시 닫으면 Cache Manager가 캐시 엔트리를 드롭한다(해당 파일을 여는 주체가
 * 시스템에서 유일해야 효과).
 */
static int windowsaio_invalidate_cache(struct fio_file *f)
{
	DWORD error;
	DWORD isharemode = (FILE_SHARE_DELETE | FILE_SHARE_READ |
				FILE_SHARE_WRITE);
	HANDLE ihFile;
	int rc = 0;

	/*
	 * Encourage Windows to drop cached parts of a file by temporarily
	 * opening it for non-buffered access. Note: this will only work when
	 * the following is the only thing with the file open on the whole
	 * system.
	 */
	dprint(FD_IO, "windowaio: attempt invalidate cache for %s\n",
			f->file_name);
	ihFile = CreateFile(f->file_name, 0, isharemode, NULL, OPEN_EXISTING,
			FILE_FLAG_NO_BUFFERING, NULL);
	/* [한국어] access=0(메타데이터 열기), NO_BUFFERING으로 캐시 우회 모드. */

	if (ihFile != INVALID_HANDLE_VALUE) {
		if (!CloseHandle(ihFile)) {
			error = GetLastError();
			log_info("windowsaio: invalidation fd close %s failed: error %lu\n",
				 f->file_name, error);
			rc = 1;
		}
	} else {
		error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND) {
			log_info("windowsaio: cache invalidation of %s failed: error %lu\n",
				 f->file_name, error);
			rc = 1;
		}
	}

	return rc;
}

/*
 * [한국어]
 * fio_windowsaio_open_file - 파일을 OVERLAPPED 비동기 I/O 용도로 열고 IOCP에 바인딩.
 *
 * @td: 잡. @f: 열 대상 fio_file. @return: 0=성공, 1=실패.
 *
 * flag 조합:
 *  - FILE_FLAG_POSIX_SEMANTICS: 대소문자 구분 등 POSIX 유사 동작.
 *  - FILE_FLAG_OVERLAPPED:     비동기 I/O 필수.
 *  - FILE_FLAG_NO_BUFFERING:   td->o.odirect 시(커널 캐시 우회).
 *  - FILE_FLAG_WRITE_THROUGH:  td->o.sync_io 시(쓰기 즉시 flush).
 *  - FILE_FLAG_RANDOM_ACCESS / SEQUENTIAL_SCAN: fadvise_hint에 따라 캐시 관리자 힌트.
 *
 * 호출 체인: td_io_open_file → [이 함수] → CreateFile → CreateIoCompletionPort(파일 연결).
 */
static int fio_windowsaio_open_file(struct thread_data *td, struct fio_file *f)
{
	int rc = 0;
	DWORD flags = FILE_FLAG_POSIX_SEMANTICS | FILE_FLAG_OVERLAPPED;
	DWORD sharemode = FILE_SHARE_READ | FILE_SHARE_WRITE;
	DWORD openmode = OPEN_ALWAYS;
	DWORD access;

	dprint(FD_FILE, "fd open %s\n", f->file_name);

	if (f->filetype == FIO_TYPE_PIPE) {
		log_err("windowsaio: pipes are not supported\n");
		return 1;
	}

	if (!strcmp(f->file_name, "-")) {
		log_err("windowsaio: can't read/write to stdin/out\n");
		return 1;
	}

	if (td->o.odirect)
		flags |= FILE_FLAG_NO_BUFFERING;
	if (td->o.sync_io)
		flags |= FILE_FLAG_WRITE_THROUGH;

	/*
	 * Inform Windows whether we're going to be doing sequential or
	 * random IO so it can tune the Cache Manager
	 */
	switch (td->o.fadvise_hint) {
	case F_ADV_TYPE:
		if (td_random(td))
			flags |= FILE_FLAG_RANDOM_ACCESS;
		else
			flags |= FILE_FLAG_SEQUENTIAL_SCAN;
		break;
	case F_ADV_RANDOM:
		flags |= FILE_FLAG_RANDOM_ACCESS;
		break;
	case F_ADV_SEQUENTIAL:
		flags |= FILE_FLAG_SEQUENTIAL_SCAN;
		break;
	case F_ADV_NONE:
		break;
	default:
		log_err("fio: unknown fadvise type %d\n", td->o.fadvise_hint);
	}

	if ((!td_write(td) && !(td->flags & TD_F_SYNCS)) || read_only)
		access = GENERIC_READ;
	else
		access = (GENERIC_READ | GENERIC_WRITE);
	/* [한국어] 읽기 전용 잡이면 WRITE 권한 요청 금지(리드온리 FS 대응). */

	if (td->o.create_on_open)
		openmode = OPEN_ALWAYS;		/* [한국어] 없으면 생성, 있으면 열기. */
	else
		openmode = OPEN_EXISTING;	/* [한국어] 반드시 존재해야 함. */

	/* If we're going to use direct I/O, Windows will try and invalidate
	 * its cache at that point so there's no need to do it here */
	if (td->o.invalidate_cache && !td->o.odirect)
		windowsaio_invalidate_cache(f);

	f->hFile = CreateFile(f->file_name, access, sharemode,
		NULL, openmode, flags, NULL);

	if (f->hFile == INVALID_HANDLE_VALUE) {
		log_err("windowsaio: failed to open file \"%s\"\n", f->file_name);
		rc = 1;
	}

	/* Only set up the completion port and thread if we're not just
	 * querying the device size */
	if (!rc && td->io_ops_data != NULL) {
		struct windowsaio_data *wd;

		wd = td->io_ops_data;

		if (CreateIoCompletionPort(f->hFile, wd->iocp, 0, 0) == NULL) {
			/* [한국어] 두 번째 인자로 기존 IOCP를 줘서 파일을 그 포트에 바인딩.
			 * 이후 이 파일의 모든 OVERLAPPED 완료는 wd->iocp로 큐잉된다. */
			log_err("windowsaio: failed to create io completion port\n");
			rc = 1;
		}
	}

	return rc;
}

/*
 * [한국어]
 * fio_windowsaio_close_file - CreateFile로 연 핸들을 닫는다. IOCP 바인딩은 핸들 close 시 자동 해제.
 */
static int fio_windowsaio_close_file(struct thread_data fio_unused *td, struct fio_file *f)
{
	int rc = 0;

	dprint(FD_FILE, "fd close %s\n", f->file_name);

	if (f->hFile != INVALID_HANDLE_VALUE) {
		if (!CloseHandle(f->hFile)) {
			log_info("windowsaio: failed to close file handle for \"%s\"\n", f->file_name);
			rc = 1;
		}
	}

	f->hFile = INVALID_HANDLE_VALUE;
	return rc;
}

/*
 * [한국어]
 * timeout_expired - GetTickCount 기반 wraparound-safe 타임아웃 판정.
 * 32-bit tick counter가 약 49.7일 주기로 wrap 하므로 두 경우(정상/wrap)를 모두 처리한다.
 */
static BOOL timeout_expired(DWORD start_count, DWORD end_count)
{
	BOOL expired = FALSE;
	DWORD current_time;

	current_time = GetTickCount();

	if ((end_count > start_count) && current_time >= end_count)
		expired = TRUE;
	else if (current_time < start_count && current_time > end_count)
		expired = TRUE;
	/* [한국어] wrap 발생 시 start>end 관계가 뒤집어지므로 보정 분기. */

	return expired;
}

/*
 * [한국어]
 * fio_windowsaio_event - event 인덱스 → aio_events 슬롯의 io_u.
 */
static struct io_u* fio_windowsaio_event(struct thread_data *td, int event)
{
	struct windowsaio_data *wd = td->io_ops_data;
	return wd->aio_events[event];
}

/* dequeue completion entrees directly (no separate completion thread) */
/*
 * [한국어]
 * fio_windowsaio_getevents_nothread - no_completion_thread 모드 getevents.
 *
 * GetQueuedCompletionStatusEx로 한 번에 최대 16개 완료 패킷을 회수해 io_u에 결과를 분배.
 * 타임아웃(t)이 주어지면 timeout_expired 기반 종료 조건으로 사용.
 *
 * 호출 체인: fio_windowsaio_getevents → [이 함수] → GetQueuedCompletionStatusEx
 */
static int fio_windowsaio_getevents_nothread(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct windowsaio_data *wd = td->io_ops_data;
	unsigned int dequeued = 0;
	struct io_u *io_u;
	DWORD start_count = 0;
	DWORD end_count = 0;
	DWORD mswait = 250;		/* [한국어] 기본 250ms 폴링 주기. */
	struct fio_overlapped *fov;

	if (t != NULL) {
		mswait = (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
		start_count = GetTickCount();
		end_count = start_count + (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
	}

	do {
		BOOL ret;
		OVERLAPPED *ovl;

		ULONG entries = min(16, max-dequeued);
		OVERLAPPED_ENTRY oe[16];
		ret = GetQueuedCompletionStatusEx(wd->iocp, oe, 16, &entries, mswait, 0);
		/* [한국어] 최대 16개 완료 엔트리 배치 수거, alertable=FALSE. */
		if (ret && entries) {
			int entry_num;

			for (entry_num=0; entry_num<entries; entry_num++) {
				ovl = oe[entry_num].lpOverlapped;
				fov = CONTAINING_RECORD(ovl, struct fio_overlapped, o);
				/* [한국어] OVERLAPPED 필드 포인터 → 소속 fio_overlapped 복원. */
				io_u = fov->io_u;

				if (ovl->Internal == ERROR_SUCCESS) {
					io_u->resid = io_u->xfer_buflen - ovl->InternalHigh;
					io_u->error = 0;
				} else {
					io_u->resid = io_u->xfer_buflen;
					io_u->error = win_to_posix_error(GetLastError());
				}

				fov->io_complete = FALSE;
				wd->aio_events[dequeued] = io_u;
				dequeued++;
			}
		}

		if (dequeued >= min ||
			(t != NULL && timeout_expired(start_count, end_count)))
			break;
	} while (1);
	return dequeued;
}

/* dequeue completion entrees creates by separate IoCompletionRoutine thread */
/*
 * [한국어]
 * fio_windowaio_getevents_thread - 완료 스레드 모드 getevents.
 *
 * 완료 스레드가 이미 io_u마다 fov->io_complete를 set해 놓았으므로, 여기서는
 * in-flight 중인 io_u를 순회하며 플래그를 확인한다. 부족하면 iocomplete_event를
 * WaitForSingleObject로 기다리며 주기적으로 재검사.
 *
 * 호출 체인: fio_windowsaio_getevents → [이 함수] → io_u_qiter + WaitForSingleObject
 */
static int fio_windowaio_getevents_thread(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct windowsaio_data *wd = td->io_ops_data;
	unsigned int dequeued = 0;
	struct io_u *io_u;
	int i;
	struct fio_overlapped *fov;
	DWORD start_count = 0;
	DWORD end_count = 0;
	DWORD status;
	DWORD mswait = 250;

	if (t != NULL) {
		mswait = (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
		start_count = GetTickCount();
		end_count = start_count + (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
	}

	do {
		io_u_qiter(&td->io_u_all, io_u, i) {
			if (!(io_u->flags & IO_U_F_FLIGHT))
				continue;

			fov = (struct fio_overlapped*)io_u->engine_data;

			if (fov->io_complete) {
				fov->io_complete = FALSE;	/* [한국어] 재수확 방지. */
				wd->aio_events[dequeued] = io_u;
				dequeued++;
			}
		}
		if (dequeued >= min)
			break;

		if (dequeued < min) {
			status = WaitForSingleObject(wd->iocomplete_event, mswait);
			/* [한국어] 완료 스레드가 SetEvent 할 때까지(또는 mswait 경과까지) 블로킹.
			 * auto-reset 이벤트이므로 Wait 해제 시 자동 non-signaled 로 복귀. */
			if (status != WAIT_OBJECT_0 && dequeued >= min)
				break;
		}

		if (dequeued >= min ||
		    (t != NULL && timeout_expired(start_count, end_count)))
			break;
	} while (1);

	return dequeued;
}

/*
 * [한국어]
 * fio_windowsaio_getevents - no_completion_thread 옵션에 따라 두 경로 중 하나로 분기.
 */
static int fio_windowsaio_getevents(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct windowsaio_options *o = td->eo;

	if (o->no_completion_thread)
		return fio_windowsaio_getevents_nothread(td, min, max, t);
	return fio_windowaio_getevents_thread(td, min, max, t);
}

/*
 * [한국어]
 * fio_windowsaio_queue - Windows AIO 엔진의 I/O 제출 콜백.
 *
 * OVERLAPPED에 오프셋 상/하위 32비트를 분할 기록하고 ReadFile/WriteFile을 발행.
 * DDIR_SYNC/DATASYNC는 FlushFileBuffers로 동기 처리, DDIR_TRIM은 미지원.
 * ReadFile/WriteFile은 성공(동기 즉시 완료) 또는 ERROR_IO_PENDING(비동기 진행)이
 * 모두 "잘 접수됨"이며, 그 외는 실제 오류.
 *
 * 호출 체인: td_io_queue → [이 함수] → ReadFile/WriteFile/FlushFileBuffers
 */
static enum fio_q_status fio_windowsaio_queue(struct thread_data *td,
					      struct io_u *io_u)
{
	struct fio_overlapped *o = io_u->engine_data;
	LPOVERLAPPED lpOvl = &o->o;
	BOOL success = FALSE;
	int rc = FIO_Q_COMPLETED;

	fio_ro_check(td, io_u);

	lpOvl->Internal = 0;			/* [한국어] NT 상태코드 슬롯 초기화. */
	lpOvl->InternalHigh = 0;		/* [한국어] 전송 바이트 수 슬롯 초기화. */
	lpOvl->Offset = io_u->offset & 0xFFFFFFFF;	/* [한국어] 64비트 오프셋의 하위 32비트. */
	lpOvl->OffsetHigh = io_u->offset >> 32;		/* [한국어] 상위 32비트. */

	switch (io_u->ddir) {
	case DDIR_WRITE:
		success = WriteFile(io_u->file->hFile, io_u->xfer_buf,
					io_u->xfer_buflen, NULL, lpOvl);
		/* [한국어] lpNumberOfBytesWritten=NULL — 비동기 완료 시 OVERLAPPED.InternalHigh로 수신. */
		break;
	case DDIR_READ:
		success = ReadFile(io_u->file->hFile, io_u->xfer_buf,
					io_u->xfer_buflen, NULL, lpOvl);
		break;
	case DDIR_SYNC:
	case DDIR_DATASYNC:
	case DDIR_SYNC_FILE_RANGE:
		success = FlushFileBuffers(io_u->file->hFile);
		/* [한국어] 동기 플러시 — Windows는 OVERLAPPED 버전 플러시가 없음. */
		if (!success) {
			log_err("windowsaio: failed to flush file buffers\n");
			io_u->error = win_to_posix_error(GetLastError());
		}

		return FIO_Q_COMPLETED;
	case DDIR_TRIM:
		log_err("windowsaio: manual TRIM isn't supported on Windows\n");
		io_u->error = 1;
		io_u->resid = io_u->xfer_buflen;
		return FIO_Q_COMPLETED;
	default:
		assert(0);
		break;
	}

	if (success || GetLastError() == ERROR_IO_PENDING)
		rc = FIO_Q_QUEUED;
		/* [한국어] 동기 완료든 비동기 pending이든 "접수됨" — fio에 QUEUED로 보고.
		 * 완료 자체는 어쨌든 IOCP를 통해 보고되므로 수집 경로는 동일. */
	else {
		io_u->error = win_to_posix_error(GetLastError());
		io_u->resid = io_u->xfer_buflen;
	}

	return rc;
}

/* Runs as a thread and waits for queued IO to complete */
/*
 * [한국어]
 * IoCompletionRoutine - 완료 스레드 엔트리 포인트. 잡당 1개 생성.
 *
 * GetQueuedCompletionStatus를 250ms 타임아웃으로 호출하면서 완료 패킷을 수거,
 * OVERLAPPED → fio_overlapped 역참조로 대응 io_u를 찾아 결과(에러/잔여)를 기록하고
 * io_complete=TRUE로 set한 뒤 iocomplete_event를 signal. 잡이 iothread_running=FALSE로
 * 플래그를 내리면 루프 종료, 컨텍스트와 IOCP를 해제한다.
 *
 * 실행 컨텍스트: CreateThread로 생성된 별도 Windows 스레드. 잡 스레드와 별개.
 */
static DWORD WINAPI IoCompletionRoutine(LPVOID lpParameter)
{
	OVERLAPPED *ovl;
	struct fio_overlapped *fov;
	struct io_u *io_u;
	struct windowsaio_data *wd;
	struct thread_ctx *ctx;
	ULONG_PTR ulKey = 0;
	DWORD bytes;

	ctx = (struct thread_ctx*)lpParameter;
	wd = ctx->wd;

	do {
		BOOL ret;

		ret = GetQueuedCompletionStatus(ctx->iocp, &bytes, &ulKey,
						&ovl, 250);
		/* [한국어] 250ms 타임아웃으로 대기 — 종료 플래그 주기적 확인 목적. */
		if (!ret && ovl == NULL)
			continue;
		/* [한국어] ret=FALSE & ovl=NULL: 타임아웃. 단순 재시도. */

		fov = CONTAINING_RECORD(ovl, struct fio_overlapped, o);
		io_u = fov->io_u;

		if (ovl->Internal == ERROR_SUCCESS) {
			io_u->resid = io_u->xfer_buflen - ovl->InternalHigh;
			io_u->error = 0;
		} else {
			io_u->resid = io_u->xfer_buflen;
			io_u->error = win_to_posix_error(GetLastError());
		}

		fov->io_complete = TRUE;	/* [한국어] 잡 스레드에 완료 시그널(flag). */
		SetEvent(wd->iocomplete_event);	/* [한국어] Wait 블로킹 중인 잡 스레드 깨움. */
	} while (ctx->wd->iothread_running);

	CloseHandle(ctx->iocp);
	free(ctx);
	return 0;
}

/*
 * [한국어]
 * fio_windowsaio_io_u_free - io_u 파괴 시 engine_data(fio_overlapped) 해제.
 */
static void fio_windowsaio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct fio_overlapped *o = io_u->engine_data;

	if (o) {
		io_u->engine_data = NULL;
		free(o);
	}
}

/*
 * [한국어]
 * fio_windowsaio_io_u_init - io_u 생성 시 fio_overlapped 할당 + 역링크/플래그 초기화.
 * OVERLAPPED.hEvent=NULL: IOCP 사용 시 이벤트 기반 알림을 비활성화.
 */
static int fio_windowsaio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct fio_overlapped *o;

	o = malloc(sizeof(*o));
	o->io_complete = FALSE;
	o->io_u = io_u;
	o->o.hEvent = NULL;
	io_u->engine_data = o;
	return 0;
}

/* [한국어] 엔진 vtable — fio 코어가 이 테이블을 통해 콜백을 디스패치. */
static struct ioengine_ops ioengine = {
	.name		= "windowsaio",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_windowsaio_init,
	.queue		= fio_windowsaio_queue,
	.getevents	= fio_windowsaio_getevents,
	.event		= fio_windowsaio_event,
	.cleanup	= fio_windowsaio_cleanup,
	.open_file	= fio_windowsaio_open_file,
	.close_file	= fio_windowsaio_close_file,
	.get_file_size	= generic_get_file_size,
	.io_u_init	= fio_windowsaio_io_u_init,
	.io_u_free	= fio_windowsaio_io_u_free,
	.options	= options,
	.option_struct_size	= sizeof(struct windowsaio_options),
};

/*
 * [한국어]
 * fio_windowsaio_register - 프로그램 시작 시 엔진을 코어에 등록(constructor).
 */
static void fio_init fio_windowsaio_register(void)
{
	register_ioengine(&ioengine);
}

/*
 * [한국어]
 * fio_windowsaio_unregister - 종료 시 등록 해제(destructor).
 */
static void fio_exit fio_windowsaio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
