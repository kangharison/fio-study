/*
 * [한국어 설명] 동적 확장 출력 버퍼 헤더 (output_buffer.h)
 *
 * === 파일의 역할 ===
 * buf_output 구조체(버퍼 포인터, 현재 길이, 최대 길이)와 buf_output_init,
 * buf_output_free, buf_output_add 함수의 선언을 정의한다.
 *
 * === fio에서의 사용 ===
 * fio의 출력 관련 모듈에서 동적 버퍼를 사용하여 텍스트를 누적 생성할 때
 * 이 헤더를 포함한다.
 */
#ifndef FIO_OUTPUT_BUFFER_H
#define FIO_OUTPUT_BUFFER_H

#include <stddef.h>

struct buf_output {
	char *buf;		/* [한국어] 동적 할당된 버퍼 포인터 (NULL이면 미초기화) */
	size_t buflen;		/* [한국어] 현재 사용 중인 바이트 수 */
	size_t max_buflen;	/* [한국어] 할당된 총 바이트 수 (buflen <= max_buflen) */
};

void buf_output_init(struct buf_output *out);
void buf_output_free(struct buf_output *out);
size_t buf_output_add(struct buf_output *out, const char *buf, size_t len);

#endif
