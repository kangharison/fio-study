/*
 * [한국어 설명] HP-UX 플랫폼 OS 추상화 헤더 (os-hpux.h)
 *
 * === 파일의 역할 ===
 * HP-UX에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * DIOC_DESCRIBE_EXT ioctl로 디스크 크기 조회, pstat으로 물리 메모리,
 * mpctl(MPC_GETNUMSPUS)로 CPU 수 조회. aiocb64 타입을 사용하는 특이사항.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 __hpux 감지 시 포함됨.
 *
 * === 주요 함수 요약 ===
 * - blockdev_size(): DIOC_DESCRIBE_EXT ioctl (disk_describe_type_ext_t)
 * - os_phys_mem(): pstat(PSTAT_STATIC)으로 물리 메모리 x 페이지 크기
 * - cpus_configured(): mpctl(MPC_GETNUMSPUS)
 */
#ifndef FIO_OS_HPUX_H
#define FIO_OS_HPUX_H

#define	FIO_OS	os_hpux

#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/fadvise.h>
#include <sys/mman.h>
#include <sys/mpctl.h>
#include <sys/diskio.h>
#include <sys/param.h>
#include <sys/pstat.h>
#include <time.h>
#include <aio.h>
#include <arm.h>

#include "../file.h"

#define FIO_HAVE_ODIRECT
#define FIO_USE_GENERIC_INIT_RANDOM_STATE
#define FIO_HAVE_CHARDEV_SIZE

#define OS_MAP_ANON		MAP_ANONYMOUS
#define OS_MSG_DONTWAIT		0

#define POSIX_MADV_DONTNEED	MADV_DONTNEED
#define POSIX_MADV_SEQUENTIAL	MADV_SEQUENTIAL
#define POSIX_MADV_RANDOM	MADV_RANDOM
#define posix_madvise(ptr, sz, hint)	madvise((ptr), (sz), (hint))

#ifndef MSG_WAITALL
#define MSG_WAITALL	0x40
#endif

#define FIO_USE_GENERIC_SWAP

/* [한국어] HP-UX는 64비트 AIO 구조체(aiocb64)를 사용하므로 별도 typedef 필요 */
#define FIO_OS_HAVE_AIOCB_TYPEDEF

#ifdef CONFIG_PTHREAD_GETAFFINITY
#define FIO_HAVE_GET_THREAD_AFFINITY
#define fio_get_thread_affinity(mask)	\
	pthread_getaffinity_np(pthread_self(), sizeof(mask), &(mask))
#endif

typedef struct aiocb64 os_aiocb_t;

static inline int blockdev_invalidate_cache(struct fio_file *f)
{
	return ENOTSUP;
}

static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	disk_describe_type_ext_t dext;

	if (!ioctl(f->fd, DIOC_DESCRIBE_EXT, &dext)) {
		unsigned long long lba;

		lba = ((uint64_t) dext.maxsva_high << 32) | dext.maxsva_low;
		*bytes = lba * dext.lgblksz;
		return 0;
	}

	*bytes = 0;
	return errno;
}

static inline int chardev_size(struct fio_file *f, unsigned long long *bytes)
{
	return blockdev_size(f, bytes);
}

static inline unsigned long long os_phys_mem(void)
{
	unsigned long long ret;
	struct pst_static pst;
	union pstun pu;

	pu.pst_static = &pst;
	if (pstat(PSTAT_STATIC, pu, sizeof(pst), 0, 0) == -1)
		return 0;

	ret = pst.physical_memory;
	ret *= pst.page_size;
	return ret;
}

#define FIO_HAVE_CPU_CONF_SYSCONF

static inline unsigned int cpus_configured(void)
{
	return mpctl(MPC_GETNUMSPUS, 0, NULL);
}

#endif
