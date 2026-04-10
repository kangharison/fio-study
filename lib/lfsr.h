/*
 * [한국어 설명] 선형 피드백 시프트 레지스터(LFSR) 헤더 (lfsr.h)
 *
 * === 파일의 역할 ===
 * LFSR 난수 생성기의 상태 구조체와 API를 정의한다.
 * 중복 없는 의사 난수 시퀀스 생성을 위한 인터페이스를 제공한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - FIO_MAX_TAPS(6): LFSR 탭의 최대 개수
 * - struct fio_lfsr: xormask(XOR 피드백 마스크), last_val(현재 상태값),
 *   cached_bit(최상위 비트 캐시), max_val(최대 유효 값),
 *   num_vals(생성된 값 카운터), spin(스텝 배수), cycle_length(부분 순환 길이) 포함
 * - lfsr_init: 범위, 시드, spin 값으로 초기화
 * - lfsr_next: 다음 고유 오프셋을 생성 (범위 소진 시 1 반환)
 * - lfsr_reset: 새 시드로 LFSR 상태 재설정
 *
 * === fio에서의 사용 ===
 * io_u.c에서 --norandommap 모드의 I/O 오프셋 생성에 사용된다.
 * 비트맵(axmap) 대신 LFSR을 사용하면 메모리를 절약하면서도 전체 범위 커버리지를 보장한다.
 */

#ifndef FIO_LFSR_H
#define FIO_LFSR_H

#include <inttypes.h>

#define FIO_MAX_TAPS	6

struct lfsr_taps {
	unsigned int length;
	unsigned int taps[FIO_MAX_TAPS];
};


struct fio_lfsr {
	uint64_t xormask;
	uint64_t last_val;
	uint64_t cached_bit;
	uint64_t max_val;
	uint64_t num_vals;
	uint64_t cycle_length;
	uint64_t cached_cycle_length;
	unsigned int spin;
};

int lfsr_next(struct fio_lfsr *fl, uint64_t *off);
int lfsr_init(struct fio_lfsr *fl, uint64_t size,
	      uint64_t seed, unsigned int spin);
int lfsr_reset(struct fio_lfsr *fl, uint64_t seed);

#endif
