/*
 * [한국어 설명] HP PA-RISC (HPPA) 아키텍처 지원 헤더 (arch-hppa.h)
 *
 * === 파일의 역할 ===
 * HP PA-RISC 프로세서를 위한 아키텍처별 지원을 제공한다.
 * cr16(Control Register 16) 사이클 카운터와 메모리 배리어를 정의하며,
 * 현재는 컴파일러 수준의 메모리 배리어만 구현되어 있다.
 *
 * === 제공하는 기능 ===
 * - nop: 빈 연산 매크로
 * - read_barrier() / write_barrier(): 컴파일러 수준 메모리 배리어
 */
#ifndef ARCH_HPPA_H
#define ARCH_HPPA_H

#define FIO_ARCH	(arch_hppa)

#define nop	do { } while (0)

#define read_barrier()	__asm__ __volatile__ ("" : : : "memory")
#define write_barrier()	__asm__ __volatile__ ("" : : : "memory")

#endif
