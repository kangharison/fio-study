/*
 * [한국어 설명] Intel Itanium (IA-64) 아키텍처 지원 헤더 (arch-ia64.h)
 *
 * === 파일의 역할 ===
 * Intel Itanium (IA-64) 프로세서를 위한 아키텍처별 지원을 제공한다.
 * ar.itc(Interval Time Counter) 레지스터를 통해 CPU 사이클 카운터를 읽고,
 * mf(memory fence) 명령어로 메모리 배리어를 구현한다.
 *
 * === 제공하는 기능 ===
 * - get_cpu_clock(): ar.itc 레지스터를 이용한 사이클 카운터 읽기
 * - read_barrier() / write_barrier(): mf 명령어 기반 메모리 배리어
 * - ia64_popcnt(): 인라인 어셈블리 기반 population count
 * - arch_ffz(): 첫 번째 0 비트 탐색 함수
 * - arch_init(): TSC reliable 플래그 설정 초기화
 */
#ifndef ARCH_IA64_H
#define ARCH_IA64_H

#define FIO_ARCH	(arch_ia64)

#define nop		asm volatile ("hint @pause" ::: "memory");
#define read_barrier()	asm volatile ("mf" ::: "memory")
#define write_barrier()	asm volatile ("mf" ::: "memory")

/* [한국어] ia64_popcnt - Itanium의 popcnt 명령어로 설정된 비트 수를 센다 */
#define ia64_popcnt(x)							\
({									\
	unsigned long ia64_intri_res;					\
	asm ("popcnt %0=%1" : "=r" (ia64_intri_res) : "r" (x));		\
	ia64_intri_res;							\
})

/* [한국어] arch_ffz - popcnt를 활용하여 최하위 0 비트의 위치를 구한다 */
static inline unsigned long arch_ffz(unsigned long bitmask)
{
	return ia64_popcnt(bitmask & (~bitmask - 1));
}

/* [한국어] get_cpu_clock - ar.itc(Interval Time Counter) 레지스터에서 사이클 카운터를 읽는다 */
static inline unsigned long long get_cpu_clock(void)
{
	unsigned long long ret;

	__asm__ __volatile__("mov %0=ar.itc" : "=r" (ret) : : "memory");
	return ret;
}

#define ARCH_HAVE_INIT
extern bool tsc_reliable;
static inline int arch_init(char *envp[])
{
	tsc_reliable = true;
	return 0;
}

#define ARCH_HAVE_FFZ
#define ARCH_HAVE_CPU_CLOCK

#endif
