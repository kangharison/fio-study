/*
 * [한국어 설명] 최상위 설정 비트 찾기 (Find Last Set) 유틸리티 (fls.h)
 *
 * === 파일의 역할 ===
 * 32비트 정수에서 가장 높은 위치에 설정된 비트(최상위 비트)의 위치를 반환하는 __fls() 함수를 제공한다.
 * 이진 탐색 방식으로 상위 비트부터 확인하며, fls(0)=0, fls(1)=1, fls(0x80000000)=32를 반환한다.
 * Linux 커널의 비트 연산 유틸리티에서 유래한 구현이다.
 *
 * === fio에서의 사용 ===
 * 주로 roundup_pow2() 등에서 값을 2의 거듭제곱으로 올림할 때 최상위 비트 위치를 구하는 데 사용된다.
 * 큐 깊이나 버퍼 크기를 2의 거듭제곱으로 정렬하는 등의 내부 연산에 활용된다.
 */
#ifndef _ASM_GENERIC_BITOPS_FLS_H_
#define _ASM_GENERIC_BITOPS_FLS_H_

/**
 * fls - find last (most-significant) bit set
 * @x: the word to search
 *
 * This is defined the same way as ffs.
 * Note fls(0) = 0, fls(1) = 1, fls(0x80000000) = 32.
 */

static inline int __fls(int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		r -= 1;
	}
	return r;
}

#endif /* _ASM_GENERIC_BITOPS_FLS_H_ */
