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
 * mtd-utils 패키지에서 가져온 공통 유틸리티 매크로와 헬퍼 함수를 제공한다.
 * MIN/MAX 매크로, 정렬(ALIGN) 매크로, 에러/경고 메시지 출력 매크로,
 * 문자열→정수 변환 헬퍼, 2의 거듭제곱 판별 함수 등이 포함된다.
 * PROGRAM_NAME 매크로가 사전에 정의되어야 한다 (이 파일에서는 "libmtd").
 *
 * === 전체 아키텍처에서의 위치 ===
 * libmtd.c와 libmtd_legacy.c에서 에러 처리, 메모리 할당, 문자열 변환 등에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - libmtd.c, libmtd_legacy.c: 이 매크로들의 주요 사용자
 * - libmtd_xalloc.h: 메모리 할당 래퍼 (#include로 포함)
 *
 * === 주요 매크로/함수 요약 ===
 * - MIN/MAX/ALIGN: 기본 유틸리티 매크로
 * - errmsg/sys_errmsg: 에러 메시지 출력 매크로
 * - is_power_of_2(): 2의 거듭제곱 판별
 * - simple_strtoX(): 문자열→정수 변환 래퍼
 */
#ifndef __MTD_UTILS_COMMON_H__
#define __MTD_UTILS_COMMON_H__

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <features.h>
#include <inttypes.h>
#include <sys/sysmacros.h>

#ifndef PROGRAM_NAME
# error "You must define PROGRAM_NAME before including this header"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* [한국어] 기본 유틸리티 매크로 - Linux 커널 스타일 */
#ifndef MIN	/* some C lib headers define this for us */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define min(a, b) MIN(a, b) /* glue for linux kernel source */

/* [한국어] 정렬 매크로: x를 a의 배수로 올림 정렬 (a는 2의 거듭제곱이어야 함) */
#define ALIGN(x,a) __ALIGN_MASK(x,(__typeof__(x))(a)-1)
#define __ALIGN_MASK(x,mask) (((x)+(mask))&~(mask))

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
#define O_CLOEXEC 0
#endif

/* [한국어] off_t 타입의 printf 포맷 지정자 - 64비트 파일 오프셋 지원 여부에 따라 다름 */
/* define a print format specifier for off_t */
#ifdef __USE_FILE_OFFSET64
#define PRIxoff_t PRIx64
#define PRIdoff_t PRId64
#else
#define PRIxoff_t "l"PRIx32
#define PRIdoff_t "l"PRId32
#endif

/* [한국어] 메시지 출력 매크로 계열 - PROGRAM_NAME을 자동으로 앞에 붙임 */
/* Verbose messages */
#define bareverbose(verbose, fmt, ...) do {                        \
	if (verbose)                                               \
		printf(fmt, ##__VA_ARGS__);                        \
} while(0)
#define verbose(verbose, fmt, ...) \
	bareverbose(verbose, "%s: " fmt "\n", PROGRAM_NAME, ##__VA_ARGS__)

/* Normal messages */
#define normsg_cont(fmt, ...) do {                                 \
	printf("%s: " fmt, PROGRAM_NAME, ##__VA_ARGS__);           \
} while(0)
#define normsg(fmt, ...) do {                                      \
	normsg_cont(fmt "\n", ##__VA_ARGS__);                      \
} while(0)

/* [한국어] 에러 메시지 매크로 - stderr에 출력하고 -1 반환 (표현식으로 사용 가능) */
/* Error messages */
#define errmsg(fmt, ...)  ({                                                \
	fprintf(stderr, "%s: error!: " fmt "\n", PROGRAM_NAME, ##__VA_ARGS__); \
	-1;                                                                 \
})
#define errmsg_die(fmt, ...) do {                                           \
	exit(errmsg(fmt, ##__VA_ARGS__));                                   \
} while(0)

/* [한국어] 시스템 에러 매크로 - errno 값과 strerror() 메시지를 함께 출력 */
/* System error messages */
#define sys_errmsg(fmt, ...)  ({                                            \
	int _err = errno;                                                   \
	errmsg(fmt, ##__VA_ARGS__);                                         \
	fprintf(stderr, "%*serror %d (%s)\n", (int)sizeof(PROGRAM_NAME) + 1,\
		"", _err, strerror(_err));                                  \
	-1;                                                                 \
})
#define sys_errmsg_die(fmt, ...) do {                                       \
	exit(sys_errmsg(fmt, ##__VA_ARGS__));                               \
} while(0)

/* Warnings */
#define warnmsg(fmt, ...) do {                                                \
	fprintf(stderr, "%s: warning!: " fmt "\n", PROGRAM_NAME, ##__VA_ARGS__); \
} while(0)

/* [한국어] 2의 거듭제곱 판별 - n & (n-1)이 0이면 비트가 하나만 설정된 것 */
static inline int is_power_of_2(unsigned long long n)
{
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
simple_strtoX(strtol, long int)
simple_strtoX(strtoll, long long int)
simple_strtoX(strtoul, unsigned long int)
simple_strtoX(strtoull, unsigned long long int)

/* Simple version-printing for utils */
#define common_print_version() \
do { \
	printf("%s %s\n", PROGRAM_NAME, VERSION); \
} while (0)

#include "libmtd_xalloc.h"

#ifdef __cplusplus
}
#endif

#endif /* !__MTD_UTILS_COMMON_H__ */
