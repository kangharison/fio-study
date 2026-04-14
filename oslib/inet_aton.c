/*
 * [한국어 설명] IP 주소 문자열 파싱 함수 폴리필 구현 (inet_aton.c)
 *
 * === 파일의 역할 ===
 * inet_aton() 함수의 폴리필 구현을 제공한다. 내부적으로 최신 표준 함수인
 * inet_pton()을 호출하는 단순 래퍼이다. inet_aton()을 직접 제공하지 않는
 * 시스템(예: 일부 Windows 환경)을 위한 호환성 레이어이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, 네트워크 관련 코드에서 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: 네트워크 I/O 엔진 (engines/net.c)
 * - 의존: inet_pton() (POSIX 표준 소켓 함수)
 *
 * === 주요 함수 요약 ===
 * - inet_aton(): inet_pton(AF_INET, ...)의 래퍼
 */
#include "inet_aton.h"

/*
 * [한국어]
 * inet_aton - IPv4 문자열을 이진 네트워크 주소로 변환
 *
 * @cp: "192.168.1.1" 형태의 IPv4 주소 문자열
 * @inp: 변환된 이진 주소가 저장될 구조체
 * @return: 성공 시 1, 실패 시 0
 *
 * inet_pton(AF_INET, ...)을 직접 호출하는 단순 래퍼이다.
 * inet_pton은 inet_aton의 현대적 대체 함수로 IPv6도 지원한다.
 *
 * 호출 체인:
 *   engines/net.c 등 → [inet_aton()] → inet_pton()
 */
int inet_aton(const char *cp, struct in_addr *inp)
{
	return inet_pton(AF_INET, cp, inp);
}
