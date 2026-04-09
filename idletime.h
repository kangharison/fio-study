/*
 * [한국어] idletime.h - CPU 유휴 시간 프로파일링 헤더
 *
 * fio 실행 중 CPU 유휴율을 측정하기 위한 구조체, 상수, API를 정의한다.
 * 각 CPU에 SCHED_IDLE 우선순위 스레드를 배치하여 유휴 시간을 추정한다.
 */
#ifndef FIO_IDLETIME_H
#define FIO_IDLETIME_H

#include <sys/time.h>
#include <sys/types.h>
#include "os/os.h"

/* [한국어] 보정(calibration) 관련 상수 */
#define CALIBRATE_RUNS  10    /* 보정 반복 횟수 */
#define CALIBRATE_SCALE 1000  /* 분산 감소를 위한 스케일 팩터 */
#define MAX_CPU_STR_LEN 32    /* CPU 이름 문자열 최대 길이 */

/* [한국어] 유휴 프로파일링 옵션 */
enum {
	IDLE_PROF_OPT_NONE,    /* 프로파일링 비활성화 */
	IDLE_PROF_OPT_CALI,    /* calibration only - 보정만 수행 */
	IDLE_PROF_OPT_SYSTEM,  /* 시스템 전체 유휴율 측정 */
	IDLE_PROF_OPT_PERCPU   /* CPU별 유휴율 측정 */
};

/* [한국어] 유휴 프로파일링 상태 */
enum {
	 IDLE_PROF_STATUS_OK,         /* 정상 */
	 IDLE_PROF_STATUS_CALI_STOP,  /* 보정 완료, 프로파일링 건너뜀 */
	 IDLE_PROF_STATUS_PROF_STOP,  /* 프로파일링 중지 신호 */
	 IDLE_PROF_STATUS_ABORT       /* 에러로 인한 중단 */
};

/* [한국어] idle_prof_thread - CPU별 유휴 프로파일링 스레드 상태 구조체 */
struct idle_prof_thread {
	pthread_t thread;              /* 스레드 핸들 */
	int cpu;                       /* 바인딩된 CPU 번호 */
	int state;                     /* 스레드 상태 (TD_NOT_CREATED 등) */
	struct timespec tps;           /* 프로파일링 시작 시각 */
	struct timespec tpe;           /* 프로파일링 종료 시각 */
	double cali_time; /* microseconds to finish a unit work - 단위 작업 보정 시간 (마이크로초) */
	double loops;                  /* 완료된 루프 수 (유휴율 계산에 사용) */
	double idleness;               /* 유휴율 (0.0=완전 사용 중, 1.0=완전 유휴) */
	unsigned char *data;             /* bytes to be touched - 단위 작업용 데이터 버퍼 */
	pthread_cond_t  cond;          /* 보정 완료 신호용 조건변수 */
	pthread_mutex_t init_lock;     /* 초기화 동기화 뮤텍스 */
	pthread_mutex_t start_lock;    /* 프로파일링 시작 동기화 뮤텍스 */

	os_cpu_mask_t cpu_mask;        /* CPU 친화성 마스크 */
};

/* [한국어] idle_prof_common - 유휴 프로파일링 전역 공유 상태 구조체 */
struct idle_prof_common {
	struct idle_prof_thread *ipts; /* CPU별 스레드 배열 */
	int nr_cpus;                   /* 시스템 CPU 수 */
	int status;                    /* 프로파일링 상태 (IDLE_PROF_STATUS_*) */
	int opt;                       /* 프로파일링 옵션 (IDLE_PROF_OPT_*) */
	double cali_mean;              /* 보정 시간 평균 */
	double cali_stddev;            /* 보정 시간 표준편차 */
	void *buf;    /* single data allocation for all threads - 모든 스레드의 데이터 버퍼 */
};

/* [한국어] 유휴 프로파일링 API */
extern int fio_idle_prof_parse_opt(const char *);

extern void fio_idle_prof_init(void);
extern void fio_idle_prof_start(void);
extern void fio_idle_prof_stop(void);

extern void show_idle_prof_stats(int, struct json_object *, struct buf_output *);

extern void fio_idle_prof_cleanup(void);

#endif
