/*
 * [한국어 설명] 계층적 "스파스 비트맵"(axmap) — fio 의 랜덤맵 구현 (axmap.c)
 *
 * === 파일의 역할 ===
 * fio 가 랜덤 I/O 모드에서 "이미 방문한 블록" 을 추적하는 영구 자료구조를 제공한다.
 * 단순 비트맵 대신 log64(n) 레벨의 계층 구조(bitmap-of-bitmaps)로 구성되어
 * 레벨 N 의 비트 하나가 레벨 N-1 의 한 워드(64비트)가 모두 채워졌는지를 나타낸다.
 * 이로 인해 ① 비트맵이 채워질수록 탐색/조회가 짧아지고 ② "다음 미사용 블록"
 * 탐색이 최상위에서 하향 O(log n) 으로 빠르게 수행되며 ③ 캐시 친화적이다.
 * 20000 블록 기준 오버헤드 약 1.9% (블록당 1.019비트), 점근적으로 1.58% 로 수렴.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 기본 랜덤맵(`norandommap=0`) 경로의 핵심이다. 사용 흐름:
 *
 *   init.c
 *     → td_random_map_alloc() / init_random_state
 *         → axmap_new(nr_blocks)          // [본 파일] 레벨 수 계산 + 각 레벨 malloc
 *
 *   io_u.c (랜덤 I/O 생성)
 *     → get_next_rand_offset()
 *         → axmap_isset(map, bit)         // 이미 쓴 블록인지 확인 (topdown, O(log n))
 *         → axmap_set(map, bit)           // 쓰면서 사용됨으로 표시 (bottom-up 전파)
 *         → axmap_next_free(map, bit)     // 다음 미사용 블록 찾기
 *
 *   종료 시
 *     → axmap_free(map)                   // 모든 레벨 + 루트 해제
 *
 * 실행 컨텍스트: 잡 스레드 당 하나의 axmap 인스턴스를 소유하므로 단일 스레드
 * 접근(락 불필요). 대체 랜덤 생성기(lfsr)는 axmap 과 배타적.
 *
 * === 타 모듈과의 연결 ===
 * - axmap.h : struct axmap 전방선언, 공개 API(axmap_new/free/reset/set/isset/
 *             next_free/set_nr) 선언.
 * - ../arch/arch.h : BITS_PER_LONG, ffz(find-first-zero) — 아키텍처별 비트 연산
 *             하드웨어 명령(BSF/CLZ/TZCNT 등) 기반 포팅.
 * - ../minmax.h : min() 매크로 — 워드 경계 클램핑에 사용.
 * - 데이터 흐름: 블록 번호(uint64_t) → UNIT_SHIFT(6비트) 단위로 분할 →
 *             (offset, bit) 쌍으로 변환 → axmap_handler 가 레벨 0..N-1 순회.
 *
 * === 주요 함수/구조체 요약 ===
 * - axmap_new(nr_bits): log64(nr_bits) 레벨 구조 할당 후 0 초기화.
 * - axmap_free(axmap): 모든 레벨 map 배열과 자기 자신 해제.
 * - axmap_reset(axmap): 모든 레벨을 0 으로 재초기화 (랜덤 범위 순회 재시작 시).
 * - axmap_set(bit_nr): 단일 비트 설정. 아래 레벨부터 상위로 전파하여 워드가
 *                      가득 찼으면 상위 비트도 세워 요약 정보를 유지.
 * - axmap_set_nr(bit_nr, nr_bits): 한 워드 경계 내에서 연속 nr_bits 설정.
 * - axmap_isset(bit_nr): 특정 비트 조회. 최상위 레벨부터 하향 탐색(조기 종료).
 * - axmap_next_free(bit_nr): bit_nr 다음 빈 비트. 레벨 0 현재 워드 → topdown 검색 → wrap.
 * - axmap_find_first_free(bit_nr): 최상위부터 하향으로 첫 빈 비트 탐색.
 * - struct axmap/axmap_level: 다단계 비트맵 루트/한 레벨.
 * - axmap_handler/axmap_handler_topdown: 레벨 순회 콜백 디스패처.
 */

/*
 * Bitmap of bitmaps, where each layer is number-of-bits-per-word smaller than
 * the previous. Hence an 'axmap', since we axe each previous layer into a
 * much smaller piece. I swear, that is why it's named like that. It has
 * nothing to do with anything remotely narcissistic.
 *
 * A set bit at layer N indicates a full word at layer N-1, and so forth. As
 * the bitmap becomes progressively more full, checking for existence
 * becomes cheaper (since fewer layers are walked, making it a lot more
 * cache friendly) and locating the next free space likewise.
 *
 * Axmaps get pretty close to optimal (1 bit per block) space usage, since
 * layers quickly diminish in size. Doing the size math is straight forward,
 * since we have log64(blocks) layers of maps. For 20000 blocks, overhead
 * is roughly 1.9%, or 1.019 bits per block. The number quickly converges
 * towards 1.0158, or 1.58% of overhead.
 */
#include <stdio.h>	/* [한국어] (디버깅용 printf 가 필요한 경우 대비. 현재 구현엔 직접 호출 없음) */
#include <stdlib.h>	/* [한국어] malloc/calloc/free — 레벨 배열과 각 map 의 동적 할당 */
#include <string.h>	/* [한국어] memset — axmap_reset 에서 레벨의 비트맵을 일괄 0 으로 클리어 */
#include <assert.h>	/* [한국어] assert — 내부 불변식 검증 (릴리스에서 NDEBUG 로 제거 가능) */

#include "../arch/arch.h"	/* [한국어] BITS_PER_LONG (32/64), ffz() (find-first-zero) — 아키텍처별 비트 연산 */
#include "axmap.h"		/* [한국어] 본 파일의 공개 API 선언 (struct axmap 은 여기서 전방선언) */
#include "../minmax.h"		/* [한국어] min(a, b) 매크로 — 워드 경계 클램핑에 사용 */

/* [한국어] 한 워드(unsigned long) 에 포함되는 비트 수의 log2 값.
 * 64비트 플랫폼: 6 (2^6 = 64비트), 32비트: 5 (2^5 = 32비트).
 * 이 값은 bit_nr → (offset, bit) 변환(`offset = bit_nr >> UNIT_SHIFT`,
 * `bit = bit_nr & MASK`) 의 상수로 전역 사용된다. */
#if BITS_PER_LONG == 64
#define UNIT_SHIFT		6	/* [한국어] 64비트: log2(64) */
#elif BITS_PER_LONG == 32
#define UNIT_SHIFT		5	/* [한국어] 32비트: log2(32) */
#else
#error "Number of arch bits unknown"	/* [한국어] 지원하지 않는 비트 폭 — 컴파일 타임 에러 */
#endif

/* [한국어] 한 워드에 들어가는 블록(비트) 수와 비트 인덱스 마스크 */
#define BLOCKS_PER_UNIT		(1U << UNIT_SHIFT)		/* [한국어] 64 또는 32 */
#define BLOCKS_PER_UNIT_MASK	(BLOCKS_PER_UNIT - 1)		/* [한국어] 0x3F 또는 0x1F — `& MASK` 로 비트 오프셋 */

/* [한국어] bit_masks[n] = n 개의 하위 비트가 1 인 마스크 (n=0..64).
 * axmap_set_fn 이 "한 워드 안에서 bit 부터 nr_bits 개를 한 번에 OR 세팅" 할 때
 * `bit_masks[nr_bits] << bit` 로 마스크를 계산한다. 64비트 플랫폼에서는
 * 최대 n=64 (0xFFFFFFFFFFFFFFFF) 를 포함해야 하므로 아래 #if 64 분기가 추가된다. */
static const unsigned long bit_masks[] = {
	0x0000000000000000, 0x0000000000000001, 0x0000000000000003, 0x0000000000000007,
	0x000000000000000f, 0x000000000000001f, 0x000000000000003f, 0x000000000000007f,
	0x00000000000000ff, 0x00000000000001ff, 0x00000000000003ff, 0x00000000000007ff,
	0x0000000000000fff, 0x0000000000001fff, 0x0000000000003fff, 0x0000000000007fff,
	0x000000000000ffff, 0x000000000001ffff, 0x000000000003ffff, 0x000000000007ffff,
	0x00000000000fffff, 0x00000000001fffff, 0x00000000003fffff, 0x00000000007fffff,
	0x0000000000ffffff, 0x0000000001ffffff, 0x0000000003ffffff, 0x0000000007ffffff,
	0x000000000fffffff, 0x000000001fffffff, 0x000000003fffffff, 0x000000007fffffff,
	0x00000000ffffffff,
#if BITS_PER_LONG == 64
	0x00000001ffffffff, 0x00000003ffffffff, 0x00000007ffffffff, 0x0000000fffffffff,
	0x0000001fffffffff, 0x0000003fffffffff, 0x0000007fffffffff, 0x000000ffffffffff,
	0x000001ffffffffff, 0x000003ffffffffff, 0x000007ffffffffff, 0x00000fffffffffff,
	0x00001fffffffffff, 0x00003fffffffffff, 0x00007fffffffffff, 0x0000ffffffffffff,
	0x0001ffffffffffff, 0x0003ffffffffffff, 0x0007ffffffffffff, 0x000fffffffffffff,
	0x001fffffffffffff, 0x003fffffffffffff, 0x007fffffffffffff, 0x00ffffffffffffff,
	0x01ffffffffffffff, 0x03ffffffffffffff, 0x07ffffffffffffff, 0x0fffffffffffffff,
	0x1fffffffffffffff, 0x3fffffffffffffff, 0x7fffffffffffffff, 0xffffffffffffffff	/* [한국어] 64비트 포화 (전체 1) — 한 워드 완전 채움 마스크 */
#endif
};

/**
 * struct axmap_level - a bitmap used to implement struct axmap
 * @level: Level index. Each map has at least one level with index zero. The
 *	higher the level index, the fewer bits a struct axmap_level contains.
 * @map_size: Number of elements of the @map array.
 * @map: A bitmap with @map_size elements.
 */
struct axmap_level {
	int level;
	/* [한국어] 이 레벨의 인덱스. 0 = 최하위(실제 블록 사용 여부).
	 * 상위 레벨(i>0)의 한 비트는 하위 레벨(i-1)의 한 워드가 전부 1인지를 요약한다.
	 * 설정자: axmap_new 초기화 루프에서 한 번 기록.
	 * 읽는 자: axmap_set_fn 이 `if (!al->level)` 로 레벨 0 특례 처리(set_bits 카운팅). */

	unsigned long map_size;
	/* [한국어] map 배열의 원소 개수 (unsigned long 워드 수).
	 * 설정자: axmap_new 에서 nr_bits / 64 의 올림으로 계산.
	 * 읽는 자: axmap_find_first_free 경계 검사, axmap_reset 의 memset 크기. */

	unsigned long *map;
	/* [한국어] 이 레벨의 실제 비트맵 배열. 레벨 0 은 `사용됨` 여부,
	 * 상위 레벨은 하위 워드 충만 요약.
	 * 설정자: axmap_new 가 malloc 으로 할당, axmap_reset 이 memset(0).
	 * 읽는 자: axmap_handler/handler_topdown 로 set/isset/find_first_free 경로가 공유.
	 * 메모리 모델: 단일 잡 스레드만 접근하므로 동기화 불필요. */
};

/**
 * struct axmap - a set that can store numbers 0 .. @nr_bits - 1
 * @nr_level: Number of elements of the @levels array.
 * @levels: struct axmap_level array in which lower levels contain more bits
 *	than higher levels.
 * @nr_bits: One more than the highest value stored in the set.
 */
struct axmap {
	unsigned int nr_levels;
	/* [한국어] 계층 수 = ceil(log64(nr_bits)). 예: 1M 블록 → 4 레벨.
	 * 설정자: axmap_new 에서 루프로 결정. 읽는 자: 모든 handler. */

	struct axmap_level *levels;
	/* [한국어] 레벨 배열. levels[0] 이 최하위(실제 비트),
	 * levels[nr_levels-1] 이 최상위(루트 요약).
	 * 설정자: axmap_new 가 calloc. 읽는 자: 전 API 공유.
	 * 해제: axmap_free 에서 각 level.map 해제 후 배열 해제. */

	uint64_t nr_bits;
	/* [한국어] 추적하는 전체 비트(블록) 수. 잡의 랜덤 I/O 총 블록 수에 대응.
	 * 값 범위: 1 ~ 2^64-1 (현실적으로 TB 단위까지).
	 * 설정자: axmap_new 파라미터로 저장. 읽는 자: axmap_next_free/isset 경계 검사. */
};

/*
 * [한국어] axmap_reset - 비트맵의 모든 비트를 0 으로 초기화
 *
 * @axmap: 대상 axmap
 *
 * 모든 레벨의 map 배열을 memset(0) 한다. axmap_new 후 초기 상태를 보장하거나,
 * 잡이 랜덤 범위를 한 바퀴 순회한 뒤 재시작할 때 호출.
 *
 * 실행 컨텍스트: 잡 스레드 초기화/재시작. 단일 스레드.
 */
/* Remove all elements from the @axmap set */
void axmap_reset(struct axmap *axmap)
{
	int i;

	for (i = 0; i < axmap->nr_levels; i++) {		/* [한국어] 레벨 0 부터 최상위까지 순회 */
		struct axmap_level *al = &axmap->levels[i];

		memset(al->map, 0, al->map_size * sizeof(unsigned long));	/* [한국어] 바이트 단위 영 초기화 — 아키텍처별 memset 최적화 사용 */
	}
}

/*
 * [한국어] axmap_free - axmap 과 모든 레벨 메모리 해제
 *
 * @axmap: 해제 대상. NULL 허용 (no-op).
 *
 * 호출 시점: 잡 종료 단계 (td 해제 루틴).
 */
void axmap_free(struct axmap *axmap)
{
	unsigned int i;

	if (!axmap)
		return;						/* [한국어] NULL 방어 (free(NULL) 안전이지만 상위 nr_levels 접근 방지) */

	for (i = 0; i < axmap->nr_levels; i++)
		free(axmap->levels[i].map);			/* [한국어] 각 레벨의 map 배열 해제 */

	free(axmap->levels);					/* [한국어] 레벨 배열 자체 해제 */
	free(axmap);						/* [한국어] 루트 구조체 해제 */
}

/*
 * [한국어] axmap_new - nr_bits 를 커버하는 다단계 계층 비트맵 생성
 *
 * @nr_bits: 추적할 총 블록(비트) 수
 * @return: 생성된 axmap 포인터, 실패 시 NULL (모든 부분 할당 롤백)
 *
 * 레벨 수 계산: ceil(nr_bits / 64) → ceil(.../64) → ... → 1 도달까지 반복.
 * 각 레벨의 map_size 는 상위로 올라갈수록 1/64 씩 줄어든다.
 * 초기화 후 axmap_reset 으로 전 비트 0 보장.
 *
 * 에러 경로: malloc 실패 시 free_levels 라벨로 점프하여 부분 할당 해제.
 * 호출 체인: init.c → [axmap_new].
 */
/* Allocate memory for a set that can store the numbers 0 .. @nr_bits - 1. */
struct axmap *axmap_new(uint64_t nr_bits)
{
	struct axmap *axmap;
	unsigned int i, levels;

	axmap = malloc(sizeof(*axmap));
	if (!axmap)
		return NULL;

	/* [한국어] 필요한 레벨 수 계산: 매 레벨 크기를 1/64 로 줄여 1 도달까지 */
	levels = 1;
	i = (nr_bits + BLOCKS_PER_UNIT - 1) >> UNIT_SHIFT;	/* [한국어] 레벨 1 의 워드 수 = ceil(nr_bits/64) */
	while (i > 1) {
		i = (i + BLOCKS_PER_UNIT - 1) >> UNIT_SHIFT;	/* [한국어] 매 반복마다 다음 상위 레벨 크기 */
		levels++;
	}

	axmap->nr_levels = levels;				/* [한국어] 레벨 수 저장 */
	axmap->levels = calloc(axmap->nr_levels, sizeof(struct axmap_level));	/* [한국어] 레벨 배열(0 으로 초기화) */
	if (!axmap->levels)
		goto free_axmap;
	axmap->nr_bits = nr_bits;				/* [한국어] 총 비트 수 저장 */

	for (i = 0; i < axmap->nr_levels; i++) {		/* [한국어] 각 레벨의 map 배열 할당 */
		struct axmap_level *al = &axmap->levels[i];

		nr_bits = (nr_bits + BLOCKS_PER_UNIT - 1) >> UNIT_SHIFT;	/* [한국어] 이 레벨의 워드 수 = ceil(이전 레벨 비트 수 / 64) */

		al->level = i;
		al->map_size = nr_bits;
		al->map = malloc(al->map_size * sizeof(unsigned long));	/* [한국어] 레벨의 실제 bit 배열 */
		if (!al->map)
			goto free_levels;

	}

	axmap_reset(axmap);					/* [한국어] 전체를 0 으로 깨끗이 리셋 */
	return axmap;

free_levels:
	for (i = 0; i < axmap->nr_levels; i++)			/* [한국어] 부분 할당된 map 들 해제 */
		free(axmap->levels[i].map);

	free(axmap->levels);					/* [한국어] 레벨 배열 해제 */

free_axmap:
	free(axmap);						/* [한국어] 루트 해제 */
	return NULL;
}

/*
 * Call @func for each level, starting at level zero, until a level is found
 * for which @func returns true. Return false if none of the @func calls
 * returns true.
 */
/*
 * [한국어] axmap_handler - 레벨 0 부터 상위로 콜백을 전파 (bottom-up)
 *
 * @axmap: 대상 axmap
 * @bit_nr: 작용할 비트 번호 (블록 번호)
 * @func: 각 레벨에서 호출될 콜백. true 반환 시 상위 전파 중단.
 * @data: 콜백에 전달할 사용자 데이터 (axmap_set_data 등)
 * @return: 어느 레벨에서든 true 가 반환되면 true, 전부 false 면 false.
 *
 * 용도: axmap_set 에서 사용. 레벨 0 에서 비트 설정 → 워드가 모두 1 이 되면
 * 상위로 한 비트 전파 → 또 모두 1 이면 더 상위로... 하는 패턴.
 *
 * 반환 true 의 의미는 콜백마다 다름 (set_fn 은 "워드가 아직 모두 1 이 아님" 을
 * true 로 돌려 상위 전파를 중단 — 즉 "상위는 갱신 불필요").
 */
static bool axmap_handler(struct axmap *axmap, uint64_t bit_nr,
			  bool (*func)(struct axmap_level *, uint64_t, unsigned int,
			  void *), void *data)
{
	struct axmap_level *al;
	uint64_t index = bit_nr;				/* [한국어] 레벨이 올라갈수록 UNIT_SHIFT 만큼 축소됨 */
	int i;

	for (i = 0; i < axmap->nr_levels; i++) {
		unsigned long offset = index >> UNIT_SHIFT;	/* [한국어] 현 레벨의 워드 오프셋 */
		unsigned int bit = index & BLOCKS_PER_UNIT_MASK;	/* [한국어] 워드 내 비트 위치 */

		al = &axmap->levels[i];

		if (func(al, offset, bit, data))
			return true;				/* [한국어] 콜백이 "더 이상 상위 전파 불필요" 라고 보고 */

		if (index)
			index >>= UNIT_SHIFT;			/* [한국어] 한 레벨 위로: 비트 번호를 /64 */
	}

	return false;
}

/*
 * Call @func for each level, starting at the highest level, until a level is
 * found for which @func returns true. Return false if none of the @func calls
 * returns true.
 */
/*
 * [한국어] axmap_handler_topdown - 최상위 레벨부터 하향으로 콜백 호출 (top-down)
 *
 * @axmap: 대상 axmap
 * @bit_nr: 작용할 비트 번호
 * @func: 각 레벨에서 호출될 콜백. true 반환 시 즉시 종료.
 * @return: 어느 레벨에서든 true 반환 시 true, 아니면 false.
 *
 * 용도: axmap_isset 에서 사용. 최상위 요약 비트가 0 이면 하위 전체도 0 이므로
 * 즉시 false 반환 가능 (조기 종료로 캐시 미스 감소).
 */
static bool axmap_handler_topdown(struct axmap *axmap, uint64_t bit_nr,
	bool (*func)(struct axmap_level *, uint64_t, unsigned int, void *))
{
	int i;

	for (i = axmap->nr_levels - 1; i >= 0; i--) {
		uint64_t index = bit_nr >> (UNIT_SHIFT * i);		/* [한국어] 각 레벨의 인덱스 = bit_nr >> (6*i) */
		unsigned long offset = index >> UNIT_SHIFT;		/* [한국어] 워드 오프셋 */
		unsigned int bit = index & BLOCKS_PER_UNIT_MASK;	/* [한국어] 비트 위치 */

		if (func(&axmap->levels[i], offset, bit, NULL))
			return true;					/* [한국어] 레벨에서 긍정 → 즉시 반환 */
	}

	return false;
}

/*
 * [한국어] struct axmap_set_data - axmap_set_fn 에 전달되는 입출력 컨텍스트
 */
struct axmap_set_data {
	unsigned int nr_bits;
	/* [한국어] [입력] 이번 호출에서 설정하려는 비트 수 (한 워드 경계 내).
	 * axmap_set 은 1, axmap_set_nr 은 1..BLOCKS_PER_UNIT 범위로 지정. */

	unsigned int set_bits;
	/* [한국어] [출력] 실제로 이번 호출이 새로 세팅한 비트 수. 이미 1 이었던 부분은 제외.
	 * 레벨 0 에서만 유효. axmap_set_nr 의 반환값 누적에 사용. */
};

/*
 * Set at most @__data->nr_bits bits in @al at offset @offset. Do not exceed
 * the boundary of the element at offset @offset. Return the number of bits
 * that have been set in @__data->set_bits if @al->level == 0.
 */
/*
 * [한국어] axmap_set_fn - 한 레벨에서 "bit 위치부터 연속 nr_bits 비트 세팅" 콜백
 *
 * @al: 현재 레벨 (axmap_handler 가 각 레벨을 순회하며 호출)
 * @offset: 워드 오프셋 (al->map 내)
 * @bit: 워드 내 시작 비트 (0..63)
 * @__data: struct axmap_set_data (in/out)
 * @return: true = 이 워드가 아직 모두 1 이 아님 → 상위 전파 불필요 (handler 종료)
 *          false = 이 워드가 완전히 1 로 채워짐 → 상위 레벨도 업데이트해야 함
 *
 * 핵심: 한 워드 경계를 넘지 않는다. 경계 초과는 호출자(axmap_set_nr)가 분할하여 해결.
 * overlap 처리: 이미 일부 비트가 1 이면 "연속 구간이 끊긴다" 는 규약에 따라 nr_bits 를
 *              축소하여 first overlap 직전까지만 세팅하고 false 반환(부분 성공).
 */
static bool axmap_set_fn(struct axmap_level *al, uint64_t offset,
			 unsigned int bit, void *__data)
{
	struct axmap_set_data *data = __data;
	unsigned long mask, overlap;
	unsigned int nr_bits;

	nr_bits = min(data->nr_bits, BLOCKS_PER_UNIT - bit);	/* [한국어] 워드 경계 초과 방지: (64 - bit) 로 상한 */

	mask = bit_masks[nr_bits] << bit;			/* [한국어] 세팅할 비트 마스크: 하위 nr_bits 가 1 인 마스크를 bit 만큼 시프트 */

	/*
	 * Mask off any potential overlap, only sets contig regions
	 */
	overlap = al->map[offset] & mask;			/* [한국어] 현재 워드에서 세팅 영역과 겹치는 비트들 */
	if (overlap == mask) {					/* [한국어] 완전 중첩 — 이미 모두 1. 새로 설정된 비트 없음 */
		data->set_bits = 0;
		return true;					/* [한국어] 워드가 "모두 1" 이 아닐 수 있으므로 상위 전파 불필요 반환 */
	}

	if (overlap) {						/* [한국어] 부분 overlap — 연속 구간 깨짐 */
		nr_bits = ffz(~overlap) - bit;			/* [한국어] ~overlap 의 첫 0 비트 위치 - bit = 연속 가능 길이 (즉, 첫 1 직전까지) */
		if (!nr_bits)
			return true;				/* [한국어] bit 위치가 이미 1 이면 아무것도 못 설정 */
		mask = bit_masks[nr_bits] << bit;		/* [한국어] 축소된 nr_bits 기반으로 마스크 재계산 */
	}

	assert(mask);
	assert(!(al->map[offset] & mask));			/* [한국어] 이 시점의 mask 는 겹침 없음이 보장되어야 함 */
	al->map[offset] |= mask;				/* [한국어] 실제 비트 세팅 — 핵심 쓰기 연산 */

	if (!al->level)
		data->set_bits = nr_bits;			/* [한국어] 레벨 0 만 실제 블록 수 카운트 (상위는 요약 비트라 카운트 무의미) */

	/* For the next level */
	data->nr_bits = 1;					/* [한국어] 상위 레벨 전파 시 비트 1 개만 설정 (상위 비트 하나가 하위 워드 전체 요약) */

	return al->map[offset] != -1UL;				/* [한국어] 워드가 전부 1 이 되지 않았으면 상위 전파 종료(true).
								 * 전부 1 (= -1UL 즉 0xFFFF...) 이면 false 반환 → 상위로 전파하여 요약 비트 세움 */
}

/*
 * Set up to @data->nr_bits starting from @bit_nr in @axmap. Start at
 * @bit_nr. If that bit has not yet been set then set it and continue until
 * either @data->nr_bits have been set or a 1 bit is found. It is guaranteed
 * that all bits that have been requested to set fit in the same unsigned long
 * word of level 0 of @axmap.
 */
/*
 * [한국어] __axmap_set - 한 워드 경계 내에서 nr_bits 세팅하는 내부 함수
 *
 * axmap 경계와 워드 경계를 검사하여 안전한 범위로 좁힌 뒤 axmap_handler 에 위임.
 */
static void __axmap_set(struct axmap *axmap, uint64_t bit_nr,
			 struct axmap_set_data *data)
{
	unsigned int nr_bits = data->nr_bits;

	if (bit_nr > axmap->nr_bits)				/* [한국어] 전체 영역 초과 — 무시 (의도치 않은 호출 방어) */
		return;
	else if (bit_nr + nr_bits > axmap->nr_bits)
		nr_bits = axmap->nr_bits - bit_nr;		/* [한국어] 끝부분 초과 방지: 상한 클램프 */

	assert(nr_bits <= BLOCKS_PER_UNIT);			/* [한국어] 한 워드 경계 내 보장 */

	axmap_handler(axmap, bit_nr, axmap_set_fn, data);	/* [한국어] bottom-up 전파로 세팅 + 상위 요약 업데이트 */
}

/*
 * [한국어] axmap_set - 단일 비트를 설정 (블록을 "사용됨" 으로 표시)
 *
 * @axmap: 비트맵
 * @bit_nr: 설정할 비트 번호 (블록 번호)
 *
 * 실행 컨텍스트: 잡 스레드. 단일 스레드 접근.
 * 호출 체인: io_u.c (mark_random_map 등) → [axmap_set] → __axmap_set → axmap_handler.
 */
void axmap_set(struct axmap *axmap, uint64_t bit_nr)
{
	struct axmap_set_data data = { .nr_bits = 1, };		/* [한국어] 한 번에 1 비트만 설정 */

	__axmap_set(axmap, bit_nr, &data);
}

/*
 * Set up to @nr_bits starting from @bit in @axmap. Start at @bit. If that
 * bit has not yet been set then set it and continue until either @nr_bits
 * have been set or a 1 bit is found. Return the number of bits that have been
 * set.
 */
/*
 * [한국어] axmap_set_nr - 연속된 nr_bits 를 설정 (워드 경계 넘어서도 가능)
 *
 * @axmap: 비트맵
 * @bit_nr: 시작 비트
 * @nr_bits: 설정하려는 비트 수
 * @return: 실제로 "새로" 설정된 비트 수 (기존 1 인 비트 만나면 조기 종료)
 *
 * 구현: 워드 경계를 넘지 않도록 max_bits = BLOCKS_PER_UNIT - (bit_nr & MASK) 로 쪼개어
 *        __axmap_set 반복 호출. 기존 1 비트(연속 끊김) 만나면 즉시 중단.
 *
 * 사용처: io_u.c 가 순차 쓰기/verify 시 연속 블록 범위를 효율적으로 표시.
 */
unsigned int axmap_set_nr(struct axmap *axmap, uint64_t bit_nr,
			  unsigned int nr_bits)
{
	unsigned int set_bits = 0;

	do {
		struct axmap_set_data data = { .nr_bits = nr_bits, };
		unsigned int max_bits, this_set;

		max_bits = BLOCKS_PER_UNIT - (bit_nr & BLOCKS_PER_UNIT_MASK);	/* [한국어] 현재 워드에서 남은 비트 수 */
		if (nr_bits > max_bits)
			data.nr_bits = max_bits;			/* [한국어] 워드 경계 내로 제한 */

		this_set = data.nr_bits;				/* [한국어] 이번 청크의 요청 비트 수 보관 */
		__axmap_set(axmap, bit_nr, &data);			/* [한국어] 실제 세팅 (data.set_bits 에 결과 기록) */
		set_bits += data.set_bits;				/* [한국어] 누적 */
		if (data.set_bits != this_set)
			break;						/* [한국어] 요청량보다 적게 세팅됨 → 연속 끊김 → 종료 */

		nr_bits -= data.set_bits;				/* [한국어] 남은 요청량 감소 */
		bit_nr += data.set_bits;				/* [한국어] 시작 비트 전진 */
	} while (nr_bits);

	return set_bits;						/* [한국어] 총 새로 세팅된 비트 수 */
}

/*
 * [한국어] axmap_isset_fn - 한 레벨에서 "bit 위치가 1 인가?" 검사 콜백 (topdown 용)
 *
 * @al: 현재 레벨
 * @offset: 워드 오프셋
 * @bit: 워드 내 비트
 * @unused: 미사용
 * @return: 비트가 1 이면 true → topdown handler 는 즉시 반환
 *
 * 왜 topdown 으로 쓰는가: 상위 레벨 요약이 이미 1 이면 조기 종료 가능(충만 요약).
 * 단, 주의 — 현 구현은 "상위 비트가 1 이면 하위도 1" 이 아니라 "상위 비트가 1 이면
 * 하위 워드 전체가 1" 을 의미한다. 따라서 상위에서 true 가 나면 반드시 하위도 true.
 * 상위에서 false 가 나와도 하위가 1 일 수 있으므로 계속 하향. 최종 레벨 0 에서 판정.
 */
static bool axmap_isset_fn(struct axmap_level *al, uint64_t offset,
			   unsigned int bit, void *unused)
{
	return (al->map[offset] & (1ULL << bit)) != 0;		/* [한국어] 해당 비트 검사 */
}

/*
 * [한국어] axmap_isset - 특정 비트(블록) 가 이미 사용됨으로 표시되었는지 확인
 *
 * @axmap: 비트맵
 * @bit_nr: 확인할 비트
 * @return: 사용됨이면 true
 *
 * 최상위부터 하향 탐색. 상위가 1 (= 하위 64비트 전부 1) 이면 즉시 true.
 * 실행 컨텍스트: 잡 스레드. 단일 스레드 접근.
 */
bool axmap_isset(struct axmap *axmap, uint64_t bit_nr)
{
	if (bit_nr <= axmap->nr_bits)				/* [한국어] 경계 내부만 검사 */
		return axmap_handler_topdown(axmap, bit_nr, axmap_isset_fn);

	return false;						/* [한국어] 범위 초과는 사용된 적 없다고 간주 */
}

/*
 * Find the first free bit that is at least as large as bit_nr.  Return
 * -1 if no free bit is found before the end of the map.
 */
/*
 * [한국어] axmap_find_first_free - bit_nr 이상의 첫 미설정 비트를 찾음
 *
 * @axmap: 비트맵
 * @bit_nr: 탐색 시작 비트
 * @return: 빈 비트의 번호, 없으면 -1ULL
 *
 * 최상위 레벨부터 하향으로 내려가며:
 *   1. 해당 워드에서 bit 이상 첫 0 비트 탐색 (`~bit_masks[bit] & ~map[offset]`).
 *   2. 없으면 그 다음 워드부터 0 이 있는 워드 탐색.
 *   3. 발견하면 다음 레벨로 index 확장 후 반복.
 *   4. 최종 레벨 0 에서 찾은 index 가 nr_bits 초과면 -1 반환.
 */
static uint64_t axmap_find_first_free(struct axmap *axmap, uint64_t bit_nr)
{
	int i;
	unsigned long temp;
	unsigned int bit;
	uint64_t offset, base_index, index;
	struct axmap_level *al;

	index = 0;
	for (i = axmap->nr_levels - 1; i >= 0; i--) {		/* [한국어] 최상위부터 */
		al = &axmap->levels[i];

		/* Shift previously calculated index for next level */
		index <<= UNIT_SHIFT;				/* [한국어] 이전 레벨의 index 를 하위 레벨 크기에 맞게 64배 확장 */

		/*
		 * Start from an index that's at least as large as the
		 * originally passed in bit number.
		 */
		base_index = bit_nr >> (UNIT_SHIFT * i);	/* [한국어] 현 레벨에서의 bit_nr 대응 인덱스 */
		if (index < base_index)
			index = base_index;			/* [한국어] 보정: bit_nr 이후만 탐색 */

		/* Get the offset and bit for this level */
		offset = index >> UNIT_SHIFT;
		bit = index & BLOCKS_PER_UNIT_MASK;

		/*
		 * If the previous level had unused bits in its last
		 * word, the offset could be bigger than the map at
		 * this level. That means no free bits exist before the
		 * end of the map, so return -1.
		 */
		if (offset >= al->map_size)
			return -1ULL;				/* [한국어] 맵 끝을 넘어섬 → 없음 */

		/* Check the first word starting with the specific bit */
		temp = ~bit_masks[bit] & ~al->map[offset];	/* [한국어] 현 워드에서 "bit 이상의 0 비트" 마스크 */
		if (temp)
			goto found;				/* [한국어] 찾음 → 다음 레벨로 */

		/*
		 * No free bit in the first word, so iterate
		 * looking for a word with one or more free bits.
		 */
		for (offset++; offset < al->map_size; offset++) {
			temp = ~al->map[offset];		/* [한국어] 워드 전체의 0 비트 마스크 */
			if (temp)
				goto found;
		}

		/* Did not find a free bit */
		return -1ULL;

found:
		/* Compute the index of the free bit just found */
		index = (offset << UNIT_SHIFT) + ffz(~temp);	/* [한국어] 워드 오프셋 * 64 + 첫 0 비트 위치 */
	}

	/* If found an unused bit in the last word of level 0, return -1 */
	if (index >= axmap->nr_bits)
		return -1ULL;					/* [한국어] 마지막 워드의 잔여 비트는 실제 블록이 아님 */

	return index;
}

/*
 * 'bit_nr' is already set. Find the next free bit after this one.
 * Return -1 if no free bits found.
 */
/*
 * [한국어] axmap_next_free - bit_nr 다음의 미설정 비트를 찾아 반환
 *
 * @axmap: 비트맵
 * @bit_nr: 현재 비트 (이미 설정된 것으로 간주)
 * @return: 다음 미설정 비트, 없으면 -1ULL
 *
 * 최적화 경로:
 *   1. next_bit = bit_nr + 1, 맵 끝이면 0 으로 wrap.
 *   2. 레벨 0 의 현재 워드에서 O(1) 로 빠른 탐색 (`~bit_masks[bit] & ~map[offset]`).
 *   3. 없으면 axmap_find_first_free 로 topdown 탐색.
 *   4. 여전히 없고 next_bit != 0 이면 0 부터 wrap-around 재탐색.
 *
 * 호출 체인: io_u.c (get_next_rand_offset 등) → [axmap_next_free].
 */
uint64_t axmap_next_free(struct axmap *axmap, uint64_t bit_nr)
{
	uint64_t ret;
	uint64_t next_bit = bit_nr + 1;
	unsigned long temp;
	uint64_t offset;
	unsigned int bit;

	if (bit_nr >= axmap->nr_bits)
		return -1ULL;					/* [한국어] 현재 비트가 이미 범위 밖 */

	/* If at the end of the map, wrap-around */
	if (next_bit == axmap->nr_bits)
		next_bit = 0;					/* [한국어] 끝이면 앞으로 돌림 */

	offset = next_bit >> UNIT_SHIFT;
	bit = next_bit & BLOCKS_PER_UNIT_MASK;

	/*
	 * As an optimization, do a quick check for a free bit
	 * in the current word at level 0. If not found, do
	 * a topdown search.
	 */
	temp = ~bit_masks[bit] & ~axmap->levels[0].map[offset];	/* [한국어] 레벨 0 의 현재 워드에서 "bit 이상 0 비트" 마스크 */
	if (temp) {
		ret = (offset << UNIT_SHIFT) + ffz(~temp);	/* [한국어] 워드 내 첫 0 비트 인덱스 */

		/* Might have found an unused bit at level 0 */
		if (ret >= axmap->nr_bits)
			ret = -1ULL;				/* [한국어] 마지막 워드 잔여(실제 블록 아님) 필터 */
	} else
		ret = axmap_find_first_free(axmap, next_bit);	/* [한국어] 현 워드 탐색 실패 → topdown */

	/*
	 * If there are no free bits starting at next_bit and going
	 * to the end of the map, wrap around by searching again
	 * starting at bit 0.
	 */
	if (ret == -1ULL && next_bit != 0)
		ret = axmap_find_first_free(axmap, 0);		/* [한국어] wrap-around: 앞에서 다시 탐색 */
	return ret;
}
