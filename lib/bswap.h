/*
 * [한국어 설명] 빅엔디언↔CPU 바이트 순서 변환 유틸 헤더 (bswap.h)
 *
 * === 파일의 역할 ===
 * fio 가 다루는 파일 포맷이나 네트워크 프로토콜 중 일부가 빅엔디언(BE) 로
 * 바이트를 직렬화하는 관습을 따를 때(예: NVMe 의 일부 필드, 블록 트레이스
 * 포맷, 일부 이더넷 헤더), 런타임 CPU 의 네이티브 바이트 순서(fio 주 타겟인
 * x86_64/ARM64 는 LE) 로 변환하기 위한 `__be32_to_cpu`, `__be64_to_cpu`
 * 두 인라인 함수를 제공한다. CONFIG_LITTLE_ENDIAN 매크로가 정의된 빌드에서는
 * 수동 바이트 재배치 구현을 사용하고, 정의되지 않은(= BE CPU 인) 빌드에서는
 * 값을 그대로 반환한다. GCC `__builtin_bswap32/64` 를 직접 쓰지 않고 수동
 * 비트 연산으로 구현한 이유는 (a) 비 GCC 컴파일러 지원과 (b) 최적화기가
 * 이 패턴을 bswap 명령으로 자동 축약하기 때문이다 — 수동 구현은 가독성과
 * 호환성의 최소 공통분모를 확보한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "파일 포맷/네트워크 직렬화" 계층의 최하층 유틸. verify.c, iolog.c,
 * server.c 등이 cpu_to_le64 / le64_to_cpu 등 LE 변환은 arch/arch.h 에 의존
 * 하지만, BE 입력이 들어오는 외부 포맷 처리 경로에서는 이 파일의 함수를
 * 사용한다.
 * 호출 체인:
 *   blktrace.c / iolog 리더 → raw BE 값 읽기 → __be64_to_cpu(v) → 내부 LE
 *     자료구조 또는 CPU native 연산.
 *
 * === 타 모듈과의 연결 ===
 * - configure : CONFIG_LITTLE_ENDIAN 정의 여부 결정.
 * - <inttypes.h> : uint32_t/uint64_t.
 * - arch/arch.h 와 대비 : arch.h 의 cpu_to_le* 는 LE 쪽을 담당. 본 파일은
 *   BE 쪽 전용.
 * 데이터 흐름: 외부 포맷 바이트 → 정수로 해석(가능한 한 unaligned safe) →
 * __be*_to_cpu → CPU native 에서 산술 연산.
 *
 * === 주요 함수/구조체 요약 ===
 * - __be32_to_cpu(val) : LE 빌드에서 4 바이트 역순 재조립. BE 빌드에서는 항등.
 * - __be64_to_cpu(val) : LE 빌드에서 8 바이트 역순 재조립. BE 빌드에서는 항등.
 */
#ifndef FIO_BSWAP_H
#define FIO_BSWAP_H
/* [한국어] 헤더 가드. 여러 소스에서 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint32_t/uint64_t 고정폭 타입. 플랫폼의 long/int
 * 크기에 무관하게 정확한 32/64 비트 레지스터를 기대. */

#ifdef CONFIG_LITTLE_ENDIAN
/* [한국어] CONFIG_LITTLE_ENDIAN : configure 스크립트가 빌드 타겟 CPU 의
 * 바이트 순서를 감지해 LE 인 경우 정의. x86_64, ARM64(기본), RISC-V 에서
 * 활성화. 이 블록은 "CPU 는 LE, 입력은 BE" 경우이므로 실제로 바이트를
 * 재배치해야 한다. */

/*
 * [한국어] __be32_to_cpu - 빅엔디언 32비트 값을 리틀엔디언(CPU 네이티브)으로 변환.
 * 각 바이트를 추출하여 역순으로 재조합한다.
 *
 * 예: 0x12345678 (BE 해석: 0x78, 0x56, 0x34, 0x12) → 0x78563412 (CPU LE 표현).
 * 실행 컨텍스트: 어디서나 호출 가능. 순수 함수.
 */
static inline uint32_t __be32_to_cpu(uint32_t val)
{
	uint32_t c1, c2, c3, c4;
	/* [한국어] 4 개 바이트를 분리 저장. 현대 컴파일러는 이 패턴을 bswap
	 * 단일 명령으로 접힌다(예: x86 `bswap eax`, ARM `rev w0, w0`). */

	c1 = (val >> 24) & 0xff;
	/* [한국어] 최상위 바이트 추출 → 결과의 최하위 바이트로 이동할 바이트. */
	c2 = (val >> 16) & 0xff;
	/* [한국어] 상위 두 번째 바이트. */
	c3 = (val >> 8) & 0xff;
	/* [한국어] 상위 세 번째 바이트. */
	c4 = val & 0xff;
	/* [한국어] 최하위 바이트 → 결과의 최상위 바이트로 이동. */

	return c1 | c2 << 8 | c3 << 16 | c4 << 24;
	/* [한국어] 역순 재조립: (원래 MSB)→결과 LSB, (원래 LSB)→결과 MSB. */
}

/* [한국어] __be64_to_cpu - 빅엔디언 64비트 값을 리틀엔디언으로 변환.
 * 8 바이트 버전. 최적화기는 단일 bswap 또는 2 개의 bswap+shift 로 축약 가능. */
static inline uint64_t __be64_to_cpu(uint64_t val)
{
	uint64_t c1, c2, c3, c4, c5, c6, c7, c8;
	/* [한국어] 8 바이트를 역순으로 재조합하기 위한 임시 변수. */

	c1 = (val >> 56) & 0xff;
	/* [한국어] bit[63:56] (BE 의 첫 바이트 = 최상위) → 결과의 최하위 바이트. */
	c2 = (val >> 48) & 0xff;
	c3 = (val >> 40) & 0xff;
	c4 = (val >> 32) & 0xff;
	c5 = (val >> 24) & 0xff;
	c6 = (val >> 16) & 0xff;
	c7 = (val >> 8) & 0xff;
	c8 = val & 0xff;
	/* [한국어] bit[7:0] (BE 의 마지막 바이트 = 최하위) → 결과의 최상위 바이트. */

	return c1 | c2 << 8 | c3 << 16 | c4 << 24 | c5 << 32 | c6 << 40 | c7 << 48 | c8 << 56;
	/* [한국어] c1 이 결과의 bit[7:0] 에, c8 이 결과의 bit[63:56] 에 배치되어
	 * 엔디언이 뒤집힌 64비트 값이 나온다. */
}
#else
/* [한국어] BE CPU 빌드(또는 CONFIG_LITTLE_ENDIAN 미정의) : 입력이 이미 BE 이고
 * CPU 도 BE 이므로 변환이 필요 없다. 컴파일러는 이 인라인들을 완전히 제거. */

/* [한국어] 빅엔디언 시스템에서는 변환 불필요 - 값을 그대로 반환. */
static inline uint64_t __be64_to_cpu(uint64_t val)
{
	return val;
	/* [한국어] no-op. 컴파일러가 호출 자체를 제거하여 오버헤드 0. */
}

static inline uint32_t __be32_to_cpu(uint32_t val)
{
	return val;
	/* [한국어] no-op. */
}
#endif

#endif
/* [한국어] FIO_BSWAP_H 가드 종료. */
