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
 * CRC-32C(Castagnoli 다항식, reflected form 0x1EDC6F41 / 실제 머신 표현
 * 0x82F63B78) 의 공개 API 를 정의하고, 하드웨어 가속 가능 여부에 따라
 * Intel SSE4.2(_mm_crc32_u64) 또는 ARMv8 CRC32(__crc32cd) 구현을 런타임에
 * 선택하는 **디스패처 인라인**(fio_crc32c) 을 제공한다. CRC-32C 는 iSCSI,
 * SCTP, NVMe PI Type 0(32B), Btrfs, ext4 메타데이터 체크섬, Intel Storage
 * 등에서 표준 체크섬으로 사용되며 fio 에서는 verify=crc32c 모드로 호출된다.
 * 소프트웨어 폴백은 256 엔트리 reflected 룩업 테이블(crc32c.c 의 crc32c_table)
 * 을 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   1) verify.c::fill_crc32c() / verify_io_u_crc32c()
 *        → fio_crc32c(buf, len) [본 헤더 인라인]
 *            → ARM64 사용 가능? crc32c_arm64() : Intel 사용 가능? crc32c_intel() : crc32c_sw()
 *   2) verify_header 자체의 crc32 필드도 CRC-32C 로 해싱(헤더 자신 제외 영역).
 *
 * probe 단계:
 *   init.c 의 crc32c_intel_probe() / crc32c_arm64_probe() 가 CPUID / HWCAP
 *   검사 후 전역 crc32c_intel_available / crc32c_arm64_available 플래그를 설정.
 *
 * === 타 모듈과의 연결 ===
 * - crc32c.c: 소프트웨어 폴백 구현(테이블 기반).
 * - crc32c-intel.c: Intel SSE4.2 CRC32 명령어 구현(ARCH_HAVE_SSE4_2 빌드 시).
 * - crc32c-arm64.c: ARMv8 CRC32 확장 구현(ARCH_HAVE_CRC_CRYPTO 빌드 시).
 * - arch/arch.h: ARCH_HAVE_SSE4_2, ARCH_HAVE_CRC_CRYPTO 정의 여부.
 * - lib/types.h: bool 타입 공급(전역 플래그).
 * - verify.c: verify=crc32c 옵션 및 verify_header 체크섬.
 * - inttypes.h: uint32_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_crc32c(): 런타임 디스패처 인라인 — ARM64 → Intel → SW 순 폴백.
 * - crc32c_sw(): 소프트웨어 테이블 폴백(모든 플랫폼 사용 가능).
 * - crc32c_intel()/probe(): SSE4.2 가속 구현 및 CPU 감지.
 * - crc32c_arm64()/probe(): ARMv8 CRC 가속 구현 및 감지.
 */
#ifndef CRC32C_H
/* [한국어] 헤더 가드 — verify.c / 각 백엔드 구현 / test.c 동시 포함 대비. */
#define CRC32C_H

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함: uint32_t 고정폭 — CRC-32C 결과 타입이 정확히 32비트여야 함. */

#include "../arch/arch.h"
/* [한국어] ../arch/arch.h 포함: ARCH_HAVE_SSE4_2, ARCH_HAVE_CRC_CRYPTO 매크로 정의.
 * 이 매크로들은 configure 빌드 시 아키텍처별로 정의되어 하드웨어 가속 구현의
 * 컴파일 여부를 결정한다. */

#include "../lib/types.h"
/* [한국어] ../lib/types.h 포함: bool 타입 정의. C99 이전 컴파일러 호환 목적으로
 * fio 는 자체 bool/true/false 타입을 lib/types.h 에서 제공한다. */

/* [한국어] 소프트웨어 CRC-32C 구현 — 모든 플랫폼에서 사용 가능한 기본 폴백.
 * 호출 체인: fio_crc32c() 에서 두 HW 플래그가 모두 false 일 때 호출. */
extern uint32_t crc32c_sw(unsigned char const *, unsigned long);

/* [한국어] ARM64 CRC 하드웨어 가속 사용 가능 여부. crc32c_arm64_probe() 가 런타임에 설정.
 * 설정자: crc32c_arm64_probe() — getauxval(AT_HWCAP) & HWCAP_CRC32 검사.
 * 읽는 자: fio_crc32c() 인라인 분기.
 * 값 범위: true/false. 초기값 false(probe 전 기본).
 * 동기화: probe 는 init 단계(단일 스레드)에 한 번 호출되고 이후 읽기 전용이라 안전. */
extern bool crc32c_arm64_available;

/* [한국어] Intel SSE4.2 CRC 하드웨어 가속 사용 가능 여부.
 * 설정자: crc32c_intel_probe() — CPUID(EAX=1).ECX bit20 검사.
 * 읽는 자: fio_crc32c() 인라인 분기. */
extern bool crc32c_intel_available;

#ifdef ARCH_HAVE_CRC_CRYPTO
/* [한국어] ARCH_HAVE_CRC_CRYPTO 정의 시: ARMv8 CRC32 확장(FEAT_CRC32) 사용 가능 빌드.
 * crc32c_arm64() 는 __crc32cd/cw/ch/cb 인트린직과 1024B 3-way PMULL reduction 을 사용. */

/* [한국어] ARMv8 CRC 명령어 기반 CRC-32C 구현 선언. */
extern uint32_t crc32c_arm64(unsigned char const *, unsigned long);
/* [한국어] HWCAP_CRC32 비트 검사로 crc32c_arm64_available 설정(초기화 단계에서 1회 호출). */
extern void crc32c_arm64_probe(void);
#else
/* [한국어] ARCH_HAVE_CRC_CRYPTO 미정의: 하드웨어 가속 빌드 불가 플랫폼(x86 등).
 * crc32c_arm64 심볼을 crc32c_sw 로 치환하여 fio_crc32c() 인라인이 동일 이름으로
 * 호출 가능하게 유지. probe 는 no-op. */
#define crc32c_arm64 crc32c_sw
static inline void crc32c_arm64_probe(void)
{
	/* [한국어] ARM64 미빌드 — 검사할 기능 없음. 인라인 빈 함수로 호출부를 단순화. */
}
#endif /* ARCH_HAVE_CRC_CRYPTO */

#ifdef ARCH_HAVE_SSE4_2
/* [한국어] ARCH_HAVE_SSE4_2 정의 시: x86_64 + SSE4.2 CRC32 명령어 사용 가능 빌드.
 * crc32c_intel() 는 CRC32 opcode(0xF2 0x0F 0x38 0xF0/0xF1) 또는 _mm_crc32_u64 인트린직으로 구현. */

extern uint32_t crc32c_intel(unsigned char const *, unsigned long);
/* [한국어] CPUID(EAX=1).ECX bit20 (SSE4.2) 검사로 crc32c_intel_available 설정. */
extern void crc32c_intel_probe(void);
#else
/* [한국어] SSE4.2 미빌드(예: ARM64, 32비트 x86 일부): crc32c_intel 호출 시 SW 폴백. */
#define crc32c_intel crc32c_sw
static inline void crc32c_intel_probe(void)
{
	/* [한국어] SSE4.2 미빌드 — 검사 불필요. 빈 인라인 함수. */
}
#endif /* ARCH_HAVE_SSE4_2 */

/*
 * [한국어]
 * fio_crc32c - CRC-32C 체크섬 계산 (런타임 최적 구현 자동 선택)
 *
 * @buf: CRC 계산 대상 버퍼. NULL 금지.
 * @len: 바이트 수(unsigned long — 실질 무제한).
 * @return: 32비트 CRC-32C 체크섬.
 *
 * 동작 우선순위:
 *   1) crc32c_arm64_available == true  → crc32c_arm64(): ARMv8 FEAT_CRC32 가속
 *   2) crc32c_intel_available == true  → crc32c_intel(): SSE4.2 CRC32 명령어
 *   3) 그 외                           → crc32c_sw():    256엔트리 테이블 폴백
 *
 * probe 함수들이 프로그램 초기화에서 available 플래그를 설정한 뒤 본 인라인이 분기.
 *
 * 호출 체인:
 *   verify.c::fill_crc32c() → fio_crc32c() → HW/SW 구현 → v_crc32c 저장
 *   verify.c::__fill_hdr(), __hdr_crc() → verify_header 자체 체크섬에도 사용
 *
 * 실행 컨텍스트: 잡 스레드(verify) 또는 벤치마크 스레드. 에러 경로: 없음.
 */
static inline uint32_t fio_crc32c(unsigned char const *buf, unsigned long len)
{
	/* [한국어] 1순위: ARM64 HW 가속. probe 에서 HWCAP_CRC32 검출 시 true. */
	if (crc32c_arm64_available)
		return crc32c_arm64(buf, len);

	/* [한국어] 2순위: Intel SSE4.2 CRC32 명령어. probe 에서 CPUID bit20 검출 시 true. */
	if (crc32c_intel_available)
		return crc32c_intel(buf, len);

	/* [한국어] 3순위: 소프트웨어 테이블 폴백 — 항상 사용 가능. */
	return crc32c_sw(buf, len);
}

#endif
