/*
 * [한국어 설명] ARM 32비트 아키텍처 지원 헤더 (arch-arm.h)
 *
 * === 파일의 역할 ===
 * 32비트 ARM 프로세서를 위한 기본 기능을 제공한다. ARMv4~v6 구형 코어에서는
 * 컴파일러 배리어만 사용하고, ARMv7A/v8A 이상에서는 __sync_synchronize를
 * 통한 하드웨어 메모리 배리어를 사용하는 조건부 컴파일을 수행한다.
 *
 * === 제공하는 기능 ===
 * - nop: ARM 버전별 NOP 명령어 (mov r0,r0 또는 nop)
 * - read_barrier(): 읽기 메모리 배리어 (ARMv7+ 하드웨어 / ARMv6- 컴파일러)
 * - write_barrier(): 쓰기 메모리 배리어 (ARMv7+ 하드웨어 / ARMv6- 컴파일러)
 */
#ifndef ARCH_ARM_H
#define ARCH_ARM_H

#define FIO_ARCH	(arch_arm)

#if defined (__ARM_ARCH_4__) || defined (__ARM_ARCH_4T__) \
	|| defined (__ARM_ARCH_5__) || defined (__ARM_ARCH_5T__) || defined (__ARM_ARCH_5E__)\
	|| defined (__ARM_ARCH_5TE__) || defined (__ARM_ARCH_5TEJ__) \
	|| defined(__ARM_ARCH_6__)  || defined(__ARM_ARCH_6J__) || defined(__ARM_ARCH_6Z__) || defined(__ARM_ARCH_6ZK__) \
	|| defined(__ARM_ARCH_6KZ__) || defined(__ARM_ARCH_6K__)
#define nop             __asm__ __volatile__("mov\tr0,r0\t@ nop\n\t")
#define read_barrier()	__asm__ __volatile__ ("" : : : "memory")
#define write_barrier()	__asm__ __volatile__ ("" : : : "memory")
#elif defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7VE__) || defined(__ARM_ARCH_8A__)
#define	nop		__asm__ __volatile__ ("nop")
#define read_barrier()	__sync_synchronize()
#define write_barrier()	__sync_synchronize()
#else
#error "unsupported ARM architecture"
#endif

#endif
