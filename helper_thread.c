/*
 * [한국어 설명] fio 헬퍼 스레드 구현 (helper_thread.c)
 *
 * === 파일의 역할 ===
 * 메인 I/O 스레드와 별도로 동작하며, 주기적으로 디스크 유틸리티 업데이트,
 * 실시간 상태 출력, Steady State 체크, Ramp period 체크, 로그 샘플링을 수행한다.
 * 파이프를 통해 메인 스레드로부터 액션(RESET, DO_STAT, EXIT)을 수신한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * backend.c의 run_threads()에서 helper_thread_create()로 생성.
 * 주기적 타이머와 select()로 대기하며, 메인 스레드와 파이프로 통신.
 * 호출 체인: run_threads() [backend.c] → helper_thread_create() [이 파일]
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: run_threads()에서 헬퍼 스레드 생성/종료
 * - diskutil.c: update_io_ticks()로 디스크 I/O 통계 수집
 * - steadystate.c: steadystate_check()로 정상 상태 도달 여부 확인
 * - helper_thread.h: API 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - helper_thread_create(): 헬퍼 스레드 생성
 * - helper_thread_exit(): 헬퍼 스레드 종료 및 join
 * - helper_do_stat(): 즉시 통계 출력 요청 (파이프 통신)
 * - helper_reset(): 타이머 리셋 요청
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef CONFIG_HAVE_TIMERFD_CREATE
#include <sys/timerfd.h>      /* 고해상도 타이머 (timerfd) */
#endif
#ifdef CONFIG_VALGRIND_DEV
#include <valgrind/drd.h>     /* Valgrind DRD 데이터 레이스 감지 지원 */
#else
#define DRD_IGNORE_VAR(x) do { } while (0)
#endif

#ifdef WIN32
#include "os/os-windows.h"    /* Windows 호환 레이어 */
#endif

#include "fio.h"              /* fio 핵심 구조체 및 매크로 */
#include "smalloc.h"          /* 공유 메모리 할당기 */
#include "helper_thread.h"    /* 헬퍼 스레드 함수 선언 */
#include "steadystate.h"      /* Steady State 감지 */
#include "pshared.h"          /* 프로세스 간 공유 뮤텍스 */

/* [한국어] 전역 변수 */
static int sleep_accuracy_ms;  /* select() 타임아웃 정밀도 (ms 단위, 시스템 클럭 틱 기반) */
static int timerfd = -1;       /* 고해상도 타이머 fd (-1이면 미사용) */

/* [한국어] 파이프를 통해 전달되는 액션 종류 */
enum action {
	A_EXIT		= 1,   /* 헬퍼 스레드 종료 */
	A_RESET		= 2,   /* 타이머 리셋 */
	A_DO_STAT	= 3,   /* 즉시 통계 출력 */
};

/* [한국어] 헬퍼 스레드 내부 데이터 (공유 메모리에 할당) */
static struct helper_data {
	volatile int exit;          /* 종료 플래그 */
	int pipe[2];                /* 0: 읽기 끝; 1: 쓰기 끝 */
	struct sk_out *sk_out;      /* 서버 모드 소켓 출력 */
	pthread_t thread;           /* 헬퍼 스레드 핸들 */
	struct fio_sem *startup_sem; /* 초기화 완료 시그널용 세마포어 */
} *helper_data;

/* [한국어] 주기적 타이머 구조체 - 각 주기 작업의 이름, 만료 시각, 간격, 콜백 */
struct interval_timer {
	const char	*name;         /* 타이머 이름 (디버그용) */
	struct timespec	expires;       /* 다음 만료 시각 */
	uint32_t	interval_ms;   /* 실행 간격 (ms, 0이면 비활성) */
	int		(*func)(void); /* 만료 시 호출할 콜백 함수 */
};

/* [한국어] 헬퍼 스레드 데이터 파괴 - 파이프 닫고 공유 메모리 해제 */
void helper_thread_destroy(void)
{
	if (!helper_data)
		return;

	close(helper_data->pipe[0]);
	close(helper_data->pipe[1]);
	sfree(helper_data);
}

/* === Windows용 소켓/파이프 호환 함수 === */
#ifdef _WIN32
/* [한국어] Windows: Winsock 초기화 */
static void sock_init(void)
{
	WSADATA wsaData;
	int res;

	/* It is allowed to call WSAStartup() more than once. */
	res = WSAStartup(MAKEWORD(2, 2), &wsaData);
	assert(res == 0);
}

/* [한국어] Windows: 소켓을 논블로킹 모드로 설정 */
static int make_nonblocking(int fd)
{
	unsigned long arg = 1;

	return ioctlsocket(fd, FIONBIO, &arg);
}

/* [한국어] Windows: 소켓으로 데이터 전송 (파이프 에뮬레이션) */
static int write_to_pipe(int fd, const void *buf, size_t len)
{
	return send(fd, buf, len, 0);
}

/* [한국어] Windows: 소켓에서 데이터 수신 (파이프 에뮬레이션) */
static int read_from_pipe(int fd, void *buf, size_t len)
{
	return recv(fd, buf, len, 0);
}
#else
/* === POSIX용 파이프 함수 === */
static void sock_init(void)
{
}

/* [한국어] POSIX: 파일 디스크립터를 논블로킹 모드로 설정 */
static int make_nonblocking(int fd)
{
	return fcntl(fd, F_SETFL, O_NONBLOCK);
}

/* [한국어] POSIX: 파이프에 데이터 쓰기 */
static int write_to_pipe(int fd, const void *buf, size_t len)
{
	return write(fd, buf, len);
}

/* [한국어] POSIX: 파이프에서 데이터 읽기 */
static int read_from_pipe(int fd, void *buf, size_t len)
{
	return read(fd, buf, len);
}
#endif

/* [한국어] 시그널 블로킹 - 헬퍼 스레드에서 시그널 처리 방지 (메인 스레드가 처리) */
static void block_signals(void)
{
#ifdef CONFIG_PTHREAD_SIGMASK
	sigset_t sigmask;

	int ret;

	ret = pthread_sigmask(SIG_UNBLOCK, NULL, &sigmask);
	assert(ret == 0);
	ret = pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
#endif
}

/* [한국어] 파이프를 통해 헬퍼 스레드에 액션 전송 */
static void submit_action(enum action a)
{
	const char data = a;
	int ret;

	if (!helper_data)
		return;

	ret = write_to_pipe(helper_data->pipe[1], &data, sizeof(data));
	if (ret != 1) {
		log_err("failed to write action into pipe, err %i:%s", errno, strerror(errno));
		assert(0);
	}
}

/* [한국어] 타이머 리셋 요청 - 모든 interval_timer를 현재 시각 기준으로 재설정 */
void helper_reset(void)
{
	submit_action(A_RESET);
}

/*
 * May be invoked in signal handler context and hence must only call functions
 * that are async-signal-safe. See also
 * https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04_03.
 */
/* [한국어] 즉시 통계 출력 요청 - 시그널 핸들러에서도 호출 가능 (async-signal-safe) */
void helper_do_stat(void)
{
	submit_action(A_DO_STAT);
}

/* [한국어] 헬퍼 스레드 종료 여부 확인 */
bool helper_should_exit(void)
{
	if (!helper_data)
		return true;

	return helper_data->exit;
}

/* [한국어] 헬퍼 스레드 종료 - 종료 플래그 설정 후 join 대기 */
void helper_thread_exit(void)
{
	if (!helper_data)
		return;

	helper_data->exit = 1;
	pthread_join(helper_data->thread, NULL);
}

/*
 * [한국어] 모든 타이머를 현재 시각 기준으로 리셋
 * 각 타이머의 다음 만료 시각을 (현재 + interval_ms)로 설정하고,
 * 가장 빠른 다음 이벤트까지의 시간(ms)을 반환
 */
/* Resets timers and returns the time in milliseconds until the next event. */
static int reset_timers(struct interval_timer timer[], int num_timers,
			struct timespec *now)
{
	uint32_t msec_to_next_event = INT_MAX;
	int i;

	for (i = 0; i < num_timers; ++i) {
		timer[i].expires = *now;
		timespec_add_msec(&timer[i].expires, timer[i].interval_ms);
		msec_to_next_event = min_not_zero(msec_to_next_event,
						  timer[i].interval_ms);
	}

	return msec_to_next_event;
}

/*
 * Waits for an action from fd during at least timeout_ms. `fd` must be in
 * non-blocking mode.
 */
/*
 * [한국어] 파이프 fd에서 액션을 대기
 * timeout_ms 동안 select()로 블로킹하며, 타이머 또는 파이프 이벤트 발생 시 반환.
 * timerfd가 있으면 고해상도 타이머를 사용하여 정밀도 향상.
 */
static uint8_t wait_for_action(int fd, unsigned int timeout_ms)
{
	struct timeval timeout = {
		.tv_sec  = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};
	fd_set rfds, efds;
	uint8_t action = 0;
	uint64_t exp;
	int res;

	/* [한국어] 먼저 논블로킹으로 즉시 읽기 시도 */
	res = read_from_pipe(fd, &action, sizeof(action));
	if (res > 0 || timeout_ms == 0)
		return action;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	FD_ZERO(&efds);
	FD_SET(fd, &efds);
#ifdef CONFIG_HAVE_TIMERFD_CREATE
	{
		/*
		 * If the timer frequency is 100 Hz, select() will round up
		 * `timeout` to the next multiple of 1 / 100 Hz = 10 ms. Hence
		 * use a high-resolution timer if possible to increase
		 * select() timeout accuracy.
		 */
		/* [한국어] timerfd를 사용하여 select() 타임아웃 정밀도 개선 */
		struct itimerspec delta = {};

		delta.it_value.tv_sec = timeout.tv_sec;
		delta.it_value.tv_nsec = timeout.tv_usec * 1000;
		res = timerfd_settime(timerfd, 0, &delta, NULL);
		assert(res == 0);
		FD_SET(timerfd, &rfds);
	}
#endif
	res = select(max(fd, timerfd) + 1, &rfds, NULL, &efds,
		     timerfd >= 0 ? NULL : &timeout);
	if (res < 0) {
		log_err("fio: select() call in helper thread failed: %s",
			strerror(errno));
		return A_EXIT;
	}
	if (FD_ISSET(fd, &rfds))
		read_from_pipe(fd, &action, sizeof(action));
	/* [한국어] timerfd 만료 이벤트 소비 */
	if (timerfd >= 0 && FD_ISSET(timerfd, &rfds)) {
		res = read(timerfd, &exp, sizeof(exp));
		assert(res == sizeof(exp));
	}
	return action;
}

/*
 * Verify whether or not timer @it has expired. If timer @it has expired, call
 * @it->func(). @now is the current time. @msec_to_next_event is an
 * input/output parameter that represents the time until the next event.
 */
/*
 * [한국어] 타이머 만료 여부 평가
 * 만료되었으면 콜백 함수를 호출하고, 다음 만료 시각을 재설정.
 * 클럭 점프(시간 급변)도 감지하여 보정.
 * msec_to_next_event를 갱신하여 가장 빠른 다음 이벤트 시간 유지.
 */
static int eval_timer(struct interval_timer *it, const struct timespec *now,
		      unsigned int *msec_to_next_event)
{
	int64_t delta_ms;
	bool expired;

	/* interval == 0 means that the timer is disabled. */
	/* [한국어] interval이 0이면 비활성 타이머 */
	if (it->interval_ms == 0)
		return 0;

	delta_ms = rel_time_since(now, &it->expires);
	expired = delta_ms <= sleep_accuracy_ms;  /* 슬립 정밀도 이내면 만료로 판정 */
	if (expired) {
		timespec_add_msec(&it->expires, it->interval_ms);
		delta_ms = rel_time_since(now, &it->expires);
		/* [한국어] 클럭 점프 감지 - 예상 범위를 벗어나면 타이머 재설정 */
		if (delta_ms < it->interval_ms - sleep_accuracy_ms ||
		    delta_ms > it->interval_ms + sleep_accuracy_ms) {
			dprint(FD_HELPERTHREAD,
			       "%s: delta = %" PRIi64 " <> %u. Clock jump?\n",
			       it->name, delta_ms, it->interval_ms);
			delta_ms = it->interval_ms;
			it->expires = *now;
			timespec_add_msec(&it->expires, it->interval_ms);
		}
	}
	*msec_to_next_event = min((unsigned int)delta_ms, *msec_to_next_event);
	return expired ? it->func() : 0;  /* 만료 시 콜백 실행 */
}

/*
 * [한국어] 헬퍼 스레드 메인 함수
 *
 * 주기적 타이머 배열 구성:
 *   - disk_util: 디스크 I/O 통계 업데이트 (DISK_UTIL_MSEC 간격)
 *   - status_interval: 실시간 상태 출력 (status_interval 옵션)
 *   - steadystate: SS 도달 여부 체크 (ss_check_interval 간격)
 *   - ramp_period: 램프 기간 완료 체크
 *
 * 메인 루프: 파이프에서 액션 대기 -> 타이머 평가 -> 로그 처리 -> 상태 출력
 */
static void *helper_thread_main(void *data)
{
	struct helper_data *hd = data;
	unsigned int msec_to_next_event, next_log;
	/* [한국어] 주기적 타이머 배열 초기화 */
	struct interval_timer timer[] = {
		{
			.name = "disk_util",
			.interval_ms = DISK_UTIL_MSEC,
			.func = update_io_ticks,       /* 디스크 I/O 틱 업데이트 */
		},
		{
			.name = "status_interval",
			.interval_ms = status_interval,
			.func = __show_running_run_stats, /* 실행 중 통계 출력 */
		},
		{
			.name = "steadystate",
			.interval_ms = steadystate_enabled ? ss_check_interval :
				0,
			.func = steadystate_check,     /* Steady State 도달 체크 */
		},
		{
			.name = "ramp_period",
			.interval_ms = ramp_period_enabled ?
				RAMP_PERIOD_CHECK_MSEC : 0,
			.func = ramp_period_check,     /* 램프 기간 완료 체크 */
		},
	};
	struct timespec ts;
	long clk_tck;
	int ret = 0;

	/* [한국어] 시스템 클럭 틱 조회 - 슬립 정밀도 계산에 사용 */
	os_clk_tck(&clk_tck);

	dprint(FD_HELPERTHREAD, "clk_tck = %ld\n", clk_tck);
	assert(clk_tck > 0);
	sleep_accuracy_ms = (1000 + clk_tck - 1) / clk_tck;  /* 올림 나눗셈 */

#ifdef CONFIG_HAVE_TIMERFD_CREATE
	/* [한국어] 고해상도 타이머 생성 - select() 정밀도 1ms로 향상 */
	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	assert(timerfd >= 0);
	sleep_accuracy_ms = 1;
#endif

	sk_out_assign(hd->sk_out);

	/* Let another thread handle signals. */
	/* [한국어] 시그널 블로킹 - 메인 스레드가 시그널 처리하도록 */
	block_signals();

	fio_get_mono_time(&ts);
	msec_to_next_event = reset_timers(timer, FIO_ARRAY_SIZE(timer), &ts);

	/* [한국어] 초기화 완료를 생성자(helper_thread_create)에게 알림 */
	fio_sem_up(hd->startup_sem);

	/* [한국어] 메인 이벤트 루프 */
	while (!ret && !hd->exit) {
		uint8_t action;
		int i;

		/* [한국어] 다음 타이머 만료까지 대기하며 파이프 액션 수신 */
		action = wait_for_action(hd->pipe[0], msec_to_next_event);
		if (action == A_EXIT)
			break;

		fio_get_mono_time(&ts);

		msec_to_next_event = INT_MAX;

		/* [한국어] A_RESET 수신 시 모든 타이머를 현재 시각 기준으로 리셋 */
		if (action == A_RESET)
			msec_to_next_event = reset_timers(timer,
						FIO_ARRAY_SIZE(timer), &ts);

		/* [한국어] 모든 타이머 평가 - 만료된 타이머의 콜백 실행 */
		for (i = 0; i < FIO_ARRAY_SIZE(timer); ++i)
			ret = eval_timer(&timer[i], &ts, &msec_to_next_event);

		/* [한국어] A_DO_STAT 수신 시 즉시 통계 출력 */
		if (action == A_DO_STAT)
			__show_running_run_stats();

		/* [한국어] 로그 샘플링 주기 계산 */
		next_log = calc_log_samples();
		if (!next_log)
			next_log = DISK_UTIL_MSEC;

		msec_to_next_event = min(next_log, msec_to_next_event);
		dprint(FD_HELPERTHREAD,
		       "next_log: %u, msec_to_next_event: %u\n",
		       next_log, msec_to_next_event);

		/* [한국어] 백엔드 모드가 아니면 스레드 상태 출력 (터미널 표시) */
		if (!is_backend)
			print_thread_status();
	}

	/* [한국어] 정리: timerfd 닫기 */
	if (timerfd >= 0) {
		close(timerfd);
		timerfd = -1;
	}

	/* [한국어] 미기록 로그 데이터 플러시 */
	fio_writeout_logs(false);

	sk_out_drop();
	return NULL;
}

/*
 * Connect two sockets to each other to emulate the pipe() system call on Windows.
 */
/*
 * [한국어] Windows에서 pipe() 시스템 콜을 에뮬레이션
 * 루프백 소켓 2개를 연결하여 파이프처럼 사용
 */
int pipe_over_loopback(int fd[2])
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t len = sizeof(addr);
	int res;

	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	sock_init();

	fd[0] = socket(AF_INET, SOCK_STREAM, 0);
	if (fd[0] < 0)
		goto err;
	fd[1] = socket(AF_INET, SOCK_STREAM, 0);
	if (fd[1] < 0)
		goto close_fd_0;
	res = bind(fd[0], (struct sockaddr *)&addr, len);
	if (res < 0)
		goto close_fd_1;
	res = getsockname(fd[0], (struct sockaddr *)&addr, &len);
	if (res < 0)
		goto close_fd_1;
	res = listen(fd[0], 1);
	if (res < 0)
		goto close_fd_1;
	res = connect(fd[1], (struct sockaddr *)&addr, len);
	if (res < 0)
		goto close_fd_1;
	res = accept(fd[0], NULL, NULL);
	if (res < 0)
		goto close_fd_1;
	close(fd[0]);
	fd[0] = res;   /* accept()로 얻은 fd를 읽기 끝으로 사용 */
	return 0;

close_fd_1:
	close(fd[1]);

close_fd_0:
	close(fd[0]);

err:
	return -1;
}

/*
 * [한국어] 헬퍼 스레드 생성
 * 1) 공유 메모리에 helper_data 할당
 * 2) 디스크 유틸리티 및 Steady State 초기 설정
 * 3) 파이프 생성 (통신용) 및 논블로킹 설정
 * 4) 헬퍼 스레드 시작 후 초기화 완료 대기
 */
int helper_thread_create(struct fio_sem *startup_sem, struct sk_out *sk_out)
{
	struct helper_data *hd;
	int ret;

	hd = scalloc(1, sizeof(*hd));  /* 공유 메모리에 0으로 초기화하여 할당 */
	if (!hd)
		return 1;

	setup_disk_util();       /* 디스크 유틸리티 초기화 */
	steadystate_setup();     /* Steady State 데이터 배열 할당 */

	hd->sk_out = sk_out;

	/* [한국어] 파이프 생성 - 플랫폼별 분기 */
#if defined(CONFIG_PIPE2)
	ret = pipe2(hd->pipe, O_CLOEXEC);     /* Linux: exec 시 자동 닫힘 */
#elif defined(CONFIG_PIPE)
	ret = pipe(hd->pipe);                  /* POSIX 표준 파이프 */
#else
	ret = pipe_over_loopback(hd->pipe);    /* Windows: 루프백 소켓 에뮬레이션 */
#endif
	if (ret)
		return 1;

	/* [한국어] 읽기 끝을 논블로킹으로 설정 (select 기반 이벤트 루프용) */
	ret = make_nonblocking(hd->pipe[0]);
	assert(ret >= 0);

	hd->startup_sem = startup_sem;

	DRD_IGNORE_VAR(helper_data);  /* Valgrind DRD: 의도된 레이스 무시 */

	ret = pthread_create(&hd->thread, NULL, helper_thread_main, hd);
	if (ret) {
		log_err("Can't create helper thread: %s\n", strerror(ret));
		return 1;
	}

	helper_data = hd;

	/* [한국어] 헬퍼 스레드 초기화 완료 대기 */
	dprint(FD_MUTEX, "wait on startup_sem\n");
	fio_sem_down(startup_sem);
	dprint(FD_MUTEX, "done waiting on startup_sem\n");
	return 0;
}
