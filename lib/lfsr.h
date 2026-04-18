/*
 * [한국어 설명] 선형 피드백 시프트 레지스터(LFSR) 기반 중복 없는 오프셋 생성기 공개 헤더 (lfsr.h)
 *
 * === 파일의 역할 ===
 * lib/lfsr.c 가 구현하는 Galois LFSR 기반 비트시프트 의사난수 생성기의 상태
 * 구조체(`struct fio_lfsr`, 보조 테이블 타입 `struct lfsr_taps`) 와 세 공개
 * API(`lfsr_init`, `lfsr_next`, `lfsr_reset`) 를 노출한다. LFSR 은 비트 수
 * n 에 대해 2^n-1 의 순환 주기를 갖고 "모든 값을 정확히 한 번씩 방문" 하는
 * 성질이 있어, fio 의 랜덤 I/O 잡에서 `--random_generator=lfsr` 옵션으로
 * axmap 없이도 전 범위 1 회 방문을 보장하는 초저비용 대안이 된다. 원형
 * 논문 근거는 Koopman 의 primitive polynomial 테이블.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 오프셋 생성기 중 "axmap 대체" 경로. axmap 은 log64 단계 비트맵으로
 * 방문 추적에 추가 메모리가 필요하지만, LFSR 은 상태 8 바이트 정도로 같은
 * 보장을 제공한다. 단, 분포는 "사이클 내 uniform" 일 뿐이며 특정 분포
 * (zipf/gauss) 는 표현하지 못해 단순 uniform 워크로드에 한정된다. 부가적
 * "Latin-square" 방지 스킵 정책(spin 파라미터) 로 연속 오프셋의 자기상관을
 * 낮춘다.
 * 호출 체인:
 *   io_u.c get_next_rand_offset (random_generator=lfsr 분기)
 *     → lfsr_next(&td->lfsr, &off)
 *   init.c / filesetup.c : 잡 init 시 lfsr_init(&td->lfsr, size, seed, spin).
 *
 * === 타 모듈과의 연결 ===
 * - lfsr.c : 구현. primitive taps 테이블, XOR 피드백, max_val 초과 값 스킵.
 * - lib/fls.h : __fls 로 필요한 LFSR 비트 수 계산.
 * - io_u.c : 사용자 측.
 * 데이터 흐름: size → 필요한 비트 수 n 결정 → taps 선택 → seed 세팅 →
 * 루프당 lfsr_next → 오프셋 반환 → max_val 초과하는 비트 패턴은 스킵하여
 * 다시 시프트(= 비트 수에 딱 맞는 2^n-1 주기 보정).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct lfsr_taps : 비트 수와 그에 대응하는 primitive polynomial 탭 배열.
 * - struct fio_lfsr : 런타임 상태(xormask, last_val, cached_bit, max_val, ...).
 * - lfsr_init : size 에서 필요한 비트 수를 계산해 taps/max_val/xormask 설정.
 * - lfsr_next : 다음 미방문 오프셋 반환. 전 범위 소진 시 1 반환(종료 신호).
 * - lfsr_reset : seed 만 바꿔 상태를 리셋(잡 재시작용).
 */

#ifndef FIO_LFSR_H
#define FIO_LFSR_H
/* [한국어] 헤더 가드. io_u.c / init.c 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t. LFSR 상태/값/마스크가 모두 64비트 폭. */

#define FIO_MAX_TAPS	6
/* [한국어] 한 primitive polynomial 이 가질 수 있는 비-0 탭(= XOR 에 기여하는
 * 비트 위치) 의 최대 개수. Koopman 테이블에서 대부분의 64비트 이하 원시
 * 다항식이 3~5 개의 탭을 쓰고, 안전 상한으로 6 을 둔다. 이보다 큰 값이
 * 필요한 경우는 이 테이블에서 다루지 않는다. */

struct lfsr_taps {
	unsigned int length;
	/* [한국어] 이 엔트리의 LFSR 비트 폭(= n). 예: 15 → 주기 2^15-1 = 32767.
	 * 설정자: lfsr.c 의 static 테이블이 컴파일 타임에 정의.
	 * 읽는 자: lfsr_init 이 size 에 맞는 최소 length 를 선택. */

	unsigned int taps[FIO_MAX_TAPS];
	/* [한국어] 이 length 에 대한 primitive polynomial 의 탭(비트 위치) 배열.
	 * 0 으로 종결. 예: length=15 의 탭 = {15, 14, 0} 이면 x^15 + x^14 + 1.
	 * 설정자: 컴파일 타임 상수. 읽는 자: lfsr_init 이 xormask 로 조합. */
};


struct fio_lfsr {
	uint64_t xormask;
	/* [한국어] XOR 피드백 마스크. taps 배열의 각 비트 위치에 해당하는 비트
	 * 를 1 로 세팅한 값. next 반복에서 last_val 과 XOR 되어 다음 상태를 결정.
	 * 설정자: lfsr_init. 읽는 자: lfsr_next.
	 * 값 범위: length 비트 내의 0 이 아닌 패턴. 0 이면 LFSR 이 상수에 갇힘. */

	uint64_t last_val;
	/* [한국어] 현재 LFSR 상태(= 직전 반환 값). next 호출 직후 "새 상태 =
	 * 다음 반환 값" 으로 갱신된다.
	 * 설정자: lfsr_init 이 seed 로, 이후 lfsr_next 가 매 호출마다 갱신.
	 * 값 범위: [1, max_val]. 0 은 상태로 사용 불가(0 이면 XOR 만으로 0 유지). */

	uint64_t cached_bit;
	/* [한국어] 1 << (length - 1) — 최상위 비트 위치의 캐시된 비트 마스크.
	 * Galois LFSR 의 XOR 조건 판정("LSB 가 1 이면 xormask 와 XOR") 을 위한
	 * 보조값. lfsr.c 구현 디테일에 따라 용도 다양.
	 * 설정자: lfsr_init. 읽는 자: lfsr_next. */

	uint64_t max_val;
	/* [한국어] 이 LFSR 이 반환하는 유효 값의 최대치(= size - 1).
	 * length 비트 LFSR 의 순환은 [1, 2^length - 1] 을 돌지만, 잡 오프셋
	 * 범위가 2^length 보다 작을 수 있으므로 max_val 초과 값은 lfsr_next
	 * 내부에서 "스킵(다시 시프트)" 하여 건너뛴다.
	 * 설정자: lfsr_init. 읽는 자: lfsr_next. */

	uint64_t num_vals;
	/* [한국어] 지금까지 반환한 값의 개수. max_val 에 도달하면 "전 범위 소진"
	 * 신호로 lfsr_next 가 1 을 반환하여 호출자가 잡을 종료하도록 알린다.
	 * 설정자: lfsr_next 가 증가. 읽는 자: lfsr_next 의 종료 판정. */

	uint64_t cycle_length;
	/* [한국어] Latin-square 스킵 감지용 부분 순환 카운터. spin 이 nonzero 인
	 * 경우 특정 주기에서 자기상관 피크가 나타나는 것을 피하기 위해 강제 스킵.
	 * 0 이면 비활성화(순수 LFSR). */

	uint64_t cached_cycle_length;
	/* [한국어] cycle_length 의 초기값 캐시. lfsr_reset 시 cycle_length 복원에
	 * 사용(리셋 후 다시 부분 순환 감지를 시작). */

	unsigned int spin;
	/* [한국어] 한 번의 lfsr_next 호출이 내부적으로 수행할 LFSR 스텝 배수(0..15).
	 * spin=0 : 기본(1 스텝). spin>0 : 스텝을 (1+spin) 번 진행하여 상관성 감소.
	 * `--random_generator=lfsr:<spin>` 식으로 튜닝 가능. */
};

int lfsr_next(struct fio_lfsr *fl, uint64_t *off);
/* [한국어]
 * lfsr_next - 다음 미방문 오프셋을 *off 에 기록.
 *
 * @fl: lfsr_init 으로 준비된 상태.
 * @off: (출력) 반환 오프셋. [0, max_val].
 * @return: 0 성공, 1 전 범위 소진(잡 종료 신호).
 *
 * 내부에서 Galois XOR 시프트 (1+spin) 회 수행 → max_val 초과 시 반복 → 확정.
 * num_vals > max_val 되면 종료 신호 반환. */

int lfsr_init(struct fio_lfsr *fl, uint64_t size,
	      uint64_t seed, unsigned int spin);
/* [한국어]
 * lfsr_init - size 를 포괄하는 최소 비트 LFSR 을 구성.
 *
 * @fl: 호출자 소유 상태.
 * @size: 필요 오프셋 수(= max_val + 1).
 * @seed: 초기 상태(= 첫 last_val). 0 은 허용되지 않으며 lfsr.c 가 보정.
 * @spin: 스텝 배수. 0 이면 순수 LFSR.
 * @return: 0 성공, 비영 실패(size 가 너무 커서 테이블에 없는 경우 등).
 *
 * 내부 동작: __fls(size) 로 필요 비트 수 n 계산 → static taps 테이블에서
 * length==n 엔트리를 찾아 xormask 구성 → max_val/cached_bit/cycle_length
 * 사전 계산. 잡 초기화 시 한 번. */

int lfsr_reset(struct fio_lfsr *fl, uint64_t seed);
/* [한국어]
 * lfsr_reset - 이미 init 된 상태에 새 seed 만 적용해 재순환 시작.
 *
 * @return: 0 성공. 잡 루프(--loops>1) 에서 매 루프마다 호출 가능. */

#endif
/* [한국어] FIO_LFSR_H 가드 종료. */
