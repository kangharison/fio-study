/*
 * Native Solaris async IO engine
 *
 */

/*
 * [한국어 설명] solarisaio I/O 엔진 구현 (solarisaio.c)
 *
 * === 엔진 개요 ===
 * Solaris 운영체제의 네이티브 비동기 I/O 인터페이스(aioread/aiowrite/aiowait)를 사용하는
 * I/O 엔진이다. POSIX AIO가 아닌 Solaris 고유의 <sys/asynch.h> API를 직접 사용하며,
 * MAXASYNCHIO 상수로 최대 동시 I/O 깊이가 제한된다. 선택적으로 SIGIO 시그널 기반 완료
 * 통지도 지원한다.
 *
 * === 사용하는 시스템 호출/라이브러리 ===
 * aioread(), aiowrite(), aiowait() (Solaris <sys/asynch.h>)
 * fsync(), fdatasync()
 * sigaction() (SIGIO 완료 모드 사용 시)
 *
 * === fio에서의 사용법 ===
 * --ioengine=solarisaio 옵션으로 선택
 *
 * === 구현하는 주요 콜백 ===
 * .init (fio_solarisaio_init) - aio 이벤트 배열 할당 및 최대 깊이 설정
 * .prep (fio_solarisaio_prep) - io_u에 aio 상태 초기화
 * .queue (fio_solarisaio_queue) - aioread/aiowrite로 비동기 I/O 제출
 * .getevents (fio_solarisaio_getevents) - aiowait()으로 완료 이벤트 수집
 * .event (fio_solarisaio_event) - 완료된 io_u 반환
 * .cleanup (fio_solarisaio_cleanup) - 리소스 해제
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "../fio.h"

#include <sys/asynch.h>

struct solarisaio_data {
	struct io_u **aio_events;
	unsigned int aio_pending;
	unsigned int nr;
	unsigned int max_depth;
};

static int fio_solarisaio_prep(struct thread_data fio_unused *td,
			    struct io_u *io_u)
{
	struct solarisaio_data *sd = td->io_ops_data;

	io_u->resultp.aio_return = AIO_INPROGRESS;
	io_u->engine_data = sd;
	return 0;
}

static void wait_for_event(struct timeval *tv)
{
	struct solarisaio_data *sd;
	struct io_u *io_u;
	aio_result_t *res;

	res = aiowait(tv);
	if (res == (aio_result_t *) -1) {
		int err = errno;

		if (err != EINVAL) {
			log_err("fio: solarisaio got %d in aiowait\n", err);
			exit(err);
		}
		return;
	} else if (!res)
		return;

	io_u = container_of(res, struct io_u, resultp);
	sd = io_u->engine_data;

	if (io_u->resultp.aio_return >= 0) {
		io_u->resid = io_u->xfer_buflen - io_u->resultp.aio_return;
		io_u->error = 0;
	} else
		io_u->error = io_u->resultp.aio_errno;

	/*
	 * For SIGIO, we need a write barrier between the two, so that
	 * the ->aio_pending store is seen after the ->aio_events store
	 */
	sd->aio_events[sd->aio_pending] = io_u;
	write_barrier();
	sd->aio_pending++;
	sd->nr--;
}

static int fio_solarisaio_getevents(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct solarisaio_data *sd = td->io_ops_data;
	struct timeval tv;
	int ret;

	if (!min || !t) {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	} else {
		tv.tv_sec = t->tv_sec;
		tv.tv_usec = t->tv_nsec / 1000;
	}

	while (sd->aio_pending < min)
		wait_for_event(&tv);

	/*
	 * should be OK without locking, as int operations should be atomic
	 */
	ret = sd->aio_pending;
	sd->aio_pending -= ret;
	return ret;
}

static struct io_u *fio_solarisaio_event(struct thread_data *td, int event)
{
	struct solarisaio_data *sd = td->io_ops_data;

	return sd->aio_events[event];
}

static enum fio_q_status fio_solarisaio_queue(struct thread_data fio_unused *td,
			      struct io_u *io_u)
{
	struct solarisaio_data *sd = td->io_ops_data;
	struct fio_file *f = io_u->file;
	off_t off;
	int ret;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_SYNC) {
		if (sd->nr)
			return FIO_Q_BUSY;
		if (fsync(f->fd) < 0)
			io_u->error = errno;

		return FIO_Q_COMPLETED;
	}

	if (io_u->ddir == DDIR_DATASYNC) {
		if (sd->nr)
			return FIO_Q_BUSY;
		if (fdatasync(f->fd) < 0)
			io_u->error = errno;

		return FIO_Q_COMPLETED;
	}

	if (sd->nr == sd->max_depth)
		return FIO_Q_BUSY;

	off = io_u->offset;
	if (io_u->ddir == DDIR_READ)
		ret = aioread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, off,
					SEEK_SET, &io_u->resultp);
	else
		ret = aiowrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, off,
					SEEK_SET, &io_u->resultp);
	if (ret) {
		io_u->error = errno;
		td_verror(td, io_u->error, "xfer");
		return FIO_Q_COMPLETED;
	}

	sd->nr++;
	return FIO_Q_QUEUED;
}

static void fio_solarisaio_cleanup(struct thread_data *td)
{
	struct solarisaio_data *sd = td->io_ops_data;

	if (sd) {
		free(sd->aio_events);
		free(sd);
	}
}

/*
 * Set USE_SIGNAL_COMPLETIONS to use SIGIO as completion events.
 */
#ifdef USE_SIGNAL_COMPLETIONS
static void fio_solarisaio_sigio(int sig)
{
	wait_for_event(NULL);
}

static void fio_solarisaio_init_sigio(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = fio_solarisaio_sigio;
	act.sa_flags = SA_RESTART;
	sigaction(SIGIO, &act, NULL);
}
#endif

static int fio_solarisaio_init(struct thread_data *td)
{
	unsigned int max_depth;
	struct solarisaio_data *sd;
	sd = calloc(1, sizeof(*sd));

	max_depth = td->o.iodepth;
	if (max_depth > MAXASYNCHIO) {
		max_depth = MAXASYNCHIO;
		log_info("fio: lower depth to %d due to OS constraints\n",
							max_depth);
	}

	sd->aio_events = calloc(max_depth, sizeof(struct io_u *));
	sd->max_depth = max_depth;

#ifdef USE_SIGNAL_COMPLETIONS
	fio_solarisaio_init_sigio();
#endif

	td->io_ops_data = sd;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "solarisaio",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_solarisaio_init,
	.prep		= fio_solarisaio_prep,
	.queue		= fio_solarisaio_queue,
	.getevents	= fio_solarisaio_getevents,
	.event		= fio_solarisaio_event,
	.cleanup	= fio_solarisaio_cleanup,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
};

static void fio_init fio_solarisaio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_solarisaio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
