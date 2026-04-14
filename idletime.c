/*
 * [한국어] idletime.c - CPU 유휴 시간 프로파일링
 *
 * 이 파일은 fio 실행 중 각 CPU의 유휴율(idleness)을 측정하는 기능을 구현한다.
 * 주요 원리:
 *   - 각 CPU에 SCHED_IDLE 우선순위의 스레드를 배치하여 단위 작업을 반복 수행
 *   - 유휴 시간이 많을수록 더 많은 루프를 실행하므로, 완료 루프 수로 유휴율 추정
 *   - 보정(calibration) 단계에서 단위 작업의 기준 시간을 측정
 *
 * 주요 함수:
 *   1) calibrate_unit()      - 단위 작업의 기준 실행 시간 측정
 *   2) idle_prof_thread_fn() - 유휴 프로파일링 스레드 메인 함수
 *   3) fio_idle_prof_init()  - 프로파일링 초기화 (스레드 생성, 보정)
 *   4) fio_idle_prof_start() - 프로파일링 시작 (스레드 잠금 해제)
 *   5) fio_idle_prof_stop()  - 프로파일링 중지 및 유휴율 계산
 *   6) show_idle_prof_stats() - 결과 출력 (텍스트 및 JSON)
 
 * === 파일의 역할 ===
 * fio 실행 중 각 CPU의 유휴율을 측정. SCHED_IDLE 스레드로 유휴 시간 추정.
 *
 * === 전체 아키텍처에서의 위치 ===
 * backend.c에서 fio_idle_prof_init/start/stop으로 프로파일링 제어.
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: 프로파일링 시작/중지
 * - idletime.h: API 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_idle_prof_init(): 초기화 (스레드 생성, 보정)
 * - fio_idle_prof_start/stop(): 시작/중지
 */
#include <math.h>
#include "fio.h"
#include "json.h"
#include "idletime.h"

/* [한국어] ipc - 유휴 프로파일링 전역 공유 상태.
 *          volatile로 선언하여 스레드 간 가시성을 보장한다. */
static volatile struct idle_prof_common ipc;

/*
 * Get time to complete an unit work on a particular cpu.
 * The minimum number in CALIBRATE_RUNS runs is returned.
 */
/* [한국어] calibrate_unit - 특정 CPU에서 단위 작업(page_size 바이트 쓰기)의 최소 실행 시간 측정.
 *          CALIBRATE_RUNS회 반복 중 최솟값을 반환한다.
 *          CALIBRATE_SCALE을 곱하여 분산을 줄인다. */
static double calibrate_unit(unsigned char *data)
{
	unsigned long t, i, j, k;
	struct timespec tps;
	double tunit = 0.0;

	for (i = 0; i < CALIBRATE_RUNS; i++) {

		fio_gettime(&tps, NULL);
		/* scale for less variance */
		for (j = 0; j < CALIBRATE_SCALE; j++) {
			/* unit of work */
			for (k=0; k < page_size; k++) {
				data[(k + j) % page_size] = k % 256;
				/*
				 * we won't see STOP here. this is to match
				 * the same statement in the profiling loop.
				 */
				if (ipc.status == IDLE_PROF_STATUS_PROF_STOP)
					return 0.0;
			}
		}

		t = utime_since_now(&tps);
		if (!t)
			continue;

		/* get the minimum time to complete CALIBRATE_SCALE units */
		if ((i == 0) || ((double)t < tunit))
			tunit = (double)t;
	}

	return tunit / CALIBRATE_SCALE;
}

/* [한국어] free_cpu_affinity - CPU 친화성 마스크 해제 */
static void free_cpu_affinity(struct idle_prof_thread *ipt)
{
#if defined(FIO_HAVE_CPU_AFFINITY)
	fio_cpuset_exit(&ipt->cpu_mask);
#endif
}

/* [한국어] set_cpu_affinity - 스레드를 지정된 CPU에 바인딩.
 *          CPU 친화성 마스크를 초기화하고 해당 CPU 비트를 설정한 뒤 적용한다. */
static int set_cpu_affinity(struct idle_prof_thread *ipt)
{
#if defined(FIO_HAVE_CPU_AFFINITY)
	if (fio_cpuset_init(&ipt->cpu_mask)) {
		log_err("fio: cpuset init failed\n");
		return -1;
	}

	fio_cpu_set(&ipt->cpu_mask, ipt->cpu);

	if (fio_setaffinity(gettid(), ipt->cpu_mask)) {
		log_err("fio: fio_setaffinity failed\n");
		fio_cpuset_exit(&ipt->cpu_mask);
		return -1;
	}

	return 0;
#else
	log_err("fio: fio_setaffinity not supported\n");
	return -1;
#endif
}

/* [한국어] idle_prof_thread_fn - 유휴 프로파일링 스레드의 메인 함수.
 *          동작 흐름:
 *          1) init_lock 대기 -> CPU 친화성 설정 -> 보정(calibration) 수행
 *          2) SCHED_IDLE 우선순위 설정 -> 초기화 완료 신호
 *          3) start_lock 대기 -> 프로파일링 루프 진입 (PROF_STOP까지 반복)
 *          4) 완료된 루프 수(ipt->loops)를 기록하여 유휴율 계산에 사용 */
static void *idle_prof_thread_fn(void *data)
{
	int retval;
	unsigned long j, k;
	struct idle_prof_thread *ipt = data;

	/* wait for all threads are spawned */
	pthread_mutex_lock(&ipt->init_lock);

	/* exit if any other thread failed to start */
	if (ipc.status == IDLE_PROF_STATUS_ABORT) {
		pthread_mutex_unlock(&ipt->init_lock);
		return NULL;
	}

	retval = set_cpu_affinity(ipt);
	if (retval == -1) {
		ipt->state = TD_EXITED;
		pthread_mutex_unlock(&ipt->init_lock);
		return NULL;
        }

	/* 보정 수행: 단위 작업의 기준 시간 측정 */
	ipt->cali_time = calibrate_unit(ipt->data);

	/* delay to set IDLE class till now for better calibration accuracy */
#if defined(CONFIG_SCHED_IDLE)
	if ((retval = fio_set_sched_idle()))
		log_err("fio: fio_set_sched_idle failed\n");
#else
	retval = -1;
	log_err("fio: fio_set_sched_idle not supported\n");
#endif
	if (retval == -1) {
		ipt->state = TD_EXITED;
		pthread_mutex_unlock(&ipt->init_lock);
		goto do_exit;
	}

	ipt->state = TD_INITIALIZED;

	/* signal the main thread that calibration is done */
	pthread_cond_signal(&ipt->cond);
	pthread_mutex_unlock(&ipt->init_lock);

	/* wait for other calibration to finish */
	pthread_mutex_lock(&ipt->start_lock);

	/* exit if other threads failed to initialize */
	if (ipc.status == IDLE_PROF_STATUS_ABORT) {
		pthread_mutex_unlock(&ipt->start_lock);
		goto do_exit;
	}

	/* exit if we are doing calibration only */
	if (ipc.status == IDLE_PROF_STATUS_CALI_STOP) {
		pthread_mutex_unlock(&ipt->start_lock);
		goto do_exit;
	}

	/* 프로파일링 루프 시작: PROF_STOP 신호까지 단위 작업 반복 */
	fio_gettime(&ipt->tps, NULL);
	ipt->state = TD_RUNNING;

	j = 0;
	while (1) {
		for (k = 0; k < page_size; k++) {
			ipt->data[(k + j) % page_size] = k % 256;
			if (ipc.status == IDLE_PROF_STATUS_PROF_STOP) {
				fio_gettime(&ipt->tpe, NULL);
				goto idle_prof_done;
			}
		}
		j++;
	}

idle_prof_done:

	/* 완료된 루프 수 기록 (정수부 + 부분 루프) */
	ipt->loops = j + (double) k / page_size;
	ipt->state = TD_EXITED;
	pthread_mutex_unlock(&ipt->start_lock);

do_exit:
	free_cpu_affinity(ipt);
	return NULL;
}

/* calculate mean and standard deviation to complete an unit of work */
/* [한국어] calibration_stats - 모든 CPU의 보정 시간으로 평균과 표준편차를 계산.
 *          이 통계는 유휴율 계산의 기준이 된다. */
static void calibration_stats(void)
{
	int i;
	double sum = 0.0, var = 0.0;
	struct idle_prof_thread *ipt;

	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];
		sum += ipt->cali_time;
	}

	ipc.cali_mean = sum/ipc.nr_cpus;

	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];
		var += pow(ipt->cali_time-ipc.cali_mean, 2);
	}

	ipc.cali_stddev = sqrt(var/(ipc.nr_cpus-1));
}

/* [한국어] fio_idle_prof_init - 유휴 프로파일링 초기화.
 *          각 CPU별로 스레드를 생성하고, 보정(calibration)을 수행한다.
 *          동작 흐름:
 *          1) CPU 수 확인 및 메모리 할당 (스레드 구조체 + 데이터 버퍼)
 *          2) 각 CPU별 뮤텍스/조건변수 초기화 후 스레드 생성
 *          3) init_lock 해제 -> 보정 완료 대기 -> 통계 계산 */
void fio_idle_prof_init(void)
{
	int i, ret;
	struct timespec ts;
	pthread_attr_t tattr;
	pthread_condattr_t cattr;
	struct idle_prof_thread *ipt;

	ipc.nr_cpus = cpus_configured();
	ipc.status = IDLE_PROF_STATUS_OK;

	if (ipc.opt == IDLE_PROF_OPT_NONE)
		return;

	ret = pthread_condattr_init(&cattr);
	assert(ret == 0);
#ifdef CONFIG_PTHREAD_CONDATTR_SETCLOCK
	ret = pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	assert(ret == 0);
#endif

	if ((ret = pthread_attr_init(&tattr))) {
		log_err("fio: pthread_attr_init %s\n", strerror(ret));
		return;
	}
	if ((ret = pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM))) {
		log_err("fio: pthread_attr_setscope %s\n", strerror(ret));
		return;
	}

	/* 각 CPU별 프로파일링 스레드 구조체 할당 */
	ipc.ipts = malloc(ipc.nr_cpus * sizeof(struct idle_prof_thread));
	if (!ipc.ipts) {
		log_err("fio: malloc failed\n");
		return;
	}

	/* 모든 스레드가 공유할 데이터 버퍼 할당 (CPU별 page_size씩) */
	ipc.buf = malloc(ipc.nr_cpus * page_size);
	if (!ipc.buf) {
		log_err("fio: malloc failed\n");
		free(ipc.ipts);
		return;
	}

	/*
	 * profiling aborts on any single thread failure since the
	 * result won't be accurate if any cpu is not used.
	 */
	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];

		ipt->cpu = i;
		ipt->state = TD_NOT_CREATED;
		ipt->data = (unsigned char *)(ipc.buf + page_size * i);

		if ((ret = pthread_mutex_init(&ipt->init_lock, NULL))) {
			ipc.status = IDLE_PROF_STATUS_ABORT;
			log_err("fio: pthread_mutex_init %s\n", strerror(ret));
			break;
		}

		if ((ret = pthread_mutex_init(&ipt->start_lock, NULL))) {
			ipc.status = IDLE_PROF_STATUS_ABORT;
			log_err("fio: pthread_mutex_init %s\n", strerror(ret));
			break;
		}

		if ((ret = pthread_cond_init(&ipt->cond, &cattr))) {
			ipc.status = IDLE_PROF_STATUS_ABORT;
			log_err("fio: pthread_cond_init %s\n", strerror(ret));
			break;
		}

		/* make sure all threads are spawned before they start */
		pthread_mutex_lock(&ipt->init_lock);

		/* make sure all threads finish init before profiling starts */
		pthread_mutex_lock(&ipt->start_lock);

		if ((ret = pthread_create(&ipt->thread, &tattr, idle_prof_thread_fn, ipt))) {
			ipc.status = IDLE_PROF_STATUS_ABORT;
			log_err("fio: pthread_create %s\n", strerror(ret));
			break;
		} else
			ipt->state = TD_CREATED;

		if ((ret = pthread_detach(ipt->thread))) {
			/* log error and let the thread spin */
			log_err("fio: pthread_detach %s\n", strerror(ret));
		}
	}

	/*
	 * let good threads continue so that they can exit
	 * if errors on other threads occurred previously.
	 */
	/* [한국어] 모든 스레드의 init_lock을 해제하여 보정을 시작시킨다.
	 *          에러 발생 시에도 해제하여 스레드가 정상 종료할 수 있게 한다. */
	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];
		pthread_mutex_unlock(&ipt->init_lock);
	}

	if (ipc.status == IDLE_PROF_STATUS_ABORT)
		return;

	/* wait for calibration to finish */
	/* [한국어] 각 스레드의 보정 완료를 대기. 1초 타임아웃으로 조건변수 대기한다. */
	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];
		pthread_mutex_lock(&ipt->init_lock);
		while ((ipt->state != TD_EXITED) &&
		       (ipt->state!=TD_INITIALIZED)) {
#ifdef CONFIG_PTHREAD_CONDATTR_SETCLOCK
			clock_gettime(CLOCK_MONOTONIC, &ts);
#else
			clock_gettime(CLOCK_REALTIME, &ts);
#endif
			ts.tv_sec += 1;
			pthread_cond_timedwait(&ipt->cond, &ipt->init_lock, &ts);
		}
		pthread_mutex_unlock(&ipt->init_lock);

		/*
		 * any thread failed to initialize would abort other threads
		 * later after fio_idle_prof_start.
		 */
		if (ipt->state == TD_EXITED)
			ipc.status = IDLE_PROF_STATUS_ABORT;
	}

	/* 보정 성공 시 평균/표준편차 계산, 실패 시 0으로 초기화 */
	if (ipc.status != IDLE_PROF_STATUS_ABORT)
		calibration_stats();
	else
		ipc.cali_mean = ipc.cali_stddev = 0.0;

	if (ipc.opt == IDLE_PROF_OPT_CALI)
		ipc.status = IDLE_PROF_STATUS_CALI_STOP;
}

/* [한국어] fio_idle_prof_start - 프로파일링 시작.
 *          모든 스레드의 start_lock을 해제하여 프로파일링 루프에 진입시킨다. */
void fio_idle_prof_start(void)
{
	int i;
	struct idle_prof_thread *ipt;

	if (ipc.opt == IDLE_PROF_OPT_NONE)
		return;

	/* unlock regardless abort is set or not */
	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];
		pthread_mutex_unlock(&ipt->start_lock);
	}
}

/* [한국어] fio_idle_prof_stop - 프로파일링 중지 및 유휴율 계산.
 *          PROF_STOP 신호를 보내고 모든 스레드 종료를 대기한 뒤,
 *          각 CPU의 유휴율을 계산한다:
 *            idleness = (완료 루프 수 * 보정 기준 시간) / 실제 경과 시간 */
void fio_idle_prof_stop(void)
{
	int i;
	uint64_t runt;
	struct timespec ts;
	struct idle_prof_thread *ipt;

	if (ipc.opt == IDLE_PROF_OPT_NONE)
		return;

	if (ipc.opt == IDLE_PROF_OPT_CALI)
		return;

	ipc.status = IDLE_PROF_STATUS_PROF_STOP;

	/* wait for all threads to exit from profiling */
	for (i = 0; i < ipc.nr_cpus; i++) {
		ipt = &ipc.ipts[i];
		pthread_mutex_lock(&ipt->start_lock);
		while ((ipt->state != TD_EXITED) &&
		       (ipt->state!=TD_NOT_CREATED)) {
			fio_gettime(&ts, NULL);
			ts.tv_sec += 1;
			/* timed wait in case a signal is not received */
			pthread_cond_timedwait(&ipt->cond, &ipt->start_lock, &ts);
		}
		pthread_mutex_unlock(&ipt->start_lock);

		/* calculate idleness */
		/* [한국어] 유휴율 = (루프 수 * 단위 작업 시간) / 총 경과 시간
		 *          1.0이면 100% 유휴, 0.0이면 CPU가 완전히 사용 중이었음 */
		if (ipc.cali_mean != 0.0) {
			runt = utime_since(&ipt->tps, &ipt->tpe);
			if (runt)
				ipt->idleness = ipt->loops * ipc.cali_mean / runt;
			else
				ipt->idleness = 0.0;
		} else
			ipt->idleness = 0.0;
	}

	/*
	 * memory allocations are freed via explicit fio_idle_prof_cleanup
	 * after profiling stats are collected by apps.
	 */
}

/*
 * return system idle percentage when cpu is -1;
 * return one cpu idle percentage otherwise.
 */
/* [한국어] fio_idle_prof_cpu_stat - CPU별 또는 시스템 전체 유휴율 반환.
 *          cpu == -1이면 전체 CPU의 평균 유휴율, 그 외에는 해당 CPU의 유휴율.
 *          반환 값은 백분율 (0.0 ~ 100.0) */
static double fio_idle_prof_cpu_stat(int cpu)
{
	int i, nr_cpus = ipc.nr_cpus;
	struct idle_prof_thread *ipt;
	double p = 0.0;

	if (ipc.opt == IDLE_PROF_OPT_NONE)
		return 0.0;

	if ((cpu >= nr_cpus) || (cpu < -1)) {
		log_err("fio: idle profiling invalid cpu index\n");
		return 0.0;
	}

	if (cpu == -1) {
		for (i = 0; i < nr_cpus; i++) {
			ipt = &ipc.ipts[i];
			p += ipt->idleness;
		}
		p /= nr_cpus;
	} else {
		ipt = &ipc.ipts[cpu];
		p = ipt->idleness;
	}

	return p * 100.0;
}

/* [한국어] fio_idle_prof_cleanup - 프로파일링 리소스 해제.
 *          스레드 구조체 배열과 데이터 버퍼를 해제한다. */
void fio_idle_prof_cleanup(void)
{
	if (ipc.ipts) {
		free(ipc.ipts);
		ipc.ipts = NULL;
	}

	if (ipc.buf) {
		free(ipc.buf);
		ipc.buf = NULL;
	}
}

/* [한국어] fio_idle_prof_parse_opt - 유휴 프로파일링 옵션 문자열 파싱.
 *          "calibrate": 보정만 수행하고 결과 출력 후 종료
 *          "system":    시스템 전체 유휴율 측정
 *          "percpu":    CPU별 유휴율 측정 */
int fio_idle_prof_parse_opt(const char *args)
{
	ipc.opt = IDLE_PROF_OPT_NONE; /* default */

	if (!args) {
		log_err("fio: empty idle-prof option string\n");
		return -1;
	}

#if defined(FIO_HAVE_CPU_AFFINITY) && defined(CONFIG_SCHED_IDLE)
	if (strcmp("calibrate", args) == 0) {
		ipc.opt = IDLE_PROF_OPT_CALI;
		fio_idle_prof_init();
		fio_idle_prof_start();
		fio_idle_prof_stop();
		show_idle_prof_stats(FIO_OUTPUT_NORMAL, NULL, NULL);
		return 1;
	} else if (strcmp("system", args) == 0) {
		ipc.opt = IDLE_PROF_OPT_SYSTEM;
		return 0;
	} else if (strcmp("percpu", args) == 0) {
		ipc.opt = IDLE_PROF_OPT_PERCPU;
		return 0;
	} else {
		log_err("fio: incorrect idle-prof option: %s\n", args);
		return -1;
	}
#else
	log_err("fio: idle-prof not supported on this platform\n");
	return -1;
#endif
}

/* [한국어] show_idle_prof_stats - 유휴 프로파일링 결과를 출력.
 *          FIO_OUTPUT_NORMAL: 텍스트 형식으로 시스템/CPU별 유휴율 및 보정 통계 출력
 *          FIO_OUTPUT_JSON:  JSON 객체에 cpu_idleness 키로 결과 추가 */
void show_idle_prof_stats(int output, struct json_object *parent,
			  struct buf_output *out)
{
	int i, nr_cpus = ipc.nr_cpus;
	struct json_object *tmp;
	char s[MAX_CPU_STR_LEN];

	if (output == FIO_OUTPUT_NORMAL) {
		if (ipc.opt > IDLE_PROF_OPT_CALI)
			log_buf(out, "\nCPU idleness:\n");
		else if (ipc.opt == IDLE_PROF_OPT_CALI)
			log_buf(out, "CPU idleness:\n");

		if (ipc.opt >= IDLE_PROF_OPT_SYSTEM)
			log_buf(out, "  system: %3.2f%%\n", fio_idle_prof_cpu_stat(-1));

		if (ipc.opt == IDLE_PROF_OPT_PERCPU) {
			log_buf(out, "  percpu: %3.2f%%", fio_idle_prof_cpu_stat(0));
			for (i = 1; i < nr_cpus; i++)
				log_buf(out, ", %3.2f%%", fio_idle_prof_cpu_stat(i));
			log_buf(out, "\n");
		}

		if (ipc.opt >= IDLE_PROF_OPT_CALI) {
			log_buf(out, "  unit work: mean=%3.2fus,", ipc.cali_mean);
			log_buf(out, " stddev=%3.2f\n", ipc.cali_stddev);
		}

		return;
	}

	/* JSON 출력: cpu_idleness 객체에 시스템/CPU별 유휴율 추가 */
	if ((ipc.opt != IDLE_PROF_OPT_NONE) && (output & FIO_OUTPUT_JSON)) {
		if (!parent)
			return;

		tmp = json_create_object();
		if (!tmp)
			return;

		json_object_add_value_object(parent, "cpu_idleness", tmp);
		json_object_add_value_float(tmp, "system", fio_idle_prof_cpu_stat(-1));

		if (ipc.opt == IDLE_PROF_OPT_PERCPU) {
			for (i = 0; i < nr_cpus; i++) {
				snprintf(s, MAX_CPU_STR_LEN, "cpu-%d", i);
				json_object_add_value_float(tmp, s, fio_idle_prof_cpu_stat(i));
			}
		}

		json_object_add_value_float(tmp, "unit_mean", ipc.cali_mean);
		json_object_add_value_float(tmp, "unit_stddev", ipc.cali_stddev);
	}
}
