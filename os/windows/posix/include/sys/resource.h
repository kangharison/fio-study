/*
 * [한국어 설명] Windows용 sys/resource.h 호환 헤더
 * POSIX getrusage() 선언과 rusage 구조체(사용자/커널 시간, 컨텍스트 스위치 등) 정의.
 * RUSAGE_SELF(프로세스), RUSAGE_THREAD(스레드) 지원.
 * 실제 구현은 windows/posix.c에서 GetProcessTimes/GetThreadTimes 기반.
 */
#ifndef SYS_RESOURCE_H
#define SYS_RESOURCE_H

#define RUSAGE_SELF	0
#define RUSAGE_THREAD	1

struct rusage
{
	struct timeval ru_utime;
	struct timeval ru_stime;
	int ru_nvcsw;
	int ru_minflt;
	int ru_majflt;
	int ru_nivcsw;
};

int getrusage(int who, struct rusage *r_usage);

#endif /* SYS_RESOURCE_H */
