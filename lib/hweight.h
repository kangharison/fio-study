/*
 * [한국어 설명] 해밍 가중치 헤더 (hweight.h)
 *
 * === 파일의 역할 ===
 * hweight8, hweight32, hweight64 함수의 선언을 제공한다.
 * 각각 8비트, 32비트, 64비트 정수의 설정된 비트 수를 반환한다.
 *
 * === fio에서의 사용 ===
 * fio 내부에서 비트마스크의 popcount가 필요한 모듈에서 이 헤더를 포함한다.
 */
#ifndef FIO_HWEIGHT_H
#define FIO_HWEIGHT_H

#include <inttypes.h>

unsigned int hweight8(uint8_t w);
unsigned int hweight32(uint32_t w);
unsigned int hweight64(uint64_t w);

#endif
