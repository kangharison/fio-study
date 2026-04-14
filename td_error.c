/*
 * [한국어 설명] 스레드 에러 처리 모듈 (td_error.c)
 *
 * === 파일의 역할 ===
 * fio 작업 스레드에서 발생하는 I/O 에러를 분류하고 처리하는 기능을 담당한다.
 * continue_on_error/ignore_error 옵션으로 특정 에러를 무시하고 계속 실행 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * backend.c의 do_io()에서 I/O 에러 발생 시 td_non_fatal_error()를 호출.
 * 호출 체인: do_io() [backend.c] → td_non_fatal_error() [이 파일]
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: I/O 에러 발생 시 에러 분류/처리 호출
 * - td_error.h: 에러 타입 열거형 및 함수 선언
 * - io_ddir.h: I/O 방향 정의 (읽기/쓰기/검증)
 *
 * === 주요 함수/구조체 요약 ===
 * - td_error_type(): 에러를 읽기/쓰기/검증 유형으로 분류
 * - td_non_fatal_error(): 에러가 치명적이지 않은지 판별
 * - update_error_count(): 에러 카운터 업데이트 및 첫 에러 기록
 */

#include "fio.h"       /* fio 핵심 구조체 및 매크로 */
#include "io_ddir.h"   /* I/O 방향 정의 (DDIR_READ, DDIR_WRITE 등) */
#include "td_error.h"  /* 에러 타입 열거형 및 함수 선언 */

/* [한국어] 기본 비치명적 에러 목록 - ignore_error가 설정되지 않은 경우 사용
 * EIO: 일반 I/O 에러, EILSEQ: 시퀀스 에러 (검증 실패) */
static int __NON_FATAL_ERR[] = { EIO, EILSEQ };

/* [한국어] I/O 방향과 에러 코드로부터 에러 유형(비트)을 결정
 * - EILSEQ: 항상 검증(VERIFY) 에러로 분류
 * - DDIR_READ: 읽기 에러로 분류
 * - 그 외: 쓰기 에러로 분류 */
enum error_type_bit td_error_type(enum fio_ddir ddir, int err)
{
	if (err == EILSEQ)
		return ERROR_TYPE_VERIFY_BIT;
	if (ddir == DDIR_READ)
		return ERROR_TYPE_READ_BIT;
	return ERROR_TYPE_WRITE_BIT;
}

/* [한국어] 해당 에러가 비치명적(무시 가능)인지 판별
 * - continue_on_error 비트가 해당 에러 유형에 설정되어 있어야 함
 * - ignore_error 배열에 해당 에러 코드가 포함되어 있으면 비치명적(1 반환)
 * - ignore_error가 설정되지 않은 경우 기본 목록(__NON_FATAL_ERR) 사용
 * 반환값: 1 = 비치명적 (무시 가능), 0 = 치명적 (작업 중단) */
int td_non_fatal_error(struct thread_data *td, enum error_type_bit etype,
		       int err)
{
	unsigned int i;

	/* [한국어] ignore_error가 미설정이면 기본 비치명적 에러 목록 사용 */
	if (!td->o.ignore_error[etype]) {
		td->o.ignore_error[etype] = __NON_FATAL_ERR;
		td->o.ignore_error_nr[etype] = FIO_ARRAY_SIZE(__NON_FATAL_ERR);
	}

	/* [한국어] continue_on_error에 해당 에러 유형 비트가 없으면 치명적 */
	if (!(td->o.continue_on_error & (1 << etype)))
		return 0;
	/* [한국어] ignore_error 목록에서 해당 에러 코드 검색 */
	for (i = 0; i < td->o.ignore_error_nr[etype]; i++)
		if (td->o.ignore_error[etype][i] == err)
			return 1;  /* 목록에 있으면 비치명적 */

	return 0;  /* 목록에 없으면 치명적 */
}

/* [한국어] 에러 카운터 업데이트
 * - 총 에러 수 증가
 * - 첫 번째 에러인 경우 에러 코드를 기록 (이후 보고에 사용) */
void update_error_count(struct thread_data *td, int err)
{
	td->total_err_count++;
	if (td->total_err_count == 1)
		td->first_error = err;
}
