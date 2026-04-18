/*
 * [한국어 설명] verify/buffer 패턴 파서 및 버퍼 조작 공개 API 헤더 (pattern.h)
 *
 * === 파일의 역할 ===
 * lib/pattern.c 가 구현하는 "사용자 지정 바이트 패턴" 파서/채움/비교 엔진의
 * 공개 API 를 노출한다. fio 옵션 `--verify_pattern=`, `--buffer_pattern=` 은
 * 16진수, 10진수, 큰/작은 따옴표 문자열, 파일 경로, 포맷 지시자(%o 등)의
 * 조합을 받아 버퍼를 특정 바이트 패턴으로 채우거나 검증한다. 본 헤더는
 * (1) MAX_PATTERN_SIZE 한계 매크로 (2) 포맷 지시자 설명 테이블 타입
 * `struct pattern_fmt_desc` 와 파싱 결과 `struct pattern_fmt` (3) 5 개
 * 공개 함수 — parse_and_fill_pattern_alloc, paste_format, paste_format_inplace,
 * cpy_pattern, cmp_pattern — 를 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "쓰기 버퍼 준비" 와 "검증" 두 경로에 걸친 공용 유틸. 옵션 파싱은
 * 메인(파서) 스레드, 실제 cpy/cmp/paste 는 잡 스레드에서 수행.
 * 호출 체인(쓰기):
 *   options.c str_buffer_pattern_cb → parse_and_fill_pattern_alloc (메인)
 *     → io_u.c fill_io_buffer → cpy_pattern (잡) → paste_format (%o 등 삽입)
 * 호출 체인(검증):
 *   verify.c verify_io_u_pattern → cmp_pattern (잡/검증 스레드).
 *
 * === 타 모듈과의 연결 ===
 * - pattern.c : 구현(2-pass 파싱, 더블링 memcpy, 2 회 memcmp 검증).
 * - lib/strntol.h : 10 진수 구간 파싱에 사용.
 * - options.c : str_verify_pattern_cb / str_buffer_pattern_cb 이 허브.
 * - io_u.c : 쓰기 버퍼 채움.
 * - verify.c : 읽기 후 버퍼 검증.
 * - FIO_SERVER_MAX_CMD_MB : MAX_PATTERN_SIZE 산정의 근거(네트워크 전송 시
 *   2 개 패턴이 들어갈 여유를 두고 128MiB 로 제한).
 * 데이터 흐름: 사용자 문자열 → parse_and_fill_pattern_alloc → 할당된 out
 * 버퍼 + pattern_fmt 배열 → io_u 생성 시 cpy_pattern 으로 전체 버퍼에 반복
 * 복사 → paste_format 으로 %o 등 동적 값 삽입 → I/O 엔진에 전달.
 *
 * === 주요 함수/구조체 요약 ===
 * - MAX_PATTERN_SIZE(128MiB) : 동적 할당 패턴의 최대 크기.
 * - struct pattern_fmt_desc : (fmt, len, paste 콜백) 포맷 설명 테이블 엔트리.
 * - struct pattern_fmt : (off, desc) 파싱된 포맷의 버퍼 내 위치.
 * - parse_and_fill_pattern_alloc : 입력 문자열을 파싱해 out 버퍼를 malloc
 *   하고 fmt 배열을 채움.
 * - paste_format / paste_format_inplace : 런타임 동적 값(오프셋 등) 삽입.
 * - cpy_pattern : 짧은 패턴을 긴 버퍼에 O(log n) 더블링 memcpy 로 복사.
 * - cmp_pattern : 같은 패턴의 반복 구조를 활용해 2 회 memcmp 로 검증.
 */

#ifndef FIO_PARSE_PATTERN_H
#define FIO_PARSE_PATTERN_H
/* [한국어] 헤더 가드. options.c / verify.c / io_u.c / pattern.c 모두가 포함. */

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
/* [한국어] 패턴 최대 크기 128 MiB. 결정 이유:
 * (1) 서버/클라이언트 프로토콜 명령 한도 FIO_SERVER_MAX_CMD_MB 내에 "두 개의
 *     패턴(--buffer_pattern + --verify_pattern)" 이 함께 들어가도록.
 * (2) 수신 측에서 네트워크로부터 패턴을 검증 가능한 상한.
 * (3) 실전에서 I/O 크기가 이 한도를 초과하는 경우가 거의 없어 자동 잘림이
 *     일어나더라도 의미 손실이 적음.
 * 설정자: 컴파일 타임 상수. 읽는 자: parse_and_fill_pattern_alloc 가 할당
 * 요청 크기 검증. 값 범위: 불변(128 MiB). */

/**
 * Pattern format description. The input for 'parse_pattern'.
 * Describes format with its name and callback, which should
 * be called to paste something inside the buffer.
 */
struct pattern_fmt_desc {
	const char  *fmt;
	/* [한국어] 포맷 문자열. 예: "%o" = 블록 오프셋, "%b" = 블록 번호.
	 * 파서가 입력에서 이 문자열을 탐지하면 해당 위치를 포맷 지시자로 등록.
	 * 설정자: options.c 의 정적 배열이 컴파일 타임에 등록.
	 * 읽는 자: pattern.c parse_format 이 비교.
	 * 값 범위: NUL 종결 문자열(보통 2~4 문자). 정적 생존. */

	unsigned int len;
	/* [한국어] 이 포맷이 버퍼에서 차지하는 바이트 수(예: "%o" 가 8 바이트
	 * 오프셋 값으로 치환되면 len=8). 파싱 단계에서 이 공간을 out 버퍼에
	 * 예약해 두고 paste 콜백이 런타임에 실제 값을 기록.
	 * 설정자: 정적 배열. 읽는 자: parse/paste 단계. */

	int (*paste)(char *buf, unsigned int len, void *priv);
	/* [한국어] 런타임 동적 값을 버퍼에 기록하는 콜백.
	 * @buf: 기록 대상 위치(out 버퍼의 해당 off 바이트).
	 * @len: 기록할 바이트 수(= 위 len 필드).
	 * @priv: io_u 또는 블록 메타데이터 포인터. verify.c 의 paste_blockoff 가
	 *   priv 에서 오프셋을 꺼내 LE 8 바이트로 기록.
	 * @return: 0 성공, 비영 실패.
	 * 설정자: 정적 배열. 읽는 자: paste_format / paste_format_inplace. */
};

/**
 * Pattern format. The output of 'parse_pattern'.
 * Describes the exact position inside the xbuffer.
 */
struct pattern_fmt {
	unsigned int off;
	/* [한국어] 이 포맷 지시자가 out 버퍼에서 시작하는 바이트 오프셋.
	 * 설정자: parse_and_fill_pattern_alloc 가 입력 문자열을 순회하며 설정.
	 * 읽는 자: paste_format 이 "버퍼 + off" 위치에 값 삽입.
	 * 값 범위: 0 ≤ off < 패턴 바이트 길이. */

	const struct pattern_fmt_desc *desc;
	/* [한국어] 이 위치에 대응하는 포맷 설명자 포인터(정적 테이블의 엔트리).
	 * paste 콜백과 len 을 얻는 경로.
	 * 설정자: 파서. 읽는 자: paste_format. */
};

int parse_and_fill_pattern_alloc(const char *in, unsigned int in_len,
		char **out, const struct pattern_fmt_desc *fmt_desc,
		struct pattern_fmt *fmt, unsigned int *fmt_sz_out);
/* [한국어]
 * parse_and_fill_pattern_alloc - 입력 문자열을 파싱해 out 버퍼를 malloc 하고
 *   fmt 배열(호출자 제공)을 채운다.
 *
 * @in, @in_len : 사용자 입력 문자열.
 * @out : (출력) malloc 으로 새로 할당된 바이트 패턴. 호출자가 free().
 * @fmt_desc : 지원 포맷 지시자 테이블(NULL-terminated 배열).
 * @fmt : (출력) 파싱된 포맷 위치 배열(호출자가 미리 할당). 크기는 최대
 *   예상 지시자 수.
 * @fmt_sz_out : (입출력) 입력 시 fmt 의 capacity, 출력 시 실제 저장된 개수.
 * @return: 성공 시 할당된 바이트 수, 실패 시 음수.
 *
 * 실행 컨텍스트: fio 메인(옵션 파서) 스레드. 1 잡당 한 번 호출. */

int paste_format_inplace(char *pattern, unsigned int pattern_len,
			 struct pattern_fmt *fmt, unsigned int fmt_sz,
			 void *priv);
/* [한국어] paste_format_inplace - 주어진 pattern 바이트 배열의 fmt 위치에
 * 직접 paste 콜백 결과를 덮어씀. I/O 버퍼를 쓰기 직전 %o 같은 런타임 값을
 * 버퍼에 주입할 때 사용. 호출 컨텍스트: 잡 스레드. */

int paste_format(const char *pattern, unsigned int pattern_len,
		 struct pattern_fmt *fmt, unsigned int fmt_sz,
		 char *out, unsigned int out_len, void *priv);
/* [한국어] paste_format - pattern 을 out 에 memcpy 하면서 fmt 위치에
 * paste 콜백 결과를 기록. out_len 이 패턴보다 길면 나머지는 cpy_pattern 이
 * 별도로 채움 관행. */

int cpy_pattern(const char *pattern, unsigned int pattern_len,
		char *out, unsigned int out_len);
/* [한국어] cpy_pattern - 짧은 pattern 을 긴 out 버퍼에 반복 복사.
 * 구현: 더블링 기법 — out 에 pattern 을 1 번 복사한 뒤 2 배씩 키워가며
 * memcpy. O(log(out_len/pattern_len)) 회의 memcpy 만 수행. */

int cmp_pattern(const char *pattern, unsigned int pattern_size,
		unsigned int off, const char *buf, unsigned int len);
/* [한국어] cmp_pattern - 읽은 버퍼 buf(len 바이트) 가 pattern(pattern_size) 의
 * 반복 구조와 일치하는지 검증. off 는 buf 가 "패턴 주기 내 어디에서 시작
 * 하는지" 를 지정(I/O 오프셋 mod pattern_size).
 * 구현: 두 번의 memcmp 로 완결 — (1) off 에서 pattern 끝까지, (2) buf 의
 * 나머지를 pattern 의 처음부터 비교하면 전체 반복 블록이 일관함을 증명.
 * 반환: 0=일치, 비영=불일치 위치. 호출 컨텍스트: verify.c (잡/검증 스레드). */

#endif
/* [한국어] FIO_PARSE_PATTERN_H 가드 종료. */
