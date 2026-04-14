/*
 * [한국어 설명] MTD 라이브러리 메모리 할당 래퍼 헤더 (libmtd_xalloc.h)
 *
 * === 파일의 역할 ===
 * 메모리 할당 실패 시 프로그램을 종료하는 안전한 메모리 할당 래퍼 함수를 제공한다.
 * xmalloc, xcalloc, xzalloc, xrealloc, xstrdup, xasprintf 등의 함수가 있으며,
 * 모두 할당 실패 시 sys_errmsg_die()를 호출하여 에러 메시지를 출력하고 종료한다.
 * "x" 접두사는 "exit on failure" 관례를 따른다 (BusyBox, Git 등에서도 사용).
 *
 * === 전체 아키텍처에서의 위치 ===
 * libmtd_common.h에 의해 포함되며, libmtd.c와 libmtd_legacy.c의 모든 메모리 할당에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - libmtd_common.h: 이 파일을 #include하여 libmtd 전체에 노출
 * - libmtd.c, libmtd_legacy.c: xmalloc(), xzalloc() 등 사용
 *
 * === 주요 함수 요약 ===
 * - xmalloc(): malloc() + 실패 시 프로그램 종료
 * - xcalloc(): calloc() + 실패 시 종료
 * - xzalloc(): 0으로 초기화된 메모리 할당
 * - xrealloc(): realloc() + 실패 시 종료
 * - xstrdup(): strdup() + 실패 시 종료
 * - xasprintf(): vasprintf() + 실패 시 종료
 */
/*
 * memory wrappers
 *
 * Copyright (c) Artem Bityutskiy, 2007, 2008
 * Copyright 2001, 2002 Red Hat, Inc.
 *           2001 David A. Schleef <ds@lineo.com>
 *           2002 Axis Communications AB
 *           2001, 2002 Erik Andersen <andersen@codepoet.org>
 *           2004 University of Szeged, Hungary
 *           2006 KaiGai Kohei <kaigai@ak.jp.nec.com>
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

#ifndef __MTD_UTILS_XALLOC_H__
#define __MTD_UTILS_XALLOC_H__

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * [한국어] __attribute__((unused))를 붙여 헤더에 정의된 static 함수가
 * 사용되지 않더라도 gcc 경고가 발생하지 않도록 한다.
 */
/*
 * Mark these functions as unused so that gcc does not emit warnings
 * when people include this header but don't use every function.
 */

/* [한국어] xmalloc: malloc + 실패 시 "out of memory" 에러와 함께 프로그램 종료 */
__attribute__((unused))
static void *xmalloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr == NULL && size != 0)
		sys_errmsg_die("out of memory");
	return ptr;
}

/* [한국어] xcalloc: calloc (0으로 초기화된 배열 할당) + 실패 시 종료 */
__attribute__((unused))
static void *xcalloc(size_t nmemb, size_t size)
{
	void *ptr = calloc(nmemb, size);

	if (ptr == NULL && nmemb != 0 && size != 0)
		sys_errmsg_die("out of memory");
	return ptr;
}

/* [한국어] xzalloc: 0으로 초기화된 단일 객체 할당 (xcalloc(1, size)의 편의 래퍼) */
__attribute__((unused))
static void *xzalloc(size_t size)
{
	return xcalloc(1, size);
}

/* [한국어] xrealloc: realloc (기존 버퍼 크기 변경) + 실패 시 종료 */
__attribute__((unused))
static void *xrealloc(void *ptr, size_t size)
{
	ptr = realloc(ptr, size);
	if (ptr == NULL && size != 0)
		sys_errmsg_die("out of memory");
	return ptr;
}

/* [한국어] xstrdup: strdup (문자열 복제) + 실패 시 종료, NULL 입력은 NULL 반환 */
__attribute__((unused))
static char *xstrdup(const char *s)
{
	char *t;

	if (s == NULL)
		return NULL;
	t = strdup(s);
	if (t == NULL)
		sys_errmsg_die("out of memory");
	return t;
}

/* [한국어] GNU 확장이 사용 가능한 경우에만 xasprintf 정의 */
#ifdef _GNU_SOURCE

/* [한국어] xasprintf: vasprintf (포맷 문자열 동적 할당) + 실패 시 종료 */
__attribute__((unused))
static int xasprintf(char **strp, const char *fmt, ...)
{
	int cnt;
	va_list ap;

	va_start(ap, fmt);
	cnt = vasprintf(strp, fmt, ap);
	va_end(ap);

	if (cnt == -1)
		sys_errmsg_die("out of memory");

	return cnt;
}
#endif

#endif /* !__MTD_UTILS_XALLOC_H__ */
