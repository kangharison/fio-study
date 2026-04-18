/*
 * [한국어 설명] 길이 제한 문자열 복제 함수 폴리필 구현 (strndup.c)
 *
 * === 파일의 역할 ===
 * POSIX.1-2008 에서 도입된 strndup(3) 함수의 폴리필을 제공한다. strndup 은
 * strdup 과 동일한 의미론이지만 최대 n 바이트까지만 복제하며, 원본 길이가
 * n 을 넘더라도 사본은 언제나 NUL-종단된다. POSIX.1-2008 미만 환경(구형
 * BSD·Windows)에서는 이 심볼이 없으므로 configure 단계에서
 * CONFIG_HAVE_STRNDUP 미정의 시 빌드에 포함된다. 구현은 malloc 으로
 * n+1 바이트 버퍼를 확보한 후 strncpy 로 최대 n 바이트를 복사하고
 * 명시적으로 0 바이트를 덧붙이는 2단계 — strncpy 는 n 바이트 복사 중
 * 원본이 먼저 종단되면 나머지를 0 으로 채우지만, n 을 채웠을 때는 NUL 을
 * 붙이지 않으므로 [n] 인덱스에 수동으로 '\0' 을 써서 불변식을 보장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전역에서 "문자열의 부분 복제 후 소유권 이전" 패턴이 필요한 경로에
 * 사용된다. 예를 들어 옵션 파서(parse.c)가 "--name=value" 같은 복합 토큰을
 * '=' 기준으로 잘라 name 부분만 힙 사본으로 넘길 때, iolog 에서 경로
 * prefix 를 추출할 때, blktrace 장치명 매칭 시 접두어 취득 때 호출된다.
 * CONFIG_HAVE_STRNDUP 가 정의되면 이 전체 translation unit 이 #ifndef
 * 가드로 비어있어 link 시 libc 의 네이티브 구현을 사용한다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 전역(옵션 파서·엔진 파라미터 분리·경로 치환 등).
 * - 의존: C 표준 라이브러리의 malloc(3)·strncpy(3). 할당 실패 시 NULL 을
 *   전파하므로 호출자는 반드시 반환값을 검사해야 한다. 해제는 free(3).
 * - 공유 상태: 없음 — 순수 함수. 재진입/스레드 안전(malloc 스레드 안전성
 *   전제).
 *
 * === 주요 함수 요약 ===
 * - strndup(): 원본에서 최대 n 바이트까지를 복제하여 NUL-종단 힙 사본을
 *   반환한다. 호출자는 free() 로 해제 책임을 가진다.
 */
#ifndef CONFIG_HAVE_STRNDUP  /* [한국어] libc 가 strndup 을 제공하면 이 구현은 제외 — 중복 심볼 회피. */

#include <stdlib.h>  /* [한국어] malloc/free 선언 — 힙 버퍼 할당에 필요. */
#include <string.h>  /* [한국어] strncpy 선언 — 길이 제한 복사에 사용. */
#include "strndup.h"  /* [한국어] 공개 API 선언을 함수 정의와 일치시키기 위한 로컬 헤더. */

/*
 * [한국어]
 * strndup - 문자열의 앞 n 바이트를 복제하여 NUL-종단 힙 사본 반환
 *
 * @s: 복제할 원본 문자열. strnlen(s, n) 바이트까지만 읽히므로, s 가 n 바이트
 *     이전에 '\0' 으로 끝나면 실제 복사량은 그 길이까지이다. s 가 전체 n
 *     바이트 유효 메모리 범위를 벗어나지 않아야 한다(호출자 책임).
 * @n: 복제할 최대 바이트 수(NUL 제외). n=0 이면 빈 문자열("")이 반환된다.
 * @return: 새로 malloc 된 NUL-종단 문자열 포인터. malloc 실패 시 NULL.
 *          호출자는 free(3) 로 해제해야 한다.
 *
 * 왜 필요한가: strcpy/strdup 은 길이 제한이 없어 경계가 모호한 입력에서
 * 버퍼 오버리드 위험이 있고, strncpy 는 항상 NUL-종단을 보장하지 않는다.
 * strndup 은 두 단점을 모두 회피하면서 "소유권 있는 사본"을 안전하게 생성한다.
 *
 * 동작 단계:
 * 1) malloc(n+1) — NUL 한 바이트 여분 확보.
 * 2) str != NULL 이면 strncpy 로 최대 n 바이트 복사.
 * 3) str[n] = '\0' 으로 경계 바이트를 명시 NUL 종단 (strncpy 가 NUL
 *    종단을 보장하지 않는 케이스 커버).
 * 4) 포인터 반환.
 *
 * 호출 체인:
 *   fio 전역 파서/포매터 → [strndup()] → malloc() + strncpy()
 */
char *strndup(const char *s, size_t n)
{
	char *str = malloc(n + 1);  /* [한국어] NUL 여분 위해 n+1 바이트 요청. n 이 매우 크면 NULL 가능. */

	if (str) {  /* [한국어] 할당 성공 시에만 복사; 실패 시 NULL 을 그대로 전파해 호출자 검사 의무화. */
		strncpy(str, s, n);  /* [한국어] 최대 n 바이트 복사. s 가 n 전에 끝나면 잔여는 0 으로 패딩. */
		str[n] = '\0';  /* [한국어] s 가 >= n 길이인 경우 strncpy 가 NUL 을 빠뜨릴 수 있으므로 명시 종단. */
	}

	return str;  /* [한국어] 소유권 있는 사본 또는 NULL. 호출자 free() 책임. */
}

#endif  /* [한국어] !CONFIG_HAVE_STRNDUP 블록 종료. */
