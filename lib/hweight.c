/*
 * [한국어 설명] 해밍 가중치 (popcount) 구현 (hweight.c)
 *
 * === 파일의 역할 ===
 * 정수에서 1로 설정된 비트의 개수(해밍 가중치, popcount)를 세는 함수를 제공한다.
 * 8비트, 32비트, 64비트 버전이 각각 구현되어 있으며, 비트 연산 트릭을 사용하여
 * 효율적으로 계산한다.
 *
 * === fio에서의 사용 ===
 * fio 내부에서 비트마스크의 설정된 비트 수를 계산해야 할 때 사용된다.
 * 예를 들어 CPU 친화성 마스크에서 활성 CPU 수를 세는 등의 용도로 활용된다.
 */
#include "hweight.h"

/*
 * [한국어] hweight8 - 8비트 정수의 설정된 비트 수(popcount)를 반환
 *
 * 비트 조작 트릭 (Hamming weight 알고리즘):
 * 1단계: 인접 비트 쌍의 합을 계산 (0x55 마스크)
 * 2단계: 4비트 그룹의 합을 계산 (0x33 마스크)
 * 3단계: 8비트 전체 합을 계산 (0x0F 마스크)
 */
unsigned int hweight8(uint8_t w)
{
	unsigned int res = w - ((w >> 1) & 0x55);

	res = (res & 0x33) + ((res >> 2) & 0x33);
	return (res + (res >> 4)) & 0x0F;
}

/* [한국어] hweight32 - 32비트 popcount. hweight8과 동일한 알고리즘의 32비트 확장 */
unsigned int hweight32(uint32_t w)
{
	unsigned int res = w - ((w >> 1) & 0x55555555);

	res = (res & 0x33333333) + ((res >> 2) & 0x33333333);
	res = (res + (res >> 4)) & 0x0F0F0F0F;
	res = res + (res >> 8);
	return (res + (res >> 16)) & 0x000000FF;
}

/*
 * [한국어] hweight64 - 64비트 popcount
 * 32비트 시스템에서는 상위/하위 32비트를 각각 계산하여 합산하고,
 * 64비트 시스템에서는 64비트 전용 마스크로 직접 계산한다.
 */
unsigned int hweight64(uint64_t w)
{
#if BITS_PER_LONG == 32
	/* [한국어] 32비트 시스템: 상위/하위 절반을 각각 계산 후 합산 */
	return hweight32((unsigned int)(w >> 32)) + hweight32((unsigned int)w);
#else
	uint64_t res = w - ((w >> 1) & 0x5555555555555555ULL);
	res = (res & 0x3333333333333333ULL) + ((res >> 2) & 0x3333333333333333ULL);
	res = (res + (res >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
	res = res + (res >> 8);
	res = res + (res >> 16);
	return (res + (res >> 32)) & 0x00000000000000FFULL;
#endif
}
