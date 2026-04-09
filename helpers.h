#ifndef FIO_HELPERS_H
#define FIO_HELPERS_H
/*
 * [한국어] helpers.h - 시스템 콜 폴백 헬퍼 함수 헤더
 *
 * 플랫폼에서 지원하지 않는 시스템 콜(fallocate, sync_file_range 등)에 대한
 * 폴백 함수 선언을 제공한다. 실제 구현은 helpers.c에 있다.
 */

#include <sys/types.h>

#include "os/os.h"

extern int fallocate(int fd, int mode, off_t offset, off_t len);       /* 파일 공간 사전 할당 */
extern int posix_fallocate(int fd, off_t offset, off_t len);           /* POSIX 파일 공간 할당 */
#ifndef CONFIG_SYNC_FILE_RANGE
extern int sync_file_range(int fd, uint64_t offset, uint64_t nbytes,   /* 파일 범위 동기화 */
					unsigned int flags);
#endif
#ifndef CONFIG_SYNCFS
extern int syncfs(int fd);                                              /* 파일시스템 동기화 */
#endif
extern int posix_fadvise(int fd, off_t offset, off_t len, int advice); /* 파일 접근 힌트 */

#endif /* FIO_HELPERS_H_ */
