#ifndef FIO_PROFILE_H
#define FIO_PROFILE_H
/*
 * [한국어] profile.h - 프로파일 시스템 헤더
 *
 * 사전 정의된 워크로드 프로파일을 위한 구조체 및 함수를 선언한다.
 * 프로파일은 특정 벤치마크 시나리오(예: tiobench, act)를
 * 재현하기 위한 옵션/후크 집합이다.
 
 * === 파일의 역할 ===
 * 워크로드 프로파일을 위한 구조체 및 함수를 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * profile.c와 짝을 이루는 헤더. profiles/*.c에서 프로파일 정의 시 사용.
 *
 * === 타 모듈과의 연결 ===
 * - profile.c: 이 헤더의 함수 구현
 * - profiles/*.c: 프로파일 정의
 *
 * === 주요 함수/구조체 요약 ===
 * - struct profile_ops: 프로파일 콜백 (prep/init/io_ops)
 * - register_profile()/load_profile(): 등록/로드 API
 */

#include "flist.h"

/*
 * Functions for overriding internal fio io_u functions
 */
/* [한국어] 프로파일별 I/O 후크 함수 포인터 구조체
 * fio의 내부 io_u 처리를 프로파일이 오버라이드할 수 있게 한다. */
struct prof_io_ops {
	int (*td_init)(struct thread_data *);   /* 스레드 초기화 콜백 */
	void (*td_exit)(struct thread_data *);  /* 스레드 종료 콜백 */

	int (*io_u_lat)(struct thread_data *, uint64_t);  /* I/O 지연시간 콜백 */
};

/* [한국어] 프로파일 정의 구조체 - 하나의 워크로드 프로파일을 나타냄 */
struct profile_ops {
	struct flist_head list;   /* 전역 프로파일 리스트 연결 노드 */
	char name[32];            /* 프로파일 이름 (예: "tiobench") */
	char desc[64];            /* 프로파일 설명 */
	int flags;                /* 프로파일 플래그 */

	/*
	 * Profile specific options
	 */
	struct fio_option *options;  /* 프로파일 고유 옵션 배열 */
	void *opt_data;              /* 옵션 데이터 저장소 */

	/*
	 * Called after parsing options, to prepare 'cmdline'
	 */
	int (*prep_cmd)(void);       /* 명령줄 준비 콜백 */

	/*
	 * The complete command line
	 */
	const char **cmdline;        /* 프로파일이 생성하는 명령줄 옵션 배열 */

	struct prof_io_ops *io_ops;  /* I/O 후크 함수 포인터 */
};

int register_profile(struct profile_ops *);
void unregister_profile(struct profile_ops *);
int load_profile(const char *);
struct profile_ops *find_profile(const char *);
void profile_add_hooks(struct thread_data *);

int profile_td_init(struct thread_data *);
void profile_td_exit(struct thread_data *);

#endif
