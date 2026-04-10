/*
 * [한국어 설명] LoongArch 64비트 아키텍처 지원 헤더 (arch-loongarch64.h)
 *
 * === 파일의 역할 ===
 * LoongArch 64비트 프로세서를 위한 아키텍처별 지원을 제공한다.
 * RDTIME 명령어를 통한 사이클 카운터 읽기와 dbar(data barrier)
 * 명령어를 사용한 메모리 배리어를 구현한다.
 *
 * === 제공하는 기능 ===
 * - read_barrier() / write_barrier(): dbar 0 명령어 기반 메모리 배리어
 * - nop: dbar 0 명령어로 구현된 빈 연산
 */
#ifndef ARCH_LOONGARCH64_H
#define ARCH_LOONGARCH64_H

#define FIO_ARCH	(arch_loongarch64)

#define read_barrier()		__asm__ __volatile__("dbar 0": : :"memory")
#define write_barrier()		__asm__ __volatile__("dbar 0": : :"memory")
#define nop			__asm__ __volatile__("dbar 0": : :"memory")

#endif
