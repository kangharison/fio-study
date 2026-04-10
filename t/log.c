/*
 * [한국어 설명] 테스트용 로그 출력 유틸리티 (log.c)
 *
 * === 파일의 역할 ===
 * 테스트 프로그램에서 사용하는 간이 로그 출력 함수(log_err, log_info)를 제공한다.
 * log_err는 stderr로, log_info는 stdout로 포맷된 메시지를 출력하며,
 * fio 본체의 로그 함수를 테스트 환경에서 대체하는 역할을 한다.
 */
#include <stdio.h>
#include <stdarg.h>
#include "../minmax.h"

size_t log_err(const char *format, ...)
{
	char buffer[1024];
	va_list args;
	size_t len;

	va_start(args, format);
	len = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	len = min(len, sizeof(buffer) - 1);

	return fwrite(buffer, len, 1, stderr);
}

size_t log_info(const char *format, ...)
{
	char buffer[1024];
	va_list args;
	size_t len;

	va_start(args, format);
	len = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	len = min(len, sizeof(buffer) - 1);

	return fwrite(buffer, len, 1, stdout);
}
