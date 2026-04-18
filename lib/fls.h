/*
 * [한국어 설명] Find Last (Most-Significant) Set Bit 유틸 헤더 (fls.h)
 *
 * === 파일의 역할 ===
 * 32 비트 정수 x 에서 가장 높은 위치(1-기반) 의 1 비트 인덱스를 반환하는
 * 순수 인라인 함수 `__fls(x)` 를 제공한다. 이진 탐색(16→8→4→2→1 5 단계) 으로
 * 64 사이클 이하에 결과를 내며, 외부 라이브러리 의존 없이 헤더 only 로
 * 구현되어 있어 어떤 fio 소스든 포함 즉시 사용 가능하다. `__fls(0)=0`,
 * `__fls(1)=1`, `__fls(0x80000000)=32` 로 정의되며 Linux 커널의
 * include/asm-generic/bitops/fls.h 와 수학적으로 동일한 의미(반환 값이
 * ctz vs clz 관점에서 1-기반). fio 에서는 주로 `roundup_pow2` 에서 호출되어
 * "이 값 이상인 최소 2^k" 를 log O(1) 에 구하는 데 쓰인다. 직접 호출되는
 * 곳은 lfsr.c 의 size→비트 수 계산, axmap.c 의 레벨 계산 등.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "비트 연산 최하층" 유틸. 다른 헤더들이 거의 자유롭게 포함한다.
 * __builtin_clz 기반 최적화는 아키텍처별 구현이 존재하는 경우 arch.h 를
 * 통해 오버라이드될 수 있지만, 본 파일은 generic fallback 버전을 제공한다.
 * 호출 체인: roundup_pow2 → __fls(depth-1) / lfsr_init → __fls(size).
 *
 * === 타 모듈과의 연결 ===
 * - roundup.h : __fls 를 쓰는 주요 인라인.
 * - axmap.c : 계층 레벨 수 계산.
 * - lfsr.c : 필요 LFSR 비트 폭 결정.
 * 데이터 흐름: 정수 입력 → MSB 위치 반환. 순수 함수.
 *
 * === 주요 함수/구조체 요약 ===
 * - __fls(int x) : x 의 MSB 위치(1..32). x==0 이면 0.
 */
#ifndef _ASM_GENERIC_BITOPS_FLS_H_
#define _ASM_GENERIC_BITOPS_FLS_H_
/* [한국어] 헤더 가드 이름이 `_ASM_GENERIC_BITOPS_FLS_H_` 인 것은 Linux 커널의
 * include/asm-generic/bitops/fls.h 를 사실상 그대로 포팅한 파일임을 반영한다.
 * 이름이 바뀌면 해당 커널 파일과의 충돌 여지가 사라지고 소스 단위 추적성
 * 이 유지된다. */

/**
 * fls - find last (most-significant) bit set
 * @x: the word to search
 *
 * This is defined the same way as ffs.
 * Note fls(0) = 0, fls(1) = 1, fls(0x80000000) = 32.
 */

/*
 * [한국어]
 * __fls - 32 비트 정수 x 의 최상위 1 비트 위치(1-기반) 반환.
 *
 * @x: 검사 대상(int). signed 지만 비트 연산만 수행하므로 음수여도 같은 결과.
 * @return: 1..32 범위의 MSB 위치. x==0 이면 0.
 *
 * 동작 원리: 이진 탐색으로 상위 16/8/4/2/1 비트씩 확인하며 r 을 조정.
 *   16비트 상위에 비트가 없으면 x <<= 16 하고 r 을 16 빼서 8비트 창으로
 *   축소. 같은 방식으로 8→4→2→1 순으로 내려간다. 최종 r 이 x 의 MSB 위치.
 *
 * 실행 컨텍스트: 헤더 인라인, 순수 함수, 재진입 안전.
 * 호출 체인: roundup_pow2 / axmap.c / lfsr.c → [이 함수].
 * 대안: GCC/Clang 은 __builtin_clz(x) 로 32 - __builtin_clz(x) 를 계산해
 *   더 빠를 수 있으나, 본 구현은 비-GCC 환경과 arch.h 미구현 환경에서도
 *   동일 결과를 보장하는 generic 폴백이다.
 */
static inline int __fls(int x)
{
	int r = 32;
	/* [한국어] r 을 전체 비트 폭(32) 으로 시작. 아래 5 단계에서 x 의 MSB
	 * 를 최상위 비트(bit 31) 쪽으로 시프트하며, 비트가 없는 상위 구간만큼
	 * r 에서 빼는 이진 탐색. */

	if (!x)
		return 0;
	/* [한국어] x==0 은 MSB 가 없으므로 0 반환(커널 규약). 호출자(roundup_pow2
	 * 등) 가 이 경우를 별도 처리해야 함. */

	if (!(x & 0xffff0000u)) {
		/* [한국어] 상위 16 비트가 모두 0 이면 MSB 는 하위 16 비트에 있음.
		 * x 를 좌측으로 16 시프트해 하위 16 비트를 상위 16 비트 위치로
		 * 이동시키고, r 에서 16 을 뺀다. */
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		/* [한국어] 상위 8 비트가 0 이면 같은 방식으로 8 씩 축소. */
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		/* [한국어] 상위 4 비트 확인. */
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		/* [한국어] 상위 2 비트 확인. */
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		/* [한국어] 최상위 1 비트가 0 이면 r 을 1 추가로 감소. 이 단계가
		 * 끝나면 r 은 정확히 MSB 위치. 시프트는 더 이상 필요 없음. */
		r -= 1;
	}
	return r;
	/* [한국어] r 반환. 예: x=1 → 상위 30 비트 0 → r=30 감소 → r=2, 다시 MSB
	 * 1 비트 없음으로 r--=1. (검증: fls(1) == 1 규약 만족) */
}

#endif /* _ASM_GENERIC_BITOPS_FLS_H_ */
/* [한국어] 가드 종료. */
