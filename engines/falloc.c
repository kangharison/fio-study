/*
 * [한국어 설명] falloc I/O 엔진 구현 (falloc.c)
 *
 * === 엔진 개요 ===
 * fallocate() 시스템 호출을 사용하여 파일 공간을 사전 할당하는 엔진이다.
 * 실제 데이터를 쓰지 않고 파일 시스템에 블록을 할당하므로 공간 할당 성능을 측정할 수 있다.
 * READ는 FALLOC_FL_KEEP_SIZE, WRITE는 크기 확장 fallocate,
 * TRIM은 FALLOC_FL_PUNCH_HOLE로 동작한다.
 *
 * === 사용하는 시스템 호출/라이브러리 ===
 * fallocate(2), open(2), close(2)
 *
 * === fio에서의 사용법 ===
 * --ioengine=falloc 옵션으로 선택
 *
 * === 구현하는 주요 콜백 ===
 * - .queue: fio_fallocate_queue (fallocate 호출 수행)
 * - .open_file: open_file (O_CREAT|O_RDWR로 파일 열기, TRIM 지원)
 * - .close_file / .get_file_size: generic 콜백 사용
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
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "../fio.h"
#include "../filehash.h"

/*
 * generic_open_file is not appropriate because does not allow to perform
 * TRIM in to file
 */
static int open_file(struct thread_data *td, struct fio_file *f)
{
	int from_hash = 0;

	dprint(FD_FILE, "fd open %s\n", f->file_name);

	if (f->filetype != FIO_TYPE_FILE && f->filetype != FIO_TYPE_BLOCK) {
		log_err("fio: only files and blockdev are supported fallocate \n");
		return 1;
	}
	if (!strcmp(f->file_name, "-")) {
		log_err("fio: can't read/write to stdin/out\n");
		return 1;
	}

open_again:
	from_hash = file_lookup_open(f, O_CREAT|O_RDWR);

	if (f->fd == -1) {
		char buf[FIO_VERROR_SIZE];
		int e = errno;

		snprintf(buf, sizeof(buf), "open(%s)", f->file_name);
		td_verror(td, e, buf);
	}

	if (!from_hash && f->fd != -1) {
		if (add_file_hash(f)) {
			int fio_unused ret;

			/*
			 * OK to ignore, we haven't done anything with it
			 */
			ret = generic_close_file(td, f);
			goto open_again;
		}
	}

	return 0;
}

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE     0x01 /* default is extend size */
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE    0x02 /* de-allocates range */
#endif

static enum fio_q_status fio_fallocate_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;
	int flags = 0;

	fio_ro_check(td, io_u);

	if (!ddir_sync(io_u->ddir)) {
		if (io_u->ddir == DDIR_READ)
			flags = FALLOC_FL_KEEP_SIZE;
		else if (io_u->ddir == DDIR_WRITE)
			flags = 0;
		else if (io_u->ddir == DDIR_TRIM)
			flags = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

		ret = fallocate(f->fd, flags, io_u->offset, io_u->xfer_buflen);
	} else {
		ret = do_io_u_sync(td, io_u);
	}

	if (ret)
		io_u->error = errno;

	return FIO_Q_COMPLETED;
}

static struct ioengine_ops ioengine = {
	.name		= "falloc",
	.version	= FIO_IOOPS_VERSION,
	.queue		= fio_fallocate_queue,
	.open_file	= open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_SYNCFS,
};

static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
