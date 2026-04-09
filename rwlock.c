/*
 * [한국어] rwlock.c - 프로세스 간 공유 가능한 읽기-쓰기 잠금(rwlock)
 *
 * 이 파일은 pthread_rwlock 기반의 읽기-쓰기 잠금을 구현한다.
 * mmap(MAP_SHARED)으로 할당되어 프로세스 간에 공유할 수 있다.
 * CONFIG_PSHARED가 정의된 경우 PTHREAD_PROCESS_SHARED 속성을 설정한다.
 *
 * 주요 함수:
 *   fio_rwlock_init()   - rwlock 생성 (공유 메모리 할당 + 초기화)
 *   fio_rwlock_remove() - rwlock 해제 (파괴 + munmap)
 *   fio_rwlock_read()   - 읽기 잠금 획득
 *   fio_rwlock_write()  - 쓰기 잠금 획득
 *   fio_rwlock_unlock() - 잠금 해제
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>

#include "log.h"
#include "rwlock.h"
#include "os/os.h"

/* [한국어] 쓰기 잠금 획득 - 배타적 접근 (다른 읽기/쓰기 차단) */
void fio_rwlock_write(struct fio_rwlock *lock)
{
	assert(lock->magic == FIO_RWLOCK_MAGIC);
	pthread_rwlock_wrlock(&lock->lock);
}

/* [한국어] 읽기 잠금 획득 - 공유 접근 (다른 읽기 허용, 쓰기 차단) */
void fio_rwlock_read(struct fio_rwlock *lock)
{
	assert(lock->magic == FIO_RWLOCK_MAGIC);
	pthread_rwlock_rdlock(&lock->lock);
}

/* [한국어] 잠금 해제 (읽기/쓰기 공통) */
void fio_rwlock_unlock(struct fio_rwlock *lock)
{
	assert(lock->magic == FIO_RWLOCK_MAGIC);
	pthread_rwlock_unlock(&lock->lock);
}

/* [한국어] rwlock 해제 - pthread_rwlock 파괴 후 공유 메모리 해제 */
void fio_rwlock_remove(struct fio_rwlock *lock)
{
	assert(lock->magic == FIO_RWLOCK_MAGIC);
	pthread_rwlock_destroy(&lock->lock);
	munmap((void *) lock, sizeof(*lock));
}

/* [한국어] rwlock 생성 및 초기화 함수
 * MAP_SHARED로 메모리를 할당하여 프로세스 간 공유가 가능하게 한다.
 * CONFIG_PSHARED가 정의되면 PTHREAD_PROCESS_SHARED 속성을 설정한다. */
struct fio_rwlock *fio_rwlock_init(void)
{
	struct fio_rwlock *lock;
	pthread_rwlockattr_t attr;
	int ret;

	/* 프로세스 간 공유를 위해 mmap(MAP_SHARED)으로 할당 */
	lock = (void *) mmap(NULL, sizeof(struct fio_rwlock),
				PROT_READ | PROT_WRITE,
				OS_MAP_ANON | MAP_SHARED, -1, 0);
	if (lock == MAP_FAILED) {
		perror("mmap rwlock");
		lock = NULL;
		goto err;
	}

	lock->magic = FIO_RWLOCK_MAGIC;  /* 매직 넘버로 유효성 검증 */

	ret = pthread_rwlockattr_init(&attr);
	if (ret) {
		log_err("pthread_rwlockattr_init: %s\n", strerror(ret));
		goto err;
	}
#ifdef CONFIG_PSHARED
	/* 프로세스 간 공유 속성 설정 */
	ret = pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	if (ret) {
		log_err("pthread_rwlockattr_setpshared: %s\n", strerror(ret));
		goto destroy_attr;
	}

	ret = pthread_rwlock_init(&lock->lock, &attr);
#else
	ret = pthread_rwlock_init(&lock->lock, NULL);
#endif

	if (ret) {
		log_err("pthread_rwlock_init: %s\n", strerror(ret));
		goto destroy_attr;
	}

	pthread_rwlockattr_destroy(&attr);

	return lock;
destroy_attr:
	pthread_rwlockattr_destroy(&attr);
err:
	if (lock)
		fio_rwlock_remove(lock);
	return NULL;
}
