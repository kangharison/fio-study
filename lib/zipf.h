/*
 * [한국어 설명] Zipf 및 Pareto 분포 헤더 (zipf.h)
 *
 * === 파일의 역할 ===
 * Zipf 분포와 Pareto 분포 생성기의 상태 구조체와 API를 정의한다.
 * 두 분포 모두 동일한 zipf_state 구조체를 공유하여 메모리를 절약한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct zipf_state: nranges(범위 크기), theta(Zipf 매개변수), zeta2/zetan(사전 계산된 zeta 값),
 *   pareto_pow(Pareto 지수), frand_state(내부 난수 상태), rand_off(중심 오프셋),
 *   disable_hash(해시 비활성화 플래그)를 포함
 * - zipf_init/zipf_next: Zipf 분포 초기화 및 다음 값 생성
 * - pareto_init/pareto_next: Pareto 분포 초기화 및 다음 값 생성
 *
 * === fio에서의 사용 ===
 * io_u.c에서 랜덤 I/O 오프셋 생성 시 사용된다. --random_distribution 옵션이
 * zipf 또는 pareto로 설정되면 이 구조체가 초기화되어 오프셋 선택에 활용된다.
 */

#ifndef FIO_ZIPF_H
#define FIO_ZIPF_H

#include <inttypes.h>
#include "rand.h"
#include "types.h"

struct zipf_state {
	uint64_t nranges;	/* [한국어] 전체 블록 범위 (오프셋 수) */
	double theta;		/* [한국어] Zipf 매개변수 (>1: 편중 증가, =1: 균등에 가까움) */
	double zeta2;		/* [한국어] H(2, theta) = 1 + 0.5^theta (사전 계산된 zeta 값) */
	double zetan;		/* [한국어] H(N, theta) = sum(1/i^theta) (정규화 상수) */
	double pareto_pow;	/* [한국어] Pareto 지수 = log(h)/log(1-h) (Pareto 모드 전용) */
	struct frand_state rand;	/* [한국어] 내부 Tausworthe PRNG 상태 */
	uint64_t rand_off;	/* [한국어] 분포의 중심 오프셋 (center 파라미터로 설정) */
	bool disable_hash;	/* [한국어] true이면 해시를 적용하지 않아 순위 기반 접근 패턴 유지 */
};

void zipf_init(struct zipf_state *zs, uint64_t nranges, double theta,
	       double center, unsigned int seed);
uint64_t zipf_next(struct zipf_state *zs);

void pareto_init(struct zipf_state *zs, uint64_t nranges, double h,
		 double center, unsigned int seed);
uint64_t pareto_next(struct zipf_state *zs);
void zipf_disable_hash(struct zipf_state *zs);

#endif
