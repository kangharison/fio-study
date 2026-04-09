/*
 * [한국어] trim.h - TRIM/DISCARD I/O 지원 헤더
 *
 * SSD 트림 기능과 관련된 함수 선언 및 인라인 유틸리티를 제공한다.
 * FIO_HAVE_TRIM이 정의되지 않은 플랫폼에서는 스텁(stub) 함수를 제공하여
 * 트림 관련 호출이 안전하게 무시되도록 한다.
 */
#ifndef FIO_TRIM_H
#define FIO_TRIM_H

#ifdef FIO_HAVE_TRIM
#include "flist.h"              /* fio 연결 리스트 */
#include "iolog.h"              /* I/O 로그 및 io_piece 구조체 */
#include "compiler/compiler.h"  /* 컴파일러 속성 매크로 (__must_check 등) */
#include "lib/types.h"          /* 기본 타입 정의 */
#include "os/os.h"              /* OS별 추상화 */

/* [한국어] 다음 트림 대상 영역을 가져옴 - 반환값 반드시 확인 필요 (__must_check) */
extern bool __must_check get_next_trim(struct thread_data *td, struct io_u *io_u);
/* [한국어] 현재 I/O를 트림으로 수행할지 확률적으로 결정 */
extern bool io_u_should_trim(struct thread_data *td, struct io_u *io_u);

/*
 * Determine whether a given io_u should be logged for verify or
 * for discard
 */
/* [한국어] 트림 목록에서 io_piece 항목을 제거하고 트림 카운터 감소 */
static inline void remove_trim_entry(struct thread_data *td, struct io_piece *ipo)
{
	if (!flist_empty(&ipo->trim_list)) {
		flist_del_init(&ipo->trim_list);
		td->trim_entries--;
	}
}

#else
/* [한국어] FIO_HAVE_TRIM 미정의 시 스텁 함수들 - 트림 미지원 플랫폼용 */
static inline bool get_next_trim(struct thread_data *td, struct io_u *io_u)
{
	return false;
}
static inline bool io_u_should_trim(struct thread_data *td, struct io_u *io_u)
{
	return false;
}
static inline void remove_trim_entry(struct thread_data *td, struct io_piece *ipo)
{
}
#endif

#endif
