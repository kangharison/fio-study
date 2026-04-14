/*
 * [한국어 설명] 난수 생성기 구현 (rand.c)
 *
 * === 파일의 역할 ===
 * Tausworthe 알고리즘 기반의 의사 난수 생성기(PRNG)를 구현한다.
 * 32비트(Taus88, 주기 약 2^88)와 64비트(Taus258, 주기 약 2^258) 두 가지 버전을 제공하며,
 * 난수 시드 초기화 및 버퍼를 난수 데이터로 채우는 기능을 포함한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - Taus88: 3개의 상태 변수(s1, s2, s3)를 XOR 조합하는 결합 Tausworthe 생성기
 * - Taus258: 5개의 상태 변수를 사용하는 64비트 확장 버전
 * - LCG(Linear Congruential Generator): 시드 초기화에 사용
 * - CONFIG_SEED_BUCKETS: 여러 소수 기반 시드로 병렬 해시하여 버퍼를 빠르게 채움
 * - __fill_random_buf_percentage: 난수와 패턴을 비율에 따라 혼합하여 버퍼 채움
 *
 * === fio에서의 사용 ===
 * fio의 모든 난수 기반 기능의 핵심 엔진이다. 랜덤 I/O 오프셋 생성, 블록 크기 랜덤화,
 * 검증용 버퍼 데이터 생성, buffer_compress_percentage 옵션 처리 등에 사용된다.
 */

/*
  This is a maximally equidistributed combined Tausworthe generator
  based on code from GNU Scientific Library 1.5 (30 Jun 2004)

   x_n = (s1_n ^ s2_n ^ s3_n)

   s1_{n+1} = (((s1_n & 4294967294) <<12) ^ (((s1_n <<13) ^ s1_n) >>19))
   s2_{n+1} = (((s2_n & 4294967288) << 4) ^ (((s2_n << 2) ^ s2_n) >>25))
   s3_{n+1} = (((s3_n & 4294967280) <<17) ^ (((s3_n << 3) ^ s3_n) >>11))

   The period of this generator is about 2^88.

   From: P. L'Ecuyer, "Maximally Equidistributed Combined Tausworthe
   Generators", Mathematics of Computation, 65, 213 (1996), 203--213.

   This is available on the net from L'Ecuyer's home page,

   http://www.iro.umontreal.ca/~lecuyer/myftp/papers/tausme.ps
   ftp://ftp.iro.umontreal.ca/pub/simulation/lecuyer/papers/tausme.ps

   There is an erratum in the paper "Tables of Maximally
   Equidistributed Combined LFSR Generators", Mathematics of
   Computation, 68, 225 (1999), 261--269:
   http://www.iro.umontreal.ca/~lecuyer/myftp/papers/tausme2.ps

        ... the k_j most significant bits of z_j must be non-
        zero, for each j. (Note: this restriction also applies to the
        computer code given in [4], but was mistakenly not mentioned in
        that paper.)

   This affects the seeding procedure by imposing the requirement
   s1 > 1, s2 > 7, s3 > 15.

*/

#include <string.h>
#include "rand.h"
#include "pattern.h"
#include "../hash.h"

int arch_random;

/*
 * [한국어] __seed - 시드 값이 최소값 이상이 되도록 보정
 *
 * @x: 입력 시드 값
 * @m: 최소 허용 값 (Tausworthe 알고리즘의 제약 조건: s1>1, s2>7, s3>15)
 * @return: m 이상으로 보정된 시드 값
 *
 * Tausworthe 생성기에서 각 상태 변수의 상위 k_j 비트가 0이 아니어야 하는
 * 제약 조건을 충족시키기 위해, 시드가 최소값보다 작으면 m을 더해서 보정한다.
 */
static inline uint64_t __seed(uint64_t x, uint64_t m)
{
	return (x < m) ? x + m : x;
}

/*
 * [한국어] __init_rand32 - 32비트 Taus88 상태를 시드로 초기화
 *
 * @state: 초기화할 Taus88 상태 구조체
 * @seed: 초기 시드 값
 *
 * LCG(Linear Congruential Generator)로 시드를 확산시켜 3개의 상태 변수를
 * 초기화한 뒤, 6회의 워밍업 반복(crank)으로 초기 상관성을 제거한다.
 * __seed()로 각 상태 변수가 Tausworthe 제약(s1>1, s2>7, s3>15)을 만족하도록 보정한다.
 *
 * 호출 체인: init_rand()/init_rand_seed() → [__init_rand32]
 */
static void __init_rand32(struct taus88_state *state, unsigned int seed)
{
	int cranks = 6;

#define LCG(x, seed)  ((x) * 69069 ^ (seed))

	/* [한국어] LCG로 시드를 확산하여 3개의 상태 변수를 초기화 */
	state->s1 = __seed(LCG((2^31) + (2^17) + (2^7), seed), 1);
	state->s2 = __seed(LCG(state->s1, seed), 7);
	state->s3 = __seed(LCG(state->s2, seed), 15);

	/* [한국어] 6회 워밍업: 초기 상태의 통계적 편향을 제거 */
	while (cranks--)
		__rand32(state);
}

/*
 * [한국어] __init_rand64 - 64비트 Taus258 상태를 시드로 초기화
 *
 * @state: 초기화할 Taus258 상태 구조체
 * @seed: 64비트 초기 시드 값
 *
 * 64비트 LCG로 시드를 확산시켜 5개의 상태 변수를 초기화한다.
 * Taus258은 주기가 약 2^258으로 32비트 버전보다 훨씬 긴 주기를 제공하며,
 * 대규모 랜덤 I/O 테스트에서 주기 반복이 발생하지 않도록 한다.
 *
 * 호출 체인: init_rand()/init_rand_seed() → [__init_rand64]
 */
void __init_rand64(struct taus258_state *state, uint64_t seed)
{
	int cranks = 6;

#define LCG64(x, seed)  ((x) * 6906969069ULL ^ (seed))

	/* [한국어] 64비트 LCG로 5개 상태 변수를 체인 초기화 */
	state->s1 = __seed(LCG64((2^31) + (2^17) + (2^7), seed), 1);
	state->s2 = __seed(LCG64(state->s1, seed), 7);
	state->s3 = __seed(LCG64(state->s2, seed), 15);
	state->s4 = __seed(LCG64(state->s3, seed), 33);
	state->s5 = __seed(LCG64(state->s4, seed), 49);

	/* [한국어] 6회 워밍업: 초기 상태의 통계적 편향을 제거 */
	while (cranks--)
		__rand64(state);
}

/*
 * [한국어] init_rand - 기본 시드(1)로 난수 생성기 초기화
 *
 * @state: 초기화할 통합 난수 상태
 * @use64: true이면 64비트 Taus258, false이면 32비트 Taus88 사용
 *
 * 시드를 1로 고정하여 초기화하므로 매번 동일한 난수 시퀀스를 생성한다.
 * 재현 가능한 테스트가 필요할 때 사용된다.
 *
 * 호출 체인: fio 초기화 코드 → [init_rand] → __init_rand32/__init_rand64
 */
void init_rand(struct frand_state *state, bool use64)
{
	state->use64 = use64;

	if (!use64)
		__init_rand32(&state->state32, 1);
	else
		__init_rand64(&state->state64, 1);
}

/*
 * [한국어] init_rand_seed - 지정된 시드로 난수 생성기 초기화
 *
 * @state: 초기화할 통합 난수 상태
 * @seed: 사용자 지정 시드 값 (--randseed 옵션 등에서 전달)
 * @use64: true이면 64비트, false이면 32비트 생성기 사용
 *
 * 사용자가 --randseed 옵션으로 시드를 지정하면 이 함수가 호출되어
 * 재현 가능한 난수 시퀀스를 생성한다.
 *
 * 호출 체인: init.c 등 → [init_rand_seed] → __init_rand32/__init_rand64
 */
void init_rand_seed(struct frand_state *state, uint64_t seed, bool use64)
{
	state->use64 = use64;

	if (!use64)
		__init_rand32(&state->state32, (unsigned int) seed);
	else
		__init_rand64(&state->state64, seed);
}

/*
 * [한국어] __fill_random_buf_small - 작은 버퍼를 해시 체인으로 난수 데이터로 채움
 *
 * @buf: 채울 버퍼 포인터
 * @len: 버퍼 길이 (바이트)
 * @seed: 시작 시드 값
 *
 * 시드를 반복적으로 해시(__hash_u64)하여 각 8바이트 워드를 채운다.
 * __fill_random_buf()의 나머지 부분이나 작은 버퍼에 대해 호출된다.
 */
void __fill_random_buf_small(void *buf, unsigned int len, uint64_t seed)
{
	uint64_t *b = buf;
	uint64_t *e = b  + len / sizeof(*b);
	unsigned int rest = len % sizeof(*b);

	/* [한국어] 8바이트 단위로 해시 체인을 통해 버퍼 채움 */
	for (; b != e; ++b) {
		*b = seed;
		seed = __hash_u64(seed);
	}

	/* [한국어] 8바이트 미만의 나머지 부분 처리 */
	if (fio_unlikely(rest))
		__builtin_memcpy(e, &seed, rest);
}

/*
 * [한국어] __fill_random_buf - 소수 기반 병렬 해시로 버퍼를 빠르게 난수 데이터로 채움
 *
 * @buf: 채울 버퍼 포인터
 * @len: 버퍼 길이 (바이트)
 * @seed: 시작 시드 값
 *
 * CONFIG_SEED_BUCKETS(기본 16)개의 독립적인 해시 체인을 병렬로 실행하여
 * 버퍼를 빠르게 채운다. 각 버킷은 seed * 소수(prime)로 초기화되어
 * 서로 다른 해시 시퀀스를 생성한다. 나머지는 __fill_random_buf_small()로 처리.
 * buffer_compress_percentage 옵션에서 난수 영역을 채울 때 성능이 중요하다.
 *
 * 호출 체인: fill_random_buf() → [__fill_random_buf] → __hash_u64
 */
void __fill_random_buf(void *buf, unsigned int len, uint64_t seed)
{
	/* [한국어] 16개의 소수 배열 - 각 버킷의 시드를 분산시키는 데 사용 */
	static uint64_t prime[] = {1, 2, 3, 5, 7, 11, 13, 17,
				   19, 23, 29, 31, 37, 41, 43, 47};
	uint64_t *b, *e, s[CONFIG_SEED_BUCKETS];
	unsigned int rest;
	int p;

	/*
	 * Calculate the max index which is multiples of the seed buckets.
	 * [한국어] 버킷 수의 배수로 정렬된 최대 인덱스를 계산
	 */
	rest = (len / sizeof(*b) / CONFIG_SEED_BUCKETS) * CONFIG_SEED_BUCKETS;

	b = buf;
	e = b + rest;

	rest = len - (rest * sizeof(*b));

	/* [한국어] 각 버킷을 seed * 소수로 초기화하여 독립적인 해시 체인 생성 */
	for (p = 0; p < CONFIG_SEED_BUCKETS; p++)
		s[p] = seed * prime[p];

	/* [한국어] 16개 버킷을 인터리브하여 버퍼를 채움 (캐시 친화적) */
	for (; b != e; b += CONFIG_SEED_BUCKETS) {
		for (p = 0; p < CONFIG_SEED_BUCKETS; ++p) {
			b[p] = s[p];
			s[p] = __hash_u64(s[p]);
		}
	}

	/* [한국어] 버킷 배수에 맞지 않는 나머지 부분을 작은 버퍼 함수로 처리 */
	__fill_random_buf_small(b, rest, s[0]);
}

/*
 * [한국어] fill_random_buf - frand_state에서 시드를 생성하여 버퍼를 난수로 채움
 *
 * @fs: 난수 상태 (시드 생성에 사용)
 * @buf: 채울 버퍼
 * @len: 버퍼 길이
 * @return: 사용된 시드 값 (검증 시 동일 시드로 재생성 가능)
 *
 * 호출 체인: io_u.c 등 → [fill_random_buf] → __fill_random_buf
 */
uint64_t fill_random_buf(struct frand_state *fs, void *buf,
			 unsigned int len)
{
	uint64_t r = __get_next_seed(fs);

	__fill_random_buf(buf, len, r);
	return r;
}

/*
 * [한국어] __fill_random_buf_percentage - 난수와 패턴을 비율에 따라 혼합하여 버퍼 채움
 *
 * @seed: 난수 생성 시드
 * @buf: 채울 버퍼
 * @percentage: 압축 가능한(패턴) 데이터의 비율 (0~100)
 * @segment: 세그먼트 크기 (난수+패턴 한 쌍의 크기)
 * @len: 전체 버퍼 길이
 * @pattern: 압축 가능한 영역에 채울 패턴 (NULL이면 0으로 채움)
 * @pbytes: 패턴의 바이트 수
 *
 * buffer_compress_percentage 옵션을 구현한다. 예: percentage=70이면
 * 각 세그먼트의 30%는 난수, 70%는 패턴(또는 0)으로 채워진다.
 * 이를 통해 압축 알고리즘이 지정된 비율로 압축할 수 있는 데이터를 생성한다.
 *
 * 호출 체인: fill_random_buf_percentage() → [__fill_random_buf_percentage]
 */
void __fill_random_buf_percentage(uint64_t seed, void *buf,
				  unsigned int percentage,
				  unsigned int segment, unsigned int len,
				  char *pattern, unsigned int pbytes)
{
	unsigned int this_len;

	if (percentage == 100) {
		if (pbytes)
			(void)cpy_pattern(pattern, pbytes, buf, len);
		else
			memset(buf, 0, len);
		return;
	}

	if (segment > len)
		segment = len;

	while (len) {
		/*
		 * Fill random chunk
		 */
		this_len = ((unsigned long long)segment * (100 - percentage)) / 100;
		if (this_len > len)
			this_len = len;

		__fill_random_buf(buf, this_len, seed);

		len -= this_len;
		if (!len)
			break;
		buf += this_len;
		this_len = segment - this_len;

		if (this_len > len)
			this_len = len;
		else if (len - this_len <= sizeof(long))
			this_len = len;

		if (pbytes)
			(void)cpy_pattern(pattern, pbytes, buf, this_len);
		else
			memset(buf, 0, this_len);

		len -= this_len;
		buf += this_len;
	}
}

/*
 * [한국어] fill_random_buf_percentage - frand_state에서 시드를 뽑아 비율 혼합 버퍼 생성
 *
 * @fs: 난수 상태
 * @buf, @percentage, @segment, @len, @pattern, @pbytes: __fill_random_buf_percentage와 동일
 * @return: 사용된 시드 값
 *
 * 호출 체인: io_u.c → [fill_random_buf_percentage] → __fill_random_buf_percentage
 */
uint64_t fill_random_buf_percentage(struct frand_state *fs, void *buf,
				    unsigned int percentage,
				    unsigned int segment, unsigned int len,
				    char *pattern, unsigned int pbytes)
{
	uint64_t r = __get_next_seed(fs);

	__fill_random_buf_percentage(r, buf, percentage, segment, len,
					pattern, pbytes);
	return r;
}
