/*
 * [한국어 설명] IEEE 754 부동소수점 pack/unpack (ieee754.c)
 *
 * === 파일의 역할 ===
 * IEEE 754 형식의 부동소수점 값을 정수로 직렬화(pack)하거나 정수에서 부동소수점으로
 * 역직렬화(unpack)하는 함수를 제공한다. 플랫폼 독립적인 방식으로 부동소수점 데이터를
 * 바이트 스트림으로 변환할 수 있게 해준다.
 *
 * === fio에서의 사용 ===
 * fio의 클라이언트-서버 프로토콜에서 네트워크를 통해 부동소수점 값을 전송할 때
 * 이식 가능한 직렬화를 위해 사용된다.
 */
/*
 * Shamelessly lifted from Beej's Guide to Network Programming, found here:
 *
 * http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html#serialization
 *
 * Below code was granted to the public domain.
 */
#include "ieee754.h"

uint64_t pack754(long double f, unsigned bits, unsigned expbits)
{
	long double fnorm;
	int shift;
	long long sign, exp, significand;
	unsigned significandbits = bits - expbits - 1; // -1 for sign bit

	// get this special case out of the way
	if (f == 0.0)
		return 0;

	// check sign and begin normalization
	if (f < 0) {
		sign = 1;
		fnorm = -f;
	} else {
		sign = 0;
		fnorm = f;
	}

	// get the normalized form of f and track the exponent
	shift = 0;
	while (fnorm >= 2.0) {
		fnorm /= 2.0;
		shift++;
	}
	while (fnorm < 1.0) {
		fnorm *= 2.0;
		shift--;
	}
	fnorm = fnorm - 1.0;

	// calculate the binary form (non-float) of the significand data
	significand = fnorm * ((1LL << significandbits) + 0.5f);

	// get the biased exponent
	exp = shift + ((1 << (expbits - 1)) - 1); // shift + bias

	// return the final answer
	return (sign << (bits - 1)) | (exp << (bits-expbits - 1)) | significand;
}

long double unpack754(uint64_t i, unsigned bits, unsigned expbits)
{
	long double result;
	long long shift;
	unsigned bias;
	unsigned significandbits = bits - expbits - 1; // -1 for sign bit

	if (i == 0)
		return 0.0;

	// pull the significand
	result = (i & ((1LL << significandbits) - 1)); // mask
	result /= (1LL << significandbits); // convert back to float
	result += 1.0f; // add the one back on

	// deal with the exponent
	bias = (1 << (expbits - 1)) - 1;
	shift = ((i >> significandbits) & ((1LL << expbits) - 1)) - bias;
	while (shift > 0) {
		result *= 2.0;
		shift--;
	}
	while (shift < 0) {
		result /= 2.0;
		shift++;
	}

	// sign it
	result *= (i >> (bits - 1)) & 1 ? -1.0 : 1.0;

	return result;
}
