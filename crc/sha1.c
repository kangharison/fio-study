/*
 * Based on the Mozilla SHA1 (see mozilla-sha1/sha1.c),
 * optimized to do word accesses rather than byte accesses,
 * and to avoid unnecessary copies into the context array.
 */

/*
 * [한국어 설명] SHA-1 해시 알고리즘 구현 (sha1.c)
 *
 * === 파일의 역할 ===
 * SHA-1(Secure Hash Algorithm 1)을 구현한다.
 * 임의 길이 입력 데이터를 160비트(20바이트) 해시로 변환한다.
 * FIPS 180 표준을 따르며, Mozilla SHA-1 기반으로 워드 단위 접근 최적화되었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 verify 기능에서 SHA-1 해시를 사용한 데이터 무결성 검증에 사용된다.
 * 호출 체인: verify.c → fio_sha1_init/update/final → blk_SHA1Block
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=sha1 옵션 시 호출
 * - sha1.h: 구조체와 인터페이스 정의
 * - crc/test.c: 벤치마크 테스트
 *
 * === 주요 함수 요약 ===
 * - fio_sha1_init(): FIPS 180 초기값으로 상태 초기화
 * - fio_sha1_update(): 데이터를 64바이트 블록 단위로 처리
 * - fio_sha1_final(): 패딩 추가 후 최종 해시 확정
 * - blk_SHA1Block(): 512비트 블록을 80스텝(4라운드)으로 처리하는 핵심 변환 함수
 *
 * === SHA-1 라운드 구조 ===
 * - 라운드 1 (T_0_15/T_16_19): (((C^D)&B)^D) + 0x5A827999
 * - 라운드 2 (T_20_39): (B^C^D) + 0x6ED9EBA1
 * - 라운드 3 (T_40_59): ((B&C)+(D&(B^C))) + 0x8F1BBCDC
 * - 라운드 4 (T_60_79): (B^C^D) + 0xCA62C1D6
 */

#include <string.h>
/* [한국어] memcpy: update 에서 블록 누적 시 사용. */
#include <arpa/inet.h>
/* [한국어] htonl: 길이 필드를 빅엔디안으로 인코딩(FIPS 180 규약) 시 사용. */

#include "sha1.h"
/* [한국어] sha1.h: fio_sha1_ctx 구조체(H[5], size, W[16]) 와 공용 API 선언. */

/* Hash one 64-byte block of data */
/* [한국어] 내부 전용 변환 함수의 전방 선언 — 본 파일 하단에서 정의. */
static void blk_SHA1Block(struct fio_sha1_ctx *ctx, const unsigned int *data);

/*
 * [한국어]
 * fio_sha1_init - FIPS 180 의 표준 초기값으로 SHA-1 컨텍스트 설정
 *
 * @ctx: 초기화 대상. size=0, H[0..4] 에 FIPS 180 매직 상수 기입.
 *
 * 호출 체인: verify.c / crc/test.c → [fio_sha1_init] → 초기화만
 */
void fio_sha1_init(struct fio_sha1_ctx *ctx)
{
	/* [한국어] 누적 바이트 수 0 — update 에서 증분·final 에서 길이 필드로 사용. */
	ctx->size = 0;

	/* Initialize H with the magic constants (see FIPS180 for constants)
	 */
	/* [한국어] FIPS 180 §5.3.1 의 160비트 초기 해시값 H0~H4. MD5 의 첫 4개와 같고
	 * H4=0xc3d2e1f0 이 추가되어 5워드(=160비트) 상태를 이룬다. */
	ctx->H[0] = 0x67452301;
	ctx->H[1] = 0xefcdab89;
	ctx->H[2] = 0x98badcfe;
	ctx->H[3] = 0x10325476;
	ctx->H[4] = 0xc3d2e1f0;
}

/*
 * [한국어]
 * fio_sha1_update - 입력 데이터를 64B 블록 단위로 누적·변환
 *
 * @ctx:  초기화된 컨텍스트.
 * @data: 입력 포인터.
 * @len:  바이트 길이.
 *
 * 동작:
 *   1) 이미 부분 블록이 있으면(=size%64!=0) 먼저 그 공간을 채워 full block 으로 만들고 transform.
 *   2) 이후 64B 단위로 반복 transform.
 *   3) 남은 <64B 는 W[] 에 저장해 다음 호출/final 에서 이어받음.
 *
 * 호출 체인: verify.c / test.c / fio_sha1_final(패딩 주입용 재호출) → [fio_sha1_update] → blk_SHA1Block
 */
void fio_sha1_update(struct fio_sha1_ctx *ctx, const void *data,
		     unsigned long len)
{
	/* [한국어] 현재 부분 블록의 오프셋(0..63). size & 63 == size % 64. */
	int lenW = ctx->size & 63;

	/* [한국어] 전체 누적 바이트 갱신. */
	ctx->size += len;

	/* Read the data into W and process blocks as they get full
	 */
	/* [한국어] 기존 부분 블록이 있으면 그 빈 공간부터 먼저 채운다. */
	if (lenW) {
		/* [한국어] 블록에 남은 공간 (0..64-lenW). */
		int left = 64 - lenW;
		/* [한국어] 새 데이터가 남은 공간보다 적으면 그만큼만 복사. */
		if (len < left)
			left = len;
		/* [한국어] W 의 lenW 오프셋부터 left 바이트 복사 — char* 캐스팅은 바이트 주소 연산용. */
		memcpy(lenW + (char *)ctx->W, data, left);
		/* [한국어] 오프셋 갱신 — 64 에 도달하면 wrap. */
		lenW = (lenW + left) & 63;
		/* [한국어] 소비한 만큼 len 감소. */
		len -= left;
		/* [한국어] data 포인터 전진. */
		data += left;
		/* [한국어] 블록이 아직 덜 찼으면 반환(다음 호출에서 이어감). */
		if (lenW)
			return;
		/* [한국어] 블록이 꽉 찼으므로 transform 으로 소화. */
		blk_SHA1Block(ctx, ctx->W);
	}
	/* [한국어] 64B 블록 단위로 반복 transform (data 가 정렬되었다고 가정). */
	while (len >= 64) {
		blk_SHA1Block(ctx, data);
		data += 64;
		len -= 64;
	}
	/* [한국어] 남은 <64B 는 W 선두부터 저장 — 다음 update/final 이 이어받음. */
	if (len)
		memcpy(ctx->W, data, len);
}

/*
 * [한국어]
 * fio_sha1_final - SHA-1 패딩과 길이 필드를 주입하여 최종 변환 완료
 *
 * @ctx: 컨텍스트. 종료 후 ctx->H 에 최종 160비트 해시(5 × 32비트 빅엔디안).
 *
 * 패딩 규약(FIPS 180): 메시지 끝에 0x80 바이트 하나 + 0 바이트들 + 64비트 빅엔디안
 * 길이(비트 단위). 패딩 길이는 (메시지 길이 + 1) mod 64 가 56 이 되도록 조정.
 *
 * 호출 체인: verify.c / test.c → [fio_sha1_final] → fio_sha1_update
 */
void fio_sha1_final(struct fio_sha1_ctx *ctx)
{
	/* [한국어] 패딩 배열 — 첫 바이트 0x80, 나머지는 0(부분 초기화자 규칙). */
	static const unsigned char pad[64] = { 0x80 };
	/* [한국어] 64비트 길이 필드 — 32비트 두 워드로 빅엔디안 표현. */
	unsigned int padlen[2];
	/* [한국어] 현재 부분 블록 오프셋 저장용. */
	int i;

	/* Pad with a binary 1 (ie 0x80), then zeroes, then length
	 */
	/* [한국어] 상위 32비트: size * 8 의 상위 비트 = size >> 29 (size 는 바이트 → 비트로 *8). */
	padlen[0] = htonl(ctx->size >> 29);
	/* [한국어] 하위 32비트: size * 8 = size << 3. */
	padlen[1] = htonl(ctx->size << 3);

	/* [한국어] 현재 블록 오프셋. */
	i = ctx->size & 63;
	/* [한국어] 0x80 + 0 패딩을 추가해 오프셋을 56 (mod 64) 로 맞춤.
	 * 길이: 1 + ((55 - i) mod 64). */
	fio_sha1_update(ctx, pad, 1+ (63 & (55 - i)));
	/* [한국어] 마지막 8바이트에 64비트 빅엔디안 길이 추가 — 자동으로 transform 트리거. */
	fio_sha1_update(ctx, padlen, 8);
}

#if defined(__i386__) || defined(__x86_64__)
/* [한국어] x86 에서는 rol/ror 명령을 직접 인라인 어셈블리로 사용해 32비트 회전을 가속.
 * 상수 시프트량 n 은 "i"(immediate) 제약으로 전달되어 컴파일타임에 명령어에 박힘. */
#define SHA_ASM(op, x, n) ({ unsigned int __res; __asm__(op " %1,%0":"=r" (__res):"i" (n), "0" (x)); __res; })
#define SHA_ROL(x,n)	SHA_ASM("rol", x, n)
#define SHA_ROR(x,n)	SHA_ASM("ror", x, n)

#else
/* [한국어] 다른 아키텍처(ARM/PPC 등)에서는 C 표현식으로 대체. 컴파일러는 보통 rol 인식. */
#define SHA_ROT(X,l,r)	(((X) << (l)) | ((X) >> (r)))
#define SHA_ROL(X,n)	SHA_ROT(X,n,32-(n))
#define SHA_ROR(X,n)	SHA_ROT(X,32-(n),n)

#endif

/* This "rolls" over the 512-bit array */
/* [한국어] W(x) = array[x mod 16] — 80 스텝 메시지 스케줄을 16워드 링버퍼로 구현.
 * setW(x, val) 는 volatile 캐스팅으로 컴파일러의 스토어 제거·재배치 금지(과거 GCC 버그 회피). */
#define W(x) (array[(x)&15])
#define setW(x, val) (*(volatile unsigned int *)&W(x) = (val))

/*
 * Where do we get the source from? The first 16 iterations get it from
 * the input data, the next mix it from the 512-bit array.
 */
/* [한국어] SHA_SRC(t): 메시지 스케줄 첫 16스텝은 입력 data[t] 를 빅엔디안으로 읽음.
 * SHA_MIX(t): 이후 16..79 스텝은 W(t+13) ^ W(t+8) ^ W(t+2) ^ W(t) 의 1비트 좌회전으로 유도. */
#define SHA_SRC(t) htonl(data[t])
#define SHA_MIX(t) SHA_ROL(W(t+13) ^ W(t+8) ^ W(t+2) ^ W(t), 1)

/* [한국어] SHA_ROUND: 한 스텝의 공통 코드 — TEMP 에 메시지 워드 저장(새 스케줄 슬롯에도 저장),
 * E += TEMP + rol(A,5) + f(B,C,D) + K; B = ror(B, 2). (A..E 는 스텝마다 로테이트 호출로 순환.) */
#define SHA_ROUND(t, input, fn, constant, A, B, C, D, E) do { \
	unsigned int TEMP = input(t); setW(t, TEMP); \
	E += TEMP + SHA_ROL(A,5) + (fn) + (constant); \
	B = SHA_ROR(B, 2); } while (0)

/* [한국어] 4라운드별 비선형 함수(fn) + 상수(K) 매크로. FIPS 180 §6.1 표.
 *   Round 1 (0-19):  f = Ch(B,C,D) = (B&C)|(~B&D), K = 0x5a827999  — T_0_15/T_16_19 로 나뉨.
 *   Round 2 (20-39): f = Parity(B,C,D) = B^C^D,    K = 0x6ed9eba1.
 *   Round 3 (40-59): f = Maj(B,C,D) = (B&C)|(C&D)|(B&D),  K = 0x8f1bbcdc.
 *                    — 본 구현은 (B&C)+(D&(B^C)) 형태로 동치지만 XOR+AND 연산으로 더 빠름.
 *   Round 4 (60-79): f = Parity,                   K = 0xca62c1d6. */
#define T_0_15(t, A, B, C, D, E)  SHA_ROUND(t, SHA_SRC, (((C^D)&B)^D) , 0x5a827999, A, B, C, D, E )
#define T_16_19(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, (((C^D)&B)^D) , 0x5a827999, A, B, C, D, E )
#define T_20_39(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, (B^C^D) , 0x6ed9eba1, A, B, C, D, E )
#define T_40_59(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, ((B&C)+(D&(B^C))) , 0x8f1bbcdc, A, B, C, D, E )
#define T_60_79(t, A, B, C, D, E) SHA_ROUND(t, SHA_MIX, (B^C^D) ,  0xca62c1d6, A, B, C, D, E )

/*
 * [한국어]
 * blk_SHA1Block - 512비트(64B) 블록 1개 SHA-1 변환
 *
 * @ctx:  컨텍스트(H[5] in/out).
 * @data: 64바이트 블록 포인터(빅엔디안 입력).
 *
 * 동작: 레지스터 A/B/C/D/E 를 H[] 에서 복사해와 80 스텝(4라운드 × 20) 적용 후
 * H[] 에 누적 덧셈(Merkle-Damgård 방식).
 *
 * 호출 체인: fio_sha1_update → [blk_SHA1Block]
 */
static void blk_SHA1Block(struct fio_sha1_ctx *ctx, const unsigned int *data)
{
	/* [한국어] 5개의 32비트 작업 레지스터 — 매 스텝에서 회전된 이름으로 호출되어 순환. */
	unsigned int A,B,C,D,E;
	/* [한국어] 16워드 메시지 스케줄 링버퍼 — W(t) 매크로가 array[t & 15] 로 접근. */
	unsigned int array[16];

	/* [한국어] 기존 chaining value 적재. */
	A = ctx->H[0];
	B = ctx->H[1];
	C = ctx->H[2];
	D = ctx->H[3];
	E = ctx->H[4];

	/* Round 1 - iterations 0-16 take their input from 'data' */
	/* [한국어] 스텝 0~15: 메시지 워드를 data 에서 직접 읽는 T_0_15. 레지스터 인자가 매 스텝
	 * 한 자리씩 회전하는 이유는 SHA-1 스펙의 "A=rol(A,5) + ...; E=D; D=C; C=rol(B,30); B=A;"
	 * 대입 시퀀스를 매크로 인자 순서 회전으로 표현한 최적화. */
	T_0_15( 0, A, B, C, D, E);
	T_0_15( 1, E, A, B, C, D);
	T_0_15( 2, D, E, A, B, C);
	T_0_15( 3, C, D, E, A, B);
	T_0_15( 4, B, C, D, E, A);
	T_0_15( 5, A, B, C, D, E);
	T_0_15( 6, E, A, B, C, D);
	T_0_15( 7, D, E, A, B, C);
	T_0_15( 8, C, D, E, A, B);
	T_0_15( 9, B, C, D, E, A);
	T_0_15(10, A, B, C, D, E);
	T_0_15(11, E, A, B, C, D);
	T_0_15(12, D, E, A, B, C);
	T_0_15(13, C, D, E, A, B);
	T_0_15(14, B, C, D, E, A);
	T_0_15(15, A, B, C, D, E);

	/* Round 1 - tail. Input from 512-bit mixing array */
	/* [한국어] 스텝 16~19: 메시지 스케줄(W) 에서 워드를 가져오는 T_16_19. 비선형 함수는 Round 1 과 동일. */
	T_16_19(16, E, A, B, C, D);
	T_16_19(17, D, E, A, B, C);
	T_16_19(18, C, D, E, A, B);
	T_16_19(19, B, C, D, E, A);

	/* Round 2 */
	/* [한국어] 스텝 20~39: Parity(B,C,D) 비선형, K=0x6ed9eba1. */
	T_20_39(20, A, B, C, D, E);
	T_20_39(21, E, A, B, C, D);
	T_20_39(22, D, E, A, B, C);
	T_20_39(23, C, D, E, A, B);
	T_20_39(24, B, C, D, E, A);
	T_20_39(25, A, B, C, D, E);
	T_20_39(26, E, A, B, C, D);
	T_20_39(27, D, E, A, B, C);
	T_20_39(28, C, D, E, A, B);
	T_20_39(29, B, C, D, E, A);
	T_20_39(30, A, B, C, D, E);
	T_20_39(31, E, A, B, C, D);
	T_20_39(32, D, E, A, B, C);
	T_20_39(33, C, D, E, A, B);
	T_20_39(34, B, C, D, E, A);
	T_20_39(35, A, B, C, D, E);
	T_20_39(36, E, A, B, C, D);
	T_20_39(37, D, E, A, B, C);
	T_20_39(38, C, D, E, A, B);
	T_20_39(39, B, C, D, E, A);

	/* Round 3 */
	/* [한국어] 스텝 40~59: Maj(B,C,D) 비선형, K=0x8f1bbcdc. */
	T_40_59(40, A, B, C, D, E);
	T_40_59(41, E, A, B, C, D);
	T_40_59(42, D, E, A, B, C);
	T_40_59(43, C, D, E, A, B);
	T_40_59(44, B, C, D, E, A);
	T_40_59(45, A, B, C, D, E);
	T_40_59(46, E, A, B, C, D);
	T_40_59(47, D, E, A, B, C);
	T_40_59(48, C, D, E, A, B);
	T_40_59(49, B, C, D, E, A);
	T_40_59(50, A, B, C, D, E);
	T_40_59(51, E, A, B, C, D);
	T_40_59(52, D, E, A, B, C);
	T_40_59(53, C, D, E, A, B);
	T_40_59(54, B, C, D, E, A);
	T_40_59(55, A, B, C, D, E);
	T_40_59(56, E, A, B, C, D);
	T_40_59(57, D, E, A, B, C);
	T_40_59(58, C, D, E, A, B);
	T_40_59(59, B, C, D, E, A);

	/* Round 4 */
	/* [한국어] 스텝 60~79: Parity(B,C,D) 비선형, K=0xca62c1d6. */
	T_60_79(60, A, B, C, D, E);
	T_60_79(61, E, A, B, C, D);
	T_60_79(62, D, E, A, B, C);
	T_60_79(63, C, D, E, A, B);
	T_60_79(64, B, C, D, E, A);
	T_60_79(65, A, B, C, D, E);
	T_60_79(66, E, A, B, C, D);
	T_60_79(67, D, E, A, B, C);
	T_60_79(68, C, D, E, A, B);
	T_60_79(69, B, C, D, E, A);
	T_60_79(70, A, B, C, D, E);
	T_60_79(71, E, A, B, C, D);
	T_60_79(72, D, E, A, B, C);
	T_60_79(73, C, D, E, A, B);
	T_60_79(74, B, C, D, E, A);
	T_60_79(75, A, B, C, D, E);
	T_60_79(76, E, A, B, C, D);
	T_60_79(77, D, E, A, B, C);
	T_60_79(78, C, D, E, A, B);
	T_60_79(79, B, C, D, E, A);

	/* [한국어] Merkle-Damgård 누적: chaining value 에 이번 블록 결과를 더한다. */
	ctx->H[0] += A;
	ctx->H[1] += B;
	ctx->H[2] += C;
	ctx->H[3] += D;
	ctx->H[4] += E;
}
