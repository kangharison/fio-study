/*
 * [한국어 설명] SHA-512 해시 알고리즘 헤더 (sha512.h)
 *
 * === 파일의 역할 ===
 * SHA-512(Secure Hash Algorithm 512-bit) 해시 함수의 인터페이스를 정의한다.
 * SHA-2 계열로, 512비트(64바이트) 다이제스트를 생성하는 암호학적 해시 함수이다.
 * 1024비트 입력 블록을 80스텝으로 처리하며 64비트 연산을 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_sha512_init/update/final → sha512_transform (내부)
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=sha512 옵션 시 데이터 무결성 검증
 * - sha512.c: 구현
 *
 * === 주요 구조체 ===
 * - fio_sha512_ctx: SHA-512 계산 상태
 */
#ifndef FIO_SHA512_H
#define FIO_SHA512_H

#include <inttypes.h>

struct fio_sha512_ctx {
	uint64_t state[8];
	/* 8개의 64비트 해시 상태 워드 (a~h) */
	uint32_t count[4];
	/* 처리한 비트 수 (128비트 카운터를 32비트 4개로 표현) */
	uint8_t *buf;
	/* 외부 할당 버퍼 - 임시 저장 및 최종 해시 출력용 */
	uint64_t W[80];
	/* 메시지 스케줄 배열 (80개의 64비트 워드) */
};

void fio_sha512_init(struct fio_sha512_ctx *);
void fio_sha512_update(struct fio_sha512_ctx *, const uint8_t *, unsigned int);
void fio_sha512_final(struct fio_sha512_ctx *sctx);

#endif
