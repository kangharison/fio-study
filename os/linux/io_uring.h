/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * Header file for the io_uring interface.
 *
 * Copyright (C) 2019 Jens Axboe
 * Copyright (C) 2019 Christoph Hellwig
 */
/*
 * [한국어 설명]
 * io_uring.h - Linux io_uring 커널 인터페이스 정의 (UAPI 헤더)
 *
 * === io_uring 아키텍처 ===
 * io_uring은 리눅스 커널 5.1+에서 도입된 고성능 비동기 I/O 프레임워크.
 * 유저 공간과 커널 공간 사이에 공유 메모리 링 버퍼를 사용하여
 * 시스템 콜 오버헤드를 최소화하고, 배치 처리를 지원함.
 *
 * === 핵심 구조 ===
 *
 * 1. SQ (Submission Queue) Ring:
 *    - 유저 → 커널 방향의 I/O 요청 제출용 링 버퍼
 *    - SQE(Submission Queue Entry) 배열과 인덱스 배열로 구성
 *    - 유저가 tail을 증가시켜 요청 추가, 커널이 head를 증가시켜 소비
 *
 * 2. CQ (Completion Queue) Ring:
 *    - 커널 → 유저 방향의 I/O 완료 통지용 링 버퍼
 *    - CQE(Completion Queue Entry) 배열로 구성
 *    - 커널이 tail을 증가시켜 완료 추가, 유저가 head를 증가시켜 소비
 *
 * 3. 메모리 매핑:
 *    - IORING_OFF_SQ_RING (0x0): SQ 링 오프셋 → mmap으로 SQ 메타데이터 공유
 *    - IORING_OFF_CQ_RING (0x8000000): CQ 링 오프셋
 *    - IORING_OFF_SQES (0x10000000): SQE 배열 오프셋
 *
 * === 주요 시스템 콜 ===
 * - io_uring_setup(): 링 생성 및 초기화 → io_uring_params 반환
 * - io_uring_enter(): SQ 제출 알림 및/또는 CQ 이벤트 대기
 * - io_uring_register(): 버퍼/파일/이벤트fd 등을 커널에 사전 등록
 *
 * === 성능 최적화 기법 ===
 * - SQPOLL: 커널 스레드가 SQ를 폴링 → io_uring_enter() 호출 불필요
 * - IOPOLL: 인터럽트 없이 완료를 폴링 → NVMe 등 고속 디바이스용
 * - 등록된 버퍼/파일: 매 I/O마다의 pinning/lookup 비용 제거
 * - 배치 제출: 여러 SQE를 한 번에 제출
 */
#ifndef LINUX_IO_URING_H
#define LINUX_IO_URING_H

#include <linux/fs.h>
#include <linux/types.h>

/*
 * [한국어] SQE (Submission Queue Entry) - I/O 요청 하나를 기술하는 구조체
 * 유저가 SQ 링에 이 구조체를 채워 넣으면 커널이 해당 I/O를 수행함.
 * 크기: 기본 64바이트, IORING_SETUP_SQE128 사용 시 128바이트
 */
struct io_uring_sqe {
	__u8	opcode;		/* [한국어] 연산 유형 (IORING_OP_READ, IORING_OP_WRITE 등) */
	__u8	flags;		/* [한국어] SQE 플래그 (IOSQE_FIXED_FILE, IOSQE_IO_LINK 등) */
	__u16	ioprio;		/* [한국어] I/O 우선순위 (ioprio_value로 설정) */
	__s32	fd;		/* [한국어] 대상 파일 디스크립터 (또는 등록된 파일 인덱스) */
	union {
		__u64	off;	/* [한국어] 파일 내 오프셋 (read/write 시) */
		__u64	addr2;
		struct {
			__u32	cmd_op;   /* [한국어] uring_cmd의 명령 opcode */
			__u32	__pad1;
		};
	};
	union {
		__u64	addr;	/* [한국어] 데이터 버퍼 주소 또는 iovec 배열 포인터 */
		__u64	splice_off_in;
	};
	__u32	len;		/* [한국어] 버퍼 크기 또는 iovec 개수 */
	union {
		__kernel_rwf_t	rw_flags;
		__u32		fsync_flags;
		__u16		poll_events;	/* compatibility */
		__u32		poll32_events;	/* word-reversed for BE */
		__u32		sync_range_flags;
		__u32		msg_flags;
		__u32		timeout_flags;
		__u32		accept_flags;
		__u32		cancel_flags;
		__u32		open_flags;
		__u32		statx_flags;
		__u32		fadvise_advice;
		__u32		splice_flags;
		__u32		rename_flags;
		__u32		unlink_flags;
		__u32		hardlink_flags;
		__u32		uring_cmd_flags;
	};
	__u64	user_data;	/* [한국어] 완료 시 CQE에 그대로 전달되는 사용자 데이터 (fio에서는 io_u 포인터) */
	/* pack this to avoid bogus arm OABI complaints */
	union {
		/* index into fixed buffers, if used */
		__u16	buf_index;
		/* for grouped buffer selection */
		__u16	buf_group;
	} __attribute__((packed));
	/* personality to use, if used */
	__u16	personality;
	union {
		__s32	splice_fd_in;
		__u32	file_index;
	};
	union {
		struct {
			__u64	addr3;
			__u64	__pad2[1];
		};
		struct {
			__u64	attr_ptr; /* pointer to attribute information */
			__u64	attr_type_mask; /* bit mask of attributes */
		};
		/*
		 * If the ring is initialized with IORING_SETUP_SQE128, then
		 * this field is used for 80 bytes of arbitrary command data
		 */
		__u8	cmd[0];
	};
};

/* [한국어] SQE 속성 유형 마스크 플래그 */
#define IORING_RW_ATTR_FLAG_PI	(1U << 0)  /* PI(Protection Information) 속성 사용 */
/* [한국어] PI 속성 정보 - SQE에 연결하여 파일시스템 수준 PI를 지원 */
struct io_uring_attr_pi {
		__u16	flags;
		__u16	app_tag;
		__u32	len;
		__u64	addr;
		__u64	seed;
		__u64	rsvd;
};
/* [한국어] SQE 플래그 비트 위치 열거형 */
enum {
	IOSQE_FIXED_FILE_BIT,         /* 등록된 파일 인덱스 사용 */
	IOSQE_IO_DRAIN_BIT,           /* 이전 모든 I/O 완료 후 실행 */
	IOSQE_IO_LINK_BIT,            /* 다음 SQE와 연결 (이전 성공 시에만 실행) */
	IOSQE_IO_HARDLINK_BIT,        /* 강한 연결 (이전 결과와 무관하게 실행) */
	IOSQE_ASYNC_BIT,              /* 항상 비동기 실행 강제 */
	IOSQE_BUFFER_SELECT_BIT,      /* buf_group에서 버퍼 선택 */
	IOSQE_CQE_SKIP_SUCCESS_BIT,  /* 성공 시 CQE 생성 안 함 */
};

/*
 * sqe->flags
 */
/* use fixed fileset */
#define IOSQE_FIXED_FILE	(1U << IOSQE_FIXED_FILE_BIT)
/* issue after inflight IO */
#define IOSQE_IO_DRAIN		(1U << IOSQE_IO_DRAIN_BIT)
/* links next sqe */
#define IOSQE_IO_LINK		(1U << IOSQE_IO_LINK_BIT)
/* like LINK, but stronger */
#define IOSQE_IO_HARDLINK	(1U << IOSQE_IO_HARDLINK_BIT)
/* always go async */
#define IOSQE_ASYNC		(1U << IOSQE_ASYNC_BIT)
/* select buffer from sqe->buf_group */
#define IOSQE_BUFFER_SELECT	(1U << IOSQE_BUFFER_SELECT_BIT)
/* don't post CQE if request succeeded */
#define IOSQE_CQE_SKIP_SUCCESS	(1U << IOSQE_CQE_SKIP_SUCCESS_BIT)

/*
 * [한국어] io_uring_setup() 플래그
 * 링 생성 시 동작 모드를 설정. fio의 io_uring 엔진에서 hipri, sqpoll 등 옵션으로 제어.
 */
#define IORING_SETUP_IOPOLL	(1U << 0)	/* [한국어] I/O 완료를 폴링으로 확인 (인터럽트 없음, NVMe용) */
#define IORING_SETUP_SQPOLL	(1U << 1)	/* [한국어] 커널 SQ 폴링 스레드 사용 (시스콜 최소화) */
#define IORING_SETUP_SQ_AFF	(1U << 2)	/* [한국어] sq_thread_cpu 유효 (SQ 폴링 스레드 CPU 지정) */
#define IORING_SETUP_CQSIZE	(1U << 3)	/* [한국어] 애플리케이션이 CQ 크기 지정 */
#define IORING_SETUP_CLAMP	(1U << 4)	/* [한국어] SQ/CQ 크기를 최대값으로 클램프 */
#define IORING_SETUP_ATTACH_WQ	(1U << 5)	/* [한국어] 기존 workqueue에 연결 */
#define IORING_SETUP_R_DISABLED	(1U << 6)	/* [한국어] 비활성 상태로 시작 */
#define IORING_SETUP_SUBMIT_ALL	(1U << 7)	/* [한국어] 에러 시에도 제출 계속 */
/*
 * Cooperative task running. When requests complete, they often require
 * forcing the submitter to transition to the kernel to complete. If this
 * flag is set, work will be done when the task transitions anyway, rather
 * than force an inter-processor interrupt reschedule. This avoids interrupting
 * a task running in userspace, and saves an IPI.
 */
#define IORING_SETUP_COOP_TASKRUN	(1U << 8)
/*
 * If COOP_TASKRUN is set, get notified if task work is available for
 * running and a kernel transition would be needed to run it. This sets
 * IORING_SQ_TASKRUN in the sq ring flags. Not valid with COOP_TASKRUN.
 */
#define IORING_SETUP_TASKRUN_FLAG	(1U << 9)

#define IORING_SETUP_SQE128		(1U << 10) /* [한국어] SQE 128바이트 (uring_cmd용 추가 공간) */
#define IORING_SETUP_CQE32		(1U << 11) /* [한국어] CQE 32바이트 (추가 완료 데이터) */

/*
 * Only one task is allowed to submit requests
 */
#define IORING_SETUP_SINGLE_ISSUER	(1U << 12)

/*
 * Defer running task work to get events.
 * Rather than running bits of task work whenever the task transitions
 * try to do it just before it is needed.
 */
#define IORING_SETUP_DEFER_TASKRUN	(1U << 13)

/*
 * Application provides the memory for the rings
 */
#define IORING_SETUP_NO_MMAP		(1U << 14)

/*
 * Register the ring fd in itself for use with
 * IORING_REGISTER_USE_REGISTERED_RING; return a registered fd index rather
 * than an fd.
 */
#define IORING_SETUP_REGISTERED_FD_ONLY	(1U << 15)

/*
 * Removes indirection through the SQ index array.
 */
#define IORING_SETUP_NO_SQARRAY		(1U << 16)

/* [한국어] 하이브리드 폴링: 완료가 임박할 때까지 sleep 후 폴링으로 전환 */
#define IORING_SETUP_HYBRID_IOPOLL	(1U << 17)

/*
 * Allow both 16b and 32b CQEs. If a 32b CQE is posted, it will have
 * IORING_CQE_F_32 set in cqe->flags.
 */
#define IORING_SETUP_CQE_MIXED		(1U << 18)

/*
 * [한국어] io_uring 연산 코드 (SQE의 opcode 필드에 설정)
 * fio에서 주로 사용하는 opcode:
 * - IORING_OP_READV/WRITEV: vectored I/O (iovec 사용)
 * - IORING_OP_READ_FIXED/WRITE_FIXED: 등록된 버퍼로 I/O (fixedbufs 옵션)
 * - IORING_OP_READ/WRITE: 단일 버퍼 I/O
 * - IORING_OP_FSYNC: fsync/fdatasync
 * - IORING_OP_URING_CMD: NVMe passthrough 등 디바이스별 커맨드
 */
enum {
	IORING_OP_NOP,              /* 무연산 (테스트/벤치마크용) */
	IORING_OP_READV,            /* readv - scatter read */
	IORING_OP_WRITEV,           /* writev - gather write */
	IORING_OP_FSYNC,            /* fsync/fdatasync */
	IORING_OP_READ_FIXED,       /* 등록된 버퍼로 읽기 */
	IORING_OP_WRITE_FIXED,      /* 등록된 버퍼로 쓰기 */
	IORING_OP_POLL_ADD,         /* 폴 이벤트 추가 */
	IORING_OP_POLL_REMOVE,      /* 폴 이벤트 제거 */
	IORING_OP_SYNC_FILE_RANGE,  /* sync_file_range */
	IORING_OP_SENDMSG,          /* sendmsg */
	IORING_OP_RECVMSG,          /* recvmsg */
	IORING_OP_TIMEOUT,          /* 타임아웃 설정 */
	IORING_OP_TIMEOUT_REMOVE,   /* 타임아웃 제거 */
	IORING_OP_ACCEPT,           /* accept 연결 수락 */
	IORING_OP_ASYNC_CANCEL,     /* 진행 중인 요청 취소 */
	IORING_OP_LINK_TIMEOUT,     /* 링크된 타임아웃 */
	IORING_OP_CONNECT,          /* connect */
	IORING_OP_FALLOCATE,        /* fallocate */
	IORING_OP_OPENAT,           /* openat */
	IORING_OP_CLOSE,            /* close */
	IORING_OP_FILES_UPDATE,     /* 등록된 파일 업데이트 */
	IORING_OP_STATX,            /* statx */
	IORING_OP_READ,             /* 단일 버퍼 읽기 */
	IORING_OP_WRITE,            /* 단일 버퍼 쓰기 */
	IORING_OP_FADVISE,          /* fadvise */
	IORING_OP_MADVISE,          /* madvise */
	IORING_OP_SEND,             /* send */
	IORING_OP_RECV,             /* recv */
	IORING_OP_OPENAT2,          /* openat2 */
	IORING_OP_EPOLL_CTL,        /* epoll_ctl */
	IORING_OP_SPLICE,           /* splice */
	IORING_OP_PROVIDE_BUFFERS,  /* 버퍼 풀에 버퍼 제공 */
	IORING_OP_REMOVE_BUFFERS,   /* 버퍼 풀에서 버퍼 제거 */
	IORING_OP_TEE,              /* tee */
	IORING_OP_SHUTDOWN,         /* shutdown */
	IORING_OP_RENAMEAT,         /* renameat */
	IORING_OP_UNLINKAT,         /* unlinkat */
	IORING_OP_MKDIRAT,          /* mkdirat */
	IORING_OP_SYMLINKAT,        /* symlinkat */
	IORING_OP_LINKAT,           /* linkat */
	IORING_OP_MSG_RING,         /* 다른 링에 메시지 전송 */
	IORING_OP_FSETXATTR,        /* fsetxattr */
	IORING_OP_SETXATTR,         /* setxattr */
	IORING_OP_FGETXATTR,        /* fgetxattr */
	IORING_OP_GETXATTR,         /* getxattr */
	IORING_OP_SOCKET,           /* socket 생성 */
	IORING_OP_URING_CMD,        /* 디바이스별 커맨드 (NVMe passthrough 등) */

	IORING_OP_LAST,             /* 마지막 opcode 마커 */
};

/*
 * sqe->uring_cmd_flags
 * IORING_URING_CMD_FIXED	use registered buffer; pass thig flag
 *				along with setting sqe->buf_index.
 */
#define IORING_URING_CMD_FIXED	(1U << 0)

/*
 * sqe->fsync_flags
 */
#define IORING_FSYNC_DATASYNC	(1U << 0)

/*
 * sqe->timeout_flags
 */
#define IORING_TIMEOUT_ABS		(1U << 0)
#define IORING_TIMEOUT_UPDATE		(1U << 1)
#define IORING_TIMEOUT_BOOTTIME		(1U << 2)
#define IORING_TIMEOUT_REALTIME		(1U << 3)
#define IORING_LINK_TIMEOUT_UPDATE	(1U << 4)
#define IORING_TIMEOUT_ETIME_SUCCESS	(1U << 5)
#define IORING_TIMEOUT_CLOCK_MASK	(IORING_TIMEOUT_BOOTTIME | IORING_TIMEOUT_REALTIME)
#define IORING_TIMEOUT_UPDATE_MASK	(IORING_TIMEOUT_UPDATE | IORING_LINK_TIMEOUT_UPDATE)
/*
 * sqe->splice_flags
 * extends splice(2) flags
 */
#define SPLICE_F_FD_IN_FIXED	(1U << 31) /* the last bit of __u32 */

/*
 * POLL_ADD flags. Note that since sqe->poll_events is the flag space, the
 * command flags for POLL_ADD are stored in sqe->len.
 *
 * IORING_POLL_ADD_MULTI	Multishot poll. Sets IORING_CQE_F_MORE if
 *				the poll handler will continue to report
 *				CQEs on behalf of the same SQE.
 *
 * IORING_POLL_UPDATE		Update existing poll request, matching
 *				sqe->addr as the old user_data field.
 */
#define IORING_POLL_ADD_MULTI	(1U << 0)
#define IORING_POLL_UPDATE_EVENTS	(1U << 1)
#define IORING_POLL_UPDATE_USER_DATA	(1U << 2)

#define IORING_NOP_INJECT_RESULT	(1U << 0)
#define IORING_NOP_FILE			(1U << 1)
#define IORING_NOP_FIXED_FILE		(1U << 2)
#define IORING_NOP_FIXED_BUFFER		(1U << 3)

/*
 * [한국어] CQE (Completion Queue Entry) - I/O 완료 결과를 담는 구조체
 * 커널이 I/O를 완료하면 CQ 링에 이 구조체를 추가함.
 * 크기: 기본 16바이트, IORING_SETUP_CQE32 사용 시 32바이트
 */
struct io_uring_cqe {
	__u64	user_data;	/* [한국어] SQE에서 설정한 user_data가 그대로 전달됨 (fio: io_u 포인터) */
	__s32	res;		/* [한국어] 결과 코드: 성공 시 전송 바이트 수, 실패 시 음수 에러코드 */
	__u32	flags;          /* [한국어] CQE 플래그 (IORING_CQE_F_BUFFER, IORING_CQE_F_MORE 등) */

	/*
	 * If the ring is initialized with IORING_SETUP_CQE32, then this field
	 * contains 16-bytes of padding, doubling the size of the CQE.
	 */
	__u64 big_cqe[];
};

/*
 * cqe->flags
 *
 * IORING_CQE_F_BUFFER	If set, the upper 16 bits are the buffer ID
 * IORING_CQE_F_MORE	If set, parent SQE will generate more CQE entries
 */
#define IORING_CQE_F_BUFFER		(1U << 0)
#define IORING_CQE_F_MORE		(1U << 1)

enum {
	IORING_CQE_BUFFER_SHIFT		= 16,
};

/*
 * [한국어] mmap 매직 오프셋
 * 애플리케이션이 mmap()으로 커널과 공유 메모리를 매핑할 때 사용하는 오프셋.
 * io_uring_setup() 반환 fd에 대해 mmap(fd, offset)으로 접근.
 */
#define IORING_OFF_SQ_RING		0ULL          /* SQ 링 메타데이터 (head, tail, flags 등) */
#define IORING_OFF_CQ_RING		0x8000000ULL  /* CQ 링 메타데이터 */
#define IORING_OFF_SQES			0x10000000ULL /* SQE 배열 */

/*
 * [한국어] SQ 링 오프셋 구조체
 * io_uring_params.sq_off에 채워져 반환됨.
 * 각 필드는 mmap된 SQ 링 메모리 내에서의 바이트 오프셋을 나타냄.
 * 애플리케이션은 이 오프셋으로 head, tail, flags 등에 접근.
 */
struct io_sqring_offsets {
	__u32 head;          /* [한국어] SQ head 오프셋 - 커널이 소비한 위치 (커널이 갱신) */
	__u32 tail;          /* [한국어] SQ tail 오프셋 - 유저가 추가한 위치 (유저가 갱신) */
	__u32 ring_mask;     /* [한국어] 링 마스크 (entries - 1, 인덱스 래핑용) */
	__u32 ring_entries;  /* [한국어] 링 엔트리 수 (2의 거듭제곱) */
	__u32 flags;         /* [한국어] SQ 플래그 오프셋 (IORING_SQ_NEED_WAKEUP 등) */
	__u32 dropped;       /* [한국어] 드롭된 SQE 수 오프셋 */
	__u32 array;         /* [한국어] SQ 인덱스 배열 오프셋 (SQE 인덱스를 간접 참조) */
	__u32 resv1;
	__u64 resv2;
};

/*
 * [한국어] SQ 링 플래그
 */
#define IORING_SQ_NEED_WAKEUP	(1U << 0) /* [한국어] SQPOLL 스레드가 sleep 중 → io_uring_enter()로 깨워야 함 */
#define IORING_SQ_CQ_OVERFLOW	(1U << 1) /* [한국어] CQ 링이 오버플로됨 → CQE를 빨리 소비해야 함 */

/* [한국어] CQ 링 오프셋 구조체 (io_uring_params.cq_off에 채워짐) */
struct io_cqring_offsets {
	__u32 head;          /* [한국어] CQ head 오프셋 - 유저가 소비한 위치 (유저가 갱신) */
	__u32 tail;          /* [한국어] CQ tail 오프셋 - 커널이 추가한 위치 (커널이 갱신) */
	__u32 ring_mask;     /* [한국어] 링 마스크 */
	__u32 ring_entries;  /* [한국어] 링 엔트리 수 */
	__u32 overflow;      /* [한국어] 오버플로 카운터 오프셋 */
	__u32 cqes;          /* [한국어] CQE 배열 오프셋 */
	__u32 flags;         /* [한국어] CQ 플래그 오프셋 */
	__u32 resv1;
	__u64 resv2;
};

/*
 * cq_ring->flags
 */

/* disable eventfd notifications */
#define IORING_CQ_EVENTFD_DISABLED	(1U << 0)

/*
 * [한국어] io_uring_enter() 플래그
 * SQ 제출과 CQ 이벤트 수확을 제어하는 플래그
 */
#define IORING_ENTER_GETEVENTS		(1U << 0)  /* [한국어] CQ에 완료 이벤트가 올 때까지 대기 */
#define IORING_ENTER_SQ_WAKEUP		(1U << 1)  /* [한국어] SQPOLL 스레드 깨우기 */
#define IORING_ENTER_SQ_WAIT		(1U << 2)  /* [한국어] SQ에 빈 슬롯이 생길 때까지 대기 */
#define IORING_ENTER_EXT_ARG		(1U << 3)  /* [한국어] 확장 인자 사용 (시그마스크, 타임아웃) */
#define IORING_ENTER_REGISTERED_RING	(1U << 4)  /* [한국어] 등록된 링 fd 사용 */
#define IORING_ENTER_NO_IOWAIT		(1U << 7)  /* [한국어] I/O 완료 대기 안 함 */

/*
 * [한국어] io_uring_setup() 파라미터 구조체
 * 입력: sq_entries, flags, sq_thread_cpu, sq_thread_idle, wq_fd
 * 출력: cq_entries(실제 할당), features(커널 기능), sq_off/cq_off(mmap 오프셋)
 * fio의 fio_ioring_queue_init()에서 이 구조체를 채워 io_uring_setup() 호출.
 */
struct io_uring_params {
	__u32 sq_entries;       /* [한국어] SQ 엔트리 수 (입력/출력) */
	__u32 cq_entries;       /* [한국어] CQ 엔트리 수 (IORING_SETUP_CQSIZE 시 입력, 아니면 출력) */
	__u32 flags;            /* [한국어] IORING_SETUP_* 플래그 조합 */
	__u32 sq_thread_cpu;    /* [한국어] SQPOLL 스레드를 바인딩할 CPU (SQ_AFF 시) */
	__u32 sq_thread_idle;   /* [한국어] SQPOLL 스레드 유휴 타임아웃 (밀리초) */
	__u32 features;         /* [한국어] 커널이 지원하는 기능 비트마스크 (IORING_FEAT_*) */
	__u32 wq_fd;            /* [한국어] ATTACH_WQ용 기존 링 fd */
	__u32 resv[3];
	struct io_sqring_offsets sq_off;  /* [한국어] SQ 링 mmap 오프셋 정보 */
	struct io_cqring_offsets cq_off;  /* [한국어] CQ 링 mmap 오프셋 정보 */
};

/*
 * [한국어] io_uring_params->features 플래그
 * 커널이 io_uring_setup() 응답에서 설정하여 지원하는 기능을 알려줌.
 * 애플리케이션은 이 플래그를 확인하여 기능 사용 가능 여부를 판단.
 */
#define IORING_FEAT_SINGLE_MMAP		(1U << 0)  /* SQ/CQ를 하나의 mmap으로 매핑 가능 */
#define IORING_FEAT_NODROP		(1U << 1)  /* CQ 오버플로 시에도 CQE를 드롭하지 않음 */
#define IORING_FEAT_SUBMIT_STABLE	(1U << 2)  /* 제출 후 SQE 데이터 수정 안전 */
#define IORING_FEAT_RW_CUR_POS		(1U << 3)  /* off=-1로 현재 파일 위치 사용 가능 */
#define IORING_FEAT_CUR_PERSONALITY	(1U << 4)  /* 현재 프로세스 자격증명 사용 */
#define IORING_FEAT_FAST_POLL		(1U << 5)  /* 내부 폴링 최적화 지원 */
#define IORING_FEAT_POLL_32BITS 	(1U << 6)  /* 32비트 폴 이벤트 지원 */
#define IORING_FEAT_SQPOLL_NONFIXED	(1U << 7)  /* SQPOLL에서 비고정 파일 사용 가능 */
#define IORING_FEAT_EXT_ARG		(1U << 8)  /* io_uring_enter 확장 인자 지원 */
#define IORING_FEAT_NATIVE_WORKERS	(1U << 9)  /* 네이티브 워커 스레드 사용 */
#define IORING_FEAT_RSRC_TAGS		(1U << 10) /* 리소스 태그 지원 */
#define IORING_FEAT_CQE_SKIP		(1U << 11) /* CQE 스킵 기능 지원 */
#define IORING_FEAT_NO_IOWAIT		(1U << 17) /* NO_IOWAIT 플래그 지원 */

/*
 * [한국어] io_uring_register() opcode
 * 버퍼, 파일, 이벤트fd 등을 커널에 사전 등록하여 매 I/O마다의 오버헤드를 제거.
 * fio에서 fixedbufs 옵션 → REGISTER_BUFFERS, registerfiles → REGISTER_FILES 사용.
 */
enum {
	IORING_REGISTER_BUFFERS			= 0,   /* 버퍼 등록 (pinning 비용 제거) */
	IORING_UNREGISTER_BUFFERS		= 1,   /* 버퍼 등록 해제 */
	IORING_REGISTER_FILES			= 2,   /* 파일 등록 (fd lookup 비용 제거) */
	IORING_UNREGISTER_FILES			= 3,   /* 파일 등록 해제 */
	IORING_REGISTER_EVENTFD			= 4,   /* eventfd 등록 (완료 통지용) */
	IORING_UNREGISTER_EVENTFD		= 5,   /* eventfd 해제 */
	IORING_REGISTER_FILES_UPDATE		= 6,   /* 등록된 파일 부분 업데이트 */
	IORING_REGISTER_EVENTFD_ASYNC		= 7,   /* 비동기 eventfd 등록 */
	IORING_REGISTER_PROBE			= 8,   /* 지원 opcode 조회 */
	IORING_REGISTER_PERSONALITY		= 9,   /* 자격증명 등록 */
	IORING_UNREGISTER_PERSONALITY		= 10,  /* 자격증명 해제 */
	IORING_REGISTER_RESTRICTIONS		= 11,  /* 링 제한 설정 */
	IORING_REGISTER_ENABLE_RINGS		= 12,  /* 비활성 링 활성화 */

	/* [한국어] 태깅 지원 확장 버전 */
	IORING_REGISTER_FILES2			= 13,
	IORING_REGISTER_FILES_UPDATE2		= 14,
	IORING_REGISTER_BUFFERS2		= 15,
	IORING_REGISTER_BUFFERS_UPDATE		= 16,

	/* [한국어] io-wq 워커 스레드 CPU 친화성 설정 */
	IORING_REGISTER_IOWQ_AFF		= 17,
	IORING_UNREGISTER_IOWQ_AFF		= 18,

	/* [한국어] io-wq 최대 워커 스레드 수 설정 */
	IORING_REGISTER_IOWQ_MAX_WORKERS	= 19,

	/* [한국어] 링 fd 자체를 등록 (REGISTERED_RING 사용) */
	IORING_REGISTER_RING_FDS		= 20,
	IORING_UNREGISTER_RING_FDS		= 21,

	IORING_REGISTER_LAST
};

/* [한국어] io-wq 워커 카테고리
 * BOUND: 특정 CPU에 바인딩된 워커
 * UNBOUND: CPU 바인딩 없는 워커 (블로킹 I/O 처리) */
enum {
	IO_WQ_BOUND,
	IO_WQ_UNBOUND,
};

/* [한국어] (deprecated) 등록된 파일 업데이트 구조체. io_uring_rsrc_update 사용 권장 */
struct io_uring_files_update {
	__u32 offset;
	__u32 resv;
	__aligned_u64 /* __s32 * */ fds;
};

/* [한국어] 리소스(버퍼/파일) 일괄 등록 구조체 */
struct io_uring_rsrc_register {
	__u32 nr;
	__u32 resv;
	__u64 resv2;
	__aligned_u64 data;
	__aligned_u64 tags;
};

struct io_uring_rsrc_update {
	__u32 offset;
	__u32 resv;
	__aligned_u64 data;
};

struct io_uring_rsrc_update2 {
	__u32 offset;
	__u32 resv;
	__aligned_u64 data;
	__aligned_u64 tags;
	__u32 nr;
	__u32 resv2;
};

/* Skip updating fd indexes set to this value in the fd table */
#define IORING_REGISTER_FILES_SKIP	(-2)

#define IO_URING_OP_SUPPORTED	(1U << 0)

/* [한국어] opcode 지원 여부 정보 (IORING_REGISTER_PROBE 응답의 배열 항목) */
struct io_uring_probe_op {
	__u8 op;        /* opcode 번호 */
	__u8 resv;
	__u16 flags;	/* IO_URING_OP_SUPPORTED이면 해당 opcode 지원 */
	__u32 resv2;
};

/* [한국어] io_uring probe 응답 - 커널이 지원하는 opcode 목록을 조회
 * fio에서 IORING_OP_URING_CMD 지원 여부를 확인하는 데 사용 */
struct io_uring_probe {
	__u8 last_op;	/* 지원되는 마지막 opcode 번호 */
	__u8 ops_len;	/* ops[] 배열 길이 */
	__u16 resv;
	__u32 resv2[3];
	struct io_uring_probe_op ops[0];  /* 가변 길이 opcode 지원 정보 배열 */
};

/* [한국어] 링 제한 설정 구조체 - 특정 opcode/플래그만 허용하도록 제한
 * 보안을 위해 신뢰할 수 없는 코드에 제한된 링을 전달할 때 사용 */
struct io_uring_restriction {
	__u16 opcode;     /* 제한 유형 (IORING_RESTRICTION_*) */
	union {
		__u8 register_op; /* 허용할 register opcode */
		__u8 sqe_op;      /* 허용할 SQE opcode */
		__u8 sqe_flags;   /* 허용/필수 SQE 플래그 */
	};
	__u8 resv;
	__u32 resv2[3];
};

/*
 * io_uring_restriction->opcode values
 */
enum {
	/* Allow an io_uring_register(2) opcode */
	IORING_RESTRICTION_REGISTER_OP		= 0,

	/* Allow an sqe opcode */
	IORING_RESTRICTION_SQE_OP		= 1,

	/* Allow sqe flags */
	IORING_RESTRICTION_SQE_FLAGS_ALLOWED	= 2,

	/* Require sqe flags (these flags must be set on each submission) */
	IORING_RESTRICTION_SQE_FLAGS_REQUIRED	= 3,

	IORING_RESTRICTION_LAST
};

/*
 * [한국어] io_uring_enter() 확장 인자 (IORING_ENTER_EXT_ARG 사용 시)
 * 시그널 마스크와 타임아웃을 함께 전달하여
 * CQ 이벤트 대기 시 시그널 처리와 타임아웃을 동시에 제어.
 */
struct io_uring_getevents_arg {
	__u64	sigmask;     /* [한국어] 시그널 마스크 포인터 */
	__u32	sigmask_sz;  /* [한국어] 시그널 마스크 크기 */
	__u32	pad;
	__u64	ts;          /* [한국어] 타임아웃 (struct __kernel_timespec 포인터) */
};

#endif
