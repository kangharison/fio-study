/*
 * [한국어] diskutil.h - 디스크 유틸리티 통계 헤더
 *
 * /proc/diskstats 및 /sys/block/에서 디스크 I/O 활용도를 수집하기 위한
 * 구조체 및 함수를 정의한다. iostat과 유사한 디스크 통계를 fio 내부에서 제공한다.
 
 * === 파일의 역할 ===
 * /proc/diskstats, /sys/block/ 기반 디스크 I/O 활용도 수집 구조체/함수 정의.
 *
 * === 전체 아키텍처에서의 위치 ===
 * diskutil.c와 짝을 이루는 헤더. helper_thread.c에서 주기적 업데이트 호출.
 *
 * === 타 모듈과의 연결 ===
 * - diskutil.c: 이 헤더의 함수 구현
 * - helper_thread.c: update_io_ticks() 주기적 호출
 *
 * === 주요 함수/구조체 요약 ===
 * - struct disk_util: 디스크 유틸리티 통계
 * - init_disk_util()/update_io_ticks(): 초기화/업데이트
 */
#ifndef FIO_DISKUTIL_H
#define FIO_DISKUTIL_H
#define FIO_DU_NAME_SZ		64

#include <stdint.h>
#include <limits.h>

#include "helper_thread.h"  /* 헬퍼 스레드 인터페이스 */
#include "fio_sem.h"        /* fio 세마포어 */
#include "flist.h"          /* fio 연결 리스트 */
#include "lib/ieee754.h"    /* IEEE 754 부동소수점 변환 */

/**
 * @ios: Number of I/O operations that have been completed successfully.
 * @merges: Number of I/O operations that have been merged.
 * @sectors: I/O size in 512-byte units.
 * @ticks: Time spent on I/O in milliseconds.
 * @io_ticks: CPU time spent on I/O in milliseconds.
 * @time_in_queue: Weighted time spent doing I/O in milliseconds.
 *
 * For the array members, index 0 refers to reads and index 1 refers to writes.
 */
/* [한국어] 디스크 I/O 통계 원시 데이터 - /sys/block/<dev>/stat에서 읽은 값
 * 배열 인덱스: [0]=읽기, [1]=쓰기 */
struct disk_util_stats {
	uint64_t ios[2];           /* 완료된 I/O 연산 수 */
	uint64_t merges[2];        /* 병합된 I/O 연산 수 */
	uint64_t sectors[2];       /* 전송된 섹터 수 (512바이트 단위) */
	uint64_t ticks[2];         /* I/O 처리에 소요된 시간 (밀리초) */
	uint64_t io_ticks;         /* I/O가 활성 상태였던 시간 (밀리초) */
	uint64_t time_in_queue;    /* I/O 큐에서 대기한 가중 시간 (밀리초) */
	uint64_t msec;             /* 측정 경과 시간 (밀리초) */
};

/*
 * Disk utilization as read from /sys/block/<dev>/stat
 */
/* [한국어] 디스크 유틸리티 통계 - 디바이스 이름과 통계 데이터를 포함 */
struct disk_util_stat {
	uint8_t name[FIO_DU_NAME_SZ];  /* 디바이스 이름 (예: "sda") */
	struct disk_util_stats s;       /* I/O 통계 데이터 */
};

/* [한국어] 디스크 유틸리티 집계 구조체 - 여러 슬레이브 디바이스의 통계를 집계 */
struct disk_util_agg {
	uint64_t ios[2];           /* 집계된 I/O 연산 수 */
	uint64_t merges[2];        /* 집계된 병합 I/O 수 */
	uint64_t sectors[2];       /* 집계된 섹터 전송량 */
	uint64_t ticks[2];         /* 집계된 I/O 시간 */
	uint64_t io_ticks;         /* 집계된 활성 I/O 시간 */
	uint64_t time_in_queue;    /* 집계된 큐 대기 시간 */
	uint32_t slavecount;       /* 슬레이브 디바이스 수 */
	uint32_t pad;              /* 패딩 (정렬용) */
	fio_fp64_t max_util;       /* 최대 활용률 (%) */
};

/*
 * Per-device disk util management
 */
/* [한국어] 디바이스별 디스크 유틸리티 관리 구조체
 * sysfs에서 디바이스 통계를 읽고, 마스터-슬레이브 관계(소프트웨어 RAID)를 관리한다. */
struct disk_util {
	struct flist_head list;       /* 전역 disk_list에 연결되는 노드 */
	/* If this disk is a slave, hook it into the master's
	 * list using this head.
	 */
	struct flist_head slavelist;  /* 마스터의 slaves 리스트에 연결되는 노드 */

	char *sysfs_root;             /* sysfs 디바이스 경로 (예: /sys/block/sda) */
	char path[PATH_MAX];          /* stat 파일 경로 (예: /sys/block/sda/stat) */
	int major, minor;             /* 디바이스 major/minor 번호 */

	struct disk_util_stat dus;      /* 누적 통계 */
	struct disk_util_stat last_dus; /* 이전 폴링의 통계 (델타 계산용) */

	struct disk_util_agg agg;     /* 슬레이브 집계 통계 */

	/* For software raids, this entry maintains pointers to the
	 * entries for the slave devices. The disk_util entries for
	 * the slaves devices should primarily be maintained through
	 * the disk_list list, i.e. for memory allocation and
	 * de-allocation, etc. Whereas this list should be used only
	 * for aggregating a software RAID's disk util figures.
	 */
	struct flist_head slaves;     /* 소프트웨어 RAID의 슬레이브 디바이스 리스트 */

	struct timespec time;         /* 마지막 통계 폴링 시각 */

	struct fio_sem *lock;         /* 동시 접근 보호용 세마포어 */
	unsigned long users;          /* 이 디바이스를 사용하는 스레드 수 */
};

/* [한국어] 디스크 유틸리티 사용자 수 변경 - 슬레이브 디바이스의 사용자 수도 함께 변경 */
static inline void disk_util_mod(struct disk_util *du, int val)
{
	if (du) {
		struct flist_head *n;

		fio_sem_down(du->lock);
		du->users += val;

		flist_for_each(n, &du->slavelist) {
			struct disk_util *slave;

			slave = flist_entry(n, struct disk_util, slavelist);
			slave->users += val;
		}
		fio_sem_up(du->lock);
	}
}

/* [한국어] 디스크 유틸리티 사용자 증가 */
static inline void disk_util_inc(struct disk_util *du)
{
	disk_util_mod(du, 1);
}

/* [한국어] 디스크 유틸리티 사용자 감소 */
static inline void disk_util_dec(struct disk_util *du)
{
	disk_util_mod(du, -1);
}

#define DISK_UTIL_MSEC	(250)  /* 디스크 유틸리티 폴링 주기 (밀리초) */

extern struct flist_head disk_list;  /* 전역 디스크 유틸리티 리스트 */

/*
 * disk util stuff
 */
#ifdef FIO_HAVE_DISK_UTIL
extern void init_disk_util(struct thread_data *);   /* 디스크 유틸리티 초기화 */
extern int update_io_ticks(void);                    /* I/O 통계 갱신 (주기적 호출) */
extern void setup_disk_util(void);                   /* 전역 세마포어 초기화 */
extern void disk_util_prune_entries(void);           /* 모든 디스크 유틸 항목 제거 */
#else
/* keep this as a function to avoid a warning in handle_du() */
#define disk_util_prune_entries()
#define init_disk_util(td)
#define setup_disk_util()

static inline int update_io_ticks(void)
{
	return helper_should_exit();
}
#endif

#endif
