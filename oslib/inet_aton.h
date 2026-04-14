/*
 * [한국어 설명] IP 주소 문자열 파싱 함수 폴리필 헤더 (inet_aton.h)
 *
 * === 파일의 역할 ===
 * inet_aton() 함수가 시스템에 없는 경우를 위한 선언부이다.
 * inet_aton()은 "192.168.1.1" 형태의 IPv4 주소 문자열을 이진 네트워크 주소로 변환한다.
 * POSIX에서 폐기(deprecated)되었고 inet_pton()으로 대체되었지만, 일부 레거시 코드에서
 * 아직 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio의 네트워크 I/O 엔진(engines/net.c 등)에서
 * IP 주소 파싱에 사용될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/inet_aton.c: 실제 구현부 (inet_pton() 래퍼)
 * - engines/net.c: 네트워크 I/O 엔진에서 IP 주소 파싱 시 사용
 *
 * === 주요 함수 요약 ===
 * - inet_aton(): IPv4 문자열을 이진 네트워크 주소로 변환
 */
#ifndef FIO_INET_ATON_LIB_H
#define FIO_INET_ATON_LIB_H

#include <arpa/inet.h>

int inet_aton(const char *cp, struct in_addr *inp);

#endif
