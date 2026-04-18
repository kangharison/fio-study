/*
 * Cryptographic API.
 *
 * SHA-256, as specified in
 * http://csrc.nist.gov/cryptval/shs/sha256-384-512.pdf
 *
 * SHA-256 code by Jean-Luc Cooke <jlcooke@certainkey.com>.
 *
 * Copyright (c) Jean-Luc Cooke <jlcooke@certainkey.com>
 * Copyright (c) Andrew McDonald <andrew@mcdonald.org.uk>
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 */
/*
 * [한국어 설명] SHA-256 해시 구현 (sha256.c)
 *
 * === 파일의 역할 ===
 * FIPS 180-4 SHA-256 을 구현한다. 512비트(64B) 입력 블록을 64 스텝으로 처리해
 * 256비트 다이제스트(8 × 32비트 state)를 생성하는 Merkle-Damgård 해시.
 * 32비트 산술을 사용하므로 32비트 CPU 에서도 빠르다. Linux 커널 crypto/sha256.c
 * 기반. Ch/Maj/Σ0/Σ1/σ0/σ1 보조 함수, 64개 라운드 상수(inline), 16→64 메시지 스케줄
 * 유도로 구성.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 의 VERIFY_SHA256 경로.
 * 쓰기: fill_sha256(verify.c) → fio_sha256_init → update → final → state[] 직렬화.
 * 호출 체인:
 *   verify.c::fill_sha256/verify_io_u_sha256 → fio_sha256_* → sha256_transform
 *   crc/test.c::t_sha256 → 동일
 *
 * === 타 모듈과의 연결 ===
 * - sha256.h: fio_sha256_ctx(state[8], count, buf[64]/buf[] — 호출자가 조정) 정의.
 * - lib/bswap.h: __be32_to_cpu — 빅엔디안 입력을 CPU 순서로.
 * - verify.c / crc/test.c: 호출자.
 * 동기화: 컨텍스트 단위 독립 — 락 불요.
 *
 * === 주요 함수 요약 ===
 * - Ch/Maj/ror32: 비선형·회전 보조 함수.
 * - e0/e1(Σ0/Σ1), s0/s1(σ0/σ1): 회전 XOR 매크로.
 * - H0~H7: 초기 해시값.
 * - LOAD_OP/BLEND_OP: 메시지 스케줄 로드/믹스.
 * - sha256_transform: 블록 1개 64 스텝 변환.
 * - fio_sha256_init/update/final: 공용 API.
 */
#include <string.h>
/* [한국어] memcpy: 블록 누적·버퍼 조작. */

#include "../lib/bswap.h"
/* [한국어] __be32_to_cpu 매크로 공급. */
#include "sha256.h"
/* [한국어] sha256.h: fio_sha256_ctx 와 공용 API. */

/* [한국어] 다이제스트 바이트 수(=32)와 HMAC 블록 크기(=64) 상수. */
#define SHA256_DIGEST_SIZE	32
#define SHA256_HMAC_BLOCK_SIZE	64

/*
 * [한국어] Ch - SHA-2 의 "choose" 비선형 함수. z^(x&(y^z)) 는 (x&y)^(~x&z) 와 동치. */
static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z)
{
	return z ^ (x & (y ^ z));
}

/*
 * [한국어] Maj - SHA-2 의 "majority" 비선형 함수. (x&y) | (z&(x|y)) = 다수결. */
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z)
{
	return (x & y) | (z & (x | y));
}

/* [한국어] SHA-256 의 Σ/σ 회전 XOR — 인자는 모두 32비트, FIPS 180-4 §4.1.2. */
#define e0(x)       (ror32(x, 2) ^ ror32(x,13) ^ ror32(x,22))
#define e1(x)       (ror32(x, 6) ^ ror32(x,11) ^ ror32(x,25))
#define s0(x)       (ror32(x, 7) ^ ror32(x,18) ^ (x >> 3))
#define s1(x)       (ror32(x,17) ^ ror32(x,19) ^ (x >> 10))

/* [한국어] SHA-256 초기값 — 소수 2..19 의 제곱근 분수부 앞 32비트. */
#define H0         0x6a09e667
#define H1         0xbb67ae85
#define H2         0x3c6ef372
#define H3         0xa54ff53a
#define H4         0x510e527f
#define H5         0x9b05688c
#define H6         0x1f83d9ab
#define H7         0x5be0cd19

/*
 * [한국어] ror32 - 32비트 우회전. word>>shift | word<<(32-shift). */
static inline uint32_t ror32(uint32_t word, unsigned int shift)
{
	 return (word >> shift) | (word << (32 - shift));
}

/*
 * [한국어] LOAD_OP - 입력 블록 I 번째 32비트 워드를 빅엔디안으로 읽어 W[I] 에 저장.
 */
static inline void LOAD_OP(int I, uint32_t *W, const uint8_t *input)
{
	W[I] = __be32_to_cpu(((uint32_t *)(input))[I]);
}

/*
 * [한국어] BLEND_OP - 메시지 스케줄 I>=16 슬롯 유도:
 *   W[I] = σ1(W[I-2]) + W[I-7] + σ0(W[I-15]) + W[I-16]. */
static inline void BLEND_OP(int I, uint32_t *W)
{
	W[I] = s1(W[I-2]) + W[I-7] + s0(W[I-15]) + W[I-16];
}

/*
 * [한국어]
 * sha256_transform - 512비트 블록 1개 SHA-256 변환
 *
 * @state: 8워드 chaining value (in/out).
 * @input: 64B 빅엔디안 블록.
 *
 * 동작: W[0..15] 입력 로드 → W[16..63] 유도 → a..h 레지스터 적재 →
 * 64 스텝(본 구현은 완전 언롤)의 T1=h+Σ1(e)+Ch(e,f,g)+K[i]+W[i];
 * T2=Σ0(a)+Maj(a,b,c); d+=T1; h=T1+T2; 시퀀스 → state 누적.
 *
 * 호출 체인: fio_sha256_update/final → [sha256_transform]
 */
static void sha256_transform(uint32_t *state, const uint8_t *input)
{
	/* [한국어] 작업 레지스터 a..h 와 임시 T1/T2. */
	uint32_t a, b, c, d, e, f, g, h, t1, t2;
	/* [한국어] 64 워드 메시지 스케줄 — 스택 버퍼. */
	uint32_t W[64];
	/* [한국어] 루프 인덱스. */
	int i;

	/* load the input */
	/* [한국어] 16 워드 직접 로드. */
	for (i = 0; i < 16; i++)
		LOAD_OP(i, W, input);

	/* now blend */
	/* [한국어] 16~63 워드 스케줄 유도. */
	for (i = 16; i < 64; i++)
		BLEND_OP(i, W);

	/* load the state into our registers */
	/* [한국어] chaining value 레지스터 적재. */
	a=state[0];  b=state[1];  c=state[2];  d=state[3];
	e=state[4];  f=state[5];  g=state[6];  h=state[7];

	/* now iterate */
	/* [한국어] 64 스텝을 완전 언롤 — 아래 8줄 한 쌍이 한 스텝에 대응.
	 * 각 스텝: T1 = h + Σ1(e) + Ch(e,f,g) + K[i] + W[i];
	 *          T2 = Σ0(a) + Maj(a,b,c);
	 *          d += T1; h = T1 + T2.
	 * 이후 a..h 가 한 자리씩 순환 — 변수명을 바꿔 그 순환을 표현. */
	t1 = h + e1(e) + Ch(e,f,g) + 0x428a2f98 + W[ 0];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0x71374491 + W[ 1];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0xb5c0fbcf + W[ 2];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0xe9b5dba5 + W[ 3];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0x3956c25b + W[ 4];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0x59f111f1 + W[ 5];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0x923f82a4 + W[ 6];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0xab1c5ed5 + W[ 7];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0xd807aa98 + W[ 8];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0x12835b01 + W[ 9];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0x243185be + W[10];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0x550c7dc3 + W[11];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0x72be5d74 + W[12];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0x80deb1fe + W[13];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0x9bdc06a7 + W[14];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0xc19bf174 + W[15];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0xe49b69c1 + W[16];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0xefbe4786 + W[17];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0x0fc19dc6 + W[18];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0x240ca1cc + W[19];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0x2de92c6f + W[20];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0x4a7484aa + W[21];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0x5cb0a9dc + W[22];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0x76f988da + W[23];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0x983e5152 + W[24];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0xa831c66d + W[25];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0xb00327c8 + W[26];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0xbf597fc7 + W[27];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0xc6e00bf3 + W[28];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0xd5a79147 + W[29];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0x06ca6351 + W[30];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0x14292967 + W[31];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0x27b70a85 + W[32];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0x2e1b2138 + W[33];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0x4d2c6dfc + W[34];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0x53380d13 + W[35];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0x650a7354 + W[36];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0x766a0abb + W[37];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0x81c2c92e + W[38];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0x92722c85 + W[39];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0xa2bfe8a1 + W[40];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0xa81a664b + W[41];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0xc24b8b70 + W[42];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0xc76c51a3 + W[43];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0xd192e819 + W[44];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0xd6990624 + W[45];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0xf40e3585 + W[46];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0x106aa070 + W[47];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0x19a4c116 + W[48];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0x1e376c08 + W[49];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0x2748774c + W[50];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0x34b0bcb5 + W[51];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0x391c0cb3 + W[52];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0x4ed8aa4a + W[53];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0x5b9cca4f + W[54];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0x682e6ff3 + W[55];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	t1 = h + e1(e) + Ch(e,f,g) + 0x748f82ee + W[56];
	t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
	t1 = g + e1(d) + Ch(d,e,f) + 0x78a5636f + W[57];
	t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
	t1 = f + e1(c) + Ch(c,d,e) + 0x84c87814 + W[58];
	t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
	t1 = e + e1(b) + Ch(b,c,d) + 0x8cc70208 + W[59];
	t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
	t1 = d + e1(a) + Ch(a,b,c) + 0x90befffa + W[60];
	t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
	t1 = c + e1(h) + Ch(h,a,b) + 0xa4506ceb + W[61];
	t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
	t1 = b + e1(g) + Ch(g,h,a) + 0xbef9a3f7 + W[62];
	t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
	t1 = a + e1(f) + Ch(f,g,h) + 0xc67178f2 + W[63];
	t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;

	/* [한국어] Merkle-Damgård 누적. */
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;

	/* clear any sensitive info... */
	/* [한국어] 레지스터·메시지 스케줄 지움 — 민감 정보 소거(DCE 주의). */
	a = b = c = d = e = f = g = h = t1 = t2 = 0;
	memset(W, 0, 64 * sizeof(uint32_t));
}

/*
 * [한국어]
 * fio_sha256_init - SHA-256 컨텍스트 초기화(H0~H7 + count=0)
 *
 * 호출 체인: verify.c / test.c → [fio_sha256_init]
 */
void fio_sha256_init(struct fio_sha256_ctx *sctx)
{
	/* [한국어] FIPS 180-4 §5.3.3 의 초기 해시값. */
	sctx->state[0] = H0;
	sctx->state[1] = H1;
	sctx->state[2] = H2;
	sctx->state[3] = H3;
	sctx->state[4] = H4;
	sctx->state[5] = H5;
	sctx->state[6] = H6;
	sctx->state[7] = H7;
	/* [한국어] 누적 바이트 수 초기화 — 64비트 카운터. */
	sctx->count = 0;
}

/*
 * [한국어]
 * fio_sha256_update - SHA-256 입력 누적(64B 블록)
 *
 * 동작: partial(=count%64) 계산 → count 갱신 → (partial + len > 63) 이면 먼저 partial 메꿔
 * 한 블록 flush 후 64B 단위 루프 → 남은 꼬리는 buf 에 저장.
 *
 * 호출 체인: verify.c / test.c / fio_sha256_final → [fio_sha256_update] → sha256_transform
 */
void fio_sha256_update(struct fio_sha256_ctx *sctx, const uint8_t *data,
		       unsigned int len)
{
	/* [한국어] partial: 현 부분 블록 오프셋, done: 현재까지 소비한 data 오프셋(음수 트릭 사용). */
	unsigned int partial, done;
	/* [한국어] 실제 transform 에 투입될 소스 포인터 — 처음엔 data 직접, partial 메운 뒤엔 buf. */
	const uint8_t *src;

	/* [한국어] partial = count % 64. */
	partial = sctx->count & 0x3f;
	/* [한국어] 누적 바이트 카운트 증가. */
	sctx->count += len;
	/* [한국어] 소비 오프셋 초기화. */
	done = 0;
	/* [한국어] 기본은 data 직접. */
	src = data;

	/* [한국어] 합이 64B 이상이면 한 블록 이상 transform 가능. */
	if ((partial + len) > 63) {
		/* [한국어] 이전 블록 부분이 남아있으면 먼저 메꿔 한 블록 완성. */
		if (partial) {
			/* [한국어] done 을 음수로 세팅 — 아래 루프에서 data 기준 offset 이 깔끔해짐. */
			done = -partial;
			/* [한국어] buf 뒷부분에 data 에서 (64 - partial) 바이트 복사. */
			memcpy(sctx->buf + partial, data, done + 64);
			/* [한국어] 첫 블록은 buf 에서 읽어 transform. */
			src = sctx->buf;
		}

		/* [한국어] 64B 단위 transform 루프. */
		do {
			/* [한국어] 한 블록 변환. */
			sha256_transform(sctx->state, src);
			/* [한국어] 64B 소비. */
			done += 64;
			/* [한국어] 다음 블록은 data 에서 직접. */
			src = data + done;
		} while (done + 63 < len);

		/* [한국어] partial 플러시 끝. */
		partial = 0;
	}
	/* [한국어] 남은 꼬리(<64B)를 buf 에 저장 — 다음 호출에서 이어감. */
	memcpy(sctx->buf + partial, src, len - done);
}

/*
 * [한국어]
 * fio_sha256_final - SHA-256 패딩 + 64비트 길이 주입 + state[] 를 buf[0..7] 에 직렬화
 *
 * 패딩(FIPS 180-4 §5.1.1): 0x80 + 0... + 64비트 빅엔디안 비트-길이. 56 mod 64 에 맞춤.
 *
 * 호출 체인: verify.c / test.c → [fio_sha256_final] → fio_sha256_update
 *
 * 주의: 본 구현은 마지막에 state[i] 를 buf[i] 에 uint32_t 그대로 대입(직렬화 아님).
 * 호출자(verify.c / test.c)가 네트워크 바이트 순서로 재해석 필요 시 별도 처리한다.
 */
void fio_sha256_final(struct fio_sha256_ctx *sctx)
{
	/* [한국어] 비트 길이 = count*8 (64비트). */
	uint64_t bits;
	/* [한국어] 현재 오프셋·패딩 길이. */
	unsigned int index, pad_len;
	/* [한국어] 루프 인덱스. */
	int i;
	/* [한국어] 0x80 + 0 패딩 배열. */
	static const uint8_t padding[64] = { 0x80, };

	/* Save number of bits */
	/* [한국어] 비트 단위 길이. */
	bits = (uint64_t) sctx->count << 3;

	/* Pad out to 56 mod 64. */
	/* [한국어] 오프셋 index = count % 64. */
	index = sctx->count & 0x3f;
	/* [한국어] 0x80 + 0 패딩 길이 계산. */
	pad_len = (index < 56) ? (56 - index) : ((64+56) - index);
	/* [한국어] 패딩 update. */
	fio_sha256_update(sctx, padding, pad_len);

	/* Append length (before padding) */
	/* [한국어] 8바이트 길이 필드 update — 마지막 블록 transform 유발. */
	fio_sha256_update(sctx, (const uint8_t *)&bits, sizeof(bits));

	/* Store state in digest */
	/* [한국어] 8 워드 state 를 buf[] 배열에 그대로 복사 — 본 구현은 바이트 직렬화를
	 * 호출자에게 위임한다(sha256.h 의 buf 타입이 uint32_t* 인 경우가 많음). */
	for (i = 0; i < 8; i++)
		sctx->buf[i] = sctx->state[i];
}
