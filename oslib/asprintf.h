/*
 * [한국어 설명] asprintf/vasprintf 폴리필 헤더 (asprintf.h)
 *
 * === 파일의 역할 ===
 * 시스템 C 라이브러리가 asprintf(3)/vasprintf(3) 를 제공하지 않을 때를 위한
 * 폴백 선언부이다. 두 함수는 printf 형식 문자열을 동적 할당 버퍼에 출력하며
 * (*strp 에 strdup 된 결과 저장, 반환값은 저장된 문자 수), 호출자가 free()
 * 로 해제해야 한다. GNU 확장으로 glibc/musl/BSD 대부분에서 제공되지만
 * Windows MSVCRT, 일부 임베디드 libc, 오래된 Solaris 등은 미지원이라 fio
 * 의 configure 가 CONFIG_HAVE_ASPRINTF / CONFIG_HAVE_VASPRINTF 매크로로
 * 판별해 본 헤더의 선언 여부를 결정한다. 시스템 제공 시 이 헤더의 선언은
 * 사라지고 <stdio.h> 의 표준 선언이 사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 레이어는 fio 전반의 이식성 경계층이다. asprintf 는 옵션 파싱,
 * 파일 경로 조립(verify state 파일·iolog 이름), 로그 메시지, 에러 문자열,
 * HTTP 엔진의 URL/헤더 조립 등 문자열 동적 생성이 필요한 모든 곳에서 호출된다.
 * 호출 체인(예시):
 *   verify.c::verify_save_state() → aux_path || asprintf(&path, "%s/%s", ...)
 *   engines/http.c::_add_aws_auth_header() → asprintf(&hdr, "Authorization: ...")
 *   server.c::fio_server_parse_string() → asprintf(&host_copy, ...)
 *   기타 init.c/options.c/stat.c 의 동적 로그 포맷 경로.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/asprintf.c: 폴백 구현(vsnprintf 2회 호출 — 길이 측정 → malloc → 실제 포맷).
 * - configure / config-host.h: CONFIG_HAVE_ASPRINTF / CONFIG_HAVE_VASPRINTF 정의 여부.
 * - fio.h / 각종 .c: 이 헤더를 간접 포함하여 asprintf 사용.
 * - <stdarg.h>: va_list 타입 정의 공급(vasprintf 시그니처에 필요).
 *
 * === 주요 함수/구조체 요약 ===
 * - vasprintf(strp, fmt, ap): va_list 기반 포맷 동적 할당.
 * - asprintf(strp, fmt, ...): 가변 인자 편의 래퍼.
 */
#ifndef FIO_ASPRINTF_H
/* [한국어] 헤더 가드 — 여러 translation unit 에서 포함 가능. */
#define FIO_ASPRINTF_H

#include <stdarg.h>
/* [한국어] <stdarg.h> 포함 이유: vasprintf 의 va_list 매개변수 타입 공급. C 표준에
 * 정의된 헤더라 이식성 안전. va_start/va_arg/va_end 매크로도 구현 측에서 활용. */

#ifndef CONFIG_HAVE_VASPRINTF
/*
 * [한국어]
 * vasprintf - va_list 기반 printf 형식 문자열 동적 할당
 *
 * @strp: 결과 포인터 저장 위치. 성공 시 *strp 에 malloc 된 NUL 종료 문자열 저장,
 *        호출자가 free() 로 해제해야 함. 실패 시 *strp 값은 정의되지 않음.
 * @fmt:  printf 형식 문자열.
 * @ap:   가변 인자 목록(이미 va_start 로 초기화된 상태).
 * @return: 저장된 문자 수(NUL 제외). 실패 시 -1.
 *
 * 폴백 구현: vsnprintf(NULL,0,...) 로 길이 측정 → malloc → vsnprintf 재호출.
 * 호출 체인: asprintf() 가 내부적으로 호출, 또는 호출자가 직접 va_list 로 호출.
 */
int vasprintf(char **strp, const char *fmt, va_list ap);
#endif

#ifndef CONFIG_HAVE_ASPRINTF
/*
 * [한국어]
 * asprintf - 가변 인자 printf 형식 문자열 동적 할당
 *
 * @strp: 결과 포인터 저장 위치(vasprintf 와 동일).
 * @fmt:  printf 형식 문자열.
 * @...:  가변 인자.
 * @return: 저장된 문자 수(NUL 제외). 실패 시 -1.
 *
 * 구현은 단순히 va_start 후 vasprintf 로 위임. 시스템 제공 asprintf 가 있으면
 * CONFIG_HAVE_ASPRINTF 로 본 선언을 생략해 표준 선언을 사용.
 */
int asprintf(char **strp, const char *fmt, ...);
#endif

#endif /* FIO_ASPRINTF_H */
