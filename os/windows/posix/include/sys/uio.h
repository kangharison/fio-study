/*
 * [한국어 설명] Windows용 sys/uio.h 호환 헤더
 * POSIX scatter/gather I/O용 iovec 구조체와 readv/writev 선언.
 * iov_base: 데이터 버퍼 주소, iov_len: 버퍼 크기.
 * readv는 미구현(ENOSYS), writev는 send() 루프로 에뮬레이션 (windows/posix.c).
 */
#ifndef SYS_UIO_H
#define SYS_UIO_H

#include <inttypes.h>
#include <unistd.h>

struct iovec
{
	void	*iov_base;  /* Base address of a memory region for input or output */
	size_t	 iov_len;   /* The size of the memory pointed to by iov_base */
};

 ssize_t readv(int fildes, const struct iovec *iov, int iovcnt);
 ssize_t writev(int fildes, const struct iovec *iov, int iovcnt);

#endif /* SYS_UIO_H */
