/*
 * Shamelessly lifted from the 2.6 kernel (crypto/md5.c)
 */

/*
 * [한국어 설명] MD5 해시 알고리즘 구현 (md5.c)
 *
 * === 파일의 역할 ===
 * RFC 1321에 정의된 MD5(Message-Digest Algorithm 5) 해시 함수를 구현한다.
 * 임의 길이의 입력 데이터를 128비트(16바이트) 해시값으로 변환한다.
 * Linux 2.6 커널의 crypto/md5.c에서 가져온 코드이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 데이터 무결성 검증(verify) 파이프라인에서 사용된다.
 * I/O 쓰기 시 데이터의 MD5 해시를 저장하고, 읽기 시 재계산하여 비교한다.
 * 호출 체인: verify.c → fio_md5_init() → fio_md5_update() → fio_md5_final()
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: I/O 데이터의 무결성을 검증할 때 MD5 해시를 사용
 * - crc/test.c: 해시 알고리즘 벤치마크에서 MD5 성능 측정
 * - md5.h: 매크로(F1~F4, MD5STEP)와 fio_md5_ctx 구조체 정의
 *
 * === 주요 함수 요약 ===
 * - md5_transform(): 512비트 블록 하나를 처리하는 핵심 변환 함수 (4라운드 × 16스텝)
 * - fio_md5_init(): 해시 상태를 RFC 1321 초기값(매직 상수)으로 설정
 * - fio_md5_update(): 입력 데이터를 512비트 블록 단위로 나누어 변환 적용
 * - fio_md5_final(): 패딩과 길이 정보를 추가하고 최종 해시값 확정
 */
#include <string.h>
#include "md5.h"

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
	uint32_t a, b, c, d;

	a = hash[0];
	b = hash[1];
	c = hash[2];
	d = hash[3];

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
	mctx->hash[0] = 0x67452301;
	mctx->hash[1] = 0xefcdab89;
	mctx->hash[2] = 0x98badcfe;
	mctx->hash[3] = 0x10325476;
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
	/* [한국어] 내부 버퍼에서 아직 채울 수 있는 남은 공간 계산 (byte_count의 하위 6비트 = 현재 블록 내 오프셋) */
	const uint32_t avail = sizeof(mctx->block) - (mctx->byte_count & 0x3f);

	mctx->byte_count += len;

	if (avail > len) {
		memcpy((char *)mctx->block + (sizeof(mctx->block) - avail),
		       data, len);
		return;
	}

	memcpy((char *)mctx->block + (sizeof(mctx->block) - avail),
	       data, avail);

	md5_transform(mctx->hash, mctx->block);
	data += avail;
	len -= avail;

	while (len >= sizeof(mctx->block)) {
		memcpy(mctx->block, data, sizeof(mctx->block));
		md5_transform(mctx->hash, mctx->block);
		data += sizeof(mctx->block);
		len -= sizeof(mctx->block);
	}

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
	const unsigned int offset = mctx->byte_count & 0x3f;
	char *p = (char *)mctx->block + offset;
	int padding = 56 - (offset + 1);

	*p++ = 0x80;
	if (padding < 0) {
		memset(p, 0x00, padding + sizeof (uint64_t));
		md5_transform(mctx->hash, mctx->block);
		p = (char *)mctx->block;
		padding = 56;
	}

	memset(p, 0, padding);
	mctx->block[14] = mctx->byte_count << 3;
	mctx->block[15] = mctx->byte_count >> 29;
	md5_transform(mctx->hash, mctx->block);
}
