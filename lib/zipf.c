/*
 * [한국어 설명] Zipf 및 Pareto 분포 생성기 (zipf.c)
 *
 * === 파일의 역할 ===
 * Zipf 분포와 Pareto 분포를 따르는 난수를 생성한다.
 * 이 두 분포는 비균등(non-uniform) 랜덤 I/O 패턴을 시뮬레이션하는 데 사용되며,
 * 소수의 핫 영역에 접근이 집중되는 실제 워크로드를 모델링할 수 있다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct zipf_state: nranges, theta, zetan, pareto_pow 등을 포함하는 분포 상태
 * - Zipf: zeta 함수를 사전 계산(최대 10M 반복)하고, 역변환 샘플링으로 값 생성
 * - Pareto: pow(rand, pareto_pow) 공식으로 멱법칙(power-law) 분포 생성
 * - __hash_u64: 생성된 값을 해시하여 공간적 편향을 분산시킴 (disable_hash로 비활성화 가능)
 * - rand_off: center 파라미터로 분포의 중심 오프셋을 지정
 *
 * === fio에서의 사용 ===
 * --random_distribution=zipf:theta 또는 pareto:h 옵션으로 활성화된다.
 * theta가 클수록 소수 블록에 접근이 집중되고, Pareto의 h 값이 작을수록
 * 더 균등한 분포를 생성한다. 데이터베이스나 캐시 워크로드 벤치마크에 유용하다.
 */

#include <math.h>
#include <string.h>
#include "zipf.h"
#include "../minmax.h"
#include "../hash.h"

/* [한국어] zeta 함수 사전 계산의 최대 반복 횟수 (성능과 정밀도의 트레이드오프) */
#define ZIPF_MAX_GEN	10000000UL

/*
 * [한국어] zipf_update - zeta 함수를 사전 계산하여 zs->zetan에 저장
 *
 * Zipf 분포의 정규화 상수인 zeta(N, theta) = sum(1/i^theta, i=1..N)을 계산한다.
 * 범위가 클 때 정밀도를 위해 최대 10M개까지 합산하며, 이 값이 이후
 * zipf_next()에서 역변환 샘플링의 기준으로 사용된다.
 */
static void zipf_update(struct zipf_state *zs)
{
	uint64_t to_gen;
	unsigned int i;

	/*
	 * It can become very costly to generate long sequences. Just cap it at
	 * 10M max, that should be doable in 1-2s on even slow machines.
	 * Precision will take a slight hit, but nothing major.
	 */
	to_gen = min(zs->nranges, (uint64_t) ZIPF_MAX_GEN);

	for (i = 0; i < to_gen; i++)
		zs->zetan += pow(1.0 / (double) (i + 1), zs->theta);
}

/*
 * [한국어] shared_rand_init - Zipf/Pareto 공통 초기화
 *
 * @zs: 분포 상태 구조체
 * @nranges: 전체 블록 범위 (오프셋 수)
 * @center: 분포의 중심 위치 (0.0~1.0, -1이면 랜덤)
 * @seed: 난수 시드
 *
 * 내부 PRNG를 초기화하고, center 파라미터에 따라 분포의 중심 오프셋을 설정한다.
 */
static void shared_rand_init(struct zipf_state *zs, uint64_t nranges,
			     double center, unsigned int seed)
{
	memset(zs, 0, sizeof(*zs));
	zs->nranges = nranges;

	init_rand_seed(&zs->rand, seed, 0);
	zs->rand_off = __rand(&zs->rand);
	if (center != -1)
		zs->rand_off = nranges * center;
}

/*
 * [한국어] zipf_init - Zipf 분포 생성기 초기화
 *
 * @zs: 초기화할 분포 상태
 * @nranges: 전체 블록 범위
 * @theta: Zipf 매개변수 (>1일수록 소수 블록에 집중, 1에 가까우면 균등)
 * @center: 분포 중심 (-1이면 랜덤)
 * @seed: 난수 시드
 *
 * zeta2(H(2,theta))와 zetan(H(N,theta))을 사전 계산한다.
 * 호출 체인: init.c (init_rand_distribution) → [zipf_init]
 */
void zipf_init(struct zipf_state *zs, uint64_t nranges, double theta,
	       double center, unsigned int seed)
{
	shared_rand_init(zs, nranges, center, seed);

	zs->theta = theta;
	zs->zeta2 = pow(1.0, zs->theta) + pow(0.5, zs->theta);

	zipf_update(zs);
}

/*
 * [한국어] zipf_next - Zipf 분포를 따르는 다음 값을 생성
 *
 * @zs: 분포 상태
 * @return: [0, nranges-1] 범위의 Zipf 분포 값
 *
 * 역변환 샘플링 알고리즘을 사용한다:
 * 1. 균등 난수 rand_uni를 생성
 * 2. rand_z = rand_uni * zetan으로 Zipf CDF 역함수를 적용
 * 3. rand_z 구간에 따라 val을 결정 (1, 2, 또는 연속 근사)
 * 4. 해시로 공간적 편향을 분산시키고 중심 오프셋을 더함
 *
 * 호출 체인: io_u.c (get_next_rand_offset) → [zipf_next]
 */
uint64_t zipf_next(struct zipf_state *zs)
{
	double alpha, eta, rand_uni, rand_z;
	unsigned long long n = zs->nranges;
	unsigned long long val;

	/* [한국어] alpha: 역변환의 지수, eta: 연속 근사를 위한 보정 계수 */
	alpha = 1.0 / (1.0 - zs->theta);
	eta = (1.0 - pow(2.0 / n, 1.0 - zs->theta)) / (1.0 - zs->zeta2 / zs->zetan);

	/* [한국어] [0, 1) 균등 난수를 생성하고 zetan으로 스케일링 */
	rand_uni = (double) __rand(&zs->rand) / (double) FRAND32_MAX;
	rand_z = rand_uni * zs->zetan;

	/* [한국어] Zipf CDF의 역함수: 구간별로 순위(rank)를 결정 */
	if (rand_z < 1.0)
		val = 1;
	else if (rand_z < (1.0 + pow(0.5, zs->theta)))
		val = 2;
	else
		val = 1 + (unsigned long long)(n * pow(eta*rand_uni - eta + 1.0, alpha));

	val--;

	/* [한국어] 해시로 연속적인 순위 값을 공간적으로 분산 */
	if (!zs->disable_hash)
		val = __hash_u64(val);

	return (val + zs->rand_off) % zs->nranges;
}

/*
 * [한국어] pareto_init - Pareto 분포 생성기 초기화
 *
 * @zs: 초기화할 분포 상태
 * @nranges: 전체 블록 범위
 * @h: Pareto 매개변수 (0에 가까울수록 균등, 1에 가까울수록 편중)
 * @center: 분포 중심 (-1이면 랜덤)
 * @seed: 난수 시드
 *
 * pareto_pow = log(h) / log(1-h)를 사전 계산한다.
 * 호출 체인: init.c (init_rand_distribution) → [pareto_init]
 */
void pareto_init(struct zipf_state *zs, uint64_t nranges, double h,
		 double center, unsigned int seed)
{
	shared_rand_init(zs, nranges, center, seed);
	/* [한국어] 멱법칙 지수를 사전 계산 */
	zs->pareto_pow = log(h) / log(1.0 - h);
}

/*
 * [한국어] pareto_next - Pareto 분포를 따르는 다음 값을 생성
 *
 * @zs: 분포 상태
 * @return: [0, nranges-1] 범위의 Pareto 분포 값
 *
 * pow(균등난수, pareto_pow)로 멱법칙 분포를 생성한다.
 * 호출 체인: io_u.c (get_next_rand_offset) → [pareto_next]
 */
uint64_t pareto_next(struct zipf_state *zs)
{
	double rand = (double) __rand(&zs->rand) / (double) FRAND32_MAX;
	unsigned long long n;

	/* [한국어] 멱법칙: rand^pareto_pow로 편중된 값 생성 */
	n = (zs->nranges - 1) * pow(rand, zs->pareto_pow);

	if (!zs->disable_hash)
		n = __hash_u64(n);

	return (n + zs->rand_off)  % zs->nranges;
}

void zipf_disable_hash(struct zipf_state *zs)
{
	zs->disable_hash = true;
}
