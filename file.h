/*
 * [한국어 설명] fio 파일 관련 구조체/상수/함수 헤더 (file.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 fio에서 사용하는 파일 관련 핵심 데이터 구조와 상수를 정의한다.
 * fio_file 구조체(파일 상태), fio_filetype(파일 유형), fio_file_flags(상태 플래그),
 * 파일 잠금 모드, fallocate 모드, 파일 셋업/열기/닫기 API를 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio.h에서 이 헤더를 포함하여 thread_data가 fio_file 배열(files[])을 관리한다.
 * filesetup.c에서 fio_file의 생성/확장/열기/닫기를 구현한다.
 * file.h → filesetup.c(구현) / fio.h(thread_data에 포함) / ioengines.c(파일 열기/닫기)
 *
 * === 타 모듈과의 연결 ===
 * - filesetup.c: fio_file의 생성, 확장, 열기, 닫기 구현
 * - fio.h: thread_data에 files[] 배열로 포함
 * - ioengines.c: td_io_open_file()/td_io_close_file()에서 fio_file 사용
 * - filehash.c: 파일 해시 테이블로 동일 파일 공유 관리
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_file: 개별 파일의 모든 상태 정보 (경로, 크기, fd, 오프셋, 플래그 등)
 * - enum fio_filetype: 파일 유형 (일반 파일, 블록/캐릭터 디바이스, 파이프, 디렉토리)
 * - enum fio_file_flags: 파일 상태 플래그 (열림, 닫는중, 확장 필요 등)
 * - FILE_FLAG_FNS 매크로: 파일 플래그 set/clear/test 인라인 함수 자동 생성
 */
#ifndef FIO_FILE_H
#define FIO_FILE_H

#include <string.h>
#include "compiler/compiler.h"  /* 컴파일러 호환 매크로 (__must_check 등) */
#include "io_ddir.h"            /* I/O 방향 정의 (DDIR_READ, DDIR_WRITE 등) */
#include "flist.h"              /* fio 연결 리스트 */
#include "lib/zipf.h"           /* Zipf 분포 랜덤 생성기 */
#include "lib/axmap.h"          /* 비트맵 기반 랜덤 I/O 블록 추적 */
#include "lib/lfsr.h"           /* LFSR(선형 피드백 시프트 레지스터) 랜덤 생성기 */
#include "lib/gauss.h"          /* 가우스 분포 랜덤 생성기 */

/* Forward declarations */
/* [한국어] 전방 선언 - 실제 정의는 다른 헤더에 있음 */
struct zoned_block_device_info;
struct fdp_ruh_info;

/*
 * The type of object we are working on
 */
/* [한국어] I/O 대상 파일/디바이스의 유형을 나타내는 열거형 */
enum fio_filetype {
	FIO_TYPE_FILE = 1,		/* plain file */        /* 일반 파일 */
	FIO_TYPE_BLOCK,			/* block device */      /* 블록 디바이스 (예: /dev/sda) */
	FIO_TYPE_CHAR,			/* character device */   /* 캐릭터 디바이스 (예: /dev/null) */
	FIO_TYPE_PIPE,			/* pipe */              /* 파이프 (stdin/stdout) */
	FIO_TYPE_DIR,			/* directory */          /* 디렉토리 (syncfs용) */
};

/* [한국어] 파일 상태 플래그 - 비트마스크로 여러 상태를 동시에 표현 */
enum fio_file_flags {
	FIO_FILE_open		= 1 << 0,	/* file is open */           /* 파일이 열려 있음 */
	FIO_FILE_closing	= 1 << 1,	/* file being closed */      /* 파일 닫는 중 */
	FIO_FILE_extend		= 1 << 2,	/* needs extend */           /* 파일 확장 필요 */
	FIO_FILE_done		= 1 << 3,	/* io completed to this file */ /* 이 파일에 대한 I/O 완료 */
	FIO_FILE_size_known	= 1 << 4,	/* size has been set */      /* 파일 크기가 확인됨 */
	FIO_FILE_hashed		= 1 << 5,	/* file is on hash */        /* 파일 해시 테이블에 등록됨 */
	FIO_FILE_partial_mmap	= 1 << 6,	/* can't do full mmap */     /* 전체 mmap 불가 (부분만 가능) */
	FIO_FILE_axmap		= 1 << 7,	/* uses axmap */             /* axmap 비트맵 사용 중 */
	FIO_FILE_lfsr		= 1 << 8,	/* lfsr is used */           /* LFSR 랜덤 생성기 사용 중 */
	FIO_FILE_smalloc	= 1 << 9,	/* smalloc file/file_name */ /* 공유 메모리(smalloc)로 할당됨 */
};

/* [한국어] 파일 잠금 모드 - 멀티스레드/멀티프로세스 환경에서 파일 접근 제어 */
enum file_lock_mode {
	FILE_LOCK_NONE,        /* 잠금 없음 */
	FILE_LOCK_EXCLUSIVE,   /* 배타적 잠금 (세마포어) */
	FILE_LOCK_READWRITE,   /* 읽기/쓰기 잠금 (rwlock) */
};

/*
 * How fio chooses what file to service next. Choice of uniformly random, or
 * some skewed random variants, or just sequentially go through them or
 * roundrobing.
 */
/* [한국어] 파일 서비스 순서 정책 - 다음에 I/O할 파일을 선택하는 방식 */
enum {
	FIO_FSERVICE_RANDOM		= 1,  /* 균일 랜덤 선택 */
	FIO_FSERVICE_RR			= 2,  /* 라운드 로빈 */
	FIO_FSERVICE_SEQ		= 3,  /* 순차적 선택 */
	__FIO_FSERVICE_NONUNIFORM	= 0x100,  /* 비균일 분포 플래그 비트 */
	FIO_FSERVICE_ZIPF		= __FIO_FSERVICE_NONUNIFORM | 4,  /* Zipf 분포 */
	FIO_FSERVICE_PARETO		= __FIO_FSERVICE_NONUNIFORM | 5,  /* 파레토 분포 */
	FIO_FSERVICE_GAUSS		= __FIO_FSERVICE_NONUNIFORM | 6,  /* 가우스 분포 */

	FIO_FSERVICE_SHIFT		= 10,  /* 서비스 유형 비트 시프트 값 */
};

/*
 * No pre-allocation when laying down files, or call posix_fallocate(), or
 * call fallocate() with FALLOC_FL_KEEP_SIZE set.
 */
/* [한국어] 파일 사전 할당(fallocate) 방식 - 파일 레이아웃 시 디스크 공간 확보 방법 */
enum fio_fallocate_mode {
	FIO_FALLOCATE_NONE	= 1,  /* 사전 할당 안 함 */
	FIO_FALLOCATE_POSIX	= 2,  /* posix_fallocate() 사용 */
	FIO_FALLOCATE_KEEP_SIZE	= 3,  /* fallocate(FALLOC_FL_KEEP_SIZE) - 파일 크기 유지하며 할당 */
	FIO_FALLOCATE_NATIVE	= 4,  /* OS 네이티브 fallocate 사용 */
	FIO_FALLOCATE_TRUNCATE	= 5,  /* ftruncate()로 크기 조정 */
};

/*
 * Each thread_data structure has a number of files associated with it,
 * this structure holds state information for a single file.
 */
/*
 * [한국어] fio_file 구조체 - fio의 핵심 파일 상태 구조체
 *
 * 각 thread_data(작업 스레드)는 여러 파일을 가질 수 있으며,
 * 이 구조체는 개별 파일의 모든 상태 정보를 보관한다.
 * 파일 디스크립터, 크기, 오프셋, 랜덤 맵, 잠금 등을 포함한다.
 */
struct fio_file {
	struct flist_head hash_list;  /* 파일 해시 테이블 연결 리스트 노드 */
	enum fio_filetype filetype;   /* 파일 유형 (일반/블록/캐릭터/파이프/디렉토리) */

	int fd;            /* 파일 디스크립터 */
	int shadow_fd;     /* 그림자 fd - 해시 충돌 시 이전 fd를 보관 (Linux blkid 우회용) */
#ifdef WIN32
	HANDLE hFile;      /* Windows 파일 핸들 */
	HANDLE ioCP;       /* Windows I/O 완료 포트 */
#endif

	/*
	 * filename and possible memory mapping
	 */
	/* [한국어] 파일 이름 및 디바이스 번호 */
	unsigned int major, minor;  /* 디바이스 메이저/마이너 번호 */
	int fileno;                 /* 파일 배열 내 인덱스 번호 */
	char *file_name;            /* 파일 경로명 */

	/*
	 * size of the file, offset into file, and io size from that offset
	 * (be aware io_size is different from thread_options::io_size)
	 */
	/* [한국어] 파일 크기 및 I/O 범위 설정 */
	uint64_t real_file_size;  /* 실제 파일/디바이스 크기 */
	uint64_t file_offset;     /* 파일 내 I/O 시작 오프셋 */
	uint64_t io_size;         /* file_offset부터의 I/O 대상 크기 (thread_options::io_size와 다름) */

	struct fio_ruhs_info *ruhs_info;      /* FDP(Flexible Data Placement) RUH 정보 */
	struct fio_ruhs_scheme *ruhs_scheme;   /* FDP RUH 배치 스킴 */

	/*
	 * Zoned block device information. See also zonemode=zbd.
	 */
	/* [한국어] ZBD(Zoned Block Device) 관련 정보 */
	struct zoned_block_device_info *zbd_info;  /* ZBD 디바이스 정보 */
	/* zonemode=zbd working area */
	uint32_t min_zone;	/* inclusive */  /* I/O 대상 최소 존 번호 (포함) */
	uint32_t max_zone;	/* exclusive */  /* I/O 대상 최대 존 번호 (미포함) */

	/* SP Random Info */
	/* [한국어] SP(Structured Placement) 랜덤 정보 */
	struct sprandom_info *spr_info;

	/*
	 * Track last end and last start of IO for a given data direction
	 */
	/* [한국어] 각 I/O 방향(읽기/쓰기/트림)별 마지막 위치 추적 */
	uint64_t last_pos[DDIR_RWDIR_CNT];    /* 각 방향별 마지막 I/O 종료 위치 */
	uint64_t last_start[DDIR_RWDIR_CNT];  /* 각 방향별 마지막 I/O 시작 위치 */

	uint64_t first_write;  /* 첫 번째 쓰기 위치 */
	uint64_t last_write;   /* 마지막 쓰기 위치 */

	/*
	 * For use by the io engine to store offset
	 */
	/* [한국어] I/O 엔진이 내부적으로 사용하는 오프셋 */
	uint64_t engine_pos;

	/*
	 * For use by the io engine for private data storage
	 */
	/* [한국어] I/O 엔진 전용 사설 데이터 저장 공간 */
	void *engine_data;

	/*
	 * if io is protected by a semaphore, this is set
	 */
	/* [한국어] 파일 I/O 동기화를 위한 잠금 - 세마포어 또는 rwlock 중 하나 사용 */
	union {
		struct fio_sem *lock;       /* 배타적 잠금용 세마포어 */
		struct fio_rwlock *rwlock;  /* 읽기/쓰기 잠금용 rwlock */
	};

	/*
	 * block map or LFSR for random io
	 */
	/* [한국어] 랜덤 I/O용 블록 추적 - axmap(비트맵) 또는 LFSR 중 하나 사용 */
	union {
		struct axmap *io_axmap;  /* 비트맵 기반 블록 맵 (이미 I/O한 블록 추적) */
		struct fio_lfsr lfsr;    /* LFSR 기반 랜덤 블록 생성기 */
	};

	/*
	 * Used for zipf random distribution
	 */
	/* [한국어] 비균일 랜덤 분포용 - Zipf 또는 가우스 분포 중 하나 사용 */
	union {
		struct zipf_state zipf;    /* Zipf/Pareto 분포 상태 */
		struct gauss_state gauss;  /* 가우스(정규) 분포 상태 */
	};

	int references;              /* 이 파일을 참조하는 활성 I/O 수 */
	enum fio_file_flags flags;   /* 파일 상태 플래그 비트마스크 */

	struct disk_util *du;        /* 디스크 유틸리티 통계 */
};

/* [한국어] I/O 엔진 전용 데이터 접근 매크로 */
#define FILE_ENG_DATA(f)		((f)->engine_data)
#define FILE_SET_ENG_DATA(f, data)	((f)->engine_data = (data))

/*
 * [한국어] FILE_FLAG_FNS 매크로 - 파일 플래그 조작 인라인 함수를 자동 생성
 *
 * 각 플래그(name)에 대해 3개의 함수를 생성:
 *   fio_file_set_##name(f)   - 플래그 설정 (비트 OR)
 *   fio_file_clear_##name(f) - 플래그 해제 (비트 AND NOT)
 *   fio_file_##name(f)       - 플래그 테스트 (비트 AND, 0이 아니면 true)
 */
#define FILE_FLAG_FNS(name)						\
static inline void fio_file_set_##name(struct fio_file *f)		\
{									\
	(f)->flags = (enum fio_file_flags) ((f)->flags | FIO_FILE_##name);	\
}									\
static inline void fio_file_clear_##name(struct fio_file *f)		\
{									\
	(f)->flags = (enum fio_file_flags) ((f)->flags & ~FIO_FILE_##name);	\
}									\
static inline int fio_file_##name(struct fio_file *f)			\
{									\
	return ((f)->flags & FIO_FILE_##name) != 0;			\
}

/* [한국어] 각 파일 플래그에 대한 set/clear/test 함수 생성 */
FILE_FLAG_FNS(open);          /* fio_file_set_open / fio_file_clear_open / fio_file_open */
FILE_FLAG_FNS(closing);       /* fio_file_set_closing / fio_file_clear_closing / fio_file_closing */
FILE_FLAG_FNS(extend);        /* fio_file_set_extend / fio_file_clear_extend / fio_file_extend */
FILE_FLAG_FNS(done);          /* fio_file_set_done / fio_file_clear_done / fio_file_done */
FILE_FLAG_FNS(size_known);    /* fio_file_set_size_known / ... / fio_file_size_known */
FILE_FLAG_FNS(hashed);        /* fio_file_set_hashed / ... / fio_file_hashed */
FILE_FLAG_FNS(partial_mmap);  /* fio_file_set_partial_mmap / ... / fio_file_partial_mmap */
FILE_FLAG_FNS(axmap);         /* fio_file_set_axmap / ... / fio_file_axmap */
FILE_FLAG_FNS(lfsr);          /* fio_file_set_lfsr / ... / fio_file_lfsr */
FILE_FLAG_FNS(smalloc);       /* fio_file_set_smalloc / ... / fio_file_smalloc */
#undef FILE_FLAG_FNS

/*
 * File FS mount information.
 */
/*
 * [한국어] fio_mount 구조체 - 파일이 위치한 파일 시스템 마운트 정보
 *
 * end_syncfs 옵션 사용 시, 동일 파일 시스템의 파일들을 그룹화하여
 * syncfs()를 한 번만 호출하기 위해 사용한다.
 */
struct fio_mount {
	struct flist_head list;   /* 마운트 리스트 연결 노드 */
	const char *base;         /* 마운트 베이스 경로 포인터 */
	char __base[256];         /* 마운트 베이스 경로 저장 버퍼 */
	unsigned int key;         /* st_dev 값 (디바이스 식별자) - 같은 FS인지 판별 */
	struct fio_file *f;       /* 연관된 fio_file (syncfs용 디렉토리 파일) */
	void *dir;                /* opendir()로 열린 DIR 포인터 */
};

/*
 * File setup/shutdown
 */
/* [한국어] 파일 셋업/해제 관련 외부 함수 선언 */
struct thread_data;
extern void close_files(struct thread_data *);                                    /* 모든 열린 파일 닫기 */
extern void close_and_free_files(struct thread_data *);                           /* 파일 닫기 + 메모리 해제 + unlink */
extern uint64_t get_start_offset(struct thread_data *, struct fio_file *);        /* I/O 시작 오프셋 계산 */
extern int __must_check setup_files(struct thread_data *);                        /* 파일 셋업 메인 함수 (크기 확인, 생성, 확장) */
extern int __must_check file_invalidate_cache(struct thread_data *, struct fio_file *); /* 파일 캐시 무효화 */
#ifdef __cplusplus
extern "C" {
#endif
extern int __must_check generic_open_file(struct thread_data *, struct fio_file *);       /* 범용 파일 열기 */
extern int __must_check generic_close_file(struct thread_data *, struct fio_file *);      /* 범용 파일 닫기 */
extern int __must_check generic_get_file_size(struct thread_data *, struct fio_file *);   /* 범용 파일 크기 조회 */
extern int __must_check generic_prepopulate_file(struct thread_data *, struct fio_file *);/* 파일에 데이터 사전 채우기 */
#ifdef __cplusplus
}
#endif
extern int __must_check file_lookup_open(struct fio_file *f, int flags);   /* 해시에서 파일 조회 후 열기 */
extern bool __must_check pre_read_files(struct thread_data *);             /* 모든 파일 사전 읽기 (캐시 워밍) */
extern unsigned long long get_rand_file_size(struct thread_data *td);      /* 랜덤 파일 크기 생성 (file_size_low~high 범위) */
extern int add_file(struct thread_data *, const char *, int, int);         /* 파일 추가 */
extern int add_file_exclusive(struct thread_data *, const char *);         /* 중복 없이 파일 추가 */
extern void get_file(struct fio_file *);                                   /* 파일 참조 카운트 증가 */
extern int __must_check put_file(struct thread_data *, struct fio_file *); /* 파일 참조 카운트 감소 + 0이면 닫기 */
extern void put_file_log(struct thread_data *, struct fio_file *);         /* 로그용 put_file */
extern void lock_file(struct thread_data *, struct fio_file *, enum fio_ddir);  /* 파일 잠금 획득 */
extern void unlock_file(struct thread_data *, struct fio_file *);          /* 파일 잠금 해제 */
extern void unlock_file_all(struct thread_data *, struct fio_file *);      /* 모든 파일 잠금 해제 */
extern int add_dir_files(struct thread_data *, const char *);              /* 디렉토리 내 파일 재귀 추가 */
extern bool init_random_map(struct thread_data *);                         /* 랜덤 I/O 맵 초기화 */
extern void dup_files(struct thread_data *, struct thread_data *);         /* 파일 목록 복제 (job 복제 시) */
extern int get_fileno(struct thread_data *, const char *);                 /* 파일명으로 인덱스 조회 */
extern void free_release_files(struct thread_data *);                      /* 파일 해제 및 리소스 정리 */
extern void filesetup_mem_free(void);                                      /* 초기화 단계 메모리 해제 */
extern void fio_file_reset(struct thread_data *, struct fio_file *);       /* 파일 상태 초기화 (위치, axmap 등) */
extern bool fio_files_done(struct thread_data *);                          /* 모든 파일 I/O 완료 여부 확인 */
extern bool exists_and_not_regfile(const char *);                          /* 파일 존재하며 일반 파일이 아닌지 확인 */
extern int fio_set_directio(struct thread_data *, struct fio_file *);      /* Direct I/O 설정 (O_DIRECT 미지원 플랫폼용) */
extern void fio_file_free(struct fio_file *);                              /* fio_file 구조체 메모리 해제 */
#ifdef CONFIG_SYNCFS
extern int fio_open_fs(struct thread_data *td, struct fio_mount *fm);      /* syncfs용 FS 디렉토리 열기 */
extern void fio_close_fs(struct fio_mount *fm);                            /* syncfs용 FS 디렉토리 닫기 */
#endif

#endif
