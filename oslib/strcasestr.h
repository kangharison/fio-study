/*
 * [한국어 설명] strcasestr 폴리필 헤더 (strcasestr.h)
 *
 * === 파일의 역할 ===
 * GNU 확장 문자열 함수 strcasestr(3) 가 시스템에 없을 때를 위한 폴백 선언부이다.
 * strcasestr 은 strstr 의 대소문자 무시 변종으로, haystack 에서 needle 을
 * ASCII 대소문자 구분 없이 검색하여 첫 일치 위치의 포인터를 반환한다. glibc/musl/
 * 일부 BSD 는 제공하지만 Windows MSVCRT, Solaris 일부 버전에는 없어 fio configure
 * 가 CONFIG_STRCASESTR 로 판별 후 본 헤더의 선언을 활성화한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어. fio 에서는 옵션 파싱·ioengine 이름 매칭·로그 필터링 등
 * 사용자 지정 토큰의 케이스 인센시티브 검색이 필요한 곳에서 호출된다.
 * 호출 체인(예시):
 *   init.c/options.c 의 possible values 매칭, engines/ 의 URI 스키마 감지,
 *   server.c 의 클라이언트 버전 문자열 검사 등.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strcasestr.c: 소문자 정규화 + strstr 조합의 폴백 구현.
 * - configure: CONFIG_STRCASESTR 정의(시스템 제공 감지) 시 본 선언 제거.
 * - 시스템 <string.h>: CONFIG_STRCASESTR 경로에서 표준 선언이 대신 사용됨.
 *
 * === 주요 함수/구조체 요약 ===
 * - strcasestr(haystack, needle): 대소문자 무시 부분 문자열 검색.
 */
#ifndef CONFIG_STRCASESTR
/* [한국어] 시스템이 strcasestr 을 제공하지 않는 경우에만 선언 노출. */

#ifndef FIO_STRCASESTR_H
/* [한국어] 헤더 가드. */
#define FIO_STRCASESTR_H

/*
 * [한국어]
 * strcasestr - 대소문자 무시 부분 문자열 검색
 *
 * @haystack: 검색 대상 문자열(NUL 종료).
 * @needle:   찾을 문자열(NUL 종료). 빈 문자열이면 haystack 반환(표준 strstr 규약).
 * @return:   첫 일치 위치의 포인터(haystack 내부). 미발견 시 NULL.
 *
 * 동작: needle 과 haystack 의 해당 구간을 tolower() 후 비교하는 naive 구현.
 *      (폴백이 필요한 플랫폼에서는 성능보다 이식성이 우선.)
 *
 * 호출 체인: fio 옵션 파싱·엔진 이름 매칭 등 케이스 인센시티브 검색 경로.
 * 실행 컨텍스트: 메인 또는 잡 스레드. 에러: 없음(순수 함수).
 */
char *strcasestr(const char *haystack, const char *needle);

#endif

#endif
