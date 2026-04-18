/*
 * Cryptographic API.
 *
 * T10 Data Integrity Field CRC16 Crypto Transform
 *
 * Copyright (c) 2007 Oracle Corporation.  All rights reserved.
 * Written by Martin K. Petersen <martin.petersen@oracle.com>
 * Copyright (C) 2013 Intel Corporation
 * Author: Tim Chen <tim.c.chen@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

/*
 * [한국어 설명] T10 DIF CRC-16 구현 (crct10dif_common.c)
 *
 * === 파일의 역할 ===
 * SCSI T10 DIF(Data Integrity Field) 규약의 16비트 Guard CRC를 구현한다.
 * 사용 다항식: x^16 + x^15 + x^11 + x^9 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
 * (=0x8BB7, reflected 아님, NVMe/SCSI PI Type1/2 Guard 필드와 동일).
 * 본 파일은 두 백엔드를 제공한다:
 *   1) CONFIG_LIBISAL 정의 시: Intel ISA-L 라이브러리의 crc16_t10dif() 로
 *      PCLMULQDQ 하드웨어 가속을 위임(SSE4.2/AVX 급 CPU 에서 수 GB/s 달성).
 *   2) 미정의 시: 256엔트리 룩업 테이블 기반 소프트웨어 구현.
 * 초기값은 호출자가 지정(일반적으로 0, 또는 이어붙이기 시 직전 CRC).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 verify 에서는 NVMe/SCSI PI 필드를 흉내내는 데이터 무결성 CRC로 사용된다.
 * 특히 PI Type1/2 에서 512/4096B 섹터마다 2B Guard + 2B AppTag + 4B RefTag 형태의
 * 8B/16B PI 필드가 부착되는데, 그 중 Guard 가 바로 T10 DIF CRC-16.
 * xnvme/sg/io_uring_cmd 엔진이 PI를 활성화하면 컨트롤러가 CRC 를 자동 생성/검증하지만,
 * fio 가 소프트웨어로 비교할 때는 이 함수를 호출한다.
 * 호출 체인: verify.c / xnvme·sg 검증 경로 → [fio_crc_t10dif]
 *            → (ISA-L 있으면) crc16_t10dif() / (없으면) 테이블 루프.
 *
 * === 타 모듈과의 연결 ===
 * - crc-t10dif.h: fio_crc_t10dif() 프로토타입(초기 crc 입력을 받는 증분 API).
 * - isa-l/crc.h (CONFIG_LIBISAL 시): crc16_t10dif() 공급 — 내부에 PCLMULQDQ 가속 루프.
 * - verify.c: T10 DIF 검증 옵션이 활성화되면 호출.
 * 데이터 흐름: 호출자 crc(초기 0) → fio_crc_t10dif → 갱신 CRC 반환 → 다음 섹터에 재투입 가능.
 * 동기화: 순수 계산·테이블 read-only — 재진입 안전.
 *
 * === 주요 함수 요약 ===
 * - fio_crc_t10dif(crc, buffer, len): 증분 CRC-16 T10 DIF 계산. 반환값을 다음 호출의
 *   crc 입력으로 전달하면 여러 세그먼트를 이어서 해시할 수 있다.
 * - t10_dif_crc_table[256]: (ISA-L 미사용 경로용) 다항식 0x8BB7로 생성된 룩업 테이블.
 */

/* [한국어] 빌드 분기: ISA-L(Intel Intelligent Storage Acceleration Library) 가 있으면
 * PCLMULQDQ 가속 경로로 위임, 없으면 테이블 기반 소프트 루프. configure 단계에서
 * `--enable-libisal` 또는 자동 검출 시 -DCONFIG_LIBISAL 이 정의되고, -lisal 링크가 추가된다. */
#ifdef CONFIG_LIBISAL
#include <isa-l/crc.h>
/* [한국어] isa-l/crc.h: ISA-L 의 CRC 공공 API(crc16_t10dif, crc32_iscsi, crc64_* 등)
 * 프로토타입을 제공. crc16_t10dif 는 내부에 PCLMULQDQ reduce-by-16 루프를 포함해
 * 단일 패스로 수 GB/s 처리량을 낸다. */

/*
 * [한국어]
 * fio_crc_t10dif - T10 DIF CRC-16 계산(ISA-L 가속 위임 버전)
 *
 * @crc:    현재까지 누적된 16비트 CRC 값(새 계산은 0, 이어붙이기는 직전 반환값).
 * @buffer: 입력 데이터 포인터(바이트 스트림). 정렬 요건은 ISA-L 내부에서 처리.
 * @len:    데이터 길이(바이트).
 * @return: 갱신된 16비트 CRC.
 *
 * 본 경로는 얇은 래퍼 — 모든 실연산은 ISA-L 의 crc16_t10dif 가 담당하며,
 * 해당 함수는 CPU 기능을 런타임에 자동 감지(AVX/AVX2/AVX-512)해 최적 경로를 선택.
 *
 * 호출 체인: verify.c → [fio_crc_t10dif] → isa-l::crc16_t10dif
 */
extern unsigned short fio_crc_t10dif(unsigned short crc,
				     const unsigned char *buffer,
				     unsigned int len)
{
	/* [한국어] ISA-L 하드웨어 가속 경로로 단순 위임. 내부는 PCLMULQDQ reduce-by-N 루프. */
	return crc16_t10dif(crc, buffer, len);
}

/* [한국어] CONFIG_LIBISAL 미정의 시: 테이블 기반 소프트웨어 구현을 사용한다. */
#else
#include "crc-t10dif.h"
/* [한국어] crc-t10dif.h: fio_crc_t10dif() 프로토타입 제공. 테이블은 본 파일 내부 static.
 * verify.c 등 호출자는 이 헤더만 포함하면 되며 구현 분기는 build-time 에 숨겨짐. */

/* Table generated using the following polynomium:
 * x^16 + x^15 + x^11 + x^9 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
 * gt: 0x8bb7
 */
/* [한국어] T10 DIF CRC-16 룩업 테이블(다항식 0x8BB7).
 * 설정자: 컴파일타임 하드코딩 상수.
 * 읽는 자: 하단 fio_crc_t10dif() 루프만.
 * 값 범위: 엔트리 각 16비트, 256개(=512B). 본 파일 내부 static 으로 외부 노출 없음.
 * 동기화: .rodata 상주 — 락 불필요. */
static const unsigned short t10_dif_crc_table[256] = {
	0x0000, 0x8BB7, 0x9CD9, 0x176E, 0xB205, 0x39B2, 0x2EDC, 0xA56B,
	0xEFBD, 0x640A, 0x7364, 0xF8D3, 0x5DB8, 0xD60F, 0xC161, 0x4AD6,
	0x54CD, 0xDF7A, 0xC814, 0x43A3, 0xE6C8, 0x6D7F, 0x7A11, 0xF1A6,
	0xBB70, 0x30C7, 0x27A9, 0xAC1E, 0x0975, 0x82C2, 0x95AC, 0x1E1B,
	0xA99A, 0x222D, 0x3543, 0xBEF4, 0x1B9F, 0x9028, 0x8746, 0x0CF1,
	0x4627, 0xCD90, 0xDAFE, 0x5149, 0xF422, 0x7F95, 0x68FB, 0xE34C,
	0xFD57, 0x76E0, 0x618E, 0xEA39, 0x4F52, 0xC4E5, 0xD38B, 0x583C,
	0x12EA, 0x995D, 0x8E33, 0x0584, 0xA0EF, 0x2B58, 0x3C36, 0xB781,
	0xD883, 0x5334, 0x445A, 0xCFED, 0x6A86, 0xE131, 0xF65F, 0x7DE8,
	0x373E, 0xBC89, 0xABE7, 0x2050, 0x853B, 0x0E8C, 0x19E2, 0x9255,
	0x8C4E, 0x07F9, 0x1097, 0x9B20, 0x3E4B, 0xB5FC, 0xA292, 0x2925,
	0x63F3, 0xE844, 0xFF2A, 0x749D, 0xD1F6, 0x5A41, 0x4D2F, 0xC698,
	0x7119, 0xFAAE, 0xEDC0, 0x6677, 0xC31C, 0x48AB, 0x5FC5, 0xD472,
	0x9EA4, 0x1513, 0x027D, 0x89CA, 0x2CA1, 0xA716, 0xB078, 0x3BCF,
	0x25D4, 0xAE63, 0xB90D, 0x32BA, 0x97D1, 0x1C66, 0x0B08, 0x80BF,
	0xCA69, 0x41DE, 0x56B0, 0xDD07, 0x786C, 0xF3DB, 0xE4B5, 0x6F02,
	0x3AB1, 0xB106, 0xA668, 0x2DDF, 0x88B4, 0x0303, 0x146D, 0x9FDA,
	0xD50C, 0x5EBB, 0x49D5, 0xC262, 0x6709, 0xECBE, 0xFBD0, 0x7067,
	0x6E7C, 0xE5CB, 0xF2A5, 0x7912, 0xDC79, 0x57CE, 0x40A0, 0xCB17,
	0x81C1, 0x0A76, 0x1D18, 0x96AF, 0x33C4, 0xB873, 0xAF1D, 0x24AA,
	0x932B, 0x189C, 0x0FF2, 0x8445, 0x212E, 0xAA99, 0xBDF7, 0x3640,
	0x7C96, 0xF721, 0xE04F, 0x6BF8, 0xCE93, 0x4524, 0x524A, 0xD9FD,
	0xC7E6, 0x4C51, 0x5B3F, 0xD088, 0x75E3, 0xFE54, 0xE93A, 0x628D,
	0x285B, 0xA3EC, 0xB482, 0x3F35, 0x9A5E, 0x11E9, 0x0687, 0x8D30,
	0xE232, 0x6985, 0x7EEB, 0xF55C, 0x5037, 0xDB80, 0xCCEE, 0x4759,
	0x0D8F, 0x8638, 0x9156, 0x1AE1, 0xBF8A, 0x343D, 0x2353, 0xA8E4,
	0xB6FF, 0x3D48, 0x2A26, 0xA191, 0x04FA, 0x8F4D, 0x9823, 0x1394,
	0x5942, 0xD2F5, 0xC59B, 0x4E2C, 0xEB47, 0x60F0, 0x779E, 0xFC29,
	0x4BA8, 0xC01F, 0xD771, 0x5CC6, 0xF9AD, 0x721A, 0x6574, 0xEEC3,
	0xA415, 0x2FA2, 0x38CC, 0xB37B, 0x1610, 0x9DA7, 0x8AC9, 0x017E,
	0x1F65, 0x94D2, 0x83BC, 0x080B, 0xAD60, 0x26D7, 0x31B9, 0xBA0E,
	0xF0D8, 0x7B6F, 0x6C01, 0xE7B6, 0x42DD, 0xC96A, 0xDE04, 0x55B3
};

/*
 * [한국어]
 * fio_crc_t10dif - T10 DIF CRC-16 계산(소프트웨어 테이블 경로)
 *
 * @crc:    입력 CRC(새 계산은 0, 증분이면 직전 반환값).
 * @buffer: 바이트 데이터 포인터.
 * @len:    바이트 길이.
 * @return: 갱신된 16비트 CRC.
 *
 * 알고리즘: 표준 CRC-16 반사 없는 폼(big-endian).
 *   crc = (crc << 8) ^ t10_dif_crc_table[((crc >> 8) ^ buffer[i]) & 0xff]
 * 매 반복마다 CRC 상위 8비트와 입력 바이트 XOR 해 테이블 인덱스 생성, CRC를 8비트
 * 좌시프트 후 테이블 결과와 XOR. 이렇게 하면 한 바이트 분의 다항식 분할을
 * 한 번에 해결한다.
 *
 * 호출 체인: verify.c / xnvme·sg PI 검증 → [fio_crc_t10dif] → t10_dif_crc_table[]
 */
extern unsigned short fio_crc_t10dif(unsigned short crc,
				     const unsigned char *buffer,
				     unsigned int len)
{
	/* [한국어] 반복 인덱스 — 루프 카운트 시작. unsigned int 로 선언되어 len 과 타입 일치. */
	unsigned int i;

	/* [한국어] 바이트 단위로 len 회 순회 — reflected 가 아니므로 "상위 8비트" 방식. */
	for (i = 0 ; i < len ; i++)
		/* [한국어] (crc >> 8) 로 상위 바이트를 하위로 가져오고 입력 바이트와 XOR 해
		 * 0~255 인덱스를 만들어 테이블에서 16비트 잔여(synchroeffect)를 얻는다.
		 * crc << 8 은 현 CRC 를 한 바이트 위로 밀어 새 입력 공간을 확보. 마지막 XOR 로
		 * 8비트 분 다항식 분할 완료. */
		crc = (crc << 8) ^ t10_dif_crc_table[((crc >> 8) ^ buffer[i]) & 0xff];

	/* [한국어] 누적된 CRC 반환. 호출자는 PI Guard 필드(2B)에 기록하거나 비교. */
	return crc;
}

#endif
/* [한국어] CONFIG_LIBISAL 분기 종료. 두 경로 중 하나만 컴파일되어 링크에 참여. */
