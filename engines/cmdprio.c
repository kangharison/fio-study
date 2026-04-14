/*
 * IO priority handling helper functions common to the libaio and io_uring
 * engines.
 */
/*
 * [한국어 설명]
 * cmdprio.c - libaio/io_uring 엔진 공통 I/O 우선순위 처리 구현
 *
 * === 파일의 역할 ===
 * 이 파일은 libaio와 io_uring 같은 비동기 I/O 엔진이 공통으로 사용하는
 * "커맨드별 우선순위 설정(cmdprio)" 로직을 제공한다. 사용자가 잡 파일/CLI에
 * cmdprio_percentage, cmdprio_bssplit, cmdprio_class, cmdprio_hint 같은 옵션을
 * 주면, 매 I/O마다 확률 기반 또는 블록 크기별 분포에 따라 io_u->ioprio/
 * ioprio_hint 값을 조정해 커널 I/O 스케줄러(blk-mq scheduler)에 전달될
 * 우선순위를 제어한다. 또한 우선순위별 분리된 clat(완료 지연) 통계 슬롯을
 * td->ts.clat_prio에 할당해 통계 수집 경로와 연동한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * libaio.c / io_uring.c 같은 엔진이 init에서 fio_cmdprio_init을 호출해 옵션을
 * 파싱·검증·통계 슬롯을 준비하고, queue 경로에서 io_u마다 fio_cmdprio_set_ioprio을
 * 호출해 ioprio 필드를 변경한 뒤 커널에 I/O를 제출한다. 실행 컨텍스트는
 * 잡 스레드(유저스페이스)이며, 커널 측에서는 ioprio_set 또는 SQE의 ioprio
 * 필드를 통해 block layer 우선순위 큐로 라우팅된다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위: libaio.c, io_uring.c, 기타 비동기 엔진의 init/queue.
 * - 하위: lib/rand.c(rand_between), stat.c(clat_prio 관리), options.c(옵션 파싱).
 * - 공유: thread_data(td->o.ioprio_class, td->ioprio, td->ts.clat_prio[ddir]),
 *   io_u(ioprio, ioprio_hint, clat_prio_index).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_cmdprio_init(): 옵션 해석·모드 결정(PERC/BSSPLIT)·내부 배열 구축·clat 슬롯 할당.
 * - fio_cmdprio_set_ioprio(): 핫 패스. io_u의 ddir/블록 크기에 따라 확률적으로
 *   ioprio 덮어쓰기 및 clat_prio_index 설정.
 * - fio_cmdprio_percentage(): 현재 io_u에 적용할 비율 계산(PERC vs BSSPLIT 분기).
 * - fio_cmdprio_gen_perc / fio_cmdprio_parse_and_gen_bssplit(): 모드별 설정 생성.
 * - fio_cmdprio_cleanup(): prios/bsprio 배열 해제.
 * - struct cmdprio / cmdprio_prio / cmdprio_bsprio_desc: 모드·항목 기술 구조체.
 *
 * === 핵심 동작 흐름 ===
 * 1. fio_cmdprio_init()
 *    - 사용자 옵션 분석 → 모드 결정 (PERC 또는 BSSPLIT)
 *    - cmdprio_class 미설정 시 기본값 RT(Real-Time) 클래스 사용
 *    - 모드에 따라 fio_cmdprio_gen_perc() 또는 fio_cmdprio_parse_and_gen_bssplit() 호출
 *
 * 2. fio_cmdprio_set_ioprio() [핫 패스 - 매 I/O마다 호출]
 *    - fio_cmdprio_percentage()로 현재 io_u에 적용할 우선순위 비율 조회
 *    - rand_between()으로 난수 생성 → 비율 범위 내면 io_u->ioprio 변경
 *    - BSSPLIT 모드에서는 블록 크기에 맞는 항목을 선형 탐색
 *
 * 3. fio_cmdprio_cleanup()
 *    - bsprio 배열과 내부 prios 배열 해제
 *    - options 포인터 NULL 처리 (td->eo는 free_ioengine()에서 해제)
 *
 * === clat_prio 통계 체계 ===
 * 우선순위별로 별도의 완료 지연시간(clat) 통계를 수집하기 위해:
 * - 초기화 시 모든 고유 우선순위 값을 수집 (cmdprio_values)
 * - td->ts.clat_prio[ddir][] 배열에 우선순위별 통계 슬롯 할당
 * - 각 cmdprio_prio에 clat_prio_index를 저장하여 io_u에 전파
 * - 인덱스 0은 항상 기본 우선순위(td->ioprio)에 예약됨
 */

#include "cmdprio.h"

/*
 * [한국어] 파싱 중 사용되는 임시 배열
 * cmdprio_bssplit 옵션 문자열을 파싱한 결과를 보관.
 * 해당 bsprio_desc 생성 완료 후 free됨.
 */
struct cmdprio_parse_result {
	struct split_prio *entries;  /* 파싱된 블록크기/우선순위/비율 항목 배열 */
	int nr_entries;              /* entries 배열의 항목 수 */
};

/*
 * [한국어] 초기화 중 사용되는 임시 배열
 * 모든 고유 우선순위 값을 수집하여 clat_prio_stat 배열을 생성하고
 * 각 cmdprio_prio에 clat_prio_index를 할당하는 데 사용.
 * 초기화 완료 후 free됨.
 */
struct cmdprio_values {
	unsigned int *prios;  /* 고유 우선순위 값 배열 (인덱스 0 = 기본 우선순위) */
	int nr_prios;         /* 현재까지 수집된 고유 우선순위 수 */
};

/*
 * [한국어] 우선순위 값이 이미 수집된 고유 우선순위 배열에 존재하는지 검색
 * 존재하면 해당 인덱스 반환, 없으면 -1 반환.
 * clat_prio 통계 인덱스를 할당하기 위해 사용됨.
 */
static int find_clat_prio_index(unsigned int *all_prios, int nr_prios,
				int32_t prio)
{
	int i;

	for (i = 0; i < nr_prios; i++) {
		if (all_prios[i] == prio)
			return i;
	}

	return -1;
}

/**
 * assign_clat_prio_index - In order to avoid stat.c the need to loop through
 * all possible priorities each time add_clat_sample() / add_lat_sample() is
 * called, save which index to use in each cmdprio_prio. This will later be
 * propagated to the io_u, if the specific io_u was determined to use a cmdprio
 * priority value.
 *
 * [한국어] 우선순위별 clat 통계 인덱스를 할당
 * 새로운 우선순위 값이면 values 배열에 추가하고 새 인덱스 부여.
 * 이미 존재하면 기존 인덱스를 재사용하여 동일 우선순위는 같은 통계 슬롯을 공유.
 * 이 인덱스는 나중에 io_u->clat_prio_index로 전파되어
 * stat.c에서 add_clat_sample() 시 빠르게 올바른 통계 슬롯을 찾을 수 있게 함.
 */
static void assign_clat_prio_index(struct cmdprio_prio *prio,
				   struct cmdprio_values *values)
{
	int clat_prio_index = find_clat_prio_index(values->prios,
						   values->nr_prios,
						   prio->prio);
	if (clat_prio_index == -1) {
		clat_prio_index = values->nr_prios;
		values->prios[clat_prio_index] = prio->prio;
		values->nr_prios++;
	}
	prio->clat_prio_index = clat_prio_index;
}

/**
 * init_cmdprio_values - Allocate a temporary array that can hold all unique
 * priorities (per ddir), so that we can assign_clat_prio_index() for each
 * cmdprio_prio during setup. This temporary array is freed after setup.
 *
 * [한국어] 고유 우선순위 수집용 임시 배열 초기화
 * max_unique_prios + 1 크기로 할당 (+1은 기본 우선순위 td->ioprio용).
 * 인덱스 0에는 항상 기본 우선순위(ts->ioprio)를 저장함.
 */
static int init_cmdprio_values(struct cmdprio_values *values,
			       int max_unique_prios, struct thread_stat *ts)
{
	values->prios = calloc(max_unique_prios + 1,
			       sizeof(*values->prios));
	if (!values->prios)
		return 1;

	/* td->ioprio/ts->ioprio is always stored at index 0. */
	values->prios[0] = ts->ioprio;
	values->nr_prios++;

	return 0;
}

/**
 * init_ts_clat_prio - Allocates and fills a clat_prio_stat array which holds
 * all unique priorities (per ddir).
 *
 * [한국어] 우선순위별 clat 통계 배열을 thread_stat에 할당
 * values에 수집된 고유 우선순위 수만큼 clat_prio_stat을 할당하고
 * 각 슬롯에 해당 ioprio 값을 기록. 이후 stat.c에서 우선순위별 지연시간 통계 수집에 사용됨.
 */
static int init_ts_clat_prio(struct thread_stat *ts, enum fio_ddir ddir,
			     struct cmdprio_values *values)
{
	int i;

	if (alloc_clat_prio_stat_ddir(ts, ddir, values->nr_prios))
		return 1;

	for (i = 0; i < values->nr_prios; i++)
		ts->clat_prio[ddir][i].ioprio = values->prios[i];

	return 0;
}

/*
 * [한국어] 동일 블록 크기를 가진 파싱 항목들(start~end)을 하나의 cmdprio_bsprio에 채움
 * entries[i].prio가 -1이면 implicit_cmdprio(사용자가 cmdprio_class/cmdprio로 지정한 값)를 사용.
 * 각 항목에 clat 통계 인덱스를 할당하고, 총 비율이 100%를 초과하면 에러.
 */
static int fio_cmdprio_fill_bsprio(struct cmdprio_bsprio *bsprio,
				   struct split_prio *entries,
				   struct cmdprio_values *values,
				   int implicit_cmdprio, int start, int end)
{
	struct cmdprio_prio *prio;
	int i = end - start + 1;

	bsprio->prios = calloc(i, sizeof(*bsprio->prios));
	if (!bsprio->prios)
		return 1;

	bsprio->bs = entries[start].bs;
	bsprio->nr_prios = 0;
	for (i = start; i <= end; i++) {
		prio = &bsprio->prios[bsprio->nr_prios];
		prio->perc = entries[i].perc;
		if (entries[i].prio == -1)
			prio->prio = implicit_cmdprio;
		else
			prio->prio = entries[i].prio;
		assign_clat_prio_index(prio, values);
		bsprio->tot_perc += entries[i].perc;
		if (bsprio->tot_perc > 100) {
			log_err("fio: cmdprio_bssplit total percentage "
				"for bs: %"PRIu64" exceeds 100\n",
				bsprio->bs);
			free(bsprio->prios);
			return 1;
		}
		bsprio->nr_prios++;
	}

	return 0;
}

/*
 * [한국어] 파싱된 결과에서 bsprio_desc 구조체를 생성
 * 파싱 결과는 블록 크기 순으로 정렬되어 있으므로,
 * 동일 블록 크기의 연속 항목들을 하나의 cmdprio_bsprio로 묶음.
 * 이렇게 하면 핫 패스에서 블록 크기로 빠르게 검색 가능.
 */
static int
fio_cmdprio_generate_bsprio_desc(struct cmdprio_bsprio_desc *bsprio_desc,
				 struct cmdprio_parse_result *parse_res,
				 struct cmdprio_values *values,
				 int implicit_cmdprio)
{
	struct split_prio *entries = parse_res->entries;
	int nr_entries = parse_res->nr_entries;
	struct cmdprio_bsprio *bsprio;
	int i, start, count = 0;

	/*
	 * The parsed result is sorted by blocksize, so count only the number
	 * of different blocksizes, to know how many cmdprio_bsprio we need.
	 */
	for (i = 0; i < nr_entries; i++) {
		while (i + 1 < nr_entries && entries[i].bs == entries[i + 1].bs)
			i++;
		count++;
	}

	/*
	 * This allocation is not freed on error. Instead, the calling function
	 * is responsible for calling fio_cmdprio_cleanup() on error.
	 */
	bsprio_desc->bsprios = calloc(count, sizeof(*bsprio_desc->bsprios));
	if (!bsprio_desc->bsprios)
		return 1;

	start = 0;
	bsprio_desc->nr_bsprios = 0;
	for (i = 0; i < nr_entries; i++) {
		while (i + 1 < nr_entries && entries[i].bs == entries[i + 1].bs)
			i++;
		bsprio = &bsprio_desc->bsprios[bsprio_desc->nr_bsprios];
		/*
		 * All parsed entries with the same blocksize get saved in the
		 * same cmdprio_bsprio, to expedite the search in the hot path.
		 */
		if (fio_cmdprio_fill_bsprio(bsprio, entries, values,
					    implicit_cmdprio, start, i))
			return 1;

		start = i + 1;
		bsprio_desc->nr_bsprios++;
	}

	return 0;
}

/*
 * [한국어] str_split_parse()의 콜백 함수
 * cmdprio_bssplit 문자열을 ddir별로 파싱하여 parse_res에 저장.
 * DDIR_TRIM은 cmdprio를 지원하지 않으므로 무시.
 */
static int fio_cmdprio_bssplit_ddir(struct thread_options *to, void *cb_arg,
				    enum fio_ddir ddir, char *str, bool data)
{
	struct cmdprio_parse_result *parse_res_arr = cb_arg;
	struct cmdprio_parse_result *parse_res = &parse_res_arr[ddir];

	if (ddir == DDIR_TRIM)
		return 0;

	if (split_parse_prio_ddir(to, &parse_res->entries,
				  &parse_res->nr_entries, str))
		return 1;

	return 0;
}

/*
 * [한국어] cmdprio_bssplit 옵션 문자열을 파싱
 * 입력 문자열을 복제하고 양끝 공백을 제거한 후 str_split_parse()로 파싱.
 * 결과는 parse_res 배열(ddir별)에 저장됨.
 */
static int fio_cmdprio_bssplit_parse(struct thread_data *td, const char *input,
				     struct cmdprio_parse_result *parse_res)
{
	char *str, *p;
	int ret = 0;

	p = str = strdup(input);

	strip_blank_front(&str);
	strip_blank_end(str);

	ret = str_split_parse(td, str, fio_cmdprio_bssplit_ddir, parse_res,
			      false);

	free(p);
	return ret;
}

/**
 * fio_cmdprio_percentage - Returns the percentage of I/Os that should
 * use a cmdprio priority value (rather than the default context priority).
 *
 * For CMDPRIO_MODE_BSSPLIT, if the percentage is non-zero, we will also
 * return the matching bsprio, to avoid the same linear search elsewhere.
 * For CMDPRIO_MODE_PERC, we will never return a bsprio.
 *
 * [한국어] 현재 io_u에 적용할 우선순위 변경 비율을 반환 [핫 패스]
 * - PERC 모드: ddir에 해당하는 고정 비율 반환, bsprio는 NULL
 * - BSSPLIT 모드: io_u->buflen과 일치하는 블록 크기를 선형 탐색하여
 *   해당 항목의 총 비율과 bsprio 포인터를 반환
 * - 일치하는 블록 크기가 없으면 0 반환 (우선순위 변경 안 함)
 */
static int fio_cmdprio_percentage(struct cmdprio *cmdprio, struct io_u *io_u,
				  struct cmdprio_bsprio **bsprio)
{
	struct cmdprio_bsprio *bsprio_entry;
	enum fio_ddir ddir = io_u->ddir;
	int i;

	switch (cmdprio->mode) {
	case CMDPRIO_MODE_PERC:
		*bsprio = NULL;
		return cmdprio->perc_entry[ddir].perc;
	case CMDPRIO_MODE_BSSPLIT:
		for (i = 0; i < cmdprio->bsprio_desc[ddir].nr_bsprios; i++) {
			bsprio_entry = &cmdprio->bsprio_desc[ddir].bsprios[i];
			if (bsprio_entry->bs == io_u->buflen) {
				*bsprio = bsprio_entry;
				return bsprio_entry->tot_perc;
			}
		}
		break;
	default:
		/*
		 * An I/O engine should never call this function if cmdprio
		 * is not is use.
		 */
		assert(0);
	}

	/*
	 * This is totally fine, the given blocksize simply does not
	 * have any (non-zero) cmdprio_bssplit entries defined.
	 */
	*bsprio = NULL;
	return 0;
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
 * [한국어] I/O 우선순위 확률적 설정 [핫 패스 - 매 I/O마다 호출]
 *
 * 동작 과정:
 * 1. fio_cmdprio_percentage()로 현재 io_u에 적용할 우선순위 비율(p) 조회
 * 2. p가 0이면 즉시 false 반환 (우선순위 변경 불필요)
 * 3. 0~99 범위의 난수 생성
 * 4. 난수 >= p이면 false 반환 (이번 I/O는 기본 우선순위 유지)
 * 5. 난수 < p이면 모드에 따라 io_u->ioprio와 io_u->clat_prio_index 설정
 *    - PERC 모드: 해당 ddir의 perc_entry에서 직접 가져옴
 *    - BSSPLIT 모드: bsprio 내 prios를 누적 비율로 순회하여 선택
 */
bool fio_cmdprio_set_ioprio(struct thread_data *td, struct cmdprio *cmdprio,
			    struct io_u *io_u)
{
	struct cmdprio_bsprio *bsprio;
	unsigned int p, rand;
	uint32_t perc = 0;
	int i;

	p = fio_cmdprio_percentage(cmdprio, io_u, &bsprio);
	if (!p)
		return false;

	rand = rand_between(&td->prio_state, 0, 99);
	if (rand >= p)
		return false;

	switch (cmdprio->mode) {
	case CMDPRIO_MODE_PERC:
		io_u->ioprio = cmdprio->perc_entry[io_u->ddir].prio;
		io_u->clat_prio_index =
			cmdprio->perc_entry[io_u->ddir].clat_prio_index;
		return true;
	case CMDPRIO_MODE_BSSPLIT:
		assert(bsprio);
		for (i = 0; i < bsprio->nr_prios; i++) {
			struct cmdprio_prio *prio = &bsprio->prios[i];

			perc += prio->perc;
			if (rand < perc) {
				io_u->ioprio = prio->prio;
				io_u->clat_prio_index = prio->clat_prio_index;
				return true;
			}
		}
		break;
	default:
		assert(0);
	}

	/* When rand < p (total perc), we should always find a cmdprio_prio. */
	assert(0);
	return false;
}

/*
 * [한국어] PERC 모드 초기화
 * 각 ddir(read/write)에 대해:
 * - 비율이 0이거나 해당 ddir이 비활성이면 스킵
 * - cmdprio_class/cmdprio/cmdprio_hint로 ioprio 값 조합
 * - clat 통계 인덱스 할당
 * - thread_stat에 clat_prio 배열 생성
 */
static int fio_cmdprio_gen_perc(struct thread_data *td, struct cmdprio *cmdprio)
{
	struct cmdprio_options *options = cmdprio->options;
	struct cmdprio_prio *prio;
	struct cmdprio_values values[CMDPRIO_RWDIR_CNT] = {};
	struct thread_stat *ts = &td->ts;
	enum fio_ddir ddir;
	int ret;

	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {
		/*
		 * Do not allocate a clat_prio array nor set the cmdprio struct
		 * if zero percent of the I/Os (for the ddir) should use a
		 * cmdprio priority value, or when the ddir is not enabled.
		 */
		if (!options->percentage[ddir] ||
		    (ddir == DDIR_READ && !td_read(td)) ||
		    (ddir == DDIR_WRITE && !td_write(td)))
			continue;

		ret = init_cmdprio_values(&values[ddir], 1, ts);
		if (ret)
			goto err;

		prio = &cmdprio->perc_entry[ddir];
		prio->perc = options->percentage[ddir];
		prio->prio = ioprio_value(options->class[ddir],
					  options->level[ddir],
					  options->hint[ddir]);
		assign_clat_prio_index(prio, &values[ddir]);

		ret = init_ts_clat_prio(ts, ddir, &values[ddir]);
		if (ret)
			goto err;

		free(values[ddir].prios);
		values[ddir].prios = NULL;
		values[ddir].nr_prios = 0;
	}

	return 0;

err:
	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++)
		free(values[ddir].prios);
	free_clat_prio_stats(ts);

	return ret;
}

/*
 * [한국어] BSSPLIT 모드 파싱 및 초기화
 * 1. cmdprio_bssplit 문자열을 파싱하여 parse_res에 저장
 * 2. 각 ddir에 대해:
 *    - 파싱 항목이 없거나 해당 ddir이 비활성이면 스킵
 *    - 고유 우선순위 수집용 임시 배열 초기화
 *    - implicit_cmdprio 계산 (prio=-1인 항목에 사용할 기본 우선순위)
 *    - bsprio_desc 생성 (블록 크기별 우선순위 구조체)
 *    - clat_prio 통계 배열 생성
 * 에러 시 모든 할당된 자원을 정리
 */
static int fio_cmdprio_parse_and_gen_bssplit(struct thread_data *td,
					     struct cmdprio *cmdprio)
{
	struct cmdprio_options *options = cmdprio->options;
	struct cmdprio_parse_result parse_res[CMDPRIO_RWDIR_CNT] = {};
	struct cmdprio_values values[CMDPRIO_RWDIR_CNT] = {};
	struct thread_stat *ts = &td->ts;
	int ret, implicit_cmdprio;
	enum fio_ddir ddir;

	ret = fio_cmdprio_bssplit_parse(td, options->bssplit_str,
					&parse_res[0]);
	if (ret)
		goto err;

	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {
		/*
		 * Do not allocate a clat_prio array nor set the cmdprio structs
		 * if there are no non-zero entries (for the ddir), or when the
		 * ddir is not enabled.
		 */
		if (!parse_res[ddir].nr_entries ||
		    (ddir == DDIR_READ && !td_read(td)) ||
		    (ddir == DDIR_WRITE && !td_write(td))) {
			free(parse_res[ddir].entries);
			parse_res[ddir].entries = NULL;
			parse_res[ddir].nr_entries = 0;
			continue;
		}

		ret = init_cmdprio_values(&values[ddir],
					  parse_res[ddir].nr_entries, ts);
		if (ret)
			goto err;

		implicit_cmdprio = ioprio_value(options->class[ddir],
						options->level[ddir],
						options->hint[ddir]);

		ret = fio_cmdprio_generate_bsprio_desc(&cmdprio->bsprio_desc[ddir],
						       &parse_res[ddir],
						       &values[ddir],
						       implicit_cmdprio);
		if (ret)
			goto err;

		free(parse_res[ddir].entries);
		parse_res[ddir].entries = NULL;
		parse_res[ddir].nr_entries = 0;

		ret = init_ts_clat_prio(ts, ddir, &values[ddir]);
		if (ret)
			goto err;

		free(values[ddir].prios);
		values[ddir].prios = NULL;
		values[ddir].nr_prios = 0;
	}

	return 0;

err:
	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {
		free(parse_res[ddir].entries);
		free(values[ddir].prios);
	}
	free_clat_prio_stats(ts);
	fio_cmdprio_cleanup(cmdprio);

	return ret;
}

/*
 * [한국어] 모드에 따라 적절한 초기화 함수를 호출
 * cmdprio_class가 설정되지 않은 경우, 기본값으로 RT(Real-Time) 클래스를 사용.
 * 이는 cmdprio의 주 목적이 "높은 우선순위 I/O 시뮬레이션"이기 때문.
 */
static int fio_cmdprio_parse_and_gen(struct thread_data *td,
				     struct cmdprio *cmdprio)
{
	struct cmdprio_options *options = cmdprio->options;
	int i, ret;

	/*
	 * If cmdprio_percentage/cmdprio_bssplit is set and cmdprio_class
	 * is not set, default to RT priority class.
	 */
	for (i = 0; i < CMDPRIO_RWDIR_CNT; i++) {
		/*
		 * A cmdprio value is only used when fio_cmdprio_percentage()
		 * returns non-zero, so it is safe to set a class even for a
		 * DDIR that will never use it.
		 */
		if (!options->class[i])
			options->class[i] = IOPRIO_CLASS_RT;
	}

	switch (cmdprio->mode) {
	case CMDPRIO_MODE_BSSPLIT:
		ret = fio_cmdprio_parse_and_gen_bssplit(td, cmdprio);
		break;
	case CMDPRIO_MODE_PERC:
		ret = fio_cmdprio_gen_perc(td, cmdprio);
		break;
	default:
		assert(0);
		return 1;
	}

	return ret;
}

/*
 * [한국어] cmdprio 자원 해제
 * BSSPLIT 모드에서 할당된 bsprios 배열과 각 bsprio의 prios 배열을 해제.
 * options 포인터는 NULL로 설정만 하고 free하지 않음 (td->eo의 일부이므로).
 */
void fio_cmdprio_cleanup(struct cmdprio *cmdprio)
{
	enum fio_ddir ddir;
	int i;

	for (ddir = 0; ddir < CMDPRIO_RWDIR_CNT; ddir++) {
		for (i = 0; i < cmdprio->bsprio_desc[ddir].nr_bsprios; i++)
			free(cmdprio->bsprio_desc[ddir].bsprios[i].prios);
		free(cmdprio->bsprio_desc[ddir].bsprios);
		cmdprio->bsprio_desc[ddir].bsprios = NULL;
		cmdprio->bsprio_desc[ddir].nr_bsprios = 0;
	}

	/*
	 * options points to a cmdprio_options struct that is part of td->eo.
	 * td->eo itself will be freed by free_ioengine().
	 */
	cmdprio->options = NULL;
}

/*
 * [한국어] cmdprio 초기화 진입점
 * 1. bssplit_str이 있으면 BSSPLIT 모드
 * 2. percentage가 있으면 PERC 모드
 * 3. 둘 다 있으면 에러 (상호 배타적)
 * 4. 둘 다 없으면 NONE 모드 → 즉시 반환
 * 모드가 결정되면 fio_cmdprio_parse_and_gen()으로 실제 초기화 수행.
 */
int fio_cmdprio_init(struct thread_data *td, struct cmdprio *cmdprio,
		     struct cmdprio_options *options)
{
	struct thread_options *to = &td->o;
	bool has_cmdprio_percentage = false;
	bool has_cmdprio_bssplit = false;
	int i;

	cmdprio->options = options;

	if (options->bssplit_str && strlen(options->bssplit_str))
		has_cmdprio_bssplit = true;

	for (i = 0; i < CMDPRIO_RWDIR_CNT; i++) {
		if (options->percentage[i])
			has_cmdprio_percentage = true;
	}

	/*
	 * Check for option conflicts
	 */
	if (has_cmdprio_percentage && has_cmdprio_bssplit) {
		log_err("%s: cmdprio_percentage and cmdprio_bssplit options "
			"are mutually exclusive\n",
			to->name);
		return 1;
	}

	if (has_cmdprio_bssplit)
		cmdprio->mode = CMDPRIO_MODE_BSSPLIT;
	else if (has_cmdprio_percentage)
		cmdprio->mode = CMDPRIO_MODE_PERC;
	else
		cmdprio->mode = CMDPRIO_MODE_NONE;

	/* Nothing left to do if cmdprio is not used */
	if (cmdprio->mode == CMDPRIO_MODE_NONE)
		return 0;

	return fio_cmdprio_parse_and_gen(td, cmdprio);
}
