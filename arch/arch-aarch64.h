/*
 * [한국어 설명] ARM 64비트 (AArch64/ARM64) 아키텍처 지원 헤더 (arch-aarch64.h)
 *
 * === 파일의 역할 ===
 * AArch64 (ARM64) 프로세서를 위한 저수준 기능을 제공한다. CNTVCT_EL0 가상
 * 타이머 카운터를 통한 사이클 측정, RBIT+CLZ 명령어 조합의 FFZ 비트 연산,
 * __sync_synchronize 기반 메모리 배리어를 구현한다.
 *
 * === 제공하는 기능 ===
 * - get_cpu_clock(): CNTVCT_EL0 레지스터로 가상 타이머 카운터 읽기
 * - arch_ffz(): RBIT/CLZ 명령어를 이용한 Find First Zero 비트 연산
 * - read_barrier(), write_barrier(): __sync_synchronize 메모리 배리어
 * - isb(): ISB (Instruction Synchronization Barrier) 명령어
 * - arch_init(): tsc_reliable을 true로 설정하는 초기화 함수
 */
#ifndef ARCH_AARCH64_H
#define ARCH_AARCH64_H

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#define FIO_ARCH	(arch_aarch64)

#define nop		do { __asm__ __volatile__ ("yield"); } while (0)
#define read_barrier()	do { __sync_synchronize(); } while (0)
#define write_barrier()	do { __sync_synchronize(); } while (0)

static inline int arch_ffz(unsigned long bitmask)
{
	unsigned long count, reversed_bits;
	if (~bitmask == 0)	/* ffz() in lib/ffz.h does this. */
		return 63;

	__asm__ __volatile__ ("rbit %1, %2\n"
			      "clz %0, %1\n" : 
			      "=r"(count), "=&r"(reversed_bits) :
			      "r"(~bitmask));
	return count;
}

#define ARCH_HAVE_FFZ

#define isb()	asm volatile("isb" : : : "memory")

static inline unsigned long long get_cpu_clock(void)
{
	unsigned long val;

	isb();
	asm volatile("mrs %0, cntvct_el0" : "=r" (val));
	return val;
}
#define ARCH_HAVE_CPU_CLOCK

#define ARCH_HAVE_INIT
extern bool tsc_reliable;
static inline int arch_init(char *envp[])
{
	tsc_reliable = true;
	return 0;
}

#define __do_syscallN(...) ({						\
	__asm__ volatile (						\
		"svc 0"							\
		: "=r"(x0)						\
		: __VA_ARGS__						\
		: "memory", "cc");					\
	(long) x0;							\
})

#define __do_syscall0(__n) ({						\
	register long x8 __asm__("x8") = __n;				\
	register long x0 __asm__("x0");					\
									\
	__do_syscallN("r" (x8));					\
})

#define __do_syscall1(__n, __a) ({					\
	register long x8 __asm__("x8") = __n;				\
	register __typeof__(__a) x0 __asm__("x0") = __a;		\
									\
	__do_syscallN("r" (x8), "0" (x0));				\
})

#define __do_syscall2(__n, __a, __b) ({					\
	register long x8 __asm__("x8") = __n;				\
	register __typeof__(__a) x0 __asm__("x0") = __a;		\
	register __typeof__(__b) x1 __asm__("x1") = __b;		\
									\
	__do_syscallN("r" (x8), "0" (x0), "r" (x1));			\
})

#define __do_syscall3(__n, __a, __b, __c) ({				\
	register long x8 __asm__("x8") = __n;				\
	register __typeof__(__a) x0 __asm__("x0") = __a;		\
	register __typeof__(__b) x1 __asm__("x1") = __b;		\
	register __typeof__(__c) x2 __asm__("x2") = __c;		\
									\
	__do_syscallN("r" (x8), "0" (x0), "r" (x1), "r" (x2));		\
})

#define __do_syscall4(__n, __a, __b, __c, __d) ({			\
	register long x8 __asm__("x8") = __n;				\
	register __typeof__(__a) x0 __asm__("x0") = __a;		\
	register __typeof__(__b) x1 __asm__("x1") = __b;		\
	register __typeof__(__c) x2 __asm__("x2") = __c;		\
	register __typeof__(__d) x3 __asm__("x3") = __d;		\
									\
	__do_syscallN("r" (x8), "0" (x0), "r" (x1), "r" (x2), "r" (x3));\
})

#define __do_syscall5(__n, __a, __b, __c, __d, __e) ({			\
	register long x8 __asm__("x8") = __n;				\
	register __typeof__(__a) x0 __asm__("x0") = __a;		\
	register __typeof__(__b) x1 __asm__("x1") = __b;		\
	register __typeof__(__c) x2 __asm__("x2") = __c;		\
	register __typeof__(__d) x3 __asm__("x3") = __d;		\
	register __typeof__(__e) x4 __asm__("x4") = __e;		\
									\
	__do_syscallN("r" (x8), "0" (x0), "r" (x1), "r" (x2), "r" (x3),	\
			"r"(x4));					\
})

#define __do_syscall6(__n, __a, __b, __c, __d, __e, __f) ({		\
	register long x8 __asm__("x8") = __n;				\
	register __typeof__(__a) x0 __asm__("x0") = __a;		\
	register __typeof__(__b) x1 __asm__("x1") = __b;		\
	register __typeof__(__c) x2 __asm__("x2") = __c;		\
	register __typeof__(__d) x3 __asm__("x3") = __d;		\
	register __typeof__(__e) x4 __asm__("x4") = __e;		\
	register __typeof__(__f) x5 __asm__("x5") = __f;		\
									\
	__do_syscallN("r" (x8), "0" (x0), "r" (x1), "r" (x2), "r" (x3),	\
			"r" (x4), "r"(x5));				\
})

#define FIO_ARCH_HAS_SYSCALL

#endif
