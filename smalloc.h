/*
 * [한국어] smalloc.h - 공유 메모리 할당기 API
 *
 * mmap 기반 프로세스 간 공유 메모리 할당/해제 함수 선언.
 * fio에서 작업(job) 간 공유해야 하는 데이터(통계, 옵션 등)에 사용된다.
 */
#ifndef FIO_SMALLOC_H
#define FIO_SMALLOC_H

#include <stddef.h>  /* size_t 정의 */

/* [한국어] 공유 메모리 할당/해제 API */
extern void *smalloc(size_t);          /* 공유 메모리 할당 (malloc 대응) */
extern void *scalloc(size_t, size_t);  /* 공유 메모리 배열 할당 (calloc 대응) */
extern void sfree(void *);            /* 공유 메모리 해제 (free 대응) */
extern char *smalloc_strdup(const char *);  /* 공유 메모리 문자열 복사 (strdup 대응) */

/* [한국어] 할당기 초기화/정리 */
extern void sinit(void);              /* 공유 메모리 풀 초기화 */
extern void scleanup(void);           /* 공유 메모리 풀 전체 정리 */
extern void smalloc_debug(size_t);    /* 디버그 정보 출력 (OOM 시 진단용) */

extern unsigned int smalloc_pool_size; /* 풀 크기 (--alloc-size 옵션으로 설정) */

#endif
