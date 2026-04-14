/*
 * [한국어 설명] 긴 옵션(long option) 파싱 함수 폴리필 헤더 (getopt.h)
 *
 * === 파일의 역할 ===
 * getopt_long_only() 함수가 시스템에 없는 경우를 위한 구조체 및 함수 선언부이다.
 * 커맨드라인에서 --옵션이름 형태의 긴 옵션을 파싱하는 기능을 제공한다.
 * CONFIG_GETOPT_LONG_ONLY가 정의되면 시스템의 <getopt.h>를 사용하고,
 * 그렇지 않으면 자체 구현을 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 커맨드라인 파싱에 핵심적으로 사용된다.
 * main() → parse_options() [init.c]에서 getopt_long_only()를 호출하여
 * 사용자가 전달한 --name=value 형태의 옵션을 처리한다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/getopt_long.c: 실제 구현부
 * - init.c: parse_options()에서 커맨드라인 옵션 파싱에 사용
 * - fio.c: main()에서 간접 사용
 *
 * === 주요 구조체/함수 요약 ===
 * - struct option: 긴 옵션 정의 (이름, 인자 유무, 플래그, 값)
 * - enum (no_argument 등): 옵션 인자 모드 상수
 * - getopt_long_only(): 긴 옵션만으로 파싱하는 함수
 */
#ifdef CONFIG_GETOPT_LONG_ONLY

#include <getopt.h>

#else

#ifndef _GETOPT_H
#define _GETOPT_H

/* [한국어] 긴 옵션 정의 구조체 - getopt_long_only()에 전달되는 옵션 배열의 원소 */
struct option {
	const char *name;   /* [한국어] 옵션 이름 (예: "output") */
	int has_arg;        /* [한국어] 인자 필요 여부: no_argument/required_argument/optional_argument */
	int *flag;          /* [한국어] NULL이 아니면 *flag에 val을 저장하고 0 반환, NULL이면 val 반환 */
	int val;            /* [한국어] 이 옵션이 선택되었을 때 반환할 값 (보통 짧은 옵션 문자) */
};

/* [한국어] 옵션 인자 모드 상수 */
enum {
	no_argument	  = 0,   /* [한국어] 인자 없음 (예: --verbose) */
	required_argument = 1,   /* [한국어] 인자 필수 (예: --output=file) */
	optional_argument = 2,   /* [한국어] 인자 선택 (예: --debug[=level]) */
};

int getopt_long_only(int, char *const *, const char *, const struct option *, int *);

#endif
#endif
