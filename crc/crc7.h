/*
 * [한국어 설명] CRC-7 체크섬 헤더 (crc7.h)
 *
 * === 파일의 역할 ===
 * CRC-7 체크섬의 공개 API 와 인라인 헬퍼를 정의한다. CRC-7 은 다항식
 * x^7 + x^3 + 1 (바이트 표현 0x09, 또는 shift-left 후 0x89)을 사용하는 7비트
 * CRC 로, SD 카드/MMC 프로토콜의 CMD/응답 패킷 검증에 표준으로 쓰인다.
 * fio 에서는 verify=crc7 옵션으로 verify_header.v_crc7 필드에 저장되어
 * 데이터 무결성 검증에 사용된다.
 * 구현 전략: 256 엔트리 사전 계산 신드롬 테이블(crc7_syndrome_table) 을 이용해
 * 바이트 단위로 CRC 를 갱신(인라인 crc7_byte).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_crc7() / verify_io_u_crc7()
 *     → fio_crc7(buf, len) — 본 헤더 선언, crc7.c 에서 loop 로 구현
 *       → crc7_byte(crc, byte) 인라인 호출 반복
 *         → crc7_syndrome_table[(crc<<1) ^ byte] 룩업
 *
 * === 타 모듈과의 연결 ===
 * - crc7.c: 룩업 테이블 정의 및 fio_crc7() 루프 구현.
 * - verify.c: verify=crc7 옵션 처리.
 * - crc/test.c: --crctest=crc7 벤치마크 등록.
 *
 * === 주요 함수/구조체 요약 ===
 * - crc7_syndrome_table[256]: 컴파일 타임 상수 테이블, crc7.c 에서 정의.
 * - crc7_byte(crc, byte): 인라인 바이트 단위 CRC-7 갱신.
 * - fio_crc7(buf, len): 버퍼 전체 CRC-7 계산(초기값 0, 최종 반환 시 7비트 유지).
 */
#ifndef CRC7_H
/* [한국어] 헤더 가드 — verify.c / crc7.c / test.c 동시 포함 가능. */
#define CRC7_H

/* [한국어] CRC-7 신드롬 테이블 — 다항식 x^7+x^3+1 로부터 256 엔트리 사전 계산.
 * 설정자: crc7.c 에서 const unsigned char crc7_syndrome_table[256] = {...} 형태로 초기화.
 * 읽는 자: crc7_byte() 가 (crc<<1)^byte 인덱스로 참조.
 * 값 범위: 7비트 값(상위 비트 0). 컴파일 타임 상수 → .rodata 섹션.
 * 동기화: 읽기 전용이라 스레드 간 공유 안전. */
extern const unsigned char crc7_syndrome_table[256];

/*
 * [한국어]
 * crc7_byte - 1바이트를 CRC-7 계산에 반영 (인라인 헬퍼)
 *
 * @crc:  현재까지의 CRC-7 누적값(하위 7비트 유효, 상위 비트는 0).
 * @data: 반영할 1바이트.
 * @return: 갱신된 CRC-7 값(7비트).
 *
 * 동작: CRC 를 1비트 좌측 시프트하고 데이터 바이트와 XOR 한 결과를
 *      신드롬 테이블 인덱스로 사용해 다음 CRC 를 얻는다. 전형적인 테이블 기반
 *      non-reflected CRC 구현이다.
 *
 * 호출 체인: fio_crc7() 의 for 루프 → 각 바이트마다 본 함수 호출.
 * 실행 컨텍스트: 인라인 함수 — 호출 지점(잡 스레드 또는 벤치마크) 따름.
 */
static inline unsigned char crc7_byte(unsigned char crc, unsigned char data)
{
	/* [한국어] (crc<<1) ^ data : 다음 신드롬 인덱스 계산. 테이블은 이미 다항식
	 * 나눗셈 결과를 pre-computed 하므로 룩업 한 번에 7비트 결과 확보. */
	return crc7_syndrome_table[(crc << 1) ^ data];
}

/*
 * [한국어]
 * fio_crc7 - 버퍼 전체의 CRC-7 계산
 *
 * @buffer: CRC 계산 대상 바이트 버퍼. NULL 금지.
 * @len:    처리할 바이트 수.
 * @return: 7비트 CRC-7 값(상위 비트 0).
 *
 * 호출 체인: verify.c::fill_crc7() → fio_crc7() → 반환값이 v_crc7 필드에 저장.
 * 실행 컨텍스트: 잡 스레드(verify) 또는 벤치마크 스레드.
 * 에러 경로: 없음 — 순수 계산.
 */
extern unsigned char fio_crc7(const unsigned char *buffer, unsigned int len);

#endif
