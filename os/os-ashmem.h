/*
 * [한국어 설명] Android ashmem 기반 SysV 공유 메모리 에뮬레이션 (os-ashmem.h)
 *
 * === 파일의 역할 ===
 * Android의 Bionic C 라이브러리가 SysV 공유 메모리(shmget/shmat/shmdt/shmctl)를
 * 지원하지 않으므로, ashmem(Android Shared Memory) 디바이스를 사용하여 에뮬레이션.
 * fio의 smalloc.c에서 공유 메모리 할당 시 사용됨.
 *
 * === 전체 아키텍처에서의 위치 ===
 * os/os-linux.h에서 __ANDROID__ 감지 시 포함됨.
 *
 * === 주요 함수 요약 ===
 * - shmget(): ashmem 디바이스 열기 + 이름/크기 설정
 * - shmat(): mmap으로 ashmem 영역 매핑 (크기를 앞 8바이트에 저장)
 * - shmdt(): munmap으로 해제 (저장된 크기 사용)
 * - shmctl(IPC_RMID): ashmem 언핀 + 닫기
 */
#ifndef CONFIG_NO_SHM
/*
 * Bionic doesn't support SysV shared memory, so implement it using ashmem
 */
#include <stdio.h>
#include <linux/ashmem.h>
#include <linux/shm.h>
#include <android/api-level.h>
#ifdef CONFIG_ASHAREDMEMORY_CREATE
#include <android/sharedmem.h>
#else
#define ASHMEM_DEVICE	"/dev/ashmem"
#endif
#define shmid_ds shmid64_ds
#define SHM_HUGETLB    04000

/* [한국어] SysV shmctl 에뮬레이션 - IPC_RMID 시 ashmem 언핀 후 fd 닫기 */
static inline int shmctl(int __shmid, int __cmd, struct shmid_ds *__buf)
{
	int ret=0;
	if (__cmd == IPC_RMID)
	{
		int length = ioctl(__shmid, ASHMEM_GET_SIZE, NULL);
		struct ashmem_pin pin = {0 , length};
		ret = ioctl(__shmid, ASHMEM_UNPIN, &pin);
		close(__shmid);
	}
	return ret;
}

#ifdef CONFIG_ASHAREDMEMORY_CREATE
static inline int shmget(key_t __key, size_t __size, int __shmflg)
{
	char keybuf[11];

	sprintf(keybuf, "%d", __key);

	return ASharedMemory_create(keybuf, __size + sizeof(uint64_t));
}
#else
static inline int shmget(key_t __key, size_t __size, int __shmflg)
{
	int fd,ret;
	char keybuf[11];

	fd = open(ASHMEM_DEVICE, O_RDWR);
	if (fd < 0)
		return fd;

	sprintf(keybuf,"%d",__key);
	ret = ioctl(fd, ASHMEM_SET_NAME, keybuf);
	if (ret < 0)
		goto error;

	/* Stores size in first 8 bytes, allocate extra space */
	ret = ioctl(fd, ASHMEM_SET_SIZE, __size + sizeof(uint64_t));
	if (ret < 0)
		goto error;

	return fd;

error:
	close(fd);
	return ret;
}
#endif

/* [한국어] shmat 에뮬레이션 - mmap 후 앞 8바이트에 크기 저장 (shmdt에서 사용) */
static inline void *shmat(int __shmid, const void *__shmaddr, int __shmflg)
{
	size_t size = ioctl(__shmid, ASHMEM_GET_SIZE, NULL);
	/* Needs to be 8-byte aligned to prevent SIGBUS on 32-bit ARM */
	uint64_t *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, __shmid, 0);
	/* Save size at beginning of buffer, for use with munmap */
	*ptr = size;
	return ptr + 1;
}

static inline int shmdt (const void *__shmaddr)
{
	/* Find mmap size which we stored at the beginning of the buffer */
	uint64_t *ptr = (uint64_t *)__shmaddr - 1;
	size_t size = *ptr;
	return munmap(ptr, size);
}
#endif
