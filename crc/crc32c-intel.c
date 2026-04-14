/*
 * [한국어 설명] CRC-32C Intel SSE4.2 하드웨어 가속 구현 (crc32c-intel.c)
 *
 * === 파일의 역할 ===
 * Intel SSE4.2 명령어셋의 CRC32 명령어를 사용하여 CRC-32C를 하드웨어로
 * 가속 계산한다. 소프트웨어 구현(crc32c_sw) 대비 수십 배 빠른 성능을 제공한다.
 * Austin Zhang의 LKML 패치에 기반한 코드이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 시작 시 crc32c_intel_probe()가 CPUID를 확인하여 SSE4.2 지원 여부를 감지한다.
 * 지원 시 fio_crc32c()에서 자동으로 이 구현을 선택한다.
 * 호출 체인: fio_crc32c() [crc32c.h] → crc32c_intel() [이 파일]
 *
 * === 타 모듈과의 연결 ===
 * - crc32c.h: fio_crc32c() 디스패치 함수에서 crc32c_intel_available 플래그 확인
 * - crc32c.c: 소프트웨어 폴백 (SSE4.2 미지원 시)
 * - arch/arch.h: ARCH_HAVE_SSE4_2, BITS_PER_LONG, do_cpuid() 매크로 제공
 *
 * === 주요 함수 요약 ===
 * - crc32c_intel(): SSE4.2 CRC32 명령어로 데이터의 CRC-32C 계산
 * - crc32c_intel_le_hw_byte(): 바이트 단위 하드웨어 CRC (잔여 데이터 처리용)
 * - crc32c_intel_probe(): CPUID로 SSE4.2 지원 여부 감지
 */
#include "crc32c.h"

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

/* [한국어] Intel SSE4.2 CRC32C 하드웨어 가속 사용 가능 여부 - probe()에서 설정 */
bool crc32c_intel_available = false;

#ifdef ARCH_HAVE_SSE4_2

#if BITS_PER_LONG == 64
#define REX_PRE "0x48, "
#define SCALE_F 8
#else
#define REX_PRE
#define SCALE_F 4
#endif

static bool crc32c_probed;

/*
 * [한국어]
 * crc32c_intel_le_hw_byte - 바이트 단위 하드웨어 CRC-32C 계산
 *
 * @crc: 현재까지의 CRC 누적값
 * @data: 처리할 데이터 포인터
 * @length: 처리할 바이트 수
 * @return: 갱신된 CRC 값
 *
 * 워드(4/8바이트) 단위로 처리할 수 없는 잔여 바이트에 대해
 * 바이트 단위 CRC32 명령어(.byte 0xf2,0xf,0x38,0xf0,0xf1)를 사용한다.
 * 이 인라인 어셈블리는 crc32b 명령어의 바이트 인코딩이다.
 */
static uint32_t crc32c_intel_le_hw_byte(uint32_t crc, unsigned char const *data,
					unsigned long length)
{
	while (length--) {
		__asm__ __volatile__(
			".byte 0xf2, 0xf, 0x38, 0xf0, 0xf1"
			:"=S"(crc)
			:"0"(crc), "c"(*data)
		);
		data++;
	}

	return crc;
}

/*
 * Steps through buffer one byte at at time, calculates reflected 
 * crc using table.
 */
/*
 * [한국어]
 * crc32c_intel - Intel SSE4.2 CRC32 명령어를 사용한 하드웨어 가속 CRC-32C 계산
 *
 * @data: CRC를 계산할 데이터 버퍼
 * @length: 데이터 길이(바이트)
 * @return: 계산된 32비트 CRC-32C 값
 *
 * 데이터를 SCALE_F(64비트에서 8바이트, 32비트에서 4바이트) 단위로 처리하며,
 * 각 청크에 대해 crc32q(64비트) 또는 crc32l(32비트) 명령어를 실행한다.
 * 워드 경계에 맞지 않는 잔여 바이트는 crc32c_intel_le_hw_byte()로 처리한다.
 * 인라인 어셈블리의 .byte 시퀀스는 crc32 명령어의 바이트 인코딩이다.
 *
 * 호출 체인:
 *   fio_crc32c() [crc32c.h] → [crc32c_intel] → 인라인 asm (crc32 명령어)
 */
uint32_t crc32c_intel(unsigned char const *data, unsigned long length)
{
	unsigned int iquotient = length / SCALE_F;
	unsigned int iremainder = length % SCALE_F;
#if BITS_PER_LONG == 64
	uint64_t *ptmp = (uint64_t *) data;
#else
	uint32_t *ptmp = (uint32_t *) data;
#endif
	uint32_t crc = ~0;

	while (iquotient--) {
		__asm__ __volatile__(
			".byte 0xf2, " REX_PRE "0xf, 0x38, 0xf1, 0xf1;"
			:"=S"(crc)
			:"0"(crc), "c"(*ptmp)
		);
		ptmp++;
	}

	if (iremainder)
		crc = crc32c_intel_le_hw_byte(crc, (unsigned char *)ptmp,
				 iremainder);

	return crc;
}

/*
 * [한국���]
 * crc32c_intel_probe - CPU가 SSE4.2(CRC32 명령어)를 지원하는지 감지
 *
 * CPUID 명령어(EAX=1)의 ECX 레지스터 비트 20이 SSE4.2 지원 플래그이다.
 * 한 번만 감지하며, 결과를 crc32c_intel_available 전역 변수에 저장한다.
 * fio 시작 시 호출되어 이후 fio_crc32c()의 디스패치 경로를 결정한다.
 *
 * 호출 체인:
 *   fio_crctest() [test.c] 또는 초기화 코드 → [crc32c_intel_probe] → do_cpuid()
 */
void crc32c_intel_probe(void)
{
	if (!crc32c_probed) {
		unsigned int eax, ebx, ecx = 0, edx;

		eax = 1;

		do_cpuid(&eax, &ebx, &ecx, &edx);
		crc32c_intel_available = (ecx & (1 << 20)) != 0;
		crc32c_probed = true;
	}
}

#endif /* ARCH_HAVE_SSE4_2 */
