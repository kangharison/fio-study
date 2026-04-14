/*
 * [한국어 설명] SuperH (SH) 아키텍처 지원 헤더 (arch-sh.h)
 *
 * === 파일의 역할 ===
 * Renesas SuperH 32비트 프로세서를 위한 아키텍처별 지원을 제공한다.
 * ELF auxiliary vector에서 CPU 기능을 감지하여 synco 명령어 사용 가능 여부를
 * 판별하고, 이에 따라 적절한 메모리 배리어를 선택적으로 적용한다.
 *
 * === 제공하는 기능 ===
 * - mb(): CPU 기능에 따라 synco 또는 컴파일러 배리어를 선택하는 매크로
 * - read_barrier() / write_barrier(): mb() 기반 메모리 배리어
 * - arch_init(): ELF auxv에서 LLSC 기능 감지 및 arch_flags 설정
 * - CPU_HAS_LLSC: Load-Linked/Store-Conditional 기능 플래그
 */
/* Renesas SH (32bit) only */

#ifndef ARCH_SH_H
#define ARCH_SH_H

#define FIO_ARCH	(arch_sh)

#define nop             __asm__ __volatile__ ("nop": : :"memory")

#define mb()								\
	do {								\
		if (arch_flags & ARCH_FLAG_1)				\
			__asm__ __volatile__ ("synco": : :"memory");	\
		else							\
			__asm__ __volatile__ (" " : : : "memory");	\
	} while (0)

#define read_barrier()	mb()
#define write_barrier()	mb()

#include <stdio.h>
#include <elf.h>

extern unsigned long arch_flags;

#define CPU_HAS_LLSC	0x0040

/*
 * [한국어] arch_init - ELF auxiliary vector를 탐색하여 CPU_HAS_LLSC 기능을 감지한다.
 * LLSC(Load-Linked/Store-Conditional)가 지원되면 synco 메모리 배리어를 활성화한다.
 * envp 뒤의 ELF auxv 배열을 직접 순회하는 저수준 기법을 사용한다.
 */
static inline int arch_init(char *envp[])
{
	Elf32_auxv_t *auxv;

	while (*envp++ != NULL)
		;

	for (auxv = (Elf32_auxv_t *) envp; auxv->a_type != AT_NULL; auxv++) {
		if (auxv->a_type == AT_HWCAP) {
			if (auxv->a_un.a_val & CPU_HAS_LLSC) {
				arch_flags |= ARCH_FLAG_1;
				break;
			}
		}
	}

	return 0;
}

#define ARCH_HAVE_INIT

#endif
