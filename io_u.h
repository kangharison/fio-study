/**
 * io_u.h - fio I/O 유닛(io_u) 헤더 파일
 *
 * [io_u의 생명주기 (Lifecycle of io_u)]
 *
 *   io_u는 fio에서 하나의 I/O 요청을 표현하는 핵심 구조체이다.
 *   각 io_u 인스턴스는 다음과 같은 생명주기를 거친다:
 *
 *   1. freelist (유휴 상태)
 *      - 초기 생성 후 또는 I/O 완료 후 freelist에 대기
 *      - IO_U_F_FREE 플래그가 설정되어 있음
 *      - get_io_u() 호출 시 freelist에서 꺼내어 사용
 *
 *   2. active (활성 상태)
 *      - get_io_u()를 통해 freelist에서 꺼내진 상태
 *      - offset, buflen, ddir 등 I/O 파라미터가 설정됨
 *      - 버퍼(buf)에 데이터가 채워짐 (쓰기의 경우)
 *      - IO_U_F_FREE 플래그가 해제됨
 *
 *   3. in-flight (전송 중 상태)
 *      - I/O 엔진(ioengine)에 제출(submit)된 상태
 *      - IO_U_F_FLIGHT 플래그가 설정됨
 *      - issue_time이 기록됨
 *      - 비동기 엔진의 경우 여러 io_u가 동시에 in-flight 가능
 *
 *   4. completed (완료 상태)
 *      - I/O 엔진이 완료를 보고한 상태
 *      - io_u_sync_complete() 또는 io_u_queued_complete()로 처리
 *      - 레이턴시 통계가 계산됨 (start_time, issue_time 기반)
 *      - 검증(verify)이 필요한 경우 verify_list에 추가
 *
 *   5. freelist로 복귀
 *      - put_io_u()를 통해 freelist로 반환
 *      - IO_U_F_FREE 플래그가 다시 설정됨
 *      - 다음 I/O 요청에 재사용됨
 *
 *   [재큐잉 경로]
 *      - I/O 엔진이 FIO_Q_BUSY를 반환하면 requeue_io_u()로 재큐잉
 *      - 재큐잉된 io_u는 다음 제출 시도에서 우선 처리됨
 */
#ifndef FIO_IO_U
#define FIO_IO_U

#include "compiler/compiler.h"
#include "os/os.h"
#include "io_ddir.h"
#include "debug.h"
#include "file.h"
#include "workqueue.h"

#ifdef CONFIG_LIBAIO
#include <libaio.h>
#endif

/**
 * io_u 플래그 (io_u flags)
 * 각 io_u의 현재 상태 및 속성을 비트마스크로 표현한다.
 * io_u->flags 필드에 OR 연산으로 조합하여 사용한다.
 */
enum {
	IO_U_F_FREE		= 1 << 0,
	/* io_u가 freelist에 있는 유휴 상태임을 나타냄.
	 * get_io_u() 시 해제되고, put_io_u() 시 다시 설정됨 */

	IO_U_F_FLIGHT		= 1 << 1,
	/* io_u가 I/O 엔진에 제출되어 전송 중(in-flight)임을 나타냄.
	 * 완료 콜백 시 해제됨 */

	IO_U_F_NO_FILE_PUT	= 1 << 2,
	/* 완료 시 파일 참조 카운트를 감소시키지 않음.
	 * 특수한 I/O (예: verify)에서 파일 해제를 방지할 때 사용 */

	IO_U_F_IN_CUR_DEPTH	= 1 << 3,
	/* 현재 I/O depth 계산에 포함되어 있음을 나타냄.
	 * iodepth 제어에 사용되며, 큐 깊이 관리에 필수 */

	IO_U_F_BUSY_OK		= 1 << 4,
	/* I/O 엔진이 FIO_Q_BUSY를 반환해도 에러로 처리하지 않음.
	 * 일부 엔진에서 재시도 가능한 상황을 허용할 때 설정 */

	IO_U_F_TRIMMED		= 1 << 5,
	/* 해당 영역이 이미 TRIM/DISCARD 처리되었음을 나타냄.
	 * 검증(verify) 시 해당 영역의 데이터 비교를 건너뛸 수 있음 */

	IO_U_F_BARRIER		= 1 << 6,
	/* 이 io_u가 배리어(barrier) I/O임을 나타냄.
	 * 이전의 모든 I/O가 완료된 후에 실행되어야 함 */

	IO_U_F_VER_LIST		= 1 << 7,
	/* 검증 리스트(verify_list)에 연결되어 있음을 나타냄.
	 * verify 단계에서 완료된 쓰기를 추적하는 데 사용 */

	IO_U_F_PATTERN_DONE	= 1 << 8,
	/* 버퍼에 패턴 데이터가 이미 채워져 있음을 나타냄.
	 * 동일 패턴의 중복 채움을 방지하여 성능을 최적화 */

	IO_U_F_DEVICE_ERROR	= 1 << 9,
	/* 디바이스 레벨에서 에러가 발생했음을 나타냄.
	 * 일반적인 I/O 에러와 구분하여 디바이스 오류를 추적 */

	IO_U_F_VER_IN_DEV	= 1 << 10, /* Verify data in device */
	/* 검증 데이터가 디바이스에 저장되어 있음을 나타냄.
	 * 디바이스로부터 데이터를 읽어 검증을 수행해야 함 */
};

/*
 * The io unit
 */
/* I/O 유닛 구조체
 * fio에서 하나의 I/O 작업을 표현하는 핵심 데이터 구조.
 * 오프셋, 길이, 방향(읽기/쓰기), 버퍼, 타이밍 정보 등
 * I/O 요청에 필요한 모든 정보를 담고 있다. */
struct io_u {
	struct timespec start_time;
	/* I/O 요청이 생성된 시각.
	 * get_io_u() 호출 시 기록되며,
	 * 전체 레이턴시(total latency) 계산의 시작점이 됨 */

	struct timespec issue_time;
	/* I/O가 실제로 엔진에 제출(issue)된 시각.
	 * submission latency = issue_time - start_time
	 * completion latency = completion_time - issue_time
	 * 이 두 시각으로 slat, clat, lat 통계를 산출함 */

	struct fio_file *file;
	/* 이 I/O가 대상으로 하는 파일(또는 디바이스) 포인터.
	 * 파일의 크기, 오프셋 범위, fd 등의 정보를 참조함.
	 * I/O 완료 시 참조 카운트가 감소됨 (IO_U_F_NO_FILE_PUT 제외) */

	unsigned int flags;
	/* io_u의 상태 플래그 비트마스크.
	 * 위의 IO_U_F_* enum 값들의 OR 조합.
	 * io_u_set() / io_u_clear() 매크로로 스레드 안전하게 조작 */

	enum fio_ddir ddir;
	/* I/O 방향 (Data Direction).
	 * DDIR_READ(읽기), DDIR_WRITE(쓰기), DDIR_TRIM(트림),
	 * DDIR_SYNC(동기화) 등의 값을 가짐.
	 * I/O 엔진이 어떤 종류의 연산을 수행할지 결정 */

	/*
	 * For replay workloads, we may want to account as a different
	 * IO type than what is being submitted.
	 */
	/* 리플레이 워크로드에서 제출되는 I/O 타입과 다른 타입으로
	 * 통계를 집계하고 싶을 때 사용한다.
	 * 예: 실제로는 쓰기를 하지만, 읽기로 통계를 집계 */
	enum fio_ddir acct_ddir;
	/* 통계 집계용 I/O 방향.
	 * -1이면 실제 ddir을 사용하고, 그 외에는 이 값으로 집계.
	 * acct_ddir() 헬퍼 함수가 적절한 값을 반환 */

	/*
	 * Write generation
	 */
	/* 쓰기 세대 번호 */
	uint64_t numberio;
	/* 이 io_u에 대한 I/O 시퀀스 번호.
	 * 쓰기 횟수를 추적하여 데이터 검증 시 올바른 세대의
	 * 데이터인지 확인하는 데 사용. verify 헤더에 기록됨 */

	/*
	 * IO priority.
	 */
	/* I/O 우선순위 */
	unsigned short ioprio;
	/* I/O 요청의 우선순위 값.
	 * ioprio_set() 시스템 호출에 전달되는 값.
	 * 우선순위 클래스(class)와 레벨(level)을 인코딩 */

	unsigned short clat_prio_index;
	/* 완료 레이턴시(clat) 우선순위 인덱스.
	 * 우선순위별로 별도의 clat 통계를 유지할 때,
	 * 해당 io_u가 어느 우선순위 버킷에 속하는지 나타냄 */

	/*
	 * number of trim ranges for this IO.
	 */
	/* 이 I/O에 대한 trim 범위의 수 */
	unsigned int number_trim;
	/* TRIM/DISCARD 연산에서 한 번에 처리할 범위(range)의 개수.
	 * 다중 범위 trim을 지원하는 엔진에서 사용 */

	/*
	 * Allocated/set buffer and length
	 */
	/* 할당/설정된 버퍼와 길이 */
	unsigned long long buflen;
	/* I/O 요청의 버퍼 크기 (바이트 단위).
	 * 블록 크기(bs) 설정에 따라 결정되며,
	 * 가변 블록 크기를 사용할 경우 매 I/O마다 달라질 수 있음 */

	unsigned long long offset;	/* is really ->xfer_offset... */
	/* I/O가 수행될 파일 내 오프셋 (바이트 단위).
	 * 실제로는 전송 오프셋(xfer_offset)의 역할을 함.
	 * 순차/랜덤 접근 패턴에 따라 결정됨.
	 * 주의: 주석 원문대로 실제로는 xfer_offset을 의미 */

	unsigned long long verify_offset;	/* is really ->offset */
	/* 검증(verify)에 사용되는 원래 오프셋.
	 * 실제로는 원본 offset 값을 보관.
	 * 부분 전송(partial transfer) 발생 시 offset과 다를 수 있음 */

	void *buf;
	/* I/O 데이터 버퍼 포인터.
	 * 메모리 정렬(alignment)된 버퍼로, 쓰기 시 전송할 데이터,
	 * 읽기 시 수신한 데이터가 저장됨.
	 * td->orig_buffer에서 할당된 영역을 가리킴 */

	/*
	 * Initial seed for generating the buffer contents
	 */
	/* 버퍼 내용을 생성하기 위한 초기 시드 값 */
	uint64_t rand_seed;
	/* 버퍼 내용 생성에 사용되는 난수 시드.
	 * 검증(verify) 시 동일한 시드로 데이터를 재생성하여
	 * 쓰기 데이터와 읽기 데이터를 비교할 수 있게 함 */

	/*
	 * IO engine state, may be different from above when we get
	 * partial transfers / residual data counts
	 */
	/* I/O 엔진 상태. 부분 전송(partial transfer) 또는
	 * 잔여 데이터(residual data)가 발생하면
	 * 위의 buf/buflen과 다를 수 있다. */
	void *xfer_buf;
	/* 실제 전송에 사용되는 버퍼 포인터.
	 * 부분 전송 시 buf + (이미 전송된 바이트)를 가리킴.
	 * 최초에는 buf와 동일하게 설정됨 */

	unsigned long long xfer_buflen;
	/* 실제 전송할 남은 데이터 길이.
	 * 부분 전송 시 buflen에서 이미 전송된 양을 뺀 값.
	 * 최초에는 buflen과 동일하게 설정됨 */

	/*
	 * Parameter related to pre-filled buffers and
	 * their size to handle variable block sizes.
	 */
	/* 미리 채워진(pre-filled) 버퍼와 관련된 파라미터.
	 * 가변 블록 크기 처리를 위한 버퍼 크기 정보. */
	unsigned long long buf_filled_len;
	/* 버퍼에 이미 채워진 데이터의 길이.
	 * 가변 블록 크기 사용 시, 현재 buflen보다 작으면
	 * 추가 데이터를 채워야 함을 나타냄.
	 * IO_U_F_PATTERN_DONE 플래그와 함께 중복 채움 방지에 사용 */

	struct io_piece *ipo;
	/* I/O 조각(io_piece) 구조체 포인터.
	 * verify 모드에서 쓰기 완료된 I/O의 오프셋/길이 정보를 보관.
	 * 나중에 읽기 검증 시 어느 영역을 확인해야 하는지 추적.
	 * 리플레이 워크로드에서도 I/O 순서 재현에 사용됨 */

	unsigned long long resid;
	/* 잔여(residual) 데이터 바이트 수.
	 * 부분 전송 시 아직 전송되지 않은 바이트 수를 나타냄.
	 * resid > 0이면 xfer_buf/xfer_buflen이 조정되어 재전송 시도 */

	unsigned int error;
	/* I/O 에러 코드.
	 * 0이면 성공, 그 외에는 errno 값.
	 * 완료 처리 시 이 값을 기반으로 에러 통계를 갱신 */

	int inflight_idx;
	/* in-flight 배열에서의 인덱스.
	 * 현재 전송 중인 io_u 목록에서 이 io_u의 위치를 추적.
	 * 완료 처리 시 빠른 제거를 위해 사용 */

	/*
	 * io engine private data
	 */
	/* I/O 엔진 전용(private) 데이터 */
	union {
		unsigned int index;
		/* io_u 배열 내에서의 인덱스 번호.
		 * td->io_u_all 배열에서 이 io_u의 위치.
		 * 일부 I/O 엔진이 io_u를 식별하는 데 사용 */

		unsigned int seen;
		/* 완료 처리 시 이미 확인(seen)되었는지 표시.
		 * 중복 완료 방지에 사용.
		 * index와 메모리를 공유하는 union 멤버 */
	};

	void *engine_data;
	/* I/O 엔진이 자유롭게 사용할 수 있는 범용 포인터.
	 * 각 엔진이 io_u에 엔진 고유의 추가 정보를 부착할 때 사용.
	 * 예: io_uring의 sqe 포인터, 네트워크 엔진의 소켓 정보 등 */

	union {
		struct flist_head verify_list;
		/* 검증 대기 리스트의 연결 노드.
		 * 쓰기 완료 후 검증이 필요한 io_u들을 연결 리스트로 관리.
		 * IO_U_F_VER_LIST 플래그와 함께 사용 */

		struct workqueue_work work;
		/* 워크큐 작업 항목.
		 * 비동기 검증(offload verify) 시 워크큐에 제출할 때 사용.
		 * verify_list와 메모리를 공유하는 union 멤버
		 * (동시에 사용되지 않으므로 union으로 최적화) */
	};

	/*
	 * ZBD mode zbd_queue_io callback: called after engine->queue operation
	 * to advance a zone write pointer and eventually unlock the I/O zone.
	 * @q indicates the I/O queue status (busy, queued or completed).
	 * @success == true means that the I/O operation has been queued or
	 * completed successfully.
	 */
	/* ZBD 모드 zbd_queue_io 콜백:
	 * 엔진의 queue 연산 후 호출되어 존(zone) 쓰기 포인터를 전진시키고,
	 * 필요 시 해당 I/O 존의 잠금(lock)을 해제한다.
	 * @q: I/O 큐 상태 (busy, queued, completed 중 하나)
	 * @success == true: I/O가 성공적으로 큐잉 또는 완료되었음 */
	void (*zbd_queue_io)(struct thread_data *td, struct io_u *, int *q);

	/*
	 * ZBD mode zbd_put_io callback: called in after completion of an I/O
	 * or commit of an async I/O to unlock the I/O target zone.
	 */
	/* ZBD 모드 zbd_put_io 콜백:
	 * I/O 완료 후 또는 비동기 I/O의 커밋(commit) 후 호출되어
	 * 대상 존(zone)의 잠금을 해제한다. */
	void (*zbd_put_io)(struct thread_data *td, const struct io_u *);

	/*
	 * Callback for io completion
	 */
	/* I/O 완료 콜백 */
	int (*end_io)(struct thread_data *, struct io_u **);
	/* I/O 완료 시 호출되는 콜백 함수 포인터.
	 * 검증(verify) 워크로드 등에서 완료 후 추가 처리를 수행.
	 * 반환값: 0이면 성공, 음수면 에러 */

	uint32_t dtype;
	/* 디렉티브 타입 (Directive Type).
	 * NVMe의 디렉티브(directive) 기능에서 사용하는 타입 값.
	 * 데이터 배치(placement) 힌트 등의 디렉티브 유형을 지정 */

	uint32_t dspec;
	/* 디렉티브 스펙 (Directive Specific).
	 * NVMe 디렉티브의 세부 스펙 값.
	 * dtype과 함께 I/O에 대한 디바이스 힌트를 전달 */

	/**
	 * I/O 엔진별 데이터 공용체 (Engine-specific data union)
	 *
	 * 각 I/O 엔진이 자체적으로 필요로 하는 제어 블록을 저장한다.
	 * 한 번에 하나의 엔진만 사용되므로 union으로 메모리를 절약한다.
	 */
	union {
#ifdef CONFIG_LIBAIO
		struct iocb iocb;
		/* Linux AIO (libaio) 제어 블록.
		 * io_submit()에 전달하는 비동기 I/O 요청 구조체.
		 * 오프셋, 길이, fd, 버퍼 등의 정보를 담고 있음 */
#endif
#ifdef CONFIG_POSIXAIO
		os_aiocb_t aiocb;
		/* POSIX AIO 제어 블록.
		 * aio_read()/aio_write()에 전달하는 비동기 I/O 구조체.
		 * 이식성(portability)을 위한 POSIX 표준 인터페이스 */
#endif
#ifdef FIO_HAVE_SGIO
		struct sg_io_hdr hdr;
		/* SCSI Generic (SG) I/O 헤더.
		 * SG_IO ioctl을 통한 SCSI 명령 직접 전송에 사용.
		 * SCSI CDB, sense 데이터 등을 포함 */
#endif
#ifdef CONFIG_SOLARISAIO
		aio_result_t resultp;
		/* Solaris AIO 결과 구조체.
		 * Solaris 플랫폼의 비동기 I/O 결과를 저장 */
#endif
#ifdef CONFIG_RDMA
		struct ibv_mr *mr;
		/* RDMA 메모리 영역(Memory Region) 포인터.
		 * InfiniBand/RDMA를 통한 I/O에서 등록된
		 * 메모리 영역을 참조 */
#endif
		void *mmap_data;
		/* mmap 엔진에서 사용하는 매핑된 메모리 포인터.
		 * 파일이 메모리 매핑된 경우 해당 주소를 저장 */
	};

	void *pi_attr;
	/* 보호 정보(Protection Information) 속성 포인터.
	 * NVMe의 end-to-end 데이터 보호(T10-DIF/PI) 기능에서
	 * 보호 정보 메타데이터를 저장하는 버퍼를 가리킴 */
};

/*
 * io unit handling
 */
/* I/O 유닛 처리 함수 선언 */

extern struct io_u *__get_io_u(struct thread_data *);
/* freelist에서 io_u를 꺼내는 내부 함수.
 * IO_U_F_FREE 플래그를 해제하고 io_u를 반환.
 * 상위 함수 get_io_u()에서 호출됨.
 * freelist가 비어 있으면 NULL 반환 */

extern struct io_u *get_io_u(struct thread_data *);
/* I/O 유닛을 할당하고 파라미터를 설정하는 주요 함수.
 * __get_io_u()로 io_u를 확보한 뒤,
 * 오프셋/길이/방향 등을 설정하고 버퍼를 채움.
 * start_time도 이 시점에 기록됨 */

extern void put_io_u(struct thread_data *, struct io_u *);
/* 사용 완료된 io_u를 freelist로 반환하는 함수.
 * IO_U_F_FREE 플래그를 설정하고, 파일 참조를 해제함.
 * I/O depth 카운터도 감소시킴 */

extern void clear_io_u(struct thread_data *, struct io_u *);
/* io_u의 상태를 초기화(clear)하는 함수.
 * 에러 정보, 잔여 데이터 등을 리셋함 */

extern void requeue_io_u(struct thread_data *, struct io_u **);
/* I/O 엔진이 BUSY를 반환한 io_u를 재큐잉하는 함수.
 * io_u를 다시 제출 대기열의 앞쪽에 배치하여
 * 다음 제출 루프에서 우선 처리되도록 함.
 * 포인터를 NULL로 설정하여 호출자의 참조를 제거 */

extern int __must_check io_u_sync_complete(struct thread_data *, struct io_u *);
/* 동기 I/O의 완료를 처리하는 함수.
 * 단일 io_u에 대해 레이턴시 계산, 통계 갱신,
 * put_io_u() 호출을 수행.
 * 반환값: 완료된 I/O 수 (에러 시 음수).
 * __must_check: 반환값을 반드시 확인해야 함 */

extern int __must_check io_u_queued_complete(struct thread_data *, int);
/* 비동기 I/O의 완료를 수확(reap)하는 함수.
 * 두 번째 인자: 최소 수확할 완료 이벤트 수.
 * 완료된 모든 io_u에 대해 통계 갱신 및 반환 처리.
 * 반환값: 완료된 I/O 수 (에러 시 음수) */

extern void io_u_queued(struct thread_data *, struct io_u *);
/* io_u가 엔진에 성공적으로 큐잉되었음을 기록하는 함수.
 * issue_time을 기록하고 submission latency를 계산 */

extern int io_u_quiesce(struct thread_data *);
/* 현재 in-flight인 모든 I/O가 완료될 때까지 대기하는 함수.
 * drain/quiesce 동작으로, 큐를 비울 때 사용.
 * 반환값: 완료된 I/O 수 */

extern void io_u_log_error(struct thread_data *, struct io_u *);
/* I/O 에러 발생 시 에러 정보를 로그에 기록하는 함수.
 * 오프셋, 길이, 방향, 에러 코드 등을 출력 */

extern void io_u_mark_depth(struct thread_data *, unsigned int);
/* I/O depth 통계 분포를 기록하는 함수.
 * 현재 depth 값에 해당하는 히스토그램 버킷을 증가시킴 */

extern void fill_io_buffer(struct thread_data *, void *, unsigned long long, unsigned long long);
/* 지정된 버퍼를 I/O 패턴 데이터로 채우는 함수.
 * 인자: td, 버퍼 포인터, 최소 크기, 최대 크기.
 * 쓰기용 데이터 또는 검증용 패턴을 생성 */

extern void io_u_fill_buffer(struct thread_data *td, struct io_u *, unsigned long long, unsigned long long);
/* io_u의 버퍼를 I/O 데이터로 채우는 함수.
 * fill_io_buffer()의 io_u 전용 래퍼.
 * buf_filled_len을 갱신하여 중복 채움을 방지 */

void io_u_mark_complete(struct thread_data *, unsigned int);
/* I/O 완료 수에 대한 통계 분포를 기록하는 함수.
 * 한 번의 reap 호출에서 완료된 I/O 수의 히스토그램을 갱신 */

void io_u_mark_submit(struct thread_data *, unsigned int);
/* I/O 제출 수에 대한 통계 분포를 기록하는 함수.
 * 한 번의 submit 호출에서 제출된 I/O 수의 히스토그램을 갱신 */

bool queue_full(const struct thread_data *);
/* I/O 큐가 가득 찼는지 확인하는 함수.
 * 현재 in-flight I/O 수가 iodepth에 도달했으면 true 반환.
 * 새로운 I/O 제출 전에 호출하여 큐 오버플로를 방지 */

int do_io_u_sync(const struct thread_data *, struct io_u *);
/* 동기 I/O 연산(fsync/fdatasync)을 수행하는 함수.
 * DDIR_SYNC 방향의 io_u를 처리 */

int do_io_u_trim(struct thread_data *, struct io_u *);
/* TRIM/DISCARD 연산을 수행하는 함수.
 * DDIR_TRIM 방향의 io_u를 처리하여
 * 지정된 영역의 데이터를 폐기(discard)함 */

#ifdef FIO_INC_DEBUG
/**
 * dprint_io_u - io_u의 디버그 정보를 출력하는 인라인 함수
 * @io_u: 출력할 I/O 유닛
 * @p: 출력 접두사 문자열 (호출 위치 식별용)
 *
 * 오프셋, 길이, 방향, 파일명 등의 io_u 정보를 디버그 로그에 출력.
 * FIO_INC_DEBUG가 정의된 경우에만 활성화됨.
 */
static inline void dprint_io_u(struct io_u *io_u, const char *p)
{
	struct fio_file *f = io_u->file;

	if (f)
		dprint(FD_IO, "%s: io_u %p: off=0x%llx,len=0x%llx,ddir=%d,file=%s\n",
				p, io_u,
				(unsigned long long) io_u->offset,
				io_u->buflen, io_u->ddir,
				f->file_name);
	else
		dprint(FD_IO, "%s: io_u %p: off=0x%llx,len=0x%llx,ddir=%d\n",
				p, io_u,
				(unsigned long long) io_u->offset,
				io_u->buflen, io_u->ddir);
}
#else
#define dprint_io_u(io_u, p)
/* 디버그 비활성화 시 dprint_io_u는 빈 매크로로 치환됨 */
#endif

/**
 * acct_ddir - 통계 집계에 사용할 I/O 방향을 반환하는 인라인 함수
 * @io_u: 대상 I/O 유닛
 *
 * acct_ddir이 -1이 아니면 acct_ddir을, 그렇지 않으면 실제 ddir을 반환.
 * 리플레이 워크로드에서 실제 I/O와 다른 방향으로 통계를 집계할 때 사용.
 */
static inline enum fio_ddir acct_ddir(struct io_u *io_u)
{
	if (io_u->acct_ddir != -1)
		return io_u->acct_ddir;

	return io_u->ddir;
}

/* io_u 플래그 조작 매크로 */

#define io_u_clear(td, io_u, val)	\
	td_flags_clear((td), &(io_u->flags), (val))
/* io_u에서 지정된 플래그 비트를 해제(clear)하는 매크로.
 * td_flags_clear()를 통해 스레드 안전(thread-safe)하게 동작.
 * 예: io_u_clear(td, io_u, IO_U_F_FLIGHT) */

#define io_u_set(td, io_u, val)		\
	td_flags_set((td), &(io_u)->flags, (val))
/* io_u에 지정된 플래그 비트를 설정(set)하는 매크로.
 * td_flags_set()을 통해 스레드 안전(thread-safe)하게 동작.
 * 예: io_u_set(td, io_u, IO_U_F_FREE) */

#endif
