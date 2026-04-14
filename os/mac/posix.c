/*
 * [한국어 설명] macOS posix_fadvise 에뮬레이션 구현 (mac/posix.c)
 *
 * === 파일의 역할 ===
 * macOS가 지원하지 않는 posix_fadvise()를 에뮬레이션한다.
 * POSIX_FADV_RANDOM/SEQUENTIAL은 F_RDAHEAD fcntl로, POSIX_FADV_DONTNEED는
 * mmap+msync(MS_INVALIDATE)+munmap으로 페이지 캐시를 해제한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os-mac.h → mac/posix.h → mac/posix.c
 * filesetup.c, io_u.c 등에서 posix_fadvise() 호출 시 사용.
 *
 * === 주요 함수 요약 ===
 * - posix_fadvise(): 어드바이스에 따라 readahead 또는 캐시 해제 수행
 * - discard_pages(): mmap+msync로 페이지 캐시 무효화 (16GB 단위 청크)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>

#include "../../log.h"

#include "posix.h"

/* [한국어] 한 번에 mmap할 최대 크기 (16GB) - 대용량 파일을 청크 단위로 처리 */
#define MMAP_CHUNK_SIZE		(16LL * 1024 * 1024 * 1024)

/*
 * NB: performance of discard_pages() will be slower under Rosetta.
 */
/*
 * [한국어]
 * discard_pages - 페이지 캐시 무효화 (POSIX_FADV_DONTNEED 에뮬레이션)
 * @fd: 대상 파일 디스크립터
 * @offset: 시작 오프셋
 * @len: 해제할 길이
 *
 * mmap(PROT_NONE) → msync(MS_INVALIDATE) → munmap 순서로
 * 커널에 해당 페이지를 폐기하도록 요청. 16GB 청크 단위로 반복.
 * Rosetta(x86 에뮬레이션) 환경에서는 성능이 느릴 수 있음.
 */
static int discard_pages(int fd, off_t offset, off_t len)
{
	/* Align offset and len to page size */
	long pagesize = sysconf(_SC_PAGESIZE);
	long offset_pad = offset % pagesize;
	offset -= offset_pad;
	len += offset_pad;
	len = (len + pagesize - 1) & -pagesize;

	while (len > 0) {
		int saved_errno;
		size_t mmap_len = MIN(MMAP_CHUNK_SIZE, len);
		void *addr = mmap(0, mmap_len, PROT_NONE, MAP_SHARED, fd,
				  offset);

		if (addr == MAP_FAILED) {
			saved_errno = errno;
			log_err("discard_pages: failed to mmap (%s), "
				"offset = %llu, len = %zu\n",
				strerror(errno), offset, mmap_len);
			return saved_errno;
		}

		if (msync(addr, mmap_len, MS_INVALIDATE)) {
			saved_errno = errno;
			log_err("discard_pages: msync failed to free cache "
				"pages\n");

			if (munmap(addr, mmap_len) < 0)
				log_err("discard_pages: munmap failed (%s)\n",
					strerror(errno));
			return saved_errno;
		}

		if (munmap(addr, mmap_len) < 0) {
			saved_errno = errno;
			log_err("discard_pages: munmap failed (%s), "
				"len = %zu)\n", strerror(errno), mmap_len);
			return saved_errno;
		}

		len -= mmap_len;
		offset += mmap_len;
	}

	return 0;
}

/* [한국어] F_RDAHEAD fcntl로 readahead 활성/비활성화 */
static inline int set_readhead(int fd, bool enabled) {
	int ret;

	ret = fcntl(fd, F_RDAHEAD, enabled ? 1 : 0);
	if (ret == -1) {
		ret = errno;
	}

	return ret;
}

/*
 * [한국어]
 * posix_fadvise - macOS용 posix_fadvise 에뮬레이션
 * @fd: 파일 디스크립터
 * @offset: 시작 오프셋
 * @len: 길이
 * @advice: POSIX_FADV_* 상수
 * @return: 0=성공, 에러코드=실패
 *
 * FADV_RANDOM → readahead 끄기, FADV_SEQUENTIAL → readahead 켜기,
 * FADV_DONTNEED → 페이지 캐시 해제 (discard_pages).
 */
int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
	int ret;

	switch(advice) {
	case POSIX_FADV_NORMAL:
		ret = 0;
		break;
	case POSIX_FADV_RANDOM:
		ret = set_readhead(fd, false);
		break;
	case POSIX_FADV_SEQUENTIAL:
		ret = set_readhead(fd, true);
		break;
	case POSIX_FADV_DONTNEED:
		ret = discard_pages(fd, offset, len);
		break;
	default:
		ret = EINVAL;
	}

	return ret;
}
