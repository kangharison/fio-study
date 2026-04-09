/*
 * A simple kernel FIFO implementation.
 *
 * Copyright (C) 2004 Stelian Pop <stelian@popies.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
/*
 * [한국어] fifo.c - 커널 스타일 FIFO 원형 버퍼 구현
 *
 * Linux 커널의 kfifo를 단순화한 FIFO(선입선출) 버퍼 구현이다.
 * 버퍼 크기는 2의 거듭제곱이어야 하며, 비트 AND 연산으로 인덱스를 래핑한다.
 * in/out 인덱스가 단조 증가하므로 별도의 래핑 처리 없이 오버플로를 자연스럽게 처리한다.
 *
 * 주요 함수:
 *   fifo_alloc() - FIFO 버퍼 생성
 *   fifo_free()  - FIFO 버퍼 해제
 *   fifo_put()   - 데이터 삽입 (쓰기)
 *   fifo_get()   - 데이터 추출 (읽기)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fifo.h"
#include "minmax.h"

/* [한국어] FIFO 버퍼를 할당하고 초기화하는 함수
 * size는 2의 거듭제곱이어야 비트 AND 래핑이 올바르게 동작한다. */
struct fifo *fifo_alloc(unsigned int size)
{
	struct fifo *fifo;

	fifo = malloc(sizeof(struct fifo));
	if (!fifo)
		return NULL;

	fifo->buffer = malloc(size);
	fifo->size = size;
	fifo->in = fifo->out = 0;

	return fifo;
}

/* [한국어] FIFO 버퍼를 해제하는 함수 */
void fifo_free(struct fifo *fifo)
{
	free(fifo->buffer);
	free(fifo);
}

/* [한국어] FIFO에 데이터를 삽입하는 함수
 * 버퍼 끝과 처음에 걸쳐 두 번에 나눠 복사할 수 있다 (원형 버퍼).
 * 반환값은 실제로 삽입된 바이트 수이다. */
unsigned int fifo_put(struct fifo *fifo, void *buffer, unsigned int len)
{
	unsigned int l;

	len = min(len, fifo_room(fifo));

	/* first put the data starting from fifo->in to buffer end */
	/* 버퍼의 in 위치부터 끝까지 복사 */
	l = min(len, fifo->size - (fifo->in & (fifo->size - 1)));
	memcpy(fifo->buffer + (fifo->in & (fifo->size - 1)), buffer, l);

	/* then put the rest (if any) at the beginning of the buffer */
	/* 나머지를 버퍼 처음부터 복사 (래핑) */
	memcpy(fifo->buffer, buffer + l, len - l);

	/*
	 * Ensure that we add the bytes to the fifo -before-
	 * we update the fifo->in index.
	 */

	fifo->in += len;

	return len;
}

/* [한국어] FIFO에서 데이터를 추출하는 함수
 * buf가 NULL이면 데이터를 버린다 (skip 용도).
 * in과 out이 같아지면 인덱스를 0으로 리셋한다.
 * 반환값은 실제로 추출된 바이트 수이다. */
unsigned int fifo_get(struct fifo *fifo, void *buf, unsigned int len)
{
	len = min(len, fifo->in - fifo->out);

	if (buf) {
		unsigned int l;

		/*
		 * first get the data from fifo->out until the end of the buffer
		 */
		/* out 위치부터 버퍼 끝까지 복사 */
		l = min(len, fifo->size - (fifo->out & (fifo->size - 1)));
		memcpy(buf, fifo->buffer + (fifo->out & (fifo->size - 1)), l);

		/*
		 * then get the rest (if any) from the beginning of the buffer
		 */
		/* 나머지를 버퍼 처음부터 복사 (래핑) */
		memcpy(buf + l, fifo->buffer, len - l);
	}

	fifo->out += len;

	/* in과 out이 같으면 인덱스 리셋으로 오버플로 방지 */
	if (fifo->in == fifo->out)
		fifo->in = fifo->out = 0;

	return len;
}
