/*
 * [한국어 설명] SHA-1 해시 알고리즘 헤더 (sha1.h)
 *
 * === 파일의 역할 ===
 * SHA-1(Secure Hash Algorithm 1) 해시 함수의 인터페이스를 정의한다.
 * SHA-1은 160비트(20바이트) 다이제스트를 생성하는 암호학적 해시 함수이다.
 * Mozilla SHA-1 구현 기반으로, 워드 단위 접근으로 최적화되었다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_sha1_init/update/final → blk_SHA1Block (내부)
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=sha1 옵션 시 데이터 무결성 검증
 * - sha1.c: 구현
 * - crc/test.c: 벤치마크 테스트
 *
 * === 주요 구조체 ===
 * - fio_sha1_ctx: SHA-1 계산 상태 (5개 해시 워드 H, 16개 워킹 배열 W, 총 바이트 수)
 */
#ifndef FIO_SHA1
#define FIO_SHA1

#include <inttypes.h>

/*
 * Based on the Mozilla SHA1 (see mozilla-sha1/sha1.h),
 * optimized to do word accesses rather than byte accesses,
 * and to avoid unnecessary copies into the context array.
 */

struct fio_sha1_ctx {
	uint32_t *H;
	/* 해시 상태 포인터 (5개의 uint32_t: H0~H4)
	 * 외부에서 할당한 배열을 가리킴 - 최종 160���트 해시가 여기에 저장됨 */
	unsigned int W[16];
	/* 512비트(16워드) 메시지 스케줄 배열
	 * 입력 데이터가 64바이트 미만일 때 임시 버퍼로도 사용됨 */
	unsigned long long size;
	/* 지금까지 처리한 총 바이트 수
	 * final()에서 패딩 시 메시지 길이를 기록하는 데 사용 */
};

void fio_sha1_init(struct fio_sha1_ctx *);
void fio_sha1_update(struct fio_sha1_ctx *, const void *dataIn, unsigned long len);
void fio_sha1_final(struct fio_sha1_ctx *);

#endif
