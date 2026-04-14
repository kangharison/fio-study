/*
 * [한국어 설명] Windows용 sys/mman.h 호환 헤더
 * POSIX 메모리 매핑 API(mmap/munmap/msync/mlock/munlock)와
 * 관련 상수(PROT_*, MAP_*, MS_*)를 정의.
 * 실제 구현은 windows/posix.c에서 VirtualAlloc/MapViewOfFile 기반.
 */
#ifndef SYS_MMAN_H
#define SYS_MMAN_H

#include <sys/types.h>

#define PROT_NONE	0x1
#define PROT_READ	0x2
#define PROT_WRITE	0x4

#define MAP_ANON			0x1
#define MAP_ANONYMOUS		MAP_ANON
#define MAP_FIXED			0x2
#define MAP_HASSEMAPHORE	0x4
#define MAP_INHERIT			0x8
#define MAP_NOCORE			0x10
#define MAP_NOSYNC			0x20
#define MAP_PREFAULT_READ	0x40
#define MAP_PRIVATE			0x80
#define MAP_SHARED			0x100
#define MAP_STACK			0x200

#define MAP_FAILED			NULL

#define MS_ASYNC			0x1
#define MS_SYNC				0x2
#define MS_INVALIDATE		0x3

int posix_madvise(void *addr, size_t len, int advice);
void *mmap(void *addr, size_t len, int prot, int flags,
		int fildes, off_t off);
int munmap(void *addr, size_t len);
int msync(void *addr, size_t len, int flags);
int munlock(const void * addr, size_t len);
int mlock(const void *addr, size_t len);

#endif /* SYS_MMAN_H */
