#ifndef FIO_IO_U_QUEUE
#define FIO_IO_U_QUEUE
/*
 * [한국어] io_u_queue.h - I/O 유닛(io_u) 큐 및 링 버퍼 헤더
 *
 * io_u 구조체의 freelist를 관리하기 위한 두 가지 자료구조를 정의한다:
 *   - io_u_queue: 스택(LIFO) 기반 배열 큐 (push/pop)
 *   - io_u_ring:  원형 버퍼(FIFO) 기반 링 (rpush/rpop)
 
 * === 파일의 역할 ===
 * io_u 스택(LIFO) 큐와 원형 버퍼(FIFO) 링의 구조체/API를 정의.
 *
 * === 전체 아키텍처에서의 위치 ===
 * io_u_queue.c와 짝을 이루는 헤더.
 *
 * === 타 모듈과의 연결 ===
 * - io_u_queue.c: 이 헤더의 함수 구현
 *
 * === 주요 함수/구조체 요약 ===
 * - struct io_u_queue: 스택(LIFO) 큐
 * - struct io_u_ring: 원형 버퍼(FIFO) 링
 */

#include <assert.h>
#include <stddef.h>

#include "lib/types.h"

struct io_u;

/* [한국어] 스택(LIFO) 방식의 io_u 큐 구조체 */
struct io_u_queue {
	struct io_u **io_us;   /* io_u 포인터 배열 */
	unsigned int nr;       /* 현재 저장된 io_u 수 */
	unsigned int max;      /* 최대 용량 */
};

/* [한국어] 큐에서 io_u를 꺼내는 함수 (LIFO - 마지막에 넣은 것을 먼저 꺼냄) */
static inline struct io_u *io_u_qpop(struct io_u_queue *q)
{
	if (q->nr) {
		const unsigned int next = --q->nr;
		struct io_u *io_u = q->io_us[next];

		q->io_us[next] = NULL;
		return io_u;
	}

	return NULL;
}

/* [한국어] 큐에 io_u를 넣는 함수 (배열 끝에 추가) */
static inline void io_u_qpush(struct io_u_queue *q, struct io_u *io_u)
{
	if (q->nr < q->max) {
		q->io_us[q->nr++] = io_u;
		return;
	}

	assert(0);
}

/* [한국어] 큐가 비어있는지 확인 */
static inline int io_u_qempty(const struct io_u_queue *q)
{
	return !q->nr;
}

/* [한국어] 큐의 모든 io_u를 순회하는 매크로 */
#define io_u_qiter(q, io_u, i)	\
	for (i = 0; i < (q)->nr && (io_u = (q)->io_us[i]); i++)

bool io_u_qinit(struct io_u_queue *q, unsigned int nr, bool shared);
void io_u_qexit(struct io_u_queue *q, bool shared);

/* [한국어] 원형 버퍼(FIFO) 방식의 io_u 링 구조체
 * head/tail 인덱스와 비트 AND 래핑으로 동작한다. */
struct io_u_ring {
	unsigned int head;      /* 쓰기 위치 */
	unsigned int tail;      /* 읽기 위치 */
	unsigned int max;       /* 버퍼 크기 (2의 거듭제곱) */
	struct io_u **ring;     /* io_u 포인터 링 버퍼 */
};

bool io_u_rinit(struct io_u_ring *ring, unsigned int nr);
void io_u_rexit(struct io_u_ring *ring);

/* [한국어] 링에 io_u를 삽입하는 함수 (FIFO - head에 추가) */
static inline void io_u_rpush(struct io_u_ring *r, struct io_u *io_u)
{
	if (r->head + 1 != r->tail) {
		r->ring[r->head] = io_u;
		r->head = (r->head + 1) & (r->max - 1);
		return;
	}

	assert(0);
}

/* [한국어] 링에서 io_u를 꺼내는 함수 (FIFO - tail에서 추출) */
static inline struct io_u *io_u_rpop(struct io_u_ring *r)
{
	if (r->head != r->tail) {
		struct io_u *io_u = r->ring[r->tail];

		r->tail = (r->tail + 1) & (r->max - 1);
		return io_u;
	}

	return NULL;
}

/* [한국어] 링이 비어있는지 확인 (head == tail이면 비어있음) */
static inline int io_u_rempty(struct io_u_ring *ring)
{
	return ring->head == ring->tail;
}

#endif
