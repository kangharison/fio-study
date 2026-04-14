/*
 * [한국어 설명] Windows용 arpa/inet.h 호환 헤더
 * Winsock2(ws2tcpip.h)를 래핑하여 POSIX 네트워크 타입/상수를 제공.
 * socklen_t, in_addr_t 타입 정의 및 EAI_SYSTEM → EAI_FAIL 매핑.
 */
#ifndef ARPA_INET_H
#define ARPA_INET_H

#include <ws2tcpip.h>
#include <inttypes.h>

typedef int socklen_t;
typedef int in_addr_t;

/* EAI_SYSTEM isn't used on Windows, so map it to EAI_FAIL */
#define EAI_SYSTEM EAI_FAIL

in_addr_t inet_network(const char *cp);

#endif /* ARPA_INET_H */
