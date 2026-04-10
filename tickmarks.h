/*
 * [한국어 설명] 그래프 눈금(tickmark) 계산 헤더 (tickmarks.h)
 *
 * 그래프의 축에 표시할 눈금 간격과 값을 계산하는 모듈.
 * "Graphics Gems" (Andrew S. Glassner)의 Paul Heckbert 알고리즘을 기반으로
 * "nice number"(1, 2, 5의 배수)를 사용하여 읽기 좋은 눈금 간격을 생성한다.
 */
#ifndef TICKMARKS_H
#define TICKMARKS_H

/* 하나의 눈금을 나타내는 구조체 */
struct tickmark {
	double value;       /* 눈금의 수치 값 */
	char string[20];    /* 표시할 문자열 (포맷팅된 값, K/M/G 접미사 포함 가능) */
};

/*
 * calc_tickmarks - 주어진 범위에 대해 적절한 눈금 위치를 계산
 * @min: 데이터 최소값
 * @max: 데이터 최대값
 * @nticks: 원하는 눈금 개수 (대략적인 값, 실제 개수는 다를 수 있음)
 * @tm: 계산된 tickmark 배열이 할당되어 반환됨 (호출자가 free 해야 함)
 * @power_of_ten: 축약에 사용된 10의 거듭제곱이 반환됨 (0, 3, 6, 9)
 * @use_KMG_symbols: true이면 K/M/G 접미사를 사용하여 축약
 * @base_off: 단위 축약의 기본 오프셋
 * @return: 실제 생성된 눈금 개수
 */
int calc_tickmarks(double min, double max, int nticks, struct tickmark **tm,
			int *power_of_ten, int use_KMG_symbols, int base_off);

#endif
