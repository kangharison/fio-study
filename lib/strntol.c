/*
 * [한국어 설명] 길이 제한 문자열-정수 변환 (strntol.c)
 *
 * === 파일의 역할 ===
 * 표준 strtol(3) 은 NUL 종결 C 문자열을 기대하나, fio 내부 파서(예: pattern.c 의
 * 바이트 패턴 "0xDEADBEEF:0x42:foo" 처럼 구분자로 분리된 토큰)는 버퍼 내부의
 * 일부 구간만 정수로 해석해야 하는 경우가 많다. 본 파일의 strntol() 은 "시작 포인터 +
 * 최대 길이"를 받아, 해당 구간을 내부 24B 스택 버퍼로 복사한 뒤 NUL 종결하여
 * strtol() 에 위임하고, 반환된 endptr 포인터를 원래 입력 버퍼 안의 대응 위치로
 * 보정하는 안전한 래퍼를 제공한다. 버퍼 크기 24는 LONG_MIN/LONG_MAX 의 최대
 * 십진 표기(부호 포함 약 21자)와 여유분을 합한 값이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 공용 문자열 파싱 유틸리티. 현재 lib/pattern.c 의 number 토큰 해석 경로에서
 * 직접 호출되며, 호출자는 이후 endptr 을 이용해 구분자(':' 등) 뒤의 잔여 입력을
 * 계속 파싱한다. 일회성 변환용이라 상태를 보유하지 않고, 재진입/스레드 안전하다.
 *
 * === 타 모듈과의 연결 ===
 * - lib/pattern.c: 주 호출자. --buffer_pattern=0xAB:0xCD 같은 옵션 파싱.
 * - strntol.h: 함수 프로토타입 선언.
 * - <stdlib.h> strtol(3): 실제 변환 엔진. base 0/2/8/10/16 해석 규칙,
 *   errno=ERANGE + LONG_MIN/LONG_MAX 반환, 선행 공백/부호 처리 규칙을 공유.
 * - <string.h> memcpy: 입력 일부를 스택 버퍼로 복사.
 * - <limits.h> LONG_MIN/LONG_MAX: 오버플로우 판정용 경계값.
 *
 * === 주요 함수/구조체 요약 ===
 * - strntol(str, sz, end, base): 길이 sz 범위의 입력을 base 진법으로 long 파싱.
 *   반환: 변환된 long; 오버플로우 시 LONG_MIN 또는 LONG_MAX + errno=ERANGE(strtol 규약).
 *   end(NULL 가능): 원 입력 내부에서 "처음 비변환 문자"를 가리키도록 설정.
 *   입력 크기가 0이거나 내부 버퍼(24)를 초과하면 ret=0, *end=str 그대로 반환(실패 신호).
 * - 본 파일은 자체 구조체를 정의하지 않는다.
 */
#include <string.h>             /* [한국어] memcpy 사용자 입력 → 스택 임시 버퍼로 복사 */
#include <stdlib.h>             /* [한국어] strtol(3) 및 NULL 매크로 */
#include <limits.h>             /* [한국어] LONG_MIN/LONG_MAX — 오버플로우 결과 비교용 */

#include "strntol.h"            /* [한국어] 본 파일이 공개하는 strntol() 프로토타입 */

/*
 * [한국어]
 * strntol - 길이가 제한된(비-NUL-종결 가능) 문자열을 long 정수로 안전하게 변환한다.
 *
 * @str:  변환 시작 포인터. NULL 허용하지 않음(호출자 책임).
 *        NUL 종결이 보장되지 않아도 된다 — sz 로 경계 지정하기 때문.
 * @sz:   str 부터 읽을 수 있는 최대 바이트 수(포함하지 않는 상한).
 *        선행 공백 스킵으로 beg 이 앞으로 당겨질 때 sz 도 함께 감소.
 * @end:  (옵션) 변환 후 "변환에 사용되지 않은 첫 문자"를 가리킬 포인터의 주소.
 *        NULL 이면 endptr 갱신을 생략. 유효할 때는 원 입력 버퍼 내부 위치가 되도록
 *        내부 버퍼 기준 endptr 을 beg 기준으로 재보정한다.
 * @base: 진법. 0=strtol 자동(0x→16, 0→8, 그 외→10), 2/8/10/16 명시 가능.
 * @return: 성공 시 변환된 long 값.
 *          오버플로우: LONG_MIN/LONG_MAX (strtol(3) 규약, errno=ERANGE 세팅됨).
 *          입력이 비었거나 내부 버퍼(24B) 초과: 0 반환 + *end=str 로 "변환 없음" 신호.
 *
 * 동작 단계:
 *   1) beg 을 str 에서 시작해 ' '(공백)만 건너뛴다. sz 도 동기 감소.
 *      strtol(3) 자체도 선행 공백을 스킵하지만, sz 를 먼저 먹지 않으면 버퍼
 *      오버플로우 검사(아래 2단계)가 부정확해지므로 여기서 선제 소비한다.
 *   2) 남은 sz 가 0 이거나 24(sizeof(buf)) 이상이면 안전하게 처리 불가 →
 *      0 반환. 24 제한은 부호/0x 접두 포함 long 의 최대 표기를 덮는 크기.
 *   3) beg 부터 sz 바이트를 스택 buf 에 memcpy 후 NUL 종결.
 *   4) strtol(buf, end, base) 로 실제 변환. end 는 임시로 buf 내부 주소를 받는다.
 *   5) 오버플로우 특수값(LONG_MIN/LONG_MAX)이면 그대로 반환 — *end 재매핑 생략해도
 *      호출자는 보통 errno/반환값 자체로 에러 검출하므로 문제 없음.
 *   6) 정상 경로면 "*end 은 buf 기반 주소" → "beg + (*end - buf)" 로 원 입력 주소로 환산.
 *
 * 실행 컨텍스트: 파서 경로(init/parse_options) 또는 옵션 파싱 중 어디서나 호출.
 *               상태 없음 → MT-안전, 재진입 안전.
 *
 * 호출 체인: pattern.c(parse_number/__parse_pattern) → [strntol] → strtol(3).
 *
 * 에러 처리: 호출자는 반환값 + *end 변화량으로 "얼마나 소비됐는지" 판단.
 *           소비 0( *end==str )이면 해당 토큰은 숫자가 아님.
 */
long strntol(const char *str, size_t sz, char **end, int base)
{
	/* Expect that digit representation of LONG_MAX/MIN
	 * not greater than this buffer */
	/* [한국어] long 표기 최대 길이(21B) + 여유 → 24B 스택 버퍼. heap 할당 회피로 MT 안전 */
	char buf[24];
	/* [한국어] strtol 결과 저장 */
	long ret;
	/* [한국어] 공백 스킵 후의 실제 숫자 시작 위치; endptr 보정의 기준점 */
	const char *beg = str;

	/* Catch up leading spaces */
	/* [한국어] ASCII 공백 ' ' 만 소비 (탭/줄바꿈은 strtol 에 위임).
	 * sz 를 함께 감소시켜 버퍼 오버플로우 검사의 기준을 정확하게 유지 */
	for (; beg && sz && *beg == ' '; beg++, sz--)
		;

	/* [한국어] sz=0 : 변환할 문자 없음.
	 * sz>=24 : 스택 버퍼보다 크거나 같은 입력은 복사 불가(잘라내면 의미가 달라질 수 있어 거부) */
	if (!sz || sz >= sizeof(buf)) {
		/* [한국어] 호출자에게 "변환 0바이트" 를 알리기 위해 end 에 원 입력(str) 그대로 기록 */
		if (end)
			*end = (char *)str;
		/* [한국어] 관례상 변환 실패 시 0 반환. 호출자는 *end==str 로 실제 실패 판단 가능 */
		return 0;
	}

	/* [한국어] beg..beg+sz 를 임시 버퍼로 복사 후 NUL 종결 — strtol 의 C-string 전제 충족 */
	memcpy(buf, beg, sz);
	/* [한국어] sz 는 sizeof(buf) 미만이 보장되므로 buf[sz] 접근 안전 */
	buf[sz] = '\0';
	/* [한국어] 실제 변환. *end 은 일시적으로 buf 내부 주소를 받음 */
	ret = strtol(buf, end, base);
	/* [한국어] 오버플로우 케이스: errno=ERANGE 도 strtol 이 세팅. *end 보정은 불필요
	 * (호출자는 반환값으로 에러를 식별하는 것이 보통) */
	if (ret == LONG_MIN || ret == LONG_MAX)
		return ret;
	/* [한국어] 성공 경로: *end 을 buf 기준 주소에서 원 입력(beg) 기준 주소로 재매핑.
	 * 이로써 호출자는 원래 자신이 넘긴 버퍼 내에서 다음 파싱 위치를 알 수 있다 */
	if (end)
		*end = (char *)beg + (*end - buf);
	return ret;
}
