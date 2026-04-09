/* [한국어] io_u.c - fio의 I/O 유닛(io_u) 관리 핵심 파일
 *
 * 이 파일은 fio 벤치마크 도구에서 I/O 요청의 전체 생명주기를 관리한다.
 * io_u는 하나의 I/O 작업을 나타내는 구조체로, 다음 과정을 거친다:
 *
 *   1. 할당(Allocation): get_io_u() / __get_io_u()에서 프리리스트로부터 io_u를 꺼냄
 *   2. 설정(Setup): fill_io_u()에서 오프셋, 버퍼 길이, 데이터 방향 결정
 *   3. 제출(Submit): io_u_queued()에서 제출 레이턴시 기록
 *   4. 완료(Completion): io_u_sync_complete() / io_u_queued_complete()에서 완료 처리
 *   5. 반환(Return): put_io_u()에서 프리리스트로 io_u를 되돌림
 *
 * 주요 개념:
 *   - offset 결정: 순차(sequential) 또는 랜덤(random) 방식으로 I/O 위치 결정
 *   - buflen 결정: min_bs ~ max_bs 범위 내에서 블록 크기 결정
 *   - ddir 결정: read/write/trim/sync 등 데이터 방향 결정
 *   - random map: 랜덤 I/O 시 이미 접근한 블록을 추적하여 전체 범위 커버
 */
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "fio.h"
#include "verify.h"
#include "trim.h"
#include "lib/rand.h"
#include "lib/axmap.h"
#include "err.h"
#include "lib/pow2.h"
#include "minmax.h"
#include "zbd.h"
#include "sprandom.h"

/* [한국어] I/O 완료 데이터 구조체
 * I/O 완료 시 결과 정보를 저장하는 구조체.
 * nr: 완료할 I/O 개수 (입력값)
 * error: 발생한 에러 코드 (출력값)
 * bytes_done: 각 방향(read/write/trim)별 완료된 바이트 수 (출력값)
 * time: 완료 시각 (출력값)
 */
struct io_completion_data {
	int nr;				/* input */
	/* [한국어] 완료할 I/O 요청 수 (입력) */

	int error;			/* output */
	/* [한국어] 에러 발생 시 에러 코드 (출력) */
	uint64_t bytes_done[DDIR_RWDIR_CNT];	/* output */
	/* [한국어] 각 방향(read/write/trim)별 완료된 바이트 수 (출력) */
	struct timespec time;		/* output */
	/* [한국어] I/O 완료 시각 (출력) */
};

/*
 * The ->io_axmap contains a map of blocks we have or have not done io
 * to yet. Used to make sure we cover the entire range in a fair fashion.
 */
/* [한국어] 랜덤 맵에서 특정 블록이 아직 I/O되지 않았는지(비어있는지) 확인하는 함수.
 *
 * @f: 파일 구조체 포인터
 * @block: 확인할 블록 번호
 * @return: 해당 블록이 비어있으면 true, 이미 I/O했으면 false
 *
 * io_axmap은 비트맵 기반의 자료구조로, 랜덤 I/O 시 전체 파일 범위를
 * 균등하게 커버하기 위해 이미 접근한 블록을 추적한다.
 */
static bool random_map_free(struct fio_file *f, const uint64_t block)
{
	return !axmap_isset(f->io_axmap, block);
}

/*
 * Mark a given offset as used in the map.
 */
/* [한국어] 주어진 오프셋을 랜덤 맵에서 사용됨으로 표시하는 함수.
 *
 * @td: 스레드 데이터 (작업 설정 정보 포함)
 * @io_u: I/O 유닛 구조체
 * @offset: 표시할 오프셋 (바이트 단위)
 * @buflen: I/O 버퍼 길이
 * @return: 실제로 표시된 바이트 수 (맵에서 이미 표시된 블록이 있으면 줄어들 수 있음)
 *
 * 랜덤 I/O에서 같은 위치를 중복 접근하지 않도록 맵에 기록한다.
 * min_bs 단위로 블록 번호를 계산하고, axmap_set_nr()로 해당 블록들을 표시한다.
 */
static uint64_t mark_random_map(struct thread_data *td, struct io_u *io_u,
				uint64_t offset, uint64_t buflen)
{
	/* [한국어] 현재 방향(ddir)에 대한 최소 블록 크기 */
	unsigned long long min_bs = td->o.min_bs[io_u->ddir];
	struct fio_file *f = io_u->file;
	unsigned long long nr_blocks;
	uint64_t block;

	/* [한국어] 파일 오프셋을 기준으로 블록 번호 계산 */
	block = (offset - f->file_offset) / (uint64_t) min_bs;
	/* [한국어] 이 I/O가 차지하는 블록 수 계산 (올림) */
	nr_blocks = (buflen + min_bs - 1) / min_bs;
	assert(nr_blocks > 0);

	/* [한국어] BUSY_OK 플래그가 없으면 맵에 블록을 표시 */
	if (!(io_u->flags & IO_U_F_BUSY_OK)) {
		nr_blocks = axmap_set_nr(f->io_axmap, block, nr_blocks);
		assert(nr_blocks > 0);
	}

	/* [한국어] 실제 표시된 블록 수에 맞춰 buflen 조정 */
	if ((nr_blocks * min_bs) < buflen)
		buflen = nr_blocks * min_bs;

	return buflen;
}

/* [한국어] 특정 방향(ddir)에 대해 파일의 마지막 블록 번호를 계산하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향 (READ/WRITE/TRIM)
 * @return: 마지막 블록 번호 (ba 단위), 블록이 없으면 0
 *
 * 파일의 io_size, real_file_size, zone_range 등을 고려하여
 * 접근 가능한 최대 블록 수를 계산한다. ba(block alignment)로 나눠 블록 수를 구한다.
 */
static uint64_t last_block(struct thread_data *td, struct fio_file *f,
			   enum fio_ddir ddir)
{
	uint64_t max_blocks;
	uint64_t max_size;

	/* [한국어] ddir이 유효한 read/write 방향인지 검증 */
	assert(ddir_rw(ddir));

	/*
	 * Hmm, should we make sure that ->io_size <= ->real_file_size?
	 * -> not for now since there is code assuming it could go either.
	 */
	/* [한국어] 파일의 I/O 크기를 가져오되, 실제 파일 크기를 초과하지 않도록 제한 */
	max_size = f->io_size;
	if (max_size > f->real_file_size)
		max_size = f->real_file_size;

	/* [한국어] 스트라이드 존 모드에서는 zone_range로 제한 */
	if (td->o.zone_mode == ZONE_MODE_STRIDED && td->o.zone_range)
		max_size = td->o.zone_range;

	/* [한국어] min_bs가 ba보다 크면, 마지막 블록이 파일 끝을 넘지 않도록 보정 */
	if (td->o.min_bs[ddir] > td->o.ba[ddir])
		max_size -= td->o.min_bs[ddir] - td->o.ba[ddir];

	/* [한국어] ba(블록 정렬 단위)로 나눠서 최대 블록 수 계산 */
	max_blocks = max_size / (uint64_t) td->o.ba[ddir];
	if (!max_blocks)
		return 0;

	return max_blocks;
}


/* [한국어] SP Random(특수 랜덤) 방식으로 다음 랜덤 오프셋을 가져오는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향 (WRITE만 지원)
 * @b: 결과 블록 번호를 저장할 포인터
 * @lastb: 마지막 블록 번호 (사용되지 않음)
 * @return: 0=성공, 1=모든 주소를 한 번씩 다 기록함(완료)
 *
 * SP RANDOM은 모든 주소를 정확히 한 번씩 기록하는 특수 랜덤 패턴이다.
 */
static int __get_next_rand_offset_sprandom(struct thread_data *td, struct fio_file *f,
					   enum fio_ddir ddir, uint64_t *b,
					   uint64_t lastb)
{
	/* [한국어] SP Random은 쓰기(WRITE) 전용 */
	assert(ddir == DDIR_WRITE);

	/* SP RANDOM writes all addresses once */
	/* [한국어] sprandom_get_next_offset으로 다음 오프셋 획득, 실패하면 작업 완료 */
	if (sprandom_get_next_offset(f->spr_info, f, b)) {
		dprint(FD_SPRANDOM, "sprandom is done\n");
		td->done = 1;
		return 1;
	}
	return 0;
}

/* [한국어] 기본 랜덤 오프셋 생성 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @lastb: 마지막 블록 번호 (범위 상한)
 * @return: 0=성공, 1=빈 블록 없음(실패)
 *
 * 난수 생성기(Tausworthe 또는 LFSR)를 사용하여 랜덤 블록 번호를 생성한다.
 * random map이 활성화된 경우 이미 사용된 블록을 건너뛰고 빈 블록을 찾는다.
 */
static int __get_next_rand_offset(struct thread_data *td, struct fio_file *f,
				  enum fio_ddir ddir, uint64_t *b,
				  uint64_t lastb)
{
	uint64_t r;

	/* [한국어] Tausworthe 계열 난수 생성기 사용 시 */
	if (td->o.random_generator == FIO_RAND_GEN_TAUSWORTHE ||
	    td->o.random_generator == FIO_RAND_GEN_TAUSWORTHE64) {

		/* [한국어] 난수 생성 */
		r = __rand(&td->offset_state);

		dprint(FD_RANDOM, "off rand %llu\n", (unsigned long long) r);

		/* [한국어] 난수를 0~lastb 범위로 매핑하여 블록 번호 결정 */
		*b = lastb * (r / (rand_max(&td->offset_state) + 1.0));
	} else {
		/* [한국어] LFSR(선형 피드백 시프트 레지스터) 방식 사용 */
		uint64_t off = 0;

		assert(fio_file_lfsr(f));

		/* [한국어] LFSR에서 다음 값 획득, 실패 시 반환 */
		if (lfsr_next(&f->lfsr, &off))
			return 1;

		*b = off;
	}

	/*
	 * if we are not maintaining a random map, we are done.
	 */
	/* [한국어] 랜덤 맵을 유지하지 않으면 바로 반환 */
	if (!file_randommap(td, f))
		goto ret;

	/*
	 * calculate map offset and check if it's free
	 */
	/* [한국어] 맵에서 해당 블록이 비어있는지 확인 */
	if (random_map_free(f, *b))
		goto ret;

	dprint(FD_RANDOM, "get_next_rand_offset: offset %llu busy\n",
						(unsigned long long) *b);

	/* [한국어] 해당 블록이 사용 중이면 다음 빈 블록을 탐색 */
	*b = axmap_next_free(f->io_axmap, *b);
	/* [한국어] 빈 블록이 없으면 실패 반환 */
	if (*b == (uint64_t) -1ULL)
		return 1;
ret:
	return 0;
}

/* [한국어] Zipf 분포를 사용하여 다음 랜덤 오프셋을 생성하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 항상 0 (성공)
 *
 * Zipf 분포는 소수의 핫 블록에 접근이 집중되는 패턴을 시뮬레이션한다.
 */
static int __get_next_rand_offset_zipf(struct thread_data *td,
				       struct fio_file *f, enum fio_ddir ddir,
				       uint64_t *b)
{
	*b = zipf_next(&f->zipf);
	return 0;
}

/* [한국어] Pareto 분포를 사용하여 다음 랜덤 오프셋을 생성하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 항상 0 (성공)
 *
 * Pareto 분포(80/20 법칙)를 따르는 접근 패턴을 생성한다.
 */
static int __get_next_rand_offset_pareto(struct thread_data *td,
					 struct fio_file *f, enum fio_ddir ddir,
					 uint64_t *b)
{
	*b = pareto_next(&f->zipf);
	return 0;
}

/* [한국어] 가우시안(정규) 분포를 사용하여 다음 랜덤 오프셋을 생성하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 항상 0 (성공)
 *
 * 가우시안 분포는 파일 중앙 부근에 접근이 집중되는 패턴을 생성한다.
 */
static int __get_next_rand_offset_gauss(struct thread_data *td,
					struct fio_file *f, enum fio_ddir ddir,
					uint64_t *b)
{
	*b = gauss_next(&f->gauss);
	return 0;
}

/* [한국어] 절대 크기 기반 존 분할(zoned_abs) 랜덤 오프셋 생성 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 0=성공, 1=실패
 *
 * 사용자가 정의한 존(zone) 영역을 절대 크기(바이트)로 지정하고,
 * 각 존에 확률적으로 접근 빈도를 할당한다.
 * 예: zone1=0-1GB(70%), zone2=1GB-2GB(30%)
 */
static int __get_next_rand_offset_zoned_abs(struct thread_data *td,
					    struct fio_file *f,
					    enum fio_ddir ddir, uint64_t *b)
{
	struct zone_split_index *zsi;
	uint64_t lastb, send, stotal;
	unsigned int v;

	/* [한국어] 파일의 마지막 블록 번호 계산 */
	lastb = last_block(td, f, ddir);
	if (!lastb)
		return 1;

	/* [한국어] 존 분할이 설정되지 않았으면 기본 랜덤 오프셋 사용 */
	if (!td->o.zone_split_nr[ddir]) {
bail:
		return __get_next_rand_offset(td, f, ddir, b, lastb);
	}

	/*
	 * Generate a value, v, between 1 and 100, both inclusive
	 */
	/* [한국어] 1~100 사이의 난수를 생성하여 어느 존에 해당하는지 결정 */
	v = rand_between(&td->zone_state, 1, 100);

	/*
	 * Find our generated table. 'send' is the end block of this zone,
	 * 'stotal' is our start offset.
	 */
	/* [한국어] 생성된 난수 v에 해당하는 존 인덱스에서 시작/끝 블록 가져오기 */
	zsi = &td->zone_state_index[ddir][v - 1];
	/* [한국어] 이전 존까지의 누적 크기 (시작 오프셋) */
	stotal = zsi->size_prev / td->o.ba[ddir];
	/* [한국어] 현재 존의 끝 위치 */
	send = zsi->size / td->o.ba[ddir];

	/*
	 * Should never happen
	 */
	/* [한국어] 버그 체크: send가 유효하지 않은 경우 */
	if (send == -1U) {
		if (!fio_did_warn(FIO_WARN_ZONED_BUG))
			log_err("fio: bug in zoned generation\n");
		goto bail;
	} else if (send > lastb) {
		/*
		 * This happens if the user specifies ranges that exceed
		 * the file/device size. We can't handle that gracefully,
		 * so error and exit.
		 */
		/* [한국어] 존 범위가 파일 크기를 초과하면 에러 */
		log_err("fio: zoned_abs sizes exceed file size\n");
		return 1;
	}

	/*
	 * Generate index from 0..send-stotal
	 */
	/* [한국어] 선택된 존 내에서 랜덤 오프셋 생성 */
	if (__get_next_rand_offset(td, f, ddir, b, send - stotal) == 1)
		return 1;

	/* [한국어] 존의 시작 오프셋을 더해 최종 블록 번호 결정 */
	*b += stotal;
	return 0;
}

/* [한국어] 비율(퍼센트) 기반 존 분할(zoned) 랜덤 오프셋 생성 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 0=성공, 1=실패
 *
 * 존 영역을 파일 크기의 퍼센트로 지정한다.
 * 예: 파일의 0-50%(70% 확률), 50-100%(30% 확률)
 */
static int __get_next_rand_offset_zoned(struct thread_data *td,
					struct fio_file *f, enum fio_ddir ddir,
					uint64_t *b)
{
	unsigned int v, send, stotal;
	uint64_t offset, lastb;
	struct zone_split_index *zsi;

	/* [한국어] 마지막 블록 번호 계산 */
	lastb = last_block(td, f, ddir);
	if (!lastb)
		return 1;

	/* [한국어] 존 분할 미설정 시 기본 랜덤 방식 사용 */
	if (!td->o.zone_split_nr[ddir]) {
bail:
		return __get_next_rand_offset(td, f, ddir, b, lastb);
	}

	/*
	 * Generate a value, v, between 1 and 100, both inclusive
	 */
	/* [한국어] 1~100 사이 난수 생성 */
	v = rand_between(&td->zone_state, 1, 100);

	/* [한국어] 난수 v에 해당하는 존의 퍼센트 범위 가져오기 */
	zsi = &td->zone_state_index[ddir][v - 1];
	/* [한국어] 이전 존까지의 누적 퍼센트 (시작 퍼센트) */
	stotal = zsi->size_perc_prev;
	/* [한국어] 현재 존의 끝 퍼센트 */
	send = zsi->size_perc;

	/*
	 * Should never happen
	 */
	/* [한국어] 유효성 체크 */
	if (send == -1U) {
		if (!fio_did_warn(FIO_WARN_ZONED_BUG))
			log_err("fio: bug in zoned generation\n");
		goto bail;
	}

	/*
	 * 'send' is some percentage below or equal to 100 that
	 * marks the end of the current IO range. 'stotal' marks
	 * the start, in percent.
	 */
	/* [한국어] 퍼센트를 실제 블록 오프셋으로 변환 */
	if (stotal)
		offset = stotal * lastb / 100ULL;
	else
		offset = 0;

	/* [한국어] 존의 크기를 블록 수로 변환 */
	lastb = lastb * (send - stotal) / 100ULL;

	/*
	 * Generate index from 0..send-of-lastb
	 */
	/* [한국어] 존 내에서 랜덤 블록 생성 */
	if (__get_next_rand_offset(td, f, ddir, b, lastb) == 1)
		return 1;

	/*
	 * Add our start offset, if any
	 */
	/* [한국어] 존의 시작 오프셋을 더해 최종 위치 결정 */
	if (offset)
		*b += offset;

	return 0;
}

/* [한국어] 랜덤 오프셋 생성의 최상위 디스패처 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 0=성공, 1=실패
 *
 * 설정된 랜덤 분포 종류(균일, Zipf, Pareto, 가우시안, 존 기반 등)에 따라
 * 적절한 랜덤 오프셋 생성 함수를 호출한다.
 */
static int get_next_rand_offset(struct thread_data *td, struct fio_file *f,
				enum fio_ddir ddir, uint64_t *b)
{
	/* [한국어] SP Random 모드이고 쓰기 방향이면 SP Random 사용 */
	if (td->o.sprandom && ddir == DDIR_WRITE) {
		return __get_next_rand_offset_sprandom(td, f, ddir, b, 0);
	} else if (td->o.random_distribution == FIO_RAND_DIST_RANDOM) {
		/* [한국어] 기본 균일 랜덤 분포 */
		uint64_t lastb;

		lastb = last_block(td, f, ddir);
		if (!lastb)
			return 1;

		return __get_next_rand_offset(td, f, ddir, b, lastb);
	} else if (td->o.random_distribution == FIO_RAND_DIST_ZIPF)
		/* [한국어] Zipf 분포 */
		return __get_next_rand_offset_zipf(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_PARETO)
		/* [한국어] Pareto 분포 */
		return __get_next_rand_offset_pareto(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_GAUSS)
		/* [한국어] 가우시안 분포 */
		return __get_next_rand_offset_gauss(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_ZONED)
		/* [한국어] 퍼센트 기반 존 분할 */
		return __get_next_rand_offset_zoned(td, f, ddir, b);
	else if (td->o.random_distribution == FIO_RAND_DIST_ZONED_ABS)
		/* [한국어] 절대 크기 기반 존 분할 */
		return __get_next_rand_offset_zoned_abs(td, f, ddir, b);

	log_err("fio: unknown random distribution: %d\n", td->o.random_distribution);
	return 1;
}

/* [한국어] 다음 I/O를 랜덤으로 수행해야 하는지 판단하는 함수.
 *
 * @td: 스레드 데이터
 * @ddir: 데이터 방향
 * @return: true=랜덤으로 수행, false=순차로 수행
 *
 * perc_rand[ddir]가 100이면 항상 랜덤.
 * 그렇지 않으면 확률적으로 결정 (예: perc_rand=70이면 70% 확률로 랜덤).
 */
static bool should_do_random(struct thread_data *td, enum fio_ddir ddir)
{
	unsigned int v;

	/* [한국어] 100%이면 항상 랜덤 */
	if (td->o.perc_rand[ddir] == 100)
		return true;

	/* [한국어] 1~100 사이 난수를 생성하여 perc_rand와 비교 */
	v = rand_between(&td->seq_rand_state[ddir], 1, 100);

	return v <= td->o.perc_rand[ddir];
}

/* [한국어] 루프 반복 시 캐시를 무효화하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 *
 * 시간 기반(time_based) 실행에서 파일 끝에 도달하여 처음으로 되돌아갈 때,
 * 캐시를 무효화하여 실제 I/O가 발생하도록 한다.
 * direct I/O가 아니고 invalidate_cache가 설정된 경우에만 동작.
 */
static void loop_cache_invalidate(struct thread_data *td, struct fio_file *f)
{
	struct thread_options *o = &td->o;

	/* [한국어] 캐시 무효화 옵션이 켜져 있고 direct I/O가 아닌 경우에만 실행 */
	if (o->invalidate_cache && !o->odirect) {
		int fio_unused ret;

		ret = file_invalidate_cache(td, f);
	}
}

/* [한국어] 다음 랜덤 블록을 가져오는 함수 (재시도 로직 포함).
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @b: 결과 블록 번호를 저장할 포인터
 * @return: 0=성공, 1=실패
 *
 * 먼저 get_next_rand_offset()을 시도하고, 실패하면
 * time_based 모드에서는 파일을 리셋하고 다시 시도한다.
 */
static int get_next_rand_block(struct thread_data *td, struct fio_file *f,
			       enum fio_ddir ddir, uint64_t *b)
{
	/* [한국어] 먼저 랜덤 오프셋 생성 시도 */
	if (!get_next_rand_offset(td, f, ddir, b))
		return 0;

	/* [한국어] 시간 기반 또는 비균일 파일 서비스 모드에서는 파일 리셋 후 재시도 */
	if (td->o.time_based ||
	    (td->o.file_service_type & __FIO_FSERVICE_NONUNIFORM)) {
		fio_file_reset(td, f);
		loop_cache_invalidate(td, f);
		if (!get_next_rand_offset(td, f, ddir, b))
			return 0;
	}

	dprint(FD_IO, "%s: rand offset failed, last=%llu, size=%llu\n",
			f->file_name, (unsigned long long) f->last_pos[ddir],
			(unsigned long long) f->real_file_size);
	return 1;
}

/* [한국어] 순차(sequential) I/O에서 다음 오프셋을 계산하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @ddir: 데이터 방향
 * @offset: 결과 오프셋을 저장할 포인터 (파일 오프셋 기준 상대 위치)
 * @return: 0=성공, 1=파일 끝 도달(실패)
 *
 * 순차 I/O에서는 마지막 위치(last_pos)를 기준으로 다음 위치를 결정한다.
 * 시간 기반 실행에서 파일 끝에 도달하면 처음으로 되돌아간다.
 * ddir_seq_add를 통해 건너뛰기(hole) 또는 역방향 I/O도 지원한다.
 */
static int get_next_seq_offset(struct thread_data *td, struct fio_file *f,
			       enum fio_ddir ddir, uint64_t *offset)
{
	struct thread_options *o = &td->o;

	assert(ddir_rw(ddir));

	/*
	 * If we reach the end for a time based run, reset us back to 0
	 * and invalidate the cache, if we need to.
	 */
	/* [한국어] 시간 기반 실행에서 파일 끝에 도달하면 처음으로 되돌림 */
	if (f->last_pos[ddir] >= f->io_size + get_start_offset(td, f) &&
	    o->time_based && o->nr_files == 1) {
		f->last_pos[ddir] = f->file_offset;
		loop_cache_invalidate(td, f);
	}

	/*
	 * If we reach the end for a rw-io-size based run, reset us back to 0
	 * and invalidate the cache, if we need to.
	 */
	/* [한국어] 읽기+쓰기 혼합에서 io_size가 size보다 클 때, 끝에 도달하면 리셋 */
	if (td_rw(td) && o->io_size > o->size) {
		if (f->last_pos[ddir] >= f->io_size + get_start_offset(td, f)) {
			f->last_pos[ddir] = f->file_offset;
			loop_cache_invalidate(td, f);
		}
        }

	/* [한국어] 마지막 위치가 실제 파일 크기 내에 있으면 다음 오프셋 계산 */
	if (f->last_pos[ddir] < f->real_file_size) {
		uint64_t pos;

		/*
		 * Only rewind if we already hit the end
		 */
		/* [한국어] 역방향 순차 I/O에서 파일 시작점에 도달했으면 끝으로 되돌림 */
		if (f->last_pos[ddir] == f->file_offset &&
		    f->file_offset && o->ddir_seq_add < 0) {
			if (f->real_file_size > f->io_size)
				f->last_pos[ddir] = f->io_size;
			else
				f->last_pos[ddir] = f->real_file_size;
		}

		/* [한국어] 파일 오프셋 기준 상대 위치 계산 */
		pos = f->last_pos[ddir] - f->file_offset;
		/* [한국어] ddir_seq_add가 설정되어 있으면 건너뛰기 적용 */
		if (pos && o->ddir_seq_add) {
			pos += o->ddir_seq_add;

			/*
			 * If we reach beyond the end of the file
			 * with holed IO, wrap around to the
			 * beginning again. If we're doing backwards IO,
			 * wrap to the end.
			 */
			/* [한국어] 파일 끝을 넘으면 순방향은 처음으로, 역방향은 끝으로 순환 */
			if (pos >= f->real_file_size) {
				if (o->ddir_seq_add > 0)
					pos = f->file_offset;
				else {
					if (f->real_file_size > f->io_size)
						pos = f->io_size;
					else
						pos = f->real_file_size;

					pos += o->ddir_seq_add;
				}
			}
		}

		*offset = pos;
		return 0;
	}

	/* [한국어] 파일 끝을 넘어서면 실패 반환 */
	return 1;
}

/* [한국어] 다음 I/O 블록(오프셋)을 결정하는 핵심 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛 구조체
 * @ddir: 데이터 방향
 * @rw_seq: 읽기/쓰기 시퀀스 히트 여부 (1이면 새 시퀀스 시작)
 * @is_random: 결과가 랜덤 I/O인지 여부를 저장할 포인터
 * @return: 0=성공, 1=실패
 *
 * 랜덤/순차 결정, rw_seq 모드(SEQ/IDENT), randtrimwrite 등
 * 다양한 I/O 패턴에 따라 다음 블록 위치를 결정한다.
 */
static int get_next_block(struct thread_data *td, struct io_u *io_u,
			  enum fio_ddir ddir, int rw_seq,
			  bool *is_random)
{
	struct fio_file *f = io_u->file;
	uint64_t b, offset;
	int ret;

	assert(ddir_rw(ddir));

	/* [한국어] 초기화: 아직 결정되지 않은 상태 */
	b = offset = -1ULL;

	/* [한국어] randtrimwrite 모드의 쓰기는 바로 직전 trim 위치를 사용 */
	if (td_randtrimwrite(td) && ddir == DDIR_WRITE) {
		/* don't mark randommap for these writes */
		/* [한국어] 랜덤맵에 표시하지 않도록 BUSY_OK 설정 */
		io_u_set(td, io_u, IO_U_F_BUSY_OK);
		offset = f->last_start[DDIR_TRIM] - f->file_offset;
		*is_random = true;
		ret = 0;
	} else if (rw_seq) {
		/* [한국어] rw_seq가 1이면 새 시퀀스 시작 */
		if (td_random(td)) {
			/* [한국어] 랜덤 모드에서 실제로 랜덤을 할지 순차를 할지 확률적 결정 */
			if (should_do_random(td, ddir)) {
				ret = get_next_rand_block(td, f, ddir, &b);
				*is_random = true;
			} else {
				/* [한국어] 순차로 결정됐으면 순차 오프셋 시도, 실패 시 랜덤 */
				*is_random = false;
				io_u_set(td, io_u, IO_U_F_BUSY_OK);
				ret = get_next_seq_offset(td, f, ddir, &offset);
				if (ret)
					ret = get_next_rand_block(td, f, ddir, &b);
			}
		} else {
			/* [한국어] 순차 전용 모드 */
			*is_random = false;
			ret = get_next_seq_offset(td, f, ddir, &offset);
		}
	} else {
		/* [한국어] rw_seq가 0: 시퀀스 진행 중 */
		io_u_set(td, io_u, IO_U_F_BUSY_OK);
		*is_random = false;

		if (td->o.rw_seq == RW_SEQ_SEQ) {
			/* [한국어] RW_SEQ_SEQ: 순차 우선, 실패 시 랜덤 폴백 */
			ret = get_next_seq_offset(td, f, ddir, &offset);
			if (ret) {
				ret = get_next_rand_block(td, f, ddir, &b);
				*is_random = false;
			}
		} else if (td->o.rw_seq == RW_SEQ_IDENT) {
			/* [한국어] RW_SEQ_IDENT: 같은 위치에 반복 접근 */
			if (f->last_start[ddir] != -1ULL)
				offset = f->last_start[ddir] - f->file_offset;
			else
				offset = 0;
			ret = 0;
		} else {
			log_err("fio: unknown rw_seq=%d\n", td->o.rw_seq);
			ret = 1;
		}
	}

	/* [한국어] 성공 시 io_u->offset 설정 */
	if (!ret) {
		if (offset != -1ULL)
			/* [한국어] offset이 직접 지정된 경우 (순차 등) */
			io_u->offset = offset;
		else if (b != -1ULL)
			/* [한국어] 블록 번호가 지정된 경우, ba(블록 정렬) 단위로 변환 */
			io_u->offset = b * td->o.ba[ddir];
		else {
			log_err("fio: bug in offset generation: offset=%llu, b=%llu\n", (unsigned long long) offset, (unsigned long long) b);
			ret = 1;
		}
		/* [한국어] 검증용 오프셋도 동일하게 설정 */
		io_u->verify_offset = io_u->offset;
	}

	return ret;
}

/*
 * For random io, generate a random new block and see if it's used. Repeat
 * until we find a free one. For sequential io, just return the end of
 * the last io issued.
 */
/* [한국어] 다음 I/O 오프셋을 결정하는 상위 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛 구조체
 * @is_random: 결과가 랜덤 I/O인지 여부를 저장할 포인터
 * @return: 0=성공, 1=실패
 *
 * ddir_seq_nr 카운터를 관리하여 시퀀스 전환 시점을 판단하고,
 * get_next_block()을 호출하여 오프셋을 결정한다.
 * 결과 오프셋이 파일 범위 내에 있는지 검증하고,
 * file_offset을 더해 절대 오프셋으로 변환한다.
 */
static int get_next_offset(struct thread_data *td, struct io_u *io_u,
			   bool *is_random)
{
	struct fio_file *f = io_u->file;
	enum fio_ddir ddir = io_u->ddir;
	int rw_seq_hit = 0;

	assert(ddir_rw(ddir));

	/* [한국어] ddir_seq_nr 카운터 감소, 0이 되면 새 시퀀스 시작 */
	if (td->o.ddir_seq_nr && !--td->ddir_seq_nr) {
		rw_seq_hit = 1;
		td->ddir_seq_nr = td->o.ddir_seq_nr;
	}

	/* [한국어] 다음 블록 결정 */
	if (get_next_block(td, io_u, ddir, rw_seq_hit, is_random))
		return 1;

	/* [한국어] 오프셋이 io_size를 초과하는지 검증 */
	if (io_u->offset >= f->io_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= io_size %llu\n",
					(unsigned long long) io_u->offset,
					(unsigned long long) f->io_size);
		return 1;
	}

	/* [한국어] 파일의 시작 오프셋을 더해 절대 오프셋으로 변환 */
	io_u->offset += f->file_offset;
	/* [한국어] 절대 오프셋이 실제 파일 크기를 초과하는지 검증 */
	if (io_u->offset >= f->real_file_size) {
		dprint(FD_IO, "get_next_offset: offset %llu >= size %llu\n",
					(unsigned long long) io_u->offset,
					(unsigned long long) f->real_file_size);
		return 1;
	}

	/*
	 * For randtrimwrite, we decide whether to issue a trim or a write
	 * based on whether the offsets for the most recent trim and write
	 * operations match. If they don't match that means we just issued a
	 * new trim and the next operation should be a write. If they *do*
	 * match that means we just completed a trim+write pair and the next
	 * command should be a trim.
	 *
	 * This works fine for sequential workloads but for random workloads
	 * it's possible to complete a trim+write pair and then have the next
	 * randomly generated offset match the previous offset. If that happens
	 * we need to alter the offset for the last write operation in order
	 * to ensure that we issue a write operation the next time through.
	 */
	/* [한국어] randtrimwrite에서 trim 후 write를 보장하기 위한 보정 로직.
	 * trim과 write의 마지막 시작 오프셋이 같으면 trim+write 쌍이 완료된 것이므로
	 * 다음은 trim이어야 함. 하지만 랜덤 오프셋이 이전과 같아지면 잘못 판단할 수 있어
	 * write의 마지막 오프셋을 1 줄여서 다음에 write가 발행되도록 함. */
	if (td_randtrimwrite(td) && ddir == DDIR_TRIM &&
	    f->last_start[DDIR_TRIM] == io_u->offset)
		f->last_start[DDIR_WRITE]--;

	io_u->verify_offset = io_u->offset;
	return 0;
}

/* [한국어] I/O 요청이 파일 범위 내에 맞는지 확인하는 인라인 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛 구조체
 * @buflen: 확인할 버퍼 길이
 * @return: true=범위 내에 맞음, false=초과
 */
static inline bool io_u_fits(struct thread_data *td, struct io_u *io_u,
			     unsigned long long buflen)
{
	struct fio_file *f = io_u->file;

	return io_u->offset + buflen <= f->io_size + get_start_offset(td, f);
}

/* [한국어] 다음 I/O의 버퍼 길이(블록 크기)를 결정하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛 구조체
 * @is_random: 현재 I/O가 랜덤인지 여부
 * @return: 결정된 버퍼 길이 (바이트), 0이면 실패
 *
 * min_bs와 max_bs가 같으면 고정 블록 크기 반환.
 * 다르면 난수를 이용하여 범위 내 크기 결정.
 * bssplit이 설정된 경우 확률 분포에 따라 블록 크기 선택.
 * bs_unaligned가 아니면 min_bs 단위로 정렬.
 */
static unsigned long long get_next_buflen(struct thread_data *td, struct io_u *io_u,
				    bool is_random)
{
	int ddir = io_u->ddir;
	unsigned long long buflen = 0;
	unsigned long long minbs, maxbs;
	uint64_t frand_max, r;
	bool power_2;

	assert(ddir_rw(ddir));

	/* [한국어] randtrimwrite 모드의 쓰기는 직전 trim 크기를 그대로 사용 */
	if (td_randtrimwrite(td) && ddir == DDIR_WRITE) {
		struct fio_file *f = io_u->file;

		return f->last_pos[DDIR_TRIM] - f->last_start[DDIR_TRIM];
	}

	/* [한국어] bs_is_seq_rand 옵션: 순차/랜덤에 따라 다른 bs 설정 사용 */
	if (td->o.bs_is_seq_rand)
		ddir = is_random ? DDIR_WRITE : DDIR_READ;

	/* [한국어] 해당 방향의 최소/최대 블록 크기 */
	minbs = td->o.min_bs[ddir];
	maxbs = td->o.max_bs[ddir];

	/* [한국어] 최소=최대이면 고정 블록 크기 */
	if (minbs == maxbs)
		return minbs;

	/*
	 * If we can't satisfy the min block size from here, then fail
	 */
	/* [한국어] 최소 블록 크기도 파일 범위에 맞지 않으면 실패 */
	if (!io_u_fits(td, io_u, minbs))
		return 0;

	/* [한국어] 난수 생성기의 최대값 */
	frand_max = rand_max(&td->bsrange_state[ddir]);
	do {
		/* [한국어] 난수 생성 */
		r = __rand(&td->bsrange_state[ddir]);

		if (!td->o.bssplit_nr[ddir]) {
			/* [한국어] bssplit 미설정: minbs~maxbs 범위에서 균일 분포 */
			buflen = minbs + (unsigned long long) ((double) maxbs *
					(r / (frand_max + 1.0)));
		} else {
			/* [한국어] bssplit 설정: 확률 테이블에 따라 블록 크기 선택 */
			long long perc = 0;
			unsigned int i;

			for (i = 0; i < td->o.bssplit_nr[ddir]; i++) {
				struct bssplit *bsp = &td->o.bssplit[ddir][i];

				if (!bsp->perc)
					continue;
				buflen = bsp->bs;
				perc += bsp->perc;
				/* [한국어] 누적 확률이 난수 비율을 초과하고 범위 내이면 선택 */
				if ((r / perc <= frand_max / 100ULL) &&
				    io_u_fits(td, io_u, buflen))
					break;
			}
		}

		/* [한국어] 블록 크기 정렬: 2의 거듭제곱이면 비트마스크, 아니면 나머지 연산 */
		power_2 = is_power_of_2(minbs);
		if (!td->o.bs_unaligned && power_2)
			buflen &= ~(minbs - 1);
		else if (!td->o.bs_unaligned && !power_2)
			buflen -= buflen % minbs;
		/* [한국어] 최대 블록 크기 초과 방지 */
		if (buflen > maxbs)
			buflen = maxbs;
	} while (!io_u_fits(td, io_u, buflen));
	/* [한국어] 파일 범위 내에 맞을 때까지 반복 */

	return buflen;
}

/* [한국어] 읽기/쓰기 혼합(rwmix) 바이트 카운터를 설정하는 함수.
 *
 * @td: 스레드 데이터
 *
 * 현재 방향의 발행 횟수와 반대 방향의 비율을 곱해
 * 다음 방향 전환 시점을 결정한다.
 * 예: rwmix_read=70이면, write 비율 30%를 기준으로 전환 시점 계산.
 */
static void set_rwmix_bytes(struct thread_data *td)
{
	unsigned int diff;

	/*
	 * we do time or byte based switch. this is needed because
	 * buffered writes may issue a lot quicker than they complete,
	 * whereas reads do not.
	 */
	/* [한국어] 반대 방향의 비율을 가져와 전환 임계치 계산 */
	diff = td->o.rwmix[td->rwmix_ddir ^ 1];
	td->rwmix_issues = (td->io_issues[td->rwmix_ddir] * diff) / 100;
}

/* [한국어] 읽기/쓰기 혼합에서 랜덤으로 방향을 결정하는 인라인 함수.
 *
 * @td: 스레드 데이터
 * @return: DDIR_READ 또는 DDIR_WRITE
 *
 * rwmix[DDIR_READ] 확률에 따라 읽기 또는 쓰기 방향을 결정한다.
 */
static inline enum fio_ddir get_rand_ddir(struct thread_data *td)
{
	unsigned int v;

	/* [한국어] 1~100 사이 난수 생성 */
	v = rand_between(&td->rwmix_state, 1, 100);

	/* [한국어] rwmix[READ] 이하이면 읽기, 초과하면 쓰기 */
	if (v <= td->o.rwmix[DDIR_READ])
		return DDIR_READ;

	return DDIR_WRITE;
}

/* [한국어] 진행 중인 모든 I/O를 대기하여 완료시키는 함수 (quiesce = 정지/안정화).
 *
 * @td: 스레드 데이터
 * @return: 완료된 I/O 수 (양수) 또는 에러 코드 (음수)
 *
 * 슬립하기 전이나 큐 깊이를 줄일 때 호출하여,
 * 레이턴시 측정이 왜곡되지 않도록 대기 중인 I/O를 모두 완료시킨다.
 * io_u_queued 또는 cur_depth가 있으면 먼저 commit하고,
 * in_flight인 I/O가 모두 완료될 때까지 반복한다.
 */
int io_u_quiesce(struct thread_data *td)
{
	int ret = 0, completed = 0, err = 0;

	/*
	 * We are going to sleep, ensure that we flush anything pending as
	 * not to skew our latency numbers.
	 *
	 * Changed to only monitor 'in flight' requests here instead of the
	 * td->cur_depth, b/c td->cur_depth does not accurately represent
	 * io's that have been actually submitted to an async engine,
	 * and cur_depth is meaningless for sync engines.
	 */
	/* [한국어] 큐에 쌓인 I/O가 있으면 먼저 커밋(제출) */
	if (td->io_u_queued || td->cur_depth)
		td_io_commit(td);

	/* [한국어] 비행 중(in-flight)인 I/O가 모두 완료될 때까지 대기 */
	while (td->io_u_in_flight) {
		ret = io_u_queued_complete(td, 1);
		if (ret > 0)
			completed += ret;
		else if (ret < 0)
			err = ret;
	}

	/* [한국어] 검증 로그 버퍼가 가득 찼으면 확장 */
	if (td->flags & TD_F_REGROW_LOGS)
		regrow_logs(td);

	if (completed)
		return completed;

	return err;
}

/* [한국어] 레이트 제한(rate limiting)에 따라 I/O 방향을 조정하는 함수.
 *
 * @td: 스레드 데이터
 * @ddir: 원래 결정된 데이터 방향
 * @return: 조정된 데이터 방향 (또는 DDIR_TIMEOUT)
 *
 * rate 옵션이 설정된 경우, 목표 속도를 초과하지 않도록
 * I/O 발행을 지연시키거나 반대 방향으로 전환한다.
 * 양쪽 모두 목표를 초과하면 적은 쪽의 대기 시간만큼 슬립한다.
 */
static enum fio_ddir rate_ddir(struct thread_data *td, enum fio_ddir ddir)
{
	/* [한국어] 반대 방향 */
	enum fio_ddir odir = ddir ^ 1;
	uint64_t usec;
	uint64_t now;

	assert(ddir_rw(ddir));
	/* [한국어] 현재 시각 (에포크 기준 마이크로초) */
	now = utime_since_now(&td->epoch);

	/*
	 * if rate_next_io_time is in the past, need to catch up to rate
	 */
	/* [한국어] 현재 방향의 다음 I/O 예정 시각이 이미 지났으면 바로 진행 */
	if (td->rate_next_io_time[ddir] <= now)
		return ddir;

	/*
	 * We are ahead of rate in this direction. See if we
	 * should switch.
	 */
	/* [한국어] 현재 방향이 목표 속도보다 앞서 있음 -> 전환 검토 */
	if (td_rw(td) && td->o.rwmix[odir]) {
		/*
		 * Other direction is behind rate, switch
		 */
		/* [한국어] 반대 방향이 뒤처져 있으면 반대 방향으로 전환 */
		if (td->rate_next_io_time[odir] <= now)
			return odir;

		/*
		 * Both directions are ahead of rate. sleep the min,
		 * switch if necessary
		 */
		/* [한국어] 양쪽 모두 앞서 있으면, 더 빨리 도달하는 쪽으로 슬립 */
		if (td->rate_next_io_time[ddir] <=
		    td->rate_next_io_time[odir]) {
			usec = td->rate_next_io_time[ddir] - now;
		} else {
			usec = td->rate_next_io_time[odir] - now;
			ddir = odir;
		}
	} else
		usec = td->rate_next_io_time[ddir] - now;

	/* [한국어] 인라인 모드에서는 슬립 전에 대기 중인 I/O 완료 */
	if (td->o.io_submit_mode == IO_MODE_INLINE)
		io_u_quiesce(td);

	/* [한국어] 타임아웃 초과 시 DDIR_TIMEOUT 반환 */
	if (td->o.timeout && ((usec + now) > td->o.timeout)) {
		/*
		 * check if the usec is capable of taking negative values
		 */
		if (now > td->o.timeout) {
			ddir = DDIR_TIMEOUT;
			return ddir;
		}
		usec = td->o.timeout - now;
	}
	/* [한국어] 목표 속도에 맞추기 위해 슬립 */
	usec_sleep(td, usec);

	/* [한국어] 슬립 후 타임아웃 또는 종료 확인 */
	now = utime_since_now(&td->epoch);
	if ((td->o.timeout && (now > td->o.timeout)) || td->terminate)
		ddir = DDIR_TIMEOUT;

	return ddir;
}

/*
 * Return the data direction for the next io_u. If the job is a
 * mixed read/write workload, check the rwmix cycle and switch if
 * necessary.
 */
/* [한국어] 다음 I/O의 데이터 방향(read/write/sync/trim)을 결정하는 함수.
 *
 * @td: 스레드 데이터
 * @return: 결정된 데이터 방향 (enum fio_ddir)
 *
 * 1. 먼저 fsync/fdatasync/sync_file_range 필요 여부 확인
 * 2. 읽기+쓰기 혼합이면 rwmix 비율에 따라 방향 결정
 * 3. 단일 방향(read/write/trim)이면 해당 방향 반환
 * 4. rate 제한이 있으면 rate_ddir()로 조정
 */
static enum fio_ddir get_rw_ddir(struct thread_data *td)
{
	enum fio_ddir ddir;

	/*
	 * See if it's time to fsync/fdatasync/sync_file_range first,
	 * and if not then move on to check regular I/Os.
	 */
	/* [한국어] 마지막 I/O가 쓰기였으면 sync 필요 여부 확인 */
	if (should_fsync(td) && td->last_ddir_issued == DDIR_WRITE) {
		/* [한국어] fsync_blocks마다 DDIR_SYNC 발행 */
		if (td->o.fsync_blocks && td->io_issues[DDIR_WRITE] &&
		    !(td->io_issues[DDIR_WRITE] % td->o.fsync_blocks))
			return DDIR_SYNC;

		/* [한국어] fdatasync_blocks마다 DDIR_DATASYNC 발행 */
		if (td->o.fdatasync_blocks && td->io_issues[DDIR_WRITE] &&
		    !(td->io_issues[DDIR_WRITE] % td->o.fdatasync_blocks))
			return DDIR_DATASYNC;

		/* [한국어] sync_file_range 주기적 발행 */
		if (td->sync_file_range_nr && td->io_issues[DDIR_WRITE] &&
		    !(td->io_issues[DDIR_WRITE] % td->sync_file_range_nr))
			return DDIR_SYNC_FILE_RANGE;
	}

	if (td_rw(td)) {
		/*
		 * Check if it's time to seed a new data direction.
		 */
		/* [한국어] 읽기+쓰기 혼합: rwmix_issues 초과 시 방향 전환 */
		if (td->io_issues[td->rwmix_ddir] >= td->rwmix_issues) {
			/*
			 * Put a top limit on how many bytes we do for
			 * one data direction, to avoid overflowing the
			 * ranges too much
			 */
			/* [한국어] 랜덤으로 새 방향 선택 */
			ddir = get_rand_ddir(td);

			if (ddir != td->rwmix_ddir)
				set_rwmix_bytes(td);

			td->rwmix_ddir = ddir;
		}
		ddir = td->rwmix_ddir;
	} else if (td_read(td))
		/* [한국어] 읽기 전용 */
		ddir = DDIR_READ;
	else if (td_write(td))
		/* [한국어] 쓰기 전용 */
		ddir = DDIR_WRITE;
	else if (td_trim(td))
		/* [한국어] trim 전용 */
		ddir = DDIR_TRIM;
	else
		/* [한국어] 유효하지 않은 방향 */
		ddir = DDIR_INVAL;

	if (!should_check_rate(td)) {
		/*
		 * avoid time-consuming call to utime_since_now() if rate checking
		 * isn't being used. this imrpoves IOPs 50%. See:
		 * https://github.com/axboe/fio/issues/1501#issuecomment-1418327049
		 */
		/* [한국어] rate 체크가 불필요하면 시간 관련 호출 생략 (성능 50% 향상) */
		td->rwmix_ddir = ddir;
	} else
		/* [한국어] rate 제한 적용 */
		td->rwmix_ddir = rate_ddir(td, ddir);
	return td->rwmix_ddir;
}

/* [한국어] io_u의 데이터 방향(ddir)을 설정하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛 구조체
 *
 * get_rw_ddir()로 방향을 결정한 후,
 * ZBD(존 블록 디바이스) 모드와 trimwrite 모드에 맞게 조정하고,
 * 배리어(barrier) 플래그도 설정한다.
 */
static void set_rw_ddir(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 기본 방향 결정 */
	enum fio_ddir ddir = get_rw_ddir(td);

	/* [한국어] ZBD 모드에서 방향 조정 */
	if (td->o.zone_mode == ZONE_MODE_ZBD)
		ddir = zbd_adjust_ddir(td, io_u, ddir);

	/* [한국어] trimwrite 모드에서 trim과 write를 번갈아 수행 */
	if (td_trimwrite(td) && !ddir_sync(ddir)) {
		struct fio_file *f = io_u->file;
		/* [한국어] 마지막 write와 trim 위치가 같으면 다음은 trim */
		if (f->last_start[DDIR_WRITE] == f->last_start[DDIR_TRIM])
			ddir = DDIR_TRIM;
		else
			ddir = DDIR_WRITE;
	}

	/* [한국어] ddir과 acct_ddir(통계 기록용 방향) 설정 */
	io_u->ddir = io_u->acct_ddir = ddir;

	/* [한국어] 배리어 플래그 설정: barrier_blocks마다 I/O 배리어 삽입 */
	if (io_u->ddir == DDIR_WRITE && td_ioengine_flagged(td, FIO_BARRIER) &&
	    td->o.barrier_blocks &&
	   !(td->io_issues[DDIR_WRITE] % td->o.barrier_blocks) &&
	     td->io_issues[DDIR_WRITE])
		io_u_set(td, io_u, IO_U_F_BARRIER);
}

/* [한국어] 파일 참조를 해제하고 에러를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 *
 * put_file()로 파일 참조 카운트를 줄이고,
 * 실패 시 td_verror()로 에러를 기록한다.
 */
void put_file_log(struct thread_data *td, struct fio_file *f)
{
	unsigned int ret = put_file(td, f);

	if (ret)
		td_verror(td, ret, "file close");
}

/* [한국어] I/O 유닛을 프리리스트로 반환하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: 반환할 I/O 유닛
 *
 * I/O 완료 후 io_u를 프리리스트에 되돌려 재사용할 수 있게 한다.
 * 비동기 처리 시 락을 잡고, 파일 참조 해제, cur_depth 감소,
 * FREE 플래그 설정 후 프리리스트에 푸시한다.
 * 대기 중인 스레드에게 io_u가 사용 가능함을 알린다.
 */
void put_io_u(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 비동기 처리(검증 스레드 등) 시 락 필요 여부 결정 */
	const bool needs_lock = td_async_processing(td);

	/* [한국어] ZBD 관련 io_u 정리 */
	zbd_put_io_u(td, io_u);

	/* [한국어] 자식 스레드면 부모 스레드의 리스트 사용 */
	if (td->parent)
		td = td->parent;

	if (needs_lock)
		__td_io_u_lock(td);

	/* [한국어] 파일 참조가 있고 NO_FILE_PUT가 아니면 파일 참조 해제 */
	if (io_u->file && !(io_u->flags & IO_U_F_NO_FILE_PUT))
		put_file_log(td, io_u->file);

	/* [한국어] 파일 포인터 초기화 및 FREE 플래그 설정 */
	io_u->file = NULL;
	io_u_set(td, io_u, IO_U_F_FREE);

	/* [한국어] 현재 깊이(cur_depth) 감소 */
	if (io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		assert(!(td->flags & TD_F_CHILD));
	}
	/* [한국어] 프리리스트에 io_u 반환 */
	io_u_qpush(&td->io_u_freelist, io_u);
	/* [한국어] 대기 중인 스레드에게 io_u 사용 가능 알림 */
	td_io_u_free_notify(td);

	if (needs_lock)
		__td_io_u_unlock(td);
}

/* [한국어] io_u의 비행 중(in-flight) 플래그를 초기화하는 인라인 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 *
 * FLIGHT, BUSY_OK, PATTERN_DONE 플래그를 모두 제거한다.
 */
static inline void io_u_clear_inflight_flags(struct thread_data *td,
					      struct io_u *io_u)
{
	io_u_clear(td, io_u, IO_U_F_FLIGHT | IO_U_F_BUSY_OK |
		   IO_U_F_PATTERN_DONE);
}

/* [한국어] io_u를 정리하고 프리리스트로 반환하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 *
 * 비행 중 플래그를 제거한 후 put_io_u()로 반환한다.
 */
void clear_io_u(struct thread_data *td, struct io_u *io_u)
{
	io_u_clear_inflight_flags(td, io_u);
	put_io_u(td, io_u);
}

/* [한국어] I/O 유닛을 재큐잉하는 함수 (재시도를 위해 큐에 되돌림).
 *
 * @td: 스레드 데이터
 * @io_u: 재큐잉할 I/O 유닛 (이중 포인터, 함수 종료 후 NULL로 설정)
 *
 * short I/O(부분 완료) 등의 경우 io_u를 재큐잉 리스트에 넣어
 * 나중에 다시 처리할 수 있게 한다.
 * 이미 FLIGHT 상태였으면 io_issues 카운터를 되돌린다.
 */
void requeue_io_u(struct thread_data *td, struct io_u **io_u)
{
	const bool needs_lock = td_async_processing(td);
	struct io_u *__io_u = *io_u;
	/* [한국어] 통계 기록용 방향 */
	enum fio_ddir ddir = acct_ddir(__io_u);

	dprint(FD_IO, "requeue %p\n", __io_u);

	if (td->parent)
		td = td->parent;

	if (needs_lock)
		__td_io_u_lock(td);

	/* [한국어] FREE 플래그 설정 */
	io_u_set(td, __io_u, IO_U_F_FREE);
	/* [한국어] 비행 중이었던 read/write면 발행 카운터 되돌림 */
	if ((__io_u->flags & IO_U_F_FLIGHT) && ddir_rw(ddir))
		td->io_issues[ddir]--;

	/* [한국어] FLIGHT 플래그 제거 */
	io_u_clear(td, __io_u, IO_U_F_FLIGHT);
	/* [한국어] 현재 깊이 감소 */
	if (__io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		assert(!(td->flags & TD_F_CHILD));
	}

	/* [한국어] 재큐잉 리스트에 푸시 */
	io_u_rpush(&td->io_u_requeues, __io_u);
	/* [한국어] 사용 가능 알림 */
	td_io_u_free_notify(td);

	if (needs_lock)
		__td_io_u_unlock(td);

	/* [한국어] 호출자의 포인터를 NULL로 설정 */
	*io_u = NULL;
}

/* [한국어] 스트라이드 존 모드를 설정하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 *
 * 스트라이드 존 모드에서는 zone_size만큼 I/O하면
 * zone_range + zone_skip만큼 이동하여 다음 존으로 넘어간다.
 * 파일 끝을 넘으면 처음으로 순환한다.
 */
static void setup_strided_zone_mode(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;

	assert(td->o.zone_mode == ZONE_MODE_STRIDED);
	assert(td->o.zone_size);
	assert(td->o.zone_range);

	/*
	 * See if it's time to switch to a new zone
	 */
	/* [한국어] 현재 존에서 zone_size만큼 I/O했으면 다음 존으로 이동 */
	if (td->zone_bytes >= td->o.zone_size) {
		td->zone_bytes = 0;
		/* [한국어] zone_range + zone_skip만큼 오프셋 이동 */
		f->file_offset += td->o.zone_range + td->o.zone_skip;

		/*
		 * Wrap from the beginning, if we exceed the file size
		 */
		/* [한국어] 파일 끝을 넘으면 처음으로 순환 */
		if (f->file_offset >= f->real_file_size)
			f->file_offset = get_start_offset(td, f);

		f->last_pos[io_u->ddir] = f->file_offset;
		td->io_skip_bytes += td->o.zone_skip;
	}

	/*
	 * If zone_size > zone_range, then maintain the same zone until
	 * zone_bytes >= zone_size.
	 */
	/* [한국어] zone_size > zone_range일 때, zone_range 끝에 도달하면
	 * 같은 존의 시작으로 돌아감 (zone_bytes가 zone_size에 도달할 때까지) */
	if (f->last_pos[io_u->ddir] >= (f->file_offset + td->o.zone_range)) {
		dprint(FD_IO, "io_u maintain zone offset=%" PRIu64 "/last_pos=%" PRIu64 "\n",
				f->file_offset, f->last_pos[io_u->ddir]);
		f->last_pos[io_u->ddir] = f->file_offset;
	}

	/*
	 * For random: if 'norandommap' is not set and zone_size > zone_range,
	 * map needs to be reset as it's done with zone_range everytime.
	 */
	/* [한국어] 랜덤 I/O에서 zone_range 단위로 랜덤맵 리셋 */
	if ((td->zone_bytes % td->o.zone_range) == 0)
		fio_file_reset(td, f);
}

/* [한국어] 다중 범위 trim I/O를 위해 io_u를 채우는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @return: 0=성공, 1=실패
 *
 * num_range 개수만큼 trim 범위를 생성하여 io_u 버퍼에 저장한다.
 * 각 범위마다 오프셋과 길이를 결정하고, 랜덤맵에 표시한다.
 */
static int fill_multi_range_io_u(struct thread_data *td, struct io_u *io_u)
{
	bool is_random;
	uint64_t buflen, i = 0;
	struct trim_range *range;
	struct fio_file *f = io_u->file;
	uint8_t *buf;

	buf = io_u->buf;
	buflen = 0;

	/* [한국어] num_range 개수만큼 반복하여 trim 범위 생성 */
	while (i < td->o.num_range) {
		range = (struct trim_range *)buf;
		/* [한국어] 오프셋 결정 */
		if (get_next_offset(td, io_u, &is_random)) {
			dprint(FD_IO, "io_u %p, failed getting offset\n",
			       io_u);
			break;
		}

		/* [한국어] 버퍼 길이 결정 */
		io_u->buflen = get_next_buflen(td, io_u, is_random);
		if (!io_u->buflen) {
			dprint(FD_IO, "io_u %p, failed getting buflen\n", io_u);
			break;
		}

		/* [한국어] 파일 크기 초과 검사 */
		if (io_u->offset + io_u->buflen > io_u->file->real_file_size) {
			dprint(FD_IO, "io_u %p, off=0x%llx + len=0x%llx exceeds file size=0x%llx\n",
			       io_u,
			       (unsigned long long) io_u->offset, io_u->buflen,
			       (unsigned long long) io_u->file->real_file_size);
			break;
		}

		/* [한국어] trim 범위의 시작 오프셋과 길이를 기록 */
		range->start = io_u->offset;
		range->len = io_u->buflen;
		buflen += io_u->buflen;
		/* [한국어] 파일의 마지막 시작/위치 업데이트 */
		f->last_start[io_u->ddir] = io_u->offset;
		f->last_pos[io_u->ddir] = io_u->offset + range->len;

		buf += sizeof(struct trim_range);
		i++;

		/* [한국어] 랜덤 I/O면 랜덤맵에 표시 */
		if (td_random(td) && file_randommap(td, io_u->file))
			mark_random_map(td, io_u, io_u->offset, io_u->buflen);
		dprint_io_u(io_u, "fill");
	}
	if (buflen) {
		/*
		 * Set buffer length as overall trim length for this IO, and
		 * tell the ioengine about the number of ranges to be trimmed.
		 */
		/* [한국어] 전체 trim 길이와 범위 수를 io_u에 설정 */
		io_u->buflen = buflen;
		io_u->number_trim = i;
		return 0;
	}

	return 1;
}

/* [한국어] I/O 유닛의 핵심 정보(오프셋, 버퍼 길이, 방향)를 채우는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @return: 0=성공, 1=실패
 *
 * io_u의 전체 설정 과정:
 * 1. set_rw_ddir()로 데이터 방향(read/write/sync/trim) 결정
 * 2. 존 모드(strided/ZBD) 설정
 * 3. 다중 범위 trim이면 fill_multi_range_io_u() 호출
 * 4. 아니면 get_next_offset()으로 오프셋, get_next_buflen()으로 크기 결정
 * 5. ZBD 모드에서 블록 조정
 * 6. 데이터 보호(dp) 정보 채우기
 * 7. 랜덤맵에 표시
 */
static int fill_io_u(struct thread_data *td, struct io_u *io_u)
{
	bool is_random;
	uint64_t offset;
	enum io_u_action ret;

	/* [한국어] NOIO 엔진이면 I/O 설정 건너뜀 */
	if (td_ioengine_flagged(td, FIO_NOIO))
		goto out;

	/* [한국어] 1단계: 데이터 방향 결정 */
	set_rw_ddir(td, io_u);

	/* [한국어] 유효하지 않은 방향이면 실패 */
	if (io_u->ddir == DDIR_INVAL || io_u->ddir == DDIR_TIMEOUT) {
		dprint(FD_IO, "invalid direction received ddir = %d", io_u->ddir);
		return 1;
	}
	/*
	 * fsync() or fdatasync() or trim etc, we are done
	 */
	/* [한국어] sync 계열 방향이면 오프셋/크기 설정 불필요 */
	if (!ddir_rw(io_u->ddir))
		goto out;

	/* [한국어] 2단계: 존 모드 설정 */
	if (td->o.zone_mode == ZONE_MODE_STRIDED)
		setup_strided_zone_mode(td, io_u);
	else if (td->o.zone_mode == ZONE_MODE_ZBD)
		setup_zbd_zone_mode(td, io_u);

	/* [한국어] 3단계: 다중 범위 trim이면 별도 처리 */
	if (multi_range_trim(td, io_u)) {
		if (fill_multi_range_io_u(td, io_u))
			return 1;
	} else {
		/*
		 * No log, let the seq/rand engine retrieve the next buflen and
		 * position.
		 */
		/* [한국어] 4단계: 오프셋 결정 */
		if (get_next_offset(td, io_u, &is_random)) {
			dprint(FD_IO, "io_u %p, failed getting offset\n", io_u);
			return 1;
		}

		/* [한국어] 4단계: 버퍼 길이(블록 크기) 결정 */
		io_u->buflen = get_next_buflen(td, io_u, is_random);
		if (!io_u->buflen) {
			dprint(FD_IO, "io_u %p, failed getting buflen\n", io_u);
			return 1;
		}
	}
	offset = io_u->offset;

	/* [한국어] 5단계: ZBD 모드에서 블록 위치/크기 조정 */
	if (td->o.zone_mode == ZONE_MODE_ZBD) {
		ret = zbd_adjust_block(td, io_u);
		if (ret == io_u_eof) {
			dprint(FD_IO, "zbd_adjust_block() returned io_u_eof\n");
			return 1;
		}
	}

	/* [한국어] 6단계: 데이터 보호(Data Placement) 정보 채우기 */
	if (td->o.dp_type != FIO_DP_NONE)
		dp_fill_dspec_data(td, io_u);

	/* [한국어] 파일 크기 초과 검사 */
	if (io_u->offset + io_u->buflen > io_u->file->real_file_size) {
		dprint(FD_IO, "io_u %p, off=0x%llx + len=0x%llx exceeds file size=0x%llx\n",
			io_u,
			(unsigned long long) io_u->offset, io_u->buflen,
			(unsigned long long) io_u->file->real_file_size);
		return 1;
	}

	/*
	 * mark entry before potentially trimming io_u
	 */
	/* [한국어] 7단계: 랜덤 I/O면 랜덤맵에 표시 (io_u->buflen이 줄어들 수 있음) */
	if (!multi_range_trim(td, io_u) && td_random(td) && file_randommap(td, io_u->file))
		io_u->buflen = mark_random_map(td, io_u, offset, io_u->buflen);

out:
	if (!multi_range_trim(td, io_u))
		dprint_io_u(io_u, "fill");
	/* [한국어] 검증용 오프셋 설정 */
	io_u->verify_offset = io_u->offset;
	/* [한국어] 존 바이트 카운터 업데이트 */
	td->zone_bytes += io_u->buflen;
	return 0;
}

/* [한국어] I/O 제출/완료 통계를 위한 범위별 카운터를 증가시키는 내부 함수.
 *
 * @map: 통계 맵 배열 (7개 버킷: 0, 1-4, 5-8, 9-16, 17-32, 33-64, 65+)
 * @nr: 한 번에 제출/완료된 I/O 수
 *
 * 한 번의 제출/완료 호출에서 처리된 I/O 수를 범위별로 분류하여 기록한다.
 */
static void __io_u_mark_map(uint64_t *map, unsigned int nr)
{
	int idx = 0;

	switch (nr) {
	default:
		idx = 6;	/* [한국어] 65개 이상 */
		break;
	case 33 ... 64:
		idx = 5;
		break;
	case 17 ... 32:
		idx = 4;
		break;
	case 9 ... 16:
		idx = 3;
		break;
	case 5 ... 8:
		idx = 2;
		break;
	case 1 ... 4:
		idx = 1;
		fio_fallthrough;
	case 0:
		break;
	}

	map[idx]++;
}

/* [한국어] I/O 제출 통계를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @nr: 한 번에 제출된 I/O 수
 */
void io_u_mark_submit(struct thread_data *td, unsigned int nr)
{
	__io_u_mark_map(td->ts.io_u_submit, nr);
	td->ts.total_submit++;
}

/* [한국어] I/O 완료 통계를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @nr: 한 번에 완료된 I/O 수
 */
void io_u_mark_complete(struct thread_data *td, unsigned int nr)
{
	__io_u_mark_map(td->ts.io_u_complete, nr);
	td->ts.total_complete++;
}

/* [한국어] I/O 큐 깊이 통계를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @nr: 제출된 I/O 수
 *
 * 현재 큐 깊이(cur_depth)를 범위별로 분류하여 기록한다.
 * 버킷: 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+
 */
void io_u_mark_depth(struct thread_data *td, unsigned int nr)
{
	int idx = 0;

	switch (td->cur_depth) {
	default:
		idx = 6;	/* [한국어] 64 이상 */
		break;
	case 32 ... 63:
		idx = 5;
		break;
	case 16 ... 31:
		idx = 4;
		break;
	case 8 ... 15:
		idx = 3;
		break;
	case 4 ... 7:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 1:
		break;
	}

	td->ts.io_u_map[idx] += nr;
}

/* [한국어] 나노초 단위 레이턴시를 범위별로 기록하는 함수 (1000ns 미만).
 *
 * @td: 스레드 데이터
 * @nsec: 레이턴시 (나노초)
 *
 * 0-1, 2-3, 4-9, 10-19, 20-49, 50-99, 100-249, 250-499, 500-749, 750-999 ns
 */
static void io_u_mark_lat_nsec(struct thread_data *td, unsigned long long nsec)
{
	int idx = 0;

	assert(nsec < 1000);

	switch (nsec) {
	case 750 ... 999:
		idx = 9;
		break;
	case 500 ... 749:
		idx = 8;
		break;
	case 250 ... 499:
		idx = 7;
		break;
	case 100 ... 249:
		idx = 6;
		break;
	case 50 ... 99:
		idx = 5;
		break;
	case 20 ... 49:
		idx = 4;
		break;
	case 10 ... 19:
		idx = 3;
		break;
	case 4 ... 9:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 0 ... 1:
		break;
	}

	assert(idx < FIO_IO_U_LAT_N_NR);
	td->ts.io_u_lat_n[idx]++;
}

/* [한국어] 마이크로초 단위 레이턴시를 범위별로 기록하는 함수 (1~999 usec).
 *
 * @td: 스레드 데이터
 * @usec: 레이턴시 (마이크로초)
 */
static void io_u_mark_lat_usec(struct thread_data *td, unsigned long long usec)
{
	int idx = 0;

	assert(usec < 1000 && usec >= 1);

	switch (usec) {
	case 750 ... 999:
		idx = 9;
		break;
	case 500 ... 749:
		idx = 8;
		break;
	case 250 ... 499:
		idx = 7;
		break;
	case 100 ... 249:
		idx = 6;
		break;
	case 50 ... 99:
		idx = 5;
		break;
	case 20 ... 49:
		idx = 4;
		break;
	case 10 ... 19:
		idx = 3;
		break;
	case 4 ... 9:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 0 ... 1:
		break;
	}

	assert(idx < FIO_IO_U_LAT_U_NR);
	td->ts.io_u_lat_u[idx]++;
}

/* [한국어] 밀리초 단위 레이턴시를 범위별로 기록하는 함수 (1ms 이상).
 *
 * @td: 스레드 데이터
 * @msec: 레이턴시 (밀리초)
 *
 * 0-1, 2-3, 4-9, 10-19, 20-49, 50-99, 100-249, 250-499,
 * 500-749, 750-999, 1000-1999, 2000+ ms
 */
static void io_u_mark_lat_msec(struct thread_data *td, unsigned long long msec)
{
	int idx = 0;

	assert(msec >= 1);

	switch (msec) {
	default:
		idx = 11;	/* [한국어] 2000ms 이상 */
		break;
	case 1000 ... 1999:
		idx = 10;
		break;
	case 750 ... 999:
		idx = 9;
		break;
	case 500 ... 749:
		idx = 8;
		break;
	case 250 ... 499:
		idx = 7;
		break;
	case 100 ... 249:
		idx = 6;
		break;
	case 50 ... 99:
		idx = 5;
		break;
	case 20 ... 49:
		idx = 4;
		break;
	case 10 ... 19:
		idx = 3;
		break;
	case 4 ... 9:
		idx = 2;
		break;
	case 2 ... 3:
		idx = 1;
		fio_fallthrough;
	case 0 ... 1:
		break;
	}

	assert(idx < FIO_IO_U_LAT_M_NR);
	td->ts.io_u_lat_m[idx]++;
}

/* [한국어] 레이턴시를 적절한 단위(ns/us/ms)로 분류하여 기록하는 디스패처 함수.
 *
 * @td: 스레드 데이터
 * @nsec: 레이턴시 (나노초)
 */
static void io_u_mark_latency(struct thread_data *td, unsigned long long nsec)
{
	if (nsec < 1000)
		/* [한국어] 1000ns 미만 -> 나노초 단위 */
		io_u_mark_lat_nsec(td, nsec);
	else if (nsec < 1000000)
		/* [한국어] 1ms 미만 -> 마이크로초 단위 */
		io_u_mark_lat_usec(td, nsec / 1000);
	else
		/* [한국어] 1ms 이상 -> 밀리초 단위 */
		io_u_mark_lat_msec(td, nsec / 1000000);
}

/* [한국어] 다음 파일 번호를 랜덤으로 선택하는 내부 함수.
 *
 * @td: 스레드 데이터
 * @return: 선택된 파일 번호
 *
 * 파일 서비스 타입(random, zipf, pareto, gauss)에 따라
 * 다음에 I/O할 파일 번호를 랜덤으로 결정한다.
 */
static unsigned int __get_next_fileno_rand(struct thread_data *td)
{
	unsigned long fileno;

	/* [한국어] 기본 랜덤: 균일 분포로 파일 번호 선택 */
	if (td->o.file_service_type == FIO_FSERVICE_RANDOM) {
		uint64_t frand_max = rand_max(&td->next_file_state);
		unsigned long r;

		r = __rand(&td->next_file_state);
		return (unsigned int) ((double) td->o.nr_files
				* (r / (frand_max + 1.0)));
	}

	/* [한국어] Zipf/Pareto/가우시안 분포로 파일 번호 선택 */
	if (td->o.file_service_type == FIO_FSERVICE_ZIPF)
		fileno = zipf_next(&td->next_file_zipf);
	else if (td->o.file_service_type == FIO_FSERVICE_PARETO)
		fileno = pareto_next(&td->next_file_zipf);
	else if (td->o.file_service_type == FIO_FSERVICE_GAUSS)
		fileno = gauss_next(&td->next_file_gauss);
	else {
		log_err("fio: bad file service type: %d\n", td->o.file_service_type);
		assert(0);
		return 0;
	}

	return fileno >> FIO_FSERVICE_SHIFT;
}

/*
 * Get next file to service by choosing one at random
 */
/* [한국어] 랜덤으로 다음 서비스할 파일을 선택하는 함수.
 *
 * @td: 스레드 데이터
 * @goodf: 필수 플래그 (이 플래그가 있어야 선택)
 * @badf: 금지 플래그 (이 플래그가 있으면 건너뜀)
 * @return: 선택된 파일 포인터 또는 에러
 *
 * 파일이 닫혀 있으면 열고, 필요한 플래그 조건을 만족하는 파일을 찾을 때까지 반복.
 */
static struct fio_file *get_next_file_rand(struct thread_data *td,
					   enum fio_file_flags goodf,
					   enum fio_file_flags badf)
{
	struct fio_file *f;
	int fno;

	do {
		int opened = 0;

		/* [한국어] 랜덤 파일 번호 생성 */
		fno = __get_next_fileno_rand(td);

		f = td->files[fno];
		/* [한국어] 이미 완료된 파일이면 건너뜀 */
		if (fio_file_done(f))
			continue;

		/* [한국어] 파일이 아직 열리지 않았으면 열기 시도 */
		if (!fio_file_open(f)) {
			int err;

			/* [한국어] 열린 파일 수 제한 확인 */
			if (td->nr_open_files >= td->o.open_files)
				return ERR_PTR(-EBUSY);

			err = td_io_open_file(td, f);
			if (err)
				continue;
			opened = 1;
		}

		/* [한국어] 플래그 조건 확인: goodf 만족하고 badf 없으면 선택 */
		if ((!goodf || (f->flags & goodf)) && !(f->flags & badf)) {
			dprint(FD_FILE, "get_next_file_rand: %p\n", f);
			return f;
		}
		/* [한국어] 조건 불만족 시 방금 연 파일은 닫기 */
		if (opened)
			td_io_close_file(td, f);
	} while (1);
}

/*
 * Get next file to service by doing round robin between all available ones
 */
/* [한국어] 라운드 로빈 방식으로 다음 서비스할 파일을 선택하는 함수.
 *
 * @td: 스레드 데이터
 * @goodf: 필수 플래그
 * @badf: 금지 플래그
 * @return: 선택된 파일 포인터 또는 NULL
 *
 * next_file 인덱스를 순환하면서 조건에 맞는 파일을 찾는다.
 * 한 바퀴 돌아도 못 찾으면 NULL 반환.
 */
static struct fio_file *get_next_file_rr(struct thread_data *td, int goodf,
					 int badf)
{
	/* [한국어] 시작 위치 기억 (한 바퀴 돌았는지 확인용) */
	unsigned int old_next_file = td->next_file;
	struct fio_file *f;

	do {
		int opened = 0;

		f = td->files[td->next_file];

		/* [한국어] 다음 파일 인덱스 증가 (순환) */
		td->next_file++;
		if (td->next_file >= td->o.nr_files)
			td->next_file = 0;

		dprint(FD_FILE, "trying file %s %x\n", f->file_name, f->flags);
		/* [한국어] 완료된 파일이면 건너뜀 */
		if (fio_file_done(f)) {
			f = NULL;
			continue;
		}

		/* [한국어] 파일이 열리지 않았으면 열기 시도 */
		if (!fio_file_open(f)) {
			int err;

			if (td->nr_open_files >= td->o.open_files)
				return ERR_PTR(-EBUSY);

			err = td_io_open_file(td, f);
			if (err) {
				dprint(FD_FILE, "error %d on open of %s\n",
					err, f->file_name);
				f = NULL;
				continue;
			}
			opened = 1;
		}

		dprint(FD_FILE, "goodf=%x, badf=%x, ff=%x\n", goodf, badf,
								f->flags);
		/* [한국어] 플래그 조건 충족 시 선택 */
		if ((!goodf || (f->flags & goodf)) && !(f->flags & badf))
			break;

		if (opened)
			td_io_close_file(td, f);

		f = NULL;
	} while (td->next_file != old_next_file);
	/* [한국어] 한 바퀴 돌 때까지 반복 */

	dprint(FD_FILE, "get_next_file_rr: %p\n", f);
	return f;
}

/* [한국어] 파일 서비스 정책에 따라 다음 파일을 선택하는 내부 함수.
 *
 * @td: 스레드 데이터
 * @return: 선택된 파일 포인터, NULL(모두 완료), 또는 에러 포인터
 *
 * 파일 서비스 타입에 따라:
 * - SEQ: 순차적으로 한 파일씩
 * - RR: 라운드 로빈
 * - RANDOM/ZIPF/PARETO/GAUSS: 랜덤 선택
 * file_service_left로 같은 파일에 연속 I/O 횟수를 제어.
 */
static struct fio_file *__get_next_file(struct thread_data *td)
{
	struct fio_file *f;

	assert(td->o.nr_files <= td->files_index);

	/* [한국어] 모든 파일이 완료됐으면 NULL 반환 */
	if (td->nr_done_files >= td->o.nr_files) {
		dprint(FD_FILE, "get_next_file: nr_open=%d, nr_done=%d,"
				" nr_files=%d\n", td->nr_open_files,
						  td->nr_done_files,
						  td->o.nr_files);
		return NULL;
	}

	/* [한국어] 현재 서비스 중인 파일이 유효하면 계속 사용 */
	f = td->file_service_file;
	if (f && fio_file_open(f) && !fio_file_closing(f)) {
		/* [한국어] 순차 모드면 파일이 끝날 때까지 계속 */
		if (td->o.file_service_type == FIO_FSERVICE_SEQ)
			goto out;
		/* [한국어] 아직 남은 서비스 횟수가 있으면 계속 */
		if (td->file_service_left) {
			td->file_service_left--;
			goto out;
		}
	}

	/* [한국어] 라운드 로빈/순차이면 RR 방식, 아니면 랜덤 방식 */
	if (td->o.file_service_type == FIO_FSERVICE_RR ||
	    td->o.file_service_type == FIO_FSERVICE_SEQ)
		f = get_next_file_rr(td, FIO_FILE_open, FIO_FILE_closing);
	else
		f = get_next_file_rand(td, FIO_FILE_open, FIO_FILE_closing);

	if (IS_ERR(f))
		return f;

	/* [한국어] 새 파일을 현재 서비스 파일로 설정 */
	td->file_service_file = f;
	/* [한국어] 이 파일에 연속으로 서비스할 횟수 설정 */
	td->file_service_left = td->file_service_nr - 1;
out:
	if (f)
		dprint(FD_FILE, "get_next_file: %p [%s]\n", f, f->file_name);
	else
		dprint(FD_FILE, "get_next_file: NULL\n");
	return f;
}

/* [한국어] 다음 서비스할 파일을 선택하는 래퍼 함수.
 *
 * @td: 스레드 데이터
 * @return: 선택된 파일 포인터
 */
static struct fio_file *get_next_file(struct thread_data *td)
{
	return __get_next_file(td);
}

/* [한국어] io_u에 파일을 설정하고 I/O 정보를 채우는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @return: 0=성공, 양수=실패, 음수=에러(-EBUSY 등)
 *
 * 파일을 가져오고 fill_io_u()로 I/O 정보를 채운다.
 * 실패하면 파일을 닫고 다음 파일을 시도한다.
 * 비균일 파일 서비스이면 파일을 리셋하고, 아니면 완료 처리한다.
 */
static long set_io_u_file(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f;

	do {
		/* [한국어] 다음 서비스할 파일 가져오기 */
		f = get_next_file(td);
		if (IS_ERR_OR_NULL(f))
			return PTR_ERR(f);

		/* [한국어] io_u에 파일 설정 및 참조 카운트 증가 */
		io_u->file = f;
		get_file(f);

		/* [한국어] fill_io_u()로 I/O 정보 채우기 성공 시 루프 종료 */
		if (!fill_io_u(td, io_u))
			break;

		/* [한국어] 실패 시 정리: ZBD 해제, 파일 참조 해제 및 닫기 */
		zbd_put_io_u(td, io_u);

		put_file_log(td, f);
		td_io_close_file(td, f);
		io_u->file = NULL;

		/* [한국어] 타임아웃이면 즉시 반환 */
		if (io_u->ddir == DDIR_TIMEOUT)
			return 1;

		/* [한국어] 비균일 서비스이면 파일 리셋, 아니면 완료 처리 */
		if (td->o.file_service_type & __FIO_FSERVICE_NONUNIFORM)
			fio_file_reset(td, f);
		else {
			fio_file_set_done(f);
			td->nr_done_files++;
			dprint(FD_FILE, "%s: is done (%d of %d)\n", f->file_name,
					td->nr_done_files, td->o.nr_files);
		}
	} while (1);

	return 0;
}

/* [한국어] 최대 레이턴시 초과 시 치명적 에러를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: 해당 I/O 유닛
 * @icd: I/O 완료 데이터
 * @tnsec: 실제 레이턴시 (나노초)
 * @max_nsec: 허용 최대 레이턴시 (나노초)
 */
static void lat_fatal(struct thread_data *td, struct io_u *io_u, struct io_completion_data *icd,
		      unsigned long long tnsec, unsigned long long max_nsec)
{
	if (!td->error) {
		log_err("fio: latency of %llu nsec exceeds specified max (%llu nsec): %s %s %llu %llu\n",
					tnsec, max_nsec,
					io_u->file->file_name,
					io_ddir_name(io_u->ddir),
					io_u->offset, io_u->buflen);
	}
	td_verror(td, ETIMEDOUT, "max latency exceeded");
	icd->error = ETIMEDOUT;
}

/* [한국어] 레이턴시 타겟의 새 측정 사이클을 시작하는 함수.
 *
 * @td: 스레드 데이터
 *
 * 현재 시각과 I/O 블록 수를 기록하여 새 측정 시작점으로 삼는다.
 */
static void lat_new_cycle(struct thread_data *td)
{
	fio_gettime(&td->latency_ts, NULL);
	td->latency_ios = ddir_rw_sum(td->io_blocks);
	td->latency_failed = 0;
}

/*
 * We had an IO outside the latency target. Reduce the queue depth. If we
 * are at QD=1, then it's time to give up.
 */
/* [한국어] 레이턴시 타겟 초과 시 큐 깊이를 줄이는 내부 함수.
 *
 * @td: 스레드 데이터
 * @return: true=QD=1에서도 실패(포기), false=QD 줄임
 *
 * 이진 탐색 방식으로 큐 깊이를 줄인다.
 * QD를 줄인 후 기존 I/O를 모두 완료(quiesce)시키고 새 사이클 시작.
 */
static bool __lat_target_failed(struct thread_data *td)
{
	/* [한국어] QD=1이면 더 줄일 수 없으므로 포기 */
	if (td->latency_qd == 1)
		return true;

	/* [한국어] 현재 QD를 상한으로 설정 */
	td->latency_qd_high = td->latency_qd;

	if (td->latency_qd == td->latency_qd_low)
		td->latency_qd_low--;

	/* [한국어] 이진 탐색: (현재 + 하한) / 2 */
	td->latency_qd = (td->latency_qd + td->latency_qd_low) / 2;
	td->latency_stable_count = 0;

	dprint(FD_RATE, "Ramped down: %d %d %d\n", td->latency_qd_low, td->latency_qd, td->latency_qd_high);

	/*
	 * When we ramp QD down, quiesce existing IO to prevent
	 * a storm of ramp downs due to pending higher depth.
	 */
	/* [한국어] QD를 줄인 후 진행 중인 I/O를 모두 완료시킴 */
	io_u_quiesce(td);
	lat_new_cycle(td);
	return false;
}

/* [한국어] 레이턴시 타겟 초과 시 처리하는 외부 함수.
 *
 * @td: 스레드 데이터
 * @return: true=포기(QD=1에서도 실패), false=계속
 *
 * percentile이 100%이면 즉시 __lat_target_failed() 호출.
 * 그렇지 않으면 실패 횟수만 증가시키고 나중에 판단.
 */
static bool lat_target_failed(struct thread_data *td)
{
	if (td->o.latency_percentile.u.f == 100.0)
		return __lat_target_failed(td);

	/* [한국어] 실패 횟수 증가 (사이클 끝에서 percentile로 판단) */
	td->latency_failed++;
	return false;
}

/* [한국어] 레이턴시 타겟을 초기화하는 함수.
 *
 * @td: 스레드 데이터
 *
 * latency_target이 설정되어 있으면 QD=1부터 시작하여
 * 이진 탐색으로 최적 QD를 찾아간다.
 */
void lat_target_init(struct thread_data *td)
{
	td->latency_end_run = 0;

	if (td->o.latency_target) {
		dprint(FD_RATE, "Latency target=%llu\n", td->o.latency_target);
		fio_gettime(&td->latency_ts, NULL);
		/* [한국어] QD=1부터 시작 */
		td->latency_qd = 1;
		/* [한국어] 상한은 사용자 지정 iodepth */
		td->latency_qd_high = td->o.iodepth;
		td->latency_qd_low = 1;
		td->latency_ios = ddir_rw_sum(td->io_blocks);
	} else
		/* [한국어] 타겟 없으면 전체 iodepth 사용 */
		td->latency_qd = td->o.iodepth;
}

/* [한국어] 레이턴시 타겟을 리셋하는 함수.
 *
 * @td: 스레드 데이터
 *
 * 실행이 아직 끝나지 않았으면 lat_target_init()으로 재초기화.
 */
void lat_target_reset(struct thread_data *td)
{
	if (!td->latency_end_run)
		lat_target_init(td);
}

/* [한국어] 레이턴시 타겟을 만족했을 때 큐 깊이를 올리는 함수.
 *
 * @td: 스레드 데이터
 *
 * 이진 탐색으로 QD를 올린다. 아직 실패 상한을 모르면 2배로,
 * 상한이 알려져 있으면 (현재 + 상한) / 2로 올린다.
 * 안정 상태에서 3회 연속 성공하면 상한을 1 올린다.
 * 최적 QD를 찾으면 작업을 종료한다.
 */
static void lat_target_success(struct thread_data *td)
{
	const unsigned int qd = td->latency_qd;
	struct thread_options *o = &td->o;

	/* [한국어] 현재 QD를 하한으로 설정 */
	td->latency_qd_low = td->latency_qd;

	if (td->latency_qd + 1 == td->latency_qd_high) {
		/*
		 * latency_qd will not incease on lat_target_success(), so
		 * called stable. If we stick with this queue depth, the
		 * final latency is likely lower than latency_target. Fix
		 * this by increasing latency_qd_high slowly. Use a naive
		 * heuristic here. If we get lat_target_success() 3 times
		 * in a row, increase latency_qd_high by 1.
		 */
		/* [한국어] 안정 상태: 3회 연속 성공 시 상한을 1 올림 */
		if (++td->latency_stable_count >= 3) {
			td->latency_qd_high++;
			td->latency_stable_count = 0;
		}
	}

	/*
	 * If we haven't failed yet, we double up to a failing value instead
	 * of bisecting from highest possible queue depth. If we have set
	 * a limit other than td->o.iodepth, bisect between that.
	 */
	/* [한국어] 아직 실패 경험 없으면 2배로, 있으면 이진 탐색 */
	if (td->latency_qd_high != o->iodepth)
		td->latency_qd = (td->latency_qd + td->latency_qd_high) / 2;
	else
		td->latency_qd *= 2;

	/* [한국어] iodepth 상한 제한 */
	if (td->latency_qd > o->iodepth)
		td->latency_qd = o->iodepth;

	dprint(FD_RATE, "Ramped up: %d %d %d\n", td->latency_qd_low, td->latency_qd, td->latency_qd_high);

	/*
	 * Same as last one, we are done. Let it run a latency cycle, so
	 * we get only the results from the targeted depth.
	 */
	/* [한국어] QD가 변하지 않으면 최적값을 찾은 것 */
	if (!o->latency_run && td->latency_qd == qd) {
		if (td->latency_end_run) {
			/* [한국어] 최종 실행도 완료됨 -> 작업 종료 */
			dprint(FD_RATE, "We are done\n");
			td->done = 1;
		} else {
			/* [한국어] 최적 QD에서 최종 측정 실행 시작 */
			dprint(FD_RATE, "Quiesce and final run\n");
			io_u_quiesce(td);
			td->latency_end_run = 1;
			reset_all_stats(td);
			reset_io_stats(td);
		}
	}

	lat_new_cycle(td);
}

/*
 * Check if we can bump the queue depth
 */
/* [한국어] 레이턴시 타겟 체크: 측정 윈도우가 끝났는지 확인하고 QD 조정.
 *
 * @td: 스레드 데이터
 *
 * latency_window 시간이 경과하면 성공률을 계산하고,
 * latency_percentile 이상이면 QD를 올리고,
 * 미만이면 QD를 내린다.
 */
void lat_target_check(struct thread_data *td)
{
	uint64_t usec_window;
	uint64_t ios;
	double success_ios;

	/* [한국어] 측정 윈도우 경과 시간 확인 */
	usec_window = utime_since_now(&td->latency_ts);
	if (usec_window < td->o.latency_window)
		return;

	/* [한국어] 이 윈도우 동안의 I/O 수와 성공률 계산 */
	ios = ddir_rw_sum(td->io_blocks) - td->latency_ios;
	success_ios = (double) (ios - td->latency_failed) / (double) ios;
	success_ios *= 100.0;

	dprint(FD_RATE, "Success rate: %.2f%% (target %.2f%%)\n", success_ios, td->o.latency_percentile.u.f);

	/* [한국어] 성공률이 목표 이상이면 QD 올림, 미만이면 내림 */
	if (success_ios >= td->o.latency_percentile.u.f)
		lat_target_success(td);
	else
		__lat_target_failed(td);
}

/*
 * If latency target is enabled, we might be ramping up or down and not
 * using the full queue depth available.
 */
/* [한국어] I/O 큐가 가득 찼는지 확인하는 함수.
 *
 * @td: 스레드 데이터
 * @return: true=큐가 가득 참, false=아직 여유 있음
 *
 * 프리리스트가 비었거나, 레이턴시 타겟에 의해
 * 현재 깊이가 목표 QD에 도달했으면 가득 찬 것으로 판단.
 */
bool queue_full(const struct thread_data *td)
{
	/* [한국어] 프리리스트가 비었으면 가득 참 */
	const int qempty = io_u_qempty(&td->io_u_freelist);

	if (qempty)
		return true;
	/* [한국어] 레이턴시 타겟이 없으면 아직 여유 있음 */
	if (!td->o.latency_target)
		return false;

	/* [한국어] 현재 깊이가 레이턴시 목표 QD에 도달했으면 가득 참 */
	return td->cur_depth >= td->latency_qd;
}

/* [한국어] 프리리스트 또는 재큐잉 리스트에서 io_u를 가져오는 내부 함수.
 *
 * @td: 스레드 데이터
 * @return: 가져온 io_u 포인터 또는 NULL
 *
 * I/O 유닛 할당의 핵심 함수. 다음 순서로 io_u를 가져온다:
 * 1. 재큐잉 리스트(requeues)에 있으면 거기서 가져옴 (재시도 우선)
 * 2. 큐가 가득 차지 않았으면 프리리스트에서 가져옴
 * 3. 비동기 처리 시 둘 다 없으면 대기(cond_wait)
 *
 * 가져온 io_u의 플래그를 초기화하고 cur_depth를 증가시킨다.
 */
struct io_u *__get_io_u(struct thread_data *td)
{
	const bool needs_lock = td_async_processing(td);
	struct io_u *io_u = NULL;

	/* [한국어] 정지 상태이면 NULL 반환 */
	if (td->stop_io)
		return NULL;

	if (needs_lock)
		__td_io_u_lock(td);

again:
	/* [한국어] 1순위: 재큐잉 리스트에서 꺼내기 */
	if (!io_u_rempty(&td->io_u_requeues)) {
		io_u = io_u_rpop(&td->io_u_requeues);
		io_u->resid = 0;
		/* [한국어] fsync 중이면 파일 참조 해제 */
		if (io_u->file && td->runstate == TD_FSYNCING) {
			put_file_log(td, io_u->file);
			io_u->file = NULL;
		}
	} else if (!queue_full(td)) {
		/* [한국어] 2순위: 큐에 여유가 있으면 프리리스트에서 꺼내기 */
		io_u = io_u_qpop(&td->io_u_freelist);

		/* [한국어] 새로 꺼낸 io_u 초기화 */
		io_u->file = NULL;
		io_u->buflen = 0;
		io_u->resid = 0;
		io_u->end_io = NULL;
	}

	if (io_u) {
		/* [한국어] FREE 상태였는지 검증 */
		assert(io_u->flags & IO_U_F_FREE);
		/* [한국어] 각종 플래그 초기화 */
		io_u_clear(td, io_u, IO_U_F_FREE | IO_U_F_NO_FILE_PUT |
				 IO_U_F_TRIMMED | IO_U_F_BARRIER |
				 IO_U_F_VER_LIST);

		io_u->error = 0;
		io_u->acct_ddir = -1;
		/* [한국어] 현재 깊이 증가 */
		td->cur_depth++;
		assert(!(td->flags & TD_F_CHILD));
		io_u_set(td, io_u, IO_U_F_IN_CUR_DEPTH);
		io_u->ipo = NULL;
	} else if (td_async_processing(td)) {
		int ret;
		/*
		 * We ran out, wait for async verify threads to finish and
		 * return one
		 */
		/* [한국어] 프리리스트가 비었으면 비동기 검증 스레드 완료 대기 */
		assert(!(td->flags & TD_F_CHILD));
		ret = pthread_cond_wait(&td->free_cond, &td->io_u_lock);
		if (fio_unlikely(ret != 0)) {
			td->error = errno;
		} else if (!td->error)
			goto again;
	}

	if (needs_lock)
		__td_io_u_unlock(td);

	return io_u;
}

/* [한국어] trim 백로그에서 다음 trim을 가져올지 확인하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @return: true=trim을 가져옴, false=trim 없음
 *
 * trim_backlog이 설정되어 있으면, 일정 주기마다
 * 이전에 기록된 위치를 trim하는 I/O를 발행한다.
 * trim_batch로 연속 trim 횟수를 제어한다.
 */
static bool check_get_trim(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] trim 백로그 플래그가 없으면 패스 */
	if (!(td->flags & TD_F_TRIM_BACKLOG))
		return false;
	/* [한국어] trim할 엔트리가 없으면 패스 */
	if (!td->trim_entries) {
		td->trim_batch = 0;
		return false;
	}

	/* [한국어] trim_batch가 남아 있으면 계속 trim */
	if (td->trim_batch) {
		td->trim_batch--;
		if (get_next_trim(td, io_u))
			return true;
		else
			td->trim_batch = 0;
	} else if (!(td->io_hist_len % td->o.trim_backlog) &&
		     td->last_ddir_completed != DDIR_TRIM) {
		/* [한국어] trim_backlog 주기에 도달하고 마지막이 trim이 아니면 trim 시작 */
		if (get_next_trim(td, io_u)) {
			td->trim_batch = td->o.trim_batch;
			if (!td->trim_batch)
				td->trim_batch = td->o.trim_backlog;
			td->trim_batch--;
			return true;
		}
	}

	return false;
}

/* [한국어] 검증 백로그에서 다음 검증을 가져올지 확인하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @return: true=검증 I/O를 가져옴, false=없음
 *
 * verify_backlog이 설정되어 있으면, 일정 주기마다
 * 이전에 쓴 데이터를 읽어서 검증하는 I/O를 발행한다.
 */
static bool check_get_verify(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 검증 백로그 플래그가 없으면 패스 */
	if (!(td->flags & TD_F_VER_BACKLOG))
		return false;

	if (td->io_hist_len) {
		int get_verify = 0;

		/* [한국어] verify_batch가 남아 있으면 계속 검증 */
		if (td->verify_batch)
			get_verify = 1;
		else if (!(td->io_hist_len % td->o.verify_backlog) &&
			 td->last_ddir_completed != DDIR_READ) {
			/* [한국어] verify_backlog 주기에 도달하면 검증 시작 */
			td->verify_batch = td->o.verify_batch;
			if (!td->verify_batch)
				td->verify_batch = td->o.verify_backlog;
			get_verify = 1;
		}

		if (get_verify && !get_next_verify(td, io_u)) {
			td->verify_batch--;
			return true;
		}
	}

	return false;
}

/*
 * Fill offset and start time into the buffer content, to prevent too
 * easy compressible data for simple de-dupe attempts. Do this for every
 * 512b block in the range, since that should be the smallest block size
 * we can expect from a device.
 */
/* [한국어] 버퍼 내용에 오프셋과 시간을 삽입하여 중복 제거를 방지하는 함수.
 *
 * @io_u: I/O 유닛
 *
 * 512바이트 블록 단위로 오프셋과 시작 시간을 버퍼에 삽입한다.
 * 이렇게 하면 같은 패턴 데이터라도 각 블록이 고유해져서
 * 단순한 중복 제거(dedupe) 시도를 방지한다.
 * 캐시라인(64바이트) 내에서 랜덤 위치에 데이터를 삽입하여 성능을 높인다.
 */
static void small_content_scramble(struct io_u *io_u)
{
	/* [한국어] 512바이트 블록 수 계산 */
	unsigned long long i, nr_blocks = io_u->buflen >> 9;
	unsigned int offset;
	uint64_t boffset, *iptr;
	char *p;

	if (!nr_blocks)
		return;

	p = io_u->xfer_buf;
	boffset = io_u->offset;

	/* [한국어] 이전에 채워진 버퍼 정보 초기화 */
	if (io_u->buf_filled_len)
		io_u->buf_filled_len = 0;

	/*
	 * Generate random index between 0..7. We do chunks of 512b, if
	 * we assume a cacheline is 64 bytes, then we have 8 of those.
	 * Scramble content within the blocks in the same cacheline to
	 * speed things up.
	 */
	/* [한국어] 캐시라인 내 삽입 위치 결정 (0~7번 캐시라인 중 하나) */
	offset = (io_u->start_time.tv_nsec ^ boffset) & 7;

	for (i = 0; i < nr_blocks; i++) {
		/*
		 * Fill offset into start of cacheline, time into end
		 * of cacheline
		 */
		/* [한국어] 캐시라인 시작에 오프셋 삽입 */
		iptr = (void *) p + (offset << 6);
		*iptr = boffset;

		/* [한국어] 캐시라인 끝에 시간 삽입 */
		iptr = (void *) p + 64 - 2 * sizeof(uint64_t);
		iptr[0] = io_u->start_time.tv_sec;
		iptr[1] = io_u->start_time.tv_nsec;

		p += 512;
		boffset += 512;
	}
}

/*
 * Return an io_u to be processed. Gets a buflen and offset, sets direction,
 * etc. The returned io_u is fully ready to be prepped, populated and submitted.
 */
/* [한국어] ★ 핵심 함수 ★ 처리할 io_u를 가져오는 최상위 함수.
 *
 * @td: 스레드 데이터
 * @return: 준비된 io_u 포인터, NULL(사용 가능한 io_u 없음), 에러 포인터
 *
 * io_u의 전체 생명주기에서 "할당+설정" 단계를 담당한다.
 * 반환된 io_u는 다음이 모두 설정되어 즉시 제출할 수 있는 상태:
 *   - ddir (데이터 방향: read/write/trim/sync)
 *   - offset (I/O 시작 위치)
 *   - buflen (I/O 크기)
 *   - file (대상 파일)
 *   - buf/xfer_buf (데이터 버퍼)
 *   - ioprio (I/O 우선순위)
 *   - start_time (시작 시각)
 *
 * 처리 순서:
 * 1. __get_io_u()로 프리리스트/재큐잉에서 io_u 할당
 * 2. 검증(verify) 백로그 확인 -> 검증 I/O면 바로 out
 * 3. trim 백로그 확인 -> trim I/O면 바로 out
 * 4. 재큐잉에서 온 io_u면 파일이 이미 설정되어 있으므로 바로 out
 * 5. iolog 사용 시 로그에서 다음 I/O 정보 읽기
 * 6. 아니면 set_io_u_file()로 파일 선택 및 I/O 정보 설정
 * 7. 쓰기의 경우 버퍼 리필/스크램블 처리
 * 8. xfer_buf/xfer_buflen 설정
 * 9. td_io_prep()으로 엔진별 사전 준비
 * 10. start_time 기록
 */
struct io_u *get_io_u(struct thread_data *td)
{
	struct fio_file *f;
	struct io_u *io_u;
	int do_scramble = 0;
	long ret = 0;

	/* [한국어] 1단계: io_u 할당 (프리리스트 또는 재큐잉에서) */
	io_u = __get_io_u(td);
	if (!io_u) {
		dprint(FD_IO, "__get_io_u failed\n");
		return NULL;
	}

	/* [한국어] 2단계: 검증 백로그에서 검증 I/O가 있으면 가져옴 */
	if (check_get_verify(td, io_u))
		goto out;
	/* [한국어] 3단계: trim 백로그에서 trim I/O가 있으면 가져옴 */
	if (check_get_trim(td, io_u))
		goto out;

	/*
	 * from a requeue, io_u already setup
	 */
	/* [한국어] 4단계: 재큐잉에서 온 io_u면 이미 설정 완료 */
	if (io_u->file)
		goto out;

	/*
	 * If using an iolog, grab next piece if any available.
	 */
	/* [한국어] 5단계: iolog 사용 시 로그에서 다음 I/O 정보 읽기 */
	if (td->flags & TD_F_READ_IOLOG) {
		if (read_iolog_get(td, io_u))
			goto err_put;
	} else if (set_io_u_file(td, io_u)) {
		/* [한국어] 6단계: 파일 선택 및 I/O 정보 채우기 */
		ret = -EBUSY;
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	f = io_u->file;
	if (!f) {
		dprint(FD_IO, "io_u %p, setting file failed\n", io_u);
		goto err_put;
	}

	assert(fio_file_open(f));

	/* [한국어] 7단계: 읽기/쓰기 방향이고 다중 범위 trim이 아닌 경우 */
	if (ddir_rw(io_u->ddir) && !multi_range_trim(td, io_u)) {
		/* [한국어] buflen이 0이면 에러 (NOIO 엔진 제외) */
		if (!io_u->buflen && !td_ioengine_flagged(td, FIO_NOIO)) {
			dprint(FD_IO, "get_io_u: zero buflen on %p\n", io_u);
			goto err_put;
		}

		/* [한국어] 파일의 마지막 시작/위치 업데이트 */
		f->last_start[io_u->ddir] = io_u->offset;
		f->last_pos[io_u->ddir] = io_u->offset + io_u->buflen;

		if (io_u->ddir == DDIR_WRITE) {
			/* [한국어] 쓰기: 버퍼 리필이 필요하면 새 데이터로 채움 */
			if (td->flags & TD_F_REFILL_BUFFERS) {
				io_u_fill_buffer(td, io_u,
					td->o.min_bs[DDIR_WRITE],
					io_u->buflen);
			} else if ((td->flags & TD_F_SCRAMBLE_BUFFERS) &&
				   !(td->flags & TD_F_COMPRESS) &&
				   !(td->flags & TD_F_DO_VERIFY)) {
				/* [한국어] 스크램블 대상이면 나중에 처리 (시간 기록 후) */
				do_scramble = 1;
			}
		} else if (io_u->ddir == DDIR_READ) {
			/*
			 * Reset the buf_filled parameters so next time if the
			 * buffer is used for writes it is refilled.
			 */
			/* [한국어] 읽기: buf_filled_len 초기화 (다음 쓰기 시 리필 필요) */
			io_u->buf_filled_len = 0;
		}
	}

	/*
	 * Set io data pointers.
	 */
	/* [한국어] 8단계: 전송 버퍼 포인터 설정 */
	io_u->xfer_buf = io_u->buf;
	io_u->xfer_buflen = io_u->buflen;

	/*
	 * Remember the issuing context priority. The IO engine may change this.
	 */
	/* [한국어] I/O 우선순위 설정 */
	io_u->ioprio = td->ioprio;
	io_u->clat_prio_index = 0;
out:
	assert(io_u->file);
	/* [한국어] 9단계: 엔진별 사전 준비 (td_io_prep) */
	if (!td_io_prep(td, io_u)) {
		/* [한국어] 10단계: 레이턴시 측정을 위한 시작 시각 기록 */
		if (!td->o.disable_lat)
			fio_gettime(&io_u->start_time, NULL);

		/* [한국어] 버퍼 스크램블 처리 (시작 시각이 필요하므로 여기서 수행) */
		if (do_scramble)
			small_content_scramble(io_u);

		return io_u;
	}
err_put:
	/* [한국어] 실패 시 io_u를 프리리스트로 반환 */
	dprint(FD_IO, "get_io_u failed\n");
	put_io_u(td, io_u);
	return ERR_PTR(ret);
}

/* [한국어] I/O 에러를 로그에 기록하는 내부 함수.
 *
 * @td: 스레드 데이터
 * @io_u: 에러가 발생한 I/O 유닛
 *
 * 비치명적 에러(무시 가능한 에러)는 error_dump 옵션이 있을 때만 로그에 기록.
 * 치명적 에러는 td_verror()로 스레드 에러 상태를 설정.
 * 디바이스 에러이면 별도 메시지 표시.
 */
static void __io_u_log_error(struct thread_data *td, struct io_u *io_u)
{
	/* [한국어] 에러 유형 판별 */
	enum error_type_bit eb = td_error_type(io_u->ddir, io_u->error);
	bool non_fatal_error = td_non_fatal_error(td, eb, io_u->error);

	/*
	 * Non-fatal errors (errors that should be ignored), are normally not
	 * dumped to the log, unless td->o.error_dump. Regardless, non-fatal
	 * errors should never call td_verror() to set td->error.
	 */
	/* [한국어] 비치명적 에러이고 error_dump가 꺼져 있으면 로그 생략 */
	if (non_fatal_error && !td->o.error_dump)
		return;

	log_err("fio: io_u error%s%s: %s: %s offset=%llu, buflen=%llu\n",
		io_u->file ? " on file " : "",
		io_u->file ? io_u->file->file_name : "",
		(io_u->flags & IO_U_F_DEVICE_ERROR) ?
			"Device-specific error" : strerror(io_u->error),
		io_ddir_name(io_u->ddir),
		io_u->offset, io_u->xfer_buflen);

	/* [한국어] ZBD 관련 에러 로그 */
	zbd_log_err(td, io_u);

	/* [한국어] 엔진별 상세 에러 정보 출력 */
	if (td->io_ops->errdetails) {
		char *err = td->io_ops->errdetails(td, io_u);

		if (err) {
			log_err("fio: %s\n", err);
			free(err);
		}
	}

	/* [한국어] 치명적 에러이면 스레드 에러 상태 설정 */
	if (!td->error && !non_fatal_error)
		td_verror(td, io_u->error, "io_u error");
}

/* [한국어] I/O 에러를 로그에 기록하는 공개 함수 (부모 스레드에도 기록).
 *
 * @td: 스레드 데이터
 * @io_u: 에러가 발생한 I/O 유닛
 */
void io_u_log_error(struct thread_data *td, struct io_u *io_u)
{
	__io_u_log_error(td, io_u);
	/* [한국어] 자식 스레드면 부모에도 에러 기록 */
	if (td->parent)
		__io_u_log_error(td->parent, io_u);
}

/* [한국어] gtod(gettimeofday) 호출을 줄일 수 있는지 확인하는 인라인 함수.
 *
 * @td: 스레드 데이터
 * @return: true=시간 측정 최소화 가능, false=일부 측정 필요
 *
 * clat, slat, bw 모두 비활성화되었거나 gtod_reduce가 설정되면 true.
 */
static inline bool gtod_reduce(struct thread_data *td)
{
	return (td->o.disable_clat && td->o.disable_slat && td->o.disable_bw)
			|| td->o.gtod_reduce;
}

/* [한국어] trim된 블록 정보를 업데이트하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 *
 * 블록 정보 맵에서 해당 블록의 상태를 TRIMMED로 변경하고
 * trim 횟수를 증가시킨다.
 */
static void trim_block_info(struct thread_data *td, struct io_u *io_u)
{
	uint32_t *info = io_u_block_info(td, io_u);

	/* [한국어] 이미 trim 실패 이상 상태이면 건너뜀 */
	if (BLOCK_INFO_STATE(*info) >= BLOCK_STATE_TRIM_FAILURE)
		return;

	*info = BLOCK_INFO(BLOCK_STATE_TRIMMED, BLOCK_INFO_TRIMS(*info) + 1);
}

/* [한국어] I/O 완료 시 레이턴시/대역폭/IOPS 통계를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: 완료된 I/O 유닛
 * @icd: I/O 완료 데이터
 * @idx: 데이터 방향 인덱스
 * @bytes: 완료된 바이트 수
 *
 * 다음 통계를 기록한다:
 * - lat (총 레이턴시): start_time ~ completion_time
 * - clat (완료 레이턴시): issue_time ~ completion_time
 * - bw (대역폭): 단위 시간당 전송량
 * - iops (초당 I/O 횟수)
 * - max_latency 초과 시 치명적 에러 처리
 */
static void account_io_completion(struct thread_data *td, struct io_u *io_u,
				  struct io_completion_data *icd,
				  const enum fio_ddir idx, unsigned int bytes)
{
	/* [한국어] gtod 축소 여부: 축소하지 않을 때만 clat 계산 */
	const int no_reduce = !gtod_reduce(td);
	unsigned long long llnsec = 0;

	/* [한국어] 자식 스레드면 부모의 통계에 기록 */
	if (td->parent)
		td = td->parent;

	/* [한국어] 통계 비활성화이면 건너뜀 */
	if (!td->o.stats || td_ioengine_flagged(td, FIO_NOSTATS))
		return;

	/* [한국어] clat 계산: issue_time ~ completion_time */
	if (no_reduce)
		llnsec = ntime_since(&io_u->issue_time, &icd->time);

	/* [한국어] lat(총 레이턴시) 계산 및 기록: start_time ~ completion_time */
	if (!td->o.disable_lat) {
		unsigned long long tnsec;

		tnsec = ntime_since(&io_u->start_time, &icd->time);
		add_lat_sample(td, idx, tnsec, bytes, io_u);

		/* [한국어] 프로파일 연산자의 레이턴시 콜백 호출 */
		if (td->flags & TD_F_PROFILE_OPS) {
			struct prof_io_ops *ops = &td->prof_io_ops;

			if (ops->io_u_lat)
				icd->error = ops->io_u_lat(td, tnsec);
		}

		/* [한국어] read/write에 대해 최대 레이턴시 및 타겟 검사 */
		if (ddir_rw(idx)) {
			/* [한국어] max_latency 초과 시 치명적 에러 */
			if (td->o.max_latency[idx] && tnsec > td->o.max_latency[idx])
				lat_fatal(td, io_u, icd, tnsec, td->o.max_latency[idx]);
			/* [한국어] latency_target 초과 시 처리 */
			if (td->o.latency_target && tnsec > td->o.latency_target) {
				if (lat_target_failed(td))
					lat_fatal(td, io_u, icd, tnsec, td->o.latency_target);
			}
		}
	}

	if (ddir_rw(idx)) {
		/* [한국어] clat(완료 레이턴시) 기록 및 레이턴시 분포 업데이트 */
		if (!td->o.disable_clat) {
			add_clat_sample(td, idx, llnsec, bytes, io_u);
			io_u_mark_latency(td, llnsec);
		}

		/* [한국어] 대역폭(bw) 샘플 기록 */
		if (!td->o.disable_bw && per_unit_log(td->bw_log))
			add_bw_sample(td, io_u, bytes, llnsec);

		/* [한국어] IOPS 샘플 기록 */
		if (no_reduce && per_unit_log(td->iops_log))
			add_iops_sample(td, io_u, bytes);
	} else if (ddir_sync(idx) && !td->o.disable_clat)
		/* [한국어] sync 방향의 clat 기록 */
		add_sync_clat_sample(&td->ts, llnsec);

	/* [한국어] trim 블록 정보 업데이트 */
	if (td->ts.nr_block_infos && io_u->ddir == DDIR_TRIM)
		trim_block_info(td, io_u);
}

/* [한국어] 쓰기 완료 시 파일의 첫/마지막 쓰기 위치를 기록하는 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @offset: 쓰기 오프셋
 * @bytes: 쓴 바이트 수
 *
 * sync_file_range를 위해 dirty 범위를 추적한다.
 */
static void file_log_write_comp(const struct thread_data *td, struct fio_file *f,
				uint64_t offset, unsigned int bytes)
{
	if (!f)
		return;

	/* [한국어] 첫 쓰기 위치 업데이트 */
	if (f->first_write == -1ULL || offset < f->first_write)
		f->first_write = offset;
	/* [한국어] 마지막 쓰기 위치 업데이트 */
	if (f->last_write == -1ULL || ((offset + bytes) > f->last_write))
		f->last_write = offset + bytes;
}

/* [한국어] I/O 통계를 기록해야 하는지 확인하는 함수.
 *
 * @td: 스레드 데이터
 * @return: true=기록해야 함, false=아직 ramp 기간 또는 비활성 상태
 *
 * ramp 기간이 끝나고 실행/검증 상태일 때만 통계를 기록한다.
 */
static bool should_account(struct thread_data *td)
{
	return ramp_period_over(td) && (td->runstate == TD_RUNNING ||
					   td->runstate == TD_VERIFYING);
}

/* [한국어] 단일 I/O 완료를 처리하는 핵심 내부 함수.
 *
 * @td: 스레드 데이터
 * @io_u_ptr: I/O 유닛 이중 포인터 (재큐잉 시 NULL로 설정될 수 있음)
 * @icd: I/O 완료 데이터
 *
 * I/O 완료 처리의 전체 흐름:
 * 1. FLIGHT 플래그 제거 및 inflight 캐시 무효화
 * 2. ZBD 쓰기 에러 복구 처리
 * 3. 검증(verify) 데이터 상태 업데이트
 * 4. sync 방향이면 통계 기록 후 반환
 * 5. read/write 완료:
 *    - short I/O(부분 완료) 감지 및 재큐잉
 *    - io_blocks, io_bytes 카운터 업데이트
 *    - 통계 기록
 *    - end_io 콜백 호출
 * 6. 에러 처리: 비치명적이면 에러 카운트 증가 후 계속
 */
static void io_completed(struct thread_data *td, struct io_u **io_u_ptr,
			 struct io_completion_data *icd)
{
	struct io_u *io_u = *io_u_ptr;
	enum fio_ddir ddir = io_u->ddir;
	struct fio_file *f = io_u->file;

	dprint_io_u(io_u, "complete");

	/* [한국어] FLIGHT 상태였는지 검증 후 플래그 제거 */
	assert(io_u->flags & IO_U_F_FLIGHT);
	io_u_clear_inflight_flags(td, io_u);
	/* [한국어] inflight 캐시 무효화 (중복 I/O 방지) */
	invalidate_inflight(td, io_u);

	/* [한국어] ZBD 모드에서 쓰기 에러 복구 */
	if (td->o.zone_mode == ZONE_MODE_ZBD && td->o.recover_zbd_write_error &&
	    io_u->error && io_u->ddir == DDIR_WRITE &&
	    !td_ioengine_flagged(td, FIO_SYNCIO))
		zbd_recover_write_error(td, io_u);

	/*
	 * Mark IO ok to verify
	 */
	/* [한국어] 검증 상태 업데이트 */
	if (io_u->ipo) {
		/*
		 * Remove errored entry from the verification list
		 */
		/* [한국어] 에러 시 검증 목록에서 제거 */
		if (io_u->error)
			unlog_io_piece(td, io_u);
		else {
			/* [한국어] 성공 시 IN_FLIGHT 플래그 제거 (검증 가능 상태) */
			atomic_store_release(&io_u->ipo->flags,
					io_u->ipo->flags & ~IP_F_IN_FLIGHT);
		}
	}

	/* [한국어] sync 방향 처리 */
	if (ddir_sync(ddir)) {
		if (io_u->error)
			goto error;
		/* [한국어] sync 성공 시 dirty 범위 초기화 */
		if (f) {
			f->first_write = -1ULL;
			f->last_write = -1ULL;
		}
		if (should_account(td))
			account_io_completion(td, io_u, icd, ddir, io_u->buflen);
		return;
	}

	/* [한국어] 마지막 완료된 방향 기록 */
	td->last_ddir_completed = ddir;

	/* [한국어] read/write 성공 완료 처리 */
	if (!io_u->error && ddir_rw(ddir)) {
		/* [한국어] 실제 전송된 바이트 (전체 - 잔여) */
		unsigned long long bytes = io_u->xfer_buflen - io_u->resid;
		int ret;

		/*
		 * Make sure we notice short IO from here, and requeue them
		 * appropriately!
		 */
		/* [한국어] short I/O 감지: 일부만 전송된 경우 나머지를 재큐잉 */
		if (bytes && io_u->resid) {
			io_u->xfer_buflen = io_u->resid;
			io_u->xfer_buf += bytes;
			io_u->offset += bytes;
			td->ts.short_io_u[io_u->ddir]++;
			/* [한국어] 파일 범위 내이면 재큐잉하여 나머지 처리 */
			if (io_u->offset < io_u->file->real_file_size) {
				requeue_io_u(td, io_u_ptr);
				return;
			}
		}

		/* [한국어] I/O 블록/바이트 카운터 업데이트 */
		td->io_blocks[ddir]++;
		td->io_bytes[ddir] += bytes;

		/* [한국어] 검증 목록이 아닌 경우 현재 실행의 카운터도 업데이트 */
		if (!(io_u->flags & IO_U_F_VER_LIST)) {
			td->this_io_blocks[ddir]++;
			td->this_io_bytes[ddir] += bytes;
		}

		/* [한국어] 쓰기 완료 시 파일의 쓰기 범위 기록 */
		if (ddir == DDIR_WRITE)
			file_log_write_comp(td, f, io_u->offset, bytes);

		/* [한국어] 통계 기록 */
		if (should_account(td))
			account_io_completion(td, io_u, icd, ddir, bytes);

		icd->bytes_done[ddir] += bytes;

		/* [한국어] end_io 콜백 호출 (검증 등) */
		if (io_u->end_io) {
			ret = io_u->end_io(td, io_u_ptr);
			io_u = *io_u_ptr;
			if (ret && !icd->error)
				icd->error = ret;
		}
	} else if (io_u->error) {
error:
		/* [한국어] 에러 처리 */
		icd->error = io_u->error;
		io_u_log_error(td, io_u);
	}
	if (icd->error) {
		enum error_type_bit eb = td_error_type(ddir, icd->error);

		/* [한국어] 비치명적 에러이면 카운트만 증가하고 계속 진행 */
		if (!td_non_fatal_error(td, eb, icd->error))
			return;

		/*
		 * If there is a non_fatal error, then add to the error count
		 * and clear all the errors.
		 */
		/* [한국어] 에러 카운트 증가 및 에러 상태 초기화 */
		update_error_count(td, icd->error);
		td_clear_error(td);
		icd->error = 0;
		if (io_u)
			io_u->error = 0;
	}
}

/* [한국어] I/O 완료 데이터(icd)를 초기화하는 함수.
 *
 * @td: 스레드 데이터
 * @icd: I/O 완료 데이터
 * @nr: 완료할 I/O 수
 *
 * 현재 시각 기록, 에러 초기화, 바이트 카운터 초기화.
 */
static void init_icd(struct thread_data *td, struct io_completion_data *icd,
		     int nr)
{
	int ddir;

	/* [한국어] gtod 축소 모드가 아니면 현재 시각 기록 */
	if (!gtod_reduce(td))
		fio_gettime(&icd->time, NULL);

	icd->nr = nr;

	icd->error = 0;
	/* [한국어] 각 방향별 바이트 카운터 초기화 */
	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
		icd->bytes_done[ddir] = 0;
}

/* [한국어] 비동기 엔진에서 여러 I/O 완료를 처리하는 함수.
 *
 * @td: 스레드 데이터
 * @icd: I/O 완료 데이터
 *
 * icd->nr 개수만큼 엔진의 event()로 완료된 io_u를 가져와
 * io_completed()로 처리한 후 put_io_u()로 반환한다.
 */
static void ios_completed(struct thread_data *td,
			  struct io_completion_data *icd)
{
	struct io_u *io_u;
	int i;

	for (i = 0; i < icd->nr; i++) {
		/* [한국어] 엔진에서 i번째 완료된 io_u 가져오기 */
		io_u = td->io_ops->event(td, i);

		/* [한국어] 완료 처리 */
		io_completed(td, &io_u, icd);

		/* [한국어] 프리리스트로 반환 (재큐잉된 경우 io_u가 NULL) */
		if (io_u)
			put_io_u(td, io_u);
	}
}

/* [한국어] I/O 완료 후 bytes_done 카운터를 업데이트하는 함수.
 *
 * @td: 스레드 데이터
 * @icd: I/O 완료 데이터
 *
 * 검증(verify) 모드에서는 bytes_verified를 업데이트하고,
 * 일반 모드에서는 bytes_done을 각 방향별로 업데이트한다.
 */
static void io_u_update_bytes_done(struct thread_data *td,
				   struct io_completion_data *icd)
{
	int ddir;

	/* [한국어] 검증 모드에서는 읽기 바이트를 검증 바이트로 기록 */
	if (td->runstate == TD_VERIFYING) {
		td->bytes_verified += icd->bytes_done[DDIR_READ];
		if (td_write(td))
			return;
	}

	/* [한국어] 각 방향별 완료 바이트 누적 */
	for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++)
		td->bytes_done[ddir] += icd->bytes_done[ddir];
}

/*
 * Complete a single io_u for the sync engines.
 */
/* [한국어] ★ 핵심 함수 ★ 동기 엔진용 단일 I/O 완료 처리 함수.
 *
 * @td: 스레드 데이터
 * @io_u: 완료된 I/O 유닛
 * @return: 0=성공, -1=에러
 *
 * 동기 I/O 엔진(sync, psync 등)에서 I/O 완료 후 호출된다.
 * icd 초기화 -> io_completed() -> put_io_u() -> bytes_done 업데이트
 */
int io_u_sync_complete(struct thread_data *td, struct io_u *io_u)
{
	struct io_completion_data icd;

	/* [한국어] 완료 데이터 초기화 (시각 기록 등) */
	init_icd(td, &icd, 1);
	/* [한국어] I/O 완료 처리 (통계, 에러, 재큐잉 등) */
	io_completed(td, &io_u, &icd);

	/* [한국어] io_u 반환 (재큐잉되지 않은 경우) */
	if (io_u)
		put_io_u(td, io_u);

	if (icd.error) {
		td_verror(td, icd.error, "io_u_sync_complete");
		return -1;
	}

	/* [한국어] bytes_done 카운터 업데이트 */
	io_u_update_bytes_done(td, &icd);

	return 0;
}

/*
 * Called to complete min_events number of io for the async engines.
 */
/* [한국어] ★ 핵심 함수 ★ 비동기 엔진용 I/O 완료 처리 함수.
 *
 * @td: 스레드 데이터
 * @min_evts: 최소 완료 대기 이벤트 수 (0이면 논블로킹)
 * @return: 완료된 I/O 수 (양수) 또는 에러 (-1)
 *
 * 비동기 I/O 엔진(libaio, io_uring 등)에서 완료 이벤트를 수집한다.
 * td_io_getevents()로 완료된 이벤트를 가져오고,
 * ios_completed()로 각각 처리한 후 bytes_done을 업데이트한다.
 */
int io_u_queued_complete(struct thread_data *td, int min_evts)
{
	struct io_completion_data icd;
	struct timespec *tvp = NULL;
	int ret;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 0, };

	dprint(FD_IO, "io_u_queued_complete: min=%d\n", min_evts);

	/* [한국어] min_evts=0이면 논블로킹 (타임아웃 0) */
	if (!min_evts)
		tvp = &ts;
	/* [한국어] min_evts가 cur_depth를 초과하면 조정 */
	else if (min_evts > td->cur_depth)
		min_evts = td->cur_depth;

	/* No worries, td_io_getevents fixes min and max if they are
	 * set incorrectly */
	/* [한국어] 엔진에서 완료 이벤트 가져오기 */
	ret = td_io_getevents(td, min_evts, td->o.iodepth_batch_complete_max, tvp);
	if (ret < 0) {
		td_verror(td, -ret, "td_io_getevents");
		return ret;
	} else if (!ret)
		return ret;

	/* [한국어] 완료 데이터 초기화 및 처리 */
	init_icd(td, &icd, ret);
	ios_completed(td, &icd);
	if (icd.error) {
		td_verror(td, icd.error, "io_u_queued_complete");
		return -1;
	}

	/* [한국어] bytes_done 카운터 업데이트 */
	io_u_update_bytes_done(td, &icd);

	return ret;
}

/*
 * Call when io_u is really queued, to update the submission latency.
 */
/* [한국어] ★ 핵심 함수 ★ I/O가 실제로 큐에 들어갔을 때 호출되는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: 큐잉된 I/O 유닛
 *
 * 제출 레이턴시(slat, submission latency)를 기록한다.
 * slat = io_u가 제출되기까지 걸린 시간 (start_time ~ issue_time).
 * ramp 기간이 끝나고 통계가 활성화된 경우에만 기록.
 */
void io_u_queued(struct thread_data *td, struct io_u *io_u)
{
	if (!td->o.disable_slat && ramp_period_over(td) && td->o.stats) {
		if (td->parent)
			td = td->parent;
		/* [한국어] slat 샘플 기록 */
		add_slat_sample(td, io_u);
	}
}

/*
 * See if we should reuse the last seed, if dedupe is enabled
 */
/* [한국어] 중복 제거(dedupe) 설정에 따라 버퍼 생성용 난수 상태를 결정하는 함수.
 *
 * @td: 스레드 데이터
 * @return: 사용할 난수 상태 포인터
 *
 * dedupe_percentage에 따라:
 * - 0%: 항상 새 데이터 생성 (현재 상태 사용)
 * - 100%: 항상 같은 데이터 (이전 상태 복사 후 현재 상태 사용)
 * - 1-99%: 확률적으로 이전 데이터 재사용 또는 새 데이터 생성
 *
 * dedupe_mode에 따라:
 * - REPEAT: 직전 데이터를 그대로 재사용
 * - WORKING_SET: 미리 정의된 고유 페이지 풀에서 랜덤 선택
 */
static struct frand_state *get_buf_state(struct thread_data *td)
{
	unsigned int v;
	unsigned long long i;

	/* [한국어] dedupe 비활성화: 항상 현재 상태 (새 데이터) */
	if (!td->o.dedupe_percentage)
		return &td->buf_state;
	else if (td->o.dedupe_percentage == 100) {
		/* [한국어] 100% dedupe: 이전 상태를 보존하고 현재 상태 사용 */
		frand_copy(&td->buf_state_prev, &td->buf_state);
		return &td->buf_state;
	}

	/* [한국어] 확률적 결정: 1~100 사이 난수 생성 */
	v = rand_between(&td->dedupe_state, 1, 100);

	if (v <= td->o.dedupe_percentage)
		switch (td->o.dedupe_mode) {
		case DEDUPE_MODE_REPEAT:
			/*
			* The caller advances the returned frand_state.
			* A copy of prev should be returned instead since
			* a subsequent intention to generate a deduped buffer
			* might result in generating a unique one
			*/
			/* [한국어] REPEAT 모드: 이전 상태의 복사본 반환 */
			frand_copy(&td->buf_state_ret, &td->buf_state_prev);
			return &td->buf_state_ret;
		case DEDUPE_MODE_WORKING_SET:
			/* [한국어] WORKING_SET 모드: 고유 페이지 풀에서 랜덤 선택 */
			i = rand_between(&td->dedupe_working_set_index_state, 0, td->num_unique_pages - 1);
			frand_copy(&td->buf_state_ret, &td->dedupe_working_set_states[i]);
			return &td->buf_state_ret;
		default:
			log_err("unexpected dedupe mode %u\n", td->o.dedupe_mode);
			assert(0);
		}

	/* [한국어] dedupe 확률에 걸리지 않으면 새 데이터 생성 */
	return &td->buf_state;
}

/* [한국어] 버퍼 생성 후 난수 상태를 저장하는 함수.
 *
 * @td: 스레드 데이터
 * @rs: 사용한 난수 상태 포인터
 *
 * 다음 dedupe 판단을 위해 현재 상태를 이전 상태로 복사.
 */
static void save_buf_state(struct thread_data *td, struct frand_state *rs)
{
	/* [한국어] 100% dedupe: 이전 상태를 현재 상태에서 복원 */
	if (td->o.dedupe_percentage == 100)
		frand_copy(rs, &td->buf_state_prev);
	/* [한국어] 새 데이터를 생성한 경우: 현재 상태를 이전 상태로 저장 */
	else if (rs == &td->buf_state)
		frand_copy(&td->buf_state_prev, rs);
}

/* [한국어] I/O 버퍼를 데이터로 채우는 함수.
 *
 * @td: 스레드 데이터
 * @buf: 채울 버퍼 포인터
 * @min_write: 최소 쓰기 크기
 * @max_bs: 최대 블록 크기 (채울 전체 크기)
 *
 * 버퍼 채우기 우선순위:
 * 1. compress/dedupe 설정 시: 압축률에 맞는 혼합 패턴 생성
 * 2. buffer_pattern 설정 시: 지정된 패턴으로 채우기
 * 3. zero_buffers 설정 시: 0으로 채우기
 * 4. 기본: 랜덤 데이터로 채우기
 */
void fill_io_buffer(struct thread_data *td, void *buf, unsigned long long min_write,
		    unsigned long long max_bs)
{
	struct thread_options *o = &td->o;

	/* [한국어] CUDA 메모리이면 호스트에서 채울 수 없으므로 건너뜀 */
	if (o->mem_type == MEM_CUDA_MALLOC)
		return;

	if (o->compress_percentage || o->dedupe_percentage) {
		/* [한국어] 압축/dedupe: 청크 단위로 혼합 패턴 생성 */
		unsigned int perc = td->o.compress_percentage;
		struct frand_state *rs = NULL;
		unsigned long long left = max_bs;
		unsigned long long this_write;

		do {
			/*
			 * Buffers are either entirely dedupe-able or not.
			 * If we choose to dedup, the buffer should undergo
			 * the same manipulation as the original write. Which
			 * means we should retrack the steps we took for compression
			 * as well.
			 */
			/* [한국어] 첫 청크에서 dedupe 여부 결정 */
			if (!rs)
				rs = get_buf_state(td);

			min_write = min(min_write, left);

			/* [한국어] compress_chunk 단위로 처리 */
			this_write = min_not_zero(min_write,
						(unsigned long long) td->o.compress_chunk);

			/* [한국어] 지정된 압축률에 맞는 혼합 패턴 생성
			 * (perc% 는 압축 가능한 패턴, 나머지는 랜덤) */
			fill_random_buf_percentage(rs, buf, perc,
				this_write, this_write,
				o->buffer_pattern,
				o->buffer_pattern_bytes);

			buf += this_write;
			left -= this_write;
			save_buf_state(td, rs);
		} while (left);
	} else if (o->buffer_pattern_bytes)
		/* [한국어] 지정된 패턴으로 채우기 */
		fill_buffer_pattern(td, buf, max_bs);
	else if (o->zero_buffers)
		/* [한국어] 0으로 채우기 */
		memset(buf, 0, max_bs);
	else
		/* [한국어] 랜덤 데이터로 채우기 */
		fill_random_buf(get_buf_state(td), buf, max_bs);
}

/*
 * "randomly" fill the buffer contents
 */
/* [한국어] io_u의 버퍼를 데이터로 채우는 래퍼 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @min_write: 최소 쓰기 크기
 * @max_bs: 최대 블록 크기
 *
 * buf_filled_len을 초기화하고 fill_io_buffer()를 호출한다.
 */
void io_u_fill_buffer(struct thread_data *td, struct io_u *io_u,
		      unsigned long long min_write, unsigned long long max_bs)
{
	io_u->buf_filled_len = 0;
	fill_io_buffer(td, io_u->buf, min_write, max_bs);
}

/* [한국어] sync_file_range 시스템 콜을 수행하는 내부 함수.
 *
 * @td: 스레드 데이터
 * @f: 파일 구조체
 * @return: sync_file_range()의 반환값
 *
 * 파일의 dirty 범위(first_write ~ last_write)에 대해
 * sync_file_range()를 호출하여 부분 동기화한다.
 */
static int do_sync_file_range(const struct thread_data *td,
			      struct fio_file *f)
{
	uint64_t offset, nbytes;

	offset = f->first_write;
	nbytes = f->last_write - f->first_write;

	if (!nbytes)
		return 0;

	return sync_file_range(f->fd, offset, nbytes, td->o.sync_file_range);
}

/* [한국어] 동기화(sync) I/O를 수행하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛 (ddir이 SYNC/DATASYNC/SYNC_FILE_RANGE/SYNCFS 중 하나)
 * @return: 성공 시 0 이상, 실패 시 음수
 *
 * io_u->ddir에 따라 적절한 동기화 시스템 콜을 호출:
 * - DDIR_SYNC: fsync() (macOS에서는 fcntl F_FULLFSYNC)
 * - DDIR_DATASYNC: fdatasync()
 * - DDIR_SYNC_FILE_RANGE: sync_file_range()
 * - DDIR_SYNCFS: syncfs()
 */
int do_io_u_sync(const struct thread_data *td, struct io_u *io_u)
{
	int ret;

	if (io_u->ddir == DDIR_SYNC) {
#ifdef CONFIG_FCNTL_SYNC
		ret = fcntl(io_u->file->fd, F_FULLFSYNC);
#else
		ret = fsync(io_u->file->fd);
#endif
	} else if (io_u->ddir == DDIR_DATASYNC) {
#ifdef CONFIG_FDATASYNC
		ret = fdatasync(io_u->file->fd);
#else
		ret = io_u->xfer_buflen;
		io_u->error = EINVAL;
#endif
	} else if (io_u->ddir == DDIR_SYNC_FILE_RANGE) {
		ret = do_sync_file_range(td, io_u->file);
	} else if (io_u->ddir == DDIR_SYNCFS) {
		ret = syncfs(io_u->file->fd);
	} else {
		ret = io_u->xfer_buflen;
		io_u->error = EINVAL;
	}

	if (ret < 0)
		io_u->error = errno;

	return ret;
}

/* [한국어] trim I/O를 수행하는 함수.
 *
 * @td: 스레드 데이터
 * @io_u: I/O 유닛
 * @return: 성공 시 xfer_buflen (trim된 바이트), 실패 시 0
 *
 * ZBD 모드에서는 zbd_do_io_u_trim()을 먼저 시도하고,
 * 일반 모드에서는 os_trim()으로 파일의 지정된 범위를 trim한다.
 * trim은 SSD 등에서 더 이상 사용하지 않는 블록을 해제하여
 * 성능을 유지하는 데 사용된다.
 */
int do_io_u_trim(struct thread_data *td, struct io_u *io_u)
{
#ifndef FIO_HAVE_TRIM
	/* [한국어] trim 미지원 플랫폼에서는 EINVAL 에러 */
	io_u->error = EINVAL;
	return 0;
#else
	struct fio_file *f = io_u->file;
	int ret;

	/* [한국어] ZBD 모드에서 trim 처리 */
	if (td->o.zone_mode == ZONE_MODE_ZBD) {
		ret = zbd_do_io_u_trim(td, io_u);
		/* [한국어] ZBD에서 완전히 처리되었으면 바로 반환 */
		if (ret == io_u_completed)
			return io_u->xfer_buflen;
		if (ret)
			goto err;
	}

	/* [한국어] OS의 trim 기능으로 지정된 범위 trim */
	ret = os_trim(f, io_u->offset, io_u->xfer_buflen);
	if (!ret)
		return io_u->xfer_buflen;

err:
	io_u->error = ret;
	return 0;
#endif
}
