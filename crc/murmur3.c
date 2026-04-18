/*
 * [한국어 설명] MurmurHash3 (x86 32비트 변형) 구현 (murmur3.c)
 *
 * === 파일의 역할 ===
 * Austin Appleby의 비-암호 해시 알고리즘 MurmurHash3 의 "x86 32bit" 변형을
 * 구현한다. 4바이트(32비트) 블록 단위로 처리하며, 각 블록에 두 상수
 * c1=0xcc9e2d51, c2=0x1b873593 를 이용한 곱셈·좌회전·XOR 를 적용하고,
 * 블록 간에는 누적 상태 h1 을 `rotl32(h1,13)*5 + 0xe6546b64` 로 퍼뜨린다.
 * 버퍼 말미의 1~3바이트 잔여(tail)는 별도 switch fall-through 로 처리하며,
 * 마지막에 fmix32 로 비트를 완전히 뒤섞어 "avalanche effect"를 달성한다.
 * 암호 안전성은 없으며, 해시 테이블 분산과 비-위조성 체크섬(오류 검출) 용도.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 및 난수/해시 유틸 경로에서 사용. 주로 crc/test.c 의 --crctest
 * 벤치마크 대상으로 등록되며, 일부 hash.h 계열 유틸과 동일 맥락에서 해시
 * 키 생성 보조로도 활용된다.
 * 호출 체인:
 *   verify.c / crc/test.c::t_murmur3 → [murmurhash3] → murmur3_tail → fmix32 → rotl32
 *
 * === 타 모듈과의 연결 ===
 * - murmur3.h: murmurhash3() 프로토타입, MURMUR3_SEED(있다면) 상수.
 * - compiler/compiler.h: `fio_fallthrough` 매크로([[fallthrough]]/__attribute__((fallthrough)))
 *   제공 — switch 폴스루 경고 억제용.
 * - verify.c: verify=murmur3 옵션이 있다면 호출.
 * - crc/test.c: --crctest murmur3 래퍼가 NR_CHUNKS회 호출.
 * 데이터 흐름: 쓰기 버퍼 → murmurhash3(seed=고정값) → 32비트 해시 → verify/테스트 누적.
 * 동기화: 순수 함수 — 재진입 안전·전역 없음. 모든 상수는 const.
 *
 * === 주요 함수 요약 ===
 * - rotl32(x, r) [static inline]: 32비트 좌회전. "(x<<r)|(x>>(32-r))".
 * - fmix32(h) [static inline]: 최종 비트 확산 — h ^= h>>16; h*=0x85ebca6b; h ^= h>>13;
 *   h*=0xc2b2ae35; h ^= h>>16. 모든 입력 비트가 모든 출력 비트에 영향을 주도록 설계.
 * - murmur3_tail(data, nblocks, len, c1, c2, h1): 마지막 1~3바이트 잔여 처리.
 * - murmurhash3(key, len, seed): 공용 진입점 — 4바이트 블록 루프 + tail + fmix32.
 */
#include "murmur3.h"
/* [한국어] murmur3.h: murmurhash3() 프로토타입 선언. verify.c/test.c 가 이 헤더 포함. */
#include "../compiler/compiler.h"
/* [한국어] compiler.h: fio_fallthrough 매크로(GCC/Clang 의
 * __attribute__((fallthrough)) 또는 C2x [[fallthrough]]) — switch 폴스루 경고를
 * 의도적으로 허용함을 컴파일러에 알린다. */

/*
 * [한국어]
 * rotl32 - 32비트 좌회전(rotate left)
 *
 * @x: 회전 대상 32비트 값.
 * @r: 회전 비트 수(0..31). 33 이상은 UB(호출자 책임).
 * @return: r비트 좌회전 결과.
 *
 * 회전은 "좌 시프트한 비트와 우측으로 밀려난 비트를 다시 끌어와 결합"하는 연산.
 * MurmurHash3 의 핵심 비트 섞음 연산 — 곱셈 다음에 늘 등장해 비트 상관을 파괴한다.
 *
 * 호출 체인: murmurhash3/murmur3_tail/fmix32(아님, 여기선 직접 사용 안 함) → [rotl32]
 */
static inline uint32_t rotl32(uint32_t x, int8_t r)
{
	/* [한국어] (x << r): 상위로 r비트 이동 — 상위 r비트는 잃는다.
	 *         (x >> (32 - r)): 우측으로 밀려난 비트가 다시 하위에 들어오도록 보충.
	 * OR 로 결합 → 순환 이동 완성. */
	return (x << r) | (x >> (32 - r));
}

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

/*
 * [한국어]
 * fmix32 - MurmurHash3 최종 혼합(finalization mix) 함수
 *
 * @h: 최종 혼합 전 누적 해시값.
 * @return: 비트 단위 상관이 완전히 깨진 최종 32비트 해시.
 *
 * XOR-shift + 곱셈을 3쌍 적용해 "입력의 어떤 1비트가 바뀌어도 출력 비트 절반이
 * 평균적으로 뒤집히는" avalanche 성질을 만들어낸다. 상수 0x85ebca6b, 0xc2b2ae35
 * 는 Austin Appleby 가 테스트로 찾아낸 최적값으로, 바꾸면 해시 분산이 무너진다.
 *
 * 호출 체인: murmur3_tail → [fmix32] → (연산만)
 */
static inline uint32_t fmix32(uint32_t h)
{
	/* [한국어] 상위 16비트를 하위로 XOR 반영 — 상·하위 비트 혼합 단계 1. */
	h ^= h >> 16;
	/* [한국어] 곱셈 상수 0x85ebca6b — 비선형 혼합 1. */
	h *= 0x85ebca6b;
	/* [한국어] 상위 19비트를 하위로 XOR 반영(h>>13 의 상위 비트가 하위 13비트와 섞임). */
	h ^= h >> 13;
	/* [한국어] 곱셈 상수 0xc2b2ae35 — 비선형 혼합 2. */
	h *= 0xc2b2ae35;
	/* [한국어] 마지막 XOR-shift — 세 번째 상·하위 혼합으로 avalanche 성질 완성. */
	h ^= h >> 16;

	/* [한국어] 혼합 완료된 해시 반환. */
	return h;
}

/*
 * [한국어]
 * murmur3_tail - 4바이트 블록으로 정렬되지 않는 잔여 1~3바이트 처리
 *
 * @data:    입력 시작 포인터(블록 루프가 사용한 원본).
 * @nblocks: 이미 처리한 4바이트 블록 수 — tail 시작 오프셋 계산용.
 * @len:    원 입력 총 길이(바이트) — tail 길이는 (len & 3), fmix32 에도 길이 XOR 입력.
 * @c1, @c2: 블록 처리 곱셈 상수(호출자와 동일한 0xcc9e2d51, 0x1b873593).
 * @h1:     블록 루프가 반환한 누적 해시 — tail 과 혼합 후 fmix32 에 전달.
 * @return: 잔여 + 길이 XOR + fmix32 를 거친 최종 32비트 해시.
 *
 * switch/fallthrough 구조: len&3 이 3·2·1 에 따라 한 바이트씩 k1 에 모은 뒤,
 * case 1 구간에서만 본격적인 비선형 혼합(k1*c1; k1<<<15; k1*c2; h1^=k1) 을 수행.
 * 이후 fmix32(h1 ^ len) 으로 결과 finalize.
 *
 * 호출 체인: murmurhash3 → [murmur3_tail] → fmix32 → rotl32
 */
static uint32_t murmur3_tail(const uint8_t *data, const int nblocks,
			     uint32_t len, const uint32_t c1,
			     const uint32_t c2, uint32_t h1)
{
	/* [한국어] 블록 처리가 끝난 바로 뒤 위치에서 tail 바이트 포인터를 계산.
	 * nblocks*4 바이트가 이미 소비됐으므로 data + (nblocks*4) 부터 1~3바이트가 남아 있다. */
	const uint8_t *tail = (const uint8_t *)(data + nblocks * 4);

	/* [한국어] 잔여 바이트 누적용 임시 변수 — 0부터 쌓아 올린다(상위 비트부터 채움). */
	uint32_t k1 = 0;
	/* [한국어] len & 3 == tail 바이트 수. 3→2→1 으로 fallthrough 하며 한 바이트씩 쌓는다. */
	switch (len & 3) {
	case 3:
		/* [한국어] 세 번째 tail 바이트를 bit 16~23 자리에 배치. */
		k1 ^= tail[2] << 16;
		/* [한국어] case 2 로 의도적 폴스루 — 컴파일러 경고 억제. */
		fio_fallthrough;
	case 2:
		/* [한국어] 두 번째 tail 바이트를 bit 8~15 자리에 배치. */
		k1 ^= tail[1] << 8;
		/* [한국어] case 1 로 의도적 폴스루. */
		fio_fallthrough;
	case 1:
		/* [한국어] 첫 번째 tail 바이트를 bit 0~7 자리에 배치. */
		k1 ^= tail[0];
		/* [한국어] 블록 처리와 동일한 혼합 시퀀스 — c1 곱, 15비트 좌회전, c2 곱. */
		k1 *= c1;
		k1 = rotl32(k1, 15);
		k1 *= c2;
		/* [한국어] 누적 해시에 tail 혼합값 반영. */
		h1 ^= k1;
	};

	/* [한국어] 길이를 XOR 해 "같은 비트 패턴이라도 길이가 다르면 해시도 다름"을 보장하고
	 * 최종 avalanche 를 위해 fmix32 적용. */
	return fmix32(h1 ^ len);
}

/*
 * [한국어]
 * murmurhash3 - MurmurHash3 32비트(x86) 해시 계산 공용 진입점
 *
 * @key:  입력 버퍼 포인터.
 * @len:  바이트 길이.
 * @seed: 해시 시드(외부에서 제공하는 초기값) — 같은 key 라도 seed 가 다르면 해시도 다름.
 * @return: 32비트 해시.
 *
 * 동작 순서:
 *   1) 입력을 4바이트 블록 nblocks 개로 분할(len/4). blocks 포인터는 블록 끝에서
 *      역방향 인덱스 [-nblocks..0) 로 접근해 한 번의 포인터 산술로 끝 경계 확인을 겸함.
 *   2) 각 블록 k1 = blocks[i] 에 대해:
 *        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
 *        h1 ^= k1; h1 = rotl32(h1, 13); h1 = h1*5 + 0xe6546b64;
 *   3) 남은 tail 바이트를 murmur3_tail 이 처리 — 내부에서 fmix32 호출로 finalize.
 *
 * 호출 체인: verify.c / crc/test.c → [murmurhash3] → murmur3_tail → fmix32 → rotl32
 */
uint32_t murmurhash3(const void *key, uint32_t len, uint32_t seed)
{
	/* [한국어] 입력 버퍼를 바이트 포인터로 재해석 — tail 처리에 필요. */
	const uint8_t *data = (const uint8_t *)key;
	/* [한국어] 블록 개수(=len/4). 나머지는 murmur3_tail 이 처리. */
	const int nblocks = len / 4;
	/* [한국어] 누적 해시 상태(h1) — seed 에서 시작. */
	uint32_t h1 = seed;
	/* [한국어] MurmurHash3 x86 변형의 1차 곱셈 상수. */
	const uint32_t c1 = 0xcc9e2d51;
	/* [한국어] 2차 곱셈 상수. */
	const uint32_t c2 = 0x1b873593;
	/* [한국어] 블록 배열의 "끝 다음" 포인터 — 아래 루프에서 i<0 음수 인덱스로 접근하여
	 * [blocks-nblocks..blocks) 범위를 순회한다. 버퍼 끝에서 거슬러 올라가는 방식은
	 * 캐시 프리페처와 분기 예측에 유리. */
	const uint32_t *blocks = (const uint32_t *)(data + nblocks * 4);
	/* [한국어] 음수 인덱스 카운터 — -nblocks 에서 시작해 0이 되면 종료. */
	int i;

	/* [한국어] nblocks 회 반복 — 각 반복은 1개의 4바이트 블록 처리. */
	for (i = -nblocks; i; i++) {
		/* [한국어] 현재 블록 값(uint32_t 로 읽어들인 4바이트). */
		uint32_t k1 = blocks[i];

		/* [한국어] 1차 혼합: c1 곱. */
		k1 *= c1;
		/* [한국어] 15비트 좌회전 — 곱셈 결과 비트를 휘감아 순환시킴. */
		k1 = rotl32(k1, 15);
		/* [한국어] 2차 혼합: c2 곱 — k1 은 이제 "혼합된 블록". */
		k1 *= c2;

		/* [한국어] 누적 해시에 블록 혼합값 XOR. */
		h1 ^= k1;
		/* [한국어] h1 자체도 13비트 좌회전하여 비트 상관 파괴. */
		h1 = rotl32(h1, 13);
		/* [한국어] 마지막 스프레드: h1 = h1*5 + 0xe6546b64. MurmurHash3 의 핵심 스프레드 상수. */
		h1 = h1 * 5 + 0xe6546b64;
	}

	/* [한국어] tail(1~3바이트) 처리 + 길이 XOR + fmix32 로 최종 해시 반환. */
	return murmur3_tail(data, nblocks, len, c1, c2, h1);
}
