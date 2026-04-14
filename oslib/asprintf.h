/*
 * [한국어 설명] 포맷 문자열 동적 할당 함수 폴리필 헤더 (asprintf.h)
 *
 * === 파일의 역할 ===
 * asprintf()와 vasprintf() 함수가 시스템에 없는 경우를 위한 선언부이다.
 * 이 함수들은 printf 형식의 포맷 문자열을 동적으로 할당된 버퍼에 출력한다.
 * CONFIG_HAVE_ASPRINTF/CONFIG_HAVE_VASPRINTF 매크로로 시스템 제공 여부를 판단한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 레이어는 fio 전체에서 사용되는 이식성(portability) 함수를 제공한다.
 * asprintf()는 옵션 파싱, 경로 생성, 로그 메시지 등 문자열 동적 생성이 필요한
 * 모든 곳에서 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - fio 코어 전반: 문자열 동적 생성이 필요한 곳에서 사용
 * - oslib/asprintf.c: 실제 구현부
 * - configure 빌드 시스템: CONFIG_HAVE_ASPRINTF 매크로 정의 여부 결정
 *
 * === 주요 함수 요약 ===
 * - vasprintf(): va_list 기반 포맷 문자열 동적 할당
 * - asprintf(): 가변 인자 기반 포맷 문자열 동적 할당
 */
#ifndef FIO_ASPRINTF_H
#define FIO_ASPRINTF_H

#include <stdarg.h>

/* [한국어] 시스템에 vasprintf()가 없는 경우에만 선언 */
#ifndef CONFIG_HAVE_VASPRINTF
int vasprintf(char **strp, const char *fmt, va_list ap);
#endif
/* [한국어] 시스템에 asprintf()가 없는 경우에만 선언 */
#ifndef CONFIG_HAVE_ASPRINTF
int asprintf(char **strp, const char *fmt, ...);
#endif

#endif /* FIO_ASPRINTF_H */
