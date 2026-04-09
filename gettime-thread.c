/*
 * [한국어] gettime-thread.c - gettimeofday 오프로드 전용 스레드
 *
 * 이 파일은 시간 조회(gettimeofday)를 별도 스레드에서 연속 실행하여
 * 핫 I/O 경로에서의 시스템 콜 오버헤드를 줄이는 기능을 구현한다.
 *
 * 동작 방식:
 *   1) fio_gtod_init()          - 공유 메모리에 fio_ts 구조체 할당
 *   2) fio_start_gtod_thread()  - 전용 스레드 생성 (detach 모드)
 *   3) gtod_thread_main()       - 무한 루프에서 gettimeofday 호출 + seqlock으로 갱신
 *   4) 다른 스레드들은 fio_ts->ts를 seqlock으로 읽어 시간을 얻음
 *
 * seqlock을 사용하여 읽기 측에서 잠금 없이 일관된 시간값을 얻을 수 있다.
 * --gtod-reduce 또는 --gtod-cpu 옵션으로 활성화된다.
 */
#include <sys/time.h>
#include <time.h>

#include "fio.h"
#include "lib/seqlock.h"
#include "smalloc.h"

/* [한국어] 전역 변수들 */
struct fio_ts *fio_ts;                    /* 공유 시간 구조체 (seqlock 보호) */
int fio_gtod_offload = 0;                /* 오프로드 활성화 플래그 */
static pthread_t gtod_thread;            /* gettimeofday 전용 스레드 */
static os_cpu_mask_t fio_gtod_cpumask;   /* 스레드 CPU 친화성 마스크 */

/* [한국어] 공유 시간 구조체를 공유 메모리에 할당하는 초기화 함수 */
void fio_gtod_init(void)
{
	if (fio_ts)
		return;

	fio_ts = smalloc(sizeof(*fio_ts));
}

/* [한국어] gettimeofday를 호출하고 seqlock으로 보호하며 시간을 갱신하는 함수
 * tv_usec를 1000배 하여 tv_nsec(나노초)로 저장한다. */
static void fio_gtod_update(void)
{
	struct timeval __tv;

	if (!fio_ts)
		return;

	gettimeofday(&__tv, NULL);

	write_seqlock_begin(&fio_ts->seqlock);
	fio_ts->ts.tv_sec = __tv.tv_sec;
	fio_ts->ts.tv_nsec = __tv.tv_usec * 1000;
	write_seqlock_end(&fio_ts->seqlock);
}

/* [한국어] gtod 스레드 시작 시 CPU 친화성 설정을 위한 데이터 구조체 (미사용) */
struct gtod_cpu_data {
	struct fio_sem *sem;
	unsigned int cpu;
};

/* [한국어] gettimeofday 전용 스레드의 메인 함수
 * CPU 친화성을 설정한 후, 작업이 남아있는 동안 무한 루프로 시간을 갱신한다.
 * nop 명령으로 CPU를 약간 릴렉스하지만, 정밀도 유지를 위해 sleep하지 않는다. */
static void *gtod_thread_main(void *data)
{
	struct fio_sem *sem = data;
	int ret;

	ret = fio_setaffinity(gettid(), fio_gtod_cpumask);

	fio_sem_up(sem);  /* 생성자에게 초기화 완료 신호 */

	if (ret == -1) {
		log_err("gtod: setaffinity failed\n");
		return NULL;
	}

	/*
	 * As long as we have jobs around, update the clock. It would be nice
	 * to have some way of NOT hammering that CPU with gettimeofday(),
	 * but I'm not sure what to use outside of a simple CPU nop to relax
	 * it - we don't want to lose precision.
	 */
	/* nr_segments > 0인 동안 (작업이 남아있는 동안) 시간을 계속 갱신 */
	while (nr_segments) {
		fio_gtod_update();
		nop;
	}

	return NULL;
}

/* [한국어] gettimeofday 오프로드 스레드를 생성하고 시작하는 함수
 * 세마포어로 스레드 초기화 완료를 동기화한 후 detach 모드로 실행한다. */
int fio_start_gtod_thread(void)
{
	struct fio_sem *sem;
	pthread_attr_t attr;
	int ret;

	sem = fio_sem_init(FIO_SEM_LOCKED);
	if (!sem)
		return 1;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 2 * PTHREAD_STACK_MIN);  /* 최소 스택의 2배 */
	ret = pthread_create(&gtod_thread, &attr, gtod_thread_main, sem);
	pthread_attr_destroy(&attr);
	if (ret) {
		log_err("Can't create gtod thread: %s\n", strerror(ret));
		goto err;
	}

	/* 스레드를 detach하여 자동 자원 회수 */
	ret = pthread_detach(gtod_thread);
	if (ret) {
		log_err("Can't detach gtod thread: %s\n", strerror(ret));
		goto err;
	}

	/* 스레드 초기화 완료를 대기 */
	dprint(FD_MUTEX, "wait on startup_sem\n");
	fio_sem_down(sem);
	dprint(FD_MUTEX, "done waiting on startup_sem\n");
err:
	fio_sem_remove(sem);
	return ret;
}

/* [한국어] gettimeofday 오프로드 스레드의 CPU 친화성을 설정하는 함수
 * --gtod-cpu 옵션으로 지정된 CPU에 스레드를 바인딩한다. */
void fio_gtod_set_cpu(unsigned int cpu)
{
#ifdef FIO_HAVE_CPU_AFFINITY
	fio_cpu_set(&fio_gtod_cpumask, cpu);
#endif
}
