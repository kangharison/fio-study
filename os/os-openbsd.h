/*
 * [한국어 설명] OpenBSD 플랫폼 OS 추상화 헤더 (os-openbsd.h)
 *
 * === 파일의 역할 ===
 * OpenBSD에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * DIOCGDINFO ioctl로 디스크 크기 조회, sysctl(HW_PHYSMEM64)로 물리 메모리 조회,
 * uname 기반 SHM attach 지원 여부 확인 (OpenBSD 5.1 이상).
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 __OpenBSD__ 감지 시 포함됨.
 *
 * === 주요 함수 요약 ===
 * - blockdev_size(): DIOCGDINFO ioctl (disklabel)
 * - shm_attach_to_open_removed(): OpenBSD 버전 체크 (5.1+)
 */
#ifndef FIO_OS_OPENBSD_H
#define FIO_OS_OPENBSD_H

#define	FIO_OS	os_openbsd

#include <errno.h>
#include <sys/param.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/dkio.h>
#include <sys/disklabel.h>
#include <sys/endian.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>

/* XXX hack to avoid conflicts between rbtree.h and <sys/tree.h> */
#undef RB_BLACK
#undef RB_RED
#undef RB_ROOT

#include "../file.h"

#define FIO_USE_GENERIC_INIT_RANDOM_STATE
#define FIO_HAVE_FS_STAT
#define FIO_HAVE_GETTID
#define FIO_HAVE_SHM_ATTACH_REMOVED

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
	return (int)(intptr_t) pthread_self();
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

/*
 * [한국어]
 * shm_attach_to_open_removed - 삭제된 SHM 세그먼트에 attach 가능 여부 확인
 * OpenBSD 5.1 이상에서 지원. uname으로 버전을 파싱하여 확인.
 * smalloc.c에서 공유 메모리 할당 시 사용.
 */
static inline int shm_attach_to_open_removed(void)
{
	struct utsname uts;
	int major, minor;

	if (uname(&uts) == -1)
		return 0;

	/*
	 * Return 1 if >= OpenBSD 5.1 according to 97900ebf,
	 * assuming both major/minor versions are < 10.
	 */
	if (uts.release[0] > '9' || uts.release[0] < '0')
		return 0;
	if (uts.release[1] != '.')
		return 0;
	if (uts.release[2] > '9' || uts.release[2] < '0')
		return 0;

	major = uts.release[0] - '0';
	minor = uts.release[2] - '0';

	if (major > 5)
		return 1;
	if (major == 5 && minor >= 1)
		return 1;

	return 0;
}

#endif
