/*
 * [한국어 설명] AIX 플랫폼 OS 추상화 헤더 (os-aix.h)
 *
 * === 파일의 역할 ===
 * IBM AIX에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * IOCINFO ioctl로 SCSI 디스크 크기 조회, sysconf(_SC_AIX_REALMEM)로
 * 물리 메모리 조회. 바이트 스왑은 범용(FIO_USE_GENERIC_SWAP) 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 _AIX 감지 시 포함됨.
 *
 * === 주요 함수 요약 ===
 * - blockdev_size(): IOCINFO ioctl (devinfo 구조체)
 * - os_phys_mem(): sysconf(_SC_AIX_REALMEM) * 1024
 */
#ifndef FIO_OS_AIX_H
#define FIO_OS_AIX_H

#define	FIO_OS	os_aix

#include <errno.h>
#include <unistd.h>
#include <sys/devinfo.h>
#include <sys/ioctl.h>

#include "../file.h"

#define FIO_HAVE_ODIRECT
#define FIO_USE_GENERIC_INIT_RANDOM_STATE

#define OS_MAP_ANON		MAP_ANON
#define OS_MSG_DONTWAIT		0

#define FIO_USE_GENERIC_SWAP

#ifdef CONFIG_PTHREAD_GETAFFINITY
#define FIO_HAVE_GET_THREAD_AFFINITY
#define fio_get_thread_affinity(mask)	\
	pthread_getaffinity_np(pthread_self(), sizeof(mask), &(mask))
#endif

static inline int blockdev_invalidate_cache(struct fio_file *f)
{
	return ENOTSUP;
}

static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	struct devinfo info;

	if (!ioctl(f->fd, IOCINFO, &info)) {
        	*bytes = (unsigned long long)info.un.scdk.numblks *
				info.un.scdk.blksize;
		return 0;
	}

	return errno;
}

static inline unsigned long long os_phys_mem(void)
{
	long mem = sysconf(_SC_AIX_REALMEM);

	if (mem == -1)
		return 0;

	return (unsigned long long) mem * 1024;
}

#endif
