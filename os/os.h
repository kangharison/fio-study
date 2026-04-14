/*
 * [한국어 설명] OS 추상화 메인 헤더 (os.h)
 *
 * === 파일의 역할 ===
 * fio가 다양한 운영체제(Linux, macOS, FreeBSD, Windows 등)에서 동작할 수 있도록
 * 플랫폼별 차이를 추상화하는 최상위 헤더. 컴파일 타임에 OS를 감지하여
 * 적절한 os-<platform>.h 헤더를 포함하고, 해당 플랫폼에서 미제공하는
 * 기능에 대해 폴백(fallback) 구현을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 거의 모든 모듈이 이 헤더를 포함하며, 플랫폼 독립적인 코드 작성을 가능하게 함.
 * 호출 체인: fio 전체 → os/os.h → os/os-<platform>.h → 플랫폼 시스템 콜
 *
 * === 타 모듈과의 연결 ===
 * - arch/arch.h: CPU 아키텍처별 정의 (바이트 오더, 배리어 등)
 * - lib/types.h: 기본 타입 정의
 * - file.h: fio_file 구조체 (blockdev_size, fio_fallocate 등에서 사용)
 * - backend.c, io_u.c 등 핵심 모듈이 CPU 친화성, 메모리 매핑, 블록 디바이스 크기 등에 접근
 *
 * === 주요 함수/구조체 요약 ===
 * - blockdev_size(): 블록 디바이스 크기 조회 (플랫폼별 구현)
 * - fio_setaffinity()/fio_getaffinity(): CPU 친화성 설정/조회
 * - init_random_seeds(): /dev/urandom에서 난수 시드 초기화
 * - os_phys_mem(): 시스템 물리 메모리 크기 조회
 * - fio_swap16/32/64(): 바이트 오더 변환 함수
 * - os_cpu_mask_t: 플랫폼별 CPU 마스크 타입
 */
#ifndef FIO_OS_H
#define FIO_OS_H

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "../arch/arch.h" /* IWYU pragma: export */
#include "../lib/types.h"

/* [한국어] 지원 OS 식별자 열거형 - FIO_OS 매크로에 설정됨 */
enum {
	os_linux = 1,
	os_aix,
	os_freebsd,
	os_hpux,
	os_mac,
	os_netbsd,
	os_openbsd,
	os_solaris,
	os_windows,
	os_android,
	os_dragonfly,
	os_qnx,

	os_nr,	/* [한국어] OS 총 개수 (배열 크기 결정용) */
};

/* [한국어] CPU 하드웨어 기능 플래그 - os_cpu_has()에서 런타임 감지에 사용 */
typedef enum {
        CPU_ARM64_CRC32C,	/* [한국어] ARM64 CRC32C 하드웨어 가속 지원 여부 */
} cpu_features;

/*
 * [한국어] 컴파일 타임 OS 감지 및 플랫폼별 헤더 포함
 * 각 플랫폼 헤더에서 blockdev_size(), os_phys_mem(), CPU 친화성 등을 정의.
 * 플랫폼 헤더가 정의하지 않는 기능은 아래의 폴백 구현이 사용됨.
 */
/* IWYU pragma: begin_exports */
#if defined(__linux__)
#include "os-linux.h"
#elif defined(__FreeBSD__)
#include "os-freebsd.h"
#elif defined(__OpenBSD__)
#include "os-openbsd.h"
#elif defined(__QNX__)
#include "os-qnx.h"
#elif defined(__NetBSD__)
#include "os-netbsd.h"
#elif defined(__sun__)
#include "os-solaris.h"
#elif defined(__APPLE__)
#include "os-mac.h"
#elif defined(_AIX)
#include "os-aix.h"
#elif defined(__hpux)
#include "os-hpux.h"
#elif defined(WIN32)
#include "os-windows.h"
#elif defined (__DragonFly__)
#include "os-dragonfly.h"
#else
#error "unsupported os"
#endif

/* [한국어] EDQUOT(디스크 쿼터 초과) 에러가 없는 플랫폼에서 EIO로 대체 */
#ifndef EDQUOT
#define EDQUOT	EIO
#endif

#ifdef CONFIG_POSIXAIO
#include <aio.h>
#ifndef FIO_OS_HAVE_AIOCB_TYPEDEF
typedef struct aiocb os_aiocb_t;
#endif
#endif

#ifndef CONFIG_STRSEP
#include "../oslib/strsep.h"
#endif

#ifndef CONFIG_STRLCAT
#include "../oslib/strlcat.h"
#endif
/* IWYU pragma: end_exports */

#ifdef MSG_DONTWAIT
#define OS_MSG_DONTWAIT	MSG_DONTWAIT
#endif

#ifndef POSIX_FADV_DONTNEED
#define POSIX_FADV_DONTNEED	(0)
#define POSIX_FADV_SEQUENTIAL	(0)
#define POSIX_FADV_RANDOM	(0)
#define POSIX_FADV_NORMAL	(0)
#endif

/*
 * [한국어] CPU 친화성 폴백 구현
 * 플랫폼이 FIO_HAVE_CPU_AFFINITY를 정의하지 않으면 아무것도 하지 않는
 * 스텁 함수들을 제공. cpus_allowed 옵션은 이 경우 무시됨.
 */
#ifndef FIO_HAVE_CPU_AFFINITY
#define fio_cpu_clear(mask, cpu)	do { } while (0)
typedef unsigned long os_cpu_mask_t;

static inline int fio_setaffinity(int pid, os_cpu_mask_t cpumask)
{
	return 0;
}

static inline int fio_getaffinity(int pid, os_cpu_mask_t *cpumask)
{
	return -1;
}

static inline int fio_cpuset_exit(os_cpu_mask_t *mask)
{
	return -1;
}

static inline int fio_cpus_split(os_cpu_mask_t *mask, unsigned int cpu_index)
{
	return 0;
}
#else
extern int fio_cpus_split(os_cpu_mask_t *mask, unsigned int cpu);
#endif

/* [한국어] I/O 우선순위 폴백 - Linux 외 플랫폼에서 ioprio 기능 무효화 */
#ifndef FIO_HAVE_IOPRIO_CLASS
#define ioprio_class(prio)		0
#define ioprio_value_is_class_rt(prio)	(false)
#define IOPRIO_MIN_PRIO_CLASS		0
#define IOPRIO_MAX_PRIO_CLASS		0
#define ioprio_hint(prio)		0
#define IOPRIO_MIN_PRIO_HINT		0
#define IOPRIO_MAX_PRIO_HINT		0
#endif
#ifndef FIO_HAVE_IOPRIO
#define ioprio_value(prioclass, prio, priohint)	(0)
#define ioprio(ioprio)			0
#define ioprio_set(which, who, prioclass, prio, priohint) (0)
#define IOPRIO_MIN_PRIO			0
#define IOPRIO_MAX_PRIO			0
#endif

/* [한국어] O_DIRECT 지원 여부 - 미지원 플랫폼에서는 0으로 설정하여 direct=1 옵션 무효화 */
#ifndef FIO_HAVE_ODIRECT
#define OS_O_DIRECT			0
#else
#define OS_O_DIRECT			O_DIRECT
#endif

/* [한국어] 거대 페이지(Huge Page) 지원 폴백
 * FIO_HUGE_PAGE 기본값 4MB (4194304). 미지원 시 0으로 설정.
 * hugepage-size 옵션과 연동되어 대규모 I/O 버퍼 할당에 사용됨. */
#ifndef FIO_HAVE_HUGETLB
#define SHM_HUGETLB			0
#define MAP_HUGETLB			0
#ifndef FIO_HUGE_PAGE
#define FIO_HUGE_PAGE			0
#endif
#else
#ifndef FIO_HUGE_PAGE
#define FIO_HUGE_PAGE			4194304
#endif
#endif

#ifndef FIO_HAVE_MMAP_HUGE
#define MAP_HUGETLB			0
#endif

#ifndef FIO_O_NOATIME
#define FIO_O_NOATIME			0
#endif

#ifndef OS_RAND_MAX
#define OS_RAND_MAX			RAND_MAX
#endif

/* [한국어] 기본 I/O 엔진 - Linux: psync, Windows: windowsaio */
#ifndef FIO_PREFERRED_ENGINE
#define FIO_PREFERRED_ENGINE	"psync"
#endif

#ifndef FIO_OS_PATH_SEPARATOR
#define FIO_OS_PATH_SEPARATOR	'/'
#endif

#ifndef FIO_PREFERRED_CLOCK_SOURCE
#define FIO_PREFERRED_CLOCK_SOURCE	CS_CGETTIME
#endif

#ifndef CONFIG_SOCKLEN_T
typedef unsigned int socklen_t;
#endif

#ifndef FIO_OS_HAS_CTIME_R
#define os_ctime_r(x, y, z)     (void) ctime_r((x), (y))
#endif

/*
 * [한국어] 범용 바이트 스왑 함수
 * 플랫폼이 자체 바이트 스왑을 제공하지 않을 때 사용하는 제네릭 구현.
 * 네트워크 프로토콜, 디스크 데이터 등 엔디안 변환에 사용됨.
 */
#ifdef FIO_USE_GENERIC_SWAP
static inline uint16_t fio_swap16(uint16_t val)
{
	return (val << 8) | (val >> 8);
}

static inline uint32_t fio_swap32(uint32_t val)
{
	val = ((val & 0xff00ff00UL) >> 8) | ((val & 0x00ff00ffUL) << 8);

	return (val >> 16) | (val << 16);
}

static inline uint64_t fio_swap64(uint64_t val)
{
	val = ((val & 0xff00ff00ff00ff00ULL) >> 8) |
	      ((val & 0x00ff00ff00ff00ffULL) << 8);
	val = ((val & 0xffff0000ffff0000ULL) >> 16) |
	      ((val & 0x0000ffff0000ffffULL) << 16);

	return (val >> 32) | (val << 32);
}
#endif

/*
 * [한국어] 바이트 오더 변환 매크로
 * 리틀 엔디안: be→cpu는 스왑 필요, le→cpu는 그대로
 * 빅 엔디안: be→cpu는 그대로, le→cpu는 스왑 필요
 * stat.c, client/server 등에서 네트워크/디스크 데이터 변환에 사용.
 */
#ifndef FIO_HAVE_BYTEORDER_FUNCS
#ifdef CONFIG_LITTLE_ENDIAN
#define __be16_to_cpu(x)		fio_swap16(x)
#define __be32_to_cpu(x)		fio_swap32(x)
#define __be64_to_cpu(x)		fio_swap64(x)
#define __le16_to_cpu(x)		(x)
#define __le32_to_cpu(x)		(x)
#define __le64_to_cpu(x)		(x)
#define __cpu_to_be16(x)		fio_swap16(x)
#define __cpu_to_be32(x)		fio_swap32(x)
#define __cpu_to_be64(x)		fio_swap64(x)
#define __cpu_to_le16(x)		(x)
#define __cpu_to_le32(x)		(x)
#define __cpu_to_le64(x)		(x)
#else
#define __be16_to_cpu(x)		(x)
#define __be32_to_cpu(x)		(x)
#define __be64_to_cpu(x)		(x)
#define __le16_to_cpu(x)		fio_swap16(x)
#define __le32_to_cpu(x)		fio_swap32(x)
#define __le64_to_cpu(x)		fio_swap64(x)
#define __cpu_to_be16(x)		(x)
#define __cpu_to_be32(x)		(x)
#define __cpu_to_be64(x)		(x)
#define __cpu_to_le16(x)		fio_swap16(x)
#define __cpu_to_le32(x)		fio_swap32(x)
#define __cpu_to_le64(x)		fio_swap64(x)
#endif
#endif /* FIO_HAVE_BYTEORDER_FUNCS */

#ifdef FIO_INTERNAL
#define be16_to_cpu(val) ({			\
	typecheck(uint16_t, val);		\
	__be16_to_cpu(val);			\
})
#define be32_to_cpu(val) ({			\
	typecheck(uint32_t, val);		\
	__be32_to_cpu(val);			\
})
#define be64_to_cpu(val) ({			\
	typecheck(uint64_t, val);		\
	__be64_to_cpu(val);			\
})
#define le16_to_cpu(val) ({			\
	typecheck(uint16_t, val);		\
	__le16_to_cpu(val);			\
})
#define le32_to_cpu(val) ({			\
	typecheck(uint32_t, val);		\
	__le32_to_cpu(val);			\
})
#define le64_to_cpu(val) ({			\
	typecheck(uint64_t, val);		\
	__le64_to_cpu(val);			\
})
#endif

#define cpu_to_be16(val) ({			\
	typecheck(uint16_t, val);		\
	__cpu_to_be16(val);			\
})
#define cpu_to_be32(val) ({			\
	typecheck(uint32_t, val);		\
	__cpu_to_be32(val);			\
})
#define cpu_to_be64(val) ({			\
	typecheck(uint64_t, val);		\
	__cpu_to_be64(val);			\
})
#define cpu_to_le16(val) ({			\
	typecheck(uint16_t, val);		\
	__cpu_to_le16(val);			\
})
#define cpu_to_le32(val) ({			\
	typecheck(uint32_t, val);		\
	__cpu_to_le32(val);			\
})
#define cpu_to_le64(val) ({			\
	typecheck(uint64_t, val);		\
	__cpu_to_le64(val);			\
})

/* [한국어] 캐시 라인 크기 기본값 128바이트 - 실제 값은 arch_cache_line_size()로 조회 */
#define FIO_DEF_CL_SIZE		128

/*
 * [한국어]
 * os_cache_line_size - CPU 캐시 라인 크기 조회
 *
 * @return: 캐시 라인 크기 (바이트), 감지 실패 시 기본값 128
 *
 * false sharing 방지를 위한 구조체 정렬에 사용됨.
 */
static inline int os_cache_line_size(void)
{
#ifdef FIO_HAVE_CL_SIZE
	int ret = arch_cache_line_size();

	if (ret <= 0)
		return FIO_DEF_CL_SIZE;

	return ret;
#else
	return FIO_DEF_CL_SIZE;
#endif
}

/*
 * [한국어]
 * blockdev_size - 블록 디바이스 크기 조회 (제네릭 구현)
 *
 * @f: 대상 fio_file (fd가 열려 있어야 함)
 * @bytes: 크기를 저장할 포인터
 * @return: 0=성공, errno=실패
 *
 * lseek(SEEK_END)로 파일 끝 위치를 구하는 범용 방식.
 * Linux는 BLKGETSIZE64 ioctl, macOS는 DKIOC* 등 플랫폼별 구현이 우선함.
 */
#ifdef FIO_USE_GENERIC_BDEV_SIZE
static inline int blockdev_size(struct fio_file *f, unsigned long long *bytes)
{
	off_t end;

	*bytes = 0;

	end = lseek(f->fd, 0, SEEK_END);
	if (end < 0)
		return errno;

	*bytes = end;
	return 0;
}
#endif

/*
 * [한국어]
 * init_random_seeds - 난수 시드 초기화 (/dev/urandom 사용)
 *
 * @rand_seeds: 시드 값을 저장할 버퍼
 * @size: 버퍼 크기 (바이트)
 * @return: 0=성공, 1=실패
 *
 * fio 시작 시 각 스레드의 난수 생성기를 초기화하는 데 사용.
 * Windows는 CryptGenRandom()으로 별도 구현.
 */
#ifdef FIO_USE_GENERIC_INIT_RANDOM_STATE
static inline int init_random_seeds(uint64_t *rand_seeds, int size)
{
	int fd;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) {
		return 1;
	}

	if (read(fd, rand_seeds, size) < size) {
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
#endif

/* [한국어] 파일시스템 여유 공간 조회 폴백 - 미지원 시 0 반환 */
#ifndef FIO_HAVE_FS_STAT
static inline unsigned long long get_fs_free_size(const char *path)
{
	return 0;
}
#endif

/* [한국어] 시스템에 구성된 CPU 수 조회 - sysconf(_SC_NPROCESSORS_CONF) 사용 */
#ifndef FIO_HAVE_CPU_CONF_SYSCONF
static inline unsigned int cpus_configured(void)
{
	int nr_cpus = sysconf(_SC_NPROCESSORS_CONF);

	return nr_cpus >= 1 ? nr_cpus : 1;
}
#endif

#ifndef CPU_COUNT
#ifdef FIO_HAVE_CPU_AFFINITY
static inline int CPU_COUNT(os_cpu_mask_t *mask)
{
	int max_cpus = cpus_configured();
	int nr_cpus, i;

	for (i = 0, nr_cpus = 0; i < max_cpus; i++)
		if (fio_cpu_isset(mask, i))
			nr_cpus++;

	return nr_cpus;
}
#endif
#endif

#ifndef FIO_HAVE_GETTID
#ifndef CONFIG_HAVE_GETTID
static inline int gettid(void)
{
	return getpid();
}
#endif
#endif

#ifndef FIO_HAVE_SHM_ATTACH_REMOVED
static inline int shm_attach_to_open_removed(void)
{
	return 0;
}
#endif

/* [한국어] fallocate 폴백 - 미지원 플랫폼에서 ENOSYS 반환 */
#ifndef FIO_HAVE_NATIVE_FALLOCATE
static inline bool fio_fallocate(struct fio_file *f, uint64_t offset, uint64_t len)
{
	errno = ENOSYS;
	return false;
}
#endif

#if defined(CONFIG_POSIX_FALLOCATE) || defined(FIO_HAVE_NATIVE_FALLOCATE)
# define FIO_HAVE_DEFAULT_FALLOCATE
#endif

#ifndef FIO_HAVE_CPU_HAS
static inline bool os_cpu_has(cpu_features feature)
{
	return false;
}
#endif

#ifndef FIO_EMULATED_MKDIR_TWO
# define fio_mkdir(path, mode)	mkdir(path, mode)
#endif

#ifdef _SC_CLK_TCK
static inline void os_clk_tck(long *clk_tck)
{
	*clk_tck = sysconf(_SC_CLK_TCK);
}
#else
extern void os_clk_tck(long *clk_tck);
#endif

#endif /* FIO_OS_H */
