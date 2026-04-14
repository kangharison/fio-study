/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */
/*
 * [한국어 설명] 블록 장치 구역(zoned) 인터페이스 헤더 (blkzoned.h)
 *
 * === 파일의 역할 ===
 * Linux 커널의 ZBD(Zoned Block Device) 인터페이스에 대한 fio의 추상화 레이어이다.
 * 구역 블록 장치(ZBD)는 SMR(Shingled Magnetic Recording) HDD나 ZNS SSD 등에서
 * 사용되는 장치로, 디스크를 구역(zone)으로 나누어 순차 쓰기를 강제한다.
 * CONFIG_HAS_BLKZONED가 정의되면 linux-blkzoned.c의 실제 구현을 사용하고,
 * 그렇지 않으면 -EIO를 반환하는 스텁 함수를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 ZBD 지원 체인:
 *   zbd.c (ZBD 정책 관리) → blkzoned.h → linux-blkzoned.c (Linux ioctl 호출)
 * I/O 엔진이 구역 블록 장치에 대해 I/O를 수행할 때 이 인터페이스를 통해
 * 구역 정보를 조회하고, 쓰기 포인터를 리셋하거나 이동한다.
 *
 * === 타 모듈과의 연결 ===
 * - zbd.c: 구역 블록 장치 정책 관리자 (이 헤더의 주요 호출자)
 * - oslib/linux-blkzoned.c: Linux 커널 ioctl 기반 실제 구현
 * - zbd_types.h: zbd_zone, zbd_zoned_model 등의 타입 정의
 *
 * === 주요 함수 요약 ===
 * - blkzoned_get_zoned_model(): 장치의 ZBD 모델(host-aware/host-managed) 조회
 * - blkzoned_report_zones(): 장치의 구역 정보 리포트
 * - blkzoned_reset_wp(): 쓰기 포인터(Write Pointer) 리셋
 * - blkzoned_finish_zone(): 구역 완료 처리
 * - blkzoned_get_max_open_zones(): 최대 동시 오픈 구역 수 조회
 * - blkzoned_get_max_active_zones(): 최대 활성 구역 수 조회
 * - blkzoned_move_zone_wp(): 쓰기 포인터 이동 (데이터 기록)
 */
#ifndef FIO_BLKZONED_H
#define FIO_BLKZONED_H

#include "zbd_types.h"

/* [한국어] CONFIG_HAS_BLKZONED가 정의된 경우: linux-blkzoned.c의 실제 구현 사용 */
#ifdef CONFIG_HAS_BLKZONED
extern int blkzoned_get_zoned_model(struct thread_data *td,
			struct fio_file *f, enum zbd_zoned_model *model);
extern int blkzoned_report_zones(struct thread_data *td,
				struct fio_file *f, uint64_t offset,
				struct zbd_zone *zones, unsigned int nr_zones);
extern int blkzoned_reset_wp(struct thread_data *td, struct fio_file *f,
				uint64_t offset, uint64_t length);
extern int blkzoned_move_zone_wp(struct thread_data *td, struct fio_file *f,
				 struct zbd_zone *z, uint64_t length,
				 const char *buf);
extern int blkzoned_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				       unsigned int *max_open_zones);
extern int blkzoned_get_max_active_zones(struct thread_data *td,
					 struct fio_file *f,
					 unsigned int *max_active_zones);
extern int blkzoned_finish_zone(struct thread_data *td, struct fio_file *f,
				uint64_t offset, uint64_t length);
#else
/* [한국어] ZBD를 지원하지 않는 시스템용 스텁 함수들 - 대부분 -EIO 반환 */
/*
 * Define stubs for systems that do not have zoned block device support.
 */
static inline int blkzoned_get_zoned_model(struct thread_data *td,
			struct fio_file *f, enum zbd_zoned_model *model)
{
	/*
	 * [한국어] 블록 장치 파일이면 ZBD 에뮬레이션을 허용 (모델은 NONE)
	 * 블록 장치가 아니면 -ENODEV 반환
	 */
	/*
	 * If this is a block device file, allow zbd emulation.
	 */
	if (f->filetype == FIO_TYPE_BLOCK) {
		*model = ZBD_NONE;
		return 0;
	}

	return -ENODEV;
}
static inline int blkzoned_report_zones(struct thread_data *td,
				struct fio_file *f, uint64_t offset,
				struct zbd_zone *zones, unsigned int nr_zones)
{
	return -EIO;
}
static inline int blkzoned_reset_wp(struct thread_data *td, struct fio_file *f,
				    uint64_t offset, uint64_t length)
{
	return -EIO;
}
static inline int blkzoned_move_zone_wp(struct thread_data *td,
					struct fio_file *f, struct zbd_zone *z,
					uint64_t length, const char *buf)
{
	return -EIO;
}
static inline int blkzoned_get_max_open_zones(struct thread_data *td, struct fio_file *f,
					      unsigned int *max_open_zones)
{
	return -EIO;
}
static inline int blkzoned_get_max_active_zones(struct thread_data *td,
						struct fio_file *f,
						unsigned int *max_open_zones)
{
	return -EIO;
}
static inline int blkzoned_finish_zone(struct thread_data *td,
				       struct fio_file *f,
				       uint64_t offset, uint64_t length)
{
	return -EIO;
}
#endif

#endif /* FIO_BLKZONED_H */
