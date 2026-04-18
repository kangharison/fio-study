/*
 * [한국어 설명] CRC-64 체크섬 헤더 (crc64.h)
 *
 * === 파일의 역할 ===
 * 두 가지 64비트 CRC 체크섬의 공개 API 를 정의한다.
 *   1) fio_crc64(): 범용 Jones 다항식 CRC-64(일반 verify 용)
 *      - 다항식 0x95AC9329AC4BC9B5 (Jones polynomial, reflected form).
 *   2) fio_crc64_nvme(): NVMe 1.4+ 스펙 Rocksoft 다항식 CRC-64(64-bit PI Guard 용)
 *      - 다항식(reflected) 0x9A6C9329AC4BC9B5.
 *      - NVMe 규약에 따라 초기값 0xFFFFFFFFFFFFFFFF, 출력 XOR 0xFFFF... 반전.
 *      - CONFIG_LIBISAL64 정의 시 ISA-L 의 crc64_rocksoft_refl PCLMULQDQ 가속 사용.
 *      - 미정의 시 crc64table.h 의 crc64nvmetable[256] 소프트 폴백.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_crc64() / verify_io_u_crc64()
 *     → fio_crc64(buf, len)         // 범용, verify_header.v_crc64 저장
 *   engines/nvme.c / io_uring.c (URING_CMD NVMe passthru)
 *     → fio_crc64_nvme(crc, buf, len)  // 64B PI Guard 필드 계산
 *
 * === 타 모듈과의 연결 ===
 * - crc64.c: 두 함수의 실제 구현. CONFIG_LIBISAL64 유무에 따라 가속/폴백 분기.
 * - crc64table.h: NVMe Rocksoft CRC-64 소프트 폴백용 256 엔트리 룩업 테이블.
 * - verify.c: verify=crc64 옵션의 fill/verify 경로.
 * - engines/nvme.c: 64비트 PI Guard(확장 LBA + 64B PI) 생성 및 검증.
 * - engines/io_uring.c (URING_CMD): NVMe passthru PI 경로.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_crc64(buf, len): 한 번의 호출로 버퍼 전체 CRC 계산. 내부에서 0 초기값.
 * - fio_crc64_nvme(crc, buf, len): 증분/스트리밍 가능, NVMe 스펙 규약 준수
 *   (초기값은 호출자가 0 또는 ~0 을 전달, 최종 반환에는 ~ 반전이 포함되지 않을 수
 *   있어 상위 레이어가 NVMe 명령 생성 시 추가 반전을 수행한다 — 구현 주석 참조).
 */
#ifndef CRC64_H
/* [한국어] 헤더 가드 — verify.c / engines/nvme.c / test.c 동시 포함 가능. */
#define CRC64_H

/*
 * [한국어]
 * fio_crc64 - 범용 CRC-64 (Jones 다항식) 한 번에 계산
 *
 * @buf: CRC 계산 대상 바이트 버퍼. NULL 금지.
 * @len: 버퍼 길이(바이트). unsigned long 은 플랫폼별 32/64비트이나 verify
 *       블록 크기는 수 MiB 이하이므로 충분.
 * @return: 64비트 CRC 체크섬. verify_header.v_crc64 에 저장되어 재검증 시 비교.
 *
 * 호출 체인:
 *   verify.c::fill_crc64() → fio_crc64() → v_crc64 저장
 *   verify.c::verify_io_u_crc64() → 재계산 후 저장값과 비교
 *
 * 실행 컨텍스트: 잡 스레드(verify). 실패 시 없음(순수 계산).
 */
unsigned long long fio_crc64(const unsigned char *, unsigned long);

/*
 * [한국어]
 * fio_crc64_nvme - NVMe Rocksoft CRC-64 증분 계산(PI Guard 64B 용)
 *
 * @crc: 이전까지의 CRC 누적값. 처음 호출 시 0(또는 NVMe 규약에 따라 0xFFFF...) 전달.
 * @p:   계산 대상 버퍼(void* 바이트 스트림).
 * @len: 처리 바이트 수.
 * @return: 갱신된 CRC-64 값. NVMe 64B PI Guard 필드에 그대로 기록할 수 있는 형식.
 *
 * CONFIG_LIBISAL64 정의 시 ISA-L 의 crc64_rocksoft_refl() PCLMULQDQ 가속 호출,
 * 미정의 시 crc64table.h 의 256 엔트리 테이블 룩업으로 계산한다.
 *
 * 호출 체인:
 *   engines/nvme.c::fio_nvme_generate_guard() → fio_crc64_nvme() → PI Guard 채움
 *   engines/nvme.c::fio_nvme_pi_verify() → 재계산 후 Guard 와 비교
 *
 * 실행 컨텍스트: 잡 스레드(prep) 또는 완료 수집(verify). 에러 경로: 없음.
 */
unsigned long long fio_crc64_nvme(unsigned long long crc, const void *p,
				  unsigned int len);

#endif
