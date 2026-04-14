/*
 * Copyright (C) 2008, 2009 Nokia Corporation
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
 * [한국어 설명] MTD 라이브러리 공개 API 헤더 (libmtd.h)
 *
 * === 파일의 역할 ===
 * MTD(Memory Technology Device) 라이브러리의 공개 인터페이스를 정의한다.
 * MTD는 플래시 메모리(NOR, NAND, DataFlash 등)를 다루기 위한 Linux 커널 서브시스템이다.
 * 이 라이브러리는 MTD 장치의 정보 조회, 읽기/쓰기, 소거, 잠금/해제, 배드 블록 관리 등
 * 다양한 작업을 사용자 공간에서 수행할 수 있게 한다.
 * mtd-utils 패키지에서 가져온(import) 코드이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 MTD I/O 엔진(engines/mtd.c)에서 사용된다:
 *   engines/mtd.c → libmtd.h API → libmtd.c 구현 → ioctl/sysfs
 *
 * === 타 모듈과의 연결 ===
 * - engines/mtd.c: MTD I/O 엔진 (이 라이브러리의 주요 사용자)
 * - libmtd.c: 공개 API의 주요 구현
 * - libmtd_int.h: 내부 데이터 구조체
 * - libmtd_legacy.c: 구형 커널 지원 (sysfs 미지원)
 *
 * === 주요 구조체/함수 요약 ===
 * - struct mtd_info: 시스템의 MTD 장치 전체 정보 (장치 수, 번호 범위)
 * - struct mtd_dev_info: 개별 MTD 장치 정보 (크기, 소거블록 크기, 타입 등)
 * - libmtd_open()/close(): 라이브러리 초기화/종료
 * - mtd_read()/write(): 데이터 읽기/쓰기
 * - mtd_erase(): 소거블록(eraseblock) 소거
 * - mtd_is_bad()/mark_bad(): 배드 블록 확인/표시
 */
#ifndef __LIBMTD_H__
#define __LIBMTD_H__

#ifdef __cplusplus
extern "C" {
#endif

// Needed for uint8_t, uint64_t
#include <stdint.h>

/* [한국어] MTD 장치 이름의 최대 길이 */
/* Maximum MTD device name length */
#define MTD_NAME_MAX 127
/* [한국어] MTD 장치 타입 문자열의 최대 길이 (예: "nand", "nor") */
/* Maximum MTD device type string length */
#define MTD_TYPE_MAX 64

/* [한국어] MTD 라이브러리 디스크립터 - 불투명(opaque) 포인터로 내부적으로 struct libmtd */
/* MTD library descriptor */
typedef void * libmtd_t;

/* Forward decls */
struct region_info_user;

/*
 * [한국어] 시스템의 MTD 장치 전체 요약 정보
 * mtd_get_info()로 채워지며 시스템에 몇 개의 MTD 장치가 있는지,
 * 번호 범위가 어떻게 되는지를 파악하는 데 사용된다.
 */
/**
 * @mtd_dev_cnt: count of MTD devices in system
 * @lowest_mtd_num: lowest MTD device number in system
 * @highest_mtd_num: highest MTD device number in system
 * @sysfs_supported: non-zero if sysfs is supported by MTD
 */
struct mtd_info
{
	int mtd_dev_cnt;
	int lowest_mtd_num;
	int highest_mtd_num;
	unsigned int sysfs_supported:1;
};

/*
 * [한국어] 개별 MTD 장치의 상세 정보 구조체
 * mtd_get_dev_info()로 채워지며, 플래시 타입, 크기, 소거블록 크기,
 * 최소 I/O 단위, OOB(Out-Of-Band) 크기 등 장치의 물리적 특성을 담는다.
 * eb_size: 소거블록 크기 - NAND 플래시에서 소거 가능한 최소 단위
 * min_io_size: 최소 I/O 단위 - 보통 페이지 크기 (NAND에서 2K/4K)
 * oob_size: OOB 영역 크기 - ECC, 메타데이터 저장용 추가 영역
 */
/**
 * struct mtd_dev_info - information about an MTD device.
 * @mtd_num: MTD device number
 * @major: major number of corresponding character device
 * @minor: minor number of corresponding character device
 * @type: flash type (constants like %MTD_NANDFLASH defined in mtd-abi.h)
 * @type_str: static R/O flash type string
 * @name: device name
 * @size: device size in bytes
 * @eb_cnt: count of eraseblocks
 * @eb_size: eraseblock size
 * @min_io_size: minimum input/output unit size
 * @subpage_size: sub-page size
 * @oob_size: OOB size (zero if the device does not have OOB area)
 * @region_cnt: count of additional erase regions
 * @writable: zero if the device is read-only
 * @bb_allowed: non-zero if the MTD device may have bad eraseblocks
 */
struct mtd_dev_info
{
	int mtd_num;
	int major;
	int minor;
	int type;
	char type_str[MTD_TYPE_MAX + 1];
	char name[MTD_NAME_MAX + 1];
	long long size;
	int eb_cnt;
	int eb_size;
	int min_io_size;
	int subpage_size;
	int oob_size;
	int region_cnt;
	unsigned int writable:1;
	unsigned int bb_allowed:1;
};

/**
 * libmtd_open - open MTD library.
 *
 * This function initializes and opens the MTD library and returns MTD library
 * descriptor in case of success and %NULL in case of failure. In case of
 * failure, errno contains zero if MTD is not present in the system, or
 * contains the error code if a real error happened.
 */
libmtd_t libmtd_open(void);

/**
 * libmtd_close - close MTD library.
 * @desc: MTD library descriptor
 */
void libmtd_close(libmtd_t desc);

/**
 * mtd_dev_present - check whether a MTD device is present.
 * @desc: MTD library descriptor
 * @mtd_num: MTD device number to check
 *
 * This function returns %1 if MTD device is present and %0 if not.
 */
int mtd_dev_present(libmtd_t desc, int mtd_num);

/**
 * mtd_get_info - get general MTD information.
 * @desc: MTD library descriptor
 * @info: the MTD device information is returned here
 *
 * This function fills the passed @info object with general MTD information and
 * returns %0 in case of success and %-1 in case of failure. If MTD subsystem is
 * not present in the system, errno is set to @ENODEV.
 */
int mtd_get_info(libmtd_t desc, struct mtd_info *info);

/**
 * mtd_get_dev_info - get information about an MTD device.
 * @desc: MTD library descriptor
 * @node: name of the MTD device node
 * @mtd: the MTD device information is returned here
 *
 * This function gets information about MTD device defined by the @node device
 * node file and saves this information in the @mtd object. Returns %0 in case
 * of success and %-1 in case of failure. If MTD subsystem is not present in the
 * system, or the MTD device does not exist, errno is set to @ENODEV.
 */
int mtd_get_dev_info(libmtd_t desc, const char *node, struct mtd_dev_info *mtd);

/**
 * mtd_get_dev_info1 - get information about an MTD device.
 * @desc: MTD library descriptor
 * @mtd_num: MTD device number to fetch information about
 * @mtd: the MTD device information is returned here
 *
 * This function is identical to 'mtd_get_dev_info()' except that it accepts
 * MTD device number, not MTD character device.
 */
int mtd_get_dev_info1(libmtd_t desc, int mtd_num, struct mtd_dev_info *mtd);

/**
 * mtd_lock - lock eraseblocks.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to lock
 *
 * This function locks eraseblock @eb. Returns %0 in case of success and %-1
 * in case of failure.
 */
int mtd_lock(const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_unlock - unlock eraseblocks.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to lock
 *
 * This function unlocks eraseblock @eb. Returns %0 in case of success and %-1
 * in case of failure.
 */
int mtd_unlock(const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_erase - erase an eraseblock.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to erase
 *
 * This function erases eraseblock @eb of MTD device described by @fd. Returns
 * %0 in case of success and %-1 in case of failure.
 */
int mtd_erase(libmtd_t desc, const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_regioninfo - get information about an erase region.
 * @fd: MTD device node file descriptor
 * @regidx: index of region to look up
 * @reginfo: the region information is returned here
 *
 * This function gets information about an erase region defined by the
 * @regidx index and saves this information in the @reginfo object.
 * Returns %0 in case of success and %-1 in case of failure. If the
 * @regidx is not valid or unavailable, errno is set to @ENODEV.
 */
int mtd_regioninfo(int fd, int regidx, struct region_info_user *reginfo);

/**
 * mtd_is_locked - see if the specified eraseblock is locked.
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to check
 *
 * This function checks to see if eraseblock @eb of MTD device described
 * by @fd is locked. Returns %0 if it is unlocked, %1 if it is locked, and
 * %-1 in case of failure. If the ioctl is not supported (support was added in
 * Linux kernel 2.6.36) or this particular device does not support it, errno is
 * set to @ENOTSUPP.
 */
int mtd_is_locked(const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_torture - torture an eraseblock.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to torture
 *
 * This function tortures eraseblock @eb. Returns %0 in case of success and %-1
 * in case of failure.
 */
int mtd_torture(libmtd_t desc, const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_is_bad - check if eraseblock is bad.
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to check
 *
 * This function checks if eraseblock @eb is bad. Returns %0 if not, %1 if yes,
 * and %-1 in case of failure.
 */
int mtd_is_bad(const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_mark_bad - mark an eraseblock as bad.
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to mark as bad
 *
 * This function marks eraseblock @eb as bad. Returns %0 in case of success and
 * %-1 in case of failure.
 */
int mtd_mark_bad(const struct mtd_dev_info *mtd, int fd, int eb);

/**
 * mtd_read - read data from an MTD device.
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to read from
 * @offs: offset within the eraseblock to read from
 * @buf: buffer to read data to
 * @len: how many bytes to read
 *
 * This function reads @len bytes of data from eraseblock @eb and offset @offs
 * of the MTD device defined by @mtd and stores the read data at buffer @buf.
 * Returns %0 in case of success and %-1 in case of failure.
 */
int mtd_read(const struct mtd_dev_info *mtd, int fd, int eb, int offs,
	     void *buf, int len);

/**
 * mtd_write - write data to an MTD device.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to write to
 * @offs: offset within the eraseblock to write to
 * @data: data buffer to write
 * @len: how many data bytes to write
 * @oob: OOB buffer to write
 * @ooblen: how many OOB bytes to write
 * @mode: write mode (e.g., %MTD_OOB_PLACE, %MTD_OOB_RAW)
 *
 * This function writes @len bytes of data to eraseblock @eb and offset @offs
 * of the MTD device defined by @mtd. Returns %0 in case of success and %-1 in
 * case of failure.
 *
 * Can only write to a single page at a time if writing to OOB.
 */
int mtd_write(libmtd_t desc, const struct mtd_dev_info *mtd, int fd, int eb,
	      int offs, void *data, int len, void *oob, int ooblen,
	      uint8_t mode);

/**
 * mtd_read_oob - read out-of-band area.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @start: page-aligned start address
 * @length: number of OOB bytes to read
 * @data: read buffer
 *
 * This function reads @length OOB bytes starting from address @start on
 * MTD device described by @fd. The address is specified as page byte offset
 * from the beginning of the MTD device. This function returns %0 in case of
 * success and %-1 in case of failure.
 */
int mtd_read_oob(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		 uint64_t start, uint64_t length, void *data);

/**
 * mtd_write_oob - write out-of-band area.
 * @desc: MTD library descriptor
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @start: page-aligned start address
 * @length: number of OOB bytes to write
 * @data: write buffer
 *
 * This function writes @length OOB bytes starting from address @start on
 * MTD device described by @fd. The address is specified as page byte offset
 * from the beginning of the MTD device. Returns %0 in case of success and %-1
 * in case of failure.
 */
int mtd_write_oob(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		  uint64_t start, uint64_t length, void *data);

/**
 * mtd_write_img - write a file to MTD device.
 * @mtd: MTD device description object
 * @fd: MTD device node file descriptor
 * @eb: eraseblock to write to
 * @offs: offset within the eraseblock to write to
 * @img_name: the file to write
 *
 * This function writes an image @img_name the MTD device defined by @mtd. @eb
 * and @offs are the starting eraseblock and offset on the MTD device. Returns
 * %0 in case of success and %-1 in case of failure.
 */
int mtd_write_img(const struct mtd_dev_info *mtd, int fd, int eb, int offs,
		  const char *img_name);

/**
 * mtd_probe_node - test MTD node.
 * @desc: MTD library descriptor
 * @node: the node to test
 *
 * This function tests whether @node is an MTD device node and returns %1 if it
 * is, and %-1 if it is not (errno is %ENODEV in this case) or if an error
 * occurred.
 */
int mtd_probe_node(libmtd_t desc, const char *node);

#ifdef __cplusplus
}
#endif

#endif /* __LIBMTD_H__ */
