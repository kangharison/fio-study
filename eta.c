/*
 * Status and ETA code
 */
/*
 * [한국어] eta.c - ETA(예상 완료 시간) 및 진행률 계산/표시
 *
 * 이 파일은 fio 실행 중 터미널에 표시되는 상태 표시줄을 담당한다.
 * 주요 기능:
 *   1) check_str_update()     - 각 스레드의 현재 상태를 문자(r/w/R/W/V 등)로 변환
 *   2) eta_to_str()           - 남은 시간(초)을 "01d:02h:03m:04s" 형식 문자열로 변환
 *   3) thread_eta()           - 개별 스레드의 예상 남은 시간을 계산
 *   4) calc_rate()/calc_iops()- 구간별 대역폭(KiB/s) 및 IOPS 계산
 *   5) calc_thread_status()   - 모든 스레드를 순회하며 전체 ETA/속도/상태 집계
 *   6) gen_eta_str()          - 속도/IOPS 문자열 생성 ([r=xxx,w=xxx][r=xxx,w=xxx IOPS])
 *   7) display_thread_status()- 최종 상태 표시줄을 터미널에 출력
 *   8) get_jobs_eta()         - jobs_eta 구조체를 할당하고 계산 결과를 채워 반환
 *   9) print_thread_status()  - get_jobs_eta + display_thread_status 래퍼
 *  10) print_status_init()    - 상태 문자열 초기화 (스레드를 'P'(대기) 상태로 설정)
 *
 * 상태 표시줄 출력 형식 예시:
 *   Jobs: 2 (f=4): [RW][50.0%][r=100MiB/s,w=50MiB/s][r=1000,w=500 IOPS][eta 00m:30s]
 *
 * 스레드 상태 문자 매핑:
 *   P=미생성, C=생성됨, I=초기화됨, /=램프업, p=사전읽기,
 *   R/r=순차/랜덤 읽기, W/w=순차/랜덤 쓰기, M/m=순차/랜덤 혼합,
 *   D/d=순차/랜덤 트림, V=검증, F=fsync, f=마무리,
 *   E=종료, _=수거완료, X=에러종료, K=시그널종료
 */

/* 표준 라이브러리 헤더 */
#include <unistd.h>        /* STDOUT_FILENO, isatty() 등 POSIX 기본 함수 */
#include <string.h>        /* memcpy, strlen, strcpy 등 문자열 처리 */
#include <stdlib.h>        /* calloc, free 등 메모리 할당 */
#ifdef CONFIG_VALGRIND_DEV
#include <valgrind/drd.h>  /* Valgrind DRD(Data Race Detector) 지원 */
#else
#define DRD_IGNORE_VAR(x) do { } while (0)  /* Valgrind 미사용 시 빈 매크로 */
#endif

/* fio 내부 헤더 */
#include "fio.h"           /* fio 핵심 구조체(thread_data, jobs_eta 등) 및 매크로 */
#include "lib/pow2.h"      /* is_power_of_2() - 2의 거듭제곱 판별 유틸리티 */

/*
 * [한국어] 전역 상태 문자열 변수
 * __run_str: 각 스레드의 상태를 1문자로 저장하는 원본 배열 (인덱스 = thread_number - 1)
 * run_str:   __run_str을 압축한 형태 (예: "RR" -> "R(2)") - 실제 출력에 사용
 */
static char __run_str[REAL_MAX_JOBS + 1];
static char run_str[__THREAD_RUNSTR_SZ(REAL_MAX_JOBS) + 1];

/*
 * [한국어] 상태 문자열 압축 함수
 * 원본 문자열(rstr)에서 연속된 동일 문자를 "문자(개수)" 형태로 압축
 * 예: "RRRWW" -> "R(3),W(2)"
 * 이렇게 하면 수백 개의 스레드가 있어도 상태줄이 짧아진다.
 */
static void update_condensed_str(char *rstr, char *run_str_condensed)
{
	if (*rstr) {
		while (*rstr) {
			int nr = 1;

			*run_str_condensed++ = *rstr++;  /* 현재 문자 복사 */
			while (*(rstr - 1) == *rstr) {   /* 동일 문자 연속 카운트 */
				rstr++;
				nr++;
			}
			run_str_condensed += sprintf(run_str_condensed, "(%u),", nr);  /* "(개수)," 추가 */
		}
		run_str_condensed--;  /* 마지막 쉼표 제거를 위해 포인터 후퇴 */
	}
	*run_str_condensed = '\0';  /* 널 종료 */
}

/*
 * Sets the status of the 'td' in the printed status map.
 */
/*
 * [한국어] 스레드 상태를 상태 문자로 변환
 * td->runstate 값에 따라 적절한 문자를 __run_str에 기록한다.
 * 상태 변경 시마다 호출되어 run_str도 함께 갱신한다.
 *
 * 상태 문자 규칙:
 *   - 대문자(R,W,M,D) = 순차(sequential) I/O
 *   - 소문자(r,w,m,d) = 랜덤(random) I/O
 *   - 혼합(mixed) 워크로드에서 rwmix 비율에 따라 R/W/M 결정
 */
static void check_str_update(struct thread_data *td)
{
	char c = __run_str[td->thread_number - 1];  /* 현재 상태 문자 */

	switch (td->runstate) {
	case TD_REAPED:          /* 수거 완료 상태 */
		if (td->error)
			c = 'X';    /* 에러로 종료 */
		else if (td->sig)
			c = 'K';    /* 시그널로 종료(killed) */
		else
			c = '_';    /* 정상 종료 후 수거됨 */
		break;
	case TD_EXITED:          /* 종료됨 (아직 수거 전) */
		c = 'E';
		break;
	case TD_RAMP:            /* 램프업 기간 (워밍업) */
		c = '/';
		break;
	case TD_RUNNING:         /* 실행 중 - I/O 방향과 패턴에 따라 문자 결정 */
		if (td_rw(td)) {
			/* 읽기+쓰기 혼합 워크로드 */
			if (td_random(td)) {
				/* 랜덤 혼합 */
				if (td->o.rwmix[DDIR_READ] == 100)
					c = 'r';    /* 사실상 100% 랜덤 읽기 */
				else if (td->o.rwmix[DDIR_WRITE] == 100)
					c = 'w';    /* 사실상 100% 랜덤 쓰기 */
				else
					c = 'm';    /* 랜덤 혼합 */
			} else {
				/* 순차 혼합 */
				if (td->o.rwmix[DDIR_READ] == 100)
					c = 'R';    /* 사실상 100% 순차 읽기 */
				else if (td->o.rwmix[DDIR_WRITE] == 100)
					c = 'W';    /* 사실상 100% 순차 쓰기 */
				else
					c = 'M';    /* 순차 혼합 */
			}
		} else if (td_read(td)) {
			/* 읽기 전용 */
			if (td_random(td))
				c = 'r';        /* 랜덤 읽기 */
			else
				c = 'R';        /* 순차 읽기 */
		} else if (td_write(td)) {
			/* 쓰기 전용 */
			if (td_random(td))
				c = 'w';        /* 랜덤 쓰기 */
			else
				c = 'W';        /* 순차 쓰기 */
		} else {
			/* 트림(discard) */
			if (td_random(td))
				c = 'd';        /* 랜덤 트림 */
			else
				c = 'D';        /* 순차 트림 */
		}
		break;
	case TD_PRE_READING:     /* 사전 읽기 단계 */
		c = 'p';
		break;
	case TD_VERIFYING:       /* 데이터 검증 중 */
		c = 'V';
		break;
	case TD_FSYNCING:        /* fsync 수행 중 */
		c = 'F';
		break;
	case TD_FINISHING:       /* 마무리 단계 */
		c = 'f';
		break;
	case TD_CREATED:         /* 스레드 생성됨 (아직 실행 전) */
		c = 'C';
		break;
	case TD_INITIALIZED:     /* 초기화 완료 */
	case TD_SETTING_UP:      /* 설정 중 */
		c = 'I';
		break;
	case TD_NOT_CREATED:     /* 아직 생성되지 않음 (대기) */
		c = 'P';
		break;
	default:
		log_err("state %d\n", td->runstate);
	}

	__run_str[td->thread_number - 1] = c;  /* 상태 문자 갱신 */
	update_condensed_str(__run_str, run_str);  /* 압축 문자열도 갱신 */
}

/*
 * Convert seconds to a printable string.
 */
/*
 * [한국어] 초 단위 ETA를 사람이 읽기 쉬운 문자열로 변환
 * 예: 90061초 -> "01d:01h:01m:01s"
 * eta_sec이 -1이면 계산 불가를 의미하여 "--" 출력
 */
void eta_to_str(char *str, unsigned long eta_sec)
{
	unsigned int d, h, m, s;     /* 일, 시, 분, 초 */
	int disp_hour = 0;          /* 시간 표시 여부 플래그 */

	if (eta_sec == -1) {
		sprintf(str, "--");  /* 계산 불가 시 "--" 표시 */
		return;
	}

	/* 초 -> 분 -> 시 -> 일로 분해 */
	s = eta_sec % 60;
	eta_sec /= 60;
	m = eta_sec % 60;
	eta_sec /= 60;
	h = eta_sec % 24;
	eta_sec /= 24;
	d = eta_sec;

	if (d) {
		disp_hour = 1;                          /* 일이 있으면 시간도 표시 */
		str += sprintf(str, "%02ud:", d);
	}

	if (h || disp_hour)
		str += sprintf(str, "%02uh:", h);

	str += sprintf(str, "%02um:", m);
	sprintf(str, "%02us", s);
}

/*
 * Best effort calculation of the estimated pending runtime of a job.
 */
/*
 * [한국어] 개별 스레드의 예상 남은 시간(ETA) 계산
 *
 * 계산 방식:
 *   1) 실행 중(TD_RUNNING/TD_VERIFYING): 완료된 바이트 비율로 추정
 *      - time_based 모드: 경과시간/타임아웃과 바이트 비율 중 작은 값 사용
 *      - perc = bytes_done / bytes_total (또는 elapsed / timeout)
 *      - eta = elapsed * (1/perc) - elapsed
 *   2) 아직 시작 전(TD_NOT_CREATED 등): 타임아웃 또는 rate 기반 추정
 *      - t_eta = timeout + start_delay (+ ramp_time)
 *      - r_eta = bytes_total / rate_bytes + start_delay
 *   3) 이미 완료: 0 반환
 *
 * zone_size/zone_skip 보정: 존 기반 워크로드에서 skip 구간은 실제 I/O가 아니므로 제외
 * verify 보정: 쓰기 후 검증 시 총 바이트가 2배 (혼합이면 쓰기 비율만큼 추가)
 */
static unsigned long thread_eta(struct thread_data *td)
{
	unsigned long long bytes_total, bytes_done;  /* 총 바이트, 완료된 바이트 */
	unsigned long eta_sec = 0;                   /* 계산된 ETA (초) */
	unsigned long elapsed;                       /* 경과 시간 (초) */
	uint64_t timeout;                            /* 타임아웃 설정값 (초) */

	elapsed = (mtime_since_now(&td->epoch) + 999) / 1000;  /* 밀리초 -> 초 (올림) */
	timeout = td->o.timeout / 1000000UL;                    /* 마이크로초 -> 초 */

	bytes_total = td->total_io_size;  /* 총 I/O 크기 */

	/* 진행률 추적 불가 플래그 확인 */
	if (td->flags & TD_F_NO_PROGRESS)
		return -1;

	/* fill_device 모드: 디바이스가 가득 찰 때까지 쓰므로, 실제 채워진 크기 사용 */
	if (td->o.fill_device && td->o.size  == -1ULL) {
		if (!td->fill_device_size || td->fill_device_size == -1ULL)
			return 0;

		bytes_total = td->fill_device_size;
	}

	/*
	 * If io_size is set, bytes_total is an exact value that does not need
	 * adjustment.
	 */
	/*
	 * [한국어] zone_size + zone_skip이 설정된 경우 bytes_total 보정
	 * 존(zone) 사이의 skip 구간은 실제 I/O가 아니므로 총량에서 제외해야
	 * 정확한 ETA를 계산할 수 있다.
	 * io_size가 명시적으로 설정된 경우에는 이미 정확한 값이므로 보정하지 않음.
	 */
	if (td->o.zone_size && td->o.zone_skip && bytes_total &&
	    !fio_option_is_set(&td->o, io_size)) {
		unsigned int nr_zones;       /* 처리할 존의 수 */
		uint64_t zone_bytes;         /* 존 하나의 크기 (데이터 + skip) */

		/*
		 * Calculate the upper bound of the number of zones that will
		 * be processed, including skipped bytes between zones. If this
		 * is larger than total_io_size (e.g. when --io_size or --size
		 * specify a small value), use the lower bound to avoid
		 * adjustments to a negative value that would result in a very
		 * large bytes_total and an incorrect eta.
		 */
		/* [한국어] 존 개수의 상한을 계산하되, skip을 빼면 음수가 될 수 있으므로 하한도 확인 */
		zone_bytes = td->o.zone_size + td->o.zone_skip;
		nr_zones = (bytes_total + zone_bytes - 1) / zone_bytes;  /* 올림 나눗셈 */
		if (bytes_total < nr_zones * td->o.zone_skip)
			nr_zones = bytes_total / zone_bytes;  /* 하한으로 폴백 */
		bytes_total -= nr_zones * td->o.zone_skip;  /* skip 구간 제외 */
	}

	/*
	 * if writing and verifying afterwards, bytes_total will be twice the
	 * size. In a mixed workload, verify phase will be the size of the
	 * first stage writes.
	 */
	/*
	 * [한국어] 쓰기 후 검증(do_verify) 시 총 바이트 보정
	 * - 순수 쓰기: 쓰기 + 검증 읽기 = 2배
	 * - 혼합(rw): 쓰기 비율(rwmix[WRITE])만큼만 검증 분량 추가
	 */
	if (td->o.do_verify && td->o.verify && td_write(td)) {
		if (td_rw(td)) {
			unsigned int perc = 50;  /* 기본값: 50% 쓰기 */

			if (td->o.rwmix[DDIR_WRITE])
				perc = td->o.rwmix[DDIR_WRITE];

			bytes_total += (bytes_total * perc) / 100;  /* 쓰기 비율만큼 추가 */
		} else {
			bytes_total <<= 1;  /* 순수 쓰기면 2배 */
		}
	}

	/* [한국어] 실행 중이거나 검증 중인 스레드: 완료율 기반 ETA 계산 */
	if (td->runstate == TD_RUNNING || td->runstate == TD_VERIFYING) {
		double perc, perc_t;  /* perc: 바이트 기반 진행률, perc_t: 시간 기반 진행률 */

		bytes_done = ddir_rw_sum(td->io_bytes);  /* 읽기+쓰기+트림 완료 바이트 합산 */

		if (bytes_total) {
			perc = (double) bytes_done / (double) bytes_total;
			if (perc > 1.0)
				perc = 1.0;  /* 100% 초과 방지 */
		} else {
			perc = 0.0;
		}

		/* [한국어] time_based 모드: 시간 기반 진행률과 바이트 기반 중 작은 값 사용 */
		if (td->o.time_based) {
			if (timeout) {
				perc_t = (double) elapsed / (double) timeout;
				if (perc_t < perc)
					perc = perc_t;  /* 더 보수적인(느린) 추정치 사용 */
			} else {
				/*
				 * Will never hit, we can't have time_based
				 * without a timeout set.
				 */
				perc = 0.0;
			}
		}

		if (perc == 0.0) {
			eta_sec = timeout;  /* 진행률 0이면 타임아웃 전체를 ETA로 설정 */
		} else {
			/* [한국어] ETA = 총 예상 시간 - 경과 시간 = elapsed/perc - elapsed */
			eta_sec = (unsigned long) (elapsed * (1.0 / perc)) - elapsed;
		}

		/* [한국어] ETA가 남은 타임아웃보다 크면 타임아웃으로 제한 */
		if (td->o.timeout &&
		    eta_sec > (timeout + done_secs - elapsed))
			eta_sec = timeout + done_secs - elapsed;
	} else if (td->runstate == TD_NOT_CREATED || td->runstate == TD_CREATED
			|| td->runstate == TD_INITIALIZED
			|| td->runstate == TD_SETTING_UP
			|| td->runstate == TD_RAMP
			|| td->runstate == TD_PRE_READING) {
		/*
		 * [한국어] 아직 시작 전인 스레드: 타임아웃과 rate 기반 추정
		 * t_eta: 타임아웃 + 시작 지연 + 램프 시간 기반
		 * r_eta: 총 바이트 / 설정된 rate 기반
		 * 둘 다 있으면 더 작은 값(빨리 끝나는 쪽) 사용
		 */
		int64_t t_eta = 0, r_eta = 0;
		unsigned long long rate_bytes;  /* 설정된 rate 합산 (bytes/s) */

		/*
		 * We can only guess - assume it'll run the full timeout
		 * if given, otherwise assume it'll run at the specified rate.
		 */
		if (td->o.timeout) {
			uint64_t __timeout = td->o.timeout;
			uint64_t start_delay = td->o.start_delay;
			uint64_t ramp_time = td->o.ramp_time;

			t_eta = __timeout + start_delay;
			if (in_ramp_period(td))
				t_eta += ramp_time;       /* 램프 기간이면 추가 */
			t_eta /= 1000000ULL;          /* 마이크로초 -> 초 */

			/* [한국어] 현재 램프 기간 중이면 이미 경과한 램프 시간을 빼줌 */
			if ((td->runstate == TD_RAMP) && in_ramp_period(td)) {
				unsigned long ramp_left;

				ramp_left = mtime_since_now(&td->epoch);
				ramp_left = (ramp_left + 999) / 1000;  /* 밀리초 -> 초 (올림) */
				if (ramp_left <= t_eta)
					t_eta -= ramp_left;
			}
		}
		/* [한국어] 각 방향(읽기/쓰기/트림)의 설정된 rate를 합산 */
		rate_bytes = 0;
		if (td_read(td))
			rate_bytes  = td->o.rate[DDIR_READ];
		if (td_write(td))
			rate_bytes += td->o.rate[DDIR_WRITE];
		if (td_trim(td))
			rate_bytes += td->o.rate[DDIR_TRIM];

		if (rate_bytes) {
			r_eta = bytes_total / rate_bytes;               /* 총 바이트 / rate */
			r_eta += (td->o.start_delay / 1000000ULL);      /* 시작 지연 추가 */
		}

		/* [한국어] t_eta와 r_eta 중 유효한 값 선택 (둘 다 있으면 최솟값) */
		if (r_eta && t_eta)
			eta_sec = min(r_eta, t_eta);
		else if (r_eta)
			eta_sec = r_eta;
		else if (t_eta)
			eta_sec = t_eta;
		else
			eta_sec = 0;
	} else {
		/*
		 * thread is already done or waiting for fsync
		 */
		/* [한국어] 이미 완료되었거나 fsync 대기 중 - ETA는 0 */
		eta_sec = 0;
	}

	return eta_sec;
}

/*
 * [한국어] 구간별 전송 속도(대역폭) 계산
 * mtime 밀리초 동안의 바이트 변화량으로 KiB/s 단위의 속도를 계산한다.
 * unified_rw_rep == UNIFIED_MIXED이면 모든 방향을 rate[0]에 합산한다.
 *
 * 계산식: rate = (diff_bytes * 1000 / mtime_ms) / 1024  [KiB/s]
 */
static void calc_rate(int unified_rw_rep, unsigned long mtime,
		      unsigned long long *io_bytes,
		      unsigned long long *prev_io_bytes, uint64_t *rate)
{
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		unsigned long long diff, this_rate;

		diff = io_bytes[i] - prev_io_bytes[i];  /* 구간 내 전송 바이트 */
		if (mtime)
			this_rate = ((1000 * diff) / mtime) / 1024; /* KiB/s */
		else
			this_rate = 0;

		if (unified_rw_rep == UNIFIED_MIXED) {
			rate[i] = 0;           /* 개별 방향은 0 */
			rate[0] += this_rate;  /* 모두 rate[0]에 합산 */
		} else
			rate[i] = this_rate;   /* 방향별 개별 저장 */

		prev_io_bytes[i] = io_bytes[i];  /* 이전 값 갱신 */
	}
}

/*
 * [한국어] 구간별 IOPS 계산
 * mtime 밀리초 동안의 I/O 블록 수 변화량으로 IOPS를 계산한다.
 * unified_rw_rep == UNIFIED_MIXED이면 모든 방향을 iops[0]에 합산한다.
 *
 * 계산식: iops = diff_ios * 1000 / mtime_ms
 */
static void calc_iops(int unified_rw_rep, unsigned long mtime,
		      unsigned long long *io_iops,
		      unsigned long long *prev_io_iops, unsigned int *iops)
{
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		unsigned long long diff, this_iops;

		diff = io_iops[i] - prev_io_iops[i];  /* 구간 내 I/O 횟수 */
		if (mtime)
			this_iops = (diff * 1000) / mtime;
		else
			this_iops = 0;

		if (unified_rw_rep == UNIFIED_MIXED) {
			iops[i] = 0;           /* 개별 방향은 0 */
			iops[0] += this_iops;  /* 모두 iops[0]에 합산 */
		} else
			iops[i] = this_iops;   /* 방향별 개별 저장 */

		prev_io_iops[i] = io_iops[i];  /* 이전 값 갱신 */
	}
}

/*
 * Allow a little slack - if we're within 95% of the time, allow ETA.
 */
/*
 * [한국어] ETA 표시 간격에 약간의 여유(slack)를 허용
 * eta_interval_msec의 95% 이상 경과했으면 ETA 갱신을 허용한다.
 * 타이머 부정확성으로 인해 정확히 interval에 맞추기 어려우므로 5% 여유를 둔다.
 */
bool eta_time_within_slack(unsigned int time)
{
	return time > ((eta_interval_msec * 95) / 100);
}

/*
 * These are the conditions under which we might be able to skip the eta
 * calculation.
 */
/*
 * [한국어] ETA 계산을 건너뛸 수 있는 조건 확인
 * 1) 출력 형식이 NORMAL이 아니고 stdout으로 출력 중이면 건너뜀 (JSON/terse 모드)
 * 2) temp_stall_ts가 설정되었거나 eta_print가 NEVER이면 건너뜀
 * 3) stdout이 터미널이 아니고(파이프/파일 리다이렉트) eta_print가 ALWAYS가 아니면 건너뜀
 */
static bool skip_eta(void)
{
	if (!(output_format & FIO_OUTPUT_NORMAL) && f_out == stdout)
		return true;
	if (temp_stall_ts || eta_print == FIO_ETA_NEVER)
		return true;
	if (!isatty(STDOUT_FILENO) && eta_print != FIO_ETA_ALWAYS)
		return true;

	return false;
}

/*
 * Print status of the jobs we know about. This includes rate estimates,
 * ETA, thread state, etc.
 */
/*
 * [한국어] 모든 스레드의 상태를 집계하여 jobs_eta 구조체에 채우는 핵심 함수
 *
 * 처리 흐름:
 *   1) 모든 스레드를 순회하며:
 *      - 실행 중인 스레드 수, 파일 수, rate/iops 제한 등을 je에 집계
 *      - 각 스레드의 ETA를 thread_eta()로 계산
 *      - 상태 문자를 check_str_update()로 갱신
 *      - io_bytes/io_iops를 합산
 *   2) 전체 ETA 결정:
 *      - exitall_on_terminate: 가장 빨리 끝나는 스레드의 ETA (최솟값)
 *      - 그 외: stonewall 구간별 ETA 합산 + 현재 구간 내 최댓값
 *   3) 대역폭/IOPS 계산: 표시 주기에 맞춰 calc_rate/calc_iops 호출
 *   4) run_str(상태 문자열)을 je에 복사
 *
 * 반환값: true면 표시할 데이터가 있음, false면 건너뜀
 */
static bool calc_thread_status(struct jobs_eta *je, int force)
{
	int unified_rw_rep;              /* 통합 읽기/쓰기 리포팅 모드 */
	bool any_td_in_ramp;             /* 램프 기간 중인 스레드 존재 여부 */
	uint64_t rate_time, disp_time, bw_avg_time, *eta_secs;
	unsigned long long io_bytes[DDIR_RWDIR_CNT] = {};   /* 방향별 I/O 바이트 합산 */
	unsigned long long io_iops[DDIR_RWDIR_CNT] = {};    /* 방향별 I/O 횟수 합산 */
	struct timespec now;

	/* [한국어] static 변수: 함수 호출 간 이전 값을 유지하여 구간 계산에 사용 */
	static unsigned long long rate_io_bytes[DDIR_RWDIR_CNT];   /* 대역폭 로그용 이전 바이트 */
	static unsigned long long disp_io_bytes[DDIR_RWDIR_CNT];   /* 화면 표시용 이전 바이트 */
	static unsigned long long disp_io_iops[DDIR_RWDIR_CNT];    /* 화면 표시용 이전 IOPS */
	static struct timespec rate_prev_time, disp_prev_time;     /* 이전 측정 시각 */

	bool ret = true;

	/* [한국어] force가 아니고 skip 조건에 해당하면 건너뜀 (bw_log만 예외) */
	if (!force && skip_eta()) {
		if (write_bw_log)
			ret = false;  /* bw_log가 있으면 계산은 하되 화면 출력은 안 함 */
		else
			return false;
	}

	/* [한국어] 첫 호출 시 이전 시각을 현재 시각으로 초기화 */
	if (!ddir_rw_sum(rate_io_bytes))
		fill_start_time(&rate_prev_time);
	if (!ddir_rw_sum(disp_io_bytes))
		fill_start_time(&disp_prev_time);

	eta_secs = calloc(thread_number, sizeof(uint64_t));  /* 스레드별 ETA 배열 할당 */

	je->elapsed_sec = (mtime_since_genesis() + 999) / 1000;  /* 전체 경과 시간 (초) */

	bw_avg_time = ULONG_MAX;    /* 대역폭 평균 주기의 최솟값 추적 */
	unified_rw_rep = 0;
	for_each_td(td) {
		unified_rw_rep += td->o.unified_rw_rep;  /* 통합 리포팅 사용 여부 합산 */
		if (is_power_of_2(td->o.kb_base))
			je->is_pow2 = 1;             /* KB 단위가 1024 기반인지 */
		je->unit_base = td->o.unit_base;     /* 단위 기준 (비트/바이트) */
		je->sig_figs = td->o.sig_figs;      /* 유효 숫자 자릿수 */
		if (td->o.bw_avg_time < bw_avg_time)
			bw_avg_time = td->o.bw_avg_time;  /* 가장 짧은 bw_avg_time 사용 */

		/* [한국어] 실행 중인 스레드: rate/iops 제한값, 열린 파일 수 집계 */
		if (td->runstate == TD_RUNNING || td->runstate == TD_VERIFYING
		    || td->runstate == TD_FSYNCING
		    || td->runstate == TD_PRE_READING
		    || td->runstate == TD_FINISHING) {
			je->nr_running++;
			if (td_read(td)) {
				je->t_rate[0] += td->o.rate[DDIR_READ];          /* 목표 읽기 rate */
				je->t_iops[0] += td->o.rate_iops[DDIR_READ];     /* 목표 읽기 IOPS */
				je->m_rate[0] += td->o.ratemin[DDIR_READ];       /* 최소 읽기 rate */
				je->m_iops[0] += td->o.rate_iops_min[DDIR_READ]; /* 최소 읽기 IOPS */
			}
			if (td_write(td)) {
				je->t_rate[1] += td->o.rate[DDIR_WRITE];
				je->t_iops[1] += td->o.rate_iops[DDIR_WRITE];
				je->m_rate[1] += td->o.ratemin[DDIR_WRITE];
				je->m_iops[1] += td->o.rate_iops_min[DDIR_WRITE];
			}
			if (td_trim(td)) {
				je->t_rate[2] += td->o.rate[DDIR_TRIM];
				je->t_iops[2] += td->o.rate_iops[DDIR_TRIM];
				je->m_rate[2] += td->o.ratemin[DDIR_TRIM];
				je->m_iops[2] += td->o.rate_iops_min[DDIR_TRIM];
			}

			je->files_open += td->nr_open_files;   /* 열린 파일 수 합산 */
		} else if (td->runstate == TD_RAMP) {
			je->nr_running++;   /* 램프 중도 실행 중으로 카운트 */
			je->nr_ramp++;      /* 램프 중인 스레드 별도 카운트 */
		} else if (td->runstate == TD_SETTING_UP)
			je->nr_setting_up++;   /* 설정 중인 스레드 수 */
		else if (td->runstate < TD_RUNNING)
			je->nr_pending++;      /* 대기 중인 스레드 수 */

		/* [한국어] 3초 이상 경과 후부터 ETA 계산 (초기에는 데이터 부족) */
		if (je->elapsed_sec >= 3)
			eta_secs[__td_index] = thread_eta(td);
		else
			eta_secs[__td_index] = INT_MAX;  /* 아직 계산 불가 */

		check_str_update(td);  /* 상태 문자 갱신 */

		/* [한국어] 설정 완료 이후의 스레드만 I/O 통계에 포함 */
		if (td->runstate > TD_SETTING_UP) {
			int ddir;

			for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
				if (unified_rw_rep) {
					io_bytes[0] += td->io_bytes[ddir];   /* 통합: 모두 [0]에 합산 */
					io_iops[0] += td->io_blocks[ddir];
				} else {
					io_bytes[ddir] += td->io_bytes[ddir]; /* 방향별 합산 */
					io_iops[ddir] += td->io_blocks[ddir];
				}
			}
		}
	} end_for_each();

	/*
	 * [한국어] 전체 ETA 결정
	 * exitall_on_terminate 모드: 가장 빨리 끝나는 스레드가 전체를 종료시키므로 최솟값
	 * 일반 모드: stonewall 구간의 ETA는 순차 합산, 구간 내에서는 최댓값
	 */
	if (exitall_on_terminate) {
		je->eta_sec = INT_MAX;
		for_each_td_index() {
			if (eta_secs[__td_index] < je->eta_sec)
				je->eta_sec = eta_secs[__td_index];  /* 최솟값 선택 */
		} end_for_each();
	} else {
		unsigned long eta_stone = 0;  /* stonewall 구간 ETA 누적 */

		je->eta_sec = 0;
		for_each_td(td) {
			if ((td->runstate == TD_NOT_CREATED) && td->o.stonewall)
				eta_stone += eta_secs[__td_index];  /* stonewall: 순차 합산 */
			else {
				if (eta_secs[__td_index] > je->eta_sec)
					je->eta_sec = eta_secs[__td_index];  /* 구간 내 최댓값 */
			}
		} end_for_each();
		je->eta_sec += eta_stone;  /* stonewall 구간 ETA 추가 */
	}

	free(eta_secs);  /* ETA 배열 해제 */

	fio_gettime(&now, NULL);
	rate_time = mtime_since(&rate_prev_time, &now);  /* 대역폭 로그용 경과 시간 */

	/* [한국어] 램프 기간 중인 스레드가 있는지 확인 (있으면 bw 로그 기록 보류) */
	any_td_in_ramp = false;
	for_each_td(td) {
		any_td_in_ramp |= in_ramp_period(td);
	} end_for_each();

	/* [한국어] bw_avg_time 이상 경과하고 램프 중인 스레드가 없으면 대역폭 로그 기록 */
	if (write_bw_log && rate_time > bw_avg_time && !any_td_in_ramp) {
		calc_rate(unified_rw_rep, rate_time, io_bytes, rate_io_bytes,
				je->rate);
		memcpy(&rate_prev_time, &now, sizeof(now));
		regrow_agg_logs();  /* 집계 로그 버퍼 확장 */
		for_each_rw_ddir(ddir) {
			add_agg_sample(sample_val(je->rate[ddir]), ddir, 0);  /* 집계 샘플 추가 */
		}
	}

	disp_time = mtime_since(&disp_prev_time, &now);  /* 화면 표시용 경과 시간 */

	/* [한국어] 표시 간격(slack 포함)에 도달하지 않았으면 건너뜀 */
	if (!force && !eta_time_within_slack(disp_time))
		return false;

	/* [한국어] 화면 표시용 대역폭 및 IOPS 계산 */
	calc_rate(unified_rw_rep, disp_time, io_bytes, disp_io_bytes, je->rate);
	calc_iops(unified_rw_rep, disp_time, io_iops, disp_io_iops, je->iops);

	memcpy(&disp_prev_time, &now, sizeof(now));  /* 표시 시각 갱신 */

	/* [한국어] 실행 중이거나 대기 중인 스레드가 없으면 표시할 필요 없음 */
	if (!force && !je->nr_running && !je->nr_pending)
		return false;

	je->nr_threads = thread_number;               /* 총 스레드 수 */
	update_condensed_str(__run_str, run_str);      /* 압축 상태 문자열 갱신 */
	memcpy(je->run_str, run_str, strlen(run_str)); /* je에 복사 */
	return ret;
}

/*
 * [한국어] ETA 상태 표시줄의 속도/IOPS 부분 문자열 생성
 * 출력 형식: [r=100MiB/s,w=50MiB/s][r=1000,w=500 IOPS]
 *
 * has[] 배열로 각 방향(read/write/trim)에 데이터가 있는지 확인하고,
 * 데이터가 있는 방향만 출력한다.
 * 반환값: 출력된 문자 수 (snprintf 스타일)
 */
static int gen_eta_str(struct jobs_eta *je, char *p, size_t left,
		       char **rate_str, char **iops_str)
{
	static const char c[DDIR_RWDIR_CNT] = {'r', 'w', 't'};  /* 방향 약자: read, write, trim */
	bool has[DDIR_RWDIR_CNT];    /* 각 방향에 데이터가 있는지 */
	bool has_any = false;        /* 하나라도 데이터가 있는지 */
	const char *sep;             /* 구분자 (첫 항목은 빈 문자열, 이후 ",") */
	int l = 0;                   /* 출력된 총 문자 수 */

	for_each_rw_ddir(ddir) {
		has[ddir] = (je->rate[ddir] || je->iops[ddir]);
		has_any |= has[ddir];
	}
	if (!has_any)
		return 0;  /* 출력할 데이터 없음 */

	/* [한국어] 속도(rate) 부분: [r=xxx,w=xxx] */
	l += snprintf(p + l, left - l, "[");
	sep = "";
	for_each_rw_ddir(ddir) {
		if (has[ddir]) {
			l += snprintf(p + l, left - l, "%s%c=%s",
					sep, c[ddir], rate_str[ddir]);
			sep = ",";
		}
	}
	/* [한국어] IOPS 부분: [r=xxx,w=xxx IOPS] */
	l += snprintf(p + l, left - l, "][");
	sep = "";
	for_each_rw_ddir(ddir) {
		if (has[ddir]) {
			l += snprintf(p + l, left - l, "%s%c=%s",
					sep, c[ddir], iops_str[ddir]);
			sep = ",";
		}
	}
	l += snprintf(p + l, left - l, " IOPS]");

	return l;
}

/*
 * [한국어] 최종 상태 표시줄을 터미널에 출력하는 함수
 *
 * 출력 형식:
 *   Jobs: N (f=M), [min_rate]-[target_rate]: [상태문자열][진행률%][속도][IOPS][eta 남은시간]
 *
 * 동작:
 *   1) ETA 초를 문자열로 변환, 진행률 계산
 *   2) "Jobs: N (f=M)" 기본 정보 출력
 *   3) rate 제한이 있으면 min-target 범위 표시
 *   4) 상태 문자열, 진행률, 속도, IOPS, ETA를 한 줄에 조합
 *   5) '\r'로 같은 줄을 덮어쓰기 (터미널 상태줄 갱신)
 *   6) eta_new_line 설정 시 주기적으로 줄바꿈 삽입
 */
void display_thread_status(struct jobs_eta *je)
{
	static struct timespec disp_eta_new_line;  /* 마지막 줄바꿈 시각 */
	static int eta_new_line_init, eta_new_line_pending;  /* 줄바꿈 상태 */
	static int linelen_last;       /* 이전 출력 줄 길이 (짧아지면 공백으로 패딩) */
	static int eta_good;           /* ETA가 유효해진 이후 플래그 */
	char output[__THREAD_RUNSTR_SZ(REAL_MAX_JOBS) + 512], *p = output;  /* 출력 버퍼 */
	char eta_str[128];             /* ETA 문자열 버퍼 */
	double perc = 0.0;             /* 진행률 (0.0 ~ 1.0) */

	/* [한국어] ETA가 유효하고 경과 시간이 있으면 진행률 계산 */
	if (je->eta_sec != INT_MAX && je->elapsed_sec) {
		perc = (double) je->elapsed_sec / (double) (je->elapsed_sec + je->eta_sec);
		eta_to_str(eta_str, je->eta_sec);
	}

	/* [한국어] 이전에 줄바꿈이 예약되었으면 실행 */
	if (eta_new_line_pending) {
		eta_new_line_pending = 0;
		linelen_last = 0;
		p += sprintf(p, "\n");
	}

	/* [한국어] 기본 정보: 실행 중 스레드 수, 열린 파일 수 */
	p += sprintf(p, "Jobs: %d (f=%d)", je->nr_running, je->files_open);

	/* rate limits, if any */
	/* [한국어] rate 제한이 설정된 경우: "min_rate-target_rate" 형태로 표시 */
	if (je->m_rate[0] || je->m_rate[1] || je->m_rate[2] ||
	    je->t_rate[0] || je->t_rate[1] || je->t_rate[2]) {
		char *tr, *mr;

		/* 최소 rate와 목표 rate를 사람이 읽기 쉬운 문자열로 변환 */
		mr = num2str(je->m_rate[0] + je->m_rate[1] + je->m_rate[2],
				je->sig_figs, 1, je->is_pow2, N2S_BYTEPERSEC);
		tr = num2str(je->t_rate[0] + je->t_rate[1] + je->t_rate[2],
				je->sig_figs, 1, je->is_pow2, N2S_BYTEPERSEC);

		p += sprintf(p, ", %s-%s", mr, tr);
		free(tr);
		free(mr);
	} else if (je->m_iops[0] || je->m_iops[1] || je->m_iops[2] ||
		   je->t_iops[0] || je->t_iops[1] || je->t_iops[2]) {
		/* [한국어] IOPS 제한이 설정된 경우: "min_iops-target_iops IOPS" 형태 */
		p += sprintf(p, ", %d-%d IOPS",
					je->m_iops[0] + je->m_iops[1] + je->m_iops[2],
					je->t_iops[0] + je->t_iops[1] + je->t_iops[2]);
	}

	/* current run string, % done, bandwidth, iops, eta */
	/* [한국어] 상태 문자열, 진행률, 대역폭, IOPS, ETA를 조합하여 출력 */
	if (je->eta_sec != INT_MAX && je->nr_running) {
		char perc_str[32];                        /* 진행률 문자열 */
		char *iops_str[DDIR_RWDIR_CNT];           /* 방향별 IOPS 문자열 */
		char *rate_str[DDIR_RWDIR_CNT];           /* 방향별 속도 문자열 */
		size_t left;                               /* 출력 버퍼 남은 크기 */
		int l;
		int ddir;
		int linelen;                               /* 현재 줄 길이 */

		/*
		 * [한국어] 진행률 문자열 결정
		 * - ETA가 0이고 아직 유효한 적 없으면 "-.-%"
		 * - 모든 스레드가 램프 중이면 "-.-%"
		 * - ETA가 -1(계산 불가)이면 "-.-%"
		 * - 설정 중인 스레드가 있으면 그 비율만큼 진행률 감소
		 */
		if ((!je->eta_sec && !eta_good) || je->nr_ramp == je->nr_running ||
		    je->eta_sec == -1)
			strcpy(perc_str, "-.-%");
		else {
			double mult = 100.0;

			if (je->nr_setting_up && je->nr_running)
				mult *= (1.0 - (double) je->nr_setting_up / (double) je->nr_running);

			eta_good = 1;             /* 이후로는 유효한 ETA 표시 */
			perc *= mult;
			sprintf(perc_str, "%3.1f%%", perc);
		}

		/* [한국어] 각 방향의 속도/IOPS를 사람이 읽기 쉬운 문자열로 변환 */
		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			rate_str[ddir] = num2str(je->rate[ddir], 4,
						1024, je->is_pow2, je->unit_base);
			iops_str[ddir] = num2str(je->iops[ddir], 4, 1, 0, N2S_NONE);
		}

		/* [한국어] 상태 문자열 + 진행률 + 속도/IOPS + ETA를 버퍼에 조합 */
		left = sizeof(output) - (p - output) - 1;
		l = snprintf(p, left, ": [%s][%s]", je->run_str, perc_str);
		l += gen_eta_str(je, p + l, left - l, rate_str, iops_str);
		l += snprintf(p + l, left - l, "[eta %s]", eta_str);

		/* If truncation occurred adjust l so p is on the null */
		/* [한국어] 버퍼 초과(truncation) 발생 시 포인터 보정 */
		if (l >= left)
			l = left - 1;
		p += l;
		linelen = p - output;
		/* [한국어] 이전 줄보다 짧아지면 공백으로 남은 부분을 덮어씀 (잔여 문자 제거) */
		if (l >= 0 && linelen < linelen_last)
			p += sprintf(p, "%*s", linelen_last - linelen, "");
		linelen_last = linelen;

		/* [한국어] 동적 할당된 문자열 해제 */
		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			free(rate_str[ddir]);
			free(iops_str[ddir]);
		}
	}
	/* [한국어] '\r'(캐리지 리턴)으로 줄 시작으로 돌아감 -> 같은 줄 덮어쓰기 효과 */
	sprintf(p, "\r");

	printf("%s", output);

	/* [한국어] eta_new_line 설정 시: 일정 시간마다 줄바꿈을 삽입하여 이력 보존 */
	if (!eta_new_line_init) {
		fio_gettime(&disp_eta_new_line, NULL);
		eta_new_line_init = 1;                   /* 첫 호출 시 초기화 */
	} else if (eta_new_line && mtime_since_now(&disp_eta_new_line) > eta_new_line) {
		fio_gettime(&disp_eta_new_line, NULL);
		eta_new_line_pending = 1;                /* 다음 호출에서 줄바꿈 삽입 */
	}

	fflush(stdout);  /* 출력 버퍼 즉시 플러시 (터미널에 바로 표시) */
}

/*
 * [한국어] jobs_eta 구조체를 할당하고 현재 상태를 계산하여 반환
 * 서버 모드에서 클라이언트에게 상태를 전송할 때도 사용된다.
 *
 * force: true면 표시 조건을 무시하고 강제 계산
 * size: 반환된 구조체의 실제 크기 (run_str 길이에 따라 가변)
 *
 * 반환값: 할당된 jobs_eta 포인터 (호출자가 free 해야 함), 실패 시 NULL
 */
struct jobs_eta *get_jobs_eta(bool force, size_t *size)
{
	struct jobs_eta *je;

	if (!thread_number)
		return NULL;   /* 스레드가 없으면 NULL 반환 */

	*size = sizeof(*je) + THREAD_RUNSTR_SZ + 8;  /* 최대 크기로 할당 */
	je = calloc(1, *size);
	if (!je)
		return NULL;   /* 메모리 할당 실패 */

	/* [한국어] calc_thread_status가 false를 반환하면 표시할 데이터 없음 */
	if (!calc_thread_status(je, force)) {
		free(je);
		return NULL;
	}

	/* [한국어] 실제 사용된 크기로 조정 (run_str 길이 + 1) */
	*size = sizeof(*je) + strlen((char *) je->run_str) + 1;
	return je;
}

/*
 * [한국어] 상태 표시줄 출력 래퍼 함수
 * get_jobs_eta()로 상태를 계산하고, display_thread_status()로 출력한다.
 * 주기적으로 helper_thread에서 호출된다.
 */
void print_thread_status(void)
{
	struct jobs_eta *je;
	size_t size;

	je = get_jobs_eta(false, &size);
	if (je) {
		display_thread_status(je);
		free(je);       /* 사용 후 메모리 해제 */
	}
}

/*
 * [한국어] 상태 문자열 초기화 함수
 * fio 시작 시 각 스레드를 'P'(미생성, Pending) 상태로 설정한다.
 *
 * compiletime_assert: jobs_eta와 jobs_eta_packed의 크기가 같은지
 *   컴파일 타임에 검증 (네트워크 전송 시 구조체 패킹 일관성 보장)
 * DRD_IGNORE_VAR: Valgrind DRD에서 __run_str의 데이터 레이스 경고를 무시
 *   (여러 스레드가 동시에 접근하지만 의미적으로 안전)
 */
void print_status_init(int thr_number)
{
	struct jobs_eta_packed jep;

	compiletime_assert(sizeof(struct jobs_eta) == sizeof(jep), "jobs_eta");

	DRD_IGNORE_VAR(__run_str);
	__run_str[thr_number] = 'P';          /* 해당 스레드를 'P'(대기) 상태로 초기화 */
	update_condensed_str(__run_str, run_str);  /* 압축 문자열 갱신 */
}
