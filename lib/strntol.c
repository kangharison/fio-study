/*
 * [한국어 설명] 길이 제한 문자열-정수 변환 (strntol.c)
 *
 * === 파일의 역할 ===
 * 길이가 제한된 문자열(null-terminated가 아닐 수 있음)을 long 정수로 변환하는
 * 함수를 제공한다. 내부적으로 임시 버퍼에 복사하여 null-terminate한 뒤
 * strtol()을 호출하는 안전한 방식으로 구현되어 있다.
 *
 * === fio에서의 사용 ===
 * fio의 설정 파싱이나 문자열 처리에서 길이가 고정된 버퍼의 문자열을
 * 숫자로 변환할 때 안전하게 사용된다.
 */
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "strntol.h"

long strntol(const char *str, size_t sz, char **end, int base)
{
	/* Expect that digit representation of LONG_MAX/MIN
	 * not greater than this buffer */
	char buf[24];
	long ret;
	const char *beg = str;

	/* Catch up leading spaces */
	for (; beg && sz && *beg == ' '; beg++, sz--)
		;

	if (!sz || sz >= sizeof(buf)) {
		if (end)
			*end = (char *)str;
		return 0;
	}

	memcpy(buf, beg, sz);
	buf[sz] = '\0';
	ret = strtol(buf, end, base);
	if (ret == LONG_MIN || ret == LONG_MAX)
		return ret;
	if (end)
		*end = (char *)beg + (*end - buf);
	return ret;
}
