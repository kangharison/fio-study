/*
 * [한국어 설명] fio 의사 난수 생성기(PRNG) 공개/인라인 헤더 (rand.h)
 *
 * === 파일의 역할 ===
 * lib/rand.c 의 외부 API(init_rand, init_rand_seed, __init_rand64,
 * __fill_random_buf, fill_random_buf, __fill_random_buf_percentage,
 * fill_random_buf_percentage) 선언과 함께, 고성능 tight-loop 경로에서 쓰이는
 * 핵심 PRNG 인라인 함수들(__rand32, __rand64, __rand, __rand_0_1, rand32_upto,
 * rand64_upto, rand_between, __get_next_seed, frand_copy 등) 과 상태
 * 구조체(struct taus88_state, taus258_state, frand_state) 를 완전 정의로
 * 노출한다. 인라인으로 헤더에 두는 이유는 io_u 경로마다 호출되는 상수
 * 시프트/XOR 연산의 함수 호출 오버헤드를 제거하기 위함이다.
 *
 * 알고리즘:
 *   - 32비트 : L'Écuyer "Taus88" (탭 13/2/3 의 Tausworthe) — 주기 약 2^88.
 *   - 64비트 : L'Écuyer "Taus258" (5 조합 Tausworthe) — 주기 약 2^258.
 * 두 버전 모두 내부 상태의 상위 비트 특성("s1>1, s2>7, s3>15" 같은 제약) 을
 * 유지해야 올바르게 순환한다 — 이 제약은 init_rand_seed 가 보장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "모든 랜덤 결정" 의 뿌리. 오프셋 선택(io_u.c), 버퍼 채움(random buf,
 * compress%, dedupe%), 블록 크기 분포(bssplit), 지연시간 샘플링(steadystate),
 * rate 변동, pattern 난수 채움 등 수십 경로가 frand_state 를 소유하고 여기의
 * 인라인들을 호출한다. 시드는 FIO_RAND_SEED (파이썬 스타일 16-way 시드 버킷)
 * 으로 잡 간 독립성을 보장.
 * 호출 체인(대표):
 *   io_u.c get_io_u → __rand(&td->rand_state) or rand_between(...)
 *   stat.c buffer_compress_percentage → __fill_random_buf_percentage
 *   verify.c populate_verify_io_u → fill_random_buf 로 검증 버퍼 채움
 *
 * === 타 모듈과의 연결 ===
 * - rand.c : 비인라인 함수 정의(seed 초기화, 버퍼 채움).
 * - lib/types.h : bool 폴백.
 * - io_u.c / verify.c / stat.c / engines/* : 소비자.
 * - ../hash.h : __hash_u64 — zipf/gauss 공간 분산에 사용(이 파일은 직접
 *   포함하지 않지만 frand_state 를 공유하는 자료구조들이 함께 쓴다).
 * 데이터 흐름: seed(uint64_t) → init_rand_seed → Tausworthe 상태 → __rand
 * 반복 → 실수/정수/범위 값 → 호출자.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct taus88_state : 32비트 상태 3 워드(s1,s2,s3).
 * - struct taus258_state : 64비트 상태 5 워드(s1..s5).
 * - struct frand_state : union 으로 32/64 비트 상태를 합치고 use64 플래그로
 *   런타임에 분기. 잡/엔진마다 이 구조체를 자체 소유.
 * - __rand / __rand_0_1 : 핵심 생성기. __rand_0_1 은 [0,1) 균등.
 * - rand_between(state, start, end) : 범위 지정(양 끝 포함) 정수 샘플.
 * - init_rand_seed / init_rand : 시드 초기화. use64 플래그로 모드 결정.
 * - fill_random_buf / __fill_random_buf : 임의 길이 버퍼를 난수로 채움.
 * - fill_random_buf_percentage : buffer_compress_percentage 를 시뮬레이션
 *   하기 위해 지정 비율만 난수로 채우고 나머지는 패턴으로 채움.
 */

#ifndef FIO_RAND_H
#define FIO_RAND_H
/* [한국어] 헤더 가드. 거의 모든 fio 소스가 간접 포함하므로 강력한 가드 필수. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint32_t/uint64_t/unsigned int 고정폭. Tausworthe
 * 상태의 정확한 비트 폭 보장. */

#include <assert.h>
/* [한국어] <assert.h> : rand32_upto/rand64_upto 가 use64 플래그 정합성을
 * assert 로 검증. 디버그 빌드에서 오용 조기 감지. */

#include "types.h"
/* [한국어] "types.h" : bool 폴백. init_rand / init_rand_seed 인자에 사용. */

/* [한국어] 32비트 난수의 최대값(0xFFFFFFFF). -1U 로 표현하여 UINT_MAX 의
 * 고정폭 의미를 명시. __rand_0_1 에서 분모로 사용. */
#define FRAND32_MAX	(-1U)
/* [한국어] 32비트 난수 범위 + 1 (double, [0,1) 변환용). 2^32 = 4294967296 을
 * 직접 상수로 쓰면 long 오버플로가 이식성 문제를 일으킬 수 있어 1ULL 로
 * 승격 후 계산. 1.0 으로 곱하는 이유는 정수 → double 변환을 컴파일 타임에 고정. */
#define FRAND32_MAX_PLUS_ONE	(1.0 * (1ULL << 32))
/* [한국어] 64비트 난수의 최대값(0xFFFFFFFFFFFFFFFF). -1ULL 로 표현. */
#define FRAND64_MAX	(-1ULL)
/* [한국어] 64비트 난수 범위 + 1. 2^64 는 단일 상수 표현 불가하므로
 * (2^32) × (2^32) 로 분리 곱셈. double 정밀도 53 비트로는 정확한 표현이
 * 아니지만 [0,1) 변환에 사용되는 분모로는 충분한 근사. */
#define FRAND64_MAX_PLUS_ONE	(1.0 * (1ULL << 32) * (1ULL << 32))

struct taus88_state {
	unsigned int s1, s2, s3;
	/* [한국어] 3 개의 Tausworthe 상태 변수. XOR 조합으로 32비트 난수 생성.
	 * 제약: s1>1, s2>7, s3>15 (각각 상위 2/3/4 비트 중 하나라도 0 이면
	 *   Tausworthe 의 주기가 붕괴). init_rand_seed 가 이 제약을 강제하여
	 *   초기화. 설정자: init_rand_seed/__rand32. 읽는 자: __rand32 내부 루프.
	 * 동기화: 잡 스레드 단일 소유. 다른 잡은 서로 다른 seed 로 독립. */
};

struct taus258_state {
	uint64_t s1, s2, s3, s4, s5;
	/* [한국어] 5 개의 64비트 Tausworthe 상태 변수. XOR 조합으로 64비트 난수
	 * 생성. 주기 약 2^258 으로, 현실적인 I/O 테스트 지속 시간 내 주기
	 * 반복이 절대 일어나지 않음.
	 * 제약: 각 si 가 특정 마스크(예: s1 의 상위 1 비트가 0 이면 안 됨) 를
	 *   만족해야 하며, __init_rand64 가 이를 보장. */
};

struct frand_state {
	unsigned int use64;
	/* [한국어] 0 이면 32비트 Taus88, 1 이면 64비트 Taus258 사용.
	 * `--allrandrepeat`, `--randseed`, 플랫폼(32 vs 64 bit), 잡 옵션에 따라
	 * init_rand/init_rand_seed 가 결정. 읽는 자: __rand/__rand_0_1/rand_between. */
	union {
		struct taus88_state state32;
		/* [한국어] 32비트 모드일 때 활성. */
		struct taus258_state state64;
		/* [한국어] 64비트 모드일 때 활성. */
	};
	/* [한국어] union 사용으로 두 모드가 동시에 메모리를 차지하지 않아 frand_state
	 * 전체 크기는 40 바이트 수준(64비트 모드 기준). 잡당 여러 개 보유해도
	 * 메모리 압박 작음. */
};

/* [한국어] rand_max - 현재 모드(32/64비트) 의 난수 최대값을 반환. 호출자가
 * 정규화/범위 계산에 사용. */
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
	/* [한국어] 32비트 상태 deep-copy. frand_copy 의 32 비트 분기가 호출.
	 * checkpoint / savestate 기능(`--verify_state_save`) 에서 스냅샷용. */
}

static inline void __frand64_copy(struct taus258_state *dst,
				  struct taus258_state *src)
{
	dst->s1 = src->s1;
	dst->s2 = src->s2;
	dst->s3 = src->s3;
	dst->s4 = src->s4;
	dst->s5 = src->s5;
	/* [한국어] 64비트 상태 deep-copy. 5 워드 복사. */
}

static inline void frand_copy(struct frand_state *dst, struct frand_state *src)
{
	if (src->use64)
		__frand64_copy(&dst->state64, &src->state64);
	else
		__frand32_copy(&dst->state32, &src->state32);

	dst->use64 = src->use64;
	/* [한국어] 모드 플래그 복사. 두 상태가 완전히 같은 시퀀스를 생성하도록. */
}

/*
 * [한국어] __rand32 - Taus88 알고리즘으로 32비트 난수 한 개 생성.
 *
 * TAUSWORTHE 매크로: ((s & c) << d) ^ (((s << a) ^ s) >> b)
 *   - a/b/d 는 시프트 상수, c 는 상위 비트 보존 마스크.
 *   각 상태 변수를 독립적으로 갱신한 뒤 XOR 조합하여 최종 값을 생성한다.
 *   마스크(c) 는 상위 비트 제약(si 의 상위 몇 비트가 항상 유지되어야) 을
 *   위해 하위 비트를 클리어한다.
 *
 * 실행 컨텍스트: 어디서나. tight loop 인라인 삽입.
 * 호출 체인: __rand → __rand32 → TAUSWORTHE 매크로.
 */
static inline unsigned int __rand32(struct taus88_state *state)
{
#define TAUSWORTHE(s,a,b,c,d) ((s&c)<<d) ^ (((s <<a) ^ s)>>b)
	/* [한국어] Tausworthe 한 단계 매크로. s 는 상태, a 는 전방 시프트,
	 * b 는 후방 시프트, c 는 보존 마스크, d 는 최종 시프트. 결과를 새
	 * 상태로 사용. */

	state->s1 = TAUSWORTHE(state->s1, 13, 19, 4294967294UL, 12);
	/* [한국어] s1 갱신: 마스크 4294967294UL = 0xFFFFFFFE (하위 1 비트 클리어)
	 * 로 "s1 > 1" 제약 유지. 시프트 13/19/12 는 L'Écuyer 검증 상수. */
	state->s2 = TAUSWORTHE(state->s2, 2, 25, 4294967288UL, 4);
	/* [한국어] s2 갱신: 마스크 0xFFFFFFF8 (하위 3 비트 클리어) → "s2 > 7" 제약. */
	state->s3 = TAUSWORTHE(state->s3, 3, 11, 4294967280UL, 17);
	/* [한국어] s3 갱신: 마스크 0xFFFFFFF0 (하위 4 비트 클리어) → "s3 > 15" 제약. */

	/* [한국어] 3 개 상태의 XOR 조합이 최종 난수. 각 si 는 독립 시퀀스이고
	 * XOR 로 엔트로피가 합성되어 주기 2^88 의 고품질 난수를 얻는다. */
	return (state->s1 ^ state->s2 ^ state->s3);
}

/*
 * [한국어] __rand64 - Taus258 알고리즘으로 64비트 난수 한 개 생성.
 *
 * 5 개의 64비트 상태 변수를 각각 시프트-XOR 연산으로 갱신한 뒤
 * 전체를 XOR 조합하여 64비트 난수를 생성한다. 주기 약 2^258.
 * 각 si 의 마스크(18446744073709551614ULL 등) 는 상위 보존 비트 수를 고정
 * (s1 상위 1, s2 상위 9, s3 상위 12, s4 상위 17, s5 상위 24 비트 유지).
 */
static inline uint64_t __rand64(struct taus258_state *state)
{
	uint64_t xval;
	/* [한국어] 각 상태 갱신의 중간값. (s << a) ^ s 후 시프트 b 결과. */

	xval = ((state->s1 <<  1) ^ state->s1) >> 53;
	state->s1 = ((state->s1 & 18446744073709551614ULL) << 10) ^ xval;
	/* [한국어] s1: 마스크 0xFFFFFFFFFFFFFFFE (하위 1 비트 클리어).
	 * L'Écuyer Taus258 의 정의 상수 (a=1, b=53, c=..., d=10). */

	xval = ((state->s2 << 24) ^ state->s2) >> 50;
	state->s2 = ((state->s2 & 18446744073709551104ULL) <<  5) ^ xval;
	/* [한국어] s2: 마스크 0xFFFFFFFFFFFFFE00 (하위 9 비트 클리어). */

	xval = ((state->s3 <<  3) ^ state->s3) >> 23;
	state->s3 = ((state->s3 & 18446744073709547520ULL) << 29) ^ xval;
	/* [한국어] s3: 마스크 0xFFFFFFFFFFFFF000 (하위 12 비트 클리어). */

	xval = ((state->s4 <<  5) ^ state->s4) >> 24;
	state->s4 = ((state->s4 & 18446744073709420544ULL) << 23) ^ xval;
	/* [한국어] s4: 마스크 0xFFFFFFFFFFFE0000 (하위 17 비트 클리어). */

	xval = ((state->s5 <<  3) ^ state->s5) >> 33;
	state->s5 = ((state->s5 & 18446744073701163008ULL) <<  8) ^ xval;
	/* [한국어] s5: 마스크 0xFFFFFFFFFF000000 (하위 24 비트 클리어). */

	return (state->s1 ^ state->s2 ^ state->s3 ^ state->s4 ^ state->s5);
	/* [한국어] 5 개 상태 XOR 조합이 최종 64비트 난수. */
}

static inline uint64_t __rand(struct frand_state *state)
{
	if (state->use64)
		return __rand64(&state->state64);
	else
		return __rand32(&state->state32);
	/* [한국어] 런타임 분기 한 번에 모드별 생성기로 위임. tight loop 에서도
	 * 분기 예측기가 잡 생애주기 동안 모드를 바꾸지 않으므로 비용 거의 0. */
}

/*
 * [한국어] __rand_0_1 - [0, 1) 범위의 균등 분포 실수를 반환.
 *
 * (난수 + 1.0) / (최대값 + 1.0) 으로 변환하여 0.0 이상 1.0 미만의 값을 생성.
 * zipf, gauss 등 분포 생성기에서 균등 난수를 기반으로 역변환 샘플링할 때 사용.
 */
static inline double __rand_0_1(struct frand_state *state)
{
	if (state->use64) {
		uint64_t val = __rand64(&state->state64);

		return (val + 1.0) / FRAND64_MAX_PLUS_ONE;
		/* [한국어] 64비트 결과를 double 로 변환 후 정규화. 53비트 정밀도
		 * 초과 부분은 반올림되지만 통계적 분포에 영향 없음. */
	} else {
		uint32_t val = __rand32(&state->state32);

		return (val + 1.0) / FRAND32_MAX_PLUS_ONE;
		/* [한국어] 32비트 결과는 정확히 double 로 무손실 표현 가능(53>>32). */
	}
}

/*
 * [한국어] rand32_upto - [0, end] 범위의 32비트 난수를 반환.
 * 스케일링 방식으로 범위를 제한. rand_between() 에서 내부적으로 호출됨.
 */
static inline uint32_t rand32_upto(struct frand_state *state, uint32_t end)
{
	uint32_t r;

	assert(!state->use64);
	/* [한국어] 이 함수는 32비트 모드 전용. 64비트 모드 상태를 잘못 넘기면
	 * 중간 결과의 엔트로피가 32비트로 잘려 범위 편향 위험. assert 로 개발
	 * 단계에서 오용 차단. */

	r = __rand32(&state->state32);
	end++;
	/* [한국어] end++ : 입력 "end 포함" 을 "[0, end+1)" 으로 변환하여 곱셈
	 * 스케일링이 자연스럽게 끝 포함 범위를 생성. */
	return (int) ((double)end * (r / FRAND32_MAX_PLUS_ONE));
	/* [한국어] (r / 최대값+1) 은 [0,1), 거기에 end+1 을 곱하면 [0, end+1)
	 * 실수. (int) 절삭으로 [0, end] 정수. double 경유로 오버플로 회피. */
}

/* [한국어] rand64_upto - [0, end] 범위의 64비트 난수를 반환. */
static inline uint64_t rand64_upto(struct frand_state *state, uint64_t end)
{
	uint64_t r;

	assert(state->use64);
	/* [한국어] 64비트 모드 전용 assert. */

	r = __rand64(&state->state64);
	end++;
	return (uint64_t) ((double)end * (r / FRAND64_MAX_PLUS_ONE));
	/* [한국어] 32 버전과 동일 공식, double 경유 스케일링. 64비트 범위 풀
	 * 사용 시 double 의 53비트 정밀도 한계로 분포가 11비트 간격으로 뭉쳐
	 * 질 수 있으나 I/O 오프셋 분포로는 무시할 수 있는 오차. */
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
	/* [한국어] [start, end] 양끝 포함. 범위 (end-start) 를 *_upto 로 뽑고
	 * start 를 더해 이동. start > end 인 경우 호출자가 사전에 차단해야 함
	 * (음의 차 → 매우 큰 양수로 래핑되어 잘못된 분포). */
}

/*
 * [한국어] __get_next_seed - 다음 시드 값을 생성.
 *
 * 64비트 플랫폼(sizeof(int) != sizeof(long*)) 에서는 두 번의 난수를 곱하여
 * 전체 64비트 범위를 커버하는 시드를 생성한다.
 * fill_random_buf() 에서 버퍼 채우기용 시드를 뽑을 때 호출됨.
 */
static inline uint64_t __get_next_seed(struct frand_state *fs)
{
	uint64_t r = __rand(fs);

	/* [한국어] 64비트 플랫폼에서 두 난수를 곱해 전체 비트 범위를 활용.
	 * sizeof(int) != sizeof(long*) : 32비트 int + 64비트 포인터 플랫폼 판별
	 * 이디엄(구형 ILP32 vs LP64 구분). LP64 에서는 __rand 가 32비트 모드일
	 * 경우 상위 32비트가 0 으로 남으므로 두 번째 난수 곱셈으로 64비트
	 * 공간을 확장한다. */
	if (sizeof(int) != sizeof(long *))
		r *= (unsigned long) __rand(fs);

	return r;
}

extern void init_rand(struct frand_state *, bool);
/* [한국어] init_rand - 시스템 기본 시드(/dev/urandom 또는 시간+pid) 로 상태
 * 를 초기화. 두 번째 인자 use64 : true=64비트 Taus258, false=32비트 Taus88.
 * 잡별 분리 시드가 필요하지 않은 부속 경로(테스트, 유틸) 에서 사용. */

extern void init_rand_seed(struct frand_state *, uint64_t seed, bool);
/* [한국어] init_rand_seed - 주어진 seed 로 상태 초기화. 잡 스레드별로 FIO_RAND_SEED
 * 의 16-way 시드 버킷 중 하나를 받아 호출되어 잡 간 독립성을 보장.
 * 동작: seed 를 LCG 로 펼쳐 Tausworthe 상태의 "상위 비트 제약" 을 만족시키는
 * 초기 s1..s5 를 구성. 같은 seed 는 항상 같은 시퀀스를 재생산(재현성). */

void __init_rand64(struct taus258_state *state, uint64_t seed);
/* [한국어] __init_rand64 - use64 내부 전용 초기화 헬퍼. init_rand_seed 의
 * 64비트 분기가 호출. API 로 직접 쓸 일은 거의 없음. */

extern void __fill_random_buf(void *buf, unsigned int len, uint64_t seed);
/* [한국어] __fill_random_buf - seed 로부터 독립 PRNG 를 만들어 buf 에 len
 * 바이트를 난수로 채움. 호출자의 frand_state 상태를 갱신하지 않는 버전(순수).
 * verify.c 가 "오프셋 기반 재현 가능한 난수 패턴" 생성을 위해 블록마다 다른
 * seed 로 호출. */

extern uint64_t fill_random_buf(struct frand_state *, void *buf, unsigned int len);
/* [한국어] fill_random_buf - 주어진 frand_state 에서 새 seed 를 뽑아 buf 를
 * 채움. 반환: 사용한 seed (호출자가 재현/검증용으로 기록). 잡 쓰기 버퍼에
 * 매 I/O 마다 호출되는 tight path. */

extern void __fill_random_buf_percentage(uint64_t, void *, unsigned int, unsigned int, unsigned int, char *, unsigned int);
/* [한국어] __fill_random_buf_percentage - 버퍼의 일부만 난수로 채우고 나머지
 * 는 고정 패턴(호출자 제공) 으로 채움. `--buffer_compress_percentage` 의 역
 * 기능: 지정한 compress% 는 반복 패턴으로, 나머지는 난수로 채워 압축률을
 * 제어. 인자 순서: seed, buf, len, percentage, chunk, pattern, pat_len. */

extern uint64_t fill_random_buf_percentage(struct frand_state *, void *, unsigned int, unsigned int, unsigned int, char *, unsigned int);
/* [한국어] fill_random_buf_percentage - 위 함수의 frand_state 버전. 새 seed
 * 를 뽑아 처리하고 seed 반환. 잡 쓰기 경로에서 매 I/O 마다 호출. */

#endif
/* [한국어] FIO_RAND_H 가드 종료. */
