/* crc32c -- calculate and POSIX.2 checksum
   Copyright (C) 92, 1995-1999 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.  */

/*
 * [한국어 설명] CRC-32C (Castagnoli) 체크섬 헤더 (crc32c.h)
 *
 * === 파일의 역할 ===
 * CRC-32C(Castagnoli 다항식, 0x1EDC6F41) 체크섬의 인터페이스를 정의한다.
 * CRC-32C는 iSCSI, NVMe, ext4 등에서 널리 사용되는 체크섬으로,
 * Intel SSE4.2와 ARM CRC32 명령어로 하드웨어 가속이 가능하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 verify 기능에서 가장 자주 사용되는 체크섬 알고리즘 중 하나이다.
 * 런타임에 CPU가 하드웨어 CRC32C를 지원하는지 감지(probe)하고,
 * 지원 시 하드웨어 가속, 미지원 시 소프트웨어 폴백을 자동 선택한다.
 * 호출 체인: verify.c → fio_crc32c() → crc32c_intel/arm64/sw 중 택1
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=crc32c 옵션 시 데이터 무결성 검증
 * - crc32c.c: 소프트웨어 구현 (테이블 기반)
 * - crc32c-intel.c: Intel SSE4.2 하드웨어 가속 구현
 * - crc32c-arm64.c: ARM64 CRC 확장 하드웨어 가속 구현
 * - arch/arch.h: ARCH_HAVE_SSE4_2, ARCH_HAVE_CRC_CRYPTO 매크로 정의
 *
 * === 주요 함수 요약 ===
 * - fio_crc32c(): 런타임에 최적 구현을 자동 선택하는 래퍼 (인라인)
 * - crc32c_sw(): 소프트웨어 폴백 구현
 * - crc32c_intel(): Intel SSE4.2 하드웨어 가속 (조건부 컴파일)
 * - crc32c_arm64(): ARM64 CRC 하드웨어 가속 (조건부 컴파일)
 * - crc32c_intel_probe()/crc32c_arm64_probe(): CPU 기능 감지
 */
#ifndef CRC32C_H
#define CRC32C_H

#include <inttypes.h>

#include "../arch/arch.h"
#include "../lib/types.h"

/* [한국어] 소프트웨어 CRC-32C 구현 - 모든 플랫폼에서 사용 가능한 폴백 */
extern uint32_t crc32c_sw(unsigned char const *, unsigned long);
/* [한국어] ARM64 CRC 하드웨어 가속 사용 가능 여부 (런타임 probe에서 설정) */
extern bool crc32c_arm64_available;
/* [한국어] Intel SSE4.2 CRC 하드웨어 가속 사용 가능 여부 (런타임 probe에서 설정) */
extern bool crc32c_intel_available;

#ifdef ARCH_HAVE_CRC_CRYPTO
extern uint32_t crc32c_arm64(unsigned char const *, unsigned long);
extern void crc32c_arm64_probe(void);
#else
#define crc32c_arm64 crc32c_sw
static inline void crc32c_arm64_probe(void)
{
}
#endif /* ARCH_HAVE_CRC_CRYPTO */

#ifdef ARCH_HAVE_SSE4_2
extern uint32_t crc32c_intel(unsigned char const *, unsigned long);
extern void crc32c_intel_probe(void);
#else
#define crc32c_intel crc32c_sw
static inline void crc32c_intel_probe(void)
{
}
#endif /* ARCH_HAVE_SSE4_2 */

/*
 * [한국어]
 * fio_crc32c - CRC-32C 체크섬 계산 (최적 구현 자동 선택)
 *
 * @buf: CRC를 계산할 데이터 버퍼
 * @len: 데이터 길이(바이트)
 * @return: 계산된 32비트 CRC-32C 체크섬
 *
 * 런타임에 CPU 기능을 감지하여 최적의 구현을 선택한다:
 *   1순위: ARM64 CRC 확장 (crc32c_arm64) - ARM 서버/모바일
 *   2순위: Intel SSE4.2 CRC32 명령어 (crc32c_intel) - x86_64
 *   3순위: 소프트웨어 테이블 룩업 (crc32c_sw) - 범용 폴백
 * probe 함수들이 프로그램 시작 시 호출되어 available 플래그를 설정한다.
 */
static inline uint32_t fio_crc32c(unsigned char const *buf, unsigned long len)
{
	if (crc32c_arm64_available)
		return crc32c_arm64(buf, len);

	if (crc32c_intel_available)
		return crc32c_intel(buf, len);

	return crc32c_sw(buf, len);
}

#endif
