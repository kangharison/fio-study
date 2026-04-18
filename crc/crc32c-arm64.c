/*
 * [한국어 설명] ARM64 하드웨어 CRC-32C 백엔드 (crc32c-arm64.c)
 *
 * === 파일의 역할 ===
 * ARMv8-A CRC 확장(ARM Cryptography Extensions 중 CRC32 하위 집합 — FEAT_CRC32,
 * HWCAP_CRC32)의 `__crc32cb/__crc32ch/__crc32cw/__crc32cd` 인트린직과,
 * PMULL(ARMv8 Crypto 확장의 Polynomial Multiply) 의 `vmull_p64` 를 결합해
 * Castagnoli 다항식(0x1EDC6F41, reflected 0x82F63B78) 의 CRC-32C 를 1024바이트 블록
 * 단위로 "3-way interleaved" 병렬 파이프라인으로 계산한다. 한 블록당:
 *   - 8바이트 초기 로드 → crc0 시드 설정
 *   - 336바이트씩 crc0/crc1/crc2 세 스트림 독립 계산(42회 uint64_t × 3 × 그룹 반복)
 *   - 블록 끝에서 crc0·crc1 을 PMULL 로 상수 K1/K2 와 다항식 곱하여 crc2 와 결합
 * 이 구조는 CPU 파이프라인의 ILP(Instruction-Level Parallelism)를 극대화해
 * 단일 스트림 대비 약 3배 처리량을 낸다(Cortex-A76 기준 ~10 GB/s).
 * 1024B 미만 잔여는 8B/4B/2B/1B 폴백으로 처리.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 초기화 시점에 crc32c_arm64_probe()가 HWCAP_CRC32 를 검사하여 ARM CRC 확장
 * 지원 여부를 `crc32c_arm64_available` 전역 플래그에 기록한다. 이후 런타임에
 * fio_crc32c() 인라인 디스패처(crc32c.h)가 이 플래그를 보고 하드웨어 경로 또는
 * 소프트웨어 폴백(crc32c_sw, crc32c.c)을 선택한다. 같은 디스패처는 x86 에서는
 * crc32c_intel_available 를 본다(crc32c-intel.c).
 * 호출 체인:
 *   fio 시작 → crc32c_arm64_probe → os_cpu_has(CPU_ARM64_CRC32C)
 *   verify.c → fio_crc32c() [crc32c.h] → (arm64 가능 시) crc32c_arm64 → __crc32cd/vmull_p64
 *
 * === 타 모듈과의 연결 ===
 * - crc32c.h: fio_crc32c() 인라인 디스패처와 crc32c_arm64_available 전역 extern.
 * - os/os.h: os_cpu_has(CPU_ARM64_CRC32C) 커버 매크로(Linux 의 경우
 *   getauxval(AT_HWCAP) & HWCAP_CRC32, macOS/iOS 는 sysctl hw.optional.armv8_crc32 등).
 * - arm_acle.h / arm_neon.h: ARM C Language Extensions — __crc32c{b,h,w,d} 인트린직과
 *   poly64_t/vmull_p64 PMULL 인트린직을 공급.
 * - crc32c.c: ARM CRC 미지원 CPU 의 소프트웨어 폴백.
 * 데이터 흐름: verify 경로에서 I/O 버퍼 포인터 + 길이 전달 → 64비트 누적 crc 반환.
 * 동기화: crc32c_arm64_available 은 probe 에서만 기록되고 이후 read-only — race 없음.
 * crc32c_probed 도 동일. 본 계산 함수는 순수 — 재진입 안전.
 *
 * === 주요 함수 요약 ===
 * - crc32c_arm64_probe(): HWCAP_CRC32 검사로 ARM CRC 확장 지원 여부 확정.
 * - crc32c_arm64(data, length): 1024B 블록 3-way 파이프라인 + 잔여 폴백.
 * - CRC32C3X8/CRC32C7X3X8 매크로: 블록 내부 루프 언롤용 코드 확장 도우미.
 */
#include "crc32c.h"
/* [한국어] crc32c.h: fio_crc32c() 인라인 디스패처, crc32c_arm64/intel_available 전역
 * extern, crc32c_sw() 프로토타입을 제공. verify.c 가 이 헤더만 포함하면 된다. */
#include "../os/os.h"
/* [한국어] os/os.h: os_cpu_has(CPU_ARM64_CRC32C) 매크로 — 플랫폼별 HWCAP/sysctl 래퍼.
 * Linux 는 getauxval(AT_HWCAP) & HWCAP_CRC32 로 확인. */

/* [한국어] ARM64 CRC 확장 가용성 플래그 — 초기에 false, probe() 가 결과 기록.
 * 설정자: crc32c_arm64_probe() (프로세스 시작 시 1회).
 * 읽는 자: fio_crc32c() 인라인 디스패처(crc32c.h) — true 면 본 파일 경로, 아니면 sw.
 * 값 범위: true/false.
 * 동기화: probe 이후 읽기 전용이므로 락 불필요. */
bool crc32c_arm64_available = false;

#ifdef ARCH_HAVE_CRC_CRYPTO
/* [한국어] 빌드 가드: configure 단계에서 arm_acle.h + -march=armv8-a+crc(+crypto) 가
 * 성공적으로 감지되어야 본 블록이 컴파일된다. 그 외(x86, 구형 ARM, 크로스 컴파일 실패)
 * 에서는 본 파일은 전역 플래그 정의만 남고 함수 본체는 사라진다. */

/*
 * [한국어] 3-way 병렬 CRC 매크로 정의.
 *
 * CRC32C3X8(ITR): 한 "반복 슬롯 ITR" 에서 세 개의 CRC 스트림(crc0/crc1/crc2)에 대해
 *   각각 8바이트(uint64_t) 1개씩을 처리한다. 세 스트림은 각각 data 포인터 기준으로
 *   336바이트(42*8) 간격을 두고 서로 다른 영역을 읽는다 — 블록 1024B 를
 *   8(초기 seed) + 336 + 336 + 336 + 8(마지막 병합) 로 분할하기 때문.
 *
 * CRC32C7X3X8(ITR): CRC32C3X8 을 7회 언롤해 21개의 __crc32cd 연산을 한 매크로 확장에
 *   담는다. 아래 루프는 이 매크로를 6회 호출(ITR=0..5)하여 총 42*3 = 126개의 64비트
 *   연산으로 1024B 본체를 소화한다.
 *
 * 왜 3-way? ARM Cortex-A76/Neoverse 의 __crc32cd 지연(latency)은 약 2~3사이클,
 * 처리량(throughput)은 1/1사이클. 스트림이 1개면 매 명령어가 직전 결과에 의존해
 * 파이프라인이 stall 되지만, 3개면 서로 독립이라 ILP 로 거의 3배 throughput 달성.
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
/* [한국어] arm_acle.h: __crc32cb/h/w/d 인트린직(ARM C Language Extensions). ARMv8+CRC
 * 기능이 있어야 링크 시 적절한 CRC32 명령어로 번역된다. */
#include <arm_neon.h>
/* [한국어] arm_neon.h: NEON/Crypto 인트린직 — 여기서는 poly64_t 타입과 vmull_p64
 * (Polynomial Multiply Long, PMULL) 가 필요. CRC reduction 트릭에 사용. */

/* [한국어] probe 가 한 번만 수행됐는지 표기하는 정적 플래그 — race 방지용.
 * 설정자/읽는 자: crc32c_arm64_probe() 내부.
 * 값 범위: true/false.
 * 동기화: 호출은 초기화 시점 단일 스레드 — 별도 락 불요. */
static bool crc32c_probed;

/*
 * Function to calculate reflected crc with PMULL Instruction
 * crc done "by 3" for fixed input block size of 1024 bytes
 */
/*
 * [한국어]
 * crc32c_arm64 - ARM CRC 확장 + PMULL 로 CRC-32C 고속 계산
 *
 * @data:   입력 바이트 포인터.
 * @length: 전체 길이(바이트).
 * @return: CRC-32C 누적값(최종 XOR 없음, reflected).
 *
 * 동작:
 *   1) 1024B 블록 반복:
 *      - data 의 처음 8B 를 crc0 시드로 흡수, crc1/crc2 를 0으로 초기화.
 *      - CRC32C7X3X8(0..5) 로 42*3 개의 __crc32cd 연산(336B 스트림 × 3) 수행.
 *      - 블록 끝 8B 는 crc2 로 직접 흡수.
 *      - PMULL(vmull_p64) 로 crc0·crc1 에 reduction 상수 K1(0xe417f38a)·
 *        K2(0x8f158014) 를 다항식 곱하여 crc2 와 XOR 합쳐 하나의 CRC 로 축약.
 *   2) 남은 <1024B 는 8B, 4B, 2B, 1B 순서로 해당 크기 __crc32c* 호출.
 *
 * 실행 컨텍스트: 잡/verify 스레드. probe 는 init 단계 1회 후 고정.
 *
 * 호출 체인: fio_crc32c() [crc32c.h] → [crc32c_arm64] → __crc32cd/__crc32cw/__crc32ch/__crc32cb + vmull_p64
 */
uint32_t crc32c_arm64(unsigned char const *data, unsigned long length)
{
	/* [한국어] 부호 있는 len — 아래 while 에서 "len -= 1024" 한 뒤 음수 비교로
	 * 블록 루프 종료 여부를 판단하기 위해 signed 로 선언. */
	signed long len = length;
	/* [한국어] CRC-32C 초기값 ~0 (reflected 방식, Castagnoli 표준). */
	uint32_t crc = ~0;
	/* [한국어] 3-way 스트림의 세 누적 레지스터 — 아래 블록 루프에서 초기화. */
	uint32_t crc0, crc1, crc2;

	/* Load two consts: K1 and K2 */
	/* [한국어] PMULL reduction 상수. 블록 끝에서 crc0·crc1 를 crc2 와 합치는데 사용.
	 * K1 = x^(1024*8+32) mod P(x), K2 = x^(1024*8-32*2) mod P(x) 의 각각의 값. */
	const poly64_t k1 = 0xe417f38a, k2 = 0x8f158014;
	/* [한국어] PMULL 결과를 담을 임시 64비트 레지스터 t0/t1. */
	uint64_t t0, t1;

	/* [한국어] 1024B 단위 블록 처리 — len 이 음수가 되면 블록 1개 공간이 없으므로 탈출. */
	while ((len -= 1024) >= 0) {
		/* Do first 8 bytes here for better pipelining */
		/* [한국어] 블록 첫 8B 를 crc0 에 흡수 — 파이프라이닝을 위해 루프 본체 바깥에서 로드. */
		crc0 = __crc32cd(crc, *(const uint64_t *)data);
		/* [한국어] 나머지 두 스트림은 0부터 시작(이들 스트림은 아직 입력을 받지 않았음). */
		crc1 = 0;
		crc2 = 0;
		/* [한국어] data 포인터를 8B 전진 — 이제 data 는 3*336B 본체의 시작점. */
		data += sizeof(uint64_t);

		/* Process block inline
		   Process crc0 last to avoid dependency with above */
		/* [한국어] 42*3 개의 __crc32cd 를 7×3×8 (=168) 전개로 수행 — 매크로 인자 0..5 로 6회. */
		CRC32C7X3X8(0);
		CRC32C7X3X8(1);
		CRC32C7X3X8(2);
		CRC32C7X3X8(3);
		CRC32C7X3X8(4);
		CRC32C7X3X8(5);

		/* [한국어] 본체 336*3 = 1008B 소비 후 data 포인터 전진. */
		data += 42*3*sizeof(uint64_t);

		/* Merge crc0 and crc1 into crc2
		   crc1 multiply by K2
		   crc0 multiply by K1 */
		/* [한국어] crc1 을 상수 K2 와 다항식 곱(Polynomial Multiply): PMULL 한 번. */
		t1 = (uint64_t)vmull_p64(crc1, k2);
		/* [한국어] crc0 을 상수 K1 과 다항식 곱. */
		t0 = (uint64_t)vmull_p64(crc0, k1);
		/* [한국어] 블록 마지막 8B 를 crc2 에 직접 흡수 → 축약의 기반. */
		crc = __crc32cd(crc2, *(const uint64_t *)data);
		/* [한국어] t1(=crc1*K2) 을 다시 CRC32 로 환원 — 시작 seed 0 으로 다항식 다이제스트. */
		crc1 = __crc32cd(0, t1);
		/* [한국어] 환원된 crc1 을 최종 누적에 XOR. */
		crc ^= crc1;
		/* [한국어] t0(=crc0*K1) 환원. */
		crc0 = __crc32cd(0, t0);
		/* [한국어] 환원된 crc0 을 최종 누적에 XOR — 3-way 병합 완료. */
		crc ^= crc0;

		/* [한국어] 블록 마지막 8B 소비 — 다음 블록 시작 포인터 전진. */
		data += sizeof(uint64_t);
	}

	/* [한국어] 블록 루프 탈출 후 len 은 "추가한 -1024 때문에 음수일 수 있으므로" 다시 복구.
	 * len += 1024 결과가 0이면 딱 맞아 떨어졌다는 의미 → 더 처리할 것 없음. */
	if (!(len += 1024))
		return crc;

	/* [한국어] 8바이트 단위 잔여 처리 — 8B 미만이 남으면 탈출. */
	while ((len -= sizeof(uint64_t)) >= 0) {
                /* [한국어] 8바이트 블록 한 개를 64비트 CRC 명령으로 흡수. */
                crc = __crc32cd(crc, *(const uint64_t *)data);
                /* [한국어] 포인터 8B 전진. */
                data += sizeof(uint64_t);
        }

        /* The following is more efficient than the straight loop */
        /* [한국어] 4바이트가 남아있으면 32비트 명령으로 한 번에 흡수. */
        if (len & sizeof(uint32_t)) {
                /* [한국어] __crc32cw: 32비트 CRC32C 인트린직. */
                crc = __crc32cw(crc, *(const uint32_t *)data);
                /* [한국어] 포인터 4B 전진. */
                data += sizeof(uint32_t);
        }
        /* [한국어] 2바이트 잔여 처리. */
        if (len & sizeof(uint16_t)) {
                /* [한국어] __crc32ch: 16비트 CRC32C 인트린직. */
                crc = __crc32ch(crc, *(const uint16_t *)data);
                /* [한국어] 포인터 2B 전진. */
                data += sizeof(uint16_t);
        }
        /* [한국어] 1바이트 잔여 처리. */
        if (len & sizeof(uint8_t)) {
                /* [한국어] __crc32cb: 8비트 CRC32C 인트린직 — 마지막이므로 포인터 전진 불필요. */
                crc = __crc32cb(crc, *(const uint8_t *)data);
        }

	/* [한국어] 최종 32비트 CRC-32C 반환. 호출자(fio_crc32c 인라인)가 그대로 유저에게 전달. */
	return crc;
}

/*
 * [한국어]
 * crc32c_arm64_probe - ARM CRC 확장 지원 여부 감지(1회성)
 *
 * os/os.h 의 os_cpu_has(CPU_ARM64_CRC32C) 를 호출하여 HWCAP_CRC32 비트 또는
 * 플랫폼별 sysctl 결과를 확인하고, crc32c_arm64_available 전역 플래그에 기록한다.
 * 이미 probed 되었으면 즉시 반환 — 두 번 호출 시 cost 0.
 *
 * 실행 컨텍스트: fio 프로세스 초기화 시점(init 스레드 단일). 이후 flag 는 read-only.
 *
 * 호출 체인: fio main()/init → [crc32c_arm64_probe] → os_cpu_has
 */
void crc32c_arm64_probe(void)
{
	/* [한국어] 이미 감지했으면 skip — idempotent 보장. */
	if (!crc32c_probed) {
		/* [한국어] 플랫폼별 CPU 기능 질의 — Linux: getauxval(AT_HWCAP)&HWCAP_CRC32. */
		crc32c_arm64_available = os_cpu_has(CPU_ARM64_CRC32C);
		/* [한국어] 재진입 방지 플래그 세팅. */
		crc32c_probed = true;
	}
}

#endif /* ARCH_HAVE_CRC_CRYPTO */
/* [한국어] 빌드 가드 종료. 비-ARM64 또는 CRC 확장 미지원 플랫폼에서는 전역 플래그
 * 정의만 남고 계산 함수는 링크 포함되지 않는다(fio_crc32c 디스패처는 false 를 보고 sw). */
