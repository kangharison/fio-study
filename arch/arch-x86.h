/*
 * [한국어 설명] x86 (32비트) 아키텍처 지원 헤더 (arch-x86.h)
 *
 * === 파일의 역할 ===
 * 32비트 x86 프로세서를 위한 저수준 기능을 제공한다. x86_64 버전과 유사하지만
 * 32비트 환경에 맞게 CPUID에서 ebx 레지스터를 xchg로 보존하고, RDTSC 결과를
 * 64비트로 합치는 방식이 다르다.
 *
 * === 제공하는 기능 ===
 * - do_cpuid(): 32비트 환경용 CPUID 명령어 래퍼 (ebx 보존 처리)
 * - get_cpu_clock(): RDTSC 명령어로 CPU 타임스탬프 카운터 읽기
 * - arch_ffz(): BSFL 명령어를 이용한 Find First Zero 비트 연산
 * - read_barrier(), write_barrier(): 컴파일러 메모리 배리어
 */
#ifndef ARCH_X86_H
#define ARCH_X86_H

/* [한국어] do_cpuid - 32비트 환경용 CPUID 래퍼 (PIC 호환을 위해 ebx를 xchg로 보존) */
static inline void do_cpuid(unsigned int *eax, unsigned int *ebx,
			    unsigned int *ecx, unsigned int *edx)
{
	asm volatile("xchgl %%ebx, %1\ncpuid\nxchgl %%ebx, %1"
		: "=a" (*eax), "=r" (*ebx), "=c" (*ecx), "=d" (*edx)
		: "0" (*eax)
		: "memory");
}

#include "arch-x86-common.h" /* IWYU pragma: export */

#define FIO_ARCH	(arch_x86)

#define	FIO_HUGE_PAGE		4194304		/* [한국어] 4MB - 32비트 x86 huge page 크기 */

#define nop		__asm__ __volatile__("rep;nop": : :"memory")
#define read_barrier()	__asm__ __volatile__("": : :"memory")
#define write_barrier()	__asm__ __volatile__("": : :"memory")

/* [한국어] arch_ffz - BSFL(Bit Scan Forward Long) 명령어로 최하위 0 비트 위치를 찾는다 */
static inline unsigned long arch_ffz(unsigned long bitmask)
{
	__asm__("bsfl %1,%0" :"=r" (bitmask) :"r" (~bitmask));
	return bitmask;
}

/* [한국어] get_cpu_clock - RDTSC로 64비트 TSC 값을 읽는다 ("=A" 제약조건으로 EDX:EAX를 한 번에 반환) */
static inline unsigned long long get_cpu_clock(void)
{
	unsigned long long ret;

	__asm__ __volatile__("rdtsc" : "=A" (ret));
	return ret;
}

#define ARCH_HAVE_FFZ
#define ARCH_HAVE_CPU_CLOCK

#endif
