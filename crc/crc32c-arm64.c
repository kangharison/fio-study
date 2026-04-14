/*
 * [한국어 설명] CRC-32C ARM64 하드웨어 가속 구현 (crc32c-arm64.c)
 *
 * === 파일의 역할 ===
 * ARM64(AArch64) CPU의 CRC32 확장 명령어(__crc32cd 등)와
 * PMULL 다항식 곱셈 명령어(vmull_p64)를 사용하여 CRC-32C를 하드웨어로
 * 고속 계산한다. 1024바이트 블록을 3-way 병렬 파이프라인으로 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 시작 시 crc32c_arm64_probe()가 CPU 기능을 확인하여
 * ARM64 CRC 지원 여부를 감지한다. 지원 시 fio_crc32c()에서 자동 선택된다.
 * 호출 체인: fio_crc32c() [crc32c.h] → crc32c_arm64() [이 파일]
 *
 * === 타 모듈과의 연결 ===
 * - crc32c.h: fio_crc32c()에서 crc32c_arm64_available 플래그 확인
 * - crc32c.c: 소프트웨어 폴백 (ARM CRC 미지원 시)
 * - os/os.h: os_cpu_has() 함수로 CPU 기능 감지
 *
 * === 주요 함수 요약 ===
 * - crc32c_arm64(): 3-way 파이프라인 CRC-32C 계산 (1024바이트 블록 단위)
 * - crc32c_arm64_probe(): ARM64 CRC 확장 지원 여부 감지
 */
#include "crc32c.h"
#include "../os/os.h"

/* [한국어] ARM64 CRC 하드웨어 가속 사용 가능 여부 - probe()에서 설정 */
bool crc32c_arm64_available = false;

#ifdef ARCH_HAVE_CRC_CRYPTO

/*
 * [한국어] 3-way 병렬 CRC 매크로
 * CRC32C3X8: 3개의 독립적인 CRC 스트림(crc0, crc1, crc2)을 동시에 계산한다.
 *   각 스트림은 1024바이트 블록을 42개의 uint64_t(336바이트)씩 3등분하여 처리한다.
 * CRC32C7X3X8: CRC32C3X8을 7회 반복하여 42개 워드를 처리한다.
 * 이 3-way 분할은 CPU 파이프라인의 명령어 수준 병렬성(ILP)을 활용하여
 * 단일 스트림 대비 약 3배의 처리량을 달성한다.
 */
#define CRC32C3X8(ITR) \
	crc1 = __crc32cd(crc1, *((const uint64_t *)data + 42*1 + (ITR)));\
	crc2 = __crc32cd(crc2, *((const uint64_t *)data + 42*2 + (ITR)));\
	crc0 = __crc32cd(crc0, *((const uint64_t *)data + 42*0 + (ITR)));

#define CRC32C7X3X8(ITR) do {\
	CRC32C3X8((ITR)*7+0) \
	CRC32C3X8((ITR)*7+1) \
	CRC32C3X8((ITR)*7+2) \
	CRC32C3X8((ITR)*7+3) \
	CRC32C3X8((ITR)*7+4) \
	CRC32C3X8((ITR)*7+5) \
	CRC32C3X8((ITR)*7+6) \
	} while(0)

#include <arm_acle.h>
#include <arm_neon.h>

static bool crc32c_probed;

/*
 * Function to calculate reflected crc with PMULL Instruction
 * crc done "by 3" for fixed input block size of 1024 bytes
 */
uint32_t crc32c_arm64(unsigned char const *data, unsigned long length)
{
	signed long len = length;
	uint32_t crc = ~0;
	uint32_t crc0, crc1, crc2;

	/* Load two consts: K1 and K2 */
	const poly64_t k1 = 0xe417f38a, k2 = 0x8f158014;
	uint64_t t0, t1;

	while ((len -= 1024) >= 0) {
		/* Do first 8 bytes here for better pipelining */
		crc0 = __crc32cd(crc, *(const uint64_t *)data);
		crc1 = 0;
		crc2 = 0;
		data += sizeof(uint64_t);

		/* Process block inline
		   Process crc0 last to avoid dependency with above */
		CRC32C7X3X8(0);
		CRC32C7X3X8(1);
		CRC32C7X3X8(2);
		CRC32C7X3X8(3);
		CRC32C7X3X8(4);
		CRC32C7X3X8(5);

		data += 42*3*sizeof(uint64_t);

		/* Merge crc0 and crc1 into crc2
		   crc1 multiply by K2
		   crc0 multiply by K1 */

		t1 = (uint64_t)vmull_p64(crc1, k2);
		t0 = (uint64_t)vmull_p64(crc0, k1);
		crc = __crc32cd(crc2, *(const uint64_t *)data);
		crc1 = __crc32cd(0, t1);
		crc ^= crc1;
		crc0 = __crc32cd(0, t0);
		crc ^= crc0;

		data += sizeof(uint64_t);
	}

	if (!(len += 1024))
		return crc;

	while ((len -= sizeof(uint64_t)) >= 0) {
                crc = __crc32cd(crc, *(const uint64_t *)data);
                data += sizeof(uint64_t);
        }

        /* The following is more efficient than the straight loop */
        if (len & sizeof(uint32_t)) {
                crc = __crc32cw(crc, *(const uint32_t *)data);
                data += sizeof(uint32_t);
        }
        if (len & sizeof(uint16_t)) {
                crc = __crc32ch(crc, *(const uint16_t *)data);
                data += sizeof(uint16_t);
        }
        if (len & sizeof(uint8_t)) {
                crc = __crc32cb(crc, *(const uint8_t *)data);
        }

	return crc;
}

/*
 * [한국어]
 * crc32c_arm64_probe - ARM64 CPU가 CRC32 확장 명령어를 지원하는지 감지
 *
 * os_cpu_has(CPU_ARM64_CRC32C)를 호출하여 CRC32 명령어 지원 여부를 확인한다.
 * 한 번만 감지하며, 결과를 crc32c_arm64_available 전역 변수에 저장한다.
 *
 * 호출 체인:
 *   초기화 코드 / fio_crctest() → [crc32c_arm64_probe] → os_cpu_has()
 */
void crc32c_arm64_probe(void)
{
	if (!crc32c_probed) {
		crc32c_arm64_available = os_cpu_has(CPU_ARM64_CRC32C);
		crc32c_probed = true;
	}
}

#endif /* ARCH_HAVE_CRC_CRYPTO */
