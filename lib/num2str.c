/*
 * [한국어 설명] 숫자를 사람이 읽기 쉬운 문자열로 변환 (num2str.c)
 *
 * === 파일의 역할 ===
 * 큰 정수(바이트 수, 블록 수, 비트 수 등)를 "1.23M" / "1.5GiB" / "100 KB/s" 같은
 * SI/IEC 단위 접두사로 환산된 사람 친화적 문자열로 변환한다. 반올림 자리올림까지
 * 포함한 "캐리" 로직을 포함하며, N2S_* enum 으로 단위 접미사(B, bit, /s, B/s, bit/s)
 * 를 선택할 수 있다. pow2 플래그로 1024 기반(IEC: Ki/Mi/Gi) 또는 1000 기반(SI: k/M/G)
 * 두 모드를 지원하며, 사용자 지정 base 승수까지 고려해 원본 숫자의 단위 레벨을
 * 사전 정규화한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * stat.c 의 show_thread_status_normal/terse/json 에서 IOPS/대역폭/총 전송량을
 * 터미널 출력용 문자열로 포맷할 때 주로 호출된다. 또한 diskutil.c/eta.c 등의
 * 진행 상황 출력, ETA/rate 계산 결과 표시에도 사용된다. 반환값은 asprintf 로
 * 할당된 heap 메모리이므로 호출자가 free 책임을 진다.
 *
 * === 타 모듈과의 연결 ===
 * - num2str.h:                       함수 프로토타입, enum n2s_unit (N2S_NONE, N2S_PERSEC, N2S_BYTE,
 *                                    N2S_BIT, N2S_BYTEPERSEC, N2S_BITPERSEC) 정의.
 * - ../compiler/compiler.h:          compiletime_assert 매크로 — 빌드 시 단위 배열 크기 일치 검증.
 * - ../oslib/asprintf.h:             이식 가능 asprintf — 결과 문자열 heap 할당.
 * - stat.c / diskutil.c / eta.c:     주 호출자.
 * - bytes2str_simple():              간단한 IEC 변환 — 버퍼 기반으로 heap 할당 회피.
 *
 * === 주요 함수/구조체 요약 ===
 * - bytes2str_simple(buf, bufsize, bytes):
 *     bytes 를 1024 기반 IEC 문자열("1.50 GiB") 로 buf 에 snprintf. buf 반환.
 *     소수 2자리 고정 — 단순 출력 용도.
 * - num2str(num, maxlen, base, pow2, units):
 *     num 을 maxlen 자릿수 이하가 되도록 승수 축약. 반올림/자리올림 포함.
 *     반환: asprintf 로 malloc 된 문자열(호출자가 free 책임).
 * 자체 구조체 없음 — 내부 정적 배열로 단위 접두사 테이블 유지.
 */
#include <assert.h>             /* [한국어] units 인덱스 범위 검사용 assert */
#include <stdlib.h>             /* [한국어] malloc/free (asprintf 의 내부 의존) */
#include <stdio.h>              /* [한국어] sprintf/snprintf — 내부 포매팅 */
#include <string.h>             /* [한국어] strlen — 자릿수 계산 */

#include "../compiler/compiler.h"   /* [한국어] compiletime_assert 매크로 — sistr/iecstr 크기 일치 보장 */
#include "../oslib/asprintf.h"      /* [한국어] asprintf — 동적 할당 문자열 포매팅(Windows 포함 이식 래퍼) */
#include "num2str.h"                /* [한국어] 공개 API 프로토타입 + enum n2s_unit 정의 */


/* [한국어] IEC 이진 접두사 테이블.
 * 인덱스 0..6 각각 "" / Ki / Mi / Gi / Ti / Pi / Ei — 2^10/^20/^30/^40/^50/^60.
 * 길이 7. static const 로 모듈 내 공유.
 * 설정자: 모듈 초기화 시 상수로 고정.
 * 읽는 자: bytes2str_simple, num2str 가 pow2==1 모드에서 unitprefix 로 선택.
 * 값 범위: 문자열 리터럴 집합 — 변경 불가.
 * 동기화: 읽기 전용이므로 락 불필요. */
static const char *iecstr[] = { "", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei" };

/**
 * bytes2str_simple - Converts a byte value to a human-readable string.
 * @buf:      buffer to store the resulting string
 * @bufsize:  size of the buffer
 * @bytes:    number of bytes to convert
 * @returns : pointer to the buf containing the formatted string.
 * Converts the given byte value into a human-readable string using IEC units
 * (e.g., KiB, MiB, GiB), and stores the result in the provided buffer.
 * The output is formatted with two decimal places of precision.
 */
/*
 * [한국어]
 * bytes2str_simple - 바이트 수를 IEC 단위 문자열로 간단 변환(소수 2자리 고정).
 *
 * @buf:     결과를 쓸 호출자 소유 버퍼. NULL 금지.
 * @bufsize: buf 크기. snprintf 가 안전히 절단 처리.
 * @bytes:   변환할 바이트 수(64비트 정수).
 * @return:  buf 자체(편의상).
 *
 * 동작 단계:
 *   1) buf[0]='\0' — 안전 초기화.
 *   2) size 를 1024 로 나누며 단위 레벨 증가(IEC 접두사 배열 길이 한계까지).
 *   3) snprintf 로 "%.2f %sB" 포맷으로 결과 생성.
 *
 * 실행 컨텍스트: 어디서나. 상태 없음 — MT 안전.
 *
 * 호출 체인: 각종 로그 출력 경로 → [bytes2str_simple] → snprintf.
 */
const char *bytes2str_simple(char *buf, size_t bufsize, uint64_t bytes)
{
	/* [한국어] 현재 접두사 인덱스(0: 단위 없음, 1: Ki, 2: Mi, ...) */
	int unit = 0;
	/* [한국어] 축약 중인 값 — double 로 승급하여 소수 표현 확보 */
	double size = (double)bytes;

	/* [한국어] 안전 초기화: bufsize==0 같은 비정상 입력에서도 NULL 종결 보장 */
	buf[0] = '\0';

	/* [한국어] 1024 단위 축약 루프 — 배열 끝(Ei)에 도달하거나 1024 미만이 되면 종료 */
	while (size >= 1024.0 && unit < FIO_ARRAY_SIZE(iecstr) - 1) {
		size /= 1024.0;
		unit++;
	}

	/* [한국어] 최종 포맷 "X.XX [Ki/Mi/...]B". snprintf 는 bufsize 제한을 자동 처리 */
	snprintf(buf, bufsize, "%.2f %sB", size, iecstr[unit]);

	return buf;
}


/**
 * num2str() - Cheesy number->string conversion, complete with carry rounding error.
 * @num: quantity (e.g., number of blocks, bytes or bits)
 * @maxlen: max number of digits in the output string (not counting prefix and units, but counting .)
 * @base: multiplier for num (e.g., if num represents Ki, use 1024)
 * @pow2: select unit prefix - 0=power-of-10 decimal SI, nonzero=power-of-2 binary IEC
 * @units: select units - N2S_* constants defined in num2str.h
 * @returns a malloc'd buffer containing "number[<unit prefix>][<units>]"
 */
/*
 * [한국어]
 * num2str - 큰 정수를 사람 친화적 접미사 문자열로 변환(소수/반올림/자리올림 포함).
 *
 * @num:    변환할 양(블록/바이트/비트 등).
 * @maxlen: 결과 숫자 부분의 최대 자릿수(소수점 포함, 접두사/단위 제외).
 *          예: maxlen=4 이고 num=1234567 → "1.23M" 처럼 축약.
 * @base:   num 의 스케일. num 자체가 이미 Ki 단위라면 base=1024 로 주면 됨.
 *          base>1 이면 thousand 씩 나누며 post_index 증가 — 사전 정규화.
 * @pow2:   0=SI(k/M/G/T/P/E, 1000 기반), 비0=IEC(Ki/Mi/..., 1024 기반).
 * @units:  N2S_* 로 지정되는 접미사(B, bit, /s, B/s, bit/s). N2S_BIT/BITPERSEC 는
 *          내부에서 num *= 8 로 자동 비트 환산.
 * @return: asprintf 로 malloc 된 문자열(호출자가 free 책임). 실패 시 NULL.
 *
 * 동작 단계:
 *   1) 컴파일 타임 sistr/iecstr 크기 일치 assert, units 인덱스 범위 assert.
 *   2) pow2 플래그로 unitprefix 테이블 선택.
 *   3) base>1 이면 thousand 로 반복 나눠 초기 post_index 결정(사전 정규화).
 *   4) units 가 BIT/BITPERSEC 면 num*=8 로 비트 단위 환산.
 *   5) num 을 문자열로 찍어 길이가 maxlen 이하가 될 때까지 thousand 로 나눔.
 *      나눌 때 modulo 를 보관해 소수 자리 생성에 사용. carry 는 반올림용.
 *   6) 나눗셈 결과와 modulo 로 "정수[.소수]<접두사><단위>" 조립.
 *      소수 자리를 기록할 공간이 부족하면 정수만 (반올림 적용 후) 출력.
 *      sprintf 의 %.*f 가 "1.000..." 을 반환하면 num 을 증가(자리올림).
 *
 * 실행 컨텍스트: 출력 경로. 반환값이 malloc 이므로 호출자 free 책임.
 *
 * 호출 체인: stat.c 등 → [num2str] → sprintf/asprintf.
 *
 * 에러 처리: asprintf 실패 시 NULL 반환. 호출자는 NULL 검사 필수.
 */
char *num2str(uint64_t num, int maxlen, int base, int pow2, enum n2s_unit units)
{
	/* [한국어] SI 10진 접두사 — 1000 의 거듭제곱(k=10^3, M=10^6, ...).
	 * iecstr 와 크기가 같아야 함(아래 compiletime_assert 가 검증) */
	const char *sistr[] = { "", "k", "M", "G", "T", "P", "E" };
	/* [한국어] pow2 플래그에 따라 SI 또는 IEC 테이블 가리킴 */
	const char **unitprefix;
	/* [한국어] 각 units 값에 대응하는 접미사 문자열. 인덱스드 초기화로 오매핑 방지 */
	static const char *const unitstr[] = {
		[N2S_NONE]	= "",
		[N2S_PERSEC]	= "/s",
		[N2S_BYTE]	= "B",
		[N2S_BIT]	= "bit",
		[N2S_BYTEPERSEC]= "B/s",
		[N2S_BITPERSEC]	= "bit/s"
	};
	/* [한국어] 나눗셈 단위 — pow2 이면 1024(IEC), 아니면 1000(SI) */
	const unsigned int thousand = pow2 ? 1024 : 1000;
	/* [한국어] 각 축약 단계에서의 나머지 — 소수 자릿수 생성의 원천 */
	unsigned int modulo;
	/* [한국어] post_index: 접두사 테이블 인덱스. carry: 소수 반올림 → 정수 자리올림 플래그 */
	int post_index, carry = 0;
	/* [한국어] 임시 변환 버퍼(정수 자릿수 계산 + 소수 포맷) */
	char tmp[32];
	/* [한국어] asprintf 가 할당한 최종 결과 버퍼 포인터 */
	char *buf;

	/* [한국어] 빌드 타임 체크 — sistr 와 iecstr 의 크기가 반드시 같아야 함(인덱스 공유) */
	compiletime_assert(sizeof(sistr) == sizeof(iecstr), "unit prefix arrays must be identical sizes");
	/* [한국어] units 인덱스가 unitstr 테이블 범위 내인지 검증(0 이상은 enum 으로 강제) */
	assert(units < FIO_ARRAY_SIZE(unitstr));

	/* [한국어] pow2 설정에 따라 단위 접두사 테이블 선택 */
	if (pow2)
		unitprefix = iecstr;
	else
		unitprefix = sistr;

	/* [한국어] base 가 1보다 크면 num 이 이미 어떤 승수에 스케일된 상태.
	 * thousand 로 나누며 post_index 증가 → 초기 접두사 위치 결정 */
	for (post_index = 0; base > 1; post_index++)
		base /= thousand;

	/* [한국어] units 처리: BIT/BITPERSEC 은 num 을 8 배하여 비트로 환산.
	 * 나머지는 그대로 사용 */
	switch (units) {
	case N2S_NONE:
		break;
	case N2S_PERSEC:
		break;
	case N2S_BYTE:
		break;
	case N2S_BIT:
		num *= 8;
		break;
	case N2S_BYTEPERSEC:
		break;
	case N2S_BITPERSEC:
		num *= 8;
		break;
	}

	/*
	 * Divide by K/Ki until string length of num <= maxlen.
	 */
	/* [한국어] modulo=-1U 는 "한 번도 나눠지지 않았음" 표시 — 아래 분기에서 구분. */
	modulo = -1U;
	/* [한국어] post_index 가 접두사 테이블 상한에 닿을 때까지 축약 시도 */
	while (post_index < FIO_ARRAY_SIZE(sistr)) {
		/* [한국어] 현재 num 의 자릿수 파악 */
		sprintf(tmp, "%llu", (unsigned long long) num);
		/* [한국어] 자릿수가 maxlen 이하가 되면 축약 종료 */
		if (strlen(tmp) <= maxlen)
			break;

		/* [한국어] 아직 너무 큼 — thousand 로 나누며 다음 접두사로 이동 */
		modulo = num % thousand;      /* [한국어] 소수 자리 생성용 나머지 보관 */
		num /= thousand;
		/* [한국어] modulo 가 thousand/2 이상이면 반올림 보정 (자리올림 후보) */
		carry = modulo >= thousand / 2;
		post_index++;
	}

	/* [한국어] 축약을 너무 많이 해 접두사 범위를 넘은 경우 0 으로 복구(일어나지 않도록 설계) */
	if (post_index >= FIO_ARRAY_SIZE(sistr))
		post_index = 0;

	/*
	 * If no modulo, then we're done.
	 */
	/* [한국어] 한 번도 나눠지지 않았다면 — 즉, 축약 없이 num 그대로면 — 바로 done 으로 */
	if (modulo == -1U) {
done:
		/* [한국어] 최종 포맷: "%llu<prefix><unit>". asprintf 실패 시 buf=NULL */
		if (asprintf(&buf, "%llu%s%s", (unsigned long long) num,
			     unitprefix[post_index], unitstr[units]) < 0)
			buf = NULL;
		return buf;
	}

	/*
	 * If no room for decimals, then we're done.
	 */
	/* [한국어] 축약된 num 자릿수 재측정 후, 소수 자리 놓을 공간(1자 이상)이 없는지 확인 */
	sprintf(tmp, "%llu", (unsigned long long) num);
	if ((int)(maxlen - strlen(tmp)) <= 1) {
		/* [한국어] 반올림 보정 — carry 면 정수 부분 +1 */
		if (carry)
			num++;
		/* [한국어] 정수만 출력하고 종료 */
		goto done;
	}

	/*
	 * Fill in everything and return the result.
	 */
	/* [한국어] 소수 자릿수는 "maxlen - 정수 자리 - 소수점" 만큼 */
	assert(maxlen - strlen(tmp) - 1 > 0);
	assert(modulo < thousand);
	/* [한국어] modulo/thousand 로 [0,1) 분수 생성 후 %.*f 로 소수 자리 포맷.
	 * tmp 는 "0.xxxx" 또는 "1.0000" 형태 */
	sprintf(tmp, "%.*f", (int)(maxlen - strlen(tmp) - 1),
		(double)modulo / (double)thousand);

	/* [한국어] 반올림이 "1.0000" 을 만들면 정수 자리에 +1 필요 (자리올림) */
	if (tmp[0] == '1')
		num++;

	/* [한국어] "정수.소수<prefix><unit>" 조립. &tmp[2] 는 "0." 를 건너뛴 소수 부분 */
	if (asprintf(&buf, "%llu.%s%s%s", (unsigned long long) num, &tmp[2],
		     unitprefix[post_index], unitstr[units]) < 0)
		buf = NULL;
	return buf;
}
