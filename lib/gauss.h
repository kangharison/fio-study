/*
 * [한국어 설명] 가우시안(정규) 분포 I/O 오프셋 생성기 공개 헤더 (gauss.h)
 *
 * === 파일의 역할 ===
 * lib/gauss.c 가 구현하는 "정규 분포를 따르는 블록 오프셋 생성기" 의 상태
 * 구조체(`struct gauss_state`) 와 세 공개 API(`gauss_init`, `gauss_next`,
 * `gauss_disable_hash`) 를 노출한다. fio 의 `--random_distribution=normal`
 * 옵션이 설정되면 io_u.c 가 잡별로 이 구조체를 준비해 오프셋을 고른다.
 * 구현은 Central Limit Theorem (CLT) 근사로, U(0,1) 균등 난수 12 개를 합하여
 * -6..+6 의 표준 정규 유사 난수를 얻는 고전적 방법을 사용한다(dev% 옵션이
 * 표준편차를 결정). 성능상 Box-Muller 대신 CLT 를 쓰는 이유는 log/sqrt 를
 * 쓰지 않아 O(1) 고정 시간이고 SIMD 친화적이기 때문.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 I/O 오프셋 결정 경로의 한 종류(다른 종류: zipf, pareto, uniform, lfsr,
 * sprandom). `--random_distribution=normal:<dev%>:<center>` 로 튜닝 가능.
 * 호출 체인:
 *   io_u.c get_io_u → get_next_rand_offset → get_next_rand_block
 *     → gauss_next(&td->gauss_state)
 *   init.c / filesetup.c : 잡 init 시 gauss_init(gs, nranges, dev, center, seed).
 *
 * === 타 모듈과의 연결 ===
 * - gauss.c : 구현. CLT 12 합, hash mix(__hash_u64) 로 공간 분산.
 * - lib/rand.h : struct frand_state 와 __rand_0_1 제공. gauss_state 가 내부
 *   frand_state 를 소유.
 * - io_u.c : 사용자 측.
 * - <inttypes.h> : uint64_t.
 * 데이터 흐름: seed → init_rand_seed → 루프당 __rand_0_1 × 12 → 평균화 →
 * stddev 곱 → rand_off 더하기 → __hash_u64 분산 → 0..nranges-1 범위로 클램프.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct gauss_state : 내부 PRNG + 범위 + 표준편차 + 중심 + hash 비활성 플래그.
 * - gauss_init : 범위/편차/중심/시드로 초기화. dev 가 퍼센트 → 블록 수 변환.
 * - gauss_next : 다음 오프셋 반환.
 * - gauss_disable_hash : 해시 믹싱 단계 끄기(테스트용, 재현성 확보 용도).
 */

#ifndef FIO_GAUSS_H
#define FIO_GAUSS_H
/* [한국어] 헤더 가드. io_u.c / init.c 에서 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t 범위 표현. nranges 가 대형 디바이스를
 * 포괄해야 해서 64 비트 필수. */

#include "rand.h"
/* [한국어] "rand.h" : struct frand_state 의 완전 정의. gauss_state 가 frand_state
 * 를 값으로 삽입하므로 헤더가 필요(포인터라면 전방 선언으로 충분하지만
 * 여기는 임베드). __rand_0_1 인라인도 같이 노출되어 gauss.c 가 사용. */

struct gauss_state {
	struct frand_state r;
	/* [한국어] 이 분포 생성기 전용 내부 Tausworthe PRNG 상태.
	 * 설정자: gauss_init 이 seed 로 init_rand_seed 호출하여 초기화.
	 * 읽는 자: gauss_next 가 12 회 __rand_0_1 호출.
	 * 값 범위: 유효 Tausworthe 상태(s1>1, s2>7, s3>15). init_rand_seed 가 보장.
	 * 동기화: 잡 스레드 단일 소유. 다중 잡은 서로 다른 seed 로 독립 실행. */

	uint64_t nranges;
	/* [한국어] 전체 블록 범위(오프셋 후보 수). 반환되는 오프셋은 [0, nranges)
	 * 에 속한다. 설정자: gauss_init. 읽는 자: gauss_next 의 클램프/모듈로.
	 * 값 범위: ≥ 1. 1 이면 항상 0 반환. */

	unsigned int stddev;
	/* [한국어] 표준편차. dev% 를 블록 수로 환산한 값. dev=10% 이고 nranges=
	 * 1,000,000 이면 stddev=100,000. 0 이면 순수 CLT 결과(스케일 없음) 를
	 * 사용해 평균 0 근처의 좁은 분포를 얻는다.
	 * 설정자: gauss_init. 읽는 자: gauss_next 가 CLT 결과에 곱함. */

	unsigned int rand_off;
	/* [한국어] 분포의 중심 오프셋(center 파라미터로 설정). 정규분포의 평균
	 * 위치를 여기로 이동시킨다. 예: center=0.5 & nranges=1M → rand_off=500000.
	 * 설정자: gauss_init. 읽는 자: gauss_next. */

	bool disable_hash;
	/* [한국어] true 이면 결과에 추가 해시 믹싱을 적용하지 않는다. 보통 fio 는
	 * 오프셋에 __hash_u64 를 적용해 공간적으로 분산시키지만, 회귀 테스트/
	 * 디버깅에서 분포 형상 자체를 그대로 관찰하려면 이 플래그를 켠다.
	 * 설정자: gauss_disable_hash API. 읽는 자: gauss_next. */
};

void gauss_init(struct gauss_state *gs, unsigned long nranges, double dev,
		double center, unsigned int seed);
/* [한국어]
 * gauss_init - 정규분포 생성기를 초기화.
 *
 * @gs: 호출자 소유 상태 구조체(스택/힙 모두 가능).
 * @nranges: 전체 블록 범위. 반환 오프셋은 [0, nranges).
 * @dev: dev% — 표준편차 백분율(0..100). 내부에서 stddev = nranges * dev / 100.
 * @center: 평균 위치 백분율(0.0..1.0). 내부에서 rand_off = nranges * center.
 * @seed: PRNG 시드. 잡 간 독립성을 위해 고유 값이어야 함.
 *
 * 실행 컨텍스트: 잡 초기화(메인 또는 잡 스레드). gauss_next 호출 전 반드시 1회. */

unsigned long long gauss_next(struct gauss_state *gs);
/* [한국어]
 * gauss_next - 다음 오프셋 후보를 반환.
 *
 * @gs: gauss_init 으로 준비된 상태.
 * @return: [0, nranges) 범위의 64비트 오프셋. 정규분포 형태.
 *
 * 내부 동작: __rand_0_1 를 12 번 호출해 합 → 평균 6 을 빼 표준정규 근사 →
 * stddev 곱 → rand_off 더함 → 경계 반사/클램프 → disable_hash 아니면
 * __hash_u64 로 공간 분산. 호출 횟수는 잡의 I/O 수와 동일(tight path).
 */

void gauss_disable_hash(struct gauss_state *gs);
/* [한국어] gauss_disable_hash - disable_hash 를 true 로 세팅. 분포 형상 자체를
 * 검증할 때(unittests/ 등) 사용. 일반 I/O 실행에서는 호출하지 않음. */

#endif
/* [한국어] FIO_GAUSS_H 가드 종료. */
