/*
 * [한국어 설명] CRC-64 체크섬 헤더 (crc64.h)
 *
 * === 파일의 역할 ===
 * 64비트 CRC 체크섬 함수의 인터페이스를 정의한다.
 * 두 가지 CRC-64 변형을 제공한다:
 *   1) fio_crc64(): 범용 CRC-64 (다항식 0x95AC9329AC4BC9B5)
 *   2) fio_crc64_nvme(): NVMe 규격 CRC-64 (증분 계산 지원, libisal 가속 가능)
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → fio_crc64() 또는 fio_crc64_nvme()
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=crc64 옵션 시 호출
 * - crc64.c: 두 함수의 구현
 * - crc64table.h: NVMe CRC-64 룩업 테이블
 */
#ifndef CRC64_H
#define CRC64_H

/* [한국어] 범용 CRC-64 - 전체 버퍼를 한 번에 처리 */
unsigned long long fio_crc64(const unsigned char *, unsigned long);

/* [한국어] NVMe 규격 CRC-64 - 시드(crc)를 받아 증분 계산 가능, libisal 가속 지원 */
unsigned long long fio_crc64_nvme(unsigned long long crc, const void *p,
				  unsigned int len);

#endif
