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

static inline uint64_t __seed(uint64_t x, uint64_t m)
{
	return (x < m) ? x + m : x;
}

static void __init_rand32(struct taus88_state *state, unsigned int seed)
{
	int cranks = 6;

#define LCG(x, seed)  ((x) * 69069 ^ (seed))

	state->s1 = __seed(LCG((2^31) + (2^17) + (2^7), seed), 1);
	state->s2 = __seed(LCG(state->s1, seed), 7);
	state->s3 = __seed(LCG(state->s2, seed), 15);

	while (cranks--)
		__rand32(state);
}

void __init_rand64(struct taus258_state *state, uint64_t seed)
{
	int cranks = 6;

#define LCG64(x, seed)  ((x) * 6906969069ULL ^ (seed))

	state->s1 = __seed(LCG64((2^31) + (2^17) + (2^7), seed), 1);
	state->s2 = __seed(LCG64(state->s1, seed), 7);
	state->s3 = __seed(LCG64(state->s2, seed), 15);
	state->s4 = __seed(LCG64(state->s3, seed), 33);
	state->s5 = __seed(LCG64(state->s4, seed), 49);

	while (cranks--)
		__rand64(state);
}

void init_rand(struct frand_state *state, bool use64)
{
	state->use64 = use64;

	if (!use64)
		__init_rand32(&state->state32, 1);
	else
		__init_rand64(&state->state64, 1);
}

void init_rand_seed(struct frand_state *state, uint64_t seed, bool use64)
{
	state->use64 = use64;

	if (!use64)
		__init_rand32(&state->state32, (unsigned int) seed);
	else
		__init_rand64(&state->state64, seed);
}

void __fill_random_buf_small(void *buf, unsigned int len, uint64_t seed)
{
	uint64_t *b = buf;
	uint64_t *e = b  + len / sizeof(*b);
	unsigned int rest = len % sizeof(*b);

	for (; b != e; ++b) {
		*b = seed;
		seed = __hash_u64(seed);
	}

	if (fio_unlikely(rest))
		__builtin_memcpy(e, &seed, rest);
}

void __fill_random_buf(void *buf, unsigned int len, uint64_t seed)
{
	static uint64_t prime[] = {1, 2, 3, 5, 7, 11, 13, 17,
				   19, 23, 29, 31, 37, 41, 43, 47};
	uint64_t *b, *e, s[CONFIG_SEED_BUCKETS];
	unsigned int rest;
	int p;

	/*
	 * Calculate the max index which is multiples of the seed buckets.
	 */
	rest = (len / sizeof(*b) / CONFIG_SEED_BUCKETS) * CONFIG_SEED_BUCKETS;

	b = buf;
	e = b + rest;

	rest = len - (rest * sizeof(*b));

	for (p = 0; p < CONFIG_SEED_BUCKETS; p++)
		s[p] = seed * prime[p];

	for (; b != e; b += CONFIG_SEED_BUCKETS) {
		for (p = 0; p < CONFIG_SEED_BUCKETS; ++p) {
			b[p] = s[p];
			s[p] = __hash_u64(s[p]);
		}
	}

	__fill_random_buf_small(b, rest, s[0]);
}

uint64_t fill_random_buf(struct frand_state *fs, void *buf,
			 unsigned int len)
{
	uint64_t r = __get_next_seed(fs);

	__fill_random_buf(buf, len, r);
	return r;
}

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
