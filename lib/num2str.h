/*
 * [한국어 설명] 숫자-문자열 변환 헤더 (num2str.h)
 *
 * === 파일의 역할 ===
 * num2str() 함수와 bytes2str_simple() 함수의 선언 및 단위 열거형(N2S_BYTE,
 * N2S_BITPERSEC 등)을 정의한다.
 *
 * === fio에서의 사용 ===
 * fio의 통계 모듈에서 숫자 값을 사람이 읽기 쉬운 단위 문자열로 변환하기 위해
 * 이 헤더를 포함한다.
 */
#ifndef FIO_NUM2STR_H
#define FIO_NUM2STR_H

#include <inttypes.h>

enum n2s_unit {
	N2S_NONE	= 0,
	N2S_PERSEC	= 1,
	N2S_BYTE	= 2,
	N2S_BIT		= 3,
	N2S_BYTEPERSEC	= 4,
	N2S_BITPERSEC	= 5,
};

extern char *num2str(uint64_t, int, int, int, enum n2s_unit);

extern const char *bytes2str_simple(char *buf, size_t bufsize, uint64_t bytes);

#endif
