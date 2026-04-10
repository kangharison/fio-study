/*
 * [한국어 설명] 정렬된 메모리 할당 래퍼 (memalign.c)
 *
 * === 파일의 역할 ===
 * 지정된 정렬 경계에 맞춰 메모리를 할당하고 해제하는 함수를 제공한다.
 * 내부적으로 할당 함수(malloc 등)를 호출한 뒤 포인터를 정렬하고, 원래 포인터와의
 * 오프셋을 footer에 저장하여 해제 시 올바른 주소를 복원한다.
 *
 * === fio에서의 사용 ===
 * Direct I/O(O_DIRECT) 사용 시 DMA 정렬이 필요한 I/O 버퍼를 할당할 때 사용된다.
 * 섹터 크기나 페이지 크기에 맞춘 정렬된 메모리가 필요한 곳에서 호출된다.
 */
#include <assert.h>
#include <stdlib.h>

#include "memalign.h"
#include "smalloc.h"

#define PTR_ALIGN(ptr, mask)   \
	(char *)((uintptr_t)((ptr) + (mask)) & ~(mask))

struct align_footer {
	unsigned int offset;
};

void *__fio_memalign(size_t alignment, size_t size, malloc_fn fn)
{
	struct align_footer *f;
	void *ptr, *ret = NULL;

	assert(!(alignment & (alignment - 1)));

	ptr = fn(size + alignment + sizeof(*f) - 1);
	if (ptr) {
		ret = PTR_ALIGN(ptr, alignment - 1);
		f = ret + size;
		f->offset = (uintptr_t) ret - (uintptr_t) ptr;
	}

	return ret;
}

void __fio_memfree(void *ptr, size_t size, free_fn fn)
{
	struct align_footer *f = ptr + size;

	fn(ptr - f->offset);
}
