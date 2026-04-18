/*
 * [한국어 설명] IEEE 754 부동소수점 직렬화/역직렬화 (ieee754.c)
 *
 * === 파일의 역할 ===
 * 임의의 long double 값을 IEEE 754 이진 표현의 비트 패턴으로 직접 "조립(pack)" 하고
 * 반대로 비트 패턴에서 값을 "해체(unpack)" 하는 함수 두 개를 제공한다. 단순히
 * union 으로 float/double 을 uint64_t 에 오버레이하면 호스트 CPU 의 부동소수점
 * 표현이 이미 IEEE 754 라는 전제에서만 옳은데, fio 의 클라이언트-서버 프로토콜은
 * 서로 다른 아키텍처/엔디안 조합에서도 동일한 숫자 의미를 유지해야 하므로,
 * 값 자체를 "부호 1비트 + 편향 지수 + 정규화 가수" 로 수동 분해하여 완전히
 * 이식 가능한 비트 패턴으로 만든다. 엔디안 변환은 별도(상위 계층)에서 cpu_to_le64
 * 등으로 처리되지만, 부동소수점 포맷 비트 레이아웃 자체는 본 파일이 확정한다.
 *
 * 구현 출처: Beej's Guide to Network Programming 의 퍼블릭 도메인 예제 코드를 기반.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 서버 모드(fio --server) 와 클라이언트 모드 간 stats/latency percentile/bandwidth
 * 샘플 등의 float 데이터를 교환할 때 server.c 의 convert_io_stat() 등에서
 * fio_double_to_uint64/fio_uint64_to_double 매크로(ieee754.h)가 본 파일의 두 함수를
 * 호출한다. 이 비트 변환으로 변환된 uint64_t 는 이후 cpu_to_le64 로 네트워크 바이트
 * 순서로 직렬화되어 소켓에 전송된다.
 *
 * === 타 모듈과의 연결 ===
 * - ieee754.h:   본 파일이 제공하는 pack754/unpack754 프로토타입 및 편의 매크로
 *                fio_double_to_uint64(d)/fio_uint64_to_double(u) 제공.
 * - server.c / client.c / stat.c: 네트워크/IPC 경로의 float 직렬화 호출자.
 * - 독립성: 표준 라이브러리만 사용 — 외부 의존 최소.
 *
 * === 주요 함수/구조체 요약 ===
 * - pack754(f, bits, expbits):
 *     부동소수점 f 를 (bits, expbits) 조합의 IEEE 754 비트 패턴(uint64_t)으로 조립.
 *     예: pack754(x, 64, 11) → IEEE 754 double, pack754(x, 32, 8) → float.
 *     0.0 특수 케이스 조기 처리.
 * - unpack754(i, bits, expbits):
 *     비트 패턴 i 를 long double 로 복원.
 * 자체 구조체 없음.
 *
 * === IEEE 754 레이아웃 요약 (double 기준 bits=64, expbits=11) ===
 *   비트 63:     부호(sign) — 1=음수, 0=양수/0
 *   비트 62..52: 지수(11비트) — 편향 1023 을 뺀 값이 실제 지수.
 *   비트 51..0:  가수(52비트) — 정규화 가정으로 "1.xxx" 의 xxx 부분만 저장.
 */
/*
 * Shamelessly lifted from Beej's Guide to Network Programming, found here:
 *
 * http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html#serialization
 *
 * Below code was granted to the public domain.
 */
#include "ieee754.h"            /* [한국어] pack754/unpack754 선언 + 편의 매크로 */

/*
 * [한국어]
 * pack754 - 부동소수점 값을 IEEE 754 형식의 정수 비트 패턴으로 직렬화.
 *
 * @f:       변환할 부동소수점 값(long double 로 받아 float/double 양쪽 호환).
 * @bits:    전체 비트 수. 64(double) 또는 32(float).
 * @expbits: 지수 비트 수. 11(double) 또는 8(float). 가수비트 = bits-expbits-1.
 * @return:  IEEE 754 비트 패턴이 담긴 uint64_t. 상위 미사용 비트는 0.
 *
 * 동작 단계:
 *   1) f==0.0 특수: 모든 비트 0 인 "positive zero" 비트 패턴을 반환(부호/지수/가수 모두 0).
 *      (이 구현은 -0.0 을 별도 구분하지 않음 — 비트 레벨에서 +0 과 동일 처리됨)
 *   2) 부호 비트 결정: f<0 이면 sign=1, fnorm=|f|; 그 외 sign=0, fnorm=f.
 *   3) 정규화(normalize): 1.0 <= fnorm < 2.0 이 되도록 2 로 곱하거나 나누며 지수 shift 누적.
 *   4) 가수 비트열 생성: fnorm-=1.0 (숨은 선행 1 제거) 후 2^significandbits 를 곱하여 정수화.
 *   5) 편향 지수 계산: exp = shift + (2^(expbits-1) - 1). double 은 bias=1023, float 은 127.
 *   6) 조립: (sign << bits-1) | (exp << significandbits) | significand.
 *
 * 실행 컨텍스트: 순수 수치 계산 — 상태/MT 이슈 없음.
 *
 * 호출 체인: fio_double_to_uint64 매크로(ieee754.h) → [pack754] → server.c 직렬화.
 *
 * 에러 처리: subnormal/NaN/Inf 는 이 단순 구현이 완전히 정확히 재현하지 못할 수 있으나,
 *           fio 의 통계값 범위에서는 거의 문제 없는 수준.
 */
uint64_t pack754(long double f, unsigned bits, unsigned expbits)
{
	/* [한국어] 정규화된 값(1.0 <= fnorm < 2.0) 을 보관 */
	long double fnorm;
	/* [한국어] 지수 계산 중 누적되는 2의 거듭제곱 이동량 */
	int shift;
	/* [한국어] 최종 조립에 쓸 세 구성요소: 부호(0/1), 편향 지수, 가수 정수화 결과 */
	long long sign, exp, significand;
	/* [한국어] 가수 비트 수: 전체 - 지수 - 부호 1비트 */
	unsigned significandbits = bits - expbits - 1; // -1 for sign bit

	// get this special case out of the way
	/* [한국어] 0.0 특수: 모든 필드 0 인 positive zero 패턴 반환. -0.0 도 비트 동일 처리 */
	if (f == 0.0)
		return 0;

	// check sign and begin normalization
	/* [한국어] 부호 추출 및 절댓값으로 변환 — 이후 정규화는 양수 전제 */
	if (f < 0) {
		sign = 1;         /* [한국어] 음수 → 부호 비트 1 */
		fnorm = -f;       /* [한국어] 절댓값 저장 */
	} else {
		sign = 0;         /* [한국어] 양수/0 → 부호 비트 0 */
		fnorm = f;
	}

	// get the normalized form of f and track the exponent
	/* [한국어] fnorm 이 [1.0, 2.0) 범위에 들어올 때까지 2 를 곱/나누며 shift 카운트 */
	shift = 0;
	/* [한국어] 2.0 이상이면 절반씩 내리며 shift 증가(지수 양의 방향) */
	while (fnorm >= 2.0) {
		fnorm /= 2.0;
		shift++;
	}
	/* [한국어] 1.0 미만이면 두 배씩 올리며 shift 감소(지수 음의 방향) */
	while (fnorm < 1.0) {
		fnorm *= 2.0;
		shift--;
	}
	/* [한국어] IEEE 754 정규화 수: 정규화 후 선행 1 은 "숨은 비트" 로 저장 생략 —
	 * 가수에는 소수 부분(1.xxx 의 xxx) 만 담기므로 1.0 을 뺀다 */
	fnorm = fnorm - 1.0;

	// calculate the binary form (non-float) of the significand data
	/* [한국어] 가수 이진 정수화: fnorm * 2^significandbits 근사 반올림.
	 * 끝의 0.5f 는 반올림 보정(트런케이션 방지) */
	significand = fnorm * ((1LL << significandbits) + 0.5f);

	// get the biased exponent
	/* [한국어] 실제 지수(shift) 에 편향(bias) 을 더해 비트열에 저장할 값 산출.
	 * bias = 2^(expbits-1) - 1. double 은 1023, float 은 127 */
	exp = shift + ((1 << (expbits - 1)) - 1); // shift + bias

	// return the final answer
	/* [한국어] 최종 비트 패턴 조립:
	 *   bit bits-1        = sign
	 *   bits bits-2..significandbits = exp (expbits 개)
	 *   bits significandbits-1..0 = significand */
	return (sign << (bits - 1)) | (exp << (bits-expbits - 1)) | significand;
}

/*
 * [한국어]
 * unpack754 - IEEE 754 비트 패턴 정수를 long double 로 역직렬화.
 *
 * @i:       IEEE 754 비트 패턴(uint64_t).
 * @bits:    전체 비트 수(64 또는 32).
 * @expbits: 지수 비트 수(11 또는 8).
 * @return:  복원된 long double 값. i==0 이면 0.0.
 *
 * 동작 단계:
 *   1) i==0 특수 — positive zero 그대로 0.0 반환.
 *   2) 가수 필드 추출 후 정규화 값 1.xxx 로 복원(숨은 선행 1 다시 더함).
 *   3) 지수 필드 추출 → 편향 빼기 → shift.
 *   4) shift 만큼 2 를 곱하거나 나눠서 실제 크기 복원.
 *   5) 부호 비트로 결과에 ±1 곱.
 *
 * 실행 컨텍스트: 순수 계산, MT-안전.
 *
 * 호출 체인: fio_uint64_to_double 매크로 → [unpack754] → 클라이언트/서버 역직렬화.
 *
 * 에러 처리: subnormal/NaN/Inf 근사 한계 — pack754 와 동일. 통계 범위 내에서만 사용.
 */
long double unpack754(uint64_t i, unsigned bits, unsigned expbits)
{
	/* [한국어] 복원된 값의 누적 저장 */
	long double result;
	/* [한국어] 지수 복원에 사용할 shift 값(편향 제거 후) */
	long long shift;
	/* [한국어] 편향 상수 — IEEE 754 는 지수를 비음수 비트 필드로 저장하기 위해 bias 를 더해 둠 */
	unsigned bias;
	/* [한국어] 가수 비트 수 — pack754 와 동일 계산 */
	unsigned significandbits = bits - expbits - 1; // -1 for sign bit

	/* [한국어] 0 비트 패턴은 0.0 으로 직접 반환 */
	if (i == 0)
		return 0.0;

	// pull the significand
	/* [한국어] 가수 필드 추출: 하위 significandbits 비트 마스크 */
	result = (i & ((1LL << significandbits) - 1)); // mask
	/* [한국어] 정수 값 → 분수 값으로 환산: 2^significandbits 로 나누면 [0,1) 분수 */
	result /= (1LL << significandbits); // convert back to float
	/* [한국어] 숨은 선행 1 을 다시 더해 정규화 표현 1.xxx 복원 */
	result += 1.0f; // add the one back on

	// deal with the exponent
	/* [한국어] 편향 값 계산 (double: 1023, float: 127) */
	bias = (1 << (expbits - 1)) - 1;
	/* [한국어] 지수 비트 추출 후 편향 제거 → 실제 shift 량 */
	shift = ((i >> significandbits) & ((1LL << expbits) - 1)) - bias;
	/* [한국어] 양의 지수 — 2 를 shift 번 곱 */
	while (shift > 0) {
		result *= 2.0;
		shift--;
	}
	/* [한국어] 음의 지수 — 2 로 shift 번 나눔 */
	while (shift < 0) {
		result /= 2.0;
		shift++;
	}

	// sign it
	/* [한국어] 최상위 비트로 부호 결정: 1=음수 → -1 곱, 0=양수 → +1 곱 */
	result *= (i >> (bits - 1)) & 1 ? -1.0 : 1.0;

	return result;
}
