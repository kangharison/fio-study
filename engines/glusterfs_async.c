/*
 * glusterfs engine
 *
 * IO engine using Glusterfs's gfapi async interface
 *
 */

/*
 * [한국어 설명] GlusterFS 비동기 I/O 엔진 구현 (glusterfs_async.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 GlusterFS 클라이언트 라이브러리 gfapi(glfs_*)의 비동기 인터페이스를
 * 사용하는 fio I/O 엔진("gfapi_async")을 구현한다. 잡 스레드는 glfs_pread_async /
 * glfs_pwrite_async / glfs_fsync_async / glfs_fdatasync_async / (옵션)
 * glfs_discard_async 로 I/O를 제출하고, gfapi 내부 워커 스레드가 완료되면
 * 콜백(gf_async_cb)이 호출되어 io_u->engine_data(fio_gf_iou)의 io_complete 플래그를
 * 1로 설정한다. fio 잡 스레드는 td_io_getevents() 경로에서 io_u_all 큐를 폴링하여
 * 완료된 io_u를 수확한다. GlusterFS 볼륨 클라이언트 측 I/O 성능을 FUSE 마운트를
 * 우회해 측정하는 것이 목적이며, 코드 상태는 "실험적(experimental)"이고
 * td->o.use_thread=1 (스레드 잡 모드)로 강제된다(gfapi 콜백이 fork 격리된 자식
 * 프로세스 메모리에서 fio 내부 구조를 만지면 안 되므로).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 비동기 ioengine 계약 경로에 속한다. 흐름:
 *   backend.c의 잡 루프
 *     → td_io_queue() [ioengines.c]
 *         → fio_gf_async_queue()   (제출만, FIO_Q_QUEUED 반환)
 *             → glfs_p*_async(...) [libgfapi]
 *                 → (gfapi 워커 스레드) → gf_async_cb() → io_complete=1
 *     → td_io_getevents() [ioengines.c]
 *         → fio_gf_getevents() (io_u_all 순회, 완료된 io_u 수집)
 *     → fio_gf_event() (수집된 배열에서 i번째 io_u 반환)
 *     → put_io_u() [io_u.c]
 * 공통 연결/파일 오픈 경로(fio_gf_setup/open_file/close_file/unlink_file/
 * get_file_size/cleanup)는 동반 파일 engines/glusterfs.c에 있고, 본 파일은
 * 비동기 특화 콜백만 제공한다. 실행 컨텍스트는 두 가지이다: (a) 잡 스레드
 * — init/queue/getevents/event/cleanup, (b) gfapi 내부 워커 스레드 — gf_async_cb.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(fio 코어): ioengines.c가 struct ioengine_ops의 콜백들을 호출한다.
 *   init=fio_gf_async_setup, cleanup=fio_gf_cleanup, queue=fio_gf_async_queue,
 *   getevents=fio_gf_getevents, event=fio_gf_event, io_u_init=fio_gf_io_u_init,
 *   io_u_free=fio_gf_io_u_free, open_file/close_file/unlink_file/get_file_size는
 *   glusterfs.c에서 제공.
 * - 하위(외부 라이브러리): libgfapi — glfs_pread_async/glfs_pwrite_async/
 *   glfs_fsync_async/glfs_fdatasync_async, 선택적 glfs_discard_async(CONFIG_GF_TRIM
 *   이 정의된 경우). CONFIG_GF_NEW_API가 정의되면 콜백 시그니처가 prestat/poststat
 *   인자를 포함하도록 확장된다.
 * - 동반 파일: engines/glusterfs.c(공통 init/cleanup, 볼륨/파일 open), gfapi.h
 *   (struct gf_data, struct gf_options, fio_gf_* 선언 공유).
 * - 공유 상태: thread_data::io_ops_data == (struct gf_data *) — 볼륨 핸들(fs/fd),
 *   aio_events 링, gf_options. io_u::engine_data == (struct fio_gf_iou *) —
 *   완료 콜백과 getevents 사이의 완료 신호 전달 매개.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_gf_async_setup(td): 공통 fio_gf_setup() 후 aio_events 배열(iodepth 크기)
 *   할당. td->o.use_thread=1 강제.
 * - fio_gf_async_queue(td, io_u): ddir에 따라 glfs_p*_async로 제출, 성공 시
 *   FIO_Q_QUEUED, 실패 시 io_u->error 세팅 후 FIO_Q_COMPLETED(에러 보고 경로).
 * - gf_async_cb(fd, ret, [prestat,poststat,] data): gfapi 완료 콜백.
 *   io_u->engine_data->io_complete = 1.
 * - fio_gf_getevents(td, min, max, t): io_u_all을 순회하며 IO_U_F_FLIGHT 이면서
 *   io_complete 인 io_u를 aio_events[]에 적재, min 미만이면 100us usleep 후 재시도.
 * - fio_gf_event(td, i): aio_events[i] 반환 (fio 코어의 td_io_event() 계약).
 * - fio_gf_io_u_init/free(td, io_u): io_u당 fio_gf_iou 할당/해제.
 * - struct fio_gf_iou: { io_u*, io_complete } — 콜백-폴러 간 완료 비트 채널.
 * - static struct ioengine_ops ioengine: fio에 등록되는 플러그인 vtable.
 *   FIO_DISKLESSIO 플래그는 로컬 파일 디스크립터 없이도 I/O가 가능함을 알린다
 *   (GlusterFS 볼륨 핸들이 대신 사용됨).
 */

#include "gfapi.h"
/* [한국어] #include "gfapi.h": 동반 glusterfs.c와 공유되는 선언 헤더.
 *  struct gf_data(볼륨 fs/fd/aio_events), struct gf_options, 공통 콜백 프로토타입
 *  (fio_gf_setup, fio_gf_cleanup, fio_gf_open_file, fio_gf_close_file,
 *   fio_gf_unlink_file, fio_gf_get_file_size), 그리고 libgfapi 원본 헤더
 *  <glusterfs/api/glfs.h>를 체인 인클루드한다. 본 비동기 엔진이 의존하는 모든
 *  외부 타입(glfs_fd_t, glfs_stat, ssize_t)이 이 헤더를 거쳐 들어온다. */

#define NOT_YET 1
/* [한국어] #define NOT_YET 1: 본 엔진이 "아직 실험 단계"임을 표시하는 플래그.
 *  아래 fio_gf_async_setup()에서 #if defined(NOT_YET)로 경고 로그를 출력하도록
 *  하는 스위치. 향후 안정화되면 이 매크로를 제거하여 경고를 끄는 용도.
 *  값(1)은 단지 defined 조건을 만족시키기 위한 관례이며 수치적 의미는 없다. */

/* [한국어] GlusterFS 비동기 I/O 유닛 확장 데이터
 *  io_u::engine_data로 부착되어, gfapi 완료 콜백(워커 스레드)과 잡 스레드의
 *  폴러(fio_gf_getevents) 사이에서 "완료됨" 신호를 주고받는 보조 구조체. */
struct fio_gf_iou {
	struct io_u *io_u;
	/* [한국어] 이 fio_gf_iou와 짝지어진 fio io_u 포인터 (1:1 역참조).
	 * 설정자: fio_gf_io_u_init()에서 io_u->engine_data = io; io->io_u = io_u; 로
	 *         최초 설정. 이후 io_u의 수명 전체 동안 고정.
	 * 읽는 자: 현재 코드에서는 직접 참조되지는 않으나(gf_async_cb는 data 인자로
	 *         io_u를 직접 받음), 향후 디버깅/traceability를 위해 보관되는 역참조.
	 * 값 범위: 유효한 io_u* (NULL 아님). fio_gf_io_u_free() 시점까지 유효.
	 * 동기화: io_u는 한 잡 스레드 내에서만 생성/해제되므로 별도 락 불필요.
	 *         콜백 스레드는 io_complete 필드만 쓰고 이 포인터는 건드리지 않는다. */

	int io_complete;
	/* [한국어] 비동기 I/O 완료 비트(0=진행 중, 1=완료).
	 * 설정자: gf_async_cb() (gfapi 워커 스레드 컨텍스트)가 완료 시점에 1로 세팅.
	 *         fio_gf_io_u_init()에서 0으로 초기화.
	 * 읽는 자: fio_gf_getevents() (잡 스레드)가 폴링하며 1을 발견하면 다시 0으로
	 *         리셋하고 aio_events[]에 io_u를 적재.
	 * 값 범위: {0,1}. 이론상 다른 값은 진입 불가.
	 * 동기화: **명시적 락/atomic/memory barrier 없음** — 이는 본 엔진이 experimental
	 *         로 표시된 이유 중 하나이다. 실제로는 gfapi 워커가 단일 스레드로
	 *         순차 콜백을 돌리고, usleep(100) 루프로 인한 메모리 가시성에 의존한다.
	 *         엄밀히는 atomic_store/load 또는 pthread_mutex가 필요한 레이스 영역. */
};

/*
 * [한국어]
 * fio_gf_event - ioengine_ops::event 콜백 구현 (수집된 완료 배열의 i번째 io_u 반환)
 *
 * @td: 현재 잡의 thread_data. td->io_ops_data를 통해 gf_data(aio_events) 접근.
 * @event: 0..(직전 getevents 반환값-1) 범위의 인덱스.
 * @return: 완료된 io_u* — fio 코어(io_u.c의 io_completed 경로)가 통계 반영 후
 *          put_io_u()로 반환한다.
 *
 * fio는 td_io_getevents()로 완료 개수를 받은 뒤, 개수만큼 td_io_event(td, i)를
 * 순차 호출하여 각 완료 io_u를 가져온다. 본 엔진은 완료 io_u를 배열
 * gf_data::aio_events[]에 이미 적재해 두었으므로 단순 인덱싱만 수행한다.
 *
 * 실행 컨텍스트: 잡 스레드(작업 제출·수확을 담당하는 단일 스레드).
 * Caller: ioengines.c의 td_io_event() → backend.c의 io_completed() 경로.
 * Callee: 없음 (순수 배열 참조).
 * 에러 경로: 없음. event 인덱스 유효성은 호출자가 보장.
 *
 * 호출 체인: backend.c do_io() → io_completed() → td_io_event() → [이 함수]
 */
static struct io_u *fio_gf_event(struct thread_data *td, int event)
{
	struct gf_data *gf_data = td->io_ops_data;
	/* [한국어] 잡별 엔진 상태 포인터 획득. io_ops_data는 fio_gf_setup()(동반
	 * glusterfs.c)에서 calloc으로 할당되어 볼륨 핸들(fs/fd), aio_events 링,
	 * 옵션을 담는다. 본 엔진에서는 aio_events만 사용. */

	dprint(FD_IO, "%s\n", __FUNCTION__);
	/* [한국어] FD_IO 디버그 채널(--debug=io)로 함수 진입 로그 출력. 프로덕션
	 * 빌드에서는 매크로가 no-op에 가깝게 최적화되어 성능 영향 미미. */

	return gf_data->aio_events[event];
	/* [한국어] 사전에 fio_gf_getevents가 적재해 둔 완료 io_u 포인터를 그대로
	 * 반환. 배열 경계는 호출자(td_io_event)가 직전 getevents 반환값으로 제한하므로
	 * 여기서 검사하지 않는다. */
}

/*
 * [한국어]
 * fio_gf_getevents - ioengine_ops::getevents 콜백 구현 (완료 io_u 수확 폴러)
 *
 * @td: 잡의 thread_data.
 * @min: 최소 수집 목표 개수. min개 모이기 전까지는 블록(usleep 재시도)해야 한다.
 * @max: 상한 — aio_events[]는 iodepth 크기이므로 max도 iodepth 이하여야 안전.
 * @t:   상대 타임아웃(현 구현은 **사용하지 않음** — 순수 무한 대기 폴링).
 * @return: 실제 수집된 이벤트 수(>= min). 오류 반환은 없다.
 *
 * 동작: io_u_all 큐(잡이 보유한 모든 io_u의 플랫 배열)를 순회하며 현재 in-flight
 * (IO_U_F_FLIGHT)이고 콜백이 io_complete=1로 표시해 둔 io_u를 수집한다. 수집된
 * io_u는 aio_events[events++]에 넣고 io_complete를 0으로 리셋한다. max에 도달하면
 * 즉시 탈출, min 미만이면 100us usleep 후 처음부터 재스캔.
 *
 * 실행 컨텍스트: 잡 스레드. 콜백(gf_async_cb)은 gfapi 워커 스레드에서 io_complete
 * 를 1로 세팅한다. io_complete 필드는 명시적 동기화 없이 읽고 쓰므로 usleep으로
 * 인한 컨텍스트 스위치 시의 캐시 플러시/가시성에 의존하는 취약한 모델 — experimental.
 * 에러 경로: 없음(항상 성공). 상위의 타임아웃 처리 누락은 알려진 한계.
 *
 * 호출 체인: backend.c do_io() → td_io_getevents() → [이 함수] → (usleep loop)
 */
static int fio_gf_getevents(struct thread_data *td, unsigned int min,
			    unsigned int max, const struct timespec *t)
{
	struct gf_data *g = td->io_ops_data;
	/* [한국어] 엔진 상태 포인터 획득(위와 동일). */
	unsigned int events = 0;
	/* [한국어] 이번 호출에서 수집한 완료 이벤트 카운트. 반환값. */
	struct io_u *io_u;
	/* [한국어] io_u_qiter 매크로가 각 반복마다 채우는 루프 변수. */
	int i;
	/* [한국어] io_u_qiter가 사용하는 인덱스(io_u_all에서의 위치). */

	dprint(FD_IO, "%s\n", __FUNCTION__);
	/* [한국어] 디버그 로그 — 진입 확인용. */
	do {
		/* [한국어] 외부 do-while: min개를 모을 때까지 반복 스캔. */
		io_u_qiter(&td->io_u_all, io_u, i) {
			/* [한국어] io_u_qiter: io_u_all(잡이 보유한 모든 io_u의
			 * 벡터) 전체를 순회하는 매크로. io_u_all은 iodepth 크기의
			 * 배열과 동등하며, free/in-flight/done 상태와 무관하게
			 * "존재하는 모든 io_u"를 탐색한다. */
			struct fio_gf_iou *io;
			/* [한국어] io_u의 엔진 전용 보조 구조체 포인터. */

			if (!(io_u->flags & IO_U_F_FLIGHT))
				/* [한국어] IO_U_F_FLIGHT: 이 io_u가 현재 제출되어
				 * 완료 대기 중인지 여부. 비행 중이 아닌 io_u(free
				 * 상태 등)는 완료 수확 대상이 아니므로 건너뛴다. */
				continue;

			io = io_u->engine_data;
			/* [한국어] io_u에 부착된 fio_gf_iou 획득 — 여기 io_complete
			 * 플래그가 완료 신호. */
			if (io->io_complete) {
				/* [한국어] 비동기 콜백이 완료를 표시했는가?
				 * (엄밀히는 atomic_load가 필요한 지점이나 본 엔진은
				 *  experimental 이라 스핀+usleep 가시성에 의존.) */
				io->io_complete = 0;
				/* [한국어] 중복 수확 방지 및 다음 재사용을 위한
				 * 리셋. 이 io_u는 이미 완료로 처리되어 put_io_u
				 * 이후 free 풀로 돌아갈 예정. */
				g->aio_events[events] = io_u;
				/* [한국어] 완료된 io_u를 순서대로 aio_events 배열에
				 * 저장. 이후 fio 코어가 fio_gf_event(td, i)로 하나씩
				 * 가져가게 된다. */
				events++;
				/* [한국어] 수집 카운트 증가. */

				if (events >= max)
					/* [한국어] 상한(max) 도달 시 즉시 내부 루프
					 * 탈출 — 배열 오버플로 방지. */
					break;
			}

		}
		if (events < min)
			/* [한국어] 목표 최소치 미달: gfapi 워커가 더 완료를 올릴
			 * 때까지 잠깐 양보. */
			usleep(100);
			/* [한국어] 100 마이크로초 슬립. 비지 웨이트를 피해 CPU
			 * 양보. 타임아웃 인자 t는 여기서 고려되지 않는 **알려진
			 * 한계** — 조기 반환(ETIMEDOUT) 없이 min 달성까지 대기. */
		else
			break;
			/* [한국어] min 이상 수집 완료 → 외부 루프 탈출. */

	} while (1);

	return events;
	/* [한국어] 수집 이벤트 수 반환. fio 코어는 이 값만큼 fio_gf_event()를 호출한다. */
}

/*
 * [한국어]
 * fio_gf_io_u_free - ioengine_ops::io_u_free 콜백 (io_u별 엔진 데이터 해제)
 *
 * @td: 잡 컨텍스트(여기서는 사용하지 않지만 콜백 시그니처 준수용).
 * @io_u: 해제 대상 io_u — 아직 engine_data가 부착되어 있으면 free한다.
 * @return: 없음.
 *
 * 잡 종료 시(io_u_all의 각 io_u에 대해) 또는 재초기화 시 호출되어, 본 엔진이
 * io_u_init에서 malloc 했던 fio_gf_iou 메모리를 반환한다. 완료되지 않은 채로
 * 해제되면 경고 로그를 출력(자원 누수/프로그래밍 오류 탐지).
 *
 * 실행 컨텍스트: 잡 스레드의 종료/정리 경로.
 * Caller: io_u_free_mem()/put_io_u 종료 경로 → td_io_u_free() → [이 함수].
 * Callee: log_err(), free(3).
 * 에러 경로: 반환값 없음. io_complete==1 상태로 진입 시 에러 로그만 남기고 해제.
 */
static void fio_gf_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct fio_gf_iou *io = io_u->engine_data;
	/* [한국어] 부착된 엔진 데이터 포인터 획득. NULL일 수 있다
	 * (io_u_init이 실패했거나 이미 해제된 경우). */

	if (io) {
		/* [한국어] 유효한 포인터에만 해제 로직 적용 — 이중 해제 방지. */
		if (io->io_complete)
			/* [한국어] 완료 플래그가 남아있다는 것은 getevents가 수확
			 * 하기 전에 해제 경로로 들어왔음을 의미(비정상). */
			log_err("incomplete IO found.\n");
			/* [한국어] fio 표준 에러 로거로 경고 출력. 치명적 중단은
			 * 아니지만 사용자에게 이상 상황을 알림. */
		io_u->engine_data = NULL;
		/* [한국어] 댕글링 포인터 방지를 위해 명시적 NULL 세팅. */
		free(io);
		/* [한국어] io_u_init에서 malloc한 fio_gf_iou 반환. */
	}
}

/*
 * [한국어]
 * fio_gf_io_u_init - ioengine_ops::io_u_init 콜백 (io_u별 엔진 데이터 할당)
 *
 * @td: 잡 컨텍스트 — 실패 시 td_verror()로 에러 보고.
 * @io_u: 초기화 대상 io_u — engine_data 슬롯에 fio_gf_iou를 부착한다.
 * @return: 0 성공, 1 실패(malloc 실패 시, errno 보고 후).
 *
 * fio가 io_u_all의 모든 io_u를 생성할 때 각각에 대해 한 번씩 호출된다(td_io_u_init).
 * 본 엔진은 io_u마다 작은 동반 구조체 fio_gf_iou를 두어 완료 콜백과 폴러 간
 * 신호를 주고받는다.
 *
 * 실행 컨텍스트: 잡 스레드의 초기화 단계(루프 시작 전).
 * Caller: backend.c thread_main() → td_io_u_init() → ops->io_u_init.
 * Callee: malloc(3), td_verror().
 * 에러 경로: malloc 실패 시 td_verror로 errno 기록 후 1 반환 → td->error 세팅
 * → 잡 전체 조기 종료.
 */
static int fio_gf_io_u_init(struct thread_data *td, struct io_u *io_u)
{
    struct fio_gf_iou *io;
    /* [한국어] 새로 할당할 엔진 데이터 포인터. */
	dprint(FD_FILE, "%s\n", __FUNCTION__);
	/* [한국어] FD_FILE 디버그 채널 로그(init 경로에 주로 사용). */

    io = malloc(sizeof(struct fio_gf_iou));
    /* [한국어] 고정 크기 동반 구조체 할당. calloc이 아니므로 아래서 필드 명시
     * 초기화 필요. */
    if (!io) {
        /* [한국어] OOM 처리. */
        td_verror(td, errno, "malloc");
        /* [한국어] fio 표준 에러 보고 — td->error, td->verror 채우고 로그. */
        return 1;
        /* [한국어] 0이 아닌 반환 → 상위(td_io_u_init)에서 잡 중단으로 이어짐. */
    }
    io->io_complete = 0;
    /* [한국어] 완료 플래그 초기화. 콜백이 1로 세팅하기 전까지 "진행 중". */
    io->io_u = io_u;
    /* [한국어] 역참조 저장 — 디버깅/추적용. */
    io_u->engine_data = io;
    /* [한국어] fio 코어가 io_u 단위로 엔진 데이터를 주고받을 수 있게 부착.
     * 이후 queue/getevents/콜백 경로에서 이 포인터로 접근한다. */
	return 0;
	/* [한국어] 성공 반환. */
}

#if defined(CONFIG_GF_NEW_API)
/* [한국어] CONFIG_GF_NEW_API: configure 스크립트가 최신 libgfapi(prestat/poststat
 *  인자를 포함하는 확장 콜백 시그니처)를 감지했을 때 정의됨. 구 버전 API와
 *  바이너리 호환을 위해 컴파일 시 분기. */
static void gf_async_cb(glfs_fd_t * fd, ssize_t ret, struct glfs_stat *prestat,
			struct glfs_stat *poststat, void *data)
#else
/* [한국어] 구 버전 libgfapi: (fd, ret, data) 시그니처. */
static void gf_async_cb(glfs_fd_t * fd, ssize_t ret, void *data)
#endif
/*
 * [한국어]
 * gf_async_cb - libgfapi 비동기 I/O 완료 콜백
 *
 * @fd:   완료된 I/O의 glfs_fd_t (본 엔진에서는 검사하지 않음 — 단일 볼륨/fd).
 * @ret:  바이트 수(>=0) 또는 음수 오류. 현재 구현은 오류를 io_u->error에 반영하지
 *        않는 한계가 있다(ret 확인 없이 무조건 완료 표시).
 * @prestat/@poststat: CONFIG_GF_NEW_API 전용 — I/O 전/후 stat (미사용).
 * @data: glfs_p*_async 호출 시 전달한 io_u 포인터(제출 시 마지막 인자).
 * @return: 없음 (void, libgfapi 콜백 계약).
 *
 * 실행 컨텍스트: **gfapi 내부 워커 스레드**(잡 스레드 아님). 따라서 fio 자료
 * 구조에 접근 시 동기화가 필요하지만, 현재 코드는 io_complete만 건드려 최소
 * 접촉 면으로 관리한다. 이는 experimental 표기의 근거이기도 하다.
 * Caller: libgfapi 워커 — glfs_p*_async 완료 핸들러 디스패처.
 * Callee: 없음(플래그 세팅만).
 * 에러 경로: ret<0 이어도 현재 io_complete=1 로 동일 처리 — 향후 보강 여지.
 *
 * 호출 체인: libgfapi worker → [이 함수] → io_u->engine_data->io_complete=1
 *            → (잡 스레드 폴러) fio_gf_getevents() 수확
 */
{
	struct io_u *io_u = data;
	/* [한국어] 콜백 컨텍스트 복원 — 제출 시 마지막 인자로 넘긴 io_u. */
	struct fio_gf_iou *iou = io_u->engine_data;
	/* [한국어] 부착된 엔진 데이터 획득. io_u_init에서 설정된 값. */

	dprint(FD_IO, "%s ret %zd\n", __FUNCTION__, ret);
	/* [한국어] 디버그 로그에 반환 바이트 수 출력. */
	iou->io_complete = 1;
	/* [한국어] 완료 비트 세팅 — 잡 스레드 폴러가 곧 수확한다. 현 구현에서는
	 * atomic/mb 없이 평범한 스토어이므로, 잡 스레드는 usleep(100) 루프의
	 * 컨텍스트 스위치로 인한 암묵적 메모리 가시성에 의존한다. */
}

/*
 * [한국어]
 * fio_gf_async_queue - GlusterFS 비동기 엔진의 I/O 제출 콜백 (ioengine_ops::queue)
 *
 * @td:   잡의 thread_data — fio_unused 표식은 현 구현이 첫 인자를 직접 참조하지
 *        않는다는 컴파일러 힌트(실제로는 td->io_ops_data를 참조하므로 엄밀히는
 *        사용하지만, 과거 호환 표기가 남아 있음).
 * @io_u: 제출할 I/O 요청(준비 완료 상태 — buf/offset/ddir 세팅 끝).
 * @return: FIO_Q_QUEUED(비동기 제출 성공, 완료는 나중), FIO_Q_COMPLETED(실패로
 *          즉시 에러 완료 처리 — io_u->error 세팅).
 *
 * 동작: io_u->ddir(READ/WRITE/TRIM/SYNC/DATASYNC)에 따라 대응 glfs_*_async()를
 * 호출하고, 콜백으로 gf_async_cb와 data=io_u를 전달한다. 비동기이므로 성공
 * 반환은 "큐잉됨"을 의미한다(미완료). TRIM 경로는 CONFIG_GF_TRIM 있을 때만 존재.
 *
 * 실행 컨텍스트: 잡 스레드. libgfapi 내부에서 워커 스레드로 디스패치된다.
 * Caller: ioengines.c td_io_queue() → ops->queue.
 * Callee: glfs_pread_async / glfs_pwrite_async / glfs_discard_async /
 *         glfs_fdatasync_async / glfs_fsync_async (libgfapi).
 * 에러 경로: glfs_*_async 반환 != 0 → goto failed → io_u->error, td_verror,
 *           FIO_Q_COMPLETED 반환(상위에서 즉시 에러 카운트 반영).
 *
 * 호출 체인: td_io_queue() → [이 함수] → glfs_p*_async() → (libgfapi worker)
 *            → gf_async_cb() → io_complete=1
 */
static enum fio_q_status fio_gf_async_queue(struct thread_data fio_unused * td,
					    struct io_u *io_u)
{
	struct gf_data *g = td->io_ops_data;
	/* [한국어] 엔진 상태 포인터 — 볼륨 fd(g->fd)를 async 호출에 사용. */
	int r;
	/* [한국어] glfs_*_async 반환값(0=성공, 음수/errno=실패). */

	dprint(FD_IO, "%s op %s\n", __FUNCTION__, io_ddir_name(io_u->ddir));
	/* [한국어] 디버그: 어떤 방향(READ/WRITE/TRIM/...)이 들어왔는지 로깅. */

	fio_ro_check(td, io_u);
	/* [한국어] read-only 보호: 잡이 readonly인데 WRITE가 오면 assert/종료.
	 * 프로그래밍 오류 방어. */

	if (io_u->ddir == DDIR_READ)
		/* [한국어] READ 경로: 비동기 preadv에 해당. xfer_buf에 읽어들임. */
		r = glfs_pread_async(g->fd, io_u->xfer_buf, io_u->xfer_buflen,
				     io_u->offset, 0, gf_async_cb, io_u);
		/* [한국어] 인자: (fd, buf, size, offset, flags=0, cb, cb_data).
		 *  flags=0 — 특별 옵션 없음. cb_data로 io_u를 전달해 콜백 내
		 *  engine_data 복원. */
	else if (io_u->ddir == DDIR_WRITE)
		/* [한국어] WRITE 경로: 비동기 pwrite. */
		r = glfs_pwrite_async(g->fd, io_u->xfer_buf, io_u->xfer_buflen,
				      io_u->offset, 0, gf_async_cb, io_u);
		/* [한국어] pwrite: 오프셋 기반 쓰기. flags=0. */
#if defined(CONFIG_GF_TRIM)
	/* [한국어] libgfapi가 discard(trim) 지원을 빌드했을 때만 컴파일.
	 *  GlusterFS 볼륨이 sparse/thin 프로비저닝을 지원할 때 범위 해제 기능. */
	else if (io_u->ddir == DDIR_TRIM)
		r = glfs_discard_async(g->fd, io_u->offset, io_u->xfer_buflen,
				       gf_async_cb, io_u);
		/* [한국어] TRIM: 특정 범위 해제 요청. 데이터 버퍼는 필요 없음. */
#endif
	else if (io_u->ddir == DDIR_DATASYNC)
		/* [한국어] DATASYNC: fdatasync 유사 — 데이터만 내구성 보장. */
		r = glfs_fdatasync_async(g->fd, gf_async_cb, io_u);
	else if (io_u->ddir == DDIR_SYNC)
		/* [한국어] SYNC: fsync 유사 — 데이터+메타데이터 내구성 보장. */
		r = glfs_fsync_async(g->fd, gf_async_cb, io_u);
	else
		r = EINVAL;
		/* [한국어] 지원하지 않는 ddir(예: 미래 확장값) → EINVAL로 실패
		 * 경로 진입. */

	if (r) {
		/* [한국어] 0이 아니면 제출 실패 — 에러 보고 경로. */
		log_err("glfs queue failed.\n");
		io_u->error = r;
		/* [한국어] io_u에 에러 코드 기록 — 상위가 통계/보고에 사용. */
		goto failed;
	}
	return FIO_Q_QUEUED;
	/* [한국어] 성공: 비동기 큐잉 완료. 완료는 getevents가 수확한다. */

failed:
	io_u->error = r;
	/* [한국어] (중복 세팅이나 안전상 재확인) 에러 코드 고정. */
	td_verror(td, io_u->error, "xfer");
	/* [한국어] fio 표준 에러 보고 — "xfer" 태그로 전송 오류 표시. */
	return FIO_Q_COMPLETED;
	/* [한국어] 즉시 완료(실패)로 반환 — 상위가 io_u를 에러로 처리하고
	 * put_io_u로 free 풀 반환. */
}

/*
 * [한국어]
 * fio_gf_async_setup - ioengine_ops::init 콜백 (비동기 엔진 초기화)
 *
 * @td: 잡의 thread_data.
 * @return: 0 성공, <0 실패(-ENOMEM 등) 또는 fio_gf_setup()이 반환한 오류 코드.
 *
 * 역할: (1) NOT_YET 경고 로그 출력(실험 단계 고지), (2) 공통 setup 호출
 * (볼륨 연결, gf_data 할당), (3) use_thread=1 강제(gfapi 콜백이 잡 주소공간에서
 * 실행되어야 하므로 fork 모드 금지), (4) aio_events 링(크기=iodepth) 할당.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계(잡 루프 시작 전, 단 1회).
 * Caller: backend.c thread_main() → td_io_init() → ops->init.
 * Callee: fio_gf_setup() (glusterfs.c), calloc(3), fio_gf_cleanup() (실패 정리).
 * 에러 경로: fio_gf_setup 실패 → 해당 rc 즉시 반환. aio_events calloc 실패 →
 * fio_gf_cleanup으로 공통 자원 정리 후 -ENOMEM 반환.
 */
static int fio_gf_async_setup(struct thread_data *td)
{
	struct gf_data *g;
	/* [한국어] 공통 setup이 할당하는 gf_data 포인터를 받아올 지역 변수. */
	int r;
	/* [한국어] 반환/결과 코드. */

#if defined(NOT_YET)
	/* [한국어] 위 #define NOT_YET 1에 의해 현재 항상 활성. 사용자에게
	 *  본 엔진의 불안정성을 상기시키는 경고 출력. */
	log_err("the async interface is still very experimental...\n");
#endif
	r = fio_gf_setup(td);
	/* [한국어] 동반 glusterfs.c의 공통 초기화: glfs_new/glfs_init로 볼륨 연결,
	 * td->io_ops_data에 gf_data 할당. */
	if (r)
		return r;
		/* [한국어] 공통 초기화 실패 → 그대로 전파(cleanup은 fio_gf_setup
		 * 내부에서 이미 수행된다고 가정). */

	td->o.use_thread = 1;
	/* [한국어] 스레드 잡 모드 강제 — fork 잡이면 gfapi 내부 워커 스레드가
	 * 자식 프로세스의 독립 주소공간에서 실행되어 io_u 접근이 불가능해진다.
	 * 이 옵션은 backend에 "프로세스가 아닌 pthread로 잡을 띄우라"고 지시. */
	g = td->io_ops_data;
	/* [한국어] 공통 setup이 채워둔 gf_data 포인터 획득. */
	g->aio_events = calloc(td->o.iodepth, sizeof(struct io_u *));
	/* [한국어] 완료 수확용 링 할당 — getevents가 완료 io_u 포인터를
	 * 여기 기록하고, event(td,i)가 인덱스로 반환한다. 크기=iodepth는
	 * 동시 in-flight 상한(잡 옵션 --iodepth). calloc으로 0 초기화. */
	if (!g->aio_events) {
		/* [한국어] OOM: 이미 볼륨까지 연결한 상태이므로 롤백 필요. */
		r = -ENOMEM;
		fio_gf_cleanup(td);
		/* [한국어] 공통 정리: glfs_close/glfs_fini, gf_data 해제. */
		return r;
	}

	return r;
	/* [한국어] r==0: 초기화 성공. */
}

/* [한국어] static struct ioengine_ops ioengine — fio에 등록되는 이 엔진의
 *  플러그인 vtable. fio_init 생성자(fio_gf_register)에서 register_ioengine
 *  으로 전역 리스트에 연결되어 이름("gfapi_async")으로 선택 가능해진다.
 *  각 필드는 ioengines.c가 잡 생애 주기의 특정 시점에 호출하는 훅. */
static struct ioengine_ops ioengine = {
	.name = "gfapi_async",
	/* [한국어] 사용자 --ioengine=gfapi_async 로 선택하는 식별자. */
	.version = FIO_IOOPS_VERSION,
	/* [한국어] fio ABI 버전. 구조체 레이아웃이 바뀌면 증가 — 동적 로드
	 * 호환성 검증에 사용. */
	.init = fio_gf_async_setup,
	/* [한국어] 잡 시작 1회 — 볼륨 연결 + aio_events 할당. */
	.cleanup = fio_gf_cleanup,
	/* [한국어] 잡 종료 1회 — 볼륨 해제(공통, glusterfs.c). */
	.queue = fio_gf_async_queue,
	/* [한국어] io_u당 호출 — 비동기 제출. */
	.open_file = fio_gf_open_file,
	/* [한국어] 파일별 1회 — 대상 파일 open(glusterfs.c). */
	.close_file = fio_gf_close_file,
	/* [한국어] 파일별 1회 — 대상 파일 close(glusterfs.c). */
	.unlink_file = fio_gf_unlink_file,
	/* [한국어] --unlink 옵션 시 파일 삭제(glusterfs.c). */
	.get_file_size = fio_gf_get_file_size,
	/* [한국어] 잡 시작 시 파일 크기 조회(glusterfs.c). */
	.getevents = fio_gf_getevents,
	/* [한국어] 완료 수확 폴러 — 본 파일 구현. */
	.event = fio_gf_event,
	/* [한국어] getevents 후 개별 io_u 반환 — 본 파일 구현. */
	.io_u_init = fio_gf_io_u_init,
	/* [한국어] io_u별 1회 — fio_gf_iou 할당. */
	.io_u_free = fio_gf_io_u_free,
	/* [한국어] io_u별 1회 — fio_gf_iou 해제. */
	.options = gfapi_options,
	/* [한국어] 엔진 전용 옵션 테이블(gfapi.h/glusterfs.c에서 정의) —
	 *  volume/brick 등 gfapi 관련 옵션 파서. */
	.option_struct_size = sizeof(struct gf_options),
	/* [한국어] 옵션 구조체 크기 — fio가 잡별 옵션 버퍼를 할당할 때 사용. */
	.flags = FIO_DISKLESSIO,
	/* [한국어] FIO_DISKLESSIO: 로컬 디스크 파일 디스크립터가 없어도 되는
	 *  엔진임을 fio에 알림(GlusterFS 볼륨 핸들이 대체). 이 플래그로 인해
	 *  fio는 파일 생성/pre-alloc 경로 일부를 건너뛴다. */
};

/*
 * [한국어]
 * fio_gf_register - fio 초기화 시점 자동 호출되는 엔진 등록자
 *
 * fio_init 속성(=GCC constructor)이 붙어 있어 fio 프로세스 시작 시(main 진입 전)
 * 자동 실행된다. 이를 통해 엔진 이름이 동적으로 ioengine 레지스트리에 추가되어
 * --ioengine=gfapi_async 로 선택 가능해진다.
 *
 * 실행 컨텍스트: 프로세스 메인 스레드, main() 진입 직전(생성자 체인).
 * Caller: dynamic linker / libc ctor runner.
 * Callee: register_ioengine() (ioengines.c).
 * 에러 경로: 없음(단순 연결 리스트 삽입).
 */
static void fio_init fio_gf_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] 전역 ioengine 리스트에 본 vtable 포인터 추가. */
}

/*
 * [한국어]
 * fio_gf_unregister - fio 종료 시점 자동 호출되는 엔진 해제자
 *
 * fio_exit(=GCC destructor) 속성으로 프로세스 종료 시 자동 실행되어 레지스트리
 * 에서 본 엔진을 제거한다. shared object로 동적 로드된 경우에 특히 의미가 있다.
 *
 * 실행 컨텍스트: 프로세스 종료 단계(atexit/destructor).
 * Caller: libc destructor runner.
 * Callee: unregister_ioengine() (ioengines.c).
 * 에러 경로: 없음.
 */
static void fio_exit fio_gf_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 리스트에서 본 vtable 제거 — 이후 이름 참조 불가. */
}
