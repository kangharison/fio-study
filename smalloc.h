/*
 * [한국어 설명] 공유 메모리 할당기 API (smalloc.h)
 *
 * === 파일의 역할 ===
 * mmap 기반 프로세스 간 공유 메모리 할당/해제 함수를 선언한다.
 * fio에서 작업(job) 간 공유해야 하는 데이터(thread_data, 통계, 옵션 등)에 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * smalloc.c와 짝을 이루는 헤더. fio 전체에서 공유 메모리가 필요한 곳에서 참조.
 *
 * === 타 모듈과의 연결 ===
 * - smalloc.c: 이 헤더의 함수 구현
 * - init.c, iolog.c, backend.c: smalloc/sfree로 공유 데이터 할당/해제
 * - libfio.c: sinit()/scleanup()으로 할당기 초기화/정리
 *
 * === 주요 함수/구조체 요약 ===
 * - smalloc()/scalloc(): 공유 메모리 할당 (malloc/calloc 대응)
 * - sfree(): 공유 메모리 해제 (free 대응)
 * - sinit()/scleanup(): 할당기 초기화/정리
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
