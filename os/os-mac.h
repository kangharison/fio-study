/*
 * [한국어 설명] macOS(Apple) 플랫폼 OS 추상화 헤더 (os-mac.h)
 *
 * === 파일의 역할 ===
 * macOS에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * F_NOCACHE를 통한 O_DIRECT 에뮬레이션, DKIOC* ioctl로 디스크 크기 조회,
 * sysctl로 물리 메모리 조회, F_PREALLOCATE로 fallocate 에뮬레이션 등을 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 __APPLE__ 감지 시 포함됨.
 *
 * === 타 모듈과의 연결 ===
 * - mac/posix.h, mac/posix.c: posix_fadvise() 에뮬레이션
 * - file.h: fio_file 구조체
 *
 * === 주요 함수 요약 ===
 * - fio_set_odirect(): F_NOCACHE fcntl로 캐시 우회
 * - blockdev_size(): DKIOCGETBLOCKCOUNT/DKIOCGETBLOCKSIZE ioctl
 * - fio_fallocate(): F_PREALLOCATE fcntl + ftruncate
 * - os_phys_mem(): sysctl(HW_PHYSMEM)
 */
#ifndef FIO_OS_APPLE_H
#define FIO_OS_APPLE_H

#define	FIO_OS	os_mac

#include <errno.h>
#include <fcntl.h>
#include <sys/disk.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <mach/mach_init.h>
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>

#include "../arch/arch.h"
#include "../file.h"

#include "mac/posix.h"

#define FIO_USE_GENERIC_INIT_RANDOM_STATE
#define FIO_HAVE_GETTID
#define FIO_HAVE_CHARDEV_SIZE
#define FIO_HAVE_NATIVE_FALLOCATE
#define FIO_HAVE_CPU_HAS

#define OS_MAP_ANON		MAP_ANON

#define fio_swap16(x)	OSSwapInt16(x)
#define fio_swap32(x)	OSSwapInt32(x)
#define fio_swap64(x)	OSSwapInt64(x)

#ifdef CONFIG_PTHREAD_GETAFFINITY
#define FIO_HAVE_GET_THREAD_AFFINITY
#define fio_get_thread_affinity(mask)	\
	pthread_getaffinity_np(pthread_self(), sizeof(mask), &(mask))
#endif

/*
 * [한국어] macOS의 O_DIRECT 에뮬레이션
 * macOS는 O_DIRECT를 지원하지 않으므로 F_NOCACHE fcntl로 캐시를 우회.
 * direct=1 옵션 사용 시 호출됨.
 */
#define FIO_OS_DIRECTIO
static inline int fio_set_odirect(struct fio_file *f)
{
	if (fcntl(f->fd, F_NOCACHE, 1) == -1)
		return errno;
	return 0;
}

/* [한국어] macOS 블록 디바이스 크기 조회 - 블록 수 x 블록 크기 */
static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	uint32_t block_size;
	uint64_t block_count;

	if (ioctl(f->fd, DKIOCGETBLOCKCOUNT, &block_count) == -1)
		return errno;
	if (ioctl(f->fd, DKIOCGETBLOCKSIZE, &block_size) == -1)
		return errno;

	*bytes = block_size;
	*bytes *= block_count;
	return 0;
}

static inline int chardev_size(struct fio_file *f, unsigned long long *bytes)
{
	/*
	 * Could be a raw block device, this is better than just assuming
	 * we can't get the size at all.
	 */
	if (!blockdev_size(f, bytes))
		return 0;

	*bytes = -1ULL;
	return 0;
}

static inline int blockdev_invalidate_cache(struct fio_file *f)
{
	return ENOTSUP;
}

static inline unsigned long long os_phys_mem(void)
{
	int mib[2] = { CTL_HW, HW_PHYSMEM };
	unsigned long long mem;
	size_t len = sizeof(mem);

	sysctl(mib, 2, &mem, &len, NULL, 0);
	return mem;
}

#ifndef CONFIG_HAVE_GETTID
static inline int gettid(void)
{
	return mach_thread_self();
}
#endif

/* [한국어] macOS fallocate 에뮬레이션 - F_PREALLOCATE fcntl + ftruncate 조합 */
static inline bool fio_fallocate(struct fio_file *f, uint64_t offset, uint64_t len)
{
	fstore_t store = {F_ALLOCATEALL, F_PEOFPOSMODE, offset, len};
	if (fcntl(f->fd, F_PREALLOCATE, &store) != -1) {
		if (ftruncate(f->fd, len) == 0)
			return true;
	}

	return false;
}

static inline bool os_cpu_has(cpu_features feature)
{
	/* just check for arm on OSX for now, we know that has it */
	if (feature != CPU_ARM64_CRC32C)
		return false;
	return FIO_ARCH == arch_aarch64;
}

#endif

#define CONFIG_POSIX_FADVISE
