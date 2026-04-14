/*
 * [한국어 설명] 문자열 토큰화 함수 폴리필 헤더 (strsep.h)
 *
 * === 파일의 역할 ===
 * strsep() 함수가 시스템에 없는 경우를 위한 선언부이다.
 * strsep()은 strtok()의 재진입 가능(thread-safe)한 대안으로,
 * 구분자(delimiter)를 기준으로 문자열을 토큰으로 분리한다.
 * BSD 계열에서 유래했으며 일부 시스템에서는 없을 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio에서 문자열 파싱/토큰화에 사용된다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strsep.c: 실제 구현부
 * - configure: CONFIG_STRSEP 매크로 정의 여부 결정
 *
 * === 주요 함수 요약 ===
 * - strsep(): 구분자 기반 문자열 토큰 분리 (strtok의 재진입 가능 대안)
 */
#ifndef CONFIG_STRSEP

#ifndef FIO_STRSEP_LIB_H
#define FIO_STRSEP_LIB_H

char *strsep(char **, const char *);

#endif

#endif
