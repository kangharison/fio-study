/*
 * [한국어] log.c - fio 로깅 시스템 구현
 *
 * fio의 모든 로그 출력을 담당하는 파일이다.
 * 실행 모드에 따라 출력 대상이 자동으로 전환된다:
 *   - 서버 모드(is_backend): 네트워크를 통해 클라이언트로 전송
 *   - syslog 모드(log_syslog): 시스템 로그로 전송
 *   - 일반 모드: f_out(stdout) 또는 f_err(stderr)로 출력
 *
 * 주요 API:
 *   log_info()     - 정보 메시지 출력 (printf 형식)
 *   log_err()      - 에러 메시지 출력 (stderr + f_err)
 *   log_info_buf() - 원시 버퍼 출력
 *   __log_buf()    - 버퍼에 포맷 문자열 추가 (지연 출력)
 */
#include "log.h"

#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>

#include "fio.h"
#include "oslib/asprintf.h"

/* [한국어] 원시 버퍼를 로그로 출력. 모드에 따라 서버/syslog/파일로 분기 */
size_t log_info_buf(const char *buf, size_t len)
{
	/*
	 * buf could be NULL (not just "").
	 */
	/* [한국어] buf가 NULL일 수 있으므로 방어적 검사 */
	if (!buf)
		return 0;

	/* [한국어] 서버 모드: 네트워크를 통해 클라이언트에게 전송 */
	if (is_backend) {
		ssize_t ret = fio_server_text_output(FIO_LOG_INFO, buf, len);
		if (ret != -1)
			return ret;
	}

	/* [한국어] syslog 모드: 시스템 로그로 전송 */
	if (log_syslog) {
		syslog(LOG_INFO, "%s", buf);
		return len;
	} else
		return fwrite(buf, len, 1, f_out);	/* [한국어] 일반 모드: f_out에 출력 */
}

/* [한국어] va_list를 받아서 포맷팅 후 log_info_buf로 출력 */
size_t log_valist(const char *fmt, va_list args)
{
	char *buffer;
	int len;

	len = vasprintf(&buffer, fmt, args);
	if (len < 0)
		return 0;
	len = log_info_buf(buffer, len);
	free(buffer);

	return len;
}

/* add prefix for the specified type in front of the valist */
/* [한국어] 디버그 타입별 접두사(타입명 + PID)를 붙여 출력 (디버그 빌드 전용) */
#ifdef FIO_INC_DEBUG
void log_prevalist(int type, const char *fmt, va_list args)
{
	char *buf1, *buf2;
	int len;
	pid_t pid;

	pid = gettid();
	/* [한국어] 특정 job의 디버그만 출력하도록 필터링 */
	if (fio_debug_jobp && *fio_debug_jobp != -1U
	    && pid != *fio_debug_jobp)
		return;

	len = vasprintf(&buf1, fmt, args);
	if (len < 0)
		return;
	/* [한국어] "타입명    PID   원본메시지" 형식으로 접두사 추가 */
	len = asprintf(&buf2, "%-8s %-5u %s", debug_levels[type].name,
		       (int) pid, buf1);
	free(buf1);
	if (len < 0)
		return;
	log_info_buf(buf2, len);
	free(buf2);
}
#endif

/* [한국어] printf 형식의 정보 메시지 출력 함수 */
ssize_t log_info(const char *format, ...)
{
	va_list args;
	ssize_t ret;

	va_start(args, format);
	ret = log_valist(format, args);
	va_end(args);

	return ret;
}

/* [한국어] 버퍼(buf_output)에 포맷 문자열을 추가하는 함수 (지연 출력용) */
size_t __log_buf(struct buf_output *buf, const char *format, ...)
{
	char *buffer;
	va_list args;
	int len;

	va_start(args, format);
	len = vasprintf(&buffer, format, args);
	va_end(args);
	if (len < 0)
		return 0;
	len = buf_output_add(buf, buffer, len);
	free(buffer);

	return len;
}

/* [한국어] 출력 버퍼 플러시. 서버/syslog 모드에서는 불필요 (항상 즉시 전송) */
int log_info_flush(void)
{
	if (is_backend || log_syslog)
		return 0;

	return fflush(f_out);
}

/* [한국어] 에러 메시지 출력. stderr와 f_err 양쪽에 출력하며, 서버 모드 시 클라이언트로도 전송 */
ssize_t log_err(const char *format, ...)
{
	ssize_t ret;
	int len;
	char *buffer;
	va_list args;

	va_start(args, format);
	len = vasprintf(&buffer, format, args);
	va_end(args);
	if (len < 0)
		return len;

	/* [한국어] 서버 모드: 클라이언트에게 에러 메시지 전송 */
	if (is_backend) {
		ret = fio_server_text_output(FIO_LOG_ERR, buffer, len);
		if (ret != -1)
			goto done;
	}

	/* [한국어] syslog 모드: 시스템 로그로 전송 */
	if (log_syslog) {
		syslog(LOG_INFO, "%s", buffer);
		ret = len;
	} else {
		/* [한국어] f_err가 stderr와 다르면 양쪽 모두에 출력 */
		if (f_err != stderr)
			ret = fwrite(buffer, len, 1, stderr);

		ret = fwrite(buffer, len, 1, f_err);
	}

done:
	free(buffer);
	return ret;
}

/* [한국어] 로그 레벨 번호를 사람이 읽을 수 있는 문자열로 변환 */
const char *log_get_level(int level)
{
	static const char *levels[] = { "Unknown", "Debug", "Info", "Error",
						"Unknown" };

	if (level >= FIO_LOG_NR)
		level = FIO_LOG_NR;

	return levels[level];
}
