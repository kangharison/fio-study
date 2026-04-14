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

/* [한국어] 포인터를 mask+1 바이트 경계로 올림 정렬하는 매크로 */
#define PTR_ALIGN(ptr, mask)   \
	(char *)((uintptr_t)((ptr) + (mask)) & ~(mask))

struct align_footer {
	unsigned int offset;
	/* [한국어] 정렬된 포인터와 원래 malloc 포인터 간의 바이트 차이.
	 * 해제 시 이 값을 빼서 원래 포인터를 복원한다 */
};

/*
 * [한국어] __fio_memalign - 지정된 정렬 경계에 맞춰 메모리를 할당
 *
 * @alignment: 정렬 경계 (2의 거듭제곱이어야 함)
 * @size: 요청 크기
 * @fn: 실제 메모리 할당 함수 (malloc 또는 smalloc)
 * @return: 정렬된 메모리 포인터 (실패 시 NULL)
 *
 * alignment + sizeof(footer) - 1만큼 여유를 두고 할당한 뒤,
 * 포인터를 정렬하고 끝에 footer를 배치하여 원래 포인터 오프셋을 저장한다.
 *
 * 호출 체인: fio_memalign() 등 → [__fio_memalign]
 */
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

/*
 * [한국어] __fio_memfree - __fio_memalign()으로 할당된 메모리를 해제
 *
 * @ptr: __fio_memalign()이 반환한 정렬된 포인터
 * @size: 할당 시 요청한 크기 (footer 위치 계산용)
 * @fn: 실제 메모리 해제 함수 (free 또는 sfree)
 *
 * ptr + size 위치의 footer에서 오프셋을 읽어 원래 malloc 포인터를 복원한 뒤 해제.
 */
void __fio_memfree(void *ptr, size_t size, free_fn fn)
{
	struct align_footer *f = ptr + size;

	fn(ptr - f->offset);
}
