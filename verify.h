/*
 * [한국어] verify.h - 데이터 무결성 검증 헤더
 *
 * 이 파일은 fio의 I/O 데이터 무결성 검증에 필요한 구조체, 열거형, API를 정의한다.
 * 주요 내용:
 *   1) 검증 유형 열거형 - CRC7/16/32/64, MD5, SHA1/256/512, SHA3, xxHash 등
 *   2) verify_header 구조체 - 각 데이터 블록에 붙는 검증 헤더 (매직넘버, 오프셋, 시드 등)
 *   3) vhdr_* 구조체들 - 각 체크섬 알고리즘별 다이제스트 저장 구조체
 *   4) 검증 API 함수 선언 - populate, verify, fill_pattern 등
 *
 * 동작 원리:
 *   쓰기 시: populate_verify_io_u()로 데이터 블록에 패턴 + 체크섬 헤더를 채움
 *   읽기 시: verify_io_u()로 읽어온 데이터의 체크섬을 재계산하여 헤더와 비교
 */
#ifndef FIO_VERIFY_H
#define FIO_VERIFY_H

#include <stdint.h>
#include "compiler/compiler.h"  /* __must_check 등 컴파일러 속성 매크로 */
#include "verify-state.h"       /* 검증 상태 저장/복원 관련 정의 */

#define FIO_HDR_MAGIC	0xacca  /* [한국어] 검증 헤더 매직 넘버 - 헤더 유효성 판별에 사용 */

/*
 * [한국어] 검증 유형 열거형 - fio가 지원하는 모든 데이터 무결성 검증 알고리즘
 *
 * 사용자는 --verify=<type> 옵션으로 원하는 검증 알고리즘을 선택한다.
 * 각 알고리즘은 쓰기 시 체크섬을 계산하여 헤더에 저장하고,
 * 읽기 시 데이터를 다시 계산하여 저장된 값과 비교한다.
 */
enum {
	VERIFY_NONE = 0,		/* no verification */
					/* [한국어] 검증 없음 */
	VERIFY_HDR_ONLY,		/* verify header only, kept for sake of
					 * compatibility with old configurations
					 * which use 'verify=meta' */
					/* [한국어] 헤더만 검증 (이전 'verify=meta' 호환) */
	VERIFY_MD5,			/* md5 sum data blocks */
					/* [한국어] MD5 해시로 데이터 블록 검증 (128비트) */
	VERIFY_CRC64,			/* crc64 sum data blocks */
					/* [한국어] CRC64 체크섬으로 데이터 블록 검증 */
	VERIFY_CRC32,			/* crc32 sum data blocks */
					/* [한국어] CRC32 체크섬으로 데이터 블록 검증 */
	VERIFY_CRC32C,			/* crc32c sum data blocks */
					/* [한국어] CRC32C(Castagnoli) 체크섬으로 데이터 블록 검증 */
	VERIFY_CRC32C_INTEL,		/* crc32c sum data blocks with hw */
					/* [한국어] 인텔 하드웨어 가속 CRC32C 검증 */
	VERIFY_CRC16,			/* crc16 sum data blocks */
					/* [한국어] CRC16 체크섬으로 데이터 블록 검증 */
	VERIFY_CRC7,			/* crc7 sum data blocks */
					/* [한국어] CRC7 체크섬으로 데이터 블록 검증 */
	VERIFY_SHA256,			/* sha256 sum data blocks */
					/* [한국어] SHA-256 해시로 데이터 블록 검증 (256비트) */
	VERIFY_SHA512,			/* sha512 sum data blocks */
					/* [한국어] SHA-512 해시로 데이터 블록 검증 (512비트) */
	VERIFY_SHA3_224,		/* sha3-224 sum data blocks */
					/* [한국어] SHA3-224 해시로 데이터 블록 검증 */
	VERIFY_SHA3_256,		/* sha3-256 sum data blocks */
					/* [한국어] SHA3-256 해시로 데이터 블록 검증 */
	VERIFY_SHA3_384,		/* sha3-384 sum data blocks */
					/* [한국어] SHA3-384 해시로 데이터 블록 검증 */
	VERIFY_SHA3_512,		/* sha3-512 sum data blocks */
					/* [한국어] SHA3-512 해시로 데이터 블록 검증 */
	VERIFY_XXHASH,			/* xxhash sum data blocks */
					/* [한국어] xxHash로 데이터 블록 검증 (빠른 비암호화 해시) */
	VERIFY_SHA1,			/* sha1 sum data blocks */
					/* [한국어] SHA-1 해시로 데이터 블록 검증 (160비트) */
	VERIFY_PATTERN,			/* verify specific patterns */
					/* [한국어] 특정 패턴으로 데이터 블록 검증 (헤더 포함) */
	VERIFY_PATTERN_NO_HDR,		/* verify specific patterns, no hdr */
					/* [한국어] 특정 패턴으로 검증하되 헤더 없음 */
	VERIFY_NULL,			/* pretend to verify */
					/* [한국어] 검증을 수행하는 척만 함 (실제 검증 없음) */
};

/*
 * Set the high bit to distinguish versioned headers from older
 * non-versioned headers.
 */
/* [한국어] 버전이 있는 헤더와 이전 버전 없는 헤더를 구별하기 위해 최상위 비트를 설정 */
#define VERIFY_HEADER_VERSION 0x81

/*
 * A header structure associated with each checksummed data block. It is
 * followed by a checksum specific header that contains the verification
 * data.
 */
/*
 * [한국어] 검증 헤더 구조체 - 각 데이터 블록 앞에 붙는 메타데이터
 *
 * 데이터 블록 레이아웃:
 *   [verify_header][체크섬별 헤더(vhdr_*)][실제 데이터...]
 *
 * 쓰기 시 이 헤더를 채우고, 읽기 시 헤더의 값들을 검증한다.
 */
struct verify_header {
	uint16_t magic;         /* [한국어] 매직 넘버 (FIO_HDR_MAGIC=0xacca) - 헤더 유효성 확인 */
	uint8_t version;        /* [한국어] 헤더 버전 (VERIFY_HEADER_VERSION) */
	uint8_t verify_type;    /* [한국어] 검증 유형 (위 열거형 값 중 하나) */
	uint32_t len;           /* [한국어] 이 헤더가 커버하는 전체 블록 길이 (헤더 + 데이터) */
	uint64_t rand_seed;     /* [한국어] 데이터 생성에 사용된 난수 시드 */
	uint64_t offset;        /* [한국어] 파일 내 이 블록의 오프셋 */
	uint32_t time_sec;      /* [한국어] I/O 시작 시간 (초) */
	uint32_t time_nsec;     /* [한국어] I/O 시작 시간 (나노초) */
	uint64_t numberio;      /* [한국어] I/O 시퀀스 번호 - 쓰기 순서 추적에 사용 */
	uint16_t thread;        /* [한국어] 이 블록을 쓴 스레드 번호 */
	uint32_t crc32;         /* [한국어] 헤더 자체의 CRC32C 체크섬 (헤더 무결성 검증용) */
};

/* [한국어] 각 체크섬 알고리즘별 다이제스트 저장 구조체 (vhdr = verify header) */

struct vhdr_md5 {
	uint32_t md5_digest[4]; /* [한국어] MD5 다이제스트 (128비트 = 4 x 32비트) */
};
struct vhdr_sha3_224 {
	uint8_t sha[224 / 8];   /* [한국어] SHA3-224 다이제스트 (28바이트) */
};
struct vhdr_sha3_256 {
	uint8_t sha[256 / 8];   /* [한국어] SHA3-256 다이제스트 (32바이트) */
};
struct vhdr_sha3_384 {
	uint8_t sha[384 / 8];   /* [한국어] SHA3-384 다이제스트 (48바이트) */
};
struct vhdr_sha3_512 {
	uint8_t sha[512 / 8];   /* [한국어] SHA3-512 다이제스트 (64바이트) */
};
struct vhdr_sha512 {
	uint8_t sha512[128];    /* [한국어] SHA-512 다이제스트 (128바이트 버퍼) */
};
struct vhdr_sha256 {
	uint8_t sha256[64];     /* [한국어] SHA-256 다이제스트 (64바이트 버퍼) */
};
struct vhdr_sha1 {
	uint32_t sha1[5];       /* [한국어] SHA-1 다이제스트 (160비트 = 5 x 32비트) */
};
struct vhdr_crc64 {
	uint64_t crc64;         /* [한국어] CRC64 체크섬 값 */
};
struct vhdr_crc32 {
	uint32_t crc32;         /* [한국어] CRC32/CRC32C 체크섬 값 */
};
struct vhdr_crc16 {
	uint16_t crc16;         /* [한국어] CRC16 체크섬 값 */
};
struct vhdr_crc7 {
	uint8_t crc7;           /* [한국어] CRC7 체크섬 값 */
};
struct vhdr_xxhash {
	uint32_t hash;          /* [한국어] xxHash 해시 값 (32비트) */
};

/*
 * Verify helpers
 */
/* [한국어] 검증 핵심 API 함수들 */
extern void populate_verify_io_u(struct thread_data *, struct io_u *);           /* [한국어] 쓰기 전 io_u 버퍼에 패턴 데이터와 검증 헤더를 채움 */
extern int __must_check get_next_verify(struct thread_data *td, struct io_u *);  /* [한국어] 검증할 다음 I/O 조각을 io_hist에서 꺼내옴 */
extern int __must_check verify_io_u(struct thread_data *, struct io_u **);       /* [한국어] 읽어온 io_u 데이터의 무결성을 검증 (메인 검증 함수) */
extern int verify_io_u_async(struct thread_data *, struct io_u **);              /* [한국어] 비동기 검증 스레드로 io_u 검증을 위임 */
extern void fill_verify_pattern(struct thread_data *td, void *p, unsigned int len, struct io_u *io_u, uint64_t seed, int use_seed);  /* [한국어] 버퍼에 검증 패턴 데이터를 채움 */
extern void fill_buffer_pattern(struct thread_data *td, void *p, unsigned int len);  /* [한국어] 버퍼에 사용자 지정 패턴을 채움 */
extern void fio_verify_init(struct thread_data *td);  /* [한국어] 검증 초기화 (CRC32C 하드웨어 가속 탐지 등) */

/*
 * Async verify offload
 */
/* [한국어] 비동기 검증 오프로드 - 별도 스레드에서 검증을 수행하여 I/O 성능 영향 최소화 */
extern int verify_async_init(struct thread_data *);   /* [한국어] 비동기 검증 스레드 풀 생성 */
extern void verify_async_exit(struct thread_data *);  /* [한국어] 비동기 검증 스레드 풀 종료 및 정리 */

/*
 * Callbacks for pasting formats in the pattern buffer
 */
/* [한국어] 패턴 버퍼에 포맷을 삽입하는 콜백 */
extern int paste_blockoff(char *buf, unsigned int len, void *priv);  /* [한국어] 블록 오프셋을 패턴 버퍼에 삽입 (리틀엔디안 변환) */

#endif
