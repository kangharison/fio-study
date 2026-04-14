/*
 * [한국어 설명] Windows용 poll.h 호환 헤더
 * POSIX poll() 함수와 nfds_t 타입을 선언.
 * 실제 구현은 windows/posix.c에서 select() 기반으로 에뮬레이션.
 */
#ifndef POLL_H
#define POLL_H

#include <winsock2.h>

typedef int nfds_t;

int poll(struct pollfd fds[], nfds_t nfds, int timeout);

#endif /* POLL_H */
