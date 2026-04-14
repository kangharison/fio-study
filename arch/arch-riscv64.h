/*
 * [한국어 설명] RISC-V 64비트 아키텍처 지원 헤더 (arch-riscv64.h)
 *
 * === 파일의 역할 ===
 * 64비트 RISC-V 프로세서를 위한 저수준 기능을 제공한다. RDTIME 명령어를 통한
 * 타이머 카운터 읽기, fence 명령어 기반의 읽기/쓰기 메모리 배리어, ecall을
 * 이용한 직접 시스템 콜 인터페이스를 구현한다.
 *
 * === 제공하는 기능 ===
 * - get_cpu_clock(): RDTIME 명령어로 타이머 값 읽기
 * - read_barrier(): fence r,r 읽기 메모리 배리어
 * - write_barrier(): fence w,w 쓰기 메모리 배리어
 * - arch_init(): tsc_reliable을 true로 설정하는 초기화 함수
 * - __do_syscall0~6: ecall 기반 직접 시스템 콜 매크로
 */
#ifndef ARCH_RISCV64_H
#define ARCH_RISCV64_H

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#define FIO_ARCH	(arch_riscv64)

#define nop		__asm__ __volatile__ ("nop")
#define read_barrier()		__asm__ __volatile__("fence r, r": : :"memory")
#define write_barrier()		__asm__ __volatile__("fence w, w": : :"memory")

/* [한국어] get_cpu_clock - RDTIME 명령어로 플랫폼 타이머 값을 읽어 시간을 측정한다 */
static inline unsigned long long get_cpu_clock(void)
{
	unsigned long val;

	asm volatile("rdtime %0" : "=r"(val));
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

/*
 * [한국어] RISC-V 직접 시스템 콜 매크로 (__do_syscall0 ~ __do_syscall6)
 * ecall 명령어로 커널 시스템 콜을 직접 수행한다.
 * RISC-V ABI: a7=시스콜번호, a0~a5=인자1~6
 */
#define __do_syscallM(...) ({						\
	__asm__ volatile (						\
		"ecall"							\
		: "=r"(a0)						\
		: __VA_ARGS__						\
		: "memory", "a1");					\
	(long) a0;							\
})

#define __do_syscallN(...) ({						\
	__asm__ volatile (						\
		"ecall"							\
		: "=r"(a0)						\
		: __VA_ARGS__						\
		: "memory");					\
	(long) a0;							\
})

#define __do_syscall0(__n) ({						\
	register long a7 __asm__("a7") = __n;				\
	register long a0 __asm__("a0");					\
									\
	__do_syscallM("r" (a7));					\
})

#define __do_syscall1(__n, __a) ({					\
	register long a7 __asm__("a7") = __n;				\
	register __typeof__(__a) a0 __asm__("a0") = __a;		\
									\
	__do_syscallM("r" (a7), "0" (a0));				\
})

#define __do_syscall2(__n, __a, __b) ({					\
	register long a7 __asm__("a7") = __n;				\
	register __typeof__(__a) a0 __asm__("a0") = __a;		\
	register __typeof__(__b) a1 __asm__("a1") = __b;		\
									\
	__do_syscallN("r" (a7), "0" (a0), "r" (a1));			\
})

#define __do_syscall3(__n, __a, __b, __c) ({				\
	register long a7 __asm__("a7") = __n;				\
	register __typeof__(__a) a0 __asm__("a0") = __a;		\
	register __typeof__(__b) a1 __asm__("a1") = __b;		\
	register __typeof__(__c) a2 __asm__("a2") = __c;		\
									\
	__do_syscallN("r" (a7), "0" (a0), "r" (a1), "r" (a2));		\
})

#define __do_syscall4(__n, __a, __b, __c, __d) ({			\
	register long a7 __asm__("a7") = __n;				\
	register __typeof__(__a) a0 __asm__("a0") = __a;		\
	register __typeof__(__b) a1 __asm__("a1") = __b;		\
	register __typeof__(__c) a2 __asm__("a2") = __c;		\
	register __typeof__(__d) a3 __asm__("a3") = __d;		\
									\
	__do_syscallN("r" (a7), "0" (a0), "r" (a1), "r" (a2), "r" (a3));\
})

#define __do_syscall5(__n, __a, __b, __c, __d, __e) ({			\
	register long a7 __asm__("a7") = __n;				\
	register __typeof__(__a) a0 __asm__("a0") = __a;		\
	register __typeof__(__b) a1 __asm__("a1") = __b;		\
	register __typeof__(__c) a2 __asm__("a2") = __c;		\
	register __typeof__(__d) a3 __asm__("a3") = __d;		\
	register __typeof__(__e) a4 __asm__("a4") = __e;		\
									\
	__do_syscallN("r" (a7), "0" (a0), "r" (a1), "r" (a2), "r" (a3),	\
			"r"(a4));					\
})

#define __do_syscall6(__n, __a, __b, __c, __d, __e, __f) ({		\
	register long a7 __asm__("a7") = __n;				\
	register __typeof__(__a) a0 __asm__("a0") = __a;		\
	register __typeof__(__b) a1 __asm__("a1") = __b;		\
	register __typeof__(__c) a2 __asm__("a2") = __c;		\
	register __typeof__(__d) a3 __asm__("a3") = __d;		\
	register __typeof__(__e) a4 __asm__("a4") = __e;		\
	register __typeof__(__f) a5 __asm__("a5") = __f;		\
									\
	__do_syscallN("r" (a7), "0" (a0), "r" (a1), "r" (a2), "r" (a3),	\
			"r" (a4), "r"(a5));				\
})

#define FIO_ARCH_HAS_SYSCALL

#endif
