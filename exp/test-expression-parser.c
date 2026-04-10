/*
 * [한국어 설명] 실험적 수식 파서 테스트 (test-expression-parser.c)
 *
 * === 파일의 역할 ===
 * fio 옵션에서 사용되는 산술 수식 파싱 기능을 테스트하는 실험적 프로그램이다.
 * 덧셈, 뺄셈, 곱셈 등의 산술 표현식이 올바르게 파싱되고 계산되는지 검증한다.
 * 새로운 수식 파서 기능의 개발 및 디버깅을 위한 테스트 용도로 사용된다.
 */
/*
 * (C) Copyright 2014, Stephen M. Cameron.
 *
 * The license below covers all files distributed with fio unless otherwise
 * noted in the file itself.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include <stdio.h>
#include <string.h>

#include "../y.tab.h"

extern int evaluate_arithmetic_expression(const char *buffer, long long *ival,
					  double *dval, double implied_units, int is_time);
 
int main(int argc, char *argv[])
{
	int rc, bye = 0;
	long long result;
	double dresult;
	char buffer[100];

	do {
		if (fgets(buffer, 90, stdin) == NULL)
			break;
		rc = strlen(buffer);
		if (rc > 0 && buffer[rc - 1] == '\n')
			buffer[rc - 1] = '\0';
		rc = evaluate_arithmetic_expression(buffer, &result, &dresult, 1.0, 0);
		if (!rc) {
			printf("%lld (%20.20lf)\n", result, dresult);
		} else {
			fprintf(stderr, "Syntax error\n");
			result = 0;
			dresult = 0;
		}
	} while (!bye);
	return 0;
}

