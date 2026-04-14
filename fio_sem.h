/*
 * [한국어] fio_sem.h - fio 세마포어 헤더 파일
 *
 * fio 내부에서 사용하는 세마포어 구조체와 API를 선언한다.
 * 공유 메모리(mmap) 기반으로 프로세스 간 동기화를 지원하며,
 * pthread mutex + condition variable로 구현되어 있다.
 
 * === 파일의 역할 ===
 * fio 내부에서 사용하는 세마포어 구조체와 API를 선언한다.
 * 공유 메모리(mmap) 기반으로 프로세스 간 동기화를 지원.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_sem.c와 짝을 이루는 헤더. fio 전체에서 동기화에 사용.
 *
 * === 타 모듈과의 연결 ===
 * - fio_sem.c: 이 헤더의 함수 구현
 * - smalloc.c, backend.c, server.c: 세마포어 사용
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_sem: 세마포어 구조체 (mutex + cond + value + magic)
 * - fio_sem_init/remove/down/up: 세마포어 라이프사이클 API
 */
#ifndef FIO_SEM_H
#define FIO_SEM_H

#include <pthread.h>
#include "lib/types.h"

/* [한국어] 세마포어 유효성 검증용 매직 넘버 (use-after-free 방지) */
#define FIO_SEM_MAGIC		0x4d555445U

/* [한국어] fio 세마포어 구조체 - 프로세스 간 공유 가능한 동기화 객체 */
struct fio_sem {
	pthread_mutex_t lock;	/* [한국어] 상호 배제용 뮤텍스 */
	pthread_cond_t cond;	/* [한국어] 대기/통지용 조건 변수 */
	int value;		/* [한국어] 세마포어 카운터 값 (0이면 잠금 상태) */
	int waiters;		/* [한국어] 현재 대기 중인 스레드/프로세스 수 */
	int magic;		/* [한국어] 유효성 검증용 매직 넘버 */
};

/* [한국어] 세마포어 초기 상태 열거형 */
enum {
	FIO_SEM_LOCKED	= 0,	/* [한국어] 잠금 상태로 초기화 (value=0) */
	FIO_SEM_UNLOCKED	= 1,	/* [한국어] 해제 상태로 초기화 (value=1) */
};

/* [한국어] 기존 메모리에 세마포어 초기화 (내부용) */
extern int __fio_sem_init(struct fio_sem *, int);
/* [한국어] mmap으로 공유 메모리를 할당하고 세마포어 초기화 */
extern struct fio_sem *fio_sem_init(int);
/* [한국어] smalloc(공유 메모리 할당기)으로 세마포어 할당 및 초기화 */
extern struct fio_sem *fio_shared_sem_init(int);
/* [한국어] 세마포어 내부 자원 해제 (메모리는 해제하지 않음) */
extern void __fio_sem_remove(struct fio_sem *);
/* [한국어] 세마포어 자원 해제 + mmap 메모리 해제 */
extern void fio_sem_remove(struct fio_sem *);
/* [한국어] 공유 메모리 세마포어 자원 해제 + sfree */
extern void fio_shared_sem_remove(struct fio_sem *);
/* [한국어] 세마포어 값 증가 (V 연산, 잠금 해제/시그널) */
extern void fio_sem_up(struct fio_sem *);
/* [한국어] 세마포어 값 감소 (P 연산, 잠금 획득, 값이 0이면 대기) */
extern void fio_sem_down(struct fio_sem *);
/* [한국어] 세마포어 비차단 획득 시도 (성공 시 false, 실패 시 true 반환) */
extern bool fio_sem_down_trylock(struct fio_sem *);
/* [한국어] 타임아웃 있는 세마포어 획득 (밀리초 단위, 타임아웃 시 ETIMEDOUT) */
extern int fio_sem_down_timeout(struct fio_sem *, unsigned int);

#endif
