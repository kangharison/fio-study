/*
 * [한국어 설명] fio 내장 memcpy 벤치마크/테스터 (memcpy.c)
 *
 * === 파일의 역할 ===
 * fio 실행 시 `--memcpy-test[=type]` 옵션이 주어지면 호출되는, 시스템의 메모리
 * 복사 성능을 측정하는 벤치마크 드라이버이다. 8바이트부터 524288바이트까지
 * 11개의 "한 번의 복사 크기(블록 크기)"에 대해, 32MiB짜리 src/dst 버퍼를
 * 64회 반복(NR_ITERS=64)하며 복사하여 MiB/sec 대역폭을 측정한다. 복사 방법은
 *   1) 표준 libc `memcpy`
 *   2) 표준 libc `memmove` (src/dst 겹침 허용)
 *   3) "simple" = 바이트 단위 단순 루프 (벡터화/언롤링 없음, 베이스라인)
 *   4) "hybrid" = 블록 크기 ≥64B 는 simple, <64B 는 memcpy
 * 네 가지를 지원하며, "--memcpy-test=memcpy,hybrid" 처럼 쉼표로 여러 타입을
 * 조합할 수 있다. `help`/`list` 를 넣으면 지원 타입 목록만 출력하고 종료한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 는 I/O 엔진이 실제 디바이스에 읽기/쓰기를 하기 전에 사용자 공간 버퍼를
 * 채우거나(`fill_random_buf`, `cpy_pattern`) 검증 버퍼와 비교하거나(`verify.c`)
 * splice/mmap 경로에서 사용자 버퍼로 페이지를 옮기는 등, 수많은 곳에서 memcpy
 * 류 연산을 수행한다. 메모리 대역폭이 병목일 때(NVMe Gen5 × 다중 잡) 측정된
 * IOPS 가 실제 디바이스 성능이 아니라 CPU-DRAM 대역폭 상한이 될 수 있으므로,
 * 이 파일은 fio 본 실행 전 시스템의 순수 메모리 복사 대역폭을 사전 측정하여
 * 결과 해석의 기준선을 제공한다.
 *
 * 호출 체인:
 *   fio 실행 (fio.c main)
 *     → parse_cmd_line()            // --memcpy-test[=type] 파싱
 *     → fio_memcpy_test(type)       // [본 파일의 유일한 공개 엔트리]
 *         ├─ setup_tests()          // 32MiB src/dst malloc + init_rand_seed + fill_random_buf
 *         ├─ usec_spin(100000)      // CPU 워밍업 (P-state 상승, 캐시 초기화)
 *         ├─ t[i].fn(&tests[0])     // 첫 테스트로 버퍼 페이지 폴트 유도
 *         ├─ fio_gettime/utime_since_now   // 고해상도 시간 측정 (gettime.c)
 *         └─ free_tests()           // 버퍼 해제
 * 본 파일은 I/O 엔진 경로(io_u.c, ioengines.c) 와는 무관하며, 완전히 독립적인
 * 벤치마크 하위 명령으로서 동작한다.
 *
 * === 타 모듈과의 연결 ===
 * - memcpy.h: fio_memcpy_test() 프로토타입만 선언하여 fio.c 에서 호출되도록 노출.
 * - rand.h: init_rand_seed(), fill_random_buf() 를 통해 src 버퍼를 난수로 초기화
 *           (0 초기화 시 페이지가 zero-page 로 머물러 복사 동작이 왜곡되는 것 방지).
 * - os/os.h: usec_spin() 의 플랫폼별 구현 제공 (Linux 는 clock_gettime 기반 busy loop).
 * - fio_time.h: utime_since_now() — timespec 차이를 마이크로초로 환산.
 * - gettime.h: fio_gettime() — 모노토닉 클록 래퍼. CLOCK_MONOTONIC 기반.
 * 데이터 흐름: malloc → fill_random_buf(src) → t_memcpy/t_memmove/t_simple/t_hybrid
 *            → free_tests(). dst 는 초기화하지 않음 (memcpy 가 덮어쓰므로).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_memcpy_test(type): 공개 엔트리. type 문자열 → mask 로 변환 후 각 방법별 루프.
 * - t_memcpy/t_memmove/t_simple/t_hybrid: 4종 복사 방법의 드라이버 (do_test 매크로 사용).
 * - simple_memcpy: 바이트 단위 naive 복사 (컴파일러가 SSE/AVX 로 벡터화해도 베이스라인).
 * - do_test: NR_ITERS=64 회 반복하며 32MiB 버퍼를 test->size 조각으로 나눠 복사하는 매크로.
 * - setup_tests/free_tests: 32MiB src/dst 할당 + 난수 초기화 / 해제.
 * - get_test_mask/list_types: "memcpy,hybrid" 같은 쉼표 문자열을 비트마스크로 변환/목록 출력.
 * - struct memcpy_test: 한 블록 크기에 대한 {name, src, dst, size}.
 * - struct memcpy_type: 한 복사 방법에 대한 {name, mask, fn}.
 */
#include <inttypes.h>	/* [한국어] uint64_t 등 고정 폭 정수 타입 (대역폭 계산의 오버플로 방지) */
#include <stdio.h>	/* [한국어] printf/fprintf — 결과 출력 및 에러 메시지 */
#include <stdlib.h>	/* [한국어] malloc/free, strdup — 버퍼와 타입 문자열 처리 */
#include <string.h>	/* [한국어] memcpy/memmove/strcmp/strsep — 표준 libc 복사 함수(측정 대상) 및 토큰 파싱 */

#include "memcpy.h"		/* [한국어] fio_memcpy_test() 프로토타입 (외부에서 호출 가능하도록 선언) */
#include "rand.h"		/* [한국어] init_rand_seed/fill_random_buf — src 버퍼를 난수로 초기화하여 zero-page 최적화를 방지 */
#include "../os/os.h"		/* [한국어] usec_spin() — 지정된 마이크로초만큼 busy loop (CPU 워밍업용, 플랫폼별 구현) */
#include "../fio_time.h"	/* [한국어] utime_since_now() — struct timespec 차이를 usec 단위로 환산 */
#include "../gettime.h"		/* [한국어] fio_gettime() — CLOCK_MONOTONIC 래퍼, 나노초 해상도 측정용 */

/* [한국어] 각 테스트에서 사용되는 src/dst 버퍼의 크기. 32MiB 로 고정.
 * - L3 캐시를 확실히 넘겨 실제 DRAM 대역폭을 측정하려는 의도.
 * - 32MiB × 64 iter = 2GiB 가 한 블록 크기당 복사되어 측정 시간이 수십 ms 수준이 되도록 계산됨.
 * - unsigned long long (ULL) 로 선언하여 32비트 플랫폼에서도 오버플로 방지. */
#define BUF_SIZE	32 * 1024 * 1024ULL

/* [한국어] 각 테스트의 반복 횟수. 64회 누적하여 평균 대역폭을 계산한다.
 * 한 번의 측정으로는 스케줄러/인터럽트 잡음이 커서, 다수 반복으로 평균화한다. */
#define NR_ITERS	64

/*
 * struct memcpy_test
 *
 * [한국어] 한 개의 "블록 크기(한 번의 복사에서 전달할 size 파라미터)" 벤치마크
 * 케이스를 기술한다. tests[] 전역 배열에 11개 크기(8, 16, 96, 128, 256, 512,
 * 2048, 8192, 131072, 262144, 524288)가 하드코딩되어 있다.
 */
struct memcpy_test {
	const char *name;
	/* [한국어] 결과 출력 시 사용될 사람이 읽을 수 있는 이름 (예: "8 bytes", "131072 bytes").
	 * 설정자: tests[] 배열 초기화 시 컴파일 타임 상수.
	 * 읽는 자: fio_memcpy_test() 결과 출력 루프(printf "%s: %.2f MiB/sec"). NULL 이면 배열 끝 sentinel.
	 * 값 범위: 정적 문자열 또는 NULL. */

	void *src;
	/* [한국어] 소스 버퍼 포인터. setup_tests() 에서 malloc(BUF_SIZE) 된 뒤
	 * fill_random_buf 로 난수 채움. 모든 tests[i] 가 동일 src/dst 를 공유한다.
	 * 설정자: setup_tests(). 읽는 자: do_test 매크로의 src=test->src.
	 * 값 범위: malloc 성공 시 유효 포인터, 실패 경로는 setup_tests 가 1 반환.
	 * 동기화: 단일 스레드(메인)에서만 접근하므로 락 불필요. */

	void *dst;
	/* [한국어] 대상 버퍼 포인터. src 와 구조 동일. memcpy/memmove 의 dst 인자로 전달.
	 * 초기 내용은 무관 (측정은 바이트 단위 복사 수행이므로 덮어쓰기). */

	size_t size;
	/* [한국어] 한 번의 memcpy(dst, src, size) 호출에서 전달할 바이트 수.
	 * do_test 가 BUF_SIZE/size 만큼 반복하여 32MiB 전체를 복사한다.
	 * 값 범위: 8 ~ 524288 (tests[] 에 하드코딩). 설정자: tests[] 초기화. */
};

/* [한국어] 측정 대상 블록 크기 카탈로그. 작은 크기(8~128B)는 브랜치/fast-path
 * 오버헤드를 주로 관측하고, 중간(256B~2KB)은 L1/L2 스트리밍, 큰(128KB+)은
 * DRAM 대역폭을 측정한다. .name=NULL 이 배열 끝 sentinel.
 * .src/.dst 는 setup_tests() 에서 런타임에 공통 버퍼 주소로 설정된다. */
static struct memcpy_test tests[] = {
	{ .name	= "8 bytes",		.size = 8, },		/* [한국어] 최소 크기 — 함수 호출 오버헤드 비중이 큼 */
	{ .name	= "16 bytes",		.size = 16, },		/* [한국어] 한 SSE 레지스터(128비트) 크기 */
	{ .name	= "96 bytes",		.size = 96, },		/* [한국어] 3 × 32B — AVX-256 언롤 경계 */
	{ .name	= "128 bytes",		.size = 128, },		/* [한국어] 캐시 라인 2개 크기(64B × 2) */
	{ .name	= "256 bytes",		.size = 256, },		/* [한국어] glibc memcpy rep movsb 임계 근처 */
	{ .name	= "512 bytes",		.size = 512, },		/* [한국어] 페이지 크기(4KB)의 1/8 */
	{ .name	= "2048 bytes",		.size = 2048, },	/* [한국어] 중간 — L1 스트리밍 */
	{ .name	= "8192 bytes",		.size = 8192, },	/* [한국어] 2 페이지 — TLB 경계 */
	{ .name	= "131072 bytes",	.size = 131072, },	/* [한국어] 128KiB — L2 캐시 경계 */
	{ .name	= "262144 bytes",	.size = 262144, },	/* [한국어] 256KiB — L2/L3 경계 */
	{ .name	= "524288 bytes",	.size = 524288, },	/* [한국어] 512KiB — L3 진입 */
	{ .name	= NULL, },					/* [한국어] sentinel: 배열 끝을 표시, 루프 종료 조건 */
};

/*
 * struct memcpy_type
 *
 * [한국어] 한 개의 "복사 방법(memcpy/memmove/simple/hybrid)" 을 기술한다.
 * get_test_mask() 가 쉼표 구분 사용자 입력을 비트마스크로 변환할 때 사용한다.
 */
struct memcpy_type {
	const char *name;
	/* [한국어] 사용자 입력에서 매칭할 문자열 ("memcpy", "memmove", "simple", "hybrid").
	 * NULL 이면 t[] 배열 끝 sentinel. */

	unsigned int mask;
	/* [한국어] 이 방법을 식별하는 비트 (T_MEMCPY, T_MEMMOVE, T_SIMPLE, T_HYBRID).
	 * fio_memcpy_test() 는 test_mask & t[i].mask 로 선택 여부를 판정.
	 * 여러 방법을 "," 로 조합하면 OR 로 결합된다. */

	void (*fn)(struct memcpy_test *);
	/* [한국어] 이 방법으로 한 개의 memcpy_test 를 64회 반복 실행하는 콜백.
	 * 설정자: t[] 정적 초기화. 호출자: fio_memcpy_test() 의 내부 루프.
	 * 호출 규약: test->src, dst, size 를 사용해 BUF_SIZE 만큼 복사. */
};

/*
 * [한국어] 복사 방법을 비트로 구분하는 enum. 쉼표 구분 입력을 OR 로 합쳐
 * 한 번의 호출에서 여러 방법을 순차 측정할 수 있게 한다.
 */
enum {
	T_MEMCPY	= 1U << 0,
	/* [한국어] 표준 libc memcpy. 아키텍처별 최적화(SSE/AVX/rep movsb)가 자동 적용된다.
	 * 대부분의 플랫폼에서 가장 빠르며, 실제 fio 내부가 가장 많이 사용하는 경로. */

	T_MEMMOVE	= 1U << 1,
	/* [한국어] 표준 libc memmove. src/dst 겹침을 허용하므로 memcpy 보다 약간 느리다.
	 * fio 는 버퍼 재사용이 많아 memmove 오버헤드를 참고로 측정. */

	T_SIMPLE	= 1U << 2,
	/* [한국어] 바이트 단위 단순 복사 (while(len--) *d++ = *s++).
	 * 컴파일러가 벡터화할 수도 있으나 보수적 베이스라인 역할. */

	T_HYBRID	= 1U << 3,
	/* [한국어] 블록 크기 ≥64B 는 simple, <64B 는 memcpy.
	 * "작은 크기는 memcpy 의 오버헤드가 큰가?" 를 확인하기 위한 비교 지표. */
};

/*
 * [한국어] do_test - 한 memcpy_test 를 NR_ITERS 회 반복하며 BUF_SIZE 전체를 복사
 *
 * @test: memcpy_test 포인터 (src, dst, size 사용)
 * @fn:   실제 복사 함수 (memcpy/memmove/simple_memcpy)
 *
 * 매크로로 작성된 이유: fn 을 함수 포인터가 아닌 심볼로 직접 호출하여
 * 인라인/레지스터 할당을 컴파일러가 최적화하도록 한다. 함수 포인터로 받으면
 * 최적화가 약해진다(간접 호출 비용).
 *
 * 내부 루프: BUF_SIZE(32MiB) 를 test->size 조각으로 나눠 복사하고, 남은 잔여는
 * this=left 로 잘라서 마지막 호출을 안전하게 수행. src/dst 가 연속적으로
 * 진행되므로 데이터는 캐시 스트리밍 경로를 타게 된다.
 */
#define do_test(test, fn)	do {					\
	size_t left, this;						\
	void *src, *dst;						\
	int i;								\
									\
	for (i = 0; i < NR_ITERS; i++) {				\
		left = BUF_SIZE;					\
		src = test->src;					\
		dst = test->dst;					\
		while (left) {						\
			this = test->size;				\
			if (this > left)				\
				this = left;				\
			(fn)(dst, src, this);				\
			left -= this;					\
			src += this;					\
			dst += this;					\
		}							\
	}								\
} while (0)

/*
 * [한국어] t_memcpy - 표준 memcpy 로 한 블록 크기 테스트를 반복 실행
 *
 * @test: 측정 대상 memcpy_test (크기/버퍼 지정)
 *
 * 실행 컨텍스트: fio 메인 스레드(단일). 벤치마크 모드이므로 I/O 엔진/잡 스레드와 무관.
 * 호출 체인: fio_memcpy_test() → t[0].fn=[t_memcpy] → do_test → memcpy.
 */
static void t_memcpy(struct memcpy_test *test)
{
	do_test(test, memcpy);	/* [한국어] do_test 매크로에 libc memcpy 를 바인딩하여 전개 */
}

/*
 * [한국어] t_memmove - 표준 memmove 로 한 블록 크기 테스트를 반복 실행
 *
 * memcpy 와 다른 점: 겹침 허용 검사(일반적으로 src 와 dst 비교 후 정/역방향 선택)
 * 때문에 약간의 오버헤드가 존재한다. 여기서는 src/dst 가 분리된 버퍼이지만
 * memmove 는 그 사실을 사전에 알 수 없다.
 */
static void t_memmove(struct memcpy_test *test)
{
	do_test(test, memmove);	/* [한국어] do_test 매크로에 libc memmove 를 바인딩 */
}

/*
 * [한국어] simple_memcpy - 바이트 단위 단순 복사 구현
 *
 * @dst: 대상 포인터 (void* → char* 로 받아 바이트 단위 진행)
 * @src: 소스 포인터 (void const*)
 * @len: 복사할 바이트 수
 *
 * 역할: 컴파일러 최적화 없이도 예측 가능한 "최악의 경우" 기준선을 측정한다.
 * 단, 현대 컴파일러는 이 루프도 SIMD 로 벡터화할 수 있으므로 실제 실행 코드는
 * 최적화 플래그(-O2/-O3)에 따라 달라진다.
 *
 * 실행 컨텍스트: 메인 스레드(벤치마크 드라이버). 재진입 안전(로컬 변수만 사용).
 */
static void simple_memcpy(void *dst, void const *src, size_t len)
{
 	char *d = dst;			/* [한국어] dst 를 char* 로 캐스팅하여 바이트 증분 가능 */
	const char *s = src;		/* [한국어] src 도 const char* 로 캐스팅, 읽기 전용 보장 */

	while (len--)			/* [한국어] len 이 0 이 될 때까지 반복. 후위 감소이므로 len=1 일 때도 한 번 실행 */
		*d++ = *s++;		/* [한국어] 바이트 한 개 복사 후 두 포인터 모두 전진 */
}

/*
 * [한국어] t_simple - simple_memcpy 로 한 블록 크기 테스트를 반복 실행
 */
static void t_simple(struct memcpy_test *test)
{
	do_test(test, simple_memcpy);	/* [한국어] do_test 매크로에 simple_memcpy 를 바인딩 */
}

/*
 * [한국어] t_hybrid - 블록 크기에 따라 memcpy 와 simple_memcpy 를 전환
 *
 * 64B 경계는 일반적인 캐시 라인 크기이며, 이 이상에서는 memcpy 의 벡터화
 * 최적화가 이득이 없거나 오히려 분기 오버헤드로 역효과가 날 수 있다는
 * 실험적 관측에 근거한 휴리스틱이다.
 */
static void t_hybrid(struct memcpy_test *test)
{
	if (test->size >= 64)		/* [한국어] 블록 64B 이상이면 단순 복사로 처리 */
		do_test(test, simple_memcpy);	/* [한국어] do_test 전개 + simple_memcpy 바인딩 */
	else				/* [한국어] 그 외(8~16B 등 짧은 크기)는 memcpy 사용 */
		do_test(test, memcpy);	/* [한국어] 짧은 크기에서는 SIMD 프롤로그 오버헤드가 작은 memcpy 가 유리 */
}

/* [한국어] 복사 방법 카탈로그. 사용자 입력 문자열과 내부 비트를 매핑한다.
 * .name=NULL 이 sentinel. get_test_mask 와 list_types 모두 이 배열을 순회. */
static struct memcpy_type t[] = {
	{ .name = "memcpy",	.mask = T_MEMCPY,	.fn = t_memcpy, },	/* [한국어] 기본 libc memcpy */
	{ .name = "memmove",	.mask = T_MEMMOVE,	.fn = t_memmove, },	/* [한국어] 겹침 허용 libc memmove */
	{ .name = "simple",	.mask = T_SIMPLE,	.fn = t_simple, },	/* [한국어] 바이트 단위 단순 복사 */
	{ .name = "hybrid",	.mask = T_HYBRID,	.fn = t_hybrid, },	/* [한국어] 64B 기준 memcpy/simple 혼합 */
	{ .name = NULL, },							/* [한국어] 배열 끝 sentinel */
};

/*
 * [한국어] get_test_mask - "memcpy,hybrid" 같은 쉼표 구분 문자열을 비트마스크로 변환
 *
 * @type: 사용자가 `--memcpy-test=TYPE` 으로 전달한 원본 문자열 (수정 금지)
 * @return: T_MEMCPY|T_MEMMOVE|T_SIMPLE|T_HYBRID 의 OR 조합. 매칭 실패 부분은 무시되고 0 반환 가능.
 *
 * strdup 으로 복제한 뒤 strsep(",") 로 토큰을 잘라내며 t[] 에서 매칭한다.
 * strsep 는 원본 문자열을 파괴적으로 수정하므로 복제가 필수.
 *
 * 호출 체인: fio_memcpy_test() → [get_test_mask] → strdup/strsep/strcmp.
 */
static unsigned int get_test_mask(const char *type)
{
	char *ostr, *str = strdup(type);	/* [한국어] type 을 복제 (strsep 가 원본을 수정하므로). ostr 는 free 용 원본 포인터 */
	unsigned int mask;			/* [한국어] 누적 비트마스크 (결과 반환값) */
	char *name;				/* [한국어] strsep 가 반환하는 한 토큰 */
	int i;					/* [한국어] t[] 테이블 인덱스 */

	ostr = str;				/* [한국어] strsep 가 str 을 전진시키므로, free() 할 원래 주소를 따로 보관 */
	mask = 0;				/* [한국어] 초기 마스크 0 으로 시작하여 매칭 토큰마다 OR */
	while ((name = strsep(&str, ",")) != NULL) {	/* [한국어] "," 단위로 토큰 추출; str 을 진행시키고 NULL 이면 종료 */
		for (i = 0; t[i].name; i++) {		/* [한국어] t[] 의 각 방법과 토큰명 비교 */
			if (!strcmp(t[i].name, name)) {	/* [한국어] 정확 일치 검사 (strcmp==0 이면 일치) */
				mask |= t[i].mask;	/* [한국어] 해당 방법의 비트를 OR 하여 누적 */
				break;			/* [한국어] 찾았으니 내부 루프 탈출 */
			}
		}
	}

	free(ostr);				/* [한국어] strdup 한 원본 버퍼 해제 (메모리 누수 방지) */
	return mask;				/* [한국어] 매칭된 모든 방법의 비트합 반환. 미매칭 시 0 */
}

/*
 * [한국어] list_types - 지원되는 복사 방법 이름을 한 줄씩 출력
 *
 * @return: 항상 1 (fio_memcpy_test 에서 에러 또는 help 종료 코드로 사용)
 *
 * 사용자가 `--memcpy-test=help` 또는 `list` 를 지정하거나, 잘못된 타입 문자열을
 * 주었을 때 출력되는 도움말. 호출자는 이 반환값을 그대로 fio_memcpy_test 반환값으로 사용.
 */
static int list_types(void)
{
	int i;			/* [한국어] t[] 순회 인덱스 */

	for (i = 0; t[i].name; i++)	/* [한국어] sentinel(NULL) 전까지 반복 */
		printf("%s\n", t[i].name);	/* [한국어] 한 줄에 방법 이름 하나 출력 (파이프/grep 친화적) */

	return 1;			/* [한국어] fio main 에 "벤치마크 루프 건너뜀" 신호. 1 = 즉시 종료 */
}

/*
 * [한국어] setup_tests - 공통 src/dst 버퍼(각 32MiB)를 할당하고 src 를 난수로 채움
 *
 * @return: 0 = 성공, 1 = 메모리 할당 실패 또는 tests[] 가 비어있음
 *
 * 난수 초기화가 중요한 이유: 0 으로 둔 페이지는 Linux 가 COW 로 zero-page 한 장에
 * 매핑해두기 때문에, 읽기 측이 실제 물리 페이지를 읽지 않아 대역폭이 과대평가된다.
 * fill_random_buf(state, src, BUF_SIZE) 로 실제 물리 페이지를 쓰게 만들어
 * 측정 시 두 개의 32MiB 영역이 실제로 DRAM에 존재하도록 보장한다.
 *
 * 호출 체인: fio_memcpy_test() → [setup_tests] → malloc + init_rand_seed + fill_random_buf.
 */
static int setup_tests(void)
{
	struct memcpy_test *test;		/* [한국어] tests[i] 참조용 포인터 */
	struct frand_state state;		/* [한국어] rand.c 의 fio 전용 난수 상태 (스택에 할당, 재현 가능한 시드 0x8989) */
	void *src, *dst;			/* [한국어] 32MiB 공통 버퍼 2개. 모든 tests[i] 가 공유 */
	int i;					/* [한국어] tests[] 순회 인덱스 */

	if (!tests[0].name)			/* [한국어] tests[] 가 완전히 비어 있으면(유지보수 사고 방지) 조기 성공 반환 */
		return 0;

	src = malloc(BUF_SIZE);			/* [한국어] 32MiB 소스 버퍼. 대용량이지만 벤치마크 1회성이므로 OK */
	dst = malloc(BUF_SIZE);			/* [한국어] 32MiB 대상 버퍼. 난수 초기화 불필요(memcpy 가 덮어쓰므로) */
	if (!src || !dst) {			/* [한국어] 둘 중 하나라도 실패하면 양쪽 모두 free (하나만 성공했을 가능성 있음) */
		free(src);			/* [한국어] NULL 에 free() 는 no-op 이므로 안전 */
		free(dst);			/* [한국어] 짝 해제 */
		return 1;			/* [한국어] 에러 반환 (fio_memcpy_test 가 1 반환) */
	}

	init_rand_seed(&state, 0x8989, 0);	/* [한국어] 시드 0x8989, use64=false → 32비트 Taus88. 재현 가능한 패턴 */
	fill_random_buf(&state, src, BUF_SIZE);	/* [한국어] src 전체를 난수로 채워 zero-page 최적화를 봉쇄 */

	for (i = 0; tests[i].name; i++) {	/* [한국어] 11개 테스트 모두에 동일한 src/dst 공유 */
		test = &tests[i];		/* [한국어] 현재 원소 참조 */
		test->src = src;		/* [한국어] 공통 src 주입 */
		test->dst = dst;		/* [한국어] 공통 dst 주입 */
	}

	return 0;				/* [한국어] 성공 */
}

/*
 * [한국어] free_tests - setup_tests 가 할당한 src/dst 버퍼 해제
 *
 * tests[0].src/dst 에만 free 를 호출해도 모든 테스트가 동일 버퍼를 공유하므로 충분.
 * fio_memcpy_test() 반환 직전 호출.
 */
static void free_tests(void)
{
	free(tests[0].src);	/* [한국어] 32MiB 소스 버퍼 해제 */
	free(tests[0].dst);	/* [한국어] 32MiB 대상 버퍼 해제 */
}

/*
 * [한국어] fio_memcpy_test - 공개 엔트리. memcpy 벤치마크 전체를 실행
 *
 * @type: 테스트 유형 문자열.
 *        NULL              → 전체 방법 실행 (mask = ~0U)
 *        "help" 또는 "list" → 방법 목록만 출력하고 종료
 *        기타               → get_test_mask 로 파싱, 매칭 실패 시 에러 후 목록 출력
 * @return: 0 = 성공, 1 = 유효하지 않은 타입 또는 버퍼 할당 실패
 *
 * 실행 컨텍스트: fio 메인 스레드 (I/O 엔진 초기화 이전, 벤치마크 하위 명령).
 * 호출자: fio.c main (parse_cmd_line 에서 memcpy_test 옵션을 보고 호출).
 *
 * 동작:
 *   1. type 파싱 → test_mask 생성
 *   2. setup_tests 로 버퍼 준비
 *   3. 각 방법(t[i]) 별로:
 *      - test_mask 확인
 *      - usec_spin(100000) 으로 CPU/캐시 워밍업 (100ms)
 *      - 첫 테스트(tests[0]) 를 미리 한 번 실행하여 페이지 폴트 해소
 *      - 11개 블록 크기마다 fio_gettime 으로 시작 시각 기록
 *      - 방법.fn(tests[j]) 실행
 *      - utime_since_now 로 경과 시간(usec) 계산
 *      - MiB/sec = (NR_ITERS * BUF_SIZE) / usec / (1.024 * 1.024)
 *      - usec 이 0 이면 분할 불능 → "inf MiB/sec" 출력
 *   4. free_tests 로 정리
 *
 * MiB/sec 계산의 (1.024 * 1.024) 나눔: bytes/usec = MB/sec (십진) → MiB/sec (이진)
 * 변환. 엄밀히는 1.048576 이어야 하나 여기서는 1.024^2 = 1.048576 으로 동일.
 *
 * 호출 체인:
 *   fio main → [fio_memcpy_test] → get_test_mask/list_types
 *                                 → setup_tests → init_rand_seed/fill_random_buf
 *                                 → usec_spin → t[i].fn → memcpy/memmove/simple
 *                                 → fio_gettime/utime_since_now → printf
 *                                 → free_tests
 */
int fio_memcpy_test(const char *type)
{
	unsigned int test_mask = 0;		/* [한국어] 실행할 방법들의 비트 OR. 0 이면 아무 방법도 선택 안 됨 */
	int j, i;				/* [한국어] i=방법 인덱스, j=블록 크기 인덱스 */

	if (!type)				/* [한국어] type 미지정 시 모든 방법 실행 */
		test_mask = ~0U;		/* [한국어] 모든 비트 1 → 모든 방법 선택 */
	else if (!strcmp(type, "help") || !strcmp(type, "list"))	/* [한국어] 도움말/목록 요청 */
		return list_types();		/* [한국어] 목록 출력하고 1 반환 (벤치마크 미수행) */
	else
		test_mask = get_test_mask(type);	/* [한국어] "memcpy,hybrid" 등 사용자 지정 파싱 */

	if (!test_mask) {			/* [한국어] 어떤 방법과도 매칭되지 않음 → 에러 */
		fprintf(stderr, "fio: unknown hash `%s`. Available:\n", type);	/* [한국어] stderr 로 진단 메시지 ("hash" 는 상위 호환 잔재 문구) */
		return list_types();		/* [한국어] 가능한 방법 목록을 안내로 출력 */
	}

	if (setup_tests()) {			/* [한국어] 버퍼 할당 + 난수 초기화. 0=성공, 1=실패 */
		fprintf(stderr, "setting up mem regions failed\n");	/* [한국어] malloc 64MiB 실패는 매우 드물지만 명시적으로 보고 */
		return 1;			/* [한국어] 벤치마크 중단 */
	}

	for (i = 0; t[i].name; i++) {		/* [한국어] 네 방법 순회 (sentinel 전까지) */
		struct timespec ts;		/* [한국어] 시작 시각 저장 */
		double mb_sec;			/* [한국어] 결과 대역폭(MiB/sec) */
		uint64_t usec;			/* [한국어] 경과 시간(마이크로초) */

		if (!(t[i].mask & test_mask))	/* [한국어] 이 방법이 선택되지 않았으면 스킵 */
			continue;

		/*
		 * For first run, make sure CPUs are spun up and that
		 * we've touched the data.
		 * [한국어] 워밍업 단계:
		 * - usec_spin(100000): 100ms busy loop → CPU 가 P-state 저전력에서 최고 성능으로 승격되도록 유도.
		 * - t[i].fn(&tests[0]): 첫 테스트(8 bytes)를 실측 전에 한 번 실행 →
		 *   src/dst 버퍼의 모든 페이지가 실제로 물리 페이지에 매핑되어(page fault 해소) 첫 측정 편향 제거.
		 */
		usec_spin(100000);		/* [한국어] 100ms 동안 CPU 바쁘게 돌려 주파수 상승 유도 */
		t[i].fn(&tests[0]);		/* [한국어] 첫 테스트로 캐시/TLB 워밍, 페이지 폴트 해소 */

		printf("%s\n", t[i].name);	/* [한국어] 방법 이름을 섹션 헤더로 출력 */

		for (j = 0; tests[j].name; j++) {	/* [한국어] 11개 블록 크기 순회 */
			fio_gettime(&ts, NULL);	/* [한국어] 시작 시각 기록 (CLOCK_MONOTONIC 기반) */
			t[i].fn(&tests[j]);	/* [한국어] 실제 측정: 32MiB × 64회 = 2GiB 복사 */
			usec = utime_since_now(&ts);	/* [한국어] 지금까지 경과한 마이크로초 계산 */

			if (usec) {		/* [한국어] 0 이면 나눗셈 불가 → 아래 else 로 */
				unsigned long long mb = NR_ITERS * BUF_SIZE;	/* [한국어] 총 복사 바이트 = 64 × 32MiB = 2GiB */

				mb_sec = (double) mb / (double) usec;	/* [한국어] bytes/usec = MB/sec (십진 기준) */
				mb_sec /= (1.024 * 1.024);		/* [한국어] MB/sec → MiB/sec 환산 (1 MiB = 1.048576 MB) */
				printf("\t%s:\t%8.2f MiB/sec\n", tests[j].name, mb_sec);	/* [한국어] 탭 들여쓰기로 가독성 확보, 소수 2자리 */
			} else
				printf("\t%s:inf MiB/sec\n", tests[j].name);	/* [한국어] 경과 0usec 은 클록 해상도 부족 신호. "inf" 로 표기 */
		}
	}

	free_tests();				/* [한국어] 32MiB × 2 버퍼 해제 */
	return 0;				/* [한국어] 정상 종료 */
}
