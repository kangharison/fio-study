/*
 * IO verification helpers
 */
/*
 * [한국어 설명] I/O 데이터 무결성 검증 엔진 (verify.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio가 쓴 데이터를 다시 읽어서 무결성을 확인하는 검증 기능을 구현한다.
 * 쓰기 시 데이터 블록에 패턴 + 체크섬 헤더를 채우고, 읽기 시 체크섬을 재계산하여
 * 데이터 손상 여부를 판별한다. CRC, MD5, SHA, xxHash 등 다양한 알고리즘을 지원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 쓰기 경로: backend.c의 do_io() → populate_verify_io_u() [이 파일]
 * 검증 경로: backend.c의 do_verify() → get_next_verify() → verify_io_u() [이 파일]
 * 비동기 검증: verify_async_thread()가 별도 스레드에서 검증 수행
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: do_io()에서 쓰기 시 populate_verify_io_u() 호출, do_verify()에서 검증
 * - io_u.c: io_u 버퍼에 검증 데이터를 채우고 검증 결과를 보고
 * - verify.h: 검증 유형 열거형, verify_header 구조체, API 선언
 * - crc/*.c: 각 체크섬/해시 알고리즘 구현 (md5, crc32, sha256 등)
 * - 핵심 자료구조: verify_header(블록 헤더), io_u(I/O 요청)
 *
 * === 주요 함수/구조체 요약 ===
 * - populate_verify_io_u(): 쓰기 전 io_u 버퍼에 패턴 + 체크섬 헤더 채움
 * - verify_io_u(): 읽어온 데이터의 체크섬을 재계산하여 무결성 확인
 * - verify_async_thread(): 비동기 검증 워커 스레드의 메인 루프
 * - get_next_verify(): io_hist에서 다음 검증할 I/O 조각 꺼냄
 * - verify_save/load_state(): 검증 상태를 파일로 저장/복원 (재시작 지원)
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <unistd.h>      /* POSIX API (read, write, close 등) */
#include <fcntl.h>       /* 파일 제어 (open 플래그: O_CREAT, O_RDONLY 등) */
#include <string.h>      /* 문자열/메모리 함수 (memcpy, memcmp, strdup 등) */
#include <assert.h>      /* assert 매크로 */
#include <pthread.h>     /* POSIX 스레드 (비동기 검증 스레드용) */
#include <libgen.h>      /* basename() - 파일 경로에서 파일명 추출 */

/* fio 내부 헤더 파일들 */
#include "arch/arch.h"          /* 아키텍처별 정의 (바이트 오더 등) */
#include "fio.h"                /* fio 핵심 구조체 및 매크로 */
#include "verify.h"             /* 검증 관련 구조체/열거형/API 선언 */
#include "trim.h"               /* TRIM 관련 유틸리티 */
#include "lib/rand.h"           /* 난수 생성기 (__rand 등) */
#include "lib/hweight.h"        /* 해밍 가중치 (비트 에러 카운트에 사용) */
#include "lib/pattern.h"        /* 패턴 채우기/비교 유틸리티 */
#include "oslib/asprintf.h"     /* OS 독립적 asprintf */

/* 체크섬/해시 알고리즘 구현 헤더들 */
#include "crc/md5.h"            /* MD5 해시 */
#include "crc/crc64.h"          /* CRC64 체크섬 */
#include "crc/crc32.h"          /* CRC32 체크섬 */
#include "crc/crc32c.h"         /* CRC32C (Castagnoli) 체크섬 */
#include "crc/crc16.h"          /* CRC16 체크섬 */
#include "crc/crc7.h"           /* CRC7 체크섬 */
#include "crc/sha256.h"         /* SHA-256 해시 */
#include "crc/sha512.h"         /* SHA-512 해시 */
#include "crc/sha1.h"           /* SHA-1 해시 */
#include "crc/xxhash.h"         /* xxHash (빠른 비암호화 해시) */
#include "crc/sha3.h"           /* SHA-3 해시 패밀리 */

/* [한국어] 전방 선언 - 파일 내에서 상호 참조되는 함수들 */
static void populate_hdr(struct thread_data *td, struct io_u *io_u,
			 struct verify_header *hdr, unsigned int header_num,
			 unsigned int header_len);  /* [한국어] 검증 헤더를 채우고 체크섬을 계산하는 함수 */
static void __fill_hdr(struct thread_data *td, struct io_u *io_u,
		       struct verify_header *hdr, unsigned int header_num,
		       unsigned int header_len, uint64_t rand_seed);  /* [한국어] 헤더 필드를 직접 채우는 내부 함수 */

/* [한국어] 버퍼에 사용자 지정 패턴을 채움 (buffer_pattern 옵션에 의해 설정된 패턴) */
void fill_buffer_pattern(struct thread_data *td, void *p, unsigned int len)
{
	(void)cpy_pattern(td->o.buffer_pattern, td->o.buffer_pattern_bytes, p, len);
}

/* [한국어] 시드 기반으로 버퍼를 랜덤 데이터로 채움 (compress_percentage 옵션에 따라 압축 가능한 비율 조절) */
static void __fill_buffer(struct thread_options *o, uint64_t seed, void *p,
			  unsigned int len)
{
	__fill_random_buf_percentage(seed, p, o->compress_percentage, len, len, o->buffer_pattern, o->buffer_pattern_bytes);
}

/*
 * [한국어] 검증 패턴으로 버퍼를 채우는 핵심 함수
 *
 * verify_pattern_bytes가 0이면 랜덤 데이터로 채우고 (시드 기반 재현 가능),
 * 0이 아니면 사용자 지정 패턴으로 채운다.
 * verify_pattern_interval이 설정되면 해당 간격마다 패턴을 반복한다.
 */
void fill_verify_pattern(struct thread_data *td, void *p, unsigned int len,
			 struct io_u *io_u, uint64_t seed, int use_seed)
{
	struct thread_options *o = &td->o;
	unsigned int interval = o->verify_pattern_interval;
	unsigned long long offset = io_u->offset;

	/* [한국어] 사용자 지정 패턴이 없으면 랜덤 데이터로 채움 */
	if (!o->verify_pattern_bytes) {
		dprint(FD_VERIFY, "fill random bytes len=%u\n", len);

		if (!use_seed) {
			seed = __rand(&td->verify_state);
			if (sizeof(int) != sizeof(long *))
				seed *= (unsigned long)__rand(&td->verify_state);
		}
		io_u->rand_seed = seed;
		__fill_buffer(o, seed, p, len);
		return;
	}

	/* Skip if we were here and we do not need to patch pattern with
	 * format. However, we cannot skip if verify_offset is set because we
	 * have swapped the header with pattern bytes */
	/* [한국어] 이미 패턴이 채워져 있고 포맷 패치가 불필요하면 건너뜀.
	 * 단, verify_offset이 설정되면 헤더와 패턴 바이트가 교환되므로 건너뛸 수 없음 */
	if (!td->o.verify_fmt_sz && io_u->buf_filled_len >= len && !td->o.verify_offset) {
		dprint(FD_VERIFY, "using already filled verify pattern b=%d len=%u\n",
			o->verify_pattern_bytes, len);
		return;
	}

	if (!interval)
		interval = len;

	/* [한국어] 패턴을 interval 간격으로 반복하면서 버퍼를 채움.
	 * paste_format()으로 포맷 문자열(예: 블록 오프셋)도 함께 삽입 */
	io_u->offset += (p - io_u->buf) - (p - io_u->buf) % interval;
	for (unsigned int bytes_done = 0, bytes_todo = 0; bytes_done < len;
			bytes_done += bytes_todo, p += bytes_todo, io_u->offset += interval) {
		bytes_todo = (p - io_u->buf) % interval;
		if (!bytes_todo)
			bytes_todo = interval;
		bytes_todo = min(bytes_todo, len - bytes_done);

		(void)paste_format(td->o.verify_pattern, td->o.verify_pattern_bytes,
				   td->o.verify_fmt, td->o.verify_fmt_sz,
				   p, bytes_todo, io_u);
	}

	io_u->buf_filled_len = len;
	io_u->offset = offset;
}

/*
 * [한국어] 검증 헤더 간격(increment)을 계산
 *
 * 하나의 io_u 버퍼에 여러 개의 검증 헤더가 들어갈 수 있다.
 * verify_interval이 설정되면 해당 간격마다 헤더를 배치하고,
 * 설정되지 않으면 전체 buflen을 하나의 블록으로 취급한다.
 */
static unsigned int get_hdr_inc(struct thread_data *td, struct io_u *io_u)
{
	unsigned int hdr_inc;

	/*
	 * If we use bs_unaligned, buflen can be larger than the verify
	 * interval (which just defaults to the smallest blocksize possible).
	 */
	/* [한국어] bs_unaligned 사용 시 buflen이 verify_interval보다 클 수 있으므로
	 * 이 경우 buflen 전체를 하나의 블록으로 사용 */
	hdr_inc = io_u->buflen;
	if (td->o.verify_interval && td->o.verify_interval <= io_u->buflen &&
	    !td->o.bs_unaligned)
		hdr_inc = td->o.verify_interval;

	return hdr_inc;
}

/*
 * [한국어] 패턴 데이터를 채운 후 각 블록마다 검증 헤더를 삽입
 *
 * 1단계: fill_verify_pattern()으로 전체 버퍼를 패턴/랜덤 데이터로 채움
 * 2단계: hdr_inc 간격으로 버퍼를 순회하면서 각 블록 시작에 populate_hdr()로 헤더 삽입
 */
static void fill_pattern_headers(struct thread_data *td, struct io_u *io_u,
				 uint64_t seed, int use_seed)
{
	unsigned int hdr_inc, header_num;
	struct verify_header *hdr;
	void *p = io_u->buf;

	/* [한국어] 먼저 전체 버퍼를 패턴/랜덤 데이터로 채움 */
	fill_verify_pattern(td, p, io_u->buflen, io_u, seed, use_seed);

	/* [한국어] 각 검증 블록마다 헤더(매직넘버, 체크섬 등)를 삽입 */
	hdr_inc = get_hdr_inc(td, io_u);
	header_num = 0;
	for (; p < io_u->buf + io_u->buflen; p += hdr_inc) {
		hdr = p;
		populate_hdr(td, io_u, hdr, header_num, hdr_inc);
		header_num++;
	}
}

/* [한국어] 두 메모리 영역의 내용을 교환 (verify_offset 처리에 사용) */
static void memswp(void *buf1, void *buf2, unsigned int len)
{
	char swap[200];

	assert(len <= sizeof(swap));

	memcpy(&swap, buf1, len);
	memcpy(buf1, buf2, len);
	memcpy(buf2, &swap, len);
}

/* [한국어] 메모리 영역을 16진수로 덤프 (검증 실패 시 CRC 값 출력에 사용) */
static void hexdump(void *buffer, int len)
{
	unsigned char *p = buffer;
	int i;

	for (i = 0; i < len; i++)
		log_err("%02x", p[i]);
	log_err("\n");
}

/*
 * Prepare for separation of verify_header and checksum header
 */
/*
 * [한국어] 검증 유형에 따른 전체 헤더 크기를 반환
 *
 * 반환값 = sizeof(verify_header) + sizeof(체크섬별 헤더)
 * 예: MD5의 경우 verify_header(54바이트) + vhdr_md5(16바이트)
 * VERIFY_PATTERN_NO_HDR의 경우 헤더 없이 0을 반환
 */
static inline unsigned int __hdr_size(int verify_type)
{
	unsigned int len = 0;

	switch (verify_type) {
	case VERIFY_NONE:
	case VERIFY_HDR_ONLY:
	case VERIFY_NULL:
	case VERIFY_PATTERN:
		len = 0;
		break;
	case VERIFY_MD5:
		len = sizeof(struct vhdr_md5);
		break;
	case VERIFY_CRC64:
		len = sizeof(struct vhdr_crc64);
		break;
	case VERIFY_CRC32C:
	case VERIFY_CRC32:
	case VERIFY_CRC32C_INTEL:
		len = sizeof(struct vhdr_crc32);
		break;
	case VERIFY_CRC16:
		len = sizeof(struct vhdr_crc16);
		break;
	case VERIFY_CRC7:
		len = sizeof(struct vhdr_crc7);
		break;
	case VERIFY_SHA256:
		len = sizeof(struct vhdr_sha256);
		break;
	case VERIFY_SHA512:
		len = sizeof(struct vhdr_sha512);
		break;
	case VERIFY_SHA3_224:
		len = sizeof(struct vhdr_sha3_224);
		break;
	case VERIFY_SHA3_256:
		len = sizeof(struct vhdr_sha3_256);
		break;
	case VERIFY_SHA3_384:
		len = sizeof(struct vhdr_sha3_384);
		break;
	case VERIFY_SHA3_512:
		len = sizeof(struct vhdr_sha3_512);
		break;
	case VERIFY_XXHASH:
		len = sizeof(struct vhdr_xxhash);
		break;
	case VERIFY_SHA1:
		len = sizeof(struct vhdr_sha1);
		break;
	case VERIFY_PATTERN_NO_HDR:
		return 0;
	default:
		log_err("fio: unknown verify header!\n");
		assert(0);
	}

	return len + sizeof(struct verify_header);
}

/* [한국어] 헤더 크기를 반환 (VERIFY_PATTERN_NO_HDR이면 0) */
static inline unsigned int hdr_size(struct thread_data *td,
				    struct verify_header *hdr)
{
	if (td->o.verify == VERIFY_PATTERN_NO_HDR)
		return 0;

	return __hdr_size(hdr->verify_type);
}

/* [한국어] verify_header 바로 뒤에 위치한 체크섬별 헤더(vhdr_*) 포인터를 반환 */
static void *hdr_priv(struct verify_header *hdr)
{
	void *priv = hdr;

	return priv + sizeof(struct verify_header);
}

/*
 * Verify container, pass info to verify handlers and allow them to
 * pass info back in case of error
 */
/*
 * [한국어] 검증 컨테이너 구조체 - 검증 핸들러 함수들에 입력/출력 정보를 전달
 *
 * 입력: 검증할 io_u, 헤더 번호, 스레드 데이터
 * 출력 (에러 시): 알고리즘 이름, 기대한 CRC, 실제 CRC, CRC 길이
 */
struct vcont {
	/*
	 * Input
	 */
	struct io_u *io_u;       /* [한국어] 검증할 I/O 유닛 */
	unsigned int hdr_num;    /* [한국어] 현재 블록의 헤더 번호 (0부터 시작) */
	struct thread_data *td;  /* [한국어] 스레드 데이터 */

	/*
	 * Output, only valid in case of error
	 */
	const char *name;        /* [한국어] 검증 알고리즘 이름 (에러 로그용) */
	void *good_crc;          /* [한국어] 기대한 체크섬 값 (헤더에 저장된 값) */
	void *bad_crc;           /* [한국어] 실제 계산된 체크섬 값 (불일치 시) */
	unsigned int crc_len;    /* [한국어] 체크섬 길이 (바이트) */
};

#define DUMP_BUF_SZ	255  /* [한국어] 덤프 버퍼 크기 상수 */

/*
 * [한국어] 검증 실패 시 데이터를 파일로 덤프
 *
 * 검증에 실패한 데이터 블록을 "<파일명>.<오프셋>.<타입>" 형식의 파일로 저장한다.
 * type은 "received"(실제 읽은 데이터) 또는 "expected"(기대한 데이터)
 */
static void dump_buf(char *buf, unsigned int len, unsigned long long offset,
		     const char *type, struct fio_file *f)
{
	char *ptr, *fname;
	char sep[2] = { FIO_OS_PATH_SEPARATOR, 0 };
	int ret, fd;

	ptr = strdup(f->file_name);

	if (asprintf(&fname, "%s%s%s.%llu.%s", aux_path ? : "",
		     aux_path ? sep : "", basename(ptr), offset, type) < 0) {
		if (!fio_did_warn(FIO_WARN_VERIFY_BUF))
			log_err("fio: not enough memory for dump buffer filename\n");
		goto free_ptr;
	}

	fd = open(fname, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) {
		perror("open verify buf file");
		goto free_fname;
	}

	while (len) {
		ret = write(fd, buf, len);
		if (!ret)
			break;
		else if (ret < 0) {
			perror("write verify buf file");
			break;
		}
		len -= ret;
		buf += ret;
	}

	close(fd);
	log_err("       %s data dumped as %s\n", type, fname);

free_fname:
	free(fname);

free_ptr:
	free(ptr);
}

/*
 * Dump the contents of the read block and re-generate the correct data
 * and dump that too.
 */
/*
 * [한국어] 검증 실패한 블록의 실제 데이터와 기대 데이터를 모두 파일로 덤프
 *
 * 1) 디스크에서 읽어온 데이터를 "received" 파일로 저장
 * 2) 원본 시드로 데이터를 재생성하여 "expected" 파일로 저장
 * verify_dump 옵션이 켜져 있을 때만 동작
 */
static void __dump_verify_buffers(struct verify_header *hdr, struct vcont *vc)
{
	struct thread_data *td = vc->td;
	struct io_u *io_u = vc->io_u;
	unsigned long hdr_offset;
	struct io_u dummy;
	void *buf;

	if (!td->o.verify_dump)
		return;

	/*
	 * Dump the contents we just read off disk
	 */
	/* [한국어] 디스크에서 읽어온 실제 데이터를 파일로 덤프 */
	hdr_offset = vc->hdr_num * hdr->len;

	dump_buf(io_u->buf + hdr_offset, hdr->len, io_u->verify_offset + hdr_offset,
			"received", vc->io_u->file);

	/*
	 * Allocate a new buf and re-generate the original data
	 */
	/* [한국어] 새 버퍼를 할당하고 원본 시드로 기대 데이터를 재생성 */
	buf = malloc(io_u->buflen);
	dummy = *io_u;
	dummy.buf = buf;
	dummy.rand_seed = hdr->rand_seed;
	dummy.buf_filled_len = 0;
	dummy.buflen = io_u->buflen;

	fill_pattern_headers(td, &dummy, hdr->rand_seed, 1);

	dump_buf(buf + hdr_offset, hdr->len, io_u->verify_offset + hdr_offset,
			"expected", vc->io_u->file);
	free(buf);
}

/* [한국어] 검증 버퍼 덤프 래퍼 - PATTERN_NO_HDR인 경우 임시 헤더를 생성하여 덤프 */
static void dump_verify_buffers(struct verify_header *hdr, struct vcont *vc)
{
	struct thread_data *td = vc->td;
	struct verify_header shdr;

	if (td->o.verify == VERIFY_PATTERN_NO_HDR) {
		__fill_hdr(td, vc->io_u, &shdr, 0, vc->io_u->buflen, 0);
		hdr = &shdr;
	}

	__dump_verify_buffers(hdr, vc);
}

/* [한국어] 검증 실패 정보를 로그에 출력 (파일명, 오프셋, 길이, 기대/실제 CRC 등) */
static void log_verify_failure(struct verify_header *hdr, struct vcont *vc)
{
	unsigned long long offset;
	uint32_t len;
	struct thread_data *td = vc->td;

	offset = vc->io_u->verify_offset;
	if (td->o.verify != VERIFY_PATTERN_NO_HDR) {
		len = hdr->len;
		offset += (unsigned long long) vc->hdr_num * len;
	} else {
		len = vc->io_u->buflen;
	}

	log_err("%.8s: verify failed at file %s offset %llu, length %u"
			" (requested block: offset=%llu, length=%llu, flags=%x)\n",
			vc->name, vc->io_u->file->file_name, offset, len,
			vc->io_u->verify_offset, vc->io_u->buflen, vc->io_u->flags);

	if (vc->good_crc && vc->bad_crc) {
		log_err("       Expected CRC: ");
		hexdump(vc->good_crc, vc->crc_len);
		log_err("       Received CRC: ");
		hexdump(vc->bad_crc, vc->crc_len);
	}

	dump_verify_buffers(hdr, vc);
}

/*
 * Return data area 'header_num'
 */
/* [한국어] 지정된 블록 번호의 데이터 영역 시작 포인터를 반환 (헤더를 건너뜀) */
static inline void *io_u_verify_off(struct verify_header *hdr, struct vcont *vc)
{
	return vc->io_u->buf + vc->hdr_num * hdr->len + hdr_size(vc->td, hdr);
}

/*
 * [한국어] 패턴 일치 여부를 검사
 *
 * 먼저 cmp_pattern()으로 빠른 비교를 시도하고,
 * 실패 시 바이트 단위로 비교하여 불일치 위치와 비트 에러 수를 보고
 */
static int check_pattern(char *buf, unsigned int len, unsigned int mod,
		unsigned int pattern_size, char *pattern, unsigned int header_size)
{
	unsigned int i;
	int rc;

	rc = cmp_pattern(pattern, pattern_size, mod, buf, len);
	if (!rc)
		goto done;

	/* Slow path, compare each byte */
	/* [한국어] 느린 경로: 바이트별 비교로 정확한 에러 위치와 비트 에러 수 파악 */
	for (i = 0; i < len; i++) {
		if (buf[i] != pattern[mod]) {
			unsigned int bits;

			bits = hweight8(buf[i] ^ pattern[mod]);
			log_err("fio: got pattern '%02x', wanted '%02x'. Bad bits %d\n",
				(unsigned char)buf[i],
				(unsigned char)pattern[mod],
				bits);
			log_err("fio: bad pattern block offset %u\n",
				i + header_size);
			rc = EILSEQ;
			goto done;
		}
		mod++;
		if (mod == pattern_size)
			mod = 0;
	}

done:
	return rc;
}

/*
 *  The current thread will need its own buffer if there are multiple threads
 *  and the pattern contains the offset. Fio currently only has one pattern
 *  format specifier so we only need to check that one, but this may need to be
 *  changed if fio ever gains more pattern format specifiers.
 */
/*
 * [한국어] 패턴에 오프셋이 포함되어 있고 멀티스레드/비동기 검증인 경우
 * 각 스레드가 자체 패턴 버퍼를 사용해야 하는지 판별 (스레드 안전성)
 */
static inline bool pattern_need_buffer(struct thread_data *td)
{
	return (td->o.verify_async || td->o.use_thread) &&
		td->o.verify_fmt_sz &&
		td->o.verify_fmt[0].desc->paste == paste_blockoff;
}

/*
 * [한국어] 패턴 기반 검증 함수 - 읽어온 데이터가 쓰기 시 채운 패턴과 일치하는지 확인
 *
 * 3가지 비교 모드:
 *   1) verify_interval 미설정 + verify_pattern_interval 미설정: 전체 버퍼를 한 번에 비교
 *   2) verify_interval 설정 + verify_pattern_interval 미설정: verify_interval 단위로 비교
 *   3) verify_pattern_interval 설정: 해당 간격의 세그먼트 단위로 비교
 */
static int verify_io_u_pattern(struct verify_header *hdr, struct vcont *vc)
{
	struct thread_data *td = vc->td;
	struct io_u *io_u = vc->io_u;
	char *buf, *pattern;
	unsigned int header_size = __hdr_size(td->o.verify);
	unsigned int len, mod, pattern_size, pattern_interval_mod, bytes_done = 0, bytes_todo;
	int rc;
	unsigned long long offset = io_u->offset;

	pattern = td->o.verify_pattern;
	pattern_size = td->o.verify_pattern_bytes;
	assert(pattern_size != 0);

	/*
	 * Make this thread safe when verify_async is set and the verify
	 * pattern includes the offset.
	 */
	if (pattern_need_buffer(td)) {
		pattern = malloc(pattern_size);
		assert(pattern);
		memcpy(pattern, td->o.verify_pattern, pattern_size);
	}

	if (!td->o.verify_pattern_interval) {
		(void)paste_format_inplace(pattern, pattern_size,
					   td->o.verify_fmt, td->o.verify_fmt_sz, io_u);
	}

	/*
	 * We have 3 cases here:
	 * 1. Compare the entire buffer if (1) verify_interval is not set and
	 * (2) verify_pattern_interval is not set
	 * 2. Compare the entire *verify_interval* if (1) verify_interval *is*
	 * set and (2) verify_pattern_interval is not set
	 * 3. Compare *verify_pattern_interval* segments or subsets thereof if
	 * (2) verify_pattern_interval is set
	 */

	buf = (char *) hdr + header_size;
	len = get_hdr_inc(td, io_u) - header_size;
	if (td->o.verify_pattern_interval) {
		unsigned int extent = get_hdr_inc(td, io_u) * vc->hdr_num + header_size;
		pattern_interval_mod = extent % td->o.verify_pattern_interval;
		mod = pattern_interval_mod % pattern_size;
		bytes_todo = min(len, td->o.verify_pattern_interval - pattern_interval_mod);
		io_u->offset += extent / td->o.verify_pattern_interval * td->o.verify_pattern_interval;
	} else {
		mod = (get_hdr_inc(td, io_u) * vc->hdr_num + header_size) % pattern_size;
		bytes_todo = len;
		pattern_interval_mod = 0;
	}

	while (bytes_done < len) {
		if (td->o.verify_pattern_interval) {
			(void)paste_format_inplace(pattern, pattern_size,
					td->o.verify_fmt, td->o.verify_fmt_sz,
					io_u);
		}

		rc = check_pattern(buf, bytes_todo, mod, pattern_size, pattern, header_size);
		if (rc) {
			vc->name = "pattern";
			log_verify_failure(hdr, vc);
			break;
		}

		mod = 0;
		bytes_done += bytes_todo;
		buf += bytes_todo;
		io_u->offset += td->o.verify_pattern_interval;
		bytes_todo = min(len - bytes_done, td->o.verify_pattern_interval);
	}

	io_u->offset = offset;
	if (pattern_need_buffer(td))
		free(pattern);
	return rc;
}

/* [한국어] xxHash 검증 - 데이터의 xxHash를 재계산하여 헤더에 저장된 값과 비교 */
static int verify_io_u_xxhash(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_xxhash *vh = hdr_priv(hdr);
	uint32_t hash;
	void *state;

	dprint(FD_VERIFY, "xxhash verify io_u %p, len %u\n", vc->io_u, hdr->len);

	state = XXH32_init(1);
	XXH32_update(state, p, hdr->len - hdr_size(vc->td, hdr));
	hash = XXH32_digest(state);

	if (vh->hash == hash)
		return 0;

	vc->name = "xxhash";
	vc->good_crc = &vh->hash;
	vc->bad_crc = &hash;
	vc->crc_len = sizeof(hash);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] SHA3 검증 공통 함수 - SHA3-224/256/384/512에서 공유하는 검증 로직 */
static int verify_io_u_sha3(struct verify_header *hdr, struct vcont *vc,
			    struct fio_sha3_ctx *sha3_ctx, uint8_t *sha,
			    unsigned int sha_size, const char *name)
{
	void *p = io_u_verify_off(hdr, vc);

	dprint(FD_VERIFY, "%s verify io_u %p, len %u\n", name, vc->io_u, hdr->len);

	fio_sha3_update(sha3_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_sha3_final(sha3_ctx);

	if (!memcmp(sha, sha3_ctx->sha, sha_size))
		return 0;

	vc->name = name;
	vc->good_crc = sha;
	vc->bad_crc = sha3_ctx->sha;
	vc->crc_len = sha_size;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] SHA3-224 검증 */
static int verify_io_u_sha3_224(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_224 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_224_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_224_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_224_DIGEST_SIZE, "sha3-224");
}

/* [한국어] SHA3-256 검증 */
static int verify_io_u_sha3_256(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_256 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_256_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_256_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_256_DIGEST_SIZE, "sha3-256");
}

/* [한국어] SHA3-384 검증 */
static int verify_io_u_sha3_384(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_384 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_384_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_384_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_384_DIGEST_SIZE, "sha3-384");
}

/* [한국어] SHA3-512 검증 */
static int verify_io_u_sha3_512(struct verify_header *hdr, struct vcont *vc)
{
	struct vhdr_sha3_512 *vh = hdr_priv(hdr);
	uint8_t sha[SHA3_512_DIGEST_SIZE];
	struct fio_sha3_ctx sha3_ctx = {
		.sha = sha,
	};

	fio_sha3_512_init(&sha3_ctx);

	return verify_io_u_sha3(hdr, vc, &sha3_ctx, vh->sha,
				SHA3_512_DIGEST_SIZE, "sha3-512");
}

/* [한국어] SHA-512 검증 - 데이터의 SHA-512를 재계산하여 헤더와 비교 */
static int verify_io_u_sha512(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha512 *vh = hdr_priv(hdr);
	uint8_t sha512[128];
	struct fio_sha512_ctx sha512_ctx = {
		.buf = sha512,
	};

	dprint(FD_VERIFY, "sha512 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_sha512_init(&sha512_ctx);
	fio_sha512_update(&sha512_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_sha512_final(&sha512_ctx);

	if (!memcmp(vh->sha512, sha512_ctx.buf, sizeof(sha512)))
		return 0;

	vc->name = "sha512";
	vc->good_crc = vh->sha512;
	vc->bad_crc = sha512_ctx.buf;
	vc->crc_len = sizeof(vh->sha512);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] SHA-256 검증 - 데이터의 SHA-256을 재계산하여 헤더와 비교 */
static int verify_io_u_sha256(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha256 *vh = hdr_priv(hdr);
	uint8_t sha256[64];
	struct fio_sha256_ctx sha256_ctx = {
		.buf = sha256,
	};

	dprint(FD_VERIFY, "sha256 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_sha256_init(&sha256_ctx);
	fio_sha256_update(&sha256_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_sha256_final(&sha256_ctx);

	if (!memcmp(vh->sha256, sha256_ctx.buf, sizeof(sha256)))
		return 0;

	vc->name = "sha256";
	vc->good_crc = vh->sha256;
	vc->bad_crc = sha256_ctx.buf;
	vc->crc_len = sizeof(vh->sha256);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] SHA-1 검증 - 데이터의 SHA-1을 재계산하여 헤더와 비교 */
static int verify_io_u_sha1(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_sha1 *vh = hdr_priv(hdr);
	uint32_t sha1[5];
	struct fio_sha1_ctx sha1_ctx = {
		.H = sha1,
	};

	dprint(FD_VERIFY, "sha1 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_sha1_init(&sha1_ctx);
	fio_sha1_update(&sha1_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_sha1_final(&sha1_ctx);

	if (!memcmp(vh->sha1, sha1_ctx.H, sizeof(sha1)))
		return 0;

	vc->name = "sha1";
	vc->good_crc = vh->sha1;
	vc->bad_crc = sha1_ctx.H;
	vc->crc_len = sizeof(vh->sha1);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] CRC7 검증 - 데이터의 CRC7을 재계산하여 헤더와 비교 */
static int verify_io_u_crc7(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc7 *vh = hdr_priv(hdr);
	unsigned char c;

	dprint(FD_VERIFY, "crc7 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc7(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc7)
		return 0;

	vc->name = "crc7";
	vc->good_crc = &vh->crc7;
	vc->bad_crc = &c;
	vc->crc_len = 1;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] CRC16 검증 - 데이터의 CRC16을 재계산하여 헤더와 비교 */
static int verify_io_u_crc16(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc16 *vh = hdr_priv(hdr);
	unsigned short c;

	dprint(FD_VERIFY, "crc16 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc16(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc16)
		return 0;

	vc->name = "crc16";
	vc->good_crc = &vh->crc16;
	vc->bad_crc = &c;
	vc->crc_len = 2;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] CRC64 검증 - 데이터의 CRC64를 재계산하여 헤더와 비교 */
static int verify_io_u_crc64(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc64 *vh = hdr_priv(hdr);
	unsigned long long c;

	dprint(FD_VERIFY, "crc64 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc64(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc64)
		return 0;

	vc->name = "crc64";
	vc->good_crc = &vh->crc64;
	vc->bad_crc = &c;
	vc->crc_len = 8;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] CRC32 검증 - 데이터의 CRC32를 재계산하여 헤더와 비교 */
static int verify_io_u_crc32(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc32 *vh = hdr_priv(hdr);
	uint32_t c;

	dprint(FD_VERIFY, "crc32 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc32(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc32)
		return 0;

	vc->name = "crc32";
	vc->good_crc = &vh->crc32;
	vc->bad_crc = &c;
	vc->crc_len = 4;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] CRC32C (Castagnoli) 검증 - 데이터의 CRC32C를 재계산하여 헤더와 비교 */
static int verify_io_u_crc32c(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_crc32 *vh = hdr_priv(hdr);
	uint32_t c;

	dprint(FD_VERIFY, "crc32c verify io_u %p, len %u\n", vc->io_u, hdr->len);

	c = fio_crc32c(p, hdr->len - hdr_size(vc->td, hdr));

	if (c == vh->crc32)
		return 0;

	vc->name = "crc32c";
	vc->good_crc = &vh->crc32;
	vc->bad_crc = &c;
	vc->crc_len = 4;
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/* [한국어] MD5 검증 - 데이터의 MD5 해시를 재계산하여 헤더와 비교 */
static int verify_io_u_md5(struct verify_header *hdr, struct vcont *vc)
{
	void *p = io_u_verify_off(hdr, vc);
	struct vhdr_md5 *vh = hdr_priv(hdr);
	uint32_t hash[MD5_HASH_WORDS];
	struct fio_md5_ctx md5_ctx = {
		.hash = hash,
	};

	dprint(FD_VERIFY, "md5 verify io_u %p, len %u\n", vc->io_u, hdr->len);

	fio_md5_init(&md5_ctx);
	fio_md5_update(&md5_ctx, p, hdr->len - hdr_size(vc->td, hdr));
	fio_md5_final(&md5_ctx);

	if (!memcmp(vh->md5_digest, md5_ctx.hash, sizeof(hash)))
		return 0;

	vc->name = "md5";
	vc->good_crc = vh->md5_digest;
	vc->bad_crc = md5_ctx.hash;
	vc->crc_len = sizeof(hash);
	log_verify_failure(hdr, vc);
	return EILSEQ;
}

/*
 * Push IO verification to a separate thread
 */
/*
 * [한국어] I/O 검증을 비동기 스레드로 오프로드
 *
 * io_u를 verify_list에 추가하고 검증 스레드에 시그널을 보낸다.
 * 이를 통해 메인 I/O 스레드는 검증 완료를 기다리지 않고 다음 I/O를 진행할 수 있다.
 */
int verify_io_u_async(struct thread_data *td, struct io_u **io_u_ptr)
{
	struct io_u *io_u = *io_u_ptr;

	pthread_mutex_lock(&td->io_u_lock);

	if (io_u->file)
		put_file_log(td, io_u->file);  /* [한국어] 파일 참조 카운트 감소 */

	/* [한국어] 현재 depth에서 제거 (비동기 검증으로 넘기므로) */
	if (io_u->flags & IO_U_F_IN_CUR_DEPTH) {
		td->cur_depth--;
		io_u_clear(td, io_u, IO_U_F_IN_CUR_DEPTH);
	}
	flist_add_tail(&io_u->verify_list, &td->verify_list);  /* [한국어] 검증 대기 리스트에 추가 */
	*io_u_ptr = NULL;

	pthread_cond_signal(&td->verify_cond);   /* [한국어] 검증 스레드를 깨움 */
	pthread_mutex_unlock(&td->io_u_lock);
	return 0;
}

/*
 * Thanks Rusty, for spending the time so I don't have to.
 *
 * http://rusty.ozlabs.org/?p=560
 */
/*
 * [한국어] 메모리 영역이 모두 0인지 빠르게 확인 (TRIM 검증에 사용)
 *
 * 처음 16바이트를 수동으로 확인한 후, 나머지는 자기 자신과 memcmp하는
 * Rusty Russell의 트릭을 사용하여 빠르게 검사
 */
static int mem_is_zero(const void *data, size_t length)
{
	const unsigned char *p = data;
	size_t len;

	/* Check first 16 bytes manually */
	/* [한국어] 처음 16바이트를 수동으로 확인 */
	for (len = 0; len < 16; len++) {
		if (!length)
			return 1;
		if (*p)
			return 0;
		p++;
		length--;
	}

	/* Now we know that's zero, memcmp with self. */
	/* [한국어] 앞 16바이트가 0임을 확인했으므로, 나머지를 이 영역과 비교 */
	return memcmp(data, p, length) == 0;
}

/* [한국어] 느린 버전의 zero 확인 - 첫 번째 0이 아닌 바이트의 오프셋도 반환 */
static int mem_is_zero_slow(const void *data, size_t length, size_t *offset)
{
	const unsigned char *p = data;

	*offset = 0;
	while (length) {
		if (*p)
			break;
		(*offset)++;
		length--;
		p++;
	}

	return !length;
}

/*
 * [한국어] TRIM된 영역의 검증 - TRIM 후 데이터가 모두 0인지 확인
 *
 * trim_zero 옵션이 켜져 있을 때만 동작.
 * TRIM된 블록은 0으로 반환되어야 하며, 그렇지 않으면 검증 실패
 */
static int verify_trimmed_io_u(struct thread_data *td, struct io_u *io_u)
{
	size_t offset;

	if (!td->o.trim_zero)
		return 0;

	if (mem_is_zero(io_u->buf, io_u->buflen))
		return 0;

	mem_is_zero_slow(io_u->buf, io_u->buflen, &offset);

	log_err("trim: verify failed at file %s offset %llu, length %llu"
		", block offset %lu\n",
			io_u->file->file_name, io_u->verify_offset, io_u->buflen,
			(unsigned long) offset);
	return EILSEQ;
}

/*
 * [한국어] 검증 헤더의 유효성을 확인
 *
 * 다음 항목들을 순서대로 검증한다:
 *   1) magic  - 매직 넘버(0xacca)가 맞는지
 *   2) version - 헤더 버전이 현재 버전과 일치하는지
 *   3) len    - 헤더 길이가 기대값과 일치하는지
 *   4) rand_seed - 난수 시드가 일치하는지 (verify_header_seed 옵션 사용 시)
 *   5) offset - 블록 오프셋이 일치하는지
 *   6) numberio - I/O 시퀀스 번호가 일치하는지 (쓰기 워크로드 + 고정 블록 크기 시)
 *   7) crc32  - 헤더 자체의 CRC32C가 맞는지
 */
static int verify_header(struct io_u *io_u, struct thread_data *td,
			 struct verify_header *hdr, unsigned int hdr_num,
			 unsigned int hdr_len)
{
	void *p = hdr;
	uint32_t crc;

	if (hdr->magic != FIO_HDR_MAGIC) {
		log_err("verify: bad magic header %x, wanted %x",
			hdr->magic, FIO_HDR_MAGIC);
		goto err;
	}
	if (hdr->version != VERIFY_HEADER_VERSION) {
		log_err("verify: unsupported header version %x, wanted %x. Are you trying to verify across versions of fio?",
			hdr->version, VERIFY_HEADER_VERSION);
		goto err;
	}
	if (hdr->len != hdr_len) {
		log_err("verify: bad header length %u, wanted %u",
			hdr->len, hdr_len);
		goto err;
	}
	if (td->o.verify_header_seed && (hdr->rand_seed != io_u->rand_seed)) {
		log_err("verify: bad header rand_seed %"PRIu64
			", wanted %"PRIu64,
			hdr->rand_seed, io_u->rand_seed);
		goto err;
	}
	if (hdr->offset != io_u->verify_offset + hdr_num * td->o.verify_interval) {
		log_err("verify: bad header offset %"PRIu64
			", wanted %llu",
			hdr->offset, io_u->verify_offset);
		goto err;
	}

	/*
	 * For read-only workloads, the program cannot be certain of the
	 * last numberio written to a block. Checking of numberio will be
	 * done only for workloads that write data.  For verify_only or
	 * any mode de-selecting verify_write_sequence, numberio check is
	 * skipped.
	 */
	/* [한국어] numberio 검증: 쓰기 워크로드이고, 블록 크기가 고정이며,
	 * 시간 기반이 아닌 경우에만 수행. 읽기 전용이면 마지막 쓰기 번호를 알 수 없으므로 건너뜀 */
	if (td_write(td) && (td_min_bs(td) == td_max_bs(td)) &&
	    !td->o.time_based)
		if (td->o.verify_write_sequence)
			if (hdr->numberio != io_u->numberio) {
				log_err("verify: bad header numberio %"PRIu64
					", wanted %"PRIu64,
					hdr->numberio, io_u->numberio);
				goto err;
			}

	crc = fio_crc32c(p, offsetof(struct verify_header, crc32));
	if (crc != hdr->crc32) {
		log_err("verify: bad header crc %x, calculated %x",
			hdr->crc32, crc);
		goto err;
	}
	return 0;

err:
	log_err(" at file %s offset %llu, length %u"
		" (requested block: offset=%llu, length=%llu)\n",
		io_u->file->file_name,
		io_u->verify_offset + hdr_num * hdr_len, hdr_len,
		io_u->verify_offset, io_u->buflen);

	if (td->o.verify_dump)
		dump_buf(p, hdr_len, io_u->verify_offset + hdr_num * hdr_len,
				"hdr_fail", io_u->file);

	return EILSEQ;
}

/*
 * [한국어] 메인 검증 함수 - 읽어온 io_u 데이터의 무결성을 검증
 *
 * 검증 흐름:
 *   1) VERIFY_NULL이거나 READ가 아니면 검증 건너뜀
 *   2) 가짜 I/O 엔진(null 등)이면 검증 건너뜀
 *   3) TRIM된 블록이면 verify_trimmed_io_u()로 처리
 *   4) 각 검증 블록(hdr_inc 간격)에 대해:
 *      a) verify_header()로 헤더 유효성 확인
 *      b) 검증 유형에 따라 해당 알고리즘의 검증 함수 호출
 *   5) 치명적 에러 시 스레드 종료 표시
 */
int verify_io_u(struct thread_data *td, struct io_u **io_u_ptr)
{
	struct verify_header *hdr;
	struct io_u *io_u = *io_u_ptr;
	unsigned int header_size, hdr_inc, hdr_num = 0;
	void *p;
	int ret;

	/* [한국어] 검증이 NULL이거나 읽기가 아닌 경우 검증 불필요 */
	if (td->o.verify == VERIFY_NULL || io_u->ddir != DDIR_READ)
		return 0;
	/*
	 * If the IO engine is faking IO (like null), then just pretend
	 * we verified everything.
	 */
	/* [한국어] 가짜 I/O 엔진(null 등)은 실제 데이터가 없으므로 검증 건너뜀 */
	if (td_ioengine_flagged(td, FIO_FAKEIO))
		return 0;

	/*
	 * If data has already been verified from the device, we can skip
	 * the actual verification phase here.
	 */
	/* [한국어] 장치에서 이미 검증된 데이터는 소프트웨어 검증을 건너뜀 */
	if (io_u->flags & IO_U_F_VER_IN_DEV)
		return 0;

	if (io_u->flags & IO_U_F_TRIMMED) {
		ret = verify_trimmed_io_u(td, io_u);
		goto done;
	}

	hdr_inc = get_hdr_inc(td, io_u);

	ret = 0;
	for (p = io_u->buf; p < io_u->buf + io_u->buflen;
	     p += hdr_inc, hdr_num++) {
		struct vcont vc = {
			.io_u		= io_u,
			.hdr_num	= hdr_num,
			.td		= td,
		};
		unsigned int verify_type;

		if (ret && td->o.verify_fatal)
			break;

		header_size = __hdr_size(td->o.verify);
		if (td->o.verify_offset)
			memswp(p, p + td->o.verify_offset, header_size);
		hdr = p;

		if (td->o.verify != VERIFY_PATTERN_NO_HDR) {
			ret = verify_header(io_u, td, hdr, hdr_num, hdr_inc);
			if (ret)
				return ret;
		}

		if (td->o.verify != VERIFY_NONE)
			verify_type = td->o.verify;
		else
			verify_type = hdr->verify_type;

		switch (verify_type) {
		case VERIFY_HDR_ONLY:
			/* Header is always verified, check if pattern is left
			 * for verification. */
			if (td->o.verify_pattern_bytes)
				ret = verify_io_u_pattern(hdr, &vc);
			break;
		case VERIFY_MD5:
			ret = verify_io_u_md5(hdr, &vc);
			break;
		case VERIFY_CRC64:
			ret = verify_io_u_crc64(hdr, &vc);
			break;
		case VERIFY_CRC32C:
		case VERIFY_CRC32C_INTEL:
			ret = verify_io_u_crc32c(hdr, &vc);
			break;
		case VERIFY_CRC32:
			ret = verify_io_u_crc32(hdr, &vc);
			break;
		case VERIFY_CRC16:
			ret = verify_io_u_crc16(hdr, &vc);
			break;
		case VERIFY_CRC7:
			ret = verify_io_u_crc7(hdr, &vc);
			break;
		case VERIFY_SHA256:
			ret = verify_io_u_sha256(hdr, &vc);
			break;
		case VERIFY_SHA512:
			ret = verify_io_u_sha512(hdr, &vc);
			break;
		case VERIFY_SHA3_224:
			ret = verify_io_u_sha3_224(hdr, &vc);
			break;
		case VERIFY_SHA3_256:
			ret = verify_io_u_sha3_256(hdr, &vc);
			break;
		case VERIFY_SHA3_384:
			ret = verify_io_u_sha3_384(hdr, &vc);
			break;
		case VERIFY_SHA3_512:
			ret = verify_io_u_sha3_512(hdr, &vc);
			break;
		case VERIFY_XXHASH:
			ret = verify_io_u_xxhash(hdr, &vc);
			break;
		case VERIFY_SHA1:
			ret = verify_io_u_sha1(hdr, &vc);
			break;
		case VERIFY_PATTERN:
		case VERIFY_PATTERN_NO_HDR:
			ret = verify_io_u_pattern(hdr, &vc);
			break;
		default:
			log_err("Bad verify type %u\n", hdr->verify_type);
			ret = EINVAL;
		}

		if (ret && verify_type != hdr->verify_type && verify_type != VERIFY_PATTERN_NO_HDR)
			log_err("fio: verify type mismatch (%u media, %u given)\n",
					hdr->verify_type, verify_type);
	}

done:
	/* [한국어] 검증 실패 시 verify_fatal 옵션이 켜져 있으면 스레드 종료 표시 */
	if (ret && td->o.verify_fatal)
		fio_mark_td_terminate(td);

	return ret;
}

/* ================================================================
 * [한국어] 체크섬 계산 함수들 (fill_*) - 쓰기 시 데이터의 체크섬을 계산하여 헤더에 저장
 * ================================================================ */

/* [한국어] xxHash 체크섬 계산 및 헤더에 저장 */
static void fill_xxhash(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_xxhash *vh = hdr_priv(hdr);
	void *state;

	state = XXH32_init(1);
	XXH32_update(state, p, len);
	vh->hash = XXH32_digest(state);
}

/* [한국어] SHA3 공통 계산 함수 - update + final 호출 */
static void fill_sha3(struct fio_sha3_ctx *sha3_ctx, void *p, unsigned int len)
{
	fio_sha3_update(sha3_ctx, p, len);
	fio_sha3_final(sha3_ctx);
}

/* [한국어] SHA3-224 체크섬 계산 및 헤더에 저장 */
static void fill_sha3_224(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_224 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_224_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] SHA3-256 체크섬 계산 및 헤더에 저장 */
static void fill_sha3_256(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_256 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_256_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] SHA3-384 체크섬 계산 및 헤더에 저장 */
static void fill_sha3_384(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_384 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_384_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] SHA3-512 체크섬 계산 및 헤더에 저장 */
static void fill_sha3_512(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha3_512 *vh = hdr_priv(hdr);
	struct fio_sha3_ctx sha3_ctx = {
		.sha = vh->sha,
	};

	fio_sha3_512_init(&sha3_ctx);
	fill_sha3(&sha3_ctx, p, len);
}

/* [한국어] SHA-512 체크섬 계산 및 헤더에 저장 */
static void fill_sha512(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha512 *vh = hdr_priv(hdr);
	struct fio_sha512_ctx sha512_ctx = {
		.buf = vh->sha512,
	};

	fio_sha512_init(&sha512_ctx);
	fio_sha512_update(&sha512_ctx, p, len);
	fio_sha512_final(&sha512_ctx);
}

/* [한국어] SHA-256 체크섬 계산 및 헤더에 저장 */
static void fill_sha256(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha256 *vh = hdr_priv(hdr);
	struct fio_sha256_ctx sha256_ctx = {
		.buf = vh->sha256,
	};

	fio_sha256_init(&sha256_ctx);
	fio_sha256_update(&sha256_ctx, p, len);
	fio_sha256_final(&sha256_ctx);
}

/* [한국어] SHA-1 체크섬 계산 및 헤더에 저장 */
static void fill_sha1(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_sha1 *vh = hdr_priv(hdr);
	struct fio_sha1_ctx sha1_ctx = {
		.H = vh->sha1,
	};

	fio_sha1_init(&sha1_ctx);
	fio_sha1_update(&sha1_ctx, p, len);
	fio_sha1_final(&sha1_ctx);
}

/* [한국어] CRC7 체크섬 계산 및 헤더에 저장 */
static void fill_crc7(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc7 *vh = hdr_priv(hdr);

	vh->crc7 = fio_crc7(p, len);
}

/* [한국어] CRC16 체크섬 계산 및 헤더에 저장 */
static void fill_crc16(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc16 *vh = hdr_priv(hdr);

	vh->crc16 = fio_crc16(p, len);
}

/* [한국어] CRC32 체크섬 계산 및 헤더에 저장 */
static void fill_crc32(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc32 *vh = hdr_priv(hdr);

	vh->crc32 = fio_crc32(p, len);
}

/* [한국어] CRC32C (Castagnoli) 체크섬 계산 및 헤더에 저장 */
static void fill_crc32c(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc32 *vh = hdr_priv(hdr);

	vh->crc32 = fio_crc32c(p, len);
}

/* [한국어] CRC64 체크섬 계산 및 헤더에 저장 */
static void fill_crc64(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_crc64 *vh = hdr_priv(hdr);

	vh->crc64 = fio_crc64(p, len);
}

/* [한국어] MD5 해시 계산 및 헤더에 저장 */
static void fill_md5(struct verify_header *hdr, void *p, unsigned int len)
{
	struct vhdr_md5 *vh = hdr_priv(hdr);
	struct fio_md5_ctx md5_ctx = {
		.hash = (uint32_t *) vh->md5_digest,
	};

	fio_md5_init(&md5_ctx);
	fio_md5_update(&md5_ctx, p, len);
	fio_md5_final(&md5_ctx);
}

/*
 * [한국어] 검증 헤더의 모든 필드를 채우는 내부 함수
 *
 * 매직 넘버, 버전, 검증 유형, 길이, 시드, 오프셋, 시간, 스레드 번호,
 * I/O 시퀀스 번호를 설정하고, 마지막에 헤더 자체의 CRC32C를 계산하여 저장
 */
static void __fill_hdr(struct thread_data *td, struct io_u *io_u,
		       struct verify_header *hdr, unsigned int header_num,
		       unsigned int header_len, uint64_t rand_seed)
{
	void *p = hdr;

	hdr->magic = FIO_HDR_MAGIC;              /* [한국어] 매직 넘버 설정 */
	hdr->version = VERIFY_HEADER_VERSION;    /* [한국어] 헤더 버전 설정 */
	hdr->verify_type = td->o.verify;         /* [한국어] 검증 유형 설정 */
	hdr->len = header_len;                   /* [한국어] 블록 전체 길이 */
	hdr->rand_seed = rand_seed;              /* [한국어] 데이터 생성에 사용된 시드 */
	hdr->offset = io_u->verify_offset + header_num * td->o.verify_interval;  /* [한국어] 파일 내 오프셋 */
	hdr->time_sec = io_u->start_time.tv_sec;    /* [한국어] I/O 시작 시간 (초) */
	hdr->time_nsec = io_u->start_time.tv_nsec;  /* [한국어] I/O 시작 시간 (나노초) */
	hdr->thread = td->thread_number;             /* [한국어] 쓴 스레드 번호 */
	hdr->numberio = io_u->numberio;              /* [한국어] I/O 시퀀스 번호 */
	hdr->crc32 = fio_crc32c(p, offsetof(struct verify_header, crc32));  /* [한국어] 헤더 자체의 CRC32C */
}


/* [한국어] 헤더 채우기 래퍼 - PATTERN_NO_HDR 모드가 아닐 때만 헤더를 채움 */
static void fill_hdr(struct thread_data *td, struct io_u *io_u,
		     struct verify_header *hdr, unsigned int header_num,
		     unsigned int header_len, uint64_t rand_seed)
{
	if (td->o.verify != VERIFY_PATTERN_NO_HDR)
		__fill_hdr(td, io_u, hdr, header_num, header_len, rand_seed);
}

/*
 * [한국어] 검증 헤더를 완성하는 함수 - 헤더 필드를 채우고, 데이터 영역의 체크섬을 계산
 *
 * 1) fill_hdr()로 헤더 메타데이터를 채움
 * 2) 검증 유형에 따라 데이터 영역의 체크섬을 계산하여 체크섬별 헤더(vhdr_*)에 저장
 * 3) verify_offset이 설정되면 헤더와 데이터를 교환 (헤더를 오프셋 위치로 이동)
 */
static void populate_hdr(struct thread_data *td, struct io_u *io_u,
			 struct verify_header *hdr, unsigned int header_num,
			 unsigned int header_len)
{
	unsigned int data_len;
	void *data;
	char *p;

	p = (char *) hdr;

	/* [한국어] 헤더 메타데이터 (매직, 버전, 오프셋 등) 채우기 */
	fill_hdr(td, io_u, hdr, header_num, header_len, io_u->rand_seed);

	if (header_len <= hdr_size(td, hdr)) {
		td_verror(td, EINVAL, "Blocksize too small");
		return;
	}
	data_len = header_len - hdr_size(td, hdr);  /* [한국어] 데이터 영역 길이 = 전체 블록 - 헤더 크기 */

	/* [한국어] 데이터 영역 포인터 (헤더 바로 뒤) */
	data = p + hdr_size(td, hdr);
	/* [한국어] 검증 유형에 따라 적절한 체크섬 계산 함수 호출 */
	switch (td->o.verify) {
	case VERIFY_MD5:
		dprint(FD_VERIFY, "fill md5 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_md5(hdr, data, data_len);
		break;
	case VERIFY_CRC64:
		dprint(FD_VERIFY, "fill crc64 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc64(hdr, data, data_len);
		break;
	case VERIFY_CRC32C:
	case VERIFY_CRC32C_INTEL:
		dprint(FD_VERIFY, "fill crc32c io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc32c(hdr, data, data_len);
		break;
	case VERIFY_CRC32:
		dprint(FD_VERIFY, "fill crc32 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc32(hdr, data, data_len);
		break;
	case VERIFY_CRC16:
		dprint(FD_VERIFY, "fill crc16 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc16(hdr, data, data_len);
		break;
	case VERIFY_CRC7:
		dprint(FD_VERIFY, "fill crc7 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_crc7(hdr, data, data_len);
		break;
	case VERIFY_SHA256:
		dprint(FD_VERIFY, "fill sha256 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha256(hdr, data, data_len);
		break;
	case VERIFY_SHA512:
		dprint(FD_VERIFY, "fill sha512 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha512(hdr, data, data_len);
		break;
	case VERIFY_SHA3_224:
		dprint(FD_VERIFY, "fill sha3-224 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_224(hdr, data, data_len);
		break;
	case VERIFY_SHA3_256:
		dprint(FD_VERIFY, "fill sha3-256 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_256(hdr, data, data_len);
		break;
	case VERIFY_SHA3_384:
		dprint(FD_VERIFY, "fill sha3-384 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_384(hdr, data, data_len);
		break;
	case VERIFY_SHA3_512:
		dprint(FD_VERIFY, "fill sha3-512 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha3_512(hdr, data, data_len);
		break;
	case VERIFY_XXHASH:
		dprint(FD_VERIFY, "fill xxhash io_u %p, len %u\n",
						io_u, hdr->len);
		fill_xxhash(hdr, data, data_len);
		break;
	case VERIFY_SHA1:
		dprint(FD_VERIFY, "fill sha1 io_u %p, len %u\n",
						io_u, hdr->len);
		fill_sha1(hdr, data, data_len);
		break;
	case VERIFY_HDR_ONLY:
	case VERIFY_PATTERN:
	case VERIFY_PATTERN_NO_HDR:
		/* nothing to do here */
		break;
	default:
		log_err("fio: bad verify type: %d\n", td->o.verify);
		assert(0);
	}

	/* [한국어] verify_offset이 설정되면 헤더를 지정된 오프셋 위치로 이동 (memswp으로 교환) */
	if (td->o.verify_offset && hdr_size(td, hdr))
		memswp(p, p + td->o.verify_offset, hdr_size(td, hdr));
}

/*
 * fill body of io_u->buf with random data and add a header with the
 * checksum of choice
 */
/*
 * [한국어] 쓰기 전 io_u 버퍼 준비 - 패턴/랜덤 데이터로 채우고 체크섬 헤더를 삽입
 *
 * VERIFY_NULL이면 아무 것도 하지 않음 (검증 비활성화)
 * 그 외에는 fill_pattern_headers()를 호출하여 전체 버퍼를 준비
 */
void populate_verify_io_u(struct thread_data *td, struct io_u *io_u)
{
	if (td->o.verify == VERIFY_NULL)
		return;

	fill_pattern_headers(td, io_u, 0, 0);
}

/*
 * [한국어] 검증할 다음 I/O 조각을 io_hist에서 꺼내옴
 *
 * I/O 이력(io_hist_tree 또는 io_hist_list)에서 완료된 쓰기 조각을 가져와
 * io_u에 오프셋, 길이, 파일 등의 정보를 설정한다.
 * 검증 모드에서는 이전에 쓴 블록을 다시 읽어서 검증하는데, 이 함수가 그 블록 정보를 제공한다.
 *
 * 반환값: 0=성공 (io_u가 채워짐), 1=검증할 항목 없음
 */
int get_next_verify(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = NULL;

	/*
	 * this io_u is from a requeue, we already filled the offsets
	 */
	/* [한국어] 재큐된 io_u는 이미 오프셋이 설정되어 있으므로 바로 반환 */
	if (io_u->file)
		return 0;

	/* [한국어] RB 트리에서 먼저 검색, 없으면 리스트에서 검색 */
	if (!RB_EMPTY_ROOT(&td->io_hist_tree)) {
		struct fio_rb_node *n = rb_first(&td->io_hist_tree);

		ipo = rb_entry(n, struct io_piece, rb_node);

		/*
		 * Ensure that the associated IO has completed
		 */
		/* [한국어] 아직 진행 중인(in-flight) I/O는 검증할 수 없으므로 건너뜀 */
		if (atomic_load_acquire(&ipo->flags) & IP_F_IN_FLIGHT)
			goto nothing;

		rb_erase(n, &td->io_hist_tree);
		assert(ipo->flags & IP_F_ONRB);
		ipo->flags &= ~IP_F_ONRB;
	} else if (!flist_empty(&td->io_hist_list)) {
		ipo = flist_first_entry(&td->io_hist_list, struct io_piece, list);

		/*
		 * Ensure that the associated IO has completed
		 */
		/* [한국어] 리스트에서도 in-flight 확인 */
		if (atomic_load_acquire(&ipo->flags) & IP_F_IN_FLIGHT)
			goto nothing;

		flist_del(&ipo->list);
		assert(ipo->flags & IP_F_ONLIST);
		ipo->flags &= ~IP_F_ONLIST;
	}

	/* [한국어] io_piece에서 io_u로 검증에 필요한 정보를 복사 */
	if (ipo) {
		td->io_hist_len--;

		io_u->offset = ipo->offset;           /* [한국어] 파일 내 오프셋 */
		io_u->verify_offset = ipo->offset;    /* [한국어] 검증용 오프셋 */
		io_u->buflen = ipo->len;              /* [한국어] 블록 길이 */
		io_u->numberio = ipo->numberio;       /* [한국어] I/O 시퀀스 번호 */
		io_u->file = ipo->file;               /* [한국어] 대상 파일 */
		io_u_set(td, io_u, IO_U_F_VER_LIST);

		if (ipo->flags & IP_F_TRIMMED)
			io_u_set(td, io_u, IO_U_F_TRIMMED);

		if (!fio_file_open(io_u->file)) {
			int r = td_io_open_file(td, io_u->file);

			if (r) {
				dprint(FD_VERIFY, "failed file %s open\n",
						io_u->file->file_name);
				return 1;
			}
		}

		get_file(ipo->file);                  /* [한국어] 파일 참조 카운트 증가 */
		assert(fio_file_open(io_u->file));
		io_u->ddir = DDIR_READ;               /* [한국어] 검증은 읽기로 수행 */
		io_u->xfer_buf = io_u->buf;
		io_u->xfer_buflen = io_u->buflen;

		remove_trim_entry(td, ipo);
		free(ipo);
		dprint(FD_VERIFY, "get_next_verify: ret io_u %p\n", io_u);

		/* [한국어] 패턴이 없으면 검증 상태에서 시드를 생성하여 데이터 재현에 사용 */
		if (!td->o.verify_pattern_bytes) {
			io_u->rand_seed = __rand(&td->verify_state);
			if (sizeof(int) != sizeof(long *))
				io_u->rand_seed *= __rand(&td->verify_state);
		}
		return 0;
	}

nothing:
	dprint(FD_VERIFY, "get_next_verify: empty\n");
	return 1;
}

/*
 * [한국어] 검증 모듈 초기화 - CRC32C 하드웨어 가속 탐지
 *
 * CRC32C 검증이 선택된 경우 ARM64 및 Intel 하드웨어 가속 지원을 확인한다.
 * 하드웨어 가속이 가능하면 소프트웨어 구현 대신 하드웨어 명령어를 사용하여 성능 향상
 */
void fio_verify_init(struct thread_data *td)
{
	if (td->o.verify == VERIFY_CRC32C_INTEL ||
	    td->o.verify == VERIFY_CRC32C) {
		crc32c_arm64_probe();   /* [한국어] ARM64 CRC32C 하드웨어 가속 탐지 */
		crc32c_intel_probe();   /* [한국어] Intel SSE4.2 CRC32C 하드웨어 가속 탐지 */
	}
}

/*
 * [한국어] 비동기 검증 워커 스레드의 메인 루프
 *
 * 동작 흐름:
 *   1) verify_cpumask가 설정되면 CPU 친화도(affinity) 설정
 *   2) verify_list에 항목이 들어올 때까지 조건변수로 대기
 *   3) 리스트에서 io_u를 꺼내 verify_io_u()로 검증
 *   4) 치명적이지 않은 에러는 카운트만 하고 계속 진행
 *   5) 치명적 에러 또는 종료 신호 시 루프 탈출
 */
static void *verify_async_thread(void *data)
{
	struct thread_data *td = data;
	struct io_u *io_u;
	int ret = 0;

	/* [한국어] 검증 스레드의 CPU 친화도 설정 (verify_cpumask 옵션) */
	if (fio_option_is_set(&td->o, verify_cpumask) &&
	    fio_setaffinity(td->pid, td->o.verify_cpumask)) {
		log_err("fio: failed setting verify thread affinity\n");
		goto done;
	}

	do {
		FLIST_HEAD(list);

		read_barrier();
		if (td->verify_thread_exit)
			break;

		/* [한국어] 검증할 io_u가 들어올 때까지 조건변수로 대기 */
		pthread_mutex_lock(&td->io_u_lock);

		while (flist_empty(&td->verify_list) &&
		       !td->verify_thread_exit) {
			ret = pthread_cond_wait(&td->verify_cond,
							&td->io_u_lock);
			if (ret) {
				break;
			}
		}

		/* [한국어] 검증 대기 리스트를 로컬 리스트로 옮김 (락 최소화) */
		flist_splice_init(&td->verify_list, &list);
		pthread_mutex_unlock(&td->io_u_lock);

		if (flist_empty(&list))
			continue;

		/* [한국어] 로컬 리스트의 모든 io_u를 순회하며 검증 수행 */
		while (!flist_empty(&list)) {
			io_u = flist_first_entry(&list, struct io_u, verify_list);
			flist_del_init(&io_u->verify_list);

			io_u_set(td, io_u, IO_U_F_NO_FILE_PUT);
			ret = verify_io_u(td, &io_u);  /* [한국어] 실제 검증 수행 */

			put_io_u(td, io_u);  /* [한국어] 검증 완료된 io_u 반환 */
			if (!ret)
				continue;
			/* [한국어] 치명적이지 않은 에러는 카운트만 하고 계속 */
			if (td_non_fatal_error(td, ERROR_TYPE_VERIFY_BIT, ret)) {
				update_error_count(td, ret);
				td_clear_error(td);
				ret = 0;
			}
		}
	} while (!ret);

	if (ret) {
		td_verror(td, ret, "async_verify");
		if (td->o.verify_fatal)
			fio_mark_td_terminate(td);
	}

done:
	/* [한국어] 스레드 종료 처리 - 카운트 감소 및 메인 스레드에 알림 */
	pthread_mutex_lock(&td->io_u_lock);
	td->nr_verify_threads--;
	pthread_cond_signal(&td->free_cond);
	pthread_mutex_unlock(&td->io_u_lock);

	return NULL;
}

/*
 * [한국어] 비동기 검증 스레드 풀 초기화
 *
 * verify_async 옵션에 지정된 수만큼 검증 스레드를 생성한다.
 * 각 스레드는 detach 모드로 동작하며, verify_async_thread()를 실행한다.
 * 모든 스레드 생성에 실패하면 종료 신호를 보내고 에러를 반환한다.
 */
int verify_async_init(struct thread_data *td)
{
	int i, ret;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 2 * PTHREAD_STACK_MIN);  /* [한국어] 최소 스택 크기의 2배 */

	td->verify_thread_exit = 0;

	td->verify_threads = malloc(sizeof(pthread_t) * td->o.verify_async);
	for (i = 0; i < td->o.verify_async; i++) {
		ret = pthread_create(&td->verify_threads[i], &attr,
					verify_async_thread, td);
		if (ret) {
			log_err("fio: async verify creation failed: %s\n",
					strerror(ret));
			break;
		}
		ret = pthread_detach(td->verify_threads[i]);
		if (ret) {
			log_err("fio: async verify thread detach failed: %s\n",
					strerror(ret));
			break;
		}
		td->nr_verify_threads++;
	}

	pthread_attr_destroy(&attr);

	if (i != td->o.verify_async) {
		log_err("fio: only %d verify threads started, exiting\n", i);

		pthread_mutex_lock(&td->io_u_lock);
		td->verify_thread_exit = 1;
		pthread_cond_broadcast(&td->verify_cond);
		pthread_mutex_unlock(&td->io_u_lock);

		return 1;
	}

	return 0;
}

/*
 * [한국어] 비동기 검증 스레드 풀 종료
 *
 * 종료 플래그를 설정하고 모든 검증 스레드가 종료할 때까지 대기한 후
 * 스레드 배열 메모리를 해제한다.
 */
void verify_async_exit(struct thread_data *td)
{
	pthread_mutex_lock(&td->io_u_lock);
	td->verify_thread_exit = 1;                    /* [한국어] 종료 플래그 설정 */
	pthread_cond_broadcast(&td->verify_cond);      /* [한국어] 모든 대기 중인 스레드 깨움 */

	/* [한국어] 모든 검증 스레드가 종료할 때까지 대기 */
	while (td->nr_verify_threads)
		pthread_cond_wait(&td->free_cond, &td->io_u_lock);

	pthread_mutex_unlock(&td->io_u_lock);
	free(td->verify_threads);
	td->verify_threads = NULL;
}

/*
 * [한국어] 블록 오프셋을 패턴 버퍼에 삽입하는 콜백
 *
 * io_u의 현재 오프셋을 리틀엔디안으로 변환하여 패턴 버퍼에 복사.
 * verify_fmt에서 %o 포맷 지시자가 사용될 때 호출된다.
 */
int paste_blockoff(char *buf, unsigned int len, void *priv)
{
	struct io_u *io = priv;
	unsigned long long off;

	typecheck(__typeof__(off), io->offset);
	off = cpu_to_le64((uint64_t)io->offset);
	len = min(len, (unsigned int)sizeof(off));
	memcpy(buf, &off, len);
	return 0;
}

/*
 * [한국어] 모든 스레드의 I/O 상태를 수집하여 all_io_list 구조체로 반환
 *
 * 각 스레드의 I/O depth, 시퀀스 번호, 난수 상태, in-flight 정보 등을
 * 직렬화하여 하나의 버퍼에 저장한다.
 * save_mask로 특정 스레드만 선택하거나 IO_LIST_ALL로 전체를 선택할 수 있다.
 * 검증 상태 저장 (verify_save_state)에서 사용된다.
 */
struct all_io_list *get_all_io_list(int save_mask, size_t *sz)
{
	struct all_io_list *rep;
	size_t depth;
	void *next;
	int nr;

	compiletime_assert(sizeof(struct all_io_list) == 8, "all_io_list");

	/*
	 * Calculate reply space needed. We need one 'io_state' per thread,
	 * and the size will vary depending on depth.
	 */
	/* [한국어] 필요한 메모리 크기 계산: 스레드 수 x io_state + depth별 in-flight 정보 */
	depth = 0;
	nr = 0;
	for_each_td(td) {
		if (save_mask != IO_LIST_ALL && (__td_index + 1) != save_mask)
			continue;
		td->stop_io = 1;
		td->flags |= TD_F_VSTATE_SAVED;
		depth += (td->o.iodepth * td->o.nr_files);
		nr++;
	} end_for_each();

	if (!nr)
		return NULL;

	*sz = sizeof(*rep);
	*sz += nr * sizeof(struct thread_io_list);
	*sz += depth * sizeof(struct inflight_write);
	rep = calloc(1, *sz);

	rep->threads = cpu_to_le64((uint64_t) nr);

	next = &rep->state[0];
	for_each_td(td) {
		struct thread_io_list *s = next;

		if (save_mask != IO_LIST_ALL && (__td_index + 1) != save_mask)
			continue;

		for (int i = 0; i < td->o.iodepth; i++)
			s->inflight[i].numberio = cpu_to_le64(atomic_load_acquire(&td->inflight_numberio[i]));

		s->depth = cpu_to_le32((uint32_t) td->o.iodepth);
		s->numberio = cpu_to_le64((uint64_t) atomic_load_acquire(&td->inflight_issued));
		s->index = cpu_to_le64((uint64_t) __td_index);
		if (td->offset_state.use64) {
			s->rand.state64.s[0] = cpu_to_le64(td->offset_state.state64.s1);
			s->rand.state64.s[1] = cpu_to_le64(td->offset_state.state64.s2);
			s->rand.state64.s[2] = cpu_to_le64(td->offset_state.state64.s3);
			s->rand.state64.s[3] = cpu_to_le64(td->offset_state.state64.s4);
			s->rand.state64.s[4] = cpu_to_le64(td->offset_state.state64.s5);
			s->rand.state64.s[5] = 0;
			s->rand.use64 = cpu_to_le64((uint64_t)1);
		} else {
			s->rand.state32.s[0] = cpu_to_le32(td->offset_state.state32.s1);
			s->rand.state32.s[1] = cpu_to_le32(td->offset_state.state32.s2);
			s->rand.state32.s[2] = cpu_to_le32(td->offset_state.state32.s3);
			s->rand.state32.s[3] = 0;
			s->rand.use64 = 0;
		}
		snprintf((char *) s->name, sizeof(s->name), "%s", td->o.name);
		next = io_list_next(s);
	} end_for_each();

	return rep;
}

/*
 * [한국어] 검증 상태 파일을 열기
 *
 * 쓰기 모드(for_write=1): O_CREAT | O_TRUNC | O_WRONLY | O_SYNC로 열기
 * 읽기 모드(for_write=0): O_RDONLY로 열기
 * 파일명은 verify_state_gen_name()으로 생성 (이름, 접두어, 번호 조합)
 */
static int open_state_file(const char *name, const char *prefix, int num,
			   int for_write)
{
	char out[PATH_MAX];
	int flags;
	int fd;

	if (for_write)
		flags = O_CREAT | O_TRUNC | O_WRONLY | O_SYNC;
	else
		flags = O_RDONLY;

#ifdef _WIN32
	flags |= O_BINARY;
#endif

	verify_state_gen_name(out, sizeof(out), name, prefix, num);

	fd = open(out, flags, 0644);
	if (fd == -1) {
		perror("fio: open state file");
		log_err("fio: state file: %s (for_write=%d)\n", out, for_write);
		return -1;
	}

	return fd;
}

/*
 * [한국어] 개별 스레드의 I/O 상태를 파일에 기록
 *
 * verify_state_hdr(버전, 크기, CRC) 헤더 + thread_io_list 데이터를 파일에 저장.
 * CRC32C로 데이터 무결성을 보장한다.
 */
static int write_thread_list_state(struct thread_io_list *s,
				   const char *prefix)
{
	struct verify_state_hdr hdr;
	uint64_t crc;
	ssize_t ret;
	int fd;

	fd = open_state_file((const char *) s->name, prefix, s->index, 1);
	if (fd == -1)
		return 1;

	crc = fio_crc32c((void *)s, thread_io_list_sz(s));

	hdr.version = cpu_to_le64((uint64_t) VSTATE_HDR_VERSION);
	hdr.size = cpu_to_le64((uint64_t) thread_io_list_sz(s));
	hdr.crc = cpu_to_le64(crc);
	ret = write(fd, &hdr, sizeof(hdr));
	if (ret != sizeof(hdr))
		goto write_fail;

	ret = write(fd, s, thread_io_list_sz(s));
	if (ret != thread_io_list_sz(s)) {
write_fail:
		if (ret < 0)
			perror("fio: write state file");
		log_err("fio: failed to write state file\n");
		ret = 1;
	} else
		ret = 0;

	close(fd);
	return ret;
}

/* [한국어] 모든 스레드의 검증 상태를 개별 파일에 저장 */
void __verify_save_state(struct all_io_list *state, const char *prefix)
{
	struct thread_io_list *s = &state->state[0];
	unsigned int i;

	for (i = 0; i < le64_to_cpu(state->threads); i++) {
		write_thread_list_state(s,  prefix);
		s = io_list_next(s);
	}
}

/*
 * [한국어] 검증 상태 저장의 최상위 함수
 *
 * 모든 스레드의 I/O 상태를 수집하고, "local" 접두어로 상태 파일에 저장.
 * aux_path가 설정되면 해당 경로에 저장, 아니면 현재 디렉토리에 저장.
 */
void verify_save_state(int mask)
{
	struct all_io_list *state;
	size_t sz;

	state = get_all_io_list(mask, &sz);
	if (state) {
		char prefix[PATH_MAX];

		if (aux_path)
			sprintf(prefix, "%s%clocal", aux_path, FIO_OS_PATH_SEPARATOR);
		else
			strcpy(prefix, "local");

		__verify_save_state(state, prefix);
		free(state);
	}
}

/* [한국어] 스레드의 검증 상태 메모리 해제 */
void verify_free_state(struct thread_data *td)
{
	if (td->vstate)
		free(td->vstate);
}

/*
 * [한국어] 로드된 검증 상태를 스레드에 할당
 *
 * 파일에서 읽어온 thread_io_list의 바이트 오더를 호스트 오더로 변환하고,
 * td->vstate에 저장한다. 이후 verify_state_should_stop()에서 참조된다.
 */
void verify_assign_state(struct thread_data *td, void *p)
{
	struct thread_io_list *s = p;
	int i;

	s->depth = le32_to_cpu(s->depth);
	s->numberio = le64_to_cpu(s->numberio);
	s->rand.use64 = le64_to_cpu(s->rand.use64);

	if (s->rand.use64) {
		for (i = 0; i < 6; i++)
			s->rand.state64.s[i] = le64_to_cpu(s->rand.state64.s[i]);
	} else {
		for (i = 0; i < 4; i++)
			s->rand.state32.s[i] = le32_to_cpu(s->rand.state32.s[i]);
	}

	for (i = 0; i < s->depth; i++) {
		s->inflight[i].numberio = le64_to_cpu(s->inflight[i].numberio);
		dprint(FD_VERIFY, "verify_assign_state numberio=%"PRIu64", inflight[%d]=%"PRIu64"\n", s->numberio, i, s->inflight[i].numberio);
	}

	td->vstate = p;
}

/*
 * [한국어] 검증 상태 헤더의 유효성 검사
 *
 * 바이트 오더를 변환하고, 버전과 CRC를 확인하여 상태 파일이 유효한지 판별.
 * 반환값: 0=유효, 1=무효
 */
int verify_state_hdr(struct verify_state_hdr *hdr, struct thread_io_list *s)
{
	uint64_t crc;

	hdr->version = le64_to_cpu(hdr->version);
	hdr->size = le64_to_cpu(hdr->size);
	hdr->crc = le64_to_cpu(hdr->crc);

	if (hdr->version != VSTATE_HDR_VERSION)
		return 1;

	crc = fio_crc32c((void *)s, hdr->size);
	if (crc != hdr->crc)
		return 1;

	return 0;
}

/*
 * [한국어] 파일에서 검증 상태를 로드
 *
 * 상태 파일을 열어 헤더(버전, 크기, CRC)를 읽고 검증한 후,
 * thread_io_list 데이터를 읽어서 verify_assign_state()로 스레드에 할당.
 * verify_state 옵션이 설정되지 않으면 아무 것도 하지 않고 0을 반환.
 */
int verify_load_state(struct thread_data *td, const char *prefix)
{
	struct verify_state_hdr hdr;
	void *s = NULL;
	uint64_t crc;
	ssize_t ret;
	int fd;

	if (!td->o.verify_state)
		return 0;

	fd = open_state_file(td->o.name, prefix, td->thread_number - 1, 0);
	if (fd == -1)
		return 1;

	ret = read(fd, &hdr, sizeof(hdr));
	if (ret != sizeof(hdr)) {
		if (ret < 0)
			td_verror(td, errno, "read verify state hdr");
		log_err("fio: failed reading verify state header\n");
		goto err;
	}

	hdr.version = le64_to_cpu(hdr.version);
	hdr.size = le64_to_cpu(hdr.size);
	hdr.crc = le64_to_cpu(hdr.crc);

	if (hdr.version != VSTATE_HDR_VERSION) {
		log_err("fio: unsupported (%d) version in verify state header\n",
				(unsigned int) hdr.version);
		goto err;
	}

	s = malloc(hdr.size);
	ret = read(fd, s, hdr.size);
	if (ret != hdr.size) {
		if (ret < 0)
			td_verror(td, errno, "read verify state");
		log_err("fio: failed reading verity state\n");
		goto err;
	}

	crc = fio_crc32c(s, hdr.size);
	if (crc != hdr.crc) {
		log_err("fio: verify state is corrupt\n");
		goto err;
	}

	close(fd);

	verify_assign_state(td, s);
	return 0;
err:
	if (s)
		free(s);
	close(fd);
	return 1;
}

/*
 * Use the loaded verify state to know when to stop doing verification
 */
/*
 * [한국어] 로드된 검증 상태를 기반으로 검증 중단 여부를 판별
 *
 * 이전 실행에서 저장된 상태와 현재 I/O 시퀀스 번호를 비교하여:
 *   - numberio >= 저장된 최대 시퀀스: 중단 (이전 실행에서 쓰지 않은 영역)
 *   - numberio < 최대 시퀀스이지만 in-flight였던 쓰기: 중단 (미완료 쓰기)
 *   - 그 외: 계속 검증
 *
 * 이를 통해 비정상 종료 후 재시작 시 안전하게 검증 범위를 제한할 수 있다.
 */
int verify_state_should_stop(struct thread_data *td, uint64_t numberio)
{
	struct thread_io_list *s = td->vstate;
	int i;

	dprint(FD_VERIFY, "verify_state_should_stop numberio=%"PRIu64"\n", numberio);
	if (!s)
		return 0;

	/* If the current seq is lower than the max issued seq, check to make sure
	 * the write was not inflight.
	 */
	if (numberio < s->numberio) {
		for (i = 0; i < s->depth; i++) {
			if (s->inflight[i].numberio == numberio) {
				log_info("Stop verify because seq %"PRIu64" was an inflight write\n",
					numberio);
				return 1;
			}
		}
	} else {
		log_info("Stop verify because seq %"PRIu64" >= %"PRIu64"\n",
			numberio, s->numberio);
		return 1;
	}

	return 0;
}
