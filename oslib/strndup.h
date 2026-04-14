/*
 * [한국어 설명] 길이 제한 문자열 복제 함수 폴리필 헤더 (strndup.h)
 *
 * === 파일의 역할 ===
 * strndup() 함수가 시스템에 없는 경우를 위한 선언부이다.
 * strndup()은 원본 문자열에서 최대 n바이트까지만 복제하여 새 힙 메모리에 저장한다.
 * POSIX.1-2008에서 표준화되었지만 일부 구형 시스템에서는 없을 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, 문자열 부분 복제가 필요한 곳에서 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strndup.c: 실제 구현부
 * - configure: CONFIG_HAVE_STRNDUP 매크로 정의 여부 결정
 *
 * === 주요 함수 요약 ===
 * - strndup(): 최대 n바이트까지 문자열을 복제하여 동적 할당된 사본 반환
 */
#ifndef CONFIG_HAVE_STRNDUP

#ifndef FIO_STRNDUP_LIB_H
#define FIO_STRNDUP_LIB_H

#include <stddef.h>

char *strndup(const char *s, size_t n);

#endif

#endif
