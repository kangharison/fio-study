/*
 * [한국어 설명] 버퍼 패턴 헤더 (pattern.h)
 *
 * === 파일의 역할 ===
 * I/O 버퍼 패턴 파싱 및 채우기의 공개 API와 관련 구조체를 정의한다.
 * 패턴 포맷 설명자와 파싱 결과 구조체, 그리고 패턴 조작 함수 선언을 포함한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - MAX_PATTERN_SIZE (128MiB): 동적 할당 패턴의 최대 크기 제한
 * - struct pattern_fmt_desc: 포맷 문자열(fmt), 길이(len), paste 콜백을 정의
 * - struct pattern_fmt: 파싱된 포맷의 버퍼 내 위치(off)와 설명자 포인터
 * - parse_and_fill_pattern_alloc: 입력을 파싱하고 결과 버퍼를 자동 할당
 * - cpy_pattern/cmp_pattern: 패턴 복사 및 비교
 * - paste_format/paste_format_inplace: 런타임 포맷 값 삽입
 *
 * === fio에서의 사용 ===
 * verify.c에서 검증 패턴 처리에 사용되고, io_u.c에서 쓰기 버퍼 초기화에 사용된다.
 * --buffer_pattern, --verify_pattern 옵션의 파싱 결과가 이 구조체들에 저장된다.
 */

#ifndef FIO_PARSE_PATTERN_H
#define FIO_PARSE_PATTERN_H

/*
 * The pattern is dynamically allocated, but that doesn't mean there
 * are not limits. The network protocol has a limit of
 * FIO_SERVER_MAX_CMD_MB and potentially two patterns must fit in there.
 * There's also a need to verify the incoming data from the network and
 * this provides a sensible check.
 *
 * 128MiB is an arbitrary limit that meets these criteria. The patterns
 * tend to be truncated at the IO size anyway and IO sizes that large
 * aren't terribly practical.
 */
#define MAX_PATTERN_SIZE	(128 << 20)

/**
 * Pattern format description. The input for 'parse_pattern'.
 * Describes format with its name and callback, which should
 * be called to paste something inside the buffer.
 */
struct pattern_fmt_desc {
	const char  *fmt;
	unsigned int len;
	int (*paste)(char *buf, unsigned int len, void *priv);
};

/**
 * Pattern format. The output of 'parse_pattern'.
 * Describes the exact position inside the xbuffer.
 */
struct pattern_fmt {
	unsigned int off;
	const struct pattern_fmt_desc *desc;
};

int parse_and_fill_pattern_alloc(const char *in, unsigned int in_len,
		char **out, const struct pattern_fmt_desc *fmt_desc,
		struct pattern_fmt *fmt, unsigned int *fmt_sz_out);

int paste_format_inplace(char *pattern, unsigned int pattern_len,
			 struct pattern_fmt *fmt, unsigned int fmt_sz,
			 void *priv);

int paste_format(const char *pattern, unsigned int pattern_len,
		 struct pattern_fmt *fmt, unsigned int fmt_sz,
		 char *out, unsigned int out_len, void *priv);

int cpy_pattern(const char *pattern, unsigned int pattern_len,
		char *out, unsigned int out_len);

int cmp_pattern(const char *pattern, unsigned int pattern_size,
		unsigned int off, const char *buf, unsigned int len);

#endif
