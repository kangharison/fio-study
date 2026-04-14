/*
 * [한국어 설명] SHA-256 해시 알고리즘 헤더 (sha256.h)
 *
 * === 파일의 역할 ===
 * SHA-256(Secure Hash Algorithm 256-bit) 해시 함수의 인터페이스를 정의한다.
 * SHA-2 계열로, 256비트(32바이트) 다이제스트를 생성하는 암호학적 해시 함수이다.
 * NIST FIPS 180-4 표준에 정의되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_sha256_init/update/final → sha256_transform (내부)
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=sha256 옵션 시 데이터 무결성 검증
 * - sha256.c: 구현
 * - crc/test.c: 벤치마크 테스트
 *
 * === 주요 구조체 ===
 * - fio_sha256_ctx: SHA-256 계산 상태
 */
#ifndef FIO_SHA256_H
#define FIO_SHA256_H

#include <inttypes.h>

/* [한국어] SHA-256 다이제스트 크기: 256비트 = 32바이트 */
#define SHA256_DIGEST_SIZE	32
/* [한국어] SHA-256 입력 블록 크기: 512비트 = 64바이트 */
#define SHA256_BLOCK_SIZE	64

struct fio_sha256_ctx {
	uint32_t count;
	/* 지금까지 처리한 총 바이트 수 */
	uint32_t state[SHA256_DIGEST_SIZE / 4];
	/* 8개의 32비트 해시 상태 워드 (a~h) - FIPS 180 초기값으로 시작 */
	uint8_t *buf;
	/* 외부에서 할당한 버퍼 포인터
	 * update()에서 블록 미만 데이터의 임시 저장소로 사용
	 * final() 후 최종 해시가 여기에 저장됨 */
};

void fio_sha256_init(struct fio_sha256_ctx *);
void fio_sha256_update(struct fio_sha256_ctx *, const uint8_t *, unsigned int);
void fio_sha256_final(struct fio_sha256_ctx *);

#endif
