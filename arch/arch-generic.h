/*
 * [한국어 설명] 제네릭/폴백 아키텍처 지원 헤더 (arch-generic.h)
 *
 * === 파일의 역할 ===
 * 특정 아키텍처가 감지되지 않을 때 사용되는 범용 폴백 구현을 제공한다.
 * 하드웨어 고유 최적화 없이 컴파일러 수준의 메모리 배리어만 제공하며,
 * 모든 플랫폼에서 최소한의 기능이 동작하도록 보장한다.
 *
 * === 제공하는 기능 ===
 * - nop: 빈 연산 매크로 (실제 동작 없음)
 * - read_barrier() / write_barrier(): 컴파일러 수준 메모리 배리어
 */
#ifndef ARCH_GENERIC_H
#define ARCH_GENERIC_H

#define FIO_ARCH	(arch_generic)

#define nop			do { } while (0)
#define read_barrier()		__asm__ __volatile__("": : :"memory")
#define write_barrier()		__asm__ __volatile__("": : :"memory")

#endif
