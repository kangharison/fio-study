/*
 * [한국어 설명] exec I/O 엔진 구현 (exec.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 exec I/O 엔진을 구현한다. 이 엔진은 **데이터 I/O를 전혀 수행하지
 * 않는 "외부 프로세스 실행 측정 엔진"**이다. read/write 경로로 블록을 전송하는 대신,
 * 사용자가 지정한 외부 프로그램(program 옵션)을 fork(2)+execvp(3)로 기동하고,
 * 해당 프로그램이 동작하는 동안 fio의 thinktime 루프가 주기적으로 폴링하며,
 * 잡의 timeout이 경과하면 SIGTERM → grace_time 대기 → SIGKILL 순서로 자식 프로세스를
 * 정리한다. std_redirect=1일 경우 자식의 stdout/stderr를 <jobname>.stdout,
 * <jobname>.stderr 파일로 리다이렉트한다. arguments 문자열 내 "%r"은 런타임(초),
 * "%n"은 잡 이름으로 치환된다. 주로 "워크로드 재생 도구(stress-ng, dd 등)를 특정
 * 시점에 트리거해 타 잡과 동시에 실행"하는 혼합 시나리오에 활용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 I/O 엔진 플러그인 계약(struct ioengine_ops)을 따르는 엔진 중 하나로,
 * ioengines.c::td_io_queue()에서 본 파일의 fio_exec_queue()가 호출된다. 일반
 * 엔진(libaio, sync 등)은 io_u 하나당 하나의 블록 전송을 수행하지만, exec 엔진은
 * io_u를 무시하고(인자에 fio_unused 표시) FIO_Q_COMPLETED를 즉시 반환하여 fio의
 * 통계 회계에는 참여하되 실제 전송 바이트는 0이 되게 한다. 실행 컨텍스트는 잡당
 * 1개의 잡 스레드(또는 --thread=0 프로세스)이며, fork된 자식은 별도 주소 공간에서
 * 독립적으로 동작한다. 엔진 플래그 FIO_DISKLESSIO|FIO_NOIO로 fio 코어에 "이 엔진은
 * 파일을 필요로 하지 않고 실제 I/O를 하지 않음"을 통지한다. FIO_SYNCIO 플래그로
 * 동기 엔진(commit/getevents 별도 호출 불필요)임을 알린다.
 *
 * 호출 체인(간략):
 *   main() → fio_backend() → thread_main() (잡 스레드)
 *     → td_io_init() → fio_exec_init()
 *     → (잡 루프) td_io_queue() → fio_exec_queue() → exec_background() → fork/execvp
 *     → ... thinktime 루프 ... kill(SIGTERM) → sleep(grace_time)
 *     → td_io_cleanup() → fio_exec_cleanup() → kill(SIGKILL)
 *
 * === 타 모듈과의 연결 ===
 * - 상위 호출자: ioengines.c (td_io_queue, td_io_init, td_io_cleanup).
 *   → td_set_runstate()/td_vmsg()는 backend.c/stat.c에 정의.
 *   → utime_since_now()는 gettime.c에서 현재 시각과 td->start 간 마이크로초 차이 반환.
 * - 하위 호출자(자신이 호출하는 것):
 *   fork(2), execvp(3)  — POSIX 프로세스 생성/대체 (Linux: clone(2) 래퍼).
 *   waitpid(2)는 본 엔진에서는 직접 호출하지 않고 부모가 자식을 blocking 대기하지 않는다
 *   대신 주기적 polling(kill + 종료)으로 제어한다.
 *   kill(2) — 지정 시그널(SIGTERM/SIGKILL)을 자식 PID에 전달.
 *   signal 관련 매크로: SIGTERM(정중한 종료 요청, 자식이 핸들러로 트랩 가능),
 *                      SIGKILL(커널이 강제 종료, 트랩 불가).
 *   open(2)/dup2(2)/close(2) — stdout/stderr 파일 리다이렉트용.
 *   asprintf(3)/snprintf(3)/strstr(3)/strspn(3)/strcspn(3) — 문자열 가공.
 *   usleep(3)/sleep(3) — polling 대기.
 * - 옵션 시스템: optgroup.h의 FIO_OPT_C_ENGINE 카테고리에 속하지만 group은
 *   FIO_OPT_G_INVALID(서브그룹 없음)로 등록.
 * - 공유 상태: thread_data::eo (io_options_struct)에 struct exec_options가 저장됨.
 *   eo->pid에 fork 결과 저장. cleanup과 queue가 동일 잡 스레드에서 접근하므로 별도
 *   락 불필요.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct exec_options: program/arguments/grace_time/std_redirect/pid를 저장.
 * - str_replace(): orig 내 모든 rep를 with로 치환하여 새 문자열 반환 (malloc).
 * - expand_variables(): arguments 내 %r, %n을 런타임/잡명으로 치환.
 * - exec_background(): fork하여 자식에서 execvp, 부모는 pid 저장 후 즉시 반환.
 * - fio_exec_queue(): 최초 호출 시 exec_background 기동, 이후 호출은 thinktime 대기
 *   와 timeout 감시(kill SIGTERM + grace_time).
 * - fio_exec_init(): program 옵션 검증, thinktime=50ms, nr_files=1 설정, pid=-1 초기화.
 * - fio_exec_cleanup(): 자식 pid>0이면 SIGKILL로 강제 정리.
 * - fio_exec_open(): no-op (FIO_DISKLESSIO이므로 실파일 없음).
 * - ioengine 등록: fio_init/fio_exit 생성자/소멸자를 통해 load-time 등록.
 */

/*
 * Exec engine
 *
 * Doesn't transfer any data, merely run 3rd party tools
 *
 */
/* [한국어] fio 핵심 헤더: struct thread_data, struct io_u, fio_q_status, td_set_runstate,
 * td_vmsg, log_info/log_err, fio_unused, fio_init/fio_exit, register_ioengine 등을 포함.
 * 이 엔진이 fio 플러그인 계약을 따르기 위해 필수. */
#include "../fio.h"
/* [한국어] fio 옵션 시스템: FIO_OPT_STR_STORE/FIO_OPT_INT/FIO_OPT_BOOL 같은 타입과
 * FIO_OPT_C_ENGINE/FIO_OPT_G_INVALID 등 카테고리/그룹 상수 정의. fio_option 등록 시 필요. */
#include "../optgroup.h"
/* [한국어] POSIX signal 인터페이스. 이 엔진이 자식 프로세스에게 SIGTERM/SIGKILL을
 * 보내기 위해 사용 (kill(2)의 인자 상수 정의). */
#include <signal.h>

/* [한국어] exec 엔진 전용 옵션 구조체.
 * fio의 옵션 파서는 아래 options[] 테이블의 off1 오프셋을 이용해 이 구조체의 필드에
 * 값을 직접 기록한다. thread_data::eo 포인터를 통해 접근되며, 잡마다 하나씩 존재.
 * 설정자: fio 옵션 파서(options.c). 읽는 자: 본 파일의 모든 함수.
 * 동기화: 잡 스레드 단일 쓰기/읽기이므로 락 불필요. */
struct exec_options {
	void *pad;
	/* [한국어] fio 옵션 시스템의 정렬 패딩 바이트.
	 * 설정자: 컴파일러(구조체 선두에 위치시키기 위함).
	 * 읽는 자: fio 옵션 파서가 off1 오프셋을 계산할 때 0번째 오프셋을 건너뛰게 함
	 *          (옛 fio 버전 호환성 및 옵션 메타데이터 배치 관례).
	 * 값 범위: 사용되지 않는 더미 포인터. 항상 NULL.
	 * 동기화: 접근되지 않음. */

	char *program;
	/* [한국어] 외부에서 실행할 프로그램의 경로(바이너리 이름). 필수 옵션.
	 * 설정자: fio 명령줄/잡 파일 파서가 "program=..." 옵션을 파싱하여 strdup 할당.
	 * 읽는 자: fio_exec_init()(검증), exec_background()(execvp 첫 인자).
	 * 값 범위: NULL 불가(초기화에서 검증). PATH 검색이 execvp에 의해 수행됨.
	 * 동기화: 잡 시작 후 불변이므로 락 불필요. */

	char *arguments;
	/* [한국어] 프로그램에 전달할 인수 문자열(공백 구분). 옵션.
	 * 특수 치환: "%r" → 잡 timeout(초), "%n" → 잡 이름. expand_variables()가 처리.
	 * 설정자: fio 옵션 파서.
	 * 읽는 자: expand_variables() → str_replace()로 치환 후 공백 분리되어 argv 구성.
	 * 값 범위: NULL 가능(인수 없이 실행). 문자열 소유권은 fio 옵션 시스템.
	 * 동기화: 잡 시작 후 불변. */

	int grace_time;
	/* [한국어] SIGTERM 송신 후 SIGKILL까지 대기할 유예 시간(초). 기본 1초.
	 * 설정자: fio 옵션 파서(기본값 "1" → 1초).
	 * 읽는 자: fio_exec_queue()가 timeout 초과 시 sleep(eo->grace_time) 호출.
	 * 값 범위: 0 이상의 정수. 0이면 즉시 SIGKILL이 cleanup에서 전달됨.
	 * 동기화: 불변. */

	unsigned int std_redirect;
	/* [한국어] 자식 프로세스의 stdout/stderr를 파일로 리다이렉트할지 여부(불리언).
	 * 설정자: fio 옵션 파서. 기본값 "1"(활성).
	 * 읽는 자: exec_background()가 분기하여 open+dup2 수행.
	 * 값 범위: 0 또는 1. FIO_OPT_BOOL 타입.
	 * 동기화: 불변. */

	pid_t pid;
	/* [한국어] fork(2)로 생성된 자식 프로세스의 PID. -1은 "아직 실행 전"의 센티넬.
	 * 설정자: fio_exec_init()(-1로 초기화), exec_background()(부모측에서 실제 PID 저장).
	 * 읽는 자: fio_exec_queue()(최초 실행 분기 판정/타임아웃 시 kill),
	 *          fio_exec_cleanup()(종료 시 SIGKILL).
	 * 값 범위: -1(미시작), >0(실행 중 혹은 좀비). fork 실패 시 -1 유지.
	 * 동기화: 단일 잡 스레드 내 접근이므로 락 불필요. */
};

/* [한국어] fio 옵션 테이블. fio 코어(options.c)는 엔진 등록 시 ioengine.options를
 * 따라가 각 엔트리를 잡 파일/CLI 옵션으로 노출한다. .off1은 struct exec_options 내
 * 저장 오프셋. 마지막 엔트리 .name=NULL이 테이블 종단 표시. */
static struct fio_option options[] = {
	{
		.name     = "program",                                 /* [한국어] 옵션 키 이름 (잡 파일에 "program=..." 형태) */
		.lname    = "Program",                                 /* [한국어] 긴 이름(도움말용) */
		.type     = FIO_OPT_STR_STORE,                         /* [한국어] 문자열 저장 타입 (strdup으로 복사) */
		.off1     = offsetof(struct exec_options, program),    /* [한국어] exec_options::program 필드에 기록 */
		.help     = "Program to execute",                      /* [한국어] --cmdhelp 출력 */
		.category = FIO_OPT_C_ENGINE,                          /* [한국어] 카테고리: 엔진 옵션 */
		.group    = FIO_OPT_G_INVALID,                         /* [한국어] 서브 그룹 없음 */
	},
	{
		.name     = "arguments",                               /* [한국어] 인수 문자열 옵션 키 */
		.lname    = "Arguments",
		.type     = FIO_OPT_STR_STORE,                         /* [한국어] 문자열로 저장 (치환은 런타임에) */
		.off1     = offsetof(struct exec_options, arguments),
		.help     = "Arguments to pass",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "grace_time",                              /* [한국어] SIGTERM→SIGKILL 유예시간(초) */
		.lname    = "Grace time",
		.type     = FIO_OPT_INT,                               /* [한국어] 정수 타입 */
		.minval   = 0,                                         /* [한국어] 음수 금지(즉시 kill은 0으로 표현) */
		.def      = "1",                                       /* [한국어] 기본 1초 */
		.off1     = offsetof(struct exec_options, grace_time),
		.help     = "Grace time before sending a SIGKILL",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name     = "std_redirect",                            /* [한국어] stdout/stderr 리다이렉트 토글 */
		.lname    = "Std redirect",
		.type     = FIO_OPT_BOOL,                              /* [한국어] 불리언(0/1) */
		.def      = "1",                                       /* [한국어] 기본 활성 */
		.off1     = offsetof(struct exec_options, std_redirect),
		.help     = "Redirect stdout & stderr to files",
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name = NULL,                                          /* [한국어] 테이블 종단 표식 (fio 옵션 파서의 루프 종료 조건) */
	},
};

/*
 * [한국어]
 * str_replace - orig 문자열 내 모든 rep 부분 문자열을 with로 치환한 새 문자열 반환
 *
 * @orig: 원본 문자열 (NULL 가능 → 즉시 반환)
 * @rep:  치환 대상 substring (NULL/빈 문자열 → orig 반환)
 * @with: 치환 값 (NULL이면 ""로 처리 → rep를 빈 문자열로 대체 = 삭제)
 * @return: 치환된 새 문자열(malloc된 메모리, 호출자 소유). 치환 대상 없거나 실패 시
 *          orig 반환(이 경우 호출자는 반환값이 orig와 동일한지 보고 해제 여부 결정 필요).
 *
 * 왜 필요한가: arguments 옵션 내 "%r", "%n" 치환(expand_variables)을 위해 사용하는
 * 범용 치환 함수. fio가 제공하지 않는 동적 치환을 자체 구현.
 *
 * 동작 단계:
 *  1) sanity check: orig/rep NULL, rep 빈 문자열 → orig 그대로 반환.
 *  2) 1차 패스: orig 내 rep 등장 횟수 count를 센다.
 *  3) 결과 길이 = strlen(orig) + (len_with - len_rep) * count + 1 로 malloc.
 *  4) 2차 패스: rep 앞까지 복사 → with 복사 → orig 포인터 전진, count 소진까지 반복.
 *  5) 남은 orig 꼬리를 복사하여 종결.
 *
 * 실행 컨텍스트: 잡 스레드의 init/queue 경로에서만 호출. 재진입 없음.
 * 호출 체인: expand_variables() → [이 함수]
 *
 * 주의: 반환 메모리가 orig 그대로면 free()하면 안 된다(이중 해제 위험).
 * expand_variables()의 현재 구현은 이 분기에서 잠재적 미해제가 발생할 수 있으나,
 * 본 주석 작업은 코드 수정 없이 관찰만 한다.
 */
static char *str_replace(char *orig, const char *rep, const char *with)
{
	/*
	 * Replace a substring by another.
	 *
	 * Returns the new string if occurrences were found
	 * Returns orig if no occurrence is found
	 */
	char *result, *insert, *tmp;        /* [한국어] 작업용 포인터: result=신규 버퍼 시작, insert=탐색 커서, tmp=쓰기 커서 */
	int len_rep, len_with, len_front, count; /* [한국어] 길이 캐시 및 등장 횟수 */

	/* sanity checks and initialization */
	/* [한국어] NULL 방어: 원본 또는 치환 패턴이 없으면 치환 불가 → 원본 그대로 반환. */
	if (!orig || !rep)
		return orig;

	len_rep = strlen(rep);              /* [한국어] 치환 대상 길이 계산 (매 루프 반복 피함) */
	/* [한국어] 빈 패턴은 무한 루프 위험 + 의미 없음 → 원본 반환. */
	if (len_rep == 0)
		return orig;

	/* [한국어] with가 NULL이면 ""로 취급: rep를 삭제하는 효과. */
	if (!with)
		with = "";
	len_with = strlen(with);            /* [한국어] 치환값 길이 캐시 */

	insert = orig;                      /* [한국어] 탐색 커서를 원본 시작으로 */
	/* [한국어] 1차 패스: strstr로 rep 등장 횟수 count를 센다. 커서를 매번 len_rep만큼
	 * 전진시켜 중첩 매칭 방지. */
	for (count = 0; (tmp = strstr(insert, rep)); ++count) {
		insert = tmp + len_rep;         /* [한국어] 방금 찾은 rep 바로 뒤부터 다음 탐색 */
	}

	/* [한국어] 결과 버퍼 할당: 원본 길이 + (with-rep)*count + NUL 1바이트.
	 * (len_with - len_rep)이 음수여도 count배 적용되어 정확한 크기 산출. */
	tmp = result = malloc(strlen(orig) + (len_with - len_rep) * count + 1);

	/* [한국어] malloc 실패 시 원본 반환(호출자에게 치환 실패를 투명하게). */
	if (!result)
		return orig;

	/* [한국어] 2차 패스: count 횟수만큼 (rep 앞 구간 복사 → with 복사 → orig 전진) 반복. */
	while (count--) {
		insert = strstr(orig, rep);      /* [한국어] 현재 orig에서 다음 rep 위치 찾기 */
		len_front = insert - orig;       /* [한국어] rep 앞 구간 길이 */
		tmp = strncpy(tmp, orig, len_front) + len_front; /* [한국어] 앞 구간 복사 후 쓰기 커서 전진 */
		tmp = strcpy(tmp, with) + len_with;              /* [한국어] with 복사 후 커서 전진 (NUL은 아직 덮어씀) */
		orig += len_front + len_rep;     /* [한국어] 원본 커서를 rep 뒤로 전진 */
	}
	/* [한국어] 남은 꼬리 복사 (strcpy가 NUL까지 복사하여 문자열 종결). */
	strcpy(tmp, orig);
	return result;                       /* [한국어] 새 버퍼 반환 (호출자가 free 책임) */
}

/*
 * [한국어]
 * expand_variables - arguments 내 "%r"/"%n" 변수를 런타임/잡명으로 치환
 *
 * @o:         잡의 thread_options (timeout, name 읽음)
 * @arguments: 원본 인수 문자열 (eo->arguments)
 * @return:    치환된 새 문자열 (malloc 메모리 또는 arguments 그대로의 포인터)
 *
 * 왜 필요한가: 사용자가 jobfile에 "arguments=run %r seconds as %n"처럼 작성했을 때,
 * 실행 시 %r을 잡 timeout 초, %n을 잡 이름으로 바꾼 실제 명령줄을 만들어야 한다.
 *
 * 동작:
 *   1) o->timeout(마이크로초)을 1_000_000으로 나눠 초 단위 문자열 "str"에 저장.
 *   2) str_replace로 %r → str 치환 → expanded_runtime.
 *   3) str_replace로 expanded_runtime 내 %n → o->name 치환 → expanded_name.
 *   4) 중간 버퍼 expanded_runtime 해제 후 expanded_name 반환.
 *
 * 실행 컨텍스트: 잡 스레드 exec_background() 경로에서만 호출.
 * 호출 체인: exec_background() → [이 함수] → str_replace()
 *
 * 주의: str_replace가 치환 대상 없을 때 원본 포인터를 그대로 반환하므로,
 * arguments == expanded_runtime인 경우 free(expanded_runtime)은 arguments를
 * 이중 해제할 위험이 있다(원본 코드 이슈, 본 작업에서는 수정하지 않음).
 */
static char *expand_variables(const struct thread_options *o, char *arguments)
{
	char str[16];                             /* [한국어] 초 단위 timeout을 십진 문자열로 저장할 버퍼(최대 ~15자리) */
	char *expanded_runtime, *expanded_name;   /* [한국어] 단계별 치환 결과 포인터 */
	/* [한국어] o->timeout은 fio 코어에서 마이크로초 단위로 저장됨.
	 * 1,000,000으로 나눠 초로 변환 후 "%lld" 포맷으로 문자열화. */
	snprintf(str, sizeof(str), "%lld", o->timeout / 1000000);

	/* %r is replaced by the runtime in seconds */
	/* [한국어] 1단계: "%r" → 런타임 초 문자열로 치환. */
	expanded_runtime = str_replace(arguments, "%r", str);

	/* %n is replaced by the name of the running job */
	/* [한국어] 2단계: 그 결과에서 "%n" → 잡 이름(o->name)으로 치환. */
	expanded_name = str_replace(expanded_runtime, "%n", o->name);

	/* [한국어] 중간 버퍼 해제. (str_replace가 치환 없이 arguments를 그대로 돌려줬다면
	 * 여기서 arguments가 해제될 위험 — 원본 이슈.) */
	free(expanded_runtime);
	return expanded_name;                     /* [한국어] 최종 치환 결과 반환 */
}

/*
 * [한국어]
 * exec_background - 외부 프로그램을 백그라운드로 fork + execvp
 *
 * @o:  thread_options (name/timeout 읽기)
 * @eo: exec_options (program/arguments/std_redirect/grace_time/pid)
 * @return: 부모 경로에서 성공 시 0, 실패 시 -1. 자식 경로는 execvp 성공 시 도달하지 않음.
 *
 * 왜 필요한가: fio 잡 스레드의 주소 공간 안에서 exec(3)를 직접 호출하면 fio 자체가
 * 치환되어 버린다. 따라서 fork(2)로 자식을 만들고 자식에서만 execvp를 호출해야 한다.
 *
 * 동작 단계:
 *   1) std_redirect=1이면 "<jobname>.stdout", "<jobname>.stderr" 출력 파일을 open.
 *   2) arguments에서 %r/%n 치환.
 *   3) fork() 호출 → 부모는 eo->pid 저장 후 return, 실패는 -1 반환, 자식만 아래 진입.
 *   4) 자식: std_redirect=1이면 dup2로 stdout(fd=1)/stderr(fd=2)를 출력 파일로 덮어씀.
 *   5) "program arguments" 형태 명령줄을 asprintf로 조립 후, 공백 기준으로 argv 배열 구성.
 *   6) execvp(argv[0], argv) — 현재 자식 프로세스 이미지를 대상 프로그램으로 대체.
 *      성공 시 이후 코드는 실행되지 않는다. 실패 시 malloc된 메모리 해제 후 0 반환.
 *
 * 실행 컨텍스트:
 *   - fork 호출 시 부모/자식이 분기. 부모는 잡 스레드, 자식은 새 프로세스.
 *   - execvp 이후 자식은 대상 프로그램의 제어 흐름에 따라 동작.
 *   - 부모는 wait 하지 않고 즉시 반환 → fio_exec_queue가 이후 thinktime 루프로 감시.
 *
 * 호출 체인: fio_exec_queue() → [이 함수] → fork(2)/execvp(3)/open(2)/dup2(2)
 *
 * 에러 경로: open 실패 또는 fork 실패 시 부모 측에서 자원 해제 후 -1 반환.
 */
static int exec_background(const struct thread_options *o, struct exec_options *eo)
{
	char *outfilename = NULL, *errfilename = NULL; /* [한국어] stdout/stderr 리다이렉트 파일명(asprintf 할당) */
	int outfd = 0, errfd = 0;                      /* [한국어] 해당 파일 디스크립터 */
	pid_t pid;                                     /* [한국어] fork 반환값(0=자식, >0=부모가 본 자식 PID, <0=실패) */
	char *expanded_arguments = NULL;               /* [한국어] %r/%n 치환된 인수 문자열 */
	/* For the arguments splitting */
	char **arguments_array = NULL;                 /* [한국어] execvp에 넘길 argv 배열 (동적 확장) */
	char *p;                                       /* [한국어] 명령줄 파싱 커서 */
	char *exec_cmd = NULL;                         /* [한국어] "program arguments" 합친 전체 명령줄 */
	size_t arguments_nb_items = 0, q;              /* [한국어] argv 원소 수, 현재 토큰 길이 */

	/* [한국어] 출력 파일명 생성: "<잡이름>.stdout". asprintf는 필요한 크기를 내부 할당. */
	if (asprintf(&outfilename, "%s.stdout", o->name) < 0)
		return -1;                             /* [한국어] 할당 실패 → 즉시 -1 (outfilename은 미정의) */

	/* [한국어] 에러 파일명 생성. 실패 시 앞서 할당한 outfilename도 해제. */
	if (asprintf(&errfilename, "%s.stderr", o->name) < 0) {
		free(outfilename);
		return -1;
	}

	/* If we have variables in the arguments, let's expand them */
	/* [한국어] arguments 내 %r, %n 치환. NULL인 경우 expand_variables → str_replace가
	 * NULL을 그대로 돌려주므로 expanded_arguments도 NULL. */
	expanded_arguments = expand_variables(o, eo->arguments);

	/* [한국어] stdout/stderr 리다이렉트가 활성일 경우: 출력 파일을 open하고,
	 * fork 후 자식에서 dup2로 fd 1/2에 덮어쓴다. */
	if (eo->std_redirect) {
		/* [한국어] 사용자에게 리다이렉트 대상 파일 안내. */
		log_info("%s : Saving output of %s %s : stdout=%s stderr=%s\n",
			 o->name, eo->program, expanded_arguments, outfilename,
			 errfilename);

		/* Creating the stderr & stdout output files */
		/* [한국어] stdout용 파일 open.
		 *   O_CREAT: 없으면 생성. O_WRONLY: 쓰기 전용. O_TRUNC: 기존 내용 잘라냄.
		 *   mode 0644: 소유자 rw, 그룹/기타 r. */
		outfd = open(outfilename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (outfd < 0) {
			/* [한국어] open 실패 경로 — 이미 할당된 문자열 모두 해제 후 -1. */
			log_err("fio: cannot open output file %s : %s\n",
				outfilename, strerror(errno));
			free(outfilename);
			free(errfilename);
			free(expanded_arguments);
			return -1;
		}

		/* [한국어] stderr용 파일 open (동일 플래그/모드). */
		errfd = open(errfilename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (errfd < 0) {
			log_err("fio: cannot open output file %s : %s\n",
				errfilename, strerror(errno));
			free(outfilename);
			free(errfilename);
			free(expanded_arguments);
			close(outfd);                /* [한국어] 먼저 연 outfd는 명시적 close */
			return -1;
		}
	} else {
		/* [한국어] 리다이렉트 비활성 — 사용자에게 실행 사실만 통지. */
		log_info("%s : Running %s %s\n",
			 o->name, eo->program, expanded_arguments);
	}

	/* [한국어] fork(2): 커널이 현재 프로세스를 복제하여 두 개의 프로세스(동일 코드,
	 * 독립 주소 공간)로 분기한다. 반환값으로 부모/자식을 구분:
	 *   pid > 0 : 부모가 본 자식 PID
	 *   pid == 0: 자식 프로세스 자신
	 *   pid < 0 : 실패(errno 설정)
	 * Linux에서는 실제로 clone(2)을 특정 플래그로 호출하여 구현됨. */
	pid = fork();

	/* We are on the control thread (parent side of the fork */
	/* [한국어] 부모 경로: 자식 PID 저장 + 부모 측 자원 해제 후 즉시 반환. */
	if (pid > 0) {
		eo->pid = pid;                        /* [한국어] 이후 queue/cleanup에서 kill 대상으로 사용 */
		if (eo->std_redirect) {
			/* The output file is for the client side of the fork */
			/* [한국어] 자식이 dup2로 복제한 fd는 자식에 유효. 부모 쪽 원본 fd는 닫는다. */
			close(outfd);
			close(errfd);
			free(outfilename);
			free(errfilename);
		}
		free(expanded_arguments);             /* [한국어] 치환 문자열은 부모가 소유하므로 해제 */
		return 0;                             /* [한국어] 성공: 잡 스레드는 queue 루프로 복귀 */
	}

	/* If the fork failed */
	/* [한국어] fork 실패 경로: errno 기반 에러 메시지 + 자원 해제 + -1 반환. */
	if (pid < 0) {
		log_err("fio: forking failed %s \n", strerror(errno));
		if (eo->std_redirect) {
			close(outfd);
			close(errfd);
			free(outfilename);
			free(errfilename);
		}
		free(expanded_arguments);
		return -1;
	}

	/* We are in the worker (child side of the fork) */
	/* [한국어] 자식 경로: pid == 0. 여기서부터 execvp 호출 시점까지가 자식 코드. */
	if (pid == 0) {
		if (eo->std_redirect) {
			/* replace stdout by the output file we create */
			/* [한국어] dup2(outfd, 1): fd 1(표준출력)을 outfd 복제본으로 치환.
			 * 이후 자식이 printf/fprintf(stdout)을 하면 outfilename 파일에 기록됨.
			 * 기존 fd 1은 자동으로 close된다(dup2 의미). */
			dup2(outfd, 1);
			/* replace stderr by the output file we create */
			/* [한국어] dup2(errfd, 2): fd 2(표준에러)를 errfd로 치환. */
			dup2(errfd, 2);
			close(outfd);                 /* [한국어] 원본 fd는 더 이상 필요 없음 — 누수 방지 close */
			close(errfd);
			free(outfilename);
			free(errfilename);
		}

		/*
		 * Let's split the command line into a null terminated array to
		 * be passed to the exec'd program.
		 * But don't asprintf expanded_arguments if NULL as it would be
		 * converted to a '(null)' argument, while we want no arguments
		 * at all.
		 */
		/* [한국어] expanded_arguments가 NULL이 아니면 "program args" 형태 조립.
		 * NULL이면 "(null)" 문자열이 들어가지 않도록 분기하여 "program"만 조립. */
		if (expanded_arguments != NULL) {
			if (asprintf(&exec_cmd, "%s %s", eo->program, expanded_arguments) < 0) {
				free(expanded_arguments); /* [한국어] 실패 시 자식에서 최소한의 정리 후 종료 */
				return -1;
			}
		} else {
			if (asprintf(&exec_cmd, "%s", eo->program) < 0)
				return -1;
		}

		/*
		 * Let's build an argv array to based on the program name and
		 * arguments
		 */
		/* [한국어] exec_cmd를 공백 구분 토큰으로 분리하여 argv 배열을 구성.
		 * 각 토큰을 malloc 복사하고 배열 끝에 NULL을 추가해 execvp 계약 충족. */
		p = exec_cmd;
		for (;;) {
			p += strspn(p, " ");          /* [한국어] 선행 공백 건너뜀(strspn: 공백만 있는 최장 접두 길이) */

			/* [한국어] 다음 공백까지 길이가 0이면 더 이상 토큰 없음 → 루프 탈출. */
			if (!(q = strcspn(p, " ")))
				break;

			if (q) {
				/* [한국어] argv 배열을 한 원소 확장 (realloc: 기존 내용 유지). */
				arguments_array =
				    realloc(arguments_array,
					    (arguments_nb_items +
					     1) * sizeof(char *));
				/* [한국어] 토큰 저장용 q+1 바이트 할당 (+1은 NUL). */
				arguments_array[arguments_nb_items] =
				    malloc(q + 1);
				/* [한국어] 원문에서 토큰 q바이트 복사. */
				strncpy(arguments_array[arguments_nb_items], p,
					q);
				/* [한국어] 명시적 NUL 종결 (strncpy는 q바이트 모두 채워질 경우 NUL 미부여). */
				arguments_array[arguments_nb_items][q] = 0;
				arguments_nb_items++;
				p += q;                  /* [한국어] 커서를 이번 토큰 뒤로 전진 */
			}
		}

		/* Adding a null-terminated item to close the list */
		/* [한국어] execvp는 argv가 NULL로 종료되어야 한다. 마지막 원소를 NULL로. */
		arguments_array =
		    realloc(arguments_array,
			    (arguments_nb_items + 1) * sizeof(char *));
		arguments_array[arguments_nb_items] = NULL;

		/*
		 * Replace the fio program from the child fork by the target
		 * program
		 */
		/* [한국어] execvp(file, argv): 커널이 현재 프로세스의 이미지(코드/데이터/힙/스택)를
		 * 대상 프로그램의 이미지로 **완전히 대체**한다.
		 *   - "vp": argv 벡터 사용 + PATH 환경변수 검색.
		 *   - 성공 시 반환하지 않음 — 이후 코드는 실행 불가.
		 *   - 실패 시에만 -1 반환 + errno 설정. */
		execvp(arguments_array[0], arguments_array);
	}
	/* We never reach this place */
	/* Let's free the malloc'ed structures to make static checkers happy */
	/* [한국어] execvp 성공 시 도달 불가. 정적 분석기 경고 회피를 위해 해제 코드를 남겨둠.
	 * 만약 execvp가 실패해 여기에 도달해도 자식 프로세스이므로 최소 정리 후 return. */
	if (expanded_arguments)
		free(expanded_arguments);
	if (arguments_array)
		free(arguments_array);
	return 0;
}

/*
 * [한국어]
 * fio_exec_queue - exec 엔진의 queue 콜백 (io_u 무시)
 *
 * @td: 잡의 thread_data
 * @io_u: 사용되지 않음 (데이터 I/O 없음 — fio_unused 표시)
 * @return: 항상 FIO_Q_COMPLETED (즉시 완료; fio가 바로 통계 집계)
 *
 * 왜 필요한가: fio의 엔진 계약에서 queue 콜백은 I/O를 제출하는 지점이다. exec 엔진은
 * 데이터 I/O 없이 외부 프로그램을 실행하는 역할이므로, "첫 queue 호출 = 프로그램 시작",
 * "이후 queue 호출 = thinktime 단위 감시 틱"으로 재해석한다.
 *
 * 동작:
 *   - eo->pid == -1: 아직 실행 안 함 → exec_background()로 자식 fork+exec.
 *   - eo->pid != -1: 이미 실행 중 → thinktime 만큼 usleep 후 timeout 경과 검사.
 *     timeout 초과면 SIGTERM 송신 + sleep(grace_time)으로 자식에게 정리 시간 부여.
 *
 * 실행 컨텍스트: 잡 스레드. td_io_queue에서 매 I/O 루프 반복마다 호출.
 * 재진입/동시성: 잡당 단일 스레드 호출이므로 경쟁 없음.
 *
 * 호출 체인: backend.c::td_io_queue() → [이 함수] → exec_background()/kill(2)
 *
 * 에러 경로: exec_background 실패 시에도 FIO_Q_COMPLETED 반환(현 구현). 잡은 정상
 * 종료되지만 실제 외부 프로그램은 시작되지 않음.
 */
static enum fio_q_status
fio_exec_queue(struct thread_data *td, struct io_u fio_unused * io_u)
{
	struct thread_options *o = &td->o;     /* [한국어] 잡 옵션 (timeout, thinktime 읽기) */
	struct exec_options *eo = td->eo;      /* [한국어] 엔진별 옵션/상태 */

	/* Let's execute the program the first time we get queued */
	/* [한국어] 첫 호출 분기: pid 센티넬(-1)이면 프로그램을 아직 시작하지 않은 상태. */
	if (eo->pid == -1) {
		exec_background(o, eo);        /* [한국어] fork+execvp로 자식 시작 (eo->pid 업데이트) */
	} else {
		/*
		 * The program is running in background, let's check on a
		 * regular basis
		 * if the time is over and if we need to stop the tool
		 */
		/* [한국어] thinktime(기본 50ms = 50000us)만큼 대기. CPU 점유를 낮추면서 폴링.
		 * usleep은 SIGALRM 등에 의해 조기 종료 가능. */
		usleep(o->thinktime);
		/* [한국어] 잡 시작(td->start)으로부터 현재까지 경과 시간(us)을 timeout(us)과 비교.
		 * utime_since_now는 gettime.c 정의, 단조증가 시계 기반. */
		if (utime_since_now(&td->start) > o->timeout) {
			/* Let's stop the child */
			/* [한국어] kill(2)로 자식에게 SIGTERM 전달. 자식이 시그널 핸들러를 설치해
			 * 두었다면 깨끗한 종료 가능. 커널은 프로세스 테이블에서 pid를 찾아
			 * 시그널을 pending 큐에 넣는다. */
			kill(eo->pid, SIGTERM);
			/*
			 * Let's give grace_time (1 sec by default) to the 3rd
			 * party tool to stop
			 */
			/* [한국어] grace_time(초) 만큼 휴면. 자식이 자발적으로 종료할 여유를 줌.
			 * 이후에도 살아있으면 fio_exec_cleanup()에서 SIGKILL로 강제 종료. */
			sleep(eo->grace_time);
		}
	}

	/* [한국어] fio 코어에게 "이 I/O는 즉시 완료됨"을 통지. 전송 바이트는 0. */
	return FIO_Q_COMPLETED;
}

/*
 * [한국어]
 * fio_exec_init - exec 엔진 초기화 콜백
 *
 * @td: 잡의 thread_data
 * @return: 0 = 성공, 1 = 실패(program 미지정)
 *
 * 왜 필요한가: 엔진별 옵션 검증과 잡 파라미터 조정을 수행한다. fio 코어는 init이
 * 끝날 때까지 잡을 실제로 시작하지 않으므로, 이 함수에서 thinktime/nr_files 등
 * exec 엔진 동작 모델에 맞는 값을 설정한다.
 *
 * 동작:
 *   1) eo->pid = -1(센티넬)로 초기화 — "아직 fork 전".
 *   2) program 옵션 미설정이면 에러 로그 + 1 반환 → fio가 잡을 실패 처리.
 *   3) 현재 runstate 저장 후 TD_SETTING_UP으로 전환(잡 시작 지연, qsort 캘리브레이션 등
 *      준비 중임을 fio 코어에 알림).
 *   4) thinktime_blocks=1, type=COMPLETE, thinktime_spin=0, thinktime=50_000us(50ms)
 *      — queue 루프가 50ms 주기로 감시하도록 설정.
 *   5) nr_files=open_files=1 — 실제 파일은 없지만 fio 회계 요구 충족용 더미 1.
 *   6) runstate 복원 후 0 반환.
 *
 * 실행 컨텍스트: 잡 스레드의 td_io_init 단계. 잡 실행 루프 이전에 한 번만 호출.
 * 호출 체인: ioengines.c::td_io_init() → [이 함수]
 */
static int fio_exec_init(struct thread_data *td)
{
	struct thread_options *o = &td->o;     /* [한국어] 잡 옵션 포인터 */
	struct exec_options *eo = td->eo;      /* [한국어] 엔진 옵션 포인터 */
	int td_previous_state;                 /* [한국어] 변경 전 runstate 저장 (복원용) */

	eo->pid = -1;                          /* [한국어] 센티넬: "아직 자식 프로세스 없음" */

	/* [한국어] program 옵션 필수 검증. 미설정 시 fio vmsg(로그 + verror) 호출 후 1 반환. */
	if (!eo->program) {
		td_vmsg(td, EINVAL,
			"no program is defined, it is mandatory to define one",
			"exec");
		return 1;                      /* [한국어] 비 0 → fio 코어가 잡을 실패로 처리 */
	}

	/* [한국어] 초기화 정보 로깅 (디버깅 및 확인용). */
	log_info("%s : program=%s, arguments=%s\n",
		 td->o.name, eo->program, eo->arguments);

	/* Saving the current thread state */
	/* [한국어] 현재 runstate 백업. 아래에서 TD_SETTING_UP으로 바꿨다가 복원한다. */
	td_previous_state = td->runstate;

	/*
	 * Reporting that we are preparing the engine
	 * This is useful as the qsort() calibration takes time
	 * This prevents the job from starting before init is completed
	 */
	/* [한국어] TD_SETTING_UP으로 전환 — fio 코어/프론트엔드가 "초기화 중"으로 인식.
	 * qsort 캘리브레이션이나 다른 초기화가 오래 걸릴 때 잡 시작 타이밍 밀림을 방지. */
	td_set_runstate(td, TD_SETTING_UP);

	/*
	 * set thinktime_sleep and thinktime_spin appropriately
	 */
	/* [한국어] thinktime 회계: 1 블록마다 thinktime 적용, 완료 기준(COMPLETE)으로 계산. */
	o->thinktime_blocks = 1;
	o->thinktime_blocks_type = THINKTIME_BLOCKS_TYPE_COMPLETE;
	/* [한국어] busy-wait 비활성 — 순수 sleep 기반 대기로 CPU 낭비 억제. */
	o->thinktime_spin = 0;
	/* 50ms pause when waiting for the program to complete */
	/* [한국어] 폴링 주기 50ms. 너무 짧으면 CPU 낭비, 너무 길면 timeout 정밀도 저하. */
	o->thinktime = 50000;

	/* [한국어] exec 엔진은 실제 파일이 없지만 fio 회계가 1파일을 가정하도록 설정.
	 * FIO_DISKLESSIO 플래그와 조합되어 실제 파일 오픈은 수행되지 않는다. */
	o->nr_files = o->open_files = 1;

	/* Let's restore the previous state. */
	/* [한국어] runstate 복원 — 초기화 완료 후 정상 실행 상태로 되돌림. */
	td_set_runstate(td, td_previous_state);
	return 0;                              /* [한국어] 성공 */
}

/*
 * [한국어]
 * fio_exec_cleanup - exec 엔진 정리 콜백
 *
 * @td: 잡의 thread_data
 *
 * 왜 필요한가: 잡이 정상/비정상 종료되는 경로 모두에서 "살아있을 수 있는 자식 프로세스"를
 * 확실히 종료하기 위함. queue 루프가 SIGTERM만 보냈거나, 자식이 트랩하지 않고 무시
 * 한 경우에도 여기서 SIGKILL로 확정 종료한다.
 *
 * 동작: eo->pid > 0 이면 kill(pid, SIGKILL). SIGKILL은 커널이 직접 프로세스를 제거하며
 * 자식이 트랩할 수 없다(핸들러/블록 불가).
 *
 * 실행 컨텍스트: 잡 스레드 종료 경로. td_io_cleanup에서 호출.
 * 호출 체인: ioengines.c::td_io_cleanup() → [이 함수] → kill(2)
 *
 * 주의: 본 함수는 waitpid를 호출하지 않아 좀비가 잠시 남을 수 있다. fio 프로세스가
 * 곧 종료되면 init에 의해 수거된다.
 */
static void fio_exec_cleanup(struct thread_data *td)
{
	struct exec_options *eo = td->eo;      /* [한국어] 엔진 상태 (pid) 접근 */
	/* Send a sigkill to ensure the job is well terminated */
	/* [한국어] pid 유효성(>0) 확인 후 SIGKILL 전달. -1이면 아직 fork조차 안 됐거나 실패. */
	if (eo->pid > 0)
		kill(eo->pid, SIGKILL);
}

/*
 * [한국어]
 * fio_exec_open - open_file 콜백 (no-op)
 *
 * @td: (사용 안 함) 잡 thread_data
 * @f:  (사용 안 함) fio_file
 * @return: 항상 0 (성공)
 *
 * 왜 필요한가: ioengine_ops.open_file은 엔진이 자체 파일 오픈을 수행할 때 사용하나,
 * exec 엔진은 실제 I/O가 없으므로 단순히 0을 반환하여 fio 코어의 파일 개방 절차를
 * 충족시킨다. FIO_DISKLESSIO 플래그와 함께 fio 코어의 기본 open 경로가 건너뛰어지므로
 * 본 함수는 호출되어도 아무 것도 하지 않는다.
 *
 * 실행 컨텍스트: 잡 스레드의 파일 오픈 단계.
 */
static int
fio_exec_open(struct thread_data fio_unused * td,
	      struct fio_file fio_unused * f)
{
	return 0;                              /* [한국어] no-op — 항상 성공 */
}

/* [한국어] fio I/O 엔진 플러그인 계약체(ioengine_ops). fio 코어가 엔진을 로드할 때
 * 이 구조체 포인터를 통해 콜백을 해석한다. 필드별 의미는 ioengine.h 참조.
 * - FIO_SYNCIO:    동기 엔진(별도 getevents 불필요, queue가 즉시 완료 처리)
 * - FIO_DISKLESSIO: 실제 파일/디바이스가 필요 없음
 * - FIO_NOIO:      실제 데이터 전송 없음 — 통계 일부 필드에서 제외 */
static struct ioengine_ops ioengine = {
	.name               = "exec",                            /* [한국어] 사용자 --ioengine=exec로 선택할 이름 */
	.version            = FIO_IOOPS_VERSION,                 /* [한국어] ABI 버전(코어와의 호환 체크) */
	.queue              = fio_exec_queue,                    /* [한국어] I/O 제출 콜백 (본 엔진에서는 실행/감시) */
	.init               = fio_exec_init,                     /* [한국어] 초기화 콜백 */
	.cleanup            = fio_exec_cleanup,                  /* [한국어] 정리 콜백 (자식 SIGKILL) */
	.open_file          = fio_exec_open,                     /* [한국어] 파일 오픈 콜백 (no-op) */
	.flags              = FIO_SYNCIO | FIO_DISKLESSIO | FIO_NOIO, /* [한국어] 엔진 특성 비트 플래그 */
	.options            = options,                           /* [한국어] 옵션 테이블 포인터 */
	.option_struct_size = sizeof(struct exec_options),       /* [한국어] 옵션 구조체 크기 (fio가 할당 시 사용) */
};

/*
 * [한국어]
 * fio_exec_register - 로드 타임에 exec 엔진을 fio 코어에 등록
 *
 * fio_init 속성으로 선언되어 프로그램/공유라이브러리 로드 시 자동 호출되는 생성자.
 * register_ioengine()가 ioengine_list에 이 엔진을 추가하여 이후 --ioengine=exec로
 * 선택 가능하게 된다. 호출 컨텍스트는 main() 진입 전의 C 런타임 생성자 단계.
 */
static void fio_init fio_exec_register(void)
{
	register_ioengine(&ioengine);          /* [한국어] 엔진 리스트에 삽입 */
}

/*
 * [한국어]
 * fio_exec_unregister - 프로세스 종료 시 엔진 등록 해제
 *
 * fio_exit 속성으로 선언되어 exit 핸들러로 자동 호출되는 소멸자.
 * unregister_ioengine()가 리스트에서 엔트리를 제거. 메모리 릭 방지 및 공유 라이브러리
 * 언로드 안전성 확보.
 */
static void fio_exit fio_exec_unregister(void)
{
	unregister_ioengine(&ioengine);        /* [한국어] 엔진 리스트에서 제거 */
}
