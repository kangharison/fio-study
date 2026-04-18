/*
 * [한국어 설명] getopt_long_only 폴리필 헤더 (oslib/getopt.h)
 *
 * === 파일의 역할 ===
 * GNU 확장 getopt_long_only(3) 가 시스템에 없을 때를 위한 폴백 선언부와
 * struct option, 인자 모드 enum(no_argument/required_argument/optional_argument)
 * 을 제공한다. getopt_long_only 는 getopt_long 의 변종으로 "-" 접두 옵션도
 * 긴 이름으로 취급하므로(예: -output 가 --output 과 동일), fio 의 단문자/긴
 * 옵션 혼합 체계(`-t 60 --name=job1`)와 호환된다.
 * CONFIG_GETOPT_LONG_ONLY 매크로가 정의되면 시스템 <getopt.h> 그대로 사용,
 * 미정의 시 oslib/getopt_long.c 의 자체 구현이 링크된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 커맨드라인 파싱은 init.c::parse_cmd_line() → getopt_long_only() 루프로
 * 동작한다. 긴 옵션 테이블은 init.c 의 l_opts[] 배열(매우 큼 — 160+ 엔트리)로
 * 정의되며, 각 엔트리가 struct option 포맷으로 fio 옵션 이름과 내부 식별자를
 * 매핑한다. client-server 모드에서는 FIO_CLIENT_FLAG 비트(16번 비트) 로 옵션이
 * 서버 측에서만 처리되는지 클라이언트에도 영향 주는지를 표시한다.
 * 호출 체인: main() [fio.c] → parse_options() [init.c] → parse_cmd_line()
 *   → getopt_long_only(argc, argv, cmd_optstr, l_opts, &idx) → 옵션 처리 switch
 *
 * === 타 모듈과의 연결 ===
 * - oslib/getopt_long.c: 자체 구현부(자동 축약 매칭, 에러 보고 optopt/opterr).
 * - init.c: parse_cmd_line()/l_opts[] 의 주요 사용자.
 * - fio.c: main() 경로에서 간접 사용.
 * - configure: CONFIG_GETOPT_LONG_ONLY 정의 여부 결정.
 *
 * === 주요 구조체/함수 요약 ===
 * - struct option: 긴 옵션 하나의 메타데이터(이름, 인자 모드, 플래그, val).
 * - enum {no_argument, required_argument, optional_argument}: has_arg 값 상수.
 * - getopt_long_only(): 긴 옵션 기반 파싱 루프의 단일 step 함수.
 */
#ifdef CONFIG_GETOPT_LONG_ONLY
/* [한국어] 시스템이 getopt_long_only 를 직접 제공하는 경우 — <getopt.h> 재포함. */

#include <getopt.h>

#else
/* [한국어] 시스템 미지원 — fio 자체 구현의 선언부 제공. */

#ifndef _GETOPT_H
/* [한국어] 관용적 가드명(_GETOPT_H) — 원본 glibc getopt.h 와 동일해 호환성 유지. */
#define _GETOPT_H

/*
 * [한국어] struct option: 긴 옵션 정의
 * 호출자가 NULL 종료 배열(마지막 엔트리 {0,0,0,0})로 l_opts[] 를 구성해 전달한다.
 */
struct option {
	const char *name;
	/* [한국어] 긴 옵션 이름(예: "output", "directory", "ioengine").
	 * 설정자: 호출자가 init.c::l_opts[] 정적 테이블에서 지정.
	 * 읽는 자: getopt_long_only() 가 strncmp 로 argv 와 매칭.
	 * 값 범위: NUL 종료 ASCII 문자열. NULL 은 배열 종료 센티넬.
	 * 동기화: 읽기 전용 .rodata — 스레드 간 공유 안전. */

	int has_arg;
	/* [한국어] 인자 필요 여부: no_argument(0)/required_argument(1)/optional_argument(2).
	 * 설정자: l_opts[] 정의.
	 * 읽는 자: getopt_long_only() 가 "=값" 파싱 분기.
	 * 값 범위: 세 enum 값 중 하나. */

	int *flag;
	/* [한국어] 플래그 포인터:
	 *   NULL 이 아니면 옵션 발견 시 *flag = val 로 저장하고 getopt 는 0 반환.
	 *   NULL 이면 getopt 가 val 값을 직접 반환(호출자의 switch-case 분기).
	 * 설정자: l_opts[] 정의. fio 는 거의 모든 엔트리에서 NULL 사용(switch 분기).
	 * 읽는 자: getopt_long_only(). */

	int val;
	/* [한국어] 옵션 식별자 — flag==NULL 일 때 반환값, 아니면 *flag 에 저장할 값.
	 * 설정자: l_opts[] 정의. 통상 짧은 옵션 문자(예: 't', 'h') 또는 특별 열거값.
	 * 읽는 자: 호출자 switch. */
};

/* [한국어] has_arg 에 사용할 세 모드 — POSIX getopt_long 과 동일. */
enum {
	no_argument	  = 0,
	/* [한국어] 인자 없음. 예: --verbose, --help. */
	required_argument = 1,
	/* [한국어] 인자 필수. 예: --output=file 또는 --output file. */
	optional_argument = 2,
	/* [한국어] 인자 선택. 예: --debug[=level]. GNU 확장 기능. */
};

/*
 * [한국어]
 * getopt_long_only - 긴 옵션 기반 argv 파싱 1 step
 *
 * @argc:     main 의 argc.
 * @argv:     main 의 argv (내용은 getopt 가 자유롭게 재정렬/포인터 이동).
 * @shortopts: 단문자 옵션 문자열(예: "hvt:o:"). NULL 허용(긴 옵션만 허용).
 * @longopts:  struct option 배열(NULL 종료 센티넬 필수).
 * @longindex: 매칭된 긴 옵션 인덱스가 저장될 포인터(NULL 허용).
 * @return: 옵션 식별자 또는 -1(더 이상 없음), '?'(알 수 없는 옵션), ':'(인자 누락).
 *
 * 동작: optarg, optind, optopt 전역을 갱신하며 argv 를 순회. "-"/"--" 접두 모두
 *      긴 이름으로 시도 — fio 의 `-t 60 --name=job1` 혼합 문법 지원 핵심.
 *
 * 호출 체인: init.c::parse_cmd_line() 의 while 루프.
 * 실행 컨텍스트: 메인 스레드(잡 실행 전 파싱 단계). 에러: 알 수 없는 옵션 시 stderr 로 메시지 + '?' 반환.
 */
int getopt_long_only(int, char *const *, const char *, const struct option *, int *);

#endif
#endif
