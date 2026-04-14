/*
 * [한국어 설명] 대소문자 무시 부분 문자열 검색 함수 폴리필 헤더 (strcasestr.h)
 *
 * === 파일의 역할 ===
 * strcasestr() 함수가 시스템에 없는 경우를 위한 선언부이다.
 * strcasestr()은 대소문자를 구분하지 않고 haystack에서 needle을 검색한다.
 * GNU 확장 함수로 모든 플랫폼에서 사용 가능하지 않을 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio에서 대소문자 무시 문자열 검색에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strcasestr.c: 실제 구현부
 * - configure: CONFIG_STRCASESTR 매크로 정의 여부 결정
 *
 * === 주요 함수 요약 ===
 * - strcasestr(): 대소문자 무시 부분 문자열 검색
 */
#ifndef CONFIG_STRCASESTR

#ifndef FIO_STRCASESTR_H
#define FIO_STRCASESTR_H

char *strcasestr(const char *haystack, const char *needle);

#endif

#endif
