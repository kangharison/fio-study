/*
 * [한국어 설명] 2의 거듭제곱으로 올림 유틸리티 (roundup.h)
 *
 * === 파일의 역할 ===
 * 주어진 값을 가장 가까운 2의 거듭제곱으로 올림하는 roundup_pow2() 함수를 제공한다.
 * 내부적으로 __fls() (Find Last Set)를 사용하여 최상위 비트 위치를 구한 뒤,
 * 1을 해당 위치만큼 좌측 시프트하여 2의 거듭제곱 값을 계산한다.
 *
 * === fio에서의 사용 ===
 * 큐 깊이(iodepth)나 내부 버퍼 크기 등을 2의 거듭제곱으로 정렬해야 할 때 사용된다.
 * 링 버퍼나 해시 테이블 등 2의 거듭제곱 크기를 요구하는 자료구조의 크기 결정에 활용된다.
 */
#ifndef FIO_ROUNDUP_H
#define FIO_ROUNDUP_H

#include "lib/fls.h"

static inline unsigned roundup_pow2(unsigned depth)
{
	return 1UL << __fls(depth - 1);
}

#endif
