/*
 * This file contains job initialization and setup functions.
 */
/*
 * [한국어 설명] fio 잡 초기화 및 설정 (init.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 **잡(job) 초기화와 옵션 파싱**을 총괄한다. main()이
 * 호출하는 parse_options()를 최상위 진입점으로 하여, 아래 네 가지
 * 대주제를 한 파일에서 처리한다:
 *   (1) 명령줄 인자 파싱 (--name/--rw/--ioengine/--output-format/
 *       --section/--server/--client 등) — getopt_long_only(3) + FIO_OPT_*
 *       플래그 + l_opts[] 테이블을 조합하여 long/short 옵션을 단일 스위치로
 *       처리한다.
 *   (2) 잡 파일(.fio) INI 파서 — `[global]` 과 `[jobname]` 섹션 헤더를
 *       sscanf("[%255[^\\n]]") 로 인식하고, 섹션 몸체의 "key=value" 줄을
 *       수집해 fio_options_parse() 에 넘겨 thread_options 필드에 기입한다.
 *       `include filename` 지시어를 재귀적으로 __parse_jobs_ini()로 처리한다.
 *   (3) thread_data 공유 메모리 할당 체인 — JOBS_PER_SEG 단위로 shmget(2)/
 *       shmat(2) 으로 POSIX 공유 메모리 세그먼트(struct thread_segment)를
 *       만들고, get_new_job()이 부모 td 를 얕은 복사한 뒤 dup_files/
 *       fio_options_mem_dupe/profile_add_hooks 로 깊은 복사 보강을 수행한다.
 *       공유 메모리를 쓰는 이유는 --thread=0 기본 모드에서 job 이 fork(2)
 *       로 분리된 프로세스로 실행되어 부모-자식 간 통계 공유가 필요하기
 *       때문이다.
 *   (4) 옵션 의존성 해결(fixup_options) 및 최종 잡 등록(add_job) —
 *       readonly/trimwrite/zone_mode/verify_interval/rate vs rate_iops/
 *       압축 vs refill_buffers/thinktime_spin/iodepth_low·batch/sprandom
 *       전제조건 등 200+ 검증 지점을 일괄 처리하고, init_flags()로
 *       TD_F_VER_BACKLOG/TRIM_BACKLOG/READ_IOLOG/REFILL_BUFFERS/
 *       SCRAMBLE_BUFFERS/DO_VERIFY/NEED_LOCK/CHECK_RATE 비트를 세팅한 뒤
 *       setup_log() 으로 lat/slat/clat/bw/iops/hist 로그 스트림을 연다.
 *
 * 또한 부수적으로 signal 관련 처리(atexit(free_shm))와 dlopen 된 I/O 엔진
 * 해제(free_ioengine), 난수 시드 파생(td_fill_rand_seeds/init_rand_seed),
 * blktrace 병합 훅(merge_blktrace_iologs), steady-state 초기화
 * (td_steadystate_init), flow control 초기화(flow_init_job) 등
 * 런타임 직전 모든 pre-flight 훅을 여기에서 호출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   _start → __libc_start_main → main() [fio.c]
 *     → initialize_fio() [fio.c]
 *     → parse_options(argc, argv) [이 파일]
 *         → fio_init_options() [이 파일]
 *             → fio_options_fill_optstring() [이 파일]
 *             → fio_options_dup_and_init(l_opts) [options.c]
 *             → atexit(free_shm) [glibc: exit 시 역순 호출]
 *             → fill_def_thread() [이 파일]
 *                 → fio_fill_default_options() [options.c]
 *         → fio_test_cconv(&def_thread.o) [cconv.c]
 *         → parse_cmd_line(argc, argv, FIO_CLIENT_TYPE_CLI) [이 파일]
 *             → getopt_long_only(3) 루프
 *                 → add_job() / get_new_job() / ioengine_load() / ...
 *             → (서버 모드) fio_start_server() [server.c]
 *             → (클라이언트 모드) fio_client_add() [client.c]
 *         → (잡 파일 있으면) parse_jobs_ini(ini_file[i], ...) [이 파일]
 *             → __parse_jobs_ini() [이 파일 재귀]
 *                 → get_new_job() → ioengine_load()
 *                 → fio_options_parse() [parse.c]
 *                 → add_job() [이 파일]
 *                     → init_flags() / ioengine_load()
 *                     → setup_random_seeds() / fixup_options()
 *                     → flow_init_job() / setup_log() × 5
 *                     → td_steadystate_init() / setup_rate()
 *                     → numjobs>1 이면 재귀적으로 add_job()
 *     → (이후 main 에서) fio_backend() 또는 fio_handle_clients() [backend.c]
 *
 * 실행 컨텍스트:
 *   - 이 파일의 모든 함수는 **메인 프로세스의 단일 스레드**에서 실행된다.
 *     잡이 spawn 되기 이전 단계이므로 thread_data 들 간 동기화가 필요
 *     없다. 유일한 예외는 fio_debug_jobp/fio_warned 가 공유 메모리에
 *     위치해 이후 자식 프로세스에서도 참조된다는 점이다.
 *   - 서버 모드(-S) 에서는 parse_cmd_line 이 fio_start_server() 로
 *     분기해 클라이언트 요청 루프에 진입하고, 이후 클라이언트 요청마다
 *     parse_options 가 다시 호출될 수 있다(optind=1 초기화 참조).
 *
 * === 타 모듈과의 연결 ===
 * - fio.c: main()에서 parse_options() 호출. did_arg/is_backend/nr_clients
 *   등 전역을 공유해 이후 분기를 결정.
 * - parse.c: fio_options_parse(), fill_default_options(), check_str_time()
 *   등 옵션 엔진을 제공. 본 파일은 이를 호출만 하고 자체 파싱 로직은
 *   INI 섹션 감지 수준에 그친다.
 * - options.c: fio_options[] 테이블의 엔트리 정의와 fio_check_options/
 *   fio_options_dup_and_init/fio_option_is_set/fio_options_mem_dupe/
 *   fio_fill_default_options/fio_options_set_ioengine_opts/
 *   fio_cmd_option_parse/fio_show_option_help 를 공급.
 * - ioengines.c: load_ioengine/free_ioengine/td_set_ioengine_flags/
 *   td_ioengine_flagged — IO 엔진 dlopen(3) 기반 로드/해제/플래그 확인.
 * - smalloc.c: sinit()/scleanup() — 공유 메모리 풀 초기화/정리.
 * - filesetup.c: add_file/dup_files/for_each_file/fio_file_free —
 *   잡 당 파일 목록 관리.
 * - filehash.c: file_hash_exit — 중복 파일 감지 해시 테이블 해제.
 * - verify.c: verify_header 구조체 크기 참조(verify_offset 검증).
 * - iolog.c: setup_log/iolog_file_inflate — 로그 스트림 개방, gz 해제.
 * - stat.c: init_thread_stat_min_vals — 통계 최소값 초기화.
 * - steadystate.c: td_steadystate_init — 정상상태 감지 훅.
 * - flow.c: flow_init/flow_exit/flow_init_job/flow_exit_job — job 간
 *   IO rate 동기화용 flow control.
 * - filelock.c: fio_filelock_exit — 파일 잠금 해제.
 * - profile.c: profile_add_hooks/profile_td_init/profile_td_exit —
 *   프로파일 훅 연결.
 * - blktrace.c: merge_blktrace_iologs — blktrace 병합.
 * - server.c/client.c: fio_server_*, fio_client_* — 백엔드/클라이언트
 *   모드 처리, fio_clients_send_ini 로 원격 파싱 위임.
 *
 * 데이터 흐름:
 *   argv[] / .fio 파일 → [본 파일 파서] → thread_options 필드
 *     → fixup_options 로 필드 간 정합 보정 → thread_data.flags 비트
 *     → 공유 메모리 세그먼트에 기록 → backend.c 가 fork/pthread_create
 *     로 잡 spawn → 잡 스레드가 읽기 전용으로 thread_options 참조.
 *
 * 공유하는 핵심 자료구조:
 *   - struct thread_data — 잡 하나당 1개, 공유 메모리 segments[] 에 배치.
 *   - struct thread_options — td->o 에 임베딩. 옵션 파서의 최종 산출물.
 *   - struct ioengine_ops — td->io_ops 에 포인터로 연결, dlopen 핸들을
 *     ops->dlhandle 로 간접 소유.
 *   - segments[REAL_MAX_SEG] — thread_segment 배열, nr_segments/cur_segment
 *     로 인덱싱.
 *   - def_thread — "[global]" 섹션을 받는 템플릿 thread_data.
 *   - l_opts[FIO_NR_OPTIONS] — getopt_long_only(3) 의 struct option 테이블.
 *   - fio_debug / fio_debug_jobp / fio_warned — 공유 메모리의 디버그 플래그.
 *
 * === 주요 함수/구조체 요약 ===
 * - parse_options(argc,argv): 최상위 진입점. fio_init_options → parse_cmd_line
 *   → (ini 파일들에 대해) parse_jobs_ini → 정리.
 * - parse_cmd_line(): getopt_long_only(3) 루프. 약 50개 옵션을 단일 switch
 *   로 디스패치. --name 을 만나면 get_new_job → ioengine_load → ... →
 *   add_job 을 내부적으로 수행.
 * - parse_jobs_ini() / __parse_jobs_ini(): INI 파서. 섹션 헤더 [...] 를
 *   sscanf 로 인식하고 include 지시자를 재귀 처리. 섹션 몸체 옵션을
 *   문자열 배열에 모아 fio_options_parse() 로 일괄 파싱.
 * - get_new_job(): 공유 메모리에서 td 한 칸을 확보하고 부모 td 복사,
 *   opt_list/fs_list 깊은 복사, fio_options_mem_dupe 수행.
 * - add_job(): 최종 잡 등록. init_flags, ioengine_load, add_file,
 *   setup_random_seeds, fixup_options, flow_init_job, setup_log × 5,
 *   init_thread_stat_min_vals, td_steadystate_init, setup_rate, 그리고
 *   numjobs 재귀 확장을 모두 수행.
 * - fixup_options(): 200+ 옵션 간 의존성 해결. 가장 복잡한 정적 검증기.
 * - ioengine_load(): dlopen(3) 으로 .so 로드 또는 내장 엔진 참조,
 *   td->io_ops 와 td->eo 를 초기화.
 * - setup_random_seeds() / td_fill_rand_seeds() / init_rand_offset_seed():
 *   FIO_RAND_NR_OFFS 개의 시드 슬롯을 파생하고 frand_state 들을 초기화.
 * - free_shm() / free_threads_shm() / add_thread_segment() /
 *   expand_thread_area(): POSIX shmget/shmat/shmdt/shmctl 기반 세그먼트
 *   수명 관리.
 * - set_debug(): --debug 비트마스크 해석 (debug_levels[] 테이블 사용).
 * - struct fpre_keyword[]: $jobname/$jobnum/$filenum/$clientuid 치환
 *   키워드 테이블. make_filename() 이 사용.
 * - struct option l_opts[]: getopt_long_only(3) 의 long option 테이블.
 *   FIO_CLIENT_FLAG 비트를 val 상위에 OR 하여 클라이언트 전달 여부 표시.
 *
 * === 왜 이 파일이 fio 전체에서 가장 복잡한가 ===
 * fio 는 "하나의 바이너리가 수백 가지 옵션을 받아 I/O 워크로드를 표현"
 * 하는 설계라서, 옵션 간 조합은 조합 폭발이 발생한다. fixup_options()
 * 한 함수만 보면 zone_mode / sprandom / verify / compress / rate /
 * iodepth / dedupe / ss / fdp 등 서로 독립적인 기능 도메인이 모두 여기서
 * 교차 검증된다. 이 파일의 얕은 주석을 보완하는 이유는, 다른 모듈로
 * 점프하지 않고도 옵션 파이프라인 전체를 이해할 수 있게 하기 위함이다.
 */

/* ============================================================
 * [한국어] 헤더 포함 사유 카탈로그
 * 아래 각 #include 는 본 파일에서 어떤 심볼/매크로를 공급하는지,
 * 그리고 왜 없으면 컴파일이나 런타임이 무너지는지를 개별로 명시한다.
 * ============================================================ */

#include <stdio.h>
/* [한국어] FILE 포인터 타입, fopen/fclose/fgets/fscanf/printf/snprintf/
 * sprintf/stdin/stdout/stderr 공급. 본 파일은 INI 파일을 fopen("r") 로
 * 읽고, usage() 가 printf 로 도움말을 출력하며, --output 옵션이
 * stdout/stderr 을 파일로 리다이렉트한다. strsep 기반 버퍼 파싱
 * 시에도 snprintf 로 파일명을 조립한다. */

#include <stdlib.h>
/* [한국어] malloc/calloc/realloc/free/atoi/strdup(일부 libc) 및 exit(3)
 * 공급. 옵션 문자열 복제(strdup), 옵션 배열 동적 확장(realloc), 전역
 * 트리거 버퍼 할당, 그리고 parse_cmd_line() 말미의 exit(exit_val) 용도. */

#include <unistd.h>
/* [한국어] getpid(2)/access(2) 공급. fill_def_thread() 가 현재
 * 프로세스의 CPU 친화도를 getpid() 로 조회하고, --client 가 파일
 * 경로인지 access(F_OK) 로 판별하며, include 지시자의 상대 경로
 * 존재 확인도 access 로 수행한다. */

#include <ctype.h>
/* [한국어] isspace(3)/iscntrl(3) 공급. is_empty_or_comment() 가 줄의
 * 전 문자가 공백/제어문자인지 확인해 INI 빈 줄을 판별한다. */

#include <string.h>
/* [한국어] strcmp/strncmp/strcpy/strdup/strlen/strchr/strrchr/strstr/
 * strsep/memset/memcpy/strerror(3) 공급. 섹션 이름 비교, include
 * 지시자 접두 확인, 키워드 치환(make_filename), 옵션 수집, thread_data
 * 얕은 복사, 에러 메시지 생성에 전반 사용. */

#include <errno.h>
/* [한국어] errno 변수와 EINVAL/ENOMEM/ENOSPC 매크로 공급. shmget(2) 실패
 * 시 errno 를 확인해 EINVAL/ENOMEM/ENOSPC 면 사용자 자원 한계로 간주해
 * 조용히 실패하고, 그 외에는 perror 로 진단. INI fopen 실패 역시
 * errno 를 td_verror 로 기록. */

#include <sys/ipc.h>
/* [한국어] System V IPC 의 IPC_CREAT/IPC_RMID 매크로와 key_t 타입 공급.
 * shmget/shmctl 의 첫 인자가 key_t 이고, 세그먼트 생성 플래그
 * (IPC_CREAT|0600)과 삭제 플래그(IPC_RMID)가 여기서 온다. */

#include <sys/types.h>
/* [한국어] pid_t/size_t/off_t 등 POSIX 기본 타입 정의 공급. getpid 의
 * 반환 타입, shm_id 관련 타입 호환성, 파일 읽기 버퍼 크기 계산 등에
 * 암묵적으로 의존. 일부 시스템에서는 sys/shm.h 보다 먼저 포함해야
 * 한다. */

#include <dlfcn.h>
/* [한국어] dlopen(3)/dlclose(3)/dlsym(3)/dlerror(3) 공급. ioengine_load()
 * 경로에서 본 파일은 dlclose 만 직접 사용하지만(엔진 교체 시 이전
 * 핸들 정리), load_ioengine()/free_ioengine() 쪽에서 dlopen/dlsym 을
 * 공유하므로 선언이 필요하다. .so 엔진 플러그인 로드의 핵심. */

#ifdef CONFIG_VALGRIND_DEV
#include <valgrind/drd.h>
/* [한국어] Valgrind DRD(Data Race Detector) 클라이언트 요청 매크로 공급.
 * DRD_IGNORE_VAR(x) 는 해당 변수를 DRD 의 경쟁 감지 대상에서 제외한다.
 * 공유 메모리의 thread_data 는 정상 경로상 부모가 초기화한 뒤 자식이
 * 읽기만 하지만, DRD 는 이를 경쟁으로 오탐할 수 있어 명시적으로
 * suppress 해야 한다. configure 단계에서 valgrind-dev 가 발견되면
 * CONFIG_VALGRIND_DEV 가 켜진다. */
#else
/* [한국어] Valgrind 가 없으면 매크로를 no-op 로 정의해 호출부를 그대로
 * 유지한다. do{}while(0) 은 매크로 치환 시 세미콜론 문제를 피하는
 * 관용 패턴. */
#define DRD_IGNORE_VAR(x) do { } while (0)
#endif

#include "fio.h"
/* [한국어] fio 의 중앙 헤더. struct thread_data, thread_options,
 * fio_file, ioengine_ops, enum fio_ddir, TD_F_* 플래그, FIO_*
 * 매크로 대부분, log_info/log_err, td_verror, dprint(FD_*), for_each_td,
 * for_each_file, td_read/td_write/td_trim/td_random/td_trimwrite,
 * td_ioengine_flagged, fio_option_is_set, add_file, dup_files,
 * frand_state 관련 API, flow_init/init_rand_seed 등 거의 모든 fio
 * 코어 심볼을 공급한다. */

#ifndef FIO_NO_HAVE_SHM_H
#include <sys/shm.h>
/* [한국어] System V 공유 메모리 API 공급: shmget(2)/shmat(2)/shmdt(2)/
 * shmctl(2), struct shmid_ds, SHM_RDONLY 등. thread_segment 1개 =
 * 하나의 shm 세그먼트. FIO_NO_HAVE_SHM_H 는 Windows/Android 등 SHM
 * 미지원 플랫폼 대응이며, 그 경우 CONFIG_NO_SHM 분기로 malloc 폴백. */
#endif

#include "parse.h"
/* [한국어] fio 옵션 파서 엔진. fio_options_parse(), fio_cmd_option_parse(),
 * fio_cmd_ioengine_option_parse(), fio_options_dup_and_init(),
 * fio_options_set_ioengine_opts(), fio_fill_default_options(),
 * strip_blank_front(), strip_blank_end(), check_str_time(), options_init(),
 * options_mem_dupe(), FIO_GETOPT_JOB, FIO_GETOPT_IOENGINE 등 공급. */

#include "smalloc.h"
/* [한국어] fio 전용 공유 메모리 풀 할당기. sinit()/scleanup()/
 * smalloc_pool_size 전역 공급. --alloc-size 옵션이 smalloc_pool_size 를
 * 조정해 sinit() 으로 풀을 재초기화한다. */

#include "filehash.h"
/* [한국어] 파일 해시 테이블 API. file_hash_exit() 공급. fio 는 같은
 * 파일을 여러 잡이 공유할 때 fio_file 객체를 재사용하도록 해시에
 * 등록하며, 본 파일은 종료 시 이를 해제한다. */

#include "verify.h"
/* [한국어] struct verify_header 정의 공급. fixup_options() 가
 * verify_offset + sizeof(struct verify_header) 가 verify_interval 을
 * 초과하지 않는지 검증한다. */

#include "profile.h"
/* [한국어] 실행 프로파일 API. profile_add_hooks/profile_td_init/
 * profile_td_exit 공급. --profile 옵션으로 사전 정의된 워크로드 템플릿
 * (tiobench, act 등)을 덮어쓸 수 있다. */

#include "server.h"
/* [한국어] 클라이언트/서버 분산 모드 API. fio_server_send_add_job,
 * fio_start_server, fio_server_set_arg, fio_server_internal_set,
 * fio_client_add, fio_client_add_ini_file, fio_client_add_cmd_option,
 * fio_clients_connect, fio_clients_send_ini, fio_client_ops,
 * client_sockaddr_str, nr_clients 공급. */

#include "idletime.h"
/* [한국어] CPU 유휴시간 측정 API. fio_idle_prof_parse_opt 공급. --idle-prof
 * 옵션이 인자("system"/"percpu"/"calibrate")를 여기서 해석. */

#include "filelock.h"
/* [한국어] 파일 잠금 API. fio_filelock_exit() 공급. 잡이 verify_state
 * 파일 등을 락으로 보호할 때 사용하며 본 파일은 종료 시 해제만. */

#include "steadystate.h"
/* [한국어] 정상상태(steady state) 감지 API. td_steadystate_init 공급.
 * 잡이 일정 윈도우 동안 수렴(iops/bw 분산 작음)하면 조기 종료. */

#include "blktrace.h"
/* [한국어] blktrace 재생/병합 API. merge_blktrace_iologs 공급. --read_iolog
 * 과 --merge-blktrace-only 분기에서 사용. */

#include "oslib/asprintf.h"
/* [한국어] asprintf(3) 의 OS 독립 구현. include 지시자의 상대 경로를
 * 현재 파일 디렉토리와 결합할 때 full_fn 을 동적 할당한다. */

#include "oslib/getopt.h"
/* [한국어] getopt_long_only(3) 와 struct option 의 OS 독립 구현.
 * parse_cmd_line 의 핵심 파서. required_argument/optional_argument/
 * no_argument 매크로도 여기서 온다. */

#include "oslib/strcasestr.h"
/* [한국어] strcasestr(3) 의 OS 독립 구현. make_filename() 이 $jobname
 * 등 키워드를 대소문자 무시로 탐색할 때 사용. */

#include "crc/test.h"
/* [한국어] CRC/체크섬 벤치마크 함수 fio_crctest 공급. --crctest
 * 옵션에서 호출. */

#include "lib/pow2.h"
/* [한국어] is_power_of_2(n) 판별 함수 공급. add_job() 이 kb_base (1000 vs
 * 1024) 를 파워-오브-2 여부로 구분해 num2str 표시 단위 결정. */

#include "lib/memcpy.h"
/* [한국어] fio_memcpy_test() 벤치마크 공급. --memcpytest 옵션에서 호출. */

const char fio_version_string[] = FIO_VERSION;
/* [한국어] fio 버전 문자열.
 * 설정자: 컴파일 시 FIO_VERSION 매크로(예: "fio-3.42")가 치환되어 결정된다.
 * 읽는 자: fio.c main() 의 version 출력, usage() 의 헤더, --version 처리,
 *   그리고 parse_options() 말미의 일반 출력 헤더에서 참조.
 * 값 범위: null-terminated C 문자열. 링커 타임에 고정되며 읽기 전용.
 * 동기화: const 이고 초기화 후 변경되지 않으므로 동기화 불필요. */

#define FIO_RANDSEED		(0xb1899bedUL)
/* [한국어] 난수 생성기의 역사적 기본 시드.
 * 현재 이 매크로는 본 파일 내에서 직접 참조되지 않지만, 과거 rand_seed
 * 옵션의 디폴트로 쓰였고 하위 호환성과 문서화를 위해 유지된다. 새로
 * 추가되는 난수 코드는 td->rand_seeds[] 배열에서 시드를 뽑아야 한다. */

static char **ini_file;
/* [한국어] 명령줄에서 수집된 잡 파일(.fio) 경로 배열.
 * 설정자: parse_cmd_line() 의 말미 루프에서 남은 positional 인자를
 *   strdup 하여 realloc 확장된 포인터 배열에 축적.
 * 읽는 자: parse_options() 가 각 파일마다 parse_jobs_ini(ini_file[i], ...)
 *   호출 뒤 free(ini_file[i]) 하고 최종적으로 free(ini_file).
 * 값 범위: NULL(초기) 또는 realloc 된 char* 배열 포인터. 각 엔트리는
 *   strdup 결과라 소유권 이관된 힙 포인터.
 * 동기화: 메인 스레드 단독 소유라 락 불필요. */

static bool dump_cmdline;
/* [한국어] --showcmd 옵션 플래그.
 * 설정자: parse_cmd_line() 의 case 's'.
 * 읽는 자: parse_dryrun(), __parse_jobs_ini() 내 dump 분기, add_job() 의
 *   dryrun 체크.
 * 값 범위: false(기본) / true. 일단 true 가 되면 실제 I/O 는 수행되지 않고
 *   잡 정의를 명령줄 형태로만 출력한다.
 * 동기화: 메인 스레드에서만 변경. */

static bool parse_only;
/* [한국어] --parse-only 옵션 플래그. 옵션 구문 검증만 수행하고 I/O 는
 * 실행하지 않는다. 설정자: case 'P'. 읽는 자: parse_dryrun(). */

static bool merge_blktrace_only;
/* [한국어] --merge-blktrace-only 옵션 플래그. blktrace 로그를 병합만 하고
 * 잡을 실행하지 않는다. 설정자: case 'A'. 읽는 자: add_job() 이 병합 완료
 * 후 put_job 으로 잡을 버리는 조건. */

static struct thread_data def_thread;
/* [한국어] 기본(글로벌) 스레드 데이터 = "[global]" 섹션의 값을 담는
 * 템플릿. 실제 잡으로는 실행되지 않고, get_new_job() 이 이 구조체를
 * 부모로 삼아 자식 td 를 복사 생성한다.
 * 설정자: fill_def_thread() 가 fio_getaffinity + fio_fill_default_options
 *   로 초기화. 이후 --name=global 이나 [global] 섹션에서 fio_options_parse
 *   가 필드를 채워넣는다.
 * 읽는 자: get_new_job(), ioengine_load(origeo 분기), add_job, parse_options
 *   말미의 options_free.
 * 값 범위: 정적 0-초기화 후 runtime 에 채워짐.
 * 동기화: 메인 스레드 단독 접근. */

struct thread_segment segments[REAL_MAX_SEG];
/* [한국어] thread_data 들을 담는 공유 메모리 세그먼트 배열.
 * 설정자: add_thread_segment() 가 shmget+shmat 으로 한 세그먼트당
 *   JOBS_PER_SEG(보통 16) 개 td 와 2개의 디버그 포인터를 담는 chunk 를 할당.
 * 읽는 자: get_new_job / for_each_td / free_threads_shm / add_job /
 *   backend.c 의 잡 스폰 루프 (자식 프로세스도 attach).
 * 값 범위: segments[0..nr_segments-1] 만 유효. REAL_MAX_SEG 는 최대 세그먼트 수.
 * 동기화: 메인 프로세스가 초기화하고, 자식은 fork(2) 이후 read-mostly.
 *   thread_data 필드별로는 원자적 업데이트(잡 상태, 통계 등)를 가정. */

static char **job_sections;
/* [한국어] --section 옵션으로 지정된 섹션 이름 문자열 배열.
 * 설정자: parse_cmd_line() 의 case 'x' 가 realloc + strdup 로 append.
 * 읽는 자: skip_this_section() 이 섹션 헤더 파싱 시 검사.
 * 값 범위: NULL(기본 - 모든 섹션 실행) 또는 strdup 한 문자열 배열.
 * 소유권: 각 엔트리는 free(job_sections[i]) 로 회수. __parse_jobs_ini 의
 *   말미 정리 루프에서 해제되고 nr_job_sections 도 0 으로 리셋. */

static int nr_job_sections;
/* [한국어] job_sections 배열 원소 수. 0 이면 "모든 섹션 실행". */

bool exitall_on_terminate = false;
/* [한국어] 한 잡이라도 에러로 끝나면 모든 잡을 종료할지 여부.
 * fio.h 에 선언되어 있고 옵션 --exitall 에서 켤 수 있다. backend.c 의 잡
 * 종료 핸들러에서 참조. */

int output_format = FIO_OUTPUT_NORMAL;
/* [한국어] 출력 형식 비트마스크. FIO_OUTPUT_NORMAL/TERSE/JSON/JSON_PLUS
 * 를 OR 로 조합 가능. --output-format/--minimal/--append-terse 로 변경.
 * stat.c 의 show_run_stats, client.c 의 결과 출력 경로에서 분기. */

int eta_print = FIO_ETA_AUTO;
/* [한국어] ETA 표시 모드. FIO_ETA_AUTO(tty 이면 ON)/ALWAYS/NEVER.
 * --eta 옵션으로 제어되고 eta.c 의 disk_util_print 가 참조. */

unsigned int eta_interval_msec = 1000;
/* [한국어] ETA 갱신 간격(ms). --eta-interval 로 변경. 최소 DISK_UTIL_MSEC
 * 이상이어야 함(디스크 유틸리티 측정 간격과 정합). */

int eta_new_line = 0;
/* [한국어] ETA 출력에 줄바꿈을 강제할 주기(초). --eta-newline 으로 설정.
 * 0 은 줄바꿈 없음(carriage return 으로 in-place 갱신). */

FILE *f_out = NULL;
/* [한국어] 일반 출력 파일 포인터. 초기값 stdout(fio_init_options 에서
 * 설정). --output=FILE 이 지정되면 fopen("w+") 결과로 대체된다. */

FILE *f_err = NULL;
/* [한국어] 에러 출력 파일 포인터. 초기 stderr, --output 시 f_out 과 동일
 * 파일로 지정된다(로그 혼합 출력). */

char *exec_profile = NULL;
/* [한국어] --profile=NAME 으로 지정된 실행 프로파일 이름.
 * profiles/ 디렉토리의 .c 가 load_profile 로 등록된 이름과 일치해야 한다. */

int warnings_fatal = 0;
/* [한국어] --warnings-fatal: 경고를 치명적으로 처리할지 여부.
 * fixup_options 와 옵션 파서에서 `ret |= warnings_fatal` 패턴으로 사용되어,
 * 값이 1 이면 경고가 즉시 리턴 코드를 오염시킨다. */

int terse_version = 3;
/* [한국어] terse 출력의 스키마 버전(2..5). --terse-version. stat.c 의
 * terse 출력 경로에서 분기하여 호환 포맷을 고른다. */

bool is_backend = false;
/* [한국어] 현재 프로세스가 fio --server 로 기동된 백엔드인지 여부.
 * 설정자: parse_cmd_line case 'S'. 읽는 자: backend.c 및 server.c 곳곳. */

bool is_local_backend = false;
/* [한국어] 로컬 백엔드 모드(fio 클라이언트가 내부 스폰한 동일 호스트
 * 서버)인지 여부. client.c 에서 설정한다. */

int nr_clients = 0;
/* [한국어] 연결된 원격 클라이언트 수. --client 로 증가. 이후 main() 가
 * nr_clients>0 이면 fio_handle_clients 로 분기, 아니면 fio_backend. */

bool log_syslog = false;
/* [한국어] syslog 로 로그를 보내는 모드 여부. 서버 모드에서 활성화. */

bool write_bw_log = false;
/* [한국어] --bandwidth-log 활성 플래그. aggregate bw 로그 파일을 생성할지
 * 여부. write_bw_log_name 과 쌍으로 사용. */

const char *write_bw_log_name;
/* [한국어] 집계 bw 로그의 기본 이름. --bandwidth-log=NAME 으로 지정되거나
 * 기본값 "agg" 가 된다. */

bool read_only = false;
/* [한국어] --readonly: 전역 읽기 전용 모드. fixup_options() 와 fio_ro_check
 * 가 참조해 쓰기/트림을 거부한다. 안전장치로만 사용된다. */

int status_interval = 0;
/* [한국어] --status-interval: 상태 전체 덤프 주기(초). 0 이면 비활성. */

char *trigger_file = NULL;
/* [한국어] --trigger-file: 파일이 생성되면 로컬/원격 trigger 명령을 실행. */

long long trigger_timeout = 0;
/* [한국어] --trigger-timeout: 지정 시각(초) 후 trigger 를 자동 발사. */

char *trigger_cmd = NULL;
/* [한국어] --trigger: 로컬에서 실행할 trigger 명령. system(3) 으로 실행. */

char *trigger_remote_cmd = NULL;
/* [한국어] --trigger-remote: 원격 서버가 실행할 trigger 명령 문자열. */

char *aux_path = NULL;
/* [한국어] --aux-path: fio 가 생성하는 보조 파일(verify state, log 등)
 * 의 기본 디렉토리. */

static int prev_group_jobs;
/* [한국어] 직전 그룹에 속한 잡 수 누적기. stonewall 이나 new_group 이
 * 설정될 때 groupid 를 bump 할지 결정하는 데 사용. add_job() 이 증감. */

unsigned long fio_debug = 0;
/* [한국어] 디버그 카테고리 비트마스크. 각 비트는 FD_* enum 값에 대응
 * (fio.h). set_debug() 가 --debug=list 를 파싱하고 dprint(FD_X, ...) 매크로가
 * (fio_debug & (1 << FD_X)) 로 로그 게이팅. */

unsigned int fio_debug_jobno = -1;
/* [한국어] --debug=job:NR 로 지정된 특정 잡 번호에만 디버그 출력.
 * -1 은 필터링 없음(모든 잡). */

unsigned int *fio_debug_jobp = NULL;
/* [한국어] 공유 메모리에 배치된 fio_debug_jobno 의 미러.
 * 설정자: add_thread_segment() 가 첫 세그먼트 끝에 fio_debug_jobno 용
 *   슬롯을 잡아 지정.
 * 읽는 자: 자식 잡 프로세스도 attach 이후 이 주소를 참조해 필터링.
 * 동기화: 단일 쓰기(메인), 다중 읽기(자식)라 atomic 읽기 가정. */

unsigned int *fio_warned = NULL;
/* [한국어] 중복 경고 방지 비트마스크의 공유 메모리 포인터. 같은 경고가
 * 자식들 사이에서 한 번만 출력되도록 atomic bit set 으로 게이팅. */

static char cmd_optstr[256];
/* [한국어] getopt_long_only(3) 에 넘기는 짧은 옵션 문자열 버퍼.
 * fio_options_fill_optstring() 이 l_opts[] 의 val 필드와 has_arg 필드를
 * 보고 자동 생성. 예: "o:l:b::m..." */

static bool did_arg;
/* [한국어] 실질적 인자가 처리됐는지 여부. --help, --version 같이 exit 로
 * 빠지는 옵션에서도 true 가 되어, 나중에 잡이 0개일 때 에러로 안내하지
 * 않도록 한다. */

#define FIO_CLIENT_FLAG		(1 << 16)
/* [한국어] l_opts 의 val 필드 상위 비트에 OR 하여 "이 옵션은 원격 클라이언트
 * 에도 전달해야 한다"고 표시하는 마커. parse_cmd_line 이 수신 직후
 * parse_cmd_client 로 forward 하고, 하위 8비트만 실제 옵션 값으로 사용.
 * 16번째 비트를 고른 이유는 ASCII 옵션 문자(하위 8비트)와 겹치지 않는
 * 넉넉한 상위 공간을 확보하기 위함. */

/*
 * Command line options. These will contain the above, plus a few
 * extra that only pertain to fio itself and not jobs.
 */
/* [한국어] fio 자체(잡 옵션이 아닌) 명령줄 옵션의 struct option 배열.
 *
 * struct option 의 각 필드 의미 (getopt(3) 스펙):
 *   .name     - "--long-name" 의 long 문자열.
 *   .has_arg  - no_argument / required_argument / optional_argument.
 *               required_argument 는 "="과 인자 필수(예: --output=FILE).
 *               optional_argument 는 "=" 뒤에 인자가 있을 수도 있다.
 *   .flag     - NULL 로 두면 val 값이 그대로 반환된다(본 코드는 전부 NULL).
 *   .val      - 매칭되었을 때 getopt 가 반환하는 int 값. fio 는 이 값의
 *               상위 비트에 FIO_CLIENT_FLAG 를 OR 해 "클라이언트에 전달
 *               해야 하는 옵션"을 한 워드로 표현한다. 하위 8비트는 단일
 *               ASCII 문자(예: 'o') 로 parse_cmd_line 의 switch 에서
 *               디스패치된다.
 *
 * FIO_CLIENT_FLAG 가 켜진 옵션: 원격 fio 클라이언트에게도 전달해
 *   서버와 동일한 동작을 하게 한다. parse_cmd_line 의 초입에서
 *   parse_cmd_client(cur_client, argv[optind-1]) 로 forward 한 뒤
 *   플래그를 지우고 로컬에서도 해석한다.
 *
 * 배열 말미는 {.name = NULL} 로 종료한다 - getopt(3) 계약.
 */
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
 * [한국어]
 * free_threads_shm() - thread_data 를 담는 모든 공유 메모리 세그먼트를
 *                      해제한다.
 *
 * @return: 없음 (void).
 *
 * 왜 필요한가: add_thread_segment() 가 shmget(2)+shmat(2) 로 만든 System V
 *   IPC 세그먼트는 커널이 자동 회수하지 않으며, shmctl(IPC_RMID) 가
 *   호출되어야 "마지막 detach 시 삭제" 마크가 붙는다. 프로세스 종료
 *   시에도 남아있는 IPC 세그먼트는 `ipcs` 에 누적되므로 반드시 명시적
 *   해제가 필요하다.
 *
 * 동작 단계:
 *   1. 세그먼트 배열을 0..nr_segments 로 순회.
 *   2. CONFIG_NO_SHM 이 꺼진 경우: shmdt(ptr) 로 현 프로세스에서 detach
 *      → shmctl(shm_id, IPC_RMID, &sbuf) 로 커널 측 세그먼트 삭제.
 *   3. CONFIG_NO_SHM (Windows 등) 인 경우: 단순 malloc 대체이므로
 *      free() 만 호출.
 *   4. nr_segments 와 cur_segment 를 0 으로 리셋해 재사용 가능 상태 복원.
 *
 * 실행 컨텍스트: 메인 프로세스 종료 경로(free_shm → atexit) 또는 에러
 *   회복 경로. 자식 잡이 detach 하기 전에 부모가 RMID 만 찍어두면 자식
 *   종료 시 자동 회수되지만, fio 는 자식도 실행 중에 shmctl 결과를 공유
 *   하므로 메인이 최종적으로 한번 더 청소한다.
 *
 * 호출 체인: free_shm() → [이 함수] → shmdt(2)/shmctl(2).
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
 * [한국어]
 * free_shm() - fio 종료 시 atexit(3) 훅으로 호출되어 모든 프로세스 공유
 *              자원을 정리하는 marshalling 함수.
 *
 * @return: 없음.
 *
 * 왜 필요한가: fio 는 공유 메모리, smalloc 풀, 파일 잠금, 파일 해시 등
 *   여러 OS 자원을 선점하므로 한 곳에서 해제를 모아 둬야 leak 가
 *   없다. atexit(3) 에 등록되어 있어 exit(3) 경로뿐 아니라 main() 의
 *   정상 return 경로에서도 자동 실행된다.
 *
 * 정리 순서:
 *   1. flow_exit() - flow 카운터/lock 해제.
 *   2. fio_debug_jobp/fio_warned = NULL - 곧 해제될 공유 메모리 슬롯에
 *      매달린 포인터를 무효화.
 *   3. free_threads_shm() - segments[] 에 있는 thread_data 들을 detach/
 *      삭제.
 *   4. trigger_* 문자열 해제 후 NULL 리셋.
 *   5. options_free(fio_options, &def_thread.o) - def_thread.o 의 동적
 *      할당된 문자열 필드를 깊이 해제.
 *   6. fio_filelock_exit - 파일 잠금 테이블 정리.
 *   7. file_hash_exit - 중복 파일 해시 정리.
 *   8. scleanup - smalloc 풀 munmap/해제.
 *
 * 실행 컨텍스트: 프로세스 종료 직전, 단일 스레드. 다른 잡은 이미 join 된
 *   상태. FUZZING_BUILD 에서는 oss-fuzz 가 매 iteration 마다 free_shm 을
 *   호출하면 상태가 파괴되어 잘못된 crash 로 보고될 수 있어 완전 no-op.
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
 * [한국어]
 * add_thread_segment() - thread_data 를 담을 새 공유 메모리 세그먼트를
 *                        할당·초기화한다.
 *
 * @return: 성공 시 0. 세그먼트 수 상한 초과 시 -1, shmat 실패 시 1.
 *
 * 왜 필요한가: thread_data 구조체는 큰 편(수 KB)이고, 한 번에 수천 개
 *   잡을 허용해야 하므로 단일 shmget 으로 전체를 할당하면 공유 메모리
 *   상한(SHMMAX)을 초과한다. JOBS_PER_SEG(보통 16) 잡 단위의 청크를
 *   여러 개 만들어 필요할 때마다 추가한다.
 *
 * 동작 단계:
 *   1. nr_segments 상한(REAL_MAX_SEG) 검사.
 *   2. JOBS_PER_SEG * sizeof(thread_data) + 2 * sizeof(uint) 크기로 shmget
 *      (뒤의 2 uint 는 fio_debug_jobp/fio_warned 공간).
 *   3. shmat 으로 프로세스 주소공간에 붙임.
 *   4. shm_attach_to_open_removed() 가 참이면 즉시 shmctl(IPC_RMID)
 *      하여 마지막 detach 시 자동 삭제되도록 함(Linux 지원 동작).
 *   5. memset 으로 0 초기화, DRD_IGNORE_VAR 로 Valgrind 경쟁 감지 제외.
 *   6. 첫 세그먼트이면 디버그 포인터 슬롯을 세그먼트 끝에 배치하고
 *      flow_init() 으로 flow 제어 초기화.
 *
 * 실행 컨텍스트: 메인 스레드. 공유 메모리 생성은 커널 자원 할당이므로
 *   실패 시 errno 가 EINVAL(잘못된 크기)/ENOMEM(메모리 부족)/ENOSPC
 *   (세그먼트 한계 초과) 를 줄 수 있다. fio 는 이 3 가지는 "자원 한계"
 *   로 판단해 조용히 실패(perror 생략).
 *
 * 호출 체인: expand_thread_area() → [이 함수] → shmget(2)/shmat(2).
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
 * [한국어]
 * expand_thread_area() - 현재 세그먼트에 여유가 있으면 그대로 반환,
 *                        없으면 add_thread_segment 로 새 세그먼트 할당.
 *
 * @return: 0 성공(또는 확장 불필요), 비-0 할당 실패.
 *
 * 호출자: get_new_job() 이 슬롯 확보 직전에 호출.
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
 * [한국어]
 * dump_print_option() - --showcmd 모드에서 잡의 단일 옵션을 "--key=value"
 *                       꼴의 명령줄 한 토큰으로 stdout 에 출력한다.
 *
 * @param p: print_option 구조체. 파서가 수집한 (name, value) 페어.
 * @return: 없음.
 *
 * 특이 처리: "description" 옵션은 값에 쉼표/공백이 흔하므로 큰따옴표로
 *   감싼다. 값이 NULL 이면 --key 뒤에 공백만 찍어(인자 없음 옵션) 가시성
 *   유지.
 *
 * 사용 컨텍스트: fio --showcmd job.fio 가 INI 파일을 "동등한 명령줄" 로
 *   바꿔보기 위해 호출하는 경로. 실제 I/O 는 수행하지 않는다.
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
 * [한국어]
 * dump_opt_list() - td->opt_list 에 모아둔 print_option 링크드 리스트를
 *                   순회하면서 dump_print_option 으로 한 엔트리씩 CLI 토큰
 *                   출력. --showcmd 의 본체.
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
 * [한국어]
 * copy_opt_list() - src->opt_list 의 print_option 엔트리들을 dst->opt_list
 *                   로 깊이 복사. 각 name/value 문자열은 strdup 으로 독립
 *                   할당하여 부모-자식 간 쓰기 경쟁 없음.
 *
 * 호출자: get_new_job() 이 def_thread 이외의 부모에서 옵션 히스토리를
 *   승계할 때 사용.
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
 * [한국어]
 * get_new_job() - 공유 메모리 세그먼트에서 빈 thread_data 슬롯을 하나
 *                 확보하고, 부모 td 를 얕은 복사 + 리스트/파일 깊은 복사로
 *                 초기화해 돌려준다. global=true 이면 별도 할당 없이
 *                 def_thread 포인터를 반환.
 *
 * @param global     : true 면 "[global]" 용 템플릿 td(def_thread) 반환.
 * @param parent     : 복사 원본. 옵션 기본값 상속 원천. 최상위 호출에서는
 *                    보통 &def_thread, numjobs 재귀에서는 원본 잡.
 * @param preserve_eo: true 면 엔진 옵션 포인터(td->eo) 를 유지해 NULL 로
 *                    덮어쓰지 않음. add_job 가 numjobs 복제 시 사용.
 * @param jobname    : 섹션 이름. NULL 이면 이름 설정 skip(CLI 의 익명 잡).
 * @return: 새 td 포인터 또는 NULL(세그먼트 확장 실패).
 *
 * 처리 단계:
 *   1. global → 즉시 def_thread 반환(슬롯 소모 없음).
 *   2. expand_thread_area → 필요 시 새 세그먼트 할당.
 *   3. 현재 세그먼트에서 nr_threads++ 슬롯을 잡고, 전역 thread_number++.
 *   4. *td = *parent 로 얕은 복사(옵션 포인터 포함).
 *   5. opt_list 는 새로 초기화하고 부모가 def_thread 가 아니면 깊이 복사
 *      (copy_opt_list 가 strdup 로 print_option 문자열 복제).
 *   6. td->io_ops/io_ops_init = NULL/0 - 엔진은 반드시 후속 ioengine_load
 *      에서 재바인딩.
 *   7. preserve_eo 가 false 면 td->eo 도 NULL 리셋.
 *   8. uid/gid = -1U (미설정 센티넬).
 *   9. fs_list 초기화 + dup_files 로 부모의 fio_file[] 복제.
 *   10. fio_options_mem_dupe - thread_options 내 동적 문자열 필드 깊은
 *       복사(strdup) 로 부모와 쓰기 경합 차단.
 *   11. profile_add_hooks - 프로파일 훅 연결.
 *   12. thread_number/subjob_number 설정.
 *   13. jobname 주어지면 td->o.name = strdup(jobname).
 *   14. 그룹 리포팅 비활성 또는 첫 잡이면 stat_number++.
 *
 * 실행 컨텍스트: 메인 스레드. 여러 번 호출되며 각 호출이 세그먼트 상 한
 *   칸을 소비. 실패 시 에러 로그 후 NULL.
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
 * [한국어]
 * put_job() - get_new_job 의 역연산. 잡 폐기 시 관련 자원을 반납하고
 *             세그먼트 슬롯을 0 으로 밀어 재사용 가능 상태로 복원한다.
 *
 * @param td: 해제할 thread_data. def_thread 는 템플릿이므로 호출해도 NOP.
 * @return: 없음.
 *
 * 해제 순서:
 *   1. def_thread 는 무조건 skip.
 *   2. profile_td_exit - 프로파일 훅 종료.
 *   3. flow_exit_job - flow 제어에서 leave.
 *   4. error 가 있으면 td->verror 문자열을 로그 출력.
 *   5. fio_options_free / fio_dump_options_free - 옵션 문자열 해제.
 *   6. io_ops 가 있으면 free_ioengine - dlclose/ eo free.
 *   7. name strdup 해제.
 *   8. memset(td, 0) - 슬롯 내용 소거.
 *   9. 세그먼트 nr_threads-- 와 전역 thread_number-- 로 카운터 감소.
 *
 * 실행 컨텍스트: 메인 스레드. 보통 add_job 에러 경로나 --showcmd 이후,
 *   또는 parse 에러 회복 경로에서 호출.
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
 * [한국어]
 * __setup_rate() - 주어진 ddir(READ/WRITE/TRIM) 의 bps(바이트/초) rate 를
 *                  계산·저장하고 관련 타이머 상태를 초기화.
 *
 * @param td  : 대상 thread_data.
 * @param ddir: 0=READ / 1=WRITE / 2=TRIM.
 * @return: 0 성공, -1 bps 가 0(너무 낮음).
 *
 * 계산 규칙:
 *   - o->rate[ddir] 이 직접 지정되었으면 그대로 rate_bps 에 저장.
 *   - 아니면 rate_iops[ddir] * min_bs[ddir] 로 환산.
 *
 * 이후 backend 의 레이트 리미터(check_min_rate/rate 관련 sleep)가
 * rate_next_io_time[ddir] 을 기준으로 IO 간 delay 를 계산하며,
 * rate_io_issue_bytes 누계와 last_usec 타임스탬프를 쌍으로 사용한다.
 *
 * 실행 컨텍스트: add_job() 의 setup_rate() 단계에서 호출. 잡 스폰 전이라
 *   동기화 불필요.
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
 * [한국어]
 * setup_rate() - 3 개의 ddir 에 대해 rate/rate_iops 가 지정된 경우
 *                __setup_rate 로 위임하고, 에러는 비트 OR 로 집계.
 *
 * @param td: 대상 thread_data.
 * @return: 0 성공, 비-0 한 방향이라도 실패.
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
 * [한국어]
 * fixed_block_size() - 3 ddir 모두 min_bs==max_bs 이고 서로 동일한지 판정.
 *                      verify 와 randommap 이 가변 bs 에서 제약되므로
 *                      fixup_options 가 이 플래그로 분기.
 *
 * @param o: thread_options.
 * @return: 전부 같으면 1, 하나라도 다르면 0.
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
/*
 * [한국어]
 * gcd() - 유클리드 호제법 기반 최대공약수. verify_interval 이 min_bs/max_bs
 *         의 공약수여야 한다는 제약을 만족시키기 위해 fixup_options 가 두
 *         값의 gcd 를 자동 계산하는 용도. 재귀 구현(O(log min(m,n))).
 */
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
 * [한국어]
 * fixup_options() - thread_options 의 필드 간 의존성을 검증·보정하는 거대
 *                   정적 체커. fio 전체에서 가장 복잡한 "옵션 규칙 집합"이다.
 *
 * @param td: 대상 thread_data (o = &td->o 에 접근).
 * @return: 비트 OR 된 에러 코드(0 = 정상, 1 = 치명, warnings_fatal = 경고를
 *          치명으로 격상한 경우). 비-0 반환 시 add_job 은 goto err 로 잡을
 *          폐기한다.
 *
 * 왜 필요한가: fio 는 수백 개 옵션이 있으며 대부분 직교하지 않고 서로 조건
 *   관계를 가진다. 예) `verify` 가 켜져 있으면 `refill_buffers` 가 암시적
 *   으로 켜져야 하고, `norandommap` 과 `verify` 가 동시에 켜지면 가변 bs 에서
 *   오프셋 충돌이 날 수 있다. 이 함수는 그런 조건을 한 곳에 모아 파서 뒤에서
 *   일괄 처리한다. 콜백 기반 대안도 있지만 규칙이 타 옵션을 여러 개 참조해
 *   대상 옵션 하나에 붙이기 어려워 "lazy" 한 중앙 집중식으로 구현.
 *
 * 검증·보정 카테고리(대표):
 *   [readonly] --readonly 에서 write/trim 거부.
 *   [trimwrite/멀티레인지] num_range>1 + trimwrite/비지원 엔진 조합 거부.
 *   [PSHARED] 프로세스 공유 뮤텍스가 없으면 use_thread 강제.
 *   [iolog] write_iolog 와 read_iolog 동시 지정 시 read 우선.
 *   [zone_mode] ZBD vs STRIDED vs NOT_SPECIFIED vs NONE 의 상호 교정 및
 *               create_serialize / write_zone_remainder / norandommap 정합.
 *   [SPRandom] random + write + LFSR + norandommap=1 을 요구.
 *   [block sizes] min_bs/max_bs 기본값, rw_min_bs 산출, blockalign vs
 *                 randommap, size<bs 거부.
 *   [verify] multi-writer 경고, time_based-only 쓰기 경고, refill_buffers
 *            암시 세팅, verify_interval 보정(최소 bs 와 공약수), verify_offset
 *            + verify_header 크기 검사, write_sequence/header_seed 기본값.
 *   [oatomic] 엔진의 FIO_ATOMICWRITES 지원 여부 검증.
 *   [pre_read] invalidate_cache 와의 교차, PIPEIO 거부.
 *   [단위] FIO_BIT_BASED 엔진은 기본 단위 N2S_BITPERSEC 으로 전환.
 *   [fdatasync 폴백] CONFIG_FDATASYNC 미지원 시 fsync 로 전환.
 *   [Windows] 동기 엔진 + O_DIRECT/O_SYNC 조합 거부.
 *   [compress] 100% 압축이면 zero_buffers 로 최적화, 부분 압축이면 refill
 *              비트 세트.
 *   [random_distribution] 비균일이면 norandommap 강제.
 *   [rand_seed] 명시 시 rand_repeatable 비활성.
 *   [gtod_cpu] 설정 시 gtod 오프로드 스레드 기동.
 *   [latency/iops] disable_lat/clat/slat 으로 percentiles 비활성.
 *   [ms→ns] max_latency/latency_target 을 내부 ns 단위로 변환.
 *   [dedupe working set] size 미지정/nr_files>1 거부.
 *   [steady state] ss_check_interval 일관성과 ss_dur 배수 검사.
 *   [FDP] dp_type 와의 충돌 체크.
 *
 * 실행 컨텍스트: add_job() 가 잡 스폰 직전 호출. 메인 스레드 단독. 실패는
 *   잡 폐기로 이어진다. 경고는 warnings_fatal 에 따라 오염 여부 결정.
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
 * [한국어]
 * init_rand_file_service() - 파일 선택 정책이 비균일 분포(ZIPF/PARETO/GAUSS)
 *                            일 때 해당 생성기를 초기화.
 *
 * @param td: 대상 thread_data.
 * @return: 없음.
 *
 * 동작: nranges = nr_files << FIO_FSERVICE_SHIFT 로 유효 샘플 공간 확장
 *   (shift 로 세분화해 작은 nr_files 에서도 분포가 잘 그려지도록).
 *   각 분포별로 zipf_init/pareto_init/gauss_init 호출 후 zipf_disable_hash
 *   /gauss_disable_hash 로 해시 최적화 경로 비활성(결정적 재현성 유지).
 *
 * 실행 컨텍스트: td_fill_rand_seeds() 에서 file_service_type 이 NONUNIFORM
 *   마스크를 가질 때만 호출.
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
 * [한국어]
 * init_rand_offset_seed() - 오프셋용 frand_state(td->offset_state) 를 시드.
 *
 * @param td: 대상 thread_data.
 * @return: 없음.
 *
 * 왜 별도 함수인가: 파일 크기 / 블록 크기 조합이 32 비트 범위를 초과하면
 *   (get_next_offset 경로에서) 64 비트 생성기가 필요해지며, 이 때 런타임에
 *   오프셋 RNG 만 재초기화해야 한다. 이를 외부에서 호출할 수 있도록
 *   public 심볼로 분리되었다.
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
 * [한국어]
 * td_fill_rand_seeds() - td->rand_seeds[FIO_RAND_*_OFF] 슬롯을 토대로
 *                        잡이 사용하는 모든 frand_state 생성기를 시드·초기화.
 *
 * @param td: 대상 thread_data.
 * @return: 없음.
 *
 * 왜 이렇게 많은 RNG 인가: fio 는 여러 독립적인 축(블록 크기/오프셋/파일/
 *   지연/버퍼/트림/dedupe/prio/zone/FDP/SPRandom/verify/rwmix/poisson)을
 *   각각 독립 생성기로 다뤄야 한다. 시드를 분리해야 예를 들어 "오프셋은
 *   같은데 bs 만 바꾸기" 같은 재현성 모델링이 가능하다.
 *
 * 특수 대응 규칙:
 *   - verify != NONE: write_seed = read_seed. 쓰기와 읽기 단계가 같은
 *     오프셋 시퀀스를 재생해야 검증이 성립.
 *   - trimwrite: trim_seed = write_seed. 트림 후 동일 LBA 에 쓰기.
 *   - 64 비트 난수(Tausworthe64) 요청 시 use64=true 로 각 init_rand_seed
 *     호출에 전달.
 *
 * 초기화 슬롯(모두 td->rand_seeds[FIO_RAND_*_OFF] 에서 유래):
 *   bsrange_state[3]       - READ/WRITE/TRIM 블록 크기 생성기.
 *   verify_state            - 검증용 데이터 패턴 생성기.
 *   rwmix_state             - read/write 혼합비 추첨.
 *   next_file_state         - 파일 선택 RNG (균일 분포일 때).
 *   next_file_zipf/gauss    - init_rand_file_service() 가 초기화.
 *   file_size_state         - file_size_low..high 랜덤 크기.
 *   trim_state              - 트림 확률 추첨.
 *   delay_state             - start_delay_orig..high 랜덤 지연.
 *   poisson_state[3]        - rate_process=poisson 시 IO 간 간격 생성.
 *   dedupe_state            - 중복 블록 추첨.
 *   zone_state              - 존 선택.
 *   prio_state              - cmdprio 확률 추첨.
 *   dedupe_working_set_index_state - dedupe 워킹셋 인덱싱.
 *   offset_state            - init_rand_offset_seed (오프셋).
 *   seq_rand_state[DDIR_RWDIR_CNT] - 순차/랜덤 전환 추첨.
 *   buf_state/buf_state_prev - 버퍼 내용 랜덤화 및 백업.
 *   fdp_state               - FDP RUHID 선택.
 *   sprandom_state          - SPRandom LFSR 전환.
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
 * [한국어]
 * setup_random_seeds() - td->rand_seeds[FIO_RAND_NR_OFFS] 슬롯을 채우고,
 *                        td_fill_rand_seeds() 로 모든 frand_state 를 시드한다.
 *
 * @param td: 대상 thread_data (잡 당 하나).
 * @return: 성공 시 0, 시스템 RNG 초기화 실패 시 해당 errno 반환 값.
 *
 * 모드 분기:
 *   - rand_repeatable=0 && 사용자가 rand_seed 를 명시 안 함 → 시스템 RNG
 *     (init_random_seeds: /dev/urandom 또는 getrandom(2)) 으로 실제 랜덤.
 *   - 그 외 → rand_seed(없으면 컴파일 기본) 에서 해시 곱 0x9e370001UL 을
 *     4 회 적용해 파생 시드 base 를 만들고, 각 슬롯 i 에 대해
 *     seeds[i] = base * td->thread_number + i 를 기록. 그 다음 base 를 다시
 *     해시 곱으로 전진시켜 슬롯 간 상관을 줄인다. 이 결정적 파생이
 *     "재현 가능한 워크로드" 목표의 핵심.
 *
 * 이후 td_fill_rand_seeds(td) 가 각 슬롯을 frand_state 로 변환(init_rand_seed)
 * 하여 블록 크기/verify/파일선택/지연/포아송/트림/dedupe/존/prio/오프셋/
 * 순차-랜덤 전환/버퍼내용/FDP/sprandom 등 개별 RNG 를 준비한다.
 *
 * 실행 컨텍스트: add_job() 전반부 – 잡 스폰 직전, 단일 스레드. 시스템 RNG
 *   실패 시 td_verror 를 통해 에러가 add_job 으로 전파된다.
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
 * [한국어]
 * ioengine_load() - td 에 설정된 IO 엔진(td->o.ioengine 문자열)을 dlopen 또는
 *                   내장 engine_list 에서 찾아 td->io_ops 에 연결하고,
 *                   엔진 전용 옵션(td->eo) 구조체를 준비한다.
 *
 * @param td: 대상 thread_data.
 * @return: 성공 0, 실패 1(로드 불가).
 *
 * 왜 필요한가: fio 는 엔진을 plugin 형태로 설계했고, 잡마다 다른 엔진
 *   (sync/libaio/io_uring/net/nvme/rbd …)을 쓸 수 있다. 엔진별 내부 상태는
 *   td->eo 라는 thread_options 와 유사한 전용 구조체에 보관되며, 엔진의
 *   options[] 테이블을 기반으로 해당 구조체 크기(option_struct_size)가
 *   정의된다.
 *
 * 동작 단계:
 *   1. td->o.ioengine 이 있어야 함(없으면 내부 버그).
 *   2. 이미 td->io_ops 가 있다 =  이전에 로드된 엔진이 있다는 뜻.
 *      - 이름이 같으면 즉시 0 반환(재사용).
 *      - 다르면 load_ioengine 으로 새 ops 를 받아 비교. 실제로 같은
 *        dlhandle/ops 로 판명되면 0 반환(해시 이름만 다른 alias).
 *      - 다른 dlhandle 이면 이전 dlclose + free_ioengine.
 *   3. load_ioengine(td) 호출로 실제 ops 를 받아 td->io_ops 에 대입.
 *      load_ioengine 내부에서 engine_list(정적 등록) 또는 dlopen(".so"
 *      확장명) 으로 동적 로드하고 ops->dlhandle 에 핸들 저장.
 *   4. option_struct_size != 0 이면 td->eo = malloc(size). 부모의 eo 가
 *      있고 엔진 옵션 정의가 같으면 memcpy + options_mem_dupe 로 깊은
 *      복사, 아니면 0-초기화 + fill_default_options. eo 의 첫 필드 자리에
 *      td 포인터를 저장해 엔진 콜백에서 container_of 대신 간편 역참조
 *      가능하게 한다.
 *   5. odirect=1 이면 엔진에 FIO_RAWIO 플래그를 강제 추가(일부 엔진이
 *      direct I/O 인 경우에 raw I/O 로 간주되는 경로 공유).
 *   6. td_set_ioengine_flags(td) 로 엔진 플래그(FIO_SYNCIO/ASYNCIO/
 *      DISKLESSIO 등) 를 td->flags 에 축적.
 *
 * 실행 컨텍스트: 메인 스레드. parse_cmd_line 의 --ioengine 처리, 잡 파일
 *   parser, add_job 재귀 확장에서 반복 호출 가능. 첫 로드 이후 동일
 *   엔진이면 no-op.
 *
 * 호출 체인:
 *   parse_cmd_line/parse_jobs_ini → add_job → [ioengine_load]
 *     → load_ioengine (ioengines.c)
 *     → dlopen(3)/dlsym(3) (.so 엔진 시)
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
 * [한국어]
 * init_flags() - thread_options 의 설정값을 읽어 td->flags 비트를 세팅.
 *                핫 패스에서 "if(o->옵션_중첩_조회)" 대신 단일 비트 체크로
 *                분기하도록 만드는 캐시 역할.
 *
 * @param td: 대상 thread_data.
 * @return: 없음.
 *
 * 대응하는 TD_F_* 비트 풀이:
 *   TD_F_VER_BACKLOG      - verify_backlog 사용. 쓰기 중간중간 읽어서 검증.
 *   TD_F_TRIM_BACKLOG     - trim_backlog 사용. 주기적 트림 주입.
 *   TD_F_READ_IOLOG       - read_iolog_file 사용(replay 모드).
 *   TD_F_REFILL_BUFFERS   - refill_buffers. 매 IO 마다 버퍼 재생성.
 *   TD_F_SCRAMBLE_BUFFERS - scramble_buffers. 단순 repeat 패턴이 아닌 랜덤 채움.
 *                           명시적으로 요청됐거나, zero_buffers 가 명시 설정
 *                           되지 않은 경우 기본값으로 활성.
 *   TD_F_DO_VERIFY        - verify != NONE.
 *   TD_F_NEED_LOCK        - verify_async 또는 offload 모드. io_u 공유 자원
 *                           경합 보호.
 *   TD_F_CHECK_RATE       - rate/rate_iops/ratemin/rate_iops_min 중 하나라도
 *                           설정. backend 핫 루프가 sleep 제어를 수행해야 함.
 *
 * CUDA 메모리: mem_type==MEM_CUDA_MALLOC 이면 device memory 에서 CPU 스크램블
 *   못하므로 TD_F_SCRAMBLE_BUFFERS 비활성.
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

/* [한국어] 파일명 포맷 치환 키워드 enum.
 * 설정자: make_filename() 이 fpre_keywords[] 를 순회하며 key 필드로 switch.
 * 값 범위: 배열 엔트리와 1:1 대응. FPRE_NONE=0 은 현재 미사용(미래 예약). */
enum {
	FPRE_NONE = 0,
	FPRE_JOBNAME,    /* [한국어] $jobname → 잡 이름 문자열 치환. */
	FPRE_JOBNUM,     /* [한국어] $jobnum → numjobs 의 subjob 인덱스 치환. */
	FPRE_FILENUM,    /* [한국어] $filenum → 같은 잡의 파일 인덱스 치환. */
	FPRE_CLIENTUID   /* [한국어] $clientuid → 클라이언트 소켓 주소 문자열. */
};

/* [한국어] 파일명 포맷 치환 규칙 테이블.
 * 설정자: 컴파일 시점 정적 초기화(불변).
 * 읽는 자: make_filename() 이 strcasestr 로 각 키워드 검색.
 * 값 범위: strlen 은 첫 사용 시 계산되어 캐시되며, keyword 는 불변 리터럴.
 * 배열은 {.keyword=NULL} 센티넬로 종료. */
static struct fpre_keyword {
	const char *keyword;
	/* [한국어] 치환 대상 리터럴. 예: "$jobname".
	 * 설정자: 컴파일 타임. 읽는 자: make_filename (strcasestr). */

	size_t strlen;
	/* [한국어] keyword 의 바이트 길이. 첫 호출 때 0→실제 값으로 메모이즈.
	 * 캐시하는 이유는 strlen 은 O(n) 이라 여러 번 반복하는 make_filename
	 * 루프에서 낭비이기 때문. */

	int key;
	/* [한국어] FPRE_* 라벨. switch 분기에 사용. */
} fpre_keywords[] = {
	{ .keyword = "$jobname",	.key = FPRE_JOBNAME, },
	{ .keyword = "$jobnum",		.key = FPRE_JOBNUM, },
	{ .keyword = "$filenum",	.key = FPRE_FILENUM, },
	{ .keyword = "$clientuid",	.key = FPRE_CLIENTUID, },
	{ .keyword = NULL, },  /* [한국어] NULL 센티넬 - 순회 종료 표시. */
	};

/*
 * [한국어]
 * make_filename() - filename_format 옵션 문자열의 키워드들을 실제 값으로
 *                   치환해 최종 파일명을 생성.
 *
 * @param buf     : 결과 저장 버퍼.
 * @param buf_size: buf 용량.
 * @param o       : thread_options. filename_format 필드 참조.
 * @param jobname : 잡 섹션 이름.
 * @param jobnum  : numjobs 의 subjob 인덱스.
 * @param filenum : 같은 잡의 nr_files 중 현재 파일 인덱스.
 * @return: buf 자체(호출자 편의).
 *
 * 치환 가능 키워드: $jobname / $jobnum / $filenum / $clientuid.
 *   각 키워드는 fpre_keywords[] 배열에 정의되어 있고, strcasestr 로 대소문자
 *   무시로 찾아 copy 버퍼에 prefix + 치환값 + suffix 를 재조립 → 다시 buf 에
 *   복사하는 2-buffer ping-pong 방식. 같은 키워드가 여러 번 나오면 while(1)
 *   루프로 반복 치환.
 *
 * 형식 미지정 기본값: "$jobname.$jobnum.$filenum" 대신 sprintf("%s.%d.%d", ...)
 *   의 즉석 포맷으로 축약(과거 호환).
 *
 * 실행 컨텍스트: add_job() 의 파일 자동 생성 루프. client_sockaddr_str 은
 *   원격 실행 환경의 클라이언트 주소가 주입된 전역.
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
 * [한국어]
 * parse_dryrun() - 실행 대신 파서 검사/출력만 수행하는 모드인지 반환.
 *
 * @return: dump_cmdline(--showcmd) 또는 parse_only(--parse-only) 중 하나면 true.
 *
 * 호출자: add_job() 이 드라이런이면 잡 등록 대신 put_job 으로 폐기.
 *   parse_options() 가 잡 수 0 개 상태를 허용할지 판단.
 */
bool parse_dryrun(void)
{
	return dump_cmdline || parse_only;
}

/*
 * [한국어]
 * gen_log_name() - "{logname}_{logtype}[.threadnr].{suf}" 형태로 로그 파일
 *                  이름 문자열을 조립.
 *
 * @param name, size: 출력 버퍼.
 * @param logtype   : "lat"/"slat"/"clat"/"bw"/"iops"/"clat_hist" 중 하나.
 * @param logname   : 베이스 이름(잡 이름이나 사용자 지정).
 * @param num       : 스레드 번호(잡 번호).
 * @param suf       : "log" 또는 "log.fz"(gzip 저장 시).
 * @param per_job   : per-job 로그 옵션. 참이면 이름에 .threadnr 삽입.
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
 * [한국어]
 * check_waitees() - wait_for 옵션의 대상 잡 이름이 실제 등록된 잡 중 몇 개와
 *                   일치하는지 카운트. subjob(numjobs>1 의 복제본)은 제외해
 *                   원본 잡만 대상으로 한다.
 *
 * @param waitee: 찾을 잡 이름.
 * @return: 일치하는 잡 수(0/1 이 정상, 2+ 는 중복 에러).
 *
 * 호출자: wait_for_ok() 가 유효성 판정에 사용.
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
 * [한국어]
 * wait_for_ok() - wait_for 의 3 가지 에러 조건을 검사:
 *                 (a) 자기 자신을 기다리는 self-reference,
 *                 (b) 존재하지 않는 잡 이름,
 *                 (c) 같은 이름의 잡이 여러 개 있어 어느 쪽을 기다릴지 모호.
 *
 * @param jobname: 현재 잡 이름.
 * @param o      : 현재 잡 옵션 (o->wait_for).
 * @return: 유효/미설정이면 true, 에러면 false.
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
 * [한국어]
 * verify_per_group_options() - 동일 groupid 를 공유하는 잡들의 통계 집계
 *                               옵션(lat_percentiles)이 일관된지 확인.
 *                               group_reporting 출력을 위해 필수.
 *
 * @param td      : 현재 잡.
 * @param jobname : 에러 메시지용.
 * @return: 0 정상, 1 불일치 발견.
 *
 * 왜 필요한가: 그룹 집계는 백분위수 계산 파이프라인을 공유하므로 한 잡이라도
 *   lat_percentiles 를 다르게 설정하면 버킷 크기가 어긋나 수치 의미가 붕괴.
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
 * [한국어]
 * make_log_name() - 로그 파일명이 NULL 또는 빈 문자열이면 잡 이름으로 대체.
 *                   "빈 문자열은 미설정과 동일"로 취급하는 규약.
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
 * [한국어]
 * add_job() - 잡 등록의 최종 단계. thread_data 를 런타임에 필요한 모든
 *             런-타임 상태(엔진, 파일 리스트, 난수, flow, 로그, 통계, 그룹,
 *             rate, steady-state) 로 마감하고, numjobs 만큼 서브잡을
 *             재귀적으로 확장한다.
 *
 * @param td         : 등록할 thread_data. get_new_job 이 이미 만든 포인터.
 * @param jobname    : 섹션 이름 또는 사용자가 준 잡 이름.
 * @param job_add_num: numjobs 확장에서의 서브잡 인덱스. 0 = 원본 잡,
 *                    1..numjobs-1 = 복제본.
 * @param recursed   : 본 함수가 numjobs 재귀로 들어왔는지 플래그. 서버 모드
 *                    send_add_job 중복 전송 방지에 사용.
 * @param client_type: FIO_CLIENT_TYPE_CLI / GUI / ... 결과를 리모트에 전달
 *                    할 때 태깅용.
 * @return: 0 성공, -1 실패(중간 어디서든 goto err → put_job).
 *
 * 핵심 처리 순서(코드 블록 단위):
 *   1. def_thread 면 즉시 반환(템플릿이라 실제 잡 없음).
 *   2. init_flags(td) - TD_F_* 비트 세팅(verify, scramble, rate 등).
 *   3. parse_dryrun() 이면 put_job 으로 폐기하고 종료(--showcmd/--parse-only).
 *   4. profile_td_init / ioengine_load - 프로파일 훅 + I/O 엔진 로드.
 *   5. 파일 자동 생성: filename 미지정 + files 0 개 + iolog 읽기 아님 이면
 *      filename_format 에 따라 add_file 로 nr_files 개 생성.
 *   6. setup_random_seeds - 모든 frand_state 시드.
 *   7. fixup_options - 의존성/충돌 검증(실패 시 goto err).
 *   8. init_dedupe_working_set_seeds (global 아닐 때).
 *   9. wait_for_ok - wait_for 대상 잡이 유일하고 자기가 아닌지.
 *   10. flow_init_job - job 간 flow 제어 참여.
 *   11. diskless 엔진이면 real_file_size 를 -1ULL 로 강제.
 *   12. fio_sem_init(LOCKED) - 잡 start 세마포어 생성(메인이 release).
 *   13. ts.* percentiles/sig_figs 구성, init_thread_stat_min_vals.
 *   14. ddir_seq_nr = 1 - 첫 get_next_offset 에서 랜덤 오프셋 생성되도록.
 *   15. stonewall/new_group 처리 → groupid 증가 조건 판정.
 *   16. verify_per_group_options - 그룹 내 lat_percentiles 일관성.
 *   17. setup_rate - rate 또는 rate_iops 를 bps 로 환산.
 *   18. td_ramp_period_init - ramp_time 타이머 준비.
 *   19. lat/slat/clat/hist/bw/iops 로그 파일 스트림 개방(setup_log).
 *       - write_lat_log + log_issue_time 조합 검증.
 *       - hist_log 은 서버 모드에서 zlib 필수.
 *   20. 첫 서브잡이면 콘솔에 한 줄 요약(rw/bs/ioengine/iodepth) 출력, 서버
 *       모드면 fio_server_send_add_job.
 *   21. td_steadystate_init - ss_dur 감지 윈도우 예약.
 *   22. merge_blktrace_file 이 있으면 merge_blktrace_iologs. merge-only 모드
 *       이면 잡을 버리고 0 반환.
 *   23. numjobs > 1 이면 get_new_job 으로 템플릿 복제 후 add_job 재귀 호출.
 *       서브잡에서는 numjobs/stonewall/new_group 을 지우고 subjob_number 설정.
 *       filename 자동 할당인 경우 복제된 파일 리스트를 초기화해 중복 오픈을
 *       피한다.
 *
 * 실행 컨텍스트: 메인 스레드. 이 시점에서 잡은 아직 spawn 되지 않았으며
 *   thread_data 는 공유 메모리 안에 있어 잡 시작 후 자식도 그대로 참조한다.
 *
 * 에러 경로: 단계 4~22 어디서든 실패하면 goto err 로 분기 → put_job(td)
 *   가 세그먼트 슬롯을 반환하고 자원 정리 후 -1 리턴. numjobs 재귀 실패 시
 *   부모도 동일 경로로 폐기.
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
 * [한국어]
 * add_job_opts() - "key=value" 형태 문자열 배열을 CLI 토큰처럼 파싱해
 *                  잡을 프로그래매틱하게 추가한다. profiles/ 의 내장 프로파일
 *                  이 주로 사용.
 *
 * @param o          : NULL 종료 문자열 배열. 각 토큰은 "name=X" 또는 "key=V".
 * @param client_type: add_job 에 전달.
 *
 * 상태 머신: name= 토큰을 만날 때마다 이전 잡(td) 을 add_job 으로 마감하고
 *   새 td 시작. "name=" 앞 글로벌 옵션은 td_parent(def_thread) 에 누적.
 *   in_global 플래그로 첫 "name" 이전/이후 상태 구분.
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
 * [한국어]
 * skip_this_section() - --section 목록이 있을 때 주어진 섹션 이름이 실행
 *                       대상인지 판정. global 섹션은 무조건 포함한다.
 *
 * @param name: 섹션 이름.
 * @return: skip 이면 1, 실행이면 0.
 *
 * nr_job_sections==0 이면 --section 미사용이므로 모두 실행(0 반환).
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
 * [한국어]
 * is_empty_or_comment() - INI 파서의 라인 필터. 줄이 공백/제어문자만
 *                         포함하거나, 유효 문자 전에 ';' 또는 '#' 을 만나면
 *                         주석으로 간주해 skip.
 *
 * @param line: 검사할 널 종료 문자열.
 * @return: 1 이면 무시, 0 이면 유의미한 내용.
 *
 * 알고리즘: 앞쪽부터 한 글자씩 보고 (1) ';' 또는 '#' 즉시 주석,
 *   (2) 비공백/비제어 문자 발견 시 내용 존재(0 반환), (3) 끝까지 공백이면 빈 줄.
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
 * [한국어]
 * __parse_jobs_ini() - fio .fio 잡 파일(INI 형식)의 본체 파서.
 *                      [global] / [jobname] 섹션, 섹션 몸체의 "key=value"
 *                      옵션, 그리고 "include path" 지시자를 처리한다.
 *
 * @param td            : nested=1(include 재귀) 일 때 상위에서 이미 만든 td.
 *                       초기 호출은 NULL. assert(td || !nested).
 * @param file          : 파일 경로(is_buf=0) 또는 RAW 문자열 버퍼(is_buf=1).
 *                       "-" 는 stdin 을 의미하며, stdin_occupied 로 이중 사용
 *                       (read_iolog_file="-" 과의 충돌) 을 감지한다.
 * @param is_buf        : true 면 file 을 버퍼 포인터로 해석하고 strsep 으로
 *                       줄 분할. false 면 fopen 후 fgets.
 * @param stonewall_flag: 첫 잡 앞에 stonewall 을 자동 삽입할지. 여러 파일을
 *                       순차 실행할 때 경계를 긋기 위한 용도.
 * @param type          : 클라이언트 타입 태그(add_job 에 전달).
 * @param nested        : include 로 재귀 호출된 상태 여부. 1 이면 섹션 헤더
 *                       ([...]) 을 금지하고 옵션만 이어 수집.
 * @param name          : 섹션 이름 버퍼 포인터(280B). nested 에서는 상위로
 *                       부터 받고, 최상위에서는 calloc 로 확보 후 out 에서
 *                       free.
 * @param popts,aopts,nopts: 옵션 문자열 배열과 용량·개수. include 재귀 시
 *                       부모 섹션의 배열에 이어서 append 할 수 있도록 공유.
 * @return: 0 성공, 1/errno 실패.
 *
 * INI 문법 요약:
 *   ; 또는 # 은 주석.
 *   [name]               잡 섹션 시작. name == "global" 이면 def_thread 템플릿
 *                        에 쌓이고, 그 외 이름은 get_new_job 으로 새 td 생성.
 *   key = value          옵션 정의. 본 파서는 라인 수집만 하고 실제 파싱은
 *                        fio_options_parse() 에 위임.
 *   include path         path 에 적힌 다른 파일을 재귀적으로 파싱. 상대 경로는
 *                        현재 파일의 디렉토리 기준. nested 안에서는 새 섹션이
 *                        허용되지 않으므로 include 파일은 "옵션 조각"만 담을
 *                        수 있다.
 *
 * 처리 흐름:
 *   1. 파일 열기(is_buf/stdin/일반).
 *   2. 줄 읽기 → 공백/주석 제거 → 섹션 헤더이면 이름 추출 + skip_this_section
 *      판정 → get_new_job → stonewall 주입.
 *   3. 섹션 내부에서 다시 줄을 읽어 include 이면 재귀, 아니면 strdup 으로
 *      opts[num_opts++] 에 수집. 배열 포화 시 2 배 realloc.
 *   4. 섹션 끝(새 [...] 또는 EOF) 에 도달하면 fio_options_parse(td, opts,
 *      num_opts) 로 일괄 파싱. 성공 시 add_job, 실패 시 put_job.
 *   5. opts 내용 free.
 *   6. 파일 종료 후 --section 목록/섹션 이름 버퍼/opts 배열 해제.
 *
 * 실행 컨텍스트: 메인 스레드. 재귀는 include 한 번당 한 단계 깊이. opts 배열과
 *   name 버퍼는 트리 전체에서 공유되므로 include 깊이가 커져도 O(n) 메모리.
 *
 * 호출 체인:
 *   parse_options → parse_jobs_ini → [__parse_jobs_ini]
 *     → fio_options_parse → add_job.
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
 * [한국어]
 * parse_jobs_ini() - __parse_jobs_ini 의 공개 thin wrapper. nested/name/popts
 *                    등의 재귀 파라미터를 0/NULL 로 넘겨 최상위 호출을 뜻한다.
 *                    외부(parse_options, server.c)가 진입점으로 사용.
 */
int parse_jobs_ini(char *file, int is_buf, int stonewall_flag, int type)
{
	return __parse_jobs_ini(NULL, file, is_buf, stonewall_flag, type,
			0, NULL, NULL, NULL, NULL);
}

/*
 * [한국어]
 * fill_def_thread() - def_thread(글로벌 템플릿) 를 기본 상태로 리셋.
 *                     parse_options 가 여러 잡 파일을 순차 처리할 때 각 파일
 *                     사이에 호출해, 이전 파일의 [global] 이 다음 파일로
 *                     전파되지 않도록 한다.
 *
 * @return: 항상 0.
 *
 * 동작: memset 0 → opt_list 초기화 → fio_getaffinity 로 현재 프로세스
 *   CPU 마스크 기록 → error_dump=1 기본값 → fio_fill_default_options 로
 *   fio_options[] 테이블의 .def 값을 일괄 채움.
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
 * [한국어]
 * show_debug_categories() - usage()/--debug=help 에서 사용 가능한 디버그
 *                           카테고리 이름 목록을 80 칼럼 폭으로 wrap 해 출력.
 *                           FIO_INC_DEBUG 가 켜진 빌드에서만 실제 내용 있음.
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
 * [한국어]
 * usage() - "--help" 출력. 옵션 참조 목록을 printf 로 나열.
 *
 * @param name: argv[0](바이너리 경로). "usage: foo [options]" 머리행에 사용.
 *
 * 표시되지 않는 옵션: --append-terse(별칭), --latency-log(deprecated).
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
 * [한국어] 디버그 카테고리 정적 테이블.
 * 각 엔트리의 .shift 는 fio_debug 비트 중 해당 카테고리의 위치를 지정한다.
 * dprint(FD_IO, ...) 는 (fio_debug & (1 << FD_IO)) 로 게이팅된다.
 * 배열 말미의 {.name=NULL} 은 순회 종료 센티넬.
 *
 * 설정자: set_debug() 가 --debug=io,verify 같은 토큰을 파싱해 해당 shift
 *   비트를 OR.
 * 읽는 자: show_debug_categories(), set_debug(), 그리고 dprint 매크로가
 *   런타임에 fio_debug 를 검사.
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
 * [한국어]
 * set_debug() - --debug=list 인수를 파싱해 fio_debug 비트마스크를 구성.
 *
 * @param string: "io,verify" 또는 "all" 또는 "?"/"help" 또는 "job:NR".
 * @return: 0 성공, 1 help 출력 요청으로 exit 유도.
 *
 * 특이 처리: "job" 카테고리는 ":N" 접미사로 대상 잡 번호를 기록
 *   (fio_debug_jobno 전역). 이 필터와 FD_* 마스크가 AND 조합되어
 *   dprint 가 출력을 결정한다.
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
 * [한국어]
 * fio_options_fill_optstring() - l_opts[] 를 순회하며 getopt(3) 용 짧은
 *                                optstring 을 자동 생성해 cmd_optstr 에 저장.
 *
 * 규약: has_arg == required_argument → ':' 한 번, optional_argument → '::' 두 번.
 *   no_argument 는 문자만 기록.
 *
 * 왜 자동 생성: l_opts 에 옵션이 추가될 때마다 optstring 을 손으로 맞추면
 *   실수 위험이 커서, 단일 진실의 소스로 l_opts 에 has_arg 를 두고 런타임에
 *   파생한다.
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
 * [한국어]
 * client_flag_set() - 주어진 하위 8비트 옵션 문자가 l_opts[] 에서
 *                     FIO_CLIENT_FLAG 비트를 가진 옵션인지 조회.
 *
 * @param c: getopt 가 반환한 옵션 값(하위 8비트로 마스킹된 상태 가정).
 * @return: FIO_CLIENT_FLAG 또는 0.
 *
 * 왜 필요한가: getopt 반환값이 이미 FIO_CLIENT_FLAG 를 뺀 형태일 때도
 *   "이 옵션이 원격 전달 대상인지" 를 재확인하기 위해 사용.
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
 * [한국어]
 * parse_cmd_client() - FIO_CLIENT_FLAG 마킹된 옵션을 원격 클라이언트에게
 *                      포워딩. 서버가 옵션을 직접 소비하더라도 클라이언트도
 *                      동일 옵션을 적용하도록 보장.
 */
static void parse_cmd_client(void *client, char *opt)
{
	fio_client_add_cmd_option(client, opt);
}

/*
 * [한국어]
 * show_closest_option() - 인식 불가 옵션에 대해 편집거리(string_distance,
 *                         Levenshtein) 로 가장 가까운 l_opts 엔트리를 찾아
 *                         "Did you mean X?" 제안. 거리가 너무 멀면 생략.
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
 * [한국어]
 * parse_output_format() - --output-format 의 쉼표 구분 리스트를 해석해
 *                         output_format 비트마스크를 재구성.
 *
 * 인식 토큰:
 *   minimal/terse/csv → FIO_OUTPUT_TERSE
 *   json              → FIO_OUTPUT_JSON
 *   json+             → FIO_OUTPUT_JSON | JSON_PLUS (상세 per-sample)
 *   normal            → FIO_OUTPUT_NORMAL
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
 * [한국어]
 * parse_cmd_line() - fio 의 CLI 파서. getopt_long_only(3) 기반으로 l_opts[]
 *                    의 약 50+ 개 옵션을 단일 switch 로 디스패치한다. 동시에
 *                    --name 등의 잡 옵션이 오면 get_new_job + ioengine_load
 *                    + add_job 을 내부적으로 오케스트레이션해 잡을 즉시 등록.
 *
 * @param argc        : argc(표준). main() 에서 그대로 전달.
 * @param argv        : argv(표준). 바이너리 이름 + CLI 토큰. getopt 가 argv
 *                     를 in-place 로 재정렬 가능.
 * @param client_type : FIO_CLIENT_TYPE_CLI 등. 원격 클라이언트 모드에서 잡을
 *                     등록할 때 태깅용.
 * @return: 수집된 잡 파일(INI) 개수(ini_idx). parse_options() 가 이 개수만큼
 *   parse_jobs_ini 를 호출한다.
 *
 * 왜 함수가 크게 비대한가: fio 는 "부트스트랩 CLI + 잡 옵션 + I/O 엔진
 *   전용 옵션" 을 한 바이너리에서 모두 받기 때문에 한 switch 에 FIO_GETOPT_JOB
 *   (fio_options[]), FIO_GETOPT_IOENGINE (엔진 옵션), 그리고 고유 CLI 옵션을
 *   모두 분기해야 한다.
 *
 * 주요 처리 경로:
 *   - optind=1 로 매 호출 초기화(서버 모드에서 클라이언트 요청마다 파서 재진입).
 *   - getopt_long_only 루프에서 c 를 받아 FIO_CLIENT_FLAG 가 켜졌으면 원격
 *     클라이언트로 forward 후 플래그 제거.
 *   - 단순 플래그: -m(minimal), -r(readonly), -w(warnings_fatal) 등은 전역 토글.
 *   - 값 저장: -o(출력 파일), -F(출력 형식 - parse_output_format), -V(terse
 *     version), -e(ETA 모드), -E/-O(ETA 간격), -d(디버그), -x(--section),
 *     -X(inflate-log), -p(profile), -D(daemonize), -W(trigger 파일) 등.
 *   - 분기성 동작: -T(cpuclock test) / -G(crctest) / -M(memcpy test) /
 *     -I(idle-prof) 는 즉시 해당 루틴을 실행 후 do_exit=1.
 *   - 클라이언트/서버: -S, -N(Windows), -C, -R.
 *   - 잡 옵션(FIO_GETOPT_JOB): opt == "name" 을 만나면 이전 잡을 add_job 으로
 *     마감하고 새 td 생성(get_new_job) + 엔진 로드. 그 외 키는 fio_cmd_option_
 *     parse 에 위임. opt == "ioengine" 이면 엔진을 다시 로드해 옵션 세트를
 *     교체.
 *   - 엔진 옵션(FIO_GETOPT_IOENGINE): 현재 td 가 있으면
 *     fio_cmd_ioengine_option_parse 로 td->eo 에 주입.
 *   - 에러: '?' 는 show_closest_option 으로 levenshtein 제안 + fallthrough
 *     로 exit.
 *   - 루프 종료 후 : do_exit && !(is_backend || nr_clients) 이면 exit().
 *     클라이언트/서버 모드이면 계속해서 fio_clients_connect 로 네트워크 연결.
 *   - 서버 모드이면 fio_start_server 로 진입해 이벤트 루프 시작(여기서 return).
 *   - 남은 positional 인자를 ini_file 배열에 strdup 수집.
 *
 * 실행 컨텍스트: 메인 스레드. 서버 모드에서는 클라이언트 요청마다 같은 함수가
 *   재진입될 수 있어 optind 를 매번 리셋한다. 재진입 시에도 전역 def_thread/
 *   segments 는 공유.
 *
 * 호출 체인:
 *   parse_options → [parse_cmd_line] → (switch 내부) add_job / get_new_job /
 *     ioengine_load / fio_options_parse / fio_client_add / fio_start_server / ...
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
 * [한국어]
 * fio_init_options() - 옵션 파서 시스템의 bring-up 루틴.
 *
 * @return: 0 성공, 1 def_thread 초기화 실패.
 *
 * 수행:
 *   1. f_out=stdout, f_err=stderr.
 *   2. fio_options_fill_optstring() 으로 cmd_optstr 생성.
 *   3. fio_options_dup_and_init(l_opts) 가 fio_options[] 의 잡 옵션들을
 *      l_opts 끝에 append 하며, FIO_GETOPT_JOB 값을 val 에 부여해
 *      parse_cmd_line switch 에서 구분되도록 함.
 *   4. atexit(free_shm) - 프로세스 종료 시 공유 자원 정리 훅.
 *   5. fill_def_thread() - def_thread 를 기본값으로 채움.
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
 * [한국어]
 * parse_options() - fio 옵션 파싱 전체 오케스트레이터. main()이 호출하는
 *                   최상위 진입점으로, 본 파일의 모든 하위 루틴을 한 순서로
 *                   엮어 thread_data 배열을 구성한다.
 *
 * @param argc : argc(from main).
 * @param argv : argv(from main).
 * @return: 0 성공. 1 실패(옵션 시스템 초기화 실패, ini 파싱 실패,
 *   정상 경로가 아닌데 잡이 0 개). 성공인데도 잡이 0 개가 허용되는
 *   경우는 parse_dryrun/exec_profile/is_backend/nr_clients/did_arg 가
 *   true 일 때.
 *
 * 흐름:
 *   1. fio_init_options() - f_out/f_err 을 표준 스트림으로, optstring 생성,
 *      l_opts 에 잡 옵션 추가, atexit(free_shm), def_thread 초기화.
 *   2. fio_test_cconv(&def_thread.o) - thread_options <-> thread_options_pack
 *      변환 자기검사(네트워크 전송 안정성 보증).
 *   3. parse_cmd_line - CLI 파싱. 이 과정에서 --name 등의 잡 옵션이 있으면
 *      내부적으로 add_job 이 호출되어 이미 잡이 생성될 수 있음. 남은
 *      positional 인자는 ini_file[] 로 수집되어 job_files 에 개수 반환.
 *   4. job_files > 0:
 *      각 i 에 대해:
 *        - i >= 1 이면 fill_def_thread 를 다시 호출해 [global] 누적 리셋.
 *        - nr_clients (클라이언트 모드) → fio_clients_send_ini 로 원격 전송
 *          (서버에서 파싱).
 *        - !is_backend (로컬 모드) → parse_jobs_ini 로 로컬 파싱.
 *      각 파일 처리 후 free(ini_file[i]).
 *   5. job_files == 0 && nr_clients > 0 → 잡 파일 없이 클라이언트 모드: 빈
 *      송신으로 서버가 자체 ini 로 동작하게 함.
 *   6. 정리: free(ini_file), fio_options_free(&def_thread), filesetup_mem_free.
 *   7. thread_number == 0 이면 유효한 동작 여부를 판정. 잡은 없지만 dryrun/
 *      profile/backend/client/did_arg 중 하나라면 허용. 아니면 usage + 실패.
 *   8. output_format 이 NORMAL 이면 헤더(fio 버전 문자열) 출력.
 *
 * 실행 컨텍스트: main() 의 단일 스레드. 이후 fio_backend 또는 fio_handle_clients
 *   로 제어가 이관되어 실제 I/O 가 시작된다.
 *
 * 에러 경로: 어떤 단계든 실패하면 1 반환, main()에서 exit(1)에 의해 프로세스
 *   종료. atexit(free_shm) 이 등록되어 있어 잡 자원과 SHM 은 자동 정리.
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
 * [한국어]
 * options_default_fill() - def_thread.o 의 현재 상태를 주어진 thread_options
 *                          에 얕은 복사로 주입. 주로 내부 옵션 프리셋 도구가
 *                          사용.
 *
 * 주의: 얕은 복사이므로 문자열 필드가 공유된다. 복사본이 독립적 수명이어야
 *   하면 이어서 fio_options_mem_dupe 를 호출해야 한다.
 */
void options_default_fill(struct thread_options *o)
{
	memcpy(o, &def_thread.o, sizeof(*o));
}

/*
 * [한국어]
 * get_global_options() - def_thread(정적 템플릿 td) 의 주소를 외부에 노출.
 *                        server.c 가 원격 잡 추가 시 글로벌 옵션을 스냅샷
 *                        하거나, profiles/ 가 [global] 기본값을 오버라이드할 때
 *                        사용.
 */
struct thread_data *get_global_options(void)
{
	return &def_thread;
}
