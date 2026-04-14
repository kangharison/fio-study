/*
 * [한국어 설명] 메인 아키텍처 추상화 헤더 (arch.h)
 *
 * === 파일의 역할 ===
 * 컴파일러 매크로(__i386__, __x86_64__, __aarch64__ 등)를 통해 현재 CPU 아키텍처를
 * 감지하고, 해당 아키텍처 전용 헤더 파일을 자동으로 포함시킨다. arch_t 열거형으로
 * 지원되는 모든 아키텍처를 정의하며, 원자적 연산 매크로와 io_uring 시스템 콜 번호 등
 * 공통 인프라를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전체에서 #include "arch/arch.h"로 포함되며, 아키텍처별 get_cpu_clock(),
 * arch_ffz(), read_barrier()/write_barrier() 등의 구현을 투명하게 제공한다.
 * gettime.c에서 get_cpu_clock()을, io_u.c에서 arch_ffz()를 사용한다.
 *
 * === 타 모듈과의 연결 ===
 * - 각 arch-*.h: 아키텍처별 구체 구현 (get_cpu_clock, arch_ffz, tsc_barrier 등)
 * - lib/ffz.h: arch_ffz 미지원 시 소프트웨어 폴백 FFZ 구현
 * - t/io_uring.c: __do_syscall 매크로로 io_uring 시스템 콜 직접 호출
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

/* [한국어] 지원되는 CPU 아키텍처 열거형 - 각 값은 아키텍처별 헤더와 1:1 대응 */
enum {
	arch_x86_64 = 1,	/* 64비트 x86 (AMD64/Intel 64) */
	arch_x86,		/* 32비트 x86 (i386) */
	arch_ppc,		/* PowerPC (32/64비트) */
	arch_ia64,		/* Intel Itanium (IA-64) */
	arch_s390,		/* IBM System/390 메인프레임 */
	arch_alpha,		/* DEC Alpha */
	arch_sparc,		/* SPARC 32비트 */
	arch_sparc64,		/* SPARC 64비트 */
	arch_arm,		/* ARM 32비트 */
	arch_sh,		/* Renesas SuperH */
	arch_hppa,		/* HP PA-RISC */
	arch_mips,		/* MIPS (32/64비트) */
	arch_aarch64,		/* ARM 64비트 (AArch64) */
	arch_loongarch64,	/* LoongArch 64비트 */
	arch_riscv64,		/* RISC-V 64비트 */

	arch_generic,		/* 미지원 아키텍처용 범용 폴백 */

	arch_nr,		/* 아키텍처 총 개수 (배열 크기용 센티널) */
};

/* [한국어] 아키텍처별 기능 플래그 - arch_init()에서 설정, 런타임 기능 분기에 사용 */
enum {
	ARCH_FLAG_1	= 1 << 0,	/* 예: PPC의 ATB(Alternate Time Base) 사용 여부 */
	ARCH_FLAG_2	= 1 << 1,
	ARCH_FLAG_3	= 1 << 2,
	ARCH_FLAG_4	= 1 << 3,
};

extern unsigned long arch_flags;

#define ARCH_CPU_CLOCK_WRAPS

/* [한국어] 원자적 연산 매크로 - C++과 C11 _Atomic을 추상화하여 스레드 안전 카운터 등에 사용 */
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

/* [한국어] x86_64 이외의 아키텍처에서 TSC 읽기 전후의 메모리 배리어 (x86_64는 mfence 사용) */
#if !defined(__x86_64__) && defined(CONFIG_SYNC_SYNC)
static inline void tsc_barrier(void)
{
	__sync_synchronize();
}
#endif

#include "../lib/ffz.h"
/* IWYU pragma: end_exports */

/* [한국어] 아키텍처 초기화 기본 구현 - ARCH_HAVE_INIT 미정의 시 빈 함수로 대체 */
#ifndef ARCH_HAVE_INIT
static inline int arch_init(char *envp[])
{
	return 0;
}
#endif

/*
 * [한국어] io_uring 시스템 콜 번호 정의
 * Alpha만 별도 번호(535~537)를 사용하고 나머지 아키텍처는 공통 번호(425~427)를 사용한다.
 * 커널 헤더에 정의가 없는 구형 환경에서도 io_uring을 직접 호출할 수 있도록 폴백을 제공한다.
 */
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
