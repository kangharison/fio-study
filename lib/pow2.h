/*
 * [한국어 설명] 2의 거듭제곱 판별 유틸리티 (pow2.h)
 *
 * === 파일의 역할 ===
 * 주어진 64비트 값이 2의 거듭제곱인지 판별하는 is_power_of_2() 함수를 제공한다.
 * val & (val - 1) == 0 이라는 비트 연산 트릭을 사용하여 효율적으로 판별하며,
 * 0은 2의 거듭제곱이 아닌 것으로 처리한다.
 *
 * === fio에서의 사용 ===
 * I/O 블록 크기, 정렬(alignment), 버퍼 크기 등의 설정값이 2의 거듭제곱인지 검증할 때 사용된다.
 * 많은 I/O 엔진과 디바이스가 2의 거듭제곱 크기를 요구하므로 입력 유효성 검사에 활용된다.
 */
#ifndef FIO_POW2_H
#define FIO_POW2_H

#include <inttypes.h>
#include "types.h"

static inline bool is_power_of_2(uint64_t val)
{
	return (val != 0 && ((val & (val - 1)) == 0));
}

#endif
