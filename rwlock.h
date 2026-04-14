#ifndef FIO_RWLOCK_H
#define FIO_RWLOCK_H
/*
 * [한국어] rwlock.h - 프로세스 간 공유 가능한 읽기-쓰기 잠금 헤더
 *
 * pthread_rwlock 기반의 읽기-쓰기 잠금 구조체와 인터페이스를 정의한다.
 * 매직 넘버(FIO_RWLOCK_MAGIC)로 유효성을 검증한다.
 
 * === 파일의 역할 ===
 * pthread_rwlock 기반 읽기-쓰기 잠금 구조체와 API를 정의.
 *
 * === 전체 아키텍처에서의 위치 ===
 * rwlock.c와 짝을 이루는 헤더.
 *
 * === 타 모듈과의 연결 ===
 * - rwlock.c: 이 헤더의 함수 구현
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_rwlock: rwlock + magic 필드
 * - fio_rwlock_init/read/write/unlock(): API
 */

#include <pthread.h>

#define FIO_RWLOCK_MAGIC	0x52574c4fU  /* "RWLO" - 유효성 검증용 매직 넘버 */

/* [한국어] 읽기-쓰기 잠금 구조체 */
struct fio_rwlock {
	pthread_rwlock_t lock;  /* pthread 읽기-쓰기 잠금 */
	int magic;              /* 유효성 검증용 매직 넘버 */
};

extern void fio_rwlock_read(struct fio_rwlock *);     /* 읽기 잠금 획득 */
extern void fio_rwlock_write(struct fio_rwlock *);    /* 쓰기 잠금 획득 */
extern void fio_rwlock_unlock(struct fio_rwlock *);   /* 잠금 해제 */
extern struct fio_rwlock *fio_rwlock_init(void);      /* rwlock 생성 */
extern void fio_rwlock_remove(struct fio_rwlock *);   /* rwlock 해제 */

#endif
