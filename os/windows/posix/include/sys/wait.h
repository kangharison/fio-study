/*
 * [한국어 설명] Windows용 sys/wait.h 호환 헤더
 * POSIX 프로세스 대기 API(waitpid) 선언과 종료 상태 매크로(WIFEXITED 등) 정의.
 * Windows에서 fork/wait 미지원이므로 매크로는 항상 0 반환, waitpid는 ENOSYS.
 */
#ifndef SYS_WAIT_H
#define SYS_WAIT_H

#define WIFSIGNALED(a)	0
#define WIFEXITED(a)	0
#define WTERMSIG(a)		0
#define WEXITSTATUS(a)	0
#define WNOHANG			1

pid_t waitpid(pid_t, int *stat_loc, int options);

#endif /* SYS_WAIT_H */
