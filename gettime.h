/*
 * [한국어] gettime.h - 시간 측정 API 선언
 *
 * fio에서 사용하는 시간 측정 관련 함수와 구조체를 선언한다.
 * 지원하는 클록 소스:
 *   - CS_GTOD     : gettimeofday() 기반 (벽시계 시간)
 *   - CS_CGETTIME : clock_gettime(CLOCK_MONOTONIC) 기반 (단조 증가 시계)
 *   - CS_CPUCLOCK : CPU TSC(타임스탬프 카운터) 기반 (가장 낮은 오버헤드)
 */
#ifndef FIO_GETTIME_H
#define FIO_GETTIME_H

#include <sys/time.h>

#include "arch/arch.h"       /* 아키텍처별 CPU 클록 지원 (get_cpu_clock 등) */
#include "lib/seqlock.h"     /* 시퀀스 락 - gtod 스레드와의 동기화에 사용 */

/*
 * Clock sources
 * [한국어] 클록 소스 열거형 - fio가 지원하는 시간 측정 방식
 */
enum fio_cs {
	CS_GTOD		= 1,   /* gettimeofday() 사용 */
	CS_CGETTIME,           /* clock_gettime() 사용 (기본값) */
	CS_CPUCLOCK,           /* CPU TSC 카운터 사용 */
	CS_INVAL,              /* 유효하지 않은 클록 소스 (초기화 확인용) */
};

/* [한국어] 모노토닉(단조 증가) 시간을 가져옴. CLOCK_MONOTONIC 또는 CLOCK_REALTIME 사용 */
extern int fio_get_mono_time(struct timespec *);
/* [한국어] 현재 설정된 클록 소스에 따라 시간을 가져오는 핵심 함수 */
extern void fio_gettime(struct timespec *, void *);
/* [한국어] gtod(gettimeofday) 오프로드 스레드 관련 초기화 */
extern void fio_gtod_init(void);
/* [한국어] 클록 초기화 - CPU 클록 캘리브레이션 및 클록 소스 선택 */
extern void fio_clock_init(void);
/* [한국어] 별도 스레드에서 주기적으로 시간을 갱신하는 gtod 오프로드 스레드 시작 */
extern int fio_start_gtod_thread(void);
/* [한국어] CPU 클록(TSC)의 단조 증가 여부를 멀티코어 환경에서 검증 */
extern int fio_monotonic_clocktest(int debug);
/* [한국어] 스레드 로컬 클록 상태 초기화 (TLS 키 설정) */
extern void fio_local_clock_init(void);

/*
 * [한국어] fio_ts - gtod 오프로드 스레드가 갱신하는 공유 시간 구조체
 *
 * 별도 스레드가 주기적으로 ts를 갱신하고, I/O 워커 스레드들이
 * seqlock을 통해 시스템 콜 없이 빠르게 시간을 읽을 수 있다.
 */
extern struct fio_ts {
	struct seqlock seqlock;  /* 읽기-쓰기 동기화용 시퀀스 락 */
	struct timespec ts;      /* 최신 타임스탬프 */
} *fio_ts;

/*
 * [한국어] fio_gettime_offload - gtod 오프로드 스레드에서 시간을 가져옴
 *
 * fio_ts가 활성화되어 있으면 seqlock을 사용하여 시스템 콜 없이
 * 캐시된 시간을 읽어온다. 높은 IOPS 환경에서 오버헤드를 줄이는 데 유용하다.
 * 반환값: 1이면 성공적으로 읽음, 0이면 오프로드 미사용
 */
static inline int fio_gettime_offload(struct timespec *ts)
{
	unsigned int seq;

	if (!fio_ts)
		return 0;

	do {
		seq = read_seqlock_begin(&fio_ts->seqlock);
		*ts = fio_ts->ts;
	} while (read_seqlock_retry(&fio_ts->seqlock, seq));

	return 1;
}

/* [한국어] gtod 오프로드 스레드를 특정 CPU에 바인딩 */
extern void fio_gtod_set_cpu(unsigned int cpu);

#endif
