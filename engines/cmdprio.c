/*
 * IO priority handling helper functions common to the libaio and io_uring
 * engines.
 */
/*
 * [한국어 설명]
 * cmdprio.c - libaio / io_uring / sg 등 비동기 엔진이 공유하는 "커맨드 단위
 * I/O 우선순위(ioprio) 설정" 공통 유틸리티 구현.
 *
 * === 파일의 역할 ===
 * 이 파일은 엔진별 구현이 아니라 "라이브러리"이다. 사용자가 잡 파일/CLI 에
 * cmdprio_percentage, cmdprio_bssplit, cmdprio_class, cmdprio_hint 등의 옵션을
 * 주면, 해당 옵션을 파싱·정규화하여 struct cmdprio 내부 테이블로 구축하고,
 * 매 I/O 제출 직전에 fio_cmdprio_set_ioprio()를 통해 io_u->ioprio / ioprio_hint
 * 필드를 덮어써서 커널 block layer(blk-mq I/O scheduler)에 우선순위 힌트를
 * 전달한다. 또한 우선순위별로 분리된 완료지연(clat) 통계 슬롯을
 * td->ts.clat_prio[ddir] 배열에 사전 할당하고, 각 cmdprio_prio 항목에
 * clat_prio_index를 기록하여 io_u 경로에서 add_clat_sample() 시 O(1) 로 올바른
 * 통계 슬롯을 찾을 수 있게 한다.
 *
 * 리눅스 ioprio_set(2) 배경:
 *   커널은 프로세스/스레드 단위 ioprio 기본값을 ioprio_set(2) 시스템콜로
 *   설정받고, 개별 I/O 요청에 대해서는 (1) libaio: iocb->aio_reqprio,
 *   (2) io_uring: SQE.ioprio, (3) sg: request_t->ioprio 필드를 통해
 *   커맨드 단위 ioprio 를 받는다. ioprio 값은 16비트로 인코딩되며
 *   상위 3비트가 CLASS, 중간 2비트가 HINT, 하위 11비트 중 일부가 LEVEL 이다.
 *     - IOPRIO_CLASS_RT   (1): Real-Time. BFQ/mq-deadline 에서 최우선 처리.
 *     - IOPRIO_CLASS_BE   (2): Best-Effort (기본). LEVEL(0~7)로 가중치 구분.
 *     - IOPRIO_CLASS_IDLE (3): 시스템이 유휴일 때만 디스패치.
 *     - IOPRIO_HINT_DEV_DURATION_LIMIT_* (1~7): NVMe/SCSI 장치에 "이 요청을
 *       이 시간 한도 안에 처리해달라"는 QoS 힌트. 커널 5.20+ 에서 지원.
 *   ioprio_value(class, level, hint) 매크로는 이 세 값을 16비트로 인코딩한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   ioengine .init  (libaio_init / fio_ioring_init / fio_sgio_init)
 *     → fio_cmdprio_init()                  [옵션→내부 테이블 구축]
 *         → fio_cmdprio_parse_and_gen()
 *             → fio_cmdprio_gen_perc()             (PERC 모드)
 *             → fio_cmdprio_parse_and_gen_bssplit()(BSSPLIT 모드)
 *   ioengine .queue (libaio_queue / fio_ioring_queue / ...)
 *     → (엔진별 cmdprio_ops 콜백) → fio_cmdprio_set_ioprio()
 *         → fio_cmdprio_percentage()  [ddir/bs 별 확률 조회]
 *         → rand_between(&td->prio_state, 0, 99) [확률 추첨]
 *         → io_u->ioprio / io_u->clat_prio_index 갱신
 *   ioengine .cleanup
 *     → fio_cmdprio_cleanup()
 * 실행 컨텍스트는 잡 스레드(유저스페이스) 단독이므로 별도 락이 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 *   - 상위 엔진: engines/libaio.c, engines/io_uring.c, engines/sg.c 가 자신의
 *     엔진 옵션 구조체 안에 struct cmdprio_options 를 임베드하고, init/queue/
 *     cleanup 에서 이 유틸의 함수를 호출한다. 엔진들은 struct cmdprio_ops 를
 *     통해 "내 엔진의 cmdprio 인스턴스를 어떻게 꺼낼지"를 콜백으로 등록한다.
 *   - 헤더: engines/cmdprio.h — struct cmdprio, cmdprio_prio, cmdprio_bsprio,
 *     cmdprio_bsprio_desc, cmdprio_options, cmdprio_mode enum, 공용 옵션
 *     파서(FIO_OPT_CMDPRIO 류)를 선언.
 *   - 라이브러리: lib/num2str? 가 아닌 lib/rand.c(rand_between), options.c
 *     (str_split_parse, split_parse_prio_ddir), stat.c(alloc_clat_prio_stat_ddir,
 *     free_clat_prio_stats), oslib/strcasestr 등.
 *   - 공유 상태:
 *       * td->o.ioprio_class / td->ioprio       : 잡 기본 ioprio(clat index 0)
 *       * td->ts.clat_prio[ddir][i]             : 우선순위별 완료지연 통계 슬롯
 *       * td->prio_state                        : 확률 추첨용 난수 상태
 *       * io_u->ioprio / io_u->clat_prio_index  : 엔진이 SQE/iocb로 전달
 *
 * === 주요 함수/구조체 요약 ===
 *   [진입점]
 *     - fio_cmdprio_init()          옵션 파싱 + 모드 결정 + 테이블/통계 구축.
 *     - fio_cmdprio_set_ioprio()    핫패스. 확률적으로 io_u->ioprio 덮어쓰기.
 *     - fio_cmdprio_cleanup()       bsprios / prios 배열 해제.
 *   [내부 파이프라인]
 *     - fio_cmdprio_parse_and_gen()            모드 분기 + 기본 class=RT.
 *     - fio_cmdprio_gen_perc()                 PERC 모드 테이블 구축.
 *     - fio_cmdprio_parse_and_gen_bssplit()    BSSPLIT 모드 전체 파이프.
 *     - fio_cmdprio_bssplit_parse()            str_split_parse 래퍼.
 *     - fio_cmdprio_bssplit_ddir()             ddir별 파서 콜백.
 *     - fio_cmdprio_generate_bsprio_desc()     파싱 결과→bsprio 배열.
 *     - fio_cmdprio_fill_bsprio()              동일 bs 그룹 채우기.
 *     - fio_cmdprio_percentage()               핫패스의 ddir/bs→perc 조회.
 *   [통계 인덱스 할당]
 *     - init_cmdprio_values() / assign_clat_prio_index() /
 *       find_clat_prio_index() / init_ts_clat_prio()
 *   [구조체]
 *     - struct cmdprio_parse_result : BSSPLIT 파서의 중간 산출물(임시).
 *     - struct cmdprio_values       : 고유 ioprio 값 수집용 임시 버퍼.
 *     - (외부) struct cmdprio / cmdprio_prio / cmdprio_bsprio /
 *               cmdprio_bsprio_desc / cmdprio_options — cmdprio.h.
 *
 * === cmdprio 옵션 문법 간이 레퍼런스 ===
 *   cmdprio_percentage=P[,P]     : ddir별로 P% 의 I/O에 지정 ioprio 적용.
 *   cmdprio_class=N[,N]          : 1=RT, 2=BE, 3=IDLE (미지정 시 기본 RT).
 *   cmdprio=LEVEL[,LEVEL]        : class 내 레벨(0~7).
 *   cmdprio_hint=HINT[,HINT]     : IOPRIO_HINT_DEV_DURATION_LIMIT_*.
 *   cmdprio_bssplit="BS/PCT/PRIO:BS/PCT/PRIO:..."
 *                                : 블록크기별로 여러 (percentage, prio) 쌍을
 *                                  지정. PRIO=-1 이면 cmdprio_class/cmdprio/
 *                                  cmdprio_hint 조합을 암묵적으로 사용.
 *   두 옵션(percentage vs bssplit)은 상호배타이며 동시에 주면 에러.
 *
 * === clat_prio 통계 인덱스 체계 ===
 *   add_clat_sample() 가 매 완료마다 "이 io_u의 ioprio 가 ts->clat_prio[] 배열에서
 *   몇 번째 슬롯인지" 를 선형 탐색하지 않도록, 초기화 단계에서 모든 고유
 *   ioprio 값을 수집해 index 를 사전 부여한다. 인덱스 0 은 항상 잡 기본
 *   ioprio(td->ioprio == ts->ioprio) 에 예약된다. cmdprio 가 덮어쓴 io_u 는
 *   clat_prio_index 를 해당 항목의 것으로 교체한다.
 */

#include "cmdprio.h"
/* [한국어] struct cmdprio, cmdprio_prio, cmdprio_bsprio[_desc], cmdprio_options,
 * enum cmdprio_mode, fio_cmdprio_* 공개 함수 시그니처와 함께 thread_data/io_u,
 * split_prio, alloc_clat_prio_stat_ddir/free_clat_prio_stats, ioprio_value(),
 * rand_between(), str_split_parse/split_parse_prio_ddir 등 본 유틸에 필요한
 * 모든 심볼을 간접 포함한다(cmdprio.h 가 fio.h 를 끌어들인다). */

/*
 * [한국어] struct cmdprio_parse_result — BSSPLIT 모드 파서의 ddir별 중간 산출물.
 *
 * cmdprio_bssplit 문자열은 "BS/PCT/PRIO:..." 형태이며, ddir(read/write)별로
 * 독립된 서브문자열을 가질 수 있다. 이 구조체는 파싱 1단계(문자열→배열) 결과를
 * 담는 단명(shared-lifetime=parse only) 버퍼이며, 2단계에서
 * fio_cmdprio_generate_bsprio_desc() 가 이 배열을 읽어 최종 bsprio_desc 테이블을
 * 만든 뒤 즉시 free 된다.
 */
struct cmdprio_parse_result {
	struct split_prio *entries;
	/* [한국어] split_parse_prio_ddir() 이 동적 할당해 채워주는 "한 ddir의 모든
	 * split 항목" 배열. 각 원소는 (bs, perc, prio) 3-튜플.
	 * 설정자: fio_cmdprio_bssplit_ddir() 이 split_parse_prio_ddir() 을 통해 할당.
	 * 읽는 자: fio_cmdprio_generate_bsprio_desc() / fio_cmdprio_fill_bsprio().
	 * 값 범위: 파서가 오름차순 bs 로 정렬해 돌려준다. NULL 가능(항목 0개 ddir).
	 * 동기화: 잡 스레드 내 초기화 단계에서만 접근하므로 락 불필요.
	 * 수명: 2단계 변환이 끝나면 fio_cmdprio_parse_and_gen_bssplit() 이 free. */

	int nr_entries;
	/* [한국어] entries 배열의 유효 항목 수(파싱된 BS/PCT/PRIO 튜플 개수).
	 * 설정자: split_parse_prio_ddir() 의 출력.
	 * 읽는 자: 2단계 루프 상한, 그리고 "항목 0개이면 해당 ddir 스킵" 판정.
	 * 값 범위: 0 ~ 수십. 0 이면 해당 ddir 에 cmdprio 미적용.
	 * 동기화: 동일 잡 스레드 단독 소유. */
};

/*
 * [한국어] struct cmdprio_values — 고유(unique) ioprio 값 수집용 임시 버퍼.
 *
 * clat 통계 인덱스 할당을 위해, 설정 단계에서 등장하는 모든 ioprio 값을
 * 중복 없이 모아 prios[] 에 쌓고, 그 인덱스(0..nr_prios-1)를 각 cmdprio_prio
 * 항목의 clat_prio_index 로 기록한다. 인덱스 0 은 잡 기본 ioprio 에 예약된다.
 * 수집이 끝나면 ts->clat_prio[ddir] 로 복사된 뒤 이 임시 버퍼는 free 된다.
 */
struct cmdprio_values {
	unsigned int *prios;
	/* [한국어] 이 ddir 에서 등장한 고유 ioprio 값들의 배열. prios[0] 은 항상
	 * 잡 기본 ioprio(ts->ioprio) 로 예약되어, cmdprio 가 적용되지 않은 io_u
	 * 도 안전한 통계 슬롯을 가리키도록 한다.
	 * 설정자: init_cmdprio_values() 가 calloc + prios[0]=ts->ioprio 초기화,
	 *         이후 assign_clat_prio_index() 가 새 값을 뒷자리에 append.
	 * 읽는 자: find_clat_prio_index() (중복 검색), init_ts_clat_prio()
	 *         (ts->clat_prio[ddir][i].ioprio 로 복사).
	 * 값 범위: 유효 ioprio 16비트 인코딩 값. calloc 로 0 초기화.
	 * 동기화: 잡 스레드 초기화 단계 단독. 실패 경로에서 free 필요. */

	int nr_prios;
	/* [한국어] prios[] 에 현재까지 저장된 고유 값의 개수. 다음 append 위치이기도 하다.
	 * 설정자: init_cmdprio_values() 에서 1 로 시작(인덱스 0 예약),
	 *         assign_clat_prio_index() 에서 신규 값 발견 시 ++.
	 * 읽는 자: find_clat_prio_index() 의 탐색 상한, init_ts_clat_prio() 의
	 *         alloc_clat_prio_stat_ddir(ts, ddir, nr_prios) 인자.
	 * 값 범위: 1 이상(항상 기본 ioprio 포함). 상한은 split entries + 1.
	 * 동기화: 잡 스레드 단독. */
};

/*
 * [한국어]
 * find_clat_prio_index - 고유 ioprio 배열에서 주어진 값의 인덱스 검색.
 *
 * @all_prios: cmdprio_values::prios 와 동일한 고유 값 배열(선형).
 * @nr_prios : 현재까지 수집된 고유 값 수.
 * @prio     : 검색하려는 ioprio 16비트 인코딩 값.
 * @return   : 발견 시 0..nr_prios-1 의 인덱스, 미발견 시 -1.
 *
 * 왜 필요한가: cmdprio 는 같은 (class, level, hint) 조합이 여러 split 항목에
 * 반복될 수 있다. 이 경우 동일 값을 통계 슬롯에서 하나로 합쳐야 의미 있는
 * per-ioprio 지연 분포가 나온다. 선형 검색이지만 nr_prios 가 매우 작아
 * (보통 < 8) 문제되지 않는다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계. 핫패스 아님.
 *
 * 호출 체인:
 *   fio_cmdprio_gen_perc / fio_cmdprio_fill_bsprio
 *     → assign_clat_prio_index() → [find_clat_prio_index]
 */
static int find_clat_prio_index(unsigned int *all_prios, int nr_prios,
				int32_t prio)
{
	int i;   /* [한국어] 선형 탐색용 인덱스. */

	for (i = 0; i < nr_prios; i++) {              /* [한국어] 현재까지 수집된 고유 값만 순회 — 배열 범위 밖 접근 방지. */
		if (all_prios[i] == prio)             /* [한국어] 인코딩된 ioprio 값 일치 여부(class/level/hint 모두 포함). */
			return i;                     /* [한국어] 동일 값 발견: 이미 할당된 clat_prio_index 재사용. */
	}

	return -1;                                    /* [한국어] 미발견: 호출자가 신규 슬롯을 할당해야 함을 알리는 센티넬. */
}

/**
 * assign_clat_prio_index - In order to avoid stat.c the need to loop through
 * all possible priorities each time add_clat_sample() / add_lat_sample() is
 * called, save which index to use in each cmdprio_prio. This will later be
 * propagated to the io_u, if the specific io_u was determined to use a cmdprio
 * priority value.
 *
 * [한국어]
 * assign_clat_prio_index - cmdprio_prio 항목에 clat 통계 인덱스를 부여.
 *
 * @prio  : 인덱스를 채워넣을 대상 cmdprio_prio (prio 필드가 이미 세팅되어 있어야 함).
 * @values: 이 ddir 의 고유 ioprio 수집 버퍼(초기화됨, 인덱스 0 = 기본 ioprio).
 *
 * 동작:
 *   1) find_clat_prio_index 로 동일 ioprio 값이 이미 수집되었는지 조회.
 *   2) 신규이면 values->prios[nr_prios] 에 append 하고 nr_prios++.
 *   3) 결정된 인덱스를 prio->clat_prio_index 에 저장.
 * 이 인덱스는 나중에 fio_cmdprio_set_ioprio() 가 io_u->clat_prio_index 로
 * 전파하여 stat.c 의 add_clat_sample() 가 O(1) 슬롯 접근을 하도록 돕는다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계 단독.
 *
 * 호출 체인:
 *   fio_cmdprio_gen_perc / fio_cmdprio_fill_bsprio
 *     → [assign_clat_prio_index] → find_clat_prio_index
 */
static void assign_clat_prio_index(struct cmdprio_prio *prio,
				   struct cmdprio_values *values)
{
	int clat_prio_index = find_clat_prio_index(values->prios,      /* [한국어] 기존 수집된 고유 ioprio 중 동일 값 검색. */
						   values->nr_prios,
						   prio->prio);
	if (clat_prio_index == -1) {                                   /* [한국어] 미발견 → 새 슬롯을 부여해야 함. */
		clat_prio_index = values->nr_prios;                    /* [한국어] 신규 인덱스는 현재 끝 위치. */
		values->prios[clat_prio_index] = prio->prio;           /* [한국어] prios 배열 끝에 새 ioprio 값 append. */
		values->nr_prios++;                                    /* [한국어] 고유 값 개수 증가 — 다음 append 위치 전진. */
	}
	prio->clat_prio_index = clat_prio_index;                       /* [한국어] 결정된 인덱스를 cmdprio_prio 에 기록(이후 io_u로 전파). */
}

/**
 * init_cmdprio_values - Allocate a temporary array that can hold all unique
 * priorities (per ddir), so that we can assign_clat_prio_index() for each
 * cmdprio_prio during setup. This temporary array is freed after setup.
 *
 * [한국어]
 * init_cmdprio_values - 고유 ioprio 수집용 임시 버퍼 할당 및 0번 슬롯 예약.
 *
 * @values          : 초기화 대상 cmdprio_values 구조체.
 * @max_unique_prios: 해당 ddir 에서 등장할 수 있는 cmdprio 항목의 최대 개수
 *                    (PERC 모드=1, BSSPLIT 모드=parse_res->nr_entries).
 * @ts              : 잡 통계 구조체(thread_stat). ts->ioprio 로 기본 ioprio 참조.
 * @return          : 0=성공, 1=calloc 실패(호출자 에러 경로로 점프).
 *
 * 왜 +1 인가: 인덱스 0 을 "잡 기본 ioprio" 전용으로 예약하기 때문. cmdprio 가
 * 적용되지 않은 io_u 도 clat_prio[0] 에 기록되어야 일관된 통계 구조가 된다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 *
 * 호출 체인:
 *   fio_cmdprio_gen_perc / fio_cmdprio_parse_and_gen_bssplit
 *     → [init_cmdprio_values]
 */
static int init_cmdprio_values(struct cmdprio_values *values,
			       int max_unique_prios, struct thread_stat *ts)
{
	values->prios = calloc(max_unique_prios + 1,                   /* [한국어] +1 은 인덱스 0 = 기본 ioprio 예약분. calloc 로 0 초기화. */
			       sizeof(*values->prios));
	if (!values->prios)                                            /* [한국어] OOM — 상위에서 free_clat_prio_stats 경로로 정리. */
		return 1;

	/* td->ioprio/ts->ioprio is always stored at index 0. */
	values->prios[0] = ts->ioprio;                                 /* [한국어] 잡 기본 ioprio 를 0번 슬롯에 고정 — 미적용 io_u 의 통계 귀속지. */
	values->nr_prios++;                                            /* [한국어] 이미 1개(기본)를 담았으므로 nr_prios=1 로 시작. */

	return 0;                                                      /* [한국어] 성공. */
}

/**
 * init_ts_clat_prio - Allocates and fills a clat_prio_stat array which holds
 * all unique priorities (per ddir).
 *
 * [한국어]
 * init_ts_clat_prio - thread_stat 에 우선순위별 clat 통계 슬롯 배열을 생성/채움.
 *
 * @ts    : 잡 통계 컨테이너.
 * @ddir  : 대상 데이터 방향(READ / WRITE). cmdprio 는 TRIM 미지원.
 * @values: 이 ddir 에서 수집된 고유 ioprio 목록.
 * @return: 0=성공, 1=할당 실패.
 *
 * 동작: alloc_clat_prio_stat_ddir() 가 ts->clat_prio[ddir] 에 nr_prios 개
 * clat_prio_stat 슬롯을 할당하고, 여기에 ioprio 값 라벨만 채워 넣는다.
 * 실제 샘플은 이후 stat.c 의 add_clat_sample() 이 io_u->clat_prio_index 로
 * 직접 접근해 채운다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 *
 * 호출 체인:
 *   fio_cmdprio_gen_perc / fio_cmdprio_parse_and_gen_bssplit
 *     → [init_ts_clat_prio] → alloc_clat_prio_stat_ddir (stat.c)
 */
static int init_ts_clat_prio(struct thread_stat *ts, enum fio_ddir ddir,
			     struct cmdprio_values *values)
{
	int i;   /* [한국어] 슬롯 채우기 루프 인덱스. */

	if (alloc_clat_prio_stat_ddir(ts, ddir, values->nr_prios))     /* [한국어] stat.c: ts->clat_prio[ddir]에 nr_prios 개 슬롯 할당. 실패 시 1 반환. */
		return 1;

	for (i = 0; i < values->nr_prios; i++)                         /* [한국어] 각 슬롯에 ioprio 라벨을 기록 — 샘플은 이후 add_clat_sample 에서 채워짐. */
		ts->clat_prio[ddir][i].ioprio = values->prios[i];

	return 0;                                                      /* [한국어] 성공. */
}

/*
 * [한국어]
 * fio_cmdprio_fill_bsprio - 파싱 결과에서 동일 블록 크기의 항목 구간(start..end)을
 *                           하나의 cmdprio_bsprio 에 채워 넣는다.
 *
 * @bsprio         : 채울 대상(블록 크기 한 종류에 대한 (perc, prio) 리스트).
 * @entries        : 파싱된 split_prio 배열(bs 로 정렬됨).
 * @values         : clat 인덱스 부여용 고유 ioprio 수집 버퍼.
 * @implicit_cmdprio: split 항목의 prio 가 -1(미지정) 일 때 대체로 쓸 ioprio
 *                   인코딩 값. cmdprio_class/cmdprio/cmdprio_hint 옵션으로 합성.
 * @start / @end   : entries 배열에서 동일 bs 구간의 시작/끝 인덱스(포함).
 * @return         : 0=성공, 1=OOM 또는 총 퍼센티지 > 100 위반.
 *
 * 총 퍼센티지 검증: 한 블록 크기에 대한 (perc) 총합이 100 을 넘으면 파싱 에러.
 * 100 미만은 허용되며, 나머지 비율은 "기본 ioprio 로 보내기"에 해당한다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 *
 * 호출 체인:
 *   fio_cmdprio_generate_bsprio_desc() → [fio_cmdprio_fill_bsprio]
 *     → assign_clat_prio_index() / calloc()
 */
static int fio_cmdprio_fill_bsprio(struct cmdprio_bsprio *bsprio,
				   struct split_prio *entries,
				   struct cmdprio_values *values,
				   int implicit_cmdprio, int start, int end)
{
	struct cmdprio_prio *prio;                         /* [한국어] 현재 채우는 항목 포인터(루프 내 갱신). */
	int i = end - start + 1;                           /* [한국어] 이 bs 그룹의 항목 수 — prios 배열 크기 계산에 재사용. */

	bsprio->prios = calloc(i, sizeof(*bsprio->prios)); /* [한국어] (perc, prio) 항목 배열 할당(0초기화). */
	if (!bsprio->prios)                                /* [한국어] OOM — 상위 cleanup 이 bsprio_desc 전체 free. */
		return 1;

	bsprio->bs = entries[start].bs;                    /* [한국어] 이 그룹을 대표하는 블록 크기. 핫패스의 선형 매칭 키. */
	bsprio->nr_prios = 0;                              /* [한국어] 유효 항목 수 — 아래 루프에서 성공 시마다 증가. */
	for (i = start; i <= end; i++) {                   /* [한국어] 동일 bs 구간 내 split 항목 순회. */
		prio = &bsprio->prios[bsprio->nr_prios];   /* [한국어] 현재 append 위치. */
		prio->perc = entries[i].perc;              /* [한국어] 이 (bs, prio) 조합에 할당된 퍼센티지. */
		if (entries[i].prio == -1)                 /* [한국어] -1 = 사용자가 split 에 prio 를 안 적음 → 암묵적 cmdprio 사용. */
			prio->prio = implicit_cmdprio;     /* [한국어] cmdprio_class/cmdprio/cmdprio_hint 로 합성된 기본 ioprio. */
		else
			prio->prio = entries[i].prio;      /* [한국어] split 에 명시된 ioprio 인코딩 값 그대로 사용. */
		assign_clat_prio_index(prio, values);      /* [한국어] 이 (perc, prio) 조합에 clat 통계 인덱스 부여. */
		bsprio->tot_perc += entries[i].perc;       /* [한국어] 이 bs 그룹의 누적 퍼센티지 — 핫패스에서 fio_cmdprio_percentage 반환값이 됨. */
		if (bsprio->tot_perc > 100) {              /* [한국어] 퍼센티지 총합 > 100 은 잘못된 입력. */
			log_err("fio: cmdprio_bssplit total percentage "
				"for bs: %"PRIu64" exceeds 100\n",
				bsprio->bs);
			free(bsprio->prios);               /* [한국어] 부분 할당 해제 — bsprio 자체는 호출자 소유. */
			return 1;
		}
		bsprio->nr_prios++;                        /* [한국어] 이번 항목 반영 확정 — 다음 슬롯으로 전진. */
	}

	return 0;                                          /* [한국어] 성공. */
}

/*
 * [한국어]
 * fio_cmdprio_generate_bsprio_desc - 파싱 결과(split 배열)를 "블록 크기별
 *                                     bsprio 리스트"로 재편성한다.
 *
 * @bsprio_desc    : 결과를 담을 cmdprio_bsprio_desc(이 ddir 전용).
 * @parse_res      : fio_cmdprio_bssplit_parse() 가 채워준 split 배열.
 * @values         : clat 인덱스 부여용 고유 ioprio 버퍼.
 * @implicit_cmdprio: split 항목 prio=-1 시 대체 ioprio.
 * @return         : 0=성공, 1=실패(상위가 fio_cmdprio_cleanup 로 정리).
 *
 * 이 함수는 2단 루프: (1) 서로 다른 bs 종류 개수를 세어 상위 배열 크기를
 * 확정한 뒤 calloc, (2) 다시 훑으며 동일 bs 구간마다 fio_cmdprio_fill_bsprio
 * 를 호출한다. 핫패스에서 블록 크기 매칭을 O(distinct-bs) 선형 검색만으로
 * 끝내기 위한 자료구조 설계이다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 */
static int
fio_cmdprio_generate_bsprio_desc(struct cmdprio_bsprio_desc *bsprio_desc,
				 struct cmdprio_parse_result *parse_res,
				 struct cmdprio_values *values,
				 int implicit_cmdprio)
{
	struct split_prio *entries = parse_res->entries;       /* [한국어] 파싱된 split 배열(bs 오름차순) 로컬 alias. */
	int nr_entries = parse_res->nr_entries;                /* [한국어] 총 split 항목 수. */
	struct cmdprio_bsprio *bsprio;                         /* [한국어] 현재 채우는 bs 그룹 포인터. */
	int i, start, count = 0;                               /* [한국어] i=탐색 커서, start=현재 bs 그룹의 시작 인덱스, count=고유 bs 개수. */

	/*
	 * The parsed result is sorted by blocksize, so count only the number
	 * of different blocksizes, to know how many cmdprio_bsprio we need.
	 */
	for (i = 0; i < nr_entries; i++) {                     /* [한국어] 1차 스캔: 고유 bs 개수 세기. */
		while (i + 1 < nr_entries && entries[i].bs == entries[i + 1].bs) /* [한국어] 연속된 동일 bs 구간을 한 번에 건너뜀. */
			i++;
		count++;                                        /* [한국어] 이 구간이 하나의 bsprio 슬롯에 대응. */
	}

	/*
	 * This allocation is not freed on error. Instead, the calling function
	 * is responsible for calling fio_cmdprio_cleanup() on error.
	 */
	bsprio_desc->bsprios = calloc(count, sizeof(*bsprio_desc->bsprios)); /* [한국어] 고유 bs 수만큼 bsprio 슬롯 확보. */
	if (!bsprio_desc->bsprios)                             /* [한국어] OOM — 상위가 cleanup 호출로 기존 자원 회수. */
		return 1;

	start = 0;                                             /* [한국어] 첫 그룹의 시작은 0. */
	bsprio_desc->nr_bsprios = 0;                           /* [한국어] 누적 카운터 초기화. */
	for (i = 0; i < nr_entries; i++) {                     /* [한국어] 2차 스캔: 실제로 그룹별 fill. */
		while (i + 1 < nr_entries && entries[i].bs == entries[i + 1].bs) /* [한국어] 동일 bs 구간 끝까지 진전(i 가 그룹 마지막 인덱스가 됨). */
			i++;
		bsprio = &bsprio_desc->bsprios[bsprio_desc->nr_bsprios]; /* [한국어] 이 그룹에 대응하는 슬롯. */
		/*
		 * All parsed entries with the same blocksize get saved in the
		 * same cmdprio_bsprio, to expedite the search in the hot path.
		 */
		if (fio_cmdprio_fill_bsprio(bsprio, entries, values,           /* [한국어] [start..i] 구간을 bsprio 1개에 채움. */
					    implicit_cmdprio, start, i))
			return 1;

		start = i + 1;                                 /* [한국어] 다음 그룹의 시작 인덱스 갱신. */
		bsprio_desc->nr_bsprios++;                     /* [한국어] 채워진 bsprio 슬롯 수 증가. */
	}

	return 0;                                              /* [한국어] 성공. */
}

/*
 * [한국어]
 * fio_cmdprio_bssplit_ddir - str_split_parse() 가 각 ddir 서브문자열에 대해
 *                            호출하는 콜백. 해당 ddir 의 parse_result 를 채운다.
 *
 * @to     : 잡 thread_options (에러 메시지용 잡 이름 등).
 * @cb_arg : 호출자(fio_cmdprio_bssplit_parse) 가 넘긴 parse_res 배열 베이스.
 * @ddir   : 콜백을 호출한 ddir(READ/WRITE/TRIM).
 * @str    : 이 ddir 에 대응하는 서브문자열(이미 ddir 구분자로 쪼개진 상태).
 * @data   : bssplit 와 공용 파서 인터페이스의 "data-prio 스위치". 여기선 false
 *           (prio 전용 파서 → split_parse_prio_ddir 사용)로 전달되어 미사용.
 * @return : 0=성공, 1=파싱 실패(상위가 에러 전파).
 *
 * TRIM 은 cmdprio 대상이 아니므로 즉시 성공 반환(무시). 나머지는
 * split_parse_prio_ddir() 이 문자열을 tokenize 해 split_prio 배열로 채운다.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계, 파서 내부 콜백.
 */
static int fio_cmdprio_bssplit_ddir(struct thread_options *to, void *cb_arg,
				    enum fio_ddir ddir, char *str, bool data)
{
	struct cmdprio_parse_result *parse_res_arr = cb_arg;         /* [한국어] 콜백에 실려 온 ddir 배열 베이스. */
	struct cmdprio_parse_result *parse_res = &parse_res_arr[ddir]; /* [한국어] 이번 콜백의 ddir 전용 슬롯. */

	if (ddir == DDIR_TRIM)                                       /* [한국어] cmdprio 는 TRIM 미지원 → 조용히 스킵. */
		return 0;

	if (split_parse_prio_ddir(to, &parse_res->entries,           /* [한국어] options.c: "BS/PCT/PRIO:..." 를 split_prio 배열로 파싱. */
				  &parse_res->nr_entries, str))
		return 1;                                            /* [한국어] 파서가 로그를 남기고 실패 — 상위로 전파. */

	return 0;                                                    /* [한국어] 성공. */
}

/*
 * [한국어]
 * fio_cmdprio_bssplit_parse - cmdprio_bssplit 옵션 원본 문자열을 안전하게
 *                             복제/정리한 뒤 str_split_parse() 로 ddir 파서를 기동.
 *
 * @td       : 잡 컨텍스트(파서에 옵션/로그 포인터 제공).
 * @input    : 사용자 원본 문자열(const, 수정 금지).
 * @parse_res: CMDPRIO_RWDIR_CNT 크기 배열. 각 ddir 슬롯이 이 함수 이후 채워짐.
 * @return   : 0=성공, 비0=파싱 실패.
 *
 * strdup 으로 쓰기 가능 사본을 만들고 앞뒤 공백을 제거한 뒤 파서에 넘긴다.
 * str_split_parse 의 마지막 인자 false 는 "data-bssplit 아님(=prio bssplit)" 신호.
 */
static int fio_cmdprio_bssplit_parse(struct thread_data *td, const char *input,
				     struct cmdprio_parse_result *parse_res)
{
	char *str, *p;        /* [한국어] str=파서에 넘길 작업 포인터, p=free 할 원본 alias. */
	int ret = 0;          /* [한국어] 파서 반환값 누적 변수(0 으로 초기화). */

	p = str = strdup(input);   /* [한국어] 원본 보호용 복제. p 는 free 대상, str 은 trim 으로 이동될 수 있음. */

	strip_blank_front(&str);   /* [한국어] 선두 공백 제거(포인터 자체가 앞으로 이동할 수 있음). */
	strip_blank_end(str);      /* [한국어] 꼬리 공백 제거(null-terminator 를 당김). */

	ret = str_split_parse(td, str, fio_cmdprio_bssplit_ddir, parse_res, /* [한국어] ddir 구분자(',')로 쪼개 각 조각을 콜백에 전달. */
			      false);

	free(p);                    /* [한국어] 원본 복제 메모리 해제(이동된 str 이 아니라 p 사용에 주의). */
	return ret;                 /* [한국어] 파서 결과 그대로 전파. */
}

/**
 * fio_cmdprio_percentage - Returns the percentage of I/Os that should
 * use a cmdprio priority value (rather than the default context priority).
 *
 * For CMDPRIO_MODE_BSSPLIT, if the percentage is non-zero, we will also
 * return the matching bsprio, to avoid the same linear search elsewhere.
 * For CMDPRIO_MODE_PERC, we will never return a bsprio.
 *
 * [한국어]
 * fio_cmdprio_percentage - 현재 io_u 에 cmdprio 를 적용할 "확률(%)" 조회.
 *
 * @cmdprio: 이 잡의 cmdprio 테이블.
 * @io_u   : 이번에 queue 될 I/O 유닛(ddir/buflen 만 읽음).
 * @bsprio : [out] BSSPLIT 모드에서 매칭된 bsprio 포인터(또는 NULL).
 * @return : 0..100. 0 이면 "이 I/O 에는 cmdprio 미적용"을 의미.
 *
 * 핫패스. 모드별 분기:
 *   - PERC   : cmdprio->perc_entry[ddir].perc 를 그대로 반환. bsprio=NULL.
 *   - BSSPLIT: bsprio_desc[ddir] 를 선형 스캔해 io_u->buflen 과 일치하는
 *              bs 를 찾는다. 없으면 0, 있으면 tot_perc 와 해당 bsprio 를 반환.
 *
 * assert(0) 경로는 "cmdprio 가 NONE 인데 엔진이 set_ioprio 를 호출했다"는
 * 계약 위반 — 엔진 쪽 버그로 간주.
 *
 * 실행 컨텍스트: 잡 스레드 queue 경로(핫패스).
 *
 * 호출 체인:
 *   fio_cmdprio_set_ioprio() → [fio_cmdprio_percentage]
 */
static int fio_cmdprio_percentage(struct cmdprio *cmdprio, struct io_u *io_u,
				  struct cmdprio_bsprio **bsprio)
{
	struct cmdprio_bsprio *bsprio_entry;              /* [한국어] BSSPLIT 선형 스캔 중 현재 검사 항목. */
	enum fio_ddir ddir = io_u->ddir;                  /* [한국어] 이 I/O 의 방향 — 테이블 인덱스. */
	int i;                                            /* [한국어] 스캔 인덱스. */

	switch (cmdprio->mode) {                          /* [한국어] 잡 초기화 때 결정된 모드로 분기. */
	case CMDPRIO_MODE_PERC:
		*bsprio = NULL;                           /* [한국어] PERC 모드는 bs별 매칭이 없으므로 항상 NULL. */
		return cmdprio->perc_entry[ddir].perc;    /* [한국어] ddir 고정 퍼센티지(0 이면 미적용). */
	case CMDPRIO_MODE_BSSPLIT:
		for (i = 0; i < cmdprio->bsprio_desc[ddir].nr_bsprios; i++) { /* [한국어] 고유 bs 수만큼 선형 탐색(보통 수 개). */
			bsprio_entry = &cmdprio->bsprio_desc[ddir].bsprios[i];
			if (bsprio_entry->bs == io_u->buflen) { /* [한국어] 이번 I/O 블록 크기와 일치? */
				*bsprio = bsprio_entry;         /* [한국어] 호출자에게 매칭된 bsprio 포인터 전달(재탐색 방지). */
				return bsprio_entry->tot_perc;  /* [한국어] 이 bs 에 정의된 총 퍼센티지. */
			}
		}
		break;                                    /* [한국어] 매칭 실패 → 아래 fallthrough 로 0 반환. */
	default:
		/*
		 * An I/O engine should never call this function if cmdprio
		 * is not is use.
		 */
		assert(0);                                /* [한국어] 계약 위반 감지: NONE 모드인데 호출됨. */
	}

	/*
	 * This is totally fine, the given blocksize simply does not
	 * have any (non-zero) cmdprio_bssplit entries defined.
	 */
	*bsprio = NULL;                                   /* [한국어] 매칭 실패 시 명시적 NULL. */
	return 0;                                         /* [한국어] 0 = 이 I/O 에는 cmdprio 적용 안 함. */
}

/**
 * fio_cmdprio_set_ioprio - Set an io_u ioprio according to cmdprio options
 *
 * Generates a random percentage value to determine if an io_u ioprio needs
 * to be set. If the random percentage value is within the user specified
 * percentage of I/Os that should use a cmdprio priority value (rather than
 * the default priority), then this function updates the io_u with an ioprio
 * value as defined by the cmdprio/cmdprio_hint/cmdprio_class or
 * cmdprio_bssplit options.
 *
 * Return true if the io_u ioprio was changed and false otherwise.
 *
 * [한국어]
 * fio_cmdprio_set_ioprio - 핫패스. 확률 추첨을 통해 io_u->ioprio 를 덮어쓴다.
 *
 * @td     : 잡 컨텍스트(난수 상태 td->prio_state 접근).
 * @cmdprio: 이 잡의 cmdprio 테이블.
 * @io_u   : 이번에 queue 될 I/O 유닛. 성공 시 ioprio / clat_prio_index 갱신.
 * @return : true=ioprio 변경됨(엔진은 그 값을 SQE/iocb 에 전달),
 *           false=변경 없음(엔진 기본 ioprio 유지).
 *
 * 알고리즘:
 *   1) fio_cmdprio_percentage() 로 "적용 확률 p" 와 bs 매칭 결과 조회.
 *   2) p==0 이면 즉시 false.
 *   3) rand_between(0,99) 로 0..99 난수 추첨, rand >= p 면 false.
 *   4) PERC 모드: perc_entry[ddir].prio 적용.
 *      BSSPLIT 모드: 누적 퍼센티지로 bsprio->prios[] 를 순회하여 선택(CDF).
 *   5) io_u->ioprio 및 io_u->clat_prio_index 갱신 후 true.
 *
 * 난수 공정성: rand_between 은 td->prio_state 전용 RNG 를 사용하므로, 잡의
 * 메인 I/O 생성 RNG(tdsr) 시퀀스와 독립적이다. 같은 시드를 줘도 cmdprio 추첨은
 * I/O 생성과 결합되지 않는다.
 *
 * 실행 컨텍스트: 잡 스레드 queue 경로(핫패스, 매 io_u 마다 1회).
 *
 * 호출 체인:
 *   ioengine queue (libaio_queue / fio_ioring_queue / fio_sgio_queue)
 *     → (엔진별 cmdprio_ops wrapper) → [fio_cmdprio_set_ioprio]
 *       → fio_cmdprio_percentage / rand_between
 */
bool fio_cmdprio_set_ioprio(struct thread_data *td, struct cmdprio *cmdprio,
			    struct io_u *io_u)
{
	struct cmdprio_bsprio *bsprio;     /* [한국어] BSSPLIT 매칭 결과 포인터(out-param). */
	unsigned int p, rand;              /* [한국어] p=적용확률(%), rand=0..99 추첨값. */
	uint32_t perc = 0;                 /* [한국어] BSSPLIT CDF 누적합. */
	int i;                             /* [한국어] prios 배열 인덱스. */

	p = fio_cmdprio_percentage(cmdprio, io_u, &bsprio); /* [한국어] 이 I/O 에 cmdprio 를 적용할 확률과 bs 매칭 획득. */
	if (!p)                                             /* [한국어] 0% → 변경 없음. 핫패스 조기 탈출. */
		return false;

	rand = rand_between(&td->prio_state, 0, 99);        /* [한국어] 균등분포 0..99 추첨. prio_state 는 이 잡 전용. */
	if (rand >= p)                                      /* [한국어] 추첨값이 확률 바깥 → 이번 I/O 는 기본 ioprio 유지. */
		return false;

	switch (cmdprio->mode) {                            /* [한국어] 모드별 ioprio 선택 방법 분기. */
	case CMDPRIO_MODE_PERC:
		io_u->ioprio = cmdprio->perc_entry[io_u->ddir].prio;               /* [한국어] ddir 고정 ioprio 덮어쓰기. */
		io_u->clat_prio_index =                                            /* [한국어] add_clat_sample 이 쓸 통계 슬롯 인덱스 전파. */
			cmdprio->perc_entry[io_u->ddir].clat_prio_index;
		return true;
	case CMDPRIO_MODE_BSSPLIT:
		assert(bsprio);                                                    /* [한국어] p>0 이면 percentage 가 bsprio 도 채워줬어야 함. */
		for (i = 0; i < bsprio->nr_prios; i++) {                           /* [한국어] (perc, prio) 항목들의 CDF 상에서 추첨값 위치 찾기. */
			struct cmdprio_prio *prio = &bsprio->prios[i];

			perc += prio->perc;                                        /* [한국어] CDF 누적 — 항목별 퍼센티지 가산. */
			if (rand < perc) {                                         /* [한국어] 이 항목 구간에 추첨값 포함 → 선택. */
				io_u->ioprio = prio->prio;                         /* [한국어] 해당 (class/level/hint) 조합의 ioprio 적용. */
				io_u->clat_prio_index = prio->clat_prio_index;     /* [한국어] 동일 값끼리 공유되는 통계 슬롯 인덱스. */
				return true;
			}
		}
		break;                                                             /* [한국어] 도달 시 아래 assert(0) 로. */
	default:
		assert(0);                                                         /* [한국어] NONE 모드에서 set_ioprio 호출은 계약 위반. */
	}

	/* When rand < p (total perc), we should always find a cmdprio_prio. */
	assert(0);                                                                 /* [한국어] CDF 총합 == tot_perc == p 이므로 rand<p 면 반드시 매칭. */
	return false;                                                              /* [한국어] 도달 불가 — 컴파일러 경고 억제용. */
}

/*
 * [한국어]
 * fio_cmdprio_gen_perc - PERC 모드 테이블 구축.
 *
 * @td     : 잡 컨텍스트(ts / ddir 활성 여부 판정).
 * @cmdprio: 목적지 테이블. perc_entry[ddir] 를 채운다.
 * @return : 0=성공, 1=실패(상위로 전파, 부분 자원 cleanup 됨).
 *
 * 각 ddir 에 대해:
 *   - percentage[ddir] 가 0 이거나 해당 ddir 가 잡에 비활성이면 스킵.
 *   - 고유 ioprio 수집 버퍼(크기 1+1=2) 초기화.
 *   - ioprio_value(class, level, hint) 로 ioprio 인코딩.
 *   - assign_clat_prio_index 로 통계 인덱스 부여.
 *   - init_ts_clat_prio 로 ts->clat_prio[ddir] 슬롯 할당/라벨.
 *   - 성공 경로에서 임시 prios 버퍼 해제.
 * 실패 시 모든 ddir 의 values->prios 해제 + free_clat_prio_stats 로 ts 정리.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 */
static int fio_cmdprio_gen_perc(struct thread_data *td, struct cmdprio *cmdprio)
{
	struct cmdprio_options *options = cmdprio->options;       /* [한국어] 엔진이 넘겨준 사용자 옵션 스냅샷. */
	struct cmdprio_prio *prio;                                /* [한국어] 현재 ddir 의 perc_entry 포인터. */
	struct cmdprio_values values[CMDPRIO_RWDIR_CNT] = {};     /* [한국어] ddir별 임시 수집 버퍼(zero-init). */
	struct thread_stat *ts = &td->ts;                         /* [한국어] clat 슬롯 부착 대상 통계 컨테이너. */
	enum fio_ddir ddir;                                       /* [한국어] ddir 순회 변수. */
	int ret;                                                  /* [한국어] 서브루틴 반환값 누적. */

	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {        /* [한국어] READ/WRITE 두 방향만(TRIM 미지원). */
		/*
		 * Do not allocate a clat_prio array nor set the cmdprio struct
		 * if zero percent of the I/Os (for the ddir) should use a
		 * cmdprio priority value, or when the ddir is not enabled.
		 */
		if (!options->percentage[ddir] ||                 /* [한국어] 0% → 이 ddir 에는 cmdprio 자체가 의미 없음. */
		    (ddir == DDIR_READ && !td_read(td)) ||        /* [한국어] 잡이 읽기를 하지 않으면 skip. */
		    (ddir == DDIR_WRITE && !td_write(td)))        /* [한국어] 잡이 쓰기를 하지 않으면 skip. */
			continue;

		ret = init_cmdprio_values(&values[ddir], 1, ts);  /* [한국어] PERC 는 ddir당 항목 1개 → max_unique=1, +1은 기본 포함. */
		if (ret)
			goto err;

		prio = &cmdprio->perc_entry[ddir];                /* [한국어] 핫패스가 읽을 최종 저장 위치. */
		prio->perc = options->percentage[ddir];           /* [한국어] 적용 확률(%) 저장. */
		prio->prio = ioprio_value(options->class[ddir],   /* [한국어] (class, level, hint) 를 16비트 ioprio 로 인코딩. */
					  options->level[ddir],
					  options->hint[ddir]);
		assign_clat_prio_index(prio, &values[ddir]);      /* [한국어] clat 통계 인덱스 부여(인덱스 1, 0은 기본 ioprio). */

		ret = init_ts_clat_prio(ts, ddir, &values[ddir]); /* [한국어] ts->clat_prio[ddir] 슬롯 할당/라벨. */
		if (ret)
			goto err;

		free(values[ddir].prios);                         /* [한국어] 임시 버퍼 해제 — 정상 경로에서 즉시 회수. */
		values[ddir].prios = NULL;                        /* [한국어] err 경로가 중복 free 하지 않도록 NULL 설정. */
		values[ddir].nr_prios = 0;                        /* [한국어] 일관성 유지. */
	}

	return 0;                                                 /* [한국어] 모든 ddir 성공. */

err:
	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++)          /* [한국어] 부분 할당된 모든 임시 버퍼 회수(NULL 도 free 안전). */
		free(values[ddir].prios);
	free_clat_prio_stats(ts);                                 /* [한국어] 이미 생성된 ts->clat_prio 도 stat.c 에서 일괄 해제. */

	return ret;                                               /* [한국어] 실패 원인 전파. */
}

/*
 * [한국어]
 * fio_cmdprio_parse_and_gen_bssplit - BSSPLIT 모드 전체 파이프라인.
 *
 * @td     : 잡 컨텍스트.
 * @cmdprio: 목적지 cmdprio (bsprio_desc[ddir] 를 채움).
 * @return : 0=성공, 1=실패.
 *
 * 단계:
 *   1) fio_cmdprio_bssplit_parse() — 옵션 문자열 → parse_res[ddir] 배열.
 *   2) 각 ddir 에 대해:
 *      - 항목 0 이거나 ddir 비활성이면 parse_res 만 정리하고 skip.
 *      - init_cmdprio_values() — 고유 ioprio 버퍼(크기 nr_entries+1).
 *      - implicit_cmdprio = ioprio_value(class, level, hint)  [split prio=-1 대체용].
 *      - fio_cmdprio_generate_bsprio_desc() — bs별 bsprio 테이블 생성.
 *      - parse_res 정리.
 *      - init_ts_clat_prio() — ts->clat_prio[ddir] 슬롯 할당/라벨.
 *      - values 임시 버퍼 정리.
 *   실패 시 모든 parse_res / values / clat_prio / bsprios 를 한꺼번에 회수.
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 */
static int fio_cmdprio_parse_and_gen_bssplit(struct thread_data *td,
					     struct cmdprio *cmdprio)
{
	struct cmdprio_options *options = cmdprio->options;               /* [한국어] 사용자 옵션(문자열/배열 포함). */
	struct cmdprio_parse_result parse_res[CMDPRIO_RWDIR_CNT] = {};    /* [한국어] ddir별 1차 파싱 결과(0-init). */
	struct cmdprio_values values[CMDPRIO_RWDIR_CNT] = {};             /* [한국어] ddir별 고유 ioprio 수집 버퍼. */
	struct thread_stat *ts = &td->ts;                                 /* [한국어] clat 슬롯 부착 대상. */
	int ret, implicit_cmdprio;                                        /* [한국어] ret=반환 누적, implicit=split prio=-1 대체. */
	enum fio_ddir ddir;                                               /* [한국어] ddir 순회 변수. */

	ret = fio_cmdprio_bssplit_parse(td, options->bssplit_str,         /* [한국어] 1단계: 문자열 → ddir별 split 배열. */
					&parse_res[0]);
	if (ret)
		goto err;

	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {                /* [한국어] 각 ddir 별로 2단계 변환/등록 수행. */
		/*
		 * Do not allocate a clat_prio array nor set the cmdprio structs
		 * if there are no non-zero entries (for the ddir), or when the
		 * ddir is not enabled.
		 */
		if (!parse_res[ddir].nr_entries ||                        /* [한국어] 이 ddir 에 split 항목이 하나도 없음. */
		    (ddir == DDIR_READ && !td_read(td)) ||                /* [한국어] 잡이 읽기를 하지 않음. */
		    (ddir == DDIR_WRITE && !td_write(td))) {              /* [한국어] 잡이 쓰기를 하지 않음. */
			free(parse_res[ddir].entries);                    /* [한국어] 불필요해진 파싱 결과 정리. */
			parse_res[ddir].entries = NULL;                   /* [한국어] 이중 free 방지. */
			parse_res[ddir].nr_entries = 0;                   /* [한국어] 일관성 유지. */
			continue;                                         /* [한국어] 다음 ddir. */
		}

		ret = init_cmdprio_values(&values[ddir],                  /* [한국어] 고유 ioprio 수집 버퍼 할당(항목 수 + 1). */
					  parse_res[ddir].nr_entries, ts);
		if (ret)
			goto err;

		implicit_cmdprio = ioprio_value(options->class[ddir],     /* [한국어] split 에 prio=-1 로 표기된 항목에 쓸 기본 ioprio. */
						options->level[ddir],
						options->hint[ddir]);

		ret = fio_cmdprio_generate_bsprio_desc(&cmdprio->bsprio_desc[ddir], /* [한국어] 핵심 변환: split 배열 → bs별 bsprio 테이블. */
						       &parse_res[ddir],
						       &values[ddir],
						       implicit_cmdprio);
		if (ret)
			goto err;

		free(parse_res[ddir].entries);                            /* [한국어] 변환 완료 — 1차 파싱 결과 회수. */
		parse_res[ddir].entries = NULL;                           /* [한국어] err 경로 중복 free 방지. */
		parse_res[ddir].nr_entries = 0;                           /* [한국어] 일관성 유지. */

		ret = init_ts_clat_prio(ts, ddir, &values[ddir]);         /* [한국어] ts->clat_prio[ddir] 슬롯 할당/라벨. */
		if (ret)
			goto err;

		free(values[ddir].prios);                                 /* [한국어] 임시 수집 버퍼 즉시 해제. */
		values[ddir].prios = NULL;                                /* [한국어] err 경로 중복 free 방지. */
		values[ddir].nr_prios = 0;                                /* [한국어] 일관성 유지. */
	}

	return 0;                                                         /* [한국어] 모든 ddir 성공. */

err:
	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {                /* [한국어] 모든 ddir 의 임시 자원 일괄 회수. */
		free(parse_res[ddir].entries);                            /* [한국어] 파싱 결과 해제(NULL 도 안전). */
		free(values[ddir].prios);                                 /* [한국어] 수집 버퍼 해제. */
	}
	free_clat_prio_stats(ts);                                         /* [한국어] ts 내 clat_prio 배열 전부 해제. */
	fio_cmdprio_cleanup(cmdprio);                                     /* [한국어] 이미 생성된 bsprio_desc / prios 도 정리. */

	return ret;                                                       /* [한국어] 실패 원인 전파. */
}

/*
 * [한국어]
 * fio_cmdprio_parse_and_gen - 모드 분기 + cmdprio_class 기본값 보정 후
 *                             적절한 generator 호출.
 *
 * @td     : 잡 컨텍스트.
 * @cmdprio: 목적지 테이블(이미 mode 는 결정됨).
 * @return : 0=성공, 1=실패.
 *
 * 정책: cmdprio 의 주 용도가 "고우선 I/O 섞기 실험" 이므로, class 를 명시하지
 * 않았다면 IOPRIO_CLASS_RT 로 자동 설정한다(해당 ddir 에 cmdprio 가 실제로 쓰이지
 * 않더라도 안전하다 — 핫패스가 percentage>0 일 때만 prio 값을 읽기 때문).
 *
 * 실행 컨텍스트: 잡 스레드 초기화 단계.
 */
static int fio_cmdprio_parse_and_gen(struct thread_data *td,
				     struct cmdprio *cmdprio)
{
	struct cmdprio_options *options = cmdprio->options;   /* [한국어] 사용자 옵션 로컬 alias. */
	int i, ret;                                           /* [한국어] i=ddir 인덱스, ret=제너레이터 반환값. */

	/*
	 * If cmdprio_percentage/cmdprio_bssplit is set and cmdprio_class
	 * is not set, default to RT priority class.
	 */
	for (i = 0; i < CMDPRIO_RWDIR_CNT; i++) {             /* [한국어] READ/WRITE 각각에 대해 class 기본값 보정. */
		/*
		 * A cmdprio value is only used when fio_cmdprio_percentage()
		 * returns non-zero, so it is safe to set a class even for a
		 * DDIR that will never use it.
		 */
		if (!options->class[i])                       /* [한국어] class=0 은 "미지정"(IOPRIO_CLASS_NONE) 의미. */
			options->class[i] = IOPRIO_CLASS_RT;  /* [한국어] 기본 Real-Time — blk-mq 스케줄러에서 최우선. */
	}

	switch (cmdprio->mode) {                              /* [한국어] fio_cmdprio_init 에서 결정된 모드로 제너레이터 선택. */
	case CMDPRIO_MODE_BSSPLIT:
		ret = fio_cmdprio_parse_and_gen_bssplit(td, cmdprio);  /* [한국어] 블록크기별 split 테이블 구축. */
		break;
	case CMDPRIO_MODE_PERC:
		ret = fio_cmdprio_gen_perc(td, cmdprio);               /* [한국어] ddir 고정 퍼센티지 테이블 구축. */
		break;
	default:
		assert(0);                                             /* [한국어] NONE 은 fio_cmdprio_init 에서 조기 반환되어 여기로 올 수 없음. */
		return 1;
	}

	return ret;                                           /* [한국어] 제너레이터 결과 그대로 전파. */
}

/*
 * [한국어]
 * fio_cmdprio_cleanup - cmdprio 가 들고 있던 모든 동적 자원을 해제.
 *
 * @cmdprio: 정리 대상(BSSPLIT 모드에서 동적 할당된 bsprios/prios 존재 가능).
 *
 * 정책:
 *   - BSSPLIT 모드에서만 bsprios/prios 가 할당되며, PERC 모드는 perc_entry 가
 *     struct 내부 임베드라 별도 해제가 없다.
 *   - cmdprio->options 는 엔진의 td->eo 일부이므로 여기서 free 하지 않고
 *     포인터만 NULL 로 끊는다. td->eo 는 free_ioengine() 에서 해제된다.
 *
 * 실행 컨텍스트: 잡 스레드 cleanup 단계(또는 init 실패 경로).
 *
 * 호출 체인:
 *   ioengine .cleanup   → [fio_cmdprio_cleanup]
 *   fio_cmdprio_parse_and_gen_bssplit (err 경로) → [fio_cmdprio_cleanup]
 */
void fio_cmdprio_cleanup(struct cmdprio *cmdprio)
{
	enum fio_ddir ddir;    /* [한국어] ddir 순회 변수. */
	int i;                 /* [한국어] bsprios 배열 순회 인덱스. */

	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {                              /* [한국어] READ/WRITE 양방향 자원 회수. */
		for (i = 0; i < cmdprio->bsprio_desc[ddir].nr_bsprios; i++)              /* [한국어] 각 bs 그룹의 prios 배열부터 해제. */
			free(cmdprio->bsprio_desc[ddir].bsprios[i].prios);
		free(cmdprio->bsprio_desc[ddir].bsprios);                                /* [한국어] bs 그룹 배열 자체 해제. */
		cmdprio->bsprio_desc[ddir].bsprios = NULL;                               /* [한국어] dangling 포인터 방지. */
		cmdprio->bsprio_desc[ddir].nr_bsprios = 0;                               /* [한국어] 일관성 유지. */
	}

	/*
	 * options points to a cmdprio_options struct that is part of td->eo.
	 * td->eo itself will be freed by free_ioengine().
	 */
	cmdprio->options = NULL;    /* [한국어] 소유권이 td->eo 에 있으므로 참조만 끊는다. */
}

/*
 * [한국어]
 * fio_cmdprio_init - 엔진이 호출하는 cmdprio 유틸리티 진입점.
 *
 * @td     : 잡 컨텍스트(잡 이름/stat/ioprio 등 접근).
 * @cmdprio: 엔진이 자신의 io_ops_data 에 보유한 cmdprio 인스턴스(이 함수가 채움).
 * @options: 엔진 옵션 구조체에 임베드된 cmdprio_options 의 주소.
 * @return : 0=성공(또는 cmdprio 미사용), 1=에러(잡 중단).
 *
 * 모드 결정 규칙:
 *   - bssplit_str 이 비어있지 않으면 BSSPLIT 모드 후보.
 *   - percentage[] 중 하나라도 0 이 아니면 PERC 모드 후보.
 *   - 둘 다 설정되면 상호배타 → 에러.
 *   - 둘 다 없으면 NONE → 즉시 성공 반환(엔진은 이후 set_ioprio 를 호출하면 안 됨).
 * NONE 이 아니면 fio_cmdprio_parse_and_gen() 에 위임.
 *
 * 실행 컨텍스트: 잡 스레드 ioengine .init 단계.
 *
 * 호출 체인:
 *   libaio_init / fio_ioring_init / fio_sgio_init
 *     → [fio_cmdprio_init] → fio_cmdprio_parse_and_gen → gen_perc / bssplit
 */
int fio_cmdprio_init(struct thread_data *td, struct cmdprio *cmdprio,
		     struct cmdprio_options *options)
{
	struct thread_options *to = &td->o;           /* [한국어] 잡 공통 옵션(에러 로그에 잡 이름 사용). */
	bool has_cmdprio_percentage = false;          /* [한국어] PERC 모드 후보 플래그. */
	bool has_cmdprio_bssplit = false;             /* [한국어] BSSPLIT 모드 후보 플래그. */
	int i;                                        /* [한국어] ddir 인덱스. */

	cmdprio->options = options;                   /* [한국어] 유틸 내부에서 참조할 옵션 포인터 저장(수명: td->eo). */

	if (options->bssplit_str && strlen(options->bssplit_str))  /* [한국어] 문자열 존재 + 비어있지 않음 검사. */
		has_cmdprio_bssplit = true;

	for (i = 0; i < CMDPRIO_RWDIR_CNT; i++) {     /* [한국어] 어느 ddir 라도 percentage>0 이면 PERC 후보. */
		if (options->percentage[i])
			has_cmdprio_percentage = true;
	}

	/*
	 * Check for option conflicts
	 */
	if (has_cmdprio_percentage && has_cmdprio_bssplit) {   /* [한국어] 두 옵션은 의미가 겹쳐 동시 사용 불가. */
		log_err("%s: cmdprio_percentage and cmdprio_bssplit options "
			"are mutually exclusive\n",
			to->name);
		return 1;                                      /* [한국어] 잡 중단 유도. */
	}

	if (has_cmdprio_bssplit)                               /* [한국어] 문자열 기반 상세 스케줄이 우선순위 높은 표현력. */
		cmdprio->mode = CMDPRIO_MODE_BSSPLIT;
	else if (has_cmdprio_percentage)                       /* [한국어] ddir 단위 단순 비율. */
		cmdprio->mode = CMDPRIO_MODE_PERC;
	else
		cmdprio->mode = CMDPRIO_MODE_NONE;             /* [한국어] 옵션 미사용 — 엔진은 set_ioprio 경로를 타면 안 됨. */

	/* Nothing left to do if cmdprio is not used */
	if (cmdprio->mode == CMDPRIO_MODE_NONE)                /* [한국어] 조기 반환 — 테이블/통계 자원 낭비 방지. */
		return 0;

	return fio_cmdprio_parse_and_gen(td, cmdprio);         /* [한국어] 모드별 제너레이터에 위임 — 실질적인 테이블/통계 구축. */
}
