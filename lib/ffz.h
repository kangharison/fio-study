/*
 * [한국어 설명] Find First Zero / First Set Bit 유틸 헤더 (ffz.h)
 *
 * === 파일의 역할 ===
 * 64 비트 워드에서 최하위 1 비트(LSB 1) 의 위치를 찾는 `ffs64()` 순수 인라인과,
 * 비트마스크를 반전(~) 시켜 ffs64 를 호출함으로써 "최하위 0 비트" 위치를
 * 찾는 `ffz()`/`ffz64()` 를 제공한다. 본 파일은 axmap(계층 비트맵) 의 "다음
 * 미설정 비트 찾기" 의 핵심 원시 연산을 제공한다. axmap_next_free 는 상위
 * 레벨에서 ffz 를 호출해 빈 슬롯(0 비트) 이 있는 64비트 워드를 찾고, 그 안의
 * 정확한 비트 위치를 ffs64 로 내려가며 파악한다. ARCH_HAVE_FFZ 가 정의된
 * 아키텍처(x86 의 `bsf` 명령 등) 에서는 arch.h 의 최적화 구현으로 치환되도록
 * #ifndef 가드를 둔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "비트 탐색" 최하층. axmap.c 가 주 소비자이고, 그 외에도 각종 플래그
 * 비트맵에서 빈 슬롯을 찾을 때 쓰인다.
 * 호출 체인:
 *   axmap_next_free → 레벨 k 의 워드 w → ffz(w) or ffz64(w) → 레벨 k-1 로 이동
 *   → (하위 레벨 반복) → 최종 비트 인덱스 반환.
 *
 * === 타 모듈과의 연결 ===
 * - axmap.c : 주 소비자.
 * - arch/arch-*.h : ARCH_HAVE_FFZ 정의 시 arch_ffz 로 치환(x86 의 bsfq 등).
 * - <inttypes.h> : uint64_t.
 * 데이터 흐름: 64비트 비트마스크 → ffs64/ffz → 0..63 위치 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - ffs64(word) : word 의 LSB 1 비트 위치(0..63). word==0 이면 호출자가
 *   사전 검사 필요(이 구현은 undefined 아닌 0 반환이지만 의미 없음).
 * - ffz(bitmask) : unsigned long 비트마스크의 첫 0 비트 위치. ~mask 를
 *   ffs64 에 넣어 구함. ARCH_HAVE_FFZ 가 있으면 arch_ffz 매크로로 치환.
 * - ffz64(bitmask) : 64비트 전용 버전. 항상 generic 구현.
 */
#ifndef FIO_FFZ_H
#define FIO_FFZ_H
/* [한국어] 헤더 가드. axmap.c / 각 엔진 등 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t. 본 헤더의 주 연산 타입. */

/*
 * [한국어] ffs64 - 64비트 워드에서 최하위 설정 비트(LSB) 의 위치를 반환.
 *
 * @word: 검사 대상 비트 패턴.
 * @return: 0..63 — LSB 1 비트의 0-기반 인덱스. word==0 이면 마지막 단계의
 *          `if(!(word & 1))` 까지 모두 실행되어 63 을 반환하지만 의미 없음
 *          (호출자가 사전에 word != 0 를 확인해야 함).
 *
 * 이진 탐색 방식으로 O(log 64) = O(6) 스텝에 찾는다.
 * 하위 절반에 비트가 없으면 상위 절반으로 이동하며 위치를 누적한다.
 *
 * 실행 컨텍스트: 헤더 인라인, 순수 함수, 재진입 안전.
 */
static inline int ffs64(uint64_t word)
{
	int r = 0;
	/* [한국어] 누적 비트 오프셋. 하위 절반에 비트가 없으면 32→16→8→4→2→1
	 * 씩 더해가며 최종 LSB 위치를 도출. */

	/* [한국어] 하위 32비트가 모두 0 이면 상위 32비트로 이동. */
	if ((word & 0xffffffff) == 0) {
		r += 32;
		word >>= 32;
		/* [한국어] 비트가 상위 32 에만 있으므로 시프트하여 하위 32 로 가져
		 * 와 아래 스텝들이 동일 로직을 재사용할 수 있게 한다. */
	}
	if (!(word & 0xffff)) {
		word >>= 16;
		r += 16;
		/* [한국어] 하위 16비트가 0 이면 다음 16비트 창으로. */
	}
	if (!(word & 0xff)) {
		word >>= 8;
		r += 8;
		/* [한국어] 하위 8비트가 0 이면 다음 8비트 창으로. */
	}
	if (!(word & 0xf)) {
		word >>= 4;
		r += 4;
		/* [한국어] 하위 4비트(니블) 가 0 이면 다음 니블로. */
	}
	if (!(word & 3)) {
		word >>= 2;
		r += 2;
		/* [한국어] 하위 2비트가 0 이면 다음 2비트 쌍으로. */
	}
	if (!(word & 1))
		r += 1;
		/* [한국어] 남은 1 비트 검사 — 0 이면 r 을 1 증가(상위 비트가 LSB
		 * 임을 의미). 2 비트 쌍이 01, 10, 11 로 남아 있어야 LSB 가 그 쌍에 있음. */

	return r;
	/* [한국어] 최종 LSB 1 비트의 0-기반 인덱스. */
}

#ifndef ARCH_HAVE_FFZ
/* [한국어] ARCH_HAVE_FFZ : arch-x86.h / arch-arm64.h 등이 네이티브 명령(bsf/clz
 * 역활용) 으로 최적화된 arch_ffz 를 제공할 때 정의. 정의되지 않은 아키텍처
 * 에서는 아래 generic 폴백을 인라인으로 사용. */

/* [한국어] ffz - 첫 번째 0 비트를 찾음. 비트마스크를 반전(~) 시켜 ffs64 로 찾기.
 * unsigned long 타입으로 받는 것은 axmap 의 내부 워드 크기와 일치시키기 위함. */
static inline int ffz(unsigned long bitmask)
{
	return ffs64(~bitmask);
	/* [한국어] 비트마스크의 "0 비트 위치" 는 "반전된 비트마스크의 1 비트 위치"
	 * 와 동일. 이 항등식으로 하나의 구현을 재사용. */
}

#else
#define ffz(bitmask)	arch_ffz(bitmask)
/* [한국어] 아키텍처가 최적화 버전을 제공하면 단순 매크로 치환. 호출자 쪽
 * 컴파일 결과는 단일 CPU 명령으로 접힘. */
#endif

static inline int ffz64(uint64_t bitmask)
{
	return ffs64(~bitmask);
	/* [한국어] ffz 의 64비트 전용 버전. axmap.c 가 64비트 워드를 다룰 때
	 * unsigned long 의 크기에 의존하지 않기 위해 명시적 64비트 API 를 따로
	 * 둔다(32비트 플랫폼에서는 unsigned long 이 32비트이므로 ffz 와 폭 차이). */
}

#endif
/* [한국어] FIO_FFZ_H 가드 종료. */
