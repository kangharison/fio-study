/*
 * [한국어 설명] ftruncate I/O 엔진 구현 (ftruncate.c)
 *
 * === 엔진 개요 ===
 * ftruncate() 시스템 호출을 사용하여 파일 크기 변경(truncation) 성능을 측정하는 엔진이다.
 * WRITE 방향에서만 ftruncate를 수행하며, 실제 데이터 전송 없이 파일 크기를 변경한다.
 * 가장 간단한 엔진 중 하나로, queue 콜백만 구현한다.
 *
 * === 사용하는 시스템 호출/라이브러리 ===
 * ftruncate(2)
 *
 * === fio에서의 사용법 ===
 * --ioengine=ftruncate 옵션으로 선택
 *
 * === 구현하는 주요 콜백 ===
 * - .queue: fio_ftruncate_queue (ftruncate 호출)
 * - .open_file / .close_file / .get_file_size: generic 콜백 사용
 */

/*
 * ftruncate: ioengine for https://git.kernel.org/pub/scm/linux/kernel/git/axboe/fio
 *
 * IO engine that does regular truncates to simulate data transfer
 * as fio ioengine.
 * DDIR_WRITE does ftruncate
 *
 */
#include <errno.h>
#include <unistd.h>

#include "../fio.h"

static enum fio_q_status fio_ftruncate_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret = 0;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_WRITE)
		ret = ftruncate(f->fd, io_u->offset);
	else if (ddir_sync(io_u->ddir))
		ret = do_io_u_sync(td, io_u);
	else
		io_u->error = EINVAL;

	if (ret)
		io_u->error = errno;

	return FIO_Q_COMPLETED;
}

static struct ioengine_ops ioengine = {
	.name		= "ftruncate",
	.version	= FIO_IOOPS_VERSION,
	.queue		= fio_ftruncate_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO | FIO_FAKEIO | FIO_SYNCFS,
};

static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
