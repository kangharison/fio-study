/*
 * Code related to writing an iolog of what a thread is doing, and to
 * later read that back and replay
 */
/*
 * [한국어] iolog.c - I/O 로깅 및 재생
 *
 * 이 파일은 fio의 I/O 패턴을 파일에 기록하고, 나중에 재생하는 기능을 구현한다.
 * 주요 기능:
 *   1) log_io_u() / log_file()        - I/O 동작과 파일 이벤트를 iolog 파일에 기록
 *   2) read_iolog() / read_iolog_get() - iolog 파일을 읽어 I/O 패턴을 재생
 *   3) init_iolog()                    - iolog 읽기/쓰기 모드 초기화
 *   4) setup_log() / flush_log()       - 성능 로그 설정 및 파일 출력
 *   5) gz_work() / iolog_flush()       - zlib 기반 로그 압축/해제
 *   6) td_writeout_logs()              - 모든 로그(BW/LAT/IOPS 등)를 파일에 기록
 *
 * iolog 파일 형식:
 *   - 버전 2: "<파일명> <동작> <오프셋> <크기>" 형식
 *   - 버전 3: "<타임스탬프> <파일명> <동작> <오프셋> <크기>" 형식 (시간 기반 재생 지원)
 *
 * 로그 압축 흐름 (CONFIG_ZLIB):
 *   로그 청크 초과 → iolog_cur_flush() → workqueue에 gz_work 제출
 *   → deflate 압축 → chunk_list에 추가 → flush_log()에서 inflate 후 출력
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef CONFIG_ZLIB
#include <zlib.h>           /* 로그 압축/해제를 위한 zlib */
#endif

/* fio 내부 헤더 파일들 */
#include "flist.h"          /* fio 연결 리스트 */
#include "fio.h"            /* fio 핵심 구조체 및 매크로 */
#include "trim.h"           /* trim 관련 유틸리티 */
#include "filelock.h"       /* 파일 잠금 (로그 출력 동기화) */
#include "smalloc.h"        /* 공유 메모리 할당기 */
#include "blktrace.h"       /* blktrace 형식 iolog 지원 */
#include "pshared.h"        /* 프로세스 간 공유 뮤텍스/조건변수 */
#include "lib/roundup.h"    /* 2의 거듭제곱으로 올림 유틸리티 */

/* 소켓 통신 헤더 (Unix 소켓을 통한 iolog 읽기 지원) */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

/* [한국어] 전방 선언 - 로그 압축 후 파일 기록 */
static int iolog_flush(struct io_log *log);

/* [한국어] iolog 파일의 버전 식별 문자열 */
static const char iolog_ver2[] = "fio version 2 iolog";
static const char iolog_ver3[] = "fio version 3 iolog";

/* [한국어] I/O piece를 재생 큐(io_log_list)에 추가하고 총 I/O 크기를 갱신 */
void queue_io_piece(struct thread_data *td, struct io_piece *ipo)
{
	flist_add_tail(&ipo->list, &td->io_log_list);
	td->total_io_size += ipo->len;
}

/* [한국어] I/O 동작을 iolog 파일에 기록 (타임스탬프, 파일명, 방향, 오프셋, 크기) */
void log_io_u(const struct thread_data *td, const struct io_u *io_u)
{
	struct timespec now;

	if (!td->o.write_iolog_file)
		return;

	fio_gettime(&now, NULL);
	fprintf(td->iolog_f, "%llu %s %s %llu %llu\n",
		(unsigned long long) utime_since_now(&td->io_log_start_time),
		io_u->file->file_name, io_ddir_name(io_u->ddir), io_u->offset,
		io_u->buflen);

}

/* [한국어] 파일 이벤트(add/open/close)를 iolog 파일에 기록 */
void log_file(struct thread_data *td, struct fio_file *f,
	      enum file_log_act what)
{
	const char *act[] = { "add", "open", "close" };
	struct timespec now;

	assert(what < 3);

	if (!td->o.write_iolog_file)
		return;


	/*
	 * this happens on the pre-open/close done before the job starts
	 */
	/* [한국어] 작업 시작 전 pre-open/close 시에는 iolog_f가 아직 NULL */
	if (!td->iolog_f)
		return;

	fio_gettime(&now, NULL);
	fprintf(td->iolog_f, "%llu %s %s\n",
		(unsigned long long) utime_since_now(&td->io_log_start_time),
		f->file_name, act[what]);
}

/* [한국어] iolog 재생 시 지연(delay) 시간을 대기하는 함수
 * 실제 경과 시간을 고려하여 남은 지연만큼만 대기하고,
 * 대기 중에도 완료된 I/O를 수거하여 자원을 반환한다 */
static void iolog_delay(struct thread_data *td, unsigned long delay)
{
	uint64_t usec = utime_since_now(&td->last_issue);
	unsigned long orig_delay = delay;
	struct timespec ts;
	int ret = 0;

	/* [한국어] 이전 시간 오프셋(누적 오차)이 지연보다 크면 즉시 반환 */
	if (delay < td->time_offset) {
		td->time_offset = 0;
		return;
	}

	delay -= td->time_offset;
	/* [한국어] 이미 I/O 처리에 소요된 시간이 지연보다 크면 즉시 반환 */
	if (delay < usec)
		return;

	delay -= usec;

	fio_gettime(&ts, NULL);

	/* [한국어] 지연 시간이 남아있는 동안 루프: 완료된 I/O를 수거하며 대기 */
	while (delay && !td->terminate) {
		ret = io_u_queued_complete(td, 0);
		if (ret < 0)
			td_verror(td, -ret, "io_u_queued_complete");
		if (td->flags & TD_F_REGROW_LOGS)
			regrow_logs(td);
		if (utime_since_now(&ts) > delay)
			break;
	}

	/* [한국어] 실제 대기 시간과 목표 지연의 차이를 오프셋으로 보정 */
	usec = utime_since_now(&ts);
	if (usec > orig_delay)
		td->time_offset = usec - orig_delay;
	else
		td->time_offset = 0;
}

/* [한국어] 특수 io_piece 처리 (파일 열기/닫기/삭제 등 비-I/O 동작)
 * 반환값: 0 = 일반 I/O, 1 = 특수 동작 처리 완료, -1 = 에러 */
static int ipo_special(struct thread_data *td, struct io_piece *ipo)
{
	struct fio_file *f;
	int ret;

	/*
	 * Not a special ipo
	 */
	/* [한국어] DDIR_INVAL이 아니면 일반 I/O → 0 반환 */
	if (ipo->ddir != DDIR_INVAL)
		return 0;

	f = td->files[ipo->fileno];

	if (ipo->delay)
		iolog_delay(td, ipo->delay);
	if (fio_fill_issue_time(td))
		fio_gettime(&td->last_issue, NULL);
	/* [한국어] 파일 동작 종류에 따라 분기 처리 */
	switch (ipo->file_action) {
	case FIO_LOG_OPEN_FILE:
		/* [한국어] replay_redirect 모드에서 이미 열린 파일은 재열기 무시 */
		if (td->o.replay_redirect && fio_file_open(f)) {
			dprint(FD_FILE, "iolog: ignoring re-open of file %s\n",
					f->file_name);
			break;
		}
		ret = td_io_open_file(td, f);
		if (!ret) {
			/* [한국어] 데이터 배치(DP) 기능 초기화 */
			if (td->o.dp_type != FIO_DP_NONE) {
				int dp_init_ret = dp_init(td);

				if (dp_init_ret != 0) {
					td_verror(td, abs(dp_init_ret), "dp_init");
					return -1;
				}
			}
			break;
		}
		td_verror(td, ret, "iolog open file");
		return -1;
	case FIO_LOG_CLOSE_FILE:
		td_io_close_file(td, f);
		break;
	case FIO_LOG_UNLINK_FILE:
		td_io_unlink_file(td, f);
		break;
	case FIO_LOG_ADD_FILE:
		/*
		 * Nothing to do
		 */
		break;
	default:
		log_err("fio: bad file action %d\n", ipo->file_action);
		break;
	}

	return 1;
}

/* [한국어] 전방 선언 - iolog 파일 파싱 함수 */
static bool read_iolog(struct thread_data *td);

/* [한국어] replay_time_scale을 적용하여 ttime 기반 지연 시간을 계산
 * scale이 100이면 원본 속도, 200이면 2배속 재생, 50이면 절반 속도 */
unsigned long long delay_since_ttime(const struct thread_data *td,
	       unsigned long long time)
{
	double tmp;
	double scale;
	const unsigned long long *last_ttime = &td->io_log_last_ttime;

	if (!*last_ttime || td->o.no_stall || time < *last_ttime)
		return 0;
	else if (td->o.replay_time_scale == 100)
		return time - *last_ttime;


	scale = (double) 100.0 / (double) td->o.replay_time_scale;
	tmp = time - *last_ttime;
	return tmp * scale;
}

/* [한국어] iolog에서 다음 I/O를 가져와 io_u에 설정하는 함수
 * 반환값: 0 = I/O 준비 완료, 1 = 더 이상 I/O 없음 (완료 또는 에러) */
int read_iolog_get(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo;
	unsigned long elapsed;

	while (!flist_empty(&td->io_log_list)) {
		int ret;

		/* [한국어] 청크 모드: 현재 청크 소진 시 다음 청크 로드 */
		if (td->o.read_iolog_chunked) {
			if (td->io_log_checkmark == td->io_log_current) {
				if (td->io_log_blktrace) {
					if (!read_blktrace(td))
						return 1;
				} else {
					if (!read_iolog(td))
						return 1;
				}
			}
			td->io_log_current--;
		}
		/* [한국어] 큐의 첫 번째 io_piece를 꺼내서 처리 */
		ipo = flist_first_entry(&td->io_log_list, struct io_piece, list);
		flist_del(&ipo->list);
		remove_trim_entry(td, ipo);

		/* [한국어] 특수 동작(파일 열기/닫기 등)인지 확인 */
		ret = ipo_special(td, ipo);
		if (ret < 0) {
			free(ipo);
			break;
		} else if (ret > 0) {
			free(ipo);
			continue;
		}

		/* [한국어] io_u에 방향, 오프셋, 크기, 파일 등을 설정 */
		io_u->ddir = ipo->ddir;
		if (ipo->ddir != DDIR_WAIT) {
			io_u->offset = ipo->offset;
			io_u->verify_offset = ipo->offset;
			io_u->buflen = ipo->len;
			io_u->file = td->files[ipo->fileno];
			get_file(io_u->file);
			dprint(FD_IO, "iolog: get %llu/%llu/%s\n", io_u->offset,
						io_u->buflen, io_u->file->file_name);
			if (ipo->delay)
				iolog_delay(td, ipo->delay);

			/* [한국어] 데이터 배치(DP) 정보 채우기 */
			if (td->o.dp_type != FIO_DP_NONE)
				dp_fill_dspec_data(td, io_u);
		} else {
			/* [한국어] DDIR_WAIT: 지정된 시간까지 대기 */
			elapsed = mtime_since_genesis();
			if (ipo->delay > elapsed)
				usec_sleep(td, (ipo->delay - elapsed) * 1000);
		}

		free(ipo);

		if (io_u->ddir != DDIR_WAIT)
			return 0;
	}

	td->done = 1;
	return 1;
}

/* [한국어] I/O 히스토리 트리와 리스트를 모두 정리(prune)하여 메모리 해제 */
void prune_io_piece_log(struct thread_data *td)
{
	struct io_piece *ipo;
	struct fio_rb_node *n;

	/* [한국어] RB 트리에서 모든 노드 제거 */
	while ((n = rb_first(&td->io_hist_tree)) != NULL) {
		ipo = rb_entry(n, struct io_piece, rb_node);
		rb_erase(n, &td->io_hist_tree);
		remove_trim_entry(td, ipo);
		td->io_hist_len--;
		free(ipo);
	}

	/* [한국어] 연결 리스트에서 모든 노드 제거 */
	while (!flist_empty(&td->io_hist_list)) {
		ipo = flist_first_entry(&td->io_hist_list, struct io_piece, list);
		flist_del(&ipo->list);
		remove_trim_entry(td, ipo);
		td->io_hist_len--;
		free(ipo);
	}
}

/*
 * log a successful write, so we can unwind the log for verify
 */
/* [한국어] 성공한 쓰기 I/O를 히스토리에 기록 (나중에 verify에서 되감기(unwind)용)
 * 랜덤 맵이 없거나 offset modifier가 있는 경우 RB 트리에 정렬 삽입하여
 * 중복 블록을 감지하고, 그렇지 않으면 단순 리스트에 추가한다 */
void log_io_piece(struct thread_data *td, struct io_u *io_u)
{
	struct fio_rb_node **p, *parent;
	struct io_piece *ipo, *__ipo;

	ipo = calloc(1, sizeof(struct io_piece));
	init_ipo(ipo);
	ipo->file = io_u->file;
	ipo->offset = io_u->offset;
	ipo->len = io_u->buflen;
	ipo->numberio = io_u->numberio;
	ipo->flags = IP_F_IN_FLIGHT;       /* [한국어] 아직 완료되지 않은 상태 표시 */

	io_u->ipo = ipo;

	/* [한국어] trim 대상이면 trim 리스트에도 추가 */
	if (io_u_should_trim(td, io_u)) {
		flist_add_tail(&ipo->trim_list, &td->trim_list);
		td->trim_entries++;
	}

	/*
	 * Sort writes if we don't have a random map in which case we need to
	 * check for duplicate blocks and drop the old one, which we rely on
	 * the rb insert/lookup for handling. Sort writes if we have offset
	 * modifier which can also create duplicate blocks.
	 */
	/* [한국어] 중복 위험이 없으면 단순 리스트에 추가하고 반환 */
	if (!fio_offset_overlap_risk(td)) {
		INIT_FLIST_HEAD(&ipo->list);
		flist_add_tail(&ipo->list, &td->io_hist_list);
		ipo->flags |= IP_F_ONLIST;
		td->io_hist_len++;
		return;
	}

	RB_CLEAR_NODE(&ipo->rb_node);

	/*
	 * Sort the entry into the verification list
	 */
	/* [한국어] RB 트리에 정렬 삽입 - 파일 → 오프셋 순서로 비교
	 * 중복(overlap) 발견 시 기존 노드를 제거하고 재시작 */
restart:
	p = &td->io_hist_tree.rb_node;
	parent = NULL;
	while (*p) {
		int overlap = 0;
		parent = *p;

		__ipo = rb_entry(parent, struct io_piece, rb_node);
		if (ipo->file < __ipo->file)
			p = &(*p)->rb_left;
		else if (ipo->file > __ipo->file)
			p = &(*p)->rb_right;
		else if (ipo->offset < __ipo->offset) {
			p = &(*p)->rb_left;
			overlap = ipo->offset + ipo->len > __ipo->offset;
		}
		else if (ipo->offset > __ipo->offset) {
			p = &(*p)->rb_right;
			overlap = __ipo->offset + __ipo->len > ipo->offset;
		}
		else
			overlap = 1;

		if (overlap) {
			dprint(FD_IO, "iolog: overlap %llu/%lu, %llu/%lu\n",
				__ipo->offset, __ipo->len,
				ipo->offset, ipo->len);
			td->io_hist_len--;
			rb_erase(parent, &td->io_hist_tree);
			remove_trim_entry(td, __ipo);
			if (!(__ipo->flags & IP_F_IN_FLIGHT))
				free(__ipo);
			goto restart;
		}
	}

	/* [한국어] 새 노드를 RB 트리에 삽입하고 색상 균형 조정 */
	rb_link_node(&ipo->rb_node, parent, p);
	rb_insert_color(&ipo->rb_node, &td->io_hist_tree);
	ipo->flags |= IP_F_ONRB;
	td->io_hist_len++;
}

/* [한국어] 실패한 I/O의 히스토리 기록을 취소(unlog)하고 io_piece를 해제
 * block_info가 설정된 경우 해당 블록의 상태를 실패로 갱신 */
void unlog_io_piece(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = io_u->ipo;

	/* [한국어] 블록 정보 추적 시 실패 상태 기록 */
	if (td->ts.nr_block_infos) {
		uint32_t *info = io_u_block_info(td, io_u);
		if (BLOCK_INFO_STATE(*info) < BLOCK_STATE_TRIM_FAILURE) {
			if (io_u->ddir == DDIR_TRIM)
				*info = BLOCK_INFO_SET_STATE(*info,
						BLOCK_STATE_TRIM_FAILURE);
			else if (io_u->ddir == DDIR_WRITE)
				*info = BLOCK_INFO_SET_STATE(*info,
						BLOCK_STATE_WRITE_FAILURE);
		}
	}

	if (!ipo)
		return;

	/* [한국어] RB 트리 또는 리스트에서 제거 */
	if (ipo->flags & IP_F_ONRB)
		rb_erase(&ipo->rb_node, &td->io_hist_tree);
	else if (ipo->flags & IP_F_ONLIST)
		flist_del(&ipo->list);

	free(ipo);
	io_u->ipo = NULL;
	td->io_hist_len--;
}

/* [한국어] I/O piece의 길이를 실제 전송된 크기로 조정 (부분 I/O 처리) */
void trim_io_piece(const struct io_u *io_u)
{
	struct io_piece *ipo = io_u->ipo;

	if (!ipo)
		return;

	ipo->len = io_u->xfer_buflen - io_u->resid;
}

/* [한국어] iolog 쓰기 파일을 플러시하고 닫기 */
void write_iolog_close(struct thread_data *td)
{
	if (!td->iolog_f)
		return;

	fflush(td->iolog_f);
	fclose(td->iolog_f);
	free(td->iolog_buf);
	td->iolog_f = NULL;
	td->iolog_buf = NULL;
}

/* [한국어] 청크 모드에서 다음에 가져올 iolog 항목 수를 계산
 * 경과 시간 대비 소비 속도를 기반으로 1초분의 항목 수를 예측 */
int64_t iolog_items_to_fetch(struct thread_data *td)
{
	struct timespec now;
	uint64_t elapsed;
	uint64_t for_1s;
	int64_t items_to_fetch;

	if (!td->io_log_highmark)
		return 10;


	fio_gettime(&now, NULL);
	elapsed = ntime_since(&td->io_log_highmark_time, &now);
	if (elapsed) {
		/* [한국어] 1초당 소비할 항목 수 추정 → 다음 청크 크기 결정 */
		for_1s = (td->io_log_highmark - td->io_log_current) * 1000000000 / elapsed;
		items_to_fetch = for_1s - td->io_log_current;
		if (items_to_fetch < 0)
			items_to_fetch = 0;
	} else
		items_to_fetch = 0;

	/* [한국어] highmark와 checkmark 갱신 (checkmark = 다음 로드 트리거 지점) */
	td->io_log_highmark = td->io_log_current + items_to_fetch;
	td->io_log_checkmark = (td->io_log_highmark + 1) / 2;
	fio_gettime(&td->io_log_highmark_time, NULL);

	return items_to_fetch;
}

/* [한국어] iolog 라인 파싱 매크로 - I/O 동작인지 파일 동작인지 구분
 * 버전3: 5개 필드(시간,파일,동작,오프셋,크기) = I/O, 3개 = 파일동작
 * 버전2: 4개 필드(파일,동작,오프셋,크기) = I/O, 2개 = 파일동작 */
#define io_act(_td, _r) (((_td)->io_log_version == 3 && (r) == 5) || \
					((_td)->io_log_version == 2 && (r) == 4))
#define file_act(_td, _r) (((_td)->io_log_version == 3 && (r) == 3) || \
					((_td)->io_log_version == 2 && (r) == 2))

/*
 * Read version 2 and 3 iolog data. It is enhanced to include per-file logging,
 * syncs, etc.
 */
/* [한국어] iolog 파일을 파싱하여 io_piece 리스트를 구축하는 핵심 함수
 * 버전 2/3을 모두 지원하며, 청크 모드에서는 일부분만 읽을 수 있다 */
static bool read_iolog(struct thread_data *td)
{
	unsigned long long offset;
	unsigned int bytes;
	unsigned long long delay = 0;
	int reads, writes, trims, waits, fileno = 0, file_action = 0; /* stupid gcc */
	char *rfname, *fname, *act;
	char *str, *p;
	enum fio_ddir rw;
	bool realloc = false;
	int64_t items_to_fetch = 0;
	int syncs;

	/* [한국어] 청크 모드: 가져올 항목 수 결정 (0이면 이미 충분) */
	if (td->o.read_iolog_chunked) {
		items_to_fetch = iolog_items_to_fetch(td);
		if (!items_to_fetch)
			return true;
	}

	/*
	 * Read in the read iolog and store it, reuse the infrastructure
	 * for doing verifications.
	 */
	/* [한국어] 파싱용 버퍼 할당 */
	str = malloc(4096);
	rfname = fname = malloc(256+16);
	act = malloc(256+16);

	syncs = reads = writes = trims = waits = 0;
	/* [한국어] iolog 파일에서 한 줄씩 읽어 파싱 */
	while ((p = fgets(str, 4096, td->io_log_rfile)) != NULL) {
		struct io_piece *ipo;
		int r;
		unsigned long long ttime;

		/* [한국어] 버전 3: 타임스탬프 포함 파싱 */
		if (td->io_log_version == 3) {
			r = sscanf(p, "%llu %256s %256s %llu %u", &ttime, rfname, act,
							&offset, &bytes);
			delay = delay_since_ttime(td, ttime);
			td->io_log_last_ttime = ttime;
			/*
			 * "wait" is not allowed with version 3
			 */
			/* [한국어] 버전 3에서는 wait 명령 무시 (타임스탬프가 지연을 대체) */
			if (!strcmp(act, "wait")) {
				log_err("iolog: ignoring wait command with"
					" version 3 for file %s\n", fname);
				continue;
			}
		} else /* version 2 */
			r = sscanf(p, "%256s %256s %llu %u", rfname, act, &offset, &bytes);

		if (td->o.replay_redirect)
			fname = td->o.replay_redirect;

		if (io_act(td, r)) {
			/*
			 * Check action first
			 */
			/* [한국어] I/O 동작 문자열을 ddir 열거형으로 변환 */
			if (!strcmp(act, "wait"))
				rw = DDIR_WAIT;
			else if (!strcmp(act, "read")) {
				if (td->o.replay_skip & (1u << DDIR_READ))
					continue;
				rw = DDIR_READ;
			} else if (!strcmp(act, "write")) {
				if (td->o.replay_skip & (1u << DDIR_WRITE))
					continue;
				rw = DDIR_WRITE;
			} else if (!strcmp(act, "sync")) {
				if (td->o.replay_skip & (1u << DDIR_SYNC))
					continue;
				rw = DDIR_SYNC;
			} else if (!strcmp(act, "datasync"))
				rw = DDIR_DATASYNC;
			else if (!strcmp(act, "trim")) {
				if (td->o.replay_skip & (1u << DDIR_TRIM))
					continue;
				rw = DDIR_TRIM;
			} else {
				log_err("fio: bad iolog file action: %s\n",
									act);
				continue;
			}
			fileno = get_fileno(td, fname);
		} else if (file_act(td, r)) {
			/* [한국어] 파일 동작(add/open/close) 처리 */
			rw = DDIR_INVAL;
			if (!strcmp(act, "add")) {
				if (td->o.replay_redirect &&
				    get_fileno(td, fname) != -1) {
					dprint(FD_FILE, "iolog: ignoring"
						" re-add of file %s\n", fname);
				} else {
					fileno = add_file(td, fname, td->subjob_number, 1);
					file_action = FIO_LOG_ADD_FILE;
				}
			} else if (!strcmp(act, "open")) {
				fileno = get_fileno(td, fname);
				file_action = FIO_LOG_OPEN_FILE;
			} else if (!strcmp(act, "close")) {
				fileno = get_fileno(td, fname);
				file_action = FIO_LOG_CLOSE_FILE;
			} else {
				log_err("fio: bad iolog file action: %s\n",
									act);
				continue;
			}
		} else {
			log_err("bad iolog%d: %s\n", td->io_log_version, p);
			continue;
		}

		/* [한국어] I/O 방향별 카운터 갱신 및 읽기 전용 모드 필터링 */
		if (rw == DDIR_READ)
			reads++;
		else if (rw == DDIR_WRITE) {
			/*
			 * Don't add a write for ro mode
			 */
			if (read_only)
				continue;
			writes++;
		} else if (rw == DDIR_TRIM) {
			/*
			 * Don't add a trim for ro mode
			 */
			if (read_only)
				continue;
			trims++;
		} else if (rw == DDIR_WAIT) {
			if (td->o.no_stall)
				continue;
			waits++;
		} else if (rw == DDIR_INVAL) {
		} else if (ddir_sync(rw)) {
			syncs++;
		} else {
			log_err("bad ddir: %d\n", rw);
			continue;
		}

		/*
		 * Make note of file
		 */
		/* [한국어] io_piece를 생성하고 필드를 설정하여 재생 큐에 추가 */
		ipo = calloc(1, sizeof(*ipo));
		init_ipo(ipo);
		ipo->ddir = rw;
		if (td->io_log_version == 3)
			ipo->delay = delay;
		if (rw == DDIR_WAIT) {
			ipo->delay = offset;
		} else {
			/* [한국어] replay_scale 적용: 오프셋을 스케일링 */
			if (td->o.replay_scale)
				ipo->offset = offset / td->o.replay_scale;
			else
				ipo->offset = offset;
			ipo_bytes_align(td->o.replay_align, ipo);

			ipo->len = bytes;
			/* [한국어] 최대 블록 크기 갱신 (나중에 버퍼 재할당 필요) */
			if (rw != DDIR_INVAL && bytes > td->o.max_bs[rw]) {
				realloc = true;
				td->o.max_bs[rw] = bytes;
			}
			ipo->fileno = fileno;
			ipo->file_action = file_action;
			td->o.size += bytes;
		}

		queue_io_piece(td, ipo);

		/* [한국어] 청크 모드: 지정된 항목 수만큼만 읽고 중단 */
		if (td->o.read_iolog_chunked) {
			td->io_log_current++;
			items_to_fetch--;
			if (items_to_fetch == 0)
				break;
		}
	}

	free(str);
	free(act);
	free(rfname);

	/* [한국어] 청크 모드 후처리: highmark/checkmark 갱신 */
	if (td->o.read_iolog_chunked) {
		td->io_log_highmark = td->io_log_current;
		td->io_log_checkmark = (td->io_log_highmark + 1) / 2;
		fio_gettime(&td->io_log_highmark_time, NULL);
	}

	if (writes && read_only) {
		log_err("fio: <%s> skips replay of %d writes due to"
			" read-only\n", td->o.name, writes);
		writes = 0;
	}
	if (syncs)
		td->flags |= TD_F_SYNCS;

	/* [한국어] 청크 모드 반환: 항목이 0이면 EOF, 버퍼 재할당 필요 시 처리 */
	if (td->o.read_iolog_chunked) {
		if (td->io_log_current == 0) {
			return false;
		}
		td->o.td_ddir = TD_DDIR_RW;
		if (realloc && td->orig_buffer)
		{
			io_u_quiesce(td);
			free_io_mem(td);
			if (init_io_u_buffers(td))
				return false;
		}
		return true;
	}

	if (!reads && !writes && !waits && !trims)
		return false;

	/* [한국어] 발견된 I/O 방향에 따라 td_ddir 플래그 설정 */
	td->o.td_ddir = 0;
	if (reads)
		td->o.td_ddir |= TD_DDIR_READ;
	if (writes)
		td->o.td_ddir |= TD_DDIR_WRITE;
	if (trims)
		td->o.td_ddir |= TD_DDIR_TRIM;

	return true;
}

/* [한국어] 경로가 Unix 소켓인지 확인 */
static bool is_socket(const char *path)
{
	struct stat buf;
	int r;

	r = stat(path, &buf);
	if (r == -1)
		return false;

	return S_ISSOCK(buf.st_mode);
}

/* [한국어] Unix 소켓에 연결하여 파일 디스크립터 반환 (iolog 소켓 읽기용) */
static int open_socket(const char *path)
{
	struct sockaddr_un addr;
	int ret, fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return fd;

	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) >=
	    sizeof(addr.sun_path)) {
		log_err("%s: path name %s is too long for a Unix socket\n",
			__func__, path);
	}

	ret = connect(fd, (const struct sockaddr *)&addr, strlen(path) + sizeof(addr.sun_family));
	if (!ret)
		return fd;

	close(fd);
	return -1;
}

/*
 * open iolog, check version, and call appropriate parser
 */
/* [한국어] iolog 읽기 초기화: 파일/소켓/stdin을 열고 버전을 확인한 후 파싱 시작
 * 소켓, stdin, 일반 파일 세 가지 소스를 지원한다 */
static bool init_iolog_read(struct thread_data *td, char *fname)
{
	char buffer[256], *p;
	FILE *f = NULL;

	dprint(FD_IO, "iolog: name=%s\n", fname);

	/* [한국어] 소스 종류에 따라 파일 스트림 열기 */
	if (is_socket(fname)) {
		int fd;

		fd = open_socket(fname);
		if (fd >= 0)
			f = fdopen(fd, "r");
	} else if (!strcmp(fname, "-")) {
		f = stdin;
	} else
		f = fopen(fname, "r");

	if (!f) {
		perror("fopen read iolog");
		return false;
	}

	/* [한국어] 첫 줄을 읽어 버전 확인 */
	p = fgets(buffer, sizeof(buffer), f);
	if (!p) {
		td_verror(td, errno, "iolog read");
		log_err("fio: unable to read iolog\n");
		fclose(f);
		return false;
	}

	/*
	 * versions 2 and 3 of the iolog store a specific string as the
	 * first line, check for that
	 */
	/* [한국어] 버전 문자열 비교 - 버전 1은 더 이상 지원하지 않음 */
	if (!strncmp(iolog_ver2, buffer, strlen(iolog_ver2)))
		td->io_log_version = 2;
	else if (!strncmp(iolog_ver3, buffer, strlen(iolog_ver3)))
		td->io_log_version = 3;
	else {
		log_err("fio: iolog version 1 is no longer supported\n");
		fclose(f);
		return false;
	}

	free_release_files(td);
	td->io_log_rfile = f;
	return read_iolog(td);
}

/*
 * Set up a log for storing io patterns.
 */
/* [한국어] iolog 쓰기 초기화: 파일 열기, 버퍼 설정, 버전 헤더 기록, 기존 파일 등록 */
static bool init_iolog_write(struct thread_data *td)
{
	struct fio_file *ff;
	FILE *f;
	unsigned int i;

	f = fopen(td->o.write_iolog_file, "a");
	if (!f) {
		perror("fopen write iolog");
		return false;
	}

	/*
	 * That's it for writing, setup a log buffer and we're done.
	  */
	/* [한국어] 8KB 버퍼로 버퍼링 I/O 설정 */
	td->iolog_f = f;
	td->iolog_buf = malloc(8192);
	setvbuf(f, td->iolog_buf, _IOFBF, 8192);
	fio_gettime(&td->io_log_start_time, NULL);

	/*
	 * write our version line
	 */
	/* [한국어] iolog 버전 3 헤더 기록 */
	if (fprintf(f, "%s\n", iolog_ver3) < 0) {
		perror("iolog init\n");
		return false;
	}

	/*
	 * add all known files
	 */
	/* [한국어] 현재 알려진 모든 파일을 "add" 이벤트로 기록 */
	for_each_file(td, ff, i)
		log_file(td, ff, FIO_LOG_ADD_FILE);

	return true;
}

/* [한국어] iolog 시스템 초기화 진입점
 * read_iolog_file이 설정되면 읽기 모드, write_iolog_file이면 쓰기 모드로 초기화
 * blktrace 형식 파일도 자동 감지하여 처리한다 */
bool init_iolog(struct thread_data *td)
{
	bool ret;

	if (td->o.read_iolog_file) {
		int need_swap;
		char * fname = get_name_by_idx(td->o.read_iolog_file, td->subjob_number);

		/*
		 * Check if it's a blktrace file and load that if possible.
		 * Otherwise assume it's a normal log file and load that.
		 */
		/* [한국어] blktrace 형식인지 확인 후 적절한 초기화 함수 호출 */
		if (is_blktrace(fname, &need_swap)) {
			td->io_log_blktrace = 1;
			ret = init_blktrace_read(td, fname, need_swap);
		} else {
			td->io_log_blktrace = 0;
			ret = init_iolog_read(td, fname);
		}
		free(fname);
	} else if (td->o.write_iolog_file)
		ret = init_iolog_write(td);
	else
		ret = true;

	if (!ret)
		td_verror(td, EINVAL, "failed initializing iolog");

	init_disk_util(td);

	return ret;
}

/* [한국어] 성능 로그(io_log) 초기화 및 설정
 * 파라미터에 따라 로그 타입, 오프셋/우선순위 기록, 압축, 윈도우 평균 등을 설정하고
 * 히스토그램 리스트와 pending 버퍼를 초기화한다 */
void setup_log(struct io_log **log, struct log_params *p,
	       const char *filename)
{
	struct io_log *l;
	int i;
	struct io_u_plat_entry *entry;
	struct flist_head *list;

	/* [한국어] 공유 메모리에 io_log 할당 및 기본 필드 설정 */
	l = scalloc(1, sizeof(*l));
	assert(l);
	INIT_FLIST_HEAD(&l->io_logs);
	l->log_type = p->log_type;
	l->log_offset = p->log_offset;
	l->log_prio = p->log_prio;
	l->log_issue_time = p->log_issue_time;
	l->log_gz = p->log_gz;
	l->log_gz_store = p->log_gz_store;
	l->avg_msec = p->avg_msec;
	l->hist_msec = p->hist_msec;
	l->hist_coarseness = p->hist_coarseness;
	l->filename = strdup(filename);
	l->td = p->td;

	/* Initialize histogram lists for each r/w direction,
	 * with initial io_u_plat of all zeros:
	 */
	/* [한국어] R/W/TRIM 각 방향의 히스토그램 리스트를 초기화 (제로 플랫 엔트리 삽입) */
	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		list = &l->hist_window[i].list;
		INIT_FLIST_HEAD(list);
		entry = calloc(1, sizeof(struct io_u_plat_entry));
		flist_add(&entry->list, list);
	}

	/* [한국어] offload 모드가 아니면 pending 버퍼 사전 할당 */
	if (l->td && l->td->o.io_submit_mode != IO_MODE_OFFLOAD) {
		unsigned int def_samples = DEF_LOG_ENTRIES;
		struct io_logs *__p;

		__p = calloc(1, sizeof(*l->pending));
		if (l->td->o.iodepth > DEF_LOG_ENTRIES)
			def_samples = roundup_pow2(l->td->o.iodepth);
		__p->max_samples = def_samples;
		__p->log = calloc(__p->max_samples, log_entry_sz(l));
		l->pending = __p;
	}

	/* [한국어] 로그 방향 마스크 비트 설정 */
	if (l->log_offset)
		l->log_ddir_mask = LOG_OFFSET_SAMPLE_BIT;
	if (l->log_prio)
		l->log_ddir_mask |= LOG_PRIO_SAMPLE_BIT;
	/*
	 * The bandwidth-log option generates agg-read_bw.log,
	 * agg-write_bw.log and agg-trim_bw.log for which l->td is NULL.
	 * Check if l->td is valid before dereferencing it.
	 */
	/* [한국어] 집계 로그(td==NULL)에서는 BOTH 모드 확인 불가 → td 유효성 검사 */
	if (l->td && l->td->o.log_max == IO_LOG_SAMPLE_BOTH)
		l->log_ddir_mask |= LOG_AVG_MAX_SAMPLE_BIT;

	if (l->log_issue_time)
		l->log_ddir_mask |= LOG_ISSUE_TIME_SAMPLE_BIT;

	INIT_FLIST_HEAD(&l->chunk_list);

	/* [한국어] 압축 설정: 집계 로그이면 압축 비활성화, 아니면 뮤텍스 초기화 */
	if (l->log_gz && !p->td)
		l->log_gz = 0;
	else if (l->log_gz || l->log_gz_store) {
		mutex_init_pshared(&l->chunk_lock);
		mutex_init_pshared(&l->deferred_free_lock);
		p->td->flags |= TD_F_COMPRESS_LOG;
	}

	*log = l;
}

#ifdef CONFIG_SETVBUF
/* [한국어] 파일 스트림에 1MB 버퍼 설정 (대용량 로그 출력 성능 향상) */
static void *set_file_buffer(FILE *f)
{
	size_t size = 1048576;
	void *buf;

	buf = malloc(size);
	setvbuf(f, buf, _IOFBF, size);
	return buf;
}

/* [한국어] 파일 버퍼 해제 */
static void clear_file_buffer(void *buf)
{
	free(buf);
}
#else
/* [한국어] CONFIG_SETVBUF 미지원 시 빈 구현 */
static void *set_file_buffer(FILE *f)
{
	return NULL;
}

static void clear_file_buffer(void *buf)
{
}
#endif

/* [한국어] io_log 메모리 전체 해제 - 모든 로그 청크와 pending 버퍼 포함 */
void free_log(struct io_log *log)
{
	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);
		free(cur_log->log);
		sfree(cur_log);
	}

	if (log->pending) {
		free(log->pending->log);
		free(log->pending);
		log->pending = NULL;
	}

	free(log->pending);
	free(log->filename);
	sfree(log);
}

/* [한국어] 히스토그램 구간 합산 - stride 크기만큼의 버킷을 합산
 * io_u_plat_last가 있으면 차이값(현재-이전)을, 없으면 절대값을 반환 */
uint64_t hist_sum(int j, int stride, uint64_t *io_u_plat,
		uint64_t *io_u_plat_last)
{
	uint64_t sum;
	int k;

	if (io_u_plat_last) {
		for (k = sum = 0; k < stride; k++)
			sum += io_u_plat[j + k] - io_u_plat_last[j + k];
	} else {
		for (k = sum = 0; k < stride; k++)
			sum += io_u_plat[j + k];
	}

	return sum;
}

/* [한국어] 히스토그램 샘플을 파일에 출력
 * 각 샘플에 대해 시간, 방향, 블록크기, 각 버킷의 합산값을 CSV 형식으로 기록 */
static void flush_hist_samples(FILE *f, int hist_coarseness, void *samples,
			       uint64_t sample_size)
{
	struct io_sample *s;
	bool log_offset, log_issue_time;
	uint64_t i, j, nr_samples;
	struct io_u_plat_entry *entry, *entry_before;
	uint64_t *io_u_plat;
	uint64_t *io_u_plat_before;

	int stride = 1 << hist_coarseness;  /* [한국어] 히스토그램 해상도: 2^coarseness개 버킷을 하나로 */

	if (!sample_size)
		return;

	/* [한국어] 첫 샘플에서 로그 플래그 확인 */
	s = __get_sample(samples, 0, 0, 0);
	log_offset = (s->__ddir & LOG_OFFSET_SAMPLE_BIT) != 0;
	log_issue_time = (s->__ddir & LOG_ISSUE_TIME_SAMPLE_BIT) != 0;

	nr_samples = sample_size / __log_entry_sz(log_offset, log_issue_time);

	for (i = 0; i < nr_samples; i++) {
		s = __get_sample(samples, log_offset, log_issue_time, i);

		entry = s->data.plat_entry;
		io_u_plat = entry->io_u_plat;

		/* [한국어] 이전 스냅샷과의 차이를 계산하여 델타 히스토그램 출력 */
		entry_before = flist_first_entry(&entry->list, struct io_u_plat_entry, list);
		io_u_plat_before = entry_before->io_u_plat;

		fprintf(f, "%lu, %u, %llu, ", (unsigned long) s->time,
						io_sample_ddir(s), (unsigned long long) s->bs);
		for (j = 0; j < FIO_IO_U_PLAT_NR - stride; j += stride) {
			fprintf(f, "%llu, ", (unsigned long long)
			        hist_sum(j, stride, io_u_plat, io_u_plat_before));
		}
		fprintf(f, "%llu\n", (unsigned long long)
		        hist_sum(FIO_IO_U_PLAT_NR - stride, stride, io_u_plat,
					io_u_plat_before));

		flist_del(&entry_before->list);
		free(entry_before);
	}
}

/* [한국어] 샘플 출력 필드를 버퍼에 포맷팅하는 헬퍼 함수 */
static int print_sample_fields(char **p, size_t *left, const char *fmt, ...) {
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsnprintf(*p, *left, fmt, ap);
	if (ret < 0 || ret >= *left) {
		log_err("sample file write failed: %d\n", ret);
		va_end(ap);
		return -1;
	}
	va_end(ap);

	*p += ret;
	*left -= ret;

	return 0;
}

/*
 * flush_samples - Generate output for log samples
 * Each sample output is built using a temporary buffer. This buffer size
 * assumptions are:
 * - Each sample has less than 10 fields
 * - Each sample field fits in 25 characters (20 digits for 64 bit number
 *   and a few bytes delimiter)
 */
/* [한국어] 일반(비-히스토그램) 로그 샘플을 파일에 출력
 * 각 샘플을 임시 버퍼에 조립한 후 한 줄로 기록
 * 플래그에 따라 오프셋, 우선순위, 평균+최대, 발행시간 등을 선택적으로 포함 */
void flush_samples(FILE *f, void *samples, uint64_t sample_size)
{
	struct io_sample *s;
	bool log_offset, log_prio, log_avg_max, log_issue_time;
	uint64_t i, nr_samples;
	char buf[256];
	char *p;
	size_t left;
	int ret;

	if (!sample_size)
		return;

	/* [한국어] 첫 샘플에서 로그 플래그 비트 추출 */
	s = __get_sample(samples, 0, 0, 0);
	log_offset = (s->__ddir & LOG_OFFSET_SAMPLE_BIT) != 0;
	log_prio = (s->__ddir & LOG_PRIO_SAMPLE_BIT) != 0;
	log_avg_max = (s->__ddir & LOG_AVG_MAX_SAMPLE_BIT) != 0;
	log_issue_time = (s->__ddir & LOG_ISSUE_TIME_SAMPLE_BIT) != 0;

	nr_samples = sample_size / __log_entry_sz(log_offset, log_issue_time);

	for (i = 0; i < nr_samples; i++) {
		s = __get_sample(samples, log_offset, log_issue_time, i);
		p = buf;
		left = sizeof(buf);

		/* [한국어] 기본 필드: 시간, 값(레이턴시/BW/IOPS) */
		ret = print_sample_fields(&p, &left, "%" PRIu64 ", %" PRId64,
					  s->time, s->data.val.val0);
		if (ret)
			return;

		/* [한국어] BOTH 모드: 최대값도 추가 출력 */
		if (log_avg_max) {
			ret = print_sample_fields(&p, &left, ", %" PRId64,
						  s->data.val.val1);
			if (ret)
				return;
		}

		/* [한국어] 방향, 블록 크기 출력 */
		ret = print_sample_fields(&p, &left, ", %u, %llu",
					  io_sample_ddir(s),
					  (unsigned long long) s->bs);
		if (ret)
			return;

		/* [한국어] 오프셋 포함 시 출력 */
		if (log_offset) {
			ret = print_sample_fields(&p, &left, ", %llu",
						  (unsigned long long) s->aux[IOS_AUX_OFFSET_INDEX]);
			if (ret)
				return;
		}

		/* [한국어] 우선순위: 설정 시 16진수, 아니면 RT 클래스 여부(0/1) */
		if (log_prio)
			ret = print_sample_fields(&p, &left, ", 0x%04x",
						  s->priority);
		else
			ret = print_sample_fields(&p, &left, ", %u",
						  ioprio_value_is_class_rt(s->priority));
		if (ret)
			return;

		/* [한국어] 발행 시간 포함 시 출력 */
		if (log_issue_time) {
			ret = print_sample_fields(&p, &left, ", %llu",
						  (unsigned long long) s->aux[IOS_AUX_ISSUE_TIME_INDEX]);
			if (ret)
				return;
		}

		fprintf(f, "%s\n", buf);
	}
}

#ifdef CONFIG_ZLIB

/* [한국어] 로그 압축/해제 작업 데이터 구조체 */
struct iolog_flush_data {
	struct workqueue_work work;    /* 워크큐 작업 항목 */
	struct io_log *log;            /* 대상 로그 */
	void *samples;                 /* 압축할 샘플 데이터 */
	uint32_t nr_samples;           /* 샘플 수 */
	bool free;                     /* 완료 후 자동 해제 여부 */
};

#define GZ_CHUNK	131072  /* [한국어] 압축 청크 크기: 128KB */

/* [한국어] 새 압축 청크를 할당하고 초기화 */
static struct iolog_compress *get_new_chunk(unsigned int seq)
{
	struct iolog_compress *c;

	c = malloc(sizeof(*c));
	INIT_FLIST_HEAD(&c->list);
	c->buf = malloc(GZ_CHUNK);
	c->len = 0;
	c->seq = seq;
	return c;
}

/* [한국어] 압축 청크 메모리 해제 */
static void free_chunk(struct iolog_compress *ic)
{
	free(ic->buf);
	free(ic);
}

/* [한국어] zlib inflate 스트림 초기화 (해제용)
 * gz_hdr가 설정되면 gzip 헤더 자동 감지 활성화 */
static int z_stream_init(z_stream *stream, int gz_hdr)
{
	int wbits = 15;

	memset(stream, 0, sizeof(*stream));
	stream->zalloc = Z_NULL;
	stream->zfree = Z_NULL;
	stream->opaque = Z_NULL;
	stream->next_in = Z_NULL;

	/*
	 * zlib magic - add 32 for auto-detection of gz header or not,
	 * if we decide to store files in a gzip friendly format.
	 */
	/* [한국어] wbits에 32를 더하면 gzip/raw 형식을 자동 감지 */
	if (gz_hdr)
		wbits += 32;

	if (inflateInit2(stream, wbits) != Z_OK)
		return 1;

	return 0;
}

/* [한국어] 청크 해제(inflate) 반복 처리를 위한 상태 구조체 */
struct inflate_chunk_iter {
	unsigned int seq;      /* 현재 시퀀스 번호 */
	int err;               /* 에러 코드 */
	void *buf;             /* 해제된 데이터 버퍼 */
	size_t buf_size;       /* 버퍼 할당 크기 */
	size_t buf_used;       /* 버퍼 사용량 */
	size_t chunk_sz;       /* 청크 기본 크기 */
};

/* [한국어] 하나의 시퀀스에 대한 inflate를 완료하고 샘플을 출력 */
static void finish_chunk(z_stream *stream, FILE *f,
			 struct inflate_chunk_iter *iter)
{
	int ret;

	ret = inflateEnd(stream);
	if (ret != Z_OK)
		log_err("fio: failed to end log inflation seq %d (%d)\n",
				iter->seq, ret);

	flush_samples(f, iter->buf, iter->buf_used);
	free(iter->buf);
	iter->buf = NULL;
	iter->buf_size = iter->buf_used = 0;
}

/*
 * Iterative chunk inflation. Handles cases where we cross into a new
 * sequence, doing flush finish of previous chunk if needed.
 */
/* [한국어] 반복적 청크 해제 - 새 시퀀스로 넘어갈 때 이전 청크를 마무리하고
 * 출력 버퍼가 부족하면 자동 확장하며 inflate를 수행 */
static size_t inflate_chunk(struct iolog_compress *ic, int gz_hdr, FILE *f,
			    z_stream *stream, struct inflate_chunk_iter *iter)
{
	size_t ret;

	dprint(FD_COMPRESS, "inflate chunk size=%lu, seq=%u\n",
				(unsigned long) ic->len, ic->seq);

	/* [한국어] 시퀀스가 바뀌면 이전 시퀀스 마무리 후 새 스트림 초기화 */
	if (ic->seq != iter->seq) {
		if (iter->seq)
			finish_chunk(stream, f, iter);

		z_stream_init(stream, gz_hdr);
		iter->seq = ic->seq;
	}

	stream->avail_in = ic->len;
	stream->next_in = ic->buf;

	if (!iter->buf_size) {
		iter->buf_size = iter->chunk_sz;
		iter->buf = malloc(iter->buf_size);
	}

	/* [한국어] 입력 데이터가 남아있는 동안 inflate 반복 */
	while (stream->avail_in) {
		size_t this_out = iter->buf_size - iter->buf_used;
		int err;

		stream->avail_out = this_out;
		stream->next_out = iter->buf + iter->buf_used;

		err = inflate(stream, Z_NO_FLUSH);
		if (err < 0) {
			log_err("fio: failed inflating log: %d\n", err);
			iter->err = err;
			break;
		}

		iter->buf_used += this_out - stream->avail_out;

		/* [한국어] 출력 버퍼 부족 시 확장 */
		if (!stream->avail_out) {
			iter->buf_size += iter->chunk_sz;
			iter->buf = realloc(iter->buf, iter->buf_size);
			continue;
		}

		if (err == Z_STREAM_END)
			break;
	}

	ret = (void *) stream->next_in - ic->buf;

	dprint(FD_COMPRESS, "inflated to size=%lu\n", (unsigned long) iter->buf_size);

	return ret;
}

/*
 * Inflate stored compressed chunks, or write them directly to the log
 * file if so instructed.
 */
/* [한국어] 압축된 청크 리스트를 순회하며 해제(inflate)하거나 바이너리로 직접 기록
 * log_gz_store 모드이면 압축 데이터를 그대로 파일에 쓰고,
 * 아니면 inflate하여 텍스트 샘플로 출력한다 */
static int inflate_gz_chunks(struct io_log *log, FILE *f)
{
	struct inflate_chunk_iter iter = { .chunk_sz = log->log_gz, };
	z_stream stream;

	while (!flist_empty(&log->chunk_list)) {
		struct iolog_compress *ic;

		ic = flist_first_entry(&log->chunk_list, struct iolog_compress, list);
		flist_del(&ic->list);

		if (log->log_gz_store) {
			size_t ret;

			dprint(FD_COMPRESS, "log write chunk size=%lu, "
				"seq=%u\n", (unsigned long) ic->len, ic->seq);

			ret = fwrite(ic->buf, ic->len, 1, f);
			if (ret != 1 || ferror(f)) {
				iter.err = errno;
				log_err("fio: error writing compressed log\n");
			}
		} else
			inflate_chunk(ic, log->log_gz_store, f, &stream, &iter);

		free_chunk(ic);
	}

	if (iter.seq) {
		finish_chunk(&stream, f, &iter);
		free(iter.buf);
	}

	return iter.err;
}

/*
 * Open compressed log file and decompress the stored chunks and
 * write them to stdout. The chunks are stored sequentially in the
 * file, so we iterate over them and do them one-by-one.
 */
/* [한국어] 압축된 로그 파일을 열어 해제하고 stdout으로 출력하는 독립 유틸리티 함수
 * 파일 전체를 메모리에 읽은 후, 순차적으로 청크를 inflate한다 */
int iolog_file_inflate(const char *file)
{
	struct inflate_chunk_iter iter = { .chunk_sz = 64 * 1024 * 1024, };
	struct iolog_compress ic;
	z_stream stream;
	struct stat sb;
	size_t ret;
	size_t total;
	void *buf;
	FILE *f;

	f = fopen(file, "rb");
	if (!f) {
		perror("fopen");
		return 1;
	}

	if (stat(file, &sb) < 0) {
		fclose(f);
		perror("stat");
		return 1;
	}

	ic.buf = buf = malloc(sb.st_size);
	ic.len = sb.st_size;
	ic.seq = 1;

	ret = fread(ic.buf, ic.len, 1, f);
	if (ret == 0 && ferror(f)) {
		perror("fread");
		fclose(f);
		free(buf);
		return 1;
	} else if (ferror(f) || (!feof(f) && ret != 1)) {
		log_err("fio: short read on reading log\n");
		fclose(f);
		free(buf);
		return 1;
	}

	fclose(f);

	/*
	 * Each chunk will return Z_STREAM_END. We don't know how many
	 * chunks are in the file, so we just keep looping and incrementing
	 * the sequence number until we have consumed the whole compressed
	 * file.
	 */
	/* [한국어] 각 청크는 Z_STREAM_END를 반환 → 시퀀스 번호 증가하며 전체 소비 */
	total = ic.len;
	do {
		size_t iret;

		iret = inflate_chunk(&ic,  1, stdout, &stream, &iter);
		total -= iret;
		if (!total)
			break;
		if (iter.err)
			break;

		ic.seq++;
		ic.len -= iret;
		ic.buf += iret;
	} while (1);

	if (iter.seq) {
		finish_chunk(&stream, stdout, &iter);
		free(iter.buf);
	}

	free(buf);
	return iter.err;
}

#else

/* [한국어] CONFIG_ZLIB 미정의 시 빈 구현 (압축 비지원) */
static int inflate_gz_chunks(struct io_log *log, FILE *f)
{
	return 0;
}

int iolog_file_inflate(const char *file)
{
	log_err("fio: log inflation not possible without zlib\n");
	return 1;
}

#endif

/* [한국어] 로그를 파일에 기록 (flush)
 * 1) 압축 청크가 있으면 inflate하여 기록
 * 2) 남은 로그 엔트리를 순차적으로 기록
 * do_append가 true이면 파일에 추가(append), false이면 새로 작성 */
void flush_log(struct io_log *log, bool do_append)
{
	void *buf;
	FILE *f;

	/*
	 * If log_gz_store is true, we are writing a binary file.
	 * Set the mode appropriately (on all platforms) to avoid issues
	 * on windows (line-ending conversions, etc.)
	 */
	/* [한국어] gz_store 모드이면 바이너리 모드("wb"/"ab")로 열기 */
	if (!do_append)
		if (log->log_gz_store)
			f = fopen(log->filename, "wb");
		else
			f = fopen(log->filename, "w");
	else
		if (log->log_gz_store)
			f = fopen(log->filename, "ab");
		else
			f = fopen(log->filename, "a");
	if (!f) {
		perror("fopen log");
		return;
	}

	buf = set_file_buffer(f);

	/* [한국어] 압축 청크를 먼저 처리 (inflate 또는 바이너리 기록) */
	inflate_gz_chunks(log, f);

	/* [한국어] 남은 로그 청크를 순차적으로 출력 */
	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);

		/* [한국어] 히스토그램 로그이면 히스토그램 형식, 아니면 일반 형식으로 출력 */
		if (log->td && log == log->td->clat_hist_log)
			flush_hist_samples(f, log->hist_coarseness, cur_log->log,
			                   log_sample_sz(log, cur_log));
		else
			flush_samples(f, cur_log->log, log_sample_sz(log, cur_log));

		sfree(cur_log);
	}

	fclose(f);
	clear_file_buffer(buf);
}

/* [한국어] 로그 기록 완료 처리: 압축 플러시 → 파일 잠금 → 전송/파일기록 → 잠금 해제 → 해제
 * trylock이 true이면 비차단 잠금 시도 (실패 시 1 반환) */
static int finish_log(struct thread_data *td, struct io_log *log, int trylock)
{
	if (td->flags & TD_F_COMPRESS_LOG)
		iolog_flush(log);

	if (trylock) {
		if (fio_trylock_file(log->filename))
			return 1;
	} else
		fio_lock_file(log->filename);

	/* [한국어] GUI/백엔드 모드이면 네트워크로 전송, 아니면 파일에 기록 */
	if (td->client_type == FIO_CLIENT_TYPE_GUI || is_backend)
		fio_send_iolog(td, log, log->filename);
	else
		flush_log(log, !td->o.per_job_logs);

	fio_unlock_file(log->filename);
	free_log(log);
	return 0;
}

/* [한국어] 압축 청크 리스트의 총 크기(바이트)를 반환 */
size_t log_chunk_sizes(struct io_log *log)
{
	struct flist_head *entry;
	size_t ret;

	if (flist_empty(&log->chunk_list))
		return 0;

	ret = 0;
	pthread_mutex_lock(&log->chunk_lock);
	flist_for_each(entry, &log->chunk_list) {
		struct iolog_compress *c;

		c = flist_entry(entry, struct iolog_compress, list);
		ret += c->len;
	}
	pthread_mutex_unlock(&log->chunk_lock);
	return ret;
}

#ifdef CONFIG_ZLIB

/* [한국어] 지연 해제 큐에 포인터 추가 (압축 진행 중 안전한 메모리 해제를 위해) */
static void iolog_put_deferred(struct io_log *log, void *ptr)
{
	if (!ptr)
		return;

	pthread_mutex_lock(&log->deferred_free_lock);
	if (log->deferred < IOLOG_MAX_DEFER) {
		log->deferred_items[log->deferred] = ptr;
		log->deferred++;
	} else if (!fio_did_warn(FIO_WARN_IOLOG_DROP))
		log_err("fio: had to drop log entry free\n");
	pthread_mutex_unlock(&log->deferred_free_lock);
}

/* [한국어] 지연 해제 큐에 쌓인 모든 포인터를 실제 해제 */
static void iolog_free_deferred(struct io_log *log)
{
	int i;

	if (!log->deferred)
		return;

	pthread_mutex_lock(&log->deferred_free_lock);

	for (i = 0; i < log->deferred; i++) {
		free(log->deferred_items[i]);
		log->deferred_items[i] = NULL;
	}

	log->deferred = 0;
	pthread_mutex_unlock(&log->deferred_free_lock);
}

/* [한국어] zlib deflate를 사용하여 로그 샘플을 압축하는 핵심 작업 함수
 * 입력 샘플을 GZ_CHUNK 크기의 청크들로 분할 압축하여 chunk_list에 추가
 * 압축 완료 후 원본 샘플은 지연 해제 큐로 이동 */
static int gz_work(struct iolog_flush_data *data)
{
	struct iolog_compress *c = NULL;
	struct flist_head list;
	unsigned int seq;
	z_stream stream;
	size_t total = 0;
	int ret;

	INIT_FLIST_HEAD(&list);

	/* [한국어] deflate 스트림 초기화 */
	memset(&stream, 0, sizeof(stream));
	stream.zalloc = Z_NULL;
	stream.zfree = Z_NULL;
	stream.opaque = Z_NULL;

	ret = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
	if (ret != Z_OK) {
		log_err("fio: failed to init gz stream\n");
		goto err;
	}

	seq = ++data->log->chunk_seq;

	stream.next_in = (void *) data->samples;
	stream.avail_in = data->nr_samples * log_entry_sz(data->log);

	dprint(FD_COMPRESS, "deflate input size=%lu, seq=%u, log=%s\n",
				(unsigned long) stream.avail_in, seq,
				data->log->filename);
	/* [한국어] 입력 데이터를 청크 단위로 압축 */
	do {
		if (c)
			dprint(FD_COMPRESS, "seq=%d, chunk=%lu\n", seq,
				(unsigned long) c->len);
		c = get_new_chunk(seq);
		stream.avail_out = GZ_CHUNK;
		stream.next_out = c->buf;
		ret = deflate(&stream, Z_NO_FLUSH);
		if (ret < 0) {
			log_err("fio: deflate log (%d)\n", ret);
			free_chunk(c);
			goto err;
		}

		c->len = GZ_CHUNK - stream.avail_out;
		flist_add_tail(&c->list, &list);
		total += c->len;
	} while (stream.avail_in);

	/* [한국어] 마지막 청크에 Z_FINISH로 스트림 종료 */
	stream.next_out = c->buf + c->len;
	stream.avail_out = GZ_CHUNK - c->len;

	ret = deflate(&stream, Z_FINISH);
	if (ret < 0) {
		/*
		 * Z_BUF_ERROR is special, it just means we need more
		 * output space. We'll handle that below. Treat any other
		 * error as fatal.
		 */
		/* [한국어] Z_BUF_ERROR는 출력 공간 부족 → 아래서 추가 청크로 처리 */
		if (ret != Z_BUF_ERROR) {
			log_err("fio: deflate log (%d)\n", ret);
			flist_del(&c->list);
			free_chunk(c);
			goto err;
		}
	}

	total -= c->len;
	c->len = GZ_CHUNK - stream.avail_out;
	total += c->len;
	dprint(FD_COMPRESS, "seq=%d, chunk=%lu\n", seq, (unsigned long) c->len);

	/* [한국어] Z_STREAM_END가 아니면 추가 청크를 할당하여 나머지 압축 */
	if (ret != Z_STREAM_END) {
		do {
			c = get_new_chunk(seq);
			stream.avail_out = GZ_CHUNK;
			stream.next_out = c->buf;
			ret = deflate(&stream, Z_FINISH);
			c->len = GZ_CHUNK - stream.avail_out;
			total += c->len;
			flist_add_tail(&c->list, &list);
			dprint(FD_COMPRESS, "seq=%d, chunk=%lu\n", seq,
				(unsigned long) c->len);
		} while (ret != Z_STREAM_END);
	}

	dprint(FD_COMPRESS, "deflated to size=%lu\n", (unsigned long) total);

	ret = deflateEnd(&stream);
	if (ret != Z_OK)
		log_err("fio: deflateEnd %d\n", ret);

	/* [한국어] 원본 샘플을 지연 해제 큐로 이동 */
	iolog_put_deferred(data->log, data->samples);

	/* [한국어] 압축된 청크 리스트를 로그의 chunk_list에 병합 */
	if (!flist_empty(&list)) {
		pthread_mutex_lock(&data->log->chunk_lock);
		flist_splice_tail(&list, &data->log->chunk_list);
		pthread_mutex_unlock(&data->log->chunk_lock);
	}

	ret = 0;
done:
	if (data->free)
		sfree(data);
	return ret;
err:
	/* [한국어] 에러 시 생성된 청크들을 모두 해제 */
	while (!flist_empty(&list)) {
		c = flist_first_entry(list.next, struct iolog_compress, list);
		flist_del(&c->list);
		free_chunk(c);
	}
	ret = 1;
	goto done;
}

/*
 * Invoked from our compress helper thread, when logging would have exceeded
 * the specified memory limitation. Compresses the previously stored
 * entries.
 */
/* [한국어] 압축 헬퍼 스레드에서 호출되는 비동기 압축 래퍼 */
static int gz_work_async(struct submit_worker *sw, struct workqueue_work *work)
{
	return gz_work(container_of(work, struct iolog_flush_data, work));
}

/* [한국어] 압축 워커 초기화 - CPU 어피니티 설정 */
static int gz_init_worker(struct submit_worker *sw)
{
	struct thread_data *td = sw->wq->td;

	if (!fio_option_is_set(&td->o, log_gz_cpumask))
		return 0;

	if (fio_setaffinity(gettid(), td->o.log_gz_cpumask) == -1) {
		log_err("gz: failed to set CPU affinity\n");
		return 1;
	}

	return 0;
}

/* [한국어] 로그 압축 워크큐 연산(ops) 구조체 */
static struct workqueue_ops log_compress_wq_ops = {
	.fn		= gz_work_async,
	.init_worker_fn	= gz_init_worker,
	.nice		= 1,
};

/* [한국어] 로그 압축 워크큐 초기화 (TD_F_COMPRESS_LOG 플래그 설정 시) */
int iolog_compress_init(struct thread_data *td, struct sk_out *sk_out)
{
	if (!(td->flags & TD_F_COMPRESS_LOG))
		return 0;

	workqueue_init(td, &td->log_compress_wq, &log_compress_wq_ops, 1, sk_out);
	return 0;
}

/* [한국어] 로그 압축 워크큐 종료 */
void iolog_compress_exit(struct thread_data *td)
{
	if (!(td->flags & TD_F_COMPRESS_LOG))
		return;

	workqueue_exit(&td->log_compress_wq);
}

/*
 * Queue work item to compress the existing log entries. We reset the
 * current log to a small size, and reference the existing log in the
 * data that we queue for compression. Once compression has been done,
 * this old log is freed. Will not return until the log compression
 * has completed, and will flush all previous logs too
 */
/* [한국어] 모든 로그 엔트리를 동기적으로 압축하는 함수
 * 워크큐를 플러시한 후, 각 로그 청크를 gz_work()로 압축
 * 압축 완료될 때까지 블로킹한다 */
static int iolog_flush(struct io_log *log)
{
	struct iolog_flush_data *data;

	workqueue_flush(&log->td->log_compress_wq);
	data = malloc(sizeof(*data));
	if (!data)
		return 1;

	data->log = log;
	data->free = false;

	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);

		data->samples = cur_log->log;
		data->nr_samples = cur_log->nr_samples;

		sfree(cur_log);

		gz_work(data);
	}

	free(data);
	return 0;
}

/* [한국어] 현재 로그 청크를 비동기 압축 큐에 제출
 * 청크의 소유권을 워크큐로 넘기고, 지연 해제 큐를 처리 */
int iolog_cur_flush(struct io_log *log, struct io_logs *cur_log)
{
	struct iolog_flush_data *data;

	data = smalloc(sizeof(*data));
	if (!data)
		return 1;

	data->log = log;

	data->samples = cur_log->log;
	data->nr_samples = cur_log->nr_samples;
	data->free = true;

	/* [한국어] 청크를 초기화하여 재사용 가능 상태로 */
	cur_log->nr_samples = cur_log->max_samples = 0;
	cur_log->log = NULL;

	workqueue_enqueue(&log->td->log_compress_wq, &data->work);

	iolog_free_deferred(log);

	return 0;
}
#else

/* [한국어] CONFIG_ZLIB 미정의 시 빈 구현들 */
static int iolog_flush(struct io_log *log)
{
	return 1;
}

int iolog_cur_flush(struct io_log *log, struct io_logs *cur_log)
{
	return 1;
}

int iolog_compress_init(struct thread_data *td, struct sk_out *sk_out)
{
	return 0;
}

void iolog_compress_exit(struct thread_data *td)
{
}

#endif

/* [한국어] 현재(마지막) 로그 청크를 반환, 비어있으면 NULL */
struct io_logs *iolog_cur_log(struct io_log *log)
{
	if (flist_empty(&log->io_logs))
		return NULL;

	return flist_last_entry(&log->io_logs, struct io_logs, list);
}

/* [한국어] 로그 내 전체 샘플 수를 합산하여 반환 */
uint64_t iolog_nr_samples(struct io_log *iolog)
{
	struct flist_head *entry;
	uint64_t ret = 0;

	flist_for_each(entry, &iolog->io_logs) {
		struct io_logs *cur_log;

		cur_log = flist_entry(entry, struct io_logs, list);
		ret += cur_log->nr_samples;
	}

	return ret;
}

/* [한국어] 로그 기록 래퍼 - 로그가 존재하면 finish_log 호출 */
static int __write_log(struct thread_data *td, struct io_log *log, int try)
{
	if (log)
		return finish_log(td, log, try);

	return 0;
}

/* [한국어] IOPS 로그 기록 */
static int write_iops_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (per_unit_log(td->iops_log) != unit_log)
		return 0;

	ret = __write_log(td, td->iops_log, try);
	if (!ret)
		td->iops_log = NULL;

	return ret;
}

/* [한국어] 제출 레이턴시(slat) 로그 기록 */
static int write_slat_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->slat_log, try);
	if (!ret)
		td->slat_log = NULL;

	return ret;
}

/* [한국어] 완료 레이턴시(clat) 로그 기록 */
static int write_clat_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->clat_log, try);
	if (!ret)
		td->clat_log = NULL;

	return ret;
}

/* [한국어] 완료 레이턴시 히스토그램 로그 기록 */
static int write_clat_hist_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->clat_hist_log, try);
	if (!ret)
		td->clat_hist_log = NULL;

	return ret;
}

/* [한국어] 총 레이턴시(lat) 로그 기록 */
static int write_lat_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->lat_log, try);
	if (!ret)
		td->lat_log = NULL;

	return ret;
}

/* [한국어] 대역폭(bandwidth) 로그 기록 */
static int write_bandw_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (per_unit_log(td->bw_log) != unit_log)
		return 0;

	ret = __write_log(td, td->bw_log, try);
	if (!ret)
		td->bw_log = NULL;

	return ret;
}

/* [한국어] 로그 타입별 비트 마스크 - 각 로그의 기록 완료 추적용 */
enum {
	BW_LOG_MASK	= 1,
	LAT_LOG_MASK	= 2,
	SLAT_LOG_MASK	= 4,
	CLAT_LOG_MASK	= 8,
	IOPS_LOG_MASK	= 16,
	CLAT_HIST_LOG_MASK = 32,

	ALL_LOG_NR	= 6,    /* 총 로그 타입 수 */
};

/* [한국어] 로그 타입과 기록 함수의 매핑 구조체 */
struct log_type {
	unsigned int mask;
	int (*fn)(struct thread_data *, int, bool);
};

/* [한국어] 모든 로그 타입의 마스크-함수 매핑 테이블 */
static struct log_type log_types[] = {
	{
		.mask	= BW_LOG_MASK,
		.fn	= write_bandw_log,
	},
	{
		.mask	= LAT_LOG_MASK,
		.fn	= write_lat_log,
	},
	{
		.mask	= SLAT_LOG_MASK,
		.fn	= write_slat_log,
	},
	{
		.mask	= CLAT_LOG_MASK,
		.fn	= write_clat_log,
	},
	{
		.mask	= IOPS_LOG_MASK,
		.fn	= write_iops_log,
	},
	{
		.mask	= CLAT_HIST_LOG_MASK,
		.fn	= write_clat_hist_log,
	}
};

/* [한국어] 특정 스레드의 모든 로그를 파일에 기록
 * 각 로그 타입을 순회하며 기록 시도, 파일 잠금 충돌 시 재시도
 * 모든 로그가 기록 완료될 때까지 반복한다 */
void td_writeout_logs(struct thread_data *td, bool unit_logs)
{
	unsigned int log_mask = 0;
	unsigned int log_left = ALL_LOG_NR;
	int old_state, i;

	/* [한국어] 스레드 상태를 FINISHING으로 전환 */
	old_state = td_bump_runstate(td, TD_FINISHING);

	finalize_logs(td, unit_logs);

	/* [한국어] 모든 로그 타입이 기록될 때까지 반복 */
	while (log_left) {
		int prev_log_left = log_left;

		for (i = 0; i < ALL_LOG_NR && log_left; i++) {
			struct log_type *lt = &log_types[i];
			int ret;

			if (!(log_mask & lt->mask)) {
				ret = lt->fn(td, log_left != 1, unit_logs);
				if (!ret) {
					log_left--;
					log_mask |= lt->mask;
				}
			}
		}

		/* [한국어] 진전이 없으면 5ms 대기 후 재시도 (파일 잠금 충돌 해소 대기) */
		if (prev_log_left == log_left)
			usleep(5000);
	}

	td_restore_runstate(td, old_state);
}

/* [한국어] 모든 스레드의 로그를 파일에 기록 */
void fio_writeout_logs(bool unit_logs)
{
	for_each_td(td) {
		td_writeout_logs(td, unit_logs);
	} end_for_each();
}
