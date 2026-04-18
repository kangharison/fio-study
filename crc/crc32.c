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
 * [한국어 설명] POSIX.2 표준 CRC-32 구현 — 다항식 0x04C11DB7(big-endian) (crc32.c)
 *
 * === 파일의 역할 ===
 * POSIX.2 `cksum` 호환 형태의 "non-reflected" CRC-32 를 소프트웨어로 계산한다.
 * 생성 다항식은 0x04C11DB7(이더넷/PKZIP/gzip 과 동일한 generator, 본 구현은
 * 반전하지 않은 MSB-first 형태)이며, 초기값 0, 최종 XOR 없음.
 * 런타임 경로는 단 하나 — 256엔트리 룩업 테이블 `crctab[]`과 while 루프.
 * 하드웨어 가속 버전은 없다(CRC-32C 와 혼동 주의: 하드웨어 CRC32 명령은 Castagnoli
 * 다항식을 쓰며, 본 파일과는 다른 결과를 낸다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 경로의 VERIFY_CRC32 공급자.
 * 쓰기 시 fill_crc32(verify.c) → fio_crc32() [이 파일]이 verify_header.v_crc32 (4B)를
 * 채우고, 읽기 시 verify_io_u_crc32()가 재계산 후 비교. CRC-32C 는 별도 경로
 * (crc32c.c + crc32c-intel.c + crc32c-arm64.c + fio_crc32c 인라인 디스패치)이며,
 * 본 함수는 순수 POSIX CRC-32 옵션에서만 선택된다.
 * 호출 체인:
 *   verify.c::fill_crc32/verify_io_u_crc32 → [fio_crc32] → crctab[] 참조
 *   crc/test.c::t_crc32 → [fio_crc32]
 *
 * === 타 모듈과의 연결 ===
 * - crc32.h: fio_crc32() 프로토타입과 (부가) `uint32_t` 타입 재노출.
 * - verify.c: verify=crc32 옵션이 선택되면 fill_crc32/verify_io_u_crc32 에서 호출.
 * - crc/test.c: --crctest 벤치마크의 t_crc32 래퍼.
 * - crc32c.c / crc32c-intel.c / crc32c-arm64.c: 혼동 방지 — 그쪽은 Castagnoli 다항식.
 * 데이터 흐름: 쓰기 버퍼 → fio_crc32(초기 0) → verify_header.v_crc32 (4B).
 * 동기화: 순수 계산 함수 — 재진입 안전. crctab[] 은 읽기 전용 상수.
 *
 * === 주요 함수 요약 ===
 * - crctab[256]: 다항식 0x04C11DB7 로부터 생성된 MSB-first(non-reflected) 상수 테이블.
 * - fio_crc32(buffer, length): 초기값 0에서 시작해 바이트 단위로 CRC 갱신 후 반환.
 *   최종 XOR(~crc) 적용 없음 — 호출자가 필요하면 스스로 수행.
 */
#include "crc32.h"
/* [한국어] crc32.h: fio_crc32() 프로토타입과 uint32_t 타입 재노출.
 * verify.c / crc/test.c 가 동일 헤더로 ABI 공유. */

/*
 * [한국어] CRC-32 룩업 테이블 (256개 × 4B = 1024B 상수).
 * 설정자: 컴파일타임 하드코딩. 다항식 0x04C11DB7 의 MSB-first(반사 없음) 분할 결과.
 * 읽는 자: 본 파일의 fio_crc32() 루프만(static 으로 외부 노출 차단).
 * 값 범위: 엔트리 각 uint32_t 전체 범위. table[0]==0 고정(단위원).
 * 동기화: .rodata 상주 — 락 없이 모든 스레드 공유 가능.
 *
 * 본 테이블 덕분에 런타임은 비트별 8회 shift/xor 대신 "XOR 1 + 시프트 1 + 테이블 조회 1"
 * 로 바이트 한 개분 CRC 를 갱신한다 — 약 8배 가속.
 */
static const uint32_t crctab[256] = {
  0x0,
  0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B,
  0x1A864DB2, 0x1E475005, 0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6,
  0x2B4BCB61, 0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
  0x4C11DB70, 0x48D0C6C7, 0x4593E01E, 0x4152FDA9, 0x5F15ADAC,
  0x5BD4B01B, 0x569796C2, 0x52568B75, 0x6A1936C8, 0x6ED82B7F,
  0x639B0DA6, 0x675A1011, 0x791D4014, 0x7DDC5DA3, 0x709F7B7A,
  0x745E66CD, 0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039,
  0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5, 0xBE2B5B58,
  0xBAEA46EF, 0xB7A96036, 0xB3687D81, 0xAD2F2D84, 0xA9EE3033,
  0xA4AD16EA, 0xA06C0B5D, 0xD4326D90, 0xD0F37027, 0xDDB056FE,
  0xD9714B49, 0xC7361B4C, 0xC3F706FB, 0xCEB42022, 0xCA753D95,
  0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1, 0xE13EF6F4,
  0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D, 0x34867077, 0x30476DC0,
  0x3D044B19, 0x39C556AE, 0x278206AB, 0x23431B1C, 0x2E003DC5,
  0x2AC12072, 0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16,
  0x018AEB13, 0x054BF6A4, 0x0808D07D, 0x0CC9CDCA, 0x7897AB07,
  0x7C56B6B0, 0x71159069, 0x75D48DDE, 0x6B93DDDB, 0x6F52C06C,
  0x6211E6B5, 0x66D0FB02, 0x5E9F46BF, 0x5A5E5B08, 0x571D7DD1,
  0x53DC6066, 0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
  0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E, 0xBFA1B04B,
  0xBB60ADFC, 0xB6238B25, 0xB2E29692, 0x8AAD2B2F, 0x8E6C3698,
  0x832F1041, 0x87EE0DF6, 0x99A95DF3, 0x9D684044, 0x902B669D,
  0x94EA7B2A, 0xE0B41DE7, 0xE4750050, 0xE9362689, 0xEDF73B3E,
  0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2, 0xC6BCF05F,
  0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686, 0xD5B88683, 0xD1799B34,
  0xDC3ABDED, 0xD8FBA05A, 0x690CE0EE, 0x6DCDFD59, 0x608EDB80,
  0x644FC637, 0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB,
  0x4F040D56, 0x4BC510E1, 0x46863638, 0x42472B8F, 0x5C007B8A,
  0x58C1663D, 0x558240E4, 0x51435D53, 0x251D3B9E, 0x21DC2629,
  0x2C9F00F0, 0x285E1D47, 0x36194D42, 0x32D850F5, 0x3F9B762C,
  0x3B5A6B9B, 0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF,
  0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623, 0xF12F560E,
  0xF5EE4BB9, 0xF8AD6D60, 0xFC6C70D7, 0xE22B20D2, 0xE6EA3D65,
  0xEBA91BBC, 0xEF68060B, 0xD727BBB6, 0xD3E6A601, 0xDEA580D8,
  0xDA649D6F, 0xC423CD6A, 0xC0E2D0DD, 0xCDA1F604, 0xC960EBB3,
  0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7, 0xAE3AFBA2,
  0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B, 0x9B3660C6, 0x9FF77D71,
  0x92B45BA8, 0x9675461F, 0x8832161A, 0x8CF30BAD, 0x81B02D74,
  0x857130C3, 0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640,
  0x4E8EE645, 0x4A4FFBF2, 0x470CDD2B, 0x43CDC09C, 0x7B827D21,
  0x7F436096, 0x7200464F, 0x76C15BF8, 0x68860BFD, 0x6C47164A,
  0x61043093, 0x65C52D24, 0x119B4BE9, 0x155A565E, 0x18197087,
  0x1CD86D30, 0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC,
  0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088, 0x2497D08D,
  0x2056CD3A, 0x2D15EBE3, 0x29D4F654, 0xC5A92679, 0xC1683BCE,
  0xCC2B1D17, 0xC8EA00A0, 0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB,
  0xDBEE767C, 0xE3A1CBC1, 0xE760D676, 0xEA23F0AF, 0xEEE2ED18,
  0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4, 0x89B8FD09,
  0x8D79E0BE, 0x803AC667, 0x84FBDBD0, 0x9ABC8BD5, 0x9E7D9662,
  0x933EB0BB, 0x97FFAD0C, 0xAFB010B1, 0xAB710D06, 0xA6322BDF,
  0xA2F33668, 0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4
};

/*
 * [한국어]
 * fio_crc32 - 버퍼의 POSIX.2 CRC-32 체크섬 계산
 *
 * @buffer: 바이트 데이터 포인터.
 * @length: 길이(바이트, unsigned long — 대용량 버퍼도 한 번에 처리 가능).
 * @return: 32비트 CRC. 최종 반전(~) 적용 없음(일부 CRC-32 표준은 최종 XOR 0xFFFFFFFF
 *          을 요구하는데, 본 함수는 호출자에게 위임).
 *
 * 알고리즘(non-reflected):
 *   crc <<= 8 로 CRC 를 상위로 밀고,
 *   (crc >> 24) 로 곧 버려질 최상위 바이트를 추출해 입력 바이트와 XOR 해 인덱스 생성,
 *   crctab[index] 와 XOR 하여 다항식 분할 한 바이트 분을 완료.
 *
 * 실행 컨텍스트: 잡/verify 스레드, 재진입 안전. 전역 상태 없음.
 *
 * 호출 체인:
 *   verify.c::fill_crc32/verify_io_u_crc32 → [fio_crc32] → crctab[]
 *   crc/test.c::t_crc32 → [fio_crc32]
 */
uint32_t fio_crc32(const void *buffer, unsigned long length)
{
	/* [한국어] void* 를 바이트 포인터로 재해석 — 포인터 산술(cp++)과 역참조(*cp)용. */
	const unsigned char *cp = (const unsigned char *) buffer;
	/* [한국어] CRC-32 누적 레지스터. 초기값 0(POSIX.2). */
	uint32_t crc = 0;

	/* [한국어] length 후위 감소로 정확히 length 회 반복. length==0 이면 초기 CRC 그대로 반환. */
	while (length--)
		/* [한국어] 한 바이트분 다항식 분할:
		 *   (crc >> 24)        — 곧 밖으로 밀려날 최상위 8비트를 하위로 이동.
		 *   ^ *(cp++)          — 이번 입력 바이트와 XOR 로 테이블 인덱스 8비트 생성,
		 *                        cp 를 다음 바이트로 진행.
		 *   & 0xFF             — 8비트로 마스킹(이미 상위는 비어 있지만 방어적).
		 *   crctab[...]         — 다항식에 의한 16단 분할 결과.
		 *   (crc << 8) ^ ...    — CRC 를 한 바이트 위로 밀어 하위 8비트 공간을 만든 뒤
		 *                        테이블 값과 XOR 하여 누적. */
		crc = (crc << 8) ^ crctab[((crc >> 24) ^ *(cp++)) & 0xFF];

	/* [한국어] 누적된 32비트 CRC 반환 — verify_header 나 벤치마크 누적기에 저장. */
	return crc;
}
