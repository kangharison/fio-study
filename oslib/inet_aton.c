/*
 * [한국어 설명] IPv4 점-십진 문자열 파싱 함수 폴리필 구현 (inet_aton.c)
 *
 * === 파일의 역할 ===
 * BSD/SUSv2 시절 표준이던 inet_aton(3) 함수의 폴리필을 제공한다.
 * inet_aton()은 "192.168.1.1" 같은 점-십진 IPv4 문자열을 32비트 네트워크
 * 바이트 순서의 in_addr 구조체로 변환한다. 일부 플랫폼(예: Windows 일부
 * 구버전·glibc 이 아닌 libc)에서 이 심볼이 제공되지 않으므로,
 * configure 스크립트가 해당 심볼 부재를 감지하면 이 폴리필을 빌드에 포함시켜
 * fio 측 호출자(엔진/옵션 파서)가 동일 API 면(面)을 유지하도록 한다.
 * 내부 구현은 IPv6까지 통합 지원하는 현대 POSIX 함수 inet_pton(3)을
 * AF_INET 고정으로 호출하는 단순 래퍼이다 — 즉 glibc 이전의 "numeric string
 * to in_addr" 의미론을 inet_pton 의미론으로 위임한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 oslib/ 이식성 계층에 위치한다. 이 계층은 POSIX.1-2001·Linux·GNU·
 * glibc 의 특정 기능을 다른 플랫폼에서도 안전하게 사용하도록 누락된 심볼을
 * 조건부로 채워넣는 역할을 한다. inet_aton 은 주로 엔진(net·rdma 등)의
 * 호스트 파싱 경로와 일부 서버 IP 설정 경로에서 호출된다. 호출 체인은
 *   main() → parse_options() → init.c → engines/net.c 의 IP 파싱 코드
 *   → [inet_aton()] → inet_pton(AF_INET, ...) 커널 소켓 파싱 API
 * 형태이며, 이 파일은 그 최하단 어댑터 노드에 해당한다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: engines/net.c 의 fio_netio_setup_connect() 등 IP 주소 문자열
 *   → 이진 주소 변환이 필요한 지점, server.c 의 클라이언트 호스트 파싱.
 * - 의존: POSIX.1-2001 소켓 API 인 inet_pton(). AF_INET(4바이트 IPv4)로
 *   호출하므로 netinet/in.h 의 struct in_addr 타입 레이아웃에 의존한다.
 * - 공유 자료구조: struct in_addr {uint32_t s_addr;} — 네트워크 바이트
 *   오더(big-endian)로 저장되는 32비트 IPv4 주소.
 *
 * === 주요 함수 요약 ===
 * - inet_aton(): 점-십진 문자열을 inet_pton(AF_INET)로 위임. 성공 1, 실패 0
 *   (inet_aton 의 반환 관례는 POSIX 의 inet_pton 관례와 동일하게 매핑됨).
 */
#include "inet_aton.h"  /* [한국어] 함수 선언과 struct in_addr 간접 포함을 위한 로컬 헤더. */

/*
 * [한국어]
 * inet_aton - IPv4 점-십진 문자열을 이진 네트워크 주소로 변환
 *
 * @cp: "A.B.C.D" 형태의 NUL-종단 IPv4 문자열. 0xA0·0xC0a80001 등 역사적
 *      BSD 확장 포맷은 지원하지 않는다(inet_pton 이 거부).
 * @inp: 성공 시 32비트 네트워크 바이트 오더 주소를 저장할 out 파라미터.
 *       실패 시 내용 보장 없음.
 * @return: 유효 주소로 성공 시 1, 파싱 실패 시 0. 레거시 inet_aton 관례와
 *          동일하다 (inet_pton 은 1/0/-1 을 반환하지만 AF_INET 분기에서
 *          -1 경로가 관습적으로 나타나지 않으므로 직접 대입해도 무방).
 *
 * 이 래퍼가 필요한 이유: inet_aton 은 BSD 확장으로 시작해 SUSv2 에서
 * 표준화되었다가 SUSv3/POSIX.1-2001 에서 inet_pton 에 의해 obsoleted 되었다.
 * 일부 libc 는 inet_pton 만 제공하므로 fio 의 오래된 호출 사이트를 건드리지
 * 않기 위해 이 폴리필로 ABI 를 유지한다.
 *
 * 호출 체인:
 *   engines/net.c · server.c → [inet_aton()] → inet_pton(AF_INET, ...)
 *   → 커널 소켓 주소 파서
 */
int inet_aton(const char *cp, struct in_addr *inp)
{
	/* [한국어] AF_INET 고정으로 inet_pton 호출 — 4바이트 IPv4 파싱 경로.
	 * inet_pton 의 반환값은 0(포맷 오류) / 1(성공) / -1(지원 안 됨)인데,
	 * AF_INET 분기에서는 -1 이 실용상 발생하지 않으므로 그대로 전달해도
	 * inet_aton 의 0/1 관례와 호환된다. */
	return inet_pton(AF_INET, cp, inp);
}
