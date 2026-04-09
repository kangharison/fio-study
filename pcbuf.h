/**
 * SPDX-License-Identifier: GPL-2.0 only
 *
 * Copyright (c) 2025 Sandisk Corporation or its affiliates.
 */
/**
 * Two-phase circular buffer implementation for producer/consumer separation.
 *
 * This header defines the data structures and inline functions for a two-phase
 * circular buffer, allowing staged writes and explicit commit of data batches.
 * Useful for double-buffered systems or scenarios requiring controlled visibility
 * of produced data to consumers.
 */
/*
 * [한국어] pcbuf.h - 2단계 원형 버퍼 (프로듀서-컨슈머 분리)
 *
 * 2단계 원형 버퍼를 구현한다:
 *   1단계 (스테이징): 프로듀서가 pcb_push_staged()로 데이터를 임시 저장
 *   2단계 (커밋):     pcb_commit()으로 스테이징된 데이터를 컨슈머에게 공개
 *
 * 핵심 포인터 3개:
 *   - staging_head : 프로듀서가 다음에 쓸 위치 (스테이징 영역의 끝)
 *   - commit_head  : 커밋된 데이터의 끝 (컨슈머에게 보이는 범위)
 *   - read_tail    : 컨슈머가 다음에 읽을 위치
 *
 * 데이터 흐름: push_staged -> commit -> pop
 *   [read_tail ... commit_head] : 컨슈머가 읽을 수 있는 커밋된 데이터
 *   [commit_head ... staging_head] : 스테이징되었지만 아직 커밋 안 된 데이터
 *
 * 용도: 더블 버퍼링, 배치 커밋이 필요한 프로듀서-컨슈머 시나리오
 */
#ifndef PHASE_CIRCULAR_BUFFER_H
#define PHASE_CIRCULAR_BUFFER_H

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

/**
 * struct pc_buf - Two-phase circular buffer.
 * @commit_head:  Index of the next committed element in the buffer (visible to consumer).
 * @staging_head: Index of the next staged (but not yet committed) element (written by producer).
 * @read_tail:    Index of the next element to be read by the consumer.
 * @capacity:     Total capacity of the buffer (number of elements).
 * @buffer:       Buffer data.
 *
 * This structure implements a two-phase circular buffer, where data is first staged
 * by advancing @staging_head, and only becomes visible to the consumer when @commit_head
 * is explicitly updated. This allows for controlled commit of data batches, useful in
 * double-buffered systems or producer/consumer separation.
 */
/* [한국어] 2단계 원형 버퍼 구조체 */
struct pc_buf {
	uint64_t commit_head;   /* 커밋 헤드: 컨슈머에게 보이는 데이터의 끝 인덱스 */
	uint64_t staging_head;  /* 스테이징 헤드: 프로듀서가 쓴 데이터의 끝 인덱스 */
	uint64_t read_tail;     /* 읽기 테일: 컨슈머가 다음에 읽을 인덱스 */
	uint64_t capacity;      /* 버퍼 용량 (원소 수) */
	uint64_t buffer[];      /* 유연한 배열 멤버 - 실제 데이터 저장 공간 */
};

/**
 * pcb_alloc - Allocate and initialize buffer.
 * @capacity: Number of elements the buffer can hold.
 *
 * Returns a pointer to the allocated buffer, or NULL on failure.
 */
/* [한국어] 버퍼를 할당하고 초기화. 실패 시 NULL 반환 */
static inline struct pc_buf *pcb_alloc(uint64_t capacity)
{
	size_t size = sizeof(struct pc_buf) + sizeof(uint64_t) * capacity;
	struct pc_buf *cb = (struct pc_buf *)malloc(size);

	if (!cb)
		return NULL;
	cb->commit_head = 0;
	cb->staging_head = 0;
	cb->read_tail = 0;
	cb->capacity = capacity;
	return cb;
}

/**
 * pcb_is_empty - Check if the buffer is empty.
 * @cb: pointer to the pc_buf structure.
 *
 * Returns true if the buffer has no committed data.
 */
/* [한국어] 버퍼가 비어있는지 확인 (커밋된 데이터가 없으면 비어있음) */
static inline bool pcb_is_empty(const struct pc_buf *cb)
{
	return cb->read_tail == cb->commit_head;
}

/**
 * pcb_is_full - Check if the buffer is full.
 * @cb: pointer to the pc_buf structure.
 *
 * Returns true if the buffer cannot accept more staged data.
 */
/* [한국어] 버퍼가 가득 찼는지 확인 (스테이징 공간이 없으면 가득 찬 것) */
static inline bool pcb_is_full(const struct pc_buf *cb)
{
	return ((cb->staging_head + 1) % cb->capacity) == cb->read_tail;
}

/**
 * pcb_push_staged - Push a value into the staged buffer.
 * @cb: pointer to the pc_buf structure.
 * @value: value to be staged.
 *
 * Returns true if the value was successfully staged, false if the buffer is full.
 */
/* [한국어] 스테이징 영역에 값을 추가. 가득 차면 false 반환 */
static inline bool pcb_push_staged(struct pc_buf *cb, uint64_t value)
{
	if (pcb_is_full(cb))
		return false;

	cb->buffer[cb->staging_head] = value;
	cb->staging_head = (cb->staging_head + 1) % cb->capacity;
	return true;
}

/**
 * pcb_commit - Commit the staged data to make it visible to consumers.
 * @cb: pointer to the pc_buf structure.
 *
 * Updates the commit head to the current staging head, making
 * all staged data visible to consumers. It should be called after staging data.
 */
/* [한국어] 스테이징된 데이터를 커밋하여 컨슈머에게 공개 */
static inline void pcb_commit(struct pc_buf *cb)
{
	cb->commit_head = cb->staging_head;
}

/**
 * pcb_pop - Pop a value from the committed buffer.
 * @cb: pointer to the pc_buf structure.
 * @out: pointer to the variable to store the popped value.
 *
 * Returns true if a value was successfully popped, false if the buffer is empty.
 */
/* [한국어] 커밋된 버퍼에서 값을 하나 꺼냄. 비어있으면 false 반환 */
static inline bool pcb_pop(struct pc_buf *cb, uint64_t *out)
{
	if (pcb_is_empty(cb))
		return false;

	*out = cb->buffer[cb->read_tail];
	cb->read_tail = (cb->read_tail + 1) % cb->capacity;
	return true;
}

/**
 * pcb_print_committed - Print the contents of the committed buffer.
 * @cb: pointer to the pc_buf structure.
 *
 * This function prints all committed data in the buffer.
 */
/* [한국어] 커밋된 버퍼의 내용을 출력 (디버깅용) */
static inline void pcb_print_committed(const struct pc_buf *cb)
{
	uint64_t i = cb->read_tail;

	printf("Committed buffer: ");
	while (i != cb->commit_head) {
		printf("%" PRIu64 " ", cb->buffer[i]);
		i = (i + 1) % cb->capacity;
	}
	printf("\n");
}

/**
 * pcb_print_staged - Print the contents of the staged buffer.
 * @cb: pointer to the pc_buf structure.
 *
 * This function prints all staged data that has not yet been committed.
 */
/* [한국어] 스테이징 영역의 내용을 출력 (아직 커밋되지 않은 데이터, 디버깅용) */
static inline void pcb_print_staged(const struct pc_buf *cb)
{
	uint64_t i = cb->commit_head;

	printf("Staged (not visible yet): ");
	while (i != cb->staging_head) {
		printf("%" PRIu64 " ", cb->buffer[i]);
		i = (i + 1) % cb->capacity;
	}
	printf("\n");
}

/**
 * pcb_committed_size - Get the size of committed data in the buffer.
 * @cb: pointer to the pc_buf structure.
 *
 * Returns the number of elements that have been committed and are visible to consumers.
 */
/* [한국어] 커밋된 데이터의 원소 수를 반환 (컨슈머가 읽을 수 있는 양) */
static inline uint64_t pcb_committed_size(const struct pc_buf *cb)
{
	if (cb->commit_head >= cb->read_tail)
		return cb->commit_head - cb->read_tail;
	else
		return cb->capacity - cb->read_tail + cb->commit_head;
}

/**
 * pcb_staged_size - Get the size of staged data in the buffer.
 * @cb: pointer to the pc_buf structure.
 *
 * Returns the number of elements that have been staged but not yet committed.
 */
/* [한국어] 스테이징되었지만 아직 커밋되지 않은 데이터의 원소 수를 반환 */
static inline uint64_t pcb_staged_size(const struct pc_buf *cb)
{
	if (cb->staging_head >= cb->commit_head)
		return cb->staging_head - cb->commit_head;
	else
		return cb->capacity - cb->commit_head + cb->staging_head;
}

/**
 * pcb_space_available - Check if there is space available for staging.
 * @cb: pointer to the pc_buf structure.
 *
 * Returns true if there is space available for staging new data, false if the buffer is full.
 */
/* [한국어] 스테이징 가능한 공간이 있는지 확인 (1슬롯은 full/empty 구분용으로 예약) */
static inline bool pcb_space_available(const struct pc_buf *cb)
{
	uint64_t used = pcb_committed_size(cb) + pcb_staged_size(cb);
	/* keep 1 slot reserved to distinguish full from empty */
	return used < (cb->capacity - 1);
}

#endif /* PHASE_CIRCULAR_BUFFER_H */
