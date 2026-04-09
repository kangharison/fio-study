/*
 * [한국어] fio_shared_sem.c - 공유 메모리(smalloc) 기반 세마포어 구현
 *
 * fio_sem.c의 세마포어와 동일하지만, mmap 대신 smalloc(fio 공유 메모리 할당기)을
 * 사용하여 메모리를 할당한다. 이렇게 하면 부모 프로세스가 자식 프로세스에서
 * 할당한 세마포어를 해제할 수 있다.
 *
 * 이 파일이 fio_sem.c에서 분리된 이유:
 *   fio_sem.c의 함수들은 smalloc 자체에서 사용되므로,
 *   smalloc에 의존하는 함수를 같은 파일에 두면 순환 의존성이 발생한다.
 */

/*
 * Separate out the two helper functions for fio_sem from "fio_sem.c".
 * These two functions depend on fio shared memory. Other fio_sem
 * functions in "fio_sem.c" are used for fio shared memory. This file
 * separation is required to avoid build failures caused by circular
 * dependency.
 */

#include <stdio.h>

#include "fio_sem.h"
#include "smalloc.h"

/*
 * Allocate and initialize fio_sem lock object in the same manner as
 * fio_sem_init(), except the lock object is allocated from the fio
 * shared memory. This allows the parent process to free the lock
 * allocated by child processes.
 */
/* [한국어] smalloc으로 공유 메모리에 세마포어를 할당하고 초기화 */
struct fio_sem *fio_shared_sem_init(int value)
{
	struct fio_sem *sem;

	/* [한국어] fio 내부 공유 메모리 할당기(smalloc)에서 메모리 확보 */
	sem = smalloc(sizeof(struct fio_sem));
	if (!sem)
		return NULL;

	if (!__fio_sem_init(sem, value))
		return sem;

	/* [한국어] 초기화 실패 시 정리 */
	fio_shared_sem_remove(sem);
	return NULL;
}

/*
 * Free the fio_sem lock object allocated by fio_shared_sem_init().
 */
/* [한국어] 공유 메모리 세마포어를 파괴하고 sfree로 메모리 반환 */
void fio_shared_sem_remove(struct fio_sem *sem)
{
	__fio_sem_remove(sem);
	sfree(sem);	/* [한국어] smalloc으로 할당된 메모리를 fio 공유 메모리로 반환 */
}
