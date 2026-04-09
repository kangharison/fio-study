/*
 * simple memory allocator, backed by mmap() so that it hands out memory
 * that can be shared across processes and threads
 */
/*
 * [한국어] smalloc.c - 공유 메모리 할당기 (mmap 기반)
 *
 * 이 파일은 mmap()을 사용하여 프로세스/스레드 간 공유 가능한 메모리를 할당하는
 * 간단한 메모리 할당기를 구현한다.
 *
 * 주요 구조:
 *   - pool: mmap으로 확보한 메모리 영역. 비트맵으로 블록 사용 여부를 관리
 *   - block_hdr: 각 할당 블록의 헤더 (크기 + 선택적 레드존)
 *   - bitmap: 각 비트가 SMALLOC_BPB(32)바이트 블록 하나에 대응
 *
 * 할당 흐름:
 *   smalloc() -> smalloc_pool() -> __smalloc_pool()
 *     비트맵에서 연속된 빈 블록을 찾아 할당하고, 블록 헤더를 설정
 *
 * 해제 흐름:
 *   sfree() -> sfree_pool()
 *     포인터가 속한 풀을 찾아 비트맵의 해당 비트를 클리어
 *
 * 레드존(SMALLOC_REDZONE):
 *   메모리 corruption 탐지를 위해 블록 앞뒤에 매직 값을 기록/검증
 */

/* 시스템 헤더 */
#include <sys/mman.h>    /* mmap, munmap - 메모리 매핑 */
#include <assert.h>      /* assert - 디버그 단언 */
#include <string.h>      /* memset, strlen, strcpy */

/* fio 내부 헤더 */
#include "fio.h"         /* fio 핵심 구조체 및 매크로 */
#include "fio_sem.h"     /* 프로세스 간 공유 세마포어 */
#include "os/os.h"       /* OS 추상화 (OS_MAP_ANON 등) */
#include "smalloc.h"     /* 공유 메모리 할당기 API */
#include "log.h"         /* 로깅 함수 (log_err 등) */

/* [한국어] 매크로 정의 - 메모리 할당기 설정 상수 */
#define SMALLOC_REDZONE		/* define to detect memory corruption */ /* 메모리 손상 감지용 레드존 활성화 */

#define SMALLOC_BPB	32	/* block size, bytes-per-bit in bitmap */ /* 비트맵 1비트당 바이트 수 (블록 크기) */
#define SMALLOC_BPI	(sizeof(unsigned int) * 8)  /* 비트맵 워드(unsigned int) 하나의 비트 수 */
#define SMALLOC_BPL	(SMALLOC_BPB * SMALLOC_BPI) /* 비트맵 워드 하나가 관리하는 바이트 수 (32 * 32 = 1024) */

#define INITIAL_SIZE	16*1024*1024	/* new pool size */ /* 새 풀의 기본 크기: 16MB */
#define INITIAL_POOLS	8		/* maximum number of pools to setup */ /* 초기에 생성할 풀 개수 */

#define MAX_POOLS	16  /* 최대 풀 개수 */

#define SMALLOC_PRE_RED		0xdeadbeefU  /* 블록 앞쪽 레드존 매직 값 */
#define SMALLOC_POST_RED	0x5aa55aa5U  /* 블록 뒤쪽 레드존 매직 값 */

unsigned int smalloc_pool_size = INITIAL_SIZE; /* 풀 크기 (--alloc-size 옵션으로 변경 가능) */
#ifdef SMALLOC_REDZONE
static const int int_mask = sizeof(int) - 1;  /* int 정렬 마스크 (레드존 포인터 정렬에 사용) */
#endif

/* [한국어] 메모리 풀 구조체 - mmap으로 확보한 하나의 메모리 영역을 관리 */
struct pool {
	struct fio_sem *lock;			/* protects this pool */ /* 풀 접근 보호용 세마포어 */
	void *map;				/* map of blocks */ /* mmap된 메모리 시작 주소 */
	unsigned int *bitmap;			/* blocks free/busy map */ /* 블록 사용 여부 비트맵 */
	size_t free_blocks;		/* free blocks */ /* 사용 가능한 블록 수 */
	size_t nr_blocks;			/* total blocks */ /* 전체 비트맵 워드 수 */
	size_t next_non_full;  /* 다음 빈 공간 탐색 시작 인덱스 (검색 최적화) */
	size_t mmap_size;      /* mmap된 전체 크기 (데이터 + 비트맵) */
};

/* [한국어] 블록 헤더 - 각 할당된 블록 앞에 위치하여 크기 및 레드존 정보 저장 */
struct block_hdr {
	size_t size;           /* 할당된 블록 전체 크기 (헤더 + 데이터 + 레드존 포함) */
#ifdef SMALLOC_REDZONE
	unsigned int prered;   /* 앞쪽 레드존 값 (SMALLOC_PRE_RED이어야 함) */
#endif
};

/*
 * This suppresses the voluminous potential bitmap printout when
 * smalloc encounters an OOM error
 */
/* [한국어] OOM 발생 시 비트맵 출력을 억제하는 디버그 플래그 */
static const bool enable_smalloc_debug = false;

/* [한국어] 전역 변수 - 풀 관리 */
static struct pool *mp;            /* 풀 배열 포인터 (mmap으로 할당, 프로세스 간 공유) */
static unsigned int nr_pools;      /* 현재 생성된 풀 개수 */
static unsigned int last_pool;     /* 마지막으로 할당 성공한 풀 인덱스 (캐시 역할) */

/* [한국어] 포인터가 해당 풀의 유효 범위 내에 있는지 검사 */
static inline int ptr_valid(struct pool *pool, void *ptr)
{
	unsigned int pool_size = pool->nr_blocks * SMALLOC_BPL;

	return (ptr >= pool->map) && (ptr < pool->map + pool_size);
}

/* [한국어] 바이트 크기를 블록 수로 변환 (올림 나눗셈) */
static inline size_t size_to_blocks(size_t size)
{
	return (size + SMALLOC_BPB - 1) / SMALLOC_BPB;
}

/* [한국어] 비트맵의 연속 블록에 대해 함수(func)를 반복 적용하는 범용 반복자
 *   pool_idx: 비트맵 워드 인덱스, idx: 워드 내 비트 오프셋
 *   func: mask_cmp(빈 블록 확인), mask_set(할당 표시), mask_clear(해제 표시) 중 하나 */
static int blocks_iter(struct pool *pool, unsigned int pool_idx,
		       unsigned int idx, size_t nr_blocks,
		       int (*func)(unsigned int *map, unsigned int mask))
{

	while (nr_blocks) {
		unsigned int this_blocks, mask;
		unsigned int *map;

		if (pool_idx >= pool->nr_blocks)
			return 0;

		map = &pool->bitmap[pool_idx];

		this_blocks = nr_blocks;
		if (this_blocks + idx > SMALLOC_BPI) {
			this_blocks = SMALLOC_BPI - idx;
			idx = SMALLOC_BPI - this_blocks;
		}

		if (this_blocks == SMALLOC_BPI)
			mask = -1U;
		else
			mask = ((1U << this_blocks) - 1) << idx;

		if (!func(map, mask))
			return 0;

		nr_blocks -= this_blocks;
		idx = 0;
		pool_idx++;
	}

	return 1;
}

/* [한국어] 비트맵 워드에서 mask에 해당하는 비트가 모두 0(빈 상태)인지 검사 */
static int mask_cmp(unsigned int *map, unsigned int mask)
{
	return !(*map & mask);
}

/* [한국어] 비트맵 워드에서 mask에 해당하는 비트를 클리어(블록 해제) */
static int mask_clear(unsigned int *map, unsigned int mask)
{
	assert((*map & mask) == mask);
	*map &= ~mask;
	return 1;
}

/* [한국어] 비트맵 워드에서 mask에 해당하는 비트를 설정(블록 할당) */
static int mask_set(unsigned int *map, unsigned int mask)
{
	assert(!(*map & mask));
	*map |= mask;
	return 1;
}

/* [한국어] 지정된 위치부터 nr_blocks개의 블록이 모두 비어있는지 확인 */
static int blocks_free(struct pool *pool, unsigned int pool_idx,
		       unsigned int idx, size_t nr_blocks)
{
	return blocks_iter(pool, pool_idx, idx, nr_blocks, mask_cmp);
}

/* [한국어] 지정된 위치부터 nr_blocks개의 블록을 사용 중으로 표시 */
static void set_blocks(struct pool *pool, unsigned int pool_idx,
		       unsigned int idx, size_t nr_blocks)
{
	blocks_iter(pool, pool_idx, idx, nr_blocks, mask_set);
}

/* [한국어] 지정된 위치부터 nr_blocks개의 블록을 해제(비트 클리어) */
static void clear_blocks(struct pool *pool, unsigned int pool_idx,
			 unsigned int idx, size_t nr_blocks)
{
	blocks_iter(pool, pool_idx, idx, nr_blocks, mask_clear);
}

/* [한국어] 비트맵 워드에서 start 위치부터 다음 0비트(빈 블록)의 인덱스를 찾음 */
static int find_next_zero(int word, int start)
{
	assert(word != -1U);
	word >>= start;
	return ffz(word) + start;
}

/* [한국어] 새로운 메모리 풀을 추가 — mmap으로 메모리를 확보하고 비트맵과 세마포어를 초기화 */
static bool add_pool(struct pool *pool, unsigned int alloc_size)
{
	int bitmap_blocks;
	int mmap_flags;
	void *ptr;

	if (nr_pools == MAX_POOLS)
		return false;

#ifdef SMALLOC_REDZONE
	alloc_size += sizeof(unsigned int);
#endif
	alloc_size += sizeof(struct block_hdr);
	if (alloc_size < INITIAL_SIZE)
		alloc_size = INITIAL_SIZE;

	/* round up to nearest full number of blocks */ /* 블록 경계로 올림 정렬 */
	alloc_size = (alloc_size + SMALLOC_BPL - 1) & ~(SMALLOC_BPL - 1);
	bitmap_blocks = alloc_size / SMALLOC_BPL;
	alloc_size += bitmap_blocks * sizeof(unsigned int);
	pool->mmap_size = alloc_size;

	pool->nr_blocks = bitmap_blocks;
	pool->free_blocks = bitmap_blocks * SMALLOC_BPI;

	mmap_flags = OS_MAP_ANON;
#ifdef CONFIG_ESX
	mmap_flags |= MAP_PRIVATE;
#else
	mmap_flags |= MAP_SHARED;
#endif
	ptr = mmap(NULL, alloc_size, PROT_READ|PROT_WRITE, mmap_flags, -1, 0);

	if (ptr == MAP_FAILED)
		goto out_fail;

	pool->map = ptr;
	pool->bitmap = (unsigned int *)((char *) ptr + (pool->nr_blocks * SMALLOC_BPL));
	memset(pool->bitmap, 0, bitmap_blocks * sizeof(unsigned int));

	pool->lock = fio_sem_init(FIO_SEM_UNLOCKED);
	if (!pool->lock)
		goto out_fail;

	nr_pools++;
	return true;
out_fail:
	log_err("smalloc: failed adding pool\n");
	if (pool->map)
		munmap(pool->map, pool->mmap_size);
	return false;
}

/* [한국어] 공유 메모리 할당기 초기화 — 풀 배열을 mmap하고 초기 풀들을 생성
 *   --alloc-size 옵션으로 sinit()이 여러 번 호출될 수 있으므로,
 *   풀 배열(mp)은 최초 1회만 할당한다. */
void sinit(void)
{
	bool ret;
	int i;

	/*
	 * sinit() can be called more than once if alloc-size is
	 * set. But we want to allocate space for the struct pool
	 * instances only once.
	 */
	if (!mp) {
		mp = (struct pool *) mmap(NULL,
			MAX_POOLS * sizeof(struct pool),
			PROT_READ | PROT_WRITE,
			OS_MAP_ANON | MAP_SHARED, -1, 0);

		assert(mp != MAP_FAILED);
	}

	for (i = 0; i < INITIAL_POOLS; i++) {
		ret = add_pool(&mp[nr_pools], smalloc_pool_size);
		if (!ret)
			break;
	}

	/*
	 * If we added at least one pool, we should be OK for most
	 * cases.
	 */
	assert(i);
}

/* [한국어] 단일 풀 정리 — mmap 해제 및 세마포어 제거 */
static void cleanup_pool(struct pool *pool)
{
	/*
	 * This will also remove the temporary file we used as a backing
	 * store, it was already unlinked
	 */
	munmap(pool->map, pool->mmap_size);

	if (pool->lock)
		fio_sem_remove(pool->lock);
}

/* [한국어] 공유 메모리 할당기 전체 정리 — 모든 풀을 해제하고 풀 배열도 munmap */
void scleanup(void)
{
	unsigned int i;

	for (i = 0; i < nr_pools; i++)
		cleanup_pool(&mp[i]);

	munmap(mp, MAX_POOLS * sizeof(struct pool));
}

/* [한국어] 레드존 관련 함수들 (SMALLOC_REDZONE 활성화 시) */
#ifdef SMALLOC_REDZONE
/* [한국어] 블록 뒤쪽 레드존 포인터를 계산 (int 정렬 보장) */
static void *postred_ptr(struct block_hdr *hdr)
{
	uintptr_t ptr;

	ptr = (uintptr_t) hdr + hdr->size - sizeof(unsigned int);
	ptr = (uintptr_t) PTR_ALIGN(ptr, int_mask);

	return (void *) ptr;
}

/* [한국어] 블록 앞뒤에 레드존 매직 값을 기록 */
static void fill_redzone(struct block_hdr *hdr)
{
	unsigned int *postred = postred_ptr(hdr);

	hdr->prered = SMALLOC_PRE_RED;
	*postred = SMALLOC_POST_RED;
}

/* [한국어] 해제 시 레드존이 파괴되지 않았는지 검증 — 메모리 corruption 감지 */
static void sfree_check_redzone(struct block_hdr *hdr)
{
	unsigned int *postred = postred_ptr(hdr);

	if (hdr->prered != SMALLOC_PRE_RED) {
		log_err("smalloc pre redzone destroyed!\n"
			" ptr=%p, prered=%x, expected %x\n",
				hdr+1, hdr->prered, SMALLOC_PRE_RED);
		assert(0);
	}
	if (*postred != SMALLOC_POST_RED) {
		log_err("smalloc post redzone destroyed!\n"
			"  ptr=%p, postred=%x, expected %x\n",
				hdr+1, *postred, SMALLOC_POST_RED);
		assert(0);
	}
}
#else
static void fill_redzone(struct block_hdr *hdr)
{
}

static void sfree_check_redzone(struct block_hdr *hdr)
{
}
#endif

/* [한국어] 특정 풀에서 메모리 블록을 해제 — 레드존 검사 후 비트맵 클리어 */
static void sfree_pool(struct pool *pool, void *ptr)
{
	struct block_hdr *hdr;
	unsigned int i, idx;
	unsigned long offset;

	if (!ptr)
		return;

	ptr -= sizeof(*hdr);
	hdr = ptr;

	assert(ptr_valid(pool, ptr));

	sfree_check_redzone(hdr);

	offset = ptr - pool->map;
	i = offset / SMALLOC_BPL;
	idx = (offset % SMALLOC_BPL) / SMALLOC_BPB;

	fio_sem_down(pool->lock);
	clear_blocks(pool, i, idx, size_to_blocks(hdr->size));
	if (i < pool->next_non_full)
		pool->next_non_full = i;
	pool->free_blocks += size_to_blocks(hdr->size);
	fio_sem_up(pool->lock);
}

/* [한국어] 공유 메모리 해제 — 포인터가 속한 풀을 찾아 sfree_pool()로 해제 */
void sfree(void *ptr)
{
	struct pool *pool = NULL;
	unsigned int i;

	if (!ptr)
		return;

	for (i = 0; i < nr_pools; i++) {
		if (ptr_valid(&mp[i], ptr)) {
			pool = &mp[i];
			break;
		}
	}

	if (pool) {
		sfree_pool(pool, ptr);
		return;
	}

	log_err("smalloc: ptr %p not from smalloc pool\n", ptr);
}

/* [한국어] 풀에서 빈 공간이 있는 비트맵 워드의 인덱스를 탐색
 *   next_non_full부터 시작하여 순환 탐색 */
static unsigned int find_best_index(struct pool *pool)
{
	unsigned int i;

	assert(pool->free_blocks);

	for (i = pool->next_non_full; pool->bitmap[i] == -1U; i++) {
		if (i == pool->nr_blocks - 1) {
			unsigned int j;

			for (j = 0; j < pool->nr_blocks; j++)
				if (pool->bitmap[j] != -1U)
					return j;
		}
	}

	return i;
}

/* [한국어] 풀 내부 할당 핵심 함수 — 비트맵에서 연속된 빈 블록을 찾아 할당 표시하고 포인터 반환
 *   세마포어로 동기화하여 멀티프로세스 안전 보장 */
static void *__smalloc_pool(struct pool *pool, size_t size)
{
	size_t nr_blocks;
	unsigned int i;
	unsigned int offset;
	unsigned int last_idx;
	void *ret = NULL;

	fio_sem_down(pool->lock);

	nr_blocks = size_to_blocks(size);
	if (nr_blocks > pool->free_blocks)
		goto fail;

	pool->next_non_full = find_best_index(pool);

	last_idx = 0;
	offset = -1U;
	i = pool->next_non_full;
	while (i < pool->nr_blocks) {
		unsigned int idx;

		if (pool->bitmap[i] == -1U) {
			i++;
			last_idx = 0;
			continue;
		}

		idx = find_next_zero(pool->bitmap[i], last_idx);
		if (!blocks_free(pool, i, idx, nr_blocks)) {
			idx += nr_blocks;
			if (idx < SMALLOC_BPI)
				last_idx = idx;
			else {
				last_idx = 0;
				while (idx >= SMALLOC_BPI) {
					i++;
					idx -= SMALLOC_BPI;
				}
			}
			continue;
		}
		set_blocks(pool, i, idx, nr_blocks);
		offset = i * SMALLOC_BPL + idx * SMALLOC_BPB;
		break;
	}

	if (i < pool->nr_blocks) {
		pool->free_blocks -= nr_blocks;
		ret = pool->map + offset;
	}
fail:
	fio_sem_up(pool->lock);
	return ret;
}

/* [한국어] 사용자 요청 크기를 실제 할당 크기로 변환 (헤더 + 레드존 포함) */
static size_t size_to_alloc_size(size_t size)
{
	size_t alloc_size = size + sizeof(struct block_hdr);

	/*
	 * Round to int alignment, so that the postred pointer will
	 * be naturally aligned as well.
	 */
#ifdef SMALLOC_REDZONE
	alloc_size += sizeof(unsigned int);
	alloc_size = (alloc_size + int_mask) & ~int_mask;
#endif

	return alloc_size;
}

/* [한국어] 특정 풀에서 메모리 할당 — 블록 헤더 설정, 레드존 기록, 데이터 영역 0 초기화 */
static void *smalloc_pool(struct pool *pool, size_t size)
{
	size_t alloc_size = size_to_alloc_size(size);
	void *ptr;

	ptr = __smalloc_pool(pool, alloc_size);
	if (ptr) {
		struct block_hdr *hdr = ptr;

		hdr->size = alloc_size;
		fill_redzone(hdr);

		ptr += sizeof(*hdr);
		memset(ptr, 0, size);
	}

	return ptr;
}

/* [한국어] 디버그용: 풀의 비트맵 상태를 문자열로 출력 (enable_smalloc_debug가 true일 때만) */
static void smalloc_print_bitmap(struct pool *pool)
{
	size_t nr_blocks = pool->nr_blocks;
	unsigned int *bitmap = pool->bitmap;
	unsigned int i, j;
	char *buffer;

	if (!enable_smalloc_debug)
		return;

	buffer = malloc(SMALLOC_BPI + 1);
	if (!buffer)
		return;
	buffer[SMALLOC_BPI] = '\0';

	for (i = 0; i < nr_blocks; i++) {
		unsigned int line = bitmap[i];

		/* skip completely full lines */
		if (line == -1U)
			continue;

		for (j = 0; j < SMALLOC_BPI; j++)
			if ((1 << j) & line)
				buffer[SMALLOC_BPI-1-j] = '1';
			else
				buffer[SMALLOC_BPI-1-j] = '0';

		log_err("smalloc: bitmap %5u, %s\n", i, buffer);
	}

	free(buffer);
}

/* [한국어] 디버그 정보 출력 — 각 풀의 free/total 블록 수를 표시하고 할당 시도 테스트 */
void smalloc_debug(size_t size)
{
	unsigned int i;
	size_t alloc_size = size_to_alloc_size(size);
	size_t alloc_blocks;

	alloc_blocks = size_to_blocks(alloc_size);

	if (size)
		log_err("smalloc: size = %lu, alloc_size = %lu, blocks = %lu\n",
			(unsigned long) size, (unsigned long) alloc_size,
			(unsigned long) alloc_blocks);
	for (i = 0; i < nr_pools; i++) {
		log_err("smalloc: pool %u, free/total blocks %u/%u\n", i,
			(unsigned int) (mp[i].free_blocks),
			(unsigned int) (mp[i].nr_blocks*sizeof(unsigned int)*8));
		if (size && mp[i].free_blocks >= alloc_blocks) {
			void *ptr = smalloc_pool(&mp[i], size);
			if (ptr) {
				sfree(ptr);
				last_pool = i;
				log_err("smalloc: smalloc_pool %u succeeded\n", i);
			} else {
				log_err("smalloc: smalloc_pool %u failed\n", i);
				log_err("smalloc: next_non_full=%u, nr_blocks=%u\n",
					(unsigned int) mp[i].next_non_full, (unsigned int) mp[i].nr_blocks);
				smalloc_print_bitmap(&mp[i]);
			}
		}
	}
}

/* [한국어] 공유 메모리 할당 (외부 API) — 모든 풀을 순회하며 할당 시도
 *   last_pool부터 시작하여 효율적으로 탐색. 실패 시 OOM 에러 출력 */
void *smalloc(size_t size)
{
	unsigned int i, end_pool;

	if (size != (unsigned int) size)
		return NULL;

	i = last_pool;
	end_pool = nr_pools;

	do {
		for (; i < end_pool; i++) {
			void *ptr = smalloc_pool(&mp[i], size);

			if (ptr) {
				last_pool = i;
				return ptr;
			}
		}
		if (last_pool) {
			end_pool = last_pool;
			last_pool = i = 0;
			continue;
		}

		break;
	} while (1);

	log_err("smalloc: OOM. Consider using --alloc-size to increase the "
		"shared memory available.\n");
	smalloc_debug(size);
	return NULL;
}

/* [한국어] calloc의 공유 메모리 버전 — smalloc이 이미 0 초기화하므로 별도 memset 불필요 */
void *scalloc(size_t nmemb, size_t size)
{
	/*
	 * smalloc_pool (called by smalloc) will zero the memory, so we don't
	 * need to do it here.
	 */
	return smalloc(nmemb * size);
}

/* [한국어] strdup의 공유 메모리 버전 — 문자열을 공유 메모리에 복사 */
char *smalloc_strdup(const char *str)
{
	char *ptr = NULL;

	ptr = smalloc(strlen(str) + 1);
	if (ptr)
		strcpy(ptr, str);
	return ptr;
}
