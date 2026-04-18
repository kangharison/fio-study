/*
 * [한국어 설명] 재진입 가능 문자열 토큰 분리 함수 폴리필 구현 (strsep.c)
 *
 * === 파일의 역할 ===
 * BSD 유래 strsep(3) 함수의 폴리필을 제공한다. strsep 은 구분자 집합으로
 * 문자열을 토큰화하되, strtok(3) 과 달리 호출 간 상태를 정적 전역 변수에
 * 숨기지 않고 호출자가 넘기는 `char **stringp` 에 외재화하므로 재진입/
 * 스레드 안전하며, 빈 토큰("a::b" → "a", "", "b") 도 분리해 준다는 점이
 * 핵심 차이다. configure 스크립트가 CONFIG_STRSEP 미정의 시 이 구현을
 * 빌드에 포함시킨다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 oslib/ 이식성 계층. 호출처는 옵션 값(예: "--rwmixread=70,30")을
 * 쉼표 기준으로 토큰화하는 경로, linux-blkzoned.c 의 read_file 에서 개행
 * 제거에 사용하는 경로, iolog.c 의 blktrace 라인 파싱 등이다. 구현은
 * O(n·|delim|) 의 단순 스캔으로 충분하다(입력 크기가 작다).
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 옵션 파서, linux-blkzoned.c::read_file 개행 제거,
 *   engines/* 의 파라미터 문자열 분해 등.
 * - 의존: 없음(순수 C). 파괴적 파싱이므로 입력 문자열이 수정 가능해야 함
 *   — 리터럴 상수 문자열에 대해 호출하면 segfault(주의).
 * - 공유 상태: 외재화된 *stringp 로만 상태 유지 — 재진입/스레드 안전.
 *
 * === 주요 함수 요약 ===
 * - strsep(): *stringp 에서 delim 의 첫 일치를 NUL 로 치환해 토큰을 종결하고
 *   *stringp 를 다음 토큰 시작으로 갱신, 마지막 토큰 후엔 NULL 로 셋.
 */
#ifndef CONFIG_STRSEP  /* [한국어] libc 네이티브 strsep 가 있으면 본 구현 제외. */

#include <stddef.h>  /* [한국어] NULL 매크로. */
#include "strsep.h"  /* [한국어] 공개 선언 — 시그니처 일치 보장. */

/*
 * [한국어]
 * strsep - 구분자 집합 기반 파괴적 토큰 분리 (재진입 가능)
 *
 * @stringp: 파싱 상태 포인터의 포인터. 호출마다 *stringp 가 다음 토큰의
 *           시작으로 전진되고, 마지막 토큰을 소비하면 NULL 로 설정된다.
 *           *stringp == NULL 이면 더 이상 토큰이 없다는 의미.
 * @delim: 구분자 문자 집합 NUL-종단 문자열. 이 문자 중 어느 것이든 구분자로
 *         사용된다. 예: ",\n" 이면 쉼표 또는 개행 어느 쪽이든 토큰을 끊는다.
 * @return: 현재 토큰의 시작 포인터(호출 시점의 *stringp). 더 없으면 NULL.
 *
 * strtok 과의 차이:
 *  - strtok 은 정적 전역에 상태를 숨겨 재진입 불가 + 연속 구분자를 하나로
 *    뭉쳐 처리(빈 토큰 없음).
 *  - strsep 은 외재화된 상태 + 빈 토큰 ""도 정식 반환 — CSV 파싱 친화적.
 *  - 둘 다 입력을 파괴적으로 수정(NUL 삽입)하므로 리터럴에는 사용 금지.
 *
 * 동작 단계:
 * 1) s = *stringp; NULL 이면 종료(NULL 반환).
 * 2) tok = s (현 토큰 시작 기억).
 * 3) s 를 한 문자씩 스캔하며 c = *s++ 로 읽어 delim 의 각 문자와 비교.
 * 4) 일치 구분자 발견:
 *    - c == 0 (문자열 끝): *stringp = NULL 로 설정 — 더 이상 토큰 없음.
 *    - 그 외: s[-1] = 0 으로 구분자를 NUL 로 치환해 토큰 종결, *stringp =
 *      다음 토큰 시작 위치.
 * 5) tok 반환.
 *
 * 호출 체인:
 *   fio 파서/linux-blkzoned.c::read_file → [strsep()]
 */
char *strsep(char **stringp, const char *delim)
{
	char *s, *tok;  /* [한국어] s: 현재 스캔 포인터, tok: 현재 토큰 시작. */
	const char *spanp;  /* [한국어] delim 문자열 스캔용 포인터. */
	int c, sc;  /* [한국어] c: 현재 haystack 문자, sc: 현재 delim 문자. int 로 승격해 EOF/0 비교 안전. */

	s = *stringp;  /* [한국어] 외재화 상태 로드. */
	if (!s)  /* [한국어] NULL = 소진. 종료 신호. */
		return NULL;

	/* [한국어] tok은 현재 토큰의 시작 위치를 기억 */
	tok = s;
	do {
		c = *s++;  /* [한국어] 한 문자 읽고 s 를 한 칸 전진. c == 0 이면 문자열 끝. */
		/* [한국어] 현재 문자를 모든 구분자와 비교 */
		spanp = delim;
		do {
			sc = *spanp++;  /* [한국어] 다음 구분자 문자. sc == 0 이면 delim 소진(불일치). */
			if (sc == c) {  /* [한국어] 일치 — 또는 c == 0 이면 NUL=='\0' 매치로 문자열 끝. */
				/* [한국어] 문자열 끝(NUL)이면 *stringp를 NULL로 설정 */
				if (c == 0)
					s = NULL;  /* [한국어] 다음 호출에서 NULL 반환하도록 외재화 상태 소진. */
				else
					/* [한국어] 구분자를 NUL로 대체하여 토큰 종료 */
					s[-1] = 0;  /* [한국어] 방금 읽은 구분자 위치(s[-1])를 NUL 로 치환 — 파괴적. */
				*stringp = s;  /* [한국어] 외재화 — 다음 호출 재개 위치. */
				return tok;  /* [한국어] 현 토큰 반환. */
			}
		} while (sc != 0);  /* [한국어] delim 전체 검사. 0 도달 = 현 c 는 구분자 아님. */
	} while (1);  /* [한국어] 외부 루프는 return 으로만 탈출(위). */
}

#endif  /* [한국어] !CONFIG_STRSEP 블록 종료. */
