/**
 * SPDX-License-Identifier: GPL-2.0 only
 *
 * Copyright (c) 2025 Sandisk Corporation or its affiliates.
 */
/*
 * [한국어] sprandom.h - SSD 정상 상태 랜덤 쓰기 생성기 헤더
 *
 * SSD의 정상 상태(Steady-State) 데이터 분포를 모델링하여
 * 실제 SSD 워크로드와 유사한 랜덤 쓰기 패턴을 생성하기 위한
 * 구조체와 API를 정의한다.
 */

#ifndef FIO_SPRANDOM_H
#define FIO_SPRANDOM_H

#include <stdint.h>
#include "lib/rand.h"
#include "pcbuf.h"

/**
 * struct sprandom_info - information for sprandom operations.
 *
 * @over_provisioning:  Over-provisioning ratio for the flash device.
 * @region_sz:          Size of each region in bytes.
 * @num_regions:        Number of SPRandom regions.
 * @validity_dist:      validity for each region.
 * @invalid_pct:        invalidation percentages per region.
 * @invalid_buf:        invalidation offsets two pahse buffer.
 * @invalid_capacity:   maximal size of invalidation buffer for a region.
 * @invalid_count:      number of invalid offsets in each phase.
 * @current_region:     index of the current region being processed.
 * @curr_phase:         current phase of the invalidation process (0 or 1).
 * @region_write_count: number of writes performed in the current region.
 * @writes_remaining:   umber of writes left to perform.
 * @rand_state:         state for the random number generator.
 */
/* [한국어] sprandom_info - sprandom 연산 상태 구조체.
 *          리전 기반 쓰기 생성 및 무효화 관리에 필요한 모든 상태를 포함한다. */
struct sprandom_info {
	double    over_provisioning;    /* Over-Provisioning 비율 (예: 0.2 = 20%) */
	uint64_t  region_sz;            /* 리전 크기 (바이트) */
	uint64_t  cache_sz;             /* SSD 캐시 크기 (무효화 지연 모드용) */
	uint32_t  num_regions;          /* 리전 총 개수 */

	uint32_t  *invalid_pct;         /* 리전별 무효화 확률 (PCT_PRECISION 스케일) */

	/* Invalidation list - 무효화 목록 (2-phase 순환 버퍼) */
	struct pc_buf *invalid_buf;     /* 무효화 오프셋 저장 버퍼 */
	uint64_t invalid_capacity;      /* 무효화 버퍼 최대 용량 */
	size_t   invalid_count[2];      /* 각 phase의 무효화 오프셋 수 */
	uint32_t current_region;        /* 현재 처리 중인 리전 인덱스 */
	uint32_t curr_phase;            /* 현재 무효화 phase (0 또는 1) */

	/* Region and write tracking - 리전 및 쓰기 추적 */
	uint64_t region_write_count;    /* 리전당 총 쓰기 수 */
	uint64_t writes_remaining;      /* 현재 리전의 남은 쓰기 수 */

	struct frand_state *rand_state; /* 난수 생성기 상태 */
};

/**
 * sprandom_init - Initialize the sprandom for a given file and thread.
 * @td: FIO thread data
 * @f: FIO file
 *
 * Returns 0 on success, or a negative error code on failure.
 */
/* [한국어] sprandom_init - 파일/스레드별 sprandom 초기화 */
int sprandom_init(struct thread_data *td, struct fio_file *f);

/**
 * sprandom_free - Frees resources associated with a sprandom_info structure.
 * @info: sprandom_info structure to be freed.
 */
/* [한국어] sprandom_free - sprandom 리소스 해제 */
void sprandom_free(struct sprandom_info *info);

/**
 * sprandom_get_next_offset - Get the next random offset for a file.
 * @info: sprandom_info structure containing the state
 * @f: FIO file
 * @b: Output pointer to store the next offset.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
/* [한국어] sprandom_get_next_offset - 다음 랜덤 쓰기 오프셋 생성 */
int sprandom_get_next_offset(struct sprandom_info *info, struct fio_file *f, uint64_t *b);

#endif /* FIO_SPRANDOM_H */
