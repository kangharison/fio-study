/*
 * Copyright (C) 2018 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */
/*
 * [한국어] zbd.h - Zoned Block Device(ZBD) 지원 헤더
 *
 * 이 파일은 ZBD(NVMe ZNS, SMR HDD 등)를 위한 fio 확장을 정의한다.
 * 주요 내용:
 *   1) fio_zone_info          - 개별 존 정보 (시작 위치, 쓰기 포인터, 상태 등)
 *   2) zoned_block_device_info - ZBD 전체 특성 (존 크기, 존 수, 활성 존 제한 등)
 *   3) io_u_action            - I/O 조정 결과 (수락, EOF, 완료)
 *   4) ZBD API 함수           - 초기화, I/O 조정, 존 리셋, 에러 복구 등
 *
 * ZBD의 핵심 개념:
 *   - 존(Zone): 디바이스를 고정 크기 영역으로 분할, 각 존은 순차 쓰기만 허용
 *   - 쓰기 포인터(WP): 다음 쓰기 위치를 추적, 반드시 WP 위치에서만 쓰기 가능
 *   - 존 리셋: WP를 존 시작으로 되돌림 (데이터 삭제)
 *   - 활성 존 제한: 동시에 쓸 수 있는 존 수가 디바이스에 의해 제한
 
 * === 파일의 역할 ===
 * ZBD(NVMe ZNS, SMR HDD)를 위한 fio 확장을 정의한다. fio_zone_info(존 정보),
 * zoned_block_device_info(ZBD 특성), io_u_action(I/O 조정 결과), ZBD API를 포함.
 *
 * === 전체 아키텍처에서의 위치 ===
 * zbd.c와 짝을 이루는 헤더. backend.c, io_u.c에서 ZBD 관련 함수 호출 시 참조.
 *
 * === 타 모듈과의 연결 ===
 * - zbd.c: 이 헤더의 함수 구현
 * - backend.c: do_io()에서 zbd_adjust_block() 호출
 * - oslib/blkzoned.h: OS별 ZBD 인터페이스
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_zone_info: 개별 존 정보 (시작, WP, 상태)
 * - struct zoned_block_device_info: ZBD 전체 특성 (존 크기, 수, 활성 제한)
 * - zbd_adjust_block(): I/O를 존 제약에 맞게 조정
 */
#ifndef FIO_ZBD_H
#define FIO_ZBD_H

#include "io_u.h"              /* I/O 유닛 구조체 */
#include "ioengines.h"         /* I/O 엔진 인터페이스 */
#include "oslib/blkzoned.h"    /* OS별 ZBD 인터페이스 */
#include "zbd_types.h"         /* ZBD 타입 정의 (존 타입, 조건 등) */

struct fio_file;

/* [한국어] I/O 조정 결과 — zbd_adjust_block()의 반환값 */
enum io_u_action {
	io_u_accept	= 0,    /* I/O를 그대로 수행 */
	io_u_eof	= 1,    /* 파일 끝 도달 (더 이상 I/O 불가) */
	io_u_completed  = 2,    /* I/O가 이미 완료됨 (존 리셋 등으로 처리) */
};

/**
 * [한국어] 개별 존 정보 구조체
 *
 * ZBD의 각 존에 대한 메타데이터를 추적한다.
 * 존 뮤텍스(mutex)로 보호되며, 여러 스레드가 동시에 같은 존에 접근할 때 동기화한다.
 * 비트 필드를 사용하여 메모리를 절약한다 (수만 개의 존이 존재할 수 있으므로).
 */
struct fio_zone_info {
	pthread_mutex_t		mutex;              /* 존 접근 동기화 뮤텍스 */
	uint64_t		start;              /* 존 시작 오프셋 (바이트) */
	uint64_t		wp;                 /* 쓰기 포인터 위치 (바이트) */
	uint64_t		capacity;           /* 존 사용 가능 용량 (존 크기와 다를 수 있음) */
	uint32_t		writes_in_flight;   /* 현재 진행 중인 쓰기 수 */
	uint32_t		max_write_error_offset; /* 쓰기 실패한 최대 오프셋 */
	enum zbd_zone_type	type:2;             /* 존 타입: 순차/컨벤셔널 */
	enum zbd_zone_cond	cond:4;             /* 존 상태: empty/open/closed/full 등 */
	unsigned int		has_wp:1;           /* 쓰기 포인터를 가지는 존인지 */
	unsigned int		write:1;            /* 현재 쓰기 대상 존인지 */
	unsigned int		reset_zone:1;       /* 쓰기 전 리셋이 필요한지 */
	unsigned int		fixing_zone_wp:1;   /* 쓰기 포인터 복구 진행 중인지 */
};

/**
 * zoned_block_device_info - zoned block device characteristics
 * @model: Device model.
 * @max_write_zones: global limit on the number of sequential write zones which
 *      are simultaneously written. A zero value means unlimited zones of
 *      simultaneous writes and that write target zones will not be tracked in
 *      the write_zones array.
 * @max_active_zones: device side limit on the number of sequential write zones
 *	in open or closed conditions. A zero value means unlimited number of
 *	zones in the conditions.
 * @mutex: Protects the modifiable members in this structure (refcount and
 *		num_open_zones).
 * @zone_size: size of a single zone in bytes.
 * @wp_valid_data_bytes: total size of data in zones with write pointers
 * @write_min_zone: Minimum zone index of all job's write ranges. Inclusive.
 * @write_max_zone: Maximum zone index of all job's write ranges. Exclusive.
 * @zone_size_log2: log2 of the zone size in bytes if it is a power of 2 or 0
 *		if the zone size is not a power of 2.
 * @nr_zones: number of zones
 * @refcount: number of fio files that share this structure
 * @num_write_zones: number of write target zones
 * @write_cnt: Number of writes since the latest zone reset triggered by
 *	       the zone_reset_frequency fio job parameter.
 * @write_zones: zone numbers of write target zones
 * @zone_info: description of the individual zones
 *
 * Only devices for which all zones have the same size are supported.
 * Note: if the capacity is not a multiple of the zone size then the last zone
 * will be smaller than 'zone_size'.
 */
/*
 * [한국어] ZBD 전체 특성 구조체
 *
 * 디바이스 단위의 ZBD 메타데이터를 관리한다.
 * 같은 디바이스를 사용하는 여러 fio_file이 refcount로 공유한다.
 * zone_info[]는 가변 길이 배열(flex array)로 존 수만큼 할당된다.
 */
struct zoned_block_device_info {
	enum zbd_zoned_model	model;              /* 디바이스 모델: none/host-aware/host-managed */
	uint32_t		max_write_zones;    /* 동시 쓰기 가능한 최대 존 수 (0=무제한) */
	uint32_t		max_active_zones;   /* 디바이스의 활성 존 제한 (0=무제한) */
	pthread_mutex_t		mutex;              /* ZBD 구조체 접근 동기화 */
	uint64_t		zone_size;          /* 존 크기 (바이트) */
	uint64_t		wp_valid_data_bytes;/* WP가 있는 존들의 총 유효 데이터 크기 */
	uint32_t		write_min_zone;     /* 쓰기 범위 최소 존 인덱스 (포함) */
	uint32_t		write_max_zone;     /* 쓰기 범위 최대 존 인덱스 (제외) */
	uint32_t		zone_size_log2;     /* 존 크기가 2의 거듭제곱이면 log2, 아니면 0 */
	uint32_t		nr_zones;           /* 전체 존 수 */
	uint32_t		refcount;           /* 이 구조체를 공유하는 fio_file 수 */
	uint32_t		num_write_zones;    /* 현재 쓰기 대상 존 수 */
	uint32_t		write_cnt;          /* zone_reset_frequency 이후 쓰기 횟수 */
	uint32_t		write_zones[ZBD_MAX_WRITE_ZONES]; /* 쓰기 대상 존 번호 배열 */
	struct fio_zone_info	zone_info[0];       /* 개별 존 정보 (가변 길이 배열) */
};

/* [한국어] ZBD API 함수들 */
int zbd_init_files(struct thread_data *td);
		/* 파일의 ZBD 모델 감지 및 존 정보 로드 */
void zbd_recalc_options_with_zone_granularity(struct thread_data *td);
		/* 옵션 값을 존 경계에 맞게 재조정 */
int zbd_setup_files(struct thread_data *td);
		/* ZBD 파일 셋업 — 존 상태 확인, 쓰기 대상 존 초기화 */
void zbd_free_zone_info(struct fio_file *f);
		/* 존 정보 메모리 해제 */
void zbd_file_reset(struct thread_data *td, struct fio_file *f);
		/* 파일의 모든 쓰기 존 리셋 */
bool zbd_unaligned_write(int error_code);
		/* 에러 코드가 비정렬 쓰기인지 판별 */
void setup_zbd_zone_mode(struct thread_data *td, struct io_u *io_u);
		/* zonemode=strided 시 I/O 범위를 존 경계에 맞게 조정 */
enum fio_ddir zbd_adjust_ddir(struct thread_data *td, struct io_u *io_u,
			      enum fio_ddir ddir);
		/* I/O 방향 조정 — 빈 존에 대한 읽기를 쓰기로 전환 등 */
enum io_u_action zbd_adjust_block(struct thread_data *td, struct io_u *io_u);
		/* I/O 오프셋/크기를 존 제약에 맞게 조정 (핵심 함수) */
char *zbd_write_status(const struct thread_stat *ts);
		/* ZBD 쓰기 상태 문자열 생성 */
int zbd_do_io_u_trim(struct thread_data *td, struct io_u *io_u);
		/* 존 리셋으로 트림 수행 */
void zbd_log_err(const struct thread_data *td, const struct io_u *io_u);
		/* ZBD 관련 I/O 에러 로깅 */
void zbd_recover_write_error(struct thread_data *td, struct io_u *io_u);
		/* 쓰기 실패 후 존 상태 복구 */

/* [한국어] 파일 닫기 시 ZBD 정보 해제 */
static inline void zbd_close_file(struct fio_file *f)
{
	if (f->zbd_info)
		zbd_free_zone_info(f);
}

/* [한국어] I/O 큐잉 시 ZBD 후처리 — 쓰기 포인터 업데이트 등 */
static inline void zbd_queue_io_u(struct thread_data *td, struct io_u *io_u,
				  enum fio_q_status *status)
{
	if (io_u->zbd_queue_io) {
		io_u->zbd_queue_io(td, io_u, (int *)status);
		io_u->zbd_queue_io = NULL;
	}
}

/* [한국어] I/O 완료/취소 시 ZBD 정리 — writes_in_flight 감소 등 */
static inline void zbd_put_io_u(struct thread_data *td, struct io_u *io_u)
{
	if (io_u->zbd_put_io) {
		io_u->zbd_put_io(td, io_u);
		io_u->zbd_queue_io = NULL;
		io_u->zbd_put_io = NULL;
	}
}

#endif /* FIO_ZBD_H */
