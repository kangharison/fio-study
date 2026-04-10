/*
 * [한국어 설명] IEEE 754 부동소수점 변환 테스트 (ieee754.c)
 *
 * === 파일의 역할 ===
 * fio의 IEEE 754 부동소수점 pack/unpack 함수(fio_double_to_uint64, fio_uint64_to_double)의
 * 정확성을 검증하는 테스트 프로그램이다. 여러 double 값을 uint64로 변환한 뒤 다시
 * double로 복원하여 원본과의 차이(delta)를 확인한다.
 */
#include <stdio.h>
#include "../lib/ieee754.h"

static double values[] = { -17.23, 17.23, 123.4567, 98765.4321,
	3.14159265358979323, 0.0 };

int main(int argc, char *argv[])
{
	uint64_t i;
	double f, delta;
	int j, differences = 0;

	j = 0;
	do {
		i = fio_double_to_uint64(values[j]);
		f = fio_uint64_to_double(i);
		delta = values[j] - f;
		printf("%26.20lf -> %26.20lf, delta = %26.20lf\n", values[j],
			f, delta);
		if (f != values[j])
			differences++;
		j++;
	} while (values[j] != 0.0);

	return differences;
}
