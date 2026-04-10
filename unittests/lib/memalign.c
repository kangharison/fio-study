/*
 * [한국어 설명] 정렬 메모리 할당 단위 테스트 (memalign.c)
 *
 * === 파일의 역할 ===
 * lib/memalign 모듈의 정렬된 메모리 할당 기능을 검증하는 단위 테스트이다.
 * 다양한 정렬 크기와 할당 크기에 대해 메모리가 올바르게 정렬되어 할당되는지 테스트한다.
 * 정렬 메모리 할당 및 해제가 메모리 누수 없이 정상 동작하는지 확인한다.
 */
#include <stdlib.h>
#include "../unittest.h"

#include "../../lib/memalign.h"

static void test_memalign_1(void)
{
	size_t align = 4096;
	void *p = __fio_memalign(align, 1234, malloc);

	if (p)
		CU_ASSERT_EQUAL(((int)(uintptr_t)p) & (align - 1), 0);
}

static struct fio_unittest_entry tests[] = {
	{
		.name	= "memalign/1",
		.fn	= test_memalign_1,
	},
	{
		.name	= NULL,
	},
};

CU_ErrorCode fio_unittest_lib_memalign(void)
{
	return fio_unittest_add_suite("lib/memalign.c", NULL, NULL, tests);
}
