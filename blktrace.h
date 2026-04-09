/*
 * [한국어] blktrace.h - blktrace 구조체 및 API 선언
 *
 * blktrace는 Linux 블록 레이어의 I/O 트레이스 데이터를 캡처하는 도구이다.
 * 이 헤더는 blktrace 바이너리 파일을 읽고 재생하기 위한 구조체와 함수를 선언한다.
 * FIO_HAVE_BLKTRACE가 정의되지 않은 경우, 모든 함수는 false를 반환하는 스텁으로 대체된다.
 */
#ifndef FIO_BLKTRACE_H
#define FIO_BLKTRACE_H


#ifdef FIO_HAVE_BLKTRACE

#include <asm/types.h>       /* __u64 등 커널 타입 정의 */

#include "blktrace_api.h"    /* blk_io_trace 구조체 및 매직 넘버 */

/*
 * [한국어] blktrace_cursor - blktrace 파일을 순차적으로 읽기 위한 커서 구조체
 *
 * 여러 blktrace 파일을 병합(merge)할 때 각 파일의 읽기 상태를 추적한다.
 */
struct blktrace_cursor {
	struct fifo		*fifo;	// fifo queue for reading  /* 읽기용 FIFO 큐 */
	FILE			*f;	// blktrace file           /* blktrace 입력 파일 포인터 */
	__u64			length; // length of trace         /* 트레이스의 전체 시간 길이 */
	struct blk_io_trace	t;	// current io trace        /* 현재 읽은 I/O 트레이스 항목 */
	int			swap;	// bitwise reverse required /* 엔디안 바이트 스왑 필요 여부 */
	int			scalar;	// scale percentage        /* 시간 스케일링 백분율 */
	int			iter;	// current iteration       /* 현재 반복 횟수 */
	int			nr_iter; // number of iterations to run /* 총 반복 실행 횟수 */
};

/* [한국어] 파일이 blktrace 바이너리 형식인지 매직 넘버로 확인 */
bool is_blktrace(const char *, int *);
/* [한국어] blktrace 파일을 열고 io_piece 리스트로 읽어들이기 초기화 */
bool init_blktrace_read(struct thread_data *, const char *, int);
/* [한국어] blktrace 파일에서 트레이스 항목들을 읽어 I/O 작업으로 변환 */
bool read_blktrace(struct thread_data* td);

/* [한국어] 여러 blktrace 파일을 시간순으로 병합하여 단일 파일로 출력 */
int merge_blktrace_iologs(struct thread_data *td);

#else

/*
 * [한국어] blktrace 미지원 플랫폼용 스텁 함수들
 * FIO_HAVE_BLKTRACE가 정의되지 않으면 모든 함수가 false를 반환한다.
 */
static inline bool is_blktrace(const char *fname, int *need_swap)
{
	return false;
}

static inline bool init_blktrace_read(struct thread_data *td, const char *fname,
				 int need_swap)
{
	return false;
}

static inline bool read_blktrace(struct thread_data* td)
{
	return false;
}


static inline int merge_blktrace_iologs(struct thread_data *td)
{
	return false;
}

#endif
#endif
