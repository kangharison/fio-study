/*
 * libaio engine
 *
 * IO engine using the Linux native aio interface.
 *
 */
/*
 * [한국어 설명]
 * libaio 엔진 - Linux 네이티브 비동기 I/O (AIO) 인터페이스를 사용하는 fio I/O 엔진
 *
 * === 핵심 동작 흐름 ===
 * 1. fio_libaio_init()      : 엔진 초기화 - 링 버퍼, iocb 배열, io_event 배열 등 할당
 * 2. fio_libaio_post_init() : io_queue_init()으로 커널 AIO 컨텍스트 생성
 * 3. fio_libaio_prep()      : 각 I/O 요청에 대한 iocb 구조체 준비 (read/write/fsync)
 * 4. fio_libaio_queue()     : I/O 요청을 내부 링 버퍼에 추가 (아직 커널에 제출하지 않음)
 * 5. fio_libaio_commit()    : 링 버퍼에 쌓인 요청들을 io_submit()으로 커널에 일괄 제출
 * 6. fio_libaio_getevents() : io_getevents()로 완료된 I/O 이벤트 수확(reap)
 * 7. fio_libaio_event()     : 개별 완료 이벤트에서 io_u 추출 및 결과 확인
 * 8. fio_libaio_cleanup()   : 자원 해제 및 AIO 컨텍스트 파괴
 *
 * === Linux AIO 시스템 콜 관계 ===
 * - io_queue_init() : 커널 AIO 컨텍스트(io_context_t) 생성 (내부적으로 io_setup 호출)
 * - io_submit()     : 준비된 iocb 배열을 커널에 제출하여 비동기 I/O 시작
 * - io_getevents()  : 완료된 I/O 이벤트를 커널에서 가져옴 (블로킹/논블로킹 가능)
 * - io_destroy()    : AIO 컨텍스트 파괴 및 자원 해제
 *
 * === 링 버퍼 구조 ===
 * iocbs[]와 io_us[] 배열이 링 버퍼로 사용됨:
 * - head: queue()에서 증가 (새 요청 추가 위치)
 * - tail: commit()에서 증가 (커널에 제출된 위치)
 * - queued: 현재 링 버퍼에 대기 중인 요청 수 (head - tail)
 * - entries: 링 버퍼 크기 (= iodepth)
 *
 * === userspace reap ===
 * 커널의 AIO 링 버퍼를 직접 사용자 공간에서 읽어 io_getevents() 시스템 콜을
 * 우회하는 최적화 기법. 시스템 콜 오버헤드를 줄여 성능을 향상시킴.
 */

/* 표준 라이브러리 헤더 */
#include <stdlib.h>    /* malloc, calloc, free 등 메모리 관리 함수 */
#include <unistd.h>    /* usleep 등 POSIX 유틸리티 함수 */
#include <errno.h>     /* errno, EAGAIN, EINTR 등 에러 코드 정의 */
#include <libaio.h>    /* Linux AIO 라이브러리: io_submit, io_getevents, iocb 등 */
#include <sys/time.h>      /* 시간 관련 구조체 및 함수 */
#include <sys/resource.h>  /* 리소스 제한 관련 */

/* fio 내부 헤더 */
#include "../fio.h"            /* fio 핵심 구조체 및 함수 (thread_data, io_u 등) */
#include "../lib/pow2.h"       /* is_power_of_2() 매크로 - 2의 거듭제곱 판별 */
#include "../optgroup.h"       /* 옵션 그룹 관련 */
#include "../lib/memalign.h"   /* 메모리 정렬 관련 */
#include "cmdprio.h"           /* 명령 우선순위(cmdprio) 기능 지원 */

/* Should be defined in newest aio_abi.h */
/* [한국어] 최신 aio_abi.h에 정의되어야 하는 플래그.
 * IOCB_FLAG_IOPRIO: iocb에 I/O 우선순위를 설정할 수 있게 해주는 플래그.
 * 이 플래그가 설정되면 iocb.aio_reqprio 필드의 값이 커널에 의해 사용됨. */
#ifndef IOCB_FLAG_IOPRIO
#define IOCB_FLAG_IOPRIO    (1 << 1)
#endif

/* Hack for libaio < 0.3.111 */
/* [한국어] libaio 라이브러리 버전 0.3.111 미만에 대한 호환성 처리.
 * 구버전에서는 aio_rw_flags 필드가 없고 __pad2라는 패딩 필드만 있음.
 * 이 매크로로 aio_rw_flags를 __pad2에 매핑하여 RWF_NOWAIT 등의 플래그를 사용 가능하게 함. */
#ifndef CONFIG_LIBAIO_RW_FLAGS
#define aio_rw_flags __pad2
#endif

/* [한국어] 함수 전방 선언 (forward declaration)
 * fio_libaio_commit: queue()에서 쌓인 요청을 커널에 제출
 * fio_libaio_init: 엔진 초기화 */
static int fio_libaio_commit(struct thread_data *td);
static int fio_libaio_init(struct thread_data *td);

/*
 * [한국어] libaio 엔진의 핵심 데이터 구조체
 *
 * 이 구조체는 fio의 thread_data->io_ops_data에 저장되어
 * libaio 엔진의 모든 상태를 관리한다.
 */
struct libaio_data {
	io_context_t aio_ctx;           /* 커널 AIO 컨텍스트 핸들.
	                                 * io_queue_init()으로 생성되며, io_submit()과
	                                 * io_getevents()에서 사용됨. */
	struct io_event *aio_events;    /* 완료된 I/O 이벤트를 저장하는 배열.
	                                 * io_getevents()가 이 배열에 결과를 채워넣음.
	                                 * 크기: entries (= iodepth) */
	struct iocb **iocbs;            /* I/O 제어 블록(iocb) 포인터 배열 (링 버퍼).
	                                 * io_submit()에 전달할 iocb 포인터들을 저장.
	                                 * queue()에서 추가, commit()에서 소비. */
	struct io_u **io_us;            /* io_u 포인터 배열 (링 버퍼).
	                                 * iocbs[]와 1:1 대응. commit 시 타임스탬프 기록 등에 사용. */

	struct io_u **io_u_index;       /* io_u 인덱스 배열 (현재 사용되지 않는 것으로 보임) */
	struct iovec *iovecs;		/* for vectored requests */
	                                /* [한국어] 벡터 I/O (readv/writev) 요청에 사용되는 iovec 배열.
	                                 * libaio_vectored 옵션이 활성화되면 preadv/pwritev 사용. */

	/*
	 * Basic ring buffer. 'head' is incremented in _queue(), and
	 * 'tail' is incremented in _commit(). We keep 'queued' so
	 * that we know if the ring is full or empty, when
	 * 'head' == 'tail'. 'entries' is the ring size, and
	 * 'is_pow2' is just an optimization to use AND instead of
	 * modulus to get the remainder on ring increment.
	 */
	/*
	 * [한국어] 기본 링 버퍼 구조:
	 * - 'head': _queue()에서 증가 - 새 I/O 요청이 추가되는 위치
	 * - 'tail': _commit()에서 증가 - 커널에 제출된 I/O의 위치
	 * - 'queued': head == tail일 때 링이 꽉 찼는지 비었는지 구별하기 위한 카운터
	 * - 'entries': 링 버퍼의 크기 (= iodepth 값)
	 * - 'is_pow2': entries가 2의 거듭제곱이면 true.
	 *   모듈로(%) 연산 대신 AND(&) 비트 연산으로 링 인덱스를 계산하여 성능 최적화.
	 *
	 * 링 버퍼 동작 예시 (entries=4):
	 *   queue() 호출 → iocbs[head]에 저장, head++, queued++
	 *   commit() 호출 → iocbs[tail]부터 io_submit(), tail++, queued--
	 */
	int is_pow2;                    /* entries가 2의 거듭제곱인지 여부 (최적화용) */
	unsigned int entries;           /* 링 버퍼 크기 (= td->o.iodepth) */
	unsigned int queued;            /* 현재 큐에 대기 중인 I/O 요청 수 */
	unsigned int head;              /* 링 버퍼의 헤드 인덱스 (다음 삽입 위치) */
	unsigned int tail;              /* 링 버퍼의 테일 인덱스 (다음 제출 위치) */

	struct cmdprio cmdprio;         /* 명령 우선순위(cmdprio) 설정 구조체.
	                                 * I/O 요청별로 다른 우선순위를 적용할 수 있음. */
};

/*
 * [한국어] libaio 엔진 옵션 구조체
 *
 * fio 설정 파일에서 사용자가 지정할 수 있는 libaio 엔진 전용 옵션들.
 * 예: userspace_reap=1, nowait=1, libaio_vectored=1
 */
struct libaio_options {
	struct thread_data *td;             /* 부모 thread_data에 대한 역참조 포인터 */
	unsigned int userspace_reap;        /* 1이면 커널 시스템 콜 대신 사용자 공간에서
	                                     * AIO 링 버퍼를 직접 읽어 완료 이벤트를 수확.
	                                     * io_getevents() 시스템 콜 오버헤드를 줄임. */
	struct cmdprio_options cmdprio_options; /* 명령 우선순위 관련 옵션 */
	unsigned int nowait;                /* 1이면 RWF_NOWAIT 플래그를 설정.
	                                     * I/O가 즉시 완료될 수 없으면 블로킹 대신
	                                     * -EAGAIN을 반환하도록 함. */
	unsigned int vectored;              /* 1이면 pread/pwrite 대신 preadv/pwritev 사용.
	                                     * scatter/gather I/O를 활용. */
};

/*
 * [한국어] fio 옵션 배열 정의
 *
 * fio의 옵션 파싱 시스템에 libaio 엔진 전용 옵션들을 등록.
 * 각 옵션은 이름, 타입, 구조체 내 오프셋, 도움말 텍스트 등을 포함.
 */
static struct fio_option options[] = {
	{
		/* [한국어] userspace_reap 옵션:
		 * 사용자 공간 이벤트 수확(reap) 활성화.
		 * 커널의 AIO 완료 링 버퍼를 직접 읽어서 시스템 콜 오버헤드를 줄임.
		 * 타입: FIO_OPT_STR_SET - 값 없이 이름만 지정하면 활성화됨. */
		.name	= "userspace_reap",
		.lname	= "Libaio userspace reaping",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct libaio_options, userspace_reap),
		.help	= "Use alternative user-space reap implementation",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		/* [한국어] nowait 옵션:
		 * RWF_NOWAIT 플래그를 I/O 요청에 설정.
		 * I/O가 블록 레이어에서 대기해야 하는 경우 (예: 페이지 캐시 미스, 잠금 경합)
		 * 블로킹 대신 즉시 -EAGAIN을 반환하도록 함.
		 * 지연 시간에 민감한 워크로드에서 유용. */
		.name	= "nowait",
		.lname	= "RWF_NOWAIT",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct libaio_options, nowait),
		.help	= "Set RWF_NOWAIT for reads/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},
	{
		/* [한국어] libaio_vectored 옵션:
		 * pread/pwrite 대신 preadv/pwritev (벡터 I/O) 사용.
		 * iovec 구조체를 통해 scatter/gather I/O를 수행.
		 * 단일 시스템 콜로 여러 버퍼에 대한 I/O가 가능하지만,
		 * 여기서는 iov_count=1로 사용하므로 주로 API 호환성/테스트 목적. */
		.name	= "libaio_vectored",
		.lname	= "Use libaio preadv,pwritev",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct libaio_options, vectored),
		.help	= "Use libaio {preadv,pwritev} instead of libaio {pread,pwrite}",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBAIO,
	},

	/* [한국어] 명령 우선순위(cmdprio) 관련 옵션 매크로 확장.
	 * cmdprio_percentage, cmdprio_class, cmdprio 등의 옵션이 자동으로 추가됨. */
	CMDPRIO_OPTIONS(struct libaio_options, FIO_OPT_G_LIBAIO),
	{
		.name	= NULL,  /* [한국어] 옵션 배열의 종료 마커 (sentinel) */
	},
};

/*
 * [한국어] 링 버퍼 인덱스 증가 함수
 *
 * @역할: 링 버퍼의 인덱스(head 또는 tail)를 add만큼 증가시킨다.
 *        링 크기(entries)가 2의 거듭제곱이면 AND 비트 연산으로,
 *        아니면 모듈로(%) 연산으로 래핑(wrapping)한다.
 * @파라미터:
 *   - ld: libaio_data 포인터 (링 버퍼 메타데이터 포함)
 *   - val: 증가시킬 인덱스의 포인터 (&head 또는 &tail)
 *   - add: 증가량
 * @반환값: 없음 (val이 직접 수정됨)
 *
 * @성능 최적화:
 *   entries가 2의 거듭제곱일 때: (*val + add) & (entries - 1)
 *   → 모듈로 연산보다 빠른 비트 AND 연산 사용
 *   예: entries=8 → entries-1=7=0b0111, (5+4)&7 = 9&7 = 1
 */
static inline void ring_inc(struct libaio_data *ld, unsigned int *val,
			    unsigned int add)
{
	if (ld->is_pow2)
		/* [한국어] 2의 거듭제곱: AND 비트 마스크로 빠르게 래핑 */
		*val = (*val + add) & (ld->entries - 1);
	else
		/* [한국어] 2의 거듭제곱이 아닌 경우: 모듈로 연산으로 래핑 */
		*val = (*val + add) % ld->entries;
}

/*
 * [한국어] I/O 요청 준비 함수
 *
 * @역할: io_u 구조체의 정보를 기반으로 Linux AIO용 iocb 구조체를 설정한다.
 *        읽기/쓰기 방향, 파일 디스크립터, 버퍼, 오프셋 등을 iocb에 채운다.
 * @파라미터:
 *   - td: fio 스레드 데이터 (엔진 옵션 등 포함)
 *   - io_u: fio의 I/O 유닛 (하나의 I/O 요청을 나타냄)
 * @반환값: 항상 0 (성공)
 *
 * @Linux AIO 관계:
 *   io_prep_pread()   → iocb를 비동기 읽기용으로 설정
 *   io_prep_pwrite()  → iocb를 비동기 쓰기용으로 설정
 *   io_prep_preadv()  → iocb를 벡터 비동기 읽기용으로 설정
 *   io_prep_pwritev() → iocb를 벡터 비동기 쓰기용으로 설정
 *   io_prep_fsync()   → iocb를 비동기 fsync용으로 설정
 *   이 함수들은 libaio 라이브러리가 제공하는 헬퍼로,
 *   iocb의 opcode, fd, buf, nbytes, offset 등의 필드를 채워준다.
 *
 * @흐름: fio 프레임워크가 각 io_u에 대해 이 함수를 호출한 뒤,
 *        queue() → commit() 순서로 진행.
 */
static int fio_libaio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_options *o = td->eo;         /* [한국어] 엔진 옵션 가져오기 */
	struct fio_file *f = io_u->file;           /* [한국어] I/O 대상 파일 */
	struct iocb *iocb = &io_u->iocb;           /* [한국어] io_u에 내장된 iocb 구조체 포인터 */
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 (iovecs 접근용) */

	/* [한국어] I/O 방향에 따라 iocb를 적절히 준비 */
	if (io_u->ddir == DDIR_READ) {
		/* [한국어] === 읽기 요청 === */
		if (o->vectored) {
			/* [한국어] 벡터 I/O 모드: preadv 사용 */
			struct iovec *iov = &ld->iovecs[io_u->index];
			/* [한국어] iovec에 버퍼 주소와 크기 설정 */
			iov->iov_base = io_u->xfer_buf;
			iov->iov_len = (size_t)io_u->xfer_buflen;
			/* [한국어] iocb를 벡터 읽기(preadv)용으로 설정.
			 * 파라미터: iocb, fd, iov배열, iov개수, 오프셋 */
			io_prep_preadv(iocb, f->fd, iov, 1, io_u->offset);
		} else {
			/* [한국어] 일반 I/O 모드: pread 사용 */
			/* [한국어] iocb를 일반 읽기(pread)용으로 설정.
			 * 파라미터: iocb, fd, 버퍼, 크기, 오프셋 */
			io_prep_pread(iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen,
						  io_u->offset);
		}
		if (o->nowait)
			/* [한국어] nowait 옵션이 설정되어 있으면 RWF_NOWAIT 플래그 추가.
			 * I/O가 즉시 처리될 수 없으면 블로킹 대신 EAGAIN 반환. */
			iocb->aio_rw_flags |= RWF_NOWAIT;
	} else if (io_u->ddir == DDIR_WRITE) {
		/* [한국어] === 쓰기 요청 === */
		if (o->vectored) {
			/* [한국어] 벡터 I/O 모드: pwritev 사용 */
			struct iovec *iov = &ld->iovecs[io_u->index];

			iov->iov_base = io_u->xfer_buf;
			iov->iov_len = (size_t)io_u->xfer_buflen;
			/* [한국어] iocb를 벡터 쓰기(pwritev)용으로 설정 */
			io_prep_pwritev(iocb, f->fd, iov, 1, io_u->offset);
		} else {
			/* [한국어] 일반 I/O 모드: pwrite 사용 */
			io_prep_pwrite(iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen,
						   io_u->offset);
		}
		if (o->nowait)
			/* [한국어] 쓰기에도 RWF_NOWAIT 적용 가능 */
			iocb->aio_rw_flags |= RWF_NOWAIT;
#ifdef FIO_HAVE_RWF_ATOMIC
		/* [한국어] RWF_ATOMIC 지원 시, oatomic 옵션이 설정되어 있으면
		 * 원자적(atomic) 쓰기 플래그를 추가.
		 * 이는 쓰기가 원자적으로 수행되어 부분 쓰기(partial write)가 없음을 보장. */
		if (td->o.oatomic)
			iocb->aio_rw_flags |= RWF_ATOMIC;
#endif
	} else if (ddir_sync(io_u->ddir))
		/* [한국어] === 동기화 요청 (fsync/fdatasync) ===
		 * iocb를 비동기 fsync용으로 설정. 파일의 데이터를 디스크에 플러시. */
		io_prep_fsync(iocb, f->fd);

	return 0;  /* [한국어] 항상 성공 반환 */
}

/*
 * [한국어] 명령 우선순위(cmdprio) 준비 함수
 *
 * @역할: cmdprio 기능이 활성화된 경우, I/O 요청에 우선순위를 설정한다.
 *        특정 비율의 I/O 요청에 높은/낮은 우선순위를 부여하여
 *        QoS(Quality of Service) 시나리오를 시뮬레이션할 수 있다.
 * @파라미터:
 *   - td: 스레드 데이터
 *   - io_u: 우선순위를 설정할 I/O 유닛
 * @반환값: 없음
 *
 * @상세:
 *   fio_cmdprio_set_ioprio()가 true를 반환하면 (이 요청에 우선순위 적용 결정),
 *   iocb의 aio_reqprio에 우선순위 값을 설정하고
 *   IOCB_FLAG_IOPRIO 플래그를 켜서 커널이 이 우선순위를 인식하도록 함.
 */
static inline void fio_libaio_cmdprio_prep(struct thread_data *td,
					   struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;   /* [한국어] 엔진 데이터 가져오기 */
	struct cmdprio *cmdprio = &ld->cmdprio;     /* [한국어] cmdprio 설정 가져오기 */

	/* [한국어] 이 io_u에 대해 우선순위를 적용할지 판단하고, 적용할 경우 설정 */
	if (fio_cmdprio_set_ioprio(td, cmdprio, io_u)) {
		io_u->iocb.aio_reqprio = io_u->ioprio;          /* [한국어] iocb에 I/O 우선순위 값 설정 */
		io_u->iocb.u.c.flags |= IOCB_FLAG_IOPRIO;       /* [한국어] 우선순위 사용 플래그 활성화 */
	}
}

/*
 * [한국어] 개별 완료 이벤트 처리 함수
 *
 * @역할: getevents()로 수확한 완료 이벤트 배열에서 특정 인덱스의 이벤트를 가져와
 *        대응하는 io_u를 반환하고, I/O 결과(성공/실패/부분완료)를 확인한다.
 * @파라미터:
 *   - td: 스레드 데이터
 *   - event: 이벤트 배열 내 인덱스 (0부터 시작)
 * @반환값: 해당 이벤트에 대응하는 io_u 포인터
 *
 * @상세:
 *   io_event 구조체의 필드:
 *   - obj: 원래 제출한 iocb의 포인터
 *   - res: I/O 결과 (성공 시 전송된 바이트 수, 실패 시 음수 에러 코드)
 *   - res2: 보조 결과 (보통 0)
 *
 *   container_of() 매크로로 iocb 포인터에서 io_u 포인터를 역추적.
 *   이는 io_u 구조체 내에 iocb가 내장되어 있기 때문에 가능.
 */
static struct io_u *fio_libaio_event(struct thread_data *td, int event)
{
	struct libaio_data *ld = td->io_ops_data;   /* [한국어] 엔진 데이터 가져오기 */
	struct io_event *ev;                         /* [한국어] 완료 이벤트 포인터 */
	struct io_u *io_u;                           /* [한국어] 반환할 io_u 포인터 */

	/* [한국어] 이벤트 배열에서 해당 인덱스의 이벤트 가져오기 */
	ev = ld->aio_events + event;
	/* [한국어] iocb 포인터(ev->obj)에서 io_u 포인터를 역추적.
	 * container_of(ptr, type, member): ptr이 type 구조체의 member 필드를 가리킬 때,
	 * type 구조체의 시작 주소를 계산하는 매크로. */
	io_u = container_of(ev->obj, struct io_u, iocb);

	/* [한국어] I/O 결과 확인 */
	if (ev->res != io_u->xfer_buflen) {
		/* [한국어] 전송된 바이트 수가 요청한 크기와 다른 경우 */
		if (ev->res > io_u->xfer_buflen)
			/* [한국어] res가 요청 크기보다 크면 에러 코드임 (음수 에러가 unsigned로 표현됨).
			 * 예: -EIO가 매우 큰 양수로 나타남. 부호를 반전하여 에러 코드로 저장. */
			io_u->error = -ev->res;
		else
			/* [한국어] res가 요청 크기보다 작으면 부분 완료(short I/O).
			 * resid에 남은(미전송) 바이트 수를 기록. */
			io_u->resid = io_u->xfer_buflen - ev->res;
	} else
		/* [한국어] 전송된 바이트 수가 요청한 크기와 정확히 일치 → 성공 */
		io_u->error = 0;

	return io_u;  /* [한국어] 처리된 io_u 반환 */
}

/*
 * [한국어] 커널 AIO 링 버퍼 구조체 (사용자 공간 접근용)
 *
 * 커널의 AIO 구현은 내부적으로 링 버퍼를 사용하여 완료 이벤트를 관리한다.
 * 이 링 버퍼는 io_context_t가 가리키는 메모리 영역에 매핑되어 있으며,
 * userspace_reap 옵션이 활성화되면 io_getevents() 시스템 콜 없이
 * 이 구조체를 직접 읽어서 완료 이벤트를 수확할 수 있다.
 *
 * 이는 커널 내부 구현에 의존하는 것으로, 공식 API가 아님에 주의.
 * AIO_RING_MAGIC 값으로 이 구조체가 유효한지 검증한다.
 */
struct aio_ring {
	unsigned id;		 /** kernel internal index number */
	                     /* [한국어] 커널 내부 인덱스 번호 */
	unsigned nr;		 /** number of io_events */
	                     /* [한국어] 링에 저장 가능한 io_event 개수 */
	unsigned head;       /* [한국어] 링 버퍼의 head (소비자 측 - 읽기 위치) */
	unsigned tail;       /* [한국어] 링 버퍼의 tail (생산자 측 - 커널이 새 이벤트를 추가하는 위치) */

	unsigned magic;              /* [한국어] 매직 넘버 (AIO_RING_MAGIC = 0xa10a10a1).
	                              * 이 값으로 링 버퍼가 유효한 AIO 링인지 검증. */
	unsigned compat_features;    /* [한국어] 호환 가능한 기능 플래그 */
	unsigned incompat_features;  /* [한국어] 비호환 기능 플래그 */
	unsigned header_length;	/** size of aio_ring */
	                        /* [한국어] aio_ring 헤더의 크기 (바이트) */

	struct io_event events[0];   /* [한국어] 유연한 배열 멤버 (flexible array member).
	                              * 실제 완료 이벤트가 저장되는 배열.
	                              * 크기는 nr에 의해 결정됨. */
};

/* [한국어] AIO 링 버퍼 유효성 검증을 위한 매직 넘버.
 * "a10a10a1" - "AIO AIO A1"을 연상시키는 값. */
#define AIO_RING_MAGIC	0xa10a10a1

/*
 * [한국어] 사용자 공간 이벤트 수확 함수 (userspace reap)
 *
 * @역할: io_getevents() 시스템 콜을 사용하지 않고, 커널의 AIO 링 버퍼를
 *        직접 사용자 공간에서 읽어 완료된 I/O 이벤트를 수확한다.
 *        시스템 콜 오버헤드를 완전히 제거하여 높은 IOPS 워크로드에서 성능 향상.
 * @파라미터:
 *   - aio_ctx: 커널 AIO 컨텍스트 (실제로는 aio_ring 구조체의 주소)
 *   - max: 최대 수확할 이벤트 수
 *   - events: 수확한 이벤트를 저장할 배열
 * @반환값: 실제 수확한 이벤트 수
 *
 * @커널 AIO 링 버퍼 동작:
 *   - 커널은 I/O 완료 시 ring->tail 위치에 이벤트를 추가하고 tail을 증가
 *   - 사용자는 ring->head 위치에서 이벤트를 읽고 head를 증가
 *   - head == tail이면 더 이상 읽을 이벤트가 없음
 *   - atomic_store_release로 head를 업데이트하여 커널과의 동기화 보장
 */
static int user_io_getevents(io_context_t aio_ctx, unsigned int max,
			     struct io_event *events)
{
	long i = 0;                                              /* [한국어] 수확한 이벤트 카운터 */
	unsigned head;                                           /* [한국어] 현재 head 위치 */
	struct aio_ring *ring = (struct aio_ring*) aio_ctx;      /* [한국어] AIO 컨텍스트를
	                                                          * aio_ring 구조체로 캐스팅.
	                                                          * io_context_t는 실제로 이 링 버퍼의
	                                                          * 메모리 주소를 가리킴. */

	/* [한국어] 최대 max개까지 완료 이벤트를 수확하는 루프 */
	while (i < max) {
		head = ring->head;  /* [한국어] 현재 소비 위치(head) 읽기 */

		if (head == ring->tail) {
			/* There are no more completions */
			/* [한국어] head == tail: 더 이상 완료된 이벤트가 없음. 루프 종료. */
			break;
		} else {
			/* There is another completion to reap */
			/* [한국어] 완료된 이벤트가 있으므로 수확 */
			events[i] = ring->events[head];  /* [한국어] 링에서 이벤트를 출력 배열로 복사 */
			atomic_store_release(&ring->head,
					     (head + 1) % ring->nr);
			/* [한국어] head를 다음 위치로 이동.
			 * atomic_store_release: 메모리 배리어를 포함하여
			 * 이벤트 데이터 읽기가 head 업데이트 전에 완료됨을 보장.
			 * 이는 커널이 head를 읽고 새 이벤트를 추가할 때
			 * 데이터 경쟁(race condition)을 방지. */
			i++;
		}
	}

	return i;  /* [한국어] 실제 수확한 이벤트 수 반환 */
}

/*
 * [한국어] 완료 이벤트 수확 함수 (getevents)
 *
 * @역할: 커널에서 완료된 I/O 이벤트를 가져온다.
 *        최소 min개, 최대 max개의 이벤트를 수확할 때까지 반복.
 *        userspace_reap 옵션에 따라 커널 시스템 콜 또는 직접 링 버퍼 접근 사용.
 * @파라미터:
 *   - td: 스레드 데이터
 *   - min: 최소 수확해야 할 이벤트 수
 *   - max: 최대 수확할 이벤트 수
 *   - t: 타임아웃 (NULL이면 무한 대기 가능)
 * @반환값: 수확한 이벤트 수 (성공), 음수 에러 코드 (실패)
 *
 * @핵심 흐름:
 *   1. userspace_reap이면 user_io_getevents()로 직접 수확 시도
 *   2. 아니면 io_getevents() 시스템 콜로 커널에서 수확
 *   3. 이벤트를 충분히 수확하지 못하면 commit()을 호출하여 대기 중인 요청 제출
 *   4. min개 이상 수확할 때까지 반복
 *
 * @fio 흐름에서의 위치:
 *   queue() → commit() → getevents() → event()
 *   getevents()는 commit()으로 제출된 I/O가 완료되기를 기다림.
 */
static int fio_libaio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct libaio_data *ld = td->io_ops_data;    /* [한국어] 엔진 데이터 가져오기 */
	struct libaio_options *o = td->eo;           /* [한국어] 엔진 옵션 가져오기 */
	/* [한국어] actual_min: 실제 최소 수확 수.
	 * iodepth_batch_complete_min이 0이면 논블로킹 모드(actual_min=0),
	 * 아니면 min 값 사용 (최소 min개 수확할 때까지 대기). */
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct timespec __lt, *lt = NULL;            /* [한국어] 로컬 타임아웃 변수 */
	int r, events = 0;                           /* [한국어] r: 각 호출의 반환값, events: 누적 수확 수 */

	/* [한국어] 타임아웃이 지정되었으면 로컬 복사본 생성.
	 * io_getevents()가 t를 수정할 수 있으므로 원본을 보호. */
	if (t) {
		__lt = *t;
		lt = &__lt;
	}

	/* [한국어] 최소 min개의 이벤트를 수확할 때까지 반복하는 메인 루프 */
	do {
		if (o->userspace_reap == 1
		    && actual_min == 0
		    && ((struct aio_ring *)(ld->aio_ctx))->magic
				== AIO_RING_MAGIC) {
			/* [한국어] === 사용자 공간 수확 모드 ===
			 * 조건: userspace_reap 활성화 AND 논블로킹 모드(actual_min==0)
			 *       AND AIO 링 매직 넘버 유효
			 * io_getevents() 시스템 콜 없이 링 버퍼를 직접 읽음.
			 * 주의: actual_min > 0 (블로킹)일 때는 시스템 콜을 사용해야 함.
			 *       사용자 공간에서는 새 이벤트를 기다리며 블로킹할 수 없으므로. */
			r = user_io_getevents(ld->aio_ctx, max - events,
				ld->aio_events + events);
		} else {
			/* [한국어] === 커널 시스템 콜 모드 ===
			 * io_getevents(ctx, min_nr, max_nr, events, timeout)
			 * - 최소 actual_min개, 최대 (max-events)개의 이벤트를 기다림
			 * - 이벤트는 aio_events + events 위치에 저장 (이전 수확분 뒤에 추가)
			 * - lt: 타임아웃 (NULL이면 min_nr개 도착까지 무한 대기) */
			r = io_getevents(ld->aio_ctx, actual_min,
				max - events, ld->aio_events + events, lt);
		}
		if (r > 0) {
			/* [한국어] 이벤트를 성공적으로 수확한 경우 */
			events += r;  /* [한국어] 누적 이벤트 수 증가 */
			/* [한국어] 남은 최소 필요량을 감소.
			 * min()으로 actual_min이 음수(언더플로)가 되는 것을 방지. */
			actual_min -= min((unsigned int)events, actual_min);
		}
		else if ((min && r == 0) || r == -EAGAIN) {
			/* [한국어] 이벤트 없음(0) 또는 EAGAIN 발생.
			 * 아직 대기 중인 요청이 있으면 commit()을 호출하여
			 * 큐에 남아있는 요청을 커널에 제출.
			 * 이후 짧은 대기(10us) 후 재시도. */
			fio_libaio_commit(td);
			if (actual_min)
				usleep(10);  /* [한국어] 10마이크로초 대기 후 재시도 */
		} else if (r != -EINTR)
			/* [한국어] EINTR(시그널 인터럽트)이 아닌 다른 에러면 루프 탈출.
			 * EINTR은 시그널에 의한 일시적 중단이므로 재시도 가능. */
			break;
	} while (events < min);  /* [한국어] min개 이상 수확할 때까지 반복 */

	/* [한국어] 에러 발생 시 에러 코드 반환, 성공 시 수확한 이벤트 수 반환 */
	return r < 0 ? r : events;
}

/*
 * [한국어] I/O 요청 큐잉 함수 (queue)
 *
 * @역할: 하나의 I/O 요청(io_u)을 내부 링 버퍼에 추가한다.
 *        이 시점에서는 아직 커널에 제출하지 않음 - 실제 제출은 commit()에서 수행.
 *        이렇게 큐잉 후 일괄 제출하는 방식으로 io_submit() 시스템 콜 횟수를 줄여
 *        배치 효율을 높인다.
 * @파라미터:
 *   - td: 스레드 데이터
 *   - io_u: 큐에 추가할 I/O 유닛
 * @반환값:
 *   - FIO_Q_QUEUED: 큐에 성공적으로 추가됨 (아직 제출되지 않음)
 *   - FIO_Q_BUSY: 큐가 꽉 참 (iodepth에 도달)
 *   - FIO_Q_COMPLETED: 동기적으로 즉시 완료됨 (TRIM, SYNCFS)
 *
 * @핵심 흐름:
 *   fio_libaio_queue() → 링 버퍼에 추가
 *   → fio_libaio_commit() → io_submit()으로 커널에 제출
 *   → fio_libaio_getevents() → io_getevents()로 완료 수확
 */
static enum fio_q_status fio_libaio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */

	/* [한국어] 읽기 전용 모드에서 쓰기 요청이 오면 에러 처리 */
	fio_ro_check(td, io_u);

	/* [한국어] 큐가 꽉 찼는지 확인 (현재 대기 수 == iodepth).
	 * 큐가 꽉 차면 FIO_Q_BUSY를 반환하여 fio가 먼저 완료 이벤트를
	 * 수확하도록 유도. */
	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/* [한국어] TRIM(discard)과 SYNCFS는 비동기로 처리할 수 없으므로
	 * 동기적으로 즉시 처리. 단, 이미 큐에 다른 요청이 있으면
	 * 먼저 그것들을 처리해야 하므로 BUSY 반환. */
	if (io_u->ddir == DDIR_TRIM || io_u->ddir == DDIR_SYNCFS) {
		if (ld->queued)
			return FIO_Q_BUSY;  /* [한국어] 대기 중인 비동기 요청이 있으면 먼저 처리 필요 */

		/* [한국어] TRIM 또는 SYNCFS를 동기적으로 즉시 실행 */
		if (io_u->ddir == DDIR_TRIM)
			do_io_u_trim(td, io_u);    /* [한국어] TRIM(discard) 수행 */
		else
			do_io_u_sync(td, io_u);    /* [한국어] SYNCFS 수행 */
		io_u_mark_submit(td, 1);           /* [한국어] 제출 통계 1 증가 */
		io_u_mark_complete(td, 1);         /* [한국어] 완료 통계 1 증가 */
		return FIO_Q_COMPLETED;            /* [한국어] 즉시 완료됨을 반환 */
	}

	/* [한국어] cmdprio(명령 우선순위) 모드가 활성화되어 있으면
	 * 이 io_u에 대해 우선순위를 설정 */
	if (ld->cmdprio.mode != CMDPRIO_MODE_NONE)
		fio_libaio_cmdprio_prep(td, io_u);

	/* [한국어] === 링 버퍼에 요청 추가 ===
	 * iocbs[head]에 iocb 포인터 저장 (io_submit()에 전달할 배열)
	 * io_us[head]에 io_u 포인터 저장 (나중에 타임스탬프 기록 등에 사용)
	 * head를 1 증가 (링 래핑 적용)
	 * queued 카운터 증가 */
	ld->iocbs[ld->head] = &io_u->iocb;  /* [한국어] iocb 포인터를 head 위치에 저장 */
	ld->io_us[ld->head] = io_u;         /* [한국어] io_u 포인터를 head 위치에 저장 */
	ring_inc(ld, &ld->head, 1);         /* [한국어] head 인덱스를 1 증가 (링 래핑) */
	ld->queued++;                        /* [한국어] 대기 중인 요청 수 증가 */
	return FIO_Q_QUEUED;                 /* [한국어] 큐에 추가됨을 반환 (아직 미제출) */
}

/*
 * [한국어] 제출된 I/O의 타임스탬프 기록 함수
 *
 * @역할: commit()에서 io_submit() 성공 후 호출되어,
 *        제출된 각 io_u에 제출 시각(issue_time)을 기록한다.
 *        이 타임스탬프는 지연 시간(latency) 측정에 사용됨.
 * @파라미터:
 *   - td: 스레드 데이터
 *   - io_us: 제출된 io_u 포인터 배열
 *   - nr: 제출된 io_u 개수
 * @반환값: 없음
 */
static void fio_libaio_queued(struct thread_data *td, struct io_u **io_us,
			      unsigned int nr)
{
	struct timespec now;   /* [한국어] 현재 시각 */
	unsigned int i;        /* [한국어] 루프 카운터 */

	/* [한국어] issue_time 기록이 필요 없으면 조기 반환 */
	if (!fio_fill_issue_time(td))
		return;

	/* [한국어] 현재 시각을 한 번만 가져옴 (모든 io_u에 동일 시각 적용) */
	fio_gettime(&now, NULL);

	/* [한국어] 제출된 각 io_u에 제출 시각을 기록 */
	for (i = 0; i < nr; i++) {
		struct io_u *io_u = io_us[i];

		/* [한국어] issue_time에 현재 시각 복사 */
		memcpy(&io_u->issue_time, &now, sizeof(now));
		/* [한국어] io_u의 큐잉 완료 처리 (지연 시간 계산 등에 활용) */
		io_u_queued(td, io_u);
	}

	/*
	 * only used for iolog
	 */
	/* [한국어] iolog(I/O 로그) 파일을 사용하는 경우에만
	 * 마지막 제출 시각을 thread_data에도 기록 */
	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &now, sizeof(now));
}

/*
 * [한국어] I/O 요청 일괄 제출 함수 (commit)
 *
 * @역할: queue()에서 링 버퍼에 쌓아둔 I/O 요청들을 io_submit() 시스템 콜로
 *        커널에 일괄 제출한다. 여러 요청을 한 번의 시스템 콜로 제출하여
 *        시스템 콜 오버헤드를 최소화.
 * @파라미터:
 *   - td: 스레드 데이터
 * @반환값: 0 (성공), 음수 에러 코드 (실패)
 *
 * @핵심 동작:
 *   1. 링 버퍼의 tail부터 queued개 만큼의 iocb를 io_submit()으로 제출
 *   2. 링 버퍼가 끝에서 래핑되는 경우, 끝까지만 먼저 제출하고 다음 루프에서 나머지 제출
 *   3. EAGAIN, EINTR, ENOMEM 등의 에러를 적절히 처리
 *
 * @io_submit() 관계:
 *   io_submit(ctx, nr, iocbs[]) - nr개의 iocb를 커널 AIO에 제출
 *   반환값 > 0: 성공적으로 제출된 iocb 수
 *   반환값 == 0: 아무것도 제출되지 않음
 *   반환값 < 0: 에러 코드 (-EAGAIN, -ENOMEM 등)
 *
 * @fio 흐름:
 *   queue()에서 링 버퍼에 추가 → commit()에서 io_submit()으로 커널에 제출
 *   → getevents()에서 완료 대기 및 수확
 */
static int fio_libaio_commit(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */
	struct iocb **iocbs;        /* [한국어] 이번에 제출할 iocb 배열의 시작 포인터 */
	struct io_u **io_us;        /* [한국어] 이번에 제출할 io_u 배열의 시작 포인터 */
	struct timespec ts;         /* [한국어] EAGAIN 대기 시작 시각 (30초 타임아웃용) */
	int ret, wait_start = 0;    /* [한국어] ret: io_submit 반환값, wait_start: 대기 시작 여부 */

	/* [한국어] 큐에 대기 중인 요청이 없으면 아무것도 하지 않음 */
	if (!ld->queued)
		return 0;

	/* [한국어] 모든 대기 요청을 제출할 때까지 반복 */
	do {
		long nr = ld->queued;  /* [한국어] 제출할 요청 수 */

		/* [한국어] 링 버퍼의 tail부터 끝(entries)까지의 연속 공간만 제출 가능.
		 * 링 버퍼가 끝에서 래핑되면, 끝까지의 요청만 먼저 제출하고
		 * 다음 루프 반복에서 시작 부분의 나머지를 제출. */
		nr = min((unsigned int) nr, ld->entries - ld->tail);
		/* [한국어] tail 위치부터의 io_us와 iocbs 포인터 설정 */
		io_us = ld->io_us + ld->tail;
		iocbs = ld->iocbs + ld->tail;

		/* [한국어] === 핵심: io_submit()으로 커널에 I/O 요청 일괄 제출 ===
		 * io_submit(aio_ctx, 제출할_개수, iocb_포인터_배열)
		 * 성공하면 실제 제출된 개수를 반환 (nr 이하일 수 있음) */
		ret = io_submit(ld->aio_ctx, nr, iocbs);
		if (ret > 0) {
			/* [한국어] 제출 성공: ret개의 요청이 커널에 제출됨 */
			fio_libaio_queued(td, io_us, ret);  /* [한국어] 제출된 io_u에 타임스탬프 기록 */
			io_u_mark_submit(td, ret);          /* [한국어] 제출 통계 업데이트 */

			ld->queued -= ret;                  /* [한국어] 대기 수에서 제출된 수 차감 */
			ring_inc(ld, &ld->tail, ret);       /* [한국어] tail을 제출된 수만큼 전진 */
			ret = 0;                            /* [한국어] 성공 상태로 설정 */
			wait_start = 0;                     /* [한국어] 대기 타이머 리셋 */
		} else if (ret == -EINTR || !ret) {
			/* [한국어] EINTR: 시그널에 의해 중단됨 → 재시도
			 * ret==0: 아무것도 제출되지 않음 → 재시도 */
			if (!ret)
				io_u_mark_submit(td, ret);  /* [한국어] 0개 제출 통계 기록 */
			wait_start = 0;
			continue;  /* [한국어] 루프 계속 (재시도) */
		} else if (ret == -EAGAIN) {
			/*
			 * If we get EAGAIN, we should break out without
			 * error and let the upper layer reap some
			 * events for us. If we have no queued IO, we
			 * must loop here. If we loop for more than 30s,
			 * just error out, something must be buggy in the
			 * IO path.
			 */
			/* [한국어] EAGAIN: 커널 AIO 큐가 꽉 참.
			 * - 큐에 다른 요청이 있으면: 에러 없이 탈출하여 상위 레이어가
			 *   완료 이벤트를 수확하도록 함 (수확 후 큐에 공간이 생김).
			 * - 큐에 다른 요청이 없으면: 여기서 대기하며 재시도해야 함.
			 *   30초 이상 대기하면 I/O 경로에 버그가 있다고 판단하여 에러 반환. */
			if (ld->queued) {
				ret = 0;   /* [한국어] 에러가 아닌 정상 반환 */
				break;     /* [한국어] 루프 탈출 → 상위에서 이벤트 수확 */
			}
			if (!wait_start) {
				/* [한국어] 대기 시작 시각 기록 */
				fio_gettime(&ts, NULL);
				wait_start = 1;
			} else if (mtime_since_now(&ts) > 30000) {
				/* [한국어] 30초(30000ms) 이상 대기: 에러로 판단하고 탈출 */
				log_err("fio: aio appears to be stalled, giving up\n");
				break;
			}
			usleep(1);   /* [한국어] 1마이크로초 대기 후 재시도 */
			continue;
		} else if (ret == -ENOMEM) {
			/*
			 * If we get -ENOMEM, reap events if we can. If
			 * we cannot, treat it as a fatal event since there's
			 * nothing we can do about it.
			 */
			/* [한국어] ENOMEM: 메모리 부족으로 제출 실패.
			 * - 큐에 다른 요청이 있으면: 먼저 이벤트를 수확하여 메모리를 해제.
			 * - 큐가 비어있으면: 치명적 에러로 처리 (복구 불가). */
			if (ld->queued)
				ret = 0;   /* [한국어] 에러가 아닌 정상 반환 */
			break;
		} else
			/* [한국어] 기타 에러 (예: -EBADF, -EFAULT 등): 루프 탈출 */
			break;
	} while (ld->queued);  /* [한국어] 대기 중인 요청이 남아있으면 계속 제출 */

	return ret;  /* [한국어] 0이면 성공, 음수면 에러 코드 */
}

/*
 * [한국어] 엔진 정리/해제 함수 (cleanup)
 *
 * @역할: libaio 엔진이 사용한 모든 자원을 해제한다.
 *        AIO 컨텍스트 파괴, 메모리 해제 등을 수행.
 * @파라미터:
 *   - td: 스레드 데이터
 * @반환값: 없음
 *
 * @주의사항:
 *   io_destroy()는 부모 프로세스에서만 호출.
 *   자식 프로세스(TD_F_CHILD)에서는 exit_aio()에서 병렬로 처리되어
 *   RCU(Read-Copy-Update) 지연을 줄임.
 */
static void fio_libaio_cleanup(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */

	if (ld) {
		/*
		 * Work-around to avoid huge RCU stalls at exit time. If we
		 * don't do this here, then it'll be torn down by exit_aio().
		 * But for that case we can parallellize the freeing, thus
		 * speeding it up a lot.
		 */
		/* [한국어] 종료 시 거대한 RCU 지연(stall)을 피하기 위한 우회 방법.
		 * 여기서 AIO 컨텍스트를 파괴하지 않으면 exit_aio()에서 처리됨.
		 * 그러나 exit_aio()에서는 해제를 병렬화할 수 있어서 더 빠름.
		 *
		 * TD_F_CHILD가 아닌 경우(부모 프로세스)에만 여기서 io_destroy() 호출.
		 * 자식 프로세스들은 exit_aio()에서 병렬로 처리됨. */
		if (!(td->flags & TD_F_CHILD))
			io_destroy(ld->aio_ctx);  /* [한국어] 커널 AIO 컨텍스트 파괴 */

		fio_cmdprio_cleanup(&ld->cmdprio);  /* [한국어] cmdprio 자원 해제 */
		free(ld->iovecs);        /* [한국어] iovec 배열 해제 */
		free(ld->aio_events);    /* [한국어] io_event 배열 해제 */
		free(ld->iocbs);         /* [한국어] iocb 포인터 배열 해제 */
		free(ld->io_us);         /* [한국어] io_u 포인터 배열 해제 */
		free(ld);                /* [한국어] libaio_data 구조체 자체 해제 */
	}
}

/*
 * [한국어] 엔진 사후 초기화 함수 (post_init)
 *
 * @역할: init() 이후에 호출되어 커널 AIO 컨텍스트를 생성한다.
 *        init()과 분리된 이유는, AIO 컨텍스트 생성이 fork() 전에 이루어져야 하며
 *        리소스 제한 등의 설정이 완료된 후에 수행되어야 하기 때문.
 * @파라미터:
 *   - td: 스레드 데이터
 * @반환값: 0 (성공), 1 (실패)
 *
 * @Linux AIO 관계:
 *   io_queue_init(maxevents, &ctx) = io_setup(maxevents, &ctx)
 *   - maxevents: 동시에 처리할 수 있는 최대 I/O 요청 수 (= iodepth)
 *   - ctx: 생성된 AIO 컨텍스트가 저장될 변수
 *   실패 시 /proc/sys/fs/aio-max-nr 값을 확인할 필요가 있음.
 */
static int fio_libaio_post_init(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops_data;  /* [한국어] 엔진 데이터 가져오기 */
	int err;

	/* [한국어] 커널 AIO 컨텍스트 생성.
	 * iodepth 크기의 AIO 큐를 커널에 요청.
	 * 내부적으로 io_setup() 시스템 콜을 호출함. */
	err = io_queue_init(td->o.iodepth, &ld->aio_ctx);
	if (err) {
		/* [한국어] 실패 시 에러 보고.
		 * 일반적 원인: /proc/sys/fs/aio-max-nr 한도 초과,
		 * 또는 메모리 부족. */
		td_verror(td, -err, "io_queue_init");
		return 1;  /* [한국어] 실패 */
	}

	return 0;  /* [한국어] 성공 */
}

/*
 * [한국어] 엔진 초기화 함수 (init)
 *
 * @역할: libaio 엔진의 데이터 구조체를 할당하고 초기화한다.
 *        링 버퍼, iocb 배열, io_event 배열, iovec 배열 등을 할당.
 *        이 함수는 실제 AIO 컨텍스트를 생성하지 않음 (post_init에서 수행).
 * @파라미터:
 *   - td: 스레드 데이터
 * @반환값: 0 (성공), 1 (실패)
 *
 * @메모리 할당:
 *   - libaio_data: 엔진의 메인 데이터 구조체
 *   - aio_events[entries]: io_getevents() 결과를 저장할 배열
 *   - iocbs[entries]: iocb 포인터 배열 (링 버퍼)
 *   - io_us[entries]: io_u 포인터 배열 (링 버퍼)
 *   - iovecs[entries]: 벡터 I/O용 iovec 배열
 *   여기서 entries = iodepth (사용자가 설정한 I/O 깊이)
 */
static int fio_libaio_init(struct thread_data *td)
{
	struct libaio_data *ld;                     /* [한국어] 엔진 데이터 포인터 */
	struct libaio_options *o = td->eo;          /* [한국어] 엔진 옵션 가져오기 */
	int ret;

	/* [한국어] libaio_data 구조체를 0으로 초기화하여 할당 */
	ld = calloc(1, sizeof(*ld));

	/* [한국어] 링 버퍼 크기를 iodepth로 설정 */
	ld->entries = td->o.iodepth;
	/* [한국어] entries가 2의 거듭제곱인지 확인 (ring_inc 최적화용) */
	ld->is_pow2 = is_power_of_2(ld->entries);
	/* [한국어] 완료 이벤트 저장용 io_event 배열 할당 */
	ld->aio_events = calloc(ld->entries, sizeof(struct io_event));
	/* [한국어] iocb 포인터 배열 할당 (링 버퍼, io_submit에 전달) */
	ld->iocbs = calloc(ld->entries, sizeof(struct iocb *));
	/* [한국어] io_u 포인터 배열 할당 (링 버퍼, 타임스탬프 기록 등에 사용) */
	ld->io_us = calloc(ld->entries, sizeof(struct io_u *));
	/* [한국어] 벡터 I/O용 iovec 배열 할당 */
	ld->iovecs = calloc(ld->entries, sizeof(ld->iovecs[0]));

	/* [한국어] 엔진 데이터를 thread_data에 저장.
	 * 이후 다른 콜백 함수들이 td->io_ops_data로 접근. */
	td->io_ops_data = ld;

	/* [한국어] 명령 우선순위(cmdprio) 기능 초기화 */
	ret = fio_cmdprio_init(td, &ld->cmdprio, &o->cmdprio_options);
	if (ret) {
		/* [한국어] cmdprio 초기화 실패 시 에러 보고 */
		td_verror(td, EINVAL, "fio_libaio_init");
		return 1;  /* [한국어] 실패 */
	}

	return 0;  /* [한국어] 성공 */
}

/*
 * [한국어] I/O 엔진 오퍼레이션 구조체 정의
 *
 * fio의 I/O 엔진 인터페이스를 구현하는 함수 포인터 테이블.
 * fio 프레임워크는 이 구조체를 통해 libaio 엔진의 각 기능을 호출한다.
 *
 * 전체 호출 흐름:
 *   init() → post_init() → [prep() → queue() → commit() → getevents() → event()] 반복 → cleanup()
 *
 * 각 콜백 함수의 역할:
 *   init:        엔진 데이터 구조체 할당 및 초기화
 *   post_init:   커널 AIO 컨텍스트 생성
 *   prep:        각 io_u에 대해 iocb 구조체 준비
 *   queue:       io_u를 내부 링 버퍼에 추가 (미제출)
 *   commit:      링 버퍼의 요청들을 io_submit()으로 커널에 일괄 제출
 *   getevents:   io_getevents()로 완료 이벤트 수확
 *   event:       개별 완료 이벤트에서 io_u 추출 및 결과 확인
 *   cleanup:     자원 해제 및 AIO 컨텍스트 파괴
 *   open_file:   파일 열기 (generic 구현 사용)
 *   close_file:  파일 닫기 (generic 구현 사용)
 *   get_file_size: 파일 크기 가져오기 (generic 구현 사용)
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "libaio",            /* [한국어] 엔진 이름: "libaio" */
	.version		= FIO_IOOPS_VERSION,   /* [한국어] I/O 오퍼레이션 API 버전 */
	.flags			= FIO_ASYNCIO_SYNC_TRIM |       /* [한국어] TRIM은 동기적으로 처리 */
					FIO_ASYNCIO_SYNC_SYNCFS |       /* [한국어] SYNCFS는 동기적으로 처리 */
					FIO_ASYNCIO_SETS_ISSUE_TIME |   /* [한국어] 엔진이 issue_time을 직접 설정 */
					FIO_ATOMICWRITES,               /* [한국어] 원자적 쓰기 지원 */
	.init			= fio_libaio_init,         /* [한국어] 초기화 콜백 */
	.post_init		= fio_libaio_post_init,    /* [한국어] 사후 초기화 콜백 (AIO ctx 생성) */
	.prep			= fio_libaio_prep,         /* [한국어] I/O 준비 콜백 (iocb 설정) */
	.queue			= fio_libaio_queue,        /* [한국어] I/O 큐잉 콜백 (링 버퍼에 추가) */
	.commit			= fio_libaio_commit,       /* [한국어] I/O 제출 콜백 (io_submit) */
	.getevents		= fio_libaio_getevents,    /* [한국어] 이벤트 수확 콜백 (io_getevents) */
	.event			= fio_libaio_event,        /* [한국어] 개별 이벤트 처리 콜백 */
	.cleanup		= fio_libaio_cleanup,      /* [한국어] 정리/해제 콜백 */
	.open_file		= generic_open_file,       /* [한국어] 파일 열기 (범용 구현 사용) */
	.close_file		= generic_close_file,      /* [한국어] 파일 닫기 (범용 구현 사용) */
	.get_file_size		= generic_get_file_size,   /* [한국어] 파일 크기 조회 (범용 구현 사용) */
	.options		= options,                 /* [한국어] 엔진 전용 옵션 배열 */
	.option_struct_size	= sizeof(struct libaio_options), /* [한국어] 옵션 구조체 크기 */
};

/*
 * [한국어] 엔진 등록 함수 (생성자)
 *
 * @역할: fio 프로그램 시작 시 자동으로 호출되어 libaio 엔진을 fio에 등록한다.
 * @속성: fio_init - __attribute__((constructor))와 유사하게 main() 전에 실행됨.
 *        이를 통해 fio가 "ioengine=libaio"를 인식할 수 있게 됨.
 */
static void fio_init fio_libaio_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] ioengine 구조체를 fio의 엔진 목록에 등록 */
}

/*
 * [한국어] 엔진 해제 함수 (소멸자)
 *
 * @역할: fio 프로그램 종료 시 자동으로 호출되어 libaio 엔진 등록을 해제한다.
 * @속성: fio_exit - __attribute__((destructor))와 유사하게 프로그램 종료 시 실행됨.
 */
static void fio_exit fio_libaio_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] fio의 엔진 목록에서 libaio 엔진 제거 */
}
