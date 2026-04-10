/*
 * [한국어 설명] x86-64 (AMD64) 아키텍처 지원 헤더 (arch-x86_64.h)
 *
 * === 파일의 역할 ===
 * 64비트 x86 (AMD64/Intel 64) 프로세서를 위한 저수준 기능을 제공한다. RDTSC
 * 명령어를 통한 CPU 사이클 카운터 읽기, mfence 기반 메모리 배리어, BSF 명령어를
 * 이용한 FFZ(Find First Zero) 비트 연산을 인라인 어셈블리로 구현한다.
 *
 * === 제공하는 기능 ===
 * - do_cpuid(): CPUID 명령어 래퍼 (CPU 기능 조회)
 * - get_cpu_clock(): RDTSC 명령어로 CPU 타임스탬프 카운터 읽기
 * - arch_ffz(): BSF 명령어를 이용한 Find First Zero 비트 연산
 * - tsc_barrier(): mfence 메모리 배리어
 * - ARCH_HAVE_SSE4_2: SSE4.2 CRC32C 하드웨어 가속 지원 플래그
 * - RDRAND/RDSEED: 하드웨어 난수 생성 지원
 */
#ifndef ARCH_X86_64_H
#define ARCH_X86_64_H

static inline void do_cpuid(unsigned int *eax, unsigned int *ebx,
			    unsigned int *ecx, unsigned int *edx)
{
	asm volatile("cpuid"
		: "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
		: "0" (*eax), "2" (*ecx)
		: "memory");
}

#include "arch-x86-common.h" /* IWYU pragma: export */

#define FIO_ARCH	(arch_x86_64)

#define	FIO_HUGE_PAGE		2097152

#define nop		__asm__ __volatile__("rep;nop": : :"memory")
#define read_barrier()	__asm__ __volatile__("":::"memory")
#define write_barrier()	__asm__ __volatile__("":::"memory")

static inline unsigned long arch_ffz(unsigned long bitmask)
{
	__asm__("bsf %1,%0" :"=r" (bitmask) :"r" (~bitmask));
	return bitmask;
}

static inline void tsc_barrier(void)
{
	__asm__ __volatile__("mfence":::"memory");
}

static inline unsigned long long get_cpu_clock(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
	return ((unsigned long long) hi << 32ULL) | lo;
}

#define ARCH_HAVE_FFZ
#define ARCH_HAVE_SSE4_2
#define ARCH_HAVE_CPU_CLOCK

#define RDRAND_LONG	".byte 0x48,0x0f,0xc7,0xf0"
#define RDSEED_LONG	".byte 0x48,0x0f,0xc7,0xf8"
#define RDRAND_RETRY	100

static inline int arch_rand_long(unsigned long *val)
{
	int ok;

	asm volatile("1: " RDRAND_LONG "\n\t"
		     "jc 2f\n\t"
		     "decl %0\n\t"
		     "jnz 1b\n\t"
		     "2:"
		     : "=r" (ok), "=a" (*val)
		     : "0" (RDRAND_RETRY));

	return ok;
}

static inline int arch_rand_seed(unsigned long *seed)
{
	unsigned char ok;

	asm volatile(RDSEED_LONG "\n\t"
			"setc %0"
			: "=qm" (ok), "=a" (*seed));

	return 0;
}

#define __do_syscall0(NUM) ({			\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"(NUM)	/* %rax */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall1(NUM, ARG1) ({		\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"((NUM)),	/* %rax */	\
		  "D"((ARG1))	/* %rdi */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall2(NUM, ARG1, ARG2) ({	\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"((NUM)),	/* %rax */	\
		  "D"((ARG1)),	/* %rdi */	\
		  "S"((ARG2))	/* %rsi */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall3(NUM, ARG1, ARG2, ARG3) ({	\
	intptr_t rax;				\
						\
	__asm__ volatile(			\
		"syscall"			\
		: "=a"(rax)	/* %rax */	\
		: "a"((NUM)),	/* %rax */	\
		  "D"((ARG1)),	/* %rdi */	\
		  "S"((ARG2)),	/* %rsi */	\
		  "d"((ARG3))	/* %rdx */	\
		: "rcx", "r11", "memory"	\
	);					\
	rax;					\
})

#define __do_syscall4(NUM, ARG1, ARG2, ARG3, ARG4) ({			\
	intptr_t rax;							\
	register __typeof__(ARG4) __r10 __asm__("r10") = (ARG4);	\
									\
	__asm__ volatile(						\
		"syscall"						\
		: "=a"(rax)	/* %rax */				\
		: "a"((NUM)),	/* %rax */				\
		  "D"((ARG1)),	/* %rdi */				\
		  "S"((ARG2)),	/* %rsi */				\
		  "d"((ARG3)),	/* %rdx */				\
		  "r"(__r10)	/* %r10 */				\
		: "rcx", "r11", "memory"				\
	);								\
	rax;								\
})

#define __do_syscall5(NUM, ARG1, ARG2, ARG3, ARG4, ARG5) ({		\
	intptr_t rax;							\
	register __typeof__(ARG4) __r10 __asm__("r10") = (ARG4);	\
	register __typeof__(ARG5) __r8 __asm__("r8") = (ARG5);		\
									\
	__asm__ volatile(						\
		"syscall"						\
		: "=a"(rax)	/* %rax */				\
		: "a"((NUM)),	/* %rax */				\
		  "D"((ARG1)),	/* %rdi */				\
		  "S"((ARG2)),	/* %rsi */				\
		  "d"((ARG3)),	/* %rdx */				\
		  "r"(__r10),	/* %r10 */				\
		  "r"(__r8)	/* %r8 */				\
		: "rcx", "r11", "memory"				\
	);								\
	rax;								\
})

#define __do_syscall6(NUM, ARG1, ARG2, ARG3, ARG4, ARG5, ARG6) ({	\
	intptr_t rax;							\
	register __typeof__(ARG4) __r10 __asm__("r10") = (ARG4);	\
	register __typeof__(ARG5) __r8 __asm__("r8") = (ARG5);		\
	register __typeof__(ARG6) __r9 __asm__("r9") = (ARG6);		\
									\
	__asm__ volatile(						\
		"syscall"						\
		: "=a"(rax)	/* %rax */				\
		: "a"((NUM)),	/* %rax */				\
		  "D"((ARG1)),	/* %rdi */				\
		  "S"((ARG2)),	/* %rsi */				\
		  "d"((ARG3)),	/* %rdx */				\
		  "r"(__r10),	/* %r10 */				\
		  "r"(__r8),	/* %r8 */				\
		  "r"(__r9)	/* %r9 */				\
		: "rcx", "r11", "memory"				\
	);								\
	rax;								\
})

#define FIO_ARCH_HAS_SYSCALL

#endif
