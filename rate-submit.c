/*
 * Rated submission helpers
 *
 * Copyright (C) 2015 Jens Axboe <axboe@kernel.dk>
 *
 */
/*
 * [한국어] rate-submit.c - 속도 제한 I/O 제출 (rate 옵션 시 사용, 워크큐 기반)
 *
 * 이 파일은 io_submit_mode=offload일 때 사용되는 워크큐 기반 I/O 제출 메커니즘을 구현한다.
 * rate 옵션이 설정되면, 메인 스레드가 직접 I/O를 제출하는 대신 워크큐의 워커 스레드에
 * I/O를 위임(offload)하여 정밀한 속도 제어를 달성한다.
 *
 * 주요 흐름:
 *   1) rate_submit_init()  - 워크큐를 초기화하고 워커 스레드를 생성
 *   2) io_workqueue_fn()   - 워커 스레드가 실제 I/O를 제출하는 핵심 콜백
 *   3) rate_submit_exit()  - 워크큐를 정리하고 워커 스레드를 종료
 *
 * 워크큐 콜백 구조:
 *   - fn                    : I/O 제출 (td_io_queue)
 *   - pre_sleep_flush_fn    : 슬립 전 미완료 I/O 존재 여부 확인
 *   - pre_sleep_fn          : 슬립 전 대기 중인 I/O를 quiesce(정리)
 *   - update_acct_fn        : 워커의 I/O 통계를 부모 스레드에 합산
 *   - alloc/free_worker_fn  : 워커별 thread_data 할당/해제
 *   - init/exit_worker_fn   : 워커의 I/O 엔진 초기화/종료
 */

#include <assert.h>     /* assert() 매크로 */
#include <errno.h>      /* errno 및 에러 코드 */
#include <pthread.h>    /* POSIX 스레드 (뮤텍스, 조건변수) */

#include "fio.h"            /* fio 핵심 구조체 및 매크로 */
#include "ioengines.h"      /* I/O 엔진 인터페이스 */
#include "lib/getrusage.h"  /* 리소스 사용량 조회 */
#include "rate-submit.h"    /* 속도 제한 제출 헤더 */

/* [한국어] I/O 오버랩(겹침) 검사 - serialize_overlap 옵션이 설정된 경우
 * 동일 영역에 대한 동시 I/O를 방지하기 위해 뮤텍스를 사용하여 순차적으로 검사한다.
 * 오버랩이 발견되면 다른 스레드가 진행할 수 있도록 잠금을 해제한 뒤 재시도한다. */
static void check_overlap(struct io_u *io_u)
{
	int res;

	/*
	 * Allow only one thread to check for overlap at a time to prevent two
	 * threads from thinking the coast is clear and then submitting IOs
	 * that overlap with each other.
	 *
	 * If an overlap is found, release the lock and re-acquire it before
	 * checking again to give other threads a chance to make progress.
	 *
	 * If no overlap is found, release the lock when the io_u's
	 * IO_U_F_FLIGHT flag is set so that this io_u can be checked by other
	 * threads as they assess overlap.
	 */
	res = pthread_mutex_lock(&overlap_check);
	if (fio_unlikely(res != 0)) {
		log_err("failed to lock overlap check mutex, err: %i:%s", errno, strerror(errno));
		abort();
	}

retry:
	/* 모든 스레드를 순회하며 현재 io_u와 겹치는 진행 중인 I/O가 있는지 확인 */
	for_each_td(td) {
		if (td->runstate <= TD_SETTING_UP ||
		    td->runstate >= TD_FINISHING ||
		    !td->o.serialize_overlap ||
		    td->o.io_submit_mode != IO_MODE_OFFLOAD)
			continue;

		if (!in_flight_overlap(&td->io_u_all, io_u))
			continue;

		/* 오버랩 발견: 잠금을 해제하고 다른 스레드에 양보한 뒤 재시도 */
		res = pthread_mutex_unlock(&overlap_check);
		if (fio_unlikely(res != 0)) {
			log_err("failed to unlock overlap check mutex, err: %i:%s", errno, strerror(errno));
			abort();
		}
		res = pthread_mutex_lock(&overlap_check);
		if (fio_unlikely(res != 0)) {
			log_err("failed to lock overlap check mutex, err: %i:%s", errno, strerror(errno));
			abort();
		}
		goto retry;
	} end_for_each();
}

/* [한국어] 워크큐 I/O 제출 콜백 - 워커 스레드가 실제 I/O를 처리하는 핵심 함수
 * io_u를 큐에 제출하고, FIO_Q_BUSY이면 완료를 기다린 뒤 재시도한다.
 * 완료 후 io_queue_event()로 결과를 처리한다. */
static int io_workqueue_fn(struct submit_worker *sw,
			   struct workqueue_work *work)
{
	struct io_u *io_u = container_of(work, struct io_u, work);
	const enum fio_ddir ddir = io_u->ddir;  /* I/O 방향 (읽기/쓰기/트림) */
	struct thread_data *td = sw->priv;       /* 워커의 thread_data */
	int ret, error;

	/* serialize_overlap이 설정된 경우, 제출 전에 오버랩 검사 수행 */
	if (td->o.serialize_overlap)
		check_overlap(io_u);

	dprint(FD_RATE, "io_u %p queued by %u\n", io_u, gettid());

	io_u_set(td, io_u, IO_U_F_NO_FILE_PUT);

	td->cur_depth++;  /* 현재 큐 깊이 증가 */

	/* I/O 제출 루프: BUSY이면 완료된 I/O를 수거 후 재시도 */
	do {
		ret = td_io_queue(td, io_u);
		if (ret != FIO_Q_BUSY)
			break;
		ret = io_u_queued_complete(td, 1);
		if (ret > 0)
			td->cur_depth -= ret;  /* 완료된 수만큼 깊이 감소 */
		else if (ret < 0)
			break;
		io_u_clear(td, io_u, IO_U_F_FLIGHT);
	} while (1);

	dprint(FD_RATE, "io_u %p ret %d by %u\n", io_u, ret, gettid());

	/* I/O 큐 이벤트 처리 (완료/에러 상태 갱신) */
	error = io_queue_event(td, io_u, &ret, ddir, NULL, 0, NULL);

	if (ret == FIO_Q_COMPLETED)
		td->cur_depth--;  /* 동기 완료: 깊이 감소 */
	else if (ret == FIO_Q_QUEUED) {
		unsigned int min_evts;

		/* iodepth=1이면 즉시 완료를 기다리고, 아니면 비동기로 진행 */
		if (td->o.iodepth == 1)
			min_evts = 1;
		else
			min_evts = 0;

		ret = io_u_queued_complete(td, min_evts);
		if (ret > 0)
			td->cur_depth -= ret;
	}

	/* 에러 발생 시 부모 스레드에 시그널을 보내 대기를 해제 */
	if (error || td->error) {
		pthread_mutex_lock(&td->io_u_lock);
		pthread_cond_signal(&td->parent->free_cond);
		pthread_mutex_unlock(&td->io_u_lock);
	}

	return 0;
}

/* [한국어] 슬립 전 플러시 필요 여부 확인 - 미완료 I/O가 남아있으면 true 반환 */
static bool io_workqueue_pre_sleep_flush_fn(struct submit_worker *sw)
{
	struct thread_data *td = sw->priv;

	if (td->error)
		return false;
	if (td->io_u_queued || td->cur_depth || td->io_u_in_flight)
		return true;

	return false;
}

/* [한국어] 슬립 전 처리 - 대기 중인 모든 I/O를 quiesce(완료 대기)하여 정리 */
static void io_workqueue_pre_sleep_fn(struct submit_worker *sw)
{
	struct thread_data *td = sw->priv;
	int ret;

	ret = io_u_quiesce(td);
	if (ret > 0)
		td->cur_depth -= ret;
}

/* [한국어] 워커 스레드 할당 콜백 - 워커별 thread_data를 동적 할당 */
static int io_workqueue_alloc_fn(struct submit_worker *sw)
{
	struct thread_data *td;

	td = calloc(1, sizeof(*td));
	sw->priv = td;
	return 0;
}

/* [한국어] 워커 스레드 해제 콜백 - 할당된 thread_data를 해제 */
static void io_workqueue_free_fn(struct submit_worker *sw)
{
	free(sw->priv);
	sw->priv = NULL;
}

/* [한국어] 워커 스레드 초기화 콜백 - 부모의 설정을 복사하고 I/O 엔진을 초기화
 * 부모 thread_data의 옵션, 통계, 파일 정보를 복제한 뒤
 * ioengine_load()와 td_io_init()으로 I/O 엔진을 준비한다. */
static int io_workqueue_init_worker_fn(struct submit_worker *sw)
{
	struct thread_data *parent = sw->wq->td;  /* 부모(원본) thread_data */
	struct thread_data *td = sw->priv;         /* 워커의 thread_data */

	/* 부모의 옵션과 통계 구조체를 복사 */
	memcpy(&td->o, &parent->o, sizeof(td->o));
	memcpy(&td->ts, &parent->ts, sizeof(td->ts));
	td->o.uid = td->o.gid = -1U;
	dup_files(td, parent);            /* 파일 디스크립터 복제 */
	td->eo = parent->eo;             /* 엔진 옵션 포인터 공유 */
	fio_options_mem_dupe(td);         /* 문자열 옵션 메모리 복제 */
	td->iolog_f = parent->iolog_f;   /* I/O 로그 파일 포인터 공유 */

	/* I/O 엔진 로드 */
	if (ioengine_load(td))
		goto err;

	td->pid = gettid();  /* 워커의 스레드 ID 저장 */

	/* 각종 리스트 헤드 초기화 */
	INIT_FLIST_HEAD(&td->io_log_list);
	INIT_FLIST_HEAD(&td->io_hist_list);
	INIT_FLIST_HEAD(&td->verify_list);
	INIT_FLIST_HEAD(&td->trim_list);
	td->io_hist_tree = RB_ROOT;

	td->o.iodepth = 1;  /* 워커는 항상 iodepth=1로 동작 */
	if (td_io_init(td))
		goto err_io_init;

	if (td->io_ops->post_init && td->io_ops->post_init(td))
		goto err_io_init;

	/* 시간 기준점 설정 및 초기 상태 정리 */
	set_epoch_time(td, td->o.log_alternate_epoch_clock_id, td->o.job_start_clock_id);
	fio_getrusage(&td->ru_start);
	clear_io_state(td, 1);

	td_set_runstate(td, TD_RUNNING);
	td->flags |= TD_F_CHILD | TD_F_NEED_LOCK;
	td->parent = parent;
	return 0;

err_io_init:
	close_ioengine(td);
err:
	return 1;

}

/* [한국어] 워커 스레드 종료 콜백 - 워커의 통계를 부모에 합산하고 리소스를 정리
 * per-priority 통계는 offload 모드에서 지원하지 않으므로 비활성화한다. */
static void io_workqueue_exit_worker_fn(struct submit_worker *sw,
					unsigned int *sum_cnt)
{
	struct thread_data *td = sw->priv;

	(*sum_cnt)++;

	/*
	 * io_workqueue_update_acct_fn() doesn't support per prio stats, and
	 * even if it did, offload can't be used with all async IO engines.
	 * If group reporting is set in the parent td, the group result
	 * generated by __show_run_stats() can still contain multiple prios
	 * from different offloaded jobs.
	 */
	sw->wq->td->ts.disable_prio_stat = 1;
	sum_thread_stats(&sw->wq->td->ts, &td->ts);  /* 워커 통계를 부모에 합산 */

	/* 리소스 정리: 옵션 메모리, 파일, I/O 엔진 해제 */
	fio_options_free(td);
	close_and_free_files(td);
	if (td->io_ops)
		close_ioengine(td);
	td_set_runstate(td, TD_EXITED);
}

#ifdef CONFIG_SFAA
/* [한국어] 원자적 값 합산 - __sync_fetch_and_add를 사용한 락-프리 합산
 * CONFIG_SFAA(Sync Fetch And Add) 지원 시 뮤텍스 없이 원자적으로 합산한다. */
static void sum_val(uint64_t *dst, uint64_t *src)
{
	if (*src) {
		__sync_fetch_and_add(dst, *src);
		*src = 0;
	}
}
#else
/* [한국어] 비원자적 값 합산 - 뮤텍스로 보호되는 환경에서 사용 */
static void sum_val(uint64_t *dst, uint64_t *src)
{
	if (*src) {
		*dst += *src;
		*src = 0;
	}
}
#endif

/* [한국어] 두 뮤텍스 동시 해제 - CONFIG_SFAA가 없을 때만 실제로 해제
 * 원자적 연산 미지원 시 뮤텍스를 통한 동기화가 필요하다. */
static void pthread_double_unlock(pthread_mutex_t *lock1,
				  pthread_mutex_t *lock2)
{
#ifndef CONFIG_SFAA
	pthread_mutex_unlock(lock1);
	pthread_mutex_unlock(lock2);
#endif
}

/* [한국어] 두 뮤텍스 동시 획득 - 데드락 방지를 위해 주소 순서로 잠금 */
static void pthread_double_lock(pthread_mutex_t *lock1, pthread_mutex_t *lock2)
{
#ifndef CONFIG_SFAA
	if (lock1 < lock2) {
		pthread_mutex_lock(lock1);
		pthread_mutex_lock(lock2);
	} else {
		pthread_mutex_lock(lock2);
		pthread_mutex_lock(lock1);
	}
#endif
}

/* [한국어] 특정 방향(읽기/쓰기/트림)의 I/O 통계를 src에서 dst로 합산 */
static void sum_ddir(struct thread_data *dst, struct thread_data *src,
		     enum fio_ddir ddir)
{
	pthread_double_lock(&dst->io_wq.stat_lock, &src->io_wq.stat_lock);

	sum_val(&dst->io_bytes[ddir], &src->io_bytes[ddir]);
	sum_val(&dst->io_blocks[ddir], &src->io_blocks[ddir]);
	sum_val(&dst->this_io_blocks[ddir], &src->this_io_blocks[ddir]);
	sum_val(&dst->this_io_bytes[ddir], &src->this_io_bytes[ddir]);
	sum_val(&dst->bytes_done[ddir], &src->bytes_done[ddir]);
	if (ddir == DDIR_READ)
		sum_val(&dst->bytes_verified, &src->bytes_verified);

	pthread_double_unlock(&dst->io_wq.stat_lock, &src->io_wq.stat_lock);
}

/* [한국어] 워커의 I/O 바이트/블록 통계를 부모 스레드에 주기적으로 업데이트 */
static void io_workqueue_update_acct_fn(struct submit_worker *sw)
{
	struct thread_data *src = sw->priv;    /* 워커 (소스) */
	struct thread_data *dst = sw->wq->td;  /* 부모 (대상) */

	if (td_read(src))
		sum_ddir(dst, src, DDIR_READ);
	if (td_write(src))
		sum_ddir(dst, src, DDIR_WRITE);
	if (td_trim(src))
		sum_ddir(dst, src, DDIR_TRIM);

}

/* [한국어] 워크큐 콜백 함수 테이블 - rated(속도 제한) 모드에서 사용하는 워크큐 연산 정의 */
static struct workqueue_ops rated_wq_ops = {
	.fn			= io_workqueue_fn,             /* I/O 제출 콜백 */
	.pre_sleep_flush_fn	= io_workqueue_pre_sleep_flush_fn, /* 슬립 전 플러시 확인 */
	.pre_sleep_fn		= io_workqueue_pre_sleep_fn,       /* 슬립 전 I/O 정리 */
	.update_acct_fn		= io_workqueue_update_acct_fn,     /* 통계 업데이트 */
	.alloc_worker_fn	= io_workqueue_alloc_fn,           /* 워커 할당 */
	.free_worker_fn		= io_workqueue_free_fn,            /* 워커 해제 */
	.init_worker_fn		= io_workqueue_init_worker_fn,     /* 워커 초기화 */
	.exit_worker_fn		= io_workqueue_exit_worker_fn,     /* 워커 종료 */
};

/* [한국어] 속도 제한 워크큐 초기화 - offload 모드일 때만 워크큐를 생성
 * iodepth 수만큼의 워커 스레드를 가진 워크큐를 초기화한다. */
int rate_submit_init(struct thread_data *td, struct sk_out *sk_out)
{
	if (td->o.io_submit_mode != IO_MODE_OFFLOAD)
		return 0;

	return workqueue_init(td, &td->io_wq, &rated_wq_ops, td->o.iodepth, sk_out);
}

/* [한국어] 속도 제한 워크큐 종료 - 워크큐를 해체하고 워커 스레드를 종료 */
void rate_submit_exit(struct thread_data *td)
{
	if (td->o.io_submit_mode != IO_MODE_OFFLOAD)
		return;

	workqueue_exit(&td->io_wq);
}
