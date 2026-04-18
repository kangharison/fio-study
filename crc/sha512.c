/* SHA-512 code by Jean-Luc Cooke <jlcooke@certainkey.com>
 *
 * Copyright (c) Jean-Luc Cooke <jlcooke@certainkey.com>
 * Copyright (c) Andrew McDonald <andrew@mcdonald.org.uk>
 * Copyright (c) 2003 Kyle McMartin <kyle@debian.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 */

/*
 * [한국어 설명] SHA-512 (및 SHA-384 초기값 정의) 해시 구현 (sha512.c)
 *
 * === 파일의 역할 ===
 * FIPS 180-4 의 SHA-512 를 구현한다. 1024비트(128B) 입력 블록을 80 스텝으로 처리해
 * 512비트 다이제스트(8 × 64비트 워드 state[0..7])를 생성하는 Merkle-Damgård 해시.
 * Ch/Maj 비선형 함수, Σ0/Σ1/σ0/σ1 회전 함수, 80개 라운드 상수(sha512_K) 로 구성되며,
 * 64비트 산술을 쓰기에 64비트 CPU 에서는 SHA-256 보다 더 빠르게 나올 수 있다.
 * 본 파일은 SHA-512 용 초기값 H0~H7 만 사용하지만, SHA-384 초기값 HP0~HP7 도 매크로로
 * 정의되어 있어 재사용 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 의 VERIFY_SHA512(+ SHA384) 경로.
 * 쓰기: fill_sha512(verify.c) → fio_sha512_init → update → final → state[] 직렬화.
 * 읽기: verify_io_u_sha512 가 동일 시퀀스로 재계산 후 verify_header.v_sha512 와 비교.
 * 호출 체인:
 *   verify.c::fill_sha512/verify_io_u_sha512 → fio_sha512_* → sha512_transform
 *   crc/test.c::t_sha512 → 동일
 *
 * === 타 모듈과의 연결 ===
 * - sha512.h: fio_sha512_ctx 구조체(state[8], count[4], buf[128], W[80]) 와 공용 API.
 * - lib/bswap.h: __be64_to_cpu 매크로(리틀엔디안 CPU 에서 입력 bswap, 빅엔디안은 no-op).
 * - verify.c / crc/test.c: 호출자.
 * 동기화: 컨텍스트 단위 독립 — 락 불요. sha512_K 테이블 read-only.
 *
 * === 주요 함수 요약 ===
 * - Ch/Maj/RORuint64_t: 비선형·회전 보조 함수.
 * - sha512_K[80]: FIPS 180-4 Table 6 의 상수.
 * - LOAD_OP/BLEND_OP: 메시지 스케줄의 로드·믹스 동작.
 * - sha512_transform: 블록 1개 80 스텝 변환.
 * - fio_sha512_init/update/final: 공용 API.
 */
#include <string.h>
/* [한국어] memcpy/memset: 블록 누적·보안을 위한 워드 지움 등에 사용. */

#include "../lib/bswap.h"
/* [한국어] lib/bswap.h: __be64_to_cpu(x) 매크로 — x 를 빅엔디안으로 간주하여 CPU 순서로 변환. */
#include "sha512.h"
/* [한국어] sha512.h: fio_sha512_ctx 구조체와 공용 API 프로토타입. */

/* [한국어] 다이제스트 바이트 수와 HMAC 블록 크기 상수 — FIPS 180-4 고정값. */
#define SHA384_DIGEST_SIZE 48
#define SHA512_DIGEST_SIZE 64
#define SHA384_HMAC_BLOCK_SIZE 128
#define SHA512_HMAC_BLOCK_SIZE 128

/*
 * [한국어] Ch - SHA-2 의 "choose" 비선형 함수 = (x AND y) XOR (NOT x AND z).
 * 본 구현은 z ^ (x & (y^z)) 형태로 동치(연산 절약). */
static inline uint64_t Ch(uint64_t x, uint64_t y, uint64_t z)
{
        return z ^ (x & (y ^ z));
}

/*
 * [한국어] Maj - SHA-2 의 "majority" 비선형 함수 = (x AND y) XOR (x AND z) XOR (y AND z).
 * 본 구현은 (x&y) | (z&(x|y)) 형태로 동치. */
static inline uint64_t Maj(uint64_t x, uint64_t y, uint64_t z)
{
        return (x & y) | (z & (x | y));
}

/*
 * [한국어] RORuint64_t - 64비트 우회전(rotate right).
 * @x: 피회전 값. @y: 회전 비트 수(1..63). */
static inline uint64_t RORuint64_t(uint64_t x, uint64_t y)
{
        return (x >> y) | (x << (64 - y));
}

/* [한국어] SHA-512 라운드 상수 80개 — FIPS 180-4 §4.2.3.
 * 설정자: 컴파일타임 상수. 읽는 자: sha512_transform 의 80 스텝. .rodata — 락 불요. */
static const uint64_t sha512_K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
        0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
        0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
        0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
        0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
        0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
        0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
        0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
        0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
        0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
        0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
        0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
        0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
        0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
        0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
        0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
        0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
        0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
        0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
        0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
        0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

/* [한국어] Σ0/Σ1 (대문자 시그마): 스텝 혼합에 쓰이는 회전 XOR.
 *   Σ0 = ror28 ^ ror34 ^ ror39
 *   Σ1 = ror14 ^ ror18 ^ ror41
 * 그리고 σ0/σ1 (소문자): 메시지 스케줄 BLEND_OP 에 쓰이는 회전 + 우시프트.
 *   σ0 = ror1  ^ ror8  ^ (x>>7)
 *   σ1 = ror19 ^ ror61 ^ (x>>6) */
#define e0(x)       (RORuint64_t(x,28) ^ RORuint64_t(x,34) ^ RORuint64_t(x,39))
#define e1(x)       (RORuint64_t(x,14) ^ RORuint64_t(x,18) ^ RORuint64_t(x,41))
#define s0(x)       (RORuint64_t(x, 1) ^ RORuint64_t(x, 8) ^ (x >> 7))
#define s1(x)       (RORuint64_t(x,19) ^ RORuint64_t(x,61) ^ (x >> 6))

/* H* initial state for SHA-512 */
/* [한국어] SHA-512 초기 해시값 — 소수의 제곱근 분수부에서 유도된 FIPS 180-4 §5.3.5 상수. */
#define H0         0x6a09e667f3bcc908ULL
#define H1         0xbb67ae8584caa73bULL
#define H2         0x3c6ef372fe94f82bULL
#define H3         0xa54ff53a5f1d36f1ULL
#define H4         0x510e527fade682d1ULL
#define H5         0x9b05688c2b3e6c1fULL
#define H6         0x1f83d9abfb41bd6bULL
#define H7         0x5be0cd19137e2179ULL

/* H'* initial state for SHA-384 */
/* [한국어] SHA-384 초기값(§5.3.4) — 본 파일은 SHA-512 만 초기화하지만 매크로는 정의해둠. */
#define HP0 0xcbbb9d5dc1059ed8ULL
#define HP1 0x629a292a367cd507ULL
#define HP2 0x9159015a3070dd17ULL
#define HP3 0x152fecd8f70e5939ULL
#define HP4 0x67332667ffc00b31ULL
#define HP5 0x8eb44a8768581511ULL
#define HP6 0xdb0c2e0d64f98fa7ULL
#define HP7 0x47b5481dbefa4fa4ULL

/*
 * [한국어] LOAD_OP - 입력 블록의 I 번째 64비트 워드를 빅엔디안으로 읽어 W[I] 에 저장.
 * SHA-2 규격은 입력을 빅엔디안 64비트 워드로 해석하므로 리틀엔디안 CPU 에서는 bswap.
 */
static inline void LOAD_OP(int I, uint64_t *W, const uint8_t *input)
{
	W[I] = __be64_to_cpu( ((uint64_t *)(input))[I] );
}

/*
 * [한국어] BLEND_OP - 메시지 스케줄의 I >= 16 슬롯을 유도 —
 *   W[I] = σ1(W[I-2]) + W[I-7] + σ0(W[I-15]) + W[I-16].
 * FIPS 180-4 §6.4.2 Step 1.
 */
static inline void BLEND_OP(int I, uint64_t *W)
{
	W[I] = s1(W[I-2]) + W[I-7] + s0(W[I-15]) + W[I-16];
}

/*
 * [한국어]
 * sha512_transform - 1024비트(128B) 블록 1개 SHA-512 변환
 *
 * @state: 8워드 chaining value (in/out).
 * @W:     외부에서 제공된 80워드 메시지 스케줄 버퍼(컨텍스트 내부 고정 배열).
 * @input: 128B 빅엔디안 입력 블록.
 *
 * 동작(FIPS 180-4 §6.4.2):
 *   1) W[0..15] 에 입력을 LOAD_OP 로 로드(빅엔디안).
 *   2) W[16..79] 를 BLEND_OP 로 유도.
 *   3) 레지스터 a..h 에 state 를 복사.
 *   4) 80 스텝을 8-스텝 언롤된 루프로 수행 — 각 스텝은 T1 = h + Σ1(e) + Ch(e,f,g) + K[i] + W[i],
 *      T2 = Σ0(a) + Maj(a,b,c), d += T1, h = T1 + T2, 이후 a..h 순환.
 *   5) state[] 에 a..h 누적 덧셈.
 *   6) 레지스터 제로화(민감 정보 소거).
 *
 * 호출 체인: fio_sha512_update/fio_sha512_final → [sha512_transform]
 */
static void sha512_transform(uint64_t *state, uint64_t *W, const uint8_t *input)
{
	/* [한국어] 작업 레지스터 a..h 와 임시 T1/T2. */
	uint64_t a, b, c, d, e, f, g, h, t1, t2;

	/* [한국어] 스텝/루프 인덱스. */
	int i;

	/* load the input */
	/* [한국어] 첫 16 워드는 입력에서 직접 로드(빅엔디안). */
	for (i = 0; i < 16; i++)
		LOAD_OP(i, W, input);

	/* [한국어] 16..79 워드는 앞 워드들로 스케줄 유도. */
	for (i = 16; i < 80; i++)
		BLEND_OP(i, W);

	/* load the state into our registers */
	/* [한국어] 이전 블록까지의 chaining value 적재. */
	a=state[0];   b=state[1];   c=state[2];   d=state[3];
	e=state[4];   f=state[5];   g=state[6];   h=state[7];

	/* now iterate */
	/* [한국어] 80 스텝을 8단위로 언롤 — 각 본체는 스텝 내 "a→h 순환"을 변수명 바꿔 표현. */
	for (i=0; i<80; i+=8) {
		t1 = h + e1(e) + Ch(e,f,g) + sha512_K[i  ] + W[i  ];
		t2 = e0(a) + Maj(a,b,c);    d+=t1;    h=t1+t2;
		t1 = g + e1(d) + Ch(d,e,f) + sha512_K[i+1] + W[i+1];
		t2 = e0(h) + Maj(h,a,b);    c+=t1;    g=t1+t2;
		t1 = f + e1(c) + Ch(c,d,e) + sha512_K[i+2] + W[i+2];
		t2 = e0(g) + Maj(g,h,a);    b+=t1;    f=t1+t2;
		t1 = e + e1(b) + Ch(b,c,d) + sha512_K[i+3] + W[i+3];
		t2 = e0(f) + Maj(f,g,h);    a+=t1;    e=t1+t2;
		t1 = d + e1(a) + Ch(a,b,c) + sha512_K[i+4] + W[i+4];
		t2 = e0(e) + Maj(e,f,g);    h+=t1;    d=t1+t2;
		t1 = c + e1(h) + Ch(h,a,b) + sha512_K[i+5] + W[i+5];
		t2 = e0(d) + Maj(d,e,f);    g+=t1;    c=t1+t2;
		t1 = b + e1(g) + Ch(g,h,a) + sha512_K[i+6] + W[i+6];
		t2 = e0(c) + Maj(c,d,e);    f+=t1;    b=t1+t2;
		t1 = a + e1(f) + Ch(f,g,h) + sha512_K[i+7] + W[i+7];
		t2 = e0(b) + Maj(b,c,d);    e+=t1;    a=t1+t2;
	}

	/* [한국어] Merkle-Damgård 누적: chaining value 에 이번 블록 결과 덧셈. */
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;

	/* erase our data */
	/* [한국어] 레지스터 제로화 — 민감 정보(내부 상태) 소거. 컴파일러가 DCE 하지 못하도록
	 * 실제로는 volatile 장치가 더 안전하지만 관례적으로 이렇게 기록. */
	a = b = c = d = e = f = g = h = t1 = t2 = 0;
}

/*
 * [한국어]
 * fio_sha512_init - SHA-512 컨텍스트 초기화(H0~H7 + 비트 카운터 0)
 *
 * @sctx: 초기화 대상.
 *
 * SHA-512 는 128비트 길이 카운터(count[4] = 4×32비트)를 사용 — 2^128 비트까지 해시 가능.
 *
 * 호출 체인: verify.c / crc/test.c → [fio_sha512_init]
 */
void fio_sha512_init(struct fio_sha512_ctx *sctx)
{
	/* [한국어] FIPS 180-4 §5.3.5 초기 해시값. */
	sctx->state[0] = H0;
	sctx->state[1] = H1;
	sctx->state[2] = H2;
	sctx->state[3] = H3;
	sctx->state[4] = H4;
	sctx->state[5] = H5;
	sctx->state[6] = H6;
	sctx->state[7] = H7;
	/* [한국어] 128비트 비트-길이 카운터 4 워드 모두 0. */
	sctx->count[0] = sctx->count[1] = sctx->count[2] = sctx->count[3] = 0;
}

/*
 * [한국어]
 * fio_sha512_update - SHA-512 블록 누적(128B 단위)
 *
 * @sctx: 컨텍스트.
 * @data: 입력 바이트.
 * @len:  바이트 길이.
 *
 * 동작:
 *   1) 현재 버퍼 오프셋 idx = (count[0]/8) mod 128 계산.
 *   2) count[] 를 입력 비트 수(len*8) 만큼 증가 — 128비트 폭 캐리 체인.
 *   3) idx + len >= 128 이면: 먼저 버퍼의 빈 공간을 채워 한 블록 flush, 이후 128B 단위로 루프.
 *   4) 남은 <128B 는 buf 에 저장.
 *
 * 호출 체인: verify.c / test.c / fio_sha512_final(패딩 주입) → [fio_sha512_update] → sha512_transform
 */
void fio_sha512_update(struct fio_sha512_ctx *sctx, const uint8_t *data,
		       unsigned int len)
{
	/* [한국어] i: data 소비 오프셋, idx: 버퍼 내 현재 위치, part_len: 버퍼 남은 공간. */
	unsigned int i, idx, part_len;

	/* Compute number of bytes mod 128 */
	/* [한국어] count[0] 는 "메시지 비트 수 하위 32비트" — >> 3 로 바이트 환산, & 0x7F 로 %128. */
	idx = (unsigned int)((sctx->count[0] >> 3) & 0x7F);

	/* Update number of bits */
	/* [한국어] count[0] 에 len*8 을 더하고, 오버플로(len<<3 보다 작아지면)가 발생하면
	 * count[1] 로 캐리, 또 넘치면 count[2], count[3] 순으로 전파 — 128비트 덧셈 구현. */
	if ((sctx->count[0] += (len << 3)) < (len << 3)) {
		if ((sctx->count[1] += 1) < 1)
			if ((sctx->count[2] += 1) < 1)
				sctx->count[3]++;
		/* [한국어] 또한 상위 32비트(len>>29) 도 count[1] 에 누적 — len*8 의 상위 부분 분리. */
		sctx->count[1] += (len >> 29);
	}

        /* [한국어] 버퍼에 남은 공간 (128 - idx). */
        part_len = 128 - idx;

	/* Transform as many times as possible. */
	/* [한국어] 새 입력이 최소 한 블록을 완성할 수 있는지. */
	if (len >= part_len) {
		/* [한국어] 먼저 버퍼의 빈 공간을 채워 한 블록 완성. */
		memcpy(&sctx->buf[idx], data, part_len);
		/* [한국어] 첫 블록 flush. */
		sha512_transform(sctx->state, sctx->W, sctx->buf);

		/* [한국어] 완전한 128B 블록들은 data 에서 직접 읽어 transform. */
		for (i = part_len; i + 127 < len; i+=128)
			sha512_transform(sctx->state, sctx->W, &data[i]);

		/* [한국어] 버퍼 초기화 — 꼬리 저장 위치는 0부터. */
		idx = 0;
	} else {
		/* [한국어] 블록 완성 불가 — 전체를 버퍼에 저장만 함. */
		i = 0;
	}

	/* Buffer remaining input */
	/* [한국어] 남은 꼬리(<128B)를 버퍼에 저장 — 다음 update/final 에 이어감. */
	memcpy(&sctx->buf[idx], &data[i], len - i);

	/* erase our data */
	/* [한국어] 메시지 스케줄 워드 배열 W 지우기(민감 정보 소거 관행). */
	memset(sctx->W, 0, sizeof(sctx->W));
}

/*
 * [한국어]
 * fio_sha512_final - SHA-512 패딩 + 길이 필드 주입 + 다이제스트 직렬화
 *
 * @sctx: 컨텍스트. 종료 후 sctx->buf 선두에 64B(=512비트) 빅엔디안 다이제스트.
 *
 * 패딩(FIPS 180-4 §5.1.2): 0x80 + 0... + 128비트 길이 필드. 길이 필드는 메시지 비트 수를
 * 빅엔디안으로 기록하며 블록 끝 16바이트에 위치. 패딩 길이는 (메시지 길이 + 1) mod 128 이
 * 112 가 되도록 조정.
 *
 * 호출 체인: verify.c / test.c → [fio_sha512_final] → fio_sha512_update → sha512_transform
 */
void fio_sha512_final(struct fio_sha512_ctx *sctx)
{
	/* [한국어] 다이제스트 직렬화 대상 — ctx->buf 시작 주소를 별칭으로 사용. */
	uint8_t *hash = sctx->buf;
	/* [한국어] 0x80 + 0 패딩 배열(최대 128B). */
	static uint8_t padding[128] = { 0x80, };
	/* [한국어] 현재 버퍼 오프셋, 패딩 필요 길이. */
	unsigned int index, pad_len;
	/* [한국어] 128비트 길이 필드 버퍼(빅엔디안). */
	uint8_t bits[128];
	/* [한국어] 직렬화 임시 변수. */
	uint64_t t2;
	uint32_t t;
	/* [한국어] 루프 인덱스. */
	int i, j;

        /* [한국어] 로컬 변수 명시 초기화(과거 컴파일러 경고 억제 관행). */
        index = pad_len = t = i = j = 0;
        t2 = 0;

	/* Save number of bits */
	/* [한국어] 128비트 비트-카운트(count[0..3])를 빅엔디안 바이트 16개로 직렬화.
	 * bits[15] 가 최하위 바이트, bits[0] 이 최상위 바이트.
	 * count[0]: 하위 32비트, count[1]: 다음 32비트, ..., count[3]: 최상위 32비트. */
	t = sctx->count[0];
	bits[15] = t; t>>=8;
	bits[14] = t; t>>=8;
	bits[13] = t; t>>=8;
	bits[12] = t;
	t = sctx->count[1];
	bits[11] = t; t>>=8;
	bits[10] = t; t>>=8;
	bits[9 ] = t; t>>=8;
	bits[8 ] = t;
	t = sctx->count[2];
	bits[7 ] = t; t>>=8;
	bits[6 ] = t; t>>=8;
	bits[5 ] = t; t>>=8;
	bits[4 ] = t;
	t = sctx->count[3];
	bits[3 ] = t; t>>=8;
	bits[2 ] = t; t>>=8;
	bits[1 ] = t; t>>=8;
	bits[0 ] = t;

	/* Pad out to 112 mod 128. */
	/* [한국어] 현재 바이트 오프셋(=count[0]/8 % 128). */
	index = (sctx->count[0] >> 3) & 0x7f;
	/* [한국어] 0x80 + 0 패딩 길이: (112 - index) 또는 부족하면 한 블록 뛰어넘어 (128+112) - index. */
	pad_len = (index < 112) ? (112 - index) : ((128+112) - index);
	/* [한국어] 정적 padding 배열의 앞부분을 update 로 흘려 넣어 패딩 완성. */
	fio_sha512_update(sctx, padding, pad_len);

	/* Append length (before padding) */
	/* [한국어] 128비트 길이 필드 16바이트 추가 — 이 update 로 마지막 블록 transform 유발. */
	fio_sha512_update(sctx, bits, 16);

	/* Store state in digest */
	/* [한국어] 8 워드(각 64비트)의 state 를 빅엔디안 바이트 64개로 직렬화 — hash[j..j+7] 에 기록.
	 * (char) 캐스팅은 하위 8비트 추출 힌트(바이트 단위 저장). */
	for (i = j = 0; i < 8; i++, j += 8) {
		/* [한국어] 현재 워드 복사(소비용). */
		t2 = sctx->state[i];
		/* [한국어] 최하위 바이트부터 채우고 shift 해 위로 올라감 — 결과적으로 j 에서 j+7 까지
		 * 빅엔디안 순서(최상위 바이트가 hash[j])가 된다. */
		hash[j+7] = (char)t2 & 0xff; t2>>=8;
		hash[j+6] = (char)t2 & 0xff; t2>>=8;
		hash[j+5] = (char)t2 & 0xff; t2>>=8;
		hash[j+4] = (char)t2 & 0xff; t2>>=8;
		hash[j+3] = (char)t2 & 0xff; t2>>=8;
		hash[j+2] = (char)t2 & 0xff; t2>>=8;
		hash[j+1] = (char)t2 & 0xff; t2>>=8;
		hash[j  ] = (char)t2 & 0xff;
	}
}
