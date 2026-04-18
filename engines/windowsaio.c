/*
 * windowsaio engine
 *
 * IO engine using Windows IO Completion Ports.
 */

/*
 * [한국어 설명] Windows IOCP 기반 비동기 I/O 엔진 구현 (windowsaio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Microsoft Windows의 I/O Completion Ports(IOCP) 메커니즘을 이용하여
 * fio가 Windows 환경에서 비동기 디스크 I/O를 수행할 수 있게 하는 ioengine 플러그인이다.
 * POSIX aio / Linux io_uring에 대응하는 Windows 네이티브 비동기 I/O 모델로서,
 * FILE_FLAG_OVERLAPPED로 연 HANDLE에 ReadFile/WriteFile을 OVERLAPPED 구조체와 함께
 * 전달하면 NT I/O Manager가 요청을 큐잉하고, 완료 시 연결된 IOCP(IO Completion Port)에
 * 완료 패킷을 포스팅한다. 엔진은 잡 단위로 1개의 IOCP를 생성하고, 잡이 여는 모든
 * 파일 핸들을 이 IOCP에 바인딩한 후, io_u마다 fio_overlapped(= OVERLAPPED + io_u 역링크)
 * 를 할당해 커널과 사용자공간 사이에서 완료를 추적한다. 두 가지 완료 수집 모드가 있다.
 * 기본 모드에서는 별도의 "완료 스레드"(CreateThread로 생성된 IoCompletionRoutine)가
 * GetQueuedCompletionStatus()를 상시 호출해 완료 패킷을 수거하고 io_complete 플래그를
 * 세팅한 뒤 auto-reset 이벤트(iocomplete_event)를 signal하여 잡 스레드를 깨운다.
 * no_completion_thread=1 옵션 모드에서는 잡 스레드가 getevents 호출 내부에서 직접
 * GetQueuedCompletionStatusEx()로 배치(최대 16개) 수거를 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend의 잡 루프는 load_ioengine("windowsaio")를 통해 아래 ioengine_ops 테이블을
 * 찾고, init → open_file → io_u_init → queue → getevents → event → ... → close_file →
 * cleanup 순으로 콜백을 호출한다. 호출 체인(주 경로):
 *   backend.c:do_io() → ioengines.c:td_io_queue() → fio_windowsaio_queue()
 *     → ReadFile()/WriteFile()/FlushFileBuffers() (NT syscalls로 내려감)
 *   완료 경로 1 (기본, 완료 스레드 모드):
 *     NT I/O Manager → IOCP → [별도 스레드] IoCompletionRoutine()
 *       → fov->io_complete=TRUE + SetEvent(iocomplete_event)
 *       → 잡 스레드 fio_windowaio_getevents_thread() (WaitForSingleObject)
 *   완료 경로 2 (no_completion_thread=1):
 *     잡 스레드 fio_windowsaio_getevents_nothread()
 *       → GetQueuedCompletionStatusEx() 직접 호출.
 * 실행 컨텍스트: Windows에서 fio 잡은 프로세스/스레드 두 모델 모두 지원하지만, IOCP는
 * 프로세스 내 공유 리소스이므로 이 엔진은 실질적으로 스레드 기반 잡에 최적화되어 있다.
 * 완료 스레드는 Win32 CreateThread()로 생성되며, 잡 옵션으로 cpumask가 지정되어 있으면
 * 완료 스레드도 fio_setaffinity()를 통해 같은 CPU 마스크로 바인딩된다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존 모듈 (Windows 측):
 *   * CreateFile / CloseHandle — 파일/디바이스 HANDLE 생명주기 (kernel32)
 *   * CreateIoCompletionPort — IOCP 생성 및 파일 핸들의 포트 바인딩 (kernel32)
 *   * ReadFile / WriteFile / FlushFileBuffers — 비동기 I/O 제출과 동기 플러시 (kernel32)
 *   * GetQueuedCompletionStatus / GetQueuedCompletionStatusEx — 완료 수거 (kernel32)
 *   * CreateEvent / SetEvent / WaitForSingleObject — 완료 스레드 ↔ 잡 스레드 동기화
 *   * CreateThread — 완료 처리 스레드 생성 (kernel32)
 *   * GetLastError / GetTickCount — Win32 오류/타임스탬프
 * - 의존 모듈 (fio 측):
 *   * fio.h — thread_data, io_u, ioengine_ops, FIO_Q_* 계약, fio_ro_check, io_u_qiter 등
 *   * optgroup.h — FIO_OPT_C_ENGINE / FIO_OPT_G_WINDOWSAIO 옵션 분류
 *   * os/windows/* — win_to_posix_error(), geterrno_from_win_error() 에러 변환
 * - 이 파일에 의존하는 모듈: fio 코어 런타임(ioengines.c의 load_ioengine)이 이름
 *   "windowsaio"로 ioengine 구조체를 조회한다. Windows 빌드에서 실질적 기본 엔진이다.
 * - 데이터 흐름: io_u(xfer_buf, xfer_buflen, offset, ddir)
 *     → OVERLAPPED.Offset/OffsetHigh (64비트 오프셋 분할)
 *     → ReadFile/WriteFile에 HANDLE + 버퍼 + OVERLAPPED 포인터 전달
 *     → NT I/O Manager가 커널 I/O 수행
 *     → 완료 시 OVERLAPPED.Internal = NTSTATUS, InternalHigh = 전송 바이트
 *     → CONTAINING_RECORD(ovl, struct fio_overlapped, o)로 소속 구조체 복원
 *     → io_u->error, io_u->resid(잔여 바이트)로 환산.
 * - 공유 자료구조:
 *   * td->io_ops_data = struct windowsaio_data* (잡 단위 IOCP/완료스레드/이벤트/카운터 보관)
 *   * io_u->engine_data = struct fio_overlapped* (OVERLAPPED + io_u 역링크 + 완료 플래그)
 *   * fio_file->hFile = HANDLE (개별 파일 핸들; 파일 열기 시 IOCP에 바인딩됨)
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_overlapped:
 *     io_u와 1:1로 매핑되는 확장 OVERLAPPED. engine_data 슬롯에 저장.
 *     CONTAINING_RECORD 이디엄으로 커널이 반환한 OVERLAPPED*에서 fio_overlapped*를 복원.
 * - struct windowsaio_data:
 *     잡 단위 상태. iocp / iothread / iocomplete_event / aio_events 배열 / 스레드 런플래그.
 * - struct windowsaio_options:
 *     엔진별 옵션. 현재는 no_completion_thread 하나.
 * - fio_windowsaio_init(): windowsaio_data 할당, IOCP/이벤트/완료 스레드 생성.
 * - fio_windowsaio_open_file(): FILE_FLAG_OVERLAPPED로 CreateFile, IOCP에 연결, 직접 IO/힌트 처리.
 * - fio_windowsaio_queue(): OVERLAPPED에 오프셋 주입 후 ReadFile/WriteFile/FlushFileBuffers.
 * - IoCompletionRoutine(): 완료 스레드 — IOCP에서 완료 패킷 수거 → io_complete=TRUE + SetEvent.
 * - fio_windowsaio_getevents(): 옵션에 따라 thread 모드 / nothread 모드 분기.
 * - fio_windowsaio_cleanup(): 완료 스레드 종료, IOCP/이벤트 핸들/메모리 해제.
 *
 * ioengine_ops 계약 요약(이 엔진 해당 사항):
 *   .init        — 잡 1회. 실패 시 rc=1 반환, 잡 중단.
 *   .cleanup     — 잡 1회. init의 역동작. 완료 스레드 join 필수.
 *   .open_file   — fio_file마다 1회. CreateFile + IOCP 바인딩.
 *   .close_file  — open_file 짝. CloseHandle만 수행(IOCP 바인딩 자동 해제).
 *   .io_u_init   — io_u마다 1회. fio_overlapped 할당/링크.
 *   .io_u_free   — io_u_init 짝. engine_data 해제.
 *   .queue       — io_u 1개 제출. 반환: FIO_Q_QUEUED(접수) / FIO_Q_COMPLETED(동기 결정).
 *   .getevents   — 최소 min개 완료까지 수거. 반환=수거 개수.
 *   .event       — getevents가 반환한 N개 중 i번째 io_u 조회.
 *   .get_file_size — generic_get_file_size 위임(코어 공통).
 *   .options / .option_struct_size — 엔진별 옵션 테이블 노출.
 *
 * 주의: 이 엔진은 FIO_SYNCIO 플래그를 사용하지 않는다 — 기본적으로 비동기 엔진이며,
 * DDIR_SYNC/DATASYNC는 FlushFileBuffers로 동기 처리하되 queue()가 FIO_Q_COMPLETED로
 * 즉시 보고해 코어가 commit/getevents를 건너뛰게 한다.
 */

/* [한국어] <stdio.h> — printf/fprintf 계열 표준 I/O. dprint()/log_err()/log_info() 매크로가
 * 내부적으로 이 헤더의 함수를 간접 사용하므로 포함. */
#include <stdio.h>
/* [한국어] <stdlib.h> — malloc/calloc/free 프로토타입. fio_overlapped, windowsaio_data,
 * aio_events 배열을 힙에 할당하는 데 필수. */
#include <stdlib.h>
/* [한국어] <unistd.h> — POSIX 호환 유틸(close/read/write 등)의 프로토타입. Windows 빌드에서는
 * mingw/cygwin 헤더가 이를 얇게 에뮬레이션하며, fio 공통 규약상 포함한다. */
#include <unistd.h>
/* [한국어] <signal.h> — signal(2) 프로토타입. 이 파일에서 직접 시그널 설치를 하진 않지만
 * fio 공통 헤더 의존성 및 POSIX 호환 빌드 환경에서의 전방 선언 일관성을 위해 포함. */
#include <signal.h>
/* [한국어] <errno.h> — errno 전역 변수. win_to_posix_error()가 변환한 Windows 에러를
 * POSIX errno 값으로 저장할 수 있도록 노출. io_u->error에도 POSIX errno가 들어간다. */
#include <errno.h>

/* [한국어] "../fio.h" — fio 코어의 모든 공용 타입·매크로·선언의 허브:
 *  * struct thread_data, struct io_u, struct ioengine_ops, struct fio_file
 *  * FIO_Q_COMPLETED / FIO_Q_QUEUED / FIO_Q_BUSY 반환값 enum
 *  * DDIR_READ/WRITE/SYNC/DATASYNC/TRIM 등 I/O 방향
 *  * dprint(), log_err(), log_info(), fio_init/fio_exit 매크로
 *  * register_ioengine(), unregister_ioengine()
 *  * fio_ro_check(), io_u_qiter(), fio_option_is_set(), fio_setaffinity()
 *  * FIO_IOOPS_VERSION (ABI 버전 체크용 상수)
 *  * fio_file::hFile 필드 (Windows 빌드에서 Win32 HANDLE 타입) */
#include "../fio.h"
/* [한국어] "../optgroup.h" — fio --cmdhelp에 노출되는 옵션 카테고리/그룹 enum:
 *  * FIO_OPT_C_ENGINE   : "이 옵션은 I/O 엔진 카테고리" 표식
 *  * FIO_OPT_G_WINDOWSAIO : "windowsaio 엔진 옵션 그룹" 서브그룹 표식
 * 아래 options[] 테이블에서 .category/.group 필드에 사용. */
#include "../optgroup.h"

/* [한국어] CANCELIOEX: Win32 CancelIoEx API 함수 포인터 타입 정의.
 * 파라미터: (HANDLE 파일핸들, LPOVERLAPPED 특정 요청)
 * WINAPI(__stdcall) 호출 규약은 Win32 표준. 일부 Windows 런타임에선
 * CancelIoEx를 GetProcAddress로 동적 로드하기 위해 이런 함수포인터 타입이 필요하다.
 * 현재 파일에서는 선언만 있고 실제 호출은 없음(취소 경로 미구현) — 향후 I/O 취소
 * 지원을 염두에 둔 전방 선언. */
typedef BOOL (WINAPI *CANCELIOEX)(HANDLE hFile, LPOVERLAPPED lpOverlapped);

/* [한국어] geterrno_from_win_error: os/windows/*(또는 oslib)에서 제공하는 헬퍼의 전방 선언.
 * Win32 GetLastError() 값(DWORD)을 POSIX errno(int)로 변환하며, 변환 테이블에 없으면
 * deferrno를 폴백으로 반환한다. 여기서는 선언만 두고, 구현은 플랫폼별 파일에 있다.
 * (현재 파일에서는 대신 win_to_posix_error가 사용되고 있다 — 레거시 호환 선언) */
int geterrno_from_win_error (DWORD code, int deferrno);

/* [한국어] struct fio_overlapped: io_u 1개에 대응하는 확장 OVERLAPPED 블록.
 *
 * 설계 배경:
 *   Win32의 OVERLAPPED 구조체는 비동기 I/O가 "pending" 상태인 동안 절대로 다른
 *   용도로 재사용되거나 해제되어서는 안 된다(OVERLAPPED lifetime rule).
 *   따라서 io_u마다 1개를 별도로 할당해 io_u 생명주기와 연결한다.
 * 복원 이디엄:
 *   완료 스레드가 받은 OVERLAPPED* 포인터로부터 Windows의 CONTAINING_RECORD 매크로를
 *   이용해 소속 fio_overlapped*를 역추적한다 — C의 offsetof 기반 트릭으로, .o 필드가
 *   구조체 맨 앞에 있어야 안전한 패턴이지만 CONTAINING_RECORD는 임의 오프셋도 지원.
 * 생명주기:
 *   io_u_init 시 1회 malloc → io_u->engine_data로 연결 → io_u_free 시 free.
 *   잡 단위 모든 io_u는 같은 iocp에 묶여 완료 처리된다. */
struct fio_overlapped {
	OVERLAPPED o;
	/* [한국어] Win32 OVERLAPPED 구조체(인라인 포함). 비동기 I/O의 기술자.
	 * 주요 필드 (Windows SDK 정의):
	 *   - Internal     : 완료 후 NTSTATUS 코드. 성공 시 STATUS_SUCCESS(ERROR_SUCCESS=0).
	 *   - InternalHigh : 완료 후 실제 전송된 바이트 수.
	 *   - Offset       : 파일 내 시작 오프셋의 하위 32비트.
	 *   - OffsetHigh   : 상위 32비트(합쳐서 64비트 오프셋).
	 *   - hEvent       : 완료 이벤트 핸들. IOCP 사용 시 NULL 권장(이벤트 알림 이중화 방지).
	 * 설정자:
	 *   fio_windowsaio_queue()가 Internal=0, InternalHigh=0, Offset/OffsetHigh 초기화.
	 *   fio_windowsaio_io_u_init()가 hEvent=NULL 설정.
	 *   커널(NT I/O Manager)이 I/O 완료 시 Internal/InternalHigh를 덮어씀.
	 * 읽는 자:
	 *   IoCompletionRoutine 또는 fio_windowsaio_getevents_nothread가 Internal/InternalHigh
	 *   를 읽어 io_u->error/resid를 환산.
	 * 값 범위: Windows SDK <winnt.h> 정의에 따름.
	 * 동기화: 커널 writer ↔ 유저공간 reader. I/O 제출 시점부터 완료 수확까지 이 메모리는
	 *         절대 재사용/해제되어선 안 된다. fence는 IOCP 큐잉(완료 포스팅)이 역할. */

	struct io_u *io_u;
	/* [한국어] 이 OVERLAPPED에 대응하는 fio io_u로의 역링크.
	 * 설정자: fio_windowsaio_io_u_init()가 io_u→fio_overlapped 생성 직후 1회 세팅.
	 * 읽는 자: IoCompletionRoutine과 fio_windowsaio_getevents_nothread가 CONTAINING_RECORD
	 *          로 fio_overlapped를 복원한 뒤 이 필드로 io_u를 얻어 결과를 기록.
	 * 값 범위: 유효한 io_u 포인터(NULL 불가, io_u 생명주기와 일치).
	 * 동기화: io_u 1개 ↔ fio_overlapped 1개 고정 페어. 잡 스레드가 생성/해제하며,
	 *         중간 소유권은 커널 I/O가 완료될 때까지 이 페어가 I/O "in-flight" 상태를 표현. */

	BOOL io_complete;
	/* [한국어] 완료 스레드(IoCompletionRoutine)가 "이 io_u 완료 도착함"을 잡 스레드에 알리는
	 * user-space 플래그. no_completion_thread=0(기본) 모드 전용.
	 * 설정자:
	 *   - fio_windowsaio_io_u_init()가 FALSE로 초기화.
	 *   - IoCompletionRoutine()가 완료 수거 시 TRUE로 세팅.
	 *   - fio_windowaio_getevents_thread() 또는 getevents_nothread가 수확 완료 후 FALSE로 리셋.
	 * 읽는 자: fio_windowaio_getevents_thread의 io_u_qiter 루프에서 체크.
	 * 값 범위: TRUE | FALSE.
	 * 동기화: 완료 스레드 writer(TRUE) ↔ 잡 스레드 reader/writer(FALSE 리셋).
	 *         SetEvent/WaitForSingleObject의 메모리 배리어가 가시성 보장.
	 *         읽기 원자성은 BOOL(=int) 정렬 write에 의존. race는 잡 스레드가 Wait 직후
	 *         전체 io_u_qiter를 다시 돌리므로 최악의 경우 한 사이클 지연으로 흡수됨. */
};

/* [한국어] struct windowsaio_data: 잡 단위 엔진 상태 구조체. td->io_ops_data에 매달림.
 * 잡 스레드와 완료 스레드(있는 경우) 양쪽에서 접근하므로 필드별 동기화 규약 주의.
 * 생명주기: fio_windowsaio_init에서 calloc → fio_windowsaio_cleanup에서 free. */
struct windowsaio_data {
	struct io_u **aio_events;
	/* [한국어] getevents가 fio 코어에 반환할 "완료된 io_u" 순서 배열. 크기=iodepth.
	 * 설정자:
	 *   - fio_windowsaio_init()가 malloc(iodepth * sizeof(io_u*))로 할당.
	 *   - fio_windowsaio_getevents_nothread 또는 getevents_thread가 수거된 순서대로 write.
	 * 읽는 자: fio_windowsaio_event()가 인덱스로 access해 fio 코어에 io_u 반환.
	 * 값 범위: 유효한 in-flight io_u 포인터들. 수확 후 다음 getevents 전까지 유효.
	 * 동기화: 잡 스레드 전용. 완료 스레드는 io_complete 플래그만 건드리고 이 배열은 만지지 않음. */

	HANDLE iocp;
	/* [한국어] 이 잡 전용 I/O Completion Port 핸들.
	 * 설정자: fio_windowsaio_init()에서 CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)
	 *         로 빈 IOCP 생성.
	 * 읽는 자:
	 *   - fio_windowsaio_open_file()가 CreateIoCompletionPort(f->hFile, wd->iocp, ...)로
	 *     파일 핸들을 이 포트에 바인딩.
	 *   - GetQueuedCompletionStatus / GetQueuedCompletionStatusEx의 첫 인자.
	 *   - 완료 스레드가 thread_ctx로 복사 받아 사용.
	 * 값 범위: 유효한 Win32 커널 객체 핸들. NULL(=INVALID)이면 생성 실패.
	 * 동기화: 커널 내부에서 스레드-세이프. 완료 패킷은 내부 큐에 직렬화. */

	HANDLE iothread;
	/* [한국어] 완료 처리 스레드 핸들. no_completion_thread=0(기본)일 때만 유효.
	 * 설정자: fio_windowsaio_init()에서 CreateThread(... IoCompletionRoutine, ctx, ...).
	 * 읽는 자: fio_windowsaio_cleanup()가 WaitForSingleObject로 join + CloseHandle.
	 * 값 범위: 유효한 스레드 핸들, 또는 NULL(no_completion_thread=1인 잡).
	 * 동기화: cleanup에서 iothread_running=FALSE를 먼저 세팅해야 레이스 없이 join 가능. */

	HANDLE iocomplete_event;
	/* [한국어] 완료 스레드가 잡 스레드를 깨우기 위한 auto-reset Win32 Event.
	 * 설정자: fio_windowsaio_init()가 CreateEvent(NULL, FALSE, FALSE, NULL) —
	 *         2번째 인자 FALSE = auto-reset(Wait 해제 시 자동으로 non-signaled 복귀),
	 *         3번째 인자 FALSE = 초기 non-signaled 상태.
	 * 읽는 자: fio_windowaio_getevents_thread()가 WaitForSingleObject로 대기.
	 * Signal 트리거: IoCompletionRoutine()가 각 완료 수거 후 SetEvent로 포스트.
	 * 값 범위: signaled / non-signaled 두 상태.
	 * 동기화: Win32 커널 객체이므로 SetEvent/WaitForSingleObject가 내부적으로 배리어 제공. */

	BOOL iothread_running;
	/* [한국어] 완료 스레드 메인 루프의 실행 플래그. cleanup 시 FALSE로 내려 루프 종료 유도.
	 * 설정자: fio_windowsaio_init()에서 TRUE, fio_windowsaio_cleanup()에서 FALSE.
	 * 읽는 자: IoCompletionRoutine()가 do/while 조건으로 체크.
	 * 값 범위: TRUE | FALSE.
	 * 동기화: single-writer(잡 스레드) / single-reader(완료 스레드). 전이는 TRUE→FALSE
	 *         단방향이고, 완료 스레드는 GQCS의 250ms 타임아웃마다 재검사하므로 지연은
	 *         최대 250ms 수준에서 흡수된다. 강한 메모리 배리어 불필요(전이 단방향 + 주기적 폴). */
};

/* [한국어] struct thread_ctx: 완료 스레드 시작 시 전달하는 생성 컨텍스트.
 * fio_windowsaio_init에서 malloc → CreateThread의 lpParameter로 전달 →
 * IoCompletionRoutine 종료 시 free. iocp와 wd를 묶어 한 번에 넘기기 위한 래퍼. */
struct thread_ctx {
	HANDLE iocp;
	/* [한국어] 완료 스레드가 수거할 IOCP 핸들(windowsaio_data::iocp의 복사본).
	 * 설정자: fio_windowsaio_init()가 ctx->iocp = hFile로 주입.
	 * 읽는 자: IoCompletionRoutine()이 GetQueuedCompletionStatus의 첫 인자로 사용.
	 * 값 범위: 유효한 IOCP 핸들. 완료 스레드 종료 시 CloseHandle로 닫는다(소유권 이전).
	 * 동기화: 한 스레드 전용, 락 불필요. */

	struct windowsaio_data *wd;
	/* [한국어] 완료 처리에 필요한 엔진 상태 포인터. 주로 iocomplete_event와
	 * iothread_running 접근용.
	 * 설정자: fio_windowsaio_init()가 ctx->wd = wd.
	 * 읽는 자: IoCompletionRoutine이 SetEvent(wd->iocomplete_event) 및 루프 조건에 사용.
	 * 값 범위: 유효한 windowsaio_data 포인터. 잡 수명동안 유효.
	 * 동기화: 완료 스레드와 잡 스레드가 공유하는 공유 상태 — 필드별 동기화는 windowsaio_data 참조. */
};

/* [한국어] struct windowsaio_options: 엔진별 옵션 구조체. fio 옵션 파서가 잡당 1개 할당.
 * td->eo에 연결되며, 크기는 아래 ioengine.option_struct_size로 전달된다. */
struct windowsaio_options {
	struct thread_data *td;
	/* [한국어] 옵션 파서가 역참조용으로 기록하는 상위 thread_data.
	 * 설정자: fio 옵션 인프라가 옵션 구조체 할당 시 자동 세팅.
	 * 읽는 자: 옵션 콜백(현 파일에선 미사용).
	 * 값 범위: 유효한 td 포인터.
	 * 동기화: 잡 단위 소유, 불변. */

	unsigned int no_completion_thread;
	/* [한국어] 완료 수거 모드 선택 플래그. 0=별도 완료 스레드 사용(기본),
	 * 1=잡 스레드가 getevents 내부에서 직접 GQCS-Ex 호출.
	 * 설정자: 옵션 파서가 잡 파일/CLI의 `no_completion_thread=1` 매칭 시 세팅.
	 * 읽는 자: fio_windowsaio_init(완료 스레드 생성 여부), fio_windowsaio_getevents(분기).
	 * 값 범위: 0 또는 1 (FIO_OPT_STR_SET 타입).
	 * 동기화: 잡 시작 시 1회 설정 후 불변, 락 불필요. */
};

/* [한국어] options[]: 이 엔진이 노출하는 잡 파일/CLI 옵션 목록.
 * 마지막은 .name=NULL 센티널로 종료. fio_option 구조체 배열은 ioengine.options에 연결된다. */
static struct fio_option options[] = {
	{
		.name	= "no_completion_thread",
		/* [한국어] 옵션의 단축/식별 키. 잡파일: `no_completion_thread=1`, CLI: `--no_completion_thread=1`. */
		.lname	= "No completion polling thread",
		/* [한국어] 사람이 읽을 긴 이름. --cmdhelp 출력 및 도움말에 표시됨. */
		.type	= FIO_OPT_STR_SET,
		/* [한국어] 불리언 플래그 타입(값 유무만 확인; =1 또는 단독 지정 시 set). */
		.off1	= offsetof(struct windowsaio_options, no_completion_thread),
		/* [한국어] 옵션이 기록될 struct windowsaio_options 내 필드 오프셋.
		 * 파서가 td->eo + off1 위치에 값을 써 넣는다. */
		.help	= "Use to avoid separate completion polling thread",
		/* [한국어] --cmdhelp/잡파일 도움말에 표시될 1줄 설명. */
		.category = FIO_OPT_C_ENGINE,
		/* [한국어] 옵션 카테고리: "I/O 엔진 관련". --cmdhelp에서 그룹핑에 사용. */
		.group	= FIO_OPT_G_WINDOWSAIO,
		/* [한국어] 세부 그룹: "windowsaio 엔진". */
	},
	{
		.name	= NULL,
		/* [한국어] 배열 종료 센티널 — fio 옵션 파서가 name=NULL을 만나면 스캔 종료. */
	},
};

/* [한국어] IoCompletionRoutine 전방 선언 — fio_windowsaio_init의 CreateThread 인자로
 * 참조되기 때문에 함수 정의보다 먼저 선언이 필요. */
static DWORD WINAPI IoCompletionRoutine(LPVOID lpParameter);

/*
 * [한국어]
 * fio_windowsaio_init - 잡 초기화 시 IOCP, 완료 이벤트, 완료 스레드를 생성하고
 *                       windowsaio_data를 td->io_ops_data에 부착한다.
 *
 * @td:     이 잡(스레드/프로세스)에 대응하는 thread_data. td->o.iodepth가 aio_events
 *          배열 크기를 결정. td->eo는 struct windowsaio_options로 캐스팅됨.
 * @return: 0=성공, 1=어느 단계에서든 실패(잡이 중단됨).
 *
 * 단계:
 *   1) windowsaio_data calloc (실패 시 rc=1)
 *   2) aio_events 배열 malloc (iodepth * io_u*)
 *   3) auto-reset iocomplete_event 생성(CreateEvent)
 *   4) 실패 시 여기까지 자원 역순 해제
 *   5) td->io_ops_data에 연결
 *   6) 빈 IOCP 생성(CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...))
 *   7) iothread_running=TRUE, iocp 저장
 *   8) no_completion_thread==0이면 thread_ctx malloc + CreateThread로 IoCompletionRoutine 시작
 *   9) cpumask 옵션 지정 시 fio_setaffinity로 완료 스레드 CPU 바인딩
 *
 * 호출 체인: backend.c:do_io() 진입 전 → ioengines.c:td_io_init() → ioengine_ops.init (이 함수)
 *           → Win32 CreateIoCompletionPort / CreateEvent / CreateThread.
 * 실행 컨텍스트: 해당 잡의 메인 스레드. 1회만 호출.
 */
static int fio_windowsaio_init(struct thread_data *td)
{
	struct windowsaio_data *wd;  /* [한국어] 할당할 엔진 상태 포인터(이후 td->io_ops_data로 저장). */
	int rc = 0;                  /* [한국어] 누적 에러 플래그(0=성공). 단계별 가드 역할. */

	wd = calloc(1, sizeof(struct windowsaio_data));
	/* [한국어] 잡 상태 구조체를 0으로 초기화 할당. HANDLE들도 NULL(=잘못된 핸들)로 시작. */
	if (wd == NULL) {
		/* [한국어] calloc 실패: 호스트 OOM. 잡을 중단해야 함. */
		 log_err("windowsaio: failed to allocate memory for engine data\n");
		rc = 1;
	}

	if (!rc) {
		/* [한국어] wd 할당 성공 시에만 진입. */
		wd->aio_events = malloc(td->o.iodepth * sizeof(struct io_u*));
		/* [한국어] getevents가 보고할 수 있는 최대 완료 수 = iodepth. 배열 할당. */
		if (wd->aio_events == NULL) {
			/* [한국어] OOM. wd는 아래 에러 처리 블록에서 회수. */
			log_err("windowsaio: failed to allocate memory for aio events list\n");
			rc = 1;
		}
	}

	if (!rc) {
		/* Create an auto-reset event */
		wd->iocomplete_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		/* [한국어] Win32 CreateEvent:
		 *  - 1번 NULL        : 기본 보안 속성
		 *  - 2번 FALSE       : auto-reset (Wait 해제 직후 커널이 non-signaled로 되돌림)
		 *  - 3번 FALSE       : 초기 상태 non-signaled
		 *  - 4번 NULL        : 익명 이벤트(이름 없음 → 프로세스 내 공유만)
		 * 완료 스레드가 SetEvent, 잡 스레드가 WaitForSingleObject로 사용할 동기화 이벤트. */
		if (wd->iocomplete_event == NULL) {
			/* [한국어] CreateEvent 실패는 매우 드물지만 방어. */
			log_err("windowsaio: failed to create io complete event handle\n");
			rc = 1;
		}
	}

	if (rc) {
		/* [한국어] 위 단계들에서 한 번이라도 실패하면 역순 정리. */
		if (wd != NULL) {
			/* [한국어] wd calloc은 성공했지만 이후 단계에서 실패한 경우. */
			if (wd->aio_events != NULL)
				free(wd->aio_events);
				/* [한국어] aio_events가 할당됐다면 해제(iocomplete_event 생성 실패 경로). */

			free(wd);
			/* [한국어] wd 구조체 본체 해제. */
		}
	}

	td->io_ops_data = wd;
	/* [한국어] 성공이든 실패든 일단 포인터를 저장 — 실패 시 NULL(이전 free) 또는 부분 초기화된 wd.
	 * fio 코어는 init 실패 시 cleanup을 호출하지 않도록 설계되어 있으나, 안전을 위해 수행. */

	if (!rc) {
		/* [한국어] 여기까지 성공한 경우에만 IOCP + 완료 스레드 설정 단계 진입. */
		struct thread_ctx *ctx;                /* [한국어] 완료 스레드 전달용 컨텍스트. */
		struct windowsaio_data *wd;             /* [한국어] 상위 wd 섀도잉(의도적 재선언). */
		HANDLE hFile;                            /* [한국어] IOCP 핸들 임시 변수(이름 오해 주의: 파일 아닌 IOCP). */
		struct windowsaio_options *o = td->eo;  /* [한국어] 엔진 옵션(no_completion_thread 접근). */

		hFile = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
		/* [한국어] 빈 IOCP 생성.
		 *  - 1번 FileHandle=INVALID_HANDLE_VALUE : 파일 바인딩 없이 포트만 생성
		 *  - 2번 ExistingCompletionPort=NULL    : 새 포트 생성
		 *  - 3번 CompletionKey=0                : 파일 바인딩 시 각각 지정
		 *  - 4번 NumberOfConcurrentThreads=0    : 프로세서 수에 맡김
		 * 이후 open_file에서 각 파일 HANDLE을 이 IOCP에 연결한다. */
		if (hFile == INVALID_HANDLE_VALUE) {
			/* [한국어] IOCP 생성 실패 — Windows 자원 고갈 또는 권한 문제. */
			log_err("windowsaio: failed to create io completion port\n");
			rc = 1;
		}

		wd = td->io_ops_data;       /* [한국어] 외부 wd 재바인딩(섀도잉된 로컬 변수에 저장). */
		wd->iothread_running = TRUE; /* [한국어] 완료 스레드 메인 루프가 돌아야 함을 표시. */
		wd->iocp = hFile;             /* [한국어] 이후 open_file/getevents가 사용할 IOCP 저장. */

		if (o->no_completion_thread == 0) {
			/* [한국어] 기본 모드: 별도 완료 스레드를 돌려 완료 패킷을 상시 수거. */
			if (!rc)
				ctx = malloc(sizeof(struct thread_ctx));
				/* [한국어] 완료 스레드에 iocp와 wd를 함께 넘기기 위한 힙 컨텍스트 할당. */

			if (!rc && ctx == NULL) {
				/* [한국어] ctx malloc 실패 — IOCP는 이미 생성됐으므로 close가 필요. */
				log_err("windowsaio: failed to allocate memory for thread context structure\n");
				CloseHandle(hFile);
				/* [한국어] 방금 생성한 IOCP 회수. 완료 스레드가 없으면 소유할 곳이 없다. */
				rc = 1;
			}

			if (!rc) {
				DWORD threadid;
				/* [한국어] 완료 스레드의 OS 스레드 ID(affinity 설정에 사용). */

				ctx->iocp = hFile;  /* [한국어] 완료 스레드에 IOCP 핸들 전달(소유권 이전). */
				ctx->wd = wd;        /* [한국어] 엔진 상태 포인터 — 이벤트 signal용. */
				wd->iothread = CreateThread(NULL, 0, IoCompletionRoutine, ctx, 0, &threadid);
				/* [한국어] CreateThread:
				 *  - 1번 lpThreadAttributes=NULL : 기본 보안/inheritable=FALSE
				 *  - 2번 dwStackSize=0           : 기본 스택(1MiB)
				 *  - 3번 lpStartAddress          : IoCompletionRoutine 진입점
				 *  - 4번 lpParameter=ctx         : 힙 컨텍스트(스레드 종료 시 free 책임 있음)
				 *  - 5번 dwCreationFlags=0       : 즉시 실행(CREATE_SUSPENDED 아님)
				 *  - 6번 lpThreadId=&threadid    : OUT — affinity에 사용할 OS 스레드 ID 수신 */
				if (!wd->iothread)
					log_err("windowsaio: failed to create io completion thread\n");
					/* [한국어] 스레드 생성 실패 경고. 아래에서 rc=1 셋. */
				else if (fio_option_is_set(&td->o, cpumask))
					fio_setaffinity(threadid, td->o.cpumask);
					/* [한국어] 잡 옵션 cpumask가 명시적으로 설정된 경우 완료 스레드도 동일 마스크로 바인딩.
					 * fio_setaffinity는 내부적으로 Win32 SetThreadAffinityMask를 호출. */
			}
			if (rc || wd->iothread == NULL)
				rc = 1;
				/* [한국어] 이전 단계 실패 또는 스레드 생성 실패 시 전체 init 실패로 표시. */
		}
	}

	return rc;  /* [한국어] 0=성공, 1=실패. 호출자(ioengines.c:td_io_init)가 잡 중단 결정. */
}

/*
 * [한국어]
 * fio_windowsaio_cleanup - 잡 종료 시 완료 스레드 안전 종료 + 모든 핸들/메모리 해제.
 *
 * @td: 이 잡의 thread_data. td->io_ops_data에서 windowsaio_data를 꺼내 정리.
 *
 * 순서(중요):
 *   1) iothread_running=FALSE — 완료 스레드 루프 종료 신호
 *   2) WaitForSingleObject(iothread, INFINITE) — 스레드가 실제 종료될 때까지 대기 (race-free)
 *   3) CloseHandle(iothread), CloseHandle(iocomplete_event)
 *   4) free(aio_events), free(wd)
 *   5) td->io_ops_data = NULL (혹시 모를 재호출 방어)
 *
 * 주의: IOCP 핸들(iocp)은 IoCompletionRoutine이 자체 종료 시 CloseHandle + free(ctx)를
 *       수행하므로 여기서는 직접 close하지 않는다.
 *
 * 호출 체인: backend 잡 루프 종료 → ioengines.c:td_io_cleanup → ioengine_ops.cleanup (이 함수).
 * 실행 컨텍스트: 잡 메인 스레드, 1회.
 */
static void fio_windowsaio_cleanup(struct thread_data *td)
{
	struct windowsaio_data *wd;  /* [한국어] 엔진 상태 포인터. NULL 체크 후 정리. */

	wd = td->io_ops_data;         /* [한국어] init에서 저장한 상태 획득. */

	if (wd != NULL) {
		/* [한국어] init 실패 케이스에서도 td->io_ops_data가 NULL일 수 있어 가드. */
		wd->iothread_running = FALSE;
		/* [한국어] 완료 스레드 루프에 "종료하라" 신호. 다음 GQCS 타임아웃(최대 250ms) 내 인지됨.
		 * single-writer/single-reader라 atomic이 아닌 일반 저장으로 충분. */
		WaitForSingleObject(wd->iothread, INFINITE);
		/* [한국어] 완료 스레드가 exit return할 때까지 블로킹 — 이 시점 이후 wd→iocomplete_event를
		 * 건드리는 코드가 없음을 보장(race-free 해제 전제). INFINITE 대기는 스레드가
		 * 반드시 종료된다는 상위 가정(runnable flag + 250ms 폴) 위에서 안전. */

		CloseHandle(wd->iothread);
		/* [한국어] 스레드 커널 객체의 참조 해제. 스레드 컨텍스트 회수. */
		CloseHandle(wd->iocomplete_event);
		/* [한국어] 완료 이벤트 커널 객체 해제. */

		free(wd->aio_events);
		/* [한국어] init에서 malloc한 완료 이벤트 포인터 배열 해제. */
		free(wd);
		/* [한국어] 엔진 상태 구조체 본체 해제. */

		td->io_ops_data = NULL;
		/* [한국어] 이후 우발적 접근 방지 — dangling 방지. */
	}
}

/*
 * [한국어]
 * windowsaio_invalidate_cache - 지정 파일의 Windows Cache Manager 캐시 엔트리 드롭 유도.
 *
 * @f:      대상 파일.
 * @return: 0=정상 진행(파일 없음 포함 — ERROR_FILE_NOT_FOUND는 정상), 1=실제 오류.
 *
 * 배경: Windows에는 POSIX posix_fadvise(POSIX_FADV_DONTNEED) 직접 대응 API가 없다.
 *      대신 동일 파일을 FILE_FLAG_NO_BUFFERING으로 잠시 열었다가 닫으면 Cache Manager가
 *      해당 파일의 캐시 페이지를 퍼지하는 동작을 보인다. 단, 이 트릭은 열린 핸들이
 *      시스템 내 유일할 때만 효과가 있다 — 다른 프로세스가 해당 파일을 열어두고 있으면
 *      캐시가 공유되어 드롭되지 않는다.
 *
 * 호출 체인: fio_windowsaio_open_file() → (invalidate_cache 옵션) → [이 함수]
 *           → CreateFile(FILE_FLAG_NO_BUFFERING) → CloseHandle.
 * 실행 컨텍스트: 잡 스레드.
 */
static int windowsaio_invalidate_cache(struct fio_file *f)
{
	DWORD error;                  /* [한국어] GetLastError() 임시 저장 — 오류 로그 출력용. */
	DWORD isharemode = (FILE_SHARE_DELETE | FILE_SHARE_READ |
				FILE_SHARE_WRITE);
	/* [한국어] dwShareMode: 우리가 파일을 여는 동안 다른 프로세스에게 허용할 공유 접근 모드.
	 *  - FILE_SHARE_READ  : 다른 프로세스의 READ 열기 허용(우리 열기가 그들의 READ를 막지 않음)
	 *  - FILE_SHARE_WRITE : WRITE 열기 허용
	 *  - FILE_SHARE_DELETE: DELETE/rename 허용
	 * 캐시 무효화 시 파일을 독점하지 않도록 가능한 모든 공유 플래그를 set. */
	HANDLE ihFile;                /* [한국어] 임시 파일 핸들 — 열자마자 닫음. */
	int rc = 0;                   /* [한국어] 반환 코드. 0=성공, 1=실제 오류. */

	/*
	 * Encourage Windows to drop cached parts of a file by temporarily
	 * opening it for non-buffered access. Note: this will only work when
	 * the following is the only thing with the file open on the whole
	 * system.
	 */
	dprint(FD_IO, "windowaio: attempt invalidate cache for %s\n",
			f->file_name);
	/* [한국어] 디버그 트레이스 — --debug=io로 활성화. */
	ihFile = CreateFile(f->file_name, 0, isharemode, NULL, OPEN_EXISTING,
			FILE_FLAG_NO_BUFFERING, NULL);
	/* [한국어] CreateFile:
	 *  - lpFileName            : 파일 경로
	 *  - dwDesiredAccess=0     : 메타데이터만 열기(읽기/쓰기 권한 없음) — 캐시 무효화 목적
	 *  - dwShareMode=isharemode: 공유 플래그
	 *  - lpSecurityAttributes=NULL : 기본
	 *  - dwCreationDisposition=OPEN_EXISTING : 반드시 존재해야 함(없으면 ERROR_FILE_NOT_FOUND)
	 *  - dwFlagsAndAttributes=FILE_FLAG_NO_BUFFERING : Cache Manager 우회 →
	 *      이 모드로 파일을 열었다가 닫으면 캐시 드롭 유도 효과
	 *  - hTemplateFile=NULL    : 템플릿 미사용
	 * 주의: 이 호출은 일반적으로 "열리고 바로 닫힘"만 해도 효과가 충분하다. */

	if (ihFile != INVALID_HANDLE_VALUE) {
		/* [한국어] 파일이 존재하고 성공적으로 열렸다. */
		if (!CloseHandle(ihFile)) {
			/* [한국어] 닫기 실패는 매우 드문 상황(핸들 손상 등). */
			error = GetLastError();
			log_info("windowsaio: invalidation fd close %s failed: error %lu\n",
				 f->file_name, error);
			rc = 1;
		}
	} else {
		/* [한국어] CreateFile 실패 경로 — 파일이 없거나 권한 문제. */
		error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND) {
			/* [한국어] 파일 없음은 "잡이 파일을 곧 생성할 예정"인 정상 케이스이므로 무시.
			 * 다른 오류(접근 거부 등)만 로그 + rc=1. */
			log_info("windowsaio: cache invalidation of %s failed: error %lu\n",
				 f->file_name, error);
			rc = 1;
		}
	}

	return rc;  /* [한국어] 0=OK(파일 없음 포함), 1=실제 오류. */
}

/*
 * [한국어]
 * fio_windowsaio_open_file - 파일을 OVERLAPPED 비동기 I/O 용도로 열고 잡의 IOCP에 바인딩.
 *
 * @td:     잡. td->o.odirect/sync_io/fadvise_hint/create_on_open/invalidate_cache 참고.
 * @f:      열 대상 fio_file. 성공 시 f->hFile에 Win32 HANDLE을 저장.
 * @return: 0=성공, 1=실패(파일 미지원 타입 또는 CreateFile 실패).
 *
 * flag 조합(FILE_FLAG_*):
 *  - FILE_FLAG_POSIX_SEMANTICS: 대소문자 구분 등 POSIX 유사 동작.
 *  - FILE_FLAG_OVERLAPPED:     비동기 I/O(필수). ReadFile/WriteFile이 OVERLAPPED로 pending 가능.
 *  - FILE_FLAG_NO_BUFFERING:   td->o.odirect 시. Cache Manager 우회(섹터 정렬 필요).
 *  - FILE_FLAG_WRITE_THROUGH:  td->o.sync_io 시. 쓰기가 디스크 플래터에 도달할 때까지 기다림.
 *  - FILE_FLAG_RANDOM_ACCESS / SEQUENTIAL_SCAN: fadvise_hint에 따른 Cache Manager 힌트
 *    (NO_BUFFERING과 병용 시 무시됨).
 *
 * 미지원:
 *  - 파이프(FIO_TYPE_PIPE): Windows 파이프에 대한 IOCP 지원은 별도 코드 경로 필요.
 *  - stdin/stdout("-"):    비동기 I/O 대상 부적합.
 *  - TRIM:                 WriteFile/ReadFile에서 직접 지원 없음(queue()에서 거부).
 *
 * 호출 체인: td_io_open_file → ioengine_ops.open_file (이 함수) → CreateFile →
 *           CreateIoCompletionPort(파일을 IOCP에 연결).
 * 실행 컨텍스트: 잡 메인 스레드. 파일마다 1회.
 */
static int fio_windowsaio_open_file(struct thread_data *td, struct fio_file *f)
{
	int rc = 0;  /* [한국어] 반환 코드. */
	DWORD flags = FILE_FLAG_POSIX_SEMANTICS | FILE_FLAG_OVERLAPPED;
	/* [한국어] 기본 플래그: POSIX 시맨틱(대소문자 구분) + OVERLAPPED(비동기 I/O 필수). */
	DWORD sharemode = FILE_SHARE_READ | FILE_SHARE_WRITE;
	/* [한국어] 다른 프로세스의 READ/WRITE 열기 허용 — 잡 병렬 실행 대비. DELETE는 금지(rename 금지). */
	DWORD openmode = OPEN_ALWAYS;
	/* [한국어] 기본: 있으면 열기, 없으면 생성. 아래에서 td->o.create_on_open에 따라 재조정. */
	DWORD access;  /* [한국어] GENERIC_READ | GENERIC_WRITE 등 아래에서 결정. */

	dprint(FD_FILE, "fd open %s\n", f->file_name);
	/* [한국어] 디버그 트레이스 — --debug=file. */

	if (f->filetype == FIO_TYPE_PIPE) {
		/* [한국어] 파이프(프로세스간) 타입은 이 엔진이 지원하지 않음. Linux에서는 가능하나 Windows 파이프는 별도. */
		log_err("windowsaio: pipes are not supported\n");
		return 1;
	}

	if (!strcmp(f->file_name, "-")) {
		/* [한국어] 특수 이름 "-"은 stdin/stdout 의미 — 비동기 디스크 I/O 대상 아님. */
		log_err("windowsaio: can't read/write to stdin/out\n");
		return 1;
	}

	if (td->o.odirect)
		flags |= FILE_FLAG_NO_BUFFERING;
		/* [한국어] direct I/O: Cache Manager 우회. 버퍼/오프셋/길이가 디스크 섹터에 정렬되어야 함
		 * (보통 512/4096B). 미정렬 시 ERROR_INVALID_PARAMETER. */
	if (td->o.sync_io)
		flags |= FILE_FLAG_WRITE_THROUGH;
		/* [한국어] 쓰기 완료 = 물리 미디어 도달. 캐시된 비동기 쓰기가 드라이브 큐에서 즉시 커밋. */

	/*
	 * Inform Windows whether we're going to be doing sequential or
	 * random IO so it can tune the Cache Manager
	 */
	switch (td->o.fadvise_hint) {
	case F_ADV_TYPE:
		/* [한국어] "잡 타입에서 자동 추정": random이면 RANDOM_ACCESS, 아니면 SEQUENTIAL_SCAN. */
		if (td_random(td))
			flags |= FILE_FLAG_RANDOM_ACCESS;
			/* [한국어] 무작위 접근 — Cache Manager 프리페치 억제. */
		else
			flags |= FILE_FLAG_SEQUENTIAL_SCAN;
			/* [한국어] 순차 접근 — 공격적 read-ahead 활성화. */
		break;
	case F_ADV_RANDOM:
		flags |= FILE_FLAG_RANDOM_ACCESS;
		/* [한국어] 사용자가 명시적으로 random 힌트 지정. */
		break;
	case F_ADV_SEQUENTIAL:
		flags |= FILE_FLAG_SEQUENTIAL_SCAN;
		/* [한국어] 명시적 sequential 힌트. */
		break;
	case F_ADV_NONE:
		break;
		/* [한국어] 힌트 없음 — Cache Manager 기본 휴리스틱 사용. */
	default:
		log_err("fio: unknown fadvise type %d\n", td->o.fadvise_hint);
		/* [한국어] 알 수 없는 값 — 옵션 파서 버그 또는 신규 값. 경고만. */
	}

	if ((!td_write(td) && !(td->flags & TD_F_SYNCS)) || read_only)
		access = GENERIC_READ;
		/* [한국어] 읽기만 하는 잡 & fsync류도 없음, 또는 전역 read_only → READ 권한만.
		 * 불필요한 WRITE 요청을 피해 ro 파일시스템/디바이스에서도 열 수 있게 함. */
	else
		access = (GENERIC_READ | GENERIC_WRITE);
		/* [한국어] 쓰기 또는 fsync가 필요한 잡 → READ+WRITE. 일부 OS는 WRITE만은 허용하지 않음. */

	if (td->o.create_on_open)
		openmode = OPEN_ALWAYS;		/* [한국어] 없으면 생성, 있으면 열기(기본값과 동일). */
	else
		openmode = OPEN_EXISTING;	/* [한국어] 파일이 반드시 존재해야 함 — 잡 시작 전에 미리 생성 필요. */

	/* If we're going to use direct I/O, Windows will try and invalidate
	 * its cache at that point so there's no need to do it here */
	if (td->o.invalidate_cache && !td->o.odirect)
		windowsaio_invalidate_cache(f);
		/* [한국어] invalidate_cache 옵션 + 버퍼링 I/O → NO_BUFFERING 열기 트릭으로 캐시 드롭 유도. */

	f->hFile = CreateFile(f->file_name, access, sharemode,
		NULL, openmode, flags, NULL);
	/* [한국어] 실제 파일 HANDLE 생성. 위에서 조립한 access/sharemode/openmode/flags 조합으로 호출.
	 * fio_file::hFile은 Windows 빌드 전용 필드(POSIX 빌드에선 fd int). */

	if (f->hFile == INVALID_HANDLE_VALUE) {
		/* [한국어] 열기 실패 — 파일 없음/권한/잘못된 이름/direct I/O 정렬 실패 등. */
		log_err("windowsaio: failed to open file \"%s\"\n", f->file_name);
		rc = 1;
	}

	/* Only set up the completion port and thread if we're not just
	 * querying the device size */
	if (!rc && td->io_ops_data != NULL) {
		/* [한국어] 실제 I/O를 수행할 잡에서만 IOCP 바인딩(get_file_size 단독 호출 경로 제외). */
		struct windowsaio_data *wd;

		wd = td->io_ops_data;

		if (CreateIoCompletionPort(f->hFile, wd->iocp, 0, 0) == NULL) {
			/* [한국어] CreateIoCompletionPort (두 번째 인자에 기존 IOCP 전달 → 파일 핸들을 그 포트에 바인딩):
			 *  - FileHandle=f->hFile               : 바인딩할 파일
			 *  - ExistingCompletionPort=wd->iocp   : 이 파일의 완료를 받을 IOCP
			 *  - CompletionKey=0                   : per-file key 미사용(CONTAINING_RECORD로 복원)
			 *  - NumberOfConcurrentThreads=0       : (이미 지정됨, 무시)
			 * 바인딩 후 이 파일의 OVERLAPPED I/O 완료는 모두 wd->iocp로 큐잉된다. */
			log_err("windowsaio: failed to create io completion port\n");
			rc = 1;
		}
	}

	return rc;  /* [한국어] 0=성공, 1=실패. */
}

/*
 * [한국어]
 * fio_windowsaio_close_file - open_file로 연 파일 핸들 닫기.
 *
 * @td: 잡(미사용 — fio_unused 어노테이션).
 * @f:  닫을 파일. f->hFile 사용 후 INVALID_HANDLE_VALUE로 리셋.
 * @return: 0=성공, 1=CloseHandle 실패.
 *
 * 주의: IOCP 바인딩은 파일 핸들이 close되면 커널이 자동으로 해제하므로 별도 해제 API 호출 불필요.
 *
 * 호출 체인: td_io_close_file → ioengine_ops.close_file (이 함수) → CloseHandle.
 * 실행 컨텍스트: 잡 메인 스레드. 파일마다 1회.
 */
static int fio_windowsaio_close_file(struct thread_data fio_unused *td, struct fio_file *f)
{
	int rc = 0;  /* [한국어] 반환 코드. */

	dprint(FD_FILE, "fd close %s\n", f->file_name);
	/* [한국어] 디버그 트레이스. */

	if (f->hFile != INVALID_HANDLE_VALUE) {
		/* [한국어] 유효 핸들인 경우에만 close. open 실패로 INVALID인 경우 no-op. */
		if (!CloseHandle(f->hFile)) {
			/* [한국어] CloseHandle 실패 — 이미 닫혔거나 손상된 핸들. */
			log_info("windowsaio: failed to close file handle for \"%s\"\n", f->file_name);
			rc = 1;
		}
	}

	f->hFile = INVALID_HANDLE_VALUE;
	/* [한국어] 핸들 필드를 무효값으로 리셋 — 이후 재사용/더블클로즈 방지. */
	return rc;
}

/*
 * [한국어]
 * timeout_expired - GetTickCount 기반 wraparound-safe 타임아웃 판정.
 *
 * @start_count: 타이머 시작 시점의 GetTickCount() 값.
 * @end_count:   만료 시점의 GetTickCount() 값(= start + duration ms).
 * @return:      TRUE=만료, FALSE=아직.
 *
 * 배경: GetTickCount()는 32-bit DWORD 밀리초 카운터로, 약 49.7일마다 0으로 wrap한다.
 *      (end > start)인 "정상" 경우와, 시작 중 wrap이 일어나 (start > end)인 경우를
 *      모두 안전하게 처리해야 한다.
 *
 * 호출 체인: fio_windowsaio_getevents_nothread / getevents_thread 루프.
 * 실행 컨텍스트: 잡 스레드.
 */
static BOOL timeout_expired(DWORD start_count, DWORD end_count)
{
	BOOL expired = FALSE;
	/* [한국어] 기본 "아직 안 지남". */
	DWORD current_time;

	current_time = GetTickCount();
	/* [한국어] 시스템 부팅 이후 경과 ms(32-bit). 모노토닉이지만 wrap 존재. */

	if ((end_count > start_count) && current_time >= end_count)
		expired = TRUE;
		/* [한국어] 정상 경우: start < end이고, 현재가 end를 넘었다. */
	else if (current_time < start_count && current_time > end_count)
		expired = TRUE;
		/* [한국어] wrap 경우: start 시점 이후 wrap → 현재 < start이면서 end도 넘겼다.
		 * 즉 타이머 구간이 0 경계를 넘나든 상태에서 현재 시각이 end까지 도달. */

	return expired;
}

/*
 * [한국어]
 * fio_windowsaio_event - ioengine_ops.event 진입점. event 인덱스에 대응하는 io_u 반환.
 *
 * @td:    잡.
 * @event: 0..(getevents 반환값 - 1). 코어가 getevents가 알려준 수만큼 이 함수를 순차 호출.
 * @return: wd->aio_events[event] — 완료된 io_u.
 *
 * 호출 체인: td_io_getevents() → ioengine_ops.event (이 함수) → 배열 인덱싱.
 * 실행 컨텍스트: 잡 스레드.
 */
static struct io_u* fio_windowsaio_event(struct thread_data *td, int event)
{
	struct windowsaio_data *wd = td->io_ops_data;
	/* [한국어] 잡별 상태에서 완료 배열 접근. */
	return wd->aio_events[event];
	/* [한국어] 단순 인덱싱 — getevents가 슬롯 순서대로 채워 놓았음. */
}

/* dequeue completion entrees directly (no separate completion thread) */
/*
 * [한국어]
 * fio_windowsaio_getevents_nothread - no_completion_thread=1 모드의 getevents.
 *
 * @td:  잡.
 * @min: fio 코어가 요구하는 최소 완료 개수(이 값 이상 수거할 때까지 대기).
 * @max: 이번 호출에서 수거할 최대 개수.
 * @t:   타임아웃(NULL이면 무한 — 단, GQCS-Ex의 mswait 인자로 250ms 청크 반복).
 * @return: 실제 수거한 이벤트 수(<= max).
 *
 * 동작:
 *   - GetQueuedCompletionStatusEx(iocp, &oe, 16, &entries, mswait, alertable=FALSE)로
 *     한 번에 최대 16개 완료 패킷(OVERLAPPED_ENTRY) 배치 수거.
 *   - 각 엔트리의 lpOverlapped → CONTAINING_RECORD로 fio_overlapped 복원 → io_u 획득.
 *   - OVERLAPPED.Internal(NTSTATUS)에서 성공/에러 판별, InternalHigh에서 전송 바이트.
 *   - dequeued >= min 또는 타임아웃 만료 시 루프 종료.
 *
 * 호출 체인: fio_windowsaio_getevents (분기) → [이 함수] → GetQueuedCompletionStatusEx.
 * 실행 컨텍스트: 잡 스레드.
 */
static int fio_windowsaio_getevents_nothread(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct windowsaio_data *wd = td->io_ops_data;  /* [한국어] 상태/IOCP/aio_events 접근. */
	unsigned int dequeued = 0;                      /* [한국어] 누적 수거 개수. */
	struct io_u *io_u;                              /* [한국어] 복원된 io_u 임시. */
	DWORD start_count = 0;                          /* [한국어] 타임아웃 기준 시작 틱. */
	DWORD end_count = 0;                            /* [한국어] 만료 시점 틱. */
	DWORD mswait = 250;		/* [한국어] GQCS-Ex 단일 호출의 대기 한도(ms). 기본 250ms. */
	struct fio_overlapped *fov;                      /* [한국어] OVERLAPPED → fio_overlapped 복원 결과. */

	if (t != NULL) {
		/* [한국어] 코어가 타임아웃을 지정 → ms 환산 + wrap-safe 만료 시각 계산. */
		mswait = (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
		start_count = GetTickCount();
		end_count = start_count + (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
	}

	do {
		BOOL ret;                    /* [한국어] GQCS-Ex 반환값. */
		OVERLAPPED *ovl;             /* [한국어] 각 엔트리에서 추출할 OVERLAPPED 포인터. */

		ULONG entries = min(16, max-dequeued);
		/* [한국어] 이번 호출에서 받을 최대 엔트리 수 — 요청 남은 개수와 16 중 작은 값.
		 * (주의: min()은 fio의 매크로/로컬 유틸) */
		OVERLAPPED_ENTRY oe[16];
		/* [한국어] GQCS-Ex 출력 배열. 각 엔트리: {lpCompletionKey, lpOverlapped,
		 *  Internal, dwNumberOfBytesTransferred}. 크기 16은 배치 수거 튜닝 상수. */
		ret = GetQueuedCompletionStatusEx(wd->iocp, oe, 16, &entries, mswait, 0);
		/* [한국어] GetQueuedCompletionStatusEx:
		 *  - CompletionPort=wd->iocp : 수거할 포트
		 *  - lpCompletionPortEntries=oe : OUT 엔트리 배열
		 *  - ulCount=16             : 배열 용량
		 *  - ulNumEntriesRemoved=&entries : IN/OUT (초기값 무시, OUT으로 실제 수거 수)
		 *  - dwMilliseconds=mswait  : 대기 한도(ms)
		 *  - fAlertable=0 (FALSE)   : APC로 깨지 않음(정상 완료만)
		 * 성공 시 ret=TRUE, 타임아웃 시 ret=FALSE & GetLastError()=WAIT_TIMEOUT. */
		if (ret && entries) {
			/* [한국어] 적어도 1개 이상 수거된 경우에만 분배. */
			int entry_num;

			for (entry_num=0; entry_num<entries; entry_num++) {
				ovl = oe[entry_num].lpOverlapped;
				/* [한국어] 엔트리에서 OVERLAPPED 포인터 추출. */
				fov = CONTAINING_RECORD(ovl, struct fio_overlapped, o);
				/* [한국어] OVERLAPPED 필드 주소 → 소속 fio_overlapped 시작 주소 역추적.
				 * 매크로 구현: (char*)ovl - offsetof(fio_overlapped, o). */
				io_u = fov->io_u;
				/* [한국어] 역링크로 io_u 복원. */

				if (ovl->Internal == ERROR_SUCCESS) {
					/* [한국어] NTSTATUS가 STATUS_SUCCESS(0). 성공 경로. */
					io_u->resid = io_u->xfer_buflen - ovl->InternalHigh;
					/* [한국어] 요청 바이트 - 실제 전송 바이트 = 잔여 바이트.
					 * 부분 전송(short read) 시 0보다 큼. */
					io_u->error = 0;
				} else {
					/* [한국어] 에러 경로. */
					io_u->resid = io_u->xfer_buflen;
					/* [한국어] 모두 미전송으로 간주 — 방어적. */
					io_u->error = win_to_posix_error(GetLastError());
					/* [한국어] Win32 에러 → POSIX errno 변환(os/windows/* 구현). */
				}

				fov->io_complete = FALSE;
				/* [한국어] (nothread 모드에선 무의미하지만) 혹시라도 thread 모드가 혼용될 때
				 * 이중 수확 방지를 위해 리셋. */
				wd->aio_events[dequeued] = io_u;
				/* [한국어] 코어에 보고할 완료 배열에 저장. */
				dequeued++;
			}
		}

		if (dequeued >= min ||
			(t != NULL && timeout_expired(start_count, end_count)))
			break;
			/* [한국어] min 달성 또는 타임아웃 — 루프 종료. */
	} while (1);
	return dequeued;  /* [한국어] 실제 수거 개수 반환. */
}

/* dequeue completion entrees creates by separate IoCompletionRoutine thread */
/*
 * [한국어]
 * fio_windowaio_getevents_thread - 완료 스레드 모드(기본)의 getevents.
 *
 * 완료 스레드가 이미 각 in-flight io_u에 대해 fov->io_complete=TRUE를 설정해 두었으므로
 * 여기서는 td->io_u_all을 순회하며 플래그가 켜진 것을 수확하기만 하면 된다.
 * 아직 부족하면 WaitForSingleObject(iocomplete_event, 250ms)로 대기 + 재스캔.
 *
 * @td:  잡.
 * @min: 최소 수거 개수.
 * @max: 최대 수거 개수(이 함수에서는 사용 안 함 — io_u_qiter 순회 자연 상한이 iodepth).
 * @t:   타임아웃(NULL이면 무제한).
 * @return: 실제 수거 개수.
 *
 * 호출 체인: fio_windowsaio_getevents (분기) → [이 함수] → io_u_qiter + WaitForSingleObject.
 * 실행 컨텍스트: 잡 스레드. 완료 스레드는 병렬로 io_complete 플래그를 set 중.
 */
static int fio_windowaio_getevents_thread(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct windowsaio_data *wd = td->io_ops_data;  /* [한국어] aio_events 저장 위치. */
	unsigned int dequeued = 0;                      /* [한국어] 수거 누적. */
	struct io_u *io_u;                              /* [한국어] 순회 중인 io_u 포인터. */
	int i;                                          /* [한국어] io_u_qiter 인덱스. */
	struct fio_overlapped *fov;                     /* [한국어] io_u->engine_data 캐스팅. */
	DWORD start_count = 0;                          /* [한국어] 타임아웃 시작 틱. */
	DWORD end_count = 0;                            /* [한국어] 만료 시점 틱. */
	DWORD status;                                   /* [한국어] WaitForSingleObject 반환값. */
	DWORD mswait = 250;                             /* [한국어] Wait 단일 호출 대기 한도. */

	if (t != NULL) {
		/* [한국어] 타임아웃 스펙 변환: timespec → ms. */
		mswait = (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
		start_count = GetTickCount();
		end_count = start_count + (t->tv_sec * 1000) + (t->tv_nsec / 1000000);
	}

	do {
		io_u_qiter(&td->io_u_all, io_u, i) {
			/* [한국어] 잡의 전체 io_u 배열 순회(free+in-flight 포함). */
			if (!(io_u->flags & IO_U_F_FLIGHT))
				continue;
				/* [한국어] in-flight 아닌 것(free list에 있는 것)은 스킵. */

			fov = (struct fio_overlapped*)io_u->engine_data;
			/* [한국어] io_u_init에서 연결된 fio_overlapped 획득. */

			if (fov->io_complete) {
				/* [한국어] 완료 스레드가 "완료 도착"으로 표시한 io_u. */
				fov->io_complete = FALSE;	/* [한국어] 동일 io_u 재수확 방지. */
				wd->aio_events[dequeued] = io_u;
				dequeued++;
			}
		}
		if (dequeued >= min)
			break;
			/* [한국어] 요구치 달성 — 즉시 반환(대기 없이). */

		if (dequeued < min) {
			status = WaitForSingleObject(wd->iocomplete_event, mswait);
			/* [한국어] 완료 스레드가 SetEvent(iocomplete_event) 할 때까지 또는 mswait ms 경과까지 블로킹.
			 * 반환:
			 *  - WAIT_OBJECT_0 (0) : 이벤트 signaled — 새 완료 도착 가능성
			 *  - WAIT_TIMEOUT      : 시간 경과
			 *  - WAIT_FAILED       : 에러
			 * auto-reset 이벤트이므로 Wait 해제와 동시에 non-signaled로 복귀 —
			 * 다음 Wait는 새 SetEvent를 기다리게 된다. */
			if (status != WAIT_OBJECT_0 && dequeued >= min)
				break;
				/* [한국어] 타임아웃이면서 이미 min 달성한 경우 종료 —
				 * 방어적 조건(위의 `dequeued < min` 진입 조건상 이 분기는 사실상 도달 불가). */
		}

		if (dequeued >= min ||
		    (t != NULL && timeout_expired(start_count, end_count)))
			break;
			/* [한국어] 요구치 달성 또는 전체 타임아웃 만료. */
	} while (1);

	return dequeued;
}

/*
 * [한국어]
 * fio_windowsaio_getevents - ioengine_ops.getevents 진입점. 옵션에 따라 두 경로 분기.
 *
 * @td:   잡. td->eo에서 no_completion_thread 플래그 조회.
 * @min:  최소 수거 수.
 * @max:  최대 수거 수.
 * @t:    타임아웃(NULL 가능).
 * @return: 실제 수거 수.
 *
 * 분기:
 *   no_completion_thread=1 → fio_windowsaio_getevents_nothread
 *   기본                   → fio_windowaio_getevents_thread
 *
 * 호출 체인: td_io_getevents() → ioengine_ops.getevents (이 함수) → 서브루틴.
 * 실행 컨텍스트: 잡 스레드.
 */
static int fio_windowsaio_getevents(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct windowsaio_options *o = td->eo;  /* [한국어] 엔진 옵션 접근. */

	if (o->no_completion_thread)
		return fio_windowsaio_getevents_nothread(td, min, max, t);
		/* [한국어] 잡 스레드가 직접 GQCS-Ex로 배치 수거. */
	return fio_windowaio_getevents_thread(td, min, max, t);
	/* [한국어] 기본: 완료 스레드가 세팅한 io_complete 플래그 수확. */
}

/*
 * [한국어]
 * fio_windowsaio_queue - ioengine_ops.queue 진입점. io_u 1개를 Windows AIO로 제출.
 *
 * @td:   잡.
 * @io_u: 제출할 I/O 유닛(xfer_buf/xfer_buflen/offset/ddir/file 등 세팅됨).
 * @return:
 *   FIO_Q_COMPLETED — DDIR_SYNC/DATASYNC(동기 플러시), DDIR_TRIM(미지원 에러),
 *                     또는 ReadFile/WriteFile 자체 실패(즉시 에러 결정).
 *   FIO_Q_QUEUED    — ReadFile/WriteFile이 동기 성공했거나 ERROR_IO_PENDING 반환(둘 다 "정상 접수").
 *                     실제 완료는 IOCP를 통해 추후 수확.
 *
 * 상세:
 *   - OVERLAPPED.Offset/OffsetHigh에 64비트 offset을 하위/상위 32비트로 분할 저장.
 *   - WriteFile/ReadFile의 lpNumberOfBytesWritten/Read=NULL: 비동기 모드에서는
 *     완료 후 OVERLAPPED.InternalHigh로 전송 바이트를 받는다.
 *   - FlushFileBuffers는 OVERLAPPED 버전이 없어 동기 호출(해당 잡 스레드 블로킹).
 *   - DDIR_TRIM(수동 디스카드)은 Win32가 직접 지원하지 않음 → 즉시 에러 반환.
 *
 * 호출 체인: backend.c:do_io → td_io_queue() → ioengine_ops.queue (이 함수)
 *           → ReadFile/WriteFile/FlushFileBuffers (kernel32).
 * 실행 컨텍스트: 잡 스레드.
 */
static enum fio_q_status fio_windowsaio_queue(struct thread_data *td,
					      struct io_u *io_u)
{
	struct fio_overlapped *o = io_u->engine_data;
	/* [한국어] io_u_init에서 연결된 fio_overlapped 획득. */
	LPOVERLAPPED lpOvl = &o->o;
	/* [한국어] OVERLAPPED 포인터 — Win32 API에 전달될 것. */
	BOOL success = FALSE;
	/* [한국어] ReadFile/WriteFile 동기 성공 여부 임시. */
	int rc = FIO_Q_COMPLETED;
	/* [한국어] 기본 반환값. 동기 완료/에러 경로에선 이 값 그대로, 비동기 수락 시 QUEUED로 전환. */

	fio_ro_check(td, io_u);
	/* [한국어] 읽기전용 잡에 WRITE가 오지 않았는지 방어 체크(assert 매크로). */

	lpOvl->Internal = 0;			/* [한국어] NT 상태코드(NTSTATUS) 슬롯 초기화 — 커널이 완료 시 덮어씀. */
	lpOvl->InternalHigh = 0;		/* [한국어] 전송 바이트 수 슬롯 초기화. */
	lpOvl->Offset = io_u->offset & 0xFFFFFFFF;	/* [한국어] 64비트 파일 오프셋의 하위 32비트. */
	lpOvl->OffsetHigh = io_u->offset >> 32;		/* [한국어] 상위 32비트(합해 파일 내 절대 위치). */

	switch (io_u->ddir) {
	case DDIR_WRITE:
		success = WriteFile(io_u->file->hFile, io_u->xfer_buf,
					io_u->xfer_buflen, NULL, lpOvl);
		/* [한국어] WriteFile(OVERLAPPED 비동기):
		 *  - hFile            : FILE_FLAG_OVERLAPPED로 연 파일 핸들
		 *  - lpBuffer         : 사용자 버퍼
		 *  - nNumberOfBytesToWrite : 요청 바이트
		 *  - lpNumberOfBytesWritten=NULL : 비동기 모드 — 완료 시 OVERLAPPED.InternalHigh로 수신
		 *  - lpOverlapped     : Offset/OffsetHigh 세팅된 OVERLAPPED
		 * 반환 TRUE  : 동기 완료(데이터가 이미 커널 버퍼에 전달됨)
		 * 반환 FALSE & GetLastError()==ERROR_IO_PENDING : 비동기 pending
		 * 반환 FALSE & 다른 에러 : 즉시 실패 */
		break;
	case DDIR_READ:
		success = ReadFile(io_u->file->hFile, io_u->xfer_buf,
					io_u->xfer_buflen, NULL, lpOvl);
		/* [한국어] ReadFile — 동작 계약은 WriteFile과 동일(방향 반대). */
		break;
	case DDIR_SYNC:
	case DDIR_DATASYNC:
	case DDIR_SYNC_FILE_RANGE:
		success = FlushFileBuffers(io_u->file->hFile);
		/* [한국어] FlushFileBuffers — 파일의 모든 pending write를 디스크까지 flush.
		 * Windows에는 OVERLAPPED 비동기 버전이 없어 동기 호출(잡 스레드 블로킹).
		 * POSIX fsync/fdatasync/sync_file_range에 해당하는 3가지 ddir 모두 동일 처리.
		 * 반환 TRUE=성공, FALSE=실패. */
		if (!success) {
			/* [한국어] 플러시 실패는 디바이스 오류 등 심각 — 에러 기록. */
			log_err("windowsaio: failed to flush file buffers\n");
			io_u->error = win_to_posix_error(GetLastError());
		}

		return FIO_Q_COMPLETED;
		/* [한국어] 동기 완료 — fio 코어가 commit/getevents를 건너뛰고 put_io_u 수행. */
	case DDIR_TRIM:
		log_err("windowsaio: manual TRIM isn't supported on Windows\n");
		/* [한국어] Windows는 블록 레벨 DISCARD를 개별 I/O로 노출하지 않음
		 * (DeviceIoControl/FSCTL_FILE_LEVEL_TRIM 등은 있으나 이 엔진에서 미구현). */
		io_u->error = 1;                         /* [한국어] 에러 표시. */
		io_u->resid = io_u->xfer_buflen;        /* [한국어] 잔여=전체 요청(아무것도 안 함). */
		return FIO_Q_COMPLETED;                  /* [한국어] 즉시 완료(에러)로 보고. */
	default:
		assert(0);
		/* [한국어] 알 수 없는 ddir — 코어 버그. 디버그 빌드에서 abort. */
		break;
	}

	if (success || GetLastError() == ERROR_IO_PENDING)
		rc = FIO_Q_QUEUED;
		/* [한국어] 동기 완료(success=TRUE)든 비동기 pending(ERROR_IO_PENDING)이든 "정상 접수".
		 * Windows는 FILE_FLAG_OVERLAPPED에서 동기 완료여도 IOCP에 완료 패킷이 큐잉되므로
		 * (기본 동작; SetFileCompletionNotificationModes로 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
		 * 을 설정하지 않은 한) 완료 수거는 동일 경로에서 이뤄진다. 따라서 fio에는 QUEUED로 보고. */
	else {
		/* [한국어] 즉시 실패 경로 — 파라미터 오류, 핸들 무효, 권한 부족 등. */
		io_u->error = win_to_posix_error(GetLastError());
		io_u->resid = io_u->xfer_buflen;
	}

	return rc;  /* [한국어] FIO_Q_QUEUED(정상) 또는 FIO_Q_COMPLETED(에러로 즉시 결정). */
}

/* Runs as a thread and waits for queued IO to complete */
/*
 * [한국어]
 * IoCompletionRoutine - 완료 처리 스레드 엔트리 포인트. 잡당 1개 생성(기본 모드).
 *
 * @lpParameter: CreateThread에 전달한 struct thread_ctx * (iocp, wd 포함).
 * @return: 0 — 스레드 종료 코드(fio에서 사용되지 않음).
 *
 * 동작 루프:
 *   1) GetQueuedCompletionStatus(ctx->iocp, ..., 250ms)로 완료 패킷 1개 수거.
 *   2) 타임아웃(ret=FALSE, ovl=NULL)이면 재시도 — iothread_running 체크용 주기적 깨어남.
 *   3) 완료 패킷이 있으면 OVERLAPPED → fio_overlapped → io_u 역참조.
 *   4) NTSTATUS(Internal)과 전송 바이트(InternalHigh)로 io_u->error/resid 환산.
 *   5) fov->io_complete = TRUE; SetEvent(wd->iocomplete_event)로 잡 스레드 깨움.
 *   6) iothread_running=TRUE인 동안 반복. FALSE가 되면 루프 탈출.
 * 종료 후:
 *   - CloseHandle(ctx->iocp): IOCP 핸들 소유권 이전받아 여기서 close.
 *   - free(ctx): 컨텍스트 해제.
 *
 * 실행 컨텍스트: fio_windowsaio_init의 CreateThread로 생성된 별도 Windows 스레드.
 *                 잡 스레드와 독립적으로 병렬 실행. 잡 스레드 = queue/getevents/event 호출자.
 */
static DWORD WINAPI IoCompletionRoutine(LPVOID lpParameter)
{
	OVERLAPPED *ovl;                   /* [한국어] GQCS가 반환하는 OVERLAPPED 포인터. */
	struct fio_overlapped *fov;        /* [한국어] CONTAINING_RECORD로 복원한 엔진 구조체. */
	struct io_u *io_u;                 /* [한국어] 최종 복원된 io_u. */
	struct windowsaio_data *wd;        /* [한국어] ctx->wd 복사본 — SetEvent 대상 접근용. */
	struct thread_ctx *ctx;            /* [한국어] CreateThread 파라미터 캐스팅 결과. */
	ULONG_PTR ulKey = 0;               /* [한국어] GQCS의 completion key(이 엔진은 미사용, 0). */
	DWORD bytes;                       /* [한국어] GQCS가 전송 바이트를 돌려주는 out 인자(미사용, InternalHigh 사용). */

	ctx = (struct thread_ctx*)lpParameter;
	/* [한국어] LPVOID(void*)을 본 타입으로 복원. */
	wd = ctx->wd;
	/* [한국어] 엔진 상태 포인터 저장. */

	do {
		BOOL ret;

		ret = GetQueuedCompletionStatus(ctx->iocp, &bytes, &ulKey,
						&ovl, 250);
		/* [한국어] GetQueuedCompletionStatus:
		 *  - CompletionPort=ctx->iocp
		 *  - lpNumberOfBytesTransferred=&bytes : OUT (전송 바이트; OVERLAPPED.InternalHigh와 동일 값)
		 *  - lpCompletionKey=&ulKey           : OUT (per-file key; 이 엔진은 0 고정이라 무시)
		 *  - lpOverlapped=&ovl                : OUT (완료된 I/O의 OVERLAPPED)
		 *  - dwMilliseconds=250               : 250ms 타임아웃 — iothread_running 체크 주기
		 * 반환값 조합:
		 *  - ret=TRUE, ovl!=NULL  : 성공한 I/O 완료
		 *  - ret=FALSE, ovl!=NULL : 실패한 I/O 완료(에러코드 조회 가능)
		 *  - ret=FALSE, ovl=NULL  : 타임아웃 또는 IOCP 자체 오류 */
		if (!ret && ovl == NULL)
			continue;
			/* [한국어] 타임아웃 — do/while 조건(iothread_running) 재확인 위해 루프 상단으로. */

		fov = CONTAINING_RECORD(ovl, struct fio_overlapped, o);
		/* [한국어] OVERLAPPED 필드 시작 주소 → 소속 fio_overlapped 시작 주소 역계산. */
		io_u = fov->io_u;
		/* [한국어] 역링크로 io_u 획득. */

		if (ovl->Internal == ERROR_SUCCESS) {
			/* [한국어] 성공 — 실제 전송 바이트로 resid 계산. */
			io_u->resid = io_u->xfer_buflen - ovl->InternalHigh;
			io_u->error = 0;
		} else {
			/* [한국어] 에러 — resid=전체(보수적), errno 변환 후 저장. */
			io_u->resid = io_u->xfer_buflen;
			io_u->error = win_to_posix_error(GetLastError());
			/* [한국어] 주의: GetLastError()는 스레드 로컬이라 이 완료 스레드의 직전 호출
			 * 컨텍스트에 따라 부정확할 수 있음. 더 정확한 변환은 RtlNtStatusToDosError(ovl->Internal)
			 * 이겠으나 이 엔진은 간이 변환 사용. */
		}

		fov->io_complete = TRUE;	/* [한국어] 잡 스레드에게 "이 io_u 완료 도착" 신호(flag set). */
		SetEvent(wd->iocomplete_event);	/* [한국어] WaitForSingleObject 대기 중인 잡 스레드 깨움. */
	} while (ctx->wd->iothread_running);
	/* [한국어] 메인 루프 조건 — cleanup에서 FALSE로 바뀌면 탈출. */

	CloseHandle(ctx->iocp);
	/* [한국어] IOCP 커널 객체 회수 — init에서 생성된 포트의 수명은 완료 스레드와 함께 끝남. */
	free(ctx);
	/* [한국어] 컨텍스트 구조체 해제 — init에서 malloc한 메모리. */
	return 0;  /* [한국어] 스레드 종료 코드. fio는 GetExitCodeThread를 호출하지 않으므로 값 무관. */
}

/*
 * [한국어]
 * fio_windowsaio_io_u_free - ioengine_ops.io_u_free 진입점. io_u 해제 시 engine_data 회수.
 *
 * @td:   잡(미사용).
 * @io_u: 해제될 io_u. engine_data가 NULL이 아니면 free.
 *
 * 호출 체인: io_u.c:free_io_u → ioengine_ops.io_u_free (이 함수).
 * 실행 컨텍스트: 잡 스레드. io_u 1개당 1회(잡 cleanup 시 모든 io_u에 대해).
 */
static void fio_windowsaio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct fio_overlapped *o = io_u->engine_data;  /* [한국어] init에서 연결된 포인터. */

	if (o) {
		/* [한국어] NULL 방어 — io_u_init 실패 이력이 있는 경우. */
		io_u->engine_data = NULL;  /* [한국어] 먼저 dangling 방지로 clear. */
		free(o);                    /* [한국어] fio_overlapped 본체 해제(OVERLAPPED 포함). */
	}
}

/*
 * [한국어]
 * fio_windowsaio_io_u_init - ioengine_ops.io_u_init 진입점. io_u 생성 시 fio_overlapped 할당.
 *
 * @td:   잡(미사용).
 * @io_u: 방금 할당된 io_u. engine_data에 fio_overlapped를 연결한다.
 * @return: 0=성공(현 구현은 항상 0). malloc 실패는 NULL 역참조로 crash 가능 — fio 관행.
 *
 * 필드 초기화:
 *   - io_complete = FALSE (완료 아님)
 *   - io_u 역링크 저장
 *   - OVERLAPPED.hEvent = NULL (IOCP 사용 시 이벤트 기반 알림 비활성화)
 *     * hEvent가 NULL이 아니면 각 I/O 완료 시 그 이벤트도 signal되어 이중 알림 발생 —
 *       IOCP만 쓰는 본 엔진에는 불필요.
 *
 * 호출 체인: io_u.c:init_io_u → ioengine_ops.io_u_init (이 함수).
 * 실행 컨텍스트: 잡 스레드. io_u 1개당 1회(잡 시작 시 iodepth개만큼).
 */
static int fio_windowsaio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct fio_overlapped *o;

	o = malloc(sizeof(*o));
	/* [한국어] io_u 전용 OVERLAPPED+역링크+플래그 할당. malloc 실패 시 이후 NULL 역참조. */
	o->io_complete = FALSE;          /* [한국어] 아직 완료 아님. */
	o->io_u = io_u;                   /* [한국어] 완료 수거 시 io_u 복원을 위한 역링크. */
	o->o.hEvent = NULL;               /* [한국어] OVERLAPPED.hEvent=NULL — IOCP만으로 완료 알림. */
	io_u->engine_data = o;            /* [한국어] io_u → fio_overlapped 연결. */
	return 0;                          /* [한국어] 성공. */
}

/* [한국어] ioengine_ops: fio 코어가 이 엔진을 디스패치하기 위한 vtable.
 * register_ioengine()에 의해 전역 엔진 리스트에 링크되며, load_ioengine("windowsaio")로 매칭. */
static struct ioengine_ops ioengine = {
	.name		= "windowsaio",
	/* [한국어] 엔진 식별 문자열. 잡파일 `ioengine=windowsaio` 또는 `--ioengine=windowsaio`와 매칭.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:load_ioengine()의 strcmp.
	 * 값 범위: NUL 종결 ASCII, 고유해야 함(전역 엔진 리스트 내).
	 * 동기화: 등록 후 불변, 락 불필요. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전 상수. fio 코어가 자신의 FIO_IOOPS_VERSION과 비교해
	 * 불일치 시 로드 거부(엔진 구조체 레이아웃 불일치에 의한 crash 방지).
	 * 설정자: 이 초기화(fio.h의 최신 매크로). 읽는 자: register_ioengine().
	 * 값 범위: fio.h 정의 정수. 동기화: 불변. */

	.init		= fio_windowsaio_init,
	/* [한국어] 잡 시작 1회 호출. windowsaio_data 할당, IOCP/이벤트/완료 스레드 생성.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:td_io_init().
	 * 반환: 0=성공, !=0=실패(잡 중단).
	 * 동기화: 잡 스레드 단독, 1회만 호출. */

	.queue		= fio_windowsaio_queue,
	/* [한국어] io_u 1개 제출. ReadFile/WriteFile/FlushFileBuffers 발행.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:td_io_queue().
	 * 반환: FIO_Q_QUEUED(비동기 접수)/FIO_Q_COMPLETED(동기 완료 or 에러 결정).
	 * 동기화: 잡 스레드 단독, 순차 호출(동일 잡에서 재진입 없음). */

	.getevents	= fio_windowsaio_getevents,
	/* [한국어] 완료 이벤트 수거. 옵션에 따라 두 경로 중 하나.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:td_io_getevents().
	 * 반환: 수거된 이벤트 수(>= min이 이상적, 타임아웃 시 미만 가능).
	 * 동기화: 잡 스레드. 기본 모드에선 완료 스레드(writer)와 io_complete/aio_events
	 * 공유 — 플래그 기반 단방향 데이터 플로우로 racy 아님. */

	.event		= fio_windowsaio_event,
	/* [한국어] getevents가 보고한 N건 각각에 대한 io_u 조회(인덱스 0..N-1).
	 * 설정자: 이 초기화. 읽는 자: ioengines.c.
	 * 반환: 유효한 io_u 포인터(NULL 불가).
	 * 동기화: 잡 스레드 단독. */

	.cleanup	= fio_windowsaio_cleanup,
	/* [한국어] 잡 종료 1회. 완료 스레드 join + 핸들/메모리 회수.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:td_io_cleanup().
	 * 동기화: 잡 스레드 단독, 1회만 호출. init 성공 이후에만 호출됨. */

	.open_file	= fio_windowsaio_open_file,
	/* [한국어] fio_file마다 호출. CreateFile(FILE_FLAG_OVERLAPPED) + IOCP 바인딩.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:td_io_open_file().
	 * 반환: 0=성공, !=0=실패(해당 파일 스킵 또는 잡 중단).
	 * 동기화: 잡 스레드 단독. 파일 순차 처리. */

	.close_file	= fio_windowsaio_close_file,
	/* [한국어] open_file 짝. CloseHandle.
	 * 설정자: 이 초기화. 읽는 자: ioengines.c:td_io_close_file().
	 * 동기화: 잡 스레드 단독. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 파일 크기 질의 — 코어 제공 공통 루틴 사용(stat 유사).
	 * 설정자: 이 초기화. 읽는 자: filesetup.c.
	 * 엔진 특화 로직 없음 — 기본 구현 위임. 동기화: 잡 스레드. */

	.io_u_init	= fio_windowsaio_io_u_init,
	/* [한국어] io_u 생성 시 fio_overlapped 할당/링크.
	 * 설정자: 이 초기화. 읽는 자: io_u.c:init_io_u().
	 * io_u 당 1회. 동기화: 잡 스레드 단독. */

	.io_u_free	= fio_windowsaio_io_u_free,
	/* [한국어] io_u 파괴 시 fio_overlapped 해제.
	 * 설정자: 이 초기화. 읽는 자: io_u.c:free_io_u().
	 * io_u 당 1회. 동기화: 잡 스레드 단독. */

	.options	= options,
	/* [한국어] 이 엔진의 fio_option 테이블. --cmdhelp/--enghelp에 노출.
	 * 설정자: 이 초기화. 읽는 자: fio 옵션 파서 init.
	 * 값: 위 options[] 배열(.name=NULL 종료).
	 * 동기화: 전역 불변 테이블. */

	.option_struct_size	= sizeof(struct windowsaio_options),
	/* [한국어] td->eo로 할당될 옵션 구조체의 크기. 파서가 malloc 크기 결정에 사용.
	 * 설정자: 이 초기화. 읽는 자: 옵션 파서.
	 * 값 범위: 양의 정수(현 구조 크기 = 8바이트 td 포인터 + 4바이트 플래그 + 패딩).
	 * 동기화: 컴파일 타임 상수. */

	/* [한국어] 주: 이 엔진은 flags 필드를 명시적으로 설정하지 않는다(=0).
	 *   - FIO_SYNCIO를 세팅하지 않음 → 기본이 비동기 엔진.
	 *   - FIO_DISKLESSIO를 세팅하지 않음 → 실제 파일/디바이스 필요.
	 *   - FIO_NOEXTEND 미세트 → 잡 중 파일 확장 허용.
	 *   - FIO_RAWIO 미세트 → 블록/파일 모두 허용(Windows는 block device 별도 처리).
	 *   - FIO_MEMALIGN 미세트 → O_DIRECT(FILE_FLAG_NO_BUFFERING) 시에도 기본 정렬 가정.
	 *   - FIO_BARRIER 미세트 → 드라이브 배리어 요구 없음.
	 *   - FIO_UNIDIR 미세트 → 읽기/쓰기 모두 가능.
	 *   - FIO_NODISKUTIL 미세트 → 디스크 통계(util%) 수집 허용(Windows에서 의미 제한적).
	 *   - FIO_FAKEIO 미세트 → 실제 데이터 이동 발생.
	 * 플래그 제로 = "평범한 비동기 블록 I/O 엔진" 기본형. */
};

/*
 * [한국어]
 * fio_windowsaio_register - 프로세스 로드 시 이 엔진을 전역 리스트에 등록(constructor).
 *
 * fio_init 속성(= __attribute__((constructor)))에 의해 main() 진입 전에 실행된다.
 * 정적 바이너리에서는 기동 시 1회, 공유 라이브러리(.so/.dll) 빌드에서는 dlopen/LoadLibrary 시점에 호출.
 *
 * 호출 체인: libc 동적 로더 (crt 초기화) → [이 함수] → register_ioengine(&ioengine).
 * 실행 컨텍스트: 프로세스 메인 스레드, 단 1회.
 */
static void fio_init fio_windowsaio_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] 전역 엔진 리스트에 링크 — load_ioengine("windowsaio") 매칭을 가능케 함. */
}

/*
 * [한국어]
 * fio_windowsaio_unregister - 프로세스 종료 시 등록 해제(destructor).
 *
 * fio_exit 속성(= __attribute__((destructor)))에 의해 main() 복귀 후 호출된다.
 * 정적 바이너리에서는 동작상 불필요하나 공유 라이브러리(dlclose) 정리 경로 안전장치.
 *
 * 호출 체인: libc atexit/dlclose → [이 함수] → unregister_ioengine(&ioengine).
 * 실행 컨텍스트: 프로세스 종료 스레드(일반적으로 메인), 단 1회.
 */
static void fio_exit fio_windowsaio_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 엔진 리스트에서 제거. */
}
