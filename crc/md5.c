/*
 * Shamelessly lifted from the 2.6 kernel (crypto/md5.c)
 */

/*
 * [한국어 설명] MD5 해시 알고리즘 구현 (md5.c)
 *
 * === 파일의 역할 ===
 * RFC 1321 규격 MD5(Message-Digest Algorithm 5) 를 구현한다. 128비트 다이제스트를
 * 생성하는 Merkle-Damgård 구조 해시로, 입력을 512비트(64B) 블록 단위로 나눠
 * "64스텝 = 4라운드 × 16스텝" 의 비선형 혼합 함수(md5_transform)를 적용한다.
 * 스텝 상수(0xd76aa478 …)는 sin 함수에서 도출된 RFC 1321 고정값이고, 각 라운드는
 * 서로 다른 비선형 함수(F1~F4, md5.h 매크로)와 좌회전 비트 수를 사용한다.
 * 출력은 리틀엔디안 4 × 32비트 워드(A/B/C/D). 현재는 암호 안전성이 깨져 있어
 * 암호 용도로는 사용하지 않지만, fio 에서는 "저렴하고 강한 체크섬" 용도이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 의 VERIFY_MD5 경로.
 * 쓰기: fill_md5(verify.c) → fio_md5_init → fio_md5_update → fio_md5_final.
 *       결과 hash[4] 를 verify_header.v_md5 (16B) 에 저장.
 * 읽기: verify_io_u_md5 가 동일 순서로 재계산 후 헤더와 비교.
 * 호출 체인:
 *   verify.c::fill_md5/verify_io_u_md5 → fio_md5_* → md5_transform
 *   crc/test.c::t_md5 → 동일 시퀀스
 *
 * === 타 모듈과의 연결 ===
 * - md5.h: fio_md5_ctx 구조체(hash[4], block[MD5_BLOCK_WORDS=16], byte_count),
 *   F1~F4 비선형 함수 매크로, MD5STEP(f, w, x, y, z, in, s) 매크로(a = b + rol(a + f(...) + in, s)).
 * - string.h: memcpy/memset — 블록 누적 및 패딩 clear 용.
 * - verify.c / crc/test.c: 호출자.
 * 데이터 흐름: 쓰기 버퍼 → md5_update → ... → hash[4] (16B).
 * 동기화: fio_md5_ctx 는 호출자가 소유 — 컨텍스트 단위 독립, 락 불요.
 * md5_transform 의 상수는 매크로 리터럴이라 읽기 전용.
 *
 * === 주요 함수 요약 ===
 * - md5_transform(hash, in): 512비트 블록 1개 처리 — 4라운드 × 16 MD5STEP.
 * - fio_md5_init(mctx): RFC 1321 초기 벡터(A=0x67452301 등)로 hash[4] 설정.
 * - fio_md5_update(mctx, data, len): 블록 단위 누적. 64B 미달분은 block[] 에 대기.
 * - fio_md5_final(mctx): 0x80 패딩 + 56mod64 정렬 + 64비트 길이 추가 → 최종 transform.
 */
#include <string.h>
/* [한국어] memcpy/memset: update/final 에서 블록 누적·패딩 clear 에 필요. */
#include "md5.h"
/* [한국어] md5.h: fio_md5_ctx 구조체 · F1~F4 비선형 매크로 · MD5STEP 매크로 공급. */

/*
 * [한국어]
 * md5_transform - 512비트(16워드) 입력 블록 하나를 처리하는 MD5 핵심 변환 함수
 *
 * @hash: 현재 해시 상태 배열 (4개의 uint32_t: A, B, C, D)
 * @in: 처리할 512비트 입력 블록 (16개의 uint32_t 워드)
 *
 * MD5는 4개의 라운드로 구성되며, 각 라운드는 16개의 스텝을 수행한다:
 *   - 라운드 1 (스텝 0~15):  F1 함수 사용 - 조건부 선택
 *   - 라운드 2 (스텝 16~31): F2 함수 사용 - F1의 변형
 *   - 라운드 3 (스텝 32~47): F3 함수 사용 - XOR 패리티
 *   - 라운드 4 (스텝 48~63): F4 함수 사용 - OR/NOT 결합
 * 각 스텝에서 MD5STEP 매크로가 비선형 함수 + 입력 워드 + 상수를 결합하고
 * 좌측 순환 시프트를 적용한다. 상수값(0xd76aa478 등)은 sin 함수에서 도출된
 * RFC 1321 규격값이다.
 * 최종적으로 변환 결과를 기존 해시 상태에 누적(+=)한다.
 *
 * 호출 체인:
 *   fio_md5_update()/fio_md5_final() → [md5_transform] → (내부 연산)
 */
static void md5_transform(uint32_t *hash, uint32_t const *in)
{
	/* [한국어] 작업 레지스터 A/B/C/D — hash[4] 에서 복사해와 64스텝 동안 갱신. */
	uint32_t a, b, c, d;

	/* [한국어] 이전 블록까지의 chaining value 를 레지스터에 적재. */
	a = hash[0];
	b = hash[1];
	c = hash[2];
	d = hash[3];

	/* [한국어] Round 1 (0~15): F1(x,y,z) = (x & y) | (~x & z) — 조건부 선택자.
	 * MD5STEP(f, a, b, c, d, in_const, s):
	 *   a = b + rol(a + f(b,c,d) + in_const, s)
	 * s 는 좌회전 비트 수(표준 7/12/17/22 사이클). 각 in[i] + 상수 는 메시지 워드와
	 * RFC 1321 의 sin 기반 상수의 합. */
	MD5STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
	MD5STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
	MD5STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
	MD5STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
	MD5STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
	MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

	/* [한국어] Round 2 (16~31): F2(x,y,z) = (x & z) | (y & ~z).
	 * 메시지 워드 순열이 (5i+1) mod 16 로 바뀌어 in 접근이 순차적이지 않음. */
	MD5STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
	MD5STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
	MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
	MD5STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
	MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
	MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
	MD5STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
	MD5STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
	MD5STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

	/* [한국어] Round 3 (32~47): F3(x,y,z) = x ^ y ^ z — 삼중 XOR(패리티). 순열 (3i+5) mod 16. */
	MD5STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
	MD5STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
	MD5STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
	MD5STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
	MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);

	/* [한국어] Round 4 (48~63): F4(x,y,z) = y ^ (x | ~z). 순열 (7i) mod 16. */
	MD5STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
	MD5STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
	MD5STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
	MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
	MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);

	/* [한국어] 변환 결과를 기존 해시 상태에 누적 - Merkle-Damgård 구조의 핵심 */
	hash[0] += a;
	hash[1] += b;
	hash[2] += c;
	hash[3] += d;
}

/*
 * [한국어]
 * fio_md5_init - MD5 컨텍스트를 초기 상태로 설정
 *
 * @mctx: 초기화할 MD5 컨텍스트. hash 포인터는 호출자가 미리 할당해야 함
 *
 * RFC 1321에 정의된 4개의 매직 상수로 해시 상태(A,B,C,D)를 초기화한다.
 * 이 값들은 MD5 알고리즘의 표준 초기값(IV, Initialization Vector)이다.
 *
 * 호출 체인:
 *   verify.c (verify_io_u_md5) → [fio_md5_init] → 해시 상태 설정
 */
void fio_md5_init(struct fio_md5_ctx *mctx)
{
	/* [한국어] RFC 1321 §3.3 의 표준 초기값(IV) A/B/C/D. "매직 상수" 로 불리는 이유는
	 * 작은 정수의 16진 표현(01234567..) 을 리틀엔디안 바이트 순서로 쌓아 얻은 값. */
	mctx->hash[0] = 0x67452301;
	mctx->hash[1] = 0xefcdab89;
	mctx->hash[2] = 0x98badcfe;
	mctx->hash[3] = 0x10325476;
	/* [한국어] (주의) byte_count/block[] 은 호출자가 0 초기화해야 함 — fio 는 스택에서 .hash 만
	 * 바인딩한 구조체 초기화자({.hash = digest}) 로 나머지 필드가 자동 0 되는 패턴을 사용. */
}

/*
 * [한국어]
 * fio_md5_update - 입력 데이터를 MD5 해시에 반영
 *
 * @mctx: MD5 컨텍스트 (init으로 초기화된 상태)
 * @data: 해시에 반영할 입력 데이터 포인터
 * @len: 입력 데이터의 바이트 길이
 *
 * 입력 데이터를 512비트(64바이트) 블록 단위로 나누어 md5_transform()을 호출한다.
 * 블록 크기에 미달하는 데이터는 내부 버퍼(mctx->block)에 임시 저장하고,
 * 다음 update() 호출 시 이전 잔여 데이터와 결합하여 처리한다.
 * 이를 통해 큰 데이터를 여러 번의 update() 호출로 나누어 처리할 수 있다.
 *
 * 호출 체인:
 *   verify.c → [fio_md5_update] → md5_transform()
 */
void fio_md5_update(struct fio_md5_ctx *mctx, const uint8_t *data,
		    unsigned int len)
{
	/* [한국어] 내부 64B 블록 버퍼에서 "지금 몇 바이트를 더 담을 수 있는지" 계산.
	 * byte_count & 0x3f = byte_count % 64 = 현재 부분 블록 오프셋. */
	const uint32_t avail = sizeof(mctx->block) - (mctx->byte_count & 0x3f);

	/* [한국어] 누적 바이트 카운트 갱신 — final 의 패딩 길이 계산에 사용. */
	mctx->byte_count += len;

	/* [한국어] 새 입력이 여유 공간보다 작으면 아직 블록 못 채움 → 버퍼에 누적하고 반환. */
	if (avail > len) {
		memcpy((char *)mctx->block + (sizeof(mctx->block) - avail),
		       data, len);
		return;
	}

	/* [한국어] 여유 공간만큼 먼저 채워 64B 블록 완성. */
	memcpy((char *)mctx->block + (sizeof(mctx->block) - avail),
	       data, avail);

	/* [한국어] 완성된 블록을 transform 으로 소화 → hash[] 갱신. */
	md5_transform(mctx->hash, mctx->block);
	/* [한국어] data 포인터와 잔여 len 을 갱신. */
	data += avail;
	len -= avail;

	/* [한국어] 완전한 64B 블록이 있으면 반복 소화. */
	while (len >= sizeof(mctx->block)) {
		/* [한국어] data 블록을 내부 버퍼에 복사(정렬 보장) 후 transform. */
		memcpy(mctx->block, data, sizeof(mctx->block));
		md5_transform(mctx->hash, mctx->block);
		data += sizeof(mctx->block);
		len -= sizeof(mctx->block);
	}

	/* [한국어] 64B 미만 꼬리는 내부 버퍼 선두부터 저장 — 다음 update/final 이 이어받는다. */
	memcpy(mctx->block, data, len);
}

/*
 * [한국어]
 * fio_md5_final - MD5 해시 계산을 완료하고 최종 다이제스트 확정
 *
 * @mctx: MD5 컨텍스트 - 완료 후 mctx->hash에 최종 128비트 해시값이 저장됨
 *
 * MD5 패딩 규칙(RFC 1321)에 따라:
 *   1) 데이터 끝에 0x80 바이트(비트 '1')를 추가
 *   2) 56바이트(448비트) 경계까지 0x00으로 채움
 *   3) 마지막 8바이트에 원본 메시지의 비트 길이를 리틀엔디안으로 기록
 *   4) 마지막 블록에 대해 md5_transform() 수행
 * 이를 통해 메시지 길이가 블록 크기의 배수가 아니어도 올바른 해시를 생성한다.
 *
 * 호출 체인:
 *   verify.c → [fio_md5_final] → md5_transform()
 */
void fio_md5_final(struct fio_md5_ctx *mctx)
{
	/* [한국어] 현재 부분 블록 오프셋(0..63) — byte_count % 64. */
	const unsigned int offset = mctx->byte_count & 0x3f;
	/* [한국어] 블록 내 다음 기록 위치 포인터. */
	char *p = (char *)mctx->block + offset;
	/* [한국어] 0x80 바이트 1개 기록 후 56바이트 경계까지 채울 0 의 개수.
	 * (음수면 한 블록을 더 써야 함 = flush + 다음 블록에 0 채움.) */
	int padding = 56 - (offset + 1);

	/* [한국어] RFC 1321 패딩: 메시지 끝에 반드시 0x80(=비트 '1' 하나 + 7개 0). */
	*p++ = 0x80;
	/* [한국어] 이번 블록에 56바이트 경계까지 여유가 없으면 먼저 한 블록을 flush. */
	if (padding < 0) {
		/* [한국어] 이번 블록 남은 공간을 0 으로 채움 (padding 은 음수이므로 + sizeof(u64) 보정). */
		memset(p, 0x00, padding + sizeof (uint64_t));
		/* [한국어] 첫 flush transform. */
		md5_transform(mctx->hash, mctx->block);
		/* [한국어] 포인터를 새 블록 시작으로 되돌림. */
		p = (char *)mctx->block;
		/* [한국어] 새 블록에서 56바이트 전부를 0 으로 채우게 됨. */
		padding = 56;
	}

	/* [한국어] 56바이트 경계까지 0 으로 패딩. */
	memset(p, 0, padding);
	/* [한국어] 마지막 8바이트(= 2 × uint32_t 워드 14/15) 에 "원본 메시지 비트 길이 64비트" 리틀엔디안 기록.
	 * block[14] = 하위 32비트(= byte_count * 8 modulo 2^32), block[15] = 상위 32비트(= byte_count >> 29). */
	mctx->block[14] = mctx->byte_count << 3;
	mctx->block[15] = mctx->byte_count >> 29;
	/* [한국어] 마지막 transform — 이후 mctx->hash 가 최종 16B MD5. */
	md5_transform(mctx->hash, mctx->block);
}
