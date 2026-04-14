/*
 * fio - the flexible io tester
 *
 * Copyright (C) 2005 Jens Axboe <axboe@suse.de>
 * Copyright (C) 2006-2012 Jens Axboe <axboe@kernel.dk>
 *
 * The license below covers all files distributed with fio unless otherwise
 * noted in the file itself.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
/*
 * [한국어 설명] fio 라이브러리 핵심 초기화/정리 및 유틸리티 (libfio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 전역 상태 관리 및 초기화/종료를 담당한다. I/O 카운터 리셋,
 * 스레드 상태 전이, 전역 종료 시그널 전달, fio 전체 초기화/정리를 포함한다.
 * initialize_fio()/deinitialize_fio()가 프로그램 시작/종료 시 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * main() [fio.c]에서 initialize_fio()를 호출하고, 종료 시 deinitialize_fio()를 호출.
 * backend.c의 thread_main()에서 td_set_runstate(), reset_all_stats() 등을 호출.
 * 호출 체인: main() → initialize_fio() [이 파일] / thread_main() → td_set_runstate()
 *
 * === 타 모듈과의 연결 ===
 * - fio.c: main()에서 initialize_fio()/deinitialize_fio() 호출
 * - backend.c: thread_main()에서 td_set_runstate(), reset_all_stats() 호출
 * - smalloc.c: sinit()/scleanup()으로 공유 메모리 초기화/정리
 * - filelock.c: fio_filelock_init()/fio_filelock_exit()
 *
 * === 주요 함수/구조체 요약 ===
 * - initialize_fio(): fio 전체 초기화 (엔디안 검사, 메모리, 파일잠금, 로케일)
 * - deinitialize_fio(): fio 종료 정리
 * - td_set_runstate(): 스레드 실행 상태 전이 (CREATED→RUNNING→EXITED 등)
 * - fio_terminate_threads(): 지정된 그룹/전체 스레드에 종료 신호 전달
 * - reset_all_stats(): 모든 통계 카운터 리셋 (ramp time 이후)
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <string.h>    /* 문자열 처리 함수 (memcpy, memset 등) */
#include <signal.h>    /* 시그널 처리 (SIGTERM 등) */
#include <stdint.h>    /* 고정 크기 정수 타입 (uint8_t, uint64_t 등) */
#include <locale.h>    /* 로케일 설정 (숫자 출력 형식) */
#include <fcntl.h>     /* 파일 제어 (O_NONBLOCK 등) */

/* fio 내부 헤더 파일들 */
#include "fio.h"           /* fio 핵심 구조체 및 매크로 */
#include "smalloc.h"       /* 공유 메모리 할당기 */
#include "os/os.h"         /* OS별 추상화 계층 */
#include "filelock.h"      /* 파일 잠금 서브시스템 */
#include "helper_thread.h" /* 헬퍼 스레드 (통계, 디스크 유틸 등) */
#include "filehash.h"      /* 파일 해시 테이블 */

/* [한국어] 전역 디스크 목록 - 시스템의 디스크 장치 추적 */
FLIST_HEAD(disk_list);

/* [한국어] 아키텍처별 플래그 (예: TSC 지원 여부 등) */
unsigned long arch_flags = 0;

/* [한국어] 페이지 크기 관련 전역 변수 - 메모리 정렬에 사용 */
uintptr_t page_mask = 0;  /* 페이지 마스크 (page_size - 1), 정렬 검사에 사용 */
uintptr_t page_size = 0;  /* 시스템 페이지 크기 (일반적으로 4096) */

/* see os/os.h */
/* [한국어] OS 이름 문자열 배열 - 지원하는 운영체제 목록 */
static const char *fio_os_strings[os_nr] = {
	"Invalid",
	"Linux",
	"AIX",
	"FreeBSD",
	"HP-UX",
	"OSX",
	"NetBSD",
	"OpenBSD",
	"Solaris",
	"Windows",
	"Android",
	"DragonFly",
};

/* see arch/arch.h */
/* [한국어] 아키텍처 이름 문자열 배열 - 지원하는 CPU 아키텍처 목록 */
static const char *fio_arch_strings[arch_nr] = {
	"Invalid",
	"x86-64",
	"x86",
	"ppc",
	"ia64",
	"s390",
	"alpha",
	"sparc",
	"sparc64",
	"arm",
	"sh",
	"hppa",
	"mips",
	"aarch64",
	"loongarch64",
	"riscv64",
	"generic"
};

/* [한국어] I/O 카운터 초기화 - 바이트/블록 카운터, rate 관련 카운터를 0으로 리셋
 * all=1이면 모든 방향(read/write/trim)의 카운터를 리셋,
 * all=0이면 zone_bytes, rwmix_issues, nr_done_files만 리셋 */
static void reset_io_counters(struct thread_data *td, int all)
{
	int ddir;

	if (all) {
		for (ddir = 0; ddir < DDIR_RWDIR_CNT; ddir++) {
			td->stat_io_bytes[ddir] = 0;     /* 통계용 I/O 바이트 */
			td->this_io_bytes[ddir] = 0;     /* 현재 루프의 I/O 바이트 */
			td->stat_io_blocks[ddir] = 0;    /* 통계용 I/O 블록 수 */
			td->this_io_blocks[ddir] = 0;    /* 현재 루프의 I/O 블록 수 */
			td->last_rate_check_bytes[ddir] = 0;  /* rate 검사 기준 바이트 */
			td->last_rate_check_blocks[ddir] = 0; /* rate 검사 기준 블록 */
			td->bytes_done[ddir] = 0;        /* 완료된 바이트 수 */
			td->rate_io_issue_bytes[ddir] = 0;    /* rate 제어용 발행 바이트 */
			td->rate_next_io_time[ddir] = 0;      /* 다음 I/O 예정 시각 */
			td->last_usec[ddir] = 0;         /* 마지막 I/O 시간(마이크로초) */
		}
		td->bytes_verified = 0;  /* 검증 완료 바이트 수 리셋 */
	}

	td->zone_bytes = 0;     /* 현재 존(zone)에서 수행한 바이트 */

	td->rwmix_issues = 0;   /* 읽기/쓰기 혼합 비율 카운터 */

	/*
	 * reset file done count if we are to start over
	 */
	/* [한국어] time_based, 루프 반복, 또는 검증 모드일 때 완료 파일 수 리셋 */
	if (td->o.time_based || td->loops > 1 || td->o.do_verify)
		td->nr_done_files = 0;
}

/* [한국어] 스레드의 I/O 상태를 완전히 초기화
 * - 카운터 리셋, 파일 닫기, 파일 오프셋 재설정
 * - rand_repeatable이면 난수 시드도 재설정
 * - 진행 중인(inflight) I/O도 클리어 */
void clear_io_state(struct thread_data *td, int all)
{
	struct fio_file *f;
	unsigned int i;

	reset_io_counters(td, all);

	close_files(td);
	for_each_file(td, f, i) {
		fio_file_clear_done(f);
		f->file_offset = get_start_offset(td, f);
	}

	/*
	 * Re-Seed random number generator if rand_repeatable is true
	 */
	if (td->o.rand_repeatable)
		td_fill_rand_seeds(td);

	clear_inflight(td);
}

/*
 * Update thinktime block counter
 */
/* [한국어] thinktime 블록 카운터 업데이트
 * - thinktime은 I/O 사이의 지연 시간을 시뮬레이션하는 기능
 * - 오프로드 워커는 thinktime 카운터가 없으므로 조기 반환 */
static void update_thinktime_blocks_counter(struct thread_data *td)
{
	unsigned long long b;

	/* an offload worker has no thinktime blocks counters initialized */
	if (!td->thinktime_blocks_counter)
		return;

	b = ddir_rw_sum(td->thinktime_blocks_counter);
	td->last_thinktime_blocks -= b;
}

/* [한국어] 모든 통계 카운터를 리셋 - ramp time 이후 또는 통계 리셋 요청 시 호출
 * I/O 카운터, 바이트/블록/이슈 카운터, 런타임, 에포크 시간 등을 모두 초기화 */
void reset_all_stats(struct thread_data *td)
{
	int i;

	reset_io_counters(td, 1);

	update_thinktime_blocks_counter(td);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		td->io_bytes[i] = 0;           /* 총 I/O 바이트 */
		td->io_blocks[i] = 0;          /* 총 I/O 블록 */
		td->io_issues[i] = 0;          /* 총 I/O 발행 횟수 */
		td->ts.total_io_u[i] = 0;      /* 총 io_u 사용 수 */
		td->ts.runtime[i] = 0;         /* 각 방향별 런타임 */
	}

	/* [한국어] 에포크 시간을 현재 시간으로 재설정하고, 각 샘플링 시간도 동기화 */
	set_epoch_time(td, td->o.log_alternate_epoch_clock_id, td->o.job_start_clock_id);
	memcpy(&td->start, &td->epoch, sizeof(td->epoch));
	memcpy(&td->iops_sample_time, &td->epoch, sizeof(td->epoch));
	memcpy(&td->bw_sample_time, &td->epoch, sizeof(td->epoch));
	memcpy(&td->ss.prev_time, &td->epoch, sizeof(td->epoch));

	td->last_thinktime = td->epoch;

	lat_target_reset(td);       /* 레이턴시 타겟 리셋 */
	clear_rusage_stat(td);      /* 리소스 사용량 통계 클리어 */
	helper_reset();             /* 헬퍼 스레드 리셋 */
}

/* [한국어] fio 전역 상태 리셋 - 새로운 실행을 위해 그룹/스레드/세그먼트 카운터를 초기화 */
void reset_fio_state(void)
{
	int i;

	groupid = 0;        /* 그룹 ID 카운터 리셋 */
	thread_number = 0;  /* 스레드 번호 카운터 리셋 */
	cur_segment = 0;    /* 현재 세그먼트 인덱스 리셋 */
	for (i = 0; i < nr_segments; i++)
		segments[i].nr_threads = 0;  /* 각 세그먼트의 스레드 수 리셋 */
	stat_number = 0;    /* 통계 번호 리셋 */
	done_secs = 0;      /* 완료 시간(초) 리셋 */
}

/* [한국어] OS 이름 문자열 반환 - 인덱스로 OS 이름을 조회 */
const char *fio_get_os_string(int nr)
{
	if (nr < os_nr)
		return fio_os_strings[nr];

	return NULL;
}

/* [한국어] 아키텍처 이름 문자열 반환 - 인덱스로 아키텍처 이름을 조회 */
const char *fio_get_arch_string(int nr)
{
	if (nr < arch_nr)
		return fio_arch_strings[nr];

	return NULL;
}

/* [한국어] 스레드 실행 상태 문자열 배열 - 각 상태의 이름 매핑 */
static const char *td_runstates[] = {
	"NOT_CREATED",   /* 스레드 미생성 */
	"CREATED",       /* 스레드 생성됨 */
	"INITIALIZED",   /* 초기화 완료 */
	"RAMP",          /* 램프업(워밍업) 중 */
	"SETTING_UP",    /* 설정 중 */
	"RUNNING",       /* I/O 실행 중 */
	"PRE_READING",   /* 사전 읽기 중 */
	"VERIFYING",     /* 데이터 검증 중 */
	"FSYNCING",      /* fsync 수행 중 */
	"FINISHING",     /* 마무리 중 */
	"EXITED",        /* 종료됨 */
	"REAPED",        /* 회수됨 (자원 해제 완료) */
};

/* [한국어] 실행 상태 번호를 문자열 이름으로 변환 */
const char *runstate_to_name(int runstate)
{
	compiletime_assert(TD_LAST == 12, "td runstate list");
	if (runstate >= 0 && runstate < TD_LAST)
		return td_runstates[runstate];

	return "invalid";
}

/* [한국어] 스레드 실행 상태 설정 - 상태 전이를 수행하고 디버그 로그 출력 */
void td_set_runstate(struct thread_data *td, int runstate)
{
	if (td->runstate == runstate)
		return;

	dprint(FD_PROCESS, "pid=%d: runstate %s -> %s\n", (int) td->pid,
						runstate_to_name(td->runstate),
						runstate_to_name(runstate));
	td->runstate = runstate;
}

/* [한국어] 스레드 실행 상태를 새 상태로 변경하고, 이전 상태를 반환
 * td_restore_runstate()와 함께 사용하여 임시 상태 변경 후 복원 가능 */
int td_bump_runstate(struct thread_data *td, int new_state)
{
	int old_state = td->runstate;

	td_set_runstate(td, new_state);
	return old_state;
}

/* [한국어] 스레드 실행 상태를 이전 상태로 복원 */
void td_restore_runstate(struct thread_data *td, int old_state)
{
	td_set_runstate(td, old_state);
}

/* [한국어] 스레드에 종료 표시 - 종료 시간 기록 후 terminate 플래그 설정
 * write_barrier()로 메모리 순서 보장 */
void fio_mark_td_terminate(struct thread_data *td)
{
	fio_gettime(&td->terminate_time, NULL);
	write_barrier();
	td->terminate = true;
}

/* [한국어] 지정된 그룹 또는 전체 스레드에 종료 신호 전달
 * terminate 모드에 따라:
 *   - TERMINATE_GROUP: 특정 그룹만 종료
 *   - TERMINATE_STONEWALL: 실행 중인 스레드만 종료
 *   - TERMINATE_ALL: 모든 스레드 종료
 * 실행 중인 스레드는 자연 종료를 기다리고, 시작 전 스레드는 SIGTERM 전송 */
void fio_terminate_threads(unsigned int group_id, unsigned int terminate)
{
	pid_t pid = getpid();

	dprint(FD_PROCESS, "terminate group_id=%d\n", group_id);

	for_each_td(td) {
		if ((terminate == TERMINATE_GROUP && group_id == TERMINATE_ALL) ||
		    (terminate == TERMINATE_GROUP && group_id == td->groupid) ||
		    (terminate == TERMINATE_STONEWALL && td->runstate >= TD_RUNNING) ||
		    (terminate == TERMINATE_ALL)) {
			dprint(FD_PROCESS, "setting terminate on %s/%d\n",
						td->o.name, (int) td->pid);

			if (td->terminate)
				continue;

			fio_mark_td_terminate(td);
			td->o.start_delay = 0;

			/*
			 * if the thread is running, just let it exit
			 */
			if (!td->pid || pid == td->pid)
				continue;
			else if (td->runstate < TD_RAMP)
				kill(td->pid, SIGTERM);  /* 아직 시작 안 한 스레드에게 SIGTERM */
			else {
				struct ioengine_ops *ops = td->io_ops;

				/* [한국어] I/O 엔진에 terminate 콜백이 있으면 호출 */
				if (ops && ops->terminate)
					ops->terminate(td);
			}
		}
	} end_for_each();
}

/* [한국어] 실행 중이거나 대기 중인 I/O 스레드가 있는지 확인
 * 반환값: 1 = 아직 실행 중인 스레드 있음, 0 = 모든 스레드 종료됨
 *        -1 = cpuio 스레드만 있었음 (실제 I/O 스레드 없음) */
int fio_running_or_pending_io_threads(void)
{
	int nr_io_threads = 0;

	for_each_td(td) {
		if (td->io_ops_init && td_ioengine_flagged(td, FIO_NOIO))
			continue;
		nr_io_threads++;
		if (td->runstate < TD_EXITED)
			return 1;
	} end_for_each();

	if (!nr_io_threads)
		return -1; /* we only had cpuio threads to begin with */
	return 0;
}

/* [한국어] 파일 디스크립터를 논블로킹 모드로 설정
 * 이전 플래그를 반환하여 나중에 복원할 수 있게 함 */
int fio_set_fd_nonblocking(int fd, const char *who)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		log_err("fio: %s failed to get file flags: %s\n", who, strerror(errno));
	else {
		int new_flags = flags | O_NONBLOCK;

		new_flags = fcntl(fd, F_SETFL, new_flags);
		if (new_flags < 0)
			log_err("fio: %s failed to get file flags: %s\n", who, strerror(errno));
	}

	return flags;
}

/* [한국어] 엔디안 검사 에러 코드 */
enum {
	ENDIAN_INVALID_BE = 1,      /* 빅엔디안이 감지되었으나 리틀엔디안으로 설정됨 */
	ENDIAN_INVALID_LE,          /* 리틀엔디안이 감지되었으나 빅엔디안으로 설정됨 */
	ENDIAN_INVALID_CONFIG,      /* 엔디안 설정이 없음 */
	ENDIAN_BROKEN,              /* 엔디안 감지 실패 */
};

/* [한국어] 시스템의 엔디안 설정이 컴파일 시 설정과 일치하는지 검사
 * 64비트 정수에 0x12를 저장하고 바이트 위치를 확인하여 엔디안 판별 */
static int endian_check(void)
{
	union {
		uint8_t c[8];
		uint64_t v;
	} u;
	int le = 0, be = 0;

	u.v = 0x12;
	if (u.c[7] == 0x12)
		be = 1;
	else if (u.c[0] == 0x12)
		le = 1;

#if defined(CONFIG_LITTLE_ENDIAN)
	if (be)
		return ENDIAN_INVALID_BE;
#elif defined(CONFIG_BIG_ENDIAN)
	if (le)
		return ENDIAN_INVALID_LE;
#else
	return ENDIAN_INVALID_CONFIG;
#endif

	if (!le && !be)
		return ENDIAN_BROKEN;

	return 0;
}

/* [한국어] fio 전체 초기화 함수 - 프로그램 시작 시 한 번 호출
 * 수행 작업:
 *   1) 구조체 정렬 검사 (compiletime_assert) - ARM 등에서 정렬 오류 방지
 *   2) 엔디안 설정 일치 여부 검사
 *   3) 아키텍처 초기화 (arch_init)
 *   4) 공유 메모리 할당기 초기화 (sinit)
 *   5) 파일 잠금 서브시스템 초기화
 *   6) 파일 해시 초기화
 *   7) 로케일 설정 (숫자 출력 형식)
 *   8) 시스템 페이지 크기 조회
 *   9) 키워드 초기화 */
int initialize_fio(char *envp[])
{
	long ps;
	int err;

	/*
	 * We need these to be properly 64-bit aligned, otherwise we
	 * can run into problems on archs that fault on unaligned fp
	 * access (ARM).
	 */
	/* [한국어] 컴파일 타임 정렬 검사 - 구조체 멤버가 올바르게 정렬되었는지 확인 */
	compiletime_assert((offsetof(struct thread_data, ts) % sizeof(void *)) == 0, "ts");
	compiletime_assert((offsetof(struct thread_stat, percentile_list) % 8) == 0, "stat percentile_list");
	compiletime_assert((offsetof(struct thread_stat, total_run_time) % 8) == 0, "total_run_time");
	compiletime_assert((offsetof(struct thread_stat, total_err_count) % 8) == 0, "total_err_count");
	compiletime_assert((offsetof(struct thread_stat, latency_percentile) % 8) == 0, "stat latency_percentile");
	compiletime_assert((offsetof(struct thread_data, ts.clat_stat) % 8) == 0, "ts.clat_stat");
	compiletime_assert((offsetof(struct thread_options_pack, zipf_theta) % 8) == 0, "zipf_theta");
	compiletime_assert((offsetof(struct thread_options_pack, pareto_h) % 8) == 0, "pareto_h");
	compiletime_assert((offsetof(struct thread_options_pack, percentile_list) % 8) == 0, "percentile_list");
	compiletime_assert((offsetof(struct thread_options_pack, latency_percentile) % 8) == 0, "latency_percentile");
	compiletime_assert((offsetof(struct jobs_eta, m_rate) % 8) == 0, "m_rate");

	compiletime_assert(__TD_F_LAST <= TD_ENG_FLAG_SHIFT, "TD_ENG_FLAG_SHIFT");
	compiletime_assert((__TD_F_LAST + __FIO_IOENGINE_F_LAST) <= 8*sizeof(((struct thread_data *)0)->flags), "td->flags");
	compiletime_assert(BSSPLIT_MAX <= ZONESPLIT_MAX, "bsssplit/zone max");

	/* [한국어] 엔디안 설정 검증 */
	err = endian_check();
	if (err) {
		log_err("fio: endianness settings appear wrong.\n");
		switch (err) {
		case ENDIAN_INVALID_BE:
			log_err("fio: got big-endian when configured for little\n");
			break;
		case ENDIAN_INVALID_LE:
			log_err("fio: got little-endian when configured for big\n");
			break;
		case ENDIAN_INVALID_CONFIG:
			log_err("fio: not configured to any endianness\n");
			break;
		case ENDIAN_BROKEN:
			log_err("fio: failed to detect endianness\n");
			break;
		default:
			assert(0);
			break;
		}
		log_err("fio: please report this to fio@vger.kernel.org\n");
		return 1;
	}

	arch_init(envp);  /* 아키텍처별 초기화 (TSC 등) */

	sinit();  /* 공유 메모리(smalloc) 풀 초기화 */

	if (fio_filelock_init()) {
		log_err("fio: failed initializing filelock subsys\n");
		return 1;
	}

	file_hash_init();  /* 파일 해시 테이블 초기화 */

	/*
	 * We need locale for number printing, if it isn't set then just
	 * go with the US format.
	 */
	/* [한국어] LC_NUMERIC이 설정되지 않았으면 미국 형식(콤마 구분) 사용 */
	if (!getenv("LC_NUMERIC"))
		setlocale(LC_NUMERIC, "en_US");

	/* [한국어] 시스템 페이지 크기 조회 - 메모리 정렬 및 I/O 크기 계산에 사용 */
	ps = sysconf(_SC_PAGESIZE);
	if (ps < 0) {
		log_err("Failed to get page size\n");
		return 1;
	}

	page_size = ps;
	page_mask = ps - 1;

	fio_keywords_init();  /* 설정 키워드 파서 초기화 */
	return 0;
}

/* [한국어] fio 종료 정리 함수 - 키워드 파서 해제 */
void deinitialize_fio(void)
{
	fio_keywords_exit();
}
