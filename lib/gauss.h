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
	struct frand_state r;
	uint64_t nranges;
	unsigned int stddev;
	unsigned int rand_off;
	bool disable_hash;
};

void gauss_init(struct gauss_state *gs, unsigned long nranges, double dev,
		double center, unsigned int seed);
unsigned long long gauss_next(struct gauss_state *gs);
void gauss_disable_hash(struct gauss_state *gs);

#endif
