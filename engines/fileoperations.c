/*
 * [한국어 설명] fileoperations I/O 엔진 구현 (fileoperations.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 **데이터 I/O를 전혀 수행하지 않는** 특수 엔진 6종(filecreate/
 * filestat/filedelete, dircreate/dirstat/dirdelete)을 한 모듈에 구현한다.
 * 측정 대상은 블록 읽기/쓰기가 아니라 **파일시스템 메타연산**의 지연 시간
 * 즉 open(O_CREAT)·stat/lstat/statx·unlink·mkdir·rmdir에 대한 VFS 경로의
 * 레이턴시이다. 엔진은 잡이 정의한 파일명 목록(fio_file[])을 대상으로 해당
 * 연산만 반복하고, 각 연산의 소요 시간을 fio_gettime + ntime_since_now로
 * 재 add_clat_sample()로 통계에 누적한다. 실제 read/write는 호출하지 않으므로
 * FIO_FAKEIO 플래그로 선언되며, queue()는 항상 즉시 FIO_Q_COMPLETED를 반환한다.
 * stat 계열은 옵션(stat_type)으로 stat(2)/lstat(2)/statx(2) 중 선택 가능해
 * 리눅스 메타데이터 조회 경로별 성능 차이를 비교하는 벤치마크에 적합하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 엔진 계층(ioengines.c)은 각 잡(thread_data)에 대해 init → setup →
 * get_file_size → open_file → (loop: queue → getevents) → close_file →
 * cleanup 순으로 ioengine_ops 콜백을 호출한다. 본 파일은 동기(FIO_SYNCIO)
 * 엔진이라 queue()가 곧 완료를 반환하며 commit/getevents 경로가 비어 있다.
 * 또한 FIO_DISKLESSIO 플래그로 디스크 없이 순수 파일시스템 경로만 검증한다.
 * 핵심 트릭은 **"연산을 open_file 콜백 안에서 수행"**하는 것이다 — 즉
 * create/stat/delete를 실제 I/O 루프 안이 아닌 파일 오픈 단계에서 1회씩
 * 수행해 그 지연을 측정하고, queue()는 빈 동작으로 완료 처리한다. 실행
 * 컨텍스트는 fio 잡 스레드(유저스페이스)이며, 커널 측은 VFS → 파일시스템
 * 드라이버(inode allocator / dentry cache) 경로를 거친다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위 호출자(ioengines.c): td_io_init → init(), setup_files → setup()
 *   (dircreate/dirstat/dirdelete만), td_io_open_file → open_file() 콜백
 *   (여기서 실제 메타연산 발생), td_io_queue → queue_io(), td_io_unlink_file
 *   → unlink_file() (디렉터리 엔진), td_io_cleanup → cleanup().
 * - 하위 호출 대상: glibc/POSIX wrappers — open(2), unlink(2), stat(2),
 *   lstat(2), statx(2) (oslib/statx.h 제공, 리눅스 4.11+), mkdir(2) /
 *   rmdir(2) (fio_mkdir 래퍼), realpath(3).
 * - 옵션 그룹: optgroup.h의 FIO_OPT_C_ENGINE(카테고리) / FIO_OPT_G_FILESTAT
 *   (서브그룹). fio --enghelp filestat 출력에 stat_type 옵션이 표시된다.
 * - 공유 상태: thread_data::io_ops_data에 fc_data(엔진 런타임 상태),
 *   thread_data::eo에 filestat_options(옵션 값), fio_file::file_name (타겟
 *   경로), fio_file::fd (open_file 엔진에서만 의미 있음).
 * - 통계: add_clat_sample()로 td->ts.clat_stat[stat_ddir]에 누적된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - init()/cleanup(): fc_data 할당·해제 및 엔진명 접두사(file/dir)로 op_engine 결정.
 * - setup_dirs(): dircreate/dirstat/dirdelete가 테스트 대상 디렉터리를 먼저 만든다.
 * - open_file(): FILE은 open(O_CREAT), DIR은 mkdir. 지연 측정 및 add_clat_sample.
 * - stat_file(): stat_type에 따라 stat/lstat/statx 수행, 지연 측정.
 * - delete_file(): FILE은 unlink, DIR은 rmdir. 지연 측정.
 * - queue_io(): 실제 I/O 없이 FIO_Q_COMPLETED 반환(동기 FAKEIO 계약).
 * - remove_dir()/invalidate_do_nothing()/get_file_size(): 보조 콜백.
 * - ioengine_filecreate/filestat/filedelete/dircreate/dirstat/dirdelete: 6개의
 *   ioengine_ops 인스턴스. register/unregister는 fio_init/fio_exit 훅.
 * - struct fc_data: {stat_ddir(통계 방향), op_engine(FILE/DIR)}.
 * - struct filestat_options: {stat_type (stat/lstat/statx 선택)}.
 */

/*
 * file/directory operations engine
 *
 * IO engine that doesn't do any IO, just operates files/directories
 * and tracks the latency of the operation.
 */
#include <stdio.h>	/* [한국어] snprintf 등 표준 C I/O — td_verror 메시지 포매팅에 사용. */
#include <stdlib.h>	/* [한국어] calloc/free/realpath — fc_data 할당과 절대경로 변환에 필요. */
#include <fcntl.h>	/* [한국어] open(2)과 O_CREAT/O_RDWR 플래그 정의 — 파일 생성 경로. */
#include <errno.h>	/* [한국ари] errno 전역 — open/stat/unlink 등의 실패 원인 코드를 td_verror에 전달. */
#include <sys/types.h>	/* [한국어] mode_t·off_t 등 POSIX 타입 — mkdir 권한 인자 타입에 필요. */
#include <sys/stat.h>	/* [한국어] struct stat 및 stat/lstat/S_IFDIR 정의 — 메타 조회 API 전체. */
#include <unistd.h>	/* [한국어] close(2)/unlink(2)/rmdir(2) 선언 — delete 경로의 핵심 syscall. */
#include "../fio.h"	/* [한국어] fio 공용 API: thread_data, fio_file, ioengine_ops, add_clat_sample,
			 * td_verror, fio_gettime, ntime_since_now, dprint, log_err, for_each_file 등. */
#include "../optgroup.h"/* [한국어] FIO_OPT_C_ENGINE/FIO_OPT_G_FILESTAT 옵션 카테고리/그룹 상수 — options 테이블에 사용. */
#include "../oslib/statx.h"/* [한국어] statx(2) 래퍼 — 리눅스 커널 4.11+에서만 제공되므로 fio가 OS 추상화해서 제공. */

/*
 * [한국어] 이 파일이 구현하는 엔진의 종류를 구분하는 열거형.
 * init()에서 td->o.ioengine 문자열 접두사("file"/"dir")를 비교해 결정되며,
 * open_file/delete_file이 FILE(open/unlink) 경로와 DIR(mkdir/rmdir) 경로를
 * 분기할 때 스위치 기준으로 사용된다.
 */
enum fio_engine {
	UNKNOWN_OP_ENGINE = 0,
	/* [한국어] 초기값 — init() 시작 시 설정된다. 엔진명이 "file"/"dir"로 시작하지
	 * 않는 비정상 상태를 방어적으로 탐지하기 위한 센티널 값.
	 * 설정자: init()의 `data->op_engine = UNKNOWN_OP_ENGINE;` 라인.
	 * 읽는 자: open_file/delete_file의 `else { log_err(...); return 1; }` 분기.
	 * 동기화: 잡별(thread_data별) 독립 변수라 스레드 간 공유 없음. */

	FILE_OP_ENGINE = 1,
	/* [한국어] 파일 엔진(filecreate/filestat/filedelete)임을 나타냄.
	 * 설정자: init()에서 strncmp(td->o.ioengine, "file", 4)==0일 때.
	 * 읽는 자: open_file은 open(O_CREAT)로 파일 생성, delete_file은 unlink.
	 * filestat은 stat_file 콜백을 직접 open_file 필드로 연결하므로 이 플래그를
	 * 체크하지 않고도 동작한다. */

	DIR_OP_ENGINE = 2,
	/* [한국어] 디렉터리 엔진(dircreate/dirstat/dirdelete)임을 나타냄.
	 * 설정자: init()에서 strncmp(td->o.ioengine, "dir", 3)==0일 때.
	 * 읽는 자: open_file은 fio_mkdir 경로로 분기, delete_file은 rmdir 경로로 분기.
	 * setup_dirs/remove_dir도 이 엔진들에서만 unlink_file로 등록된다. */
};

/* [한국어] fileoperations 엔진의 내부 상태 구조체 — thread_data::io_ops_data에 저장. */
struct fc_data {
	enum fio_ddir stat_ddir;
	/* [한국어] add_clat_sample()에 전달할 데이터 방향(DDIR_READ / DDIR_WRITE).
	 * 설정자: init()에서 td_read(td)이면 READ, td_write(td)이면 WRITE.
	 *          즉 잡의 rw 옵션(read/write/randread/randwrite)에 따라 결정.
	 * 읽는 자: open_file/stat_file/delete_file의 지연 샘플 기록 경로.
	 * 값 범위: DDIR_READ(0) 또는 DDIR_WRITE(1). DDIR_TRIM은 사용 안 함.
	 * 동기화: 잡 스레드 단일 소유로 락 불필요. */

	enum fio_engine op_engine;
	/* [한국어] 이 잡이 파일 엔진인지 디렉터리 엔진인지 구분하는 플래그.
	 * 설정자: init()에서 td->o.ioengine 이름 접두사로 결정.
	 * 읽는 자: open_file, delete_file에서 open vs mkdir / unlink vs rmdir 분기.
	 * 값 범위: enum fio_engine의 세 값 중 하나.
	 * 동기화: 잡 스레드 단일 소유. */
};

/*
 * [한국어] filestat/dirstat 엔진 전용 옵션 구조체.
 * thread_data::eo로 접근되며 fio 옵션 파서가 stat_type 값을 .off1 오프셋에 기록한다.
 */
struct filestat_options {
	void *pad;
	/* [한국어] fio 옵션 시스템의 관례적 필드 — 구조체 첫 워드에 내부 용도 포인터 공간을
	 * 확보하기 위한 패딩. 옵션 파서가 내부적으로 사용하며 엔진 코드는 건드리지 않는다.
	 * 설정자/읽는 자: fio 옵션 파서 내부. 값 범위: 보통 NULL. 동기화: 파서 단일 진입. */

	unsigned int stat_type;
	/* [한국어] stat 호출 종류 선택(FIO_FILESTAT_STAT/LSTAT/STATX 중 하나).
	 * 설정자: options 테이블의 .off1 = offsetof(filestat_options, stat_type)를 통해
	 *          파서가 잡 파일의 stat_type=... 지시자 값을 기록.
	 * 읽는 자: stat_file()의 switch 문.
	 * 값 범위: 1(stat) / 2(lstat) / 3(statx). 기본값은 옵션 테이블의 .def="stat".
	 * 동기화: 잡 스레드 단일 읽기. */
};

/*
 * [한국어] stat_type 옵션의 가능한 값. posval 테이블의 .oval과 1:1 대응된다.
 */
enum {
	FIO_FILESTAT_STAT	= 1,	/* [한국어] stat(2) 사용 — 심볼릭 링크 추적, 기본값. */
	FIO_FILESTAT_LSTAT	= 2,	/* [한국어] lstat(2) 사용 — 링크 자체의 메타데이터 조회. */
	FIO_FILESTAT_STATX	= 3,	/* [한국어] statx(2) 사용 — 리눅스 4.11+의 확장 stat(마운트 ID, btime 등). */
};

/*
 * [한국어] filestat/dirstat 엔진이 노출하는 옵션 테이블.
 * 엔진 ops의 .options 필드에 연결되어 파서가 잡 파일에서 "stat_type=..." 지시자를
 * 발견하면 이 배열을 따라 값을 해석한다. NULL 이름 엔트리가 종단 마커.
 */
static struct fio_option options[] = {
	{
		.name	= "stat_type",					/* [한국어] 잡 파일/CLI에서 쓰는 키 이름. */
		.lname	= "stat_type",					/* [한국어] 긴 이름 — 현재 동일. */
		.type	= FIO_OPT_STR,					/* [한국어] 문자열 열거형(posval) 옵션 타입. */
		.off1	= offsetof(struct filestat_options, stat_type),	/* [한국어] 값을 저장할 구조체 오프셋 — td->eo 기반. */
		.help	= "Specify stat system call type to measure lookup/getattr performance",
									/* [한국어] fio --enghelp filestat 도움말에 노출되는 설명. */
		.def	= "stat",					/* [한국어] 기본값 — 미지정 시 stat(2) 사용. */
		.posval = {						/* [한국어] 허용 값 목록(문자열↔정수 매핑). */
			  { .ival = "stat",				/* [한국어] 입력 문자열. */
			    .oval = FIO_FILESTAT_STAT,			/* [한국어] 내부 정수 값(1). */
			    .help = "Use stat(2)",			/* [한국어] 각 값별 부연 설명. */
			  },
			  { .ival = "lstat",
			    .oval = FIO_FILESTAT_LSTAT,			/* [한국어] lstat: 심볼릭 링크 비추적. */
			    .help = "Use lstat(2)",
			  },
			  { .ival = "statx",
			    .oval = FIO_FILESTAT_STATX,			/* [한국어] statx: 확장 메타데이터, 리눅스 한정. */
			    .help = "Use statx(2) if exists",
			  },
		},
		.category = FIO_OPT_C_ENGINE,				/* [한국어] 옵션 카테고리: I/O 엔진. */
		.group	= FIO_OPT_G_FILESTAT,				/* [한국어] 서브그룹: filestat 전용. */
	},
	{
		.name	= NULL,						/* [한국어] 테이블 종단 마커 — 파서가 여기서 순회 중단. */
	},
};

/*
 * [한국어]
 * setup_dirs - dircreate/dirstat/dirdelete 엔진의 setup 단계에서 모든 대상 디렉터리를 선-생성.
 *
 * @td: fio 잡 컨텍스트. td->files[]의 파일명 목록이 테스트 디렉터리 경로.
 * @return: 0 성공, 0이 아니면 실패(첫 실패 지점에서 break). errno==EEXIST는 성공 취급.
 *
 * 왜 필요한가: dirstat/dirdelete 엔진은 "이미 존재하는 디렉터리"를 조회/삭제해야 하므로
 * 실측 루프 전에 디렉터리들이 생성되어 있어야 한다. dircreate는 측정 자체가 생성이지만
 * 실행 중 필요한 부모 디렉터리 확보 용도로 setup을 재사용한다.
 * 실행 컨텍스트: 메인 스레드(ioengine_ops.setup 단계), fio_backend 초기화 구간.
 * caller: ioengines.c의 td_io_init → ops->setup 호출.
 * callee: fio_mkdir (OS 추상화 mkdir) → mkdir(2) 시스템 콜.
 * 에러 경로: log_err로 출력 후 비-EEXIST 에러만 상위 실패로 전파.
 *
 * 호출 체인: td_io_init → setup_dirs → fio_mkdir → mkdir(2).
 */
static int setup_dirs(struct thread_data *td)
{
	int ret = 0;			/* [한국어] 루프 결과 누적 — 마지막 실패 코드를 상위에 반환. */
	int i;				/* [한국어] for_each_file 매크로의 인덱스 변수. */
	struct fio_file *f;		/* [한국어] 현재 순회 중인 파일 디스크립터 객체(여기서는 디렉터리 경로). */

	for_each_file(td, f, i) {	/* [한국어] 잡에 속한 모든 대상 경로를 순회 — files[] 배열 기반 매크로. */
		dprint(FD_FILE, "setup directory %s\n", f->file_name);	/* [한국어] 디버그 로그(--debug=file). */
		ret = fio_mkdir(f->file_name, 0700);			/* [한국어] 권한 0700으로 디렉터리 생성. fio_mkdir은 OS별 mkdir 래퍼. */
		if ((ret && errno != EEXIST)) {				/* [한국어] 실패하고 "이미 존재"가 아니면 치명적 에러. */
			log_err("create directory %s failed with %d\n",
				f->file_name, errno);			/* [한국어] 사용자에게 errno 출력(인간 가독 메시지는 생략). */
			break;						/* [한국어] 루프 중단 — 첫 실패 지점 보고. */
		}
		ret = 0;						/* [한국어] EEXIST였다면 성공으로 취급해 ret 초기화. */
	}
	return ret;							/* [한국어] 마지막 (실패 또는 0) 상태를 상위에 반환. */
}

/*
 * [한국어]
 * open_file - filecreate/dircreate 엔진의 open_file 콜백. 실측 대상 연산(open/mkdir)을
 *              실제로 수행하면서 그 지연을 add_clat_sample로 기록한다.
 *
 * @td: fio 잡. td->io_ops_data = fc_data, td->o.disable_lat 플래그 참조.
 * @f:  대상 파일/디렉터리. f->file_name이 경로, f->fd에 결과 FD/상태 저장.
 * @return: 0 성공, 1 실패. 실패 시 td_verror로 에러 문자열을 잡 통계에 기록.
 *
 * 왜 필요한가: fileoperations 엔진은 I/O 루프(queue)가 아닌 **파일 오픈 단계**에서
 * 측정하고자 하는 메타연산을 수행해 그 지연만을 clat 샘플로 집계한다.
 * 실행 컨텍스트: 잡 스레드. 각 파일에 대해 1회 호출됨(반복 반복 측정이 필요하면
 * fio의 loops/file opens 옵션으로 재오픈을 유발해야 함).
 * caller: ioengines.c td_io_open_file → ops->open_file.
 * callee: open(2)(O_CREAT|O_RDWR, 0600), fio_mkdir→mkdir(2), fio_gettime, ntime_since_now,
 *         add_clat_sample, td_verror.
 *
 * 호출 체인: fio_backend → init_io_u → td_io_open_file → open_file() →
 *            open(2) 또는 mkdir(2) → add_clat_sample.
 */
static int open_file(struct thread_data *td, struct fio_file *f)
{
	struct timespec start;					/* [한국어] fio_gettime로 기록할 시작 시각 — 나노초 정밀. */
	int do_lat = !td->o.disable_lat;			/* [한국어] 지연 측정 플래그 — 사용자가 lat 기록을 껐다면 측정도 스킵. */
	struct fc_data *fcd = td->io_ops_data;			/* [한국어] init()에서 할당한 엔진 상태 참조. */

	dprint(FD_FILE, "fd open %s\n", f->file_name);		/* [한국어] 디버그: 지금 여는 경로. */

	if (f->filetype != FIO_TYPE_FILE) {			/* [한국어] 블록 디바이스/캐릭터 디바이스는 허용 안 함 — 메타 엔진의 제약. */
		log_err("fio: only files are supported\n");
		return 1;					/* [한국어] 잘못된 타겟 타입 → 실패. */
	}
	if (!strcmp(f->file_name, "-")) {			/* [한국어] 표준입출력 지시자는 의미 없음. */
		log_err("fio: can't read/write to stdin/out\n");
		return 1;
	}

	if (do_lat)
		fio_gettime(&start, NULL);			/* [한국어] 시작 시각 샘플링(clock_gettime 기반). */

	if (fcd->op_engine == FILE_OP_ENGINE)
		f->fd = open(f->file_name, O_CREAT|O_RDWR, 0600);/* [한국어] 파일 생성 — O_CREAT는 없으면 inode 할당, 권한 0600. */
	else if (fcd->op_engine == DIR_OP_ENGINE)
		f->fd = fio_mkdir(f->file_name, S_IFDIR);	/* [한국어] 디렉터리 생성. S_IFDIR 비트는 파일타입 힌트(주의: mkdir의 모드는 권한만 쓰며 S_IFDIR은 OS별로 무시될 수 있음 — 레거시 코드). */
	else {
		log_err("fio: unknown file/directory operation engine\n");
		return 1;					/* [한국어] init()에서 엔진 판별 실패 시 방어 경로. */
	}

	if (f->fd == -1) {					/* [한국어] open/mkdir 실패 — errno 보존 후 에러 문자열 생성. */
		char buf[FIO_VERROR_SIZE];			/* [한국어] fio 에러 메시지 고정 크기 버퍼. */
		int e = errno;					/* [한국어] td_verror 호출 전 errno 복사(중간 호출로 덮일 가능성 대비). */

		snprintf(buf, sizeof(buf), "open(%s)", f->file_name);	/* [한국어] 실패 맥락 문자열. */
		td_verror(td, e, buf);				/* [한국어] 잡 통계에 에러 등록 — 이후 fio가 종료 이유로 사용. */
		return 1;
	}

	if (do_lat) {
		struct fc_data *data = td->io_ops_data;		/* [한국어] fcd와 동일하나 스코프 지역 재획득(원본 유지). */
		uint64_t nsec;					/* [한국어] 측정된 지연(ns). */

		nsec = ntime_since_now(&start);			/* [한국어] start 이후 경과 시간(ns). */
		add_clat_sample(td, data->stat_ddir, nsec, 0, NULL);	/* [한국어] clat 히스토그램에 한 샘플 기록 — stat_ddir 방향 버킷. */
	}

	return 0;						/* [한국어] open/mkdir 성공 및 측정 완료. */
}

/*
 * [한국어]
 * stat_file - filestat/dirstat 엔진의 open_file 콜백. 실제로는 파일을 "열지" 않고
 *             stat/lstat/statx 중 하나로 메타데이터 조회 1회의 지연을 측정한다.
 *
 * @td: td->eo에서 filestat_options를 가져와 stat_type 분기.
 * @f:  조회할 파일/디렉터리 경로(f->file_name).
 * @return: 0 성공 / 1 실패.
 *
 * 왜 필요한가: stat 계열은 FD를 연 채로 lat를 측정하지 않고 경로 조회 자체의
 * lat을 봐야 하므로 open 대신 stat 호출을 open_file 콜백에 매핑한다.
 * 실행 컨텍스트: 잡 스레드.
 * caller: td_io_open_file → ops->open_file(= stat_file).
 * callee: stat(2) / lstat(2) / statx(2) / realpath(3).
 *
 * 호출 체인: td_io_open_file → stat_file → stat/lstat/statx → add_clat_sample.
 */
static int stat_file(struct thread_data *td, struct fio_file *f)
{
	struct filestat_options *o = td->eo;			/* [한국어] filestat 엔진 옵션 구조체 — stat_type 포함. */
	struct timespec start;					/* [한국어] 시작 시각. */
	int do_lat = !td->o.disable_lat;			/* [한국어] 지연 기록 여부. */
	struct stat statbuf;					/* [한국어] stat/lstat 결과 버퍼(사용은 안 하지만 syscall이 기록 필요). */
#ifndef WIN32
	struct statx statxbuf;					/* [한국어] statx 결과 버퍼(Windows 제외). */
	char *abspath;						/* [한국어] realpath로 해석한 절대 경로(statx에 필요). */
#endif								/* [한국어] Windows에서는 statx 불가 — 분기. */
	int ret;						/* [한국어] stat 계열 반환값 — 0 성공 / -1 실패. */

	dprint(FD_FILE, "fd stat %s\n", f->file_name);		/* [한국어] 디버그 로그. */

	if (f->filetype != FIO_TYPE_FILE) {			/* [한국어] 정규 파일/디렉터리만 허용. */
		log_err("fio: only files are supported\n");
		return 1;
	}
	if (!strcmp(f->file_name, "-")) {			/* [한국어] 표준입출력 미지원. */
		log_err("fio: can't read/write to stdin/out\n");
		return 1;
	}

	if (do_lat)
		fio_gettime(&start, NULL);			/* [한국어] 조회 시작 시각. */

	switch (o->stat_type) {					/* [한국어] 옵션 값에 따라 stat 종류 분기. */
	case FIO_FILESTAT_STAT:
		ret = stat(f->file_name, &statbuf);		/* [한국어] stat(2) — 심볼릭 링크 추적. */
		break;
	case FIO_FILESTAT_LSTAT:
		ret = lstat(f->file_name, &statbuf);		/* [한국어] lstat(2) — 링크 자체 조회. */
		break;
	case FIO_FILESTAT_STATX:
#ifndef WIN32
		abspath = realpath(f->file_name, NULL);		/* [한국어] statx는 절대 경로를 요구 — NULL이면 glibc가 버퍼 malloc. */
		if (abspath) {
			ret = statx(-1, abspath, 0, STATX_ALL, &statxbuf);	/* [한국어] dirfd=-1(AT_FDCWD 아님, 절대경로 기반), flags=0, STATX_ALL로 모든 속성 요청. */
			free(abspath);				/* [한국어] realpath가 할당한 버퍼 해제 — 누수 방지. */
		} else
			ret = -1;				/* [한국어] realpath 실패 → stat 실패로 취급. */
#else
		ret = -1;					/* [한국어] Windows에서는 statx 미지원 — 에러 반환. */
#endif
		break;
	default:
		ret = -1;					/* [한국어] 알 수 없는 stat_type — 방어 경로. */
		break;
	}

	if (ret == -1) {					/* [한국어] 실패 시 에러 문자열 구성 후 td_verror. */
		char buf[FIO_VERROR_SIZE];
		int e = errno;					/* [한국어] errno 선보관. */

		snprintf(buf, sizeof(buf), "stat(%s) type=%u", f->file_name,
			o->stat_type);				/* [한국어] 어떤 stat 종류가 실패했는지 포함. */
		td_verror(td, e, buf);
		return 1;
	}

	if (do_lat) {
		struct fc_data *data = td->io_ops_data;		/* [한국어] 엔진 상태(stat_ddir) 획득. */
		uint64_t nsec;

		nsec = ntime_since_now(&start);			/* [한국어] 지연(ns) 계산. */
		add_clat_sample(td, data->stat_ddir, nsec, 0, NULL);	/* [한국어] clat 샘플 기록. */
	}

	return 0;
}

/*
 * [한국어]
 * delete_file - filedelete/dirdelete 엔진의 open_file 콜백. unlink/rmdir 1회 지연 측정.
 *
 * @td: td->io_ops_data에서 op_engine 구분.
 * @f:  삭제 대상.
 * @return: 0 성공 / 1 실패.
 *
 * 실행 컨텍스트: 잡 스레드, 파일별 1회.
 * caller: td_io_open_file → ops->open_file(= delete_file).
 * callee: unlink(2) / rmdir(2).
 * 주의: 엔진의 생명주기상 "open_file" 슬롯을 재사용해 실제로는 파일을 삭제하므로,
 *       close_file 콜백은 존재하지 않으며 호출 후 f->fd는 설정되지 않는다.
 *
 * 호출 체인: td_io_open_file → delete_file → unlink(2)/rmdir(2) → add_clat_sample.
 */
static int delete_file(struct thread_data *td, struct fio_file *f)
{
	struct timespec start;					/* [한국어] 삭제 시작 시각. */
	int do_lat = !td->o.disable_lat;			/* [한국어] 지연 측정 활성화 여부. */
	struct fc_data *fcd = td->io_ops_data;			/* [한국어] op_engine 분기를 위한 상태. */
	int ret;						/* [한국어] syscall 반환값. */

	dprint(FD_FILE, "fd delete %s\n", f->file_name);	/* [한국어] 디버그 로그. */

	if (f->filetype != FIO_TYPE_FILE) {			/* [한국어] 정규 파일/디렉터리만 지원. */
		log_err("fio: only files are supported\n");
		return 1;
	}
	if (!strcmp(f->file_name, "-")) {			/* [한국어] 표준입출력 미지원. */
		log_err("fio: can't read/write to stdin/out\n");
		return 1;
	}

	if (do_lat)
		fio_gettime(&start, NULL);			/* [한국어] 삭제 시작 시각 기록. */

	if (fcd->op_engine == FILE_OP_ENGINE)
		ret = unlink(f->file_name);			/* [한국어] 파일 엔진: unlink(2) — 링크 카운트 감소, 0이면 inode 해제. */
	else if (fcd->op_engine == DIR_OP_ENGINE)
		ret = rmdir(f->file_name);			/* [한국어] 디렉터리 엔진: rmdir(2) — 빈 디렉터리만 제거 가능. */
	else {
		log_err("fio: unknown file/directory operation engine\n");
		return 1;					/* [한국어] 비정상 op_engine 방어. */
	}

	if (ret == -1) {					/* [한국어] 삭제 실패 처리. */
		char buf[FIO_VERROR_SIZE];
		int e = errno;

		snprintf(buf, sizeof(buf), "delete(%s)", f->file_name);
		td_verror(td, e, buf);				/* [한국어] 에러를 잡 통계에 기록. */
		return 1;
	}

	if (do_lat) {
		struct fc_data *data = td->io_ops_data;
		uint64_t nsec;

		nsec = ntime_since_now(&start);			/* [한국어] 삭제 지연(ns). */
		add_clat_sample(td, data->stat_ddir, nsec, 0, NULL);	/* [한국어] clat 통계 누적. */
	}

	return 0;
}

/*
 * [한국어]
 * invalidate_do_nothing - 페이지 캐시 무효화 콜백을 no-op 처리.
 *
 * @td, @f: 사용 안 함.
 * @return: 항상 0.
 *
 * 왜 필요한가: fio는 순차 재읽기 편향을 피하려 open 후 posix_fadvise(DONTNEED) 같은
 * 방식으로 파일 캐시를 무효화한다. 그러나 filestat/filedelete/dirstat/dirdelete는
 * 파일을 "열지" 않으므로 무효화할 대상이 없고 기본 generic 경로가 에러를 낸다.
 * 이 콜백은 "아무것도 안 함"으로 그 호출을 안전하게 흘려보낸다.
 * 실행 컨텍스트: 잡 스레드, 매 파일 open 직후.
 *
 * 호출 체인: ioengines.c file_invalidate_cache → ops->invalidate → invalidate_do_nothing.
 */
static int invalidate_do_nothing(struct thread_data *td, struct fio_file *f)
{
	/* do nothing because file not opened */
	return 0;						/* [한국어] 무효화할 FD가 없으므로 즉시 성공 반환. */
}

/*
 * [한국어]
 * queue_io - fio I/O 큐잉 콜백. 본 엔진은 실제 I/O가 없으므로 즉시 완료를 반환한다.
 *
 * @td: 잡.
 * @io_u: fio가 제출한 I/O 유닛. ddir_sync이면 fsync류 요청.
 * @return: FIO_Q_COMPLETED — 동기 엔진 규약.
 *
 * 실행 컨텍스트: 잡 스레드의 I/O 루프.
 * 특이사항: FIO_SYNCFS 플래그가 걸린 엔진(filecreate/filedelete/dirdelete)은
 *           ddir_sync io_u(예: fsync)를 지원한다고 선언하므로 여기서 do_io_u_sync로
 *           실제 fsync를 수행할 수 있다.
 *
 * 호출 체인: td_io_queue → queue_io → (필요시) do_io_u_sync → fsync(2).
 */
static enum fio_q_status queue_io(struct thread_data *td, struct io_u *io_u)
{
	if (ddir_sync(io_u->ddir) && do_io_u_sync(td, io_u))	/* [한국어] sync 방향 I/O이면 fsync/fdatasync 수행 — 0이 아닌 반환은 실패. */
		io_u->error = errno;				/* [한국어] errno를 io_u에 기록 → fio가 통계에 반영. */
	return FIO_Q_COMPLETED;					/* [한국어] FAKEIO: 실제 데이터 I/O 없음 → 즉시 완료. */
}

/*
 * Ensure that we at least have a block size worth of IO to do for each
 * file. If the job file has td->o.size < nr_files * block_size, then
 * fio won't do anything.
 */
/*
 * [한국어]
 * get_file_size - 가짜 파일 크기를 설정해 fio가 각 파일에 최소 1블록 분량의 "I/O"를
 *                 배분하도록 한다. 실제 읽기/쓰기는 없지만, fio의 I/O 배분 로직은
 *                 파일 크기를 기반으로 io_u를 생성하므로 0이면 잡이 공회전한다.
 *
 * @td, @f: td_min_bs(td)로 블록 사이즈 질의.
 * @return: 0 성공.
 *
 * 실행 컨텍스트: 메인/잡 스레드 초기화.
 * 적용 엔진: filecreate/dircreate/dirdelete. filestat/filedelete는 기존 파일의 실제
 *            크기가 필요하므로 generic_get_file_size(stat 기반)을 사용한다.
 *
 * 호출 체인: setup_files → ops->get_file_size.
 */
static int get_file_size(struct thread_data *td, struct fio_file *f)
{
	f->real_file_size = td_min_bs(td);			/* [한국어] 잡의 최소 블록 크기를 "크기"로 위장 — io_u 1개 분량 확보. */
	return 0;
}

/*
 * [한국어]
 * init - 엔진별 fc_data 할당과 초기화. 엔진명 접두사로 op_engine 판별.
 *
 * @td: td->o.ioengine(이름), td_read/td_write(rw 방향) 사용.
 * @return: 항상 0 (calloc 실패 시 NULL 참조 리스크 있지만 원본 유지).
 *
 * 실행 컨텍스트: 잡 스레드 시작 직후(td_io_init).
 * 부작용: td->io_ops_data에 fc_data 포인터 저장 — cleanup에서 해제.
 *
 * 호출 체인: fio_backend → thread_main → td_io_init → ops->init.
 */
static int init(struct thread_data *td)
{
	struct fc_data *data;					/* [한국어] 엔진 상태 포인터. */

	data = calloc(1, sizeof(*data));			/* [한국어] 0으로 초기화된 상태 구조체 할당. */

	if (td_read(td))
		data->stat_ddir = DDIR_READ;			/* [한국어] rw가 read 계열이면 통계 버킷을 READ로. */
	else if (td_write(td))
		data->stat_ddir = DDIR_WRITE;			/* [한국어] write 계열이면 WRITE로 — trim/기타는 0(READ) 유지. */

	data->op_engine = UNKNOWN_OP_ENGINE;			/* [한국어] 기본값 — 아래에서 접두사 일치 시 갱신. */

	if (!strncmp(td->o.ioengine, "file", 4)) {		/* [한국어] "filecreate/filestat/filedelete" 접두사 체크. */
		data->op_engine = FILE_OP_ENGINE;
		dprint(FD_FILE, "Operate engine type: file\n");
	}
	if (!strncmp(td->o.ioengine, "dir", 3)) {		/* [한국어] "dircreate/dirstat/dirdelete" 접두사 체크. */
		data->op_engine = DIR_OP_ENGINE;
		dprint(FD_FILE, "Operate engine type: directory\n");
	}

	td->io_ops_data = data;					/* [한국어] 엔진 콜백들이 공통으로 접근할 수 있게 등록. */
	return 0;
}

/*
 * [한국어]
 * cleanup - init이 할당한 fc_data 해제. td_io_cleanup 단계에서 호출.
 * 실행 컨텍스트: 잡 스레드 종료 직전.
 * 호출 체인: thread_main 종료 경로 → td_io_cleanup → ops->cleanup.
 */
static void cleanup(struct thread_data *td)
{
	struct fc_data *data = td->io_ops_data;			/* [한국어] init이 저장한 포인터 획득. */

	free(data);						/* [한국어] calloc 쌍 해제. td->io_ops_data는 이후 접근하지 않음을 fio 프레임워크가 보장. */
}

/*
 * [한국어]
 * remove_dir - 디렉터리 엔진(dircreate/dirstat/dirdelete)의 unlink_file 콜백.
 *              fio의 정리 단계에서 테스트 디렉터리를 제거한다(rmdir 1회, 측정 대상 아님).
 *
 * @f: 제거할 디렉터리.
 * @return: rmdir의 반환값(성공 0, 실패 -1).
 *
 * 실행 컨텍스트: 잡 종료 시 fio의 unlink_file 경로.
 * 호출 체인: backend 정리 루프 → ops->unlink_file → rmdir(2).
 */
static int remove_dir(struct thread_data *td, struct fio_file *f)
{
	dprint(FD_FILE, "remove directory %s\n", f->file_name);	/* [한국어] 디버그: 어떤 디렉터리를 지우는지. */
	return rmdir(f->file_name);				/* [한국어] rmdir(2) 호출 — 빈 디렉터리만 성공. */
}

/*
 * [한국어] 엔진 정의: filecreate — open(O_CREAT)로 파일 생성 레이턴시 측정.
 * 플래그 해설:
 *   FIO_DISKLESSIO  : 블록 디바이스 필요 없음(검증 스킵).
 *   FIO_SYNCIO      : queue가 즉시 완료(비동기 커밋 없음).
 *   FIO_FAKEIO      : 실제 데이터 I/O 없음(바이트 카운터 집계 생략).
 *   FIO_SYNCFS      : ddir_sync(fsync) io_u를 처리할 수 있음.
 *   FIO_NOSTATS     : 기본 bw/iops 통계 비활성화(대신 clat만 수집).
 *   FIO_NOFILEHASH  : 파일명 해시 테이블에 넣지 않음(중복 open 허용).
 */
static struct ioengine_ops ioengine_filecreate = {
	.name		= "filecreate",				/* [한국어] fio --enghelp/잡파일에서 사용할 엔진 이름. */
	.version	= FIO_IOOPS_VERSION,			/* [한국어] 엔진 ABI 버전 — fio 본체와 일치해야 로드 가능. */
	.init		= init,					/* [한국어] fc_data 할당. */
	.cleanup	= cleanup,				/* [한국어] fc_data 해제. */
	.queue		= queue_io,				/* [한국어] FAKE queue — 즉시 완료. */
	.get_file_size	= get_file_size,			/* [한국어] 가짜 크기(td_min_bs) — io_u 배분용. */
	.open_file	= open_file,				/* [한국어] 실제 create 동작 + lat 측정. */
	.close_file	= generic_close_file,			/* [한국어] 표준 close(2) 경로. */
	.flags		= FIO_DISKLESSIO | FIO_SYNCIO | FIO_FAKEIO |
				FIO_SYNCFS | FIO_NOSTATS | FIO_NOFILEHASH,
};

/* [한국어] 엔진 정의: filestat — stat/lstat/statx 레이턴시. */
static struct ioengine_ops ioengine_filestat = {
	.name		= "filestat",				/* [한국어] 엔진 이름. */
	.version	= FIO_IOOPS_VERSION,			/* [한국어] ABI 버전. */
	.init		= init,					/* [한국어] 공용 init — stat_ddir/op_engine 결정. */
	.cleanup	= cleanup,				/* [한국어] 공용 cleanup. */
	.queue		= queue_io,				/* [한국어] FAKE queue. */
	.invalidate	= invalidate_do_nothing,		/* [한국어] stat은 FD가 없어 캐시 무효화 대상 없음. */
	.get_file_size	= generic_get_file_size,		/* [한국어] 실존 파일이므로 실제 크기를 stat으로 획득. */
	.open_file	= stat_file,				/* [한국어] open 대신 stat 호출 + lat 측정. */
	.flags		=  FIO_SYNCIO | FIO_FAKEIO |
				FIO_NOSTATS | FIO_NOFILEHASH,	/* [한국어] DISKLESSIO는 없음 — 실제 파일이 필요. */
	.options	= options,				/* [한국어] stat_type 옵션 노출. */
	.option_struct_size = sizeof(struct filestat_options),	/* [한국어] td->eo 할당 크기. */
};

/* [한국어] 엔진 정의: filedelete — unlink(2) 레이턴시. */
static struct ioengine_ops ioengine_filedelete = {
	.name		= "filedelete",				/* [한국어] 엔진 이름. */
	.version	= FIO_IOOPS_VERSION,
	.init		= init,
	.invalidate	= invalidate_do_nothing,		/* [한국어] 삭제는 FD가 없음. */
	.cleanup	= cleanup,
	.queue		= queue_io,
	.get_file_size	= generic_get_file_size,		/* [한국어] 실존 파일을 지우므로 실제 크기 기반. */
	.open_file	= delete_file,				/* [한국어] open 슬롯에 unlink 동작 매핑. */
	.flags		=  FIO_SYNCIO | FIO_FAKEIO |
				FIO_SYNCFS | FIO_NOSTATS | FIO_NOFILEHASH,
};

/* [한국어] 엔진 정의: dircreate — mkdir(2) 레이턴시. */
static struct ioengine_ops ioengine_dircreate = {
	.name		= "dircreate",
	.version	= FIO_IOOPS_VERSION,
	.init		= init,
	.cleanup	= cleanup,
	.queue		= queue_io,
	.get_file_size	= get_file_size,			/* [한국어] 가짜 크기 — 디렉터리는 size 개념이 다르기 때문. */
	.open_file	= open_file,				/* [한국어] 공용 open_file이 DIR 분기에서 mkdir 호출. */
	.close_file	= generic_close_file,
	.unlink_file    = remove_dir,				/* [한국어] 정리 시 rmdir. */
	.flags		= FIO_DISKLESSIO | FIO_SYNCIO | FIO_FAKEIO |
				FIO_NOSTATS | FIO_NOFILEHASH,
};

/* [한국어] 엔진 정의: dirstat — 디렉터리 stat 레이턴시. */
static struct ioengine_ops ioengine_dirstat = {
	.name		= "dirstat",
	.version	= FIO_IOOPS_VERSION,
	.setup		= setup_dirs,				/* [한국어] 측정 전 타겟 디렉터리 선-생성. */
	.init		= init,
	.cleanup	= cleanup,
	.queue		= queue_io,
	.invalidate	= invalidate_do_nothing,
	.get_file_size	= generic_get_file_size,
	.open_file	= stat_file,				/* [한국어] 공용 stat_file — stat_type에 따라 stat/lstat/statx. */
	.unlink_file	= remove_dir,				/* [한국어] 정리 시 rmdir. */
	.flags		=  FIO_DISKLESSIO | FIO_SYNCIO | FIO_FAKEIO |
				FIO_NOSTATS | FIO_NOFILEHASH,
	.options	= options,				/* [한국어] stat_type 공유. */
	.option_struct_size = sizeof(struct filestat_options),
};

/* [한국어] 엔진 정의: dirdelete — rmdir(2) 레이턴시. */
static struct ioengine_ops ioengine_dirdelete = {
	.name		= "dirdelete",
	.version	= FIO_IOOPS_VERSION,
	.setup		= setup_dirs,				/* [한국어] 지울 대상 디렉터리를 먼저 생성. */
	.init		= init,
	.invalidate	= invalidate_do_nothing,
	.cleanup	= cleanup,
	.queue		= queue_io,
	.get_file_size	= get_file_size,			/* [한국어] 가짜 크기. */
	.open_file	= delete_file,				/* [한국어] open 슬롯에 rmdir 매핑(공용 delete_file이 DIR 분기 수행). */
	.unlink_file	= remove_dir,				/* [한국어] fio 정리 경로의 안전망. */
	.flags		= FIO_DISKLESSIO | FIO_SYNCIO | FIO_FAKEIO |
				FIO_SYNCFS | FIO_NOSTATS | FIO_NOFILEHASH,
};

/*
 * [한국어]
 * fio_fileoperations_register - 공유 라이브러리 로드 시점에 6개 엔진을 fio 엔진
 *                                레지스트리에 등록. fio_init 매크로는 GCC constructor
 *                                속성(또는 수동 콜러)으로 main 이전에 실행됨을 보장.
 * 실행 컨텍스트: 프로세스 시작 시 1회, 메인 스레드.
 * 호출 체인: dlopen/프로세스 시작 → fio_init 생성자 → register_ioengine × 6.
 */
static void fio_init fio_fileoperations_register(void)
{
	register_ioengine(&ioengine_filecreate);	/* [한국어] filecreate 등록. */
	register_ioengine(&ioengine_filestat);		/* [한국어] filestat 등록. */
	register_ioengine(&ioengine_filedelete);	/* [한국어] filedelete 등록. */
	register_ioengine(&ioengine_dircreate);		/* [한국어] dircreate 등록. */
	register_ioengine(&ioengine_dirstat);		/* [한국어] dirstat 등록. */
	register_ioengine(&ioengine_dirdelete);		/* [한국어] dirdelete 등록. */
}

/*
 * [한국어]
 * fio_fileoperations_unregister - 프로세스 종료 시점(destructor)에 엔진을 해제.
 * 실행 컨텍스트: exit 훅.
 * 호출 체인: atexit/destructor → unregister_ioengine × 6.
 */
static void fio_exit fio_fileoperations_unregister(void)
{
	unregister_ioengine(&ioengine_filecreate);	/* [한국어] filecreate 해제. */
	unregister_ioengine(&ioengine_filestat);	/* [한국어] filestat 해제. */
	unregister_ioengine(&ioengine_filedelete);	/* [한국어] filedelete 해제. */
	unregister_ioengine(&ioengine_dircreate);	/* [한국어] dircreate 해제. */
	unregister_ioengine(&ioengine_dirstat);		/* [한국어] dirstat 해제. */
	unregister_ioengine(&ioengine_dirdelete);	/* [한국어] dirdelete 해제. */
}
