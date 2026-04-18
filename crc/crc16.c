/*
 *      crc16.c
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2. See the file COPYING for more details.
 */

/*
 * [한국어 설명] CRC-16 (CRC-16/ARC 계열, 다항식 0x8005) 체크섬 구현 (crc16.c)
 *
 * === 파일의 역할 ===
 * 표준 CRC-16 체크섬을 256엔트리 룩업 테이블(`crc16_table[]`)과 바이트 단위
 * 계산 루프 `fio_crc16()`으로 제공한다. 사용 다항식은 x^16 + x^15 + x^2 + 1
 * (=0x8005, reflected 형태 0xA001)이며 초기값 0, 최종 XOR 없음의 CRC-16/ARC 변종에
 * 해당한다. 테이블은 "한 바이트 입력마다 다항식 분할을 8단계 미리 수행한 결과"이므로
 * 런타임에는 XOR 1회 + 테이블 조회 1회로 8비트 분의 CRC 갱신이 완료된다.
 * 본 모듈은 순수 소프트웨어 구현으로 하드웨어 가속 폴백이 없다(짧은 16비트 CRC이고
 * fio 벤치마크 상 CPU 병목이 드물다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 경로의 VERIFY_CRC16 공급자.
 * 쓰기 시 fill_crc16(verify.c) → fio_crc16() [이 파일]이 verify_header.v_crc16
 * 16비트 필드를 채우고, 읽기 시 verify_io_u_crc16()이 동일 경로로 재계산 후
 * 비교한다. CRC-16은 오류 검출 능력은 낮지만 헤더 오버헤드가 2B에 불과해
 * 매우 작은 블록 검증(예: 섹터 헤더 필드) 용도로 유용하다.
 * 호출 체인:
 *   verify.c::fill_crc16/verify_io_u_crc16 → [fio_crc16] → crc16_byte() [crc16.h 인라인] → crc16_table[]
 *   crc/test.c::t_crc16 → [fio_crc16]
 *
 * === 타 모듈과의 연결 ===
 * - crc16.h: `fio_crc16()` 프로토타입과 `crc16_byte(crc, byte)` 인라인 래퍼
 *   (=crc16_table[(crc ^ byte) & 0xFF] ^ (crc >> 8)) 정의. `crc16_table[]`을 extern 선언.
 * - verify.c: verify=crc16 옵션 시 fill_crc16/verify_io_u_crc16 에서 호출.
 * - crc/test.c: --crctest 벤치마크 러너의 t_crc16 래퍼가 NR_CHUNKS회 호출.
 * 데이터 흐름: 쓰기버퍼 → fio_crc16(초기값 0) → verify_header.v_crc16 (2B). 읽기 시 재계산 비교.
 * 동기화: 순수 계산, 전역 변경 없음. `crc16_table[]`은 const 읽기 전용 데이터 섹션.
 *
 * === 주요 함수 요약 ===
 * - crc16_table[256]: 다항식 0x8005로부터 생성된 reflected-input 룩업 테이블(상수 데이터).
 * - fio_crc16(buffer, len): 초기값 0에서 시작해 입력 바이트마다 crc16_byte() 로 8비트
 *   분할을 수행하여 최종 16비트 CRC 반환. 내부 루프만 있고 분기/하드웨어 경로 없음.
 */
#include "crc16.h"
/* [한국어] crc16.h: fio_crc16() 프로토타입과 crc16_byte() 인라인 함수,
 * crc16_table[] extern 선언을 공급. verify.c 와 test.c 가 같은 헤더를 참조해 ABI 공유. */

/** CRC table for the CRC-16. The poly is 0x8005 (x^16 + x^15 + x^2 + 1) */
/* [한국어] CRC-16 (poly 0x8005, reflected 입력) 룩업 테이블.
 * 설정자: 컴파일 시 미리 계산된 상수(본 소스에 하드코딩). 런타임 변경 없음(const).
 * 읽는 자: crc16_byte()(crc16.h 인라인), 본 파일 fio_crc16() 루프.
 * 값 범위: 각 항목은 0x0000~0xFFFF 16비트, 총 256엔트리(=512B).
 * 동기화: read-only .rodata 섹션에 배치되어 모든 스레드가 락 없이 공유 가능. */
unsigned short const crc16_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

/*
 * [한국어]
 * fio_crc16 - 버퍼 전체의 CRC-16 체크섬(초기값 0) 계산
 *
 * @buffer: CRC를 계산할 원시 데이터 버퍼(바이트 스트림으로 해석).
 * @len:    데이터 길이(바이트). 0 이면 0 반환(초기 CRC 그대로).
 * @return: 16비트 CRC 값. 상위 패딩 없음(0x0000~0xFFFF).
 *
 * 알고리즘: CRC16/ARC 변종 — 초기값 0, reflected 입출력, 최종 XOR 없음.
 * 각 바이트 b 에 대해 crc16_byte(crc, b) 를 적용(매크로 정의:
 *   crc = (crc >> 8) ^ crc16_table[(crc ^ b) & 0xFF])하여 8비트씩 다항식 분할을 수행.
 *
 * 실행 컨텍스트: 잡 스레드 또는 verify 스레드 — 재진입 안전(순수 계산).
 * 상태 공유 없음. crc16_table[] 은 읽기 전용이라 락 불필요.
 *
 * 호출 체인:
 *   verify.c::fill_crc16/verify_io_u_crc16 → [fio_crc16] → crc16_byte(인라인) → crc16_table[]
 *   crc/test.c::t_crc16 → [fio_crc16]
 */
unsigned short fio_crc16(const void *buffer, unsigned int len)
{
	/* [한국어] 바이트 단위 접근을 위한 재해석 캐스트. void* 는 포인터 산술 불가이므로
	 * unsigned char* 로 변환해 ++ 와 역참조가 가능하게 만든다. */
	const unsigned char *cp = (const unsigned char *) buffer;
	/* [한국어] CRC-16/ARC 의 초기값 0. (다른 변종은 0xFFFF 초기값을 쓰기도 함 — 본 함수는 0.) */
	unsigned short crc = 0;

	/* [한국어] len 만큼 반복 — 후위 감소(len--)로 "현재 len 값이 0이 아닐 때만 진입"
	 * 패턴이 되어, 0부터의 길이 검증과 종료가 한 표현으로 처리된다. */
	while (len--)
		/* [한국어] crc16_byte(crc, byte) = crc16_table[(crc ^ byte) & 0xFF] ^ (crc >> 8).
		 * 하위 8비트와 입력을 XOR 해 테이블 인덱스를 만들고, 원 CRC 를 8비트 우시프트한
		 * 값과 테이블 결과를 다시 XOR 하여 8비트 분의 다항식 분할을 한 번에 끝낸다.
		 * *cp++ 로 다음 바이트 진행. */
		crc = crc16_byte(crc, *cp++);
	/* [한국어] 최종 16비트 CRC 반환. verify.c 는 이 값을 verify_header.v_crc16 에 저장. */
	return crc;
}
