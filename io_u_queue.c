/*
 * [한국어] io_u_queue.c - I/O 유닛(io_u) 큐 및 링 버퍼 관리
 *
 * 이 파일은 io_u 구조체의 freelist를 관리하는 두 가지 자료구조를 구현한다:
 *   1) io_u_queue - 스택(LIFO) 방식의 배열 기반 큐
 *      - 주로 io_u freelist로 사용 (get_io_u/put_io_u에서 활용)
 *      - shared=true이면 smalloc(공유 메모리), false이면 calloc으로 할당
 *   2) io_u_ring  - 원형 버퍼(FIFO) 방식의 링
 *      - 크기를 2의 거듭제곱으로 올림하여 비트 AND 래핑 사용
 */
#include <stdlib.h>
#include <string.h>
#include "io_u_queue.h"
#include "smalloc.h"

/* [한국어] io_u 큐를 초기화하는 함수
 * shared=true이면 프로세스 간 공유를 위해 smalloc으로 할당한다. */
bool io_u_qinit(struct io_u_queue *q, unsigned int nr, bool shared)
{
	if (shared)
		q->io_us = smalloc(nr * sizeof(struct io_u *));
	else
		q->io_us = calloc(nr, sizeof(struct io_u *));

	if (!q->io_us)
		return false;

	q->nr = 0;      /* 현재 저장된 io_u 수 */
	q->max = nr;    /* 최대 용량 */
	return true;
}

/* [한국어] io_u 큐를 해제하는 함수 */
void io_u_qexit(struct io_u_queue *q, bool shared)
{
	if (shared)
		sfree(q->io_us);
	else
		free(q->io_us);
}

/* [한국어] io_u 링 버퍼를 초기화하는 함수
 * 크기를 2의 거듭제곱으로 올림하여 비트 AND 연산으로 래핑할 수 있게 한다. */
bool io_u_rinit(struct io_u_ring *ring, unsigned int nr)
{
	ring->max = nr + 1;
	/* 2의 거듭제곱으로 올림 (비트 연산 트릭) */
	if (ring->max & (ring->max - 1)) {
		ring->max--;
		ring->max |= ring->max >> 1;
		ring->max |= ring->max >> 2;
		ring->max |= ring->max >> 4;
		ring->max |= ring->max >> 8;
		ring->max |= ring->max >> 16;
		ring->max++;
	}

	ring->ring = calloc(ring->max, sizeof(struct io_u *));
	if (!ring->ring)
		return false;

	ring->head = ring->tail = 0;
	return true;
}

/* [한국어] io_u 링 버퍼를 해제하는 함수 */
void io_u_rexit(struct io_u_ring *ring)
{
	free(ring->ring);
}
