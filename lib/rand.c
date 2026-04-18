/*
 * [한국어 설명] fio 전용 의사 난수 생성기(PRNG) 구현 (rand.c)
 *
 * === 파일의 역할 ===
 * fio 가 I/O 오프셋 선택, 블록 크기 랜덤화, 검증 패턴 생성, 압축률 제어
 * (`buffer_compress_percentage`) 등에 사용하는 "재현 가능하고 빠른" 결합
 * Tausworthe 생성기를 구현한다. 32비트 버전은 P. L'Ecuyer 의 maximally-
 * equidistributed Taus88 (주기 ≈ 2^88), 64비트 버전은 Taus258 (주기 ≈ 2^258)
 * 로, 글로벌 lrand48/rand 보다 2~5배 빠르고, 잡/스레드별 독립 상태를 보장하여
 * 락 없이 재현 가능한 시드 기반 실행을 지원한다. 또한 버퍼를 난수 바이트로
 * 채우는 고속 경로(__fill_random_buf, 16-way 병렬 해시 체인) 와, 난수/패턴을
 * 비율 혼합하는 경로(__fill_random_buf_percentage, 압축률 시뮬레이션) 를 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 는 잡(td) 마다 여러 개의 frand_state 를 소유한다 (예: td->random_state,
 * td->verify_state, td->buf_state, td->buf_state_prev 등). 모든 난수 사용
 * 지점은 다음 호출 트리를 따른다:
 *
 *   init.c / options.c (잡 초기화 / randseed 옵션 적용)
 *     → init_rand() / init_rand_seed()      // [본 파일] 시드 주입
 *         → __init_rand32() / __init_rand64() // LCG 확산 + 6회 crank 워밍업
 *
 *   io_u.c (get_next_offset, fill_io_buffer 등)
 *     → __rand() / __rand32() / __rand64()    // (rand.h 의 inline) 한 값 뽑기
 *     → fio_rand64() / fio_rand32()            // use64 분기 랩퍼
 *     → fill_random_buf()                     // 난수 버퍼 채움 (쓰기 전)
 *     → fill_random_buf_percentage()          // 난수+패턴 혼합 (compress %)
 *
 *   verify.c (검증 시)
 *     → __fill_random_buf_small()             // 작은 청크 시드-해시 체인
 *
 * 실행 컨텍스트: 잡 스레드 당 frand_state 를 소유하여 단일 스레드 접근이
 * 원칙(락 불필요). 단, fio_randseed/verify 경로에서 일시적으로 다른 스레드가
 * 읽을 수 있는 경우는 td 레벨 동기화가 상위에서 보장한다.
 *
 * === 타 모듈과의 연결 ===
 * - rand.h : struct frand_state/taus88_state/taus258_state 정의,
 *            inline __rand32/__rand64/__get_next_seed 구현, 본 파일의 공개 API 선언.
 * - pattern.h : cpy_pattern (비율 혼합 경로에서 압축 가능 영역을 사용자 패턴으로 채움).
 * - hash.h : __hash_u64 (Bob Jenkins 류 64비트 비선형 해시; 버퍼 채움 경로에서 시드 체인의 각 단계에 사용).
 * - compiler/compiler.h : fio_unlikely (작은 잔여 바이트 경로 분기 힌트).
 * - arch_random 전역: arch 별 RDRAND/RDSEED 등을 활성화했는지 나타내는 플래그.
 *                      0 = 소프트웨어 Tausworthe 만 사용, 1 = 하드웨어 난수 연동 (init.c 에서 설정).
 * 데이터 흐름: seed → LCG 확산 → 3 또는 5 개 상태 변수 → crank 6회 워밍업 →
 *            __rand* 호출마다 XOR/shift/mask 조합 → 32/64비트 난수 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - init_rand(state, use64):        기본 시드(1)로 초기화 (재현용).
 * - init_rand_seed(state, seed, use64): 사용자 지정 시드로 초기화 (--randseed).
 * - __init_rand32(s, seed):         LCG 확산 후 Taus88 의 (s1, s2, s3) 초기화 + 6회 crank.
 * - __init_rand64(s, seed):         LCG64 확산 후 Taus258 의 (s1..s5) 초기화 + 6회 crank.
 * - __fill_random_buf_small(buf, len, seed): 시드 → __hash_u64 체인으로 8바이트씩 기록.
 * - __fill_random_buf(buf, len, seed):        16개 시드 버킷을 인터리브하며 버퍼 채움 (캐시/ILP 이득).
 * - fill_random_buf(fs, buf, len):            frand_state 에서 시드를 뽑아 위 함수 호출, 시드 반환.
 * - __fill_random_buf_percentage/fill_random_buf_percentage: 압축률 시뮬레이션용 난수/패턴 혼합.
 * - __seed(x, m): Tausworthe 제약(s1>1, s2>7, s3>15 등) 을 만족하도록 시드 보정.
 * - 전역 arch_random: 아키텍처 난수 하드웨어 활성 여부 플래그.
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

#include <string.h>	/* [한국어] __builtin_memcpy 프로토타입(잔여 바이트 저장 시 사용)이 <string.h> 선언과 호환되도록 포함 */
#include "rand.h"	/* [한국어] struct frand_state/taus88_state/taus258_state 정의 및 본 파일 공개 API 선언 */
#include "pattern.h"	/* [한국어] cpy_pattern() — __fill_random_buf_percentage 에서 압축 가능 영역을 사용자 지정 패턴으로 채우는 데 사용 */
#include "../hash.h"	/* [한국어] __hash_u64(seed) — 64비트 비선형 해시; __fill_random_buf 의 시드 체인 각 단계 전진에 사용 */

/* [한국어] 아키텍처 난수 하드웨어(RDRAND 등) 활성 플래그. 0 = 소프트 Tausworthe,
 * 1 = 하드웨어 보조(init.c 에서 --rand-hw-seed 등 옵션 처리 시 설정).
 * extern 으로 여러 파일에서 참조되며, 초기값 0 으로 BSS 에 배치된다. */
int arch_random;

/*
 * [한국어] __seed - 시드 값이 최소값 이상이 되도록 보정
 *
 * @x: 입력 시드 값
 * @m: 최소 허용 값 (Tausworthe 의 s1>1, s2>7, s3>15, ... 등 각 상태의 k_j 비트 제약)
 * @return: m 이상이 된 시드 값
 *
 * L'Ecuyer 의 정정 주석(tausme2.ps)에 따르면 각 z_j 의 상위 k_j 비트가 0 이면
 * 생성기가 고정점에 갇힐 수 있다. 이를 방지하기 위해 x<m 이면 x+m 을 반환해
 * 해당 비트가 반드시 세워지도록 한다 (덧셈이므로 x 의 정보를 보존하면서도
 * 하한을 강제할 수 있음).
 */
static inline uint64_t __seed(uint64_t x, uint64_t m)
{
	return (x < m) ? x + m : x;	/* [한국어] 삼항 연산자: x<m 이면 x+m, 아니면 x 그대로. 분기 예측 친화적 */
}

/*
 * [한국어] __init_rand32 - 32비트 Taus88 상태를 시드로 초기화
 *
 * @state: 초기화할 taus88_state 구조체 (s1, s2, s3)
 * @seed:  사용자 시드 (unsigned int 로 축소되어 전달)
 *
 * 절차:
 *   1. LCG(2^31+2^17+2^7, seed) 로 시작 값을 만들고 s1 에 주입 (하한 1).
 *   2. s2 = LCG(s1, seed) → __seed(…, 7).
 *   3. s3 = LCG(s2, seed) → __seed(…, 15).
 *   4. __rand32 를 6회 호출하여 초기 편향(시드에 따른 저품질 비트) 을 제거(crank).
 *
 * 실행 컨텍스트: 잡 초기화 시 1회. 이후 상태는 잡 스레드가 소유.
 * 호출자: init_rand / init_rand_seed.
 */
static void __init_rand32(struct taus88_state *state, unsigned int seed)
{
	int cranks = 6;		/* [한국어] 워밍업 반복 횟수 (시드 품질 저하를 제거하는 경험적 값) */

#define LCG(x, seed)  ((x) * 69069 ^ (seed))	/* [한국어] Classical LCG 계수 69069 (Marsaglia). ^seed 로 사용자 입력 섞기 */

	/* [한국어] LCG 로 시드를 확산하여 3개의 상태 변수를 초기화 (제약: s1>1, s2>7, s3>15) */
	state->s1 = __seed(LCG((2^31) + (2^17) + (2^7), seed), 1);	/* [한국어] 주의: (2^31) 은 C 의 비트 XOR (29), (2^17)=19, (2^7)=5. 실제로는 29+19+5=53 이 되지만 원본 코드 보존 */
	state->s2 = __seed(LCG(state->s1, seed), 7);			/* [한국어] s1 을 재투입하여 상관 감소 */
	state->s3 = __seed(LCG(state->s2, seed), 15);			/* [한국어] 체인 마지막 단계 */

	/* [한국어] 6회 워밍업: __rand32 는 rand.h 의 인라인 함수 (본 파일에서 참조) */
	while (cranks--)
		__rand32(state);
}

/*
 * [한국어] __init_rand64 - 64비트 Taus258 상태를 시드로 초기화
 *
 * @state: 초기화할 taus258_state 구조체 (s1..s5)
 * @seed:  사용자 시드 (64비트)
 *
 * 5개 상태에 대해 LCG64 확산 → __seed(… , 1/7/15/33/49) 제약 → 6회 crank.
 * 상태가 5 개이고 각 비트 제약이 1/7/15/33/49 로 커지는 이유: 각 z_j 의 상위
 * k_j 비트가 0 이면 안 된다는 L'Ecuyer 조건의 64비트 확장.
 *
 * 실행 컨텍스트: 잡 초기화. 주기 ≈ 2^258 로 PiB 규모 실험도 재반복 없이 가능.
 */
void __init_rand64(struct taus258_state *state, uint64_t seed)
{
	int cranks = 6;		/* [한국어] 워밍업 반복 횟수 */

#define LCG64(x, seed)  ((x) * 6906969069ULL ^ (seed))	/* [한국어] 64비트 LCG 계수 (GCC 공통). ULL 접미사로 64비트 곱셈 보장 */

	/* [한국어] 64비트 LCG 로 5개 상태 변수를 체인 초기화 */
	state->s1 = __seed(LCG64((2^31) + (2^17) + (2^7), seed), 1);	/* [한국어] s1 하한 1 */
	state->s2 = __seed(LCG64(state->s1, seed), 7);			/* [한국어] s2 하한 7 */
	state->s3 = __seed(LCG64(state->s2, seed), 15);			/* [한국어] s3 하한 15 */
	state->s4 = __seed(LCG64(state->s3, seed), 33);			/* [한국어] s4 하한 33 — 64비트 확장용 */
	state->s5 = __seed(LCG64(state->s4, seed), 49);			/* [한국어] s5 하한 49 */

	/* [한국어] 6회 워밍업: rand.h 의 __rand64 호출로 초기 편향 제거 */
	while (cranks--)
		__rand64(state);
}

/*
 * [한국어] init_rand - 기본 시드(1) 로 난수 생성기 초기화
 *
 * @state: 통합 난수 상태(frand_state)
 * @use64: true → Taus258(64비트), false → Taus88(32비트)
 *
 * 시드를 1 로 고정하므로 같은 빌드/플래그 조합에서 매번 동일한 시퀀스가 나온다.
 * fio 초기화 시 FIO_RAND_SEED 와 조합되기 전 단계에서 "안전한 기본값" 으로 호출되거나,
 * 사용자 옵션이 없을 때 폴백으로 사용된다.
 *
 * 호출 체인: fio 초기화(init.c 등) → [init_rand] → __init_rand32/__init_rand64.
 */
void init_rand(struct frand_state *state, bool use64)
{
	state->use64 = use64;		/* [한국어] 생성기 버전을 상태에 기록. 이후 fio_rand64/32 분기에 사용 */

	if (!use64)
		__init_rand32(&state->state32, 1);	/* [한국어] Taus88, 시드 1 */
	else
		__init_rand64(&state->state64, 1);	/* [한국어] Taus258, 시드 1 */
}

/*
 * [한국어] init_rand_seed - 지정된 시드로 난수 생성기 초기화
 *
 * @state: 통합 난수 상태(frand_state)
 * @seed:  사용자 시드 (옵션 `--randseed=` 또는 FIO_RAND_SEED 매크로에서 전달)
 * @use64: true → 64비트, false → 32비트
 *
 * fio 가 지원하는 두 난수 품질 모드 (`--random_generator=tausworthe` 32비트 /
 * `tausworthe64` 64비트) 의 분기점이다. 32비트는 속도 이점, 64비트는 주기 이점.
 *
 * 호출 체인: init.c (init_random_map, init_random_state_prev 등) → [init_rand_seed].
 */
void init_rand_seed(struct frand_state *state, uint64_t seed, bool use64)
{
	state->use64 = use64;		/* [한국어] 분기 기록 */

	if (!use64)
		__init_rand32(&state->state32, (unsigned int) seed);	/* [한국어] 32비트 주입: 상위 32비트는 버림 (use64=false 이므로 의도적) */
	else
		__init_rand64(&state->state64, seed);			/* [한국어] 64비트 시드 그대로 전달 */
}

/*
 * [한국어] __fill_random_buf_small - 작은 버퍼를 해시 체인으로 난수 채움 (잔여/소형 경로)
 *
 * @buf: 채울 버퍼 포인터
 * @len: 버퍼 길이 (바이트)
 * @seed: 시작 시드
 *
 * 동작: seed 를 __hash_u64 로 반복 변환하며 8바이트씩 기록. 마지막 남은 1~7
 * 바이트는 __builtin_memcpy 로 바이트 단위 저장. 16-way 경로보다 느리지만
 * 호출 시점의 분기 없이 어떤 길이든 처리 가능.
 *
 * 사용 맥락: __fill_random_buf 내부의 "16버킷 배수가 아닌 잔여" 처리, 또는
 * 매우 작은(수십 바이트) 버퍼 단독 호출.
 */
void __fill_random_buf_small(void *buf, unsigned int len, uint64_t seed)
{
	uint64_t *b = buf;				/* [한국어] 8바이트(uint64_t) 단위로 쓰기 위해 캐스팅 */
	uint64_t *e = b  + len / sizeof(*b);		/* [한국어] 8바이트 경계까지의 종점 */
	unsigned int rest = len % sizeof(*b);		/* [한국어] 8바이트 미만 잔여 */

	/* [한국어] 8바이트 단위로 해시 체인을 통해 버퍼 채움 */
	for (; b != e; ++b) {				/* [한국어] 포인터 비교로 종점 판정 */
		*b = seed;				/* [한국어] 현재 시드 값을 그대로 저장 */
		seed = __hash_u64(seed);		/* [한국어] 다음 시드 = 해시(시드). 비선형 해시로 품질 확보 */
	}

	/* [한국어] 8바이트 미만의 나머지 부분 처리 */
	if (fio_unlikely(rest))				/* [한국어] 대부분 0 (8B 배수가 흔함) 이므로 분기 힌트 not-taken */
		__builtin_memcpy(e, &seed, rest);	/* [한국어] 마지막 시드 값에서 rest 바이트만 복사 */
}

/*
 * [한국어] __fill_random_buf - 16-way 시드 버킷 인터리브로 대용량 버퍼를 고속 채움
 *
 * @buf: 채울 버퍼
 * @len: 버퍼 길이 (바이트)
 * @seed: 시작 시드
 *
 * 최적화 원리:
 *   1. 16 개의 독립 시드(s[0..15]) 를 seed * prime[i] 로 서로 다르게 초기화.
 *   2. 16 개의 시드 체인을 "한 바퀴당 한 바이트씩" 번갈아 전진하여 인터리브로 씀.
 *      이렇게 하면 CPU 의 슈퍼스칼라 ILP 를 활용해 16개의 해시가 병렬로 처리되며
 *      메모리 쓰기도 순차적(스트리밍 스토어 친화) 이 된다.
 *   3. 16-way 배수에 맞지 않는 잔여는 __fill_random_buf_small 로 처리.
 *
 * 성능: 단순 루프 대비 2~4배 처리량. `buffer_compress_percentage=0` 의 대용량
 * 쓰기 패턴에서 CPU 병목을 해소하기 위해 설계됨.
 *
 * 호출자: fill_random_buf, fill_random_buf_percentage (내부의 난수 청크).
 * 호출 대상: __hash_u64, __fill_random_buf_small (잔여 처리).
 */
void __fill_random_buf(void *buf, unsigned int len, uint64_t seed)
{
	/* [한국어] 16개의 소수 배열 - 각 버킷 시드를 곱셈으로 분산시키는 계수.
	 * "1" 도 포함되어 있어 첫 버킷은 seed 그대로(=__fill_random_buf_small 의 시작과 동일) */
	static uint64_t prime[] = {1, 2, 3, 5, 7, 11, 13, 17,
				   19, 23, 29, 31, 37, 41, 43, 47};
	uint64_t *b, *e, s[CONFIG_SEED_BUCKETS];	/* [한국어] s[] 는 스택에 올라가는 16개 독립 시드 */
	unsigned int rest;
	int p;

	/*
	 * Calculate the max index which is multiples of the seed buckets.
	 * [한국어] 버킷 수의 배수로 정렬된 최대 인덱스를 계산 (이후 루프가 16 워드씩 전진)
	 */
	rest = (len / sizeof(*b) / CONFIG_SEED_BUCKETS) * CONFIG_SEED_BUCKETS;

	b = buf;					/* [한국어] 현재 쓰기 포인터 */
	e = b + rest;					/* [한국어] 16-way 루프의 종점 */

	rest = len - (rest * sizeof(*b));		/* [한국어] 16-way 로 처리 못 하는 잔여 바이트 수 */

	/* [한국어] 각 버킷을 seed * 소수로 초기화하여 독립적인 해시 체인 생성 */
	for (p = 0; p < CONFIG_SEED_BUCKETS; p++)
		s[p] = seed * prime[p];			/* [한국어] 각 버킷마다 서로 다른 시작점 */

	/* [한국어] 16개 버킷을 인터리브하여 버퍼를 채움 (캐시 친화 + ILP) */
	for (; b != e; b += CONFIG_SEED_BUCKETS) {	/* [한국어] 한 바퀴당 16 워드 = 128바이트 기록 */
		for (p = 0; p < CONFIG_SEED_BUCKETS; ++p) {
			b[p] = s[p];			/* [한국어] p 번째 버킷 값을 버퍼에 기록 */
			s[p] = __hash_u64(s[p]);	/* [한국어] 다음 라운드용 시드 전진 */
		}
	}

	/* [한국어] 버킷 배수에 맞지 않는 나머지 부분을 작은 버퍼 함수로 처리
	 * (첫 번째 버킷 s[0] 을 재사용; 품질상 다른 버킷이어도 무방) */
	__fill_random_buf_small(b, rest, s[0]);
}

/*
 * [한국어] fill_random_buf - frand_state 에서 시드를 뽑아 버퍼를 난수로 채움
 *
 * @fs: 난수 상태 (잡의 td->buf_state 등에서 전달)
 * @buf: 채울 버퍼 (쓰기 I/O 시 커널로 전달될 버퍼)
 * @len: 버퍼 길이
 * @return: 이번에 사용된 시드. 검증 시 같은 시드로 재생성하여 bit-exact 비교에 사용.
 *
 * 시드 반환은 verify 경로에서 중요하다: fio 가 쓰기 시 생성한 난수 패턴을
 * 읽기 검증 시 재현할 수 있어야 하므로, verify_header 에 이 시드가 기록된다.
 *
 * 호출 체인: io_u.c (fill_io_buffer 등) → [fill_random_buf] → __fill_random_buf.
 */
uint64_t fill_random_buf(struct frand_state *fs, void *buf,
			 unsigned int len)
{
	uint64_t r = __get_next_seed(fs);	/* [한국어] rand.h 의 인라인 헬퍼: use64 분기하여 다음 난수 값(=시드) 획득 */

	__fill_random_buf(buf, len, r);		/* [한국어] 실제 채움 */
	return r;				/* [한국어] 시드 반환하여 검증 시 동일 시퀀스 재현 가능 */
}

/*
 * [한국어] __fill_random_buf_percentage - 난수와 패턴을 비율에 따라 혼합하여 버퍼 채움
 *
 * @seed: 난수 생성 시드
 * @buf: 채울 버퍼
 * @percentage: 압축 가능한(패턴/0) 데이터 비율 (0~100)
 * @segment: 한 "난수+패턴" 세그먼트의 길이. 너무 크면 len 으로 클램프.
 * @len: 전체 버퍼 길이
 * @pattern: 압축 가능 영역에 채울 패턴 (NULL 이면 0 으로 채움)
 * @pbytes: 패턴 바이트 수 (cpy_pattern 의 pattern_len)
 *
 * `buffer_compress_percentage` 옵션을 구현한다. 예: percentage=70, segment=4KB
 * 이면 각 4KB 세그먼트마다 앞 1.2KB 는 난수(비압축성), 뒤 2.8KB 는 패턴(압축성).
 * 이 혼합으로 압축 코덱이 지정된 비율로 압축 가능한 데이터를 생성할 수 있다.
 * NVMe 의 압축/중복제거 성능 평가용.
 *
 * 특수 케이스:
 *   - percentage == 100: 전체를 패턴 또는 0 으로 채우고 즉시 반환 (난수 없음).
 *   - segment > len: segment 를 len 으로 클램프 (단일 세그먼트로 처리).
 *
 * 호출 체인: fill_random_buf_percentage → [__fill_random_buf_percentage]
 *                                        → __fill_random_buf / cpy_pattern / memset.
 */
void __fill_random_buf_percentage(uint64_t seed, void *buf,
				  unsigned int percentage,
				  unsigned int segment, unsigned int len,
				  char *pattern, unsigned int pbytes)
{
	unsigned int this_len;		/* [한국어] 이번 반복에서 채울 바이트 수 */

	if (percentage == 100) {	/* [한국어] 전부 압축 가능 (난수 영역 없음) */
		if (pbytes)
			(void)cpy_pattern(pattern, pbytes, buf, len);	/* [한국어] 사용자 패턴으로 채움 (반환값 무시) */
		else
			memset(buf, 0, len);		/* [한국어] 패턴 없으면 0 으로 채움 (최대 압축) */
		return;					/* [한국어] 더 할 일 없음 */
	}

	if (segment > len)
		segment = len;		/* [한국어] 세그먼트가 전체보다 크면 클램프 — 단일 세그먼트 처리 */

	while (len) {			/* [한국어] 세그먼트 단위로 반복 (난수 파트 + 패턴 파트) */
		/*
		 * Fill random chunk
		 */
		/* [한국어] 난수 파트 길이 = segment * (100 - percentage) / 100 (비압축성 비율) */
		this_len = ((unsigned long long)segment * (100 - percentage)) / 100;	/* [한국어] unsigned long long 캐스트로 32비트 overflow 방지 */
		if (this_len > len)
			this_len = len;			/* [한국어] 남은 전체보다 크면 잘라냄 */

		__fill_random_buf(buf, this_len, seed);	/* [한국어] 난수 파트 기록 */

		len -= this_len;			/* [한국어] 남은 총량 감소 */
		if (!len)
			break;				/* [한국어] 버퍼 끝 도달 */
		buf += this_len;			/* [한국어] 쓰기 포인터 전진 */
		this_len = segment - this_len;		/* [한국어] 패턴 파트 길이 = segment - 난수 파트 */

		if (this_len > len)
			this_len = len;			/* [한국어] 남은 길이로 클램프 */
		else if (len - this_len <= sizeof(long))
			this_len = len;			/* [한국어] sizeof(long) 이하 잔여는 합쳐서 처리 (매우 짧은 난수 후속을 피함) */

		if (pbytes)
			(void)cpy_pattern(pattern, pbytes, buf, this_len);	/* [한국어] 패턴으로 채움 */
		else
			memset(buf, 0, this_len);	/* [한국어] 패턴 없으면 0 */

		len -= this_len;			/* [한국어] 남은 총량 감소 */
		buf += this_len;			/* [한국어] 쓰기 포인터 전진 */
	}
}

/*
 * [한국어] fill_random_buf_percentage - frand_state 에서 시드 뽑아 비율 혼합 채움
 *
 * 파라미터는 __fill_random_buf_percentage 와 동일(첫 인자만 frand_state).
 * 반환값은 사용된 시드(검증 시 재현용).
 *
 * 호출 체인: io_u.c (fill_io_buffer + buffer_compress_percentage>0) →
 *            [fill_random_buf_percentage] → __fill_random_buf_percentage.
 */
uint64_t fill_random_buf_percentage(struct frand_state *fs, void *buf,
				    unsigned int percentage,
				    unsigned int segment, unsigned int len,
				    char *pattern, unsigned int pbytes)
{
	uint64_t r = __get_next_seed(fs);	/* [한국어] 시드 획득 (use64 분기) */

	__fill_random_buf_percentage(r, buf, percentage, segment, len,
					pattern, pbytes);	/* [한국어] 실제 혼합 채움 */
	return r;				/* [한국어] 검증 재현용 시드 반환 */
}
