/*
 * [한국어 설명] 길이 제한 문자열 복제 단위 테스트 (strndup.c)
 *
 * === 파일의 역할 ===
 * oslib/strndup 모듈의 최대 길이를 지정하여 문자열을 복제하는 기능을 검증하는 단위 테스트이다.
 * 지정된 길이까지만 문자열을 복사하고 널 종료를 올바르게 처리하는지 테스트한다.
 * 시스템에 strndup이 없는 환경에서 fio의 자체 구현이 정상 동작하는지 확인한다.
 */
#include "../unittest.h"

#ifndef CONFIG_HAVE_STRNDUP
#include "../../oslib/strndup.h"
#else
#include <string.h>
#endif

static void test_strndup_1(void)
{
	char s[] = "test";
	char *p = strndup(s, 3);

	if (p) {
		CU_ASSERT_EQUAL(strcmp(p, "tes"), 0);
		CU_ASSERT_EQUAL(strlen(p), 3);
	}
}

static void test_strndup_2(void)
{
	char s[] = "test";
	char *p = strndup(s, 4);

	if (p) {
		CU_ASSERT_EQUAL(strcmp(p, s), 0);
		CU_ASSERT_EQUAL(strlen(p), 4);
	}
}

static void test_strndup_3(void)
{
	char s[] = "test";
	char *p = strndup(s, 5);

	if (p) {
		CU_ASSERT_EQUAL(strcmp(p, s), 0);
		CU_ASSERT_EQUAL(strlen(p), 4);
	}
}

static struct fio_unittest_entry tests[] = {
	{
		.name	= "strndup/1",
		.fn	= test_strndup_1,
	},
	{
		.name	= "strndup/2",
		.fn	= test_strndup_2,
	},
	{
		.name	= "strndup/3",
		.fn	= test_strndup_3,
	},
	{
		.name	= NULL,
	},
};

CU_ErrorCode fio_unittest_oslib_strndup(void)
{
	return fio_unittest_add_suite("oslib/strndup.c", NULL, NULL, tests);
}
