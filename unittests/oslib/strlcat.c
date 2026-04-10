/*
 * [한국어 설명] 안전한 문자열 연결 단위 테스트 (strlcat.c)
 *
 * === 파일의 역할 ===
 * oslib/strlcat 모듈의 버퍼 크기를 고려한 안전한 문자열 연결 기능을 검증하는 단위 테스트이다.
 * 대상 버퍼의 크기를 초과하지 않도록 문자열을 연결하는 동작이 올바른지 테스트한다.
 * 버퍼 오버플로를 방지하는 안전한 문자열 처리가 다양한 경계 조건에서 정상 작동하는지 확인한다.
 */
#include "../unittest.h"

#ifndef CONFIG_STRLCAT
#include "../../oslib/strlcat.h"
#else
#include <string.h>
#endif

static void test_strlcat_1(void)
{
	char dst[32];
	char src[] = "test";
	size_t ret;

	dst[0] = '\0';
	ret = strlcat(dst, src, sizeof(dst));

	CU_ASSERT_EQUAL(strcmp(dst, "test"), 0);
	CU_ASSERT_EQUAL(ret, 4); /* total length it tried to create */
}

static void test_strlcat_2(void)
{
	char dst[32];
	char src[] = "test";
	size_t ret;

	dst[0] = '\0';
	ret = strlcat(dst, src, strlen(dst));

	CU_ASSERT_EQUAL(strcmp(dst, ""), 0);
	CU_ASSERT_EQUAL(ret, 4); /* total length it tried to create */
}

static struct fio_unittest_entry tests[] = {
	{
		.name	= "strlcat/1",
		.fn	= test_strlcat_1,
	},
	{
		.name	= "strlcat/2",
		.fn	= test_strlcat_2,
	},
	{
		.name	= NULL,
	},
};

CU_ErrorCode fio_unittest_oslib_strlcat(void)
{
	return fio_unittest_add_suite("oslib/strlcat.c", NULL, NULL, tests);
}
