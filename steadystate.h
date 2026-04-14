/*
 * [한국어] steadystate.h - Steady State(정상 상태) 감지 헤더
 *
 * IOPS/BW/레이턴시의 변동이 안정화되었는지 판단하기 위한
 * 구조체, 상태 플래그, API 함수 선언.
 *
 * 두 가지 판정 모드:
 *   - slope: 최소자승법으로 추세 기울기 계산 (FIO_SS_SLOPE)
 *   - deviation: 평균 대비 최대 편차 계산 (기본)
 *
 * 세 가지 메트릭:
 *   - IOPS (FIO_SS_IOPS), BW (FIO_SS_BW), 레이턴시 (FIO_SS_LAT)
 *
 * 옵션 수식어:
 *   - PCT (FIO_SS_PCT): 판정값을 평균의 백분율로 환산
 
 * === 파일의 역할 ===
 * IOPS/BW/레이턴시의 변동 안정화 판단을 위한 구조체, 상태 플래그, API를 선언.
 * slope(기울기)/deviation(편차) 판정 모드, IOPS/BW/LAT 메트릭을 지원.
 *
 * === 전체 아키텍처에서의 위치 ===
 * steadystate.c와 짝을 이루는 헤더. helper_thread.c에서 SS 체크 시 참조.
 *
 * === 타 모듈과의 연결 ===
 * - steadystate.c: 이 헤더의 함수 구현
 * - helper_thread.c: 주기적으로 steadystate_check() 호출
 *
 * === 주요 함수/구조체 요약 ===
 * - struct steadystate_data: SS 판정 데이터 (윈도우, 기울기, 편차)
 * - FIO_SS_SLOPE/FIO_SS_IOPS/FIO_SS_BW/FIO_SS_PCT: SS 모드 플래그
 */
#ifndef FIO_STEADYSTATE_H
#define FIO_STEADYSTATE_H

#include "thread_options.h"  /* 스레드 옵션 구조체 */

/* [한국어] SS API 함수 선언 */
extern void steadystate_free(struct thread_data *);          /* SS 데이터 배열 해제 */
extern int steadystate_check(void);                          /* 주기적 SS 도달 여부 체크 */
extern void steadystate_setup(void);                         /* SS 초기 설정 (데이터 배열 할당) */
extern int td_steadystate_init(struct thread_data *);        /* 스레드별 SS 파라미터 초기화 */
extern uint64_t steadystate_bw_mean(const struct thread_stat *);   /* SS BW 평균 */
extern uint64_t steadystate_iops_mean(const struct thread_stat *); /* SS IOPS 평균 */
extern uint64_t steadystate_lat_mean(const struct thread_stat *);  /* SS 레이턴시 평균 */

extern bool steadystate_enabled;          /* SS 기능 전역 활성화 플래그 */
extern unsigned int ss_check_interval;    /* SS 체크 간격 (ms) */

/*
 * [한국어] Steady State 데이터 구조체
 * 각 스레드(또는 그룹의 대표 스레드)가 보유하며,
 * 순환 버퍼로 윈도우 크기만큼의 샘플을 유지한다.
 */
struct steadystate_data {
	double limit;                  /* 판정 임계값 */
	unsigned long long dur;        /* SS 윈도우 지속 시간 (초) */
	unsigned long long ramp_time;  /* 데이터 수집 시작 전 대기 시간 */

	uint32_t state;                /* SS 상태 플래그 (FIO_SS_* 조합) */

	unsigned int head;             /* 순환 버퍼 헤드 (가장 오래된 데이터) */
	unsigned int tail;             /* 순환 버퍼 테일 (최신 데이터 삽입 위치) */
	uint64_t *iops_data;          /* IOPS 샘플 배열 (순환 버퍼) */
	uint64_t *bw_data;            /* BW 샘플 배열 (순환 버퍼) */
	uint64_t *lat_data;           /* 레이턴시 샘플 배열 (순환 버퍼) */

	double slope;                  /* 최소자승법으로 계산한 기울기 */
	double deviation;              /* 평균 대비 최대 편차 */
	double criterion;              /* 최종 판정값 (slope 또는 deviation, PCT 적용 후) */

	/* [한국어] 최소자승법 및 점진적 합 갱신을 위한 누적값 */
	uint64_t sum_y;                /* y값(메트릭)의 합 */
	uint64_t sum_x;                /* x값(인덱스)의 합 (사전 계산) */
	uint64_t sum_x_sq;             /* x^2의 합 (사전 계산) */
	uint64_t sum_xy;               /* x*y의 합 */
	uint64_t oldest_y;             /* 윈도우에서 제거될 가장 오래된 y값 */

	/* [한국어] 이전 체크 시점의 값 (변화량 계산용) */
	struct timespec prev_time;     /* 이전 체크 시각 */
	uint64_t prev_iops;           /* 이전 체크 시점의 누적 IOPS */
	uint64_t prev_bytes;          /* 이전 체크 시점의 누적 바이트 */
	double prev_lat_sum;          /* 이전 체크 시점의 레이턴시 합 */
	uint64_t prev_lat_samples;    /* 이전 체크 시점의 레이턴시 샘플 수 */
};

/*
 * [한국어] SS 상태 플래그 비트 인덱스
 * enum 값을 비트 시프트하여 FIO_SS_* 플래그로 사용
 */
enum {
	__FIO_SS_IOPS = 0,        /* IOPS 기반 판정 */
	__FIO_SS_BW,              /* BW(대역폭) 기반 판정 */
	__FIO_SS_SLOPE,           /* 기울기(slope) 모드 */
	__FIO_SS_ATTAINED,        /* SS 도달 완료 */
	__FIO_SS_RAMP_OVER,       /* ramp_time 경과 완료 */
	__FIO_SS_DATA,            /* 데이터 배열 할당됨 */
	__FIO_SS_PCT,             /* 백분율 모드 */
	__FIO_SS_BUFFER_FULL,     /* 순환 버퍼가 가득 참 (최초 윈도우 채움 완료) */
	__FIO_SS_LAT,             /* 레이턴시 기반 판정 */
};

/* [한국어] SS 상태 플래그 비트마스크 */
enum {
	FIO_SS_IOPS		= 1 << __FIO_SS_IOPS,         /* IOPS 기반 */
	FIO_SS_BW		= 1 << __FIO_SS_BW,           /* BW 기반 */
	FIO_SS_SLOPE		= 1 << __FIO_SS_SLOPE,        /* 기울기 모드 */
	FIO_SS_ATTAINED		= 1 << __FIO_SS_ATTAINED,     /* SS 도달 완료 */
	FIO_SS_RAMP_OVER	= 1 << __FIO_SS_RAMP_OVER,    /* ramp 완료 */
	FIO_SS_DATA		= 1 << __FIO_SS_DATA,         /* 데이터 할당됨 */
	FIO_SS_PCT		= 1 << __FIO_SS_PCT,          /* 백분율 모드 */
	FIO_SS_BUFFER_FULL	= 1 << __FIO_SS_BUFFER_FULL,  /* 버퍼 가득 참 */
	FIO_SS_LAT		= 1 << __FIO_SS_LAT,          /* 레이턴시 기반 */

	/* [한국어] 편의를 위한 조합 플래그 */
	FIO_SS_IOPS_SLOPE	= FIO_SS_IOPS | FIO_SS_SLOPE,  /* IOPS + 기울기 */
	FIO_SS_BW_SLOPE		= FIO_SS_BW | FIO_SS_SLOPE,    /* BW + 기울기 */
	FIO_SS_LAT_SLOPE	= FIO_SS_LAT | FIO_SS_SLOPE,   /* 레이턴시 + 기울기 */
};

#endif
