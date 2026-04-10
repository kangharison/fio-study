/*
 * [한국어 설명] 길이 제한 문자열-정수 변환 헤더 (strntol.h)
 *
 * === 파일의 역할 ===
 * strntol() 함수의 선언을 제공한다. 문자열 포인터, 길이, 끝 포인터,
 * 진법(base)을 인자로 받아 long 값을 반환한다.
 *
 * === fio에서의 사용 ===
 * fio의 문자열 파싱 코드에서 길이가 제한된 문자열을 안전하게 숫자로 변환하기 위해
 * 이 헤더를 포함한다.
 */
#ifndef FIO_STRNTOL_H
#define FIO_STRNTOL_H

#include <stdint.h>

long strntol(const char *str, size_t sz, char **end, int base);

#endif
