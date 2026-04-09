/*
 * [한국어] stat.h - fio 통계 수집/출력 시스템 헤더
 *
 * 이 파일은 fio의 성능 측정 및 통계 보고 시스템을 정의한다.
 * 주요 내용:
 *   1) group_run_stats   - 잡 그룹 단위의 실행 통계 (집계 대역폭 등)
 *   2) thread_stat       - 개별 스레드의 상세 통계 (레이턴시, IOPS, 대역폭 등)
 *   3) jobs_eta          - 실행 중 ETA(진행률) 표시를 위한 구조체
 *   4) 퍼센타일 버킷     - 로그 스케일 히스토그램으로 레이턴시 퍼센타일 계산
 *   5) 통계 API 함수     - 샘플 추가, 통계 출력, 리셋 등
 *
 * 퍼센타일 계산 원리:
 *   레이턴시 값을 로그 스케일 버킷에 분류하여 메모리를 절약하면서도
 *   정확한 퍼센타일(p50, p99 등)을 계산한다.
 *   MSB(최상위 비트)로 그룹을 결정하고, 다음 M비트로 버킷 인덱스를 결정한다.
 *   오차 한계: 1/2^(M+1) ≈ 0.78% (M=6일 때)
 */
#ifndef FIO_STAT_H
#define FIO_STAT_H

#include "iolog.h"             /* I/O 로그 구조체 */
#include "lib/output_buffer.h" /* 출력 버퍼 */
#include "diskutil.h"          /* 디스크 유틸리티 통계 */
#include "json.h"              /* JSON 출력 */

/*
 * [한국어] 잡 그룹 단위의 실행 통계
 * group_reporting 옵션이 켜진 잡들의 결과를 집계한다.
 * 서버→클라이언트 전송을 위해 packed 속성을 가진다.
 */
struct group_run_stats {
	uint64_t max_run[DDIR_RWDIR_CNT], min_run[DDIR_RWDIR_CNT]; /* 최대/최소 실행 시간 (ms) */
	uint64_t max_bw[DDIR_RWDIR_CNT], min_bw[DDIR_RWDIR_CNT];   /* 최대/최소 대역폭 (KB/s) */
	uint64_t iobytes[DDIR_RWDIR_CNT];   /* 방향별 총 I/O 바이트 */
	uint64_t agg[DDIR_RWDIR_CNT];       /* 집계 대역폭 (KB/s) */
	uint32_t kb_base;                    /* 1024 또는 1000 */
	uint32_t unit_base;                  /* 단위 기준 (1=비트, 8=바이트) */
	uint32_t sig_figs;                   /* 유효 자릿수 */
	uint32_t groupid;                    /* 그룹 ID */
	uint32_t unified_rw_rep;             /* 읽기/쓰기 통합 보고 모드 */
} __attribute__((packed));

/*
 * [한국어] I/O 깊이 분포 통계를 위한 버킷 수
 * 7개 레벨: 1, 2, 4, 8, 16, 32, >=64
 */
#define FIO_IO_U_MAP_NR	7
#define FIO_IO_U_LAT_N_NR 10  /* 나노초 레이턴시 분포 버킷 수 */
#define FIO_IO_U_LAT_U_NR 10  /* 마이크로초 레이턴시 분포 버킷 수 */
#define FIO_IO_U_LAT_M_NR 12  /* 밀리초 레이턴시 분포 버킷 수 */

/*
 * [한국어] 퍼센타일 계산을 위한 로그 스케일 히스토그램 상수
 *
 * PLAT_BITS=6: 각 그룹 내 64개 버킷 → 오차 < 0.78%
 * PLAT_GROUP_NR=29: 29개 그룹 → 최대 2^34 나노초(~17초)까지 추적
 * PLAT_NR=29*64=1856: 전체 버킷 수
 */
#define FIO_IO_U_PLAT_BITS 6                                    /* 버킷 인덱스 비트 수 (M) */
#define FIO_IO_U_PLAT_VAL (1 << FIO_IO_U_PLAT_BITS)            /* 그룹당 버킷 수 (64) */
#define FIO_IO_U_PLAT_GROUP_NR 29                               /* 총 그룹 수 */
#define FIO_IO_U_PLAT_NR (FIO_IO_U_PLAT_GROUP_NR * FIO_IO_U_PLAT_VAL) /* 전체 버킷 수 (1856) */
#define FIO_IO_U_LIST_MAX_LEN 20  /* 퍼센타일 목록의 최대 항목 수 (기본+사용자 지정) */

/*
 * Aggregate latency samples for reporting percentile(s).
 *
 * EXECUTIVE SUMMARY
 *
 * FIO_IO_U_PLAT_BITS determines the maximum statistical error on the
 * value of resulting percentiles. The error will be approximately
 * 1/2^(FIO_IO_U_PLAT_BITS+1) of the value.
 *
 * FIO_IO_U_PLAT_GROUP_NR and FIO_IO_U_PLAT_BITS determine the maximum
 * range being tracked for latency samples. The maximum value tracked
 * accurately will be 2^(GROUP_NR + PLAT_BITS - 1) nanoseconds.
 *
 * FIO_IO_U_PLAT_GROUP_NR and FIO_IO_U_PLAT_BITS determine the memory
 * requirement of storing those aggregate counts. The memory used will
 * be (FIO_IO_U_PLAT_GROUP_NR * 2^FIO_IO_U_PLAT_BITS) * sizeof(uint64_t)
 * bytes.
 *
 * FIO_IO_U_PLAT_NR is the total number of buckets.
 *
 * DETAILS
 *
 * Suppose the lat varies from 0 to 999 (usec), the straightforward
 * method is to keep an array of (999 + 1) buckets, in which a counter
 * keeps the count of samples which fall in the bucket, e.g.,
 * {[0],[1],...,[999]}. However this consumes a huge amount of space,
 * and can be avoided if an approximation is acceptable.
 *
 * One such method is to let the range of the bucket to be greater
 * than one. This method has low accuracy when the value is small. For
 * example, let the buckets be {[0,99],[100,199],...,[900,999]}, and
 * the represented value of each bucket be the mean of the range. Then
 * a value 0 has a round-off error of 49.5. To improve on this, we
 * use buckets with non-uniform ranges, while bounding the error of
 * each bucket within a ratio of the sample value. A simple example
 * would be when error_bound = 0.005, buckets are {
 * {[0],[1],...,[99]}, {[100,101],[102,103],...,[198,199]},..,
 * {[900,909],[910,919]...}  }. The total range is partitioned into
 * groups with different ranges, then buckets with uniform ranges. An
 * upper bound of the error is (range_of_bucket/2)/value_of_bucket
 *
 * For better efficiency, we implement this using base two. We group
 * samples by their Most Significant Bit (MSB), extract the next M bit
 * of them as an index within the group, and discard the rest of the
 * bits.
 *
 * E.g., assume a sample 'x' whose MSB is bit n (starting from bit 0),
 * and use M bit for indexing
 *
 *        | n |    M bits   | bit (n-M-1) ... bit 0 |
 *
 * Because x is at least 2^n, and bit 0 to bit (n-M-1) is at most
 * (2^(n-M) - 1), discarding bit 0 to (n-M-1) makes the round-off
 * error
 *
 *           2^(n-M)-1    2^(n-M)    1
 *      e <= --------- <= ------- = ---
 *             2^n          2^n     2^M
 *
 * Furthermore, we use "mean" of the range to represent the bucket,
 * the error e can be lowered by half to 1 / 2^(M+1). By using M bits
 * as the index, each group must contains 2^M buckets.
 *
 * E.g. Let M (FIO_IO_U_PLAT_BITS) be 6
 *      Error bound is 1/2^(6+1) = 0.0078125 (< 1%)
 *
 *	Group	MSB	#discarded	range of		#buckets
 *			error_bits	value
 *	----------------------------------------------------------------
 *	0*	0~5	0		[0,63]			64
 *	1*	6	0		[64,127]		64
 *	2	7	1		[128,255]		64
 *	3	8	2		[256,511]		64
 *	4	9	3		[512,1023]		64
 *	...	...	...		[...,...]		...
 *	28	33	27		[8589934592,+inf]**	64
 *
 *  * Special cases: when n < (M-1) or when n == (M-1), in both cases,
 *    the value cannot be rounded off. Use all bits of the sample as
 *    index.
 *
 *  ** If a sample's MSB is greater than 33, it will be counted as 33.
 */

/*
 * [한국어] 블록 정보 — 트림 사이클 추적
 * 각 블록의 상태(쓰기/트림/실패)와 트림 횟수를 32비트에 인코딩한다.
 * 상위 3비트: 상태, 하위 29비트: 트림 횟수
 */
#define MAX_NR_BLOCK_INFOS	8192     /* 추적 가능한 최대 블록 수 */
#define BLOCK_INFO_STATE_SHIFT	29       /* 상태 비트 시작 위치 */
#define BLOCK_INFO_TRIMS(block_info)	\
	((block_info) & ((1 << BLOCK_INFO_STATE_SHIFT) - 1))  /* 트림 횟수 추출 */
#define BLOCK_INFO_STATE(block_info)		\
	((block_info) >> BLOCK_INFO_STATE_SHIFT)               /* 상태 추출 */
#define BLOCK_INFO(state, trim_cycles)	\
	((trim_cycles) | ((unsigned int) (state) << BLOCK_INFO_STATE_SHIFT))
#define BLOCK_INFO_SET_STATE(block_info, state)	\
	BLOCK_INFO(state, BLOCK_INFO_TRIMS(block_info))
enum block_info_state {
	BLOCK_STATE_UNINIT,         /* 초기화 안 됨 */
	BLOCK_STATE_TRIMMED,        /* 트림됨 */
	BLOCK_STATE_WRITTEN,        /* 쓰기됨 */
	BLOCK_STATE_TRIM_FAILURE,   /* 트림 실패 */
	BLOCK_STATE_WRITE_FAILURE,  /* 쓰기 실패 */
	BLOCK_STATE_COUNT,          /* 상태 수 (센티넬) */
};

#define FIO_JOBNAME_SIZE	128  /* 잡 이름 최대 길이 */
#define FIO_JOBDESC_SIZE	256  /* 잡 설명 최대 길이 */
#define FIO_VERROR_SIZE		128  /* 검증 에러 메시지 최대 길이 */
#define UNIFIED_SPLIT		0    /* 읽기/쓰기 통계를 분리 보고 */
#define UNIFIED_MIXED		1    /* 읽기/쓰기 통계를 혼합 보고 */
#define UNIFIED_BOTH		2    /* 분리 + 혼합 모두 보고 */

/* [한국어] 레이턴시 종류 열거형 */
enum fio_lat {
	FIO_SLAT = 0,    /* 제출 레이턴시 (submission latency) */
	FIO_CLAT,        /* 완료 레이턴시 (completion latency) */
	FIO_LAT,         /* 전체 레이턴시 (total = slat + clat) */

	FIO_LAT_CNT = 3,
};

/*
 * [한국어] 우선순위별 완료 레이턴시 통계
 * cmdprio 옵션으로 I/O 우선순위를 다르게 설정했을 때,
 * 각 우선순위별로 별도의 퍼센타일 버킷과 통계를 유지한다.
 */
struct clat_prio_stat {
	uint64_t io_u_plat[FIO_IO_U_PLAT_NR];  /* 퍼센타일 히스토그램 버킷 */
	struct io_stat clat_stat;                /* 완료 레이턴시 기본 통계 */
	uint32_t ioprio;                         /* I/O 우선순위 값 */
};

/*
 * [한국어] 개별 스레드의 상세 통계 구조체
 *
 * fio 실행 결과의 핵심 데이터 구조로, 다음 정보를 포함한다:
 *   - 레이턴시 통계: 제출(slat), 완료(clat), 전체(lat) — 평균, 최소, 최대, 표준편차
 *   - 대역폭/IOPS 통계: 시간에 따른 변동 통계
 *   - 퍼센타일 히스토그램: 로그 스케일 버킷으로 p50, p99 등 계산
 *   - I/O 깊이 분포: 큐에 몇 개씩 쌓였는지 분포
 *   - 시스템 사용량: CPU 시간, 컨텍스트 스위치, 페이지 폴트
 *   - Steady State: 안정 상태 감지 데이터
 *
 * 네트워크 전송을 위해 packed 속성을 가진다.
 */
struct thread_stat {
	char name[FIO_JOBNAME_SIZE];        /* 잡 이름 */
	char verror[FIO_VERROR_SIZE];       /* 검증 에러 메시지 */
	uint32_t error;                     /* 에러 코드 */
	uint32_t thread_number;             /* 스레드 번호 */
	uint32_t groupid;                   /* 그룹 ID */
	uint64_t job_start;                 /* 잡 시작 시각 (ns) */
	uint32_t pid;                       /* 프로세스/스레드 ID */
	char description[FIO_JOBDESC_SIZE]; /* 잡 설명 */
	uint32_t members;                   /* 그룹 내 멤버 수 */
	uint32_t unified_rw_rep;            /* 읽기/쓰기 통합 보고 모드 */
	uint32_t disable_prio_stat;         /* 우선순위별 통계 비활성화 */

	/*
	 * [한국어] 레이턴시 및 대역폭 통계
	 * io_stat 구조체: samples, min_val, max_val, mean, S(표준편차 계산용)
	 */
	struct io_stat sync_stat __attribute__((aligned(8))); /* fsync/fdatasync 통계 */
	struct io_stat clat_stat[DDIR_RWDIR_CNT];  /* 완료 레이턴시 (ns) */
	struct io_stat slat_stat[DDIR_RWDIR_CNT];  /* 제출 레이턴시 (ns) */
	struct io_stat lat_stat[DDIR_RWDIR_CNT];   /* 전체 레이턴시 (ns) */
	struct io_stat bw_stat[DDIR_RWDIR_CNT];    /* 대역폭 (KB/s) */
	struct io_stat iops_stat[DDIR_RWDIR_CNT];  /* IOPS */

	/*
	 * [한국어] 시스템 리소스 사용량 (getrusage)
	 */
	uint64_t usr_time;   /* 사용자 공간 CPU 시간 (ms) */
	uint64_t sys_time;   /* 커널 공간 CPU 시간 (ms) */
	uint64_t ctx;        /* 컨텍스트 스위치 횟수 */
	uint64_t minf, majf; /* 마이너/메이저 페이지 폴트 */

	/*
	 * [한국어] 퍼센타일 및 I/O 깊이 분포 설정
	 */
	uint32_t clat_percentiles;   /* 완료 레이턴시 퍼센타일 활성화 */
	uint32_t lat_percentiles;    /* 전체 레이턴시 퍼센타일 활성화 */
	uint32_t slat_percentiles;   /* 제출 레이턴시 퍼센타일 활성화 */
	uint32_t pad;
	uint64_t percentile_precision; /* 퍼센타일 정밀도 (소수점 자릿수) */
	fio_fp64_t percentile_list[FIO_IO_U_LIST_MAX_LEN]; /* 보고할 퍼센타일 목록 */

	/* [한국어] I/O 깊이 및 레이턴시 분포 히스토그램 */
	uint64_t io_u_map[FIO_IO_U_MAP_NR];       /* I/O 깊이 분포 (1,2,4,8,16,32,64+) */
	uint64_t io_u_submit[FIO_IO_U_MAP_NR];    /* 제출 배치 크기 분포 */
	uint64_t io_u_complete[FIO_IO_U_MAP_NR];  /* 완료 배치 크기 분포 */
	uint64_t io_u_lat_n[FIO_IO_U_LAT_N_NR];   /* 나노초 레이턴시 분포 */
	uint64_t io_u_lat_u[FIO_IO_U_LAT_U_NR];   /* 마이크로초 레이턴시 분포 */
	uint64_t io_u_lat_m[FIO_IO_U_LAT_M_NR];   /* 밀리초 레이턴시 분포 */
	uint64_t io_u_plat[FIO_LAT_CNT][DDIR_RWDIR_CNT][FIO_IO_U_PLAT_NR]; /* 퍼센타일 버킷 [slat/clat/lat][읽기/쓰기/트림] */
	uint64_t io_u_sync_plat[FIO_IO_U_PLAT_NR]; /* sync 작업 퍼센타일 버킷 */

	/* [한국어] I/O 카운터 */
	uint64_t total_io_u[DDIR_RWDIR_SYNC_CNT]; /* 방향별 총 I/O 수 */
	uint64_t short_io_u[DDIR_RWDIR_CNT];      /* 짧은(불완전) I/O 수 */
	uint64_t drop_io_u[DDIR_RWDIR_CNT];       /* 드롭된 I/O 수 */
	uint64_t total_submit;                     /* 총 제출 수 */
	uint64_t total_complete;                   /* 총 완료 수 */

	/* [한국어] 처리량 및 실행 시간 */
	uint64_t io_bytes[DDIR_RWDIR_CNT];         /* 방향별 총 I/O 바이트 */
	uint64_t runtime[DDIR_RWDIR_CNT];          /* 방향별 실행 시간 (ms) */
	uint64_t total_run_time;                   /* 전체 실행 시간 (ms) */

	/* [한국어] I/O 에러 통계 */
	union {
		uint16_t continue_on_error;  /* 에러 시 계속 실행 플래그 */
		uint32_t pad2;
	};
	uint32_t first_error;          /* 첫 번째 에러 코드 */
	uint64_t total_err_count;      /* 총 에러 횟수 */

	/* [한국어] 블록 상태 추적 (트림 검증용) */
	uint64_t nr_block_infos;                       /* 추적 중인 블록 수 */
	uint32_t block_infos[MAX_NR_BLOCK_INFOS];      /* 블록별 상태+트림 횟수 */

	uint32_t kb_base;              /* 단위 기준 (1024 또는 1000) */
	uint32_t unit_base;            /* 1=비트, 8=바이트 */

	/* [한국어] ZBD(Zoned Block Device) 통계 */
	uint64_t nr_zone_resets;       /* 존 리셋 횟수 */
	uint16_t count_zone_resets;    /* 존 리셋 카운팅 활성화 플래그 */
	uint16_t pad3;

	/* [한국어] 레이턴시 목표 자동 탐색 결과 */
	uint32_t latency_depth;        /* 목표 레이턴시를 만족하는 최대 큐 깊이 */
	uint64_t latency_target;       /* 목표 레이턴시 (us) */
	fio_fp64_t latency_percentile; /* 목표 퍼센타일 */
	uint64_t latency_window;       /* 측정 윈도우 (us) */

	uint32_t sig_figs;             /* 유효 자릿수 */

	/* [한국어] Steady State 감지 데이터 */
	uint64_t ss_dur;               /* steady state 감지 기간 */
	uint32_t ss_state;             /* 현재 steady state 상태 */
	uint32_t ss_head;              /* 순환 버퍼 헤드 인덱스 */

	fio_fp64_t ss_limit;           /* steady state 판단 한계값 */
	fio_fp64_t ss_slope;           /* 데이터 기울기 */
	fio_fp64_t ss_deviation;       /* 데이터 편차 */
	fio_fp64_t ss_criterion;       /* 판단 기준값 */

	uint32_t ioprio;               /* I/O 우선순위 (td->ioprio 미러) */

	union {
		uint64_t *ss_iops_data;
		/*
		 * For FIO_NET_CMD_TS, the pointed to data will temporarily
		 * be stored at this offset from the start of the payload.
		 */
		uint64_t ss_iops_data_offset;
		uint64_t pad4;
	};

	union {
		uint64_t *ss_bw_data;
		/*
		 * For FIO_NET_CMD_TS, the pointed to data will temporarily
		 * be stored at this offset from the start of the payload.
		 */
		uint64_t ss_bw_data_offset;
		uint64_t pad5;
	};

	union {
		uint64_t *ss_lat_data;
		/*
		 * For FIO_NET_CMD_TS, the pointed to data will temporarily
		 * be stored at this offset from the start of the payload.
		 */
		uint64_t ss_lat_data_offset;
		uint64_t pad5b;
	};

	union {
		struct clat_prio_stat *clat_prio[DDIR_RWDIR_CNT];
		/*
		 * For FIO_NET_CMD_TS, the pointed to data will temporarily
		 * be stored at this offset from the start of the payload.
		 */
		uint64_t clat_prio_offset[DDIR_RWDIR_CNT];
		uint64_t pad6;
	};
	uint32_t nr_clat_prio[DDIR_RWDIR_CNT];

	uint64_t cachehit;             /* 캐시 히트 횟수 */
	uint64_t cachemiss;            /* 캐시 미스 횟수 */
} __attribute__((packed));

/*
 * [한국어] ETA(진행률) 표시를 위한 구조체 매크로
 * 실행 중 상태 표시줄에 보여줄 정보를 담는다:
 *   - 실행/대기/셋업 중인 잡 수
 *   - 방향별 대역폭, IOPS, 속도 제한
 *   - 경과 시간, 남은 시간
 *   - 각 스레드의 상태 문자열 (run_str[])
 */
#define JOBS_ETA {							\
	uint32_t nr_running;						\
	uint32_t nr_ramp;						\
									\
	uint32_t nr_pending;						\
	uint32_t nr_setting_up;						\
									\
	uint64_t m_rate[DDIR_RWDIR_CNT];				\
	uint64_t t_rate[DDIR_RWDIR_CNT];				\
	uint64_t rate[DDIR_RWDIR_CNT];					\
	uint32_t m_iops[DDIR_RWDIR_CNT];				\
	uint32_t t_iops[DDIR_RWDIR_CNT];				\
	uint32_t iops[DDIR_RWDIR_CNT];					\
	uint32_t pad;							\
	uint64_t elapsed_sec;						\
	uint64_t eta_sec;						\
	uint32_t is_pow2;						\
	uint32_t unit_base;						\
									\
	uint32_t sig_figs;						\
									\
	uint32_t files_open;						\
									\
	/*								\
	 * Network 'copy' of run_str[]					\
	 */								\
	uint32_t nr_threads;						\
	uint32_t pad2;							\
	uint8_t run_str[];						\
}

struct jobs_eta JOBS_ETA;
struct jobs_eta_packed JOBS_ETA __attribute__((packed));

/* [한국어] 퍼센타일 버킷 스냅샷 — 로그 간격마다 저장하여 시계열 퍼센타일 출력 */
struct io_u_plat_entry {
	struct flist_head list;                        /* 연결 리스트 노드 */
	uint64_t io_u_plat[FIO_IO_U_PLAT_NR];         /* 버킷 스냅샷 */
};

extern struct fio_sem *stat_sem;  /* 통계 출력 동기화 세마포어 */

/* [한국어] 통계 API 함수들 */
extern struct jobs_eta *get_jobs_eta(bool force, size_t *size);
		/* ETA 데이터 수집 — 모든 스레드의 진행 상황을 집계 */
extern void stat_init(void);    /* 통계 시스템 초기화 */
extern void stat_exit(void);    /* 통계 시스템 정리 */

/* [한국어] 통계 출력 함수들 */
extern struct json_object * show_thread_status(struct thread_stat *ts, struct group_run_stats *rs, struct flist_head *, struct buf_output *);
		/* 개별 스레드 통계 출력 (일반/JSON/terse 형식) */
extern void show_group_stats(const struct group_run_stats *rs, struct buf_output *);
		/* 그룹 집계 통계 출력 */
extern void display_thread_status(struct jobs_eta *je);
		/* ETA 상태 표시줄 출력 */
extern void __show_run_stats(void);
		/* 실행 완료 후 최종 통계 출력 */
extern int __show_running_run_stats(void);
		/* 실행 중 중간 통계 출력 (USR1 시그널 핸들러) */
extern void show_running_run_stats(void);
extern void check_for_running_stats(void);

/* [한국어] 통계 집계 함수들 */
extern void sum_thread_stats(struct thread_stat *dst, const struct thread_stat *src);
		/* 스레드 통계를 dst에 합산 */
extern void sum_group_stats(struct group_run_stats *dst, const struct group_run_stats *src);
		/* 그룹 통계를 dst에 합산 */
extern void init_thread_stat_min_vals(struct thread_stat *ts);
extern void init_thread_stat(struct thread_stat *ts);
extern void init_group_run_stat(struct group_run_stats *gs);
extern void eta_to_str(char *str, unsigned long eta_sec);
		/* 초 단위 ETA를 "HH:MM:SS" 문자열로 변환 */

/* [한국어] 통계 계산 함수들 */
extern bool calc_lat(const struct io_stat *is, unsigned long long *min, unsigned long long *max, double *mean, double *dev);
		/* io_stat에서 min/max/평균/표준편차 계산 */
extern unsigned int calc_clat_percentiles(const uint64_t *io_u_plat, unsigned long long nr, fio_fp64_t *plist, unsigned long long **output, unsigned long long *maxv, unsigned long long *minv);
		/* 히스토그램 버킷에서 퍼센타일 값 계산 */
extern void stat_calc_lat_n(const struct thread_stat *ts, double *io_u_lat);
extern void stat_calc_lat_m(const struct thread_stat *ts, double *io_u_lat);
extern void stat_calc_lat_u(const struct thread_stat *ts, double *io_u_lat);
extern void stat_calc_dist(const uint64_t *map, unsigned long total, double *io_u_dist);
extern void reset_io_stats(struct thread_data *);
		/* I/O 통계 리셋 (레이턴시 목표 자동 탐색 시 사용) */
extern void update_rusage_stat(struct thread_data *);
		/* getrusage()로 시스템 리소스 사용량 업데이트 */
extern void clear_rusage_stat(struct thread_data *);

/* [한국어] 샘플 추가 함수들 — I/O 완료 시 호출되어 통계에 데이터 포인트 추가 */
extern void add_lat_sample(struct thread_data *, enum fio_ddir,
			   unsigned long long, unsigned long long,
			   struct io_u *);       /* 전체 레이턴시 샘플 */
extern void add_clat_sample(struct thread_data *, enum fio_ddir,
			    unsigned long long, unsigned long long,
			    struct io_u *);      /* 완료 레이턴시 샘플 */
extern void add_slat_sample(struct thread_data *, struct io_u *);
		/* 제출 레이턴시 샘플 */
extern void add_agg_sample(union io_sample_data, enum fio_ddir, unsigned long long);
		/* 집계 로그 샘플 */
extern void add_iops_sample(struct thread_data *, struct io_u *,
				unsigned int);   /* IOPS 샘플 */
extern void add_bw_sample(struct thread_data *, struct io_u *,
				unsigned int, unsigned long long); /* 대역폭 샘플 */
extern void add_sync_clat_sample(struct thread_stat *ts,
				unsigned long long nsec); /* sync 완료 레이턴시 */
extern int calc_log_samples(void);
extern void free_clat_prio_stats(struct thread_stat *);
extern int alloc_clat_prio_stat_ddir(struct thread_stat *, enum fio_ddir, int);

/* [한국어] 디스크 유틸리티 통계 출력 */
extern void print_disk_util(const struct disk_util_stat *, const struct disk_util_agg *, int terse, struct buf_output *);
extern void json_array_add_disk_util(const struct disk_util_stat *dus,
				     const struct disk_util_agg *agg, struct json_array *parent);

extern struct io_log *agg_io_log[DDIR_RWDIR_CNT];
extern bool write_bw_log;

/* [한국어] 레이턴시 단위 자동 변환 — 값이 충분히 크면 상위 단위로 변환 */
static inline bool nsec_to_usec(unsigned long long *min,
				unsigned long long *max, double *mean,
				double *dev)
{
	if (*min > 2000 && *max > 99999 && *dev > 1000.0) {
		*min /= 1000;
		*max /= 1000;
		*mean /= 1000.0;
		*dev /= 1000.0;
		return true;
	}

	return false;
}

static inline bool nsec_to_msec(unsigned long long *min,
				unsigned long long *max, double *mean,
				double *dev)
{
	if (*min > 2000000 && *max > 99999999ULL && *dev > 1000000.0) {
		*min /= 1000000;
		*max /= 1000000;
		*mean /= 1000000.0;
		*dev /= 1000000.0;
		return true;
	}

	return false;
}

/*
 * [한국어] 스레드 상태 문자열 크기
 * 최악의 경우 1개 스레드당 5자(축약 표현)이므로 nr*5 할당
 */
#define __THREAD_RUNSTR_SZ(nr)	((nr) * 5)
#define THREAD_RUNSTR_SZ	__THREAD_RUNSTR_SZ(thread_number)

uint32_t *io_u_block_info(struct thread_data *td, struct io_u *io_u);

#endif
