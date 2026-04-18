/*
 * [한국어 설명] SHA-1 해시 알고리즘 헤더 (sha1.h)
 *
 * === 파일의 역할 ===
 * SHA-1(Secure Hash Algorithm 1, FIPS 180-4 Legacy) 해시 함수의 공개 API 를
 * 정의한다. 160비트(20바이트) 다이제스트를 생성하며 512비트(64바이트) 입력
 * 블록을 80 라운드로 처리한다. Mozilla SHA-1 구현 기반으로 32비트 워드 단위
 * 접근과 컨텍스트 배열 복사 최소화 최적화를 적용했다. SHA-1 은 충돌 공격으로
 * 암호학적 용도에서 퇴역했으나 fio 의 verify=sha1 은 데이터 무결성 검증(공격자
 * 모델이 없는 랜덤 오류 탐지) 용도라 여전히 유효.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_sha1() / verify_io_u_sha1()
 *     → fio_sha1_init(ctx)
 *     → fio_sha1_update(ctx, data, len)    // 임의 횟수
 *     → fio_sha1_final(ctx)                // 20바이트 다이제스트가 ctx->H 에 저장
 *   결과는 verify_header.v_sha1 에 저장되어 재검증 시 비교.
 *
 * === 타 모듈과의 연결 ===
 * - sha1.c: 실제 구현(blk_SHA1Block — 80스텝 4라운드 F_0_19/F_20_39/F_40_59/F_60_79).
 * - verify.c: verify=sha1 옵션의 fill/verify 경로.
 * - crc/test.c: --crctest=sha1 벤치마크 등록.
 * - inttypes.h: uint32_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_sha1_ctx: SHA-1 계산 상태(H[5], W[16] 메시지 스케줄 링, size).
 * - fio_sha1_init(): H[0..4] 에 SHA-1 초기 해시값 설정.
 * - fio_sha1_update(): 데이터 바이트 누적 투입.
 * - fio_sha1_final(): 패딩 + 길이 필드 → 20바이트 다이제스트를 H 에 남김.
 */
#ifndef FIO_SHA1
/* [한국어] 헤더 가드 — verify.c / sha1.c / test.c 동시 포함 대비. */
#define FIO_SHA1

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: uint32_t 고정폭 정수 정의. SHA-1 의 H, W 는
 * 32비트 워드 연산이 알고리즘 정의의 핵심이라 플랫폼 의존 피하기 위함. */

/*
 * Based on the Mozilla SHA1 (see mozilla-sha1/sha1.h),
 * optimized to do word accesses rather than byte accesses,
 * and to avoid unnecessary copies into the context array.
 */

/*
 * [한국어] SHA-1 스트리밍 계산 컨텍스트
 * 호출자가 외부 버퍼(uint32_t H[5])를 할당하여 ctx->H 에 연결해둔다.
 * init → update(여러 번) → final 순서로 사용, final 후 H 앞 20바이트가 다이제스트.
 */
struct fio_sha1_ctx {
	uint32_t *H;
	/* [한국어] 해시 상태 포인터 — 5개의 uint32_t(H0~H4) 배열을 가리킴.
	 * 설정자: 호출자가 init 호출 전에 ctx->H = local_H_array 로 지정(외부 소유).
	 *        fio_sha1_init() 이 H[0..4] 에 SHA-1 초기 해시값
	 *        (0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0) 적재.
	 * 읽는 자: 내부 blk_SHA1Block() 각 블록 라운드에서 읽고 갱신,
	 *         final() 종료 후 호출자가 H 앞 20바이트(5 워드)를 다이제스트로 복사.
	 * 값 범위: 유효한 20바이트(5×uint32_t) 버퍼 포인터. NULL 금지.
	 * 동기화: 잡 스레드 단독 소유 — 락 불필요. */

	unsigned int W[16];
	/* [한국어] 512비트(16워드) 메시지 스케줄 링버퍼.
	 * 설정자: update() 가 입력 바이트를 32비트 워드로 패킹하여 W[0..15] 채움.
	 *        blk_SHA1Block() 이 원격 스케줄 W(t) = rotl(W(t-3)^W(t-8)^W(t-14)^W(t-16), 1)
	 *        을 W[t & 15] 에 오버라이트하며 링으로 재사용(메모리 절약).
	 *        블록 미만 잔여 바이트 저장소로도 겸용.
	 * 읽는 자: blk_SHA1Block() 각 라운드 t 에서 W[t & 15] 조회.
	 * 값 범위: 32비트 부호 없는 정수.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */

	unsigned long long size;
	/* [한국어] 지금까지 투입한 총 바이트 수(unsigned long long — 64비트).
	 * 설정자: update() 가 호출 시마다 len 누적.
	 * 읽는 자: final() 이 패딩 시 비트 길이(size * 8) 로 환산해 마지막 64비트에 삽입.
	 * 값 범위: 0 ~ 2^64-1(실질 무한). SHA-1 스펙 상한.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */
};

/*
 * [한국어]
 * fio_sha1_init - SHA-1 컨텍스트 초기화
 * @ctx: 호출자 소유 컨텍스트. ctx->H 가 5워드 버퍼를 가리키고 있어야 함.
 * 동작: H[0..4] 를 SHA-1 표준 초기값으로 설정, size=0, W[] 는 lazy 초기화.
 */
void fio_sha1_init(struct fio_sha1_ctx *);

/*
 * [한국어]
 * fio_sha1_update - 임의 길이 바이트를 SHA-1 에 투입
 * @ctx:    초기화된 컨텍스트.
 * @dataIn: 바이트 버퍼(void* — 호출자가 원하는 타입으로 전달 가능).
 * @len:    바이트 수(unsigned long — 플랫폼 기본 폭).
 * 동작: W[] 에 워드 패킹 후 64바이트 블록이 완성되면 blk_SHA1Block() 호출, size 누적.
 */
void fio_sha1_update(struct fio_sha1_ctx *, const void *dataIn, unsigned long len);

/*
 * [한국어]
 * fio_sha1_final - 마지막 블록 패딩 및 다이제스트 생성
 * @ctx: update 를 완료한 컨텍스트. 완료 후 ctx->H[0..4] 에 20바이트 해시.
 * 동작: 1) 0x80 + 0 패딩, 2) 마지막 64비트에 비트 길이, 3) 최종 blk_SHA1Block(),
 *      4) H[] 는 이미 빅엔디안 워드로 저장되므로 별도 직렬화 없이 사용 가능.
 * 호출 후 ctx 는 재사용 금지(init 재호출 필요).
 */
void fio_sha1_final(struct fio_sha1_ctx *);

#endif
