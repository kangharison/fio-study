/*
 * [한국어 설명] 장치 마운트 상태 확인 (mountcheck.c)
 *
 * === 파일의 역할 ===
 * 주어진 장치(device)가 현재 파일시스템에 마운트되어 있는지 확인하는 함수를 제공한다.
 * Linux(getmntent), BSD(getmntinfo), NetBSD(getmntinfo+statvfs) 등 플랫폼별로
 * 다른 API를 사용하여 마운트 테이블을 검색한다.
 *
 * === fio에서의 사용 ===
 * fio가 블록 장치에 직접 I/O를 수행하기 전에 안전 검사로 사용된다. 마운트된
 * 장치에 대한 직접 쓰기는 파일시스템 손상을 유발할 수 있으므로 사전에 경고한다.
 */
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_GETMNTENT
#include <mntent.h>

#include "mountcheck.h"

#define MTAB	"/etc/mtab"

int device_is_mounted(const char *dev)
{
	FILE *mtab;
	struct mntent *mnt;
	int ret = 0;

	mtab = setmntent(MTAB, "r");
	if (!mtab)
		return 0;

	while ((mnt = getmntent(mtab)) != NULL) {
		if (!mnt->mnt_fsname)
			continue;
		if (!strcmp(mnt->mnt_fsname, dev)) {
			ret = 1;
			break;
		}
	}

	endmntent(mtab);
	return ret;
}

#elif defined(CONFIG_GETMNTINFO)
/* for most BSDs */
#include <sys/param.h>
#include <sys/mount.h>

int device_is_mounted(const char *dev)
{
	struct statfs *st;
	int i, ret;

	ret = getmntinfo(&st, MNT_NOWAIT);
	if (ret <= 0)
		return 0;

	for (i = 0; i < ret; i++) {
		if (!strcmp(st[i].f_mntfromname, dev))
			return 1;
	}

	return 0;
}

#elif defined(CONFIG_GETMNTINFO_STATVFS)
/* for NetBSD */
#include <sys/statvfs.h>

int device_is_mounted(const char *dev)
{
	struct statvfs *st;
	int i, ret;

	ret = getmntinfo(&st, MNT_NOWAIT);
	if (ret <= 0)
		return 0;

	for (i = 0; i < ret; i++) {
		if (!strcmp(st[i].f_mntfromname, dev))
			return 1;
	}

	return 0;
}

#else
/* others */

int device_is_mounted(const char *dev)
{
	return 0;
}

#endif
