/*
 * [한국어 설명] 동적 확장 출력 버퍼 공개 API 헤더 (output_buffer.h)
 *
 * === 파일의 역할 ===
 * lib/output_buffer.c 가 구현하는 "계속 append 되며 필요 시 realloc 으로
 * 자라나는 char 버퍼" 자료구조 struct buf_output 과 그 세 조작 함수
 * (init/free/add) 를 노출한다. printf 류처럼 파일 디스크립터로 즉시
 * 내보내지 않고, 메모리에 누적했다가 한 번에 flush 하고 싶을 때(예: 서버
 * 모드에서 통계를 한 덩어리로 클라이언트에 전송) 사용한다. 내부는 단순
 * realloc 기반이지만 BUF_INC=1024 바이트의 묶음 단위 성장으로 realloc
 * 빈도를 제어한다(이 상수는 .c 에 있으며 헤더에는 노출 안 됨).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "결과 출력/전송" 계층의 하위 유틸. stat.c 의 show_run_stats 계열,
 * server.c 의 TEXT 명령 준비, diskutil.c 의 디스크 사용률 출력, client.c 의
 * 원격 수신 텍스트 스트림이 모두 struct buf_output 을 중간 버퍼로 사용하여
 * 일관된 출력 포맷을 구축한 뒤 log_info_buf / fio_server_send_cmd 등으로
 * 한 번에 내보낸다.
 * 호출 체인:
 *   stat.c __show_run_stats → buf_output_init(&out) → show_* → buf_output_add
 *     → 최종 log_info_buf(&out) → buf_output_free(&out)
 *
 * === 타 모듈과의 연결 ===
 * - output_buffer.c : 구조체 메서드 구현(malloc/realloc, lazy 할당 이디엄).
 * - stat.c / diskutil.c / server.c / client.c : 사용자 측.
 * - <stddef.h> (본 헤더가 포함) : size_t 정의.
 * 데이터 흐름: 작은 문자열 조각들 → buf_output_add → 내부 realloc → 최종
 * log_info_buf / fio_server_send_cmd 로 한 번에 전송 후 buf_output_free.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct buf_output : buf(힙 포인터) + buflen(현재 길이) + max_buflen(할당된
 *   총 바이트 수). lazy 할당으로 초기에 NULL 로 두었다가 첫 add 시 malloc.
 * - buf_output_init : 구조체 필드 0/NULL 초기화.
 * - buf_output_free : 내부 버퍼 free + 포인터 NULL 세트.
 * - buf_output_add : len 바이트 append. 부족 시 realloc. 추가 바이트 수 반환.
 */
#ifndef FIO_OUTPUT_BUFFER_H
#define FIO_OUTPUT_BUFFER_H
/* [한국어] 헤더 가드. stat.c / diskutil.c / server.c 등에서 포함 중복 방지. */

#include <stddef.h>
/* [한국어] <stddef.h> : size_t 정의. buflen/max_buflen 필드와 buf_output_add
 * 의 len 파라미터에 사용. */

struct buf_output {
	char *buf;
	/* [한국어] 동적 할당된 바이트 버퍼 포인터.
	 * 설정자: buf_output_init 이 NULL 로 초기화, buf_output_add 가 첫 호출
	 *   시 malloc 하거나 이후 realloc 으로 교체.
	 * 읽는 자: log_info_buf / fio_server_send_cmd 등이 (buf, buflen) 페어로
	 *   읽어 실제 출력/전송.
	 * 값 범위: NULL(미초기화/해제) 또는 유효 힙 포인터.
	 * 동기화: 단일 스레드 소유(호출자가 스레드 격리 책임). 공유 필요 시
	 *   외부 락 필요. */

	size_t buflen;
	/* [한국어] 현재 버퍼에 쌓인 바이트 수(NUL 포함 여부는 add 호출자 규약).
	 * 설정자: buf_output_add 가 len 만큼 증가. buf_output_init/free 가 0 으로
	 *   리셋.
	 * 읽는 자: log_info_buf 등이 buflen 바이트만큼만 출력.
	 * 값 범위: 0 ≤ buflen ≤ max_buflen. 절대 max_buflen 을 넘지 않음. */

	size_t max_buflen;
	/* [한국어] 현재 할당된 버퍼의 총 바이트 수.
	 * 설정자: 첫 add 에서 BUF_INC 단위로 설정, realloc 시 증분.
	 * 읽는 자: buf_output_add 내부가 (buflen + new_len > max_buflen) 판정에
	 *   사용.
	 * 값 범위: 0 또는 BUF_INC(1024) 배수. */
};

void buf_output_init(struct buf_output *out);
/* [한국어] buf_output_init - 구조체의 모든 필드를 0/NULL 로 초기화.
 * buf_output_add 이전에 반드시 호출. 힙 할당은 하지 않음(lazy). */

void buf_output_free(struct buf_output *out);
/* [한국어] buf_output_free - out->buf 가 있으면 free() 하고 필드를 0/NULL 로
 * 되돌림. 해제 후 재사용 하려면 buf_output_init 을 다시 호출할 수 있음. */

size_t buf_output_add(struct buf_output *out, const char *buf, size_t len);
/* [한국어] buf_output_add - len 바이트를 out 에 append. 공간 부족 시
 * realloc 으로 확장. 반환: 실제로 추가된 바이트 수(일반적으로 len, 단
 * realloc 실패 시 0 이거나 부분 값). 호출자는 NUL 종결을 원하면 직접 '\0'
 * 을 포함해 호출하거나 후처리해야 한다. */

#endif
/* [한국어] FIO_OUTPUT_BUFFER_H 가드 종료. */
