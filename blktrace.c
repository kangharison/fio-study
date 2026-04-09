/*
 * blktrace support code for fio
 */
/*
 * [한국어] blktrace.c - blktrace 지원 코드
 *
 * 이 파일은 Linux blktrace 바이너리 파일을 읽고 재생하는 기능을 구현한다.
 * blktrace는 블록 레이어의 I/O 이벤트를 기록하는 도구이며, fio는 이 데이터를
 * 읽어 실제 I/O 패턴을 재현(replay)할 수 있다.
 *
 * 주요 기능:
 *   1) is_blktrace()        - 파일이 blktrace 바이너리 형식인지 매직 넘버로 판별
 *   2) init_blktrace_read() - blktrace 파일을 열고 io_piece 리스트로 로드
 *   3) read_blktrace()      - 트레이스 항목을 순차적으로 읽어 I/O 작업(io_piece)으로 변환
 *   4) merge_blktrace_iologs() - 여러 blktrace 파일을 시간순으로 병합
 *
 * 트레이스 재생 흐름:
 *   blk_io_trace 읽기 -> 바이트 스왑(필요시) -> 매직/버전 검증 ->
 *   queue_trace()로 I/O 유형 분류 -> store_ipo()로 io_piece 저장 ->
 *   fio 메인 루프에서 순차 재생
 */

/* 표준 라이브러리 헤더 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/sysmacros.h>   /* major/minor 매크로 */

/* fio 내부 헤더 */
#include "flist.h"           /* fio 연결 리스트 */
#include "fio.h"             /* fio 핵심 구조체 */
#include "iolog.h"           /* I/O 로그 관련 */
#include "blktrace.h"        /* blktrace API 선언 */
#include "blktrace_api.h"    /* blk_io_trace 구조체, 매직 넘버 */
#include "oslib/linux-dev-lookup.h" /* 디바이스 이름 조회 */

/*
 * [한국어] file_cache - 디바이스 파일 번호 캐시 구조체
 *
 * 동일한 디바이스에 대한 반복적인 탐색을 피하기 위해
 * 마지막으로 조회한 major/minor 번호와 파일 번호를 캐시한다.
 */
struct file_cache {
	unsigned int maj;      /* 마지막 조회한 major 디바이스 번호 */
	unsigned int min;      /* 마지막 조회한 minor 디바이스 번호 */
	unsigned int fileno;   /* 해당 디바이스의 fio 파일 인덱스 */
};

/*
 * Just discard the pdu by seeking past it.
 */
/* [한국어] PDU(Protocol Data Unit)를 건너뜀. blk_io_trace 뒤에 붙는 추가 데이터를 무시 */
static int discard_pdu(FILE* f, struct blk_io_trace *t)
{
	if (t->pdu_len == 0)
		return 0;

	dprint(FD_BLKTRACE, "discard pdu len %u\n", t->pdu_len);
	if (fseek(f, t->pdu_len, SEEK_CUR) < 0)
		return -errno;

	return t->pdu_len;
}

/*
 * Check if this is a blktrace binary data file. We read a single trace
 * into memory and check for the magic signature.
 */
/* [한국어] 파일이 blktrace 바이너리 형식인지 매직 넘버(BLK_IO_TRACE_MAGIC)로 확인 */
bool is_blktrace(const char *filename, int *need_swap)
{
	struct blk_io_trace t;
	int fd, ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return false;

	ret = read(fd, &t, sizeof(t));
	close(fd);

	if (ret < 0) {
		perror("read blktrace");
		return false;
	} else if (ret != sizeof(t)) {
		log_err("fio: short read on blktrace file\n");
		return false;
	}

	/* 매직 넘버의 상위 24비트가 일치하면 blktrace 파일 */
	if ((t.magic & 0xffffff00) == BLK_IO_TRACE_MAGIC) {
		*need_swap = 0;
		return true;
	}

	/*
	 * Maybe it needs to be endian swapped...
	 */
	/* [한국어] 엔디안이 다른 시스템에서 생성된 파일일 수 있으므로 바이트 스왑 후 재확인 */
	t.magic = fio_swap32(t.magic);
	if ((t.magic & 0xffffff00) == BLK_IO_TRACE_MAGIC) {
		*need_swap = 1;
		return true;
	}

	return false;
}

/* [한국어] 디바이스 번호에서 major/minor를 추출하는 매크로 (커널과 다른 비트 레이아웃 사용) */
#define FMINORBITS	20
#define FMINORMASK	((1U << FMINORBITS) - 1)
#define FMAJOR(dev)	((unsigned int) ((dev) >> FMINORBITS))
#define FMINOR(dev)	((unsigned int) ((dev) & FMINORMASK))

/* [한국어] 파일 열기/닫기 이벤트를 io_piece로 추가하여 재생 시 파일 관리에 사용 */
static void trace_add_open_close_event(struct thread_data *td, int fileno, enum file_log_act action)
{
	struct io_piece *ipo;

	ipo = calloc(1, sizeof(*ipo));
	init_ipo(ipo);

	ipo->ddir = DDIR_INVAL;
	ipo->fileno = fileno;
	ipo->file_action = action;
	flist_add_tail(&ipo->list, &td->io_log_list);
}

/*
 * [한국어] 트레이스에 나온 디바이스를 fio의 파일 목록에 추가
 *
 * 캐시를 먼저 확인하고, 캐시 미스 시 기존 파일 목록을 탐색한다.
 * 없으면 디바이스 이름을 조회하여 새 파일로 등록한다.
 * replay_redirect 옵션이 설정되면 원본 디바이스 대신 지정된 디바이스로 리다이렉트한다.
 */
static int trace_add_file(struct thread_data *td, __u32 device,
			  struct file_cache *cache)
{
	unsigned int maj = FMAJOR(device);
	unsigned int min = FMINOR(device);
	struct fio_file *f;
	char dev[256];
	unsigned int i;

	/* 캐시 히트: 동일한 디바이스면 바로 반환 */
	if (cache->maj == maj && cache->min == min)
		return cache->fileno;

	cache->maj = maj;
	cache->min = min;

	/*
	 * check for this file in our list
	 */
	/* [한국어] 이미 등록된 파일인지 major/minor로 탐색 */
	for_each_file(td, f, i)
		if (f->major == maj && f->minor == min) {
			cache->fileno = f->fileno;
			return cache->fileno;
		}

	strcpy(dev, "/dev");
	if (blktrace_lookup_device(td->o.replay_redirect, dev, maj, min)) {
		int fileno;

		if (td->o.replay_redirect)
			dprint(FD_BLKTRACE, "device lookup: %d/%d\n overridden"
					" with: %s\n", maj, min,
					td->o.replay_redirect);
		else
			dprint(FD_BLKTRACE, "device lookup: %d/%d\n", maj, min);

		dprint(FD_BLKTRACE, "add devices %s\n", dev);
		fileno = add_file_exclusive(td, dev);
		td->o.open_files++;
		td->files[fileno]->major = maj;
		td->files[fileno]->minor = min;
		trace_add_open_close_event(td, fileno, FIO_LOG_OPEN_FILE);
		cache->fileno = fileno;
	}

	return cache->fileno;
}

/* [한국어] I/O 크기를 replay_align 옵션에 맞게 정렬(올림) */
static void t_bytes_align(struct thread_options *o, struct blk_io_trace *t)
{
	if (!o->replay_align)
		return;

	t->bytes = (t->bytes + o->replay_align - 1) & ~(o->replay_align - 1);
}

/*
 * Store blk_io_trace data in an ipo for later retrieval.
 */
/*
 * [한국어] blk_io_trace 데이터를 io_piece(ipo)로 변환하여 저장
 *
 * 섹터 번호를 바이트 오프셋으로 변환하고, replay_scale로 축소하며,
 * 시간 지연(delay)을 나노초에서 마이크로초로 변환한다.
 */
static void store_ipo(struct thread_data *td, unsigned long long offset,
		      unsigned int bytes, int rw, unsigned long long ttime,
		      int fileno)
{
	struct io_piece *ipo;

	ipo = calloc(1, sizeof(*ipo));
	init_ipo(ipo);

	ipo->offset = offset * 512;           /* 섹터 -> 바이트 변환 */
	if (td->o.replay_scale)
		ipo->offset = ipo->offset / td->o.replay_scale; /* 오프셋 스케일링 */
	ipo_bytes_align(td->o.replay_align, ipo);
	ipo->len = bytes;
	ipo->delay = ttime / 1000;            /* 나노초 -> 마이크로초 변환 */
	if (rw)
		ipo->ddir = DDIR_WRITE;
	else
		ipo->ddir = DDIR_READ;
	ipo->fileno = fileno;

	dprint(FD_BLKTRACE, "store ddir=%d, off=%llu, len=%lu, delay=%lu\n",
							ipo->ddir, ipo->offset,
							ipo->len, ipo->delay);
	queue_io_piece(td, ipo);
}

/* [한국어] 알림(notify) 타입 트레이스 처리 - 프로세스/타임스탬프/메시지 알림을 디버그 로깅 */
static bool handle_trace_notify(struct blk_io_trace *t)
{
	switch (t->action) {
	case BLK_TN_PROCESS:
		dprint(FD_BLKTRACE, "got process notify: %x, %d\n",
				t->action, t->pid);
		break;
	case BLK_TN_TIMESTAMP:
		dprint(FD_BLKTRACE, "got timestamp notify: %x, %d\n",
				t->action, t->pid);
		break;
	case BLK_TN_MESSAGE:
		break;
	default:
		dprint(FD_BLKTRACE, "unknown trace act %x\n", t->action);
		break;
	}
	return false;
}

/* [한국어] DISCARD(TRIM) 트레이스 처리 - TRIM 요청을 io_piece로 저장 */
static bool handle_trace_discard(struct thread_data *td,
				 struct blk_io_trace *t,
				 unsigned long long ttime,
				 unsigned long *ios, unsigned long long *bs,
				 struct file_cache *cache)
{
	struct io_piece *ipo;
	int fileno;

	/* replay_skip 비트마스크로 TRIM 건너뛰기 설정 확인 */
	if (td->o.replay_skip & (1u << DDIR_TRIM))
		return false;

	ipo = calloc(1, sizeof(*ipo));
	init_ipo(ipo);
	fileno = trace_add_file(td, t->device, cache);

	ios[DDIR_TRIM]++;
	if (t->bytes > bs[DDIR_TRIM])
		bs[DDIR_TRIM] = t->bytes;  /* 최대 블록 크기 갱신 */

	td->o.size += t->bytes;

	INIT_FLIST_HEAD(&ipo->list);

	ipo->offset = t->sector * 512;       /* 섹터 -> 바이트 */
	if (td->o.replay_scale)
		ipo->offset = ipo->offset / td->o.replay_scale;
	ipo_bytes_align(td->o.replay_align, ipo);
	ipo->len = t->bytes;
	ipo->delay = ttime / 1000;
	ipo->ddir = DDIR_TRIM;
	ipo->fileno = fileno;

	dprint(FD_BLKTRACE, "store discard, off=%llu, len=%lu, delay=%lu\n",
							ipo->offset, ipo->len,
							ipo->delay);
	queue_io_piece(td, ipo);
	return true;
}

/* [한국어] 크기가 0인 트레이스를 무시하면서 경고 출력 */
static void dump_trace(struct blk_io_trace *t)
{
	log_err("blktrace: ignoring zero byte trace: action=%x\n", t->action);
}

/*
 * [한국어] 파일시스템(읽기/쓰기) 트레이스 처리
 *
 * BLK_TC_WRITE 플래그로 읽기/쓰기를 구분하고,
 * replay_skip으로 건너뛰기 여부를 확인한 뒤 store_ipo()로 저장한다.
 */
static bool handle_trace_fs(struct thread_data *td, struct blk_io_trace *t,
			    unsigned long long ttime, unsigned long *ios,
			    unsigned long long *bs, struct file_cache *cache)
{
	int rw;
	int fileno;

	fileno = trace_add_file(td, t->device, cache);

	rw = (t->action & BLK_TC_ACT(BLK_TC_WRITE)) != 0;

	if (rw) {
		if (td->o.replay_skip & (1u << DDIR_WRITE))
			return false;
	} else {
		if (td->o.replay_skip & (1u << DDIR_READ))
			return false;
	}

	if (!t->bytes) {
		if (!fio_did_warn(FIO_WARN_BTRACE_ZERO))
			dump_trace(t);
		return false;
	}

	if (t->bytes > bs[rw])
		bs[rw] = t->bytes;   /* 방향별 최대 블록 크기 갱신 */

	ios[rw]++;
	td->o.size += t->bytes;
	store_ipo(td, t->sector, t->bytes, rw, ttime, fileno);
	return true;
}

/* [한국어] FLUSH(동기화) 트레이스 처리 - 캐시 플러시 요청을 io_piece로 저장 */
static bool handle_trace_flush(struct thread_data *td, struct blk_io_trace *t,
			       unsigned long long ttime, unsigned long *ios,
			       struct file_cache *cache)
{
	struct io_piece *ipo;
	int fileno;

	if (td->o.replay_skip & (1u << DDIR_SYNC))
		return false;

	ipo = calloc(1, sizeof(*ipo));
	init_ipo(ipo);
	fileno = trace_add_file(td, t->device, cache);

	ipo->delay = ttime / 1000;
	ipo->ddir = DDIR_SYNC;
	ipo->fileno = fileno;

	ios[DDIR_SYNC]++;
	dprint(FD_BLKTRACE, "store flush delay=%lu\n", ipo->delay);

	/* TD_F_SYNCS 플래그 설정: 이 작업에 sync 연산이 포함됨을 표시 */
	if (!(td->flags & TD_F_SYNCS))
		td->flags |= TD_F_SYNCS;

	queue_io_piece(td, ipo);
	return true;
}

/*
 * We only care for queue traces, most of the others are side effects
 * due to internal workings of the block layer.
 */
/*
 * [한국어] 트레이스 항목을 큐잉 단계에서 처리
 *
 * __BLK_TA_QUEUE 액션만 처리한다. 블록 레이어의 다른 액션(merge, complete 등)은
 * 내부 동작의 부수효과이므로 무시한다.
 * 이전 트레이스와의 시간 차이(delay)를 계산하고, 트레이스 유형에 따라
 * notify/discard/flush/fs 핸들러로 분기한다.
 */
static bool queue_trace(struct thread_data *td, struct blk_io_trace *t,
			 unsigned long *ios, unsigned long long *bs,
			 struct file_cache *cache)
{
	unsigned long long *last_ttime = &td->io_log_last_ttime;
	unsigned long long delay = 0;

	if ((t->action & 0xffff) != __BLK_TA_QUEUE)
		return false;

	/* notify가 아닌 경우에만 시간 지연 계산 */
	if (!(t->action & BLK_TC_ACT(BLK_TC_NOTIFY))) {
		delay = delay_since_ttime(td, t->time);
		*last_ttime = t->time;
	}

	t_bytes_align(&td->o, t);

	/* 트레이스 유형별 핸들러로 분기 */
	if (t->action & BLK_TC_ACT(BLK_TC_NOTIFY))
		return handle_trace_notify(t);
	else if (t->action & BLK_TC_ACT(BLK_TC_DISCARD))
		return handle_trace_discard(td, t, delay, ios, bs, cache);
	else if (t->action & BLK_TC_ACT(BLK_TC_FLUSH))
		return handle_trace_flush(td, t, delay, ios, cache);
	else
		return handle_trace_fs(td, t, delay, ios, bs, cache);
}

/* [한국어] blk_io_trace 구조체의 모든 필드를 엔디안 바이트 스왑 */
static void byteswap_trace(struct blk_io_trace *t)
{
	t->magic = fio_swap32(t->magic);
	t->sequence = fio_swap32(t->sequence);
	t->time = fio_swap64(t->time);
	t->sector = fio_swap64(t->sector);
	t->bytes = fio_swap32(t->bytes);
	t->action = fio_swap32(t->action);
	t->pid = fio_swap32(t->pid);
	t->device = fio_swap32(t->device);
	t->cpu = fio_swap32(t->cpu);
	t->error = fio_swap16(t->error);
	t->pdu_len = fio_swap16(t->pdu_len);
}

/* [한국어] 트레이스가 쓰기 또는 DISCARD 연산인지 확인 */
static bool t_is_write(struct blk_io_trace *t)
{
	return (t->action & BLK_TC_ACT(BLK_TC_WRITE | BLK_TC_DISCARD)) != 0;
}

/* [한국어] 트레이스의 I/O 방향(READ/WRITE/TRIM)을 fio_ddir로 변환 */
static enum fio_ddir t_get_ddir(struct blk_io_trace *t)
{
	if (t->action & BLK_TC_ACT(BLK_TC_READ))
		return DDIR_READ;
	else if (t->action & BLK_TC_ACT(BLK_TC_WRITE))
		return DDIR_WRITE;
	else if (t->action & BLK_TC_ACT(BLK_TC_DISCARD))
		return DDIR_TRIM;

	return DDIR_INVAL;
}

/* [한국어] I/O 큐잉 시 해당 방향의 큐 깊이(depth) 증가 */
static void depth_inc(struct blk_io_trace *t, int *depth)
{
	enum fio_ddir ddir;

	ddir = t_get_ddir(t);
	if (ddir != DDIR_INVAL)
		depth[ddir]++;
}

/* [한국어] I/O 병합(merge) 시 해당 방향의 큐 깊이 감소 */
static void depth_dec(struct blk_io_trace *t, int *depth)
{
	enum fio_ddir ddir;

	ddir = t_get_ddir(t);
	if (ddir != DDIR_INVAL)
		depth[ddir]--;
}

/* [한국어] I/O 완료(complete) 시 최대 큐 깊이를 갱신하고 현재 깊이를 리셋 */
static void depth_end(struct blk_io_trace *t, int *this_depth, int *depth)
{
	enum fio_ddir ddir = DDIR_INVAL;

	ddir = t_get_ddir(t);
	if (ddir != DDIR_INVAL) {
		depth[ddir] = max(depth[ddir], this_depth[ddir]);
		this_depth[ddir] = 0;
	}
}

/*
 * Load a blktrace file by reading all the blk_io_trace entries, and storing
 * them as io_pieces like the fio text version would do.
 */
/*
 * [한국어] blktrace 파일 읽기 초기화
 *
 * 파일을 열고, read_blktrace()를 호출하여 모든 트레이스를 io_piece로 변환한다.
 * 성공하면 td->files_index에 디바이스 파일들이 등록된 상태가 된다.
 */
bool init_blktrace_read(struct thread_data *td, const char *filename, int need_swap)
{
	int old_state;

	td->io_log_rfile = fopen(filename, "rb");
	if (!td->io_log_rfile) {
		td_verror(td, errno, "open blktrace file");
		goto err;
	}
	td->io_log_blktrace_swap = need_swap;
	td->io_log_last_ttime = 0;
	td->o.size = 0;

	free_release_files(td);

	old_state = td_bump_runstate(td, TD_SETTING_UP);

	if (!read_blktrace(td)) {
		goto err;
	}

	td_restore_runstate(td, old_state);

	if (!td->files_index) {
		log_err("fio: did not find replay device(s)\n");
		return false;
	}

	return true;

err:
	if (td->io_log_rfile) {
		fclose(td->io_log_rfile);
		td->io_log_rfile = NULL;
	}
	return false;
}

/*
 * [한국어] blktrace 파일에서 트레이스 항목들을 읽어 I/O 작업으로 변환
 *
 * 메인 읽기 루프:
 *   1) fread()로 blk_io_trace 구조체를 읽음
 *   2) 바이트 스왑 적용 (필요시)
 *   3) 매직 넘버와 버전 검증
 *   4) PDU 건너뛰기
 *   5) 큐 깊이 추적 (queue/merge/complete 이벤트)
 *   6) queue_trace()로 I/O 작업 분류 및 저장
 *
 * read_iolog_chunked 모드: 대용량 트레이스를 청크 단위로 나누어 읽음
 * 완료 후: td_ddir, max_bs, iodepth를 트레이스 데이터에서 자동 설정
 */
bool read_blktrace(struct thread_data* td)
{
	struct blk_io_trace t;
	struct file_cache cache = {
		.maj = ~0U,           /* 캐시 무효화를 위해 불가능한 값으로 초기화 */
		.min = ~0U,
	};
	unsigned long ios[DDIR_RWDIR_SYNC_CNT] = { };    /* 방향별 I/O 카운트 */
	unsigned long long rw_bs[DDIR_RWDIR_CNT] = { };   /* 방향별 최대 블록 크기 */
	unsigned long skipped_writes;
	FILE *f = td->io_log_rfile;
	int i, max_depth;
	struct fio_file *fiof;
	int this_depth[DDIR_RWDIR_CNT] = { };  /* 현재 진행 중인 I/O 깊이 */
	int depth[DDIR_RWDIR_CNT] = { };       /* 관찰된 최대 I/O 깊이 */
	int64_t items_to_fetch = 0;

	/* 청크 모드: 한 번에 읽을 항목 수 결정 */
	if (td->o.read_iolog_chunked) {
		items_to_fetch = iolog_items_to_fetch(td);
		if (!items_to_fetch)
			return true;
	}

	skipped_writes = 0;
	do {
		int ret = fread(&t, 1, sizeof(t), f);

		if (ferror(f)) {
			td_verror(td, errno, "read blktrace file");
			goto err;
		} else if (feof(f)) {
			break;
		} else if (ret < (int) sizeof(t)) {
			log_err("fio: iolog short read\n");
			break;
		}

		/* 필요시 엔디안 바이트 스왑 적용 */
		if (td->io_log_blktrace_swap)
			byteswap_trace(&t);

		/* 매직 넘버 검증 */
		if ((t.magic & 0xffffff00) != BLK_IO_TRACE_MAGIC) {
			log_err("fio: bad magic in blktrace data: %x\n",
								t.magic);
			goto err;
		}
		/* 버전 검증 */
		if ((t.magic & 0xff) != BLK_IO_TRACE_VERSION) {
			log_err("fio: bad blktrace version %d\n",
								t.magic & 0xff);
			goto err;
		}
		ret = discard_pdu(f, &t);
		if (ret < 0) {
			td_verror(td, -ret, "blktrace lseek");
			goto err;
		}

		/* notify가 아닌 경우 큐 깊이 추적 */
		if ((t.action & BLK_TC_ACT(BLK_TC_NOTIFY)) == 0) {
			if ((t.action & 0xffff) == __BLK_TA_QUEUE)
				depth_inc(&t, this_depth);     /* 큐잉: 깊이 증가 */
			else if (((t.action & 0xffff) == __BLK_TA_BACKMERGE) ||
				((t.action & 0xffff) == __BLK_TA_FRONTMERGE))
				depth_dec(&t, this_depth);     /* 병합: 깊이 감소 */
			else if ((t.action & 0xffff) == __BLK_TA_COMPLETE)
				depth_end(&t, this_depth, depth); /* 완료: 최대값 갱신 */

			/* 읽기 전용 모드에서 쓰기 건너뛰기 */
			if (t_is_write(&t) && read_only) {
				skipped_writes++;
				continue;
			}
		}

		if (!queue_trace(td, &t, ios, rw_bs, &cache))
			continue;

		/* 청크 모드: 할당량 소진 시 중단 */
		if (td->o.read_iolog_chunked) {
			td->io_log_current++;
			items_to_fetch--;
			if (items_to_fetch == 0)
				break;
		}
	} while (1);

	/* 청크 모드: 하이워터마크/체크마크 갱신 */
	if (td->o.read_iolog_chunked) {
		td->io_log_highmark = td->io_log_current;
		td->io_log_checkmark = (td->io_log_highmark + 1) / 2;
		fio_gettime(&td->io_log_highmark_time, NULL);
	}

	if (skipped_writes)
		log_err("fio: %s skips replay of %lu writes due to read-only\n",
						td->o.name, skipped_writes);

	/* 청크 모드 완료 처리: 방향 설정 및 버퍼 재할당 */
	if (td->o.read_iolog_chunked) {
		if (td->io_log_current == 0) {
			return false;
		}
		td->o.td_ddir = TD_DDIR_RW;
		if ((rw_bs[DDIR_READ] > td->o.max_bs[DDIR_READ] ||
		     rw_bs[DDIR_WRITE] > td->o.max_bs[DDIR_WRITE] ||
		     rw_bs[DDIR_TRIM] > td->o.max_bs[DDIR_TRIM]) &&
		    td->orig_buffer)
		{
			td->o.max_bs[DDIR_READ] = max(td->o.max_bs[DDIR_READ], rw_bs[DDIR_READ]);
			td->o.max_bs[DDIR_WRITE] = max(td->o.max_bs[DDIR_WRITE], rw_bs[DDIR_WRITE]);
			td->o.max_bs[DDIR_TRIM] = max(td->o.max_bs[DDIR_TRIM], rw_bs[DDIR_TRIM]);
			io_u_quiesce(td);
			free_io_mem(td);
			if (init_io_u_buffers(td))
				return false;
		}
		return true;
	}

	/* 비청크 모드: 모든 파일에 닫기 이벤트 추가 */
	for_each_file(td, fiof, i)
		trace_add_open_close_event(td, fiof->fileno, FIO_LOG_CLOSE_FILE);

	fclose(td->io_log_rfile);
	td->io_log_rfile = NULL;

	/*
	 * For stacked devices, we don't always get a COMPLETE event so
	 * the depth grows to insane values. Limit it to something sane(r).
	 */
	/* [한국어] 스택형 디바이스에서는 COMPLETE 이벤트가 누락될 수 있어 깊이를 1024로 제한 */
	max_depth = 0;
	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		if (depth[i] > 1024)
			depth[i] = 1024;
		else if (!depth[i] && ios[i])
			depth[i] = 1;
		max_depth = max(depth[i], max_depth);
	}

	if (!ios[DDIR_READ] && !ios[DDIR_WRITE] && !ios[DDIR_TRIM] &&
	    !ios[DDIR_SYNC]) {
		log_err("fio: found no ios in blktrace data\n");
		return false;
	}

	/* 트레이스에서 관찰된 I/O 방향 및 최대 블록 크기 설정 */
	td->o.td_ddir = 0;
	if (ios[DDIR_READ]) {
		td->o.td_ddir |= TD_DDIR_READ;
		td->o.max_bs[DDIR_READ] = rw_bs[DDIR_READ];
	}
	if (ios[DDIR_WRITE]) {
		td->o.td_ddir |= TD_DDIR_WRITE;
		td->o.max_bs[DDIR_WRITE] = rw_bs[DDIR_WRITE];
	}
	if (ios[DDIR_TRIM]) {
		td->o.td_ddir |= TD_DDIR_TRIM;
		td->o.max_bs[DDIR_TRIM] = rw_bs[DDIR_TRIM];
	}

	/*
	 * If depth wasn't manually set, use probed depth
	 */
	/* [한국어] iodepth가 수동 설정되지 않았으면 트레이스에서 관찰된 최대 깊이 사용 */
	if (!fio_option_is_set(&td->o, iodepth))
		td->o.iodepth = td->o.iodepth_low = max_depth;

	return true;
err:
	fclose(f);
	return false;
}

/*
 * [한국어] 병합 파라미터(스칼라/반복횟수) 리스트 초기화
 *
 * vals 배열의 값을 각 blktrace_cursor의 지정된 오프셋 필드에 설정한다.
 * vals가 비어있으면 기본값(def)을 사용하고, 개수가 로그 수와 불일치하면 에러를 반환한다.
 */
static int init_merge_param_list(fio_fp64_t *vals, struct blktrace_cursor *bcs,
				 int nr_logs, int def, size_t off)
{
	int i = 0, len = 0;

	while (len < FIO_IO_U_LIST_MAX_LEN && vals[len].u.f != 0.0)
		len++;

	if (len && len != nr_logs)
		return len;

	for (i = 0; i < nr_logs; i++) {
		int *val = (int *)((char *)&bcs[i] + off);
		*val = def;
		if (len)
			*val = (int)vals[i].u.f;
	}

	return 0;

}

/* [한국어] 여러 blktrace 커서 중 타임스탬프가 가장 빠른 항목의 인덱스를 찾음 */
static int find_earliest_io(struct blktrace_cursor *bcs, int nr_logs)
{
	__u64 time = ~(__u64)0;
	int idx = 0, i;

	for (i = 0; i < nr_logs; i++) {
		if (bcs[i].t.time < time) {
			time = bcs[i].t.time;
			idx = i;
		}
	}

	return idx;
}

/*
 * [한국어] 파일 읽기 완료 처리
 *
 * 반복 횟수가 남아있으면 파일을 처음으로 되감고,
 * 모든 반복이 끝나면 파일을 닫고 활성 로그 배열을 압축한다.
 */
static void merge_finish_file(struct blktrace_cursor *bcs, int i, int *nr_logs)
{
	bcs[i].iter++;
	if (bcs[i].iter < bcs[i].nr_iter) {
		fseek(bcs[i].f, 0, SEEK_SET);
		return;
	}

	*nr_logs -= 1;

	/* close file */
	fclose(bcs[i].f);

	/* keep active files contiguous */
	/* [한국어] 닫힌 파일 슬롯을 마지막 활성 파일로 덮어써서 배열을 연속으로 유지 */
	memmove(&bcs[i], &bcs[*nr_logs], sizeof(bcs[i]));
}

/*
 * [한국어] 단일 blktrace 커서에서 다음 유효한 트레이스 항목을 읽음
 *
 * fio가 관심 있는 __BLK_TA_QUEUE 액션만 반환하고,
 * 나머지 액션은 PDU를 건너뛴 뒤 다시 읽기를 시도한다(read_skip 레이블).
 * 반복(iter) 시 시간을 누적하여 연속적인 시간 흐름을 만든다.
 */
static int read_trace(struct thread_data *td, struct blktrace_cursor *bc)
{
	int ret = 0;
	struct blk_io_trace *t = &bc->t;

read_skip:
	/* read an io trace */
	ret = fread(&t, 1, sizeof(t), bc->f);
	if (ferror(bc->f)) {
		td_verror(td, errno, "read blktrace file");
		return ret;
	} else if (feof(bc->f)) {
		if (!bc->length)
			bc->length = bc->t.time;  /* 트레이스 전체 길이 기록 */
		return ret;
	} else if (ret < (int) sizeof(*t)) {
		log_err("fio: iolog short read\n");
		return -1;
	}

	if (bc->swap)
		byteswap_trace(t);

	/* skip over actions that fio does not care about */
	/* [한국어] QUEUE가 아니거나 유효하지 않은 방향의 액션은 건너뜀 */
	if ((t->action & 0xffff) != __BLK_TA_QUEUE ||
	    t_get_ddir(t) == DDIR_INVAL) {
		ret = discard_pdu(bc->f, t);
		if (ret < 0) {
			td_verror(td, -ret, "blktrace lseek");
			return ret;
		}
		goto read_skip;
	}

	/* 반복 횟수와 스칼라를 적용하여 시간 조정 */
	t->time = (t->time + bc->iter * bc->length) * bc->scalar / 100;

	return ret;
}

/* [한국어] blk_io_trace를 출력 파일에 기록 (PDU는 포함하지 않음) */
static int write_trace(FILE *fp, struct blk_io_trace *t)
{
	/* pdu is not used so just write out only the io trace */
	t->pdu_len = 0;
	return fwrite((void *)t, sizeof(*t), 1, fp);
}

/*
 * [한국어] 여러 blktrace 파일을 시간순으로 병합
 *
 * 흐름:
 *   1) 병합 파라미터(scalar, nr_iter) 초기화
 *   2) 출력 파일 및 입력 파일들 열기
 *   3) 각 입력 파일에서 첫 번째 트레이스 읽기
 *   4) 가장 빠른 타임스탬프를 가진 트레이스를 출력 파일에 쓰기 (반복)
 *   5) 병합된 파일을 read_iolog_file로 설정
 *
 * merge_blktrace_scalars: 각 파일의 시간 스케일링 비율 (%)
 * merge_blktrace_iters: 각 파일의 반복 횟수
 */
int merge_blktrace_iologs(struct thread_data *td)
{
	int nr_logs = get_max_str_idx(td->o.read_iolog_file);
	struct blktrace_cursor *bcs = malloc(sizeof(struct blktrace_cursor) *
					     nr_logs);
	struct blktrace_cursor *bc;
	FILE *merge_fp;
	char *str, *ptr, *name, *merge_buf;
	int i, ret;

	/* 스칼라(시간 배율) 파라미터 초기화 (기본값: 100%) */
	ret = init_merge_param_list(td->o.merge_blktrace_scalars, bcs, nr_logs,
				    100, offsetof(struct blktrace_cursor,
						  scalar));
	if (ret) {
		log_err("fio: merge_blktrace_scalars(%d) != nr_logs(%d)\n",
			ret, nr_logs);
		goto err_param;
	}

	/* 반복 횟수 파라미터 초기화 (기본값: 1회) */
	ret = init_merge_param_list(td->o.merge_blktrace_iters, bcs, nr_logs,
				    1, offsetof(struct blktrace_cursor,
						nr_iter));
	if (ret) {
		log_err("fio: merge_blktrace_iters(%d) != nr_logs(%d)\n",
			ret, nr_logs);
		goto err_param;
	}

	/* setup output file */
	/* [한국어] 병합 결과를 쓸 출력 파일 열기 (128KB 버퍼링) */
	merge_fp = fopen(td->o.merge_blktrace_file, "w");
	merge_buf = malloc(128 * 1024);
	if (!merge_buf)
		goto err_out_file;
	ret = setvbuf(merge_fp, merge_buf, _IOFBF, 128 * 1024);
	if (ret)
		goto err_merge_buf;

	/* setup input files */
	/* [한국어] 콤마로 구분된 입력 파일 목록을 파싱하여 각각 열기 */
	str = ptr = strdup(td->o.read_iolog_file);
	nr_logs = 0;
	for (i = 0; (name = get_next_str(&ptr)) != NULL; i++) {
		bcs[i].f = fopen(name, "rb");
		if (!bcs[i].f) {
			log_err("fio: could not open file: %s\n", name);
			ret = -errno;
			free(str);
			goto err_file;
		}
		nr_logs++;

		if (!is_blktrace(name, &bcs[i].swap)) {
			log_err("fio: file is not a blktrace: %s\n", name);
			free(str);
			goto err_file;
		}

		/* 각 파일에서 첫 번째 유효 트레이스 읽기 */
		ret = read_trace(td, &bcs[i]);
		if (ret < 0) {
			free(str);
			goto err_file;
		} else if (!ret) {
			merge_finish_file(bcs, i, &nr_logs);
			i--;
		}
	}
	free(str);

	/* merge files */
	/* [한국어] 모든 파일이 소진될 때까지 가장 빠른 트레이스를 선택하여 출력 */
	while (nr_logs) {
		i = find_earliest_io(bcs, nr_logs);
		bc = &bcs[i];
		/* skip over the pdu */
		ret = discard_pdu(bc->f, &bc->t);
		if (ret < 0) {
			td_verror(td, -ret, "blktrace lseek");
			goto err_file;
		}

		ret = write_trace(merge_fp, &bc->t);
		ret = read_trace(td, bc);
		if (ret < 0)
			goto err_file;
		else if (!ret)
			merge_finish_file(bcs, i, &nr_logs);
	}

	/* set iolog file to read from the newly merged file */
	/* [한국어] 병합된 파일을 iolog 입력 파일로 설정하여 이후 재생에 사용 */
	td->o.read_iolog_file = td->o.merge_blktrace_file;
	ret = 0;

err_file:
	/* cleanup */
	for (i = 0; i < nr_logs; i++) {
		fclose(bcs[i].f);
	}
err_merge_buf:
	free(merge_buf);
err_out_file:
	fflush(merge_fp);
	fclose(merge_fp);
err_param:
	free(bcs);

	return ret;
}
