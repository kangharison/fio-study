/*
 * [한국어 설명] null I/O 엔진 구현 (null.c)
 *
 * === 파일의 역할 ===
 * 실제 디스크/네트워크 I/O를 수행하지 않는 "가짜" 엔진으로, fio 자체의 프레임워크
 * (잡 스케줄링, io_u 생명주기, 통계 수집, 타이밍 측정) 오버헤드를 측정하거나
 * 유닛 테스트하기 위해 사용된다. 또한 외부 엔진 로딩 메커니즘(LD_LIBRARY_PATH를
 * 통해 .so를 dlopen)을 시연하는 레퍼런스로서 C++ 빌드(FIO_EXTERNAL_ENGINE)가
 * 조건부 포함되어 있다. I/O는 단지 배열에 쌓였다가 commit 단계에서 events로
 * 옮겨지기만 한다.
 *
 * ★ 중요: 이 파일은 fio "ioengine_ops 콜백 계약 전반을 이해하기 위한 기준 파일"
 * 이다. libaio.c / io_uring.c / nvme.c 등 실제 I/O 엔진의 콜백 구현을 읽기 전,
 * 이 파일의 각 콜백이 언제/어떤 전제로 호출되며 어떤 반환값이 코어에 어떤 의미를
 * 갖는지 숙지하는 용도로 참조한다. 실 I/O 엔진은 이 골격에 "실제 submit/poll"만
 * 덧붙인 구조라 보면 된다.
 *
 * ioengine_ops 콜백 계약 요약:
 *   .init        : td_io_init() 시 1회 호출. td->io_ops_data에 엔진별 상태 부착.
 *                  반환 0=성공, 음수=실패(잡 중단). 실행 스레드: 해당 잡 스레드.
 *   .cleanup     : 잡 종료 시 1회 호출. init에서 잡은 모든 자원을 해제.
 *   .open_file   : 각 fio_file마다 호출(FIO_DISKLESSIO면 코어가 건너뛸 수 있음).
 *                  0=성공, 음수=실패.
 *   .close_file  : open_file과 짝. null 엔진은 생략(코어 기본 동작에 위임).
 *   .queue       : io_u 1개를 엔진에 제출. 반환값:
 *                    FIO_Q_COMPLETED — 즉시 완료(동기). 코어가 put_io_u 수행.
 *                    FIO_Q_QUEUED    — 비동기 수락. 추후 getevents로 수확 예정.
 *                    FIO_Q_BUSY      — 큐 만원. 코어가 commit/getevents 후 재시도.
 *   .commit      : queue로 쌓인 배치를 실제 커널에 일괄 제출. 반환 0=성공.
 *   .getevents   : min_events 이상 완료까지 대기(또는 즉시 반환). 반환값=완료 수.
 *   .event       : getevents가 보고한 각 완료 인덱스(0..N-1)에 대응하는 io_u 반환.
 *   .flags       : FIO_SYNCIO / FIO_ASYNCIO_SETS_ISSUE_TIME / FIO_DISKLESSIO /
 *                  FIO_FAKEIO / FIO_NOEXTEND 등. 코어 동작 분기의 힌트.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend의 잡 루프가 load_ioengine("null") 후 init→queue→commit→getevents→event
 * 순으로 이 엔진의 콜백을 호출한다. iodepth==1인 경우 FIO_SYNCIO 플래그를 세팅해
 * queue()에서 즉시 COMPLETED를 돌려주고, iodepth>1이면 비동기처럼 큐잉/커밋
 * 단계를 모두 거쳐 완료 이벤트를 보고한다. 실행 컨텍스트는 각 잡 스레드이다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h: thread_data, io_u, ioengine_ops 등 공용 타입.
 * - io_u_queued / io_u_mark_submit: io_u.c의 통계/타이밍 유틸. commit 시 호출.
 * - backend.c: 잡 루프가 이 엔진의 콜백을 스케줄.
 * - 공유 상태: td->io_ops_data에 struct null_data 포인터가 저장되어 해당 잡의
 *   queue/commit/getevents/event 사이에서 상태를 공유한다. 잡 스레드 단독 소유.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct null_data:       큐 배열 + queued/events 카운트.
 * - null_init():            iodepth에 따라 SYNCIO/ASYNCIO 플래그와 배열 할당.
 * - null_queue():           FIO_Q_COMPLETED(동기) 또는 배열에 push 후 FIO_Q_QUEUED.
 * - null_commit():          queued → events 일괄 승격, 제출 시간 기록.
 * - null_getevents()/null_event(): events 수 반환 및 완료된 io_u 조회.
 * - fio_null_* 래퍼:        C ABI 콜백 진입점, td->io_ops_data를 null_data로 캐스팅.
 * - (선택) C++ NullData 래퍼 + get_ioengine(): FIO_EXTERNAL_ENGINE 빌드 시 dlopen용.
 */

/*
 * null engine
 *
 * IO engine that doesn't do any real IO transfers, it just pretends to.
 * The main purpose is to test fio itself.
 *
 * It also can act as external C++ engine - compiled with:
 *
 * g++ -O2 -g -shared -rdynamic -fPIC -o cpp_null null.c \
 *	-include ../config-host.h -DFIO_EXTERNAL_ENGINE
 *
 * to test it execute:
 *
 * LD_LIBRARY_PATH=./engines ./fio examples/cpp_null.fio
 *
 */
#include <stdlib.h>   /* [한국어] malloc/calloc/free — null_data 및 io_us 배열 할당 */
#include <assert.h>   /* [한국어] assert — init 후 null_data 비어있지 않은지 방어적 검사 */

#include "../fio.h"   /* [한국어] fio 코어 타입/매크로(thread_data, io_u, ioengine_ops, FIO_Q_*, dprint 등) */

/*
 * [한국어] null 엔진의 잡별 상태 구조체.
 * td->io_ops_data로 저장되며 잡 스레드 단독 소유(동기화 불필요).
 */
struct null_data {
	struct io_u **io_us;
	/* [한국어] 현재 큐잉된 io_u 포인터 배열(크기 = td->o.iodepth).
	 * 설정자: null_init()에서 iodepth>1일 때 calloc; null_queue()가 슬롯에 write.
	 * 읽는 자: null_event()가 인덱스로 io_u 복원; null_queued()/null_commit()가 순회.
	 * 값 범위: 유효한 io_u 포인터(큐잉 구간) 또는 미사용 슬롯.
	 * 동기화: 잡 스레드 전용이라 락 없음. iodepth==1이면 NULL 상태로 남는다. */

	int queued;
	/* [한국어] 현재 배열에 쌓여 있으나 아직 commit되지 않은 io_u 개수.
	 * 설정자: null_queue()에서 ++, null_commit()에서 0으로 리셋.
	 * 읽는 자: null_commit/null_queued에서 순회 상한으로 사용.
	 * 값 범위: 0..iodepth. 동기화: 잡 스레드 전용. */

	int events;
	/* [한국어] commit에 의해 "완료" 표시된 건수 — getevents가 반환할 값.
	 * 설정자: null_commit()가 queued를 events로 옮김; null_getevents()가 소비 후 0.
	 * 읽는 자: null_queue()에서 FIO_Q_BUSY 판단, null_getevents()가 리턴.
	 * 값 범위: 0..iodepth. 동기화: 잡 스레드 전용. */
};

/*
 * [한국어]
 * null_event - 완료 인덱스 event에 해당하는 io_u 포인터를 반환.
 * @nd:    null_data. @event: 0..events-1 범위 인덱스.
 * @return: io_us[event] (항상 유효, 완료 전 호출 금지).
 * 호출 체인: td_io_getevents → fio_null_event → [null_event].
 */
static struct io_u *null_event(struct null_data *nd, int event)
{
	return nd->io_us[event];  /* [한국어] 배열 직인덱싱 — null 엔진은 FIFO 완료 */
}

/*
 * [한국어]
 * null_getevents - 완료 이벤트 수를 보고하고 내부 카운터를 소비.
 * @nd:          null_data.
 * @min_events:  최소 완료 요청 수(>0이면 events를 그대로 소진).
 * @max:        (미사용) 최대 수집 수.
 * @t:          (미사용) 타임아웃.
 * @return:     반환 시점에 완료된 이벤트 수.
 * 호출 체인: td_io_getevents → fio_null_getevents → [null_getevents].
 */
static int null_getevents(struct null_data *nd, unsigned int min_events,
			  unsigned int fio_unused max,
			  const struct timespec fio_unused *t)
{
	int ret = 0;   /* [한국어] 반환할 이벤트 수 초기화 */

	if (min_events) {        /* [한국어] 호출자가 실제 수확 의사가 있을 때만 소비 */
		ret = nd->events;    /* [한국어] 현재 누적 완료 수를 결과로 */
		nd->events = 0;      /* [한국어] 소비 표시 — 다음 commit 전까지 0 */
	}

	return ret;   /* [한국어] fio 코어에 완료 수 통지 */
}

/*
 * [한국어]
 * null_queued - 큐잉된 io_u들의 issue_time을 현재 시각으로 설정
 * 비동기 모드에서 I/O 발행 시간을 정확히 기록하기 위해 commit 시점에 호출된다.
 */
static void null_queued(struct thread_data *td, struct null_data *nd)
{
	struct timespec now;   /* [한국어] 현재 시각 스냅샷 — 모든 대기 io_u에 같은 값 부여 */

	/* [한국어] 잡 옵션상 issue_time 기록이 필요한지 빠르게 조기 반환 */
	if (!fio_fill_issue_time(td))
		return;

	fio_gettime(&now, NULL);  /* [한국어] 단조 시계 시각 획득(지연 통계의 기준) */

	for (int i = 0; i < nd->queued; i++) {   /* [한국어] 큐에 쌓인 모든 io_u 순회 */
		struct io_u *io_u = nd->io_us[i];

		memcpy(&io_u->issue_time, &now, sizeof(now));  /* [한국어] 제출 시각 기록 */
		io_u_queued(td, io_u);                          /* [한국어] fio 코어 통계에 큐잉 이벤트 전달 */
	}
}

/*
 * [한국어]
 * null_commit - 큐잉된 I/O를 일괄 "완료" 처리
 * 실제 I/O 없이 큐잉된 수를 events로 옮기고 queued를 리셋한다.
 * io_u_mark_submit()으로 통계에 제출 수를 기록한다.
 */
static int null_commit(struct thread_data *td, struct null_data *nd)
{
	/* [한국어] 이전 커밋의 events가 아직 소비되지 않았다면 재커밋하지 않는다
	 * (getevents에서 0으로 비운 뒤에야 다음 배치를 커밋) */
	if (!nd->events) {
		null_queued(td, nd);   /* [한국어] 제출 시각 기록 + 코어 통계 */

#ifndef FIO_EXTERNAL_ENGINE
		/* [한국어] 내부(빌트인) 엔진 빌드에서만 제출 건수 통계 갱신.
		 * 외부 엔진(C++ shared lib) 빌드에선 fio 코어 심볼 미가용 가능성 대비 */
		io_u_mark_submit(td, nd->queued);
#endif
		nd->events = nd->queued;  /* [한국어] 모든 큐잉 건을 한 번에 완료로 승격 */
		nd->queued = 0;            /* [한국어] 큐 비우기 */
	}

	return 0;  /* [한국어] commit은 항상 성공 */
}

/*
 * [한국어]
 * null_queue - I/O 큐잉 (내부 구현)
 *
 * SYNCIO 모드(iodepth=1)이면 즉시 FIO_Q_COMPLETED를 반환한다.
 * 비동기 모드에서는 io_u를 배열에 추가하고 FIO_Q_QUEUED를 반환한다.
 * 이전 이벤트가 아직 소비되지 않았으면 FIO_Q_BUSY로 대기한다.
 */
static enum fio_q_status null_queue(struct thread_data *td,
				    struct null_data *nd, struct io_u *io_u)
{
	fio_ro_check(td, io_u);  /* [한국어] readonly 잡에 WRITE 요청이 오지 않았는지 검사 */

	/* [한국어] iodepth==1 → SYNCIO 경로: 큐잉 없이 즉시 완료 보고 */
	if (td->io_ops->flags & FIO_SYNCIO)
		return FIO_Q_COMPLETED;
	/* [한국어] 이전 배치의 완료 이벤트가 아직 소비되지 않음 → BUSY 반환
	 * (fio 코어가 getevents/event를 호출해 비운 뒤 재시도) */
	if (nd->events)
		return FIO_Q_BUSY;

	nd->io_us[nd->queued++] = io_u;  /* [한국어] 다음 빈 슬롯에 io_u 등록 */
	return FIO_Q_QUEUED;               /* [한국어] 비동기 대기 상태 통지 */
}

/*
 * [한국어]
 * null_open - 가짜 파일 오픈 콜백. 실제 FD 없이 항상 성공.
 * 호출 체인: td_io_open_file → fio_null_open → [null_open].
 */
static int null_open(struct null_data fio_unused *nd,
		     struct fio_file fio_unused *f)
{
	return 0;  /* [한국어] 성공만 반환 — null 엔진은 파일이 필요 없다 */
}

/*
 * [한국어]
 * null_cleanup - 잡 종료 시 null_data 해제.
 * 호출 체인: td_io_cleanup → fio_null_cleanup → [null_cleanup].
 */
static void null_cleanup(struct null_data *nd)
{
	if (nd) {                /* [한국어] init 실패 경로에서도 안전 */
		free(nd->io_us);  /* [한국어] iodepth>1이면 calloc된 배열 해제 (NULL free 안전) */
		free(nd);          /* [한국어] 구조체 본체 해제 */
	}
}

/*
 * [한국어]
 * null_init - null 엔진 초기화 (내부 구현)
 *
 * iodepth=1이면 SYNCIO 플래그를 설정하여 동기 모드로 동작하고,
 * iodepth>1이면 io_us 배열을 할당하여 비동기 큐잉을 지원한다.
 * td_set_ioengine_flags()로 변경된 플래그를 스레드에 반영한다.
 */
static struct null_data *null_init(struct thread_data *td)
{
	struct null_data *nd;
	nd = malloc(sizeof(*nd));    /* [한국어] 상태 구조체 할당(실패 시 아래 memset 전에 UB — fio 관행) */

	memset(nd, 0, sizeof(*nd));   /* [한국어] 전체 0 초기화 */

	if (td->o.iodepth != 1) {
		/* [한국어] 비동기 모드: iodepth 슬롯 배열 할당 + 비동기 엔진이 issue_time을
		 * 직접 채움을 코어에 알리는 플래그 셋 */
		nd->io_us = calloc(td->o.iodepth, sizeof(struct io_u *));
		td->io_ops->flags |= FIO_ASYNCIO_SETS_ISSUE_TIME;
	} else
		/* [한국어] iodepth==1 → 동기 경로로 표식 */
		td->io_ops->flags |= FIO_SYNCIO;

	td_set_ioengine_flags(td);  /* [한국어] 변경된 ioops->flags를 td 내부 캐시에 반영 */
	return nd;                    /* [한국어] io_ops_data에 저장될 포인터 반환 */
}

/* [한국어] 아래 블록은 순수 C 빌드(__cplusplus 미정의)일 때 사용되는 ioengine_ops 콜백들.
 * 각 함수는 td->io_ops_data(= struct null_data *)를 꺼내 내부 구현(null_*)에 위임하는 얇은 래퍼다. */
#ifndef __cplusplus

/* [한국어] fio_null_event - ioengine_ops.event 진입점. 인덱스로 완료된 io_u 조회. */
static struct io_u *fio_null_event(struct thread_data *td, int event)
{
	return null_event(td->io_ops_data, event);
}

/* [한국어] fio_null_getevents - ioengine_ops.getevents 진입점. 완료 수 수확. */
static int fio_null_getevents(struct thread_data *td, unsigned int min_events,
			      unsigned int max, const struct timespec *t)
{
	struct null_data *nd = td->io_ops_data;  /* [한국어] 상태 포인터 추출 */
	return null_getevents(nd, min_events, max, t);
}

/* [한국어] fio_null_commit - ioengine_ops.commit 진입점. queued→events 승격. */
static int fio_null_commit(struct thread_data *td)
{
	return null_commit(td, td->io_ops_data);
}

/* [한국어] fio_null_queue - ioengine_ops.queue 진입점. io_u 접수. */
static enum fio_q_status fio_null_queue(struct thread_data *td,
					struct io_u *io_u)
{
	return null_queue(td, td->io_ops_data, io_u);
}

/* [한국어] fio_null_open - ioengine_ops.open_file 진입점. no-op. */
static int fio_null_open(struct thread_data *td, struct fio_file *f)
{
	return null_open(td->io_ops_data, f);
}

/* [한국어] fio_null_cleanup - ioengine_ops.cleanup 진입점. 상태 해제. */
static void fio_null_cleanup(struct thread_data *td)
{
	null_cleanup(td->io_ops_data);
}

/* [한국어] fio_null_init - ioengine_ops.init 진입점. 상태 생성/부착. */
static int fio_null_init(struct thread_data *td)
{
	td->io_ops_data = null_init(td);  /* [한국어] 잡별 상태 할당 */
	assert(td->io_ops_data);            /* [한국어] 할당 실패 시 즉시 abort (디버그 보호) */
	return 0;
}

/*
 * [한국어] ioengine_ops — fio 코어가 잡 수명 동안 호출할 콜백 집합.
 * 이 구조체 자체는 register_ioengine()에 의해 전역 엔진 리스트에 링크된다.
 * 각 필드는 아래 §4 양식(설정자/읽는 자/값 범위/동기화) 기준으로 서술한다.
 */
static struct ioengine_ops ioengine = {
	.name		= "null",
	/* [한국어] 엔진 식별 문자열. 잡 파일의 `ioengine=null`과 매칭된다.
	 * 설정자: 이 초기화. 읽는 자: load_ioengine()의 strcmp 매칭.
	 * 값 범위: NUL 종결 ASCII. 동기화: 읽기 전용(등록 후 불변). */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전. fio 코어와 엔진 간 구조체 레이아웃 불일치를
	 * 런타임에 탐지하기 위한 상수. 설정자: 이 초기화. 읽는 자: register_ioengine().
	 * 값 범위: fio.h에 정의된 최신 매크로. 동기화: 불변. */

	.queue		= fio_null_queue,
	/* [한국어] io_u 제출 콜백. td_io_queue()가 호출.
	 * 설정자: 이 초기화. 읽는 자: 잡 루프.
	 * 반환값: FIO_Q_COMPLETED/QUEUED/BUSY — 각 의미는 상단 블록 참조.
	 * 동기화: 잡 스레드 단일 호출, 재진입 없음. */

	.commit		= fio_null_commit,
	/* [한국어] 배치 제출 콜백. queue 여러 번 후 td_io_commit()에서 호출.
	 * 반환: 0=성공, 음수=실패. 동기화: 잡 스레드 전용. */

	.getevents	= fio_null_getevents,
	/* [한국어] 완료 이벤트 수확 콜백. td_io_getevents()에서 호출.
	 * 반환: 수확된 이벤트 개수(>=min_events 보장이 이상적).
	 * 동기화: 잡 스레드 전용. null은 즉시 반환(대기 없음). */

	.event		= fio_null_event,
	/* [한국어] 완료된 io_u 조회 콜백. getevents가 N을 반환하면 코어가
	 * event(td, 0..N-1)을 N번 호출해 io_u를 회수한다.
	 * 반환: 유효한 io_u 포인터(NULL 불가). 동기화: 잡 스레드 전용. */

	.init		= fio_null_init,
	/* [한국어] 잡 시작 시 엔진 상태 초기화. 반환 0=성공, 음수=잡 중단.
	 * 동기화: 잡 스레드 전용, 1회만 호출. */

	.cleanup	= fio_null_cleanup,
	/* [한국어] 잡 종료 시 엔진 상태 해제. init와 대칭.
	 * 동기화: 잡 스레드 전용, 1회만 호출. */

	.open_file	= fio_null_open,
	/* [한국어] fio_file 오픈 훅. FIO_DISKLESSIO가 세팅되어 있어도 코어는
	 * 파일 메타(크기 등) 관리 목적으로 호출할 수 있음. 반환 0=성공.
	 * 동기화: 잡 스레드 전용. */

	.flags		= FIO_DISKLESSIO | FIO_FAKEIO,
	/* [한국어] 엔진 특성 플래그.
	 *   FIO_DISKLESSIO — 실제 파일/블록 디바이스가 필요 없음(코어가 파일 존재
	 *                    여부·크기 검증을 생략 가능).
	 *   FIO_FAKEIO     — 데이터 이동이 실제로 일어나지 않는 "가짜" 엔진임을 공지
	 *                    (verify 등 데이터 내용에 의존하는 기능이 무의미함을 알림).
	 * 설정자: 이 초기화 + null_init()에서 FIO_SYNCIO/FIO_ASYNCIO_SETS_ISSUE_TIME
	 *         추가. 읽는 자: 잡 루프 전반. 동기화: 잡 스레드 전용. */
};

/*
 * [한국어]
 * fio_null_register - 내부 빌트인 엔진을 전역 리스트에 등록.
 * `fio_init` 속성(= __attribute__((constructor)))에 의해 main() 진입 전에 호출된다.
 * 호출 체인: libc 동적 로더 → [fio_null_register] → register_ioengine().
 * 실행 컨텍스트: 프로세스 메인 스레드, 단 1회.
 */
static void fio_init fio_null_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] 전역 엔진 리스트(ioengine list)에 링크 — load_ioengine("null") 매칭을 가능케 함 */
}

/*
 * [한국어]
 * fio_null_unregister - 프로세스 종료 시 엔진 등록 해제.
 * `fio_exit`(= __attribute__((destructor))) 속성에 의해 main() 복귀 후 호출된다.
 * 정적 바이너리에서는 동작상 불필요하나 .so 빌드의 dlclose 대비 안전 장치.
 */
static void fio_exit fio_null_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] 전역 엔진 리스트에서 언링크 */
}

#else  /* [한국어] __cplusplus: 아래는 C++ 외부 엔진 빌드 경로 */

#ifdef FIO_EXTERNAL_ENGINE
/* [한국어] 이 섹션은 `g++ -shared -DFIO_EXTERNAL_ENGINE ... null.c`로 빌드되어
 * fio가 dlopen 하여 사용하는 외부 엔진("cpp_null") 형태의 예제를 제공한다.
 * NullData는 null_data를 감싼 얇은 RAII 래퍼이며, get_ioengine()이 진입점이다. */

/*
 * [한국어] NullData: null_data를 RAII로 감싼 C++ 클래스.
 * 생성자에서 null_init, 소멸자에서 null_cleanup을 호출해 자원 수명을 관리한다.
 * 필드 impl_는 기존 C 구조체 포인터로, 모든 C++ 메서드가 이를 통해 C 구현을 재사용한다.
 */
struct NullData {
	/* [한국어] 생성자 — 잡 init 시 1회. null_init()로 C 구현의 상태 구조체를 확보. */
	NullData(struct thread_data *td)
	{
		impl_ = null_init(td);  /* [한국어] C 측 상태 생성 */
		assert(impl_);            /* [한국어] OOM 방어 */
	}

	/* [한국어] 소멸자 — 잡 cleanup 시 1회. null_cleanup으로 C 구현 해제. */
	~NullData()
	{
		null_cleanup(impl_);   /* [한국어] C 측 해제 */
	}

	/* [한국어] td->io_ops_data를 NullData*로 복원하는 정적 헬퍼.
	 * 모든 extern "C" 진입점에서 첫 번째로 호출해 self-pointer를 얻는다. */
	static NullData *get(struct thread_data *td)
	{
		return reinterpret_cast<NullData *>(td->io_ops_data);  /* [한국어] void*→NullData* 타입 복원 */
	}

	/* [한국어] event 계약: 완료 인덱스 → io_u. C 구현(null_event)에 위임. */
	io_u *fio_null_event(struct thread_data *, int event)
	{
		return null_event(impl_, event);
	}

	/* [한국어] getevents 계약: 완료 수 수확. C 구현에 위임. */
	int fio_null_getevents(struct thread_data *, unsigned int min_events,
			       unsigned int max, const struct timespec *t)
	{
		return null_getevents(impl_, min_events, max, t);
	}

	/* [한국어] commit 계약: queued→events 승격. C 구현에 위임. */
	int fio_null_commit(struct thread_data *td)
	{
		return null_commit(td, impl_);
	}

	/* [한국어] queue 계약: io_u 접수. C 구현에 위임. */
	fio_q_status fio_null_queue(struct thread_data *td, struct io_u *io_u)
	{
		return null_queue(td, impl_, io_u);
	}

	/* [한국어] open_file 계약: no-op. C 구현에 위임. */
	int fio_null_open(struct thread_data *, struct fio_file *f)
	{
		return null_open(impl_, f);
	}

private:
	struct null_data *impl_;
	/* [한국어] 실제 null 엔진 상태에 대한 포인터.
	 * 설정자: 생성자. 읽는 자: 모든 메서드. 동기화: 객체 소유 스레드 전용. */
};

/* [한국어] extern "C": fio 코어가 dlsym으로 찾는 심볼 이름의 C ABI를 유지한다.
 * C++ 이름 맹글링(name mangling)을 비활성화해 "fio_null_queue" 같은 언더코어드
 * 심볼이 그대로 .so의 동적 심볼 테이블에 노출되도록 한다. */
extern "C" {

/* [한국어] ioengine_ops.event — C++ 래퍼 진입점. NullData::get으로 상태 복원 후 위임. */
static struct io_u *fio_null_event(struct thread_data *td, int event)
{
	return NullData::get(td)->fio_null_event(td, event);  /* [한국어] C++ 메서드 재호출(동명 메서드) */
}

/* [한국어] ioengine_ops.getevents — 외부 엔진용 wrapper. 계약은 상단 블록 참조. */
static int fio_null_getevents(struct thread_data *td, unsigned int min_events,
			      unsigned int max, const struct timespec *t)
{
	return NullData::get(td)->fio_null_getevents(td, min_events, max, t);  /* [한국어] 객체 메서드로 위임 */
}

/* [한국어] ioengine_ops.commit — 배치 제출 외부 엔진 wrapper. */
static int fio_null_commit(struct thread_data *td)
{
	return NullData::get(td)->fio_null_commit(td);  /* [한국어] 위임 */
}

/* [한국어] ioengine_ops.queue — io_u 접수 외부 엔진 wrapper. */
static fio_q_status fio_null_queue(struct thread_data *td, struct io_u *io_u)
{
	return NullData::get(td)->fio_null_queue(td, io_u);  /* [한국어] 위임 */
}

/* [한국어] ioengine_ops.open_file — 외부 엔진 wrapper. no-op. */
static int fio_null_open(struct thread_data *td, struct fio_file *f)
{
	return NullData::get(td)->fio_null_open(td, f);  /* [한국어] 위임 */
}

/* [한국어] ioengine_ops.init — 외부 엔진 wrapper. NullData RAII 객체 생성. */
static int fio_null_init(struct thread_data *td)
{
	td->io_ops_data = new NullData(td);   /* [한국어] new로 C++ 객체 생성 — 소멸은 cleanup에서 delete */
	return 0;                               /* [한국어] new 실패 시 std::bad_alloc throw — fio 관행상 중단 */
}

/* [한국어] ioengine_ops.cleanup — 외부 엔진 wrapper. NullData 소멸. */
static void fio_null_cleanup(struct thread_data *td)
{
	delete NullData::get(td);   /* [한국어] ~NullData()가 null_cleanup 호출 */
}

static struct ioengine_ops ioengine;  /* [한국어] 외부 엔진의 노출 심볼(아래 get_ioengine이 초기화) */

/*
 * [한국어]
 * get_ioengine - 외부 엔진 진입점. fio 코어가 dlsym("get_ioengine") 후 호출하여
 *                ioengine 구조체를 확보한다. 여기에 콜백들을 바인딩해 돌려준다.
 */
void get_ioengine(struct ioengine_ops **ioengine_ptr)
{
	*ioengine_ptr = &ioengine;   /* [한국어] 호출자에게 전역 ioengine의 주소 노출 */

	ioengine.name           = "cpp_null";             /* [한국어] 외부 엔진 식별자(잡파일 ioengine=cpp_null과 매칭) */
	ioengine.version        = FIO_IOOPS_VERSION;      /* [한국어] ABI 버전 검증용 */
	ioengine.queue          = fio_null_queue;         /* [한국어] queue 콜백 바인딩 */
	ioengine.commit         = fio_null_commit;        /* [한국어] commit 콜백 바인딩 */
	ioengine.getevents      = fio_null_getevents;     /* [한국어] getevents 콜백 바인딩 */
	ioengine.event          = fio_null_event;         /* [한국어] event 콜백 바인딩 */
	ioengine.init           = fio_null_init;          /* [한국어] init 콜백 바인딩 */
	ioengine.cleanup        = fio_null_cleanup;       /* [한국어] cleanup 콜백 바인딩 */
	ioengine.open_file      = fio_null_open;          /* [한국어] open_file 콜백 바인딩 */
	ioengine.flags          = FIO_DISKLESSIO | FIO_FAKEIO;  /* [한국어] 내부 빌드와 동일한 특성 플래그 */
}
}
#endif /* FIO_EXTERNAL_ENGINE */

#endif /* __cplusplus */
