/*
 * [한국어 설명] 오프로드 모드 워크큐 헤더 (workqueue.h)
 *
 * === 파일의 역할 ===
 * I/O 작업을 별도 워커 스레드 풀로 오프로드하기 위한 구조체 및 함수를 선언한다.
 * rate-submit 모드에서 메인 스레드가 I/O를 워커에 위임하여 일정 제출 속도를 유지.
 *
 * === 전체 아키텍처에서의 위치 ===
 * workqueue.c와 짝을 이루는 헤더. rate-submit.c와 iolog.c에서 참조.
 * workqueue.h → workqueue.c(구현) / rate-submit.c(I/O 오프로드) / iolog.c(압축)
 *
 * === 타 모듈과의 연결 ===
 * - workqueue.c: 이 헤더의 구조체와 함수의 구현
 * - rate-submit.c: rate 제한 I/O 제출 시 워크큐 사용
 * - iolog.c: 로그 압축 작업을 워크큐에 제출
 *
 * === 주요 함수/구조체 요약 ===
 * - struct workqueue_work: 워크큐에 추가되는 개별 작업 단위
 * - struct submit_worker: 워커 스레드 구조체 (스레드, 뮤텍스, 작업 리스트)
 * - struct workqueue: 워크큐 전체 관리 구조체 (워커 배열, 콜백)
 * - workqueue_init/enqueue/flush/exit: 워크큐 라이프사이클 API
 */
#ifndef FIO_RATE_H
#define FIO_RATE_H

#include <inttypes.h>
#include <pthread.h>

#include "flist.h"         /* 연결 리스트 (flist_head) */
#include "lib/types.h"     /* 기본 타입 정의 */

struct sk_out;             /* 소켓 출력 컨텍스트 (서버 모드용) */
struct thread_data;        /* fio 스레드 데이터 (전방 선언) */

/* [한국어] 워크큐에 추가되는 개별 작업 단위 */
struct workqueue_work {
	struct flist_head list;  /* 워커의 work_list에 연결되는 리스트 노드 */
};

/* [한국어] 워커 스레드 구조체 - 각 워커의 상태 및 작업 리스트 관리 */
struct submit_worker {
	pthread_t thread;              /* 워커 스레드 핸들 */
	pthread_mutex_t lock;          /* 워커별 뮤텍스 (work_list 보호) */
	pthread_cond_t cond;           /* 작업 도착 대기용 조건변수 */
	struct flist_head work_list;   /* 이 워커에 할당된 작업 리스트 */
	unsigned int flags;            /* 워커 상태 플래그 (SW_F_IDLE 등) */
	unsigned int index;            /* 워커 배열 내 인덱스 */
	uint64_t seq;                  /* 마지막으로 할당받은 작업 순서 번호 */
	struct workqueue *wq;          /* 소속 워크큐 역참조 */
	void *priv;                    /* 워커별 사적 데이터 (io_u 풀 등) */
	struct sk_out *sk_out;         /* 서버 모드 소켓 출력 */
};

/* [한국어] 워크큐 콜백 함수 타입 정의 */
typedef int (workqueue_work_fn)(struct submit_worker *, struct workqueue_work *);
typedef bool (workqueue_pre_sleep_flush_fn)(struct submit_worker *);  /* 슬립 전 flush 필요 여부 확인 */
typedef void (workqueue_pre_sleep_fn)(struct submit_worker *);        /* 슬립 전 실행 (미완료 I/O 처리) */
typedef int (workqueue_alloc_worker_fn)(struct submit_worker *);      /* 워커별 자원 할당 */
typedef void (workqueue_free_worker_fn)(struct submit_worker *);      /* 워커별 자원 해제 */
typedef int (workqueue_init_worker_fn)(struct submit_worker *);       /* 워커 초기화 콜백 */
typedef void (workqueue_exit_worker_fn)(struct submit_worker *, unsigned int *); /* 워커 종료 콜백 */
typedef void (workqueue_update_acct_fn)(struct submit_worker *);      /* 통계 갱신 콜백 */

/* [한국어] 워크큐 오퍼레이션 구조체 - 콜백 함수 집합 */
struct workqueue_ops {
	workqueue_work_fn *fn;                    /* 작업 처리 함수 (필수) */
	workqueue_pre_sleep_flush_fn *pre_sleep_flush_fn; /* 슬립 전 flush 확인 */
	workqueue_pre_sleep_fn *pre_sleep_fn;     /* 슬립 전 처리 */

	workqueue_update_acct_fn *update_acct_fn; /* 통계 갱신 */

	workqueue_alloc_worker_fn *alloc_worker_fn; /* 워커 할당 */
	workqueue_free_worker_fn *free_worker_fn;   /* 워커 해제 */

	workqueue_init_worker_fn *init_worker_fn; /* 워커 초기화 */
	workqueue_exit_worker_fn *exit_worker_fn; /* 워커 종료 */

	unsigned int nice;                        /* 워커 스레드 nice 값 */
};

/* [한국어] 워크큐 구조체 - 워커 스레드 풀 및 동기화 관리 */
struct workqueue {
	unsigned int max_workers;         /* 최대 워커 수 */

	struct thread_data *td;           /* 소속 fio 스레드 데이터 */
	struct workqueue_ops ops;         /* 콜백 함수 집합 */

	uint64_t work_seq;                /* 작업 순서 카운터 (워커 선택 기준) */
	struct submit_worker *workers;    /* 워커 배열 (공유 메모리) */
	unsigned int next_free_worker;    /* 다음 유휴 워커 검색 시작 인덱스 */

	pthread_cond_t flush_cond;        /* flush 완료 대기용 조건변수 */
	pthread_mutex_t flush_lock;       /* flush 보호용 뮤텍스 */
	pthread_mutex_t stat_lock;        /* 통계 보호용 뮤텍스 */
	volatile int wake_idle;           /* flush 시 유휴 전환 알림 요청 플래그 */
};

/* [한국어] 워크큐 API */
int workqueue_init(struct thread_data *td, struct workqueue *wq, struct workqueue_ops *ops, unsigned int max_workers, struct sk_out *sk_out);
void workqueue_exit(struct workqueue *wq);

void workqueue_enqueue(struct workqueue *wq, struct workqueue_work *work);
void workqueue_flush(struct workqueue *wq);

/* [한국어] 슬립 전 flush 필요 여부 확인 - pre_sleep_flush_fn 콜백 호출 */
static inline bool workqueue_pre_sleep_check(struct submit_worker *sw)
{
	struct workqueue *wq = sw->wq;

	if (!wq->ops.pre_sleep_flush_fn)
		return false;

	return wq->ops.pre_sleep_flush_fn(sw);
}

/* [한국어] 슬립 전 처리 - 미완료된 I/O 이벤트 수거 등 */
static inline void workqueue_pre_sleep(struct submit_worker *sw)
{
	struct workqueue *wq = sw->wq;

	if (wq->ops.pre_sleep_fn)
		wq->ops.pre_sleep_fn(sw);
}

/* [한국어] 워커 초기화 콜백 호출 - 워커 스레드 시작 시 실행 */
static inline int workqueue_init_worker(struct submit_worker *sw)
{
	struct workqueue *wq = sw->wq;

	if (!wq->ops.init_worker_fn)
		return 0;

	return wq->ops.init_worker_fn(sw);
}

/* [한국어] 워커 종료 콜백 호출 - 워커 스레드 종료 시 자원 정리 */
static inline void workqueue_exit_worker(struct submit_worker *sw,
					 unsigned int *sum_cnt)
{
	struct workqueue *wq = sw->wq;
	unsigned int tmp = 1;

	if (!wq->ops.exit_worker_fn)
		return;

	if (!sum_cnt)
		sum_cnt = &tmp;

	wq->ops.exit_worker_fn(sw, sum_cnt);
}
#endif
