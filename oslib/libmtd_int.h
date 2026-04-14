/*
 * Copyright (c) International Business Machines Corp., 2006
 * Copyright (C) 2009 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Author: Artem Bityutskiy
 *
 * MTD library.
 */

/* Imported from mtd-utils by dehrenberg */

/*
 * [한국어 설명] MTD 라이브러리 내부 데이터 구조체 헤더 (libmtd_int.h)
 *
 * === 파일의 역할 ===
 * MTD 라이브러리의 내부 구현에서만 사용되는 데이터 구조체와 상수를 정의한다.
 * struct libmtd가 핵심으로, sysfs 파일 경로 패턴들과 64비트 ioctl 지원 여부를
 * 추적한다. 공개 API에서는 libmtd_t(void*)로 은닉된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * libmtd.c와 libmtd_legacy.c에서 내부적으로 사용되며,
 * 공개 API 사용자(engines/mtd.c)에게는 노출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - libmtd.c: 이 구조체를 사용하는 주요 구현 파일
 * - libmtd_legacy.c: sysfs 미지원 시 /proc/mtd 기반 레거시 함수 선언
 * - libmtd.h: 공개 API (libmtd_t를 통해 간접 참조)
 *
 * === 주요 구조체 요약 ===
 * - struct libmtd: MTD 라이브러리 내부 상태 (sysfs 경로 패턴, ioctl 지원 여부)
 * - SYSFS_MTD 등 상수: /sys/class/mtd 하위 파일명 정의
 * - legacy_* 함수: sysfs 미지원 커널용 레거시 API 선언
 */
#ifndef __LIBMTD_INT_H__
#define __LIBMTD_INT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PROGRAM_NAME "libmtd"

/* [한국어] sysfs의 MTD 관련 파일 경로 및 속성명 상수 */
/* [한국어] /sys/class/mtd 디렉토리 하위의 각 속성 파일 이름 */
#define SYSFS_MTD        "class/mtd"
#define MTD_NAME_PATT    "mtd%d"
#define MTD_DEV          "dev"
#define MTD_NAME         "name"
#define MTD_TYPE         "type"
#define MTD_EB_SIZE      "erasesize"
#define MTD_SIZE         "size"
#define MTD_MIN_IO_SIZE  "writesize"
#define MTD_SUBPAGE_SIZE "subpagesize"
#define MTD_OOB_SIZE     "oobsize"
#define MTD_REGION_CNT   "numeraseregions"
#define MTD_FLAGS        "flags"

/* [한국어] 64비트 오프셋 ioctl 지원 상태 - 커널 2.6.31+에서 추가됨 */
#define OFFS64_IOCTLS_UNKNOWN       0  /* [한국어] 아직 확인되지 않음 */
#define OFFS64_IOCTLS_NOT_SUPPORTED 1  /* [한국어] 지원하지 않음 (구형 커널) */
#define OFFS64_IOCTLS_SUPPORTED     2  /* [한국어] 지원함 */

/**
 * libmtd - MTD library description data structure.
 * @sysfs_mtd: MTD directory in sysfs
 * @mtd: MTD device sysfs directory pattern
 * @mtd_dev: MTD device major/minor numbers file pattern
 * @mtd_name: MTD device name file pattern
 * @mtd_type: MTD device type file pattern
 * @mtd_eb_size: MTD device eraseblock size file pattern
 * @mtd_size: MTD device size file pattern
 * @mtd_min_io_size: minimum I/O unit size file pattern
 * @mtd_subpage_size: sub-page size file pattern
 * @mtd_oob_size: MTD device OOB size file pattern
 * @mtd_region_cnt: count of additional erase regions file pattern
 * @mtd_flags: MTD device flags file pattern
 * @sysfs_supported: non-zero if sysfs is supported by MTD
 * @offs64_ioctls: %OFFS64_IOCTLS_SUPPORTED if 64-bit %MEMERASE64,
 *                 %MEMREADOOB64, %MEMWRITEOOB64 MTD device ioctls are
 *                 supported, %OFFS64_IOCTLS_NOT_SUPPORTED if not, and
 *                 %OFFS64_IOCTLS_UNKNOWN if it is not known yet;
 *
 *  Note, we cannot find out whether 64-bit ioctls are supported by MTD when we
 *  are initializing the library, because this requires an MTD device node.
 *  Indeed, we have to actually call the ioctl and check for %ENOTTY to find
 *  out whether it is supported or not.
 *
 *  Thus, we leave %offs64_ioctls uninitialized in 'libmtd_open()', and
 *  initialize it later, when corresponding libmtd function is used, and when
 *  we actually have a device node and can invoke an ioctl command on it.
 */
struct libmtd
{
	char *sysfs_mtd;
	char *mtd;
	char *mtd_dev;
	char *mtd_name;
	char *mtd_type;
	char *mtd_eb_size;
	char *mtd_size;
	char *mtd_min_io_size;
	char *mtd_subpage_size;
	char *mtd_oob_size;
	char *mtd_region_cnt;
	char *mtd_flags;
	unsigned int sysfs_supported:1;
	unsigned int offs64_ioctls:2;
};

/* [한국어] sysfs 미지원 구형 커널(2.6.30 미만)용 레거시 함수 선언 */
/* [한국어] /proc/mtd 파일을 파싱하여 MTD 정보를 얻는 대체 구현 */
int legacy_libmtd_open(void);
int legacy_dev_present(int mtd_num);
int legacy_mtd_get_info(struct mtd_info *info);
int legacy_get_dev_info(const char *node, struct mtd_dev_info *mtd);
int legacy_get_dev_info1(int dev_num, struct mtd_dev_info *mtd);

#ifdef __cplusplus
}
#endif

#endif /* !__LIBMTD_INT_H__ */
