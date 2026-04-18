/*
 * [한국어 설명] fio --crctest — 해시/체크섬 알고리즘 벤치마크 러너 (test.c)
 *
 * === 파일의 역할 ===
 * `fio --crctest [type[,type...]]` 명령의 구현. crc/ 서브트리가 제공하는 17개 알고리즘
 * (md5, crc64, crc32, crc32c, crc16, crc7, sha1, sha256, sha512, sha3-224/256/384/512,
 * xxhash, murmur3, jhash, fnv)을 CHUNK=128KB × NR_CHUNKS=2048 = 256MB 크기로 각각
 * 실행해 MiB/sec 단위 처리량을 표준출력에 찍는다. fio 의 성능·정확성 검증 및
 * 시스템 간 하드웨어 가속 성능 비교에 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: main() [fio.c] → (`--crctest` 옵션 분기) → fio_crctest() [이 파일]
 *   → crc32c_arm64_probe() / crc32c_intel_probe() (CRC-32C 하드웨어 감지)
 *   → 각 알고리즘별 t_* 래퍼 → fio_md5_update 등 crc/*.c 공용 API.
 *
 * === 타 모듈과의 연결 ===
 * - fio.c: `--crctest` 명령줄 옵션 분기. parse_cmd_line 경로.
 * - os/os.h: 플랫폼 추상 헤더(CPU probe 의존).
 * - gettime.h/fio_time.h: fio_gettime / utime_since_now — 나노초 해상도 타이머.
 * - lib/rand.h: init_rand_seed/fill_random_buf — 테스트 버퍼를 결정적 난수로 채움.
 * - crc/*.h: 각 알고리즘의 공용 API.
 * - hash.h: jhash (Jenkins hash, lib/hash.c) — 커널 유래.
 * 데이터 흐름: CHUNK 크기 버퍼 1개 malloc → 난수 채움 → 각 t_* 가 NR_CHUNKS 회 반복 →
 *   usec 측정 → MB/s 환산.
 *
 * === 지원 알고리즘 (17종) ===
 * md5, crc64, crc32, crc32c, crc16, crc7, sha1, sha256, sha512,
 * sha3-{224,256,384,512}, xxhash, murmur3, jhash, fnv.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct test_type: 각 알고리즘 엔트리(이름, 마스크, 실행 함수, 출력값 누적).
 * - enum T_*: 알고리즘별 마스크 비트(1<<0..1<<16).
 * - t_md5 / t_crc32 / ... / t_fnv: 각 알고리즘의 NR_CHUNKS 회 실행 래퍼.
 * - t[]: 이름↔마스크↔함수 매핑 테이블(`{NULL}` sentinel 종료).
 * - get_test_mask(type): ","-구분 문자열을 비트마스크로 파싱.
 * - list_types(): 가능한 이름 목록 출력.
 * - fio_crctest(type): 메인 진입 — probe → 버퍼 준비 → 선택된 알고리즘만 실행 → 결과 출력.
 */
#include <inttypes.h>
/* [한국어] 표준 정수 매크로(PRIu64 등) — 현재 파일은 uint32_t 등 표준 타입만 쓰지만 관례적 포함. */
#include <stdio.h>
/* [한국어] printf/fprintf/sprintf — 결과 출력 및 에러 메시지. */
#include <stdlib.h>
/* [한국어] malloc/free/strdup — 테스트 버퍼 할당, 문자열 파싱 시 복제. */
#include <string.h>
/* [한국어] strcmp/strsep/strlen/strdup — 옵션 파싱. */

#include "../os/os.h"
/* [한국어] OS 추상 헤더(usec_spin, 플랫폼 의존 매크로). */
#include "../gettime.h"
/* [한국어] fio_gettime — struct timespec 획득(프로세스 단조 시계). */
#include "../fio_time.h"
/* [한국어] utime_since_now — 두 시각 간 usec 차. */
#include "../lib/rand.h"
/* [한국어] frand_state 구조체, init_rand_seed/fill_random_buf — 결정적 난수 발생. */

/* [한국어] 아래 blocks — 각 알고리즘의 공용 API. fio_md5_init/update/final,
 * fio_crc32(), XXH32_*, murmurhash3(), fnv() 등. */
#include "../crc/md5.h"
#include "../crc/crc64.h"
#include "../crc/crc32.h"
#include "../crc/crc32c.h"
#include "../crc/crc16.h"
#include "../crc/crc7.h"
#include "../crc/sha1.h"
#include "../crc/sha256.h"
#include "../crc/sha512.h"
#include "../crc/sha3.h"
#include "../crc/xxhash.h"
#include "../crc/murmur3.h"
#include "../crc/fnv.h"
#include "../hash.h"
/* [한국어] hash.h: jhash() Jenkins 32비트 해시(커널 lib/jhash.h 포트). */

#include "test.h"
/* [한국어] test.h: fio_crctest() 프로토타입 — fio.c 가 이 헤더로 호출. */

/* [한국어] 테스트 청크 크기: 128KB - 한 번에 처리하는 데이터 블록 */
#define CHUNK		131072U
/* [한국어] 청크 반복 횟수: 2048회 - 총 256MB(128KB × 2048) 데이터 처리 */
#define NR_CHUNKS	  2048U

/*
 * [한국어] struct test_type — 테이블 엔트리.
 * 설정자: 아래 t[] 전역 테이블에 하드코딩된 설계시 상수.
 * 읽는 자: fio_crctest() 의 메인 루프와 get_test_mask().
 * 동기화: name/mask/fn 은 read-only 로 쓰이고, output 은 각 벤치 함수만 단일 스레드에서 누적. */
struct test_type {
	const char *name;
	/* [한국어] 알고리즘 이름(커맨드라인 옵션 문자열과 매칭되는 소문자 id). NULL sentinel 종료. */

	unsigned int mask;
	/* [한국어] 이 알고리즘에 대응하는 T_* 비트 — get_test_mask 가 ","-구분 이름 문자열을 OR. */

	void (*fn)(struct test_type *, void *, size_t);
	/* [한국어] 실제 벤치마크를 수행하는 래퍼 포인터. 각 래퍼는 NR_CHUNKS 회 루프로
	 * CHUNK 크기 버퍼를 해시하여 처리량을 측정 가능한 분량으로 만든다. */

	uint32_t output;
	/* [한국어] 해시 결과 누적 — 컴파일러의 DCE(Dead Code Elimination)가 해시 호출 자체를
	 * 제거하지 못하도록 결과를 실제로 "쓰는" 역할. t->output += fnv(...) 패턴. */
};

/* [한국어] 각 알고리즘의 비트 마스크 — get_test_mask 가 문자열 토큰을 이 비트로 환산.
 * 설정자: enum 정의(컴파일타임). 읽는 자: get_test_mask/fio_crctest.
 * 추가 시 test.c 전역 t[] 에도 엔트리 추가 필요. */
enum {
	T_MD5		= 1U << 0,
	T_CRC64		= 1U << 1,
	T_CRC32		= 1U << 2,
	T_CRC32C	= 1U << 3,
	T_CRC16		= 1U << 4,
	T_CRC7		= 1U << 5,
	T_SHA1		= 1U << 6,
	T_SHA256	= 1U << 7,
	T_SHA512	= 1U << 8,
	T_XXHASH	= 1U << 9,
	T_MURMUR3	= 1U << 10,
	T_JHASH		= 1U << 11,
	T_FNV		= 1U << 12,
	T_SHA3_224	= 1U << 13,
	T_SHA3_256	= 1U << 14,
	T_SHA3_384	= 1U << 15,
	T_SHA3_512	= 1U << 16,
};

/*
 * [한국어] t_md5 - MD5 벤치마크 NR_CHUNKS 회 반복.
 * 스트리밍 모델: init 한 번, update+final 반복. final 뒤 update 를 또 호출하는 것은
 * 엄밀한 MD5 사용법이 아니지만 벤치마크에서는 "transform 실행량" 이 목표라 무해.
 */
static void t_md5(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 스택에 4×32비트 다이제스트 버퍼. */
	uint32_t digest[4];
	/* [한국어] 구조체 지정 초기화 — hash 만 digest 로 연결, 나머지 필드는 0. */
	struct fio_md5_ctx ctx = { .hash = digest };
	int i;

	/* [한국어] 초기 IV 적재. */
	fio_md5_init(&ctx);

	/* [한국어] NR_CHUNKS 회 반복 해시. */
	for (i = 0; i < NR_CHUNKS; i++) {
		fio_md5_update(&ctx, buf, size);
		fio_md5_final(&ctx);
	}
}

/*
 * [한국어] t_crc64 - fio_crc64 를 NR_CHUNKS 회 호출, 결과를 output 에 누적(DCE 방지).
 */
static void t_crc64(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += fio_crc64(buf, size);
}

/*
 * [한국어] t_crc32 - fio_crc32 NR_CHUNKS 회.
 */
static void t_crc32(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += fio_crc32(buf, size);
}

/*
 * [한국어] t_crc32c - fio_crc32c 인라인 디스패처(crc32c.h) NR_CHUNKS 회. 하드웨어 경로
 * (SSE4.2/ARM CRC) 가 활성화되어 있으면 자동 선택.
 */
static void t_crc32c(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += fio_crc32c(buf, size);
}

/*
 * [한국어] t_crc16 - fio_crc16 NR_CHUNKS 회.
 */
static void t_crc16(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += fio_crc16(buf, size);
}

/*
 * [한국어] t_crc7 - fio_crc7 NR_CHUNKS 회.
 */
static void t_crc7(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += fio_crc7(buf, size);
}

/*
 * [한국어] t_sha1 - fio_sha1_init/update/final 벤치.
 */
static void t_sha1(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 5×32비트 해시 버퍼. */
	uint32_t sha[5];
	/* [한국어] 구조체 init — H 필드만 sha 로 연결. */
	struct fio_sha1_ctx ctx = { .H = sha };
	int i;

	fio_sha1_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++) {
		fio_sha1_update(&ctx, buf, size);
		fio_sha1_final(&ctx);
	}
}

/*
 * [한국어] t_sha256 - SHA-256 벤치.
 */
static void t_sha256(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 64바이트(= 8×8 워드) 해시 버퍼 — sha256 final 이 state 를 여기 복사. */
	uint8_t sha[64];
	struct fio_sha256_ctx ctx = { .buf = sha };
	int i;

	fio_sha256_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++) {
		fio_sha256_update(&ctx, buf, size);
		fio_sha256_final(&ctx);
	}
}

/*
 * [한국어] t_sha512 - SHA-512 벤치.
 * 주의: 이 벤치는 final 을 호출하지 않는다 — update 만 반복하며 transform 비용만 측정.
 * (SHA-512 final 은 내부에서 update 를 호출하므로 update 전용 루프가 더 일정한 벤치.)
 */
static void t_sha512(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 128바이트 임시 버퍼 — ctx.buf 가 가리킬 곳(컨텍스트의 블록 버퍼 역할). */
	uint8_t sha[128];
	struct fio_sha512_ctx ctx = { .buf = sha };
	int i;

	fio_sha512_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++)
		fio_sha512_update(&ctx, buf, size);
}

/* [한국어] t_sha3_{224,256,384,512} - SHA-3 각 변종 벤치. 다이제스트 크기만 다르고
 * 내부 스펀지 엔진은 공용 fio_sha3_update/final 을 그대로 사용. */
static void t_sha3_224(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 28B 다이제스트 버퍼. */
	uint8_t sha[SHA3_224_DIGEST_SIZE];
	struct fio_sha3_ctx ctx = { .sha = sha };
	int i;

	fio_sha3_224_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++) {
		fio_sha3_update(&ctx, buf, size);
		fio_sha3_final(&ctx);
	}
}

static void t_sha3_256(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 32B 다이제스트. */
	uint8_t sha[SHA3_256_DIGEST_SIZE];
	struct fio_sha3_ctx ctx = { .sha = sha };
	int i;

	fio_sha3_256_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++) {
		fio_sha3_update(&ctx, buf, size);
		fio_sha3_final(&ctx);
	}
}

static void t_sha3_384(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 48B 다이제스트. */
	uint8_t sha[SHA3_384_DIGEST_SIZE];
	struct fio_sha3_ctx ctx = { .sha = sha };
	int i;

	fio_sha3_384_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++) {
		fio_sha3_update(&ctx, buf, size);
		fio_sha3_final(&ctx);
	}
}

static void t_sha3_512(struct test_type *t, void *buf, size_t size)
{
	/* [한국어] 64B 다이제스트. */
	uint8_t sha[SHA3_512_DIGEST_SIZE];
	struct fio_sha3_ctx ctx = { .sha = sha };
	int i;

	fio_sha3_512_init(&ctx);

	for (i = 0; i < NR_CHUNKS; i++) {
		fio_sha3_update(&ctx, buf, size);
		fio_sha3_final(&ctx);
	}
}

/*
 * [한국어] t_murmur3 - MurmurHash3 32비트(x86 변형) 벤치. 시드는 임의 고정값 0x8989. */
static void t_murmur3(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += murmurhash3(buf, size, 0x8989);
}

/*
 * [한국어] t_jhash - Jenkins 32비트 해시(lib/hash.c / hash.h). 같은 시드 0x8989. */
static void t_jhash(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += jhash(buf, size, 0x8989);
}

/*
 * [한국어] t_fnv - FNV-1a 64비트 해시 벤치. 시드 0x8989 (실제 FNV offset basis 는 아니지만
 * 벤치 성능만 측정하므로 임의 시드로 무방). */
static void t_fnv(struct test_type *t, void *buf, size_t size)
{
	int i;

	for (i = 0; i < NR_CHUNKS; i++)
		t->output += fnv(buf, size, 0x8989);
}

/*
 * [한국어] t_xxhash - XXH32 스트리밍 API 벤치 — init 후 NR_CHUNKS 회 update, 마지막에 digest.
 * digest 가 XXH32_init 의 malloc 을 free 까지 맡으므로 호출자의 명시적 해제 불필요. */
static void t_xxhash(struct test_type *t, void *buf, size_t size)
{
	void *state;
	int i;

	/* [한국어] XXH32 상태 malloc + 시드 0x8989 로 초기화. */
	state = XXH32_init(0x8989);

	for (i = 0; i < NR_CHUNKS; i++)
		XXH32_update(state, buf, size);

	/* [한국어] 최종 digest — 내부에서 free(state) 수행. */
	t->output = XXH32_digest(state);
}

/*
 * [한국어] t[] - 이름↔마스크↔함수 매핑 테이블.
 * 설정자: 본 파일(컴파일타임).
 * 읽는 자: get_test_mask/fio_crctest/list_types — 순회 시 .name==NULL 을 종료 조건으로.
 * 값 범위: 17 엔트리 + sentinel 1. 알고리즘 추가 시 enum T_* 와 동기화 필수.
 * 동기화: output 필드만 런타임 갱신, 그 외는 read-only 고정. */
static struct test_type t[] = {
	{
		.name = "md5",
		.mask = T_MD5,
		.fn = t_md5,
	},
	{
		.name = "crc64",
		.mask = T_CRC64,
		.fn = t_crc64,
	},
	{
		.name = "crc32",
		.mask = T_CRC32,
		.fn = t_crc32,
	},
	{
		.name = "crc32c",
		.mask = T_CRC32C,
		.fn = t_crc32c,
	},
	{
		.name = "crc16",
		.mask = T_CRC16,
		.fn = t_crc16,
	},
	{
		.name = "crc7",
		.mask = T_CRC7,
		.fn = t_crc7,
	},
	{
		.name = "sha1",
		.mask = T_SHA1,
		.fn = t_sha1,
	},
	{
		.name = "sha256",
		.mask = T_SHA256,
		.fn = t_sha256,
	},
	{
		.name = "sha512",
		.mask = T_SHA512,
		.fn = t_sha512,
	},
	{
		.name = "xxhash",
		.mask = T_XXHASH,
		.fn = t_xxhash,
	},
	{
		.name = "murmur3",
		.mask = T_MURMUR3,
		.fn = t_murmur3,
	},
	{
		.name = "jhash",
		.mask = T_JHASH,
		.fn = t_jhash,
	},
	{
		.name = "fnv",
		.mask = T_FNV,
		.fn = t_fnv,
	},
	{
		.name = "sha3-224",
		.mask = T_SHA3_224,
		.fn = t_sha3_224,
	},
	{
		.name = "sha3-256",
		.mask = T_SHA3_256,
		.fn = t_sha3_256,
	},
	{
		.name = "sha3-384",
		.mask = T_SHA3_384,
		.fn = t_sha3_384,
	},
	{
		.name = "sha3-512",
		.mask = T_SHA3_512,
		.fn = t_sha3_512,
	},
	{
		.name = NULL,
		/* [한국어] sentinel 엔트리 — 이름이 NULL 인 레코드가 나오면 반복 종료. */
	},
};

/*
 * [한국어]
 * get_test_mask - ","-구분 알고리즘 이름 리스트를 비트마스크로 변환
 *
 * @type: "md5,sha256,crc32c" 같은 사용자 입력. 공백 처리는 없음(소문자, ",").
 * @return: 매칭된 알고리즘의 T_* OR 결과. 하나도 못 찾으면 0(호출자가 에러 처리).
 *
 * 동작: strdup 로 복제 후 strsep(",") 로 분할, 각 토큰을 t[] 의 이름과 strcmp 하여 mask 축적.
 *
 * 호출 체인: fio_crctest → [get_test_mask]
 */
static unsigned int get_test_mask(const char *type)
{
	/* [한국어] ostr: free 용 원본 포인터 보관. str: strsep 가 진행 중 수정하는 포인터. */
	char *ostr, *str = strdup(type);
	unsigned int mask;
	char *name;
	int i;

	ostr = str;
	mask = 0;
	/* [한국어] ","로 나눠 토큰을 하나씩 꺼냄. str 은 strsep 가 NULL 이 될 때까지 전진. */
	while ((name = strsep(&str, ",")) != NULL) {
		/* [한국어] t[] 전체를 순회하며 이름 매칭 검색. */
		for (i = 0; t[i].name; i++) {
			if (!strcmp(t[i].name, name)) {
				/* [한국어] 매칭된 알고리즘의 mask OR. */
				mask |= t[i].mask;
				break;
			}
		}
	}

	/* [한국어] strdup 로 할당한 원본 문자열 해제. */
	free(ostr);
	return mask;
}

/*
 * [한국어]
 * list_types - 지원하는 알고리즘 이름을 한 줄씩 출력
 *
 * @return: 1 (fio_crctest 가 이를 exit status 로 반환 — "help/list" 는 성공이지만 0이 아닌 값).
 */
static int list_types(void)
{
	int i;

	/* [한국어] sentinel 까지 순회하며 이름 출력. */
	for (i = 0; t[i].name; i++)
		printf("%s\n", t[i].name);

	return 1;
}

/*
 * [한국어]
 * fio_crctest - --crctest 메인 진입점
 *
 * @type: 커맨드라인 인자(NULL = 모든 알고리즘, "help"/"list" = 목록 출력, 아니면 토큰 파싱).
 * @return: 0(성공) 또는 1(도움말/미지정). fio.c 의 main 이 이 리턴값을 전달.
 *
 * 동작:
 *   1) CRC-32C 하드웨어 경로 감지(arm64 / intel).
 *   2) 선택 마스크 결정.
 *   3) CHUNK(128KB) 버퍼 malloc + 난수 채움.
 *   4) 선택된 각 알고리즘에 대해: 첫 실행 시 CPU 웜업(100ms spin + 1회 실행),
 *      본 실행 시간 측정(usec), MB/s 환산해 출력.
 *   5) 버퍼 해제.
 *
 * 실행 컨텍스트: fio 프로세스 메인 스레드 단일 — 병렬 없음, 전역 상태 경쟁 없음.
 *
 * 호출 체인: main [fio.c] → [fio_crctest] → probe → get_test_mask/list_types → t_*
 */
int fio_crctest(const char *type)
{
	/* [한국어] 실행할 알고리즘 비트마스크. */
	unsigned int test_mask = 0;
	/* [한국어] 처리할 총 바이트 수(= 128KB × 2048 = 256MB). MB/s 환산 분자. */
	uint64_t mb = CHUNK * NR_CHUNKS;
	/* [한국어] 난수 발생기 상태(lib/rand.c). */
	struct frand_state state;
	int i, first = 1;
	void *buf;

	/* [한국어] CRC-32C 하드웨어 경로 감지 — fio_crc32c 디스패처가 이후 hw 로 분기 가능. */
	crc32c_arm64_probe();
	crc32c_intel_probe();

	/* [한국어] 인자 없음 → 모든 알고리즘 실행. */
	if (!type)
		test_mask = ~0U;
	/* [한국어] "help"/"list" → 사용 가능 목록 출력 후 1 반환. */
	else if (!strcmp(type, "help") || !strcmp(type, "list"))
		return list_types();
	/* [한국어] 외에는 파싱해 mask 산정. */
	else
		test_mask = get_test_mask(type);

	/* [한국어] 매칭 실패 — 에러 메시지 + 목록 출력. */
	if (!test_mask) {
		fprintf(stderr, "fio: unknown hash `%s`. Available:\n", type);
		return list_types();
	}

	/* [한국어] CHUNK 크기 버퍼 할당(=128KB)·결정적 난수로 채움 — 재현 가능한 벤치 결과. */
	buf = malloc(CHUNK);
	init_rand_seed(&state, 0x8989, 0);
	fill_random_buf(&state, buf, CHUNK);

	/* [한국어] 각 엔트리를 순회하며 조건에 맞으면 벤치 실행. */
	for (i = 0; t[i].name; i++) {
		struct timespec ts;
		double mb_sec;
		uint64_t usec;
		char pre[3];

		/* [한국어] 선택 마스크에 없는 알고리즘은 건너뜀. */
		if (!(t[i].mask & test_mask))
			continue;

		/*
		 * For first run, make sure CPUs are spun up and that
		 * we've touched the data.
		 */
		/* [한국어] 첫 실행: 100ms 스핀(usec_spin)으로 CPU 클럭 상승 유도 + 워밍 1회
		 * (buf/icache/tlb 예열) — 후속 측정값을 안정화. */
		if (first) {
			usec_spin(100000);
			t[i].fn(&t[i], buf, CHUNK);
		}

		/* [한국어] 시작 시각 캡처. */
		fio_gettime(&ts, NULL);
		/* [한국어] 본 벤치 실행 — NR_CHUNKS 회 해시 반복(내부 루프). */
		t[i].fn(&t[i], buf, CHUNK);
		/* [한국어] 경과 시간(usec). */
		usec = utime_since_now(&ts);

		/* [한국어] usec > 0 이면 MB/s 환산, 아니면 "inf" 출력. */
		if (usec) {
			/* [한국어] MB/s = bytes / usec (단위 환산: bytes/us = MB/s * 1.048576…). */
			mb_sec = (double) mb / (double) usec;
			/* [한국어] 1 MiB = 1.024 * 1.024 MB. MB/s → MiB/s 환산. */
			mb_sec /= (1.024 * 1.024);
			/* [한국어] 이름이 길면 탭 1개, 짧으면 탭 2개 — 정렬된 표 출력을 위한 트릭. */
			if (strlen(t[i].name) >= 7)
				sprintf(pre, "\t");
			else
				sprintf(pre, "\t\t");
			printf("%s:%s%8.2f MiB/sec\n", t[i].name, pre, mb_sec);
		} else
			printf("%s:inf MiB/sec\n", t[i].name);
		/* [한국어] 다음 iteration 부터는 워밍업 생략. */
		first = 0;
	}

	/* [한국어] 벤치 버퍼 해제. */
	free(buf);
	return 0;
}
