/*
 * Copyright (c) Artem Bityutskiy, 2007, 2008
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Imported from mtd-utils by dehrenberg */

/*
 * [한국어 설명] MTD 유틸리티 공통 매크로/헬퍼 헤더 (libmtd_common.h)
 *
 * === 파일의 역할 ===
 * mtd-utils 패키지에서 포팅한 공통 유틸리티 매크로/헬퍼의 집합 헤더이다. MIN/MAX,
 * ALIGN, off_t printf 포맷, 에러/경고 메시지 매크로(errmsg/sys_errmsg/warnmsg
 * /normsg/verbose — 모두 PROGRAM_NAME 접두), is_power_of_2, simple_strtoX
 * 변환 헬퍼 매크로, 그리고 libmtd_xalloc.h(에러 시 즉시 종료하는 x-접두 할당자들)
 * 를 한 번에 포함한다.
 *
 * PROGRAM_NAME 매크로가 본 헤더 포함 이전에 반드시 #define 되어 있어야 한다.
 * fio 빌드에서는 libmtd_int.h 가 PROGRAM_NAME="libmtd" 로 정의하고 본 헤더를
 * 간접 포함하므로, 호출자(oslib/libmtd.c, libmtd_legacy.c)는 libmtd_int.h 먼저,
 * 본 헤더는 뒤에 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 MTD I/O 스택:
 *   engines/mtd.c (fio 엔진 콜백)
 *     → oslib/libmtd.h (공개 API)
 *       → oslib/libmtd.c (구현) — 본 헤더의 매크로/헬퍼 소비자
 *         → ioctl / /sys/class/mtd 파싱
 * 에러 발생 시 sys_errmsg_die → exit() 로 프로세스 종료하는 정책을 유지하나,
 * fio 의 I/O 엔진 내부에서는 원칙적으로 프로세스 종료는 피하는 편이라 호출
 * 컨텍스트에 주의가 필요(현재 구현은 상당히 치명적 오류 경로에만 die 사용).
 *
 * === 타 모듈과의 연결 ===
 * - libmtd.c, libmtd_legacy.c: 본 헤더의 주 소비자(errmsg, xmalloc 등 사용).
 * - libmtd_xalloc.h: 본 헤더 끝에서 포함 — x-접두 할당자 공개.
 * - engines/mtd.c: libmtd.h 의 공개 API 사용자(본 헤더는 간접).
 *
 * === 주요 매크로/함수 요약 ===
 * - MIN/MAX/min/ALIGN/__ALIGN_MASK: Linux 커널 스타일 유틸리티.
 * - min_t/max_t: GCC typeof 를 활용한 타입 인식 비교 매크로.
 * - O_CLOEXEC: 구형 <fcntl.h> 폴백 정의.
 * - PRIxoff_t / PRIdoff_t: off_t printf 포맷 지정자(32/64비트 분기).
 * - bareverbose/verbose/normsg/normsg_cont: 일반/상세 메시지 출력 매크로.
 * - errmsg/errmsg_die: 에러 메시지(선택적 exit).
 * - sys_errmsg/sys_errmsg_die: errno + strerror 포함 에러 메시지.
 * - warnmsg: 경고 메시지.
 * - is_power_of_2(n): 2의 거듭제곱 판별.
 * - simple_strtoX 매크로: strtol/strtoll/strtoul/strtoull 의 완전 소비 검증 래퍼.
 * - common_print_version: PROGRAM_NAME VERSION 출력.
 * - #include "libmtd_xalloc.h": x-접두 안전 할당자 노출.
 */
#ifndef __MTD_UTILS_COMMON_H__
/* [한국어] 헤더 가드 — libmtd 의 여러 translation unit 에서 포함 가능. */
#define __MTD_UTILS_COMMON_H__

/* [한국어] 아래 #include 들은 mtd-utils 의 기본 의존성 묶음 — 매크로 구현에
 * printf/fprintf/malloc/exit/strtol/errno/strerror/ioctl major/minor 등이 필요. */
#include <stdbool.h>
/* [한국어] <stdbool.h>: bool/true/false 표준 타입. simple_strtoX 등에서 사용. */
#include <stdio.h>
/* [한국어] <stdio.h>: printf/fprintf/stderr — 메시지 출력 매크로의 뼈대. */
#include <stdlib.h>
/* [한국어] <stdlib.h>: malloc/calloc/realloc/exit/strtol/strtoll/strtoul/strtoull —
 * xalloc 래퍼와 simple_strtoX 변환 매크로의 기반 함수들. */
#include <ctype.h>
/* [한국어] <ctype.h>: isspace/isalpha 등 — libmtd.c 의 문자열 처리 경로에서 사용. */
#include <string.h>
/* [한국어] <string.h>: strerror/strdup/memcpy — sys_errmsg 의 errno → 메시지 변환. */
#include <fcntl.h>
/* [한국어] <fcntl.h>: O_CLOEXEC 등 open(2) 플래그 정의. 구형 시스템 폴백 정의도 포함. */
#include <errno.h>
/* [한국어] <errno.h>: errno 글로벌 및 EINVAL/ENOMEM/ENODEV/ENOTSUPP 상수. */
#include <features.h>
/* [한국어] <features.h>: glibc 의 __USE_FILE_OFFSET64/__USE_GNU 등 기능 테스트 매크로.
 * PRIxoff_t 분기 결정에 사용. */
#include <inttypes.h>
/* [한국어] <inttypes.h>: PRIx32/PRIx64/PRId32/PRId64 포맷 매크로 — PRIxoff_t 조합에 필요. */
#include <sys/sysmacros.h>
/* [한국어] <sys/sysmacros.h>: major()/minor() 매크로 — MTD 디바이스 번호 추출에 사용. */

#ifndef PROGRAM_NAME
/* [한국어] PROGRAM_NAME 미정의 시 컴파일 에러로 즉시 중단 — 메시지 매크로가 PROGRAM_NAME
 * 을 리터럴 연결하므로 정의 누락은 링크가 아닌 빌드 시점에 잡는 것이 안전. */
# error "You must define PROGRAM_NAME before including this header"
#endif

#ifdef __cplusplus
extern "C" {
/* [한국어] C++ 에서 포함 시 C 링커 네이밍 유지. */
#endif

#ifndef MIN	/* some C lib headers define this for us */
/* [한국어] MIN(a,b): 작은 값. 표준 <sys/param.h> 에 이미 있을 수 있어 조건부. */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
/* [한국어] MAX(a,b): 큰 값. */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define min(a, b) MIN(a, b) /* glue for linux kernel source */
/* [한국어] min(): Linux 커널 코드에서 이름이 소문자라 포팅 편의용 alias. */

/* [한국어] ALIGN(x, a): x 를 a 의 배수로 올림 정렬. a 는 2 의 거듭제곱이어야 함.
 * mask = a-1 을 빼고 NOT 하여 최하위 비트 클리어. NAND 소거 블록 경계 계산 등에 사용. */
#define ALIGN(x,a) __ALIGN_MASK(x,(__typeof__(x))(a)-1)
#define __ALIGN_MASK(x,mask) (((x)+(mask))&~(mask))

/* [한국어] min_t/max_t: GCC __typeof__ 로 두 값의 타입을 내부 임시 변수로 고정해
 * 부호/폭 혼합 비교를 안전하게 수행. t 는 문서화용(실제로는 typeof 사용). */
#define min_t(t,x,y) ({ \
	__typeof__((x)) _x = (x); \
	__typeof__((y)) _y = (y); \
	(_x < _y) ? _x : _y; \
})

#define max_t(t,x,y) ({ \
	__typeof__((x)) _x = (x); \
	__typeof__((y)) _y = (y); \
	(_x > _y) ? _x : _y; \
})

#ifndef O_CLOEXEC
/* [한국어] O_CLOEXEC: open(2) 시 fd 에 FD_CLOEXEC 설정(자식 프로세스 상속 차단).
 * 구형 시스템에서 정의되지 않은 경우 0 으로 대체(보호 없음). */
#define O_CLOEXEC 0
#endif

/* define a print format specifier for off_t */
#ifdef __USE_FILE_OFFSET64
/* [한국어] glibc 의 _FILE_OFFSET_BITS=64 빌드: off_t 가 64비트라 PRIx64/PRId64 사용. */
#define PRIxoff_t PRIx64
#define PRIdoff_t PRId64
#else
/* [한국어] 32비트 off_t: long 타입이라 "l"PRIx32 처럼 길이 한정자 l 을 붙여야 함. */
#define PRIxoff_t "l"PRIx32
#define PRIdoff_t "l"PRId32
#endif

/* Verbose messages */
/* [한국어] bareverbose: verbose 조건 참일 때만 printf. PROGRAM_NAME 접두 없음. */
#define bareverbose(verbose, fmt, ...) do {                        \
	if (verbose)                                               \
		printf(fmt, ##__VA_ARGS__);                        \
} while(0)
/* [한국어] verbose: "<PROGRAM_NAME>: <메시지>\n" 형식으로 stdout 출력. */
#define verbose(verbose, fmt, ...) \
	bareverbose(verbose, "%s: " fmt "\n", PROGRAM_NAME, ##__VA_ARGS__)

/* Normal messages */
/* [한국어] normsg_cont: 개행 없이 stdout 출력(연속 메시지용). */
#define normsg_cont(fmt, ...) do {                                 \
	printf("%s: " fmt, PROGRAM_NAME, ##__VA_ARGS__);           \
} while(0)
/* [한국어] normsg: 개행 포함 stdout 출력. */
#define normsg(fmt, ...) do {                                      \
	normsg_cont(fmt "\n", ##__VA_ARGS__);                      \
} while(0)

/* Error messages */
/* [한국어] errmsg: stderr 에 에러 메시지 출력 후 -1 을 식괄 값으로 반환하는 GNU 확장 표현식.
 * 호출 예: return errmsg("open failed: %s", path);  // 에러 메시지 + return -1 동시. */
#define errmsg(fmt, ...)  ({                                                \
	fprintf(stderr, "%s: error!: " fmt "\n", PROGRAM_NAME, ##__VA_ARGS__); \
	-1;                                                                 \
})
/* [한국어] errmsg_die: 에러 메시지 출력 후 exit(-1) — 복구 불가 상황용. */
#define errmsg_die(fmt, ...) do {                                           \
	exit(errmsg(fmt, ##__VA_ARGS__));                                   \
} while(0)

/* System error messages */
/* [한국어] sys_errmsg: errno 를 캡처해 strerror 와 함께 출력. 두 줄 메시지.
 * errmsg 실행 후 _err 를 추가 출력하므로 errno 가 중간에 바뀌지 않도록 맨 처음에 저장. */
#define sys_errmsg(fmt, ...)  ({                                            \
	int _err = errno;                                                   \
	errmsg(fmt, ##__VA_ARGS__);                                         \
	fprintf(stderr, "%*serror %d (%s)\n", (int)sizeof(PROGRAM_NAME) + 1,\
		"", _err, strerror(_err));                                  \
	-1;                                                                 \
})
/* [한국어] sys_errmsg_die: sys_errmsg 출력 후 exit — libmtd 의 치명적 할당 실패 등에서 사용. */
#define sys_errmsg_die(fmt, ...) do {                                       \
	exit(sys_errmsg(fmt, ##__VA_ARGS__));                               \
} while(0)

/* Warnings */
/* [한국어] warnmsg: 경고 메시지 stderr 출력 — 계속 실행 가능한 상황. */
#define warnmsg(fmt, ...) do {                                                \
	fprintf(stderr, "%s: warning!: " fmt "\n", PROGRAM_NAME, ##__VA_ARGS__); \
} while(0)

/*
 * [한국어]
 * is_power_of_2 - n 이 2 의 거듭제곱인지 판별
 * @n: 검사할 unsigned long long 정수.
 * @return: 0 이 아니고 단 하나의 비트만 설정되어 있으면 1, 그 외 0.
 * 동작: n != 0 && (n & (n-1)) == 0 — 전형적인 비트 트릭.
 * 용도: MTD 소거 블록 크기가 2의 거듭제곱인지 검증, 정렬 계산 전제 확인 등.
 */
static inline int is_power_of_2(unsigned long long n)
{
	/* [한국어] n==0 은 2^(-infty) 로 취급하지 않으므로 별도 배제. */
	return (n != 0 && ((n & (n - 1)) == 0));
}

/**
 * simple_strtoX - convert a hex/dec/oct string into a number
 * @snum: buffer to convert
 * @error: set to 1 when buffer isn't fully consumed
 *
 * These functions are similar to the standard strtoX() functions, but they are
 * a little bit easier to use if you want to convert full string of digits into
 * the binary form. The typical usage:
 *
 * int error = 0;
 * unsigned long num;
 *
 * num = simple_strtoul(str, &error);
 * if (error || ... if needed, your check that num is not out of range ...)
 * 	error_happened();
 */
/* [한국어] simple_strtoX 매크로: strtol 류를 호출해 전체 문자열이 완전히 숫자로
 * 소비되었는지 검증(!*snum 즉 빈 입력 거부 + !*endptr 즉 남은 문자 없어야 통과).
 * 실패 시 errmsg 를 통해 stderr 에 출력하고 *error=1. 기수 인자 base=0 이라
 * 0x/0 접두 자동 감지(16/10/8 진). */
#define simple_strtoX(func, type) \
static inline type simple_##func(const char *snum, int *error) \
{ \
	char *endptr; \
	type ret = func(snum, &endptr, 0); \
 \
	if (error && (!*snum || *endptr)) { \
		errmsg("%s: unable to parse the number '%s'", #func, snum); \
		*error = 1; \
	} \
 \
	return ret; \
}
/* [한국어] 네 가지 변종 — 부호/폭 조합: long / long long / unsigned long / unsigned long long. */
simple_strtoX(strtol, long int)
simple_strtoX(strtoll, long long int)
simple_strtoX(strtoul, unsigned long int)
simple_strtoX(strtoull, unsigned long long int)

/* Simple version-printing for utils */
/* [한국어] common_print_version: mtd-utils 관례적 버전 출력. fio 내에서는 거의 사용되지 않음. */
#define common_print_version() \
do { \
	printf("%s %s\n", PROGRAM_NAME, VERSION); \
} while (0)

#include "libmtd_xalloc.h"
/* [한국어] libmtd_xalloc.h 포함 이유: sys_errmsg_die 가 이미 정의된 이후에 xalloc
 * 인라인 함수들이 본 매크로를 사용 가능해야 하므로 매크로 정의 이후에 포함하는 순서가 필수. */

#ifdef __cplusplus
}
/* [한국어] extern "C" 블록 종료. */
#endif

#endif /* !__MTD_UTILS_COMMON_H__ */
