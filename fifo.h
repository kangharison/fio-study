#ifndef FIO_FIFO_H
#define FIO_FIFO_H
/*
 * A simple FIFO implementation.
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
 * [한국어] fifo.h - 커널 스타일 FIFO 원형 버퍼 헤더
 *
 * Linux kfifo를 단순화한 FIFO 구현의 구조체 및 인터페이스를 정의한다.
 * 버퍼 크기는 2의 거듭제곱이어야 하며, in/out 인덱스는 단조 증가한다.
 */

/* [한국어] FIFO 버퍼 구조체 */
struct fifo {
	unsigned char *buffer;	/* the buffer holding the data */    /* 데이터를 저장하는 버퍼 */
	unsigned int size;	/* the size of the allocated buffer */    /* 할당된 버퍼 크기 (2의 거듭제곱) */
	unsigned int in;	/* data is added at offset (in % size) */    /* 쓰기 오프셋 (단조 증가) */
	unsigned int out;	/* data is extracted from off. (out % size) */    /* 읽기 오프셋 (단조 증가) */
};

struct fifo *fifo_alloc(unsigned int);
unsigned int fifo_put(struct fifo *, void *, unsigned int);
unsigned int fifo_get(struct fifo *, void *, unsigned int);
void fifo_free(struct fifo *);

/* [한국어] FIFO에 저장된 데이터의 바이트 수를 반환 */
static inline unsigned int fifo_len(struct fifo *fifo)
{
	return fifo->in - fifo->out;
}

/* [한국어] FIFO의 남은 빈 공간(바이트)을 반환 */
static inline unsigned int fifo_room(struct fifo *fifo)
{
	return fifo->size - fifo->in + fifo->out;
}

#endif
