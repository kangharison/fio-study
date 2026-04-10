/*
 * [한국어 설명] GCC 경고 억제 snprintf 래퍼 (nowarn_snprintf.h)
 *
 * === 파일의 역할 ===
 * GCC 8 이상에서 발생하는 -Wformat-truncation 경고를 억제하는 snprintf 래퍼 함수를 제공한다.
 * nowarn_snprintf()는 내부적으로 vsnprintf()를 호출하되, GCC pragma를 사용하여
 * 문자열 잘림(truncation) 관련 경고를 일시적으로 비활성화한다.
 *
 * === fio에서의 사용 ===
 * fio 내부에서 버퍼 크기가 제한된 문자열 포맷팅 시 불필요한 컴파일 경고를 방지하기 위해 사용된다.
 * 파일 경로, 작업 이름 등 의도적으로 잘릴 수 있는 문자열을 안전하게 포맷팅할 때 활용된다.
 */
#ifndef _NOWARN_SNPRINTF_H_
#define _NOWARN_SNPRINTF_H_

#include <stdio.h>
#include <stdarg.h>

static inline int nowarn_snprintf(char *str, size_t size, const char *format,
				  ...)
{
	va_list args;
	int res;

	va_start(args, format);
#if __GNUC__ -0 >= 8
#pragma GCC diagnostic push "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
	res = vsnprintf(str, size, format, args);
#if __GNUC__ -0 >= 8
#pragma GCC diagnostic pop "-Wformat-truncation"
#endif
	va_end(args);

	return res;
}

#endif
