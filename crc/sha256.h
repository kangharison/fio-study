/*
 * [한국어 설명] SHA-256 해시 알고리즘 헤더 (sha256.h)
 *
 * === 파일의 역할 ===
 * SHA-256(FIPS 180-4) 해시 함수의 공개 API 를 정의한다. SHA-2 계열의 중심
 * 변종으로, 256비트(32바이트) 다이제스트를 생성하며 512비트(64바이트) 입력
 * 블록을 64 라운드로 처리한다. 32비트 워드 연산 기반이라 32/64비트 플랫폼
 * 모두에서 균형 잡힌 성능을 낸다. fio 에서는 verify=sha256 모드의 데이터 무결성
 * 검증에 사용되며, 서버 프로토콜의 AWS SigV4 HMAC-SHA256 체인에도 간접 활용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_sha256() / verify_io_u_sha256()
 *     → fio_sha256_init(ctx)
 *     → fio_sha256_update(ctx, data, len)    // 임의 횟수
 *     → fio_sha256_final(ctx)                // 32바이트 다이제스트 생성
 *   결과는 verify_header.v_sha256 필드에 저장 후 재검증 시 비교.
 *   engines/http.c 의 AWS SigV4 서명 체인도 간접적으로 동일 API 사용
 *   (_gen_hex_sha256/_hmac 등을 통해).
 *
 * === 타 모듈과의 연결 ===
 * - sha256.c: 실제 구현(sha256_transform 64 라운드, H0~H7 FIPS 초기값).
 * - verify.c: verify=sha256 옵션 처리.
 * - engines/http.c: AWS SigV4 서명용 간접 사용.
 * - crc/test.c: --crctest=sha256 벤치마크 등록.
 * - inttypes.h: uint32_t/uint8_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_sha256_ctx: 스트리밍 SHA-256 컨텍스트(8개 state + count + buf).
 * - fio_sha256_init(): FIPS 180-4 초기값(H0~H7) 적재.
 * - fio_sha256_update(): 임의 길이 바이트 투입 → 512비트 블록 단위 변환.
 * - fio_sha256_final(): 패딩(0x80 + 0 + 64비트 길이) 후 buf 에 32바이트 기록.
 */
#ifndef FIO_SHA256_H
/* [한국어] 헤더 가드 — verify.c / sha256.c / http.c / test.c 동시 포함 대비. */
#define FIO_SHA256_H

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: uint32_t(상태 워드, count), uint8_t(바이트 입력/출력)
 * 고정폭 정수 공급. FIPS 180-4 의 비트-폭 정의가 절대적이라 이식성에 필수. */

/* [한국어] SHA-256 다이제스트 크기: 256비트 = 32바이트.
 * 사용처: struct fio_sha256_ctx.state[] 배열 크기(SHA256_DIGEST_SIZE/4 = 8),
 *       호출자의 출력 버퍼(ctx->buf) 최소 크기 검증용. */
#define SHA256_DIGEST_SIZE	32
/* [한국어] SHA-256 입력 블록 크기: 512비트 = 64바이트. HMAC 키 길이 정규화 시 사용. */
#define SHA256_BLOCK_SIZE	64

/*
 * [한국어] SHA-256 스트리밍 계산 컨텍스트
 * init → update(여러 번) → final 순서로 사용. 최종 결과는 ctx->buf 에 배치.
 */
struct fio_sha256_ctx {
	uint32_t count;
	/* [한국어] 지금까지 처리한 총 바이트 수(비트가 아님 — 내부 구현이 8배 곱함).
	 * 설정자: update() 가 호출 시마다 len 누적. final() 이 패딩 시 비트 수(×8)로 환산.
	 * 읽는 자: update()/final() 내부 패딩 계산.
	 * 값 범위: 0 ~ 2^32-1(4 GiB). verify 블록은 훨씬 작으므로 실질적 제한 없음.
	 * 주의: SHA-512 는 128비트 카운터(count[4])를 쓰지만 SHA-256 은 32비트만으로
	 *       충분해 본 구현이 단일 uint32_t 사용. 2^32 바이트 초과 입력 시 overflow
	 *       발생 — fio verify 블록 크기에서는 발생 불가.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */

	uint32_t state[SHA256_DIGEST_SIZE / 4];
	/* [한국어] 8개의 32비트 해시 상태 워드(a~h = H0~H7). SHA256_DIGEST_SIZE/4 = 8.
	 * 설정자: fio_sha256_init() 이 FIPS 180-4 초기 해시값(소수 제곱근 소수 부분)으로 초기화.
	 * 읽는 자: sha256_transform() 매 블록 64 라운드마다 읽고 갱신, final() 이 직렬화.
	 * 값 범위: 32비트 부호 없는 정수. 중간 상태.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */

	uint8_t *buf;
	/* [한국어] 외부에서 할당한 이중 용도 버퍼 포인터(최소 SHA256_BLOCK_SIZE=64 바이트).
	 * 설정자: 호출자가 init 전에 ctx->buf = stack_buffer 등으로 지정.
	 * 읽는 자: update() 가 블록 미만 잔여 바이트 저장소로 이용.
	 *         final() 이 완료 후 32바이트 다이제스트를 이 버퍼 앞부분에 기록.
	 * 값 범위: 유효한 버퍼 포인터(NULL 금지). 버퍼 크기는 구현 요구 최소 이상.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */
};

/*
 * [한국어]
 * fio_sha256_init - SHA-256 컨텍스트 초기화
 * @ctx: 호출자 소유 컨텍스트. ctx->buf 가 유효한 버퍼를 가리키고 있어야 함.
 * 동작: state[0..7] 에 FIPS 180-4 SHA-256 초기 해시값 설정, count=0.
 * 호출 체인: verify.c::fill_sha256()/verify_io_u_sha256() → fio_sha256_init()
 * 실행 컨텍스트: 잡 스레드. 에러: 없음.
 */
void fio_sha256_init(struct fio_sha256_ctx *);

/*
 * [한국어]
 * fio_sha256_update - 임의 길이 바이트를 SHA-256 에 투입
 * @ctx:  초기화된 컨텍스트.
 * @data: 투입할 바이트 버퍼.
 * @len:  바이트 수(unsigned int — 최대 4 GiB).
 * 동작: 64바이트 블록이 찰 때마다 sha256_transform() 호출, 나머지는 ctx->buf 에 보관.
 */
void fio_sha256_update(struct fio_sha256_ctx *, const uint8_t *, unsigned int);

/*
 * [한국어]
 * fio_sha256_final - 마지막 블록 패딩 및 다이제스트 출력
 * @ctx: update 를 완료한 컨텍스트. 완료 후 ctx->buf[0..31] 에 해시가 배치됨.
 * 동작: 1) 0x80 + 0 패딩, 2) 마지막 64비트에 비트 길이, 3) 최종 transform 호출,
 *      4) state[0..7] 빅엔디안 직렬화하여 buf 에 기록.
 * 호출 후 ctx 는 재사용 금지(init 재호출 필요).
 */
void fio_sha256_final(struct fio_sha256_ctx *);

#endif
