/*
 * [한국어] filelock.h - 파일 잠금 시스템 헤더
 *
 * 파일명 기반의 배타적 잠금(exclusive lock)을 제공한다.
 * 여러 fio job이 동일한 파일에 동시에 접근할 때 동기화를 보장한다.
 */
#ifndef FIO_LOCK_FILE_H
#define FIO_LOCK_FILE_H

#include "lib/types.h"

/* [한국어] 파일명으로 배타적 잠금 획득 (잠금될 때까지 대기) */
extern void fio_lock_file(const char *);
/* [한국어] 파일명으로 잠금 시도 (비차단, 실패 시 true 반환) */
extern bool fio_trylock_file(const char *);
/* [한국어] 파일명으로 잠금 해제 */
extern void fio_unlock_file(const char *);

/* [한국어] 파일 잠금 시스템 초기화 (smalloc으로 공유 메모리 할당) */
extern int fio_filelock_init(void);
/* [한국어] 파일 잠금 시스템 종료 및 자원 해제 */
extern void fio_filelock_exit(void);

#endif
