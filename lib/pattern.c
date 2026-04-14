/*
 * [한국어 설명] 버퍼 패턴 채우기/파싱 (pattern.c)
 *
 * === 파일의 역할 ===
 * 사용자가 지정한 패턴 문자열을 파싱하여 I/O 버퍼를 채우는 기능을 구현한다.
 * 16진수, 10진수, 문자열, 파일 내용, 포맷 지시자(%o 등)를 조합한 복합 패턴을 지원하며,
 * 패턴을 버퍼 전체에 반복 복사하고 검증하는 기능도 제공한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - parse_and_fill_pattern: 입력 문자열을 순회하며 타입별 파서(문자열/숫자/파일/포맷)를 호출
 * - parse_number: 0x 접두사로 16진수, 그 외 10진수 파싱 (INT_MIN~INT_MAX 범위)
 * - parse_format: %o 같은 포맷 지시자를 인식하여 런타임에 값을 삽입할 위치 예약
 * - dup_pattern: memcpy 더블링으로 패턴을 버퍼 전체에 O(log n) 시간에 복사
 * - cmp_pattern: 패턴 반복 구조를 이용한 효율적 버퍼 검증 (루프 최소화)
 * - paste_format_inplace/paste_format: 포맷 콜백으로 동적 값(오프셋 등)을 버퍼에 삽입
 *
 * === fio에서의 사용 ===
 * --buffer_pattern과 --verify_pattern 옵션을 처리한다. 쓰기 시 지정된 패턴으로
 * 버퍼를 채우고, 검증 시 읽은 데이터와 패턴을 비교하여 데이터 무결성을 확인한다.
 * %o 포맷으로 블록 오프셋을 삽입하면 각 블록이 고유한 내용을 갖게 된다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "strntol.h"
#include "pattern.h"
#include "../minmax.h"
#include "../oslib/strcasestr.h"
#include "../oslib/strndup.h"

/**
 * parse_file() - parses binary file to fill buffer
 * @beg - string input, extract filename from this
 * @out - output buffer where parsed number should be put
 * @out_len - length of the output buffer
 * @filled - pointer where number of bytes successfully
 *           parsed will be put
 *
 * Returns the end pointer where parsing has been stopped.
 * In case of parsing error or lack of bytes in output buffer
 * NULL will be returned.
 */
static const char *parse_file(const char *beg, char *out,
			      unsigned int out_len,
			      unsigned int *filled)
{
	const char *end;
	char *file;
	int fd;
	ssize_t rc, count = 0;

	if (!out_len)
		goto err_out;

	assert(*beg == '\'');
	beg++;
	end = strchr(beg, '\'');
	if (!end)
		goto err_out;

	file = strndup(beg, end - beg);
	if (file == NULL)
		goto err_out;

#ifdef _WIN32
	fd = open(file, O_RDONLY | O_BINARY);
#else
	fd = open(file, O_RDONLY);
#endif
	if (fd < 0)
		goto err_free_out;

	if (out) {
		while (1) {
			rc = read(fd, out, out_len - count);
			if (rc == 0)
				break;
			if (rc == -1)
				goto err_free_close_out;

			count += rc;
			out += rc;
		}
	} else {
		count = lseek(fd, 0, SEEK_END);
		if (count == -1)
			goto err_free_close_out;
		if (count >= out_len)
			count = out_len;
	}

	*filled = count;
	close(fd);
	free(file);

	/* Catch up quote */
	return end + 1;

err_free_close_out:
	close(fd);
err_free_out:
	free(file);
err_out:
	return NULL;

}

/**
 * parse_string() - parses string in double quotes, like "abc"
 * @beg - string input
 * @out - output buffer where parsed number should be put
 * @out_len - length of the output buffer
 * @filled - pointer where number of bytes successfully
 *           parsed will be put
 *
 * Returns the end pointer where parsing has been stopped.
 * In case of parsing error or lack of bytes in output buffer
 * NULL will be returned.
 */
static const char *parse_string(const char *beg, char *out,
				unsigned int out_len,
				unsigned int *filled)
{
	const char *end;

	if (!out_len)
		return NULL;

	assert(*beg == '"');
	beg++;
	end = strchr(beg, '"');
	if (!end)
		return NULL;
	if (end - beg > out_len)
		return NULL;

	if (out)
		memcpy(out, beg, end - beg);
	*filled = end - beg;

	/* Catch up quote */
	return end + 1;
}

/**
 * parse_number() - parses numbers
 * @beg - string input
 * @out - output buffer where parsed number should be put
 * @out_len - length of the output buffer
 * @filled - pointer where number of bytes successfully
 *           parsed will be put
 *
 * Supports decimals in the range [INT_MIN, INT_MAX] and
 * hexidecimals of any size, which should be started with
 * prefix 0x or 0X.
 *
 * Returns the end pointer where parsing has been stopped.
 * In case of parsing error or lack of bytes in output buffer
 * NULL will be returned.
 */
static const char *parse_number(const char *beg, char *out,
				unsigned int out_len,
				unsigned int *filled)
{
	const char *end;
	unsigned int val;
	long lval;
	int num, i;

	if (!out_len)
		return NULL;

	num = 0;
	sscanf(beg, "0%*[xX]%*[0-9a-fA-F]%n", &num);
	if (num == 0) {
		/* Here we are trying to parse decimal */

		char *_end;

		/* Looking ahead */
		_end = strcasestr(beg, "0x");
		if (_end)
			num = _end - beg;
		if (num)
			lval = strntol(beg, num, &_end, 10);
		else
			lval = strtol(beg, &_end, 10);
		if (beg == _end || lval > INT_MAX || lval < INT_MIN)
			return NULL;
		end = _end;
		i = 0;
		if (!lval) {
			num    = 0;
			if (out)
				out[i] = 0x00;
			i      = 1;
		} else {
			val = (unsigned int)lval;
			for (; val && out_len; out_len--, i++, val >>= 8)
				if (out)
					out[i] = val & 0xff;
			if (val)
				return NULL;
		}
	} else {
		assert(num > 2);

		/* Catch up 0x prefix */
		num -= 2;
		beg += 2;

		/* Look back, handle this combined string: 0xff0x14 */
		if (beg[num] && !strncasecmp(&beg[num - 1], "0x", 2))
			num--;

		end  = beg + num;

		for (i = 0; num && out_len;
		     out_len--, i++, num -= 2, beg += 2) {
			const char *fmt;

			fmt = (num & 1 ? "%1hhx" : "%2hhx");
			if (out)
				sscanf(beg, fmt, &out[i]);
			if (num & 1) {
				num++;
				beg--;
			}
		}
		if (num)
			return NULL;
	}

	*filled = i;
	return end;

}

/**
 * parse_format() - parses formats, like %o, etc
 * @in - string input
 * @out - output buffer where space for format should be reserved
 * @parsed - number of bytes which were already parsed so far
 * @out_len - length of the output buffer
 * @fmt_desc - format descriptor array, what we expect to find
 * @fmt - format array, the output
 * @fmt_sz - size of format array
 *
 * This function tries to find formats, e.g.:
 *   %o - offset of the block
 *
 * In case of successful parsing it fills the format param
 * with proper offset and the size of the expected value, which
 * should be pasted into buffer using the format 'func' callback.
 *
 * Returns the end pointer where parsing has been stopped.
 * In case of parsing error or lack of bytes in output buffer
 * NULL will be returned.
 */
static const char *parse_format(const char *in, char *out, unsigned int parsed,
				unsigned int out_len, unsigned int *filled,
				const struct pattern_fmt_desc *fmt_desc,
				struct pattern_fmt *fmt, unsigned int fmt_sz)
{
	int i;
	struct pattern_fmt *f = NULL;
	unsigned int len = 0;

	if (!out_len || !fmt_desc || !fmt || !fmt_sz)
		return NULL;

	assert(*in == '%');

	for (i = 0; fmt_desc[i].fmt; i++) {
		const struct pattern_fmt_desc *desc;

		desc = &fmt_desc[i];
		len  = strlen(desc->fmt);
		if (0 == strncmp(in, desc->fmt, len)) {
			fmt->desc = desc;
			fmt->off  = parsed;
			f = fmt;
			break;
		}
	}

	if (!f)
		return NULL;
	if (f->desc->len > out_len)
		return NULL;

	if (out)
		memset(out, '\0', f->desc->len);
	*filled = f->desc->len;

	return in + len;
}

/**
 * parse_and_fill_pattern() - Parses combined input, which consists of strings,
 *                            numbers and pattern formats.
 * @in - string input
 * @in_len - size of the input string
 * @out - output buffer where parsed result will be put, may be NULL
 *	  in which case this function just calculates the required
 *	  length of the buffer
 * @out_len - lengths of the output buffer
 * @fmt_desc - array of pattern format descriptors [input]
 * @fmt - array of pattern formats [output]
 * @fmt_sz - pointer where the size of pattern formats array stored [input],
 *           after successful parsing this pointer will contain the number
 *           of parsed formats if any [output].
 *
 * strings:
 *   bytes sequence in double quotes, e.g. "123".
 *   NOTE: there is no way to escape quote, so "123\"abc" does not work.
 *
 * numbers:
 *   hexadecimal - sequence of hex bytes starting from 0x or 0X prefix,
 *                 e.g. 0xff12ceff1100ff
 *   decimal     - decimal number in range [INT_MIN, INT_MAX]
 *
 * formats:
 *   %o - offset of block, reserved 8 bytes.
 *
 * Explicit examples of combined string:
 * #1                  #2                 #3        #4
 *    in="abcd"          in=-1024           in=66     in=0xFF0X1
 *   out=61 62 63 64    out=00 fc ff ff    out=42    out=ff 01
 *
 * #5                                #6
 *    in=%o                            in="123"0xFFeeCC
 *   out=00 00 00 00 00 00 00 00      out=31 32 33 ff ec cc
 *
 * #7
 *   in=-100xab"1"%o"2"
 *  out=f6 ff ff ff ab 31 00 00 00 00 00 00 00 00 32
 *
 * #9
 *    in=%o0xdeadbeef%o
 *   out=00 00 00 00 00 00 00 00 de ad be ef 00 00 00 00 00 00 00 00
 *
 * #10
 *    in=0xfefefefefefefefefefefefefefefefefefefefefe
 *   out=fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe
 *
 * Returns number of bytes filled or err < 0 in case of failure.
 */
/*
 * [한국어] parse_and_fill_pattern - 복합 패턴 문자열을 파싱하여 버퍼에 채움
 *
 * @in: 입력 패턴 문자열 (예: "0xff""hello"%o-100)
 * @in_len: 입력 길이
 * @out: 출력 버퍼 (NULL이면 필요한 길이만 계산)
 * @out_len: 출력 버퍼 길이
 * @fmt_desc: 포맷 설명자 배열 (%o 등의 정의)
 * @fmt: 파싱된 포맷 정보 배열 [출력]
 * @fmt_sz_out: 포맷 배열 크기 [입출력]
 * @return: 채워진 바이트 수 (에러 시 -EINVAL)
 *
 * 입력 문자열을 순회하며 첫 문자로 타입을 판별한다:
 * - ': 파일 경로 (parse_file)
 * - ": 문자열 (parse_string)
 * - %: 포맷 지시자 (parse_format)
 * - 기타: 숫자 (parse_number, 0x 접두사로 16진수 판별)
 *
 * 호출 체인: parse_and_fill_pattern_alloc() → [parse_and_fill_pattern]
 */
static int parse_and_fill_pattern(const char *in, unsigned int in_len,
				  char *out, unsigned int out_len,
				  const struct pattern_fmt_desc *fmt_desc,
				  struct pattern_fmt *fmt,
				  unsigned int *fmt_sz_out)
{
	const char *beg, *end, *out_beg = out;
	unsigned int total = 0, fmt_rem = 0;

	if (!in || !in_len || !out_len)
		return -EINVAL;
	if (fmt_sz_out)
		fmt_rem = *fmt_sz_out;

	beg = in;
	do {
		unsigned int filled;
		int parsed_fmt;

		filled     = 0;
		parsed_fmt = 0;

		switch (*beg) {
		case '\'':
			end = parse_file(beg, out, out_len, &filled);
			break;
		case '"':
			end = parse_string(beg, out, out_len, &filled);
			break;
		case '%':
			end = parse_format(beg, out, out - out_beg, out_len,
					   &filled, fmt_desc, fmt, fmt_rem);
			parsed_fmt = 1;
			break;
		default:
			end = parse_number(beg, out, out_len, &filled);
			break;
		}

		if (!end)
			return -EINVAL;

		if (parsed_fmt) {
			assert(fmt_rem);
			fmt_rem--;
			fmt++;
		}

		assert(end - beg <= in_len);
		in_len -= end - beg;
		beg     = end;

		assert(filled);
		assert(filled <= out_len);
		out_len -= filled;
		total   += filled;
		if (out)
			out += filled;

	} while (in_len);

	if (fmt_sz_out)
		*fmt_sz_out -= fmt_rem;
	return total;
}

/**
 * parse_and_fill_pattern_alloc() - Parses combined input, which consists of
 *				    strings, numbers and pattern formats and
 *				    allocates a buffer for the result.
 *
 * @in - string input
 * @in_len - size of the input string
 * @out - pointer to the output buffer pointer, this will be set to the newly
 *        allocated pattern buffer which must be freed by the caller
 * @fmt_desc - array of pattern format descriptors [input]
 * @fmt - array of pattern formats [output]
 * @fmt_sz - pointer where the size of pattern formats array stored [input],
 *           after successful parsing this pointer will contain the number
 *           of parsed formats if any [output].
 *
 * See documentation on parse_and_fill_pattern() above for a description
 * of the functionality.
 *
 * Returns number of bytes filled or err < 0 in case of failure.
 */
/*
 * [한국어] parse_and_fill_pattern_alloc - 패턴을 파싱하고 결과 버퍼를 자동 할당
 *
 * 먼저 out=NULL로 호출하여 필요한 크기를 계산하고, malloc한 뒤 다시 파싱한다.
 * 호출자가 반환된 *out을 free()해야 한다.
 *
 * 호출 체인: options.c (set_verify_pattern 등) → [parse_and_fill_pattern_alloc]
 */
int parse_and_fill_pattern_alloc(const char *in, unsigned int in_len,
		char **out, const struct pattern_fmt_desc *fmt_desc,
		struct pattern_fmt *fmt, unsigned int *fmt_sz_out)
{
	int count;

	/* [한국어] 1차 호출: out=NULL로 필요한 크기만 계산 */
	count = parse_and_fill_pattern(in, in_len, NULL, MAX_PATTERN_SIZE,
				       fmt_desc, fmt, fmt_sz_out);
	if (count < 0)
		return count;

	/* [한국어] 2차 호출: 계산된 크기로 할당 후 실제 파싱 */
	*out = malloc(count);
	count = parse_and_fill_pattern(in, in_len, *out, count, fmt_desc,
				       fmt, fmt_sz_out);
	if (count < 0) {
		free(*out);
		*out = NULL;
	}

	return count;
}

/**
 * dup_pattern() - Duplicates part of the pattern all over the buffer.
 *
 * Returns 0 in case of success or errno < 0 in case of failure.
 */
static int dup_pattern(char *out, unsigned int out_len, unsigned int pattern_len)
{
	unsigned int left, len, off;

	if (out_len <= pattern_len)
		/* Normal case */
		return 0;

	off  = pattern_len;
	left = (out_len - off);
	len  = min(left, off);

	/* Duplicate leftover
	 * [한국어] 더블링 기법: 채워진 영역을 2배씩 복사하여 O(log n) 시간에 전체 버퍼를 채움.
	 * 첫 복사는 pattern_len바이트, 다음은 2*pattern_len, 그 다음은 4*pattern_len... */
	while (left) {
		memcpy(out + off, out, len);
		left -= len;
		off <<= 1;
		len   = min(left, off);
	}

	return 0;
}

/**
 * cpy_pattern() - Copies pattern to the buffer.
 *
 * Function copies pattern along the whole buffer.
 *
 * Returns 0 in case of success or errno < 0 in case of failure.
 */
int cpy_pattern(const char *pattern, unsigned int pattern_len,
		char *out, unsigned int out_len)
{
	unsigned int len;

	if (!pattern || !pattern_len || !out || !out_len)
		return -EINVAL;

	/* Copy pattern */
	len = min(pattern_len, out_len);
	memcpy(out, pattern, len);

	/* Spread filled chunk all over the buffer */
	return dup_pattern(out, out_len, pattern_len);
}

/**
 * cmp_pattern() - Compares pattern and buffer.
 *
 * For the sake of performance this function avoids any loops.
 * Firstly it tries to compare the buffer itself, checking that
 * buffer consists of repeating patterns along the buffer size.
 *
 * If the difference is not found then the function tries to compare
 * buffer and pattern.
 *
 * Returns 0 in case of success or errno < 0 in case of failure.
 */
/*
 * [한국어] cmp_pattern - 버퍼가 패턴의 반복으로 구성되어 있는지 비교
 *
 * @pattern: 원본 패턴
 * @pattern_size: 패턴 크기
 * @off: 패턴 내 시작 오프셋 (버퍼가 패턴의 중간부터 시작할 때)
 * @buf: 비교할 버퍼
 * @len: 버퍼 길이
 * @return: 0=일치, -EILSEQ=불일치
 *
 * 루프를 최소화하기 위해 먼저 버퍼 내부의 반복 패턴을 비교하고(자기 자신과 비교),
 * 그 다음 패턴과 직접 비교하여 2번의 memcmp로 검증을 완료한다.
 *
 * 호출 체인: verify.c → [cmp_pattern]
 */
int cmp_pattern(const char *pattern, unsigned int pattern_size,
		unsigned int off, const char *buf, unsigned int len)
{
	int rc;
	unsigned int size;

	/* Find the difference in buffer
	 * [한국어] 버퍼 내부의 반복 일관성을 확인 (buf[0..] vs buf[pattern_size..]) */
	if (len > pattern_size) {
		rc = memcmp(buf, buf + pattern_size, len - pattern_size);
		if (rc)
			return -EILSEQ;
	}
	/* Compare second part of the pattern with buffer */
	if (off) {
		size = min(len, pattern_size - off);
		rc = memcmp(buf, pattern + off, size);
		if (rc)
			return -EILSEQ;
		buf += size;
		len -= size;
	}
	/* Compare first part of the pattern or the whole pattern
	 * with buffer */
	if (len) {
		size = min(len, (off ? off : pattern_size));
		rc = memcmp(buf, pattern, size);
		if (rc)
			return -EILSEQ;
	}

	return 0;
}

/**
 * paste_format_inplace() - Pastes parsed formats to the pattern.
 *
 * This function pastes formats to the pattern. If @fmt_sz is 0
 * function does nothing and pattern buffer is left untouched.
 *
 * Returns 0 in case of success or errno < 0 in case of failure.
 */
int paste_format_inplace(char *pattern, unsigned int pattern_len,
			 struct pattern_fmt *fmt, unsigned int fmt_sz,
			 void *priv)
{
	int i, rc;
	unsigned int len;

	if (!pattern || !pattern_len || !fmt)
		return -EINVAL;

	/* Paste formats for first pattern chunk */
	for (i = 0; i < fmt_sz; i++) {
		struct pattern_fmt *f;

		f = &fmt[i];
		if (pattern_len <= f->off)
			break;
		len = min(pattern_len - f->off, f->desc->len);
		rc  = f->desc->paste(pattern + f->off, len, priv);
		if (rc)
			return rc;
	}

	return 0;
}

/**
 * paste_format() - Pastes parsed formats to the buffer.
 *
 * This function copies pattern to the buffer, pastes format
 * into it and then duplicates pattern all over the buffer size.
 *
 * Returns 0 in case of success or errno < 0 in case of failure.
 */
int paste_format(const char *pattern, unsigned int pattern_len,
		 struct pattern_fmt *fmt, unsigned int fmt_sz,
		 char *out, unsigned int out_len, void *priv)
{
	int rc;
	unsigned int len;

	if (!pattern || !pattern_len || !out || !out_len)
		return -EINVAL;

	/* Copy pattern */
	len = min(pattern_len, out_len);
	memcpy(out, pattern, len);

	rc = paste_format_inplace(out, len, fmt, fmt_sz, priv);
	if (rc)
		return rc;

	/* Spread filled chunk all over the buffer */
	return dup_pattern(out, out_len, pattern_len);
}
