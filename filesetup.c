/*
 * [한국어 설명] fio 파일/디바이스 셋업 모듈 (filesetup.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio가 I/O 테스트를 시작하기 전에 파일/디바이스를 준비하는 모든 로직을
 * 구현한다. 파일 크기 확인, 생성/확장, I/O 범위 계산, 사전 읽기(캐시 워밍),
 * 파일 열기/닫기, 랜덤 맵 초기화 등을 담당한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * backend.c의 thread_main()에서 setup_files()를 호출하여 파일을 준비한 뒤,
 * do_io()에서 실제 I/O를 수행한다.
 * 호출 체인: thread_main() [backend.c] → setup_files() [이 파일]
 *           → get_file_sizes() → extend_file() → init_random_map()
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: thread_main()에서 setup_files()/init_random_map() 호출
 * - ioengines.c: td_io_open_file()/td_io_close_file()이 generic_open/close_file() 호출
 * - filehash.c: 파일 해시 테이블로 동일 파일 공유 관리
 * - file.h: fio_file 구조체 정의
 * - 핵심 자료구조: fio_file(파일 정보), thread_data(스레드 상태)
 *
 * === 주요 함수/구조체 요약 ===
 * - setup_files(): 파일 셋업 메인 함수 (크기 확인, 생성/확장, 범위 계산)
 * - generic_open_file(): I/O 엔진의 기본 파일 열기 구현
 * - generic_close_file(): I/O 엔진의 기본 파일 닫기 구현
 * - extend_file(): 파일을 필요한 크기까지 확장
 * - init_random_map(): 랜덤 I/O를 위한 블록 맵/LFSR 초기화
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <unistd.h>      /* POSIX API (read, write, close, ftruncate 등) */
#include <fcntl.h>       /* 파일 제어 (open 플래그: O_RDWR, O_CREAT 등) */
#include <string.h>      /* 문자열 처리 (strcmp, strlen 등) */
#include <assert.h>      /* 디버그 단언문 */
#include <dirent.h>      /* 디렉토리 탐색 (opendir, readdir) */
#include <libgen.h>      /* 경로 처리 (dirname, basename) */
#include <sys/stat.h>    /* 파일 상태 조회 (stat, S_ISBLK 등) */

/* fio 내부 헤더 파일들 */
#include "fio.h"           /* fio 핵심 구조체 및 매크로 */
#include "smalloc.h"       /* 공유 메모리 할당기 (프로세스 간 공유 데이터) */
#include "filehash.h"      /* 파일 해시 테이블 (동일 파일 공유 관리) */
#include "options.h"       /* 옵션 처리 */
#include "os/os.h"         /* OS 추상화 레이어 */
#include "hash.h"          /* 해시 함수 (jhash 등) */
#include "lib/axmap.h"     /* 비트맵 기반 블록 맵 (랜덤 I/O 추적) */
#include "rwlock.h"        /* 읽기/쓰기 잠금 */
#include "zbd.h"           /* ZBD(Zoned Block Device) 지원 */
#include "sprandom.h"      /* SP(Structured Placement) 랜덤 */

#ifdef CONFIG_LINUX_FALLOCATE
#include <linux/falloc.h>  /* Linux fallocate 플래그 (FALLOC_FL_KEEP_SIZE 등) */
#endif

/* [한국어] 전역 파일명 리스트 - 중복 파일 생성 방지를 위한 이미 할당된 파일명 목록 */
static FLIST_HEAD(filename_list);

/*
 * List entry for filename_list
 */
/* [한국어] 파일명 리스트 항목 - 파일명과 리스트 노드를 묶는 구조체 */
struct file_name {
	struct flist_head list;   /* 연결 리스트 노드 */
	char *filename;           /* 파일 경로명 */
};

/* [한국어] 스레드 에러 상태 초기화 - error 코드와 에러 메시지를 클리어 */
static inline void clear_error(struct thread_data *td)
{
	td->error = 0;
	td->verror[0] = '\0';
}

/*
 * [한국어] OS 네이티브 fallocate 수행
 * 파일에 대해 OS 고유의 fallocate를 호출하여 디스크 공간을 사전 할당한다.
 * 성공하면 false(0), 실패하면 true(1)를 반환한다.
 */
static int native_fallocate(struct thread_data *td, struct fio_file *f)
{
	bool success;

	success = fio_fallocate(f, 0, f->real_file_size);
	dprint(FD_FILE, "native fallocate of file %s size %llu was "
			"%ssuccessful\n", f->file_name,
			(unsigned long long) f->real_file_size,
			!success ? "un": "");

	if (success)
		return false;

	if (errno == ENOSYS)
		dprint(FD_FILE, "native fallocate is not implemented\n");

	return true;
}

/*
 * [한국어] fallocate 모드에 따라 파일 공간을 사전 할당하는 함수
 *
 * td->o.fallocate_mode에 따라 다른 방식으로 파일 공간을 할당:
 *   - NATIVE: OS 네이티브 fallocate
 *   - NONE: 아무것도 안 함
 *   - POSIX: posix_fallocate() 호출
 *   - KEEP_SIZE: fallocate(FALLOC_FL_KEEP_SIZE) - 파일 크기 변경 없이 공간 확보
 *   - TRUNCATE: ftruncate()로 크기 조정
 *
 * fill_device 모드에서는 호출하지 않음 (디바이스를 가득 채울 것이므로)
 */
static void fallocate_file(struct thread_data *td, struct fio_file *f)
{
	if (td->o.fill_device)
		return;

	switch (td->o.fallocate_mode) {
	case FIO_FALLOCATE_NATIVE:
		native_fallocate(td, f);
		break;
	case FIO_FALLOCATE_NONE:
		break;
#ifdef CONFIG_POSIX_FALLOCATE
	case FIO_FALLOCATE_POSIX: {
		int r;

		dprint(FD_FILE, "posix_fallocate file %s size %llu\n",
				 f->file_name,
				 (unsigned long long) f->real_file_size);

		r = posix_fallocate(f->fd, 0, f->real_file_size);
		if (r > 0)
			log_err("fio: posix_fallocate fails: %s\n", strerror(r));
		break;
		}
#endif /* CONFIG_POSIX_FALLOCATE */
#ifdef CONFIG_LINUX_FALLOCATE
	case FIO_FALLOCATE_KEEP_SIZE: {
		int r;

		dprint(FD_FILE, "fallocate(FALLOC_FL_KEEP_SIZE) "
				"file %s size %llu\n", f->file_name,
				(unsigned long long) f->real_file_size);

		r = fallocate(f->fd, FALLOC_FL_KEEP_SIZE, 0, f->real_file_size);
		if (r != 0)
			td_verror(td, errno, "fallocate");

		break;
		}
#endif /* CONFIG_LINUX_FALLOCATE */
	case FIO_FALLOCATE_TRUNCATE: {
		int r;

		dprint(FD_FILE, "ftruncate file %s size %llu\n",
				f->file_name,
				(unsigned long long) f->real_file_size);
		r = ftruncate(f->fd, f->real_file_size);
		if (r != 0)
			td_verror(td, errno, "ftruncate");

		break;
	}
	default:
		log_err("fio: unknown fallocate mode: %d\n", td->o.fallocate_mode);
		assert(0);
	}
}

/*
 * Leaves f->fd open on success, caller must close
 */
/*
 * [한국어] 파일을 필요한 크기까지 확장하는 함수
 *
 * 읽기 작업이나 overwrite 모드의 쓰기 작업에서는 파일에 실제 데이터가
 * 존재해야 하므로, 이 함수가 파일을 생성하고 데이터를 채운다.
 *
 * 동작 흐름:
 *   1) 읽기 작업 또는 overwrite 쓰기면 new_layout=1 (전체 재배치 필요)
 *   2) 기존 파일 unlink 후 새로 생성 (필요한 경우)
 *   3) fallocate로 공간 사전 할당
 *   4) new_layout이면 ftruncate + write 루프로 데이터 채우기
 *   5) fill_device 모드면 ENOSPC까지 계속 쓰기
 *
 * 성공 시 f->fd가 열린 상태로 반환 (호출자가 닫아야 함)
 * 반환값: 0=성공, 1=실패
 */
static int extend_file(struct thread_data *td, struct fio_file *f)
{
	int new_layout = 0, unlink_file = 0, flags;
	unsigned long long left;
	unsigned long long bs;
	char *b = NULL;

	/* [한국어] 읽기 전용 모드에서는 파일 확장 거부 */
	if (read_only) {
		log_err("fio: refusing extend of file due to read-only\n");
		return 0;
	}

	/*
	 * check if we need to lay the file out complete again. fio
	 * does that for operations involving reads, or for writes
	 * where overwrite is set
	 */
	/* [한국어] 파일 전체를 다시 배치해야 하는지 확인
	 * - 읽기 작업: 읽을 데이터가 있어야 하므로 전체 배치 필요
	 * - overwrite 쓰기: 기존 데이터 위에 덮어쓰므로 전체 배치 필요
	 * - FIO_NOEXTEND 엔진: 파일 확장을 지원하지 않는 엔진 */
	if (td_read(td) ||
	   (td_write(td) && td->o.overwrite && !td->o.file_append) ||
	    (td_write(td) && td_ioengine_flagged(td, FIO_NOEXTEND)))
		new_layout = 1;
	/* [한국어] overwrite가 아닌 쓰기면 기존 파일 삭제 후 새로 생성 */
	if (td_write(td) && !td->o.overwrite && !td->o.file_append)
		unlink_file = 1;

	/* [한국어] 기존 파일 삭제 (새 레이아웃이 필요하거나 unlink가 필요한 경우) */
	if (unlink_file || new_layout) {
		int ret;

		dprint(FD_FILE, "layout unlink %s\n", f->file_name);

		ret = td_io_unlink_file(td, f);
		if (ret != 0 && ret != ENOENT) {
			td_verror(td, errno, "unlink");
			return 1;
		}
	}

	/* [한국어] 파일 열기 플래그 설정 */
	flags = O_WRONLY;
	if (td->o.allow_create)
		flags |= O_CREAT;       /* 파일 생성 허용 */
	if (new_layout)
		flags |= O_TRUNC;       /* 새 레이아웃이면 기존 내용 잘라냄 */

#ifdef WIN32
	flags |= _O_BINARY;
#endif

	dprint(FD_FILE, "open file %s, flags %x\n", f->file_name, flags);
	f->fd = open(f->file_name, flags, 0644);
	if (f->fd < 0) {
		int err = errno;

		if (err == ENOENT && !td->o.allow_create)
			log_err("fio: file creation disallowed by "
					"allow_file_create=0\n");
		else
			td_verror(td, err, "open");
		return 1;
	}

	/* [한국어] fallocate 모드에 따라 디스크 공간 사전 할당 */
	fallocate_file(td, f);

	/*
	 * If our jobs don't require regular files initially, we're done.
	 */
	/* [한국어] 전체 재배치가 필요 없으면 여기서 종료 (파일만 열어두면 충분) */
	if (!new_layout)
		goto done;

	/*
	 * The size will be -1ULL when fill_device is used, so don't truncate
	 * or fallocate this file, just write it
	 */
	/* [한국어] fill_device 모드가 아닐 때만 ftruncate로 파일 크기 설정
	 * fill_device 모드에서는 크기가 -1ULL이므로 truncate하지 않고 그냥 쓴다 */
	if (!td->o.fill_device) {
		dprint(FD_FILE, "truncate file %s, size %llu\n", f->file_name,
					(unsigned long long) f->real_file_size);
		if (ftruncate(f->fd, f->real_file_size) == -1) {
			if (errno != EFBIG) {
				td_verror(td, errno, "ftruncate");
				goto err;
			}
		}
	}

	/* [한국어] 쓰기 루프: 파일에 실제 데이터를 채움
	 * max_bs[WRITE] 크기의 블록 단위로 반복 쓰기 */
	left = f->real_file_size;
	bs = td->o.max_bs[DDIR_WRITE];
	if (bs > left)
		bs = left;

	b = malloc(bs);
	if (!b) {
		td_verror(td, errno, "malloc");
		goto err;
	}

	while (left && !td->terminate) {
		ssize_t r;

		if (bs > left)
			bs = left;

		/* [한국어] I/O 버퍼를 패턴 데이터로 채움 (verify 옵션에 따라 다름) */
		fill_io_buffer(td, b, bs, bs);

		r = write(f->fd, b, bs);

		if (r > 0) {
			left -= r;
			continue;
		} else {
			if (r < 0) {
				int __e = errno;

				/* [한국어] ENOSPC/EDQUOT: 디스크 풀 또는 쿼터 초과
				 * fill_device 모드면 정상 종료, 아니면 에러 */
				if (__e == ENOSPC || __e == EDQUOT) {
					const char *__e_name;
					if (td->o.fill_device)
						break;
					if (__e == ENOSPC)
						__e_name = "ENOSPC";
					else
						__e_name = "EDQUOT";
					log_info("fio: %s on laying out "
						 "file, stopping\n", __e_name);
				}
				td_verror(td, errno, "write");
			} else
				td_verror(td, EIO, "write");

			goto err;
		}
	}

	/* [한국어] 종료 처리: 중단되었으면 파일 삭제, 아니면 fsync 수행 */
	if (td->terminate) {
		dprint(FD_FILE, "terminate unlink %s\n", f->file_name);
		td_io_unlink_file(td, f);
	} else if (td->o.create_fsync) {
		if (fsync(f->fd) < 0) {
			td_verror(td, errno, "fsync");
			goto err;
		}
	}
	/* [한국어] fill_device + 읽기 전용 작업: 실제 기록된 크기로 파일 크기 재설정 */
	if (td->o.fill_device && !td_write(td)) {
		fio_file_clear_size_known(f);
		if (td_io_get_file_size(td, f))
			goto err;
		if (f->io_size > f->real_file_size)
			f->io_size = f->real_file_size;
	}

	free(b);
done:
	return 0;
err:
	close(f->fd);
	f->fd = -1;
	if (b)
		free(b);
	return 1;
}

/*
 * [한국어] 파일 사전 읽기 함수 - 페이지 캐시 워밍 (pre-reading)
 *
 * I/O 테스트 시작 전에 파일을 미리 읽어서 페이지 캐시에 올려둔다.
 * 이는 "콜드 캐시" 상태에서의 성능 편차를 제거하기 위해 사용된다.
 * 파이프/NoIO 엔진이나 캐릭터 디바이스에서는 스킵한다.
 *
 * 반환값: true=성공, false=실패
 */
static bool pre_read_file(struct thread_data *td, struct fio_file *f)
{
	int r, did_open = 0, old_runstate;
	unsigned long long left;
	unsigned long long bs;
	bool ret = true;
	char *b;

	/* [한국어] 파이프 I/O나 NoIO 엔진에서는 사전 읽기 불필요 */
	if (td_ioengine_flagged(td, FIO_PIPEIO) ||
	    td_ioengine_flagged(td, FIO_NOIO))
		return true;

	/* [한국어] 캐릭터 디바이스는 사전 읽기 불가 */
	if (f->filetype == FIO_TYPE_CHAR)
		return true;

	/* [한국어] 파일이 아직 열려있지 않으면 직접 열기 */
	if (!fio_file_open(f)) {
		if (td->io_ops->open_file(td, f)) {
			log_err("fio: cannot pre-read, failed to open file\n");
			return false;
		}
		did_open = 1;
	}

	/* [한국어] 실행 상태를 TD_PRE_READING으로 변경 (통계/모니터링용) */
	old_runstate = td_bump_runstate(td, TD_PRE_READING);

	left = f->io_size;
	bs = td->o.max_bs[DDIR_READ];
	if (bs > left)
		bs = left;

	b = calloc(1, bs);
	if (!b) {
		td_verror(td, errno, "malloc");
		ret = false;
		goto error;
	}

	/* [한국어] 파일 오프셋 위치로 이동 */
	if (lseek(f->fd, f->file_offset, SEEK_SET) < 0) {
		td_verror(td, errno, "lseek");
		log_err("fio: failed to lseek pre-read file\n");
		ret = false;
		goto error;
	}

	/* [한국어] 파일 전체를 순차적으로 읽어서 캐시에 올림 */
	while (left && !td->terminate) {
		if (bs > left)
			bs = left;

		r = read(f->fd, b, bs);

		if (r == (int) bs) {
			left -= bs;
			continue;
		} else {
			td_verror(td, EIO, "pre_read");
			break;
		}
	}

error:
	/* [한국어] 실행 상태 복원 */
	td_restore_runstate(td, old_runstate);

	/* [한국어] 이 함수에서 열었으면 닫기 */
	if (did_open)
		td->io_ops->close_file(td, f);

	free(b);
	return ret;
}

/*
 * Generic function to prepopulate regular file with data.
 * Useful if you want to make sure I/O engine has data to read.
 * Leaves f->fd open on success, caller must close.
 */
/*
 * [한국어] 일반 파일에 데이터를 사전 채우는 범용 함수
 *
 * I/O 엔진이 읽을 "실제" 데이터가 파일에 필요할 때 사용한다.
 * extend_file()과 유사하지만, 이미 존재하는 파일에 데이터만 채우는 용도이며
 * I/O 엔진의 prepopulate_file 콜백으로 사용될 수 있다.
 *
 * 성공 시 f->fd가 열린 상태로 반환 (호출자가 닫아야 함)
 * 반환값: 0=성공, 1=실패
 */
int generic_prepopulate_file(struct thread_data *td, struct fio_file *f)
{
	int flags;
	unsigned long long left, bs;
	char *b = NULL;

	/* generic function for regular files only */
	/* [한국어] 일반 파일에서만 사용 가능 */
	assert(f->filetype == FIO_TYPE_FILE);

	if (read_only) {
		log_err("fio: refusing to write a file due to read-only\n");
		return 0;
	}

	flags = O_WRONLY;
	if (td->o.allow_create)
		flags |= O_CREAT;

#ifdef WIN32
	flags |= _O_BINARY;
#endif

	dprint(FD_FILE, "open file %s, flags %x\n", f->file_name, flags);
	f->fd = open(f->file_name, flags, 0644);
	if (f->fd < 0) {
		int err = errno;

		if (err == ENOENT && !td->o.allow_create)
			log_err("fio: file creation disallowed by "
					"allow_file_create=0\n");
		else
			td_verror(td, err, "open");
		return 1;
	}

	/* [한국어] 파일 전체 크기만큼 데이터를 쓰기 */
	left = f->real_file_size;
	bs = td->o.max_bs[DDIR_WRITE];
	if (bs > left)
		bs = left;

	b = malloc(bs);
	if (!b) {
		td_verror(td, errno, "malloc");
		goto err;
	}

	while (left && !td->terminate) {
		ssize_t r;

		if (bs > left)
			bs = left;

		fill_io_buffer(td, b, bs, bs);

		r = write(f->fd, b, bs);

		if (r > 0) {
			left -= r;
		} else {
			td_verror(td, errno, "write");
			goto err;
		}
	}

	/* [한국어] 종료 처리: 중단되었으면 파일 삭제, 아니면 fsync */
	if (td->terminate) {
		dprint(FD_FILE, "terminate unlink %s\n", f->file_name);
		td_io_unlink_file(td, f);
	} else if (td->o.create_fsync) {
		if (fsync(f->fd) < 0) {
			td_verror(td, errno, "fsync");
			goto err;
		}
	}

	free(b);
	return 0;
err:
	close(f->fd);
	f->fd = -1;
	if (b)
		free(b);
	return 1;
}

/*
 * [한국어] 랜덤 파일 크기 생성
 *
 * file_size_low ~ file_size_high 범위에서 랜덤 크기를 생성한다.
 * rw_min_bs 단위로 정렬되어 반환된다.
 */
unsigned long long get_rand_file_size(struct thread_data *td)
{
	unsigned long long ret, sized;
	uint64_t frand_max;
	uint64_t r;

	frand_max = rand_max(&td->file_size_state);
	r = __rand(&td->file_size_state);
	sized = td->o.file_size_high - td->o.file_size_low;  /* 크기 범위 */
	ret = (unsigned long long) ((double) sized * (r / (frand_max + 1.0)));
	ret += td->o.file_size_low;          /* 최소 크기 더하기 */
	ret -= (ret % td->o.rw_min_bs);      /* 최소 블록 크기로 정렬 */
	return ret;
}

/*
 * [한국어] 일반 파일의 크기를 stat()으로 조회
 * f->real_file_size에 실제 파일 크기를 설정한다.
 */
static int file_size(struct thread_data *td, struct fio_file *f)
{
	struct stat st;

	if (stat(f->file_name, &st) == -1) {
		td_verror(td, errno, "fstat");
		return 1;
	}

	f->real_file_size = st.st_size;
	return 0;
}

/*
 * [한국어] 블록 디바이스 크기 조회
 * 디바이스를 열어서 ioctl로 크기를 얻은 후 닫는다.
 */
static int bdev_size(struct thread_data *td, struct fio_file *f)
{
	unsigned long long bytes = 0;
	int r;

	if (td->io_ops->open_file(td, f)) {
		log_err("fio: failed opening blockdev %s for size check\n",
			f->file_name);
		return 1;
	}

	r = blockdev_size(f, &bytes);
	if (r) {
		td_verror(td, r, "blockdev_size");
		goto err;
	}

	if (!bytes) {
		log_err("%s: zero sized block device?\n", f->file_name);
		goto err;
	}

	f->real_file_size = bytes;
	td->io_ops->close_file(td, f);
	return 0;
err:
	td->io_ops->close_file(td, f);
	return 1;
}

/*
 * [한국어] 캐릭터 디바이스 크기 조회
 * FIO_HAVE_CHARDEV_SIZE가 정의된 플랫폼에서만 크기를 조회하며,
 * 미지원 플랫폼에서는 -1ULL(무한 크기)을 설정한다.
 */
static int char_size(struct thread_data *td, struct fio_file *f)
{
#ifdef FIO_HAVE_CHARDEV_SIZE
	unsigned long long bytes = 0;
	int r;

	if (td->io_ops->open_file(td, f)) {
		log_err("fio: failed opening chardev %s for size check\n",
			f->file_name);
		return 1;
	}

	r = chardev_size(f, &bytes);
	if (r) {
		td_verror(td, r, "chardev_size");
		goto err;
	}

	if (!bytes) {
		log_err("%s: zero sized char device?\n", f->file_name);
		goto err;
	}

	f->real_file_size = bytes;
	td->io_ops->close_file(td, f);
	return 0;
err:
	td->io_ops->close_file(td, f);
	return 1;
#else
	/* [한국어] 캐릭터 디바이스 크기 조회 미지원 - 무한 크기로 설정 */
	f->real_file_size = -1ULL;
	return 0;
#endif
}

/*
 * [한국어] 파일 유형에 따라 적절한 크기 조회 함수를 호출
 *
 * 이미 크기가 알려져 있으면 바로 반환한다.
 * 파일 유형별로 file_size/bdev_size/char_size를 호출하고,
 * 성공하면 size_known 플래그를 설정한다.
 */
static int get_file_size(struct thread_data *td, struct fio_file *f)
{
	int ret = 0;

	/* [한국어] 이미 크기를 알고 있으면 스킵 */
	if (fio_file_size_known(f))
		return 0;

	/* [한국어] 파일 유형에 따라 적절한 크기 조회 함수 호출 */
	if (f->filetype == FIO_TYPE_FILE)
		ret = file_size(td, f);
	else if (f->filetype == FIO_TYPE_BLOCK)
		ret = bdev_size(td, f);
	else if (f->filetype == FIO_TYPE_CHAR)
		ret = char_size(td, f);
	else {
		f->real_file_size = -1;
		log_info("%s: failed to get file size of %s\n", td->o.name,
					f->file_name);
		return 1; /* avoid offset extends end error message */
	}

	/*
	 * Leave ->real_file_size with 0 since it could be expectation
	 * of initial setup for regular files.
	 */
	if (ret)
		return ret;

	/*
	 * ->file_offset normally hasn't been initialized yet, so this
	 * is basically always false unless ->real_file_size is -1, but
	 * if ->real_file_size is -1 this message doesn't make sense.
	 * As a result, this message is basically useless.
	 */
	/* [한국어] 오프셋이 파일 크기를 초과하는지 검증 (대부분 여기에 걸리지 않음) */
	if (f->file_offset > f->real_file_size) {
		log_err("%s: offset extends end (%llu > %llu)\n", td->o.name,
					(unsigned long long) f->file_offset,
					(unsigned long long) f->real_file_size);
		return 1;
	}

	fio_file_set_size_known(f);
	return 0;
}

/*
 * [한국어] 파일 캐시 무효화 내부 함수
 *
 * I/O 엔진, 파일 유형에 따라 다양한 방법으로 페이지 캐시를 무효화:
 *   - I/O 엔진에 invalidate 콜백이 있으면 그것을 호출
 *   - 일반 파일: posix_fadvise(POSIX_FADV_DONTNEED)로 캐시 힌트
 *   - 블록 디바이스: blockdev_invalidate_cache()로 ioctl 호출
 *   - 캐릭터 디바이스/파이프: 캐시 무효화 미지원
 *
 * 캐시 무효화 실패는 치명적이지 않으며 경고만 출력한다.
 */
static int __file_invalidate_cache(struct thread_data *td, struct fio_file *f,
				   unsigned long long off,
				   unsigned long long len)
{
	int errval = 0, ret = 0;

#ifdef CONFIG_ESX
	return 0;
#endif

	/* [한국어] 기본값 처리: -1ULL이면 파일의 I/O 범위 전체를 대상으로 함 */
	if (len == -1ULL)
		len = f->io_size;
	if (off == -1ULL)
		off = f->file_offset;

	if (len == -1ULL || off == -1ULL)
		return 0;

	if (td->io_ops->invalidate) {
		/* [한국어] I/O 엔진 자체 캐시 무효화 콜백 호출 */
		dprint(FD_IO, "invalidate %s cache %s\n", td->io_ops->name,
			f->file_name);
		ret = td->io_ops->invalidate(td, f);
		if (ret < 0)
			errval = -ret;
	} else if (td_ioengine_flagged(td, FIO_DISKLESSIO)) {
		dprint(FD_IO, "invalidate not supported by ioengine %s\n",
		       td->io_ops->name);
	} else if (f->filetype == FIO_TYPE_FILE) {
		/* [한국어] 일반 파일: POSIX_FADV_DONTNEED로 페이지 캐시 해제 요청 */
		dprint(FD_IO, "declare unneeded cache %s: %llu/%llu\n",
			f->file_name, off, len);
		ret = posix_fadvise(f->fd, off, len, POSIX_FADV_DONTNEED);
		if (ret)
			errval = ret;
	} else if (f->filetype == FIO_TYPE_BLOCK) {
		int retry_count = 0;

		/* [한국어] 블록 디바이스: ioctl로 페이지 캐시 드롭 */
		dprint(FD_IO, "drop page cache %s\n", f->file_name);
		ret = blockdev_invalidate_cache(f);
		while (ret < 0 && errno == EAGAIN && retry_count++ < 25) {
			/*
			 * Linux multipath devices reject ioctl while
			 * the maps are being updated. That window can
			 * last tens of milliseconds; we'll try up to
			 * a quarter of a second.
			 */
			/* [한국어] Linux 멀티패스 디바이스는 맵 업데이트 중 ioctl을 거부할 수 있어
			 * EAGAIN이면 10ms 간격으로 최대 25회(250ms) 재시도 */
			usleep(10000);
			ret = blockdev_invalidate_cache(f);
		}
		if (ret < 0 && errno == EACCES && geteuid()) {
			if (!fio_did_warn(FIO_WARN_ROOT_FLUSH)) {
				log_err("fio: only root may flush block "
					"devices. Cache flush bypassed!\n");
			}
		}
		if (ret < 0)
			errval = errno;
	} else if (f->filetype == FIO_TYPE_CHAR ||
		   f->filetype == FIO_TYPE_PIPE) {
		dprint(FD_IO, "invalidate not supported %s\n", f->file_name);
	}

	/*
	 * Cache flushing isn't a fatal condition, and we know it will
	 * happen on some platforms where we don't have the proper
	 * function to flush eg block device caches. So just warn and
	 * continue on our way.
	 */
	/* [한국어] 캐시 무효화 실패는 치명적이지 않음 - 경고만 출력 후 계속 */
	if (errval)
		log_info("fio: cache invalidation of %s failed: %s\n",
			 f->file_name, strerror(errval));

	return 0;

}

/*
 * [한국어] 파일 캐시 무효화 공개 인터페이스
 * 파일이 열려 있을 때만 전체 I/O 범위에 대해 캐시 무효화를 수행한다.
 */
int file_invalidate_cache(struct thread_data *td, struct fio_file *f)
{
	if (!fio_file_open(f))
		return 0;

	return __file_invalidate_cache(td, f, -1ULL, -1ULL);
}

/*
 * [한국어] 범용 파일 닫기 함수 - 대부분의 I/O 엔진이 사용하는 기본 close_file 구현
 *
 * 해시 테이블에서 파일을 제거하고, fd와 shadow_fd를 모두 닫는다.
 * engine_pos도 0으로 초기화한다.
 */
int generic_close_file(struct thread_data fio_unused *td, struct fio_file *f)
{
	int ret = 0;

	dprint(FD_FILE, "fd close %s\n", f->file_name);

	/* [한국어] 해시 테이블에서 파일 항목 제거 */
	remove_file_hash(f);

	if (close(f->fd) < 0)
		ret = errno;

	f->fd = -1;

	/* [한국어] 그림자 fd도 함께 닫기 */
	if (f->shadow_fd != -1) {
		close(f->shadow_fd);
		f->shadow_fd = -1;
	}

	f->engine_pos = 0;
	return ret;
}

/*
 * [한국어] 해시 테이블에서 파일을 조회하고 열기
 *
 * 같은 파일이 해시 테이블에 있으면 잠금(lock)을 공유한다.
 * 반환값: 1=해시에서 찾음, 0=새로 열음
 */
int file_lookup_open(struct fio_file *f, int flags)
{
	struct fio_file *__f;
	int from_hash;

	/* [한국어] 파일 해시에서 같은 이름의 기존 파일 조회 */
	__f = lookup_file_hash(f->file_name);
	if (__f) {
		dprint(FD_FILE, "found file in hash %s\n", f->file_name);
		f->lock = __f->lock;  /* 기존 파일의 잠금 공유 */
		from_hash = 1;
	} else {
		dprint(FD_FILE, "file not found in hash %s\n", f->file_name);
		from_hash = 0;
	}

#ifdef WIN32
	flags |= _O_BINARY;
#endif

	f->fd = open(f->file_name, flags, 0600);
	return from_hash;
}

/*
 * [한국어] 모든 파일의 그림자 fd(shadow_fd) 닫기
 *
 * 파일 디스크립터가 부족할 때(EMFILE) 호출되어 그림자 fd를 회수한다.
 * 반환값: 닫은 그림자 fd 수
 */
static int file_close_shadow_fds(struct thread_data *td)
{
	struct fio_file *f;
	int num_closed = 0;
	unsigned int i;

	for_each_file(td, f, i) {
		if (f->shadow_fd == -1)
			continue;

		close(f->shadow_fd);
		f->shadow_fd = -1;
		num_closed++;
	}

	return num_closed;
}

/*
 * [한국어] 범용 파일 열기 함수 - 대부분의 I/O 엔진이 사용하는 기본 open_file 구현
 *
 * 파일명이 "-"이면 stdin/stdout으로 처리한다.
 * I/O 방향(읽기/쓰기/트림)에 따라 적절한 open 플래그를 설정하고,
 * 성공하면 파일 해시 테이블에 등록한다.
 *
 * 해시 충돌 시 shadow_fd에 기존 fd를 보관하고 다시 열기를 시도한다.
 * (Linux에서 블록 디바이스 close 시 udev가 blkid를 호출하여 캐시를 오염시키는 것을 방지)
 *
 * 반환값: 0=성공, 1=실패
 */
int generic_open_file(struct thread_data *td, struct fio_file *f)
{
	int is_std = 0;
	int flags = 0;
	int from_hash = 0;

	dprint(FD_FILE, "fd open %s\n", f->file_name);

	/* [한국어] "-"는 stdin/stdout을 의미 */
	if (!strcmp(f->file_name, "-")) {
		if (td_rw(td)) {
			log_err("fio: can't read/write to stdin/out\n");
			return 1;
		}
		is_std = 1;

		/*
		 * move output logging to stderr, if we are writing to stdout
		 */
		/* [한국어] stdout에 쓰기할 때는 로그 출력을 stderr로 전환 */
		if (td_write(td))
			f_out = stderr;
	}

	/* [한국어] 기본 플래그 설정 */
	if (td->o.odirect)
		flags |= OS_O_DIRECT;          /* Direct I/O */
	flags |= td->o.sync_io;              /* 동기 I/O (O_SYNC/O_DSYNC) */
	if (td->o.create_on_open && td->o.allow_create)
		flags |= O_CREAT;                /* 열기 시 파일 생성 */
	if (f->filetype != FIO_TYPE_FILE)
		flags |= FIO_O_NOATIME;          /* 비정규 파일은 atime 갱신 안 함 */

open_again:
	/* [한국어] I/O 방향에 따라 열기 모드 결정 */
	if (td_write(td)) {
		/* [한국어] 쓰기 작업 */
		if (!read_only)
			flags |= O_RDWR;

		if (td->o.verify_only) {
			/* [한국어] 검증 전용 모드: 읽기만 필요 */
			flags &= ~O_RDWR;
			flags |= O_RDONLY;
		}

		if (f->filetype == FIO_TYPE_FILE && td->o.allow_create)
			flags |= O_CREAT;

		if (is_std)
			f->fd = dup(STDOUT_FILENO);
		else
			from_hash = file_lookup_open(f, flags);
	} else if (td_read(td)) {
		/* [한국어] 읽기 작업 */
		if (td_ioengine_flagged(td, FIO_RO_NEEDS_RW_OPEN) && !read_only)
			flags |= O_RDWR;     /* 일부 엔진은 읽기에도 O_RDWR 필요 */
		else
			flags |= O_RDONLY;

		if (is_std)
			f->fd = dup(STDIN_FILENO);
		else
			from_hash = file_lookup_open(f, flags);
	} else if (td_trim(td)) {
		/* [한국어] 트림 작업 */
		assert(!td_rw(td)); /* should have matched above */
		if (!read_only)
			flags |= O_RDWR;
		from_hash = file_lookup_open(f, flags);
	}

	if (f->fd == -1) {
		char buf[FIO_VERROR_SIZE];
		int __e = errno;

		/* [한국어] EPERM + NOATIME이면 NOATIME 제거 후 재시도 */
		if (__e == EPERM && (flags & FIO_O_NOATIME)) {
			flags &= ~FIO_O_NOATIME;
			goto open_again;
		}
		/* [한국어] EMFILE(fd 부족)이면 shadow_fd 회수 후 재시도 */
		if (__e == EMFILE && file_close_shadow_fds(td))
			goto open_again;

		snprintf(buf, sizeof(buf), "open(%s)", f->file_name);

		if (__e == EINVAL && (flags & OS_O_DIRECT)) {
			log_err("fio: looks like your file system does not " \
				"support direct=1/buffered=0\n");
		}

		td_verror(td, __e, buf);
		return 1;
	}

	/* [한국어] 새로 열린 파일을 해시 테이블에 등록
	 * 등록 실패(동일 파일이 이미 등록됨)하면 현재 fd를 shadow_fd에 보관하고 재시도.
	 * Linux에서 블록 디바이스 close 시 udev/blkid가 호출되어 캐시를 오염시키는
	 * 문제를 우회하기 위해 shadow_fd를 사용한다. */
	if (!from_hash && f->fd != -1) {
		if (add_file_hash(f)) {
			int fio_unused ret;

			/*
			 * Stash away descriptor for later close. This is to
			 * work-around a "feature" on Linux, where a close of
			 * an fd that has been opened for write will trigger
			 * udev to call blkid to check partitions, fs id, etc.
			 * That pollutes the device cache, which can slow down
			 * unbuffered accesses.
			 */
			if (f->shadow_fd == -1)
				f->shadow_fd = f->fd;
			else {
				/*
			 	 * OK to ignore, we haven't done anything
				 * with it
				 */
				ret = generic_close_file(td, f);
			}
			goto open_again;
		}
	}

	return 0;
}

/*
 * This function i.e. get_file_size() is the default .get_file_size
 * implementation of majority of I/O engines.
 */
/* [한국어] 범용 파일 크기 조회 - 대부분의 I/O 엔진이 사용하는 기본 get_file_size 구현 */
int generic_get_file_size(struct thread_data *td, struct fio_file *f)
{
	return get_file_size(td, f);
}

/*
 * open/close all files, so that ->real_file_size gets set
 */
/*
 * [한국어] 모든 파일의 크기를 조회하는 함수
 *
 * 각 파일에 대해 td_io_get_file_size()를 호출하여 real_file_size를 설정한다.
 * ENOENT(파일 미존재)는 무시하고, -1ULL인 경우 size/nr_files로 대체한다.
 */
static int get_file_sizes(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int err = 0;

	for_each_file(td, f, i) {
		dprint(FD_FILE, "get file size for %p/%d/%s\n", f, i,
								f->file_name);

		if (td_io_get_file_size(td, f)) {
			/* [한국어] ENOENT(파일 미존재)는 새로 생성할 파일이므로 에러 클리어 */
			if (td->error != ENOENT) {
				log_err("%s\n", td->verror);
				err = 1;
				break;
			}
			clear_error(td);
		}

		/*
		 * There are corner cases where we end up with -1 for
		 * ->real_file_size due to unsupported file type, etc.
		 * We then just set to size option value divided by number
		 * of files, similar to the way file ->io_size is set.
		 * stat(2) failure doesn't set ->real_file_size to -1.
		 */
		/* [한국어] real_file_size가 -1ULL인 예외 상황 처리
		 * (미지원 파일 유형 등) -> size/nr_files로 대체 */
		if (f->real_file_size == -1ULL && td->o.size)
			f->real_file_size = td->o.size / td->o.nr_files;
	}

	return err;
}

/*
 * [한국어] 파일 시스템 마운트 리스트 메모리 해제
 */
static void free_fs_list(struct thread_data *td)
{
	struct flist_head *n, *tmp;
	struct fio_mount *fm;

	flist_for_each_safe(n, tmp, &td->fs_list) {
		fm = flist_entry(n, struct fio_mount, list);
		flist_del(&fm->list);
		free(fm);
	}
}

/*
 * Get the list of unique file system mounts storing the thread files.
 */
/*
 * [한국어] 파일이 위치한 고유 파일 시스템 마운트를 리스트에 추가
 *
 * end_syncfs 옵션 사용 시, 동일 디바이스(st_dev)의 파일 시스템은
 * 한 번만 등록하여 syncfs() 호출을 최적화한다.
 * 일반 파일만 대상이며, 블록/캐릭터 디바이스는 무시한다.
 */
static int add_file_fs(struct thread_data *td, struct fio_file *f)
{
#ifdef CONFIG_SYNCFS
	struct flist_head *n;
	struct fio_mount *fm;
	struct stat sb;
	char buf[256];
	char *fsdir;

	/* [한국어] 일반 파일만 대상 */
	if (f->filetype != FIO_TYPE_FILE)
		return 0;

	/* [한국어] 파일의 디렉토리 경로에서 디바이스 번호 조회 */
	snprintf(buf, FIO_ARRAY_SIZE(buf), "%s", f->file_name);
	fsdir = dirname(buf);
	if (stat(fsdir, &sb) < 0) {
		log_err("fio: failed to get dir %s information (%s)\n",
			fsdir, strerror(errno));
		return 1;
	}

	/* [한국어] 이미 같은 디바이스(st_dev)가 등록되어 있으면 스킵 */
	fm = NULL;
	flist_for_each(n, &td->fs_list) {
		fm = flist_entry(n, struct fio_mount, list);
		if (fm->key == sb.st_dev)
			return 0;
	}

	/* [한국어] 새 마운트 포인트 등록 */
	fm = calloc(1, sizeof(*fm));
	if (!fm) {
		free_fs_list(td);
		return 1;
	}

	snprintf(fm->__base, FIO_ARRAY_SIZE(fm->__base), "%s", fsdir);
	fm->base = fm->__base;
	fm->key = sb.st_dev;
	flist_add(&fm->list, &td->fs_list);

	dprint(FD_FILE, "Add FS %s\n", fm->base);
#endif
	return 0;
}

/*
 * Get free number of bytes for each file on each unique mount.
 */
/*
 * [한국어] 모든 파일의 파일 시스템 여유 공간 합계를 계산
 *
 * 블록/캐릭터 디바이스는 real_file_size를 직접 더하고,
 * 일반 파일은 고유 마운트별로 한 번만 여유 공간을 조회하여 합산한다.
 * fill_device 모드에서 가용 디스크 크기를 파악하기 위해 사용한다.
 */
static unsigned long long get_fs_free_counts(struct thread_data *td)
{
	struct flist_head *n, *tmp;
	unsigned long long ret = 0;
	struct fio_mount *fm;
	FLIST_HEAD(list);
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i) {
		struct stat sb;
		char buf[256];

		/* [한국어] 블록/캐릭터 디바이스는 디바이스 크기를 직접 합산 */
		if (f->filetype == FIO_TYPE_BLOCK || f->filetype == FIO_TYPE_CHAR) {
			if (f->real_file_size != -1ULL)
				ret += f->real_file_size;
			continue;
		} else if (f->filetype != FIO_TYPE_FILE)
			continue;

		snprintf(buf, FIO_ARRAY_SIZE(buf), "%s", f->file_name);

		if (stat(buf, &sb) < 0) {
			if (errno != ENOENT)
				break;
			strcpy(buf, ".");
			if (stat(buf, &sb) < 0)
				break;
		}

		/* [한국어] 같은 디바이스(st_dev)의 마운트가 이미 있으면 중복 계산 방지 */
		fm = NULL;
		flist_for_each(n, &list) {
			fm = flist_entry(n, struct fio_mount, list);
			if (fm->key == sb.st_dev)
				break;

			fm = NULL;
		}

		if (fm)
			continue;

		/* [한국어] 새 마운트 포인트를 임시 리스트에 추가 */
		fm = calloc(1, sizeof(*fm));
		snprintf(fm->__base, FIO_ARRAY_SIZE(fm->__base), "%s", buf);
		fm->base = basename(fm->__base);
		fm->key = sb.st_dev;
		flist_add(&fm->list, &list);
	}

	/* [한국어] 각 고유 마운트의 여유 공간을 합산 */
	flist_for_each_safe(n, tmp, &list) {
		unsigned long long sz;

		fm = flist_entry(n, struct fio_mount, list);
		flist_del(&fm->list);

		sz = get_fs_free_size(fm->base);
		if (sz && sz != -1ULL)
			ret += sz;

		free(fm);
	}

	return ret;
}

/*
 * [한국어] I/O 시작 오프셋 계산
 *
 * 다음 요소들을 고려하여 파일 내 I/O 시작 위치를 계산:
 *   - file_append: 파일 끝에서 시작
 *   - start_offset / start_offset_percent: 기본 시작 위치
 *   - offset_increment / offset_increment_percent: 서브잡별 오프셋 증가분
 *   - start_offset_align: 블록 정렬
 *
 * 반환값: 계산된 시작 오프셋 (바이트)
 */
uint64_t get_start_offset(struct thread_data *td, struct fio_file *f)
{
	bool align = false;
	struct thread_options *o = &td->o;
	unsigned long long align_bs;
	unsigned long long offset;
	unsigned long long increment;

	/* [한국어] file_append 모드: 기존 파일 끝에서 시작 */
	if (o->file_append && f->filetype == FIO_TYPE_FILE)
		return f->real_file_size;

	/* [한국어] 오프셋 증가분 계산 (퍼센트 또는 절대값) */
	if (o->offset_increment_percent) {
		assert(!o->offset_increment);
		increment = o->offset_increment_percent * f->real_file_size / 100;
		align = true;
	} else
		increment = o->offset_increment;

	/* [한국어] 시작 오프셋 계산 (퍼센트 또는 절대값) */
	if (o->start_offset_percent > 0) {
		/* calculate the raw offset */
		offset = (f->real_file_size * o->start_offset_percent / 100) +
			(td->subjob_number * increment);

		align = true;
	} else {
		/* start_offset_percent not set */
		offset = o->start_offset +
				td->subjob_number * increment;
	}

	if (align) {
		/*
		 * if offset_align is provided, use it
		 */
		/* [한국어] 정렬 기준 결정: start_offset_align이 설정되면 사용, 아니면 최소 블록 크기 */
		if (fio_option_is_set(o, start_offset_align)) {
			align_bs = o->start_offset_align;
		} else {
			/* else take the minimum block size */
			align_bs = td_min_bs(td);
		}

		/*
		 * block align the offset at the next available boundary at
		 * ceiling(offset / align_bs) * align_bs
		 */
		/* [한국어] 오프셋을 align_bs 경계로 올림 정렬 */
		offset = (offset / align_bs + (offset % align_bs != 0)) * align_bs;
	}

	return offset;
}

/*
 * Find longest path component that exists and return its length
 */
/*
 * [한국어] 경로에서 존재하는 가장 긴 디렉토리 경로의 길이를 반환
 *
 * create_work_dirs()에서 사용되며, 이미 존재하는 경로 부분은
 * mkdir를 시도하지 않도록 오프셋을 계산한다.
 */
static int longest_existing_path(const char *path)
{
	char buf[PATH_MAX];
	bool done;
	char *buf_pos;
	int offset;
#ifdef WIN32
	DWORD dwAttr;
#else
	struct stat sb;
#endif

	sprintf(buf, "%s", path);
	done = false;
	/* [한국어] 뒤에서부터 디렉토리 구분자를 찾아가며 존재하는 경로를 확인 */
	while (!done) {
		buf_pos = strrchr(buf, FIO_OS_PATH_SEPARATOR);
		if (!buf_pos) {
			offset = 0;
			break;
		}

		*(buf_pos + 1) = '\0';

#ifdef WIN32
		dwAttr = GetFileAttributesA(buf);
		if (dwAttr != INVALID_FILE_ATTRIBUTES) {
			done = true;
		}
#else
		if (stat(buf, &sb) == 0)
			done = true;
#endif
		if (done)
			offset = buf_pos - buf;
		else
			*buf_pos = '\0';
	}

	return offset;
}

/*
 * [한국어] 파일 경로에 필요한 작업 디렉토리들을 생성
 *
 * 파일 경로에 존재하지 않는 중간 디렉토리가 있으면 mkdir로 생성한다.
 * 예: "/data/fio/test/file" -> /data/fio/test 디렉토리 트리 생성
 *
 * 반환값: true=성공, false=실패
 */
static bool create_work_dirs(struct thread_data *td, const char *fname)
{
	char path[PATH_MAX];
	char *start, *end;
	int offset;

	snprintf(path, PATH_MAX, "%s", fname);
	start = path;

	/* [한국어] 이미 존재하는 가장 긴 경로까지 건너뜀 */
	offset = longest_existing_path(path);
	end = start + offset;
	/* [한국어] 나머지 경로 구성요소를 하나씩 mkdir */
	while ((end = strchr(end, FIO_OS_PATH_SEPARATOR)) != NULL) {
		if (end == start) {
			end++;
			continue;
		}
		*end = '\0';
		errno = 0;
		if (fio_mkdir(path, 0700) && errno != EEXIST) {
			log_err("fio: failed to create dir (%s): %s\n",
				start, strerror(errno));
			return false;
		}
		*end = FIO_OS_PATH_SEPARATOR;
		end++;
	}
	td->flags |= TD_F_DIRS_CREATED;
	return true;
}

/*
 * Open the files and setup files sizes, creating files if necessary.
 */
/*
 * [한국어] 파일 셋업 메인 함수 - fio 실행 전 모든 파일을 준비
 *
 * 이 함수는 I/O 테스트에 사용될 모든 파일의 크기를 확인하고,
 * 필요한 경우 파일을 생성/확장하며, 각 파일의 I/O 범위를 계산한다.
 *
 * 처리 흐름:
 *   1) 작업 디렉토리 생성 (필요한 경우)
 *   2) 파일/디바이스 크기 조회 (get_file_sizes 또는 엔진의 setup)
 *   3) ZBD 초기화 (zone 모드인 경우)
 *   4) 파일별 io_size 계산 (size, file_size, size_percent 등 옵션 반영)
 *   5) 파일 확장 (extend_file) - 읽기 작업 시 데이터가 필요한 경우
 *   6) 파일 사전 채우기 (prepopulate) - 엔진이 지원하는 경우
 *   7) SPRandom 초기화 (필요한 경우)
 *   8) total_io_size 계산
 *   9) ZBD 최종 셋업
 *  10) FDP(Data Placement) 초기화
 *
 * 반환값: 0=성공, 1=실패
 */
int setup_files(struct thread_data *td)
{
	unsigned long long total_size, extend_size;
	struct thread_options *o = &td->o;
	struct fio_file *f;
	unsigned int i, nr_fs_extra = 0;
	int err = 0, need_extend;
	int old_state;
	const unsigned long long bs = td_min_bs(td);  /* 최소 블록 크기 */
	uint64_t fs = 0;  /* 파일별 크기 (size / nr_files) */

	dprint(FD_FILE, "setup files\n");

	/* [한국어] 실행 상태를 TD_SETTING_UP으로 변경 */
	old_state = td_bump_runstate(td, TD_SETTING_UP);

	/* [한국어] 1단계: 파일 경로에 필요한 디렉토리 생성 */
	for_each_file(td, f, i) {
		if (!td_ioengine_flagged(td, FIO_DISKLESSIO) &&
		    strchr(f->file_name, FIO_OS_PATH_SEPARATOR) &&
		    !(td->flags & TD_F_DIRS_CREATED) &&
		    !create_work_dirs(td, f->file_name))
			goto err_out;
	}

	/* [한국어] end_syncfs 옵션 호환성 검사 */
	if (td->o.end_syncfs &&
	    !td_ioengine_flagged(td, FIO_SYNCFS) &&
	    !td_ioengine_flagged(td, FIO_ASYNCIO_SYNC_SYNCFS)) {
		log_err("%s: I/O engine does not support syncfs\n", o->name);
		td_verror(td, EINVAL, "end_syncfs");
		goto err_out;
	}

	/*
	 * Find out physical size of files or devices for this thread,
	 * before we determine I/O size and range of our targets.
	 * If ioengine defines a setup() method, it's responsible for
	 * opening the files and setting f->real_file_size to indicate
	 * the valid range for that file.
	 */
	/* [한국어] 2단계: 파일/디바이스의 실제 크기 조회
	 * I/O 엔진에 setup() 콜백이 있으면 그것을 사용, 없으면 기본 get_file_sizes() */
	if (td->io_ops->setup)
		err = td->io_ops->setup(td);
	else
		err = get_file_sizes(td);

	if (err)
		goto err_out;

	/* [한국어] 3단계: ZBD(Zoned Block Device) 초기화 */
	if (td->o.zone_mode == ZONE_MODE_ZBD) {
		err = zbd_init_files(td);
		if (err)
			goto err_out;
	}
	zbd_recalc_options_with_zone_granularity(td);

	/* [한국어] iolog 재생 모드에서는 이하 크기 계산을 건너뜀 */
	if (o->read_iolog_file)
		goto done;

	/*
	 * check sizes. if the files/devices do not exist and the size
	 * isn't passed to fio, abort. While at it, build the list of file
	 * system mounts if end_syncfs is specified.
	 */
	/* [한국어] 4단계: 전체 크기 합산 및 FS 마운트 리스트 구성 */
	total_size = 0;
	for_each_file(td, f, i) {
		f->fileno = i;
		if (f->real_file_size == -1ULL)
			total_size = -1ULL;
		else
			total_size += f->real_file_size;

		/* [한국어] end_syncfs 옵션이면 파일 시스템 마운트 정보 수집 */
		if (td->o.end_syncfs) {
			err = add_file_fs(td, f);
			if (err)
				goto err_out;
		}
	}

	/* [한국어] fill_device 모드: 파일 시스템 여유 공간 조회 */
	if (o->fill_device)
		td->fill_device_size = get_fs_free_counts(td);

	/*
	 * device/file sizes are zero and no size given, punt
	 */
	/* [한국어] 크기가 0이고 size= 옵션도 없으면 에러 */
	if ((!total_size || total_size == -1ULL) && !o->size &&
	    !td_ioengine_flagged(td, FIO_NOIO) && !o->fill_device &&
	    !(o->nr_files && (o->file_size_low || o->file_size_high))) {
		log_err("%s: you need to specify size=\n", o->name);
		td_verror(td, EINVAL, "total_file_size");
		goto err_out;
	}

	/*
	 * Calculate per-file size and potential extra size for the
	 * first files, if needed (i.e. if we don't have a fixed size).
	 */
	/* [한국어] 파일별 크기 계산: size / nr_files
	 * 나누어떨어지지 않으면 앞의 파일들에 블록 단위로 추가 분배 */
	if (!o->file_size_low && o->nr_files) {
		uint64_t all_fs;

		fs = o->size / o->nr_files;
		all_fs = fs * o->nr_files;

		if (all_fs < o->size)
			nr_fs_extra = (o->size - all_fs) / bs;
	}

	/*
	 * now file sizes are known, so we can set ->io_size. if size= is
	 * not given, ->io_size is just equal to ->real_file_size. if size
	 * is given, ->io_size is size / nr_files.
	 */
	/* [한국어] 5단계: 각 파일의 io_size (I/O 대상 크기) 및 file_offset 설정 */
	extend_size = total_size = 0;
	need_extend = 0;
	for_each_file(td, f, i) {
		f->file_offset = get_start_offset(td, f);

		/*
		 * Update ->io_size depending on options specified.
		 * ->file_size_low being 0 means filesize option isn't set.
		 * Non zero ->file_size_low equals ->file_size_high means
		 * filesize option is set in a fixed size format.
		 * Non zero ->file_size_low not equals ->file_size_high means
		 * filesize option is set in a range format.
		 */
		/* [한국어] io_size 결정 로직:
		 * - filesize 미설정(file_size_low==0): size/nr_files 사용
		 * - filesize 고정값: file_size_low - file_offset
		 * - filesize 범위: 랜덤 크기 생성 */
		if (!o->file_size_low) {
			/*
			 * no file size or range given, file size is equal to
			 * total size divided by number of files. If the size
			 * doesn't divide nicely with the min blocksize,
			 * make the first files bigger.
			 */
			f->io_size = fs;
			if (nr_fs_extra) {
				nr_fs_extra--;
				f->io_size += bs;
			}

			/*
			 * We normally don't come here for regular files, but
			 * if the result is 0 for a regular file, set it to the
			 * real file size. This could be size of the existing
			 * one if it already exists, but otherwise will be set
			 * to 0. A new file won't be created because
			 * ->io_size + ->file_offset equals ->real_file_size.
			 */
			/* [한국어] io_size가 0이면 실제 파일 크기에서 오프셋을 뺀 값 사용 */
			if (!f->io_size) {
				if (f->file_offset > f->real_file_size)
					goto err_offset;
				f->io_size = f->real_file_size - f->file_offset;
				if (!f->io_size)
					log_info("fio: file %s may be ignored\n",
						f->file_name);
			}
		} else if (f->real_file_size < o->file_size_low ||
			   f->real_file_size > o->file_size_high) {
			if (f->file_offset > o->file_size_low)
				goto err_offset;
			/*
			 * file size given. if it's fixed, use that. if it's a
			 * range, generate a random size in-between.
			 */
			/* [한국어] filesize가 고정값이면 그대로, 범위이면 랜덤 크기 생성 */
			if (o->file_size_low == o->file_size_high)
				f->io_size = o->file_size_low - f->file_offset;
			else {
				f->io_size = get_rand_file_size(td)
						- f->file_offset;
			}
		} else
			f->io_size = f->real_file_size - f->file_offset;

		/* [한국어] total_size 합산 (size_percent, io_size_percent 적용) */
		if (f->io_size == -1ULL)
			total_size = -1ULL;
		else {
			uint64_t io_size;

			/* [한국어] size_percent 적용: 파일의 일부분만 I/O 대상으로 함 */
                        if (o->size_percent && o->size_percent != 100) {
				uint64_t file_size;

				file_size = f->io_size + f->file_offset;
				f->io_size = (file_size *
					      o->size_percent) / 100;
				if (f->io_size > (file_size - f->file_offset))
					f->io_size = file_size - f->file_offset;

				f->io_size -= (f->io_size % td_min_bs(td));
			}

			io_size = f->io_size;
			/* [한국어] io_size_percent 적용: 총 I/O 양 조정 */
			if (o->io_size_percent && o->io_size_percent != 100) {
				io_size *= o->io_size_percent;
				io_size /= 100;
			}

			total_size += io_size;
		}

		/* [한국어] 파일 확장 필요 여부 판단: io_size + offset > 실제 크기이면 확장 필요 */
		if (f->filetype == FIO_TYPE_FILE &&
		    (f->io_size + f->file_offset) > f->real_file_size) {
			if (!td_ioengine_flagged(td, FIO_DISKLESSIO) &&
			    !o->create_on_open) {
				need_extend++;
				extend_size += (f->io_size + f->file_offset);
				fio_file_set_extend(f);
			} else if (!td_ioengine_flagged(td, FIO_DISKLESSIO) ||
				   (td_ioengine_flagged(td, FIO_DISKLESSIO) &&
				    td_ioengine_flagged(td, FIO_FAKEIO)))
				f->real_file_size = f->io_size + f->file_offset;
		}
	}

	/* [한국어] block_error_hist 옵션: 트림 블록별 에러 히스토그램 초기화 */
	if (td->o.block_error_hist) {
		int len;

		assert(td->o.nr_files == 1);	/* checked in fixup_options */
		f = td->files[0];
		len = f->io_size / td->o.bs[DDIR_TRIM];
		if (len > MAX_NR_BLOCK_INFOS || len <= 0) {
			log_err("fio: cannot calculate block histogram with "
				"%d trim blocks, maximum %d\n",
				len, MAX_NR_BLOCK_INFOS);
			td_verror(td, EINVAL, "block_error_hist");
			goto err_out;
		}

		td->ts.nr_block_infos = len;
		for (i = 0; i < len; i++)
			td->ts.block_infos[i] =
				BLOCK_INFO(0, BLOCK_STATE_UNINIT);
	} else
		td->ts.nr_block_infos = 0;

	/* [한국어] 전체 I/O 크기가 size= 옵션보다 작으면 total_size로 조정 */
	if (!o->size || (total_size && o->size > total_size))
		o->size = total_size;

	/* [한국어] 블록 크기가 데이터 범위보다 크면 에러 */
	if (o->size < td_min_bs(td)) {
		log_err("fio: blocksize is larger than data set range\n");
		goto err_out;
	}

	/*
	 * See if we need to extend some files, typically needed when our
	 * target regular files don't exist yet, but our jobs require them
	 * initially due to read I/Os.
	 */
	/* [한국어] 6단계: 파일 확장 - 읽기 작업에 필요한 데이터 파일 생성 */
	if (need_extend) {
		temp_stall_ts = 1;  /* 통계 타이머 일시 중지 */
		if (output_format & FIO_OUTPUT_NORMAL) {
			log_info("%s: Laying out IO file%s (%u file%s / %s%lluMiB)\n",
				 o->name,
				 need_extend > 1 ? "s" : "",
				 need_extend,
				 need_extend > 1 ? "s" : "",
				 need_extend > 1 ? "total " : "",
				 extend_size >> 20);
		}

		for_each_file(td, f, i) {
			unsigned long long old_len = -1ULL, extend_len = -1ULL;

			if (!fio_file_extend(f))
				continue;

			assert(f->filetype == FIO_TYPE_FILE);
			fio_file_clear_extend(f);
			if (!o->fill_device) {
				old_len = f->real_file_size;
				extend_len = f->io_size + f->file_offset -
						old_len;
			}
			f->real_file_size = (f->io_size + f->file_offset);
			err = extend_file(td, f);
			if (err)
				break;

			/* [한국어] 확장된 영역의 캐시를 무효화 */
			err = __file_invalidate_cache(td, f, old_len,
								extend_len);

			/*
			 * Shut up static checker
			 */
			if (f->fd != -1)
				close(f->fd);

			f->fd = -1;
			if (err)
				break;
		}
		temp_stall_ts = 0;
	}

	if (err)
		goto err_out;

	/*
	 * Prepopulate files with data. It might be expected to read some
	 * "real" data instead of zero'ed files (if no writes to file occurred
	 * prior to a read job). Engine has to provide a way to do that.
	 */
	/* [한국어] 7단계: 파일 데이터 사전 채우기 (I/O 엔진의 prepopulate_file 콜백) */
	if (td->io_ops->prepopulate_file) {
		temp_stall_ts = 1;

		for_each_file(td, f, i) {
			if (output_format & FIO_OUTPUT_NORMAL) {
				log_info("%s: Prepopulating IO file (%s)\n",
							o->name, f->file_name);
			}

			err = td->io_ops->prepopulate_file(td, f);
			if (err)
				break;

			err = __file_invalidate_cache(td, f, f->file_offset,
								f->io_size);

			/*
			 * Shut up static checker
			 */
			if (f->fd != -1)
				close(f->fd);

			f->fd = -1;
			if (err)
				break;
		}
		temp_stall_ts = 0;
	}

	if (err)
		goto err_out;

	/* [한국어] 8단계: SPRandom 초기화 (sprandom 옵션 사용 시) */
	if (td->o.sprandom) {
		if (td->o.nr_files != 1) {
			 log_err("fio: SPRandom supports only one file");
			 goto err_out;
		}
		err = sprandom_init(td, td->files[0]);
		if (err)
			goto err_out;
	}

	/* [한국어] 9단계: total_io_size 계산 (io_size 또는 size) * loops */
	if (o->io_size)
		td->total_io_size = o->io_size * o->loops;
	else
		td->total_io_size = o->size * o->loops;

done:
	/* [한국어] 10단계: ZBD 최종 셋업 */
	if (td->o.zone_mode == ZONE_MODE_ZBD) {
		err = zbd_setup_files(td);
		if (err)
			goto err_out;
	}

	/* [한국어] create_only 모드: 파일 생성만 하고 I/O는 수행하지 않음 */
	if (o->create_only)
		td->done = 1;

	td_restore_runstate(td, old_state);

	/* [한국어] 11단계: FDP(Flexible Data Placement) 초기화 */
	if (td->o.dp_type != FIO_DP_NONE) {
		err = dp_init(td);
		if (err)
			goto err_out;
	}

	return 0;

err_offset:
	log_err("%s: you need to specify valid offset=\n", o->name);
err_out:
	td_restore_runstate(td, old_state);
	return 1;
}

/*
 * [한국어] 모든 파일에 대해 사전 읽기(pre-read) 수행
 * 파일을 미리 읽어서 페이지 캐시를 워밍한다.
 * 반환값: true=모두 성공, false=하나라도 실패
 */
bool pre_read_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_FILE, "pre_read files\n");

	for_each_file(td, f, i) {
		if (!pre_read_file(td, f))
			return false;
	}

	return true;
}

/*
 * [한국어] 개별 파일의 랜덤 분포 초기화 (내부 함수)
 *
 * random_distribution 옵션에 따라 Zipf, Pareto, 또는 Gauss 분포를
 * 파일별로 초기화한다. 파일 이름의 해시를 시드로 사용하여
 * 파일별로 다른 랜덤 패턴을 생성한다.
 */
static void __init_rand_distribution(struct thread_data *td, struct fio_file *f)
{
	unsigned int range_size, seed;
	uint64_t nranges;
	uint64_t fsize;

	/* [한국어] 블록 크기: 읽기/쓰기 중 작은 최소 블록 크기 사용 */
	range_size = min(td->o.min_bs[DDIR_READ], td->o.min_bs[DDIR_WRITE]);
	fsize = min(f->real_file_size, f->io_size);

	/* [한국어] 전체 블록 수 계산 */
	nranges = (fsize + range_size - 1ULL) / range_size;

	/* [한국어] 파일 이름 해시 기반 시드 생성 (파일별로 고유) */
	seed = jhash(f->file_name, strlen(f->file_name), 0) * td->thread_number *
		td->rand_seeds[FIO_RAND_BLOCK_OFF];

	/* [한국어] 분포 유형에 따라 초기화 */
	if (td->o.random_distribution == FIO_RAND_DIST_ZIPF)
		zipf_init(&f->zipf, nranges, td->o.zipf_theta.u.f, td->o.random_center.u.f, seed);
	else if (td->o.random_distribution == FIO_RAND_DIST_PARETO)
		pareto_init(&f->zipf, nranges, td->o.pareto_h.u.f, td->o.random_center.u.f, seed);
	else if (td->o.random_distribution == FIO_RAND_DIST_GAUSS)
		gauss_init(&f->gauss, nranges, td->o.gauss_dev.u.f, td->o.random_center.u.f, seed);
}

/*
 * [한국어] 랜덤 분포 초기화 (외부 인터페이스)
 *
 * 균일(RANDOM), 구역(ZONED) 분포가 아닌 경우에만 실행한다.
 * 모든 파일에 대해 __init_rand_distribution()을 호출한다.
 *
 * 반환값: true=초기화 수행됨, false=초기화 불필요
 */
static bool init_rand_distribution(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int state;

	/* [한국어] 균일 랜덤이나 구역 기반 분포는 별도 초기화 불필요 */
	if (td->o.random_distribution == FIO_RAND_DIST_RANDOM ||
	    td->o.random_distribution == FIO_RAND_DIST_ZONED ||
	    td->o.random_distribution == FIO_RAND_DIST_ZONED_ABS)
		return false;

	state = td_bump_runstate(td, TD_SETTING_UP);

	for_each_file(td, f, i)
		__init_rand_distribution(td, f);

	td_restore_runstate(td, state);
	return true;
}

/*
 * Check if the number of blocks exceeds the randomness capability of
 * the selected generator. Tausworthe is 32-bit, the others are fully
 * 64-bit capable.
 */
/*
 * [한국어] 랜덤 생성기의 한계 확인
 *
 * Tausworthe(32비트) 생성기는 FRAND32_MAX(약 40억)개의 블록까지만 지원한다.
 * 블록 수가 이를 초과하면:
 *   - 사용자가 명시적으로 선택하지 않았으면 tausworthe64로 자동 전환
 *   - 사용자가 명시적으로 선택했으면 경고만 출력
 */
static int check_rand_gen_limits(struct thread_data *td, struct fio_file *f,
				 uint64_t blocks)
{
	if (blocks <= FRAND32_MAX)
		return 0;
	if (td->o.random_generator != FIO_RAND_GEN_TAUSWORTHE)
		return 0;

	/*
	 * If the user hasn't specified a random generator, switch
	 * to tausworthe64 with informational warning. If the user did
	 * specify one, just warn.
	 */
	log_info("fio: file %s exceeds 32-bit tausworthe random generator.\n",
			f->file_name);

	if (!fio_option_is_set(&td->o, random_generator)) {
		log_info("fio: Switching to tausworthe64. Use the "
			 "random_generator= option to get rid of this "
			 "warning.\n");
		td->o.random_generator = FIO_RAND_GEN_TAUSWORTHE64;
		init_rand_offset_seed(td);
		return 0;
	}

	/*
	 * Just make this information to avoid breaking scripts.
	 */
	log_info("fio: Use the random_generator= option to switch to lfsr or "
			 "tausworthe64.\n");
	return 0;
}

/*
 * [한국어] 랜덤 I/O 맵 초기화 - 랜덤 블록 선택을 위한 자료구조 초기화
 *
 * 처리 순서:
 *   1) 비균일 분포(Zipf/Pareto/Gauss) 초기화 시도
 *   2) 순차 I/O면 스킵
 *   3) 파일별로 랜덤 생성기 한계 확인
 *   4) LFSR 생성기 선택 시: LFSR 초기화
 *   5) norandommap이 아니면: axmap(비트맵) 할당
 *   6) 할당 실패 시: softrandommap이면 경고 후 계속, 아니면 에러
 *
 * 반환값: true=성공, false=실패
 */
bool init_random_map(struct thread_data *td)
{
	unsigned long long blocks;
	struct fio_file *f;
	unsigned int i;

	/* [한국어] 비균일 분포 초기화가 수행되었으면 완료 */
	if (init_rand_distribution(td))
		return true;
	/* [한국어] 순차 I/O면 랜덤 맵 불필요 */
	if (!td_random(td))
		return true;

	for_each_file(td, f, i) {
		uint64_t fsize = min(f->real_file_size, f->io_size);

		/* [한국어] strided zone 모드면 zone_range만큼만 매핑 */
		if (td->o.zone_mode == ZONE_MODE_STRIDED)
			fsize = td->o.zone_range;

		blocks = fsize / (unsigned long long) td->o.rw_min_bs;

		/* [한국어] 랜덤 생성기 한계 확인 (32비트 Tausworthe 등) */
		if (check_rand_gen_limits(td, f, blocks))
			return false;

		if (td->o.random_generator == FIO_RAND_GEN_LFSR) {
			/* [한국어] LFSR 기반 랜덤 생성기 초기화 */
			uint64_t seed;

			seed = td->rand_seeds[FIO_RAND_BLOCK_OFF];

			if (!lfsr_init(&f->lfsr, blocks, seed, 0)) {
				fio_file_set_lfsr(f);
				continue;
			} else {
				log_err("fio: failed initializing LFSR\n");
				return false;
			}
		} else if (!td->o.norandommap) {
			/* [한국어] axmap(비트맵) 기반 블록 추적 - 방문한 블록을 비트로 표시 */
			f->io_axmap = axmap_new(blocks);
			if (f->io_axmap) {
				fio_file_set_axmap(f);
				continue;
			}
		} else if (td->o.norandommap)
			continue;

		/* [한국어] 할당 실패 처리: softrandommap이면 경고 후 계속 */
		if (!td->o.softrandommap) {
			log_err("fio: failed allocating random map. If running"
				" a large number of jobs, try the 'norandommap'"
				" option or set 'softrandommap'. Or give"
				" a larger --alloc-size to fio.\n");
			return false;
		}

		log_info("fio: file %s failed allocating random map. Running "
			 "job without.\n", f->file_name);
	}

	return true;
}

/* [한국어] 모든 열린 파일 닫기 (메모리 해제 없이) */
void close_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i) {
		if (fio_file_open(f))
			td_io_close_file(td, f);
	}
}

/*
 * [한국어] fio_file 구조체 메모리 해제
 *
 * axmap, RUH 정보, SPRandom 정보를 먼저 해제하고,
 * smalloc으로 할당된 것인지에 따라 free 또는 sfree를 호출한다.
 */
void fio_file_free(struct fio_file *f)
{
	if (fio_file_axmap(f))
		axmap_free(f->io_axmap);
	if (f->ruhs_info)
		sfree(f->ruhs_info);
	if (f->spr_info)
		sprandom_free(f->spr_info);
	if (!fio_file_smalloc(f)) {
		free(f->file_name);
		free(f);
	} else {
		sfree(f->file_name);
		sfree(f);
	}
}

/*
 * [한국어] 모든 파일을 닫고 메모리를 해제하는 정리 함수
 *
 * 각 파일에 대해:
 *   1) unlink 옵션이면 파일 삭제
 *   2) 열려있으면 닫기
 *   3) 해시 테이블에서 제거
 *   4) 다시 한번 unlink (닫은 후에도)
 *   5) ZBD/FDP 관련 자원 해제
 *   6) fio_file 메모리 해제
 * 마지막으로 파일 배열과 잠금 배열을 해제한다.
 */
void close_and_free_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_FILE, "close files\n");

	for_each_file(td, f, i) {
		/* [한국어] unlink 옵션 + 일반 파일이면 삭제 */
		if (td->o.unlink && f->filetype == FIO_TYPE_FILE) {
			dprint(FD_FILE, "free unlink %s\n", f->file_name);
			td_io_unlink_file(td, f);
		}

		if (fio_file_open(f))
			td_io_close_file(td, f);

		remove_file_hash(f);

		/* [한국어] 닫은 후에도 다시 한번 unlink 시도 */
		if (td->o.unlink && f->filetype == FIO_TYPE_FILE) {
			dprint(FD_FILE, "free unlink %s\n", f->file_name);
			td_io_unlink_file(td, f);
		}

		zbd_close_file(f);
		fdp_free_ruhs_info(f);
		fio_file_free(f);
	}

	/* [한국어] 파일 배열 및 잠금 배열 메모리 해제 */
	td->o.filename = NULL;
	free(td->files);
	free(td->file_locks);
	td->files_index = 0;
	td->files = NULL;
	td->file_locks = NULL;
	td->o.file_lock_mode = FILE_LOCK_NONE;
	td->o.nr_files = 0;
}

/*
 * [한국어] 파일 유형 자동 감지
 *
 * 파일명이 "-"이면 파이프(stdin/stdout),
 * 그렇지 않으면 stat()으로 파일 유형을 판별:
 *   S_ISBLK -> 블록 디바이스
 *   S_ISCHR -> 캐릭터 디바이스
 *   S_ISFIFO -> 파이프
 *   기본값 -> 일반 파일
 */
static void get_file_type(struct fio_file *f)
{
	struct stat sb;

	if (!strcmp(f->file_name, "-"))
		f->filetype = FIO_TYPE_PIPE;
	else
		f->filetype = FIO_TYPE_FILE;

#ifdef WIN32
	/* \\.\ is the device namespace in Windows, where every file is
	 * a block device */
	if (strncmp(f->file_name, "\\\\.\\", 4) == 0)
		f->filetype = FIO_TYPE_BLOCK;
#endif

	if (!stat(f->file_name, &sb)) {
		if (S_ISBLK(sb.st_mode))
			f->filetype = FIO_TYPE_BLOCK;
		else if (S_ISCHR(sb.st_mode))
			f->filetype = FIO_TYPE_CHAR;
		else if (S_ISFIFO(sb.st_mode))
			f->filetype = FIO_TYPE_PIPE;
	}
}

/*
 * [한국어] 파일명이 이미 할당(등록)되었는지 확인하는 내부 함수
 *
 * 블룸 필터로 빠르게 체크한 후, 실제 리스트를 순회하여 확인한다.
 * set=true이면 블룸 필터에도 등록한다.
 */
static bool __is_already_allocated(const char *fname, bool set)
{
	struct flist_head *entry;
	bool ret;

	/* [한국어] 블룸 필터로 빠른 존재 여부 체크 */
	ret = file_bloom_exists(fname, set);
	if (!ret)
		return ret;

	/* [한국어] 블룸 필터 히트: 실제 리스트에서 정확히 확인 (false positive 방지) */
	flist_for_each(entry, &filename_list) {
		struct file_name *fn;

		fn = flist_entry(entry, struct file_name, list);

		if (!strcmp(fn->filename, fname))
			return true;
	}

	return false;
}

/* [한국어] 파일명이 이미 할당되었는지 확인 (잠금 보호) */
static bool is_already_allocated(const char *fname)
{
	bool ret;

	fio_file_hash_lock();
	ret = __is_already_allocated(fname, false);
	fio_file_hash_unlock();

	return ret;
}

/*
 * [한국어] 파일명을 할당 완료 목록에 등록
 *
 * 멀티 잡(numjobs > 1) 환경에서 같은 파일의 중복 생성을 방지하기 위해,
 * 파일명을 전역 filename_list에 등록한다.
 */
static void set_already_allocated(const char *fname)
{
	struct file_name *fn;

	fn = malloc(sizeof(struct file_name));
	fn->filename = strdup(fname);

	fio_file_hash_lock();
	if (!__is_already_allocated(fname, true)) {
		flist_add_tail(&fn->list, &filename_list);
		fn = NULL;  /* 리스트에 추가됨, 해제하면 안 됨 */
	}
	fio_file_hash_unlock();

	/* [한국어] 이미 등록되어 있었으면 새로 만든 것을 해제 */
	if (fn) {
		free(fn->filename);
		free(fn);
	}
}

/* [한국어] 할당 완료 목록의 모든 항목 해제 */
static void free_already_allocated(void)
{
	struct flist_head *entry, *tmp;
	struct file_name *fn;

	if (flist_empty(&filename_list))
		return;

	fio_file_hash_lock();
	flist_for_each_safe(entry, tmp, &filename_list) {
		fn = flist_entry(entry, struct file_name, list);
		free(fn->filename);
		flist_del(&fn->list);
		free(fn);
	}

	fio_file_hash_unlock();
}

/*
 * [한국어] 새 fio_file 구조체 할당
 *
 * FIO_NOFILEHASH 엔진이면 일반 calloc, 아니면 공유 메모리(scalloc) 사용.
 * fd와 shadow_fd를 -1로 초기화하고 fio_file_reset()을 호출한다.
 */
static struct fio_file *alloc_new_file(struct thread_data *td)
{
	struct fio_file *f;

	if (td_ioengine_flagged(td, FIO_NOFILEHASH))
		f = calloc(1, sizeof(*f));
	else
		f = scalloc(1, sizeof(*f));  /* 공유 메모리 할당 (프로세스 간 공유 가능) */
	if (!f) {
		assert(0);
		return NULL;
	}

	f->fd = -1;
	f->shadow_fd = -1;
	fio_file_reset(td, f);
	if (!td_ioengine_flagged(td, FIO_NOFILEHASH))
		fio_file_set_smalloc(f);
	return f;
}

/*
 * [한국어] 파일이 존재하면서 일반 파일(regular file)이 아닌지 확인
 *
 * 블록 디바이스, 캐릭터 디바이스 등 특수 파일인 경우 true를 반환한다.
 * add_file()에서 기존 디바이스 파일의 중복 등록을 방지하기 위해 사용한다.
 */
bool exists_and_not_regfile(const char *filename)
{
	struct stat sb;

	if (lstat(filename, &sb) == -1)
		return false;

#ifndef WIN32 /* NOT Windows */
	if (S_ISREG(sb.st_mode))
		return false;
#else
	/* \\.\ is the device namespace in Windows, where every file
	 * is a device node */
	if (S_ISREG(sb.st_mode) && strncmp(filename, "\\\\.\\", 4) != 0)
		return false;
#endif

	return true;
}

/*
 * [한국어] 작업(thread_data)에 파일을 추가하는 핵심 함수
 *
 * 파일 경로를 구성하고, fio_file 구조체를 할당하여 파일 배열에 등록한다.
 * 멀티 잡 환경에서 이미 할당된 일반 파일은 중복 추가하지 않는다.
 *
 * 매개변수:
 *   td     - 작업 스레드 데이터
 *   fname  - 파일명 (directory 옵션이 있으면 경로가 앞에 붙음)
 *   numjob - 잡 번호 (디렉토리 설정에 사용)
 *   inc    - 1이면 nr_files 카운터 증가
 *
 * 반환값: 파일 인덱스, 또는 중복 시 0
 */
int add_file(struct thread_data *td, const char *fname, int numjob, int inc)
{
	int cur_files = td->files_index;
	char file_name[PATH_MAX];
	struct fio_file *f;
	int len = 0;

	dprint(FD_FILE, "add file %s\n", fname);

	/* [한국어] directory 옵션이 있으면 경로 접두사 구성 */
	if (td->o.directory)
		len = set_name_idx(file_name, PATH_MAX, td->o.directory, numjob,
					td->o.unique_filename);

	sprintf(file_name + len, "%s", fname);

	/* clean cloned siblings using existing files */
	/* [한국어] 멀티 잡 환경에서 이미 할당된 일반 파일은 중복 추가하지 않음 */
	if (numjob && is_already_allocated(file_name) &&
	    !exists_and_not_regfile(fname))
		return 0;

	/* [한국어] 새 fio_file 구조체 할당 */
	f = alloc_new_file(td);

	/* [한국어] 파일 배열 크기가 부족하면 확장 */
	if (td->files_size <= td->files_index) {
		unsigned int new_size = td->o.nr_files + 1;

		dprint(FD_FILE, "resize file array to %d files\n", new_size);

		td->files = realloc(td->files, new_size * sizeof(f));
		if (td->files == NULL) {
			log_err("fio: realloc OOM\n");
			assert(0);
		}
		if (td->o.file_lock_mode != FILE_LOCK_NONE) {
			td->file_locks = realloc(td->file_locks, new_size);
			if (!td->file_locks) {
				log_err("fio: realloc OOM\n");
				assert(0);
			}
			td->file_locks[cur_files] = FILE_LOCK_NONE;
		}
		td->files_size = new_size;
	}
	td->files[cur_files] = f;
	f->fileno = cur_files;

	/*
	 * init function, io engine may not be loaded yet
	 */
	/* [한국어] 디스크리스 I/O 엔진이면 파일 크기를 무한으로 설정 */
	if (td->io_ops && td_ioengine_flagged(td, FIO_DISKLESSIO))
		f->real_file_size = -1ULL;

	/* [한국어] 파일명 저장 (NOFILEHASH면 일반 strdup, 아니면 공유 메모리) */
	if (td_ioengine_flagged(td, FIO_NOFILEHASH))
		f->file_name = strdup(file_name);
	else
		f->file_name = smalloc_strdup(file_name);

	/* can't handle smalloc failure from here */
	assert(f->file_name);

	/* [한국어] 파일 유형 설정: 옵션으로 지정되었으면 사용, 아니면 자동 감지 */
	if (td->o.filetype)
		f->filetype = td->o.filetype;
	else
		get_file_type(f);

	/* [한국어] 파일 잠금 모드에 따라 세마포어 또는 rwlock 초기화 */
	switch (td->o.file_lock_mode) {
	case FILE_LOCK_NONE:
		break;
	case FILE_LOCK_READWRITE:
		f->rwlock = fio_rwlock_init();
		break;
	case FILE_LOCK_EXCLUSIVE:
		f->lock = fio_sem_init(FIO_SEM_UNLOCKED);
		break;
	default:
		log_err("fio: unknown lock mode: %d\n", td->o.file_lock_mode);
		assert(0);
	}

	td->files_index++;

	/* [한국어] 멀티 잡이면 중복 방지를 위해 파일명을 등록 */
	if (td->o.numjobs > 1)
		set_already_allocated(file_name);

	if (inc)
		td->o.nr_files++;

	dprint(FD_FILE, "file %p \"%s\" added at %d\n", f, f->file_name,
							cur_files);

	return cur_files;
}

/*
 * [한국어] 중복 없이 파일 추가
 *
 * 이미 동일한 파일명이 등록되어 있으면 기존 인덱스를 반환하고,
 * 없으면 add_file()로 새로 추가한다.
 */
int add_file_exclusive(struct thread_data *td, const char *fname)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i) {
		if (!strcmp(f->file_name, fname))
			return i;
	}

	return add_file(td, fname, 0, 1);
}

/*
 * [한국어] 파일 참조 카운트 증가
 * 파일이 열려 있어야 하며, I/O가 진행 중임을 표시한다.
 */
void get_file(struct fio_file *f)
{
	dprint(FD_FILE, "get file %s, ref=%d\n", f->file_name, f->references);
	assert(fio_file_open(f));
	f->references++;
}

/*
 * [한국어] 파일 참조 카운트 감소 및 닫기
 *
 * 참조 카운트가 0이 되면:
 *   1) 디스크 유틸리티 참조 감소
 *   2) 파일 잠금 해제
 *   3) fsync_on_close 옵션이면 fsync 수행
 *   4) I/O 엔진의 close_file 호출
 *
 * 반환값: 0=성공, 에러코드=실패
 */
int put_file(struct thread_data *td, struct fio_file *f)
{
	int f_ret = 0, ret = 0;

	dprint(FD_FILE, "put file %s, ref=%d\n", f->file_name, f->references);

	if (!fio_file_open(f)) {
		assert(f->fd == -1);
		return 0;
	}

	assert(f->references);
	if (--f->references)
		return 0;

	/* [한국어] 참조 카운트가 0이 됨 - 파일 닫기 처리 */
	disk_util_dec(f->du);

	if (td->o.file_lock_mode != FILE_LOCK_NONE)
		unlock_file_all(td, f);

	/* [한국어] fsync_on_close: 파일 닫기 전 fsync 수행 */
	if (should_fsync(td) && td->o.fsync_on_close) {
		f_ret = fsync(f->fd);
		if (f_ret < 0)
			f_ret = errno;
	}

	if (td->io_ops->close_file)
		ret = td->io_ops->close_file(td, f);

	if (!ret)
		ret = f_ret;

	td->nr_open_files--;
	fio_file_clear_closing(f);
	fio_file_clear_open(f);
	assert(f->fd == -1);
	return ret;
}

/*
 * [한국어] 파일 잠금 획득
 *
 * file_lock_mode에 따라:
 *   - READWRITE: 읽기 방향이면 읽기 잠금, 쓰기 방향이면 쓰기 잠금
 *   - EXCLUSIVE: 세마포어로 배타적 잠금
 */
void lock_file(struct thread_data *td, struct fio_file *f, enum fio_ddir ddir)
{
	if (!f->lock || td->o.file_lock_mode == FILE_LOCK_NONE)
		return;

	if (td->o.file_lock_mode == FILE_LOCK_READWRITE) {
		if (ddir == DDIR_READ)
			fio_rwlock_read(f->rwlock);
		else
			fio_rwlock_write(f->rwlock);
	} else if (td->o.file_lock_mode == FILE_LOCK_EXCLUSIVE)
		fio_sem_down(f->lock);

	td->file_locks[f->fileno] = td->o.file_lock_mode;
}

/* [한국어] 파일 잠금 해제 */
void unlock_file(struct thread_data *td, struct fio_file *f)
{
	if (!f->lock || td->o.file_lock_mode == FILE_LOCK_NONE)
		return;

	if (td->o.file_lock_mode == FILE_LOCK_READWRITE)
		fio_rwlock_unlock(f->rwlock);
	else if (td->o.file_lock_mode == FILE_LOCK_EXCLUSIVE)
		fio_sem_up(f->lock);

	td->file_locks[f->fileno] = FILE_LOCK_NONE;
}

/* [한국어] 파일에 대한 모든 잠금 해제 (아직 잠겨있는 경우에만) */
void unlock_file_all(struct thread_data *td, struct fio_file *f)
{
	if (td->o.file_lock_mode == FILE_LOCK_NONE || !td->file_locks)
		return;
	if (td->file_locks[f->fileno] != FILE_LOCK_NONE)
		unlock_file(td, f);
}

/*
 * [한국어] 디렉토리를 재귀적으로 탐색하여 모든 일반 파일을 작업에 추가
 *
 * 하위 디렉토리도 재귀적으로 탐색하며, 일반 파일(S_ISREG)만 add_file()한다.
 * "."과 ".." 항목은 건너뛴다.
 *
 * 반환값: false=성공, true=에러 발생
 */
static bool recurse_dir(struct thread_data *td, const char *dirname)
{
	struct dirent *dir;
	bool ret = false;
	DIR *D;

	D = opendir(dirname);
	if (!D) {
		char buf[FIO_VERROR_SIZE];

		snprintf(buf, FIO_VERROR_SIZE, "opendir(%s)", dirname);
		td_verror(td, errno, buf);
		return true;
	}

	while ((dir = readdir(D)) != NULL) {
		char full_path[PATH_MAX];
		struct stat sb;

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;

		sprintf(full_path, "%s%c%s", dirname, FIO_OS_PATH_SEPARATOR, dir->d_name);

		if (lstat(full_path, &sb) == -1) {
			if (errno != ENOENT) {
				td_verror(td, errno, "stat");
				ret = true;
				break;
			}
		}

		if (S_ISREG(sb.st_mode)) {
			add_file(td, full_path, 0, 1);
			continue;
		}
		if (!S_ISDIR(sb.st_mode))
			continue;

		/* [한국어] 하위 디렉토리를 재귀적으로 탐색 */
		ret = recurse_dir(td, full_path);
		if (ret)
			break;
	}

	closedir(D);
	return ret;
}

/*
 * [한국어] 디렉토리 내 파일을 작업에 추가하는 공개 인터페이스
 * 성공하면 추가된 파일 수를 로그로 출력한다.
 */
int add_dir_files(struct thread_data *td, const char *path)
{
	int ret = recurse_dir(td, path);

	if (!ret)
		log_info("fio: opendir added %d files\n", td->o.nr_files);

	return ret;
}

/*
 * [한국어] 다른 작업(org)의 파일 목록을 현재 작업(td)으로 복제
 *
 * 잡 복제(numjobs > 1) 시 사용된다. 각 파일에 대해:
 *   1) 새 fio_file 할당
 *   2) 파일명과 파일 유형 복사
 *   3) 잠금 객체 공유 (같은 파일에 대한 접근 동기화)
 */
void dup_files(struct thread_data *td, struct thread_data *org)
{
	struct fio_file *f;
	unsigned int i;

	dprint(FD_FILE, "dup files: %d\n", org->files_index);

	if (!org->files)
		return;

	td->files = calloc(org->files_index, sizeof(f));

	if (td->o.file_lock_mode != FILE_LOCK_NONE)
		td->file_locks = malloc(org->files_index);

	assert(org->files_index >= org->o.nr_files);
	for_each_file(org, f, i) {
		struct fio_file *__f;

		__f = alloc_new_file(td);

		if (f->file_name) {
			if (td_ioengine_flagged(td, FIO_NOFILEHASH))
				__f->file_name = strdup(f->file_name);
			else
				__f->file_name = smalloc_strdup(f->file_name);

			/* can't handle smalloc failure from here */
			assert(__f->file_name);
			__f->filetype = f->filetype;
		}

		/* [한국어] 잠금 객체 공유: 같은 파일을 사용하는 모든 잡이 동일한 잠금 사용 */
		if (td->o.file_lock_mode == FILE_LOCK_EXCLUSIVE)
			__f->lock = f->lock;
		else if (td->o.file_lock_mode == FILE_LOCK_READWRITE)
			__f->rwlock = f->rwlock;

		td->files[i] = __f;
	}
}

/*
 * Returns the index that matches the filename, or -1 if not there
 */
/* [한국어] 파일명으로 파일 인덱스를 조회. 없으면 -1 반환 */
int get_fileno(struct thread_data *td, const char *fname)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i)
		if (!strcmp(f->file_name, fname))
			return i;

	return -1;
}

/*
 * For log usage, where we add/open/close files automatically
 */
/* [한국어] 파일 해제 및 리소스 정리 (로그 사용 등 자동 파일 관리용) */
void free_release_files(struct thread_data *td)
{
	free_fs_list(td);

	close_files(td);
	td->o.nr_files = 0;
	td->o.open_files = 0;
	td->files_index = 0;
}

/*
 * [한국어] 파일 상태를 초기 상태로 리셋
 *
 * 모든 I/O 방향의 last_pos를 file_offset으로, last_start를 -1ULL로 초기화한다.
 * axmap이면 리셋, LFSR이면 시드로 재초기화한다.
 * ZBD 파일도 리셋한다.
 */
void fio_file_reset(struct thread_data *td, struct fio_file *f)
{
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		f->last_pos[i] = f->file_offset;
		f->last_start[i] = -1ULL;
	}

	if (fio_file_axmap(f))
		axmap_reset(f->io_axmap);
	else if (fio_file_lfsr(f))
		lfsr_reset(&f->lfsr, td->rand_seeds[FIO_RAND_BLOCK_OFF]);

	zbd_file_reset(td, f);
}

/*
 * [한국어] 모든 파일의 I/O가 완료되었는지 확인
 * 모든 파일에 done 플래그가 설정되어 있으면 true 반환
 */
bool fio_files_done(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	for_each_file(td, f, i)
		if (!fio_file_done(f))
			return false;

	return true;
}

/* free memory used in initialization phase only */
/* [한국어] 초기화 단계에서만 사용된 메모리 해제 (할당 완료 목록 정리) */
void filesetup_mem_free(void)
{
	free_already_allocated();
}

/*
 * This function is for platforms which support direct I/O but not O_DIRECT.
 */
/*
 * [한국어] O_DIRECT를 지원하지 않는 플랫폼에서 Direct I/O 설정
 *
 * 일부 플랫폼(예: Solaris)에서는 O_DIRECT 대신 별도의 ioctl이나
 * fcntl로 Direct I/O를 설정해야 한다. 이 함수가 그 역할을 한다.
 *
 * 반환값: 0=성공, -1=실패
 */
int fio_set_directio(struct thread_data *td, struct fio_file *f)
{
#ifdef FIO_OS_DIRECTIO
	int ret = fio_set_odirect(f);

	if (ret) {
		td_verror(td, ret, "fio_set_directio");
#if defined(__sun__)
		if (ret == ENOTTY) { /* ENOTTY suggests RAW device or ZFS */
			log_err("fio: doing directIO to RAW devices or ZFS not supported\n");
		} else {
			log_err("fio: the file system does not seem to support direct IO\n");
		}
#else
		log_err("fio: the file system does not seem to support direct IO\n");
#endif
		return -1;
	}

	return 0;
#else
	log_err("fio: direct IO is not supported on this host operating system\n");
	return -1;
#endif
}

#ifdef CONFIG_SYNCFS
/*
 * [한국어] syncfs용 파일 시스템 디렉토리 열기
 *
 * end_syncfs 옵션 사용 시, 각 파일 시스템 마운트 포인트를 디렉토리로 열어서
 * dirfd()를 통해 syncfs() 시스템 콜에 사용할 fd를 얻는다.
 *
 * 반환값: 0=성공, -1=실패
 */
int fio_open_fs(struct thread_data *td, struct fio_mount *fm)
{
	struct fio_file *f;

	assert(!fm->f);
	assert(!fm->dir);

	f = alloc_new_file(td);
	if (!f)
		return -1;

	if (td_ioengine_flagged(td, FIO_NOFILEHASH))
		f->file_name = strdup(fm->base);
	else
		f->file_name = smalloc_strdup(fm->base);

	/* [한국어] 디렉토리를 열어서 DIR 포인터 획득 */
	fm->dir = opendir(fm->base);
	if (!fm->dir) {
		log_err("fio: open dir %s failed (%s)\n",
			fm->base, strerror(errno));
		return -1;
	}

	/* [한국어] 디렉토리 타입 파일로 설정하고 열림 상태 표시 */
	f->filetype = FIO_TYPE_DIR;
	f->fd = dirfd(fm->dir);   /* DIR에서 fd 추출 */
	fio_file_set_open(f);

	fm->f = f;

	return 0;
}

/*
 * [한국어] syncfs용 파일 시스템 디렉토리 닫기
 * opendir로 열었던 디렉토리를 닫고 관련 리소스를 해제한다.
 */
void fio_close_fs(struct fio_mount *fm)
{
	struct fio_file *f = fm->f;

	assert(f && fio_file_open(f));
	assert(f->filetype == FIO_TYPE_DIR);
	assert(fm->dir);

	closedir(fm->dir);
	fm->dir = NULL;

	fio_file_clear_open(f);
	fio_file_free(f);
	fm->f = NULL;
}
#endif
