/*
 * [한국어 설명] CRC-7 체크섬 헤더 (crc7.h)
 *
 * === 파일의 역할 ===
 * CRC-7 체크섬 함수의 인터페이스를 정의한다.
 * CRC-7은 다항식 x^7 + x^3 + 1을 사용하는 7비트 CRC로,
 * SD 카드 프로토콜의 명령 패킷 검증 등에 사용된다.
 * fio에서는 verify=crc7 옵션으로 데이터 무결성 검증에 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_crc7() → crc7_byte() → crc7_syndrome_table[]
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=crc7 옵션 시 호출
 * - crc7.c: 룩업 테이블과 fio_crc7() 구현
 * - crc/test.c: 벤치마크 테스트
 */
#ifndef CRC7_H
#define CRC7_H

/* [한국어] CRC-7 신드롬 테이블 - 다항식 x^7 + x^3 + 1로부터 생성 */
extern const unsigned char crc7_syndrome_table[256];

/*
 * [한국어]
 * crc7_byte - 1바이트를 CRC-7 계산에 반영 (인라인)
 *
 * @crc: 현재 CRC 값 (7비트)
 * @data: 반영할 1바이트 데이터
 * @return: 갱신된 CRC 값
 *
 * CRC를 1비트 좌측 시프트하고 데이터와 XOR한 값을 테이블 인덱스로 사용한다.
 */
static inline unsigned char crc7_byte(unsigned char crc, unsigned char data)
{
	return crc7_syndrome_table[(crc << 1) ^ data];
}

extern unsigned char fio_crc7(const unsigned char *buffer, unsigned int len);

#endif
