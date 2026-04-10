/*
 * [한국어 설명] SPARC 32비트 아키텍처 지원 헤더 (arch-sparc.h)
 *
 * === 파일의 역할 ===
 * SPARC 32비트 프로세서를 위한 최소한의 아키텍처 지원을 제공한다.
 * 캐시 라인 크기 정의와 컴파일러 메모리 배리어만 구현하며,
 * 하드웨어 고유 사이클 카운터 등은 제공하지 않고 제네릭 구현에 의존한다.
 *
 * === 제공하는 기능 ===
 * - nop: 빈 연산 매크로
 * - read_barrier() / write_barrier(): 컴파일러 수준 메모리 배리어
 */
#ifndef ARCH_SPARC_H
#define ARCH_SPARC_H

#define FIO_ARCH	(arch_sparc)

#define nop	do { } while (0)

#define read_barrier()	__asm__ __volatile__ ("" : : : "memory")
#define write_barrier()	__asm__ __volatile__ ("" : : : "memory")

#endif
