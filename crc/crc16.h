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
 * 표준 CRC-16(ARC 변형) 의 공개 API 와 인라인 바이트 갱신 헬퍼를 정의한다.
 * 다항식 0x8005 (x^16 + x^15 + x^2 + 1), 초기값 0, reflected 입력/출력 방식으로
 * USB, Modbus, XMODEM 등의 프로토콜과 fio 서버 네트워크 프로토콜(server.c —
 * fio_net_cmd_crc) 의 명령 무결성 검증에 사용된다.
 * 구현 전략: 256 엔트리 사전 계산 테이블(crc16_table) 의 reflected 룩업.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   1) verify.c::fill_crc16() / verify_io_u_crc16()
 *        → fio_crc16(buf, len) → crc16_byte() 루프 → crc16_table[] 룩업
 *   2) server.c::fio_net_cmd_crc() / verify_convert_cmd()
 *        → fio_crc16() 로 fio_net_cmd 본체 + PDU 의 무결성 검증용 CRC 계산
 *
 * === 타 모듈과의 연결 ===
 * - crc16.c: 룩업 테이블(crc16_table) 정의 및 fio_crc16() 루프 구현.
 * - verify.c: verify=crc16 옵션 처리.
 * - server.c: fio 네트워크 프로토콜의 명령 헤더 CRC 필드 검증.
 * - crc/test.c: --crctest=crc16 벤치마크 등록.
 *
 * === 주요 함수/구조체 요약 ===
 * - crc16_table[256]: reflected CRC-16 룩업 테이블(crc16.c 에서 정의, .rodata).
 * - crc16_byte(): 인라인 1바이트 CRC-16 갱신.
 * - fio_crc16(buf, len): 버퍼 전체 CRC-16 계산(초기값 0).
 */
#ifndef __CRC16_H
/* [한국어] 헤더 가드 — verify.c / server.c / crc16.c / test.c 동시 포함 대비. */
#define __CRC16_H

/* [한국어] CRC-16 리플렉트 룩업 테이블 — 다항식 0x8005 로부터 256 엔트리 사전 계산.
 * 설정자: crc16.c 에서 const unsigned short crc16_table[256] = {...} 로 초기화.
 * 읽는 자: crc16_byte() 가 (crc ^ data) & 0xFF 인덱스로 참조.
 * 값 범위: 16비트 값. 읽기 전용(.rodata 섹션).
 * 동기화: 컴파일 타임 상수라 스레드 간 공유 안전. */
extern unsigned short const crc16_table[256];

/*
 * [한국어]
 * fio_crc16 - 버퍼 전체의 CRC-16 계산
 * @buffer: CRC 계산 대상 버퍼. NULL 금지.
 * @len:    바이트 수.
 * @return: 16비트 CRC-16 값(다항식 0x8005, 초기값 0, 반사 방식).
 * 호출 체인: verify.c::fill_crc16() → fio_crc16() → v_crc16 저장
 *           server.c::fio_net_cmd_crc() → 명령 무결성 필드 계산
 */
extern unsigned short fio_crc16(const void *buffer, unsigned int len);

/*
 * [한국어]
 * crc16_byte - 1바이트를 CRC-16 계산에 반영 (인라인)
 *
 * @crc:  현재까지의 CRC-16 누적값.
 * @data: 반영할 1바이트.
 * @return: 갱신된 CRC-16 값.
 *
 * 동작(reflected): CRC 를 우측 8비트 시프트하고, (CRC ^ data) 의 하위 8비트를
 *                테이블 인덱스로 사용해 XOR. 일반적 reflected CRC 구현 관용구.
 *
 * 호출 체인: fio_crc16() for 루프에서 바이트마다 호출.
 * 실행 컨텍스트: 인라인 — 호출 지점 따름(잡 스레드 / 서버 스레드 / 벤치마크).
 */
static inline unsigned short crc16_byte(unsigned short crc,
					const unsigned char data)
{
	/* [한국어] crc >> 8 : 상위 8비트를 유지 후 하위로 이동.
	 * (crc ^ data) & 0xff : 테이블 인덱스 추출. 두 결과를 XOR 하여 다음 CRC 얻음. */
	return (crc >> 8) ^ crc16_table[(crc ^ data) & 0xff];
}

#endif /* __CRC16_H */
