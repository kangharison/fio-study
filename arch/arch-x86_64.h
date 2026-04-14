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

/* [한국어] do_cpuid - 64비트 환경용 CPUID 명령어 래퍼 (레지스터를 직접 전달) */
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

#define	FIO_HUGE_PAGE		2097152		/* [한국어] 2MB - x86_64 huge page 크기 */

#define nop		__asm__ __volatile__("rep;nop": : :"memory")  /* [한국어] CPU 양보 힌트 (하이퍼스레딩 친화적) */
#define read_barrier()	__asm__ __volatile__("":::"memory")   /* [한국어] 컴파일러 읽기 배리어 */
#define write_barrier()	__asm__ __volatile__("":::"memory")  /* [한국어] 컴파일러 쓰기 배리어 */

/* [한국어] arch_ffz - BSF(Bit Scan Forward) 명령어로 최하위 0 비트의 위치를 찾는다 */
static inline unsigned long arch_ffz(unsigned long bitmask)
{
	__asm__("bsf %1,%0" :"=r" (bitmask) :"r" (~bitmask));
	return bitmask;
}

/* [한국어] tsc_barrier - mfence 명령어로 TSC 읽기 전후의 메모리 순서를 보장한다 */
static inline void tsc_barrier(void)
{
	__asm__ __volatile__("mfence":::"memory");
}

/*
 * [한국어] get_cpu_clock - RDTSC 명령어로 CPU 타임스탬프 카운터를 읽는다
 * 하위 32비트(EAX)와 상위 32비트(EDX)를 합쳐 64비트 카운터 값을 반환한다.
 * fio의 고정밀 시간 측정에 사용되며, tsc_reliable이 true일 때만 신뢰할 수 있다.
 */
static inline unsigned long long get_cpu_clock(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
	return ((unsigned long long) hi << 32ULL) | lo;
}

#define ARCH_HAVE_FFZ
#define ARCH_HAVE_SSE4_2
#define ARCH_HAVE_CPU_CLOCK

/* [한국어] RDRAND/RDSEED 하드웨어 난수 명령어 (바이트 코드로 직접 인코딩) */
#define RDRAND_LONG	".byte 0x48,0x0f,0xc7,0xf0"
#define RDSEED_LONG	".byte 0x48,0x0f,0xc7,0xf8"
#define RDRAND_RETRY	100	/* [한국어] RDRAND 실패 시 최대 재시도 횟수 */

/* [한국어] arch_rand_long - RDRAND 명령어로 64비트 하드웨어 난수를 생성한다 */
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

/* [한국어] arch_rand_seed - RDSEED 명령어로 하드웨어 시드 값을 생성한다 */
static inline int arch_rand_seed(unsigned long *seed)
{
	unsigned char ok;

	asm volatile(RDSEED_LONG "\n\t"
			"setc %0"
			: "=qm" (ok), "=a" (*seed));

	return 0;
}

/*
 * [한국어] 직접 시스템 콜 매크로 (__do_syscall0 ~ __do_syscall6)
 * syscall 명령어를 인라인 어셈블리로 직접 호출하여 glibc를 우회한다.
 * io_uring 등에서 커널 시스템 콜을 직접 수행할 때 사용된다.
 * x86_64 ABI: RAX=시스콜번호, RDI/RSI/RDX/R10/R8/R9=인자1~6
 */
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
