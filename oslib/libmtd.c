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
 * [한국어 설명] MTD 라이브러리 주요 구현 (libmtd.c)
 *
 * === 파일의 역할 ===
 * MTD(Memory Technology Device) 라이브러리의 핵심 구현 파일이다.
 * sysfs(/sys/class/mtd)를 통해 MTD 장치 정보를 조회하고, ioctl을 통해
 * 소거(erase), 읽기(read), 쓰기(write), 잠금(lock/unlock), 배드블록 관리 등
 * MTD 장치의 모��� 주요 작업을 수행한다.
 * sysfs가 지원되지 않는 구형 커널에서는 libmtd_legacy.c로 폴백한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 MTD I/O 엔진에서 사용된다:
 *   engines/mtd.c → libmtd.h API → [libmtd.c] → sysfs 읽기 / ioctl 호출
 *
 * === 타 모듈과의 연결 ===
 * - libmtd.h: 공개 API 선언
 * - libmtd_int.h: struct libmtd 내부 구조체
 * - libmtd_legacy.c: sysfs 미지원 시 레거시 구현
 * - libmtd_common.h: 에러 매크로, xmalloc() 등
 * - <mtd/mtd-user.h>: MTD ioctl 정의 (MEMERASE, MEMWRITE 등)
 *
 * === 주요 함수 요약 ===
 * - libmtd_open()/close(): 라이브러리 초기화/해제 (sysfs 경로 구성)
 * - mtd_get_info(): 시스템의 MTD 장치 목록 조회
 * - mtd_get_dev_info(): 개별 MTD 장치 상세 정보 조회
 * - mtd_erase(): 소거블록 소거 (MEMERASE/MEMERASE64 ioctl)
 * - mtd_read()/write(): MTD 장치 데이터 읽기/쓰기
 * - mtd_torture(): 소거블록 고문 테스트 (패턴 쓰기/읽기 반복)
 */
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <inttypes.h>

#include "../compiler/compiler.h"

#include <mtd/mtd-user.h>
#include "libmtd.h"

#include "libmtd_int.h"
#include "libmtd_common.h"

/*
 * [한국어]
 * mkpath - 두 경로 구성요소를 합쳐 전체 경로 생성
 *
 * sysfs 경로를 동적으로 구성하는 데 사용된다.
 * 예: mkpath("/sys/class/mtd", "mtd0") → "/sys/class/mtd/mtd0"
 */
/**
 * mkpath - compose full path from 2 given components.
 * @path: the first component
 * @name: the second component
 *
 * This function returns the resulting path in case of success and %NULL in
 * case of failure.
 */
static char *mkpath(const char *path, const char *name)
{
	char *n;
	size_t len1 = strlen(path);
	size_t len2 = strlen(name);

	n = xmalloc(len1 + len2 + 6);

	memcpy(n, path, len1);
	if (n[len1 - 1] != '/')
		n[len1++] = '/';

	memcpy(n + len1, name, len2 + 1);
	return n;
}

/*
 * [한국어]
 * read_data - sysfs 파일에서 데이터를 읽는 범용 헬퍼
 *
 * sysfs 속성 파일은 작은 텍스트이므로 한 번에 읽어 NUL 종료 처리한다.
 * 파일 내용이 buf_len보다 크면 EINVAL 에러를 반환한다.
 */
/**
 * read_data - read data from a file.
 * @file: the file to read from
 * @buf: the buffer to read to
 * @buf_len: buffer length
 *
 * This function returns number of read bytes in case of success and %-1 in
 * case of failure. Note, if the file contains more then @buf_len bytes of
 * date, this function fails with %EINVAL error code.
 */
static int read_data(const char *file, void *buf, int buf_len)
{
	int fd, rd, tmp, tmp1;

	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return -1;

	rd = read(fd, buf, buf_len);
	if (rd == -1) {
		sys_errmsg("cannot read \"%s\"", file);
		goto out_error;
	}

	if (rd == buf_len) {
		errmsg("contents of \"%s\" is too long", file);
		errno = EINVAL;
		goto out_error;
	}

	((char *)buf)[rd] = '\0';

	/* Make sure all data is read */
	tmp1 = read(fd, &tmp, 1);
	if (tmp1 == 1) {
		sys_errmsg("cannot read \"%s\"", file);
		goto out_error;
	}
	if (tmp1) {
		errmsg("file \"%s\" contains too much data (> %d bytes)",
		       file, buf_len);
		errno = EINVAL;
		goto out_error;
	}

	if (close(fd)) {
		sys_errmsg("close failed on \"%s\"", file);
		return -1;
	}

	return rd;

out_error:
	close(fd);
	return -1;
}

/**
 * read_major - read major and minor numbers from a file.
 * @file: name of the file to read from
 * @major: major number is returned here
 * @minor: minor number is returned here
 *
 * This function returns % in case of success, and %-1 in case of failure.
 */
static int read_major(const char *file, int *major, int *minor)
{
	int ret;
	char buf[50];

	ret = read_data(file, buf, 50);
	if (ret < 0)
		return ret;

	ret = sscanf(buf, "%d:%d\n", major, minor);
	if (ret != 2) {
		errno = EINVAL;
		return errmsg("\"%s\" does not have major:minor format", file);
	}

	if (*major < 0 || *minor < 0) {
		errno = EINVAL;
		return errmsg("bad major:minor %d:%d in \"%s\"",
			      *major, *minor, file);
	}

	return 0;
}

/**
 * dev_get_major - get major and minor numbers of an MTD device.
 * @lib: libmtd descriptor
 * @mtd_num: MTD device number
 * @major: major number is returned here
 * @minor: minor number is returned here
 *
 * This function returns zero in case of success and %-1 in case of failure.
 */
static int dev_get_major(struct libmtd *lib, int mtd_num, int *major, int *minor)
{
	char file[strlen(lib->mtd_dev) + 50];

	sprintf(file, lib->mtd_dev, mtd_num);
	return read_major(file, major, minor);
}

/**
 * dev_read_data - read data from an MTD device's sysfs file.
 * @patt: file pattern to read from
 * @mtd_num: MTD device number
 * @buf: buffer to read to
 * @buf_len: buffer length
 *
 * This function returns number of read bytes in case of success and %-1 in
 * case of failure.
 */
static int dev_read_data(const char *patt, int mtd_num, void *buf, int buf_len)
{
	char file[strlen(patt) + 100];

	sprintf(file, patt, mtd_num);
	return read_data(file, buf, buf_len);
}

/**
 * read_hex_ll - read a hex 'long long' value from a file.
 * @file: the file to read from
 * @value: the result is stored here
 *
 * This function reads file @file and interprets its contents as hexadecimal
 * 'long long' integer. If this is not true, it fails with %EINVAL error code.
 * Returns %0 in case of success and %-1 in case of failure.
 */
static int read_hex_ll(const char *file, long long *value)
{
	int fd, rd;
	char buf[50];

	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return -1;

	rd = read(fd, buf, sizeof(buf));
	if (rd == -1) {
		sys_errmsg("cannot read \"%s\"", file);
		goto out_error;
	}
	if (rd == sizeof(buf)) {
		errmsg("contents of \"%s\" is too long", file);
		errno = EINVAL;
		goto out_error;
	}
	buf[rd] = '\0';

	if (sscanf(buf, "%llx\n", value) != 1) {
		errmsg("cannot read integer from \"%s\"\n", file);
		errno = EINVAL;
		goto out_error;
	}

	if (*value < 0) {
		errmsg("negative value %lld in \"%s\"", *value, file);
		errno = EINVAL;
		goto out_error;
	}

	if (close(fd))
		return sys_errmsg("close failed on \"%s\"", file);

	return 0;

out_error:
	close(fd);
	return -1;
}

/**
 * read_pos_ll - read a positive 'long long' value from a file.
 * @file: the file to read from
 * @value: the result is stored here
 *
 * This function reads file @file and interprets its contents as a positive
 * 'long long' integer. If this is not true, it fails with %EINVAL error code.
 * Returns %0 in case of success and %-1 in case of failure.
 */
static int read_pos_ll(const char *file, long long *value)
{
	int fd, rd;
	char buf[50];

	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return -1;

	rd = read(fd, buf, 50);
	if (rd == -1) {
		sys_errmsg("cannot read \"%s\"", file);
		goto out_error;
	}
	if (rd == 50) {
		errmsg("contents of \"%s\" is too long", file);
		errno = EINVAL;
		goto out_error;
	}

	if (sscanf(buf, "%lld\n", value) != 1) {
		errmsg("cannot read integer from \"%s\"\n", file);
		errno = EINVAL;
		goto out_error;
	}

	if (*value < 0) {
		errmsg("negative value %lld in \"%s\"", *value, file);
		errno = EINVAL;
		goto out_error;
	}

	if (close(fd))
		return sys_errmsg("close failed on \"%s\"", file);

	return 0;

out_error:
	close(fd);
	return -1;
}

/**
 * read_hex_int - read an 'int' value from a file.
 * @file: the file to read from
 * @value: the result is stored here
 *
 * This function is the same as 'read_pos_ll()', but it reads an 'int'
 * value, not 'long long'.
 */
static int read_hex_int(const char *file, int *value)
{
	long long res;

	if (read_hex_ll(file, &res))
		return -1;

	/* Make sure the value has correct range */
	if (res > INT_MAX || res < INT_MIN) {
		errmsg("value %lld read from file \"%s\" is out of range",
		       res, file);
		errno = EINVAL;
		return -1;
	}

	*value = res;
	return 0;
}

/**
 * read_pos_int - read a positive 'int' value from a file.
 * @file: the file to read from
 * @value: the result is stored here
 *
 * This function is the same as 'read_pos_ll()', but it reads an 'int'
 * value, not 'long long'.
 */
static int read_pos_int(const char *file, int *value)
{
	long long res;

	if (read_pos_ll(file, &res))
		return -1;

	/* Make sure the value is not too big */
	if (res > INT_MAX) {
		errmsg("value %lld read from file \"%s\" is out of range",
		       res, file);
		errno = EINVAL;
		return -1;
	}

	*value = res;
	return 0;
}

/**
 * dev_read_hex_int - read an hex 'int' value from an MTD device sysfs file.
 * @patt: file pattern to read from
 * @mtd_num: MTD device number
 * @value: the result is stored here
 *
 * This function returns %0 in case of success and %-1 in case of failure.
 */
static int dev_read_hex_int(const char *patt, int mtd_num, int *value)
{
	char file[strlen(patt) + 50];

	sprintf(file, patt, mtd_num);
	return read_hex_int(file, value);
}

/**
 * dev_read_pos_int - read a positive 'int' value from an MTD device sysfs file.
 * @patt: file pattern to read from
 * @mtd_num: MTD device number
 * @value: the result is stored here
 *
 * This function returns %0 in case of success and %-1 in case of failure.
 */
static int dev_read_pos_int(const char *patt, int mtd_num, int *value)
{
	char file[strlen(patt) + 50];

	sprintf(file, patt, mtd_num);
	return read_pos_int(file, value);
}

/**
 * dev_read_pos_ll - read a positive 'long long' value from an MTD device sysfs file.
 * @patt: file pattern to read from
 * @mtd_num: MTD device number
 * @value: the result is stored here
 *
 * This function returns %0 in case of success and %-1 in case of failure.
 */
static int dev_read_pos_ll(const char *patt, int mtd_num, long long *value)
{
	char file[strlen(patt) + 50];

	sprintf(file, patt, mtd_num);
	return read_pos_ll(file, value);
}

/*
 * [한국어]
 * type_str2int - MTD 장치 타입 문자열을 정수 상수로 변환
 *
 * sysfs의 type 파일에서 읽은 문자열("nand", "nor" 등)을
 * <mtd/mtd-user.h>의 MTD_NANDFLASH, MTD_NORFLASH 등 상수로 변환한다.
 */
/**
 * type_str2int - convert MTD device type to integer.
 * @str: MTD device type string to convert
 *
 * This function converts MTD device type string @str, read from sysfs, into an
 * integer.
 */
static int type_str2int(const char *str)
{
	if (!strcmp(str, "nand"))
		return MTD_NANDFLASH;
	if (!strcmp(str, "mlc-nand"))
		return MTD_MLCNANDFLASH;
	if (!strcmp(str, "nor"))
		return MTD_NORFLASH;
	if (!strcmp(str, "rom"))
		return MTD_ROM;
	if (!strcmp(str, "absent"))
		return MTD_ABSENT;
	if (!strcmp(str, "dataflash"))
		return MTD_DATAFLASH;
	if (!strcmp(str, "ram"))
		return MTD_RAM;
	if (!strcmp(str, "ubi"))
		return MTD_UBIVOLUME;
	return -1;
}

/**
 * dev_node2num - find UBI device number by its character device node.
 * @lib: MTD library descriptor
 * @node: name of the MTD device node
 * @mtd_num: MTD device number is returned here
 *
 * This function returns %0 in case of success and %-1 in case of failure.
 */
static int dev_node2num(struct libmtd *lib, const char *node, int *mtd_num)
{
	struct stat st;
	int i, mjr, mnr;
	struct mtd_info info;

	if (stat(node, &st))
		return sys_errmsg("cannot get information about \"%s\"", node);

	if (!S_ISCHR(st.st_mode)) {
		errmsg("\"%s\" is not a character device", node);
		errno = EINVAL;
		return -1;
	}

	mjr = major(st.st_rdev);
	mnr = minor(st.st_rdev);

	if (mtd_get_info((libmtd_t *)lib, &info))
		return -1;

	for (i = info.lowest_mtd_num; i <= info.highest_mtd_num; i++) {
		int mjr1, mnr1, ret;

		ret = dev_get_major(lib, i, &mjr1, &mnr1);
		if (ret) {
			if (errno == ENOENT)
				continue;
			if (!errno)
				break;
			return -1;
		}

		if (mjr1 == mjr && mnr1 == mnr) {
			errno = 0;
			*mtd_num = i;
			return 0;
		}
	}

	errno = ENODEV;
	return -1;
}

/**
 * sysfs_is_supported - check whether the MTD sub-system supports MTD.
 * @lib: MTD library descriptor
 *
 * The Linux kernel MTD subsystem gained MTD support starting from kernel
 * 2.6.30 and libmtd tries to use sysfs interface if possible, because the NAND
 * sub-page size is available there (and not available at all in pre-sysfs
 * kernels).
 *
 * Very old kernels did not have "/sys/class/mtd" directory. Not very old
 * kernels (e.g., 2.6.29) did have "/sys/class/mtd/mtdX" directories, by there
 * were no files there, e.g., the "name" file was not present. So all we can do
 * is to check for a "/sys/class/mtd/mtdX/name" file. But this is not a
 * reliable check, because if this is a new system with no MTD devices - we'll
 * treat it as a pre-sysfs system.
 */
static int sysfs_is_supported(struct libmtd *lib)
{
	int fd, num = -1;
	DIR *sysfs_mtd;
	char file[strlen(lib->mtd_name) + 10];

	sysfs_mtd = opendir(lib->sysfs_mtd);
	if (!sysfs_mtd) {
		if (errno == ENOENT) {
			errno = 0;
			return 0;
		}
		return sys_errmsg("cannot open \"%s\"", lib->sysfs_mtd);
	}

	/*
	 * First of all find an "mtdX" directory. This is needed because there
	 * may be, for example, mtd1 but no mtd0.
	 */
	while (1) {
		int ret, mtd_num;
		char tmp_buf[256];
		struct dirent *dirent;

		dirent = readdir(sysfs_mtd);
		if (!dirent)
			break;

		if (strlen(dirent->d_name) >= 255) {
			errmsg("invalid entry in %s: \"%s\"",
			       lib->sysfs_mtd, dirent->d_name);
			errno = EINVAL;
			closedir(sysfs_mtd);
			return -1;
		}

		ret = sscanf(dirent->d_name, MTD_NAME_PATT"%s",
			     &mtd_num, tmp_buf);
		if (ret == 1) {
			num = mtd_num;
			break;
		}
	}

	if (closedir(sysfs_mtd))
		return sys_errmsg("closedir failed on \"%s\"", lib->sysfs_mtd);

	if (num == -1)
		/* No mtd device, treat this as pre-sysfs system */
		return 0;

	sprintf(file, lib->mtd_name, num);
	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return 0;

	if (close(fd)) {
		sys_errmsg("close failed on \"%s\"", file);
		return -1;
	}

	return 1;
}

/*
 * [한국어]
 * libmtd_open - MTD 라이브러리를 초기화하고 디스크립터를 반환
 *
 * @return: 성공 시 라이브러리 디스크립터(libmtd_t), 실패 시 NULL
 *
 * 동작 과정:
 * 1) struct libmtd를 할당하고 초기화
 * 2) sysfs 경로 패턴들을 구성 (/sys/class/mtd/mtdN/name 등)
 * 3) sysfs_is_supported()로 sysfs MTD 지원 여부 확인
 * 4) sysfs 미지원 시 경로를 NULL로 설정 (이후 legacy_* 함수 사용)
 * 5) sysfs 지원 시 모든 속성 파일 경로 패턴을 구성
 *
 * 호출 체인:
 *   engines/mtd.c → [libmtd_open()] → mkpath(), sysfs_is_supported()
 */
libmtd_t libmtd_open(void)
{
	struct libmtd *lib;

	lib = xzalloc(sizeof(*lib));

	lib->offs64_ioctls = OFFS64_IOCTLS_UNKNOWN;

	lib->sysfs_mtd = mkpath("/sys", SYSFS_MTD);
	if (!lib->sysfs_mtd)
		goto out_error;

	lib->mtd = mkpath(lib->sysfs_mtd, MTD_NAME_PATT);
	if (!lib->mtd)
		goto out_error;

	lib->mtd_name = mkpath(lib->mtd, MTD_NAME);
	if (!lib->mtd_name)
		goto out_error;

	if (!sysfs_is_supported(lib)) {
		free(lib->mtd);
		free(lib->sysfs_mtd);
		free(lib->mtd_name);
		lib->mtd_name = lib->mtd = lib->sysfs_mtd = NULL;
		return lib;
	}

	lib->mtd_dev = mkpath(lib->mtd, MTD_DEV);
	if (!lib->mtd_dev)
		goto out_error;

	lib->mtd_type = mkpath(lib->mtd, MTD_TYPE);
	if (!lib->mtd_type)
		goto out_error;

	lib->mtd_eb_size = mkpath(lib->mtd, MTD_EB_SIZE);
	if (!lib->mtd_eb_size)
		goto out_error;

	lib->mtd_size = mkpath(lib->mtd, MTD_SIZE);
	if (!lib->mtd_size)
		goto out_error;

	lib->mtd_min_io_size = mkpath(lib->mtd, MTD_MIN_IO_SIZE);
	if (!lib->mtd_min_io_size)
		goto out_error;

	lib->mtd_subpage_size = mkpath(lib->mtd, MTD_SUBPAGE_SIZE);
	if (!lib->mtd_subpage_size)
		goto out_error;

	lib->mtd_oob_size = mkpath(lib->mtd, MTD_OOB_SIZE);
	if (!lib->mtd_oob_size)
		goto out_error;

	lib->mtd_region_cnt = mkpath(lib->mtd, MTD_REGION_CNT);
	if (!lib->mtd_region_cnt)
		goto out_error;

	lib->mtd_flags = mkpath(lib->mtd, MTD_FLAGS);
	if (!lib->mtd_flags)
		goto out_error;

	lib->sysfs_supported = 1;
	return lib;

out_error:
	libmtd_close((libmtd_t)lib);
	return NULL;
}

/*
 * [한국어]
 * libmtd_close - MTD 라이브러리 자원 해제
 *
 * @desc: libmtd_open()에서 반환된 라이브러리 디스크립터
 *
 * 모든 동적 할당된 sysfs 경로 문자열과 struct libmtd를 free()한다.
 */
void libmtd_close(libmtd_t desc)
{
	struct libmtd *lib = (struct libmtd *)desc;

	free(lib->mtd_flags);
	free(lib->mtd_region_cnt);
	free(lib->mtd_oob_size);
	free(lib->mtd_subpage_size);
	free(lib->mtd_min_io_size);
	free(lib->mtd_size);
	free(lib->mtd_eb_size);
	free(lib->mtd_type);
	free(lib->mtd_dev);
	free(lib->mtd_name);
	free(lib->mtd);
	free(lib->sysfs_mtd);
	free(lib);
}

/* [한국어] mtd_dev_present: MTD 장치 존재 여부 확인 (sysfs stat 또는 legacy /proc/mtd) */
int mtd_dev_present(libmtd_t desc, int mtd_num) {
	struct stat st;
	struct libmtd *lib = (struct libmtd *)desc;

	if (!lib->sysfs_supported) {
		return legacy_dev_present(mtd_num) == 1;
	} else {
		char file[strlen(lib->mtd) + 10];

		sprintf(file, lib->mtd, mtd_num);
		return !stat(file, &st);
	}
}

/*
 * [한국어]
 * mtd_get_info - 시스템의 MTD 장치 전체 정보를 수집
 *
 * @desc: 라이브러리 디스크립터
 * @info: 결과가 저장될 mtd_info 구조체
 * @return: 성공 시 0, 실패 시 -1
 *
 * sysfs 지원 시 /sys/class/mtd 디렉토리를 스캔하여 mtdN 패턴의 디렉토리를 세고,
 * 장치 수, 최소/최대 번호를 파악한다. sysfs 미지원 시 legacy_mtd_get_info() 호출.
 */
int mtd_get_info(libmtd_t desc, struct mtd_info *info)
{
	DIR *sysfs_mtd;
	struct dirent *dirent;
	struct libmtd *lib = (struct libmtd *)desc;

	memset(info, 0, sizeof(struct mtd_info));

	if (!lib->sysfs_supported)
		return legacy_mtd_get_info(info);

	info->sysfs_supported = 1;

	/*
	 * We have to scan the MTD sysfs directory to identify how many MTD
	 * devices are present.
	 */
	sysfs_mtd = opendir(lib->sysfs_mtd);
	if (!sysfs_mtd) {
		if (errno == ENOENT) {
			errno = ENODEV;
			return -1;
		}
		return sys_errmsg("cannot open \"%s\"", lib->sysfs_mtd);
	}

	info->lowest_mtd_num = INT_MAX;
	while (1) {
		int mtd_num, ret;
		char tmp_buf[256];

		errno = 0;
		dirent = readdir(sysfs_mtd);
		if (!dirent)
			break;

		if (strlen(dirent->d_name) >= 255) {
			errmsg("invalid entry in %s: \"%s\"",
			       lib->sysfs_mtd, dirent->d_name);
			errno = EINVAL;
			goto out_close;
		}

		ret = sscanf(dirent->d_name, MTD_NAME_PATT"%s",
			     &mtd_num, tmp_buf);
		if (ret == 1) {
			info->mtd_dev_cnt += 1;
			if (mtd_num > info->highest_mtd_num)
				info->highest_mtd_num = mtd_num;
			if (mtd_num < info->lowest_mtd_num)
				info->lowest_mtd_num = mtd_num;
		}
	}

	if (!dirent && errno) {
		sys_errmsg("readdir failed on \"%s\"", lib->sysfs_mtd);
		goto out_close;
	}

	if (closedir(sysfs_mtd))
		return sys_errmsg("closedir failed on \"%s\"", lib->sysfs_mtd);

	if (info->lowest_mtd_num == INT_MAX)
		info->lowest_mtd_num = 0;

	return 0;

out_close:
	closedir(sysfs_mtd);
	return -1;
}

/*
 * [한국어]
 * mtd_get_dev_info1 - MTD 장치 번호로 개별 장치 상세 정보 조회
 *
 * @desc: 라이브러리 디스크립터
 * @mtd_num: MTD 장치 번호
 * @mtd: 결과가 저장될 mtd_dev_info 구조체
 * @return: 성공 시 0, 실패 시 -1
 *
 * sysfs의 각 속성 파일(name, type, erasesize, size 등)을 읽어
 * mtd_dev_info 구조체를 채운다. sysfs 미지원 시 legacy 함수 사용.
 */
int mtd_get_dev_info1(libmtd_t desc, int mtd_num, struct mtd_dev_info *mtd)
{
	int ret;
	struct libmtd *lib = (struct libmtd *)desc;

	memset(mtd, 0, sizeof(struct mtd_dev_info));
	mtd->mtd_num = mtd_num;

	if (!mtd_dev_present(desc, mtd_num)) {
		errno = ENODEV;
		return -1;
	} else if (!lib->sysfs_supported)
		return legacy_get_dev_info1(mtd_num, mtd);

	if (dev_get_major(lib, mtd_num, &mtd->major, &mtd->minor))
		return -1;

	ret = dev_read_data(lib->mtd_name, mtd_num, &mtd->name,
			    MTD_NAME_MAX + 1);
	if (ret < 0)
		return -1;
	((char *)mtd->name)[ret - 1] = '\0';

	ret = dev_read_data(lib->mtd_type, mtd_num, &mtd->type_str,
			    MTD_TYPE_MAX + 1);
	if (ret < 0)
		return -1;
	((char *)mtd->type_str)[ret - 1] = '\0';

	if (dev_read_pos_int(lib->mtd_eb_size, mtd_num, &mtd->eb_size))
		return -1;
	if (dev_read_pos_ll(lib->mtd_size, mtd_num, &mtd->size))
		return -1;
	if (dev_read_pos_int(lib->mtd_min_io_size, mtd_num, &mtd->min_io_size))
		return -1;
	if (dev_read_pos_int(lib->mtd_subpage_size, mtd_num, &mtd->subpage_size))
		return -1;
	if (dev_read_pos_int(lib->mtd_oob_size, mtd_num, &mtd->oob_size))
		return -1;
	if (dev_read_pos_int(lib->mtd_region_cnt, mtd_num, &mtd->region_cnt))
		return -1;
	if (dev_read_hex_int(lib->mtd_flags, mtd_num, &ret))
		return -1;
	mtd->writable = !!(ret & MTD_WRITEABLE);

	mtd->eb_cnt = mtd->size / mtd->eb_size;
	mtd->type = type_str2int(mtd->type_str);
	mtd->bb_allowed = !!(mtd->type == MTD_NANDFLASH ||
				mtd->type == MTD_MLCNANDFLASH);

	return 0;
}

/*
 * [한국어]
 * mtd_get_dev_info - 장치 노드 경로로 MTD 장치 정보 조회
 *
 * @desc: 라이브러리 디스크립터
 * @node: 장치 노드 경로 (예: "/dev/mtd0")
 * @mtd: 결과가 저장될 mtd_dev_info 구조체
 * @return: 성공 시 0, 실패 시 -1
 *
 * 장치 노드에서 major:minor를 얻어 MTD 번호를 찾고, mtd_get_dev_info1()에 위임.
 */
int mtd_get_dev_info(libmtd_t desc, const char *node, struct mtd_dev_info *mtd)
{
	int mtd_num;
	struct libmtd *lib = (struct libmtd *)desc;

	if (!lib->sysfs_supported)
		return legacy_get_dev_info(node, mtd);

	if (dev_node2num(lib, node, &mtd_num))
		return -1;

	return mtd_get_dev_info1(desc, mtd_num, mtd);
}

/* [한국어] ioctl 에러 발생 시 장치 번호와 소거블록 번호를 포함한 에러 메시지 출력 */
static inline int mtd_ioctl_error(const struct mtd_dev_info *mtd, int eb,
				  const char *sreq)
{
	return sys_errmsg("%s ioctl failed for eraseblock %d (mtd%d)",
			  sreq, eb, mtd->mtd_num);
}

/* [한국어] 소거블록 번호의 유효 범위(0 ~ eb_cnt-1) 검증 */
static int mtd_valid_erase_block(const struct mtd_dev_info *mtd, int eb)
{
	if (eb < 0 || eb >= mtd->eb_cnt) {
		errmsg("bad eraseblock number %d, mtd%d has %d eraseblocks",
		       eb, mtd->mtd_num, mtd->eb_cnt);
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/*
 * [한국어]
 * mtd_xlock - 소거블록 잠금/해제의 공통 구현
 *
 * MEMLOCK/MEMUNLOCK ioctl을 호출하여 소거블록을 잠그거나 해제한다.
 * 잠긴 소거블록은 소거/쓰기가 불가능하다 (NOR 플래시에서 주로 사용).
 */
static int mtd_xlock(const struct mtd_dev_info *mtd, int fd, int eb, int req,
		     const char *sreq)
{
	int ret;
	struct erase_info_user ei;

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	ei.start = eb * mtd->eb_size;
	ei.length = mtd->eb_size;

	ret = ioctl(fd, req, &ei);
	if (ret < 0)
		return mtd_ioctl_error(mtd, eb, sreq);

	return 0;
}
#define mtd_xlock(mtd, fd, eb, req) mtd_xlock(mtd, fd, eb, req, #req)

int mtd_lock(const struct mtd_dev_info *mtd, int fd, int eb)
{
	return mtd_xlock(mtd, fd, eb, MEMLOCK);
}

int mtd_unlock(const struct mtd_dev_info *mtd, int fd, int eb)
{
	return mtd_xlock(mtd, fd, eb, MEMUNLOCK);
}

/*
 * [한국어]
 * mtd_erase - 소거블록을 소거하여 모든 비트를 0xFF로 초기화
 *
 * @desc: 라이브러리 디스크립터
 * @mtd: MTD 장치 정보
 * @fd: 장치 파일 디스크립터
 * @eb: 소거할 소거블록 번호
 * @return: 성공 시 0, 실패 시 -1
 *
 * NAND/NOR 플래시는 쓰기 전에 반드시 소거해야 한다 (0→1 전환은 소거로만 가능).
 * 먼저 MEMERASE64(64비트)를 시도하고, 지원하지 않으면 MEMERASE(32비트)로 폴백한다.
 *
 * 호출 체인:
 *   engines/mtd.c → [mtd_erase()] → ioctl(MEMERASE64/MEMERASE)
 */
int mtd_erase(libmtd_t desc, const struct mtd_dev_info *mtd, int fd, int eb)
{
	int ret;
	struct libmtd *lib = (struct libmtd *)desc;
	struct erase_info_user64 ei64;
	struct erase_info_user ei;

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	ei64.start = (__u64)eb * mtd->eb_size;
	ei64.length = mtd->eb_size;

	if (lib->offs64_ioctls == OFFS64_IOCTLS_SUPPORTED ||
	    lib->offs64_ioctls == OFFS64_IOCTLS_UNKNOWN) {
		ret = ioctl(fd, MEMERASE64, &ei64);
		if (ret == 0)
			return ret;

		if (errno != ENOTTY ||
		    lib->offs64_ioctls != OFFS64_IOCTLS_UNKNOWN)
			return mtd_ioctl_error(mtd, eb, "MEMERASE64");

		/*
		 * MEMERASE64 support was added in kernel version 2.6.31, so
		 * probably we are working with older kernel and this ioctl is
		 * not supported.
		 */
		lib->offs64_ioctls = OFFS64_IOCTLS_NOT_SUPPORTED;
	}

	if (ei64.start + ei64.length > 0xFFFFFFFF) {
		errmsg("this system can address only %u eraseblocks",
		       0xFFFFFFFFU / mtd->eb_size);
		errno = EINVAL;
		return -1;
	}

	ei.start = ei64.start;
	ei.length = ei64.length;
	ret = ioctl(fd, MEMERASE, &ei);
	if (ret < 0)
		return mtd_ioctl_error(mtd, eb, "MEMERASE");
	return 0;
}

int mtd_regioninfo(int fd, int regidx, struct region_info_user *reginfo)
{
	int ret;

	if (regidx < 0) {
		errno = ENODEV;
		return -1;
	}

	reginfo->regionindex = regidx;

	ret = ioctl(fd, MEMGETREGIONINFO, reginfo);
	if (ret < 0)
		return sys_errmsg("%s ioctl failed for erase region %d",
			"MEMGETREGIONINFO", regidx);

	return 0;
}

int mtd_is_locked(const struct mtd_dev_info *mtd, int fd, int eb)
{
	int ret;
	erase_info_t ei;

	ei.start = eb * mtd->eb_size;
	ei.length = mtd->eb_size;

	ret = ioctl(fd, MEMISLOCKED, &ei);
	if (ret < 0) {
		if (errno != ENOTTY && errno != EOPNOTSUPP)
			return mtd_ioctl_error(mtd, eb, "MEMISLOCKED");
		else
			errno = EOPNOTSUPP;
	}

	return ret;
}

/* [한국어] 고문 테스트에 사용할 바이트 패턴: 체커보드(0xa5, 0x5a)와 0x00 */
/* Patterns to write to a physical eraseblock when torturing it */
static uint8_t patterns[] = {0xa5, 0x5a, 0x0};

/**
 * check_pattern - check if buffer contains only a certain byte pattern.
 * @buf: buffer to check
 * @patt: the pattern to check
 * @size: buffer size in bytes
 *
 * This function returns %1 in there are only @patt bytes in @buf, and %0 if
 * something else was also found.
 */
static int check_pattern(const void *buf, uint8_t patt, int size)
{
	int i;

	for (i = 0; i < size; i++)
		if (((const uint8_t *)buf)[i] != patt)
			return 0;
	return 1;
}

/*
 * [한국어]
 * mtd_torture - 소거블록의 건전성을 테스트하는 고문(torture) 함수
 *
 * @desc: 라이브러리 디스크립터
 * @mtd: MTD 장치 정보
 * @fd: 장치 파일 디스크립터
 * @eb: 테스트할 소거블록 번호
 * @return: 성공 시 0, 실패 시 -1
 *
 * 여러 패턴(0xa5, 0x5a, 0x00)으로 소거→쓰기→읽기→검증 사이클을 반복하여
 * 소거블록이 정상 동작하는지 확인한다. 소거 후 0xFF인지, 패턴 쓰기 후
 * 올바르게 읽히는지를 ��증한다. 불량 블록 후보를 확인하는 데 사용된다.
 */
int mtd_torture(libmtd_t desc, const struct mtd_dev_info *mtd, int fd, int eb)
{
	int err, i, patt_count;
	void *buf;

	normsg("run torture test for PEB %d", eb);
	patt_count = FIO_ARRAY_SIZE(patterns);

	buf = xmalloc(mtd->eb_size);

	for (i = 0; i < patt_count; i++) {
		err = mtd_erase(desc, mtd, fd, eb);
		if (err)
			goto out;

		/* Make sure the PEB contains only 0xFF bytes */
		err = mtd_read(mtd, fd, eb, 0, buf, mtd->eb_size);
		if (err)
			goto out;

		err = check_pattern(buf, 0xFF, mtd->eb_size);
		if (err == 0) {
			errmsg("erased PEB %d, but a non-0xFF byte found", eb);
			errno = EIO;
			goto out;
		}

		/* Write a pattern and check it */
		memset(buf, patterns[i], mtd->eb_size);
		err = mtd_write(desc, mtd, fd, eb, 0, buf, mtd->eb_size, NULL,
				0, 0);
		if (err)
			goto out;

		memset(buf, ~patterns[i], mtd->eb_size);
		err = mtd_read(mtd, fd, eb, 0, buf, mtd->eb_size);
		if (err)
			goto out;

		err = check_pattern(buf, patterns[i], mtd->eb_size);
		if (err == 0) {
			errmsg("pattern %x checking failed for PEB %d",
				patterns[i], eb);
			errno = EIO;
			goto out;
		}
	}

	normsg("PEB %d passed torture test, do not mark it a bad", eb);

out:
	free(buf);
	return -1;
}

/*
 * [한국어]
 * mtd_is_bad - 소거블록이 배드 블록인지 확인
 *
 * @return: 정상이면 0, 배드이면 1, 에러 시 -1
 *
 * NAND 플래시는 제조 시 또는 사용 중 배드 블록이 발생할 수 있다.
 * MEMGETBADBLOCK ioctl로 커널에 배드 블록 여부를 질의한다.
 * bb_allowed가 false인 장치(NOR 등)는 항상 0을 반환한다.
 */
int mtd_is_bad(const struct mtd_dev_info *mtd, int fd, int eb)
{
	int ret;
	loff_t seek;

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	if (!mtd->bb_allowed)
		return 0;

	seek = (loff_t)eb * mtd->eb_size;
	ret = ioctl(fd, MEMGETBADBLOCK, &seek);
	if (ret == -1)
		return mtd_ioctl_error(mtd, eb, "MEMGETBADBLOCK");
	return ret;
}

/*
 * [한국어]
 * mtd_mark_bad - 소거블록을 배드 블록으로 표시
 *
 * MEMSETBADBLOCK ioctl로 커널에 해당 소거블록을 영구적으로 배드 표시한다.
 * 한 번 표시되면 이후 이 블록은 사용되지 않는다.
 */
int mtd_mark_bad(const struct mtd_dev_info *mtd, int fd, int eb)
{
	int ret;
	loff_t seek;

	if (!mtd->bb_allowed) {
		errno = EINVAL;
		return -1;
	}

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	seek = (loff_t)eb * mtd->eb_size;
	ret = ioctl(fd, MEMSETBADBLOCK, &seek);
	if (ret == -1)
		return mtd_ioctl_error(mtd, eb, "MEMSETBADBLOCK");
	return 0;
}

/*
 * [한국어]
 * mtd_read - MTD 장치에서 데이터 읽기
 *
 * @mtd: MTD 장치 정보
 * @fd: 장치 파일 디스크립터
 * @eb: 읽을 소거블록 번호
 * @offs: 소거블록 내 오프셋
 * @buf: 데이터를 저장할 버퍼
 * @len: 읽을 바이트 수
 * @return: 성공 시 0, 실패 시 -1
 *
 * lseek로 소거블록 시작 + offs 위치로 이동한 후 read()로 데이터를 읽는다.
 * 짧은 읽기(short read)가 발생할 수 있으므로 루프로 전체 길이를 읽는다.
 *
 * 호출 체인:
 *   engines/mtd.c → [mtd_read()] → lseek(), read()
 */
int mtd_read(const struct mtd_dev_info *mtd, int fd, int eb, int offs,
	     void *buf, int len)
{
	int ret, rd = 0;
	off_t seek;

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	if (offs < 0 || offs + len > mtd->eb_size) {
		errmsg("bad offset %d or length %d, mtd%d eraseblock size is %d",
		       offs, len, mtd->mtd_num, mtd->eb_size);
		errno = EINVAL;
		return -1;
	}

	/* Seek to the beginning of the eraseblock */
	seek = (off_t)eb * mtd->eb_size + offs;
	if (lseek(fd, seek, SEEK_SET) != seek)
		return sys_errmsg("cannot seek mtd%d to offset %"PRIdoff_t,
				  mtd->mtd_num, seek);

	while (rd < len) {
		ret = read(fd, buf, len);
		if (ret < 0)
			return sys_errmsg("cannot read %d bytes from mtd%d (eraseblock %d, offset %d)",
					  len, mtd->mtd_num, eb, offs);
		rd += ret;
	}

	return 0;
}

static int legacy_auto_oob_layout(const struct mtd_dev_info *mtd, int fd,
				  int ooblen, void *oob) {
	struct nand_oobinfo old_oobinfo;
	int start, len;
	uint8_t *tmp_buf;

	/* Read the current oob info */
	if (ioctl(fd, MEMGETOOBSEL, &old_oobinfo))
		return sys_errmsg("MEMGETOOBSEL failed");

	tmp_buf = malloc(ooblen);
	memcpy(tmp_buf, oob, ooblen);

	/*
	 * We use autoplacement and have the oobinfo with the autoplacement
	 * information from the kernel available
	 */
	if (old_oobinfo.useecc == MTD_NANDECC_AUTOPLACE) {
		int i, tags_pos = 0;
		for (i = 0; old_oobinfo.oobfree[i][1]; i++) {
			/* Set the reserved bytes to 0xff */
			start = old_oobinfo.oobfree[i][0];
			len = old_oobinfo.oobfree[i][1];
			memcpy(oob + start, tmp_buf + tags_pos, len);
			tags_pos += len;
		}
	} else {
		/* Set at least the ecc byte positions to 0xff */
		start = old_oobinfo.eccbytes;
		len = mtd->oob_size - start;
		memcpy(oob + start, tmp_buf + start, len);
	}
	free(tmp_buf);

	return 0;
}

/*
 * [한국어]
 * mtd_write - MTD 장치에 데이터(+OOB) 쓰기
 *
 * @desc: 라이브러리 디스크립터
 * @mtd: MTD 장치 정보
 * @fd: 장치 파일 디스크립터
 * @eb: 쓸 소거블록 번호
 * @offs: 소거블록 내 오프셋 (서브페이지 정렬 필요)
 * @data: 데이터 버퍼
 * @len: 데이터 길이 (서브페이지 정렬 필요)
 * @oob: OOB 데이터 버퍼 (NULL이면 OOB 쓰지 않음)
 * @ooblen: OOB 데이터 길이
 * @mode: OOB 쓰기 모드 (MTD_OPS_AUTO_OOB 등)
 * @return: 성공 시 0, 실패 시 -1
 *
 * OOB가 있으면 MEMWRITE ioctl을 먼저 시도하고, 지원하지 않으면
 * 구형 OOB ioctl로 폴백한다. 데이터는 lseek + write()로 기록한다.
 *
 * 호출 체인:
 *   engines/mtd.c → [mtd_write()] → ioctl(MEMWRITE), write()
 */
int mtd_write(libmtd_t desc, const struct mtd_dev_info *mtd, int fd, int eb,
	      int offs, void *data, int len, void *oob, int ooblen,
	      uint8_t mode)
{
	int ret;
	off_t seek;
	struct mtd_write_req ops;

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	if (offs < 0 || offs + len > mtd->eb_size) {
		errmsg("bad offset %d or length %d, mtd%d eraseblock size is %d",
		       offs, len, mtd->mtd_num, mtd->eb_size);
		errno = EINVAL;
		return -1;
	}
	if (offs % mtd->subpage_size) {
		errmsg("write offset %d is not aligned to mtd%d min. I/O size %d",
		       offs, mtd->mtd_num, mtd->subpage_size);
		errno = EINVAL;
		return -1;
	}
	if (len % mtd->subpage_size) {
		errmsg("write length %d is not aligned to mtd%d min. I/O size %d",
		       len, mtd->mtd_num, mtd->subpage_size);
		errno = EINVAL;
		return -1;
	}

	/* Calculate seek address */
	seek = (off_t)eb * mtd->eb_size + offs;

	if (oob) {
		ops.start = seek;
		ops.len = len;
		ops.ooblen = ooblen;
		ops.usr_data = (uint64_t)(unsigned long)data;
		ops.usr_oob = (uint64_t)(unsigned long)oob;
		ops.mode = mode;

		ret = ioctl(fd, MEMWRITE, &ops);
		if (ret == 0)
			return 0;
		else if (errno != ENOTTY && errno != EOPNOTSUPP)
			return mtd_ioctl_error(mtd, eb, "MEMWRITE");

		/* Fall back to old OOB ioctl() if necessary */
		if (mode == MTD_OPS_AUTO_OOB)
			if (legacy_auto_oob_layout(mtd, fd, ooblen, oob))
				return -1;
		if (mtd_write_oob(desc, mtd, fd, seek, ooblen, oob) < 0)
			return sys_errmsg("cannot write to OOB");
	}
	if (data) {
		/* Seek to the beginning of the eraseblock */
		if (lseek(fd, seek, SEEK_SET) != seek)
			return sys_errmsg("cannot seek mtd%d to offset %"PRIdoff_t,
					mtd->mtd_num, seek);
		ret = write(fd, data, len);
		if (ret != len)
			return sys_errmsg("cannot write %d bytes to mtd%d "
					  "(eraseblock %d, offset %d)",
					  len, mtd->mtd_num, eb, offs);
	}

	return 0;
}

/*
 * [한국어]
 * do_oob_op - OOB(Out-Of-Band) 읽기/쓰기의 공통 구현
 *
 * OOB는 NAND 플래시 페이지의 추가 영역으로, ECC(Error Correction Code)와
 * 메타데이터를 저장한다. 먼저 64비트 ioctl(MEMREADOOB64/MEMWRITEOOB64)을
 * 시도하고, 지원하지 않으면 32비트 ioctl로 폴백한다.
 */
static int do_oob_op(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
	      uint64_t start, uint64_t length, void *data, unsigned int cmd64,
	      unsigned int cmd)
{
	int ret, oob_offs;
	struct mtd_oob_buf64 oob64;
	struct mtd_oob_buf oob;
	unsigned long long max_offs;
	const char *cmd64_str, *cmd_str;
	struct libmtd *lib = (struct libmtd *)desc;

	if (cmd64 ==  MEMREADOOB64) {
		cmd64_str = "MEMREADOOB64";
		cmd_str   = "MEMREADOOB";
	} else {
		cmd64_str = "MEMWRITEOOB64";
		cmd_str   = "MEMWRITEOOB";
	}

	max_offs = (unsigned long long)mtd->eb_cnt * mtd->eb_size;
	if (start >= max_offs) {
		errmsg("bad page address %" PRIu64 ", mtd%d has %d eraseblocks (%llu bytes)",
		       start, mtd->mtd_num, mtd->eb_cnt, max_offs);
		errno = EINVAL;
		return -1;
	}

	oob_offs = start & (mtd->min_io_size - 1);
	if (oob_offs + length > mtd->oob_size || length == 0) {
		errmsg("Cannot write %" PRIu64 " OOB bytes to address %" PRIu64 " (OOB offset %u) - mtd%d OOB size is only %d bytes",
		       length, start, oob_offs, mtd->mtd_num,  mtd->oob_size);
		errno = EINVAL;
		return -1;
	}

	oob64.start = start;
	oob64.length = length;
	oob64.usr_ptr = (uint64_t)(unsigned long)data;

	if (lib->offs64_ioctls == OFFS64_IOCTLS_SUPPORTED ||
	    lib->offs64_ioctls == OFFS64_IOCTLS_UNKNOWN) {
		ret = ioctl(fd, cmd64, &oob64);
		if (ret == 0)
			return ret;

		if (errno != ENOTTY ||
		    lib->offs64_ioctls != OFFS64_IOCTLS_UNKNOWN) {
			sys_errmsg("%s ioctl failed for mtd%d, offset %" PRIu64 " (eraseblock %" PRIu64 ")",
				   cmd64_str, mtd->mtd_num, start, start / mtd->eb_size);
		}

		/*
		 * MEMREADOOB64/MEMWRITEOOB64 support was added in kernel
		 * version 2.6.31, so probably we are working with older kernel
		 * and these ioctls are not supported.
		 */
		lib->offs64_ioctls = OFFS64_IOCTLS_NOT_SUPPORTED;
	}

	if (oob64.start > 0xFFFFFFFFULL) {
		errmsg("this system can address only up to address %lu",
		       0xFFFFFFFFUL);
		errno = EINVAL;
		return -1;
	}

	oob.start = oob64.start;
	oob.length = oob64.length;
	oob.ptr = data;

	ret = ioctl(fd, cmd, &oob);
	if (ret < 0)
		sys_errmsg("%s ioctl failed for mtd%d, offset %" PRIu64 " (eraseblock %" PRIu64 ")",
			   cmd_str, mtd->mtd_num, start, start / mtd->eb_size);
	return ret;
}

int mtd_read_oob(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		 uint64_t start, uint64_t length, void *data)
{
	return do_oob_op(desc, mtd, fd, start, length, data,
			 MEMREADOOB64, MEMREADOOB);
}

int mtd_write_oob(libmtd_t desc, const struct mtd_dev_info *mtd, int fd,
		  uint64_t start, uint64_t length, void *data)
{
	return do_oob_op(desc, mtd, fd, start, length, data,
			 MEMWRITEOOB64, MEMWRITEOOB);
}

/*
 * [한국어]
 * mtd_write_img - 이미지 파일을 MTD 장치에 기록
 *
 * @mtd: MTD 장치 정보
 * @fd: 장치 파일 디스크립터
 * @eb: 시작 소거블록 번호
 * @offs: 시작 오프셋
 * @img_name: 기록할 이미지 파일 경로
 * @return: 성공 시 0, 실패 시 -1
 *
 * 이미지 파일을 열어 MTD 장치에 소거블록 단위로 기록한다.
 * 이미지 크기가 서브페이지에 정렬되어야 하고, 장치 범위를 넘지 않아야 한다.
 */
int mtd_write_img(const struct mtd_dev_info *mtd, int fd, int eb, int offs,
		  const char *img_name)
{
	int tmp, ret, in_fd, len, written = 0;
	off_t seek;
	struct stat st;
	char *buf;

	ret = mtd_valid_erase_block(mtd, eb);
	if (ret)
		return ret;

	if (offs < 0 || offs >= mtd->eb_size) {
		errmsg("bad offset %d, mtd%d eraseblock size is %d",
		       offs, mtd->mtd_num, mtd->eb_size);
		errno = EINVAL;
		return -1;
	}
	if (offs % mtd->subpage_size) {
		errmsg("write offset %d is not aligned to mtd%d min. I/O size %d",
		       offs, mtd->mtd_num, mtd->subpage_size);
		errno = EINVAL;
		return -1;
	}

	in_fd = open(img_name, O_RDONLY | O_CLOEXEC);
	if (in_fd == -1)
		return sys_errmsg("cannot open \"%s\"", img_name);

	if (fstat(in_fd, &st)) {
		sys_errmsg("cannot stat %s", img_name);
		goto out_close;
	}

	len = st.st_size;
	if (len % mtd->subpage_size) {
		errmsg("size of \"%s\" is %d byte, which is not aligned to "
		       "mtd%d min. I/O size %d", img_name, len, mtd->mtd_num,
		       mtd->subpage_size);
		errno = EINVAL;
		goto out_close;
	}
	tmp = (offs + len + mtd->eb_size - 1) / mtd->eb_size;
	if (eb + tmp > mtd->eb_cnt) {
		errmsg("\"%s\" image size is %d bytes, mtd%d size is %d "
		       "eraseblocks, the image does not fit if we write it "
		       "starting from eraseblock %d, offset %d",
		       img_name, len, mtd->mtd_num, mtd->eb_cnt, eb, offs);
		errno = EINVAL;
		goto out_close;
	}

	/* Seek to the beginning of the eraseblock */
	seek = (off_t)eb * mtd->eb_size + offs;
	if (lseek(fd, seek, SEEK_SET) != seek) {
		sys_errmsg("cannot seek mtd%d to offset %"PRIdoff_t,
			    mtd->mtd_num, seek);
		goto out_close;
	}

	buf = xmalloc(mtd->eb_size);

	while (written < len) {
		int rd = 0;

		do {
			ret = read(in_fd, buf, mtd->eb_size - offs - rd);
			if (ret == -1) {
				sys_errmsg("cannot read \"%s\"", img_name);
				goto out_free;
			}
			rd += ret;
		} while (ret && rd < mtd->eb_size - offs);

		ret = write(fd, buf, rd);
		if (ret != rd) {
			sys_errmsg("cannot write %d bytes to mtd%d (eraseblock %d, offset %d)",
				   len, mtd->mtd_num, eb, offs);
			goto out_free;
		}

		offs = 0;
		eb += 1;
		written += rd;
	}

	free(buf);
	close(in_fd);
	return 0;

out_free:
	free(buf);
out_close:
	close(in_fd);
	return -1;
}

/*
 * [한국어]
 * mtd_probe_node - 노드가 MTD 장치인지 확인
 *
 * @desc: 라이브러리 디스크립터
 * @node: 확인할 장치 노드 경로
 * @return: MTD 장치이면 1, 아니면 -1 (errno=ENODEV)
 *
 * 노드의 major:minor를 시스템의 모든 MTD 장치와 비교하여 확인한다.
 */
int mtd_probe_node(libmtd_t desc, const char *node)
{
	struct stat st;
	struct mtd_info info;
	int i, mjr, mnr;
	struct libmtd *lib = (struct libmtd *)desc;

	if (stat(node, &st))
		return sys_errmsg("cannot get information about \"%s\"", node);

	if (!S_ISCHR(st.st_mode)) {
		errmsg("\"%s\" is not a character device", node);
		errno = EINVAL;
		return -1;
	}

	mjr = major(st.st_rdev);
	mnr = minor(st.st_rdev);

	if (mtd_get_info((libmtd_t *)lib, &info))
		return -1;

	if (!lib->sysfs_supported)
		return 0;

	for (i = info.lowest_mtd_num; i <= info.highest_mtd_num; i++) {
		int mjr1, mnr1, ret;

		ret = dev_get_major(lib, i, &mjr1, &mnr1);
		if (ret) {
			if (errno == ENOENT)
				continue;
			if (!errno)
				break;
			return -1;
		}

		if (mjr1 == mjr && mnr1 == mnr)
			return 1;
	}

	errno = 0;
	return -1;
}
