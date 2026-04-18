/*
 * Common values for SHA-3 algorithms
 */

/*
 * [한국어 설명] SHA-3(Keccak) 해시 알고리즘 헤더 (sha3.h)
 *
 * === 파일의 역할 ===
 * SHA-3 계열(FIPS 202) 해시 함수의 공개 API 와 변종별 상수를 정의한다.
 * SHA-3 는 Keccak 스펀지 함수 기반으로 SHA-1/SHA-2 의 Merkle-Damgård 구조와
 * 완전히 다르다. 상태는 1600비트(25 × 64비트 레인) 이며, rate r = 200 - 2c 바이트를
 * 한 번에 흡수(absorb) 하고 c/2 비트의 보안 강도를 capacity 에서 얻는다.
 * 본 헤더는 4개 변종(224/256/384/512) 모두의 DIGEST_SIZE / BLOCK_SIZE(rate) 상수,
 * 단일 스펀지 컨텍스트 구조체, 그리고 init/update/final 공개 API 를 선언한다.
 * 라운드는 24회(Keccak-f[1600] 기준), 매 라운드 Theta/Rho/Pi/Chi/Iota 5단계를 수행.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_sha3_XXX() / verify_io_u_sha3_XXX()
 *     → fio_sha3_{224,256,384,512}_init(ctx)  // 변종별 rate/md_len 설정
 *     → fio_sha3_update(ctx, data, len)       // 스펀지 흡수 단계
 *     → fio_sha3_final(ctx)                   // 압착(squeeze) 단계, ctx->sha 에 기록
 *
 * === 타 모듈과의 연결 ===
 * - sha3.c: 실제 구현(Keccak-f[1600] 24라운드, rotc/piln/rndc 테이블).
 * - verify.c: verify=sha3-224/256/384/512 옵션 처리.
 * - crc/test.c: --crctest=sha3-* 벤치마크 등록.
 * - inttypes.h: uint64_t/uint8_t 고정폭 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_sha3_ctx: Keccak 상태(st[25]), md_len, rate(rsiz), 잔여 버퍼.
 * - fio_sha3_{224,256,384,512}_init(): 변종별 rate/md_len 설정.
 * - fio_sha3_update(): 흡수 단계 — rate 바이트 블록이 차면 Keccak-f[1600] 호출.
 * - fio_sha3_final(): 도메인 분리 패딩(0x06) + trailing 0x80 후 최종 permute,
 *                    ctx->sha 에 md_len 바이트 출력(squeeze).
 */
#ifndef __CRYPTO_SHA3_H__
/* [한국어] 헤더 가드 — verify.c / sha3.c / test.c 동시 포함 대비. */
#define __CRYPTO_SHA3_H__

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: SHA-3 는 64비트 레인을 갖는 Keccak 상태를
 * uint64_t st[25] 로 유지하므로 고정폭 정수 정의 필수. uint8_t 는 입력 바이트/
 * 잔여 버퍼/출력 다이제스트용. */

/* [한국어] SHA3-224 다이제스트 크기: 28바이트. BLOCK_SIZE(rate) = 200 - 2*28 = 144바이트.
 * rate 가 클수록 capacity(c = 200-rate) 가 작아져 보안 강도가 낮지만 속도는 빨라진다.
 * 224 변종은 capacity 56바이트 = 448비트 → 반쪽인 224비트 보안 강도. */
#define SHA3_224_DIGEST_SIZE	(224 / 8)
#define SHA3_224_BLOCK_SIZE	(200 - 2 * SHA3_224_DIGEST_SIZE)

/* [한국어] SHA3-256 다이제스트 32바이트, rate 136바이트, capacity 64바이트(256비트 보안). */
#define SHA3_256_DIGEST_SIZE	(256 / 8)
#define SHA3_256_BLOCK_SIZE	(200 - 2 * SHA3_256_DIGEST_SIZE)

/* [한국어] SHA3-384 다이제스트 48바이트, rate 104바이트, capacity 96바이트(384비트 보안). */
#define SHA3_384_DIGEST_SIZE	(384 / 8)
#define SHA3_384_BLOCK_SIZE	(200 - 2 * SHA3_384_DIGEST_SIZE)

/* [한국어] SHA3-512 다이제스트 64바이트, rate 72바이트, capacity 128바이트(512비트 보안).
 * rate 가 가장 작아 단위 바이트당 Keccak-f 호출 빈도가 높으므로 가장 느린 변종. */
#define SHA3_512_DIGEST_SIZE	(512 / 8)
#define SHA3_512_BLOCK_SIZE	(200 - 2 * SHA3_512_DIGEST_SIZE)

/*
 * [한국어] Keccak 스펀지 계산 컨텍스트 — 4개 변종이 동일 구조 재사용
 * init_XXX 가 md_len/rsiz 를 변종별로 설정하는 것만 다르다.
 */
struct fio_sha3_ctx {
	uint64_t	st[25];
	/* [한국어] Keccak 상태: 5×5×64비트 = 1600비트의 permutation state.
	 * 설정자: init_XXX() 이 0 으로 리셋. update() 가 입력 워드를 XOR-흡수,
	 *        매 rate 바이트마다 keccakf() 가 Theta/Rho/Pi/Chi/Iota 24라운드 적용.
	 * 읽는 자: final() 이 st 앞 md_len 바이트를 ctx->sha 에 복사(squeeze 단계).
	 * 값 범위: 64비트 부호 없는 정수 25개. 각 라운드 결과에 의미.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */

	unsigned int	md_len;
	/* [한국어] 최종 출력 다이제스트 바이트 수(28/32/48/64 중 하나).
	 * 설정자: init_224/256/384/512() 가 각각 28/32/48/64 설정.
	 * 읽는 자: final() 이 st 를 md_len 바이트만큼 sha 에 직렬화. */

	unsigned int	rsiz;
	/* [한국어] rate 바이트 크기 = 200 - 2*md_len. 한 번에 흡수하는 바이트 수.
	 * capacity = 200 - rsiz (보안 강도 = capacity/2 비트). */

	unsigned int	rsizw;
	/* [한국어] rate 를 8로 나눈 값(64비트 워드 수). keccakf 흡수 시 루프 카운터. */

	unsigned int	partial;
	/* [한국어] 현재 buf[] 에 채워진 잔여 바이트 수(0 ~ rsiz-1). update() 가 갱신. */

	uint8_t		buf[SHA3_224_BLOCK_SIZE];
	/* [한국어] 블록 미만 잔여 데이터 임시 버퍼.
	 * 크기는 가장 큰 rate(SHA3-224 의 144바이트)로 할당 — 4변종 공통 사용.
	 * 설정자: update() 가 블록 미달 바이트 저장. final() 이 패딩 바이트 추가.
	 * 읽는 자: update()/final() 이 rate 바이트 차면 st 에 XOR 흡수. */

	uint8_t		*sha;
	/* [한국어] 최종 다이제스트 출력 버퍼 포인터(호출자 소유, 최소 md_len 바이트).
	 * 설정자: 호출자가 init 전에 ctx->sha = local_buf 로 지정.
	 * 읽는 자: final() 이 md_len 바이트 기록(squeeze 결과).
	 * 값 범위: 유효한 버퍼 포인터. NULL 금지. */
};

/*
 * [한국어]
 * fio_sha3_{224,256,384,512}_init - SHA-3 변종별 초기화
 * @sctx: 호출자 소유 컨텍스트. sctx->sha 가 출력 버퍼 가리켜야 함.
 * 동작: st[]=0, md_len = DIGEST_SIZE, rsiz = BLOCK_SIZE, rsizw = rsiz/8, partial=0.
 */
void fio_sha3_224_init(struct fio_sha3_ctx *sctx);
void fio_sha3_256_init(struct fio_sha3_ctx *sctx);
void fio_sha3_384_init(struct fio_sha3_ctx *sctx);
void fio_sha3_512_init(struct fio_sha3_ctx *sctx);

/*
 * [한국어]
 * fio_sha3_update - SHA-3 흡수 단계 (임의 길이 바이트 투입)
 * @sctx: 초기화된 컨텍스트. @data: 입력 버퍼. @len: 바이트 수.
 * @return: 성공 시 0(현 구현은 항상 0, 미래 확장 대비 반환).
 * 동작: buf 에 잔여 바이트 + 신규 데이터 합쳐 rate 바이트가 될 때마다
 *      8바이트씩 st[] 에 XOR 하고 keccakf() 호출.
 */
int fio_sha3_update(struct fio_sha3_ctx *sctx, const uint8_t *data,
		    unsigned int len);

/*
 * [한국어]
 * fio_sha3_final - SHA-3 최종 처리 (도메인 분리 패딩 + 압착)
 * @sctx: update 완료된 컨텍스트. 성공 후 sctx->sha 에 md_len 바이트 다이제스트.
 * 동작: 1) buf[partial]=0x06 (SHA-3 도메인 분리자, SHAKE 와 구별),
 *      2) buf[rsiz-1] |= 0x80 (padding 끝),
 *      3) 마지막 블록 흡수 후 keccakf(),
 *      4) st[] 의 첫 md_len 바이트를 sha 에 복사.
 */
void fio_sha3_final(struct fio_sha3_ctx *sctx);

#endif
