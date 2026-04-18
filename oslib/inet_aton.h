/*
 * [한국어 설명] inet_aton 폴리필 헤더 (inet_aton.h)
 *
 * === 파일의 역할 ===
 * BSD 유래 함수 inet_aton(3) 가 시스템에 없을 때를 위한 폴백 선언부이다.
 * inet_aton 은 "192.168.1.1" 형태의 IPv4 점 4쌍 문자열을 네트워크 바이트
 * 순서의 32비트 정수(struct in_addr::s_addr)로 변환한다. POSIX 2001 에서
 * 표준화 목록이 제거되고 inet_pton(AF_INET,...) 로 대체되었지만, fio 의 레거시
 * 코드(engines/rdma.c, engines/net.c, server.c 일부 경로) 에서 여전히 사용된다.
 * 시스템 제공 시 <arpa/inet.h> 의 표준 선언이 그대로 쓰이고, 미지원 시 oslib/
 * inet_aton.c 의 inet_pton 래퍼 구현이 링크된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어. 주 호출 경로는 fio 의 네트워크 엔진/서버 프로토콜에서
 * 사용자가 넘긴 IPv4 주소 문자열을 sockaddr_in 에 채우는 변환 단계이다.
 * 호출 체인(예시):
 *   server.c::fio_server_parse_host() → inet_aton() → sockaddr_in::sin_addr 채움
 *   engines/rdma.c::aton() 래퍼 → inet_aton() 또는 gethostbyname 폴백
 *
 * === 타 모듈과의 연결 ===
 * - oslib/inet_aton.c: 폴백 구현(inet_pton(AF_INET,...) 래퍼).
 * - engines/net.c, engines/rdma.c, server.c: IPv4 문자열 파싱 소비자.
 * - <arpa/inet.h>: struct in_addr 정의 공급(본 헤더에서 직접 include).
 * - configure: 시스템 inet_aton 감지 후 본 선언 포함 여부 결정.
 *
 * === 주요 함수/구조체 요약 ===
 * - inet_aton(cp, inp): 문자열 → in_addr 변환. 성공 1, 실패 0 반환(표준 BSD 규약).
 *   inet_pton 과 다른 규약(inet_pton 은 1/0/-1) 에 주의.
 */
#ifndef FIO_INET_ATON_LIB_H
/* [한국어] 헤더 가드 — 네트워크 엔진/서버가 동시 포함 가능. */
#define FIO_INET_ATON_LIB_H

#include <arpa/inet.h>
/* [한국어] <arpa/inet.h> 포함 이유: struct in_addr 타입 정의 공급. 시스템 제공
 * inet_aton 선언도 여기 있으므로, CONFIG_HAVE_INET_ATON 이 정의된 빌드에서는
 * 본 파일의 선언과 충돌 없이 <arpa/inet.h> 의 표준 선언이 사용된다. */

/*
 * [한국어]
 * inet_aton - IPv4 점 4쌍 문자열을 이진 네트워크 주소로 변환
 *
 * @cp:  "192.168.1.1" 형태의 NUL 종료 ASCII 문자열. 십진·팔진(0 접두)·
 *       16진(0x 접두)·3쌍(a.b.c — c 가 하위 16비트)·2쌍·1쌍 역사적 변종도 허용
 *       (BSD 관대 파서 규약). NULL 금지.
 * @inp: 결과를 받을 struct in_addr 포인터. 성공 시 inp->s_addr 에 네트워크 바이트
 *       순서 32비트 값 저장(이미 htonl 된 상태).
 * @return: 성공 1, 실패(형식 오류) 0. inet_pton 의 1/0/-1 규약과 혼동 금지.
 *
 * 호출 체인: server.c / engines/rdma.c / engines/net.c → inet_aton() → sockaddr 채움.
 * 실행 컨텍스트: 메인 또는 잡 스레드. 에러 경로: 실패 시 inp 미정, 호출자가 0 반환 처리.
 */
int inet_aton(const char *cp, struct in_addr *inp);

#endif
