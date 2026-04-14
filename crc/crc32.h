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
 * [한국어 설명] CRC-32 체크섬 헤더 (crc32.h)
 *
 * === 파일의 역할 ===
 * POSIX.2 표준 CRC-32 체크섬 함수의 인터페이스를 정의한다.
 * CRC-32는 데이터 전송/저장 시 오류 검출에 널리 사용되는 순환 중복 검사 알고리즘이다.
 * 다항식 0x04C11DB7 (x^32 + x^26 + x^23 + ... + x + 1)을 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 데이터 무결성 검증(verify) 기능에서 CRC-32 체크섬 옵션 선택 시 사용된다.
 * 호출 체인: verify.c → fio_crc32() → (테이블 기반 계산)
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=crc32 옵션 시 데이터 무결성 검증
 * - crc/test.c: CRC-32 벤치마크 성능 테스트
 * - crc32c.h: 유사하지만 다른 다항식을 사용하는 CRC-32C (Castagnoli)
 */
#ifndef CRC32_H
#define CRC32_H

#include <inttypes.h>

extern uint32_t fio_crc32(const void * const, unsigned long);

#endif
