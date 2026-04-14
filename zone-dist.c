/*
 * [한국어] zone-dist.c - 존(zone) 분배 인덱스 생성 로직
 *
 * 이 파일은 zone_split 옵션에 따라 I/O 접근 패턴을 존별로 분배하는
 * 인덱스 테이블을 생성한다. 핫 I/O 경로에서 매번 계산하지 않고,
 * 미리 생성된 인덱스 테이블을 참조하여 성능을 최적화한다.
 *
 * zone_split은 디바이스를 여러 영역으로 나누고, 각 영역에
 * 서로 다른 접근 비율(access_perc)과 크기 비율(size_perc)을 부여한다.
 
 * === 파일의 역할 ===
 * zone_split 옵션에 따라 I/O 접근을 존별로 분배하는 인덱스 테이블을 생성.
 *
 * === 전체 아키텍처에서의 위치 ===
 * init.c에서 td_zone_gen_index()로 인덱스 생성. io_u.c에서 참조.
 *
 * === 타 모듈과의 연결 ===
 * - init.c: 인덱스 생성 호출
 * - io_u.c: 오프셋 결정 시 인덱스 참조
 * - zone-dist.h: API 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - td_zone_gen_index(): 존 분배 인덱스 생성
 * - td_zone_free_index(): 인덱스 해제
 */
#include <stdlib.h>
#include "fio.h"
#include "zone-dist.h"

/* [한국어] 특정 I/O 방향(ddir)에 대한 존 분배 인덱스를 생성하는 내부 함수
 * zone_split 설정을 기반으로 100개 항목의 인덱스 테이블을 만든다.
 * 각 항목은 접근 비율(%)에 해당하는 크기 범위를 저장한다. */
static void __td_zone_gen_index(struct thread_data *td, enum fio_ddir ddir)
{
	unsigned int i, j, sprev, aprev;
	uint64_t sprev_sz;

	/* 접근 비율 0~99%에 대응하는 100개 엔트리 할당 */
	td->zone_state_index[ddir] = malloc(sizeof(struct zone_split_index) * 100);

	sprev_sz = sprev = aprev = 0;
	for (i = 0; i < td->o.zone_split_nr[ddir]; i++) {
		struct zone_split *zsp = &td->o.zone_split[ddir][i];

		/* 이 존의 접근 비율 범위에 해당하는 인덱스 엔트리를 채움 */
		for (j = aprev; j < aprev + zsp->access_perc; j++) {
			struct zone_split_index *zsi = &td->zone_state_index[ddir][j];

			zsi->size_perc = sprev + zsp->size_perc;       /* 누적 크기 비율 */
			zsi->size_perc_prev = sprev;                     /* 이전 누적 크기 비율 */

			zsi->size = sprev_sz + zsp->size;               /* 누적 크기 (바이트) */
			zsi->size_prev = sprev_sz;                       /* 이전 누적 크기 */
		}

		aprev += zsp->access_perc;    /* 접근 비율 누적 */
		sprev += zsp->size_perc;      /* 크기 비율 누적 */
		sprev_sz += zsp->size;        /* 크기 누적 */
	}
}

/* [한국어] zone_split 설정이 존재하는지 확인하는 함수
 * 읽기/쓰기/트림 모든 방향의 zone_split 수를 합산한다. */
static bool has_zones(struct thread_data *td)
{
	int i, zones = 0;

	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		zones += td->o.zone_split_nr[i];

	return zones != 0;
}

/*
 * Generate state table for indexes, so we don't have to do it inline from
 * the hot IO path
 */
/* [한국어] 모든 I/O 방향에 대해 존 분배 인덱스를 생성하는 외부 함수
 * zone_split 설정이 없으면 아무것도 하지 않는다. */
void td_zone_gen_index(struct thread_data *td)
{
	int i;

	if (!has_zones(td))
		return;

	/* DDIR_RWDIR_CNT개(읽기, 쓰기, 트림)의 인덱스 포인터 배열 할당 */
	td->zone_state_index = malloc(DDIR_RWDIR_CNT *
					sizeof(struct zone_split_index *));

	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		__td_zone_gen_index(td, i);
}

/* [한국어] 존 분배 인덱스를 해제하는 함수
 * 각 방향별 인덱스를 free하고 전체 배열도 해제한다. */
void td_zone_free_index(struct thread_data *td)
{
	int i;

	if (!td->zone_state_index)
		return;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		free(td->zone_state_index[i]);
		td->zone_state_index[i] = NULL;
	}

	free(td->zone_state_index);
	td->zone_state_index = NULL;
}
