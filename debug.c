/*
 * [한국어 설명] 디버그 출력 모듈 (debug.c)
 *
 * === 파일의 역할 ===
 * fio의 디버그 로깅 기능을 구현한다. FIO_INC_DEBUG가 정의된 경우에만
 * 실제 디버그 출력이 활성화되며, dprint() 매크로에서 비트마스크 검사를
 * 통과한 경우에만 __dprint()가 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * debug.h의 dprint() 매크로가 fio 전체에서 사용되며, 이 파일이 구현을 제공.
 * --debug=io,verify 등 커맨드라인으로 카테고리별 활성화 가능.
 *
 * === 타 모듈과의 연결 ===
 * - debug.h: dprint() 매크로, 디버그 카테고리 정의
 * - log.c: log_prevalist()로 실제 출력 수행
 * - fio 전체: dprint()로 디버그 메시지 출력
 *
 * === 주요 함수/구조체 요약 ===
 * - __dprint(): 비트마스크 필터를 통과한 디버그 메시지 출력
 * - fio_debug: 전역 디버그 비트마스크 (활성화된 카테고리)
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
