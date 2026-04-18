/*
 * [한국어 설명] mmap I/O 엔진 구현 (mmap.c)
 *
 * === 파일의 역할 ===
 * 일반 파일(혹은 블록 디바이스)을 mmap(2)으로 사용자 주소 공간에 매핑한 뒤,
 * read(2)/write(2) 없이 memcpy로만 I/O를 수행하는 fio 동기 엔진이다. 이로써
 * 페이지 캐시를 통한 파일 액세스 경로 전체(파일시스템 페이지 폴트 · readahead ·
 * writeback)를 벤치마크할 수 있다. 32비트 환경에서 가상 주소 공간 고갈을
 * 피하기 위해 총 1GiB(MMAP_TOTAL_SZ)를 잡별 파일 수로 나눠 상한(mmap_map_size)을
 * 두고, 파일이 너무 크면 partial_mmap 플래그를 세워 매 요청마다 부분 매핑으로
 * 다시 mmap한다. THP(Transparent Huge Pages) 지원 빌드에서는 thp=1 옵션으로
 * MADV_HUGEPAGE 힌트를 주고 MAP_PRIVATE로 매핑한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend 잡 루프 → load_ioengine("mmap") → init → open_file(FD 개방 + fmd 부착)
 * → (prep → queue)* → close_file → cleanup 순으로 호출된다. 이 엔진은 FIO_SYNCIO
 * 이므로 queue() 내부에서 memcpy/msync로 I/O를 즉시 완료하고 FIO_Q_COMPLETED를
 * 반환한다. 실행 컨텍스트는 각 잡 스레드이며, mmap된 영역의 페이지 폴트 처리는
 * 리눅스 VM/파일시스템 레이어가 담당한다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h: thread_data/fio_file/io_u/ioengine_ops, page_size/page_mask 글로벌.
 * - optgroup.h: FIO_OPT_G_MMAP 옵션 그룹.
 * - verify.h: VERIFY_* 상수 — 쓰기 후 검증 시 읽기 보호도 필요.
 * - sys/mman.h: mmap/munmap/msync/madvise/posix_madvise.
 * - generic_open_file/close_file/get_file_size: fio 공용 파일 헬퍼.
 * - 공유 상태: FILE_ENG_DATA(f) = struct fio_mmap_data — 파일당 매핑 상태.
 *   mmap_map_size는 프로세스 전역(엔진 init이 잡당 재계산).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_mmap_data: 매핑 시작 주소/크기/오프셋.
 * - fio_mmapio_init():   페이지 정렬 검증 + 파일당 매핑 크기 상한 산출.
 * - fio_mmapio_open_file()/close_file(): FD + fmd 생성/해제.
 * - fio_mmap_file():     실제 mmap + madvise 튜닝 + 실패 롤백.
 * - fio_mmapio_prep_full()/prep_limited(): 전체/부분 매핑 전략.
 * - fio_mmapio_prep():   요청이 현재 매핑 범위 밖이면 재매핑; io_u->mmap_data 결정.
 * - fio_mmapio_queue():  memcpy(READ/WRITE), msync(SYNC), do_io_u_trim(TRIM), odirect 후처리.
 */

/*
 * mmap engine
 *
 * IO engine that reads/writes from files by doing memcpy to/from
 * a memory mapped region of the file.
 *
 */
#include <stdio.h>       /* [한국어] log_err/log_info의 printf-계열 포매팅을 공급. */
#include <stdlib.h>      /* [한국어] calloc(fmd 할당) / free(fmd 해제) — malloc 계열. */
#include <errno.h>       /* [한국어] errno 전역과 EIO/EINVAL — mmap/munmap/msync/madvise 실패 경로 공용. */
#include <sys/mman.h>    /* [한국어] mmap(2)/munmap(2)/msync(2)/madvise(2)/posix_madvise(3),
                            PROT_READ/WRITE, MAP_SHARED/PRIVATE, MS_SYNC, POSIX_MADV_* 매크로 공급. */

#include "../fio.h"       /* [한국어] thread_data/fio_file/io_u/ioengine_ops 등 코어 자료구조, page_size/page_mask 전역. */
#include "../optgroup.h"  /* [한국어] FIO_OPT_C_ENGINE/FIO_OPT_G_MMAP — 옵션 카테고리/그룹 상수. */
#include "../verify.h"    /* [한국어] VERIFY_NONE 등 verify 모드 상수 — PROT_READ 추가 판단에 사용. */

/*
 * Limits us to 1GiB of mapped files in total on 32-bit architectures
 */
/* [한국어] 32비트 주소공간의 가상 메모리 포화를 피하기 위한 총 매핑 상한.
 * 잡별 nr_files로 나눠 파일당 실효 매핑 최댓값(mmap_map_size)을 계산한다. */
#define MMAP_TOTAL_SZ	(1 * 1024 * 1024 * 1024UL)

/* [한국어] 파일당 최대 매핑 크기(바이트). fio_mmapio_init에서 MMAP_TOTAL_SZ/nr_files로 설정.
 * 설정자: fio_mmapio_init(). 읽는 자: prep_limited/prep_full.
 * 동기화: 잡 시작 이후 불변. 여러 잡이 같은 프로세스에 있을 수 있으나 값은 잡 독립. */
static unsigned long mmap_map_size;

/*
 * [한국어] 파일별 mmap 매핑 상태를 저장하는 구조체.
 * FILE_ENG_DATA(f)로 부착되어 해당 잡 스레드 단독 접근(락 불필요).
 */
struct fio_mmap_data {
	void *mmap_ptr;
	/* [한국어] 현재 파일에 대한 mmap 영역의 시작 가상 주소.
	 * 설정자: fio_mmap_file()의 mmap 성공 시 저장, 해제/실패 시 NULL.
	 * 읽는 자: fio_madvise_file, fio_mmapio_prep(io_u->mmap_data 계산), queue, close.
	 * 값 범위: 유효 매핑 주소 또는 NULL. 동기화: 잡 스레드 단독. */

	size_t mmap_sz;
	/* [한국어] 현재 유효 매핑 영역의 바이트 크기(부분 매핑 시 io_u 단위일 수 있음).
	 * 설정자: prep_full/prep_limited에서 결정. 읽는 자: munmap/msync/prep 범위 체크. */

	off_t mmap_off;
	/* [한국어] 파일 내에서 매핑이 시작되는 오프셋. 전체 매핑이면 f->file_offset,
	 * 부분 매핑이면 io_u->offset.
	 * 설정자: prep_full/prep_limited. 읽는 자: prep이 io_u->offset - mmap_off로
	 * 가상 주소 산출. */
};

#ifdef CONFIG_HAVE_THP
/*
 * [한국어] CONFIG_HAVE_THP 빌드 시에만 존재하는 엔진 옵션 구조체.
 * thp=1이면 MADV_HUGEPAGE 힌트 + MAP_PRIVATE 매핑을 사용해 커널 VM이 2MiB/1GiB
 * huge page로 병합하도록 유도한다(Transparent Huge Pages).
 * option_struct_size = sizeof(struct mmap_options)로 ioengine.options 테이블과 연동.
 */
struct mmap_options {
	void *pad;
	/* [한국어] 옵션 파서의 off1==0 판정 회피용 더미 포인터.
	 * 설정자: 없음(의미 있는 값 할당되지 않음).
	 * 읽는 자: 없음. 단지 뒤따르는 실제 옵션 필드의 offsetof()가 0이 되지 않게 함.
	 * 값 범위: 미정의(잡 시작 시 calloc 0).
	 * 동기화: 읽히지 않으므로 동기화 불필요. */

	unsigned int thp;
	/* [한국어] Transparent Huge Pages 사용 여부 플래그(0 또는 1).
	 * 설정자: fio 옵션 파서가 "thp=1" 지정 시 이 필드에 기록.
	 * 읽는 자: fio_madvise_file()의 MADV_HUGEPAGE 호출, fio_mmap_get_shared()의
	 *          MAP_PRIVATE 선택 분기.
	 * 값 범위: 0(기본, MAP_SHARED), 1(MAP_PRIVATE + MADV_HUGEPAGE).
	 * 동기화: 잡 시작 이후 불변, 잡 스레드 단독 읽기. */
};

static struct fio_option options[] = {
	{
		.name	= "thp",                                  /* [한국어] 잡 파일/CLI 옵션 이름 (--thp). */
		.lname	= "Transparent Huge Pages",               /* [한국어] help 출력용 긴 이름. */
		.type	= FIO_OPT_INT,                            /* [한국어] 정수 타입(0/1) — FIO_OPT_BOOL 대신 INT 사용(역사적 이유). */
		.off1	= offsetof(struct mmap_options, thp),     /* [한국어] mmap_options.thp 필드의 바이트 오프셋 — pad 덕에 non-zero. */
		.help	= "Memory Advise Huge Page",               /* [한국어] --enghelp 출력 문구. */
		.category = FIO_OPT_C_ENGINE,                      /* [한국어] 엔진 전용 옵션 카테고리. */
		.group	= FIO_OPT_G_MMAP,                         /* [한국어] mmap 엔진 옵션 그룹(optgroup.h 정의). */
	},
	{
		.name = NULL,                                      /* [한국어] 배열 종료 센티널 — 파서가 여기서 루프 종료. */
	},
};
#endif

/*
 * [한국어]
 * fio_madvise_file - mmap된 영역에 대해 madvise 힌트 적용
 *
 * THP(Transparent Huge Pages) 옵션이 설정되면 MADV_HUGEPAGE를 적용하고,
 * fadvise_hint 설정에 따라 POSIX_MADV_RANDOM/SEQUENTIAL 힌트를 적용한다.
 * 커널의 페이지 캐시 예측 알고리즘에 접근 패턴을 알려준다.
 */
/*
 * [한국어]
 * fio_madvise_file - mmap된 영역에 madvise/posix_madvise 힌트 적용.
 *                   접근 패턴(랜덤/순차)과 THP 요구를 커널 VM에 알려준다.
 * @return: true 성공, false 실패(td_verror로 에러 기록).
 * 호출 체인: fio_mmap_file → [fio_madvise_file] → madvise/posix_madvise.
 */
static bool fio_madvise_file(struct thread_data *td, struct fio_file *f,
			     size_t length)

{
	int flags;
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);
#ifdef CONFIG_HAVE_THP
	struct mmap_options *o = td->eo;

	/* Ignore errors on this optional advisory */
	/* [한국어] THP 요청 시 MADV_HUGEPAGE — 실패해도 조용히 무시(최적화 힌트) */
	if (o->thp)
		madvise(fmd->mmap_ptr, length, MADV_HUGEPAGE);
#endif

	if (!td->o.fadvise_hint)
		return true;  /* [한국어] fadvise 힌트 비활성 — 조기 반환 */

	/* [한국어] fadvise_hint 타입에 따라 POSIX madvise 플래그 결정 */
	if (td->o.fadvise_hint == F_ADV_TYPE)
		flags = td_random(td) ? POSIX_MADV_RANDOM : POSIX_MADV_SEQUENTIAL;
	else if (td->o.fadvise_hint == F_ADV_RANDOM)
		flags = POSIX_MADV_RANDOM;
	else if (td->o.fadvise_hint == F_ADV_SEQUENTIAL)
		flags = POSIX_MADV_SEQUENTIAL;
	else {
		log_err("fio: unknown madvise type %d\n", td->o.fadvise_hint);
		return false;
	}

	if (posix_madvise(fmd->mmap_ptr, length, flags) < 0) {
		td_verror(td, errno, "madvise");
		return false;
	}

	return true;
}

/*
 * [한국어]
 * fio_mmap_get_shared - mmap 플래그(MAP_SHARED vs MAP_PRIVATE) 선택.
 * THP 빌드에서 thp=1이면 익명 huge page 승격 친화적인 MAP_PRIVATE를 사용한다.
 */
#ifdef CONFIG_HAVE_THP
static int fio_mmap_get_shared(struct thread_data *td)
{
	struct mmap_options *o = td->eo;

	if (o->thp)
		return MAP_PRIVATE;   /* [한국어] THP 힌트와 어울리게 PRIVATE */
	return MAP_SHARED;
}
#else
static int fio_mmap_get_shared(struct thread_data *td)
{
	return MAP_SHARED;            /* [한국어] 표준: 쓰기가 파일에 반영되도록 SHARED */
}
#endif

/*
 * [한국어]
 * fio_mmap_file - 파일의 지정 영역을 메모리에 매핑
 *
 * td의 읽기/쓰기 모드에 따라 PROT_READ/PROT_WRITE 플래그를 설정하고,
 * mmap()으로 파일을 메모리에 매핑한다. 매핑 후 madvise 힌트를 적용하고,
 * POSIX_MADV_DONTNEED로 초기 페이지를 비운다.
 * THP 사용 시 MAP_PRIVATE, 아니면 MAP_SHARED로 매핑한다.
 */
static int fio_mmap_file(struct thread_data *td, struct fio_file *f,
			 size_t length, off_t off)
{
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);
	int flags = 0, shared = fio_mmap_get_shared(td);  /* [한국어] MAP_SHARED or PRIVATE */

	/* [한국어] 잡의 rw 모드와 verify 옵션에 따라 PROT_READ/WRITE 결정:
	 *  - rw 혼합 + verify_only 아님 → RW
	 *  - 쓰기 전용 + verify_only 아님 → WRITE; verify 있으면 READ도 필요
	 *  - 그 외(읽기 전용/verify_only) → READ */
	if (td_rw(td) && !td->o.verify_only)
		flags = PROT_READ | PROT_WRITE;
	else if (td_write(td) && !td->o.verify_only) {
		flags = PROT_WRITE;

		if (td->o.verify != VERIFY_NONE)
			flags |= PROT_READ;
	} else
		flags = PROT_READ;

	/* [한국어] 실제 매핑. NULL 힌트는 커널이 주소 선택. MAP_FAILED 체크 필수 */
	fmd->mmap_ptr = mmap(NULL, length, flags, shared, f->fd, off);
	if (fmd->mmap_ptr == MAP_FAILED) {
		fmd->mmap_ptr = NULL;       /* [한국어] 에러 경로에서 munmap 재시도 방지 */
		td_verror(td, errno, "mmap");
		goto err;
	}

	if (!fio_madvise_file(td, f, length))
		goto err;                     /* [한국어] madvise 실패는 치명적 — 롤백 */

	/* [한국어] POSIX_MADV_DONTNEED: 현재 페이지 캐시에서 비우기 — 콜드 캐시 재현 */
	if (posix_madvise(fmd->mmap_ptr, length, POSIX_MADV_DONTNEED) < 0) {
		td_verror(td, errno, "madvise");
		goto err;
	}

#ifdef FIO_MADV_FREE
	/* [한국어] 블록 디바이스는 FIO_MADV_FREE로 게으른 해제 힌트(지원 시) */
	if (f->filetype == FIO_TYPE_BLOCK)
		(void) posix_madvise(fmd->mmap_ptr, fmd->mmap_sz, FIO_MADV_FREE);
#endif

err:
	/* [한국어] 에러 발생 + 매핑은 성립했다면 정리로 munmap 수행 */
	if (td->error && fmd->mmap_ptr)
		munmap(fmd->mmap_ptr, length);

	return td->error;
}

/*
 * Just mmap an appropriate portion, we cannot mmap the full extent
 */
/*
 * [한국어]
 * fio_mmapio_prep_limited - 전체 파일을 한 번에 매핑할 수 없을 때 현재 io_u만
 *                           커버하는 부분 매핑으로 대체.
 * @return: 0 성공, EIO(블록 크기가 엔진 상한 초과).
 */
static int fio_mmapio_prep_limited(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);

	if (io_u->buflen > mmap_map_size) {
		log_err("fio: bs too big for mmap engine\n");
		return EIO;
	}

	fmd->mmap_off = io_u->offset;   /* [한국어] 부분 매핑의 시작점은 요청 오프셋 */
	fmd->mmap_sz = io_u->buflen;     /* [한국어] 크기는 요청 길이(한 블록) */

	return fio_mmap_file(td, f, fmd->mmap_sz, fmd->mmap_off);
}

/*
 * Attempt to mmap the entire file
 */
/*
 * [한국어]
 * fio_mmapio_prep_full - 파일 전체(f->io_size)를 한 번 매핑 시도. 32비트에서
 *                        상한 초과 시 partial 모드로 전환하고 EINVAL 반환(호출자가
 *                        prep_limited 경로로 폴백).
 */
static int fio_mmapio_prep_full(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);
	int ret;

	if (fio_file_partial_mmap(f))
		return EINVAL;   /* [한국어] 이미 partial 모드로 전환된 파일은 스킵 */

	/* [한국어] 32비트(size_t<8) + 파일이 잡당 상한 초과 → partial 강제 */
	if (sizeof(size_t) < 8 && f->io_size > mmap_map_size) {
		fio_file_set_partial_mmap(f);
		return EINVAL;
	}

	fmd->mmap_sz = f->io_size;         /* [한국어] 전체 파일 매핑 */
	fmd->mmap_off = f->file_offset;

	ret = fio_mmap_file(td, f, fmd->mmap_sz, fmd->mmap_off);
	if (ret)
		fio_file_set_partial_mmap(f);   /* [한국어] 실패 → 다음부터 부분 매핑 */

	return ret;
}

/*
 * [한국어]
 * fio_mmapio_prep - mmap 엔진의 I/O 준비 콜백
 *
 * I/O 요청이 현재 매핑 범위 내에 있으면 바로 사용하고,
 * 범위를 벗어나면 기존 매핑을 해제하고 새로 매핑한다.
 * 먼저 전체 매핑(prep_full)을 시도하고, 실패하면 제한적 매핑(prep_limited)을 시도한다.
 * io_u->mmap_data에 I/O 대상 메모리 주소를 설정한다.
 *
 * 호출 체인: td_io_prep() → [이 함수] → fio_mmap_file() → mmap(2)
 */
static int fio_mmapio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);
	int ret;

	/*
	 * It fits within existing mapping, use it
	 */
	/* [한국어] 이미 맞는 범위로 매핑돼 있으면 재매핑 없이 주소 계산만 진행 */
	if (io_u->offset >= fmd->mmap_off &&
	    io_u->offset + io_u->buflen <= fmd->mmap_off + fmd->mmap_sz)
		goto done;

	/*
	 * unmap any existing mapping
	 */
	/* [한국어] 기존 매핑이 있으면 먼저 해제(오버랩 매핑 방지) */
	if (fmd->mmap_ptr) {
		if (munmap(fmd->mmap_ptr, fmd->mmap_sz) < 0)
			return errno;
		fmd->mmap_ptr = NULL;
	}

	/* [한국어] 먼저 전체 매핑 시도 → 실패 시 에러 상태 초기화 후 부분 매핑으로 폴백 */
	if (fio_mmapio_prep_full(td, io_u)) {
		td_clear_error(td);
		ret = fio_mmapio_prep_limited(td, io_u);
		if (ret)
			return ret;
	}

done:
	/* [한국어] io_u가 복사 대상으로 쓸 사용자 주소 계산:
	 * base + (요청 절대 오프셋 - 매핑 시작 오프셋) */
	io_u->mmap_data = fmd->mmap_ptr + io_u->offset - fmd->mmap_off;
	return 0;
}

/*
 * [한국어]
 * fio_mmapio_queue - mmap 엔진의 I/O 제출 콜백
 *
 * 읽기는 mmap 영역에서 xfer_buf로 memcpy, 쓰기는 반대로 memcpy를 수행한다.
 * sync 계열은 msync(MS_SYNC)로 디스크에 동기화한다.
 * odirect 모드에서는 추가로 msync + POSIX_MADV_DONTNEED로 캐시를 비운다.
 * 모든 I/O가 동기적으로 완료되므로 항상 FIO_Q_COMPLETED를 반환한다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → memcpy(3)/msync(2)
 */
static enum fio_q_status fio_mmapio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);

	fio_ro_check(td, io_u);   /* [한국어] readonly 잡 WRITE 단속 */

	if (io_u->ddir == DDIR_READ)
		/* [한국어] 매핑→유저 버퍼 memcpy. 첫 접근이면 내부에서 페이지 폴트 발생 */
		memcpy(io_u->xfer_buf, io_u->mmap_data, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_WRITE)
		/* [한국어] 유저 버퍼→매핑 memcpy. 페이지 더티 표시, msync 전엔 미반영 가능 */
		memcpy(io_u->mmap_data, io_u->xfer_buf, io_u->xfer_buflen);
	else if (ddir_sync(io_u->ddir)) {
		/* [한국어] SYNC/DATASYNC: msync(MS_SYNC)로 더티 페이지를 디스크에 내려쓰기 */
		if (msync(fmd->mmap_ptr, fmd->mmap_sz, MS_SYNC)) {
			io_u->error = errno;
			td_verror(td, io_u->error, "msync");
		}
	} else if (io_u->ddir == DDIR_TRIM) {
		/* [한국어] mmap 엔진은 TRIM을 직접 지원하지 않으므로 공용 헬퍼 사용 */
		int ret = do_io_u_trim(td, io_u);

		if (!ret)
			td_verror(td, io_u->error, "trim");
	}


	/*
	 * not really direct, but should drop the pages from the cache
	 */
	/* [한국어] odirect 모드 에뮬레이션: 쓴 직후 즉시 msync + DONTNEED로
	 * 페이지 캐시를 축출해 후속 읽기가 디스크까지 내려가도록 유도 */
	if (td->o.odirect && ddir_rw(io_u->ddir)) {
		if (msync(io_u->mmap_data, io_u->xfer_buflen, MS_SYNC) < 0) {
			io_u->error = errno;
			td_verror(td, io_u->error, "msync");
		}
		if (posix_madvise(io_u->mmap_data, io_u->xfer_buflen, POSIX_MADV_DONTNEED) < 0) {
			io_u->error = errno;
			td_verror(td, io_u->error, "madvise");
		}
	}

	return FIO_Q_COMPLETED;   /* [한국어] 동기 엔진: 즉시 완료 */
}

/*
 * [한국어]
 * fio_mmapio_init - mmap 엔진 초기화 콜백
 *
 * 블록 크기가 페이지 크기의 배수인지 검증하고(odirect/fsync 사용 시),
 * 파일 수에 따라 개별 파일당 최대 매핑 크기를 계산한다.
 */
static int fio_mmapio_init(struct thread_data *td)
{
	struct thread_options *o = &td->o;

	/* [한국어] odirect/fsync 계열 옵션과 비정렬 bs 조합은 의미가 망가지므로 거절 */
	if ((o->rw_min_bs & page_mask) &&
	    (o->odirect || o->fsync_blocks || o->fdatasync_blocks)) {
		log_err("fio: mmap options dictate a minimum block size of "
			"%llu bytes\n", (unsigned long long) page_size);
		return 1;
	}

	/* [한국어] 파일당 매핑 상한 계산 — 32비트에서도 총 1GiB 이하 유지 */
	mmap_map_size = MMAP_TOTAL_SZ / o->nr_files;
	return 0;
}

/*
 * [한국어]
 * fio_mmapio_open_file - mmap 엔진의 파일 열기 콜백
 *
 * generic_open_file로 파일을 열고, fio_mmap_data 구조체를 할당하여
 * FILE_SET_ENG_DATA로 파일에 연결한다. 실제 mmap은 prep 단계에서 수행한다.
 */
static int fio_mmapio_open_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_mmap_data *fmd;
	int ret;

	ret = generic_open_file(td, f);   /* [한국어] 표준 FD 오픈 */
	if (ret)
		return ret;

	fmd = calloc(1, sizeof(*fmd));    /* [한국어] 파일별 상태 할당 */
	if (!fmd) {
		int fio_unused __ret;
		__ret = generic_close_file(td, f);   /* [한국어] OOM 롤백 */
		return 1;
	}

	FILE_SET_ENG_DATA(f, fmd);         /* [한국어] 파일에 부착 — 이후 prep/queue에서 참조 */
	return 0;
}

/*
 * [한국어]
 * fio_mmapio_close_file - mmap 엔진의 파일 닫기 콜백
 *
 * fio_mmap_data를 해제하고 partial_mmap 플래그를 초기화한 후
 * generic_close_file로 파일 디스크립터를 닫는다.
 */
static int fio_mmapio_close_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_mmap_data *fmd = FILE_ENG_DATA(f);

	FILE_SET_ENG_DATA(f, NULL);            /* [한국어] 슬롯 비우기 */
	free(fmd);                               /* [한국어] 상태 해제 */
	fio_file_clear_partial_mmap(f);         /* [한국어] partial 플래그 리셋(재오픈 대비) */

	return generic_close_file(td, f);
}

/*
 * [한국어] mmap 엔진의 ioengine_ops 테이블.
 * ioengines.c::register_ioengine()이 이 포인터를 engine_list에 등록하고,
 * load_ioengine("mmap")이 여기서 .init/.prep/.queue/.open_file 등을 찾아 td->io_ops에 바인딩.
 * 각 필드는 jobdata의 생명주기 훅이며, 코어는 이 구조체의 NULL 여부로 지원 여부를 판단한다.
 */
static struct ioengine_ops ioengine = {
	.name		= "mmap",
	/* [한국어] --ioengine=mmap에서 선택되는 엔진 식별자. engine_list 검색 키. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] fio 코어와 엔진 간 ABI 버전. check_engine_ops()가 불일치 시 로드 거부. */

	.init		= fio_mmapio_init,
	/* [한국어] 잡 시작 시 1회 — bs/페이지 크기 정합성 검증과 파일당 매핑 상한 산출.
	 * 호출자: ioengines.c::td_io_init. cleanup 쌍은 없음(init이 전역만 수정). */

	.prep		= fio_mmapio_prep,
	/* [한국어] 각 io_u에 대해 queue() 직전 호출. 현재 매핑 범위 체크 후 필요 시 재매핑하고
	 *          io_u->mmap_data에 memcpy 대상 주소를 기록.
	 * 호출자: ioengines.c::td_io_prep. */

	.queue		= fio_mmapio_queue,
	/* [한국어] 엔진의 핵심 — memcpy(READ/WRITE) / msync(SYNC) / do_io_u_trim(TRIM) 발행.
	 * FIO_SYNCIO이므로 반환값은 항상 FIO_Q_COMPLETED. */

	.open_file	= fio_mmapio_open_file,
	/* [한국어] generic_open_file로 FD 획득 + fio_mmap_data calloc하여 FILE_SET_ENG_DATA. */

	.close_file	= fio_mmapio_close_file,
	/* [한국어] open_file의 역 — fmd 해제 + partial 플래그 리셋 + generic_close_file. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] stat(2) 기반 파일 크기 질의 기본 구현 위임 — mmap 엔진 고유 로직 불필요. */

	.flags		= FIO_SYNCIO | FIO_NOEXTEND,
	/* [한국어] 엔진 속성 비트:
	 *   FIO_SYNCIO    — queue()에서 즉시 완료되는 동기 엔진 (io_u_mark_submit+complete를
	 *                   코어가 자동 처리, getevents/event 훅 없어도 됨).
	 *   FIO_NOEXTEND  — 파일 자동 확장 금지. mmap은 사전 매핑된 범위만 다루므로 잡 수행 중
	 *                   파일 크기 변화를 허용하지 않음(SIGBUS 위험 회피). */

#ifdef CONFIG_HAVE_THP
	.options	= options,
	/* [한국어] CONFIG_HAVE_THP 빌드 시 thp 옵션 테이블을 코어에 노출. */

	.option_struct_size = sizeof(struct mmap_options),
	/* [한국어] 옵션 저장용 per-job 메모리 크기. 코어가 calloc하여 파서가 off1로 채움. */
#endif
};

/*
 * [한국어] fio 바이너리 링크 시 constructor 속성으로 자동 호출되어 mmap 엔진을
 * ioengines.c의 engine_list에 삽입한다. 이후 --ioengine=mmap이 find_ioengine으로 조회 가능.
 */
static void fio_init fio_mmapio_register(void)
{
	register_ioengine(&ioengine); /* [한국어] engine_list flist_add_tail — 단일 스레드 초기화 경로라 락 불필요. */
}

/*
 * [한국어] destructor 속성 — fio 프로세스 종료 직전 호출되어 engine_list에서 제거.
 * 정적 빌드에서도 릭리스트 정리 목적.
 */
static void fio_exit fio_mmapio_unregister(void)
{
	unregister_ioengine(&ioengine); /* [한국어] flist_del_init로 해제. */
}
