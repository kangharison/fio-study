/*
 * [한국어 설명] 길이 제한 문자열 복제 함수 폴리필 구현 (strndup.c)
 *
 * === 파일의 역할 ===
 * strndup() 함수의 폴리필 구현을 제공한다. 원본 문자열에서 최대 n바이트까지
 * 복제하여 NUL 종료된 새 힙 메모리를 반환한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio에서 부분 문자열 복제가 필요한 곳에서 사용된다.
 * CONFIG_HAVE_STRNDUP이 정의되면 이 파일은 컴파일에서 제외된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 전반에서 문자열 부분 복제 필요 시
 * - 의존: malloc(), strncpy() (C 표준 라이브러리)
 *
 * === 주요 함수 요약 ===
 * - strndup(): 최대 n바이트까지 문자열을 복제하여 동적 할당된 사본 반환
 */
#ifndef CONFIG_HAVE_STRNDUP

#include <stdlib.h>
#include <string.h>
#include "strndup.h"

/*
 * [한국어]
 * strndup - 최대 n바이트까지 문자열을 복제
 *
 * @s: 복제할 원본 문자열
 * @n: 복제할 최대 바이트 수
 * @return: 새로 할당된 문자열 포인터, 실패 시 NULL
 *
 * n+1 바이트를 할당하고 strncpy로 최대 n바이트를 복사한 후
 * 마지막에 NUL 종료 문자를 추가한다. 호출자가 free()로 해제해야 한다.
 *
 * 호출 체인:
 *   fio 전반 → [strndup()] → malloc(), strncpy()
 */
char *strndup(const char *s, size_t n)
{
	char *str = malloc(n + 1);

	if (str) {
		strncpy(str, s, n);
		str[n] = '\0';
	}

	return str;
}

#endif
