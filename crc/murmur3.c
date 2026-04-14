/*
 * [한국어 설명] MurmurHash3 해시 구현 (murmur3.c)
 *
 * === 파일의 역할 ===
 * MurmurHash3 32비트(x86) 변형을 구현한다.
 * 4바이트 블록 단위로 처리하며, 곱셈 + 회전 + XOR 연산을 결합하여
 * 우수한 눈사태 효과(avalanche effect)를 달성한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c / crc/test.c → murmurhash3() [이 파일]
 *
 * === 타 모듈과의 연결 ===
 * - murmur3.h: 인터페이스 정의
 * - compiler/compiler.h: fio_fallthrough 매크로 (switch fall-through 경고 억제)
 *
 * === 주요 함수 요약 ===
 * - murmurhash3(): 32비트 MurmurHash3 계산 (시드 지원)
 * - rotl32(): 32비트 좌측 순환 시프트 (내부)
 * - fmix32(): 최종 혼합(finalization mix) - 눈사태 효과 강화 (내부)
 * - murmur3_tail(): 4바이트 미만 잔여 데이터 처리 (내부)
 */
#include "murmur3.h"
#include "../compiler/compiler.h"

/*
 * [한국어]
 * rotl32 - 32비트 좌측 순환 시프트
 *
 * @x: 회전할 값
 * @r: 회전 비트 수
 * @return: r비트만큼 좌측 회전된 값
 */
static inline uint32_t rotl32(uint32_t x, int8_t r)
{
	return (x << r) | (x >> (32 - r));
}

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

/*
 * [한국어]
 * fmix32 - 최종 혼합(finalization mix) 함수
 *
 * @h: 혼합할 해시값
 * @return: 눈사태 효과가 적용된 최종 해시값
 *
 * XOR 시프트 + 곱셈을 3단계로 적용하여 모든 입력 비트가
 * 출력의 모든 비트에 영향을 미치도록 한다 (완전 눈사태 효과).
 * 상수 0x85ebca6b, 0xc2b2ae35는 실험적으로 최적화된 값이다.
 */
static inline uint32_t fmix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;

	return h;
}

static uint32_t murmur3_tail(const uint8_t *data, const int nblocks,
			     uint32_t len, const uint32_t c1,
			     const uint32_t c2, uint32_t h1)
{
	const uint8_t *tail = (const uint8_t *)(data + nblocks * 4);

	uint32_t k1 = 0;
	switch (len & 3) {
	case 3:
		k1 ^= tail[2] << 16;
		fio_fallthrough;
	case 2:
		k1 ^= tail[1] << 8;
		fio_fallthrough;
	case 1:
		k1 ^= tail[0];
		k1 *= c1;
		k1 = rotl32(k1, 15);
		k1 *= c2;
		h1 ^= k1;
	};

	return fmix32(h1 ^ len);
}

/*
 * [한국어]
 * murmurhash3 - MurmurHash3 32비트 해시 계산
 *
 * @key: 해시를 계산할 데이터 포인터
 * @len: 데이터 길이(바이트)
 * @seed: 해시 시드값 - 같은 데이터라도 시드가 다르면 다른 해시를 생성
 * @return: 계산된 32비트 해시값
 *
 * 동작 과정:
 *   1) 데이터를 4바이트 블록 단위로 처리: 각 블록에 c1/c2 상수를 곱하고
 *      회전 후 h1에 XOR → h1을 회전하고 5를 곱한 뒤 상수 추가
 *   2) 잔여 바이트(1~3바이트)를 murmur3_tail()로 처리
 *   3) 최종 길이 XOR 후 fmix32()로 눈사태 효과 적용
 *
 * 호출 체인:
 *   verify.c / crc/test.c → [murmurhash3] → murmur3_tail() → fmix32()
 */
uint32_t murmurhash3(const void *key, uint32_t len, uint32_t seed)
{
	const uint8_t *data = (const uint8_t *)key;
	const int nblocks = len / 4;
	uint32_t h1 = seed;
	const uint32_t c1 = 0xcc9e2d51;
	const uint32_t c2 = 0x1b873593;
	const uint32_t *blocks = (const uint32_t *)(data + nblocks * 4);
	int i;

	for (i = -nblocks; i; i++) {
		uint32_t k1 = blocks[i];

		k1 *= c1;
		k1 = rotl32(k1, 15);
		k1 *= c2;

		h1 ^= k1;
		h1 = rotl32(h1, 13);
		h1 = h1 * 5 + 0xe6546b64;
	}

	return murmur3_tail(data, nblocks, len, c1, c2, h1);
}
