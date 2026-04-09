/*
 * [한국어] fio_sem.c - fio 세마포어 구현 (공유 메모리 기반, 프로세스 간 동기화)
 *
 * pthread mutex와 condition variable을 이용한 세마포어를 구현한다.
 * mmap(MAP_SHARED)으로 공유 메모리를 할당하여 프로세스 간 동기화가 가능하다.
 *
 * 주요 API:
 *   fio_sem_init()          - 공유 메모리에 세마포어 생성
 *   fio_sem_remove()        - 세마포어 파괴 및 메모리 해제
 *   fio_sem_down()          - P 연산 (잠금 획득, 값이 0이면 대기)
 *   fio_sem_up()            - V 연산 (잠금 해제, 대기자 깨움)
 *   fio_sem_down_trylock()  - 비차단 잠금 시도
 *   fio_sem_down_timeout()  - 타임아웃 있는 잠금 획득
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#ifdef CONFIG_VALGRIND_DEV
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif

#include "fio_sem.h"
#include "pshared.h"
#include "os/os.h"
#include "fio_time.h"
#include "gettime.h"

/* [한국어] 세마포어 내부 자원(mutex, cond) 파괴. 메모리 자체는 해제하지 않음 */
void __fio_sem_remove(struct fio_sem *sem)
{
	assert(sem->magic == FIO_SEM_MAGIC);
	pthread_mutex_destroy(&sem->lock);
	pthread_cond_destroy(&sem->cond);

	/*
	 * When not running on Valgrind, ensure any subsequent attempt to grab
	 * this semaphore will fail with an assert, instead of just silently
	 * hanging. When running on Valgrind, let Valgrind detect
	 * use-after-free.
         */
	/* [한국어] Valgrind 미실행 시 메모리를 0으로 초기화하여 이후 접근 시 assert 실패 유도 */
	if (!RUNNING_ON_VALGRIND)
		memset(sem, 0, sizeof(*sem));
}

/* [한국어] mmap으로 할당된 세마포어를 파괴하고 메모리를 해제(munmap) */
void fio_sem_remove(struct fio_sem *sem)
{
	__fio_sem_remove(sem);
	munmap((void *) sem, sizeof(*sem));
}

/* [한국어] 기존 메모리 영역에 세마포어를 초기화 (내부 헬퍼 함수) */
int __fio_sem_init(struct fio_sem *sem, int value)
{
	int ret;

	sem->value = value;		/* [한국어] 초기 카운터 값 설정 */
	/* Initialize .waiters explicitly for Valgrind. */
	sem->waiters = 0;		/* [한국어] 대기자 수 초기화 */
	sem->magic = FIO_SEM_MAGIC;	/* [한국어] 매직 넘버 설정 (유효성 검증용) */

	/* [한국어] 프로세스 간 공유 가능한 mutex + cond 초기화 */
	ret = mutex_cond_init_pshared(&sem->lock, &sem->cond);
	if (ret)
		return ret;

	return 0;
}

/* [한국어] mmap(MAP_SHARED)으로 공유 메모리를 할당하고 세마포어를 초기화하여 반환 */
struct fio_sem *fio_sem_init(int value)
{
	struct fio_sem *sem = NULL;

	/* [한국어] 익명 공유 메모리 매핑 - fork된 자식 프로세스와 공유 가능 */
	sem = (void *) mmap(NULL, sizeof(struct fio_sem),
				PROT_READ | PROT_WRITE,
				OS_MAP_ANON | MAP_SHARED, -1, 0);
	if (sem == MAP_FAILED) {
		perror("mmap semaphore");
		return NULL;
	}

	if (!__fio_sem_init(sem, value))
		return sem;

	/* [한국어] 초기화 실패 시 정리 후 NULL 반환 */
	fio_sem_remove(sem);
	return NULL;
}

/* [한국어] 타임아웃 시간이 실제로 경과했는지 검증하는 헬퍼 함수 */
static bool sem_timed_out(struct timespec *t, unsigned int msecs)
{
	struct timeval tv;
	struct timespec now;

	gettimeofday(&tv, NULL);
	now.tv_sec = tv.tv_sec;
	now.tv_nsec = tv.tv_usec * 1000;

	return mtime_since(t, &now) >= msecs;
}

/* [한국어] 밀리초 단위 타임아웃이 있는 세마포어 획득 (P 연산) */
int fio_sem_down_timeout(struct fio_sem *sem, unsigned int msecs)
{
	struct timespec base;	/* [한국어] 시작 시각 (타임아웃 이중 확인용) */
	struct timespec t;	/* [한국어] 절대 타임아웃 시각 */
	int ret = 0;

	assert(sem->magic == FIO_SEM_MAGIC);

	/* [한국어] 플랫폼에 따라 MONOTONIC 또는 REALTIME 클럭 사용 */
#ifdef CONFIG_PTHREAD_CONDATTR_SETCLOCK
	clock_gettime(CLOCK_MONOTONIC, &t);
#else
	clock_gettime(CLOCK_REALTIME, &t);
#endif

	base = t;

	/* [한국어] 밀리초를 초/나노초로 변환하여 절대 타임아웃 시각 계산 */
	t.tv_sec += msecs / 1000;
	t.tv_nsec += ((msecs * 1000000ULL) % 1000000000);
	if (t.tv_nsec >= 1000000000) {
		t.tv_nsec -= 1000000000;
		t.tv_sec++;
	}

	pthread_mutex_lock(&sem->lock);

	sem->waiters++;
	while (!sem->value && !ret) {
		/*
		 * Some platforms (FreeBSD 9?) seems to return timed out
		 * way too early, double check.
		 */
		/* [한국어] 일부 플랫폼에서 조기 타임아웃 반환 문제 대응: 실제 경과 시간 이중 확인 */
		ret = pthread_cond_timedwait(&sem->cond, &sem->lock, &t);
		if (ret == ETIMEDOUT && !sem_timed_out(&base, msecs))
			ret = 0;
	}
	sem->waiters--;

	if (!ret) {
		sem->value--;	/* [한국어] 세마포어 획득 성공: 카운터 감소 */
		pthread_mutex_unlock(&sem->lock);
		return 0;
	}

	pthread_mutex_unlock(&sem->lock);
	return ret;		/* [한국어] 타임아웃(ETIMEDOUT) 반환 */
}

/* [한국어] 비차단 세마포어 획득 시도. 성공 시 false, 실패(이미 잠김) 시 true 반환 */
bool fio_sem_down_trylock(struct fio_sem *sem)
{
	bool ret = true;

	assert(sem->magic == FIO_SEM_MAGIC);

	pthread_mutex_lock(&sem->lock);
	if (sem->value) {
		sem->value--;	/* [한국어] 값이 양수면 획득 성공 */
		ret = false;
	}
	pthread_mutex_unlock(&sem->lock);

	return ret;
}

/* [한국어] 세마포어 획득 (P 연산). 값이 0이면 양수가 될 때까지 대기 */
void fio_sem_down(struct fio_sem *sem)
{
	assert(sem->magic == FIO_SEM_MAGIC);

	pthread_mutex_lock(&sem->lock);

	while (!sem->value) {
		sem->waiters++;
		pthread_cond_wait(&sem->cond, &sem->lock);
		sem->waiters--;
	}

	sem->value--;		/* [한국어] 세마포어 카운터 감소 (잠금 획득) */
	pthread_mutex_unlock(&sem->lock);
}

/* [한국어] 세마포어 해제 (V 연산). 카운터 증가 후 대기자가 있으면 깨움 */
void fio_sem_up(struct fio_sem *sem)
{
	int do_wake = 0;

	assert(sem->magic == FIO_SEM_MAGIC);

	pthread_mutex_lock(&sem->lock);
	if (!sem->value && sem->waiters)
		do_wake = 1;	/* [한국어] 값이 0이고 대기자가 있을 때만 시그널 전송 */
	sem->value++;

	if (do_wake)
		pthread_cond_signal(&sem->cond);

	pthread_mutex_unlock(&sem->lock);
}
