/*
 * [한국어] thread_options.h - fio 작업(job) 옵션 구조체 정의
 *
 * 이 파일은 fio에서 각 작업(job)의 모든 설정을 저장하는 구조체를 정의한다.
 * 주요 구조체:
 *   - thread_options     : 런타임에 사용하는 옵션 구조체 (포인터 포함)
 *   - thread_options_pack: 네트워크 전송용 직렬화 구조체 (고정 크기, packed)
 *
 * 각 필드는 fio 명령줄 또는 job 파일의 옵션에 1:1 대응한다.
 * 예: --bs=4k -> thread_options.bs[], --iodepth=32 -> thread_options.iodepth
 */
#ifndef FIO_THREAD_OPTIONS_H
#define FIO_THREAD_OPTIONS_H

#include "arch/arch.h"
#include "os/os.h"
#include "options.h"
#include "stat.h"
#include "gettime.h"
#include "lib/ieee754.h"
#include "lib/pattern.h"
#include "td_error.h"

/* [한국어] 존(zone) 모드 - zonemode 옵션에 대응 */
enum fio_zone_mode {
	ZONE_MODE_NOT_SPECIFIED	= 0, /* 지정되지 않음 */
	ZONE_MODE_NONE		= 1, /* 존 모드 비활성화 */
	ZONE_MODE_STRIDED	= 2, /* perform I/O in one zone at a time */ /* 한 번에 하나의 존에서 I/O 수행 */
	/* perform I/O across multiple zones simultaneously */
	ZONE_MODE_ZBD		= 3, /* ZBD(Zoned Block Device) 모드 */
};

/*
 * What type of allocation to use for io buffers
 */
/* [한국어] I/O 버퍼 메모리 할당 방식 - mem/iomem 옵션에 대응 */
enum fio_memtype {
	MEM_MALLOC = 0,	/* ordinary malloc */            /* 일반 malloc */
	MEM_SHM,	/* use shared memory segments */         /* 공유 메모리 세그먼트 */
	MEM_SHMHUGE,	/* use shared memory segments with huge pages */ /* 공유 메모리 + 대형 페이지 */
	MEM_MMAP,	/* use anonynomous mmap */               /* 익명 mmap */
	MEM_MMAPHUGE,	/* memory mapped huge file */            /* 대형 페이지 mmap */
	MEM_MMAPSHARED, /* use mmap with shared flag */         /* 공유 플래그 mmap */
	MEM_CUDA_MALLOC,/* use GPU memory */                    /* GPU 메모리 */
};

/*
 * What mode to use for deduped data generation
 */
/* [한국어] 중복제거(dedupe) 데이터 생성 모드 */
enum dedupe_mode {
	DEDUPE_MODE_REPEAT = 0,      /* 반복 패턴으로 중복 데이터 생성 */
	DEDUPE_MODE_WORKING_SET = 1, /* 작업 세트 기반 중복 데이터 생성 */
};

#define ERROR_STR_MAX	128   /* 에러 문자열 최대 길이 */

#define BSSPLIT_MAX	64    /* 블록 크기 분할(bssplit) 최대 항목 수 */
#define ZONESPLIT_MAX	256   /* 존 분할(zonesplit) 최대 항목 수 */

/* [한국어] 분할(split) 파싱 결과를 담는 범용 구조체 */
struct split {
	unsigned int nr;                           /* 분할 항목 수 */
	unsigned long long val1[ZONESPLIT_MAX];    /* 첫 번째 값 배열 */
	unsigned long long val2[ZONESPLIT_MAX];    /* 두 번째 값 배열 */
};

/* [한국어] 우선순위 기반 블록 크기 분할 항목 */
struct split_prio {
	uint64_t bs;     /* 블록 크기 */
	int32_t prio;    /* I/O 우선순위 */
	uint32_t perc;   /* 비율(%) */
};

/* [한국어] 블록 크기 분할(bssplit) 항목 - bssplit 옵션에 대응 */
struct bssplit {
	uint64_t bs;     /* 블록 크기 (바이트) */
	uint32_t perc;   /* 이 블록 크기의 비율(%) */
};

/* [한국어] 존 분할(zonesplit) 항목 - zonesplit 옵션에 대응 */
struct zone_split {
	uint8_t access_perc;  /* 접근 비율(%) */
	uint8_t size_perc;    /* 크기 비율(%) */
	uint8_t pad[6];       /* 패딩 */
	uint64_t size;        /* 존 크기 (바이트) */
};

#define NR_OPTS_SZ	(FIO_MAX_OPTS / (8 * sizeof(uint64_t)))

#define OPT_MAGIC	0x4f50544e  /* 옵션 구조체 유효성 검증용 매직 넘버 ("OPTN") */

/*
 * [한국어] thread_options - fio 작업(job)의 모든 런타임 옵션을 저장하는 핵심 구조체
 *
 * 각 필드는 fio job 파일이나 명령줄 옵션에 대응한다.
 * 포인터 필드는 런타임 전용이며, 네트워크 전송 시 thread_options_pack으로 변환된다.
 */
struct thread_options {
	int magic;                        /* 매직 넘버 - 구조체 유효성 검증 */
	uint64_t set_options[NR_OPTS_SZ]; /* 어떤 옵션이 명시적으로 설정되었는지 비트맵 */
	char *description;                /* description= : 작업 설명 문자열 */
	char *name;                       /* name= : 작업 이름 */
	char *wait_for;                   /* wait_for= : 이 작업 시작 전 대기할 작업 이름 */
	char *directory;                  /* directory= : I/O 대상 디렉토리 */
	char *filename;                   /* filename= : I/O 대상 파일명 */
	char *filename_format;            /* filename_format= : 파일명 포맷 문자열 */
	char *opendir;                    /* opendir= : 열 디렉토리 */
	char *ioengine;                   /* ioengine= : I/O 엔진 이름 (예: libaio, io_uring) */
	char *ioengine_so_path;           /* ioengine 외부 공유 라이브러리 경로 */
	char *mmapfile;                   /* mmapfile= : mmap 대상 파일 */
	enum td_ddir td_ddir;             /* rw= : I/O 방향 (read, write, randread 등) */
	unsigned int rw_seq;              /* rw_sequencer= : 순차/랜덤 시퀀서 */
	unsigned int kb_base;             /* kb_base= : KB 단위 기준 (1000 또는 1024) */
	unsigned int unit_base;           /* unit_base= : 단위 기준 */
	unsigned int ddir_seq_nr;         /* ddir_seq_nr : 순차 I/O 방향 전환 횟수 */
	long long ddir_seq_add;           /* ddir_seq_add : 순차 I/O 방향 전환 시 오프셋 증가량 */
	unsigned int iodepth;             /* iodepth= : 비동기 I/O 큐 깊이 */
	unsigned int iodepth_low;         /* iodepth_low= : I/O 제출 재개 수위 */
	unsigned int iodepth_batch;       /* iodepth_batch= : 한 번에 제출할 I/O 수 */
	unsigned int iodepth_batch_complete_min; /* iodepth_batch_complete_min= : 최소 완료 대기 수 */
	unsigned int iodepth_batch_complete_max; /* iodepth_batch_complete_max= : 최대 완료 대기 수 */
	unsigned int serialize_overlap;   /* serialize_overlap= : 겹치는 I/O 직렬화 */

	unsigned int unique_filename;     /* unique_filename= : 고유 파일명 사용 여부 */

	unsigned long long size;          /* size= : 총 I/O 크기 */
	unsigned long long io_size;       /* io_size= : 실제 수행할 I/O 양 */
	unsigned int size_percent;        /* size_percent : 파일 크기의 백분율 */
	unsigned int size_nz;             /* size의 정규화된 값 */
	unsigned int io_size_percent;     /* io_size의 백분율 */
	unsigned int io_size_nz;          /* io_size의 정규화된 값 */
	unsigned int fill_device;         /* fill_device= : 디바이스 전체를 채울지 여부 */
	unsigned int file_append;         /* file_append= : 파일 끝에 추가 쓰기 */
	unsigned long long file_size_low; /* filesize= : 파일 크기 범위 하한 */
	unsigned long long file_size_high;/* filesize= : 파일 크기 범위 상한 */
	unsigned long long start_offset;  /* offset= : I/O 시작 오프셋 */
	unsigned long long start_offset_align; /* offset_align= : 시작 오프셋 정렬 */
	unsigned int start_offset_nz;     /* offset의 정규화된 값 */

	unsigned long long bs[DDIR_RWDIR_CNT];     /* bs= : 블록 크기 [read, write, trim] */
	unsigned long long ba[DDIR_RWDIR_CNT];     /* blockalign= : 블록 정렬 [read, write, trim] */
	unsigned long long min_bs[DDIR_RWDIR_CNT]; /* bsrange= : 최소 블록 크기 */
	unsigned long long max_bs[DDIR_RWDIR_CNT]; /* bsrange= : 최대 블록 크기 */
	struct bssplit *bssplit[DDIR_RWDIR_CNT];   /* bssplit= : 블록 크기 분할 배열 */
	unsigned int bssplit_nr[DDIR_RWDIR_CNT];   /* bssplit 항목 수 */

	int *ignore_error[ERROR_TYPE_CNT];         /* ignore_error= : 무시할 에러 코드 배열 */
	unsigned int ignore_error_nr[ERROR_TYPE_CNT]; /* 무시할 에러 코드 수 */
	unsigned int error_dump;                   /* error_dump= : 에러 발생 시 덤프 */

	unsigned int nr_files;            /* nrfiles= : 사용할 파일 수 */
	unsigned int open_files;          /* openfiles= : 동시에 열 파일 수 */
	unsigned int filetype;            /* 파일 유형 (일반 파일, 블록 디바이스 등) */
	enum file_lock_mode file_lock_mode; /* file_lock_mode : 파일 잠금 모드 */

	unsigned int odirect;             /* direct= : O_DIRECT 사용 여부 */
	unsigned int oatomic;             /* atomic= : O_ATOMIC 사용 여부 */
	unsigned int invalidate_cache;    /* invalidate= : 캐시 무효화 */
	unsigned int create_serialize;    /* create_serialize= : 파일 생성 직렬화 */
	unsigned int create_fsync;        /* create_fsync= : 파일 생성 후 fsync */
	unsigned int create_on_open;      /* create_on_open= : 열 때 파일 생성 */
	unsigned int create_only;         /* create_only= : 파일 생성만 수행 */
	unsigned int end_fsync;           /* end_fsync= : 작업 종료 시 fsync */
	unsigned int end_syncfs;          /* end_syncfs= : 작업 종료 시 syncfs */
	unsigned int pre_read;            /* pre_read= : 사전 읽기 */
	unsigned int sync_io;             /* sync= : 동기 I/O (O_SYNC) */
	unsigned int write_hint;          /* write_hint= : 쓰기 힌트 */
	unsigned int verify;              /* verify= : 데이터 검증 알고리즘 (md5, crc32 등) */
	unsigned int do_verify;           /* do_verify= : 검증 수행 여부 */
	unsigned int verify_interval;     /* verify_interval= : 검증 간격 (바이트) */
	unsigned int verify_offset;       /* verify_offset= : 검증 헤더 오프셋 */
	char *verify_pattern;             /* verify_pattern= : 검증 패턴 */
	unsigned int verify_pattern_bytes;/* verify_pattern 바이트 수 */
	unsigned int verify_pattern_interval; /* 패턴 반복 간격 */
	struct pattern_fmt verify_fmt[8]; /* 검증 패턴 포맷 */
	unsigned int verify_fmt_sz;       /* 검증 패턴 포맷 크기 */
	unsigned int verify_fatal;        /* verify_fatal= : 검증 실패 시 치명적 에러 */
	unsigned int verify_dump;         /* verify_dump= : 검증 실패 시 데이터 덤프 */
	unsigned int verify_async;        /* verify_async= : 비동기 검증 스레드 수 */
	unsigned long long verify_backlog;/* verify_backlog= : 검증 백로그 크기 */
	unsigned int verify_batch;        /* verify_batch= : 검증 배치 크기 */
	unsigned int experimental_verify; /* experimental_verify= : 실험적 검증 */
	unsigned int verify_state;        /* verify_state= : 검증 상태 추적 */
	unsigned int verify_state_save;   /* verify_state_save= : 검증 상태 저장 */
	unsigned int verify_write_sequence; /* verify_write_sequence= : 쓰기 순서 검증 */
	unsigned int verify_header_seed;  /* verify_header_seed= : 검증 헤더 시드값 */
	unsigned int use_thread;          /* thread= : 프로세스 대신 스레드 사용 */
	unsigned int unlink;              /* unlink= : 작업 후 파일 삭제 */
	unsigned int unlink_each_loop;    /* unlink_each_loop= : 매 루프마다 파일 삭제 */
	unsigned int do_disk_util;        /* disk_util= : 디스크 유틸리티 통계 수집 */
	unsigned int override_sync;       /* fsync/fdatasync 오버라이드 */
	unsigned int rand_repeatable;     /* randrepeat= : 재현 가능한 난수 */
	unsigned long long rand_seed;     /* randseed= : 난수 시드값 */
	unsigned int log_avg_msec;        /* log_avg_msec= : 로그 평균 주기 (밀리초) */
	unsigned int log_hist_msec;       /* log_hist_msec= : 히스토그램 로그 주기 */
	unsigned int log_hist_coarseness; /* log_hist_coarseness= : 히스토그램 정밀도 */
	unsigned int log_max;             /* log_max_value= : 최댓값 로깅 */
	unsigned int log_offset;          /* log_offset= : 오프셋 로깅 */
	unsigned int log_gz;              /* log_compression= : 로그 gzip 압축 */
	unsigned int log_gz_store;        /* log_store_compressed= : 압축된 로그 저장 */
	unsigned int log_alternate_epoch; /* log_alternate_epoch= : 대체 에포크 사용 */
	unsigned int log_alternate_epoch_clock_id; /* 대체 에포크 클럭 ID */
	unsigned int norandommap;         /* norandommap= : 랜덤맵 비활성화 */
	unsigned int softrandommap;       /* softrandommap= : 소프트 랜덤맵 */
	unsigned int sprandom;            /* sprandom= : 구조적 의사 랜덤 */
	unsigned int spr_num_regions;     /* sprandom 영역 수 */
	unsigned long long spr_cache_size;/* sprandom 캐시 크기 */
	fio_fp64_t spr_over_provisioning; /* sprandom 오버 프로비저닝 비율 */
	unsigned int bs_unaligned;        /* bs_unaligned= : 비정렬 블록 크기 허용 */
	unsigned int fsync_on_close;      /* fsync_on_close= : 닫을 때 fsync */
	unsigned int bs_is_seq_rand;      /* 블록 크기가 순차/랜덤별 다른지 여부 */

	unsigned int verify_only;         /* verify_only= : 검증만 수행 (쓰기 없음) */

	unsigned int random_distribution; /* random_distribution= : 랜덤 분포 유형 */
	unsigned int exitall_error;       /* exitall_on_error= : 에러 시 모든 작업 종료 */

	struct zone_split *zone_split[DDIR_RWDIR_CNT]; /* zonesplit= : 존 분할 배열 */
	unsigned int zone_split_nr[DDIR_RWDIR_CNT];    /* 존 분할 항목 수 */

	fio_fp64_t zipf_theta;            /* zipf 분포 세타 값 */
	fio_fp64_t pareto_h;              /* pareto 분포 h 값 */
	fio_fp64_t gauss_dev;             /* 가우시안 분포 표준편차 */
	fio_fp64_t random_center;         /* 랜덤 분포 중심값 */

	unsigned int random_generator;    /* random_generator= : 난수 생성기 유형 */

	unsigned int perc_rand[DDIR_RWDIR_CNT]; /* percentage_random= : 랜덤 I/O 비율(%) */

	unsigned int hugepage_size;       /* hugepage-size= : 대형 페이지 크기 */
	unsigned long long rw_min_bs;     /* 읽기/쓰기 최소 블록 크기 */
	unsigned int fsync_blocks;        /* fsync= : N개 블록마다 fsync */
	unsigned int fdatasync_blocks;    /* fdatasync= : N개 블록마다 fdatasync */
	unsigned int barrier_blocks;      /* barrier= : N개 블록마다 배리어 */
	unsigned long long start_delay;   /* startdelay= : 시작 지연 (마이크로초) */
	unsigned long long start_delay_orig; /* 원래 시작 지연값 */
	unsigned long long start_delay_high; /* startdelay 범위 상한 */
	unsigned long long timeout;       /* runtime/timeout= : 최대 실행 시간 */
	unsigned long long ramp_time;     /* ramp_time= : 워밍업 시간 (통계에서 제외) */
	unsigned long long ramp_size;     /* ramp_size= : 워밍업 동안의 I/O 양 */
	unsigned int ss_state;            /* steadystate= : 정상상태 감지 설정 */
	fio_fp64_t ss_limit;              /* 정상상태 판정 기준 */
	unsigned long long ss_dur;        /* 정상상태 판정 기간 */
	unsigned long long ss_ramp_time;  /* 정상상태 워밍업 시간 */
	unsigned long long ss_check_interval; /* 정상상태 체크 간격 */
	unsigned int overwrite;           /* overwrite= : 덮어쓰기 허용 */
	unsigned int bw_avg_time;         /* bw_avg_time= : 대역폭 평균 시간 (밀리초) */
	unsigned int iops_avg_time;       /* iops_avg_time= : IOPS 평균 시간 (밀리초) */
	unsigned int loops;               /* loops= : 작업 반복 횟수 */
	unsigned long long zone_range;    /* zonerange= : 존 범위 크기 */
	unsigned long long zone_size;     /* zonesize= : 존 크기 */
	unsigned long long zone_capacity; /* zonecapacity= : 존 용량 */
	unsigned long long zone_skip;     /* zoneskip= : 존 건너뛰기 크기 */
	uint32_t zone_skip_nz;            /* zoneskip의 정규화된 값 */
	enum fio_zone_mode zone_mode;     /* zonemode= : 존 모드 */
	unsigned long long lockmem;       /* lockmem= : 잠글 메모리 양 */
	enum fio_memtype mem_type;        /* mem/iomem= : 메모리 할당 유형 */
	unsigned int mem_align;           /* mem_align= : 메모리 정렬 */

	unsigned long long max_latency[DDIR_RWDIR_CNT]; /* max_latency= : 최대 허용 지연시간 */

	unsigned int exit_what;           /* exitall= 또는 exitall_on_error= 동작 */
	unsigned int stonewall;           /* stonewall= : 이전 작업 완료 후 시작 */
	unsigned int new_group;           /* new_group= : 새 리포트 그룹 */
	unsigned int numjobs;             /* numjobs= : 이 작업의 복제 수 */
	os_cpu_mask_t cpumask;            /* cpus_allowed= : CPU 친화도 마스크 */
	os_cpu_mask_t verify_cpumask;     /* verify_cpus_allowed= : 검증 CPU 마스크 */
	os_cpu_mask_t log_gz_cpumask;     /* log_compression_cpus= : 로그 압축 CPU 마스크 */
	unsigned int cpus_allowed_policy; /* cpus_allowed_policy= : CPU 할당 정책 */
	char *numa_cpunodes;              /* numa_cpu_nodes= : NUMA CPU 노드 */
	unsigned short numa_mem_mode;     /* numa_mem_policy= : NUMA 메모리 정책 */
	unsigned int numa_mem_prefer_node;/* NUMA 선호 노드 */
	char *numa_memnodes;              /* numa_mem_nodes= : NUMA 메모리 노드 */
	unsigned int gpu_dev_id;          /* gpu_dev_id= : GPU 디바이스 ID */
	unsigned int start_offset_percent;/* offset_increment의 백분율 */

	unsigned int iolog;               /* iolog 관련 플래그 */
	unsigned int rwmixcycle;          /* rwmixcycle (더 이상 사용 안 함) */
	unsigned int rwmix[DDIR_RWDIR_CNT]; /* rwmixread=/rwmixwrite= : 읽기/쓰기 혼합 비율 */
	char *comm;                       /* 내부 통신 이름 */
	unsigned int nice;                /* nice= : 프로세스 nice 값 */
	unsigned int ioprio;              /* prio= : I/O 우선순위 */
	unsigned int ioprio_class;        /* prioclass= : I/O 우선순위 클래스 */
	unsigned int ioprio_hint;         /* priohint= : I/O 우선순위 힌트 */
	unsigned int file_service_type;   /* file_service_type= : 파일 서비스 유형 */
	unsigned int group_reporting;     /* group_reporting= : 그룹 리포팅 */
	unsigned int stats;               /* stats= : 통계 활성화 */
	unsigned int fadvise_hint;        /* fadvise_hint= : fadvise 힌트 */
	enum fio_fallocate_mode fallocate_mode; /* fallocate= : 사전할당 모드 */
	unsigned int zero_buffers;        /* zero_buffers= : 버퍼를 0으로 초기화 */
	unsigned int refill_buffers;      /* refill_buffers= : 매번 버퍼 재생성 */
	unsigned int scramble_buffers;    /* scramble_buffers= : 버퍼 내용 스크램블 */
	char *buffer_pattern;             /* buffer_pattern= : 버퍼 패턴 */
	unsigned int buffer_pattern_bytes;/* 버퍼 패턴 바이트 수 */
	unsigned int compress_percentage; /* buffer_compress_percentage= : 압축 가능 비율(%) */
	unsigned int compress_chunk;      /* buffer_compress_chunk= : 압축 청크 크기 */
	unsigned int dedupe_percentage;   /* dedupe_percentage= : 중복제거 비율(%) */
	unsigned int dedupe_mode;         /* dedupe_mode= : 중복제거 모드 */
	unsigned int dedupe_working_set_percentage; /* 중복제거 작업 세트 비율 */
	unsigned int dedupe_global;       /* dedupe_global= : 전역 중복제거 */
	unsigned int time_based;          /* time_based= : 시간 기반 실행 */
	unsigned int disable_lat;         /* disable_lat= : 전체 지연시간 비활성화 */
	unsigned int disable_clat;        /* disable_clat= : 완료 지연시간 비활성화 */
	unsigned int disable_slat;        /* disable_slat= : 제출 지연시간 비활성화 */
	unsigned int disable_bw;          /* disable_bw= : 대역폭 측정 비활성화 */
	unsigned int unified_rw_rep;      /* unified_rw_reporting= : 읽기/쓰기 통합 리포팅 */
	unsigned int gtod_reduce;         /* gtod_reduce= : gettimeofday 호출 줄이기 */
	unsigned int gtod_cpu;            /* gtod_cpu= : gettimeofday 전용 CPU */
	unsigned int job_start_clock_id;  /* job_start_clock_id= : 작업 시작 클럭 ID */
	enum fio_cs clocksource;          /* clocksource= : 시간 소스 */
	unsigned int no_stall;            /* no_stall : 스톨 방지 */
	unsigned int trim_percentage;     /* trim_percentage= : trim 비율(%) */
	unsigned int trim_batch;          /* trim_batch= : trim 배치 크기 */
	unsigned int trim_zero;           /* trim_zero= : trim 후 0 검증 */
	unsigned long long trim_backlog;  /* trim_backlog= : trim 백로그 */
	unsigned int clat_percentiles;    /* clat_percentiles= : 완료 지연시간 백분위수 */
	unsigned int slat_percentiles;    /* slat_percentiles= : 제출 지연시간 백분위수 */
	unsigned int lat_percentiles;     /* lat_percentiles= : 전체 지연시간 백분위수 */
	unsigned int percentile_precision;	/* digits after decimal for percentiles */ /* 백분위수 소수점 자릿수 */
	fio_fp64_t percentile_list[FIO_IO_U_LIST_MAX_LEN]; /* percentile_list= : 백분위수 목록 */

	char *read_iolog_file;            /* read_iolog= : 재생할 I/O 로그 파일 */
	bool read_iolog_chunked;          /* read_iolog_chunked= : 청크 단위 로그 읽기 */
	char *write_iolog_file;           /* write_iolog= : I/O 로그 기록 파일 */
	char *merge_blktrace_file;        /* merge_blktrace_file= : blktrace 병합 파일 */
	fio_fp64_t merge_blktrace_scalars[FIO_IO_U_LIST_MAX_LEN]; /* blktrace 스케일 */
	fio_fp64_t merge_blktrace_iters[FIO_IO_U_LIST_MAX_LEN];   /* blktrace 반복 */

	unsigned int write_bw_log;        /* write_bw_log= : 대역폭 로그 기록 */
	unsigned int write_lat_log;       /* write_lat_log= : 지연시간 로그 기록 */
	unsigned int write_iops_log;      /* write_iops_log= : IOPS 로그 기록 */
	unsigned int write_hist_log;      /* write_hist_log= : 히스토그램 로그 기록 */

	char *bw_log_file;                /* 대역폭 로그 파일 경로 */
	char *lat_log_file;               /* 지연시간 로그 파일 경로 */
	char *iops_log_file;              /* IOPS 로그 파일 경로 */
	char *hist_log_file;              /* 히스토그램 로그 파일 경로 */
	char *replay_redirect;            /* replay_redirect= : 재생 리다이렉트 대상 */

	/*
	 * Pre-run and post-run shell
	 */
	/* [한국어] 작업 전/후 실행할 셸 명령 */
	char *exec_prerun;                /* exec_prerun= : 작업 시작 전 실행 명령 */
	char *exec_postrun;               /* exec_postrun= : 작업 종료 후 실행 명령 */

	unsigned int thinkcycles;         /* thinkcycles= : I/O 사이 CPU 사이클 수 */

	unsigned int thinktime;           /* thinktime= : I/O 사이 대기 시간 (마이크로초) */
	unsigned int thinktime_spin;      /* thinktime_spin= : 스핀 대기 시간 */
	unsigned int thinktime_blocks;    /* thinktime_blocks= : N개 블록 후 대기 */
	unsigned int thinktime_blocks_type; /* thinktime_blocks_type= : 블록 카운트 유형 */
	unsigned int thinktime_iotime;    /* thinktime_iotime= : I/O 시간 기반 대기 */

	uint64_t rate[DDIR_RWDIR_CNT];    /* rate= : 대역폭 제한 (바이트/초) */
	uint64_t ratemin[DDIR_RWDIR_CNT]; /* rate_min= : 최소 대역폭 */
	unsigned int ratecycle;           /* rate_cycle= : 속도 체크 주기 */
	unsigned int io_submit_mode;      /* io_submit_mode= : I/O 제출 모드 (inline/offload) */
	unsigned int rate_iops[DDIR_RWDIR_CNT];     /* rate_iops= : IOPS 제한 */
	unsigned int rate_iops_min[DDIR_RWDIR_CNT]; /* rate_iops_min= : 최소 IOPS */
	unsigned int rate_process;        /* rate_process= : 속도 처리 방식 */
	unsigned int rate_ign_think;      /* rate_ignore_thinktime= : 속도 계산 시 thinktime 무시 */

	char *ioscheduler;                /* ioscheduler= : I/O 스케줄러 (예: mq-deadline) */

	/*
	 * I/O Error handling
	 */
	/* [한국어] I/O 에러 처리 */
	enum error_type continue_on_error; /* continue_on_error= : 에러 시 계속 실행 */

	/*
	 * Benchmark profile type
	 */
	/* [한국어] 벤치마크 프로파일 */
	char *profile;                    /* profile= : 벤치마크 프로파일 이름 */

	/*
	 * blkio cgroup support
	 */
	/* [한국어] blkio cgroup 지원 */
	char *cgroup;                     /* cgroup= : cgroup 경로 */
	unsigned int cgroup_weight;       /* cgroup_weight= : cgroup 가중치 */
	unsigned int cgroup_nodelete;     /* cgroup_nodelete= : cgroup 삭제 안 함 */

	unsigned int uid;                 /* uid= : 사용자 ID */
	unsigned int gid;                 /* gid= : 그룹 ID */

	unsigned int offset_increment_percent; /* offset_increment 백분율 */
	unsigned int offset_increment_nz;      /* offset_increment 정규화된 값 */
	unsigned long long offset_increment;   /* offset_increment= : 파일 간 오프셋 증분 */
	unsigned long long number_ios;         /* number_ios= : 수행할 총 I/O 수 */

	unsigned int num_range;           /* num_range= : I/O 범위 수 (멀티 레인지) */

	unsigned int sync_file_range;     /* sync_file_range= : sync_file_range 플래그 */

	unsigned long long latency_target;/* latency_target= : 목표 지연시간 */
	unsigned long long latency_window;/* latency_window= : 지연시간 측정 창 */
	uint32_t latency_run;             /* latency_run= : 지연시간 기반 실행 */
	fio_fp64_t latency_percentile;    /* latency_percentile= : 지연시간 백분위수 */

	/*
	 * flow support
	 */
	/* [한국어] 흐름 제어(flow control) - 작업 간 동기화 */
	int flow_id;                      /* flow_id= : 흐름 ID */
	unsigned int flow;                /* flow= : 흐름 가중치 */
	unsigned int flow_sleep;          /* flow_sleep= : 흐름 대기 시간 */

	unsigned int sig_figs;            /* sig_figs= : 유효 숫자 */

	unsigned block_error_hist;        /* block_error_hist : 블록 에러 히스토그램 */

	unsigned int replay_align;        /* replay_align= : 재생 정렬 */
	unsigned int replay_scale;        /* replay_scale= : 재생 스케일 */
	unsigned int replay_time_scale;   /* replay_time_scale= : 재생 시간 스케일 */
	unsigned int replay_skip;         /* replay_skip= : 재생 건너뛰기 */

	unsigned int per_job_logs;        /* per_job_logs= : 작업별 로그 */

	unsigned int allow_create;        /* allow_file_create= : 파일 생성 허용 */
	unsigned int allow_mounted_write; /* allow_mounted_write= : 마운트된 파일시스템 쓰기 허용 */

	/* Parameters that affect zonemode=zbd */
	/* [한국어] ZBD(Zoned Block Device) 관련 파라미터 */
	unsigned int read_beyond_wp;      /* read_beyond_wp= : 쓰기 포인터 너머 읽기 */
	int max_open_zones;               /* max_open_zones= : 최대 열린 존 수 */
	unsigned int job_max_open_zones;  /* job_max_open_zones= : 작업별 최대 열린 존 수 */
	unsigned int ignore_zone_limits;  /* ignore_zone_limits= : 존 제한 무시 */
	unsigned int recover_zbd_write_error; /* 존 쓰기 에러 복구 */
	unsigned int write_zone_remainder;/* 존 나머지 영역에 쓰기 */
	fio_fp64_t zrt;                   /* zrt= : 존 리셋 임계값 */
	fio_fp64_t zrf;                   /* zrf= : 존 리셋 빈도 */

	/* [한국어] FDP(Flexible Data Placement) 관련 파라미터 */
	unsigned int fdp;                 /* fdp= : FDP 활성화 */
	unsigned int dp_type;             /* dataplacement= : 데이터 배치 유형 */
	unsigned int dp_id_select;        /* dp_id_select= : 배치 핸들 선택 방식 */
	uint16_t dp_ids[FIO_MAX_DP_IDS]; /* fdp_ids= : 배치 핸들 ID 배열 */
	unsigned int dp_nr_ids;           /* 배치 핸들 ID 수 */
	char *dp_scheme_file;             /* dp_scheme= : 배치 스킴 파일 */

	unsigned int log_entries;         /* log_entries= : 로그 항목 수 */
	unsigned int log_prio;            /* log_prio= : 로그에 우선순위 기록 */
	unsigned int log_issue_time;      /* log_issue_time= : 로그에 제출 시간 기록 */
};

#define FIO_TOP_STR_MAX		256  /* 네트워크 전송용 문자열 최대 길이 */

/*
 * [한국어] thread_options_pack - 네트워크 전송용 직렬화 구조체
 *
 * thread_options의 네트워크 전송(클라이언트-서버) 버전.
 * 포인터 대신 고정 크기 배열을 사용하며, __attribute__((packed))로 패딩 제거.
 * convert_thread_options_to_net()으로 직렬화, convert_thread_options_to_cpu()로 역직렬화.
 */
struct thread_options_pack {
	uint64_t set_options[NR_OPTS_SZ];
	uint8_t description[FIO_TOP_STR_MAX];
	uint8_t name[FIO_TOP_STR_MAX];
	uint8_t wait_for[FIO_TOP_STR_MAX];
	uint8_t directory[FIO_TOP_STR_MAX];
	uint8_t filename[FIO_TOP_STR_MAX];
	uint8_t filename_format[FIO_TOP_STR_MAX];
	uint8_t opendir[FIO_TOP_STR_MAX];
	uint8_t ioengine[FIO_TOP_STR_MAX];
	uint8_t mmapfile[FIO_TOP_STR_MAX];
	uint32_t td_ddir;
	uint32_t rw_seq;
	uint32_t kb_base;
	uint32_t unit_base;
	uint32_t ddir_seq_nr;
	uint64_t ddir_seq_add;
	uint32_t iodepth;
	uint32_t iodepth_low;
	uint32_t iodepth_batch;
	uint32_t iodepth_batch_complete_min;
	uint32_t iodepth_batch_complete_max;
	uint32_t serialize_overlap;

	uint64_t size;
	uint64_t io_size;
	uint32_t size_percent;
	uint32_t size_nz;
	uint32_t io_size_percent;
	uint32_t io_size_nz;
	uint32_t fill_device;
	uint32_t file_append;
	uint32_t unique_filename;
	uint64_t file_size_low;
	uint64_t file_size_high;
	uint64_t start_offset;
	uint64_t start_offset_align;
	uint32_t start_offset_nz;

	uint64_t bs[DDIR_RWDIR_CNT];
	uint64_t ba[DDIR_RWDIR_CNT];
	uint64_t min_bs[DDIR_RWDIR_CNT];
	uint64_t max_bs[DDIR_RWDIR_CNT];
	struct bssplit bssplit[DDIR_RWDIR_CNT][BSSPLIT_MAX];
	uint32_t bssplit_nr[DDIR_RWDIR_CNT];

	uint32_t ignore_error[ERROR_TYPE_CNT][ERROR_STR_MAX];
	uint32_t ignore_error_nr[ERROR_TYPE_CNT];
	uint32_t error_dump;

	uint32_t nr_files;
	uint32_t open_files;
	uint32_t filetype;
	uint32_t file_lock_mode;

	uint32_t odirect;
	uint32_t oatomic;
	uint32_t invalidate_cache;
	uint32_t create_serialize;
	uint32_t create_fsync;
	uint32_t create_on_open;
	uint32_t create_only;
	uint32_t end_fsync;
	uint32_t pre_read;
	uint32_t sync_io;
	uint32_t write_hint;
	uint32_t verify;
	uint32_t do_verify;
	uint32_t verify_interval;
	uint32_t verify_offset;
	uint32_t verify_pattern_bytes;
	uint32_t verify_pattern_interval;
	uint32_t verify_fatal;
	uint32_t verify_dump;
	uint32_t verify_async;
	uint64_t verify_backlog;
	uint32_t verify_batch;
	uint32_t experimental_verify;
	uint32_t verify_state;
	uint32_t verify_state_save;
	uint32_t verify_write_sequence;
	uint32_t verify_header_seed;
	uint32_t use_thread;
	uint32_t unlink;
	uint32_t unlink_each_loop;
	uint32_t do_disk_util;
	uint32_t override_sync;
	uint32_t rand_repeatable;
	uint64_t rand_seed;
	uint32_t log_avg_msec;
	uint32_t log_hist_msec;
	uint32_t log_hist_coarseness;
	uint32_t log_max;
	uint32_t log_offset;
	uint32_t log_gz;
	uint32_t log_gz_store;
	uint32_t log_alternate_epoch;
	uint32_t log_alternate_epoch_clock_id;
	uint32_t norandommap;
	uint32_t softrandommap;
	uint32_t sprandom;
	uint32_t spr_num_regions;
	uint64_t spr_cache_size;
	fio_fp64_t spr_over_provisioning;
	uint32_t bs_unaligned;
	uint32_t fsync_on_close;
	uint32_t bs_is_seq_rand;

	uint32_t random_distribution;
	uint32_t exitall_error;

	uint32_t sync_file_range;
	uint32_t end_syncfs;
	uint32_t pad;

	struct zone_split zone_split[DDIR_RWDIR_CNT][ZONESPLIT_MAX];
	uint32_t zone_split_nr[DDIR_RWDIR_CNT];

	fio_fp64_t zipf_theta;
	fio_fp64_t pareto_h;
	fio_fp64_t gauss_dev;
	fio_fp64_t random_center;

	uint32_t random_generator;

	uint32_t perc_rand[DDIR_RWDIR_CNT];

	uint32_t hugepage_size;
	uint64_t rw_min_bs;
	uint32_t fsync_blocks;
	uint32_t fdatasync_blocks;
	uint32_t barrier_blocks;
	uint64_t start_delay;
	uint64_t start_delay_high;
	uint64_t timeout;
	uint64_t ramp_time;
	uint64_t ramp_size;
	uint64_t ss_dur;
	uint64_t ss_ramp_time;
	uint32_t ss_state;
	fio_fp64_t ss_limit;
	uint64_t ss_check_interval;
	uint32_t overwrite;
	uint32_t bw_avg_time;
	uint32_t iops_avg_time;
	uint32_t loops;
	uint64_t zone_range;
	uint64_t zone_size;
	uint64_t zone_capacity;
	uint64_t zone_skip;
	uint64_t lockmem;
	uint32_t zone_skip_nz;
	uint32_t mem_type;
	uint32_t mem_align;

	uint32_t exit_what;
	uint32_t stonewall;
	uint32_t new_group;
	uint32_t numjobs;

	/*
	 * We currently can't convert these, so don't enable them
	 */
#if 0
	uint8_t cpumask[FIO_TOP_STR_MAX];
	uint8_t verify_cpumask[FIO_TOP_STR_MAX];
	uint8_t log_gz_cpumask[FIO_TOP_STR_MAX];
#endif
	uint32_t gpu_dev_id;
	uint32_t start_offset_percent;
	uint32_t cpus_allowed_policy;
	uint32_t iolog;
	uint32_t rwmixcycle;
	uint32_t rwmix[DDIR_RWDIR_CNT];
	uint8_t comm[FIO_TOP_STR_MAX];
	uint32_t nice;
	uint32_t ioprio;
	uint32_t ioprio_class;
	uint32_t ioprio_hint;
	uint32_t file_service_type;
	uint32_t group_reporting;
	uint32_t stats;
	uint32_t fadvise_hint;
	uint32_t fallocate_mode;
	uint32_t zero_buffers;
	uint32_t refill_buffers;
	uint32_t scramble_buffers;
	uint32_t buffer_pattern_bytes;
	uint32_t compress_percentage;
	uint32_t compress_chunk;
	uint32_t dedupe_percentage;
	uint32_t dedupe_mode;
	uint32_t dedupe_working_set_percentage;
	uint32_t dedupe_global;
	uint32_t time_based;
	uint32_t disable_lat;
	uint32_t disable_clat;
	uint32_t disable_slat;
	uint32_t disable_bw;
	uint32_t unified_rw_rep;
	uint32_t gtod_reduce;
	uint32_t gtod_cpu;
	uint32_t job_start_clock_id;
	uint32_t clocksource;
	uint32_t no_stall;
	uint32_t trim_percentage;
	uint32_t trim_batch;
	uint32_t trim_zero;
	uint64_t trim_backlog;
	uint32_t clat_percentiles;
	uint32_t lat_percentiles;
	uint32_t slat_percentiles;
	uint32_t percentile_precision;
	uint32_t pad2;
	fio_fp64_t percentile_list[FIO_IO_U_LIST_MAX_LEN];

	uint8_t read_iolog_file[FIO_TOP_STR_MAX];
	uint8_t write_iolog_file[FIO_TOP_STR_MAX];
	uint8_t merge_blktrace_file[FIO_TOP_STR_MAX];
	fio_fp64_t merge_blktrace_scalars[FIO_IO_U_LIST_MAX_LEN];
	fio_fp64_t merge_blktrace_iters[FIO_IO_U_LIST_MAX_LEN];

	uint32_t write_bw_log;
	uint32_t write_lat_log;
	uint32_t write_iops_log;
	uint32_t write_hist_log;

	uint8_t bw_log_file[FIO_TOP_STR_MAX];
	uint8_t lat_log_file[FIO_TOP_STR_MAX];
	uint8_t iops_log_file[FIO_TOP_STR_MAX];
	uint8_t hist_log_file[FIO_TOP_STR_MAX];
	uint8_t replay_redirect[FIO_TOP_STR_MAX];

	/*
	 * Pre-run and post-run shell
	 */
	uint8_t exec_prerun[FIO_TOP_STR_MAX];
	uint8_t exec_postrun[FIO_TOP_STR_MAX];

	uint32_t thinkcycles;

	uint32_t thinktime;
	uint32_t thinktime_spin;
	uint32_t thinktime_blocks;
	uint32_t thinktime_blocks_type;
	uint32_t thinktime_iotime;

	uint64_t rate[DDIR_RWDIR_CNT];
	uint64_t ratemin[DDIR_RWDIR_CNT];
	uint32_t ratecycle;
	uint32_t io_submit_mode;
	uint32_t rate_iops[DDIR_RWDIR_CNT];
	uint32_t rate_iops_min[DDIR_RWDIR_CNT];
	uint32_t rate_process;
	uint32_t rate_ign_think;

	uint8_t ioscheduler[FIO_TOP_STR_MAX];

	/*
	 * I/O Error handling
	 */
	uint32_t continue_on_error;

	/*
	 * Benchmark profile type
	 */
	uint8_t profile[FIO_TOP_STR_MAX];

	/*
	 * blkio cgroup support
	 */
	uint8_t cgroup[FIO_TOP_STR_MAX];
	uint32_t cgroup_weight;
	uint32_t cgroup_nodelete;

	uint32_t uid;
	uint32_t gid;

	uint32_t offset_increment_percent;
	uint32_t offset_increment_nz;
	uint64_t offset_increment;
	uint64_t number_ios;

	uint64_t latency_target;
	uint64_t latency_window;
	uint64_t max_latency[DDIR_RWDIR_CNT];
	uint32_t latency_run;
	fio_fp64_t latency_percentile;

	/*
	 * flow support
	 */
	int32_t flow_id;
	uint32_t flow;
	uint32_t flow_sleep;

	uint32_t sig_figs;

	uint32_t block_error_hist;

	uint32_t replay_align;
	uint32_t replay_scale;
	uint32_t replay_time_scale;
	uint32_t replay_skip;

	uint32_t per_job_logs;

	uint32_t allow_create;
	uint32_t allow_mounted_write;

	uint32_t zone_mode;
	int32_t max_open_zones;
	uint32_t ignore_zone_limits;
	uint32_t recover_zbd_write_error;
	uint32_t write_zone_remainder;

	uint32_t log_entries;
	uint32_t log_prio;
	uint32_t log_issue_time;

	uint32_t fdp;
	uint32_t dp_type;
	uint32_t dp_id_select;
	uint16_t dp_ids[FIO_MAX_DP_IDS];
	uint32_t dp_nr_ids;
	uint8_t dp_scheme_file[FIO_TOP_STR_MAX];

	uint32_t num_range;
	/*
	 * verify_pattern followed by buffer_pattern from the unpacked struct
	 */
	uint8_t patterns[];
} __attribute__((packed));

/* [한국어] 옵션 변환 및 유틸리티 함수 선언 */
extern int convert_thread_options_to_cpu(struct thread_options *o,
		struct thread_options_pack *top, size_t top_sz);
extern size_t thread_options_pack_size(struct thread_options *o);
extern void convert_thread_options_to_net(struct thread_options_pack *top, struct thread_options *);
extern int fio_test_cconv(struct thread_options *);
extern void options_default_fill(struct thread_options *o);

/* [한국어] split 파싱 콜백 함수 타입 */
typedef int (split_parse_fn)(struct thread_options *, void *,
			     enum fio_ddir, char *, bool);

/* [한국어] 문자열을 분할하여 파싱 */
extern int str_split_parse(struct thread_data *td, char *str,
			   split_parse_fn *fn, void *eo, bool data);

/* [한국어] 방향별 split 파싱 (bssplit, zonesplit 등) */
extern int split_parse_ddir(struct thread_options *o, struct split *split,
			    char *str, bool absolute, unsigned int max_splits);

/* [한국어] 우선순위 기반 방향별 split 파싱 */
extern int split_parse_prio_ddir(struct thread_options *o,
				 struct split_prio **entries, int *nr_entries,
				 char *str);

#endif
