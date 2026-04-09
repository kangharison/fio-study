/*
 * [한국어] parse.h - fio 옵션 파싱 시스템의 핵심 헤더
 *
 * 이 파일은 fio의 설정(옵션) 파싱 프레임워크를 정의한다.
 * 주요 내용:
 *   1) fio_opt_type     - 옵션의 타입 열거형 (문자열, 정수, 불린, 범위 등)
 *   2) value_pair       - 문자열 옵션 값과 정수 값의 매핑 쌍
 *   3) fio_option       - 개별 옵션 정의 구조체 (이름, 타입, 오프셋, 범위, 콜백 등)
 *   4) 파싱 API         - parse_option(), fill_default_options(), options_init() 등
 *   5) 유틸리티 함수     - str_to_decimal(), check_str_bytes(), string_distance() 등
 *   6) td_var() 인라인  - 옵션 오프셋으로부터 thread_data 내 실제 변수 포인터를 계산
 *
 * fio 옵션 시스템의 작동 원리:
 *   - 각 옵션은 fio_option 구조체로 정의되며, off1~off6 필드는 thread_options 구조체 내
 *     해당 변수의 오프셋(offsetof)을 저장한다.
 *   - 파싱 시 td_var()를 통해 오프셋 → 실제 메모리 주소로 변환하여 값을 저장한다.
 *   - posval[] 배열은 문자열 ↔ 정수 매핑을 정의한다 (예: "read" → DDIR_READ).
 */
#ifndef FIO_PARSE_H
#define FIO_PARSE_H

#include <inttypes.h>
#include "flist.h"           /* fio 연결 리스트 (flist_head) */

/*
 * [한국어] 옵션 타입 열거형
 * fio의 각 옵션이 어떤 종류의 값을 받는지 정의한다.
 */
enum fio_opt_type {
	FIO_OPT_INVALID = 0,     /* 무효 옵션 (초기화 용) */
	FIO_OPT_STR,             /* 문자열 선택형: posval[]에서 매칭 (예: rw=read) */
	FIO_OPT_STR_ULL,         /* 문자열 선택형이지만 64비트 값으로 저장 */
	FIO_OPT_STR_MULTI,       /* 복수 문자열 선택 (비트 OR 가능) */
	FIO_OPT_STR_VAL,         /* 단위 접미사 지원 문자열 값 (예: bs=4k → 4096) */
	FIO_OPT_STR_VAL_TIME,    /* 시간 단위 접미사 지원 (예: runtime=30s) */
	FIO_OPT_STR_STORE,       /* 문자열을 그대로 저장 (예: filename=/dev/sda) */
	FIO_OPT_RANGE,           /* 범위값 (예: bsrange=4k-8k) */
	FIO_OPT_INT,             /* 정수값 (32비트) */
	FIO_OPT_ULL,             /* 부호 없는 64비트 정수값 */
	FIO_OPT_BOOL,            /* 불린값 (0 또는 1) */
	FIO_OPT_FLOAT_LIST,      /* 부동소수점 리스트 (예: percentile_list=50:90:99) */
	FIO_OPT_STR_SET,         /* 설정 플래그 (인자 없이 존재만으로 활성화) */
	FIO_OPT_STR_VAL_ZONE,    /* 존(zone) 값 접미사 지원 (예: offset=4z) */
	FIO_OPT_DEPRECATED,      /* 폐지된 옵션 (사용 시 에러) */
	FIO_OPT_SOFT_DEPRECATED, /* 소프트 폐지 (경고만 출력) */
	FIO_OPT_UNSUPPORTED,     /* 미지원 옵션 (항상 마지막에 위치해야 함) */
};

/*
 * [한국어] 값 쌍 구조체 - 문자열 옵션과 정수 값의 매핑
 * 예: { .ival = "read", .oval = DDIR_READ, .help = "Sequential read" }
 * posval[] 배열에서 사용되어 사용자 입력 문자열을 내부 정수값으로 변환한다.
 */
struct value_pair {
	const char *ival;		/* 입력 문자열 (예: "read", "write", "randread") */
	unsigned long long oval;	/* 출력 값 (매핑될 정수/열거형 값) */
	const char *help;		/* 이 값에 대한 도움말 텍스트 */
	int orval;			/* 1이면 기존 값에 OR 연산으로 합산 (비트 플래그용) */
	void *cb;			/* 서브옵션 콜백 함수 포인터 */
};

#define OPT_LEN_MAX 	8192    /* 옵션 문자열 최대 길이 */
#define PARSE_MAX_VP	32      /* 하나의 옵션이 가질 수 있는 최대 posval 개수 */

/*
 * [한국어] 옵션 정의 구조체
 * fio의 모든 옵션(약 300개 이상)은 이 구조체의 배열로 정의된다.
 * options.c의 fio_options[] 배열에서 각 옵션마다 하나씩 초기화된다.
 *
 * off1~off6는 thread_options 구조체 내 대상 변수의 오프셋으로,
 * 파싱 결과를 td_var()를 통해 해당 위치에 직접 저장한다.
 * 범위(RANGE) 옵션은 off1/off2에 min/max를, 읽기/쓰기 분리 시 off3~off6도 사용한다.
 */
struct fio_option {
	const char *name;		/* 옵션 이름 (예: "rw", "bs", "iodepth") */
	const char *lname;		/* 긴 이름 (GUI/도움말용, 예: "readwrite") */
	const char *alias;		/* 호환용 이전 이름 (예: "readwrite" → "rw") */
	enum fio_opt_type type;		/* 옵션 타입 (위 열거형 참조) */
	unsigned int off1;		/* thread_options 내 대상 변수의 오프셋 #1 */
	unsigned int off2;		/* 오프셋 #2 (범위의 상한, 또는 쓰기용 값) */
	unsigned int off3;		/* 오프셋 #3 (트림용 또는 추가 분리값) */
	unsigned int off4;		/* 오프셋 #4 */
	unsigned int off5;		/* 오프셋 #5 */
	unsigned int off6;		/* 오프셋 #6 */
	unsigned long long maxval;	/* 최대 허용 값 */
	int minval;			/* 최소 허용 값 */
	double maxfp;			/* 부동소수점 최대값 */
	double minfp;			/* 부동소수점 최소값 */
	unsigned int interval;		/* 클라이언트 힌트: 적절한 조정 간격 */
	unsigned int maxlen;		/* 문자열 최대 길이 */
	int neg;			/* 1이면 저장 시 값을 반전 (예: nounlink → !unlink) */
	int prio;			/* 파싱 우선순위 (높을수록 먼저 처리) */
	void *cb;			/* 콜백 함수 (커스텀 파싱/검증 시 사용) */
	const char *help;		/* 도움말 텍스트 */
	const char *def;		/* 기본값 문자열 (NULL이면 기본값 없음) */
	struct value_pair posval[PARSE_MAX_VP]; /* 허용 가능한 값 목록 (문자열→정수 매핑) */
	const char *parent;		/* 부모 옵션 이름 (계층 구조 표시용) */
	int hide;			/* 부모가 설정 안 되면 숨김 */
	int hide_on_set;		/* 부모가 설정되면 숨김 (반대 동작) */
	const char *inverse;		/* 반대 옵션 이름 (예: serialize_overlap ↔ 다른 옵션) */
	struct fio_option *inv_opt;	/* inverse 옵션의 캐시된 포인터 */
	int (*verify)(const struct fio_option *, void *); /* 값 검증 콜백 */
	const char *prof_name;		/* 특정 프로파일에서만 유효 */
	void *prof_opts;		/* 프로파일 옵션 데이터 */
	uint64_t category;		/* 옵션 카테고리 (I/O, 파일, 통계 등) */
	uint64_t group;			/* 옵션 그룹 (같은 그룹끼리 묶어서 표시) */
	void *gui_data;			/* gfio GUI용 데이터 */
	int is_seconds;			/* 시간값의 기본 단위가 초인지 여부 */
	int is_time;			/* 시간 기반 값인지 여부 */
	int no_warn_def;		/* STR_SET에 def가 있어도 경고 안 함 */
	int pow2;			/* 2의 거듭제곱이어야 하는지 여부 */
	int no_free;			/* options_free()에서 해제하지 않음 */
};

/* [한국어] 옵션 파싱 API 함수들 */
extern int parse_option(char *, const char *, const struct fio_option *,
			const struct fio_option **, void *,
			struct flist_head *);
			/* "opt=val" 형식의 문자열을 파싱하여 data에 값 저장 */
extern void sort_options(char **, const struct fio_option *, int);
			/* 옵션 배열을 우선순위(prio) 기준으로 정렬 */
extern int parse_cmd_option(const char *t, const char *l,
			    const struct fio_option *, void *,
			    struct flist_head *);
			/* 커맨드라인 옵션 파싱 (--name=value 형식) */
extern int show_cmd_help(const struct fio_option *, const char *);
			/* 옵션 도움말 출력. 유사한 이름은 레벤슈타인 거리로 추천 */
extern void fill_default_options(void *, const struct fio_option *);
			/* 기본값(def)이 정의된 모든 옵션에 기본값 적용 */
extern void options_init(struct fio_option *);
			/* 옵션 배열 초기화 (min/max 설정, 무결성 검증) */
extern void options_mem_dupe(const struct fio_option *, void *);
			/* STR_STORE 타입 옵션의 문자열을 strdup()으로 복제 */
extern void options_free(const struct fio_option *, void *);
			/* STR_STORE 타입 옵션의 문자열 메모리 해제 */

/* [한국어] 문자열 변환 유틸리티 */
extern void strip_blank_front(char **);
			/* 문자열 앞쪽 공백 제거 */
extern void strip_blank_end(char *);
			/* 문자열 뒤쪽 공백 및 주석(; #) 제거 */
extern int str_to_decimal(const char *, long long *, int, void *, int, int);
			/* 문자열 → 정수 변환 (단위 접미사 k/m/g, 시간 접미사 s/ms/us 지원) */
extern int check_str_bytes(const char *p, long long *val, void *data);
			/* 바이트 단위 접미사가 붙은 문자열 변환 (예: "4k" → 4096) */
extern int check_str_time(const char *p, long long *val, int);
			/* 시간 단위 접미사가 붙은 문자열 변환 (예: "30s" → 30000000us) */
extern int str_to_float(const char *str, double *val, int is_time);
			/* 문자열 → 부동소수점 변환 */

/* [한국어] 레벤슈타인 편집 거리 - 오타 옵션명에 대한 유사 옵션 추천용 */
extern int string_distance(const char *s1, const char *s2);
			/* 두 문자열 간 편집 거리 계산 */
extern int string_distance_ok(const char *s1, int dist);
			/* 편집 거리가 추천할 만큼 가까운지 판단 (문자열 길이의 1/2 이하) */

/*
 * [한국어] 옵션 핸들러 콜백 타입 정의
 * 특정 옵션은 단순 오프셋 저장 대신 커스텀 콜백을 통해 값을 처리한다.
 */
typedef int (fio_opt_str_fn)(void *, const char *);      /* 문자열 옵션 콜백 */
typedef int (fio_opt_str_val_fn)(void *, long long *);   /* 숫자값 옵션 콜백 */
typedef int (fio_opt_int_fn)(void *, int *);             /* 정수/불린 옵션 콜백 */

struct thread_options;
/*
 * [한국어] td_var() - 옵션 오프셋을 실제 메모리 주소로 변환
 *
 * fio 옵션은 thread_options 구조체 내 변수의 오프셋(off1~off6)으로 대상을 지정한다.
 * 이 함수는 base 포인터(to) + offset으로 실제 변수 주소를 계산한다.
 * 프로파일 전용 옵션이면 prof_opts를 base로 사용한다.
 *
 * 예: o->off1 = offsetof(struct thread_options, bs[DDIR_READ]) 이면
 *     td_var(td, o, o->off1)은 &td->o.bs[DDIR_READ]를 반환한다.
 */
static inline void *td_var(void *to, const struct fio_option *o,
			   unsigned int offset)
{
	void *ret;

	if (o->prof_opts)           /* 프로파일 전용 옵션이면 프로파일 데이터를 base로 */
		ret = o->prof_opts;
	else
		ret = to;               /* 일반 옵션이면 thread_options를 base로 */

	return (void *) ((uintptr_t) ret + offset);  /* base + offset = 대상 변수 주소 */
}

/*
 * [한국어] 퍼센트/존 값 인코딩
 *
 * fio는 "50%" 같은 퍼센트 값과 "4z" 같은 존 값을 unsigned long long에 인코딩한다.
 * 일반 바이트 값과 구분하기 위해 특수 범위를 사용한다:
 *   - 퍼센트: -1ULL - 값 (상위 비트가 모두 1인 범위)
 *   - 존: ZONE_BASE_VAL + 존번호 (상위 1비트만 1인 범위)
 */
static inline int parse_is_percent(unsigned long long val)
{
	return val >= -101ULL;  /* 100% 이하의 퍼센트 값인지 확인 */
}

#define ZONE_BASE_VAL ((-1ULL >> 1) + 1)  /* 0x8000000000000000 — 존 값의 기준점 */
static inline int parse_is_percent_uncapped(unsigned long long val)
{
	return ZONE_BASE_VAL + -1U < val;  /* 제한 없는 퍼센트 값인지 확인 */
}

static inline int parse_is_zone(unsigned long long val)
{
	return (val - ZONE_BASE_VAL) <= -1U;  /* 존 인코딩된 값인지 확인 */
}

/*
 * [한국어] 파싱된 옵션을 덤프 목록에 저장하기 위한 구조체
 * 잡 파일 재출력(--output-format) 시 사용된다.
 */
struct print_option {
	struct flist_head list;  /* 연결 리스트 노드 */
	char *name;              /* 옵션 이름 */
	char *value;             /* 옵션 값 (NULL이면 값 없음) */
};

#endif
