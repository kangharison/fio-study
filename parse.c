/*
 * [한국어 설명] fio 옵션 파싱 엔진 (parse.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 INI 파일(잡 파일) 및 커맨드라인 옵션을 파싱하는 핵심 엔진이다.
 * "opt=val" 형식의 문자열을 분리하고, 타입별로 분기하여 값을 변환한 뒤
 * thread_options 구조체에 저장한다. 단위 접미사 처리, 오타 추천, 기본값 적용도 수행한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * init.c의 parse_jobs_ini()/parse_cmd_line()에서 이 파일의 parse_option()을 호출한다.
 * 호출 체인: init.c → parse_option() [이 파일] → handle_option() → __handle_option()
 * 파싱 예시: "bs=4k" → get_option()으로 분리 → FIO_OPT_STR_VAL 타입 →
 *           check_str_bytes("4k") → 4096 → val_store()로 td->o.bs 저장
 *
 * === 타 모듈과의 연결 ===
 * - init.c: parse_jobs_ini()/parse_cmd_line()에서 parse_option() 호출
 * - options.c: fio_options[] 배열을 참조하여 옵션 정의를 찾음
 * - parse.h: fio_option 구조체, 파싱 API 선언
 * - thread_options.h: 파싱 결과가 저장되는 대상 구조체
 *
 * === 주요 함수/구조체 요약 ===
 * - parse_option(): "opt=val" 문자열 파싱의 최상위 진입점
 * - __handle_option(): 타입별 실제 파싱 로직 (STR_VAL, INT, BOOL, RANGE 등)
 * - str_to_decimal(): 단위 접미사(k/m/g, s/ms/us) 문자열을 정수로 변환
 * - string_distance(): 레벤슈타인 편집 거리 (오타 옵션명 추천용)
 * - fill_default_options(): 미설정 옵션에 기본값 적용
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <float.h>

#include "compiler/compiler.h"  /* 컴파일러 매크로 (fio_fallthrough 등) */
#include "parse.h"              /* 옵션 파싱 구조체 및 API */
#include "debug.h"              /* dprint() 디버그 출력 */
#include "log.h"                /* log_info(), log_err() */
#include "options.h"            /* fio_options[] 배열 및 옵션 관련 */
#include "optgroup.h"           /* 옵션 그룹/카테고리 정의 */
#include "minmax.h"             /* min(), max() 매크로 */
#include "lib/ieee754.h"        /* IEEE 754 부동소수점 유틸리티 */
#include "lib/pow2.h"           /* is_power_of_2() */

#ifdef CONFIG_ARITHMETIC
#include "y.tab.h"              /* 산술 표현식 파서 (yacc/bison 생성) */
#endif

/* [한국어] 옵션 타입 이름 문자열 배열 — 디버그 출력용 */
static const char *opt_type_names[] = {
	"OPT_INVALID",
	"OPT_STR",
	"OPT_STR_ULL",
	"OPT_STR_MULTI",
	"OPT_STR_VAL",
	"OPT_STR_VAL_TIME",
	"OPT_STR_STORE",
	"OPT_RANGE",
	"OPT_INT",
	"OPT_ULL",
	"OPT_BOOL",
	"OPT_FLOAT_LIST",
	"OPT_STR_SET",
	"OPT_STR_VAL_ZONE",
	"OPT_DEPRECATED",
	"OPT_SOFT_DEPRECATED",
	"OPT_UNSUPPORTED",
};

/* [한국어] sort_options()에서 qsort 비교 시 사용하는 전역 옵션 배열 포인터 */
static const struct fio_option *__fio_options;

/*
 * [한국어] posval 문자열 매칭 시 긴 문자열을 먼저 비교하기 위한 정렬 비교 함수
 * 예: "readwrite"가 "read"보다 먼저 매칭되어야 한다.
 */
static int vp_cmp(const void *p1, const void *p2)
{
	const struct value_pair *vp1 = p1;
	const struct value_pair *vp2 = p2;

	return strlen(vp2->ival) - strlen(vp1->ival);  /* 긴 문자열이 앞으로 */
}

/*
 * [한국어] 옵션의 posval[] 배열을 복사 후 문자열 길이 내림차순으로 정렬
 * 매칭 시 가장 긴 접두사가 먼저 매칭되도록 보장한다.
 */
static void posval_sort(const struct fio_option *o, struct value_pair *vpmap)
{
	const struct value_pair *vp;
	int entries;

	memset(vpmap, 0, PARSE_MAX_VP * sizeof(struct value_pair));

	/* posval 배열에서 유효한 항목을 vpmap에 복사 */
	for (entries = 0; entries < PARSE_MAX_VP; entries++) {
		vp = &o->posval[entries];
		if (!vp->ival || vp->ival[0] == '\0')
			break;

		memcpy(&vpmap[entries], vp, sizeof(*vp));
	}

	/* 긴 문자열이 앞으로 오도록 정렬 */
	qsort(vpmap, entries, sizeof(struct value_pair), vp_cmp);
}

/*
 * [한국어] 옵션의 허용 범위(min/max)를 출력하는 헬퍼
 */
static void show_option_range(const struct fio_option *o,
			      ssize_t (*logger)(const char *format, ...))
{
	if (o->type == FIO_OPT_FLOAT_LIST) {
		const char *sep = "";
		if (!o->minfp && !o->maxfp)
			return;

		logger("%20s: ", "range");
		if (o->minfp != DBL_MIN) {
			logger("min=%f", o->minfp);
			sep = ", ";
		}
		if (o->maxfp != DBL_MAX)
			logger("%smax=%f", sep, o->maxfp);
		logger("\n");
	} else if (!o->posval[0].ival) {
		if (!o->minval && !o->maxval)
			return;

		logger("%20s: min=%d", "range", o->minval);
		if (o->maxval)
			logger(", max=%d", o->maxval);
		logger("\n");
	}
}

/* [한국어] 옵션의 허용 가능한 값 목록(posval[])을 출력 */
static void show_option_values(const struct fio_option *o)
{
	int i;

	for (i = 0; i < PARSE_MAX_VP; i++) {
		const struct value_pair *vp = &o->posval[i];

		if (!vp->ival)
			continue;

		log_info("%20s: %-10s", i == 0 ? "valid values" : "", vp->ival);
		if (vp->help)
			log_info(" %s", vp->help);
		log_info("\n");
	}

	if (i)
		log_info("\n");
}

/*
 * [한국어] 옵션의 상세 도움말 출력 (타입, 기본값, 범위, 허용 값 등)
 * is_err가 1이면 log_err로, 0이면 log_info로 출력한다.
 */
static void show_option_help(const struct fio_option *o, int is_err)
{
	/* [한국어] 옵션 타입별 사람이 읽을 수 있는 설명 문자열 */
	const char *typehelp[] = {
		[FIO_OPT_INVALID]	  = "invalid",
		[FIO_OPT_STR]		  = "string (opt=bla)",
		[FIO_OPT_STR_ULL]	  = "string (opt=bla)",
		[FIO_OPT_STR_MULTI]	  = "string with possible k/m/g postfix (opt=4k)",
		[FIO_OPT_STR_VAL]	  = "string (opt=bla)",
		[FIO_OPT_STR_VAL_TIME]	  = "string with time postfix (opt=10s)",
		[FIO_OPT_STR_STORE]	  = "string (opt=bla)",
		[FIO_OPT_RANGE]		  = "one to three ranges (opt=1k-4k[,4k-8k[,1k-8k]])",
		[FIO_OPT_INT]		  = "integer value (opt=100)",
		[FIO_OPT_ULL]		  = "integer value (opt=100)",
		[FIO_OPT_BOOL]		  = "boolean value (opt=1)",
		[FIO_OPT_FLOAT_LIST]	  = "list of floating point values separated by ':' (opt=5.9:7.8)",
		[FIO_OPT_STR_SET]	  = "empty or boolean value ([0|1])",
		[FIO_OPT_DEPRECATED]	  = "deprecated",
		[FIO_OPT_SOFT_DEPRECATED] = "deprecated",
		[FIO_OPT_UNSUPPORTED]	  = "unsupported",
	};
	ssize_t (*logger)(const char *format, ...);

	if (is_err)
		logger = log_err;
	else
		logger = log_info;

	if (o->alias)
		logger("%20s: %s\n", "alias", o->alias);

	logger("%20s: %s\n", "type", typehelp[o->type]);
	logger("%20s: %s\n", "default", o->def ? o->def : "no default");
	if (o->prof_name)
		logger("%20s: only for profile '%s'\n", "valid", o->prof_name);
	show_option_range(o, logger);
	show_option_values(o);
}

/*
 * [한국어] 시간 단위 접미사에 대한 배수 계산
 *
 * fio 내부에서 시간은 마이크로초(us) 단위로 통일한다.
 * 접미사 → 배수:
 *   us/usec → 1, ms/msec → 1000, s → 1000000,
 *   m → 60*1000000, h → 3600*1000000, d → 86400*1000000
 *
 * is_seconds=1이면 접미사 없을 때 기본 단위를 초(1000000us)로 간주한다.
 */
static unsigned long long get_mult_time(const char *str, int len,
					int is_seconds)
{
	const char *p = str;
	char *c;
	unsigned long long mult = 1;
	int i;

	/* 숫자 부분을 건너뛰어 접미사 시작 위치를 찾는다 */
	while ((p - str) <= len) {
		if (!isdigit((int) *p) && (*p != '+') && (*p != '-'))
			break;
		p++;
	}

	/* 접미사가 없으면 기본 단위 반환 */
	if (!isalpha((int) *p)) {
		if (is_seconds)
			return 1000000UL;   /* 기본 단위: 초 → 마이크로초 */
		else
			return 1;           /* 기본 단위: 마이크로초 그대로 */
	}

	/* 접미사를 소문자로 변환하여 비교 */
	c = strdup(p);
	for (i = 0; i < strlen(c); i++)
		c[i] = tolower((unsigned char)c[i]);

	if (!strncmp("us", c, 2) || !strncmp("usec", c, 4))
		mult = 1;                           /* 마이크로초 */
	else if (!strncmp("ms", c, 2) || !strncmp("msec", c, 4))
		mult = 1000;                        /* 밀리초 */
	else if (!strcmp("s", c))
		mult = 1000000;                     /* 초 */
	else if (!strcmp("m", c))
		mult = 60 * 1000000UL;              /* 분 */
	else if (!strcmp("h", c))
		mult = 60 * 60 * 1000000UL;         /* 시간 */
	else if (!strcmp("d", c))
		mult = 24 * 60 * 60 * 1000000ULL;   /* 일 */

	free(c);
	return mult;
}

/* [한국어] 바이트 단위 접미사에서 구분자로 사용되는 문자 판별 */
static int is_separator(char c)
{
	switch (c) {
	case ':':
	case '-':
	case ',':
	case '/':
		return 1;
	default:
		return 0;
	}
}

/*
 * [한국어] 바이트 단위 접미사에 대한 배수 계산 (내부 구현)
 *
 * kb_base 설정에 따라 접미사의 의미가 달라진다:
 *   kb_base=1024 (기본): k=1024, m=1048576, kib=1000, mib=1000000
 *   kb_base=1000:        k=1000, m=1000000, kib=1024, mib=1048576
 *
 * 즉 "ib" 접미사는 항상 kb_base의 반대 단위를 사용한다.
 * "%"이면 percent 플래그를 설정하고 1을 반환한다.
 */
static unsigned long long __get_mult_bytes(const char *p, void *data,
					   int *percent)
{
	unsigned int kb_base = fio_get_kb_base(data);  /* 1024 또는 1000 */
	unsigned long long ret = 1;
	unsigned int i, pow = 0, mult = kb_base;
	char *c;

	if (!p)
		return 1;

	c = strdup(p);

	/* 접미사를 소문자로 변환, 구분자에서 자름 */
	for (i = 0; i < strlen(c); i++) {
		c[i] = tolower((unsigned char)c[i]);
		if (is_separator(c[i])) {
			c[i] = '\0';
			break;
		}
	}

	/*
	 * [한국어] "ib" 접미사(pib, tib, gib, mib, kib)는 kb_base와 반대 단위를 사용:
	 *   kb_base=1024이면 "kib"는 1000 기반
	 *   kb_base=1000이면 "kib"는 1024 기반
	 * 일반 접미사(p, t, g, m, k)는 kb_base를 그대로 사용한다.
	 */
	if (!strncmp("pib", c, 3)) {
		pow = 5;
		if (kb_base == 1000)
			mult = 1024;
		else if (kb_base == 1024)
			mult = 1000;
	} else if (!strncmp("tib", c, 3)) {
		pow = 4;
		if (kb_base == 1000)
			mult = 1024;
		else if (kb_base == 1024)
			mult = 1000;
	} else if (!strncmp("gib", c, 3)) {
		pow = 3;
		if (kb_base == 1000)
			mult = 1024;
		else if (kb_base == 1024)
			mult = 1000;
	} else if (!strncmp("mib", c, 3)) {
		pow = 2;
		if (kb_base == 1000)
			mult = 1024;
		else if (kb_base == 1024)
			mult = 1000;
	} else if (!strncmp("kib", c, 3)) {
		pow = 1;
		if (kb_base == 1000)
			mult = 1024;
		else if (kb_base == 1024)
			mult = 1000;
	} else if (!strncmp("p", c, 1) || !strncmp("pb", c, 2)) {
		pow = 5;
	} else if (!strncmp("t", c, 1) || !strncmp("tb", c, 2)) {
		pow = 4;
	} else if (!strncmp("g", c, 1) || !strncmp("gb", c, 2)) {
		pow = 3;
	} else if (!strncmp("m", c, 1) || !strncmp("mb", c, 2)) {
		pow = 2;
	} else if (!strncmp("k", c, 1) || !strncmp("kb", c, 2)) {
		pow = 1;
	} else if (!strncmp("%", c, 1)) {
		*percent = 1;
		free(c);
		return ret;
	}

	/* pow 횟수만큼 mult를 곱해서 최종 배수 계산 (예: pow=3, mult=1024 → 1GB) */
	while (pow--)
		ret *= (unsigned long long) mult;

	free(c);
	return ret;
}

/*
 * [한국어] 바이트 단위 배수 계산 (외부 인터페이스)
 * 문자열에서 숫자 부분을 건너뛰고 접미사를 찾아 __get_mult_bytes()에 전달한다.
 */
static unsigned long long get_mult_bytes(const char *str, int len, void *data,
					 int *percent)
{
	const char *p = str;
	int digit_seen = 0;

	if (len < 2)
		return __get_mult_bytes(str, data, percent);

	/* 숫자 부분을 건너뛰어 접미사 시작점을 찾는다 */
	while ((p - str) <= len) {
		if (!isdigit((int) *p) &&
		    (((*p != '+') && (*p != '-')) || digit_seen))
			break;
		digit_seen |= isdigit((int) *p);
		p++;
	}

	/* 접미사가 알파벳이나 '%'가 아니면 접미사 없음 */
	if (!isalpha((int) *p) && (*p != '%'))
		p = NULL;

	return __get_mult_bytes(p, data, percent);
}

/* [한국어] 산술 표현식 평가 함수 (yacc/bison으로 생성된 파서) */
extern int evaluate_arithmetic_expression(const char *buffer, long long *ival,
					  double *dval, double implied_units,
					  int is_time);

/*
 * [한국어] 문자열을 부동소수점으로 변환
 * '('로 시작하면 산술 표현식 평가를 시도하고, 아니면 sscanf로 변환한다.
 * 반환: 1 = 성공, 0 = 실패
 */
int str_to_float(const char *str, double *val, int is_time)
{
#ifdef CONFIG_ARITHMETIC
	int rc;
	long long ival;
	double dval;

	if (str[0] == '(') {
		rc = evaluate_arithmetic_expression(str, &ival, &dval, 1.0, is_time);
		if (!rc) {
			*val = dval;
			return 1;
		}
	}
#endif
	return 1 == sscanf(str, "%lf", val);
}

/*
 * [한국어] 문자열을 정수(long long)로 변환 — 단위 접미사 처리 포함
 *
 * @str:        입력 문자열 (예: "4k", "30s", "(1024*4)")
 * @val:        결과 저장 포인터
 * @kilo:       1이면 바이트 단위 접미사(k/m/g) 처리, 0이면 시간 단위(s/ms/us) 처리
 * @data:       kb_base 조회용 thread_data (바이트 변환 시 필요)
 * @is_seconds: 1이면 접미사 없을 때 초를 기본 단위로 간주
 * @is_time:    1이면 시간 관련 값
 *
 * 반환: 0 = 성공, 1 = 실패
 *
 * '('로 시작하면 산술 표현식으로 평가, 아니면 strtoll() + 접미사 배수 적용.
 * 퍼센트('%')이면 특수 인코딩: *val = -1ULL - 원래값
 */
int str_to_decimal(const char *str, long long *val, int kilo, void *data,
		   int is_seconds, int is_time)
{
	int len, base;
	int rc = 1;
#ifdef CONFIG_ARITHMETIC
	long long ival;
	double dval;
	double implied_units = 1.0;
#endif

	len = strlen(str);
	if (!len)
		return 1;

#ifdef CONFIG_ARITHMETIC
	if (is_seconds)
		implied_units = 1000000.0;
	if (str[0] == '(')
		rc = evaluate_arithmetic_expression(str, &ival, &dval, implied_units, is_time);
	if (str[0] == '(' && !rc) {
		if (!kilo && is_seconds)
			*val = ival / 1000000LL;
		else
			*val = ival;
	}
#endif

	if (rc == 1) {
		char *endptr;

		if (strstr(str, "0x") || strstr(str, "0X"))
			base = 16;
		else
			base = 10;

		*val = strtoll(str, &endptr, base);
		if (*val == 0 && endptr == str)
			return 1;
		if (*val == LONG_MAX && errno == ERANGE)
			return 1;
	}

	if (kilo) {
		unsigned long long mult;
		int perc = 0;

		mult = get_mult_bytes(str, len, data, &perc);
		if (perc)
			*val = -1ULL - *val;
		else
			*val *= mult;
	} else
		*val *= get_mult_time(str, len, is_seconds);

	return 0;
}

/* [한국어] 바이트 단위 문자열 변환 래퍼 (kilo=1) */
int check_str_bytes(const char *p, long long *val, void *data)
{
	return str_to_decimal(p, val, 1, data, 0, 0);
}

/* [한국어] 시간 단위 문자열 변환 래퍼 (kilo=0, is_time=1) */
int check_str_time(const char *p, long long *val, int is_seconds)
{
	return str_to_decimal(p, val, 0, NULL, is_seconds, 1);
}

/* [한국어] 문자열 앞쪽 공백 제거 — 포인터를 앞으로 이동 */
void strip_blank_front(char **p)
{
	char *s = *p;

	if (!strlen(s))
		return;
	while (isspace((int) *s))
		s++;

	*p = s;
}

/*
 * [한국어] 문자열 뒤쪽 공백 및 주석 제거
 * ';'와 '#'을 주석 시작으로 처리하여 잘라내고, 뒤쪽 공백/제어문자도 제거한다.
 */
void strip_blank_end(char *p)
{
	char *start = p, *s;

	if (!strlen(p))
		return;

	/* 주석 문자(; #) 이후를 잘라냄 */
	s = strchr(p, ';');
	if (s)
		*s = '\0';
	s = strchr(p, '#');
	if (s)
		*s = '\0';
	if (s)
		p = s;

	/* 뒤쪽 공백/제어문자 제거 */
	s = p + strlen(p);
	while ((isspace((int) *s) || iscntrl((int) *s)) && (s > start))
		s--;

	*(s + 1) = '\0';
}

/* [한국어] 범위값(RANGE)의 각 항목을 바이트 단위로 변환 */
static int check_range_bytes(const char *str, long long *val, void *data)
{
	long long __val;

	if (!str_to_decimal(str, &__val, 1, data, 0, 0)) {
		*val = __val;
		return 0;
	}

	return 1;
}

/* [한국어] 문자열을 정수로 변환 (10진수 또는 0x 접두사의 16진수) */
static int check_int(const char *p, int *val)
{
	if (!strlen(p))
		return 1;
	if (strstr(p, "0x") || strstr(p, "0X")) {
		if (sscanf(p, "%x", val) == 1)
			return 0;
	} else {
		if (sscanf(p, "%u", val) == 1)
			return 0;
	}

	return 1;
}

/*
 * [한국어] 옵션 값 문자열에서 실제 값 부분의 길이를 계산
 * ','나 ':'가 나오면 그 앞까지만이 현재 값이다.
 * 예: "read,write" → "read"의 길이 4 반환
 */
static size_t opt_len(const char *str)
{
	char delimiter[] = {',', ':'};
	char *postfix;
	unsigned int i;
	size_t candidate_len;

	size_t prefix_len = strlen(str);
	for (i = 0; i < FIO_ARRAY_SIZE(delimiter); i++) {
		postfix = strchr(str, delimiter[i]);
		candidate_len = (size_t)(postfix - str);
		if (postfix && candidate_len < prefix_len)
			prefix_len = candidate_len;
	}

	return prefix_len;
}

/*
 * [한국어] posval 매칭 시 비교할 문자열 길이 결정
 * posval의 이름과 입력 문자열 중 긴 쪽을 기준으로 비교하여 정확한 매칭을 보장한다.
 */
static int str_match_len(const struct value_pair *vp, const char *str)
{
	return max(strlen(vp->ival), opt_len(str));
}

/*
 * [한국어] val_store 매크로 — 파싱된 값을 thread_options 내 대상 변수에 저장
 *
 * @ptr:  대상 포인터 (이 매크로 내에서 td_var()로 계산)
 * @val:  저장할 값
 * @off:  thread_options 내 오프셋
 * @or:   1이면 OR 연산으로 합산 (비트 플래그), 0이면 단순 대입
 * @data: thread_options 포인터
 * @o:    fio_option 구조체 (prof_opts 확인용)
 */
#define val_store(ptr, val, off, or, data, o)		\
	do {						\
		ptr = td_var((data), (o), (off));	\
		if ((or))				\
			*ptr |= (val);			\
		else					\
			*ptr = (val);			\
	} while (0)

/* [한국어] 옵션 타입을 문자열로 반환 (디버그 출력용) */
static const char *opt_type_name(const struct fio_option *o)
{
	compiletime_assert(FIO_ARRAY_SIZE(opt_type_names) - 1 == FIO_OPT_UNSUPPORTED,
				"opt_type_names[] index");

	if (o->type <= FIO_OPT_UNSUPPORTED)
		return opt_type_names[o->type];

	return "OPT_UNKNOWN?";
}

/* [한국어] 값이 옵션의 최대값을 초과하는지 검사 (is_uint면 부호 있는 비교 고려) */
static bool val_too_large(const struct fio_option *o, unsigned long long val,
			  bool is_uint)
{
	if (!o->maxval)
		return false;

	if (is_uint) {
		if ((int) val < 0)
			return (int) val > (int) o->maxval;
		return (unsigned int) val > o->maxval;
	}

	return val > o->maxval;
}

/* [한국어] 값이 옵션의 최소값 미만인지 검사 */
static bool val_too_small(const struct fio_option *o, unsigned long long val,
			  bool is_uint)
{
	if (!o->minval)
		return false;

	if (is_uint)
		return (int) val < o->minval;

	return val < o->minval;
}

/*
 * [한국어] __handle_option() — 개별 옵션의 실제 파싱 로직
 *
 * 옵션 타입(o->type)에 따라 분기하여 값을 파싱하고 thread_options에 저장한다.
 *
 * @o:     파싱할 옵션 정의
 * @ptr:   값 문자열 (예: "4k", "read", "1")
 * @data:  thread_options 포인터 (값 저장 대상)
 * @first: 첫 번째 값인지 여부 (콤마 구분 복수 값 중)
 * @more:  뒤에 더 많은 값이 있는지 여부
 * @curr:  현재 값의 인덱스 (0, 1, 2 — 읽기/쓰기/트림 분리용)
 *
 * 타입별 처리:
 *   FIO_OPT_STR/STR_ULL/STR_MULTI: posval[]에서 문자열 매칭 → 정수 저장
 *   FIO_OPT_STR_VAL/INT/ULL:       str_to_decimal()로 숫자 변환 → 저장
 *   FIO_OPT_STR_VAL_TIME:          시간 단위 변환
 *   FIO_OPT_STR_VAL_ZONE:          'z' 접미사의 존 값 처리
 *   FIO_OPT_FLOAT_LIST:            부동소수점 리스트 파싱
 *   FIO_OPT_STR_STORE:             문자열 그대로 strdup 저장
 *   FIO_OPT_RANGE:                 "min-max" 범위 파싱
 *   FIO_OPT_BOOL/STR_SET:          불린 값 파싱
 */
static int __handle_option(const struct fio_option *o, const char *ptr,
			   void *data, int first, int more, int curr)
{
	int il=0, *ilp;                  /* 정수값 및 포인터 */
	fio_fp64_t *flp;                 /* 부동소수점 리스트 포인터 */
	long long ull, *ullp;            /* 64비트 정수값 및 포인터 */
	long ul2;
	long long ull1, ull2;            /* 범위(RANGE)의 하한/상한 */
	double uf;                       /* 부동소수점 값 */
	char **cp = NULL;                /* 문자열 저장 포인터 */
	int ret = 0, is_time = 0;
	const struct value_pair *vp;
	struct value_pair posval[PARSE_MAX_VP];  /* posval 정렬용 로컬 복사본 */
	int i, all_skipped = 1;

	dprint(FD_PARSE, "__handle_option=%s, type=%s, ptr=%s\n", o->name,
							opt_type_name(o), ptr);

	/* STR_SET와 STR은 인자 없이도 동작 가능 (플래그 설정) */
	if (!ptr && o->type != FIO_OPT_STR_SET && o->type != FIO_OPT_STR) {
		log_err("Option %s requires an argument\n", o->name);
		return 1;
	}

	switch (o->type) {
	/* [한국어] 문자열 선택형: posval[]에서 입력 문자열과 매칭되는 값을 찾아 저장 */
	case FIO_OPT_STR:
	case FIO_OPT_STR_ULL:
	case FIO_OPT_STR_MULTI: {
		fio_opt_str_fn *fn = o->cb;

		posval_sort(o, posval);

		ret = 1;
		for (i = 0; i < PARSE_MAX_VP; i++) {
			vp = &posval[i];
			if (!vp->ival || vp->ival[0] == '\0')
				continue;
			all_skipped = 0;
			if (!ptr)
				break;
			if (!strncmp(vp->ival, ptr, str_match_len(vp, ptr))) {
				ret = 0;
				if (!o->off1)
					continue;
				if (o->type == FIO_OPT_STR_ULL)
					val_store(ullp, vp->oval, o->off1, vp->orval, data, o);
				else
					val_store(ilp, vp->oval, o->off1, vp->orval, data, o);
				continue;
			}
		}

		if (ret && !all_skipped)
			show_option_values(o);
		else if (fn)
			ret = fn(data, ptr);
		break;
	}
	/*
	 * [한국어] 숫자값 파싱: 시간/바이트 단위 접미사 처리 후 정수로 변환
	 * STR_VAL_TIME → is_time=1로 시간 단위 처리
	 * STR_VAL_ZONE → 'z' 접미사로 존 번호 인코딩
	 * INT, ULL, STR_VAL → 일반 숫자/바이트 단위 변환
	 */
	case FIO_OPT_STR_VAL_TIME:
		is_time = 1;
		fio_fallthrough;
	case FIO_OPT_ULL:
	case FIO_OPT_INT:
	case FIO_OPT_STR_VAL:
	case FIO_OPT_STR_VAL_ZONE:
	{
		fio_opt_str_val_fn *fn = o->cb;
		char tmp[128], *p;
		size_t len = strlen(ptr);

		if (len > 0 && ptr[len - 1] == 'z') {
			if (o->type == FIO_OPT_STR_VAL_ZONE) {
				char *ep;
				unsigned long long val;

				errno = 0;
				val = strtoul(ptr, &ep, 10);
				if (errno == 0 && ep != ptr && *ep == 'z') {
					ull = ZONE_BASE_VAL + (uint32_t)val;
					ret = 0;
					goto store_option_value;
				} else {
					log_err("%s: unexpected zone value '%s'\n",
						o->name, ptr);
					return 1;
				}
			} else {
				log_err("%s: 'z' suffix isn't applicable\n",
					o->name);
				return 1;
			}
		}

		if (!is_time && o->is_time)
			is_time = o->is_time;

		snprintf(tmp, sizeof(tmp), "%s", ptr);
		p = strchr(tmp, ',');
		if (p)
			*p = '\0';

		if (is_time)
			ret = check_str_time(tmp, &ull, o->is_seconds);
		else
			ret = check_str_bytes(tmp, &ull, data);

		dprint(FD_PARSE, "  ret=%d, out=%llu\n", ret, ull);

		if (ret)
			break;
		if (o->pow2 && !is_power_of_2(ull)) {
			log_err("%s: must be a power-of-2\n", o->name);
			return 1;
		}

		if (val_too_large(o, ull, o->type == FIO_OPT_INT)) {
			log_err("%s: max value out of range: %llu"
				" (%llu max)\n", o->name, ull, o->maxval);
			return 1;
		}
		if (val_too_small(o, ull, o->type == FIO_OPT_INT)) {
			log_err("%s: min value out of range: %lld"
				" (%d min)\n", o->name, ull, o->minval);
			return 1;
		}
		if (o->posval[0].ival) {
			posval_sort(o, posval);

			ret = 1;
			for (i = 0; i < PARSE_MAX_VP; i++) {
				vp = &posval[i];
				if (!vp->ival || vp->ival[0] == '\0')
					continue;
				if (vp->oval == ull) {
					ret = 0;
					break;
				}
			}
			if (ret) {
				log_err("fio: value %llu not allowed:\n", ull);
				show_option_values(o);
				return 1;
			}
		}

store_option_value:
		/*
		 * [한국어] 값 저장 단계
		 * 콜백(fn)이 있으면 콜백에 위임, 없으면 val_store()로 직접 저장.
		 * curr가 0이면 off1(읽기용), 1이면 off2(쓰기용), 2이면 off3(트림용)에 저장.
		 * more가 0(마지막 값)이면 아직 설정 안 된 off2/off3에도 같은 값을 복사한다.
		 * 이는 "bs=4k"처럼 하나만 지정하면 읽기/쓰기 모두 4k로 설정하는 동작이다.
		 */
		if (fn)
			ret = fn(data, &ull);
		else {
			if (o->type == FIO_OPT_INT) {
				if (first)
					val_store(ilp, ull, o->off1, 0, data, o);
				if (curr == 1) {
					if (o->off2)
						val_store(ilp, ull, o->off2, 0, data, o);
				}
				if (curr == 2) {
					if (o->off3)
						val_store(ilp, ull, o->off3, 0, data, o);
				}
				if (!more) {
					if (curr < 1) {
						if (o->off2)
							val_store(ilp, ull, o->off2, 0, data, o);
					}
					if (curr < 2) {
						if (o->off3)
							val_store(ilp, ull, o->off3, 0, data, o);
					}
				}
			} else if (o->type == FIO_OPT_ULL) {
				if (first)
					val_store(ullp, ull, o->off1, 0, data, o);
				if (curr == 1) {
					if (o->off2)
						val_store(ullp, ull, o->off2, 0, data, o);
				}
				if (curr == 2) {
					if (o->off3)
						val_store(ullp, ull, o->off3, 0, data, o);
				}
				if (!more) {
					if (curr < 1) {
						if (o->off2)
							val_store(ullp, ull, o->off2, 0, data, o);
					}
					if (curr < 2) {
						if (o->off3)
							val_store(ullp, ull, o->off3, 0, data, o);
					}
				}
			} else {
				if (first)
					val_store(ullp, ull, o->off1, 0, data, o);
				if (!more) {
					if (o->off2)
						val_store(ullp, ull, o->off2, 0, data, o);
				}
			}
		}
		break;
	}
	/*
	 * [한국어] 부동소수점 리스트 파싱
	 * ':'으로 구분된 실수 목록을 배열에 저장한다.
	 * 예: percentile_list=50.0:90.0:99.0:99.9
	 * off1은 fio_fp64_t 배열을, off2는 최대 소수점 자릿수(precision)를 가리킨다.
	 */
	case FIO_OPT_FLOAT_LIST: {
		char *cp2;

		if (first) {
			/* [한국어] 첫 값이면 정밀도를 0으로, 리스트를 0.0으로 초기화 */
			if (o->off2) {
				ul2 = 0;
				ilp = td_var(data, o, o->off2);
				*ilp = ul2;
			}

			flp = td_var(data, o, o->off1);
			for(i = 0; i < o->maxlen; i++)
				flp[i].u.f = 0.0;
		}
		if (curr >= o->maxlen) {
			log_err("the list exceeding max length %d\n",
					o->maxlen);
			return 1;
		}
		if (!str_to_float(ptr, &uf, 0)) { /* this breaks if we ever have lists of times */
			log_err("not a floating point value: %s\n", ptr);
			return 1;
		}
		if (o->minfp || o->maxfp) {
			if (uf > o->maxfp) {
				log_err("value out of range: %f"
					" (range max: %f)\n", uf, o->maxfp);
				return 1;
			}
			if (uf < o->minfp) {
				log_err("value out of range: %f"
					" (range min: %f)\n", uf, o->minfp);
				return 1;
			}
		}

		flp = td_var(data, o, o->off1);
		flp[curr].u.f = uf;

		dprint(FD_PARSE, "  out=%f\n", uf);

		/*
		** Calculate precision for output by counting
		** number of digits after period. Find first
		** period in entire remaining list each time
		*/
		cp2 = strchr(ptr, '.');
		if (cp2 != NULL) {
			int len = 0;

			while (*++cp2 != '\0' && *cp2 >= '0' && *cp2 <= '9')
				len++;

			if (o->off2) {
				ilp = td_var(data, o, o->off2);
				if (len > *ilp)
					*ilp = len;
			}
		}

		break;
	}
	/*
	 * [한국어] 문자열 저장형: 값을 strdup()으로 복사하여 그대로 저장
	 * 예: filename=/dev/sda → td->o.filename = strdup("/dev/sda")
	 * posval이 정의되어 있으면 매칭 검증도 수행한다.
	 * ':'이 있으면 앞부분은 메인 값, 뒷부분은 서브옵션 콜백에 전달한다.
	 */
	case FIO_OPT_STR_STORE: {
		fio_opt_str_fn *fn = o->cb;

		if (!strlen(ptr))
			return 1;

		if (o->off1) {
			cp = td_var(data, o, o->off1);
			if (*cp)
				free(*cp);
			*cp = strdup(ptr);
			if (strlen(ptr) > o->maxlen - 1) {
				log_err("value exceeds max length of %d\n",
					o->maxlen);
				return 1;
			}
		}

		if (fn)
			ret = fn(data, ptr);
		else if (o->posval[0].ival) {
			posval_sort(o, posval);

			ret = 1;
			for (i = 0; i < PARSE_MAX_VP; i++) {
				vp = &posval[i];
				if (!vp->ival || vp->ival[0] == '\0' || !cp)
					continue;
				all_skipped = 0;
				if (!strncmp(vp->ival, ptr, str_match_len(vp, ptr))) {
					char *rest;

					ret = 0;
					if (vp->cb)
						fn = vp->cb;
					rest = strstr(*cp ?: ptr, ":");
					if (rest) {
						if (*cp)
							*rest = '\0';
						ptr = rest + 1;
					} else
						ptr = NULL;
					break;
				}
			}
		}

		if (!all_skipped) {
			if (ret && !*cp)
				show_option_values(o);
			else if (ret && *cp)
				ret = 0;
			else if (fn && ptr)
				ret = fn(data, ptr);
		}

		break;
	}
	/*
	 * [한국어] 범위값 파싱: "min-max" 또는 "min:max" 형식
	 * 예: bsrange=4k-8k → off1=4096, off2=8192
	 * 콤마로 읽기/쓰기/트림 범위를 분리: bsrange=4k-8k,1k-4k,2k-16k
	 * min > max이면 자동으로 스왑한다.
	 */
	case FIO_OPT_RANGE: {
		char tmp[128];
		char *p1, *p2;

		snprintf(tmp, sizeof(tmp), "%s", ptr);

		/* [한국어] 콤마로 읽기/쓰기 범위를 분리 */
		p1 = strchr(tmp, ',');
		if (p1)
			*p1 = '\0';

		p1 = strchr(tmp, '-');
		if (!p1) {
			p1 = strchr(tmp, ':');
			if (!p1) {
				ret = 1;
				break;
			}
		}

		p2 = p1 + 1;
		*p1 = '\0';
		p1 = tmp;

		ret = 1;
		if (!check_range_bytes(p1, &ull1, data) &&
			!check_range_bytes(p2, &ull2, data)) {
			ret = 0;
			if (ull1 > ull2) {
				unsigned long long foo = ull1;

				ull1 = ull2;
				ull2 = foo;
			}

			if (first) {
				val_store(ullp, ull1, o->off1, 0, data, o);
				val_store(ullp, ull2, o->off2, 0, data, o);
			}
			if (curr == 1) {
				if (o->off3 && o->off4) {
					val_store(ullp, ull1, o->off3, 0, data, o);
					val_store(ullp, ull2, o->off4, 0, data, o);
				}
			}
			if (curr == 2) {
				if (o->off5 && o->off6) {
					val_store(ullp, ull1, o->off5, 0, data, o);
					val_store(ullp, ull2, o->off6, 0, data, o);
				}
			}
			if (!more) {
				if (curr < 1) {
					if (o->off3 && o->off4) {
						val_store(ullp, ull1, o->off3, 0, data, o);
						val_store(ullp, ull2, o->off4, 0, data, o);
					}
				}
				if (curr < 2) {
					if (o->off5 && o->off6) {
						val_store(ullp, ull1, o->off5, 0, data, o);
						val_store(ullp, ull2, o->off6, 0, data, o);
					}
				}
			}
		}

		break;
	}
	/*
	 * [한국어] 불린/플래그 파싱
	 * BOOL: 0 또는 1 (인자 필수)
	 * STR_SET: 인자 없으면 1로 설정 (예: --direct)
	 * neg=1이면 값을 반전 (예: nounlink → unlink=0)
	 */
	case FIO_OPT_BOOL:
	case FIO_OPT_STR_SET: {
		fio_opt_int_fn *fn = o->cb;

		if (ptr)
			ret = check_int(ptr, &il);
		else if (o->type == FIO_OPT_BOOL)
			ret = 1;
		else
			il = 1;

		dprint(FD_PARSE, "  ret=%d, out=%d\n", ret, il);

		if (ret)
			break;

		if (o->maxval && il > (int) o->maxval) {
			log_err("max value out of range: %d (%llu max)\n",
								il, o->maxval);
			return 1;
		}
		if (o->minval && il < o->minval) {
			log_err("min value out of range: %d (%d min)\n",
								il, o->minval);
			return 1;
		}

		if (o->neg)
			il = !il;

		if (fn)
			ret = fn(data, &il);
		else {
			if (first)
				val_store(ilp, il, o->off1, 0, data, o);
			if (!more) {
				if (o->off2)
					val_store(ilp, il, o->off2, 0, data, o);
			}
		}
		break;
	}
	case FIO_OPT_DEPRECATED:
		ret = 1;
		fio_fallthrough;
	case FIO_OPT_SOFT_DEPRECATED:
		log_info("Option %s is deprecated\n", o->name);
		break;
	default:
		log_err("Bad option type %u\n", o->type);
		ret = 1;
	}

	if (ret)
		return ret;

	/* [한국어] 옵션에 verify 콜백이 있으면 파싱 결과의 유효성을 추가 검증 */
	if (o->verify) {
		ret = o->verify(o, data);
		if (ret) {
			log_err("Correct format for offending option\n");
			log_err("%20s: %s\n", o->name, o->help);
			show_option_help(o, 1);
		}
	}

	return ret;
}

/*
 * [한국어] handle_option() — 콤마/콜론 구분 복수 값 처리
 *
 * "4k,8k" 같은 복수 값을 순회하며 __handle_option()을 반복 호출한다.
 * curr 인덱스가 0, 1, 2로 증가하면서 off1, off2, off3에 각각 저장된다.
 *
 * 타입에 따라 구분자가 다르다:
 *   - STR_STORE, STR, STR_ULL: 콤마 분할 안 함 (값 전체가 하나의 문자열)
 *   - FLOAT_LIST: ':'으로 분할
 *   - 나머지: ','로 먼저, 없으면 ':'나 '-'로 분할
 */
static int handle_option(const struct fio_option *o, const char *__ptr,
			 void *data)
{
	char *o_ptr, *ptr, *ptr2;
	int ret, done;

	dprint(FD_PARSE, "handle_option=%s, ptr=%s\n", o->name, __ptr);

	o_ptr = ptr = NULL;
	if (__ptr)
		o_ptr = ptr = strdup(__ptr);  /* 원본 보존을 위해 복사 */

	/*
	 * [한국어] 콤마/콜론으로 구분된 다음 파라미터가 있는지 확인하며 순회
	 * done은 현재 인덱스(curr), ptr2는 다음 값의 시작점
	 */
	done = 0;
	ret = 1;
	do {
		int __ret;

		ptr2 = NULL;
		if (ptr &&
		    (o->type != FIO_OPT_STR_STORE) &&
		    (o->type != FIO_OPT_STR) &&
		    (o->type != FIO_OPT_STR_ULL) &&
		    (o->type != FIO_OPT_FLOAT_LIST)) {
			ptr2 = strchr(ptr, ',');
			if (ptr2 && *(ptr2 + 1) == '\0')
				*ptr2 = '\0';
			if (o->type != FIO_OPT_STR_MULTI && o->type != FIO_OPT_RANGE) {
				if (!ptr2)
					ptr2 = strchr(ptr, ':');
				if (!ptr2)
					ptr2 = strchr(ptr, '-');
			}
		} else if (ptr && o->type == FIO_OPT_FLOAT_LIST) {
			ptr2 = strchr(ptr, ':');
		}

		/*
		 * Don't return early if parsing the first option fails - if
		 * we are doing multiple arguments, we can allow the first one
		 * being empty.
		 */
		__ret = __handle_option(o, ptr, data, !done, !!ptr2, done);
		if (ret)
			ret = __ret;

		if (!ptr2)
			break;

		ptr = ptr2 + 1;
		done++;
	} while (1);

	if (o_ptr)
		free(o_ptr);
	return ret;
}

/*
 * [한국어] 옵션 이름으로 fio_option 구조체를 찾는다 (변경 가능 버전)
 * o_match()는 name과 alias를 모두 비교한다.
 * UNSUPPORTED 타입은 경고를 출력하고 건너뛴다.
 */
struct fio_option *find_option(struct fio_option *options, const char *opt)
{
	struct fio_option *o;

	for (o = &options[0]; o->name; o++) {
		if (!o_match(o, opt))
			continue;
		if (o->type == FIO_OPT_UNSUPPORTED) {
			log_err("Option <%s>: %s\n", o->name, o->help);
			continue;
		}

		return o;
	}

	return NULL;
}

/* [한국어] 옵션 이름으로 fio_option 구조체를 찾는다 (const 버전) */
const struct fio_option *
find_option_c(const struct fio_option *options, const char *opt)
{
	const struct fio_option *o;

	for (o = &options[0]; o->name; o++) {
		if (!o_match(o, opt))
			continue;
		if (o->type == FIO_OPT_UNSUPPORTED) {
			log_err("Option <%s>: %s\n", o->name, o->help);
			continue;
		}

		return o;
	}

	return NULL;
}

/*
 * [한국어] "opt=val" 문자열에서 옵션 이름과 값을 분리
 * '='을 찾아 앞부분으로 옵션을 검색하고, 뒷부분을 *post에 반환한다.
 * '='이 없으면 전체를 옵션 이름으로 처리하고 *post=NULL.
 */
static const struct fio_option *
get_option(char *opt, const struct fio_option *options, char **post)
{
	const struct fio_option *o;
	char *ret;

	ret = strchr(opt, '=');
	if (ret) {
		*post = ret;
		*ret = '\0';
		ret = opt;
		(*post)++;
		strip_blank_end(ret);
		o = find_option_c(options, ret);
	} else {
		o = find_option_c(options, opt);
		*post = NULL;
	}

	return o;
}

/*
 * [한국어] 옵션 우선순위 비교 함수 (qsort용)
 * prio가 높은 옵션이 먼저 파싱되도록 내림차순 정렬한다.
 * 예: ioengine 옵션은 다른 엔진 옵션보다 먼저 파싱되어야 한다.
 */
static int opt_cmp(const void *p1, const void *p2)
{
	const struct fio_option *o;
	char *s, *foo;
	int prio1, prio2;

	prio1 = prio2 = 0;

	if (*(char **)p1) {
		s = strdup(*((char **) p1));
		o = get_option(s, __fio_options, &foo);
		if (o)
			prio1 = o->prio;
		free(s);
	}
	if (*(char **)p2) {
		s = strdup(*((char **) p2));
		o = get_option(s, __fio_options, &foo);
		if (o)
			prio2 = o->prio;
		free(s);
	}

	return prio2 - prio1;
}

/* [한국어] 옵션 배열을 우선순위(prio) 기준 내림차순으로 정렬 */
void sort_options(char **opts, const struct fio_option *options, int num_opts)
{
	__fio_options = options;
	qsort(opts, num_opts, sizeof(char *), opt_cmp);
	__fio_options = NULL;
}

/*
 * [한국어] 파싱된 옵션을 덤프 리스트에 추가
 * 나중에 잡 파일을 재출력(--output)할 때 사용된다.
 */
static void add_to_dump_list(const struct fio_option *o,
			     struct flist_head *dump_list, const char *post)
{
	struct print_option *p;

	if (!dump_list)
		return;

	p = malloc(sizeof(*p));
	p->name = strdup(o->name);
	if (post)
		p->value = strdup(post);
	else
		p->value = NULL;

	flist_add_tail(&p->list, dump_list);
}

/*
 * [한국어] 커맨드라인 옵션 파싱 (--name value 형식)
 * 옵션을 이름으로 찾아서 handle_option()으로 값을 처리한다.
 */
int parse_cmd_option(const char *opt, const char *val,
		     const struct fio_option *options, void *data,
		     struct flist_head *dump_list)
{
	const struct fio_option *o;

	o = find_option_c(options, opt);
	if (!o) {
		log_err("Bad option <%s>\n", opt);
		return 1;
	}

	if (handle_option(o, val, data)) {
		log_err("fio: failed parsing %s=%s\n", opt, val);
		return 1;
	}

	add_to_dump_list(o, dump_list, val);
	return 0;
}

/*
 * [한국어] 메인 옵션 파싱 함수 — 잡 파일의 "opt=val" 라인을 처리
 *
 * @opt:   파싱할 문자열 (수정 가능, '='을 '\0'으로 대체)
 * @input: 원본 입력 문자열 (에러 메시지용)
 * @options: 옵션 정의 배열 (fio_options[])
 * @o:     매칭된 옵션 포인터를 반환
 * @data:  thread_options 포인터
 * @dump_list: 옵션 덤프 리스트 (재출력용)
 */
int parse_option(char *opt, const char *input, const struct fio_option *options,
		 const struct fio_option **o, void *data,
		 struct flist_head *dump_list)
{
	char *post;

	if (!opt) {
		log_err("fio: failed parsing %s\n", input);
		*o = NULL;
		return 1;
	}

	*o = get_option(opt, options, &post);
	if (!*o) {
		if (post) {
			int len = strlen(opt);
			if (opt + len + 1 != post)
				memmove(opt + len + 1, post, strlen(post));
			opt[len] = '=';
		}
		return 1;
	}

	if (handle_option(*o, post, data)) {
		log_err("fio: failed parsing %s\n", input);
		return 1;
	}

	add_to_dump_list(*o, dump_list, post);
	return 0;
}

/*
 * [한국어] 레벤슈타인 편집 거리 계산 (동적 프로그래밍)
 *
 * 사용자가 오타를 낸 옵션명에 대해 가장 유사한 옵션을 추천하는 데 사용한다.
 * 예: "iopdepth" 입력 → "iodepth"를 추천 (편집 거리 1)
 *
 * 알고리즘: Wagner-Fischer (O(m*n) 시간, O(n) 공간)
 * 두 행(p, q)만 사용하여 공간을 절약한다.
 */
int string_distance(const char *s1, const char *s2)
{
	unsigned int s1_len = strlen(s1);
	unsigned int s2_len = strlen(s2);
	unsigned int *p, *q, *r;
	unsigned int i, j;

	p = malloc(sizeof(unsigned int) * (s2_len + 1));
	q = malloc(sizeof(unsigned int) * (s2_len + 1));

	p[0] = 0;
	for (i = 1; i <= s2_len; i++)
		p[i] = p[i - 1] + 1;

	for (i = 1; i <= s1_len; i++) {
		q[0] = p[0] + 1;
		for (j = 1; j <= s2_len; j++) {
			unsigned int sub = p[j - 1];
			unsigned int pmin;

			if (s1[i - 1] != s2[j - 1])
				sub++;

			pmin = min(q[j - 1] + 1, sub);
			q[j] = min(p[j] + 1, pmin);
		}
		r = p;
		p = q;
		q = r;
	}

	i = p[s2_len];
	free(p);
	free(q);
	return i;
}

/*
 * [한국어] 편집 거리가 추천할 만큼 가까운지 판단
 * 기준: 옵션 이름 길이의 1/2 이하면 추천할 만함
 * 예: "iodepth"(7글자) → 거리 3 이하면 추천
 */
int string_distance_ok(const char *opt, int distance)
{
	size_t len;

	len = strlen(opt);
	len = (len + 1) / 2;
	return distance <= len;
}

/* [한국어] 부모-자식 관계에서 자식 옵션을 찾는다 (도움말 트리 출력용) */
static const struct fio_option *find_child(const struct fio_option *options,
					   const struct fio_option *o)
{
	const struct fio_option *__o;

	for (__o = options + 1; __o->name; __o++)
		if (__o->parent && !strcmp(__o->parent, o->name))
			return __o;

	return NULL;
}

/* [한국어] 옵션을 들여쓰기와 함께 출력 (부모-자식 계층 표현) */
static void __print_option(const struct fio_option *o,
			   const struct fio_option *org,
			   int level)
{
	char name[256], *p;
	int depth;

	if (!o)
		return;

	p = name;
	depth = level;
	while (depth--)
		p += sprintf(p, "%s", "  ");

	sprintf(p, "%s", o->name);

	log_info("%-24s: %s\n", name, o->help);
}

/* [한국어] 옵션과 그 하위 옵션들을 재귀적으로 출력 */
static void print_option(const struct fio_option *o)
{
	const struct fio_option *parent;
	const struct fio_option *__o;
	unsigned int printed;
	unsigned int level;

	__print_option(o, NULL, 0);
	parent = o;
	level = 0;
	do {
		level++;
		printed = 0;

		while ((__o = find_child(o, parent)) != NULL) {
			__print_option(__o, o, level);
			o = __o;
			printed++;
		}

		parent = o;
	} while (printed);
}

/*
 * [한국어] 옵션 도움말 표시 (fio --help name 또는 --cmdhelp=name)
 *
 * name이 NULL이거나 "all"이면 전체 옵션 목록을 출력한다.
 * 특정 이름이면 정확히 매칭되는 옵션의 상세 도움말을 출력한다.
 * 매칭이 없으면 레벤슈타인 거리로 가장 유사한 옵션을 추천한다.
 */
int show_cmd_help(const struct fio_option *options, const char *name)
{
	const struct fio_option *o, *closest;
	unsigned int best_dist = -1U;
	int found = 0;
	int show_all = 0;

	if (!name || !strcmp(name, "all"))
		show_all = 1;

	closest = NULL;
	best_dist = -1;
	for (o = &options[0]; o->name; o++) {
		int match = 0;

		if (o->type == FIO_OPT_DEPRECATED ||
		    o->type == FIO_OPT_SOFT_DEPRECATED)
			continue;
		if (!exec_profile && o->prof_name)
			continue;
		if (exec_profile && !(o->prof_name && !strcmp(exec_profile, o->prof_name)))
			continue;

		if (name) {
			if (!strcmp(name, o->name) ||
			    (o->alias && !strcmp(name, o->alias)))
				match = 1;
			else {
				unsigned int dist;

				dist = string_distance(name, o->name);
				if (dist < best_dist) {
					best_dist = dist;
					closest = o;
				}
			}
		}

		if (show_all || match) {
			found = 1;
			if (match)
				log_info("%20s: %s\n", o->name, o->help);
			if (show_all) {
				if (!o->parent)
					print_option(o);
				continue;
			}
		}

		if (!match)
			continue;

		show_option_help(o, 0);
	}

	if (found)
		return 0;

	log_err("No such command: %s", name);

	/*
	 * Only print an appropriately close option, one where the edit
	 * distance isn't too big. Otherwise we get crazy matches.
	 */
	if (closest && best_dist < 3) {
		log_info(" - showing closest match\n");
		log_info("%20s: %s\n", closest->name, closest->help);
		show_option_help(closest, 0);
	} else
		log_info("\n");

	return 1;
}

/*
 * [한국어] 기본값 적용
 * 옵션 배열을 순회하며 def(기본값 문자열)가 정의된 옵션에 대해
 * handle_option()으로 기본값을 파싱하여 저장한다.
 * init.c에서 thread_data 초기화 시 호출된다.
 */
void fill_default_options(void *data, const struct fio_option *options)
{
	const struct fio_option *o;

	dprint(FD_PARSE, "filling default options\n");

	for (o = &options[0]; o->name; o++)
		if (o->def)
			handle_option(o, o->def, data);
}

/*
 * [한국어] 개별 옵션 초기화 및 무결성 검증
 *
 * 타입에 따라 자동으로 min/max를 설정하고, 필수 필드 누락을 경고한다:
 *   - BOOL: minval=0, maxval=1
 *   - INT: maxval=UINT_MAX (미설정 시)
 *   - ULL: maxval=ULLONG_MAX (미설정 시)
 *   - cb와 off1 모두 없으면 경고 (값을 저장할 곳이 없음)
 *   - category 미지정이면 FIO_OPT_C_GENERAL로 설정
 */
static void option_init(struct fio_option *o)
{
	if (o->type == FIO_OPT_DEPRECATED || o->type == FIO_OPT_UNSUPPORTED ||
	    o->type == FIO_OPT_SOFT_DEPRECATED)
		return;
	if (o->name && !o->lname)
		log_err("Option %s: missing long option name\n", o->name);
	if (o->type == FIO_OPT_BOOL) {
		o->minval = 0;
		o->maxval = 1;
	}
	if (o->type == FIO_OPT_INT) {
		if (!o->maxval)
			o->maxval = UINT_MAX;
	}
	if (o->type == FIO_OPT_ULL) {
		if (!o->maxval)
			o->maxval = ULLONG_MAX;
	}
	if (o->type == FIO_OPT_STR_SET && o->def && !o->no_warn_def) {
		log_err("Option %s: string set option with"
				" default will always be true\n", o->name);
	}
	if (!o->cb && !o->off1)
		log_err("Option %s: neither cb nor offset given\n", o->name);
	if (!o->category) {
		log_info("Option %s: no category defined. Setting to misc\n", o->name);
		o->category = FIO_OPT_C_GENERAL;
		o->group = FIO_OPT_G_INVALID;
	}
}

/*
 * [한국어] 옵션 배열 전체 초기화
 * 각 옵션에 대해 option_init()을 호출하고,
 * inverse 관계가 있는 옵션의 포인터를 캐싱한다.
 */
void options_init(struct fio_option *options)
{
	struct fio_option *o;

	dprint(FD_PARSE, "init options\n");

	for (o = &options[0]; o->name; o++) {
		option_init(o);
		if (o->inverse)
			o->inv_opt = find_option(options, o->inverse);
	}
}

/*
 * [한국어] STR_STORE 타입 옵션의 문자열을 strdup()으로 복제
 * fork/clone 시 자식이 독립적인 복사본을 가지도록 한다.
 */
void options_mem_dupe(const struct fio_option *options, void *data)
{
	const struct fio_option *o;
	char **ptr;

	dprint(FD_PARSE, "dup options\n");

	for (o = &options[0]; o->name; o++) {
		if (o->type != FIO_OPT_STR_STORE)
			continue;

		ptr = td_var(data, o, o->off1);
		if (*ptr)
			*ptr = strdup(*ptr);
	}
}

/*
 * [한국어] STR_STORE 타입 옵션의 문자열 메모리 해제
 * 스레드 종료 시 호출되어 strdup()된 문자열을 free한다.
 * no_free=1인 옵션은 해제하지 않는다 (공유 메모리 등).
 */
void options_free(const struct fio_option *options, void *data)
{
	const struct fio_option *o;
	char **ptr;

	dprint(FD_PARSE, "free options\n");

	for (o = &options[0]; o->name; o++) {
		if (o->type != FIO_OPT_STR_STORE || !o->off1 || o->no_free)
			continue;

		ptr = td_var(data, o, o->off1);
		if (*ptr) {
			free(*ptr);
			*ptr = NULL;
		}
	}
}
