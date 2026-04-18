/*
 * Native Solaris async IO engine
 *
 */

/*
 * [한국어 설명] Solaris 네이티브 AIO I/O 엔진 구현 (solarisaio.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Solaris 계열 운영체제(Solaris/illumos/OpenIndiana/SmartOS 등) 가 제공하는
 * 네이티브 비동기 I/O API(<sys/asynch.h> 의 aioread(3AIO)/aiowrite(3AIO)/aiowait(3AIO))
 * 위에 fio 의 ioengine 플러그인 계약을 구현한다. 이 API 는 POSIX 1003.1b aio_*
 * (aio_read/aio_write/aio_error/aio_return/aio_suspend; posixaio.c 가 사용) 와는 별개의
 * Solaris 고유 인터페이스이며, 완료 결과를 커널이 사용자 구조체 aio_result_t 에
 * 직접 쓰는 "콜백 없는 폴링" 모델이다. POSIX AIO 가 aio_error(&aiocb) 로 상태를
 * 조회하고 aio_return(&aiocb) 로 결과를 얻는 것과 달리, Solaris 네이티브 AIO 는
 * 사용자가 제출 시 넘긴 aio_result_t 구조체 자체에 aio_return/aio_errno 가 기록되며
 * aiowait(3AIO) 가 완료된 aio_result_t 의 포인터를 그대로 반환한다.
 * 완료 수확 전략은 두 가지: (1) 잡 스레드가 aiowait 로 타임아웃 기반 블로킹 수확,
 * (2) USE_SIGNAL_COMPLETIONS 빌드 플래그로 SIGIO 시그널 핸들러에서 비동기 수확.
 * 시스템 전역 상수 MAXASYNCHIO(보통 256) 가 허용 동시 요청 한도라 엔진 init 에서
 * td->o.iodepth 를 MAXASYNCHIO 로 자동 클램프한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: _start → main → fio_backend → load_ioengine("solarisaio") 가
 * fio_solarisaio_register() (constructor, fio_init 속성) 로 engine_list 에 등록한
 * ioengine_ops 를 잡 스레드의 td->io_ops 에 바인딩. 이후 fio 잡 루프가
 * td_io_init(=fio_solarisaio_init) → get_io_u → td_io_prep(=fio_solarisaio_prep) →
 * td_io_queue(=fio_solarisaio_queue) → td_io_getevents(=fio_solarisaio_getevents) →
 * td_io_event(=fio_solarisaio_event) → put_io_u → 반복 →
 * td_io_cleanup(=fio_solarisaio_cleanup) 순으로 호출한다. io_u 생명주기
 * (free→prepped→in_flight→completed→free) 에서 prep→queue 가 prepped→in_flight
 * 전이, getevents+event 가 in_flight→completed 전이를 담당한다.
 * 실행 컨텍스트는 주로 잡 스레드(pthread 또는 fork 자식 프로세스)이지만,
 * SIGIO 사용 시 **비동기 시그널 컨텍스트**(재진입 주의) 에서도 완료 수집 경로
 * (wait_for_event) 가 실행된다 — 이 때문에 write_barrier 를 사용한다.
 *
 * === 타 모듈과의 연결 ===
 * 상위: fio 코어(ioengines.c::td_io_*, io_u.c, backend.c::do_io) 가 이 파일의
 *       ioengine_ops 콜백을 호출.
 * 하위: Solaris C 라이브러리(libaio/libc) 의 aioread(3AIO)/aiowrite(3AIO)/
 *       aiowait(3AIO) 및 POSIX fsync(2)/fdatasync(2). aiosuspend(3AIO) 는 본 엔진이
 *       사용하지 않지만(aiowait 가 한 건씩 수확하는 "아무거나 대기" 모델과 달리
 *       aiosuspend 는 특정 aio_result_t 리스트 중 첫 완료를 대기), 개념 대비용으로만 언급.
 * 공유 상태: struct solarisaio_data 를 td->io_ops_data 에 저장. io_u->resultp
 *       (aio_result_t) 필드를 커널이 완료 시 갱신하며, fio 코어와 엔진이 이 멤버를
 *       통해 결과를 공유. io_u->engine_data 는 prep 에서 struct solarisaio_data*
 *       백포인터로 사용되어 wait_for_event 가 시그널 컨텍스트에서도 엔진 상태에 접근.
 * 데이터 흐름(READ):  잡 스레드 queue → aioread(fd, buf, len, off, SEEK_SET, &io_u->resultp)
 *       가 커널에 요청 → 커널이 I/O 완료 시 io_u->resultp.aio_return(바이트수/에러) 과
 *       aio_errno 를 채움 → aiowait(&tv) 가 해당 resultp 포인터를 반환 →
 *       container_of(res, struct io_u, resultp) 로 io_u 복원 → aio_events[] 배열에 push
 *       → fio 코어 event() 콜백이 하나씩 꺼내가며 put_io_u. WRITE 경로도 대칭이며
 *       aiowrite(3AIO) 를 사용. DDIR_SYNC/DATASYNC 는 POSIX AIO 가 aio_fsync(3) 로
 *       비동기 처리할 수 있는 것과 달리 Solaris 네이티브 AIO 에 fsync opcode 가 없어
 *       동기 fsync(2)/fdatasync(2) 로 폴백(큐 드레인 후 호출).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct solarisaio_data: aio_events(완료 io_u 배열) + aio_pending(아직 미반환 완료)
 *   + nr(in-flight) + max_depth(MAXASYNCHIO 클램프) 4필드로 잡 로컬 상태 관리.
 * - fio_solarisaio_init: solarisaio_data 할당, MAXASYNCHIO 기준 iodepth 하향, aio_events
 *   배열 calloc, (옵션) SIGIO 핸들러 설치, td->io_ops_data 등록.
 * - fio_solarisaio_prep: io_u->resultp.aio_return 을 AIO_INPROGRESS 로 초기화,
 *   engine_data 에 solarisaio_data* 백포인터 세팅(시그널 컨텍스트 접근용).
 * - fio_solarisaio_queue: ddir 분기 — DDIR_READ→aioread, DDIR_WRITE→aiowrite,
 *   DDIR_SYNC→드레인 후 fsync, DDIR_DATASYNC→드레인 후 fdatasync. max_depth 도달 시
 *   FIO_Q_BUSY 반환해 코어가 백프레셔.
 * - wait_for_event: aiowait(&tv) 한 건 수확 → container_of 로 io_u 복원 → resid/error
 *   계산 → aio_events[aio_pending++] 에 push. SIGIO 핸들러 경로와 공유.
 * - fio_solarisaio_getevents: aio_pending < min 동안 wait_for_event 반복, 스냅샷 반환.
 * - fio_solarisaio_event: aio_events[event] 직접 반환(getevents 직후 연속 호출).
 * - fio_solarisaio_cleanup: aio_events/solarisaio_data 해제.
 * - fio_solarisaio_sigio/init_sigio: USE_SIGNAL_COMPLETIONS 빌드 시 SIGIO 핸들러로
 *   즉시 완료 수확.
 * - fio_solarisaio_register/unregister: constructor/destructor — engine_list 등록/해제.
 */

#include <stdio.h>
/* [한국어] log_err/log_info 등 fio 로깅이 최종적으로 stdio 의 fprintf 계열을 사용.
 * 직접 호출은 없지만 fio.h 를 통해 끌려 들어오는 전역 버퍼링/가변인자 매크로가
 * 간접 의존한다. 빌드 시 stdio 선언 누락으로 인한 암시적 선언 경고를 막기 위해 명시. */

#include <stdlib.h>
/* [한국어] calloc(3) — fio_solarisaio_init 에서 solarisaio_data 와 aio_events 배열의
 * 0-초기화 할당에 사용. free(3) — fio_solarisaio_cleanup 에서 역순 해제.
 * exit(3) — wait_for_event 에서 aiowait 가 복구 불가 errno 로 실패할 때 즉시 종료. */

#include <unistd.h>
/* [한국어] fsync(2)/fdatasync(2) — DDIR_SYNC/DDIR_DATASYNC 경로에서 드레인 후 동기
 * 호출. Solaris 네이티브 AIO 에 비동기 fsync opcode 가 없어 폴백용으로 필요.
 * POSIX 기본 타입(ssize_t 등)도 이 헤더에서 공급. */

#include <signal.h>
/* [한국어] USE_SIGNAL_COMPLETIONS 빌드 분기에서 SIGIO 핸들러 설치용. 구체적으로
 * struct sigaction, sigaction(2), SA_RESTART 플래그, sa_handler 함수포인터 타입을 공급.
 * SIGIO 는 Solaris 커널이 AIO 완료 시 비동기 통지로 보낼 수 있는 시그널 번호로,
 * 이 경로를 쓰면 잡 스레드가 aiowait 명시 호출 없이도 핸들러에서 수확 가능. */

#include <errno.h>
/* [한국어] wait_for_event 에서 aiowait 가 -1 을 반환할 때 errno 로 원인 판별.
 * 특히 EINVAL(대기할 요청 없음; 정상 조기반환) 과 그 외(치명; exit) 분기를 위해 필요. */

#include "../fio.h"
/* [한국어] fio 코어 공용 선언을 일괄 공급 — thread_data(잡 컨텍스트), io_u(I/O 유닛,
 * 본 엔진이 사용하는 io_u->resultp/engine_data/xfer_buf/xfer_buflen/offset/ddir/file
 * 필드 전부), struct ioengine_ops(플러그인 vtable), enum fio_ddir(DDIR_READ/WRITE/
 * SYNC/DATASYNC/TRIM), enum fio_q_status(FIO_Q_COMPLETED/QUEUED/BUSY), fio_ro_check
 * (readonly 잡에서 write 금지 assert), fio_init/fio_exit 속성 매크로(GCC constructor/
 * destructor), register_ioengine/unregister_ioengine, generic_open_file/close_file/
 * get_file_size(파일 수명 공용 콜백), container_of 매크로(리눅스 커널 이디엄,
 * 멤버 포인터→감싸는 구조체 포인터 복원), write_barrier(컴파일러+CPU 메모리 배리어),
 * td_verror(에러 보고), log_err/log_info. os/os-solaris.h 를 간접 포함해 os_aiocb_t
 * 같은 플랫폼 typedef 및 MAXASYNCHIO 기본값도 끌어온다. */

#include <sys/asynch.h>
/* [한국어] Solaris 네이티브 AIO 공용 헤더. 본 엔진의 핵심 API 를 공급:
 *   - aio_result_t: { aio_return(ssize_t; 전송 바이트 또는 -1), aio_errno(int; 에러) }
 *     커널이 완료 시 직접 기록. POSIX aiocb 와 달리 요청 메타(fd/buf/len/off) 를
 *     담지 않는 순수 결과 구조체.
 *   - aioread(int fd, char *buf, int bufsz, off_t off, int whence, aio_result_t *resp):
 *     비동기 read 제출. 성공 시 0, 실패 시 -1+errno.
 *   - aiowrite(int fd, char *buf, int bufsz, off_t off, int whence, aio_result_t *resp):
 *     비동기 write 제출.
 *   - aiowait(struct timeval *tv): 아무 AIO 완료 1건 대기. tv=NULL 이면 무한 블록,
 *     tv={0,0} 이면 폴링, 그 외면 타임아웃. 반환은 완료된 aio_result_t* (사용자가
 *     제출 시 넘긴 포인터) 또는 -1(errno) 또는 NULL(타임아웃).
 *   - AIO_INPROGRESS: aio_return 의 "진행 중" 마커 상수. prep 에서 커널이 덮어쓰기
 *     전 초기값으로 사용.
 *   - MAXASYNCHIO: 프로세스당 동시 AIO 최대 요청 수(시스템 상수, 보통 256).
 *     iodepth 클램프에 사용.
 * POSIX AIO(aio.h) 의 aio_read/aio_write/aio_error/aio_return/aio_suspend 와는
 * 함수명·시그니처가 모두 다른 별개 API 이니 주의. */

/*
 * [한국어] struct solarisaio_data — Solaris AIO 엔진의 잡 로컬 상태.
 *
 * 수명: fio_solarisaio_init 에서 calloc 으로 할당되어 td->io_ops_data 에 부착.
 *       fio_solarisaio_cleanup 에서 해제.
 * 소유권: 한 잡 스레드가 단독 소유. 그러나 USE_SIGNAL_COMPLETIONS 빌드 시에는
 *         SIGIO 핸들러(비동기 시그널 컨텍스트) 도 wait_for_event 를 경유해
 *         aio_events/aio_pending/nr 을 변경하므로, 변경 순서 보장을 위해
 *         write_barrier() 를 삽입한다. 핸들러와 잡 스레드는 동일 CPU 에서
 *         번갈아 실행되므로 락 대신 배리어로 충분(단일 CPU 가시성).
 */
struct solarisaio_data {
	struct io_u **aio_events;
	/* [한국어] 완료된 io_u 포인터를 쌓아두는 배열(크기 max_depth, FIFO 인덱싱).
	 * 설정자: wait_for_event 가 sd->aio_pending 번째 슬롯에 io_u 를 저장.
	 * 읽는 자: fio_solarisaio_event(td, event) 가 aio_events[event] 로 꺼냄
	 *         (getevents 직후 0..ret-1 연속 인덱스).
	 * 값 범위: 유효한 io_u 포인터(NULL 불가, 완료 구간 내에서만 유효).
	 *         배열 슬롯 [0..aio_pending) 가 유효 구간.
	 * 동기화: 단일 생산자(wait_for_event) · 단일 소비자(getevents) 패턴.
	 *         USE_SIGNAL_COMPLETIONS 빌드 시 생산자가 시그널 핸들러가 되므로,
	 *         aio_events 스토어가 aio_pending++ 보다 먼저 관찰되도록
	 *         write_barrier 로 순서 보장. */

	unsigned int aio_pending;
	/* [한국어] aio_events 배열에 쌓였지만 아직 fio 코어가 수확해 가지 않은
	 *         완료 건수(pending-for-fio 카운터).
	 * 설정자: wait_for_event 가 ++ (완료 수확 시). fio_solarisaio_getevents 가
	 *         -= ret (스냅샷 만큼 일괄 소비).
	 * 읽는 자: fio_solarisaio_getevents 의 while(sd->aio_pending < min) 루프
	 *         탈출 조건, 스냅샷 ret 계산.
	 * 값 범위: 0..max_depth. 동일 시점에 aio_events[0..aio_pending) 가 유효.
	 * 동기화: 잡 스레드가 감소시키고, 잡 스레드 또는 SIGIO 핸들러가 증가시킴.
	 *         write_barrier 로 aio_events 스토어와 순서 보장. unsigned int 원자 R/W
	 *         는 Solaris/SPARC/x86 에서 모두 단일 워드 align 전제로 tearing 없음. */

	unsigned int nr;
	/* [한국어] 현재 커널에 제출되어 아직 완료 통지를 받지 못한 in-flight AIO 개수.
	 * 설정자: fio_solarisaio_queue 가 aioread/aiowrite 성공 시 ++.
	 *         wait_for_event 가 완료 1건 수집할 때마다 --.
	 * 읽는 자: fio_solarisaio_queue 의 DDIR_SYNC/DATASYNC 분기에서 "0 이어야
	 *         동기 fsync/fdatasync 수행" 드레인 판정. max_depth 비교로
	 *         FIO_Q_BUSY 백프레셔 적용.
	 * 값 범위: 0..max_depth.
	 * 동기화: aio_pending 과 동일(write_barrier + 단일 워드 원자성 가정). */

	unsigned int max_depth;
	/* [한국어] 본 잡이 허용하는 동시 AIO 최대 개수. min(td->o.iodepth, MAXASYNCHIO).
	 * 설정자: fio_solarisaio_init 에서 1회 설정 후 불변.
	 * 읽는 자: fio_solarisaio_queue 의 "sd->nr == sd->max_depth → FIO_Q_BUSY"
	 *         백프레셔 판정.
	 * 값 범위: 1..MAXASYNCHIO(보통 256). 0 이면 사실상 아무 것도 제출 불가.
	 * 동기화: init 이후 read-only 라 동기화 불필요. */
};

/*
 * [한국어]
 * fio_solarisaio_prep - io_u 한 건을 AIO 제출 직전에 초기화.
 *
 * @td: 잡 스레드 thread_data. 시그니처에 fio_unused 가 붙어 있지만 실제로는
 *      td->io_ops_data 에 저장된 struct solarisaio_data* 를 꺼내 사용한다
 *      (fio_unused 는 일부 엔진에서 td 를 안 쓸 때 컴파일러 경고를 억제하는 매크로).
 * @io_u: 준비할 I/O 유닛. io_u->resultp 필드(aio_result_t) 가 이 함수의 주요 타깃.
 * @return: 항상 0(성공). 현 구현은 실패 경로 없음.
 *
 * 왜 필요한가:
 *   1) aiowait 이후 container_of(res, struct io_u, resultp) 로 감싸는 io_u 포인터를
 *      복원하려면, io_u->resultp 가 io_u 구조체 내부에 인라인으로 박혀 있어야 한다.
 *      io_u 는 fio 코어가 공급하므로 resultp 는 io_u 전용 멤버로 이미 예약되어 있고,
 *      여기서는 그 값을 AIO_INPROGRESS 로 리셋해 이전 I/O 의 잔재를 지운다.
 *   2) io_u->engine_data 에 struct solarisaio_data* 백포인터를 걸어두면,
 *      wait_for_event 가 완료된 io_u 에서 엔진 상태(특히 aio_events 배열과
 *      카운터) 에 접근할 수 있다. SIGIO 시그널 핸들러에서도 이 경로가 필요하다
 *      (시그널 컨텍스트에서는 td 가 명시적으로 전달되지 않음).
 *
 * 실행 컨텍스트: 잡 스레드. td_io_prep 이 get_io_u 후, td_io_queue 직전에 호출.
 *                 io_u 상태 전이: free → prepped.
 * 호출자: ioengines.c::td_io_prep → ops->prep
 * 호출 대상: 없음(단순 필드 세팅).
 * 에러 경로: 없음(항상 성공).
 *
 * 호출 체인:
 *   backend.c::do_io → get_io_u → td_io_prep → [fio_solarisaio_prep]
 */
static int fio_solarisaio_prep(struct thread_data fio_unused *td,
			    struct io_u *io_u)
{
	struct solarisaio_data *sd = td->io_ops_data;
	/* [한국어] td->io_ops_data 에서 엔진 상태 포인터 획득. fio_solarisaio_init 에서
	 *         세팅되어 잡 수명 동안 불변. prep/queue/getevents/event/cleanup 모두
	 *         이 경로로 상태에 접근한다. */

	io_u->resultp.aio_return = AIO_INPROGRESS;
	/* [한국어] aio_result_t.aio_return 을 "진행 중" 센티넬 값으로 초기화.
	 *         커널은 aioread/aiowrite 완료 시 이 필드를 실제 전송 바이트수(>=0)
	 *         또는 실패 표시(-1)로 덮어쓴다. 초기화하지 않으면 이전 I/O 의 결과가
	 *         남아 있을 수 있어 잘못된 완료 판정 위험. POSIX AIO 의 aio_error()
	 *         함수에 해당하는 폴링 루틴을 이 엔진은 사용하지 않고, 대신 aiowait
	 *         가 "완료된 것만" 반환하므로 센티넬 검사 불필요. */

	io_u->engine_data = sd;
	/* [한국어] io_u 에 엔진 상태 백포인터 저장. 용도:
	 *         - wait_for_event 가 container_of 로 io_u 를 복원한 뒤,
	 *           io_u->engine_data 에서 struct solarisaio_data* 를 꺼내
	 *           aio_events/aio_pending/nr 에 접근.
	 *         - SIGIO 핸들러(fio_solarisaio_sigio) 에서 td 가 명시적으로 전달되지
	 *           않지만, 완료된 io_u 만 있으면 engine_data 로 엔진 상태 복원 가능. */
	return 0;
	/* [한국어] 현 구현은 실패 경로가 없으므로 항상 성공. */
}

/*
 * [한국어]
 * wait_for_event - aiowait(3AIO) 로 완료 1건을 수확해 aio_events 배열에 쌓는다.
 *
 * @tv: 타임아웃. NULL → 무한 대기(SIGIO 핸들러 호출 경로에서 사용).
 *      {0,0} → 폴링(비블록, 즉시 반환).
 *      그 외 → 지정 시간만큼 대기.
 *
 * 동작 단계:
 *   1) aiowait(tv) 호출 — 커널에서 "아무거나 완료된 한 건" 의 aio_result_t*
 *      를 반환(또는 -1/NULL).
 *   2) 반환값 검사 — (aio_result_t*)-1 이면 errno 판별, NULL 이면 타임아웃.
 *   3) container_of(res, struct io_u, resultp) 로 감싸는 io_u 를 복원
 *      (Linux 커널 이디엄, offsetof 기반 포인터 역산).
 *   4) io_u->engine_data 로 엔진 상태 sd 복원.
 *   5) aio_return 부호로 성공/실패 분기 — 성공 시 resid 계산, 실패 시 errno 이관.
 *   6) aio_events[sd->aio_pending] 에 저장, write_barrier, aio_pending++, nr--.
 *
 * 실행 컨텍스트:
 *   - (기본) 잡 스레드에서 fio_solarisaio_getevents 가 while 루프로 호출.
 *   - (USE_SIGNAL_COMPLETIONS 빌드) SIGIO 시그널 핸들러 fio_solarisaio_sigio
 *     에서도 호출. 시그널 컨텍스트는 비동기 재진입 지점이므로 aiowait 자체의
 *     async-signal-safety 가 전제된다(Solaris 에서 aiowait 는 시그널 안전 목록).
 *
 * 에러 처리:
 *   - EINVAL: 대기할 요청이 없는 정상 조기 반환(보통 queue 전에 getevents 가
 *     호출되거나 SIGIO 핸들러가 빈 큐 상태에서 실행됐을 때).
 *   - 기타 errno: 복구 불가능으로 판정 → log_err + exit(err) 즉시 종료
 *     (stale in-flight 상태를 잡에 전파하면 해석 불가능한 잔여 I/O 발생).
 *
 * POSIX AIO 와의 차이:
 *   POSIX AIO 의 aio_suspend 는 "특정 aiocb 리스트에서 최소 1건 완료 대기" 로
 *   호출자가 대기 대상을 명시해야 한다. 반면 Solaris aiowait 는 "프로세스 전역에서
 *   완료된 아무 것" 을 반환하므로 대기 대상 리스트 관리가 불필요 — 본 엔진이
 *   aio_events 만 유지하고 별도의 "in-flight 리스트" 를 안 갖는 이유.
 *
 * 호출 체인:
 *   fio_solarisaio_getevents → [wait_for_event] → aiowait(3AIO)
 *   fio_solarisaio_sigio     → [wait_for_event] → aiowait(3AIO)  (USE_SIGNAL_COMPLETIONS)
 */
static void wait_for_event(struct timeval *tv)
{
	struct solarisaio_data *sd;
	/* [한국어] 완료 io_u 에서 역추적할 엔진 상태. 함수 진입 시점에는 NULL
	 *         (아직 io_u 를 수확하지 않았음), aiowait 성공 후 io_u->engine_data
	 *         에서 대입. */
	struct io_u *io_u;
	/* [한국어] 완료된 I/O 유닛. container_of 로 resultp 주소에서 복원. */
	aio_result_t *res;
	/* [한국어] aiowait 의 반환 — 완료된 aio_result_t 포인터. 이 포인터는
	 *         fio_solarisaio_queue 가 aioread/aiowrite 에 &io_u->resultp 로 넘긴
	 *         바로 그 주소이므로, io_u 로 복원 가능. */

	res = aiowait(tv);
	/* [한국어] Solaris 커널에서 완료 1건 수확. tv=NULL → 무한 블록,
	 *         tv={0,0} → 폴링(즉시 반환), 기타 → 타임아웃 대기. 이 함수가 본 엔진의
	 *         유일한 "완료 이벤트 획득" 진입점. aiosuspend(3AIO) 는 사용하지 않음. */
	if (res == (aio_result_t *) -1) {
		/* [한국어] Solaris aiowait 의 실패 반환 규약 — (aio_result_t*)-1 이 실패 센티넬.
		 *         NULL 은 타임아웃을 의미하므로 -1 과 별개로 처리해야 함. */
		int err = errno;
		/* [한국어] errno 를 즉시 지역에 캡처 — 이후 log_err 내부에서 malloc/syscall
		 *         이 errno 를 덮어쓸 가능성 차단. */

		if (err != EINVAL) {
			/* [한국어] EINVAL 은 "대기할 AIO 가 아무 것도 없음" 의 정상 상태 코드
			 *         (예: 아직 queue 를 한 번도 부르지 않은 시점에 getevents 호출,
			 *         또는 모든 completion 을 이미 수확한 뒤 SIGIO 핸들러 재진입).
			 *         그 외의 errno 는 커널이 내부 상태 오류를 보고한 것이므로
			 *         복구 불가능으로 판정. */
			log_err("fio: solarisaio got %d in aiowait\n", err);
			/* [한국어] fio 의 stderr 로깅 채널로 원인 errno 기록. */
			exit(err);
			/* [한국어] 프로세스 즉시 종료. errno 값을 종료 코드로 전파해
			 *         상위 스크립트가 원인 식별 가능. stale in-flight 상태를
			 *         다른 잡 스레드에 전파하지 않기 위한 보수적 선택. */
		}
		return;
		/* [한국어] EINVAL → 대기할 요청 없었음. 호출자에게 "수확 0건" 을 알리기 위해
		 *         조용히 조기 반환. getevents 의 while 루프는 aio_pending 변화 없이
		 *         다음 반복으로 진행하며, 새 queue 가 들어올 때까지 blocking 시도 반복. */
	} else if (!res)
		/* [한국어] NULL 반환 → tv 에 지정한 타임아웃이 만료되어 완료 없이 복귀.
		 *         tv=NULL 경로에서는 발생하지 않음(무한 대기이므로). */
		return;

	io_u = container_of(res, struct io_u, resultp);
	/* [한국어] Linux 커널 이디엄: 구조체 멤버 포인터 → 감싸는 구조체 포인터.
	 *         실제 계산: (struct io_u*)((char*)res - offsetof(struct io_u, resultp)).
	 *         prep 에서 aioread/aiowrite 에 넘긴 &io_u->resultp 가 완료 통지로
	 *         돌아왔으므로, 같은 offset 을 빼면 감싸는 io_u 를 복원할 수 있다.
	 *         POSIX AIO 가 io_u 순회로 aiocb 매칭을 찾는 것(posixaio.c) 과 달리
	 *         O(1) 로 즉시 복원 가능한 장점. */
	sd = io_u->engine_data;
	/* [한국어] prep 에서 저장해둔 struct solarisaio_data* 백포인터.
	 *         이 경로로 aio_events 배열과 카운터에 접근. SIGIO 핸들러 경로에서도
	 *         td 없이 엔진 상태에 도달할 수 있게 해주는 핵심 설계. */

	if (io_u->resultp.aio_return >= 0) {
		/* [한국어] aio_return >= 0 → 성공 또는 부분 전송(요청량보다 적게 완료).
		 *         POSIX read/write 시맨틱과 동일하게 비음수 = 바이트수. */
		io_u->resid = io_u->xfer_buflen - io_u->resultp.aio_return;
		/* [한국어] 요청 길이 - 실제 전송 = 잔여(미전송) 바이트. 완전 성공이면 0.
		 *         fio 코어가 io_u->resid 를 보고 부분 전송 통계 처리, verify 시
		 *         전송된 영역만 검증. */
		io_u->error = 0;
		/* [한국어] 에러 없음. fio 코어는 io_u->error == 0 을 성공으로 해석. */
	} else
		io_u->error = io_u->resultp.aio_errno;
		/* [한국어] aio_return < 0 → 실패. 커널이 aio_errno 에 기록한 errno
		 *         (EIO/EFAULT/ENOSPC 등)를 io_u->error 로 이관. fio 는 td_verror
		 *         유사 경로로 이 에러를 누적해 최종 리포트. POSIX AIO 의 aio_error()
		 *         함수 호출에 해당하는 값을 aio_result_t 에서 직접 읽는 점이 특징. */

	/*
	 * For SIGIO, we need a write barrier between the two, so that
	 * the ->aio_pending store is seen after the ->aio_events store
	 */
	sd->aio_events[sd->aio_pending] = io_u;
	/* [한국어] 완료된 io_u 포인터를 aio_events 배열 다음 슬롯에 저장.
	 *         이 시점에 aio_pending 은 아직 증가 전이므로, 소비자(getevents) 는
	 *         이 슬롯을 유효 구간으로 보지 않는다. */
	write_barrier();
	/* [한국어] 컴파일러 + CPU 메모리 배리어. SIGIO 핸들러와 잡 스레드가 같은 CPU 에서
	 *         선점적으로 실행될 때, (1) aio_events[i] = io_u 스토어가 (2) aio_pending++
	 *         스토어보다 먼저 관찰되도록 보장. 배리어 없으면 x86 TSO 에서도
	 *         컴파일러 재배열로 순서가 뒤집혀, 소비자가 아직 쓰이지 않은 슬롯을
	 *         유효로 오판할 수 있음. SPARC TSO/x86 모두 store-store 재배열은
	 *         하드웨어 레벨로 막지만 컴파일러 배리어는 필요. */
	sd->aio_pending++;
	/* [한국어] 배열에 push 한 항목만큼 pending 카운터 증가. 소비자(getevents) 가
	 *         이 값을 스냅샷해 한꺼번에 차감 후 반환. */
	sd->nr--;
	/* [한국어] in-flight 카운트 감소. max_depth 비교를 통해 queue 에서 백프레셔
	 *         해제됨(이제 새 요청 제출 여유 생김). */
}

/*
 * [한국어]
 * fio_solarisaio_getevents - 완료 배치 수확 콜백(ioengine_ops::getevents).
 *
 * @td: 잡 thread_data. td->io_ops_data 에서 엔진 상태 획득.
 * @min: 최소 수확 개수. 0 이면 폴링(즉시 반환 허용).
 * @max: 최대 수확 개수. **현 구현은 max 를 사용하지 않고** aio_pending 전량을
 *       반환한다 — aio_events 배열 크기가 max_depth(≤iodepth) 로 제한되어
 *       반환량이 max 를 크게 초과할 위험이 없다고 보는 설계.
 * @t: 타임아웃(struct timespec; nanosecond 해상도). NULL 이면 "제한 없음" 대신
 *     폴링으로 간주(아래 if 분기 참고).
 * @return: 이번 호출에서 fio 코어에 돌려주는 완료 개수.
 *
 * 동작:
 *   1) timespec(ns) → timeval(us) 변환(aiowait 인터페이스가 timeval 요구).
 *      min==0 또는 t==NULL 이면 tv={0,0} 으로 폴링.
 *   2) while(aio_pending < min) 루프로 wait_for_event 를 반복 호출.
 *      각 호출은 최대 tv 만큼만 블록하므로, 반복 간 느린 주체가 있을 경우에도
 *      "무한 블록" 이 아닌 점증적 수확을 제공.
 *   3) aio_pending 스냅샷 후 전량 차감(= 배열을 비움), 스냅샷 반환.
 *
 * 실행 컨텍스트: 잡 스레드. SIGIO 모드에서는 신호 핸들러가 병렬로 aio_pending
 *                을 키울 수 있음(잡 스레드 while 루프 중 선점 가능). 본 함수는
 *                단일 소비자이므로 -= ret 는 안전하다(원자 단일 워드 R/W 가정).
 *
 * 호출 체인:
 *   backend.c::io_u_queued_complete → ioengines.c::td_io_getevents →
 *   [fio_solarisaio_getevents] → wait_for_event → aiowait(3AIO)
 */
static int fio_solarisaio_getevents(struct thread_data *td, unsigned int min,
				    unsigned int max, const struct timespec *t)
{
	struct solarisaio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태 포인터 획득. init 에서 세팅되어 불변. */
	struct timeval tv;
	/* [한국어] aiowait 에 전달할 타임아웃 버퍼. 초(tv_sec) + 마이크로초(tv_usec).
	 *         Solaris aiowait 시그니처가 struct timeval* 이라 POSIX timespec
	 *         (ns 해상도) 에서 usec 해상도로 다운컨버전 필요. */
	int ret;
	/* [한국어] fio 코어에 반환할 수확 스냅샷. aio_pending 의 "-= ret" 차감값과
	 *         return 값 모두 이 변수로 관리. */

	if (!min || !t) {
		/* [한국어] polling 요청. min==0 은 "완료가 있으면 챙기되 없으면 즉시 반환"
		 *         시맨틱. t==NULL 은 호출자가 타임아웃 의사를 표명하지 않음 — 본
		 *         엔진은 이를 보수적으로 폴링으로 해석해 드라이버 블록을 피한다. */
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		/* [한국어] {0,0} → aiowait 즉시 반환(완료 1건 이상 있으면 반환, 없으면 NULL). */
	} else {
		/* [한국어] min>0 + t 유효 → 블록 대기 타임아웃 지정. */
		tv.tv_sec = t->tv_sec;
		/* [한국어] 초 단위 그대로 복사. */
		tv.tv_usec = t->tv_nsec / 1000;
		/* [한국어] timespec.tv_nsec(ns) → timeval.tv_usec(us) 변환. 나머지 나노초
		 *         단위는 버림(Solaris aiowait 는 us 해상도만 해석). */
	}

	while (sd->aio_pending < min)
		/* [한국어] 최소 요구 건수에 도달할 때까지 반복 수확. wait_for_event 가
		 *         완료 1건을 aio_events 에 push 하며 aio_pending 을 증가시킴.
		 *         tv 가 {0,0} 이면 "있는 만큼만 긁어가고 바로 탈출" 로 효과적인
		 *         폴링. tv 가 유효 타임아웃이면 각 호출이 그 한도 내 대기. */
		wait_for_event(&tv);

	/*
	 * should be OK without locking, as int operations should be atomic
	 */
	ret = sd->aio_pending;
	/* [한국어] 현재 보유 완료 수 스냅샷. 이 스냅샷 시점에 SIGIO 핸들러가 추가로
	 *         완료를 push 해도 상관없음 — 다음 getevents 호출에서 수확. */
	sd->aio_pending -= ret;
	/* [한국어] 스냅샷만큼 차감. 0 이 되는 게 일반적이나, SIGIO 핸들러가 ret 계산과
	 *         이 라인 사이에 추가로 증가시켰다면 잔여가 남음(그래서 = 0 이 아니라
	 *         -= ret). int 단일 워드 R/W 원자성 가정으로 락 생략. */
	return ret;
	/* [한국어] fio 코어에 수확 개수 전달. 코어는 이 값으로 event() 콜백을
	 *         0..ret-1 인덱스에 대해 반복 호출해 각 io_u 를 완료 처리. */
}

/*
 * [한국어]
 * fio_solarisaio_event - getevents 가 수확한 배열에서 event 번째 io_u 반환
 *                         (ioengine_ops::event 콜백).
 *
 * @td: 잡 thread_data.
 * @event: 0 <= event < 직전 getevents 반환값. fio 코어가 0,1,2,... 로 순차 증가.
 * @return: 해당 인덱스의 완료 io_u 포인터.
 *
 * 실행 컨텍스트: 잡 스레드. getevents 직후 "getevents 반환값 ret" 만큼
 *                 event(0), event(1), ..., event(ret-1) 으로 연속 호출됨.
 *                 이 사이에 aio_events 배열을 재사용하면 안 되며,
 *                 다음 getevents 호출 전까지만 유효.
 *
 * 호출 체인:
 *   backend.c::ios_completed → ioengines.c::td_io_event → [fio_solarisaio_event]
 */
static struct io_u *fio_solarisaio_event(struct thread_data *td, int event)
{
	struct solarisaio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태 포인터 획득. */

	return sd->aio_events[event];
	/* [한국어] wait_for_event 가 저장해둔 완료 io_u 포인터를 직접 반환.
	 *         배열 인덱스 경계 체크 없음 — fio 코어가 getevents 가 반환한
	 *         ret 범위 내에서만 호출함을 전제. */
}

/*
 * [한국어]
 * fio_solarisaio_queue - I/O 제출 콜백(ioengine_ops::queue).
 *
 * @td: 잡 thread_data. td->io_ops_data 에서 엔진 상태 획득.
 * @io_u: 제출할 I/O 유닛. prep 에서 resultp/engine_data 이미 초기화됨.
 * @return: FIO_Q_QUEUED(비동기 성공) / FIO_Q_COMPLETED(동기 완료 또는 즉시 에러)
 *          / FIO_Q_BUSY(큐 full / drain 대기 중이라 코어가 재시도해야 함).
 *
 * ddir 별 분기:
 *   DDIR_READ  → aioread(3AIO) 로 네이티브 비동기 제출. 완료는 aiowait 에서.
 *   DDIR_WRITE → aiowrite(3AIO) 로 네이티브 비동기 제출.
 *   DDIR_SYNC  → in-flight 전부 드레인 후 fsync(2) 동기 호출. 비동기 opcode 없음.
 *   DDIR_DATASYNC → 드레인 후 fdatasync(2) 동기 호출.
 *   (DDIR_TRIM 은 본 엔진이 지원하지 않음 — fio 코어가 ASYNCIO_SYNC_TRIM 같은
 *    플래그로 동기 폴백을 강제하지 않으므로, TRIM 잡 설정 시 문제 발생 가능.)
 *
 * 백프레셔:
 *   - sd->nr == sd->max_depth → FIO_Q_BUSY. fio 코어는 큐 포화로 해석해 commit
 *     후 getevents 로 완료를 수확한 뒤 재호출.
 *   - DDIR_SYNC/DATASYNC 는 **드레인 요구**: sd->nr != 0 이면 FIO_Q_BUSY.
 *     fsync/fdatasync 가 진행 중인 비동기 I/O 를 커널 레벨에서 waiting 하지
 *     못하므로(Solaris 네이티브 AIO 는 aio_fsync 미지원), 안전을 위해 엔진이
 *     명시적으로 in-flight=0 까지 기다린 뒤 호출한다.
 *
 * 에러 처리:
 *   - aioread/aiowrite 가 -1 을 반환(제출 실패) → io_u->error=errno, td_verror 등록,
 *     FIO_Q_COMPLETED 반환(코어가 error 를 보고 실패로 처리).
 *   - fsync/fdatasync 실패 → io_u->error=errno 만 기록(큐잉 개념이 없음),
 *     FIO_Q_COMPLETED 반환.
 *
 * 실행 컨텍스트: 잡 스레드. 시그널 핸들러에서 호출되지 않음.
 *
 * 호출 체인:
 *   backend.c::do_io → ioengines.c::td_io_queue → [fio_solarisaio_queue] →
 *   aioread(3AIO) / aiowrite(3AIO) / fsync(2) / fdatasync(2)
 */
static enum fio_q_status fio_solarisaio_queue(struct thread_data fio_unused *td,
			      struct io_u *io_u)
{
	struct solarisaio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태. nr/max_depth 비교, 성공 시 nr++ 에 사용. */
	struct fio_file *f = io_u->file;
	/* [한국어] 대상 파일. f->fd 가 aioread/aiowrite/fsync/fdatasync 의 첫 인자.
	 *         generic_open_file 이 열어둔 fd 사용. */
	off_t off;
	/* [한국어] 파일 내 요청 오프셋. io_u->offset 에서 복사. aioread/aiowrite 의
	 *         4번째 인자(off_t) 로 전달되며, SEEK_SET 과 조합되어 절대 오프셋 의미. */
	int ret;
	/* [한국어] aioread/aiowrite 제출 반환값. 0=성공(큐잉), -1=실패(errno). */

	fio_ro_check(td, io_u);
	/* [한국어] read-only 잡(td->o.read_only) 에서 DDIR_WRITE/TRIM 시도 시 assert 실패.
	 *         잘못된 워크로드 구성을 조기에 잡기 위한 가드. fio.h 매크로. */

	if (io_u->ddir == DDIR_SYNC) {
		/* [한국어] 전체 파일 동기화 요청(메타+데이터). POSIX fsync(2) 에 대응.
		 *         Solaris 네이티브 AIO 에 비동기 fsync 가 없어 동기 폴백 필요. */
		if (sd->nr)
			/* [한국어] 아직 떠 있는 비동기 AIO 가 있음 → fsync 가 이들을
			 *         waiting 하지 못하므로 드레인 요구. */
			return FIO_Q_BUSY;
			/* [한국어] 코어가 getevents 로 in-flight 를 비운 뒤 재호출하게 됨. */
		if (fsync(f->fd) < 0)
			/* [한국어] 커널 호출: 파일 전체(메타 inode 블록, 데이터 블록) 를
			 *         스토리지에 영구 기록. ext/ZFS 등 FS 별 구현 차이 있으나
			 *         POSIX 시맨틱 동일. */
			io_u->error = errno;
			/* [한국어] 실패 시 errno 저장(코어가 error != 0 으로 실패 처리). */

		return FIO_Q_COMPLETED;
		/* [한국어] 동기 호출로 즉시 완료 — 큐잉 없이 이 자리에서 종결. */
	}

	if (io_u->ddir == DDIR_DATASYNC) {
		/* [한국어] 데이터만 동기화(메타데이터 제외). POSIX fdatasync(2) 대응.
		 *         파일 크기 변경 같은 메타는 건너뛰어 fsync 보다 빠름. */
		if (sd->nr)
			return FIO_Q_BUSY;
			/* [한국어] DDIR_SYNC 와 동일한 드레인 요구. */
		if (fdatasync(f->fd) < 0)
			io_u->error = errno;
			/* [한국어] fdatasync 실패 errno 저장. */

		return FIO_Q_COMPLETED;
	}

	if (sd->nr == sd->max_depth)
		/* [한국어] in-flight 가 허용 최대치(MAXASYNCHIO 클램프값) 에 도달.
		 *         더 제출하면 커널이 ENOMEM/EAGAIN 반환할 위험이 있어 선제적으로
		 *         백프레셔. */
		return FIO_Q_BUSY;
		/* [한국어] 코어가 다음 getevents 로 완료를 수확하면 nr 이 감소해 재시도 성공. */

	off = io_u->offset;
	/* [한국어] 오프셋 로드. off_t 는 64-bit 파일 지원 위해 ILP32+LFS 환경에서 long long. */
	if (io_u->ddir == DDIR_READ)
		/* [한국어] 읽기 요청. */
		ret = aioread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, off,
					SEEK_SET, &io_u->resultp);
		/* [한국어] Solaris 네이티브 비동기 read 제출.
		 *           인자: (fd, 사용자버퍼, 바이트수, 오프셋, whence=절대, 결과포인터)
		 *           SEEK_SET → off 가 파일 시작 기준 절대 오프셋.
		 *           &io_u->resultp → 커널이 완료 시 이 구조체에 aio_return/aio_errno 기록.
		 *           호출 성공 시 반환=0(큐잉됨), 실패 시 -1+errno. */
	else
		/* [한국어] DDIR_WRITE (나머지 ddir 중 여기 도달하는 것은 WRITE 만).
		 *         DDIR_TRIM 은 지원하지 않아 도달하면 aiowrite 로 처리되지만 정상 워크로드
		 *         에서는 발생하지 않음. */
		ret = aiowrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, off,
					SEEK_SET, &io_u->resultp);
		/* [한국어] 네이티브 비동기 write 제출. aioread 와 시그니처/시맨틱 대칭. */
	if (ret) {
		/* [한국어] 제출 자체 실패(ret != 0 → 통상 -1). 큐잉 전에 거절된 경우. */
		io_u->error = errno;
		/* [한국어] 커널이 세팅한 errno(EAGAIN/EBADF/EFAULT 등) 를 io_u 로 이관. */
		td_verror(td, io_u->error, "xfer");
		/* [한국어] fio 코어의 에러 리포팅 훅. "xfer" 라벨로 stderr/summary 에 기록. */
		return FIO_Q_COMPLETED;
		/* [한국어] 큐잉 실패 → 이 I/O 는 즉시 에러 완료로 간주. 코어가 error 를
		 *         보고 실패 경로 진입(continue_on_error 등 정책에 따라 처리). */
	}

	sd->nr++;
	/* [한국어] 제출 성공 → in-flight 카운트 증가. 다음 queue 호출의 백프레셔
	 *         판정 및 DDIR_SYNC/DATASYNC 드레인 판정에 사용. */
	return FIO_Q_QUEUED;
	/* [한국어] 비동기 큐잉 성공. 코어는 이 io_u 를 in_flight 상태로 전이시키고,
	 *         나중에 getevents/event 로 완료 수확할 때까지 보류. */
}

/*
 * [한국어]
 * fio_solarisaio_cleanup - 엔진 종료 시 자원 해제(ioengine_ops::cleanup).
 *
 * @td: 잡 thread_data.
 *
 * 동작: td->io_ops_data 가 가리키는 struct solarisaio_data 의 aio_events 배열과
 *       상태 구조체 자체를 free. init 과 대칭.
 * 실행 컨텍스트: 잡 스레드(잡 종료 경로). 이 시점에는 모든 io_u 가 이미 완료
 *                 되어 있거나 cancel 됐음이 보장됨(backend.c::do_io 가 cleanup 전
 *                 드레인 수행). NULL safe — init 실패 경로 대비.
 *
 * 호출 체인:
 *   backend.c::thread_main → ioengines.c::td_io_cleanup → [fio_solarisaio_cleanup]
 */
static void fio_solarisaio_cleanup(struct thread_data *td)
{
	struct solarisaio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태. init 실패 시 NULL 가능성 있음. */

	if (sd) {
		/* [한국어] 초기화 성공 경로. calloc 순서와 반대로 내부 배열 먼저 해제. */
		free(sd->aio_events);
		/* [한국어] init 에서 calloc(max_depth, sizeof(io_u*)) 로 할당한 완료 배열 해제. */
		free(sd);
		/* [한국어] 상태 구조체 자체 해제. td->io_ops_data = NULL 로 초기화하지 않지만
		 *         이후 재사용되지 않아 문제 없음. */
	}
}

/*
 * Set USE_SIGNAL_COMPLETIONS to use SIGIO as completion events.
 */
/* [한국어] USE_SIGNAL_COMPLETIONS 빌드 매크로 — 정의되면 커널이 AIO 완료를
 *         SIGIO 시그널로 알려주는 모드를 사용. 기본 빌드에서는 정의되지 않으며,
 *         필요 시 CFLAGS 에 -DUSE_SIGNAL_COMPLETIONS 추가. 이 모드의 장단점:
 *           장점: 잡 스레드가 aiowait 블록 없이도 완료 즉시 aio_events 에 축적.
 *           단점: 시그널 컨텍스트의 재진입 제약, async-signal-safe 함수만 사용 가능.
 *         기본 경로(매크로 미정의)에서는 getevents 에서 명시적 aiowait 만 사용. */
#ifdef USE_SIGNAL_COMPLETIONS
/*
 * [한국어]
 * fio_solarisaio_sigio - SIGIO 시그널 핸들러.
 *
 * @sig: 시그널 번호(SIGIO). 인자 자체는 사용하지 않음(단일 시그널 전용 핸들러).
 *
 * 왜 필요한가: 커널이 AIO 완료 시 SIGIO 를 프로세스에 보내도록 구성된 경우,
 *              잡 스레드가 명시적으로 aiowait 를 호출하지 않아도 이 핸들러가
 *              wait_for_event(NULL) 로 즉시 완료를 수확해 aio_events 에 축적.
 *              잡 스레드가 queue 와 getevents 사이에 "잠잠" 할 때 특히 유용.
 *
 * 실행 컨텍스트: **비동기 시그널 컨텍스트**. 잡 스레드 코드 임의 지점에서
 *                 선점적으로 진입할 수 있으므로, 이 핸들러가 건드리는 상태
 *                 (aio_events/aio_pending/nr) 는 잡 스레드 경로와 경쟁 관계.
 *                 wait_for_event 의 write_barrier 가 이 경쟁의 순서 보장을 제공.
 *
 * async-signal-safety: aiowait/errno/exit 가 Solaris 시그널 안전 함수 목록에 포함
 *                      되어야 하는데, Solaris 공식 async-signal-safe 목록에는 aiowait
 *                      가 보장되지 않아 엄밀히는 UB 여지가 있다 — 그래서 기본 빌드는
 *                      이 경로를 비활성화.
 *
 * 호출 체인:
 *   커널 AIO 완료 → 프로세스에 SIGIO 전달 → [fio_solarisaio_sigio] →
 *   wait_for_event(NULL) → aiowait(NULL)
 */
static void fio_solarisaio_sigio(int sig)
{
	wait_for_event(NULL);
	/* [한국어] 완료 1건 수확. tv=NULL 로 넘겼지만 이 경로에서는 이미 SIGIO 로
	 *         "완료 있음" 통지를 받은 상태라 aiowait 가 즉시 반환. 의미상 폴링
	 *         동치이며, 블록하지 않는다. 여러 건이 동시에 완료된 경우 추가 SIGIO
	 *         또는 다음 getevents 루프에서 나머지를 긁어간다. */
}

/*
 * [한국어]
 * fio_solarisaio_init_sigio - SIGIO 시그널 핸들러 등록.
 *
 * 왜 필요한가: USE_SIGNAL_COMPLETIONS 빌드에서 커널이 AIO 완료 시 SIGIO 를
 *              보내도록 하려면, 프로세스가 먼저 SIGIO 핸들러를 설정해두어야 함.
 *              핸들러 미설정 상태에서 SIGIO 가 도착하면 기본 동작(대체로 종료)
 *              가 적용되어 잡이 죽을 수 있음.
 *
 * 실행 컨텍스트: 잡 스레드 (fio_solarisaio_init 에서 1회 호출).
 *
 * 호출 체인:
 *   fio_solarisaio_init → [fio_solarisaio_init_sigio] → sigaction(2)
 */
static void fio_solarisaio_init_sigio(void)
{
	struct sigaction act;
	/* [한국어] POSIX sigaction(2) 에 전달할 액션 구조체.
	 *         필드: sa_handler(핸들러 함수포인터), sa_mask(핸들러 실행 중 블록할
	 *         시그널 집합), sa_flags(동작 플래그). */

	memset(&act, 0, sizeof(act));
	/* [한국어] 구조체 전체 0-초기화 — sa_mask 는 빈 시그널 집합, sa_flags 는 0
	 *         (아래에서 SA_RESTART 세팅). 예측 불가능한 잔재 값이 커널에
	 *         전달되는 것을 방지. */
	act.sa_handler = fio_solarisaio_sigio;
	/* [한국어] SIGIO 수신 시 호출될 핸들러 함수 지정. */
	act.sa_flags = SA_RESTART;
	/* [한국어] SA_RESTART — 이 시그널이 시스템 호출 중 발생해 EINTR 로 중단된
	 *         경우, 커널이 시스템 호출을 자동으로 재시작. 잡 스레드의 read/
	 *         write/nanosleep 등이 SIGIO 때문에 중단되는 것을 최소화. */
	sigaction(SIGIO, &act, NULL);
	/* [한국어] SIGIO 번호에 대해 act 액션 적용. 3번째 인자는 이전 액션 저장
	 *         버퍼로 NULL 이면 버림. 반환값 검사 없음(실패 시에도 기본 동작
	 *         유지라 치명적이지 않다고 판단). */
}
#endif

/*
 * [한국어]
 * fio_solarisaio_init - 엔진 초기화 콜백(ioengine_ops::init).
 *
 * @td: 잡 thread_data. td->o.iodepth 로 사용자 요청 큐 깊이 접근.
 * @return: 0 성공. 현 구현은 실패 경로 없음(calloc 실패 체크 생략 — 원 코드 보존).
 *
 * 동작 단계:
 *   1) struct solarisaio_data 0-초기화 할당.
 *   2) max_depth = min(td->o.iodepth, MAXASYNCHIO) 로 클램프. 초과 시 사용자에게 안내.
 *   3) aio_events 배열을 max_depth 크기로 할당(완료 포인터 저장소).
 *   4) USE_SIGNAL_COMPLETIONS 빌드 시 SIGIO 핸들러 설치.
 *   5) td->io_ops_data 에 엔진 상태 등록 — 이후 모든 콜백(prep/queue/getevents/
 *      event/cleanup) 이 이 포인터로 상태 접근.
 *
 * 왜 MAXASYNCHIO 클램프가 필요한가:
 *   Solaris 네이티브 AIO 는 프로세스당 동시 요청 수를 MAXASYNCHIO(보통 256) 로
 *   제한한다. 이를 초과해 aioread/aiowrite 를 호출하면 EAGAIN/ENOMEM 으로 실패해
 *   잡이 조기 종료될 수 있어, 엔진 레벨에서 선제적으로 사용자 iodepth 를
 *   클램프하고 로그로 알린다.
 *
 * 실행 컨텍스트: 잡 스레드(잡 시작 직후, queue 호출 전 1회).
 *
 * 호출 체인:
 *   backend.c::thread_main → ioengines.c::td_io_init → [fio_solarisaio_init]
 */
static int fio_solarisaio_init(struct thread_data *td)
{
	unsigned int max_depth;
	/* [한국어] 실제 사용할 최대 동시 AIO 깊이. 클램프 후 sd->max_depth 에 복사. */
	struct solarisaio_data *sd;
	/* [한국어] 엔진 상태 구조체 포인터. */
	sd = calloc(1, sizeof(*sd));
	/* [한국어] 상태 구조체 0-초기화 할당. aio_events=NULL, aio_pending=0, nr=0,
	 *         max_depth=0 상태에서 시작. 할당 실패 체크 없음 — 원 코드가 엔진 init
	 *         실패에 대해 관대하게 설계됨(대부분 OOM 시 프로세스 통째로 붕괴하는 전제). */

	max_depth = td->o.iodepth;
	/* [한국어] 사용자가 --iodepth=N 또는 잡 파일로 요청한 값. fio 코어가 파싱해
	 *         td->o.iodepth 에 저장. */
	if (max_depth > MAXASYNCHIO) {
		/* [한국어] 요청 값이 Solaris OS 상한을 넘음 → 클램프 필요. */
		max_depth = MAXASYNCHIO;
		/* [한국어] 상한으로 강제 제한. 보통 256. */
		log_info("fio: lower depth to %d due to OS constraints\n",
							max_depth);
		/* [한국어] 사용자에게 안내 로그. fio stdout 으로 출력되어 벤치마크 결과
		 *         해석 시 iodepth 가 요청과 다름을 알 수 있게 함. */
	}

	sd->aio_events = calloc(max_depth, sizeof(struct io_u *));
	/* [한국어] 완료 io_u 포인터를 저장할 배열 할당. 크기 = max_depth → 동시
	 *         in-flight 최대치만큼 가장 많이 쌓여도 overflow 없음. */
	sd->max_depth = max_depth;
	/* [한국어] queue 의 백프레셔 비교(sd->nr == sd->max_depth) 용 저장. */

#ifdef USE_SIGNAL_COMPLETIONS
	fio_solarisaio_init_sigio();
	/* [한국어] SIGIO 시그널 핸들러 설치. USE_SIGNAL_COMPLETIONS 빌드에서만 활성.
	 *         프로세스 전역 시그널 상태라 여러 잡 스레드가 각자 설치해도 동일 핸들러로
	 *         덮어쓰기되어 무해. */
#endif

	td->io_ops_data = sd;
	/* [한국어] fio 코어에 엔진 상태 등록. 이후 prep/queue/getevents/event/cleanup
	 *         모두 td->io_ops_data 로 sd 에 접근. */
	return 0;
	/* [한국어] 성공. 현 구현은 실패 경로 없음. */
}

/*
 * [한국어] ioengine — Solaris AIO 엔진의 ioengine_ops vtable.
 *
 * 이 구조체는 ioengines.c::load_ioengine("solarisaio") 가 찾아서 td->io_ops 에
 * 바인딩하며, 이후 fio 코어가 td->io_ops->queue(...) 식의 함수포인터 디스패치로
 * 엔진 콜백을 호출한다.
 *
 * 플래그: 본 엔진은 .flags 를 설정하지 않음(= 0). 의미:
 *   - FIO_SYNCIO 미설정: 비동기 엔진으로 선언 — queue 가 FIO_Q_QUEUED 반환 가능,
 *     코어는 getevents/event 로 완료 수확 경로 실행.
 *   - FIO_DISKLESSIO 미설정: 실제 파일/디바이스 대상 I/O.
 *   - FIO_ASYNCIO_SYNC_TRIM/SYNCFS 미설정: POSIX AIO 와 달리 Solaris 네이티브
 *     AIO 는 이 플래그를 사용하지 않음 — TRIM 을 지원하지 않아 플래그 자체가
 *     무의미하며, DDIR_SYNC/DATASYNC 는 queue 내부에서 드레인+동기 호출로 처리.
 *   - FIO_RAWIO/FIO_NOEXTEND/FIO_MEMALIGN/FIO_BARRIER/FIO_UNIDIR/FIO_NODISKUTIL
 *     모두 미설정: 특수 제약 없음 — 일반 파일/블록 디바이스 read/write 가 가능.
 *
 * 파일 수명(open_file/close_file/get_file_size) 은 모두 fio 공용 구현
 * (engines/generic 혹은 filesetup.c) 에 위임 — Solaris AIO 고유의 파일 상태가
 * 필요 없음(기존 POSIX fd 로 aioread/aiowrite 수행).
 */
static struct ioengine_ops ioengine = {
	.name		= "solarisaio",
	/* [한국어] 엔진 식별 문자열. --ioengine=solarisaio 또는 잡 파일의
	 *         ioengine=solarisaio 로 선택. ioengines.c::load_ioengine 이
	 *         engine_list 에서 이 이름으로 탐색.
	 *         설정자: 여기 리터럴. 읽는 자: find_ioengine. 동기화: read-only. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] 엔진 ABI 버전 상수. fio.h 에서 정의되며 코어/엔진 간 구조체
	 *         레이아웃 호환성 검증용. load_ioengine 이 불일치 시 엔진 거부.
	 *         설정자: 여기. 읽는 자: register_ioengine 내부 검증. 값 범위:
	 *         fio 릴리스별 증가. 동기화: read-only. */

	.init		= fio_solarisaio_init,
	/* [한국어] 엔진 초기화 콜백. td_io_init 이 호출. solarisaio_data/aio_events
	 *         할당 + MAXASYNCHIO 클램프 + (옵션) SIGIO 핸들러 설치.
	 *         설정자: 여기. 읽는 자: td_io_init. 호출 시점: 잡 시작 직후 1회.
	 *         동기화: 잡 스레드 단독. */

	.prep		= fio_solarisaio_prep,
	/* [한국어] I/O 제출 직전 io_u 초기화 콜백. io_u->resultp.aio_return =
	 *         AIO_INPROGRESS 리셋, engine_data 백포인터 세팅.
	 *         설정자: 여기. 읽는 자: td_io_prep. 호출 시점: get_io_u 직후, queue 직전.
	 *         동기화: 잡 스레드 단독. */

	.queue		= fio_solarisaio_queue,
	/* [한국어] I/O 제출 콜백. ddir 분기로 aioread/aiowrite/fsync/fdatasync 호출.
	 *         반환값: FIO_Q_QUEUED(비동기) / FIO_Q_COMPLETED(동기 또는 즉시 에러)
	 *                 / FIO_Q_BUSY(드레인/큐full).
	 *         설정자: 여기. 읽는 자: td_io_queue. 호출 시점: 매 io_u 제출.
	 *         동기화: 잡 스레드 단독. */

	.getevents	= fio_solarisaio_getevents,
	/* [한국어] 완료 배치 수확 콜백. aiowait 반복으로 aio_events 채움, pending 반환.
	 *         설정자: 여기. 읽는 자: td_io_getevents. 호출 시점: 완료 대기 루프마다.
	 *         동기화: 잡 스레드 소비 + (SIGIO 빌드시) 핸들러 생산 — write_barrier 사용. */

	.event		= fio_solarisaio_event,
	/* [한국어] 수확된 io_u 개별 조회 콜백. aio_events[event] 반환.
	 *         설정자: 여기. 읽는 자: td_io_event. 호출 시점: getevents 직후 ret 회 연속.
	 *         동기화: 잡 스레드 단독. */

	.cleanup	= fio_solarisaio_cleanup,
	/* [한국어] 종료 자원 해제 콜백. aio_events + sd free.
	 *         설정자: 여기. 읽는 자: td_io_cleanup. 호출 시점: 잡 종료 1회.
	 *         동기화: 잡 스레드 단독(모든 io_u drained 후). */

	.open_file	= generic_open_file,
	/* [한국어] 파일 열기 콜백을 fio 공용 구현에 위임. open(2) 로 O_RDONLY/O_RDWR/
	 *         O_DIRECT 등 잡 옵션에 맞춘 플래그로 열어 fio_file::fd 에 저장.
	 *         Solaris AIO 는 일반 POSIX fd 로 동작하므로 특별 처리 불필요.
	 *         설정자: 여기. 읽는 자: td_io_open_file. 동기화: 잡 스레드 단독. */

	.close_file	= generic_close_file,
	/* [한국어] 파일 닫기 공용 위임. close(2) 호출.
	 *         설정자: 여기. 읽는 자: td_io_close_file. 동기화: 잡 스레드 단독. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 파일 크기 질의 공용 위임. stat(2) 또는 디바이스 ioctl 로 크기 취득.
	 *         설정자: 여기. 읽는 자: td_io_get_file_size. 동기화: 잡 스레드 단독. */
};

/*
 * [한국어]
 * fio_solarisaio_register - 모듈 로드 시 엔진 자동 등록(constructor).
 *
 * fio_init 매크로(GCC __attribute__((constructor))) 로 main() 실행 전 libc 동적
 * 로더가 자동 호출. register_ioengine 이 전역 engine_list(ioengines.c) 에 본
 * ioengine 구조체를 추가 — 이후 load_ioengine("solarisaio") 가 이 등록을 찾음.
 *
 * 실행 컨텍스트: 프로세스 초기화(main 이전, 단일 스레드).
 */
static void fio_init fio_solarisaio_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] ioengines.c 의 전역 engine_list(flist) 꼬리에 본 ioengine 추가.
	 *         이후 fio 전체에서 --ioengine=solarisaio 조회 가능. */
}

/*
 * [한국어]
 * fio_solarisaio_unregister - 프로세스 종료 시 엔진 해제(destructor).
 *
 * fio_exit 매크로(GCC __attribute__((destructor))) 로 프로세스 종료 직전 자동 호출.
 * unregister_ioengine 이 engine_list 에서 본 구조체를 제거.
 *
 * 실행 컨텍스트: 프로세스 종료(main 이후, 단일 스레드).
 */
static void fio_exit fio_solarisaio_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] engine_list 에서 본 ioengine 제거. flist_del_init. */
}
