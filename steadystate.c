/*
 * [한국어] steadystate.c - Steady State(정상 상태) 감지 구현
 *
 * IOPS, BW(대역폭), 레이턴시의 변동이 안정화되었는지 판단하여
 * 성능이 수렴한 시점에 테스트를 조기 종료할 수 있게 한다.
 *
 * 주요 기능:
 *   1) td_steadystate_init()    - 스레드별 SS 파라미터 초기화
 *   2) steadystate_setup()      - 그룹 리포팅을 고려하여 데이터 배열 할당
 *   3) steadystate_check()      - 주기적 호출: 현재 IOPS/BW/lat 수집 후 판정
 *   4) steadystate_slope()      - 기울기(slope) 기반 판정: 최소자승법으로 추세 계산
 *   5) steadystate_deviation()  - 편차(deviation) 기반 판정: 평균 대비 최대 편차 계산
 *
 * 판정 알고리즘:
 *   - slope 모드: 윈도우 내 데이터의 선형 회귀 기울기가 limit 이하이면 SS 도달
 *   - deviation 모드: 윈도우 내 데이터의 평균 대비 최대 편차가 limit 이하이면 SS 도달
 *   - PCT 모드: 기울기/편차를 평균의 백분율로 환산하여 비교
 *
 * 데이터 구조:
 *   순환 버퍼(ring buffer)로 윈도우 크기만큼의 샘플을 유지.
 *   head: 가장 오래된 데이터, tail: 가장 최근 데이터 위치.
 */

#include <stdlib.h>

#include "fio.h"           /* fio 핵심 구조체 및 매크로 */
#include "steadystate.h"   /* Steady State 구조체 및 플래그 */

/* [한국어] 전역 변수 */
bool steadystate_enabled = false;         /* SS 기능 활성화 여부 */
unsigned int ss_check_interval = 1000;    /* SS 체크 간격 (ms, 기본 1초) */

/* [한국어] SS 관련 데이터 배열 해제 */
void steadystate_free(struct thread_data *td)
{
	free(td->ss.iops_data);
	free(td->ss.bw_data);
	free(td->ss.lat_data);
	td->ss.iops_data = NULL;
	td->ss.bw_data = NULL;
	td->ss.lat_data = NULL;
}

/* [한국어] SS 데이터 배열 할당 - 윈도우 크기(intervals)만큼 할당 */
static void steadystate_alloc(struct thread_data *td)
{
	int intervals = td->ss.dur / (ss_check_interval / 1000L);

	td->ss.bw_data = calloc(intervals, sizeof(uint64_t));    /* BW 데이터 배열 */
	td->ss.iops_data = calloc(intervals, sizeof(uint64_t));  /* IOPS 데이터 배열 */
	td->ss.lat_data = calloc(intervals, sizeof(uint64_t));   /* 레이턴시 데이터 배열 */

	td->ss.state |= FIO_SS_DATA;  /* 데이터 할당됨 플래그 */
}

/*
 * [한국어] Steady State 초기 설정
 * 그룹 리포팅이 활성화된 경우, 각 그룹의 마지막 td에만 SS 데이터를 할당.
 * 그룹 리포팅이 아닌 경우 각 td마다 개별 할당.
 */
void steadystate_setup(void)
{
	struct thread_data *prev_td;
	int prev_groupid;

	if (!steadystate_enabled)
		return;

	/*
	 * if group reporting is enabled, identify the last td
	 * for each group and use it for storing steady state
	 * data
	 */
	/* [한국어] 그룹 리포팅 시 각 그룹의 마지막 td를 찾아 SS 데이터 할당 */
	prev_groupid = -1;
	prev_td = NULL;
	for_each_td(td) {
		if (!td->ss.dur)
			continue;

		if (!td->o.group_reporting) {
			steadystate_alloc(td);  /* 개별 할당 */
			continue;
		}

		/* [한국어] 그룹이 바뀌면 이전 그룹의 마지막 td에 할당 */
		if (prev_groupid != td->groupid) {
			if (prev_td)
				steadystate_alloc(prev_td);
			prev_groupid = td->groupid;
		}
		prev_td = td;
	} end_for_each();

	/* [한국어] 마지막 그룹의 마지막 td 처리 */
	if (prev_td && prev_td->o.group_reporting)
		steadystate_alloc(prev_td);
}

/*
 * [한국어] 기울기(slope) 기반 Steady State 판정
 *
 * 최소자승법(least squares)으로 데이터의 선형 회귀 기울기를 계산.
 * 공식: slope = (sum_xy - sum_x * sum_y / n) / (sum_x^2 - (sum_x)^2 / n)
 * x값은 등간격이라 가정하여 계산을 단순화.
 *
 * 버퍼가 처음 가득 찰 때 전체 합을 계산하고,
 * 이후에는 oldest 값을 빼고 newest 값을 더하여 점진적으로 갱신.
 *
 * 반환: true이면 SS 도달 (기울기 절대값 < limit)
 */
static bool steadystate_slope(uint64_t iops, uint64_t bw, double lat,
			      struct thread_data *td)
{
	int i, j;
	double result;
	struct steadystate_data *ss = &td->ss;
	uint64_t new_val;
	int intervals = ss->dur / (ss_check_interval / 1000L);

	/* [한국어] 현재 샘플을 순환 버퍼의 tail 위치에 저장 */
	ss->bw_data[ss->tail] = bw;
	ss->iops_data[ss->tail] = iops;
	ss->lat_data[ss->tail] = (uint64_t)lat;

	/* [한국어] 판정 대상 메트릭 선택 (IOPS / BW / 레이턴시) */
	if (ss->state & FIO_SS_IOPS)
		new_val = iops;
	else if (ss->state & FIO_SS_BW)
		new_val = bw;
	else
		new_val = (uint64_t)lat;

	/* [한국어] 버퍼가 가득 찼거나 윈도우 크기에 도달했을 때 판정 수행 */
	if (ss->state & FIO_SS_BUFFER_FULL || ss->tail - ss->head == intervals - 1) {
		if (!(ss->state & FIO_SS_BUFFER_FULL)) {
			/* first time through */
			/* [한국어] 최초 1회: 전체 합(sum_y, sum_xy) 계산 */
			for (i = 0, ss->sum_y = 0; i < intervals; i++) {
				if (ss->state & FIO_SS_IOPS)
					ss->sum_y += ss->iops_data[i];
				else if (ss->state & FIO_SS_BW)
					ss->sum_y += ss->bw_data[i];
				else
					ss->sum_y += ss->lat_data[i];
				j = (ss->head + i) % intervals;
				if (ss->state & FIO_SS_IOPS)
					ss->sum_xy += i * ss->iops_data[j];
				else if (ss->state & FIO_SS_BW)
					ss->sum_xy += i * ss->bw_data[j];
				else
					ss->sum_xy += i * ss->lat_data[j];
			}
			ss->state |= FIO_SS_BUFFER_FULL;
		} else {		/* easy to update the sums */
			/* [한국어] 점진적 갱신: oldest 제거, newest 추가 */
			ss->sum_y -= ss->oldest_y;
			ss->sum_y += new_val;
			ss->sum_xy = ss->sum_xy - ss->sum_y + intervals * new_val;
		}

		/* [한국어] 다음 회차에서 제거할 oldest 값 저장 */
		if (ss->state & FIO_SS_IOPS)
			ss->oldest_y = ss->iops_data[ss->head];
		else if (ss->state & FIO_SS_BW)
			ss->oldest_y = ss->bw_data[ss->head];
		else
			ss->oldest_y = ss->lat_data[ss->head];

		/*
		 * calculate slope as (sum_xy - sum_x * sum_y / n) / (sum_(x^2)
		 * - (sum_x)^2 / n) This code assumes that all x values are
		 * equally spaced when they are often off by a few milliseconds.
		 * This assumption greatly simplifies the calculations.
		 */
		/* [한국어] 최소자승법으로 기울기 계산 */
		ss->slope = (ss->sum_xy - (double) ss->sum_x * ss->sum_y / intervals) /
				(ss->sum_x_sq - (double) ss->sum_x * ss->sum_x / intervals);
		/* [한국어] PCT 모드: 기울기를 평균의 백분율로 변환 */
		if (ss->state & FIO_SS_PCT)
			ss->criterion = 100.0 * ss->slope / (ss->sum_y / intervals);
		else
			ss->criterion = ss->slope;

		dprint(FD_STEADYSTATE, "sum_y: %llu, sum_xy: %llu, slope: %f, "
					"criterion: %f, limit: %f\n",
					(unsigned long long) ss->sum_y,
					(unsigned long long) ss->sum_xy,
					ss->slope, ss->criterion, ss->limit);

		/* [한국어] criterion의 절대값이 limit 미만이면 SS 도달 */
		result = ss->criterion * (ss->criterion < 0.0 ? -1.0 : 1.0);
		if (result < ss->limit)
			return true;
	}

	/* [한국어] 순환 버퍼 포인터 전진 */
	ss->tail = (ss->tail + 1) % intervals;
	if (ss->tail <= ss->head)
		ss->head = (ss->head + 1) % intervals;

	return false;
}

/*
 * [한국어] 편차(deviation) 기반 Steady State 판정
 *
 * 윈도우 내 데이터의 평균을 계산하고,
 * 각 샘플과 평균의 차이(절대값) 중 최대값을 deviation으로 사용.
 * PCT 모드에서는 deviation을 평균의 백분율로 변환.
 *
 * 반환: true이면 SS 도달 (최대 편차 < limit)
 */
static bool steadystate_deviation(uint64_t iops, uint64_t bw, double lat,
				  struct thread_data *td)
{
	int i;
	double diff;
	double mean;

	struct steadystate_data *ss = &td->ss;
	int intervals = ss->dur / (ss_check_interval / 1000L);

	/* [한국어] 현재 샘플을 순환 버퍼에 저장 */
	ss->bw_data[ss->tail] = bw;
	ss->iops_data[ss->tail] = iops;
	ss->lat_data[ss->tail] = (uint64_t)lat;

	if (ss->state & FIO_SS_BUFFER_FULL || ss->tail - ss->head == intervals  - 1) {
		if (!(ss->state & FIO_SS_BUFFER_FULL)) {
			/* first time through */
			/* [한국어] 최초 1회: 전체 합(sum_y) 계산 */
			for (i = 0, ss->sum_y = 0; i < intervals; i++) {
				if (ss->state & FIO_SS_IOPS)
					ss->sum_y += ss->iops_data[i];
				else if (ss->state & FIO_SS_BW)
					ss->sum_y += ss->bw_data[i];
				else
					ss->sum_y += ss->lat_data[i];
			}
			ss->state |= FIO_SS_BUFFER_FULL;
		} else {		/* easy to update the sum */
			/* [한국어] 점진적 갱신: oldest 제거, newest 추가 */
			ss->sum_y -= ss->oldest_y;
			if (ss->state & FIO_SS_IOPS)
				ss->sum_y += ss->iops_data[ss->tail];
			else if (ss->state & FIO_SS_BW)
				ss->sum_y += ss->bw_data[ss->tail];
			else
				ss->sum_y += ss->lat_data[ss->tail];
		}

		/* [한국어] 다음 회차에서 제거할 oldest 값 저장 */
		if (ss->state & FIO_SS_IOPS)
			ss->oldest_y = ss->iops_data[ss->head];
		else if (ss->state & FIO_SS_BW)
			ss->oldest_y = ss->bw_data[ss->head];
		else
			ss->oldest_y = ss->lat_data[ss->head];

		/* [한국어] 평균 계산 */
		mean = (double) ss->sum_y / intervals;
		ss->deviation = 0.0;

		/* [한국어] 각 샘플과 평균의 차이(절대값) 중 최대값을 deviation으로 */
		for (i = 0; i < intervals; i++) {
			if (ss->state & FIO_SS_IOPS)
				diff = ss->iops_data[i] - mean;
			else if (ss->state & FIO_SS_BW)
				diff = ss->bw_data[i] - mean;
			else
				diff = ss->lat_data[i] - mean;
			ss->deviation = max(ss->deviation, diff * (diff < 0.0 ? -1.0 : 1.0));
		}

		/* [한국어] PCT 모드: 편차를 평균의 백분율로 변환 */
		if (ss->state & FIO_SS_PCT)
			ss->criterion = 100.0 * ss->deviation / mean;
		else
			ss->criterion = ss->deviation;

		dprint(FD_STEADYSTATE, "intervals: %d, sum_y: %llu, mean: %f, max diff: %f, "
					"objective: %f, limit: %f\n",
					intervals,
					(unsigned long long) ss->sum_y, mean,
					ss->deviation, ss->criterion, ss->limit);

		/* [한국어] criterion이 limit 미만이면 SS 도달 */
		if (ss->criterion < ss->limit)
			return true;
	}

	/* [한국어] 순환 버퍼 포인터 전진 */
	ss->tail = (ss->tail + 1) % intervals;
	if (ss->tail == ss->head)
		ss->head = (ss->head + 1) % intervals;

	return false;
}

/*
 * [한국어] Steady State 주기적 체크 (헬퍼 스레드에서 호출)
 *
 * 모든 스레드를 순회하며:
 *   1) 각 스레드의 현재 IOPS, BW, 레이턴시를 수집
 *   2) 그룹 리포팅이면 같은 그룹의 값을 합산
 *   3) ramp_time 경과 후부터 slope 또는 deviation 판정 수행
 *   4) SS 도달 시 해당 스레드(또는 그룹 전체)에 종료 표시
 */
int steadystate_check(void)
{
	int  ddir, prev_groupid, group_ramp_time_over = 0;
	unsigned long rate_time;
	struct timespec now;
	uint64_t group_bw = 0, group_iops = 0;
	double group_lat_sum = 0.0;
	uint64_t group_lat_samples = 0;
	uint64_t td_iops, td_bytes;
	double group_lat;
	bool ret;

	prev_groupid = -1;
	for_each_td(td) {
		const bool needs_lock = td_async_processing(td);
		struct steadystate_data *ss = &td->ss;
		double td_lat_sum = 0.0;
		uint64_t td_lat_samples = 0;

		/* [한국어] SS 미설정, 아직 설정 중, 이미 종료, 이미 SS 도달한 td 건너뜀 */
		if (!ss->dur || td->runstate <= TD_SETTING_UP ||
		    td->runstate >= TD_EXITED || !ss->state ||
		    ss->state & FIO_SS_ATTAINED)
			continue;

		td_iops = 0;
		td_bytes = 0;
		/* [한국어] 새 그룹 시작 시 그룹 누적값 초기화 */
		if (!td->o.group_reporting ||
		    (td->o.group_reporting && td->groupid != prev_groupid)) {
			group_bw = 0;
			group_iops = 0;
			group_lat_sum = 0.0;
			group_lat_samples = 0;
			group_ramp_time_over = 0;
		}
		prev_groupid = td->groupid;

		fio_gettime(&now, NULL);
		/* [한국어] ramp_time 경과 확인 - 체크 간격 1회분 추가 대기 후 기록 시작 */
		if (ss->ramp_time && !(ss->state & FIO_SS_RAMP_OVER)) {
			/*
			 * Begin recording data one check interval after ss->ramp_time
			 * has elapsed
			 */
			if (utime_since(&td->epoch, &now) >= (ss->ramp_time + ss_check_interval * 1000L))
				ss->state |= FIO_SS_RAMP_OVER;
		}

		if (needs_lock)
			__td_io_u_lock(td);

		/* [한국어] 모든 방향(read/write/trim)의 IOPS, 바이트, 레이턴시 합산 */
		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			td_iops += td->io_blocks[ddir];
			td_bytes += td->io_bytes[ddir];
			td_lat_sum += td->ts.clat_stat[ddir].mean.u.f *
				      td->ts.clat_stat[ddir].samples;
			td_lat_samples += td->ts.clat_stat[ddir].samples;
		}

		if (needs_lock)
			__td_io_u_unlock(td);

		/* [한국어] 이전 체크 이후 경과 시간 및 변화량 계산 */
		rate_time = mtime_since(&ss->prev_time, &now);
		memcpy(&ss->prev_time, &now, sizeof(now));

		/* [한국어] ramp_time이 지난 td만 그룹 통계에 반영 */
		if (ss->state & FIO_SS_RAMP_OVER) {
			group_bw += rate_time * (td_bytes - ss->prev_bytes) /
				(ss_check_interval * ss_check_interval / 1000L);
			group_iops += rate_time * (td_iops - ss->prev_iops) /
				(ss_check_interval * ss_check_interval / 1000L);
			group_lat_sum += td_lat_sum - ss->prev_lat_sum;
			group_lat_samples += td_lat_samples - ss->prev_lat_samples;
			++group_ramp_time_over;
		}
		/* [한국어] 이전 값 저장 (다음 체크에서 차이 계산용) */
		ss->prev_iops = td_iops;
		ss->prev_bytes = td_bytes;
		ss->prev_lat_sum = td_lat_sum;
		ss->prev_lat_samples = td_lat_samples;

		/* [한국어] 그룹 리포팅이고 이 td에 SS 데이터가 없으면 건너뜀 */
		if (td->o.group_reporting && !(ss->state & FIO_SS_DATA))
			continue;

		/*
		 * Don't begin checking criterion until ss->ramp_time is over
		 * for at least one thread in group
		 */
		/* [한국어] 그룹 내 최소 1개 스레드의 ramp_time이 지나야 판정 시작 */
		if (!group_ramp_time_over)
			continue;

		dprint(FD_STEADYSTATE, "steadystate_check() thread: %d, "
					"groupid: %u, rate_msec: %ld, "
					"iops: %llu, bw: %llu, head: %d, tail: %d\n",
					__td_index, td->groupid, rate_time,
					(unsigned long long) group_iops,
					(unsigned long long) group_bw,
					ss->head, ss->tail);

		/* [한국어] 그룹 평균 레이턴시 계산 */
		group_lat = 0.0;
		if (group_lat_samples)
			group_lat = group_lat_sum / group_lat_samples;

		/* [한국어] slope 또는 deviation 모드로 SS 판정 */
		if (ss->state & FIO_SS_SLOPE)
			ret = steadystate_slope(group_iops, group_bw, group_lat, td);
		else
			ret = steadystate_deviation(group_iops, group_bw, group_lat, td);

		/* [한국어] SS 도달 시 해당 td(또는 그룹 전체) 종료 표시 */
		if (ret) {
			if (td->o.group_reporting) {
				for_each_td(td2) {
					if (td2->groupid == td->groupid) {
						td2->ss.state |= FIO_SS_ATTAINED;
						fio_mark_td_terminate(td2);
					}
				} end_for_each();
			} else {
				ss->state |= FIO_SS_ATTAINED;
				fio_mark_td_terminate(td);
			}
		}
	} end_for_each();
	return 0;
}

/*
 * [한국어] 스레드별 Steady State 파라미터 초기화
 * 옵션(ss_dur, ss_limit 등)을 ss 구조체에 복사하고,
 * 같은 리포팅 그룹 내 SS 옵션 일관성을 검증.
 */
int td_steadystate_init(struct thread_data *td)
{
	struct steadystate_data *ss = &td->ss;
	struct thread_options *o = &td->o;
	int intervals;

	memset(ss, 0, sizeof(*ss));

	if (o->ss_dur) {
		steadystate_enabled = true;
		o->ss_dur /= 1000000L;  /* 마이크로초 -> 초 변환 */

		/* put all steady state info in one place */
		/* [한국어] 옵션에서 SS 파라미터 복사 */
		ss->dur = o->ss_dur;                         /* SS 윈도우 지속 시간 (초) */
		ss->limit = o->ss_limit.u.f;                 /* 판정 임계값 */
		ss->ramp_time = o->ss_ramp_time;             /* 데이터 수집 시작 전 대기 시간 */
		ss_check_interval = o->ss_check_interval / 1000L; /* 체크 간격 (us -> ms) */

		ss->state = o->ss_state;                     /* SS 모드 플래그 */
		if (!td->ss.ramp_time)
			ss->state |= FIO_SS_RAMP_OVER;      /* ramp_time 미설정이면 즉시 시작 */

		/* [한국어] 윈도우 크기 계산 및 최소자승법에 필요한 sum_x, sum_x^2 사전 계산 */
		intervals = ss->dur / (ss_check_interval / 1000L);
		ss->sum_x = intervals * (intervals - 1) / 2;
		ss->sum_x_sq = (intervals - 1) * (intervals) * (2*intervals - 1) / 6;
	}

	/* make sure that ss options are consistent within reporting group */
	/* [한국어] 같은 리포팅 그룹 내 SS 옵션 일관성 검증 */
	for_each_td(td2) {
		if (td2->groupid == td->groupid) {
			struct steadystate_data *ss2 = &td2->ss;

			if (ss2->dur != ss->dur ||
			    ss2->limit != ss->limit ||
			    ss2->ramp_time != ss->ramp_time ||
			    ss2->state != ss->state ||
			    ss2->sum_x != ss->sum_x ||
			    ss2->sum_x_sq != ss->sum_x_sq) {
				td_verror(td, EINVAL, "job rejected: steadystate options must be consistent within reporting groups");
				return 1;
			}
		}
	} end_for_each();

	return 0;
}

/*
 * [한국어] SS 데이터 배열의 평균값 계산 (결과 출력용)
 * intervals개의 데이터를 합산하여 평균 반환
 */
static uint64_t steadystate_data_mean(uint64_t *data, int ss_dur)
{
	int i;
	uint64_t sum;
	int intervals = ss_dur / (ss_check_interval / 1000L);

	if (!ss_dur)
		return 0;

	for (i = 0, sum = 0; i < intervals; i++)
		sum += data[i];

	return sum / intervals;
}

/* [한국어] SS BW 데이터의 평균값 반환 */
uint64_t steadystate_bw_mean(const struct thread_stat *ts)
{
	return steadystate_data_mean(ts->ss_bw_data, ts->ss_dur);
}

/* [한국어] SS IOPS 데이터의 평균값 반환 */
uint64_t steadystate_iops_mean(const struct thread_stat *ts)
{
	return steadystate_data_mean(ts->ss_iops_data, ts->ss_dur);
}

/* [한국어] SS 레이턴시 데이터의 평균값 반환 */
uint64_t steadystate_lat_mean(const struct thread_stat *ts)
{
	return steadystate_data_mean(ts->ss_lat_data, ts->ss_dur);
}
