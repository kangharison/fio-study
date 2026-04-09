/*
 * Clock functions
 */
/*
 * [한국어] gettime.c - 시간 측정 함수 구현
 *
 * fio에서 사용하는 모든 시간 측정 기능을 구현한다.
 * 주요 기능:
 *   1) fio_gettime()     - 현재 설정된 클록 소스에서 시간을 가져오는 핵심 함수
 *   2) fio_clock_init()  - CPU 클록 캘리브레이션 및 클록 소스 초기화
 *   3) calibrate_cpu_clock() - CPU TSC 주파수를 측정하여 나노초 변환 파라미터 계산
 *   4) fio_monotonic_clocktest() - 멀티코어 환경에서 TSC 동기화 검증
 *   5) ntime_since/utime_since/mtime_since - 시간 차이 계산 유틸리티
 *
 * 지원하는 클록 소스:
 *   - CS_GTOD     : gettimeofday() - 가장 호환성 높지만 오버헤드 있음
 *   - CS_CGETTIME : clock_gettime(CLOCK_MONOTONIC) - 기본값, 단조 증가 보장
 *   - CS_CPUCLOCK : CPU TSC 직접 읽기 - 가장 빠르지만 플랫폼 의존적
 */

#include <math.h>

#include "fio.h"             /* fio 핵심 구조체 */
#include "os/os.h"           /* OS 추상화 레이어 */

/*
 * [한국어] CPU TSC(타임스탬프 카운터) 관련 전역 변수
 *
 * TSC 틱을 나노초로 변환하기 위한 파라미터들이다.
 * calibrate_cpu_clock()에서 측정/계산되며, __fio_gettime()에서 사용된다.
 *
 * 변환 공식: nsecs = ((ticks - cycles_start) * clock_mult) >> clock_shift
 * 오버플로 방지를 위해 max_cycles_shift 단위로 분할 계산한다.
 */
#if defined(ARCH_HAVE_CPU_CLOCK)
#ifndef ARCH_CPU_CLOCK_CYCLES_PER_USEC
static unsigned long long cycles_per_msec;       /* 밀리초당 CPU 사이클 수 */
static unsigned long long cycles_start;          /* 캘리브레이션 시점의 TSC 값 (기준점) */
static unsigned long long clock_mult;            /* 틱->나노초 변환 승수 */
static unsigned long long max_cycles_mask;       /* 분할 계산용 비트마스크 */
static unsigned long long nsecs_for_max_cycles;  /* max_cycles_shift 틱에 해당하는 나노초 */
static unsigned int clock_shift;                 /* 틱->나노초 변환 시프트 값 */
static unsigned int max_cycles_shift;            /* 오버플로 방지 분할 단위 */
#define MAX_CLOCK_SEC 60*60                      /* 최대 측정 가능 시간: 1시간 */
#endif
#ifdef ARCH_CPU_CLOCK_WRAPS
static unsigned int cycles_wrap;                 /* TSC 랩어라운드 감지 플래그 */
#endif
#endif
bool tsc_reliable = false;  /* TSC가 신뢰할 수 있는지 여부 (아키텍처에서 설정) */

/*
 * [한국어] tv_valid - 스레드별 클록 경고 상태
 *
 * CPU 클록 랩어라운드가 두 번 발생하면 경고를 출력하는데,
 * 스레드당 한 번만 경고하기 위해 warned 플래그를 사용한다.
 */
struct tv_valid {
	int warned;            /* 이중 랩어라운드 경고 출력 여부 */
};
#ifdef ARCH_HAVE_CPU_CLOCK
#ifdef CONFIG_TLS_THREAD
static __thread struct tv_valid static_tv_valid;  /* TLS 지원 시 스레드 로컬 변수 */
#else
static pthread_key_t tv_tls_key;                  /* TLS 미지원 시 pthread 키 사용 */
#endif
#endif

/* [한국어] 현재 선택된 클록 소스 및 상태 */
enum fio_cs fio_clock_source = FIO_PREFERRED_CLOCK_SOURCE;  /* 현재 클록 소스 */
int fio_clock_source_set = 0;                               /* 사용자가 명시적으로 설정했는지 */
static enum fio_cs fio_clock_source_inited = CS_INVAL;      /* 초기화 완료된 클록 소스 */

/*
 * [한국어] FIO_DEBUG_TIME - 시간 함수 호출 디버깅/프로파일링
 *
 * 이 모드가 활성화되면 fio_gettime()의 호출자(caller) 주소를 해시 테이블에 기록하여
 * 어디서 얼마나 자주 시간 함수를 호출하는지 추적할 수 있다.
 */
#ifdef FIO_DEBUG_TIME

#define HASH_BITS	8
#define HASH_SIZE	(1 << HASH_BITS)  /* 256개 버킷 */

static struct flist_head hash[HASH_SIZE];  /* 호출자 해시 테이블 */
static int gtod_inited;                    /* 해시 테이블 초기화 여부 */

/* [한국어] 호출자 정보를 저장하는 로그 항목 */
struct gtod_log {
	struct flist_head list;   /* 해시 버킷 내 연결 리스트 */
	void *caller;             /* 호출자의 코드 주소 */
	unsigned long calls;      /* 호출 횟수 */
};

/* [한국어] 해시 테이블에서 호출자 주소로 로그 항목을 찾음 */
static struct gtod_log *find_hash(void *caller)
{
	unsigned long h = hash_ptr(caller, HASH_BITS);
	struct flist_head *entry;

	flist_for_each(entry, &hash[h]) {
		struct gtod_log *log = flist_entry(entry, struct gtod_log,
									list);

		if (log->caller == caller)
			return log;
	}

	return NULL;
}

/* [한국어] 호출자의 호출 횟수를 증가시킴. 첫 호출이면 새 항목 생성 */
static void inc_caller(void *caller)
{
	struct gtod_log *log = find_hash(caller);

	if (!log) {
		unsigned long h;

		log = malloc(sizeof(*log));
		INIT_FLIST_HEAD(&log->list);
		log->caller = caller;
		log->calls = 0;

		h = hash_ptr(caller, HASH_BITS);
		flist_add_tail(&log->list, &hash[h]);
	}

	log->calls++;
}

/* [한국어] 디버그 모드에서 fio_gettime 호출을 기록 */
static void gtod_log_caller(void *caller)
{
	if (gtod_inited)
		inc_caller(caller);
}

/* [한국어] 프로그램 종료 시 모든 호출자별 통계를 출력 */
static void fio_exit fio_dump_gtod(void)
{
	unsigned long total_calls = 0;
	int i;

	for (i = 0; i < HASH_SIZE; i++) {
		struct flist_head *entry;
		struct gtod_log *log;

		flist_for_each(entry, &hash[i]) {
			log = flist_entry(entry, struct gtod_log, list);

			printf("function %p, calls %lu\n", log->caller,
								log->calls);
			total_calls += log->calls;
		}
	}

	printf("Total %lu gettimeofday\n", total_calls);
}

/* [한국어] 프로그램 시작 시 해시 테이블 초기화 */
static void fio_init gtod_init(void)
{
	int i;

	for (i = 0; i < HASH_SIZE; i++)
		INIT_FLIST_HEAD(&hash[i]);

	gtod_inited = 1;
}

#endif /* FIO_DEBUG_TIME */

/*
 * Queries the value of the monotonic clock if a monotonic clock is available
 * or the wall clock time if no monotonic clock is available. Returns 0 if
 * querying the clock succeeded or -1 if querying the clock failed.
 */
/* [한국어] 모노토닉(단조 증가) 시간을 가져옴. 가능하면 CLOCK_MONOTONIC, 아니면 CLOCK_REALTIME 사용 */
int fio_get_mono_time(struct timespec *ts)
{
	int ret;

#if defined(CONFIG_CLOCK_MONOTONIC)
	ret = clock_gettime(CLOCK_MONOTONIC, ts);
#else
	ret = clock_gettime(CLOCK_REALTIME, ts);
#endif
	assert(ret <= 0);
	return ret;
}

/*
 * [한국어] 내부 시간 획득 함수 - 클록 소스별 분기 처리
 *
 * CS_GTOD     : gettimeofday()를 호출하고 timeval -> timespec 변환
 * CS_CGETTIME : fio_get_mono_time()으로 clock_gettime() 호출
 * CS_CPUCLOCK : CPU TSC를 직접 읽어 나노초로 변환
 *               변환식: nsecs = ((tsc - cycles_start) * clock_mult) >> clock_shift
 *               오버플로 방지를 위해 max_cycles_shift 단위로 분할
 */
static void __fio_gettime(struct timespec *tp)
{
	switch (fio_clock_source) {
#ifdef CONFIG_GETTIMEOFDAY
	case CS_GTOD: {
		struct timeval tv;
		gettimeofday(&tv, NULL);

		tp->tv_sec = tv.tv_sec;
		tp->tv_nsec = tv.tv_usec * 1000;  /* 마이크로초 -> 나노초 */
		break;
		}
#endif
	case CS_CGETTIME: {
		if (fio_get_mono_time(tp) < 0) {
			log_err("fio: fio_get_mono_time() fails\n");
			assert(0);
		}
		break;
		}
#ifdef ARCH_HAVE_CPU_CLOCK
	case CS_CPUCLOCK: {
		uint64_t nsecs, t, multiples;
		struct tv_valid *tv;

#ifdef CONFIG_TLS_THREAD
		tv = &static_tv_valid;
#else
		tv = pthread_getspecific(tv_tls_key);
#endif

		t = get_cpu_clock();   /* TSC 레지스터 직접 읽기 */
#ifdef ARCH_CPU_CLOCK_WRAPS
		/* TSC 랩어라운드 감지 */
		if (t < cycles_start && !cycles_wrap)
			cycles_wrap = 1;
		else if (cycles_wrap && t >= cycles_start && !tv->warned) {
			log_err("fio: double CPU clock wrap\n");
			tv->warned = 1;
		}
#endif
#ifdef ARCH_CPU_CLOCK_CYCLES_PER_USEC
		nsecs = t / ARCH_CPU_CLOCK_CYCLES_PER_USEC * 1000;
#else
		/* TSC 틱을 나노초로 변환 (오버플로 방지 분할 계산) */
		t -= cycles_start;
		multiples = t >> max_cycles_shift;         /* 큰 단위 횟수 */
		nsecs = multiples * nsecs_for_max_cycles;  /* 큰 단위의 나노초 */
		nsecs += ((t & max_cycles_mask) * clock_mult) >> clock_shift; /* 나머지 */
#endif
		tp->tv_sec = nsecs / 1000000000ULL;
		tp->tv_nsec = nsecs % 1000000000ULL;
		break;
		}
#endif
	default:
		log_err("fio: invalid clock source %d\n", fio_clock_source);
		break;
	}
}

/*
 * [한국어] fio의 메인 시간 획득 함수
 *
 * 먼저 gtod 오프로드 스레드의 캐시된 시간을 확인하고 (fio_gettime_offload),
 * 사용 불가하면 __fio_gettime()으로 직접 시간을 읽는다.
 * FIO_DEBUG_TIME 모드에서는 호출자 주소를 기록한다.
 */
#ifdef FIO_DEBUG_TIME
void fio_gettime(struct timespec *tp, void *caller)
#else
void fio_gettime(struct timespec *tp, void fio_unused *caller)
#endif
{
#ifdef FIO_DEBUG_TIME
	if (!caller)
		caller = __builtin_return_address(0);

	gtod_log_caller(caller);
#endif
	if (fio_unlikely(fio_gettime_offload(tp)))
		return;

	__fio_gettime(tp);
}

/*
 * [한국어] CPU TSC 주파수 측정 (밀리초당 사이클 수)
 *
 * clock_gettime()과 TSC를 동시에 읽어 경과 시간 대비 사이클 수를 계산한다.
 * 최소 1.28ms 이상 측정하여 정확도를 확보한다.
 */
#if defined(ARCH_HAVE_CPU_CLOCK) && !defined(ARCH_CPU_CLOCK_CYCLES_PER_USEC)
static unsigned long get_cycles_per_msec(void)
{
	struct timespec s, e;
	uint64_t c_s, c_e;
	uint64_t elapsed;

	fio_get_mono_time(&s);

	c_s = get_cpu_clock();
	do {
		fio_get_mono_time(&e);
		c_e = get_cpu_clock();

		elapsed = ntime_since(&s, &e);
		if (elapsed >= 1280000)   /* 1.28ms 이상 경과하면 충분 */
			break;
	} while (1);

	return (c_e - c_s) * 1000000 / elapsed;
}

#define NR_TIME_ITERS	50   /* 캘리브레이션 반복 횟수 */

/*
 * [한국어] CPU 클록 캘리브레이션
 *
 * get_cycles_per_msec()를 NR_TIME_ITERS회 반복 측정하여
 * 표준편차 기반으로 이상치를 제거하고 평균값을 구한다.
 * 이 값으로 TSC 틱 -> 나노초 변환에 필요한 파라미터를 계산한다:
 *   - clock_mult, clock_shift: 곱셈과 시프트로 나눗셈을 대체
 *   - max_cycles_shift, max_cycles_mask: 64비트 오버플로 방지용 분할 단위
 *   - nsecs_for_max_cycles: 분할 단위당 나노초
 */
static int calibrate_cpu_clock(void)
{
	double delta, mean, S;
	uint64_t minc, maxc, avg, cycles[NR_TIME_ITERS];
	int i, samples, sft = 0;
	unsigned long long tmp, max_ticks, max_mult;

	cycles[0] = get_cycles_per_msec();
	S = delta = mean = 0.0;
	for (i = 0; i < NR_TIME_ITERS; i++) {
		cycles[i] = get_cycles_per_msec();
		delta = cycles[i] - mean;
		if (delta) {
			mean += delta / (i + 1.0);
			S += delta * (cycles[i] - mean);
		}
	}

	/*
	 * The most common platform clock breakage is returning zero
	 * indefinitely. Check for that and return failure.
	 */
	/* [한국어] TSC가 항상 0을 반환하는 고장 상태 감지 */
	if (!cycles[0] && !cycles[NR_TIME_ITERS - 1])
		return 1;

	S = sqrt(S / (NR_TIME_ITERS - 1.0));  /* 표준편차 계산 */

	/* 이상치를 제거한 트리밍 평균 계산 */
	minc = -1ULL;
	maxc = samples = avg = 0;
	for (i = 0; i < NR_TIME_ITERS; i++) {
		double this = cycles[i];

		minc = min(cycles[i], minc);
		maxc = max(cycles[i], maxc);

		if ((fmax(this, mean) - fmin(this, mean)) > S)
			continue;   /* 표준편차를 벗어나는 값은 제외 */
		samples++;
		avg += this;
	}

	S /= (double) NR_TIME_ITERS;

	for (i = 0; i < NR_TIME_ITERS; i++)
		dprint(FD_TIME, "cycles[%d]=%llu\n", i, (unsigned long long) cycles[i]);

	avg /= samples;
	cycles_per_msec = avg;
	dprint(FD_TIME, "min=%llu, max=%llu, mean=%f, S=%f, N=%d\n",
			(unsigned long long) minc,
			(unsigned long long) maxc, mean, S, NR_TIME_ITERS);
	dprint(FD_TIME, "trimmed mean=%llu, N=%d\n", (unsigned long long) avg, samples);

	/* clock_mult와 clock_shift 계산: nsecs = (ticks * clock_mult) >> clock_shift */
	max_ticks = MAX_CLOCK_SEC * cycles_per_msec * 1000ULL;
	max_mult = ULLONG_MAX / max_ticks;
	dprint(FD_TIME, "max_ticks=%llu, __builtin_clzll=%d, "
			"max_mult=%llu\n", max_ticks,
			__builtin_clzll(max_ticks), max_mult);

        /*
         * Find the largest shift count that will produce
         * a multiplier that does not exceed max_mult
         */
	/* [한국어] 오버플로 없이 가능한 최대 시프트 값을 찾아 정밀도를 극대화 */
        tmp = max_mult * cycles_per_msec / 1000000;
        while (tmp > 1) {
                tmp >>= 1;
                sft++;
                dprint(FD_TIME, "tmp=%llu, sft=%u\n", tmp, sft);
        }

	clock_shift = sft;
	clock_mult = (1ULL << sft) * 1000000 / cycles_per_msec;
	dprint(FD_TIME, "clock_shift=%u, clock_mult=%llu\n", clock_shift,
							clock_mult);

	/*
	 * Find the greatest power of 2 clock ticks that is less than the
	 * ticks in MAX_CLOCK_SEC
	 */
	/* [한국어] 오버플로 방지를 위한 분할 단위 계산: 2^max_cycles_shift */
	max_cycles_shift = max_cycles_mask = 0;
	tmp = MAX_CLOCK_SEC * 1000ULL * cycles_per_msec;
	dprint(FD_TIME, "tmp=%llu, max_cycles_shift=%u\n", tmp,
							max_cycles_shift);
	while (tmp > 1) {
		tmp >>= 1;
		max_cycles_shift++;
		dprint(FD_TIME, "tmp=%llu, max_cycles_shift=%u\n", tmp, max_cycles_shift);
	}
	/*
	 * if use use (1ULL << max_cycles_shift) * 1000 / cycles_per_msec
	 * here we will have a discontinuity every
	 * (1ULL << max_cycles_shift) cycles
	 */
	/* [한국어] 분할 단위당 나노초를 clock_mult/clock_shift로 계산하여 불연속성 방지 */
	nsecs_for_max_cycles = ((1ULL << max_cycles_shift) * clock_mult)
					>> clock_shift;

	/* Use a bitmask to calculate ticks % (1ULL << max_cycles_shift) */
	/* [한국어] 나머지 계산을 위한 비트마스크 생성 */
	for (tmp = 0; tmp < max_cycles_shift; tmp++)
		max_cycles_mask |= 1ULL << tmp;

	dprint(FD_TIME, "max_cycles_shift=%u, 2^max_cycles_shift=%llu, "
			"nsecs_for_max_cycles=%llu, "
			"max_cycles_mask=%016llx\n",
			max_cycles_shift, (1ULL << max_cycles_shift),
			nsecs_for_max_cycles, max_cycles_mask);

	cycles_start = get_cpu_clock();  /* 시간 기준점 설정 */
	dprint(FD_TIME, "cycles_start=%llu\n", cycles_start);
	return 0;
}
#else
static int calibrate_cpu_clock(void)
{
#ifdef ARCH_CPU_CLOCK_CYCLES_PER_USEC
	return 0;
#else
	return 1;
#endif
}
#endif // ARCH_HAVE_CPU_CLOCK

/*
 * [한국어] 스레드 로컬 클록 상태 초기화
 *
 * CONFIG_TLS_THREAD가 없는 경우 pthread_setspecific()으로
 * tv_valid 구조체를 스레드 로컬 저장소에 할당한다.
 */
#if defined(ARCH_HAVE_CPU_CLOCK) && !defined(CONFIG_TLS_THREAD)
void fio_local_clock_init(void)
{
	struct tv_valid *t;

	t = calloc(1, sizeof(*t));
	if (pthread_setspecific(tv_tls_key, t)) {
		log_err("fio: can't set TLS key\n");
		assert(0);
	}
}

/* [한국어] pthread 키 소멸자 - 스레드 종료 시 tv_valid 메모리 해제 */
static void kill_tv_tls_key(void *data)
{
	free(data);
}
#else
void fio_local_clock_init(void)
{
}
#endif

/*
 * [한국어] 클록 초기화 - 전체 클록 서브시스템 설정
 *
 * 1) pthread TLS 키 생성 (필요시)
 * 2) CPU 클록 캘리브레이션 실행
 * 3) TSC가 신뢰할 수 있으면 CS_CPUCLOCK으로 자동 전환
 *    (단, 사용자가 명시적으로 설정하지 않았고 모노토닉 테스트를 통과한 경우)
 */
void fio_clock_init(void)
{
	if (fio_clock_source == fio_clock_source_inited)
		return;  /* 이미 초기화됨 */

#if defined(ARCH_HAVE_CPU_CLOCK) && !defined(CONFIG_TLS_THREAD)
	if (pthread_key_create(&tv_tls_key, kill_tv_tls_key))
		log_err("fio: can't create TLS key\n");
#endif

	fio_clock_source_inited = fio_clock_source;

	if (calibrate_cpu_clock())
		tsc_reliable = false;

	/*
	 * If the arch sets tsc_reliable != 0, then it must be good enough
	 * to use as THE clock source. For x86 CPUs, this means the TSC
	 * runs at a constant rate and is synced across CPU cores.
	 */
	/* [한국어] TSC가 신뢰할 수 있고, 사용자 설정이 없으며, 동기화 테스트 통과 시 TSC 사용 */
	if (tsc_reliable) {
		if (!fio_clock_source_set && !fio_monotonic_clocktest(0))
			fio_clock_source = CS_CPUCLOCK;
	} else if (fio_clock_source == CS_CPUCLOCK)
		log_info("fio: clocksource=cpu may not be reliable\n");
	dprint(FD_TIME, "gettime: clocksource=%d\n", (int) fio_clock_source);
}

/*
 * [한국어] 두 timespec 사이의 차이를 나노초로 반환
 *
 * 일부 커널에서 시간 역전(time warp) 버그가 있을 수 있으므로
 * 음수 결과는 0으로 처리한다.
 */
uint64_t ntime_since(const struct timespec *s, const struct timespec *e)
{
	int64_t sec, nsec;

	sec = e->tv_sec - s->tv_sec;
	nsec = e->tv_nsec - s->tv_nsec;
	if (sec > 0 && nsec < 0) {
		sec--;
		nsec += 1000000000LL;
	}

       /*
	* time warp bug on some kernels?
	*/
	if (sec < 0 || (sec == 0 && nsec < 0))
		return 0;

	return nsec + (sec * 1000000000LL);
}

/* [한국어] 지정 시점부터 현재까지의 경과 시간을 나노초로 반환 */
uint64_t ntime_since_now(const struct timespec *s)
{
	struct timespec now;

	fio_gettime(&now, NULL);
	return ntime_since(s, &now);
}

/* [한국어] 두 timespec 사이의 차이를 마이크로초로 반환 */
uint64_t utime_since(const struct timespec *s, const struct timespec *e)
{
	int64_t sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_nsec - s->tv_nsec) / 1000;
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	/*
	 * time warp bug on some kernels?
	 */
	if (sec < 0 || (sec == 0 && usec < 0))
		return 0;

	return usec + (sec * 1000000);
}

/* [한국어] 지정 시점부터 현재까지의 경과 시간을 마이크로초로 반환 */
uint64_t utime_since_now(const struct timespec *s)
{
	struct timespec t;
#ifdef FIO_DEBUG_TIME
	void *p = __builtin_return_address(0);

	fio_gettime(&t, p);
#else
	fio_gettime(&t, NULL);
#endif

	return utime_since(s, &t);
}

/* [한국어] 두 timeval 사이의 차이를 밀리초로 반환 (레거시 timeval 인터페이스용) */
uint64_t mtime_since_tv(const struct timeval *s, const struct timeval *e)
{
	int64_t sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	if (sec < 0 || (sec == 0 && usec < 0))
		return 0;

	sec *= 1000;
	usec /= 1000;
	return sec + usec;
}

/* [한국어] 지정 시점부터 현재까지의 경과 시간을 밀리초로 반환 */
uint64_t mtime_since_now(const struct timespec *s)
{
	struct timespec t;
#ifdef FIO_DEBUG_TIME
	void *p = __builtin_return_address(0);

	fio_gettime(&t, p);
#else
	fio_gettime(&t, NULL);
#endif

	return mtime_since(s, &t);
}

/*
 * Returns *e - *s in milliseconds as a signed integer. Note: rounding is
 * asymmetric. If the difference yields +1 ns then 0 is returned. If the
 * difference yields -1 ns then -1 is returned.
 */
/* [한국어] 두 timespec 차이를 부호 있는 밀리초로 반환. 비대칭 반올림 주의 */
int64_t rel_time_since(const struct timespec *s, const struct timespec *e)
{
	int64_t sec, nsec;

	sec = e->tv_sec - s->tv_sec;
	nsec = e->tv_nsec - s->tv_nsec;
	if (nsec < 0) {
		sec--;
		nsec += 1000ULL * 1000 * 1000;
	}
	assert(0 <= nsec && nsec < 1000ULL * 1000 * 1000);

	return sec * 1000 + nsec / (1000 * 1000);
}

/*
 * Returns *e - *s in milliseconds as an unsigned integer. Returns 0 if
 * *e < *s.
 */
/* [한국어] 두 timespec 차이를 부호 없는 밀리초로 반환. 음수면 0 */
uint64_t mtime_since(const struct timespec *s, const struct timespec *e)
{
	return max(rel_time_since(s, e), (int64_t)0);
}

/* [한국어] 지정 시점부터 현재까지의 경과 시간을 초 단위로 반환 */
uint64_t time_since_now(const struct timespec *s)
{
	return mtime_since_now(s) / 1000;
}

/*
 * [한국어] 멀티코어 TSC 동기화 테스트
 *
 * 이 섹션은 CPU 간 TSC가 동기화되어 있는지 검증한다.
 * 각 CPU에서 스레드를 실행하여 atomic 시퀀스 번호와 함께 TSC를 기록하고,
 * 시퀀스 순서대로 정렬한 뒤 TSC가 단조 증가하는지 확인한다.
 * TSC 역전이 발견되면 CS_CPUCLOCK은 신뢰할 수 없는 것으로 판단한다.
 */
#if defined(FIO_HAVE_CPU_AFFINITY) && defined(ARCH_HAVE_CPU_CLOCK)  && \
    defined(CONFIG_SYNC_SYNC) && defined(CONFIG_CMP_SWAP)

#define CLOCK_ENTRIES_DEBUG	100000  /* 디버그 모드 샘플 수 */
#define CLOCK_ENTRIES_TEST	1000    /* 일반 테스트 샘플 수 */

/* [한국어] 클록 테스트 항목 - 시퀀스 번호, CPU ID, TSC 값을 기록 */
struct clock_entry {
	uint32_t seq;    /* 전역 atomic 시퀀스 번호 */
	uint32_t cpu;    /* 이 항목을 기록한 CPU 번호 */
	uint64_t tsc;    /* 기록 시점의 TSC 값 */
};

/* [한국어] 클록 테스트 스레드 정보 */
struct clock_thread {
	pthread_t thread;             /* 스레드 핸들 */
	int cpu;                      /* 바인딩할 CPU 번호 */
	int debug;                    /* 디버그 출력 여부 */
	struct fio_sem lock;          /* 동기화용 세마포어 (모든 스레드 동시 시작) */
	unsigned long nr_entries;     /* 기록할 항목 수 */
	uint32_t *seq;                /* 전역 시퀀스 번호 포인터 (공유) */
	struct clock_entry *entries;  /* 결과 저장 배열 */
};

/* [한국어] atomic compare-and-swap - 시퀀스 번호의 원자적 증가에 사용 */
static inline uint32_t atomic32_compare_and_swap(uint32_t *ptr, uint32_t old,
						 uint32_t new)
{
	return __sync_val_compare_and_swap(ptr, old, new);
}

/*
 * [한국어] 클록 테스트 스레드 함수
 *
 * 지정된 CPU에 바인딩한 후, 세마포어 대기로 모든 스레드가 동시에 시작한다.
 * atomic CAS로 전역 시퀀스를 증가시키면서 TSC를 기록하여
 * 나중에 CPU 간 TSC 동기화를 검증할 수 있게 한다.
 */
static void *clock_thread_fn(void *data)
{
	struct clock_thread *t = data;
	struct clock_entry *c;
	os_cpu_mask_t cpu_mask;
	unsigned long long first;
	int i;

	if (fio_cpuset_init(&cpu_mask)) {
		int __err = errno;

		log_err("clock cpuset init failed: %s\n", strerror(__err));
		goto err_out;
	}

	fio_cpu_set(&cpu_mask, t->cpu);

	/* CPU 어피니티 설정: 이 스레드를 특정 CPU에 고정 */
	if (fio_setaffinity(gettid(), cpu_mask) == -1) {
		int __err = errno;

		log_err("clock setaffinity failed: %s\n", strerror(__err));
		goto err;
	}

	fio_sem_down(&t->lock);  /* 모든 스레드가 준비될 때까지 대기 */

	first = get_cpu_clock();
	c = &t->entries[0];
	for (i = 0; i < t->nr_entries; i++, c++) {
		uint32_t seq;
		uint64_t tsc;

		c->cpu = t->cpu;
		do {
			seq = *t->seq;
			if (seq == UINT_MAX)
				break;
			tsc_barrier();           /* 메모리 배리어 */
			tsc = get_cpu_clock();   /* TSC 읽기 */
		} while (seq != atomic32_compare_and_swap(t->seq, seq, seq + 1));

		if (seq == UINT_MAX)
			break;

		c->seq = seq;
		c->tsc = tsc;
	}

	if (t->debug) {
		unsigned long long clocks;

		clocks = t->entries[i - 1].tsc - t->entries[0].tsc;
		log_info("cs: cpu%3d: %llu clocks seen, first %llu\n", t->cpu,
							clocks, first);
	}

	/*
	 * The most common platform clock breakage is returning zero
	 * indefinitely. Check for that and return failure.
	 */
	/* [한국어] TSC가 항상 0을 반환하는 고장 상태 감지 */
	if (i > 1 && !t->entries[i - 1].tsc && !t->entries[0].tsc)
		goto err;

	fio_cpuset_exit(&cpu_mask);
	return NULL;
err:
	fio_cpuset_exit(&cpu_mask);
err_out:
	return (void *) 1;
}

/* [한국어] 시퀀스 번호 기준으로 clock_entry를 정렬하는 비교 함수 */
static int clock_cmp(const void *p1, const void *p2)
{
	const struct clock_entry *c1 = p1;
	const struct clock_entry *c2 = p2;

	if (c1->seq == c2->seq)
		log_err("cs: bug in atomic sequence!\n");

	return c1->seq - c2->seq;
}

/*
 * [한국어] 모노토닉 클록 테스트 - 멀티코어 TSC 동기화 검증
 *
 * 흐름:
 *   1) 각 CPU에 대해 clock_thread 생성 및 CPU 어피니티 설정
 *   2) 세마포어로 모든 스레드를 동시에 시작
 *   3) 각 스레드가 atomic CAS로 시퀀스 번호를 증가시키며 TSC 기록
 *   4) 모든 결과를 시퀀스 번호로 정렬
 *   5) 정렬된 순서에서 TSC가 단조 증가하는지 검증
 *   6) TSC 역전이 발견되면 실패 반환
 */
int fio_monotonic_clocktest(int debug)
{
	struct clock_thread *cthreads;
	unsigned int seen_cpus, nr_cpus = cpus_configured();
	struct clock_entry *entries;
	unsigned long nr_entries, tentries, failed = 0;
	struct clock_entry *prev, *this;
	uint32_t seq = 0;
	unsigned int i;
	os_cpu_mask_t mask;

#ifdef FIO_HAVE_GET_THREAD_AFFINITY
	fio_get_thread_affinity(mask);
#else
	memset(&mask, 0, sizeof(mask));
	for (i = 0; i < nr_cpus; i++)
		fio_cpu_set(&mask, i);
#endif

	if (debug) {
		log_info("cs: reliable_tsc: %s\n", tsc_reliable ? "yes" : "no");

#ifdef FIO_INC_DEBUG
		fio_debug |= 1U << FD_TIME;
#endif
		nr_entries = CLOCK_ENTRIES_DEBUG;
	} else
		nr_entries = CLOCK_ENTRIES_TEST;

	calibrate_cpu_clock();

	if (debug) {
#ifdef FIO_INC_DEBUG
		fio_debug &= ~(1U << FD_TIME);
#endif
	}

	cthreads = malloc(nr_cpus * sizeof(struct clock_thread));
	tentries = nr_entries * nr_cpus;
	entries = malloc(tentries * sizeof(struct clock_entry));

	if (debug)
		log_info("cs: Testing %u CPUs\n", nr_cpus);

	/* 각 CPU에 대해 테스트 스레드 생성 */
	seen_cpus = 0;
	for (i = 0; i < nr_cpus; i++) {
		struct clock_thread *t = &cthreads[i];

		if (!fio_cpu_isset(&mask, i))
			continue;
		t->cpu = i;
		t->debug = debug;
		t->seq = &seq;
		t->nr_entries = nr_entries;
		t->entries = &entries[seen_cpus * nr_entries];
		__fio_sem_init(&t->lock, FIO_SEM_LOCKED);
		if (pthread_create(&t->thread, NULL, clock_thread_fn, t)) {
			failed++;
			nr_cpus = i;
			break;
		}
		seen_cpus++;
	}

	/* 모든 스레드를 동시에 시작 */
	for (i = 0; i < nr_cpus; i++) {
		struct clock_thread *t = &cthreads[i];

		if (!fio_cpu_isset(&mask, i))
			continue;
		fio_sem_up(&t->lock);
	}

	/* 스레드 완료 대기 및 결과 수집 */
	for (i = 0; i < nr_cpus; i++) {
		struct clock_thread *t = &cthreads[i];
		void *ret;

		if (!fio_cpu_isset(&mask, i))
			continue;
		pthread_join(t->thread, &ret);
		if (ret)
			failed++;
		__fio_sem_remove(&t->lock);
	}
	free(cthreads);

	if (failed) {
		if (debug)
			log_err("Clocksource test: %lu threads failed\n", failed);
		goto err;
	}

	/* 시퀀스 번호로 정렬하여 시간순 재구성 */
	tentries = nr_entries * seen_cpus;
	qsort(entries, tentries, sizeof(struct clock_entry), clock_cmp);

	/* silence silly gcc */
	/* [한국어] 정렬된 순서에서 TSC 단조 증가 여부 검증 */
	prev = NULL;
	for (failed = i = 0; i < tentries; i++) {
		this = &entries[i];

		if (!i) {
			prev = this;
			continue;
		}

		/* TSC 역전 감지: 이전 항목의 TSC가 현재보다 큰 경우 */
		if (prev->tsc > this->tsc) {
			uint64_t diff = prev->tsc - this->tsc;

			if (!debug) {
				failed++;
				break;
			}

			log_info("cs: CPU clock mismatch (diff=%llu):\n",
						(unsigned long long) diff);
			log_info("\t CPU%3u: TSC=%llu, SEQ=%u\n", prev->cpu, (unsigned long long) prev->tsc, prev->seq);
			log_info("\t CPU%3u: TSC=%llu, SEQ=%u\n", this->cpu, (unsigned long long) this->tsc, this->seq);
			failed++;
		}

		prev = this;
	}

	if (debug) {
		if (failed)
			log_info("cs: Failed: %lu\n", failed);
		else
			log_info("cs: Pass!\n");
	}
err:
	free(entries);
	return !!failed;
}

#else /* defined(FIO_HAVE_CPU_AFFINITY) && defined(ARCH_HAVE_CPU_CLOCK) */

/* [한국어] CPU 어피니티 또는 CPU 클록 미지원 플랫폼용 스텁 */
int fio_monotonic_clocktest(int debug)
{
	if (debug)
		log_info("cs: current platform does not support CPU clocks\n");
	return 1;
}

#endif
