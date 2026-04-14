/*
 * [한국어 설명] 대소문자 무시 부분 문자열 검색 함수 폴리필 구현 (strcasestr.c)
 *
 * === 파일의 역할 ===
 * strcasestr() 함수의 폴리필 구현을 제공한다. 대소문자를 구분하지 않고
 * s1(haystack)에서 s2(needle)를 검색하여 첫 번째 일치 위치의 포인터를 반환한다.
 * GNU 확장 함수로, 비(非)GNU 시스템에서는 이 폴리필이 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio에서 옵션 이름 비교 등 대소문자 무시 검색에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 옵션 파싱, I/O 엔진 이름 매칭 등
 * - 의존: tolower() (C 표준 라이브러리)
 *
 * === 주요 함수 요약 ===
 * - strcasestr(): 대소문자 무시하여 부분 문자열을 검색
 */
#ifndef CONFIG_STRCASESTR

#include <ctype.h>
#include <stddef.h>
#include "strcasestr.h"

/*
 * [한국어]
 * strcasestr - 대소문자 무시 부분 문자열 검색
 *
 * @s1: 검색 대상 문자열 (haystack)
 * @s2: 검색할 패턴 문자열 (needle)
 * @return: s1에서 s2가 처음 나타나는 위치 포인터, 없으면 NULL
 *
 * s1을 한 문자씩 이동하며 s2와 대소문자 무시 비교를 수행한다.
 * s2가 빈 문자열이면 s1의 시작을 반환한다.
 * tolower()를 사용하여 각 문자를 소문자로 변환 후 비교한다.
 *
 * 호출 체인:
 *   fio 전반 → [strcasestr()] → tolower()
 */
char *strcasestr(const char *s1, const char *s2)
{
	const char *s = s1;
	const char *p = s2;

	do {
		if (!*p)
			return (char *) s1;
		if ((*p == *s) ||
		    (tolower(*p) == tolower(*s))) {
			++p;
			++s;
		} else {
			p = s2;
			if (!*s)
				return NULL;
			s = ++s1;
		}
	} while (1);

	return *p ? NULL : (char *) s1;
}

#endif
