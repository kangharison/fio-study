/*
 * [한국어 설명] DEC Alpha 아키텍처 지원 헤더 (arch-alpha.h)
 *
 * === 파일의 역할 ===
 * DEC Alpha 프로세서를 위한 아키텍처별 지원을 제공한다.
 * RPCC(Read Processor Cycle Counter) 명령어를 통한 사이클 카운터와
 * mb(memory barrier)/wmb(write memory barrier) 명령어 기반 메모리 배리어를 구현한다.
 *
 * === 제공하는 기능 ===
 * - nop: 빈 연산 매크로
 * - read_barrier(): mb 명령어 기반 읽기 메모리 배리어
 * - write_barrier(): wmb 명령어 기반 쓰기 메모리 배리어
 */
#ifndef ARCH_ALPHA_H
#define ARCH_ALPHA_H

#define FIO_ARCH	(arch_alpha)

#define nop			do { } while (0)
#define read_barrier()		__asm__ __volatile__("mb": : :"memory")
#define write_barrier()		__asm__ __volatile__("wmb": : :"memory")

#endif
