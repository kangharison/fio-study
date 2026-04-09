/*
 * [한국어] debug.c - 디버그 출력 모듈
 *
 * fio의 디버그 로깅 기능을 구현한다.
 * FIO_INC_DEBUG가 정의된 경우에만 실제 디버그 출력이 활성화되며,
 * dprint() 매크로에서 비트마스크 검사를 통과한 경우에만 __dprint()가 호출된다.
 */

#include <assert.h>    /* 단언 매크로 */
#include <stdarg.h>    /* 가변 인자 처리 (va_list, va_start, va_end) */

#include "debug.h"     /* 디버그 레벨 정의 및 dprint 매크로 */
#include "log.h"       /* 로그 출력 함수 (log_prevalist 등) */

#ifdef FIO_INC_DEBUG
/* [한국어] 실제 디버그 메시지 출력 함수
 * - dprint() 매크로에서 비트마스크 필터를 통과한 경우에만 호출됨
 * - type: 디버그 카테고리 (FD_PROCESS, FD_IO 등)
 * - str: printf 형식의 포맷 문자열
 * - 가변 인자를 log_prevalist()에 전달하여 실제 출력 수행 */
void __dprint(int type, const char *str, ...)
{
	va_list args;

	assert(type < FD_DEBUG_MAX);  /* 유효한 디버그 타입인지 확인 */

	va_start(args, str);
	log_prevalist(type, str, args);  /* 로그 서브시스템에 출력 위임 */
	va_end(args);
}
#endif
