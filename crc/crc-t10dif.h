/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] T10 DIF(Data Integrity Field) CRC-16 헤더 (crc-t10dif.h)
 *
 * === 파일의 역할 ===
 * SCSI/SAS 스토리지의 데이터 무결성 보호(DIF/DIX — T10 SBC-3 Protection
 * Information Type 1/2/3) 에서 사용하는 CRC-16 의 공개 API 를 정의한다.
 * 이 CRC 는 일반 CRC-16(ARC, 다항식 0x8005) 과 달리 **다항식 0x8BB7**
 * (x^16 + x^15 + x^11 + x^9 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1) 을 사용하며,
 * NVMe End-to-End Data Protection 의 16바이트 PI Guard 필드 계산에도 그대로 쓰인다.
 * fio 에서는 verify=crc-t10dif 옵션과 NVMe/io_uring_cmd/xnvme 엔진의 PI 생성/검증
 * 경로에서 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_crct10dif() / verify_io_u_crct10dif()
 *     → fio_crc_t10dif(crc, buf, len) — 본 헤더 선언
 *     → crct10dif_common.c 내부 구현(ISA-L PCLMULQDQ 가속 또는 소프트 폴백)
 *   engines/nvme.c / io_uring.c 의 PI Guard 생성 경로에서도 호출.
 *
 * === 타 모듈과의 연결 ===
 * - crct10dif_common.c: 실제 구현 — CONFIG_LIBISAL 정의 시 ISA-L 의
 *   crc16_t10dif(SSE/AVX512 PCLMULQDQ) 로 위임, 없으면 256 엔트리 테이블 룩업.
 * - verify.c: verify=crc-t10dif 옵션 처리.
 * - engines/nvme.c: PI Type 1/2 Guard (16B PI) 생성 및 검증 시 사용.
 * - engines/io_uring.c (URING_CMD): 동일하게 PI 생성 경로.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_crc_t10dif(crc, buf, len): 증분형 CRC-16/T10-DIF. 초기값 0xFFFF 또는
 *   이전 호출 반환값을 crc 로 전달하여 스트리밍 해시도 가능.
 */
#ifndef __CRC_T10DIF_H
/* [한국어] 헤더 가드 — verify.c / nvme.c / io_uring.c / test.c 동시 포함 가능. */
#define __CRC_T10DIF_H

/*
 * [한국어]
 * fio_crc_t10dif - T10-DIF CRC-16 증분 계산
 *
 * @crc:    현재까지의 CRC 누적값(16비트). 최초 호출 시 0xFFFF 또는 0 으로 시작.
 *          이전 호출의 반환값을 그대로 전달하면 스트리밍 방식 체크섬이 된다.
 * @buffer: CRC 계산 대상 바이트 버퍼. NULL 금지.
 * @len:    처리할 바이트 수.
 * @return: 갱신된 CRC-16 값(16비트). 다항식 0x8BB7 기반.
 *
 * 호출 체인:
 *   verify.c::fill_crct10dif() → fio_crc_t10dif(0, buf, len) → v_crct10dif 저장
 *   verify.c::verify_io_u_crct10dif() → 동일 호출 후 기대치와 비교
 *   nvme PI 경로: 논리 블록 데이터 + 헤더의 Guard 필드 계산.
 *
 * 실행 컨텍스트: 잡 스레드(verify) 또는 엔진 prep/completion(NVMe PI).
 * 에러 경로: 없음 — 순수 함수. 하드웨어 가속 실패 시 소프트 폴백으로 자동 전환.
 */
extern unsigned short fio_crc_t10dif(unsigned short crc,
				     const unsigned char *buffer,
				     unsigned int len);

#endif
