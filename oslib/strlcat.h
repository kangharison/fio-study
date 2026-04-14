/*
 * [한국어 설명] 안전한 문자열 연결 함수 폴리필 헤더 (strlcat.h)
 *
 * === 파일의 역할 ===
 * strlcat() 함수가 시스템에 없는 경우를 위한 선언부이다.
 * strlcat()은 strncat()보다 안전한 문자열 연결 함수로, 대상 버퍼의 전체 크기를
 * 인자로 받아 버퍼 오버플로를 방지한다. BSD 계열에서 유래했으며 Linux glibc에는
 * 기본 포함되지 않는 경우가 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어의 일부로, fio 전역에서 안전한 문자열 조작에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strlcat.c: 실제 구현부
 * - configure: CONFIG_STRLCAT 매크로 정의 여부 결정
 *
 * === 주요 함수 요약 ===
 * - strlcat(): 버퍼 크기를 고려한 안전한 문자열 연결
 */
#ifndef CONFIG_STRLCAT

#ifndef FIO_STRLCAT_H
#define FIO_STRLCAT_H

#include <stddef.h>

size_t strlcat(char *dst, const char *src, size_t dsize);

#endif

#endif
