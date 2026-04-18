/*
 * [한국어 설명] 시퀀스 락(seqlock) 동기화 프리미티브 헤더 (seqlock.h)
 *
 * === 파일의 역할 ===
 * Linux 커널의 seqlock 을 헤더 only 로 이식한 경량 읽기-우선 동기화 프리미티브
 * 를 제공한다. 쓰기 측이 1 명뿐이거나 서로 직렬화되는 전제에서, 읽기 측이
 * 락을 전혀 잡지 않고 시퀀스 번호만 관찰해 "읽기 도중 쓰기가 있었는가?" 를
 * 사후적으로 확인하는 방식이다. fio 의 통계 수집 경로에서 잡 스레드가
 * 실시간으로 bytes/lat 등을 갱신하는 동안, 헬퍼/서버 스레드가 잡의 통계
 * 스냅샷을 여러 필드 단위로 일관성 있게 읽어야 할 때 매우 저비용으로
 * 동작한다(가장 무거운 락보다 한 자릿수 이상 빠름).
 *
 * 모델: sequence 가 짝수 = 안정 상태. 홀수 = 쓰기 진행 중.
 *   write_seqlock_begin : seq++ (짝→홀). 이제 쓰기 중 상태.
 *   write_seqlock_end   : seq++ (홀→짝). 이제 다시 안정.
 *   read_seqlock_begin  : seq 읽어 짝수인지 대기(홀수면 busy-wait).
 *   read_seqlock_retry  : 다시 seq 읽어 변했으면 true(=재시도 필요) 반환.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 잡 스레드 ↔ 헬퍼/서버 스레드 간 라이브 통계 동기화 계층. 비트/
 * 카운터류 단일 워드는 atomic 으로 충분하지만, stat 의 여러 필드가 일관
 * 관계를 가지는 경우(예: total_bytes 와 total_iops 이 하나의 샘플링 구간에
 * 속해야 함) 이 seqlock 을 써서 일관 스냅샷을 얻는다.
 * 호출 체인(개념):
 *   잡 스레드: write_seqlock_begin → 여러 필드 갱신 → write_seqlock_end.
 *   읽기 스레드: do { seq = read_seqlock_begin; 필드들 복사;
 *                    } while (read_seqlock_retry(&lock, seq));
 *
 * === 타 모듈과의 연결 ===
 * - arch/arch.h : atomic_load_acquire / atomic_store_release / nop /
 *   read_barrier 등 메모리 모델 원시 연산 공급.
 * - stat.c, helper_thread.c, server.c : 소비자(정확한 사용처는 소스 grep 참고).
 * 데이터 흐름: 쓰기 원자 카운터 증가(시퀀스) → 공유 필드 갱신 → 다시 시퀀스
 * 증가. 읽기: 시퀀스 관찰 → 필드 복사 → 시퀀스 재관찰로 변경 여부 판정.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct seqlock : 단일 unsigned int 시퀀스 카운터. 짝수=안정, 홀수=쓰기 중.
 * - seqlock_init : sequence=0 초기화.
 * - read_seqlock_begin / read_seqlock_retry : 읽기 측 비관망 프로토콜.
 * - write_seqlock_begin / write_seqlock_end : 쓰기 측 양방향 barrier.
 */
#ifndef FIO_SEQLOCK_H
#define FIO_SEQLOCK_H
/* [한국어] 헤더 가드. 다중 포함 방지. */

#include "types.h"
/* [한국어] "types.h" : bool 폴백. read_seqlock_retry 반환 타입. */

#include "../arch/arch.h"
/* [한국어] "../arch/arch.h" : atomic_load_acquire / atomic_store_release /
 * read_barrier() / nop 매크로 제공. 각 아키텍처별 메모리 모델 구현이
 * 선택된다(x86 은 acquire/release 가 대부분 단일 load/store + 배리어,
 * ARM64 는 LDAR/STLR 명령 활용). */

struct seqlock {
#ifdef __cplusplus
	std::atomic<unsigned int> sequence;
	/* [한국어] C++ 빌드(예: gfio 의 일부 연결) 에서는 std::atomic 을 쓰는
	 * 편이 팀원 간 의도 전달이 분명하여 조건부로 선택. 런타임 의미는 아래
	 * volatile 버전과 동일한 relaxed/acquire/release 세트로 축약됨. */
#else
	volatile unsigned int sequence;
	/* [한국어] 시퀀스 카운터. 짝수=안정 상태, 홀수=쓰기 진행 중.
	 * 쓰기 시작 시 +1(홀수), 완료 시 +1(짝수). 읽기 측은 변경 감지 후 재시도.
	 * volatile 은 컴파일러의 공격적 최적화로 인한 캐시 우회를 방지하는
	 * 최소 수단 — 메모리 장벽은 atomic_load_acquire/store_release 가 담당.
	 * 설정자: write_seqlock_begin/end, seqlock_init.
	 * 읽는 자: read_seqlock_begin/retry.
	 * 값 범위: 0..UINT_MAX (랩어라운드 허용; 재시도 판정은 부등식이 아닌
	 *   부등변경 기반이라 랩어라운드에도 안전). */
#endif
};

static inline void seqlock_init(struct seqlock *s)
{
	s->sequence = 0;
	/* [한국어] 시퀀스를 0(짝수) 로 초기화하여 안정 상태에서 출발. 구조체가
	 * BSS/영초기화 영역에 있으면 이 호출 없이도 0 이지만, 스택/힙 할당
	 * 인스턴스라면 반드시 호출 필요. */
}

/*
 * [한국어] read_seqlock_begin - 읽기 시작 시 시퀀스 번호를 읽음.
 *
 * 시퀀스 번호가 홀수(쓰기 진행 중)이면 짝수가 될 때까지 spin 대기한다.
 * 반환된 시퀀스 번호는 read_seqlock_retry() 에서 쓰기 발생 여부 검사에 사용.
 *
 * 실행 컨텍스트: 아무 읽기 스레드. 쓰기 스레드와의 경합은 busy-spin 이므로
 *   락 대기가 매우 짧아야 한다(fio 의 통계 갱신은 마이크로초 미만).
 */
static inline unsigned int read_seqlock_begin(struct seqlock *s)
{
	unsigned int seq;
	/* [한국어] 반환용 로컬 변수. 이후 read_seqlock_retry 에 같은 값을 넘김. */

	do {
		seq = atomic_load_acquire(&s->sequence);
		/* [한국어] acquire 로드 : 이후의 모든 로드/스토어가 이 로드보다
		 * 먼저 재배치되지 않도록 보장(= 아래서 읽을 필드들이 "과거의"
		 * 데이터일 수 없게 함). */
		if (!(seq & 1))
			break;
		/* [한국어] 짝수(비트 0 이 0) 이면 안정 상태 → 루프 종료. */
		nop;
		/* [한국어] 홀수이면 nop(busy-wait 힌트) 로 한 사이클 소진 후 재시도.
		 * 실전에서는 쓰기 구간이 짧아 수 사이클 내 종료 기대. */
	} while (1);

	return seq;
	/* [한국어] 안정 상태의 시퀀스 값. retry 에서 비교 기준으로 사용. */
}

/*
 * [한국어] read_seqlock_retry - 읽기 중 쓰기가 발생했는지 확인.
 * @return: true 이면 쓰기 발생 → 읽기 재시도 필요.
 *
 * read_seqlock_begin 과 이 함수 사이에 다른 필드들을 읽는 사용 이디엄.
 * 실행 컨텍스트: 읽기 스레드.
 */
static inline bool read_seqlock_retry(struct seqlock *s, unsigned int seq)
{
	read_barrier();
	/* [한국어] 이 지점 전의 모든 필드 로드를 완료시킨 뒤 시퀀스를 다시
	 * 읽게 한다 — 필드 로드와 시퀀스 재관찰의 순서를 CPU 가 재배치하지
	 * 못하게 하는 장벽. */
	return s->sequence != seq;
	/* [한국어] 시작 시 관찰한 seq 와 지금의 sequence 가 다르다면 쓰기가
	 * 일어났음 → 재시도 필요. 같으면 스냅샷 유효. */
}

/* [한국어] write_seqlock_begin - 쓰기 시작 (시퀀스를 홀수로 만듦).
 * 호출 후 이 스레드는 보호된 필드들을 자유롭게 갱신할 수 있다. 주의:
 * 쓰기 스레드가 1 명이거나 쓰기 간 상호배제가 외부 락으로 보장되어야 한다
 * — seqlock 은 write-write race 를 방어하지 않는다. */
static inline void write_seqlock_begin(struct seqlock *s)
{
	s->sequence = atomic_load_acquire(&s->sequence) + 1;
	/* [한국어] 현재 값을 acquire 로드 후 +1 하여 저장. +1 은 짝→홀 전이로
	 * "쓰기 진행 중" 신호. acquire 로드는 이후 쓰기들이 이 지점 이전으로
	 * 재배치되지 않도록 막는다. */
}

/* [한국어] write_seqlock_end - 쓰기 완료(시퀀스를 짝수로 되돌림, release 시맨틱). */
static inline void write_seqlock_end(struct seqlock *s)
{
	atomic_store_release(&s->sequence, s->sequence + 1);
	/* [한국어] release 스토어 : 이전의 모든 필드 쓰기가 이 스토어 이전에
	 * 관찰 가능함을 보장. 읽기 스레드의 acquire 로드와 쌍을 이뤄 happens-
	 * before 관계 성립. 값은 +1 하여 홀→짝 전이(안정 상태 복귀). */
}

#endif
/* [한국어] FIO_SEQLOCK_H 가드 종료. */
