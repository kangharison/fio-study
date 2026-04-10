/*
 * [한국어 설명] 단위 테스트 프레임워크 헤더 (unittest.h)
 *
 * === 파일의 역할 ===
 * CU(Check Unit) 기반 단위 테스트 프레임워크의 공통 헤더 파일이다.
 * 테스트 스위트 등록과 실행에 필요한 타입 정의와 매크로를 제공한다.
 * 모든 단위 테스트 파일에서 공통으로 포함하여 일관된 테스트 인터페이스를 보장한다.
 */
#ifndef FIO_UNITTEST_H
#define FIO_UNITTEST_H

#include <sys/types.h>

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

struct fio_unittest_entry {
	const char *name;
	CU_TestFunc fn;
};

CU_ErrorCode fio_unittest_add_suite(const char*, CU_InitializeFunc,
	CU_CleanupFunc, struct fio_unittest_entry*);

CU_ErrorCode fio_unittest_lib_memalign(void);
CU_ErrorCode fio_unittest_lib_num2str(void);
CU_ErrorCode fio_unittest_lib_strntol(void);
CU_ErrorCode fio_unittest_lib_pcbuf(void);
CU_ErrorCode fio_unittest_oslib_strlcat(void);
CU_ErrorCode fio_unittest_oslib_strndup(void);
CU_ErrorCode fio_unittest_oslib_strcasestr(void);
CU_ErrorCode fio_unittest_oslib_strsep(void);

#endif
