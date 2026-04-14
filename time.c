/*
 * [한국어] time.c - fio 시간 유틸리티 함수 모음
 *
 * 이 파일은 fio에서 사용하는 시간 관련 유틸리티를 구현한다.
 * 주요 기능:
 *   1) usec_spin() / usec_sleep() - 마이크로초 단위 대기 (busy-loop 및 nanosleep 기반)
 *   2) time_since_genesis() 등   - fio 시작 시점(genesis) 이후 경과 시간 계산
 *   3) ramp_period_check/over()  - 워밍업(ramp) 기간 관리 (통계 수집 전 안정화 구간)
 *   4) fio_time_init()           - 시간 서브시스템 초기화 (nanosleep 정밀도 측정)
 *   5) set_genesis_time() / set_epoch_time() - 기준 시각 설정
 
 * === 파일의 역할 ===
 * fio에서 사용하는 시간 유틸리티를 구현한다. 마이크로초 대기, genesis 경과 시간,
 * ramp period 관리, 시간 서브시스템 초기화를 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio.c의 main()에서 fio_time_init()으로 초기화. backend.c, io_u.c에서
 * 시간 관련 유틸리티를 광범위하게 사용.
 *
 * === 타 모듈과의 연결 ===
 * - fio.c: fio_time_init()으로 시간 서브시스템 초기화
 * - backend.c: ramp_period_check, time_since_genesis 사용
 * - fio_time.h: API 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_time_init(): 시간 서브시스템 초기화 (nanosleep 정밀도 측정)
 * - usec_spin()/usec_sleep(): 마이크로초 단위 대기
 * - time_since_genesis(): fio 시작 이후 경과 시간
 */

/* 표준 시간 관련 헤더 */
#include <time.h>
#include <sys/time.h>

#include "fio.h"

/* [한국어] genesis - fio 전체 실행의 기준 시각 (모든 시간 측정의 원점) */
static struct timespec genesis;
/* [한국어] ns_granularity - nanosleep()의 실제 정밀도 (마이크로초 단위).
 *          이 값보다 짧은 대기는 busy-loop(usec_spin)로 처리한다. */
static unsigned long ns_granularity;

/* [한국어] 워밍업(ramp) 기간의 상태 머신 */
enum ramp_period_states {
	RAMP_RUNNING,   /* 워밍업 진행 중 */
	RAMP_FINISHING, /* 워밍업 완료 조건 충족, 전환 대기 */
	RAMP_DONE       /* 워밍업 완료, 본격 측정 시작 */
};

/* [한국어] timespec에 밀리초를 더하는 유틸리티 함수.
 *          나노초 오버플로우를 초 단위로 올림 처리한다. */
void timespec_add_msec(struct timespec *ts, unsigned int msec)
{
	uint64_t adj_nsec = 1000000ULL * msec;

	ts->tv_nsec += adj_nsec;
	if (adj_nsec >= 1000000000) {
		uint64_t adj_sec = adj_nsec / 1000000000;

		ts->tv_nsec -= adj_sec * 1000000000;
		ts->tv_sec += adj_sec;
	}
	if (ts->tv_nsec >= 1000000000){
		ts->tv_nsec -= 1000000000;
		ts->tv_sec++;
	}
}

/*
 * busy looping version for the last few usec
 */
/* [한국어] usec_spin - 마이크로초 단위 busy-loop 대기.
 *          나노초 정밀도보다 짧은 대기 시간에 사용된다.
 *          nop 명령어를 반복하며 시간이 경과할 때까지 CPU를 점유한다. */
uint64_t usec_spin(unsigned int usec)
{
	struct timespec start;
	uint64_t t;

	fio_gettime(&start, NULL);
	while ((t = utime_since_now(&start)) < usec)
		nop;

	return t;
}

/*
 * busy loop for a fixed amount of cycles
 */
/* [한국어] cycles_spin - 지정된 횟수만큼 nop을 반복하는 고정 사이클 busy-loop */
void cycles_spin(unsigned int n)
{
	unsigned long i;

	for (i=0; i < n; i++)
		nop;
}

/* [한국어] usec_sleep - 마이크로초 단위 슬립 함수.
 *          nanosleep()으로 대부분의 시간을 소비하고,
 *          ns_granularity보다 짧은 나머지 시간은 usec_spin()으로 busy-wait한다.
 *          스레드 종료 신호(td->terminate)를 확인하여 조기 탈출할 수 있다.
 *          최대 1초 단위로 잘라서 sleep하여 종료 신호 응답성을 보장한다. */
uint64_t usec_sleep(struct thread_data *td, unsigned long usec)
{
	struct timespec req;
	struct timespec tv;
	uint64_t t = 0;

	do {
		unsigned long ts = usec;

		if (usec < ns_granularity) {
			t += usec_spin(usec);
			break;
		}

		ts = usec - ns_granularity;

		if (ts >= 1000000) {
			req.tv_sec = ts / 1000000;
			ts -= 1000000 * req.tv_sec;
			/*
			 * Limit sleep to ~1 second at most, otherwise we
			 * don't notice then someone signaled the job to
			 * exit manually.
			 */
			if (req.tv_sec > 1)
				req.tv_sec = 1;
		} else
			req.tv_sec = 0;

		req.tv_nsec = ts * 1000;
		fio_gettime(&tv, NULL);

		if (nanosleep(&req, NULL) < 0)
			break;

		ts = utime_since_now(&tv);
		t += ts;
		if (ts >= usec)
			break;

		usec -= ts;
	} while (!td->terminate);

	return t;
}

/* [한국어] time_since_genesis - genesis 이후 경과 시간 (나노초 단위) */
uint64_t time_since_genesis(void)
{
	return time_since_now(&genesis);
}

/* [한국어] mtime_since_genesis - genesis 이후 경과 시간 (밀리초 단위) */
uint64_t mtime_since_genesis(void)
{
	return mtime_since_now(&genesis);
}

/* [한국어] utime_since_genesis - genesis 이후 경과 시간 (마이크로초 단위) */
uint64_t utime_since_genesis(void)
{
	return utime_since_now(&genesis);
}

/* [한국어] in_ramp_period - 현재 스레드가 워밍업 기간 중인지 확인 */
bool in_ramp_period(struct thread_data *td)
{
	return td->ramp_period_state != RAMP_DONE;
}

/* [한국어] 워밍업 기간이 활성화되어 있는지 나타내는 전역 플래그 */
bool ramp_period_enabled = false;

/* [한국어] ramp_period_check - 모든 스레드의 워밍업 상태를 주기적으로 검사.
 *          ramp_time 또는 ramp_size 조건을 충족한 스레드를 RAMP_FINISHING으로 전환.
 *          group_reporting 모드에서는 같은 그룹의 모든 스레드를 함께 전환한다. */
int ramp_period_check(void)
{
	uint64_t group_bytes = 0;
	int prev_groupid = -1;
	bool group_ramp_period_over = false;

	for_each_td(td) {
		if (td->ramp_period_state != RAMP_RUNNING)
			continue;

		if (td->o.ramp_time &&
		    utime_since_now(&td->epoch) >= td->o.ramp_time) {
			td->ramp_period_state = RAMP_FINISHING;
			continue;
		}

		if (td->o.ramp_size) {
			int ddir;
			const bool needs_lock = td_async_processing(td);

			if (!td->o.group_reporting ||
			    (td->o.group_reporting &&
			     td->groupid != prev_groupid)) {
				group_bytes = 0;
				prev_groupid = td->groupid;
				group_ramp_period_over = false;
			}

			if (needs_lock)
				__td_io_u_lock(td);

			for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
				group_bytes += td->io_bytes[ddir];

			if (needs_lock)
				__td_io_u_unlock(td);

			if (group_bytes >= td->o.ramp_size) {
				td->ramp_period_state = RAMP_FINISHING;
				/*
				 * Mark ramp up for all threads in the group as
				 * done.
				 */
				if (td->o.group_reporting &&
				    !group_ramp_period_over) {
					group_ramp_period_over = true;
					for_each_td(td2) {
						if (td2->groupid == td->groupid)
							 td2->ramp_period_state = RAMP_FINISHING;
					} end_for_each();
				}
			}
		}
	} end_for_each();

	return 0;
}

/* [한국어] parent_update_ramp - 부모 스레드(offload 모드)의 워밍업 상태를 완료로 전환.
 *          통계를 리셋하고 실행 상태를 TD_RAMP로 설정한다. */
static bool parent_update_ramp(struct thread_data *td)
{
	struct thread_data *parent = td->parent;

	if (!parent || parent->ramp_period_state == RAMP_DONE)
		return false;

	reset_all_stats(parent);
	parent->ramp_period_state = RAMP_DONE;
	td_set_runstate(parent, TD_RAMP);
	return true;
}


/* [한국어] ramp_period_over - 워밍업 기간이 끝났는지 확인하고 필요 시 전환 처리.
 *          RAMP_FINISHING -> RAMP_DONE 전환 시 통계를 리셋하고,
 *          offload 모드에서는 부모 스레드의 상태도 함께 전환한다. */
bool ramp_period_over(struct thread_data *td)
{
	/*
	 * In offload mode, ramp state is tracked on the parent job td.
	 * The transition to RAMP_FINISHING is handled by the parent, so
	 * propagate it here to ensure the worker advances to RAMP_DONE.
	 */
	if (td->o.io_submit_mode == IO_MODE_OFFLOAD && td->parent &&
	    td->parent->ramp_period_state == RAMP_FINISHING)
		td->ramp_period_state = RAMP_FINISHING;

	if (td->ramp_period_state == RAMP_DONE)
		return true;

	if (td->ramp_period_state == RAMP_RUNNING)
		return false;

	td->ramp_period_state = RAMP_DONE;
	reset_all_stats(td);
	reset_io_stats(td);
	td_set_runstate(td, TD_RAMP);

	/*
	 * If we have a parent, the parent isn't doing IO. Hence
	 * the parent never enters do_io(), which will switch us
	 * from RAMP -> RUNNING. Do this manually here.
	 */
	if (parent_update_ramp(td))
		td_set_runstate(td, TD_RUNNING);

	return true;
}

/* [한국어] td_ramp_period_init - 스레드별 워밍업 기간 초기화.
 *          ramp_time과 ramp_size는 동시에 지정할 수 없다.
 *          같은 리포팅 그룹 내 ramp_size가 일관적이어야 한다. */
int td_ramp_period_init(struct thread_data *td)
{
	if (td->o.ramp_time || td->o.ramp_size) {
		if (td->o.ramp_time && td->o.ramp_size) {
			td_verror(td, EINVAL, "job rejected: cannot specify both ramp_time and ramp_size");
			return 1;
		}
		/* Make sure options are consistent within reporting group */
		for_each_td(td2) {
			if (td->groupid == td2->groupid &&
			    td->o.ramp_size != td2->o.ramp_size) {
				td_verror(td, EINVAL, "job rejected: inconsistent ramp_size within reporting group");
				return 1;
			}
		} end_for_each();
		td->ramp_period_state = RAMP_RUNNING;
		ramp_period_enabled = true;
	} else {
		td->ramp_period_state = RAMP_DONE;
	}
	return 0;
}

/* [한국어] fio_time_init - 시간 서브시스템 초기화.
 *          fio_clock_init()으로 클럭을 초기화한 뒤,
 *          nanosleep(1us)을 10회 반복하여 실제 정밀도(ns_granularity)를 측정한다.
 *          이 값은 usec_sleep()에서 busy-loop 전환 기준으로 사용된다. */
void fio_time_init(void)
{
	int i;

	fio_clock_init();

	/*
	 * Check the granularity of the nanosleep function
	 */
	for (i = 0; i < 10; i++) {
		struct timespec tv, ts;
		unsigned long elapsed;

		fio_gettime(&tv, NULL);
		ts.tv_sec = 0;
		ts.tv_nsec = 1000;

		nanosleep(&ts, NULL);
		elapsed = utime_since_now(&tv);

		if (elapsed > ns_granularity)
			ns_granularity = elapsed;
	}
}

/* [한국어] set_genesis_time - 전역 기준 시각(genesis)을 현재 시각으로 설정 */
void set_genesis_time(void)
{
	fio_gettime(&genesis, NULL);
}

/* [한국어] set_epoch_time - 스레드별 에포크(epoch) 시각 설정.
 *          td->epoch: fio 내부 클럭 기준 시각
 *          td->alternate_epoch: 로그용 대체 클럭 기준 시각 (밀리초)
 *          td->job_start: 작업 시작 클럭 기준 시각 (밀리초) */
void set_epoch_time(struct thread_data *td, clockid_t log_alternate_epoch_clock_id, clockid_t job_start_clock_id)
{
	struct timespec ts;
	fio_gettime(&td->epoch, NULL);
	clock_gettime(log_alternate_epoch_clock_id, &ts);
	td->alternate_epoch = (unsigned long long)(ts.tv_sec) * 1000 +
						  (unsigned long long)(ts.tv_nsec) / 1000000;
	if (job_start_clock_id == log_alternate_epoch_clock_id)
	{
		td->job_start = td->alternate_epoch;
	}
	else
	{
		clock_gettime(job_start_clock_id, &ts);
		td->job_start = (unsigned long long)(ts.tv_sec) * 1000 +
						(unsigned long long)(ts.tv_nsec) / 1000000;
	}
}

/* [한국어] fill_start_time - genesis 시각을 지정된 timespec 구조체에 복사 */
void fill_start_time(struct timespec *t)
{
	memcpy(t, &genesis, sizeof(genesis));
}
