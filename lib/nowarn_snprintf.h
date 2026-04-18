/*
 * [한국어 설명] GCC -Wformat-truncation 경고 억제 snprintf 래퍼 (nowarn_snprintf.h)
 *
 * === 파일의 역할 ===
 * GCC 8 이후부터 snprintf(3) 계열에 대해 도입된 `-Wformat-truncation` 경고는
 * "버퍼가 작아 포맷 결과가 잘릴 가능성이 있다" 는 정적 분석을 너무 엄격히
 * 수행해서, fio 처럼 "잘림을 허용하는 의도적 축약" 이 많은 코드베이스에서
 * 거짓 양성 경고를 대량 유발한다. 본 헤더는 `vsnprintf` 를 감싸서 해당
 * 경고를 함수 단위로만 pragma 로 억제한 인라인 함수 `nowarn_snprintf` 를
 * 제공한다. 호출자는 일반 snprintf 처럼 쓰되 잘림 경고를 받지 않는다.
 * 파일 경로, 잡 이름, 로그 접두사 등 "필요하면 자르면 된다" 는 의미의
 * 문자열 조립에만 선택적으로 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 빌드 품질 향상을 위한 cross-cutting 유틸. stat.c / options.c / server.c /
 * 각 엔진의 설정 메시지 조립부에서 사용된다.
 * 호출 체인: 호출자 (버퍼 조립) → nowarn_snprintf → vsnprintf(3).
 *
 * === 타 모듈과의 연결 ===
 * - <stdio.h> : vsnprintf / snprintf.
 * - <stdarg.h> : va_list, va_start, va_end.
 * - GCC pragma : diagnostic push/pop 로 함수 스코프 경고 억제.
 * 데이터 흐름: 가변 인자 → va_list → vsnprintf → str 버퍼.
 *
 * === 주요 함수/구조체 요약 ===
 * - nowarn_snprintf(str, size, fmt, ...) : snprintf 와 동일 시그니처. -Wformat-
 *   truncation 경고만 억제. 반환값은 vsnprintf 와 동일(잘리지 않았을 때
 *   이론적으로 기록되었을 바이트 수).
 */
#ifndef _NOWARN_SNPRINTF_H_
#define _NOWARN_SNPRINTF_H_
/* [한국어] 헤더 가드. 네임 스타일이 _XXX_H_ 인 것은 초기 커밋의 관습을
 * 유지하기 위함(다른 fio lib/*.h 는 FIO_XXX_H 를 쓰지만 본 파일은 Linux
 * 커널 관습에 가까운 이름을 채택). */

#include <stdio.h>
/* [한국어] <stdio.h> : vsnprintf 프로토타입, size_t 간접 제공. */

#include <stdarg.h>
/* [한국어] <stdarg.h> : va_list 타입과 va_start/va_end 매크로. 가변 인자
 * 함수를 vsnprintf 에 전달하기 위해 필요. */

/*
 * [한국어]
 * nowarn_snprintf - snprintf 의 -Wformat-truncation 억제 래퍼.
 *
 * @str: 출력 버퍼(호출자 소유).
 * @size: 버퍼 크기. 0 허용(이 경우 포맷만 하고 쓰지 않음).
 * @format: printf 포맷 문자열.
 * @...: 가변 인자.
 * @return: vsnprintf 반환값 — 잘리지 않았다면 기록되었을 총 바이트 수(NUL 제외).
 *
 * 구현: va_start → GCC 8+ 에서만 pragma push/ignore 로 -Wformat-truncation 억제
 *   → vsnprintf 호출 → pragma pop → va_end. 비 GCC / GCC 7 이하에서는 pragma
 *   블록이 건너뛰어지고 보통 vsnprintf 호출과 동일.
 *
 * 실행 컨텍스트: 호출자 스레드 인라인. 재진입 안전(스택만 사용).
 * 호출 체인: 호출자 → [이 함수] → vsnprintf(3) → libc.
 */
static inline int nowarn_snprintf(char *str, size_t size, const char *format,
				  ...)
{
	va_list args;
	/* [한국어] 가변 인자 리스트를 보관할 로컬. va_start/va_end 쌍으로 관리. */
	int res;
	/* [한국어] vsnprintf 반환값 보존용 로컬. */

	va_start(args, format);
	/* [한국어] 마지막 명명 인자 format 기준으로 가변 인자 순회 시작. */

#if __GNUC__ -0 >= 8
	/* [한국어] GCC 8 이상에서만 경고 억제 pragma 사용. `__GNUC__ -0` 는
	 * __GNUC__ 가 정의되지 않은 컴파일러(예: 오래된 MSVC) 에서도 0 으로
	 * 평가되게 하는 전통적 방어 이디엄. 주의: 실제로는 "#pragma GCC
	 * diagnostic ignored" 는 문자열이 아니라 식별자를 받으므로 올바른 문법은
	 * `#pragma GCC diagnostic ignored "-Wformat-truncation"` 이다(큰따옴표만).
	 * 현재 원 소스는 push 뒤에 문자열이 붙어 있어 일부 GCC 는 경고를 내기도
	 * 하지만, 빌드 실패는 발생하지 않음. 이 파일의 구현 디테일은 수정 금지
	 * 규약에 따라 그대로 둔다. */
#pragma GCC diagnostic push "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
	res = vsnprintf(str, size, format, args);
	/* [한국어] vsnprintf(3) : 가변 인자 버전 snprintf. 반환은 잘리지
	 * 않았더라면 기록되었을 총 문자 수(NUL 제외). 잘림 감지는 res >= size
	 * 로 호출자가 수행. */
#if __GNUC__ -0 >= 8
#pragma GCC diagnostic pop "-Wformat-truncation"
	/* [한국어] pragma push 로 저장해 둔 경고 상태를 복원. */
#endif
	va_end(args);
	/* [한국어] 가변 인자 리스트 해제. 일부 ABI(x86-64 va_list = 레지스터
	 * 저장 영역 참조) 에서는 필수. */

	return res;
	/* [한국어] vsnprintf 의 원 반환값을 그대로 호출자에게 전달. */
}

#endif
/* [한국어] _NOWARN_SNPRINTF_H_ 가드 종료. */
