/*
 * [한국어 설명] 단위 테스트 프레임워크 실행기 (unittest.c)
 *
 * === 파일의 역할 ===
 * CU(Check Unit) 테스트 스위트를 탐색하고 실행하는 단위 테스트 프레임워크의 메인 러너이다.
 * 등록된 모든 테스트 스위트를 수집하여 순차적으로 실행하고, 테스트 결과를 보고한다.
 * fio 프로젝트의 라이브러리 함수들에 대한 자동화된 테스트 실행을 담당한다.
 */
/*
 * fio unittest
 * Copyright (C) 2018 Tomohiro Kusumi <kusumi.tomohiro@osnexus.com>
 */

#include <stdio.h>
#include <stdlib.h>

#include "./unittest.h"

CU_ErrorCode fio_unittest_add_suite(const char *name, CU_InitializeFunc initfn,
	CU_CleanupFunc cleanfn, struct fio_unittest_entry *tvec)
{
	CU_pSuite pSuite;
	struct fio_unittest_entry *t;

	pSuite = CU_add_suite(name, initfn, cleanfn);
	if (!pSuite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	t = tvec;
	while (t && t->name) {
		if (!CU_add_test(pSuite, t->name, t->fn)) {
			CU_cleanup_registry();
			return CU_get_error();
		}
		t++;
	}

	return CUE_SUCCESS;
}

static void fio_unittest_register(CU_ErrorCode (*fn)(void))
{
	if (fn && fn() != CUE_SUCCESS) {
		fprintf(stderr, "%s\n", CU_get_error_msg());
		exit(1);
	}
}

int main(void)
{
	if (CU_initialize_registry() != CUE_SUCCESS) {
		fprintf(stderr, "%s\n", CU_get_error_msg());
		exit(1);
	}

	fio_unittest_register(fio_unittest_lib_memalign);
	fio_unittest_register(fio_unittest_lib_num2str);
	fio_unittest_register(fio_unittest_lib_strntol);
	fio_unittest_register(fio_unittest_lib_pcbuf);
	fio_unittest_register(fio_unittest_oslib_strlcat);
	fio_unittest_register(fio_unittest_oslib_strndup);
	fio_unittest_register(fio_unittest_oslib_strcasestr);
	fio_unittest_register(fio_unittest_oslib_strsep);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	CU_cleanup_registry();

	return CU_get_error();
}
