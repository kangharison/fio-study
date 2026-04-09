/*
 * This file contains job initialization and setup functions.
 */
/* 이 파일은 fio의 job 초기화 및 설정 함수들을 포함합니다.
 * fio의 전체 초기화 흐름:
 *   1. parse_options() → 명령줄 인수 파싱 및 옵션 초기화
 *   2. parse_cmd_line() → CLI 옵션 파싱 (--name, --ioengine 등)
 *   3. parse_jobs_ini() → job 파일(.fio) 파싱 ([global], [jobname] 섹션)
 *   4. get_new_job() → thread_data 구조체 할당 (공유 메모리 세그먼트에서)
 *   5. ioengine_load() → IO 엔진 동적 로드 (libaio, io_uring 등)
 *   6. fixup_options() → 옵션 간 의존성 및 충돌 해결
 *   7. add_job() → 최종 job 등록 (로그 설정, 난수 시드, rate 설정 등)
 */

/* 표준 C 라이브러리 헤더 파일들 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/ipc.h>      /* IPC(프로세스 간 통신) 관련 - 공유 메모리 키 생성에 사용 */
#include <sys/types.h>
#include <dlfcn.h>         /* 동적 라이브러리 로딩 (dlopen, dlclose) - IO 엔진 플러그인 로드에 사용 */
#ifdef CONFIG_VALGRIND_DEV
#include <valgrind/drd.h>  /* Valgrind DRD(Data Race Detector) - 스레드 경쟁 상태 감지 도구 */
#else
/* Valgrind가 없으면 DRD_IGNORE_VAR를 빈 매크로로 정의 */
#define DRD_IGNORE_VAR(x) do { } while (0)
#endif

#include "fio.h"           /* fio 핵심 헤더 - thread_data, fio_file 등 주요 구조체 정의 */
#ifndef FIO_NO_HAVE_SHM_H
#include <sys/shm.h>       /* POSIX 공유 메모리 API (shmget, shmat, shmdt, shmctl) */
#endif

#include "parse.h"         /* 옵션 파싱 관련 함수들 */
#include "smalloc.h"       /* fio 전용 소규모 메모리 할당기 (공유 메모리 기반) */
#include "filehash.h"      /* 파일 해시 테이블 - 중복 파일 감지에 사용 */
#include "verify.h"        /* 데이터 검증(verify) 관련 함수 및 구조체 */
#include "profile.h"       /* 프로파일링 관련 함수들 */
#include "server.h"        /* 클라이언트/서버 모드 관련 함수들 */
#include "idletime.h"      /* CPU 유휴 시간 측정 관련 */
#include "filelock.h"      /* 파일 잠금 관련 함수들 */
#include "steadystate.h"   /* 정상 상태(steady state) 감지 관련 */
#include "blktrace.h"      /* blktrace 로그 병합/재생 관련 */

#include "oslib/asprintf.h"  /* OS 독립적 asprintf 구현 */
#include "oslib/getopt.h"    /* OS 독립적 getopt_long 구현 */
#include "oslib/strcasestr.h" /* OS 독립적 strcasestr(대소문자 무시 문자열 검색) */

#include "crc/test.h"      /* CRC/체크섬 성능 테스트 */
#include "lib/pow2.h"      /* 2의 거듭제곱 관련 유틸리티 */
#include "lib/memcpy.h"    /* memcpy 성능 테스트 */

/* fio 버전 문자열 - FIO_VERSION 매크로에서 설정됨 */
const char fio_version_string[] = FIO_VERSION;

/* 난수 생성기의 기본 시드값 */
#define FIO_RANDSEED		(0xb1899bedUL)

/* ini 파일(job 파일) 경로 배열 - 명령줄에서 지정된 job 파일들의 목록 */
static char **ini_file;
/* --showcmd 옵션: job 파일 내용을 명령줄 형식으로 출력만 하고 실행하지 않음 */
static bool dump_cmdline;
/* --parse-only 옵션: 옵션을 파싱만 하고 IO를 실행하지 않음 */
static bool parse_only;
/* --merge-blktrace-only 옵션: blktrace 로그만 병합하고 IO를 실행하지 않음 */
static bool merge_blktrace_only;

/* 기본 스레드 데이터 - [global] 섹션의 옵션을 저장하며, 새 job의 템플릿으로 사용됨 */
static struct thread_data def_thread;
/* 스레드 세그먼트 배열 - 공유 메모리 세그먼트별로 thread_data 배열을 관리
 * 각 세그먼트는 JOBS_PER_SEG개의 job을 저장할 수 있음 */
struct thread_segment segments[REAL_MAX_SEG];
/* --section 옵션으로 지정된 job 섹션 이름 배열 */
static char **job_sections;
/* job_sections 배열의 요소 수 */
static int nr_job_sections;

/* 하나의 job이 에러로 종료되면 모든 job을 종료할지 여부 */
bool exitall_on_terminate = false;
/* 출력 형식: normal, terse(간결), json, json+ 중 선택 */
int output_format = FIO_OUTPUT_NORMAL;
/* ETA(예상 완료 시간) 출력 모드: auto, always, never */
int eta_print = FIO_ETA_AUTO;
/* ETA 갱신 간격 (밀리초 단위, 기본값 1000ms = 1초) */
unsigned int eta_interval_msec = 1000;
/* ETA 새 줄 출력 간격 (초 단위) */
int eta_new_line = 0;
/* 표준 출력 파일 포인터 (--output 옵션으로 리다이렉트 가능) */
FILE *f_out = NULL;
/* 표준 에러 파일 포인터 */
FILE *f_err = NULL;
/* --profile 옵션으로 지정된 실행 프로파일 이름 */
char *exec_profile = NULL;
/* --warnings-fatal: 경고를 치명적 오류로 처리할지 여부 */
int warnings_fatal = 0;
/* terse 출력 형식의 버전 (2~5, 기본값 3) */
int terse_version = 3;
/* 현재 프로세스가 백엔드(서버) 모드인지 여부 */
bool is_backend = false;
/* 로컬 백엔드 모드인지 여부 */
bool is_local_backend = false;
/* 연결된 원격 클라이언트 수 */
int nr_clients = 0;
/* syslog로 로그를 출력할지 여부 */
bool log_syslog = false;

/* 대역폭(bandwidth) 로그 기록 활성화 여부 */
bool write_bw_log = false;
/* 대역폭 로그 파일 이름 */
const char *write_bw_log_name;
/* --readonly: 읽기 전용 모드 - 쓰기/트림 작업 금지 */
bool read_only = false;
/* --status-interval: 상태 전체 덤프 출력 간격 */
int status_interval = 0;

/* 트리거 파일 경로 - 이 파일이 존재하면 트리거 명령 실행 */
char *trigger_file = NULL;
/* 트리거 타임아웃 (이 시간 후 트리거 명령 실행) */
long long trigger_timeout = 0;
/* 로컬에서 실행할 트리거 명령 */
char *trigger_cmd = NULL;
/* 원격에서 실행할 트리거 명령 */
char *trigger_remote_cmd = NULL;

/* fio 상태 파일들의 보조 경로 (로그 파일 등) */
char *aux_path = NULL;

/* 이전 그룹의 job 수 - stonewall/new_group 처리에 사용 */
static int prev_group_jobs;

/* 디버그 출력 비트마스크 - 각 비트가 특정 디버그 카테고리를 나타냄 */
unsigned long fio_debug = 0;
/* 특정 job 번호에 대해서만 디버그 출력 */
unsigned int fio_debug_jobno = -1;
/* 디버그 대상 job 번호 포인터 (공유 메모리에 위치) */
unsigned int *fio_debug_jobp = NULL;
/* 경고 플래그 포인터 (공유 메모리에 위치) - 중복 경고 방지용 */
unsigned int *fio_warned = NULL;

/* 명령줄 옵션 문자열 - getopt용 */
static char cmd_optstr[256];
/* 유효한 명령줄 인수가 하나라도 처리되었는지 여부 */
static bool did_arg;

/* 클라이언트 전용 플래그 - 클라이언트/서버 모드에서 클라이언트에게 전달해야 할 옵션 표시 */
#define FIO_CLIENT_FLAG		(1 << 16)

/*
 * Command line options. These will contain the above, plus a few
 * extra that only pertain to fio itself and not jobs.
 */
/* 명령줄 옵션 정의 배열. fio 자체의 옵션들(job 옵션이 아닌)을 정의합니다.
 * getopt_long_only()에서 사용되며, FIO_CLIENT_FLAG가 설정된 옵션은
 * 클라이언트/서버 모드에서 클라이언트에게도 전달됩니다. */
static struct option l_opts[FIO_NR_OPTIONS] = {
	{
		.name		= (char *) "output",       /* 출력을 파일로 리다이렉트 */
		.has_arg	= required_argument,
		.val		= 'o' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "latency-log",  /* 레이턴시 로그 (더 이상 사용되지 않음) */
		.has_arg	= required_argument,
		.val		= 'l' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "bandwidth-log", /* 대역폭 로그 생성 */
		.has_arg	= optional_argument,
		.val		= 'b' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "minimal",      /* 최소(간결) 출력 모드 */
		.has_arg	= no_argument,
		.val		= 'm' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "output-format", /* 출력 형식 지정 (terse/json/json+/normal) */
		.has_arg	= required_argument,
		.val		= 'F' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "append-terse",  /* terse 출력 추가 (--output-format=terse와 동일) */
		.has_arg	= optional_argument,
		.val		= 'f',
	},
	{
		.name		= (char *) "version",      /* 버전 정보 출력 */
		.has_arg	= no_argument,
		.val		= 'v' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "help",         /* 도움말 출력 */
		.has_arg	= no_argument,
		.val		= 'h' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "cmdhelp",      /* 특정 명령어 도움말 출력 */
		.has_arg	= optional_argument,
		.val		= 'c' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "enghelp",      /* IO 엔진 도움말 출력 */
		.has_arg	= optional_argument,
		.val		= 'i' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "showcmd",      /* job 파일을 명령줄 옵션으로 변환하여 출력 */
		.has_arg	= no_argument,
		.val		= 's' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "readonly",     /* 읽기 전용 안전 검사 활성화 */
		.has_arg	= no_argument,
		.val		= 'r' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "eta",          /* ETA 출력 시점 (always/never/auto) */
		.has_arg	= required_argument,
		.val		= 'e' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "eta-interval", /* ETA 갱신 간격 */
		.has_arg	= required_argument,
		.val		= 'O' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "eta-newline",  /* ETA 줄바꿈 간격 */
		.has_arg	= required_argument,
		.val		= 'E' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "debug",        /* 디버그 로깅 활성화 */
		.has_arg	= required_argument,
		.val		= 'd' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "parse-only",   /* 파싱만 수행 (IO 실행 안 함) */
		.has_arg	= no_argument,
		.val		= 'P' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "section",      /* 특정 섹션만 실행 */
		.has_arg	= required_argument,
		.val		= 'x' | FIO_CLIENT_FLAG,
	},
#ifdef CONFIG_ZLIB
	{
		.name		= (char *) "inflate-log",  /* 압축된 로그 파일 해제 및 출력 */
		.has_arg	= required_argument,
		.val		= 'X' | FIO_CLIENT_FLAG,
	},
#endif
	{
		.name		= (char *) "alloc-size",   /* smalloc 풀 크기 설정 (KB 단위) */
		.has_arg	= required_argument,
		.val		= 'a' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "profile",      /* 실행 프로파일 지정 */
		.has_arg	= required_argument,
		.val		= 'p' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "warnings-fatal", /* 경고를 치명적 오류로 처리 */
		.has_arg	= no_argument,
		.val		= 'w' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "max-jobs",     /* 최대 스레드/프로세스 수 (현재 무시됨) */
		.has_arg	= required_argument,
		.val		= 'j' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "terse-version", /* terse 출력 버전 (2~5) */
		.has_arg	= required_argument,
		.val		= 'V' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "server",       /* 백엔드 서버 모드로 시작 */
		.has_arg	= optional_argument,
		.val		= 'S',
	},
#ifdef WIN32
	{
		.name		= (char *) "server-internal", /* Windows 전용 내부 서버 설정 */
		.has_arg	= required_argument,
		.val		= 'N',
	},
#endif
	{	.name		= (char *) "daemonize",    /* 백그라운드 데몬으로 실행, PID를 파일에 기록 */
		.has_arg	= required_argument,
		.val		= 'D',
	},
	{
		.name		= (char *) "client",       /* 원격 백엔드 서버에 연결 */
		.has_arg	= required_argument,
		.val		= 'C',
	},
	{
		.name		= (char *) "remote-config", /* 서버에 로컬 job 파일 전송 */
		.has_arg	= required_argument,
		.val		= 'R',
	},
	{
		.name		= (char *) "cpuclock-test", /* CPU 클록 검증 테스트 */
		.has_arg	= no_argument,
		.val		= 'T',
	},
	{
		.name		= (char *) "crctest",      /* 체크섬 함수 속도 테스트 */
		.has_arg	= optional_argument,
		.val		= 'G',
	},
	{
		.name		= (char *) "memcpytest",   /* memcpy 속도 테스트 */
		.has_arg	= optional_argument,
		.val		= 'M',
	},
	{
		.name		= (char *) "idle-prof",    /* CPU 유휴 프로파일링 */
		.has_arg	= required_argument,
		.val		= 'I',
	},
	{
		.name		= (char *) "status-interval", /* 상태 덤프 출력 간격 */
		.has_arg	= required_argument,
		.val		= 'L' | FIO_CLIENT_FLAG,
	},
	{
		.name		= (char *) "trigger-file", /* 트리거 파일 경로 */
		.has_arg	= required_argument,
		.val		= 'W',
	},
	{
		.name		= (char *) "trigger-timeout", /* 트리거 타임아웃 */
		.has_arg	= required_argument,
		.val		= 'B',
	},
	{
		.name		= (char *) "trigger",      /* 로컬 트리거 명령 */
		.has_arg	= required_argument,
		.val		= 'H',
	},
	{
		.name		= (char *) "trigger-remote", /* 원격 트리거 명령 */
		.has_arg	= required_argument,
		.val		= 'J',
	},
	{
		.name		= (char *) "aux-path",     /* 보조 파일 경로 */
		.has_arg	= required_argument,
		.val		= 'K',
	},
	{
		.name		= (char *) "merge-blktrace-only", /* blktrace 병합만 수행 */
		.has_arg	= no_argument,
		.val		= 'A' | FIO_CLIENT_FLAG,
	},
	{
		.name		= NULL,  /* 옵션 배열 종료 표시 */
	},
};

/*
 * free_threads_shm() - 스레드 공유 메모리 해제 함수
 *
 * 역할: 모든 세그먼트의 thread_data 공유 메모리를 해제합니다.
 * 파라미터: 없음
 * 반환값: 없음
 *
 * 각 세그먼트에 대해:
 *   - CONFIG_NO_SHM이 아닌 경우: shmdt()로 분리 후 shmctl()로 삭제
 *   - CONFIG_NO_SHM인 경우: free()로 단순 해제
 */
void free_threads_shm(void)
{
	int i;

	/* 모든 세그먼트를 순회하며 공유 메모리 해제 */
	for (i = 0; i < nr_segments; i++) {
		struct thread_segment *seg = &segments[i];

		/* 스레드 배열이 할당되어 있는 경우에만 해제 */
		if (seg->threads) {
			void *tp = seg->threads;
#ifndef CONFIG_NO_SHM
			/* POSIX 공유 메모리 사용 시: shmdt로 분리하고 shmctl로 삭제 */
			struct shmid_ds sbuf;

			seg->threads = NULL;
			shmdt(tp);                        /* 공유 메모리 분리 (detach) */
			shmctl(seg->shm_id, IPC_RMID, &sbuf); /* 공유 메모리 세그먼트 삭제 */
			seg->shm_id = -1;
#else
			/* 공유 메모리를 사용하지 않는 경우: 일반 힙 메모리 해제 */
			seg->threads = NULL;
			free(tp);
#endif
		}
	}

	/* 세그먼트 카운터 초기화 */
	nr_segments = 0;
	cur_segment = 0;
}

/*
 * free_shm() - 전체 공유 자원 정리 함수
 *
 * 역할: fio 종료 시 atexit()에 의해 호출되어 모든 공유 자원을 정리합니다.
 * 파라미터: 없음
 * 반환값: 없음
 *
 * 정리 순서:
 *   1. flow 제어 종료
 *   2. 스레드 공유 메모리 해제
 *   3. 트리거 관련 메모리 해제
 *   4. 기본 스레드 옵션 해제
 *   5. 파일 잠금 및 해시 종료
 *   6. smalloc 정리
 */
static void free_shm(void)
{
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	/* 퍼징 빌드에서는 이 정리 과정을 건너뜀 (퍼징 시 안전하지 않은 작업 방지) */
	if (nr_segments) {
		flow_exit();               /* flow 제어 시스템 종료 */
		fio_debug_jobp = NULL;     /* 디버그 job 포인터 초기화 */
		fio_warned = NULL;         /* 경고 플래그 포인터 초기화 */
		free_threads_shm();        /* 스레드 공유 메모리 해제 */
	}

	/* 트리거 관련 동적 할당 메모리 해제 */
	free(trigger_file);
	free(trigger_cmd);
	free(trigger_remote_cmd);
	trigger_file = trigger_cmd = trigger_remote_cmd = NULL;

	/* 기본 스레드의 옵션 메모리 해제 */
	options_free(fio_options, &def_thread.o);
	/* 파일 잠금 시스템 종료 */
	fio_filelock_exit();
	/* 파일 해시 테이블 종료 */
	file_hash_exit();
	/* smalloc(소규모 메모리 할당기) 정리 */
	scleanup();
#endif
}

/*
 * add_thread_segment() - 새로운 스레드 세그먼트 추가 함수
 *
 * 역할: thread_data를 저장할 새로운 공유 메모리 세그먼트를 할당합니다.
 *       각 세그먼트는 JOBS_PER_SEG개의 job을 저장할 수 있습니다.
 * 파라미터: 없음
 * 반환값: 성공 시 0, 실패 시 -1 또는 1
 *
 * 첫 번째 세그먼트를 생성할 때는 추가로:
 *   - fio_debug_jobp (디버그 job 번호 포인터) 설정
 *   - fio_warned (경고 플래그 포인터) 설정
 *   - flow_init() (flow 제어 초기화) 호출
 */
static int add_thread_segment(void)
{
	struct thread_segment *seg = &segments[nr_segments];
	/* 세그먼트 크기 = (세그먼트당 job 수) * (thread_data 구조체 크기) */
	size_t size = JOBS_PER_SEG * sizeof(struct thread_data);
	int i;

	/* 최대 세그먼트 수 초과 확인 */
	if (nr_segments + 1 >= REAL_MAX_SEG) {
		log_err("error: maximum number of jobs reached.\n");
		return -1;
	}

	/* 디버그 포인터 2개를 위한 추가 공간 (fio_debug_jobp, fio_warned) */
	size += 2 * sizeof(unsigned int);

#ifndef CONFIG_NO_SHM
	/* POSIX 공유 메모리 할당: shmget으로 세그먼트 생성 */
	seg->shm_id = shmget(0, size, IPC_CREAT | 0600);
	if (seg->shm_id == -1) {
		if (errno != EINVAL && errno != ENOMEM && errno != ENOSPC)
			perror("shmget");
		return -1;
	}
#else
	/* 공유 메모리를 사용하지 않는 경우: malloc으로 일반 힙 메모리 할당 */
	seg->threads = malloc(size);
	if (!seg->threads)
		return -1;
#endif

#ifndef CONFIG_NO_SHM
	/* shmat으로 공유 메모리를 현재 프로세스 주소 공간에 연결 */
	seg->threads = shmat(seg->shm_id, NULL, 0);
	if (seg->threads == (void *) -1) {
		perror("shmat");
		return 1;
	}
	/* 일부 시스템에서는 attach 직후 IPC_RMID를 호출하여
	 * 마지막 프로세스가 분리될 때 자동으로 삭제되도록 함 */
	if (shm_attach_to_open_removed())
		shmctl(seg->shm_id, IPC_RMID, NULL);
#endif

	/* 세그먼트 수 증가 */
	nr_segments++;

	/* 할당된 메모리를 0으로 초기화 */
	memset(seg->threads, 0, JOBS_PER_SEG * sizeof(struct thread_data));
	/* Valgrind DRD에게 각 thread_data를 무시하도록 지시 (오탐 방지) */
	for (i = 0; i < JOBS_PER_SEG; i++)
		DRD_IGNORE_VAR(seg->threads[i]);
	seg->nr_threads = 0;

	/* Not first segment, we're done */
	/* 첫 번째 세그먼트가 아니면 여기서 완료 */
	if (nr_segments != 1) {
		cur_segment++;
		return 0;
	}

	/* 첫 번째 세그먼트에만 디버그 포인터를 세그먼트 끝에 배치 */
	fio_debug_jobp = (unsigned int *)(seg->threads + JOBS_PER_SEG);
	*fio_debug_jobp = -1;   /* -1은 모든 job에 대해 디버그 출력 */
	fio_warned = fio_debug_jobp + 1;
	*fio_warned = 0;         /* 경고 플래그 초기화 */

	/* flow 제어 시스템 초기화 (job 간 IO 속도 조절에 사용) */
	flow_init();
	return 0;
}

/*
 * The thread areas are shared between the main process and the job
 * threads/processes, and is split into chunks of JOBS_PER_SEG. If the current
 * segment has no more room, add a new chunk.
 */
/*
 * expand_thread_area() - 스레드 영역 확장 함수
 *
 * 역할: 현재 세그먼트에 여유 공간이 없으면 새 세그먼트를 추가합니다.
 *       스레드 영역은 메인 프로세스와 job 스레드/프로세스 간에 공유되며,
 *       JOBS_PER_SEG 크기의 청크로 분할됩니다.
 * 파라미터: 없음
 * 반환값: 성공 시 0, 실패 시 add_thread_segment()의 반환값
 */
static int expand_thread_area(void)
{
	struct thread_segment *seg = &segments[cur_segment];

	/* 세그먼트가 존재하고 현재 세그먼트에 여유가 있으면 확장 불필요 */
	if (nr_segments && seg->nr_threads < JOBS_PER_SEG)
		return 0;

	/* 새 세그먼트 추가 */
	return add_thread_segment();
}

/*
 * dump_print_option() - 단일 출력 옵션을 로그에 출력하는 함수
 *
 * 역할: --showcmd 모드에서 옵션을 명령줄 형식으로 출력합니다.
 * 파라미터:
 *   p - 출력할 print_option 구조체
 * 반환값: 없음
 */
static void dump_print_option(struct print_option *p)
{
	const char *delim;

	/* description 옵션은 값에 큰따옴표를 붙여서 출력 */
	if (!strcmp("description", p->name))
		delim = "\"";
	else
		delim = "";

	/* --옵션이름=값 형식으로 출력 */
	log_info("--%s%s", p->name, p->value ? "" : " ");
	if (p->value)
		log_info("=%s%s%s ", delim, p->value, delim);
}

/*
 * dump_opt_list() - thread_data의 옵션 목록을 모두 출력하는 함수
 *
 * 역할: --showcmd 모드에서 job의 모든 옵션을 출력합니다.
 * 파라미터:
 *   td - 옵션 목록을 가진 thread_data
 * 반환값: 없음
 */
static void dump_opt_list(struct thread_data *td)
{
	struct flist_head *entry;
	struct print_option *p;

	/* 옵션 목록이 비어있으면 아무것도 하지 않음 */
	if (flist_empty(&td->opt_list))
		return;

	/* 연결 리스트를 순회하며 각 옵션 출력 */
	flist_for_each(entry, &td->opt_list) {
		p = flist_entry(entry, struct print_option, list);
		dump_print_option(p);
	}
}

/*
 * copy_opt_list() - 옵션 목록을 복사하는 함수
 *
 * 역할: 부모 thread_data의 옵션 목록을 자식에게 깊은 복사(deep copy)합니다.
 * 파라미터:
 *   dst - 대상 thread_data (복사 받을 곳)
 *   src - 원본 thread_data (복사할 곳)
 * 반환값: 없음
 */
static void copy_opt_list(struct thread_data *dst, struct thread_data *src)
{
	struct flist_head *entry;

	/* 원본 옵션 목록이 비어있으면 아무것도 하지 않음 */
	if (flist_empty(&src->opt_list))
		return;

	/* 원본의 각 옵션을 순회하며 깊은 복사 수행 */
	flist_for_each(entry, &src->opt_list) {
		struct print_option *srcp, *dstp;

		srcp = flist_entry(entry, struct print_option, list);
		/* 새 옵션 구조체 할당 */
		dstp = malloc(sizeof(*dstp));
		dstp->name = strdup(srcp->name);  /* 이름 문자열 복제 */
		if (srcp->value)
			dstp->value = strdup(srcp->value);  /* 값 문자열 복제 */
		else
			dstp->value = NULL;
		/* 대상의 옵션 목록 끝에 추가 */
		flist_add_tail(&dstp->list, &dst->opt_list);
	}
}

/*
 * Return a free job structure.
 */
/*
 * get_new_job() - 새로운 job 구조체(thread_data) 할당 함수
 *
 * 역할: 새로운 job을 위한 thread_data를 할당하고 부모로부터 초기값을 복사합니다.
 *       global=true이면 def_thread(글로벌 기본 설정)를 반환합니다.
 *
 * 파라미터:
 *   global       - true이면 글로벌 thread_data(def_thread) 반환
 *   parent       - 부모 thread_data (옵션 기본값의 원본)
 *   preserve_eo  - true이면 IO 엔진 옵션(eo)을 유지
 *   jobname      - job 이름 문자열
 *
 * 반환값: 새로 할당된 thread_data 포인터, 실패 시 NULL
 *
 * 처리 과정:
 *   1. global이면 def_thread 반환
 *   2. expand_thread_area()로 공유 메모리 확보
 *   3. 부모의 thread_data를 복사
 *   4. IO 엔진 포인터 초기화
 *   5. 파일 목록 복제
 *   6. 프로파일 훅 추가
 */
static struct thread_data *get_new_job(bool global, struct thread_data *parent,
				       bool preserve_eo, const char *jobname)
{
	struct thread_segment *seg;
	struct thread_data *td;

	/* 글로벌 옵션 설정 시에는 def_thread를 그대로 반환 */
	if (global)
		return &def_thread;
	/* 스레드 영역을 확장하여 새 job을 위한 공간 확보 */
	if (expand_thread_area()) {
		log_err("error: failed to setup shm segment\n");
		return NULL;
	}

	/* 현재 세그먼트에서 다음 빈 슬롯을 가져옴 */
	seg = &segments[cur_segment];
	td = &seg->threads[seg->nr_threads++];
	thread_number++;  /* 전역 스레드 번호 증가 */
	/* 부모의 thread_data 내용을 그대로 복사 (얕은 복사) */
	*td = *parent;

	/* 옵션 목록을 새로 초기화하고, 부모가 def_thread가 아니면 옵션 목록 깊은 복사 */
	INIT_FLIST_HEAD(&td->opt_list);
	if (parent != &def_thread)
		copy_opt_list(td, parent);

	/* IO 엔진 관련 포인터 초기화 - 나중에 ioengine_load()에서 설정됨 */
	td->io_ops = NULL;
	td->io_ops_init = 0;
	/* preserve_eo가 false이면 엔진 옵션 포인터도 초기화 */
	if (!preserve_eo)
		td->eo = NULL;

	/* UID/GID를 -1로 설정 (설정되지 않음을 의미) */
	td->o.uid = td->o.gid = -1U;

	/* 파일 시스템 리스트 초기화 및 부모의 파일 목록 복제 */
	INIT_FLIST_HEAD(&td->fs_list);
	dup_files(td, parent);
	/* 옵션에 포함된 동적 할당 문자열들을 복제 (메모리 중복 방지) */
	fio_options_mem_dupe(td);

	/* 프로파일 훅 추가 (프로파일 기반 실행 시 사용) */
	profile_add_hooks(td);

	/* 고유한 스레드 번호 할당 */
	td->thread_number = thread_number;
	td->subjob_number = 0;  /* 서브잡 번호 초기화 (numjobs > 1일 때 사용) */

	/* job 이름 설정 */
	if (jobname)
		td->o.name = strdup(jobname);

	/* group_reporting이 아니거나 부모가 def_thread이면 통계 번호 증가 */
	if (!parent->o.group_reporting || parent == &def_thread)
		stat_number++;

	return td;
}

/*
 * put_job() - job 구조체 반환(해제) 함수
 *
 * 역할: 사용이 끝난 thread_data를 정리하고 해제합니다.
 *       에러가 있으면 에러 메시지를 출력합니다.
 * 파라미터:
 *   td - 해제할 thread_data
 * 반환값: 없음
 */
static void put_job(struct thread_data *td)
{
	/* def_thread는 해제하면 안 됨 (글로벌 기본 설정) */
	if (td == &def_thread)
		return;

	/* 프로파일 종료 처리 */
	profile_td_exit(td);
	/* flow 제어에서 이 job 제거 */
	flow_exit_job(td);

	/* 에러가 있으면 에러 메시지 출력 */
	if (td->error)
		log_info("fio: %s\n", td->verror);

	/* job 옵션 메모리 해제 */
	fio_options_free(td);
	/* 덤프 옵션 메모리 해제 */
	fio_dump_options_free(td);
	/* IO 엔진이 로드되어 있으면 해제 */
	if (td->io_ops)
		free_ioengine(td);

	/* job 이름 메모리 해제 */
	if (td->o.name)
		free(td->o.name);

	/* thread_data를 0으로 초기화하여 재사용 가능하게 함 */
	memset(td, 0, sizeof(*td));
	/* 세그먼트의 스레드 수와 전역 스레드 번호 감소 */
	segments[cur_segment].nr_threads--;
	thread_number--;
}

/*
 * __setup_rate() - 특정 방향(읽기/쓰기/트림)의 속도 제한 설정 함수
 *
 * 역할: 지정된 IO 방향에 대해 초당 바이트 속도를 계산하고 설정합니다.
 * 파라미터:
 *   td   - 대상 thread_data
 *   ddir - IO 방향 (DDIR_READ, DDIR_WRITE, DDIR_TRIM)
 * 반환값: 성공 시 0, 실패 시 -1
 */
static int __setup_rate(struct thread_data *td, enum fio_ddir ddir)
{
	/* 해당 방향의 최소 블록 크기 */
	unsigned long long bs = td->o.min_bs[ddir];

	/* 유효한 IO 방향인지 확인 */
	assert(ddir_rw(ddir));

	/* rate(바이트/초)가 설정되어 있으면 그대로 사용,
	 * 아니면 rate_iops * 블록크기로 바이트/초 계산 */
	if (td->o.rate[ddir])
		td->rate_bps[ddir] = td->o.rate[ddir];
	else
		td->rate_bps[ddir] = (uint64_t) td->o.rate_iops[ddir] * bs;

	/* 계산된 속도가 0이면 에러 */
	if (!td->rate_bps[ddir]) {
		log_err("rate lower than supported\n");
		return -1;
	}

	/* 속도 제한 관련 타이밍 변수 초기화 */
	td->rate_next_io_time[ddir] = 0;    /* 다음 IO 허용 시각 */
	td->rate_io_issue_bytes[ddir] = 0;  /* 발행된 IO 바이트 수 */
	td->last_usec[ddir] = 0;            /* 마지막 IO 시각 */
	return 0;
}

/*
 * setup_rate() - 모든 IO 방향의 속도 제한 설정 함수
 *
 * 역할: 읽기/쓰기/트림 각 방향에 대해 속도 제한이 설정되어 있으면
 *       __setup_rate()를 호출하여 초기화합니다.
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 성공 시 0, 실패 시 비트 OR된 에러 코드
 */
static int setup_rate(struct thread_data *td)
{
	int ret = 0;

	/* 모든 IO 방향(읽기/쓰기/트림)에 대해 순회 */
	for_each_rw_ddir(ddir) {
		/* rate 또는 rate_iops가 설정된 방향만 처리 */
		if (td->o.rate[ddir] || td->o.rate_iops[ddir]) {
			ret |= __setup_rate(td, ddir);
		}
	}
	return ret;
}

/*
 * fixed_block_size() - 블록 크기가 고정인지 확인하는 함수
 *
 * 역할: 읽기/쓰기/트림의 최소/최대 블록 크기가 모두 같은지 확인합니다.
 *       가변 블록 크기 vs 고정 블록 크기에 따라 verify 동작이 달라집니다.
 * 파라미터:
 *   o - thread_options 포인터
 * 반환값: 모든 블록 크기가 동일하면 1(true), 아니면 0(false)
 */
static int fixed_block_size(struct thread_options *o)
{
	return o->min_bs[DDIR_READ] == o->max_bs[DDIR_READ] &&
		o->min_bs[DDIR_WRITE] == o->max_bs[DDIR_WRITE] &&
		o->min_bs[DDIR_TRIM] == o->max_bs[DDIR_TRIM] &&
		o->min_bs[DDIR_READ] == o->min_bs[DDIR_WRITE] &&
		o->min_bs[DDIR_READ] == o->min_bs[DDIR_TRIM];
}

/*
 * <3 Johannes
 */
/* 유클리드 호제법을 이용한 최대공약수(GCD) 계산 - verify 간격 계산에 사용 */
static unsigned int gcd(unsigned int m, unsigned int n)
{
	if (!n)
		return m;

	return gcd(n, m % n);
}

/*
 * Lazy way of fixing up options that depend on each other. We could also
 * define option callback handlers, but this is easier.
 */
/*
 * fixup_options() - 옵션 간 의존성 및 충돌 해결 함수
 *
 * 역할: 상호 의존적인 옵션들을 검증하고 보정합니다.
 *       옵션 콜백 핸들러를 정의할 수도 있지만, 이 방식이 더 간단합니다.
 *
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 에러가 있으면 비트 OR된 에러 코드, 정상이면 0
 *
 * 주요 검증/보정 항목:
 *   - trim verify 설정 확인
 *   - readonly 모드에서 쓰기/트림 금지
 *   - zone 모드 관련 검증
 *   - verify 관련 옵션 정합성 확인
 *   - IO depth, batch 크기 보정
 *   - rate/rate_iops 상호 배타 확인
 *   - 압축, 난수 분포 관련 보정
 *   - steady state 관련 검증
 */
static int fixup_options(struct thread_data *td)
{
	struct thread_options *o = &td->o;
	int ret = 0;

	/*
	 * Denote whether we are verifying trims. Now we only have to check a
	 * single variable instead of having to check all three options.
	 */
	/* trim verify 여부를 단일 변수로 결정: verify, trim_backlog, trim_percentage 모두 설정 시 */
	td->trim_verify = o->verify && o->trim_backlog && o->trim_percentage;
	dprint(FD_VERIFY, "td->trim_verify=%d\n", td->trim_verify);

	/* readonly 모드에서 쓰기/트림/trim_verify 시도 시 에러 */
	if (read_only && (td_write(td) || td_trim(td) || td->trim_verify)) {
		log_err("fio: trim and write operations are not allowed"
			 " with the --readonly parameter.\n");
		ret |= 1;
	}

	/* trimwrite와 다중 범위(num_range > 1)는 호환되지 않음 */
	if (td_trimwrite(td) && o->num_range > 1) {
		log_err("fio: trimwrite cannot be used with multiple"
			" ranges.\n");
		ret |= 1;
	}

	/* 다중 범위 트림은 FIO_MULTI_RANGE_TRIM 플래그를 지원하는 엔진에서만 가능 */
	if (td_trim(td) && o->num_range > 1 &&
	    !td_ioengine_flagged(td, FIO_MULTI_RANGE_TRIM)) {
		log_err("fio: can't use multiple ranges with IO engine %s\n",
			td->io_ops->name);
		ret |= 1;
	}

#ifndef CONFIG_PSHARED
	/* 프로세스 공유 뮤텍스를 지원하지 않는 플랫폼에서는 스레드 모드 강제 사용 */
	if (!o->use_thread) {
		log_info("fio: this platform does not support process shared"
			 " mutexes, forcing use of threads. Use the 'thread'"
			 " option to get rid of this warning.\n");
		o->use_thread = 1;
		ret |= warnings_fatal;  /* --warnings-fatal이면 에러로 처리 */
	}
#endif

	/* write_iolog_file과 read_iolog_file이 동시에 설정되면 read가 우선 */
	if (o->write_iolog_file && o->read_iolog_file) {
		log_err("fio: read iolog overrides write_iolog\n");
		free(o->write_iolog_file);
		o->write_iolog_file = NULL;
		ret |= warnings_fatal;
	}

	/* zone_mode=none과 zone_size는 호환되지 않음 */
	if (o->zone_mode == ZONE_MODE_NONE && o->zone_size) {
		log_err("fio: --zonemode=none and --zonesize are not compatible.\n");
		ret |= 1;
	}

	/* zone_mode=zbd와 create_serialize=0은 호환되지 않음 */
	if (o->zone_mode == ZONE_MODE_ZBD && !o->create_serialize) {
		log_err("fio: --zonemode=zbd and --create_serialize=0 are not compatible.\n");
		ret |= 1;
	}

	/* zone_mode=zbd에서 write_zone_remainder=1이면 norandommap=1 필요 */
	if (o->zone_mode == ZONE_MODE_ZBD && o->write_zone_remainder) {
		if (fio_option_is_set(o, norandommap)) {
			if (o->norandommap == 0) {
				log_err("fio: write_zone_remainder=1 requires norandommap=1\n");
				ret |= 1;
			}
			/* norandommap == 1이면 OK */
		} else {
			/* 명시적으로 설정되지 않았으면 자동으로 norandommap=1 설정 */
			dprint(FD_ZBD, "fio: override norandommap=1 for write_zone_remainder=1\n");
			o->norandommap = 1;
		}
	}

	/* zone_mode=strided에서는 zone_size 필수 */
	if (o->zone_mode == ZONE_MODE_STRIDED && !o->zone_size) {
		log_err("fio: --zonesize must be specified when using --zonemode=strided.\n");
		ret |= 1;
	}

	/* zone_mode가 지정되지 않은 경우 자동 결정:
	 * zone_size가 있으면 strided, 없으면 none */
	if (o->zone_mode == ZONE_MODE_NOT_SPECIFIED) {
		if (o->zone_size)
			o->zone_mode = ZONE_MODE_STRIDED;
		else
			o->zone_mode = ZONE_MODE_NONE;
	}

	/*
	 * Strided zone mode only really works with 1 file.
	 */
	/* strided 존 모드는 파일이 1개일 때만 유효 */
	if (o->zone_mode == ZONE_MODE_STRIDED && o->open_files > 1)
		o->zone_mode = ZONE_MODE_NONE;

	/*
	 * If zone_range isn't specified, backward compatibility dictates it
	 * should be made equal to zone_size.
	 */
	/* zone_range가 지정되지 않으면 하위 호환성을 위해 zone_size와 동일하게 설정 */
	if (o->zone_mode == ZONE_MODE_STRIDED && !o->zone_range)
		o->zone_range = o->zone_size;

	/*
	 * SPRandom Requires: random write, random_generator=lfsr, norandommap=1
	 */
	/* SPRandom(Structured Pseudo-Random) 요구사항 검증:
	 * 랜덤 쓰기 + random_generator=lfsr + norandommap=1 필수 */
	if (o->sprandom) {
		if (td_write(td) && td_random(td)) {
			/* random_generator가 명시적으로 설정된 경우 lfsr인지 확인 */
			if (fio_option_is_set(o, random_generator)) {
				if (o->random_generator != FIO_RAND_GEN_LFSR) {
					log_err("fio: sprandom requires random_generator=lfsr\n");
					ret |= 1;
				}
			} else {
				/* 설정되지 않았으면 자동으로 lfsr 설정 */
				log_info("fio: sprandom sets random_generator=lfsr\n");
				o->random_generator = FIO_RAND_GEN_LFSR;
			}
			/* norandommap가 명시적으로 설정된 경우 1인지 확인 */
			if (fio_option_is_set(o, norandommap)) {
				if (o->norandommap == 0) {
					log_err("fio: sprandom requires norandommap=1\n");
					ret |= 1;
				}
				/* norandommap == 1이면 OK */
			} else {
				/* 설정되지 않았으면 자동으로 norandommap=1 설정 */
				log_info("fio: sprandom sets norandommap=1\n");
				o->norandommap = 1;
			}
		} else {
			log_err("fio: sprandom requires random write, random_generator=lfsr, norandommap=1\n");
			ret |= 1;
		}
	}

	/*
	 * Reads can do overwrites, we always need to pre-create the file
	 */
	/* 읽기 작업에서는 파일이 이미 존재해야 하므로 overwrite 플래그 설정 */
	if (td_read(td))
		o->overwrite = 1;

	/* 블록 크기 보정: min_bs와 max_bs가 설정되지 않았으면 bs값으로 설정 */
	for_each_rw_ddir(ddir) {
		if (!o->min_bs[ddir])
			o->min_bs[ddir] = o->bs[ddir];
		if (!o->max_bs[ddir])
			o->max_bs[ddir] = o->bs[ddir];
	}

	/* 모든 방향 중 최소 블록 크기 계산 (메모리 할당 등에 사용) */
	o->rw_min_bs = -1;
	for_each_rw_ddir(ddir) {
		o->rw_min_bs = min(o->rw_min_bs, o->min_bs[ddir]);
	}

	/*
	 * For random IO, allow blockalign offset other than min_bs.
	 */
	/* 블록 정렬(blockalign) 보정: 설정되지 않았거나 순차 IO이면 min_bs로 설정 */
	for_each_rw_ddir(ddir) {
		if (!o->ba[ddir] || !td_random(td))
			o->ba[ddir] = o->min_bs[ddir];
	}

	/* blockalign이 min_bs와 다르면 randommap 사용 불가 */
	if ((o->ba[DDIR_READ] != o->min_bs[DDIR_READ] ||
	    o->ba[DDIR_WRITE] != o->min_bs[DDIR_WRITE] ||
	    o->ba[DDIR_TRIM] != o->min_bs[DDIR_TRIM]) &&
	    !o->norandommap) {
		log_err("fio: Any use of blockalign= turns off randommap\n");
		o->norandommap = 1;
		ret |= warnings_fatal;
	}

	/* file_size_high가 설정되지 않으면 file_size_low와 동일하게 설정 */
	if (!o->file_size_high)
		o->file_size_high = o->file_size_low;

	/* start_delay_high가 설정된 경우, start_delay를 범위 내 랜덤 값으로 설정 */
	if (o->start_delay_high) {
		if (!o->start_delay_orig)
			o->start_delay_orig = o->start_delay;
		o->start_delay = rand_between(&td->delay_state,
						o->start_delay_orig,
						o->start_delay_high);
	}

	/* norandommap + verify + 가변 블록 크기 조합에서는 verify가 제한됨 */
	if (o->norandommap && o->verify != VERIFY_NONE
	    && !fixed_block_size(o))  {
		log_err("fio: norandommap given for variable block sizes, "
			"verify limited\n");
		ret |= warnings_fatal;
	}
	/* 비정렬 블록 크기(bs_unaligned)와 raw IO는 호환되지 않을 수 있음 */
	if (o->bs_unaligned && (o->odirect || td_ioengine_flagged(td, FIO_RAWIO)))
		log_err("fio: bs_unaligned may not work with raw io\n");

	/*
	 * thinktime_spin must be less than thinktime
	 */
	/* thinktime_spin(busy wait 시간)은 thinktime(총 대기 시간)보다 작아야 함 */
	if (o->thinktime_spin > o->thinktime)
		o->thinktime_spin = o->thinktime;

	/*
	 * The low water mark cannot be bigger than the iodepth
	 */
	/* IO depth 하한선은 iodepth보다 클 수 없음 */
	if (o->iodepth_low > o->iodepth || !o->iodepth_low)
		o->iodepth_low = o->iodepth;

	/*
	 * If batch number isn't set, default to the same as iodepth
	 */
	/* 배치 크기가 설정되지 않으면 iodepth와 동일하게 설정 */
	if (o->iodepth_batch > o->iodepth || !o->iodepth_batch)
		o->iodepth_batch = o->iodepth;

	/*
	 * If max batch complete number isn't set or set incorrectly,
	 * default to the same as iodepth_batch_complete_min
	 */
	/* 최대 배치 완료 수가 최소보다 작으면 최소와 동일하게 설정 */
	if (o->iodepth_batch_complete_min > o->iodepth_batch_complete_max)
		o->iodepth_batch_complete_max = o->iodepth_batch_complete_min;

	/*
	 * There's no need to check for in-flight overlapping IOs if the job
	 * isn't changing data or the maximum iodepth is guaranteed to be 1
	 * when we are not in offload mode
	 */
	/* 데이터를 변경하지 않거나 iodepth=1이고 offload 모드가 아니면
	 * 겹치는 IO 직렬화 검사가 불필요 */
	if (o->serialize_overlap && !(td->flags & TD_F_READ_IOLOG) &&
	    (!(td_write(td) || td_trim(td)) || o->iodepth == 1) &&
	    o->io_submit_mode != IO_MODE_OFFLOAD)
		o->serialize_overlap = 0;

	/* nr_files를 실제 파일 인덱스로 제한 */
	if (o->nr_files > td->files_index)
		o->nr_files = td->files_index;

	/* open_files를 nr_files로 제한 */
	if (o->open_files > o->nr_files || !o->open_files)
		o->open_files = o->nr_files;

	/* rate(바이트/초)와 rate_iops(IOPS)는 상호 배타적 */
	if (((o->rate[DDIR_READ] + o->rate[DDIR_WRITE] + o->rate[DDIR_TRIM]) &&
	    (o->rate_iops[DDIR_READ] + o->rate_iops[DDIR_WRITE] + o->rate_iops[DDIR_TRIM])) ||
	    ((o->ratemin[DDIR_READ] + o->ratemin[DDIR_WRITE] + o->ratemin[DDIR_TRIM]) &&
	    (o->rate_iops_min[DDIR_READ] + o->rate_iops_min[DDIR_WRITE] + o->rate_iops_min[DDIR_TRIM]))) {
		log_err("fio: rate and rate_iops are mutually exclusive\n");
		ret |= 1;
	}
	/* 최소 속도가 최대 속도를 초과하면 에러 */
	for_each_rw_ddir(ddir) {
		if ((o->rate[ddir] && (o->rate[ddir] < o->ratemin[ddir])) ||
		    (o->rate_iops[ddir] && (o->rate_iops[ddir] < o->rate_iops_min[ddir]))) {
			log_err("fio: minimum rate exceeds rate, ddir %d\n", +ddir);
			ret |= 1;
		}
	}

	/* time_based 옵션은 runtime/timeout이 필수 */
	if (!o->timeout && o->time_based) {
		log_err("fio: time_based requires a runtime/timeout setting\n");
		o->time_based = 0;
		ret |= warnings_fatal;
	}

	/* fill_device 옵션이고 size가 지정되지 않으면 무한대로 설정 */
	if (o->fill_device && !o->size)
		o->size = -1ULL;

	/* ===== verify(데이터 검증) 관련 옵션 보정 ===== */
	if (o->verify != VERIFY_NONE) {
		/* 다중 job 쓰기 시 다른 job의 블록을 덮어쓸 수 있어 verify 실패 가능성 경고 */
		if (td_write(td) && o->do_verify && o->numjobs > 1 &&
		    (o->filename ||
		     !(o->unique_filename &&
		       strstr(o->filename_format, "$jobname") &&
		       strstr(o->filename_format, "$jobnum") &&
		       strstr(o->filename_format, "$filenum")))) {
			log_info("fio: multiple writers may overwrite blocks "
				"that belong to other jobs. This can cause "
				"verification failures.\n");
			ret |= warnings_fatal;
		}

		/*
		 * Warn if verification is requested but no verification of any
		 * kind can be started due to time constraints
		 */
		/* 쓰기만 하고 time_based이면 verify 읽기 단계가 시작되지 않음을 경고 */
		if (td_write(td) && o->do_verify && o->timeout &&
		    o->time_based && !td_read(td) && !o->verify_backlog) {
			log_info("fio: verification read phase will never "
				 "start because write phase uses all of "
				 "runtime\n");
			ret |= warnings_fatal;
		}

		/* verify 사용 시 refill_buffers를 명시적으로 설정하지 않았으면 활성화 */
		if (!fio_option_is_set(o, refill_buffers))
			o->refill_buffers = 1;

		/* 가변 블록 크기에서 verify_interval 미설정 시 최소 쓰기 블록 크기로 설정 */
		if (o->max_bs[DDIR_WRITE] != o->min_bs[DDIR_WRITE] &&
		    !o->verify_interval)
			o->verify_interval = o->min_bs[DDIR_WRITE];

		/*
		 * Verify interval must be smaller or equal to the
		 * write size.
		 */
		/* verify 간격은 최소 블록 크기 이하여야 함 */
		if (o->verify_interval > o->min_bs[DDIR_WRITE])
			o->verify_interval = o->min_bs[DDIR_WRITE];
		else if (td_read(td) && o->verify_interval > o->min_bs[DDIR_READ])
			o->verify_interval = o->min_bs[DDIR_READ];

		/*
		 * Verify interval must be a factor of both min and max
		 * write size
		 */
		/* verify 간격은 최소/최대 쓰기 크기의 공약수여야 함 */
		if (!o->verify_interval ||
		    (o->min_bs[DDIR_WRITE] % o->verify_interval) ||
		    (o->max_bs[DDIR_WRITE] % o->verify_interval))
			o->verify_interval = gcd(o->min_bs[DDIR_WRITE],
							o->max_bs[DDIR_WRITE]);

		/* verify_only 모드에서는 쓰기 순서 검사와 헤더 시드 기본값 비활성화 */
		if (o->verify_only) {
			if (!fio_option_is_set(o, verify_write_sequence))
				o->verify_write_sequence = 0;

			if (!fio_option_is_set(o, verify_header_seed))
				o->verify_header_seed = 0;
		}

		/* norandommap + 비동기IO + iodepth > 1에서는 쓰기 순서 검사 비활성화 */
		if (o->norandommap && !td_ioengine_flagged(td, FIO_SYNCIO) &&
		    o->iodepth > 1) {
			/*
			 * Disable write sequence checks with norandommap and
			 * iodepth > 1.
			 * Unless we were explicitly asked to enable it.
			 */
			/* 명시적으로 설정하지 않은 경우에만 비활성화 */
			if (!fio_option_is_set(o, verify_write_sequence))
				o->verify_write_sequence = 0;
		}

		/*
		 * Verify header should not be offset beyond the verify
		 * interval.
		 */
		/* verify 헤더 오프셋이 verify 간격을 초과하면 에러 */
		if (o->verify_offset + sizeof(struct verify_header) >
		    o->verify_interval) {
			log_err("fio: cannot offset verify header beyond the "
				"verify interval.\n");
			ret |= 1;
		}

		/*
		 * Disable rand_seed check when we have verify_backlog,
		 * zone reset frequency for zonemode=zbd, or if we are using
		 * an RB tree for IO history logs.
		 * Unless we were explicitly asked to enable it.
		 */
		/* verify_backlog, zone 리셋 빈도, RB 트리 IO 히스토리 사용 시
		 * 헤더 시드 검사를 비활성화 (명시적으로 설정하지 않은 경우에만) */
		if (!td_write(td) || (td->flags & TD_F_VER_BACKLOG) ||
		    o->zrf.u.f || fio_offset_overlap_risk(td)) {
			if (!fio_option_is_set(o, verify_header_seed))
				o->verify_header_seed = 0;
		}
	}

	/* 원자적 쓰기(oatomic) 옵션 검증 */
	if (td->o.oatomic) {
		/* IO 엔진이 원자적 쓰기를 지원하지 않으면 에러 */
		if (!td_ioengine_flagged(td, FIO_ATOMICWRITES)) {
			log_err("fio: engine does not support atomic writes\n");
			td->o.oatomic = 0;
			ret |= 1;
		}

		/* 쓰기가 아니면 oatomic 비활성화 */
		if (!td_write(td))
			td->o.oatomic = 0;
	}

	/* pre_read 옵션 검증 */
	if (o->pre_read) {
		/* pre_read와 invalidate_cache는 호환되지 않음 */
		if (o->invalidate_cache)
			o->invalidate_cache = 0;
		/* 파이프 기반 IO 엔진에서는 pre_read 불가 (탐색 불가) */
		if (td_ioengine_flagged(td, FIO_PIPEIO)) {
			log_info("fio: cannot pre-read files with an IO engine"
				 " that isn't seekable. Pre-read disabled.\n");
			ret |= warnings_fatal;
		}
	}

	/* 단위 기본값 설정: 비트 기반 엔진이면 비트/초, 아니면 바이트/초 */
	if (o->unit_base == N2S_NONE) {
		if (td_ioengine_flagged(td, FIO_BIT_BASED))
			o->unit_base = N2S_BITPERSEC;
		else
			o->unit_base = N2S_BYTEPERSEC;
	}

#ifndef CONFIG_FDATASYNC
	/* fdatasync를 지원하지 않는 플랫폼에서는 fsync로 대체 */
	if (o->fdatasync_blocks) {
		log_info("fio: this platform does not support fdatasync()"
			 " falling back to using fsync().  Use the 'fsync'"
			 " option instead of 'fdatasync' to get rid of"
			 " this warning\n");
		o->fsync_blocks = o->fdatasync_blocks;
		o->fdatasync_blocks = 0;
		ret |= warnings_fatal;
	}
#endif

#ifdef WIN32
	/*
	 * Windows doesn't support O_DIRECT or O_SYNC with the _open interface,
	 * so fail if we're passed those flags
	 */
	/* Windows에서는 동기 IO 엔진에서 O_DIRECT/O_SYNC를 지원하지 않음 */
	if (td_ioengine_flagged(td, FIO_SYNCIO) && (o->odirect || o->sync_io)) {
		log_err("fio: Windows does not support direct or non-buffered io with"
				" the synchronous ioengines. Use the 'windowsaio' ioengine"
				" with 'direct=1' and 'iodepth=1' instead.\n");
		ret |= 1;
	}
#endif

	/*
	 * For fully compressible data, just zero them at init time.
	 * It's faster than repeatedly filling it. For non-zero
	 * compression, we should have refill_buffers set. Set it, unless
	 * the job file already changed it.
	 */
	/* 압축 비율 관련 최적화:
	 * 100% 압축 가능 = 0으로 채우기 (더 빠름)
	 * 부분 압축 = refill_buffers 활성화 (매번 새 데이터 생성) */
	if (o->compress_percentage) {
		if (o->compress_percentage == 100) {
			o->zero_buffers = 1;
			o->compress_percentage = 0;
		} else if (!fio_option_is_set(o, refill_buffers)) {
			o->refill_buffers = 1;
			td->flags |= TD_F_REFILL_BUFFERS;
		}
	}

	/*
	 * Using a non-uniform random distribution excludes usage of
	 * a random map
	 */
	/* 비균일 랜덤 분포(zipf, pareto, gauss 등)를 사용하면 randommap 비활성화 */
	if (o->random_distribution != FIO_RAND_DIST_RANDOM)
		o->norandommap = 1;

	/*
	 * If size is set but less than the min block size, complain
	 */
	/* size가 최소 블록 크기보다 작으면 에러 */
	if (o->size && o->size < td_min_bs(td)) {
		log_err("fio: size too small, must not be less than minimum block size: %llu < %llu\n",
			(unsigned long long) o->size, td_min_bs(td));
		ret |= 1;
	}

	/*
	 * If randseed is set, that overrides randrepeat
	 */
	/* rand_seed가 명시적으로 설정되면 rand_repeatable(고정 시드 재현) 비활성화 */
	if (fio_option_is_set(o, rand_seed))
		o->rand_repeatable = 0;

	/* FIO_NOEXTEND 플래그가 있는 엔진에서는 file_append 불가 */
	if (td_ioengine_flagged(td, FIO_NOEXTEND) && o->file_append) {
		log_err("fio: can't append/extent with IO engine %s\n", td->io_ops->name);
		ret |= 1;
	}

	/* gtod_cpu가 설정되면 전용 CPU에서 시간 측정을 오프로드 */
	if (fio_option_is_set(o, gtod_cpu)) {
		fio_gtod_init();
		fio_gtod_set_cpu(o->gtod_cpu);
		fio_gtod_offload = 1;
	}

	/* 루프 횟수 설정 (0이면 1로 보정) */
	td->loops = o->loops;
	if (!td->loops)
		td->loops = 1;

	/* 블록 에러 히스토그램은 단일 파일에서만 사용 가능 */
	if (o->block_error_hist && o->nr_files != 1) {
		log_err("fio: block error histogram only available "
			"with a single file per job, but %d files "
			"provided\n", o->nr_files);
		ret |= 1;
	}

	/* 레이턴시 통계 비활성화 옵션 처리 */
	if (o->disable_lat)
		o->lat_percentiles = 0;    /* 전체 레이턴시 백분위수 비활성화 */
	if (o->disable_clat)
		o->clat_percentiles = 0;   /* 완료 레이턴시 백분위수 비활성화 */
	if (o->disable_slat)
		o->slat_percentiles = 0;   /* 제출 레이턴시 백분위수 비활성화 */

	/* Do this only for the parent job */
	/* 부모 job에서만 수행: max_latency와 latency_target을 나노초로 변환 */
	if (!td->subjob_number) {
		/*
		 * Fix these up to be nsec internally
		 */
		/* 내부적으로 나노초 단위로 변환 */
		for_each_rw_ddir(ddir)
			o->max_latency[ddir] *= 1000ULL;

		o->latency_target *= 1000ULL;
	}

	/*
	 * Dedupe working set verifications
	 */
	/* 중복 제거(dedupe) 작업 세트 모드 검증 */
	if (o->dedupe_percentage && o->dedupe_mode == DEDUPE_MODE_WORKING_SET) {
		/* 사전 생성 dedupe 작업 세트는 size 설정 필수 */
		if (!fio_option_is_set(o, size)) {
			log_err("fio: pregenerated dedupe working set "
					"requires size to be set\n");
			ret |= 1;
		} else if (o->nr_files != 1) {
			/* 단일 파일에서만 지원 */
			log_err("fio: dedupe working set mode supported with "
					"single file per job, but %d files "
					"provided\n", o->nr_files);
			ret |= 1;
		} else if (o->dedupe_working_set_percentage + o->dedupe_percentage > 100) {
			/* 작업 세트 비율 + 중복 비율이 100%를 초과하면 달성 불가 */
			log_err("fio: impossible to reach expected dedupe percentage %u "
					"since %u percentage of size is reserved to dedupe working set "
					"(those are unique pages)\n",
					o->dedupe_percentage, o->dedupe_working_set_percentage);
			ret |= 1;
		}
	}

	/* 모든 job의 ss_check_interval이 동일해야 함 (전역 일관성 요구) */
	for_each_td(td2) {
		if (td->o.ss_check_interval != td2->o.ss_check_interval) {
			log_err("fio: conflicting ss_check_interval: %llu and %llu, must be globally equal\n",
					td->o.ss_check_interval, td2->o.ss_check_interval);
			ret |= 1;
		}
	} end_for_each();
	/* ss_check_interval은 최소 1초 */
	if (td->o.ss_dur && td->o.ss_check_interval / 1000L < 1000) {
		log_err("fio: ss_check_interval must be at least 1s\n");
		ret |= 1;

	}
	/* ss_duration은 ss_check_interval의 배수여야 하며, 그보다 커야 함 */
	if (td->o.ss_dur && (td->o.ss_dur % td->o.ss_check_interval != 0 || td->o.ss_dur <= td->o.ss_check_interval)) {
		log_err("fio: ss_duration %lluus must be multiple of ss_check_interval %lluus\n",
				td->o.ss_dur, td->o.ss_check_interval);
		ret |= 1;
	}

	/* FDP(Flexible Data Placement) 옵션 검증 */
	if (td->o.fdp) {
		if (fio_option_is_set(&td->o, dp_type) &&
			(td->o.dp_type == FIO_DP_STREAMS || td->o.dp_type == FIO_DP_NONE)) {
			/* fdp=1은 dataplacement=streams 또는 none과 호환되지 않음 */
			log_err("fio: fdp=1 is not compatible with dataplacement={streams, none}\n");
			ret |= 1;
		} else {
			td->o.dp_type = FIO_DP_FDP;
		}
	}
	return ret;
}

/*
 * init_rand_file_service() - 랜덤 파일 서비스 분포 초기화 함수
 *
 * 역할: 파일 선택에 비균일 분포(zipf, pareto, gauss)를 사용할 때
 *       해당 분포 생성기를 초기화합니다.
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 없음
 */
static void init_rand_file_service(struct thread_data *td)
{
	/* 파일 수를 FIO_FSERVICE_SHIFT만큼 시프트하여 범위 설정 */
	unsigned long nranges = td->o.nr_files << FIO_FSERVICE_SHIFT;
	const unsigned int seed = td->rand_seeds[FIO_RAND_FILE_OFF];

	/* 파일 서비스 타입에 따라 적절한 분포 생성기 초기화 */
	if (td->o.file_service_type == FIO_FSERVICE_ZIPF) {
		zipf_init(&td->next_file_zipf, nranges, td->zipf_theta, td->random_center, seed);
		zipf_disable_hash(&td->next_file_zipf);
	} else if (td->o.file_service_type == FIO_FSERVICE_PARETO) {
		pareto_init(&td->next_file_zipf, nranges, td->pareto_h, td->random_center, seed);
		zipf_disable_hash(&td->next_file_zipf);
	} else if (td->o.file_service_type == FIO_FSERVICE_GAUSS) {
		gauss_init(&td->next_file_gauss, nranges, td->gauss_dev, td->random_center, seed);
		gauss_disable_hash(&td->next_file_gauss);
	}
}

/*
 * Separate initialization of the random generator for offsets in case we need
 * to re-initialize it if we discover later on that the combination of filesize
 * and block size exceeds the limits of the default random generator.
 */
/*
 * init_rand_offset_seed() - 오프셋용 난수 생성기 초기화 함수
 *
 * 역할: IO 오프셋 계산에 사용되는 난수 생성기를 초기화합니다.
 *       파일 크기와 블록 크기 조합이 기본 생성기의 한계를 초과할 경우
 *       나중에 재초기화될 수 있어 별도 함수로 분리되어 있습니다.
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 없음
 */
void init_rand_offset_seed(struct thread_data *td)
{
	bool use64;

	/* Tausworthe64 생성기를 사용하면 64비트 모드로 초기화 */
	if (td->o.random_generator == FIO_RAND_GEN_TAUSWORTHE64)
		use64 = true;
	else
		use64 = false;

	init_rand_seed(&td->offset_state, td->rand_seeds[FIO_RAND_BLOCK_OFF], use64);
}

/*
 * td_fill_rand_seeds() - 모든 난수 생성기를 시드로 초기화하는 함수
 *
 * 역할: thread_data의 모든 난수 생성기(블록 크기, verify, 파일 선택,
 *       지연, 중복 제거, 우선순위 등)를 각각의 시드로 초기화합니다.
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 없음
 *
 * 특수 처리:
 *   - verify 사용 시: 쓰기 시드 = 읽기 시드 (동일 오프셋 보장)
 *   - trimwrite 시: 트림 시드 = 쓰기 시드 (트림 후 같은 위치에 쓰기)
 */
void td_fill_rand_seeds(struct thread_data *td)
{
	uint64_t read_seed = td->rand_seeds[FIO_RAND_BS_OFF];
	uint64_t write_seed = td->rand_seeds[FIO_RAND_BS1_OFF];
	uint64_t trim_seed = td->rand_seeds[FIO_RAND_BS2_OFF];
	int i;
	bool use64;

	/* 64비트 Tausworthe 생성기 사용 여부 확인 */
	if (td->o.random_generator == FIO_RAND_GEN_TAUSWORTHE64)
		use64 = true;
	else
		use64 = false;

	/*
	 * trimwrite is special in that we need to generate the same
	 * offsets to get the "write after trim" effect. If we are
	 * using bssplit to set buffer length distributions, ensure that
	 * we seed the trim and write generators identically. Ditto for
	 * verify, read and writes must have the same seed, if we are doing
	 * read verify.
	 */
	/* verify 사용 시 읽기와 쓰기에 동일한 시드를 사용하여 같은 오프셋 패턴 보장 */
	if (td->o.verify != VERIFY_NONE)
		write_seed = read_seed;
	/* trimwrite에서는 트림과 쓰기에 동일한 시드 사용 */
	if (td_trimwrite(td))
		trim_seed = write_seed;

	/* 블록 크기 범위 난수 생성기 초기화 (읽기/쓰기/트림 각각) */
	init_rand_seed(&td->bsrange_state[DDIR_READ], read_seed, use64);
	init_rand_seed(&td->bsrange_state[DDIR_WRITE], write_seed, use64);
	init_rand_seed(&td->bsrange_state[DDIR_TRIM], trim_seed, use64);

	/* verify 상태 난수 생성기 */
	init_rand_seed(&td->verify_state, td->rand_seeds[FIO_RAND_VER_OFF],
		use64);
	/* 읽기/쓰기 혼합 비율 난수 생성기 */
	init_rand_seed(&td->rwmix_state, td->rand_seeds[FIO_RAND_MIX_OFF], false);

	/* 파일 서비스 타입에 따른 난수 생성기 초기화 */
	if (td->o.file_service_type == FIO_FSERVICE_RANDOM)
		init_rand_seed(&td->next_file_state, td->rand_seeds[FIO_RAND_FILE_OFF], use64);
	else if (td->o.file_service_type & __FIO_FSERVICE_NONUNIFORM)
		init_rand_file_service(td);  /* 비균일 분포 초기화 */

	/* 파일 크기 난수 생성기 */
	init_rand_seed(&td->file_size_state, td->rand_seeds[FIO_RAND_FILE_SIZE_OFF], use64);
	/* 트림 난수 생성기 */
	init_rand_seed(&td->trim_state, td->rand_seeds[FIO_RAND_TRIM_OFF], false);
	/* 시작 지연 난수 생성기 */
	init_rand_seed(&td->delay_state, td->rand_seeds[FIO_RAND_START_DELAY], use64);
	/* 포아송 분포 난수 생성기 (3개 - IO 간격 모델링에 사용) */
	init_rand_seed(&td->poisson_state[0], td->rand_seeds[FIO_RAND_POISSON_OFF], 0);
	init_rand_seed(&td->poisson_state[1], td->rand_seeds[FIO_RAND_POISSON2_OFF], 0);
	init_rand_seed(&td->poisson_state[2], td->rand_seeds[FIO_RAND_POISSON3_OFF], 0);
	/* 중복 제거 난수 생성기 */
	init_rand_seed(&td->dedupe_state, td->rand_seeds[FIO_DEDUPE_OFF], false);
	/* 존 선택 난수 생성기 */
	init_rand_seed(&td->zone_state, td->rand_seeds[FIO_RAND_ZONE_OFF], false);
	/* IO 우선순위 난수 생성기 */
	init_rand_seed(&td->prio_state, td->rand_seeds[FIO_RAND_PRIO_CMDS], false);
	/* 중복 제거 작업 세트 인덱스 난수 생성기 */
	init_rand_seed(&td->dedupe_working_set_index_state, td->rand_seeds[FIO_RAND_DEDUPE_WORKING_SET_IX], use64);

	/* 오프셋용 난수 생성기 초기화 */
	init_rand_offset_seed(td);

	/* 순차/랜덤 전환 난수 생성기 초기화 (각 IO 방향별) */
	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		struct frand_state *s = &td->seq_rand_state[i];

		init_rand_seed(s, td->rand_seeds[FIO_RAND_SEQ_RAND_READ_OFF], false);
	}

	/* 버퍼 내용 난수 생성기 초기화 및 이전 상태 복사 */
	init_rand_seed(&td->buf_state, td->rand_seeds[FIO_RAND_BUF_OFF], use64);
	frand_copy(&td->buf_state_prev, &td->buf_state);

	/* FDP(Flexible Data Placement) 난수 생성기 */
	init_rand_seed(&td->fdp_state, td->rand_seeds[FIO_RAND_FDP_OFF], false);
	/* SPRandom 난수 생성기 */
	init_rand_seed(&td->sprandom_state, td->rand_seeds[FIO_RAND_SPRANDOM_OFF], false);
}

/*
 * setup_random_seeds() - 난수 시드 배열 설정 함수
 *
 * 역할: 모든 난수 생성기의 시드를 설정합니다.
 *       rand_repeatable이 아니고 rand_seed가 설정되지 않으면 시스템 RNG 사용,
 *       그렇지 않으면 결정적(deterministic) 시드 생성.
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 성공 시 0, 실패 시 에러 코드
 */
static int setup_random_seeds(struct thread_data *td)
{
	uint64_t seed;
	unsigned int i;

	/* rand_repeatable이 아니고 rand_seed가 미설정이면 시스템 RNG로 시드 생성 */
	if (!td->o.rand_repeatable && !fio_option_is_set(&td->o, rand_seed)) {
		int ret = init_random_seeds(td->rand_seeds, sizeof(td->rand_seeds));
		dprint(FD_RANDOM, "using system RNG for random seeds\n");
		if (ret)
			return ret;
	} else {
		/* 결정적 시드 생성: 기본 시드에서 해시 함수로 파생 */
		seed = td->o.rand_seed;
		for (i = 0; i < 4; i++)
			seed *= 0x9e370001UL;  /* 해시 곱셈 (좋은 분포를 위한 소수) */

		/* 각 시드 슬롯에 스레드 번호를 곱하여 고유한 시드 생성 */
		for (i = 0; i < FIO_RAND_NR_OFFS; i++) {
			td->rand_seeds[i] = seed * td->thread_number + i;
			seed *= 0x9e370001UL;
		}
	}

	/* 디버그 출력: 생성된 시드 값들 */
	dprint(FD_RANDOM, "FIO_RAND_NR_OFFS=%d\n", FIO_RAND_NR_OFFS);
	for (int i = 0; i < FIO_RAND_NR_OFFS; i++)
		dprint(FD_RANDOM, "rand_seeds[%d]=%" PRIu64 "\n", i, td->rand_seeds[i]);

	/* 생성된 시드로 모든 난수 생성기 초기화 */
	td_fill_rand_seeds(td);

	return 0;
}

/*
 * Initializes the ioengine configured for a job, if it has not been done so
 * already.
 */
/*
 * ioengine_load() - IO 엔진 로드 함수
 *
 * 역할: job에 설정된 IO 엔진을 동적으로 로드합니다.
 *       이미 같은 엔진이 로드되어 있으면 건너뜁니다.
 *       다른 엔진이 로드되어 있으면 기존 엔진을 해제하고 새 엔진을 로드합니다.
 *
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 성공 시 0, 실패 시 1
 *
 * IO 엔진 로드 후 처리:
 *   1. 엔진 전용 옵션 구조체 할당 및 초기화
 *   2. odirect 플래그에 따라 FIO_RAWIO 설정
 *   3. 엔진 플래그 설정
 */
int ioengine_load(struct thread_data *td)
{
	/* IO 엔진 이름이 설정되지 않았으면 내부 오류 */
	if (!td->o.ioengine) {
		log_err("fio: internal fault, no IO engine specified\n");
		return 1;
	}

	/* 이미 엔진이 로드되어 있는 경우 처리 */
	if (td->io_ops) {
		struct ioengine_ops *ops;
		void *dlhandle;

		/* An engine is loaded, but the requested ioengine
		 * may have changed.
		 */
		/* 현재 로드된 엔진과 요청된 엔진이 같은지 확인 */
		if (!strcmp(td->io_ops->name, td->o.ioengine)) {
			/* The right engine is already loaded */
			/* 같은 엔진이 이미 로드되어 있으므로 아무것도 하지 않음 */
			return 0;
		}

		/*
		 * Name of file and engine may be different, load ops
		 * for this name and see if they match. If they do, then
		 * the engine is unchanged.
		 */
		/* 파일 이름과 엔진 이름이 다를 수 있으므로 실제 ops를 로드하여 비교 */
		dlhandle = td->io_ops->dlhandle;
		ops = load_ioengine(td);
		if (!ops)
			goto fail;

		/* 같은 ops이고 같은 dlhandle이면 엔진 변경 없음 */
		if (ops == td->io_ops && dlhandle == td->io_ops->dlhandle)
			return 0;

		/* 다른 dlhandle이면 이전 것 닫기 */
		if (dlhandle && dlhandle != td->io_ops->dlhandle)
			dlclose(dlhandle);

		/* Unload the old engine. */
		/* 이전 엔진 해제 */
		free_ioengine(td);
	}

	/* 새 IO 엔진 로드 */
	td->io_ops = load_ioengine(td);
	if (!td->io_ops)
		goto fail;

	/* 엔진에 전용 옵션 구조체가 있는 경우 처리 */
	if (td->io_ops->option_struct_size && td->io_ops->options) {
		/*
		 * In cases where td->eo is set, clone it for a child thread.
		 * This requires that the parent thread has the same ioengine,
		 * but that requirement must be enforced by the code which
		 * cloned the thread.
		 */
		/* td->eo가 설정되어 있으면 자식 스레드를 위해 복제 */
		void *origeo = td->eo;
		/*
		 * Otherwise use the default thread options.
		 */
		/* 그렇지 않으면 기본 스레드의 엔진 옵션 사용 */
		if (!origeo && td != &def_thread && def_thread.eo &&
		    def_thread.io_ops->options == td->io_ops->options)
			origeo = def_thread.eo;

		/* 엔진 옵션 초기화 */
		options_init(td->io_ops->options);
		td->eo = malloc(td->io_ops->option_struct_size);
		/*
		 * Use the default thread as an option template if this uses the
		 * same options structure and there are non-default options
		 * used.
		 */
		/* 원본 옵션이 있으면 복사, 없으면 기본값으로 채우기 */
		if (origeo) {
			memcpy(td->eo, origeo, td->io_ops->option_struct_size);
			options_mem_dupe(td->io_ops->options, td->eo);
		} else {
			memset(td->eo, 0, td->io_ops->option_struct_size);
			fill_default_options(td->eo, td->io_ops->options);
		}
		/* 엔진 옵션 구조체의 첫 번째 필드에 td 포인터 저장 (역참조용) */
		*(struct thread_data **)td->eo = td;
	}

	/* O_DIRECT 사용 시 엔진에 FIO_RAWIO 플래그 추가 */
	if (td->o.odirect)
		td->io_ops->flags |= FIO_RAWIO;

	/* IO 엔진 플래그를 thread_data에 설정 */
	td_set_ioengine_flags(td);
	return 0;

fail:
	log_err("fio: failed to load engine\n");
	return 1;

}

/*
 * init_flags() - thread_data 플래그 초기화 함수
 *
 * 역할: thread_options의 설정값에 따라 thread_data의 flags 비트를 설정합니다.
 *       이 플래그들은 런타임에 빠른 조건 확인을 위해 사용됩니다.
 * 파라미터:
 *   td - 대상 thread_data
 * 반환값: 없음
 */
static void init_flags(struct thread_data *td)
{
	struct thread_options *o = &td->o;
	int i;

	/* verify backlog 사용 시 플래그 설정 */
	if (o->verify_backlog)
		td->flags |= TD_F_VER_BACKLOG;
	/* trim backlog 사용 시 플래그 설정 */
	if (o->trim_backlog)
		td->flags |= TD_F_TRIM_BACKLOG;
	/* IO 로그 파일 읽기 모드 */
	if (o->read_iolog_file)
		td->flags |= TD_F_READ_IOLOG;
	/* 버퍼 재채우기 활성화 */
	if (o->refill_buffers)
		td->flags |= TD_F_REFILL_BUFFERS;
	/*
	 * Always scramble buffers if asked to
	 */
	/* 명시적으로 버퍼 스크램블이 요청된 경우 항상 활성화 */
	if (o->scramble_buffers && fio_option_is_set(o, scramble_buffers))
		td->flags |= TD_F_SCRAMBLE_BUFFERS;
	/*
	 * But also scramble buffers, unless we were explicitly asked
	 * to zero them.
	 */
	/* zero_buffers가 명시적으로 설정되지 않은 한, 기본적으로 버퍼 스크램블 활성화 */
	if (o->scramble_buffers && !(o->zero_buffers &&
	    fio_option_is_set(o, zero_buffers)))
		td->flags |= TD_F_SCRAMBLE_BUFFERS;
	/* verify가 설정되면 검증 수행 플래그 설정 */
	if (o->verify != VERIFY_NONE)
		td->flags |= TD_F_DO_VERIFY;

	/* 비동기 verify 또는 오프로드 모드에서는 잠금 필요 */
	if (o->verify_async || o->io_submit_mode == IO_MODE_OFFLOAD)
		td->flags |= TD_F_NEED_LOCK;

	/* CUDA 메모리를 사용하면 버퍼 스크램블 비활성화 (GPU 메모리 직접 조작 불가) */
	if (o->mem_type == MEM_CUDA_MALLOC)
		td->flags &= ~TD_F_SCRAMBLE_BUFFERS;

	/* rate 검사가 필요한 방향이 하나라도 있으면 플래그 설정 */
	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		if (option_check_rate(td, i)) {
			td->flags |= TD_F_CHECK_RATE;
			break;
		}
	}
}

/* 파일 이름 포맷 문자열에서 사용되는 키워드 타입 열거형 */
enum {
	FPRE_NONE = 0,        /* 없음 */
	FPRE_JOBNAME,         /* $jobname - job 이름으로 치환 */
	FPRE_JOBNUM,          /* $jobnum - job 번호로 치환 */
	FPRE_FILENUM,         /* $filenum - 파일 번호로 치환 */
	FPRE_CLIENTUID        /* $clientuid - 클라이언트 소켓 주소로 치환 */
};

/* 파일 이름 포맷 키워드 구조체 및 배열 */
static struct fpre_keyword {
	const char *keyword;  /* 키워드 문자열 (예: "$jobname") */
	size_t strlen;        /* 키워드 길이 (캐시됨) */
	int key;              /* 키워드 타입 (FPRE_* 열거형) */
} fpre_keywords[] = {
	{ .keyword = "$jobname",	.key = FPRE_JOBNAME, },
	{ .keyword = "$jobnum",		.key = FPRE_JOBNUM, },
	{ .keyword = "$filenum",	.key = FPRE_FILENUM, },
	{ .keyword = "$clientuid",	.key = FPRE_CLIENTUID, },
	{ .keyword = NULL, },  /* 배열 종료 표시 */
	};

/*
 * make_filename() - 파일 이름 생성 함수
 *
 * 역할: filename_format 옵션에 따라 파일 이름을 생성합니다.
 *       $jobname, $jobnum, $filenum, $clientuid 키워드를 실제 값으로 치환합니다.
 * 파라미터:
 *   buf      - 결과 파일 이름을 저장할 버퍼
 *   buf_size - 버퍼 크기
 *   o        - thread_options (filename_format 포함)
 *   jobname  - job 이름
 *   jobnum   - job 번호
 *   filenum  - 파일 번호
 * 반환값: 생성된 파일 이름 문자열(buf) 포인터
 */
static char *make_filename(char *buf, size_t buf_size,struct thread_options *o,
			   const char *jobname, int jobnum, int filenum)
{
	struct fpre_keyword *f;
	char copy[PATH_MAX];
	size_t dst_left = PATH_MAX - 1;

	/* filename_format이 설정되지 않았으면 기본 형식 사용: jobname.jobnum.filenum */
	if (!o->filename_format || !strlen(o->filename_format)) {
		sprintf(buf, "%s.%d.%d", jobname, jobnum, filenum);
		return buf;
	}

	/* 각 키워드의 길이를 미리 계산 (성능 최적화) */
	for (f = &fpre_keywords[0]; f->keyword; f++)
		f->strlen = strlen(f->keyword);

	/* 포맷 문자열을 버퍼에 복사 */
	snprintf(buf, buf_size, "%s", o->filename_format);

	/* 각 키워드를 순회하며 치환 수행 */
	memset(copy, 0, sizeof(copy));
	for (f = &fpre_keywords[0]; f->keyword; f++) {
		do {
			size_t pre_len, post_start = 0;
			char *str, *dst = copy;

			/* 버퍼에서 키워드 검색 (대소문자 무시) */
			str = strcasestr(buf, f->keyword);
			if (!str)
				break;  /* 키워드가 없으면 다음 키워드로 */

			/* 키워드 앞부분(prefix)의 길이 계산 */
			pre_len = str - buf;
			if (strlen(str) != f->strlen)
				post_start = pre_len + f->strlen;  /* 키워드 뒷부분 시작 위치 */

			/* 키워드 앞부분을 copy 버퍼에 복사 */
			if (pre_len) {
				strncpy(dst, buf, pre_len);
				dst += pre_len;
				dst_left -= pre_len;
			}

			/* 키워드 타입에 따라 실제 값으로 치환 */
			switch (f->key) {
			case FPRE_JOBNAME: {
				/* $jobname → 실제 job 이름 */
				int ret;

				ret = snprintf(dst, dst_left, "%s", jobname);
				if (ret < 0)
					break;
				else if (ret > dst_left) {
					log_err("fio: truncated filename\n");
					dst += dst_left;
					dst_left = 0;
				} else {
					dst += ret;
					dst_left -= ret;
				}
				break;
				}
			case FPRE_JOBNUM: {
				/* $jobnum → 실제 job 번호 */
				int ret;

				ret = snprintf(dst, dst_left, "%d", jobnum);
				if (ret < 0)
					break;
				else if (ret > dst_left) {
					log_err("fio: truncated filename\n");
					dst += dst_left;
					dst_left = 0;
				} else {
					dst += ret;
					dst_left -= ret;
				}
				break;
				}
			case FPRE_FILENUM: {
				/* $filenum → 실제 파일 번호 */
				int ret;

				ret = snprintf(dst, dst_left, "%d", filenum);
				if (ret < 0)
					break;
				else if (ret > dst_left) {
					log_err("fio: truncated filename\n");
					dst += dst_left;
					dst_left = 0;
				} else {
					dst += ret;
					dst_left -= ret;
				}
				break;
				}
			case FPRE_CLIENTUID: {
				/* $clientuid → 클라이언트 소켓 주소 문자열 */
				int ret;
				ret = snprintf(dst, dst_left, "%s", client_sockaddr_str);
				if (ret < 0)
					break;
				else if (ret > dst_left) {
					log_err("fio: truncated filename\n");
					dst += dst_left;
					dst_left = 0;
				} else {
					dst += ret;
					dst_left -= ret;
				}
				break;
				}
			default:
				assert(0);  /* 알 수 없는 키워드 타입 - 프로그램 오류 */
				break;
			}

			/* 키워드 뒷부분(suffix)을 copy 버퍼에 추가 */
			if (post_start)
				strncpy(dst, buf + post_start, dst_left);

			/* copy 버퍼의 내용을 다시 buf로 복사 (다음 키워드 치환을 위해) */
			snprintf(buf, buf_size, "%s", copy);
		} while (1);  /* 같은 키워드가 여러 번 나올 수 있으므로 반복 */
	}

	return buf;
}

/*
 * parse_dryrun() - 드라이런(실제 IO 없이 파싱만) 여부 확인 함수
 *
 * 역할: --showcmd 또는 --parse-only 옵션이 설정되었는지 확인합니다.
 * 반환값: 드라이런이면 true, 아니면 false
 */
bool parse_dryrun(void)
{
	return dump_cmdline || parse_only;
}

/*
 * gen_log_name() - 로그 파일 이름 생성 함수
 *
 * 역할: 로그 파일의 전체 이름을 생성합니다.
 *       per_job이면 스레드 번호가 포함됩니다.
 * 파라미터:
 *   name    - 결과 이름을 저장할 버퍼
 *   size    - 버퍼 크기
 *   logtype - 로그 타입 문자열 (lat, slat, clat, bw, iops 등)
 *   logname - 로그 기본 이름
 *   num     - 스레드 번호
 *   suf     - 파일 확장자 (log 또는 log.fz)
 *   per_job - per-job 로그 여부
 * 반환값: 없음
 */
static void gen_log_name(char *name, size_t size, const char *logtype,
			 const char *logname, unsigned int num,
			 const char *suf, int per_job)
{
	/* per_job이면 스레드 번호 포함, 아니면 생략 */
	if (per_job)
		snprintf(name, size, "%s_%s.%d.%s", logname, logtype, num, suf);
	else
		snprintf(name, size, "%s_%s.%s", logname, logtype, suf);
}

/*
 * check_waitees() - wait_for 대상 job 존재 여부 확인 함수
 *
 * 역할: 주어진 이름의 job이 몇 개 존재하는지 세어 반환합니다.
 *       wait_for 옵션의 유효성 검증에 사용됩니다.
 * 파라미터:
 *   waitee - 대기 대상 job 이름
 * 반환값: 해당 이름의 job 수
 */
static int check_waitees(char *waitee)
{
	int ret = 0;

	/* 모든 thread_data를 순회하며 이름 일치 확인 (서브잡은 제외) */
	for_each_td(td) {
		if (td->subjob_number)
			continue;

		ret += !strcmp(td->o.name, waitee);
	} end_for_each();

	return ret;
}

/*
 * wait_for_ok() - wait_for 옵션 유효성 검증 함수
 *
 * 역할: wait_for 옵션이 설정된 경우, 대기 대상 job이 유효한지 확인합니다.
 *       자기 자신을 기다리거나, 존재하지 않는 job을 기다리거나,
 *       동일 이름의 job이 여러 개인 경우 에러를 반환합니다.
 * 파라미터:
 *   jobname - 현재 job 이름
 *   o       - thread_options
 * 반환값: 유효하면 true, 에러면 false
 */
static bool wait_for_ok(const char *jobname, struct thread_options *o)
{
	int nw;

	/* wait_for가 설정되지 않았으면 항상 OK */
	if (!o->wait_for)
		return true;

	/* 자기 자신을 기다릴 수 없음 */
	if (!strcmp(jobname, o->wait_for)) {
		log_err("%s: a job cannot wait for itself (wait_for=%s).\n",
				jobname, o->wait_for);
		return false;
	}

	/* 대기 대상 job이 존재하지 않으면 에러 */
	if (!(nw = check_waitees(o->wait_for))) {
		log_err("%s: waitee job %s unknown.\n", jobname, o->wait_for);
		return false;
	}

	/* 동일 이름의 대기 대상 job이 여러 개이면 에러 (모호성 방지) */
	if (nw > 1) {
		log_err("%s: multiple waitees %s found,\n"
			"please avoid duplicates when using wait_for option.\n",
				jobname, o->wait_for);
		return false;
	}

	return true;
}

/*
 * verify_per_group_options() - 그룹 내 옵션 일관성 검증 함수
 *
 * 역할: 같은 그룹에 속한 job들의 옵션이 일관되는지 확인합니다.
 *       현재는 lat_percentiles 옵션의 일관성만 검사합니다.
 * 파라미터:
 *   td      - 대상 thread_data
 *   jobname - job 이름 (에러 메시지 출력용)
 * 반환값: 성공 시 0, 실패 시 1
 */
static int verify_per_group_options(struct thread_data *td, const char *jobname)
{
	/* 같은 그룹의 모든 job을 순회 */
	for_each_td(td2) {
		/* 다른 그룹은 건너뜀 */
		if (td->groupid != td2->groupid)
			continue;

		/* 같은 그룹 내에서 lat_percentiles가 다르면 에러 */
		if (td->o.stats &&
		    td->o.lat_percentiles != td2->o.lat_percentiles) {
			log_err("fio: lat_percentiles in job: %s differs from group\n",
				jobname);
			return 1;
		}
	} end_for_each();

	return 0;
}

/*
 * Treat an empty log file name the same as a one not given
 */
/*
 * make_log_name() - 로그 이름 결정 함수
 *
 * 역할: 로그 파일 이름이 비어있으면 job 이름을 대신 사용합니다.
 * 파라미터:
 *   logname - 설정된 로그 파일 이름
 *   jobname - job 이름 (fallback)
 * 반환값: 유효한 로그 이름 문자열
 */
static const char *make_log_name(const char *logname, const char *jobname)
{
	/* logname이 존재하고 빈 문자열이 아니면 그대로 사용 */
	if (logname && strcmp(logname, ""))
		return logname;

	/* 그렇지 않으면 job 이름을 로그 이름으로 사용 */
	return jobname;
}

/*
 * Adds a job to the list of things todo. Sanitizes the various options
 * to make sure we don't have conflicts, and initializes various
 * members of td.
 */
/*
 * add_job() - job 등록 함수 (핵심 함수)
 *
 * 역할: thread_data를 최종적으로 검증하고 초기화하여 실행 대기열에 추가합니다.
 *       이 함수는 fio 초기화 과정의 핵심으로, 모든 옵션 검증과 런타임 설정이
 *       여기서 완료됩니다.
 *
 * 파라미터:
 *   td          - 등록할 thread_data
 *   jobname     - job 이름
 *   job_add_num - 현재 job의 서브잡 번호 (numjobs > 1일 때)
 *   recursed    - 재귀 호출 여부 (numjobs 처리 시)
 *   client_type - 클라이언트 타입 (CLI, GUI 등)
 *
 * 반환값: 성공 시 0, 실패 시 -1
 *
 * 처리 순서:
 *   1. init_flags() - 플래그 비트 설정
 *   2. ioengine_load() - IO 엔진 로드
 *   3. 파일 추가 (filename 미설정 시 자동 생성)
 *   4. setup_random_seeds() - 난수 시드 설정
 *   5. fixup_options() - 옵션 의존성 해결
 *   6. flow_init_job() - flow 제어 초기화
 *   7. 로그 파일 설정 (lat, bw, iops, hist)
 *   8. 통계 초기화
 *   9. stonewall/그룹 처리
 *   10. rate 설정
 *   11. steady state 초기화
 *   12. numjobs > 1이면 재귀적으로 서브잡 추가
 */
static int add_job(struct thread_data *td, const char *jobname, int job_add_num,
		   int recursed, int client_type)
{
	unsigned int i;
	char fname[PATH_MAX + 1];
	int numjobs, file_alloced;
	struct thread_options *o = &td->o;
	char logname[PATH_MAX + 32];

	/*
	 * the def_thread is just for options, it's not a real job
	 */
	/* def_thread는 옵션 템플릿일 뿐 실제 job이 아니므로 즉시 반환 */
	if (td == &def_thread)
		return 0;

	/* 플래그 비트 초기화 (verify, scramble, rate 등) */
	init_flags(td);

	/*
	 * if we are just dumping the output command line, don't add the job
	 */
	/* 드라이런 모드에서는 job을 추가하지 않고 해제만 함 */
	if (parse_dryrun()) {
		put_job(td);
		return 0;
	}

	/* 클라이언트 타입 설정 */
	td->client_type = client_type;

	/* 프로파일 초기화 (실행 프로파일이 설정된 경우) */
	if (profile_td_init(td))
		goto err;

	/* IO 엔진 동적 로드 (libaio, io_uring, sync 등) */
	if (ioengine_load(td))
		goto err;

	/* ===== 파일 설정 ===== */
	file_alloced = 0;
	/* filename이 명시적으로 지정되지 않고, 파일 목록이 비어있고,
	 * IO 로그 재생 모드가 아닌 경우 자동으로 파일 생성 */
	if (!o->filename && !td->files_index && !o->read_iolog_file) {
		file_alloced = 1;

		/* 파일이 1개이고 jobname이 일반 파일이 아닌 경우 (디바이스 등) */
		if (o->nr_files == 1 && exists_and_not_regfile(jobname))
			add_file(td, jobname, job_add_num, 0);
		else {
			/* nr_files만큼 파일 생성 (filename_format에 따라 이름 생성) */
			for (i = 0; i < o->nr_files; i++)
				add_file(td, make_filename(fname, sizeof(fname), o, jobname, job_add_num, i), job_add_num, 0);
		}
	}

	/* 난수 시드 설정 - 모든 난수 생성기 초기화 */
	if (setup_random_seeds(td)) {
		td_verror(td, errno, "setup_random_seeds");
		goto err;
	}

	/* 옵션 간 의존성 및 충돌 해결 */
	if (fixup_options(td))
		goto err;

	/* 중복 제거 작업 세트 시드 초기화 (글로벌이 아닌 경우) */
	if (!td->o.dedupe_global && init_dedupe_working_set_seeds(td, 0))
		goto err;

	/*
	 * Belongs to fixup_options, but o->name is not necessarily set as yet
	 */
	/* wait_for 옵션 검증 (fixup_options에 포함되어야 하지만
	 * 이 시점에서야 job 이름이 확정됨) */
	if (!wait_for_ok(jobname, o))
		goto err;

	/* flow 제어 초기화 (IO 속도 조절에 사용) */
	flow_init_job(td);

	/*
	 * IO engines only need this for option callbacks, and the address may
	 * change in subprocesses.
	 */
	/* IO 엔진의 td 역참조 포인터 초기화 (서브프로세스에서 주소 변경 가능) */
	if (td->eo)
		*(struct thread_data **)td->eo = NULL;

	/* 디스크리스(diskless) IO 엔진의 경우 파일 크기를 무한대로 설정 */
	if (td_ioengine_flagged(td, FIO_DISKLESSIO)) {
		struct fio_file *f;

		for_each_file(td, f, i)
			f->real_file_size = -1ULL;
	}

	/* 세마포어 초기화 (잠김 상태로 시작 - 메인 스레드가 시작 신호를 줄 때까지 대기) */
	td->sem = fio_sem_init(FIO_SEM_LOCKED);

	/* ===== 통계(thread_stat) 초기화 ===== */
	td->ts.clat_percentiles = o->clat_percentiles;    /* 완료 레이턴시 백분위수 */
	td->ts.lat_percentiles = o->lat_percentiles;      /* 전체 레이턴시 백분위수 */
	td->ts.slat_percentiles = o->slat_percentiles;    /* 제출 레이턴시 백분위수 */
	td->ts.percentile_precision = o->percentile_precision;  /* 백분위수 정밀도 */
	memcpy(td->ts.percentile_list, o->percentile_list, sizeof(o->percentile_list));
	td->ts.sig_figs = o->sig_figs;  /* 유효 숫자 자릿수 */

	/* 통계 최소값 초기화 */
	init_thread_stat_min_vals(&td->ts);

	/*
	 * td->>ddir_seq_nr needs to be initialized to 1, NOT o->ddir_seq_nr,
	 * so that get_next_offset gets a new random offset the first time it
	 * is called, instead of keeping an initial offset of 0 for the first
	 * nr-1 calls
	 */
	/* 순차 IO 방향 시퀀스 번호를 1로 초기화
	 * (첫 호출 시 새 랜덤 오프셋을 생성하기 위함) */
	td->ddir_seq_nr = 1;

	/* ===== stonewall/그룹 처리 ===== */
	/* stonewall 또는 new_group이 설정되고 이전 그룹에 job이 있으면 새 그룹 시작 */
	if ((o->stonewall || o->new_group) && prev_group_jobs) {
		prev_group_jobs = 0;
		groupid++;
		if (groupid == INT_MAX) {
			log_err("fio: too many groups defined\n");
			goto err;
		}
	}

	/* 현재 job의 그룹 ID 할당 */
	td->groupid = groupid;
	prev_group_jobs++;

	/* 그룹 리포팅에서 그룹 내 옵션 일관성 검증 */
	if (td->o.group_reporting && prev_group_jobs > 1 &&
	    verify_per_group_options(td, jobname))
		goto err;

	/* 속도 제한(rate) 설정 */
	if (setup_rate(td))
		goto err;

	/* ramp 기간 초기화 (워밍업 시간) */
	if (td_ramp_period_init(td))
		goto err;

	/* ===== 레이턴시 로그 설정 ===== */
	if (o->write_lat_log) {
		struct log_params p = {
			.td = td,
			.avg_msec = o->log_avg_msec,          /* 평균 계산 간격 */
			.hist_msec = o->log_hist_msec,          /* 히스토그램 간격 */
			.hist_coarseness = o->log_hist_coarseness, /* 히스토그램 정밀도 */
			.log_type = IO_LOG_TYPE_LAT,            /* 로그 타입: 레이턴시 */
			.log_offset = o->log_offset,            /* 오프셋 기록 여부 */
			.log_prio = o->log_prio,                /* 우선순위 기록 여부 */
			.log_issue_time = o->log_issue_time,    /* 발행 시간 기록 여부 */
			.log_gz = o->log_gz,                    /* gzip 압축 여부 */
			.log_gz_store = o->log_gz_store,        /* 압축 저장 여부 */
		};
		const char *pre = make_log_name(o->lat_log_file, o->name);
		const char *suf;

		/* log_issue_time은 log_offset과 함께 사용해야 함 */
		if (o->log_issue_time && !o->log_offset) {
			log_err("fio: log_issue_time option requires write_lat_log and log_offset options\n");
			goto err;
		}

		/* 파일 확장자 결정 (압축 저장 시 .fz) */
		if (p.log_gz_store)
			suf = "log.fz";
		else
			suf = "log";

		/* 전체 레이턴시 로그 설정 */
		if (!o->disable_lat) {
			gen_log_name(logname, sizeof(logname), "lat", pre,
				     td->thread_number, suf, o->per_job_logs);
			setup_log(&td->lat_log, &p, logname);
		}

		/* 제출 레이턴시(slat) 로그 설정 */
		if (!o->disable_slat) {
			gen_log_name(logname, sizeof(logname), "slat", pre,
				     td->thread_number, suf, o->per_job_logs);
			setup_log(&td->slat_log, &p, logname);
		}

		/* 완료 레이턴시(clat) 로그 설정 */
		if (!o->disable_clat) {
			gen_log_name(logname, sizeof(logname), "clat", pre,
				     td->thread_number, suf, o->per_job_logs);
			setup_log(&td->clat_log, &p, logname);
		}

	} else if (o->log_issue_time) {
		/* write_lat_log 없이 log_issue_time만 설정하면 에러 */
		log_err("fio: log_issue_time option requires write_lat_log and log_offset options\n");
		goto err;
	}

	/* ===== 히스토그램 로그 설정 ===== */
	if (o->write_hist_log) {
		struct log_params p = {
			.td = td,
			.avg_msec = o->log_avg_msec,
			.hist_msec = o->log_hist_msec,
			.hist_coarseness = o->log_hist_coarseness,
			.log_type = IO_LOG_TYPE_HIST,  /* 로그 타입: 히스토그램 */
			.log_offset = o->log_offset,
			.log_prio = o->log_prio,
			.log_issue_time = o->log_issue_time,
			.log_gz = o->log_gz,
			.log_gz_store = o->log_gz_store,
		};
		const char *pre = make_log_name(o->hist_log_file, o->name);
		const char *suf;

#ifndef CONFIG_ZLIB
		/* 클라이언트/서버 모드에서 히스토그램 로그는 zlib 필수 */
		if (is_backend) {
			log_err("fio: --write_hist_log requires zlib in client/server mode\n");
			goto err;
		}
#endif

		if (p.log_gz_store)
			suf = "log.fz";
		else
			suf = "log";

		gen_log_name(logname, sizeof(logname), "clat_hist", pre,
				td->thread_number, suf, o->per_job_logs);
		setup_log(&td->clat_hist_log, &p, logname);
	}

	/* ===== 대역폭(bandwidth) 로그 설정 ===== */
	if (o->write_bw_log) {
		struct log_params p = {
			.td = td,
			.avg_msec = o->log_avg_msec,
			.hist_msec = o->log_hist_msec,
			.hist_coarseness = o->log_hist_coarseness,
			.log_type = IO_LOG_TYPE_BW,  /* 로그 타입: 대역폭 */
			.log_offset = o->log_offset,
			.log_prio = o->log_prio,
			.log_issue_time = o->log_issue_time,
			.log_gz = o->log_gz,
			.log_gz_store = o->log_gz_store,
		};
		const char *pre = make_log_name(o->bw_log_file, o->name);
		const char *suf;

		/* bw_avg_time이 설정되었으면 log_avg_msec와의 최솟값 사용 */
		if (fio_option_is_set(o, bw_avg_time))
			p.avg_msec = min(o->log_avg_msec, o->bw_avg_time);
		else
			o->bw_avg_time = p.avg_msec;

		p.hist_msec = o->log_hist_msec;
		p.hist_coarseness = o->log_hist_coarseness;

		if (p.log_gz_store)
			suf = "log.fz";
		else
			suf = "log";

		gen_log_name(logname, sizeof(logname), "bw", pre,
				td->thread_number, suf, o->per_job_logs);
		setup_log(&td->bw_log, &p, logname);
	}

	/* ===== IOPS 로그 설정 ===== */
	if (o->write_iops_log) {
		struct log_params p = {
			.td = td,
			.avg_msec = o->log_avg_msec,
			.hist_msec = o->log_hist_msec,
			.hist_coarseness = o->log_hist_coarseness,
			.log_type = IO_LOG_TYPE_IOPS,  /* 로그 타입: IOPS */
			.log_offset = o->log_offset,
			.log_prio = o->log_prio,
			.log_issue_time = o->log_issue_time,
			.log_gz = o->log_gz,
			.log_gz_store = o->log_gz_store,
		};
		const char *pre = make_log_name(o->iops_log_file, o->name);
		const char *suf;

		/* iops_avg_time이 설정되었으면 log_avg_msec와의 최솟값 사용 */
		if (fio_option_is_set(o, iops_avg_time))
			p.avg_msec = min(o->log_avg_msec, o->iops_avg_time);
		else
			o->iops_avg_time = p.avg_msec;

		p.hist_msec = o->log_hist_msec;
		p.hist_coarseness = o->log_hist_coarseness;

		if (p.log_gz_store)
			suf = "log.fz";
		else
			suf = "log";

		gen_log_name(logname, sizeof(logname), "iops", pre,
				td->thread_number, suf, o->per_job_logs);
		setup_log(&td->iops_log, &p, logname);
	}

	/* job 이름이 아직 설정되지 않았으면 jobname으로 설정 */
	if (!o->name)
		o->name = strdup(jobname);

	/* ===== 일반 출력 형식에서 job 정보 출력 ===== */
	if (output_format & FIO_OUTPUT_NORMAL) {
		if (!job_add_num) {
			/* 첫 번째 서브잡일 때만 정보 출력 */
			if (is_backend && !recursed)
				fio_server_send_add_job(td);  /* 서버 모드: 클라이언트에게 job 추가 알림 */

			/* IO 작업이 있는 엔진만 정보 출력 */
			if (!td_ioengine_flagged(td, FIO_NOIO)) {
				char *c1, *c2, *c3, *c4;
				char *c5 = NULL, *c6 = NULL;
				int i2p = is_power_of_2(o->kb_base);
				struct buf_output out;

				/* 블록 크기를 사람이 읽기 좋은 형식으로 변환 */
				c1 = num2str(o->min_bs[DDIR_READ], o->sig_figs, 1, i2p, N2S_BYTE);
				c2 = num2str(o->max_bs[DDIR_READ], o->sig_figs, 1, i2p, N2S_BYTE);
				c3 = num2str(o->min_bs[DDIR_WRITE], o->sig_figs, 1, i2p, N2S_BYTE);
				c4 = num2str(o->max_bs[DDIR_WRITE], o->sig_figs, 1, i2p, N2S_BYTE);

				/* 순차/랜덤 블록 크기가 아닌 경우에만 트림 크기 표시 */
				if (!o->bs_is_seq_rand) {
					c5 = num2str(o->min_bs[DDIR_TRIM], o->sig_figs, 1, i2p, N2S_BYTE);
					c6 = num2str(o->max_bs[DDIR_TRIM], o->sig_figs, 1, i2p, N2S_BYTE);
				}

				/* job 요약 정보 출력:
				 * "jobname: (g=그룹ID): rw=방향, bs=(R) 읽기크기, (W) 쓰기크기, ..." */
				buf_output_init(&out);
				__log_buf(&out, "%s: (g=%d): rw=%s, ", td->o.name,
							td->groupid,
							ddir_str(o->td_ddir));

				if (o->bs_is_seq_rand)
					__log_buf(&out, "bs=(R) %s-%s, (W) %s-%s, bs_is_seq_rand, ",
							c1, c2, c3, c4);
				else
					__log_buf(&out, "bs=(R) %s-%s, (W) %s-%s, (T) %s-%s, ",
							c1, c2, c3, c4, c5, c6);

				__log_buf(&out, "ioengine=%s, iodepth=%u\n",
						td->io_ops->name, o->iodepth);
				log_info_buf(out.buf, out.buflen);
				buf_output_free(&out);

				/* 문자열 메모리 해제 */
				free(c1);
				free(c2);
				free(c3);
				free(c4);
				free(c5);
				free(c6);
			}
		} else if (job_add_num == 1)
			/* 서브잡이 여러 개이면 "..."으로 표시 */
			log_info("...\n");
	}

	/* steady state(정상 상태) 감지 초기화 */
	if (td_steadystate_init(td))
		goto err;

	/* blktrace 로그 병합 */
	if (o->merge_blktrace_file && !merge_blktrace_iologs(td))
		goto err;

	/* 병합만 수행 모드이면 job을 해제하고 반환 */
	if (merge_blktrace_only) {
		put_job(td);
		return 0;
	}

	/*
	 * recurse add identical jobs, clear numjobs and stonewall options
	 * as they don't apply to sub-jobs
	 */
	/* ===== numjobs 처리: 동일한 job을 여러 개 재귀적으로 추가 ===== */
	numjobs = o->numjobs;
	while (--numjobs) {
		/* 현재 td를 템플릿으로 새 job 생성 */
		struct thread_data *td_new = get_new_job(false, td, true, jobname);

		if (!td_new)
			goto err;

		/* 서브잡에서는 numjobs, stonewall, new_group 옵션 제거 */
		td_new->o.numjobs = 1;
		td_new->o.stonewall = 0;
		td_new->o.new_group = 0;
		td_new->subjob_number = numjobs;  /* 서브잡 번호 설정 */
		td_new->o.ss_dur = o->ss_dur * 1000000l;  /* steady state 기간 (마이크로초) */
		td_new->o.ss_limit = o->ss_limit;

		/* 파일이 자동 할당된 경우 서브잡의 파일 목록을 초기화
		 * (각 서브잡이 독립적인 파일을 사용하도록) */
		if (file_alloced) {
			if (td_new->files) {
				struct fio_file *f;
				for_each_file(td_new, f, i)
					fio_file_free(f);
				free(td_new->files);
				td_new->files = NULL;
			}
			td_new->files_index = 0;
			td_new->files_size = 0;
			if (td_new->o.filename) {
				free(td_new->o.filename);
				td_new->o.filename = NULL;
			}
		}

		/* 서브잡을 재귀적으로 add_job() 호출하여 추가 */
		if (add_job(td_new, jobname, numjobs, 1, client_type))
			goto err;
	}

	return 0;
err:
	/* 에러 발생 시 job 해제 */
	put_job(td);
	return -1;
}

/*
 * Parse as if 'o' was a command line
 */
/*
 * add_job_opts() - 문자열 배열을 명령줄처럼 파싱하여 job 추가 함수
 *
 * 역할: 명령줄 옵션 형식의 문자열 배열을 파싱하여 job을 생성합니다.
 *       "name" 옵션이 나오면 새 job 섹션으로 간주합니다.
 * 파라미터:
 *   o           - 옵션 문자열 배열 (NULL 종료)
 *   client_type - 클라이언트 타입
 * 반환값: 없음
 */
void add_job_opts(const char **o, int client_type)
{
	struct thread_data *td, *td_parent;
	int i, in_global = 1;   /* in_global: 현재 global 섹션 파싱 중인지 여부 */
	char jobname[32];

	i = 0;
	td_parent = td = NULL;
	/* 옵션 배열을 순회하며 파싱 */
	while (o[i]) {
		/* "name" 옵션을 만나면 새 job 섹션 시작 */
		if (!strncmp(o[i], "name", 4)) {
			in_global = 0;
			/* 이전 job이 있으면 등록 */
			if (td)
				add_job(td, jobname, 0, 0, client_type);
			td = NULL;
			sprintf(jobname, "%s", o[i] + 5);  /* "name=xxx"에서 이름 추출 */
		}
		/* 글로벌 섹션에서 부모 td 초기화 */
		if (in_global && !td_parent)
			td_parent = get_new_job(true, &def_thread, false, jobname);
		/* job 섹션에서 새 td 생성 */
		else if (!in_global && !td) {
			if (!td_parent)
				td_parent = &def_thread;
			td = get_new_job(false, td_parent, false, jobname);
		}
		/* 글로벌/job 섹션에 따라 적절한 td에 옵션 파싱 */
		if (in_global)
			fio_options_parse(td_parent, (char **) &o[i], 1);
		else
			fio_options_parse(td, (char **) &o[i], 1);
		i++;
	}

	/* 마지막 job 등록 */
	if (td)
		add_job(td, jobname, 0, 0, client_type);
}

/*
 * skip_this_section() - 지정된 섹션을 건너뛸지 확인하는 함수
 *
 * 역할: --section 옵션으로 특정 섹션만 실행할 때,
 *       현재 섹션이 건너뛰어야 할 대상인지 확인합니다.
 * 파라미터:
 *   name - 섹션 이름
 * 반환값: 건너뛰어야 하면 1, 실행해야 하면 0
 */
static int skip_this_section(const char *name)
{
	int i;

	/* --section 옵션이 지정되지 않았으면 모든 섹션 실행 */
	if (!nr_job_sections)
		return 0;
	/* global 섹션은 항상 실행 */
	if (!strncmp(name, "global", 6))
		return 0;

	/* 지정된 섹션 목록에서 현재 섹션 이름 검색 */
	for (i = 0; i < nr_job_sections; i++)
		if (!strcmp(job_sections[i], name))
			return 0;  /* 목록에 있으면 실행 */

	return 1;  /* 목록에 없으면 건너뜀 */
}

/*
 * is_empty_or_comment() - 빈 줄 또는 주석 줄 확인 함수
 *
 * 역할: 줄이 비어있거나 ';' 또는 '#'으로 시작하는 주석인지 확인합니다.
 * 파라미터:
 *   line - 확인할 줄 문자열
 * 반환값: 빈 줄이나 주석이면 1, 유효한 내용이면 0
 */
static int is_empty_or_comment(char *line)
{
	unsigned int i;

	for (i = 0; i < strlen(line); i++) {
		if (line[i] == ';')      /* ; 주석 */
			return 1;
		if (line[i] == '#')      /* # 주석 */
			return 1;
		/* 공백/제어 문자가 아닌 유효한 문자를 발견하면 내용 있음 */
		if (!isspace((int) line[i]) && !iscntrl((int) line[i]))
			return 0;
	}

	return 1;  /* 줄 전체가 공백/제어 문자이면 빈 줄 */
}

/*
 * This is our [ini] type file parser.
 */
/*
 * __parse_jobs_ini() - INI 형식 job 파일 파서 (핵심 함수)
 *
 * 역할: INI 형식의 job 파일(.fio)을 파싱하여 job들을 생성합니다.
 *       [global] 섹션은 기본 옵션, [jobname] 섹션은 개별 job을 정의합니다.
 *       include 지시자로 다른 파일을 포함할 수 있습니다.
 *
 * 파라미터:
 *   td             - 중첩(nested) 호출 시 사용할 thread_data (최초 호출 시 NULL)
 *   file           - 파일 경로 또는 버퍼 포인터
 *   is_buf         - file이 메모리 버퍼인지 여부
 *   stonewall_flag - 첫 번째 job에 stonewall 설정 여부
 *   type           - 클라이언트 타입
 *   nested         - 중첩(include) 파싱 여부
 *   name           - 현재 섹션 이름 버퍼 (중첩 시 부모에서 전달)
 *   popts          - 옵션 배열 포인터의 포인터 (중첩 시 부모와 공유)
 *   aopts          - 할당된 옵션 배열 크기
 *   nopts          - 현재 옵션 수
 *
 * 반환값: 성공 시 0, 실패 시 1
 *
 * 파싱 흐름:
 *   1. 파일 열기 (stdin, 일반 파일, 또는 메모리 버퍼)
 *   2. 줄 단위로 읽기
 *   3. [섹션이름] 발견 시 → get_new_job()으로 새 td 생성
 *   4. 옵션 줄 수집
 *   5. include 지시자 발견 시 재귀 호출
 *   6. 섹션 끝에서 fio_options_parse()로 옵션 파싱
 *   7. add_job()으로 job 등록
 */
static int __parse_jobs_ini(struct thread_data *td,
		char *file, int is_buf, int stonewall_flag, int type,
		int nested, char *name, char ***popts, int *aopts, int *nopts)
{
	bool global = false;           /* 현재 [global] 섹션인지 여부 */
	bool stdin_occupied = false;   /* stdin이 이미 사용 중인지 여부 */
	char *string;                  /* 줄 읽기 버퍼 */
	FILE *f;
	char *p;
	int ret = 0, stonewall;
	int first_sect = 1;            /* 첫 번째 섹션인지 여부 */
	int skip_fgets = 0;            /* 다음 fgets 건너뛰기 (이미 읽은 줄이 있는 경우) */
	int inside_skip = 0;           /* 현재 건너뛰는 섹션 내부인지 여부 */
	char **opts;                   /* 옵션 문자열 배열 */
	int i, alloc_opts, num_opts;   /* 옵션 배열 관리 변수 */

	dprint(FD_PARSE, "Parsing ini file %s\n", file);
	/* 중첩이 아닌 최초 호출에서는 td가 NULL이어야 함 */
	assert(td || !nested);

	/* 파일 열기: 버퍼이면 NULL, "-"이면 stdin, 그 외 파일 열기 */
	if (is_buf)
		f = NULL;
	else {
		if (!strcmp(file, "-")) {
			f = stdin;
			stdin_occupied = true;
		} else
			f = fopen(file, "r");

		if (!f) {
			int __err = errno;

			log_err("fio: unable to open '%s' job file\n", file);
			if (td)
				td_verror(td, __err, "job file open");
			return 1;
		}
	}

	/* 줄 읽기 버퍼 할당 (OPT_LEN_MAX 크기) */
	string = malloc(OPT_LEN_MAX);

	/*
	 * it's really 256 + small bit, 280 should suffice
	 */
	/* 섹션 이름 버퍼 (최초 호출 시에만 할당) */
	if (!nested) {
		name = calloc(1, 280);
	}

	/* 옵션 배열 초기화 (중첩 호출 시 부모의 배열 사용) */
	opts = NULL;
	if (nested && popts) {
		opts = *popts;
		alloc_opts = *aopts;
		num_opts = *nopts;
	}

	/* 옵션 배열이 없으면 새로 할당 */
	if (!opts) {
		alloc_opts = 8;
		opts = malloc(sizeof(char *) * alloc_opts);
		num_opts = 0;
	}

	stonewall = stonewall_flag;
	/* ===== 메인 파싱 루프 ===== */
	do {
		/*
		 * if skip_fgets is set, we already have loaded a line we
		 * haven't handled.
		 */
		/* skip_fgets가 설정되어 있으면 이미 읽은 줄을 사용 */
		if (!skip_fgets) {
			if (is_buf)
				p = strsep(&file, "\n");  /* 버퍼에서 줄 분리 */
			else
				p = fgets(string, OPT_LEN_MAX, f);  /* 파일에서 줄 읽기 */
			if (!p)
				break;  /* EOF 도달 */
		}

		skip_fgets = 0;
		strip_blank_front(&p);  /* 앞쪽 공백 제거 */
		strip_blank_end(p);     /* 뒤쪽 공백 제거 */

		dprint(FD_PARSE, "%s\n", p);
		/* 빈 줄이나 주석은 건너뜀 */
		if (is_empty_or_comment(p))
			continue;

		/* ===== 섹션 헤더 [이름] 처리 (최초 호출에서만) ===== */
		if (!nested) {
			/* [섹션이름] 형식 파싱 */
			if (sscanf(p, "[%255[^\n]]", name) != 1) {
				/* 섹션 헤더가 아닌데 건너뛰는 섹션 내부이면 무시 */
				if (inside_skip)
					continue;

				/* 섹션 외부에서 옵션이 나오면 에러 */
				log_err("fio: option <%s> outside of "
					"[] job section\n", p);
				ret = 1;
				break;
			}

			/* 닫는 괄호 ']' 제거 */
			name[strlen(name) - 1] = '\0';

			/* --section 옵션에 의해 건너뛰어야 할 섹션인지 확인 */
			if (skip_this_section(name)) {
				inside_skip = 1;
				continue;
			} else
				inside_skip = 0;

			dprint(FD_PARSE, "Parsing section [%s]\n", name);

			/* "global"로 시작하면 글로벌 섹션 */
			global = !strncmp(name, "global", 6);

			/* --showcmd 모드에서 섹션 정보 출력 */
			if (dump_cmdline) {
				if (first_sect)
					log_info("fio ");
				if (!global)
					log_info("--name=%s ", name);
				first_sect = 0;
			}

			/* 새 job을 위한 thread_data 생성 */
			td = get_new_job(global, &def_thread, false, name);
			if (!td) {
				ret = 1;
				break;
			}

			/*
			 * Separate multiple job files by a stonewall
			 */
			/* 여러 job 파일 사이에 stonewall 삽입 (동기화 장벽) */
			if (!global && stonewall) {
				td->o.stonewall = stonewall;
				stonewall = 0;
			}

			/* 새 섹션의 옵션 배열 초기화 */
			num_opts = 0;
			memset(opts, 0, alloc_opts * sizeof(char *));
		}
		else
			skip_fgets = 1;  /* 중첩 호출에서는 첫 줄을 다시 읽지 않음 */

		/* ===== 섹션 내부 옵션 읽기 루프 ===== */
		while (1) {
			if (!skip_fgets) {
				if (is_buf)
					p = strsep(&file, "\n");
				else
					p = fgets(string, OPT_LEN_MAX, f);
				if (!p)
					break;  /* EOF */
				dprint(FD_PARSE, "%s", p);
			}
			else
				skip_fgets = 0;

			/* 빈 줄/주석 건너뜀 */
			if (is_empty_or_comment(p))
				continue;

			strip_blank_front(&p);

			/*
			 * new section, break out and make sure we don't
			 * fgets() a new line at the top.
			 */
			/* 새 섹션 헤더 '[' 발견 시 현재 루프 종료 */
			if (p[0] == '[') {
				/* 중첩 파일에서는 새 섹션을 허용하지 않음 */
				if (nested) {
					log_err("No new sections in included files\n");
					ret = 1;
					goto out;
				}

				skip_fgets = 1;  /* 이 줄은 외부 루프에서 다시 처리 */
				break;
			}

			strip_blank_end(p);

			/* ===== include 지시자 처리 ===== */
			if (!strncmp(p, "include", strlen("include"))) {
				char *filename = p + strlen("include") + 1,
					*ts, *full_fn = NULL;

				/*
				 * Allow for the include filename
				 * specification to be relative.
				 */
				/* 상대 경로 지원: 현재 파일의 디렉토리를 기준으로 경로 해석 */
				if (access(filename, F_OK) &&
				    (ts = strrchr(file, '/'))) {
					if (asprintf(&full_fn, "%.*s%s",
						 (int)(ts - file + 1), file,
						 filename) < 0) {
						ret = ENOMEM;
						break;
					}
					filename = full_fn;
				}

				/* 포함된 파일을 재귀적으로 파싱 */
				ret = __parse_jobs_ini(td, filename, is_buf,
						       stonewall_flag, type, 1,
						       name, &opts,
						       &alloc_opts, &num_opts);

				if (ret) {
					log_err("Error %d while parsing "
						"include file %s\n",
						ret, filename);
				}

				if (full_fn)
					free(full_fn);

				if (ret)
					break;

				continue;
			}

			/* 옵션 배열이 가득 차면 2배로 확장 */
			if (num_opts == alloc_opts) {
				alloc_opts <<= 1;
				opts = realloc(opts,
						alloc_opts * sizeof(char *));
			}

			/* 옵션 문자열을 배열에 복제하여 저장 */
			opts[num_opts] = strdup(p);
			num_opts++;
		}

		/* 중첩 호출에서는 옵션을 부모에게 반환 */
		if (nested) {
			*popts = opts;
			*aopts = alloc_opts;
			*nopts = num_opts;
			goto out;
		}

		/* 수집된 옵션들을 한번에 파싱 */
		ret = fio_options_parse(td, opts, num_opts);

		/* stdin 중복 사용 확인 (read_iolog_file이 "-"인 경우) */
		if (!ret && td->o.read_iolog_file != NULL) {
			char *fname = get_name_by_idx(td->o.read_iolog_file,
						      td->subjob_number);
			if (!strcmp(fname, "-")) {
				if (stdin_occupied) {
					log_err("fio: only one user (read_iolog_file/job "
						"file) of stdin is permitted at once but "
						"more than one was found.\n");
					ret = 1;
				}
				stdin_occupied = true;
			}
		}
		/* 파싱 성공 시 job 등록, 실패 시 job 해제 */
		if (!ret) {
			if (dump_cmdline)
				dump_opt_list(td);

			ret = add_job(td, name, 0, 0, type);
		} else {
			log_err("fio: job %s dropped\n", name);
			put_job(td);
		}

		/* 옵션 문자열 메모리 해제 */
		for (i = 0; i < num_opts; i++)
			free(opts[i]);
		num_opts = 0;
	} while (!ret);

	/* --showcmd 모드에서 마지막 줄바꿈 */
	if (dump_cmdline)
		log_info("\n");

	/* --section 옵션으로 지정된 섹션 이름 메모리 해제 */
	i = 0;
	while (i < nr_job_sections) {
		free(job_sections[i]);
		i++;
	}

	free(job_sections);
	job_sections = NULL;
	nr_job_sections = 0;

	free(opts);
out:
	free(string);
	if (!nested)
		free(name);
	/* 파일이 stdin이 아니면 닫기 */
	if (!is_buf && f != stdin)
		fclose(f);
	return ret;
}

/*
 * parse_jobs_ini() - INI 파일 파싱 공개 인터페이스 함수
 *
 * 역할: __parse_jobs_ini()의 공개 래퍼 함수입니다.
 *       중첩 관련 파라미터를 NULL/0으로 설정하여 최초 호출을 수행합니다.
 * 파라미터:
 *   file           - 파일 경로 또는 버퍼
 *   is_buf         - 버퍼 여부
 *   stonewall_flag - stonewall 설정 여부
 *   type           - 클라이언트 타입
 * 반환값: 성공 시 0, 실패 시 1
 */
int parse_jobs_ini(char *file, int is_buf, int stonewall_flag, int type)
{
	return __parse_jobs_ini(NULL, file, is_buf, stonewall_flag, type,
			0, NULL, NULL, NULL, NULL);
}

/*
 * fill_def_thread() - 기본 스레드 초기화 함수
 *
 * 역할: def_thread(글로벌 기본 설정용 thread_data)를 0으로 초기화하고
 *       기본 옵션값을 채웁니다.
 * 파라미터: 없음
 * 반환값: 항상 0
 */
static int fill_def_thread(void)
{
	/* def_thread 구조체를 0으로 초기화 */
	memset(&def_thread, 0, sizeof(def_thread));
	/* 옵션 리스트 초기화 */
	INIT_FLIST_HEAD(&def_thread.opt_list);

	/* 현재 프로세스의 CPU 친화도(affinity)를 기본값으로 설정 */
	fio_getaffinity(getpid(), &def_thread.o.cpumask);
	/* 에러 덤프 기본 활성화 */
	def_thread.o.error_dump = 1;

	/*
	 * fill default options
	 */
	/* 모든 옵션을 기본값으로 채움 */
	fio_fill_default_options(&def_thread);
	return 0;
}

/*
 * show_debug_categories() - 디버그 카테고리 출력 함수
 *
 * 역할: --debug=help 시 사용 가능한 디버그 카테고리 목록을 출력합니다.
 * 파라미터: 없음
 * 반환값: 없음
 */
static void show_debug_categories(void)
{
#ifdef FIO_INC_DEBUG
	const struct debug_level *dl = &debug_levels[0];
	int curlen, first = 1;

	curlen = 0;
	/* 디버그 레벨 배열을 순회하며 이름 출력 (80자 줄바꿈) */
	while (dl->name) {
		int has_next = (dl + 1)->name != NULL;

		/* 첫 줄이거나 80자를 초과하면 줄바꿈 */
		if (first || curlen + strlen(dl->name) >= 80) {
			if (!first) {
				printf("\n");
				curlen = 0;
			}
			curlen += printf("\t\t\t%s", dl->name);
			curlen += 3 * (8 - 1);
			if (has_next)
				curlen += printf(",");
		} else {
			curlen += printf("%s", dl->name);
			if (has_next)
				curlen += printf(",");
		}
		dl++;
		first = 0;
	}
	printf("\n");
#endif
}

/*
 * Following options aren't printed by usage().
 * --append-terse - Equivalent to --output-format=terse, see f6a7df53.
 * --latency-log - Deprecated option.
 */
/*
 * usage() - 사용법 출력 함수
 *
 * 역할: fio의 명령줄 사용법 및 옵션 설명을 출력합니다.
 * 파라미터:
 *   name - 프로그램 이름 (argv[0])
 * 반환값: 없음
 */
static void usage(const char *name)
{
	printf("%s\n", fio_version_string);
	printf("%s [options] [job options] <job file(s)>\n", name);
	printf("  --debug=options\tEnable debug logging. May be one/more of:\n");
	show_debug_categories();
	printf("  --parse-only\t\tParse options only, don't start any IO\n");
	printf("  --merge-blktrace-only\tMerge blktraces only, don't start any IO\n");
	printf("  --output\t\tWrite output to file\n");
	printf("  --bandwidth-log\tGenerate aggregate bandwidth logs\n");
	printf("  --minimal\t\tMinimal (terse) output\n");
	printf("  --output-format=type\tOutput format (terse,json,json+,normal)\n");
	printf("  --terse-version=type\tSet terse version output format"
		" (default 3, or 2 or 4 or 5)\n");
	printf("  --version\t\tPrint version info and exit\n");
	printf("  --help\t\tPrint this page\n");
	printf("  --cpuclock-test\tPerform test/validation of CPU clock\n");
	printf("  --crctest=[type]\tTest speed of checksum functions\n");
	printf("  --cmdhelp=cmd\t\tPrint command help, \"all\" for all of"
		" them\n");
	printf("  --enghelp=engine\tPrint ioengine help, or list"
		" available ioengines\n");
	printf("  --enghelp=engine,cmd\tPrint help for an ioengine"
		" cmd\n");
	printf("  --showcmd\t\tTurn a job file into command line options\n");
	printf("  --eta=when\t\tWhen ETA estimate should be printed\n");
	printf("            \t\tMay be \"always\", \"never\" or \"auto\"\n");
	printf("  --eta-newline=t\tForce a new line for every 't'");
	printf(" period passed\n");
	printf("  --status-interval=t\tForce full status dump every");
	printf(" 't' period passed\n");
	printf("  --readonly\t\tTurn on safety read-only checks, preventing"
		" writes\n");
	printf("  --section=name\tOnly run specified section in job file,"
		" multiple sections can be specified\n");
	printf("  --alloc-size=kb\tSet smalloc pool to this size in kb"
		" (def 16384)\n");
	printf("  --warnings-fatal\tFio parser warnings are fatal\n");
	printf("  --max-jobs=nr\t\tMaximum number of threads/processes to support\n");
	printf("  --server=args\t\tStart a backend fio server\n");
	printf("  --daemonize=pidfile\tBackground fio server, write pid to file\n");
	printf("  --client=hostname\tTalk to remote backend(s) fio server at hostname\n");
	printf("  --remote-config=file\tTell fio server to load this local job file\n");
	printf("  --idle-prof=option\tReport cpu idleness on a system or percpu basis\n"
		"\t\t\t(option=system,percpu) or run unit work\n"
		"\t\t\tcalibration only (option=calibrate)\n");
#ifdef CONFIG_ZLIB
	printf("  --inflate-log=log\tInflate and output compressed log\n");
#endif
	printf("  --trigger-file=file\tExecute trigger cmd when file exists\n");
	printf("  --trigger-timeout=t\tExecute trigger at this time\n");
	printf("  --trigger=cmd\t\tSet this command as local trigger\n");
	printf("  --trigger-remote=cmd\tSet this command as remote trigger\n");
	printf("  --aux-path=path\tUse this path for fio state generated files\n");
	printf("\nFio was written by Jens Axboe <axboe@kernel.dk>\n");
}

#ifdef FIO_INC_DEBUG
/*
 * 디버그 레벨 정의 배열 - 각 디버그 카테고리의 이름, 설명, 비트 시프트값
 * --debug 옵션에서 참조됩니다.
 */
const struct debug_level debug_levels[] = {
	{ .name = "process",
	  .help = "Process creation/exit logging",       /* 프로세스 생성/종료 로깅 */
	  .shift = FD_PROCESS,
	},
	{ .name = "file",
	  .help = "File related action logging",         /* 파일 관련 작업 로깅 */
	  .shift = FD_FILE,
	},
	{ .name = "io",
	  .help = "IO and IO engine action logging (offsets, queue, completions, etc)", /* IO 및 IO 엔진 동작 로깅 */
	  .shift = FD_IO,
	},
	{ .name = "mem",
	  .help = "Memory allocation/freeing logging",   /* 메모리 할당/해제 로깅 */
	  .shift = FD_MEM,
	},
	{ .name = "blktrace",
	  .help = "blktrace action logging",             /* blktrace 동작 로깅 */
	  .shift = FD_BLKTRACE,
	},
	{ .name = "verify",
	  .help = "IO verification action logging",      /* IO 검증 동작 로깅 */
	  .shift = FD_VERIFY,
	},
	{ .name = "random",
	  .help = "Random generation logging",           /* 난수 생성 로깅 */
	  .shift = FD_RANDOM,
	},
	{ .name = "parse",
	  .help = "Parser logging",                      /* 파서 로깅 */
	  .shift = FD_PARSE,
	},
	{ .name = "diskutil",
	  .help = "Disk utility logging actions",        /* 디스크 유틸리티 로깅 */
	  .shift = FD_DISKUTIL,
	},
	{ .name = "job",
	  .help = "Logging related to creating/destroying jobs", /* job 생성/삭제 로깅 */
	  .shift = FD_JOB,
	},
	{ .name = "mutex",
	  .help = "Mutex logging",                       /* 뮤텍스 로깅 */
	  .shift = FD_MUTEX
	},
	{ .name	= "profile",
	  .help = "Logging related to profiles",         /* 프로파일 로깅 */
	  .shift = FD_PROFILE,
	},
	{ .name = "time",
	  .help = "Logging related to time keeping functions", /* 시간 관련 함수 로깅 */
	  .shift = FD_TIME,
	},
	{ .name = "net",
	  .help = "Network logging",                     /* 네트워크 로깅 */
	  .shift = FD_NET,
	},
	{ .name = "rate",
	  .help = "Rate logging",                        /* 속도 제한 로깅 */
	  .shift = FD_RATE,
	},
	{ .name = "compress",
	  .help = "Log compression logging",             /* 로그 압축 로깅 */
	  .shift = FD_COMPRESS,
	},
	{ .name = "steadystate",
	  .help = "Steady state detection logging",      /* 정상 상태 감지 로깅 */
	  .shift = FD_STEADYSTATE,
	},
	{ .name = "helperthread",
	  .help = "Helper thread logging",               /* 헬퍼 스레드 로깅 */
	  .shift = FD_HELPERTHREAD,
	},
	{ .name = "zbd",
	  .help = "Zoned Block Device logging",          /* 존 블록 디바이스 로깅 */
	  .shift = FD_ZBD,
	},
	{ .name = "sprandom",
	  .help = "SPRandom logging",                    /* SPRandom 로깅 */
	  .shift = FD_SPRANDOM,
	},
	{ .name = NULL, },  /* 배열 종료 표시 */
};

/*
 * set_debug() - 디버그 옵션 설정 함수
 *
 * 역할: --debug 옵션의 인수를 파싱하여 fio_debug 비트마스크를 설정합니다.
 *       쉼표로 구분된 여러 카테고리를 지정할 수 있습니다.
 *       "all"은 모든 카테고리를 활성화합니다.
 *       "?"/"help"는 사용 가능한 카테고리를 출력합니다.
 * 파라미터:
 *   string - 디버그 옵션 문자열 (예: "io,verify,random")
 * 반환값: 성공 시 0, 도움말 출력 시 1
 */
static int set_debug(const char *string)
{
	const struct debug_level *dl;
	char *p = (char *) string;
	char *opt;
	int i;

	if (!string)
		return 0;

	/* "?" 또는 "help"이면 사용 가능한 옵션 목록 출력 */
	if (!strcmp(string, "?") || !strcmp(string, "help")) {
		log_info("fio: dumping debug options:");
		for (i = 0; debug_levels[i].name; i++) {
			dl = &debug_levels[i];
			log_info("%s,", dl->name);
		}
		log_info("all\n");
		return 1;
	}

	/* 쉼표로 구분된 옵션을 하나씩 파싱 */
	while ((opt = strsep(&p, ",")) != NULL) {
		int found = 0;

		/* "all"이면 모든 디버그 비트 활성화 */
		if (!strncmp(opt, "all", 3)) {
			log_info("fio: set all debug options\n");
			fio_debug = ~0UL;
			continue;
		}

		/* 디버그 레벨 배열에서 일치하는 이름 검색 */
		for (i = 0; debug_levels[i].name; i++) {
			dl = &debug_levels[i];
			found = !strncmp(opt, dl->name, strlen(dl->name));
			if (!found)
				continue;

			/* "job" 카테고리는 특별: 콜론 뒤에 job 번호 지정 필요 */
			if (dl->shift == FD_JOB) {
				opt = strchr(opt, ':');
				if (!opt) {
					log_err("fio: missing job number\n");
					break;
				}
				opt++;
				fio_debug_jobno = atoi(opt);
				log_info("fio: set debug jobno %d\n",
							fio_debug_jobno);
			} else {
				/* 해당 카테고리의 디버그 비트 활성화 */
				log_info("fio: set debug option %s\n", opt);
				fio_debug |= (1UL << dl->shift);
			}
			break;
		}

		if (!found)
			log_err("fio: debug mask %s not found\n", opt);
	}
	return 0;
}
#else
/* 디버그 트레이싱이 빌드에 포함되지 않은 경우 */
static int set_debug(const char *string)
{
	log_err("fio: debug tracing not included in build\n");
	return 1;
}
#endif

/*
 * fio_options_fill_optstring() - getopt 옵션 문자열 생성 함수
 *
 * 역할: l_opts 배열에서 getopt용 짧은 옵션 문자열(optstring)을 생성합니다.
 *       required_argument이면 ':', optional_argument이면 '::'를 추가합니다.
 * 파라미터: 없음
 * 반환값: 없음 (결과는 전역 변수 cmd_optstr에 저장)
 */
static void fio_options_fill_optstring(void)
{
	char *ostr = cmd_optstr;
	int i, c;

	c = i = 0;
	/* l_opts 배열을 순회하며 옵션 문자 및 인수 표시자 추가 */
	while (l_opts[i].name) {
		ostr[c++] = l_opts[i].val;
		if (l_opts[i].has_arg == required_argument)
			ostr[c++] = ':';       /* 필수 인수 */
		else if (l_opts[i].has_arg == optional_argument) {
			ostr[c++] = ':';       /* 선택적 인수 (:: 형식) */
			ostr[c++] = ':';
		}
		i++;
	}
	ostr[c] = '\0';
}

/*
 * client_flag_set() - 클라이언트 플래그 확인 함수
 *
 * 역할: 주어진 옵션 문자가 FIO_CLIENT_FLAG를 가지고 있는지 확인합니다.
 *       클라이언트/서버 모드에서 옵션 전달 여부를 결정합니다.
 * 파라미터:
 *   c - 옵션 문자
 * 반환값: FIO_CLIENT_FLAG가 설정되어 있으면 해당 값, 아니면 0
 */
static int client_flag_set(char c)
{
	int i;

	i = 0;
	while (l_opts[i].name) {
		int val = l_opts[i].val;

		if (c == (val & 0xff))
			return (val & FIO_CLIENT_FLAG);

		i++;
	}

	return 0;
}

/*
 * parse_cmd_client() - 클라이언트에게 옵션 전달 함수
 *
 * 역할: 클라이언트/서버 모드에서 원격 클라이언트에게 명령줄 옵션을 전달합니다.
 * 파라미터:
 *   client - 클라이언트 핸들
 *   opt    - 전달할 옵션 문자열
 * 반환값: 없음
 */
static void parse_cmd_client(void *client, char *opt)
{
	fio_client_add_cmd_option(client, opt);
}

/*
 * show_closest_option() - 가장 유사한 옵션 제안 함수
 *
 * 역할: 인식할 수 없는 옵션이 입력되었을 때, 편집 거리(Levenshtein distance)를
 *       기반으로 가장 유사한 옵션을 제안합니다.
 * 파라미터:
 *   name - 입력된 (잘못된) 옵션 이름
 * 반환값: 없음
 */
static void show_closest_option(const char *name)
{
	int best_option, best_distance;
	int i, distance;

	/* 앞의 '-' 문자 제거 */
	while (*name == '-')
		name++;

	best_option = -1;
	best_distance = INT_MAX;
	i = 0;
	/* 모든 옵션을 순회하며 편집 거리 계산 */
	while (l_opts[i].name) {
		distance = string_distance(name, l_opts[i].name);
		if (distance < best_distance) {
			best_distance = distance;
			best_option = i;
		}
		i++;
	}

	/* 충분히 유사한 옵션이 있으면 제안 */
	if (best_option != -1 && string_distance_ok(name, best_distance))
		log_err("Did you mean %s?\n", l_opts[best_option].name);
}

/*
 * parse_output_format() - 출력 형식 파싱 함수
 *
 * 역할: --output-format 옵션의 인수를 파싱하여 output_format 비트마스크를 설정합니다.
 *       쉼표로 구분된 여러 형식을 동시에 지정할 수 있습니다.
 * 파라미터:
 *   optarg - 형식 문자열 (예: "json+,normal")
 * 반환값: 성공 시 0, 실패 시 1
 */
static int parse_output_format(const char *optarg)
{
	char *p, *orig, *opt;
	int ret = 0;

	p = orig = strdup(optarg);

	output_format = 0;

	/* 쉼표로 구분하여 각 형식 처리 */
	while ((opt = strsep(&p, ",")) != NULL) {
		if (!strcmp(opt, "minimal") ||
		    !strcmp(opt, "terse") ||
		    !strcmp(opt, "csv"))
			output_format |= FIO_OUTPUT_TERSE;     /* 간결 형식 */
		else if (!strcmp(opt, "json"))
			output_format |= FIO_OUTPUT_JSON;       /* JSON 형식 */
		else if (!strcmp(opt, "json+"))
			output_format |= (FIO_OUTPUT_JSON | FIO_OUTPUT_JSON_PLUS); /* JSON+ 형식 (확장) */
		else if (!strcmp(opt, "normal"))
			output_format |= FIO_OUTPUT_NORMAL;     /* 일반 형식 */
		else {
			log_err("fio: invalid output format %s\n", opt);
			ret = 1;
			break;
		}
	}

	free(orig);
	return ret;
}

/*
 * parse_cmd_line() - 명령줄 인수 파싱 함수
 *
 * 역할: fio의 전체 명령줄 인수(argc, argv)를 파싱합니다.
 *       getopt_long_only()를 사용하여 긴 옵션과 짧은 옵션을 모두 처리합니다.
 *       이 함수에서 처리되는 옵션은 fio 자체의 동작을 제어하는 것들입니다.
 *       job 파일 경로는 ini_file 배열에 수집됩니다.
 *
 * 파라미터:
 *   argc        - 인수 개수
 *   argv        - 인수 문자열 배열
 *   client_type - 클라이언트 타입
 *
 * 반환값: 수집된 ini 파일 개수 (ini_idx)
 *
 * 주요 처리 옵션:
 *   -o: 출력 파일 지정
 *   -b: 대역폭 로그 활성화
 *   -F: 출력 형식 지정
 *   -d: 디버그 옵션
 *   -S: 서버 모드
 *   -C: 클라이언트 모드
 *   FIO_GETOPT_JOB: job 옵션 (--name, --ioengine 등)
 *   FIO_GETOPT_IOENGINE: IO 엔진 전용 옵션
 */
int parse_cmd_line(int argc, char *argv[], int client_type)
{
	struct thread_data *td = NULL;     /* 현재 파싱 중인 job의 td */
	int c, ini_idx = 0, lidx, ret = 0, do_exit = 0, exit_val = 0;
	char *ostr = cmd_optstr;           /* getopt 옵션 문자열 */
	char *pid_file = NULL;             /* 데몬 PID 파일 */
	void *cur_client = NULL;           /* 현재 클라이언트 핸들 */
	bool backend = false;              /* 서버 모드 플래그 */

	/*
	 * Reset optind handling, since we may call this multiple times
	 * for the backend.
	 */
	/* 백엔드에서 여러 번 호출될 수 있으므로 optind 초기화 */
	optind = 1;

	/* ===== 메인 옵션 파싱 루프 ===== */
	while ((c = getopt_long_only(argc, argv, ostr, l_opts, &lidx)) != -1) {
		/* 클라이언트 플래그가 설정된 옵션이면 클라이언트에게도 전달 */
		if ((c & FIO_CLIENT_FLAG) || client_flag_set(c)) {
			parse_cmd_client(cur_client, argv[optind - 1]);
			c &= ~FIO_CLIENT_FLAG;  /* 클라이언트 플래그 제거하여 실제 옵션값 추출 */
		}

		switch (c) {
		case 'a':
			/* --alloc-size: smalloc 풀 크기 설정 (KB 단위 → 바이트 변환) */
			smalloc_pool_size = atoi(optarg);
			smalloc_pool_size <<= 10;
			sinit();  /* smalloc 재초기화 */
			break;
		case 'l':
			/* --latency-log: 더 이상 사용되지 않는 옵션 */
			log_err("fio: --latency-log is deprecated. Use per-job latency log options.\n");
			do_exit++;
			exit_val = 1;
			break;
		case 'b':
			/* --bandwidth-log: 대역폭 로그 활성화 */
			write_bw_log = true;
			if (optarg)
				write_bw_log_name = optarg;
			else
				write_bw_log_name = "agg";  /* 기본 이름: "agg" (aggregate) */
			break;
		case 'o': {
			/* --output: 출력을 파일로 리다이렉트 */
			FILE *tmp;

			if (f_out && f_out != stdout)
				fclose(f_out);

			tmp = fopen(optarg, "w+");
			if (!tmp) {
				log_err("fio: output file open error: %s\n", strerror(errno));
				exit_val = 1;
				do_exit++;
				break;
			}
			f_err = f_out = tmp;  /* 표준 출력과 에러를 모두 파일로 리다이렉트 */
			break;
			}
		case 'm':
			/* --minimal: 간결(terse) 출력 형식 */
			output_format = FIO_OUTPUT_TERSE;
			break;
		case 'F':
			/* --output-format: 출력 형식 지정 */
			if (parse_output_format(optarg)) {
				log_err("fio: failed parsing output-format\n");
				exit_val = 1;
				do_exit++;
				break;
			}
			break;
		case 'f':
			/* --append-terse: terse 형식 추가 */
			output_format |= FIO_OUTPUT_TERSE;
			break;
		case 'h':
			/* --help: 도움말 출력 */
			did_arg = true;
			if (!cur_client) {
				usage(argv[0]);
				do_exit++;
			}
			break;
		case 'c':
			/* --cmdhelp: 명령어 도움말 */
			did_arg = true;
			if (!cur_client) {
				fio_show_option_help(optarg);
				do_exit++;
			}
			break;
		case 'i':
			/* --enghelp: IO 엔진 도움말 */
			did_arg = true;
			if (!cur_client) {
				exit_val = fio_show_ioengine_help(optarg);
				do_exit++;
			}
			break;
		case 's':
			/* --showcmd: job 파일을 명령줄 형식으로 출력 */
			did_arg = true;
			dump_cmdline = true;
			break;
		case 'r':
			/* --readonly: 읽기 전용 모드 */
			read_only = 1;
			break;
		case 'v':
			/* --version: 버전 정보 출력 */
			did_arg = true;
			if (!cur_client) {
				log_info("%s\n", fio_version_string);
				do_exit++;
			}
			break;
		case 'V':
			/* --terse-version: terse 출력 버전 지정 (2~5) */
			terse_version = atoi(optarg);
			if (!(terse_version >= 2 && terse_version <= 5)) {
				log_err("fio: bad terse version format\n");
				exit_val = 1;
				do_exit++;
			}
			break;
		case 'e':
			/* --eta: ETA 출력 모드 (always/never/auto) */
			if (!strcmp("always", optarg))
				eta_print = FIO_ETA_ALWAYS;
			else if (!strcmp("never", optarg))
				eta_print = FIO_ETA_NEVER;
			break;
		case 'E': {
			/* --eta-newline: ETA 줄바꿈 간격 */
			long long t = 0;

			if (check_str_time(optarg, &t, 1)) {
				log_err("fio: failed parsing eta time %s\n", optarg);
				exit_val = 1;
				do_exit++;
				break;
			}
			eta_new_line = t / 1000;
			if (!eta_new_line) {
				log_err("fio: eta new line time too short\n");
				exit_val = 1;
				do_exit++;
			}
			break;
			}
		case 'O': {
			/* --eta-interval: ETA 갱신 간격 */
			long long t = 0;

			if (check_str_time(optarg, &t, 1)) {
				log_err("fio: failed parsing eta interval %s\n", optarg);
				exit_val = 1;
				do_exit++;
				break;
			}
			eta_interval_msec = t / 1000;
			if (eta_interval_msec < DISK_UTIL_MSEC) {
				log_err("fio: eta interval time too short (%umsec min)\n", DISK_UTIL_MSEC);
				exit_val = 1;
				do_exit++;
			}
			break;
			}
		case 'd':
			/* --debug: 디버그 옵션 설정 */
			if (set_debug(optarg))
				do_exit++;
			break;
		case 'P':
			/* --parse-only: 파싱만 수행 */
			did_arg = true;
			parse_only = true;
			break;
		case 'x': {
			/* --section: 특정 섹션만 실행 */
			size_t new_size;

			/* "global"은 섹션 이름으로 지정할 수 없음 */
			if (!strcmp(optarg, "global")) {
				log_err("fio: can't use global as only "
					"section\n");
				do_exit++;
				exit_val = 1;
				break;
			}
			/* 섹션 이름 배열에 추가 */
			new_size = (nr_job_sections + 1) * sizeof(char *);
			job_sections = realloc(job_sections, new_size);
			job_sections[nr_job_sections] = strdup(optarg);
			nr_job_sections++;
			break;
			}
#ifdef CONFIG_ZLIB
		case 'X':
			/* --inflate-log: 압축된 로그 파일 해제 */
			exit_val = iolog_file_inflate(optarg);
			did_arg = true;
			do_exit++;
			break;
#endif
		case 'p':
			/* --profile: 실행 프로파일 설정 */
			did_arg = true;
			if (exec_profile)
				free(exec_profile);
			exec_profile = strdup(optarg);
			break;
		case FIO_GETOPT_JOB: {
			/* ===== job 옵션 처리 (--name, --rw, --bs 등) ===== */
			const char *opt = l_opts[lidx].name;
			char *val = optarg;

			/* "name" 옵션이 나오면 이전 job을 등록하고 새 job 시작 */
			if (!strncmp(opt, "name", 4) && td) {
				ret = add_job(td, td->o.name ?: "fio", 0, 0, client_type);
				if (ret)
					goto out_free;
				td = NULL;
				did_arg = true;
			}
			/* td가 없으면 새 job 생성 */
			if (!td) {
				int is_section = !strncmp(opt, "name", 4);
				int global = 0;

				/* "name" 옵션이 아니거나 값이 "global"이면 글로벌 */
				if (!is_section || !strncmp(val, "global", 6))
					global = 1;

				/* --section 옵션에 의해 건너뛰어야 할 섹션인지 확인 */
				if (is_section && skip_this_section(val))
					continue;

				/* 새 job을 위한 td 생성 및 IO 엔진 로드 */
				td = get_new_job(global, &def_thread, true, NULL);
				if (!td || ioengine_load(td)) {
					if (td) {
						put_job(td);
						td = NULL;
					}
					do_exit++;
					exit_val = 1;
					break;
				}
				/* IO 엔진 전용 옵션을 l_opts에 추가 */
				fio_options_set_ioengine_opts(l_opts, td);
			}

			/* 필수 인수가 누락된 경우 에러 */
			if ((!val || !strlen(val)) &&
			    l_opts[lidx].has_arg == required_argument) {
				log_err("fio: option %s requires an argument\n", opt);
				ret = 1;
			} else
				/* 옵션 파싱 */
				ret = fio_cmd_option_parse(td, opt, val);

			/* 파싱 실패 시 job 해제 */
			if (ret) {
				if (td) {
					put_job(td);
					td = NULL;
				}
				do_exit++;
				exit_val = 1;
			}

			/* "ioengine" 옵션이 변경되면 새 엔진 로드 */
			if (!ret && !strcmp(opt, "ioengine")) {
				if (ioengine_load(td)) {
					put_job(td);
					td = NULL;
					do_exit++;
					exit_val = 1;
					break;
				}
				fio_options_set_ioengine_opts(l_opts, td);
			}
			break;
		}
		case FIO_GETOPT_IOENGINE: {
			/* ===== IO 엔진 전용 옵션 처리 ===== */
			const char *opt = l_opts[lidx].name;
			char *val = optarg;

			if (!td)
				break;

			/* IO 엔진 전용 옵션 파싱 */
			ret = fio_cmd_ioengine_option_parse(td, opt, val);

			if (ret) {
				if (td) {
					put_job(td);
					td = NULL;
				}
				do_exit++;
				exit_val = 1;
			}
			break;
		}
		case 'w':
			/* --warnings-fatal: 경고를 치명적 오류로 처리 */
			warnings_fatal = 1;
			break;
		case 'j':
			/* --max-jobs: 더 이상 추적/필요하지 않음, 무시 */
			/* we don't track/need this anymore, ignore it */
			break;
		case 'S':
			/* --server: 백엔드 서버 모드 */
			did_arg = true;
#ifndef CONFIG_NO_SHM
			/* 클라이언트와 서버를 동시에 실행할 수 없음 */
			if (nr_clients) {
				log_err("fio: can't be both client and server\n");
				do_exit++;
				exit_val = 1;
				break;
			}
			if (optarg)
				fio_server_set_arg(optarg);
			is_backend = true;
			backend = true;
#else
			log_err("fio: client/server requires SHM support\n");
			do_exit++;
			exit_val = 1;
#endif
			break;
#ifdef WIN32
		case 'N':
			/* Windows 전용: 내부 서버 설정 */
			did_arg = true;
			fio_server_internal_set(optarg);
			break;
#endif
		case 'D':
			/* --daemonize: 데몬 모드, PID 파일 지정 */
			if (pid_file)
				free(pid_file);
			pid_file = strdup(optarg);
			break;
		case 'I':
			/* --idle-prof: CPU 유휴 프로파일링 */
			if ((ret = fio_idle_prof_parse_opt(optarg))) {
				/* 에러 또는 캘리브레이션만 수행 시 종료 */
				did_arg = true;
				do_exit++;
				if (ret == -1)
					exit_val = 1;
			}
			break;
		case 'C':
			/* --client: 원격 서버에 클라이언트로 연결 */
			did_arg = true;
			if (is_backend) {
				log_err("fio: can't be both client and server\n");
				do_exit++;
				exit_val = 1;
				break;
			}
			/* if --client parameter contains a pathname */
			/* --client 파라미터가 파일 경로이면 호스트 목록 파일로 처리 */
			if (0 == access(optarg, R_OK)) {
				/* file contains a list of host addrs or names */
				/* 파일에서 호스트 주소/이름 목록 읽기 */
				char hostaddr[PATH_MAX] = {0};
				char formatstr[8];
				FILE * hostf = fopen(optarg, "r");
				if (!hostf) {
					log_err("fio: could not open client list file %s for read\n", optarg);
					do_exit++;
					exit_val = 1;
					break;
				}
				sprintf(formatstr, "%%%ds", PATH_MAX - 1);
				/*
				 * read at most PATH_MAX-1 chars from each
				 * record in this file
				 */
				/* 파일에서 각 줄의 호스트 주소를 읽어 클라이언트 추가 */
				while (fscanf(hostf, formatstr, hostaddr) == 1) {
					/* expect EVERY host in file to be valid */
					/* 파일의 모든 호스트가 유효해야 함 */
					if (fio_client_add(&fio_client_ops, hostaddr, &cur_client)) {
						log_err("fio: failed adding client %s from file %s\n", hostaddr, optarg);
						do_exit++;
						exit_val = 1;
						break;
					}
				}
				fclose(hostf);
				break; /* no possibility of job file for "this client only" */
			}
			/* 호스트 이름/주소로 직접 클라이언트 추가 */
			if (fio_client_add(&fio_client_ops, optarg, &cur_client)) {
				log_err("fio: failed adding client %s\n", optarg);
				do_exit++;
				exit_val = 1;
				break;
			}
			/*
			 * If the next argument exists and isn't an option,
			 * assume it's a job file for this client only.
			 */
			/* 다음 인수가 옵션이 아니면 이 클라이언트 전용 job 파일로 간주 */
			while (optind < argc) {
				if (!strncmp(argv[optind], "--", 2) ||
				    !strncmp(argv[optind], "-", 1))
					break;

				if (fio_client_add_ini_file(cur_client, argv[optind], false))
					break;
				optind++;
			}
			break;
		case 'R':
			/* --remote-config: 서버에 원격 설정 파일 전송 */
			did_arg = true;
			if (fio_client_add_ini_file(cur_client, optarg, true)) {
				do_exit++;
				exit_val = 1;
			}
			break;
		case 'T':
			/* --cpuclock-test: CPU 클록 검증 테스트 실행 */
			did_arg = true;
			do_exit++;
			exit_val = fio_monotonic_clocktest(1);
			break;
		case 'G':
			/* --crctest: CRC/체크섬 속도 테스트 */
			did_arg = true;
			do_exit++;
			exit_val = fio_crctest(optarg);
			break;
		case 'M':
			/* --memcpytest: memcpy 속도 테스트 */
			did_arg = true;
			do_exit++;
			exit_val = fio_memcpy_test(optarg);
			break;
		case 'L': {
			/* --status-interval: 상태 덤프 출력 간격 */
			long long val;

			if (check_str_time(optarg, &val, 1)) {
				log_err("fio: failed parsing time %s\n", optarg);
				do_exit++;
				exit_val = 1;
				break;
			}
			if (val < 1000) {
				log_err("fio: status interval too small\n");
				do_exit++;
				exit_val = 1;
			}
			status_interval = val / 1000;
			break;
			}
		case 'W':
			/* --trigger-file: 트리거 파일 경로 설정 */
			if (trigger_file)
				free(trigger_file);
			trigger_file = strdup(optarg);
			break;
		case 'H':
			/* --trigger: 로컬 트리거 명령 설정 */
			if (trigger_cmd)
				free(trigger_cmd);
			trigger_cmd = strdup(optarg);
			break;
		case 'J':
			/* --trigger-remote: 원격 트리거 명령 설정 */
			if (trigger_remote_cmd)
				free(trigger_remote_cmd);
			trigger_remote_cmd = strdup(optarg);
			break;
		case 'K':
			/* --aux-path: 보조 파일 경로 설정 */
			if (aux_path)
				free(aux_path);
			aux_path = strdup(optarg);
			break;
		case 'B':
			/* --trigger-timeout: 트리거 타임아웃 설정 */
			if (check_str_time(optarg, &trigger_timeout, 1)) {
				log_err("fio: failed parsing time %s\n", optarg);
				do_exit++;
				exit_val = 1;
			}
			trigger_timeout /= 1000000;  /* 마이크로초 → 초 변환 */
			break;

		case 'A':
			/* --merge-blktrace-only: blktrace 병합만 수행 */
			did_arg = true;
			merge_blktrace_only = true;
			break;
		case '?':
			/* 인식할 수 없는 옵션 */
			log_err("%s: unrecognized option '%s'\n", argv[0],
							argv[optind - 1]);
			show_closest_option(argv[optind - 1]);  /* 유사한 옵션 제안 */
			fio_fallthrough;
		default:
			do_exit++;
			exit_val = 1;
			break;
		}
		if (do_exit)
			break;
	}

	/* 종료 조건이 충족되고 백엔드/클라이언트가 아니면 즉시 종료 */
	if (do_exit && !(is_backend || nr_clients))
		exit(exit_val);

	/* 클라이언트들에게 연결 */
	if (nr_clients && fio_clients_connect())
		exit(1);

	/* 서버 모드이면 서버 시작 */
	if (is_backend && backend)
		return fio_start_server(pid_file);
	else if (pid_file)
		free(pid_file);

	/* 마지막으로 파싱 중이던 job이 있으면 등록 */
	if (td) {
		if (!ret) {
			ret = add_job(td, td->o.name ?: "fio", 0, 0, client_type);
			if (ret)
				exit(1);
		}
	}

	/* 나머지 인수들을 ini 파일(job 파일)로 수집 */
	while (!ret && optind < argc) {
		ini_idx++;
		ini_file = realloc(ini_file, ini_idx * sizeof(char *));
		ini_file[ini_idx - 1] = strdup(argv[optind]);
		optind++;
	}

out_free:
	return ini_idx;  /* 수집된 ini 파일 개수 반환 */
}

/*
 * fio_init_options() - fio 옵션 시스템 초기화 함수
 *
 * 역할: fio의 옵션 파싱 시스템을 초기화합니다.
 *       이 함수는 parse_options()에서 가장 먼저 호출됩니다.
 * 파라미터: 없음
 * 반환값: 성공 시 0, 실패 시 1
 *
 * 초기화 순서:
 *   1. 표준 출력/에러 파일 포인터 설정
 *   2. getopt 옵션 문자열 생성
 *   3. l_opts 배열 초기화 (job 옵션 추가)
 *   4. atexit()에 free_shm 등록 (프로그램 종료 시 정리)
 *   5. 기본 스레드(def_thread) 초기화
 */
int fio_init_options(void)
{
	f_out = stdout;
	f_err = stderr;

	/* getopt 옵션 문자열 생성 */
	fio_options_fill_optstring();
	/* l_opts에 fio job 옵션들을 추가하고 초기화 */
	fio_options_dup_and_init(l_opts);

	/* 프로그램 종료 시 공유 메모리 정리 함수 등록 */
	atexit(free_shm);

	/* 기본 스레드 초기화 (글로벌 옵션 템플릿) */
	if (fill_def_thread())
		return 1;

	return 0;
}

extern int fio_check_options(struct thread_options *);

/*
 * parse_options() - fio 전체 옵션 파싱 함수 (최상위 진입점)
 *
 * 역할: fio의 전체 초기화를 수행하는 최상위 함수입니다.
 *       명령줄 파싱, job 파일 파싱, 클라이언트/서버 설정을 모두 처리합니다.
 *
 * 파라미터:
 *   argc - 명령줄 인수 개수
 *   argv - 명령줄 인수 배열
 *
 * 반환값: 성공 시 0, 실패 시 1
 *
 * 전체 흐름:
 *   1. fio_init_options()  → 옵션 시스템 초기화
 *   2. fio_test_cconv()    → 내부 변환 테스트
 *   3. parse_cmd_line()    → 명령줄 파싱 (job 파일 경로 수집)
 *   4. job 파일 처리:
 *      a. 클라이언트 모드 → fio_clients_send_ini()로 서버에 전송
 *      b. 로컬 모드 → parse_jobs_ini()로 직접 파싱
 *   5. 정리 및 검증 (job이 하나도 없으면 에러)
 */
int parse_options(int argc, char *argv[])
{
	const int type = FIO_CLIENT_TYPE_CLI;
	int job_files, i;

	/* 옵션 시스템 초기화 */
	if (fio_init_options())
		return 1;
	/* 내부 변환(cconv) 테스트 - thread_options ↔ thread_options_pack 변환 검증 */
	if (fio_test_cconv(&def_thread.o))
		log_err("fio: failed internal cconv test\n");

	/* 명령줄 파싱 - job 파일 경로를 수집하고 개수 반환 */
	job_files = parse_cmd_line(argc, argv, type);

	/* 수집된 job 파일이 있는 경우 */
	if (job_files > 0) {
		for (i = 0; i < job_files; i++) {
			/* 두 번째 파일부터는 def_thread를 다시 초기화
			 * (이전 파일의 [global] 설정이 영향을 미치지 않도록) */
			if (i && fill_def_thread())
				return 1;
			if (nr_clients) {
				/* 클라이언트 모드: ini 파일을 원격 서버에 전송 */
				if (fio_clients_send_ini(ini_file[i]))
					return 1;
				free(ini_file[i]);
			} else if (!is_backend) {
				/* 로컬 모드: ini 파일을 직접 파싱하여 job 생성 */
				if (parse_jobs_ini(ini_file[i], 0, i, type))
					return 1;
				free(ini_file[i]);
			}
		}
	} else if (nr_clients) {
		/* job 파일 없이 클라이언트 모드인 경우 */
		if (fill_def_thread())
			return 1;
		if (fio_clients_send_ini(NULL))
			return 1;
	}

	/* ini_file 배열 및 기본 스레드 옵션 메모리 해제 */
	free(ini_file);
	fio_options_free(&def_thread);
	/* 파일 설정 관련 메모리 해제 */
	filesetup_mem_free();

	/* job이 하나도 정의되지 않은 경우 처리 */
	if (!thread_number) {
		if (parse_dryrun())
			return 0;      /* 드라이런 모드면 정상 */
		if (exec_profile)
			return 0;      /* 프로파일 모드면 정상 */
		if (is_backend || nr_clients)
			return 0;      /* 서버/클라이언트 모드면 정상 */
		if (did_arg)
			return 0;      /* 유효한 인수가 처리되었으면 정상 */

		/* 아무 job도 정의되지 않았으면 에러 및 사용법 출력 */
		log_err("No job(s) defined\n\n");
		usage(argv[0]);
		return 1;
	}

	/* 일반 출력 형식에서 버전 문자열 출력 */
	if (output_format & FIO_OUTPUT_NORMAL)
		log_info("%s\n", fio_version_string);

	return 0;
}

/*
 * options_default_fill() - 기본 옵션값 복사 함수
 *
 * 역할: def_thread의 옵션을 주어진 thread_options 구조체에 복사합니다.
 * 파라미터:
 *   o - 옵션을 채울 thread_options 구조체
 * 반환값: 없음
 */
void options_default_fill(struct thread_options *o)
{
	memcpy(o, &def_thread.o, sizeof(*o));
}

/*
 * get_global_options() - 글로벌 옵션 접근 함수
 *
 * 역할: 글로벌 기본 설정(def_thread)에 대한 포인터를 반환합니다.
 *       외부 모듈에서 글로벌 옵션에 접근할 때 사용합니다.
 * 파라미터: 없음
 * 반환값: def_thread(글로벌 기본 thread_data) 포인터
 */
struct thread_data *get_global_options(void)
{
	return &def_thread;
}
