/*
 * [한국어 설명] Windows용 syslog.h 호환 헤더
 * POSIX syslog API(openlog/syslog/closelog) 선언과 로그 우선순위/옵션 상수 정의.
 * 실제 구현은 windows/posix.c에서 파일("syslog.txt")에 기록.
 */
#ifndef SYSLOG_H
#define SYSLOG_H

int syslog(int priority, const char *format, ...);

#define LOG_INFO	0x1
#define LOG_ERROR	0x2
#define LOG_WARN	0x4

#define LOG_NDELAY	0x1
#define LOG_NOWAIT	0x2
#define LOG_PID		0x4
#define LOG_USER	0x8

void closelog(void);
void openlog(const char *ident, int logopt, int facility);

#endif /* SYSLOG_H */
