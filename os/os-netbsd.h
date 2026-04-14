/*
 * [한국어 설명] NetBSD 플랫폼 OS 추상화 헤더 (os-netbsd.h)
 *
 * === 파일의 역할 ===
 * NetBSD에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * DIOCGDINFO ioctl로 디스크 라벨 기반 크기 조회, _lwp_self()로 스레드 ID,
 * sysctl(HW_PHYSMEM64)로 물리 메모리 조회, statvfs로 여유 공간 조회.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 __NetBSD__ 감지 시 포함됨.
 *
 * === 주요 함수 요약 ===
 * - blockdev_size(): DIOCGDINFO ioctl (disklabel 사용)
 * - gettid(): _lwp_self() 래퍼
 * - get_fs_free_size(): statvfs로 여유 공간 조회
 */
#ifndef FIO_OS_NETBSD_H
#define FIO_OS_NETBSD_H

#define	FIO_OS	os_netbsd

#include <errno.h>
#include <lwp.h>
#include <sys/param.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/dkio.h>
#include <sys/disklabel.h>
#include <sys/endian.h>
#include <sys/sysctl.h>

/* XXX hack to avoid conflicts between rbtree.h and <sys/rbtree.h> */
#undef rb_node
#undef rb_left
#undef rb_right

#include "../file.h"

#define FIO_HAVE_ODIRECT
#define FIO_USE_GENERIC_INIT_RANDOM_STATE
#define FIO_HAVE_FS_STAT
#define FIO_HAVE_GETTID

#define OS_MAP_ANON		MAP_ANON

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 4096
#endif

#define fio_swap16(x)	bswap16(x)
#define fio_swap32(x)	bswap32(x)
#define fio_swap64(x)	bswap64(x)

#ifdef CONFIG_PTHREAD_GETAFFINITY
#define FIO_HAVE_GET_THREAD_AFFINITY
#define fio_get_thread_affinity(mask)	\
	pthread_getaffinity_np(pthread_self(), sizeof(mask), &(mask))
#endif

static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	struct disklabel dl;

	if (!ioctl(f->fd, DIOCGDINFO, &dl)) {
		*bytes = ((unsigned long long)dl.d_secperunit) * dl.d_secsize;
		return 0;
	}

	*bytes = 0;
	return errno;
}

static inline int blockdev_invalidate_cache(struct fio_file *f)
{
	return ENOTSUP;
}

static inline unsigned long long os_phys_mem(void)
{
	int mib[2] = { CTL_HW, HW_PHYSMEM64 };
	uint64_t mem;
	size_t len = sizeof(mem);

	sysctl(mib, 2, &mem, &len, NULL, 0);
	return mem;
}

#ifndef CONFIG_HAVE_GETTID
static inline int gettid(void)
{
	return (int) _lwp_self();
}
#endif

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
