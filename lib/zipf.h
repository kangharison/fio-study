/*
 * [한국어 설명] Zipf/Pareto 분포 I/O 오프셋 생성기 공개 헤더 (zipf.h)
 *
 * === 파일의 역할 ===
 * lib/zipf.c 가 구현하는 두 종류의 편중 분포 생성기(Zipf, Pareto) 의 상태
 * 구조체(`struct zipf_state`) 와 각 분포별 init/next API, 해시 비활성 API 를
 * 노출한다. 두 분포는 상태 필드가 상당히 겹치므로 구조체를 공유한다(메모리
 * 절약 + init 분기 단순화). Zipf 는 Hsu&Sinclair 1995 의 "Rejection-Inversion
 * to Generate Variates from Monotone Discrete Distributions" 기법을 쓰고,
 * Pareto 는 standard 역변환 샘플링 X = (1-U)^(-1/α) 에서 파생된 pareto_pow
 * = log(h)/log(1-h) 를 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 I/O 오프셋 결정 경로. `--random_distribution=zipf:<theta>` 또는
 * `pareto:<h>` 로 활성화된다. 캐시 워크로드(핫 블록 일부에 80/20 규칙 같은
 * 편중) 시뮬레이션에 흔히 사용.
 * 호출 체인:
 *   io_u.c get_io_u → get_next_rand_offset → get_next_rand_block
 *     → zipf_next(&td->zipf_state) or pareto_next(&td->zipf_state)
 *   init.c : zipf_init(zs, nranges, theta, center, seed) / pareto_init(...).
 *
 * === 타 모듈과의 연결 ===
 * - zipf.c : 구현. zeta(N, theta) 사전 계산(ZIPF_MAX_GEN=10M 상한으로 실행
 *   시간 제한), __hash_u64 로 공간 분산.
 * - lib/rand.h : frand_state 및 __rand_0_1.
 * - lib/types.h : bool.
 * - io_u.c : 사용자 측.
 * 데이터 흐름: theta/h + nranges → 사전 계산 zeta / pareto_pow → 루프당
 * __rand_0_1 호출 → 역변환 샘플링 → rand_off 더하기 → hash 분산 → 오프셋.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct zipf_state : 두 분포 공유 상태. Zipf 는 theta/zeta2/zetan 을,
 *   Pareto 는 pareto_pow 를 사용. 서로 쓰지 않는 필드는 비어 있거나 0.
 * - zipf_init / zipf_next : Zipf 전용 초기화/샘플.
 * - pareto_init / pareto_next : Pareto 전용.
 * - zipf_disable_hash : 테스트용 해시 믹싱 비활성화.
 */

#ifndef FIO_ZIPF_H
#define FIO_ZIPF_H
/* [한국어] 헤더 가드. io_u.c / init.c 등에서 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t. nranges/반환 오프셋의 고정폭 타입. */

#include "rand.h"
/* [한국어] "rand.h" : struct frand_state 완전 정의. zipf_state 가 값으로 임베드. */

#include "types.h"
/* [한국어] "types.h" : bool 타입. disable_hash 필드 타입. */

struct zipf_state {
	uint64_t nranges;
	/* [한국어] 전체 블록 범위(오프셋 후보 수). 반환 오프셋 < nranges.
	 * 설정자: {zipf,pareto}_init. 읽는 자: {zipf,pareto}_next. */

	double theta;
	/* [한국어] Zipf 매개변수 (>1: 상위 블록에 급격 편중, =1: 경계 케이스,
	 * <1: 약한 편중). theta=1.0 은 표준 Zipf("1/k" law).
	 * 설정자: zipf_init. Pareto 모드에서는 사용하지 않음. */

	double zeta2;
	/* [한국어] H(2, theta) = 1 + 0.5^theta. Zipf 역변환 공식에서 분모/분자의
	 * 간단 형태에 쓰이는 사전 계산 상수.
	 * 설정자: zipf_init. 읽는 자: zipf_next. */

	double zetan;
	/* [한국어] H(nranges, theta) = Σ_{i=1..nranges} 1/i^theta. 정규화 상수.
	 * nranges 가 크면 계산 비용이 크기 때문에 zipf.c 가 ZIPF_MAX_GEN 한도로
	 * 계산하고 동일 (nranges, theta) 조합이 반복되면 shared_rand_init 로
	 * 캐시 공유(같은 잡 집합에서 중복 계산 방지). */

	double pareto_pow;
	/* [한국어] Pareto 지수 = log(h)/log(1-h). 입력 h 는 "상위 h% 가 전체의
	 * (1-h)% 를 차지" 라는 시뮬레이션 매개변수. 역변환식 X = nranges *
	 * (1-U)^pareto_pow 에 곱해진다.
	 * 설정자: pareto_init. Zipf 모드에서는 사용하지 않음. */

	struct frand_state rand;
	/* [한국어] 이 분포 전용 Tausworthe PRNG 상태.
	 * 설정자: init 이 init_rand_seed 로 시드. 읽는 자: next 가 __rand_0_1 호출.
	 * 동기화: 잡 스레드 단일 소유. */

	uint64_t rand_off;
	/* [한국어] 분포의 중심 오프셋 — 샘플 결과에 더하여 분포를 회전/이동.
	 * center=0.0 이면 0, center=0.5 이면 nranges/2.
	 * 설정자: init. 읽는 자: next. */

	bool disable_hash;
	/* [한국어] true 이면 공간 해시 믹싱(__hash_u64) 을 건너뜀.
	 * 분포 형상 검증(테스트) 용. 일반 실행에서는 false.
	 * 설정자: zipf_disable_hash. 읽는 자: next. */
};

void zipf_init(struct zipf_state *zs, uint64_t nranges, double theta,
	       double center, unsigned int seed);
/* [한국어] zipf_init - Zipf 분포 생성기를 초기화.
 *
 * @zs: 호출자 소유 상태. @nranges: 범위. @theta: 편중 파라미터.
 * @center: 중심 비율(0..1) — rand_off 계산에 사용. @seed: PRNG 시드.
 *
 * 내부에서 zeta(nranges, theta) 를 사전 계산하며, nranges 가 ZIPF_MAX_GEN
 * (10M) 을 초과하면 zeta(10M, theta) 로 제한하여 계산 시간 제어. 같은
 * 매개변수가 이미 있다면 shared_rand_init 가 재사용. */

uint64_t zipf_next(struct zipf_state *zs);
/* [한국어] zipf_next - Zipf 분포를 따르는 다음 오프셋 반환. tight loop.
 * 반환: [0, nranges) 의 uint64_t. 상위 랭크가 높은 빈도로 선택됨. */

void pareto_init(struct zipf_state *zs, uint64_t nranges, double h,
		 double center, unsigned int seed);
/* [한국어] pareto_init - Pareto 분포 생성기 초기화.
 *
 * @h: 파레토 편중 파라미터(0 < h < 1). 예: h=0.2 → 상위 20% 가 80% 를 차지
 *    하는 고전적 80/20 모델에 가까움.
 *
 * 내부에서 pareto_pow = log(h)/log(1-h) 를 사전 계산. */

uint64_t pareto_next(struct zipf_state *zs);
/* [한국어] pareto_next - Pareto 분포 샘플. 반환 [0, nranges). */

void zipf_disable_hash(struct zipf_state *zs);
/* [한국어] zipf_disable_hash - Zipf/Pareto 모두에서 해시 믹싱을 끈다.
 * 공유 API(두 분포 공용). 테스트 전용. */

#endif
/* [한국어] FIO_ZIPF_H 가드 종료. */
