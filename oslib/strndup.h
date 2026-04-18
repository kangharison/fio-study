/*
 * [한국어 설명] strndup 폴리필 헤더 (strndup.h)
 *
 * === 파일의 역할 ===
 * POSIX.1-2008 strndup(3) 이 시스템에 없을 때를 위한 폴백 선언부이다.
 * strndup 은 strdup 의 길이 제한 변종으로, 원본에서 최대 n 바이트만 복제하여
 * 힙 메모리에 새 NUL 종료 문자열을 반환한다. glibc 2.10+ 는 제공하지만
 * 오래된 Solaris, 일부 임베디드 libc, 옛 Windows MSVCRT 는 없을 수 있어
 * fio configure 가 CONFIG_HAVE_STRNDUP 으로 판별한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어. 토큰 추출(strsep 후 조각 복제), 옵션 파싱 중 부분 문자열
 * 복제, 파일명/호스트명 잘라 저장 등 문자열의 부분집합을 독립 힙 객체로 만드는
 * 상황에서 사용.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strndup.c: malloc + memcpy + NUL 추가 폴백 구현.
 * - configure: CONFIG_HAVE_STRNDUP 정의 여부 결정.
 * - <string.h>: CONFIG_HAVE_STRNDUP 경로에서 표준 선언 대신 사용됨.
 * - <stddef.h>: size_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - strndup(s, n): 최대 n 바이트까지 복제된 동적 할당 문자열 반환(호출자가 free).
 */
#ifndef CONFIG_HAVE_STRNDUP
/* [한국어] 시스템 미제공 시에만 선언 활성화. */

#ifndef FIO_STRNDUP_LIB_H
/* [한국어] 헤더 가드. */
#define FIO_STRNDUP_LIB_H

#include <stddef.h>
/* [한국어] <stddef.h> 포함 이유: size_t 타입 공급. */

/*
 * [한국어]
 * strndup - 원본 문자열의 최대 n 바이트를 복제하여 동적 할당된 사본 반환
 *
 * @s: 복제할 NUL 종료 원본 문자열. NULL 시 반환은 구현별(통상 NULL 또는 충돌).
 * @n: 최대 복제 바이트 수(NUL 제외). strlen(s) < n 이면 strlen(s) 만큼만 복제.
 * @return: malloc 으로 할당한 NUL 종료 사본 포인터. 호출자가 free() 로 해제.
 *          malloc 실패 시 NULL(errno=ENOMEM).
 *
 * 폴백 구현: min(strnlen(s,n), n) 계산 → malloc(len+1) → memcpy → 마지막 NUL.
 *
 * 호출 체인: 토큰 파싱(options.c, engines 의 URI 분할)·경로 정리 등 다양.
 * 실행 컨텍스트: 호출자 따름. 에러: 할당 실패 시 NULL + errno=ENOMEM.
 */
char *strndup(const char *s, size_t n);

#endif

#endif
