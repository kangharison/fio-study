/*
 * [한국어 설명] 가우시안(정규) 분포 난수 생성기 (gauss.c)
 *
 * === 파일의 역할 ===
 * fio 의 --random_distribution=normal:dev 옵션으로 활성화되는 정규분포 기반
 * 오프셋 선택기를 구현한다. 전체 가용 오프셋 범위 [0, nranges) 중에서 "중심 부근에
 * 접근이 집중되고 꼬리로 갈수록 확률이 감소하는" 종형 분포를 생성하여 실제 워크로드
 * (hot/warm/cold 영역이 있는 데이터베이스/캐시) 에 근접한 랜덤 I/O 패턴을 시뮬레이션
 * 한다. 정규분포 난수는 수학적으로 Box-Muller 나 Ziggurat 등이 있으나, 본 파일은
 * 중심극한정리(Central Limit Theorem, CLT) 에 기반한 간단한 구현을 사용한다:
 * GAUSS_ITERS(=12) 개의 독립 균등분포 난수의 합은 평균 nranges/2, 분산
 * nranges^2 / 12 인 분포에 수렴하며, 12 번 합산하면 표준편차 1 인 정규분포에 거의
 * 일치하는 것이 알려져 있다(경험적 특례). dev 옵션(백분율) 은 추가 편차를 부여해
 * 꼬리를 확장하고 중심 집중도를 사용자가 조절할 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * init.c 의 init_rand_distribution() 이 잡 시작 시 gauss_init() 를 호출하여 상태를
 * 준비하고, 런타임에는 io_u.c 의 __get_next_rand_offset_gauss() → gauss_next() 가
 * 매 I/O 마다 다음 오프셋 순위(0..nranges-1)를 반환한다. 반환된 순위는 블록 크기에
 * 곱해져 실제 파일 오프셋이 된다. __hash_u64 를 적용해 "연속된 CLT 결과가 공간적으로
 * 인접하지 않도록" 분산시키는 것이 핵심 트릭이다.
 *
 * === 타 모듈과의 연결 ===
 * - gauss.h:   struct gauss_state 정의와 4 API(init/next/disable_hash/dev) 프로토타입.
 * - ../hash.h: __hash_u64 — 생성 순위를 해시하여 공간 편향 제거. __rand/frand_state/
 *              init_rand_seed/FRAND32_MAX 균등분포 난수 인프라.
 * - io_u.c:    런타임 주 호출자(__get_next_rand_offset_gauss).
 * - init.c:    gauss_init 을 호출해 초기화.
 * - <math.h>:  ceil() — dev 백분율을 정수 편차 상한으로 환산.
 *
 * === 주요 함수/구조체 요약 ===
 * - gauss_init():           분포 상태 초기화(nranges, dev%→stddev, center 오프셋, seed).
 * - gauss_next():           다음 순위 반환(CLT 12 합산 → 평균 → dev 적용 → 해시 → mod).
 * - gauss_dev():            [-stddev/2, +stddev/2) 균등분포 편차 한 개 생성.
 * - gauss_disable_hash():   __hash_u64 적용 비활성화(테스트/비교 용도).
 * - struct gauss_state(gauss.h 에 정의):
 *     frand_state r;        내부 균등분포 PRNG 상태(Tausworthe).
 *     uint64_t nranges;     분포 범위(오프셋 개수).
 *     unsigned int stddev;  dev 백분율을 nranges 기준으로 환산한 정수 편차 상한.
 *     uint64_t rand_off;    center 오프셋(분포 중심 이동량).
 *     bool disable_hash;    해시 비활성 플래그(기본 false).
 */

#include <math.h>               /* [한국어] ceil() — dev% × nranges / 100 의 올림 */
#include <string.h>             /* [한국어] memset — 구조체 0 초기화 */
#include "../hash.h"            /* [한국어] __hash_u64 공간 분산 해시, __rand/frand_state/FRAND32_MAX/init_rand_seed */
#include "gauss.h"              /* [한국어] struct gauss_state 정의와 본 파일의 공개 API 프로토타입 */

/* [한국어] CLT 합산 횟수. 12 는 경험적으로 uniform 의 합을 정규분포로 근사시키는
 * "마법의 수" — sum of 12 uniforms 는 평균 6, 분산 1 로 표준정규 N(0,1) 에 가까움.
 * 단, 꼬리 영역(|z|>3) 는 실제 정규분포보다 얇음(bounded) — dev 가 이 한계를 넓힌다 */
#define GAUSS_ITERS	12

/*
 * [한국어]
 * gauss_dev - 표준편차 범위 [-stddev/2, +stddev/2) 의 균등 난수를 한 개 생성.
 *
 * @gs:     가우시안 상태(난수 스트림 r 과 stddev 필드 사용).
 * @return: int 편차값. stddev==0 이면 항상 0(CLT 순수 사용).
 *
 * 용도: gauss_next 가 계산한 CLT 기반 중심값에 이 편차를 더해 꼬리를 확장한다.
 * stddev 가 nranges 의 특정 비율(dev 옵션으로 지정)일 때, 분포의 스프레드를
 * 사용자가 조절할 수 있다.
 *
 * 동작 단계:
 *   1) stddev==0 즉시 0 반환.
 *   2) __rand 로 32비트 균등 난수 r 획득.
 *   3) stddev * (r/(FRAND32_MAX+1.0)) 로 [0, stddev) 부동소수점 스케일.
 *   4) stddev/2 빼서 [-stddev/2, +stddev/2) 으로 시프트.
 *
 * 실행 컨텍스트: 잡 스레드. __rand 가 각 잡별 rand_state 를 쓰므로 MT 안전.
 *
 * 호출 체인: gauss_next → [gauss_dev] → __rand.
 */
static int gauss_dev(struct gauss_state *gs)
{
	/* [한국어] 균등 난수 원본 값(0..FRAND32_MAX) */
	unsigned int r;
	/* [한국어] 스케일링된 편차(정수) */
	int vr;

	/* [한국어] stddev 미설정(옵션 기본 0) 이면 편차 0 — 순수 CLT 만 사용 */
	if (!gs->stddev)
		return 0;

	/* [한국어] 내부 Tausworthe PRNG 에서 32비트 균등 난수 획득 */
	r = __rand(&gs->r);
	/* [한국어] [0, stddev) 스케일. FRAND32_MAX+1 분모로 [0,1) 개방구간을 정확히 보정 */
	vr = gs->stddev * (r / (FRAND32_MAX + 1.0));

	/* [한국어] 원점을 중앙으로 이동시켜 양/음 편차 모두 생성 */
	return vr - gs->stddev / 2;
}

/*
 * [한국어]
 * gauss_next - 가우시안 분포를 따르는 다음 오프셋 순위(0..nranges-1) 를 생성.
 *
 * @gs:     초기화된 가우시안 상태.
 * @return: 다음 오프셋 순위(unsigned long long) — io_u.c 가 블록 크기에 곱해 파일 오프셋으로 사용.
 *
 * 동작 단계:
 *   1) CLT 12 합: __rand 12 번의 결과를 nranges+1 로 모듈로한 합 sum.
 *      각 항은 [0, nranges] 균등분포 → 합 평균 = 6·(nranges/2) ≈ 6·nranges.
 *   2) sum / GAUSS_ITERS (올림 방식으로 (sum + ITERS-1) / ITERS) 로 평균을 구해
 *      [0, nranges) 범위로 정규화. 여기까지가 "종형" 분포 중심값.
 *   3) stddev>0 이면 gauss_dev 로 얻은 편차를 더함. 경계 초과 시 편차를 반복적으로
 *      반감하여 범위 내로 클램프 — "out of range" 샘플을 버리지 않고 줄임으로써
 *      꼬리 확률을 유지.
 *   4) disable_hash 이 아니면 __hash_u64 로 순위를 해시하여 "인접 난수가 오프셋
 *      공간에서도 인접" 한 결과를 방지(공간적 편향 제거).
 *   5) rand_off(center) 를 더하고 nranges 로 모듈로하여 결과 반환.
 *
 * 실행 컨텍스트: 잡 스레드 내(io_u.c 에서 매 I/O 마다 호출). 상태는 per-job 이라 락 불필요.
 *
 * 호출 체인: io_u.c(get_next_rand_offset → __get_next_rand_offset_gauss) → [gauss_next]
 *           → __rand / gauss_dev / __hash_u64.
 */
unsigned long long gauss_next(struct gauss_state *gs)
{
	/* [한국어] CLT 합산 누적기 */
	unsigned long long sum = 0;
	int i;

	/* [한국어] CLT: 12 개 균등 난수 [0..nranges] 의 합 — 이 합은 정규분포에 근사 수렴 */
	for (i = 0; i < GAUSS_ITERS; i++)
		sum += __rand(&gs->r) % (gs->nranges + 1);

	/* [한국어] 평균화(올림 방식) — 평균을 취하면 분포가 [0, nranges) 로 정규화됨 */
	sum = (sum + GAUSS_ITERS - 1) / GAUSS_ITERS;

	/* [한국어] 사용자 dev 설정 시 편차 추가 */
	if (gs->stddev) {
		/* [한국어] [-stddev/2, +stddev/2) 추가 편차 */
		int dev = gauss_dev(gs);

		/* [한국어] dev 가 너무 커서 sum+dev 가 경계를 넘으면 반복적으로 반으로 줄여 클램프.
		 * 이 방식은 꼬리 샘플을 완전히 버리지 않고 축소하여 분포 왜곡 최소화 */
		while (dev + sum >= gs->nranges)
			dev /= 2;
		sum += dev;
	}

	/* [한국어] 공간 편향 제거 해시 — CLT 가 연속된 중심값을 잘 주지만 실제 I/O 는
	 * 공간적으로 분산된 오프셋을 원하므로 해시로 섞는다 */
	if (!gs->disable_hash)
		sum = __hash_u64(sum);

	/* [한국어] center 오프셋을 더하고 모듈로로 범위 재정렬 → 최종 순위 반환 */
	return (sum + gs->rand_off) % gs->nranges;
}

/*
 * [한국어]
 * gauss_init - 가우시안 분포 상태 초기화.
 *
 * @gs:      초기화할 gauss_state(호출자 소유, stack/heap 무관).
 * @nranges: 전체 오프셋 범위(블록 개수).
 * @dev:     표준편차 백분율. 0 이면 순수 CLT. 100 이면 nranges/2 로 상한 클램프.
 * @center:  분포 중심(0.0~1.0 비율). -1 이면 기본(분포가 nranges 의 중간).
 * @seed:    PRNG 시드.
 *
 * 동작 단계:
 *   1) 구조체 0 초기화(disable_hash=false, stddev=0, 등).
 *   2) init_rand_seed 로 내부 Tausworthe PRNG 초기화(r 필드).
 *   3) nranges 저장.
 *   4) dev 가 0 이 아니면 stddev = ceil(nranges * dev / 100). 이 값이 nranges/2 를 넘으면
 *      분포가 실질적으로 의미를 잃으므로 상한 클램프.
 *   5) center == -1 이면 rand_off=0 (분포가 기본 중심), 그 외엔 rand_off = nranges*(center-0.5)
 *      — center=0.5 는 중앙, 0.0 은 앞쪽, 1.0 은 뒤쪽으로 이동.
 *
 * 실행 컨텍스트: init.c(init_rand_distribution) — 잡 시작 전, 단일 스레드.
 *
 * 호출 체인: init.c → [gauss_init] → init_rand_seed / memset / ceil.
 *
 * 에러 처리: 별도 반환값 없음. 입력이 비정상이면 잘못된 분포가 생성될 뿐 — 호출자가
 *           CLI 파싱에서 입력 유효성 보장 책임.
 */
void gauss_init(struct gauss_state *gs, unsigned long nranges, double dev,
		double center, unsigned int seed)
{
	/* [한국어] 모든 필드 0 초기화 — disable_hash/stddev/rand_off 등 기본값 보장 */
	memset(gs, 0, sizeof(*gs));
	/* [한국어] 내부 Tausworthe PRNG 시드. 두 번째 인자 0 은 use_random_generator=false */
	init_rand_seed(&gs->r, seed, 0);
	/* [한국어] 분포 범위 저장(이후 모듈로 연산에 사용) */
	gs->nranges = nranges;

	/* [한국어] dev!=0 이면 사용자 설정 기반 stddev 계산 */
	if (dev != 0.0) {
		/* [한국어] dev% 를 nranges 스케일로 환산. ceil 로 최소 1 이상 보장 */
		gs->stddev = ceil((double)(nranges * dev) / 100.0);
		/* [한국어] stddev 가 nranges/2 를 초과하면 분포가 사실상 균등이 됨 — 의미 상한 클램프 */
		if (gs->stddev > nranges / 2)
			gs->stddev = nranges / 2;
	}
	/* [한국어] center 입력 해석: -1=기본, 그 외는 (center-0.5)*nranges 오프셋 이동.
	 * center=0.5 → 0(중앙), center=0.0 → -nranges/2, center=1.0 → +nranges/2 */
	if (center == -1)
	  gs->rand_off = 0;
	else
	  gs->rand_off = nranges * (center - 0.5);
}

/* [한국어]
 * gauss_disable_hash - __hash_u64 공간 분산 해시 적용을 끈다.
 *
 * @gs: 가우시안 상태.
 *
 * 테스트/디버그 용도 — 해시 없이 연속된 CLT 결과의 원본 분포를 확인할 때 사용.
 * 기본값은 disable_hash=false(해시 활성).
 *
 * 호출 체인: (옵션/디버그 경로) → [gauss_disable_hash].
 */
void gauss_disable_hash(struct gauss_state *gs)
{
	/* [한국어] 플래그만 세팅. 다음 gauss_next 호출부터 해시 스킵 */
	gs->disable_hash = true;
}
