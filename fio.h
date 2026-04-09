#ifndef FIO_H
#define FIO_H

/*
 * fio.h - fio (Flexible I/O Tester)의 메인 헤더 파일
 *
 * [한국어 개요]
 * 이 파일은 fio의 핵심 데이터 구조와 함수 선언을 포함합니다.
 * 가장 중요한 구조체는 struct thread_data로, 하나의 I/O 작업(job)을 실행하는
 * 스레드/프로세스의 전체 상태를 담고 있습니다.
 *
 * 핵심 구조체 관계:
 *   thread_data (이 파일) - 스레드의 런타임 상태 전체
 *     ├── thread_options (thread_options.h) - 사용자가 설정한 옵션 값들 (.o 필드)
 *     ├── io_u (io_u.h) - 개별 I/O 요청 단위 (io_u_freelist, io_u_all 등으로 관리)
 *     ├── fio_file (file.h) - I/O 대상 파일 정보 (files[] 배열)
 *     ├── thread_stat (stat.h) - 통계 수집 결과 (.ts 필드)
 *     └── ioengine_ops (ioengines.h) - I/O 엔진 인터페이스 (.io_ops 필드)
 *
 * thread_data vs thread_options:
 *   - thread_options: 사용자가 job 파일이나 커맨드라인으로 지정한 "설정값"
 *     (블록 크기, 큐 깊이, 파일 경로, 읽기/쓰기 비율 등)
 *   - thread_data: thread_options를 포함하며, 런타임에 변하는 "상태값"도 관리
 *     (현재 완료된 I/O 수, 에러 상태, 난수 생성기 상태, 통계 등)
 *
 * thread_data와 io_u의 관계:
 *   - thread_data는 io_u 풀(pool)을 관리합니다 (io_u_freelist, io_u_all)
 *   - I/O를 발행할 때 io_u_freelist에서 io_u를 꺼내 사용하고,
 *     완료되면 다시 freelist로 반환합니다.
 *   - io_u는 하나의 I/O 연산(read/write/trim)의 오프셋, 크기, 버퍼, 방향 등을 담습니다.
 */

/* 표준 라이브러리 헤더 */
#include <sched.h>        /* CPU 스케줄링 관련 (sched_setaffinity 등) */
#include <limits.h>       /* 정수형 최대/최소값 상수 */
#include <pthread.h>      /* POSIX 스레드 (mutex, cond, thread 등) */
#include <sys/time.h>     /* 시간 관련 구조체 (timeval 등) */
#include <sys/resource.h> /* 리소스 사용량 (rusage 구조체) */
#include <errno.h>        /* 에러 번호 정의 */
#include <stdlib.h>       /* 표준 라이브러리 (malloc, free 등) */
#include <stdio.h>        /* 표준 입출력 */
#include <unistd.h>       /* POSIX 시스템 호출 (read, write, close 등) */
#include <string.h>       /* 문자열 처리 함수 */
#include <inttypes.h>     /* 정수형 포맷 매크로 (PRIu64 등) */
#include <assert.h>       /* 디버그 assertion */

/* fio 내부 헤더 파일들 */
#include "compiler/compiler.h"   /* 컴파일러별 매크로 (__must_check 등) */
#include "thread_options.h"      /* struct thread_options - 사용자 설정 옵션 구조체 */
#include "flist.h"               /* fio 연결 리스트 (Linux 커널 스타일 list_head) */
#include "fifo.h"                /* FIFO 큐 구현 */
#include "arch/arch.h"           /* 아키텍처별 코드 (x86, arm 등) */
#include "os/os.h"               /* OS별 추상화 계층 */
#include "log.h"                 /* 로깅 함수 */
#include "debug.h"               /* 디버그 유틸리티 */
#include "file.h"                /* struct fio_file - 파일 관리 구조체 */
#include "io_ddir.h"             /* I/O 방향 정의 (DDIR_READ, DDIR_WRITE, DDIR_TRIM) */
#include "ioengines.h"           /* I/O 엔진 인터페이스 (libaio, io_uring 등) */
#include "iolog.h"               /* I/O 로그 기록/재생 */
#include "helpers.h"             /* 유틸리티 헬퍼 함수 */
#include "minmax.h"              /* min/max 매크로 */
#include "options.h"             /* 옵션 파싱 관련 */
#include "profile.h"             /* 프로파일 지원 (tiobench 등 사전정의 워크로드) */
#include "fio_time.h"            /* fio 시간 관련 유틸리티 */
#include "gettime.h"             /* 고해상도 시간 측정 */
#include "oslib/getopt.h"        /* 커맨드라인 옵션 파싱 */
#include "lib/rand.h"            /* 난수 생성기 (Tausworthe, LFSR 등) */
#include "lib/rbtree.h"          /* Red-Black 트리 (정렬된 I/O 이력 관리) */
#include "lib/num2str.h"         /* 숫자-문자열 변환 유틸리티 */
#include "lib/memalign.h"        /* 정렬된 메모리 할당 */
#include "smalloc.h"             /* 공유 메모리 할당자 (프로세스 간 공유용) */
#include "client.h"              /* 클라이언트-서버 모드의 클라이언트 측 */
#include "server.h"              /* 클라이언트-서버 모드의 서버 측 */
#include "stat.h"                /* 통계 수집 및 출력 (struct thread_stat) */
#include "flow.h"                /* 흐름 제어 (여러 job 간 I/O 속도 조절) */
#include "io_u.h"                /* struct io_u - 개별 I/O 요청 단위 구조체 */
#include "io_u_queue.h"          /* io_u 큐/링 자료구조 */
#include "workqueue.h"           /* 작업 큐 (offload 모드에서 I/O 제출용) */
#include "steadystate.h"         /* 정상 상태 감지 (성능이 안정되면 종료) */
#include "lib/nowarn_snprintf.h" /* 경고 없는 snprintf 래퍼 */
#include "dedupe.h"              /* 중복 제거(dedupe) 시뮬레이션 */

/* Solaris 비동기 I/O 지원 */
#ifdef CONFIG_SOLARISAIO
#include <sys/asynch.h>
#endif

/* NUMA (Non-Uniform Memory Access) 지원 */
/* 메모리 접근 지역성을 위해 특정 NUMA 노드에 메모리를 바인딩할 수 있음 */
#ifdef CONFIG_LIBNUMA
#include <linux/mempolicy.h>
#include <numa.h>

/*
 * "local" is pseudo-policy
 */
/* "local"은 의사 정책으로, 현재 CPU가 속한 노드에 메모리를 할당하는 정책 */
#ifndef MPOL_LOCAL
#define MPOL_LOCAL 4
#endif
#endif

/* CUDA GPU 메모리 지원 (GPU Direct Storage 등에 사용) */
#ifdef CONFIG_CUDA
#include <cuda.h>
#endif

/* 전방 선언: fio 세마포어 구조체 */
struct fio_sem;

/* TRIM 명령에서 사용할 수 있는 최대 범위 수 */
#define MAX_TRIM_RANGE	256

/*
 * Range for trim command
 */
/* TRIM 명령의 범위를 나타내는 구조체 */
/* SSD의 TRIM/UNMAP/DEALLOCATE 명령에서 삭제할 영역을 지정 */
struct trim_range {
	unsigned long long start; /* TRIM 시작 오프셋 (바이트 단위) */
	unsigned long long len;   /* TRIM 길이 (바이트 단위) */
};

/*
 * offset generator types
 */
/* 오프셋 생성기 유형 */
/* 순차 I/O에서 다음 오프셋을 어떻게 결정할지 제어 */
enum {
	RW_SEQ_SEQ	= 0, /* 순차적으로 증가하는 오프셋 (기본값) */
	RW_SEQ_IDENT,    /* 동일한 오프셋을 반복 사용 */
};

/*
 * thread_data 플래그의 비트 위치(bit position) 정의
 * 아래의 TD_F_* 매크로에서 실제 비트마스크 값으로 변환됨
 */
enum {
	__TD_F_VER_BACKLOG	= 0, /* 검증(verify) 백로그가 있음 */
	__TD_F_TRIM_BACKLOG,         /* TRIM 백로그가 있음 */
	__TD_F_READ_IOLOG,           /* I/O 로그 파일에서 읽는 중 */
	__TD_F_REFILL_BUFFERS,       /* 매 I/O마다 버퍼를 다시 채움 */
	__TD_F_SCRAMBLE_BUFFERS,     /* 버퍼 내용을 섞어서 압축 방지 */
	__TD_F_DO_VERIFY,            /* 검증(verify) 단계를 수행해야 함 */
	__TD_F_PROFILE_OPS,          /* 프로파일별 커스텀 I/O 연산 사용 */
	__TD_F_COMPRESS,             /* 버퍼 데이터에 압축 패턴 적용 */
	__TD_F_COMPRESS_LOG,         /* I/O 로그를 압축하여 저장 */
	__TD_F_VSTATE_SAVED,         /* 검증 상태가 저장됨 */
	__TD_F_NEED_LOCK,            /* io_u 접근 시 뮤텍스 잠금 필요 (verify 스레드 동시 접근) */
	__TD_F_CHILD,                /* 이 스레드가 자식(fork된) 프로세스임 */
	__TD_F_NO_PROGRESS,          /* I/O 진행이 없는 상태 */
	__TD_F_REGROW_LOGS,          /* 로그 크기를 다시 증가시켜야 함 */
	__TD_F_MMAP_KEEP,            /* mmap 매핑을 유지함 */
	__TD_F_DIRS_CREATED,         /* 필요한 디렉토리가 이미 생성됨 */
	__TD_F_CHECK_RATE,           /* I/O 속도 제한 체크가 필요함 */
	__TD_F_SYNCS,                /* sync 연산이 발생함 */
	__TD_F_LAST,		/* not a real bit, keep last */
	                         /* 실제 비트가 아닌 마지막 위치 표시용 */
};

/*
 * thread_data 플래그의 실제 비트마스크 값
 * td->flags에 OR 연산으로 설정하고, AND 연산으로 확인
 */
enum {
	TD_F_VER_BACKLOG	= 1U << __TD_F_VER_BACKLOG,    /* 검증 백로그 플래그 */
	TD_F_TRIM_BACKLOG	= 1U << __TD_F_TRIM_BACKLOG,   /* TRIM 백로그 플래그 */
	TD_F_READ_IOLOG		= 1U << __TD_F_READ_IOLOG,     /* I/O 로그 읽기 플래그 */
	TD_F_REFILL_BUFFERS	= 1U << __TD_F_REFILL_BUFFERS, /* 버퍼 재충전 플래그 */
	TD_F_SCRAMBLE_BUFFERS	= 1U << __TD_F_SCRAMBLE_BUFFERS, /* 버퍼 스크램블 플래그 */
	TD_F_DO_VERIFY		= 1U << __TD_F_DO_VERIFY,      /* 검증 수행 플래그 */
	TD_F_PROFILE_OPS	= 1U << __TD_F_PROFILE_OPS,    /* 프로파일 연산 플래그 */
	TD_F_COMPRESS		= 1U << __TD_F_COMPRESS,       /* 압축 패턴 플래그 */
	TD_F_COMPRESS_LOG	= 1U << __TD_F_COMPRESS_LOG,   /* 로그 압축 플래그 */
	TD_F_VSTATE_SAVED	= 1U << __TD_F_VSTATE_SAVED,   /* 검증 상태 저장됨 플래그 */
	TD_F_NEED_LOCK		= 1U << __TD_F_NEED_LOCK,      /* 잠금 필요 플래그 */
	TD_F_CHILD		= 1U << __TD_F_CHILD,              /* 자식 프로세스 플래그 */
	TD_F_NO_PROGRESS        = 1U << __TD_F_NO_PROGRESS,   /* 진행 없음 플래그 */
	TD_F_REGROW_LOGS	= 1U << __TD_F_REGROW_LOGS,    /* 로그 재성장 플래그 */
	TD_F_MMAP_KEEP		= 1U << __TD_F_MMAP_KEEP,     /* mmap 유지 플래그 */
	TD_F_DIRS_CREATED	= 1U << __TD_F_DIRS_CREATED,   /* 디렉토리 생성 완료 플래그 */
	TD_F_CHECK_RATE		= 1U << __TD_F_CHECK_RATE,     /* 속도 체크 플래그 */
	TD_F_SYNCS		= 1U << __TD_F_SYNCS,              /* sync 발생 플래그 */
};

/*
 * 난수 생성기(PRNG) 시드 오프셋 인덱스
 * fio는 각 용도별로 독립적인 난수 생성기를 사용하여
 * 재현 가능한(reproducible) I/O 패턴을 보장합니다.
 * rand_seeds[] 배열에서 각 용도별 시드의 인덱스를 정의합니다.
 */
enum {
	FIO_RAND_BS_OFF		= 0, /* 블록 크기 랜덤화 시드 (DDIR_READ) */
	FIO_RAND_BS1_OFF,           /* 블록 크기 랜덤화 시드 (DDIR_WRITE) */
	FIO_RAND_BS2_OFF,           /* 블록 크기 랜덤화 시드 (DDIR_TRIM) */
	FIO_RAND_VER_OFF,           /* 검증(verify) 데이터 생성 시드 */
	FIO_RAND_MIX_OFF,           /* 읽기/쓰기 혼합(rwmix) 비율 시드 */
	FIO_RAND_FILE_OFF,          /* 파일 선택 랜덤화 시드 */
	FIO_RAND_BLOCK_OFF,         /* 블록 오프셋 랜덤화 시드 */
	FIO_RAND_FILE_SIZE_OFF,     /* 파일 크기 랜덤화 시드 */
	FIO_RAND_TRIM_OFF,          /* TRIM 오프셋 랜덤화 시드 */
	FIO_RAND_BUF_OFF,           /* 버퍼 내용 랜덤화 시드 */
	FIO_RAND_SEQ_RAND_READ_OFF, /* 순차/랜덤 혼합 읽기 시드 */
	FIO_RAND_SEQ_RAND_WRITE_OFF,/* 순차/랜덤 혼합 쓰기 시드 */
	FIO_RAND_SEQ_RAND_TRIM_OFF, /* 순차/랜덤 혼합 TRIM 시드 */
	FIO_RAND_START_DELAY,       /* 시작 지연 랜덤화 시드 */
	FIO_DEDUPE_OFF,             /* 중복 제거(dedupe) 시드 */
	FIO_RAND_POISSON_OFF,       /* 포아송 분포 기반 I/O 간격 시드 (읽기) */
	FIO_RAND_ZONE_OFF,          /* 존(zone) 분할 랜덤화 시드 */
	FIO_RAND_POISSON2_OFF,      /* 포아송 분포 I/O 간격 시드 (쓰기) */
	FIO_RAND_POISSON3_OFF,      /* 포아송 분포 I/O 간격 시드 (TRIM) */
	FIO_RAND_PRIO_CMDS,         /* I/O 우선순위 랜덤화 시드 */
	FIO_RAND_DEDUPE_WORKING_SET_IX, /* dedupe 워킹셋 인덱스 시드 */
	FIO_RAND_FDP_OFF,           /* FDP (Flexible Data Placement) 시드 */
	FIO_RAND_SPRANDOM_OFF,      /* 반(semi)-랜덤 시드 */
	FIO_RAND_NR_OFFS,           /* 전체 시드 오프셋 개수 (배열 크기로 사용) */
};

/*
 * I/O 모드 및 속도 처리 방식, 대기 시간 블록 유형 정의
 */
enum {
	IO_MODE_INLINE = 0,  /* 인라인 모드: 현재 스레드에서 직접 I/O 제출 */
	IO_MODE_OFFLOAD = 1, /* 오프로드 모드: 별도 워커 스레드에서 I/O 제출 */

	RATE_PROCESS_LINEAR = 0,  /* 선형 속도 제어: 일정 간격으로 I/O 발행 */
	RATE_PROCESS_POISSON = 1, /* 포아송 속도 제어: 포아송 분포에 따라 I/O 간격 결정 */

	THINKTIME_BLOCKS_TYPE_COMPLETE = 0, /* 완료된 블록 수 기준으로 대기 */
	THINKTIME_BLOCKS_TYPE_ISSUE = 1,    /* 발행한 블록 수 기준으로 대기 */
};

/*
 * 파일 접근 조언(fadvise) 유형
 * posix_fadvise() 시스템 호출에 전달할 값을 결정
 */
enum {
	F_ADV_NONE = 0,       /* 조언 없음 */
	F_ADV_TYPE,           /* I/O 유형에 따라 자동 결정 */
	F_ADV_RANDOM,         /* POSIX_FADV_RANDOM - 랜덤 접근 패턴 */
	F_ADV_SEQUENTIAL,     /* POSIX_FADV_SEQUENTIAL - 순차 접근 패턴 */
	F_ADV_NOREUSE,        /* POSIX_FADV_NOREUSE - 데이터 재사용 안함 (캐시 제거 힌트) */
};

/*
 * Per-thread/process specific data. Only used for the network client
 * for now.
 */
/* 스레드/프로세스별 네트워크 소켓 출력 데이터 관리 */
/* 클라이언트-서버 모드에서 네트워크 통신에 사용 */
void sk_out_assign(struct sk_out *); /* 현재 스레드에 소켓 출력 할당 */
void sk_out_drop(void);             /* 현재 스레드의 소켓 출력 해제 */

/*
 * 존(zone) 분할 인덱스 구조체
 * --zone_split 옵션에서 사용: I/O를 파일의 특정 영역에 가중치를 두고 분배
 * 예: zone_split=60/10:40/90 → 파일의 처음 10%에 60%의 I/O, 나머지 90%에 40%의 I/O
 */
struct zone_split_index {
	uint8_t size_perc;      /* 이 존이 전체 I/O에서 차지하는 비율 (%) */
	uint8_t size_perc_prev; /* 이전 존까지의 누적 비율 (%) */
	uint64_t size;          /* 이 존의 크기 (바이트) */
	uint64_t size_prev;     /* 이전 존까지의 누적 크기 (바이트) */
};

/*
 * This describes a single thread/process executing a fio job.
 */
/*
 * [한국어] struct thread_data - fio의 핵심 구조체
 *
 * 하나의 fio job(스레드 또는 프로세스)의 전체 런타임 상태를 관리합니다.
 * fio job 파일의 각 [jobname] 섹션이 하나의 thread_data 인스턴스에 대응됩니다.
 *
 * 이 구조체는 다음을 포함합니다:
 *   1. 사용자 설정 (thread_options o) - job 파일에서 읽은 설정값
 *   2. I/O 큐 관리 (io_u 풀, 제출 큐, 완료 큐)
 *   3. 통계 수집 (thread_stat ts, io_bytes, io_blocks 등)
 *   4. 난수 생성기 상태 (오프셋, 블록크기, 검증 등 용도별)
 *   5. 파일 관리 (files[] 배열, 파일 선택 상태)
 *   6. 속도 제어 (rate_bps, poisson_state 등)
 *   7. 검증(verify) 관련 (verify_list, verify_threads 등)
 *   8. 시간 추적 (시작시간, 에포크, 레이턴시 윈도우 등)
 */
struct thread_data {
	struct flist_head opt_list;  /* 옵션 연결 리스트 헤드 (동적 옵션 관리용) */
	unsigned long long flags;   /* TD_F_* 플래그 비트마스크 + I/O 엔진 플래그 (상위 비트) */

	/*
	 * thread_options: 사용자가 job 파일이나 커맨드라인으로 지정한 모든 설정값
	 * 예: bs(블록크기), iodepth(큐깊이), rw(읽기/쓰기 패턴),
	 *     filename(대상파일), runtime(실행시간), numjobs(스레드수) 등
	 * thread_data의 .o 필드로 접근 (예: td->o.bs[DDIR_READ])
	 */
	struct thread_options o;

	void *eo;                       /* I/O 엔진 전용 옵션 데이터 포인터 */
	pthread_t thread;               /* POSIX 스레드 핸들 */
	unsigned int thread_number;     /* 전역 스레드 번호 (0부터 시작) */
	unsigned int subjob_number;     /* numjobs 내에서의 서브잡 번호 */
	unsigned int groupid;           /* 이 스레드가 속한 그룹 ID (stonewall 구분) */

	/*
	 * thread_stat: I/O 성능 통계 결과를 저장하는 구조체
	 * IOPS, 대역폭, 레이턴시 분포 등 모든 통계 데이터를 수집
	 * 8바이트 정렬 보장 (성능 최적화)
	 */
	struct thread_stat ts __attribute__ ((aligned(8)));

	int client_type; /* 클라이언트 유형 (로컬/네트워크 등) */

	/*
	 * I/O 성능 로그 포인터들
	 * 각 로그는 시간에 따른 성능 변화를 기록하여 나중에 gnuplot 등으로 시각화 가능
	 */
	struct io_log *slat_log;      /* 제출 레이턴시(submission latency) 로그 */
	struct io_log *clat_log;      /* 완료 레이턴시(completion latency) 로그 */
	struct io_log *clat_hist_log; /* 완료 레이턴시 히스토그램 로그 */
	struct io_log *lat_log;       /* 총 레이턴시(total latency) 로그 */
	struct io_log *bw_log;        /* 대역폭(bandwidth) 로그 */
	struct io_log *iops_log;      /* IOPS 로그 */

	struct workqueue log_compress_wq; /* 로그 압축 작업을 처리하는 워크큐 */

	/*
	 * 부모 스레드 포인터
	 * numjobs > 1일 때, 복제된 자식 스레드가 원본 스레드를 가리킴
	 * 에러 전파 등에 사용됨
	 */
	struct thread_data *parent;

	/*
	 * 대역폭(BW) 샘플링을 위한 I/O 바이트 카운터
	 * 일정 시간 간격마다 이전 값과 비교하여 현재 대역폭을 계산
	 */
	uint64_t stat_io_bytes[DDIR_RWDIR_CNT]; /* 방향별(R/W/T) 통계 I/O 바이트 수 */
	struct timespec bw_sample_time;          /* 마지막 대역폭 샘플링 시각 */

	/*
	 * IOPS 샘플링을 위한 I/O 블록 카운터
	 */
	uint64_t stat_io_blocks[DDIR_RWDIR_CNT]; /* 방향별(R/W/T) 통계 I/O 블록 수 */
	struct timespec iops_sample_time;         /* 마지막 IOPS 샘플링 시각 */

	/*
	 * 리소스 사용량(CPU 시간 등) 추적
	 * getrusage()로 시작/종료 시점의 리소스 사용량을 측정
	 */
	volatile int update_rusage;     /* 리소스 사용량 업데이트 요청 플래그 (volatile: 스레드 간 가시성) */
	struct fio_sem *rusage_sem;     /* 리소스 사용량 동기화 세마포어 */
	struct rusage ru_start;         /* 시작 시점의 리소스 사용량 */
	struct rusage ru_end;           /* 종료 시점의 리소스 사용량 */

	/*
	 * 파일 관리
	 * fio job이 I/O를 수행하는 대상 파일들을 관리
	 */
	struct flist_head fs_list;      /* 파일 목록 연결 리스트 */
	struct fio_file **files;        /* 파일 포인터 배열 (인덱스로 빠른 접근) */
	unsigned char *file_locks;      /* 파일별 잠금 상태 배열 */
	unsigned int files_size;        /* files 배열의 할당된 크기 */
	unsigned int files_index;       /* 현재까지 추가된 파일 수 (다음 추가 위치) */
	unsigned int nr_open_files;     /* 현재 열려 있는 파일 수 */
	unsigned int nr_done_files;     /* 작업 완료된 파일 수 */

	/*
	 * 다음에 사용할 파일을 결정하는 상태
	 * file_service_type 옵션에 따라 라운드로빈 또는 랜덤 선택
	 */
	union {
		unsigned int next_file;            /* 라운드로빈 시 다음 파일 인덱스 */
		struct frand_state next_file_state; /* 랜덤 선택 시 난수 생성기 상태 */
	};
	/* Zipf/Gauss 분포 기반 파일 선택 시 사용하는 상태 */
	union {
		struct zipf_state next_file_zipf;  /* Zipf 분포 상태 */
		struct gauss_state next_file_gauss; /* 가우시안(정규) 분포 상태 */
	};
	/* 분포 매개변수 */
	union {
		double zipf_theta;  /* Zipf 분포의 theta 매개변수 (클수록 편향 심함) */
		double pareto_h;    /* Pareto 분포의 h 매개변수 */
		double gauss_dev;   /* 가우시안 분포의 표준편차 */
	};
	double random_center;   /* 랜덤 분포의 중심점 (0.0~1.0, 파일 내 위치 비율) */

	/*
	 * 에러 및 프로세스 상태
	 */
	int error;              /* 마지막 에러 코드 (0이면 에러 없음) */
	int sig;                /* 수신한 시그널 번호 */
	int done;               /* 작업 완료 여부 (1이면 완료) */
	int stop_io;            /* I/O 중지 요청 플래그 */
	pid_t pid;              /* 프로세스 ID (fork 모드에서 사용) */

	/*
	 * I/O 버퍼 관리
	 * fio는 I/O에 사용할 대형 버퍼를 미리 할당하고,
	 * 각 io_u가 이 버퍼의 일부분을 가리킴
	 */
	char *orig_buffer;        /* 원본 I/O 버퍼 포인터 (정렬 전) */
	size_t orig_buffer_size;  /* 원본 버퍼 크기 */

	volatile int runstate;    /* 현재 실행 상태 (TD_RUNNING, TD_VERIFYING 등, volatile: 스레드 간 가시성) */
	volatile bool terminate;  /* 종료 요청 플래그 */

	enum fio_ddir last_ddir_completed; /* 마지막으로 완료된 I/O 방향 (READ/WRITE/TRIM) */
	enum fio_ddir last_ddir_issued;    /* 마지막으로 발행된 I/O 방향 */

	int mmapfd; /* mmap에 사용된 파일 디스크립터 (메모리 할당이 mmap 방식일 때) */

	/*
	 * I/O 로그 재생(replay) 관련
	 */
	void *iolog_buf;  /* I/O 로그 읽기 버퍼 */
	FILE *iolog_f;    /* I/O 로그 파일 포인터 */

	/*
	 * 난수 시드 배열 및 난수 생성기 상태들
	 * 각 용도(블록크기, 오프셋, 검증 등)별로 독립적인 난수 생성기를 유지하여
	 * 동일한 시드로 동일한 I/O 패턴을 재현할 수 있음
	 */
	uint64_t rand_seeds[FIO_RAND_NR_OFFS]; /* 각 용도별 난수 시드 배열 */

	struct frand_state bsrange_state[DDIR_RWDIR_CNT]; /* 방향별 블록 크기 랜덤화 상태 */
	struct frand_state verify_state;            /* 검증 데이터 생성/비교 난수 상태 */
	struct frand_state verify_state_last_do_io; /* 마지막 I/O 수행 시점의 검증 난수 상태 (복원용) */
	struct frand_state trim_state;              /* TRIM 오프셋 랜덤화 상태 */
	struct frand_state delay_state;             /* 시작 지연 랜덤화 상태 */
	struct frand_state fdp_state;               /* FDP (Flexible Data Placement) 난수 상태 */

	struct frand_state buf_state;      /* 버퍼 내용 랜덤화 현재 상태 */
	struct frand_state buf_state_prev; /* 버퍼 내용 랜덤화 이전 상태 (되돌리기용) */
	struct frand_state buf_state_ret;  /* 버퍼 내용 랜덤화 반환 상태 */
	struct frand_state dedupe_state;   /* dedupe(중복제거) 시뮬레이션 난수 상태 */
	struct frand_state zone_state;     /* 존 분할 난수 상태 */
	struct frand_state prio_state;     /* I/O 우선순위 랜덤화 난수 상태 */
	struct frand_state dedupe_working_set_index_state; /* dedupe 워킹셋 인덱스 난수 상태 */
	struct frand_state *dedupe_working_set_states;     /* dedupe 워킹셋별 난수 상태 배열 */
	struct frand_state sprandom_state; /* 반(semi)-랜덤 난수 상태 */

	unsigned long long num_unique_pages; /* dedupe에서 고유 페이지 수 */

	struct zone_split_index **zone_state_index; /* 존 분할 인덱스 배열 (방향별) */
	unsigned int num_write_zones;                /* 쓰기 존의 수 */

	/*
	 * 배치 처리 설정
	 * 여러 I/O를 모아서 한번에 검증하거나 TRIM하는 단위
	 */
	unsigned int verify_batch; /* 한 번에 검증할 I/O 개수 */
	unsigned int trim_batch;   /* 한 번에 TRIM할 I/O 개수 */
	bool trim_verify;          /* TRIM 후 검증 수행 여부 */

	struct thread_io_list *vstate; /* 검증 상태 리스트 (검증 상태 저장/복원용) */

	int shm_id; /* 공유 메모리 세그먼트 ID (프로세스 모드에서 thread_data 공유) */

	/*
	 * Job default IO priority set with prioclass and prio options.
	 */
	/* prioclass와 prio 옵션으로 설정된 기본 I/O 우선순위 */
	/* ioprio 값은 Linux ioprio_set() 시스템 호출에 전달됨 */
	unsigned int ioprio;

	/*
	 * IO engine hooks, contains everything needed to submit an io_u
	 * to any of the available IO engines.
	 */
	/*
	 * I/O 엔진 연산 함수 포인터 테이블
	 * libaio, io_uring, sync, mmap 등 다양한 I/O 엔진의 인터페이스
	 * queue(), getevents(), open_file() 등의 콜백 함수를 포함
	 */
	struct ioengine_ops *io_ops;
	int io_ops_init; /* I/O 엔진이 초기화되었는지 여부 (1=초기화됨) */

	/*
	 * IO engine private data and dlhandle.
	 */
	/* I/O 엔진의 내부 데이터 (엔진별로 다른 구조체를 가리킴) */
	/* 예: libaio의 io_context, io_uring의 ring 구조체 등 */
	void *io_ops_data;

	/*
	 * Queue depth of io_u's that fio MIGHT do
	 */
	/* fio가 처리할 수 있는 io_u의 현재 큐 깊이 */
	/* io_u_queued + io_u_in_flight의 합 */
	unsigned int cur_depth;

	/*
	 * io_u's about to be committed
	 */
	/* 커밋 대기 중인 io_u 수 (queue()는 호출했지만 commit()은 아직 안 한 상태) */
	/* I/O 엔진의 queue()에 넣었지만 아직 실제로 디바이스에 제출하지 않은 것들 */
	unsigned int io_u_queued;

	/*
	 * io_u's submitted but not completed yet
	 */
	/* 제출되었지만 아직 완료되지 않은 io_u 수 */
	/* 비동기 I/O에서 디바이스가 처리 중인 요청 수 */
	unsigned int io_u_in_flight;

	/*
	 * List of free and busy io_u's
	 *
	 * [한국어] io_u 풀 관리
	 * io_u의 생명주기: freelist -> queued -> in_flight -> freelist
	 *   1. io_u_freelist에서 io_u를 꺼냄 (get_io_u)
	 *   2. 오프셋, 크기, 방향 등을 설정
	 *   3. I/O 엔진의 queue()로 제출 (io_u_queued 증가)
	 *   4. commit()으로 실제 제출 (io_u_in_flight 증가, io_u_queued 감소)
	 *   5. getevents()로 완료 확인 후 freelist로 반환 (io_u_in_flight 감소)
	 */
	struct io_u_ring io_u_requeues;   /* 재요청 대기 중인 io_u 링 버퍼 */
	struct io_u_queue io_u_freelist;  /* 사용 가능한 io_u 큐 (풀) */
	struct io_u_queue io_u_all;       /* 모든 io_u를 담는 큐 (전체 풀 관리) */
	pthread_mutex_t io_u_lock;        /* io_u 풀 접근 동기화 뮤텍스 */
	pthread_cond_t free_cond;         /* io_u가 반환될 때 대기 스레드를 깨우는 조건 변수 */

	/*
	 * async verify offload
	 */
	/*
	 * 비동기 검증(verify) 오프로드
	 * 별도의 검증 전용 스레드에서 데이터 검증을 병렬 수행
	 * I/O 스레드는 쓰기에 집중하고, 검증 스레드가 읽기+검증 담당
	 */
	struct flist_head verify_list;       /* 검증 대기 중인 io_u 리스트 */
	pthread_t *verify_threads;           /* 검증 스레드 배열 */
	unsigned int nr_verify_threads;      /* 검증 스레드 수 */
	pthread_cond_t verify_cond;          /* 검증 작업 도착 알림 조건 변수 */
	int verify_thread_exit;              /* 검증 스레드 종료 요청 플래그 */

	/*
	 * Rate state
	 */
	/*
	 * I/O 속도 제한(rate limiting) 상태
	 * rate=, rate_iops= 등의 옵션으로 I/O 속도를 제한할 때 사용
	 */
	uint64_t rate_bps[DDIR_RWDIR_CNT];               /* 방향별 목표 속도 (bytes/sec) */
	uint64_t rate_next_io_time[DDIR_RWDIR_CNT];       /* 다음 I/O를 발행해야 할 시각 (나노초) */
	unsigned long long last_rate_check_bytes[DDIR_RWDIR_CNT]; /* 마지막 속도 체크 시 바이트 수 */
	unsigned long last_rate_check_blocks[DDIR_RWDIR_CNT];     /* 마지막 속도 체크 시 블록 수 */
	unsigned long long rate_io_issue_bytes[DDIR_RWDIR_CNT];   /* 속도 제어용 발행 바이트 누적 */
	struct timespec last_rate_check_time[DDIR_RWDIR_CNT];     /* 마지막 속도 체크 시각 */
	int64_t last_usec[DDIR_RWDIR_CNT];                /* 마지막 I/O의 소요 시간 (마이크로초) */
	struct frand_state poisson_state[DDIR_RWDIR_CNT]; /* 포아송 분포 I/O 간격 난수 상태 */

	/*
	 * Enforced rate submission/completion workqueue
	 */
	/* 속도 제한 I/O 제출/완료를 위한 워크큐 (IO_MODE_OFFLOAD에서 사용) */
	struct workqueue io_wq;

	uint64_t total_io_size;      /* 총 수행할 I/O 크기 (바이트) */
	uint64_t fill_device_size;   /* fill_device=1일 때 디바이스 전체 크기 */

	/*
	 * Issue side
	 */
	/* I/O 발행(issue) 측 카운터 */
	uint64_t io_issues[DDIR_RWDIR_CNT];       /* 방향별 발행된 I/O 요청 수 */
	uint64_t verify_read_issues;               /* 검증을 위해 발행된 읽기 요청 수 */
	uint64_t io_issue_bytes[DDIR_RWDIR_CNT];   /* 방향별 발행된 I/O 바이트 수 */
	uint64_t loops;                             /* 완료된 반복(loop) 횟수 */

	/*
	 * Keep track of inflight write sequence numbers (numberio) which are used to save verify state.
	 */
	/* 진행 중인 쓰기의 시퀀스 번호(numberio) 추적 */
	/* 검증 상태를 저장할 때 어떤 쓰기가 아직 완료되지 않았는지 알기 위해 사용 */
	uint64_t *inflight_numberio;             /* 진행 중인 쓰기의 번호 배열 */
	unsigned int next_inflight_numberio_idx; /* 다음 사용할 배열 인덱스 */
	uint64_t inflight_issued;                /* 발행되었지만 미완료 쓰기 수 */

	/*
	 * Completions
	 */
	/*
	 * I/O 완료(completion) 측 카운터
	 * io_bytes: 전체 누적 / this_io_bytes: 현재 루프(loop)의 누적
	 */
	uint64_t io_blocks[DDIR_RWDIR_CNT];      /* 방향별 완료된 총 I/O 블록 수 */
	uint64_t this_io_blocks[DDIR_RWDIR_CNT]; /* 현재 루프에서 완료된 I/O 블록 수 */
	uint64_t io_bytes[DDIR_RWDIR_CNT];       /* 방향별 완료된 총 I/O 바이트 수 */
	uint64_t this_io_bytes[DDIR_RWDIR_CNT];  /* 현재 루프에서 완료된 I/O 바이트 수 */
	uint64_t io_skip_bytes;   /* 건너뛴 I/O 바이트 수 (verify에서 실패한 영역 등) */
	uint64_t zone_bytes;      /* 현재 존에서 수행한 I/O 바이트 수 */
	struct fio_sem *sem;      /* 스레드 동기화 세마포어 */
	uint64_t bytes_done[DDIR_RWDIR_CNT]; /* 방향별 실제 완료 처리된 바이트 수 */
	uint64_t bytes_verified;  /* 검증 완료된 바이트 수 */

	/*
	 * 대기 시간(thinktime) 관련
	 * 실제 애플리케이션의 I/O 간 대기 시간을 시뮬레이션
	 */
	uint64_t *thinktime_blocks_counter; /* 대기 시간 적용 블록 카운터 포인터 */
	struct timespec last_thinktime;      /* 마지막 대기 시간 적용 시각 */
	int64_t last_thinktime_blocks;       /* 마지막 대기 시간 이후 처리한 블록 수 */

	/*
	 * State for random offsets
	 */
	/* 랜덤 오프셋 생성을 위한 난수 생성기 상태 */
	struct frand_state offset_state;

	/*
	 * 시간 추적 필드들
	 */
	struct timespec start;	/* start of this loop */
	                        /* 현재 반복(loop)의 시작 시각 */
	struct timespec epoch;	/* time job was started */
	                        /* job이 시작된 시각 (기준 시간) */
	unsigned long long alternate_epoch; /* Time job was started, as clock_gettime(log_alternate_epoch_clock_id) */
	                                    /* 대체 클록으로 측정한 job 시작 시각 */
	unsigned long long job_start; /* Time job was started, as clock_gettime(job_start_clock_id) */
	                              /* job_start_clock_id 클록으로 측정한 job 시작 시각 */
	struct timespec last_issue;   /* 마지막 I/O 발행 시각 */
	long time_offset;             /* 시간 보정 오프셋 */
	struct timespec ts_cache;     /* 캐시된 현재 시각 (빈번한 시간 조회 최적화) */
	struct timespec terminate_time; /* 종료 요청 시각 */
	unsigned int ts_cache_nr;     /* 시간 캐시 갱신 카운터 */
	unsigned int ts_cache_mask;   /* 시간 캐시 갱신 마스크 (n번마다 갱신) */
	unsigned int ramp_period_state; /* 램프업 기간 상태 (워밍업 중인지 여부) */

	/*
	 * Time since last latency_window was started
	 */
	/*
	 * 레이턴시 타겟 관련 필드
	 * latency_target 옵션: 목표 레이턴시를 충족하도록 큐 깊이를 자동 조절
	 * 레이턴시가 목표보다 높으면 큐 깊이를 줄이고, 낮으면 늘림
	 */
	struct timespec latency_ts;          /* 현재 레이턴시 측정 윈도우 시작 시각 */
	unsigned int latency_qd;             /* 현재 레이턴시 타겟 큐 깊이 */
	unsigned int latency_qd_high;        /* 큐 깊이 상한 (이진 탐색 상한) */
	unsigned int latency_qd_low;         /* 큐 깊이 하한 (이진 탐색 하한) */
	unsigned int latency_failed;         /* 레이턴시 목표 실패 횟수 */
	unsigned int latency_stable_count;   /* 레이턴시 안정 연속 횟수 */
	uint64_t latency_ios;                /* 현재 윈도우에서 수행한 I/O 수 */
	int latency_end_run;                 /* 레이턴시 타겟으로 인한 실행 종료 플래그 */

	/*
	 * read/write mixed workload state
	 */
	/*
	 * 읽기/쓰기 혼합 워크로드 상태
	 * rwmixread/rwmixwrite 옵션으로 읽기/쓰기 비율을 제어
	 */
	struct frand_state rwmix_state; /* 읽기/쓰기 선택 난수 상태 */
	unsigned long rwmix_issues;     /* 혼합 워크로드에서 발행된 I/O 수 */
	enum fio_ddir rwmix_ddir;       /* 현재 혼합 I/O 방향 (READ 또는 WRITE) */
	unsigned int ddir_seq_nr;       /* 같은 방향으로 연속 발행할 횟수 (남은 수) */

	/*
	 * rand/seq mixed workload state
	 */
	/* 랜덤/순차 혼합 워크로드 상태 */
	/* percentage_random 옵션으로 랜덤과 순차 I/O의 비율을 제어 */
	struct frand_state seq_rand_state[DDIR_RWDIR_CNT];

	/*
	 * IO history logs for verification. We use a tree for sorting,
	 * if we are overwriting. Otherwise just use a fifo.
	 */
	/*
	 * 검증(verify)을 위한 I/O 이력 로그
	 * 쓰기한 위치를 기록하여 나중에 읽어서 데이터 무결성을 검증
	 * 덮어쓰기(overwrite) 시: RB 트리로 정렬하여 관리
	 * 그 외: FIFO로 순서대로 관리
	 */
	struct rb_root io_hist_tree;     /* I/O 이력 Red-Black 트리 루트 */
	struct flist_head io_hist_list;  /* I/O 이력 FIFO 리스트 */
	unsigned long io_hist_len;       /* I/O 이력 항목 수 */

	/*
	 * For IO replaying
	 */
	/*
	 * I/O 재생(replay) 관련
	 * 이전에 기록한 I/O 로그 또는 blktrace를 재생하여 동일한 I/O 패턴을 수행
	 */
	struct flist_head io_log_list;          /* 재생할 I/O 로그 항목 리스트 */
	FILE *io_log_rfile;                      /* I/O 로그 읽기 파일 포인터 */
	unsigned int io_log_blktrace;            /* blktrace 형식 로그 사용 여부 */
	unsigned int io_log_blktrace_swap;       /* blktrace 바이트 순서 변환 필요 여부 */
	unsigned long long io_log_last_ttime;    /* 마지막 I/O 로그 항목의 타임스탬프 */
	struct timespec io_log_start_time;       /* I/O 로그 재생 시작 시각 */
	unsigned int io_log_current;             /* 현재 재생 중인 로그 항목 인덱스 */
	unsigned int io_log_checkmark;           /* I/O 로그 체크포인트 마크 */
	unsigned int io_log_highmark;            /* I/O 로그 하이워터마크 (미리 읽기 한계) */
	unsigned int io_log_version;             /* I/O 로그 형식 버전 */
	struct timespec io_log_highmark_time;    /* 하이워터마크 도달 시각 */

	/*
	 * For tracking/handling discards
	 */
	/* TRIM/DISCARD 추적 및 처리 */
	struct flist_head trim_list;    /* TRIM 대상 영역 리스트 */
	unsigned long trim_entries;     /* TRIM 리스트의 항목 수 */

	/*
	 * for fileservice, how often to switch to a new file
	 */
	/* 파일 서비스: 여러 파일 간 전환 주기 제어 */
	unsigned int file_service_nr;      /* 파일 전환 주기 (이 횟수만큼 I/O 후 다음 파일로) */
	unsigned int file_service_left;    /* 현재 파일에서 남은 I/O 횟수 */
	struct fio_file *file_service_file; /* 현재 서비스 중인 파일 포인터 */

	unsigned int sync_file_range_nr; /* sync_file_range 호출 주기 */

	/*
	 * For generating file sizes
	 */
	/* 파일 크기 생성을 위한 난수 상태 (file_size_range 옵션 사용 시) */
	struct frand_state file_size_state;

	/*
	 * Error counts
	 */
	/* 에러 카운터 */
	unsigned int total_err_count; /* 누적 에러 횟수 */
	int first_error;              /* 첫 번째 발생한 에러 코드 */

	/*
	 * 흐름 제어(flow control)
	 * 여러 job 간의 I/O 속도를 동기화하여 균형 맞춤
	 */
	struct fio_flow *flow;               /* 흐름 제어 구조체 포인터 */
	unsigned long long flow_counter;     /* 흐름 제어 카운터 */

	/*
	 * Can be overloaded by profiles
	 */
	/* 프로파일에 의해 오버로드될 수 있는 I/O 연산 콜백 */
	/* tiobench 등 사전정의 워크로드 프로파일이 커스텀 동작을 주입할 때 사용 */
	struct prof_io_ops prof_io_ops;
	void *prof_data; /* 프로파일 전용 데이터 포인터 */

	void *pinned_mem; /* 고정(pinned) 메모리 포인터 (DMA 등에서 페이지 고정 필요 시) */

	struct steadystate_data ss; /* 정상 상태(steady state) 감지 데이터 */

	char verror[FIO_VERROR_SIZE]; /* 에러 메시지 문자열 버퍼 */

#ifdef CONFIG_CUDA
	/*
	 * for GPU memory management
	 */
	/* GPU 메모리 관리 (CUDA 지원 시) */
	/* GPU Direct Storage 등에서 GPU 메모리로 직접 I/O 수행 */
	int gpu_dev_cnt;           /* 사용 가능한 GPU 디바이스 수 */
	int gpu_dev_id;            /* 사용할 GPU 디바이스 ID */
	CUdevice  cu_dev;          /* CUDA 디바이스 핸들 */
	CUcontext cu_ctx;          /* CUDA 컨텍스트 핸들 */
	CUdeviceptr dev_mem_ptr;   /* GPU 디바이스 메모리 포인터 */
#endif

};

/*
 * 스레드 세그먼트 구조체
 * thread_data 배열을 세그먼트 단위로 관리하여 공유 메모리 효율성 향상
 * 각 세그먼트는 JOBS_PER_SEG개의 thread_data를 포함
 */
struct thread_segment {
	struct thread_data *threads; /* 이 세그먼트의 thread_data 배열 */
	int shm_id;                  /* 공유 메모리 세그먼트 ID */
	int nr_threads;              /* 이 세그먼트에 있는 스레드 수 */
};

/*
 * when should interactive ETA output be generated
 */
/* 대화형 ETA(예상 완료 시간) 출력 시기 설정 */
enum {
	FIO_ETA_AUTO,   /* 자동 (터미널이면 출력, 아니면 미출력) */
	FIO_ETA_ALWAYS, /* 항상 ETA 출력 */
	FIO_ETA_NEVER,  /* ETA 출력 안함 */
};

/*
 * __td_verror 매크로: thread_data에 에러 상태를 기록
 * 이미 에러가 설정되어 있으면 무시 (첫 번째 에러만 기록)
 * verror 버퍼에 파일명, 줄번호, 함수명, 에러 메시지를 포맷하여 저장
 */
#define __td_verror(td, err, msg, func)					\
	do {								\
		unsigned int ____e = (err);				\
		if ((td)->error)					\
			break;						\
		(td)->error = ____e;					\
		nowarn_snprintf(td->verror, sizeof(td->verror),		\
				"file:%s:%d, func=%s, error=%s",	\
				__FILE__, __LINE__, (func), (msg));	\
	} while (0)


/*
 * td_clear_error: thread_data와 부모의 에러 상태를 초기화
 */
#define td_clear_error(td)		do {		\
	(td)->error = 0;				\
	if ((td)->parent)				\
		(td)->parent->error = 0;		\
} while (0)

/*
 * td_verror: errno 기반 에러를 설정하고 부모에게도 전파
 * strerror()로 에러 번호를 문자열로 변환하여 기록
 */
#define td_verror(td, err, func)	do {			\
	__td_verror((td), (err), strerror((err)), (func));	\
	if ((td)->parent)					\
		__td_verror((td)->parent, (err), strerror((err)), (func)); \
} while (0)

/*
 * td_vmsg: 커스텀 메시지로 에러를 설정하고 부모에게도 전파
 */
#define td_vmsg(td, err, msg, func)	do {			\
	__td_verror((td), (err), (msg), (func));		\
	if ((td)->parent)					\
		__td_verror((td)->parent, (err), (msg), (func));	\
} while (0)

/* 문자열화(stringify) 헬퍼 매크로 */
#define __fio_stringify_1(x)	#x         /* 인자를 문자열로 변환 */
#define __fio_stringify(x)	__fio_stringify_1(x) /* 매크로 확장 후 문자열화 */

/* Job 수 및 세그먼트 관련 상수 */
#define REAL_MAX_JOBS		4096                        /* 최대 job(스레드/프로세스) 수 */
#define JOBS_PER_SEG		8                           /* 세그먼트당 job 수 */
#define REAL_MAX_SEG		(REAL_MAX_JOBS / JOBS_PER_SEG) /* 최대 세그먼트 수 (512) */

/*
 * 전역 변수 선언
 */
extern bool exitall_on_terminate;      /* 하나의 job이 종료하면 모든 job을 종료할지 여부 */
extern unsigned int thread_number;     /* 현재 등록된 전체 스레드 수 */
extern unsigned int stat_number;       /* 통계 출력용 번호 */
extern unsigned int nr_segments;       /* 사용 중인 세그먼트 수 */
extern unsigned int cur_segment;       /* 현재 세그먼트 인덱스 */
extern int groupid;                    /* 현재 그룹 ID */
extern int output_format;             /* 출력 형식 (terse, JSON, normal 등) */
extern int append_terse_output;        /* terse 출력을 파일에 추가할지 여부 */
extern int temp_stall_ts;             /* 임시 지연 타임스탬프 */
extern uintptr_t page_mask, page_size; /* 시스템 페이지 크기 및 마스크 */
extern bool read_only;                 /* 읽기 전용 모드 (쓰기 방지 안전장치) */
extern int eta_print;                  /* ETA 출력 모드 (auto/always/never) */
extern int eta_new_line;               /* ETA 출력 후 줄바꿈 간격 */
extern unsigned int eta_interval_msec; /* ETA 업데이트 간격 (밀리초) */
extern unsigned long done_secs;        /* 완료된 실행 시간 (초) */
extern int fio_gtod_offload;          /* gettimeofday를 별도 스레드에서 실행할지 여부 */
extern int fio_gtod_cpu;              /* gtod 전용 CPU 번호 */
extern enum fio_cs fio_clock_source;   /* 시간 측정 클록 소스 */
extern int fio_clock_source_set;       /* 클록 소스가 명시적으로 설정되었는지 여부 */
extern int warnings_fatal;            /* 경고를 치명적 에러로 처리할지 여부 */
extern int terse_version;             /* terse 출력 형식 버전 */
extern bool is_backend;               /* 백엔드(서버) 모드로 실행 중인지 여부 */
extern bool is_local_backend;         /* 로컬 백엔드 모드로 실행 중인지 여부 */
extern int nr_clients;                /* 연결된 클라이언트 수 */
extern bool log_syslog;               /* syslog로 로깅할지 여부 */
extern int status_interval;           /* 상태 출력 간격 (초) */
extern const char fio_version_string[]; /* fio 버전 문자열 */
extern char *trigger_file;            /* 트리거 파일 경로 (파일 존재 시 트리거 실행) */
extern char *trigger_cmd;             /* 트리거 시 실행할 로컬 명령 */
extern char *trigger_remote_cmd;      /* 트리거 시 실행할 원격 명령 */
extern long long trigger_timeout;     /* 트리거 타임아웃 (마이크로초) */
extern char *aux_path;                /* 보조 파일 경로 (로그 등) */

/* 전체 스레드 세그먼트 배열 */
extern struct thread_segment segments[REAL_MAX_SEG];

/*
 * tnumber_to_td: 전역 스레드 번호로부터 thread_data 포인터를 얻는 인라인 함수
 * 세그먼트 인덱스 = tnumber / JOBS_PER_SEG
 * 세그먼트 내 인덱스 = tnumber % JOBS_PER_SEG (비트 마스크 사용으로 최적화)
 */
static inline struct thread_data *tnumber_to_td(unsigned int tnumber)
{
	struct thread_segment *seg;

	seg = &segments[tnumber / JOBS_PER_SEG];
	return &seg->threads[tnumber & (JOBS_PER_SEG - 1)];
}

/* 백엔드(서버) 모드로 실행 중인지 확인 (로컬 포함) */
static inline bool is_running_backend(void)
{
	return is_backend || is_local_backend;
}

/* ETA 시간이 허용 오차(slack) 내에 있는지 확인 */
extern bool eta_time_within_slack(unsigned int time);

/*
 * fio_ro_check: 읽기 전용 모드에서 쓰기/TRIM이 시도되지 않는지 검증
 * 디버그 빌드에서 assert로 안전장치 역할
 */
static inline void fio_ro_check(const struct thread_data *td, struct io_u *io_u)
{
	assert(!(io_u->ddir == DDIR_WRITE && !td_write(td)) &&
	       !(io_u->ddir == DDIR_TRIM && !(td_trim(td) || td->trim_verify)));

	/*
	 * The last line above allows trim operations during trim/verify
	 * workloads. For these workloads we cannot simply set the trim bit for
	 * the thread's ddir because then fio would assume that
	 * ddir={trimewrite, randtrimwrite}.
	 */
	/*
	 * 위의 마지막 줄은 trim/verify 워크로드에서 TRIM 연산을 허용합니다.
	 * 이런 워크로드에서는 스레드의 ddir에 단순히 trim 비트를 설정할 수 없는데,
	 * 그렇게 하면 fio가 ddir를 trimwrite나 randtrimwrite로 인식하기 때문입니다.
	 */
}

/*
 * multi_range_trim: 멀티 레인지 TRIM인지 확인
 * 하나의 TRIM 명령에 여러 영역을 포함할 수 있는지 여부
 */
static inline bool multi_range_trim(struct thread_data *td, struct io_u *io_u)
{
	if (io_u->ddir == DDIR_TRIM && td->o.num_range > 1)
		return true;

	return false;
}

/*
 * should_fsync: fsync를 수행해야 하는지 판단
 * 마지막 발행된 I/O가 이미 sync였으면 중복 수행하지 않음
 * 쓰기 job이거나 override_sync가 설정된 경우에만 fsync 수행
 */
static inline bool should_fsync(struct thread_data *td)
{
	if (ddir_sync(td->last_ddir_issued))
		return false;
	if (td_write(td) || td->o.override_sync)
		return true;

	return false;
}

/*
 * Init/option functions
 */
/* 초기화 및 옵션 파싱 함수들 */
extern int __must_check fio_init_options(void);       /* fio 옵션 시스템 초기화 */
extern int __must_check parse_options(int, char **);  /* 커맨드라인 옵션 파싱 */
extern int parse_jobs_ini(char *, int, int, int);     /* INI 형식 job 파일 파싱 */
extern int parse_cmd_line(int, char **, int);         /* 커맨드라인 인자 파싱 */
extern int fio_backend(struct sk_out *);              /* fio 백엔드 메인 루프 실행 */
extern void reset_fio_state(void);                    /* fio 전역 상태 초기화 */
extern void clear_io_state(struct thread_data *, int); /* 스레드의 I/O 상태 초기화 */
extern int fio_options_parse(struct thread_data *, char **, int); /* 옵션 문자열 파싱 */
extern void fio_keywords_init(void);                  /* 옵션 키워드 초기화 */
extern void fio_keywords_exit(void);                  /* 옵션 키워드 정리 */
extern int fio_cmd_option_parse(struct thread_data *, const char *, char *);       /* 단일 옵션 파싱 */
extern int fio_cmd_ioengine_option_parse(struct thread_data *, const char *, char *); /* I/O 엔진 옵션 파싱 */
extern void fio_fill_default_options(struct thread_data *); /* 기본 옵션값으로 채우기 */
extern int fio_show_option_help(const char *);         /* 옵션 도움말 출력 */
extern void fio_options_set_ioengine_opts(struct option *long_options, struct thread_data *td); /* I/O 엔진 옵션 설정 */
extern void fio_options_dup_and_init(struct option *); /* 옵션 복제 및 초기화 */
extern char *fio_option_dup_subs(const char *);        /* 옵션 문자열 내 변수 치환 */
extern void fio_options_mem_dupe(struct thread_data *); /* 옵션의 메모리 복제 (문자열 등) */
extern void td_fill_rand_seeds(struct thread_data *);  /* 난수 시드 초기화 */
extern void init_rand_offset_seed(struct thread_data *); /* 랜덤 오프셋 시드 초기화 */
extern void add_job_opts(const char **, int);          /* job 옵션 추가 */
extern int ioengine_load(struct thread_data *);        /* I/O 엔진 동적 로드 */
extern bool parse_dryrun(void);                        /* 드라이런(실행 없이 파싱만) 모드 여부 */
extern int fio_running_or_pending_io_threads(void);    /* 실행 중이거나 대기 중인 스레드 수 */
extern int fio_set_fd_nonblocking(int, const char *);  /* 파일 디스크립터를 논블로킹으로 설정 */
extern void sig_show_status(int sig);                  /* 시그널 핸들러: 상태 출력 */
extern struct thread_data *get_global_options(void);   /* 전역 옵션 thread_data 얻기 */

extern uintptr_t page_mask;       /* 페이지 마스크 (page_size - 1) */
extern uintptr_t page_size;       /* 시스템 페이지 크기 */
extern int initialize_fio(char *envp[]); /* fio 전체 초기화 (환경변수 포함) */
extern void deinitialize_fio(void);      /* fio 정리 및 종료 */

/* getopt 관련 상수 */
#define FIO_GETOPT_JOB		0x89000000   /* fio job 옵션의 getopt ID 베이스 */
#define FIO_GETOPT_IOENGINE	0x98000000   /* I/O 엔진 옵션의 getopt ID 베이스 */
#define FIO_NR_OPTIONS		(FIO_MAX_OPTS + 128) /* 최대 옵션 수 */

/*
 * ETA/status stuff
 */
/* ETA 및 상태 출력 함수들 */
extern void print_thread_status(void);          /* 스레드 상태 출력 (ETA 포함) */
extern void print_status_init(int);             /* 상태 출력 초기화 */
extern char *fio_uint_to_kmg(unsigned int val); /* 숫자를 K/M/G 단위 문자열로 변환 */

/*
 * Thread life cycle. Once a thread has a runstate beyond TD_INITIALIZED, it
 * will never back again. It may cycle between running/verififying/fsyncing.
 * Once the thread reaches TD_EXITED, it is just waiting for the core to
 * reap it.
 */
/*
 * 스레드 생명주기(lifecycle) 상태
 * TD_INITIALIZED 이후로는 이전 상태로 돌아가지 않음.
 * RUNNING/VERIFYING/FSYNCING 사이에서는 순환 가능.
 * TD_EXITED에 도달하면 코어가 회수(reap)하기를 기다리는 상태.
 *
 * 상태 전이: NOT_CREATED -> CREATED -> INITIALIZED -> RAMP -> SETTING_UP
 *          -> RUNNING <-> PRE_READING/VERIFYING/FSYNCING
 *          -> FINISHING -> EXITED -> REAPED
 */
enum {
	TD_NOT_CREATED = 0, /* 아직 생성되지 않음 */
	TD_CREATED,         /* 스레드/프로세스가 생성됨 */
	TD_INITIALIZED,     /* 초기화 완료 (I/O 엔진, 파일 등) */
	TD_RAMP,            /* 램프업(워밍업) 기간 중 */
	TD_SETTING_UP,      /* I/O 실행 전 설정 중 */
	TD_RUNNING,         /* I/O 실행 중 */
	TD_PRE_READING,     /* 사전 읽기 중 (pre_read 옵션) */
	TD_VERIFYING,       /* 데이터 검증 중 */
	TD_FSYNCING,        /* fsync 수행 중 */
	TD_FINISHING,       /* 마무리 중 */
	TD_EXITED,          /* 스레드가 종료됨 (회수 대기) */
	TD_REAPED,          /* 코어에 의해 회수됨 */
	TD_LAST,            /* 마지막 표시 */
	TD_NR,              /* 상태 수 */
};

/*
 * I/O 엔진 플래그 관련 상수
 * td->flags의 상위 비트에 I/O 엔진 플래그를 저장
 * 하위 비트는 TD_F_* 플래그가 사용
 */
#define TD_ENG_FLAG_SHIFT	(__TD_F_LAST)                      /* I/O 엔진 플래그 시작 비트 위치 */
#define TD_ENG_FLAG_MASK	((1ULL << (__TD_F_LAST)) - 1)      /* TD_F_* 플래그 마스크 */

/*
 * td_set_ioengine_flags: I/O 엔진 플래그를 td->flags의 상위 비트에 설정
 * 기존 엔진 플래그를 지우고 새로운 플래그를 설정
 */
static inline void td_set_ioengine_flags(struct thread_data *td)
{
	td->flags = (~(TD_ENG_FLAG_MASK << TD_ENG_FLAG_SHIFT) & td->flags) |
		    ((unsigned long long)td->io_ops->flags << TD_ENG_FLAG_SHIFT);
}

/*
 * td_ioengine_flagged: 특정 I/O 엔진 플래그가 설정되어 있는지 확인
 */
static inline bool td_ioengine_flagged(struct thread_data *td,
				       enum fio_ioengine_flags flags)
{
	return ((td->flags >> TD_ENG_FLAG_SHIFT) & flags) != 0;
}

/* 스레드 실행 상태 변경 함수들 */
extern void td_set_runstate(struct thread_data *, int);     /* 실행 상태 설정 */
extern int td_bump_runstate(struct thread_data *, int);     /* 실행 상태를 올리고 이전 상태 반환 */
extern void td_restore_runstate(struct thread_data *, int); /* 이전 실행 상태로 복원 */
extern const char *runstate_to_name(int runstate);          /* 실행 상태를 문자열로 변환 */

/*
 * Allow 60 seconds for a job to quit on its own, otherwise reap with
 * a vengeance.
 */
/* job이 스스로 종료할 때까지 300초 대기, 이후 강제 회수 */
#define FIO_REAP_TIMEOUT	300

/*
 * 종료(terminate) 범위 지정
 */
enum {
	TERMINATE_NONE = 0,       /* 종료 안함 */
	TERMINATE_GROUP = 1,      /* 같은 그룹만 종료 */
	TERMINATE_STONEWALL = 2,  /* stonewall 경계까지 종료 */
	TERMINATE_ALL = -1,       /* 모든 job 종료 */
};

extern void fio_terminate_threads(unsigned int, unsigned int); /* 지정된 범위의 스레드 종료 */
extern void fio_mark_td_terminate(struct thread_data *);       /* 특정 thread_data에 종료 마크 */

/*
 * Memory helpers
 */
/* 메모리 관리 헬퍼 함수들 */
extern int __must_check fio_pin_memory(struct thread_data *);  /* 메모리를 물리 RAM에 고정 (mlock) */
extern void fio_unpin_memory(struct thread_data *);            /* 고정된 메모리 해제 */
extern int __must_check allocate_io_mem(struct thread_data *); /* I/O 버퍼 메모리 할당 */
extern void free_io_mem(struct thread_data *);                 /* I/O 버퍼 메모리 해제 */
extern void free_threads_shm(void);                            /* 스레드 공유 메모리 해제 */

/* 내부 사용: 포인터 정렬 매크로 */
#ifdef FIO_INTERNAL
#define PTR_ALIGN(ptr, mask)	\
	(char *) (((uintptr_t) (ptr) + (mask)) & ~(mask))
#endif

/*
 * Reset stats after ramp time completes
 */
/* 램프 시간(워밍업) 완료 후 통계를 리셋 */
/* 워밍업 동안의 불안정한 성능 데이터를 제거하여 정확한 측정 보장 */
extern void reset_all_stats(struct thread_data *);

/*
 * I/O 큐 이벤트 처리 함수
 * I/O 엔진의 queue() 호출 결과를 처리하고, 완료/에러 상태를 관리
 */
extern int io_queue_event(struct thread_data *td, struct io_u *io_u, int *ret,
		   enum fio_ddir ddir, uint64_t *bytes_issued, int from_verify,
		   struct timespec *comp_time);

/*
 * Latency target helpers
 */
/* 레이턴시 타겟 헬퍼 함수들 */
/* latency_target 옵션으로 목표 레이턴시를 설정하면 큐 깊이를 자동 조절 */
extern void lat_target_check(struct thread_data *);  /* 레이턴시가 목표 내인지 확인 */
extern void lat_target_init(struct thread_data *);   /* 레이턴시 타겟 초기화 */
extern void lat_target_reset(struct thread_data *);  /* 레이턴시 타겟 상태 리셋 */

/*
 * Inflight log
 */
/* 진행 중(in-flight) I/O 로그 관리 */
/* 검증 상태 저장 시 미완료 I/O를 추적하기 위해 사용 */
extern void log_inflight(struct thread_data *, struct io_u *);        /* I/O를 진행 중으로 기록 */
extern void invalidate_inflight(struct thread_data *, struct io_u *); /* 진행 중 I/O 무효화 */
extern void clear_inflight(struct thread_data *);                     /* 모든 진행 중 I/O 초기화 */

/*
 * Iterates all threads/processes within all the defined jobs
 * Usage:
 *		for_each_td(var_name_for_td) {
 *			<< bodoy of your loop >>
 *			 Note: internally-scoped loop index availble as __td_index
 *		} end_for_each_td()
 */
/*
 * 모든 thread_data를 순회하는 매크로
 * 사용법:
 *   for_each_td(td) {
 *       // td를 사용하는 코드
 *       // __td_index로 현재 인덱스 접근 가능
 *   } end_for_each()
 */
#define for_each_td(td)			\
{								\
	int __td_index;				\
	struct thread_data *(td);	\
	for (__td_index = 0, (td) = &segments[0].threads[0];\
		__td_index < (int) thread_number; __td_index++, (td) = tnumber_to_td(__td_index))

/* 인덱스만으로 순회하는 매크로 (thread_data 포인터 불필요 시) */
#define for_each_td_index()	    \
{								\
	int __td_index;				\
	for (__td_index = 0; __td_index < (int) thread_number; __td_index++)
#define	end_for_each()	}

/*
 * 특정 thread_data의 모든 파일을 순회하는 매크로
 * td: thread_data 포인터, f: fio_file 포인터 변수, i: 인덱스 변수
 */
#define for_each_file(td, f, i)	\
	if ((td)->files_index)						\
		for ((i) = 0, (f) = (td)->files[0];			\
	    	 (i) < (td)->o.nr_files && ((f) = (td)->files[i]) != NULL; \
		 (i)++)

/*
 * fio_offset_overlap_risk: 오프셋 겹침 위험이 있는지 확인
 * norandommap이나 softrandommap이 설정되었거나,
 * 순차 오프셋 추가/반복이 있는 경우 겹침 위험 있음
 */
static inline bool fio_offset_overlap_risk(struct thread_data *td)
{
	if (td->o.norandommap || td->o.softrandommap ||
	    td->o.ddir_seq_add || (td->o.ddir_seq_nr > 1))
		return true;

	return false;
}

/*
 * fio_fill_issue_time: I/O 발행 시각을 기록해야 하는지 판단
 * I/O 로그 재생 중이거나 레이턴시/대역폭 측정이 활성화된 경우 필요
 */
static inline bool fio_fill_issue_time(struct thread_data *td)
{
	if (td->o.read_iolog_file ||
	    !td->o.disable_clat || !td->o.disable_slat || !td->o.disable_bw)
		return true;

	return false;
}

/*
 * option_check_rate: 특정 방향의 속도 제한 옵션이 설정되었는지 확인
 * rate, ratemin, rate_iops, rate_iops_min 중 하나라도 설정되면 true
 */
static inline bool option_check_rate(struct thread_data *td, enum fio_ddir ddir)
{
	struct thread_options *o = &td->o;

	/*
	 * If some rate setting was given, we need to check it
	 */
	/* 속도 제한 설정이 있으면 체크 필요 */
	if (o->rate[ddir] || o->ratemin[ddir] || o->rate_iops[ddir] ||
	    o->rate_iops_min[ddir])
		return true;

	return false;
}

/*
 * should_check_rate: 속도 체크가 필요한지 확인 (TD_F_CHECK_RATE 플래그)
 */
static inline bool should_check_rate(struct thread_data *td)
{
	return (td->flags & TD_F_CHECK_RATE) != 0;
}

/*
 * td_max_bs: 모든 방향(R/W/T)에서 최대 블록 크기를 반환
 */
static inline unsigned long long td_max_bs(struct thread_data *td)
{
	unsigned long long max_bs;

	max_bs = max(td->o.max_bs[DDIR_READ], td->o.max_bs[DDIR_WRITE]);
	return max(td->o.max_bs[DDIR_TRIM], max_bs);
}

/*
 * td_min_bs: 모든 방향(R/W/T)에서 최소 블록 크기를 반환
 */
static inline unsigned long long td_min_bs(struct thread_data *td)
{
	unsigned long long min_bs;

	min_bs = min(td->o.min_bs[DDIR_READ], td->o.min_bs[DDIR_WRITE]);
	return min(td->o.min_bs[DDIR_TRIM], min_bs);
}

/*
 * td_async_processing: 비동기 처리(잠금 필요) 여부 확인
 * verify 스레드가 있으면 io_u 풀 접근 시 뮤텍스 필요
 */
static inline bool td_async_processing(struct thread_data *td)
{
	return (td->flags & TD_F_NEED_LOCK) != 0;
}

/*
 * td_offload_overlap: 오프로드 모드에서 겹침 직렬화가 활성화되었는지 확인
 * serialize_overlap=1 + io_submit_mode=offload 조합 시
 */
static inline bool td_offload_overlap(struct thread_data *td)
{
	return td->o.serialize_overlap && td->o.io_submit_mode == IO_MODE_OFFLOAD;
}

/*
 * We currently only need to do locking if we have verifier threads
 * accessing our internal structures too
 */
/*
 * 현재는 검증 스레드가 내부 구조에 접근할 때만 잠금이 필요함
 * io_u 풀의 뮤텍스 잠금/해제 헬퍼
 */
static inline void __td_io_u_lock(struct thread_data *td)
{
	pthread_mutex_lock(&td->io_u_lock);
}

static inline void __td_io_u_unlock(struct thread_data *td)
{
	pthread_mutex_unlock(&td->io_u_lock);
}

/*
 * td_io_u_free_notify: io_u가 반환되었음을 대기 중인 스레드에 알림
 * 비동기 처리 모드에서만 시그널 전송 (오버헤드 최소화)
 */
static inline void td_io_u_free_notify(struct thread_data *td)
{
	if (td_async_processing(td))
		pthread_cond_signal(&td->free_cond);
}

/*
 * td_flags_clear: thread_data의 플래그를 원자적으로 해제
 * 비동기 처리 시 __sync_fetch_and_and로 원자적 연산 수행
 * 단일 스레드 시 일반 비트 연산으로 최적화
 */
static inline void td_flags_clear(struct thread_data *td, unsigned int *flags,
				  unsigned int value)
{
	if (!td_async_processing(td))
		*flags &= ~value;
	else
		__sync_fetch_and_and(flags, ~value);
}

/*
 * td_flags_set: thread_data의 플래그를 원자적으로 설정
 * 비동기 처리 시 __sync_fetch_and_or로 원자적 연산 수행
 */
static inline void td_flags_set(struct thread_data *td, unsigned int *flags,
				unsigned int value)
{
	if (!td_async_processing(td))
		*flags |= value;
	else
		__sync_fetch_and_or(flags, value);
}

/* 아키텍처/OS 이름을 문자열로 반환하는 함수 */
extern const char *fio_get_arch_string(int); /* 아키텍처명 (x86_64, aarch64 등) */
extern const char *fio_get_os_string(int);   /* OS명 (linux, freebsd 등) */

/*
 * 출력 형식 비트 플래그 및 인덱스
 * FIO_OUTPUT_* 값들은 비트마스크로, 여러 형식을 동시에 활성화 가능
 * 예: output_format = FIO_OUTPUT_JSON | FIO_OUTPUT_NORMAL
 */
enum {
	__FIO_OUTPUT_TERSE	= 0,  /* terse(간결) 형식 인덱스 - 스크립트 파싱용 */
	__FIO_OUTPUT_JSON	= 1,  /* JSON 형식 인덱스 */
	__FIO_OUTPUT_NORMAL	= 2,  /* 일반(사람이 읽기 좋은) 형식 인덱스 */
        __FIO_OUTPUT_JSON_PLUS  = 3,  /* JSON+ 형식 인덱스 (레이턴시 퍼센타일 포함) */
	FIO_OUTPUT_NR		= 4,  /* 출력 형식 수 */

	FIO_OUTPUT_TERSE	= 1U << __FIO_OUTPUT_TERSE,     /* terse 형식 비트마스크 */
	FIO_OUTPUT_JSON		= 1U << __FIO_OUTPUT_JSON,      /* JSON 형식 비트마스크 */
	FIO_OUTPUT_NORMAL	= 1U << __FIO_OUTPUT_NORMAL,    /* 일반 형식 비트마스크 */
	FIO_OUTPUT_JSON_PLUS    = 1U << __FIO_OUTPUT_JSON_PLUS, /* JSON+ 형식 비트마스크 */
};

/*
 * 랜덤 분포 유형
 * random_distribution 옵션으로 I/O 오프셋의 확률 분포를 제어
 */
enum {
	FIO_RAND_DIST_RANDOM	= 0, /* 균등 분포 (기본값) */
	FIO_RAND_DIST_ZIPF,         /* Zipf 분포 (소수 영역에 I/O 집중, 핫스팟 시뮬레이션) */
	FIO_RAND_DIST_PARETO,       /* Pareto 분포 (80/20 법칙과 유사) */
	FIO_RAND_DIST_GAUSS,        /* 가우시안(정규) 분포 (중심 부근에 집중) */
	FIO_RAND_DIST_ZONED,        /* 존 분할 분포 (사용자 정의 영역별 비율, 비율 기반) */
	FIO_RAND_DIST_ZONED_ABS,    /* 존 분할 분포 (사용자 정의 영역별 비율, 절대값 기반) */
};

/* 기본 Zipf/Pareto 분포 매개변수 */
#define FIO_DEF_ZIPF		1.1 /* 기본 Zipf theta 값 (클수록 편향 심함) */
#define FIO_DEF_PARETO		0.2 /* 기본 Pareto h 값 */

/*
 * 난수 생성기 유형
 * randrepeat, lfsr 등의 옵션으로 선택
 */
enum {
	FIO_RAND_GEN_TAUSWORTHE = 0,   /* Tausworthe 32비트 PRNG (기본값) */
	FIO_RAND_GEN_LFSR,             /* LFSR (Linear Feedback Shift Register) */
	FIO_RAND_GEN_TAUSWORTHE64,     /* Tausworthe 64비트 PRNG (더 긴 주기) */
};

/*
 * CPU 공유 모드
 * cpus_allowed 옵션과 함께 사용하여 CPU 할당 방식 결정
 */
enum {
	FIO_CPUS_SHARED		= 0, /* 공유: 모든 job이 같은 CPU 집합 사용 */
	FIO_CPUS_SPLIT,              /* 분할: CPU를 job별로 나누어 할당 */
};

/* 트리거 관련 함수 */
extern void exec_trigger(const char *);  /* 트리거 명령 실행 */
extern void check_trigger_file(void);    /* 트리거 파일 존재 여부 확인 */

/* I/O 겹침(overlap) 검사 */
extern bool in_flight_overlap(struct io_u_queue *q, struct io_u *io_u); /* 진행 중인 I/O와 겹치는지 확인 */
extern pthread_mutex_t overlap_check; /* 겹침 검사 동기화 뮤텍스 */

/*
 * fio_memalign: 정렬된 메모리 할당
 * shared=true이면 공유 메모리(smalloc), false이면 일반 메모리(malloc) 사용
 */
static inline void *fio_memalign(size_t alignment, size_t size, bool shared)
{
	return __fio_memalign(alignment, size, shared ? smalloc : malloc);
}

/*
 * fio_memfree: 정렬된 메모리 해제
 * shared=true이면 sfree, false이면 free 사용
 */
static inline void fio_memfree(void *ptr, size_t size, bool shared)
{
	return __fio_memfree(ptr, size, shared ? sfree : free);
}

#endif
