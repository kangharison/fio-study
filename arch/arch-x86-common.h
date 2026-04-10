/*
 * [한국어 설명] x86 공통 코드 헤더 (arch-x86-common.h)
 *
 * === 파일의 역할 ===
 * 32비트 x86과 64비트 x86_64가 공유하는 공통 기능을 구현한다. CPUID 명령어를
 * 통한 CPU 기능 감지(TSC 신뢰성, RDRAND 지원 여부 등), Intel과 AMD 프로세서
 * 각각에 대한 초기화 로직을 제공한다.
 *
 * === 제공하는 기능 ===
 * - cpuid(): CPUID 명령어 고수준 래퍼 함수
 * - arch_init_intel(): Intel CPU TSC 신뢰성 및 RDRAND 기능 감지
 * - arch_init_amd(): AMD CPU 기능 감지 및 초기화
 * - tsc_reliable, arch_random: CPU 기능 플래그 외부 변수
 */
#ifndef FIO_ARCH_X86_COMMON
#define FIO_ARCH_X86_COMMON

#include <string.h>

static inline void cpuid(unsigned int op,
			 unsigned int *eax, unsigned int *ebx,
			 unsigned int *ecx, unsigned int *edx)
{
	*eax = op;
	*ecx = 0;
	do_cpuid(eax, ebx, ecx, edx);
}

#define ARCH_HAVE_INIT

extern bool tsc_reliable;
extern int arch_random;

static inline void arch_init_intel(void)
{
	unsigned int eax, ebx, ecx = 0, edx;

	/*
	 * Check for TSC
	 */
	eax = 1;
	do_cpuid(&eax, &ebx, &ecx, &edx);
	if (!(edx & (1U << 4)))
		return;

	/*
	 * Check for constant rate and synced (across cores) TSC
	 */
	eax = 0x80000007;
	do_cpuid(&eax, &ebx, &ecx, &edx);
	tsc_reliable = (edx & (1U << 8)) != 0;

	/*
	 * Check for FDRAND
	 */
	eax = 0x1;
	do_cpuid(&eax, &ebx, &ecx, &edx);
	arch_random = (ecx & (1U << 30)) != 0;
}

static inline void arch_init_amd(void)
{
	unsigned int eax, ebx, ecx, edx;

	cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
	if (eax < 0x80000007)
		return;

	cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
	tsc_reliable = (edx & (1U << 8)) != 0;
}

static inline void arch_init(char *envp[])
{
	unsigned int level;
	char str[13];

	arch_random = tsc_reliable = 0;

	cpuid(0, &level, (unsigned int *) &str[0],
			 (unsigned int *) &str[8],
			 (unsigned int *) &str[4]);

	str[12] = '\0';
	if (!strcmp(str, "GenuineIntel") || !strcmp(str, "  Shanghai  ") ||
	    !strcmp(str, "CentaurHauls"))
		arch_init_intel();
	else if (!strcmp(str, "AuthenticAMD") || !strcmp(str, "HygonGenuine"))
		arch_init_amd();
}

#endif
