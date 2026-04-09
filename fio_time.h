/*
 * [한국어] fio_time.h - fio 시간 유틸리티 함수 선언
 *
 * 이 파일은 fio에서 사용하는 시간 측정 및 관리 함수를 선언한다.
 * 주요 기능:
 *   - ntime/utime/mtime_since*() : 나노초/마이크로초/밀리초 단위 경과 시간 계산
 *   - *_since_genesis()          : fio 시작 시점(genesis)부터의 경과 시간
 *   - ramp_period_*()            : 워밍업(ramp) 기간 관리
 *   - cycles_spin/usec_spin()    : CPU 스핀 대기
 *   - usec_sleep()               : 마이크로초 단위 슬립
 */
#ifndef FIO_TIME_H
#define FIO_TIME_H

#include <stdint.h>
/* IWYU pragma: begin_exports */
#include <time.h>
#include <sys/time.h>
/* IWYU pragma: end_exports */
#include "lib/types.h"

/* [한국어] 워밍업(ramp) 기간 체크 간격: 1000밀리초 = 1초 */
#define RAMP_PERIOD_CHECK_MSEC 1000

/* [한국어] 워밍업(ramp) 기간이 활성화되었는지 나타내는 전역 플래그 */
extern bool ramp_period_enabled;

struct thread_data;

/* [한국어] 두 timespec 사이의 경과 시간 (나노초 단위) */
extern uint64_t ntime_since(const struct timespec *, const struct timespec *);
/* [한국어] 주어진 timespec부터 현재까지의 경과 시간 (나노초 단위) */
extern uint64_t ntime_since_now(const struct timespec *);
/* [한국어] 두 timespec 사이의 경과 시간 (마이크로초 단위) */
extern uint64_t utime_since(const struct timespec *, const struct timespec *);
/* [한국어] 주어진 timespec부터 현재까지의 경과 시간 (마이크로초 단위) */
extern uint64_t utime_since_now(const struct timespec *);
/* [한국어] 두 timespec 사이의 상대 시간 차이 (부호 있음, 마이크로초 단위) */
extern int64_t rel_time_since(const struct timespec *,
			      const struct timespec *);
/* [한국어] 두 timespec 사이의 경과 시간 (밀리초 단위) */
extern uint64_t mtime_since(const struct timespec *, const struct timespec *);
/* [한국어] 주어진 timespec부터 현재까지의 경과 시간 (밀리초 단위) */
extern uint64_t mtime_since_now(const struct timespec *);
/* [한국어] 두 timeval 사이의 경과 시간 (밀리초 단위, 레거시 호환) */
extern uint64_t mtime_since_tv(const struct timeval *, const struct timeval *);
/* [한국어] 주어진 timespec부터 현재까지의 경과 시간 (초 단위) */
extern uint64_t time_since_now(const struct timespec *);
/* [한국어] fio 시작 시점(genesis)부터의 경과 시간 (초 단위) */
extern uint64_t time_since_genesis(void);
/* [한국어] fio 시작 시점(genesis)부터의 경과 시간 (밀리초 단위) */
extern uint64_t mtime_since_genesis(void);
/* [한국어] fio 시작 시점(genesis)부터의 경과 시간 (마이크로초 단위) */
extern uint64_t utime_since_genesis(void);
/* [한국어] 지정된 CPU 사이클 수만큼 스핀 대기 */
extern void cycles_spin(unsigned int);
/* [한국어] 지정된 마이크로초만큼 스핀 대기하고 실제 경과 시간 반환 */
extern uint64_t usec_spin(unsigned int);
/* [한국어] 지정된 마이크로초만큼 슬립 (스레드 데이터 참조) */
extern uint64_t usec_sleep(struct thread_data *, unsigned long);
/* [한국어] 시작 시간을 현재 시간으로 채움 */
extern void fill_start_time(struct timespec *);
/* [한국어] fio 시작 시점(genesis time) 설정 */
extern void set_genesis_time(void);
/* [한국어] 워밍업(ramp) 기간 상태 확인 */
extern int ramp_period_check(void);
/* [한국어] 워밍업(ramp) 기간이 끝났는지 확인 */
extern bool ramp_period_over(struct thread_data *);
/* [한국어] 현재 워밍업(ramp) 기간 중인지 확인 */
extern bool in_ramp_period(struct thread_data *);
/* [한국어] 스레드별 워밍업(ramp) 기간 초기화 */
extern int td_ramp_period_init(struct thread_data *);
/* [한국어] 시간 서브시스템 초기화 */
extern void fio_time_init(void);
/* [한국어] timespec에 밀리초를 더함 */
extern void timespec_add_msec(struct timespec *, unsigned int);
/* [한국어] 에포크 시간 설정 (대체 클럭 소스 지원) */
extern void set_epoch_time(struct thread_data *, clockid_t, clockid_t);

#endif
