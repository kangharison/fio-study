/*
 * [한국어] log.h - fio 로깅 시스템 헤더 파일
 *
 * fio의 로그 출력 API를 선언한다.
 * 서버 모드(is_backend), syslog 모드, 일반 모드에 따라
 * 출력 대상이 자동으로 전환된다.
 
 * === 파일의 역할 ===
 * fio의 로그 출력 API를 선언한다. 서버/syslog/일반 모드별 자동 전환.
 *
 * === 전체 아키텍처에서의 위치 ===
 * log.c와 짝을 이루는 헤더. fio.h를 통해 전역 접근.
 *
 * === 타 모듈과의 연결 ===
 * - log.c: 이 헤더의 함수 구현
 * - fio.h: 이 헤더를 포함하여 전역 접근
 *
 * === 주요 함수/구조체 요약 ===
 * - log_info()/log_err(): 정보/에러 메시지 출력
 * - log_info_buf(): 원시 버퍼 출력
 */
#ifndef FIO_LOG_H
#define FIO_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

#include "lib/output_buffer.h"

/* [한국어] 표준 출력/에러 파일 포인터 (일반 모드에서 사용) */
extern FILE *f_out;
extern FILE *f_err;

/* [한국어] 에러 메시지 출력 (stderr + f_err, 서버 모드 시 클라이언트로 전송) */
extern ssize_t log_err(const char *format, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
/* [한국어] 정보 메시지 출력 (f_out, 서버 모드 시 클라이언트로 전송) */
extern ssize_t log_info(const char *format, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
/* [한국어] 버퍼에 포맷 문자열 추가 (지연 출력용) */
extern size_t __log_buf(struct buf_output *, const char *format, ...) __attribute__ ((__format__ (__printf__, 2, 3)));
/* [한국어] va_list를 받아서 정보 메시지 출력 */
extern size_t log_valist(const char *str, va_list);
/* [한국어] 디버그 타입별 접두사를 붙여 출력 (디버그 빌드 전용) */
extern void log_prevalist(int type, const char *str, va_list);
/* [한국어] 원시 버퍼를 로그로 출력 */
extern size_t log_info_buf(const char *buf, size_t len);
/* [한국어] 출력 버퍼 플러시 */
extern int log_info_flush(void);

/* [한국어] 버퍼가 있으면 버퍼에, 없으면 log_info로 직접 출력하는 매크로 */
#define log_buf(buf, format, args...)			\
({							\
	size_t __ret;					\
	if ((buf) != NULL)				\
		__ret = __log_buf(buf, format, ##args);	\
	else						\
		__ret = log_info(format, ##args);	\
	__ret;						\
})

/* [한국어] 로그 레벨 열거형 */
enum {
	FIO_LOG_DEBUG	= 1,	/* [한국어] 디버그 레벨 */
	FIO_LOG_INFO	= 2,	/* [한국어] 정보 레벨 */
	FIO_LOG_ERR	= 3,	/* [한국어] 에러 레벨 */
	FIO_LOG_NR	= 4,	/* [한국어] 레벨 총 개수 (경계값) */
};

/* [한국어] 로그 레벨 번호를 문자열로 변환 */
extern const char *log_get_level(int level);

#endif
