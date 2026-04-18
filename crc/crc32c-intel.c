/*
 * [한국어 설명] x86 Intel SSE4.2 하드웨어 CRC-32C 백엔드 (crc32c-intel.c)
 *
 * === 파일의 역할 ===
 * Intel SSE4.2 (2008 Nehalem+ / AMD Bulldozer+) 의 `CRC32` 스칼라 명령어
 * (opcode F2 48 0F 38 F0/F1 변형)를 이용해 Castagnoli 다항식(0x1EDC6F41,
 * reflected 0x82F63B78) 의 CRC-32C 를 계산한다. 64비트에서는 8B 단위 crc32q,
 * 32비트에서는 4B 단위 crc32l, 잔여 바이트는 crc32b 로 처리한다. 전부 인라인
 * 어셈블리(`.byte` 시퀀스)로 기재되어 오래된 GCC/GAS 에서도 컴파일 가능.
 * 본 파일은 CRC-32C 의 세 백엔드(x86 hw / arm64 hw / 소프트웨어) 중 x86 경로이며,
 * crc32c_intel_probe() 가 CPUID(EAX=1).ECX bit20 으로 SSE4.2 지원 여부를 감지.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 초기화에서 crc32c_intel_probe() 가 호출되어 SSE4.2 가용 여부가
 * `crc32c_intel_available` 전역 플래그에 기록된다. 런타임에 fio_crc32c() 인라인
 * 디스패처(crc32c.h)가 이 플래그를 검사해 하드웨어 경로(본 파일) 또는
 * 소프트웨어 폴백(crc32c_sw, crc32c.c)을 선택. ARM 쪽 분기는 crc32c-arm64.c 참조.
 * 호출 체인:
 *   fio main()/init → crc32c_intel_probe → do_cpuid (arch/x86.h)
 *   verify.c → fio_crc32c() [crc32c.h] → (intel 가능 시) crc32c_intel → asm CRC32 명령
 *
 * === 타 모듈과의 연결 ===
 * - crc32c.h: fio_crc32c() 디스패처, crc32c_intel_available 전역 extern.
 * - arch/arch.h (또는 arch/arch-x86.h): ARCH_HAVE_SSE4_2 매크로·BITS_PER_LONG·
 *   do_cpuid() 인라인 어셈 래퍼 공급.
 * - crc32c.c: SSE4.2 미지원 CPU 용 소프트웨어 폴백.
 * 데이터 흐름: I/O 버퍼 포인터 + 길이 → 32비트 CRC-32C.
 * 동기화: probe 이후 flag read-only. 계산 함수는 순수·재진입 안전.
 *
 * === 주요 함수 요약 ===
 * - crc32c_intel_le_hw_byte(): 바이트 단위 crc32b 명령 루프(잔여 처리 전용).
 * - crc32c_intel(): SCALE_F(8 또는 4) 단위 주 루프 + 바이트 잔여 처리.
 * - crc32c_intel_probe(): CPUID 로 SSE4.2 지원 감지(1회).
 */
#include "crc32c.h"
/* [한국어] crc32c.h: fio_crc32c 인라인 디스패처·crc32c_*_available 전역 extern·
 * crc32c_sw() 프로토타입을 제공한다. */

/*
 * Based on a posting to lkml by Austin Zhang <austin.zhang@intel.com>
 *
 * Using hardware provided CRC32 instruction to accelerate the CRC32 disposal.
 * CRC32C polynomial:0x1EDC6F41(BE)/0x82F63B78(LE)
 * CRC32 is a new instruction in Intel SSE4.2, the reference can be found at:
 * http://www.intel.com/products/processor/manuals/
 * Intel(R) 64 and IA-32 Architectures Software Developer's Manual
 * Volume 2A: Instruction Set Reference, A-M
 */

/* [한국어] x86 CRC-32C 하드웨어 가용성 플래그. false 로 시작, probe 가 결과 기록.
 * 설정자: crc32c_intel_probe() 1회.
 * 읽는 자: fio_crc32c() 디스패처(crc32c.h).
 * 값 범위: true/false.
 * 동기화: probe 이후 read-only. */
bool crc32c_intel_available = false;

#ifdef ARCH_HAVE_SSE4_2
/* [한국어] 빌드 가드: configure 가 x86 + GAS + -msse4.2 를 감지해야 본 블록 컴파일.
 * 타겟이 aarch64/ppc/riscv 면 본 파일은 전역 플래그 정의만 남긴 채 함수는 누락. */

#if BITS_PER_LONG == 64
#define REX_PRE "0x48, "
/* [한국어] 64비트 모드용 REX.W 프리픽스(0x48) — 뒤따르는 opcode 를 64비트 연산으로
 * 승격. SCALE_F=8: 한 번에 8바이트 처리. */
#define SCALE_F 8
#else
#define REX_PRE
/* [한국어] 32비트 모드 — REX 프리픽스 불필요(32비트 연산 기본). SCALE_F=4: 4바이트 처리. */
#define SCALE_F 4
#endif

/* [한국어] probe 의 idempotent 플래그 — 초기 1회만 CPUID 실행. */
static bool crc32c_probed;

/*
 * [한국어]
 * crc32c_intel_le_hw_byte - 잔여 바이트(<SCALE_F)를 crc32b 명령으로 처리
 *
 * @crc:    현재 CRC 누적값(32비트).
 * @data:   바이트 포인터.
 * @length: 바이트 수(0..SCALE_F-1 예상).
 * @return: 갱신된 CRC.
 *
 * 인라인 어셈블리 `.byte 0xf2, 0xf, 0x38, 0xf0, 0xf1` 은 `crc32 %cl, %esi`
 * 명령의 바이트 인코딩(일부 구형 GAS 가 mnemonic 을 인식 못해 수동 인코딩).
 * 오퍼랜드 제약: "=S"(crc) 는 %esi 출력, "0"(crc) 는 같은 레지스터 입력,
 * "c"(*data) 는 %cl 에 바이트 입력.
 *
 * 호출 체인: crc32c_intel → [crc32c_intel_le_hw_byte] → asm CRC32
 */
static uint32_t crc32c_intel_le_hw_byte(uint32_t crc, unsigned char const *data,
					unsigned long length)
{
	/* [한국어] length 후위 감소로 정확히 length 회 반복. */
	while (length--) {
		/* [한국어] crc32 %cl, %esi — 바이트 단위 CRC-32C 갱신.
		 * 0xf2: REPNE 프리픽스(CRC32 명령어에서 바이트 오퍼랜드 의미).
		 * 0x0f 0x38 0xf0 0xf1: CRC32 r32, r/m8 인코딩(bits: ModRM=0xF1 -> %ecx→%esi). */
		__asm__ __volatile__(
			".byte 0xf2, 0xf, 0x38, 0xf0, 0xf1"
			:"=S"(crc)
			:"0"(crc), "c"(*data)
		);
		/* [한국어] 다음 바이트로 포인터 전진. */
		data++;
	}

	/* [한국어] 갱신된 CRC 반환. */
	return crc;
}

/*
 * Steps through buffer one byte at at time, calculates reflected
 * crc using table.
 */
/*
 * [한국어]
 * crc32c_intel - Intel SSE4.2 CRC32 명령으로 CRC-32C 계산
 *
 * @data:   바이트 포인터.
 * @length: 바이트 길이.
 * @return: 32비트 CRC-32C (reflected, 최종 XOR 없음).
 *
 * 동작:
 *   1) 주 루프: SCALE_F(=8 on x86_64) 바이트 씩 crc32q(64비트) 또는 crc32l(32비트)
 *      명령으로 iquotient 회 처리.
 *   2) 잔여(iremainder) 바이트는 crc32c_intel_le_hw_byte() 가 crc32b 로 소진.
 *
 * 실행 컨텍스트: 잡/verify 스레드 — 순수 계산, 재진입 안전.
 *
 * 호출 체인: fio_crc32c() → [crc32c_intel] → asm CRC32 / crc32c_intel_le_hw_byte
 */
uint32_t crc32c_intel(unsigned char const *data, unsigned long length)
{
	/* [한국어] 주 루프 반복 횟수 — length / SCALE_F. */
	unsigned int iquotient = length / SCALE_F;
	/* [한국어] 잔여 바이트 수 — length % SCALE_F. */
	unsigned int iremainder = length % SCALE_F;
#if BITS_PER_LONG == 64
	/* [한국어] 64비트: 8B 워드 포인터로 재해석. */
	uint64_t *ptmp = (uint64_t *) data;
#else
	/* [한국어] 32비트: 4B 워드 포인터로 재해석. */
	uint32_t *ptmp = (uint32_t *) data;
#endif
	/* [한국어] CRC-32C 초기값 ~0 (Castagnoli 표준, reflected). */
	uint32_t crc = ~0;

	/* [한국어] 주 루프: 각 반복에서 1 워드(=SCALE_F 바이트) 분을 CRC 에 흡수. */
	while (iquotient--) {
		/* [한국어] crc32q(%rcx), %rsi (64비트) 또는 crc32l(%ecx), %esi (32비트).
		 * 0xf2 = REPNE 프리픽스, REX_PRE = 64비트일 때 0x48,
		 * 0x0f 0x38 0xf1 0xf1: CRC32 r32/64, r/m32/64 인코딩. */
		__asm__ __volatile__(
			".byte 0xf2, " REX_PRE "0xf, 0x38, 0xf1, 0xf1;"
			:"=S"(crc)
			:"0"(crc), "c"(*ptmp)
		);
		/* [한국어] 워드 포인터 1 증가 (= SCALE_F 바이트 전진). */
		ptmp++;
	}

	/* [한국어] 잔여 바이트가 있으면 바이트 단위 CRC 로 소진. */
	if (iremainder)
		crc = crc32c_intel_le_hw_byte(crc, (unsigned char *)ptmp,
				 iremainder);

	/* [한국어] 최종 CRC 반환 — fio_crc32c() 디스패처가 호출자에게 전달. */
	return crc;
}

/*
 * [한국어]
 * crc32c_intel_probe - CPUID 로 SSE4.2(CRC32 명령어) 지원 여부 감지(1회)
 *
 * CPUID(EAX=1) 을 실행해 반환되는 ECX 의 bit 20 이 SSE4.2 feature flag.
 * 결과를 crc32c_intel_available 전역에 기록하면 이후 디스패처가 본 경로를 선택.
 *
 * 실행 컨텍스트: 초기화 스레드 단일 — 이후 플래그 read-only.
 *
 * 호출 체인: main()/fio_crctest() → [crc32c_intel_probe] → do_cpuid()
 */
void crc32c_intel_probe(void)
{
	/* [한국어] 이미 probed 이면 skip. */
	if (!crc32c_probed) {
		/* [한국어] CPUID 결과 레지스터 — EAX/EBX/ECX/EDX. ecx 는 0으로 pre-init(일부
		 * CPUID leaf 가 ECX 를 서브-리프 인덱스로 읽기 때문 — EAX=1 에서는 무시되지만
		 * 방어적 초기화). */
		unsigned int eax, ebx, ecx = 0, edx;

		/* [한국어] CPUID leaf 1 — 기능 정보 반환(Feature Information). */
		eax = 1;

		/* [한국어] 아키텍처별 CPUID 인라인 어셈 래퍼 실행. */
		do_cpuid(&eax, &ebx, &ecx, &edx);
		/* [한국어] ECX bit 20 = SSE4.2 지원 플래그. 1 이면 CRC32 명령 사용 가능. */
		crc32c_intel_available = (ecx & (1 << 20)) != 0;
		/* [한국어] 재진입 방지 플래그. */
		crc32c_probed = true;
	}
}

#endif /* ARCH_HAVE_SSE4_2 */
/* [한국어] 빌드 가드 종료. 비-x86 또는 SSE4.2 미지원 컴파일러 타겟에서는 본 파일의
 * 함수 본체는 누락되며 crc32c_intel_available 는 false 고정, 디스패처가 sw 로 분기. */
