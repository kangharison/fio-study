/*
 * [한국어] hash.h - 해시 함수 구현 (Linux 커널 기반)
 *
 * 두 가지 해시 알고리즘을 제공한다:
 *   1) 황금비(Golden Ratio) 해시: hash_long(), hash_ptr()
 *      - 정수나 포인터를 빠르게 해시. 해시 테이블 인덱싱에 적합
 *      - 황금비 상수를 곱하여 비트를 고르게 분산시킴
 *   2) Jenkins 해시 (jhash): jhash()
 *      - 임의 길이 바이트 배열을 해싱. Bob Jenkins의 lookup3 기반
 *      - 해시 테이블, 체크섬 등에 사용
 
 * === 파일의 역할 ===
 * 황금비 해시(hash_long)와 Jenkins 해시(jhash) 두 가지 알고리즘을 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * filehash.c, client.c 등에서 해시 테이블 인덱싱에 사용.
 *
 * === 타 모듈과의 연결 ===
 * - filehash.c: 파일 해시 테이블 인덱싱
 * - client.c: 클라이언트 해시 테이블
 *
 * === 주요 함수/구조체 요약 ===
 * - hash_long()/hash_ptr(): 황금비 해시
 * - jhash(): Jenkins 해시 (바이트 배열)
 */
#ifndef _LINUX_HASH_H
#define _LINUX_HASH_H

#include <inttypes.h>
#include "arch/arch.h"
#include "compiler/compiler.h"

/* Fast hashing routine for a long.
   (C) 2002 William Lee Irwin III, IBM */

/*
 * Although a random odd number will do, it turns out that the golden
 * ratio phi = (sqrt(5)-1)/2, or its negative, has particularly nice
 * properties.
 *
 * These are the negative, (1 - phi) = (phi^2) = (3 - sqrt(5))/2.
 * (See Knuth vol 3, section 6.4, exercise 9.)
 */
/* [한국어] 황금비 상수 - 해시 분산을 위한 매직 넘버 (Knuth vol.3, 6.4절 참조) */
#define GOLDEN_RATIO_32 0x61C88647
#define GOLDEN_RATIO_64 0x61C8864680B583EBull

/*
 * [한국어] __hash_long - 64비트 정수를 황금비 곱셈으로 해싱
 *
 * 64비트 플랫폼: GOLDEN_RATIO_64를 곱함 (단일 곱셈, 매우 빠름)
 * 32비트 플랫폼: 시프트와 덧셈/뺄셈으로 동등한 연산 수행
 *                (gcc가 64비트 곱셈을 최적화하지 못하므로 수동 전개)
 */
static inline unsigned long __hash_long(uint64_t val)
{
	uint64_t hash = val;

#if BITS_PER_LONG == 64
	hash *= GOLDEN_RATIO_64;
#else
	/*  Sigh, gcc can't optimise this alone like it does for 32 bits. */
	uint64_t n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;
#endif

	return hash;
}

/*
 * [한국어] hash_long - 정수를 지정된 비트 수의 해시값으로 변환
 *
 * 상위 비트가 더 랜덤하므로, 상위 bits개 비트를 취함.
 * 예: hash_long(val, 10) -> 0~1023 범위의 해시값 반환
 */
static inline unsigned long hash_long(unsigned long val, unsigned int bits)
{
	/* High bits are more random, so use them. */
	return __hash_long(val) >> (BITS_PER_LONG - bits);
}

/* [한국어] 64비트 정수를 황금비 곱셈으로 해싱 (비트 잘라내기 없음) */
static inline uint64_t __hash_u64(uint64_t val)
{
	return val * GOLDEN_RATIO_64;
}

/* [한국어] 포인터를 지정된 비트 수의 해시값으로 변환 */
static inline unsigned long hash_ptr(void *ptr, unsigned int bits)
{
	return hash_long((uintptr_t)ptr, bits);
}

/*
 * Bob Jenkins jhash
 */
/* [한국어] Jenkins 해시 초기값 - GOLDEN_RATIO_32 사용 */

#define JHASH_INITVAL	GOLDEN_RATIO_32

/* [한국어] 32비트 왼쪽 회전(rotate left) */
static inline uint32_t rol32(uint32_t word, uint32_t shift)
{
	return (word << shift) | (word >> (32 - shift));
}

/* [한국어] jhash 내부 혼합 매크로 - 3개의 32비트 값을 가역적으로 혼합 */
/* __jhash_mix -- mix 3 32-bit values reversibly. */
#define __jhash_mix(a, b, c)			\
{						\
	a -= c;  a ^= rol32(c, 4);  c += b;	\
	b -= a;  b ^= rol32(a, 6);  a += c;	\
	c -= b;  c ^= rol32(b, 8);  b += a;	\
	a -= c;  a ^= rol32(c, 16); c += b;	\
	b -= a;  b ^= rol32(a, 19); a += c;	\
	c -= b;  c ^= rol32(b, 4);  b += a;	\
}

/* [한국어] jhash 최종 혼합 매크로 - 3개의 32비트 값(a,b,c)을 c로 최종 혼합 */
/* __jhash_final - final mixing of 3 32-bit values (a,b,c) into c */
#define __jhash_final(a, b, c)			\
{						\
	c ^= b; c -= rol32(b, 14);		\
	a ^= c; a -= rol32(c, 11);		\
	b ^= a; b -= rol32(a, 25);		\
	c ^= b; c -= rol32(b, 16);		\
	a ^= c; a -= rol32(c, 4);		\
	b ^= a; b -= rol32(a, 14);		\
	c ^= b; c -= rol32(b, 24);		\
}

/*
 * [한국어] jhash - Bob Jenkins의 해시 함수 (lookup3 기반)
 *
 * @key:      해싱할 데이터의 포인터
 * @length:   데이터 길이 (바이트)
 * @initval:  초기 해시값 (해시 테이블 시드로 사용)
 *
 * 12바이트씩 처리하며 __jhash_mix로 혼합하고,
 * 남은 바이트는 __jhash_final로 최종 혼합한다.
 * 반환값: 32비트 해시값
 */
static inline uint32_t jhash(const void *key, uint32_t length, uint32_t initval)
{
	const uint8_t *k = key;
	uint32_t a, b, c;

	/* Set up the internal state */
	/* 내부 상태 초기화 */
	a = b = c = JHASH_INITVAL + length + initval;

	/* All but the last block: affect some 32 bits of (a,b,c) */
	/* 마지막 블록을 제외한 모든 12바이트 블록 처리 */
	while (length > 12) {
		a += *k;
		b += *(k + 4);
		c += *(k + 8);
		__jhash_mix(a, b, c);
		length -= 12;
		k += 12;
	}

	/* Last block: affect all 32 bits of (c) */
	/* All the case statements fall through */
	/* 마지막 블록: c의 모든 32비트에 영향 (fall-through로 처리) */
	switch (length) {
	case 12: c += (uint32_t) k[11] << 24;	fio_fallthrough;
	case 11: c += (uint32_t) k[10] << 16;	fio_fallthrough;
	case 10: c += (uint32_t) k[9] << 8;	fio_fallthrough;
	case 9:  c += k[8];			fio_fallthrough;
	case 8:  b += (uint32_t) k[7] << 24;	fio_fallthrough;
	case 7:  b += (uint32_t) k[6] << 16;	fio_fallthrough;
	case 6:  b += (uint32_t) k[5] << 8;	fio_fallthrough;
	case 5:  b += k[4];			fio_fallthrough;
	case 4:  a += (uint32_t) k[3] << 24;	fio_fallthrough;
	case 3:  a += (uint32_t) k[2] << 16;	fio_fallthrough;
	case 2:  a += (uint32_t) k[1] << 8;	fio_fallthrough;
	case 1:  a += k[0];
		 __jhash_final(a, b, c);
		 fio_fallthrough;
	case 0: /* Nothing left to add */
		break;
	}

	return c;
}

#endif /* _LINUX_HASH_H */
