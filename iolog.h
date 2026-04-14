/*
 * [한국어 설명] I/O 로그 관련 구조체 및 API 헤더 (iolog.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 fio의 I/O 로깅 시스템에서 사용하는 핵심 자료구조와 API를 정의한다.
 * io_stat(통계), io_sample(샘플), io_log(로그 저장소), io_piece(재생 I/O 동작),
 * 로그 플래그, 로그 엔트리 크기 계산 매크로/인라인 함수를 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * iolog.c와 짝을 이루는 헤더로, stat.c와 backend.c에서도 참조된다.
 * iolog.h → iolog.c(구현) / stat.c(통계 구조체 참조) / stat.h(io_stat 포함)
 *
 * === 타 모듈과의 연결 ===
 * - iolog.c: 이 헤더의 구조체와 함수의 구현
 * - stat.c: io_stat 구조체로 온라인 통계(평균/분산) 계산
 * - stat.h: io_stat을 포함하여 thread_stat에서 사용
 * - server.c: I/O 로그를 네트워크로 전송 시 io_log 참조
 *
 * === 주요 함수/구조체 요약 ===
 * - struct io_stat: I/O 통계 (최대/최소/평균/분산, Welford 알고리즘)
 * - struct io_sample: 개별 I/O 샘플 (값, 시간, 방향, 우선순위)
 * - struct io_log: 동적 로그 저장소 (압축 지원, 히스토그램)
 * - struct io_piece: iolog 재생 시 개별 I/O 동작 (오프셋, 크기, 방향)
 * - LOG_*_SAMPLE_BIT: 로그 플래그 (오프셋, 우선순위, 발행 시간 포함 여부)
 */
#ifndef FIO_IOLOG_H
#define FIO_IOLOG_H

#include <stdio.h>

#include "lib/rbtree.h"    /* 레드-블랙 트리 (io_piece 정렬 삽입용) */
#include "lib/ieee754.h"   /* IEEE 754 부동소수점 (평균/분산 계산용) */
#include "flist.h"         /* fio 연결 리스트 */
#include "ioengines.h"     /* I/O 엔진 인터페이스 (ddir 등) */

/*
 * Use for maintaining statistics
 */
/* [한국어] I/O 통계 유지용 구조체 - 최대/최소/평균/분산을 온라인으로 계산 */
struct io_stat {
	uint64_t max_val;      /* 최대값 */
	uint64_t min_val;      /* 최소값 */
	uint64_t samples;      /* 샘플 수 */

	fio_fp64_t mean;       /* 이동 평균 (Welford 알고리즘) */
	fio_fp64_t S;          /* 분산 계산을 위한 누적값 */
};

/* [한국어] I/O 히스토그램 창(window) 구조체 - 주기적 히스토그램 스냅샷 관리 */
struct io_hist {
	uint64_t samples;          /* 현재 윈도우의 샘플 수 */
	unsigned long hist_last;   /* 마지막 히스토그램 기록 시각 */
	struct flist_head list;    /* 히스토그램 엔트리 리스트 */
};

/* [한국어] 로그 샘플링 모드 열거형 */
enum {
	IO_LOG_SAMPLE_AVG = 0, /* 평균값만 기록 */
	IO_LOG_SAMPLE_MAX,     /* 최대값만 기록 */
	IO_LOG_SAMPLE_BOTH,    /* 평균+최대값 모두 기록 */
};

/* [한국어] I/O 샘플 값 구조체 - 하나의 샘플에 두 개의 값(avg, max 등) 저장 */
struct io_sample_value {
	uint64_t val0;  /* 주 값 (평균 또는 단일 값) */
	uint64_t val1;  /* 보조 값 (최대값, BOTH 모드에서 사용) */
};

/* [한국어] I/O 샘플 데이터 공용체 - 일반 값 또는 히스토그램 플랫 엔트리 */
union io_sample_data {
	struct io_sample_value val;            /* 일반 레이턴시/BW/IOPS 값 */
	struct io_u_plat_entry *plat_entry;    /* 히스토그램 로그용 플랫 엔트리 포인터 */
};

/* [한국어] 샘플 데이터 생성 매크로 */
#define sample_val(value) ((union io_sample_data) { .val.val0 = value })
#define sample_plat(plat) ((union io_sample_data) { .plat_entry = plat })

/*
 * A single data sample
 */
/* [한국어] 단일 I/O 샘플 구조체 - 시간, 값, 방향, 우선순위, 블록크기 및 부가 데이터 */
struct io_sample {
	uint64_t time;                 /* 샘플 타임스탬프 (마이크로초) */
	union io_sample_data data;     /* 샘플 데이터 (값 또는 히스토그램) */
	uint32_t __ddir;               /* 방향 + 플래그 비트 (LOG_*_SAMPLE_BIT) */
	uint16_t priority;             /* I/O 우선순위 (ioprio) */
	uint64_t bs;                   /* 블록 크기 */
	uint64_t aux[];                /* 가변 길이 부가 데이터 (오프셋, 발행시간 등) */
};

/*
 * Enumerate indexes of auxiliary log data in struct io_sample aux[] array
 */
/* [한국어] io_sample aux[] 배열의 인덱스 열거형 */
enum {
	IOS_AUX_OFFSET_INDEX,         /* aux[0]: I/O 오프셋 */
	IOS_AUX_ISSUE_TIME_INDEX,     /* aux[1]: I/O 발행 시간 */
};

/* [한국어] 로그 타입 열거형 - 어떤 종류의 로그인지 구분 */
enum {
	IO_LOG_TYPE_LAT = 1,   /* 총 레이턴시 */
	IO_LOG_TYPE_CLAT,      /* 완료 레이턴시 (completion latency) */
	IO_LOG_TYPE_SLAT,      /* 제출 레이턴시 (submission latency) */
	IO_LOG_TYPE_BW,        /* 대역폭 */
	IO_LOG_TYPE_IOPS,      /* IOPS */
	IO_LOG_TYPE_HIST,      /* 히스토그램 */
};

/* [한국어] 로그 엔트리 기본/최대 개수 */
#define DEF_LOG_ENTRIES		1024                       /* 기본 로그 엔트리 수 */
#define MAX_LOG_ENTRIES		(1024 * DEF_LOG_ENTRIES)   /* 최대 로그 엔트리 수 (1M) */

/* [한국어] 로그 청크 구조체 - 하나의 로그 버퍼 블록 */
struct io_logs {
	struct flist_head list;    /* io_log의 io_logs 리스트에 연결 */
	uint64_t nr_samples;       /* 현재 저장된 샘플 수 */
	uint64_t max_samples;      /* 이 청크의 최대 수용 가능 샘플 수 */
	void *log;                 /* 실제 샘플 데이터 버퍼 */
};

/*
 * Dynamically growing data sample log
 */
/* [한국어] 동적으로 증가하는 I/O 로그 구조체 - fio 로그 시스템의 핵심 */
struct io_log {
	/*
	 * Entries already logged
	 */
	/* [한국어] 이미 기록된 로그 엔트리들의 리스트 */
	struct flist_head io_logs;
	uint32_t cur_log_max;          /* 현재 로그 청크의 최대 크기 */

	/*
	 * When the current log runs out of space, store events here until
	 * we have a chance to regrow
	 */
	/* [한국어] 현재 로그 공간 부족 시 임시 저장소 (regrow 전까지 사용) */
	struct io_logs *pending;

	unsigned int log_ddir_mask;    /* 방향 마스크 (LOG_*_SAMPLE_BIT 조합) */

	char *filename;                /* 로그 파일 이름 */

	struct thread_data *td;        /* 소유 스레드 데이터 */

	unsigned int log_type;         /* 로그 타입 (IO_LOG_TYPE_*) */

	/*
	 * If we fail extending the log, stop collecting more entries.
	 */
	/* [한국어] 로그 확장 실패 시 true로 설정하여 추가 수집 중단 */
	bool disabled;

	/*
	 * Log offsets
	 */
	/* [한국어] I/O 오프셋을 로그에 포함할지 여부 */
	unsigned int log_offset;

	/*
	 * Log I/O priorities
	 */
	/* [한국어] I/O 우선순위를 로그에 포함할지 여부 */
	unsigned int log_prio;

	/*
	 * Log I/O issuing time
	 */
	/* [한국어] I/O 발행 시간을 로그에 포함할지 여부 */
	unsigned int log_issue_time;

	/*
	 * Max size of log entries before a chunk is compressed
	 */
	/* [한국어] 압축 트리거 크기 - 이 크기 초과 시 청크를 압축 */
	unsigned int log_gz;

	/*
	 * Don't deflate for storing, just store the compressed bits
	 */
	/* [한국어] 압축 데이터를 그대로 저장 (해제 없이 gz 바이너리로 저장) */
	unsigned int log_gz_store;

	/*
	 * Windowed average, for logging single entries average over some
	 * period of time.
	 */
	/* [한국어] 윈도우 평균 - 일정 기간(avg_msec) 동안의 평균값을 하나의 엔트리로 기록 */
	struct io_stat avg_window[DDIR_RWDIR_CNT];   /* R/W/TRIM 방향별 통계 */
	unsigned long avg_msec;                        /* 평균 윈도우 간격 (밀리초) */
	unsigned long avg_last[DDIR_RWDIR_CNT];        /* 방향별 마지막 평균 기록 시각 */

	/*
	 * Windowed latency histograms, for keeping track of when we need to
	 * save a copy of the histogram every approximately hist_msec
	 * milliseconds.
	 */
	/* [한국어] 윈도우 히스토그램 - hist_msec 간격으로 레이턴시 분포 스냅샷 저장 */
	struct io_hist hist_window[DDIR_RWDIR_CNT];    /* 방향별 히스토그램 윈도우 */
	unsigned long hist_msec;                        /* 히스토그램 윈도우 간격 (밀리초) */
	unsigned int hist_coarseness;                   /* 히스토그램 해상도 (2^n 단위로 그룹화) */

	/* [한국어] 압축 관련 필드들 */
	pthread_mutex_t chunk_lock;                /* 압축 청크 리스트 보호 뮤텍스 */
	unsigned int chunk_seq;                    /* 압축 청크 시퀀스 번호 */
	struct flist_head chunk_list;              /* 압축된 청크 리스트 */

	/* [한국어] 지연 해제(deferred free) 관련 필드들 - 압축 중 안전한 메모리 해제 */
	pthread_mutex_t deferred_free_lock;        /* 지연 해제 리스트 보호 뮤텍스 */
#define IOLOG_MAX_DEFER	8                          /* 최대 지연 해제 항목 수 */
	void *deferred_items[IOLOG_MAX_DEFER];     /* 지연 해제 대상 포인터 배열 */
	unsigned int deferred;                     /* 현재 지연 해제 대기 항목 수 */
};

/*
 * If the upper bit is set, then we have the offset as well
 */
/* [한국어] __ddir 필드의 최상위 비트: 오프셋 정보 포함 표시 */
#define LOG_OFFSET_SAMPLE_BIT	0x80000000U
/*
 * If the bit following the upper bit is set, then we have the priority
 */
/* [한국어] 두 번째 상위 비트: 우선순위 정보 포함 표시 */
#define LOG_PRIO_SAMPLE_BIT	0x40000000U
/*
 * If the bit following prioity sample vit is set, we report both avg and max
 */
/* [한국어] 세 번째 상위 비트: 평균+최대값 동시 기록 표시 */
#define LOG_AVG_MAX_SAMPLE_BIT	0x20000000U
/*
 * If the bit following AVG_MAX_SAMPLE_BIT is set, we report the issue time also
 */
/* [한국어] 네 번째 상위 비트: I/O 발행 시간 포함 표시 */
#define LOG_ISSUE_TIME_SAMPLE_BIT	0x10000000U

/* [한국어] 모든 샘플 플래그 비트의 OR 마스크 - ddir 추출 시 이 비트들을 제거 */
#define LOG_SAMPLE_BITS		(LOG_OFFSET_SAMPLE_BIT | LOG_PRIO_SAMPLE_BIT |\
					LOG_AVG_MAX_SAMPLE_BIT |\
					LOG_ISSUE_TIME_SAMPLE_BIT)
/* [한국어] io_sample에서 순수 ddir 값만 추출하는 매크로 */
#define io_sample_ddir(io)	((io)->__ddir & ~LOG_SAMPLE_BITS)

/* [한국어] io_sample에 ddir과 로그 마스크를 함께 설정하는 인라인 함수 */
static inline void io_sample_set_ddir(struct io_log *log,
				      struct io_sample *io,
				      enum fio_ddir ddir)
{
	io->__ddir = ddir | log->log_ddir_mask;
}

/* [한국어] 로그 엔트리 하나의 크기를 계산 (오프셋/발행시간 포함 여부에 따라 가변) */
static inline size_t __log_entry_sz(bool log_offset, bool log_issue_time)
{
	size_t ret = sizeof(struct io_sample);

	if (log_offset)
		ret += sizeof(uint64_t);       /* aux[IOS_AUX_OFFSET_INDEX] */

	if (log_issue_time)
		ret += sizeof(uint64_t);       /* aux[IOS_AUX_ISSUE_TIME_INDEX] */

	return ret;
}

/* [한국어] io_log 설정에 따른 로그 엔트리 크기 반환 */
static inline size_t log_entry_sz(struct io_log *log)
{
	return __log_entry_sz(log->log_offset, log->log_issue_time);
}

/* [한국어] 현재 로그 청크의 전체 샘플 데이터 크기 (바이트) 계산 */
static inline size_t log_sample_sz(struct io_log *log, struct io_logs *cur_log)
{
	return cur_log->nr_samples * log_entry_sz(log);
}

/* [한국어] 샘플 배열에서 n번째 샘플의 포인터를 반환 (가변 크기 엔트리 대응) */
static inline struct io_sample *__get_sample(void *samples, bool log_offset,
					     bool log_issue_time,
					     uint64_t sample)
{
	uint64_t sample_offset = sample *
		__log_entry_sz(log_offset, log_issue_time);
	return (struct io_sample *) ((char *) samples + sample_offset);
}

/* [한국어] 함수 선언 - 현재 로그 청크 반환 */
struct io_logs *iolog_cur_log(struct io_log *);
/* [한국어] 함수 선언 - 전체 샘플 수 반환 */
uint64_t iolog_nr_samples(struct io_log *);
/* [한국어] 함수 선언 - 스레드의 로그 공간 재확장 */
void regrow_logs(struct thread_data *);
/* [한국어] 함수 선언 - 집계 로그 공간 재확장 */
void regrow_agg_logs(void);

/* [한국어] io_log 설정을 이용해 특정 로그 청크에서 n번째 샘플을 가져오는 래퍼 */
static inline struct io_sample *get_sample(struct io_log *iolog,
					   struct io_logs *cur_log,
					   uint64_t sample)
{
	return __get_sample(cur_log->log,
			    iolog->log_offset, iolog->log_issue_time, sample);
}

/* [한국어] io_piece 상태 플래그 */
enum {
	IP_F_ONRB	= 1,    /* RB 트리에 삽입됨 */
	IP_F_ONLIST	= 2,    /* 연결 리스트에 삽입됨 */
	IP_F_TRIMMED	= 4,    /* trim 처리 완료 */
	IP_F_IN_FLIGHT	= 8,    /* 아직 완료되지 않은 진행 중 I/O */
};

/*
 * When logging io actions, this matches a single sent io_u
 */
/* [한국어] I/O 조각(piece) 구조체 - iolog 재생 시 하나의 I/O 동작을 표현 */
struct io_piece {
	union {
		struct fio_rb_node rb_node;    /* RB 트리 노드 (정렬 저장 시) */
		struct flist_head list;        /* 연결 리스트 노드 (순차 저장 시) */
	};
	struct flist_head trim_list;           /* trim 대상 리스트 연결 */
	union {
		int fileno;                    /* 파일 번호 (iolog 읽기 시) */
		struct fio_file *file;         /* 파일 포인터 (기록 시) */
	};
	unsigned long long offset;             /* I/O 오프셋 */
	uint64_t numberio;                     /* I/O 일련번호 */
	unsigned long len;                     /* I/O 크기 (바이트) */
	unsigned int flags;                    /* 상태 플래그 (IP_F_*) */
	enum fio_ddir ddir;                    /* I/O 방향 (READ/WRITE/TRIM 등) */
	unsigned long delay;                   /* 이전 I/O 이후 지연 시간 */
	unsigned int file_action;              /* 파일 동작 (FIO_LOG_* 열거형) */
};

/*
 * Log exports
 */
/* [한국어] 파일 로그 동작 열거형 - iolog에 기록되는 파일 이벤트 종류 */
enum file_log_act {
	FIO_LOG_ADD_FILE,      /* 파일 추가 */
	FIO_LOG_OPEN_FILE,     /* 파일 열기 */
	FIO_LOG_CLOSE_FILE,    /* 파일 닫기 */
	FIO_LOG_UNLINK_FILE,   /* 파일 삭제 */
};

/* [한국어] 외부 함수 선언 - iolog 읽기/쓰기/관리 API */
struct io_u;
extern int __must_check read_iolog_get(struct thread_data *, struct io_u *);    /* iolog에서 다음 I/O 가져오기 */
extern void log_io_u(const struct thread_data *, const struct io_u *);          /* I/O 동작을 iolog에 기록 */
extern void log_file(struct thread_data *, struct fio_file *, enum file_log_act); /* 파일 이벤트를 iolog에 기록 */
extern bool __must_check init_iolog(struct thread_data *td);                    /* iolog 초기화 (읽기/쓰기) */
extern void log_io_piece(struct thread_data *, struct io_u *);                  /* 완료된 I/O를 히스토리에 기록 */
extern void unlog_io_piece(struct thread_data *, struct io_u *);                /* 실패한 I/O를 히스토리에서 제거 */
extern void trim_io_piece(const struct io_u *);                                 /* I/O piece 크기를 실제 전송량으로 조정 */
extern void queue_io_piece(struct thread_data *, struct io_piece *);            /* I/O piece를 재생 큐에 추가 */
extern void prune_io_piece_log(struct thread_data *);                           /* I/O 히스토리 트리/리스트 정리 */
extern void write_iolog_close(struct thread_data *);                            /* iolog 파일 닫기 */
int64_t iolog_items_to_fetch(struct thread_data *td);                           /* 청크 모드에서 다음 가져올 항목 수 계산 */
extern int iolog_compress_init(struct thread_data *, struct sk_out *);          /* 로그 압축 워크큐 초기화 */
extern void iolog_compress_exit(struct thread_data *);                          /* 로그 압축 워크큐 종료 */
extern size_t log_chunk_sizes(struct io_log *);                                 /* 압축 청크 총 크기 반환 */
extern int init_io_u_buffers(struct thread_data *);                             /* I/O 유닛 버퍼 초기화 */
extern unsigned long long delay_since_ttime(const struct thread_data *,         /* ttime 기반 지연 시간 계산 */
					     unsigned long long);

#ifdef CONFIG_ZLIB
extern int iolog_file_inflate(const char *);   /* 압축된 로그 파일을 해제하여 stdout에 출력 */
#endif

/*
 * Logging
 */
/* [한국어] 로그 설정 파라미터 구조체 - setup_log()에 전달 */
struct log_params {
	struct thread_data *td;    /* 소유 스레드 */
	unsigned long avg_msec;    /* 평균 윈도우 간격 */
	unsigned long hist_msec;   /* 히스토그램 윈도우 간격 */
	int hist_coarseness;       /* 히스토그램 해상도 */
	int log_type;              /* 로그 타입 (IO_LOG_TYPE_*) */
	int log_offset;            /* 오프셋 기록 여부 */
	int log_prio;              /* 우선순위 기록 여부 */
	int log_issue_time;        /* 발행 시간 기록 여부 */
	int log_gz;                /* 압축 크기 임계값 */
	int log_gz_store;          /* 압축 바이너리 그대로 저장 여부 */
	int log_compress;          /* 압축 활성화 여부 */
};

/* [한국어] 단위(per-unit) 로그 여부 판별 - avg_msec 없거나 압축 사용 시 true */
static inline bool per_unit_log(struct io_log *log)
{
	return log && (!log->avg_msec || log->log_gz || log->log_gz_store);
}

/* [한국어] 인라인 로그 여부 - LAT/CLAT/SLAT 타입은 인라인 처리 */
static inline bool inline_log(struct io_log *log)
{
	return log->log_type == IO_LOG_TYPE_LAT ||
		log->log_type == IO_LOG_TYPE_CLAT ||
		log->log_type == IO_LOG_TYPE_SLAT;
}

/* [한국어] replay_align에 따라 I/O piece의 오프셋을 정렬 (하위 비트 제거) */
static inline void ipo_bytes_align(unsigned int replay_align, struct io_piece *ipo)
{
	if (!replay_align)
		return;

	ipo->offset &= ~(replay_align - (uint64_t)1);
}

/* [한국어] 로그 최종 처리 및 출력 API */
extern void finalize_logs(struct thread_data *td, bool);           /* 로그 최종 집계 */
extern void setup_log(struct io_log **, struct log_params *, const char *); /* 로그 초기화 및 설정 */
extern void flush_log(struct io_log *, bool);                      /* 로그를 파일에 기록 */
extern void flush_samples(FILE *, void *, uint64_t);               /* 샘플 데이터를 파일에 출력 */
extern uint64_t hist_sum(int, int, uint64_t *, uint64_t *);        /* 히스토그램 구간 합산 */
extern void free_log(struct io_log *);                             /* 로그 메모리 해제 */
extern void fio_writeout_logs(bool);                               /* 모든 스레드의 로그 기록 */
extern void td_writeout_logs(struct thread_data *, bool);          /* 특정 스레드의 로그 기록 */
extern int iolog_cur_flush(struct io_log *, struct io_logs *);     /* 현재 로그 청크를 압축 큐에 제출 */

/* [한국어] io_piece 초기화 - 리스트 헤드들을 초기 상태로 설정 */
static inline void init_ipo(struct io_piece *ipo)
{
	INIT_FLIST_HEAD(&ipo->list);
	INIT_FLIST_HEAD(&ipo->trim_list);
}

/* [한국어] 압축된 로그 청크 구조체 - gz 압축 데이터 보관 */
struct iolog_compress {
	struct flist_head list;    /* chunk_list에 연결 */
	void *buf;                 /* 압축된 데이터 버퍼 */
	size_t len;                /* 압축된 데이터 길이 */
	unsigned int seq;          /* 시퀀스 번호 (순서 보장용) */
};

#endif
