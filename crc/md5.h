/*
 * [한국어 설명] MD5 해시 알고리즘 헤더 (md5.h)
 *
 * === 파일의 역할 ===
 * MD5(Message-Digest Algorithm 5) 해시 함수의 인터페이스를 정의한다.
 * MD5는 임의 길이의 입력 데이터를 128비트(16바이트) 고정 길이 해시값으로
 * 변환하는 단방향 해시 함수이다. Linux 2.6 커널의 crypto/md5.c에서 가져왔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio는 데이터 무결성 검증(verify) 기능에서 다양한 해시/체크섬 알고리즘을 사용한다.
 * MD5는 그중 하나로, verify.c에서 I/O 데이터의 무결성을 확인할 때 호출된다.
 * 호출 체인: verify.c → fio_md5_init/update/final → md5_transform (내부)
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: I/O 데이터 검증 시 MD5 해시를 계산하여 기대값과 비교
 * - crc/test.c: 해시 알고리즘 성능 벤치마크 테스트
 * - 다른 해시 알고리즘(sha1, sha256, crc32 등)과 동일한 용도로 선택 가능
 *
 * === 주요 함수 요약 ===
 * - fio_md5_init(): MD5 컨텍스트를 초기 해시값(매직 상수)으로 초기화
 * - fio_md5_update(): 데이터를 블록 단위로 처리하여 해시 갱신
 * - fio_md5_final(): 패딩/길이 추가 후 최종 해시값 확정
 *
 * === 주요 구조체 ===
 * - fio_md5_ctx: MD5 계산 상태를 보관하는 컨텍스트 구조체
 */
#ifndef MD5_H
#define MD5_H

#include <stdint.h>

/* [한국어] MD5 다이제스트 크기: 128비트 = 16바이트 */
#define MD5_DIGEST_SIZE		16
/* [한국어] HMAC에서 사용하는 블록 크기: 512비트 = 64바이트 */
#define MD5_HMAC_BLOCK_SIZE	64
/* [한국어] MD5 내부 처리 블록의 32비트 워드 수: 512비트 / 32비트 = 16워드 */
#define MD5_BLOCK_WORDS		16
/* [한국어] MD5 해시 상태의 32비트 워드 수: 128비트 / 32비트 = 4워드 (A,B,C,D) */
#define MD5_HASH_WORDS		4

/*
 * [한국어] MD5의 4개 라운드에서 사용하는 비선형 함수 F1~F4
 * 각 라운드(1~4)에서 서로 다른 비트 연산을 적용하여 혼합(mixing)을 수행한다.
 * - F1 (라운드 1): 조건부 선택 함수 - x가 1이면 y, 0이면 z 선택
 * - F2 (라운드 2): F1의 인자 순서를 바꾼 변형
 * - F3 (라운드 3): 패리티 함수 - 세 입력의 XOR
 * - F4 (라운드 4): NOT z와 OR x를 결합한 비선형 함수
 */
#define F1(x, y, z)	(z ^ (x & (y ^ z)))
#define F2(x, y, z)	F1(z, x, y)
#define F3(x, y, z)	(x ^ y ^ z)
#define F4(x, y, z)	(y ^ (x | ~z))

/*
 * [한국어] MD5 한 스텝을 수행하는 매크로
 * w에 비선형 함수 결과 + 입력 워드(in)를 더하고,
 * 좌측 순환 시프트(rotate left) s비트 후 x를 더한다.
 * RFC 1321에 정의된 MD5 변환 단계의 핵심 연산이다.
 */
#define MD5STEP(f, w, x, y, z, in, s) \
	(w += f(x, y, z) + in, w = (w<<s | w>>(32-s)) + x)

/*
 * [한국어] MD5 계산 컨텍스트 구조체
 * MD5 해시 계산의 중간 상태를 보관한다. init/update/final 전 과정에서 사용된다.
 */
struct fio_md5_ctx {
	uint32_t *hash;
	/* 현재 해시 상태를 가리키는 포인터 (4개의 uint32_t: A, B, C, D)
	 * 외부에서 할당한 배열을 가리킴 - 최종 결과도 이 배열에 저장됨
	 * verify.c에서 할당하고 fio_md5_init()에서 초기값 설정 */
	uint32_t block[MD5_BLOCK_WORDS];
	/* 현재 처리 중인 512비트(16워드) 입력 블록 버퍼
	 * update()에서 데이터가 블록 크기에 미달할 때 임시 저장 */
	uint64_t byte_count;
	/* 지금까지 처리한 총 바이트 수
	 * final()에서 패딩 시 메시지 길이를 기록하는 데 사용 */
};

extern void fio_md5_update(struct fio_md5_ctx *, const uint8_t *, unsigned int);
extern void fio_md5_final(struct fio_md5_ctx *);
extern void fio_md5_init(struct fio_md5_ctx *);

#endif
