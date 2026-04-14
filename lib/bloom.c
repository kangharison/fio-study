/*
 * [한국어 설명] 블룸 필터 구현 (bloom.c)
 *
 * === 파일의 역할 ===
 * 블룸 필터는 확률적 자료구조로, 특정 원소가 집합에 포함되어 있는지를 빠르게 판별한다.
 * 5개의 해시 함수(jhash, xxhash, murmurhash3, crc32c, fnv)를 사용하여 비트맵에 매핑하며,
 * false positive는 가능하지만 false negative는 발생하지 않는다.
 *
 * === fio에서의 사용 ===
 * fio의 중복 제거(deduplication) 감지에 사용된다. I/O 데이터 블록이 이미 기록된
 * 데이터와 중복되는지를 빠르게 확인하여 중복 쓰기 패턴을 시뮬레이션한다.
 */
#include <stdlib.h>

#include "bloom.h"
#include "../hash.h"
#include "../crc/xxhash.h"
#include "../crc/murmur3.h"
#include "../crc/crc32c.h"
#include "../crc/fnv.h"

struct bloom {
	uint64_t nentries;
	/* [한국어] 비트맵에 매핑 가능한 총 엔트리(비트) 수 */
	uint32_t *map;
	/* [한국어] 비트맵 배열. nentries 비트를 uint32_t 배열로 저장 */
};

#define BITS_PER_INDEX	(sizeof(uint32_t) * 8)
#define BITS_INDEX_MASK	(BITS_PER_INDEX - 1)

struct bloom_hash {
	unsigned int seed;
	/* [한국어] 해시 함수의 초기 시드 (BLOOM_SEED=0x8989) */
	uint32_t (*fn)(const void *, uint32_t, uint32_t);
	/* [한국어] 해시 함수 포인터 (data, len, seed) → 32비트 해시값 */
};

static uint32_t bloom_crc32c(const void *buf, uint32_t len, uint32_t seed)
{
	return fio_crc32c(buf, len);
}

static uint32_t bloom_fnv(const void *buf, uint32_t len, uint32_t seed)
{
	return fnv(buf, len, seed);
}

#define BLOOM_SEED	0x8989

static struct bloom_hash hashes[] = {
	{
		.seed = BLOOM_SEED,
		.fn = jhash,
	},
	{
		.seed = BLOOM_SEED,
		.fn = XXH32,
	},
	{
		.seed = BLOOM_SEED,
		.fn = murmurhash3,
	},
	{
		.seed = BLOOM_SEED,
		.fn = bloom_crc32c,
	},
	{
		.seed = BLOOM_SEED,
		.fn = bloom_fnv,
	},
};

#define N_HASHES	5

/*
 * [한국어] bloom_new - 블룸 필터를 생성
 *
 * @entries: 비트맵의 총 비트 수 (원소 수가 아닌 비트 수로 지정)
 * @return: 생성된 블룸 필터 (실패 시 NULL)
 *
 * CRC32C 하드웨어 가속을 프로브하고 비트맵 메모리를 calloc으로 할당한다.
 * 5개의 해시 함수를 사용하므로, false positive 확률은
 * (1 - e^(-5*n/m))^5 (n=원소 수, m=entries)로 추정된다.
 *
 * 호출 체인: fio 초기화 → [bloom_new]
 */
struct bloom *bloom_new(uint64_t entries)
{
	struct bloom *b;
	size_t no_uints;

	crc32c_arm64_probe();
	crc32c_intel_probe();

	b = malloc(sizeof(*b));
	b->nentries = entries;
	no_uints = (entries + BITS_PER_INDEX - 1) / BITS_PER_INDEX;
	b->map = calloc(no_uints, sizeof(uint32_t));
	if (!b->map) {
		free(b);
		return NULL;
	}

	return b;
}

void bloom_free(struct bloom *b)
{
	free(b->map);
	free(b);
}

/*
 * [한국어] __bloom_check - 블룸 필터에 원소를 검사하고 선택적으로 추가
 *
 * @b: 블룸 필터
 * @data: 검사할 데이터
 * @len: 데이터 길이
 * @set: true이면 존재하지 않는 비트를 설정 (추가), false이면 조회만
 * @return: 모든 해시 비트가 이미 설정되어 있었으면 true (이미 존재)
 *
 * 5개의 해시 함수로 데이터를 해시하고, 각 결과를 비트맵의 인덱스로 변환한다.
 * set=true일 때는 비트를 설정하면서 검사, set=false일 때는 하나라도 0이면 즉시 false 반환.
 */
static bool __bloom_check(struct bloom *b, const void *data, unsigned int len,
			  bool set)
{
	uint32_t hash[N_HASHES];
	int i, was_set;

	/* [한국어] 5개의 독립적 해시 함수로 데이터를 해시하고 비트 위치로 변환 */
	for (i = 0; i < N_HASHES; i++) {
		hash[i] = hashes[i].fn(data, len, hashes[i].seed);
		hash[i] = hash[i] % b->nentries;
	}

	was_set = 0;
	for (i = 0; i < N_HASHES; i++) {
		const unsigned int index = hash[i] / BITS_PER_INDEX;
		const unsigned int bit = hash[i] & BITS_INDEX_MASK;

		if (b->map[index] & (1U << bit))
			was_set++;	/* [한국어] 이미 설정된 비트 - 기존 원소와 충돌 가능 */
		else if (set)
			b->map[index] |= 1U << bit;	/* [한국어] 새 비트 설정 (추가 모드) */
		else
			break;	/* [한국어] 조회 모드에서 미설정 비트 발견 → 확실히 없음 */
	}

	/* [한국어] 5개 모두 설정되어 있었으면 true (이미 존재하거나 false positive) */
	return was_set == N_HASHES;
}

/*
 * [한국어] bloom_set - uint32_t 배열 데이터를 블룸 필터에 추가하고 이미 존재했는지 반환
 * @return: true이면 이미 존재 (또는 false positive)
 */
bool bloom_set(struct bloom *b, uint32_t *data, unsigned int nwords)
{
	return __bloom_check(b, data, nwords * sizeof(uint32_t), true);
}

/*
 * [한국어] bloom_string - 문자열 데이터의 블룸 필터 멤버십 검사/추가
 * @set: true이면 추가 + 검사, false이면 검사만
 * @return: true이면 이미 존재 (또는 false positive)
 */
bool bloom_string(struct bloom *b, const char *data, unsigned int len,
		  bool set)
{
	return __bloom_check(b, data, len, set);
}
