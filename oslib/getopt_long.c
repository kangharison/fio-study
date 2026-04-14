/*
 * [한국어 설명] 긴 옵션 파싱 함수 폴리필 구현 (getopt_long.c)
 *
 * === 파일의 역할 ===
 * getopt_long_only() 함수의 폴리필 구현을 제공한다. 이 함수는 커맨드라인에서
 * --name=value 형태의 긴 옵션과 -x 형태의 짧은 옵션을 모두 파싱한다.
 * klibc 라이브러리에서 가져온 코드로, GNU getopt_long_only()의 주요 부분집합을
 * 구현한다. 옵션 재정렬, -W foo, 첫 번째 optstring 문자 "-"는 지원하지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 커맨드라인 파싱의 핵심이다.
 * main() [fio.c] → parse_options() [init.c] → getopt_long_only()
 * fio의 모든 --옵션은 이 함수를 통해 파싱된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: init.c의 parse_options()
 * - 의존: oslib/getopt.h (struct option, enum 정의)
 * - 전역 변수: optarg(옵션 인자), optind(다음 인덱스), opterr, optopt
 *
 * === 주요 함수 요약 ===
 * - getopt_long_only(): 긴 옵션과 짧은 옵션을 모두 파싱하는 메인 함수
 * - option_matches(): 인자 문자열과 옵션 이름의 일치 여부를 확인하는 헬퍼
 */
/*
 * getopt.c
 *
 * getopt_long(), or at least a common subset thereof:
 *
 * - Option reordering is not supported
 * - -W foo is not supported
 * - First optstring character "-" not supported.
 *
 * This file was imported from the klibc library from hpa
 */

#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include "getopt.h"

/* [한국어] getopt 전역 변수들 - POSIX 표준에서 정의된 인터페이스 */
char *optarg;    /* [한국어] 현재 옵션의 인자 문자열 포인터 */
int optind, opterr, optopt;  /* [한국어] optind: 다음 argv 인덱스, optopt: 알 수 없는 옵션 문자 */

/* [한국어] 파싱 상태를 추적하는 내부 구조체 - 여러 호출 간 상태 유지 */
static struct getopt_private_state {
	const char *optptr;        /* [한국어] 현재 옵션 문자열 내 위치 (결합 옵션 -abc 처리용) */
	const char *last_optstring; /* [한국어] 마지막 호출 시 optstring - 변경 감지용 */
	char *const *last_argv;    /* [한국어] 마지막 호출 시 argv - 변경 감지용 */
} pvt;

/*
 * [한국어]
 * option_matches - 인자 문자열과 옵션 이름의 일치 여부 확인
 *
 * @arg_str: 사용자가 입력한 옵션 문자열 (--이후 부분, '='까지)
 * @opt_name: 비교할 옵션 이름
 * @smatch: 1이면 부분 매칭 허용, 0이면 전체 매칭만 허용
 * @return: 일치 시 arg_str에서 옵션 이름 이후 위치 ('=' 또는 '\0'), 불일치 시 NULL
 *
 * 호출 체인:
 *   getopt_long_only() → [option_matches()]
 */
static inline const char *option_matches(const char *arg_str,
					 const char *opt_name, int smatch)
{
	while (*arg_str != '\0' && *arg_str != '=') {
		if (*arg_str++ != *opt_name++)
			return NULL;
	}

	if (*opt_name && !smatch)
		return NULL;

	return arg_str;
}

/*
 * [한국어]
 * getopt_long_only - 긴 옵션과 짧은 옵션을 모두 파싱하는 메인 함수
 *
 * @argc: 인자 개수
 * @argv: 인자 배열
 * @optstring: 짧은 옵션 문자열 (예: "o:v" → -o는 인자 필요, -v는 인자 없음)
 * @longopts: struct option 배열 (이름이 NULL인 원소로 종료)
 * @longindex: 매칭된 긴 옵션의 인덱스가 저장될 포인터 (NULL 가능)
 * @return: 옵션 문자/값, '?'는 에러, -1은 옵션 끝
 *
 * 동작 과정:
 * 1) "--"로 시작하면 긴 옵션으로 처리 (longopts 배열에서 검색)
 * 2) "-"로 시작하면 짧은 옵션으로 처리 (optstring에서 검색)
 * 3) 옵션이 아닌 인자를 만나면 -1 반환
 * 4) GNU 확장: 긴 옵션의 부분 매칭(unique prefix)을 지원
 *
 * 호출 체인:
 *   parse_options() [init.c] → [getopt_long_only()] → option_matches()
 */
int getopt_long_only(int argc, char *const *argv, const char *optstring,
		const struct option *longopts, int *longindex)
{
	const char *carg;
	const char *osptr;
	int opt;

	optarg = NULL;

	/* getopt() relies on a number of different global state
	   variables, which can make this really confusing if there is
	   more than one use of getopt() in the same program.  This
	   attempts to detect that situation by detecting if the
	   "optstring" or "argv" argument have changed since last time
	   we were called; if so, reinitialize the query state. */

	if (optstring != pvt.last_optstring || argv != pvt.last_argv ||
	    optind < 1 || optind > argc) {
		/* optind doesn't match the current query */
		pvt.last_optstring = optstring;
		pvt.last_argv = argv;
		optind = 1;
		pvt.optptr = NULL;
	}

	carg = argv[optind];

	/* First, eliminate all non-option cases */

	if (!carg || carg[0] != '-' || !carg[1])
		return -1;

	if (carg[1] == '-') {
		const struct option *lo;
		const char *opt_end = NULL;

		optind++;

		/* Either it's a long option, or it's -- */
		if (!carg[2]) {
			/* It's -- */
			return -1;
		}

		for (lo = longopts; lo->name; lo++) {
			opt_end = option_matches(carg+2, lo->name, 0);
			if (opt_end)
			    break;
		}
		/*
		 * The GNU getopt_long_only() apparently allows a short match,
		 * if it's unique and if we don't have a full match. Let's
		 * do the same here, search and see if there is one (and only
		 * one) short match.
		 */
		if (!opt_end) {
			const struct option *lo_match = NULL;

			for (lo = longopts; lo->name; lo++) {
				const char *ret;

				ret = option_matches(carg+2, lo->name, 1);
				if (!ret)
					continue;
				if (!opt_end) {
					opt_end = ret;
					lo_match = lo;
				} else {
					opt_end = NULL;
					break;
				}
			}
			if (!opt_end)
				return '?';
			lo = lo_match;
		}

		if (longindex)
			*longindex = lo-longopts;

		if (*opt_end == '=') {
			if (lo->has_arg)
				optarg = (char *)opt_end+1;
			else
				return '?';
		} else if (lo->has_arg == 1) {
			if (!(optarg = argv[optind]))
				return '?';
			optind++;
		}

		if (lo->flag) {
			*lo->flag = lo->val;
			return 0;
		} else {
			return lo->val;
		}
	}

	if ((uintptr_t) (pvt.optptr - carg) > (uintptr_t) strlen(carg)) {
		/* Someone frobbed optind, change to new opt. */
		pvt.optptr = carg + 1;
	}

	opt = *pvt.optptr++;

	if (opt != ':' && (osptr = strchr(optstring, opt))) {
		if (osptr[1] == ':') {
			if (*pvt.optptr) {
				/* Argument-taking option with attached
				   argument */
				optarg = (char *)pvt.optptr;
				optind++;
			} else {
				/* Argument-taking option with non-attached
				   argument */
				if (osptr[2] == ':') {
					if (argv[optind + 1]) {
						optarg = (char *)argv[optind+1];
						optind += 2;
					} else {
						optarg = NULL;
						optind++;
					}
					return opt;
				} else if (argv[optind + 1]) {
					optarg = (char *)argv[optind+1];
					optind += 2;
				} else {
					/* Missing argument */
					optind++;
					return (optstring[0] == ':')
						? ':' : '?';
				}
			}
			return opt;
		} else {
			/* Non-argument-taking option */
			/* pvt.optptr will remember the exact position to
			   resume at */
			if (!*pvt.optptr)
				optind++;
			return opt;
		}
	} else {
		/* Unknown option */
		optopt = opt;
		if (!*pvt.optptr)
			optind++;
		return '?';
	}
}
