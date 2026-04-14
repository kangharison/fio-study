/*
 * [한국어 설명] 가우시안(정규) 분포 생성기 (gauss.c)
 *
 * === 파일의 역할 ===
 * 가우시안(정규) 분포를 따르는 난수를 생성한다.
 * 중심 부근에 접근이 집중되고 양쪽 끝으로 갈수록 확률이 감소하는
 * 종형(bell curve) 분포의 I/O 패턴을 시뮬레이션한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - 중심극한정리(CLT) 기반: GAUSS_ITERS(12)개의 균등 난수를 합산하여 정규 분포 근사
 * - gauss_dev: 표준편차(stddev) 범위 내에서 추가 편차를 생성하여 분포 폭을 조정
 * - struct gauss_state: frand_state(난수 상태), nranges(범위), stddev(표준편차),
 *   rand_off(중심 오프셋), disable_hash(해시 비활성화 플래그) 포함
 * - __hash_u64: 생성된 값을 해시하여 공간적 편향을 분산
 *
 * === fio에서의 사용 ===
 * --random_distribution=normal:dev 옵션으로 활성화된다. dev 값은 백분율로 표현되며,
 * 작을수록 중심에 더 집중된 접근 패턴을 생성한다. SSD 웨어 레벨링 테스트 등에 유용하다.
 */

#include <math.h>
#include <string.h>
#include "../hash.h"
#include "gauss.h"

/* [한국어] CLT(중심극한정리) 합산 횟수: 12개의 균등 난수를 합산하면 정규 분포에 근사 */
#define GAUSS_ITERS	12

/*
 * [한국어] gauss_dev - 표준편차 범위 내에서 추가 편차를 생성
 *
 * @gs: 가우시안 상태
 * @return: [-stddev/2, +stddev/2) 범위의 균등 분포 편차값
 *
 * 순수 CLT 분포에 추가적인 분산을 더하여 꼬리(tail) 부분을 확장하는 역할.
 * stddev가 0이면 이 함수는 0을 반환하여 순수 CLT만 사용하게 된다.
 */
static int gauss_dev(struct gauss_state *gs)
{
	unsigned int r;
	int vr;

	if (!gs->stddev)
		return 0;

	r = __rand(&gs->r);
	vr = gs->stddev * (r / (FRAND32_MAX + 1.0));

	return vr - gs->stddev / 2;
}

/*
 * [한국어] gauss_next - 가우시안 분포를 따르는 다음 값을 생성
 *
 * @gs: 가우시안 상태
 * @return: [0, nranges-1] 범위의 정규 분포 값
 *
 * 1. 12개의 균등 난수를 합산하여 CLT로 정규 분포를 근사
 * 2. stddev가 설정되어 있으면 추가 편차를 더함 (범위 초과 방지 포함)
 * 3. 해시로 공간적 편향을 분산시키고 중심 오프셋을 더함
 *
 * 호출 체인: io_u.c (get_next_rand_offset) → [gauss_next]
 */
unsigned long long gauss_next(struct gauss_state *gs)
{
	unsigned long long sum = 0;
	int i;

	/* [한국어] CLT: 12개 균등 난수의 합 → 평균을 취하면 정규 분포에 근사 */
	for (i = 0; i < GAUSS_ITERS; i++)
		sum += __rand(&gs->r) % (gs->nranges + 1);

	sum = (sum + GAUSS_ITERS - 1) / GAUSS_ITERS;

	if (gs->stddev) {
		int dev = gauss_dev(gs);

		/* [한국어] 편차를 더했을 때 범위를 초과하면 반으로 줄여 범위 내로 맞춤 */
		while (dev + sum >= gs->nranges)
			dev /= 2;
		sum += dev;
	}

	/* [한국어] 해시로 연속적인 CLT 값을 공간적으로 분산 */
	if (!gs->disable_hash)
		sum = __hash_u64(sum);

	return (sum + gs->rand_off) % gs->nranges;
}

/*
 * [한국어] gauss_init - 가우시안 분포 생성기 초기화
 *
 * @gs: 초기화할 가우시안 상태
 * @nranges: 전체 블록 범위
 * @dev: 표준편차 백��율 (0이면 순수 CLT, 100이면 nranges/2)
 * @center: 분포 중심 (0.0~1.0, -1이면 기본 중심)
 * @seed: 난수 시드
 *
 * 호출 체인: init.c (init_rand_distribution) → [gauss_init]
 */
void gauss_init(struct gauss_state *gs, unsigned long nranges, double dev,
		double center, unsigned int seed)
{
	memset(gs, 0, sizeof(*gs));
	init_rand_seed(&gs->r, seed, 0);
	gs->nranges = nranges;

	if (dev != 0.0) {
		gs->stddev = ceil((double)(nranges * dev) / 100.0);
		if (gs->stddev > nranges / 2)
			gs->stddev = nranges / 2;
	}
	if (center == -1)
	  gs->rand_off = 0;
	else
	  gs->rand_off = nranges * (center - 0.5);
}

void gauss_disable_hash(struct gauss_state *gs)
{
	gs->disable_hash = true;
}
