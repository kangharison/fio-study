/*
 * [한국어 설명] strlcat 폴리필 헤더 (strlcat.h)
 *
 * === 파일의 역할 ===
 * OpenBSD 유래 안전 문자열 연결 함수 strlcat(3) 가 시스템에 없을 때를 위한
 * 폴백 선언부이다. strlcat 은 strncat 과 달리:
 *   - 대상 버퍼의 **전체** 크기를 인자로 받아 오버플로 방지가 명확.
 *   - 반환값이 "연결 시도했다면 필요했을 총 길이(src 와 dst 의 연결된 상태의
 *     strlen)" 라 truncation 여부를 호출자가 (반환값 >= dsize) 로 쉽게 감지.
 *   - 결과는 항상 NUL 종료(버퍼 크기가 0 이 아닌 한).
 * BSD 계열에 기본 포함되지만 glibc 는 2.38 이전까지 미포함이었고 일부 Linux
 * 배포에서는 여전히 없을 수 있어 fio configure 가 CONFIG_STRLCAT 로 판별한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어. 경로 조립, 로그 버퍼 누적, terse 출력의 CSV 생성 등
 * 길이 제한 버퍼에 문자열을 안전하게 덧붙일 필요가 있는 모든 곳에서 사용.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strlcat.c: 폴백 구현(BSD 원본 포팅 — 길이 검사 후 byte-wise 복사).
 * - configure: CONFIG_STRLCAT 정의 여부 결정.
 * - <string.h>: CONFIG_STRLCAT 경로에서 표준 선언 대신 사용됨.
 * - <stddef.h>: size_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - strlcat(dst, src, dsize): 안전한 문자열 연결. 반환값으로 truncation 감지.
 */
#ifndef CONFIG_STRLCAT
/* [한국어] 시스템이 strlcat 미제공 시에만 본 폴백 선언 노출. */

#ifndef FIO_STRLCAT_H
/* [한국어] 헤더 가드. */
#define FIO_STRLCAT_H

#include <stddef.h>
/* [한국어] <stddef.h> 포함 이유: size_t 타입 공급. C 표준 헤더로 의존성 안전. */

/*
 * [한국어]
 * strlcat - 버퍼 크기 인식 안전 문자열 연결
 *
 * @dst:   대상 버퍼(NUL 종료 문자열 또는 NUL 포함). 최소 dsize 바이트 크기여야 함.
 * @src:   추가할 NUL 종료 문자열.
 * @dsize: 대상 버퍼의 전체 바이트 크기(NUL 포함). sizeof(buf) 값을 그대로 전달.
 * @return: strlen(dst) + strlen(src) 가 반환될 값(이상적인 전체 길이).
 *          반환값 >= dsize 면 truncation 발생 → 결과 문자열은 잘렸음.
 *
 * 동작: dst 의 기존 NUL 까지 탐색 후 남은 공간(dsize - dlen - 1) 만큼만 src 복사,
 *      마지막에 NUL 추가. 자리 부족해도 반드시 NUL 종료.
 *
 * 호출 체인: 경로 조립·로그 누적 등 다양한 문자열 조작 지점.
 * 실행 컨텍스트: 호출자 컨텍스트 따름. 에러: 없음(순수 함수).
 */
size_t strlcat(char *dst, const char *src, size_t dsize);

#endif

#endif
