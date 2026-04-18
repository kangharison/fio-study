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
 * [한국어 설명] fio 백엔드 실행 엔진 (backend.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio 로컬 백엔드의 "실행 오케스트레이터"이다. 사용자가 CLI/잡 파일로
 * 선언한 모든 잡(job)을 스레드 또는 프로세스로 물리화(spawn)하고, 각 워커가
 * 준비→I/O 루프→검증→정리의 전체 생명주기를 따라가도록 조율한다. 또한 잡 간의
 * 순서 제약(stonewall / wait_for / start_delay), runstate 전이(TD_NOT_CREATED →
 * CREATED → INITIALIZED → RAMP → RUNNING → VERIFYING → FSYNCING → FINISHING →
 * EXITED → REAPED) 추적, 시그널 처리(SIGINT/SIGTERM/SIGUSR1/SIGBREAK), 드라이런,
 * 트리거 파일 폴링, 정기 통계/디스크 유틸 헬퍼 스레드 감시까지 포괄한다. 서버
 * 모드에서는 fio.c가 fio_handle_clients()로 분기하지만, 로컬 모드와 `--server`
 * 내부 잡 실행은 모두 fio_backend()를 진입점으로 이 파일을 통과한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   main() [fio.c]
 *     → parse_options() [init.c] : 옵션 파싱, thread_data[] 배열 구성
 *     → fio_backend(sk_out) [이 파일]   ★ 최상위 진입점
 *         → helper_thread_create() [helper_thread.c] : 통계/디스크유틸 주기 갱신
 *         → run_threads() [이 파일]
 *             → for each td: pthread_create(thread_main, td) 또는 fork()→thread_main
 *                 → thread_main() [이 파일]
 *                     → td_io_init() [ioengines.c] : 엔진 플러그인 .init 호출
 *                     → init_io_u()/init_io_u_buffers() : iodepth 개 io_u 풀 할당
 *                     → setup_files()/pre_read_files() [filesetup.c]
 *                     → do_io() [이 파일]  ★ 실제 I/O 루프
 *                         → get_io_u() [io_u.c] → 오프셋/크기/방향 결정
 *                         → io_u_submit() → td_io_queue() [ioengines.c]
 *                         → wait_for_completions() → io_u_queued_complete()
 *                     → do_verify() (verify 옵션 시)
 *                     → close_and_free_files()/close_ioengine()
 *             → reap_threads() : TD_EXITED → TD_REAPED 전이, waitpid(2)
 *         → __show_run_stats() [stat.c]
 *   실행 컨텍스트: 로컬 유저스페이스. `run_threads()`는 메인 스레드, 각 잡은
 *   별도 pthread(use_thread=1) 또는 별도 프로세스(fork). 시그널은 메인 스레드가
 *   받고 fio_terminate_threads()가 공유 메모리의 td->terminate를 세팅해 전파.
 *
 * === 타 모듈과의 연결 ===
 * - fio.c        : main() → fio_backend() 진입. nr_clients 분기로 로컬/서버 선택.
 * - init.c       : parse_options()가 thread_number/thread_data[]를 채워 전달.
 * - ioengines.c  : td_io_{init,prep,queue,commit,getevents,close_file,open_file}
 *                  모든 엔진 콜백의 진입은 do_io()/do_verify()/thread_main()에서만.
 * - io_u.c       : get_io_u()/put_io_u()/io_u_queued_complete()/io_u_sync_complete()
 *                  io_u 생명주기(free → prepped → in_flight → completed → free)
 *                  는 전적으로 이 파일의 do_io()/do_verify() 루프가 구동한다.
 * - stat.c       : show_run_stats/update_rusage_stat/stat_init/stat_exit, 통계
 *                  세마포어 stat_sem 이 thread_main()의 런타임 갱신을 직렬화.
 * - verify.c     : fio_verify_init/do_verify 에서 verify_io_u(sync/async), CRC/
 *                  MD5/PATTERN 등 VERIFY_* 헤더 검증 콜백 체인을 구동.
 * - iolog.c      : read_iolog/log_io_piece/prune_io_piece_log/init_iolog/
 *                  iolog_compress_init 압축 스레드를 CPU affinity 설정 전에 생성.
 * - diskutil.c   : init_disk_util/disk_util_prune_entries — 블록 디바이스 I/O 통계.
 * - helper_thread.c : helper_thread_create(startup_sem, sk_out) — 주기 I/O tick,
 *                  status line, steadystate 등 잡과 독립적인 주기 태스크를 수행.
 * - server.c     : is_backend 플래그 분기. fio_server_got_signal/send_start/
 *                  fio_server_get_verify_state로 서버-클라이언트 동기화.
 * - smalloc.c    : smalloc/sfree — 공유 메모리 할당기. td->sem, stat_sem,
 *                  cgroup_list, agg_io_log 등 프로세스 간 공유 객체에 사용.
 * - pshared.c    : mutex_cond_init_pshared/cond_init_pshared — fork 모드에서
 *                  뮤텍스/조건변수를 프로세스 간 공유(PTHREAD_PROCESS_SHARED).
 * - 공유 전역자료구조: thread_data 배열(thread_number개), agg_io_log[DDIR_RWDIR_CNT],
 *                  startup_sem(초기화 동기화), stat_sem(통계 직렬화), overlap_check
 *                  (serialize_overlap 경합), cgroup_list/cgroup_mnt.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_backend(sk_out)     : 최상위 진입점. 프로파일 로드 → stat_init →
 *                              helper_thread_create → run_threads → 통계 → 해제.
 * - run_threads(sk_out)     : 잡 스폰/감시 컨트롤러. todo 루프로 TD_NOT_CREATED
 *                              를 생성하고 startup_sem/td->sem 으로 단계 동기화,
 *                              reap_threads 로 TD_EXITED → TD_REAPED 진행.
 * - thread_main(data)       : 각 잡 워커의 엔트리. fork_data 수령 → runstate
 *                              전이(INITIALIZED→RAMP|RUNNING→VERIFYING→FINISHING
 *                              →EXITED) → do_io/do_verify 반복 → cleanup.
 * - do_io(td, bytes_done)   : 메인 I/O 루프. get_io_u→io_u_submit→commit→
 *                              getevents→put_io_u 파이프라인. time_based/loops/
 *                              number_ios/fill_device/rate/latency_target 종료조건.
 * - do_verify(td, bytes)    : 검증 루프. get_next_verify + fsync+invalidate +
 *                              verify_io_u/verify_io_u_async 콜백 체인.
 * - io_queue_event(...)     : td_io_queue 반환(COMPLETED/QUEUED/BUSY) 분기,
 *                              short I/O(resid) 재큐잉, 완료 회계, 에러 처리.
 * - wait_for_completions()  : io_u_queued_complete를 iodepth_low 까지 반복 호출.
 * - reap_threads(nr,t,m)    : use_thread=waitpid 무관/대신 runstate 확인,
 *                              use_fork=waitpid(2)로 WIFEXITED/WIFSIGNALED 확인.
 * - set_sig_handlers()      : sigaction(SA_RESTART) 로 SIGINT/TERM/USR1/PIPE 등록.
 * - fork_data {td, sk_out}  : pthread_create/fork 후 thread_main 인자로 전달.
 *
 * === td->runstate 전이 다이어그램 (전체 잡 생명주기) ===
 *   TD_NOT_CREATED
 *        │  run_threads(): pthread_create 또는 fork() 성공
 *        ▼
 *   TD_CREATED                   ← 부모 스레드가 startup_sem 대기
 *        │  thread_main(): 초기 setup 완료
 *        ▼
 *   TD_INITIALIZED               ← 자식이 fio_sem_up(startup_sem),
 *        │                        그 후 fio_sem_down(td->sem) 블록
 *        │  run_threads(): 모든 초기화 완료 확인 후 fio_sem_up(td->sem)
 *        ▼
 *   TD_RAMP  ←→  TD_RUNNING      ← ramp_time 동안 TD_RAMP, 이후 TD_RUNNING
 *        │           │  do_io() 메인 루프 + 검증 루프 진입 시 TD_VERIFYING 전이
 *        │           ▼
 *        │      TD_VERIFYING      ← do_verify() 중
 *        │           │  복귀하면 다시 TD_RUNNING
 *        ▼           ▼
 *   TD_FSYNCING                  ← end_fsync/end_syncfs 플러시 중
 *        │
 *        ▼
 *   TD_FINISHING                 ← thread_main: 런타임 통계 최종 기록
 *        │  cleanup: close_files/close_ioengine/fio_unpin_memory
 *        ▼
 *   TD_EXITED                    ← thread_main 반환 직전. 부모가 감지.
 *        │  reap_threads(): use_thread=즉시 / use_fork=waitpid 후
 *        ▼
 *   TD_REAPED                    ← 부모가 회수 완료. nr_running-- 처리.
 *
 *   전이 함수: td_set_runstate(td, TD_XXX) (fio.h). TD_EXITED 이후에는
 *   자식이 더 이상 td->sem/stat_sem 을 건드리면 안된다. reap_threads()가
 *   TD_REAPED 를 set 후 done_secs 를 누적하고 group rate 합계를 감산.
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <unistd.h>     /* [한국어] fork(2), getpid/gettid, setsid, _exit, unlink,
                         *  usleep, read/write. 프로세스 모드에서 fork/setsid/_exit
                         *  를 사용하고, 모든 모드에서 getpid/gettid로 td->pid 기록. */
#include <string.h>     /* [한국어] memset/memcpy/strerror/strcmp/strcpy/strsep.
                         *  타임스탬프 구조체 복사, sysfs 파일 파싱(set_ioscheduler),
                         *  에러 메시지 구성에 사용. */
#include <signal.h>     /* [한국어] sigaction(2)/struct sigaction/SA_RESTART/
                         *  SIGINT/SIGTERM/SIGUSR1/SIGPIPE/SIGBREAK(Win)/kill(2)/
                         *  SIGTERM. set_sig_handlers()와 reap_threads()의
                         *  kill(td->pid, SIGTERM) 경로에서 필수. */
#include <assert.h>     /* [한국어] assert() — ddir_rw(ddir), file->du 유효성,
                         *  !(td->flags & TD_F_CHILD) 등 invariant 검증. 릴리즈
                         *  빌드에서는 NDEBUG 로 컴파일 아웃. */
#include <inttypes.h>   /* [한국어] PRIu64/PRIu32 포맷 매크로 — log_err/dprint
                         *  에서 uint64_t numberio/inflight_idx 를 안전하게 출력. */
#include <sys/stat.h>   /* [한국어] stat(2)/struct stat — __check_trigger_file()
                         *  에서 trigger_file 존재 확인, 이후 unlink(2). */
#include <sys/wait.h>   /* [한국어] waitpid(2)/WNOHANG/WIFEXITED/WEXITSTATUS/
                         *  WIFSIGNALED/WTERMSIG — reap_threads()에서 fork 자식의
                         *  종료 상태를 비블로킹으로 확인(ECHILD 처리 포함). */
#include <math.h>       /* [한국어] logf() — usec_for_io()의 포아송 프로세스
                         *  (RATE_PROCESS_POISSON)에서 지수분포 샘플링용
                         *  -ln(U)/lambda 계산. -lm 링크 필요. */
#include <pthread.h>    /* [한국어] pthread_create/detach/mutex_lock/unlock — 스레드
                         *  모드 잡 생성, overlap_check 뮤텍스(서로 다른 잡 간 io_u
                         *  겹침 검사 직렬화), PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
                         *  폴백, verify_async 스레드 소유. */

#ifdef CONFIG_LINUX
#include <linux/prctl.h>  /* [한국어] PR_SET_NAME 등 prctl 옵션 상수. Linux 전용
                           *  헤더. thread_main()이 PR_SET_NAME 으로 /proc/<pid>/comm
                           *  에 o->comm 을 기록해 top/ps 등에서 잡 이름을 보이게 함. */
#include <sys/prctl.h>    /* [한국어] prctl(2) 시스템 콜 프로토타입. PR_SET_NAME
                           *  인자 1(op), 인자 2(new-name) 로 스레드 이름 설정. */
#endif

/* fio 내부 헤더 파일들 */
#include "fio.h"            /* [한국어] fio 핵심 타입: struct thread_data, thread_options,
                             *  io_u, ioengine_ops, fio_sem, enum td_runstate(TD_NOT_CREATED
                             *  ~ TD_REAPED), enum fio_ddir/fio_q_status, TD_F_* 플래그,
                             *  IO_U_F_* 플래그, td_set_runstate/for_each_td/end_for_each,
                             *  td_io_queue/commit/getevents 프로토타입. 이 파일의
                             *  모든 핵심 심볼의 본원. */
#include "smalloc.h"        /* [한국어] smalloc/sfree/scalloc — 프로세스 간 공유
                             *  메모리(mmap MAP_SHARED) 할당기. cgroup_list,
                             *  inflight_numberio 배열 등 fork 자식이 접근해야 하는
                             *  데이터 전용. 일반 malloc 은 fork 후 COW 로 분리됨. */
#include "verify.h"         /* [한국어] VERIFY_NONE/CRC*/MD5/META/PATTERN/SHA* 열거,
                             *  verify_io_u/verify_io_u_async/populate_verify_io_u/
                             *  fill_verify_pattern/verify_save_state/verify_load_state/
                             *  verify_state_should_stop. do_verify() 및 do_io()의
                             *  TD_F_DO_VERIFY 분기가 사용. */
#include "diskutil.h"       /* [한국어] init_disk_util/update_io_ticks/
                             *  disk_util_prune_entries — /proc/diskstats 기반
                             *  블록 디바이스 I/O 통계 집계. 헬퍼 스레드가 주기 갱신. */
#include "cgroup.h"         /* [한국어] cgroup_setup/cgroup_kill/cgroup_shutdown —
                             *  o->cgroup 지정 시 /sys/fs/cgroup 밑에 잡 cgroup 생성
                             *  후 자식 PID 를 tasks 파일에 기록, 종료 시 정리. */
#include "profile.h"        /* [한국어] load_profile/profile_td_exit — tiobench,
                             *  act 등 미리 정의된 워크로드 프로파일 로드. fio_backend
                             *  선두에서 exec_profile 을 처리. */
#include "lib/rand.h"       /* [한국어] 난수 생성: __rand/__rand_0_1/frand_copy.
                             *  verify_state, poisson_state 시드 관리에 사용. */
#include "lib/memalign.h"   /* [한국어] fio_memalign/fio_memfree — 캐시라인 정렬된
                             *  메모리 할당. init_io_u()가 io_u 구조체를 64B/128B
                             *  정렬로 할당해 false-sharing 회피. */
#include "server.h"         /* [한국어] 서버 모드(fio --server) 관련: is_backend,
                             *  fio_server_got_signal/send_start/get_verify_state/
                             *  fio_clients_send_trigger. 백엔드 모드에서 SIGINT 등
                             *  을 네트워크로 클라이언트에 알림. */
#include "lib/getrusage.h"  /* [한국어] fio_getrusage — rusage_self 포함한 CPU
                             *  user/sys time 측정. update_rusage_stat/ts 에 반영. */
#include "idletime.h"       /* [한국어] fio_idle_prof_init/start/stop/cleanup —
                             *  I/O 잡 실행 전에 idle CPU 프로파일링 스레드를 띄워
                             *  백그라운드 간섭 없는 baseline을 측정하는 옵션 경로. */
#include "err.h"            /* [한국어] ERR_PTR/IS_ERR/IS_ERR_OR_NULL/PTR_ERR —
                             *  리눅스 커널 스타일의 에러 포인터 이디엄. get_io_u()가
                             *  에러 코드를 포인터 하위 비트에 인코딩해 반환할 때 사용. */
#include "workqueue.h"      /* [한국어] workqueue_init/enqueue/flush/exit —
                             *  io_submit_mode=IO_MODE_OFFLOAD 시 I/O 를 전용
                             *  스레드 풀에 위임해 잡 스레드는 get_io_u/put_io_u 만
                             *  수행하도록 분리. */
#include "lib/mountcheck.h" /* [한국어] device_is_mounted — check_mount_writes()
                             *  가 쓰기 워크로드에 마운트된 블록 디바이스를 대상으로
                             *  할 경우 실행 중단(FS 손상 방지). */
#include "rate-submit.h"    /* [한국어] rate_submit_init/exit — rate 또는 rate_iops
                             *  옵션 시 엄격한 속도 제어를 위해 별도 submit 스레드를
                             *  띄워 잡 스레드의 스케줄링 지터로부터 격리. */
#include "helper_thread.h"  /* [한국어] helper_thread_create/exit/destroy —
                             *  통계 라인 출력, 디스크 유틸 업데이트, steadystate
                             *  체크 등을 주기적으로 수행하는 글로벌 보조 스레드. */
#include "pshared.h"        /* [한국어] mutex_cond_init_pshared/cond_init_pshared —
                             *  PTHREAD_PROCESS_SHARED 속성 뮤텍스/조건변수 초기화.
                             *  use_thread=0(fork) 모드에서 부모-자식 공유 메모리에
                             *  배치된 동기화 객체는 반드시 pshared 여야 함. */
#include "zone-dist.h"      /* [한국어] td_zone_gen_index/td_zone_free_index —
                             *  zonemode=zbd 또는 존 분포(random_distribution=zoned) 시
                             *  잡이 사용할 존 인덱스 테이블 생성/해제. */
#include "fio_time.h"       /* [한국어] fio_gettime/fio_local_clock_init/mtime_since/
                             *  utime_since/utime_since_now/mtime_since_now/
                             *  usec_spin/usec_sleep/time_since_now/time_since_genesis.
                             *  이 파일의 모든 시간 측정 및 속도 제어의 근간. */

/* [한국어] ===== 전역 변수들 - 스레드 동기화 및 상태 관리 =====
 *
 * 이 파일의 static/전역은 대부분 run_threads()와 thread_main() 간의
 * 동기화를 위한 것이다. use_thread=0(fork) 모드에서는 fio_backend가
 * 호출되기 전(parse_options에서) smalloc 으로 thread_data 배열을 공유
 * 메모리에 생성해두므로, fork 된 자식 프로세스도 동일 변수를 본다.
 * extern 으로 선언된 thread_number/groupid 는 init.c 가 옵션 파싱 중에 세팅. */

static struct fio_sem *startup_sem;
/* [한국어] 모든 잡 스레드가 초기화 완료를 신호하는 공용 세마포어.
 * 설정자: fio_backend()가 fio_sem_init(FIO_SEM_LOCKED)로 생성.
 * 읽는 자: run_threads()가 fio_sem_down_timeout(startup_sem, 10000)으로 대기;
 *          thread_main()이 초기화 후 fio_sem_up(startup_sem)으로 알림.
 * 값 범위: LOCKED(0) ↔ UNLOCKED. 여러 잡이 순차 up 하므로 값>0 가능.
 * 동기화: 공유 메모리(smalloc)에 상주, fork 자식도 동일 세마포어를 만짐.
 * 생명주기: fio_backend() 선두에서 alloc, 후미 fio_sem_remove 로 해제. */

static struct flist_head *cgroup_list;
/* [한국어] 생성된 cgroup 엔트리 연결 리스트 헤드.
 * 설정자: fio_backend()가 smalloc 으로 할당 후 INIT_FLIST_HEAD.
 * 읽는 자: cgroup_setup()이 엔트리 추가, cgroup_kill()이 일괄 정리.
 * 값 범위: NULL(smalloc 실패 시) 또는 유효 flist_head.
 * 동기화: 리스트 조작은 cgroup 모듈 내부 뮤텍스로 보호. */

static struct cgroup_mnt *cgroup_mnt;
/* [한국어] cgroup 파일시스템 마운트 지점 정보 (예: /sys/fs/cgroup/blkio).
 * 설정자: 첫 cgroup_setup 호출 시 실제 마운트 지점을 탐지해 저장.
 * 읽는 자: 이후 cgroup_setup/cgroup_shutdown 호출이 재사용.
 * 값 범위: NULL(아직 탐지 전) 또는 {path, ver, ...} 구조체. */

static int exit_value;
/* [한국어] fio 프로세스 최종 종료 코드(main() 의 반환값으로 흘러감).
 * 설정자: sig_int()가 SIGINT 시 128 설정; 각 잡의 td->error 가 있으면
 *          reap_threads()가 ++exit_value; setup_files 실패 시에도 ++.
 * 읽는 자: fio_backend 리턴 시 이 값을 그대로 반환.
 * 값 범위: 0(성공) / 128(시그널) / 1+N(실패 잡 수 누적).
 * 동기화: reap_threads 는 메인 스레드에서만 접근하므로 락 불필요. */

static volatile bool fio_abort;
/* [한국어] 심각한 에러 시 통계 출력을 건너뛰도록 지시하는 플래그.
 * 설정자: run_threads()에서 startup_sem 타임아웃(10초) 또는 잡 시작 실패 시 true.
 * 읽는 자: fio_backend 후반에서 !fio_abort 인 경우에만 __show_run_stats 호출.
 * 값 범위: false(정상) / true(중단). volatile 인 이유는 시그널 핸들러가 읽을 수도 있기 때문. */

static unsigned int nr_process = 0;
/* [한국어] fork 기반으로 생성할 잡의 개수(use_thread=0 이 지정된 잡의 합).
 * 설정자: run_threads() 초반 for_each_td 루프에서 td->o.use_thread==0 카운트.
 * 읽는 자: "Starting N processes" 시작 메시지 출력에만 사용. */

static unsigned int nr_thread = 0;
/* [한국어] pthread 기반으로 생성할 잡의 개수(use_thread=1 이 지정된 잡의 합).
 * 설정자: run_threads() 초반 for_each_td 루프에서 td->o.use_thread==1 카운트.
 * 읽는 자: "Starting N threads" 시작 메시지 출력. */

/* [한국어] 집계된 I/O 로그 (읽기/쓰기/트림 방향별) */
struct io_log *agg_io_log[DDIR_RWDIR_CNT];
/* [한국어] write_bw_log 옵션 시 모든 잡의 BW/IOPS/lat 샘플을 합쳐 저장.
 * 설정자: fio_backend()가 write_bw_log 지정 시 setup_log()로 3개(R/W/T) 생성.
 * 읽는 자: 잡 스레드들이 add_agg_sample()로 기록, 종료 시 flush_log/free_log.
 * 인덱스: DDIR_READ=0, DDIR_WRITE=1, DDIR_TRIM=2 (DDIR_RWDIR_CNT=3).
 * 동기화: smalloc 공유 메모리 + 내부 뮤텍스로 잡 간 동시 append 안전. */

/* [한국어] 전역 상태 변수들 — 이 파일 바깥(init.c, stat.c 등)에서도 참조됨 */
int groupid = 0;
/* [한국어] 현재 잡에 할당할 그룹 ID. stonewall 또는 new_group 옵션을 만나면 ++.
 * 설정자: init.c 의 add_job/parse_jobs 가 옵션 해석 중 증가.
 * 읽는 자: 통계 출력(group reporting), reap_threads의 rate 합산. */

unsigned int thread_number = 0;
/* [한국어] 전체 잡 수 (use_thread 와 fork 모두 포함). thread_data 배열의 유효 길이.
 * 설정자: init.c 의 add_job 이 parse 중 ++. parse_options 완료 시 최종값.
 * 읽는 자: fio_backend/run_threads가 todo 초기값, for_each_td 순회 상한으로 사용.
 * 값 범위: 0(잡 없음) ~ REAL_MAX_JOBS. 0 이면 fio_backend 즉시 반환. */

unsigned int nr_segments = 0;
/* [한국어] 전체 thread_data 세그먼트 수(대용량 서버 모드에서 배열을 여러 블록으로 나눔).
 * 설정자: init.c 의 fio_init_options 가 smalloc 블록마다 ++.
 * 읽는 자: sig_int 는 nr_segments>0 인지만 확인(잡 파싱이 끝났는지 판단). */

unsigned int cur_segment = 0;
/* [한국어] 현재 기록 중인 세그먼트의 인덱스. 잡 추가 중 증가. */

unsigned int stat_number = 0;
/* [한국어] 통계 그룹 번호 — 여러 fio 실행이 합쳐질 때(예: 서버가 누적) 구분자. */

int temp_stall_ts;
/* [한국어] 출력 일시 정지 플래그 (print_status_init/process 가 사용).
 * true 이면 주기적 상태 라인 출력을 일시 억제(verbose 요청 직후 등). */

unsigned long done_secs = 0;
/* [한국어] 지금까지 완료된 잡들의 실행 시간 합계(초). reap_threads가 각 잡 회수 시
 *          mtime_since_now(&td->epoch)/1000 을 더해 누적. 통계 라인 ETA 계산에 사용. */

/* [한국어] 오버랩 체크용 뮤텍스 - 오프로드 모드에서 잡 간 io_u 영역 겹침 검사 시 사용.
 * 설정자: 정적 초기화자(에러체크 가능 시 NP 버전, 아니면 표준).
 * 읽는 자: thread_main()이 td_offload_overlap(td) 시 cleanup 직전 lock/unlock
 *          하여 다른 잡이 우리 io_u 를 in_flight_overlap 검사 중에 정리하지 않게 함.
 * 값 범위: PTHREAD_MUTEX_INITIALIZER로 초기화된 프로세스 공유 아닌 mutex.
 * 동기화: 동일 프로세스 내 스레드 간에서만 유의미(fork 자식 공유 X). */
#ifdef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
pthread_mutex_t overlap_check = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
/* [한국어] glibc 비표준 확장: 재귀 lock 등 에러를 즉시 EDEADLK 로 반환 — 디버깅 용이. */
#else
pthread_mutex_t overlap_check = PTHREAD_MUTEX_INITIALIZER;
/* [한국어] POSIX 표준 폴백: 에러 체크 없음. 동일 스레드 재진입 시 UB. */
#endif

extern char *write_bw_log_name;
/* [한국어] --write_bw_log=<name> 옵션에 지정된 로그 파일명 접두사.
 * 정의 위치: init.c. 사용 위치: fio_backend의 agg_io_log 파일명 구성
 *            (<name>-read_bw.log / -write_bw.log / -trim_bw.log). */

/* [한국어] 작업 시작 대기 타임아웃: 5초 (밀리초 단위).
 * 의미: run_threads가 한 배치의 잡들이 TD_INITIALIZED 로 올라올 때까지 기다리는 시간.
 *       초과 시 해당 배치의 잡에 SIGTERM 을 보내 실패 처리. */
#define JOB_START_TIMEOUT	(5 * 1000)

/*
 * [한국어]
 * sig_int - SIGINT(Ctrl+C) 및 SIGTERM(kill), 백엔드 모드에서는 SIGPIPE 까지 처리
 *
 * @sig: 수신된 시그널 번호 (SIGINT/SIGTERM/SIGPIPE).
 * @return: 없음 (시그널 핸들러).
 *
 * 왜 필요한가: 사용자가 실행 중 종료하고 싶을 때, 진행 중인 I/O 를 우아하게
 *   중단하면서도 지금까지의 통계를 잃지 않고 보여주기 위해 필요하다. 그냥
 *   프로세스가 SIGINT 로 죽으면 커널이 열린 파일만 닫을 뿐, fio 의 통계/로그
 *   flush/엔진 cleanup 은 건너뛰게 되어 사용자는 결과를 보지 못한다.
 *
 * 동작: nr_segments>0 (즉, 잡 파싱이 끝나 thread_data 가 유효한 상태) 에만
 *   동작한다. 파싱 도중 SIGINT 이면 호출되어도 아무 일 하지 않고 main() 이
 *   정상 경로로 0 종료한다(백엔드 기본 exit_value=0 유지).
 *   1) 백엔드(서버) 모드: fio_server_got_signal() 으로 네트워크 피어에게 알림.
 *      서버가 TCP 로 연결된 클라이언트에 "종료 중" 을 전달.
 *   2) 로컬 모드: 표준 에러(실제로는 log_info=stdout) 에 이유 출력, 버퍼
 *      플러시, exit_value=128(POSIX 관례: 128+SIGNUM 근접)을 지정.
 *   3) fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL) 로 모든 잡의
 *      td->terminate 플래그를 세움(공유 메모리). 각 잡 스레드는 I/O 루프의
 *      매 반복에서 이 플래그를 체크하여 탈출한다.
 *
 * 실행 컨텍스트: 메인 스레드가 등록한 시그널 핸들러지만, 리눅스에서는 임의
 *   스레드가 받을 수 있다. async-signal-safe 하지 않은 함수(log_info 등)를
 *   호출한다 — 엄격하게는 위험하지만 fio 는 "종료 수순을 돕는" 용도로 용인.
 *
 * 호출 체인:
 *   사용자 Ctrl+C / kill <pid> → 커널 → sigaction 등록된 sig_int →
 *     [서버?] fio_server_got_signal() [server.c]
 *     [로컬?] log_info/log_info_flush
 *     공통: fio_terminate_threads() → 각 td->terminate=1 세팅
 *       → do_io()/do_verify() 루프가 다음 반복에서 break
 */
static void sig_int(int sig)
{
	/* [한국어] 잡이 아직 파싱되지 않은 극초반 시그널은 무시. */
	if (nr_segments) {
		/* [한국어] 백엔드(서버) 모드: 연결된 클라이언트에게 시그널 수신을 알림.
		 * fio_server_got_signal 은 서버 측 pipe/socket에 특정 바이트를 보내
		 * 이벤트 루프를 깨운다. */
		if (is_backend)
			fio_server_got_signal(sig);
		else {
			/* [한국어] 로컬 모드: stderr 성 출력(log_info 는 stdout 이지만
			 * 잡 완료 전 종료는 예외 상황으로 명시). */
			log_info("\nfio: terminating on signal %d\n", sig);
			/* [한국어] 출력 버퍼를 강제로 내보냄 — 비정상 종료로 버퍼가
			 * 유실되는 것을 방지. */
			log_info_flush();
			/* [한국어] exit 코드 128 — POSIX 관례상 "시그널에 의한 종료" 를
			 * 의미. 쉘 $? 로 확인 가능. */
			exit_value = 128;
		}

		/* [한국어] 모든 잡 스레드에 종료 전파: TERMINATE_ALL 은 그룹/잡을
		 * 특정하지 않고 전체 thread_data[]의 td->terminate 를 세팅.
		 * 각 잡은 do_io/do_verify 루프에서 이를 체크해 빠져나와 cleanup 수행. */
		fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
	}
}

#ifdef WIN32
/*
 * [한국어]
 * sig_break - Windows SIGBREAK(Ctrl+Break 또는 console close) 핸들러
 *
 * @sig: 수신된 시그널 번호 (SIGBREAK).
 * @return: 없음.
 *
 * 왜 필요한가: Windows 런타임은 SIGBREAK 핸들러가 리턴하는 "즉시" 모든 자식
 *   프로세스를 강제 종료해 버린다(POSIX 의 SIGHUP 수준 강제성). 따라서
 *   sig_int 만 호출하고 리턴하면 잡 스레드들이 cleanup 을 마치기도 전에
 *   사라져 통계/로그가 소실된다. 이를 방지하기 위해 이 핸들러는 모든 잡이
 *   TD_EXITED 상태가 될 때까지 1초 폴링으로 기다린 후에야 리턴한다.
 *
 * 동작:
 *   1) sig_int(sig) 호출 — 공통 로직: 종료 메시지, terminate 플래그 전파.
 *   2) for_each_td 로 모든 잡을 순회하며 td->runstate >= TD_EXITED 가
 *      될 때까지 sleep(1) 반복.
 *
 * 호출 체인: Windows 콘솔 close / Ctrl+Break → sig_break → sig_int →
 *   fio_terminate_threads → (polling) → return → Windows 가 프로세스 종료.
 *
 * 플랫폼: #ifdef WIN32 로 감싸져 Linux 빌드에서는 컴파일되지 않음.
 */
static void sig_break(int sig)
{
	sig_int(sig);  /* [한국어] 공통 종료 처리 수행 */

	/**
	 * Windows terminates all job processes on SIGBREAK after the handler
	 * returns, so give them time to wrap-up and give stats
	 */
	/* [한국어] 핸들러 리턴 전에 모든 잡이 자체 정리를 마칠 때까지 폴링.
	 * sleep(1) 은 1초 단위라 잡 수가 많거나 flush 가 느리면 전체 대기가
	 * 길어질 수 있으나, 사용자 눈에는 Ctrl+Break 후 잠깐 멈췄다가 정상 종료. */
	for_each_td(td) {
		while (td->runstate < TD_EXITED)
			sleep(1);
	} end_for_each();
}
#endif

/*
 * [한국어]
 * sig_show_status - SIGUSR1 핸들러 (실시간 통계 덤프 트리거)
 *
 * @sig: 수신된 시그널 번호(실제로는 미사용 — 핸들러 시그니처 호환용).
 * @return: 없음.
 *
 * 왜 필요한가: 장시간 실행 중에 "현재까지의 BW/IOPS" 를 보고 싶을 때
 *   Ctrl+C 로 중단하지 않고도 kill -USR1 <pid> 로 즉시 상태를 덤프받기
 *   위함. show_running_run_stats()는 현재 thread_data[] 를 스냅샷해 출력.
 *
 * 호출 체인: kill -USR1 → 커널 → 메인 스레드 인터럽트 → sig_show_status →
 *   show_running_run_stats [stat.c] → log_info 로 BW/IOPS 라인 덤프.
 *
 * 주의: show_running_run_stats 내부에서 뮤텍스를 잡을 수 있어 strictly
 *   async-signal-safe 하지 않으나, 실용적 관점에서 허용.
 */
void sig_show_status(int sig)
{
	show_running_run_stats();  /* [한국어] stat.c 로 위임 — 모든 잡 스냅샷 후 출력 */
}

/*
 * [한국어]
 * set_sig_handlers - 시그널 핸들러 일괄 등록
 *
 * @return: 없음.
 *
 * 왜 필요한가: fio 는 I/O 를 수행하는 긴 실행 프로세스이므로 사용자 제어
 *   (Ctrl+C, kill), 상태 조회(USR1), 그리고 서버 모드의 broken pipe (SIGPIPE)
 *   에 대응해야 한다. 이 함수는 run_threads() 선두에서 한 번만 호출되어
 *   모든 시그널을 sigaction(2) 으로 등록한다.
 *
 * 동작: struct sigaction act 를 재사용하며(재설정 전 memset 필수), 각
 *   시그널마다 다음을 지정:
 *     - sa_handler : 호출될 함수 포인터
 *     - sa_flags=SA_RESTART : 시그널 수신으로 인터럽트된 "재시작 가능" 시스템
 *       콜(예: read/write/nanosleep)을 커널이 자동 재시도하게 함. 없으면
 *       do_io 루프의 syscall 이 EINTR 로 빠져 에러로 오해될 수 있음.
 *
 * 등록 목록:
 *   SIGINT   → sig_int          (Ctrl+C)
 *   SIGTERM  → sig_int          (kill)
 *   SIGBREAK → sig_break        (Windows 전용)
 *   SIGUSR1  → sig_show_status  (kill -USR1)
 *   SIGPIPE  → sig_int          (is_backend 일 때만. 서버 소켓 상대측이
 *              끊기면 write 가 SIGPIPE 를 유발 — 기본 행동은 프로세스
 *              종료이지만, 우리는 우아하게 잡을 정리하고 exit 하고 싶음.)
 *
 * 호출 체인: run_threads() 초반 → set_sig_handlers().
 */
static void set_sig_handlers(void)
{
	struct sigaction act;

	/* [한국어] ---- SIGINT (Ctrl+C) ---- */
	memset(&act, 0, sizeof(act));  /* [한국어] sa_mask 등 모든 필드 0 클리어 */
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;     /* [한국어] 인터럽트된 syscall 자동 재시작 */
	sigaction(SIGINT, &act, NULL); /* [한국어] NULL=oldact 저장 안 함 */

	/* [한국어] ---- SIGTERM (kill 기본 시그널) ---- */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &act, NULL);

/* Windows uses SIGBREAK as a quit signal from other applications */
#ifdef WIN32
	/* [한국어] ---- Windows SIGBREAK ----
	 * Ctrl+Break 또는 cmd 창 닫기에 의해 발생. 핸들러가 완료 대기를 수행. */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_break;
	act.sa_flags = SA_RESTART;
	sigaction(SIGBREAK, &act, NULL);
#endif

	/* [한국어] ---- SIGUSR1: 실행 중 통계 덤프 ---- */
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_show_status;
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);

	/* [한국어] ---- 서버 모드: SIGPIPE ----
	 * 기본 동작은 프로세스 강제 종료이지만, fio 서버는 클라이언트 연결이
	 * 끊겼을 때도 이미 시작된 로컬 잡들을 우아하게 마무리해야 한다. */
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
 * [한국어]
 * __check_min_rate - 단일 방향(ddir)의 최소 처리량 조건 검사
 *
 * @td:     잡의 thread_data. ratemin/rate_iops_min/ratecycle 옵션 참조,
 *          this_io_bytes/this_io_blocks 통계와 last_rate_check_* 상태 갱신.
 * @now:    현재 시각(fio_gettime 으로 채운 timespec).
 * @ddir:   READ/WRITE/TRIM 중 하나. assert(ddir_rw(ddir)) — SYNC 류 금지.
 * @return: true=미달(에러로 처리) / false=정상 또는 검사 불필요.
 *
 * 왜 필요한가: 최소 성능 SLA 가 있는 벤치마크(예: "쓰기 50MB/s 이상 유지")
 *   에서 fio 가 조건 위반 시 즉시 중단하도록. 일시적 지연은 ratecycle (기본
 *   1초) 주기로 평균화.
 *
 * 동작:
 *   1) ratemin/rate_iops_min 둘 다 0 이면 검사 OFF → false.
 *   2) 시작(td->start) 후 2초 미만이면 워밍업 — 검사 생략 → false.
 *   3) last_rate_check_* 가 세팅되어 있으면(두 번째 이상 검사):
 *      경과시간 spent=mtime_since(last, now). spent<ratecycle 이면 false.
 *      - ratemin 모드: current_rate_bytes=(delta_bytes*1000)/spent.
 *        < option_rate_bytes_min 이면 log_err + true.
 *      - rate_iops_min 모드: delta_blocks*1000/spent 로 IOPS 계산 후 비교.
 *   4) 스냅샷 갱신: last_rate_check_bytes/blocks/time ← 현재값, false 반환.
 *
 * 실행 컨텍스트: 잡 스레드. do_io 루프 후반 check_min_rate 에서 한 방향씩 호출.
 *
 * 호출 체인: do_io() → check_min_rate() → __check_min_rate().
 *
 * 에러 경로: 미달 판정 시 상위 check_min_rate 가 td_verror(EIO, "check_min_rate")
 *   를 호출해 루프를 종료시킴. exitall_on_terminate 면 fio_terminate_threads 도.
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
 * [한국어]
 * check_min_rate - 모든 활성 방향에 대해 __check_min_rate 일괄 검사
 *
 * @td:  잡의 thread_data.
 * @now: 현재 시각.
 * @return: true=하나라도 미달(루프 종료 필요) / false=모두 정상.
 *
 * 왜 필요한가: 잡은 read+write 혼합이 가능하며 각 방향별로 ratemin 이 다를
 *   수 있다. 이 함수가 for_each_rw_ddir 로 모든 방향을 검사하여 전체 SLA 판정.
 *
 * 동작: bytes_done[ddir]>0 인 방향만 __check_min_rate 수행(아직 I/O 없는
 *   방향은 체크 불가). OR 로 누적.
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
 * [한국어]
 * fio_io_sync - 엔진에 DDIR_SYNC/DDIR_SYNCFS 를 동기(blocking) 발행
 *
 * @td:    잡의 thread_data.
 * @f:     동기화 대상 파일(열려 있어야 함. 호출자가 보장).
 * @ddir:  DDIR_SYNC (파일 fsync) 또는 DDIR_SYNCFS (파일시스템 syncfs).
 * @return: true=에러(io_u 할당 실패 / prep 실패 / queue 에러 / 완료 에러),
 *          false=성공.
 *
 * 왜 필요한가: end_fsync/fsync_on_close/do_verify 등에서 "직전까지의 쓰기를
 *   디스크에 확실히 내리는" 작업이 필요하다. 이를 엔진의 정식 queue 경로로
 *   수행하면 엔진 특화 sync 구현(예: io_uring 의 IORING_OP_FSYNC, libaio 의
 *   sync 폴백)이 그대로 사용되고 통계/로그도 일관되게 기록된다.
 *
 * 동작:
 *   1) __get_io_u 로 freelist 에서 io_u 하나 할당.
 *   2) io_u->ddir=ddir, io_u->file=f, IO_U_F_NO_FILE_PUT (f 의 참조 카운트
 *      을 이 io_u 가 해제하지 않도록) 설정.
 *   3) td_io_prep → 실패 시 put_io_u + true.
 *   4) requeue 레이블에서 td_io_queue.
 *      - FIO_Q_QUEUED: td_io_commit + io_u_queued_complete(1).
 *      - FIO_Q_COMPLETED: io_u->error 검사 후 io_u_sync_complete.
 *      - FIO_Q_BUSY: commit 후 goto requeue (엔진이 수락할 때까지).
 *
 * 실행 컨텍스트: 잡 스레드. do_verify/do_io(end_fsync) 로부터 동기 호출 —
 *   완료 전까지 리턴하지 않음.
 *
 * 호출 체인:
 *   fio_file_fsync / do_io(end_fsync) / fio_syncfs / do_verify
 *     → fio_io_sync(td, f, ddir) → td_io_queue → 엔진 .queue (DDIR_SYNC 경로).
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
 * [한국어]
 * fio_file_fsync - 개별 파일에 DDIR_SYNC 수행(열기/닫기까지 책임)
 *
 * @td: 잡의 thread_data.
 * @f:  대상 파일. 열려 있지 않아도 됨.
 * @return: 0=성공, 1=실패(열기/sync/닫기 중 하나라도 실패).
 *
 * 왜 필요한가: do_io 의 end_fsync 경로에서 모든 파일을 순회하며 fsync 하려
 *   할 때, 어떤 파일은 닫혀 있을 수 있다(예: open/close 가 io_u 단위로 반복).
 *   이 함수는 상태에 관계없이 "열고, sync 하고, 닫는" 삼단계를 보장.
 *
 * 동작: fio_file_open(f) 이면 바로 fio_io_sync(td, f, DDIR_SYNC). 아니면
 *   td_io_open_file → fio_io_sync → td_io_close_file 순. ret, ret2 로 두
 *   에러를 결합해 반환.
 *
 * 실행 컨텍스트: 잡 스레드. do_io 의 end_fsync 루프, do_verify 의 pre-sync 루프.
 *
 * 호출 체인: do_io()/do_verify() → for_each_file → fio_file_fsync
 *   → td_io_open_file/fio_io_sync/td_io_close_file.
 *
 * 에러 경로: open 실패 → 즉시 1 반환. sync 또는 close 실패 → 각 ret 비트합.
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
 * [한국어]
 * fio_syncfs - 잡이 접근한 모든 파일시스템에 syncfs(2) 발행
 *
 * @td: 잡의 thread_data. td->fs_list (struct fio_mount 리스트) 를 순회.
 * @return: 0=성공 / -1=일부 실패(하지만 루프 계속) / -ENOSYS=CONFIG_SYNCFS
 *          미지원 플랫폼(예: Windows/Solaris 일부).
 *
 * 왜 필요한가: end_syncfs 옵션 사용 시 개별 파일 fsync 가 아닌 파일시스템
 *   전체 메타+데이터를 한 번에 디스크에 내리고 싶을 때. 수많은 파일에 대한
 *   fsync 를 반복하는 것보다 빠를 수 있다.
 *
 * 동작: td->fs_list 순회 — 각 fio_mount 에 대해 fio_open_fs → fio_io_sync
 *   (DDIR_SYNCFS) → fio_close_fs. 내부적으로 엔진의 queue 콜백이
 *   DDIR_SYNCFS 를 받아 syncfs(2) 시스템콜을 실행.
 *
 * 실행 컨텍스트: 잡 스레드. do_io end_fsync 경로.
 *
 * 호출 체인: do_io(end_syncfs) → fio_syncfs → fio_open_fs + fio_io_sync +
 *   fio_close_fs. 내부: td_io_queue → 엔진 .queue 에서 syncfs(2).
 *
 * 에러 경로: 중간 실패 시 err=-1 설정 후 continue — 나머지 FS 도 시도.
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
 * [한국어]
 * __update_ts_cache - td->ts_cache 를 무조건 현재 시각으로 갱신
 *
 * @td: 잡의 thread_data.
 * @return: 없음.
 *
 * 왜 필요한가: do_io 루프의 각 반복에서 fio_gettime(clock_gettime/rdtsc) 을
 *   직접 호출하면 고 IOPS 워크로드에서 무시할 수 없는 오버헤드가 된다.
 *   대신 반복마다 update_ts_cache(마스크 기반) 로 "가끔" 갱신하고,
 *   정확성이 필요한 경로(runtime_exceeded 재확인 등)에서 이 __ 접두 버전을
 *   강제 호출해 즉시 갱신한다.
 *
 * 실행 컨텍스트: 잡 스레드. do_io/do_verify 의 시간 검사 지점.
 *
 * 호출 체인: do_io/do_verify → update_ts_cache(마스크 합격 시) / 또는
 *   runtime_exceeded 분기에서 __update_ts_cache 직접.
 */
static inline void __update_ts_cache(struct thread_data *td)
{
	fio_gettime(&td->ts_cache, NULL);
}

/*
 * [한국어]
 * update_ts_cache - ts_cache_mask 주기로 가벼운 시각 갱신
 *
 * @td: 잡의 thread_data. ts_cache_nr(카운터) 증가, ts_cache_mask 로 샘플링.
 *
 * 왜 필요한가: 위 __update_ts_cache 의 주석 참조. 매 I/O 의 fio_gettime 오버헤드
 *   를 피하기 위해 "약 N 회에 1 번" 만 실제 시각을 갱신한다.
 *
 * 동작: ++ts_cache_nr & mask == mask 일 때만 __update_ts_cache 호출.
 *   mask 는 2^k - 1 형태의 비트마스크로 샘플링 주기 N=2^k.
 */
static inline void update_ts_cache(struct thread_data *td)
{
	if ((++td->ts_cache_nr & td->ts_cache_mask) == td->ts_cache_mask)
		__update_ts_cache(td);
}

/*
 * [한국어]
 * runtime_exceeded - runtime 옵션 경과 여부 판정
 *
 * @td: 잡의 thread_data. in_ramp_period / o.timeout / epoch 참조.
 * @t:  비교용 시각(ts_cache 또는 실제 fio_gettime 값).
 * @return: true=timeout 초과 / false=램프 구간이거나 아직 남음.
 *
 * 왜 필요한가: time_based / runtime 옵션 지원. 주의: ramp_time(워밍업) 동안은
 *   무조건 false 를 반환해 실제 측정 이전에는 루프가 끝나지 않도록 한다.
 *   에폭(td->epoch)은 set_epoch_time 에서 설정되며 workload 시작점.
 *
 * 실행 컨텍스트: 잡 스레드 do_io/do_verify 매 반복.
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
 * [한국어]
 * update_runtime - 방향별 누적 런타임을 μs 단위로 추적해 ms 로 보고
 *
 * @td:         잡의 thread_data. ts.runtime[ddir] (ms) 갱신.
 * @elapsed_us: [in/out] 호출자 로컬 배열. 이전 μs 값을 기억해두고 델타 계산.
 * @ddir:       READ/WRITE/TRIM.
 *
 * 왜 필요한가: ts.runtime 은 최종 통계 라인에서 ms 로 표기되지만, 잡이 매우
 *   짧으면 ms 해상도로는 부정확하다. 내부 추적은 μs 해상도로 유지하고,
 *   ms 로 변환할 때 (x + 999) / 1000 반올림을 적용해 누적 오차를 줄인다.
 *   또한 루프를 여러 번 도는 경우 이전 μs 값을 빼고 새 값을 더해 누적 오차 방지.
 *
 * 동작:
 *   - verify_only 모드에서는 write 방향 런타임 갱신 생략 (실제 write 안 함).
 *   - runtime[ddir] -= round_up(old_elapsed_us, 1000).
 *   - elapsed_us[ddir] += utime_since_now(&td->start).
 *   - runtime[ddir] += round_up(new_elapsed_us, 1000).
 *
 * 실행 컨텍스트: 잡 스레드 thread_main 외부 루프에서 stat_sem 보호 구간.
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
 * [한국어]
 * break_on_this_error - 에러 발생 시 I/O 루프를 중단할지 결정
 *
 * @td:     잡의 thread_data. td->error/o.continue_on_error/o.fill_device 참조.
 * @ddir:   방금 수행한 I/O 의 방향(READ/WRITE/TRIM). td_error_type 인자로 사용.
 * @retptr: [in/out] *retptr 은 td_io_queue 의 음수(-errno) 또는 0 이상.
 *          비치명적 에러로 판정되면 이 함수가 *retptr=0 으로 클리어해 호출자
 *          가 루프를 계속하도록 만든다.
 * @return: true = 루프 중단 필요(치명적 에러 또는 fill_device 종료 조건),
 *          false = 계속 진행(정상 / 비치명적 에러 후 복구).
 *
 * 왜 필요한가: 실제 대규모 스토리지에서는 일시적 에러(EAGAIN/EBUSY 등)나
 *   기대된 에러(fill_device 중 ENOSPC)가 발생할 수 있다. 사용자가
 *   continue_on_error 옵션으로 어떤 에러 유형을 "무시하고 계속" 할지 지정할
 *   수 있으며, 이 함수는 해당 분기를 한곳에서 수행한다.
 *
 * 동작:
 *   - ret<0 또는 td->error 면 에러 유형(eb) 을 td_error_type 으로 분류.
 *   - continue_on_error 비트(1<<eb) 가 없으면 바로 true(치명적) 반환.
 *   - td_non_fatal_error: update_error_count, td_clear_error, *retptr=0, false.
 *   - fill_device + ENOSPC/EDQUOT: 정상 종료로 처리 — td_clear_error +
 *     fio_mark_td_terminate, true 반환.
 *   - 그 외: update_error_count 후 true(치명적 에러, 루프 종료).
 *
 * 호출 체인: do_io/do_verify → io_queue_event → break_on_this_error.
 *
 * 에러 경로: td_verror 는 이 함수가 부르지 않는다(호출자가 이미 했거나
 *   td->error 에 기록됨). 여기서는 분류/클리어 만 수행.
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
 * [한국어]
 * check_update_rusage - 부모가 요청한 CPU usage 스냅샷을 응답
 *
 * @td: 잡의 thread_data.
 *
 * 왜 필요한가: helper_thread 나 stat.c 가 주기적으로 잡의 rusage (user/system
 *   CPU time) 를 읽고 싶지만, fork 모드에서는 부모가 자식의 rusage 를 직접
 *   읽을 방법이 제한적이다. 이에 td->update_rusage=1 플래그로 "측정해 달라"
 *   요청하고, 잡 스레드가 do_io 루프에서 이를 감지해 update_rusage_stat 후
 *   rusage_sem 을 up 하여 응답하는 반동기(poll) 프로토콜을 사용한다.
 *
 * 동작: td->update_rusage=0 으로 클리어 → update_rusage_stat(td) → fio_sem_up(rusage_sem).
 *
 * 실행 컨텍스트: 잡 스레드 do_io/do_verify 루프 안. rusage_sem 기다리는 쪽은
 *   별도 스레드(통계 수집측).
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
 * [한국어]
 * wait_for_completions - 인플라이트 I/O 완료 수거를 iodepth_low 까지 반복
 *
 * @td:    잡의 thread_data. cur_depth 가 변화한다.
 * @time:  완료 시점 기록용 timespec 포인터(NULL 가능). should_check_rate 일 때만 기록.
 * @return: 마지막 io_u_queued_complete 반환값. 양수=수거한 이벤트 수, 음수=에러.
 *
 * 왜 필요한가: 비동기 엔진(libaio/io_uring)에서는 iodepth 까지 I/O 를 쌓은
 *   뒤에도 계속 새 I/O 를 추가하면 커널 큐가 넘친다. 큐가 full 이면 반드시
 *   최소 1 개 이상의 완료를 수거하여 공간을 확보해야 한다. 또한 로그 확장
 *   (TD_F_REGROW_LOGS) 이 필요하면 모든 in-flight 를 잠시 quiesce 해야 한다.
 *
 * 동작:
 *   1) TD_F_REGROW_LOGS 면 io_u_quiesce 로 모든 in-flight 완료 후 리턴.
 *   2) min_evts = min(iodepth_batch_complete_min, cur_depth). full 이면
 *      최소 1 보장. should_check_rate 이면 time 기록.
 *   3) while (full && cur_depth > iodepth_low):
 *        io_u_queued_complete(td, min_evts) 호출 — td_io_getevents + 각 io_u
 *        에 대해 io_completed/put_io_u 수행 — 실패 시 즉시 리턴.
 *
 * 실행 컨텍스트: 잡 스레드. do_io/do_verify 의 reap 경로에서만 호출.
 *
 * 호출 체인:
 *   do_io()/do_verify() → wait_for_completions(td, time)
 *     → io_u_quiesce [io_u.c] (regrow 경로)
 *     → io_u_queued_complete [io_u.c] → td_io_getevents [ioengines.c]
 *
 * 에러 경로: io_u_queued_complete 음수 반환 시 do 루프 탈출, 그 값이 호출자에게.
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
 * [한국어]
 * io_queue_event - td_io_queue() 반환값에 따른 후처리 허브
 *
 * @td:           잡의 thread_data.
 * @io_u:         td_io_queue 에 넘긴 I/O 유닛. in_flight 상태이거나, COMPLETED
 *                에서 에러/resid 면 재큐잉/무효화 대상.
 * @ret:          [in/out] td_io_queue 반환값 포인터. FIO_Q_COMPLETED(0),
 *                FIO_Q_QUEUED(1), FIO_Q_BUSY(2), 또는 음수(-errno). 에러 분기
 *                후 break_on_this_error 에서 *retptr 을 0 으로 재설정할 수 있음.
 * @ddir:         회계 목적의 방향(read/write/trim). acct_ddir() 결과.
 * @bytes_issued: [in/out] do_io 에서 누적하는 "발행 바이트". QUEUED 경로와
 *                COMPLETED 의 resid 부분 완료에서 증가. NULL 가능(do_verify).
 * @from_verify:  1 이면 do_verify 에서 호출 — trim_io_piece 호출 억제, error
 *                계속 조건 생략(검증은 한 번의 read 실패가 곧 실패 알림이므로).
 * @comp_time:    완료 시각 저장소(NULL 가능). should_check_rate 일 때만 기록.
 * @return:       0 = 호출자 루프 계속, 1 = 에러로 인해 호출자가 루프 중단해야 함.
 *
 * 왜 필요한가: I/O 엔진들은 queue 콜백에서 서로 다른 "반환 의미" 를 가진다:
 *   sync 엔진은 즉시 완료(FIO_Q_COMPLETED), libaio/io_uring 은 큐잉만
 *   (FIO_Q_QUEUED), 자원 부족 시 FIO_Q_BUSY 로 백프레셔를 준다. 이 반환을
 *   추상화해 회계(bytes_issued/resid/short I/O/trim 로그), 재큐잉, 에러
 *   분기를 한곳에서 처리하는 것이 이 함수의 목적.
 *
 * 동작 상세:
 *   - FIO_Q_COMPLETED:
 *       * io_u->error 있으면: *ret = -errno, invalidate_inflight, clear_io_u.
 *       * io_u->resid 있으면(short I/O): xfer_buflen-resid 만큼만 완료.
 *         - resid 전체가 0 이면 "zero read" 로 간주 → EIO 로 실패 처리.
 *         - 일부 전송이면 버퍼/offset 을 진행시키고 requeue_io_u 로 재제출.
 *         - 파일 끝 도달 시 sync_done 라벨로 점프해 정상 완료 처리.
 *       * 정상: io_u_sync_complete 로 통계 누적 + freelist 반환.
 *       * TD_F_REGROW_LOGS: 로그 확장 필요 시 regrow_logs 호출.
 *   - FIO_Q_QUEUED:
 *       * 엔진에 commit 훅이 없으면(sync 계열) io_u_queued 직접 호출.
 *       * bytes_issued 에 xfer_buflen 누적.
 *   - FIO_Q_BUSY:
 *       * unlog_io_piece 로 write iolog 에서 미전송 항목 제거 (verify 제외).
 *       * requeue_io_u 후 td_io_commit 로 엔진에 밀어넣기 힌트.
 *   - default (음수 = 에러):
 *       * assert(*ret < 0), td_verror 로 에러 코드 전파.
 *
 * 실행 컨텍스트: 잡 스레드. do_io/do_verify 메인 루프에서 매 I/O 제출 후 호출.
 *
 * 호출 체인:
 *   do_io()/do_verify()
 *     → td_io_queue → io_queue_event(이 함수)
 *         → io_u_sync_complete / requeue_io_u / td_io_commit / td_verror
 *         → break_on_this_error 로 최종 판정
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
 * [한국어]
 * io_in_polling - 폴링(pollmode) 모드 여부
 *
 * @td: 잡의 thread_data.
 * @return: true=폴링(매 반복 완료 수거), false=배치 모드.
 *
 * 왜 필요한가: iodepth_batch_complete_min/max 둘 다 0 이면 "가능한 즉시 완료
 *   수거" 모드이다. 이 경우 full 여부와 관계없이 매번 완료를 확인한다
 *   (do_io 의 "full || io_in_polling" 분기).
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
 * [한국어]
 * unlink_all_files - 잡 소유 파일(FIO_TYPE_FILE)을 모두 unlink(2)
 *
 * @td: 잡의 thread_data.
 * @return: 0=성공, 양수=에러(td_io_unlink_file 실패).
 *
 * 왜 필요한가: unlink_each_loop 옵션은 루프마다 파일을 삭제 후 재생성하도록
 *   하여 "매번 새 파일에 쓰는" 시나리오를 시뮬레이션한다. 블록 디바이스
 *   (FIO_TYPE_BLOCK/CHAR/PIPE) 는 삭제 대상이 아니므로 건너뜀.
 *
 * 동작: for_each_file: filetype 확인 후 td_io_unlink_file 호출. 실패 시
 *   즉시 break + td_verror 로 에러 기록 후 반환.
 *
 * 실행 컨텍스트: 잡 스레드. thread_main 루프 반복 시작부.
 *
 * 호출 체인: thread_main → unlink_all_files → td_io_unlink_file [ioengines.c]
 *   (엔진 .unlink_file 콜백, 보통 generic_unlink_file → unlink(2)).
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
 * [한국어]
 * in_flight_overlap - 새 io_u 와 현재 in-flight io_u 간 오프셋 구간 겹침 검사
 *
 * @q:    td->io_u_all 큐(잡 소유 모든 io_u 배열).
 * @io_u: 검사 대상 신규 io_u (아직 큐잉 전).
 * @return: true=겹침 발견, false=충돌 없음.
 *
 * 왜 필요한가: serialize_overlap=1 일 때, 같은 영역을 동시에 read/write 하면
 *   저장계층/verify 결과가 비결정적이 된다. 이 함수가 io_u_all 을 선형 스캔
 *   하여 IO_U_F_FLIGHT 비트가 켜진 io_u 를 걸러 범위 교차[x1,x2)∩[y1,y2)≠∅
 *   조건을 검사한다.
 *
 * 복잡도: O(iodepth). iodepth 가 크면 비용이 증가하므로 serialize_overlap 은
 *   기본 비활성화.
 *
 * 호출 체인: io_u_submit → in_flight_overlap. true 면 FIO_Q_BUSY 반환 경로.
 *
 * 동기화: td_offload_overlap 가 켜지면 io_u_all 조작 전후에 overlap_check
 *   뮤텍스로 보호 — 오프로드 워커 스레드와의 경합 방지.
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
 * [한국어]
 * io_u_submit - overlap 검사 후 엔진 queue 콜백에 io_u 전달
 *
 * @td:     잡의 thread_data.
 * @io_u:   제출할 I/O 유닛. get_io_u + prep 완료 상태.
 * @return: FIO_Q_COMPLETED(즉시 완료) / FIO_Q_QUEUED(비동기 큐잉) /
 *          FIO_Q_BUSY(엔진 바쁨 or overlap 충돌). td_io_queue 가 결정.
 *
 * 왜 필요한가: serialize_overlap=1 일 때, 동일 오프셋 영역에 대한 동시 I/O
 *   는 정의되지 않은 동작을 유발하므로 in-flight 큐를 훑어 충돌을 감지해야
 *   한다. 순수 io_u 제출은 td_io_queue 에 위임하고, 이 함수는 그 앞단에서
 *   overlap 필터만 추가한다.
 *
 * 동작: serialize_overlap && cur_depth>1 && in_flight_overlap(...) 이면
 *   FIO_Q_BUSY 반환 → 호출자(io_queue_event)가 재큐잉하여 기존 I/O 완료 후 재시도.
 *   그 외에는 td_io_queue 로 위임.
 *
 * 실행 컨텍스트: 잡 스레드. do_io/do_verify 메인 루프에서만 호출.
 *
 * 호출 체인: do_io/do_verify → io_u_submit → td_io_queue → 엔진 .queue.
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
 * [한국어]
 * do_verify - 메인 검증(verify) 엔진
 *
 * @td:           검증할 잡의 thread_data. verify 옵션(VERIFY_CRC32/MD5/META/
 *                PATTERN/SHA1/...) 과 verify_pattern_bytes, do_verify,
 *                experimental_verify, verify_backlog, verify_async 등이
 *                포함된 상태로 진입. verify_state 시드는 do_io 직전에 백업됨.
 * @verify_bytes: 검증 대상 총 바이트(thread_main 에서 do_io 후의 write+trim
 *                바이트 합). experimental_verify 에서 진행 제한으로 사용.
 * @return:       없음. 에러는 td->error 에 기록. 해시 불일치는 verify_io_u
 *                콜백이 td_verror 를 호출해 td->terminate 로 전파.
 *
 * 왜 필요한가: 쓰기 워크로드의 정확성(저장 계층이 사용자 데이터를 그대로
 *   돌려주는지)을 확인하기 위함. fio 는 쓰기 시 verify_header(CRC + seed +
 *   rand_seed) 를 payload 앞에 삽입하고, 이후 동일 영역을 읽어 header 를
 *   복구/검증한다.
 *
 * 실행 컨텍스트: 잡 스레드. thread_main 루프 내부에서 do_io 완료 후 호출.
 *   verify_async 가 설정되면 실제 체크섬 계산은 별도 검증 스레드 풀에서
 *   수행(verify_async_init). 이 함수는 io_u 제출/수거만 담당.
 *
 * 동작 흐름:
 *   1) 모든 열린 파일에 대해 fio_io_sync(DDIR_SYNC) 후 file_invalidate_cache —
 *      페이지 캐시를 비워 "디스크에서 실제로 읽힌 값" 을 검증하도록 보장.
 *   2) td_set_runstate(td, TD_VERIFYING).
 *   3) while (!terminate):
 *        - 일반 모드: __get_io_u + get_next_verify (이전에 기록한 쓰기 위치)
 *        - experimental 모드: get_io_u 로 쓰기/트림을 읽기로 변환
 *        - io_u->end_io = verify_io_u_async | verify_io_u
 *        - io_u_submit → io_queue_event → 필요 시 wait_for_completions
 *   4) 완료 콜백 verify_io_u 가 header 를 읽어 체크섬 검증. 불일치면 td_verror.
 *   5) 남은 in-flight 를 io_u_queued_complete 로 수거하고 runstate 복귀.
 *
 * 호출 체인:
 *   thread_main() → do_verify(td, verify_bytes)
 *     → fio_io_sync → td_io_queue (DDIR_SYNC)
 *     → file_invalidate_cache [filesetup.c]
 *     → get_next_verify [verify.c]
 *     → io_u_submit → td_io_queue → 엔진 .queue
 *     → io_u_queued_complete → verify_io_u [verify.c]
 *
 * 에러 경로: 해시 불일치는 verify_io_u 에서 td_verror + td->terminate.
 *   io_u 할당 실패, prep 실패 시에는 put_io_u 후 루프 탈출.
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
 * [한국어]
 * exceeds_number_ios - number_ios (IOPS × 총 I/O 개수) 제한 초과 여부
 *
 * @td: 잡의 thread_data.
 * @return: true=제한 도달/초과 / false=아직 남음 or 옵션 미설정.
 *
 * 왜 필요한가: 바이트 기반(size)/시간 기반(runtime)이 아닌 "정확히 N 개의
 *   I/O 만 수행" 하는 워크로드를 위해. 예: "1000 회 랜덤 읽기로 응답시간 분포 측정".
 *
 * 계산: 완료 블록 수(io_blocks 합) + 큐 대기(io_u_queued) + 진행중(io_u_in_flight)
 *   >= (number_ios × loops). 진행 중도 포함해 약간 early-break 경향.
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
 * [한국어]
 * io_bytes_exceeded - 바이트 한도(size 또는 io_size) 도달 여부 + number_ios 통합
 *
 * @td:         잡의 thread_data. o.size/o.io_size/loops 참조.
 * @this_bytes: 방향별 바이트 수 배열. issue 경로면 io_issue_bytes, 완료 경로면
 *              this_io_bytes. 두 가지 모두 이 함수를 재사용.
 * @return: true=한도 도달(종료) / false=계속.
 *
 * 왜 필요한가: "몇 바이트/loop 수행" 제한을 walltime 과 독립적으로 표현.
 *   이 함수가 "issue vs complete" 두 경로의 종료 판정을 통합한다.
 *
 * 계산:
 *   - td_rw(td) → read+write 합.
 *   - td_write → write 만.
 *   - td_read → read 만.
 *   - else → trim 만.
 *   - limit = (io_size 설정되면 io_size else size) × loops.
 *   - return bytes >= limit || exceeds_number_ios(td).
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
 * [한국어]
 * io_issue_bytes_exceeded - "발행한" 바이트 합이 한도 초과인가
 *
 * @td: 잡의 thread_data.
 * @return: io_bytes_exceeded(td, td->io_issue_bytes).
 *
 * 왜 필요한가: do_io 메인 루프는 완료 대기 전에 계속 I/O 를 발행한다. 이미
 *   한도만큼 발행했으면 더 이상 새 I/O 를 추가하지 않고 기존 완료 대기로 전환.
 */
static bool io_issue_bytes_exceeded(struct thread_data *td)
{
	return io_bytes_exceeded(td, td->io_issue_bytes);
}

/*
 * [한국어]
 * io_complete_bytes_exceeded - "완료한" 바이트 합이 한도 초과인가
 *
 * @td: 잡의 thread_data.
 * @return: io_bytes_exceeded(td, td->this_io_bytes).
 *
 * 용도: do_dry_run() 에서 사용. 드라이런은 발행=완료 이므로 this_io_bytes 기준.
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
 * [한국어]
 * usec_for_io - 다음 I/O 를 발행해야 할 "목표 시각" 계산 (rate control 핵심)
 *
 * @td:   잡의 thread_data. rate_bps[], rate_process, bssplit_nr, min_bs,
 *        rate_io_issue_bytes, last_usec, poisson_state 참조/갱신.
 * @ddir: I/O 방향. 방향별 독립적인 rate 를 지원하기 위함.
 * @return: epoch 기준 마이크로초. do_io 의 should_check_rate 분기가 이 값을
 *          td->rate_next_io_time[ddir] 에 저장해 실제 발행 시각과 비교.
 *
 * 왜 필요한가: rate=N[MB/s] 또는 rate_iops=N 옵션을 정확히 구현하려면
 *   "다음 I/O 를 언제 내보낼지" 를 수식으로 계산해 나노/마이크로 단위로
 *   준수해야 한다. fio 는 busy-wait + usleep 조합으로 이를 수행한다.
 *
 * 동작 분기:
 *   - RATE_PROCESS_POISSON (포아송): 지수분포 샘플링
 *       val = (1e6/iops) × -ln(U) ; U ∈ (0,1)
 *     last_usec[ddir] += val 로 누적 시각 반환. 버스티한 트래픽 시뮬레이션.
 *   - bps=0 → 0 리턴 (rate 없음).
 *   - rate_iops + bssplit 조합 : iops=bps/min_bs 로 "유저가 의도한 IOPS"
 *     복원해 간격 계산.
 *   - 일반 rate: rate_io_issue_bytes[ddir] 의 누적 바이트로 경과 예상 시간 계산.
 *
 * 주의: assert(!(td->flags & TD_F_CHILD)) — 자식 잡(verify async 스레드 등)
 *   에서는 호출되지 않음.
 *
 * 실행 컨텍스트: 잡 스레드. do_io 루프 후반 should_check_rate 분기.
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
 * [한국어]
 * init_thinktime - thinktime 카운터 초기 세팅
 *
 * @td: 잡의 thread_data.
 *
 * 왜 필요한가: thinktime 은 N 블록 처리마다 일정 시간 "생각하는" 모방. 기준
 *   카운터가 완료(complete) 블록인지 발행(issue) 블록인지에 따라
 *   thinktime_blocks_counter 를 서로 다른 배열에 가리키게 한다.
 *
 * 동작: thinktime_blocks_type==COMPLETE 면 io_blocks 배열, 아니면
 *   io_issues 배열을 카운터로 등록. last_thinktime=epoch, last_thinktime_blocks=0.
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
 * [한국어]
 * handle_thinktime - thinktime_blocks/iotime 조건 충족 시 실제 대기 수행
 *
 * @td:   잡의 thread_data.
 * @ddir: 방금 수행한 I/O 방향. rate_ign_think 보정 시 방향별 계산에 사용.
 * @time: 속도 체크용 timespec 포인터(NULL 가능). 대기 후 갱신.
 *
 * 왜 필요한가: 실제 애플리케이션은 I/O 사이에 CPU 작업/사용자 입력 등
 *   "생각 시간" 이 있다. 이 함수가 thinktime_blocks (N 블록마다) 또는
 *   thinktime_iotime (N μs 의 I/O 후) 기준으로 의도적 대기를 삽입해
 *   실제 워크로드 패턴을 재현한다.
 *
 * 동작:
 *   - thinktime_iotime 경과 시 또는 thinktime_blocks 초과 시 stall=true.
 *   - stall 이면 io_u_quiesce 로 현재 in-flight 를 모두 완료시킴(깨끗한 대기 보장).
 *   - thinktime_spin μs 만큼 usec_spin (VM 에서 usec_spin 이 약간 오버할 수 있어
 *     남은 시간 재계산).
 *   - 남은 (thinktime - spent) 는 usec_sleep 으로 커널 수면.
 *   - rate_ign_think 옵션: 생각 시간 동안 "발행됐을" 바이트를 보정(생각 시간을
 *     속도 계산에서 제외) — last_usec/rate_io_issue_bytes 보정.
 *
 * 실행 컨텍스트: 잡 스레드 do_io 루프 후반.
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
 * [한국어]
 * log_inflight - 쓰기 io_u 의 numberio 를 인플라이트 슬롯에 기록
 *
 * @td:   잡의 thread_data. inflight_numberio 배열 소유자.
 * @io_u: 쓰기 방향 io_u (DDIR_WRITE). numberio 는 이미 설정되어 있음.
 *
 * 왜 필요한가: verify_state_save 는 "어디까지 쓰여졌는가" 를 재시작 후에도
 *   알기 위해 인플라이트 쓰기 numberio 를 슬롯에 기록한다. 이후 완료 시
 *   invalidate_inflight 로 슬롯을 해제한다. 이 배열 스냅샷이 verify 상태
 *   파일에 저장되어, 재실행의 do_verify 가 "확실히 완료된 영역만" 검증하게.
 *
 * 동작:
 *   - 인플라이트 로깅 비활성 또는 WRITE 아님 → 리턴.
 *   - io_u->inflight_idx != -1 (이미 슬롯 보유) → abort (버그).
 *   - td->inflight_issued != io_u->numberio → abort (순서 불일치).
 *   - next_inflight_numberio_idx 부터 원형 탐색으로 빈 슬롯(INVALID_NUMBERIO) 찾기.
 *   - atomic_store_release 로 slot := numberio 기록 후 inflight_issued := numberio+1.
 *     두 저장의 순서(프리 슬롯 기록 → issued 갱신)가 중요 — 다른 스레드가
 *     issued 를 읽은 뒤 해당 번호의 슬롯을 검색했을 때 보이도록 release 필요.
 *
 * 실행 컨텍스트: 잡 스레드 do_io 쓰기 분기에서 io_u 발행 직전.
 *
 * 에러 경로: 슬롯을 찾지 못함 = iodepth 초과로 오버플로 → abort (복구 불가 버그).
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
 * [한국어]
 * invalidate_inflight - 완료/취소된 쓰기 io_u 의 인플라이트 슬롯 해제
 *
 * @td:   잡의 thread_data.
 * @io_u: 완료된 또는 실패된 쓰기 io_u. inflight_idx 가 세팅되어 있어야 함.
 *
 * 왜 필요한가: log_inflight 가 할당한 슬롯을 해제하지 않으면 다음 쓰기가
 *   빈 슬롯을 찾지 못해 abort. 또한 verify_state 저장 시 유령 쓰기로 오염.
 *
 * 동작:
 *   - 로깅 비활성 / 쓰기 아님 / 슬롯 없음(inflight_idx=-1) → 리턴.
 *   - 이미 INVALID_NUMBERIO → abort (이중 무효화는 버그).
 *   - 슬롯의 numberio 와 io_u 의 numberio 불일치 → abort (교차 오염).
 *   - atomic_store_release 로 slot := INVALID_NUMBERIO. io_u->inflight_idx=-1.
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
 * [한국어]
 * clear_inflight - 루프 반복 사이 인플라이트 배열 전면 초기화
 *
 * @td: 잡의 thread_data.
 *
 * 왜 필요한가: loops>1 또는 time_based 에서 외부 루프를 반복할 때, 이전 루프에서
 *   남아있을 수 있는 슬롯 잔존 상태를 지워 다음 루프가 깨끗한 배열로 시작하도록.
 *   또한 실험적 verify 는 io_issues[WRITE] 를 외부에서 증가시킬 수 있어
 *   inflight_issued 를 동기화해야 함.
 *
 * 동작: 모든 슬롯 = INVALID_NUMBERIO, next_inflight_numberio_idx=0,
 *   inflight_issued := io_issues[DDIR_WRITE] 로 재동기화.
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
 * @td:         잡의 thread_data. 옵션/상태/io_u 풀/통계/엔진 ops 전체를 포함.
 * @bytes_done: 출력 파라미터. 이 do_io 호출 "한 번" 동안 새로 완료된 바이트 수를
 *              읽기/쓰기/트림(DDIR_RWDIR_CNT=3) 방향별로 채워준다. 내부에서
 *              진입 시 td->bytes_done 스냅샷을 저장하고, 종료 시 차이를 계산.
 *              호출자(thread_main)는 verify 단계에서 검증할 양을 결정하는 데 사용.
 *
 * @return: 없음 (bytes_done[] 으로 간접 반환). 에러는 td->error 에 기록.
 *
 * 왜 필요한가: 사용자가 지정한 워크로드(read/write/rw/randread/trim + iodepth +
 *   bs + rate + runtime 등)를 실제로 I/O 엔진에 전달해 수행하는 유일한 경로이다.
 *   fio 가 "측정" 하려는 BW/IOPS/lat 숫자는 결국 이 루프의 각 반복마다 io_u 를
 *   할당→제출→완료 처리하는 지점에서 수집된다.
 *
 * 실행 컨텍스트: 잡 스레드(thread_main)에서 단일 호출. 재진입하지 않음.
 *   verify 단계는 별도 do_verify() 가 동일 로직을 변형해 수행한다.
 *   SIGINT 등으로 td->terminate 가 세팅되면 다음 반복에서 break.
 *
 * 호출 체인:
 *   thread_main() → do_io(td, bytes_done)
 *     → get_io_u() / put_io_u() [io_u.c]
 *     → io_u_submit() [이 파일] → td_io_queue() [ioengines.c] → 엔진 .queue
 *     → wait_for_completions() [이 파일] → io_u_queued_complete [io_u.c]
 *        → td_io_getevents [ioengines.c] → 엔진 .getevents / .event
 *     → fio_io_sync() (end_fsync/fsync_on_close 경로)
 *     → workqueue_enqueue()/workqueue_flush() (IO_MODE_OFFLOAD)
 *
 * 에러 경로: td->error != 0 이면 break_on_this_error 분기 → 루프 탈출 →
 *   정상 정리 경로로 통합 (workqueue_flush, 인플라이트 완료 수거, end_fsync).
 *   io_u 할당 실패는 -EBUSY 일 때는 reap 재시도, 일반 에러는 즉시 탈출.
 *
 * 루프 수행 내용:
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
 * [한국어]
 * init_inflight_logging - verify_state_save 용 인플라이트 쓰기 추적 배열 할당
 *
 * @td: 잡의 thread_data. o.verify, o.verify_state_save, o.iodepth 참조.
 * @return: 0=성공(검사 OFF 포함) / 1=scalloc 실패.
 *
 * 왜 필요한가: 검증 상태를 저장/복원하려면 "어떤 쓰기가 commit 된 시점에
 *   완료되어 있었는가" 를 알아야 한다. 이 배열은 iodepth 크기의 슬롯 배열로
 *   각 슬롯에 진행 중인 쓰기의 numberio 를 기록한다. verify_save_state 가
 *   이 배열을 디스크에 저장하고, 재실행 시 이를 로드해 재검증할 영역 결정.
 *
 * 동작: scalloc(iodepth, sizeof(uint64_t)) — 공유 메모리로 할당(fork 자식
 *   공유). 모든 슬롯을 INVALID_NUMBERIO 로 초기화.
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
 * [한국어]
 * free_inflight_logging - 인플라이트 추적 배열 해제
 *
 * @td: 잡의 thread_data.
 *
 * 호출 체인: cleanup_io_u → free_inflight_logging → sfree(공유 메모리).
 * 안전성: NULL 포인터 확인 후 sfree — 두 번 해제 방지.
 */
static void free_inflight_logging(struct thread_data *td)
{
	if (td->inflight_numberio)
		sfree(td->inflight_numberio);
}

/*
 * [한국어]
 * cleanup_io_u - io_u 풀 및 버퍼 메모리 해제
 *
 * @td: 잡의 thread_data.
 *
 * 왜 필요한가: thread_main 이 정상/에러 모두 err 라벨을 거치므로, 각 경로에서
 *   할당한 io_u/버퍼/큐/엔진 per-io_u 데이터/인플라이트 로그 배열을
 *   일관되게 되돌려야 한다. 순서를 잘못 맞추면 use-after-free 가 발생.
 *
 * 해제 순서(중요):
 *   1) freelist 의 모든 io_u 에 대해 엔진의 io_u_free 콜백 호출(엔진별
 *      per-io_u 상태 정리) → fio_memfree 로 구조체 반환.
 *   2) io_u_requeues 의 io_u 들을 put_io_u 로 freelist 통과시켜 정리.
 *      (주의: 순환 참조 방지를 위해 freelist 정리 전에 이미 drain 된 상태)
 *   3) free_io_mem(td) — 데이터 버퍼 블록 해제.
 *   4) io_u_rexit/io_u_qexit — 큐 자료구조 자체 해제.
 *   5) free_inflight_logging — verify 추적 배열 해제.
 *
 * 실행 컨텍스트: 잡 스레드 thread_main err 라벨.
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
 * [한국어]
 * init_io_u - iodepth 개의 io_u 구조체 풀 할당 및 엔진 훅 호출
 *
 * @td: 잡의 thread_data. o.iodepth, io_ops, io_offload_overlap 플래그 참조.
 * @return: 0=성공, 1=에러(메모리 부족 / 엔진 io_u_init 실패 / 터미네이트).
 *
 * 왜 필요한가: fio 의 I/O 단위 객체 io_u 는 잡 시작 시 풀(iodepth 개)로 미리
 *   할당되어 do_io 루프에서 freelist↔in-flight 간 이동하며 재사용된다.
 *   이 함수는 그 풀 구축과 엔진별 per-io_u 초기화(예: libaio 의 struct iocb,
 *   io_uring 의 io_uring_sqe 포인터, DAOS 의 daos_iou 등)를 책임진다.
 *
 * 초기화 순서:
 *   1) io_u_rinit(requeues) + io_u_qinit(freelist) + io_u_qinit(io_u_all).
 *   2) os_cache_line_size() 정렬로 iodepth 회 fio_memalign.
 *   3) 각 io_u: 필드 초기화(flags=IO_U_F_FREE, index=i, inflight_idx=-1),
 *      freelist/io_u_all 양쪽에 push.
 *   4) 엔진 io_u_init 콜백(있으면) — 실패 시 에러.
 *   5) init_io_u_buffers — 데이터 버퍼 할당 + 쓰기 버퍼 패턴 초기화.
 *   6) init_inflight_logging — verify_state 추적 배열.
 *
 * 실행 컨텍스트: 잡 스레드 thread_main 초기화 단계. td_io_init 뒤에 호출.
 *
 * 에러 경로: 중간 실패 시 호출자가 err 라벨로 점프 → cleanup_io_u 가 이미
 *   추가된 일부를 정리. init_io_u 내부에서 롤백하지 않음.
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
 * [한국어]
 * init_io_u_buffers - 모든 io_u 를 위한 데이터 버퍼 풀 할당
 *
 * @td: 잡의 thread_data. o.odirect/mem_align/hugepage_size/num_range 등 참조.
 * @return: 0=성공, 1=에러(size 오버플로 / 메모리 할당 실패).
 *
 * 왜 필요한가: fio 는 I/O 성능을 측정하므로 버퍼 할당 오버헤드가 측정에 섞이면
 *   안 된다. 따라서 잡 시작 전에 iodepth × max_bs 크기의 단일 블록을 미리
 *   할당하고, 각 io_u 에 max_bs 슬라이스를 미리 배정한다. 런타임에는 포인터
 *   산술로만 버퍼 위치를 찾아간다.
 *
 * 메모리 레이아웃:
 *   [io_u[0].buf | io_u[1].buf | ... | io_u[iodepth-1].buf]
 *   각 슬라이스 크기 = max_bs (multi-range trim 이면 trim_bs=num_range×sizeof(trim_range)).
 *
 * 정렬 처리:
 *   - O_DIRECT 또는 mem_align 요구: page_mask+mem_align 만큼 여유 추가 후
 *     PTR_ALIGN 으로 정렬된 시작점 계산.
 *   - HUGE 메모리(MEM_SHMHUGE/MMAPHUGE): hugepage_size 배수로 올림.
 *
 * 쓰기 버퍼 초기화: td_write 이면 io_u_fill_buffer 로 워크로드 패턴(dedupe
 *   ratio/compressibility) 에 맞춰 초기 데이터 채움. verify_pattern_bytes 설정
 *   시 fill_verify_pattern 으로 검증 패턴 덮어씀.
 *
 * 실행 컨텍스트: 잡 스레드 init_io_u → init_io_u_buffers.
 *
 * 호출 체인: init_io_u → init_io_u_buffers → allocate_io_mem [io_u.c] +
 *   io_u_fill_buffer/fill_verify_pattern.
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
 * [한국어]
 * set_ioscheduler - 블록 디바이스의 I/O 스케줄러를 sysfs 로 변경
 *
 * @td:   잡의 thread_data. o.ioscheduler 옵션값 사용.
 * @file: 대상 파일(블록 디바이스). file->du->sysfs_root 가 /sys/block/<dev>/
 *        형태여야 함 (diskutil.c 가 탐지).
 * @return: 0=성공 또는 기능 미지원, 1=치명적 오류(파일 open 실패/write 실패/검증 실패).
 *
 * 왜 필요한가: 벤치마크 재현성을 위해 mq-deadline/none/bfq/kyber 등 스케줄러를
 *   명시적으로 선택해야 할 때. sysfs /sys/block/<dev>/queue/scheduler 에 이름을
 *   쓰면 커널이 런타임 전환을 수행.
 *
 * 동작:
 *   1) /sys/block/<dev>/queue/scheduler 를 r+ 로 open. ENOENT 면 커널/OS 가
 *      이 기능을 제공하지 않음 → 경고 후 성공 리턴.
 *   2) o.ioscheduler 문자열 fwrite.
 *   3) rewind 후 다시 fread — 커널은 "옵션1 [선택된옵션] 옵션2" 형식으로
 *      반환. "[<name>]" 부분문자열 매칭으로 선택 확인.
 *   4) "none\n" 반환은 "스케줄러 변경 불가(queue 타입이 mq-none 등)" 을
 *      의미 — 경고 후 성공(0) 리턴.
 *
 * 실행 컨텍스트: 잡 스레드 thread_main 초기화 단계(td_io_init 이후).
 *
 * 호출 체인: thread_main → switch_ioscheduler → set_ioscheduler.
 *
 * 플랫폼: Linux 전용 (FIO_HAVE_IOSCHED_SWITCH). else 빌드에서는 no-op 스텁.
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
 * [한국어]
 * switch_ioscheduler - 잡의 모든 파일에 대해 set_ioscheduler 순회 호출
 *
 * @td: 잡의 thread_data.
 * @return: 0=성공, 양수=첫 set_ioscheduler 실패 시 그 값.
 *
 * 동작: FIO_DISKLESSIO 엔진(예: null/net/cpu) 은 스케줄러 개념 없음 → 0 리턴.
 *   for_each_file: FIO_TYPE_FILE/BLOCK 만 처리, du 가 없으면(탐지 실패) skip.
 *   캐릭터/파이프는 skip. 각 파일에 set_ioscheduler 적용.
 *
 * 호출 체인: thread_main → switch_ioscheduler → set_ioscheduler × N 파일.
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

/*
 * [한국어]
 * switch_ioscheduler (스텁) - 비 Linux 플랫폼에서의 no-op 구현
 *
 * 왜 존재: /sys/block 사용 불가한 플랫폼(macOS/BSD/Windows) 에서 link 오류 방지.
 * 동작: 아무 것도 하지 않고 0 반환.
 */
static int switch_ioscheduler(struct thread_data *td)
{
	return 0;
}

#endif /* FIO_HAVE_IOSCHED_SWITCH */

/*
 * [한국어]
 * keep_running - thread_main 외부 루프의 지속 여부 결정
 *
 * @td: 잡의 thread_data.
 * @return: true=다음 루프 반복 실행 / false=thread_main 외부 while 탈출.
 *
 * 왜 필요한가: fio 의 한 잡은 loops 옵션으로 동일 워크로드를 N 회 반복할 수
 *   있고, time_based 옵션으로 "지정 시간만큼 반복" 할 수도 있다. 이 함수가
 *   thread_main 의 외부 while 조건을 담당해 루프 지속/종료를 결정한다.
 *   do_io 내부의 "하나의 루프 반복" 종료 조건과는 구분된다.
 *
 * 판정 순서(위→아래, 먼저 true/false 결정되면 즉시 반환):
 *   1) td->done → false.
 *   2) td->terminate → false.
 *   3) time_based=1 → true (시간으로만 종료 결정).
 *   4) loops>0 → loops-- 후 true.
 *   5) exceeds_number_ios → false.
 *   6) limit(io_size 또는 size) 남았으면 true, 아니면 false.
 *
 * 실행 컨텍스트: 잡 스레드. thread_main 매 외부 루프 반복 전 호출.
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
 * [한국어]
 * exec_string - exec_prerun/exec_postrun 외부 명령 실행
 *
 * @o:      thread_options 포인터(잡 이름 사용).
 * @string: 실행할 shell 명령 문자열(exec_prerun 또는 exec_postrun 옵션값).
 * @mode:   "prerun" 또는 "postrun" — 로그 파일명과 로그 메시지에 사용.
 * @return: system(3) 의 반환값. -1 이면 shell 시작 실패, 그 외는 셸의 exit 상태.
 *
 * 왜 필요한가: 벤치마크 전후로 환경 정비(캐시 drop, 디바이스 초기화, 결과 수집)
 *   가 필요한 경우가 많다. "fio 실행 파일만으로" 사전/사후 스크립트까지
 *   엮을 수 있게 하는 편의 기능.
 *
 * 동작: asprintf 로 "<cmd> > <name>.<mode>.txt 2>&1" 을 구성해 system(3) 호출.
 *   stdout/stderr 모두 파일로 저장해 잡 출력과 섞이지 않게 함.
 *
 * 보안 주의: system(3) 은 /bin/sh 로 해석되므로 셸 injection 가능. 신뢰할
 *   수 있는 잡 파일에서만 사용할 것.
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
 * [한국어]
 * do_dry_run - verify_only 경로의 "가짜 I/O" 실행 (numberio 동기화 전용)
 *
 * @td: 잡의 thread_data.
 * @return: 이번 드라이런에서 "완료됐다고 가정" 한 write+trim 바이트 합.
 *          이 값이 thread_main 에서 do_verify 의 verify_bytes 인자로 전달됨.
 *
 * 왜 필요한가: verify_only=1 모드는 "이미 과거에 쓴 데이터를 검증만" 하므로
 *   실제 쓰기를 수행하지 않는다. 하지만 검증 시 각 io_u 의 numberio/시드
 *   상태는 쓰기 시와 동일하게 진행되어야 한다. 이 함수가 get_io_u →
 *   io_u_sync_complete 경로를 실제 I/O 없이 돌려 numberio/io_issues/io_blocks
 *   상태를 "쓰기 잡과 동일하게" 맞춰준다.
 *
 * 동작: runstate=TD_RUNNING. 루프:
 *   - get_io_u → IO_U_F_FLIGHT 플래그 + resid=0/error=0 세팅.
 *   - io_issues[ddir]++ 갱신.
 *   - 쓰기면 log_io_piece 로 iolog 기록(실험적 verify 모드 제외).
 *   - io_u_sync_complete(td, io_u) — 통계 반영 + freelist 반환.
 *
 * 실행 컨텍스트: 잡 스레드. thread_main 에서 verify_only 분기.
 *
 * 호출 체인: thread_main → do_dry_run → get_io_u + log_io_piece + io_u_sync_complete.
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
 * [한국어]
 * fork_data - pthread_create/fork 에서 thread_main 으로 넘기는 2-인자 번들
 *
 * pthread_create 의 start_routine 은 void*(void*) 시그니처라 "여러 값" 을 넘기
 * 려면 구조체 포인터로 묶어 전달해야 한다. 이 구조체는 run_threads 에서
 * calloc 으로 할당되고, thread_main 초반 sk_out_assign + free(fd) 로 소비된다.
 *
 * 생성자: run_threads()가 calloc 후 td/sk_out 세팅.
 * 소비자: thread_main()이 sk_out_assign(sk_out_drop는 종료 시) 후 free(fd).
 * 수명  : 잡 하나당 1개. thread_main 진입 직후 free.
 * 공유  : 각 잡 단독 소유 — 스레드 간 공유 X, 동기화 불필요.
 */
struct fork_data {
	struct thread_data *td;
	/* [한국어] 이 잡의 thread_data 포인터.
	 * 설정자: run_threads() 가 해당 td 를 채움.
	 * 읽는 자: thread_main() 의 전 라이프사이클에서 사용.
	 * 값 범위: smalloc 으로 할당된 공유 메모리의 유효 포인터 (NULL 불가).
	 * 동기화: td 자체는 공유 메모리이므로 여러 스레드가 읽을 수 있으나,
	 *          이 포인터 필드는 해당 잡 스레드만 소유. */

	struct sk_out *sk_out;
	/* [한국어] 서버 모드의 소켓 출력 컨텍스트. 로컬 모드에서는 NULL.
	 * 설정자: run_threads() 가 자기 인자의 sk_out 을 그대로 대입.
	 * 읽는 자: thread_main() 이 sk_out_assign(TLS 등록) 후 사용.
	 * 값 범위: NULL(로컬) 또는 유효 sk_out. 서버 모드에서 log_info 등이
	 *          이 컨텍스트로 TCP 소켓에 기록. */
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
 * @data: struct fork_data *. td(thread_data*)와 sk_out(sk_out*)을 담은
 *        heap-alloc 구조체. 호출자(run_threads)가 calloc 후 소유권을 넘김.
 *        이 함수가 sk_out_assign 으로 TLS 등록 후 free(fd) 로 해제.
 *
 * @return: (void *)(uintptr_t) td->error. 0 이면 성공, 비0이면 해당 errno.
 *          pthread 모드: pthread_join 은 안 하고 detach. 부모는 runstate 로 감지.
 *          fork 모드: return 값이 main() 의 _exit(ret) 코드로 전달 — waitpid
 *          에서 WEXITSTATUS 로 관찰. errno 가 256을 초과하면 8비트 잘림.
 *
 * 왜 필요한가: run_threads()는 "N 개의 잡을 병렬 실행" 이라는 추상적 컨트롤만
 *   담당하고, 각 잡의 실제 수행(엔진 초기화→I/O→검증→정리)은 이 함수가 진행한다.
 *   pthread/fork 두 모드 모두 여기를 지나가므로 이 파일의 실질적 중심이다.
 *
 * 실행 컨텍스트: 매 잡마다 하나의 인스턴스. pthread_create 된 경우 현재
 *   프로세스 내 새 스레드(공유 주소공간), fork 된 경우 독립 프로세스(COW
 *   복제 후 공유 메모리 영역만 실제 공유). 동일 프로세스/스레드 내 재진입 없음.
 *
 * runstate 전이 (파일 상단 다이어그램 참조):
 *   진입 직후        TD_CREATED  (부모가 이미 설정)
 *   초기화 후        TD_INITIALIZED   ← startup_sem up
 *   ── 부모가 td->sem up 까지 대기 ──
 *   I/O 루프 진입    TD_RAMP / TD_RUNNING
 *   검증 루프 진입   TD_VERIFYING
 *   end_fsync 중     TD_FSYNCING
 *   통계 마무리      TD_FINISHING
 *   함수 return 직전 TD_EXITED
 *
 * 에러 경로: goto err. 어느 단계에서 실패하든 err 라벨로 점프해 공통
 *   cleanup(파일 close, io_u 풀 해제, 엔진 close, cgroup/NUMA/cpuset 해제) 수행.
 *
 * 호출 체인:
 *   run_threads() → pthread_create(thread_main, fd) OR fork()→thread_main(fd)
 *     → td_io_init [ioengines.c]
 *     → init_io_u/init_io_u_buffers [이 파일]
 *     → setup_files [filesetup.c]
 *     → do_io [이 파일] (loops/time_based 만큼 반복)
 *     → do_verify [이 파일] (verify 옵션 시)
 *     → close_and_free_files/close_ioengine [filesetup.c/ioengines.c]
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

	/* [한국어] sk_out 을 TLS 에 등록 — 이후 log_info/log_err 가 TCP 소켓으로 전송.
	 * 로컬 모드에서는 sk_out=NULL 이며 일반 stdout/stderr 로 감 */
	sk_out_assign(sk_out);
	free(fd);  /* [한국어] fork_data 번들은 여기서 소비 완료, 즉시 해제 */

	/* [한국어] PID 기록: fork 모드는 getpid(2)(자식 프로세스 PID), pthread 모드는
	 * gettid(2)(LWP tid). reap_threads 의 waitpid 는 프로세스 모드에서만 의미.
	 * setsid(2) 로 새 세션 리더가 되어 부모의 제어 터미널에서 분리 — Ctrl+C 가
	 * 자식 fio 프로세스에 직접 전달되지 않도록 (fio 부모가 sig_int 로 대신 전파). */
	if (!o->use_thread) {
		setsid();            /* [한국어] 새 세션 리더, 터미널 분리 (프로세스 모드 전용) */
		td->pid = getpid();  /* [한국어] 자식 프로세스 PID */
	} else
		td->pid = gettid();  /* [한국어] pthread 의 커널 LWP tid */

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

	/* [한국어] io_u_lock(뮤텍스) + free_cond(조건변수) 초기화.
	 * fork 모드에서는 부모와 자식이 같은 객체를 봐야 하므로 pshared 속성 필수.
	 * pthread_mutexattr_setpshared(PTHREAD_PROCESS_SHARED) 내부 호출.
	 * io_u_lock 은 io_u 큐 조작 직렬화, free_cond 는 freelist 가 비었을 때 블록. */
	ret = mutex_cond_init_pshared(&td->io_u_lock, &td->free_cond);
	if (ret) {
		td_verror(td, ret, "mutex_cond_init_pshared");
		goto err;
	}
	/* [한국어] verify_cond: 검증 스레드 풀이 대기하는 조건변수(verify_async 시). */
	ret = cond_init_pshared(&td->verify_cond);
	if (ret) {
		td_verror(td, ret, "mutex_cond_pshared");
		goto err;
	}

	/* [한국어] ===== runstate 전이: TD_CREATED → TD_INITIALIZED =====
	 * 이 지점이 "파일/엔진/io_u 이전의 기초 초기화 완료" 시점. 이후 부모가
	 * 모든 잡이 TD_INITIALIZED 가 되었는지 확인 후 td->sem up 으로 "동시 시작"
	 * 신호를 보낸다. startup_sem 은 여러 잡이 공유하는 세마포어 — up/down
	 * 횟수로 부모가 초기화 완료 수를 카운트. */
	td_set_runstate(td, TD_INITIALIZED);
	dprint(FD_MUTEX, "up startup_sem\n");
	fio_sem_up(startup_sem);       /* [한국어] 부모에게 "초기화 완료" 신호 — 카운팅 세마포어 +1 */
	dprint(FD_MUTEX, "wait on td->sem\n");
	fio_sem_down(td->sem);         /* [한국어] 부모가 "실행 시작" 신호를 줄 때까지 블록 */
	dprint(FD_MUTEX, "done waiting on td->sem\n");
	/* [한국어] 여기 이후 runstate 는 run_threads 의 전이에 의해 TD_RAMP
	 * (ramp_time 구간) 또는 TD_RUNNING 으로 설정된 상태. */

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
 * [한국어]
 * reap_threads - 종료된 잡 스레드/프로세스를 수거(TD_EXITED → TD_REAPED)
 *
 * @nr_running: [in/out] 현재 실행 중 잡 수 포인터. 회수된 개수만큼 감소.
 *              run_threads 의 메인 루프 종료 조건에 사용됨.
 * @t_rate:     [in/out] 전체 잡의 목표(rate) 바이트/초 합계. 회수 시 해당
 *              잡 몫을 감산. (디버그/로깅용 — 실제 제어에는 미사용)
 * @m_rate:     [in/out] 전체 잡의 최소(ratemin) 바이트/초 합계. 동일.
 * @return:     없음.
 *
 * 왜 필요한가: 잡 스레드/프로세스가 do_io 를 마치고 thread_main 에서
 *   TD_EXITED 로 자신을 표시해도, 부모가 그 사실을 감지하고 자원을
 *   정리(use_fork 경우 waitpid) 해야 프로세스 테이블 엔트리가 해제되고
 *   nr_running 카운트가 정확해진다. 또한 멈춘(stuck) 잡을 FIO_REAP_TIMEOUT
 *   후 강제 회수하여 run_threads 가 무한 대기하지 않게 한다.
 *
 * 동작:
 *   - for_each_td 순회:
 *       * cpuio 엔진 카운트(마지막 경우 모두 종료 판정에 사용).
 *       * td->pid==0 (아직 시작 안 된 잡): pending++.
 *       * 이미 TD_REAPED 면 skip.
 *       * use_thread=1 모드: runstate==TD_EXITED 이면 바로 TD_REAPED 전이.
 *         pthread_detach 해 놨기에 join 불필요.
 *       * use_fork=0(default) 모드: waitpid(td->pid, &status,
 *         TD_EXITED 면 0=블로킹 else WNOHANG).
 *           ECHILD → td->sig=ECHILD, TD_REAPED.
 *           WIFSIGNALED → td->sig=WTERMSIG, TD_REAPED.
 *           WIFEXITED → td->error=WEXITSTATUS, TD_REAPED.
 *       * 멈춤 감지: terminate=1 이면서 TD_FSYNCING 미도달 + FIO_REAP_TIMEOUT
 *         초과 → 강제 TD_REAPED 로 마킹 후 경고 로그.
 *   - 회수된 잡: nr_running--, m_rate/t_rate 감산, exit_value 누적,
 *     done_secs 누적, profile_td_exit/flow_exit_job.
 *   - nr_running==cputhreads && !pending && realthreads : cpuio 잡만 남음 →
 *     fio_terminate_threads 전체 호출(원래 I/O 잡이 모두 끝났는데 cpuio 만
 *     남아있는 상황 — 무한 실행 방지).
 *
 * 실행 컨텍스트: run_threads 메인 스레드에서 100ms/10ms 간격으로 호출.
 *
 * 호출 체인: run_threads() → reap_threads(&nr_running, &t_rate, &m_rate).
 *
 * 에러 경로: waitpid 가 -1/ECHILD 이면 이미 시그널 핸들러 등이 수거했거나
 *   kernel 이 자식을 알 수 없는 상태 — 경고 후 TD_REAPED 로 마킹.
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
 * [한국어]
 * __check_trigger_file - 트리거 파일 존재 시 삭제 + true, 없으면 false
 *
 * @return: true=트리거 감지 / false=미감지 또는 trigger_file 미설정.
 *
 * 왜 필요한가: 외부 프로세스가 fio 에 "특정 이벤트" 를 신호하기 위한 인밴드
 *   메커니즘. 시그널 대신 파일 존재로 시그널링하면 여러 프로세스에 대한
 *   방송성, 원자성(unlink vs rename)을 활용할 수 있다.
 *
 * 동작: stat(trigger_file) 성공이면 unlink(2) + true. unlink 실패는
 *   경고만 로그하고 true 를 유지 — 조건이 이미 소비되었음을 표현.
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
 * [한국어]
 * trigger_timedout - --trigger-timeout 옵션 경과 여부 (1회성)
 *
 * @return: true=방금 경과 확인(이후 호출은 false) / false=미경과 또는 미설정.
 *
 * 부수효과: true 반환 시 trigger_timeout=0 으로 리셋해 다시는 true 반환 안 함.
 * 호출 체인: check_trigger_file → trigger_timedout.
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
 * [한국어]
 * exec_trigger - 외부 트리거 커맨드 실행(system(3))
 *
 * @cmd: 실행할 shell 커맨드. NULL/빈 문자열이면 no-op.
 *
 * 왜 필요한가: 트리거 감지 시 사용자 정의 스크립트(예: IO 중단, 로그 수집)를
 *   실행하여 fio 를 외부 이벤트에 반응시키기 위함.
 * 보안 주의: shell injection 여지 — 신뢰 옵션에서만 사용.
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
 * [한국어]
 * check_trigger_file - 메인 스레드 폴링 루프의 트리거 훅
 *
 * 왜 필요한가: 장기 실행 중 외부에서 "지금 상태 저장하고 종료 + 재개 가능
 *   스크립트 실행" 을 요청받는 메커니즘. 파일 존재 또는 시간 경과 두 트리거
 *   중 하나라도 만족 시 아래 동작 수행.
 *
 * 동작:
 *   - 감지 시:
 *     a) 클라이언트 다수 연결(nr_clients): fio_clients_send_trigger 로
 *        원격 fio 서버들에게도 트리거 전파.
 *     b) 로컬: verify_save_state(IO_LIST_ALL) 로 모든 잡의 검증 상태 저장,
 *        fio_terminate_threads 로 종료, 마지막으로 exec_trigger 로 커맨드 실행.
 *
 * 호출 체인: run_threads 의 do_usleep 폴링 루프 → check_trigger_file.
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
 * [한국어]
 * fio_verify_load_state - 이전 run 의 검증 상태 파일 로드
 *
 * @td: 잡의 thread_data. o.verify_state 옵션이 true 일 때만 의미.
 * @return: 0=성공 또는 옵션 미설정, 양수=에러(파일 없음/포맷 오류/서버 통신 실패).
 *
 * 왜 필요한가: fio 를 중단 후 다시 시작할 때, 이미 성공 검증한 영역은
 *   건너뛰고 싶다. 이 함수가 저장된 상태(verify_save_state 로 기록)를 읽어
 *   td 의 검증 진행점을 복원한다.
 *
 * 동작 분기:
 *   - is_backend (서버 모드): fio_server_get_verify_state 로 네트워크로 수신
 *     후 verify_assign_state 로 td 에 반영.
 *   - 로컬: aux_path/ local / <jobname> 경로에서 파일 로드(verify_load_state).
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
 * [한국어]
 * do_usleep - run_threads 폴링 슬립(훅을 처리하는 wrapper usleep)
 *
 * @usecs: 대기 시간(마이크로초).
 *
 * 왜 필요한가: run_threads 의 메인 루프는 잡 상태를 짧게 폴링하며 진행한다.
 *   단순 usleep 이전에 "실행 중 통계 요청" 과 "트리거 파일" 을 먼저
 *   처리해야 응답성이 유지된다. 이 wrapper 가 그 두 훅을 보장.
 *
 * 동작: check_for_running_stats → check_trigger_file → usleep(usecs).
 */
static void do_usleep(unsigned int usecs)
{
	check_for_running_stats();
	check_trigger_file();
	usleep(usecs);
}

/*
 * [한국어]
 * check_mount_writes - 쓰기 워크로드 대상이 마운트된 블록 디바이스인지 검증
 *
 * @td: 잡의 thread_data.
 * @return: true=마운트된 디바이스 발견(실행 중단 필요), false=안전.
 *
 * 왜 필요한가: /dev/sdX 등 블록 디바이스에 fio 가 쓰기를 하면 해당 디바이스에
 *   마운트된 파일시스템의 메타데이터/저널을 덮어써 FS 손상을 유발한다.
 *   치명적 실수를 방지하기 위해 이 검사를 기본 수행하고, 사용자가 의도한
 *   경우 allow_mounted_write=1 로 우회.
 *
 * 동작: for_each_file:
 *   FIO_HAVE_CHARDEV_SIZE 가 켜지면 캐릭터 디바이스도 검사(일부 디바이스가
 *   chrdev 형태로 노출되는 경우 대비). device_is_mounted(f->file_name) 로
 *   /proc/self/mountinfo 조회.
 *
 * 호출 체인: run_threads → check_mount_writes → device_is_mounted [lib/mountcheck.c].
 *
 * 에러 경로: 감지 시 log_err 로 경로 출력 후 true 반환 — run_threads 가
 *   run_threads 자체를 즉시 return 하여 어떤 잡도 시작하지 않는다.
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
 * [한국어]
 * waitee_running - wait_for 의존 잡이 아직 종료되지 않았는지 검사
 *
 * @me: 현재 잡의 thread_data(시작 대기자).
 * @return: true=의존 잡이 아직 TD_EXITED 미만 → 시작 지연 / false=시작 가능.
 *
 * 왜 필요한가: wait_for=<name> 옵션은 "<name> 잡이 완전히 끝난 뒤 시작" 이라는
 *   단방향 의존을 표현한다. run_threads 가 매 todo 순회에서 이 함수를 호출해
 *   의존 잡이 끝날 때까지 해당 잡 생성을 보류한다.
 *
 * 동작: for_each_td 순회 — 자기 자신이나 다른 이름이면 skip. waitee 이름
 *   일치하는 td 중 runstate<TD_EXITED 가 있으면 true.
 *
 * 한계: wait_for 그래프에 순환이 있으면 무한 대기. init 단계에서 체크 필요.
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
 * @sk_out: 서버 모드 소켓 출력 컨텍스트. 로컬 모드에서는 NULL. 각 잡 스레드에
 *          TLS 로 전달되어 log_info/__log_buf 출력이 서버 소켓으로 직렬화.
 *
 * @return: 없음. 에러 집계는 전역 exit_value, fio_abort 로. 마지막에 모든
 *          잡이 TD_REAPED 가 되면 정상 리턴.
 *
 * 왜 필요한가: fio 의 잡은 서로 다른 시작 조건(start_delay, stonewall,
 *   wait_for)을 가질 수 있고, 각기 다른 엔진/파일을 초기화하며, 병렬 혹은
 *   직렬로 실행되어야 한다. 또한 startup_sem 을 통한 초기화 rendezvous 와
 *   td->sem 을 통한 "동시 시작" 동기화를 관리해야 한다. 이 모든 오케스트레이션
 *   을 담는 단일 함수가 run_threads 이다.
 *
 * 실행 컨텍스트: fio_backend()가 메인 스레드에서 호출. 이 함수 자체는 새
 *   스레드를 만들지 않고, pthread_create/fork 로 잡 워커만 띄운다.
 *   시그널 핸들러가 등록된 후(set_sig_handlers) 의 메인 스레드가 여기 머물며
 *   잡을 감시하므로, SIGINT 가 오면 즉시 감지해 td->terminate 전파.
 *
 * 동기화 핵심: 각 잡 생성 후 startup_sem 으로 "초기화 완료" 를 기다리고,
 *   그 후 td->sem 을 up 하여 잡이 실제 I/O 루프를 시작하게 함. 이 2단계
 *   핸드셰이크로 "모든 잡이 준비되지 않은 상태에서 일부만 I/O 를 시작하는"
 *   경쟁을 방지하여 통계(start time 등)의 의미를 일관되게 유지.
 *
 * 호출 체인:
 *   fio_backend() → run_threads(sk_out)
 *     → set_sig_handlers() [이 파일]
 *     → setup_files() [filesetup.c] (create_serialize 시)
 *     → pthread_create/fork → thread_main [이 파일]
 *     → reap_threads() [이 파일] → waitpid(2) (fork 모드)
 *     → update_io_ticks() [diskutil.c]
 *
 * 에러 경로:
 *   - check_mount_writes 실패 → 즉시 return (어떤 잡도 시작 전).
 *   - pthread_create 실패 → for_each_td 루프 내 break, nr_started--.
 *   - 초기화 타임아웃(10초) → fio_terminate_threads + fio_abort=true → 탈출.
 *   - 잡 시작(5초 TD_INITIALIZED 대기) 타임아웃 → kill(td->pid, SIGTERM).
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
 * [한국어]
 * free_disk_util - 디스크 유틸 엔트리 정리 + 헬퍼 스레드 종료
 *
 * 호출자: fio_backend 후미 cleanup.
 * 동작: disk_util_prune_entries [diskutil.c] + helper_thread_destroy [helper_thread.c].
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
 * @sk_out: 서버 모드 소켓 출력 컨텍스트. NULL 이면 is_local_backend=true 로
 *          설정되어 CLI/로컬 잡 파일 실행 경로. 서버 모드에서 클라이언트가
 *          잡을 전달하면 server.c 가 sk_out 을 채워 이 함수를 호출.
 *
 * @return: fio 프로세스 종료 코드(main() 반환값 → 쉘 $?).
 *          - 0 = 모든 잡 성공
 *          - 128 = SIGINT 등 시그널로 중단
 *          - 1+N = 실패한 잡 수 누적 (exit_value)
 *
 * 왜 필요한가: fio.c 의 main() 은 argc/argv 파싱과 서버/클라이언트 분기만 담당
 *   하고, "실제로 잡을 수행" 하는 오케스트레이션을 이 함수로 위임한다. 따라서
 *   fio_backend 는 parse_options 이후 모든 백엔드 자원(세마포어/통계/헬퍼
 *   스레드/cgroup 리스트)을 준비하고, run_threads 를 거쳐 통계를 출력한 뒤
 *   자원을 정리하는 "잡 실행의 진정한 문" 역할을 한다.
 *
 * 실행 컨텍스트: main() 스레드에서 호출. 이 함수 자체는 직접 I/O 를 하지 않지만,
 *   run_threads 가 생성한 잡 스레드/프로세스와 helper 스레드가 동시 실행됨.
 *
 * 호출 체인:
 *   main() [fio.c] → fio_backend(sk_out)
 *     → load_profile [profile.c] (exec_profile 지정 시)
 *     → setup_log × 3 [iolog.c] (write_bw_log 시)
 *     → stat_init [stat.c]
 *     → helper_thread_create [helper_thread.c]
 *     → run_threads [이 파일]  ← 실제 잡 실행
 *     → helper_thread_exit [helper_thread.c]
 *     → __show_run_stats [stat.c]
 *     → fio_options_free / free_disk_util / cgroup_kill / stat_exit (cleanup)
 *
 * 에러 경로:
 *   - load_profile 실패 → return 1.
 *   - thread_number==0 (잡 없음) → return 0 (정상, 아무것도 할 일 없음).
 *   - startup_sem 할당 실패 → return 1.
 *   - init_global_dedupe_working_set_seeds 실패 → return 1.
 *   - 그 외 실패는 run_threads 내부에서 처리되어 exit_value 에 누적.
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
