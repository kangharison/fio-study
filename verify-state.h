/*
 * [한국어] verify-state.h - 데이터 검증 상태 저장/복원 구조체
 *
 * fio의 verify 기능에서 I/O 상태를 저장하고 복원하기 위한 구조체를 정의한다.
 * 주요 용도:
 *   - 쓰기 작업의 상태(난수 생성기 상태, I/O 번호 등)를 파일로 저장
 *   - 이후 검증(verify) 작업에서 상태를 복원하여 동일한 순서로 검증 수행
 *   - verify_state_save/verify_load_state로 상태 직렬화/역직렬화
 *
 * 구조체:
 *   - thread_rand_state : 난수 생성기 상태 (32비트 또는 64비트)
 *   - thread_io_list    : 스레드별 I/O 상태 (깊이, 진행중 쓰기 등)
 *   - all_io_list       : 모든 스레드의 I/O 상태를 묶는 컨테이너
 *   - verify_state_hdr  : 검증 상태 파일의 헤더 (버전, 크기, CRC)
 */
#ifndef FIO_VERIFY_STATE_H
#define FIO_VERIFY_STATE_H

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "lib/nowarn_snprintf.h"

/* [한국어] 32비트 난수 생성기 상태 (4개의 32비트 상태값) */
struct thread_rand32_state {
	uint32_t s[4];
};

/* [한국어] 64비트 난수 생성기 상태 (6개의 64비트 상태값) */
struct thread_rand64_state {
	uint64_t s[6];
};

/* [한국어] 난수 생성기 상태를 통합하는 구조체 (32비트/64비트 선택) */
struct thread_rand_state {
	uint64_t use64;  /* 64비트 난수 생성기 사용 여부 */
	union {
		struct thread_rand32_state state32; /* 32비트 상태 */
		struct thread_rand64_state state64; /* 64비트 상태 */
	};
};

/* a single inflight write */
/* [한국어] 진행 중인 단일 쓰기 I/O 정보 */
struct inflight_write {
	uint64_t numberio;  /* I/O 일련번호 */
};

/* [한국어] 스레드별 I/O 상태 목록 - 검증 상태 저장의 핵심 구조체 */
struct thread_io_list {
	uint32_t depth; /* I/O depth of the job that saves the verify state */ /* I/O 큐 깊이 */
	uint64_t numberio; /* Number of issued writes */ /* 발행된 쓰기 I/O 총 수 */
	uint64_t index;       /* 현재 인덱스 */
	struct thread_rand_state rand; /* 난수 생성기 상태 */
	uint8_t name[64];     /* 작업(job) 이름 */
	struct inflight_write inflight[0]; /* 진행 중인 쓰기 배열 (가변 길이) */
};

/* [한국어] 모든 스레드의 I/O 상태를 담는 컨테이너 */
struct all_io_list {
	uint64_t threads;  /* 스레드 수 */
	struct thread_io_list state[0]; /* 스레드별 상태 배열 (가변 길이) */
};

/* [한국어] 검증 상태 파일 헤더 버전 */
#define VSTATE_HDR_VERSION	0x05

/* [한국어] 검증 상태 파일의 헤더 - 버전, 크기, CRC로 무결성 검증 */
struct verify_state_hdr {
	uint64_t version;  /* 헤더 버전 (VSTATE_HDR_VERSION) */
	uint64_t size;     /* 데이터 크기 */
	uint64_t crc;      /* CRC 체크섬 */
};

/* [한국어] 모든 스레드의 상태를 저장할 때 사용하는 마스크 */
#define IO_LIST_ALL		0xffffffff

struct io_u;
/* [한국어] 모든 스레드의 I/O 상태를 수집하여 반환 */
extern struct all_io_list *get_all_io_list(int, size_t *);
/* [한국어] 수집된 상태를 지정된 접두사 경로에 저장 */
extern void __verify_save_state(struct all_io_list *, const char *);
/* [한국어] 마스크에 해당하는 스레드의 검증 상태를 저장 */
extern void verify_save_state(int mask);
/* [한국어] 파일에서 검증 상태를 로드 */
extern int verify_load_state(struct thread_data *, const char *);
/* [한국어] 검증 상태 메모리 해제 */
extern void verify_free_state(struct thread_data *);
/* [한국어] 주어진 I/O 번호에서 검증을 중단해야 하는지 확인 */
extern int verify_state_should_stop(struct thread_data *, uint64_t);
/* [한국어] 외부 상태를 스레드에 할당 */
extern void verify_assign_state(struct thread_data *, void *);
/* [한국어] 검증 상태 헤더의 유효성 검증 (CRC 확인) */
extern int verify_state_hdr(struct verify_state_hdr *, struct thread_io_list *);

/* [한국어] 주어진 depth에 대한 thread_io_list의 전체 크기 계산 */
static inline size_t __thread_io_list_sz(uint32_t depth)
{
	return sizeof(struct thread_io_list) + depth * sizeof(struct inflight_write);
}

/* [한국어] 기존 thread_io_list 구조체의 전체 크기 계산 (리틀 엔디안 변환 포함) */
static inline size_t thread_io_list_sz(struct thread_io_list *s)
{
	return __thread_io_list_sz(le32_to_cpu(s->depth));
}

/* [한국어] 가변 길이 배열의 다음 thread_io_list 항목으로 이동 */
static inline struct thread_io_list *io_list_next(struct thread_io_list *s)
{
	return (struct thread_io_list *)((char *) s + thread_io_list_sz(s));
}

/*
 * [한국어] 검증 상태 파일명 생성
 *
 * 형식: "{prefix}-{name}-{num}-verify.state"
 * name에 포함된 '/'는 '.'으로 이스케이프 처리
 */
static inline void verify_state_gen_name(char *out, size_t size,
					 const char *name, const char *prefix,
					 int num)
{
	char ename[PATH_MAX];
	char *ptr;

	/*
	 * Escape '/', just turn them into '.'
	 */
	ptr = ename;
	do {
		*ptr = *name;
		if (*ptr == '\0')
			break;
		else if (*ptr == '/')
			*ptr = '.';
		ptr++;
		name++;
	} while (1);

	nowarn_snprintf(out, size, "%s-%s-%d-verify.state", prefix, ename, num);
	out[size - 1] = '\0';
}

/* [한국어] 유효하지 않은 I/O 번호를 나타내는 센티널 값 */
#define INVALID_NUMBERIO UINT64_MAX

#endif
