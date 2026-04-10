/*
 * [한국어 설명] MIPS 아키텍처 지원 헤더 (arch-mips.h)
 *
 * === 파일의 역할 ===
 * MIPS 프로세서를 위한 아키텍처별 지원을 제공한다.
 * RDHWR 명령어를 통한 사이클 카운터 읽기와 sync 명령어 기반의
 * 메모리 배리어를 구현하며, __SANE_USERSPACE_TYPES__ 매크로를 정의한다.
 *
 * === 제공하는 기능 ===
 * - read_barrier() / write_barrier(): 컴파일러 수준 메모리 배리어
 * - nop: 컴파일러 메모리 배리어로 구현된 빈 연산
 * - __SANE_USERSPACE_TYPES__: 사용자 공간 타입 호환성 매크로
 */
#ifndef ARCH_MIPS64_H
#define ARCH_MIPS64_H

#define FIO_ARCH	(arch_mips)

#ifndef __SANE_USERSPACE_TYPES__
#define __SANE_USERSPACE_TYPES__
#endif

#define read_barrier()		__asm__ __volatile__("": : :"memory")
#define write_barrier()		__asm__ __volatile__("": : :"memory")
#define nop			__asm__ __volatile__("": : :"memory")

#endif
