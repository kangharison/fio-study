/*
 * [한국어 설명] SPARC 64비트 아키텍처 지원 헤더 (arch-sparc64.h)
 *
 * === 파일의 역할 ===
 * SPARC 64비트 프로세서를 위한 아키텍처별 지원을 제공한다.
 * membar 명령어를 사용하여 하드웨어 수준의 메모리 배리어를 구현하며,
 * branch-always 명령어와 조합하여 안전한 배리어 실행을 보장한다.
 *
 * === 제공하는 기능 ===
 * - membar_safe(): membar 명령어를 안전하게 실행하는 매크로
 * - read_barrier(): #LoadLoad membar 기반 읽기 배리어
 * - write_barrier(): #StoreStore membar 기반 쓰기 배리어
 */
#ifndef ARCH_SPARC64_H
#define ARCH_SPARC64_H

#define FIO_ARCH	(arch_sparc64)

#define nop	do { } while (0)

/* [한국어] membar_safe - 분기 명령과 조합하여 membar를 안전하게 실행하는 매크로
 * SPARC에서는 membar가 branch delay slot에서 오동작할 수 있어 ba,pt로 감싸 보호한다 */
#define membar_safe(type) \
	do {    __asm__ __volatile__("ba,pt     %%xcc, 1f\n\t" \
					" membar   " type "\n" \
					"1:\n" \
					: : : "memory"); \
	} while (0)

#define read_barrier()		membar_safe("#LoadLoad")
#define write_barrier()		membar_safe("#StoreStore")

#endif
