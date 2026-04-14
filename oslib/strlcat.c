/*
 * [한국어 설명] 안전한 문자열 연결 함수 폴리필 구현 (strlcat.c)
 *
 * === 파일의 역할 ===
 * strlcat() 함수의 폴리필 구현을 제공한다. strlcat()은 OpenBSD에서 유래한
 * 안전한 문자열 연결 함수로, strncat()과 달리 대상 버퍼의 전체 크기(dsize)를
 * 인자로 받아 항상 NUL 종료를 보장하며 버퍼 오버플로를 방지한다.
 * 반환값이 dsize 이상이면 잘림(truncation)이 발생했음을 알 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio 전역의 문자열 조작에서 사용된다.
 * CONFIG_STRLCAT이 정의되면 시스템 제공 함수를 사용하고 이 파일은 제외된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 전반에서 문자열 연결이 필요한 곳
 * - 의존: 표준 C 문자열 함수 (strlen)
 *
 * === 주요 함수 요약 ===
 * - strlcat(): dst에 src를 안전하게 연결, 항상 NUL 종료 보장
 */
#ifndef CONFIG_STRLCAT
/*
 * Copyright (c) 1998, 2015 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>
#include "strlcat.h"

/*
 * [한국어]
 * strlcat - 버퍼 크기를 고려한 안전한 문자열 연결
 *
 * @dst: 대상 문자열 버퍼
 * @src: 연결할 원본 문자열
 * @dsize: dst 버퍼의 전체 크기 (strncat과 달리 남은 공간이 아닌 전체 크기)
 * @return: strlen(src) + MIN(dsize, strlen(initial dst))
 *          반환값 >= dsize이면 잘림(truncation)이 발생한 것
 *
 * strncat()보다 안전한 대안으로, 항상 NUL 종료를 보장한다.
 * 동작 과정:
 * 1) dst에서 NUL 종료 위치를 찾되 dsize를 넘지 않음
 * 2) 남은 공간(n)에 src의 문자를 복사
 * 3) 항상 NUL 종료 문자를 추가
 *
 * 호출 체인:
 *   fio 전반 → [strlcat()] (C 라이브러리 대체)
 */
/*
 * Appends src to string dst of size dsize (unlike strncat, dsize is the
 * full size of dst, not space left).  At most dsize-1 characters
 * will be copied.  Always NUL terminates (unless dsize <= strlen(dst)).
 * Returns strlen(src) + MIN(dsize, strlen(initial dst)).
 * If retval >= dsize, truncation occurred.
 */
size_t
strlcat(char *dst, const char *src, size_t dsize)
{
	const char *odst = dst;
	const char *osrc = src;
	size_t n = dsize;
	size_t dlen;

	/* [한국어] dst의 끝(NUL 위치)을 찾되, dsize를 넘지 않도록 함 */
	/* Find the end of dst and adjust bytes left but don't go past end. */
	while (n-- != 0 && *dst != '\0')
		dst++;
	dlen = dst - odst;
	/* [한국어] 남은 공간 계산: 전체 크기 - 현재 dst 길이 */
	n = dsize - dlen;

	/* [한국어] 남은 공간이 0이면 잘림 발생, src 길이를 더해 반환 */
	if (n-- == 0)
		return(dlen + strlen(src));
	/* [한국어] src의 문자를 남은 공간만큼 복사 */
	while (*src != '\0') {
		if (n != 0) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	/* [한국어] 항상 NUL 종료 보장 */
	*dst = '\0';

	return(dlen + (src - osrc));	/* count does not include NUL */
}

#endif
