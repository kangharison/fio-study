/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */
/*
 * [한국어 설명] Zoned Block Device(ZBD) 운영체제 추상화 헤더 (blkzoned.h)
 *
 * === 파일의 역할 ===
 * Linux 커널의 ZBD(Zoned Block Device) UAPI(<linux/blkzoned.h> — BLKREPORTZONE,
 * BLKRESETZONE, BLKOPENZONE, BLKCLOSEZONE, BLKFINISHZONE, BLKGETZONESZ,
 * BLKGETNRZONES, BLK_ZONE_COND_*, BLK_ZONE_TYPE_*)와 fio 의 ZBD 코어(zbd.c)
 * 사이 경량 어댑터 레이어이다. 호스트 관리형(Host-Managed, HM) SMR HDD 와
 * NVMe ZNS, 호스트 인식형(Host-Aware) SMR 디스크를 모두 지원한다.
 *
 * CONFIG_HAS_BLKZONED 가 정의되면 linux-blkzoned.c 의 실제 ioctl 래퍼가 링크되고,
 * 미정의(FreeBSD/macOS/Windows 등) 시에는 본 헤더의 static inline 스텁이 대부분
 * -EIO 를 반환하여 컴파일만 통과시킨다. 단 blkzoned_get_zoned_model() 스텁은
 * 블록 디바이스 파일형에 대해 ZBD_NONE(일반 블록 디바이스) 을 반환하도록 하여
 * ZBD 에뮬레이션 경로를 계속 허용한다.
 *
 * 핵심 개념:
 *   - zone: 디스크 영역(보통 256 MiB). 각 zone 은 write pointer(WP) 를 가지며
 *          쓰기는 WP 위치에서만 허용(순차 쓰기 강제). 읽기는 자유.
 *   - zone 상태(ZBD_ZONE_COND_*): EMPTY / IMP_OPEN / EXP_OPEN / CLOSED / FULL /
 *     READONLY / OFFLINE / NOT_WP.
 *   - zone 타입(ZBD_ZONE_TYPE_*): CONVENTIONAL(일반) / SEQWRITE_REQ(순차 필수) /
 *     SEQWRITE_PREF(순차 선호).
 *   - 제약: max_open_zones(동시 OPEN 상태) / max_active_zones(OPEN + CLOSED).
 *
 * === 전체 아키텍처에서의 위치 ===
 * ZBD 지원 스택:
 *   engines/{io_uring,libaio,sync,sg,libzbc,xnvme}.c → zbd.c (정책 레이어)
 *                                                   ↓
 *                                             blkzoned.h (본 헤더)
 *                                                   ↓
 *                              linux-blkzoned.c (CONFIG_HAS_BLKZONED) 또는 스텁
 *                                                   ↓
 *                                   ioctl(BLKREPORTZONE/BLKRESETZONE/...)
 *
 * 본 헤더의 함수 시그니처는 io 엔진들의 ioengine_ops 콜백(get_zoned_model,
 * report_zones, reset_wp, finish_zone, get_max_open_zones, move_zone_wp)과
 * 1:1 매핑되어 블록 디바이스 경로에서 그대로 위임 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - zbd.c: fio 의 ZBD 정책 관리자(이 헤더의 최대 소비자).
 * - zbd_types.h: 공용 zbd_zone/zbd_zoned_model/zbd_zone_cond/zbd_zone_type 정의.
 * - oslib/linux-blkzoned.c: 실제 ioctl 기반 구현.
 * - engines/io_uring.c: fio_ioring_get_zoned_model/report_zones/reset_wp/...
 *   블록 경로에서 본 헤더 함수들을 직접 위임 호출.
 * - engines/libzbc.c: libzbc 라이브러리 사용 — 본 헤더 미경유 분리 경로.
 * - engines/xnvme.c: NVMe ZNS 용 libxnvme 경유 — 본 헤더 미경유.
 *
 * === 주요 함수/구조체 요약 ===
 * - blkzoned_get_zoned_model(): 디바이스의 ZBD 모델 판별(NONE/HA/HM).
 * - blkzoned_report_zones(): 지정 영역의 zone 정보 배열 조회(BLKREPORTZONE).
 * - blkzoned_reset_wp(): 해당 영역 zone 의 WP 리셋(BLKRESETZONE).
 * - blkzoned_finish_zone(): zone 을 FULL 상태로 전환(BLKFINISHZONE).
 * - blkzoned_get_max_open_zones(): 동시 OPEN 가능 zone 수 조회.
 * - blkzoned_get_max_active_zones(): 동시 ACTIVE(OPEN+CLOSED) zone 수 조회.
 * - blkzoned_move_zone_wp(): 데이터를 zone 끝으로 추가 기록(내부적으로 pwrite).
 */
#ifndef FIO_BLKZONED_H
/* [한국어] 헤더 가드 — zbd.c / 엔진들이 동시 포함 가능. */
#define FIO_BLKZONED_H

#include "zbd_types.h"
/* [한국어] "zbd_types.h" 포함 이유: struct zbd_zone, enum zbd_zoned_model
 * (ZBD_NONE/ZBD_HOST_AWARE/ZBD_HOST_MANAGED), enum zbd_zone_cond, enum
 * zbd_zone_type, struct thread_data, struct fio_file 등 공용 타입 공급.
 * 본 헤더의 모든 함수 시그니처가 이 타입들을 사용한다. */

#ifdef CONFIG_HAS_BLKZONED
/* [한국어] CONFIG_HAS_BLKZONED: <linux/blkzoned.h> UAPI 와 BLKREPORTZONE 등
 * ioctl 이 사용 가능한 Linux 빌드. 실제 구현은 oslib/linux-blkzoned.c 에 있다. */

/*
 * [한국어]
 * blkzoned_get_zoned_model - 디바이스의 ZBD 모델(Host-Aware/Host-Managed/NONE) 조회
 * @td: 잡 컨텍스트(에러 로깅·mmap 경로용).
 * @f:  대상 파일/디바이스.
 * @model: 결과 출력(ZBD_NONE/ZBD_HOST_AWARE/ZBD_HOST_MANAGED).
 * @return: 성공 0, 실패 음수 errno.
 * 구현: /sys/block/<dev>/queue/zoned 파일 읽기 + BLKGETZONESZ 시도.
 */
extern int blkzoned_get_zoned_model(struct thread_data *td,
			struct fio_file *f, enum zbd_zoned_model *model);

/*
 * [한국어]
 * blkzoned_report_zones - 지정 오프셋부터 nr_zones 개의 zone 정보 조회
 * @td: 잡 컨텍스트. @f: 대상 디바이스. @offset: 조회 시작 바이트 오프셋.
 * @zones: 결과를 받을 zbd_zone 배열(nr_zones 개 크기).
 * @nr_zones: 최대 요청 zone 수(커널이 더 적게 반환할 수 있음).
 * @return: 실제 채워진 zone 수(>=0), 실패 시 음수.
 * 구현: BLKREPORTZONE ioctl → struct blk_zone_report[] → zbd_zone 변환.
 */
extern int blkzoned_report_zones(struct thread_data *td,
				struct fio_file *f, uint64_t offset,
				struct zbd_zone *zones, unsigned int nr_zones);

/*
 * [한국어]
 * blkzoned_reset_wp - 지정 영역의 zone WP 리셋(해당 zone 들이 EMPTY 로)
 * @offset: 영역 시작(반드시 zone 경계). @length: 바이트 길이(반드시 zone 배수).
 * length = 디바이스 전체 크기면 ALL_ZONES 최적화 경로(단일 BLKRESETZONE 호출).
 * 구현: BLKRESETZONE ioctl.
 */
extern int blkzoned_reset_wp(struct thread_data *td, struct fio_file *f,
				uint64_t offset, uint64_t length);

/*
 * [한국어]
 * blkzoned_move_zone_wp - 지정 zone 의 WP 위치에 length 바이트 기록하여 WP 이동
 * @z: 대상 zone 디스크립터(WP 읽기/쓰기 기준점).
 * @buf: 쓸 데이터(zone 의 최소 I/O 단위 정렬 필요 — NVMe ZNS 는 512B/4KiB).
 * 구현: lseek(WP) + write(length) 또는 pwrite 로 순차 쓰기 강제 준수.
 * 용도: zbd.c 가 zone 을 미리 채워둘(pre-fill) 때 사용.
 */
extern int blkzoned_move_zone_wp(struct thread_data *td, struct fio_file *f,
				 struct zbd_zone *z, uint64_t length,
				 const char *buf);

/*
 * [한국어]
 * blkzoned_get_max_open_zones - 동시 OPEN 가능 zone 수 상한 조회
 * 구현: /sys/block/<dev>/queue/max_open_zones 파싱 (0 은 제한 없음).
 * 잡 설정에서 max_open_zones 옵션과 비교 검증에 사용.
 */
extern int blkzoned_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				       unsigned int *max_open_zones);

/*
 * [한국어]
 * blkzoned_get_max_active_zones - 동시 ACTIVE(OPEN+CLOSED) zone 수 상한 조회
 * 구현: /sys/block/<dev>/queue/max_active_zones 파싱.
 */
extern int blkzoned_get_max_active_zones(struct thread_data *td,
					 struct fio_file *f,
					 unsigned int *max_active_zones);

/*
 * [한국어]
 * blkzoned_finish_zone - 지정 zone 을 FULL 상태로 강제 전환
 * @offset: zone 경계. @length: 보통 zone 크기 배수(단일 zone 이면 zone_size).
 * 구현: BLKFINISHZONE ioctl — WP 를 zone 끝으로 이동, 이후 쓰기 불가.
 * 용도: zbd.c 가 writefinish 훅에서 호출, 또는 잡 종료 시 정리.
 */
extern int blkzoned_finish_zone(struct thread_data *td, struct fio_file *f,
				uint64_t offset, uint64_t length);
#else
/*
 * Define stubs for systems that do not have zoned block device support.
 */
/* [한국어] CONFIG_HAS_BLKZONED 미정의 플랫폼(FreeBSD/macOS/Windows/일부 임베디드):
 * ZBD 미지원이므로 대부분의 호출을 -EIO 로 조기 실패시킨다.
 * 예외: get_zoned_model 은 블록 디바이스에 대해 ZBD_NONE 을 반환해 일반 블록
 * 경로가 계속 동작하도록 허용(ZBD 모드가 아닌 잡은 정상 실행). */

static inline int blkzoned_get_zoned_model(struct thread_data *td,
			struct fio_file *f, enum zbd_zoned_model *model)
{
	/*
	 * If this is a block device file, allow zbd emulation.
	 */
	/* [한국어] 블록 디바이스(FIO_TYPE_BLOCK) 면 ZBD_NONE 반환 → 일반 블록 경로 허용.
	 * 그 외(FILE/CHAR 등)는 -ENODEV 로 ZBD 에뮬레이션 불가 표시. */
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
	/* [한국어] ZBD 미지원 플랫폼 — zone 정보 획득 불가. -EIO 반환. */
	return -EIO;
}
static inline int blkzoned_reset_wp(struct thread_data *td, struct fio_file *f,
				    uint64_t offset, uint64_t length)
{
	/* [한국어] WP 리셋 ioctl 없음 — -EIO. */
	return -EIO;
}
static inline int blkzoned_move_zone_wp(struct thread_data *td,
					struct fio_file *f, struct zbd_zone *z,
					uint64_t length, const char *buf)
{
	/* [한국어] WP 이동(순차 쓰기) 지원 불가 — -EIO. */
	return -EIO;
}
static inline int blkzoned_get_max_open_zones(struct thread_data *td, struct fio_file *f,
					      unsigned int *max_open_zones)
{
	/* [한국어] sysfs max_open_zones 없음 — -EIO. */
	return -EIO;
}
static inline int blkzoned_get_max_active_zones(struct thread_data *td,
						struct fio_file *f,
						unsigned int *max_open_zones)
{
	/* [한국어] sysfs max_active_zones 없음 — -EIO. */
	return -EIO;
}
static inline int blkzoned_finish_zone(struct thread_data *td,
				       struct fio_file *f,
				       uint64_t offset, uint64_t length)
{
	/* [한국어] BLKFINISHZONE 없음 — -EIO. */
	return -EIO;
}
#endif

#endif /* FIO_BLKZONED_H */
