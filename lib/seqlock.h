/*
 * [한국어 설명] 시퀀스 락 (Sequence Lock) 동기화 프리미티브 (seqlock.h)
 *
 * === 파일의 역할 ===
 * Linux 커널에서 유래한 읽기-쓰기 동기화 메커니즘인 시퀀스 락을 구현한다.
 * 쓰기 측은 시퀀스 번호를 홀수로 만들어 쓰기 중임을 표시하고, 완료 시 짝수로 되돌린다.
 * 읽기 측은 시퀀스 번호가 변경되었는지 확인하여 읽기 중 쓰기가 발생했으면 재시도한다.
 *
 * === fio에서의 사용 ===
 * fio의 멀티스레드 환경에서 통계 데이터나 공유 상태를 락 없이 효율적으로 읽을 때 사용된다.
 * 쓰기가 드물고 읽기가 빈번한 경우에 최적화된 동기화 방식으로, I/O 통계 수집 등에 활용된다.
 */
#ifndef FIO_SEQLOCK_H
#define FIO_SEQLOCK_H

#include "types.h"
#include "../arch/arch.h"

struct seqlock {
#ifdef __cplusplus
	std::atomic<unsigned int> sequence;
#else
	volatile unsigned int sequence;
	/* [한국어] 시퀀스 카운터. 짝수=안정 상태, 홀수=쓰기 진행 중.
	 * 쓰기 시작 시 +1(홀수), 완료 시 +1(짝수). 읽기 측은 변경 감지 후 재시도 */
#endif
};

static inline void seqlock_init(struct seqlock *s)
{
	s->sequence = 0;
}

/*
 * [한국어] read_seqlock_begin - 읽기 시작 시 시퀀스 번호를 읽음
 *
 * 시퀀스 번호가 홀수(쓰기 진행 중)이면 짝수가 될 때까지 spin 대기한다.
 * 반환된 시퀀스 번호는 read_seqlock_retry()에서 쓰기 발생 여부 검사에 사용.
 */
static inline unsigned int read_seqlock_begin(struct seqlock *s)
{
	unsigned int seq;

	do {
		seq = atomic_load_acquire(&s->sequence);
		if (!(seq & 1))
			break;
		nop;
	} while (1);

	return seq;
}

/*
 * [한국어] read_seqlock_retry - 읽기 중 쓰기가 발생했는지 확인
 * @return: true이면 쓰기 발생 → 읽기 재시도 필요
 */
static inline bool read_seqlock_retry(struct seqlock *s, unsigned int seq)
{
	read_barrier();
	return s->sequence != seq;
}

/* [한국어] write_seqlock_begin - 쓰기 시작 (시퀀스를 홀수로 만듦) */
static inline void write_seqlock_begin(struct seqlock *s)
{
	s->sequence = atomic_load_acquire(&s->sequence) + 1;
}

/* [한국어] write_seqlock_end - 쓰기 완료 (시퀀스를 짝수로 되돌림, release 시맨틱) */
static inline void write_seqlock_end(struct seqlock *s)
{
	atomic_store_release(&s->sequence, s->sequence + 1);
}

#endif
