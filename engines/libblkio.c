/*
 * [한국어 설명] libblkio 기반 통합 블록 I/O 엔진 (libblkio.c)
 *
 * === 파일의 역할 ===
 * libblkio(블록 I/O 추상화 라이브러리, https://gitlab.com/libblkio/libblkio)를 백엔드로
 * 사용하는 fio I/O 엔진 "libblkio"를 구현한다. libblkio는 io_uring, nvme-io_uring,
 * virtio-blk-vhost-user/vhost-vdpa, vfio-user 등 다양한 가상/물리 블록 장치 드라이버를
 * 단일 blkio/blkioq API로 추상화하는 유저스페이스 라이브러리이며, 이 엔진은 fio의
 * ioengine_ops 비동기 계약(queue → FIO_Q_QUEUED / getevents → event)을 libblkio의
 * blkioq_read/write(v) 제출과 blkioq_do_io + blkio_completion 수확에 1:1 매핑한다.
 * 완료 대기는 block(blocking blkioq_do_io) / eventfd(완료 eventfd read) / loop(비블록
 * busy-poll) 세 모드를 지원하며, hipri 옵션으로 폴링 전용 큐(blkio_get_poll_queue)를
 * 선택할 수 있다. 메모리는 mem-region-alignment에 맞춰 blkio_alloc_mem_region으로
 * 사전 할당하거나(blkio_map_mem_region), fio 코어 버퍼를 사후 등록한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 실행 시 --ioengine=libblkio로 선택되는 플러그인. register_ioengine(fio_init
 * constructor)으로 전역 엔진 목록(engine_list, ioengines.c)에 등록되며, backend.c의
 * 잡 실행 루프가 각 잡에서 다음 순서로 콜백을 호출한다:
 *   메인(원본 fio) 프로세스: setup() → 용량 조회용 임시 blkio 생성·destroy
 *   잡 스레드/프로세스:      init() → (iomem_alloc?) → post_init() → open_file() →
 *                            반복 { queue() → commit(없음, queue 내부에서 제출 위임) →
 *                                    getevents() → event() → put_io_u } → cleanup()
 * 프로세스 전역 blkio 인스턴스 하나(proc_state.b)에 여러 blkioq를 붙여 잡 간 공유하며,
 * 각 잡 스레드는 자신의 blkioq 핸들 하나와 mem_region 하나를 독점 소유한다. hipri 잡은
 * poll-queue(blkio_get_poll_queue)를, 일반 잡은 blkio_get_queue를 인덱스 순서대로 잡는다.
 * 실행 컨텍스트: fio 메인 프로세스(setup만) + 각 잡 스레드(use_thread=1) 혹은 포크된 잡
 * 프로세스(use_thread=0). proc_state는 pthread_mutex로 공용 카운터를 직렬화한다.
 *
 * === 타 모듈과의 연결 ===
 * 상단(호출자):
 *   - backend.c/ioengines.c: td_io_init/prep/queue/commit/getevents/event 디스패치.
 *   - options.c/optgroup.c: FIO_OPT_G_LIBBLKIO 옵션 그룹 파싱.
 *   - io_u.c: io_u 생명주기(free→prepped→in_flight→completed) — 본 엔진은 queue 시
 *             blkioq에 user_data=io_u를 넣고, completion.user_data로 역매핑한다.
 * 하단(피호출):
 *   - libblkio: blkio_create/connect/start/destroy, blkio_set_{bool,int,str},
 *               blkio_get_uint64(capacity/mem-region-alignment), blkio_{alloc,map,
 *               unmap,free}_mem_region, blkio_get_queue/get_poll_queue,
 *               blkioq_{read,write,readv,writev,discard,write_zeroes,flush},
 *               blkioq_do_io, blkioq_set_completion_fd_enabled/get_completion_fd,
 *               blkio_get_error_msg.
 *   - POSIX: pthread_mutex_{lock,unlock}, fcntl(F_GETFL/F_SETFL), read(eventfd),
 *            calloc/free, strdup, strcmp.
 *   - fio 유틸: get_next_str, strip_blank_front/end, log_err, for_each_td/end_for_each.
 * 데이터 흐름:
 *   io_u->xfer_buf (fio 버퍼) ─▶ blkio_mem_region(등록된 DMA 매핑 영역)
 *                              ─▶ blkioq_{read,write}(q, offset, buf, len, io_u)
 *                              ─▶ 백엔드 드라이버(io_uring SQE / virtio desc / vfio cmd)
 *                              ─▶ 완료 → blkioq_do_io → blkio_completion[]
 *                              ─▶ completion.user_data(io_u) + .ret(-errno) 복원.
 * 공유 상태:
 *   - proc_state: 프로세스 단위 (mutex, initted_threads, initted_hipri_threads, b).
 *   - first_threaded_subjob: use_thread=1인 잡들의 옵션 일치성 검증용 전역 캐시.
 *   - incompatible_threaded_subjob_options: setup()에서 불일치 감지 시 init()가 실패.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_blkio_set_props_from_str(): "k=v,k=v" 형식 문자열을 blkio_set_str로 반복 주입.
 * - possibly_null_strs_equal(): NULL을 포함해 두 문자열 동등성 판정 (옵션 호환성 검증용).
 * - total_threaded_subjobs(): for_each_td 순회로 hipri별 threaded 잡 개수 집계.
 * - fio_blkio_check_opt_compat(): 같은 프로세스 공유 잡 간 옵션 일관성 검증.
 * - fio_blkio_create_and_connect(): blkio_create + 속성 주입 + blkio_connect.
 * - fio_blkio_setup(): 메인 프로세스에서 임시 blkio로 capacity 조회 → files[0] 크기 확정.
 * - fio_blkio_init(): 잡 스레드에서 per-thread data 할당, 공용 blkio 생성·start, 큐 획득,
 *                      eventfd 필요시 블록모드 설정, 카운터 증가.
 * - fio_blkio_post_init(): iomem_alloc이 호출되지 않았다면 fio 코어 버퍼를 mem_region
 *                           으로 사후 등록.
 * - fio_blkio_cleanup(): per-thread 해제, 마지막 잡이 blkio_destroy.
 * - fio_blkio_iomem_alloc()/_free(): blkio_alloc_mem_region으로 정렬된 버퍼 직접 할당.
 * - fio_blkio_queue(): ddir별로 blkioq_{read,write,readv,writev,discard,write_zeroes,
 *                       flush}를 호출 → 항상 FIO_Q_QUEUED 반환(비동기).
 * - fio_blkio_getevents(): wait_mode(block/eventfd/loop)별 완료 수확 루프.
 * - fio_blkio_event(): getevents가 수확한 completions[event]에서 io_u + error 복원.
 * - struct fio_blkio_data: 스레드 단위 큐/메모리 영역/iovec/completions.
 * - struct fio_blkio_options: libblkio 엔진 전용 옵션 (driver/path/num_entries/
 *                              queue_size/hipri/vectored/write_zeroes_on_trim/wait_mode/
 *                              force_enable_completion_eventfd/pre_{connect,start}_props).
 * - proc_state: 프로세스 단위 동기화 상태.
 */

/*
 * libblkio engine
 *
 * IO engine using libblkio to access various block I/O interfaces:
 * https://gitlab.com/libblkio/libblkio
 */

#include <assert.h>   /* [한국어] assert() — pthread_mutex_{lock,unlock}의 반환값 0 확인용 */
#include <errno.h>    /* [한국어] errno, ENOENT, ENOTSUP — libblkio 반환 코드 및 DDIR 미지원 처리 */
#include <stdbool.h>  /* [한국어] bool/true/false — has_mem_region, hipri 캐스팅 등에서 사용 */
#include <stddef.h>   /* [한국어] offsetof, size_t, NULL — fio_option.off1 오프셋 계산에 필수 */
#include <stdint.h>   /* [한국어] uint64_t — blkio_get_uint64(capacity / mem-region-alignment) 수신 */
#include <stdlib.h>   /* [한국어] calloc/free, strtol 계열 — per-thread 상태 할당/해제 */
#include <string.h>   /* [한국어] strcmp, strchr, strdup, strerror — 옵션 문자열 파싱과 errno 출력 */

#include <blkio.h>    /* [한국어] libblkio 공개 API — blkio/blkioq/blkio_completion/blkio_mem_region
                       *          및 blkio_create/connect/start/destroy, blkio_set_{bool,int,str},
                       *          blkio_get_uint64, blkioq_{read,write,readv,writev,discard,
                       *          write_zeroes,flush,do_io,set_completion_fd_enabled,
                       *          get_completion_fd}, blkio_get_error_msg 선언 */

#include "../fio.h"      /* [한국어] struct thread_data, struct io_u, ioengine_ops, fio_file,
                           *          FIO_Q_QUEUED/COMPLETED, DDIR_READ/WRITE/TRIM/SYNC,
                           *          fio_ro_check, io_u_log_error, log_err, for_each_td */
#include "../optgroup.h" /* [한국어] FIO_OPT_C_ENGINE, FIO_OPT_G_LIBBLKIO — 옵션 카테고리/그룹 */
#include "../options.h"  /* [한국어] struct fio_option, FIO_OPT_STR_STORE/INT/STR/STR_SET 등 */
#include "../parse.h"    /* [한국어] get_next_str, strip_blank_{front,end} — "k=v,k=v" 토큰 파싱 */

/* per-process state */
/* [한국어] 프로세스 단위로 공유되는 libblkio 인스턴스와 초기화 카운터.
 * - 같은 프로세스 내 여러 잡이 blkio 하나를 공유해 큐(blkioq) 여러 개를 사용하므로
 *   생성/파괴 시점을 참조 카운팅해야 한다.
 * - 뮤텍스는 init/cleanup/iomem_alloc/iomem_free 경로에서 카운터와 proc_state.b를 보호. */
static struct {
	pthread_mutex_t mutex;
	/* [한국어] proc_state의 모든 필드를 보호하는 프로세스 전역 뮤텍스.
	 * 설정자: PTHREAD_MUTEX_INITIALIZER로 정적 초기화(런타임 pthread_mutex_init 불필요).
	 * 읽는 자: fio_blkio_proc_lock/unlock가 잡 스레드들 간 직렬화를 제공.
	 * 값 범위: pthread glibc가 관리하는 불투명 상태.
	 * 동기화: 다중 잡 스레드 간 init/cleanup/iomem_alloc 경합을 직렬화. */

	int initted_threads;
	/* [한국어] proc_state.b를 공유하는, init()을 통과한 잡 스레드 수.
	 * 설정자: fio_blkio_init()의 성공 경로에서 ++, fio_blkio_cleanup()에서 --.
	 * 읽는 자: init()이 0일 때만 blkio_create/start 수행, cleanup()이 0이 될 때 blkio_destroy.
	 * 값 범위: 0 이상. iodepth와 무관, 프로세스 내 잡(subjob) 수.
	 * 동기화: proc_state.mutex 아래에서만 접근. */

	int initted_hipri_threads;
	/* [한국어] 위 카운터 중 hipri=1(폴링 큐 사용) 잡 스레드 수.
	 * 설정자: fio_blkio_init()이 options->hipri일 때 ++.
	 * 읽는 자: blkio_get_poll_queue 인덱스 계산, blkio_get_queue 인덱스에서도
	 *          (initted_threads - initted_hipri_threads)로 일반 큐 index 계산에 사용.
	 * 값 범위: 0 ≤ value ≤ initted_threads.
	 * 동기화: proc_state.mutex 아래에서만 접근. */

	struct blkio *b;
	/* [한국어] 프로세스 전역 공유 libblkio 인스턴스.
	 * 설정자: 첫 잡의 fio_blkio_init()이 fio_blkio_create_and_connect로 생성, blkio_start 호출.
	 * 읽는 자: 모든 잡 스레드의 queue/getevents/iomem_*이 blkio_get_queue, blkio_*_mem_region 호출.
	 * 값 범위: NULL(미초기화) 또는 유효한 libblkio 핸들.
	 * 동기화: 생성/파괴는 mutex 아래. I/O 경로의 blkioq_* 호출은 큐 단위라 비접촉(libblkio 보장). */
} proc_state = { PTHREAD_MUTEX_INITIALIZER, 0, 0, NULL };
/* [한국어] 정적 초기화: 뮤텍스는 PTHREAD_MUTEX_INITIALIZER, 카운터 0, blkio 핸들 NULL. */

/*
 * [한국어]
 * fio_blkio_proc_lock - proc_state 뮤텍스 획득.
 *
 * @return: 없음 (lock 실패 시 assert로 즉시 abort).
 *
 * proc_state.mutex는 프로세스 전역 blkio 핸들과 카운터를 보호한다. init()/cleanup()/
 * iomem_alloc()/iomem_free() 시작부에서 반드시 호출되어 잡 스레드들 간 경쟁을 직렬화한다.
 * assert(ret==0)로 PTHREAD_MUTEX_INITIALIZER 사용 실패(예: EINVAL, EDEADLK)를 드러낸다.
 *
 * 호출 체인: fio_blkio_init/cleanup/iomem_alloc/iomem_free → [이 함수] → pthread_mutex_lock
 */
static void fio_blkio_proc_lock(void) {
	int ret;                                          /* [한국어] pthread_mutex_lock 반환값 저장 */
	ret = pthread_mutex_lock(&proc_state.mutex);      /* [한국어] 프로세스 전역 뮤텍스 획득 — 블록될 수 있음 */
	assert(ret == 0);                                 /* [한국어] 정상 획득 보장; EINVAL/EDEADLK 시 abort */
}

/*
 * [한국어]
 * fio_blkio_proc_unlock - proc_state 뮤텍스 해제.
 *
 * @return: 없음.
 *
 * fio_blkio_proc_lock와 짝을 이루는 해제 헬퍼. 반환 코드가 0이 아니면(예: 소유하지 않은
 * 뮤텍스 unlock) assert로 abort한다.
 *
 * 호출 체인: 각 콜백의 종료 경로 → [이 함수] → pthread_mutex_unlock
 */
static void fio_blkio_proc_unlock(void) {
	int ret;                                          /* [한국어] pthread_mutex_unlock 반환값 */
	ret = pthread_mutex_unlock(&proc_state.mutex);    /* [한국어] 뮤텍스 해제 — 대기 중인 스레드 1명 깨움 */
	assert(ret == 0);                                 /* [한국어] unlock 실패 감지 */
}

/* per-thread state */
/* [한국어] libblkio 엔진의 스레드(잡) 단위 내부 상태 구조체.
 * td->io_ops_data에 저장되며, init()에서 할당·cleanup()에서 해제된다.
 * 한 잡 스레드는 이 구조체 하나만 소유하며 다른 스레드와 공유하지 않는다. */
struct fio_blkio_data {
	struct blkioq *q;
	/* [한국어] 이 잡 스레드 전용 libblkio I/O 큐 핸들.
	 * 설정자: fio_blkio_init()이 blkio_get_queue 또는 blkio_get_poll_queue로 획득.
	 * 읽는 자: queue()에서 blkioq_{read,write,...} 제출, getevents()에서 blkioq_do_io 호출.
	 * 값 범위: libblkio가 반환한 유효 큐 포인터 (NULL 아님).
	 * 동기화: 단일 잡 스레드가 소유하므로 별도 락 없음 — libblkio는 큐 단위로 외부 직렬화 전제. */

	int completion_fd;
	/* [한국어] 완료 eventfd 파일 디스크립터.
	 * 설정자: init()이 wait_mode=EVENTFD 또는 force_enable_completion_eventfd일 때
	 *          blkioq_get_completion_fd로 획득 후 O_NONBLOCK 해제.
	 * 읽는 자: getevents()의 EVENTFD 경로에서 read()로 완료 개수 수신.
	 * 값 범위: -1(미사용) 또는 유효한 blocking fd.
	 * 동기화: 단일 잡 스레드 소유. */

	bool has_mem_region;
	/* [한국어] iomem_alloc()이 mem_region을 할당·매핑했는지 여부.
	 * 설정자: iomem_alloc 성공 시 true, iomem_free 시 false.
	 * 읽는 자: post_init()이 false면 td->orig_buffer를 mem_region으로 사후 등록.
	 *          iomem_free()가 true일 때만 unmap/free 수행.
	 * 값 범위: true/false. 초기값(calloc) 0 = false.
	 * 동기화: 단일 잡 스레드 소유. */

	struct blkio_mem_region mem_region;
	/* [한국어] libblkio에 등록된 DMA/매핑 가능 메모리 영역 서술자.
	 * 설정자: blkio_alloc_mem_region이 .addr/.len/.fd를 채움.
	 * 읽는 자: blkio_map_mem_region/blkio_unmap_mem_region/blkio_free_mem_region 인자.
	 * 값 범위: libblkio 내부 포맷 (.addr은 사용자 버퍼 선두, .len은 정렬 상향된 크기).
	 * 동기화: 단일 잡 스레드 소유. has_mem_region==true일 때만 의미 있음. */

	struct iovec *iovecs;
	/* [한국어] 벡터 I/O(readv/writev) 요청용 iovec 배열.
	 * 설정자: init()이 calloc(iodepth, sizeof(iovec))로 할당.
	 * 읽는 자: queue()가 options->vectored일 때 io_u->index 슬롯을 채워 blkioq_{readv,writev}에 전달.
	 * 값 범위: iodepth 크기의 배열. 각 iovec는 io_u 하나당 한 개.
	 * 동기화: 단일 잡 스레드 소유. io_u->index로 인덱싱되어 중첩 없음. */

	struct blkio_completion *completions;
	/* [한국어] blkioq_do_io가 완료 정보를 채워 넣는 배열.
	 * 설정자: init()이 calloc(iodepth, sizeof(blkio_completion))로 할당.
	 * 읽는 자: getevents()가 호출 결과로 n개 채움, event()가 completions[event].user_data(=io_u)
	 *          와 .ret(-errno)로 io_u 복원.
	 * 값 범위: iodepth 크기. 각 엔트리 = {user_data, ret} (libblkio 스펙).
	 * 동기화: 단일 잡 스레드 소유. getevents/event 페어로 소비. */
};

/* [한국어] 완료 대기 전략 enum. libblkio_wait_mode 옵션의 3가지 동작 모드. */
enum fio_blkio_wait_mode {
	FIO_BLKIO_WAIT_MODE_BLOCK,
	/* [한국어] 기본값. blkioq_do_io(min=min, max=max, timeout=NULL)로 min개까지 블록 대기.
	 * libblkio 내부가 epoll 또는 드라이버별 대기 메커니즘을 활용. */

	FIO_BLKIO_WAIT_MODE_EVENTFD,
	/* [한국어] blkioq_do_io(min=0, ...)로 non-blocking 수확 후 부족분을 완료 eventfd의
	 * blocking read()로 대기. hipri와 호환 불가(폴링 큐는 eventfd 없음). */

	FIO_BLKIO_WAIT_MODE_LOOP,
	/* [한국어] blkioq_do_io(min=0, ...)를 busy-loop으로 반복 호출. CPU 100% 소비로
	 * 최저 지연을 달성 (nvme-io_uring 폴링 전용). */
};

/* [한국어] libblkio 엔진 전용 옵션 구조체. thread_options.eo로 참조되며 옵션 파서가 채움. */
struct fio_blkio_options {
	void *pad;
	/* [한국어] fio 옵션 구조체 첫 필드 정렬 패딩(옵션 off1 계산 시 thread_options와 정렬).
	 * 설정자: 사용 안 함. 읽는 자: 없음. 값 범위: 의미 없음. */

	char *driver;
	/* [한국어] libblkio 드라이버 이름 (예: "io_uring", "nvme-io_uring",
	 *          "virtio-blk-vhost-user", "virtio-blk-vhost-vdpa", "vfio-user").
	 * 설정자: --libblkio_driver=NAME 옵션으로 fio 옵션 파서가 strdup.
	 * 읽는 자: fio_blkio_create_and_connect가 blkio_create(driver, &b)에 사용.
	 * 값 범위: NULL이면 오류(init 실패). libblkio가 인식하는 드라이버 이름. */

	char *path;
	/* [한국어] 드라이버별 장치/소켓 경로 ("path" 속성).
	 * 설정자: --libblkio_path=PATH.
	 * 읽는 자: blkio_set_str(b, "path", path).
	 * 값 범위: NULL이면 "path" 속성 미설정(드라이버가 기본값 사용). */

	char *pre_connect_props;
	/* [한국어] blkio_connect() 호출 전에 주입할 임의 속성 ("k=v,k=v" 형식).
	 * 설정자: --libblkio_pre_connect_props="k1=v1,k2=v2".
	 * 읽는 자: fio_blkio_set_props_from_str가 순회하며 blkio_set_str. */

	int num_entries;
	/* [한국어] libblkio "num-entries" 속성 (큐당 SQE/요청 슬롯 수).
	 * 설정자: --libblkio_num_entries=N.
	 * 읽는 자: blkio_set_int(b, "num-entries", ...). 0이면 미설정(기본값 사용). */

	int queue_size;
	/* [한국어] libblkio "queue-size" 속성 (큐 버퍼 크기).
	 * 설정자: --libblkio_queue_size=N. 0이면 미설정. */

	char *pre_start_props;
	/* [한국어] blkio_start() 호출 전에 주입할 임의 속성.
	 * 설정자: --libblkio_pre_start_props. 읽는 자: set_props_from_str. */

	unsigned int hipri;
	/* [한국어] 폴링 큐 사용 여부 (FIO_OPT_STR_SET, 존재하면 1).
	 * 설정자: --hipri. 읽는 자: init()이 blkio_get_poll_queue로 큐 획득 결정,
	 *          num-poll-queues 속성 설정에도 반영. EVENTFD 모드와 배타적. */

	unsigned int vectored;
	/* [한국어] readv/writev 사용 여부 (1이면 벡터 경로).
	 * 설정자: --libblkio_vectored. 읽는 자: queue()가 blkioq_readv/writev vs read/write 선택. */

	unsigned int write_zeroes_on_trim;
	/* [한국어] DDIR_TRIM을 blkioq_write_zeroes로 매핑할지 여부 (기본: blkioq_discard).
	 * 설정자: --libblkio_write_zeroes_on_trim. 읽는 자: queue()의 DDIR_TRIM 분기. */

	enum fio_blkio_wait_mode wait_mode;
	/* [한국어] 완료 대기 전략.
	 * 설정자: --libblkio_wait_mode={block,eventfd,loop}. 기본 block.
	 * 읽는 자: init()이 eventfd 준비 여부, getevents()가 수확 루프 분기. */

	unsigned int force_enable_completion_eventfd;
	/* [한국어] 완료 eventfd를 실제로 쓰지 않더라도 강제 활성화 (블록 모드에서도).
	 * 설정자: --libblkio_force_enable_completion_eventfd.
	 * 읽는 자: init()이 true면 blkioq_set_completion_fd_enabled(true)+blocking fd 준비.
	 * 주의: 드라이버 성능 저하 가능, hipri와 배타. */
};

/* [한국어] libblkio 엔진의 옵션 테이블. ioengine_ops.options로 등록되어
 * fio 옵션 파서가 --libblkio_* 및 --hipri 옵션을 struct fio_blkio_options 필드에 매핑한다.
 * off1 = offsetof로 타겟 필드 오프셋 지정, category=ENGINE/group=LIBBLKIO로 분류된다. */
static struct fio_option options[] = {
	{
		.name	= "libblkio_driver",
		.lname	= "libblkio driver name",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct fio_blkio_options, driver),
		.help	= "Name of the driver to be used by libblkio",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_path",
		.lname	= "libblkio \"path\" property",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct fio_blkio_options, path),
		.help	= "Value to set the \"path\" property to",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_pre_connect_props",
		.lname	= "Additional properties to be set before blkio_connect()",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct fio_blkio_options, pre_connect_props),
		.help	= "",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_num_entries",
		.lname	= "libblkio \"num-entries\" property",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct fio_blkio_options, num_entries),
		.help	= "Value to set the \"num-entries\" property to",
		.minval	= 1,
		.interval = 1,
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_queue_size",
		.lname	= "libblkio \"queue-size\" property",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct fio_blkio_options, queue_size),
		.help	= "Value to set the \"queue-size\" property to",
		.minval	= 1,
		.interval = 1,
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_pre_start_props",
		.lname	= "Additional properties to be set before blkio_start()",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct fio_blkio_options, pre_start_props),
		.help	= "",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "hipri",
		.lname	= "Use poll queues",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct fio_blkio_options, hipri),
		.help	= "Use poll queues",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_vectored",
		.lname	= "Use blkioq_{readv,writev}()",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct fio_blkio_options, vectored),
		.help	= "Use blkioq_{readv,writev}() instead of blkioq_{read,write}()",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_write_zeroes_on_trim",
		.lname	= "Use blkioq_write_zeroes() for TRIM",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct fio_blkio_options,
				   write_zeroes_on_trim),
		.help	= "Use blkioq_write_zeroes() for TRIM instead of blkioq_discard()",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_wait_mode",
		.lname	= "How to wait for completions",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct fio_blkio_options, wait_mode),
		.help	= "How to wait for completions",
		.def	= "block",
		.posval = {
			  { .ival = "block",
			    .oval = FIO_BLKIO_WAIT_MODE_BLOCK,
			    .help = "Blocking blkioq_do_io()",
			  },
			  { .ival = "eventfd",
			    .oval = FIO_BLKIO_WAIT_MODE_EVENTFD,
			    .help = "Blocking read() on the completion eventfd",
			  },
			  { .ival = "loop",
			    .oval = FIO_BLKIO_WAIT_MODE_LOOP,
			    .help = "Busy loop with non-blocking blkioq_do_io()",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name	= "libblkio_force_enable_completion_eventfd",
		.lname	= "Force enable the completion eventfd, even if unused",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct fio_blkio_options,
				   force_enable_completion_eventfd),
		.help	= "This can impact performance",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBBLKIO,
	},
	{
		.name = NULL,
	},
};

/*
 * [한국어]
 * fio_blkio_set_props_from_str - "k1=v1,k2=v2" 문자열을 blkio_set_str로 반복 주입.
 *
 * @b: 설정 대상 libblkio 인스턴스 (connect/start 전 혹은 후 시점 상관없이 속성 key에 따라).
 * @opt_name: 에러 메시지용 fio 옵션 이름 ("libblkio_pre_connect_props" 등).
 * @str: 사용자 지정 "name=value,name=value" 문자열. NULL 허용(무동작).
 * @return: 0 성공, 1 파싱/설정 실패 (log_err로 세부 메시지 출력).
 *
 * pre_connect_props/pre_start_props 옵션 처리용 유틸. get_next_str는 포인터를 전진시키며
 * 쉼표로 구분된 토큰을 돌려주는 fio 공용 파서(parse.c). 입력을 mutate하므로 strdup 복제.
 * 각 토큰을 '='로 분리하고 앞뒤 공백을 제거한 뒤 blkio_set_str 호출.
 *
 * 호출 체인: fio_blkio_create_and_connect → [이 함수] → get_next_str/strchr/strip_blank_*/blkio_set_str
 */
static int fio_blkio_set_props_from_str(struct blkio *b, const char *opt_name,
					const char *str) {
	int ret = 0;                                       /* [한국어] 성공(0)/실패(1) 누적 — early return 대체용 */
	char *new_str, *name, *value;                      /* [한국어] new_str: strdup 복제본 / name: 현재 토큰 / value: '=' 이후 부분 */

	if (!str)                                          /* [한국어] 옵션 미지정 시 무동작(옵션적 프로퍼티이므로 정상) */
		return 0;

	/* iteration can mutate string, so copy it */
	new_str = strdup(str);                             /* [한국어] get_next_str가 문자열을 파괴적으로 파싱하므로 사본 생성 */
	if (!new_str) {                                    /* [한국어] malloc 실패 — OOM 보고 */
		log_err("fio: strdup() failed\n");
		return 1;
	}

	/* iterate over property name-value pairs */
	while ((name = get_next_str(&new_str))) {          /* [한국어] fio 파서(parse.c): 쉼표 분리 토큰 반환, 포인터 전진; NULL 반환 시 종료 */
		/* split into property name and value */
		value = strchr(name, '=');                 /* [한국어] 'name=value' 경계 탐색 */
		if (!value) {                              /* [한국어] '=' 없으면 형식 오류 */
			log_err("fio: missing '=' in option %s\n", opt_name);
			ret = 1;
			break;
		}

		*value = '\0';                             /* [한국어] '='를 NUL로 바꿔 name을 분리 */
		++value;                                   /* [한국어] value는 '=' 다음 문자부터 */

		/* strip whitespace from property name */
		strip_blank_front(&name);                  /* [한국어] 이름 선두 공백 제거 (포인터 전진) */
		strip_blank_end(name);                     /* [한국어] 이름 말미 공백을 NUL로 치환 */

		if (name[0] == '\0') {                     /* [한국어] 공백만 있던 이름 — 형식 오류 */
			log_err("fio: empty property name in option %s\n",
				opt_name);
			ret = 1;
			break;
		}

		/* strip whitespace from property value */
		strip_blank_front(&value);                 /* [한국어] 값 선두 공백 제거 */
		strip_blank_end(value);                    /* [한국어] 값 말미 공백 제거 */

		/* set property */
		if (blkio_set_str(b, name, value) != 0) {  /* [한국어] libblkio 속성 주입; 드라이버가 인식 못 하면 오류 */
			log_err("fio: error setting property '%s' to '%s': %s\n",
				name, value, blkio_get_error_msg()); /* [한국어] libblkio 마지막 에러 메시지 취득 */
			ret = 1;
			break;
		}
	}

	free(new_str);                                     /* [한국어] strdup 사본 해제 (get_next_str가 내부에서 포인터를 소비했어도 원본 base는 보존됨) */
	return ret;
}

/*
 * Log the failure of a libblkio function.
 *
 * `(void)func` is to ensure `func` exists and prevent typos
 */
/* [한국어] libblkio 함수 실패 로깅 매크로.
 * - `(void)func`로 심볼 존재 확인(오타 시 컴파일 오류 유도) — 함수를 호출하지는 않음.
 * - #func로 스트링화해 함수명 포함 에러 메시지 출력.
 * - blkio_get_error_msg()는 TLS 기반 마지막 에러 문자열을 반환(libblkio 공용). */
#define fio_blkio_log_err(func) \
	({ \
		(void)func; \
		log_err("fio: %s() failed: %s\n", #func, \
			blkio_get_error_msg()); \
	})

/*
 * [한국어]
 * possibly_null_strs_equal - NULL을 포함한 두 문자열의 동등성 판정.
 *
 * @a, @b: 비교 대상. 어느 쪽이든 NULL 가능.
 * @return: 둘 다 NULL이거나 둘 다 non-NULL이고 strcmp==0이면 true.
 *
 * fio_blkio_check_opt_compat에서 path/pre_connect_props/pre_start_props처럼 미지정(NULL)이
 * 허용되는 문자열 옵션의 일관성 검증에 사용된다. strcmp는 NULL에 대해 UB이므로 가드 필요.
 *
 * 호출 체인: fio_blkio_check_opt_compat → [이 함수] → strcmp
 */
static bool possibly_null_strs_equal(const char *a, const char *b)
{
	return (!a && !b) || (a && b && strcmp(a, b) == 0); /* [한국어] 양쪽 NULL 허용, 한쪽만 NULL이면 false */
}

/*
 * [한국어]
 * total_threaded_subjobs - 전체 잡 목록에서 libblkio+thread+지정된 hipri를 만족하는 잡 수.
 *
 * @hipri: true면 hipri=1 잡 수, false면 hipri=0 잡 수를 집계.
 * @return: 조건을 만족하는 잡 개수 (0 이상).
 *
 * init()에서 num-queues/num-poll-queues libblkio 속성 값을 결정할 때 사용된다.
 * use_thread=1(같은 프로세스 공유) 잡만 세는 이유: 같은 proc_state.b에 붙을 큐 수를
 * 미리 알아야 blkio_start 전에 num-(poll-)queues를 맞게 설정할 수 있기 때문.
 * for_each_td/end_for_each는 fio 코어가 제공하는 전역 thread_data 순회 매크로.
 *
 * 호출 체인: fio_blkio_init → [이 함수] → for_each_td 매크로
 */
static int total_threaded_subjobs(bool hipri)
{
	int count = 0;                                            /* [한국어] 일치 잡 누적 카운터 */

	for_each_td(td) {                                         /* [한국어] 모든 thread_data td 순회 (매크로) */
		const struct fio_blkio_options *options = td->eo; /* [한국어] 잡 전용 옵션 포인터 */
		if (strcmp(td->o.ioengine, "libblkio") == 0 &&    /* [한국어] 엔진이 libblkio인지 */
		    td->o.use_thread && (bool)options->hipri == hipri) /* [한국어] thread 모드 + hipri 일치 */
			++count;                                  /* [한국어] 조건 충족 시 집계 */
	} end_for_each();                                         /* [한국어] for_each_td 매크로 종료 */

	return count;
}

/* [한국어] 첫 threaded subjob의 옵션 스냅샷 — 이후 잡들과 호환성 비교에 사용.
 * 잡들이 같은 프로세스 내 단일 blkio를 공유해야 하므로 driver/path/num_entries 등이
 * 모든 잡에서 동일해야 한다. 불일치 시 setup()이 incompatible 플래그를 세워 init 실패. */
static struct {
	bool set_up;
	/* [한국어] 스냅샷이 기록되었는지 여부.
	 * 설정자: fio_blkio_check_opt_compat가 첫 threaded 잡에서 true로.
	 * 읽는 자: 동일 함수가 두 번째 잡부터 비교 경로 분기. 값: false 초기 → true. */

	bool direct;
	/* [한국어] 첫 잡의 td->o.odirect (O_DIRECT 사용 여부) 스냅샷.
	 * 설정자/읽는 자: check_opt_compat 동일 함수 내. */

	struct fio_blkio_options opts;
	/* [한국어] 첫 잡의 libblkio 옵션 전체 복사본. 이후 잡과 필드별 비교. */
} first_threaded_subjob = { 0 };                                   /* [한국어] zero-initialize (set_up=false) */

/*
 * [한국어]
 * fio_blkio_log_opt_compat_err - 옵션 불일치 에러 로그 헬퍼.
 *
 * @option_name: 불일치 감지된 옵션 이름(예 "libblkio_driver").
 * @return: 없음.
 *
 * 같은 프로세스를 공유하는 threaded 잡들은 libblkio 설정을 모두 공유해야 하므로
 * 옵션이 다르면 어느 값이 승자가 되어야 할지 결정 불가 — 실패를 권장하는 메시지.
 *
 * 호출 체인: fio_blkio_check_opt_compat → [이 함수] → log_err
 */
static void fio_blkio_log_opt_compat_err(const char *option_name)
{
	log_err("fio: jobs using engine libblkio and sharing a process must agree on the %s option\n",
		option_name);                                      /* [한국어] stderr+log에 불일치 옵션 고지 */
}

/*
 * [한국어]
 * fio_blkio_check_opt_compat - 같은 프로세스 공유 잡 간 옵션 일관성 검증.
 *
 * @td: 검사 대상 잡의 thread_data.
 * @return: 0 호환(또는 thread 모드 아님), 1 비호환 (메인 프로세스가 fio_blkio_setup에서
 *           incompatible_threaded_subjob_options 전역을 set → init()이 실패 처리).
 *
 * 첫 번째 threaded 잡 진입 시 옵션 스냅샷을 first_threaded_subjob에 저장. 두 번째 잡부터는
 * 동일 항목을 필드별 비교하여 driver/path/num_entries/queue_size/pre_connect_props/
 * pre_start_props/odirect 중 하나라도 다르면 에러. use_thread=0(fork) 잡은 프로세스가 다르므로
 * blkio도 독립 생성 가능해 이 검사에서 면제된다. 실행 컨텍스트: fio 메인 프로세스의 setup 콜백.
 *
 * 호출 체인: fio_blkio_setup → [이 함수] → strcmp/possibly_null_strs_equal
 */
static int fio_blkio_check_opt_compat(struct thread_data *td)
{
	const struct fio_blkio_options *options = td->eo, *prev_options; /* [한국어] 현재 잡 옵션 + 비교 기준 */

	if (!td->o.use_thread)                                    /* [한국어] fork 잡은 프로세스 격리 — 검사 불필요 */
		return 0; /* subjob doesn't use 'thread' */

	if (!first_threaded_subjob.set_up) {                      /* [한국어] 첫 threaded 잡 — 스냅샷 기록 */
		/* first subjob using 'thread', store options for later */
		first_threaded_subjob.set_up	= true;           /* [한국어] 이후 잡이 비교 경로 진입하도록 표시 */
		first_threaded_subjob.direct	= td->o.odirect;  /* [한국어] O_DIRECT 여부 스냅샷 */
		first_threaded_subjob.opts	= *options;       /* [한국어] 옵션 구조체 값 복사 (포인터는 공유됨에 주의) */
		return 0;
	}

	/* not first subjob using 'thread', check option compatibility */
	prev_options = &first_threaded_subjob.opts;               /* [한국어] 기준 스냅샷 참조 */

	if (td->o.odirect != first_threaded_subjob.direct) {      /* [한국어] direct/buffered 모드 일치? */
		fio_blkio_log_opt_compat_err("direct/buffered");
		return 1;
	}

	if (strcmp(options->driver, prev_options->driver) != 0) { /* [한국어] driver는 필수라 NULL 검사 없이 strcmp */
		fio_blkio_log_opt_compat_err("libblkio_driver");
		return 1;
	}

	if (!possibly_null_strs_equal(options->path, prev_options->path)) { /* [한국어] path는 선택적 — NULL 안전 비교 */
		fio_blkio_log_opt_compat_err("libblkio_path");
		return 1;
	}

	if (!possibly_null_strs_equal(options->pre_connect_props,
				      prev_options->pre_connect_props)) { /* [한국어] pre-connect 속성 문자열 비교 */
		fio_blkio_log_opt_compat_err("libblkio_pre_connect_props");
		return 1;
	}

	if (options->num_entries != prev_options->num_entries) {  /* [한국어] 큐 엔트리 수는 blkio 전역 속성이라 일치 필요 */
		fio_blkio_log_opt_compat_err("libblkio_num_entries");
		return 1;
	}

	if (options->queue_size != prev_options->queue_size) {    /* [한국어] queue-size 역시 전역 속성 */
		fio_blkio_log_opt_compat_err("libblkio_queue_size");
		return 1;
	}

	if (!possibly_null_strs_equal(options->pre_start_props,
				      prev_options->pre_start_props)) { /* [한국어] pre-start 속성 일치 확인 */
		fio_blkio_log_opt_compat_err("libblkio_pre_start_props");
		return 1;
	}

	return 0;                                                 /* [한국어] 모든 항목 일치 */
}

/*
 * [한국어]
 * fio_blkio_create_and_connect - libblkio 인스턴스 생성 + 기본 속성 주입 + connect까지 수행.
 *
 * @td: 옵션/상태를 가져올 thread_data.
 * @out_blkio: 성공 시 생성된 blkio 핸들을 반환할 포인터.
 * @return: 0 성공, 1 실패 (실패 시 blkio_destroy로 정리).
 *
 * libblkio의 3단계 생명주기 (create → properties → connect → properties → start) 중
 * "connect까지"를 처리한다. connect 후에만 설정 가능한 num-entries/queue-size는 connect
 * 후에 주입한다. 메인 프로세스 setup()과 잡 스레드 init() 양쪽에서 호출된다 — setup은
 * capacity 조회용 임시 blkio를 만들고 곧 destroy, init은 proc_state.b를 영구 보관.
 *
 * 실행 컨텍스트: 메인 fio 프로세스(setup) 또는 첫 잡 스레드(init).
 * 에러 경로: 어떤 단계든 실패하면 err_blkio_destroy로 점프해 blkio_destroy 후 반환.
 *
 * 호출 체인: fio_blkio_setup / fio_blkio_init → [이 함수] → blkio_create/set_*/connect/
 *            fio_blkio_set_props_from_str
 */
static int fio_blkio_create_and_connect(struct thread_data *td,
					struct blkio **out_blkio)
{
	const struct fio_blkio_options *options = td->eo;         /* [한국어] 잡 전용 libblkio 옵션 */
	struct blkio *b;                                          /* [한국어] 생성 중인 libblkio 핸들 */
	int ret;                                                  /* [한국어] blkio_set_bool 반환값 (ENOENT 특수 처리용) */

	if (!options->driver) {                                   /* [한국어] driver 필수 검증 */
		log_err("fio: engine libblkio requires option libblkio_driver to be set\n");
		return 1;
	}

	if (blkio_create(options->driver, &b) != 0) {             /* [한국어] 드라이버별 blkio 객체 생성 (아직 연결 안됨) */
		fio_blkio_log_err(blkio_create);
		return 1;
	}

	/* don't fail if driver doesn't have a "direct" property */
	ret = blkio_set_bool(b, "direct", td->o.odirect);         /* [한국어] O_DIRECT 상당 속성 주입 시도 */
	if (ret != 0 && ret != -ENOENT) {                         /* [한국어] ENOENT(속성 없음)는 무시 — 일부 드라이버는 direct 개념 없음 */
		fio_blkio_log_err(blkio_set_bool);
		goto err_blkio_destroy;
	}

	if (blkio_set_bool(b, "read-only", read_only) != 0) {     /* [한국어] fio 전역 read_only 플래그를 libblkio에 전달 */
		fio_blkio_log_err(blkio_set_bool);
		goto err_blkio_destroy;
	}

	if (options->path) {                                      /* [한국어] path 옵션이 지정된 경우에만 속성 주입 */
		if (blkio_set_str(b, "path", options->path) != 0) {
			fio_blkio_log_err(blkio_set_str);
			goto err_blkio_destroy;
		}
	}

	if (fio_blkio_set_props_from_str(b, "libblkio_pre_connect_props",
					 options->pre_connect_props) != 0) /* [한국어] 사용자 지정 pre-connect 속성 주입 */
		goto err_blkio_destroy;

	if (blkio_connect(b) != 0) {                              /* [한국어] 드라이버 연결 (장치/소켓 오픈) */
		fio_blkio_log_err(blkio_connect);
		goto err_blkio_destroy;
	}

	if (options->num_entries != 0) {                          /* [한국어] 0이면 드라이버 기본값 사용 */
		if (blkio_set_int(b, "num-entries",
				  options->num_entries) != 0) {   /* [한국어] connect 후에만 설정 가능한 속성 */
			fio_blkio_log_err(blkio_set_int);
			goto err_blkio_destroy;
		}
	}

	if (options->queue_size != 0) {                           /* [한국어] 0이면 기본값 */
		if (blkio_set_int(b, "queue-size", options->queue_size) != 0) {
			fio_blkio_log_err(blkio_set_int);
			goto err_blkio_destroy;
		}
	}

	if (fio_blkio_set_props_from_str(b, "libblkio_pre_start_props",
					 options->pre_start_props) != 0) /* [한국어] start 직전 최종 속성 주입(num-queues 등은 호출자에서 별도) */
		goto err_blkio_destroy;

	*out_blkio = b;                                           /* [한국어] 성공 시 핸들 반출 */
	return 0;

err_blkio_destroy:
	blkio_destroy(&b);                                        /* [한국어] 부분적으로 생성된 blkio 정리 — libblkio가 NULL로 세팅 */
	return 1;
}

/* [한국어] 메인 프로세스 setup()이 각 잡을 돌며 옵션 호환성 검사를 수행하다 하나라도
 * 불일치를 발견하면 true로 세팅. 이후 각 잡 스레드의 init()이 이 플래그를 보고 일괄 실패.
 * 설정자: setup() / 읽는 자: init() / 동기화: setup은 메인에서 순차 호출이라 경쟁 없음. */
static bool incompatible_threaded_subjob_options = false;

/*
 * [한국어]
 * fio_blkio_setup - 메인 프로세스에서 장치 용량 조회 + 옵션 호환성 검사.
 *
 * @td: 설정 대상 잡의 thread_data.
 * @return: 0 성공, 1 실패 (옵션 불일치, hipri/eventfd 상호배타 위반, blkio 생성 실패,
 *           capacity 조회 실패).
 *
 * ioengine_ops.setup 콜백. 이 콜백은 잡이 실제로 실행될 프로세스/스레드가 아니라
 * 원본 fio 메인 프로세스에서 호출되어 장치 용량을 알아야 하는 fio 코어(crc/io_u
 * 범위 결정)에 td->files[0]->real_file_size를 전달한다. 따라서 임시 blkio를 만들고 곧
 * 파괴한다(init에서 다시 생성). hipri+EVENTFD / hipri+force_enable_completion_eventfd는
 * 구조적으로 충돌하므로 여기서 차단한다. assert(files_index==1)는 libblkio 엔진이
 * 잡당 단일 파일만 지원함을 강제.
 *
 * 실행 컨텍스트: fio 메인 프로세스(단일 스레드).
 * 호출 체인: backend.c(parse_options/init 후) → td_io_setup → [이 함수] →
 *            fio_blkio_check_opt_compat / fio_blkio_create_and_connect / blkio_get_uint64
 */
static int fio_blkio_setup(struct thread_data *td)
{
	const struct fio_blkio_options *options = td->eo;   /* [한국어] 옵션 참조 */
	struct blkio *b;                                    /* [한국어] 임시 blkio 핸들 */
	int ret = 0;                                        /* [한국어] 반환 코드 기본 성공 */
	uint64_t capacity;                                  /* [한국어] 장치 용량(바이트) */

	assert(td->files_index == 1);                       /* [한국어] 잡당 파일 하나만 허용 — libblkio는 blkio 1개에 장치 1개 */

	if (fio_blkio_check_opt_compat(td) != 0) {          /* [한국어] threaded 잡 간 옵션 일관성 검증 */
		incompatible_threaded_subjob_options = true; /* [한국어] init()들이 일괄 실패하도록 전역 set */
		return 1;
	}

	if (options->hipri &&
		options->wait_mode == FIO_BLKIO_WAIT_MODE_EVENTFD) { /* [한국어] 폴링 큐에는 eventfd 없음 */
		log_err("fio: option hipri is incompatible with option libblkio_wait_mode=eventfd\n");
		return 1;
	}

	if (options->hipri && options->force_enable_completion_eventfd) { /* [한국어] 같은 이유로 강제 활성화도 금지 */
		log_err("fio: option hipri is incompatible with option libblkio_force_enable_completion_eventfd\n");
		return 1;
	}

	if (fio_blkio_create_and_connect(td, &b) != 0)      /* [한국어] capacity 읽으려면 연결 필요 */
		return 1;

	if (blkio_get_uint64(b, "capacity", &capacity) != 0) { /* [한국어] 장치/파일 크기(바이트) 조회 */
		fio_blkio_log_err(blkio_get_uint64);
		ret = 1;
		goto out_blkio_destroy;
	}

	td->files[0]->real_file_size = capacity;            /* [한국어] fio 코어가 io_u 오프셋 범위 계산에 사용 */
	fio_file_set_size_known(td->files[0]);              /* [한국어] 크기 확정 플래그 세팅 — get_file_size 호출 생략 */

out_blkio_destroy:
	blkio_destroy(&b);                                  /* [한국어] 임시 blkio 파괴 (init에서 재생성) */
	return ret;
}

/*
 * [한국어]
 * fio_blkio_init - 잡 스레드/프로세스의 per-thread 상태 초기화 + 공용 blkio 기동.
 *
 * @td: 초기화 대상 잡.
 * @return: 0 성공, 1 실패(allocation/옵션 불일치/libblkio 오류/fcntl 실패).
 *
 * 각 잡의 스레드(또는 포크된 프로세스)에서 호출된다. 첫 호출자가 proc_state.b를 생성하고
 * num-queues/num-poll-queues 속성을 설정한 뒤 blkio_start를 호출한다. 이후 잡은 이미
 * start된 blkio에서 자신의 큐 하나(blkio_get_queue 또는 blkio_get_poll_queue)만 획득.
 * hipri 잡은 proc_state.initted_hipri_threads 카운터를 인덱스로 써서 폴링 큐 N번째를
 * 받고, 일반 잡은 (initted - hipri) 값을 써서 일반 큐 N번째를 받는다. wait_mode=EVENTFD
 * 또는 force_enable_completion_eventfd일 때만 eventfd를 blocking 모드로 준비.
 *
 * 실행 컨텍스트: 잡 스레드(use_thread) 또는 잡 프로세스(fork). proc_state.mutex 보호.
 * 에러 경로: err_blkio_destroy(첫 잡 실패 시 blkio_destroy)/err_unlock(뮤텍스 해제)/
 *            err_free(per-thread 버퍼 해제).
 *
 * 호출 체인: td_io_init → [이 함수] → fio_blkio_create_and_connect / blkio_set_int /
 *            blkio_start / blkio_get_{poll_,}queue / blkioq_set_completion_fd_enabled /
 *            fcntl(F_GETFL/F_SETFL)
 */
static int fio_blkio_init(struct thread_data *td)
{
	const struct fio_blkio_options *options = td->eo;   /* [한국어] 잡별 옵션 */
	struct fio_blkio_data *data;                        /* [한국어] per-thread 상태(할당 예정) */
	int flags;                                          /* [한국어] fcntl F_GETFL 결과 저장 */

	if (td->o.use_thread && incompatible_threaded_subjob_options) { /* [한국어] setup()이 불일치 감지한 경우 */
		/*
		 * Different subjobs using option 'thread' specified
		 * incompatible options. We don't know which configuration
		 * should win, so we just fail all such subjobs.
		 */
		return 1;                                   /* [한국어] setup에서 불일치 감지 시 모든 잡을 일괄 실패 */
	}

	/*
	 * Request enqueueing is fast, and it's not possible to know exactly
	 * when a request is submitted, so never report submission latencies.
	 */
	td->o.disable_slat = 1;                             /* [한국어] submission latency 비활성 — blkioq_* 시각이 부정확하므로 통계 왜곡 방지 */

	data = calloc(1, sizeof(*data));                    /* [한국어] per-thread 상태 0초기화 할당 (has_mem_region=false 포함) */
	if (!data) {
		log_err("fio: calloc() failed\n");
		return 1;
	}

	data->iovecs = calloc(td->o.iodepth, sizeof(data->iovecs[0]));       /* [한국어] 벡터 I/O용 iovec 배열 */
	data->completions = calloc(td->o.iodepth, sizeof(data->completions[0])); /* [한국어] blkioq_do_io 완료 수집 배열 */
	if (!data->iovecs || !data->completions) {          /* [한국어] 둘 중 하나라도 실패 시 롤백 */
		log_err("fio: calloc() failed\n");
		goto err_free;
	}

	fio_blkio_proc_lock();                              /* [한국어] 공용 상태 변경 구간 진입 */

	if (proc_state.initted_threads == 0) {              /* [한국어] 이 프로세스의 첫 잡 — blkio 생성 담당 */
		/* initialize per-process blkio */
		int num_queues, num_poll_queues;            /* [한국어] 생성할 일반/폴링 큐 개수 */

		if (td->o.use_thread) {                     /* [한국어] threaded — 전체 잡 순회해 hipri별 집계 */
			num_queues 	= total_threaded_subjobs(false);
			num_poll_queues = total_threaded_subjobs(true);
		} else {                                    /* [한국어] fork — 프로세스당 자기 잡 1개뿐 */
			num_queues 	= options->hipri ? 0 : 1;
			num_poll_queues = options->hipri ? 1 : 0;
		}

		if (fio_blkio_create_and_connect(td, &proc_state.b) != 0) /* [한국어] 공용 blkio 생성+connect */
			goto err_unlock;

		if (blkio_set_int(proc_state.b, "num-queues",
				  num_queues) != 0) {       /* [한국어] 일반 큐 N개 요청 */
			fio_blkio_log_err(blkio_set_int);
			goto err_blkio_destroy;
		}

		if (blkio_set_int(proc_state.b, "num-poll-queues",
				  num_poll_queues) != 0) {  /* [한국어] 폴링 큐 N개 요청 */
			fio_blkio_log_err(blkio_set_int);
			goto err_blkio_destroy;
		}

		if (blkio_start(proc_state.b) != 0) {       /* [한국어] start 이후로는 큐 획득만 가능 — 속성 변경 금지 */
			fio_blkio_log_err(blkio_start);
			goto err_blkio_destroy;
		}
	}

	if (options->hipri) {                               /* [한국어] 폴링 잡 — 폴링 큐 인덱스 = 이미 초기화된 폴링 잡 수 */
		int i = proc_state.initted_hipri_threads;
		data->q = blkio_get_poll_queue(proc_state.b, i);
	} else {                                            /* [한국어] 일반 잡 — 일반 큐 인덱스 = (total_initted - hipri_initted) */
		int i = proc_state.initted_threads -
				proc_state.initted_hipri_threads;
		data->q = blkio_get_queue(proc_state.b, i);
	}

	if (options->wait_mode == FIO_BLKIO_WAIT_MODE_EVENTFD ||
		options->force_enable_completion_eventfd) { /* [한국어] eventfd 경로 준비 */
		/* enable completion fd and make it blocking */
		blkioq_set_completion_fd_enabled(data->q, true); /* [한국어] libblkio에 완료 통지 fd 활성화 요청 */
		data->completion_fd = blkioq_get_completion_fd(data->q); /* [한국어] eventfd 번호 획득 */

		flags = fcntl(data->completion_fd, F_GETFL);     /* [한국어] 현재 파일 상태 플래그 획득 */
		if (flags < 0) {
			log_err("fio: fcntl(F_GETFL) failed: %s\n",
				strerror(errno));
			goto err_blkio_destroy;
		}

		if (fcntl(data->completion_fd, F_SETFL,
			  flags & ~O_NONBLOCK) != 0) {      /* [한국어] O_NONBLOCK 제거 — read()가 블록 대기하도록 */
			log_err("fio: fcntl(F_SETFL) failed: %s\n",
				strerror(errno));
			goto err_blkio_destroy;
		}
	} else {
		data->completion_fd = -1;                   /* [한국어] eventfd 미사용 — sentinel */
	}

	++proc_state.initted_threads;                       /* [한국어] 전체 초기화 잡 카운터 증가 */
	if (options->hipri)
		++proc_state.initted_hipri_threads;         /* [한국어] 폴링 잡 하위 카운터 */

	/* Set data last so cleanup() does nothing if init() fails. */
	td->io_ops_data = data;                             /* [한국어] 실패 시 io_ops_data가 NULL이라 cleanup이 무동작 */

	fio_blkio_proc_unlock();                            /* [한국어] 공용 상태 해제 */

	return 0;

err_blkio_destroy:
	if (proc_state.initted_threads == 0)                /* [한국어] 첫 잡이 실패한 경우만 blkio 파괴 담당 */
		blkio_destroy(&proc_state.b);
err_unlock:
	if (proc_state.initted_threads == 0)
		proc_state.b = NULL;                        /* [한국어] destroy 후 NULL 복구 — 다음 잡도 실패시키지 않도록 */
	fio_blkio_proc_unlock();
err_free:
	free(data->completions);                            /* [한국어] calloc 실패해 NULL이어도 free(NULL) 허용 */
	free(data->iovecs);
	free(data);
	return 1;
}

/*
 * [한국어]
 * fio_blkio_post_init - fio 코어가 I/O 버퍼를 할당한 뒤 호출되는 후처리 훅.
 *
 * @td: 잡 컨텍스트.
 * @return: 0 성공, 1 blkio_map_mem_region 실패.
 *
 * iomem_alloc() 콜백이 호출되면 libblkio가 직접 버퍼를 할당·등록하므로 아무것도 안 함.
 * 그렇지 않은 경우(fio 코어가 malloc/hugepage/shared mem 등으로 직접 할당) 그 버퍼를
 * blkio_mem_region으로 사후 등록해야 DMA가 가능하다. td->orig_buffer_size는 fio가 align
 * 패딩을 추가한 값이므로 mem-region-alignment와 어긋날 수 있다 — 따라서 여기서는 순수하게
 * (max_bs × iodepth)로 재계산해 length를 잡는다.
 *
 * 실행 컨텍스트: 각 잡 스레드. init() 성공 이후, 첫 I/O 전에 호출.
 *
 * 호출 체인: backend.c → td_io_post_init → [이 함수] → blkio_map_mem_region
 */
static int fio_blkio_post_init(struct thread_data *td)
{
	struct fio_blkio_data *data = td->io_ops_data;      /* [한국어] per-thread 상태 */

	if (!data->has_mem_region) {                        /* [한국어] iomem_alloc이 호출되지 않았다면 수동 등록 필요 */
		/*
		 * Memory was allocated by the fio core and not iomem_alloc(),
		 * so we need to register it as a memory region here.
		 *
		 * `td->orig_buffer_size` is computed like `len` below, but then
		 * fio can add some padding to it to make sure it is
		 * sufficiently aligned to the page size and the mem_align
		 * option. However, this can make it become unaligned to the
		 * "mem-region-alignment" property in ways that the user can't
		 * control, so we essentially recompute `td->orig_buffer_size`
		 * here but without adding that padding.
		 */

		unsigned long long max_block_size;          /* [한국어] READ/WRITE/TRIM 중 최대 블록 크기 */
		struct blkio_mem_region region;             /* [한국어] 등록할 메모리 영역 서술자 */

		max_block_size = max(td->o.max_bs[DDIR_READ],
				     max(td->o.max_bs[DDIR_WRITE],
					 td->o.max_bs[DDIR_TRIM])); /* [한국어] 가장 큰 블록 크기로 영역 크기 계산 */

		region = (struct blkio_mem_region) {        /* [한국어] 지정 초기화 — C99 designated init */
			.addr	= td->orig_buffer,          /* [한국어] fio가 할당한 원본 버퍼 선두 */
			.len	= (size_t)max_block_size *
					(size_t)td->o.iodepth, /* [한국어] 동시 I/O iodepth개분 */
			.fd	= -1,                       /* [한국어] fd=-1 = 익명(anonymous) 메모리 */
		};

		if (blkio_map_mem_region(proc_state.b, &region) != 0) { /* [한국어] DMA 매핑 등록 */
			fio_blkio_log_err(blkio_map_mem_region);
			return 1;
		}
	}

	return 0;
}

/*
 * [한국어]
 * fio_blkio_cleanup - per-thread 상태 해제 + 마지막 잡이면 공용 blkio 파괴.
 *
 * @td: 잡 컨텍스트.
 * @return: 없음.
 *
 * 잡 종료 시점은 제각각이므로(서로 다른 잡이 같은 프로세스를 공유할 수 있음) 카운터를
 * 감소시키고 0이 된 마지막 잡만 blkio_destroy를 수행한다. io_ops_data가 NULL이면 init()이
 * 실패한 경우라 아무 자원도 없으므로 무동작.
 *
 * 실행 컨텍스트: 각 잡 스레드의 종료 시점. proc_state.mutex 보호.
 *
 * 호출 체인: backend.c → td_io_cleanup → [이 함수] → blkio_destroy
 */
static void fio_blkio_cleanup(struct thread_data *td)
{
	struct fio_blkio_data *data = td->io_ops_data;      /* [한국어] per-thread 상태 (NULL 가능) */

	/*
	 * Subjobs from different jobs can be terminated at different times, so
	 * this callback may be invoked for one subjob while another is still
	 * doing I/O. Those subjobs may share the process, so we must wait until
	 * the last subjob in the process wants to clean up to actually destroy
	 * the blkio.
	 */

	if (data) {                                         /* [한국어] init 성공한 잡만 자원 해제 */
		free(data->completions);                    /* [한국어] 완료 배열 해제 */
		free(data->iovecs);                         /* [한국어] iovec 배열 해제 */
		free(data);                                 /* [한국어] per-thread 상태 해제 */

		fio_blkio_proc_lock();                      /* [한국어] 공용 카운터 감소 직렬화 */
		if (--proc_state.initted_threads == 0) {    /* [한국어] 마지막 잡이면 blkio 파괴 */
			blkio_destroy(&proc_state.b);
			proc_state.b = NULL;                /* [한국어] 다음 실행(fio 재사용 시)을 위해 NULL 복구 */
		}
		fio_blkio_proc_unlock();
	}
}

/* [한국어] 양의 정수 x를 y의 배수로 올림. iomem_alloc에서 mem-region-alignment에 맞춤. */
#define align_up(x, y) ((((x) + (y) - 1) / (y)) * (y))

/*
 * [한국어]
 * fio_blkio_iomem_alloc - I/O 버퍼를 libblkio로 할당+DMA 등록.
 *
 * @td: 잡 컨텍스트.
 * @size: fio 코어가 요청한 버퍼 크기.
 * @return: 0 성공, 1 실패.
 *
 * ioengine_ops.iomem_alloc 콜백. fio 코어가 기본 할당 대신 엔진에 위임할 때 호출된다.
 * libblkio 드라이버별 mem-region-alignment 속성을 조회해 size를 정렬된 크기로 올림한 뒤
 * blkio_alloc_mem_region + blkio_map_mem_region을 호출한다. 이후 td->orig_buffer를
 * 할당된 영역 주소로 바꿔 fio 코어가 io_u->xfer_buf를 이 영역 안에서 분할하도록 한다.
 * FIO_SKIPPABLE_IOMEM_ALLOC 플래그 덕분에 실패해도 fio 코어가 기본 경로로 폴백 가능.
 *
 * 실행 컨텍스트: 각 잡 스레드의 버퍼 할당 시점. proc_state.mutex로 공용 blkio 접근 보호.
 *
 * 호출 체인: io_u.c 초기화 → td->io_ops->iomem_alloc → [이 함수] →
 *            blkio_get_uint64 / blkio_alloc_mem_region / blkio_map_mem_region
 */
static int fio_blkio_iomem_alloc(struct thread_data *td, size_t size)
{
	struct fio_blkio_data *data = td->io_ops_data;      /* [한국어] per-thread 상태 */
	int ret;                                            /* [한국어] 반환 코드 */
	uint64_t mem_region_alignment;                      /* [한국어] 드라이버가 요구하는 정렬 바이트 */

	if (blkio_get_uint64(proc_state.b, "mem-region-alignment",
			     &mem_region_alignment) != 0) {     /* [한국어] 드라이버 요구 정렬값 조회 (예: 페이지 크기, 4KiB) */
		fio_blkio_log_err(blkio_get_uint64);
		return 1;
	}

	/* round up size to satisfy mem-region-alignment */
	size = align_up(size, (size_t)mem_region_alignment); /* [한국어] 올림으로 정렬 요구 충족 */

	fio_blkio_proc_lock();                              /* [한국어] 공용 blkio 접근 보호 */

	if (blkio_alloc_mem_region(proc_state.b, &data->mem_region,
				   size) != 0) {             /* [한국어] DMA 가능 정렬 버퍼 할당 */
		fio_blkio_log_err(blkio_alloc_mem_region);
		ret = 1;
		goto out;
	}

	if (blkio_map_mem_region(proc_state.b, &data->mem_region) != 0) { /* [한국어] 드라이버가 접근 가능하도록 매핑 */
		fio_blkio_log_err(blkio_map_mem_region);
		ret = 1;
		goto out_free;
	}

	td->orig_buffer = data->mem_region.addr;            /* [한국어] fio 코어가 이 주소를 기준으로 io_u 버퍼 분할 */
	data->has_mem_region = true;                        /* [한국어] post_init/iomem_free가 관리하도록 표시 */

	ret = 0;
	goto out;

out_free:
	blkio_free_mem_region(proc_state.b, &data->mem_region); /* [한국어] map 실패 시 alloc만 되어있으므로 free로 되돌림 */
out:
	fio_blkio_proc_unlock();
	return ret;
}

/*
 * [한국어]
 * fio_blkio_iomem_free - iomem_alloc로 할당한 버퍼 해제.
 *
 * @td: 잡 컨텍스트.
 * @return: 없음.
 *
 * iomem_alloc 대응 해제 콜백. has_mem_region==true인 경우에만 unmap/free를 수행.
 * 실행 컨텍스트: 각 잡 스레드의 종료 직전(cleanup보다 먼저 호출될 수 있음).
 *
 * 호출 체인: io_u.c 해제 → td->io_ops->iomem_free → [이 함수] →
 *            blkio_unmap_mem_region / blkio_free_mem_region
 */
static void fio_blkio_iomem_free(struct thread_data *td)
{
	struct fio_blkio_data *data = td->io_ops_data;      /* [한국어] per-thread 상태 */

	if (data && data->has_mem_region) {                 /* [한국어] 실제 할당된 경우만 */
		fio_blkio_proc_lock();
		blkio_unmap_mem_region(proc_state.b, &data->mem_region); /* [한국어] DMA 매핑 해제 */
		blkio_free_mem_region(proc_state.b, &data->mem_region);  /* [한국어] 실제 메모리 반환 */
		fio_blkio_proc_unlock();

		data->has_mem_region = false;               /* [한국어] 중복 해제 방지 */
	}
}

/*
 * [한국어]
 * fio_blkio_open_file - 파일 열기 콜백 (noop).
 *
 * @td, @f: 사용 안 함.
 * @return: 항상 0 (성공).
 *
 * libblkio는 init()의 blkio_connect에서 이미 장치를 연 상태이므로 fio 파일 open 훅은
 * 할 일이 없다. FIO_DISKLESSIO 플래그와 함께 "파일 없음" 계약을 유지하기 위한 stub.
 *
 * 호출 체인: backend.c → td_io_open_file → [이 함수]
 */
static int fio_blkio_open_file(struct thread_data *td, struct fio_file *f)
{
	return 0;                                           /* [한국어] 실제 오픈은 blkio_connect가 처리, 여기는 계약 충족용 */
}

/*
 * [한국어]
 * fio_blkio_queue - io_u를 libblkio 큐에 제출 (핵심 fast-path).
 *
 * @td: 잡 컨텍스트.
 * @io_u: 제출할 I/O 유닛 (offset/xfer_buf/xfer_buflen/ddir/index 포함).
 * @return: FIO_Q_QUEUED(비동기 큐잉 성공) 또는 FIO_Q_COMPLETED(ENOTSUP 즉시 완료).
 *
 * ddir에 따라 blkioq_{read,write,readv,writev,discard,write_zeroes,flush}를 호출한다.
 * vectored=1이면 data->iovecs[io_u->index] 슬롯에 하나짜리 iovec을 채워 writev/readv를
 * 사용. DDIR_TRIM은 write_zeroes_on_trim 옵션에 따라 blkioq_write_zeroes 또는 discard.
 * DDIR_SYNC/DATASYNC는 blkioq_flush. 나머지는 ENOTSUP로 즉시 완료(FIO_Q_COMPLETED).
 * libblkio는 이 호출에서 실제 제출을 수행할 수도/지연할 수도 있으며, blkioq_do_io
 * (getevents에서 호출)가 최종 플러시·완료 수확을 담당한다. io_u 포인터는 user_data로
 * 전달되어 completion에서 역매핑됨. fio_ro_check는 read-only 잡이 WRITE를 하는 실수를 차단.
 *
 * 실행 컨텍스트: 잡 스레드. 큐는 스레드 단독 소유라 락 불필요.
 *
 * 호출 체인: backend.c → td_io_queue → [이 함수] →
 *            blkioq_{read,write,readv,writev,discard,write_zeroes,flush}
 */
static enum fio_q_status fio_blkio_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	const struct fio_blkio_options *options = td->eo;   /* [한국어] vectored / write_zeroes_on_trim 판정용 */
	struct fio_blkio_data *data = td->io_ops_data;      /* [한국어] 큐/iovec 배열 접근 */

	fio_ro_check(td, io_u);                             /* [한국어] read-only 잡이 쓰기 시도 시 경고 (io_u.h 매크로) */

	switch (io_u->ddir) {                               /* [한국어] 데이터 방향별 blkioq 호출 분기 */
		case DDIR_READ:
			if (options->vectored) {            /* [한국어] readv 경로 */
				struct iovec *iov = &data->iovecs[io_u->index]; /* [한국어] io_u 전용 슬롯 */
				iov->iov_base = io_u->xfer_buf;                  /* [한국어] 수신 버퍼 */
				iov->iov_len = (size_t)io_u->xfer_buflen;        /* [한국어] 길이(바이트) */

				blkioq_readv(data->q, io_u->offset, iov, 1,
					     io_u, 0);                          /* [한국어] user_data=io_u, flags=0 */
			} else {
				blkioq_read(data->q, io_u->offset,
					    io_u->xfer_buf,
					    (size_t)io_u->xfer_buflen, io_u, 0); /* [한국어] 스칼라 read */
			}
			break;
		case DDIR_WRITE:
			if (options->vectored) {            /* [한국어] writev 경로 */
				struct iovec *iov = &data->iovecs[io_u->index];
				iov->iov_base = io_u->xfer_buf;
				iov->iov_len = (size_t)io_u->xfer_buflen;

				blkioq_writev(data->q, io_u->offset, iov, 1,
					      io_u, 0);
			} else {
				blkioq_write(data->q, io_u->offset,
					     io_u->xfer_buf,
					     (size_t)io_u->xfer_buflen, io_u,
					     0);                                  /* [한국어] 스칼라 write */
			}
			break;
		case DDIR_TRIM:
			if (options->write_zeroes_on_trim) {                         /* [한국어] TRIM을 0 쓰기로 에뮬레이션 */
				blkioq_write_zeroes(data->q, io_u->offset,
						    io_u->xfer_buflen, io_u, 0);
			} else {
				blkioq_discard(data->q, io_u->offset,
					       io_u->xfer_buflen, io_u, 0);  /* [한국어] NVMe deallocate / blkdiscard 상당 */
			}
		        break;
		case DDIR_SYNC:
		case DDIR_DATASYNC:
			blkioq_flush(data->q, io_u, 0);              /* [한국어] 장치 cache flush — fsync/fdatasync 상당 */
			break;
		default:
			io_u->error = ENOTSUP;                       /* [한국어] 미지원 ddir(예: DDIR_SYNC_FILE_RANGE) */
			io_u_log_error(td, io_u);                    /* [한국어] fio 코어에 에러 로깅 */
			return FIO_Q_COMPLETED;                      /* [한국어] 즉시 완료 처리 */
	}

	return FIO_Q_QUEUED;                                         /* [한국어] 비동기 큐잉 성공 — getevents에서 완료 수확 예정 */
}

/*
 * [한국어]
 * fio_blkio_getevents - 완료된 I/O를 min~max 개 수확해 data->completions에 저장.
 *
 * @td: 잡 컨텍스트.
 * @min: 반환 전 반드시 수확해야 할 최소 완료 수.
 * @max: 한 번에 담을 최대 완료 수 (data->completions 크기 내).
 * @t: 타임아웃 (현재 구현은 무시 — 항상 NULL로 blkioq_do_io에 전달).
 * @return: 수확한 완료 수(>=0), -1 에러.
 *
 * wait_mode별 3가지 전략:
 *   - BLOCK: 단일 blkioq_do_io(min, max, NULL) — libblkio 내부가 blocking.
 *   - EVENTFD: 우선 non-block(min=0) 수확 후 부족분은 eventfd를 blocking read로 대기하고
 *              깨어나면 다시 non-block 수확. read 반환값이 8바이트(uint64_t 카운터)가
 *              아니면 오류.
 *   - LOOP: busy-loop으로 non-block 수확 반복 — CPU 100% 최저 지연 경로.
 * 수확된 완료는 data->completions[0..n-1]에 쌓이고, fio 코어는 event() 콜백으로 각 슬롯을
 * 조회한다.
 *
 * 실행 컨텍스트: 잡 스레드. 큐 단독 소유라 락 불필요.
 *
 * 호출 체인: backend.c → td_io_getevents → [이 함수] → blkioq_do_io / read(eventfd)
 */
static int fio_blkio_getevents(struct thread_data *td, unsigned int min,
			       unsigned int max, const struct timespec *t)
{
	const struct fio_blkio_options *options = td->eo;            /* [한국어] wait_mode 선택 */
	struct fio_blkio_data *data = td->io_ops_data;               /* [한국어] 큐/completions */
	int ret, n;                                                  /* [한국어] ret: 부분 호출 반환 / n: 누적 완료 수 */
	uint64_t event;                                              /* [한국어] eventfd 카운터 수신(8바이트) */

	switch (options->wait_mode) {
	case FIO_BLKIO_WAIT_MODE_BLOCK:
		n = blkioq_do_io(data->q, data->completions, (int)min, (int)max,
				 NULL);                              /* [한국어] libblkio 내부 블로킹 수확 */
		if (n < 0) {
			fio_blkio_log_err(blkioq_do_io);
			return -1;
		}
		return n;
	case FIO_BLKIO_WAIT_MODE_EVENTFD:
		n = blkioq_do_io(data->q, data->completions, 0, (int)max, NULL); /* [한국어] 먼저 즉시 가능한 만큼 수확 */
		if (n < 0) {
			fio_blkio_log_err(blkioq_do_io);
			return -1;
		}
		while (n < (int)min) {                                        /* [한국어] 아직 min 미달이면 eventfd 대기 */
			ret = read(data->completion_fd, &event, sizeof(event)); /* [한국어] blocking read — 완료 카운터 수신 */
			if (ret != sizeof(event)) {                          /* [한국어] 8바이트 미만이면 EOF/EINTR 등 비정상 */
				log_err("fio: read() on the completion fd returned %d\n",
					ret);
				return -1;
			}

			ret = blkioq_do_io(data->q, data->completions + n, 0,
					   (int)max - n, NULL);              /* [한국어] 깨어난 뒤 non-block 추가 수확 */
			if (ret < 0) {
				fio_blkio_log_err(blkioq_do_io);
				return -1;
			}

			n += ret;
		}
		return n;
	case FIO_BLKIO_WAIT_MODE_LOOP:
		for (n = 0; n < (int)min; ) {                                 /* [한국어] busy-loop — sleep 없음 */
			ret = blkioq_do_io(data->q, data->completions + n, 0,
					   (int)max - n, NULL);
			if (ret < 0) {
				fio_blkio_log_err(blkioq_do_io);
				return -1;
			}

			n += ret;                                             /* [한국어] 누적 후 조건 재검사 */
		}
		return n;
	default:
		return -1;                                                    /* [한국어] 정의되지 않은 wait_mode — 방어적 오류 */
	}
}

/*
 * [한국어]
 * fio_blkio_event - getevents가 채운 completions[event] 슬롯에서 io_u를 복원.
 *
 * @td: 잡 컨텍스트.
 * @event: completions 배열 인덱스 (0 ≤ event < 이전 getevents 반환값).
 * @return: 해당 완료의 io_u 포인터. io_u->error에 -completion->ret 설정 (>=0=성공, 양수=errno).
 *
 * libblkio completion.ret는 0=성공, <0=부정 errno이므로 io_u->error에 양수 errno로 변환.
 * user_data는 queue() 시 넘긴 io_u 포인터 그대로 보존되어 있어 즉시 복원 가능.
 *
 * 실행 컨텍스트: 잡 스레드 (getevents 직후).
 *
 * 호출 체인: backend.c → td_io_event → [이 함수]
 */
static struct io_u *fio_blkio_event(struct thread_data *td, int event)
{
	struct fio_blkio_data *data = td->io_ops_data;               /* [한국어] completions 배열 접근 */
	struct blkio_completion *completion = &data->completions[event]; /* [한국어] 해당 슬롯 */
	struct io_u *io_u = completion->user_data;                   /* [한국어] queue() 시 넘긴 io_u 복원 */

	io_u->error = -completion->ret;                              /* [한국어] libblkio: ret<0=부정 errno → fio: +errno */

	return io_u;                                                 /* [한국어] fio 코어가 put_io_u로 반환할 io_u */
}

/* [한국어] libblkio 엔진의 ioengine_ops 디스크립터. fio 코어(ioengines.c)가 이 구조체의
 * 콜백을 backend.c의 잡 루프에서 디스패치한다. 각 필드의 역할은 다음과 같다. */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "libblkio",
	/* [한국어] --ioengine=libblkio로 선택되는 이름. load_ioengine(name) 매칭 키.
	 * 설정자: 본 파일. 읽는 자: ioengines.c/find_ioengine. 값: 고유 문자열. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전 (fio.h 정의). 코어와 불일치 시 로드 거부.
	 * 설정자: 컴파일 시점 매크로. 읽는 자: check_engine_ops. */

	.flags			= FIO_DISKLESSIO | FIO_NOEXTEND |
				  FIO_NO_OFFLOAD | FIO_SKIPPABLE_IOMEM_ALLOC,
	/* [한국어] 엔진 특성 플래그 비트마스크.
	 *  - FIO_DISKLESSIO: fio가 파일 시스템 파일 생성/크기 관리를 하지 않음 (장치 기반).
	 *  - FIO_NOEXTEND: 파일 크기를 늘리는 I/O 금지 (setup에서 capacity로 고정).
	 *  - FIO_NO_OFFLOAD: I/O offload 스레드 경로 비활성 (libblkio 자체가 비동기).
	 *  - FIO_SKIPPABLE_IOMEM_ALLOC: iomem_alloc 실패 시 fio 코어 기본 할당으로 폴백 허용.
	 * 설정자: 본 파일. 읽는 자: backend.c/io_u.c가 설정/버퍼 경로 분기. */

	.setup			= fio_blkio_setup,
	/* [한국어] 메인 프로세스에서 capacity 조회 + 옵션 호환성 검사. */
	.init			= fio_blkio_init,
	/* [한국어] 잡 스레드/프로세스 단위 초기화. 첫 잡이 proc_state.b 생성·start. */
	.post_init		= fio_blkio_post_init,
	/* [한국어] fio 코어 버퍼가 할당된 뒤 mem_region 사후 등록(필요 시). */
	.cleanup		= fio_blkio_cleanup,
	/* [한국어] per-thread 해제 + 마지막 잡이 blkio_destroy. */

	.iomem_alloc		= fio_blkio_iomem_alloc,
	/* [한국어] libblkio로 직접 DMA 가능 정렬 버퍼 할당. */
	.iomem_free		= fio_blkio_iomem_free,
	/* [한국어] iomem_alloc 대응 해제. */

	.open_file		= fio_blkio_open_file,
	/* [한국어] noop (실제 오픈은 blkio_connect가 수행). */

	.queue			= fio_blkio_queue,
	/* [한국어] io_u → blkioq_{read,write,...} 매핑 후 FIO_Q_QUEUED 반환. */
	.getevents		= fio_blkio_getevents,
	/* [한국어] wait_mode별 완료 수확 루프. */
	.event			= fio_blkio_event,
	/* [한국어] completions[idx] → io_u 복원 + errno 전파. */

	.options		= options,
	/* [한국어] FIO_OPT_G_LIBBLKIO 그룹 옵션 배열(위에 정의). */
	.option_struct_size	= sizeof(struct fio_blkio_options),
	/* [한국어] td->eo 크기 힌트 — fio 파서가 offsetof 경계 검증에 사용. */
};

/*
 * [한국어]
 * fio_blkio_register - 동적 로더(constructor) — fio 시작 시 엔진을 전역 목록에 등록.
 *
 * @return: 없음.
 *
 * `fio_init` 속성(컴파일러 constructor)으로 main() 전에 자동 호출된다. register_ioengine
 * (ioengines.c)는 engine_list에 &ioengine을 추가해 --ioengine=libblkio 조회가 되도록 한다.
 *
 * 호출 체인: _init (ELF ctor) → [이 함수] → register_ioengine
 */
static void fio_init fio_blkio_register(void)
{
	register_ioengine(&ioengine);                /* [한국어] 전역 engine_list에 추가 */
}

/*
 * [한국어]
 * fio_blkio_unregister - 동적 언로더(destructor) — fio 종료 시 등록 해제.
 *
 * @return: 없음.
 *
 * `fio_exit` 속성으로 main() 이후 자동 호출. 정적/공유 라이브러리 모두에서 안전하게
 * engine_list에서 제거해 dangling pointer 방지.
 *
 * 호출 체인: _fini (ELF dtor) → [이 함수] → unregister_ioengine
 */
static void fio_exit fio_blkio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
