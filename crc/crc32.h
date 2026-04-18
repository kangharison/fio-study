/* crc32 -- calculate and POSIX.2 checksum
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
 * [한국어 설명] POSIX.2 CRC-32 체크섬 헤더 (crc32.h)
 *
 * === 파일의 역할 ===
 * POSIX.2 `cksum` 유틸리티가 사용하는 non-reflected CRC-32 의 공개 API 를 정의한다.
 * 다항식 0x04C11DB7 (x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 +
 * x^8 + x^7 + x^5 + x^4 + x^2 + x + 1) 을 사용하며, Ethernet/PNG/MPEG2 등에서
 * 널리 쓰이는 고전적 CRC-32 이다.
 *
 * 주의: 본 CRC 는 iSCSI/SCTP/ext4 에서 쓰는 **CRC-32C(Castagnoli, 다항식 0x1EDC6F41)**
 * 와 다항식이 완전히 다르다. 혼동 방지를 위해 verify 옵션 이름도 분리되어 있다
 * (verify=crc32 vs verify=crc32c).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_crc32() / verify_io_u_crc32()
 *     → fio_crc32(buf, len) — 본 헤더 선언, crc32.c 에서 테이블 기반 루프로 구현
 *
 * === 타 모듈과의 연결 ===
 * - crc32.c: fio_crc32() 실제 구현(256 엔트리 crctab 룩업, non-reflected).
 * - verify.c: verify=crc32 옵션의 fill/verify 경로.
 * - crc/test.c: --crctest=crc32 벤치마크 등록.
 * - crc/crc32c.h: 유사 이름이지만 다른 CRC(Castagnoli) — 구현 파일 분리.
 * - inttypes.h: uint32_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_crc32(buf, len): 버퍼 전체의 CRC-32 (non-reflected) 계산.
 *   내부 초기값 0, 반환값은 POSIX.2 cksum 기대 포맷에 가깝지만 length 포함
 *   여부는 구현에 따라 다름(구현 주석 확인).
 */
#ifndef CRC32_H
/* [한국어] 헤더 가드 — verify.c / crc32.c / test.c 동시 포함 대비. */
#define CRC32_H

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: uint32_t 고정폭 정의 — 32비트 CRC 결과가
 * 플랫폼별 unsigned long 크기(32/64비트)에 의존하지 않게 하기 위함. */

/*
 * [한국어]
 * fio_crc32 - POSIX.2 non-reflected CRC-32 계산
 *
 * @buf: 계산 대상 데이터 포인터. 내부적으로 unsigned char* 로 재해석.
 *       const void * const 시그니처는 호출자가 포인터 자체도 재할당하지 않음을 명시.
 * @len: 바이트 수. unsigned long 이므로 LP64 플랫폼에서 사실상 무한대, verify
 *       블록은 통상 1 MiB 이하.
 * @return: 32비트 CRC-32 체크섬(다항식 0x04C11DB7, 초기값 0, 최종 XOR 없음 — 구현 확인).
 *
 * 동작: crctab[((crc>>24) ^ byte) & 0xFF] 룩업 방식으로 바이트 단위 처리.
 *
 * 호출 체인: verify.c::fill_crc32() → fio_crc32() → v_crc32 저장.
 * 실행 컨텍스트: 잡 스레드(verify) 또는 벤치마크 스레드. 에러: 없음.
 */
extern uint32_t fio_crc32(const void * const, unsigned long);

#endif
