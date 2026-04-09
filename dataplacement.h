/*
 * [한국어] dataplacement.h - FDP(Flexible Data Placement) 데이터 배치 헤더
 *
 * NVMe FDP 및 스트림(Streams) 기반 데이터 배치를 위한 상수, 구조체, 함수 선언.
 * FDP는 호스트가 데이터의 물리적 배치를 힌트로 제공하여
 * SSD 내부의 가비지 컬렉션(GC) 효율을 높이는 기술이다.
 */
#ifndef FIO_DATAPLACEMENT_H
#define FIO_DATAPLACEMENT_H

#include "io_u.h"   /* io_u 구조체 정의 */

#define STREAMS_DIR_DTYPE	1       /* 스트림 방식의 directive type */
#define FDP_DIR_DTYPE		2       /* FDP 방식의 directive type */
#define FIO_MAX_DP_IDS 		128     /* 최대 데이터 배치 ID 개수 */
#define DP_MAX_SCHEME_ENTRIES	32      /* 스킴(scheme) 최대 항목 수 */

/*
 * How fio chooses what placement identifier to use next. Choice of
 * uniformly random, or roundrobin.
 */
/* [한국어] 배치 ID 선택 정책 */
enum {
	FIO_DP_RANDOM	= 0x1,   /* 균등 랜덤 선택 */
	FIO_DP_RR	= 0x2,   /* 라운드 로빈 선택 */
	FIO_DP_SCHEME	= 0x3,   /* 오프셋 기반 스킴 선택 */
};

/* [한국어] 데이터 배치 유형 */
enum {
	FIO_DP_NONE	= 0x0,       /* 데이터 배치 미사용 */
	FIO_DP_FDP	= 0x1,       /* NVMe FDP 사용 */
	FIO_DP_STREAMS	= 0x2,   /* 스트림 기반 배치 사용 */
};

/* [한국어] RUH(Reclaim Unit Handle) 정보 구조체
 * 디바이스에서 조회한 배치 가능한 RUH 목록을 저장한다. */
struct fio_ruhs_info {
	uint32_t nr_ruhs;    /* RUH 개수 */
	uint32_t pli_loc;    /* 현재 라운드 로빈 위치 인덱스 */
	uint16_t plis[];     /* Placement Identifier 배열 (가변 길이) */
};

/* [한국어] 스킴(scheme) 항목 - 오프셋 범위와 해당 배치 ID를 매핑 */
struct fio_ruhs_scheme_entry {
	unsigned long long start_offset;  /* 시작 오프셋 */
	unsigned long long end_offset;    /* 끝 오프셋 */
	uint16_t pli;                     /* 이 범위에 할당되는 Placement ID */
};

/* [한국어] 스킴(scheme) 구조체 - 오프셋 범위별 배치 ID 매핑 테이블 */
struct fio_ruhs_scheme {
	uint16_t nr_schemes;  /* 스킴 항목 수 */
	struct fio_ruhs_scheme_entry scheme_entries[DP_MAX_SCHEME_ENTRIES];
};

/* 데이터 배치 초기화 (각 파일에 대해 RUH 정보 및 스킴 설정) */
int dp_init(struct thread_data *td);

/* RUH 정보 및 스킴 메모리 해제 */
void fdp_free_ruhs_info(struct fio_file *f);

/* I/O 제출 시 dtype/dspec(배치 힌트) 값을 io_u에 설정 */
void dp_fill_dspec_data(struct thread_data *td, struct io_u *io_u);

#endif /* FIO_DATAPLACEMENT_H */
