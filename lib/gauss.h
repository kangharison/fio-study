/*
 * [한국어 설명] 가우시안(정규) 분포 헤더 (gauss.h)
 *
 * === 파일의 역할 ===
 * 가우시안 분포 생성기의 상태 구조체와 API를 정의한다.
 * 정규 분포 기반의 랜덤 I/O 오프셋 생성 인터페이스를 제공한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct gauss_state: frand_state r(내부 PRNG 상태), nranges(전체 블록 범위),
 *   stddev(표준편차, 0이면 순수 CLT만 사용), rand_off(중심 오프셋),
 *   disable_hash(해시 비활성화 플래그)를 포함
 * - gauss_init: 범위, 편차, 중심값, 시드로 초기화
 * - gauss_next: 다음 가우시안 분포 값을 반환
 *
 * === fio에서의 사용 ===
 * io_u.c에서 --random_distribution=normal 옵션이 설정되면 이 구조체가 초기화되어
 * I/O 오프셋 선택 시 gauss_next()를 호출하여 정규 분포를 따르는 오프셋을 생성한다.
 */

#ifndef FIO_GAUSS_H
#define FIO_GAUSS_H

#include <inttypes.h>
#include "rand.h"

struct gauss_state {
	struct frand_state r;		/* [한국어] 내부 Tausworthe PRNG 상태 */
	uint64_t nranges;		/* [한국어] 전체 블록 범위 (오프셋 수) */
	unsigned int stddev;		/* [한국어] 표준편차 (dev%로부터 계산된 블록 수, 0이면 순수 CLT) */
	unsigned int rand_off;		/* [한국어] 분포 중심 오프셋 */
	bool disable_hash;		/* [한국어] true이면 해시 분산 비활성화 */
};

void gauss_init(struct gauss_state *gs, unsigned long nranges, double dev,
		double center, unsigned int seed);
unsigned long long gauss_next(struct gauss_state *gs);
void gauss_disable_hash(struct gauss_state *gs);

#endif
