/*
 * [한국어 설명] MD5 해시 알고리즘 헤더 (md5.h)
 *
 * === 파일의 역할 ===
 * MD5(Message-Digest Algorithm 5, RFC 1321) 해시 함수의 공개 API, 4 라운드
 * 비선형 함수 F1~F4, 단일 스텝 매크로 MD5STEP, 스트리밍 컨텍스트 구조체를
 * 선언한다. MD5 는 128비트(16바이트) 다이제스트를 생성하는 단방향 해시로,
 * 암호학적 용도에서는 충돌 공격으로 퇴역했으나 fio 의 verify=md5 는 무작위
 * 비트 플립 탐지를 목적으로 하므로 여전히 실용적이다. Linux 2.6 커널
 * crypto/md5.c 를 포팅한 구현이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_md5() / verify_io_u_md5()
 *     → fio_md5_init(ctx)
 *     → fio_md5_update(ctx, data, len)   // 임의 횟수
 *     → fio_md5_final(ctx)               // 16바이트 다이제스트가 ctx->hash 에 저장
 *   결과는 verify_header.v_md5 에 저장, 재검증 시 비교.
 *
 * === 타 모듈과의 연결 ===
 * - md5.c: 실제 구현(md5_transform — 64 스텝 4 라운드).
 * - verify.c: verify=md5 옵션의 fill/verify 경로.
 * - crc/test.c: --crctest=md5 벤치마크 등록.
 * - stdint.h: uint32_t/uint64_t/uint8_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - F1/F2/F3/F4 매크로: 각 라운드의 비선형 함수.
 * - MD5STEP 매크로: 한 스텝의 연산 패턴(add + rotate-left + add).
 * - struct fio_md5_ctx: 스트리밍 계산 상태.
 * - fio_md5_{init,update,final}(): 생명주기 API.
 */
#ifndef MD5_H
/* [한국어] 헤더 가드 — verify.c / md5.c / test.c 동시 포함 대비. */
#define MD5_H

#include <stdint.h>
/* [한국어] <stdint.h> 포함 이유: uint32_t(상태 워드, 블록 워드), uint64_t(byte_count),
 * uint8_t(입력 바이트) 고정폭 정수 공급. MD5 의 비트 폭이 알고리즘 정의의 핵심. */

/* [한국어] MD5 다이제스트 크기: 128비트 = 16바이트. hash 배열 4 워드. */
#define MD5_DIGEST_SIZE		16
/* [한국어] HMAC-MD5 용 블록 크기: 512비트 = 64바이트. 키 정규화(패딩/해시) 시 사용. */
#define MD5_HMAC_BLOCK_SIZE	64
/* [한국어] MD5 내부 블록의 32비트 워드 수: 512/32 = 16 워드. block[] 배열 크기. */
#define MD5_BLOCK_WORDS		16
/* [한국어] MD5 해시 상태의 32비트 워드 수: 128/32 = 4 워드 (A, B, C, D). */
#define MD5_HASH_WORDS		4

/*
 * [한국어] MD5 4개 라운드의 비선형 혼합 함수
 * 각 라운드(1~4)에서 서로 다른 비트 연산을 적용해 avalanche 성질 확보.
 * - F1 (라운드 1): 조건부 선택 — x 비트가 1이면 y, 0이면 z 선택.
 *                원래 정의: (x&y) | (~x&z) 이지만 (z ^ (x & (y ^ z))) 변형으로 연산 절감.
 * - F2 (라운드 2): F1 인자 순서를 바꾼 변형 — F1(z, x, y).
 * - F3 (라운드 3): 패리티 — 세 입력의 XOR (선형이지만 다른 라운드가 비선형 보완).
 * - F4 (라운드 4): y ^ (x | ~z) — OR 와 NOT 결합.
 */
#define F1(x, y, z)	(z ^ (x & (y ^ z)))
#define F2(x, y, z)	F1(z, x, y)
#define F3(x, y, z)	(x ^ y ^ z)
#define F4(x, y, z)	(y ^ (x | ~z))

/*
 * [한국어] MD5 한 스텝(64개 스텝 중 1개)을 수행하는 매크로
 * w = ((w + f(x,y,z) + in) rotl s) + x
 * RFC 1321 의 MD5 변환 단계 정의. f 는 F1~F4 중 라운드별 해당 함수,
 * in 은 메시지 워드 + 상수(sin 함수 기반), s 는 라운드별 회전량.
 */
#define MD5STEP(f, w, x, y, z, in, s) \
	(w += f(x, y, z) + in, w = (w<<s | w>>(32-s)) + x)

/*
 * [한국어] MD5 스트리밍 계산 컨텍스트
 * init → update(여러 번) → final 순서로 사용. 컨텍스트는 외부 소유 hash 배열에
 * 해시 상태를 저장하므로 호출자가 4 워드(16바이트) 버퍼를 미리 준비해야 한다.
 */
struct fio_md5_ctx {
	uint32_t *hash;
	/* [한국어] 해시 상태 포인터(4개의 uint32_t: A, B, C, D 를 가리킴).
	 * 설정자: 호출자가 init 전 ctx->hash = local_A_B_C_D_array 로 지정.
	 *        fio_md5_init() 이 RFC 1321 초기값(0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476) 적재.
	 * 읽는 자: md5_transform() 이 매 블록 64 스텝에서 읽고 갱신,
	 *         final() 이 패딩 후 최종 변환 → hash 앞 16바이트가 다이제스트.
	 * 값 범위: 유효한 4워드 버퍼 포인터. NULL 금지.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */

	uint32_t block[MD5_BLOCK_WORDS];
	/* [한국어] 현재 처리 중인 512비트(16워드) 입력 블록 버퍼.
	 * 설정자: update() 가 입력 바이트를 32비트 리틀엔디안 워드로 패킹해 채움.
	 *        블록 미만 잔여 바이트 임시 저장용으로도 사용.
	 * 읽는 자: md5_transform() 이 64 스텝에서 block[0..15] 를 메시지 워드로 참조.
	 * 값 범위: 32비트 부호 없는 정수 16개.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */

	uint64_t byte_count;
	/* [한국어] 지금까지 투입한 총 바이트 수(최대 2^64-1).
	 * 설정자: update() 가 호출 시마다 len 누적.
	 * 읽는 자: final() 이 패딩 시 비트 길이(byte_count * 8) 를 마지막 64비트에 삽입.
	 * 값 범위: 0 ~ 2^64-1. MD5 스펙 상한.
	 * 동기화: 잡 스레드 단독 — 락 불필요. */
};

/*
 * [한국어]
 * fio_md5_update - 임의 길이 바이트를 MD5 에 투입
 * @ctx: 초기화된 컨텍스트. @data: 바이트 버퍼. @len: 바이트 수.
 * 동작: block 에 리틀엔디안 워드 패킹 → 64바이트마다 md5_transform() 호출.
 */
extern void fio_md5_update(struct fio_md5_ctx *, const uint8_t *, unsigned int);

/*
 * [한국어]
 * fio_md5_final - 마지막 블록 패딩 및 다이제스트 확정
 * @ctx: update 완료된 컨텍스트. 완료 후 ctx->hash[0..3] 에 16바이트 해시.
 * 동작: 0x80 + 0 패딩 → 마지막 64비트에 비트 길이 → 최종 md5_transform() → hash 유지.
 */
extern void fio_md5_final(struct fio_md5_ctx *);

/*
 * [한국어]
 * fio_md5_init - MD5 컨텍스트 초기화
 * @ctx: 호출자 소유 컨텍스트. ctx->hash 가 4워드 버퍼 가리키고 있어야 함.
 * 동작: hash[0..3] 에 RFC 1321 초기 해시값 설정, byte_count=0.
 */
extern void fio_md5_init(struct fio_md5_ctx *);

#endif
