/*
 * [한국어 설명] 길이 제한 문자열-정수 변환 단위 테스트 (strntol.c)
 *
 * === 파일의 역할 ===
 * lib/strntol 모듈의 길이 제한이 있는 문자열을 long 정수로 변환하는 기능을 검증하는 단위 테스트이다.
 * 지정된 길이까지만 파싱하여 정수로 변환하는 동작이 올바른지 다양한 케이스로 테스트한다.
 * 버퍼 오버런 방지를 위한 안전한 문자열 파싱이 정상 작동하는지 확인한다.
 */
#include "../unittest.h"

#include "../../lib/strntol.h"

static void test_strntol_1(void)
{
	char s[] = "12345";
	char *endp = NULL;
	long ret = strntol(s, strlen(s), &endp, 10);

	CU_ASSERT_EQUAL(ret, 12345);
	CU_ASSERT_NOT_EQUAL(endp, NULL);
	CU_ASSERT_EQUAL(*endp, '\0');
}

static void test_strntol_2(void)
{
	char s[] = "     12345";
	char *endp = NULL;
	long ret = strntol(s, strlen(s), &endp, 10);

	CU_ASSERT_EQUAL(ret, 12345);
	CU_ASSERT_NOT_EQUAL(endp, NULL);
	CU_ASSERT_EQUAL(*endp, '\0');
}

static void test_strntol_3(void)
{
	char s[] = "0x12345";
	char *endp = NULL;
	long ret = strntol(s, strlen(s), &endp, 16);

	CU_ASSERT_EQUAL(ret, 0x12345);
	CU_ASSERT_NOT_EQUAL(endp, NULL);
	CU_ASSERT_EQUAL(*endp, '\0');
}

static struct fio_unittest_entry tests[] = {
	{
		.name	= "strntol/1",
		.fn	= test_strntol_1,
	},
	{
		.name	= "strntol/2",
		.fn	= test_strntol_2,
	},
	{
		.name	= "strntol/3",
		.fn	= test_strntol_3,
	},
	{
		.name	= NULL,
	},
};

CU_ErrorCode fio_unittest_lib_strntol(void)
{
	return fio_unittest_add_suite("lib/strntol.c", NULL, NULL, tests);
}
