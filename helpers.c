/*
 * [한국어] helpers.c - 시스템 콜 폴백(fallback) 헬퍼 함수
 *
 * 이 파일은 플랫폼에서 지원하지 않는 시스템 콜에 대한 폴백 구현을 제공한다.
 * 각 함수는 해당 기능이 CONFIG_* 매크로로 비활성화된 경우에만 컴파일된다.
 *
 * 제공하는 폴백:
 *   - fallocate()        : 파일 공간 사전 할당 (ENOSYS 반환)
 *   - posix_fallocate()  : POSIX 파일 공간 할당 (무시, 성공 반환)
 *   - sync_file_range()  : 파일 범위 동기화 (ENOSYS 반환)
 *   - syncfs()           : 파일시스템 전체 동기화 (ENOSYS 반환)
 *   - posix_fadvise()    : 파일 접근 힌트 (무시, 성공 반환)
 */
#include <errno.h>

#include "helpers.h"

/* [한국어] fallocate 미지원 시 폴백 - ENOSYS 에러를 반환 */
#ifndef CONFIG_LINUX_FALLOCATE
int fallocate(int fd, int mode, off_t offset, off_t len)
{
	errno = ENOSYS;
	return -1;
}
#endif

/* [한국어] posix_fallocate 미지원 시 폴백 - 무시하고 성공 반환 */
#ifndef CONFIG_POSIX_FALLOCATE
int posix_fallocate(int fd, off_t offset, off_t len)
{
	return 0;
}
#endif

/* [한국어] sync_file_range 미지원 시 폴백 - ENOSYS 에러를 반환 */
#ifndef CONFIG_SYNC_FILE_RANGE
int sync_file_range(int fd, uint64_t offset, uint64_t nbytes,
		    unsigned int flags)
{
	errno = ENOSYS;
	return -1;
}
#endif

/* [한국어] syncfs 미지원 시 폴백 - ENOSYS 에러를 반환 */
#ifndef CONFIG_SYNCFS
int syncfs(int fd)
{
	errno = ENOSYS;
	return -1;
}
#endif

/* [한국어] posix_fadvise 미지원 시 폴백 - 무시하고 성공 반환 */
#ifndef CONFIG_POSIX_FADVISE
int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
	return 0;
}
#endif
