/*
 * [한국어 설명] 정렬된 메모리 할당 헤더 (memalign.h)
 *
 * === 파일의 역할 ===
 * __fio_memalign()과 __fio_memfree() 함수의 선언 및 malloc_fn/free_fn 함수
 * 포인터 타입을 정의한다. 사용자 정의 할당 함수를 전달할 수 있는 구조이다.
 *
 * === fio에서의 사용 ===
 * fio의 I/O 버퍼 할당 코드에서 정렬된 메모리 할당이 필요할 때 이 헤더를
 * 포함하여 API를 사용한다.
 */
#ifndef FIO_MEMALIGN_H
#define FIO_MEMALIGN_H

#include <inttypes.h>
#include <stdbool.h>

typedef void* (*malloc_fn)(size_t);
typedef void (*free_fn)(void*);

extern void *__fio_memalign(size_t alignment, size_t size, malloc_fn fn);
extern void __fio_memfree(void *ptr, size_t size, free_fn fn);

#endif
