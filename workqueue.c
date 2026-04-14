/*
 * Generic workqueue offload mechanism
 *
 * Copyright (C) 2015 Jens Axboe <axboe@kernel.dk>
 *
 */
/*
 * [한국어 설명] 오프로드 모드 워크큐 구현 (workqueue.c)
 *
 * === 파일의 역할 ===
 * I/O 작업을 별도의 워커 스레드 풀로 오프로드하는 범용 워크큐 메커니즘을 구현한다.
 * 워커 스레드 풀 생성, 작업 배분, 플러시, 종료/자원 해제를 담당한다.
 * rate-submit 모드에서 메인 스레드가 I/O를 워커에 위임하여 일정 제출 속도를 유지한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * rate-submit.c에서 workqueue_init/enqueue를 호출하여 I/O를 오프로드.
 * iolog.c에서 gz_work 로그 압축을 워크큐에 제출.
 * 호출 체인: rate-submit.c / iolog.c → workqueue_enqueue() [이 파일] → worker_thread()
 *
 * === 타 모듈과의 연결 ===
 * - rate-submit.c: rate 제한 I/O 제출 시 워크큐 사용
 * - iolog.c: gz_work 로그 압축 작업을 워크큐에 제출
 * - workqueue.h: 워크큐/워커 구조체 및 API 선언
 * - pshared.c: 프로세스 간 공유 뮤텍스/조건변수
 *
 * === 주요 함수/구조체 요약 ===
 * - workqueue_init(): 워커 스레드 풀 생성 및 초기화
 * - workqueue_enqueue(): 작업을 유휴 워커에게 배분
 * - workqueue_flush(): 모든 워커가 유휴 상태가 될 때까지 대기
 * - worker_thread(): 각 워커 스레드의 메인 루프
 */

#include <unistd.h>

#include "fio.h"           /* fio 핵심 구조체 및 매크로 */
#include "flist.h"         /* 연결 리스트 (flist_head) */
#include "workqueue.h"     /* 워크큐 구조체 및 함수 선언 */
#include "smalloc.h"       /* 공유 메모리 할당기 */
#include "pshared.h"       /* 프로세스 간 공유 뮤텍스/조건변수 */

/* [한국어] 워커 스레드 상태 플래그 */
enum {
	SW_F_IDLE	= 1 << 0,	/* 유휴 상태 - 작업 대기 중 */
	SW_F_RUNNING	= 1 << 1,	/* 실행 중 - 초기화 완료 후 동작 중 */
	SW_F_EXIT	= 1 << 2,	/* 종료 요청됨 */
	SW_F_ACCOUNTED	= 1 << 3,	/* 종료 처리 완료 (중복 방지) */
	SW_F_ERROR	= 1 << 4,	/* 초기화 중 에러 발생 */
};

/*
 * [한국어] 지정된 범위(start~end)에서 유휴 워커를 검색
 * 유휴 워커가 없으면 가장 오래된(seq가 작은) 워커를 best에 기록
 */
static struct submit_worker *__get_submit_worker(struct workqueue *wq,
						 unsigned int start,
						 unsigned int end,
						 struct submit_worker **best)
{
	struct submit_worker *sw = NULL;

	while (start <= end) {
		sw = &wq->workers[start];
		if (sw->flags & SW_F_IDLE)
			return sw;
		if (!(*best) || sw->seq < (*best)->seq)
			*best = sw;
		start++;
	}

	return NULL;
}

/*
 * [한국어] 작업을 할당할 워커 선택
 * next_free_worker부터 순회하며 유휴 워커를 찾고,
 * 없으면 wraparound하여 0부터 다시 검색.
 * 모든 워커가 바쁘면 가장 적게 사용된(seq가 낮은) 워커 반환.
 */
static struct submit_worker *get_submit_worker(struct workqueue *wq)
{
	unsigned int next = wq->next_free_worker;
	struct submit_worker *sw, *best = NULL;

	assert(next < wq->max_workers);

	sw = __get_submit_worker(wq, next, wq->max_workers - 1, &best);
	if (!sw && next)
		sw = __get_submit_worker(wq, 0, next - 1, &best);

	/*
	 * No truly idle found, use best match
	 */
	/* [한국어] 완전히 유휴인 워커가 없으면 가장 적합한 워커 사용 */
	if (!sw)
		sw = best;

	/* [한국어] 다음 검색 시작점 갱신 (라운드 로빈) */
	if (sw->index == wq->next_free_worker) {
		if (sw->index + 1 < wq->max_workers)
			wq->next_free_worker = sw->index + 1;
		else
			wq->next_free_worker = 0;
	}

	return sw;
}

/* [한국어] 모든 워커가 유휴 상태인지 확인 */
static bool all_sw_idle(struct workqueue *wq)
{
	int i;

	for (i = 0; i < wq->max_workers; i++) {
		struct submit_worker *sw = &wq->workers[i];

		if (!(sw->flags & SW_F_IDLE))
			return false;
	}

	return true;
}

/*
 * Must be serialized wrt workqueue_enqueue() by caller
 */
/*
 * [한국어] 워크큐 플러시 - 모든 워커가 유휴 상태가 될 때까지 블로킹 대기
 * 호출자가 workqueue_enqueue()와의 직렬화를 보장해야 함
 */
void workqueue_flush(struct workqueue *wq)
{
	pthread_mutex_lock(&wq->flush_lock);
	wq->wake_idle = 1;  /* 워커가 유휴 전환 시 flush_cond 시그널 요청 */

	while (!all_sw_idle(wq))
		pthread_cond_wait(&wq->flush_cond, &wq->flush_lock);

	wq->wake_idle = 0;
	pthread_mutex_unlock(&wq->flush_lock);
}

/*
 * Must be serialized by caller.
 */
/*
 * [한국어] 작업을 워크큐에 추가
 * 적절한 워커를 선택하고, 해당 워커의 work_list에 작업을 넣은 뒤 깨움
 */
void workqueue_enqueue(struct workqueue *wq, struct workqueue_work *work)
{
	struct submit_worker *sw;

	sw = get_submit_worker(wq);
	assert(sw);

	pthread_mutex_lock(&sw->lock);
	flist_add_tail(&work->list, &sw->work_list);  /* 작업을 워커 리스트 끝에 추가 */
	sw->seq = ++wq->work_seq;                      /* 작업 순서 번호 갱신 */
	sw->flags &= ~SW_F_IDLE;                       /* 유휴 플래그 해제 */

	pthread_cond_signal(&sw->cond);                /* 워커 스레드 깨우기 */
	pthread_mutex_unlock(&sw->lock);
}

/* [한국어] 워커의 작업 리스트에서 작업을 하나씩 꺼내 처리 */
static void handle_list(struct submit_worker *sw, struct flist_head *list)
{
	struct workqueue *wq = sw->wq;
	struct workqueue_work *work;

	while (!flist_empty(list)) {
		work = flist_first_entry(list, struct workqueue_work, list);
		flist_del_init(&work->list);
		wq->ops.fn(sw, work);  /* 등록된 콜백 함수로 작업 처리 */
	}
}

/*
 * [한국어] 워커 스레드의 메인 함수
 * 1) nice 값 설정 및 워커 초기화
 * 2) 메인 루프: 작업 대기 -> 작업 수신 -> handle_list()로 처리 -> 반복
 * 3) 종료 플래그(SW_F_EXIT) 수신 시 루프 탈출
 */
static void *worker_thread(void *data)
{
	struct submit_worker *sw = data;
	struct workqueue *wq = sw->wq;
	unsigned int ret = 0;
	FLIST_HEAD(local_list);  /* 로컬 작업 리스트 (락 밖에서 처리하기 위해 복사) */

	sk_out_assign(sw->sk_out);

	/* [한국어] nice 값 설정 - 워커 스레드 우선순위 조정 */
	if (wq->ops.nice) {
		errno = 0;
		if (nice(wq->ops.nice) == -1 && errno != 0) {
			log_err("workqueue: nice %s\n", strerror(errno));
			ret = 1;
		}
	}

	/* [한국어] 워커별 초기화 콜백 호출 */
	if (!ret)
		ret = workqueue_init_worker(sw);

	/* [한국어] 실행 상태 플래그 설정 */
	pthread_mutex_lock(&sw->lock);
	sw->flags |= SW_F_RUNNING;
	if (ret)
		sw->flags |= SW_F_ERROR;
	pthread_mutex_unlock(&sw->lock);

	/* [한국어] 초기화 완료를 workqueue_init()에 알림 */
	pthread_mutex_lock(&wq->flush_lock);
	pthread_cond_signal(&wq->flush_cond);
	pthread_mutex_unlock(&wq->flush_lock);

	if (sw->flags & SW_F_ERROR)
		goto done;

	/* [한국어] 메인 작업 처리 루프 */
	pthread_mutex_lock(&sw->lock);
	while (1) {
		if (flist_empty(&sw->work_list)) {
			if (sw->flags & SW_F_EXIT) {
				break;
			}

			/* [한국어] 슬립 전 체크 - 미처리 I/O가 있으면 flush */
			if (workqueue_pre_sleep_check(sw)) {
				pthread_mutex_unlock(&sw->lock);
				workqueue_pre_sleep(sw);
				pthread_mutex_lock(&sw->lock);
			}
		}
		/*
		 * We may have dropped and reaquired the lock, check state
		 * again.
		 */
		/* [한국어] 락을 해제했다 재획득했으므로 상태를 다시 확인 */
		if (flist_empty(&sw->work_list)) {
			if (sw->flags & SW_F_EXIT) {
				break;
			}
			/* [한국어] 유휴 상태 전환 및 flush 대기자에게 알림 */
			if (!(sw->flags & SW_F_IDLE)) {
				sw->flags |= SW_F_IDLE;
				wq->next_free_worker = sw->index;
				pthread_mutex_unlock(&sw->lock);
				pthread_mutex_lock(&wq->flush_lock);
				if (wq->wake_idle)
					pthread_cond_signal(&wq->flush_cond);
				pthread_mutex_unlock(&wq->flush_lock);
				pthread_mutex_lock(&sw->lock);
			}
		}
		if (flist_empty(&sw->work_list)) {
			if (sw->flags & SW_F_EXIT) {
				break;
			}
			/* [한국어] 작업이 없으면 조건변수에서 대기 */
			pthread_cond_wait(&sw->cond, &sw->lock);
		} else {
			/* [한국어] 작업 리스트를 로컬 리스트로 이동 (락 밖에서 처리) */
			flist_splice_init(&sw->work_list, &local_list);
		}
		pthread_mutex_unlock(&sw->lock);
		handle_list(sw, &local_list);          /* 작업 처리 */
		if (wq->ops.update_acct_fn)
			wq->ops.update_acct_fn(sw);    /* 통계 갱신 콜백 */
		pthread_mutex_lock(&sw->lock);
	}
	pthread_mutex_unlock(&sw->lock);

done:
	sk_out_drop();
	return NULL;
}

/* [한국어] 워커 자원 해제 - 종료 콜백 호출 후 뮤텍스/조건변수 파괴 */
static void free_worker(struct submit_worker *sw, unsigned int *sum_cnt)
{
	struct workqueue *wq = sw->wq;

	workqueue_exit_worker(sw, sum_cnt);

	pthread_cond_destroy(&sw->cond);
	pthread_mutex_destroy(&sw->lock);

	if (wq->ops.free_worker_fn)
		wq->ops.free_worker_fn(sw);
}

/* [한국어] 워커 스레드 종료 대기(join) 후 자원 해제 */
static void shutdown_worker(struct submit_worker *sw, unsigned int *sum_cnt)
{
	pthread_join(sw->thread, NULL);
	free_worker(sw, sum_cnt);
}

/*
 * [한국어] 워크큐 종료
 * 1) 모든 워커에 SW_F_EXIT 플래그 설정 및 깨움
 * 2) 각 워커 스레드를 join하고 자원 해제
 * 3) 공유 메모리 및 동기화 객체 파괴
 */
void workqueue_exit(struct workqueue *wq)
{
	unsigned int shutdown, sum_cnt = 0;
	struct submit_worker *sw;
	int i;

	if (!wq->workers)
		return;

	/* [한국어] 모든 워커에 종료 신호 전송 */
	for (i = 0; i < wq->max_workers; i++) {
		sw = &wq->workers[i];

		pthread_mutex_lock(&sw->lock);
		sw->flags |= SW_F_EXIT;
		pthread_cond_signal(&sw->cond);
		pthread_mutex_unlock(&sw->lock);
	}

	/* [한국어] 모든 워커의 종료를 순차적으로 처리 */
	do {
		shutdown = 0;
		for (i = 0; i < wq->max_workers; i++) {
			sw = &wq->workers[i];
			if (sw->flags & SW_F_ACCOUNTED)
				continue;
			pthread_mutex_lock(&sw->lock);
			sw->flags |= SW_F_ACCOUNTED;
			pthread_mutex_unlock(&sw->lock);
			shutdown_worker(sw, &sum_cnt);
			shutdown++;
		}
	} while (shutdown && shutdown != wq->max_workers);

	/* [한국어] 워크큐 공유 자원 해제 */
	sfree(wq->workers);
	wq->workers = NULL;
	pthread_mutex_destroy(&wq->flush_lock);
	pthread_cond_destroy(&wq->flush_cond);
	pthread_mutex_destroy(&wq->stat_lock);
}

/*
 * [한국어] 단일 워커 스레드 시작
 * 워커 구조체 초기화 후 pthread_create()로 worker_thread 생성
 */
static int start_worker(struct workqueue *wq, unsigned int index,
			struct sk_out *sk_out)
{
	struct submit_worker *sw = &wq->workers[index];
	int ret;

	INIT_FLIST_HEAD(&sw->work_list);  /* 작업 리스트 초기화 */

	ret = mutex_cond_init_pshared(&sw->lock, &sw->cond);
	if (ret)
		return ret;

	sw->wq = wq;
	sw->index = index;
	sw->sk_out = sk_out;
	sw->flags = 0;

	/* [한국어] 워커별 할당 콜백 호출 (커스텀 초기화) */
	if (wq->ops.alloc_worker_fn) {
		ret = wq->ops.alloc_worker_fn(sw);
		if (ret)
			return ret;
	}

	ret = pthread_create(&sw->thread, NULL, worker_thread, sw);
	if (!ret) {
		pthread_mutex_lock(&sw->lock);
		sw->flags |= SW_F_IDLE;  /* 생성 직후 유휴 상태 */
		pthread_mutex_unlock(&sw->lock);
		return 0;
	}

	free_worker(sw, NULL);
	return 1;
}

/*
 * [한국어] 워크큐 초기화
 * 1) 워크큐 구조체 설정 (최대 워커 수, 콜백 등)
 * 2) 공유 메모리에 워커 배열 할당
 * 3) 워커 스레드 풀 생성 및 모든 워커의 초기화 완료 대기
 */
int workqueue_init(struct thread_data *td, struct workqueue *wq,
		   struct workqueue_ops *ops, unsigned int max_workers,
		   struct sk_out *sk_out)
{
	unsigned int running;
	int i, error;
	int ret;

	wq->max_workers = max_workers;
	wq->td = td;
	wq->ops = *ops;
	wq->work_seq = 0;            /* 작업 순서 카운터 초기화 */
	wq->next_free_worker = 0;    /* 다음 유휴 워커 검색 시작점 */

	/* [한국어] 플러시용 뮤텍스/조건변수 및 통계 뮤텍스 초기화 */
	ret = mutex_cond_init_pshared(&wq->flush_lock, &wq->flush_cond);
	if (ret)
		goto err;
	ret = mutex_init_pshared(&wq->stat_lock);
	if (ret)
		goto err;

	/* [한국어] 공유 메모리에 워커 배열 할당 */
	wq->workers = smalloc(wq->max_workers * sizeof(struct submit_worker));
	if (!wq->workers)
		goto err;

	/* [한국어] 각 워커 스레드 시작 */
	for (i = 0; i < wq->max_workers; i++)
		if (start_worker(wq, i, sk_out))
			break;

	wq->max_workers = i;  /* 실제 시작된 워커 수로 갱신 */
	if (!wq->max_workers)
		goto err;

	/*
	 * Wait for them all to be started and initialized
	 */
	/* [한국어] 모든 워커가 RUNNING 상태가 될 때까지 대기 */
	error = 0;
	pthread_mutex_lock(&wq->flush_lock);
	do {
		struct submit_worker *sw;

		running = 0;
		for (i = 0; i < wq->max_workers; i++) {
			sw = &wq->workers[i];
			pthread_mutex_lock(&sw->lock);
			if (sw->flags & SW_F_RUNNING)
				running++;
			if (sw->flags & SW_F_ERROR)
				error++;
			pthread_mutex_unlock(&sw->lock);
		}

		if (error || running == wq->max_workers)
			break;

		pthread_cond_wait(&wq->flush_cond, &wq->flush_lock);
	} while (1);
	pthread_mutex_unlock(&wq->flush_lock);

	if (!error)
		return 0;

err:
	log_err("Can't create rate workqueue\n");
	td_verror(td, ESRCH, "workqueue_init");
	workqueue_exit(wq);
	return 1;
}
