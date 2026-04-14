/*
 * Common values for SHA-3 algorithms
 */

/*
 * [한국어 설명] SHA-3 해시 알고리즘 헤더 (sha3.h)
 *
 * === 파일의 역할 ===
 * SHA-3(Keccak 기반) 해시 함수 계열의 인터페이스를 정의한다.
 * NIST FIPS 202 표준에 정의된 4개 변형을 지원한다:
 *   - SHA3-224: 224비트 다이제스트
 *   - SHA3-256: 256비트 다이제스트
 *   - SHA3-384: 384비트 다이제스트
 *   - SHA3-512: 512비트 다이제스트
 * SHA-3는 SHA-1/SHA-2와 완전히 다른 구조(스펀지 함수)를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_sha3_*_init → fio_sha3_update → fio_sha3_final
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=sha3-256 등의 옵션 시 호출
 * - sha3.c: Keccak-f[1600] 순열과 스펀지 함수 구현
 * - crc/test.c: 벤치마크 테스트
 *
 * === 주요 구조체 ===
 * - fio_sha3_ctx: Keccak 상태(25×64비트 = 1600비트)와 스펀지 파라미터
 */
#ifndef __CRYPTO_SHA3_H__
#define __CRYPTO_SHA3_H__

#include <inttypes.h>

/* [한국어] SHA3-224: 다이제스트 28바이트, 블록(rate) 144바이트 */
#define SHA3_224_DIGEST_SIZE	(224 / 8)
#define SHA3_224_BLOCK_SIZE	(200 - 2 * SHA3_224_DIGEST_SIZE)

#define SHA3_256_DIGEST_SIZE	(256 / 8)
#define SHA3_256_BLOCK_SIZE	(200 - 2 * SHA3_256_DIGEST_SIZE)

#define SHA3_384_DIGEST_SIZE	(384 / 8)
#define SHA3_384_BLOCK_SIZE	(200 - 2 * SHA3_384_DIGEST_SIZE)

#define SHA3_512_DIGEST_SIZE	(512 / 8)
#define SHA3_512_BLOCK_SIZE	(200 - 2 * SHA3_512_DIGEST_SIZE)

struct fio_sha3_ctx {
	uint64_t	st[25];
	/* Keccak 상태: 5×5×64비트 = 1600비트 상태 배열
	 * 스펀지 함수의 핵심 - 흡수(absorb)/압착(squeeze) 단계에서 갱신됨 */
	unsigned int	md_len;
	/* 출력 다이제스트 길이(바이트): 28(224), 32(256), 48(384), 64(512) */
	unsigned int	rsiz;
	/* rate(바이트): 200 - 2*md_len. 한 번에 흡수할 수 있는 데이터 크기
	 * capacity = 200 - rsiz (보안 강도를 결정) */
	unsigned int	rsizw;
	/* rate를 8로 나눈 값 (64비트 워드 수) */

	unsigned int	partial;
	/* 현재 버퍼에 채워진 바이트 수 (블록 미만 잔여 데이터) */
	uint8_t		buf[SHA3_224_BLOCK_SIZE];
	/* 블록 미만 데이터 임시 저장 버퍼
	 * 가장 큰 rate(SHA3-224의 144바이트)로 할당 */

	uint8_t		*sha;
	/* 최종 해시 출력을 저장할 외부 버퍼 포인터 */
};

void fio_sha3_224_init(struct fio_sha3_ctx *sctx);
void fio_sha3_256_init(struct fio_sha3_ctx *sctx);
void fio_sha3_384_init(struct fio_sha3_ctx *sctx);
void fio_sha3_512_init(struct fio_sha3_ctx *sctx);

int fio_sha3_update(struct fio_sha3_ctx *sctx, const uint8_t *data,
		    unsigned int len);
void fio_sha3_final(struct fio_sha3_ctx *sctx);

#endif
