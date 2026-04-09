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
 * [한국어] backend.c - fio의 백엔드 실행 엔진
 *
 * 이 파일은 fio의 핵심 I/O 실행 엔진을 구현한다.
 * 주요 기능:
 *   1) do_io()      - 메인 I/O 루프. get_io_u -> prep -> queue -> commit -> getevents -> put_io_u 흐름
 *   2) thread_main() - 각 워커 스레드/프로세스의 진입점. 초기화 -> do_io -> do_verify -> 정리
 *   3) run_threads() - 모든 작업(job)을 생성하고, 시작하고, 종료될 때까지 관리
 *   4) fio_backend() - 최상위 진입점. 프로파일 로드 -> run_threads -> 통계 출력
 *
 * I/O 흐름 요약:
 *   get_io_u()   : freelist에서 io_u 구조체를 꺼내고, 오프셋/크기/방향을 결정
 *   td_io_prep() : I/O 엔진에 io_u를 준비시킴 (prep 콜백 호출)
 *   td_io_queue(): I/O 엔진에 io_u를 제출 (queue 콜백 호출)
 *   td_io_commit(): 배치 제출이 필요한 엔진에서 실제 제출을 수행
 *   io_u_queued_complete() / io_u_sync_complete(): 완료된 I/O를 수거
 *   put_io_u()  : 사용 완료된 io_u를 freelist로 반환
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <math.h>
#include <pthread.h>

#ifdef CONFIG_LINUX
#include <linux/prctl.h>  /* 프로세스 제어 (예: PR_SET_NAME으로 스레드 이름 설정) */
#include <sys/prctl.h>
#endif

/* fio 내부 헤더 파일들 */
#include "fio.h"           /* fio 핵심 구조체 및 매크로 */
#include "smalloc.h"       /* 공유 메모리 할당기 */
#include "verify.h"        /* 데이터 무결성 검증 */
#include "diskutil.h"      /* 디스크 유틸리티 (I/O 통계) */
#include "cgroup.h"        /* cgroup 지원 */
#include "profile.h"       /* 프로파일 지원 */
#include "lib/rand.h"      /* 난수 생성기 */
#include "lib/memalign.h"  /* 메모리 정렬 할당 */
#include "server.h"        /* 서버 모드 지원 */
#include "lib/getrusage.h" /* 리소스 사용량 조회 */
#include "idletime.h"      /* 유휴 시간 프로파일링 */
#include "err.h"           /* 에러 처리 매크로 (IS_ERR, PTR_ERR 등) */
#include "workqueue.h"     /* 오프로드 모드 작업 큐 */
#include "lib/mountcheck.h"/* 마운트 확인 유틸리티 */
#include "rate-submit.h"   /* 속도 제한 제출 */
#include "helper_thread.h" /* 헬퍼 스레드 (통계, 디스크 유틸 등) */
#include "pshared.h"       /* 프로세스 간 공유 뮤텍스/조건변수 */
#include "zone-dist.h"     /* 존(zone) 분배 */
#include "fio_time.h"      /* 시간 유틸리티 */

/* [한국어] 전역 변수들 - 스레드 동기화 및 상태 관리 */
static struct fio_sem *startup_sem;   /* 스레드 시작 동기화용 세마포어 */
static struct flist_head *cgroup_list;/* cgroup 목록 */
static struct cgroup_mnt *cgroup_mnt; /* cgroup 마운트 정보 */
static int exit_value;                /* 프로그램 종료 코드 */
static volatile bool fio_abort;       /* 강제 중단 플래그 (시그널 등으로 설정) */
static unsigned int nr_process = 0;   /* fork 기반 작업(job) 수 */
static unsigned int nr_thread = 0;    /* pthread 기반 작업(job) 수 */

/* [한국어] 집계된 I/O 로그 (읽기/쓰기/트림 방향별) */
struct io_log *agg_io_log[DDIR_RWDIR_CNT];

/* [한국어] 전역 상태 변수들 */
int groupid = 0;                /* 현재 그룹 ID */
unsigned int thread_number = 0; /* 전체 스레드(작업) 수 */
unsigned int nr_segments = 0;   /* 세그먼트 수 */
unsigned int cur_segment = 0;   /* 현재 세그먼트 인덱스 */
unsigned int stat_number = 0;   /* 통계 번호 */
int temp_stall_ts;              /* 일시 정지 타임스탬프 */
unsigned long done_secs = 0;    /* 완료된 작업의 총 실행 시간(초) */

/* [한국어] 오버랩 체크용 뮤텍스 - 오프로드 모드에서 io_u 영역 겹침 검사 시 사용 */
#ifdef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
pthread_mutex_t overlap_check = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
#else
pthread_mutex_t overlap_check = PTHREAD_MUTEX_INITIALIZER;
#endif

extern char *write_bw_log_name;

/* [한국어] 작업 시작 대기 타임아웃: 5초 (밀리초 단위) */
#define JOB_START_TIMEOUT	(5 * 1000)

/*
 * [한국어] sig_int - SIGINT/SIGTERM 시그널 핸들러
 *
 * @sig: 수신된 시그널 번호
 *
 * 역할: 사용자가 Ctrl+C를 누르거나 SIGTERM을 받으면 모든 스레드를 종료시킨다.
 *       nr_segments가 0보다 큰 경우에만 동작한다.
 */
static void sig_int(int sig)
{
	if (nr_segments) {
		/* 백엔드(서버) 모드인 경우 서버에 시그널 전달 */
		if (is_backend)
			fio_server_got_signal(sig);
		else {
			/* 로컬 모드: 종료 메시지 출력 */
			log_info("\nfio: terminating on signal %d\n", sig);
			log_info_flush();
			exit_value = 128;
		}

		/* 모든 스레드에 종료 신호 전달 */
		fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
	}
}

#ifdef WIN32
/*
 * [한국어] sig_break - Windows SIGBREAK 시그널 핸들러
 *
 * @sig: 수신된 시그널 번호
 *
 * 역할: Windows에서는 SIGBREAK 후 핸들러 리턴 시 모든 자식 프로세스가
 *       즉시 종료되므로, 각 스레드가 통계를 출력하고 정상 종료할 시간을 준다.
 */
static void sig_break(int sig)
{
	sig_int(sig);

	/**
	 * Windows terminates all job processes on SIGBREAK after the handler
	 * returns, so give them time to wrap-up and give stats
	 */
	/* [한국어] 모든 스레드가 TD_EXITED 상태가 될 때까지 대기 */
	for_each_td(td) {
		while (td->runstate < TD_EXITED)
			sleep(1);
	} end_for_each();
}
#endif

/*
 * [한국어] sig_show_status - SIGUSR1 시그널 핸들러
 *
 * @sig: 수신된 시그널 번호
 *
 * 역할: 실행 중인 작업의 현재 통계를 화면에 출력한다.
 *       kill -USR1 <pid>로 트리거할 수 있다.
 */
void sig_show_status(int sig)
{
	show_running_run_stats();
}

/*
 * [한국어] set_sig_handlers - 시그널 핸들러 등록
 *
 * 역할: SIGINT, SIGTERM, SIGUSR1 등 시그널 핸들러를 설정한다.
 *       백엔드 모드에서는 SIGPIPE도 처리한다.
 */
static void set_sig_handlers(void)
{
	struct sigaction act;

	/* SIGINT (Ctrl+C) 핸들러 설정 */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;  /* 시스템 콜이 시그널에 의해 중단되지 않도록 재시작 */
	sigaction(SIGINT, &act, NULL);

	/* SIGTERM (종료 요청) 핸들러 설정 */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &act, NULL);

/* Windows uses SIGBREAK as a quit signal from other applications */
#ifdef WIN32
	/* [한국어] Windows 전용: SIGBREAK 핸들러 설정 */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_break;
	act.sa_flags = SA_RESTART;
	sigaction(SIGBREAK, &act, NULL);
#endif

	/* SIGUSR1: 실행 중 통계 출력 트리거 */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_show_status;
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);

	/* 백엔드(서버) 모드에서는 SIGPIPE를 무시하지 않고 종료 처리 */
	if (is_backend) {
		memset(&act, 0, sizeof(act));
		act.sa_handler = sig_int;
		act.sa_flags = SA_RESTART;
		sigaction(SIGPIPE, &act, NULL);
	}
}

/*
 * Check if we are above the minimum rate given.
 */
/*
 * [한국어] __check_min_rate - 특정 방향(ddir)의 최소 속도 충족 여부 검사
 *
 * @td:   스레드 데이터 구조체
 * @now:  현재 시각
 * @ddir: I/O 방향 (READ, WRITE, TRIM)
 *
 * 반환값: true = 최소 속도 미달 (에러), false = 정상
 *
 * 역할: ratemin 또는 rate_iops_min 옵션이 설정된 경우,
 *       ratecycle 주기마다 실제 처리량을 확인하여 최소 속도에 미달하면 true를 반환한다.
 *       시작 후 2초간은 안정화 기간으로 검사를 건너뛴다.
 */
static bool __check_min_rate(struct thread_data *td, struct timespec *now,
			     enum fio_ddir ddir)
{
	/* 현재까지의 바이트 수와 블록(I/O) 수를 저장 */
	unsigned long long current_rate_check_bytes = td->this_io_bytes[ddir];
	unsigned long current_rate_check_blocks = td->this_io_blocks[ddir];
	/* 사용자가 설정한 최소 속도 (바이트/초) */
	unsigned long long option_rate_bytes_min = td->o.ratemin[ddir];
	/* 사용자가 설정한 최소 IOPS */
	unsigned int option_rate_iops_min = td->o.rate_iops_min[ddir];

	/* ddir이 읽기/쓰기 방향인지 확인 (TRIM 등은 제외) */
	assert(ddir_rw(ddir));

	/* 최소 속도 옵션이 설정되지 않았으면 검사 불필요 */
	if (!td->o.ratemin[ddir] && !td->o.rate_iops_min[ddir])
		return false;

	/*
	 * allow a 2 second settle period in the beginning
	 */
	/* [한국어] 시작 후 2초간은 안정화 기간으로 검사 건너뜀 */
	if (mtime_since(&td->start, now) < 2000)
		return false;

	/*
	 * if last_rate_check_blocks or last_rate_check_bytes is set,
	 * we can compute a rate per ratecycle
	 */
	/* [한국어] 이전 검사 시점이 있으면 ratecycle 주기로 속도를 계산 */
	if (td->last_rate_check_bytes[ddir] || td->last_rate_check_blocks[ddir]) {
		/* 마지막 검사 이후 경과 시간 (밀리초) */
		unsigned long spent = mtime_since(&td->last_rate_check_time[ddir], now);
		/* ratecycle 주기가 아직 안 됐으면 건너뜀 */
		if (spent < td->o.ratecycle || spent==0)
			return false;

		if (td->o.ratemin[ddir]) {
			/*
			 * check bandwidth specified rate
			 */
			/* [한국어] 대역폭(바이트/초) 기준 최소 속도 검사 */
			unsigned long long current_rate_bytes =
				((current_rate_check_bytes - td->last_rate_check_bytes[ddir]) * 1000) / spent;
			if (current_rate_bytes < option_rate_bytes_min) {
				log_err("%s: rate_min=%lluB/s not met, got %lluB/s\n",
					td->o.name, option_rate_bytes_min, current_rate_bytes);
				return true;  /* 최소 속도 미달 */
			}
		} else {
			/*
			 * checks iops specified rate
			 */
			/* [한국어] IOPS 기준 최소 속도 검사 */
			unsigned long long current_rate_iops =
				((current_rate_check_blocks - td->last_rate_check_blocks[ddir]) * 1000) / spent;

			if (current_rate_iops < option_rate_iops_min) {
				log_err("%s: rate_iops_min=%u not met, got %llu IOPS\n",
					td->o.name, option_rate_iops_min, current_rate_iops);
				return true;  /* 최소 IOPS 미달 */
			}
		}
	}

	/* 현재 값을 다음 검사 시점의 기준값으로 저장 */
	td->last_rate_check_bytes[ddir] = current_rate_check_bytes;
	td->last_rate_check_blocks[ddir] = current_rate_check_blocks;
	memcpy(&td->last_rate_check_time[ddir], now, sizeof(*now));
	return false;  /* 최소 속도 충족 */
}

/*
 * [한국어] check_min_rate - 모든 I/O 방향의 최소 속도 검사
 *
 * @td:  스레드 데이터
 * @now: 현재 시각
 *
 * 반환값: true = 하나라도 최소 속도 미달, false = 모두 정상
 *
 * 역할: READ, WRITE, TRIM 각 방향에 대해 __check_min_rate()를 호출한다.
 */
static bool check_min_rate(struct thread_data *td, struct timespec *now)
{
	bool ret = false;

	/* 모든 읽기/쓰기 방향에 대해 검사 */
	for_each_rw_ddir(ddir) {
		if (td->bytes_done[ddir])
			ret |= __check_min_rate(td, now, ddir);
	}

	return ret;
}

/*
 * Helper to handle the final sync of a file. Works just like the normal
 * io path, just does everything sync.
 */
/*
 * [한국어] fio_io_sync - 파일의 동기화(sync) I/O를 수행하는 헬퍼 함수
 *
 * @td:   스레드 데이터
 * @f:    대상 파일
 * @ddir: 동기화 방향 (DDIR_SYNC 또는 DDIR_SYNCFS)
 *
 * 반환값: true = 에러 발생, false = 성공
 *
 * 역할: 일반 I/O 경로와 동일한 방식으로 동기화를 수행한다.
 *       io_u를 할당 -> prep -> queue -> 완료 대기 순서로 진행한다.
 *       이 함수는 do_io() 루프 종료 후 end_fsync에서 호출된다.
 */
static bool fio_io_sync(struct thread_data *td, struct fio_file *f,
			enum fio_ddir ddir)
{
	/* freelist에서 io_u를 하나 가져옴 */
	struct io_u *io_u = __get_io_u(td);
	enum fio_q_status ret;

	if (!io_u)
		return true;  /* io_u 할당 실패 */

	/* 동기화 방향과 대상 파일 설정 */
	io_u->ddir = ddir;
	io_u->file = f;
	/* 파일 참조 카운트를 감소시키지 않도록 플래그 설정 */
	io_u_set(td, io_u, IO_U_F_NO_FILE_PUT);

	/* I/O 엔진에 prep 콜백 호출 */
	if (td_io_prep(td, io_u)) {
		put_io_u(td, io_u);
		return true;  /* prep 실패 */
	}

requeue:
	/* I/O 엔진에 큐잉 (실제 제출) */
	ret = td_io_queue(td, io_u);
	switch (ret) {
	case FIO_Q_QUEUED:
		/* 비동기로 큐잉됨 - commit 후 완료 대기 */
		td_io_commit(td);
		if (io_u_queued_complete(td, 1) < 0)
			return true;
		break;
	case FIO_Q_COMPLETED:
		/* 동기적으로 즉시 완료됨 */
		if (io_u->error) {
			td_verror(td, io_u->error, "td_io_queue");
			return true;
		}

		if (io_u_sync_complete(td, io_u) < 0)
			return true;
		break;
	case FIO_Q_BUSY:
		/* 엔진이 바쁨 - commit 후 재시도 */
		td_io_commit(td);
		goto requeue;
	}

	return false;  /* 동기화 성공 */
}

/*
 * [한국어] fio_file_fsync - 특정 파일에 대해 fsync를 수행
 *
 * @td: 스레드 데이터
 * @f:  대상 파일
 *
 * 반환값: 0 = 성공, 1 = 실패
 *
 * 역할: 파일이 열려 있으면 바로 sync, 아니면 열고 sync 후 닫는다.
 */
static int fio_file_fsync(struct thread_data *td, struct fio_file *f)
{
	int ret, ret2;

	/* 파일이 이미 열려 있으면 바로 동기화 수행 */
	if (fio_file_open(f))
		return fio_io_sync(td, f, DDIR_SYNC);

	/* 파일이 닫혀 있으면 열기 */
	if (td_io_open_file(td, f))
		return 1;

	/* 동기화 수행 */
	ret = fio_io_sync(td, f, DDIR_SYNC);
	ret2 = 0;
	/* 파일이 열려 있으면 닫기 */
	if (fio_file_open(f))
		ret2 = td_io_close_file(td, f);
	return (ret || ret2);
}

/*
 * [한국어] fio_syncfs - 파일 시스템 전체를 동기화 (syncfs)
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 음수 = 에러
 *
 * 역할: td->fs_list에 등록된 모든 파일 시스템 마운트 포인트에 대해
 *       syncfs를 호출하여 전체 파일 시스템 버퍼를 디스크에 플러시한다.
 */
static int fio_syncfs(struct thread_data *td)
{
#ifdef CONFIG_SYNCFS
	struct flist_head *n;
	struct fio_mount *fm;
	int err = 0;

	/* Sync all file system mounts. */
	/* [한국어] 모든 파일 시스템 마운트 포인트를 순회하며 syncfs 수행 */
	flist_for_each(n, &td->fs_list) {
		fm = flist_entry(n, struct fio_mount, list);

		dprint(FD_IO, "sync FS %s\n", fm->base);

		/* 파일 시스템을 열기 */
		if (fio_open_fs(td, fm)) {
			log_err("open %s for syncfs failed\n", fm->base);
			err = -1;
			continue;
		}

		/* syncfs 수행 */
		if (fio_io_sync(td, fm->f, DDIR_SYNCFS)) {
			log_err("syncfs %s failed\n", fm->base);
			err = -1;
			continue;
		}

		/* 파일 시스템 닫기 */
		fio_close_fs(fm);
	}

	return err;
#else
	return -ENOSYS;  /* syncfs를 지원하지 않는 플랫폼 */
#endif
}

/*
 * [한국어] __update_ts_cache - 타임스탬프 캐시를 현재 시각으로 갱신
 *
 * @td: 스레드 데이터
 *
 * 역할: fio_gettime()으로 현재 시각을 가져와 td->ts_cache에 저장한다.
 *       매번 시스템 콜을 호출하면 느리므로, 캐시를 사용하여 주기적으로만 갱신한다.
 */
static inline void __update_ts_cache(struct thread_data *td)
{
	fio_gettime(&td->ts_cache, NULL);
}

/*
 * [한국어] update_ts_cache - 주기적으로 타임스탬프 캐시를 갱신
 *
 * @td: 스레드 데이터
 *
 * 역할: ts_cache_mask에 따라 N번째 호출마다 실제 시각을 갱신한다.
 *       매 I/O마다 시스템 콜을 호출하는 오버헤드를 줄이기 위한 최적화이다.
 */
static inline void update_ts_cache(struct thread_data *td)
{
	if ((++td->ts_cache_nr & td->ts_cache_mask) == td->ts_cache_mask)
		__update_ts_cache(td);
}

/*
 * [한국어] runtime_exceeded - 실행 시간 제한 초과 여부 확인
 *
 * @td: 스레드 데이터
 * @t:  현재 시각
 *
 * 반환값: true = 시간 초과, false = 아직 시간 남음
 *
 * 역할: timeout 옵션이 설정된 경우, epoch(시작 시각)부터 경과 시간이
 *       timeout을 초과했는지 확인한다. ramp 구간에서는 항상 false를 반환한다.
 */
static inline bool runtime_exceeded(struct thread_data *td, struct timespec *t)
{
	/* 워밍업(ramp) 기간에는 시간 초과 판단하지 않음 */
	if (in_ramp_period(td))
		return false;
	/* timeout이 설정되지 않았으면 시간 제한 없음 */
	if (!td->o.timeout)
		return false;
	/* 경과 시간이 timeout 이상이면 초과 */
	if (utime_since(&td->epoch, t) >= td->o.timeout)
		return true;

	return false;
}

/*
 * We need to update the runtime consistently in ms, but keep a running
 * tally of the current elapsed time in microseconds for sub millisecond
 * updates.
 */
/*
 * [한국어] update_runtime - 런타임 통계를 갱신
 *
 * @td:         스레드 데이터
 * @elapsed_us: 방향별 누적 경과 시간(마이크로초) 배열
 * @ddir:       I/O 방향
 *
 * 역할: 밀리초 단위의 런타임 통계를 정확하게 유지하기 위해,
 *       마이크로초 단위의 누적 경과 시간을 추적하여 밀리초로 변환한다.
 *       verify_only 모드에서 쓰기 방향은 갱신하지 않는다.
 */
static inline void update_runtime(struct thread_data *td,
				  unsigned long long *elapsed_us,
				  const enum fio_ddir ddir)
{
	/* verify_only 모드에서는 쓰기 런타임을 갱신하지 않음 */
	if (ddir == DDIR_WRITE && td_write(td) && td->o.verify_only)
		return;

	/* 이전 값을 빼고, 새로운 경과 시간을 더하여 밀리초 변환 */
	td->ts.runtime[ddir] -= (elapsed_us[ddir] + 999) / 1000;
	elapsed_us[ddir] += utime_since_now(&td->start);
	td->ts.runtime[ddir] += (elapsed_us[ddir] + 999) / 1000;
}

/*
 * [한국어] break_on_this_error - 에러 발생 시 I/O 루프를 중단할지 결정
 *
 * @td:     스레드 데이터
 * @ddir:   I/O 방향
 * @retptr: 반환 코드 포인터 (에러 시 음수)
 *
 * 반환값: true = 루프 중단 필요, false = 계속 진행
 *
 * 역할: 에러 유형에 따라 continue_on_error 옵션을 확인하고,
 *       치명적 에러인지 비치명적 에러인지 판단한다.
 *       fill_device 모드에서 ENOSPC/EDQUOT는 정상 종료로 처리한다.
 */
static bool break_on_this_error(struct thread_data *td, enum fio_ddir ddir,
				int *retptr)
{
	int ret = *retptr;

	if (ret < 0 || td->error) {
		int err = td->error;
		enum error_type_bit eb;

		if (ret < 0)
			err = -ret;

		/* 에러 유형 비트를 결정 (시스템 에러, I/O 에러 등) */
		eb = td_error_type(ddir, err);
		/* continue_on_error에 해당 에러 유형이 없으면 중단 */
		if (!(td->o.continue_on_error & (1 << eb)))
			return true;

		if (td_non_fatal_error(td, eb, err)) {
		        /*
		         * Continue with the I/Os in case of
			 * a non fatal error.
			 */
			/* [한국어] 비치명적 에러: 에러 카운트를 증가시키고 계속 진행 */
			update_error_count(td, err);
			td_clear_error(td);
			*retptr = 0;
			return false;
		} else if (td->o.fill_device && (err == ENOSPC || err == EDQUOT)) {
			/*
			 * We expect to hit this error if
			 * fill_device option is set.
			 */
			/* [한국어] 디바이스 채우기 모드에서 공간 부족은 정상 종료 */
			td_clear_error(td);
			fio_mark_td_terminate(td);
			return true;
		} else {
			/*
			 * Stop the I/O in case of a fatal
			 * error.
			 */
			/* [한국어] 치명적 에러: 에러 카운트를 기록하고 중단 */
			update_error_count(td, err);
			return true;
		}
	}

	return false;
}

/*
 * [한국어] check_update_rusage - 리소스 사용량(rusage) 업데이트 요청 처리
 *
 * @td: 스레드 데이터
 *
 * 역할: 메인 스레드가 rusage 업데이트를 요청했으면(update_rusage 플래그),
 *       현재 리소스 사용량을 갱신하고 세마포어로 완료를 알린다.
 */
static void check_update_rusage(struct thread_data *td)
{
	if (td->update_rusage) {
		td->update_rusage = 0;
		update_rusage_stat(td);
		fio_sem_up(td->rusage_sem);  /* 요청자에게 완료 알림 */
	}
}

/*
 * [한국어] wait_for_completions - 큐에 있는 I/O 완료를 대기
 *
 * @td:   스레드 데이터
 * @time: 속도 체크용 시간 저장 (NULL 가능)
 *
 * 반환값: 완료된 I/O 수 (음수 = 에러)
 *
 * 역할: 큐가 가득 찼거나 로그 확장이 필요할 때 호출된다.
 *       iodepth_batch_complete_min 만큼의 I/O 완료를 대기하며,
 *       큐 깊이가 iodepth_low 이하로 떨어질 때까지 반복한다.
 *       이 함수는 do_io()의 메인 루프에서 큐가 가득 찬 경우 호출된다.
 */
static int wait_for_completions(struct thread_data *td, struct timespec *time)
{
	/* 큐가 가득 찼는지 확인 */
	const int full = queue_full(td);
	int min_evts = 0;
	int ret;

	/* 로그 확장이 필요하면 모든 진행 중인 I/O를 완료시킴 */
	if (td->flags & TD_F_REGROW_LOGS)
		return io_u_quiesce(td);

	/*
	 * if the queue is full, we MUST reap at least 1 event
	 */
	/* [한국어] 최소 수거 이벤트 수 결정: 큐가 가득 찼으면 최소 1개 */
	min_evts = min(td->o.iodepth_batch_complete_min, td->cur_depth);
	if ((full && !min_evts) || !td->o.iodepth_batch_complete_min)
		min_evts = 1;

	/* 속도 체크가 필요하면 시간 기록 */
	if (time && should_check_rate(td))
		fio_gettime(time, NULL);

	/* 완료 이벤트를 수거하고, 큐 깊이가 iodepth_low 이하가 될 때까지 반복 */
	do {
		ret = io_u_queued_complete(td, min_evts);
		if (ret < 0)
			break;
	} while (full && (td->cur_depth > td->o.iodepth_low));

	return ret;
}

/*
 * [한국어] io_queue_event - I/O 큐잉 결과를 처리하는 이벤트 핸들러
 *
 * @td:           스레드 데이터
 * @io_u:         I/O 유닛 구조체
 * @ret:          td_io_queue()의 반환값 포인터 (FIO_Q_COMPLETED/QUEUED/BUSY)
 * @ddir:         I/O 방향
 * @bytes_issued: 발행된 바이트 수 추적 포인터
 * @from_verify:  검증(verify) 경로에서 호출되었는지 여부
 * @comp_time:    완료 시간 저장용 timespec
 *
 * 반환값: 0 = 정상 계속, 1 = 루프 중단 필요
 *
 * 역할: td_io_queue()의 반환값에 따라 적절한 후처리를 수행한다.
 *       - COMPLETED: 동기적으로 완료됨. 잔여 데이터(resid) 처리 포함.
 *       - QUEUED: 비동기로 큐에 들어감. commit 훅이 없으면 queued 처리.
 *       - BUSY: 엔진이 바쁨. io_u를 재큐잉하고 commit 시도.
 *
 * I/O 흐름에서의 위치:
 *   get_io_u -> prep -> queue -> [io_queue_event] -> commit -> getevents
 */
int io_queue_event(struct thread_data *td, struct io_u *io_u, int *ret,
		   enum fio_ddir ddir, uint64_t *bytes_issued, int from_verify,
		   struct timespec *comp_time)
{
	switch (*ret) {
	case FIO_Q_COMPLETED:
		/* [한국어] I/O가 동기적으로 즉시 완료된 경우 */
		if (io_u->error) {
			/* 에러 발생: 에러 코드를 반환값으로 설정 */
			*ret = -io_u->error;
			invalidate_inflight(td, io_u);
			clear_io_u(td, io_u);
		} else if (io_u->resid) {
			/* [한국어] 부분 완료(short I/O): 잔여 데이터가 있음 */
			long long bytes = io_u->xfer_buflen - io_u->resid;
			struct fio_file *f = io_u->file;

			if (bytes_issued)
				*bytes_issued += bytes;

			if (!from_verify)
				trim_io_piece(io_u);

			/*
			 * zero read, fail
			 */
			/* [한국어] 0바이트 읽기는 실패로 처리 */
			if (!bytes) {
				if (!from_verify)
					unlog_io_piece(td, io_u);
				td_verror(td, EIO, "full resid");
				invalidate_inflight(td, io_u);
				clear_io_u(td, io_u);
				break;
			}

			/* 잔여 데이터로 io_u를 업데이트하여 재제출 준비 */
			io_u->xfer_buflen = io_u->resid;
			io_u->xfer_buf += bytes;
			io_u->offset += bytes;

			/* short I/O 통계 기록 */
			if (ddir_rw(io_u->ddir))
				td->ts.short_io_u[io_u->ddir]++;

			/* 파일 끝에 도달했으면 동기화 완료 처리 */
			if (io_u->offset == f->real_file_size)
				goto sync_done;

			/* 잔여 데이터를 처리하기 위해 io_u를 재큐잉 */
			requeue_io_u(td, &io_u);
		} else {
sync_done:
			/* [한국어] I/O 완전 완료: 속도 체크를 위한 시간 기록 */
			if (comp_time && should_check_rate(td))
				fio_gettime(comp_time, NULL);

			*ret = io_u_sync_complete(td, io_u);
			if (*ret < 0)
				break;
		}

		/* 로그 확장이 필요하면 수행 */
		if (td->flags & TD_F_REGROW_LOGS)
			regrow_logs(td);

		/*
		 * when doing I/O (not when verifying),
		 * check for any errors that are to be ignored
		 */
		/* [한국어] verify 경로에서 호출된 경우 에러 체크 건너뜀 */
		if (!from_verify)
			break;

		return 0;
	case FIO_Q_QUEUED:
		/*
		 * if the engine doesn't have a commit hook,
		 * the io_u is really queued. if it does have such
		 * a hook, it has to call io_u_queued() itself.
		 */
		/* [한국어] 비동기 큐잉 완료.
		 * commit 훅이 없는 엔진은 여기서 io_u_queued() 호출.
		 * commit 훅이 있는 엔진은 commit 시에 자체적으로 호출함. */
		if (td->io_ops->commit == NULL)
			io_u_queued(td, io_u);
		if (bytes_issued)
			*bytes_issued += io_u->xfer_buflen;
		break;
	case FIO_Q_BUSY:
		/* [한국어] 엔진이 바쁨: io_u를 재큐잉하고 commit으로 밀어넣기 시도 */
		if (!from_verify)
			unlog_io_piece(td, io_u);
		requeue_io_u(td, &io_u);
		td_io_commit(td);
		break;
	default:
		/* [한국어] 예상치 못한 에러: 음수 반환값 */
		assert(*ret < 0);
		td_verror(td, -(*ret), "td_io_queue");
		break;
	}

	/* 에러 발생 시 루프 중단 여부 판단 */
	if (break_on_this_error(td, ddir, ret))
		return 1;

	return 0;
}

/*
 * [한국어] io_in_polling - 폴링 모드 여부 확인
 *
 * @td: 스레드 데이터
 *
 * 반환값: true = 폴링 모드 (batch_complete_min과 max 모두 0)
 *
 * 역할: iodepth_batch_complete_min/max가 모두 0이면 폴링 모드이다.
 *       폴링 모드에서는 I/O를 제출할 때마다 완료를 확인한다.
 */
static inline bool io_in_polling(struct thread_data *td)
{
	return !td->o.iodepth_batch_complete_min &&
		   !td->o.iodepth_batch_complete_max;
}
/*
 * Unlinks files from thread data fio_file structure
 */
/*
 * [한국어] unlink_all_files - 스레드가 사용하는 모든 파일을 삭제
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 양수 = 에러
 *
 * 역할: unlink_each_loop 옵션이 설정된 경우, 각 루프 반복 시작 시
 *       이전 루프의 파일을 삭제하기 위해 호출된다.
 */
static int unlink_all_files(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int ret = 0;

	/* 모든 파일을 순회하며 일반 파일만 삭제 */
	for_each_file(td, f, i) {
		if (f->filetype != FIO_TYPE_FILE)
			continue;  /* 일반 파일이 아니면 건너뜀 */
		ret = td_io_unlink_file(td, f);
		if (ret)
			break;
	}

	if (ret)
		td_verror(td, ret, "unlink_all_files");

	return ret;
}

/*
 * Check if io_u will overlap an in-flight IO in the queue
 */
/*
 * [한국어] in_flight_overlap - 새 io_u가 진행 중인 I/O와 겹치는지 검사
 *
 * @q:    I/O 유닛 큐 (전체 io_u 목록)
 * @io_u: 검사할 새 io_u
 *
 * 반환값: true = 겹침 발생, false = 겹치지 않음
 *
 * 역할: serialize_overlap 옵션이 설정된 경우, 같은 영역에 대한
 *       동시 I/O를 방지하기 위해 오프셋 범위가 겹치는지 확인한다.
 *       겹침이 발견되면 FIO_Q_BUSY를 반환하여 재시도하게 한다.
 */
bool in_flight_overlap(struct io_u_queue *q, struct io_u *io_u)
{
	bool overlap;
	struct io_u *check_io_u;
	unsigned long long x1, x2, y1, y2;
	int i;

	/* 새 io_u의 오프셋 범위 [x1, x2) */
	x1 = io_u->offset;
	x2 = io_u->offset + io_u->buflen;
	overlap = false;
	/* 큐의 모든 io_u를 순회하며 겹침 검사 */
	io_u_qiter(q, check_io_u, i) {
		if (check_io_u->flags & IO_U_F_FLIGHT) {
			/* 진행 중인(in-flight) io_u의 오프셋 범위 [y1, y2) */
			y1 = check_io_u->offset;
			y2 = check_io_u->offset + check_io_u->buflen;

			/* 두 범위가 겹치는지 확인: x1 < y2 && y1 < x2 */
			if (x1 < y2 && y1 < x2) {
				overlap = true;
				dprint(FD_IO, "in-flight overlap: %llu/%llu, %llu/%llu\n",
						x1, io_u->buflen,
						y1, check_io_u->buflen);
				break;
			}
		}
	}

	return overlap;
}

/*
 * [한국어] io_u_submit - io_u를 I/O 엔진에 제출
 *
 * @td:   스레드 데이터
 * @io_u: 제출할 io_u
 *
 * 반환값: FIO_Q_COMPLETED, FIO_Q_QUEUED, 또는 FIO_Q_BUSY
 *
 * 역할: serialize_overlap 옵션이 설정되어 있고 진행 중인 I/O가 있을 때,
 *       겹침 검사를 수행한 후 td_io_queue()를 호출하여 실제 제출한다.
 *       겹침이 발견되면 FIO_Q_BUSY를 반환한다.
 *
 * I/O 흐름에서의 위치:
 *   get_io_u -> prep -> [io_u_submit(=queue)] -> commit -> getevents
 */
static enum fio_q_status io_u_submit(struct thread_data *td, struct io_u *io_u)
{
	/*
	 * Check for overlap if the user asked us to, and we have
	 * at least one IO in flight besides this one.
	 */
	/* [한국어] 겹침 검사: serialize_overlap이 설정되고 큐 깊이 > 1인 경우 */
	if (td->o.serialize_overlap && td->cur_depth > 1 &&
	    in_flight_overlap(&td->io_u_all, io_u))
		return FIO_Q_BUSY;

	/* I/O 엔진의 queue 콜백을 호출하여 실제 제출 */
	return td_io_queue(td, io_u);
}

/*
 * The main verify engine. Runs over the writes we previously submitted,
 * reads the blocks back in, and checks the crc/md5 of the data.
 */
/*
 * [한국어] do_verify - 메인 검증 엔진
 *
 * @td:           스레드 데이터
 * @verify_bytes: 검증할 총 바이트 수
 *
 * 역할: do_io()에서 수행한 쓰기 I/O를 다시 읽어들여
 *       CRC/MD5 등의 체크섬으로 데이터 무결성을 검증한다.
 *
 * 동작 흐름:
 *   1) 모든 파일에 대해 sync + 캐시 무효화 (디스크에서 직접 읽기 보장)
 *   2) TD_VERIFYING 상태로 전환
 *   3) 루프: io_u 할당 -> verify 핸들러 설정 -> 제출 -> 완료 대기
 *   4) 완료된 io_u의 데이터를 체크섬으로 검증
 */
static void do_verify(struct thread_data *td, uint64_t verify_bytes)
{
	struct fio_file *f;
	struct io_u *io_u;
	unsigned int i;
	int ret;

	dprint(FD_VERIFY, "starting loop\n");

	/*
	 * sync io first and invalidate cache, to make sure we really
	 * read from disk.
	 */
	/* [한국어] 검증 전 모든 열린 파일에 대해 sync + 캐시 무효화
	 * 이렇게 해야 페이지 캐시가 아닌 실제 디스크에서 데이터를 읽을 수 있다. */
	for_each_file(td, f, i) {
		if (!fio_file_open(f))
			continue;
		if (fio_io_sync(td, f, DDIR_SYNC))
			break;
		if (file_invalidate_cache(td, f))
			break;
	}

	check_update_rusage(td);

	if (td->error)
		return;

	/* TD_VERIFYING 상태로 전환 */
	td_set_runstate(td, TD_VERIFYING);

	io_u = NULL;
	/* [한국어] 메인 검증 루프: terminate 플래그가 설정될 때까지 반복 */
	while (!td->terminate) {
		enum fio_ddir ddir;
		int full;

		/* 타임스탬프 캐시 갱신 */
		update_ts_cache(td);
		check_update_rusage(td);

		/* 실행 시간 초과 확인 (두 번 확인으로 캐시 정확도 보장) */
		if (runtime_exceeded(td, &td->ts_cache)) {
			__update_ts_cache(td);
			if (runtime_exceeded(td, &td->ts_cache)) {
				fio_mark_td_terminate(td);
				break;
			}
		}

		/* 흐름 제어 임계값 초과 시 대기 */
		if (flow_threshold_exceeded(td))
			continue;

		if (!td->o.experimental_verify) {
			/* [한국어] 일반 검증 모드: io_u를 직접 할당하고 검증 목록에서
			 * 다음 검증 대상을 가져옴 */
			io_u = __get_io_u(td);
			if (!io_u)
				break;

			/* 다음 검증 대상 가져오기 (이전에 기록한 위치/크기 정보) */
			if (get_next_verify(td, io_u)) {
				put_io_u(td, io_u);
				break;
			}

			/* I/O 엔진에 prep */
			if (td_io_prep(td, io_u)) {
				put_io_u(td, io_u);
				break;
			}
		} else {
			/* [한국어] 실험적 검증 모드: get_io_u()를 사용하여
			 * 쓰기/트림 I/O를 읽기로 변환하여 검증 */
			if (td->bytes_verified + td->o.rw_min_bs > verify_bytes)
				break;

			while ((io_u = get_io_u(td)) != NULL) {
				if (IS_ERR_OR_NULL(io_u)) {
					io_u = NULL;
					ret = FIO_Q_BUSY;
					goto reap;
				}

				/*
				 * We are only interested in the places where
				 * we wrote or trimmed IOs. Turn those into
				 * reads for verification purposes.
				 */
				/* [한국어] 쓰기/트림 위치만 관심 대상.
				 * 읽기는 무시하고, 쓰기/트림을 읽기로 변환하여 검증 */
				if (io_u->ddir == DDIR_READ) {
					/*
					 * Pretend we issued it for rwmix
					 * accounting
					 */
					/* [한국어] 읽기 I/O는 rwmix 통계를 위해 발행된 것으로 기록 */
					td->io_issues[DDIR_READ]++;
					put_io_u(td, io_u);
					continue;
				} else if (io_u->ddir == DDIR_TRIM) {
					/* [한국어] 트림 -> 읽기로 변환하여 검증 */
					io_u->ddir = DDIR_READ;
					io_u_set(td, io_u, IO_U_F_TRIMMED);
					if (td_io_prep(td, io_u)) {
						put_io_u(td, io_u);
						continue;
					}
					break;
				} else if (io_u->ddir == DDIR_WRITE) {
					/* [한국어] 쓰기 -> 읽기로 변환하여 검증 */
					io_u->ddir = DDIR_READ;
					io_u->numberio = td->verify_read_issues;
					td->verify_read_issues++;
					populate_verify_io_u(td, io_u);
					if (td_io_prep(td, io_u)) {
						put_io_u(td, io_u);
						continue;
					}
					break;
				} else {
					/* 그 외 방향은 무시 */
					put_io_u(td, io_u);
					continue;
				}
			}

			if (!io_u)
				break;
		}

		/* 검증 상태 확인: 특정 numberio 이후로 중단해야 하는지 */
		if (verify_state_should_stop(td, io_u->numberio)) {
			put_io_u(td, io_u);
			break;
		}

		/* [한국어] 검증 완료 콜백 설정: 비동기 또는 동기 검증 핸들러 */
		if (td->o.verify_async)
			io_u->end_io = verify_io_u_async;
		else
			io_u->end_io = verify_io_u;

		ddir = io_u->ddir;
		/* 제출 지연 시간(slat) 측정 시작 */
		if (!td->o.disable_slat)
			fio_gettime(&io_u->start_time, NULL);

		/* I/O 엔진에 제출 */
		ret = io_u_submit(td, io_u);

		/* 큐 이벤트 처리 (완료/큐잉/바쁨) */
		if (io_queue_event(td, io_u, &ret, ddir, NULL, 1, NULL))
			break;

		/*
		 * if we can queue more, do so. but check if there are
		 * completed io_u's first. Note that we can get BUSY even
		 * without IO queued, if the system is resource starved.
		 */
		/* [한국어] 큐가 가득 찼거나 BUSY 반환 시 완료 이벤트 수거 */
reap:
		full = queue_full(td) || (ret == FIO_Q_BUSY && td->cur_depth);
		if (full || io_in_polling(td))
			ret = wait_for_completions(td, NULL);

		if (ret < 0)
			break;
	}

	check_update_rusage(td);

	/* [한국어] 진행 중인 모든 I/O가 완료될 때까지 대기 */
	if (td->cur_depth)
		ret = io_u_queued_complete(td, td->cur_depth);

	/* 실행 상태를 TD_RUNNING으로 복원 */
	td_set_runstate(td, TD_RUNNING);

	dprint(FD_VERIFY, "exiting loop\n");
}

/*
 * [한국어] exceeds_number_ios - I/O 횟수 제한 초과 여부 확인
 *
 * @td: 스레드 데이터
 *
 * 반환값: true = number_ios 제한 초과, false = 아직 남음
 *
 * 역할: number_ios 옵션이 설정된 경우, 발행된 I/O 블록 수 +
 *       큐에 있는 수 + 진행 중인 수가 제한을 초과했는지 확인한다.
 */
static bool exceeds_number_ios(struct thread_data *td)
{
	unsigned long long number_ios;

	if (!td->o.number_ios)
		return false;

	/* 발행된 블록 수 + 큐 대기 + 진행 중 */
	number_ios = ddir_rw_sum(td->io_blocks);
	number_ios += td->io_u_queued + td->io_u_in_flight;

	return number_ios >= (td->o.number_ios * td->loops);
}

/*
 * [한국어] io_bytes_exceeded - 바이트 수 제한 초과 여부 확인
 *
 * @td:         스레드 데이터
 * @this_bytes: 방향별 바이트 수 배열 (io_issue_bytes 또는 this_io_bytes)
 *
 * 반환값: true = 바이트 제한 초과 또는 I/O 횟수 초과, false = 아직 남음
 *
 * 역할: I/O 루프의 종료 조건을 판단한다.
 *       혼합 읽기/쓰기인 경우 양쪽 합산, 단일 방향이면 해당 방향만 확인.
 */
static bool io_bytes_exceeded(struct thread_data *td, uint64_t *this_bytes)
{
	unsigned long long bytes, limit;

	/* 작업 유형에 따라 확인할 바이트 수 선택 */
	if (td_rw(td))
		bytes = this_bytes[DDIR_READ] + this_bytes[DDIR_WRITE];
	else if (td_write(td))
		bytes = this_bytes[DDIR_WRITE];
	else if (td_read(td))
		bytes = this_bytes[DDIR_READ];
	else
		bytes = this_bytes[DDIR_TRIM];

	/* io_size가 설정되어 있으면 그것을 사용, 아니면 size 사용 */
	if (td->o.io_size)
		limit = td->o.io_size;
	else
		limit = td->o.size;

	/* 루프 횟수를 곱하여 전체 제한 계산 */
	limit *= td->loops;
	return bytes >= limit || exceeds_number_ios(td);
}

/*
 * [한국어] io_issue_bytes_exceeded - 발행된 바이트 기준 제한 초과 확인
 *
 * 역할: do_io()의 메인 루프 종료 조건으로 사용.
 *       I/O를 "발행"한 바이트가 제한을 초과했는지 확인한다.
 *       (완료 여부와 무관하게 제출한 양 기준)
 */
static bool io_issue_bytes_exceeded(struct thread_data *td)
{
	return io_bytes_exceeded(td, td->io_issue_bytes);
}

/*
 * [한국어] io_complete_bytes_exceeded - 완료된 바이트 기준 제한 초과 확인
 *
 * 역할: do_dry_run()에서 사용. 실제 완료된 바이트가 제한을 초과했는지 확인.
 */
static bool io_complete_bytes_exceeded(struct thread_data *td)
{
	return io_bytes_exceeded(td, td->this_io_bytes);
}

/*
 * used to calculate the next io time for rate control
 *
 */
/*
 * [한국어] usec_for_io - 속도 제한을 위한 다음 I/O 시각 계산
 *
 * @td:   스레드 데이터
 * @ddir: I/O 방향
 *
 * 반환값: 다음 I/O를 수행해야 할 시각 (마이크로초, epoch 기준)
 *
 * 역할: rate 옵션에 따라 I/O 간 간격을 계산하여 속도를 제한한다.
 *       - RATE_PROCESS_POISSON: 포아송 분포를 따르는 랜덤 간격
 *       - 일반 모드: 일정한 속도를 유지하기 위한 시간 계산
 */
static long long usec_for_io(struct thread_data *td, enum fio_ddir ddir)
{
	uint64_t bps = td->rate_bps[ddir];  /* 목표 초당 바이트 수 */

	assert(!(td->flags & TD_F_CHILD));

	if (td->o.rate_process == RATE_PROCESS_POISSON) {
		/* [한국어] 포아송 프로세스: 지수 분포를 따르는 랜덤 I/O 간격 */
		uint64_t val, iops;

		iops = bps / td->o.min_bs[ddir];  /* 목표 IOPS 계산 */
		/* 지수 분포에서 랜덤 간격 생성: -ln(U) / lambda */
		val = (int64_t) (1000000 / iops) *
				-logf(__rand_0_1(&td->poisson_state[ddir]));
		if (val) {
			dprint(FD_RATE, "poisson rate iops=%llu, ddir=%d\n",
					(unsigned long long) 1000000 / val,
					ddir);
		}
		td->last_usec[ddir] += val;
		return td->last_usec[ddir];
	}

	if (!bps)
		return 0;

	/*
	 * For rate_iops option combined with bssplit, recover
	 * the user provided IOPS value and calculate the I/O delay
	 * based on this value, not on bps.
	 */
	/* [한국어] rate_iops + bssplit 조합: IOPS 기반으로 지연 계산 */
	if (!td->o.rate[ddir] && td->o.bssplit_nr[ddir]) {
		uint64_t iops = bps / td->o.min_bs[ddir];

		if (!iops)
			return 0;

		td->last_usec[ddir] += (int64_t)(1000000 / iops);
		return td->last_usec[ddir];
	} else {
		/* [한국어] 일반 속도 제한: 발행된 바이트 수에 기반한 시간 계산 */
		uint64_t bytes = td->rate_io_issue_bytes[ddir];
		uint64_t secs = bytes / bps;
		uint64_t remainder = bytes % bps;

		return remainder * 1000000 / bps + secs * 1000000;
	}
}

/*
 * [한국어] init_thinktime - 씽크타임(thinktime) 초기화
 *
 * @td: 스레드 데이터
 *
 * 역할: thinktime_blocks_type에 따라 씽크타임 카운터를 초기화한다.
 *       COMPLETE 타입이면 완료된 블록, ISSUE 타입이면 발행된 블록 기준.
 */
static void init_thinktime(struct thread_data *td)
{
	if (td->o.thinktime_blocks_type == THINKTIME_BLOCKS_TYPE_COMPLETE)
		td->thinktime_blocks_counter = td->io_blocks;
	else
		td->thinktime_blocks_counter = td->io_issues;
	td->last_thinktime = td->epoch;
	td->last_thinktime_blocks = 0;
}

/*
 * [한국어] handle_thinktime - I/O 간 사고 시간(thinktime) 처리
 *
 * @td:   스레드 데이터
 * @ddir: I/O 방향
 * @time: 속도 체크용 시간 저장
 *
 * 역할: thinktime 옵션이 설정된 경우, 일정 블록 수마다 또는
 *       일정 I/O 시간마다 의도적으로 대기한다.
 *       실제 애플리케이션의 사고 시간(think time)을 시뮬레이션한다.
 *
 *       대기 시간은 두 부분으로 나뉜다:
 *       1) thinktime_spin: CPU 바쁜 대기 (spin)
 *       2) 나머지: usleep으로 슬립
 */
static void handle_thinktime(struct thread_data *td, enum fio_ddir ddir,
			     struct timespec *time)
{
	unsigned long long b;
	unsigned long long runtime_left;
	uint64_t total;
	int left;
	struct timespec now;
	bool stall = false;  /* 대기 필요 여부 */

	/* thinktime_iotime이 설정된 경우: I/O 시간 기준 대기 */
	if (td->o.thinktime_iotime) {
		fio_gettime(&now, NULL);
		if (utime_since(&td->last_thinktime, &now)
		    >= td->o.thinktime_iotime) {
			stall = true;
		} else if (!fio_option_is_set(&td->o, thinktime_blocks)) {
			/*
			 * When thinktime_iotime is set and thinktime_blocks is
			 * not set, skip the thinktime_blocks check, since
			 * thinktime_blocks default value 1 does not work
			 * together with thinktime_iotime.
			 */
			/* [한국어] thinktime_iotime이 설정되고 thinktime_blocks가
			 * 설정되지 않았으면 블록 수 기준 검사를 건너뜀 */
			return;
		}

	}

	/* thinktime_blocks 기준 검사: 일정 블록 수마다 대기 */
	b = ddir_rw_sum(td->thinktime_blocks_counter);
	if (b >= td->last_thinktime_blocks + td->o.thinktime_blocks)
		stall = true;

	if (!stall)
		return;

	/* [한국어] 대기 전에 진행 중인 모든 I/O를 완료시킴 */
	io_u_quiesce(td);

	/* spin 대기 시간 결정 (남은 런타임 고려) */
	left = td->o.thinktime_spin;
	if (td->o.timeout) {
		runtime_left = td->o.timeout - utime_since_now(&td->epoch);
		if (runtime_left < (unsigned long long)left)
			left = runtime_left;
	}

	/* CPU 바쁜 대기(spin) 수행 */
	total = 0;
	if (left)
		total = usec_spin(left);

	/*
	 * usec_spin() might run for slightly longer than intended in a VM
	 * where the vCPU could get descheduled or the hypervisor could steal
	 * CPU time. Ensure "left" doesn't become negative.
	 */
	/* [한국어] 남은 씽크타임에서 spin 시간을 빼고, usleep으로 나머지 대기 */
	if (total < td->o.thinktime)
		left = td->o.thinktime - total;
	else
		left = 0;

	if (td->o.timeout) {
		runtime_left = td->o.timeout - utime_since_now(&td->epoch);
		if (runtime_left < (unsigned long long)left)
			left = runtime_left;
	}

	/* 슬립 대기 수행 */
	if (left)
		total += usec_sleep(td, left);

	/*
	 * If we're ignoring thinktime for the rate, add the number of bytes
	 * we would have done while sleeping, minus one block to ensure we
	 * start issuing immediately after the sleep.
	 */
	/* [한국어] rate_ign_think 옵션: 씽크타임 동안 발행했을 바이트 수를
	 * 속도 계산에 보정하여, 대기 후 즉시 I/O를 재개할 수 있도록 함 */
	if (total && td->rate_bps[ddir] && td->o.rate_ign_think) {
		uint64_t missed = (td->rate_bps[ddir] * total) / 1000000ULL;
		uint64_t bs = td->o.min_bs[ddir];
		uint64_t usperop = bs * 1000000ULL / td->rate_bps[ddir];
		uint64_t over;

		if (usperop <= total)
			over = bs;
		else
			over = (usperop - total) / usperop * -bs;

		td->rate_io_issue_bytes[ddir] += (missed - over);
		/* 포아송 모드에서의 보정 */
		td->last_usec[ddir] += total;
	}

	/* 속도 체크용 시간 갱신 */
	if (time && should_check_rate(td))
		fio_gettime(time, NULL);

	/* 씽크타임 기준값 갱신 */
	td->last_thinktime_blocks = b;
	if (td->o.thinktime_iotime) {
		fio_gettime(&now, NULL);
		td->last_thinktime = now;
	}
}

/*
 * Add numberio from io_u to the inflight log.
 */
/*
 * [한국어] log_inflight - 진행 중인 쓰기 I/O의 번호를 인플라이트 로그에 기록
 *
 * @td:   스레드 데이터
 * @io_u: I/O 유닛
 *
 * 역할: verify_state_save가 활성화된 경우, 쓰기 I/O의 순번(numberio)을
 *       인플라이트 배열에 기록한다. 이를 통해 검증 시 어떤 쓰기가
 *       완료되었고 어떤 것이 진행 중이었는지 추적할 수 있다.
 *       원자적 저장(atomic_store_release)으로 순서를 보장한다.
 */
void log_inflight(struct thread_data *td, struct io_u *io_u)
{
	int idx, i;

	/* 인플라이트 로깅이 비활성화되었거나 쓰기가 아니면 무시 */
	if (!td->inflight_numberio || io_u->ddir != DDIR_WRITE)
		return;

	/* 이미 인플라이트 슬롯이 할당되었으면 에러 */
	if (io_u->inflight_idx != -1) {
		log_err("inflight_idx already set: inflight_idx=%d\n",
			io_u->inflight_idx);
		abort();
	}

	/* 발행 순번 일치 확인 */
	if (td->inflight_issued != io_u->numberio) {
		log_err("inflight_issued does not match: numberio=%"PRIu64", inflight_issued=%"PRIu64"\n",
			io_u->numberio, td->inflight_issued);
		abort();
	}

	/* Walk the inflight list until we find a free slot. */
	/* [한국어] 인플라이트 배열에서 빈 슬롯을 찾아 할당 */
	idx = td->next_inflight_numberio_idx;
	for (i = 0; i < td->o.iodepth; i++) {
		if (td->inflight_numberio[idx] == INVALID_NUMBERIO) {
			/*
			 * The order here is important - we must "protect" this write in the
			 * inflight list before making it visible in inflight_issued.
			 */
			/* [한국어] 순서 중요: 먼저 인플라이트 리스트에 기록한 후
			 * inflight_issued를 갱신해야 검증 스레드에서 일관성 유지 */
			atomic_store_release(&td->inflight_numberio[idx], io_u->numberio);
			td->next_inflight_numberio_idx = (idx + 1) % td->o.iodepth;
			io_u->inflight_idx = idx;

			atomic_store_release(&td->inflight_issued, io_u->numberio + 1);
			dprint(FD_VERIFY, "log_inflight: numberio=%"PRIu64", inflight_idx=%d\n",
				io_u->numberio, idx);
			return;
		}
		idx = (idx + 1) % td->o.iodepth;
	}

	/* 빈 슬롯을 찾지 못함 - 치명적 에러 */
	log_err("failed to allocate inflight slot: next_inflight_numberio_idx=%u\n",
		td->next_inflight_numberio_idx);
	abort();
}

/*
 * Invalidate inflight log entry.
 */
/*
 * [한국어] invalidate_inflight - 인플라이트 로그 항목 무효화
 *
 * @td:   스레드 데이터
 * @io_u: I/O 유닛
 *
 * 역할: I/O가 완료되거나 에러로 취소될 때, 해당 io_u의 인플라이트
 *       로그 항목을 INVALID_NUMBERIO로 설정하여 슬롯을 해제한다.
 */
void invalidate_inflight(struct thread_data *td, struct io_u *io_u)
{
	if (!td->inflight_numberio ||
		io_u->ddir != DDIR_WRITE ||
		io_u->inflight_idx == -1) {
		return;
	}

	dprint(FD_VERIFY, "invalidate_inflight: numberio=%"PRIu64", inflight_idx=%d\n",
		io_u->numberio, io_u->inflight_idx);

	/* 이미 무효화되었으면 에러 */
	if (td->inflight_numberio[io_u->inflight_idx] == INVALID_NUMBERIO) {
		log_err("inflight entry already invalid: numberio=%"PRIu64", inflight_idx=%d\n",
			io_u->numberio, io_u->inflight_idx);
		abort();
	} else if (td->inflight_numberio[io_u->inflight_idx] != io_u->numberio) {
		/* numberio 불일치: 데이터 손상 가능성 */
		log_err("inflight entry numberio does not match: expected numberio=%"PRIu64", observed numberio=%"PRIu64", inflight_idx=%d\n",
			io_u->numberio, td->inflight_numberio[io_u->inflight_idx], io_u->inflight_idx);
		abort();
	}

	/* 슬롯을 무효화하여 재사용 가능하게 함 */
	atomic_store_release(&td->inflight_numberio[io_u->inflight_idx], INVALID_NUMBERIO);
	io_u->inflight_idx = -1;
}

/*
 * Clear inflight log.
 */
/*
 * [한국어] clear_inflight - 인플라이트 로그 전체 초기화
 *
 * @td: 스레드 데이터
 *
 * 역할: 모든 인플라이트 슬롯을 INVALID_NUMBERIO로 초기화하고,
 *       인덱스와 발행 카운터를 리셋한다.
 *       루프 반복 사이에 호출되어 이전 루프의 상태를 정리한다.
 */
void clear_inflight(struct thread_data *td)
{
	int i;

	if (!td->inflight_numberio)
		return;

	/* 모든 슬롯을 무효화 */
	for (i = 0; i < td->o.iodepth; i++)
		td->inflight_numberio[i] = INVALID_NUMBERIO;

	td->next_inflight_numberio_idx = 0;
	/*
	 * Experimental verify can increment io_issues for writes, so catch
	 * inflight_issued up in between loops.
	 */
	/* [한국어] 실험적 검증 모드에서 io_issues가 증가할 수 있으므로
	 * 루프 간 inflight_issued를 동기화 */
	td->inflight_issued = td->io_issues[DDIR_WRITE];
}

/*
 * Main IO worker function. It retrieves io_u's to process and queues
 * and reaps them, checking for rate and errors along the way.
 *
 * Returns number of bytes written and trimmed.
 */
/*
 * ============================================================================
 * [한국어] do_io - fio의 핵심 메인 I/O 루프 (가장 중요한 함수)
 * ============================================================================
 *
 * @td:         스레드 데이터
 * @bytes_done: 방향별 완료 바이트 수를 반환하는 배열 [DDIR_RWDIR_CNT]
 *
 * 역할: fio의 실제 I/O 워크로드를 실행하는 핵심 함수이다.
 *       아래의 I/O 파이프라인을 반복 실행한다:
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  1. get_io_u()      - freelist에서 io_u 할당,           │
 *   │                       오프셋/크기/방향 결정              │
 *   │  2. td_io_prep()    - I/O 엔진에 io_u 준비 (prep 콜백)  │
 *   │  3. io_u_submit()   - I/O 엔진에 제출 (queue 콜백)      │
 *   │     = td_io_queue()                                     │
 *   │  4. td_io_commit()  - 배치 제출 수행 (commit 콜백)       │
 *   │  5. io_u_queued_complete() - 완료 이벤트 수거            │
 *   │     = td_io_getevents()     (getevents 콜백)            │
 *   │  6. put_io_u()      - io_u를 freelist로 반환             │
 *   └─────────────────────────────────────────────────────────┘
 *
 * 루프 종료 조건:
 *   - 발행 바이트가 total_bytes 이상
 *   - number_ios 초과
 *   - timeout 초과 (time_based 모드)
 *   - 에러 발생
 *   - terminate 플래그 설정 (시그널 등)
 *
 * 추가 기능:
 *   - 속도 제한 (rate) 검사
 *   - 씽크타임 (thinktime) 처리
 *   - 검증 (verify) 패턴 기록
 *   - 오프로드 모드 (IO_MODE_OFFLOAD) 지원
 *   - 지연 시간 타겟 (latency_target) 검사
 */
static void do_io(struct thread_data *td, uint64_t *bytes_done)
{
	unsigned int i;
	int ret = 0;
	uint64_t total_bytes, bytes_issued = 0;  /* 총 목표 바이트, 발행된 바이트 */

	/* 현재까지의 bytes_done 스냅샷 저장 (나중에 차이를 계산하기 위해) */
	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		bytes_done[i] = td->bytes_done[i];

	/* [한국어] 런 상태 설정: 워밍업 구간이면 TD_RAMP, 아니면 TD_RUNNING */
	if (in_ramp_period(td))
		td_set_runstate(td, TD_RAMP);
	else
		td_set_runstate(td, TD_RUNNING);

	/* 지연 시간 타겟 초기화 */
	lat_target_init(td);

	/* [한국어] 총 I/O 목표 바이트 수 결정 */
	total_bytes = td->o.size;

	/*
	* Allow random overwrite workloads to write up to io_size
	* before starting verification phase as 'size' doesn't apply.
	*/
	/* [한국어] 랜덤 덮어쓰기 + norandommap 워크로드에서는
	 * io_size가 size보다 크면 io_size까지 허용 */
	if (td_write(td) && td_random(td) && td->o.norandommap)
		total_bytes = max(total_bytes, (uint64_t) td->o.io_size);

	/*
	 * Don't break too early if io_size > size. The exception is when
	 * verify is enabled.
	 */
	/* [한국어] 순차 혼합 읽기/쓰기에서 검증 없이 io_size > size이면
	 * 조기 종료하지 않도록 total_bytes를 io_size로 확대 */
	if (td_rw(td) && !td_random(td) && td->o.verify == VERIFY_NONE)
		total_bytes = max(total_bytes, (uint64_t)td->o.io_size);

	/*
	 * If verify_backlog is enabled, we'll run the verify in this
	 * handler as well. For that case, we may need up to twice the
	 * amount of bytes.
	 */
	/* [한국어] verify_backlog 사용 시 쓰기 + 검증 읽기로 2배의 바이트가 필요 */
	if (td->o.verify != VERIFY_NONE &&
	   (td_write(td) && td->o.verify_backlog))
		total_bytes += td->o.size;

	/* In trimwrite mode, each byte is trimmed and then written, so
	 * allow total_bytes or number of ios to be twice as big */
	/* [한국어] trimwrite 모드: 각 바이트가 트림 후 쓰기되므로 2배 필요 */
	if (td_trimwrite(td)) {
		total_bytes += td->total_io_size;
		td->o.number_ios *= 2;
	}

	/*
	 * ================================================================
	 * [한국어] *** 메인 I/O 루프 시작 ***
	 *
	 * 루프 진입 조건:
	 *   - iolog 파일이 있고 처리할 항목이 남았거나
	 *   - trim 목록에 항목이 있거나
	 *   - 발행 바이트가 제한을 초과하지 않았거나
	 *   - time_based 모드인 경우
	 * ================================================================
	 */
	while ((td->o.read_iolog_file && !flist_empty(&td->io_log_list)) ||
		(!flist_empty(&td->trim_list)) || !io_issue_bytes_exceeded(td) ||
		td->o.time_based) {
		struct timespec comp_time;  /* 완료 시간 (속도 체크용) */
		struct io_u *io_u;
		int full;
		enum fio_ddir ddir;

		/* 리소스 사용량 업데이트 요청 확인 */
		check_update_rusage(td);

		/* 종료 플래그 확인 */
		if (td->terminate || td->done)
			break;

		/* 타임스탬프 캐시 갱신 */
		update_ts_cache(td);

		/* [한국어] 실행 시간 초과 확인 (캐시 + 실제 시간으로 이중 확인) */
		if (runtime_exceeded(td, &td->ts_cache)) {
			__update_ts_cache(td);
			if (runtime_exceeded(td, &td->ts_cache)) {
				fio_mark_td_terminate(td);
				break;
			}
		}

		/* 흐름 제어 임계값 초과 시 이번 반복 건너뜀 */
		if (flow_threshold_exceeded(td))
			continue;

		/*
		 * Break if we exceeded the bytes. The exception is time
		 * based runs, but we still need to break out of the loop
		 * for those to run verification, if enabled.
		 * Jobs read from iolog do not use this stop condition.
		 */
		/* [한국어] 발행 바이트 초과 시 루프 탈출.
		 * 단, time_based + verify가 아닌 경우에만 계속 실행.
		 * iolog에서 읽는 작업은 이 조건을 사용하지 않음. */
		if (bytes_issued >= total_bytes &&
		    !td->o.read_iolog_file &&
		    (!td->o.time_based ||
		     (td->o.time_based && td->o.verify != VERIFY_NONE)))
			break;

		/* ============================================================
		 * [한국어] 단계 1: get_io_u() - I/O 유닛 할당
		 *
		 * freelist에서 io_u를 하나 꺼내고, 다음 I/O의
		 * 오프셋, 크기, 방향(읽기/쓰기/트림)을 결정한다.
		 * 내부적으로 get_next_offset(), get_next_buflen() 등을 호출한다.
		 * ============================================================ */
		io_u = get_io_u(td);
		if (IS_ERR_OR_NULL(io_u)) {
			int err = PTR_ERR(io_u);

			io_u = NULL;
			ddir = DDIR_INVAL;
			if (err == -EBUSY) {
				/* 엔진이 바쁨: 완료 이벤트 수거 후 재시도 */
				ret = FIO_Q_BUSY;
				goto reap;
			}
			if (td->o.latency_target)
				goto reap;
			break;  /* 할당 실패: 루프 종료 */
		}

		/* [한국어] 쓰기 + 검증 모드: 검증 패턴을 io_u 버퍼에 기록
		 * 나중에 do_verify()에서 이 패턴을 읽어 데이터 무결성 확인 */
		if (io_u->ddir == DDIR_WRITE && td->flags & TD_F_DO_VERIFY) {
			if (!(io_u->flags & IO_U_F_PATTERN_DONE)) {
				io_u_set(td, io_u, IO_U_F_PATTERN_DONE);
				io_u->numberio = td->io_issues[io_u->ddir];
				populate_verify_io_u(td, io_u);  /* 검증 헤더/패턴 채우기 */
				log_inflight(td, io_u);           /* 인플라이트 로그에 기록 */
			}
		}

		ddir = io_u->ddir;

		/*
		 * Add verification end_io handler if:
		 *	- Asked to verify (!td_rw(td))
		 *	- Or the io_u is from our verify list (mixed write/ver)
		 */
		/* [한국어] 읽기 I/O에 검증 핸들러 연결:
		 * - 순수 읽기 워크로드에서 검증이 활성화된 경우
		 * - 또는 verify_backlog에서 가져온 검증 목록의 io_u인 경우 */
		if (td->o.verify != VERIFY_NONE && io_u->ddir == DDIR_READ &&
		    ((io_u->flags & IO_U_F_VER_LIST) || !td_rw(td))) {

			/*
			 * For read only workloads generate the seed. This way
			 * we can still verify header seed at any later
			 * invocation.
			 */
			/* [한국어] 읽기 전용 워크로드에서 시드 생성:
			 * 나중에 검증 시 헤더 시드를 확인할 수 있도록 */
			if (!td_write(td) && !td->o.verify_pattern_bytes) {
				io_u->rand_seed = __rand(&td->verify_state);
				if (sizeof(int) != sizeof(long *))
					io_u->rand_seed *= __rand(&td->verify_state);
			}

			/* 검증 상태 확인: 특정 지점 이후 중단 필요 여부 */
			if (verify_state_should_stop(td, td->io_issues[io_u->ddir])) {
				put_io_u(td, io_u);
				break;
			}

			/* 비동기 또는 동기 검증 콜백 설정 */
			if (td->o.verify_async)
				io_u->end_io = verify_io_u_async;
			else
				io_u->end_io = verify_io_u;
			td_set_runstate(td, TD_VERIFYING);
		} else if (in_ramp_period(td))
			td_set_runstate(td, TD_RAMP);     /* 워밍업 구간 */
		else
			td_set_runstate(td, TD_RUNNING);   /* 정상 실행 */

		/*
		 * Always log IO before it's issued, so we know the specific
		 * order of it. The logged unit will track when the IO has
		 * completed.
		 */
		/* [한국어] 검증을 위해 I/O 발행 전에 로그 기록.
		 * 이렇게 해야 나중에 검증 시 정확한 순서를 알 수 있다.
		 * experimental_verify 모드에서는 건너뜀. */
		if (td_write(td) && io_u->ddir == DDIR_WRITE &&
		    td->o.do_verify &&
		    td->o.verify != VERIFY_NONE &&
		    !td->o.experimental_verify)
			log_io_piece(td, io_u);

		/* ============================================================
		 * [한국어] 단계 2 & 3: I/O 제출 (prep + queue + commit)
		 *
		 * 두 가지 모드가 있다:
		 *   A) IO_MODE_OFFLOAD: 워크큐에 위임 (별도 스레드에서 처리)
		 *   B) 일반 모드: io_u_submit()으로 직접 제출
		 * ============================================================ */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD) {
			/* [한국어] 오프로드 모드: 워크큐에 io_u를 넣어 별도 스레드가 처리 */
			const unsigned long long blen = io_u->xfer_buflen;
			const enum fio_ddir __ddir = acct_ddir(io_u);

			if (td->error)
				break;

			/* 워크큐에 제출 */
			workqueue_enqueue(&td->io_wq, &io_u->work);
			ret = FIO_Q_QUEUED;

			/* I/O 발행 통계 갱신 */
			if (ddir_rw(__ddir)) {
				td->io_issues[__ddir]++;
				td->io_issue_bytes[__ddir] += blen;
				td->rate_io_issue_bytes[__ddir] += blen;
			}

			/* 속도 제한 계산 */
			if (ddir_rw(__ddir) && should_check_rate(td)) {
				td->rate_next_io_time[__ddir] = usec_for_io(td, __ddir);
				fio_gettime(&comp_time, NULL);
			}

		} else {
			/* [한국어] 일반 모드: 직접 I/O 제출
			 *
			 * io_u_submit() 내부에서:
			 *   1) serialize_overlap 검사 (겹침이면 FIO_Q_BUSY)
			 *   2) td_io_queue() 호출 = I/O 엔진의 queue 콜백
			 *      - 내부적으로 td_io_prep()도 호출됨
			 */
			ret = io_u_submit(td, io_u);

			/* 속도 제한을 위한 다음 I/O 시각 계산 */
			if (ddir_rw(ddir) && should_check_rate(td))
				td->rate_next_io_time[ddir] = usec_for_io(td, ddir);

			/* [한국어] 큐 이벤트 처리:
			 * COMPLETED면 즉시 완료 처리, QUEUED면 대기, BUSY면 재시도 */
			if (io_queue_event(td, io_u, &ret, ddir, &bytes_issued, 0, &comp_time))
				break;

			/*
			 * See if we need to complete some commands. Note that
			 * we can get BUSY even without IO queued, if the
			 * system is resource starved.
			 */
			/* ============================================================
			 * [한국어] 단계 4 & 5: 완료 이벤트 수거 (commit + getevents)
			 *
			 * 큐가 가득 찼거나 BUSY가 반환되면:
			 *   - td_io_commit()으로 배치 제출 (commit 콜백)
			 *   - io_u_queued_complete()로 완료된 I/O 수거 (getevents 콜백)
			 *   - 완료된 io_u는 put_io_u()로 freelist에 반환됨
			 * ============================================================ */
reap:
			full = queue_full(td) ||
				(ret == FIO_Q_BUSY && td->cur_depth);
			if (full || io_in_polling(td))
				ret = wait_for_completions(td, &comp_time);
		}
		if (ret < 0)
			break;

		/* [한국어] thinkcycles 옵션: CPU 사이클을 소비하여 I/O 간 지연 시뮬레이션 */
		if (ddir_rw(ddir) && td->o.thinkcycles)
			cycles_spin(td->o.thinkcycles);

		/* [한국어] thinktime 옵션: I/O 간 대기 시간 처리 */
		if (ddir_rw(ddir) && td->o.thinktime)
			handle_thinktime(td, ddir, &comp_time);

		/* 아직 데이터 전송이 없고 NOIO 엔진이 아니면 다음 반복 */
		if (!ddir_rw_sum(td->bytes_done) &&
		    !td_ioengine_flagged(td, FIO_NOIO))
			continue;

		/* [한국어] 최소 속도(ratemin) 검사: 미달 시 에러로 종료 */
		if (!in_ramp_period(td) && should_check_rate(td)) {
			if (check_min_rate(td, &comp_time)) {
				if (exitall_on_terminate || td->o.exitall_error)
					fio_terminate_threads(td->groupid, td->o.exit_what);
				td_verror(td, EIO, "check_min_rate");
				break;
			}
		}
		/* 지연 시간 타겟 검사 */
		if (!in_ramp_period(td) && td->o.latency_target)
			lat_target_check(td);
	}
	/* ================================================================
	 * [한국어] *** 메인 I/O 루프 종료 ***
	 * ================================================================ */

	check_update_rusage(td);

	/* trim 항목 누수 확인 */
	if (td->trim_entries)
		log_err("fio: %lu trim entries leaked?\n", td->trim_entries);

	/* [한국어] fill_device 모드에서 공간 부족 에러는 정상 종료로 처리 */
	if (td->o.fill_device && (td->error == ENOSPC || td->error == EDQUOT)) {
		td->error = 0;
		fio_mark_td_terminate(td);
	}
	if (!td->error) {
		struct fio_file *f;

		/* [한국어] 오프로드 모드에서는 워크큐 플러시, 일반 모드에서는 cur_depth 사용 */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD) {
			workqueue_flush(&td->io_wq);
			i = 0;
		} else
			i = td->cur_depth;

		/* [한국어] 진행 중인 모든 I/O 완료 대기 */
		if (i) {
			ret = io_u_queued_complete(td, i);
			if (td->o.fill_device &&
			    (td->error == ENOSPC || td->error == EDQUOT))
				td->error = 0;
		}

		/* [한국어] end_fsync/end_syncfs/fsync_on_close 옵션:
		 * 모든 I/O 완료 후 최종 동기화 수행 */
		if (should_fsync(td) &&
		    (td->o.end_fsync || td->o.end_syncfs ||
		     td->o.fsync_on_close)) {
			td_set_runstate(td, TD_FSYNCING);

			if (td->o.end_syncfs) {
				/* 파일 시스템 전체 동기화 */
				fio_syncfs(td);
			} else {
				/* 개별 파일별 fsync */
				for_each_file(td, f, i) {
					if (!fio_file_fsync(td, f))
						continue;

					log_err("fio: end_fsync failed for file %s\n",
						f->file_name);
				}
			}
		}
	} else {
		/* 에러 발생 시에도 진행 중인 I/O 완료 대기 */
		if (td->o.io_submit_mode == IO_MODE_OFFLOAD)
			workqueue_flush(&td->io_wq);
		ret = io_u_queued_complete(td, td->cur_depth);
	}

	/*
	 * stop job if we failed doing any IO
	 */
	/* [한국어] I/O를 전혀 수행하지 못했으면 작업 완료 플래그 설정 */
	if (!ddir_rw_sum(td->this_io_bytes))
		td->done = 1;

	/* [한국어] 이번 루프에서 수행한 바이트 수 계산 (현재 - 루프 시작 시) */
	for (i = 0; i < DDIR_RWDIR_CNT; i++)
		bytes_done[i] = td->bytes_done[i] - bytes_done[i];
}

/*
 * [한국어] init_inflight_logging - 인플라이트 로깅 초기화
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 1 = 메모리 할당 실패
 *
 * 역할: 검증(verify) + verify_state_save가 활성화된 경우,
 *       인플라이트 쓰기 추적을 위한 배열을 공유 메모리로 할당한다.
 *       배열 크기는 iodepth와 동일하다.
 */
static int init_inflight_logging(struct thread_data *td)
{
	unsigned int i;

	/* 검증이 비활성화되었거나 상태 저장이 불필요하면 건너뜀 */
	if (td->o.verify == VERIFY_NONE || !td->o.verify_state_save)
		return 0;

	/* iodepth 크기의 배열을 공유 메모리로 할당 */
	td->inflight_numberio = scalloc(td->o.iodepth, sizeof(uint64_t));
	if (!td->inflight_numberio) {
		log_err("fio: failed to alloc inflight write data\n");
		return 1;
	}

	/* 모든 슬롯을 무효 상태로 초기화 */
	for (i = 0; i < td->o.iodepth; i++)
		td->inflight_numberio[i] = INVALID_NUMBERIO;

	return 0;
}

/*
 * [한국어] free_inflight_logging - 인플라이트 로깅 메모리 해제
 *
 * @td: 스레드 데이터
 */
static void free_inflight_logging(struct thread_data *td)
{
	if (td->inflight_numberio)
		sfree(td->inflight_numberio);
}

/*
 * [한국어] cleanup_io_u - I/O 유닛 자원 정리
 *
 * @td: 스레드 데이터
 *
 * 역할: 모든 io_u를 해제하고, I/O 메모리 버퍼와 큐를 정리한다.
 *       thread_main()의 종료 경로(err 레이블)에서 호출된다.
 */
static void cleanup_io_u(struct thread_data *td)
{
	struct io_u *io_u;

	/* freelist의 모든 io_u를 해제 */
	while ((io_u = io_u_qpop(&td->io_u_freelist)) != NULL) {

		/* I/O 엔진의 io_u 해제 콜백 호출 */
		if (td->io_ops->io_u_free)
			td->io_ops->io_u_free(td, io_u);

		/* io_u 구조체 메모리 해제 */
		fio_memfree(io_u, sizeof(*io_u), td_offload_overlap(td));
	}

	/* 재큐잉된 io_u들도 해제 */
	while ((io_u = io_u_rpop(&td->io_u_requeues)) != NULL) {
		put_io_u(td, io_u);
	}

	/* I/O 데이터 버퍼 메모리 해제 */
	free_io_mem(td);

	/* 큐 자료구조 정리 */
	io_u_rexit(&td->io_u_requeues);
	io_u_qexit(&td->io_u_freelist, false);
	io_u_qexit(&td->io_u_all, td_offload_overlap(td));

	/* 인플라이트 로깅 메모리 해제 */
	free_inflight_logging(td);
}

/*
 * [한국어] init_io_u - I/O 유닛(io_u) 풀 초기화
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 1 = 에러
 *
 * 역할: iodepth 개수만큼의 io_u 구조체를 할당하고 초기화한다.
 *       io_u는 fio의 I/O 요청 단위이며, freelist에서 할당하여 사용 후 반환한다.
 *       각 io_u에는 데이터 버퍼, 오프셋, 크기, 방향 등이 포함된다.
 *
 * 초기화 순서:
 *   1) 재큐잉 링, freelist 큐, 전체 목록 큐 생성
 *   2) iodepth 개수만큼 io_u 할당 (캐시라인 정렬)
 *   3) 각 io_u를 freelist와 전체 목록에 추가
 *   4) I/O 엔진의 io_u_init 콜백 호출
 *   5) I/O 버퍼 초기화 (init_io_u_buffers)
 *   6) 인플라이트 로깅 초기화
 */
static int init_io_u(struct thread_data *td)
{
	struct io_u *io_u;
	int cl_align, i;
	int err;


	/* 큐 자료구조 초기화 */
	err = 0;
	err += !io_u_rinit(&td->io_u_requeues, td->o.iodepth);    /* 재큐잉 링 */
	err += !io_u_qinit(&td->io_u_freelist, td->o.iodepth, false); /* 프리리스트 */
	err += !io_u_qinit(&td->io_u_all, td->o.iodepth, td_offload_overlap(td)); /* 전체 목록 */

	if (err) {
		log_err("fio: failed setting up IO queues\n");
		return 1;
	}

	/* 캐시라인 크기로 정렬하여 false sharing 방지 */
	cl_align = os_cache_line_size();

	/* iodepth 개수만큼 io_u 할당 */
	for (i = 0; i < td->o.iodepth; i++) {
		void *ptr;

		if (td->terminate)
			return 1;

		/* 캐시라인 정렬된 메모리 할당 */
		ptr = fio_memalign(cl_align, sizeof(*io_u), td_offload_overlap(td));
		if (!ptr) {
			log_err("fio: unable to allocate aligned memory\n");
			return 1;
		}

		io_u = ptr;
		memset(io_u, 0, sizeof(*io_u));
		INIT_FLIST_HEAD(&io_u->verify_list);
		dprint(FD_MEM, "io_u alloc %p, index %u\n", io_u, i);

		/* io_u 기본 필드 초기화 */
		io_u->inflight_idx = -1;       /* 인플라이트 슬롯 미할당 */
		io_u->index = i;               /* 배열 인덱스 */
		io_u->flags = IO_U_F_FREE;     /* 초기 상태: 사용 가능 */
		io_u_qpush(&td->io_u_freelist, io_u);  /* freelist에 추가 */

		/*
		 * io_u never leaves this stack, used for iteration of all
		 * io_u buffers.
		 */
		/* [한국어] 전체 io_u 목록에도 추가 (순회/겹침 검사용, 이 스택에서 제거되지 않음) */
		io_u_qpush(&td->io_u_all, io_u);

		/* I/O 엔진의 io_u 초기화 콜백 호출 */
		if (td->io_ops->io_u_init) {
			int ret = td->io_ops->io_u_init(td, io_u);

			if (ret) {
				log_err("fio: failed to init engine data: %d\n", ret);
				return 1;
			}
		}
	}

	/* I/O 데이터 버퍼 초기화 */
	if (init_io_u_buffers(td))
		return 1;

	/* 인플라이트 로깅 초기화 */
	if (init_inflight_logging(td))
		return 1;

	return 0;
}

/*
 * [한국어] init_io_u_buffers - I/O 데이터 버퍼 할당 및 초기화
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 1 = 에러
 *
 * 역할: 모든 io_u가 공유하는 대규모 연속 메모리 버퍼를 할당하고,
 *       각 io_u에 max_bs 크기의 슬라이스를 할당한다.
 *       쓰기 워크로드의 경우 초기 데이터 패턴으로 버퍼를 채운다.
 *
 * 메모리 레이아웃:
 *   [io_u[0].buf | io_u[1].buf | ... | io_u[iodepth-1].buf]
 *   각 슬라이스 크기 = max_bs (또는 multi-range trim 시 trim_bs)
 */
int init_io_u_buffers(struct thread_data *td)
{
	struct io_u *io_u;
	unsigned long long max_bs, min_write, trim_bs = 0;
	int i, max_units;
	int data_xfer = 1;  /* 데이터 전송이 필요한지 여부 */
	char *p;

	max_units = td->o.iodepth;       /* io_u 개수 */
	max_bs = td_max_bs(td);          /* 최대 블록 크기 */
	min_write = td->o.min_bs[DDIR_WRITE];  /* 최소 쓰기 블록 크기 */
	/* 전체 버퍼 크기 = max_bs * iodepth */
	td->orig_buffer_size = (unsigned long long) max_bs
					* (unsigned long long) max_units;

	/* multi-range trim의 경우 trim_range 구조체 크기 사용 */
	if (td_trim(td) && td->o.num_range > 1) {
		trim_bs = td->o.num_range * sizeof(struct trim_range);
		td->orig_buffer_size = trim_bs
					* (unsigned long long) max_units;
	}

	/*
	 * For reads, writes, and multi-range trim operations we need a
	 * data buffer
	 */
	/* [한국어] NOIO 엔진이거나 데이터 전송이 없는 작업이면 버퍼 불필요 */
	if (td_ioengine_flagged(td, FIO_NOIO) ||
	    !(td_read(td) || td_write(td) || (td_trim(td) && td->o.num_range > 1)))
		data_xfer = 0;

	/*
	 * if we may later need to do address alignment, then add any
	 * possible adjustment here so that we don't cause a buffer
	 * overflow later. this adjustment may be too much if we get
	 * lucky and the allocator gives us an aligned address.
	 */
	/* [한국어] direct I/O나 메모리 정렬이 필요한 경우, 정렬 여유분 추가 */
	if (td->o.odirect || td->o.mem_align ||
	    td_ioengine_flagged(td, FIO_RAWIO))
		td->orig_buffer_size += page_mask + td->o.mem_align;

	/* hugepage 정렬 */
	if (td->o.mem_type == MEM_SHMHUGE || td->o.mem_type == MEM_MMAPHUGE) {
		unsigned long long bs;

		bs = td->orig_buffer_size + td->o.hugepage_size - 1;
		td->orig_buffer_size = bs & ~(td->o.hugepage_size - 1);
	}

	/* size_t 오버플로우 확인 */
	if (td->orig_buffer_size != (size_t) td->orig_buffer_size) {
		log_err("fio: IO memory too large. Reduce max_bs or iodepth\n");
		return 1;
	}

	/* 데이터 전송이 필요한 경우 메모리 할당 */
	if (data_xfer && allocate_io_mem(td))
		return 1;

	/* 정렬이 필요한 경우 버퍼 시작 주소를 페이지 경계로 정렬 */
	if (td->o.odirect || td->o.mem_align ||
	    td_ioengine_flagged(td, FIO_RAWIO))
		p = PTR_ALIGN(td->orig_buffer, page_mask) + td->o.mem_align;
	else
		p = td->orig_buffer;

	/* 각 io_u에 버퍼 슬라이스 할당 */
	for (i = 0; i < max_units; i++) {
		io_u = td->io_u_all.io_us[i];
		dprint(FD_MEM, "io_u alloc %p, index %u\n", io_u, i);

		if (data_xfer) {
			io_u->buf = p;
			dprint(FD_MEM, "io_u %p, mem %p\n", io_u, io_u->buf);

			/* 쓰기 워크로드: 초기 데이터로 버퍼 채우기 */
			if (td_write(td))
				io_u_fill_buffer(td, io_u, min_write, max_bs);
			if (td_write(td) && td->o.verify_pattern_bytes) {
				/*
				 * Fill the buffer with the pattern if we are
				 * going to be doing writes.
				 */
				/* [한국어] 검증 패턴이 설정된 경우 패턴으로 버퍼 채우기 */
				fill_verify_pattern(td, io_u->buf, max_bs, io_u, 0, 0);
			}
		}
		/* 다음 io_u의 버퍼 시작 주소로 이동 */
		if (td_trim(td) && td->o.num_range > 1)
			p += trim_bs;
		else
			p += max_bs;
	}

	return 0;
}

#ifdef FIO_HAVE_IOSCHED_SWITCH
/*
 * These functions are Linux specific.
 * FIO_HAVE_IOSCHED_SWITCH enabled currently means it's Linux.
 */
/*
 * [한국어] set_ioscheduler - 특정 파일의 블록 디바이스에 I/O 스케줄러를 설정
 *
 * @td:   스레드 데이터
 * @file: 대상 파일
 *
 * 반환값: 0 = 성공, 1 = 에러
 *
 * 역할: sysfs를 통해 블록 디바이스의 I/O 스케줄러를 변경한다.
 *       ioscheduler 옵션으로 지정된 스케줄러를 설정하고,
 *       다시 읽어서 실제로 변경되었는지 확인한다.
 *       Linux 전용 기능이다.
 */
static int set_ioscheduler(struct thread_data *td, struct fio_file *file)
{
	char tmp[256], tmp2[128], *p;
	FILE *f;
	int ret;

	assert(file->du && file->du->sysfs_root);
	/* sysfs의 스케줄러 파일 경로 생성 */
	sprintf(tmp, "%s/queue/scheduler", file->du->sysfs_root);

	f = fopen(tmp, "r+");
	if (!f) {
		if (errno == ENOENT) {
			log_err("fio: os or kernel doesn't support IO scheduler"
				" switching\n");
			return 0;
		}
		td_verror(td, errno, "fopen iosched");
		return 1;
	}

	/*
	 * Set io scheduler.
	 */
	/* [한국어] 스케줄러 이름을 sysfs에 쓰기 */
	ret = fwrite(td->o.ioscheduler, strlen(td->o.ioscheduler), 1, f);
	if (ferror(f) || ret != 1) {
		td_verror(td, errno, "fwrite");
		fclose(f);
		return 1;
	}

	rewind(f);

	/*
	 * Read back and check that the selected scheduler is now the default.
	 */
	/* [한국어] 변경 후 다시 읽어서 선택된 스케줄러가 [brackets]로 표시되는지 확인 */
	ret = fread(tmp, 1, sizeof(tmp) - 1, f);
	if (ferror(f) || ret < 0) {
		td_verror(td, errno, "fread");
		fclose(f);
		return 1;
	}
	tmp[ret] = '\0';
	/*
	 * either a list of io schedulers or "none\n" is expected. Strip the
	 * trailing newline.
	 */
	/* [한국어] 줄바꿈 제거 */
	p = tmp;
	strsep(&p, "\n");

	/*
	 * Write to "none" entry doesn't fail, so check the result here.
	 */
	/* [한국어] "none" 응답이면 스케줄러 변경이 지원되지 않음 */
	if (!strcmp(tmp, "none")) {
		log_err("fio: io scheduler is not tunable\n");
		fclose(f);
		return 0;
	}

	/* 대괄호로 감싼 스케줄러 이름이 결과에 있는지 확인 */
	sprintf(tmp2, "[%s]", td->o.ioscheduler);
	if (!strstr(tmp, tmp2)) {
		log_err("fio: unable to set io scheduler to %s\n", td->o.ioscheduler);
		td_verror(td, EINVAL, "iosched_switch");
		fclose(f);
		return 1;
	}

	fclose(f);
	return 0;
}

/*
 * [한국어] switch_ioscheduler - 모든 대상 파일의 I/O 스케줄러를 변경
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 양수 = 에러
 *
 * 역할: 일반 파일과 블록 디바이스 파일에 대해 set_ioscheduler()를 호출한다.
 *       diskless I/O 엔진이나 캐릭터 디바이스/파이프는 건너뛴다.
 */
static int switch_ioscheduler(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int ret = 0;

	/* diskless 엔진은 스케줄러 변경 불필요 */
	if (td_ioengine_flagged(td, FIO_DISKLESSIO))
		return 0;

	assert(td->files && td->files[0]);

	for_each_file(td, f, i) {

		/* Only consider regular files and block device files */
		/* [한국어] 일반 파일과 블록 디바이스만 처리 */
		switch (f->filetype) {
		case FIO_TYPE_FILE:
		case FIO_TYPE_BLOCK:
			/*
			 * Make sure that the device hosting the file could
			 * be determined.
			 */
			/* [한국어] 디바이스 정보(du)가 없으면 건너뜀 */
			if (!f->du)
				continue;
			break;
		case FIO_TYPE_CHAR:
		case FIO_TYPE_PIPE:
		default:
			continue;
		}

		ret = set_ioscheduler(td, f);
		if (ret)
			return ret;
	}

	return 0;
}

#else

/* [한국어] FIO_HAVE_IOSCHED_SWITCH 미지원 플랫폼: 아무것도 하지 않음 */
static int switch_ioscheduler(struct thread_data *td)
{
	return 0;
}

#endif /* FIO_HAVE_IOSCHED_SWITCH */

/*
 * [한국어] keep_running - 작업을 계속 실행할지 결정
 *
 * @td: 스레드 데이터
 *
 * 반환값: true = 계속 실행, false = 종료
 *
 * 역할: thread_main()의 메인 루프(while (keep_running(td)))에서 호출된다.
 *       다음 조건을 순서대로 확인한다:
 *       1) done 플래그가 설정되었으면 종료
 *       2) terminate 플래그가 설정되었으면 종료
 *       3) time_based 모드이면 계속 (시간이 다 될 때까지)
 *       4) loops 카운터가 남아있으면 감소시키고 계속
 *       5) number_ios 초과했으면 종료
 *       6) 아직 전송할 바이트가 남아있으면 계속
 */
static bool keep_running(struct thread_data *td)
{
	unsigned long long limit;

	if (td->done)
		return false;
	if (td->terminate)
		return false;
	if (td->o.time_based)
		return true;
	if (td->o.loops) {
		td->o.loops--;
		return true;
	}
	if (exceeds_number_ios(td))
		return false;

	/* io_size 또는 size 중 설정된 값을 제한으로 사용 */
	if (td->o.io_size)
		limit = td->o.io_size;
	else
		limit = td->o.size;

	if (limit != -1ULL && ddir_rw_sum(td->io_bytes) < limit) {
		uint64_t diff;

		/*
		 * If the difference is less than the maximum IO size, we
		 * are done.
		 */
		/* [한국어] 남은 바이트가 최대 블록 크기보다 작으면 종료 */
		diff = limit - ddir_rw_sum(td->io_bytes);
		if (diff < td_max_bs(td))
			return false;

		/* 모든 파일이 완료되었고 io_size가 설정되지 않았으면 종료 */
		if (fio_files_done(td) && !td->o.io_size)
			return false;

		return true;
	}

	return false;
}

/*
 * [한국어] exec_string - 외부 명령어 실행
 *
 * @o:      스레드 옵션
 * @string: 실행할 명령어 문자열
 * @mode:   모드 이름 (출력 파일명에 사용, "prerun" 또는 "postrun")
 *
 * 반환값: system()의 반환값
 *
 * 역할: exec_prerun/exec_postrun 옵션으로 지정된 외부 명령을 실행하고,
 *       출력을 <jobname>.<mode>.txt 파일에 저장한다.
 */
static int exec_string(struct thread_options *o, const char *string,
		       const char *mode)
{
	int ret;
	char *str;

	if (asprintf(&str, "%s > %s.%s.txt 2>&1", string, o->name, mode) < 0)
		return -1;

	log_info("%s : Saving output of %s in %s.%s.txt\n", o->name, mode,
		 o->name, mode);
	ret = system(str);
	if (ret == -1)
		log_err("fio: exec of cmd <%s> failed\n", str);

	free(str);
	return ret;
}

/*
 * Dry run to compute correct state of numberio for verification.
 */
/*
 * [한국어] do_dry_run - 검증을 위한 드라이 런 (실제 I/O 없이 상태만 시뮬레이션)
 *
 * @td: 스레드 데이터
 *
 * 반환값: 쓰기 + 트림 바이트 수
 *
 * 역할: verify_only 모드에서 실제 I/O를 수행하지 않고,
 *       get_io_u() -> io_u_sync_complete() 경로를 실행하여
 *       numberio 상태를 올바르게 설정한다.
 *       이후 do_verify()에서 이 상태를 사용하여 검증을 수행한다.
 */
static uint64_t do_dry_run(struct thread_data *td)
{
	td_set_runstate(td, TD_RUNNING);

	/* iolog이 있거나 trim 목록이 있거나 바이트 제한에 도달하지 않을 때까지 */
	while ((td->o.read_iolog_file && !flist_empty(&td->io_log_list)) ||
		(!flist_empty(&td->trim_list)) || !io_complete_bytes_exceeded(td)) {
		struct io_u *io_u;
		int ret;

		if (td->terminate || td->done)
			break;

		/* io_u 할당 및 I/O 파라미터 결정 */
		io_u = get_io_u(td);
		if (IS_ERR_OR_NULL(io_u))
			break;

		/* 실제 I/O 없이 비행 중(in-flight) 상태로 설정 */
		io_u_set(td, io_u, IO_U_F_FLIGHT);
		io_u->error = 0;
		io_u->resid = 0;
		/* 발행 통계 갱신 */
		if (ddir_rw(acct_ddir(io_u))) {
			io_u->numberio = td->io_issues[acct_ddir(io_u)];
			td->io_issues[acct_ddir(io_u)]++;
		}

		if (ddir_rw(io_u->ddir)) {
			io_u_mark_depth(td, 1);
			td->ts.total_io_u[io_u->ddir]++;
		}

		/* 쓰기 + 검증 모드: 드라이 런에서도 I/O 조각 로그 기록 */
		if (td_write(td) && io_u->ddir == DDIR_WRITE &&
		    td->o.do_verify &&
		    td->o.verify != VERIFY_NONE &&
		    !td->o.experimental_verify)
			log_io_piece(td, io_u);

		/* 동기적 완료 처리 (실제 I/O는 수행하지 않음) */
		ret = io_u_sync_complete(td, io_u);
		(void) ret;
	}

	return td->bytes_done[DDIR_WRITE] + td->bytes_done[DDIR_TRIM];
}

/*
 * [한국어] fork_data - fork/pthread_create에 전달되는 데이터 구조체
 *
 * 역할: thread_main()에 스레드 데이터와 소켓 출력 컨텍스트를 전달한다.
 */
struct fork_data {
	struct thread_data *td;   /* 이 스레드의 작업 데이터 */
	struct sk_out *sk_out;    /* 서버 모드 소켓 출력 컨텍스트 */
};

/*
 * Entry point for the thread based jobs. The process based jobs end up
 * here as well, after a little setup.
 */
/*
 * ============================================================================
 * [한국어] thread_main - 워커 스레드/프로세스의 메인 진입점
 * ============================================================================
 *
 * @data: fork_data 구조체 포인터 (td + sk_out)
 *
 * 반환값: (void*) 에러 코드 (0 = 성공)
 *
 * 역할: 각 fio 작업(job)의 전체 생명주기를 관리한다.
 *       pthread_create() 또는 fork()로 생성되어 아래 단계를 실행한다:
 *
 *   스레드 생명주기:
 *   ┌────────────────────────────────────────────────────────────┐
 *   │ 1. 초기화 단계                                              │
 *   │    - PID 설정, 시계 초기화                                   │
 *   │    - 리스트 초기화 (io_log, verify, trim 등)                 │
 *   │    - 뮤텍스/조건변수 생성                                    │
 *   │    - TD_INITIALIZED로 전환, startup_sem으로 동기화             │
 *   │    - UID/GID 설정, CPU 어피니티, NUMA 설정                    │
 *   │    - I/O 엔진 초기화 (td_io_init)                            │
 *   │    - I/O 유닛 풀 초기화 (init_io_u)                          │
 *   │    - 파일 설정 (setup_files)                                 │
 *   │    - iolog 로드, 검증 초기화                                  │
 *   │                                                             │
 *   │ 2. 실행 단계 (while keep_running 루프)                       │
 *   │    - do_io(): 메인 I/O 루프 실행                             │
 *   │    - do_verify(): 데이터 무결성 검증 (verify 옵션 시)         │
 *   │    - 런타임 통계 갱신                                        │
 *   │    - 루프 반복 (loops 옵션)                                  │
 *   │                                                             │
 *   │ 3. 정리 단계                                                 │
 *   │    - TD_FINISHING -> TD_EXITED로 전환                        │
 *   │    - rusage 통계 최종 갱신                                    │
 *   │    - 검증 상태 저장                                           │
 *   │    - 로그 출력, 압축 해제                                     │
 *   │    - exec_postrun 실행                                       │
 *   │    - 파일 닫기, io_u 정리, 엔진 종료                          │
 *   │    - cgroup 해제, cpuset 해제                                 │
 *   └────────────────────────────────────────────────────────────┘
 */
static void *thread_main(void *data)
{
	struct fork_data *fd = data;
	unsigned long long elapsed_us[DDIR_RWDIR_CNT] = { 0, };  /* 방향별 누적 경과 시간 */
	struct thread_data *td = fd->td;         /* 이 스레드의 작업 데이터 */
	struct thread_options *o = &td->o;       /* 작업 옵션 */
	struct sk_out *sk_out = fd->sk_out;      /* 서버 소켓 출력 */
	uint64_t bytes_done[DDIR_RWDIR_CNT];     /* 방향별 완료 바이트 */
	int deadlock_loop_cnt;
	bool clear_state;
	int ret;

	/* 소켓 출력 컨텍스트 설정 (서버 모드용) */
	sk_out_assign(sk_out);
	free(fd);

	/* [한국어] PID 설정: 프로세스 모드면 getpid(), 스레드 모드면 gettid() */
	if (!o->use_thread) {
		setsid();            /* 새 세션 생성 (프로세스 모드) */
		td->pid = getpid();
	} else
		td->pid = gettid();

	/* 로컬 시계 초기화 */
	fio_local_clock_init();

#ifdef CONFIG_LINUX
	/* Linux에서 스레드 이름 설정 (ps 등에서 표시) */
	if (o->comm)
		prctl(PR_SET_NAME, o->comm);
#endif

	dprint(FD_PROCESS, "jobs pid=%d started\n", (int) td->pid);

	/* 서버 모드인 경우 시작 메시지 전송 */
	if (is_backend)
		fio_server_send_start(td);

	/* [한국어] 리스트 초기화: I/O 로그, 히스토리, 검증, 트림 */
	INIT_FLIST_HEAD(&td->io_log_list);
	INIT_FLIST_HEAD(&td->io_hist_list);
	INIT_FLIST_HEAD(&td->verify_list);
	INIT_FLIST_HEAD(&td->trim_list);
	td->io_hist_tree = RB_ROOT;

	/* 뮤텍스와 조건변수 초기화 (프로세스 간 공유 가능) */
	ret = mutex_cond_init_pshared(&td->io_u_lock, &td->free_cond);
	if (ret) {
		td_verror(td, ret, "mutex_cond_init_pshared");
		goto err;
	}
	ret = cond_init_pshared(&td->verify_cond);
	if (ret) {
		td_verror(td, ret, "mutex_cond_pshared");
		goto err;
	}

	/* [한국어] TD_INITIALIZED 상태로 전환하고, 부모에게 초기화 완료를 알림.
	 * 부모(run_threads)가 td->sem을 올릴 때까지 대기. */
	td_set_runstate(td, TD_INITIALIZED);
	dprint(FD_MUTEX, "up startup_sem\n");
	fio_sem_up(startup_sem);       /* 부모에게 "초기화 완료" 신호 */
	dprint(FD_MUTEX, "wait on td->sem\n");
	fio_sem_down(td->sem);         /* 부모가 "실행 시작" 신호를 줄 때까지 대기 */
	dprint(FD_MUTEX, "done waiting on td->sem\n");

	/*
	 * A new gid requires privilege, so we need to do this before setting
	 * the uid.
	 */
	/* [한국어] 그룹 ID와 사용자 ID 변경 (권한 관련) */
	if (o->gid != -1U && setgid(o->gid)) {
		td_verror(td, errno, "setgid");
		goto err;
	}
	if (o->uid != -1U && setuid(o->uid)) {
		td_verror(td, errno, "setuid");
		goto err;
	}

	/* 존(zone) 분배 인덱스 생성 */
	td_zone_gen_index(td);

	/*
	 * Do this early, we don't want the compress threads to be limited
	 * to the same CPUs as the IO workers. So do this before we set
	 * any potential CPU affinity
	 */
	/* [한국어] 로그 압축 스레드 초기화 (CPU 어피니티 설정 전에 해야 함) */
	if (iolog_compress_init(td, sk_out))
		goto err;

	/*
	 * If we have a gettimeofday() thread, make sure we exclude that
	 * thread from this job
	 */
	/* [한국어] gtod CPU가 지정된 경우 해당 CPU를 어피니티에서 제외 */
	if (o->gtod_cpu)
		fio_cpu_clear(&o->cpumask, o->gtod_cpu);

	/*
	 * Set affinity first, in case it has an impact on the memory
	 * allocations.
	 */
	/* [한국어] CPU 어피니티 설정 (메모리 할당에 영향을 줄 수 있으므로 먼저) */
	if (fio_option_is_set(o, cpumask)) {
		if (o->cpus_allowed_policy == FIO_CPUS_SPLIT) {
			/* CPU 분할 정책: 스레드별로 다른 CPU 할당 */
			ret = fio_cpus_split(&o->cpumask, td->thread_number - 1);
			if (!ret) {
				log_err("fio: no CPUs set\n");
				log_err("fio: Try increasing number of available CPUs\n");
				td_verror(td, EINVAL, "cpus_split");
				goto err;
			}
		}
		ret = fio_setaffinity(td->pid, o->cpumask);
		if (ret == -1) {
			td_verror(td, errno, "cpu_set_affinity");
			goto err;
		}
	}

#ifdef CONFIG_LIBNUMA
	/* numa node setup */
	/* [한국어] NUMA 노드 설정: CPU 바인딩 및 메모리 정책 */
	if (fio_option_is_set(o, numa_cpunodes) ||
	    fio_option_is_set(o, numa_memnodes)) {
		struct bitmask *mask;

		if (numa_available() < 0) {
			td_verror(td, errno, "Does not support NUMA API\n");
			goto err;
		}

		/* NUMA CPU 노드 바인딩 */
		if (fio_option_is_set(o, numa_cpunodes)) {
			mask = numa_parse_nodestring(o->numa_cpunodes);
			ret = numa_run_on_node_mask(mask);
			numa_free_nodemask(mask);
			if (ret == -1) {
				td_verror(td, errno, \
					"numa_run_on_node_mask failed\n");
				goto err;
			}
		}

		/* NUMA 메모리 정책 설정 */
		if (fio_option_is_set(o, numa_memnodes)) {
			mask = NULL;
			if (o->numa_memnodes)
				mask = numa_parse_nodestring(o->numa_memnodes);

			switch (o->numa_mem_mode) {
			case MPOL_INTERLEAVE:
				/* 인터리브: 여러 노드에 교대로 할당 */
				numa_set_interleave_mask(mask);
				break;
			case MPOL_BIND:
				/* 바인드: 특정 노드에만 할당 */
				numa_set_membind(mask);
				break;
			case MPOL_LOCAL:
				/* 로컬: 현재 노드에 할당 */
				numa_set_localalloc();
				break;
			case MPOL_PREFERRED:
				/* 선호: 특정 노드를 우선 사용 */
				numa_set_preferred(o->numa_mem_prefer_node);
				break;
			case MPOL_DEFAULT:
			default:
				break;
			}

			if (mask)
				numa_free_nodemask(mask);

		}
	}
#endif

	/* 메모리 고정 (mlockall 등) */
	if (fio_pin_memory(td))
		goto err;

	/*
	 * May alter parameters that init_io_u() will use, so we need to
	 * do this first.
	 */
	/* [한국어] iolog 초기화 (init_io_u()가 사용할 파라미터를 변경할 수 있으므로 먼저) */
	if (!init_iolog(td))
		goto err;

	/* ioprio_set() has to be done before td_io_init() */
	/* [한국어] I/O 우선순위 설정 (I/O 엔진 초기화 전에 해야 함) */
	if (fio_option_is_set(o, ioprio) ||
	    fio_option_is_set(o, ioprio_class) ||
	    fio_option_is_set(o, ioprio_hint)) {
		ret = ioprio_set(IOPRIO_WHO_PROCESS, 0, o->ioprio_class,
				 o->ioprio, o->ioprio_hint);
		if (ret == -1) {
			td_verror(td, errno, "ioprio_set");
			goto err;
		}
		td->ioprio = ioprio_value(o->ioprio_class, o->ioprio,
					  o->ioprio_hint);
		td->ts.ioprio = td->ioprio;
	}

	/* [한국어] I/O 엔진 초기화 (예: libaio, io_uring, sync 등) */
	if (td_io_init(td))
		goto err;

	/* 동기 I/O 엔진에서 iodepth > 1이면 경고 */
	if (td_ioengine_flagged(td, FIO_SYNCIO) && td->o.iodepth > 1 && td->o.io_submit_mode != IO_MODE_OFFLOAD) {
		log_info("note: both iodepth >= 1 and synchronous I/O engine "
			 "are selected, queue depth will be capped at 1\n");
	}

	/* [한국어] I/O 유닛(io_u) 풀 초기화 - iodepth 개수만큼 io_u 할당 */
	if (init_io_u(td))
		goto err;

	/* 비동기 검증 스레드 초기화 */
	if (o->verify_async && verify_async_init(td))
		goto err;

	/* cgroup 설정 */
	if (o->cgroup && cgroup_setup(td, cgroup_list, &cgroup_mnt))
		goto err;

	/* nice 값 설정 (프로세스 우선순위) */
	errno = 0;
	if (nice(o->nice) == -1 && errno != 0) {
		td_verror(td, errno, "nice");
		goto err;
	}

	/* I/O 스케줄러 변경 (Linux 전용) */
	if (o->ioscheduler && switch_ioscheduler(td))
		goto err;

	/* 파일 설정 (create_serialize가 아닌 경우 여기서 수행) */
	if (!o->create_serialize && setup_files(td))
		goto err;

	/* I/O 엔진의 post_init 콜백 호출 */
	if (td->io_ops->post_init && td->io_ops->post_init(td))
		goto err;

	/* 랜덤 맵 초기화 (각 블록의 읽기/쓰기 여부 추적) */
	if (!init_random_map(td))
		goto err;

	/* exec_prerun 옵션: I/O 시작 전 외부 명령 실행 */
	if (o->exec_prerun && exec_string(o, o->exec_prerun, "prerun"))
		goto err;

	/* pre_read 옵션: I/O 시작 전 모든 파일을 미리 읽기 */
	if (o->pre_read && !pre_read_files(td))
		goto err;

	/* 검증 관련 초기화 */
	fio_verify_init(td);

	/* 속도 제한 제출 모드 초기화 */
	if (rate_submit_init(td, sk_out))
		goto err;

	/* [한국어] 에포크 시간 설정 (런타임 측정 기준점) */
	set_epoch_time(td, o->log_alternate_epoch_clock_id, o->job_start_clock_id);
	fio_getrusage(&td->ru_start);  /* 리소스 사용량 초기값 기록 */
	memcpy(&td->bw_sample_time, &td->epoch, sizeof(td->epoch));
	memcpy(&td->iops_sample_time, &td->epoch, sizeof(td->epoch));
	memcpy(&td->ss.prev_time, &td->epoch, sizeof(td->epoch));

	/* 씽크타임 초기화 */
	init_thinktime(td);

	/* 최소 속도 검사를 위한 초기 시간 설정 */
	if (o->ratemin[DDIR_READ] || o->ratemin[DDIR_WRITE] ||
			o->ratemin[DDIR_TRIM]) {
	        memcpy(&td->last_rate_check_time[DDIR_READ], &td->bw_sample_time,
					sizeof(td->bw_sample_time));
	        memcpy(&td->last_rate_check_time[DDIR_WRITE], &td->bw_sample_time,
					sizeof(td->bw_sample_time));
	        memcpy(&td->last_rate_check_time[DDIR_TRIM], &td->bw_sample_time,
					sizeof(td->bw_sample_time));
	}

	memset(bytes_done, 0, sizeof(bytes_done));
	clear_state = false;

	/*
	 * ================================================================
	 * [한국어] *** thread_main 메인 루프 ***
	 *
	 * keep_running()이 true인 동안 반복하며:
	 *   1) do_io() 또는 do_dry_run()으로 I/O 수행
	 *   2) 런타임 통계 갱신
	 *   3) do_verify()로 데이터 검증 (verify 옵션 시)
	 *   4) loops 옵션에 따라 반복
	 * ================================================================
	 */
	while (keep_running(td)) {
		uint64_t verify_bytes;

		/* 루프 시작 시간 기록 */
		fio_gettime(&td->start, NULL);
		memcpy(&td->ts_cache, &td->start, sizeof(td->start));

		/* 두 번째 이상의 루프에서는 I/O 상태 초기화 */
		if (clear_state) {
			clear_io_state(td, 0);

			/* unlink_each_loop 옵션: 각 루프마다 파일 삭제 후 재생성 */
			if (o->unlink_each_loop && unlink_all_files(td))
				break;
		}

		/* 이전 루프의 I/O 로그 정리 */
		prune_io_piece_log(td);

		if (td->o.verify_only && td_write(td))
			/* [한국어] verify_only 모드: 실제 I/O 없이 상태만 시뮬레이션 */
			verify_bytes = do_dry_run(td);
		else {
			/* [한국어] 일반 모드: 실제 I/O 수행 */
			if (!td->o.rand_repeatable)
				/* 검증 시드 상태 백업 (나중에 검증 시 복원) */
				frand_copy(&td->verify_state_last_do_io, &td->verify_state);
			do_io(td, bytes_done);  /* *** 메인 I/O 루프 호출 *** */
			if (!td->o.rand_repeatable)
				/* 검증 시드 상태 복원 */
				frand_copy(&td->verify_state, &td->verify_state_last_do_io);
			if (!ddir_rw_sum(bytes_done)) {
				/* I/O를 전혀 수행하지 못했으면 종료 */
				fio_mark_td_terminate(td);
				verify_bytes = 0;
			} else {
				verify_bytes = bytes_done[DDIR_WRITE] +
						bytes_done[DDIR_TRIM];
			}
		}

		/*
		 * If we took too long to shut down, the main thread could
		 * already consider us reaped/exited. If that happens, break
		 * out and clean up.
		 */
		/* [한국어] 메인 스레드가 이미 이 스레드를 종료된 것으로 간주했으면 탈출 */
		if (td->runstate >= TD_EXITED)
			break;

		clear_state = true;

		/*
		 * Service any pending rusage request, then acquire stat_sem to update
		 * runtime counters. This trylock loop will primarily guard against
		 * contention from concurrent stat calls or other slow operations under
		 * stat_sem.
		 */
		/* [한국어] 통계 세마포어 획득: 데드락 방지를 위해 trylock + 재시도
		 * 5초 이상 대기하면 데드락으로 간주하고 에러 처리 */
		deadlock_loop_cnt = 0;
		do {
			check_update_rusage(td);
			if (!fio_sem_down_trylock(stat_sem))
				break;
			usleep(1000);
			if (deadlock_loop_cnt++ > 5000) {
				log_err("fio seems to be stuck grabbing stat_sem, forcibly exiting\n");
				td->error = EDEADLK;
				goto err;
			}
		} while (1);

		/* [한국어] 방향별 런타임 통계 갱신 */
		if (td->io_bytes[DDIR_READ] && (td_read(td) ||
			((td->flags & TD_F_VER_BACKLOG) && td_write(td))))
			update_runtime(td, elapsed_us, DDIR_READ);
		if (td_write(td) && td->io_bytes[DDIR_WRITE])
			update_runtime(td, elapsed_us, DDIR_WRITE);
		if (td->io_bytes[DDIR_TRIM] && (td_trim(td) ||
			((td->flags & TD_F_TRIM_BACKLOG) && td_write(td))))
			update_runtime(td, elapsed_us, DDIR_TRIM);
		fio_gettime(&td->start, NULL);
		fio_sem_up(stat_sem);  /* 통계 세마포어 해제 */

		/* 에러 또는 종료 신호 시 루프 탈출 */
		if (td->error || td->terminate)
			break;

		/* [한국어] 검증 수행 여부 판단:
		 * do_verify 비활성, 검증 없음, 또는 단방향 엔진이면 건너뜀 */
		if (!o->do_verify ||
		    o->verify == VERIFY_NONE ||
		    td_ioengine_flagged(td, FIO_UNIDIR))
			continue;

		/* 검증을 위해 I/O 상태 초기화 */
		clear_io_state(td, 0);

		fio_gettime(&td->start, NULL);

		/* [한국어] *** 데이터 무결성 검증 실행 *** */
		do_verify(td, verify_bytes);

		/*
		 * See comment further up for why this is done here.
		 */
		/* rusage 및 런타임 통계 갱신 */
		check_update_rusage(td);

		fio_sem_down(stat_sem);
		update_runtime(td, elapsed_us, DDIR_READ);
		fio_gettime(&td->start, NULL);
		fio_sem_up(stat_sem);

		if (td->error || td->terminate)
			break;
	}
	/* ================================================================
	 * [한국어] *** thread_main 메인 루프 종료 ***
	 * ================================================================ */

	/*
	 * Acquire this lock if we were doing overlap checking in
	 * offload mode so that we don't clean up this job while
	 * another thread is checking its io_u's for overlap
	 */
	/* [한국어] 오프로드 + 오버랩 체크 모드에서는 정리 전에 뮤텍스 획득.
	 * 다른 스레드가 이 작업의 io_u를 겹침 검사 중일 수 있으므로. */
	if (td_offload_overlap(td)) {
		int res;

		res = pthread_mutex_lock(&overlap_check);
		if (res) {
			td->error = errno;
			goto err;
		}
	}
	td_set_runstate(td, TD_FINISHING);
	if (td_offload_overlap(td)) {
		int res;

		res = pthread_mutex_unlock(&overlap_check);
		if (res) {
			td->error = errno;
			goto err;
		}
	}

	/* [한국어] 최종 통계 갱신 */
	update_rusage_stat(td);
	td->ts.total_run_time = mtime_since_now(&td->epoch);
	for_each_rw_ddir(ddir) {
		td->ts.io_bytes[ddir] = td->io_bytes[ddir];
	}

	/* 검증 상태 저장 (나중에 재시작 시 사용) */
	if (td->o.verify_state_save && !(td->flags & TD_F_VSTATE_SAVED) &&
	    (td->o.verify != VERIFY_NONE && td_write(td)))
		verify_save_state(td->thread_number);

	/* 고정된 메모리 해제 */
	fio_unpin_memory(td);

	/* 로그 출력 */
	td_writeout_logs(td, true);

	/* 압축 및 속도 제한 정리 */
	iolog_compress_exit(td);
	rate_submit_exit(td);

	/* exec_postrun 옵션: I/O 완료 후 외부 명령 실행 */
	if (o->exec_postrun)
		exec_string(o, o->exec_postrun, "postrun");

	/* 에러 발생 시 관련 스레드 그룹 종료 */
	if (exitall_on_terminate || (o->exitall_error && td->error))
		fio_terminate_threads(td->groupid, td->o.exit_what);

err:
	/* ================================================================
	 * [한국어] *** 정리(cleanup) 단계 ***
	 * 에러 발생 시에도 여기로 점프하여 자원을 정리한다.
	 * ================================================================ */
	if (td->error)
		log_info("fio: pid=%d, err=%d/%s\n", (int) td->pid, td->error,
							td->verror);

	/* 비동기 검증 스레드 종료 */
	if (o->verify_async)
		verify_async_exit(td);

	/* 모든 파일 닫기 및 해제 */
	close_and_free_files(td);
	/* I/O 조각 로그 정리 */
	prune_io_piece_log(td);
	/* I/O 유닛 풀 정리 */
	cleanup_io_u(td);
	/* I/O 엔진 종료 */
	close_ioengine(td);
	/* cgroup 해제 */
	cgroup_shutdown(td, cgroup_mnt);
	/* 검증 상태 메모리 해제 */
	verify_free_state(td);
	/* 존 인덱스 해제 */
	td_zone_free_index(td);

	/* CPU 어피니티 마스크 해제 */
	if (fio_option_is_set(o, cpumask)) {
		ret = fio_cpuset_exit(&o->cpumask);
		if (ret)
			td_verror(td, ret, "fio_cpuset_exit");
	}

	/*
	 * do this very late, it will log file closing as well
	 */
	/* [한국어] iolog 파일 닫기 (마지막에 해야 파일 닫기도 로그에 기록됨) */
	if (o->write_iolog_file)
		write_iolog_close(td);
	if (td->io_log_rfile)
		fclose(td->io_log_rfile);

	/* TD_EXITED 상태로 전환 */
	td_set_runstate(td, TD_EXITED);

	/*
	 * Do this last after setting our runstate to exited, so we
	 * know that the stat thread is signaled.
	 */
	/* [한국어] 마지막 rusage 업데이트 (TD_EXITED 설정 후 해야 stat 스레드가 인지) */
	check_update_rusage(td);

	sk_out_drop();
	return (void *) (uintptr_t) td->error;
}

/*
 * Run over the job map and reap the threads that have exited, if any.
 */
/*
 * [한국어] reap_threads - 종료된 스레드/프로세스를 수거(reap)
 *
 * @nr_running: 실행 중인 스레드 수 포인터 (감소됨)
 * @t_rate:     목표 속도 합계 포인터 (감소됨)
 * @m_rate:     최소 속도 합계 포인터 (감소됨)
 *
 * 역할: run_threads()의 메인 루프에서 주기적으로 호출되어,
 *       TD_EXITED 상태의 스레드를 TD_REAPED로 전환한다.
 *       프로세스 모드에서는 waitpid()로 자식 프로세스 상태를 확인한다.
 *       스레드가 오래 멈춰 있으면 FIO_REAP_TIMEOUT 후 강제 종료한다.
 */
static void reap_threads(unsigned int *nr_running, uint64_t *t_rate,
			 uint64_t *m_rate)
{
	unsigned int cputhreads, realthreads, pending;
	int ret;

	/*
	 * reap exited threads (TD_EXITED -> TD_REAPED)
	 */
	/* [한국어] 모든 스레드를 순회하며 종료된 것들을 수거 */
	realthreads = pending = cputhreads = 0;
	for_each_td(td) {
		int flags = 0, status;

		/* cpuio 엔진은 별도 카운트 */
		if (!strcmp(td->o.ioengine, "cpuio"))
			cputhreads++;
		else
			realthreads++;

		/* PID가 없으면 아직 시작되지 않은 스레드 */
		if (!td->pid) {
			pending++;
			continue;
		}
		/* 이미 수거된 스레드는 건너뜀 */
		if (td->runstate == TD_REAPED)
			continue;
		if (td->o.use_thread) {
			/* pthread 모드: TD_EXITED이면 바로 TD_REAPED로 전환 */
			if (td->runstate == TD_EXITED) {
				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
			continue;
		}

		/* 프로세스 모드: waitpid()로 상태 확인 */
		flags = WNOHANG;
		if (td->runstate == TD_EXITED)
			flags = 0;  /* 이미 종료됐으면 블로킹 대기 */

		/*
		 * check if someone quit or got killed in an unusual way
		 */
		/* [한국어] 자식 프로세스의 종료 상태 확인 */
		ret = waitpid(td->pid, &status, flags);
		if (ret < 0) {
			if (errno == ECHILD) {
				/* 자식 프로세스가 이미 사라짐 */
				log_err("fio: pid=%d disappeared %d\n",
						(int) td->pid, td->runstate);
				td->sig = ECHILD;
				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
			perror("waitpid");
		} else if (ret == td->pid) {
			if (WIFSIGNALED(status)) {
				/* 시그널로 종료된 경우 */
				int sig = WTERMSIG(status);

				if (sig != SIGTERM && sig != SIGUSR2)
					log_err("fio: pid=%d, got signal=%d\n",
							(int) td->pid, sig);
				td->sig = sig;
				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
			if (WIFEXITED(status)) {
				/* 정상 종료: 종료 코드 저장 */
				if (WEXITSTATUS(status) && !td->error)
					td->error = WEXITSTATUS(status);

				td_set_runstate(td, TD_REAPED);
				goto reaped;
			}
		}

		/*
		 * If the job is stuck, do a forceful timeout of it and
		 * move on.
		 */
		/* [한국어] 스레드가 멈춰 있으면 FIO_REAP_TIMEOUT 후 강제 종료 처리 */
		if (td->terminate &&
		    td->runstate < TD_FSYNCING &&
		    time_since_now(&td->terminate_time) >= FIO_REAP_TIMEOUT) {
			log_err("fio: job '%s' (state=%d) hasn't exited in "
				"%lu seconds, it appears to be stuck. Doing "
				"forceful exit of this job.\n",
				td->o.name, td->runstate,
				(unsigned long) time_since_now(&td->terminate_time));
			td_set_runstate(td, TD_REAPED);
			goto reaped;
		}

		/*
		 * thread is not dead, continue
		 */
		/* [한국어] 스레드가 아직 실행 중이면 계속 */
		pending++;
		continue;
reaped:
		/* [한국어] 수거 완료: 실행 카운트 감소, 속도 합계 조정 */
		(*nr_running)--;
		(*m_rate) -= ddir_rw_sum(td->o.ratemin);
		(*t_rate) -= ddir_rw_sum(td->o.rate);
		if (!td->pid)
			pending--;

		if (td->error)
			exit_value++;

		/* 완료 시간 누적 */
		done_secs += mtime_since_now(&td->epoch) / 1000;
		profile_td_exit(td);
		flow_exit_job(td);
	} end_for_each();

	/* [한국어] cpuio 스레드만 남았고 대기 중인 실제 스레드가 없으면 모두 종료 */
	if (*nr_running == cputhreads && !pending && realthreads)
		fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
}

/*
 * [한국어] __check_trigger_file - 트리거 파일 존재 여부 확인
 *
 * 반환값: true = 트리거 파일이 존재함 (삭제 후), false = 없음
 *
 * 역할: trigger_file 경로에 파일이 있으면 삭제하고 true를 반환한다.
 *       외부 프로그램이 파일을 생성하여 fio에 이벤트를 알릴 수 있다.
 */
static bool __check_trigger_file(void)
{
	struct stat sb;

	if (!trigger_file)
		return false;

	if (stat(trigger_file, &sb))
		return false;

	if (unlink(trigger_file) < 0)
		log_err("fio: failed to unlink %s: %s\n", trigger_file,
							strerror(errno));

	return true;
}

/*
 * [한국어] trigger_timedout - 트리거 타임아웃 확인
 *
 * 반환값: true = 타임아웃 발생, false = 아직 아님
 *
 * 역할: 설정된 trigger_timeout 시간이 경과했는지 확인한다.
 */
static bool trigger_timedout(void)
{
	if (trigger_timeout)
		if (time_since_genesis() >= trigger_timeout) {
			trigger_timeout = 0;
			return true;
		}

	return false;
}

/*
 * [한국어] exec_trigger - 트리거 명령 실행
 *
 * @cmd: 실행할 명령어 문자열
 *
 * 역할: system()으로 트리거 명령을 실행한다.
 */
void exec_trigger(const char *cmd)
{
	int ret;

	if (!cmd || cmd[0] == '\0')
		return;

	ret = system(cmd);
	if (ret == -1)
		log_err("fio: failed executing %s trigger\n", cmd);
}

/*
 * [한국어] check_trigger_file - 트리거 파일/타임아웃 확인 및 처리
 *
 * 역할: 트리거 파일이 존재하거나 타임아웃이 발생하면,
 *       클라이언트 모드에서는 원격 명령을 전송하고,
 *       로컬 모드에서는 검증 상태 저장 후 종료 및 트리거 명령을 실행한다.
 */
void check_trigger_file(void)
{
	if (__check_trigger_file() || trigger_timedout()) {
		if (nr_clients)
			fio_clients_send_trigger(trigger_remote_cmd);
		else {
			verify_save_state(IO_LIST_ALL);
			fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
			exec_trigger(trigger_cmd);
		}
	}
}

/*
 * [한국어] fio_verify_load_state - 검증 상태 로드
 *
 * @td: 스레드 데이터
 *
 * 반환값: 0 = 성공, 양수 = 에러
 *
 * 역할: 이전 실행의 검증 상태를 로드하여 이어서 검증할 수 있게 한다.
 *       서버 모드에서는 서버로부터, 로컬 모드에서는 파일에서 로드한다.
 */
static int fio_verify_load_state(struct thread_data *td)
{
	int ret;

	if (!td->o.verify_state)
		return 0;

	if (is_backend) {
		/* 서버 모드: 서버로부터 검증 상태 수신 */
		void *data;

		ret = fio_server_get_verify_state(td->o.name,
					td->thread_number - 1, &data);
		if (!ret)
			verify_assign_state(td, data);
	} else {
		/* 로컬 모드: 파일에서 검증 상태 로드 */
		char prefix[PATH_MAX];

		if (aux_path)
			sprintf(prefix, "%s%clocal", aux_path,
					FIO_OS_PATH_SEPARATOR);
		else
			strcpy(prefix, "local");
		ret = verify_load_state(td, prefix);
	}

	return ret;
}

/*
 * [한국어] do_usleep - 트리거 파일과 통계를 확인하면서 대기
 *
 * @usecs: 대기 시간 (마이크로초)
 *
 * 역할: 단순 usleep 전에 실행 중인 통계 요청과 트리거 파일을 확인한다.
 *       run_threads()에서 폴링 대기 시 사용된다.
 */
static void do_usleep(unsigned int usecs)
{
	check_for_running_stats();
	check_trigger_file();
	usleep(usecs);
}

/*
 * [한국어] check_mount_writes - 마운트된 디바이스에 쓰기 방지
 *
 * @td: 스레드 데이터
 *
 * 반환값: true = 마운트된 디바이스 감지 (중단 필요), false = 안전
 *
 * 역할: 블록 디바이스에 쓰기 작업 시, 해당 디바이스가 마운트되어 있으면
 *       파일 시스템 손상 방지를 위해 실행을 중단한다.
 *       allow_mounted_write 옵션으로 이 검사를 무시할 수 있다.
 */
static bool check_mount_writes(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	/* 쓰기가 아니거나 allow_mounted_write가 설정되었으면 검사 안 함 */
	if (!td_write(td) || td->o.allow_mounted_write)
		return false;

	/*
	 * If FIO_HAVE_CHARDEV_SIZE is defined, it's likely that chrdevs
	 * are mkfs'd and mounted.
	 */
	/* [한국어] 블록 디바이스(및 캐릭터 디바이스) 파일만 마운트 검사 */
	for_each_file(td, f, i) {
#ifdef FIO_HAVE_CHARDEV_SIZE
		if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR)
#else
		if (f->filetype != FIO_TYPE_BLOCK)
#endif
			continue;
		if (device_is_mounted(f->file_name))
			goto mounted;
	}

	return false;
mounted:
	log_err("fio: %s appears mounted, and 'allow_mounted_write' isn't set. Aborting.\n", f->file_name);
	return true;
}

/*
 * [한국어] waitee_running - wait_for 옵션의 대상 작업이 아직 실행 중인지 확인
 *
 * @me: 현재 스레드 데이터
 *
 * 반환값: true = 대기 대상이 아직 실행 중, false = 완료됨 또는 대기 없음
 *
 * 역할: wait_for 옵션이 설정된 경우, 지정된 작업 이름을 가진
 *       다른 스레드가 TD_EXITED 이전 상태이면 true를 반환하여
 *       현재 스레드의 시작을 지연시킨다.
 */
static bool waitee_running(struct thread_data *me)
{
	const char *waitee = me->o.wait_for;
	const char *self = me->o.name;

	if (!waitee)
		return false;

	/* 모든 스레드를 순회하며 대기 대상 이름을 가진 스레드 검색 */
	for_each_td(td) {
		if (!strcmp(td->o.name, self) || strcmp(td->o.name, waitee))
			continue;

		/* 대기 대상이 아직 종료되지 않았으면 대기 필요 */
		if (td->runstate < TD_EXITED) {
			dprint(FD_PROCESS, "%s fenced by %s(%s)\n",
					self, td->o.name,
					runstate_to_name(td->runstate));
			return true;
		}
	} end_for_each();

	dprint(FD_PROCESS, "%s: %s completed, can run\n", self, waitee);
	return false;
}

/*
 * Main function for kicking off and reaping jobs, as needed.
 */
/*
 * ============================================================================
 * [한국어] run_threads - 모든 작업(job)의 생성, 시작, 수거를 관리하는 메인 컨트롤러
 * ============================================================================
 *
 * @sk_out: 서버 모드 소켓 출력 컨텍스트
 *
 * 역할: fio의 전체 작업 실행 흐름을 제어하는 함수이다.
 *       모든 작업을 순서대로 생성하고, 초기화가 완료될 때까지 대기한 후,
 *       실행을 시작시키고, 종료될 때까지 감시하며 수거한다.
 *
 *   실행 흐름:
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ 1. 사전 준비                                              │
 *   │    - gtod 오프로드 스레드 시작                              │
 *   │    - 유휴 프로파일링 초기화                                 │
 *   │    - 시그널 핸들러 설정                                     │
 *   │    - 스레드/프로세스 수 집계                                │
 *   │    - 마운트 쓰기 검사                                      │
 *   │                                                           │
 *   │ 2. 파일 설정 (create_serialize 모드)                       │
 *   │    - 순차적으로 파일 생성 (데이터 인터리빙 방지)             │
 *   │    - 검증 상태 로드                                        │
 *   │                                                           │
 *   │ 3. 메인 루프 (while todo)                                  │
 *   │    a) 스레드 생성: TD_NOT_CREATED -> TD_CREATED             │
 *   │       - start_delay, stonewall, wait_for 옵션 처리          │
 *   │       - pthread_create() 또는 fork()로 생성                 │
 *   │       - startup_sem으로 초기화 완료 대기                     │
 *   │    b) 초기화 대기: TD_CREATED -> TD_INITIALIZED             │
 *   │       - JOB_START_TIMEOUT (5초) 이내 확인                   │
 *   │    c) 실행 시작: TD_INITIALIZED -> TD_RUNNING               │
 *   │       - td->sem으로 시작 신호 전송                           │
 *   │    d) 종료 수거: reap_threads()                             │
 *   │       - TD_EXITED -> TD_REAPED                              │
 *   │                                                             │
 *   │ 4. 최종 수거 (while nr_running)                             │
 *   │    - 모든 스레드가 종료될 때까지 반복 수거                    │
 *   │                                                             │
 *   │ 5. 정리                                                     │
 *   │    - 유휴 프로파일링 중지                                    │
 *   │    - I/O 틱 업데이트                                        │
 *   └──────────────────────────────────────────────────────────┘
 */
static void run_threads(struct sk_out *sk_out)
{
	struct thread_data *td;
	unsigned int i, todo, nr_running, nr_started;
	uint64_t m_rate, t_rate;  /* 최소 속도 합계, 목표 속도 합계 */
	uint64_t spent;

	/* gtod 오프로드 스레드 시작 (별도 스레드에서 gettimeofday 수행) */
	if (fio_gtod_offload && fio_start_gtod_thread())
		return;

	/* 유휴 프로파일링 초기화 */
	fio_idle_prof_init();

	/* 시그널 핸들러 등록 */
	set_sig_handlers();

	/* [한국어] 스레드/프로세스 수 집계 및 마운트 쓰기 검사 */
	nr_thread = nr_process = 0;
	for_each_td(td) {
		if (check_mount_writes(td))
			return;  /* 마운트된 디바이스에 쓰기 시도 시 중단 */
		if (td->o.use_thread)
			nr_thread++;
		else
			nr_process++;
	} end_for_each();

	/* 시작 메시지 출력 */
	if (output_format & FIO_OUTPUT_NORMAL) {
		struct buf_output out;

		buf_output_init(&out);
		__log_buf(&out, "Starting ");
		if (nr_thread)
			__log_buf(&out, "%d thread%s", nr_thread,
						nr_thread > 1 ? "s" : "");
		if (nr_process) {
			if (nr_thread)
				__log_buf(&out, " and ");
			__log_buf(&out, "%d process%s", nr_process,
						nr_process > 1 ? "es" : "");
		}
		__log_buf(&out, "\n");
		log_info_buf(out.buf, out.buflen);
		buf_output_free(&out);
	}

	/* 카운터 초기화 */
	todo = thread_number;       /* 아직 시작하지 않은 작업 수 */
	nr_running = 0;             /* 현재 실행 중인 작업 수 */
	nr_started = 0;             /* 생성되었지만 아직 실행 시작 안 된 수 */
	m_rate = t_rate = 0;        /* 속도 합계 */

	/* [한국어] 파일 사전 설정: create_serialize 모드에서 순차적으로 파일 생성
	 * 여러 스레드가 동시에 파일을 생성하면 데이터가 인터리빙될 수 있으므로 */
	for_each_td(td) {
		print_status_init(td->thread_number - 1);

		if (!td->o.create_serialize)
			continue;

		/* 검증 상태 로드 */
		if (fio_verify_load_state(td))
			goto reap;

		/*
		 * do file setup here so it happens sequentially,
		 * we don't want X number of threads getting their
		 * client data interspersed on disk
		 */
		/* [한국어] 파일 생성/레이아웃 설정 */
		if (setup_files(td)) {
reap:
			exit_value++;
			if (td->error)
				log_err("fio: pid=%d, err=%d/%s\n",
					(int) td->pid, td->error, td->verror);
			td_set_runstate(td, TD_REAPED);
			todo--;
		} else {
			struct fio_file *f;
			unsigned int j;

			/*
			 * for sharing to work, each job must always open
			 * its own files. so close them, if we opened them
			 * for creation
			 */
			/* [한국어] 파일 공유를 위해 여기서 생성용으로 연 파일을 닫음.
			 * 각 작업이 나중에 자체적으로 파일을 열어야 함. */
			for_each_file(td, f, j) {
				if (fio_file_open(f))
					td_io_close_file(td, f);
			}
		}
	} end_for_each();

	/* make sure child processes have empty stream buffers before fork */
	/* [한국어] fork 전에 출력 버퍼를 비움 */
	log_info_flush();

	/* start idle threads before io threads start to run */
	/* [한국어] I/O 스레드 시작 전에 유휴 프로파일링 시작 */
	fio_idle_prof_start();

	/* 제네시스(전체 시작) 시간 설정 */
	set_genesis_time();

	/*
	 * ================================================================
	 * [한국어] *** run_threads 메인 루프 ***
	 *
	 * todo가 0이 될 때까지 (모든 작업이 시작되어 실행되거나 종료될 때까지):
	 *   1) 대기 중인 작업을 스레드/프로세스로 생성
	 *   2) 초기화 완료를 대기
	 *   3) 실행 시작 신호 전송
	 *   4) 종료된 스레드 수거
	 * ================================================================
	 */
	while (todo) {
		struct thread_data *map[REAL_MAX_JOBS]; /* 이번 배치에서 생성된 스레드 추적 */
		struct timespec this_start;
		int this_jobs = 0, left;   /* 이번 배치의 작업 수, 남은 수 */
		struct fork_data *fd;

		/*
		 * create threads (TD_NOT_CREATED -> TD_CREATED)
		 */
		/* [한국어] 아직 생성되지 않은 작업들을 생성 */
		for_each_td(td) {
			if (td->runstate != TD_NOT_CREATED)
				continue;

			/*
			 * never got a chance to start, killed by other
			 * thread for some reason
			 */
			/* [한국어] 시작 전에 다른 스레드에 의해 종료 신호를 받은 경우 */
			if (td->terminate) {
				todo--;
				continue;
			}

			/* start_delay 옵션: 지정된 지연 시간이 지나지 않았으면 건너뜀 */
			if (td->o.start_delay) {
				spent = utime_since_genesis();

				if (td->o.start_delay > spent)
					continue;
			}

			/* [한국어] stonewall 옵션: 이전 작업이 모두 완료될 때까지 대기 */
			if (td->o.stonewall && (nr_started || nr_running)) {
				dprint(FD_PROCESS, "%s: stonewall wait\n",
							td->o.name);
				break;
			}

			/* wait_for 옵션: 지정된 작업이 완료될 때까지 대기 */
			if (waitee_running(td)) {
				dprint(FD_PROCESS, "%s: waiting for %s\n",
						td->o.name, td->o.wait_for);
				continue;
			}

			/* 디스크 유틸리티 초기화 */
			init_disk_util(td);

			/* rusage 세마포어 초기화 */
			td->rusage_sem = fio_sem_init(FIO_SEM_LOCKED);
			td->update_rusage = 0;

			/*
			 * Set state to created. Thread will transition
			 * to TD_INITIALIZED when it's done setting up.
			 */
			/* [한국어] TD_CREATED 상태로 전환하고 이번 배치에 추가 */
			td_set_runstate(td, TD_CREATED);
			map[this_jobs++] = td;
			nr_started++;

			/* fork_data 구조체 할당 (thread_main에 전달) */
			fd = calloc(1, sizeof(*fd));
			fd->td = td;
			fd->sk_out = sk_out;

			if (td->o.use_thread) {
				/* [한국어] pthread 모드: 스레드 생성 */
				int ret;

				dprint(FD_PROCESS, "will pthread_create\n");
				ret = pthread_create(&td->thread, NULL,
							thread_main, fd);
				if (ret) {
					log_err("pthread_create: %s\n",
							strerror(ret));
					free(fd);
					nr_started--;
					break;
				}
				fd = NULL;
				/* 스레드를 detach하여 자원 자동 해제 */
				ret = pthread_detach(td->thread);
				if (ret)
					log_err("pthread_detach: %s",
							strerror(ret));
			} else {
				/* [한국어] fork 모드: 자식 프로세스 생성 */
				pid_t pid;
				dprint(FD_PROCESS, "will fork\n");
				read_barrier();
				pid = fork();
				if (!pid) {
					/* 자식 프로세스: thread_main 실행 후 _exit */
					int ret;

					ret = (int)(uintptr_t)thread_main(fd);
					/* _exit() does not flush buffers, so
					 * do it ourselves */
					log_info_flush();
					_exit(ret);
				} else if (__td_index == fio_debug_jobno)
					*fio_debug_jobp = pid;
				free(fd);
				fd = NULL;
			}
			/* [한국어] startup_sem 대기: thread_main()이 초기화를 완료할 때까지.
			 * 10초 타임아웃이 있으며, 초과 시 강제 종료. */
			dprint(FD_MUTEX, "wait on startup_sem\n");
			if (fio_sem_down_timeout(startup_sem, 10000)) {
				log_err("fio: job startup hung? exiting.\n");
				fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
				fio_abort = true;
				nr_started--;
				free(fd);
				break;
			}
			dprint(FD_MUTEX, "done waiting on startup_sem\n");
		} end_for_each();

		/*
		 * Wait for the started threads to transition to
		 * TD_INITIALIZED.
		 */
		/* [한국어] 이번 배치의 모든 스레드가 TD_INITIALIZED가 될 때까지 대기
		 * JOB_START_TIMEOUT (5초) 이내에 완료되어야 함 */
		fio_gettime(&this_start, NULL);
		left = this_jobs;
		while (left && !fio_abort) {
			if (mtime_since_now(&this_start) > JOB_START_TIMEOUT)
				break;

			do_usleep(100000);  /* 100ms 간격으로 확인 */

			for (i = 0; i < this_jobs; i++) {
				td = map[i];
				if (!td)
					continue;
				if (td->runstate == TD_INITIALIZED) {
					map[i] = NULL;
					left--;
				} else if (td->runstate >= TD_EXITED) {
					/* 초기화 중에 이미 종료된 경우 */
					map[i] = NULL;
					left--;
					todo--;
					nr_running++; /* work-around... */
				}
			}
		}

		/* 타임아웃으로 시작 실패한 작업 처리 */
		if (left) {
			log_err("fio: %d job%s failed to start\n", left,
					left > 1 ? "s" : "");
			for (i = 0; i < this_jobs; i++) {
				td = map[i];
				if (!td)
					continue;
				kill(td->pid, SIGTERM);  /* 실패한 작업에 종료 신호 */
			}
			break;
		}

		/*
		 * start created threads (TD_INITIALIZED -> TD_RUNNING).
		 */
		/* [한국어] 초기화 완료된 스레드를 실행 상태로 전환
		 * td->sem을 올려서 thread_main()의 fio_sem_down(td->sem) 대기를 해제 */
		for_each_td(td) {
			if (td->runstate != TD_INITIALIZED)
				continue;

			/* 워밍업 구간이면 TD_RAMP, 아니면 TD_RUNNING */
			if (in_ramp_period(td))
				td_set_runstate(td, TD_RAMP);
			else
				td_set_runstate(td, TD_RUNNING);
			nr_running++;
			nr_started--;
			m_rate += ddir_rw_sum(td->o.ratemin);
			t_rate += ddir_rw_sum(td->o.rate);
			todo--;
			fio_sem_up(td->sem);  /* "실행 시작" 신호 전송 */
		} end_for_each();

		/* 종료된 스레드 수거 */
		reap_threads(&nr_running, &t_rate, &m_rate);

		/* 아직 시작할 작업이 남아있으면 100ms 대기 후 다음 반복 */
		if (todo)
			do_usleep(100000);
	}

	/* [한국어] 모든 작업이 종료될 때까지 수거 반복 */
	while (nr_running) {
		reap_threads(&nr_running, &t_rate, &m_rate);
		do_usleep(10000);  /* 10ms 간격 */
	}

	/* 유휴 프로파일링 중지 */
	fio_idle_prof_stop();

	/* I/O 틱 업데이트 (디스크 유틸리티 통계) */
	update_io_ticks();
}

/*
 * [한국어] free_disk_util - 디스크 유틸리티 자원 해제
 *
 * 역할: 디스크 유틸리티 항목을 정리하고 헬퍼 스레드를 종료한다.
 */
static void free_disk_util(void)
{
	disk_util_prune_entries();
	helper_thread_destroy();
}

/*
 * ============================================================================
 * [한국어] fio_backend - fio 백엔드의 최상위 진입점
 * ============================================================================
 *
 * @sk_out: 서버 모드 소켓 출력 컨텍스트 (로컬 모드에서는 NULL)
 *
 * 반환값: 종료 코드 (0 = 성공)
 *
 * 역할: fio의 전체 실행 흐름을 제어하는 최상위 함수이다.
 *
 *   실행 흐름:
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ 1. 프로파일 로드 (exec_profile)                           │
 *   │ 2. 대역폭 로그 설정 (write_bw_log)                        │
 *   │ 3. 중복 제거 시드 초기화                                   │
 *   │ 4. 시작 세마포어 생성                                      │
 *   │ 5. 통계 초기화 및 헬퍼 스레드 생성                          │
 *   │ 6. cgroup 목록 초기화                                      │
 *   │ 7. run_threads() 호출 ← 모든 작업 실행                     │
 *   │ 8. 헬퍼 스레드 종료                                        │
 *   │ 9. 최종 통계 출력                                          │
 *   │ 10. 자원 정리 (옵션, 세마포어, 통계 등)                     │
 *   └──────────────────────────────────────────────────────────┘
 */
int fio_backend(struct sk_out *sk_out)
{
	int i;
	/* 프로파일이 지정된 경우 로드 */
	if (exec_profile) {
		if (load_profile(exec_profile))
			return 1;
		free(exec_profile);
		exec_profile = NULL;
	}
	/* 작업이 없으면 바로 종료 */
	if (!thread_number)
		return 0;

	/* [한국어] 대역폭 로그 설정: 읽기/쓰기/트림별 로그 파일 생성 */
	if (write_bw_log) {
		char read[PATH_MAX], write[PATH_MAX], trim[PATH_MAX];
		struct log_params p = {
			.log_type = IO_LOG_TYPE_BW,
		};

		snprintf(read, sizeof(read), "%s-read_bw.log", write_bw_log_name);
		snprintf(write, sizeof(write), "%s-write_bw.log", write_bw_log_name);
		snprintf(trim, sizeof(trim), "%s-trim_bw.log", write_bw_log_name);

		setup_log(&agg_io_log[DDIR_READ], &p, read);
		setup_log(&agg_io_log[DDIR_WRITE], &p, write);
		setup_log(&agg_io_log[DDIR_TRIM], &p, trim);
	}

	/* 글로벌 중복 제거 작업 세트 시드 초기화 */
	if (init_global_dedupe_working_set_seeds()) {
		log_err("fio: failed to initialize global dedupe working set\n");
		return 1;
	}

	/* [한국어] 시작 세마포어 생성: 스레드 초기화 동기화에 사용 */
	startup_sem = fio_sem_init(FIO_SEM_LOCKED);
	if (!sk_out)
		is_local_backend = true;
	if (startup_sem == NULL)
		return 1;

	/* 제네시스 시간 설정 */
	set_genesis_time();
	/* 통계 모듈 초기화 */
	stat_init();
	/* 헬퍼 스레드 생성 (주기적 통계 수집, 디스크 유틸리티 등) */
	if (helper_thread_create(startup_sem, sk_out))
		log_err("fio: failed to create helper thread\n");

	/* cgroup 목록 초기화 (공유 메모리) */
	cgroup_list = smalloc(sizeof(*cgroup_list));
	if (cgroup_list)
		INIT_FLIST_HEAD(cgroup_list);

	/* *** 모든 작업 실행 *** */
	run_threads(sk_out);

	/* 헬퍼 스레드 종료 */
	helper_thread_exit();

	/* [한국어] 정상 종료 시 최종 통계 출력 */
	if (!fio_abort) {
		__show_run_stats();
		if (write_bw_log) {
			/* 대역폭 로그 파일 플러시 및 해제 */
			for (i = 0; i < DDIR_RWDIR_CNT; i++) {
				struct io_log *log = agg_io_log[i];

				flush_log(log, false);
				free_log(log);
			}
		}
	}

	/* [한국어] 모든 스레드의 자원 정리 */
	for_each_td(td) {
		struct thread_stat *ts = &td->ts;

		free_clat_prio_stats(ts);   /* 완료 지연 시간 통계 해제 */
		steadystate_free(td);       /* 정상 상태(steady state) 데이터 해제 */
		fio_options_free(td);       /* 옵션 메모리 해제 */
		fio_dump_options_free(td);  /* 덤프 옵션 해제 */
		if (td->rusage_sem) {
			fio_sem_remove(td->rusage_sem);
			td->rusage_sem = NULL;
		}
		fio_sem_remove(td->sem);
		td->sem = NULL;
	} end_for_each();

	/* 유휴 프로파일링 정리 */
	fio_idle_prof_cleanup();
	/* 디스크 유틸리티 해제 */
	free_disk_util();
	/* cgroup 정리 */
	if (cgroup_list) {
		cgroup_kill(cgroup_list);
		sfree(cgroup_list);
	}

	/* 시작 세마포어 해제 */
	fio_sem_remove(startup_sem);
	/* 통계 모듈 정리 */
	stat_exit();
	return exit_value;
}
