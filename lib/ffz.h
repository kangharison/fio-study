/*
 * [한국어 설명] 첫 번째 0 비트 찾기 (Find First Zero) 유틸리티 (ffz.h)
 *
 * === 파일의 역할 ===
 * 비트마스크에서 첫 번째로 설정되지 않은(0인) 비트의 위치를 찾는 함수를 제공한다.
 * 내부적으로 ffs64() 함수를 통해 64비트 워드에서 첫 번째 설정된 비트를 이진 탐색 방식으로 찾고,
 * ffz()와 ffz64()는 비트마스크를 반전(~)시켜 ffs64()를 호출함으로써 첫 번째 0 비트를 찾는다.
 *
 * === fio에서의 사용 ===
 * fio의 I/O 엔진이나 내부 자료구조에서 비트맵 기반 자원 할당 시 빈 슬롯을 탐색하는 데 사용된다.
 * 아키텍처별 최적화된 ffz가 있으면(ARCH_HAVE_FFZ) 해당 구현을 사용하고, 없으면 범용 구현을 사용한다.
 */
#ifndef FIO_FFZ_H
#define FIO_FFZ_H

#include <inttypes.h>

static inline int ffs64(uint64_t word)
{
	int r = 0;

	if ((word & 0xffffffff) == 0) {
		r += 32;
		word >>= 32;
	}
	if (!(word & 0xffff)) {
		word >>= 16;
		r += 16;
	}
	if (!(word & 0xff)) {
		word >>= 8;
		r += 8;
	}
	if (!(word & 0xf)) {
		word >>= 4;
		r += 4;
	}
	if (!(word & 3)) {
		word >>= 2;
		r += 2;
	}
	if (!(word & 1))
		r += 1;

	return r;
}

#ifndef ARCH_HAVE_FFZ

static inline int ffz(unsigned long bitmask)
{
	return ffs64(~bitmask);
}

#else
#define ffz(bitmask)	arch_ffz(bitmask)
#endif

static inline int ffz64(uint64_t bitmask)
{
	return ffs64(~bitmask);
}

#endif
