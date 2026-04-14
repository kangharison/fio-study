/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 */
/*
 * [한국어 설명] Linux 구역 블록 장치(ZBD) 지원 구현 (linux-blkzoned.c)
 *
 * === 파일의 역할 ===
 * Linux 커널의 ZBD(Zoned Block Device) ioctl 인터페이스를 사용하여
 * 구역 블록 장치에 대한 조회, 리셋, 완료 등의 작업을 구현한다.
 * SMR(Shingled Magnetic Recording) HDD나 ZNS(Zoned Namespace) SSD 등
 * 구역 기반 저장 장치를 fio에서 테스트할 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 ZBD 지원 체인:
 *   zbd.c (ZBD 정책) → blkzoned.h (인터페이스) → [linux-blkzoned.c] (Linux 구현)
 * sysfs에서 장치 속성을 읽고, ioctl로 구역 작업을 수행한다.
 *
 * === 타 모듈과의 연결 ===
 * - zbd.c: 이 파일의 함수들을 호출하는 주요 모듈
 * - blkzoned.h: 인터페이스 선언
 * - zbd_types.h: zbd_zone, zbd_zoned_model 타입 정의
 * - <linux/blkzoned.h>: 커널 ioctl 구조체 (blk_zone, blk_zone_report 등)
 *
 * === 주요 함수 요약 ===
 * - blkzoned_get_zoned_model(): sysfs에서 장치의 ZBD 모델 조회
 * - blkzoned_report_zones(): BLKREPORTZONE ioctl로 구역 정보 수집
 * - blkzoned_reset_wp(): BLKRESETZONE ioctl로 쓰기 포인터 리셋
 * - blkzoned_finish_zone(): BLKFINISHZONE ioctl로 구역 완료
 * - blkzoned_move_zone_wp(): pwrite/fallocate로 쓰기 포인터 이동
 */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file.h"
#include "fio.h"
#include "lib/pow2.h"
#include "log.h"
#include "oslib/asprintf.h"
#include "smalloc.h"
#include "verify.h"
#include "zbd_types.h"

#include <linux/blkzoned.h>
/* [한국어] BLKFINISHZONE: 구역 완료 ioctl - 커널 5.5 이전에는 정의되지 않을 수 있음 */
#ifndef BLKFINISHZONE
#define BLKFINISHZONE _IOW(0x12, 136, struct blk_zone_range)
#endif
#include <linux/falloc.h>

/*
 * [한국어] 시스템의 uapi 헤더가 구역 용량(capacity) 필드를 지원하지 않는 경우,
 * 로컬 버전의 구조체를 정의한다. 구역 용량은 구역 크기(len)보다 작을 수 있으며,
 * 이는 ZNS SSD에서 구역의 실제 쓸 수 있는 용량을 나타낸다.
 */
/*
 * If the uapi headers installed on the system lacks zone capacity support,
 * use our local versions. If the installed headers are recent enough to
 * support zone capacity, do not redefine any structs.
 */
#ifndef CONFIG_HAVE_REP_CAPACITY
#define BLK_ZONE_REP_CAPACITY	(1 << 0)

struct blk_zone_v2 {
	__u64	start;          /* Zone start sector */
	__u64	len;            /* Zone length in number of sectors */
	__u64	wp;             /* Zone write pointer position */
	__u8	type;           /* Zone type */
	__u8	cond;           /* Zone condition */
	__u8	non_seq;        /* Non-sequential write resources active */
	__u8	reset;          /* Reset write pointer recommended */
	__u8	resv[4];
	__u64	capacity;       /* Zone capacity in number of sectors */
	__u8	reserved[24];
};
#define blk_zone blk_zone_v2

struct blk_zone_report_v2 {
	__u64	sector;
	__u32	nr_zones;
	__u32	flags;
struct blk_zone zones[0];
};
#define blk_zone_report blk_zone_report_v2
#endif /* CONFIG_HAVE_REP_CAPACITY */

/*
 * [한국어]
 * read_file - sysfs 파일에서 첫 줄을 읽어 반환하는 헬퍼
 *
 * @path: 읽을 파일 경로 (주로 /sys/dev/block/... 경로)
 * @return: 읽은 문자열의 동적 할당된 사본, 실패 시 NULL
 *
 * sysfs 속성 파일은 보통 한 줄로 값을 제공한다.
 * 첫 줄을 읽고 줄바꿈 문자를 제거한 후 strdup()으로 복사하여 반환한다.
 *
 * 호출 체인:
 *   blkzoned_get_sysfs_attr() → [read_file()] → fgets(), strdup()
 */
/*
 * Read up to 255 characters from the first line of a file. Strip the trailing
 * newline.
 */
static char *read_file(const char *path)
{
	char line[256], *p = line;
	FILE *f;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (!fgets(line, sizeof(line), f))
		line[0] = '\0';
	strsep(&p, "\n");
	fclose(f);

	return strdup(line);
}

/*
 * [한국어]
 * blkzoned_get_sysfs_attr - 블록 장치의 sysfs 속성값을 읽는 헬퍼
 *
 * @file_name: 블록 장치 파일 경로 (예: /dev/sda)
 * @attr: 읽을 sysfs 속성 이름 (예: "queue/zoned")
 * @return: 속성값 문자열 (호출자가 free() 해야 함), 실패 시 NULL
 *
 * 동작 과정:
 * 1) stat()으로 장치의 major:minor 번호를 얻음
 * 2) /sys/dev/block/major:minor 심볼릭 링크를 따라가 sysfs 경로 확인
 * 3) 파티션 장치인 경우 상위(홀더) 장치 경로로 전환
 * 4) sysfs 속성 파일을 읽어 값을 반환
 *
 * 호출 체인:
 *   blkzoned_get_zoned_model() 등 → [blkzoned_get_sysfs_attr()] → read_file()
 */
/*
 * Get the value of a sysfs attribute for a block device.
 *
 * Returns NULL on failure.
 * Returns a pointer to a string on success.
 * The caller is responsible for freeing the memory.
 */
static char *blkzoned_get_sysfs_attr(const char *file_name, const char *attr)
{
	char *attr_path = NULL;
	struct stat statbuf;
	char *sys_devno_path = NULL;
	char *part_attr_path = NULL;
	char *part_str = NULL;
	char sys_path[PATH_MAX];
	ssize_t sz;
	char *delim = NULL;
	char *attr_str = NULL;

	if (stat(file_name, &statbuf) < 0)
		goto out;

	if (asprintf(&sys_devno_path, "/sys/dev/block/%d:%d",
		     major(statbuf.st_rdev), minor(statbuf.st_rdev)) < 0)
		goto out;

	sz = readlink(sys_devno_path, sys_path, sizeof(sys_path) - 1);
	if (sz < 0)
		goto out;
	sys_path[sz] = '\0';

	/*
	 * If the device is a partition device, cut the device name in the
	 * canonical sysfs path to obtain the sysfs path of the holder device.
	 *   e.g.:  /sys/devices/.../sda/sda1 -> /sys/devices/.../sda
	 */
	if (asprintf(&part_attr_path, "/sys/dev/block/%s/partition",
		     sys_path) < 0)
		goto out;
	part_str = read_file(part_attr_path);
	if (part_str && *part_str == '1') {
		delim = strrchr(sys_path, '/');
		if (!delim)
			goto out;
		*delim = '\0';
	}

	if (asprintf(&attr_path,
		     "/sys/dev/block/%s/%s", sys_path, attr) < 0)
		goto out;

	attr_str = read_file(attr_path);
out:
	free(attr_path);
	free(part_str);
	free(part_attr_path);
	free(sys_devno_path);

	return attr_str;
}

/*
 * [한국어]
 * blkzoned_get_zoned_model - 블록 장치의 ZBD 모델을 조회
 *
 * @td: 스레드 데이터 (현재 사용하지 않으나 인터페이스 통일)
 * @f: 대상 파일 구조체
 * @model: ZBD 모델이 저장될 포인터 (ZBD_NONE/ZBD_HOST_AWARE/ZBD_HOST_MANAGED)
 * @return: 성공 시 0, 블록 장치가 아니면 -EINVAL
 *
 * sysfs의 queue/zoned 속성을 읽어 장치가 host-aware, host-managed,
 * 또는 일반(none) 장치인지 판별한다.
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_get_zoned_model()] → blkzoned_get_sysfs_attr()
 */
int blkzoned_get_zoned_model(struct thread_data *td, struct fio_file *f,
			     enum zbd_zoned_model *model)
{
	char *model_str = NULL;

	if (f->filetype != FIO_TYPE_BLOCK)
		return -EINVAL;

	*model = ZBD_NONE;

	model_str = blkzoned_get_sysfs_attr(f->file_name, "queue/zoned");
	if (!model_str)
		return 0;

	dprint(FD_ZBD, "%s: zbd model string: %s\n", f->file_name, model_str);
	if (strcmp(model_str, "host-aware") == 0)
		*model = ZBD_HOST_AWARE;
	else if (strcmp(model_str, "host-managed") == 0)
		*model = ZBD_HOST_MANAGED;

	free(model_str);

	return 0;
}

/*
 * [한국��]
 * blkzoned_get_max_open_zones - 장치의 최대 동시 오픈 구역 수 조회
 *
 * @td: 스레드 데이터
 * @f: 대상 파일 구조체
 * @max_open_zones: 결과가 저장될 포인터 (0이면 제한 없음)
 * @return: 성공 시 0, 블록 장치가 아니면 -EIO
 *
 * sysfs의 queue/max_open_zones 속성을 읽는다.
 * 동시에 열 수 있는 구역 수가 제한된 장치에서 fio가 적절히 I/O를 분배하는 데 사용된다.
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_get_max_open_zones()] → blkzoned_get_sysfs_attr()
 */
int blkzoned_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				unsigned int *max_open_zones)
{
	char *max_open_str;

	if (f->filetype != FIO_TYPE_BLOCK)
		return -EIO;

	max_open_str = blkzoned_get_sysfs_attr(f->file_name, "queue/max_open_zones");
	if (!max_open_str) {
		*max_open_zones = 0;
		return 0;
	}

	dprint(FD_ZBD, "%s: max open zones supported by device: %s\n",
	       f->file_name, max_open_str);
	*max_open_zones = atoll(max_open_str);

	free(max_open_str);

	return 0;
}

/*
 * [한국어]
 * blkzoned_get_max_active_zones - 장치의 최대 활성 구역 수 조회
 *
 * @td: 스레드 데이터
 * @f: 대상 파일 구조체
 * @max_active_zones: 결과가 저장될 포인터 (0이면 제한 없음)
 * @return: 성공 시 0, 블록 장치가 아니면 -EIO
 *
 * sysfs의 queue/max_active_zones 속성을 읽는다.
 * 활성 구역은 오픈/클로즈/비어있지 않은 상태의 구역을 포함한다.
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_get_max_active_zones()] → blkzoned_get_sysfs_attr()
 */
int blkzoned_get_max_active_zones(struct thread_data *td, struct fio_file *f,
				  unsigned int *max_active_zones)
{
	char *max_active_str;

	if (f->filetype != FIO_TYPE_BLOCK)
		return -EIO;

	max_active_str = blkzoned_get_sysfs_attr(f->file_name, "queue/max_active_zones");
	if (!max_active_str) {
		*max_active_zones = 0;
		return 0;
	}

	dprint(FD_ZBD, "%s: max active zones supported by device: %s\n",
	       f->file_name, max_active_str);
	*max_active_zones = atoll(max_active_str);

	free(max_active_str);

	return 0;
}

/*
 * [���국어]
 * zone_capacity - 구역의 실제 용량을 바이트 단위로 반환
 *
 * @hdr: 구역 리포트 헤더 (capacity 지원 여부 플래그 포함)
 * @blkz: 개별 구역 정보
 * @return: 구역 용량(바이트). capacity 필드를 지원하지 않으면 len 사용
 *
 * ZNS SSD에서는 구역 용량(capacity)이 구역 크기(len)보다 작을 수 있다.
 * BLK_ZONE_REP_CAPACITY 플래그로 커널이 capacity 필드를 지원하는지 확인하고,
 * 지원하면 capacity, 아니면 len을 사용한다. << 9는 섹터(512B)를 바이트로 변환.
 */
static uint64_t zone_capacity(struct blk_zone_report *hdr,
			      struct blk_zone *blkz)
{
	if (hdr->flags & BLK_ZONE_REP_CAPACITY)
		return blkz->capacity << 9;
	return blkz->len << 9;
}

/*
 * [한국어]
 * blkzoned_report_zones - BLKREPORTZONE ioctl로 구역 정보를 수집
 *
 * @td: 스레드 데이터
 * @f: 대상 파일 구조체
 * @offset: 리포트 시작 오프셋(바이트)
 * @zones: 구역 정보가 채워질 배열
 * @nr_zones: 조회할 최대 구역 수
 * @return: 성공 시 실제 리포트된 구역 수, 실패 시 음수 에러코드
 *
 * 동작 과정:
 * 1) 장치를 읽기 전용으로 오픈
 * 2) blk_zone_report 구조체를 할당하고 BLKREPORTZONE ioctl 호출
 * 3) 커널의 blk_zone 구조체를 fio의 zbd_zone 구조체로 변환
 *    - 섹터 단위를 바이트 단위로 변환 (<< 9)
 *    - 구역 타입/상태를 fio 내부 enum으로 매핑
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_report_zones()] → ioctl(BLKREPORTZONE)
 */
int blkzoned_report_zones(struct thread_data *td, struct fio_file *f,
			  uint64_t offset, struct zbd_zone *zones,
			  unsigned int nr_zones)
{
	struct blk_zone_report *hdr = NULL;
	struct blk_zone *blkz;
	struct zbd_zone *z;
	unsigned int i;
	int fd = -1, ret;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	hdr = calloc(1, sizeof(struct blk_zone_report) +
			nr_zones * sizeof(struct blk_zone));
	if (!hdr) {
		ret = -ENOMEM;
		goto out;
	}

	hdr->nr_zones = nr_zones;
	hdr->sector = offset >> 9;
	ret = ioctl(fd, BLKREPORTZONE, hdr);
	if (ret) {
		log_err("%s: BLKREPORTZONE ioctl failed, ret=%d, err=%d.\n",
			f->file_name, ret, -errno);
		ret = -errno;
		goto out;
	}

	nr_zones = hdr->nr_zones;
	blkz = (void *) hdr + sizeof(*hdr);
	z = &zones[0];
	for (i = 0; i < nr_zones; i++, z++, blkz++) {
		z->start = blkz->start << 9;
		z->wp = blkz->wp << 9;
		z->len = blkz->len << 9;
		z->capacity = zone_capacity(hdr, blkz);

		switch (blkz->type) {
		case BLK_ZONE_TYPE_CONVENTIONAL:
			z->type = ZBD_ZONE_TYPE_CNV;
			break;
		case BLK_ZONE_TYPE_SEQWRITE_REQ:
			z->type = ZBD_ZONE_TYPE_SWR;
			break;
		case BLK_ZONE_TYPE_SEQWRITE_PREF:
			z->type = ZBD_ZONE_TYPE_SWP;
			break;
		default:
			td_verror(td, errno, "invalid zone type");
			log_err("%s: invalid type for zone at sector %llu.\n",
				f->file_name, (unsigned long long)offset >> 9);
			ret = -EIO;
			goto out;
		}

		switch (blkz->cond) {
		case BLK_ZONE_COND_NOT_WP:
			z->cond = ZBD_ZONE_COND_NOT_WP;
			break;
		case BLK_ZONE_COND_EMPTY:
			z->cond = ZBD_ZONE_COND_EMPTY;
			break;
		case BLK_ZONE_COND_IMP_OPEN:
			z->cond = ZBD_ZONE_COND_IMP_OPEN;
			break;
		case BLK_ZONE_COND_EXP_OPEN:
			z->cond = ZBD_ZONE_COND_EXP_OPEN;
			break;
		case BLK_ZONE_COND_CLOSED:
			z->cond = ZBD_ZONE_COND_CLOSED;
			break;
		case BLK_ZONE_COND_FULL:
			z->cond = ZBD_ZONE_COND_FULL;
			break;
		case BLK_ZONE_COND_READONLY:
		case BLK_ZONE_COND_OFFLINE:
		default:
			/* Treat all these conditions as offline (don't use!) */
			z->cond = ZBD_ZONE_COND_OFFLINE;
			z->wp = z->start;
		}
	}

	ret = nr_zones;
out:
	free(hdr);
	close(fd);

	return ret;
}

/*
 * [한국어]
 * blkzoned_reset_wp - BLKRESETZONE ioctl로 구역의 쓰기 포인터를 리셋
 *
 * @td: 스레드 데이터
 * @f: 대상 파일 구조체
 * @offset: 리셋할 구역의 시작 오프셋(바이트)
 * @length: 리셋할 범위 길이(바이트)
 * @return: 성공 시 0, 실패 시 음수 에러코드
 *
 * 구역의 쓰기 포인터를 구역 시작으로 되돌린다 (구역 내용이 무효화됨).
 * 파일이 아직 열리지 않은 경우 임시로 열어서 ioctl을 수행한다.
 * >> 9로 바이트를 섹터 단위로 변환하여 커널에 전달한다.
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_reset_wp()] → ioctl(BLKRESETZONE)
 */
int blkzoned_reset_wp(struct thread_data *td, struct fio_file *f,
		      uint64_t offset, uint64_t length)
{
	struct blk_zone_range zr = {
		.sector         = offset >> 9,
		.nr_sectors     = length >> 9,
	};
	int fd, ret = 0;

	/* If the file is not yet opened, open it for this function. */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_RDWR | O_LARGEFILE);
		if (fd < 0)
			return -errno;
	}

	if (ioctl(fd, BLKRESETZONE, &zr) < 0)
		ret = -errno;

	if (f->fd < 0)
		close(fd);

	return ret;
}

/*
 * [한국어]
 * blkzoned_finish_zone - BLKFINISHZONE ioctl로 구역을 완료 상태로 전환
 *
 * @td: 스레드 데이터
 * @f: 대상 파일 구조체
 * @offset: 완료할 구역의 시작 오프셋(바이트)
 * @length: 범위 길이(바이트)
 * @return: 성공 시 0, 실패 시 음수 에러코드
 *
 * 구역을 FULL 상태로 전환한다. 커널 5.5 이전에는 BLKFINISHZONE을 지원하지 않으며,
 * 이 경우 ENOTTY가 반환되고 에러를 무시한다 (구형 커널은 자동 완료를 지원).
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_finish_zone()] → ioctl(BLKFINISHZONE)
 */
int blkzoned_finish_zone(struct thread_data *td, struct fio_file *f,
			 uint64_t offset, uint64_t length)
{
	struct blk_zone_range zr = {
		.sector         = offset >> 9,
		.nr_sectors     = length >> 9,
	};
	int fd, ret = 0;

	/* If the file is not yet opened, open it for this function. */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_RDWR | O_LARGEFILE);
		if (fd < 0)
			return -errno;
	}

	if (ioctl(fd, BLKFINISHZONE, &zr) < 0) {
		ret = -errno;
		/*
		 * Kernel versions older than 5.5 do not support BLKFINISHZONE
		 * and return the ENOTTY error code. These old kernels only
		 * support block devices that close zones automatically.
		 */
		if (ret == ENOTTY)
			ret = 0;
	}

	if (f->fd < 0)
		close(fd);

	return ret;
}

/*
 * [한국어]
 * blkzoned_move_zone_wp - 데이터를 기록하여 쓰기 포인터를 이동
 *
 * @td: 스레드 데이터
 * @f: 대상 파일 구조체
 * @z: 대상 구역 정보 (wp 위치 사용)
 * @length: 기록할 길이(바이트)
 * @buf: 기록할 데이터 버퍼 (NULL이면 0으로 채움)
 * @return: 성공 시 0, 실패 시 음수 에러코드
 *
 * buf가 NULL이면 fallocate(FALLOC_FL_ZERO_RANGE)로 0을 채워 쓰기 포인터를 이동하고,
 * buf가 있으면 pwrite()로 실제 데이터를 기록한다.
 * O_DIRECT로 열어 페이지 캐시를 우회한다.
 *
 * 호출 체인:
 *   zbd.c → [blkzoned_move_zone_wp()] → pwrite() / fallocate()
 */
int blkzoned_move_zone_wp(struct thread_data *td, struct fio_file *f,
			  struct zbd_zone *z, uint64_t length, const char *buf)
{
	int fd, ret = 0;

	/* If the file is not yet open, open it for this function */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_WRONLY | O_DIRECT);
		if (fd < 0)
			return -errno;
	}

	/* If write data is not provided, fill zero to move the write pointer */
	if (!buf) {
		ret = fallocate(fd, FALLOC_FL_ZERO_RANGE, z->wp, length);
		goto out;
	}

	if (pwrite(fd, buf, length, z->wp) < 0)
		ret = -errno;

out:
	if (f->fd < 0)
		close(fd);

	return ret;
}
