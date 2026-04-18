/*
 * [한국어 설명] 버퍼 크기 제한 안전 문자열 연결 함수 폴리필 구현 (strlcat.c)
 *
 * === 파일의 역할 ===
 * OpenBSD 에서 유래해 널리 이식된 strlcat(3)(Todd C. Miller, 1998·2015) 의
 * 폴리필을 제공한다. strncat(3) 의 설계상 함정(남은 공간 계산 잘못 → 오버
 * 플로, 잘림 감지 어려움)을 피하기 위해 만들어진 함수로, 세 가지 특징이 있다:
 *  1) 인자 dsize 는 "dst 버퍼의 남은 공간"이 아니라 "dst 버퍼의 전체 크기"
 *     이다. 호출자가 매 번 남은 공간을 계산할 필요 없음.
 *  2) dsize > 0 이면 항상 NUL-종단을 보장한다 — strncat 의 "꽉 차면 NUL
 *     생략" 함정 회피.
 *  3) 반환값이 dsize 이상이면 잘림(truncation) 발생 신호. 호출자가 간단한
 *     비교 한 번으로 안전성 검사 가능.
 * 반환값은 시도했을 때의 최종 문자열 길이(= strlen(src) + min(dsize,
 * strlen(initial dst))) 이며, 이 수식은 glibc style 의 snprintf 반환값과
 * 같은 "시도 길이" 계약이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 oslib/ 이식성 계층. GNU glibc 는 strlcat 을 제공하지 않으며(논쟁적
 * 정책 이유), BSD·musl·일부 Linux 배포판은 제공한다. configure 가
 * CONFIG_STRLCAT 미정의 시 본 구현을 사용. 호출 사이트는 고정 크기 배열에
 * 동적 문자열을 안전하게 이어붙이는 곳들 — 경로 조립, 에러 메시지 생성 등.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 전역의 경로/메시지 조립 경로.
 * - 의존: <string.h> 의 strlen(3). 헤더 호환을 위한 <sys/types.h> 의 size_t.
 * - 공유 상태: 없음(순수 함수, 재진입/스레드 안전).
 *
 * === 주요 함수 요약 ===
 * - strlcat(): dst + src 를 dsize 바이트 제한 하에 이어붙이고 NUL 종단 보장.
 *   반환값 >= dsize 이면 잘림.
 */
#ifndef CONFIG_STRLCAT  /* [한국어] libc 에 strlcat 이 있으면 본 정의 제외. */
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

#include <sys/types.h>  /* [한국어] size_t 타입 정의 포함. */
#include <string.h>  /* [한국어] strlen 선언. */
#include "strlcat.h"  /* [한국어] 공개 API 선언 — 헤더/정의 시그니처 일치 보장. */

/*
 * [한국어]
 * strlcat - 버퍼 크기 제한 하에 dst 뒤에 src 이어 붙이기 (항상 NUL-종단)
 *
 * @dst: 대상 버퍼. 이미 NUL-종단 문자열을 포함해야 함(dlen 계산 전제).
 *       dsize 바이트 중 일부(현재 dlen 까지)가 유효 문자열.
 * @src: 이어 붙일 원본 NUL-종단 문자열.
 * @dsize: dst 버퍼의 "전체" 크기(strncat 의 "남은 공간"과 다름).
 * @return: strlen(src) + MIN(dsize, strlen(initial dst)) — 잘림이 없었다면
 *          최종 문자열 길이. 반환값 >= dsize 이면 잘림 발생.
 *
 * 왜 이 API 가 필요한가:
 *  - strncat 은 남은 공간을 받아 이를 실수로 잘못 계산하면 쉽게 오버플로.
 *  - strlcat 은 "전체 크기"만 전달하면 내부에서 남은 공간을 계산하므로 오류
 *    확률이 낮고, 항상 NUL 종단, 잘림 감지가 반환값 단일 비교로 가능.
 *
 * 동작 단계:
 * 1) n = dsize. dst 를 n 이 0 이 되기 전까지 한 칸씩 전진하며 NUL 을 찾는다
 *    (dst 실제 길이 dlen 계산).
 * 2) 남은 공간 n = dsize - dlen.
 * 3) n == 0 이면 dst 가 이미 꽉 참 — 잘림 결과 반환(dlen + strlen(src)).
 * 4) src 를 한 문자씩 읽으며 n > 0 일 때만 dst 에 복사, 항상 src 포인터는
 *    전진(남은 길이 측정 목적).
 * 5) 마지막에 *dst = '\0' 으로 NUL 종단 보장.
 * 6) 반환값 = dlen + (src 이동 거리) — NUL 제외 길이 계약.
 *
 * 호출 체인:
 *   fio 전역 경로/메시지 조립 → [strlcat()]
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
	const char *odst = dst;  /* [한국어] dst 원 시작 — dlen 계산용 앵커. */
	const char *osrc = src;  /* [한국어] src 원 시작 — 반환값 계산용 앵커. */
	size_t n = dsize;  /* [한국어] 남은 공간 카운터(매 단계에서 의미가 바뀐다). */
	size_t dlen;  /* [한국어] dst 의 초기 strlen. */

	/* [한국어] dst의 끝(NUL 위치)을 찾되, dsize를 넘지 않도록 함 */
	/* Find the end of dst and adjust bytes left but don't go past end. */
	while (n-- != 0 && *dst != '\0')  /* [한국어] dsize 범위 내에서 NUL 탐색. n-- 후 0 이어도 루프 진입 한 번 더 가능. */
		dst++;
	dlen = dst - odst;  /* [한국어] dst 초기 문자열 길이(NUL 제외). */
	/* [한국어] 남은 공간 계산: 전체 크기 - 현재 dst 길이 */
	n = dsize - dlen;  /* [한국어] 이제 n 은 "dst 뒤에 쓸 수 있는 남은 바이트(NUL 포함)". */

	/* [한국어] 남은 공간이 0이면 잘림 발생, src 길이를 더해 반환 */
	if (n-- == 0)
		return(dlen + strlen(src));  /* [한국어] dst 가 꽉 차있어 복사 불가 — 잘림. strlen(src) 별도 측정 필요. */
	/* [한국어] src의 문자를 남은 공간만큼 복사 */
	while (*src != '\0') {
		if (n != 0) {  /* [한국어] 남은 공간이 있을 때만 실제 기록. */
			*dst++ = *src;  /* [한국어] src 문자 복사 후 dst 전진. */
			n--;  /* [한국어] 남은 공간 1 감소. */
		}
		src++;  /* [한국어] 복사 여부와 무관하게 src 전진 — 최종 src 이동 거리로 strlen(src) 계산. */
	}
	/* [한국어] 항상 NUL 종료 보장 */
	*dst = '\0';  /* [한국어] n-- 에서 1 바이트 예약해둔 자리에 NUL. */

	return(dlen + (src - osrc));	/* count does not include NUL */  /* [한국어] 시도 총 길이 반환 — snprintf 와 동일 계약. */
}

#endif  /* [한국어] !CONFIG_STRLCAT 종료. */
