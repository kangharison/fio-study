/*
 * [한국어 설명] IEEE 754 부동소수점 변환 헤더 (ieee754.h)
 *
 * === 파일의 역할 ===
 * pack754/unpack754 함수의 선언과 fio_double_to_uint64/fio_uint64_to_double 매크로를
 * 정의한다. fio_fp64_t 공용체 타입도 여기서 선언된다.
 *
 * === fio에서의 사용 ===
 * fio의 클라이언트-서버 통신에서 double 값을 uint64로 변환하여 네트워크로 전송할 때
 * 이 헤더의 매크로와 타입을 사용한다.
 */
#ifndef FIO_IEEE754_H
#define FIO_IEEE754_H

#include <inttypes.h>

extern uint64_t pack754(long double f, unsigned bits, unsigned expbits);
extern long double unpack754(uint64_t i, unsigned bits, unsigned expbits);

#define fio_double_to_uint64(val)	pack754((val), 64, 11)
#define fio_uint64_to_double(val)	unpack754((val), 64, 11)

typedef struct fio_fp64 {
	union {
		uint64_t i;		/* [한국어] 정수 표현 (네트워크 전송/직렬화용) */
		double f;		/* [한국어] 부동소수점 표현 (연산용) */
		uint8_t filler[16];	/* [한국어] 16바이트 정렬 패딩 */
	} u;
} fio_fp64_t;

#endif
