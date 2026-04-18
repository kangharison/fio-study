/*
 * Cryptographic API.
 *
 * SHA-3, as specified in
 * http://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.202.pdf
 *
 * SHA-3 code by Jeff Garzik <jeff@garzik.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)•
 * any later version.
 *
 */
/*
 * [한국어 설명] SHA-3 (Keccak 스펀지 함수) 구현 (sha3.c)
 *
 * === 파일의 역할 ===
 * NIST FIPS 202 가 정의한 SHA-3 해시 함수를 구현한다. 4가지 다이제스트 크기
 * (SHA3-224, SHA3-256, SHA3-384, SHA3-512) 를 모두 지원하며, 내부적으로는
 * 단일 엔진 — 1600비트 상태(`uint64_t st[25]`)에 대한 Keccak-f[1600] 순열을
 * 24라운드 돌리는 "스펀지 구조" — 를 공유한다. absorb(입력 흡수) 단계는
 * `update()` 에서 rate 바이트(r = 200 - 2c; c는 다이제스트 크기의 두 배) 씩
 * 입력을 상태의 앞 r/8 개 레인에 XOR 한 뒤 Keccak-f 를 적용하고, squeeze(출력 압착)
 * 단계는 `final()` 에서 SHA-3 도메인 분리 패딩(0x06 + trailing 0x80) 뒤
 * Keccak-f 한 번 더 + 리틀엔디안 변환으로 md_len 바이트를 출력한다.
 * SHA-3 는 SHA-1/SHA-2 와 완전히 다른 구조(스펀지 vs Merkle–Damgård)를 가진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 의 VERIFY_SHA3_{224,256,384,512} 경로.
 * 쓰기: fill_sha3_224(verify.c) 등 → fio_sha3_224_init → update → final → digest 저장.
 * 읽기: verify_io_u_sha3_224 등이 같은 시퀀스로 재계산 후 verify_header 의 sha 와 비교.
 * 호출 체인:
 *   verify.c::fill_sha3_xxx / verify_io_u_sha3_xxx
 *     → fio_sha3_xxx_init → fio_sha3_init → fio_sha3_update → fio_sha3_final → keccakf
 *   crc/test.c::t_sha3_{224,256,384,512} → 동일 시퀀스
 *
 * === 타 모듈과의 연결 ===
 * - sha3.h: fio_sha3_ctx 구조체 정의(st[25]/md_len/rsiz/rsizw/partial/buf[SHA3_MAX_RSIZ]/sha),
 *   SHA3_{224,256,384,512}_DIGEST_SIZE 상수, 네 init 및 공용 update/final 프로토타입.
 * - os/os.h: cpu_to_le64() 매크로(빅엔디안 CPU 에서는 bswap, 리틀엔디안에서는 no-op).
 * - verify.c: verify=sha3-xxx 옵션 경로.
 * - crc/test.c: --crctest 벤치마크 러너.
 * 동기화: fio_sha3_ctx 는 호출자 스택/동적 할당 — 호출자 단위 독립이라 락 불필요.
 * keccakf_rndc/rotc/piln 테이블과 keccakf 함수는 재진입 안전.
 *
 * === 주요 함수 요약 ===
 * - keccakf(st[25]): Keccak-f[1600] 순열 24라운드 — Theta/Rho/Pi/Chi/Iota 5단계 반복.
 * - fio_sha3_init(sctx, digest_sz): 공통 초기화(상태 0, md_len·rsiz·rsizw 계산, 버퍼 0).
 * - fio_sha3_{224,256,384,512}_init: 특정 크기로 위 공통 init 을 호출하는 얇은 래퍼.
 * - fio_sha3_update(sctx, data, len): rate 바이트 단위로 입력 흡수.
 * - fio_sha3_final(sctx): SHA-3 도메인 분리 패딩 + 최종 흡수 + squeeze → sha[md_len].
 */
#include <string.h>
/* [한국어] memset/memcpy: 상태 초기화·버퍼 복사에 필요. */

#include "../os/os.h"
/* [한국어] os/os.h: cpu_to_le64() (엔디안 변환) 매크로 공급. SHA-3 출력 레인은
 * 리틀엔디안 바이트 순서로 직렬화하므로 빅엔디안 머신에서는 bswap64 필요. */

#include "sha3.h"
/* [한국어] sha3.h: fio_sha3_ctx 구조체·SHA3_*_DIGEST_SIZE 상수·공용 API 선언. */

/* [한국어] Keccak-f[1600] 순열 라운드 수. FIPS 202 가 지정한 고정값. */
#define KECCAK_ROUNDS 24

/* [한국어] 64비트 좌회전 매크로 — (x<<y) 와 (x>>(64-y)) 를 결합. Rho 단계와 Theta 의
 * parity 회전에 사용. */
#define ROTL64(x, y) (((x) << (y)) | ((x) >> (64 - (y))))

/* [한국어] Iota 단계의 라운드 상수 24개 — FIPS 202 Algorithm 7 기반.
 * 설정자: 컴파일타임 상수.
 * 읽는 자: keccakf() 의 Iota 단계.
 * 값 범위: 각 64비트, "1이 있는 비트 위치가 라운드별로 다른" 패턴.
 * 동기화: .rodata — 락 불필요. */
static const uint64_t keccakf_rndc[24] = {
	0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
	0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
	0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
	0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
	0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
	0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
	0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
	0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

/* [한국어] Rho 단계의 레인별 좌회전 비트 수 24개.
 * keccakf_piln[i] 가 가리키는 레인에 대해 ROTL64(t, keccakf_rotc[i]) 로 섞는다. */
static const int keccakf_rotc[24] = {
	1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
	27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44
};

/* [한국어] Pi 단계의 레인 순열 인덱스 24개 — i번째에 방문할 레인 번호.
 * Rho/Pi 가 동시 적용: "st[j] = rotl(t, rotc[i])" 식 갱신. */
static const int keccakf_piln[24] = {
	10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
	15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1
};

/* update the state with given number of rounds */

/*
 * [한국어]
 * keccakf - Keccak-f[1600] 순열 24라운드 실행
 *
 * @st: 상태 배열(5×5 레인, 각 64비트 → 총 1600비트). in-place 변경.
 *
 * 한 라운드는 Theta → Rho → Pi → Chi → Iota 의 5단계:
 *   - Theta: 열(column) 패리티 XOR 로 상태 전체에 diffusion.
 *   - Rho  : 레인 단위 좌회전(keccakf_rotc[] 사용).
 *   - Pi   : 레인 순열(keccakf_piln[] 사용).
 *   - Chi  : 행(row) 단위 비선형 연산 (a ^= (~b) & c).
 *   - Iota : 라운드 상수 XOR 로 대칭성 파괴.
 *
 * 실행 컨텍스트: update/final 호출자 스레드 — 순수 계산, 재진입 안전.
 *
 * 호출 체인: fio_sha3_update/fio_sha3_final → [keccakf] (내부 연산)
 */
static void keccakf(uint64_t st[25])
{
	/* [한국어] i: Theta/Chi 열 인덱스(0..4), j: 행·Pi 인덱스, round: 현재 라운드. */
	int i, j, round;
	/* [한국어] t: Rho/Pi 순환 변수. bc[5]: Theta 의 열 패리티 / Chi 의 임시 행. */
	uint64_t t, bc[5];

	/* [한국어] 24라운드 반복 — FIPS 202 규정 수치. */
	for (round = 0; round < KECCAK_ROUNDS; round++) {

		/* Theta */
		/* [한국어] 각 열의 XOR 패리티를 bc[i] 에 저장 — 5개 열. */
		for (i = 0; i < 5; i++)
			bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15]
				^ st[i + 20];

		/* [한국어] Theta 의 본체: 각 레인에 bc[(i-1) mod 5] ^ rotl(bc[(i+1) mod 5], 1) 을 XOR. */
		for (i = 0; i < 5; i++) {
			/* [한국어] t 는 현재 열 i 에 반영할 XOR 값(이웃 열 패리티 조합). */
			t = bc[(i + 4) % 5] ^ ROTL64(bc[(i + 1) % 5], 1);
			/* [한국어] i 열에 속한 5개 레인(0,5,10,15,20 + i) 전부에 t 를 XOR. */
			for (j = 0; j < 25; j += 5)
				st[j + i] ^= t;
		}

		/* Rho Pi */
		/* [한국어] Rho+Pi 는 함께 수행 — 순환 인덱스 t 를 사용하는 "사이클 붙이기" 패턴. */
		t = st[1];
		for (i = 0; i < 24; i++) {
			/* [한국어] 이번 단계에서 갱신할 레인 번호 j (piln 순열 적용). */
			j = keccakf_piln[i];
			/* [한국어] 기존 값을 bc[0] 에 잠시 보관. */
			bc[0] = st[j];
			/* [한국어] 이전 t 를 rotc[i] 만큼 회전해 j번째 레인에 기록(Rho). */
			st[j] = ROTL64(t, keccakf_rotc[i]);
			/* [한국어] 다음 반복의 t 는 방금 치환당한 값 — 사이클 전파. */
			t = bc[0];
		}

		/* Chi */
		/* [한국어] 5개 행에 대해 비선형 연산 a ^= (~b) & c 적용. */
		for (j = 0; j < 25; j += 5) {
			/* [한국어] 행(j..j+4)의 현재 5개 레인을 bc[] 에 복사(임시). */
			for (i = 0; i < 5; i++)
				bc[i] = st[j + i];
			/* [한국어] 각 레인 i 에 대해 Chi 공식 적용 — 비선형 확산. */
			for (i = 0; i < 5; i++)
				st[j + i] ^= (~bc[(i + 1) % 5]) &
					     bc[(i + 2) % 5];
		}

		/* Iota */
		/* [한국어] (0,0) 레인에 라운드 상수 XOR — 라운드별 대칭성 파괴. */
		st[0] ^= keccakf_rndc[round];
	}
}

/*
 * [한국어]
 * fio_sha3_init - SHA-3 컨텍스트 공통 초기화(다이제스트 크기별 분기의 내부 구현)
 *
 * @sctx:      초기화 대상. 호출자가 미리 할당(스택/동적).
 * @digest_sz: 다이제스트 바이트 수 — SHA3_{224,256,384,512}_DIGEST_SIZE (28/32/48/64).
 *
 * 상태/버퍼를 0으로 초기화하고, 스펀지 파라미터 rsiz(=rate in bytes) 와 rsizw(=rate in
 * 64비트 워드) 를 계산한다. SHA-3 는 capacity c = 2 * digest_sz 로 정의되므로
 * rate r = 200 - c = 200 - 2*digest_sz 바이트(=1600비트 - 2*8*digest_sz).
 *   SHA3-224(d=28): r=144, SHA3-256(d=32): r=136, SHA3-384(d=48): r=104, SHA3-512(d=64): r=72.
 *
 * 호출 체인: fio_sha3_{224,256,384,512}_init → [fio_sha3_init]
 */
static void fio_sha3_init(struct fio_sha3_ctx *sctx, unsigned int digest_sz)
{
	/* [한국어] 스펀지 상태(25 × 8B = 200B) 전체를 0으로 클리어. */
	memset(sctx->st, 0, sizeof(sctx->st));
	/* [한국어] 다이제스트 바이트 수 저장 — final() 의 squeeze 길이 결정용. */
	sctx->md_len = digest_sz;
	/* [한국어] rate(바이트): SHA-3 정의 r = 200 - 2*digest_sz. */
	sctx->rsiz = 200 - 2 * digest_sz;
	/* [한국어] rate(64비트 워드 수): update 루프에서 XOR 할 레인 개수. */
	sctx->rsizw = sctx->rsiz / 8;
	/* [한국어] 현재 버퍼에 담긴 부분 바이트 수 — 0 으로 시작. */
	sctx->partial = 0;
	/* [한국어] 부분 버퍼(최대 rate) 클리어. */
	memset(sctx->buf, 0, sizeof(sctx->buf));
}

/*
 * [한국어] fio_sha3_224_init - SHA3-224 (28B 다이제스트) 초기화.
 * 호출 체인: verify.c / crc/test.c → [fio_sha3_224_init] → fio_sha3_init(28)
 */
void fio_sha3_224_init(struct fio_sha3_ctx *sctx)
{
	/* [한국어] 224비트 = 28B 로 공통 초기화 호출 → rate=144. */
	fio_sha3_init(sctx, SHA3_224_DIGEST_SIZE);
}

/*
 * [한국어] fio_sha3_256_init - SHA3-256 (32B) 초기화. rate=136.
 */
void fio_sha3_256_init(struct fio_sha3_ctx *sctx)
{
	/* [한국어] 256비트 = 32B. */
	fio_sha3_init(sctx, SHA3_256_DIGEST_SIZE);
}

/*
 * [한국어] fio_sha3_384_init - SHA3-384 (48B) 초기화. rate=104.
 */
void fio_sha3_384_init(struct fio_sha3_ctx *sctx)
{
	/* [한국어] 384비트 = 48B. */
	fio_sha3_init(sctx, SHA3_384_DIGEST_SIZE);
}

/*
 * [한국어] fio_sha3_512_init - SHA3-512 (64B) 초기화. rate=72.
 */
void fio_sha3_512_init(struct fio_sha3_ctx *sctx)
{
	/* [한국어] 512비트 = 64B. */
	fio_sha3_init(sctx, SHA3_512_DIGEST_SIZE);
}

/*
 * [한국어]
 * fio_sha3_update - 입력 데이터를 Keccak 스펀지에 흡수(absorb)
 *
 * @sctx: 초기화된 SHA-3 컨텍스트.
 * @data: 흡수할 입력 바이트.
 * @len:  바이트 길이.
 * @return: 0 (에러 없음).
 *
 * 동작:
 *   1) 기존 partial 과 새 len 의 합이 rate-1 미만이면 단순히 내부 버퍼에 누적 후 반환.
 *   2) 합이 rate 이상이면:
 *      a) 남은 partial 을 메꾸고 src 를 내부 버퍼 시작으로 설정.
 *      b) rate 단위로 반복 — 각 rate 블록을 상태 앞 rsizw 개 레인에 XOR 한 뒤 keccakf().
 *      c) 남은 <rate 바이트만 내부 버퍼로 카피.
 *
 * 실행 컨텍스트: 호출자 스레드, 재진입 안전(컨텍스트 단위 독립 상태).
 *
 * 호출 체인: verify.c / test.c → [fio_sha3_update] → keccakf
 */
int fio_sha3_update(struct fio_sha3_ctx *sctx, const uint8_t *data,
		    unsigned int len)
{
	/* [한국어] done: 현재까지 소비한 data 오프셋(음수에서 시작하는 트릭 사용).
	 * src: 실제 absorb 대상 포인터(내부 버퍼 또는 data 직접). */
	unsigned int done;
	const uint8_t *src;

	/* [한국어] 초기 소비량 0. */
	done = 0;
	/* [한국어] 기본은 data 로부터 직접 흡수. */
	src = data;

	/* [한국어] partial + len 이 rate-1 초과면 최소 한 블록은 흡수 가능 → 대량 루프 진입. */
	if ((sctx->partial + len) > (sctx->rsiz - 1)) {
		/* [한국어] 내부 버퍼에 남아있던 부분을 먼저 마저 채워 한 블록 완성. */
		if (sctx->partial) {
			/* [한국어] done 을 음수(-partial)로 두면 아래 rate 루프에서 포인터 계산이 깔끔. */
			done = -sctx->partial;
			/* [한국어] 버퍼 뒷부분에 data 에서 (rsiz-partial) 바이트 복사 → 한 rate 블록 완성. */
			memcpy(sctx->buf + sctx->partial, data,
			       done + sctx->rsiz);
			/* [한국어] 첫 블록은 내부 버퍼에서 absorb. */
			src = sctx->buf;
		}

		/* [한국어] rate 블록 단위 absorb 루프. */
		do {
			/* [한국어] 레인 인덱스 루프용 변수. */
			unsigned int i;

			/* [한국어] 상태 앞 rsizw 개 레인에 src 의 rsizw 워드를 XOR — SHA-3 흡수. */
			for (i = 0; i < sctx->rsizw; i++)
				sctx->st[i] ^= ((uint64_t *) src)[i];
			/* [한국어] Keccak-f 24라운드 순열로 확산. */
			keccakf(sctx->st);

			/* [한국어] 소비량 rate 만큼 증가. */
			done += sctx->rsiz;
			/* [한국어] 다음 흡수 대상은 data + done (데이터 쪽 원본으로 전환). */
			src = data + done;
		} while (done + (sctx->rsiz - 1) < len);

		/* [한국어] partial 초기화 — 방금 모두 소진. */
		sctx->partial = 0;
	}
	/* [한국어] 남은 <rate 바이트를 내부 버퍼의 이어받기 위치에 복사. */
	memcpy(sctx->buf + sctx->partial, src, len - done);
	/* [한국어] partial 카운터 갱신 — 다음 update/final 에서 시작 오프셋. */
	sctx->partial += (len - done);

	/* [한국어] 0 반환(에러 없음) — API 계약상 int 리턴을 유지. */
	return 0;
}

/*
 * [한국어]
 * fio_sha3_final - SHA-3 패딩·최종 흡수·squeeze 로 다이제스트 확정
 *
 * @sctx: 컨텍스트. 성공 시 sctx->sha 에 md_len 바이트 다이제스트 기록.
 *
 * SHA-3 패딩(도메인 분리 0x06): 마지막 partial 바이트 뒤에 0x06 기록, 블록 끝까지 0으로
 * 채운 뒤 rate-1 바이트에 0x80 (최상위 비트)을 OR. 그 블록을 흡수한 후 한 번 더 keccakf,
 * 이후 상태 앞 rsizw 개 레인을 리틀엔디안으로 직렬화해 md_len 바이트만 sha 로 복사.
 *
 * 호출 체인: verify.c / crc/test.c → [fio_sha3_final] → keccakf
 */
void fio_sha3_final(struct fio_sha3_ctx *sctx)
{
	/* [한국어] i: 레인 인덱스. inlen: 기존 partial(패딩 시작 위치). */
	unsigned int i, inlen = sctx->partial;

	/* [한국어] 도메인 분리 접두 비트(SHA-3 표준 0x06) 기록 후 inlen 을 1 증가. */
	sctx->buf[inlen++] = 0x06;
	/* [한국어] 패딩 중간은 0으로 채움. */
	memset(sctx->buf + inlen, 0, sctx->rsiz - inlen);
	/* [한국어] 마지막 바이트 MSB 에 0x80 OR — FIPS 202 패딩 규약. */
	sctx->buf[sctx->rsiz - 1] |= 0x80;

	/* [한국어] 패딩된 마지막 블록을 상태에 XOR 흡수. */
	for (i = 0; i < sctx->rsizw; i++)
		sctx->st[i] ^= ((uint64_t *) sctx->buf)[i];

	/* [한국어] 마지막 Keccak-f 로 확산 — squeeze 전 필수 단계. */
	keccakf(sctx->st);

	/* [한국어] 리틀엔디안 바이트 순서로 변환(빅엔디안 CPU 에서만 실제 bswap 동작). */
	for (i = 0; i < sctx->rsizw; i++)
		sctx->st[i] = cpu_to_le64(sctx->st[i]);

	/* [한국어] 상태 앞 md_len 바이트만 sha 로 복사 → 다이제스트 완성. */
	memcpy(sctx->sha, sctx->st, sctx->md_len);
}
