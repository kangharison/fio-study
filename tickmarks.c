/*
 * [한국어 설명] 그래프 눈금(tickmark) 계산 구현 (tickmarks.c)
 *
 * === 파일의 역할 ===
 * 그래프의 축에 표시할 눈금 위치와 라벨을 계산한다.
 * "Graphics Gems" (Andrew S. Glassner, ISBN 0-12-286166-3)의
 * Paul Heckbert 알고리즘(p.657-659)을 기반으로 구현되었다.
 *
 * === 알고리즘 핵심 ===
 * "nice number" 개념: 1, 2, 5 (또는 그 10의 거듭제곱 배)를 사용하여
 * 사람이 읽기 편한 눈금 간격을 생성한다.
 * 예: 데이터 범위가 0~87이면 눈금은 0, 10, 20, ..., 90으로 생성
 *
 * === shorten 함수 ===
 * 큰 숫자(예: 1000000)를 축약(1000K 또는 1M)하여 축 라벨의 가독성을 높인다.
 * use_KMG_symbols가 true이면 K/M/G/P/E 접미사를 사용한다.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tickmarks.h"

#define MAX(a, b) (((a) < (b)) ? (b) : (a))

/*
 * nicenum - "nice number"를 찾는 함수
 * @x: 대상 숫자
 * @round: true이면 반올림, false이면 올림
 * @return: x에 가장 가까운 nice number (1, 2, 5의 10의 거듭제곱 배)
 */
static double nicenum(double x, int round)
{
	int exp;	/* exponent of x */
	double f;	/* fractional part of x */

	exp = floor(log10(x));
	f = x / pow(10.0, exp);
	if (round) {
		if (f < 1.5)
			return 1.0 * pow(10.0, exp);
		if (f < 3.0)
			return 2.0 * pow(10.0, exp);
		if (f < 7.0)
			return 5.0 * pow(10.0, exp);
		return 10.0 * pow(10.0, exp);
	}
	if (f <= 1.0)
		return 1.0 * pow(10.0, exp);
	if (f <= 2.0)
		return 2.0 * pow(10.0, exp);
	if (f <= 5.0)
		return 5.0 * pow(10.0, exp);
	return 10.0 * pow(10.0, exp);
}

static void shorten(struct tickmark *tm, int nticks, int *power_of_ten,
			int use_KMG_symbols, int base_offset)
{
	const char shorten_chr[] = { 0, 'K', 'M', 'G', 'P', 'E', 0 };
	int i, l, minshorten, shorten_idx = 0;
	char *str;

	minshorten = 100;
	for (i = 0; i < nticks; i++) {
		str = tm[i].string;
		l = strlen(str);

		if (strcmp(str, "0") == 0)
			continue;
		if (l > 9 && strcmp(&str[l - 9], "000000000") == 0) {
			*power_of_ten = 9;
			shorten_idx = 3;
		} else if (6 < minshorten && l > 6 &&
				strcmp(&str[l - 6], "000000") == 0) {
			*power_of_ten = 6;
			shorten_idx = 2;
		} else if (l > 3 && strcmp(&str[l - 3], "000") == 0) {
			*power_of_ten = 3;
			shorten_idx = 1;
		} else {
			*power_of_ten = 0;
		}

		if (*power_of_ten < minshorten)
			minshorten = *power_of_ten;
	}

	if (minshorten == 0)
		return;
	if (!use_KMG_symbols)
		shorten_idx = 0;
	else if (base_offset)
		shorten_idx += base_offset;

	for (i = 0; i < nticks; i++) {
		str = tm[i].string;
		l = strlen(str);
		str[l - minshorten] = shorten_chr[shorten_idx];
		if (shorten_idx)
			str[l - minshorten + 1] = '\0';
	}
}

int calc_tickmarks(double min, double max, int nticks, struct tickmark **tm,
		int *power_of_ten, int use_KMG_symbols, int base_offset)
{
	char str[100];
	int nfrac;
	double d;	/* tick mark spacing */
	double graphmin, graphmax;	/* graph range min and max */
	double range, x;
	int count, i;

	/* we expect min != max */
	range = nicenum(max - min, 0);
	d = nicenum(range / (nticks - 1), 1);
	graphmin = floor(min / d) * d;
	graphmax = ceil(max / d) * d;
	nfrac = MAX(-floor(log10(d)), 0);
	snprintf(str, sizeof(str)-1, "%%.%df", nfrac);

	count = ((graphmax + 0.5 * d) - graphmin) / d + 1;
	*tm = malloc(sizeof(**tm) * count);

	i = 0;
	for (x = graphmin; x < graphmax + 0.5 * d; x += d) {
		(*tm)[i].value = x;
		sprintf((*tm)[i].string, str, x);
		i++;
	}
	shorten(*tm, i, power_of_ten, use_KMG_symbols, base_offset);
	return i;
}

#if 0

static void test_range(double x, double y)
{
	int nticks, i;

	struct tickmark *tm = NULL;
	printf("Testing range %g - %g\n", x, y);
	nticks = calc_tickmarks(x, y, 10, &tm);

	for (i = 0; i < nticks; i++)
		printf("   (%s) %g\n", tm[i].string, tm[i].value);

	printf("\n\n");
	free(tm);
}

int main(int argc, char *argv[])
{
	test_range(0.0005, 0.008);
	test_range(0.5, 0.8);
	test_range(5.5, 8.8);
	test_range(50.5, 80.8);
	test_range(-20, 20.8);
	test_range(-30, 700.8);
}
#endif
