/*
 * [한국어 설명] splice(2) 기반 제로카피 I/O 엔진 구현 (splice.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux 의 splice(2)/vmsplice(2) 시스템 호출을 이용해 "파일 ↔ 사용자 버퍼" 사이
 * 데이터 전송을 **커널 파이프 버퍼**를 매개로 수행하는 fio I/O 엔진이다. 일반 read(2)/write(2)
 * 경로가 "디스크 → 커널 페이지캐시 → 사용자 버퍼"로 최소 1회 CPU copy를 동반하는 것과 달리,
 * splice 는 파일 페이지 캐시의 **struct page 참조**만 파이프 버퍼(pipe_buffer)에 꽂아 넣고,
 * vmsplice 는 사용자 공간 페이지를 "GUP(get_user_pages)"하여 같은 파이프에 꽂거나 반대로
 * 파이프로부터 사용자 주소 영역에 **페이지 프레임을 재맵핑**한다. 즉 "실제 복사"는 페이지
 * 포인터 이동으로 대체되므로 제로카피(zero-copy)라 부른다. 이 엔진의 존재 이유는 (1) splice
 * 경로 자체의 대역폭/오버헤드 측정, (2) vmsplice-to-user 의 커널 지원 여부 런타임 탐지,
 * (3) 페이지 소유권(GIFT) 플래그의 영향 벤치마크에 있다.
 *
 * splice 제로카피 원리 요약:
 *   - splice(fd_in, off_in, fd_out, off_out, len, flags): fd_in 의 페이지 캐시 페이지를
 *     파이프 buffer slot 에 참조(pointer+refcount)로 꽂거나, 반대로 파이프의 페이지 슬롯을
 *     fd_out 의 페이지 캐시/블록 계층에 "이관"시킨다. 실제 바이트 복사는 발생하지 않으며
 *     DMA/페이지 테이블 조작만 수행.
 *   - vmsplice(pipefd, iov, nr, flags): 사용자 주소 영역(iovec)의 페이지를 파이프에 꽂거나
 *     (SPLICE_F_GIFT 세트 시 커널이 소유권을 가져감 — 이후 사용자 해당 페이지 접근 UB),
 *     파이프의 페이지를 사용자 iovec 위에 remap 한다. vmsplice-to-user 는 4KB 페이지 경계
 *     정렬이 강제되며, 미정렬 시 커널이 EFAULT 또는 EINVAL 을 반환한다.
 *   - tee(in, out, len, flags): 두 파이프 사이에서 "바이트 복제"를 참조 증가로 처리(이 파일
 *     에서는 사용하지 않지만 동일 제로카피 계열 API).
 *
 * SPLICE_F_* 플래그:
 *   - SPLICE_F_MOVE     : "페이지 이동"을 시도(pipe→file 방향에서 페이지 캐시 hijack 힌트).
 *                         실제로는 커널 버전에 따라 무시되기도 한다.
 *   - SPLICE_F_MORE     : 곧 이어질 데이터가 더 있음 — TCP 의 MSG_MORE 와 유사한 힌트.
 *   - SPLICE_F_NONBLOCK : 파이프/fd 가 블록될 상황이면 EAGAIN 즉시 반환(이 파일의 write
 *                         경로가 vmsplice 호출 시 사용 — poll 로 먼저 대기했으니 즉발로 채움).
 *   - SPLICE_F_GIFT     : vmsplice(user→pipe) 전용. 사용자가 해당 페이지 소유권을 커널에
 *                         "증여" → 이후 해당 페이지를 유저가 다시 만지면 UB. 본 엔진은 WRITE
 *                         경로에서 동일 버퍼를 다시 사용하므로 GIFT 는 쓰지 않는다.
 *
 * 파이프 버퍼 크기 한계:
 *   - 리눅스 기본 PIPE_BUF_SIZE=64KiB (16 slots × 4KiB). fio 는 SPLICE_DEF_SIZE (fio.h 에
 *     정의, 보통 64KiB) 만큼씩 끊어서 splice 호출하므로 단일 splice 로 넘길 수 있는 상한과
 *     일치. fcntl(F_SETPIPE_SZ) 로 확대 가능하나 이 엔진은 기본값만 사용한다.
 *   - 따라서 I/O 블록 크기(bs) 가 64KiB 를 초과하면 내부 루프가 다회 splice 호출을 쪼개고,
 *     64KiB 이하면 단발 호출로 처리한다.
 *
 * O_DIRECT 비호환:
 *   - splice 는 기본적으로 페이지 캐시를 통해서만 동작(DIRECT I/O 는 페이지 캐시 우회)
 *     하므로 direct=1 잡과는 의미가 충돌한다. fio 코어가 엔진 선택 시 막지는 않지만,
 *     실 파일시스템 동작은 fallback 또는 EINVAL 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: main → fio_backend → load_ioengine("splice") → ioengines.c 가 이 파일의
 * fio_spliceio_register(constructor) 로 등록해 둔 ioengine_ops 를 잡 스레드에 연결한다.
 * 잡 스레드 루프: td_io_queue → fio_spliceio_queue → (READ/WRITE 분기) → splice/vmsplice
 * 시스템 호출 → 커널 파이프 버퍼(struct pipe_inode_info) → 타깃 파일/사용자 메모리.
 * 실행 컨텍스트는 전적으로 **잡 스레드 유저스페이스**이며, I/O 가 동기적으로 끝나므로
 * getevents/event 콜백은 두지 않고 queue 에서 FIO_Q_COMPLETED 로 즉시 반환한다.
 * init 단계에서 pipe(2) 로 파이프 fd 쌍을 생성해 td->io_ops_data 에 저장하고, cleanup
 * 에서 두 fd 를 close 한다. 파일 fd 수명은 generic_open_file/close_file 이 담당한다.
 *
 * === 타 모듈과의 연결 ===
 * 상위(caller): fio 코어(ioengines.c 의 td_io_queue, io_u.c 의 io_u 채움, backend.c 의
 * 잡 루프)가 io_u 를 채워 넣고 td_io_queue 로 제출한다. td_io_init/td_io_cleanup 이
 * fio_spliceio_init/cleanup 의 트램폴린이다.
 * 하위(callee): Linux 커널의 splice(2)/vmsplice(2)/pipe(2)/read(2)/poll(2)/mmap(2)/
 * munmap(2) 시스템 콜. do_io_u_trim/do_io_u_sync 는 io_u.c 공용 헬퍼로, TRIM/FLUSH 등
 * splice 로 표현 불가한 요청을 처리한다.
 * 공유 상태: struct spliceio_data(파이프 fd 쌍과 vmsplice 지원 플래그)를 td->io_ops_data
 * 에 저장한다. 단일 잡 스레드가 소유하므로 락 불필요. generic_open_file/close_file/
 * get_file_size 는 filesetup.c 의 공용 구현을 재사용해 파일 fd 수명은 fio 코어가 관리한다.
 * 데이터 흐름(READ): 파일 fd → splice(SPLICE_F_MORE) → pipe[1] → pipe[0]
 *                    → vmsplice(SPLICE_F_MOVE, to user) → 사용자 버퍼 (혹은 구형: read(2)).
 * 데이터 흐름(WRITE): 사용자 버퍼 → vmsplice(SPLICE_F_NONBLOCK) → pipe[1] → pipe[0]
 *                    → splice → 파일 fd.
 *
 * FIO_SYNCIO/FIO_PIPEIO 플래그 근거:
 *   - FIO_SYNCIO: queue() 가 시스템 콜을 직접 기다리고 끝나므로 coreloop 는 submit/
 *     complete 를 합친 동기 경로로 취급(commit/getevents/event 호출 생략).
 *   - FIO_PIPEIO: 엔진이 파이프 기반이라 "시크 가능한 파일 포지션"을 갖지 않는다는 표식.
 *     stat/lseek 기반 최적화(예: 오프셋 기반 pre-population)는 이 엔진에 적용 불가하다고
 *     코어에 알리는 힌트.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_spliceio_init: pipe(2) 로 파이프 쌍 생성, 낙관적 vmsplice 지원 가정 플래그 ON.
 * - fio_splice_read: 신형 — file→pipe(splice) + pipe→user(vmsplice). EFAULT 시 mmap 폴백 해제,
 *   EBADF 시 구형 경로로 폴백 요청.
 * - fio_splice_read_old: 구형 — file→pipe(splice) + pipe→user(read). vmsplice-to-user
 *   를 지원하지 않는 커널용 (2.6 초기 커널 호환 경로).
 * - fio_splice_write: user→pipe(vmsplice) + pipe→file(splice). 파이프가 가득 차면 poll.
 * - fio_spliceio_queue: ddir 분기 및 폴백 제어, 부분 전송(resid) 처리.
 * - fio_spliceio_cleanup: 파이프 fd close + 상태 구조체 free.
 * - fio_spliceio_register/unregister: constructor/destructor 로 엔진 등록/해제.
 * - struct spliceio_data: 파이프 fd 쌍(pipe[0], pipe[1]) + 두 종의 폴백 플래그
 *   (vmsplice_to_user, vmsplice_to_user_map).
 */

/*
 * splice engine
 *
 * IO engine that transfers data by doing splices to/from pipes and
 * the files.
 *
 */
#include <stdio.h>
/* [한국어] 표준 입출력 — 이 파일에서 직접 사용하지는 않으나 fio.h 가 간접적으로 요구하며,
 * td_verror/log_err 가 내부적으로 printf 계열 포매팅을 사용. */
#include <stdlib.h>
/* [한국어] malloc/free 공급 — fio_spliceio_init 의 struct spliceio_data 할당,
 * fio_spliceio_cleanup 의 해제에 사용. */
#include <unistd.h>
/* [한국어] POSIX 시스템 콜 래퍼:
 *   - pipe(int fd[2])        : 이 엔진의 중심 매개체인 익명 파이프 생성.
 *   - read(fd, buf, len)     : 구형 읽기 경로(fio_splice_read_old)가 파이프 끝에서 바이트 drain.
 *   - close(fd)              : cleanup 에서 파이프 fd 쌍 해제.
 * 또한 glibc 가 splice/vmsplice/tee 프로토타입을 fcntl.h 경유로 노출할 때 _GNU_SOURCE 가
 * 필요한데, 이는 fio 빌드 시스템의 컴파일러 플래그에서 일괄 정의된다. */
#include <errno.h>
/* [한국어] errno 전역 — splice/vmsplice 에러 분기에 필수.
 *   ENODATA, EAGAIN : 재시도 가능한 일시적 실패(비파괴 재시도).
 *   EFAULT          : vmsplice-to-user 가 mmap 폴백 경로에서 페이지 매핑 불일치를 보고.
 *   EBADF           : 커널이 vmsplice-to-user 자체를 미지원 → 구형 경로로 폴백.
 *   EINVAL          : 해당 파일시스템이 splice 미지원(일부 NFS/특수 FS). */
#include <poll.h>
/* [한국어] poll(2) — fio_splice_write 가 파이프 write 끝이 쓰기 가능(POLLOUT) 해질 때까지
 * 블록 대기하는 용도. 파이프가 가득 찬 상태에서 vmsplice 가 EAGAIN 이 되는 것을 방지하기 위해
 * 명시적으로 커널에게 "공간 생길 때까지 재워달라" 요청한다. */
#include <sys/mman.h>
/* [한국어] mmap/munmap 공급 — vmsplice-to-user 의 "페이지 이사(remap)" 경로에서
 * 사용자 버퍼 주소 위에 MAP_PRIVATE|MAP_ANON 익명 매핑을 덮어 씌운다. 이렇게 하면
 * vmsplice 가 해당 가상 주소의 페이지 프레임 소유자를 파이프에서 꺼낸 페이지로 교체
 * 하더라도 원본 fio 버퍼의 페이지 회계가 오염되지 않는다. PROT_READ 로 매핑해 vmsplice
 * 반환 후 fio 코어가 버퍼를 읽기만 하도록 보장. */

#include "../fio.h"
/* [한국어] fio 코어 헤더 — 다음 심볼을 공급한다:
 *   - 타입: thread_data, io_u, fio_file, ioengine_ops, enum fio_q_status, struct iovec 재노출.
 *   - 매크로: FIO_Q_COMPLETED/QUEUED/BUSY, FIO_SYNCIO, FIO_PIPEIO, FIO_IOOPS_VERSION,
 *            fio_init/fio_exit(= __attribute__((constructor/destructor))),
 *            SPLICE_DEF_SIZE(파이프 한 번 전송 한도, 통상 64KiB = PIPE_DEF_BUFFERS * PAGE_SIZE),
 *            OS_MAP_ANON(플랫폼별 MAP_ANONYMOUS / MAP_ANON 추상화).
 *   - 유틸 함수: td_verror(에러 기록), log_err(stderr 출력), register_ioengine/
 *               unregister_ioengine, generic_open_file/close_file/get_file_size,
 *               do_io_u_trim, do_io_u_sync, fio_ro_check.
 *   - DDIR_* enum: DDIR_READ/WRITE/TRIM 및 SYNC/DATASYNC 계열. */

/*
 * [한국어] struct spliceio_data — splice 엔진이 잡 당 보유하는 상태.
 *
 * 수명: fio_spliceio_init 에서 malloc 으로 생성 → td->io_ops_data 에 연결 →
 *       fio_spliceio_cleanup 에서 close(pipe)+free. 잡 수명(thread_data) 과 동일.
 * 공유: 단일 잡 스레드가 소유(read/write 모두 같은 스레드). 다중 잡이어도 잡별로
 *       별개의 struct 가 생성되므로 잡 간 공유 없음 → 락 불필요.
 * 역할: (1) 제로카피 중간 매개체인 익명 파이프의 fd 쌍 보관,
 *       (2) 런타임에 탐지한 커널 vmsplice-to-user 지원 능력 플래그 2종 유지(점진적 폴백).
 */
struct spliceio_data {
	int pipe[2];
	/* [한국어] splice 의 중간 매개체가 되는 익명 파이프의 fd 쌍 (pipe(2) 로 생성).
	 * pipe[0] = 읽기 끝(reader, consumer), pipe[1] = 쓰기 끝(writer, producer).
	 * 설정자: fio_spliceio_init 의 pipe(2) 호출 — 실패 시 errno=EMFILE/ENFILE 가능.
	 * 읽는 자:
	 *   READ 경로  : 파일 → pipe[1] 로 splice-in, pipe[0] → 유저 버퍼 로 vmsplice-out
	 *               (또는 구형 경로에서 pipe[0] → read(2) → 유저 버퍼).
	 *   WRITE 경로 : 유저 버퍼 → pipe[1] 로 vmsplice-in, pipe[0] → 파일 로 splice-out.
	 * 값 범위: 성공 시 0 이상의 유효 fd 2개. 이 엔진 생애 동안 불변(해제 시에만 close).
	 * 해제: fio_spliceio_cleanup 에서 두 fd 모두 close.
	 * 동기화: 단일 잡 스레드 전용 → 락 불필요. 파이프 자체의 내부 버퍼는 커널 측에서
	 *         pipe inode 락으로 보호되므로 엔진에서 추가 동기화 불필요.
	 * 참고: 파이프 버퍼 총량 기본 64KiB (PIPE_DEF_BUFFERS(16) * PAGE_SIZE(4KiB)).
	 *       fcntl(F_SETPIPE_SZ) 로 확장 가능하나 이 엔진은 기본값 사용. */

	int vmsplice_to_user;
	/* [한국어] 커널이 "파이프 → 사용자 메모리" 방향의 vmsplice(2) 를 지원하는지의 런타임
	 * 탐지 플래그(부울). 초기값 1(낙관적 지원 가정).
	 * 설정자:
	 *   - fio_spliceio_init 에서 1 로 초기화.
	 *   - fio_spliceio_queue 에서 fio_splice_read 가 -EBADF 를 돌려주면 0 으로 내림.
	 * 읽는 자: fio_spliceio_queue 의 DDIR_READ 경로 진입 분기 — 1이면 신형(vmsplice) 경로,
	 *          0이면 구형(read) 경로로 디스패치.
	 * 값 범위: 0(커널 미지원 확인됨) | 1(지원 낙관/검증됨).
	 * 동기화: 잡 스레드 전용 → 락 불필요. 한 번 0 으로 내려가면 해당 잡 수명 내내 유지.
	 * 쓰기 경로에는 영향 없음 — 쓰기는 "유저 → 파이프" 방향 vmsplice 로 이 방향은 구형부터
	 * 커널이 지원한다. */

	int vmsplice_to_user_map;
	/* [한국어] vmsplice-to-user 의 **mmap 기반 "페이지 이사" 경로** 지원 여부 플래그.
	 * 초기값 1. vmsplice 가 파이프 페이지를 사용자 주소 영역에 remap 하려면 대상 주소에
	 * 이미 fio 가 할당한 heap 페이지가 점유돼 있으면 충돌할 수 있어서, PROT_READ|
	 * MAP_PRIVATE|MAP_ANON 의 익명 매핑을 원본 버퍼 주소에 덮어 씌운 뒤 vmsplice 가
	 * 그 매핑 안으로 파이프 페이지를 옮기도록 한다.
	 * 설정자:
	 *   - fio_spliceio_init 에서 1 로 초기화.
	 *   - fio_splice_read 내부에서 vmsplice 가 EFAULT 를 내면 0 으로 떨어뜨리고
	 *     현재 요청을 비-mmap 경로(io_u->xfer_buf 직결)로 재시도.
	 * 읽는 자: fio_splice_read 의 진입부 — 1이면 mmap 덮어쓰기 + PROT_READ, 0이면 원본
	 *          버퍼 그대로 기록.
	 * 값 범위: 0(EFAULT 관측 — 커널 remap 경로 미지원) | 1(시도 가능).
	 * 동기화: 잡 스레드 전용. 한 번 0 으로 내리면 이 잡 수명 내내 유지 — 다음 read 호출부터
	 *         처음부터 비-mmap 경로. */
};

/*
 * vmsplice didn't use to support splicing to user space, this is the old
 * variant of getting that job done. Doesn't make a lot of sense, but it
 * uses splices to move data from the source into a pipe.
 */
/*
 * [한국어]
 * fio_splice_read_old - vmsplice-to-user 미지원 커널용 읽기 구현 (부분 제로카피).
 *
 * @td: 잡 스레드 thread_data. 엔진 상태(td->io_ops_data = struct spliceio_data *)
 *      와 에러 기록 경로(td_verror) 접근용.
 * @io_u: 읽기 대상 I/O 유닛 — io_u->file(대상 fio_file), io_u->offset(시작 오프셋),
 *        io_u->xfer_buf(사용자 버퍼 시작 주소), io_u->xfer_buflen(바이트 수)을 포함.
 * @return: 성공 시 전송한 총 바이트(= io_u->xfer_buflen), 실패 시 -errno.
 *          반환값이 요청 길이와 다르면 fio_spliceio_queue 가 resid 로 환산.
 *
 * 왜 필요한가: 리눅스 2.6 극초창기 커널에서는 splice(2) 는 있어도 vmsplice(2) 가
 *             pipe→user 방향을 아직 지원하지 않았다. 이 경로는 그런 환경용 fallback.
 *             현대 커널에서도 신형 경로가 EBADF 로 실패하면 이 구현으로 내려온다.
 *
 * 동작 단계:
 *   (1) "파일 → splice → 파이프" : 파일 페이지 캐시 페이지의 참조를 파이프 슬롯에 꽂음
 *       — 여기까진 제로카피. SPLICE_F_MORE 로 "곧 또 채울 것" 힌트.
 *   (2) "파이프 → read(2) → 사용자 버퍼" : 파이프 페이지에서 사용자 버퍼로 실제 CPU 카피
 *       1회 발생. 즉 절반의 제로카피(파일→파이프까지만).
 *   (3) 파이프 한 번 최대 용량 SPLICE_DEF_SIZE(= 64KiB) 로 쪼개 반복 until buflen==0.
 *
 * 에러 경로:
 *   - ENODATA/EAGAIN : 비파괴 재시도(파이프 일시 고갈 등, 일반적으로 실 디스크 I/O
 *                      에서는 거의 발생하지 않음).
 *   - 그 외 splice/read 실패 : -errno 로 즉시 리턴 → fio_spliceio_queue 에서
 *                              io_u->error = errno 로 채워 상위에 보고.
 *
 * 실행 컨텍스트: 잡 스레드 유저스페이스, 동기 블록킹. queue 에서 즉시 호출되며 반환 시
 *                I/O 완료.
 * 호출 체인: backend.c 잡 루프 → td_io_queue → fio_spliceio_queue (DDIR_READ,
 *            sd->vmsplice_to_user == 0) → [fio_splice_read_old]
 *            → splice(2) + read(2).
 */
static int fio_splice_read_old(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 잡별 상태 획득 — 파이프 fd 쌍 sd->pipe[0,1] 사용하기 위해. */
	struct fio_file *f = io_u->file;
	/* [한국어] 읽을 대상 파일 메타데이터(f->fd 가 실제 커널 fd). generic_open_file 이 이미
	 * 열어둔 상태. */
	int ret, ret2, buflen;
	/* [한국어] ret  = splice 반환(파이프에 들어간 바이트),
	 *          ret2 = read 반환(파이프에서 뽑아낸 바이트),
	 *          buflen = 아직 처리할 남은 바이트. */
	off_t offset;
	/* [한국어] 파일 내 현재 읽을 오프셋. splice(2) 의 두 번째 인자에 포인터로 넘기면
	 * 커널이 "읽은 만큼 자동으로 증가"시킨다 (즉 lseek 없이 스트리밍). */
	char *p;
	/* [한국어] 사용자 버퍼의 다음 write 위치 포인터(이번 iter 에서 read(2) 타깃). */

	offset = io_u->offset;
	/* [한국어] 요청 시작 오프셋 로드(파일 내 절대 위치). */
	buflen = io_u->xfer_buflen;
	/* [한국어] 총 요청 길이 — 이 값이 0 될 때까지 루프 반복. */
	p = io_u->xfer_buf;
	/* [한국어] 사용자 버퍼 시작 포인터 초기화. */
	while (buflen) {
		/* [한국어] 남은 바이트가 0 될 때까지 반복. 단일 splice 호출로는 파이프 버퍼
		 * 크기(64KiB) 까지만 처리 가능하므로 청크 분할 필요. */
		int this_len = buflen;
		/* [한국어] 이번 iteration 에서 처리할 길이(청크 크기). */

		if (this_len > SPLICE_DEF_SIZE)
			/* [한국어] 파이프 버퍼 최대 용량(PIPE_DEF_BUFFERS*PAGE_SIZE=64KiB)
			 * 초과 시 상한으로 자름. 이 상한을 무시하면 splice 가 EAGAIN 또는
			 * 부분 전송을 보고하므로 사전에 cap. */
			this_len = SPLICE_DEF_SIZE;

		ret = splice(f->fd, &offset, sd->pipe[1], NULL, this_len, SPLICE_F_MORE);
		/* [한국어] **1단계: 파일 → 파이프 제로카피**
		 *   splice(fd_in=f->fd, off_in=&offset, fd_out=pipe[1], off_out=NULL,
		 *          len=this_len, flags=SPLICE_F_MORE).
		 *   - offset 에 포인터 전달 → 커널이 읽은 바이트만큼 자동 전진(스트리밍).
		 *   - pipe[1] 은 파이프 writer 끝이므로 off_out 은 의미 없음(NULL).
		 *   - SPLICE_F_MORE : TCP_CORK/MSG_MORE 와 유사한 "곧 더 있음" 힌트 —
		 *                     파이프 측 작은 packet 합침 최적화 유도(성능 힌트).
		 *   - 커널 동작: 파일의 페이지 캐시 페이지 구조체(struct page)를 파이프
		 *                슬롯에 참조로 꽂아 넣음 → 실제 데이터 복사 없음. */
		if (ret < 0) {
			/* [한국어] splice 실패 분기. */
			if (errno == ENODATA || errno == EAGAIN)
				/* [한국어] 일시적 자원 부족(파이프 비었거나 바쁨) — 비파괴 재시도.
				 * splice 는 idempotent 하므로 동일 인자로 다시 불러도 안전. */
				continue;

			return -errno;
			/* [한국어] 그 외 에러(EBADF, EINVAL, EIO 등) 는 복구 불가능 → -errno 로
			 * 즉시 리턴하여 queue 레이어에서 io_u->error 로 전달. */
		}

		buflen -= ret;
		/* [한국어] 파이프에 실제로 들어간 바이트만큼 "남은 작업량" 감소. ret 은 음수가 아니며
		 * 0~this_len 사이. 0 이면 아래 내부 while 루프 건너뜀. */

		while (ret) {
			/* [한국어] **2단계: 파이프 → 사용자 버퍼 draining**.
			 * splice 가 파이프에 쌓아둔 바이트를 read(2) 로 뽑아낸다. 여기서 커널이
			 * 페이지 내용을 사용자 주소로 memcpy 하므로 실제 카피 1회 발생. */
			ret2 = read(sd->pipe[0], p, ret);
			/* [한국어] read(fd=pipe[0], buf=p, count=ret). 파이프 읽기 끝에서 바이트 소비.
			 * 파이프가 비어있지 않으므로 블록되지 않음(이미 splice 로 채웠음). */
			if (ret2 < 0)
				return -errno;
				/* [한국어] read 실패(예: EINTR, EIO) → 즉시 실패 리턴. */

			ret -= ret2;
			/* [한국어] 파이프에 아직 남은 바이트. read(2) 가 한 번에 전량을 주지 않을 수
			 * 있으므로 잔량 루프. */
			p += ret2;
			/* [한국어] 사용자 버퍼의 다음 write 위치 전진. */
		}
	}

	return io_u->xfer_buflen;
	/* [한국어] 여기 도달하면 전체 요청 완료 — fio 코어가 io_u->xfer_buflen 과 동일한
	 * 값을 받으면 "부분 전송 아님" 으로 판단한다. */
}

/*
 * We can now vmsplice into userspace, so do the transfer by splicing into
 * a pipe and vmsplicing that into userspace.
 */
/*
 * [한국어]
 * fio_splice_read - vmsplice-to-user 지원 커널용 신형 읽기 구현 (완전 제로카피).
 *
 * @td: 잡 스레드 thread_data — 엔진 상태와 에러 기록 경로 공급.
 * @io_u: 읽기 대상 I/O 유닛(io_u->file, offset, xfer_buf, xfer_buflen).
 * @return: 성공 시 총 전송 바이트(= io_u->xfer_buflen),
 *          실패 시 음수:
 *            -EBADF → fio_spliceio_queue 에 "커널이 vmsplice-to-user 미지원,
 *                     엔진 전역에서 구형 경로로 폴백" 시그널.
 *            -1     → td_verror 로 상세 에러 기록된 일반 실패.
 *            -errno → 기타 시스템 콜 실패.
 *
 * 왜 필요한가: 구형 경로(fio_splice_read_old)는 파이프 → read(2) 단계에서 한 번의 CPU
 *             카피가 발생하지만, vmsplice(pipe, user_iov, SPLICE_F_MOVE) 는 파이프 슬롯의
 *             페이지 프레임을 사용자 주소 영역에 **페이지 테이블 치환(remap)**으로 직접
 *             설치하므로 카피 0회로 끝난다. 이게 splice 엔진의 본질적 목적이다.
 *
 * 핵심 아이디어 — mmap 덮어쓰기 트릭:
 *   사용자 버퍼(io_u->xfer_buf)는 fio 가 일반 malloc/posix_memalign 으로 할당한
 *   heap 메모리이며, 이미 페이지 프레임이 연결되어 있다. vmsplice-to-user 가 그 영역에
 *   파이프 페이지를 "이사"하려면 기존 페이지를 떼어내고 새 페이지를 매핑해야 하는데,
 *   이는 VM 서브시스템 관점에서 매우 까다롭다. 그래서 이 엔진은 **요청 직전에 사용자 버퍼
 *   주소 위에 MAP_PRIVATE|MAP_ANON|PROT_READ 로 익명 매핑을 덮어 씌운다**. 이러면 커널은
 *   깨끗한 가상 영역을 얻고, vmsplice 는 그 안에 파이프 페이지를 remap 하기만 하면 된다.
 *   데이터 전송 종료 후 munmap 으로 원복. 대신 이 방식은 일부 커널에서 EFAULT 를 낼 수
 *   있고(주소 정렬/플래그 조합 문제 등), 그 경우 mmap 없이 원본 버퍼로 시도하는 비-mmap
 *   경로로 폴백한다.
 *
 * 페이지 정렬 요구:
 *   vmsplice 는 페이지 경계(4KiB) 로 정렬된 주소·길이를 요구한다. fio 의 io_u 버퍼는 대개
 *   충분히 정렬되어 있지만, 길이가 4KiB 배수가 아니면 뒷부분이 부분적으로만 매핑될 수
 *   있다. 본 엔진은 그런 경우 정상 동작 범위 밖으로 간주한다.
 *
 * SPLICE_F_MOVE:
 *   vmsplice(pipe→user) 에 넘기면 "페이지 소유권 이전을 시도해 달라"는 힌트. 커널 구현에
 *   따라 실제로 참조 카운트만 증가/감소하거나, truly 이전하거나 하므로 성능 영향은 커널
 *   버전별 편차가 크다. 의미상으로는 "카피가 절대 발생하지 않아도 됨" 선언.
 *
 * 에러 처리:
 *   - ENODATA / EAGAIN : 일시 재시도(splice 단계).
 *   - EFAULT (vmsplice) : mmap 경로 오염 추정 → vmsplice_to_user_map=0, munmap 후
 *                         비-mmap 경로로 **같은 iteration 내** 즉시 재시도.
 *   - EBADF  (vmsplice) : 커널이 vmsplice-to-user 를 아예 미지원 → -EBADF 로 리턴 →
 *                         queue 가 vmsplice_to_user=0 으로 내리고 fio_splice_read_old
 *                         로 재호출.
 *   - 0 반환(short splice): 정상 상황에서 발생 불가 — ENODATA 로 기록 후 실패.
 *
 * 호출 체인: fio_spliceio_queue (DDIR_READ, sd->vmsplice_to_user==1)
 *            → [fio_splice_read] → splice(2) + vmsplice(2) (+ 선택적 mmap/munmap).
 *
 * 실행 컨텍스트: 잡 스레드 유저스페이스, 동기 블록킹.
 */
static int fio_splice_read(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태 — 파이프 fd 쌍과 폴백 플래그 접근용. */
	struct fio_file *f = io_u->file;
	/* [한국어] 소스 파일. f->fd 가 splice 의 fd_in. */
	struct iovec iov;
	/* [한국어] vmsplice 가 기대하는 사용자 영역 설명자(주소+길이). 단일 iov 를 반복 갱신하며
	 * 사용 — pipe 한 청크 드레인 당 1개 iovec 로 충분. */
	int ret , buflen, mmap_len;
	/* [한국어] ret = 각 splice/vmsplice 단계 결과(+바이트/-errno/-EBADF),
	 *          buflen = 아직 처리 안 한 바이트,
	 *          mmap_len = 최종 munmap 호출에 쓸 길이(원래 요청 길이 기억). */
	off_t offset;
	/* [한국어] 파일 오프셋. splice(2) 가 in-out 포인터로 자동 갱신. */
	void *map;
	/* [한국어] 덮어쓴 익명 매핑의 시작 주소 — mmap 경로 사용 안 할 땐 NULL. 종료 시 munmap
	 * 인자로 사용. */
	char *p;
	/* [한국어] 현재 기록 포인터(iov.iov_base 와 보조로 함께 전진). mmap 경로라면 map 을
	 * 가리키고, 아니면 io_u->xfer_buf 를 가리킴. */

	ret = 0;
	/* [한국어] 루프 밖에서 에러 판정(ret<0 체크) 하기 위한 안전 초기값. */
	offset = io_u->offset;
	/* [한국어] 요청 시작 파일 오프셋. */
	mmap_len = buflen = io_u->xfer_buflen;
	/* [한국어] munmap 길이와 남은 처리량을 같은 초기값(=요청 전체 길이)으로 설정. */

	if (sd->vmsplice_to_user_map) {
		/* [한국어] mmap 기반 "페이지 이사" 경로가 아직 유효하면 진입. */
		map = mmap(io_u->xfer_buf, buflen, PROT_READ, MAP_PRIVATE|OS_MAP_ANON, 0, 0);
		/* [한국어] 사용자 버퍼 가상 주소 위에 익명 매핑 덮어쓰기.
		 *   - addr=io_u->xfer_buf : 이 위치에 맵 생성 (커널이 사용 가능하면 수락).
		 *   - prot=PROT_READ      : 읽기 전용 — vmsplice 가 페이지 설치 후 fio 코어가
		 *                           데이터 읽기만 함.
		 *   - flags=MAP_PRIVATE|OS_MAP_ANON : 익명(파일 연결 없음) + private(COW 가능).
		 *                           MAP_FIXED 를 사용하지 않는 이유는 커널이 힌트로만
		 *                           받아들여 충돌 시 다른 주소 반환할 수 있게 하기 위함.
		 *   - fd=0, off=0         : 익명 맵이므로 무시. */
		if (map == MAP_FAILED) {
			/* [한국어] 매핑 실패 — 주소 충돌/메모리 부족 등. */
			td_verror(td, errno, "mmap io_u");
			return -1;
		}

		p = map;
		/* [한국어] 기록 포인터를 방금 얻은 매핑 시작으로. */
	} else {
		map = NULL;
		/* [한국어] 매핑 경로 비활성화 — munmap 대상 없음을 표시. */
		p = io_u->xfer_buf;
		/* [한국어] 원래 사용자 버퍼 주소 직접 사용(비-mmap 경로). */
	}

	while (buflen) {
		/* [한국어] 요청 전체를 다 옮길 때까지 청크 반복. */
		int this_len = buflen;
		/* [한국어] 이번 라운드에서 옮길 바이트 수(잠정). */
		int flags = 0;
		/* [한국어] splice 플래그 초기값. 마지막 조각이면 0 유지, 중간이면 SPLICE_F_MORE. */

		if (this_len > SPLICE_DEF_SIZE) {
			/* [한국어] 파이프 버퍼 용량 초과 → 잘라내고 "더 있음" 힌트 함께. */
			this_len = SPLICE_DEF_SIZE;
			flags = SPLICE_F_MORE;
			/* [한국어] SPLICE_F_MORE : 파이프 consumer 측 packet coalescing 최적화
			 * 유도. TCP socket 으로 splicing 할 때 주로 효과가 있지만 파일→파이프
			 * 단계에서도 커널이 힌트로 받아들인다. */
		}

		ret = splice(f->fd, &offset, sd->pipe[1], NULL, this_len,flags);
		/* [한국어] **1단계: 파일 → 파이프 (페이지 참조 이동, 제로카피)**.
		 * offset 이 포인터이므로 커널이 읽은 만큼 자동 증가시킴. flags 는 마지막 청크에서
		 * 0 이 되어 커널에 "이게 끝" 알림. */
		if (ret < 0) {
			/* [한국어] splice 실패 분기. */
			if (errno == ENODATA || errno == EAGAIN)
				/* [한국어] 일시적이면 비파괴 재시도. */
				continue;

			td_verror(td, errno, "splice-from-fd");
			/* [한국어] 영구적 에러 기록(예: EINVAL - 파일시스템 splice 미지원). */
			break;
		}

		buflen -= ret;
		/* [한국어] 파이프에 넣어둔 만큼 남은 작업량 감소. */
		iov.iov_base = p;
		/* [한국어] 사용자 도착지 주소. mmap 경로면 매핑 시작 + 누적 오프셋,
		 * 비-mmap 경로면 원본 버퍼 + 누적 오프셋. */
		iov.iov_len = ret;
		/* [한국어] 이번에 파이프에 쌓인 바이트만큼만 꺼내야 함. */

		while (iov.iov_len) {
			/* [한국어] **2단계: 파이프 → 사용자 메모리 (페이지 remap, 진짜 제로카피)**.
			 * vmsplice 한 번에 다 못 뽑을 수 있으므로 잔량 루프. */
			ret = vmsplice(sd->pipe[0], &iov, 1, SPLICE_F_MOVE);
			/* [한국어] vmsplice(fd=pipe[0], iov=&iov, nr_segs=1, flags=SPLICE_F_MOVE).
			 *   - 파이프 읽기 끝에서 iovec 에 기술된 사용자 영역으로 페이지 설치.
			 *   - SPLICE_F_MOVE : "참조 복사 대신 실제 이동 선호" 힌트(커널 버전 의존).
			 *   - 제로카피 핵심: 페이지 프레임 자체가 VM 에 의해 사용자 주소에 매핑됨.
			 *     (실제로는 대부분 커널이 refcount 관리로 처리). */
			if (ret < 0) {
				/* [한국어] vmsplice 에러 분기. */
				if (errno == EFAULT &&
				    sd->vmsplice_to_user_map) {
					/* [한국어] mmap 기반 경로에서만 발생 가능한 EFAULT —
					 * 커널이 해당 가상 영역에 페이지를 remap 할 수 없다고
					 * 판단한 경우. 매핑 트릭을 포기하고 원본 버퍼로 폴백. */
					sd->vmsplice_to_user_map = 0;
					/* [한국어] 이후부터 mmap 경로 끄기(엔진 수명 내내). */
					munmap(map, mmap_len);
					/* [한국어] 씌웠던 맵 해제 — 원본 xfer_buf 의 원래 페이지
					 * 프레임이 다시 유효해짐. */
					map = NULL;
					/* [한국어] dangling 포인터 방지. */
					p = io_u->xfer_buf;
					/* [한국어] 기록 포인터를 원래 버퍼로 환원. 주의: 이번 청크가
					 * 처음부터 재시작되므로 누적 전진은 0 리셋. */
					iov.iov_base = p;
					/* [한국어] iovec 도 원래 버퍼 시작으로 리셋 후 같은 while
					 * iteration 에서 재시도. iov.iov_len 은 변경 없음(=ret 원본). */
					continue;
				}
				if (errno == EBADF) {
					/* [한국어] vmsplice-to-user 자체를 커널이 거부 — 이 방향 자체를
					 * 미지원하는 구형 커널. 엔진 전체 폴백 필요. */
					ret = -EBADF;
					/* [한국어] queue 가 해석할 특수 신호. 이 값을 받으면 queue 에서
					 * sd->vmsplice_to_user=0 설정 후 fio_splice_read_old 재호출. */
					break;
				}
				td_verror(td, errno, "vmsplice");
				/* [한국어] 그 외 영구 에러 기록. */
				break;
			} else if (!ret) {
				/* [한국어] 0 바이트 반환 = 파이프에 데이터가 있는데도 못 꺼냄 — 비정상.
				 * (정상적인 EOF 는 발생하지 않음; splice 가 이미 bytes 를 넣어뒀기 때문) */
				td_verror(td, ENODATA, "vmsplice");
				ret = -1;
				break;
			}

			iov.iov_len -= ret;
			/* [한국어] 아직 파이프에 남아 꺼내야 할 바이트. */
			iov.iov_base += ret;
			/* [한국어] 도착지 iovec 포인터 전진. */
			p += ret;
			/* [한국어] 기록 포인터(바깥 loop 용) 전진 — 다음 청크의 iov.iov_base 를 여기서 읽음. */
		}
		if (ret < 0)
			/* [한국어] 내부 루프가 에러로 끝났으면 바깥 루프도 중단(buflen 대기 포기). */
			break;
	}

	if (sd->vmsplice_to_user_map && munmap(map, mmap_len) < 0) {
		/* [한국어] 정상 종료 시 덮어쓴 매핑을 해제. munmap 실패는 드물지만 보고.
		 * EFAULT 폴백 경로에서 이미 해제됐다면 vmsplice_to_user_map==0 이 되어 이 조건
		 * 건너뛴다. */
		td_verror(td, errno, "munnap io_u");
		/* [한국어] (원본 오탈자 'munnap' 유지 — 코드 수정 금지 원칙). */
		return -1;
	}
	if (ret < 0)
		/* [한국어] 에러로 끝났다면 그 값 그대로 상위로(-EBADF/-1/-errno). queue 가 해석. */
		return ret;

	return io_u->xfer_buflen;
	/* [한국어] 전체 성공 — 요청 전체 바이트 수 반환. */
}

/*
 * For splice writing, we can vmsplice our data buffer directly into a
 * pipe and then splice that to a file.
 */
/*
 * [한국어]
 * fio_splice_write - splice 엔진의 쓰기 경로 (완전 제로카피).
 *
 * @td: 잡 스레드 thread_data.
 * @io_u: 쓰기 대상 I/O 유닛(io_u->file, offset, xfer_buf, xfer_buflen).
 * @return: 성공 시 총 전송 바이트(= io_u->xfer_buflen),
 *          poll 실패 시 +errno (!주의: 양수 반환, 아래 설명),
 *          vmsplice/splice 실패 시 -errno.
 *
 * 왜 필요한가: 쓰기에서도 "유저 버퍼 → 커널 페이지캐시 복사" 단계를 제거하려면
 *             vmsplice(user→pipe) + splice(pipe→file) 조합이 유일하다. vmsplice 는
 *             사용자 페이지를 GUP 로 pin 하여 파이프 슬롯에 참조로 꽂는다(SPLICE_F_GIFT
 *             는 사용하지 않음 — 이 엔진은 동일 버퍼를 여러 번 재사용할 수 있으므로
 *             소유권 이전은 부적절).
 *
 * 동작:
 *   (1) 단일 iovec 로 사용자 버퍼 전체를 감싼다.
 *   (2) poll(POLLOUT, timeout=-1) 로 파이프 쓰기 끝이 쓰기 가능해질 때까지 블록 대기.
 *       파이프가 가득 찬 상태에서 vmsplice(NONBLOCK) 가 EAGAIN 내는 것을 원천 차단.
 *   (3) vmsplice(pipe[1], iov, 1, SPLICE_F_NONBLOCK) 로 사용자 페이지를 파이프에 꽂음.
 *       이 시점에서 데이터는 여전히 유저 페이지에 있고, 파이프는 참조(pinned pages)만 보유.
 *   (4) splice(pipe[0], NULL, file, &off, ret, 0) 로 파이프 페이지를 파일로 흘려보냄.
 *       flags=0(블록킹) 이므로 커널이 필요한 만큼 대기하며 파일 페이지 캐시에 쓴다.
 *   (5) vmsplice 가 부분 전송을 반환하면 iovec 전진 후 (2)~(4) 반복.
 *
 * SPLICE_F_NONBLOCK 의 의미:
 *   vmsplice 는 파이프가 가득 차면 기본적으로 블록된다. NONBLOCK 을 주면 공간 있는
 *   만큼만 채우고 즉시 반환. 이 엔진은 그 전에 poll 로 "빈 공간 있음"을 확인했으므로
 *   NONBLOCK 이어도 사실상 즉시 일부는 들어감. 0 을 리턴하는 경우는 드물다.
 *
 * GIFT 를 쓰지 않는 이유:
 *   SPLICE_F_GIFT 를 주면 커널이 해당 페이지 소유권을 "받아가며" vmsplice 직후에 사용자가
 *   해당 페이지에 쓰면 UB 가 된다. fio 는 동일 버퍼를 여러 io_u 에 재사용(또는 verify 경로
 *   에서 재읽음)하므로 GIFT 는 부적절.
 *
 * 반환값 주의사항:
 *   poll 실패 시 `return errno` (음수 아님) 는 원본 코드의 관례이며, queue 레이어에서
 *   `ret != xfer_buflen` 비교로 에러 감지 후 io_u->error=errno 경로로 처리된다. 코드 수정
 *   금지 원칙에 따라 유지.
 *
 * 호출 체인: fio_spliceio_queue (DDIR_WRITE) → [fio_splice_write]
 *            → poll(2) + vmsplice(2) + splice(2).
 *
 * 실행 컨텍스트: 잡 스레드 유저스페이스, 동기 블록킹. splice/vmsplice 동안 해당 스레드
 *                가 블록되지만 다른 잡 스레드는 영향 없음.
 */
static int fio_splice_write(struct thread_data *td, struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태 — 파이프 fd 쌍 접근용. */
	struct iovec iov = {
		.iov_base = io_u->xfer_buf,
		/* [한국어] vmsplice 전송 소스 = 사용자 버퍼 시작 주소. 페이지 정렬되어 있어야
		 * 커널 GUP(get_user_pages) 가 성공. */
		.iov_len = io_u->xfer_buflen,
		/* [한국어] 전체 전송 길이. 4KiB 배수가 아니면 끝부분 페이지는 부분만 전송
		 * 될 수 있으나 fio 의 io_u->xfer_buflen 은 대개 블록 크기 배수. */
	};
	struct pollfd pfd = { .fd = sd->pipe[1], .events = POLLOUT, };
	/* [한국어] 파이프 쓰기 끝(sd->pipe[1]) 에 대한 POLLOUT 대기 설정.
	 * poll 은 "파이프에 최소 1바이트 쓸 공간 있음" 을 보장 — 그 이상은 파이프 용량 한계. */
	struct fio_file *f = io_u->file;
	/* [한국어] 쓰기 대상 파일. */
	off_t off = io_u->offset;
	/* [한국어] 파일 내 기록 시작 오프셋 — splice(pipe→file) 가 자동 증가시킴. */
	int ret, ret2;
	/* [한국어] ret  = vmsplice 반환(파이프에 넣은 바이트),
	 *          ret2 = splice 반환(파일에 쓴 바이트). */

	while (iov.iov_len) {
		/* [한국어] 사용자 버퍼 전체를 다 보낼 때까지 반복. */
		if (poll(&pfd, 1, -1) < 0)
			/* [한국어] poll(fd_set 1개, timeout=-1 무한대) — 파이프 쓰기 가능 대기.
			 * 실패 시(EINTR 등) errno 를 **양수로 그대로** 반환(원본 관례). queue 에서
			 * 이 반환값이 xfer_buflen 과 다르므로 에러 처리 진입. */
			return errno;

		ret = vmsplice(sd->pipe[1], &iov, 1, SPLICE_F_NONBLOCK);
		/* [한국어] **1단계: 사용자 버퍼 → 파이프 (제로카피)**.
		 *   vmsplice(fd=pipe[1], iov=&iov, nr_segs=1, flags=SPLICE_F_NONBLOCK).
		 *   - 커널이 iov 의 사용자 페이지를 get_user_pages 로 pin 하고 파이프 슬롯에
		 *     참조로 꽂음. 카피 없음.
		 *   - SPLICE_F_NONBLOCK : 파이프 가득 찼다면 즉시 반환(poll 이 이미 대기했으나
		 *                         race 대비).
		 *   - GIFT 미사용 — 버퍼 재사용 가능성 유지. */
		if (ret < 0)
			return -errno;
			/* [한국어] vmsplice 실패 → -errno 로 리턴(write 경로는 음수 관례). */

		iov.iov_len -= ret;
		/* [한국어] 파이프에 들어간 만큼 남은 소스 길이 감소. */
		iov.iov_base += ret;
		/* [한국어] 소스 포인터 전진(다음 iteration 에서 이어 씀). */

		while (ret) {
			/* [한국어] **2단계: 파이프 → 파일 (블록킹 splice)**.
			 * vmsplice 로 파이프에 꽂힌 바이트를 즉시 파일로 드레인해야 다음 iter 에서
			 * 파이프에 공간이 생긴다. splice 한 번에 다 못 쓸 수 있으므로 잔량 루프. */
			ret2 = splice(sd->pipe[0], NULL, f->fd, &off, ret, 0);
			/* [한국어] splice(fd_in=pipe[0], off_in=NULL, fd_out=f->fd, off_out=&off,
			 *               len=ret, flags=0).
			 *   - off 포인터 전달 → 커널이 쓴 만큼 자동 전진(lseek 불필요).
			 *   - flags=0        : 블록킹 — 필요하면 파일시스템이 기다려도 됨.
			 *   - 커널 동작: 파이프 슬롯의 페이지를 파일 페이지 캐시에 참조로 꽂거나
			 *                (splice_write 구현에 따라) 페이지 캐시로 데이터 전달. */
			if (ret2 < 0)
				return -errno;
				/* [한국어] splice 실패 → -errno 반환. */

			ret -= ret2;
			/* [한국어] 파이프에 아직 남은 양. splice(blocking) 가 0 을 반환할 수는
			 * 거의 없지만, 파일 쿼터/용량 초과 등으로 short 가능. */
		}
	}

	return io_u->xfer_buflen;
	/* [한국어] 전체 쓰기 성공 — 요청 바이트 수 반환. */
}

/*
 * [한국어]
 * fio_spliceio_queue - splice 엔진의 I/O 제출 콜백 (ioengine_ops.queue 진입점).
 *
 * @td: 잡 스레드 thread_data.
 * @io_u: 제출할 I/O 유닛. ddir(READ/WRITE/TRIM/SYNC 계열), file, offset, xfer_buf,
 *        xfer_buflen 을 fio 코어(io_u.c 의 get_next_* 체인)가 미리 채워둔다.
 * @return: 항상 FIO_Q_COMPLETED — 이 엔진은 FIO_SYNCIO 라 모든 I/O 가 이 호출 안에서
 *          동기 완료된다. 에러가 있더라도 io_u->error 에 실어 보내고 형식적으로
 *          COMPLETED 를 반환(fio 코어가 error 필드로 성공/실패 판단).
 *
 * 왜 필요한가: FIO_SYNCIO 엔진의 표준 진입점. backend.c 의 잡 루프가 get_io_u →
 *              prep → td_io_queue → (이 함수) → (동기 완료) → put_io_u 순으로 호출한다.
 *
 * 디스패치 로직:
 *   DDIR_READ  → vmsplice_to_user 가 1이면 fio_splice_read (신형) 시도 →
 *                -EBADF 반환 시 vmsplice_to_user=0 으로 내리고 fio_splice_read_old 로 폴백.
 *                두 번째 호출은 같은 요청(io_u) 에 대해 이뤄짐 — 첫 시도가 파일 오프셋을
 *                전진시키지 않고 실패해야 재시도가 의미 있다. (-EBADF 는 첫 vmsplice 호출
 *                시점에 발생하며 그 전에 splice(파일→파이프) 는 이미 이루어졌을 수 있는데,
 *                이 경우 파이프에 남은 데이터는 다음 splice 호출에서 덮어쓰이므로 큰 문제
 *                아님 — 이는 원본 구현의 의도된 trade-off).
 *   DDIR_WRITE → fio_splice_write 직접 호출.
 *   DDIR_TRIM  → splice 로 표현 불가 → do_io_u_trim (io_u.c) 위임 — 커널에 따라
 *                BLKDISCARD ioctl 또는 fallocate(FALLOC_FL_PUNCH_HOLE) 로 처리.
 *   DDIR_SYNC/DATASYNC/SYNC_FILE_RANGE → do_io_u_sync 위임 — fsync/fdatasync/
 *                sync_file_range 시스템 콜.
 *
 * 반환값 → fio 계약 환산:
 *   (1) ret == io_u->xfer_buflen : 완전 성공 → io_u->error=0, resid 설정 안 함.
 *   (2) 0 <= ret < xfer_buflen   : 부분 전송(resid) → io_u->resid 설정, error=0 →
 *                                  fio 코어가 나머지 바이트를 재제출.
 *   (3) ret < 0                  : 에러 → io_u->error = errno (전역 errno 사용 —
 *                                  하위 함수가 -errno 리턴 직후라 아직 유효).
 *                                  EINVAL 이면 파일시스템 splice 미지원 힌트 메시지 추가.
 *
 * 에러 경로:
 *   - td_verror : thread_data 에 에러 기록 → 잡 종료 통계에 반영.
 *   - log_err   : 사용자용 stderr 안내 메시지(EINVAL 시 "looks like splice doesn't work
 *                 on this file system").
 *
 * 호출 체인: backend.c 잡 루프 → td_io_queue → [fio_spliceio_queue]
 *            → fio_splice_read|fio_splice_read_old|fio_splice_write|
 *              do_io_u_trim|do_io_u_sync.
 *
 * 실행 컨텍스트: 잡 스레드 유저스페이스, 동기 블록킹.
 */
static enum fio_q_status fio_spliceio_queue(struct thread_data *td,
					    struct io_u *io_u)
{
	struct spliceio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태 — vmsplice_to_user 플래그 검사/갱신에 필요. */
	int ret = 0;
	/* [한국어] 하위 함수 반환값 수집 변수. 초기 0(= "아직 아무것도 안 함") — 이 값으로
	 * xfer_buflen 과 비교되어 디스패치 미진입 상태 검출 가능(현재 코드 경로상 항상 분기
	 * 타므로 실제로 0 유지되는 경우 없음). */

	fio_ro_check(td, io_u);
	/* [한국어] 읽기전용 잡(td->o.read_only) 에 쓰기 요청이 오면 assert fail.
	 * io_u.c 의 get_io_u 가 대부분 방어해 주지만 엔진 단에서도 재확인(계약 방어). */

	if (io_u->ddir == DDIR_READ) {
		/* [한국어] 읽기 요청 분기. */
		if (sd->vmsplice_to_user) {
			/* [한국어] 신형 경로가 아직 가능(이 잡에서 EBADF 본 적 없음)이면 우선 시도. */
			ret = fio_splice_read(td, io_u);
			/*
			 * This kernel doesn't support vmsplice to user
			 * space. Reset the vmsplice_to_user flag, so that
			 * we retry below and don't hit this path again.
			 */
			if (ret == -EBADF)
				/* [한국어] 신형 경로가 -EBADF 로 실패 = 커널이 vmsplice-to-user 를
				 * 지원하지 않음. 플래그 내리면 아래 if(!vmsplice_to_user) 에서
				 * 자동으로 구형 경로로 재시도. 이후 잡 수명 동안 신형 경로는 재시도
				 * 안 함. */
				sd->vmsplice_to_user = 0;
		}
		if (!sd->vmsplice_to_user)
			/* [한국어] 플래그가 0 — (처음부터 0 이었거나 방금 내려갔거나). 구형 경로 실행.
			 * 방금 내려간 케이스의 경우 ret 값이 여기서 덮어쓰여 신형의 -EBADF 결과는
			 * 버려지고 구형의 성공/실패 값으로 교체된다. */
			ret = fio_splice_read_old(td, io_u);
	} else if (io_u->ddir == DDIR_WRITE)
		/* [한국어] 쓰기 경로 — 폴백 로직 없음(쓰기 방향 vmsplice 는 모든 커널이 지원). */
		ret = fio_splice_write(td, io_u);
	else if (io_u->ddir == DDIR_TRIM)
		/* [한국어] TRIM(discard) — splice 로 표현 불가하므로 io_u.c 공용 헬퍼에 위임.
		 * do_io_u_trim 은 내부적으로 BLKDISCARD ioctl 또는 fallocate(PUNCH_HOLE) 사용. */
		ret = do_io_u_trim(td, io_u);
	else
		/* [한국어] 나머지 SYNC/DATASYNC/SYNC_FILE_RANGE 계열 — do_io_u_sync 위임.
		 * fsync/fdatasync/sync_file_range(2) 시스템 콜 수행. */
		ret = do_io_u_sync(td, io_u);

	if (ret != (int) io_u->xfer_buflen) {
		/* [한국어] 요청 길이와 반환값 불일치 — 부분 전송이거나 에러. */
		if (ret >= 0) {
			/* [한국어] 음수 아님 = 0 이상의 부분 전송. 에러는 아니고 short transfer. */
			io_u->resid = io_u->xfer_buflen - ret;
			/* [한국어] 나머지 바이트 기록 — fio 코어가 이 값 보고 재제출 여부 결정. */
			io_u->error = 0;
			/* [한국어] 에러 플래그 명시적 클리어. */
			return FIO_Q_COMPLETED;
			/* [한국어] 동기 엔진이므로 즉시 완료 통지. 부분 전송도 "완료"로 보고 후
			 * resid 로 fio 코어가 처리. */
		} else
			io_u->error = errno;
			/* [한국어] ret < 0 — 하위 함수가 실패 직후라 글로벌 errno 에 에러 코드가
			 * 남아 있음. 이를 io_u->error 로 복사(시스템 콜 직후라 값 보존됨). */
	}

	if (io_u->error) {
		/* [한국어] 에러가 있으면 기록 및 사용자 메시지 출력. */
		td_verror(td, io_u->error, "xfer");
		/* [한국어] td 의 에러 상태에 기록 → 잡 종료 통계와 exit 코드에 반영. */
		if (io_u->error == EINVAL)
			/* [한국어] EINVAL 은 대부분 "해당 파일시스템이 splice 를 지원하지 않음"
			 * (예: 일부 NFS 버전, 특수 FS). 사용자에게 친절히 힌트. */
			log_err("fio: looks like splice doesn't work on this"
					" file system\n");
	}

	return FIO_Q_COMPLETED;
	/* [한국어] 동기 완료 통지. 에러든 성공이든 io_u->error 로 구분된다. */
}

/*
 * [한국어]
 * fio_spliceio_cleanup - ioengine_ops.cleanup 진입점.
 *                        잡 종료 시 파이프 fd 와 엔진 상태 구조체 해제.
 *
 * @td: 잡 스레드 thread_data — io_ops_data 에 struct spliceio_data 포인터 연결됨.
 *
 * 해제 절차: close(pipe[0]) → close(pipe[1]) → free(sd).
 * 파일 fd 는 generic_close_file 이 별도 담당.
 *
 * 왜 필요한가: init 에서 할당된 자원(파이프 2개 + 구조체) 은 잡 종료 시 반드시 해제해야
 *             프로세스 레벨 fd 고갈을 막을 수 있다. 파이프는 커널 버퍼를 점유하므로 leak
 *             시 메모리 압박 누적.
 *
 * 실행 컨텍스트: 잡 스레드, 1회만 호출. sd==NULL 방어(init 실패 경로에서도 안전).
 * 호출 체인: td_io_cleanup → ioengine_ops.cleanup → [fio_spliceio_cleanup].
 */
static void fio_spliceio_cleanup(struct thread_data *td)
{
	struct spliceio_data *sd = td->io_ops_data;
	/* [한국어] 엔진 상태 포인터 취득. init 이 실패했거나 호출된 적 없으면 NULL 가능. */

	if (sd) {
		/* [한국어] NULL 가드 — init 실패 경로 또는 중복 cleanup 호출 대비. */
		close(sd->pipe[0]);
		/* [한국어] 파이프 읽기 끝 닫기 — 커널 측 참조 카운트 감소. */
		close(sd->pipe[1]);
		/* [한국어] 파이프 쓰기 끝 닫기 — 둘 다 닫히면 커널이 pipe_inode_info 회수. */
		free(sd);
		/* [한국어] 상태 구조체 해제. td->io_ops_data 는 fio 코어가 회수 후 NULL 로 설정. */
	}
}

/*
 * [한국어]
 * fio_spliceio_init - ioengine_ops.init 진입점. splice 엔진 초기화.
 *
 * @td: 잡 스레드 thread_data.
 * @return: 0=성공, 1=실패(잡 중단).
 *
 * 수행 단계:
 *   (1) malloc(sizeof(spliceio_data)) 로 엔진 상태 할당.
 *   (2) pipe(sd->pipe) 로 익명 파이프 쌍 생성 — fd[0]=reader, fd[1]=writer.
 *       실패 시 td_verror + free(sd) + return 1.
 *   (3) vmsplice_to_user = 1 (낙관적 지원 가정).
 *   (4) vmsplice_to_user_map = 1 (mmap 경로 낙관적 활성).
 *   (5) td->io_ops_data = sd 로 엔진 상태 연결.
 *
 * 주의: 원본 코드는 malloc 실패 체크가 없다 — NULL 리턴 시 pipe(sd->pipe) 가 NULL 역참조
 *       로 SIGSEGV. 메모리 여유가 있는 환경을 전제로 한 단순화로, 코드 수정 금지 원칙 따라
 *       유지.
 *
 * 낙관적 활성화 이유:
 *   현대 커널 대부분에서 vmsplice-to-user(+map) 가 동작하므로 매 잡마다 사전 검사를
 *   수행하는 비용보다 첫 I/O 시 EBADF/EFAULT 관측 후 폴백하는 편이 저렴. 폴백도 잡 수명
 *   동안 1회만 발생.
 *
 * 실행 컨텍스트: 잡 스레드, 잡 시작 시 1회.
 * 호출 체인: td_io_init → ioengine_ops.init → [fio_spliceio_init].
 */
static int fio_spliceio_init(struct thread_data *td)
{
	struct spliceio_data *sd = malloc(sizeof(*sd));
	/* [한국어] 엔진 상태 할당. 실패(NULL) 시 아래 pipe(sd->pipe) 에서 SIGSEGV — 원본
	 * 설계 그대로 유지. */

	if (pipe(sd->pipe) < 0) {
		/* [한국어] pipe(2) 시스템 콜 — 익명 파이프 생성.
		 *   - sd->pipe[0] : 파이프 읽기 끝 (consumer).
		 *   - sd->pipe[1] : 파이프 쓰기 끝 (producer).
		 * 실패 원인: EMFILE (프로세스 fd 고갈), ENFILE (시스템 전역 fd 고갈). */
		td_verror(td, errno, "pipe");
		free(sd);
		/* [한국어] 부분 초기화 정리 — 상태 구조체는 이미 할당돼 있으므로 해제. */
		return 1;
		/* [한국어] 비-0 반환 = init 실패 → 잡 중단. */
	}

	/*
	 * Assume this work, we'll reset this if it doesn't
	 */
	sd->vmsplice_to_user = 1;
	/* [한국어] 신형 vmsplice-to-user 경로 낙관적 활성. fio_splice_read 가 -EBADF 돌려주면
	 * fio_spliceio_queue 에서 0 으로 내림. */

	/*
	 * Works with "real" vmsplice to user, eg mapping pages directly.
	 * Reset if we fail.
	 */
	sd->vmsplice_to_user_map = 1;
	/* [한국어] mmap 덮어쓰기 기반 "페이지 이사" 경로도 낙관적 활성. EFAULT 관측 시 0 으로. */

	td->io_ops_data = sd;
	/* [한국어] fio 코어에 엔진 상태 포인터 연결 — 이후 모든 콜백이 td->io_ops_data 로 접근. */
	return 0;
	/* [한국어] 성공. */
}

/*
 * [한국어] ioengine — splice 엔진의 콜백 테이블(ioengine_ops).
 *
 * 설정자: 이 translation unit 의 파일 스코프 초기화 (1회).
 * 읽는 자: register_ioengine() 이후 load_ioengine("splice") 가 매칭해서 td->io_ops 로
 *          연결. 이후 잡 루프가 각 콜백을 호출.
 * 생명주기: 프로세스 수명 전체(정적 저장 기간). unregister 시 엔진 리스트에서만 제거.
 * 동기화: 등록/해제는 프로세스 메인 스레드의 constructor/destructor 에서만 수행 — 잡 스레드는
 *         읽기만 함. 각 콜백 내부에서는 td->io_ops_data 로 잡별 상태 격리.
 *
 * 파일 수명 콜백(open/close/get_file_size)은 filesetup.c 의 generic_* 공용 구현 재사용.
 * 이 엔진은 파일 속성을 건드리지 않으므로 그대로 위임하면 충분.
 *
 * 콜백이 없는 필드의 의미:
 *   - .prep       : I/O 당 사전 처리 불필요(splice 직접 호출로 충분).
 *   - .commit     : FIO_SYNCIO 라 queue 가 즉시 완료 → 배치 제출 개념 없음.
 *   - .getevents  : 비동기 수확 경로 없음(SYNCIO).
 *   - .event      : 동일.
 *   - .io_u_init/free : io_u 버퍼 커스터마이즈 없음.
 *   - .options/option_struct_size : 엔진 전용 옵션 없음(fio 공용 옵션만 사용).
 *   - .unlink_file/invalidate : 공용 동작으로 충분.
 */
static struct ioengine_ops ioengine = {
	.name		= "splice",
	/* [한국어] 엔진 식별 문자열. 잡 파일의 `ioengine=splice` 또는 CLI `--ioengine=splice`
	 * 와 매칭. 설정자: 이 초기화. 읽는 자: load_ioengine 의 strcmp. 값 범위: NUL 종결
	 * ASCII, 전역 중복 불가(register_ioengine 시 검증). 동기화: 등록 후 불변. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전. fio.h 의 매크로 — 코어가 이 값과 자신의 기대치를 비교해
	 * 바이너리 호환성 검사(외부 .so 엔진에서 특히 중요). 설정자: 초기화. 읽는 자:
	 * register_ioengine. 값 범위: 정수. 동기화: 불변. */

	.init		= fio_spliceio_init,
	/* [한국어] 잡 시작 시 1회 호출되는 초기화 콜백. 파이프 쌍 생성 + 낙관 플래그 세팅.
	 * 반환 0=성공, 비-0=실패(잡 중단). 설정자: 초기화. 읽는 자: td_io_init.
	 * 동기화: 잡 스레드 단독, 1회. */

	.queue		= fio_spliceio_queue,
	/* [한국어] I/O 제출 콜백(동기). ddir 에 따라 splice_read/write/old 또는 do_io_u_trim/
	 * sync 디스패치. 반환 항상 FIO_Q_COMPLETED (에러든 성공이든; 실제 결과는 io_u->error 로).
	 * 설정자: 초기화. 읽는 자: td_io_queue. 동기화: 잡 스레드 전용, 재진입 없음. */

	.cleanup	= fio_spliceio_cleanup,
	/* [한국어] 잡 종료 시 1회 호출. 파이프 fd close + struct free. init 와 대칭.
	 * 설정자: 초기화. 읽는 자: td_io_cleanup. 동기화: 잡 스레드 단독. */

	.open_file	= generic_open_file,
	/* [한국어] filesetup.c 의 공용 open — O_RDONLY/O_WRONLY/O_RDWR 과 O_CREAT/O_TRUNC/
	 * O_DIRECT 등 옵션 조합으로 open(2). splice 엔진은 특별한 파일 준비가 필요 없으므로
	 * 공용 구현 직접 재사용. 설정자: 초기화. 읽는 자: td_io_open_file. 동기화: 잡 스레드. */

	.close_file	= generic_close_file,
	/* [한국어] 공용 close — close(2) + 파일 해시 제거. open_file 과 짝. 설정자: 초기화.
	 * 읽는 자: td_io_close_file. 동기화: 잡 스레드. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 공용 stat(2) 기반 파일 크기 계산. splice 는 파일 크기 특수 해석이 없음.
	 * 설정자: 초기화. 읽는 자: td_io_get_file_size(setup 단계). 동기화: 잡 스레드. */

	.flags		= FIO_SYNCIO | FIO_PIPEIO,
	/* [한국어] 엔진 특성 비트마스크.
	 *   FIO_SYNCIO (0x01) — queue() 가 직접 I/O 완료 후 반환하는 동기 엔진임을 표식.
	 *       코어가 commit/getevents/event 호출을 생략하고, issue_time 기록을 queue 직전에
	 *       자동 수행하며, iodepth>1 이어도 큐잉 없이 순차 처리한다.
	 *   FIO_PIPEIO (0x04) — 엔진이 파이프를 사용한다는 표식. 시크 가능한 파일 포지션 개념과
	 *       다르다는 것을 코어에 알림(예: sequential offset 전제 최적화 차단).
	 * 설정자: 이 초기화. 읽는 자: 잡 루프 전반(io_u.c, backend.c). 동기화: 불변. */
};

/*
 * [한국어]
 * fio_spliceio_register - constructor (GCC __attribute__((constructor)) = fio_init).
 *
 * 프로세스 시작 시 main() 진입 **전에** 자동 실행되어 이 엔진을 fio 코어의 전역 엔진
 * 리스트(engine_list)에 등록한다. 정적 링크 시 .init_array 섹션을 통해 libc 의 동적
 * 로더가 호출하고, 공유 라이브러리 링크 시에는 dlopen 시 실행된다.
 *
 * 실행 컨텍스트: 프로세스 메인 스레드, 단 1회.
 * 호출 체인: _start → __libc_start_main → .init_array → [fio_spliceio_register]
 *            → register_ioengine(&ioengine).
 */
static void fio_init fio_spliceio_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] ioengines.c 가 관리하는 flist(doubly-linked list) 에 이 엔진 추가.
	 * 이후 load_ioengine("splice") 호출 시 이 항목이 매칭되어 td->io_ops 로 연결됨. */
}

/*
 * [한국어]
 * fio_spliceio_unregister - destructor (GCC __attribute__((destructor)) = fio_exit).
 *
 * 프로세스 종료 시 main() 복귀 후 자동 실행되어 엔진 리스트에서 언링크.
 * 정적 바이너리에서는 논리적으로 불필요하지만(프로세스 종료 시 전체 해제) 공유 라이브러리
 * 빌드의 dlclose 대비 안전 장치.
 *
 * 실행 컨텍스트: 프로세스 메인 스레드, 단 1회.
 * 호출 체인: main 복귀 → .fini_array → [fio_spliceio_unregister]
 *            → unregister_ioengine(&ioengine).
 */
static void fio_exit fio_spliceio_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 엔진 리스트에서 제거 — 더 이상 load_ioengine("splice") 매칭 불가. */
}
