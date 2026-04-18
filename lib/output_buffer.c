/*
 * [한국어 설명] 동적 확장 출력 버퍼 (output_buffer.c)
 *
 * === 파일의 역할 ===
 * fio 가 긴 진단/결과 텍스트를 "여러 출력처(stdout, 로그 파일, 네트워크 서버 응답)"에
 * 원자적으로 전달하기 위해 사용하는 가변 크기 메모리 버퍼 자료구조의 구현이다.
 * realloc 기반으로 필요 시 1024B(BUF_INC) 이상 단위로 성장하며, printf 류 중간 결과를
 * 누적해 두었다가 한 번에 stdout/stderr 로 flush 하여 멀티 스레드/클라이언트-서버
 * 환경에서 부분 출력의 인터리빙을 방지한다. 또한 서버 모드(fio --server)에서는
 * 이 버퍼를 TEXT/FT_TEXT 명령 페이로드로 네트워크에 직접 송출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 공용 유틸로서 stat.c(show_thread_status_normal/terse/json), iolog.c(로그 라인
 * 구성), server.c(TEXT 명령 응답 조립), output.c 그리고 여러 엔진의 디버그 로그에서
 * 사용된다. 구조체 struct buf_output 은 소유자가 stack/heap 어디든 배치 가능하며,
 * init→add 반복→flush 또는 복사→free 생명주기를 따른다.
 *
 * === 타 모듈과의 연결 ===
 * - output_buffer.h: struct buf_output 정의와 본 파일의 4개 API 프로토타입 선언.
 * - ../minmax.h: max() 매크로 — 성장 크기 결정 시 BUF_INC 와 필요량 중 큰 값 선택.
 * - <string.h> memcpy/memset: 데이터 복사 및 신규 영역 0 초기화.
 * - <stdlib.h> realloc/free: 동적 크기 조정.
 * - stat.c/server.c/iolog.c/output.c: 주 사용처.
 *
 * === 주요 함수/구조체 요약 ===
 * - buf_output_init():   buflen=max_buflen=0, buf=NULL 로 초기화(할당 없음, lazy).
 * - buf_output_free():   buf 해제 후 init() 호출로 재사용 가능 상태로 복귀.
 * - buf_output_add():    len 바이트를 누적. 부족 시 max_buflen 을 확장(BUF_INC 단위).
 *                         반환: 실제 추가된 바이트 수(현재 구현 항상 len).
 * - struct buf_output(헤더 정의):
 *     char *buf;              실데이터 포인터(realloc 가능, 초기 NULL).
 *     size_t buflen;          현재 사용 중인 바이트 수(append 커서).
 *     size_t max_buflen;      buf 의 현재 할당 크기(용량).
 *   단일 스레드가 소유하는 전제 — 멀티 스레드 공유 시 외부 락 필요.
 */
#include <string.h>             /* [한국어] memcpy(추가), memset(0 초기화) 공급 */
#include <stdlib.h>             /* [한국어] realloc/free 동적 할당 API */

#include "output_buffer.h"      /* [한국어] struct buf_output 정의, 본 파일의 API 프로토타입 */
#include "../minmax.h"           /* [한국어] max() 매크로 — 필요량과 BUF_INC 중 큰 값 선택 */

/* [한국어] 한 번의 realloc 으로 성장시키는 최소 증가 단위(1 KiB).
 * 너무 작으면 realloc 빈도가 올라가 단편화/복사 오버헤드가 증가하고,
 * 너무 크면 짧은 출력에서 메모리 낭비가 커져 타협점으로 1 KiB 채택 */
#define BUF_INC	1024

/* [한국어]
 * buf_output_init - buf_output 구조체를 빈 상태로 초기화한다.
 *
 * @out: 호출자가 소유한 struct buf_output 포인터(stack/heap 모두 가능). NULL 금지.
 *
 * 할당을 지연(lazy)하여, 첫 add 호출 시점까지 heap 을 건드리지 않는다 — "로그를
 * 실제로 출력하지 않은 잡"에서 메모리 낭비 제거가 목적.
 *
 * 호출 체인: 호출자 임의의 출력 스트림 초기화 지점 → [buf_output_init].
 * 실행 컨텍스트: 어디서나 안전(시스템 호출 없음).
 */
void buf_output_init(struct buf_output *out)
{
	/* [한국어] 현재 용량 0 — 첫 add 시 realloc 트리거 */
	out->max_buflen = 0;
	/* [한국어] 누적된 데이터 없음 — append 커서 */
	out->buflen = 0;
	/* [한국어] realloc(NULL,...) 은 malloc 과 동치이므로 안전한 초기값 */
	out->buf = NULL;
}

/* [한국어]
 * buf_output_free - buf_output 이 소유한 메모리를 해제하고 초기화 상태로 되돌린다.
 *
 * @out: 해제 대상 버퍼. NULL 금지. buf_output_init() 이전 호출은 미정의 동작.
 *
 * free(NULL) 은 NOP 이므로 init 직후 한 번도 add 하지 않은 버퍼에도 안전히 호출 가능.
 * 해제 후 out 은 재초기화되므로 그대로 재사용해도 됨(재호출 시 재할당).
 */
void buf_output_free(struct buf_output *out)
{
	/* [한국어] 기존 할당을 해제(실제 할당이 없었다면 NULL → free 는 NOP) */
	free(out->buf);
	/* [한국어] 포인터 댕글링을 막고 구조체를 재사용 가능 상태로 복원 */
	buf_output_init(out);
}

/*
 * [한국어]
 * buf_output_add - 출력 버퍼에 len 바이트를 append 한다(필요 시 용량 자동 확장).
 *
 * @out: 대상 버퍼. 사전에 init 되어 있어야 함(정상 경로는 buf_output_init 이 보장).
 * @buf: 추가할 데이터 시작 주소. NUL 종결 여부 무관 — 길이 기반 복사.
 * @len: 추가할 바이트 수. 0 허용(즉시 return len).
 * @return: 실제 추가된 바이트 수. 현재 구현은 realloc 실패 시 별도 에러 경로 없이
 *          결과적으로 메모리 훼손/SEGV 로 이어질 수 있으나, fio 는 일반적으로
 *          이 경로에 도달하지 않는다는 전제(잡 러너 자원 확보 후 호출).
 *
 * 동작 단계:
 *   1) 남은 여유(max_buflen - buflen) 가 len 미만이면 확장 분기 진입.
 *      - 추가 필요량 need = len - 여유.
 *      - need 를 max(BUF_INC, need) 로 올림 → realloc 빈도 감소.
 *      - realloc 으로 max_buflen 확장.
 *      - 신규 영역(old_max .. new_max) 을 0 으로 초기화 —
 *        호출자가 나중에 printf 계열로 부분 기록할 때 trailing NUL 을 보장.
 *   2) out->buf[buflen..buflen+len) 에 입력 데이터 복사.
 *   3) buflen 을 len 만큼 전진(append 커서 갱신).
 *
 * 실행 컨텍스트: 보통 잡 스레드/메인 스레드 어디서나 호출. MT 안전이 아니므로
 *               공유 시 호출자가 락 책임.
 *
 * 호출 체인: log_buf/__log_buf/log_info_buf/show_thread_status_* 등 → [buf_output_add]
 *            → realloc/memcpy/memset.
 *
 * 에러 처리: realloc 실패 시 out->buf 가 NULL 이 되고 이후 memcpy 가 SEGV.
 *           fio 는 OOM 상황 자체가 러너 종료를 부를 만한 사건이라 별도 복구 X.
 */
size_t buf_output_add(struct buf_output *out, const char *buf, size_t len)
{
	/* [한국어] 남은 공간 부족 시 realloc 경로 */
	if (out->max_buflen - out->buflen < len) {
		/* [한국어] 필요한 추가 바이트(현재 여유 넘는 분량) */
		size_t need = len - (out->max_buflen - out->buflen);
		/* [한국어] 확장 전 용량을 백업 — 신규 영역 zero-fill 시작 오프셋 계산에 사용 */
		size_t old_max = out->max_buflen;

		/* [한국어] 최소 1 KiB 단위로 확장해 realloc 빈도 최소화 */
		need = max((size_t) BUF_INC, need);
		/* [한국어] 새 용량으로 갱신 후 realloc. realloc(NULL,...) 은 malloc 동치이므로 첫 호출도 안전 */
		out->max_buflen += need;
		out->buf = realloc(out->buf, out->max_buflen);

		/* [한국어] 이미 buflen 이 old_max 를 넘어섰을 수 있으므로(이전 add 에서 확장된 영역 재활용 불가 케이스)
		 * old_max 을 "진짜로 기록이 끝난 위치" 로 재정의해 신규 0-초기화 영역을 정확히 잡는다 */
		old_max = max(old_max, out->buflen + len);
		/* [한국어] old_max + need 가 새 용량을 초과하면 over-write 방지를 위해 need 를 클램프 */
		if (old_max + need > out->max_buflen)
			need = out->max_buflen - old_max;
		/* [한국어] 새로 확보된 꼬리 영역을 0 으로 초기화 —
		 * 호출자가 snprintf 로 이어 쓸 때 trailing NUL 보장 목적 */
		memset(&out->buf[old_max], 0, need);
	}

	/* [한국어] 현재 커서 위치에 사용자 데이터 복사 */
	memcpy(&out->buf[out->buflen], buf, len);
	/* [한국어] 커서 전진 — 다음 add 의 시작점 */
	out->buflen += len;
	/* [한국어] 현재 구현은 항상 len 그대로 반환(부분 append 없음) */
	return len;
}
