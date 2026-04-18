/*
 * [한국어 설명] Zipf 및 Pareto 분포 난수 생성기 (zipf.c)
 *
 * === 파일의 역할 ===
 * fio 의 --random_distribution=zipf:theta 또는 pareto:h 옵션으로 활성화되는 비균등
 * 분포 기반 오프셋 선택기를 구현한다. 두 분포 모두 "핫 영역에 접근이 집중되고 꼬리는
 * 얇게" 분포하는 멱법칙(power-law) 계열로, 데이터베이스 액세스/웹 요청/CDN 캐시 등
 * 실세계 워크로드 시뮬레이션에 적합하다. 내부적으로는 하나의 struct zipf_state 를
 * 공유하여 두 분포를 모두 지원한다.
 *
 * Zipf 분포 구현:
 *   zipf_update() 가 정규화 상수 zeta(N, theta) = sum_{i=1..N}(1/i^theta) 를 사전
 *   계산한다(최대 10M 항까지). zipf_next() 는 Jim Gray 등이 제안한 효율적 역변환
 *   샘플링(Hsu 1995, "Quickly Generating Billion-Record Synthetic Databases")의 변형
 *   을 사용한다 — 균등 난수 U ~ [0,1) 로 zeta 스케일의 Z 값을 만들고, 구간별로
 *   순위를 결정한 뒤 연속 근사 공식으로 중간 순위를 매핑한다. theta 가 클수록 상위
 *   소수 블록에 접근이 더 집중된다(theta=0 은 균등, theta → ∞ 는 한 점 집중).
 *
 * Pareto 분포 구현:
 *   pareto_init() 가 pareto_pow = log(h) / log(1-h) 를 사전 계산.
 *   pareto_next() 는 uniform^pareto_pow 공식으로 [0, nranges-1] 범위 값을 생성.
 *   h 가 0 에 가까우면 균등, 1 에 가까우면 극단 편중(80/20 법칙 근처).
 *
 * 두 분포 모두 __hash_u64 로 결과 순위를 해시하여 공간 편향을 제거한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * init.c 의 init_rand_distribution() 에서 잡 시작 시 zipf_init 또는 pareto_init 를
 * 호출해 상태를 준비하고, 런타임에는 io_u.c 의 __get_next_rand_offset_{zipf,pareto}()
 * 가 매 I/O 마다 다음 순위를 얻는다. 각 잡별 독립 상태이므로 MT 동기화 불필요.
 *
 * === 타 모듈과의 연결 ===
 * - zipf.h:      struct zipf_state 정의(공유) 및 API 6 개 프로토타입.
 * - ../minmax.h: min() 매크로 — ZIPF_MAX_GEN 과 nranges 중 작은 값 선택.
 * - ../hash.h:   __hash_u64, __rand, init_rand_seed, FRAND32_MAX — 균등 난수/해시 인프라.
 * - <math.h>:    pow, log — 확률 계산.
 * - init.c/io_u.c: 주 호출자(초기화/런타임).
 *
 * === 주요 함수/구조체 요약 ===
 * - zipf_init(zs, nranges, theta, center, seed):  Zipf 초기화(zeta2, zetan 사전 계산).
 * - zipf_next(zs):                                 Zipf 순위 반환(역변환 샘플링).
 * - pareto_init(zs, nranges, h, center, seed):     Pareto 초기화(pareto_pow 사전 계산).
 * - pareto_next(zs):                               Pareto 순위 반환(uniform^pareto_pow).
 * - zipf_disable_hash(zs):                          __hash_u64 공간 분산 비활성(테스트용).
 * - shared_rand_init (static):                      두 분포 공용 PRNG + center 오프셋 초기화.
 * - zipf_update (static):                           zetan 계산 보조.
 * struct zipf_state(zipf.h):
 *     frand_state rand;    내부 Tausworthe PRNG 상태.
 *     uint64_t nranges;    범위.
 *     double theta;        Zipf 매개변수.
 *     double zeta2, zetan; zeta 함수 사전계산 값(Zipf 전용).
 *     double pareto_pow;   Pareto 지수(Pareto 전용).
 *     uint64_t rand_off;   center 오프셋.
 *     bool disable_hash;   해시 비활성 플래그.
 */

#include <math.h>               /* [한국어] pow(지수), log(자연로그) — 확률 계산 */
#include <string.h>             /* [한국어] memset — 구조체 0 초기화 */
#include "zipf.h"               /* [한국어] struct zipf_state 정의와 API 프로토타입 */
#include "../minmax.h"           /* [한국어] min() 매크로 — 성능 상한 클램프 */
#include "../hash.h"            /* [한국어] __hash_u64 공간 분산, __rand/frand_state/FRAND32_MAX/init_rand_seed */

/* [한국어] zeta 합의 최대 반복 횟수. theta 가 작을 때는 수렴이 느려 N 이 클 수록 연산량이 많아지므로,
 * 정밀도 약간 희생하더라도 10M 항으로 clamp(대부분 머신에서 1~2초 내 계산 완료).
 * 큰 nranges 에서도 분포 특성은 사실상 유지됨 */
#define ZIPF_MAX_GEN	10000000UL

/*
 * [한국어]
 * zipf_update - Zipf 정규화 상수 zeta(N, theta) 사전 계산.
 *
 * @zs: 초기화 중인 zipf_state. zetan 필드가 갱신 대상.
 *
 * 수학적 배경:
 *   zeta(N, theta) = sum_{i=1..N} 1/i^theta.  Zipf 의 PMF 정규화 분모로 사용된다.
 *   P(rank = k) = (1/k^theta) / zeta(N, theta).
 *
 * 구현 세부:
 *   - theta 가 작을수록(1 에 가까울수록) zeta 는 발산에 가까워 계산량이 증가.
 *   - 계산 시간이 과도하지 않도록 to_gen = min(nranges, ZIPF_MAX_GEN) 로 상한.
 *   - double 누적이라 수치 오차가 있으나 분포 특성은 충분히 보존된다.
 *
 * 실행 컨텍스트: 초기화 단계(init.c). 1 회성 — 런타임 경로 아님.
 * 호출 체인: zipf_init → [zipf_update] → pow.
 */
static void zipf_update(struct zipf_state *zs)
{
	/* [한국어] 실제 합산할 항 수(상한 10M) */
	uint64_t to_gen;
	unsigned int i;

	/*
	 * It can become very costly to generate long sequences. Just cap it at
	 * 10M max, that should be doable in 1-2s on even slow machines.
	 * Precision will take a slight hit, but nothing major.
	 */
	/* [한국어] nranges 와 ZIPF_MAX_GEN 중 작은 쪽으로 제한 — 계산 시간 상한 보장 */
	to_gen = min(zs->nranges, (uint64_t) ZIPF_MAX_GEN);

	/* [한국어] zeta 누적 합 — 각 i 에 대해 1/i^theta 를 더함. i+1 은 1-based 순위 */
	for (i = 0; i < to_gen; i++)
		zs->zetan += pow(1.0 / (double) (i + 1), zs->theta);
}

/*
 * [한국어]
 * shared_rand_init - Zipf 와 Pareto 공용 초기화 루틴.
 *
 * @zs:      분포 상태.
 * @nranges: 오프셋 범위.
 * @center:  분포 중심 비율(0.0~1.0). -1 이면 랜덤 오프셋.
 * @seed:    PRNG 시드.
 *
 * 동작: 구조체 0 초기화 → nranges 기록 → PRNG 시드 → rand_off 설정.
 * rand_off 는 center=-1 인 경우 랜덤 시작점으로, 아니면 nranges*center 값으로 고정.
 */
static void shared_rand_init(struct zipf_state *zs, uint64_t nranges,
			     double center, unsigned int seed)
{
	/* [한국어] 구조체 전체 0 초기화 — disable_hash/zetan/pareto_pow 등 기본값 */
	memset(zs, 0, sizeof(*zs));
	/* [한국어] 오프셋 범위 저장 */
	zs->nranges = nranges;

	/* [한국어] 내부 Tausworthe PRNG 시드. 두 번째 인자 0 = use_random_generator=false */
	init_rand_seed(&zs->rand, seed, 0);
	/* [한국어] 초기 랜덤 오프셋 — center=-1 일 때 의미 있는 시작점 */
	zs->rand_off = __rand(&zs->rand);
	/* [한국어] center 지정 시 해당 위치로 분포 중심 이동 */
	if (center != -1)
		zs->rand_off = nranges * center;
}

/*
 * [한국어]
 * zipf_init - Zipf 분포 상태 초기화.
 *
 * @zs:      초기화 대상.
 * @nranges: 전체 블록/오프셋 수.
 * @theta:   Zipf 매개변수. theta>1 → 상위 소수 블록 집중, theta=0 → 균등.
 * @center:  분포 중심 비율(-1 = 랜덤).
 * @seed:    PRNG 시드.
 *
 * 동작: shared_rand_init → theta 저장 → zeta2(=1 + 0.5^theta, 상위 2 항의 합) 사전 계산
 *       → zipf_update 로 zetan 전체 합 계산.
 *
 * 호출 체인: init.c(init_rand_distribution) → [zipf_init] → shared_rand_init / zipf_update.
 */
void zipf_init(struct zipf_state *zs, uint64_t nranges, double theta,
	       double center, unsigned int seed)
{
	/* [한국어] PRNG + center 오프셋 초기화 */
	shared_rand_init(zs, nranges, center, seed);

	/* [한국어] Zipf 매개변수 저장 */
	zs->theta = theta;
	/* [한국어] zeta2 = 1^(-theta) + 2^(-theta). zipf_next 의 구간 판정에 사용 */
	zs->zeta2 = pow(1.0, zs->theta) + pow(0.5, zs->theta);

	/* [한국어] zetan 전체 합 사전 계산(최대 10M 항) */
	zipf_update(zs);
}

/*
 * [한국어]
 * zipf_next - Zipf 분포를 따르는 다음 오프셋 순위 반환.
 *
 * @zs:     초기화된 zipf_state.
 * @return: [0, nranges-1] 범위의 순위. io_u.c 가 블록 크기에 곱해 실제 오프셋으로 변환.
 *
 * 알고리즘(Jim Gray 등의 역변환 샘플링 변형, Hsu 1995 에 기반):
 *   alpha = 1 / (1 - theta)
 *   eta   = (1 - (2/N)^(1-theta)) / (1 - zeta(2)/zeta(N))
 *   U     = uniform [0, 1)
 *   Z     = U * zetan
 *
 *   Z < 1                           → rank = 1
 *   1 <= Z < 1 + 0.5^theta          → rank = 2
 *   Z 그 이상                        → rank = 1 + floor(N * (eta*U - eta + 1)^alpha)
 *
 *   → rank-1 → 0-based 인덱스 → 해시 → center 오프셋 합 → mod N.
 *
 * 동작 단계:
 *   1) alpha, eta 계산(매 호출마다 재계산 — 상태에 캐시하지 않음. 경량 연산이라 허용).
 *   2) 균등 난수 U 와 스케일된 Z 생성.
 *   3) Z 값 구간별로 순위 결정.
 *   4) 0-base 로 변환 후 해시 적용(공간 분산).
 *   5) center 오프셋 더해 모듈로 환산.
 *
 * 실행 컨텍스트: 잡 스레드(io_u.c). 각 잡 독립 상태.
 *
 * 호출 체인: io_u.c(__get_next_rand_offset_zipf) → [zipf_next] → pow/__rand/__hash_u64.
 */
uint64_t zipf_next(struct zipf_state *zs)
{
	/* [한국어] alpha: 역변환 공식의 지수. eta: 연속 근사 보정 계수. */
	double alpha, eta, rand_uni, rand_z;
	/* [한국어] 로컬 복사 — 가독성 향상 */
	unsigned long long n = zs->nranges;
	/* [한국어] 최종 순위 결과(1-based, 아래에서 --해 0-based 로 변환) */
	unsigned long long val;

	/* [한국어] 역변환 공식의 두 상수: alpha=1/(1-theta), eta 는 상위 2 항 비율 보정 */
	alpha = 1.0 / (1.0 - zs->theta);
	eta = (1.0 - pow(2.0 / n, 1.0 - zs->theta)) / (1.0 - zs->zeta2 / zs->zetan);

	/* [한국어] 균등 난수 [0,1) 생성 후 zetan 으로 스케일 → Zipf CDF 도메인의 Z */
	rand_uni = (double) __rand(&zs->rand) / (double) FRAND32_MAX;
	rand_z = rand_uni * zs->zetan;

	/* [한국어] 구간 판정: 상위 순위 확률 1 은 Z<1 영역, 순위 2 는 1..1+0.5^theta 영역.
	 * 그 외는 연속 근사로 중간 순위 매핑 */
	if (rand_z < 1.0)
		val = 1;
	else if (rand_z < (1.0 + pow(0.5, zs->theta)))
		val = 2;
	else
		val = 1 + (unsigned long long)(n * pow(eta*rand_uni - eta + 1.0, alpha));

	/* [한국어] 1-based → 0-based 인덱스 변환 */
	val--;

	/* [한국어] 공간 분산 해시 — 연속 순위가 인접 오프셋이 되지 않도록 */
	if (!zs->disable_hash)
		val = __hash_u64(val);

	/* [한국어] center 오프셋 합 후 nranges 모듈로로 범위 재정렬 */
	return (val + zs->rand_off) % zs->nranges;
}

/*
 * [한국어]
 * pareto_init - Pareto 분포 상태 초기화.
 *
 * @zs:      초기화 대상.
 * @nranges: 전체 범위.
 * @h:       Pareto 매개변수(0<h<1). 0 에 가까울수록 균등, 1 에 가까울수록 극단 편중.
 *           예: h=0.2 → 대략 80/20 법칙(상위 20% 가 80% 접근).
 * @center:  분포 중심(-1=랜덤).
 * @seed:    PRNG 시드.
 *
 * 사전 계산: pareto_pow = log(h) / log(1-h). pareto_next 의 지수로 사용.
 *
 * 호출 체인: init.c → [pareto_init] → shared_rand_init / log.
 */
void pareto_init(struct zipf_state *zs, uint64_t nranges, double h,
		 double center, unsigned int seed)
{
	/* [한국어] 공용 초기화 — PRNG + center 오프셋 */
	shared_rand_init(zs, nranges, center, seed);
	/* [한국어] 멱법칙 지수 사전 계산 — log(h)/log(1-h). h ∈ (0,1) 일 때 음수(편중) */
	zs->pareto_pow = log(h) / log(1.0 - h);
}

/*
 * [한국어]
 * pareto_next - Pareto 분포를 따르는 다음 오프셋 순위 반환.
 *
 * @zs:     초기화된 pareto 상태.
 * @return: [0, nranges-1] 범위의 순위.
 *
 * 알고리즘: n = (nranges-1) * U^pareto_pow.
 *   - U ~ uniform [0,1). pareto_pow 가 음수이면 U 가 0 에 가까울 때 거대한 값 → 상위 편중.
 *   - (nranges-1) 로 스케일 → [0, nranges) 범위.
 *   - 해시로 공간 분산 → center 오프셋 → mod.
 *
 * 호출 체인: io_u.c(__get_next_rand_offset_pareto) → [pareto_next] → pow/__rand/__hash_u64.
 */
uint64_t pareto_next(struct zipf_state *zs)
{
	/* [한국어] 균등 난수 [0, 1) */
	double rand = (double) __rand(&zs->rand) / (double) FRAND32_MAX;
	/* [한국어] 결과 순위(0-based) */
	unsigned long long n;

	/* [한국어] 멱법칙 샘플링: (N-1) * U^pareto_pow. pareto_pow<0 이면 극단 편중 */
	n = (zs->nranges - 1) * pow(rand, zs->pareto_pow);

	/* [한국어] 공간 분산 해시 */
	if (!zs->disable_hash)
		n = __hash_u64(n);

	/* [한국어] center 오프셋 + 모듈로로 최종 오프셋 결정 */
	return (n + zs->rand_off)  % zs->nranges;
}

/*
 * [한국어]
 * zipf_disable_hash - __hash_u64 공간 분산 해시 적용을 끈다(테스트/디버그용).
 *
 * @zs: 분포 상태. zipf/pareto 둘 다 공용 상태이므로 이 호출은 양쪽에 동일 영향.
 *
 * 호출 체인: (옵션/디버그) → [zipf_disable_hash].
 */
void zipf_disable_hash(struct zipf_state *zs)
{
	/* [한국어] 플래그 세팅 — 다음 *_next 호출부터 해시 스킵 */
	zs->disable_hash = true;
}
