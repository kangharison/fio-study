/*
 * [한국어 설명] falloc I/O 엔진 구현 (falloc.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fallocate(2) 시스템 호출을 I/O 동작으로 모사하는 fio 엔진이다.
 * 실제 데이터 전송 없이 파일 시스템에 블록을 할당/해제함으로써, 파일 시스템의
 * 공간 할당/해제(특히 extent 관리, persistent preallocation, hole punching) 성능을
 * 측정하는 것이 목적이다. READ 방향은 FALLOC_FL_KEEP_SIZE 플래그로 파일 크기를
 * 유지한 채 블록만 예약하고, WRITE 방향은 플래그 0으로 크기 확장을 수행하며,
 * TRIM 방향은 FALLOC_FL_PUNCH_HOLE로 구간 블록을 반환한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 실행 흐름에서 td_io_queue() 시점에 ops->queue로 본 파일의
 * fio_fallocate_queue가 호출된다. 동기 엔진(FIO_SYNCIO)이므로 queue 내에서
 * fallocate(2)가 즉시 수행되고 FIO_Q_COMPLETED가 반환된다. 파일 오픈 경로는
 * generic_open_file이 아닌 본 파일의 open_file()로 오버라이드되는데, 이는
 * TRIM(PUNCH_HOLE)을 수행하려면 O_RDWR가 필수이기 때문이다. 실행 컨텍스트는
 * 잡 스레드(호스트 유저스페이스)이고, 커널은 VFS → ext4/xfs 등 FS의
 * fallocate 핸들러로 요청을 전달한다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위: ioengines.c(td_io_queue/td_io_open_file), filehash.c(file_lookup_open,
 *   add_file_hash)에서 호출된다.
 * - 하위: fallocate(2) 커널 API만을 주 호출로 사용하고, sync 계열은 공용
 *   do_io_u_sync에 위임한다. 파일 닫기/크기는 filesetup.c의 generic_close_file,
 *   generic_get_file_size를 그대로 재사용한다.
 * - 공유 자료구조: fio_file(fd, file_name, filetype), io_u(offset/xfer_buflen/ddir/
 *   error)를 입출력 창구로 사용한다.
 * - 파일 해시: 같은 경로의 중복 open을 피하기 위해 filehash의 해시 테이블에
 *   fd를 등록/검색한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - open_file(): O_CREAT|O_RDWR로 파일을 열고 filehash에 등록. generic 대신 사용.
 * - fio_fallocate_queue(): ddir별로 fallocate 플래그를 선택해 호출하는 메인 콜백.
 * - ioengine: plugin vtable — name/version/queue/open_file/close_file/get_file_size/
 *   flags(FIO_SYNCIO|FIO_SYNCFS)로 구성.
 * - fio_syncio_register/unregister(): ELF ctor/dtor에서 레지스트리 등록/해제.
 */

/*
 * falloc: ioengine for https://git.kernel.org/pub/scm/linux/kernel/git/axboe/fio
 *
 * IO engine that does regular fallocate to simulate data transfer
 * as fio ioengine.
 * DDIR_READ  does fallocate(,mode = FALLOC_FL_KEEP_SIZE,)
 * DDIR_WRITE does fallocate(,mode = 0) : fallocate with size extension
 * DDIR_TRIM  does fallocate(,mode = FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE)
 *
 */
#include <stdio.h>          /* [한국어] snprintf 등 에러 메시지 포맷팅을 위해 포함. */
#include <errno.h>          /* [한국어] fallocate/open 실패 시 errno를 io_u->error/td_verror에 전달하기 위해. */
#include <fcntl.h>          /* [한국어] O_CREAT/O_RDWR 플래그 상수와 fallocate(2), FALLOC_FL_* 매크로를 위해. */

#include "../fio.h"         /* [한국어] thread_data/io_u/fio_file/ioengine_ops와 fio_ro_check/do_io_u_sync 등 공용 헬퍼. */
#include "../filehash.h"    /* [한국어] 여러 잡이 같은 파일을 공유할 때 fd를 공유하기 위한 해시 테이블 API(file_lookup_open/add_file_hash). */

/*
 * generic_open_file is not appropriate because does not allow to perform
 * TRIM in to file
 */
/*
 * [한국어]
 * open_file - falloc 엔진 전용 파일 열기 콜백
 *
 * @td: 잡 컨텍스트. td_verror로 에러 보고 시 사용.
 * @f:  열 파일. f->file_name/filetype이 사전에 채워져 있어야 하며, 반환 시
 *      f->fd가 설정되거나 실패(-1)로 남는다.
 * @return: 0=성공, 1=실패. 상위 td_io_open_file은 실패 시 이 파일을 drop.
 *
 * generic_open_file 대신 커스텀 구현이 필요한 이유: TRIM(FALLOC_FL_PUNCH_HOLE)
 * 경로를 지원하려면 파일을 O_RDWR로 열어야 하고, 경우에 따라 신규 생성이 필요해
 * O_CREAT도 필수다. 또한 본 엔진은 정규 파일과 블록 디바이스만 지원하므로
 * 그 외 타입(파이프, 문자 디바이스 등)은 명시적으로 거절한다.
 *
 * 동작 단계:
 *  1) filetype 검사(정규 파일/블록 디바이스만 허용).
 *  2) stdin/stdout 경로("-") 차단.
 *  3) filehash에서 기존 오픈 검색 또는 신규 open. 성공 시 fd 설정.
 *  4) 해시에 없으면 add_file_hash 시도. 경쟁으로 실패하면 닫고 open_again으로
 *     재시도(다른 스레드가 이미 해시에 추가했을 수 있음).
 *
 * 실행 컨텍스트: 잡 스레드(잡 시작 시 파일 오픈 단계).
 *
 * 호출 체인: td_io_open_file() → [open_file] → file_lookup_open() → open(2)
 *                                            → add_file_hash() / generic_close_file()
 */
static int open_file(struct thread_data *td, struct fio_file *f)
{
	int from_hash = 0;  /* [한국어] file_lookup_open이 기존 해시 엔트리를 재사용했으면 1, 신규 open이면 0. 중복 해시 등록 방지에 쓰인다. */

	dprint(FD_FILE, "fd open %s\n", f->file_name);  /* [한국어] FD_FILE 카테고리의 디버그 트레이스 — fio --debug=file로 활성화. */

	if (f->filetype != FIO_TYPE_FILE && f->filetype != FIO_TYPE_BLOCK) {  /* [한국어] fallocate는 정규 파일과 블록 디바이스에만 유효. 그 외 타입은 조기 차단. */
		log_err("fio: only files and blockdev are supported fallocate \n");  /* [한국어] 사용자에게 엔진 한계를 알림. */
		return 1;  /* [한국어] 파일 오픈 실패로 보고 → 상위가 이 파일을 잡에서 제외. */
	}
	if (!strcmp(f->file_name, "-")) {  /* [한국어] 특수 경로 "-"는 fio에서 stdin/stdout 의미. fallocate 대상 불가. */
		log_err("fio: can't read/write to stdin/out\n");
		return 1;
	}

open_again:
	from_hash = file_lookup_open(f, O_CREAT|O_RDWR);  /* [한국어] filehash에서 동일 경로의 기존 fd를 찾고, 없으면 open(2)로 생성. O_RDWR는 TRIM을 위한 필수 조건, O_CREAT는 공간 할당 전 파일이 없어도 생성. */

	if (f->fd == -1) {  /* [한국어] open 실패(예: 권한/공간/ENOENT) — errno 기반으로 에러 전달. */
		char buf[FIO_VERROR_SIZE];   /* [한국어] td_verror용 메시지 버퍼. */
		int e = errno;                /* [한국어] snprintf가 errno를 덮어쓸 수 있어 즉시 복사. */

		snprintf(buf, sizeof(buf), "open(%s)", f->file_name);  /* [한국어] 어떤 경로에서 실패했는지 맥락 제공. */
		td_verror(td, e, buf);   /* [한국어] 잡 단위 에러 기록 — verror_str/terminal 출력에 사용. */
	}

	if (!from_hash && f->fd != -1) {  /* [한국어] 해시에서 온 게 아니고 open이 성공한 경우에만 해시에 추가 시도. */
		if (add_file_hash(f)) {    /* [한국어] 다른 스레드가 먼저 같은 경로를 해시에 등록했다면 논0을 반환. */
			int fio_unused ret;  /* [한국어] 반환값 미사용 경고 억제용 attribute 래퍼. */

			/*
			 * OK to ignore, we haven't done anything with it
			 */
			ret = generic_close_file(td, f);  /* [한국어] 우리가 방금 연 fd를 닫는다. 아직 I/O를 하지 않았으므로 결과 무시 가능. */
			goto open_again;  /* [한국어] 해시에 이미 있는 fd를 재사용하도록 위로 돌아가 file_lookup_open 재실행. */
		}
	}

	return 0;   /* [한국어] fd가 -1이어도 여기서는 0 반환 — 상위가 f->fd를 보고 판정. (기존 동작 유지) */
}

/* [한국어] FALLOC_FL_KEEP_SIZE: glibc/커널 헤더가 구식이어서 상수가 없을 때 폴백 정의.
 * 값 0x01은 Linux UAPI(include/uapi/linux/falloc.h)에 고정. 파일 i_size를 변경하지
 * 않고 블록만 할당. 설정자: 정적 매크로. 읽는 자: fio_fallocate_queue의 flags. */
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE     0x01 /* default is extend size */
#endif
/* [한국어] FALLOC_FL_PUNCH_HOLE: 구간 블록을 FS에서 반환(구멍 내기). 반드시
 * FALLOC_FL_KEEP_SIZE와 함께 사용. 값 0x02도 Linux UAPI 고정. */
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE    0x02 /* de-allocates range */
#endif

/*
 * [한국어]
 * fio_fallocate_queue - falloc 엔진의 I/O 제출(queue) 콜백
 *
 * @td:   잡 컨텍스트. read-only 검증과 sync 방향 위임에 사용.
 * @io_u: 처리할 I/O 유닛. file/offset/xfer_buflen/ddir를 입력으로 사용하고
 *        error 슬롯에 실패 errno를 기록.
 * @return: 항상 FIO_Q_COMPLETED (동기 엔진 관례).
 *
 * 방향별 동작:
 *   DDIR_READ  : fallocate(FALLOC_FL_KEEP_SIZE) — 파일 크기 유지한 채 예약.
 *   DDIR_WRITE : fallocate(0) — 필요 시 i_size 확장.
 *   DDIR_TRIM  : fallocate(PUNCH_HOLE|KEEP_SIZE) — 구간 블록 반환.
 *   ddir_sync(): do_io_u_sync 위임(fsync/fdatasync/sync_file_range).
 *
 * 실행 컨텍스트: 잡 스레드. ftruncate 엔진과 마찬가지로 fio의 SYNCIO 경로.
 *
 * 호출 체인: td_io_queue() → [fio_fallocate_queue] → fallocate(2) | do_io_u_sync()
 */
static enum fio_q_status fio_fallocate_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct fio_file *f = io_u->file;   /* [한국어] 대상 파일 — fd를 꺼내기 위함. */
	int ret;                           /* [한국어] fallocate/do_io_u_sync 반환값. 0=성공. 초기화는 모든 분기에서 대입되므로 불필요. */
	int flags = 0;                     /* [한국어] fallocate 플래그. 기본 0(크기 확장 허용). ddir에 따라 덮어씀. */

	fio_ro_check(td, io_u);            /* [한국어] read-only 잡이 write/trim을 시도하는 구성 오류 탐지. */

	if (!ddir_sync(io_u->ddir)) {       /* [한국어] 일반 데이터 방향(READ/WRITE/TRIM) 처리 경로. */
		if (io_u->ddir == DDIR_READ)
			flags = FALLOC_FL_KEEP_SIZE;                         /* [한국어] 읽기 = 블록 예약만, 크기 불변. */
		else if (io_u->ddir == DDIR_WRITE)
			flags = 0;                                           /* [한국어] 쓰기 = 일반 fallocate(크기 확장). */
		else if (io_u->ddir == DDIR_TRIM)
			flags = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;  /* [한국어] 트림 = 구멍 내기. PUNCH_HOLE은 항상 KEEP_SIZE 필요(커널 강제). */

		ret = fallocate(f->fd, flags, io_u->offset, io_u->xfer_buflen);  /* [한국어] fallocate(2): 커널에 (offset,len) 구간 블록 할당/해제 요청. ext4/xfs/btrfs 등에서 지원. */
	} else {
		ret = do_io_u_sync(td, io_u);  /* [한국어] sync 계열(DDIR_SYNC 등)은 공통 헬퍼로. */
	}

	if (ret)
		io_u->error = errno;           /* [한국어] 실패 시 errno를 io_u에 실어 상위(통계/리포트)로 전달. */

	return FIO_Q_COMPLETED;            /* [한국어] 동기 완료 신호 — 호출자가 즉시 completion 처리로 진행. */
}

/*
 * [한국어] ioengine - falloc 엔진 플러그인 vtable. 각 필드는 fio 코어가
 * 엔진 동작을 제어하는 데 사용하는 콜백/메타데이터다.
 */
static struct ioengine_ops ioengine = {
	.name		= "falloc",
	/* [한국어] 엔진 식별자. --ioengine=falloc로 선택. 설정자: 정적. 읽는 자: ioengines.c의 이름 매칭. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전. 코어-엔진 구조체 호환성 확인. 불일치 시 등록 거부. */

	.queue		= fio_fallocate_queue,
	/* [한국어] I/O 제출 콜백. 본 엔진의 핵심 로직. td_io_queue가 호출. */

	.open_file	= open_file,
	/* [한국어] generic 대신 커스텀 open. TRIM을 위한 O_RDWR 필수 때문. */

	.close_file	= generic_close_file,
	/* [한국어] 닫기는 공용 close(2) 래퍼. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 크기 질의는 공용 fstat 래퍼. */

	.flags		= FIO_SYNCIO | FIO_SYNCFS,
	/* [한국어] FIO_SYNCIO: queue에서 즉시 완료. FIO_SYNCFS: sync 계열 ddir 처리 가능. */
};

/*
 * [한국어] fio_syncio_register - ELF ctor에서 본 엔진을 레지스트리에 추가.
 * 호출 체인: dynamic loader → [이 함수] → register_ioengine().
 */
static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] ioengines.c의 전역 리스트에 append. */
}

/*
 * [한국어] fio_syncio_unregister - ELF dtor에서 레지스트리 해제.
 * 호출 체인: process exit → [이 함수] → unregister_ioengine().
 */
static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] dangling reference 방지. */
}
