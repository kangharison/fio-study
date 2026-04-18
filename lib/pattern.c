/*
 * [한국어 설명] verify/buffer 패턴 파서 및 버퍼 채움/비교기 (pattern.c)
 *
 * === 파일의 역할 ===
 * fio 의 `--verify_pattern=`, `--buffer_pattern=` 옵션을 해석하고, 그 결과를
 * I/O 버퍼에 반복 채워넣거나(쓰기 경로) 읽은 버퍼와 비교하는(검증 경로) 기능을
 * 제공한다. 사용자가 지정할 수 있는 패턴 문법은:
 *   - 16진수 바이트 시퀀스 : `0xdeadbeef`, `0XFFeeCC`  (임의 길이)
 *   - 10진수 정수        : `-1024`, `66` (INT_MIN..INT_MAX, little-endian 배치)
 *   - 이중 따옴표 문자열   : `"hello"` (바이트 그대로)
 *   - 단일 따옴표 파일    : `'path/to/bin'` (바이너리 파일 내용 로드)
 *   - 포맷 지시자        : `%o` (블록 오프셋 — 쓰기 시점에 paste 콜백이 채움)
 * 이 요소들은 한 문자열 안에서 자유롭게 연결 가능하며(예: `-10"abc"0xffdead%o`),
 * 본 파일이 좌→우로 타입 판별 후 각 파서를 호출하고, 결과를 연속 바이트로
 * out 버퍼에 기록한다. 이후 `cpy_pattern` 이 "더블링 기법(O(log n) memcpy)"
 * 으로 buffer_pattern 을 전체 I/O 버퍼에 반복 복사하고, `cmp_pattern` 이
 * 검증 시 반복 구조를 활용해 단 2 회의 memcmp 로 버퍼 일관성을 확인한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 쓰기 경로 (잡 스레드):
 *   options.c: fio_set_option_value → str_verify_pattern_cb / str_buffer_pattern_cb
 *     → parse_and_fill_pattern_alloc()  // [본 파일] 2-pass 파싱 + malloc
 *         → parse_and_fill_pattern()    // 실제 바이트 채움
 *             ├─ parse_file / parse_string / parse_format / parse_number
 *   io_u.c: fill_io_buffer / fill_verify_pattern
 *     → cpy_pattern(pattern, pat_len, buf, buf_len)
 *         → dup_pattern()               // memcpy 더블링
 *     → paste_format(pattern, ..., priv) // %o 등 런타임 값을 삽입
 *
 * 검증 경로 (잡 스레드 또는 verify thread):
 *   verify.c: verify_io_u_pattern
 *     → cmp_pattern()                    // 2 회 memcmp 로 검증
 *
 * 실행 컨텍스트: parse_* 는 옵션 파싱 시 메인 스레드, cpy/cmp/paste 는 잡 스레드.
 * 모든 함수는 순수 함수 또는 호출자 소유 버퍼만 다루므로 재진입 안전(다만
 * parse_file 은 파일 I/O 가 있음).
 *
 * === 타 모듈과의 연결 ===
 * - pattern.h : struct pattern_fmt / pattern_fmt_desc 정의, 공개 API 선언.
 *               pattern_fmt_desc::paste 는 런타임 콜백 (예: 오프셋 insert).
 * - strntol.h : strntol() — 길이 제한이 있는 strtol 버전. parse_number 의 10진 분기에서 사용.
 * - ../minmax.h : min(a, b) 매크로 — dup_pattern, cmp_pattern, paste_format 의 클램프.
 * - ../oslib/strcasestr.h : strcasestr() — parse_number 가 "0x" 접두 위치를 찾는 데 사용(일부 OS 비표준).
 * - ../oslib/strndup.h : strndup() — parse_file 에서 파일명 복제 (길이 제한 복제).
 * - options.c : set_verify_pattern / set_buffer_pattern 옵션 콜백이 parse_and_fill_pattern_alloc 호출.
 * - verify.c : cmp_pattern 호출로 읽은 버퍼를 검증.
 * - io_u.c : cpy_pattern / paste_format 호출로 쓰기 버퍼 준비.
 *
 * === 주요 함수/구조체 요약 ===
 * - parse_and_fill_pattern(in, in_len, out, out_len, fmt_desc, fmt, *fmt_sz_out):
 *     문자열을 타입별로 판별하여 out 버퍼에 바이트 채움. out=NULL 이면 크기만 계산.
 * - parse_and_fill_pattern_alloc(in, in_len, **out, fmt_desc, fmt, *fmt_sz_out):
 *     위 함수 2-pass 사용하여 필요한 크기 계산 후 malloc → 파싱.
 * - parse_file(beg, out, out_len, *filled): `'filename'` → 파일 내용 로드.
 * - parse_string(beg, ...): `"text"` → 리터럴 바이트 복사.
 * - parse_number(beg, ...): 16진/10진 숫자 → little-endian 바이트열.
 * - parse_format(in, out, parsed, ...): `%o` 등 포맷 지시자 → 공간 예약 + fmt[] 배열 기록.
 * - cpy_pattern(pattern, pat_len, out, out_len): 전체 out 에 pattern 반복 복사.
 * - dup_pattern(out, out_len, pattern_len): out 의 앞부분을 더블링으로 전체 확장.
 * - cmp_pattern(pattern, pat_size, off, buf, len): 반복 구조 활용 2-pass 검증.
 * - paste_format_inplace/paste_format: 런타임 값(예: 블록 오프셋)을 fmt->off 자리에 삽입.
 */

#include <stdio.h>	/* [한국어] sscanf — parse_number 가 hex 바이트 쌍을 %hhx 포맷으로 파싱하는 데 사용 */
#include <stdlib.h>	/* [한국어] malloc/free — parse_and_fill_pattern_alloc 의 2-pass 할당 */
#include <string.h>	/* [한국어] strchr/strncmp/strncasecmp/memcmp/memcpy/memset/strlen — 파싱 및 버퍼 조작 */
#include <limits.h>	/* [한국어] INT_MIN/INT_MAX — parse_number 의 10진 범위 검증 */
#include <errno.h>	/* [한국어] -EINVAL/-EILSEQ 등 음수 에러 코드 반환 규약 */
#include <assert.h>	/* [한국어] 경계 불변식 확인 */
#include <fcntl.h>	/* [한국어] open() O_RDONLY (_WIN32 에서는 O_BINARY 추가) */
#include <unistd.h>	/* [한국어] read/close/lseek — parse_file 의 바이너리 I/O */

#include "strntol.h"			/* [한국어] strntol(길이 제한 strtol) — parse_number 10진 분기 */
#include "pattern.h"			/* [한국어] struct pattern_fmt, pattern_fmt_desc, MAX_PATTERN_SIZE 등 공용 정의 */
#include "../minmax.h"			/* [한국어] min() 매크로 */
#include "../oslib/strcasestr.h"	/* [한국어] 대소문자 무시 부분문자열 검색 — "0x" 포함 여부 조회 */
#include "../oslib/strndup.h"		/* [한국어] strndup — 길이 제한 복제 (파일명 추출) */

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
/*
 * [한국어] parse_file - `'path'` 형태의 단일 따옴표로 감싼 파일 경로를 읽어 out 에 내용 로드
 *
 * @beg:     첫 문자가 `'` 이어야 하는 입력 포인터
 * @out:     출력 버퍼 (NULL 허용 — 크기 계산만)
 * @out_len: 출력 버퍼 크기
 * @filled:  [out] 실제 로드된 바이트 수
 * @return:  파싱 종료 후 다음 문자 위치, 에러 시 NULL
 *
 * 동작: 여는 따옴표 다음부터 닫는 따옴표까지 strndup 으로 파일명 추출 →
 *       open(..., O_RDONLY [| O_BINARY on Windows]) → read 루프로 out 에 적재.
 *       out=NULL 이면 파일 크기만 lseek(SEEK_END) 로 측정.
 * 에러: 버퍼 부족, 따옴표 부재, open/read 실패 시 NULL 반환 (리소스 정리).
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
		goto err_out;					/* [한국어] 저장 공간 없음 → 에러 */

	assert(*beg == '\'');					/* [한국어] 호출자는 첫 문자를 `'` 로 보장해야 함 */
	beg++;							/* [한국어] 여는 따옴표 건너뜀 */
	end = strchr(beg, '\'');				/* [한국어] 닫는 따옴표 검색 */
	if (!end)
		goto err_out;					/* [한국어] 닫는 따옴표 없음 → 문법 오류 */

	file = strndup(beg, end - beg);				/* [한국어] 따옴표 사이 파일명 복제 (NUL 종료) */
	if (file == NULL)
		goto err_out;					/* [한국어] 메모리 부족 */

#ifdef _WIN32
	fd = open(file, O_RDONLY | O_BINARY);			/* [한국어] Windows: 텍스트 변환 방지를 위해 O_BINARY 추가 */
#else
	fd = open(file, O_RDONLY);				/* [한국어] POSIX: 기본 바이너리 */
#endif
	if (fd < 0)
		goto err_free_out;				/* [한국어] 파일 오픈 실패 */

	if (out) {						/* [한국어] 실제 읽기 단계 (out 이 있으면) */
		while (1) {
			rc = read(fd, out, out_len - count);	/* [한국어] 남은 용량만큼 읽기 */
			if (rc == 0)
				break;				/* [한국어] EOF */
			if (rc == -1)
				goto err_free_close_out;	/* [한국어] read 에러 */

			count += rc;				/* [한국어] 누적 바이트 */
			out += rc;				/* [한국어] 출력 포인터 전진 */
		}
	} else {						/* [한국어] 크기 계산 단계: lseek(SEEK_END) 로 파일 크기 획득 */
		count = lseek(fd, 0, SEEK_END);
		if (count == -1)
			goto err_free_close_out;
		if (count >= out_len)
			count = out_len;			/* [한국어] out_len 상한으로 클램프 */
	}

	*filled = count;					/* [한국어] 로드된 바이트 수 보고 */
	close(fd);						/* [한국어] 파일 닫기 */
	free(file);						/* [한국어] 파일명 문자열 해제 */

	/* Catch up quote */
	return end + 1;						/* [한국어] 닫는 따옴표 다음 위치 반환 */

err_free_close_out:
	close(fd);						/* [한국어] 에러 경로: 열린 fd 정리 */
err_free_out:
	free(file);						/* [한국어] 파일명 문자열 해제 */
err_out:
	return NULL;						/* [한국어] 상위에 에러 전파 */

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
/*
 * [한국어] parse_string - `"text"` 이중 따옴표 리터럴을 바이트 그대로 out 에 복사
 *
 * @beg: 첫 문자가 `"` 인 입력
 * @out/out_len/filled: 출력 버퍼와 크기, 채워진 바이트 수 [out]
 * @return: 파싱 종료 후 다음 위치 또는 NULL
 *
 * 주의: 따옴표 이스케이프(`\"`) 미지원 — 상위 주석의 NOTE 참조.
 */
static const char *parse_string(const char *beg, char *out,
				unsigned int out_len,
				unsigned int *filled)
{
	const char *end;

	if (!out_len)
		return NULL;				/* [한국어] 출력 공간 없음 */

	assert(*beg == '"');				/* [한국어] 호출자가 첫 문자 검증했어야 함 */
	beg++;						/* [한국어] 여는 " 건너뜀 */
	end = strchr(beg, '"');				/* [한국어] 닫는 " 찾기 */
	if (!end)
		return NULL;				/* [한국어] 닫히지 않은 문자열 */
	if (end - beg > out_len)
		return NULL;				/* [한국어] 출력 버퍼 부족 */

	if (out)
		memcpy(out, beg, end - beg);		/* [한국어] 두 따옴표 사이를 바이트 복사 */
	*filled = end - beg;				/* [한국어] 채워진 바이트 수 보고 */

	/* Catch up quote */
	return end + 1;					/* [한국어] 닫는 " 다음 위치 */
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
/*
 * [한국어] parse_number - 10진/16진 숫자 토큰을 리틀 엔디안 바이트열로 변환
 *
 * @beg:    입력 포인터
 * @out:    출력 버퍼 (NULL 허용 — 크기 계산 시)
 * @out_len:출력 가능 바이트 수
 * @filled: [out] 기록된 바이트 수
 * @return: 토큰 종료 후 다음 위치, 에러 시 NULL
 *
 * 알고리즘:
 *   - sscanf("0%*[xX]%*[0-9a-fA-F]%n") 로 "0x..." 길이 측정.
 *   - num == 0 이면 10진수: strncasecmp 로 뒤에 오는 "0x" 위치 찾아 10진 끝 경계 결정,
 *     strntol/strtol 로 정수 변환. INT_MIN..INT_MAX 범위 검증. 0 이면 단일 0x00 바이트.
 *     그 외에는 `for (; val; val>>=8)` 로 하위 바이트부터 out[i] 에 리틀엔디안 저장.
 *   - num > 2 이면 16진수: "0x" 건너뛴 뒤 2자리씩 sscanf("%2hhx") 로 한 바이트씩.
 *     홀수 자리는 첫 한 자리만 %1hhx 로 처리. "0xff0x14" 같은 연속 결합도 지원.
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
	sscanf(beg, "0%*[xX]%*[0-9a-fA-F]%n", &num);	/* [한국어] "0x" + hex 문자열 길이를 num 에 기록 (매칭 안 되면 num=0) */
	if (num == 0) {
		/* Here we are trying to parse decimal */

		char *_end;

		/* Looking ahead */
		_end = strcasestr(beg, "0x");		/* [한국어] 뒤이어 "0x" 가 나오는지 확인하여 10진 종료 경계 결정 */
		if (_end)
			num = _end - beg;
		if (num)
			lval = strntol(beg, num, &_end, 10);	/* [한국어] 제한 길이 10진 파싱 */
		else
			lval = strtol(beg, &_end, 10);		/* [한국어] 끝까지 10진 */
		if (beg == _end || lval > INT_MAX || lval < INT_MIN)
			return NULL;				/* [한국어] 변환 실패 또는 INT 범위 초과 */
		end = _end;
		i = 0;
		if (!lval) {					/* [한국어] 0 은 단일 0x00 바이트 */
			num    = 0;
			if (out)
				out[i] = 0x00;
			i      = 1;
		} else {					/* [한국어] 리틀엔디안 바이트열로 분해 */
			val = (unsigned int)lval;
			for (; val && out_len; out_len--, i++, val >>= 8)
				if (out)
					out[i] = val & 0xff;	/* [한국어] 하위 바이트를 순차 저장 */
			if (val)
				return NULL;			/* [한국어] out_len 소진인데 val 남음 → 공간 부족 */
		}
	} else {
		assert(num > 2);				/* [한국어] "0x" 만 있고 숫자 없음(num==2)은 sscanf 가 매칭 안 함 */

		/* Catch up 0x prefix */
		num -= 2;					/* [한국어] 접두 2자 제거 */
		beg += 2;

		/* Look back, handle this combined string: 0xff0x14 */
		if (beg[num] && !strncasecmp(&beg[num - 1], "0x", 2))
			num--;					/* [한국어] 마지막 문자가 다음 "0x" 의 '0' 이면 되돌림 (결합 숫자 처리) */

		end  = beg + num;

		for (i = 0; num && out_len;
		     out_len--, i++, num -= 2, beg += 2) {
			const char *fmt;

			fmt = (num & 1 ? "%1hhx" : "%2hhx");	/* [한국어] 홀수 잔여면 1 자리, 아니면 2 자리 */
			if (out)
				sscanf(beg, fmt, &out[i]);	/* [한국어] 바이트 한 개 파싱 */
			if (num & 1) {				/* [한국어] 홀수 처리 후 보정 */
				num++;
				beg--;
			}
		}
		if (num)
			return NULL;				/* [한국어] out_len 소진인데 남은 hex 있으면 에러 */
	}

	*filled = i;
	return end;						/* [한국어] 토큰 종료 위치 반환 */

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
/*
 * [한국어] parse_format - `%X` 포맷 지시자를 찾아 예약 공간 확보 + fmt[] 에 위치 기록
 *
 * @in:      첫 문자 `%` 인 입력
 * @out:     출력 버퍼 (예약 공간을 0 으로 채움)
 * @parsed:  지금까지 out_beg 로부터 채워진 바이트 수 (현재 포맷의 오프셋)
 * @out_len: 남은 출력 공간
 * @filled:  [out] 이번 포맷이 예약한 바이트 수 = desc->len
 * @fmt_desc: 지원되는 포맷 설명자 배열 (종료 표식 fmt_desc[i].fmt==NULL)
 * @fmt:     결과 저장용 fmt 배열 (desc 포인터 + off 오프셋 기록)
 * @fmt_sz:  fmt 배열 남은 공간
 * @return:  포맷 종료 후 다음 위치, 또는 NULL
 *
 * 예: `%o` → fmt_desc 에서 "%o" 매칭 → fmt.desc = desc, fmt.off = parsed, out 에 8바이트 0 예약.
 * 런타임에 paste_format(_inplace) 가 이 위치로 찾아가 실제 값(블록 오프셋 등) 을 기록.
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
		return NULL;					/* [한국어] 선행 조건 부족 */

	assert(*in == '%');					/* [한국어] 호출자는 첫 문자 확인 */

	for (i = 0; fmt_desc[i].fmt; i++) {			/* [한국어] 지원 포맷 테이블 순회 */
		const struct pattern_fmt_desc *desc;

		desc = &fmt_desc[i];
		len  = strlen(desc->fmt);			/* [한국어] 포맷 문자열 길이 */
		if (0 == strncmp(in, desc->fmt, len)) {		/* [한국어] 접두 일치 검사 */
			fmt->desc = desc;			/* [한국어] 매칭된 설명자 저장 */
			fmt->off  = parsed;			/* [한국어] 이 포맷이 들어갈 버퍼 내 오프셋 */
			f = fmt;
			break;
		}
	}

	if (!f)
		return NULL;					/* [한국어] 매칭 실패 */
	if (f->desc->len > out_len)
		return NULL;					/* [한국어] 예약 바이트 수 > 남은 공간 */

	if (out)
		memset(out, '\0', f->desc->len);		/* [한국어] 예약 공간을 0 으로 채움 (나중에 paste 가 덮어씀) */
	*filled = f->desc->len;

	return in + len;					/* [한국어] 포맷 문자 다음 위치 */
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
 *   out=fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe fe
 *
 * Returns number of bytes filled or err < 0 in case of failure.
 */
/*
 * [한국어] parse_and_fill_pattern - 복합 패턴 문자열을 파싱하여 out 에 바이트 채움
 *
 * @in:           입력 패턴 문자열
 * @in_len:       입력 길이 바이트
 * @out:          출력 버퍼 (NULL 허용 = 1-pass 크기 계산)
 * @out_len:      출력 버퍼 최대 크기
 * @fmt_desc:     지원 포맷 설명자 (`%o` 등)
 * @fmt:          [out] 발견된 포맷 정보 배열
 * @fmt_sz_out:   [in] 배열 최대 크기, [out] 실제 사용된 수
 * @return:       채워진 바이트 수 (>=0), 또는 -EINVAL
 *
 * 알고리즘: beg 포인터를 전진시키며 각 토큰의 첫 문자로 타입 분기:
 *   `'` → parse_file
 *   `"` → parse_string
 *   `%` → parse_format
 *   기타 → parse_number (10진/16진 자동 판별)
 * 각 파서 호출 후 in_len 에서 소비분 차감, out 포인터 전진.
 * parsed_fmt=1 경로에서는 fmt[] 슬롯 전진(fmt_rem 감소).
 *
 * 2-pass 용도: out=NULL 로 첫 호출 → count 반환 → malloc → out 로 재호출 패턴.
 */
static int parse_and_fill_pattern(const char *in, unsigned int in_len,
				  char *out, unsigned int out_len,
				  const struct pattern_fmt_desc *fmt_desc,
				  struct pattern_fmt *fmt,
				  unsigned int *fmt_sz_out)
{
	const char *beg, *end, *out_beg = out;	/* [한국어] beg=현재 파싱 위치, out_beg=출력 시작 (parsed 계산용) */
	unsigned int total = 0, fmt_rem = 0;	/* [한국어] total=누적 바이트, fmt_rem=남은 fmt[] 슬롯 */

	if (!in || !in_len || !out_len)
		return -EINVAL;
	if (fmt_sz_out)
		fmt_rem = *fmt_sz_out;		/* [한국어] fmt 배열 허용 크기 */

	beg = in;
	do {
		unsigned int filled;
		int parsed_fmt;

		filled     = 0;
		parsed_fmt = 0;

		switch (*beg) {				/* [한국어] 첫 문자로 타입 판별 */
		case '\'':
			end = parse_file(beg, out, out_len, &filled);
			break;
		case '"':
			end = parse_string(beg, out, out_len, &filled);
			break;
		case '%':
			end = parse_format(beg, out, out - out_beg, out_len,
					   &filled, fmt_desc, fmt, fmt_rem);
			parsed_fmt = 1;			/* [한국어] 포맷이 소비됨 → fmt[] 전진 플래그 */
			break;
		default:
			end = parse_number(beg, out, out_len, &filled);
			break;
		}

		if (!end)
			return -EINVAL;			/* [한국어] 어느 파서든 실패면 전체 실패 */

		if (parsed_fmt) {
			assert(fmt_rem);
			fmt_rem--;			/* [한국어] fmt 슬롯 소비 */
			fmt++;
		}

		assert(end - beg <= in_len);
		in_len -= end - beg;			/* [한국어] 소비된 입력 바이트 차감 */
		beg     = end;				/* [한국어] 다음 토큰 위치 */

		assert(filled);
		assert(filled <= out_len);
		out_len -= filled;			/* [한국어] 출력 공간 감소 */
		total   += filled;			/* [한국어] 총 바이트 누적 */
		if (out)
			out += filled;			/* [한국어] 출력 포인터 전진 (1-pass 모드에서는 여전히 out_beg=out=NULL) */

	} while (in_len);

	if (fmt_sz_out)
		*fmt_sz_out -= fmt_rem;			/* [한국어] 실제 사용된 fmt 수 = 초기 - 남은 수 */
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
 * [한국어] parse_and_fill_pattern_alloc - 2-pass 파싱(크기 계산 → malloc → 실제 파싱) 래퍼
 *
 * 호출자는 반환된 *out 버퍼를 free() 할 책임을 가진다.
 * 호출 체인: options.c (set_verify_pattern / set_buffer_pattern) → [parse_and_fill_pattern_alloc].
 */
int parse_and_fill_pattern_alloc(const char *in, unsigned int in_len,
		char **out, const struct pattern_fmt_desc *fmt_desc,
		struct pattern_fmt *fmt, unsigned int *fmt_sz_out)
{
	int count;

	/* [한국어] 1차 호출: out=NULL 로 필요한 크기만 계산 (fmt_sz_out 은 1차에서도 수정됨) */
	count = parse_and_fill_pattern(in, in_len, NULL, MAX_PATTERN_SIZE,
				       fmt_desc, fmt, fmt_sz_out);
	if (count < 0)
		return count;				/* [한국어] 파싱 에러 바로 전파 */

	/* [한국어] 2차 호출: 계산된 크기로 할당 후 실제 파싱 */
	*out = malloc(count);
	count = parse_and_fill_pattern(in, in_len, *out, count, fmt_desc,
				       fmt, fmt_sz_out);
	if (count < 0) {
		free(*out);				/* [한국어] 2차에서 실패 시 롤백 */
		*out = NULL;
	}

	return count;
}

/**
 * dup_pattern() - Duplicates part of the pattern all over the buffer.
 *
 * Returns 0 in case of success or errno < 0 in case of failure.
 */
/*
 * [한국어] dup_pattern - out 의 앞 pattern_len 바이트를 전체 out_len 으로 더블링 확장
 *
 * @out:          버퍼 (앞 pattern_len 바이트에 원본이 있음)
 * @out_len:      버퍼 전체 크기
 * @pattern_len:  반복 단위 크기
 *
 * 알고리즘: 매 반복마다 "지금까지 채워진 범위를 통째로 뒤에 복사" 하여 길이를
 * 2 배씩 늘린다. N 바이트 버퍼는 log2(N/pattern_len) 회의 memcpy 로 완성 (O(N log N)
 * 이 아니라 각 바이트가 한 번씩만 쓰이므로 총 작업량은 O(N)). memcpy 가 SIMD 로
 * 최적화되어 있어 매우 빠르다.
 *
 * 예: pattern_len=4, out_len=16
 *   초기:  AB01........ (A=앞 4바이트 원본)
 *   1회:  AB01AB01.... (off=4, len=4 복사)
 *   2회:  AB01AB01AB01AB01 (off=8, len=8 복사)
 */
static int dup_pattern(char *out, unsigned int out_len, unsigned int pattern_len)
{
	unsigned int left, len, off;

	if (out_len <= pattern_len)
		/* Normal case */
		return 0;					/* [한국어] 이미 버퍼보다 패턴이 크거나 같음 → 복사 완료 */

	off  = pattern_len;					/* [한국어] 다음 복사 지점 */
	left = (out_len - off);					/* [한국어] 아직 채울 나머지 바이트 */
	len  = min(left, off);					/* [한국어] 이번 복사량 = 현재 채워진 크기와 남은 양 중 작은 것 */

	/* Duplicate leftover
	 * [한국어] 더블링 루프 */
	while (left) {
		memcpy(out + off, out, len);			/* [한국어] 앞에서부터 len 바이트를 off 위치에 복사 */
		left -= len;					/* [한국어] 남은 바이트 감소 */
		off <<= 1;					/* [한국어] 다음 복사 지점 2 배 */
		len   = min(left, off);				/* [한국어] 다음 복사량 갱신 */
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
/*
 * [한국어] cpy_pattern - 패턴을 out 버퍼 전체에 반복 복사
 *
 * @pattern, @pattern_len: 원본 패턴
 * @out, @out_len:         대상 버퍼
 * @return: 0 = 성공, -EINVAL
 *
 * 1단계로 앞 pattern_len 바이트를 복사한 뒤 dup_pattern 으로 전체 확장.
 * 호출 체인: io_u.c fill_io_buffer → [cpy_pattern] → memcpy + dup_pattern.
 */
int cpy_pattern(const char *pattern, unsigned int pattern_len,
		char *out, unsigned int out_len)
{
	unsigned int len;

	if (!pattern || !pattern_len || !out || !out_len)
		return -EINVAL;

	/* Copy pattern */
	len = min(pattern_len, out_len);			/* [한국어] 버퍼가 패턴보다 작으면 패턴 일부만 복사 */
	memcpy(out, pattern, len);				/* [한국어] 앞부분 직접 복사 */

	/* Spread filled chunk all over the buffer */
	return dup_pattern(out, out_len, pattern_len);		/* [한국어] 더블링으로 나머지 채움 */
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
 * [한국어] cmp_pattern - 버퍼가 패턴 반복으로 구성되어 있는지 검증
 *
 * @pattern, @pattern_size: 원본 패턴
 * @off:                     패턴의 시작 오프셋 (버퍼가 패턴 중간부터 시작할 때)
 * @buf, @len:               검증할 버퍼
 * @return: 0 = 일치, -EILSEQ = 불일치
 *
 * 아이디어: 버퍼가 패턴 반복이라면
 *   1) buf[0..len-pattern_size] 와 buf[pattern_size..] 가 동일해야 함 (자기 복제).
 *   2) buf[0..pattern_size-off] 가 pattern[off..] 와 동일해야 함 (시작 정합).
 *   3) 남은 뒷부분이 pattern 앞부분과 동일해야 함.
 * 세 개의 memcmp 로 끝내므로 바이트 루프가 필요 없다. memcmp 자체는 SIMD 최적화됨.
 *
 * 호출 체인: verify.c → [cmp_pattern].
 */
int cmp_pattern(const char *pattern, unsigned int pattern_size,
		unsigned int off, const char *buf, unsigned int len)
{
	int rc;
	unsigned int size;

	/* Find the difference in buffer
	 * [한국어] 버퍼 내부의 반복 일관성 검사: buf[0..] vs buf[pattern_size..] */
	if (len > pattern_size) {
		rc = memcmp(buf, buf + pattern_size, len - pattern_size);
		if (rc)
			return -EILSEQ;				/* [한국어] 반복 깨짐 → 데이터 손상 */
	}
	/* Compare second part of the pattern with buffer */
	if (off) {						/* [한국어] 버퍼가 패턴 중간부터 시작 */
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
		size = min(len, (off ? off : pattern_size));	/* [한국어] off 가 있으면 남은 길이가 off 바이트, 없으면 pattern_size 전체 */
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
/*
 * [한국어] paste_format_inplace - 기존 pattern 버퍼 내부의 fmt->off 위치들에 런타임 값 삽입
 *
 * @pattern, @pattern_len: 대상 버퍼 (in-place 수정)
 * @fmt, @fmt_sz:          포맷 위치 배열
 * @priv:                  paste 콜백에 전달될 사용자 컨텍스트 (예: 블록 오프셋)
 * @return:                0 = 성공, 콜백 반환값(음수 = 에러)
 *
 * fmt[i].desc->paste(pattern + fmt[i].off, len, priv) 를 호출하여 예약된
 * 공간(주로 8바이트) 에 실제 값을 채운다. paste 는 fmt_desc 별로 다름
 * (예: %o 는 오프셋 write_uint64_le).
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
			break;					/* [한국어] 오프셋이 pattern 크기를 넘음 → 여기서 중단 */
		len = min(pattern_len - f->off, f->desc->len);	/* [한국어] 예약 공간 전체 또는 잔여 */
		rc  = f->desc->paste(pattern + f->off, len, priv);	/* [한국어] 포맷별 paste 콜백 호출 */
		if (rc)
			return rc;				/* [한국어] 콜백 에러 전파 */
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
/*
 * [한국어] paste_format - 패턴을 out 에 복사 → 포맷 값 삽입 → 버퍼 전체로 확장
 *
 * @pattern, @pattern_len: 원본 패턴
 * @fmt, @fmt_sz:          포맷 위치
 * @out, @out_len:         대상 버퍼
 * @priv:                  paste 콜백 컨텍스트
 *
 * 첫 패턴 청크에 값까지 삽입한 뒤, dup_pattern 으로 전체 버퍼에 반복 복사한다.
 * 이로 인해 버퍼 전체에 "값이 삽입된 pattern_len 블록" 이 반복된다.
 * 호출 체인: io_u.c → [paste_format] → memcpy + paste_format_inplace + dup_pattern.
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
	memcpy(out, pattern, len);				/* [한국어] 첫 블록을 out 에 복사 */

	rc = paste_format_inplace(out, len, fmt, fmt_sz, priv);	/* [한국어] 포맷 자리에 실제 값 삽입 */
	if (rc)
		return rc;

	/* Spread filled chunk all over the buffer */
	return dup_pattern(out, out_len, pattern_len);		/* [한국어] 더블링으로 버퍼 전체 복제 */
}
