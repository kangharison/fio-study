/*
 * [한국어 설명] IBM System/390 (s390x) 아키텍처 지원 헤더 (arch-s390.h)
 *
 * === 파일의 역할 ===
 * IBM s390/s390x 메인프레임 프로세서를 위한 저수준 기능을 제공한다. STCK(Store
 * Clock) 또는 STCKF(Store Clock Fast) 명령어를 통한 고정밀 시간 측정, BCR 15,0
 * 명령어 기반의 메모리 배리어를 구현한다.
 *
 * === 제공하는 기능 ===
 * - get_cpu_clock(): STCK/STCKF 명령어로 TOD 클록 값 읽기 (마이크로초 단위)
 * - read_barrier(), write_barrier(): BCR 15,0 직렬화 메모리 배리어
 * - ARCH_CPU_CLOCK_CYCLES_PER_USEC: 클록 사이클/마이크로초 비율 (1)
 * - arch_init(): tsc_reliable을 true로 설정하는 초기화 함수
 */
#ifndef ARCH_S390_H
#define ARCH_S390_H

#define FIO_ARCH	(arch_s390)

#define nop		asm volatile("nop" : : : "memory")
#define read_barrier()	asm volatile("bcr 15,0" : : : "memory")
#define write_barrier()	asm volatile("bcr 15,0" : : : "memory")

/*
 * [한국어] get_cpu_clock - STCK/STCKF 명령어로 TOD(Time of Day) 클록을 읽는다.
 * z196 이상에서는 STCKF(fast 버전)를 사용하고, 구형에서는 STCK를 사용한다.
 * 반환값을 12비트 우시프트하여 마이크로초 단위로 변환한다.
 */
static inline unsigned long long get_cpu_clock(void)
{
	unsigned long long clk;

#ifdef CONFIG_S390_Z196_FACILITIES
	/*
	 * Fio needs monotonic (never lower), but not strict monotonic (never
	 * the same) so store clock fast is enough.
	 */
	__asm__ __volatile__("stckf %0" : "=Q" (clk) : : "cc");
#else
	__asm__ __volatile__("stck %0" : "=Q" (clk) : : "cc");
#endif
	return clk>>12;
}

#define ARCH_CPU_CLOCK_CYCLES_PER_USEC 1
#define ARCH_HAVE_CPU_CLOCK
#undef ARCH_CPU_CLOCK_WRAPS

#define ARCH_HAVE_INIT
extern bool tsc_reliable;
static inline int arch_init(char *envp[])
{
	tsc_reliable = true;
	return 0;
}

#endif
