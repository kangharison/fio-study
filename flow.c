/*
 * [한국어 설명] I/O 흐름(flow) 제어 모듈 (flow.c)
 *
 * === 파일의 역할 ===
 * 여러 fio 작업(job) 간의 I/O 속도 비율을 제어하는 흐름 제어 기능을 구현한다.
 * 같은 flow_id를 공유하는 작업들은 각자의 flow weight에 비례하여 I/O를 수행한다.
 * 예: job A(flow=3)와 job B(flow=1)이면 A가 B의 3배 속도로 I/O 수행.
 *
 * === 전체 아키텍처에서의 위치 ===
 * backend.c의 do_io()에서 flow_threshold_exceeded()를 호출하여 비율을 체크.
 * thread_main()에서 flow_init_job()/flow_exit_job()으로 흐름 참조 관리.
 * 호출 체인: do_io() [backend.c] → flow_threshold_exceeded() [이 파일]
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: do_io()에서 흐름 임계치 체크, thread_main()에서 초기화/정리
 * - flow.h: 흐름 제어 API 선언
 * - smalloc.c: 공유 메모리로 fio_flow 구조체 할당 (프로세스 간 공유)
 *
 * === 주요 함수/구조체 요약 ===
 * - flow_threshold_exceeded(): 현재 스레드가 할당 비율을 초과했는지 검사
 * - flow_init_job()/flow_exit_job(): 작업별 흐름 초기화/정리
 * - struct fio_flow: 흐름 공유 상태 (id, refs, flow_counter)
 */

#include "fio.h"       /* fio 핵심 구조체 및 매크로 */
#include "fio_sem.h"   /* fio 세마포어 (동기화) */
#include "smalloc.h"   /* 공유 메모리 할당기 */
#include "flist.h"     /* fio 연결 리스트 */

/* [한국어] 흐름 제어 구조체 - 같은 flow_id를 공유하는 작업들의 공유 상태 */
struct fio_flow {
	unsigned int refs;              /* 이 흐름을 참조하는 작업 수 */
	unsigned int id;                /* 흐름 식별자 (flow_id) */
	struct flist_head list;         /* 전역 흐름 목록의 연결 노드 */
	unsigned long flow_counter;     /* 전체 흐름의 공유 카운터 (원자적 갱신) */
	unsigned int total_weight;      /* 모든 참여 작업의 weight 합계 */
};

/* [한국어] 전역 흐름 목록과 동기화 잠금 */
static struct flist_head *flow_list;   /* 모든 fio_flow 객체의 연결 리스트 */
static struct fio_sem *flow_lock;      /* 흐름 목록 접근 보호용 세마포어 */

/* [한국어] 현재 스레드가 흐름 비율 임계치를 초과했는지 검사
 * - 스레드의 카운터 비율과 weight 비율을 비교
 * - 초과 시 flow_sleep만큼 대기하거나 quiesce 후 1 반환 (I/O 일시 중지)
 * - 미초과 시 공유 카운터와 스레드 카운터를 증가시키고 0 반환 (I/O 계속) */
int flow_threshold_exceeded(struct thread_data *td)
{
	struct fio_flow *flow = td->flow;
	double flow_counter_ratio, flow_weight_ratio;

	if (!flow)
		return 0;

	/* [한국어] 스레드 카운터 / 전체 카운터 비율 계산 */
	flow_counter_ratio = (double)td->flow_counter /
		atomic_load_relaxed(&flow->flow_counter);
	/* [한국어] 스레드 weight / 전체 weight 비율 계산 */
	flow_weight_ratio = (double)td->o.flow /
		atomic_load_relaxed(&flow->total_weight);

	/*
	 * each thread/process executing a fio job will stall based on the
	 * expected  user ratio for a given flow_id group. the idea is to keep
	 * 2 counters, flow and job-specific counter to test if the
	 * ratio between them is proportional to other jobs in the same flow_id
	 */
	/* [한국어] 카운터 비율이 weight 비율보다 크면 이 스레드가 너무 빠른 것 -> 대기 */
	if (flow_counter_ratio > flow_weight_ratio) {
		if (td->o.flow_sleep) {
			io_u_quiesce(td);          /* 진행 중인 I/O 완료 대기 */
			usleep(td->o.flow_sleep);  /* 지정된 시간만큼 슬립 */
		} else if (td->o.zone_mode == ZONE_MODE_ZBD) {
			io_u_quiesce(td);          /* ZBD 모드에서는 슬립 없이 quiesce만 */
		}

		return 1;  /* 임계치 초과 - I/O 일시 중지 */
	}

	/*
	 * increment flow(shared counter, therefore atomically)
	 * and job-specific counter
	 */
	/* [한국어] 공유 카운터(원자적)와 스레드별 카운터를 각각 증가 */
	atomic_add(&flow->flow_counter, 1);
	++td->flow_counter;

	return 0;  /* 임계치 미초과 - I/O 계속 */
}

/* [한국어] 흐름 객체를 ID로 검색하거나 새로 생성
 * - 기존 ID가 있으면 참조 카운트 증가 후 반환
 * - 없으면 공유 메모리에 새로 할당하고 전역 목록에 추가
 * - 세마포어로 동기화하여 스레드 안전성 보장 */
static struct fio_flow *flow_get(unsigned int id)
{
	struct fio_flow *flow = NULL;
	struct flist_head *n;

	if (!flow_lock)
		return NULL;

	fio_sem_down(flow_lock);  /* 잠금 획득 */

	/* [한국어] 기존 흐름 목록에서 ID가 일치하는 항목 검색 */
	flist_for_each(n, flow_list) {
		flow = flist_entry(n, struct fio_flow, list);
		if (flow->id == id)
			break;

		flow = NULL;
	}

	/* [한국어] 해당 ID의 흐름이 없으면 새로 생성 */
	if (!flow) {
		flow = smalloc(sizeof(*flow));
		if (!flow) {
			fio_sem_up(flow_lock);
			return NULL;
		}
		flow->refs = 0;
		INIT_FLIST_HEAD(&flow->list);
		flow->id = id;
		flow->flow_counter = 1;     /* 초기값 1 (0으로 나누기 방지) */
		flow->total_weight = 0;

		flist_add_tail(&flow->list, flow_list);
	}

	flow->refs++;  /* 참조 카운트 증가 */
	fio_sem_up(flow_lock);  /* 잠금 해제 */
	return flow;
}

/* [한국어] 흐름 객체의 참조를 해제
 * - 공유 카운터에서 이 스레드가 기여한 만큼 차감
 * - 참조 카운트가 0이 되면 목록에서 제거하고 메모리 해제 */
static void flow_put(struct fio_flow *flow, unsigned long flow_counter,
				        unsigned int weight)
{
	if (!flow_lock)
		return;

	fio_sem_down(flow_lock);

	atomic_sub(&flow->flow_counter, flow_counter);  /* 스레드 기여분 차감 */
	atomic_sub(&flow->total_weight, weight);         /* weight 차감 */

	if (!--flow->refs) {
		assert(flow->flow_counter == 1);  /* 초기값 1만 남아야 함 */
		flist_del(&flow->list);
		sfree(flow);  /* 공유 메모리 해제 */
	}

	fio_sem_up(flow_lock);
}

/* [한국어] 작업(job) 시작 시 흐름 초기화
 * - flow 옵션이 설정된 경우에만 흐름 객체 획득
 * - 스레드별 카운터를 0으로 초기화
 * - 전체 weight에 이 작업의 weight를 원자적으로 추가 */
void flow_init_job(struct thread_data *td)
{
	if (td->o.flow) {
		td->flow = flow_get(td->o.flow_id);
		td->flow_counter = 0;
		atomic_add(&td->flow->total_weight, td->o.flow);
	}
}

/* [한국어] 작업(job) 종료 시 흐름 정리 - 흐름 참조 해제 */
void flow_exit_job(struct thread_data *td)
{
	if (td->flow) {
		flow_put(td->flow, td->flow_counter, td->o.flow);
		td->flow = NULL;
	}
}

/* [한국어] 전역 흐름 서브시스템 초기화
 * - 공유 메모리에 흐름 목록 할당
 * - 동기화용 세마포어 생성
 * - 흐름 목록 초기화 */
void flow_init(void)
{
	flow_list = smalloc(sizeof(*flow_list));
	if (!flow_list) {
		log_err("fio: smalloc pool exhausted\n");
		return;
	}

	flow_lock = fio_sem_init(FIO_SEM_UNLOCKED);
	if (!flow_lock) {
		log_err("fio: failed to allocate flow lock\n");
		sfree(flow_list);
		return;
	}

	INIT_FLIST_HEAD(flow_list);
}

/* [한국어] 전역 흐름 서브시스템 정리 - 세마포어와 흐름 목록 메모리 해제 */
void flow_exit(void)
{
	if (flow_lock)
		fio_sem_remove(flow_lock);
	if (flow_list)
		sfree(flow_list);
}
