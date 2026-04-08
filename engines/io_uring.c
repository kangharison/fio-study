/*
 * io_uring engine
 *
 * IO engine using the new native Linux aio io_uring interface.
 *
 */
/*
 * [한국어 개요]
 * io_uring 엔진 - Linux 커널의 io_uring 인터페이스를 사용하는 fio I/O 엔진.
 *
 * === io_uring 아키텍처 개요 ===
 * io_uring은 리눅스 커널 5.1+에서 도입된 비동기 I/O 프레임워크로,
 * 기존 libaio 대비 다음과 같은 장점을 가짐:
 *
 * 1. SQ/CQ Ring 구조:
 *    - SQ (Submission Queue) Ring: 유저 -> 커널로 I/O 요청을 제출하는 링 버퍼
 *    - CQ (Completion Queue) Ring: 커널 -> 유저로 I/O 완료를 통지하는 링 버퍼
 *    - 둘 다 mmap()으로 유저/커널 간 공유 메모리에 매핑됨
 *    - 시스템 콜 없이 링 버퍼의 head/tail 포인터만으로 통신 가능
 *
 * 2. SQE (Submission Queue Entry): I/O 요청 하나를 기술하는 구조체
 *    - opcode, fd, offset, length, buffer 주소 등을 포함
 *    - SQ Ring의 array[]는 SQE 인덱스를 가리킴 (간접 참조)
 *
 * 3. CQE (Completion Queue Entry): I/O 완료 결과를 담는 구조체
 *    - user_data: 원래 SQE에서 설정한 값 (io_u 포인터로 사용)
 *    - res: I/O 결과 (성공 시 전송된 바이트 수, 실패 시 음수 에러코드)
 *
 * === libaio와의 주요 차이점 ===
 * - 커널 폴링 (IOPOLL): hipri 옵션으로 인터럽트 없이 폴링 기반 완료 확인
 * - SQ 폴링 (SQPOLL): 커널 스레드가 SQ를 지속 감시하여 시스콜 없이 제출
 * - 등록된 버퍼 (fixedbufs): 버퍼를 미리 커널에 등록하여 매번 pinning 비용 제거
 * - 등록된 파일 (registerfiles): fd를 미리 등록하여 매 I/O마다 fd lookup 비용 제거
 * - io_uring_cmd: NVMe passthrough 등 디바이스별 커맨드 직접 전달 지원
 *
 * === 주요 함수 흐름 ===
 * fio_ioring_queue()    → SQ Ring에 I/O 요청(SQE)을 추가 (tail 포인터 증가)
 * fio_ioring_commit()   → io_uring_enter() 시스콜로 커널에 제출 알림
 *                          (SQPOLL 모드에서는 NEED_WAKEUP일 때만 호출)
 * fio_ioring_getevents() → CQ Ring에서 완료된 I/O(CQE)를 수확
 *                          (CQ head 포인터를 전진시켜 커널에 소비 완료 통지)
 */
/* 표준 라이브러리 헤더 */
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>

/* fio 내부 헤더 파일들 */
#include "../fio.h"           /* fio 코어 프레임워크 */
#include "../lib/pow2.h"      /* 2의 거듭제곱 관련 유틸리티 */
#include "../optgroup.h"      /* 옵션 그룹 정의 */
#include "../lib/memalign.h"  /* 메모리 정렬 유틸리티 */
#include "../lib/fls.h"       /* find last set bit */
#include "../lib/roundup.h"   /* 반올림 유틸리티 */
#include "../verify.h"        /* 데이터 검증 기능 */

/* ARCH_HAVE_IOURING: 현재 아키텍처가 io_uring을 지원하는 경우에만 컴파일 */
#ifdef ARCH_HAVE_IOURING

#include "../lib/types.h"
#include "../os/linux/io_uring.h"  /* io_uring 커널 인터페이스 정의 (SQE, CQE, 파라미터 등) */
#include "cmdprio.h"               /* 커맨드 우선순위 지원 */
#include "zbd.h"                   /* 존 블록 디바이스(ZBD) 지원 */
#include "nvme.h"                  /* NVMe passthrough 지원 */

#include <sys/stat.h>

/*
 * IO 무결성 검사 플래그 정의
 * Protection Information(PI) 관련 플래그가 없을 경우 직접 정의
 */
#ifndef IO_INTEGRITY_CHK_GUARD
/* flags for integrity meta */
/* 무결성 메타데이터 플래그 */
#define IO_INTEGRITY_CHK_GUARD		(1U << 0) /* enforce guard check */
/* 가드 태그 검사 강제 - 데이터 무결성의 CRC 검사 */
#define IO_INTEGRITY_CHK_REFTAG		(1U << 1) /* enforce ref check */
/* 참조 태그 검사 강제 - LBA 기반 참조 태그 일치 확인 */
#define IO_INTEGRITY_CHK_APPTAG		(1U << 2) /* enforce app check */
/* 애플리케이션 태그 검사 강제 - 사용자 정의 태그 일치 확인 */
#endif /* IO_INTEGRITY_CHK_GUARD */

/*
 * FS_IOC_GETLBMD_CAP ioctl이 정의되지 않은 경우,
 * 논리 블록 메타데이터 기능(Protection Information) 관련 구조체와 상수를 정의
 */
#ifndef FS_IOC_GETLBMD_CAP
/* Protection info capability flags */
/* 보호 정보 기능 플래그 */
#define	LBMD_PI_CAP_INTEGRITY		(1 << 0)  /* 무결성 보호 지원 */
#define	LBMD_PI_CAP_REFTAG		(1 << 1)  /* 참조 태그 지원 */

/* Checksum types for Protection Information */
/* 보호 정보에 사용되는 체크섬 유형 */
#define LBMD_PI_CSUM_NONE		0         /* 체크섬 없음 */
#define LBMD_PI_CSUM_IP			1         /* IP 체크섬 */
#define LBMD_PI_CSUM_CRC16_T10DIF	2     /* T10 DIF CRC16 체크섬 */
#define LBMD_PI_CSUM_CRC64_NVME		4     /* NVMe CRC64 체크섬 */

/*
 * Logical block metadata capability descriptor
 * If the device does not support metadata, all the fields will be zero.
 * Applications must check lbmd_flags to determine whether metadata is
 * supported or not.
 */
/*
 * 논리 블록 메타데이터 기능 디스크립터
 * 디바이스가 메타데이터를 지원하지 않으면 모든 필드가 0
 * lbmd_flags를 확인하여 메타데이터 지원 여부를 판단해야 함
 */
struct logical_block_metadata_cap {
	/* Bitmask of logical block metadata capability flags */
	/* 논리 블록 메타데이터 기능 플래그의 비트마스크 */
	__u32	lbmd_flags;
	/*
	 * The amount of data described by each unit of logical block
	 * metadata
	 */
	/* 각 논리 블록 메타데이터 단위가 기술하는 데이터 크기 (보통 LBA 크기와 같음) */
	__u16	lbmd_interval;
	/*
	 * Size in bytes of the logical block metadata associated with each
	 * interval
	 */
	/* 각 interval에 연결된 논리 블록 메타데이터의 크기(바이트) */
	__u8	lbmd_size;
	/*
	 * Size in bytes of the opaque block tag associated with each
	 * interval
	 */
	/* 각 interval에 연결된 불투명 블록 태그의 크기(바이트) */
	__u8	lbmd_opaque_size;
	/*
	 * Offset in bytes of the opaque block tag within the logical block
	 * metadata
	 */
	/* 논리 블록 메타데이터 내에서 불투명 블록 태그의 오프셋(바이트) */
	__u8	lbmd_opaque_offset;
	/* Size in bytes of the T10 PI tuple associated with each interval */
	/* 각 interval에 연결된 T10 PI 튜플의 크기(바이트) */
	__u8	lbmd_pi_size;
	/* Offset in bytes of T10 PI tuple within the logical block metadata */
	/* 논리 블록 메타데이터 내에서 T10 PI 튜플의 오프셋(바이트) */
	__u8	lbmd_pi_offset;
	/* T10 PI guard tag type */
	/* T10 PI 가드 태그 유형 (CRC16, CRC64 등) */
	__u8	lbmd_guard_tag_type;
	/* Size in bytes of the T10 PI application tag */
	/* T10 PI 애플리케이션 태그의 크기(바이트) */
	__u8	lbmd_app_tag_size;
	/* Size in bytes of the T10 PI reference tag */
	/* T10 PI 참조 태그의 크기(바이트) */
	__u8	lbmd_ref_tag_size;
	/* Size in bytes of the T10 PI storage tag */
	/* T10 PI 스토리지 태그의 크기(바이트) */
	__u8	lbmd_storage_tag_size;
	__u8	pad;  /* 패딩 바이트 (정렬용) */
};

/* 논리 블록 메타데이터 기능을 조회하는 ioctl 번호 정의 */
#define FS_IOC_GETLBMD_CAP			_IOWR(0x15, 2, struct logical_block_metadata_cap)
#endif /* FS_IOC_GETLBMD_CAP */

/*
 * uring_cmd 타입 열거형
 * io_uring_cmd로 전달할 수 있는 커맨드 유형 정의
 * 현재는 NVMe passthrough만 지원
 */
enum uring_cmd_type {
	FIO_URING_CMD_NVME = 1,  /* NVMe 패스스루 커맨드 */
};

/*
 * uring_cmd 쓰기 모드 열거형
 * NVMe uring_cmd 엔진에서 사용 가능한 쓰기 커맨드 유형
 */
enum uring_cmd_write_mode {
	FIO_URING_CMD_WMODE_WRITE = 1,   /* 일반 Write 커맨드 */
	FIO_URING_CMD_WMODE_UNCOR,       /* Write Uncorrectable - 해당 LBA를 읽기 불가능 상태로 마킹 */
	FIO_URING_CMD_WMODE_ZEROES,      /* Write Zeroes - 해당 LBA 범위를 0으로 채움 (데이터 전송 불필요) */
	FIO_URING_CMD_WMODE_VERIFY,      /* Verify - 디바이스 측 데이터 검증 */
};

/*
 * uring_cmd 검증 모드 열거형
 * 데이터 검증 단계에서 사용할 NVMe 커맨드 유형
 */
enum uring_cmd_verify_mode {
	FIO_URING_CMD_VMODE_READ = 1,    /* 일반 Read 커맨드로 읽어서 호스트에서 비교 */
	FIO_URING_CMD_VMODE_COMPARE,     /* NVMe Compare 커맨드로 디바이스 측에서 직접 비교 */
};

/*
 * SQ (Submission Queue) Ring 구조체
 *
 * io_uring의 제출 큐 링 버퍼를 유저스페이스에서 접근하기 위한 포인터 모음.
 * 커널과 유저가 mmap()으로 공유하는 메모리 영역을 가리킴.
 *
 * [SQ Ring 동작 원리]
 * - 유저는 tail을 증가시키며 새 SQE 인덱스를 array[]에 추가 (제출)
 * - 커널은 head를 증가시키며 SQE를 소비 (처리)
 * - tail - head = 현재 대기 중인 제출 수
 * - ring_mask를 사용하여 인덱스를 링 크기로 래핑 (& 연산)
 *
 * libaio와의 차이: libaio는 io_submit() 시스콜로 매번 iocb 배열을 복사하지만,
 * io_uring은 공유 메모리의 포인터만 업데이트하므로 복사 비용이 없음.
 */
struct io_sq_ring {
	unsigned *head;          /* 커널이 소비한 위치 (커널이 업데이트) */
	unsigned *tail;          /* 유저가 제출한 위치 (유저가 업데이트) */
	unsigned *ring_mask;     /* 링 인덱스 마스크 (entries - 1, 2의 거듭제곱) */
	unsigned *ring_entries;  /* 링의 총 엔트리 수 */
	unsigned *flags;         /* 링 플래그 (예: IORING_SQ_NEED_WAKEUP - SQPOLL 스레드 깨우기 필요) */
	unsigned *array;         /* SQE 인덱스 배열 - array[tail & mask] = sqe_index */
};

/*
 * CQ (Completion Queue) Ring 구조체
 *
 * io_uring의 완료 큐 링 버퍼를 유저스페이스에서 접근하기 위한 포인터 모음.
 *
 * [CQ Ring 동작 원리]
 * - 커널은 tail을 증가시키며 CQE를 추가 (완료 통지)
 * - 유저는 head를 증가시키며 CQE를 소비 (결과 수확)
 * - tail - head = 현재 수확 가능한 완료 이벤트 수
 *
 * libaio와의 차이: libaio는 io_getevents()로 이벤트를 커널에서 유저로 복사하지만,
 * io_uring은 CQE가 이미 공유 메모리에 있으므로 복사 비용이 없음.
 */
struct io_cq_ring {
	unsigned *head;                  /* 유저가 소비한 위치 (유저가 업데이트) */
	unsigned *tail;                  /* 커널이 완료를 추가한 위치 (커널이 업데이트) */
	unsigned *ring_mask;             /* 링 인덱스 마스크 */
	unsigned *ring_entries;          /* 링의 총 엔트리 수 */
	struct io_uring_cqe *cqes;       /* CQE 배열 포인터 - 실제 완료 결과가 저장됨 */
};

/*
 * mmap 매핑 정보 구조체
 * io_uring 셋업 시 커널과 공유하기 위해 mmap()한 영역의 정보를 보관
 * cleanup 시 munmap()에 필요한 포인터와 크기를 저장
 */
struct ioring_mmap {
	void *ptr;    /* mmap()으로 매핑된 메모리 주소 */
	size_t len;   /* 매핑된 메모리 크기 */
};

/*
 * io_uring 엔진의 핵심 데이터 구조체
 * 각 fio 스레드(thread_data)마다 하나씩 생성되며,
 * io_uring 인스턴스의 모든 상태를 보관
 */
struct ioring_data {
	int ring_fd;              /* io_uring 인스턴스의 파일 디스크립터 (io_uring_setup()이 반환) */

	struct io_u **io_u_index; /* io_u 포인터 배열 - 인덱스로 io_u를 빠르게 찾기 위함 */
	char *md_buf;             /* 메타데이터 버퍼 - PI(Protection Information) 사용 시 */
	char *pi_attr;            /* PI 속성 버퍼 - 각 I/O별 PI 속성 저장 */

	int *fds;                 /* 등록된 파일 디스크립터 배열 (registerfiles 옵션 사용 시) */

	struct io_sq_ring sq_ring;    /* SQ Ring 포인터들 */
	struct io_uring_sqe *sqes;    /* SQE 배열 - 실제 I/O 요청 기술자가 저장되는 공간 */
	struct iovec *iovecs;         /* I/O 벡터 배열 - scatter/gather I/O용 버퍼 기술자 */
	unsigned sq_ring_mask;        /* SQ Ring 마스크의 로컬 캐시 (빠른 접근용) */

	struct io_cq_ring cq_ring;    /* CQ Ring 포인터들 */
	unsigned cq_ring_mask;        /* CQ Ring 마스크의 로컬 캐시 */

	int async_trim_fail;      /* 비동기 TRIM이 실패했는지 여부 (실패 시 동기 모드로 폴백) */
	int queued;               /* 현재 SQ에 추가되었지만 아직 커널에 제출되지 않은 I/O 수 */
	int cq_ring_off;          /* CQ Ring에서 이벤트 수확 시작 오프셋 */
	unsigned iodepth;         /* io_uring 내부 큐 깊이 (2의 거듭제곱으로 반올림됨) */
	int prepped;              /* force_async 카운터 - N번째마다 IOSQE_ASYNC 설정 */

	struct ioring_mmap mmap[3];   /* mmap 매핑 정보: [0]=SQ Ring, [1]=SQEs, [2]=CQ Ring */

	struct cmdprio cmdprio;       /* 커맨드 우선순위 설정 */

	struct nvme_dsm *dsm;             /* NVMe Dataset Management (TRIM/DSM) 구조체 */
	uint32_t cdw12_flags[DDIR_RWDIR_CNT]; /* NVMe CDW12 플래그 (FUA 등) - 읽기/쓰기 방향별 */
	uint8_t write_opcode;             /* NVMe 쓰기 opcode (write, write_uncor, write_zeroes, verify) */

	bool is_uring_cmd_eng;            /* io_uring_cmd 엔진 여부 (NVMe passthrough 구분용) */

	struct nvme_cmd_ext_io_opts ext_opts;  /* NVMe 확장 I/O 옵션 (PI 관련) */
};

/*
 * io_uring 엔진 옵션 구조체
 * fio 설정 파일에서 사용자가 지정할 수 있는 io_uring 관련 옵션들
 */
struct ioring_options {
	struct thread_data *td;           /* 소속 스레드 데이터 포인터 */
	unsigned int hipri;               /* IOPOLL 사용 여부 - 인터럽트 없이 폴링으로 완료 확인 */
	unsigned int readfua;             /* 읽기 시 FUA(Force Unit Access) 플래그 사용 여부 - 캐시 우회 */
	unsigned int writefua;            /* 쓰기 시 FUA 플래그 사용 여부 */
	unsigned int deac;                /* Write Zeroes 시 DEAC(Deallocate) 플래그 사용 여부 */
	unsigned int write_mode;          /* 쓰기 모드 (write, uncor, zeroes, verify) */
	unsigned int verify_mode;         /* 검증 모드 (read, compare) */
	struct cmdprio_options cmdprio_options;  /* 커맨드 우선순위 옵션 */
	unsigned int fixedbufs;           /* 사전 등록 버퍼 사용 - 커널 버퍼 pinning 비용 제거 */
	unsigned int registerfiles;       /* 사전 등록 파일 사용 - fd lookup 비용 제거 */
	unsigned int sqpoll_thread;       /* SQ 폴링 스레드 사용 - 커널 스레드가 SQ를 지속 감시 */
	unsigned int sqpoll_set;          /* SQ 폴링 CPU가 명시적으로 설정되었는지 여부 */
	unsigned int sqpoll_cpu;          /* SQ 폴링 스레드를 고정할 CPU 번호 */
	unsigned int nonvectored;         /* 비벡터 I/O 사용 (IORING_OP_READ/WRITE vs READV/WRITEV) */
	unsigned int uncached;            /* RWF_DONTCACHE 플래그 사용 (버퍼드 I/O 시 캐시 미사용) */
	unsigned int nowait;              /* RWF_NOWAIT 플래그 사용 (블로킹 없이 즉시 반환) */
	unsigned int force_async;         /* N번째 요청마다 IOSQE_ASYNC 강제 (비동기 처리 강제) */
	unsigned int md_per_io_size;      /* I/O당 별도 메타데이터 버퍼 크기 */
	unsigned int pi_act;              /* PI Action 비트 - 1이면 컨트롤러가 PI 처리 */
	unsigned int apptag;              /* PI의 Application Tag 값 */
	unsigned int apptag_mask;         /* Application Tag 마스크 */
	unsigned int prchk;               /* PI 검사 플래그 (GUARD, REFTAG, APPTAG 조합) */
	char *pi_chk;                     /* PI 검사 문자열 옵션 ("GUARD,REFTAG,APPTAG") */
	enum uring_cmd_type cmd_type;     /* uring_cmd 타입 (현재 NVMe만) */
};

/*
 * io_uring_enter() 호출 시 사용할 기본 플래그
 * IORING_ENTER_GETEVENTS: 완료 이벤트를 기다림
 * (이후 IORING_FEAT_NO_IOWAIT가 지원되면 IORING_ENTER_NO_IOWAIT도 추가됨)
 */
static unsigned int enter_flags = IORING_ENTER_GETEVENTS;

/*
 * I/O 방향(읽기/쓰기)과 벡터/비벡터 여부에 따른 opcode 매핑 테이블
 * [ddir][nonvectored]: ddir=0(READ), ddir=1(WRITE)
 *                      nonvectored=0(벡터), nonvectored=1(비벡터)
 */
static const int ddir_to_op[2][2] = {
	{ IORING_OP_READV, IORING_OP_READ },    /* 읽기: 벡터=READV, 비벡터=READ */
	{ IORING_OP_WRITEV, IORING_OP_WRITE }   /* 쓰기: 벡터=WRITEV, 비벡터=WRITE */
};

/*
 * 고정 버퍼(fixedbufs) 사용 시 I/O 방향별 opcode 매핑
 * 미리 등록된 버퍼를 사용하므로 별도의 opcode 필요
 */
static const int fixed_ddir_to_op[2] = {
	IORING_OP_READ_FIXED,   /* 고정 버퍼 읽기 */
	IORING_OP_WRITE_FIXED   /* 고정 버퍼 쓰기 */
};

/*
 * [함수] fio_ioring_sqpoll_cb
 * [역할] sqthread_poll_cpu 옵션의 콜백 함수.
 *        사용자가 SQ 폴링 스레드의 CPU를 지정했을 때 호출됨.
 * [파라미터]
 *   - data: ioring_options 포인터
 *   - val: 사용자가 지정한 CPU 번호
 * [반환값] 항상 0 (성공)
 */
static int fio_ioring_sqpoll_cb(void *data, unsigned long long *val)
{
	struct ioring_options *o = data;

	o->sqpoll_cpu = *val;   /* SQ 폴링 CPU 번호 저장 */
	o->sqpoll_set = 1;      /* CPU가 명시적으로 설정되었음을 표시 */
	return 0;
}

/*
 * fio 옵션 정의 배열
 * io_uring 엔진에서 사용 가능한 모든 설정 옵션을 정의
 * 각 옵션은 fio 설정 파일이나 커맨드라인에서 사용 가능
 */
static struct fio_option options[] = {
	{
		/* hipri: IOPOLL 모드 활성화 - 인터럽트 대신 폴링으로 완료 확인하여 지연시간 감소 */
		.name	= "hipri",
		.lname	= "High Priority",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, hipri),
		.help	= "Use polled IO completions",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* readfua: 읽기 시 FUA(Force Unit Access) 플래그 설정 - 캐시를 우회하여 미디어에서 직접 읽기 */
		.name	= "readfua",
		.lname	= "Read fua flag support",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, readfua),
		.help	= "Set FUA flag (force unit access) for all Read operations",
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* writefua: 쓰기 시 FUA 플래그 설정 - 데이터가 비휘발성 미디어에 도달했음을 보장 */
		.name	= "writefua",
		.lname	= "Write fua flag support",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, writefua),
		.help	= "Set FUA flag (force unit access) for all Write operations",
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* write_mode: NVMe 쓰기 커맨드 유형 선택 */
		.name	= "write_mode",
		.lname	= "Additional Write commands support (Write Uncorrectable, Write Zeores)",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct ioring_options, write_mode),
		.help	= "Issue Write Uncorrectable or Zeroes command instead of Write command",
		.def	= "write",
		.posval = {
			  { .ival = "write",
			    .oval = FIO_URING_CMD_WMODE_WRITE,
			    .help = "Issue Write commands for write operations"
			    /* 일반 Write 커맨드 발행 */
			  },
			  { .ival = "uncor",
			    .oval = FIO_URING_CMD_WMODE_UNCOR,
			    .help = "Issue Write Uncorrectable commands for write operations"
			    /* Write Uncorrectable 커맨드 - LBA를 읽기 불가능 상태로 마킹 */
			  },
			  { .ival = "zeroes",
			    .oval = FIO_URING_CMD_WMODE_ZEROES,
			    .help = "Issue Write Zeroes commands for write operations"
			    /* Write Zeroes 커맨드 - 데이터 전송 없이 0으로 채움 */
			  },
			  { .ival = "verify",
			    .oval = FIO_URING_CMD_WMODE_VERIFY,
			    .help = "Issue Verify commands for write operations"
			    /* Verify 커맨드 - 디바이스 측 데이터 검증 */
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* verify_mode: 검증 단계에서 사용할 커맨드 유형 */
		.name	= "verify_mode",
		.lname	= "Do verify based on the configured command (e.g., Read or Compare command)",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct ioring_options, verify_mode),
		.help	= "Issue Read or Compare command in the verification phase",
		.def	= "read",
		.posval = {
			  { .ival = "read",
			    .oval = FIO_URING_CMD_VMODE_READ,
			    .help = "Issue Read commands in the verification phase"
			    /* Read로 읽어서 호스트에서 비교 */
			  },
			  { .ival = "compare",
			    .oval = FIO_URING_CMD_VMODE_COMPARE,
			    .help = "Issue Compare commands in the verification phase"
			    /* NVMe Compare 커맨드로 디바이스 측에서 비교 */
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/*
		 * fixedbufs: 사전 등록 버퍼 사용
		 *
		 * libaio와의 차이점:
		 * - libaio는 매 I/O마다 유저 버퍼를 커널에 pin/unpin해야 함
		 * - io_uring의 fixedbufs는 초기화 시 한 번만 등록하면
		 *   이후 I/O에서 pin/unpin 비용이 없음
		 * - IORING_OP_READ_FIXED / IORING_OP_WRITE_FIXED 사용
		 */
		.name	= "fixedbufs",
		.lname	= "Fixed (pre-mapped) IO buffers",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, fixedbufs),
		.help	= "Pre map IO buffers",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/*
		 * registerfiles: 사전 등록 파일
		 *
		 * libaio와의 차이점:
		 * - libaio는 매 I/O마다 fd에서 struct file을 lookup해야 함
		 * - io_uring의 registerfiles는 fd를 미리 등록하여
		 *   매 I/O에서 fd lookup + atomic refcount 비용을 제거
		 * - SQE에서 fd 대신 등록된 인덱스를 사용 (IOSQE_FIXED_FILE 플래그)
		 */
		.name	= "registerfiles",
		.lname	= "Register file set",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, registerfiles),
		.help	= "Pre-open/register files",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/*
		 * sqthread_poll: 커널 SQ 폴링 스레드 사용
		 *
		 * libaio와의 차이점:
		 * - libaio는 io_submit() 시스콜로 매번 유저→커널 컨텍스트 전환 필요
		 * - io_uring의 SQPOLL은 커널 스레드가 SQ를 지속 감시하므로
		 *   시스콜 없이 SQ tail만 업데이트하면 커널이 자동으로 I/O를 처리
		 * - 유휴 시간이 길면 커널 스레드가 sleep하고, IORING_SQ_NEED_WAKEUP
		 *   플래그가 설정됨 → 이때만 io_uring_enter() 호출
		 */
		.name	= "sqthread_poll",
		.lname	= "Kernel SQ thread polling",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, sqpoll_thread),
		.help	= "Offload submission/completion to kernel thread",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* sqthread_poll_cpu: SQ 폴링 커널 스레드를 고정할 CPU 코어 번호 */
		.name	= "sqthread_poll_cpu",
		.lname	= "SQ Thread Poll CPU",
		.type	= FIO_OPT_INT,
		.cb	= fio_ioring_sqpoll_cb,
		.help	= "What CPU to run SQ thread polling on",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/*
		 * nonvectored: 비벡터 I/O 사용 여부
		 * -1: 자동 탐지 (커널이 IORING_OP_READ/WRITE 지원하면 사용)
		 *  0: 벡터 I/O (READV/WRITEV) 사용
		 *  1: 비벡터 I/O (READ/WRITE) 사용 - iovec 오버헤드 없음
		 */
		.name	= "nonvectored",
		.lname	= "Non-vectored",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, nonvectored),
		.def	= "-1",
		.help	= "Use non-vectored read/write commands",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* uncached: RWF_DONTCACHE 플래그 사용 - 버퍼드 I/O에서 페이지 캐시 오염 방지 */
		.name	= "uncached",
		.lname	= "Uncached",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, uncached),
		.help	= "Use RWF_DONTCACHE for buffered read/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* nowait: RWF_NOWAIT 플래그 - 블로킹 없이 즉시 반환, 불가능하면 -EAGAIN */
		.name	= "nowait",
		.lname	= "RWF_NOWAIT",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, nowait),
		.help	= "Use RWF_NOWAIT for reads/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/*
		 * force_async: N번째 요청마다 IOSQE_ASYNC 플래그 강제 설정
		 * 이 플래그가 설정된 SQE는 인라인 처리 대신 항상 비동기 워커에서 실행됨
		 * 테스트/디버깅 용도로 유용
		 */
		.name	= "force_async",
		.lname	= "Force async",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, force_async),
		.help	= "Set IOSQE_ASYNC every N requests",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* cmd_type: io_uring_cmd로 전달할 커맨드 유형 (현재 NVMe만 지원) */
		.name	= "cmd_type",
		.lname	= "Uring cmd type",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct ioring_options, cmd_type),
		.help	= "Specify uring-cmd type",
		.def	= "nvme",
		.posval = {
			  { .ival = "nvme",
			    .oval = FIO_URING_CMD_NVME,
			    .help = "Issue nvme-uring-cmd",
			    /* NVMe uring_cmd 패스스루 사용 */
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	/* 커맨드 우선순위 관련 옵션 매크로 확장 (cmdprio_percentage, cmdprio_bssplit 등) */
	CMDPRIO_OPTIONS(struct ioring_options, FIO_OPT_G_IOURING),
	{
		/* md_per_io_size: I/O당 별도 메타데이터 버퍼 크기 (NVMe PI 사용 시) */
		.name	= "md_per_io_size",
		.lname	= "Separate Metadata Buffer Size per I/O",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, md_per_io_size),
		.def	= "0",
		.help	= "Size of separate metadata buffer per I/O (Default: 0)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* pi_act: Protection Information Action 비트
		 * 1이면 컨트롤러가 PI를 자동 생성/검증 (호스트가 직접 할 필요 없음)
		 * 0이면 호스트가 PI를 직접 생성하고 검증해야 함 */
		.name	= "pi_act",
		.lname	= "Protection Information Action",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, pi_act),
		.def	= "1",
		.help	= "Protection Information Action bit (pi_act=1 or pi_act=0)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* pi_chk: 어떤 PI 필드를 검사할지 지정 ("GUARD,REFTAG,APPTAG" 형식) */
		.name	= "pi_chk",
		.lname	= "Protection Information Check",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct ioring_options, pi_chk),
		.def	= NULL,
		.help	= "Control of Protection Information Checking (pi_chk=GUARD,REFTAG,APPTAG)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* apptag: PI에 사용할 Application Tag 값 (기본값 0x1234) */
		.name	= "apptag",
		.lname	= "Application Tag used in Protection Information",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, apptag),
		.def	= "0x1234",
		.help	= "Application Tag used in Protection Information field (Default: 0x1234)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* apptag_mask: Application Tag와 함께 사용할 마스크 (기본값 0xffff = 전체 비교) */
		.name	= "apptag_mask",
		.lname	= "Application Tag Mask",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, apptag_mask),
		.def	= "0xffff",
		.help	= "Application Tag Mask used with Application Tag (Default: 0xffff)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* deac: Write Zeroes 커맨드에 DEAC(Deallocate) 비트 설정
		 * 설정 시 컨트롤러가 해당 LBA 범위를 할당 해제할 수 있음 (SSD TRIM과 유사) */
		.name	= "deac",
		.lname	= "Deallocate bit for write zeroes command",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, deac),
		.help	= "Set DEAC (deallocate) flag for write zeroes command",
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		.name	= NULL,  /* 옵션 배열 종료 표시 */
	},
};

/*
 * [함수] io_uring_enter
 * [역할] io_uring_enter 시스템 콜을 호출하여 커널에 I/O 제출을 알리거나,
 *        완료된 I/O를 기다림.
 *
 * io_uring의 핵심 시스콜로, 다음 두 가지 용도로 사용:
 * 1. I/O 제출: to_submit > 0일 때, SQ Ring에 추가된 SQE들을 커널에 알림
 * 2. 완료 대기: min_complete > 0일 때, 최소 N개의 완료를 기다림
 *
 * libaio와의 차이: libaio는 io_submit()과 io_getevents() 두 개의 시스콜이 필요하지만,
 * io_uring은 io_uring_enter() 하나로 제출과 완료 대기를 동시에 할 수 있음.
 * SQPOLL 모드에서는 이 시스콜조차 필요 없는 경우가 대부분.
 *
 * [파라미터]
 *   - ld: io_uring 데이터 (ring_fd 포함)
 *   - to_submit: 제출할 SQE 수
 *   - min_complete: 최소 완료 대기 수 (0이면 기다리지 않음)
 *   - flags: 동작 플래그 (IORING_ENTER_GETEVENTS, IORING_ENTER_SQ_WAKEUP 등)
 * [반환값] 성공 시 제출된 SQE 수, 실패 시 음수 에러코드
 */
static int io_uring_enter(struct ioring_data *ld, unsigned int to_submit,
			 unsigned int min_complete, unsigned int flags)
{
#ifdef FIO_ARCH_HAS_SYSCALL
	/* 아키텍처별 직접 시스콜 호출 (glibc 우회, 약간 더 빠름) */
	return __do_syscall6(__NR_io_uring_enter, ld->ring_fd, to_submit,
				min_complete, flags, NULL, 0);
#else
	/* 표준 syscall() 래퍼 사용 */
	return syscall(__NR_io_uring_enter, ld->ring_fd, to_submit,
			min_complete, flags, NULL, 0);
#endif
}

/* 블록 디바이스 io_uring_cmd DISCARD(TRIM) 커맨드 정의 */
#ifndef BLOCK_URING_CMD_DISCARD
#define BLOCK_URING_CMD_DISCARD	_IO(0x12, 0)
#endif

/*
 * [함수] fio_ioring_prep_md
 * [역할] 메타데이터(PI, Protection Information)가 있는 I/O 요청의 SQE에
 *        메타데이터 관련 속성을 설정함.
 *        io_uring 경로(비 NVMe passthrough)에서 메타데이터를 전달할 때 사용.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛 (요청 단위)
 * [반환값] 없음
 */
static void fio_ioring_prep_md(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;        /* io_uring 엔진 데이터 */
	struct io_uring_attr_pi *pi_attr = io_u->pi_attr; /* 해당 io_u의 PI 속성 */
	struct nvme_data *data = FILE_ENG_DATA(io_u->file); /* 파일의 NVMe 데이터 */
	struct io_uring_sqe *sqe;

	/* 해당 io_u 인덱스에 대응하는 SQE를 가져옴 */
	sqe = &ld->sqes[io_u->index];

	/* SQE에 PI 속성 타입 마스크와 포인터를 설정 */
	sqe->attr_type_mask = IORING_RW_ATTR_FLAG_PI;
	sqe->attr_ptr = (__u64)(uintptr_t)pi_attr;
	/* 메타데이터 버퍼 주소 설정 (mmap_data는 io_u_init에서 할당된 메타데이터 영역) */
	pi_attr->addr = (__u64)(uintptr_t)io_u->mmap_data;

	/* 참조 태그(REFTAG) 검사가 활성화된 경우, seed를 시작 LBA로 설정 */
	if (pi_attr->flags & IO_INTEGRITY_CHK_REFTAG) {
		__u64 slba = get_slba(data, io_u->offset);  /* 오프셋을 시작 LBA로 변환 */
		pi_attr->seed = (__u32)slba;
	}
}

/*
 * [함수] fio_ioring_prep
 * [역할] 일반 io_uring 엔진(비 NVMe passthrough)에서 I/O 요청을 위한 SQE를 준비.
 *        fio의 io_u를 io_uring의 SQE로 변환하는 핵심 함수.
 *
 * [I/O 흐름에서의 위치]
 * fio core에서 io_u를 할당 → fio_ioring_prep()으로 SQE 준비
 * → fio_ioring_queue()로 SQ Ring에 추가 → fio_ioring_commit()으로 커널에 제출
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 준비할 I/O 유닛
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_prep(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;   /* io_uring 엔진 데이터 */
	struct ioring_options *o = td->eo;           /* 엔진 옵션 */
	struct fio_file *f = io_u->file;             /* 대상 파일 */
	struct io_uring_sqe *sqe;

	/* io_u 인덱스에 해당하는 SQE를 가져옴 */
	sqe = &ld->sqes[io_u->index];

	/* 등록된 파일 사용 여부에 따라 fd 설정 방식이 달라짐 */
	if (o->registerfiles) {
		sqe->fd = f->engine_pos;            /* 등록된 파일 인덱스 사용 */
		sqe->flags = IOSQE_FIXED_FILE;     /* 등록된 파일임을 표시 */
	} else {
		sqe->fd = f->fd;                    /* 일반 파일 디스크립터 사용 */
		sqe->flags = 0;
	}

	/* 읽기 또는 쓰기 I/O인 경우 */
	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {
		if (o->fixedbufs) {
			/* 고정(사전 등록) 버퍼 사용 경로
			 * 커널에 미리 등록된 버퍼를 사용하므로 pin/unpin 오버헤드 없음 */
			sqe->opcode = fixed_ddir_to_op[io_u->ddir]; /* READ_FIXED or WRITE_FIXED */
			sqe->addr = (unsigned long) io_u->xfer_buf;  /* 버퍼 주소 */
			sqe->len = io_u->xfer_buflen;                /* 전송 크기 */
			sqe->buf_index = io_u->index;                /* 등록된 버퍼의 인덱스 */
		} else {
			/* 일반 버퍼 사용 경로 */
			struct iovec *iov = &ld->iovecs[io_u->index];

			/*
			 * Update based on actual io_u, requeue could have
			 * adjusted these
			 */
			/* 실제 io_u 기반으로 업데이트 - 재큐잉 시 조정될 수 있음 */
			iov->iov_base = io_u->xfer_buf;      /* 전송 버퍼 주소 */
			iov->iov_len = io_u->xfer_buflen;    /* 전송 버퍼 크기 */

			/* 벡터/비벡터 여부에 따라 opcode 선택 */
			sqe->opcode = ddir_to_op[io_u->ddir][!!o->nonvectored];
			if (o->nonvectored) {
				/* 비벡터: 직접 주소/크기 전달 (iovec 오버헤드 없음) */
				sqe->addr = (unsigned long) iov->iov_base;
				sqe->len = iov->iov_len;
			} else {
				/* 벡터: iovec 포인터 전달 (scatter/gather I/O 가능) */
				sqe->addr = (unsigned long) iov;
				sqe->len = 1;  /* iovec 개수: 1개 */
			}
		}
		/* 메타데이터가 있으면 PI 속성 설정 */
		if (o->md_per_io_size)
			fio_ioring_prep_md(td, io_u);
		/* 읽기/쓰기 플래그 초기화 */
		sqe->rw_flags = 0;
		/* 버퍼드 I/O에서 캐시 미사용(RWF_DONTCACHE) 설정 */
		if (!td->o.odirect && o->uncached)
			sqe->rw_flags |= RWF_DONTCACHE;
		/* 논블로킹 I/O(RWF_NOWAIT) 설정 */
		if (o->nowait)
			sqe->rw_flags |= RWF_NOWAIT;
		/* 원자적 쓰기(RWF_ATOMIC) 설정 - 쓰기 방향일 때만 */
		if (td->o.oatomic && io_u->ddir == DDIR_WRITE)
			sqe->rw_flags |= RWF_ATOMIC;

		/*
		 * Since io_uring can have a submission context (sqthread_poll)
		 * that is different from the process context, we cannot rely on
		 * the IO priority set by ioprio_set() (options prio, prioclass,
		 * and priohint) to be inherited.
		 * td->ioprio will have the value of the "default prio", so set
		 * this unconditionally. This value might get overridden by
		 * fio_ioring_cmdprio_prep() if the option cmdprio_percentage or
		 * cmdprio_bssplit is used.
		 */
		/*
		 * io_uring은 제출 컨텍스트(SQPOLL 스레드)가 프로세스 컨텍스트와
		 * 다를 수 있으므로, ioprio_set()으로 설정한 I/O 우선순위가
		 * 상속되지 않을 수 있음.
		 * 따라서 SQE에 I/O 우선순위를 무조건 명시적으로 설정함.
		 * cmdprio_percentage/cmdprio_bssplit 옵션 사용 시
		 * fio_ioring_cmdprio_prep()에서 이 값이 재정의될 수 있음.
		 */
		sqe->ioprio = td->ioprio;
		sqe->off = io_u->offset;     /* 파일 내 오프셋 */
	} else if (ddir_sync(io_u->ddir)) {
		/* 동기화(sync) 계열 I/O인 경우 */
		sqe->ioprio = 0;
		if (io_u->ddir == DDIR_SYNC_FILE_RANGE) {
			/* sync_file_range: 특정 파일 범위만 동기화 */
			sqe->off = f->first_write;                  /* 동기화 시작 오프셋 */
			sqe->len = f->last_write - f->first_write;  /* 동기화 범위 길이 */
			sqe->sync_range_flags = td->o.sync_file_range; /* 동기화 플래그 */
			sqe->opcode = IORING_OP_SYNC_FILE_RANGE;
		} else {
			/* fsync / fdatasync */
			sqe->off = 0;
			sqe->addr = 0;
			sqe->len = 0;
			if (io_u->ddir == DDIR_DATASYNC)
				sqe->fsync_flags |= IORING_FSYNC_DATASYNC; /* fdatasync: 메타데이터 제외 */
			sqe->opcode = IORING_OP_FSYNC;
		}
	} else if (io_u->ddir == DDIR_TRIM) {
		/* TRIM(Discard) 요청 - 블록 디바이스의 io_uring_cmd DISCARD 사용 */
		sqe->opcode = IORING_OP_URING_CMD;
		sqe->addr = io_u->offset;           /* TRIM 시작 오프셋 */
		sqe->addr3 = io_u->xfer_buflen;     /* TRIM 길이 */
		sqe->rw_flags = 0;
		sqe->len = sqe->off = 0;
		sqe->ioprio = 0;
		sqe->cmd_op = BLOCK_URING_CMD_DISCARD;  /* 블록 DISCARD 커맨드 */
		sqe->__pad1 = 0;
		sqe->file_index = 0;
	}

	/* force_async: N번째 요청마다 IOSQE_ASYNC 플래그를 설정하여 비동기 처리 강제 */
	if (o->force_async && ++ld->prepped == o->force_async) {
		ld->prepped = 0;              /* 카운터 리셋 */
		sqe->flags |= IOSQE_ASYNC;   /* 이 SQE는 반드시 비동기 워커에서 처리 */
	}

	/* SQE의 user_data에 io_u 포인터를 저장
	 * → CQE에서 이 값으로 어떤 I/O가 완료되었는지 식별 */
	sqe->user_data = (unsigned long) io_u;
	return 0;
}

/*
 * [함수] fio_ioring_cmd_prep
 * [역할] io_uring_cmd 엔진(NVMe passthrough)에서 I/O 요청의 SQE를 준비.
 *        NVMe 커맨드를 SQE의 cmd 필드에 직접 채워넣음.
 *
 * io_uring_cmd는 io_uring을 통해 디바이스별 커맨드를 직접 전달하는 메커니즘.
 * NVMe passthrough에서는 NVMe I/O 커맨드(Read, Write, Compare 등)를
 * 커널의 블록 레이어를 우회하여 NVMe 드라이버에 직접 전달함.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 준비할 I/O 유닛
 * [반환값] 0 (성공), -EINVAL (지원하지 않는 cmd_type)
 */
static int fio_ioring_cmd_prep(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct fio_file *f = io_u->file;
	struct nvme_uring_cmd *cmd;        /* NVMe uring 커맨드 구조체 */
	struct io_uring_sqe *sqe;
	struct nvme_dsm *dsm;              /* NVMe Dataset Management 구조체 (TRIM용) */
	void *ptr = ld->dsm;
	unsigned int dsm_size;
	uint8_t read_opcode = nvme_cmd_read;  /* 기본 읽기 opcode */

	/* only supports nvme_uring_cmd */
	/* NVMe uring_cmd만 지원 - 다른 유형이면 에러 반환 */
	if (o->cmd_type != FIO_URING_CMD_NVME)
		return -EINVAL;

	/* TRIM 요청이 동기 TRIM 모드인 경우, prep 단계를 건너뜀 (동기 처리됨) */
	if (io_u->ddir == DDIR_TRIM && td->io_ops->flags & FIO_ASYNCIO_SYNC_TRIM)
		return 0;

	/* NVMe는 128바이트 SQE 사용 (IORING_SETUP_SQE128) → 인덱스를 2배로 */
	sqe = &ld->sqes[(io_u->index) << 1];

	/* 등록된 파일 사용 여부에 따라 fd 설정 */
	if (o->registerfiles) {
		sqe->fd = f->engine_pos;
		sqe->flags = IOSQE_FIXED_FILE;
	} else {
		sqe->fd = f->fd;
	}
	sqe->rw_flags = 0;
	/* 캐시 미사용 플래그 */
	if (!td->o.odirect && o->uncached)
		sqe->rw_flags |= RWF_DONTCACHE;
	/* 논블로킹 플래그 */
	if (o->nowait)
		sqe->rw_flags |= RWF_NOWAIT;

	/* io_uring_cmd opcode 설정 */
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->user_data = (unsigned long) io_u;  /* 완료 시 io_u 식별용 */
	/* 벡터/비벡터에 따라 NVMe uring cmd 유형 선택 */
	if (o->nonvectored)
		sqe->cmd_op = NVME_URING_CMD_IO;       /* 비벡터 NVMe I/O */
	else
		sqe->cmd_op = NVME_URING_CMD_IO_VEC;   /* 벡터 NVMe I/O */
	/* force_async 처리 */
	if (o->force_async && ++ld->prepped == o->force_async) {
		ld->prepped = 0;
		sqe->flags |= IOSQE_ASYNC;
	}
	/* 고정 버퍼 사용 시 uring_cmd 고정 버퍼 플래그 설정 */
	if (o->fixedbufs) {
		sqe->uring_cmd_flags = IORING_URING_CMD_FIXED;
		sqe->buf_index = io_u->index;
	}

	/* SQE의 cmd 필드에서 NVMe 커맨드 구조체를 가져옴 (128바이트 SQE의 확장 영역) */
	cmd = (struct nvme_uring_cmd *)sqe->cmd;
	/* DSM(Dataset Management) 구조체 크기 계산 및 해당 io_u의 DSM 포인터 계산 */
	dsm_size = sizeof(*ld->dsm) + td->o.num_range * sizeof(struct nvme_dsm_range);
	ptr += io_u->index * dsm_size;
	dsm = (struct nvme_dsm *)ptr;

	/*
	 * If READ command belongs to the verification phase and the
	 * verify_mode=compare, convert READ to COMPARE command.
	 */
	/*
	 * 검증 단계의 READ 커맨드이고 verify_mode=compare인 경우,
	 * READ를 NVMe COMPARE 커맨드로 변환.
	 * COMPARE는 호스트 데이터와 디바이스 데이터를 디바이스 측에서 직접 비교.
	 */
	if (io_u->flags & IO_U_F_VER_LIST && io_u->ddir == DDIR_READ &&
			o->verify_mode == FIO_URING_CMD_VMODE_COMPARE) {
		populate_verify_io_u(td, io_u);          /* 검증 데이터 채우기 */
		read_opcode = nvme_cmd_compare;          /* READ → COMPARE로 변경 */
		io_u_set(td, io_u, IO_U_F_VER_IN_DEV);  /* 디바이스 측 검증 표시 */
	}

	/* NVMe uring 커맨드를 채워넣음 (opcode, LBA, 길이, 플래그 등) */
	return fio_nvme_uring_cmd_prep(cmd, io_u,
			o->nonvectored ? NULL : &ld->iovecs[io_u->index],
			dsm, read_opcode, ld->write_opcode,
			ld->cdw12_flags[io_u->ddir]);
}

/*
 * [함수] fio_ioring_validate_md
 * [역할] 읽기 완료 후 메타데이터(PI)의 무결성을 검증.
 *        pi_act=0(호스트 측 PI 처리)이고 PI 타입이 설정된 경우에만 동작.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 검증할 I/O 유닛
 * [반환값] 없음 (에러 시 io_u->error에 설정)
 */
static void fio_ioring_validate_md(struct thread_data *td, struct io_u *io_u)
{
	struct nvme_data *data;
	struct ioring_options *o = td->eo;
	int ret;

	data = FILE_ENG_DATA(io_u->file);  /* 파일의 NVMe 데이터 */
	/* PI 타입이 있고, 읽기 방향이고, pi_act=0(호스트 측 검증)인 경우 */
	if (data->pi_type && (io_u->ddir == DDIR_READ) && !o->pi_act) {
		ret = fio_nvme_pi_verify(data, io_u);  /* PI 무결성 검증 */
		if (ret)
			io_u->error = -ret;  /* 검증 실패 시 에러 설정 */
	}

	return;
}

/*
 * [함수] fio_ioring_event
 * [역할] CQ Ring에서 완료된 이벤트(CQE) 하나를 가져와서 해당하는 io_u를 반환.
 *        일반 io_uring 엔진(비 NVMe passthrough)용.
 *
 * [CQE 처리 흐름]
 * 1. event 번호와 cq_ring_off로 CQE 인덱스 계산
 * 2. CQE의 user_data에서 io_u 포인터 복원
 * 3. CQE의 res(결과)를 확인하여 io_u->error 설정
 * 4. 메타데이터 검증 수행 (필요 시)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - event: 이벤트 인덱스 (0부터 시작, getevents가 반환한 수 범위 내)
 * [반환값] 완료된 io_u 포인터
 */
static struct io_u *fio_ioring_event(struct thread_data *td, int event)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_uring_cqe *cqe;
	struct io_u *io_u;
	unsigned index;

	/* CQ Ring에서 해당 이벤트의 인덱스 계산 (링 마스크로 래핑) */
	index = (event + ld->cq_ring_off) & ld->cq_ring_mask;

	/* 해당 인덱스의 CQE를 가져옴 */
	cqe = &ld->cq_ring.cqes[index];
	/* CQE의 user_data에서 io_u 포인터 복원 (fio_ioring_prep에서 설정한 값) */
	io_u = (struct io_u *) (uintptr_t) cqe->user_data;

	/* trim returns 0 on success */
	/* 성공 확인: 전송된 바이트 == 요청한 바이트, 또는 TRIM 성공 (결과 0) */
	if (cqe->res == io_u->xfer_buflen ||
	    (io_u->ddir == DDIR_TRIM && !cqe->res)) {
		io_u->error = 0;
		/* 읽기 완료 시 메타데이터 검증 (pi_act=0인 경우) */
		if (io_u->ddir == DDIR_READ && o->md_per_io_size && !o->pi_act)
			fio_ioring_validate_md(td, io_u);
		return io_u;
	}

	/* TRIM 실패 시 비동기 TRIM 실패 플래그를 설정하여 이후 동기 모드로 전환 */
	if (io_u->ddir == DDIR_TRIM) {
		ld->async_trim_fail = 1;
		cqe->res = 0;
	}
	/* 에러 처리: res가 요청 크기보다 크면 음수 에러코드로 해석 */
	if (cqe->res > io_u->xfer_buflen)
		io_u->error = -cqe->res;
	else
		/* 부분 완료: 남은 바이트 수(잔여량) 계산 */
		io_u->resid = io_u->xfer_buflen - cqe->res;

	return io_u;
}

/*
 * [함수] fio_ioring_cmd_event
 * [역할] CQ Ring에서 완료된 이벤트(CQE)를 가져와 해당 io_u를 반환.
 *        io_uring_cmd 엔진(NVMe passthrough)용.
 *
 * 일반 io_uring 이벤트와의 차이:
 * - NVMe는 128바이트 CQE 사용 (IORING_SETUP_CQE32) → 인덱스 2배
 * - 에러 코드가 NVMe 상태 코드(SCT/SC) 형식
 * - IO_U_F_DEVICE_ERROR 플래그로 errno와 디바이스 에러를 구분
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - event: 이벤트 인덱스
 * [반환값] 완료된 io_u 포인터
 */
static struct io_u *fio_ioring_cmd_event(struct thread_data *td, int event)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_uring_cqe *cqe;
	struct io_u *io_u;
	struct nvme_data *data;
	unsigned index;
	int ret;

	/* CQ Ring에서 이벤트 인덱스 계산 */
	index = (event + ld->cq_ring_off) & ld->cq_ring_mask;
	/* NVMe는 CQE32 사용 → 인덱스를 2배로 */
	if (o->cmd_type == FIO_URING_CMD_NVME)
		index <<= 1;

	cqe = &ld->cq_ring.cqes[index];
	io_u = (struct io_u *) (uintptr_t) cqe->user_data;

	/* CQE의 res를 에러 코드로 설정 (NVMe: 0=성공, 양수=NVMe 에러) */
	io_u->error = cqe->res;
	if (io_u->error != 0)
		goto ret;

	/* 성공한 NVMe 읽기 시 PI 검증 수행 (pi_act=0인 경우) */
	if (o->cmd_type == FIO_URING_CMD_NVME) {
		data = FILE_ENG_DATA(io_u->file);
		if (data->pi_type && (io_u->ddir == DDIR_READ) && !o->pi_act) {
			ret = fio_nvme_pi_verify(data, io_u);
			if (ret)
				io_u->error = ret;
		}
	}

ret:
	/*
	 * If IO_U_F_DEVICE_ERROR is not set, io_u->error will be parsed as an
	 * errno, otherwise device-specific error value (status value in CQE).
	 */
	/*
	 * IO_U_F_DEVICE_ERROR가 설정되지 않으면 io_u->error는 errno로 해석되고,
	 * 설정되면 디바이스별 에러 값(CQE의 상태 값, NVMe SCT/SC)으로 해석됨.
	 */
	if ((int)io_u->error > 0)
		io_u_set(td, io_u, IO_U_F_DEVICE_ERROR);     /* 양수 에러 = 디바이스 에러 */
	else
		io_u_clear(td, io_u, IO_U_F_DEVICE_ERROR);   /* 0 또는 음수 = errno */
	io_u->error = abs((int)io_u->error);               /* 에러를 절대값으로 변환 */
	return io_u;
}

/*
 * [함수] fio_ioring_cmd_errdetails
 * [역할] io_uring_cmd 에러의 상세 정보를 문자열로 생성.
 *        NVMe의 경우 CQE 상태 코드(SCT, SC)를 포맷하여 반환.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 에러가 발생한 I/O 유닛
 * [반환값] 에러 상세 문자열 (NULL이면 상세 정보 없음)
 */
static char *fio_ioring_cmd_errdetails(struct thread_data *td,
				       struct io_u *io_u)
{
	struct ioring_options *o = td->eo;
	/* NVMe 상태 코드 파싱: SCT(Status Code Type)와 SC(Status Code) */
	unsigned int sct = (io_u->error >> 8) & 0x7;   /* 상태 코드 유형 (3비트) */
	unsigned int sc = io_u->error & 0xff;           /* 상태 코드 (8비트) */
#define MAXERRDETAIL 1024
#define MAXMSGCHUNK 128
	char *msg, msgchunk[MAXMSGCHUNK];

	/* 디바이스 에러가 아니면 상세 정보 불필요 */
	if (!(io_u->flags & IO_U_F_DEVICE_ERROR))
		return NULL;

	msg = calloc(1, MAXERRDETAIL);
	strcpy(msg, "io_uring_cmd: ");

	/* 파일명 추가 */
	snprintf(msgchunk, MAXMSGCHUNK, "%s: ", io_u->file->file_name);
	strlcat(msg, msgchunk, MAXERRDETAIL);

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		/* NVMe 에러: SCT와 SC를 16진수로 출력 */
		strlcat(msg, "cq entry status (", MAXERRDETAIL);

		snprintf(msgchunk, MAXMSGCHUNK, "sct=0x%02x; ", sct);
		strlcat(msg, msgchunk, MAXERRDETAIL);

		snprintf(msgchunk, MAXMSGCHUNK, "sc=0x%02x)", sc);
		strlcat(msg, msgchunk, MAXERRDETAIL);
	} else {
		/* Print status code in generic */
		/* 기타 디바이스: 상태 코드를 일반 형식으로 출력 */
		snprintf(msgchunk, MAXMSGCHUNK, "status=0x%x", io_u->error);
		strlcat(msg, msgchunk, MAXERRDETAIL);
	}

	return msg;
}

/*
 * [함수] fio_ioring_cqring_reap
 * [역할] CQ Ring에서 완료된 이벤트(CQE)를 수확(reap)하여 사용 가능한 수를 반환.
 *
 * [CQ Ring 수확 메커니즘]
 * 1. CQ Ring의 tail(커널이 업데이트)과 head(유저가 업데이트)의 차이로 사용 가능한 CQE 수 계산
 * 2. head를 전진시켜 커널에 소비 완료를 알림
 * 3. 메모리 순서 보장: tail은 acquire로 읽고, head는 relaxed로 쓰기
 *
 * libaio와의 차이: libaio의 io_getevents()는 커널→유저 복사가 필요하지만,
 * io_uring은 공유 메모리의 포인터만 업데이트하므로 제로 카피.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - max: 최대 수확할 CQE 수
 * [반환값] 실제로 수확한 CQE 수
 */
static unsigned fio_ioring_cqring_reap(struct thread_data *td, unsigned int max)
{
	struct ioring_data *ld = td->io_ops_data;
	struct io_cq_ring *ring = &ld->cq_ring;
	/* CQ Ring의 현재 head 위치 */
	unsigned head = *ring->head;
	/* tail을 acquire 의미론으로 읽어 커널이 추가한 CQE가 보이도록 보장 */
	unsigned available = atomic_load_acquire(ring->tail) - head;

	/* 사용 가능한 CQE가 없으면 0 반환 */
	if (!available)
		return 0;

	/* 최대 max개까지만 수확 */
	available = min(available, max);
	/*
	 * The CQ consumer index is advanced before the CQEs are actually read.
	 * This is generally unsafe, as it lets the kernel reuse the CQE slots.
	 * However, the CQ is sized large enough for the maximum iodepth and a
	 * new SQE won't be submitted until the CQE is processed, so the CQE
	 * slot won't actually be reused until it has been processed.
	 */
	/*
	 * CQ 소비자 인덱스(head)를 CQE를 실제로 읽기 전에 전진시킴.
	 * 일반적으로는 안전하지 않지만 (커널이 CQE 슬롯을 재사용할 수 있으므로),
	 * CQ 크기가 최대 iodepth만큼 크고, CQE가 처리되기 전에는
	 * 새 SQE가 제출되지 않으므로 실제로 슬롯이 재사용되지 않음.
	 */
	atomic_store_relaxed(ring->head, head + available);
	return available;
}

/*
 * [함수] fio_ioring_getevents
 * [역할] 완료된 I/O 이벤트를 수집하는 함수.
 *        CQ Ring에서 CQE를 수확하고, 필요 시 io_uring_enter()로 완료를 기다림.
 *
 * [I/O 흐름에서의 위치]
 * fio_ioring_queue() → fio_ioring_commit() → [커널 I/O 처리]
 * → fio_ioring_getevents() (여기) → fio_ioring_event()로 개별 CQE 처리
 *
 * [동작 방식]
 * 1. fio_ioring_cqring_reap()으로 이미 완료된 CQE를 수확 시도
 * 2. min 개수만큼 모이면 즉시 반환
 * 3. 부족하면 io_uring_enter()로 커널에 완료를 기다리도록 요청
 *    (SQPOLL 모드에서는 io_uring_enter()를 호출하지 않고 계속 폴링)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - min: 최소 수집해야 할 이벤트 수
 *   - max: 최대 수집 가능한 이벤트 수
 *   - t: 타임아웃 (현재 미사용)
 * [반환값] 수집된 이벤트 수, 실패 시 음수 에러코드
 */
static int fio_ioring_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct ioring_data *ld = td->io_ops_data;
	/* 최소 완료 수: iodepth_batch_complete_min=0이면 non-blocking (0개도 OK) */
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct ioring_options *o = td->eo;
	struct io_cq_ring *ring = &ld->cq_ring;
	unsigned events = 0;
	int r;

	/* CQ Ring의 현재 head를 cq_ring_off에 저장 (이벤트 처리 시작점) */
	ld->cq_ring_off = *ring->head;
	/* 완료 이벤트 수집 루프 */
	for (;;) {
		/* CQ Ring에서 완료된 CQE 수확 시도 */
		r = fio_ioring_cqring_reap(td, max - events);
		if (r) {
			events += r;
			/* min 개수 이상 수집했으면 반환 */
			if (events >= min)
				return events;

			/* 아직 부족하면 남은 최소 필요 수 갱신 */
			if (actual_min != 0)
				actual_min -= r;
		}

		/* SQPOLL 모드가 아닌 경우에만 io_uring_enter()로 커널에 완료 대기 요청 */
		if (!o->sqpoll_thread) {
			r = io_uring_enter(ld, 0, actual_min, enter_flags);
			if (r < 0) {
				/* EAGAIN/EINTR은 재시도 */
				if (errno == EAGAIN || errno == EINTR)
					continue;
				r = -errno;
				td_verror(td, errno, "io_uring_enter");
				return r;
			}
		}
	}
}

/*
 * [함수] fio_ioring_cmd_nvme_pi
 * [역할] NVMe uring_cmd에서 PI(Protection Information) 데이터를 SQE에 채움.
 *        NVMe passthrough 경로에서 PI 관련 필드를 설정.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛
 * [반환값] 없음
 */
static inline void fio_ioring_cmd_nvme_pi(struct thread_data *td,
					  struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct nvme_uring_cmd *cmd;
	struct io_uring_sqe *sqe;

	/* TRIM은 PI와 무관하므로 건너뜀 */
	if (io_u->ddir == DDIR_TRIM)
		return;

	/* NVMe는 128바이트 SQE → 인덱스 2배 */
	sqe = &ld->sqes[(io_u->index) << 1];
	cmd = (struct nvme_uring_cmd *)sqe->cmd;

	/* NVMe 커맨드에 PI 필드 채움 (가드 태그, 참조 태그, 앱 태그) */
	fio_nvme_pi_fill(cmd, io_u, &ld->ext_opts);
}

/*
 * [함수] fio_ioring_setup_pi
 * [역할] 일반 io_uring 경로에서 PI 가드 태그(CRC)를 생성.
 *        비 NVMe passthrough에서 메타데이터가 있는 I/O의 가드 태그 계산.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛
 * [반환값] 없음
 */
static inline void fio_ioring_setup_pi(struct thread_data *td,
				      struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;

	/* TRIM은 PI와 무관 */
	if (io_u->ddir == DDIR_TRIM)
		return;

	/* 가드 태그(CRC16 또는 CRC64) 생성 */
	fio_nvme_generate_guard(io_u, &ld->ext_opts);
}

/*
 * [함수] fio_ioring_cmdprio_prep
 * [역할] 커맨드 우선순위(cmdprio) 옵션이 활성화된 경우,
 *        I/O의 우선순위를 SQE에 설정.
 *        cmdprio_percentage/cmdprio_bssplit에 따라 일부 I/O의 우선순위를 변경.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛
 * [반환값] 없음
 */
static inline void fio_ioring_cmdprio_prep(struct thread_data *td,
					   struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct cmdprio *cmdprio = &ld->cmdprio;

	/* 우선순위가 변경되었으면 SQE의 ioprio 필드를 업데이트 */
	if (fio_cmdprio_set_ioprio(td, cmdprio, io_u))
		ld->sqes[io_u->index].ioprio = io_u->ioprio;
}

/*
 * [함수] fio_ioring_queue
 * [역할] I/O 요청을 SQ Ring에 추가하는 함수. fio의 queue 콜백.
 *
 * === 핵심 I/O 제출 흐름 (1단계) ===
 *
 * [흐름 개요]
 * fio_ioring_queue() (여기)
 *   → SQ Ring의 array[tail]에 SQE 인덱스를 기록하고 tail을 증가
 *   → queued 카운터 증가
 *   → FIO_Q_QUEUED 반환 (나중에 commit에서 일괄 제출됨)
 *
 * fio_ioring_commit()
 *   → io_uring_enter() 시스콜로 SQ에 쌓인 SQE들을 커널에 제출
 *   → (SQPOLL: 시스콜 없이 커널 스레드가 자동 처리)
 *
 * fio_ioring_getevents()
 *   → CQ Ring에서 완료된 CQE를 수확
 *
 * [SQ Ring 동작 상세]
 * 1. tail = *ring->tail (현재 tail 위치 읽기)
 * 2. ring->array[tail & mask] = io_u->index (SQE 인덱스 기록)
 * 3. atomic_store_release(ring->tail, tail + 1) (tail 전진, 메모리 배리어)
 * 4. 커널(또는 SQPOLL 스레드)은 head와 tail의 차이를 감지하여 새 SQE 처리
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 큐에 추가할 I/O 유닛
 * [반환값]
 *   - FIO_Q_QUEUED: 성공적으로 큐에 추가됨 (나중에 commit으로 제출)
 *   - FIO_Q_BUSY: 큐가 가득 참 (iodepth에 도달)
 *   - FIO_Q_COMPLETED: 동기적으로 즉시 완료됨 (syncfs, 동기 trim)
 */
static enum fio_q_status fio_ioring_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_sq_ring *ring = &ld->sq_ring;
	unsigned tail;

	/* 읽기 전용 모드 체크 */
	fio_ro_check(td, io_u);

	/* should not hit... */
	/* 큐 깊이가 최대에 도달하면 BUSY 반환 - 이 상황은 발생하지 않아야 함 */
	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/*
	 * If this is a syncfs request, or if async trim has been tried and
	 * failed, punt to sync.
	 * */
	/*
	 * syncfs 요청이거나, 비동기 TRIM이 이전에 실패한 경우
	 * 동기 처리로 전환 (io_uring을 우회)
	 */
	if (io_u->ddir == DDIR_SYNCFS ||
	    (io_u->ddir == DDIR_TRIM && ld->async_trim_fail)) {
		/* 큐에 대기 중인 요청이 있으면 BUSY 반환 (먼저 처리해야 함) */
		if (ld->queued)
			return FIO_Q_BUSY;

		/* 동기적으로 즉시 처리 */
		if (io_u->ddir == DDIR_TRIM)
			do_io_u_trim(td, io_u);    /* 동기 TRIM 실행 */
		else
			do_io_u_sync(td, io_u);    /* 동기 sync 실행 */

		io_u_mark_submit(td, 1);    /* 제출 통계 업데이트 */
		io_u_mark_complete(td, 1);  /* 완료 통계 업데이트 */
		return FIO_Q_COMPLETED;     /* 즉시 완료로 반환 */
	}

	/* 커맨드 우선순위가 설정된 경우 SQE의 ioprio를 조정 */
	if (ld->cmdprio.mode != CMDPRIO_MODE_NONE)
		fio_ioring_cmdprio_prep(td, io_u);

	/* PI(Protection Information) 설정
	 * NVMe passthrough → NVMe PI 채우기
	 * 일반 io_uring + 메타데이터 → 가드 태그 생성 */
	if (o->cmd_type == FIO_URING_CMD_NVME && ld->is_uring_cmd_eng)
		fio_ioring_cmd_nvme_pi(td, io_u);
	else if (o->md_per_io_size)
		fio_ioring_setup_pi(td, io_u);

	/* === SQ Ring에 SQE 인덱스 추가 === */
	tail = *ring->tail;                                    /* 현재 tail 위치 읽기 */
	ring->array[tail & ld->sq_ring_mask] = io_u->index;   /* SQE 인덱스를 array에 기록 */
	atomic_store_release(ring->tail, tail + 1);            /* tail 전진 (release 배리어: 데이터가 먼저 쓰여짐을 보장) */

	ld->queued++;           /* 대기 중인 I/O 수 증가 */
	return FIO_Q_QUEUED;    /* 큐에 추가됨 - commit에서 일괄 제출될 예정 */
}

/*
 * [함수] fio_ioring_queued
 * [역할] 커널에 제출된 I/O들의 발행 시간(issue_time)을 기록.
 *        제출 지연시간(slat) 측정과 iolog에 사용됨.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - start: SQ Ring에서 시작 인덱스
 *   - nr: 처리할 I/O 수
 * [반환값] 없음
 */
static void fio_ioring_queued(struct thread_data *td, int start, int nr)
{
	struct ioring_data *ld = td->io_ops_data;
	struct timespec now;

	/* 발행 시간 기록이 필요 없으면 건너뜀 */
	if (!fio_fill_issue_time(td))
		return;

	/* 현재 시간 취득 */
	fio_gettime(&now, NULL);

	/* 각 제출된 I/O에 대해 발행 시간 기록 */
	while (nr--) {
		struct io_sq_ring *ring = &ld->sq_ring;
		/* SQ Ring의 array에서 SQE 인덱스를 가져옴 */
		int index = ring->array[start & ld->sq_ring_mask];
		/* 인덱스로 io_u를 찾음 */
		struct io_u *io_u = ld->io_u_index[index];

		/* 발행 시간 기록 */
		memcpy(&io_u->issue_time, &now, sizeof(now));
		io_u_queued(td, io_u);  /* fio에 큐잉 완료 알림 */

		start++;
	}

	/*
	 * only used for iolog
	 */
	/* iolog 파일 사용 시 마지막 발행 시간 기록 */
	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &now, sizeof(now));
}

/*
 * [함수] fio_ioring_commit
 * [역할] SQ Ring에 쌓인 I/O 요청들을 커널에 실제로 제출하는 함수.
 *        fio의 commit 콜백.
 *
 * === 핵심 I/O 제출 흐름 (2단계) ===
 *
 * fio_ioring_queue()에서 SQ Ring에 SQE를 추가한 후,
 * fio_ioring_commit()이 호출되어 io_uring_enter() 시스콜로 커널에 알림.
 *
 * [SQPOLL 모드 vs 일반 모드]
 *
 * SQPOLL 모드 (sqpoll_thread=1):
 * - 커널 스레드가 SQ를 지속 감시하므로 io_uring_enter() 불필요
 * - 단, 커널 스레드가 sleep한 경우 (IORING_SQ_NEED_WAKEUP 플래그)
 *   io_uring_enter(IORING_ENTER_SQ_WAKEUP)으로 깨워야 함
 * - libaio 대비 장점: 시스콜 오버헤드 완전 제거 가능
 *
 * 일반 모드:
 * - io_uring_enter()로 커널에 제출할 SQE 수를 알림
 * - libaio의 io_submit()에 해당하지만, SQE가 이미 공유 메모리에 있으므로
 *   데이터 복사 없이 포인터만 전달
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 에러코드 (실패)
 */
static int fio_ioring_commit(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	int ret;

	/* 대기 중인 I/O가 없으면 할 일 없음 */
	if (!ld->queued)
		return 0;

	/*
	 * Kernel side does submission. just need to check if the ring is
	 * flagged as needing a kick, if so, call io_uring_enter(). This
	 * only happens if we've been idle too long.
	 */
	/*
	 * SQPOLL 모드: 커널 측에서 제출을 처리함.
	 * 링에 NEED_WAKEUP 플래그가 설정되었는지만 확인하고,
	 * 설정되어 있으면 io_uring_enter()로 커널 스레드를 깨움.
	 * (유휴 시간이 너무 길면 커널 스레드가 sleep하므로)
	 */
	if (o->sqpoll_thread) {
		struct io_sq_ring *ring = &ld->sq_ring;
		unsigned start = *ld->sq_ring.tail - ld->queued;  /* 이번에 추가된 SQE의 시작 위치 */
		unsigned flags;

		/* SQ Ring의 flags를 읽어 NEED_WAKEUP 확인 */
		flags = atomic_load_relaxed(ring->flags);
		if (flags & IORING_SQ_NEED_WAKEUP)
			/* 커널 SQPOLL 스레드를 깨움 */
			io_uring_enter(ld, ld->queued, 0,
					IORING_ENTER_SQ_WAKEUP);
		/* 발행 시간 기록 */
		fio_ioring_queued(td, start, ld->queued);
		/* 제출 통계 업데이트 */
		io_u_mark_submit(td, ld->queued);

		ld->queued = 0;  /* 대기 큐 비움 */
		return 0;
	}

	/* 일반 모드: io_uring_enter()로 커널에 SQE 제출 */
	do {
		unsigned start = *ld->sq_ring.head;  /* SQ Ring의 현재 head (커널의 소비 위치) */
		long nr = ld->queued;                /* 제출할 수 */

		/* io_uring_enter() 호출: nr개의 SQE 제출 */
		ret = io_uring_enter(ld, nr, 0, enter_flags);
		if (ret > 0) {
			/* 성공: ret개의 SQE가 제출됨 */
			fio_ioring_queued(td, start, ret);   /* 발행 시간 기록 */
			io_u_mark_submit(td, ret);           /* 제출 통계 */

			ld->queued -= ret;   /* 대기 수 감소 */
			ret = 0;
		} else if (!ret) {
			/* 0개 제출됨 - 재시도 */
			io_u_mark_submit(td, ret);
			continue;
		} else {
			/* 에러 처리 */
			if (errno == EAGAIN || errno == EINTR) {
				/* 리소스 부족 또는 인터럽트 - CQ에서 완료를 수확하여 공간 확보 시도 */
				ret = fio_ioring_cqring_reap(td, ld->queued);
				if (ret)
					continue;
				/* Shouldn't happen */
				/* 발생하지 않아야 하는 상황 - 짧은 대기 후 재시도 */
				usleep(1);
				continue;
			}
			ret = -errno;
			td_verror(td, errno, "io_uring_enter submit");
			break;
		}
	} while (ld->queued);  /* 모든 대기 I/O가 제출될 때까지 반복 */

	return ret;
}

/*
 * [함수] fio_ioring_unmap
 * [역할] io_uring 인스턴스의 mmap 매핑을 해제하고 ring fd를 닫음.
 *        cleanup 시 호출됨.
 *
 * [파라미터]
 *   - ld: io_uring 데이터
 * [반환값] 없음
 */
static void fio_ioring_unmap(struct ioring_data *ld)
{
	int i;

	/* 3개의 mmap 영역 해제: SQ Ring, SQEs, CQ Ring */
	for (i = 0; i < FIO_ARRAY_SIZE(ld->mmap); i++)
		munmap(ld->mmap[i].ptr, ld->mmap[i].len);
	/* io_uring 파일 디스크립터 닫기 */
	close(ld->ring_fd);
}

/*
 * [함수] fio_ioring_cleanup
 * [역할] io_uring 엔진의 모든 리소스를 정리하는 함수. fio의 cleanup 콜백.
 *        mmap 해제, 동적 할당 메모리 해제 등을 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 없음
 */
static void fio_ioring_cleanup(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;

	if (ld) {
		/* 자식 스레드가 아닌 경우에만 mmap 해제 (자식은 부모의 매핑을 공유) */
		if (!(td->flags & TD_F_CHILD))
			fio_ioring_unmap(ld);

		fio_cmdprio_cleanup(&ld->cmdprio);  /* 커맨드 우선순위 리소스 해제 */
		free(ld->io_u_index);   /* io_u 인덱스 배열 해제 */
		free(ld->md_buf);       /* 메타데이터 버퍼 해제 */
		free(ld->pi_attr);      /* PI 속성 버퍼 해제 */
		free(ld->iovecs);       /* iovec 배열 해제 */
		free(ld->fds);          /* 등록된 fd 배열 해제 */
		free(ld->dsm);          /* NVMe DSM 구조체 해제 */
		free(ld);               /* 엔진 데이터 구조체 자체 해제 */
	}
}

/*
 * [함수] fio_ioring_mmap
 * [역할] io_uring 인스턴스의 SQ Ring, SQE 배열, CQ Ring을 mmap()으로
 *        유저스페이스에 매핑하는 함수.
 *
 * io_uring의 핵심 특징 중 하나: 커널과 유저가 메모리를 공유하여
 * 시스템 콜 없이 데이터를 주고받을 수 있음.
 *
 * [매핑 영역]
 * mmap[0]: SQ Ring (head, tail, flags, array 등)
 *          - IORING_OFF_SQ_RING 오프셋으로 매핑
 * mmap[1]: SQE 배열 (실제 I/O 요청 기술자들)
 *          - IORING_OFF_SQES 오프셋으로 매핑
 *          - SQE128 모드에서는 2배 크기
 * mmap[2]: CQ Ring (head, tail, CQE 배열 등)
 *          - IORING_OFF_CQ_RING 오프셋으로 매핑
 *          - CQE32 모드에서는 CQE 2배 크기
 *
 * [파라미터]
 *   - ld: io_uring 데이터
 *   - p: io_uring_setup()에서 반환된 파라미터 (각 구성요소의 오프셋 정보 포함)
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_mmap(struct ioring_data *ld, struct io_uring_params *p)
{
	struct io_sq_ring *sring = &ld->sq_ring;
	struct io_cq_ring *cring = &ld->cq_ring;
	void *ptr;

	/* === SQ Ring mmap (mmap[0]) === */
	/* 매핑 크기: array의 오프셋 + entries * sizeof(__u32) */
	ld->mmap[0].len = p->sq_off.array + p->sq_entries * sizeof(__u32);
	ptr = mmap(0, ld->mmap[0].len, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ld->ring_fd,
			IORING_OFF_SQ_RING);
	ld->mmap[0].ptr = ptr;
	/* 커널이 알려준 오프셋을 사용하여 각 필드의 포인터를 설정 */
	sring->head = ptr + p->sq_off.head;              /* SQ head 포인터 */
	sring->tail = ptr + p->sq_off.tail;              /* SQ tail 포인터 */
	sring->ring_mask = ptr + p->sq_off.ring_mask;    /* 링 마스크 */
	sring->ring_entries = ptr + p->sq_off.ring_entries; /* 엔트리 수 */
	sring->flags = ptr + p->sq_off.flags;            /* 플래그 (NEED_WAKEUP 등) */
	sring->array = ptr + p->sq_off.array;            /* SQE 인덱스 배열 */
	ld->sq_ring_mask = *sring->ring_mask;            /* 마스크를 로컬에 캐시 */

	/* === SQE 배열 mmap (mmap[1]) === */
	/* SQE128 모드(NVMe)에서는 SQE 크기가 2배 */
	if (p->flags & IORING_SETUP_SQE128)
		ld->mmap[1].len = 2 * p->sq_entries * sizeof(struct io_uring_sqe);
	else
		ld->mmap[1].len = p->sq_entries * sizeof(struct io_uring_sqe);
	ld->sqes = mmap(0, ld->mmap[1].len, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE, ld->ring_fd,
				IORING_OFF_SQES);
	ld->mmap[1].ptr = ld->sqes;

	/* === CQ Ring mmap (mmap[2]) === */
	/* CQE32 모드(NVMe)에서는 CQE 크기가 2배 */
	if (p->flags & IORING_SETUP_CQE32) {
		ld->mmap[2].len = p->cq_off.cqes +
					2 * p->cq_entries * sizeof(struct io_uring_cqe);
	} else {
		ld->mmap[2].len = p->cq_off.cqes +
					p->cq_entries * sizeof(struct io_uring_cqe);
	}
	ptr = mmap(0, ld->mmap[2].len, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ld->ring_fd,
			IORING_OFF_CQ_RING);
	ld->mmap[2].ptr = ptr;
	/* CQ Ring의 각 필드 포인터 설정 */
	cring->head = ptr + p->cq_off.head;              /* CQ head 포인터 */
	cring->tail = ptr + p->cq_off.tail;              /* CQ tail 포인터 */
	cring->ring_mask = ptr + p->cq_off.ring_mask;    /* 링 마스크 */
	cring->ring_entries = ptr + p->cq_off.ring_entries; /* 엔트리 수 */
	cring->cqes = ptr + p->cq_off.cqes;              /* CQE 배열 포인터 */
	ld->cq_ring_mask = *cring->ring_mask;             /* 마스크를 로컬에 캐시 */
	return 0;
}

/*
 * [함수] fio_ioring_probe
 * [역할] io_uring 인스턴스가 비벡터 I/O(IORING_OP_READ/WRITE)를 지원하는지 탐지.
 *        nonvectored=-1(자동) 모드에서 호출됨.
 *
 * IORING_REGISTER_PROBE를 사용하여 커널이 지원하는 opcode 목록을 조회하고,
 * READ/WRITE가 지원되면 nonvectored=1로 설정.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 없음
 */
static void fio_ioring_probe(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_uring_probe *p;
	int ret;

	/* already set by user, don't touch */
	/* 사용자가 명시적으로 설정한 경우 건드리지 않음 */
	if (o->nonvectored != -1)
		return;

	/* default to off, as that's always safe */
	/* 기본값: 벡터 I/O 사용 (항상 안전) */
	o->nonvectored = 0;

	/* probe 구조체 할당 (최대 256개 opcode 조회) */
	p = calloc(1, sizeof(*p) + 256 * sizeof(struct io_uring_probe_op));
	if (!p)
		return;

	/* io_uring_register PROBE 호출 - 커널이 지원하는 opcode 목록 조회 */
	ret = syscall(__NR_io_uring_register, ld->ring_fd,
			IORING_REGISTER_PROBE, p, 256);
	if (ret < 0)
		goto out;

	/* IORING_OP_WRITE가 조회 범위 밖이면 지원 안 함 */
	if (IORING_OP_WRITE > p->ops_len)
		goto out;

	/* READ와 WRITE 모두 지원되면 비벡터 모드 활성화 */
	if ((p->ops[IORING_OP_READ].flags & IO_URING_OP_SUPPORTED) &&
	    (p->ops[IORING_OP_WRITE].flags & IO_URING_OP_SUPPORTED))
		o->nonvectored = 1;
out:
	free(p);
}

/*
 * [함수] fio_ioring_queue_init
 * [역할] 일반 io_uring 인스턴스를 생성하고 초기화하는 함수.
 *        io_uring_setup() 시스콜을 호출하여 커널에 io_uring 인스턴스를 생성.
 *
 * [초기화 과정]
 * 1. io_uring_params 설정 (IOPOLL, SQPOLL, CQSIZE 등 플래그)
 * 2. io_uring_setup() 시스콜로 인스턴스 생성
 * 3. 실패 시 최신 기능부터 제거하며 재시도 (하위 호환성)
 * 4. probe로 비벡터 I/O 지원 여부 확인
 * 5. fixedbufs 사용 시 버퍼 등록
 * 6. mmap()으로 SQ/CQ Ring 매핑
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_queue_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	int depth = ld->iodepth;         /* 큐 깊이 (2의 거듭제곱) */
	struct io_uring_params p;
	int ret;

	memset(&p, 0, sizeof(p));

	/* IOPOLL: 폴링 기반 완료 확인 (인터럽트 없이) */
	if (o->hipri)
		p.flags |= IORING_SETUP_IOPOLL;
	/* SQPOLL: 커널 스레드가 SQ를 지속 감시 */
	if (o->sqpoll_thread) {
		p.flags |= IORING_SETUP_SQPOLL;
		/* SQPOLL 스레드를 특정 CPU에 고정 */
		if (o->sqpoll_set) {
			p.flags |= IORING_SETUP_SQ_AFF;
			p.sq_thread_cpu = o->sqpoll_cpu;
		}

		/*
		 * Submission latency for sqpoll_thread is just the time it
		 * takes to fill in the SQ ring entries, and any syscall if
		 * IORING_SQ_NEED_WAKEUP is set, we don't need to log that time
		 * separately.
		 */
		/*
		 * SQPOLL 모드에서 제출 지연시간(slat)은 SQ Ring 엔트리를 채우는 시간 +
		 * NEED_WAKEUP 시 시스콜 시간뿐이므로, 별도로 기록할 필요 없음.
		 */
		td->o.disable_slat = 1;
	}

	/*
	 * Clamp CQ ring size at our SQ ring size, we don't need more entries
	 * than that.
	 */
	/* CQ Ring 크기를 SQ Ring과 동일하게 설정 - 그 이상은 불필요 */
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = depth;

	/*
	 * Setup COOP_TASKRUN as we don't need to get IPI interrupted for
	 * completing IO operations.
	 */
	/* COOP_TASKRUN: I/O 완료 시 IPI(프로세서간 인터럽트) 대신
	 * 협력적 태스크 실행 - 불필요한 인터럽트 감소 */
	p.flags |= IORING_SETUP_COOP_TASKRUN;

	/*
	 * io_uring is always a single issuer, and we can defer task_work
	 * runs until we reap events.
	 */
	/* SINGLE_ISSUER: 단일 스레드만 제출함을 보장하여 최적화
	 * DEFER_TASKRUN: 이벤트 수확 시점까지 태스크 작업을 지연 */
	p.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

retry:
	/* io_uring_setup 시스콜: 커널에 io_uring 인스턴스 생성 요청 */
	ret = syscall(__NR_io_uring_setup, depth, &p);
	if (ret < 0) {
		/* 최신 기능이 지원되지 않으면 하나씩 제거하며 재시도 (graceful degradation) */
		if (errno == EINVAL && p.flags & IORING_SETUP_DEFER_TASKRUN) {
			p.flags &= ~IORING_SETUP_DEFER_TASKRUN;
			p.flags &= ~IORING_SETUP_SINGLE_ISSUER;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_COOP_TASKRUN) {
			p.flags &= ~IORING_SETUP_COOP_TASKRUN;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_CQSIZE) {
			p.flags &= ~IORING_SETUP_CQSIZE;
			goto retry;
		}
		return ret;
	}

	/* NO_IOWAIT 기능 지원 시 enter_flags에 추가 (불필요한 I/O 대기 방지) */
	if (p.features & IORING_FEAT_NO_IOWAIT)
		enter_flags |= IORING_ENTER_NO_IOWAIT;
	ld->ring_fd = ret;   /* io_uring 파일 디스크립터 저장 */

	/* 비벡터 I/O 지원 여부 탐지 */
	fio_ioring_probe(td);

	/* fixedbufs: 버퍼를 커널에 사전 등록 */
	if (o->fixedbufs) {
		ret = syscall(__NR_io_uring_register, ld->ring_fd,
				IORING_REGISTER_BUFFERS, ld->iovecs, depth);
		if (ret < 0)
			return ret;
	}

	/* SQ Ring, SQEs, CQ Ring을 mmap()으로 매핑 */
	return fio_ioring_mmap(ld, &p);
}

/*
 * [함수] fio_ioring_cmd_queue_init
 * [역할] io_uring_cmd 엔진(NVMe passthrough)용 io_uring 인스턴스를 생성.
 *        일반 io_uring과 유사하지만 SQE128/CQE32 모드를 추가로 설정.
 *
 * NVMe passthrough에서는:
 * - IORING_SETUP_SQE128: SQE가 128바이트로 확장 (NVMe 커맨드 64바이트 포함)
 * - IORING_SETUP_CQE32: CQE가 32바이트로 확장 (NVMe 완료 데이터 포함)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_queue_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	int depth = ld->iodepth;
	struct io_uring_params p;
	int ret;

	memset(&p, 0, sizeof(p));

	/* IOPOLL 설정 */
	if (o->hipri)
		p.flags |= IORING_SETUP_IOPOLL;
	/* SQPOLL 설정 */
	if (o->sqpoll_thread) {
		p.flags |= IORING_SETUP_SQPOLL;
		if (o->sqpoll_set) {
			p.flags |= IORING_SETUP_SQ_AFF;
			p.sq_thread_cpu = o->sqpoll_cpu;
		}

		/*
		 * Submission latency for sqpoll_thread is just the time it
		 * takes to fill in the SQ ring entries, and any syscall if
		 * IORING_SQ_NEED_WAKEUP is set, we don't need to log that time
		 * separately.
		 */
		/* SQPOLL 모드에서는 제출 지연시간 측정 비활성화 */
		td->o.disable_slat = 1;
	}
	/* NVMe는 확장 SQE(128바이트)와 확장 CQE(32바이트) 필요 */
	if (o->cmd_type == FIO_URING_CMD_NVME) {
		p.flags |= IORING_SETUP_SQE128;  /* 128바이트 SQE (기본 64 + NVMe cmd 64) */
		p.flags |= IORING_SETUP_CQE32;   /* 32바이트 CQE (기본 16 + NVMe 결과 16) */
	}

	/*
	 * Clamp CQ ring size at our SQ ring size, we don't need more entries
	 * than that.
	 */
	/* CQ Ring 크기 = SQ Ring 크기 */
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = depth;

	/*
	 * Setup COOP_TASKRUN as we don't need to get IPI interrupted for
	 * completing IO operations.
	 */
	/* 협력적 태스크 실행 설정 */
	p.flags |= IORING_SETUP_COOP_TASKRUN;

	/*
	 * io_uring is always a single issuer, and we can defer task_work
	 * runs until we reap events.
	 */
	/* 단일 발행자 + 태스크 작업 지연 */
	p.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

retry:
	/* io_uring 인스턴스 생성 */
	ret = syscall(__NR_io_uring_setup, depth, &p);
	if (ret < 0) {
		/* 하위 호환성을 위한 graceful degradation */
		if (errno == EINVAL && p.flags & IORING_SETUP_DEFER_TASKRUN) {
			p.flags &= ~IORING_SETUP_DEFER_TASKRUN;
			p.flags &= ~IORING_SETUP_SINGLE_ISSUER;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_COOP_TASKRUN) {
			p.flags &= ~IORING_SETUP_COOP_TASKRUN;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_CQSIZE) {
			p.flags &= ~IORING_SETUP_CQSIZE;
			goto retry;
		}
		return ret;
	}

	ld->ring_fd = ret;  /* io_uring 파일 디스크립터 저장 */

	/* 비벡터 I/O 지원 탐지 */
	fio_ioring_probe(td);

	/* 고정 버퍼 등록 */
	if (o->fixedbufs) {
		ret = syscall(__NR_io_uring_register, ld->ring_fd,
				IORING_REGISTER_BUFFERS, ld->iovecs, depth);
		if (ret < 0)
			return ret;
	}

	/* SQ/CQ Ring mmap 매핑 */
	return fio_ioring_mmap(ld, &p);
}

/*
 * [함수] fio_ioring_register_files
 * [역할] 파일 디스크립터를 io_uring에 사전 등록하는 함수.
 *
 * 등록된 파일의 장점:
 * - 매 I/O마다 fd → struct file lookup 비용 제거
 * - atomic refcount 증감 비용 제거
 * - SQPOLL 모드에서 필수 (커널 스레드가 유저 fd 테이블에 접근 불가)
 *
 * [동작 과정]
 * 1. 모든 파일을 generic_open_file()로 열기
 * 2. fd 배열을 io_uring_register(IORING_REGISTER_FILES)로 커널에 등록
 * 3. 등록 후 f->fd를 -1로 설정 (파일이 닫힌 것처럼 보이지만 커널 내부에서 참조 유지)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_register_files(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct fio_file *f;
	unsigned int i;
	int ret;

	/* fd 배열 할당 */
	ld->fds = calloc(td->o.nr_files, sizeof(int));

	/* 모든 파일을 열고 fd를 배열에 저장 */
	for_each_file(td, f, i) {
		ret = generic_open_file(td, f);
		if (ret)
			goto err;
		ld->fds[i] = f->fd;        /* fd 저장 */
		f->engine_pos = i;          /* 등록 인덱스 저장 (SQE에서 사용) */
	}

	/* 커널에 fd 배열 등록 */
	ret = syscall(__NR_io_uring_register, ld->ring_fd,
			IORING_REGISTER_FILES, ld->fds, td->o.nr_files);
	if (ret) {
err:
		free(ld->fds);
		ld->fds = NULL;
	}

	/*
	 * Pretend the file is closed again, and really close it if we hit
	 * an error.
	 */
	/*
	 * 파일이 닫힌 것처럼 처리 (fd=-1).
	 * 에러 발생 시에는 실제로 파일을 닫음.
	 * (커널 내부에서는 등록된 참조가 유지됨)
	 */
	for_each_file(td, f, i) {
		if (ret) {
			int fio_unused ret2;
			ret2 = generic_close_file(td, f);   /* 에러 시 실제로 닫기 */
		} else
			f->fd = -1;   /* 성공 시 fd를 -1로 (커널 내부에서 참조 유지) */
	}

	return ret;
}

/*
 * [함수] fio_ioring_post_init
 * [역할] io_uring 엔진의 후기 초기화 함수.
 *        io_u가 할당된 후 호출되어 iovec 설정, 큐 초기화, 파일 등록 등을 수행.
 *
 * fio의 초기화 순서: init → io_u 할당 → post_init
 * post_init에서는 io_u가 이미 할당되어 있으므로 버퍼 주소를 iovec에 설정 가능.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 1 (실패)
 */
static int fio_ioring_post_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_u *io_u;
	int err, i;

	/* 각 io_u의 버퍼 정보를 iovec에 설정 */
	for (i = 0; i < td->o.iodepth; i++) {
		struct iovec *iov = &ld->iovecs[i];

		io_u = ld->io_u_index[i];
		iov->iov_base = io_u->buf;             /* 버퍼 시작 주소 */
		iov->iov_len = td_max_bs(td);           /* 최대 블록 크기 */
	}

	/* io_uring 인스턴스 생성 및 초기화 */
	err = fio_ioring_queue_init(td);
	if (err) {
		int init_err = errno;

		if (init_err == ENOSYS)
			log_err("fio: your kernel doesn't support io_uring\n");
		td_verror(td, init_err, "io_queue_init");
		return 1;
	}

	/* 모든 SQE를 0으로 초기화 */
	for (i = 0; i < ld->iodepth; i++) {
		struct io_uring_sqe *sqe;

		sqe = &ld->sqes[i];
		memset(sqe, 0, sizeof(*sqe));
	}

	/* 파일 등록 (registerfiles 옵션 사용 시) */
	if (o->registerfiles) {
		err = fio_ioring_register_files(td);
		if (err) {
			td_verror(td, errno, "ioring_register_files");
			return 1;
		}
	}

	return 0;
}

/*
 * [함수] fio_ioring_cmd_post_init
 * [역할] io_uring_cmd 엔진(NVMe passthrough)의 후기 초기화 함수.
 *        fio_ioring_post_init과 유사하지만 NVMe용 확장 SQE 초기화.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 1 (실패)
 */
static int fio_ioring_cmd_post_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_u *io_u;
	int err, i;

	/* iovec 초기화 (각 io_u의 버퍼 정보 설정) */
	for (i = 0; i < td->o.iodepth; i++) {
		struct iovec *iov = &ld->iovecs[i];

		io_u = ld->io_u_index[i];
		iov->iov_base = io_u->buf;
		iov->iov_len = td_max_bs(td);
	}

	/* io_uring_cmd 인스턴스 생성 (SQE128/CQE32 포함) */
	err = fio_ioring_cmd_queue_init(td);
	if (err) {
		int init_err = errno;

		td_verror(td, init_err, "io_queue_init");
		return 1;
	}

	/* SQE 초기화 - NVMe는 128바이트 SQE이므로 2배 크기로 memset */
	for (i = 0; i < ld->iodepth; i++) {
		struct io_uring_sqe *sqe;

		if (o->cmd_type == FIO_URING_CMD_NVME) {
			sqe = &ld->sqes[i << 1];              /* 인덱스 2배 (SQE128) */
			memset(sqe, 0, 2 * sizeof(*sqe));      /* 128바이트 초기화 */
		} else {
			sqe = &ld->sqes[i];
			memset(sqe, 0, sizeof(*sqe));           /* 64바이트 초기화 */
		}
	}

	/* 파일 등록 */
	if (o->registerfiles) {
		err = fio_ioring_register_files(td);
		if (err) {
			td_verror(td, errno, "ioring_register_files");
			return 1;
		}
	}

	return 0;
}

/*
 * [함수] parse_prchk_flags
 * [역할] pi_chk 옵션 문자열을 파싱하여 prchk 비트마스크로 변환.
 *        "GUARD,REFTAG,APPTAG" 형식의 문자열을 NVMe PI 플래그로 변환.
 *
 * [파라미터]
 *   - o: io_uring 옵션 구조체
 * [반환값] 없음
 */
static void parse_prchk_flags(struct ioring_options *o)
{
	if (!o->pi_chk)
		return;

	/* 각 PI 검사 유형에 대해 문자열에서 키워드를 검색 */
	if (strstr(o->pi_chk, "GUARD") != NULL)
		o->prchk = NVME_IO_PRINFO_PRCHK_GUARD;     /* 가드 태그(CRC) 검사 */
	if (strstr(o->pi_chk, "REFTAG") != NULL)
		o->prchk |= NVME_IO_PRINFO_PRCHK_REF;      /* 참조 태그 검사 */
	if (strstr(o->pi_chk, "APPTAG") != NULL)
		o->prchk |= NVME_IO_PRINFO_PRCHK_APP;      /* 애플리케이션 태그 검사 */
}

/*
 * [함수] fio_ioring_cmd_init
 * [역할] io_uring_cmd 엔진(NVMe passthrough)의 NVMe 관련 초기화.
 *        쓰기 모드에 따른 opcode 설정, FUA 플래그 설정 등.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - ld: io_uring 데이터
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_cmd_init(struct thread_data *td, struct ioring_data *ld)
{
	struct ioring_options *o = td->eo;

	/* 쓰기가 포함된 워크로드인 경우, 쓰기 모드에 따라 NVMe opcode 결정 */
	if (td_write(td)) {
		switch (o->write_mode) {
		case FIO_URING_CMD_WMODE_UNCOR:
			ld->write_opcode = nvme_cmd_write_uncor;    /* Write Uncorrectable */
			break;
		case FIO_URING_CMD_WMODE_ZEROES:
			ld->write_opcode = nvme_cmd_write_zeroes;   /* Write Zeroes */
			/* DEAC(Deallocate) 비트 설정 시 CDW12 bit 25 */
			if (o->deac)
				ld->cdw12_flags[DDIR_WRITE] = 1 << 25;
			break;
		case FIO_URING_CMD_WMODE_VERIFY:
			ld->write_opcode = nvme_cmd_verify;         /* Verify */
			break;
		default:
			ld->write_opcode = nvme_cmd_write;          /* 일반 Write */
			break;
		}
	}

	/* FUA(Force Unit Access) 플래그 설정 - NVMe CDW12 bit 30 */
	if (o->readfua)
		ld->cdw12_flags[DDIR_READ] = 1 << 30;    /* 읽기 FUA */
	if (o->writefua)
		ld->cdw12_flags[DDIR_WRITE] = 1 << 30;   /* 쓰기 FUA */

	return 0;
}

/*
 * [함수] fio_ioring_init
 * [역할] io_uring 엔진의 메인 초기화 함수. fio의 init 콜백.
 *        엔진 데이터 구조체를 할당하고, 옵션을 검증하고,
 *        메타데이터/iovec/DSM 버퍼를 할당.
 *
 * io_uring과 io_uring_cmd 두 엔진 모두 이 함수를 공유.
 *
 * [초기화 순서]
 * 1. 옵션 검증 (sqpoll → registerfiles 필수, nr_files == open_files)
 * 2. ioring_data 구조체 할당
 * 3. iodepth를 2의 거듭제곱으로 반올림 (io_uring 요구사항)
 * 4. io_u_index, md_buf, pi_attr, iovecs 배열 할당
 * 5. cmdprio 초기화
 * 6. DSM(Dataset Management) 구조체 할당 (TRIM용)
 * 7. io_uring_cmd 엔진이면 NVMe 관련 추가 초기화
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 1 (실패)
 */
static int fio_ioring_init(struct thread_data *td)
{
	struct ioring_options *o = td->eo;
	struct ioring_data *ld;
	struct nvme_dsm *dsm;
	void *ptr;
	unsigned int dsm_size;
	unsigned long long md_size;
	int ret, i;
	struct nvme_cmd_ext_io_opts *ext_opts;

	/* sqthread submission requires registered files */
	/* SQPOLL 모드에서는 등록된 파일이 필수 (커널 스레드가 유저 fd 테이블 접근 불가) */
	if (o->sqpoll_thread)
		o->registerfiles = 1;

	/* 등록된 파일 사용 시 nr_files == open_files 이어야 함 (부분 등록 불가) */
	if (o->registerfiles && td->o.nr_files != td->o.open_files) {
		log_err("fio: io_uring registered files require nr_files to "
			"be identical to open_files\n");
		return 1;
	}

	/* 엔진 데이터 구조체 할당 */
	ld = calloc(1, sizeof(*ld));

	/* io_uring_cmd 엔진 여부 판별 (prep 함수 포인터로 구분) */
	ld->is_uring_cmd_eng = (td->io_ops->prep == fio_ioring_cmd_prep);

	/*
	 * The internal io_uring queue depth must be a power-of-2, as that's
	 * how the ring interface works. So round that up, in case the user
	 * set iodepth isn't a power-of-2. Leave the fio depth the same, as
	 * not to be driving too much of an iodepth, if we did round up.
	 */
	/*
	 * io_uring 내부 큐 깊이는 2의 거듭제곱이어야 함 (링 인터페이스 요구사항).
	 * 사용자 설정이 2의 거듭제곱이 아니면 올림.
	 * fio의 논리적 깊이는 그대로 유지하여 과도한 I/O를 방지.
	 */
	ld->iodepth = roundup_pow2(td->o.iodepth);

	/* io_u index */
	/* io_u 인덱스 배열 할당 - SQE 인덱스로 io_u를 빠르게 찾기 위함 */
	ld->io_u_index = calloc(td->o.iodepth, sizeof(struct io_u *));

	/* io_uring 경로에서 메타데이터 사용 시 apptag_mask 검증 */
	if (!ld->is_uring_cmd_eng && o->md_per_io_size) {
		if (o->apptag_mask != 0xffff) {
			log_err("fio: io_uring with metadata requires an apptag_mask of 0xffff\n");
			free(ld->io_u_index);
			free(ld);
			return 1;
		}
	}

	/*
	 * metadata buffer
	 * We are only supporting iomem=malloc / mem=malloc as of now.
	 */
	/*
	 * 메타데이터 버퍼 할당
	 * 현재 iomem=malloc / mem=malloc만 지원
	 */
	if (o->md_per_io_size && (!ld->is_uring_cmd_eng ||
	    (ld->is_uring_cmd_eng && o->cmd_type == FIO_URING_CMD_NVME))) {
		/* 전체 메타데이터 버퍼 크기 계산 (I/O당 크기 * iodepth + 정렬 여유) */
		md_size = (unsigned long long) o->md_per_io_size
				* (unsigned long long) td->o.iodepth;
		md_size += page_mask + td->o.mem_align;
		if (td->o.mem_align && td->o.mem_align > page_size)
			md_size += td->o.mem_align - page_size;
		ld->md_buf = malloc(md_size);
		if (!ld->md_buf) {
			free(ld->io_u_index);
			free(ld);
			return 1;
		}

		/* 비 NVMe passthrough 경로에서는 PI 속성 배열도 할당 */
		if (!ld->is_uring_cmd_eng) {
			ld->pi_attr = calloc(ld->iodepth, sizeof(struct io_uring_attr_pi));
			if (!ld->pi_attr) {
				free(ld->io_u_index);
				free(ld->md_buf);
				free(ld);
				return 1;
			}
		}

	}
	/* pi_chk 문자열을 비트마스크로 파싱 */
	parse_prchk_flags(o);
	/* 확장 I/O 옵션 설정 (PI 관련) */
	ext_opts = &ld->ext_opts;
	if (o->pi_act)
		ext_opts->io_flags |= NVME_IO_PRINFO_PRACT;   /* PI Action 비트 */
	ext_opts->io_flags |= o->prchk;                     /* PI 검사 플래그 */
	ext_opts->apptag = o->apptag;                        /* Application Tag */
	ext_opts->apptag_mask = o->apptag_mask;              /* Application Tag 마스크 */

	/* iovec 배열 할당 (scatter/gather I/O용) */
	ld->iovecs = calloc(ld->iodepth, sizeof(struct iovec));

	/* 엔진 데이터를 스레드 데이터에 연결 */
	td->io_ops_data = ld;

	/* 커맨드 우선순위 초기화 */
	ret = fio_cmdprio_init(td, &ld->cmdprio, &o->cmdprio_options);
	if (ret) {
		td_verror(td, EINVAL, "fio_ioring_init");
		return 1;
	}

	/*
	 * For io_uring_cmd, trims are async operations unless we are operating
	 * in zbd mode where trim means zone reset.
	 */
	/*
	 * io_uring_cmd에서 TRIM은 비동기 작업이지만,
	 * ZBD(존 블록 디바이스) 모드에서는 TRIM이 존 리셋을 의미하므로 동기 처리.
	 */
	if (td_trim(td) && td->o.zone_mode == ZONE_MODE_ZBD &&
	    ld->is_uring_cmd_eng) {
		td->io_ops->flags |= FIO_ASYNCIO_SYNC_TRIM;
	} else {
		/* DSM(Dataset Management) 구조체 할당 (TRIM/Deallocate 용) */
		dsm_size = sizeof(*ld->dsm);
		dsm_size += td->o.num_range * sizeof(struct nvme_dsm_range);
		ld->dsm = calloc(td->o.iodepth, dsm_size);
		ptr = ld->dsm;
		/* 각 I/O 슬롯에 대해 DSM 구조체의 범위 수 초기화 */
		for (i = 0; i < td->o.iodepth; i++) {
			dsm = (struct nvme_dsm *)ptr;
			dsm->nr_ranges = td->o.num_range;
			ptr += dsm_size;
		}
	}

	/* io_uring_cmd 엔진이면 NVMe 관련 추가 초기화 수행 */
	if (ld->is_uring_cmd_eng)
		return fio_ioring_cmd_init(td, ld);
	return 0;
}

/*
 * [함수] fio_ioring_io_u_init
 * [역할] 개별 io_u(I/O 유닛)의 초기화 함수. fio의 io_u_init 콜백.
 *        io_u를 io_u_index에 등록하고, 메타데이터/PI 버퍼를 연결.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 초기화할 I/O 유닛
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct nvme_pi_data *pi_data;
	char *p, *q;

	/* io_u를 인덱스 배열에 등록 (SQE 인덱스로 io_u를 찾을 수 있도록) */
	ld->io_u_index[io_u->index] = io_u;

	/* 메타데이터 버퍼의 해당 io_u 영역 계산 및 연결 */
	p = PTR_ALIGN(ld->md_buf, page_mask) + td->o.mem_align;
	p += o->md_per_io_size * io_u->index;
	io_u->mmap_data = p;   /* io_u의 메타데이터 버퍼 포인터 */

	/* PI 속성이 있으면 (비 NVMe passthrough 경로) 설정 */
	if (ld->pi_attr) {
		struct io_uring_attr_pi *pi_attr;

		/* 해당 io_u의 PI 속성 영역 계산 */
		q = ld->pi_attr;
		q += (sizeof(struct io_uring_attr_pi) * io_u->index);
		io_u->pi_attr = q;

		/* PI 속성 초기화 */
		pi_attr = io_u->pi_attr;
		pi_attr->len = o->md_per_io_size;    /* 메타데이터 크기 */
		pi_attr->app_tag = o->apptag;        /* Application Tag */
		pi_attr->flags = 0;
		/* PI 검사 플래그를 io_uring PI 속성 플래그로 변환 */
		if (o->prchk & NVME_IO_PRINFO_PRCHK_GUARD)
			pi_attr->flags |= IO_INTEGRITY_CHK_GUARD;
		if (o->prchk & NVME_IO_PRINFO_PRCHK_REF)
			pi_attr->flags |= IO_INTEGRITY_CHK_REFTAG;
		if (o->prchk & NVME_IO_PRINFO_PRCHK_APP)
			pi_attr->flags |= IO_INTEGRITY_CHK_APPTAG;
	}

	/* pi_act=0(호스트 측 PI 처리)인 경우 PI 데이터 구조체 할당 */
	if (!o->pi_act) {
		pi_data = calloc(1, sizeof(*pi_data));
		pi_data->io_flags |= o->prchk;          /* PI 검사 플래그 */
		pi_data->apptag_mask = o->apptag_mask;   /* App Tag 마스크 */
		pi_data->apptag = o->apptag;             /* App Tag 값 */
		io_u->engine_data = pi_data;              /* io_u에 엔진별 데이터 연결 */
	}

	return 0;
}

/*
 * [함수] fio_ioring_io_u_free
 * [역할] 개별 io_u의 리소스를 해제하는 함수. fio의 io_u_free 콜백.
 *        PI 데이터 구조체를 해제.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 해제할 I/O 유닛
 * [반환값] 없음
 */
static void fio_ioring_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct nvme_pi *pi = io_u->engine_data;

	free(pi);                     /* PI 데이터 해제 */
	io_u->engine_data = NULL;     /* 포인터 초기화 */
}

/*
 * [함수] fio_get_pi_info
 * [역할] 파일(블록 디바이스)에서 Protection Information 기능 정보를 조회.
 *        FS_IOC_GETLBMD_CAP ioctl을 사용하여 메타데이터/PI 지원 여부와
 *        관련 속성(크기, 유형, 오프셋 등)을 가져옴.
 *
 * 비 NVMe passthrough 경로에서 메타데이터가 있는 I/O에 사용.
 *
 * [파라미터]
 *   - f: fio 파일 구조체
 *   - data: NVMe 데이터 구조체 (결과 저장)
 * [반환값] 0 (성공), 음수 에러코드 (실패)
 */
static int fio_get_pi_info(struct fio_file *f, struct nvme_data *data)
{
	struct logical_block_metadata_cap md_cap;
	int ret;
	int fd, err = 0;

	/* 파일을 읽기 전용으로 열기 */
	fd = open(f->file_name, O_RDONLY);
	if (fd < 0)
		return -errno;

	/* ioctl로 논리 블록 메타데이터 기능 조회 */
	ret = ioctl(fd, FS_IOC_GETLBMD_CAP, &md_cap);
	if (ret < 0) {
		err = -errno;
		log_err("%s: failed to query protection information capabilities; error %d\n", f->file_name, errno);
		goto out;
	}

	/* 무결성 보호가 지원되지 않으면 에러 */
	if (!(md_cap.lbmd_flags & LBMD_PI_CAP_INTEGRITY)) {
		log_err("%s: Protection information not supported\n", f->file_name);
		err = -ENOTSUP;
		goto out;
	}

	/* Currently we don't support storage tags */
	/* 스토리지 태그는 현재 미지원 */
	if (md_cap.lbmd_storage_tag_size) {
		log_err("%s: Storage tag not supported\n", f->file_name);
		err = -ENOTSUP;
		goto out;
	}

	/* 조회된 정보를 NVMe 데이터 구조체에 저장 */
	data->lba_size = md_cap.lbmd_interval;         /* LBA 데이터 크기 */
	data->lba_shift = ilog2(data->lba_size);       /* LBA 크기의 log2 */
	data->ms = md_cap.lbmd_size;                   /* 메타데이터 크기 */
	data->pi_size = md_cap.lbmd_pi_size;           /* PI 크기 */
	data->pi_loc = !(md_cap.lbmd_pi_offset);       /* PI 위치 (시작=1, 끝=0) */

	/* Assume Type 1 PI if reference tags supported */
	/* 참조 태그가 지원되면 Type 1 PI, 아니면 Type 3 PI로 판단 */
	if (md_cap.lbmd_flags & LBMD_PI_CAP_REFTAG)
		data->pi_type = NVME_NS_DPS_PI_TYPE1;
	else
		data->pi_type = NVME_NS_DPS_PI_TYPE3;

	/* 가드 태그 유형 매핑 */
	switch (md_cap.lbmd_guard_tag_type) {
	case LBMD_PI_CSUM_CRC16_T10DIF:
		data->guard_type = NVME_NVM_NS_16B_GUARD;   /* T10 DIF CRC16 */
		break;
	case LBMD_PI_CSUM_CRC64_NVME:
		data->guard_type = NVME_NVM_NS_64B_GUARD;   /* NVMe CRC64 */
		break;
	default:
		log_err("%s: unsupported checksum type %d\n", f->file_name,
				md_cap.lbmd_guard_tag_type);
		err = -ENOTSUP;
		goto out;
	}

out:
	close(fd);
	return err;
}

/*
 * [함수] fio_ioring_open_file_md
 * [역할] 메타데이터가 있는 파일 열기 시 PI 정보를 조회하여
 *        파일 엔진 데이터에 저장.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 열려는 파일
 * [반환값] 0 (성공), 음수 (실패)
 */
static inline int fio_ioring_open_file_md(struct thread_data *td, struct fio_file *f)
{
	int ret = 0;
	struct nvme_data *data = NULL;

	/* 이미 엔진 데이터가 설정되어 있으면 건너뜀 (중복 호출 방지) */
	data = FILE_ENG_DATA(f);
	if (data == NULL) {
		data = calloc(1, sizeof(struct nvme_data));
		/* PI 정보 조회 */
		ret = fio_get_pi_info(f, data);
		if (ret) {
			free(data);
			return ret;
		}

		/* 파일에 엔진 데이터 연결 */
		FILE_SET_ENG_DATA(f, data);
	}

	return ret;
}

/*
 * [함수] fio_ioring_open_file
 * [역할] io_uring 엔진의 파일 열기 함수. fio의 open_file 콜백.
 *        메타데이터가 있으면 PI 정보를 조회하고,
 *        등록된 파일 사용 시 fd를 복원.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 열려는 파일
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_open_file(struct thread_data *td, struct fio_file *f)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;

	if (o->md_per_io_size) {
		/*
		 * This will be a no-op when called by the io_uring_cmd
		 * ioengine because engine data has already been collected by
		 * the time this call is made
		 */
		/*
		 * io_uring_cmd 엔진에서 호출 시에는 no-op (이미 엔진 데이터가 수집됨).
		 * 일반 io_uring에서만 PI 정보를 실제로 조회.
		 */
		int ret = fio_ioring_open_file_md(td, f);
		if (ret)
			return ret;
	}

	/* 등록된 파일이 아니면 일반적인 파일 열기 */
	if (!ld || !o->registerfiles)
		return generic_open_file(td, f);

	/* 등록된 파일: 이전에 저장한 fd를 복원 */
	f->fd = ld->fds[f->engine_pos];
	return 0;
}

/*
 * [함수] verify_params
 * [역할] NVMe 디바이스의 블록 크기와 메타데이터 설정이 올바른지 검증.
 *        블록 크기가 LBA 크기의 배수인지, 메타데이터 버퍼가 충분한지 확인.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - data: NVMe 디바이스 데이터
 *   - f: 대상 파일
 *   - ddir: I/O 방향 (READ, WRITE, TRIM)
 * [반환값] 0 (유효), 1 (무효)
 */
static int verify_params(struct thread_data *td, struct nvme_data *data,
			 struct fio_file *f, enum fio_ddir ddir)
{
	struct ioring_options *o = td->eo;
	unsigned int lba_size;

	/* 확장 LBA가 있으면 그 크기 사용, 없으면 기본 LBA 크기 */
	lba_size = data->lba_ext ? data->lba_ext : data->lba_size;
	/* 블록 크기가 LBA 크기의 배수인지 확인 */
	if (td->o.min_bs[ddir] % lba_size || td->o.max_bs[ddir] % lba_size) {
		if (data->lba_ext) {
			log_err("%s: block size must be a multiple of %u "
				"(LBA data size + Metadata size)\n", f->file_name, lba_size);
			/* 고정 블록 크기가 LBA 데이터 크기의 배수인 경우, 올바른 값을 제안 */
			if (td->o.min_bs[ddir] == td->o.max_bs[ddir] &&
			    !(td->o.min_bs[ddir] % data->lba_size)) {
				/* fixed block size is actually a multiple of LBA data size */
				unsigned long long suggestion = lba_size *
					(td->o.min_bs[ddir] / data->lba_size);
				log_err("Did you mean to use a block size of %llu?\n", suggestion);
			}
		} else {
			log_err("%s: block size must be a multiple of LBA data size\n",
				f->file_name);
		}
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}
	/* 메타데이터가 있고 확장 LBA가 아닌 경우, md_per_io_size가 충분한지 확인 */
	if (data->ms && !data->lba_ext && ddir != DDIR_TRIM &&
	    (o->md_per_io_size < ((td->o.max_bs[ddir] / data->lba_size) * data->ms))) {
		log_err("%s: md_per_io_size should be at least %llu bytes\n",
			f->file_name,
			((td->o.max_bs[ddir] / data->lba_size) * data->ms));
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}

	return 0;
}

/*
 * [함수] fio_ioring_open_nvme
 * [역할] NVMe 디바이스를 열고 네임스페이스 정보를 조회.
 *        NVMe 관련 설정 검증(블록 크기, PI, 쓰기 모드 등) 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: NVMe 디바이스 파일
 * [반환값] 0 (성공), 음수/1 (실패)
 */
static int fio_ioring_open_nvme(struct thread_data *td, struct fio_file *f)
{
	struct ioring_options *o = td->eo;
	struct nvme_data *data = NULL;
	__u64 nlba = 0;    /* 네임스페이스의 총 LBA 수 */
	int ret;

	/* Store the namespace-id and lba size. */
	/* 네임스페이스 ID와 LBA 크기를 저장 */
	data = FILE_ENG_DATA(f);
	if (data == NULL) {
		data = calloc(1, sizeof(struct nvme_data));
		/* NVMe 디바이스 정보 조회 (네임스페이스 정보, LBA 크기 등) */
		ret = fio_nvme_get_info(f, &nlba, o->pi_act, data);
		if (ret) {
			free(data);
			return ret;
		}

		FILE_SET_ENG_DATA(f, data);
	}

	/* 읽기/쓰기/트림 방향별로 파라미터 검증 */
	for_each_rw_ddir(ddir) {
		ret = verify_params(td, data, f, ddir);
		if (ret)
			return ret;
	}

	/*
	 * For extended logical block sizes we cannot use verify when
	 * end to end data protection checks are enabled, as the PI
	 * section of data buffer conflicts with verify.
	 */
	/*
	 * 확장 논리 블록(메타데이터가 데이터에 포함됨) 크기에서는
	 * E2E 데이터 보호가 활성화된 경우 fio verify를 사용할 수 없음.
	 * (PI 영역과 verify 데이터가 충돌)
	 */
	if (data->ms && data->pi_type && data->lba_ext &&
	    td->o.verify != VERIFY_NONE) {
		log_err("%s: for extended LBA, verify cannot be used when E2E "
			"data protection is enabled\n", f->file_name);
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}

	/* 특수 쓰기 모드(uncor, zeroes, verify)인데 쓰기가 포함되지 않으면 에러 */
	if (o->write_mode != FIO_URING_CMD_WMODE_WRITE && !td_write(td)) {
		log_err("%s: 'readwrite=|rw=' has no write\n", f->file_name);
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}

	return 0;
}

/*
 * [함수] fio_ioring_cmd_open_file
 * [역할] io_uring_cmd 엔진의 파일 열기 함수.
 *        NVMe인 경우 NVMe 관련 검증 후, 일반 파일 열기 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 열려는 파일
 * [반환값] 0 (성공), 음수/1 (실패)
 */
static int fio_ioring_cmd_open_file(struct thread_data *td, struct fio_file *f)
{
	struct ioring_options *o = td->eo;

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		int ret;

		/* NVMe 디바이스 정보 조회 및 검증 */
		ret = fio_ioring_open_nvme(td, f);
		if (ret)
			return ret;
	}

	/* 공통 파일 열기 (메타데이터 처리 + 등록된 파일/일반 열기) */
	return fio_ioring_open_file(td, f);
}

/*
 * [함수] fio_ioring_close_file
 * [역할] io_uring 엔진의 파일 닫기 함수.
 *        등록된 파일 사용 시 fd를 -1로 설정만 하고 (커널 내부 참조 유지),
 *        아니면 일반 닫기 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 닫으려는 파일
 * [반환값] 0 (성공)
 */
static int fio_ioring_close_file(struct thread_data *td, struct fio_file *f)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;

	/* 등록된 파일이 아니면 일반 닫기 */
	if (!ld || !o->registerfiles)
		return generic_close_file(td, f);

	/* 등록된 파일: fd를 -1로만 설정 (커널 내부에서 참조 유지 중) */
	f->fd = -1;
	return 0;
}

/*
 * [함수] fio_ioring_cmd_close_file
 * [역할] io_uring_cmd 엔진의 파일 닫기 함수.
 *        NVMe인 경우 엔진 데이터(NVMe 정보)를 해제하고,
 *        일반 닫기 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 닫으려는 파일
 * [반환값] 0 (성공)
 */
static int fio_ioring_cmd_close_file(struct thread_data *td,
				     struct fio_file *f)
{
	struct ioring_options *o = td->eo;

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		/* NVMe 데이터 해제 */
		struct nvme_data *data = FILE_ENG_DATA(f);

		FILE_SET_ENG_DATA(f, NULL);
		free(data);
	}

	return fio_ioring_close_file(td, f);
}

/*
 * [함수] fio_ioring_cmd_get_file_size
 * [역할] io_uring_cmd 엔진의 파일 크기 조회 함수.
 *        NVMe인 경우 네임스페이스 정보에서 크기를 계산.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_get_file_size(struct thread_data *td,
					struct fio_file *f)
{
	struct ioring_options *o = td->eo;

	/* 이미 크기를 알고 있으면 조기 반환 */
	if (fio_file_size_known(f))
		return 0;

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		struct nvme_data *data = NULL;
		__u64 nlba = 0;   /* 총 LBA 수 */
		int ret;

		data = calloc(1, sizeof(struct nvme_data));
		/* NVMe 네임스페이스 정보 조회 */
		ret = fio_nvme_get_info(f, &nlba, o->pi_act, data);
		if (ret) {
			free(data);
			return ret;
		}

		/* 파일 크기 = LBA 크기 * LBA 수 */
		if (data->lba_ext)
			f->real_file_size = data->lba_ext * nlba;   /* 확장 LBA (데이터+메타데이터) */
		else
			f->real_file_size = data->lba_size * nlba;  /* 기본 LBA (데이터만) */
		fio_file_set_size_known(f);   /* 크기 확인 완료 표시 */

		FILE_SET_ENG_DATA(f, data);
		return 0;
	}
	/* 비 NVMe: 일반적인 파일 크기 조회 */
	return generic_get_file_size(td, f);
}

/*
 * [함수] fio_ioring_get_zoned_model
 * [역할] 블록 디바이스의 ZBD(존 블록 디바이스) 모델을 조회.
 *        blkzoned 유틸리티에 위임.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - model: ZBD 모델 결과 (출력)
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_get_zoned_model(struct thread_data *td,
				      struct fio_file *f,
				      enum zbd_zoned_model *model)
{
	return blkzoned_get_zoned_model(td, f, model);
}

/*
 * [함수] fio_ioring_report_zones
 * [역할] 블록 디바이스의 존 정보를 보고.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - offset: 시작 오프셋
 *   - zbdz: 존 정보 배열 (출력)
 *   - nr_zones: 조회할 존 수
 * [반환값] 실제 보고된 존 수
 */
static int fio_ioring_report_zones(struct thread_data *td,
				   struct fio_file *f, uint64_t offset,
				   struct zbd_zone *zbdz,
				   unsigned int nr_zones)
{
	return blkzoned_report_zones(td, f, offset, zbdz, nr_zones);
}

/*
 * [함수] fio_ioring_reset_wp
 * [역할] 존 블록 디바이스의 Write Pointer를 리셋.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - offset: 리셋할 존의 시작 오프셋
 *   - length: 리셋할 범위 길이
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_reset_wp(struct thread_data *td, struct fio_file *f,
			       uint64_t offset, uint64_t length)
{
	return blkzoned_reset_wp(td, f, offset, length);
}

/*
 * [함수] fio_ioring_get_max_open_zones
 * [역할] ZBD에서 동시에 열 수 있는 최대 존 수를 조회.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - max_open_zones: 최대 오픈 존 수 (출력)
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_get_max_open_zones(struct thread_data *td,
					 struct fio_file *f,
					 unsigned int *max_open_zones)
{
	return blkzoned_get_max_open_zones(td, f, max_open_zones);
}

/*
 * [함수] fio_ioring_finish_zone
 * [역할] ZBD의 존을 FULL 상태로 전환(Finish).
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - offset: 존의 시작 오프셋
 *   - length: 존 길이
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_finish_zone(struct thread_data *td, struct fio_file *f,
				  uint64_t offset, uint64_t length)
{
	return blkzoned_finish_zone(td, f, offset, length);
}

/*
 * [함수] fio_ioring_move_zone_wp
 * [역할] ZBD의 존 Write Pointer를 이동.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - z: 대상 존
 *   - length: 이동할 길이
 *   - buf: 데이터 버퍼
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_move_zone_wp(struct thread_data *td, struct fio_file *f,
				   struct zbd_zone *z, uint64_t length,
				   const char *buf)
{
	return blkzoned_move_zone_wp(td, f, z, length, buf);
}

/*
 * [함수] fio_ioring_cmd_get_zoned_model
 * [역할] NVMe 디바이스의 ZNS(Zoned Namespace) 모델을 조회.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - model: ZBD 모델 결과 (출력)
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_get_zoned_model(struct thread_data *td,
					  struct fio_file *f,
					  enum zbd_zoned_model *model)
{
	return fio_nvme_get_zoned_model(td, f, model);
}

/*
 * [함수] fio_ioring_cmd_report_zones
 * [역할] NVMe ZNS 디바이스의 존 정보를 보고.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - offset: 시작 오프셋
 *   - zbdz: 존 정보 배열 (출력)
 *   - nr_zones: 조회할 존 수
 * [반환값] 실제 보고된 존 수
 */
static int fio_ioring_cmd_report_zones(struct thread_data *td,
				       struct fio_file *f, uint64_t offset,
				       struct zbd_zone *zbdz,
				       unsigned int nr_zones)
{
	return fio_nvme_report_zones(td, f, offset, zbdz, nr_zones);
}

/*
 * [함수] fio_ioring_cmd_reset_wp
 * [역할] NVMe ZNS 디바이스의 존 Write Pointer를 리셋.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - offset: 리셋할 존의 시작 오프셋
 *   - length: 리셋할 범위 길이
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_reset_wp(struct thread_data *td, struct fio_file *f,
				   uint64_t offset, uint64_t length)
{
	return fio_nvme_reset_wp(td, f, offset, length);
}

/*
 * [함수] fio_ioring_cmd_get_max_open_zones
 * [역할] NVMe ZNS 디바이스에서 동시에 열 수 있는 최대 존 수를 조회.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - max_open_zones: 최대 오픈 존 수 (출력)
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_get_max_open_zones(struct thread_data *td,
					     struct fio_file *f,
					     unsigned int *max_open_zones)
{
	return fio_nvme_get_max_open_zones(td, f, max_open_zones);
}

/*
 * [함수] fio_ioring_cmd_fetch_ruhs
 * [역할] NVMe FDP(Flexible Data Placement)의 RUH(Reclaim Unit Handle) 상태를 조회.
 *        FDP는 SSD의 GC(Garbage Collection) 효율을 높이기 위해
 *        데이터 배치를 제어하는 NVMe 기능.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 *   - fruhs_info: RUH 정보 결과 (입출력)
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_fetch_ruhs(struct thread_data *td, struct fio_file *f,
				     struct fio_ruhs_info *fruhs_info)
{
	struct nvme_fdp_ruh_status *ruhs;
	int bytes, nr_ruhs, ret, i;

	nr_ruhs = fruhs_info->nr_ruhs;
	/* RUH 상태 구조체 크기 계산 및 할당 */
	bytes = sizeof(*ruhs) + fruhs_info->nr_ruhs * sizeof(struct nvme_fdp_ruh_status_desc);

	ruhs = calloc(1, bytes);
	if (!ruhs)
		return -ENOMEM;

	/* NVMe I/O Management Receive 커맨드로 RUH 상태 조회 */
	ret = fio_nvme_iomgmt_ruhs(td, f, ruhs, bytes);
	if (ret)
		goto free;

	/* 결과를 fio RUH 정보 구조체로 변환 */
	fruhs_info->nr_ruhs = le16_to_cpu(ruhs->nruhsd);
	for (i = 0; i < nr_ruhs; i++)
		fruhs_info->plis[i] = le16_to_cpu(ruhs->ruhss[i].pid);
free:
	free(ruhs);
	return ret;
}

/*
 * === io_uring 엔진 등록 구조체 ===
 *
 * 일반 io_uring 엔진 (블록 I/O)
 * 이름: "io_uring"
 *
 * [콜백 함수 매핑과 I/O 흐름]
 * init        → fio_ioring_init          (엔진 초기화, 메모리 할당)
 * post_init   → fio_ioring_post_init     (큐 초기화, mmap, 파일 등록)
 * io_u_init   → fio_ioring_io_u_init     (개별 io_u 초기화)
 * io_u_free   → fio_ioring_io_u_free     (개별 io_u 해제)
 * prep        → fio_ioring_prep          (SQE 준비 - opcode, fd, offset 등 설정)
 * queue       → fio_ioring_queue         (SQ Ring에 SQE 인덱스 추가, tail 증가)
 * commit      → fio_ioring_commit        (io_uring_enter()로 커널에 제출)
 * getevents   → fio_ioring_getevents     (CQ Ring에서 CQE 수확)
 * event       → fio_ioring_event         (개별 CQE → io_u 변환)
 * cleanup     → fio_ioring_cleanup       (전체 리소스 해제)
 */
static struct ioengine_ops ioengine_uring = {
	.name			= "io_uring",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_NO_OFFLOAD | FIO_ASYNCIO_SETS_ISSUE_TIME |
				  FIO_ATOMICWRITES,
	.init			= fio_ioring_init,
	.post_init		= fio_ioring_post_init,
	.io_u_init		= fio_ioring_io_u_init,
	.io_u_free		= fio_ioring_io_u_free,
	.prep			= fio_ioring_prep,
	.queue			= fio_ioring_queue,
	.commit			= fio_ioring_commit,
	.getevents		= fio_ioring_getevents,
	.event			= fio_ioring_event,
	.cleanup		= fio_ioring_cleanup,
	.open_file		= fio_ioring_open_file,
	.close_file		= fio_ioring_close_file,
	.get_file_size		= generic_get_file_size,
	.get_zoned_model	= fio_ioring_get_zoned_model,
	.report_zones		= fio_ioring_report_zones,
	.reset_wp		= fio_ioring_reset_wp,
	.get_max_open_zones	= fio_ioring_get_max_open_zones,
	.finish_zone		= fio_ioring_finish_zone,
	.move_zone_wp		= fio_ioring_move_zone_wp,
	.options		= options,
	.option_struct_size	= sizeof(struct ioring_options),
};

/*
 * io_uring_cmd 엔진 등록 구조체
 * 이름: "io_uring_cmd"
 *
 * NVMe passthrough용 io_uring 엔진.
 * 일반 io_uring과의 차이점:
 * - prep: fio_ioring_cmd_prep (NVMe 커맨드를 SQE에 직접 채움)
 * - post_init: fio_ioring_cmd_post_init (SQE128/CQE32 초기화)
 * - event: fio_ioring_cmd_event (NVMe 상태 코드 처리)
 * - errdetails: NVMe SCT/SC 에러 상세 정보
 * - open_file: NVMe 네임스페이스 정보 조회 포함
 * - get_file_size: NVMe 네임스페이스에서 크기 계산
 * - fdp_fetch_ruhs: NVMe FDP RUH 상태 조회
 *
 * queue, commit, getevents는 일반 io_uring과 동일한 함수 사용
 * (SQ/CQ Ring 매커니즘은 동일, SQE 내용만 다름)
 */
static struct ioengine_ops ioengine_uring_cmd = {
	.name			= "io_uring_cmd",
	.version		= FIO_IOOPS_VERSION,
	.flags			= FIO_NO_OFFLOAD | FIO_MEMALIGN | FIO_RAWIO |
					FIO_ASYNCIO_SETS_ISSUE_TIME |
					FIO_MULTI_RANGE_TRIM |
					FIO_ASYNCIO_SYNC_SYNCFS,
	.init			= fio_ioring_init,
	.post_init		= fio_ioring_cmd_post_init,
	.io_u_init		= fio_ioring_io_u_init,
	.io_u_free		= fio_ioring_io_u_free,
	.prep			= fio_ioring_cmd_prep,
	.queue			= fio_ioring_queue,
	.commit			= fio_ioring_commit,
	.getevents		= fio_ioring_getevents,
	.event			= fio_ioring_cmd_event,
	.errdetails		= fio_ioring_cmd_errdetails,
	.cleanup		= fio_ioring_cleanup,
	.open_file		= fio_ioring_cmd_open_file,
	.close_file		= fio_ioring_cmd_close_file,
	.get_file_size		= fio_ioring_cmd_get_file_size,
	.get_zoned_model	= fio_ioring_cmd_get_zoned_model,
	.report_zones		= fio_ioring_cmd_report_zones,
	.reset_wp		= fio_ioring_cmd_reset_wp,
	.get_max_open_zones	= fio_ioring_cmd_get_max_open_zones,
	.options		= options,
	.option_struct_size	= sizeof(struct ioring_options),
	.fdp_fetch_ruhs		= fio_ioring_cmd_fetch_ruhs,
};

/*
 * [함수] fio_ioring_register
 * [역할] fio 시작 시 io_uring 엔진과 io_uring_cmd 엔진을 fio에 등록.
 *        fio_init 속성으로 프로그램 시작 시 자동 호출됨 (constructor).
 * [반환값] 없음
 */
static void fio_init fio_ioring_register(void)
{
	register_ioengine(&ioengine_uring);       /* io_uring 엔진 등록 */
	register_ioengine(&ioengine_uring_cmd);   /* io_uring_cmd 엔진 등록 */
}

/*
 * [함수] fio_ioring_unregister
 * [역할] fio 종료 시 io_uring 엔진 등록을 해제.
 *        fio_exit 속성으로 프로그램 종료 시 자동 호출됨 (destructor).
 * [반환값] 없음
 */
static void fio_exit fio_ioring_unregister(void)
{
	unregister_ioengine(&ioengine_uring);       /* io_uring 엔진 등록 해제 */
	unregister_ioengine(&ioengine_uring_cmd);   /* io_uring_cmd 엔진 등록 해제 */
}
#endif
