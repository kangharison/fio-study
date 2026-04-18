/*
 * [한국어 설명] 대소문자 무시 부분 문자열 검색 함수 폴리필 구현 (strcasestr.c)
 *
 * === 파일의 역할 ===
 * GNU 확장 함수 strcasestr(3) 의 폴리필을 제공한다. strcasestr 은
 * strstr(3) 의 대소문자 무시 변종으로, 문자열 s1(haystack)에서 s2(needle)가
 * 처음 나타나는 위치를 반환한다. glibc 이외의 libc(BSD·Windows MinGW 구버전
 * 등)에서는 이 심볼이 없을 수 있어 configure 가 CONFIG_STRCASESTR 미정의
 * 시 본 구현을 빌드에 포함시킨다. 알고리즘은 가장 단순한 brute-force
 * O(n·m) 검색 — 각 haystack 위치에서 needle 을 모두 일치시키려 하고
 * 불일치면 한 칸 이동한다. Boyer-Moore 같은 최적화는 하지 않으며, fio 의
 * 호출 빈도가 적고 입력 크기가 작아(옵션 이름·엔진 이름 매칭) 문제가 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 oslib/ 이식성 계층. 사용처는 대소문자 무관 문자열 매칭이
 * 필요한 곳 — 옵션 값 비교(예: "blockSize" ≒ "blocksize"), 엔진 이름의
 * 유연한 매칭, 로그/에러 메시지 검사 등이다. CONFIG_STRCASESTR 가
 * 정의되면 libc 의 네이티브 GNU 구현이 사용되고 이 translation unit 은
 * #ifndef 가드로 비어있다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 옵션 파서(parse.c)·엔진 이름 매칭(ioengines.c) 등 대소문자
 *   무관 검색이 필요한 경로.
 * - 의존: <ctype.h> 의 tolower(3). 로케일 의존이지만 fio 의 사용 범위(ASCII
 *   옵션 이름)에서는 문제없다.
 * - 공유 상태: 없음(순수 함수·재진입 가능·스레드 안전).
 *
 * === 주요 함수 요약 ===
 * - strcasestr(): s1 에서 s2 의 첫 대소문자 무시 매치 위치를 반환. 빈 s2 면
 *   s1 의 시작을 반환. 없으면 NULL.
 */
#ifndef CONFIG_STRCASESTR  /* [한국어] libc 가 GNU strcasestr 을 제공하면 본 구현 제외. */

#include <ctype.h>  /* [한국어] tolower(3) 선언. 한 문자를 소문자로 정규화. */
#include <stddef.h>  /* [한국어] NULL 매크로 정의. */
#include "strcasestr.h"  /* [한국어] 공개 선언 — 헤더와 정의 시그니처 일치 보장. */

/*
 * [한국어]
 * strcasestr - 대소문자 무시 부분 문자열 검색
 *
 * @s1: 검색 대상(haystack) — NUL-종단 문자열. NULL 이 아니어야 한다.
 * @s2: 검색 패턴(needle) — NUL-종단 문자열. 빈 문자열이면 s1 의 시작 포인터
 *      반환(strstr 과 동일 관례).
 * @return: s1 내에서 s2 의 첫 등장 위치 포인터(const 를 떼어 캐스트하므로
 *          호출자는 이 포인터를 통해 s1 을 수정해서는 안 됨 — 원본이 const
 *          로 선언됐을 수 있음). 매치가 없으면 NULL.
 *
 * 알고리즘: do/while 루프로 s(현재 haystack 위치), p(현재 needle 위치)를
 * 동시에 진행하며 매치/미스매치를 처리한다.
 *  - p 가 끝("\0")에 도달 = 완전 매치 성공 → (char*)s1 반환.
 *  - *p 와 *s 가 일치(직접 또는 tolower 비교) → 둘 다 전진.
 *  - 미스매치 → p 를 s2 로 리셋하고 s1 을 한 칸 전진(s = ++s1).
 *    s1 전진 중 NUL 에 도달하면 "s 남았는지" 체크로 NULL 반환.
 *
 * 주의: `(*p == *s)` 의 단락평가로 대소문자 동일 ASCII 는 tolower 우회
 * (미세 최적화). 마지막 return 의 `*p ? NULL : (char *)s1` 은 실제로는
 * 도달하지 않는 dead code — while(1) 때문이지만 warning 억제 용이다.
 *
 * 호출 체인:
 *   옵션/엔진 이름 매칭 코드 → [strcasestr()] → tolower()
 */
char *strcasestr(const char *s1, const char *s2)
{
	const char *s = s1;  /* [한국어] 현재 haystack 스캔 위치. s1 은 각 매치 시도의 시작 후보로 유지. */
	const char *p = s2;  /* [한국어] 현재 needle 스캔 위치. 미스매치 시 s2 로 리셋. */

	do {
		if (!*p)  /* [한국어] needle 소진 = 완전 매치. 현 시도의 시작 s1 반환. */
			return (char *) s1;  /* [한국어] const 제거 캐스트 — GNU 시그니처 호환. */
		if ((*p == *s) ||
		    (tolower(*p) == tolower(*s))) {  /* [한국어] 한 문자 매치(케이스 무관). 아스키 고속 경로 + tolower 폴백. */
			++p;  /* [한국어] needle 전진. */
			++s;  /* [한국어] haystack 전진. */
		} else {  /* [한국어] 미스매치 — 현 시도 포기, needle 을 처음부터 다시. */
			p = s2;  /* [한국어] needle 리셋. */
			if (!*s)  /* [한국어] haystack 도 소진 = 실패. */
				return NULL;
			s = ++s1;  /* [한국어] haystack 시작점을 한 칸 뒤로 이동 후 그 위치부터 재시도. */
		}
	} while (1);  /* [한국어] 명시적 break 없음 — 내부 return 으로만 탈출. */

	return *p ? NULL : (char *) s1;  /* [한국어] 도달 불가(while(1)). 컴파일러 경고 억제용 방어적 return. */
}

#endif  /* [한국어] !CONFIG_STRCASESTR 블록 종료. */
