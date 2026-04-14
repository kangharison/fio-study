/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] T10-DIF CRC-16 헤더 (crc-t10dif.h)
 *
 * === 파일의 역할 ===
 * T10 DIF(Data Integrity Field) CRC-16 함수의 인터페이스를 정의한다.
 * T10-DIF는 SCSI/SAS 스토리지에서 데이터 무결성 보호(DIF/DIX)에 사용하는
 * CRC-16으로, 다항식 0x8BB7을 사용한다.
 * 일반 CRC-16(다항식 0x8005)과는 다른 다항식이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_crc_t10dif()
 *
 * === 타 모듈과의 연결 ===
 * - crct10dif_common.c: 소프트웨어 구현 또는 ISA-L 가속 구현
 * - verify.c: verify=crc-t10dif 옵션 시 호출
 */
#ifndef __CRC_T10DIF_H
#define __CRC_T10DIF_H

extern unsigned short fio_crc_t10dif(unsigned short crc,
				     const unsigned char *buffer,
				     unsigned int len);

#endif
