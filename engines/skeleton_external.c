/*
 * Skeleton for a sample external io engine
 *
 * Should be compiled with:
 *
 * gcc -Wall -O2 -g -D_GNU_SOURCE -include ../config-host.h -shared -rdynamic -fPIC -o skeleton_external.o skeleton_external.c
 * (also requires -D_GNU_SOURCE -DCONFIG_STRSEP on Linux)
 *
 */

/*
 * [한국어 설명] 외부(external) I/O 엔진 스켈레톤/템플릿 (skeleton_external.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 실제 I/O를 수행하지 않는 "템플릿(골격) 코드"로서, 써드파티가 fio의
 * 외부 I/O 엔진을 직접 작성할 때 복사해서 사용하는 참조 구현이다. fio의 내부 엔진들
 * (sync, libaio, io_uring 등)과 달리 fio 바이너리에 정적으로 링크되지 않고, 별도의
 * 공유 라이브러리(.so / .o with -shared -fPIC)로 컴파일되어 fio가 실행 시점에
 * dlopen()/dlsym()으로 로드한다. 이 스켈레톤의 모든 콜백은 비어 있거나 기본값을
 * 반환하므로, 개발자는 이 파일을 베이스로 삼아 원하는 백엔드(새로운 스토리지 API,
 * 네트워크 프로토콜, 하드웨어 가속기 등) 호출을 채워 넣는 방식으로 엔진을 만든다.
 * 즉 이 파일 자체는 "컴파일 가능한 최소 예시"이며, 실행 의미는 FIO_Q_COMPLETED만
 * 반환하는 no-op 엔진이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 I/O 계층은 "코어(backend.c/io_u.c/ioengines.c) → ioengine_ops 콜백 →
 * 백엔드 I/O API" 구조로 구성된다. 외부 엔진은 이 구조의 가장 오른쪽(ioengine_ops)만
 * 구현하며, fio 코어와는 `struct ioengine_ops ioengine` 심볼(이 파일 마지막)로만 연결된다.
 * load_ioengine()(ioengines.c)이 이 구조체를 dlsym(handle, "ioengine")으로 찾아서
 * td->io_ops에 바인딩하면, 이후 backend.c의 잡 스레드 루프가 `td_io_queue() →
 * td->io_ops->queue() → fio_skeleton_queue()` 형태로 콜백을 호출한다.
 * 호출 체인:
 *   main() → fio_backend() → thread_main() → do_io() → td_io_queue()
 *     → (td->io_ops->queue == fio_skeleton_queue)
 *   main() → load_ioengine() [ioengines.c] → dlopen/dlsym → `ioengine` 심볼
 * 실행 컨텍스트: 모두 호스트 유저스페이스의 잡 스레드에서 수행된다 (fio는 잡 = 스레드/프로세스 모델).
 *
 * === 타 모듈과의 연결 ===
 * - 의존 모듈:
 *   · fio.h — thread_data, io_u, fio_file 등 코어 자료구조 타입 정의.
 *   · optgroup.h — FIO_OPT_C_ENGINE / FIO_OPT_G_INVALID 등 옵션 카테고리/그룹 상수.
 *   · generic_open_file()/generic_close_file() — 기본 파일 열기/닫기 구현 (filesetup.c).
 *   · fio_ro_check() — readonly 모드에서 write 시도를 차단하는 가드 (io_u.c).
 * - 이 파일에 의존하는 쪽:
 *   · ioengines.c의 load_ioengine()이 `ioengine` 심볼을 dlsym으로 로드.
 *   · 이후 backend.c의 do_io() 루프가 콜백을 호출.
 * - 데이터 흐름:
 *   coreside에서 get_io_u()로 io_u를 할당 → td_io_prep() → td_io_queue()로 엔진에 전달 →
 *   (실제 엔진이라면) 백엔드 I/O 수행 → td_io_getevents() + event()로 완료 회수 →
 *   put_io_u()로 반환. 이 스켈레톤은 queue 시점에 FIO_Q_COMPLETED만 반환하여
 *   동기 엔진처럼 즉시 완료된 것으로 처리되게 한다.
 * - 공유 자료구조:
 *   struct thread_data *td (잡 단위 컨텍스트), struct io_u (I/O 요청 단위),
 *   struct fio_file (대상 파일/디바이스), struct ioengine_ops (플러그인 콜백 테이블).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_skeleton_options: 엔진 전용 옵션 저장 구조체. `pad` 필드는 off1=0 문제를
 *   회피하기 위한 더미 포인터이며, `dummy`는 예시 옵션 필드이다.
 * - options[]: fio 옵션 파서에 등록되는 FIO_OPT_* 배열. name=NULL로 종료한다.
 * - fio_skeleton_event(): getevents가 N을 반환한 뒤 [0..N-1]번째 완료 io_u를 코어에 반환.
 *   실제 엔진에서는 엔진 내부 완료 큐에서 io_u를 꺼내 반환한다.
 * - fio_skeleton_getevents(): 비동기 엔진의 완료 이벤트를 수집하여 개수를 반환.
 * - fio_skeleton_queue(): 핵심 콜백. io_u->ddir 방향으로 io_u->xfer_buf에 I/O를 발행한다.
 *   이 스켈레톤은 항상 FIO_Q_COMPLETED(동기 완료)만 반환한다.
 * - fio_skeleton_prep(): queue 전에 io_u를 사전 가공 (옵셔널).
 * - fio_skeleton_init()/cleanup(): 잡 단위 엔진 상태 할당/해제.
 * - fio_skeleton_open/close(): 파일 열기/닫기 (여기서는 generic_*로 위임).
 * - fio_skeleton_get_zoned_model()/report_zones()/reset_wp()/get_max_open_zones():
 *   ZBD(Zoned Block Device) 모드 지원 콜백. 스켈레톤은 ZBD_NONE(일반 디바이스)로 처리.
 * - struct ioengine_ops ioengine: dlsym이 찾는 export 심볼. 모든 콜백을 이 구조체에 묶어
 *   fio 코어에 전달한다.
 */

#include <stdio.h>       /* [한국어] 표준 입출력 — 디버깅용 printf/fprintf 사용 목적. */
#include <stdlib.h>      /* [한국어] malloc/free/exit 등 표준 유틸. 엔진 상태 할당에 사용. */
#include <unistd.h>      /* [한국어] POSIX 시스템 호출(read/write/close 등) 선언. 실제 엔진은 필요. */
#include <errno.h>       /* [한국어] errno 전역 및 E* 에러 상수. 시스템 호출 실패 시 리턴 경로에 사용. */
#include <assert.h>      /* [한국어] assert() — 개발 중 불변식 검증. 릴리즈에서도 유지 가능. */

#include "../fio.h"      /* [한국어] fio 코어 타입 (thread_data, io_u, fio_file, fio_q_status 등) 정의. */
#include "../optgroup.h" /* [한국어] FIO_OPT_C_ENGINE / FIO_OPT_G_INVALID 등 옵션 카테고리/그룹 상수 정의. */

/*
 * The core of the module is identical to the ones included with fio,
 * read those. You cannot use register_ioengine() and unregister_ioengine()
 * for external modules, they should be gotten through dlsym()
 */

/*
 * The io engine can define its own options within the io engine source.
 * The option member must not be at offset 0, due to the way fio parses
 * the given option. Just add a padding pointer unless the io engine has
 * something usable.
 */
/*
 * [한국어] 외부 엔진 전용 옵션 구조체.
 * 이 구조체는 ioengine.option_struct_size로 fio 코어에 크기를 알려주며,
 * 코어는 잡마다 이 크기만큼의 메모리를 할당하여 옵션을 채운다. options[] 배열의
 * 각 항목이 .off1 = offsetof(struct fio_skeleton_options, <field>)로 이 구조체
 * 내 위치를 지정한다. fio의 옵션 파서는 off1==0을 "옵션 없음"으로 취급하므로,
 * 첫 필드는 반드시 non-zero offset이 되어야 한다 → 그래서 pad 더미를 둔다.
 */
struct fio_skeleton_options {
	void *pad;
	/* [한국어] 더미 패딩 포인터.
	 * 역할: 실제 옵션 필드(dummy 등)의 offsetof()가 0이 되지 않도록 첫 바이트를 점유.
	 * 설정자: 아무도 설정하지 않음 (실제로 사용되지 않는 값).
	 * 읽는 자: 없음. 컴파일러가 자동으로 초기 정렬 영역을 확보하게 함.
	 * 값 범위: 임의 (NULL 상태로 유지되지만 읽히지 않음).
	 * 동기화: 읽히지 않으므로 동기화 대상 아님. */

	unsigned int dummy;
	/* [한국어] 예시 옵션 플래그 필드 (FIO_OPT_STR_SET 타입).
	 * 역할: 사용자가 잡 파일에 `dummy=1`을 쓰면 여기에 1이 저장되는 예시.
	 * 설정자: fio의 옵션 파서가 options[] 항목의 .off1 오프셋으로 이 필드에 값을 기록.
	 * 읽는 자: 실제 엔진 구현에서는 queue()/init()에서 읽어 동작을 분기하는 용도로 사용.
	 *         이 스켈레톤은 읽지 않는다.
	 * 값 범위: FIO_OPT_STR_SET은 0(지정 안 함) / 1(지정됨) 중 하나.
	 * 동기화: 잡 스레드 1개만이 이 구조체를 소유하므로 별도 락 불필요. */
};

/*
 * [한국어] fio 옵션 파서에 등록할 엔진 전용 옵션 테이블.
 * 각 항목은 struct fio_option이며, 마지막 항목은 .name=NULL로 종료(센티널)한다.
 * 이 배열은 ioengine.options 필드를 통해 코어에 전달된다. static/파일 로컬로 두어
 * 다른 .so 엔진과의 심볼 충돌을 피한다.
 */
static struct fio_option options[] = {
	{
		.name	= "dummy",                                    /* [한국어] CLI/잡 파일에서 사용할 옵션 이름 (--dummy). */
		.lname	= "ldummy",                                   /* [한국어] 긴 이름(long name) — help 출력 및 파싱에 사용. */
		.type	= FIO_OPT_STR_SET,                            /* [한국어] 타입: 값 없이 "있다/없다"만 표현하는 불리언 플래그. */
		.off1	= offsetof(struct fio_skeleton_options, dummy), /* [한국어] 이 옵션 값이 저장될 구조체 내 오프셋. 0이 되면 안 됨 → pad 덕분에 non-zero. */
		.help	= "Set dummy",                                /* [한국어] `fio --enghelp=engine_name`에 표시될 설명 문자열. */
		.category = FIO_OPT_C_ENGINE,                         /* [한국어] 옵션 카테고리: 엔진 전용. always use this (원본 주석 지시). */
		.group	= FIO_OPT_G_INVALID,                          /* [한국어] 그룹: 여기서는 유의미한 그룹이 없어 INVALID(미분류). 실제 엔진은 적절히 설정. */
	},
	{
		.name	= NULL,                                        /* [한국어] 배열 종료 센티널 — 파서가 여기서 루프 종료. */
	},
};

/*
 * The ->event() hook is called to match an event number with an io_u.
 * After the core has called ->getevents() and it has returned eg 3,
 * the ->event() hook must return the 3 events that have completed for
 * subsequent calls to ->event() with [0-2]. Required.
 */
/*
 * [한국어]
 * fio_skeleton_event - 완료된 이벤트 번호를 대응하는 io_u로 변환해 반환.
 *
 * @td: 이 잡의 thread_data. 엔진별 상태는 td->io_ops_data에 저장됨.
 * @event: getevents()가 반환한 N개의 완료 이벤트 중 [0..N-1] 인덱스.
 * @return: 해당 인덱스의 완료된 io_u 포인터. 스켈레톤은 항상 NULL(no-op).
 *
 * 코어는 td_io_getevents()가 N을 반환한 직후, 각 인덱스에 대해 이 함수를 호출하여
 * 어떤 io_u가 완료되었는지 식별한다. 실제 비동기 엔진은 내부 완료 큐(배열/링)에서
 * 해당 인덱스의 io_u 포인터를 꺼내 반환한다.
 * 실행 컨텍스트: 잡 스레드의 do_io() 루프 안, io_u_queued_complete() 경로에서 호출.
 * 동시성: 잡 스레드 단독 접근 — 재진입 불가, 락 불필요.
 * 에러 경로: 스켈레톤은 에러를 만들지 않음. 실제 엔진에서는 NULL 반환이 곧 "없음"으로 취급.
 *
 * 호출 체인:
 *   do_io() → io_u_queued_complete() → td_io_getevents() → [fio_skeleton_event()]
 */
static struct io_u *fio_skeleton_event(struct thread_data *td, int event)
{
	return NULL; /* [한국어] no-op 템플릿: 완료 io_u 없음을 의미 (실제 엔진은 내부 완료 배열 참조). */
}

/*
 * The ->getevents() hook is used to reap completion events from an async
 * io engine. It returns the number of completed events since the last call,
 * which may then be retrieved by calling the ->event() hook with the event
 * numbers. Required.
 */
/*
 * [한국어]
 * fio_skeleton_getevents - 비동기 엔진의 완료 이벤트를 수집해 개수 반환.
 *
 * @td: 잡 컨텍스트.
 * @min: 최소 수집 개수 — 이 수에 도달할 때까지 블로킹 (에이전트 선택).
 * @max: 최대 수집 개수 — 이 이상은 반환하지 않음.
 * @t: 타임아웃 (NULL이면 min까지 대기, 아니면 절대/상대 시각).
 * @return: 이번 호출에서 수집된 완료 이벤트 수 (>=0). 이후 이 숫자만큼 ->event()가 호출됨.
 *
 * 실제 비동기 엔진에서는 io_getevents(2), io_uring_enter(2), epoll_wait(2),
 * aio_suspend(3) 등으로 완료를 폴링/대기한다. 스켈레톤은 완료가 없으므로 항상 0 반환.
 * 실행 컨텍스트: 잡 스레드의 io_u_queued_complete() 경로.
 *
 * 호출 체인:
 *   do_io() → io_u_queued_complete() → td_io_getevents() → [fio_skeleton_getevents()]
 *     → (실제 엔진: io_getevents/io_uring_enter 등 syscall)
 */
static int fio_skeleton_getevents(struct thread_data *td, unsigned int min,
				  unsigned int max, const struct timespec *t)
{
	return 0; /* [한국어] 완료 이벤트 0개 — 동기 엔진처럼 queue에서 즉시 완료하기 때문에 여기서는 수집할 것 없음. */
}

/*
 * The ->queue() hook is responsible for initiating io on the io_u
 * being passed in. If the io engine is a synchronous one, io may complete
 * before ->queue() returns. Required.
 *
 * The io engine must transfer in the direction noted by io_u->ddir
 * to the buffer pointed to by io_u->xfer_buf for as many bytes as
 * io_u->xfer_buflen. Residual data count may be set in io_u->resid
 * for a short read/write.
 */
/*
 * [한국어]
 * fio_skeleton_queue - 주어진 io_u에 대해 실제 I/O를 발행 (엔진의 심장부).
 *
 * @td: 잡 컨텍스트.
 * @io_u: 이번에 제출할 I/O 유닛. io_u->ddir(READ/WRITE/TRIM), io_u->xfer_buf,
 *        io_u->xfer_buflen, io_u->offset 필드를 읽어 백엔드에 전달한다.
 * @return: FIO_Q_COMPLETED(동기 완료) | FIO_Q_QUEUED(비동기 대기) | FIO_Q_BUSY(큐 포화).
 *
 * 실제 엔진에서는 백엔드 API(read/write/pread/io_submit/io_uring_submit/네트워크 send 등)를
 * 호출한다. FIO_Q_BUSY를 반환하려면 ->commit() 콜백을 구현해야 한다 (배치 플러시 용도).
 * 짧은 전송(short I/O)이 발생하면 io_u->resid에 남은 바이트 수를 기록해 코어가 재시도 가능.
 * 실행 컨텍스트: 잡 스레드 do_io() 루프 내 — 동기 모드면 여기서 완료까지 끝낸다.
 *
 * 호출 체인:
 *   do_io() → td_io_queue() → [fio_skeleton_queue()] → (실제 엔진: 백엔드 I/O 호출)
 */
static enum fio_q_status fio_skeleton_queue(struct thread_data *td,
					    struct io_u *io_u)
{
	/*
	 * Double sanity check to catch errant write on a readonly setup
	 */
	fio_ro_check(td, io_u);
	/* [한국어] readonly 잡(td->o.read_only)에 WRITE io_u가 들어왔는지 이중 검사.
	 * fio 코어가 이미 검사하지만, 엔진 단에서도 방어적으로 호출해 assert 패닉을 유도 →
	 * 개발 중 잘못된 워크로드 설정을 조기에 잡아낸다. */

	/*
	 * Could return FIO_Q_QUEUED for a queued request,
	 * FIO_Q_COMPLETED for a completed request, and FIO_Q_BUSY
	 * if we could queue no more at this point (you'd have to
	 * define ->commit() to handle that.
	 */
	return FIO_Q_COMPLETED;
	/* [한국어] 동기 완료로 보고 — 코어는 이 io_u를 바로 완료 처리하고
	 * io_u_queued_complete() 없이 put_io_u()로 바로 반납한다. */
}

/*
 * The ->prep() function is called for each io_u prior to being submitted
 * with ->queue(). This hook allows the io engine to perform any
 * preparatory actions on the io_u, before being submitted. Not required.
 */
/*
 * [한국어]
 * fio_skeleton_prep - queue() 직전에 io_u를 사전 가공 (선택적).
 *
 * @td: 잡 컨텍스트.
 * @io_u: 곧 queue()에 전달될 I/O 유닛.
 * @return: 0 성공, 음수 에러 (에러 시 코어는 이 io_u를 중단).
 *
 * 실제 엔진에서는 iovec 구성, iocb(Linux AIO) 초기화, io_uring SQE 세팅,
 * NVMe 명령 구조체 작성 등 "제출 직전 가공"을 여기서 수행한다.
 * 실행 컨텍스트: 잡 스레드 do_io() 루프 내, get_io_u() 직후.
 *
 * 호출 체인:
 *   do_io() → td_io_prep() → [fio_skeleton_prep()] → (이후 td_io_queue)
 */
static int fio_skeleton_prep(struct thread_data *td, struct io_u *io_u)
{
	return 0; /* [한국어] 사전 가공 없음 — 스켈레톤이므로 성공만 반환. */
}

/*
 * The init function is called once per thread/process, and should set up
 * any structures that this io engine requires to keep track of io. Not
 * required.
 */
/*
 * [한국어]
 * fio_skeleton_init - 잡 스레드 시작 시 1회, 엔진 전용 상태를 초기화.
 *
 * @td: 잡 컨텍스트. 엔진 상태는 td->io_ops_data에 malloc한 포인터로 저장하는 관례.
 * @return: 0 성공, 음수 실패 (실패 시 잡이 시작되지 않음).
 *
 * 실제 엔진에서는 라이브러리 컨텍스트 생성(예: io_uring_queue_init, rados_connect,
 * rbd_open 등), 완료 큐 배열 할당, 소켓 열기 등을 수행한다. cleanup()과 쌍을 이룬다.
 * 실행 컨텍스트: 잡 스레드 시작 직후 (thread_main 초반), do_io 루프 진입 전.
 *
 * 호출 체인:
 *   thread_main() → td_io_init() → [fio_skeleton_init()]
 */
static int fio_skeleton_init(struct thread_data *td)
{
	return 0; /* [한국어] 초기화할 상태 없음 — 성공 리턴. */
}

/*
 * This is paired with the ->init() function and is called when a thread is
 * done doing io. Should tear down anything setup by the ->init() function.
 * Not required.
 */
/*
 * [한국어]
 * fio_skeleton_cleanup - init()이 할당한 자원을 해제.
 *
 * @td: 잡 컨텍스트.
 *
 * 실행 컨텍스트: 잡 스레드 종료 직전 (thread_main 후반).
 * init()과 쌍을 이루므로 할당 순서의 역순으로 해제해야 누수/이중해제 방지 가능.
 *
 * 호출 체인:
 *   thread_main() → td_io_close/cleanup 경로 → [fio_skeleton_cleanup()]
 */
static void fio_skeleton_cleanup(struct thread_data *td)
{
	/* [한국어] 해제할 자원 없음 — 스켈레톤이므로 본문 비어 있음. */
}

/*
 * Hook for opening the given file. Unless the engine has special
 * needs, it usually just provides generic_open_file() as the handler.
 */
/*
 * [한국어]
 * fio_skeleton_open - fio_file을 엔진 관점에서 열기.
 *
 * @td: 잡 컨텍스트.
 * @f:  열어야 할 fio_file (파일/디바이스/네트워크 엔드포인트 등을 추상화).
 * @return: 0 성공, 음수 실패.
 *
 * 기본 구현(generic_open_file)은 POSIX open(2)을 사용해 f->fd를 채운다.
 * 네트워크 엔진(rados, rbd, rdma 등)은 이 훅에서 연결을 수립하므로 자체 구현을 사용한다.
 *
 * 호출 체인:
 *   thread_main() → td_io_open_file() → [fio_skeleton_open()] → generic_open_file() → open(2)
 */
static int fio_skeleton_open(struct thread_data *td, struct fio_file *f)
{
	return generic_open_file(td, f); /* [한국어] 기본 파일 열기(filesetup.c) 사용 — open(2) 기반 fd 획득. */
}

/*
 * Hook for closing a file. See fio_skeleton_open().
 */
/*
 * [한국어]
 * fio_skeleton_close - fio_file을 엔진 관점에서 닫기.
 *
 * @td: 잡 컨텍스트.
 * @f:  닫을 fio_file.
 * @return: 0 성공, 음수 실패.
 *
 * 호출 체인:
 *   thread_main() → td_io_close_file() → [fio_skeleton_close()] → generic_close_file() → close(2)
 */
static int fio_skeleton_close(struct thread_data *td, struct fio_file *f)
{
	return generic_close_file(td, f); /* [한국어] 기본 close(2) 기반 닫기 위임. */
}

/*
 * Hook for getting the zoned model of a zoned block device for zonemode=zbd.
 * The zoned model can be one of (see zbd_types.h):
 * - ZBD_NONE: regular block device (zone emulation will be used)
 * - ZBD_HOST_AWARE: host aware zoned block device
 * - ZBD_HOST_MANAGED: host managed zoned block device
 */
/*
 * [한국어]
 * fio_skeleton_get_zoned_model - 디바이스의 ZBD(Zoned Block Device) 모델 조회.
 *
 * @td: 잡 컨텍스트.
 * @f:  질의할 파일/디바이스.
 * @model: out — ZBD_NONE/HOST_AWARE/HOST_MANAGED 중 하나를 기록.
 * @return: 0 성공.
 *
 * ZBD는 NVMe ZNS 또는 SMR HDD처럼 존(zone) 단위로 순차 쓰기를 강제하는 디바이스 모델이다.
 * zonemode=zbd 옵션이 켜진 잡에서 fio 코어가 zbd 로직을 적용할지 결정하기 위해 호출한다.
 *
 * 호출 체인:
 *   init_zoned_block_device() → [fio_skeleton_get_zoned_model()]
 */
static int fio_skeleton_get_zoned_model(struct thread_data *td,
			struct fio_file *f, enum zbd_zoned_model *model)
{
	*model = ZBD_NONE;   /* [한국어] 스켈레톤은 존 개념 없음 → 일반 블록 디바이스로 보고 (fio가 필요하면 존을 에뮬레이션). */
	return 0;            /* [한국어] 성공. */
}

/*
 * Hook called for getting zone information of a ZBD_HOST_AWARE or
 * ZBD_HOST_MANAGED zoned block device. Up to @nr_zones zone information
 * structures can be reported using the array zones for zones starting from
 * @offset. The number of zones reported must be returned or a negative error
 * code in case of error.
 */
/*
 * [한국어]
 * fio_skeleton_report_zones - 지정 오프셋부터 nr_zones개의 존 정보를 보고.
 *
 * @td: 잡 컨텍스트.
 * @f:  대상 ZBD 디바이스.
 * @offset: 조회 시작 바이트 오프셋.
 * @zones: out — struct zbd_zone 배열에 존 정보 기록.
 * @nr_zones: 배열 크기 (최대 보고 가능 개수).
 * @return: 실제 보고한 존 개수 (>=0), 음수 에러.
 *
 * 실제 엔진에서는 blkzoned ioctl(BLKREPORTZONE) 또는 NVMe ZNS 명령(Report Zones)으로 조회.
 */
static int fio_skeleton_report_zones(struct thread_data *td, struct fio_file *f,
				     uint64_t offset, struct zbd_zone *zones,
				     unsigned int nr_zones)
{
	return 0; /* [한국어] ZBD 미지원 → 0개 보고 (ZBD_NONE이므로 호출되지 않는 것이 정상). */
}

/*
 * Hook called for resetting the write pointer position of zones of a
 * ZBD_HOST_AWARE or ZBD_HOST_MANAGED zoned block device. The write pointer
 * position of all zones in the range @offset..@offset + @length must be reset.
 */
/*
 * [한국어]
 * fio_skeleton_reset_wp - 지정 범위의 존들에 대해 Write Pointer를 초기 위치로 리셋.
 *
 * @td: 잡 컨텍스트.
 * @f:  대상 ZBD 디바이스.
 * @offset: 리셋 시작 바이트 오프셋 (존 경계 정렬).
 * @length: 리셋 길이 (존 단위로 정렬되어야 함).
 * @return: 0 성공, 음수 에러.
 *
 * 실제 엔진은 BLKRESETZONE ioctl 또는 NVMe ZNS Reset Zone 명령을 발행한다.
 * fio는 각 반복(iteration) 시작 시 존 WP를 초기화해 순차 쓰기 재시작을 가능케 한다.
 */
static int fio_skeleton_reset_wp(struct thread_data *td, struct fio_file *f,
				 uint64_t offset, uint64_t length)
{
	return 0; /* [한국어] 스켈레톤은 실제 디바이스가 없으므로 no-op 성공. */
}

/*
 * Hook called for getting the maximum number of open zones for a
 * ZBD_HOST_MANAGED zoned block device.
 * A @max_open_zones value set to zero means no limit.
 */
/*
 * [한국어]
 * fio_skeleton_get_max_open_zones - 동시 오픈 가능 존 수 조회.
 *
 * @td: 잡 컨텍스트.
 * @f:  대상 디바이스.
 * @max_open_zones: out — 최대 동시 오픈 존 수 (0 = 제한 없음).
 * @return: 0 성공.
 *
 * NVMe ZNS의 "Max Open Zones" / "Max Active Zones" 속성과 대응된다.
 */
static int fio_skeleton_get_max_open_zones(struct thread_data *td,
					   struct fio_file *f,
					   unsigned int *max_open_zones)
{
	return 0; /* [한국어] 기본값 유지 — 호출자가 *max_open_zones을 0으로 초기화해두는 관례. */
}

/*
 * Note that the structure is exported, so that fio can get it via
 * dlsym(..., "ioengine"); for (and only for) external engines.
 */
/*
 * [한국어] 외부 엔진의 진입점 심볼.
 * fio의 load_ioengine()이 dlopen(.so) → dlsym(handle, "ioengine")으로 이 전역을 찾는다.
 * 따라서 심볼명은 반드시 `ioengine`이어야 하며, static이 아닌 전역 가시성을 가져야 한다.
 * 이 구조체의 각 콜백 포인터가 fio 코어가 호출하는 "계약(contract)"이 된다.
 */
struct ioengine_ops ioengine = {
	.name		= "engine_name",                                /* [한국어] fio CLI `--ioengine=...`에서 사용될 엔진 이름 — 실제 배포 시 고유 이름으로 변경. */
	.version	= FIO_IOOPS_VERSION,                            /* [한국어] fio 코어와 이 엔진 간 ABI 버전. 불일치 시 로드 거부됨. */
	.init		= fio_skeleton_init,                            /* [한국어] 잡 시작 1회 초기화 훅. */
	.prep		= fio_skeleton_prep,                            /* [한국어] 각 io_u에 대한 사전 가공 훅. */
	.queue		= fio_skeleton_queue,                           /* [한국어] I/O 제출 훅 (필수 — 엔진의 핵심). */
	.getevents	= fio_skeleton_getevents,                       /* [한국어] 비동기 완료 수집 훅 (필수, 동기 엔진도 0 반환으로 구현). */
	.event		= fio_skeleton_event,                           /* [한국어] 인덱스 → 완료 io_u 매핑 훅 (필수). */
	.cleanup	= fio_skeleton_cleanup,                         /* [한국어] 잡 종료 시 자원 해제 훅. */
	.open_file	= fio_skeleton_open,                            /* [한국어] 파일/디바이스 열기 훅. */
	.close_file	= fio_skeleton_close,                           /* [한국어] 파일/디바이스 닫기 훅. */
	.get_zoned_model = fio_skeleton_get_zoned_model,            /* [한국어] ZBD 모델 조회 (zonemode=zbd 지원 시). */
	.report_zones	= fio_skeleton_report_zones,                /* [한국어] 존 정보 보고. */
	.reset_wp	= fio_skeleton_reset_wp,                        /* [한국어] 존 WP 리셋. */
	.get_max_open_zones = fio_skeleton_get_max_open_zones,      /* [한국어] 최대 동시 오픈 존 수. */
	.options	= options,                                       /* [한국어] 엔진 전용 옵션 테이블 포인터. */
	.option_struct_size	= sizeof(struct fio_skeleton_options),  /* [한국어] 옵션 구조체 크기 — 코어가 이 크기만큼 메모리를 할당해 잡마다 옵션을 보관. */
};
