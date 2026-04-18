/*
 * [한국어 설명] 블룸 필터(Bloom filter) 구현 (bloom.c)
 *
 * === 파일의 역할 ===
 * 블룸 필터는 "주어진 원소가 집합에 이미 존재하는가?" 를 상수 시간 + 상수 메모리로
 * 근사 판정하는 확률적 자료구조이다. 특성:
 *   - false negative 불가(있다고 보고된 원소가 실제로 없는 경우는 없음 단, SET 동시에 하는 경로 한정)
 *   - false positive 가능(없는 원소를 "있다" 고 잘못 보고할 수 있음)
 * 본 파일은 5 개 독립 해시 함수(jhash, XXH32, murmurhash3, CRC32C, FNV) 를 사용하여
 * 단일 비트맵에 원소의 지문을 기록한다. 5 개 모두가 1 로 설정된 경우에만 "존재" 로
 * 판정한다. CRC32C 는 모든 바이트를 해시에 반영하지 않는 wrapper 가 있어 fio 가
 * 제공하는 fio_crc32c(seed 인자 무시) 를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 --dedupe_percentage 옵션 경로에서 중복 제거(deduplication) 감지의 보조
 * 자료구조로 사용된다. I/O 버퍼 패턴을 생성할 때 "이 버퍼와 같은 지문이 이미
 * 기록된 적 있는가?" 를 빠르게 조회하여 중복 쓰기 패턴을 시뮬레이션한다.
 * 비트맵의 비트가 많을수록 false positive 율이 내려가므로 호출자는 예상 원소 수 ×
 * 비율(예: 20x) 정도의 bit 수로 초기화한다.
 *
 * === 타 모듈과의 연결 ===
 * - bloom.h:                     struct bloom 불투명 선언 및 본 파일의 API 프로토타입.
 * - ../hash.h / ../crc/*.h:      jhash, XXH32, murmurhash3, crc32c(+arm64/intel probe), fnv 공급.
 * - verify.c / dedupe 경로:      주 호출자.
 *
 * === 주요 함수/구조체 요약 ===
 * - bloom_new(entries):          비트맵 비트 수 entries 로 필터 생성. calloc 으로 0 초기화.
 * - bloom_free(b):               비트맵과 구조체 자체 해제.
 * - bloom_set(b, data, nwords):  uint32 배열 데이터를 "추가+조회" 한다. 이미 존재로 판정되면 true.
 * - bloom_string(b, s, len, set):문자열 데이터. set=true 는 추가+조회, false 는 조회만.
 * - __bloom_check(b, d, l, set): 내부 구현 — 5 개 해시 계산 후 각 비트를 검사/설정.
 * 자체 구조체:
 *   struct bloom { uint64_t nentries; uint32_t *map; } — 비트맵 크기와 메모리 포인터.
 *   struct bloom_hash { seed, fn } — 해시 함수 디스크립터 N_HASHES(=5) 개 배열.
 */
#include <stdlib.h>             /* [한국어] malloc/calloc/free — 필터 생성/해제 */

#include "bloom.h"              /* [한국어] 공개 API 프로토타입 */
#include "../hash.h"            /* [한국어] jhash(Jenkins) — 5 개 해시 중 하나 */
#include "../crc/xxhash.h"      /* [한국어] XXH32(Yann Collet) — 초고속 비암호학적 해시 */
#include "../crc/murmur3.h"     /* [한국어] murmurhash3(Austin Appleby) — 공간 분산성 우수 */
#include "../crc/crc32c.h"      /* [한국어] fio_crc32c + crc32c_arm64_probe / crc32c_intel_probe(HW 가속 프로브) */
#include "../crc/fnv.h"         /* [한국어] FNV-1a — 단순하나 분포 양호 */

/* [한국어]
 * 블룸 필터 본체.
 * - nentries: 비트맵의 총 비트 수. calloc 된 map 이 담는 비트 개수의 상한.
 *   설정자: bloom_new 에서 호출자 입력 값 저장.
 *   읽는 자: __bloom_check 가 해시 결과 % nentries 로 비트 인덱스 산출.
 *   값 범위: 사용자 입력 — 일반적으로 수 MiB~수 GiB 비트 수.
 *   동기화: 단일 스레드 사용 전제(fio dedupe 경로는 각 잡 단독). 공유 시 외부 락 필요.
 * - map: 실제 비트 저장소. uint32_t 배열이며, 각 요소당 32 비트.
 *   설정자: __bloom_check(set=true) 가 OR 연산으로 비트 세팅.
 *   읽는 자: __bloom_check 가 AND 연산으로 비트 테스트.
 *   값 범위: 비트맵 — 순수 비트 집합.
 *   동기화: nentries 와 동일 — 단일 스레드 전제. */
struct bloom {
	uint64_t nentries;
	/* [한국어] 이 필터에 저장 가능한 총 비트 수.
	 * 설정자: bloom_new() 가 사용자 입력으로 고정.
	 * 읽는 자: __bloom_check() 가 해시값 % nentries 로 비트 슬롯 인덱스 산출.
	 * 값 범위: uint64_t — 메모리 한계 내 임의 수.
	 * 동기화: 읽기 전용(초기화 이후 변경 없음). */
	uint32_t *map;
	/* [한국어] 비트맵 배열. 크기 = ceil(nentries / 32) uint32 elements.
	 * 설정자: bloom_new() calloc (0 초기화) + __bloom_check(set=true) OR 로 비트 세팅.
	 * 읽는 자: __bloom_check() AND 로 비트 확인.
	 * 값 범위: 비트 필드 — 각 비트 0 또는 1.
	 * 동기화: 단일 스레드 사용 전제. 여러 잡에서 공유하려면 원자 OR + 락 필요. */
};

/* [한국어] uint32_t 당 비트 수 = 32. 해시값을 32 로 나눠 어느 배열 요소인지 결정 */
#define BITS_PER_INDEX	(sizeof(uint32_t) * 8)
/* [한국어] 배열 요소 내 비트 오프셋 마스크(0x1F). 해시값 하위 5비트로 어느 비트인지 결정 */
#define BITS_INDEX_MASK	(BITS_PER_INDEX - 1)

/* [한국어]
 * 해시 함수 디스크립터.
 * - seed: 해시 함수에 전달할 초기 시드(BLOOM_SEED=0x8989 공통).
 * - fn:   해시 함수 포인터. 시그니처 (data, len, seed) → 32비트 해시.
 * 설정자: hashes[] 정적 초기화 시 한 번.
 * 읽는 자: __bloom_check 루프가 각 엔트리의 fn 호출.
 * 동기화: 읽기 전용(전역 const 배열). */
struct bloom_hash {
	unsigned int seed;
	/* [한국어] 해시 함수의 초기 시드(BLOOM_SEED 공용). 해시마다 동일 시드를 쓰지만
	 * 서로 다른 알고리즘으로 인해 5 개 해시 결과는 독립에 가까운 분포를 보임.
	 * 설정자: 배열 정적 초기화.
	 * 읽는 자: __bloom_check. */
	uint32_t (*fn)(const void *, uint32_t, uint32_t);
	/* [한국어] 해시 함수 포인터 — (data, len, seed) 받아 32비트 해시 반환.
	 * CRC32C 는 시드 개념이 없어 bloom_crc32c 래퍼로 인자 버림.
	 * 설정자: 배열 정적 초기화.
	 * 읽는 자: __bloom_check. */
};

/* [한국어]
 * bloom_crc32c - CRC32C 를 bloom_hash 시그니처에 맞춘 래퍼.
 *
 * @buf:  해시 대상 데이터.
 * @len:  길이.
 * @seed: 무시(CRC32C 는 시드 입력 없음).
 * @return: 32비트 CRC32C 해시.
 *
 * fio_crc32c 는 SSE4.2/ARMv8 HW 명령을 사용 가능한 경우 가속된다.
 * 호출 체인: __bloom_check → hashes[3].fn → [bloom_crc32c] → fio_crc32c.
 */
static uint32_t bloom_crc32c(const void *buf, uint32_t len, uint32_t seed)
{
	return fio_crc32c(buf, len);   /* [한국어] seed 파라미터 버림 — CRC32C 시드 없음 */
}

/* [한국어]
 * bloom_fnv - FNV-1a 해시를 표준 시그니처에 맞춘 래퍼.
 * 그대로 전달하는 단순 래퍼이지만 bloom_hash 의 함수 포인터 타입과 일치시키기 위해 존재.
 */
static uint32_t bloom_fnv(const void *buf, uint32_t len, uint32_t seed)
{
	return fnv(buf, len, seed);
}

/* [한국어] 블룸 필터에서 공통 사용하는 해시 시드.
 * 5 개 해시 함수 모두 같은 시드를 받지만, 알고리즘이 달라 결과는 서로 독립에 가깝다 */
#define BLOOM_SEED	0x8989

/* [한국어] 5 개 해시 함수 디스크립터 배열. 순서는 __bloom_check 에 고정된 의미는 없고,
 * 추가/제거 시 N_HASHES 매크로와 일치시켜야 함.
 * 설정자: 정적 초기화.
 * 읽는 자: __bloom_check 루프. */
static struct bloom_hash hashes[] = {
	{
		.seed = BLOOM_SEED,
		.fn = jhash,        /* [한국어] Jenkins hash — 소형 키에서 분포 양호 */
	},
	{
		.seed = BLOOM_SEED,
		.fn = XXH32,        /* [한국어] xxHash 32비트 — 매우 빠른 비암호 해시 */
	},
	{
		.seed = BLOOM_SEED,
		.fn = murmurhash3,  /* [한국어] MurmurHash3 — 표준적 블룸필터 해시 */
	},
	{
		.seed = BLOOM_SEED,
		.fn = bloom_crc32c, /* [한국어] CRC32C (HW 가속 활용) */
	},
	{
		.seed = BLOOM_SEED,
		.fn = bloom_fnv,    /* [한국어] FNV-1a */
	},
};

/* [한국어] 해시 함수 개수. 이 값이 클수록 false positive 율이 감소하나 연산 비용 증가.
 * 5 는 fio 의 dedupe 사용 맥락에서 empirical 로 결정된 타협값 */
#define N_HASHES	5

/*
 * [한국어]
 * bloom_new - 블룸 필터 생성.
 *
 * @entries: 비트맵의 총 비트 수. 원소 수가 아닌 **비트 수** 로 지정 — 혼동 주의.
 *           일반적 공식: n(원소 수) × k(해시 개수) / ln2 로 false positive 율 1% 수준 목표.
 * @return:  malloc/calloc 으로 할당된 struct bloom*. calloc 실패 시 free 후 NULL 반환.
 *
 * 동작 단계:
 *   1) CRC32C HW 가속 프로브(arm64/intel) — 일회성, idempotent.
 *   2) struct bloom 을 malloc (nentries 채움).
 *   3) no_uints = ceil(entries / 32) uint32 요소 수 계산.
 *   4) calloc 으로 비트맵을 0 초기화 — 미설정 비트 = "없음" 을 보장.
 *   5) 실패 시 구조체 free 후 NULL.
 *
 * 실행 컨텍스트: fio 초기화 단계(메인 스레드).
 * 호출 체인: 초기화 경로 → [bloom_new] → malloc/calloc + crc32c 프로브.
 *
 * 에러 처리: calloc 실패 시 부분 할당(b) 정리 후 NULL 반환.
 */
struct bloom *bloom_new(uint64_t entries)
{
	/* [한국어] 반환할 필터 핸들 */
	struct bloom *b;
	/* [한국어] 비트맵을 담을 uint32 요소 개수 */
	size_t no_uints;

	/* [한국어] CRC32C HW 가속 초기화 — 내부 플래그를 세팅해 fio_crc32c 가 HW 경로를 선택 */
	crc32c_arm64_probe();
	crc32c_intel_probe();

	/* [한국어] 구조체 먼저 할당 — nentries 필드를 설정할 수 있어야 map 할당 크기 계산 */
	b = malloc(sizeof(*b));
	b->nentries = entries;
	/* [한국어] 비트 수 → uint32 개수로 올림 환산: (entries + 31) / 32 */
	no_uints = (entries + BITS_PER_INDEX - 1) / BITS_PER_INDEX;
	/* [한국어] calloc 으로 모두 0 초기화 — 미설정 비트 = "아직 추가 안 됨" */
	b->map = calloc(no_uints, sizeof(uint32_t));
	/* [한국어] 메모리 부족 시 부분 할당(b) 회수 + NULL 반환 */
	if (!b->map) {
		free(b);
		return NULL;
	}

	return b;
}

/* [한국어]
 * bloom_free - 블룸 필터와 비트맵을 해제.
 *
 * @b: bloom_new 가 반환한 핸들. NULL 금지.
 *
 * 동작 단계: map 해제 → 구조체 해제.
 * 호출 체인: fio 정리 경로 → [bloom_free] → free.
 */
void bloom_free(struct bloom *b)
{
	free(b->map);       /* [한국어] 비트맵부터 해제(댕글링 방지) */
	free(b);            /* [한국어] 구조체 자체 해제 */
}

/*
 * [한국어]
 * __bloom_check - 블룸 필터 내부 구현 — 5 개 해시 결과로 비트 조회/설정.
 *
 * @b:    필터 핸들.
 * @data: 조회/설정할 데이터의 시작 주소.
 * @len:  데이터 길이(바이트).
 * @set:  true=존재 검사 + 미설정 비트를 세팅(추가), false=조회만 (한 비트라도 0 이면 즉시 false 반환).
 * @return: 5 개 해시 모두가 1 로 설정되어 있었으면 true(이미 존재 또는 false positive).
 *          set=false 경로에서 하나라도 0 이면 즉시 false(확실히 없음).
 *
 * 동작 단계:
 *   1) 5 개 해시를 독립 계산하여 각각 nentries 로 모듈로 — 비트 슬롯 인덱스 확정.
 *   2) 각 인덱스에 대해 배열/비트 위치 분해 후 비트 확인.
 *      - 이미 1 이면 was_set++.
 *      - 0 이고 set=true 면 비트 세팅(OR). was_set 증가는 없음(새로 추가된 비트).
 *      - 0 이고 set=false 면 조회 모드에서 "확실히 없음" 판정 — 즉시 break(조기 종료).
 *   3) 반환: was_set == N_HASHES 이면 true(이미 존재 또는 false positive).
 *
 * 실행 컨텍스트: 잡 스레드. 단일 스레드 소유 전제.
 *
 * 호출 체인: bloom_set/bloom_string → [__bloom_check] → 5 개 해시 함수.
 *
 * 에러 처리: 내부 상태 변경 전 검증 없음 — 호출자 계약에 의존.
 */
static bool __bloom_check(struct bloom *b, const void *data, unsigned int len,
			  bool set)
{
	/* [한국어] 5 개 해시 결과 저장 배열 */
	uint32_t hash[N_HASHES];
	/* [한국어] i: 루프 인덱스, was_set: 이미 1 이었던 비트 개수(존재 판정용) */
	int i, was_set;

	/* [한국어] 5 개의 독립 해시 계산. 모듈로 nentries 로 비트 슬롯 범위 제한 */
	for (i = 0; i < N_HASHES; i++) {
		hash[i] = hashes[i].fn(data, len, hashes[i].seed);
		hash[i] = hash[i] % b->nentries;
	}

	/* [한국어] 일치 카운터 초기화 — 5 이면 "모두 이미 설정됨" 판정 */
	was_set = 0;
	for (i = 0; i < N_HASHES; i++) {
		/* [한국어] uint32 배열 인덱스: hash / 32 */
		const unsigned int index = hash[i] / BITS_PER_INDEX;
		/* [한국어] 해당 uint32 내 비트 오프셋: hash & 0x1F */
		const unsigned int bit = hash[i] & BITS_INDEX_MASK;

		/* [한국어] 이미 설정된 비트 — 기존 원소와 충돌(또는 진짜 중복) */
		if (b->map[index] & (1U << bit))
			was_set++;
		else if (set)
			/* [한국어] 새 비트 세팅(추가 모드). OR 연산 — 비원자 단일스레드 전제 */
			b->map[index] |= 1U << bit;
		else
			/* [한국어] 조회 모드에서 미설정 비트 발견 → "확실히 없음" — 조기 종료 */
			break;
	}

	/* [한국어] 5 개 모두 설정되어 있었으면 true (이미 존재 또는 FP). 4 개 이하면 false */
	return was_set == N_HASHES;
}

/*
 * [한국어]
 * bloom_set - uint32_t 배열 데이터를 필터에 추가하고 이미 존재했는지 반환.
 *
 * @b:      필터 핸들.
 * @data:   uint32_t 배열 데이터 포인터. nwords 개 요소.
 * @nwords: 요소 개수(uint32 기준).
 * @return: true=이미 존재(또는 FP); false=새로 추가됨.
 *
 * 호출 체인: dedupe 감지 경로 → [bloom_set] → __bloom_check(set=true).
 */
bool bloom_set(struct bloom *b, uint32_t *data, unsigned int nwords)
{
	/* [한국어] 바이트 길이로 환산 후 __bloom_check 호출 */
	return __bloom_check(b, data, nwords * sizeof(uint32_t), true);
}

/*
 * [한국어]
 * bloom_string - 임의 바이트 스트림(문자열/버퍼) 을 조회하거나 추가.
 *
 * @b:    필터 핸들.
 * @data: 데이터 시작 포인터.
 * @len:  길이(바이트).
 * @set:  true=추가+조회, false=조회만.
 * @return: true=이미 존재(또는 FP); false=없음(set=false) / 새로 추가됨(set=true).
 *
 * 호출 체인: 다양한 경로 → [bloom_string] → __bloom_check.
 */
bool bloom_string(struct bloom *b, const char *data, unsigned int len,
		  bool set)
{
	return __bloom_check(b, data, len, set);
}
