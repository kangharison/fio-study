/*
 * [한국어] io_ddir.h - I/O 방향(direction) 열거형 및 관련 매크로/유틸리티
 *
 * fio에서 I/O 작업의 방향(읽기, 쓰기, trim, sync 등)을 정의한다.
 * 주요 열거형:
 *   - fio_ddir  : 개별 I/O 요청의 방향 (DDIR_READ, DDIR_WRITE, DDIR_TRIM 등)
 *   - td_ddir   : 작업(job) 수준의 I/O 모드 (비트 플래그 조합)
 *
 * 매크로:
 *   - td_read(), td_write() 등 : 작업의 I/O 방향 확인
 *   - ddir_rw_sum() : 읽기+쓰기+trim 합산
 */
#ifndef FIO_DDIR_H
#define FIO_DDIR_H

/* [한국어] 개별 I/O 요청의 방향을 나타내는 열거형 */
enum fio_ddir {
	DDIR_READ = 0,            /* 읽기 */
	DDIR_WRITE = 1,           /* 쓰기 */
	DDIR_TRIM = 2,            /* trim (SSD의 TRIM/UNMAP 명령) */
	DDIR_SYNC = 3,            /* fsync */
	DDIR_DATASYNC,            /* fdatasync */
	DDIR_SYNC_FILE_RANGE,     /* sync_file_range */
	DDIR_SYNCFS,              /* syncfs (전체 파일시스템 동기화) */
	DDIR_WAIT,                /* 대기 (타이머 이벤트) */
	DDIR_LAST,                /* 열거형 끝 표시 (경계값) */
	DDIR_INVAL = -1,          /* 유효하지 않은 방향 */
	DDIR_TIMEOUT = -2,        /* 타임아웃 */

	DDIR_RWDIR_CNT = 3,       /* 읽기/쓰기/trim 3가지 (통계 배열 크기) */
	DDIR_RWDIR_SYNC_CNT = 4,  /* 읽기/쓰기/trim/sync 4가지 */
};

/* [한국어] 읽기/쓰기/trim 3방향을 순회하는 매크로 */
#define for_each_rw_ddir(ddir)	for (enum fio_ddir ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)

/* [한국어] I/O 방향 열거값을 문자열로 변환 */
static inline const char *io_ddir_name(enum fio_ddir ddir)
{
	static const char *name[] = { "read", "write", "trim", "sync",
					"datasync", "sync_file_range",
					"wait", };

	if (ddir >= 0 && ddir < DDIR_LAST)
		return name[ddir];

	return "invalid";
}

/*
 * [한국어] 작업(job) 수준의 I/O 모드를 비트 플래그로 나타내는 열거형
 *
 * 비트 조합으로 다양한 I/O 패턴을 표현한다:
 *   TD_DDIR_READ  (0001) : 순차 읽기
 *   TD_DDIR_WRITE (0010) : 순차 쓰기
 *   TD_DDIR_RAND  (0100) : 랜덤 플래그
 *   TD_DDIR_TRIM  (1000) : trim 플래그
 *
 * 예: randread = READ | RAND = 0101, randrw = READ | WRITE | RAND = 0111
 */
enum td_ddir {
	TD_DDIR_READ		= 1 << 0,  /* 읽기 */
	TD_DDIR_WRITE		= 1 << 1,  /* 쓰기 */
	TD_DDIR_RAND		= 1 << 2,  /* 랜덤 접근 플래그 */
	TD_DDIR_TRIM		= 1 << 3,  /* trim */
	TD_DDIR_RW		= TD_DDIR_READ | TD_DDIR_WRITE,       /* 순차 읽기+쓰기 */
	TD_DDIR_RANDREAD	= TD_DDIR_READ | TD_DDIR_RAND,        /* 랜덤 읽기 */
	TD_DDIR_RANDWRITE	= TD_DDIR_WRITE | TD_DDIR_RAND,       /* 랜덤 쓰기 */
	TD_DDIR_RANDRW		= TD_DDIR_RW | TD_DDIR_RAND,          /* 랜덤 읽기+쓰기 */
	TD_DDIR_RANDTRIM	= TD_DDIR_TRIM | TD_DDIR_RAND,        /* 랜덤 trim */
	TD_DDIR_TRIMWRITE	= TD_DDIR_TRIM | TD_DDIR_WRITE,       /* trim + 쓰기 */
	TD_DDIR_RANDTRIMWRITE	= TD_DDIR_RANDTRIM | TD_DDIR_WRITE,   /* 랜덤 trim + 쓰기 */
};

/* [한국어] 작업(job)의 I/O 방향을 확인하는 매크로들 */
#define td_read(td)		((td)->o.td_ddir & TD_DDIR_READ)    /* 읽기를 수행하는가? */
#define td_write(td)		((td)->o.td_ddir & TD_DDIR_WRITE)   /* 쓰기를 수행하는가? */
#define td_trim(td)		((td)->o.td_ddir & TD_DDIR_TRIM)    /* trim을 수행하는가? */
#define td_rw(td)		(((td)->o.td_ddir & TD_DDIR_RW) == TD_DDIR_RW)  /* 읽기+쓰기 혼합인가? */
#define td_random(td)		((td)->o.td_ddir & TD_DDIR_RAND)    /* 랜덤 접근인가? */
#define file_randommap(td, f)	(!(td)->o.norandommap && fio_file_axmap((f)))  /* 랜덤맵 사용 여부 */
#define td_trimwrite(td)	(((td)->o.td_ddir & TD_DDIR_TRIMWRITE) \
					== TD_DDIR_TRIMWRITE)       /* trim+쓰기인가? */
#define td_randtrimwrite(td)	(((td)->o.td_ddir & TD_DDIR_RANDTRIMWRITE) \
					== TD_DDIR_RANDTRIMWRITE)   /* 랜덤 trim+쓰기인가? */

/* [한국어] 주어진 ddir이 동기화(sync) 계열 명령인지 확인 */
static inline int ddir_sync(enum fio_ddir ddir)
{
	return ddir == DDIR_SYNC || ddir == DDIR_DATASYNC ||
	       ddir == DDIR_SYNC_FILE_RANGE || ddir == DDIR_SYNCFS;
}

/* [한국어] 주어진 ddir이 실제 데이터 I/O(읽기/쓰기/trim)인지 확인 */
static inline int ddir_rw(enum fio_ddir ddir)
{
	return ddir == DDIR_READ || ddir == DDIR_WRITE || ddir == DDIR_TRIM;
}

/* [한국어] 작업 수준 I/O 모드(td_ddir)를 문자열로 변환 */
static inline const char *ddir_str(enum td_ddir ddir)
{
	static const char *__str[] = { NULL, "read", "write", "rw", "rand",
				"randread", "randwrite", "randrw",
				"trim", NULL, "trimwrite", NULL, "randtrim",
				NULL, "randtrimwrite" };

	return __str[ddir];
}

/* [한국어] 읽기+쓰기+trim 배열 값의 합산 */
#define ddir_rw_sum(arr)	\
	((arr)[DDIR_READ] + (arr)[DDIR_WRITE] + (arr)[DDIR_TRIM])

#endif
