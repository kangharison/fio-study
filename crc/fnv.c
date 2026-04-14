/*
 * [한국어 설명] FNV (Fowler-Noll-Vo) 해시 구현 (fnv.c)
 *
 * === 파일의 역할 ===
 * FNV-1a 64비트 해시 함수를 구현한다.
 * FNV_PRIME(0x100000001b3)을 곱하고 데이터를 XOR하는 단순한 알고리즘이다.
 * 64비트 워드 단위로 처리하되, 잔여 바이트는 바이트 단위로 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c / crc/test.c → fnv() [이 파일]
 *
 * === 주요 함수 요약 ===
 * - fnv(): 64비트 FNV-1a 해시 계산 (시드값 hval로 시작)
 */
#include "fnv.h"

/* [한국어] FNV-1a 64비트 소수(prime) - FNV 해시의 핵심 상수 */
#define FNV_PRIME	0x100000001b3ULL

/*
 * [한국어]
 * fnv - 64비트 FNV-1a 해시 계산
 *
 * @buf: 해시를 계산할 데이터 버퍼
 * @len: 데이터 길이(바이트)
 * @hval: 해시 초기값(시드) - 표준 FNV offset basis 또는 이전 해시값
 * @return: 계산된 64비트 해시값
 *
 * 64비트 워드 단위로 처리하여 성능을 최적화하고,
 * 워드 경계에 맞지 않는 잔여 바이트는 빅엔디안 순서로 결합하여 처리한다.
 * 알고리즘: hval = (hval * FNV_PRIME) ^ data
 */
/*
 * 64-bit fnv, but don't require 64-bit multiples of data. Use bytes
 * for the last unaligned chunk.
 */
uint64_t fnv(const void *buf, uint32_t len, uint64_t hval)
{
	const uint64_t *ptr = buf;

	while (len) {
		hval *= FNV_PRIME;
		if (len >= sizeof(uint64_t)) {
			hval ^= (uint64_t) *ptr++;
			len -= sizeof(uint64_t);
			continue;
		} else {
			const uint8_t *ptr8 = (const uint8_t *) ptr;
			uint64_t val = 0;
			int i;

			for (i = 0; i < len; i++) {
				val <<= 8;
				val |= (uint8_t) *ptr8++;
			}
			hval ^= val;
			break;
		}
	}

	return hval;
}
