/*
 * TRIM/DISCARD support
 */
/*
 * [한국어] trim.c - TRIM/DISCARD I/O 지원
 *
 * 이 파일은 SSD 등의 TRIM(DISCARD) 명령 처리를 담당한다.
 * 주요 기능:
 *   1) get_next_trim()    - 트림 목록에서 다음 트림할 영역을 가져와 io_u에 설정
 *   2) io_u_should_trim() - 확률 기반으로 현재 I/O를 트림으로 수행할지 결정
 */

#include <string.h>    /* 문자열 처리 */
#include <assert.h>    /* 단언 매크로 */

#include "fio.h"       /* fio 핵심 구조체 및 매크로 */
#include "trim.h"      /* 트림 관련 선언 */

#ifdef FIO_HAVE_TRIM
/* [한국어] 트림 목록에서 다음 트림 대상 영역을 가져와 io_u에 설정
 * - 재큐(requeue)된 io_u는 이미 파일이 설정되어 있으므로 바로 반환
 * - 트림 목록이 비어있으면 false 반환
 * - trim_zero 옵션이 꺼져 있으면 검증 목록에서도 제거
 * - trim_zero가 켜져 있으면 TRIMMED 플래그만 설정 (나중에 0 검증) */
bool get_next_trim(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo;

	/*
	 * this io_u is from a requeue, we already filled the offsets
	 */
	if (io_u->file)
		return true;
	if (flist_empty(&td->trim_list))
		return false;

	assert(td->trim_entries);
	ipo = flist_first_entry(&td->trim_list, struct io_piece, trim_list);
	remove_trim_entry(td, ipo);

	io_u->offset = ipo->offset;   /* 트림할 오프셋 */
	io_u->buflen = ipo->len;      /* 트림할 길이 */
	io_u->file = ipo->file;       /* 대상 파일 */

	/*
	 * If not verifying that trimmed ranges return zeroed data,
	 * remove this from the to-read verify lists
	 */
	/* [한국어] trim_zero가 비활성이면 검증 목록(리스트 또는 rb-tree)에서 제거 */
	if (!td->o.trim_zero) {
		if (ipo->flags & IP_F_ONLIST)
			flist_del(&ipo->list);
		else {
			assert(ipo->flags & IP_F_ONRB);
			rb_erase(&ipo->rb_node, &td->io_hist_tree);
		}
		td->io_hist_len--;
		free(ipo);
	} else
		ipo->flags |= IP_F_TRIMMED;  /* 트림됨 표시 - 나중에 0 검증용 */

	/* [한국어] 대상 파일이 열려있지 않으면 열기 시도 */
	if (!fio_file_open(io_u->file)) {
		int r = td_io_open_file(td, io_u->file);

		if (r) {
			dprint(FD_VERIFY, "failed file %s open\n",
					io_u->file->file_name);
			return false;
		}
	}

	get_file(io_u->file);            /* 파일 참조 카운트 증가 */
	assert(fio_file_open(io_u->file));
	io_u->ddir = DDIR_TRIM;         /* I/O 방향을 TRIM으로 설정 */
	io_u->xfer_buf = NULL;          /* 트림은 데이터 전송 없음 */
	io_u->xfer_buflen = io_u->buflen;

	dprint(FD_VERIFY, "get_next_trim: ret io_u %p\n", io_u);
	return true;
}

/* [한국어] 현재 I/O를 트림으로 수행할지 확률적으로 결정
 * trim_percentage 옵션에 따라 난수로 판별
 * 예: trim_percentage=10이면 약 10%의 I/O가 트림으로 수행됨 */
bool io_u_should_trim(struct thread_data *td, struct io_u *io_u)
{
	unsigned long long val;
	uint64_t frand_max;
	unsigned long r;

	if (!td->o.trim_percentage)
		return false;

	frand_max = rand_max(&td->trim_state);  /* 난수 최대값 */
	r = __rand(&td->trim_state);            /* 난수 생성 */
	val = (frand_max / 100ULL);

	val *= (unsigned long long) td->o.trim_percentage;
	return r <= val;  /* 난수가 임계값 이하이면 트림 수행 */
}
#endif
