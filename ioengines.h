/*
 * [한국어 설명] fio I/O 엔진 인터페이스 헤더 (ioengines.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 I/O 엔진이 구현해야 하는 모든 함수 포인터(콜백)와 플래그,
 * 상태 값을 정의한다. 새로운 I/O 엔진을 작성하려면 ioengine_ops 구조체의
 * 필수 콜백을 구현하고, register_ioengine()으로 등록하면 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ioengines.c와 함께 I/O 엔진 추상화 계층을 구성한다.
 * backend.c → ioengines.c(td_io_*) → engines/*.c 호출 체인에서
 * 이 헤더가 ioengine_ops 인터페이스를 정의하여 엔진 플러그인 구조를 가능하게 한다.
 *
 * === 타 모듈과의 연결 ===
 * - ioengines.c: td_io_*() 함수가 이 헤더의 ioengine_ops 콜백을 호출
 * - engines/*.c: 각 엔진이 ioengine_ops를 구현하여 등록
 * - fio.h: thread_data에 io_ops 필드로 엔진 인터페이스 포인터 저장
 * - io_u.h: io_u 구조체를 콜백 함수의 파라미터로 전달
 *
 * === 주요 함수/구조체 요약 ===
 * - struct ioengine_ops: I/O 엔진 콜백 테이블 (init, prep, queue, commit, getevents 등)
 * - enum fio_q_status: queue() 반환값 (COMPLETED, QUEUED, BUSY)
 * - enum fio_ioengine_flags: 엔진 특성 플래그 (동기/비동기, pthreads 필요 등)
 * - FIO_IOOPS_VERSION: 엔진 ABI 버전 번호 (동적 로딩 호환성 보장)
 */

#ifndef FIO_IOENGINE_H
#define FIO_IOENGINE_H

#include <stddef.h>

#include "compiler/compiler.h"
#include "flist.h"
#include "io_u.h"
#include "zbd_types.h"
#include "dataplacement.h"

/*
 * I/O 엔진 ops 구조체의 ABI 버전 번호.
 * fio 코어와 I/O 엔진 간의 인터페이스 호환성을 보장하기 위해 사용된다.
 * 동적 로딩(dlopen) 시 이 버전이 일치하지 않으면 엔진 로드를 거부한다.
 */
#define FIO_IOOPS_VERSION	39

/*
 * 정적 링크 vs 동적 엔진 로딩 제어 매크로.
 * CONFIG_DYNAMIC_ENGINES가 정의되지 않으면(정적 빌드), 엔진 등록 함수에
 * static 키워드를 붙여 심볼 충돌을 방지한다.
 * CONFIG_DYNAMIC_ENGINES가 정의되면(동적 빌드), 엔진 심볼을 외부에 노출한다.
 */
#ifndef CONFIG_DYNAMIC_ENGINES
#define FIO_STATIC	static
#else
#define FIO_STATIC
#endif

/*
 * io_ops->queue() return values
 * io_ops->queue() 함수의 반환 값 정의.
 * queue()는 I/O 요청을 엔진에 제출할 때 호출되며,
 * 아래 세 가지 상태 중 하나를 반환해야 한다.
 */
enum fio_q_status {
	FIO_Q_COMPLETED	= 0,		/* completed sync */
	/* 한국어: I/O가 동기적으로 즉시 완료되었음을 나타낸다.
	 * 동기(sync) I/O 엔진(예: sync, psync)에서 주로 반환한다.
	 * 이 값이 반환되면 fio는 getevents()/event()를 호출하지 않고
	 * 바로 완료 처리(레이턴시 기록 등)를 수행한다.
	 */

	FIO_Q_QUEUED	= 1,		/* queued, will complete async */
	/* 한국어: I/O 요청이 큐에 들어갔으며, 나중에 비동기적으로 완료될 것임을 나타낸다.
	 * 비동기 I/O 엔진(예: libaio, io_uring)에서 주로 반환한다.
	 * 이 값이 반환되면 fio는 나중에 getevents()와 event()를 호출하여
	 * 완료된 I/O를 수확(reap)한다.
	 */

	FIO_Q_BUSY	= 2,		/* no more room, call ->commit() */
	/* 한국어: 엔진의 제출 큐가 가득 차서 더 이상 요청을 받을 수 없음을 나타낸다.
	 * 이 값이 반환되면 fio는 먼저 commit()을 호출하여 대기 중인
	 * 요청들을 커널에 제출하고, 그 후 다시 queue()를 재시도한다.
	 * io_uring에서 SQ(Submission Queue)가 가득 찬 경우 등에서 발생한다.
	 */
};

/*
 * I/O 엔진 연산(operations) 구조체.
 * 각 I/O 엔진(예: sync, libaio, io_uring 등)은 이 구조체를 채워서
 * fio 코어에 자신의 동작 방식을 알려준다.
 * 필수 콜백: queue (I/O 제출의 핵심)
 * 선택 콜백: 나머지 함수 포인터들은 NULL이면 기본 동작 또는 무시된다.
 */
struct ioengine_ops {
	struct flist_head list;
	/* 한국어: 등록된 I/O 엔진들의 연결 리스트 노드.
	 * register_ioengine()으로 등록하면 전역 엔진 리스트에 추가된다.
	 * fio 내부의 이중 연결 리스트(flist) 구현을 사용한다.
	 */

	const char *name;
	/* 한국어: I/O 엔진의 이름 문자열.
	 * fio 명령줄에서 --ioengine=<name>으로 지정하는 이름이다.
	 * 예: "sync", "psync", "libaio", "io_uring" 등.
	 */

	int version;
	/* 한국어: 이 엔진이 지원하는 ioengine_ops ABI 버전.
	 * FIO_IOOPS_VERSION과 비교하여 호환성을 검증한다.
	 * 버전이 일치하지 않으면 엔진 로드가 실패한다.
	 */

	int flags;
	/* 한국어: 엔진의 동작 특성을 나타내는 비트 플래그.
	 * enum fio_ioengine_flags의 값들을 OR 조합하여 설정한다.
	 * 예: FIO_SYNCIO | FIO_RAWIO 등.
	 * fio 코어는 이 플래그를 보고 엔진의 특성에 맞게 동작을 조정한다.
	 */

	void *dlhandle;
	/* 한국어: 동적 로딩된 엔진의 dlopen() 핸들.
	 * 정적 빌드에서는 NULL이다.
	 * 엔진 해제 시 dlclose()에 전달된다.
	 */

	int (*setup)(struct thread_data *);
	/* 한국어: 엔진 초기화 전에 호출되는 사전 설정 콜백.
	 * init()보다 먼저 호출되며, 주로 옵션 검증이나 파일 레이아웃 설정에 사용된다.
	 * 호출 시점: td_io_init() 내부에서 init() 호출 전에 실행.
	 * 반환값: 0이면 성공, 0이 아니면 실패하여 스레드가 종료된다.
	 */

	int (*init)(struct thread_data *);
	/* 한국어: 엔진 초기화 콜백. 엔진별 자원 할당, 컨텍스트 생성 등을 수행한다.
	 * 호출 시점: td_io_init()에서 setup() 이후에 호출.
	 * 예: libaio에서 io_setup(), io_uring에서 io_uring_setup() 호출.
	 * 반환값: 0이면 성공, 0이 아니면 실패.
	 */

	int (*post_init)(struct thread_data *);
	/* 한국어: init() 이후 추가 초기화가 필요할 때 호출되는 콜백.
	 * 파일 열기 등이 완료된 후에 실행되므로 파일 디스크립터에 의존하는
	 * 초기화를 여기서 수행할 수 있다.
	 * 호출 시점: 파일 열기(open_file) 이후, 실제 I/O 루프 진입 전.
	 * 반환값: 0이면 성공.
	 */

	int (*prep)(struct thread_data *, struct io_u *);
	/* 한국어: I/O 요청을 큐에 넣기 전에 준비 작업을 수행하는 콜백.
	 * io_u의 오프셋, 크기, 방향 등이 결정된 후 queue() 전에 호출된다.
	 * 호출 시점: td_io_prep()에서 호출. queue() 바로 직전.
	 * 예: iocb 구조체 초기화, SQE 세팅 등.
	 * 반환값: 0이면 성공.
	 */

	enum fio_q_status (*queue)(struct thread_data *, struct io_u *);
	/* 한국어: I/O 요청을 실제로 제출(submit)하는 핵심 콜백. [필수 구현]
	 * 호출 시점: td_io_queue()에서 호출. prep() 이후 실행.
	 * 동기 엔진: 여기서 실제 read()/write()를 수행하고 FIO_Q_COMPLETED 반환.
	 * 비동기 엔진: 요청을 내부 큐에 추가하고 FIO_Q_QUEUED 반환.
	 *             큐가 가득 차면 FIO_Q_BUSY 반환.
	 * 반환값: enum fio_q_status (위 enum 참조).
	 */

	int (*commit)(struct thread_data *);
	/* 한국어: 큐에 쌓인 I/O 요청들을 한꺼번에 커널에 제출하는 콜백.
	 * 비동기 엔진에서 배치 제출(batch submit)을 위해 사용된다.
	 * 호출 시점: td_io_commit()에서 호출.
	 *           - queue()가 FIO_Q_BUSY를 반환한 후 자동 호출.
	 *           - I/O depth에 도달했을 때 호출.
	 * 예: libaio의 io_submit(), io_uring의 io_uring_enter().
	 * 반환값: 0이면 성공.
	 */

	int (*getevents)(struct thread_data *, unsigned int, unsigned int, const struct timespec *);
	/* 한국어: 완료된 I/O 이벤트를 수확(reap)하는 콜백.
	 * 인자: td, 최소 완료 대기 수(min), 최대 수확 수(max), 타임아웃.
	 * 호출 시점: td_io_getevents()에서 호출. 비동기 I/O 완료를 기다릴 때.
	 * 최소 min개의 이벤트가 완료되거나 타임아웃될 때까지 블로킹한다.
	 * 반환값: 실제로 완료된 이벤트 수. 음수면 에러.
	 * 예: libaio의 io_getevents(), io_uring의 완료 큐 폴링.
	 */

	struct io_u *(*event)(struct thread_data *, int);
	/* 한국어: getevents()로 수확한 이벤트 중 특정 인덱스의 io_u를 반환하는 콜백.
	 * 인자: td, 이벤트 인덱스(0부터 시작).
	 * 호출 시점: getevents() 이후, 완료된 각 이벤트를 처리할 때 반복 호출.
	 * fio 코어는 0부터 getevents() 반환값까지 순회하며 event()를 호출한다.
	 * 반환값: 해당 인덱스의 완료된 io_u 포인터.
	 */

	char *(*errdetails)(struct thread_data *, struct io_u *);
	/* 한국어: I/O 에러 발생 시 상세 에러 메시지를 반환하는 콜백.
	 * 호출 시점: I/O 에러 로깅 시 호출.
	 * 엔진별로 더 구체적인 에러 정보를 문자열로 제공할 수 있다.
	 * 반환값: 동적 할당된 에러 메시지 문자열(호출자가 free).
	 *         NULL이면 기본 에러 메시지 사용.
	 */

	void (*cleanup)(struct thread_data *);
	/* 한국어: 엔진 종료 시 자원을 해제하는 콜백.
	 * 호출 시점: close_ioengine() 또는 free_ioengine()에서 호출.
	 *           스레드 종료 직전, init()에서 할당한 자원을 해제한다.
	 * 예: io_uring의 io_uring_queue_exit(), libaio의 io_destroy().
	 */

	int (*open_file)(struct thread_data *, struct fio_file *);
	/* 한국어: 대상 파일/장치를 여는 콜백.
	 * 호출 시점: td_io_open_file()에서 호출.
	 *           I/O 루프에서 파일이 필요할 때, 또는 초기화 시.
	 * 대부분의 엔진은 generic_open_file()을 사용한다.
	 * 반환값: 0이면 성공.
	 */

	int (*close_file)(struct thread_data *, struct fio_file *);
	/* 한국어: 열린 파일/장치를 닫는 콜백.
	 * 호출 시점: td_io_close_file()에서 호출.
	 *           파일 사용이 끝났을 때 또는 스레드 종료 시.
	 * 대부분의 엔진은 generic_close_file()을 사용한다.
	 * 반환값: 0이면 성공.
	 */

	int (*invalidate)(struct thread_data *, struct fio_file *);
	/* 한국어: 파일의 페이지 캐시를 무효화(invalidate)하는 콜백.
	 * 호출 시점: I/O 테스트 시작 전, 캐시 영향을 배제하기 위해 호출.
	 * 기본적으로 fadvise(POSIX_FADV_DONTNEED)를 사용한다.
	 * 반환값: 0이면 성공.
	 */

	int (*unlink_file)(struct thread_data *, struct fio_file *);
	/* 한국어: 테스트 파일을 삭제(unlink)하는 콜백.
	 * 호출 시점: td_io_unlink_file()에서 호출.
	 *           cleanup 단계에서 테스트용 임시 파일을 제거할 때.
	 * 반환값: 0이면 성공.
	 */

	int (*get_file_size)(struct thread_data *, struct fio_file *);
	/* 한국어: 대상 파일/장치의 크기를 얻는 콜백.
	 * 호출 시점: td_io_get_file_size()에서 호출.
	 *           초기화 시 I/O 범위를 결정하기 위해 사용.
	 * 대부분의 엔진은 generic_get_file_size()를 사용한다.
	 * 반환값: 0이면 성공.
	 */

	int (*prepopulate_file)(struct thread_data *, struct fio_file *);
	/* 한국어: 테스트 전에 파일을 미리 데이터로 채우는(prepopulate) 콜백.
	 * 호출 시점: 파일 레이아웃 생성 후, I/O 테스트 시작 전.
	 * 반환값: 0이면 성공.
	 */

	void (*terminate)(struct thread_data *);
	/* 한국어: 엔진에 종료 신호를 보내는 콜백.
	 * 호출 시점: 스레드가 종료 요청을 받았을 때 호출.
	 * 비동기 엔진에서 블로킹 중인 getevents()를 깨우는 데 사용.
	 */

	int (*iomem_alloc)(struct thread_data *, size_t);
	/* 한국어: I/O 버퍼 메모리를 할당하는 콜백.
	 * 호출 시점: I/O 버퍼 초기화 시. 엔진이 특별한 메모리를 필요로 할 때 사용.
	 * 예: SPDK에서 DMA 가능한 메모리 할당, RDMA에서 등록된 메모리 할당.
	 * NULL이면 fio의 기본 메모리 할당(malloc/mmap 등)을 사용한다.
	 * 반환값: 0이면 성공.
	 */

	void (*iomem_free)(struct thread_data *);
	/* 한국어: iomem_alloc()으로 할당한 I/O 버퍼 메모리를 해제하는 콜백.
	 * 호출 시점: 스레드 종료 시 I/O 버퍼를 정리할 때.
	 */

	int (*io_u_init)(struct thread_data *, struct io_u *);
	/* 한국어: 개별 io_u(I/O 유닛) 구조체를 초기화하는 콜백.
	 * 호출 시점: io_u 풀(pool) 초기화 시 각 io_u마다 호출.
	 * 엔진별 io_u 확장 데이터를 할당/초기화할 때 사용.
	 * 반환값: 0이면 성공.
	 */

	void (*io_u_free)(struct thread_data *, struct io_u *);
	/* 한국어: io_u_init()에서 할당한 엔진별 io_u 데이터를 해제하는 콜백.
	 * 호출 시점: io_u 풀 해제 시 각 io_u마다 호출.
	 */

	int (*get_zoned_model)(struct thread_data *td,
			       struct fio_file *f, enum zbd_zoned_model *);
	/* 한국어: 장치의 ZBD(Zoned Block Device) 모델을 조회하는 콜백.
	 * 호출 시점: ZBD 초기화 시, 장치가 호스트관리/호스트인식/일반 중
	 *           어떤 모델인지 판별할 때.
	 * 반환값: 0이면 성공. *model에 조회 결과를 저장.
	 */

	int (*report_zones)(struct thread_data *, struct fio_file *,
			    uint64_t, struct zbd_zone *, unsigned int);
	/* 한국어: ZBD 장치의 존(zone) 정보를 보고하는 콜백.
	 * 인자: td, 파일, 시작 오프셋, 존 배열, 존 개수.
	 * 호출 시점: ZBD 초기화 시 각 존의 상태(WP 위치, 타입 등)를 읽을 때.
	 * 반환값: 보고된 존의 개수.
	 */

	int (*reset_wp)(struct thread_data *, struct fio_file *,
			uint64_t, uint64_t);
	/* 한국어: ZBD 존의 쓰기 포인터(Write Pointer)를 리셋하는 콜백.
	 * 인자: td, 파일, 시작 오프셋, 길이.
	 * 호출 시점: ZBD 워크로드에서 존을 리셋(빈 상태로 되돌리기)할 때.
	 * 반환값: 0이면 성공.
	 */

	int (*move_zone_wp)(struct thread_data *, struct fio_file *,
			    struct zbd_zone *, uint64_t, const char *);
	/* 한국어: ZBD 존의 쓰기 포인터를 특정 위치로 이동시키는 콜백.
	 * 인자: td, 파일, 존, 새 WP 위치, 디버그용 설명 문자열.
	 * 호출 시점: ZBD 에뮬레이션이나 테스트 시 WP를 강제로 옮길 때.
	 * 반환값: 0이면 성공.
	 */

	int (*get_max_open_zones)(struct thread_data *, struct fio_file *,
				  unsigned int *);
	/* 한국어: ZBD 장치에서 동시에 열 수 있는 최대 존 개수를 조회하는 콜백.
	 * 호출 시점: ZBD 초기화 시 장치의 최대 오픈 존 수 제한을 확인할 때.
	 * 반환값: 0이면 성공. *max에 최대값 저장.
	 */

	int (*get_max_active_zones)(struct thread_data *, struct fio_file *,
				    unsigned int *);
	/* 한국어: ZBD 장치에서 동시에 활성화할 수 있는 최대 존 개수를 조회하는 콜백.
	 * 호출 시점: ZBD 초기화 시 장치의 최대 활성 존 수 제한을 확인할 때.
	 * 반환값: 0이면 성공. *max에 최대값 저장.
	 */

	int (*finish_zone)(struct thread_data *, struct fio_file *,
			   uint64_t, uint64_t);
	/* 한국어: ZBD 존을 FULL 상태로 마무리(finish)하는 콜백.
	 * 인자: td, 파일, 시작 오프셋, 길이.
	 * 호출 시점: 존을 명시적으로 완료 상태로 전환할 때.
	 * 반환값: 0이면 성공.
	 */

	int (*fdp_fetch_ruhs)(struct thread_data *, struct fio_file *,
			      struct fio_ruhs_info *);
	/* 한국어: FDP(Flexible Data Placement)의 RUHS(Reclaim Unit Handle Status)를
	 * 가져오는 콜백.
	 * 호출 시점: FDP 기능을 사용하는 NVMe 장치에서 데이터 배치 정보를 조회할 때.
	 * 반환값: 0이면 성공.
	 */

	int option_struct_size;
	/* 한국어: 엔진 전용 옵션 구조체의 크기(바이트).
	 * fio가 엔진별 옵션 구조체를 할당할 때 이 크기를 사용한다.
	 * 0이면 엔진 전용 옵션이 없음을 의미한다.
	 */

	struct fio_option *options;
	/* 한국어: 엔진 전용 옵션 배열의 포인터.
	 * fio 명령줄이나 job 파일에서 엔진별 옵션을 파싱할 때 사용된다.
	 * NULL이면 엔진 전용 옵션이 없다.
	 * 배열의 마지막 항목은 name이 NULL인 센티널(sentinel)이어야 한다.
	 */
};

/*
 * I/O 엔진 플래그 비트 위치 정의 (내부용 enum).
 * 아래의 각 __FIO_* 값은 비트 위치(bit position)이며,
 * 실제 플래그 값은 enum fio_ioengine_flags에서 1 << __FIO_*로 계산된다.
 */
enum {
	__FIO_SYNCIO = 0,		/* io engine has synchronous ->queue */
	/* 한국어: 동기 I/O 엔진임을 표시.
	 * queue()가 동기적으로 I/O를 완료하고 FIO_Q_COMPLETED를 반환한다.
	 * 이 플래그가 설정되면 fio는 getevents()/event()를 호출하지 않는다.
	 * 예: sync, psync, vsync 엔진.
	 */

	__FIO_RAWIO,			/* some sort of direct/raw io */
	/* 한국어: Direct I/O 또는 Raw I/O를 사용하는 엔진.
	 * O_DIRECT 등으로 페이지 캐시를 우회하여 직접 디스크에 접근한다.
	 * 이 플래그가 설정되면 fio는 메모리 정렬 등 추가 처리를 수행한다.
	 */

	__FIO_DISKLESSIO,		/* no disk involved */
	/* 한국어: 실제 디스크 I/O가 없는 엔진.
	 * 예: null 엔진(아무것도 하지 않음), net 엔진(네트워크 I/O).
	 * 이 플래그가 설정되면 파일 생성이나 크기 검증을 건너뛴다.
	 */

	__FIO_NOEXTEND,			/* engine can't extend file */
	/* 한국어: 엔진이 파일 크기를 확장할 수 없음.
	 * 이 플래그가 설정되면 fio는 파일 범위를 넘는 I/O를 방지한다.
	 * 블록 장치(block device) 등 크기가 고정된 대상에 사용된다.
	 */

	__FIO_NODISKUTIL,		/* diskutil can't handle filename */
	/* 한국어: diskutil(디스크 사용률 모니터링)이 이 엔진의 파일명을 처리할 수 없음.
	 * 네트워크 소켓이나 특수 장치 등 일반 파일/블록장치가 아닌 경우.
	 * 이 플래그가 설정되면 디스크 사용률 통계 수집을 건너뛴다.
	 */

	__FIO_UNIDIR,			/* engine is uni-directional */
	/* 한국어: 엔진이 단방향(읽기 전용 또는 쓰기 전용)만 지원함.
	 * 혼합(read+write) 워크로드를 실행할 수 없다.
	 * 예: splice 엔진(읽기 전용).
	 */

	__FIO_NOIO,			/* thread does only pseudo IO */
	/* 한국어: 실제 I/O를 수행하지 않는 의사(pseudo) I/O 엔진.
	 * 스레드가 I/O가 아닌 다른 작업(예: CPU 부하 테스트)을 수행한다.
	 * 예: cpuio 엔진.
	 */

	__FIO_PIPEIO,			/* input/output no seekable */
	/* 한국어: 입출력 대상이 탐색(seek) 불가능한 파이프/소켓 등임.
	 * lseek()를 사용할 수 없으므로 fio는 순차 접근만 허용한다.
	 * 예: splice, net 엔진.
	 */

	__FIO_BARRIER,			/* engine supports barriers */
	/* 한국어: 엔진이 I/O 배리어(barrier)를 지원함.
	 * 배리어는 이전 I/O가 모두 완료된 후 다음 I/O를 실행하는 순서 보장 메커니즘.
	 */

	__FIO_MEMALIGN,			/* engine wants aligned memory */
	/* 한국어: 엔진이 정렬된(aligned) 메모리 버퍼를 필요로 함.
	 * Direct I/O 등에서 섹터 크기로 정렬된 버퍼가 필요할 때 사용.
	 * 이 플래그가 설정되면 fio는 posix_memalign() 등으로 정렬된 버퍼를 할당한다.
	 */

	__FIO_BIT_BASED,		/* engine uses a bit base (e.g. uses Kbit as opposed to
					   KB) */
	/* 한국어: 엔진이 바이트(byte) 대신 비트(bit) 단위를 사용함.
	 * 예: 네트워크 대역폭을 Kbit/s 단위로 표시하는 net 엔진.
	 * 이 플래그가 설정되면 fio는 통계 출력을 비트 단위로 변환한다.
	 */

	__FIO_FAKEIO,			/* engine pretends to do IO */
	/* 한국어: 엔진이 실제 I/O 없이 I/O를 수행하는 척하는 가짜(fake) I/O.
	 * fio의 기능 테스트나 벤치마크 오버헤드 측정에 사용된다.
	 * 예: null 엔진.
	 */

	__FIO_NOSTATS,			/* don't do IO stats */
	/* 한국어: 이 엔진의 I/O에 대해 통계(레이턴시, IOPS 등)를 수집하지 않음.
	 * fio 내부 벤치마크나 보조 스레드에서 사용된다.
	 */

	__FIO_NOFILEHASH,		/* doesn't hash the files for lookup later. */
	/* 한국어: 파일 해시 테이블에 파일을 등록하지 않음.
	 * 동일 파일의 중복 열기 감지가 불필요한 엔진에서 사용된다.
	 * 해시 등록 오버헤드를 줄인다.
	 */

	__FIO_ASYNCIO_SYNC_TRIM,	/* io engine has async ->queue except for trim */
	/* 한국어: 비동기 I/O 엔진이지만, TRIM(discard) 요청만 동기적으로 처리함.
	 * queue()에서 TRIM 명령은 FIO_Q_COMPLETED를, 나머지(read/write)는
	 * FIO_Q_QUEUED를 반환한다.
	 * fio 코어는 이 플래그를 보고 TRIM에 대해 동기 처리 경로를 선택한다.
	 * 예: io_uring 엔진 (커널이 비동기 TRIM을 지원하지 않는 경우).
	 */

	__FIO_ASYNCIO_SYNC_SYNCFS,	/* io engine has async ->queue except for syncfs */
	/* 한국어: 비동기 I/O 엔진이지만, syncfs 요청만 동기적으로 처리함.
	 * syncfs는 파일시스템 전체 동기화 명령이며, 이를 비동기로 처리할 수
	 * 없는 엔진에서 사용된다.
	 */

	__FIO_NO_OFFLOAD,		/* no async offload */
	/* 한국어: I/O 완료 처리를 별도 스레드로 오프로드(offload)하지 않음.
	 * 기본적으로 fio는 동기 엔진의 완료 처리를 별도 스레드에서 할 수 있는데,
	 * 이 플래그가 설정되면 그 기능을 비활성화한다.
	 */

	__FIO_ASYNCIO_SETS_ISSUE_TIME,	/* async ioengine with commit function that sets
					   issue_time */
	/* 한국어: 비동기 엔진의 commit() 함수가 I/O의 issue_time(발행 시각)을 설정함.
	 * 일반적으로 fio 코어가 queue() 시점에 issue_time을 기록하지만,
	 * 이 플래그가 설정되면 commit() 시점(실제 커널 제출 시점)에 기록한다.
	 * 이를 통해 더 정확한 레이턴시 측정이 가능하다.
	 * 예: io_uring 엔진.
	 */

	__FIO_SKIPPABLE_IOMEM_ALLOC,	/* skip iomem_alloc & iomem_free if job sets mem/iomem */
	/* 한국어: 사용자가 job 파일에서 mem/iomem 옵션을 설정한 경우,
	 * 엔진의 iomem_alloc/iomem_free 콜백 호출을 건너뛸 수 있음.
	 * 사용자가 자체적으로 메모리 관리 방식을 지정했으므로 엔진의
	 * 메모리 할당을 무시해도 안전하다는 뜻이다.
	 */

	__FIO_RO_NEEDS_RW_OPEN,		/* open files in rw mode even if we have a read job; only
					   affects ioengines using generic_open_file */
	/* 한국어: 읽기 전용 job이라도 파일을 읽기/쓰기(O_RDWR) 모드로 열어야 함.
	 * generic_open_file()을 사용하는 엔진에만 영향을 미친다.
	 * 일부 장치 드라이버가 특정 ioctl을 위해 쓰기 권한을 요구할 때 사용.
	 */

	__FIO_MULTI_RANGE_TRIM,		/* ioengine supports trim with more than one range */
	/* 한국어: 엔진이 다중 범위(multi-range) TRIM을 지원함.
	 * 한 번의 TRIM 명령으로 여러 불연속 범위를 지정할 수 있다.
	 * 이 플래그가 없으면 fio는 각 범위마다 개별 TRIM 요청을 보낸다.
	 */

	__FIO_ATOMICWRITES,		/* ioengine supports atomic writes */
	/* 한국어: 엔진이 원자적 쓰기(atomic writes)를 지원함.
	 * 원자적 쓰기는 전체 데이터가 한꺼번에 기록되거나 전혀 기록되지 않음을 보장.
	 * 전원 장애 시 부분 기록(torn write)이 발생하지 않는다.
	 */

	__FIO_SYNCFS,			/* ioengine supports syncfs */
	/* 한국어: 엔진이 syncfs(파일시스템 전체 동기화) 명령을 지원함.
	 * 이 플래그가 설정된 엔진만 syncfs 기반 워크로드를 실행할 수 있다.
	 */

	__FIO_IOENGINE_F_LAST,		/* not a real bit; used to count number of bits */
	/* 한국어: 실제 플래그가 아니라, 플래그 비트의 총 개수를 세기 위한 센티널.
	 * 컴파일 타임에 플래그 배열 크기 등을 계산할 때 사용된다.
	 */
};

/*
 * I/O 엔진 플래그의 실제 비트마스크 값 정의.
 * 위의 __FIO_* 비트 위치를 1 << 연산으로 변환한 것이다.
 * ioengine_ops.flags 필드에 이 값들을 OR 조합하여 설정한다.
 */
enum fio_ioengine_flags {
	FIO_SYNCIO			= 1 << __FIO_SYNCIO,
	/* 한국어: 동기 I/O 엔진 플래그. queue()가 즉시 완료를 반환함. */

	FIO_RAWIO			= 1 << __FIO_RAWIO,
	/* 한국어: Direct/Raw I/O 플래그. 페이지 캐시 우회. */

	FIO_DISKLESSIO			= 1 << __FIO_DISKLESSIO,
	/* 한국어: 디스크리스 I/O 플래그. 실제 디스크 없이 동작. */

	FIO_NOEXTEND			= 1 << __FIO_NOEXTEND,
	/* 한국어: 파일 확장 불가 플래그. 고정 크기 대상용. */

	FIO_NODISKUTIL  		= 1 << __FIO_NODISKUTIL,
	/* 한국어: 디스크 유틸리티 비활성화 플래그. */

	FIO_UNIDIR			= 1 << __FIO_UNIDIR,
	/* 한국어: 단방향 I/O 플래그. 읽기 또는 쓰기만 가능. */

	FIO_NOIO			= 1 << __FIO_NOIO,
	/* 한국어: 의사(pseudo) I/O 플래그. 실제 I/O 없음. */

	FIO_PIPEIO			= 1 << __FIO_PIPEIO,
	/* 한국어: 파이프 I/O 플래그. seek 불가능한 대상. */

	FIO_BARRIER			= 1 << __FIO_BARRIER,
	/* 한국어: 배리어 지원 플래그. I/O 순서 보장. */

	FIO_MEMALIGN			= 1 << __FIO_MEMALIGN,
	/* 한국어: 정렬 메모리 요구 플래그. */

	FIO_BIT_BASED			= 1 << __FIO_BIT_BASED,
	/* 한국어: 비트 단위 플래그. 통계를 비트 단위로 표시. */

	FIO_FAKEIO			= 1 << __FIO_FAKEIO,
	/* 한국어: 가짜 I/O 플래그. 실제 I/O 없이 수행하는 척함. */

	FIO_NOSTATS			= 1 << __FIO_NOSTATS,
	/* 한국어: 통계 비활성화 플래그. I/O 통계를 수집하지 않음. */

	FIO_NOFILEHASH			= 1 << __FIO_NOFILEHASH,
	/* 한국어: 파일 해시 비활성화 플래그. 파일 중복 검사 안 함. */

	FIO_ASYNCIO_SYNC_TRIM		= 1 << __FIO_ASYNCIO_SYNC_TRIM,
	/* 한국어: TRIM만 동기 처리하는 비동기 엔진 플래그. */

	FIO_ASYNCIO_SYNC_SYNCFS		= 1 << __FIO_ASYNCIO_SYNC_SYNCFS,
	/* 한국어: syncfs만 동기 처리하는 비동기 엔진 플래그. */

	FIO_NO_OFFLOAD			= 1 << __FIO_NO_OFFLOAD,
	/* 한국어: 비동기 오프로드 비활성화 플래그. */

	FIO_ASYNCIO_SETS_ISSUE_TIME	= 1 << __FIO_ASYNCIO_SETS_ISSUE_TIME,
	/* 한국어: commit()에서 issue_time을 설정하는 비동기 엔진 플래그. */

	FIO_SKIPPABLE_IOMEM_ALLOC	= 1 << __FIO_SKIPPABLE_IOMEM_ALLOC,
	/* 한국어: 사용자 메모리 설정 시 엔진 메모리 할당 건너뛰기 플래그. */

	FIO_RO_NEEDS_RW_OPEN		= 1 << __FIO_RO_NEEDS_RW_OPEN,
	/* 한국어: 읽기 job에서도 읽기/쓰기 모드로 파일 여는 플래그. */

	FIO_MULTI_RANGE_TRIM		= 1 << __FIO_MULTI_RANGE_TRIM,
	/* 한국어: 다중 범위 TRIM 지원 플래그. */

	FIO_ATOMICWRITES		= 1 << __FIO_ATOMICWRITES,
	/* 한국어: 원자적 쓰기 지원 플래그. */

	FIO_SYNCFS			= 1 << __FIO_SYNCFS,
	/* 한국어: syncfs 지원 플래그. */
};

/*
 * External engine defined symbol to fill in the engine ops structure
 * 한국어: 외부(동적 로딩) 엔진이 정의하는 심볼의 함수 포인터 타입.
 * dlopen()으로 엔진 공유 라이브러리를 로드한 후,
 * dlsym()으로 이 타입의 함수를 찾아 호출하여 ioengine_ops를 얻는다.
 */
typedef void (*get_ioengine_t)(struct ioengine_ops **);

/*
 * io engine entry points
 * 한국어: fio 코어가 I/O 엔진을 호출하기 위한 진입점(wrapper) 함수들.
 * 이 함수들은 내부적으로 ioengine_ops의 해당 콜백을 호출하며,
 * NULL 체크, 에러 처리, 통계 기록 등의 공통 로직을 포함한다.
 * __must_check 속성은 반환값을 반드시 확인하도록 컴파일러 경고를 발생시킨다.
 */

extern int __must_check td_io_init(struct thread_data *);
/* 한국어: I/O 엔진 초기화. setup() -> init() 순으로 호출한다. */

extern int __must_check td_io_prep(struct thread_data *, struct io_u *);
/* 한국어: I/O 요청 준비. 내부적으로 ops->prep()을 호출한다. */

extern enum fio_q_status __must_check td_io_queue(struct thread_data *, struct io_u *);
/* 한국어: I/O 요청 제출. 내부적으로 ops->queue()를 호출하고 결과를 처리한다. */

extern int __must_check td_io_getevents(struct thread_data *, unsigned int, unsigned int, const struct timespec *);
/* 한국어: 완료된 I/O 이벤트 수확. 내부적으로 ops->getevents()를 호출한다. */

extern void td_io_commit(struct thread_data *);
/* 한국어: 큐에 쌓인 I/O를 커널에 제출. 내부적으로 ops->commit()을 호출한다. */

extern int __must_check td_io_open_file(struct thread_data *, struct fio_file *);
/* 한국어: 파일 열기. 내부적으로 ops->open_file()을 호출한다. */

extern int td_io_close_file(struct thread_data *, struct fio_file *);
/* 한국어: 파일 닫기. 내부적으로 ops->close_file()을 호출한다. */

extern int td_io_unlink_file(struct thread_data *, struct fio_file *);
/* 한국어: 파일 삭제. 내부적으로 ops->unlink_file()을 호출한다. */

extern int __must_check td_io_get_file_size(struct thread_data *, struct fio_file *);
/* 한국어: 파일 크기 조회. 내부적으로 ops->get_file_size()를 호출한다. */

extern struct ioengine_ops *load_ioengine(struct thread_data *);
/* 한국어: 지정된 이름의 I/O 엔진을 로드한다.
 * 정적 빌드: 내장 엔진 리스트에서 검색.
 * 동적 빌드: dlopen()으로 공유 라이브러리 로드 후 get_ioengine_t 심볼 호출.
 */

extern void register_ioengine(struct ioengine_ops *);
/* 한국어: I/O 엔진을 전역 엔진 리스트에 등록한다.
 * 정적 빌드에서는 초기화 시, 동적 빌드에서는 dlopen 후 호출된다.
 */

extern void unregister_ioengine(struct ioengine_ops *);
/* 한국어: I/O 엔진을 전역 엔진 리스트에서 제거한다. */

extern void free_ioengine(struct thread_data *);
/* 한국어: I/O 엔진 자원을 해제한다. 동적 엔진이면 dlclose()도 수행. */

extern void close_ioengine(struct thread_data *);
/* 한국어: I/O 엔진을 닫는다. cleanup() 콜백을 호출한 후 free_ioengine()을 수행. */

extern int fio_show_ioengine_help(const char *engine);
/* 한국어: 지정된 엔진의 도움말(옵션 목록 등)을 출력한다.
 * --enghelp 명령줄 옵션에 의해 호출된다.
 */

#endif
