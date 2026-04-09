/*
 * [한국어] pshared.h - 프로세스 간 공유 뮤텍스/조건변수 초기화 헤더
 *
 * PTHREAD_PROCESS_SHARED 속성을 가진 mutex와 condition variable을
 * 초기화하는 유틸리티 함수들을 선언한다.
 * fio에서 fork된 자식 프로세스 간 동기화에 필수적으로 사용된다.
 */
#ifndef FIO_PSHARED_H
#define FIO_PSHARED_H

#include <pthread.h>

/* [한국어] 지정된 타입(NORMAL/ERRORCHECK/RECURSIVE 등)으로 프로세스 공유 뮤텍스 초기화 */
extern int mutex_init_pshared_with_type(pthread_mutex_t *, int);
/* [한국어] 기본 타입(PTHREAD_MUTEX_DEFAULT)으로 프로세스 공유 뮤텍스 초기화 */
extern int mutex_init_pshared(pthread_mutex_t *);
/* [한국어] 프로세스 공유 조건변수 초기화 (MONOTONIC 클럭 지원 시 적용) */
extern int cond_init_pshared(pthread_cond_t *);
/* [한국어] 프로세스 공유 뮤텍스 + 조건변수를 한 번에 초기화 */
extern int mutex_cond_init_pshared(pthread_mutex_t *, pthread_cond_t *);

#endif
