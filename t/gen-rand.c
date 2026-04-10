/*
 * [한국어 설명] 난수 생성기 분포 테스트 (gen-rand.c)
 *
 * === 파일의 역할 ===
 * fio의 내부 난수 생성기(rand_between)가 지정된 범위 내에서 균등하게 분포된 값을
 * 생성하는지 검증하는 테스트 프로그램이다. 버킷별로 생성된 값의 수를 집계하고
 * 통계적 기대값과 표준편차를 기준으로 PASS/FAIL을 판정한다.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lib/types.h"
#include "../lib/rand.h"
#include "../log.h"

int main(int argc, char *argv[])
{
	struct frand_state s;
	uint64_t i, start, end, nvalues;
	unsigned long *buckets, index, pass, fail;
	double p, dev, mean, vmin, vmax;

	if (argc < 4) {
		log_err("%s: start end nvalues\n", argv[0]);
		return 1;
	}

	start = strtoul(argv[1], NULL, 10);
	end = strtoul(argv[2], NULL, 10);

	if (start >= end) {
		log_err("%s: start must be smaller than end\n", argv[0]);
		return 1;
	}
	index = 1 + end - start;
	buckets = calloc(index, sizeof(unsigned long));

	nvalues = strtoul(argv[3], NULL, 10);

	init_rand(&s, false);

	for (i = 0; i < nvalues; i++) {
		int v = rand_between(&s, start, end);

		buckets[v - start]++;
	}

	p = 1.0 / index;
	dev = sqrt(nvalues * p * (1.0 - p));
	mean = nvalues * p;
	vmin = mean - dev;
	vmax = mean + dev;

	pass = fail = 0;
	for (i = 0; i < index; i++) {
		if (buckets[i] < vmin || buckets[i] > vmax) {
			printf("FAIL bucket%4lu: val=%8lu (%.1f < %.1f > %.1f)\n", (unsigned long) i + 1, buckets[i], vmin, mean, vmax);
			fail++;
		} else {
			printf("PASS bucket%4lu: val=%8lu (%.1f < %.1f > %.1f)\n", (unsigned long) i + 1, buckets[i], vmin, mean, vmax);
			pass++;
		}
	}

	printf("Passes=%lu, Fail=%lu\n", pass, fail);
	free(buckets);
	return 0;
}
