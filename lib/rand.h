/*
 * [한국어 설명] 난수 생성기 헤더 (rand.h)
 *
 * === 파일의 역할 ===
 * fio의 의사 난수 생성기(PRNG) 인터페이스를 정의한다.
 * 32비트 Taus88과 64비트 Taus258 상태 구조체, 인라인 난수 생성 함수,
 * 그리고 범위 지정 난수 생성 유틸리티를 제공한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct taus88_state: 3개 상태 변수(s1,s2,s3)를 갖는 32비트 Tausworthe 상태
 * - struct taus258_state: 5개 상태 변수를 갖는 64비트 Tausworthe 상태
 * - struct frand_state: use64 플래그로 32/64비트를 선택하는 통합 난수 상태
 * - TAUSWORTHE 매크로: 시프트-XOR 연산으로 다음 난수를 O(1)에 생성
 * - __rand_0_1: [0,1) 범위의 균등 분포 실수를 반환
 * - rand_between: 지정된 범위 [start, end] 내의 균등 분포 정수를 반환
 *
 * === fio에서의 사용 ===
 * fio 전체에서 사용하는 난수 생성의 핵심 헤더이다. zipf, gauss, lfsr 등
 * 다른 분포 생성기들도 이 헤더의 frand_state를 내부적으로 사용하며,
 * I/O 오프셋, 블록 크기, 지연 시간 등의 랜덤화에 활용된다.
 */

#ifndef FIO_RAND_H
#define FIO_RAND_H

#include <inttypes.h>
#include <assert.h>
#include "types.h"

/* [한국어] 32비트 난수의 최대값 (0xFFFFFFFF) */
#define FRAND32_MAX	(-1U)
/* [한국어] 32비트 난수 범위 + 1 (double, [0,1) 변환용) */
#define FRAND32_MAX_PLUS_ONE	(1.0 * (1ULL << 32))
/* [한국어] 64비트 난수의 최대값 (0xFFFFFFFFFFFFFFFF) */
#define FRAND64_MAX	(-1ULL)
/* [한국어] 64비트 난수 범위 + 1 (double, [0,1) 변환용. 오버플로 방지를 위해 분리 곱셈) */
#define FRAND64_MAX_PLUS_ONE	(1.0 * (1ULL << 32) * (1ULL << 32))

struct taus88_state {
	unsigned int s1, s2, s3;
	/* [한국어] 3개의 Tausworthe 상태 변수. XOR 조합으로 32비트 난수 생성.
	 * 제약: s1>1, s2>7, s3>15 (상위 비트가 0이면 안 됨) */
};

struct taus258_state {
	uint64_t s1, s2, s3, s4, s5;
	/* [한국어] 5개의 64비트 Tausworthe 상태 변수. XOR 조합으로 64비트 난수 생성.
	 * 주기 약 2^258으로 대규모 I/O 테스트에서도 주기 반복이 발생하지 않음 */
};

struct frand_state {
	unsigned int use64;
	/* [한국어] 0이면 32비트 Taus88, 1이면 64비트 Taus258 사용.
	 * --allrandrepeat, --randseed 설정과 플랫폼에 따라 결정됨 */
	union {
		struct taus88_state state32;
		struct taus258_state state64;
	};
};

/* [한국어] rand_max - 현재 모드(32/64비트)의 난수 최대값을 반환 */
static inline uint64_t rand_max(struct frand_state *state)
{
	if (state->use64)
		return FRAND64_MAX;
	else
		return FRAND32_MAX;
}

static inline void __frand32_copy(struct taus88_state *dst,
				  struct taus88_state *src)
{
	dst->s1 = src->s1;
	dst->s2 = src->s2;
	dst->s3 = src->s3;
}

static inline void __frand64_copy(struct taus258_state *dst,
				  struct taus258_state *src)
{
	dst->s1 = src->s1;
	dst->s2 = src->s2;
	dst->s3 = src->s3;
	dst->s4 = src->s4;
	dst->s5 = src->s5;
}

static inline void frand_copy(struct frand_state *dst, struct frand_state *src)
{
	if (src->use64)
		__frand64_copy(&dst->state64, &src->state64);
	else
		__frand32_copy(&dst->state32, &src->state32);

	dst->use64 = src->use64;
}

/*
 * [한국어] __rand32 - Taus88 알고리즘으로 32비트 난수 한 개 생성
 *
 * TAUSWORTHE 매크로: (상태 & 마스크) << 시프트 ^ ((상태 << a) ^ 상태) >> b
 * 각 상태 변수를 독립적으로 갱신한 뒤 XOR 조합하여 최종 값을 생성한다.
 * 마스크(c)는 상위 비트 제약을 유지하기 위해 하위 비트를 클리어한다.
 */
static inline unsigned int __rand32(struct taus88_state *state)
{
#define TAUSWORTHE(s,a,b,c,d) ((s&c)<<d) ^ (((s <<a) ^ s)>>b)

	state->s1 = TAUSWORTHE(state->s1, 13, 19, 4294967294UL, 12);
	state->s2 = TAUSWORTHE(state->s2, 2, 25, 4294967288UL, 4);
	state->s3 = TAUSWORTHE(state->s3, 3, 11, 4294967280UL, 17);

	/* [한국어] 3개 상태의 XOR 조합이 최종 난수 */
	return (state->s1 ^ state->s2 ^ state->s3);
}

/*
 * [한국어] __rand64 - Taus258 알고리즘으로 64비트 난수 한 개 생성
 *
 * 5개의 64비트 상태 변수를 각각 시프트-XOR 연산으로 갱신한 뒤
 * 전체를 XOR 조합하여 64비트 난수를 생성한다. 주기 약 2^258.
 */
static inline uint64_t __rand64(struct taus258_state *state)
{
	uint64_t xval;

	xval = ((state->s1 <<  1) ^ state->s1) >> 53;
	state->s1 = ((state->s1 & 18446744073709551614ULL) << 10) ^ xval;

	xval = ((state->s2 << 24) ^ state->s2) >> 50;
	state->s2 = ((state->s2 & 18446744073709551104ULL) <<  5) ^ xval;

	xval = ((state->s3 <<  3) ^ state->s3) >> 23;
	state->s3 = ((state->s3 & 18446744073709547520ULL) << 29) ^ xval;

	xval = ((state->s4 <<  5) ^ state->s4) >> 24;
	state->s4 = ((state->s4 & 18446744073709420544ULL) << 23) ^ xval;

	xval = ((state->s5 <<  3) ^ state->s5) >> 33;
	state->s5 = ((state->s5 & 18446744073701163008ULL) <<  8) ^ xval;

	return (state->s1 ^ state->s2 ^ state->s3 ^ state->s4 ^ state->s5);
}

static inline uint64_t __rand(struct frand_state *state)
{
	if (state->use64)
		return __rand64(&state->state64);
	else
		return __rand32(&state->state32);
}

/*
 * [한�����] __rand_0_1 - [0, 1) 범위의 균등 분포 실수를 반환
 *
 * (난수 + 1.0) / (최대값 + 1.0)으로 변환하여 0.0 이상 1.0 미만의 값을 생성.
 * zipf, gauss 등 분포 생성기에서 균등 난수를 기반으로 역변환 샘플링할 때 사용.
 */
static inline double __rand_0_1(struct frand_state *state)
{
	if (state->use64) {
		uint64_t val = __rand64(&state->state64);

		return (val + 1.0) / FRAND64_MAX_PLUS_ONE;
	} else {
		uint32_t val = __rand32(&state->state32);

		return (val + 1.0) / FRAND32_MAX_PLUS_ONE;
	}
}

/*
 * [한국어] rand32_upto - [0, end] 범위의 32비트 난수를 반환
 * 스케일링 방식으로 범위를 제한. rand_between()에서 내부적으로 호출됨.
 */
static inline uint32_t rand32_upto(struct frand_state *state, uint32_t end)
{
	uint32_t r;

	assert(!state->use64);

	r = __rand32(&state->state32);
	end++;
	return (int) ((double)end * (r / FRAND32_MAX_PLUS_ONE));
}

/* [한국어] rand64_upto - [0, end] 범위의 64비트 난수를 반환 */
static inline uint64_t rand64_upto(struct frand_state *state, uint64_t end)
{
	uint64_t r;

	assert(state->use64);

	r = __rand64(&state->state64);
	end++;
	return (uint64_t) ((double)end * (r / FRAND64_MAX_PLUS_ONE));
}

/*
 * Generate a random value between 'start' and 'end', both inclusive
 */
static inline uint64_t rand_between(struct frand_state *state, uint64_t start,
				    uint64_t end)
{
	if (state->use64)
		return start + rand64_upto(state, end - start);
	else
		return start + rand32_upto(state, end - start);
}

/*
 * [한국어] __get_next_seed - 다음 시드 값을 생성
 *
 * 64비트 플랫폼(sizeof(int) != sizeof(long*))에서는 두 번의 난수를 곱하여
 * 전체 64비트 범위를 커버하는 시드를 생성한다.
 * fill_random_buf()에서 버퍼 채우기용 시드를 뽑을 때 호출됨.
 */
static inline uint64_t __get_next_seed(struct frand_state *fs)
{
	uint64_t r = __rand(fs);

	/* [한국어] 64비트 플랫폼에서 두 난수를 곱해 전체 비트 범위를 활용 */
	if (sizeof(int) != sizeof(long *))
		r *= (unsigned long) __rand(fs);

	return r;
}

extern void init_rand(struct frand_state *, bool);
extern void init_rand_seed(struct frand_state *, uint64_t seed, bool);
void __init_rand64(struct taus258_state *state, uint64_t seed);
extern void __fill_random_buf(void *buf, unsigned int len, uint64_t seed);
extern uint64_t fill_random_buf(struct frand_state *, void *buf, unsigned int len);
extern void __fill_random_buf_percentage(uint64_t, void *, unsigned int, unsigned int, unsigned int, char *, unsigned int);
extern uint64_t fill_random_buf_percentage(struct frand_state *, void *, unsigned int, unsigned int, unsigned int, char *, unsigned int);

#endif
