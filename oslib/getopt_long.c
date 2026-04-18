/*
 * [한국어 설명] GNU getopt_long_only 폴리필 구현 (getopt_long.c)
 *
 * === 파일의 역할 ===
 * GNU getopt_long_only(3) 의 주요 부분집합을 구현한 폴리필이다. klibc
 * (hpa 의 klibc 프로젝트)에서 가져온 코드로, GNU 확장을 제공하지 않는
 * 플랫폼(BSD 일부·Windows·오래된 libc)에서 fio 의 --옵션 파싱이 동일하게
 * 동작하도록 한다. 주요 기능:
 *   1) "--name[=value]" / "--name value" / "-s" / "-s value" / "-sFOO"
 *      (short 옵션에 붙은 인자) 형태를 모두 처리.
 *   2) unique-prefix 매칭: 예 "--blo" 가 "--blocksize" 하나에만 부분 매치되면
 *      이를 그 옵션으로 해석(GNU 호환).
 *   3) getopt_long_only 변종: "-" 한 글자만 있어도 long option 처럼 취급(fio
 *      가 대부분 "--" 사용하지만 이 호환을 위해 유지).
 * 지원하지 않는 기능: 옵션 재정렬(non-option 을 뒤로 보내지 않음), "-W foo"
 * 형태, optstring 첫 글자 "-"(모든 인자를 1 로 반환하는 모드).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 커맨드 라인 파싱 진입점이다.
 *   main() [fio.c] → parse_options() [init.c] → parse_cmd_line()
 *     → [getopt_long_only()] → 내부 option_matches() 헬퍼
 * parse_cmd_line 은 한 옵션씩 이 함수를 호출해 c/val 을 얻고, optarg 로
 * 관련 인자(있다면)를 받는다. 루프는 -1 반환 시 종료.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: init.c 의 parse_cmd_line() (fio.c 의 main 에서 호출).
 * - 의존: <string.h>(strlen, strchr), <stdint.h>(uintptr_t),
 *   oslib/getopt.h(struct option 정의, no_argument/required_argument/
 *   optional_argument enum).
 * - 공유 상태: 4개의 POSIX 표준 전역 변수 (optarg, optind, opterr, optopt)
 *   을 이 TU 에서 정의. 내부 정적 구조체 pvt 로 호출 간 상태 유지
 *   (optstring 이나 argv 가 바뀌면 리셋하는 방어 로직 포함).
 *
 * === 주요 함수/구조체 요약 ===
 * - getopt_long_only(): 옵션 파싱 메인 루프 — argv[optind] 하나를 해석.
 * - option_matches(): 한 arg 문자열이 특정 long option 이름과 일치하는지
 *   (완전 또는 prefix) 검사하는 헬퍼.
 * - struct getopt_private_state: 결합 short 옵션(-abc)·호출자 변경 감지
 *   을 위한 내부 상태.
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

#include <stdint.h>  /* [한국어] uintptr_t — optptr 유효성 검사용 포인터 산술 범위 비교. */
#include <unistd.h>  /* [한국어] POSIX 기본 — STDIN/STDOUT 등 간접 의존. */
#include <string.h>  /* [한국어] strlen, strchr — 옵션 문자열 탐색. */

#include "getopt.h"  /* [한국어] struct option 정의 + has_arg 상수. */

/* [한국어] getopt 전역 변수들 - POSIX 표준에서 정의된 인터페이스 */
char *optarg;    /* [한국어] 직전 호출에서 매치된 옵션의 인자 문자열.
                   * 설정자: getopt_long_only 가 인자가 있는 옵션 처리 시 대입.
                   * 읽는 자: init.c::parse_cmd_line 의 switch(c) 분기.
                   * 값 범위: NULL 또는 argv 의 한 원소 또는 argv[i] 중간 포인터.
                   * 동기화: 전역 — fio 는 단일 스레드에서만 getopt 를 호출. */
int optind, opterr, optopt;  /* [한국어] optind: 다음 argv 인덱스, optopt: 알 수 없는 옵션 문자 */
/*
 * [한국어] 세 전역의 역할:
 *   - optind: 다음에 해석할 argv 인덱스(1 부터 시작). 함수 내부에서 자동 전진.
 *   - opterr: getopt 가 에러 메시지를 자동 출력할지 플래그(이 구현에선 참조만).
 *   - optopt: 알 수 없는 short 옵션 문자가 들어왔을 때 저장되어, '?' 반환과 함께
 *             호출자가 어떤 문자가 문제였는지 알 수 있게 한다.
 * 설정자/읽는 자/동기화: optarg 와 동일(단일 스레드 파싱 가정).
 */

/* [한국어] 파싱 상태를 추적하는 내부 구조체 - 여러 호출 간 상태 유지 */
static struct getopt_private_state {
	const char *optptr;
	/* [한국어] 결합 short 옵션(-abc) 처리 중 현재 해석 위치 포인터.
	 * 설정자: 함수가 short 옵션을 해석하며 한 글자씩 전진.
	 * 읽는 자: 다음 호출에서 optptr 이 NUL 이 아니면 같은 argv 원소 내
	 *          다음 문자를 이어서 해석.
	 * 값 범위: NULL, argv[optind] 어딘가를 가리킴.
	 * 동기화: 전역 상태 — 단일 스레드 전제. */
	const char *last_optstring;
	/* [한국어] 직전 호출 시 optstring 포인터 — 호출자가 다른 optstring/다른
	 * argv 로 getopt 를 재시작하면 이를 감지해 상태 리셋.
	 * 설정자: 매 호출 진입 시 비교 후 재설정.
	 * 읽는 자: 비교용. */
	char *const *last_argv;
	/* [한국어] 직전 호출 시 argv 포인터 — 상동. */
} pvt;

/*
 * [한국어]
 * option_matches - arg 문자열이 특정 long option 이름과 일치하는지 판정
 *
 * @arg_str: 사용자 입력에서 "--" 이후 부분(예: "blocksize=4k" 의 "blocksize=4k").
 * @opt_name: longopts[] 배열의 한 원소의 name 필드(NUL-종단).
 * @smatch: 0 = 정확 매치만 허용, 1 = prefix 매치 허용(unique-prefix 검색 시).
 * @return: 일치하면 arg_str 에서 name 이 끝나는 위치 포인터(= '=' 또는
 *          NUL), 아니면 NULL.
 *
 * 동작: arg_str 과 opt_name 을 문자씩 비교. '=' 또는 NUL 을 만나면 arg_str
 * 끝 판정. 중간 불일치는 NULL. smatch == 0 이면 arg_str 소진 시점에 opt_name
 * 도 소진되어야 하고(정확 매치), smatch == 1 이면 opt_name 이 남아있어도
 * 매치 간주(prefix 매치).
 *
 * 호출 체인:
 *   getopt_long_only() → [option_matches()]
 */
static inline const char *option_matches(const char *arg_str,
					 const char *opt_name, int smatch)
{
	while (*arg_str != '\0' && *arg_str != '=') {  /* [한국어] arg_str 끝('=' 또는 NUL) 전까지 한 문자씩 스캔. */
		if (*arg_str++ != *opt_name++)  /* [한국어] 대소문자 구분 완전 일치 — 불일치 시 즉시 실패. */
			return NULL;
	}

	if (*opt_name && !smatch)  /* [한국어] arg_str 는 끝났지만 opt_name 이 더 있으면, 정확 매치 모드에선 실패. */
		return NULL;

	return arg_str;  /* [한국어] 성공 — '=' 또는 NUL 위치 포인터 반환. 호출자가 이어서 '=' 후 값을 파싱. */
}

/*
 * [한국어]
 * getopt_long_only - 긴/짧은 옵션 통합 파싱 메인 루프(한 호출당 옵션 1개 처리)
 *
 * @argc: 인자 총 개수(main 의 argc).
 * @argv: 인자 배열(main 의 argv). argv[0] 은 프로그램명.
 * @optstring: short 옵션 스펙(예: "a:bc::" — a 는 인자 필수, b 는 인자 없음,
 *             c 는 인자 선택적).
 * @longopts: long 옵션 배열(NULL name 으로 sentinel 종료).
 * @longindex: 매치된 long option 의 인덱스 저장처(NULL 허용).
 * @return: 매치된 옵션의 val(또는 short 옵션 문자). 옵션 소진 시 -1, 에러 '?'
 *          또는 ':'. 전역 optarg/optind 도 갱신된다.
 *
 * 알고리즘 개요:
 * 1) optstring/argv 변경 감지 — 호출자가 재초기화했으면 pvt 리셋.
 * 2) argv[optind] 읽어 옵션 여부 판별:
 *    - '-' 로 시작하지 않으면 비옵션 인자 — -1 반환(재정렬 미지원).
 *    - "--" 만 있으면 옵션 종료 관례 — -1 반환.
 *    - "--" 접두사 → long 옵션 처리(정확 매치 → 실패 시 unique prefix).
 *    - "-" 접두사 → short 옵션 처리(optstring 스캔).
 * 3) long 옵션 분기:
 *    - '=' 가 있으면 '=' 뒤를 optarg.
 *    - has_arg == 1(required_argument) 이면 argv[optind] 를 optarg 로 소비.
 *    - flag 가 설정되어 있으면 *flag = val; 0 반환. 아니면 val 반환.
 * 4) short 옵션 분기:
 *    - optstring 에 있으면 인자 요구 여부(':' 뒤따름) 따라 optarg 채움.
 *    - '-abc' 처리 위해 pvt.optptr 로 같은 argv 원소 내 연속 옵션 해석.
 *    - 없으면 optopt = opt; '?' 반환.
 *
 * 호출 체인:
 *   main() → parse_options() → parse_cmd_line() → [getopt_long_only()]
 *     → option_matches()
 */
int getopt_long_only(int argc, char *const *argv, const char *optstring,
		const struct option *longopts, int *longindex)
{
	const char *carg;  /* [한국어] 현재 처리할 argv 원소. */
	const char *osptr;  /* [한국어] optstring 내에서 찾아낸 short 옵션 위치 포인터. */
	int opt;  /* [한국어] 현재 처리 중인 short 옵션 문자. */

	optarg = NULL;  /* [한국어] 반환 전 기본값 — 인자 없는 옵션 대응. */

	/* getopt() relies on a number of different global state
	   variables, which can make this really confusing if there is
	   more than one use of getopt() in the same program.  This
	   attempts to detect that situation by detecting if the
	   "optstring" or "argv" argument have changed since last time
	   we were called; if so, reinitialize the query state. */

	if (optstring != pvt.last_optstring || argv != pvt.last_argv ||
	    optind < 1 || optind > argc) {
		/* optind doesn't match the current query */
		pvt.last_optstring = optstring;  /* [한국어] 재기준점 저장. */
		pvt.last_argv = argv;
		optind = 1;  /* [한국어] argv[0] 은 프로그램명 — argv[1] 부터 시작. */
		pvt.optptr = NULL;  /* [한국어] 결합 short 옵션 상태 리셋. */
	}

	carg = argv[optind];  /* [한국어] 현재 처리 후보 argv 원소. */

	/* First, eliminate all non-option cases */

	if (!carg || carg[0] != '-' || !carg[1])
		return -1;  /* [한국어] NULL / "-" 아님 / "-" 혼자 → 옵션 아님. 파싱 종료. */

	if (carg[1] == '-') {  /* [한국어] "--" 접두 — long 옵션 경로. */
		const struct option *lo;  /* [한국어] 매칭 시도 중인 longopts 원소. */
		const char *opt_end = NULL;  /* [한국어] option_matches 성공 시 옵션 이름이 끝나는 위치. */

		optind++;  /* [한국어] 옵션 소비 — 다음 argv 로 전진. */

		/* Either it's a long option, or it's -- */
		if (!carg[2]) {
			/* It's -- */
			return -1;  /* [한국어] "--" 단독 = 옵션 파싱 종료 관례. */
		}

		for (lo = longopts; lo->name; lo++) {  /* [한국어] 1차: 정확 매치 검색. */
			opt_end = option_matches(carg+2, lo->name, 0);  /* [한국어] carg+2: "--" 건너뜀. */
			if (opt_end)
			    break;
		}
		/*
		 * The GNU getopt_long_only() apparently allows a short match,
		 * if it's unique and if we don't have a full match. Let's
		 * do the same here, search and see if there is one (and only
		 * one) short match.
		 */
		if (!opt_end) {  /* [한국어] 정확 매치 실패 — unique prefix 검색으로 폴백. */
			const struct option *lo_match = NULL;

			for (lo = longopts; lo->name; lo++) {
				const char *ret;

				ret = option_matches(carg+2, lo->name, 1);  /* [한국어] smatch=1: prefix 허용. */
				if (!ret)
					continue;
				if (!opt_end) {
					opt_end = ret;  /* [한국어] 첫 prefix 매치 기록. */
					lo_match = lo;
				} else {
					opt_end = NULL;  /* [한국어] 두 번째 매치 — 모호. 실패 처리. */
					break;
				}
			}
			if (!opt_end)
				return '?';  /* [한국어] 미매치 또는 다중 매치 — 에러 신호. */
			lo = lo_match;  /* [한국어] 유일 매치 확정. */
		}

		if (longindex)
			*longindex = lo-longopts;  /* [한국어] 호출자 요청 시 매치된 옵션 인덱스 반환. */

		if (*opt_end == '=') {  /* [한국어] "--name=value" 형태. */
			if (lo->has_arg)
				optarg = (char *)opt_end+1;  /* [한국어] '=' 뒤를 optarg 로. has_arg == 1 또는 2 둘 다 허용. */
			else
				return '?';  /* [한국어] 인자 금지 옵션에 '=' 가 붙음 — 에러. */
		} else if (lo->has_arg == 1) {  /* [한국어] required_argument: 다음 argv 를 optarg 로 소비. */
			if (!(optarg = argv[optind]))
				return '?';  /* [한국어] 인자 필수인데 argv 소진 — 에러. */
			optind++;  /* [한국어] 인자 소비. */
		}

		if (lo->flag) {  /* [한국어] flag 포인터 설정된 옵션: *flag = val; 반환 0. */
			*lo->flag = lo->val;
			return 0;
		} else {
			return lo->val;  /* [한국어] 일반 옵션: val 값 반환(보통 short 문자 또는 고유 id). */
		}
	}

	if ((uintptr_t) (pvt.optptr - carg) > (uintptr_t) strlen(carg)) {
		/* Someone frobbed optind, change to new opt. */
		pvt.optptr = carg + 1;  /* [한국어] optptr 이 carg 범위 벗어남 — 호출자가 optind 를 외부 조작한 흔적. 새로 시작. */
	}

	opt = *pvt.optptr++;  /* [한국어] 현재 short 옵션 문자. pvt.optptr 은 다음 문자로 전진. */

	if (opt != ':' && (osptr = strchr(optstring, opt))) {  /* [한국어] optstring 내 존재 여부 — ':' 자체는 구문자라 거부. */
		if (osptr[1] == ':') {  /* [한국어] opt 뒤에 ':' = 인자 받는 옵션. */
			if (*pvt.optptr) {
				/* Argument-taking option with attached
				   argument */
				optarg = (char *)pvt.optptr;  /* [한국어] "-sFOO" 같이 붙은 인자. */
				optind++;
			} else {
				/* Argument-taking option with non-attached
				   argument */
				if (osptr[2] == ':') {  /* [한국어] "::" = optional_argument. */
					if (argv[optind + 1]) {
						optarg = (char *)argv[optind+1];
						optind += 2;
					} else {
						optarg = NULL;
						optind++;
					}
					return opt;
				} else if (argv[optind + 1]) {  /* [한국어] required_argument + 다음 argv 존재 → 소비. */
					optarg = (char *)argv[optind+1];
					optind += 2;
				} else {
					/* Missing argument */
					optind++;
					return (optstring[0] == ':')
						? ':' : '?';  /* [한국어] optstring 첫 글자 ':' 는 에러 반환을 ':' 로 — POSIX 표준. */
				}
			}
			return opt;
		} else {
			/* Non-argument-taking option */
			/* pvt.optptr will remember the exact position to
			   resume at */
			if (!*pvt.optptr)  /* [한국어] 결합 short 옵션 소진 — 다음 argv 로. */
				optind++;
			return opt;
		}
	} else {
		/* Unknown option */
		optopt = opt;  /* [한국어] 알 수 없는 옵션 문자 저장 — 호출자가 어떤 게 문제였는지 안다. */
		if (!*pvt.optptr)
			optind++;
		return '?';  /* [한국어] 미지원 옵션 신호. */
	}
}
