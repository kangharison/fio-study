/*
 * [한국어 설명] 메인 아키텍처 추상화 헤더 (arch.h)
 *
 * === 파일의 역할 ===
 * 컴파일러 매크로(__i386__, __x86_64__, __aarch64__ 등)를 통해 현재 CPU 아키텍처를
 * 감지하고, 해당 아키텍처 전용 헤더 파일을 자동으로 포함시킨다. arch_t 열거형으로
 * 지원되는 모든 아키텍처를 정의하며, 원자적 연산 매크로와 io_uring 시스템 콜 번호 등
 * 공통 인프라를 제공한다.
 *
 * === 제공하는 기능 ===
 * - arch_x86_64, arch_arm, arch_ppc 등 아키텍처 열거형 (arch_t enum)
 * - atomic_add, atomic_sub, atomic_load_relaxed 등 원자적 연산 매크로
 * - 아키텍처별 헤더 자동 포함 (#if defined 분기)
 * - arch_init() 기본 구현 및 io_uring 시스템 콜 번호 정의
 */
#ifndef ARCH_H
#define ARCH_H

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

#include "../lib/types.h"

enum {
	arch_x86_64 = 1,
	arch_x86,
	arch_ppc,
	arch_ia64,
	arch_s390,
	arch_alpha,
	arch_sparc,
	arch_sparc64,
	arch_arm,
	arch_sh,
	arch_hppa,
	arch_mips,
	arch_aarch64,
	arch_loongarch64,
	arch_riscv64,

	arch_generic,

	arch_nr,
};

enum {
	ARCH_FLAG_1	= 1 << 0,
	ARCH_FLAG_2	= 1 << 1,
	ARCH_FLAG_3	= 1 << 2,
	ARCH_FLAG_4	= 1 << 3,
};

extern unsigned long arch_flags;

#define ARCH_CPU_CLOCK_WRAPS

#ifdef __cplusplus
#define atomic_add(p, v)						\
	std::atomic_fetch_add(p, (v))
#define atomic_sub(p, v)						\
	std::atomic_fetch_sub(p, (v))
#define atomic_load_relaxed(p)					\
	std::atomic_load_explicit(p,				\
			     std::memory_order_relaxed)
#define atomic_load_acquire(p)					\
	std::atomic_load_explicit(p,				\
			     std::memory_order_acquire)
#define atomic_store_relaxed(p, v)				\
	std::atomic_store_explicit((p), (v), std::memory_order_relaxed)
#define atomic_store_release(p, v)				\
	std::atomic_store_explicit(p, (v),			\
			     std::memory_order_release)
#else
#define atomic_add(p, v)					\
	atomic_fetch_add((_Atomic typeof(*(p)) *)(p), v)
#define atomic_sub(p, v)					\
	atomic_fetch_sub((_Atomic typeof(*(p)) *)(p), v)
#define atomic_load_relaxed(p)					\
	atomic_load_explicit((_Atomic typeof(*(p)) *)(p),	\
			     memory_order_relaxed)
#define atomic_load_acquire(p)					\
	atomic_load_explicit((_Atomic typeof(*(p)) *)(p),	\
			     memory_order_acquire)
#define atomic_store_relaxed(p, v)				\
	atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v),	\
			      memory_order_relaxed)
#define atomic_store_release(p, v)				\
	atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v),	\
			      memory_order_release)
#endif

/* IWYU pragma: begin_exports */
#if defined(__i386__)
#include "arch-x86.h"
#elif defined(__x86_64__)
#include "arch-x86_64.h"
#elif defined(__powerpc__) || defined(__powerpc64__) || defined(__ppc__)
#include "arch-ppc.h"
#elif defined(__ia64__)
#include "arch-ia64.h"
#elif defined(__alpha__)
#include "arch-alpha.h"
#elif defined(__s390x__) || defined(__s390__)
#include "arch-s390.h"
#elif defined(__sparc__)
#include "arch-sparc.h"
#elif defined(__sparc64__)
#include "arch-sparc64.h"
#elif defined(__arm__)
#include "arch-arm.h"
#elif defined(__mips__) || defined(__mips64__)
#include "arch-mips.h"
#elif defined(__sh__)
#include "arch-sh.h"
#elif defined(__hppa__)
#include "arch-hppa.h"
#elif defined(__aarch64__)
#include "arch-aarch64.h"
#elif defined(__loongarch64)
#include "arch-loongarch64.h"
#elif defined(__riscv) && __riscv_xlen == 64
#include "arch-riscv64.h"
#else
#warning "Unknown architecture, attempting to use generic model."
#include "arch-generic.h"
#endif

#if !defined(__x86_64__) && defined(CONFIG_SYNC_SYNC)
static inline void tsc_barrier(void)
{
	__sync_synchronize();
}
#endif

#include "../lib/ffz.h"
/* IWYU pragma: end_exports */

#ifndef ARCH_HAVE_INIT
static inline int arch_init(char *envp[])
{
	return 0;
}
#endif

#ifdef __alpha__
/*
 * alpha is the only exception, all other architectures
 * have common numbers for new system calls.
 */
# ifndef __NR_io_uring_setup
#  define __NR_io_uring_setup		535
# endif
# ifndef __NR_io_uring_enter
#  define __NR_io_uring_enter		536
# endif
# ifndef __NR_io_uring_register
#  define __NR_io_uring_register	537
# endif
#else /* !__alpha__ */
# ifndef __NR_io_uring_setup
#  define __NR_io_uring_setup		425
# endif
# ifndef __NR_io_uring_enter
#  define __NR_io_uring_enter		426
# endif
# ifndef __NR_io_uring_register
#  define __NR_io_uring_register	427
# endif
#endif

#define ARCH_HAVE_IOURING

#endif
