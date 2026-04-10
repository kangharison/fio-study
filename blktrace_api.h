/*
 * [한국어 설명] blktrace API 헤더 파일 (blktrace_api.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 커널의 blktrace(블록 I/O 추적) 인프라에서 사용하는
 * 데이터 구조체와 상수를 정의한다. fio는 blktrace 로그를 재생(replay)하여
 * 실제 워크로드를 재현하는 기능을 제공하며, 이 헤더가 그 인터페이스를 정의한다.
 *
 * === blktrace란? ===
 * blktrace는 리눅스 커널의 블록 I/O 계층에서 발생하는 모든 이벤트를 추적하는 도구이다.
 * 각 I/O 요청이 큐잉 → 병합 → 디스패치 → 완료되는 전 과정을 기록한다.
 * fio는 이 추적 데이터를 읽어 동일한 I/O 패턴을 재현(replay)할 수 있다.
 *
 * === fio에서의 사용 ===
 * - blktrace.c에서 blk_io_trace 구조체를 파싱하여 I/O 요청을 재구성
 * - --read_iolog=<blktrace파일> 옵션으로 blktrace 로그를 재생
 * - 트레이스 카테고리(BLK_TC_*)와 액션(BLK_TA_*)으로 이벤트 유형을 식별
 */

#ifndef BLKTRACEAPI_H
#define BLKTRACEAPI_H

#include <asm/types.h>    /* __u32, __u64 등 커널 스타일 고정 크기 타입 */

/*
 * Trace categories
 * [한국어] 트레이스 카테고리 비트마스크
 * 각 비트는 블록 I/O 이벤트의 종류를 나타낸다.
 * blk_io_trace.action 필드의 상위 16비트에 카테고리 정보가 인코딩된다.
 */
enum {
	BLK_TC_READ	= 1 << 0,	/* 읽기 요청 */
	BLK_TC_WRITE	= 1 << 1,	/* 쓰기 요청 */
	BLK_TC_FLUSH	= 1 << 2,	/* 캐시 플러시 (디스크 캐시를 영구 저장소에 기록) */
	BLK_TC_SYNC	= 1 << 3,	/* 동기 I/O (완료 보장 요청) */
	BLK_TC_QUEUE	= 1 << 4,	/* 큐잉/병합 이벤트 (I/O 스케줄러 진입) */
	BLK_TC_REQUEUE	= 1 << 5,	/* 재큐잉 (드라이버가 요청을 거부하여 다시 큐에 넣음) */
	BLK_TC_ISSUE	= 1 << 6,	/* 디스패치 (드라이버에 전달됨) */
	BLK_TC_COMPLETE	= 1 << 7,	/* 완료 (드라이버가 완료를 보고) */
	BLK_TC_FS	= 1 << 8,	/* 파일시스템 요청 */
	BLK_TC_PC	= 1 << 9,	/* SCSI passthrough 요청 (커널 우회 직접 명령) */
	BLK_TC_NOTIFY	= 1 << 10,	/* 알림 메시지 (프로세스 정보, 타임스탬프 등) */
	BLK_TC_AHEAD	= 1 << 11,	/* 미리 읽기(readahead) 요청 */
	BLK_TC_META	= 1 << 12,	/* 메타데이터 I/O (파일시스템 메타데이터) */
	BLK_TC_DISCARD	= 1 << 13,	/* TRIM/DISCARD 요청 (블록 해제) */
	BLK_TC_DRV_DATA	= 1 << 14,	/* 드라이버별 바이너리 데이터 */

	BLK_TC_END	= 1 << 15,	/* 카테고리 비트 상한 (16비트까지만 사용) */
};

/*
 * [한국어] BLK_TC_SHIFT: 카테고리 비트를 action 필드의 상위 16비트로 이동시키는 시프트 값
 * BLK_TC_ACT: 카테고리 값을 action 필드의 상위 비트 위치로 시프트하는 매크로
 * action 필드 구조: [상위 16비트: 카테고리] | [하위 16비트: 기본 액션]
 */
#define BLK_TC_SHIFT		(16)
#define BLK_TC_ACT(act)		((act) << BLK_TC_SHIFT)

/*
 * Basic trace actions
 * [한국어] 기본 트레이스 액션 - 블록 I/O 요청의 생명주기 각 단계를 나타낸다.
 *
 * 일반적인 I/O 요청의 흐름:
 *   QUEUE → (BACKMERGE/FRONTMERGE 또는 GETRQ) → INSERT → ISSUE → COMPLETE
 *
 *   1. QUEUE: 요청이 I/O 스케줄러 큐에 진입
 *   2. BACKMERGE/FRONTMERGE: 기존 요청과 병합됨 (최적화)
 *      또는 GETRQ: 새로운 요청 구조체 할당
 *   3. INSERT: 요청이 디스패치 큐에 삽입
 *   4. ISSUE: 드라이버에 전달 (실제 디스크 I/O 시작)
 *   5. COMPLETE: 드라이버가 완료를 보고
 */
enum {
	__BLK_TA_QUEUE = 1,		/* 큐에 진입 - I/O 스케줄러가 요청을 수락 */
	__BLK_TA_BACKMERGE,		/* 기존 요청의 뒤에 병합 (연속된 영역 합침) */
	__BLK_TA_FRONTMERGE,		/* 기존 요청의 앞에 병합 (연속된 영역 합침) */
	__BLK_TA_GETRQ,			/* 새 요청(request) 구조체 할당 성공 */
	__BLK_TA_SLEEPRQ,		/* 요청 할당 대기로 슬립 (메모리 부족 등) */
	__BLK_TA_REQUEUE,		/* 드라이버가 거부하여 재큐잉 */
	__BLK_TA_ISSUE,			/* 드라이버에 전달됨 (실제 디스크 I/O 시작) */
	__BLK_TA_COMPLETE,		/* 드라이버가 완료를 보고 */
	__BLK_TA_PLUG,			/* 요청 큐가 플러그됨 (I/O 병합을 위해 일시 보류) */
	__BLK_TA_UNPLUG_IO,		/* I/O에 의해 큐가 언플러그됨 (보류된 I/O 일괄 제출) */
	__BLK_TA_UNPLUG_TIMER,		/* 타이머에 의해 큐가 언플러그됨 */
	__BLK_TA_INSERT,		/* 디스패치 큐에 요청 삽입 */
	__BLK_TA_SPLIT,			/* bio가 분할됨 (크기 제한 초과) */
	__BLK_TA_BOUNCE,		/* bio가 바운스됨 (DMA 주소 범위 초과로 복사) */
	__BLK_TA_REMAP,			/* bio가 리맵됨 (device mapper 등에서 주소 변환) */
	__BLK_TA_ABORT,			/* 요청 중단 */
	__BLK_TA_DRV_DATA,		/* 드라이버별 바이너리 데이터 */
};

/*
 * Notify events.
 * [한국어] 알림 이벤트 - 트레이스 스트림에 메타정보를 삽입할 때 사용
 */
enum blktrace_notify {
	__BLK_TN_PROCESS = 0,		/* PID ↔ 프로세스 이름 매핑 정보 */
	__BLK_TN_TIMESTAMP,		/* 시스템 클록 타임스탬프 포함 */
	__BLK_TN_MESSAGE,		/* 문자열 메시지 (사용자 정의 마커 등) */
};

/*
 * Trace actions in full. Additionally, read or write is masked
 * [한국어] 완전한 트레이스 액션 매크로 - 기본 액션과 카테고리를 결합한 값
 * 이 매크로들은 blk_io_trace.action 필드에 저장되는 실제 값을 생성한다.
 * 구조: [하위 비트: 기본 액션] | [상위 비트: 카테고리]
 * 추가로 BLK_TC_READ/BLK_TC_WRITE가 OR되어 읽기/쓰기 방향도 인코딩된다.
 */
#define BLK_TA_QUEUE		(__BLK_TA_QUEUE | BLK_TC_ACT(BLK_TC_QUEUE))      /* 큐 진입 */
#define BLK_TA_BACKMERGE	(__BLK_TA_BACKMERGE | BLK_TC_ACT(BLK_TC_QUEUE))  /* 후방 병합 */
#define BLK_TA_FRONTMERGE	(__BLK_TA_FRONTMERGE | BLK_TC_ACT(BLK_TC_QUEUE)) /* 전방 병합 */
#define	BLK_TA_GETRQ		(__BLK_TA_GETRQ | BLK_TC_ACT(BLK_TC_QUEUE))      /* 요청 할당 */
#define	BLK_TA_SLEEPRQ		(__BLK_TA_SLEEPRQ | BLK_TC_ACT(BLK_TC_QUEUE))    /* 할당 대기 슬립 */
#define	BLK_TA_REQUEUE		(__BLK_TA_REQUEUE | BLK_TC_ACT(BLK_TC_REQUEUE))  /* 재큐잉 */
#define BLK_TA_ISSUE		(__BLK_TA_ISSUE | BLK_TC_ACT(BLK_TC_ISSUE))      /* 드라이버 전달 */
#define BLK_TA_COMPLETE		(__BLK_TA_COMPLETE| BLK_TC_ACT(BLK_TC_COMPLETE)) /* 완료 */
#define BLK_TA_PLUG		(__BLK_TA_PLUG | BLK_TC_ACT(BLK_TC_QUEUE))       /* 큐 플러그 */
#define BLK_TA_UNPLUG_IO	(__BLK_TA_UNPLUG_IO | BLK_TC_ACT(BLK_TC_QUEUE))  /* I/O 언플러그 */
#define BLK_TA_UNPLUG_TIMER	(__BLK_TA_UNPLUG_TIMER | BLK_TC_ACT(BLK_TC_QUEUE)) /* 타이머 언플러그 */
#define BLK_TA_INSERT		(__BLK_TA_INSERT | BLK_TC_ACT(BLK_TC_QUEUE))     /* 디스패치 큐 삽입 */
#define BLK_TA_SPLIT		(__BLK_TA_SPLIT)                                  /* bio 분할 */
#define BLK_TA_BOUNCE		(__BLK_TA_BOUNCE)                                 /* bio 바운스 */
#define BLK_TA_REMAP		(__BLK_TA_REMAP | BLK_TC_ACT(BLK_TC_QUEUE))      /* bio 리맵 */
#define BLK_TA_DRV_DATA (__BLK_TA_DRV_DATA | BLK_TC_ACT(BLK_TC_DRV_DATA))   /* 드라이버 데이터 */

/* [한국어] 알림 이벤트 매크로 - 카테고리 BLK_TC_NOTIFY와 결합 */
#define BLK_TN_PROCESS		(__BLK_TN_PROCESS | BLK_TC_ACT(BLK_TC_NOTIFY))   /* 프로세스 매핑 */
#define BLK_TN_TIMESTAMP	(__BLK_TN_TIMESTAMP | BLK_TC_ACT(BLK_TC_NOTIFY)) /* 타임스탬프 */
#define BLK_TN_MESSAGE          (__BLK_TN_MESSAGE | BLK_TC_ACT(BLK_TC_NOTIFY))  /* 메시지 */

/*
 * [한국어] 트레이스 매직 넘버와 버전
 * 트레이스 데이터의 유효성을 검증하고 형식 버전을 식별하는 데 사용된다.
 * magic 필드: (BLK_IO_TRACE_MAGIC | BLK_IO_TRACE_VERSION) 형태로 저장
 */
#define BLK_IO_TRACE_MAGIC	0x65617400    /* "eat\0"의 ASCII 값 */
#define BLK_IO_TRACE_VERSION	0x07          /* 현재 트레이스 형식 버전 7 */

/*
 * The trace itself
 * [한국어] blk_io_trace - 하나의 블록 I/O 트레이스 이벤트를 나타내는 핵심 구조체
 *
 * blktrace가 기록하는 각 이벤트는 이 구조체 형태로 저장된다.
 * fio의 blktrace.c에서 이 구조체를 파싱하여 I/O 패턴을 재구성한다.
 * 구조체 뒤에 pdu_len 바이트만큼의 추가 데이터가 올 수 있다.
 */
struct blk_io_trace {
	__u32 magic;		/* 매직 넘버 | 버전 (유효성 검증용) */
	__u32 sequence;		/* 이벤트 시퀀스 번호 (이벤트 순서 보장) */
	__u64 time;		/* 타임스탬프 (나노초 단위) */
	__u64 sector;		/* 디스크 오프셋 (섹터 단위, 보통 512바이트) */
	__u32 bytes;		/* 전송 길이 (바이트 단위) */
	__u32 action;		/* 발생한 이벤트 (BLK_TA_* | BLK_TC_READ/WRITE) */
	__u32 pid;		/* I/O를 발생시킨 프로세스 ID */
	__u32 device;		/* 디바이스 식별자 (dev_t: major/minor 번호) */
	__u32 cpu;		/* 이벤트가 발생한 CPU 번호 */
	__u16 error;		/* 완료 에러 코드 (0이면 성공) */
	__u16 pdu_len;		/* 이 트레이스 뒤에 오는 추가 데이터 길이 */
};

/*
 * The remap event
 * [한국어] blk_io_trace_remap - 리맵 이벤트의 추가 데이터 구조체
 * device mapper, RAID 등에서 I/O가 다른 디바이스로 리맵될 때의 정보를 저장한다.
 * blk_io_trace 뒤에 pdu(Protocol Data Unit)로 부착된다.
 */
struct blk_io_trace_remap {
	__u32 device;		/* 리맵 대상 디바이스 */
	__u32 device_from;	/* 원래 디바이스 */
	__u64 sector;		/* 리맵된 섹터 위치 */
};

/*
 * User setup structure passed with BLKSTARTTRACE
 * [한국어] blk_user_trace_setup - 사용자 공간에서 트레이스를 시작할 때 전달하는 설정 구조체
 * BLKSTARTTRACE ioctl에 이 구조체를 전달하여 트레이스 파라미터를 설정한다.
 */
struct blk_user_trace_setup {
	char name[32];			/* 출력 파일 이름 (커널이 채움) */
	__u16 act_mask;			/* 추적할 액션 마스크 (BLK_TC_* 조합) */
	__u32 buf_size;			/* per-CPU 트레이스 버퍼 크기 */
	__u32 buf_nr;			/* per-CPU 트레이스 버퍼 개수 */
	__u64 start_lba;		/* 추적 시작 LBA (논리 블록 주소) */
	__u64 end_lba;			/* 추적 종료 LBA */
	__u32 pid;			/* 추적 대상 프로세스 ID (0이면 전체) */
};

#endif
