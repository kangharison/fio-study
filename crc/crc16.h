/*
 *	crc16.h - CRC-16 routine
 *
 * Implements the standard CRC-16:
 *   Width 16
 *   Poly  0x8005 (x^16 + x^15 + x^2 + 1)
 *   Init  0
 *
 * Copyright (c) 2005 Ben Gardner <bgardner@wabtec.com>
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2. See the file COPYING for more details.
 */

/*
 * [한국어 설명] CRC-16 체크섬 헤더 (crc16.h)
 *
 * === 파일의 역할 ===
 * 표준 CRC-16 체크섬 함수의 인터페이스를 정의한다.
 * 다항식 0x8005 (x^16 + x^15 + x^2 + 1), 초기값 0을 사용한다.
 * USB, Modbus 등의 프로토콜에서 사용되는 표준 CRC-16이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_crc16() → crc16_byte() → crc16_table[]
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=crc16 옵션 시 호출
 * - crc16.c: 테이블과 fio_crc16() 구현
 *
 * === 주요 함수 요약 ===
 * - fio_crc16(): 버퍼 전체의 CRC-16 계산
 * - crc16_byte(): 1바이트 단위 CRC-16 갱신 (인라인)
 */
#ifndef __CRC16_H
#define __CRC16_H

extern unsigned short const crc16_table[256];

extern unsigned short fio_crc16(const void *buffer, unsigned int len);

/*
 * [한국어]
 * crc16_byte - 1바이트를 CRC-16 계산에 반영 (인라인)
 *
 * @crc: 현재 CRC-16 누적값
 * @data: 반영할 1바이트 데이터
 * @return: 갱신된 CRC-16 값
 *
 * reflected 방식: CRC를 8비트 우측 시프트하고, (CRC ^ data)의 하위 8비트를
 * 테이블 인덱스로 사용하여 XOR 연산한다.
 */
static inline unsigned short crc16_byte(unsigned short crc,
					const unsigned char data)
{
	return (crc >> 8) ^ crc16_table[(crc ^ data) & 0xff];
}

#endif /* __CRC16_H */
