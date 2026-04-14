/*
 * [한국어 설명] fio 옵션 시스템 헤더 (options.h)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio 옵션 관련 API와 전역 변수를 선언한다.
 * fio_options[] 배열(최대 512개), 옵션 관리 API(추가/삭제/검색/파싱),
 * fio_option_is_set() 확인 함수, o_match() 매칭 헬퍼를 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * options.c와 짝을 이루는 헤더로, init.c와 parse.c에서 옵션 처리 시 참조된다.
 * options.h → options.c(구현) / init.c(파싱 호출) / parse.c(파싱 엔진)
 *
 * === 타 모듈과의 연결 ===
 * - options.c: fio_options[] 배열과 API 함수의 구현
 * - init.c: parse_options()에서 fio_options[] 참조
 * - parse.c: 파싱 엔진이 fio_option 구조체를 처리
 * - ioengines.c: I/O 엔진이 add_option()으로 동적 옵션 추가
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_options[FIO_MAX_OPTS]: 모든 fio 옵션 정의 배열
 * - add_option(): I/O 엔진의 동적 옵션 추가
 * - fio_option_is_set(): 옵션이 사용자에 의해 명시적으로 설정되었는지 확인
 * - o_match(): 옵션 이름/별칭 매칭 헬퍼
 */
#ifndef FIO_OPTION_H
#define FIO_OPTION_H

#define FIO_MAX_OPTS		512  /* fio_options[] 배열의 최대 크기 */

#include <string.h>
#include <inttypes.h>
#include "parse.h"               /* fio_option 구조체, 파싱 API */
#include "lib/types.h"           /* bool 등 타입 정의 */

/* [한국어] 옵션 관리 API */
int add_option(const struct fio_option *);
		/* I/O 엔진이 자체 옵션을 fio_options[]에 동적으로 추가 */
void invalidate_profile_options(const char *);
		/* 지정된 프로파일의 옵션을 무효화 */
extern char *exec_profile;
		/* 현재 실행 중인 프로파일 이름 */

/* [한국어] posval 동적 조작 — I/O 엔진이 런타임에 옵션 값 추가/삭제 */
void add_opt_posval(const char *, const char *, const char *);
void del_opt_posval(const char *, const char *);

struct thread_data;
void fio_options_free(struct thread_data *);
		/* 스레드의 옵션 문자열 메모리 해제 */
void fio_dump_options_free(struct thread_data *);
		/* 옵션 덤프 리스트 메모리 해제 */

/* [한국어] 문자열 파싱 유틸리티 — 콤마/콜론 구분 파일명 목록 처리 */
char *get_next_str(char **ptr);
int get_max_str_idx(char *input);
char* get_name_by_idx(char *input, int index);
int set_name_idx(char *, size_t, char *, int, bool);

extern char client_sockaddr_str[];  /* --client 옵션에 사용되는 소켓 주소 문자열 */

/* [한국어] 전역 옵션 배열 — options.c에서 모든 fio 옵션이 정의됨 */
extern struct fio_option fio_options[FIO_MAX_OPTS];

/*
 * [한국어] 옵션 설정 여부 추적 시스템
 *
 * 사용자가 명시적으로 설정한 옵션과 기본값으로 설정된 옵션을 구분하기 위해,
 * thread_options 내 각 변수의 오프셋을 비트맵으로 추적한다.
 * fio_option_is_set(td, bs)는 사용자가 bs= 옵션을 직접 지정했는지 확인한다.
 */
extern bool __fio_option_is_set(struct thread_options *, unsigned int off);

#define fio_option_is_set(__td, name)					\
({									\
	const unsigned int off = offsetof(struct thread_options, name);	\
	bool __r = __fio_option_is_set((__td), off);			\
	__r;								\
})

extern void fio_option_mark_set(struct thread_options *,
				const struct fio_option *);

/*
 * [한국어] 옵션 이름 매칭 — name 또는 alias와 비교
 * parse.c의 find_option_c()에서 사용된다.
 */
static inline bool o_match(const struct fio_option *o, const char *opt)
{
	if (!strcmp(o->name, opt))
		return true;
	else if (o->alias && !strcmp(o->alias, opt))
		return true;

	return false;
}

extern struct fio_option *find_option(struct fio_option *, const char *);
extern const struct fio_option *
find_option_c(const struct fio_option *, const char *);
extern struct fio_option *fio_option_find(const char *);
		/* fio_options[] 배열에서 이름으로 옵션을 찾는다 */
extern unsigned int fio_get_kb_base(void *);
		/* kb_base 값 조회 (1024 또는 1000) */

#endif
