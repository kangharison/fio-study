/*
 * [한국어 설명] 포맷 문자열 동적 할당 함수 폴리필 구현 (asprintf.c)
 *
 * === 파일의 역할 ===
 * asprintf()와 vasprintf() 함수의 폴리필(polyfill) 구현을 제공한다.
 * 이 함수들은 printf 스타일의 포맷 문자열을 자동으로 크기가 조절된 힙 메모리에
 * 기록한다. GNU C 라이브러리에서는 기본 제공되지만, 일부 플랫폼(예: Windows)에서는
 * 없을 수 있어 폴리필이 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 레이어의 이식성 함수로, fio 전역에서 문자열 동적 생성에 사용된다.
 * configure 스크립트가 CONFIG_HAVE_ASPRINTF/CONFIG_HAVE_VASPRINTF를 정의하면
 * 이 구현은 컴파일에서 제외된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 전반 (경로 생성, 로그 메시지, 옵션 문자열 등)
 * - 의존: vsnprintf() (C 표준 라이브러리)
 *
 * === 주요 함수 요약 ===
 * - vasprintf(): va_list를 받아 동적 할당된 포맷 문자열 생성
 * - asprintf(): 가변 인자를 받아 vasprintf()를 호출하는 래퍼
 */
#include <stdio.h>
#include <stdlib.h>
#include "oslib/asprintf.h"

/*
 * [한국어]
 * vasprintf - va_list 기반 포맷 문자열을 동적 할당 버퍼에 출력
 *
 * @strp: 결과 문자열 포인터가 저장될 위치 (호출자가 free() 해야 함)
 * @fmt: printf 형식의 포맷 문자열
 * @ap: 가변 인자 리스트
 * @return: 성공 시 기록된 문자 수, 실패 시 -1
 *
 * 2단계 접근법: 1) vsnprintf(NULL,0)로 필요한 버퍼 크기를 계산한 후,
 * 2) malloc으로 할당하고 실제 포맷 문자열을 기록한다.
 * va_copy를 사용하여 va_list를 복제하는 이유는 vsnprintf()가 ap를 소비하기 때문이다.
 *
 * 호출 체인:
 *   asprintf() → [vasprintf()] → vsnprintf(), malloc()
 */
#ifndef CONFIG_HAVE_VASPRINTF
int vasprintf(char **strp, const char *fmt, va_list ap)
{
	va_list ap_copy;
	char *str;
	int len;

	/* [한국어] va_list를 복제 - vsnprintf()가 ap를 소비하므로 원본을 보존 */
#ifdef va_copy
	va_copy(ap_copy, ap);
#else
	/* [한국어] va_copy가 없는 구형 컴파일러를 위한 대체 매크로 */
	__va_copy(ap_copy, ap);
#endif
	/* [한국어] 1단계: 버퍼 없이 호출하여 필요한 문자 수만 계산 */
	len = vsnprintf(NULL, 0, fmt, ap_copy);
	va_end(ap_copy);

	if (len < 0)
		return len;

	/* [한국어] null 종료 문자를 위해 1바이트 추가 후 메모리 할당 */
	len++;
	str = malloc(len);
	*strp = str;
	/* [한국어] 2단계: 실제로 포맷 문자열을 할당된 버퍼에 기록 */
	return str ? vsnprintf(str, len, fmt, ap) : -1;
}
#endif

/*
 * [한국어]
 * asprintf - 가변 인자 기반 포맷 문자열 동적 할당
 *
 * @strp: 결과 문자열 포인터가 저장될 위치
 * @fmt: printf 형식의 포맷 문자열
 * @...: 가변 인자
 * @return: 성공 시 기록된 문자 수, 실패 시 -1
 *
 * vasprintf()의 편의 래퍼로, 가변 인자를 va_list로 변환하여 전달한다.
 *
 * 호출 체인:
 *   fio 전반 → [asprintf()] → vasprintf()
 */
#ifndef CONFIG_HAVE_ASPRINTF
int asprintf(char **strp, const char *fmt, ...)
{
	va_list arg;
	int done;

	va_start(arg, fmt);
	done = vasprintf(strp, fmt, arg);
	va_end(arg);

	return done;
}
#endif
