/*
 * [한국어 설명] Linux 플랫폼 OS 추상화 헤더 (os-linux.h)
 *
 * === 파일의 역할 ===
 * Linux(및 Android)에서 fio가 사용하는 플랫폼 전용 기능을 정의한다.
 * CPU 친화성(sched_setaffinity), I/O 우선순위(ioprio), 블록 디바이스 ioctl,
 * preadv2/pwritev2, 바이트 스왑, 캐시 라인 크기 조회 등을 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os.h에서 __linux__ 감지 시 포함됨.
 * 호출 체인: fio 전체 → os/os.h → os/os-linux.h → Linux syscall/ioctl
 *
 * === 타 모듈과의 연결 ===
 * - os-linux-syscall.h: 아키텍처별 시스템 콜 번호 정의
 * - file.h: fio_file 구조체 (blockdev_size 등에서 사용)
 * - backend.c: CPU 친화성, I/O 우선순위 설정
 * - engines/sync.c: preadv2/pwritev2 사용 (RWF_* 플래그)
 * - engines/io_uring.c: RWF_HIPRI, RWF_NOWAIT 등 플래그 사용
 *
 * === 주요 함수/구조체 요약 ===
 * - blockdev_size(): BLKGETSIZE64 ioctl로 블록 디바이스 크기 조회
 * - ioprio_set(): I/O 스케줄러 우선순위 설정 (syscall 직접 호출)
 * - os_phys_mem(): sysconf로 시스템 물리 메모리 크기 조회
 * - preadv2()/pwritev2(): 플래그 지원 scatter/gather I/O
 * - fio_fallocate(): Linux fallocate(2)로 파일 사전 할당
 */
#ifndef FIO_OS_LINUX_H
#define FIO_OS_LINUX_H

#ifdef __ANDROID__
#define FIO_OS  os_android
#else
#define	FIO_OS	os_linux
#endif

#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <linux/unistd.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <scsi/sg.h>
#include <asm/byteorder.h>
#ifdef __ANDROID__
#include "os-ashmem.h"
#define FIO_NO_HAVE_SHM_H
#endif

#ifdef ARCH_HAVE_CRC_CRYPTO
#include <sys/auxv.h>
#ifndef HWCAP_PMULL
#define HWCAP_PMULL             (1 << 4)
#endif /* HWCAP_PMULL */
#ifndef HWCAP_CRC32
#define HWCAP_CRC32             (1 << 7)
#endif /* HWCAP_CRC32 */
#endif /* ARCH_HAVE_CRC_CRYPTO */

#include "./os-linux-syscall.h"
#include "../file.h"

#ifndef __has_builtin         // Optional of course.
  #define __has_builtin(x) 0  // Compatibility with non-clang compilers.
#endif

/* [한국어] Linux에서 지원하는 기능 플래그 선언 */
#define FIO_HAVE_CPU_AFFINITY		/* [한국어] CPU 친화성 설정 지원 (sched_setaffinity) */
#define FIO_HAVE_DISK_UTIL		/* [한국어] 디스크 사용률 통계 수집 지원 */
#define FIO_HAVE_SGIO			/* [한국어] SCSI Generic I/O 지원 (sg 엔진) */
#define FIO_HAVE_IOPRIO			/* [한국어] I/O 우선순위 (ioprio_set syscall) */
#define FIO_HAVE_IOPRIO_CLASS		/* [한국어] I/O 우선순위 클래스 (RT, BE, IDLE) */
#define FIO_HAVE_IOSCHED_SWITCH		/* [한국어] I/O 스케줄러 전환 지원 */
#define FIO_HAVE_ODIRECT		/* [한국어] O_DIRECT 직접 I/O 지원 */
#define FIO_HAVE_HUGETLB		/* [한국어] 거대 페이지 (SHM_HUGETLB, MAP_HUGETLB) */
#define FIO_HAVE_BLKTRACE		/* [한국어] blktrace 기반 I/O 리플레이 지원 */
#define FIO_HAVE_CL_SIZE		/* [한국어] CPU 캐시 라인 크기 감지 (sysfs) */
#define FIO_HAVE_CGROUPS		/* [한국어] cgroup 기반 리소스 제어 */
#define FIO_HAVE_FS_STAT		/* [한국어] 파일시스템 여유 공간 조회 (statfs) */
#define FIO_HAVE_TRIM			/* [한국어] TRIM/DISCARD 지원 (BLKDISCARD ioctl) */
#define FIO_HAVE_GETTID			/* [한국어] 스레드 ID 조회 (gettid syscall) */
#define FIO_USE_GENERIC_INIT_RANDOM_STATE  /* [한국어] /dev/urandom 기반 난수 초기화 */
#define FIO_HAVE_BYTEORDER_FUNCS	/* [한국어] 커널 헤더의 바이트 오더 함수 사용 */
#define FIO_HAVE_PWRITEV2		/* [한국어] preadv2/pwritev2 지원 (RWF_* 플래그) */
#define FIO_HAVE_SHM_ATTACH_REMOVED	/* [한국어] 삭제된 SHM 세그먼트에 attach 가능 */
#define FIO_HAVE_RWF_ATOMIC		/* [한국어] RWF_ATOMIC 쓰기 플래그 지원 */

#ifdef MAP_HUGETLB
#define FIO_HAVE_MMAP_HUGE
#endif

#define OS_MAP_ANON		MAP_ANONYMOUS

#define FIO_EXT_ENG_DIR	"/usr/local/lib/fio"

typedef cpu_set_t os_cpu_mask_t;	/* [한국어] Linux CPU 마스크 = POSIX cpu_set_t */

/* [한국어] CPU 친화성 API - 리눅스 버전에 따라 2인자/3인자 변형 */
#ifdef CONFIG_3ARG_AFFINITY
#define fio_setaffinity(pid, cpumask)		\
	sched_setaffinity((pid), sizeof(cpumask), &(cpumask))
#define fio_getaffinity(pid, ptr)	\
	sched_getaffinity((pid), sizeof(cpu_set_t), (ptr))
#elif defined(CONFIG_2ARG_AFFINITY)
#define fio_setaffinity(pid, cpumask)	\
	sched_setaffinity((pid), &(cpumask))
#define fio_getaffinity(pid, ptr)	\
	sched_getaffinity((pid), (ptr))
#endif

#ifdef CONFIG_PTHREAD_GETAFFINITY
#define FIO_HAVE_GET_THREAD_AFFINITY
#define fio_get_thread_affinity(mask)	\
	pthread_getaffinity_np(pthread_self(), sizeof(mask), &(mask))
#endif

#define fio_cpu_clear(mask, cpu)	CPU_CLR((cpu), (mask))
#define fio_cpu_set(mask, cpu)		CPU_SET((cpu), (mask))
#define fio_cpu_isset(mask, cpu)	(CPU_ISSET((cpu), (mask)) != 0)
#define fio_cpu_count(mask)		CPU_COUNT((mask))

static inline int fio_cpuset_init(os_cpu_mask_t *mask)
{
	CPU_ZERO(mask);
	return 0;
}

static inline int fio_cpuset_exit(os_cpu_mask_t *mask)
{
	return 0;
}

#define FIO_MAX_CPUS			CPU_SETSIZE

/*
 * [한국어] I/O 우선순위 클래스
 * Linux의 CFQ/BFQ 스케줄러에서 I/O 요청의 서비스 순서를 결정.
 * fio의 ioscheduler 및 prio/prioclass 옵션에서 사용.
 */
enum {
	IOPRIO_CLASS_NONE,	/* [한국어] 클래스 미지정 → BE로 취급 */
	IOPRIO_CLASS_RT,	/* [한국어] 실시간 - 최우선 처리 */
	IOPRIO_CLASS_BE,	/* [한국어] Best Effort - 기본 클래스 */
	IOPRIO_CLASS_IDLE,	/* [한국어] 유휴 시에만 처리 */
};

/* [한국어] ioprio_set 대상 지정 */
enum {
	IOPRIO_WHO_PROCESS = 1,	/* [한국어] 특정 프로세스/스레드 */
	IOPRIO_WHO_PGRP,	/* [한국어] 프로세스 그룹 */
	IOPRIO_WHO_USER,	/* [한국어] 사용자 */
};

/* [한국어] ioprio 값 비트 레이아웃: [class:3비트][hint:10비트][prio:3비트] */
#define IOPRIO_BITS		16
#define IOPRIO_CLASS_SHIFT	13

#define IOPRIO_HINT_BITS	10
#define IOPRIO_HINT_SHIFT	3

#define IOPRIO_MIN_PRIO		0	/* highest priority */
#define IOPRIO_MAX_PRIO		7	/* lowest priority */

#define IOPRIO_MIN_PRIO_CLASS	0
#define IOPRIO_MAX_PRIO_CLASS	3

#define IOPRIO_MIN_PRIO_HINT	0
#define IOPRIO_MAX_PRIO_HINT	((1 << IOPRIO_HINT_BITS) - 1)

#define ioprio_class(ioprio)	((ioprio) >> IOPRIO_CLASS_SHIFT)
#define ioprio(ioprio)		((ioprio) & IOPRIO_MAX_PRIO)
#define ioprio_hint(ioprio)	\
	(((ioprio) >> IOPRIO_HINT_SHIFT) & IOPRIO_MAX_PRIO_HINT)

/*
 * [한국어]
 * ioprio_value - I/O 우선순위 값 조합
 * 클래스, 우선순위 레벨, 힌트를 하나의 16비트 값으로 합성.
 * 호출 체인: options.c(파싱) → ioprio_set() → syscall
 */
static inline int ioprio_value(int ioprio_class, int ioprio, int ioprio_hint)
{
	/*
	 * If no class is set, assume BE
	 */
        if (!ioprio_class)
                ioprio_class = IOPRIO_CLASS_BE;

	return (ioprio_class << IOPRIO_CLASS_SHIFT) |
		(ioprio_hint << IOPRIO_HINT_SHIFT) |
		ioprio;
}

static inline bool ioprio_value_is_class_rt(unsigned int priority)
{
	return ioprio_class(priority) == IOPRIO_CLASS_RT;
}

/*
 * [한국어]
 * ioprio_set - I/O 우선순위 설정 (리눅스 시스템 콜 직접 호출)
 * glibc가 래퍼를 제공하지 않으므로 syscall()로 직접 호출.
 */
static inline int ioprio_set(int which, int who, int ioprio_class, int ioprio,
			     int ioprio_hint)
{
	return syscall(__NR_ioprio_set, which, who,
		       ioprio_value(ioprio_class, ioprio, ioprio_hint));
}

#ifndef CONFIG_HAVE_GETTID
static inline int gettid(void)
{
	return syscall(__NR_gettid);
}
#endif

#define SPLICE_DEF_SIZE	(64*1024)

#ifndef BLKGETSIZE64
#define BLKGETSIZE64	_IOR(0x12,114,size_t)
#endif

#ifndef BLKFLSBUF
#define BLKFLSBUF	_IO(0x12,97)
#endif

#ifndef BLKDISCARD
#define BLKDISCARD	_IO(0x12,119)
#endif

/*
 * [한국어]
 * blockdev_invalidate_cache - 블록 디바이스 버퍼 캐시 무효화
 * BLKFLSBUF ioctl로 커널 버퍼 캐시를 플러시.
 * invalidate=1 옵션에서 사용하여 벤치마크 정확도를 높임.
 */
static inline int blockdev_invalidate_cache(struct fio_file *f)
{
	return ioctl(f->fd, BLKFLSBUF);
}

/*
 * [한국어]
 * blockdev_size - Linux 블록 디바이스 크기 조회 (BLKGETSIZE64 ioctl)
 * @f: 열린 블록 디바이스 파일
 * @bytes: 결과 크기(바이트) 저장 포인터
 * @return: 0=성공, errno=실패
 */
static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	if (!ioctl(f->fd, BLKGETSIZE64, bytes))
		return 0;

	return errno;
}

/*
 * [한국어]
 * os_phys_mem - 시스템 물리 메모리 크기 조회
 * 페이지 크기 x 총 페이지 수로 계산. 메모리 기반 파일 크기 결정에 사용.
 */
static inline unsigned long long os_phys_mem(void)
{
	long pagesize, pages;

	pagesize = sysconf(_SC_PAGESIZE);
	pages = sysconf(_SC_PHYS_PAGES);
	if (pages == -1 || pagesize == -1)
		return 0;

	return (unsigned long long) pages * (unsigned long long) pagesize;
}

#ifdef O_NOATIME
#define FIO_O_NOATIME	O_NOATIME
#else
#define FIO_O_NOATIME	0
#endif

#ifdef MADV_REMOVE
#define FIO_MADV_FREE	MADV_REMOVE
#endif

/* Check for GCC or Clang byte swap intrinsics */
#if (__has_builtin(__builtin_bswap16) && __has_builtin(__builtin_bswap32) \
     && __has_builtin(__builtin_bswap64)) || (__GNUC__ > 4 \
     || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)) /* fio_swapN */
#define fio_swap16(x)	__builtin_bswap16(x)
#define fio_swap32(x)	__builtin_bswap32(x)
#define fio_swap64(x)	__builtin_bswap64(x)
#else
#include <byteswap.h>
#define fio_swap16(x)	bswap_16(x)
#define fio_swap32(x)	bswap_32(x)
#define fio_swap64(x)	bswap_64(x)
#endif /* fio_swapN */

#define CACHE_LINE_FILE	\
	"/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size"

static inline int arch_cache_line_size(void)
{
	char size[32];
	int fd, ret;

	fd = open(CACHE_LINE_FILE, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, size, sizeof(size));

	close(fd);

	if (ret <= 0)
		return -1;
	else
		return atoi(size);
}

/* [한국어] 파일시스템 여유 공간 조회 (statfs 사용) - fill_device 옵션 등에서 참조 */
static inline unsigned long long get_fs_free_size(const char *path)
{
	unsigned long long ret;
	struct statfs s;

	if (statfs(path, &s) < 0)
		return -1ULL;

	ret = s.f_bsize;
	ret *= (unsigned long long) s.f_bfree;
	return ret;
}

/*
 * [한국어]
 * os_trim - 블록 디바이스 TRIM/DISCARD 수행 (BLKDISCARD ioctl)
 * SSD에 미사용 블록을 알려 가비지 컬렉션을 돕는다.
 * fio의 trim_percentage, trim_verify 옵션에서 사용.
 */
static inline int os_trim(struct fio_file *f, unsigned long long start,
			  unsigned long long len)
{
	uint64_t range[2];

	range[0] = start;
	range[1] = len;

	if (!ioctl(f->fd, BLKDISCARD, range))
		return 0;

	return errno;
}

#ifdef CONFIG_SCHED_IDLE
static inline int fio_set_sched_idle(void)
{
	struct sched_param p = { .sched_priority = 0, };
	return sched_setscheduler(gettid(), SCHED_IDLE, &p);
}
#endif

/* [한국어] Write Life Time Hint - 커널에 데이터 수명 힌트를 전달하여 SSD 배치 최적화 */
#ifndef F_GET_RW_HINT
#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE	1024
#endif
#define F_GET_RW_HINT		(F_LINUX_SPECIFIC_BASE + 11)
#define F_SET_RW_HINT		(F_LINUX_SPECIFIC_BASE + 12)
#define F_GET_FILE_RW_HINT	(F_LINUX_SPECIFIC_BASE + 13)
#define F_SET_FILE_RW_HINT	(F_LINUX_SPECIFIC_BASE + 14)
#endif

#ifndef RWH_WRITE_LIFE_NONE
#define RWH_WRITE_LIFE_NOT_SET	0
#define RWH_WRITE_LIFE_NONE	1
#define RWH_WRITE_LIFE_SHORT	2
#define RWH_WRITE_LIFE_MEDIUM	3
#define RWH_WRITE_LIFE_LONG	4
#define RWH_WRITE_LIFE_EXTREME	5
#endif

#define FIO_HAVE_WRITE_HINT

/* [한국어] preadv2/pwritev2 RWF 플래그 - engines/sync.c에서 사용 */
#ifndef RWF_HIPRI
#define RWF_HIPRI	0x00000001	/* [한국어] 고우선순위 폴링 I/O */
#endif
#ifndef RWF_DSYNC
#define RWF_DSYNC	0x00000002	/* [한국어] 데이터 동기화 (fdatasync 효과) */
#endif
#ifndef RWF_SYNC
#define RWF_SYNC	0x00000004	/* [한국어] 파일 동기화 (fsync 효과) */
#endif
#ifndef RWF_NOWAIT
#define RWF_NOWAIT	0x00000008	/* [한국어] 블로킹 없이 즉시 반환 (EAGAIN) */
#endif

#ifndef RWF_ATOMIC
#define RWF_ATOMIC	0x00000040
#endif

#ifndef RWF_DONTCACHE
#define RWF_DONTCACHE	0x00000080
#endif

#ifndef RWF_WRITE_LIFE_SHIFT
#define RWF_WRITE_LIFE_SHIFT		4
#define RWF_WRITE_LIFE_SHORT		(1 << RWF_WRITE_LIFE_SHIFT)
#define RWF_WRITE_LIFE_MEDIUM		(2 << RWF_WRITE_LIFE_SHIFT)
#define RWF_WRITE_LIFE_LONG		(3 << RWF_WRITE_LIFE_SHIFT)
#define RWF_WRITE_LIFE_EXTREME		(4 << RWF_WRITE_LIFE_SHIFT)
#endif

/*
 * [한국어] preadv2/pwritev2 시스템 콜 래퍼
 * glibc가 제공하지 않을 경우 syscall()로 직접 호출.
 * 32비트 시스템에서는 오프셋을 상위/하위로 분리하여 전달.
 * engines/sync.c의 fio_preadv2()/fio_pwritev2()에서 사용.
 */
#ifndef CONFIG_PWRITEV2
#ifdef __NR_preadv2
static inline void make_pos_h_l(unsigned long *pos_h, unsigned long *pos_l,
				off_t offset)
{
#if BITS_PER_LONG == 64
	*pos_l = offset;
	*pos_h = 0;
#else
	*pos_l = offset & 0xffffffff;
	*pos_h = ((uint64_t) offset) >> 32;
#endif
}
static inline ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt,
			      off_t offset, unsigned int flags)
{
	unsigned long pos_l, pos_h;

	make_pos_h_l(&pos_h, &pos_l, offset);
	return syscall(__NR_preadv2, fd, iov, iovcnt, pos_l, pos_h, flags);
}
static inline ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt,
			       off_t offset, unsigned int flags)
{
	unsigned long pos_l, pos_h;

	make_pos_h_l(&pos_h, &pos_l, offset);
	return syscall(__NR_pwritev2, fd, iov, iovcnt, pos_l, pos_h, flags);
}
#else
static inline ssize_t preadv2(int fd, const struct iovec *iov, int iovcnt,
			      off_t offset, unsigned int flags)
{
	errno = ENOSYS;
	return -1;
}
static inline ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt,
			       off_t offset, unsigned int flags)
{
	errno = ENOSYS;
	return -1;
}
#endif /* __NR_preadv2 */
#endif /* CONFIG_PWRITEV2 */

static inline int shm_attach_to_open_removed(void)
{
	return 1;
}

#ifdef CONFIG_LINUX_FALLOCATE
#define FIO_HAVE_NATIVE_FALLOCATE
static inline bool fio_fallocate(struct fio_file *f, uint64_t offset,
				 uint64_t len)
{
	int ret;
	ret = fallocate(f->fd, 0, offset, len);
	if (ret == 0)
		return true;

	/* Work around buggy old glibc versions... */
	if (ret > 0)
		errno = ret;

	return false;
}
#endif

/*
 * [한국어]
 * os_cpu_has - CPU 하드웨어 기능 런타임 감지 (ARM64 CRC32C)
 * getauxval(AT_HWCAP)로 하드웨어 기능 비트를 확인.
 * crc/ 모듈에서 하드웨어 가속 CRC 사용 여부를 결정하는 데 사용.
 */
#define FIO_HAVE_CPU_HAS
static inline bool os_cpu_has(cpu_features feature)
{
	bool have_feature;
	unsigned long fio_unused hwcap;

	switch (feature) {
#ifdef ARCH_HAVE_CRC_CRYPTO
	case CPU_ARM64_CRC32C:
		hwcap = getauxval(AT_HWCAP);
		have_feature = (hwcap & (HWCAP_PMULL | HWCAP_CRC32)) ==
			       (HWCAP_PMULL | HWCAP_CRC32);
		break;
#endif
	default:
		have_feature = false;
	}

	return have_feature;
}

#endif
