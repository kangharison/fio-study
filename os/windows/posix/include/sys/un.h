/*
 * [한국어 설명] Windows용 sys/un.h 호환 헤더
 * POSIX Unix 도메인 소켓용 sockaddr_un 구조체와 관련 타입 정의.
 * sa_family_t, in_port_t 타입 및 sun_path 경로(MAX 260자) 포함.
 */
#ifndef SYS_UN_H
#define SYS_UN_H

typedef int sa_family_t;
typedef int in_port_t;

 struct sockaddr_un
 {
	sa_family_t	sun_family; /* Address family */
	char		sun_path[260]; /* Socket pathname */
};

#endif /* SYS_UN_H */
