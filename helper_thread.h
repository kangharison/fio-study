/*
 * [한국어] helper_thread.h - fio 헬퍼 스레드 헤더
 *
 * 주기적 통계 수집, 디스크 유틸리티 업데이트, Steady State 체크 등을
 * 수행하는 헬퍼 스레드의 API 선언.
 */
#ifndef FIO_HELPER_THREAD_H
#define FIO_HELPER_THREAD_H

#include <stdbool.h>

struct fio_sem;  /* 세마포어 (전방 선언) */
struct sk_out;   /* 소켓 출력 컨텍스트 (전방 선언) */

extern void helper_reset(void);          /* 타이머 리셋 요청 */
extern void helper_do_stat(void);        /* 즉시 통계 출력 요청 (async-signal-safe) */
extern bool helper_should_exit(void);    /* 헬퍼 스레드 종료 여부 확인 */
extern void helper_thread_destroy(void); /* 헬퍼 데이터 파괴 (파이프, 메모리 해제) */
extern void helper_thread_exit(void);    /* 헬퍼 스레드 종료 및 join */
extern int helper_thread_create(struct fio_sem *, struct sk_out *); /* 헬퍼 스레드 생성 */

#endif
