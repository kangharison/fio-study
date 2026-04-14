/*
 * [한국어 설명] QNX 플랫폼 OS 추상화 헤더 (os-qnx.h)
 *
 * === 파일의 역할 ===
 * QNX RTOS에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * fstat으로 블록 디바이스 크기 조회, SYSPAGE를 통한 물리 메모리 계산,
 * SHM 미지원(FIO_NO_HAVE_SHM_H) 등 QNX 고유 특성을 처리.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 __QNX__ 감지 시 포함됨.
 *
 * === 주요 함수 요약 ===
 * - blockdev_size(): fstat()으로 blocksize * nblocks 계산
 * - os_phys_mem(): SYSPAGE asinfo에서 "ram" 영역 합산
 */
#ifndef FIO_OS_QNX_H
#define FIO_OS_QNX_H

#define	FIO_OS	os_qnx
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/syspage.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dcmd_cam.h>

/* XXX hack to avoid conflicts between rbtree.h and <sys/tree.h> */
#undef RB_BLACK
#undef RB_RED
#undef RB_ROOT

#include "../file.h"

/* QNX is not supporting SA_RESTART. Use SA_NOCLDSTOP instead of it */
#ifndef SA_RESTART
#define SA_RESTART SA_NOCLDSTOP
#endif

#define FIO_NO_HAVE_SHM_H

typedef uint64_t __u64;
typedef unsigned int __u32;

#define FIO_USE_GENERIC_INIT_RANDOM_STATE
#define FIO_HAVE_FS_STAT
#define FIO_HAVE_GETTID

#define OS_MAP_ANON		MAP_ANON

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 4096
#endif

#define fio_swap16(x)	swap16(x)
#define fio_swap32(x)	swap32(x)
#define fio_swap64(x)	swap64(x)

#ifdef CONFIG_PTHREAD_GETAFFINITY
#define FIO_HAVE_GET_THREAD_AFFINITY
#define fio_get_thread_affinity(mask)	\
	pthread_getaffinity_np(pthread_self(), sizeof(mask), &(mask))
#endif

static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	struct stat statbuf;

	if (fstat(f->fd, &statbuf) == -1) {
		*bytes = 0;
		return errno;
	}

	*bytes = (unsigned long long)(statbuf.st_blocksize * statbuf.st_nblocks);
	return 0;
}

static inline int blockdev_invalidate_cache(struct fio_file *f)
{
	return ENOTSUP;
}

/*
 * [한국어]
 * os_phys_mem - QNX 시스템 물리 메모리 조회
 * QNX의 SYSPAGE(시스템 페이지)에서 asinfo 항목을 순회하며
 * "ram" 타입 메모리 영역의 크기를 합산한다.
 */
static inline unsigned long long os_phys_mem(void)
{
	uint64_t mem = 0;
	const char *const strings = SYSPAGE_ENTRY(strings)->data;
	const struct asinfo_entry *const begin = SYSPAGE_ENTRY(asinfo);
	const struct asinfo_entry *const end = begin + SYSPAGE_ENTRY_SIZE(asinfo) / SYSPAGE_ELEMENT_SIZE(asinfo);

	assert(SYSPAGE_ELEMENT_SIZE(asinfo) == sizeof(struct asinfo_entry));

	for (const struct asinfo_entry *e = begin; e < end; ++e) {
		if (!strcmp(strings + e->name, "ram"))
			mem += e->end - e->start + 1;
	}
	return mem;
}

static inline unsigned long long get_fs_free_size(const char *path)
{
	unsigned long long ret;
	struct statvfs s;

	if (statvfs(path, &s) < 0)
		return -1ULL;

	ret = s.f_frsize;
	ret *= (unsigned long long) s.f_bfree;
	return ret;
}

#ifdef MADV_FREE
#define FIO_MADV_FREE	MADV_FREE
#endif

#endif
