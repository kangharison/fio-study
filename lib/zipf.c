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

#define ZIPF_MAX_GEN	10000000UL

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

void zipf_init(struct zipf_state *zs, uint64_t nranges, double theta,
	       double center, unsigned int seed)
{
	shared_rand_init(zs, nranges, center, seed);

	zs->theta = theta;
	zs->zeta2 = pow(1.0, zs->theta) + pow(0.5, zs->theta);

	zipf_update(zs);
}

uint64_t zipf_next(struct zipf_state *zs)
{
	double alpha, eta, rand_uni, rand_z;
	unsigned long long n = zs->nranges;
	unsigned long long val;

	alpha = 1.0 / (1.0 - zs->theta);
	eta = (1.0 - pow(2.0 / n, 1.0 - zs->theta)) / (1.0 - zs->zeta2 / zs->zetan);

	rand_uni = (double) __rand(&zs->rand) / (double) FRAND32_MAX;
	rand_z = rand_uni * zs->zetan;

	if (rand_z < 1.0)
		val = 1;
	else if (rand_z < (1.0 + pow(0.5, zs->theta)))
		val = 2;
	else
		val = 1 + (unsigned long long)(n * pow(eta*rand_uni - eta + 1.0, alpha));

	val--;

	if (!zs->disable_hash)
		val = __hash_u64(val);

	return (val + zs->rand_off) % zs->nranges;
}

void pareto_init(struct zipf_state *zs, uint64_t nranges, double h,
		 double center, unsigned int seed)
{
	shared_rand_init(zs, nranges, center, seed);
	zs->pareto_pow = log(h) / log(1.0 - h);
}

uint64_t pareto_next(struct zipf_state *zs)
{
	double rand = (double) __rand(&zs->rand) / (double) FRAND32_MAX;
	unsigned long long n;

	n = (zs->nranges - 1) * pow(rand, zs->pareto_pow);

	if (!zs->disable_hash)
		n = __hash_u64(n);

	return (n + zs->rand_off)  % zs->nranges;
}

void zipf_disable_hash(struct zipf_state *zs)
{
	zs->disable_hash = true;
}
