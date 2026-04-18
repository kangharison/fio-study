/*
 * [한국어 설명] 선형 피드백 시프트 레지스터(LFSR) 기반 "전범위 1회 방문" 생성기 (lfsr.c)
 *
 * === 파일의 역할 ===
 * Galois-type XNOR LFSR 을 이용하여, `[0, max_val]` 범위의 모든 정수를 중복 없이
 * 정확히 한 번씩 순회하는(= permutation) 의사 난수 시퀀스를 생성한다. n-비트
 * 최대 LFSR 은 주기가 `2^n - 1` 이며, 금지 상태 하나(XNOR 의 경우 all-ones)를
 * 제외하고 모든 비영(非零) 상태를 한 번씩 거친다는 성질을 활용해 fio 가 랜덤맵
 * (axmap) 을 쓰지 않고도 "모든 블록을 정확히 한 번씩 방문한다" 를 보장하는 데
 * 사용한다. spin(한 출력당 추가 시프트) 기법으로 연속 출력 간의 상관성을
 * 줄이고, Latin-square 스킵(prepare_spin) 으로 spin 으로 인해 발생할 수 있는
 * 부분 순환(sub-sequence cycle) 을 사전 탐지·보정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 랜덤 I/O 오프셋 선택 파이프라인 중 `--random_generator=lfsr` 옵션이
 * 선택됐을 때 사용된다. 일반적인 경로는 다음과 같다:
 *
 *   init.c (th 초기화)
 *     → init_random_map()                 // 옵션에 따라 axmap 또는 lfsr 선택
 *         → lfsr_init(&fl, nr_blocks, seed, spin)   // [본 파일의 초기화]
 *
 *   io_u.c (각 I/O 단위 생성)
 *     → get_next_offset()                 // 방향/분포에 따라 분기
 *         → get_next_rand_block()
 *             → get_next_rand_offset_lfsr(&fl, &offset)
 *                 → lfsr_next(&fl, &offset)         // [본 파일의 주 엔트리]
 *
 * 실행 컨텍스트: 잡 스레드(thread_data) 당 하나의 struct fio_lfsr 를 소유하며
 * 단일 스레드 접근이므로 락 불필요. 전체 주기가 소진되면 lfsr_next 가 1 을
 * 반환하여 잡이 "random 범위 전체 방문 완료" 를 인지하고 종료 시점을 잡는다.
 *
 * === 타 모듈과의 연결 ===
 * - lfsr.h : struct fio_lfsr 정의 및 lfsr_init/lfsr_reset/lfsr_next 선언.
 * - compiler/compiler.h : fio_fallthrough (switch fall-through 명시, -Wimplicit-fallthrough 회피),
 *                         fio_unlikely (분기 힌트, max_val 초과 재시도 분기 최적화).
 * - io_u.c : get_next_rand_offset_lfsr 가 lfsr_next 를 호출하며 오프셋을 받는다.
 *            "일반 난수 → axmap" 경로 대신 LFSR 경로를 선택한 잡에서만 활성.
 * - 메모리 오버헤드: axmap 은 블록 수에 비례한 비트맵(약 1.58%) 을 요구하지만,
 *                     LFSR 은 struct fio_lfsr(64바이트 수준) 만 사용 → TB급 장치에서 이점.
 * - 단점: axmap 은 "다음 빈 블록" 탐색이 O(log) 이고 set/isset 이 가능하지만,
 *         LFSR 은 오직 "다음 값을 생성" 만 가능하며 "특정 블록 방문 여부" 조회 불가.
 *
 * === 주요 함수/구조체 요약 ===
 * - lfsr_init(fl, nums, seed, spin): LFSR 을 구성(탭/마스크/cached_bit/spin 분석/시드 주입).
 * - lfsr_reset(fl, seed): 시드만 다시 주입 (금지 상태 = all-ones 검사).
 * - lfsr_next(fl, *off): 다음 고유 오프셋 생성. 전체 주기 소진 시 1 반환.
 * - __lfsr_next(fl, spin): spin+1 회의 XNOR 스텝을 switch fall-through 로 O(1) 실행.
 * - __LFSR_NEXT(fl, v): 한 스텝의 XNOR 피드백 연산을 구현한 매크로.
 * - find_lfsr(size): size 보다 큰 최소 2^n 을 찾아 n-비트 LFSR 의 탭 배열을 반환.
 * - lfsr_create_xormask(taps): 탭 위치 배열 → XOR 피드백 비트 마스크 변환.
 * - prepare_spin(fl, spin): spin 이 만들 수 있는 부분 순환 길이를 사전 계산(Latin-square 스킵).
 * - lfsr_taps[64][FIO_MAX_TAPS]: 3~63비트 LFSR 의 최대 주기 탭 위치 테이블(참조 출처 주석 참고).
 */

#include <stdio.h>	/* [한국어] (현재 디버깅용 printf 흔적이 남아있을 수 있음을 대비한 포함. 실제 본 파일은 stdio 호출 없음) */

#include "lfsr.h"			/* [한국어] struct fio_lfsr, FIO_MAX_TAPS, 공개 API 선언 */
#include "../compiler/compiler.h"	/* [한국어] fio_fallthrough(명시적 switch fall-through 매크로), fio_unlikely(분기 힌트) 제공 */

/*
 * LFSR taps retrieved from:
 * http://home1.gte.net/res0658s/electronics/LFSRtaps.html
 *
 * The memory overhead of the following tap table should be relatively small,
 * no more than 400 bytes.
 */
/*
 * [한국어] lfsr_taps[degree][i] — 각 비트 수(degree) 의 최대 주기 LFSR 탭 위치
 *
 * 표의 각 행은 "몇 번째 비트들을 XOR(XNOR) 피드백에 사용하면 해당 비트 폭에서
 * 최대 주기(2^degree - 1) 를 얻는가" 를 나타낸다. [0], [1], [2] 는 사용 불가
 * (3비트 미만 LFSR 은 최대 주기가 지나치게 짧음). 각 행의 0 으로 끝나는 부분은
 * 탭이 더 이상 없음을 의미. 예: {12, 6, 4, 1} → 비트 12, 6, 4, 1 의 XOR.
 *
 * 값 범위: 각 엔트리는 1~degree. 설정자: 컴파일 타임 상수 테이블.
 * 읽는 자: find_lfsr() 이 degree 를 골라 이 테이블의 해당 행을 반환.
 */
static uint8_t lfsr_taps[64][FIO_MAX_TAPS] =
{
	{0}, {0}, {0},		//LFSRs with less that 3-bits cannot exist
	/* [한국어] 3비트 미만 — 최대 주기가 3이하라 사용 의미 없음 */
	{3, 2},			//Tap position for 3-bit LFSR
	{4, 3},			//Tap position for 4-bit LFSR
	{5, 3},			//Tap position for 5-bit LFSR
	{6, 5},			//Tap position for 6-bit LFSR
	{7, 6},			//Tap position for 7-bit LFSR
	{8, 6, 5 ,4},		//Tap position for 8-bit LFSR — 4탭 XNOR
	{9, 5},			//Tap position for 9-bit LFSR
	{10, 7},		//Tap position for 10-bit LFSR
	{11, 9},		//Tap position for 11-bit LFSR
	{12, 6, 4, 1},		//Tap position for 12-bit LFSR
	{13, 4, 3, 1},		//Tap position for 13-bit LFSR
	{14, 5, 3, 1},		//Tap position for 14-bit LFSR
	{15, 14},		//Tap position for 15-bit LFSR
	{16, 15, 13, 4},	//Tap position for 16-bit LFSR
	{17, 14},		//Tap position for 17-bit LFSR
	{18, 11},		//Tap position for 18-bit LFSR
	{19, 6, 2, 1},		//Tap position for 19-bit LFSR
	{20, 17},		//Tap position for 20-bit LFSR
	{21, 19},		//Tap position for 21-bit LFSR
	{22, 21},		//Tap position for 22-bit LFSR
	{23, 18},		//Tap position for 23-bit LFSR
	{24, 23, 22, 17},	//Tap position for 24-bit LFSR
	{25, 22},		//Tap position for 25-bit LFSR
	{26, 6, 2, 1},		//Tap position for 26-bit LFSR
	{27, 5, 2, 1},		//Tap position for 27-bit LFSR
	{28, 25},		//Tap position for 28-bit LFSR
	{29, 27},		//Tap position for 29-bit LFSR
	{30, 6, 4, 1},		//Tap position for 30-bit LFSR
	{31, 28},		//Tap position for 31-bit LFSR
	{32, 31, 29, 1},	//Tap position for 32-bit LFSR — 4GiB 블록 커버
	{33, 20},		//Tap position for 33-bit LFSR
	{34, 27, 2, 1},		//Tap position for 34-bit LFSR
	{35, 33},		//Tap position for 35-bit LFSR
	{36, 25},		//Tap position for 36-bit LFSR
	{37, 5, 4, 3, 2, 1},	//Tap position for 37-bit LFSR — 6탭
	{38, 6, 5, 1},		//Tap position for 38-bit LFSR
	{39, 35},		//Tap position for 39-bit LFSR
	{40, 38, 21, 19},	//Tap position for 40-bit LFSR — 1TiB 블록 영역
	{41, 38},		//Tap position for 41-bit LFSR
	{42, 41, 20, 19},	//Tap position for 42-bit LFSR
	{43, 42, 38, 37},	//Tap position for 43-bit LFSR
	{44, 43, 18, 17},	//Tap position for 44-bit LFSR
	{45, 44, 42, 41},	//Tap position for 45-bit LFSR
	{46, 45, 26, 25},	//Tap position for 46-bit LFSR
	{47, 42},		//Tap position for 47-bit LFSR
	{48, 47, 21, 20},	//Tap position for 48-bit LFSR
	{49, 40},		//Tap position for 49-bit LFSR
	{50, 49, 24, 23},	//Tap position for 50-bit LFSR
	{51, 50, 36, 35},	//Tap position for 51-bit LFSR
	{52, 49},		//Tap position for 52-bit LFSR
	{53, 52, 38, 37},	//Tap position for 53-bit LFSR
	{54, 53, 18, 17},	//Tap position for 54-bit LFSR
	{55, 31},		//Tap position for 55-bit LFSR
	{56, 55, 35, 34},	//Tap position for 56-bit LFSR
	{57, 50},		//Tap position for 57-bit LFSR
	{58, 39},		//Tap position for 58-bit LFSR
	{59, 58, 38, 37},	//Tap position for 59-bit LFSR
	{60, 59},		//Tap position for 60-bit LFSR
	{61, 60, 46, 45},	//Tap position for 61-bit LFSR
	{62, 61, 6, 5},		//Tap position for 62-bit LFSR
	{63, 62},		//Tap position for 63-bit LFSR — 최대 주기 약 9.2E18, 8EiB
};

/*
 * [한국어] __LFSR_NEXT 매크로 - XNOR Galois LFSR 의 한 스텝을 실행
 *
 * 구현 원리:
 *   1. v >> 1 : 전체 레지스터를 1비트 오른쪽으로 시프트 (하위 비트 탈락).
 *   2. | cached_bit : 최상위 위치(1 << (degree-1)) 에 1 을 OR 로 밀어 넣음.
 *      (이 형태가 "Galois" 변형이며, 일반 Fibonacci LFSR 과 달리 XOR 분포가
 *       레지스터 중간 탭들에 분산되어 구현이 단순해짐.)
 *   3. ((v & 1ULL) - 1ULL) & xormask :
 *        - 원래 LSB 가 1 이면 (1-1)=0 → mask=0 → XOR 없음.
 *        - 원래 LSB 가 0 이면 (0-1)=0xFFFFFFFF... → mask 전부 유효 → xormask 전체와 XOR.
 *      이것이 곧 "LSB 에 따라 조건부로 XOR" 이며, XNOR 변형이라 금지 상태가 all-ones.
 *   4. 결과를 __v 에 저장.
 *
 * 왜 XNOR 인가: 일반 LFSR 의 금지 상태(all-zeros)는 시드=0 을 명시적으로 배제해야
 * 하지만 XNOR 의 금지 상태(all-ones)는 자연스러운 비트마스크(max) 와 비교만 하면
 * 감지하기 쉬워서 구현 친화적. lfsr_reset 이 이 검사를 수행.
 */
#define __LFSR_NEXT(__fl, __v)						\
	__v = ((__v >> 1) | __fl->cached_bit) ^			\
			(((__v & 1ULL) - 1ULL) & __fl->xormask);

/*
 * [한국어] __lfsr_next - spin+1 회의 LFSR 스텝을 루프 없이 실행 (switch fall-through)
 *
 * @fl: LFSR 상태
 * @spin: 한 출력을 위해 실행할 "추가" 스텝 수 (0 이면 1회만 실행, 15 이면 16회)
 *
 * 동작:
 *   switch(spin) 에 case 15..0 을 두고 fall-through 로 이어붙였으므로, spin 값
 *   만큼 __LFSR_NEXT 가 반복된다. 컴파일러가 jump table 을 만들면 O(1) 진입 +
 *   unrolled 실행이 되어 루프 변수 업데이트 비용 없이 매우 빠르다.
 *
 * spin 의 의미:
 *   연속된 두 lfsr_next 출력은 기본적으로 예측 가능한 선형 관계를 가진다
 *   (LFSR 은 선형성). spin>0 을 두면 출력과 출력 사이에 "보이지 않는 추가 스텝"
 *   이 삽입되어 관측 가능한 상관성이 줄어든다. 단, spin 이 (2^n - 1) 과 특정
 *   비율을 만족하면 전체 주기의 1/i 만 방문하는 부분 순환이 발생하므로,
 *   prepare_spin 이 이를 탐지하여 lfsr_next 가 "+1 추가 스텝" 을 주기적으로
 *   끼워넣어 회피한다.
 *
 * fio_fallthrough: GCC 의 -Wimplicit-fallthrough 를 만족시키기 위한 속성.
 */
static inline void __lfsr_next(struct fio_lfsr *fl, unsigned int spin)
{
	/*
	 * This should be O(1) since most compilers will create a jump table for
	 * this switch.
	 */
	switch (spin) {
		case 15: __LFSR_NEXT(fl, fl->last_val);	/* [한국어] 15회 이후 14 로 폴스루 → 총 16스텝 */
		fio_fallthrough;
		case 14: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case 13: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case 12: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case 11: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case 10: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  9: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  8: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  7: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  6: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  5: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  4: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  3: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  2: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  1: __LFSR_NEXT(fl, fl->last_val);
		fio_fallthrough;
		case  0: __LFSR_NEXT(fl, fl->last_val);	/* [한국어] spin=0 이면 정확히 한 번만 실행 후 break */
		fio_fallthrough;
		default: break;				/* [한국어] spin > 15 는 prepare_spin 이 거절하므로 여기 진입 불가 */
	}
}

/*
 * lfsr_next does the following:
 *
 * a. Return if the number of max values has been exceeded.
 * b. Check if we have a spin value that produces a repeating subsequence.
 *    This is previously calculated in `prepare_spin` and cycle_length should
 *    be > 0. If we do have such a spin:
 *
 *    i. Decrement the calculated cycle.
 *    ii. If it reaches zero, add "+1" to the spin and reset the cycle_length
 *        (we have it cached in the struct fio_lfsr)
 *
 *    In either case, continue with the calculation of the next value.
 * c. Check if the calculated value exceeds the desirable range. In this case,
 *    go back to b, else return.
 */
/*
 * [한국어] lfsr_next - LFSR 에서 다음 고유 오프셋을 생성
 *
 * @fl:  LFSR 상태 (잡 스레드 소유, 단일 스레드 접근)
 * @off: 생성된 오프셋이 저장될 포인터 (값 범위: 0 ~ max_val)
 * @return: 0 = 성공, 1 = 전체 주기 소진 (더 이상 새로운 값 없음)
 *
 * 동작 요약:
 *   1. num_vals++ > max_val 이면 "모든 값을 이미 뽑았음" → 1 반환하여 잡 종료 신호.
 *   2. cycle_length>0 이고 소진되면 이번 호출만 spin+1 로 한 스텝 더 돌려 Latin
 *      square 스킵을 수행하고 cycle_length 를 cached_cycle_length 로 리필.
 *   3. max_val 보다 큰 값이 나오면 do-while 로 다음 값까지 건너뜀 (n-비트 LFSR
 *      은 2^n-1 개 값을 생성하지만 max_val 은 그보다 작을 수 있기 때문).
 *
 * 호출자: io_u.c 의 get_next_rand_offset_lfsr → [lfsr_next].
 * 호출 대상: __lfsr_next (일반 경로 또는 Latin-square 스킵 경로).
 * 실행 컨텍스트: 잡 스레드. 재진입 불안전(fl 상태 변경).
 *
 * fio_unlikely(fl->last_val > fl->max_val): 대부분의 경우 max_val 이하이며,
 * 초과는 드물게만 발생하므로 분기 예측을 not-taken 로 유도하여 pipeline 유지.
 */
int lfsr_next(struct fio_lfsr *fl, uint64_t *off)
{
	/* [한국어] 전체 순열 길이 도달 검사. num_vals 는 지금까지 생성된 값의 카운터 */
	if (fl->num_vals++ > fl->max_val)
		return 1;			/* [한국어] 1 반환 시 호출자는 잡의 "랜덤 맵 완료" 로 처리 */

	do {
		/* [한국어] Latin-square 스킵이 예약된 지점인지 확인 */
		if (fl->cycle_length && !--fl->cycle_length) {	/* [한국어] cycle 카운터를 1 감소, 0 에 도달하면 조건 성립 */
			__lfsr_next(fl, fl->spin + 1);		/* [한국어] 이번에만 +1 스텝 더 돌려 부분 순환을 회피 */
			fl->cycle_length = fl->cached_cycle_length;	/* [한국어] 다음 스킵까지의 간격을 리셋(미리 prepare_spin 이 계산) */
		} else
			__lfsr_next(fl, fl->spin);		/* [한국어] 일반 경로: spin+1 회 스텝 */
	} while (fio_unlikely(fl->last_val > fl->max_val));	/* [한국어] n-bit LFSR 이 max_val 초과 값을 만들면 즉시 다음 값까지 건너뜀 */

	*off = fl->last_val;			/* [한국어] 현재 레지스터 값을 오프셋으로 반환 */
	return 0;				/* [한국어] 성공 */
}

/*
 * [한국어] lfsr_create_xormask - 탭 위치 배열을 XOR 피드백 비트 마스크로 변환
 *
 * @taps: lfsr_taps[degree] 행 (0 으로 종료되는 비트 위치 배열)
 * @return: 각 탭 비트에 1 을 세운 64비트 마스크
 *
 * 예: {12, 6, 4, 1} → (1<<11)|(1<<5)|(1<<3)|(1<<0) = 0x82B.
 * 주의: 테이블 값은 1-based 비트 위치이므로 (taps[i] - 1) 로 0-based 변환.
 */
static uint64_t lfsr_create_xormask(uint8_t *taps)
{
	int i;
	uint64_t xormask = 0;			/* [한국어] 누적 OR 대상 */

	for(i = 0; i < FIO_MAX_TAPS && taps[i] != 0; i++)	/* [한국어] 0 이 나올 때까지(최대 FIO_MAX_TAPS) 순회 */
		xormask |= 1ULL << (taps[i] - 1);		/* [한국어] 1-based → 0-based 변환 후 해당 비트를 OR */

	return xormask;				/* [한국어] 완성된 피드백 마스크 반환 */
}

/*
 * [한국어] find_lfsr - 주어진 크기 size 를 커버하는 최소 degree 의 탭 배열 반환
 *
 * @size: 커버해야 하는 범위 (블록 수)
 * @return: lfsr_taps[degree] 행 포인터. 적절한 degree 가 없으면 NULL.
 *
 * 2^degree > size 를 만족하는 최소 degree 를 선택한다.
 * 왜 ">" 인가: n-bit LFSR 은 0..(2^n - 1) 중 금지 상태 1개를 뺀 "2^n - 1" 개
 * 유효 상태를 순회하므로, 최소한 size 보다 하나 더 많은 여유 공간이 필요하다.
 * i=3 부터 시작하는 이유: 3비트 미만은 주기가 너무 짧아 의미 없음.
 * i=64 초과는 uint64_t 비트 범위 초과.
 */
static uint8_t *find_lfsr(uint64_t size)
{
	int i;

	/*
	 * For an LFSR, there is always a prohibited state (all ones).
	 * Thus, if we need to find the proper LFSR for our size, we must
	 * take that into account.
	 */
	for (i = 3; i < 64; i++)			/* [한국어] 3-bit 부터 63-bit 까지 순회 */
		if ((1ULL << i) > size)			/* [한국어] 2^i > size 를 만족하는 최소 i 를 채택 */
			return lfsr_taps[i];		/* [한국어] 해당 비트 폭의 탭 배열 반환 */

	return NULL;					/* [한국어] size 가 지나치게 커서 64비트로도 불가한 경우 NULL */
}

/*
 * It is well-known that all maximal n-bit LFSRs will start repeating
 * themselves after their 2^n iteration. The introduction of spins however, is
 * possible to create a repetition of a sub-sequence before we hit that mark.
 * This happens if:
 *
 * [1]: ((2^n - 1) * i) % (spin + 1) == 0,
 * where "n" is LFSR's bits and "i" any number within the range [1,spin]
 *
 * It is important to know beforehand if a spin can cause a repetition of a
 * sub-sequence (cycle) and its length. However, calculating (2^n - 1) * i may
 * produce a buffer overflow for "n" close to 64, so we expand the above to:
 *
 * [2]: (2^n - 1) -> (x * (spin + 1) + y), where x >= 0 and 0 <= y <= spin
 *
 * Thus, [1] is equivalent to (y * i) % (spin + 1) == 0;
 * Also, the cycle's length will be (x * i) + (y * i) / (spin + 1)
 */
/*
 * [한국어] prepare_spin - spin 으로 인해 발생할 수 있는 부분 순환을 사전 분석
 *
 * @fl: LFSR 상태 (spin, cycle_length, cached_cycle_length 필드에 결과 저장)
 * @spin: 사용자가 요청한 spin 값 (0..15)
 * @return: 0 = 성공, 1 = spin>15 (현재 구현 상한 초과)
 *
 * 배경: LFSR 의 전체 주기는 2^n - 1 이지만, 매 호출 spin+1 번 스텝을 돌리면
 * 주기 nominal 값이 (2^n - 1) / gcd((2^n - 1), spin+1) 가 된다. 단순화하면
 * 주기 2^n - 1 이 "몇 개의 부분 순환(cycle)" 으로 분할될 수 있다.
 * prepare_spin 은 가장 작은 부분 순환 길이를 찾아 cycle_length 에 저장하고,
 * lfsr_next 가 그만큼 돌릴 때마다 한 번씩 스텝을 더 끼워넣어 순환을 탈출하도록
 * 한다. 이것이 "Latin-square 스킵" 의 원리.
 *
 * 오버플로 회피 트릭:
 *   (2^n - 1) * i 가 64비트를 넘을 수 있으므로,
 *   (2^n - 1) = x*(spin+1) + y  (x≥0, 0≤y≤spin) 로 분해해
 *   조건 "(2^n-1)*i % (spin+1) == 0" 을 "(y*i) % (spin+1) == 0" 로 환원.
 */
static int prepare_spin(struct fio_lfsr *fl, unsigned int spin)
{
	uint64_t max = (fl->cached_bit << 1) - 1;	/* [한국어] 전체 가능한 상태 수(2^n - 1). cached_bit 가 이미 1<<(n-1) 이므로 <<1 하면 2^n */
	uint64_t x, y;					/* [한국어] 오버플로 없이 mod 계산을 위한 분해 변수 */
	int i;

	if (spin > 15)					/* [한국어] __lfsr_next 의 switch 가 15까지만 커버 */
		return 1;				/* [한국어] 규약 위반 → 상위에서 에러로 전파 */

	x = max / (spin + 1);				/* [한국어] 몫 */
	y = max % (spin + 1);				/* [한국어] 나머지 (실제 조건 판정에 사용) */
	fl->cycle_length = 0;	/* No cycle occurs, other than the expected */	/* [한국어] 기본: 부분 순환 없음 */
	fl->spin = spin;				/* [한국어] spin 값을 상태에 저장 */

	for (i = 1; i <= spin; i++) {			/* [한국어] i=1..spin 중 가장 작은 해가 있는지 탐색 */
		if ((y * i) % (spin + 1) == 0) {	/* [한국어] 부분 순환 발생 조건 */
			fl->cycle_length = (x * i) + (y * i) / (spin + 1);	/* [한국어] 부분 순환 길이 (공식 유도는 위 영어 주석 참조) */
			break;				/* [한국어] 가장 작은 i 의 길이만 필요 */
		}
	}
	fl->cached_cycle_length = fl->cycle_length;	/* [한국어] lfsr_next 에서 소진 시 리필용으로 보관 */

	/*
	 * Increment cycle length for the first time only since the stored value
	 * will not be printed otherwise.
	 */
	fl->cycle_length++;				/* [한국어] 첫 방문까지의 카운트 다운이 맞도록 1 증가 (초기 off-by-one 보정) */

	return 0;					/* [한국어] 성공 */
}

/*
 * [한국어] lfsr_reset - LFSR 상태를 시드로 초기화 (탭/마스크/spin 은 보존)
 *
 * @fl: LFSR 상태
 * @seed: 초기 상태값 (비트마스크로 잘라서 사용)
 * @return: 0 = 성공, 1 = 시드가 금지 상태(all-ones)와 같음 (사용 불가)
 *
 * 호출 시점: lfsr_init 내부에서 마지막 단계로 호출. 잡 스레드 재시작 시 별도로
 * 호출되지는 않음(fio_lfsr 자체를 재생성).
 */
int lfsr_reset(struct fio_lfsr *fl, uint64_t seed)
{
	uint64_t bitmask = (fl->cached_bit << 1) - 1;	/* [한국어] n-bit 전체 마스크 = 2^n - 1 */

	fl->num_vals = 0;				/* [한국어] 생성 카운터 0 리셋 */
	fl->last_val = seed & bitmask;			/* [한국어] 시드를 n-bit 로 잘라서 저장 */

	/* All-ones state is illegal for XNOR LFSRs */
	if (fl->last_val == bitmask)			/* [한국어] XNOR 금지 상태 검사 */
		return 1;				/* [한국어] 호출자가 에러로 처리해야 함 */

	return 0;
}

/*
 * [한국어] lfsr_init - LFSR 생성기를 초기화
 *
 * @fl: LFSR 상태 구조체
 * @nums: 전체 블록 수 (생성 범위: 0 ~ nums-1)
 * @seed: 초기 시드 값
 * @spin: 한 출력당 실행할 추가 스텝 수 (0..15). 상관성 감소/난수 품질 향상 목적.
 * @return: 0 = 성공, 1 = 범위가 너무 커서 탭 없음 또는 spin>15 또는 시드 금지 상태
 *
 * 처리 순서:
 *   1. find_lfsr(nums): 적절한 degree 의 탭 배열 획득.
 *   2. max_val/xormask/cached_bit 세팅.
 *   3. prepare_spin: 부분 순환 길이 사전 계산.
 *   4. lfsr_reset: 시드 주입 및 금지 상태 검사.
 *
 * 호출 체인: init.c (init_random_map 등) → [lfsr_init] → find_lfsr/prepare_spin/lfsr_reset.
 * 실행 컨텍스트: 잡 초기화(메인 또는 잡 스레드 setup 단계).
 */
int lfsr_init(struct fio_lfsr *fl, uint64_t nums, uint64_t seed,
	      unsigned int spin)
{
	uint8_t *taps;				/* [한국어] lfsr_taps[degree] 행 포인터 */

	taps = find_lfsr(nums);			/* [한국어] nums 를 커버하는 최소 degree 선택 */
	if (!taps)				/* [한국어] 지원 범위(~2^63) 초과 */
		return 1;

	fl->max_val = nums - 1;			/* [한국어] 출력 범위 상한 (포함) */
	fl->xormask = lfsr_create_xormask(taps);	/* [한국어] 탭 배열 → XOR 마스크 변환 */
	fl->cached_bit = 1ULL << (taps[0] - 1);	/* [한국어] 최상위 비트 (__LFSR_NEXT 에서 시프트 후 OR 에 사용). taps[0] 는 degree 값 */

	if (prepare_spin(fl, spin))		/* [한국어] 부분 순환 사전 분석; 실패(1) 는 그대로 전파 */
		return 1;

	if (lfsr_reset(fl, seed))		/* [한국어] 시드 주입; 금지 상태면 1 반환 */
		return 1;

	return 0;				/* [한국어] 모든 단계 성공 */
}
