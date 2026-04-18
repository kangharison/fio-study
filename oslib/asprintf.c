/*
 * [한국어 설명] printf-스타일 동적 할당 포매터 폴리필 구현 (asprintf.c)
 *
 * === 파일의 역할 ===
 * GNU/BSD 확장 함수 asprintf(3) 와 vasprintf(3) 의 폴리필을 제공한다.
 * 이 두 함수는 printf 의미론으로 포맷 결과를 "자동으로 적절한 크기의 힙
 * 메모리에" 기록해 소유권 있는 문자열을 반환한다(호출자 free 필요). 즉
 * snprintf 처럼 고정 버퍼를 미리 준비할 필요가 없어, 경로 조합/로그 메시지
 * 조립/옵션 문자열 생성 등에서 매우 유용하다. glibc·BSD libc 는 이를
 * 기본 제공하지만 Windows MSVC/일부 libc 는 제공하지 않으므로 configure
 * 단계에서 CONFIG_HAVE_ASPRINTF / CONFIG_HAVE_VASPRINTF 의 유무에 따라 각
 * 함수를 개별적으로 폴리필한다(두 심볼 독립 가드).
 *
 * 구현 핵심: vsnprintf 의 C99 규약을 활용한 "2-pass" 접근법 —
 *   pass 1: vsnprintf(NULL, 0, fmt, ap_copy) 로 필요한 문자 수(NUL 제외)만
 *           계산. 출력 버퍼를 건드리지 않는다.
 *   pass 2: malloc(len+1) 로 공간 확보 후 vsnprintf(str, len, fmt, ap) 로
 *           실제 기록.
 * va_list 는 pass 1 에서 소비되므로 va_copy 로 사본을 만들어 쓰는 것이
 * 필수다(va_list 는 단일 패스 API).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 oslib/ 이식성 계층. fio 전역이 경로 합성(예: /sys/class/mtd/
 * mtd%d/name), 동적 에러 메시지, IPC 커맨드 라인 조립, 옵션 덤프 등에
 * asprintf 를 광범위하게 사용한다. 따라서 CONFIG_HAVE_* 미정의 플랫폼에서도
 * 본 폴리필이 빠지면 빌드 자체가 실패한다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 전역 — oslib/linux-blkzoned.c(sysfs 경로 조립), stat.c/
 *   server.c(JSON/에러 메시지), options.c(동적 help 문자열) 등 다수.
 * - 의존: <stdio.h> 의 vsnprintf(3), <stdlib.h> 의 malloc(3), <stdarg.h>의
 *   va_list/va_start/va_end/va_copy(또는 __va_copy 폴백). 대상 문자열
 *   포인터 strp 는 성공 시 malloc 된 힙 사본을 가리키며 호출자가 free 한다.
 * - 공유 상태: 없음(순수 함수·재진입/스레드 안전, malloc 스레드 안전성 전제).
 *
 * === 주요 함수 요약 ===
 * - vasprintf(): va_list 를 받아 *strp 에 동적 할당 포맷 결과 저장.
 * - asprintf(): 가변 인자를 va_list 로 수렴해 vasprintf 를 호출하는 래퍼.
 */
#include <stdio.h>  /* [한국어] vsnprintf 선언 — 포맷 결과 크기 계산 및 기록. */
#include <stdlib.h>  /* [한국어] malloc 선언 — 출력 버퍼 힙 확보. */
#include "oslib/asprintf.h"  /* [한국어] 공개 API 선언, <stdarg.h> 간접 포함. */

/*
 * [한국어]
 * vasprintf - va_list 기반 포맷 결과를 자동 크기 힙 버퍼에 기록
 *
 * @strp: 결과 포인터가 저장될 out 파라미터. 성공 시 malloc 된 NUL-종단
 *        문자열 포인터, 실패 시 원자성 보장 없음(상위는 반환값 검사 필요).
 *        호출자는 free(*strp) 로 해제 책임.
 * @fmt: printf 포맷 문자열.
 * @ap: 초기화된 va_list — 이 함수가 (1회) 소비한다.
 * @return: 성공 시 기록된 문자 수(NUL 제외), 실패(-1)는 크기 계산 오류 또는
 *          malloc 실패.
 *
 * 왜 2-pass 인가: printf 의 결과 길이는 포맷과 실인자에 의존하므로 단일
 * 패스로 충분한 버퍼 크기를 알 수 없다. C99 에서 vsnprintf(NULL, 0, ...)
 * 가 "기록하지 않고 필요한 길이만 반환"하는 계약이 생겨 이를 2-pass 로
 * 활용한다. va_list 는 "단일 순회" 자원이므로 pass 1 에서 소비된 ap 를
 * pass 2 에서 재사용하지 못해 va_copy 로 보존본을 만든다.
 *
 * 동작 단계:
 * 1) va_copy(ap_copy, ap) — 구형 컴파일러에 대비한 __va_copy 폴백 포함.
 * 2) len = vsnprintf(NULL, 0, fmt, ap_copy); va_end(ap_copy).
 * 3) len < 0 이면 포맷 오류 — 그대로 실패 전파.
 * 4) len++ 하여 NUL 공간 포함 크기로 malloc.
 * 5) *strp 에 힙 포인터 할당.
 * 6) malloc 실패 시 -1, 성공 시 vsnprintf(str, len, fmt, ap) 반환(기록된 길이).
 *
 * 호출 체인:
 *   fio 전역 → asprintf() 또는 직접 → [vasprintf()] → vsnprintf() + malloc()
 */
#ifndef CONFIG_HAVE_VASPRINTF  /* [한국어] libc 가 vasprintf 를 제공하면 정의 생략. */
int vasprintf(char **strp, const char *fmt, va_list ap)
{
	va_list ap_copy;  /* [한국어] pass 1 에서 소비할 va_list 사본 — 원본 ap 는 pass 2 에서 사용. */
	char *str;  /* [한국어] malloc 된 결과 버퍼 임시 포인터. */
	int len;  /* [한국어] pass 1 에서 측정한 필요 문자 수(NUL 제외). */

	/* [한국어] va_list를 복제 - vsnprintf()가 ap를 소비하므로 원본을 보존 */
#ifdef va_copy
	va_copy(ap_copy, ap);  /* [한국어] C99 표준 va_copy. */
#else
	/* [한국어] va_copy가 없는 구형 컴파일러를 위한 대체 매크로 */
	__va_copy(ap_copy, ap);  /* [한국어] GCC 의 non-표준 대체 — 동일 의미. */
#endif
	/* [한국어] 1단계: 버퍼 없이 호출하여 필요한 문자 수만 계산 */
	len = vsnprintf(NULL, 0, fmt, ap_copy);  /* [한국어] C99: NULL, 0 은 기록 없이 길이만 반환 계약. */
	va_end(ap_copy);  /* [한국어] 사본 해제 — C99 규약으로 va_copy 는 대응 va_end 필수. */

	if (len < 0)  /* [한국어] 포맷 인코딩 오류(예: wide char 변환 실패) — 실패 전파. */
		return len;

	/* [한국어] null 종료 문자를 위해 1바이트 추가 후 메모리 할당 */
	len++;  /* [한국어] NUL 포함 전체 버퍼 크기. */
	str = malloc(len);  /* [한국어] 힙 할당 — 실패 시 NULL. */
	*strp = str;  /* [한국어] 호출자에게 소유권 이전(또는 NULL 설정). */
	/* [한국어] 2단계: 실제로 포맷 문자열을 할당된 버퍼에 기록 */
	return str ? vsnprintf(str, len, fmt, ap) : -1;  /* [한국어] 성공 시 기록 문자 수, 실패 시 -1. */
}
#endif  /* [한국어] !CONFIG_HAVE_VASPRINTF 종료. */

/*
 * [한국어]
 * asprintf - 가변 인자 기반 포맷 결과를 자동 크기 힙 버퍼에 기록(vasprintf 래퍼)
 *
 * @strp: 결과 힙 포인터 저장처. 호출자 free 책임.
 * @fmt: printf 포맷 문자열.
 * @...: 가변 인자.
 * @return: 기록된 문자 수(성공) 또는 -1(실패).
 *
 * 편의 래퍼로 va_start/va_end 경계만 설정하고 본체는 vasprintf 에 위임한다.
 *
 * 호출 체인:
 *   fio 전역 → [asprintf()] → vasprintf()
 */
#ifndef CONFIG_HAVE_ASPRINTF  /* [한국어] libc 네이티브 asprintf 가 있으면 정의 생략. */
int asprintf(char **strp, const char *fmt, ...)
{
	va_list arg;  /* [한국어] ... 인자를 수렴할 va_list. */
	int done;  /* [한국어] 기록 문자 수 반환값. */

	va_start(arg, fmt);  /* [한국어] fmt 이후 인자 시작 지점에 arg 를 바인딩. */
	done = vasprintf(strp, fmt, arg);  /* [한국어] 공용 구현으로 위임. */
	va_end(arg);  /* [한국어] va_list 해제 — C99 규약. */

	return done;  /* [한국어] vasprintf 반환값 그대로 전달. */
}
#endif  /* [한국어] !CONFIG_HAVE_ASPRINTF 종료. */
