/*
 * [한국어 설명] 문자열 토큰화 함수 폴리필 구현 (strsep.c)
 *
 * === 파일의 역할 ===
 * strsep() 함수의 폴리필 구현을 제공한다. 문자열을 구분자 문자로 분리하여
 * 각 토큰을 순차적으로 반환한다. strtok()과 달리 전역 상태를 사용하지 않으므로
 * 재진입 가능(thread-safe)하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio에서 옵션 값 파싱, 경로 분리 등에 사용된다.
 * CONFIG_STRSEP이 정의되면 시스템 제공 함수를 사용하고 이 파일은 제외된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 옵션 파싱, linux-blkzoned.c의 read_file() 등
 * - 의존: 없음 (순수 C 구현)
 *
 * === 주요 함수 요약 ===
 * - strsep(): 구분자 기반 문자열 토큰 분리
 */
#ifndef CONFIG_STRSEP

#include <stddef.h>
#include "strsep.h"

/*
 * [한국어]
 * strsep - 구분자 기반 문자열 토큰 분리
 *
 * @stringp: 파싱 중인 문자열 포인터의 포인터 (각 호출마다 업데이트됨)
 * @delim: 구분자 문자열 (이 중 아무 문자나 구분자로 사용됨)
 * @return: 현재 토큰의 시작 포인터, *stringp가 NULL이면 NULL 반환
 *
 * 동작 과정:
 * 1) *stringp에서 delim에 포함된 문자를 찾을 때까지 스캔
 * 2) 구분자를 NUL로 대체하여 토큰을 종료
 * 3) *stringp를 다음 토큰 시작 위치로 업데이트
 * 4) 문자열 끝에 도달하면 *stringp를 NULL로 설정
 *
 * 호출 체인:
 *   fio 파싱 코드 → [strsep()]
 */
char *strsep(char **stringp, const char *delim)
{
	char *s, *tok;
	const char *spanp;
	int c, sc;

	s = *stringp;
	if (!s)
		return NULL;

	/* [한국어] tok은 현재 토큰의 시작 위치를 기억 */
	tok = s;
	do {
		c = *s++;
		/* [한국어] 현재 문자를 모든 구분자와 비교 */
		spanp = delim;
		do {
			sc = *spanp++;
			if (sc == c) {
				/* [한국어] 문자열 끝(NUL)이면 *stringp를 NULL로 설정 */
				if (c == 0)
					s = NULL;
				else
					/* [한국어] 구분자를 NUL로 대체하여 토큰 종료 */
					s[-1] = 0;
				*stringp = s;
				return tok;
			}
		} while (sc != 0);
	} while (1);
}

#endif
