/*
 * [한국어 설명] 동적 확장 출력 버퍼 (output_buffer.c)
 *
 * === 파일의 역할 ===
 * 필요에 따라 자동으로 크기가 확장되는 출력 버퍼를 구현한다.
 * buf_output_init으로 초기화하고, buf_output_add로 데이터를 추가하면
 * 버퍼가 부족할 때 1024바이트 단위로 자동 확장(realloc)된다.
 *
 * === fio에서의 사용 ===
 * fio의 통계 출력이나 로그 메시지 등 형식화된 텍스트를 점진적으로 구성할 때
 * 사용된다. 최종 결과를 한 번에 출력하기 위해 버퍼에 텍스트를 누적한다.
 */
#include <string.h>
#include <stdlib.h>

#include "output_buffer.h"
#include "../minmax.h"

/* [한국어] 버퍼 확장 시 최소 증가 단위 (바이트) */
#define BUF_INC	1024

/* [한국어] buf_output_init - 출력 버퍼를 빈 상태로 초기화 */
void buf_output_init(struct buf_output *out)
{
	out->max_buflen = 0;
	out->buflen = 0;
	out->buf = NULL;
}

void buf_output_free(struct buf_output *out)
{
	free(out->buf);
	buf_output_init(out);
}

/*
 * [한국어] buf_output_add - 출력 버퍼에 데이터를 추가 (필요시 자동 확장)
 *
 * @out: 출력 버퍼
 * @buf: 추가할 데이터
 * @len: 데이터 길이
 * @return: 추가된 바이트 수 (항상 len)
 *
 * 남은 공간이 부족하면 BUF_INC(1024) 이상으로 realloc하여 확장한다.
 */
size_t buf_output_add(struct buf_output *out, const char *buf, size_t len)
{
	/* [한국어] 남은 공간 부족 시 자동 확장 */
	if (out->max_buflen - out->buflen < len) {
		size_t need = len - (out->max_buflen - out->buflen);
		size_t old_max = out->max_buflen;

		need = max((size_t) BUF_INC, need);
		out->max_buflen += need;
		out->buf = realloc(out->buf, out->max_buflen);

		old_max = max(old_max, out->buflen + len);
		if (old_max + need > out->max_buflen)
			need = out->max_buflen - old_max;
		memset(&out->buf[old_max], 0, need);
	}

	memcpy(&out->buf[out->buflen], buf, len);
	out->buflen += len;
	return len;
}
