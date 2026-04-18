/*
 * io_uring engine
 *
 * IO engine using the new native Linux aio io_uring interface.
 *
 */
/*
 * [한국어 설명] io_uring 기반 Linux 비동기 I/O 엔진 (io_uring.c)
 *
 * === 파일의 역할 ===
 * 리눅스 커널 5.1+의 io_uring 서브시스템을 사용하는 fio I/O 엔진 두 종류
 * ("io_uring", "io_uring_cmd")를 단일 파일로 구현한다. io_uring은 커널-유저
 * 공유 메모리 기반의 링버퍼(SQ Ring/CQ Ring) 인터페이스로, libaio가 매번
 * io_submit(2)/io_getevents(2) 시스콜로 iocb/event를 복사·이동해야 했던 비용을
 * 거의 0으로 줄인다. 본 파일은 io_uring_setup(2)/io_uring_enter(2)/
 * io_uring_register(2) 세 시스콜과 mmap(2)으로 링을 매핑한 뒤, fio의 표준 콜백
 * (init/post_init/prep/queue/commit/getevents/event/cleanup/open_file/close_file/
 * io_u_init/io_u_free + ZBD/FDP 6개)을 구현한다. 또한 hipri(IORING_SETUP_IOPOLL),
 * sqthread_poll(IORING_SETUP_SQPOLL+SQ_AFF), fixedbufs(IORING_REGISTER_BUFFERS),
 * registerfiles(IORING_REGISTER_FILES), nonvectored(READ/WRITE vs READV/WRITEV),
 * uncached(RWF_DONTCACHE), nowait(RWF_NOWAIT), force_async(IOSQE_ASYNC),
 * cmd_type=nvme(IORING_OP_URING_CMD + nvme_uring_cmd), md_per_io_size/pi_act/
 * apptag/apptag_mask/pi_chk(Protection Information), readfua/writefua/deac/
 * write_mode/verify_mode 등 io_uring과 NVMe 패스스루의 거의 모든 주요 기능을
 * 옵션으로 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=io_uring / io_uring_cmd 로 선택되어 fio 플러그인 레지스트리에 등록된다.
 * 실행 흐름:
 *   1) 프로세스 로딩 시 fio_ioring_register() (constructor 속성)이 두 ioengine_ops
 *      인스턴스(ioengine_uring, ioengine_uring_cmd)를 register_ioengine()으로 fio
 *      코어의 engine_list에 등록.
 *   2) parse_options()가 --ioengine=io_uring 매칭 → backend.c → td_io_init →
 *      fio_ioring_init: 옵션 검증 → ioring_data 할당 → iodepth round_pow2 →
 *      io_u_index/iovecs/cmdprio/dsm/ext_opts/(md_buf+pi_attr) 준비.
 *   3) io_u 풀이 채워진 뒤 td_io_init 후속의 post_init: fio_ioring_post_init
 *      (io_uring) 또는 fio_ioring_cmd_post_init (io_uring_cmd) → io_uring_setup(2)
 *      로 인스턴스 생성, mmap(2)으로 SQ Ring/SQEs/CQ Ring 3영역 매핑,
 *      옵션에 따라 IORING_REGISTER_BUFFERS / IORING_REGISTER_FILES.
 *   4) I/O 루프(do_io): get_io_u → td_io_prep(=fio_ioring_prep 또는 _cmd_prep)으로
 *      io_u→SQE 변환 → td_io_queue(=fio_ioring_queue)가 SQ array[tail&mask]에
 *      io_u->index 기록 후 tail++ → td_io_commit(=fio_ioring_commit)이 모인 SQE를
 *      io_uring_enter(to_submit, 0, GETEVENTS|NO_IOWAIT)로 커널에 알림 → 완료가
 *      필요하면 td_io_getevents(=fio_ioring_getevents)가 CQ tail-head 만큼 수확,
 *      개별 CQE는 td_io_event(=fio_ioring_event/_cmd_event)로 io_u 복원.
 *   5) 종료 시 fio_ioring_cleanup가 munmap+close, 프로세스 종료 시
 *      fio_ioring_unregister() (destructor)가 두 ioengine_ops를 unregister.
 * 실행 컨텍스트는 fio 잡 스레드 1개이며, SQPOLL 옵션이 켜지면 추가로 커널 측
 * io_sq_thread 워커 스레드가 별도 CPU(또는 자동)에서 SQ 링을 폴링한다 — 두
 * 스레드는 SQ/CQ 메모리만으로 동기화하므로 atomic_load_acquire/store_release
 * 의미를 반드시 사용한다(arm64처럼 약한 메모리 모델에서 필수).
 *
 * === 타 모듈과의 연결 ===
 * 상단(이 파일을 호출):
 *   - backend.c(do_io 루프), ioengines.c(td_io_* 디스패처), stat.c(io_u_mark_*),
 *     io_u.c(io_u_set/clear, populate_verify_io_u, io_u_queued).
 *   - cmdprio.c/h(우선순위 정책), zbd.c(zone reset 폴백), verify.c(VERIFY_NONE 등).
 *   - nvme.c(공용 NVMe 헬퍼: fio_nvme_get_info / fio_nvme_uring_cmd_prep /
 *     fio_nvme_pi_fill / fio_nvme_generate_guard / fio_nvme_pi_verify /
 *     fio_nvme_get_zoned_model / fio_nvme_report_zones / fio_nvme_reset_wp /
 *     fio_nvme_get_max_open_zones / fio_nvme_iomgmt_ruhs).
 *   - blkzoned(libblkzoned: report_zones / reset_wp / get_max_open_zones /
 *     finish_zone / move_zone_wp).
 * 하단(이 파일이 호출):
 *   - 리눅스 UAPI <linux/io_uring.h>: SQE(io_uring_sqe)/CQE(io_uring_cqe)/
 *     io_uring_params/io_uring_probe/io_uring_attr_pi와 IORING_SETUP_*/IORING_FEAT_*/
 *     IORING_REGISTER_*/IORING_OP_*/IORING_ENTER_*/IOSQE_* 상수.
 *   - 시스콜: __NR_io_uring_setup(SYS 425), __NR_io_uring_enter(SYS 426),
 *     __NR_io_uring_register(SYS 427), 그리고 mmap/munmap/close/ioctl/open.
 *   - libc: calloc/malloc/free/usleep/memset/memcpy/strstr/strcpy/strlcat/snprintf.
 * 데이터 흐름:
 *   io_u(buf=xfer_buf, offset, ddir, file) →[prep]→ SQE.{opcode/fd/addr/len/off/
 *   rw_flags/buf_index/user_data=io_u/cmd[64B for NVMe]} →[queue]→ SQ array →
 *   [enter]→ 커널 io_uring 코드 → 블록/NVMe 드라이버 → 디바이스 → CQE.{res/flags/
 *   user_data} → CQ Ring →[event]→ io_u.{error/resid} → put_io_u.
 *   NVMe 패스스루 경로(io_uring_cmd)에서는 prep 시 SQE의 확장 64B(SQE128)에
 *   nvme_uring_cmd 구조체를 패킹하고 PI/메타데이터 버퍼·DSM range·CDW12 FUA/
 *   PRINFO/DEAC 비트까지 직접 채운다.
 * 공유 상태:
 *   - SQ/CQ 링 버퍼와 SQE/CQE 배열 자체는 커널-유저 공유 메모리(mmap MAP_SHARED).
 *     head/tail은 다른 컨텍스트에서 갱신되므로 atomic_load_acquire/
 *     atomic_store_release 의미가 필수(파일 내 atomic_load_relaxed/acquire/
 *     atomic_store_release/relaxed 4종 사용 — flags는 relaxed로 충분).
 *   - registerfiles 사용 시 fd→커널 등록 인덱스 매핑이 ld->fds[]와
 *     fio_file::engine_pos에 분산 — open_file/close_file에서 정합성 유지.
 *   - SQE/CQE 슬롯은 iodepth 동안 한 io_u가 점유 — getevents에서 head를
 *     "데이터 읽기 전에" 전진시키는 안전성은 "CQ가 iodepth만큼 크고 SQE가
 *     CQE 처리 전 재사용되지 않음" 불변량에 의존(fio_ioring_cqring_reap 주석 참조).
 *
 * === 주요 함수/구조체 요약 ===
 * 초기화/해제:
 *   - fio_ioring_register/unregister: constructor/destructor — 두 ioengine_ops 등록.
 *   - fio_ioring_init: 옵션 검증, ioring_data 할당, iodepth round_pow2,
 *     io_u_index/md_buf/pi_attr/iovecs/dsm/ext_opts 준비.
 *   - fio_ioring_post_init / fio_ioring_cmd_post_init: io_uring_setup +
 *     IORING_REGISTER_BUFFERS/FILES + mmap.
 *   - fio_ioring_queue_init / fio_ioring_cmd_queue_init: setup syscall +
 *     graceful degradation(DEFER_TASKRUN→SINGLE_ISSUER→COOP_TASKRUN→CQSIZE 순).
 *   - fio_ioring_mmap: SQ Ring(mmap[0]) + SQEs(mmap[1], SQE128 시 2배) +
 *     CQ Ring(mmap[2], CQE32 시 cqes 2배) 3 영역 매핑.
 *   - fio_ioring_register_files: IORING_REGISTER_FILES, generic_open_file 후
 *     fd→ld->fds 저장, 등록 후 f->fd=-1.
 *   - fio_ioring_probe: IORING_REGISTER_PROBE로 nonvectored=-1 자동 결정.
 *   - fio_ioring_cleanup / fio_ioring_unmap: munmap×3 + close(ring_fd) + free 7종.
 *   - fio_ioring_io_u_init / fio_ioring_io_u_free: io_u_index 등록 + md_buf
 *     슬라이스 + pi_attr 슬라이스 + nvme_pi_data 할당/해제.
 * I/O 경로(일반 io_uring):
 *   - fio_ioring_prep: io_u→SQE 변환(opcode 테이블 ddir_to_op[ddir][nonvectored],
 *     fixedbufs면 fixed_ddir_to_op, ddir_sync면 FSYNC/FDATASYNC/SYNC_FILE_RANGE,
 *     TRIM이면 BLOCK_URING_CMD_DISCARD).
 *   - fio_ioring_prep_md: io_uring_attr_pi에 메타버퍼·seed(=SLBA) 채움.
 *   - fio_ioring_setup_pi: 일반 경로의 가드 태그(CRC) 생성.
 *   - fio_ioring_queue: ring->array[tail&mask]=io_u->index → store_release(tail+1)
 *     + DDIR_SYNCFS / async_trim_fail 시 do_io_u_sync/trim 동기 폴백.
 *   - fio_ioring_commit: SQPOLL=NEED_WAKEUP 분기 + 일반=io_uring_enter 루프
 *     (EAGAIN/EINTR 시 cqring_reap+usleep(1) 백오프).
 *   - fio_ioring_cqring_reap: load_acquire(tail) - head → store_relaxed(head+n).
 *   - fio_ioring_getevents: cqring_reap 루프 + (sqpoll 아니면) io_uring_enter(0, min).
 *   - fio_ioring_event: CQE.user_data → io_u, res 검사(=xfer_buflen 또는 trim 0
 *     성공), 부분=resid, 실패=-res.
 * I/O 경로(NVMe 패스스루 io_uring_cmd):
 *   - fio_ioring_cmd_prep: SQE128 인덱스 << 1, IORING_OP_URING_CMD,
 *     NVME_URING_CMD_IO[_VEC], DSM 슬라이스, verify_mode=compare 시 nvme_compare로
 *     opcode 변환, 마지막에 fio_nvme_uring_cmd_prep로 NVMe CDW0~15 채움.
 *   - fio_ioring_cmd_nvme_pi: fio_nvme_pi_fill로 SQE.cmd에 가드/REFTAG/APPTAG.
 *   - fio_ioring_cmd_event: index<<1, NVMe SCT/SC 양수=디바이스 에러
 *     (IO_U_F_DEVICE_ERROR), pi_act=0 시 fio_nvme_pi_verify.
 *   - fio_ioring_cmd_errdetails: "sct=0xNN; sc=0xNN" 포맷 메시지 반환.
 *   - fio_ioring_cmd_init: write_mode→write_opcode + readfua/writefua→cdw12_flags.
 *   - parse_prchk_flags: "GUARD,REFTAG,APPTAG" 토큰 → NVME_IO_PRINFO_PRCHK_*.
 *   - fio_ioring_cmd_open_file/get_file_size/close_file: NVMe 네임스페이스 정보
 *     조회 + 일반 파일 경로 위임.
 * ZBD/FDP 콜백:
 *   - 일반 io_uring: blkzoned_* 위임(get_zoned_model/report_zones/reset_wp/
 *     get_max_open_zones/finish_zone/move_zone_wp).
 *   - io_uring_cmd: fio_nvme_* 위임 + fio_ioring_cmd_fetch_ruhs(FDP RUH 상태).
 * 핵심 자료구조:
 *   - struct ioring_data: 잡 스레드 1개당 1 — ring_fd, sq_ring/cq_ring/sqes,
 *     io_u_index, md_buf/pi_attr, fds, iovecs, sq_ring_mask/cq_ring_mask,
 *     queued/cq_ring_off/iodepth/prepped/async_trim_fail/is_uring_cmd_eng,
 *     mmap[3], cmdprio, dsm, cdw12_flags[DDIR_RWDIR_CNT], write_opcode, ext_opts.
 *   - struct ioring_options: hipri/readfua/writefua/deac/write_mode/verify_mode/
 *     cmdprio_options/fixedbufs/registerfiles/sqpoll_thread/sqpoll_set/sqpoll_cpu/
 *     nonvectored/uncached/nowait/force_async/md_per_io_size/pi_act/apptag/
 *     apptag_mask/prchk/pi_chk/cmd_type.
 *   - struct io_sq_ring/io_cq_ring: mmap된 커널-공유 링 헤더 포인터 묶음.
 *   - struct ioring_mmap[3]: munmap용 (ptr,len) 보관.
 *   - struct logical_block_metadata_cap: FS_IOC_GETLBMD_CAP UAPI(헤더 누락 시
 *     로컬 정의) — PI/메타데이터 능력 비트맵.
 *   - enum uring_cmd_type/uring_cmd_write_mode/uring_cmd_verify_mode: 옵션 값.
 */
/* [한국어] === 표준 라이브러리 헤더 (왜 필요한지) === */
#include <stdlib.h>
/* [한국어] calloc/malloc/free — ioring_data, io_u_index, md_buf, pi_attr, iovecs,
 * fds, dsm, ld 자체와 fio_ioring_probe 임시 버퍼, fio_ioring_cmd_errdetails 메시지
 * 버퍼 등 모든 동적 할당에 필요. abs/exit 등 기타 표준 유틸리티 함수도 제공. */
#include <unistd.h>
/* [한국어] close(2) — fio_ioring_unmap에서 ring_fd close, fio_get_pi_info에서
 * 임시 fd close. usleep(3) — fio_ioring_commit이 EAGAIN/EINTR 백오프에 사용.
 * syscall(3) — __NR_io_uring_setup/_register/_enter 직접 호출용 fallback. */
#include <errno.h>
/* [한국어] errno/EAGAIN/EINTR/ENOMEM/EINVAL/ENOTSUP/ENOSYS — io_uring_enter/
 * setup/register 실패 분류와 graceful degradation 분기에 필수.
 * EAGAIN: 리소스 일시 부족(SQPOLL 큐 포화 등) — 재시도.
 * EINTR: 시그널 인터럽트 — 재시도.
 * EINVAL: 커널 미지원 플래그 — DEFER_TASKRUN/COOP_TASKRUN/CQSIZE 순으로 제거.
 * ENOSYS: io_uring_setup 자체 미지원 — "kernel doesn't support io_uring" 메시지. */
#include <sys/time.h>
/* [한국어] struct timeval/timespec 정의 — fio_gettime 으로 issue_time 기록 시 사용
 * (실제 호출은 fio.h가 제공하는 fio_gettime 매크로). */
#include <sys/resource.h>
/* [한국어] getrlimit/setrlimit 등 — io_uring은 RLIMIT_MEMLOCK 한계와 연관(고정
 * 버퍼 등록 시). 본 파일은 직접 호출하지 않으나 시스템 헤더 의존성으로 포함. */

/* [한국어] === fio 내부 헤더 (각 헤더가 공급하는 심볼) === */
#include "../fio.h"
/* [한국어] fio 코어 프레임워크. 본 파일이 사용하는 핵심 심볼:
 *  - struct thread_data, struct io_u, struct fio_file, struct ioengine_ops,
 *    enum fio_ddir(DDIR_READ/WRITE/TRIM/SYNC/DATASYNC/SYNC_FILE_RANGE/SYNCFS),
 *    enum fio_q_status(FIO_Q_QUEUED/COMPLETED/BUSY).
 *  - 매크로/함수: container_of/PTR_ALIGN, FILE_ENG_DATA/FILE_SET_ENG_DATA,
 *    for_each_file/for_each_rw_ddir, fio_gettime, fio_fill_issue_time,
 *    do_io_u_sync/do_io_u_trim, populate_verify_io_u, io_u_set/clear,
 *    io_u_mark_submit/complete/queued, td_verror, log_err, fio_ro_check,
 *    register_ioengine/unregister_ioengine, fio_init/fio_exit (constructor/
 *    destructor 속성 매크로), generic_open_file/close_file/get_file_size,
 *    td_max_bs, td_write/td_trim, page_mask/page_size.
 *  - atomic_load_acquire/atomic_load_relaxed/atomic_store_release/
 *    atomic_store_relaxed: SQ/CQ 링 head/tail 동기화 매크로. */
#include "../lib/pow2.h"
/* [한국어] is_power_of_2(), roundup_pow2() — io_uring 링 크기는 2^n 이어야 하므로
 * fio_ioring_init에서 td->o.iodepth를 roundup_pow2로 올림. */
#include "../optgroup.h"
/* [한국어] FIO_OPT_C_ENGINE / FIO_OPT_G_IOURING 옵션 카테고리·그룹 매크로.
 * options[] 테이블의 .category/.group 채울 때 사용. */
#include "../lib/memalign.h"
/* [한국어] 메모리 정렬 헬퍼(fio_memalign 등). md_buf 정렬에 사용 가능 — 본 파일은
 * page_mask/mem_align 직접 계산으로 정렬한다. */
#include "../lib/fls.h"
/* [한국어] find last set bit. ilog2() 의존 — fio_get_pi_info에서 lba_size의 log2를
 * lba_shift에 저장할 때 사용. */
#include "../lib/roundup.h"
/* [한국어] roundup_pow2() — iodepth를 2^n으로 올림. */
#include "../verify.h"
/* [한국어] VERIFY_NONE 상수와 verify 관련 심볼. fio_ioring_open_nvme에서 확장
 * LBA + E2E 보호 + verify 충돌 검사 시 사용. */

/* [한국어] ARCH_HAVE_IOURING: 현재 빌드 아키텍처가 io_uring 시스콜 번호를 정의했는지
 * config.h가 결정한 빌드 시간 가드. 미지원 아키텍처에서는 본 파일이 비어있는
 * 객체로 컴파일되어 링크 에러 없이 엔진만 빠진다. (지원: x86_64/arm64/ppc 등) */
#ifdef ARCH_HAVE_IOURING

#include "../lib/types.h"
/* [한국어] __u8/__u16/__u32/__u64 등 커널 스타일 정수 타입. io_uring UAPI는
 * 커널 헤더 스타일을 사용하므로 본 파일도 동일 타입을 사용. */
#include "../os/linux/io_uring.h"
/* [한국어] fio가 번들한 io_uring UAPI 사본 — 시스템 헤더에 io_uring.h가 없거나
 * 구버전이어도 컴파일 가능하도록 유지. 공급 심볼:
 *  - struct io_uring_sqe(opcode/flags/ioprio/fd/off/addr/len/rw_flags/user_data/
 *    buf_index/cmd[64]/cmd_op/uring_cmd_flags/attr_type_mask/attr_ptr 등),
 *    struct io_uring_cqe(user_data/res/flags),
 *    struct io_uring_params(sq_off/cq_off/flags/features/sq_entries/cq_entries/
 *    sq_thread_cpu), struct io_uring_probe/probe_op, struct io_uring_attr_pi.
 *  - IORING_SETUP_*: IOPOLL/SQPOLL/SQ_AFF/CQSIZE/CLAMP/ATTACH_WQ/R_DISABLED/
 *    SUBMIT_ALL/COOP_TASKRUN/TASKRUN_FLAG/SQE128/CQE32/SINGLE_ISSUER/
 *    DEFER_TASKRUN.
 *  - IORING_FEAT_*: NO_IOWAIT 등 커널 능력 비트.
 *  - IORING_REGISTER_*: BUFFERS/FILES/EVENTFD/PROBE 등.
 *  - IORING_ENTER_*: GETEVENTS/SQ_WAKEUP/NO_IOWAIT.
 *  - IORING_OP_*: NOP/READV/WRITEV/READ/WRITE/READ_FIXED/WRITE_FIXED/FSYNC/
 *    SYNC_FILE_RANGE/URING_CMD 등 50+개.
 *  - IOSQE_*: FIXED_FILE/ASYNC 등 SQE 플래그.
 *  - IORING_SQ_NEED_WAKEUP/CQ_OVERFLOW: SQ flags 비트.
 *  - IORING_OFF_SQ_RING/SQES/CQ_RING: mmap 오프셋 상수.
 *  - IORING_FSYNC_DATASYNC, IORING_RW_ATTR_FLAG_PI, IORING_URING_CMD_FIXED. */
#include "cmdprio.h"
/* [한국어] 공유 우선순위 정책. 공급 심볼: struct cmdprio, struct cmdprio_options,
 * enum cmdprio_mode(NONE/PERC/BSSPLIT), CMDPRIO_OPTIONS() 매크로(options 테이블에
 * cmdprio_percentage/bssplit/class/level/hint 등을 일괄 추가),
 * fio_cmdprio_init/cleanup/set_ioprio. libaio/io_uring/sg가 공유. */
#include "zbd.h"
/* [한국어] Zoned Block Device — get_zoned_model/report_zones/reset_wp/finish_zone/
 * get_max_open_zones/move_zone_wp 콜백과 ZONE_MODE_ZBD 상수. blkzoned 백엔드를
 * 위임 호출하기 위해 포함. */
#include "nvme.h"
/* [한국어] NVMe passthru 공용 헬퍼 — struct nvme_data/nvme_dsm/nvme_dsm_range/
 * nvme_uring_cmd/nvme_pi_data/nvme_cmd_ext_io_opts/nvme_fdp_ruh_status,
 * nvme_cmd_read/write/compare/write_uncor/write_zeroes/verify opcode,
 * NVME_URING_CMD_IO[_VEC], NVME_IO_PRINFO_PRACT/PRCHK_GUARD/REF/APP,
 * NVME_NS_DPS_PI_TYPE1/3, NVME_NVM_NS_16B/64B_GUARD,
 * fio_nvme_get_info / fio_nvme_uring_cmd_prep / fio_nvme_pi_fill /
 * fio_nvme_generate_guard / fio_nvme_pi_verify / fio_nvme_get_zoned_model /
 * fio_nvme_report_zones / fio_nvme_reset_wp / fio_nvme_get_max_open_zones /
 * fio_nvme_iomgmt_ruhs, ilog2, get_slba 등 다수. */

#include <sys/stat.h>
/* [한국어] open/ioctl을 위한 타입(mode_t 등). fio_get_pi_info가 open(2)으로
 * 블록 디바이스를 잠시 열어 FS_IOC_GETLBMD_CAP ioctl 후 닫을 때 필요. */

/*
 * IO 무결성 검사 플래그 정의
 * Protection Information(PI) 관련 플래그가 없을 경우 직접 정의
 */
/* [한국어] IO_INTEGRITY_CHK_GUARD가 시스템 헤더에 없으면(=구버전 커널 헤더)
 * 본 파일이 직접 동일 비트 풀이로 정의 — io_uring_attr_pi.flags의 비트 의미는
 * io_uring UAPI에 고정된 ABI이므로 안전하게 폴백 가능. */
#ifndef IO_INTEGRITY_CHK_GUARD
/* flags for integrity meta */
/* 무결성 메타데이터 플래그 */
#define IO_INTEGRITY_CHK_GUARD		(1U << 0) /* enforce guard check */
/* [한국어] 가드 태그 검사 강제. T10 PI에서 데이터 블록 자체의 CRC(보통 16비트
 * CRC16-T10DIF, 또는 NVMe 64비트 CRC64)를 디바이스가 검증하도록 요청하는 비트.
 * 설정 시 디바이스가 read 결과의 CRC와 메타데이터 가드 필드를 비교 — 불일치면
 * NVMe Status Code 0x82(Guard Check Error) 또는 SCSI ASC=0x10(LBA OUT OF RANGE).
 * 호스트가 pi_chk=GUARD 옵션을 주면 prchk |= NVME_IO_PRINFO_PRCHK_GUARD 후
 * 본 비트로 변환되어 io_uring_attr_pi.flags에 OR. */

#define IO_INTEGRITY_CHK_REFTAG		(1U << 1) /* enforce ref check */
/* [한국어] 참조 태그(REFTAG) 검사 강제. T10 PI Type1/Type2 에서 32비트 reference
 * tag가 LBA(Type1=하위 32비트, Type2=호스트 지정)와 일치해야 한다 — 잘못된 LBA로
 * 미스라우팅된 데이터 감지용. Type3에서는 REFTAG 무시(escape 0xFFFFFFFF).
 * 본 파일은 fio_ioring_prep_md에서 seed=(__u32)SLBA로 채워 커널이 시작 LBA 기준
 * 으로 증가 검증하도록 한다. */

#define IO_INTEGRITY_CHK_APPTAG		(1U << 2) /* enforce app check */
/* [한국어] 애플리케이션 태그(APPTAG) 검사 강제. 16비트 Application Tag 가
 * 호스트가 지정한 값과 apptag_mask 마스크 적용 후 일치하는지 디바이스가 검증.
 * 본 파일은 옵션 apptag(기본 0x1234)/apptag_mask(기본 0xffff=완전일치)로 받음.
 * NVMe metadata=in-band 모드에서는 io_uring 한정 apptag_mask=0xffff가 강제됨
 * (fio_ioring_init이 0xffff 아니면 거부). */
#endif /* IO_INTEGRITY_CHK_GUARD */

/*
 * FS_IOC_GETLBMD_CAP ioctl이 정의되지 않은 경우,
 * 논리 블록 메타데이터 기능(Protection Information) 관련 구조체와 상수를 정의
 */
/* [한국어] 시스템 헤더가 FS_IOC_GETLBMD_CAP 을 제공하지 않는 구버전 빌드에서
 * 본 파일이 ABI 호환되도록 동일 정의를 직접 선언. ioctl 번호와 구조체 레이아웃은
 * UAPI 고정이므로 안전하게 폴백 가능. 본 ioctl은 블록 디바이스가 노출하는
 * "논리 블록 메타데이터(PI/EXT/Storage Tag)" 능력을 호스트가 조회하는 경로. */
#ifndef FS_IOC_GETLBMD_CAP
/* Protection info capability flags */
/* 보호 정보 기능 플래그 */
#define	LBMD_PI_CAP_INTEGRITY		(1 << 0)
/* [한국어] 디바이스가 무결성 보호(PI) 기능을 지원함을 표시.
 * 비트 0이 0이면 fio_get_pi_info가 -ENOTSUP 반환 — md_per_io_size 옵션 사용 불가. */

#define	LBMD_PI_CAP_REFTAG		(1 << 1)
/* [한국어] 디바이스가 참조 태그(REFTAG) 기반 검사를 지원. 본 비트로 PI Type 결정:
 * 1이면 NVME_NS_DPS_PI_TYPE1(REFTAG=시작 LBA에서 증가), 0이면 PI_TYPE3(REFTAG 미사용). */

/* Checksum types for Protection Information */
/* 보호 정보에 사용되는 체크섬 유형 */
#define LBMD_PI_CSUM_NONE		0
/* [한국어] 가드 태그 체크섬 없음 — PI를 사용하지 않는 디바이스. */
#define LBMD_PI_CSUM_IP			1
/* [한국어] IP 체크섬(거의 사용되지 않음). 본 파일은 미지원 → -ENOTSUP. */
#define LBMD_PI_CSUM_CRC16_T10DIF	2
/* [한국어] T10 DIF CRC16 — 16바이트 PI 튜플의 가드 필드. SCSI/NVMe 표준.
 * fio_get_pi_info가 NVME_NVM_NS_16B_GUARD 로 매핑. */
#define LBMD_PI_CSUM_CRC64_NVME		4
/* [한국어] NVMe CRC64 — 64비트 가드(NVMe 64B 가드 PI). 더 강한 무결성 보장.
 * fio_get_pi_info가 NVME_NVM_NS_64B_GUARD 로 매핑. */

/*
 * Logical block metadata capability descriptor
 * If the device does not support metadata, all the fields will be zero.
 * Applications must check lbmd_flags to determine whether metadata is
 * supported or not.
 */
/*
 * 논리 블록 메타데이터 기능 디스크립터
 * 디바이스가 메타데이터를 지원하지 않으면 모든 필드가 0
 * lbmd_flags를 확인하여 메타데이터 지원 여부를 판단해야 함
 */
struct logical_block_metadata_cap {
	__u32	lbmd_flags;
	/* [한국어] 디바이스 PI/메타데이터 능력 비트마스크.
	 * 설정자: 커널 블록 레이어가 ioctl 응답으로 채움.
	 * 읽는 자: fio_get_pi_info — LBMD_PI_CAP_INTEGRITY 비트로 PI 지원 여부 판정,
	 *   LBMD_PI_CAP_REFTAG 비트로 PI Type 결정(1=Type1, 0=Type3).
	 * 값 범위: LBMD_PI_CAP_* 비트 OR. 동기화: ioctl 1회 응답이므로 별도 동기화 무. */

	__u16	lbmd_interval;
	/* [한국어] 메타데이터 한 단위가 기술하는 데이터 크기(바이트). 통상 LBA 크기와 동일.
	 * 설정자: 커널. 읽는 자: fio_get_pi_info → data->lba_size 저장 → ilog2로 lba_shift.
	 * 값 범위: 512/4096 등 디바이스 LBA. 동기화: 응답 시 1회. */

	__u8	lbmd_size;
	/* [한국어] 각 interval(=lba) 당 메타데이터 전체 크기(PI 튜플 + opaque tag 포함).
	 * 설정자: 커널. 읽는 자: data->ms 저장. 값 범위: 0/8/16/64 등.
	 * 동기화: 응답 시 1회. */

	__u8	lbmd_opaque_size;
	/* [한국어] 각 interval 당 불투명 블록 태그(opaque tag) 크기. 본 파일은 직접
	 * 사용하지 않지만 storage tag 포함 여부를 필드 단위 비교로 확인할 때 사용.
	 * 설정자: 커널. 읽는 자: 미사용(현재 호스트 측 PI 처리에 한정). */

	__u8	lbmd_opaque_offset;
	/* [한국어] 메타데이터 내 opaque tag 시작 오프셋. 현재 미사용.
	 * 설정자: 커널. 읽는 자: 미사용. */

	__u8	lbmd_pi_size;
	/* [한국어] 각 interval 당 T10 PI 튜플 크기(8/16바이트). data->pi_size 저장.
	 * 설정자: 커널. 읽는 자: fio_get_pi_info → data->pi_size. */

	__u8	lbmd_pi_offset;
	/* [한국어] 메타데이터 내 PI 튜플의 오프셋. 0이면 시작에 위치(=DIX/DIF 기본).
	 * 설정자: 커널. 읽는 자: data->pi_loc = !lbmd_pi_offset (0=tail이 아닌 head 의미). */

	__u8	lbmd_guard_tag_type;
	/* [한국어] 가드 태그 유형(LBMD_PI_CSUM_*).
	 * 설정자: 커널. 읽는 자: fio_get_pi_info의 switch — CRC16_T10DIF→16B_GUARD,
	 *   CRC64_NVME→64B_GUARD. 그 외(NONE/IP)는 -ENOTSUP. */

	__u8	lbmd_app_tag_size;
	/* [한국어] PI Application Tag 크기(바이트, 통상 2). 본 파일은 apptag 16비트 가정. */

	__u8	lbmd_ref_tag_size;
	/* [한국어] PI Reference Tag 크기(바이트, Type1=4). 본 파일은 직접 미사용 — pi_chk
	 * 옵션과 fio_nvme_pi_fill 가 처리. */

	__u8	lbmd_storage_tag_size;
	/* [한국어] PI Storage Tag 크기. 본 파일은 미지원 — 0이 아니면 -ENOTSUP. */

	__u8	pad;
	/* [한국어] 정렬 패딩. UAPI 호환 유지용. */
};

/* [한국어] _IOWR(magic=0x15, nr=2, struct logical_block_metadata_cap) — magic
 * 0x15는 일반 블록 디바이스용 ioctl 그룹 식별자. _IOWR은 양방향(인자 in/out)
 * 데이터 전달을 의미. 본 ioctl은 디바이스의 PI 능력을 조회한다. */
#define FS_IOC_GETLBMD_CAP			_IOWR(0x15, 2, struct logical_block_metadata_cap)
#endif /* FS_IOC_GETLBMD_CAP */

/*
 * uring_cmd 타입 열거형
 * io_uring_cmd로 전달할 수 있는 커맨드 유형 정의
 * 현재는 NVMe passthrough만 지원
 */
enum uring_cmd_type {
	FIO_URING_CMD_NVME = 1,
	/* [한국어] NVMe 패스스루 커맨드. 현재 유일한 값 — 장래 다른 디바이스 패스스루
	 * (예: SCSI URING_CMD, ublk) 확장 시 추가될 예정.
	 * 설정자: "cmd_type=nvme" 옵션 파싱. 읽는 자: fio_ioring_cmd_prep / _post_init /
	 *   _open_file / _get_file_size / _close_file — cmd_type != NVME 이면 -EINVAL.
	 * 동기화: 옵션 불변. */
};

/*
 * uring_cmd 쓰기 모드 열거형
 * NVMe uring_cmd 엔진에서 사용 가능한 쓰기 커맨드 유형
 */
enum uring_cmd_write_mode {
	FIO_URING_CMD_WMODE_WRITE = 1,
	/* [한국어] 일반 Write (NVMe opcode 0x01). 기본값.
	 * 설정자: "write_mode=write". 읽는 자: fio_ioring_cmd_init →
	 *   ld->write_opcode = nvme_cmd_write. */

	FIO_URING_CMD_WMODE_UNCOR,
	/* [한국어] Write Uncorrectable (NVMe opcode 0x04) — 지정 LBA 범위를
	 * "읽기 시 즉시 에러 반환" 상태로 마킹. 데이터 전송 없음. 복구 시나리오 테스트용.
	 * 설정자: "write_mode=uncor". 읽는 자: ld->write_opcode = nvme_cmd_write_uncor. */

	FIO_URING_CMD_WMODE_ZEROES,
	/* [한국어] Write Zeroes (NVMe opcode 0x08) — 데이터 전송 없이 LBA 범위를 0으로.
	 * 호스트→디바이스 대역폭 절감. 옵션 deac=1 이면 CDW12 bit25(DEAC)로 "할당 해제
	 * 힌트(TRIM 유사 시맨틱)" 전달.
	 * 설정자: "write_mode=zeroes". 읽는 자: ld->write_opcode = nvme_cmd_write_zeroes. */

	FIO_URING_CMD_WMODE_VERIFY,
	/* [한국어] Verify (NVMe opcode 0x0C) — 디바이스가 대상 LBA 범위를 읽어 내부
	 * 검증(가드/REFTAG 확인)만 수행, 호스트로 데이터 반환 없음. 매체 무결성 점검용.
	 * 설정자: "write_mode=verify". 읽는 자: ld->write_opcode = nvme_cmd_verify. */
};

/*
 * uring_cmd 검증 모드 열거형
 * 데이터 검증 단계에서 사용할 NVMe 커맨드 유형
 */
enum uring_cmd_verify_mode {
	FIO_URING_CMD_VMODE_READ = 1,
	/* [한국어] 검증 단계에서 일반 Read(NVMe 0x02)로 데이터를 호스트로 가져와
	 * fio가 패턴/CRC 비교. 기본값. 네트워크/PCIe 대역폭 소모.
	 * 설정자: "verify_mode=read". 읽는 자: fio_ioring_cmd_prep 진입 시 read_opcode
	 *   기본값. */

	FIO_URING_CMD_VMODE_COMPARE,
	/* [한국어] NVMe Compare (opcode 0x05) — 호스트가 기대값 버퍼를 보내면 디바이스
	 * 측이 매체 데이터와 비교 후 불일치 시 NVMe Status 0x85(Compare Failure) 반환.
	 * 데이터는 호스트로 돌아오지 않음(대역폭 절감). 설정자: "verify_mode=compare".
	 * 읽는 자: fio_ioring_cmd_prep이 IO_U_F_VER_LIST+DDIR_READ 조건에서 read_opcode
	 *   를 nvme_cmd_compare로 교체 + IO_U_F_VER_IN_DEV 표시. */
};

/*
 * SQ (Submission Queue) Ring 구조체
 *
 * io_uring의 제출 큐 링 버퍼를 유저스페이스에서 접근하기 위한 포인터 모음.
 * 커널과 유저가 mmap()으로 공유하는 메모리 영역을 가리킴.
 *
 * [SQ Ring 동작 원리]
 * - 유저는 tail을 증가시키며 새 SQE 인덱스를 array[]에 추가 (제출)
 * - 커널은 head를 증가시키며 SQE를 소비 (처리)
 * - tail - head = 현재 대기 중인 제출 수
 * - ring_mask를 사용하여 인덱스를 링 크기로 래핑 (& 연산)
 *
 * libaio와의 차이: libaio는 io_submit() 시스콜로 매번 iocb 배열을 복사하지만,
 * io_uring은 공유 메모리의 포인터만 업데이트하므로 복사 비용이 없음.
 */
struct io_sq_ring {
	unsigned *head;
	/* [한국어] SQ head (커널이 소비한 지점). 설정자: 커널. 읽는 자: 유저 queue()가 여유 공간 계산 시 참조.
	 * 값 범위: 0..UINT32_MAX 단조증가(모듈로 ring_entries). 동기화: 커널이 release, 유저는 acquire로 읽음. */

	unsigned *tail;
	/* [한국어] SQ tail (유저가 SQE 채움 완료 지점). 설정자: 유저 queue()가 smp_store_release.
	 * 읽는 자: 커널(또는 SQPOLL 스레드)이 acquire 로드. 값 범위: head <= tail <= head+ring_entries. */

	unsigned *ring_mask;
	/* [한국어] 인덱스 링 마스크(=entries-1). 설정자: 커널이 mmap 영역에 기록.
	 * 읽는 자: queue()가 tail & mask로 array 인덱스. 값 범위: 2^n-1. 동기화: 읽기 전용. */

	unsigned *ring_entries;
	/* [한국어] SQ 링의 총 슬롯 수. 설정자: 커널. 읽는 자: sanity 체크용.
	 * 값 범위: 2^n. 동기화: 읽기 전용. */

	unsigned *flags;
	/* [한국어] SQ 상태 플래그(IORING_SQ_NEED_WAKEUP, IORING_SQ_CQ_OVERFLOW).
	 * 설정자: 커널 SQPOLL 스레드. 읽는 자: commit()이 NEED_WAKEUP 확인 후 enter(SQ_WAKEUP).
	 * 값 범위: 비트 OR. 동기화: 커널 원자 기록 — 유저는 acquire 로드. */

	unsigned *array;
	/* [한국어] array[tail & mask] = sqes[] 인덱스 — 링 순서와 SQE 배열 분리를 위한 간접 인덱스.
	 * 설정자: queue()가 io_u->index 저장. 읽는 자: 커널 제출 경로.
	 * 값 범위: 각 원소 0..iodepth-1. 동기화: tail store-release 전에 기록 완료. */
};

/*
 * CQ (Completion Queue) Ring 구조체
 *
 * io_uring의 완료 큐 링 버퍼를 유저스페이스에서 접근하기 위한 포인터 모음.
 *
 * [CQ Ring 동작 원리]
 * - 커널은 tail을 증가시키며 CQE를 추가 (완료 통지)
 * - 유저는 head를 증가시키며 CQE를 소비 (결과 수확)
 * - tail - head = 현재 수확 가능한 완료 이벤트 수
 *
 * libaio와의 차이: libaio는 io_getevents()로 이벤트를 커널에서 유저로 복사하지만,
 * io_uring은 CQE가 이미 공유 메모리에 있으므로 복사 비용이 없음.
 */
struct io_cq_ring {
	unsigned *head;
	/* [한국어] CQ head (유저가 수확한 지점). 설정자: getevents/event가 smp_store_release로 전진.
	 * 읽는 자: 커널이 CQ 가득 찼는지 판단. 값 범위: 0..UINT32_MAX. 동기화: 싱글 컨슈머. */

	unsigned *tail;
	/* [한국어] CQ tail (커널이 완료 기록한 지점). 설정자: 커널 release.
	 * 읽는 자: getevents가 acquire 로드 후 tail-head 만큼 수확. 값 범위: head <= tail <= head+cq_entries. */

	unsigned *ring_mask;
	/* [한국어] CQ 인덱스 마스크(=cq_entries-1). 설정자: 커널. 읽는 자: event()가 (head+i)&mask. */

	unsigned *ring_entries;
	/* [한국어] CQ 링 총 슬롯 수. 설정자: 커널. 동기화: 읽기 전용. */

	struct io_uring_cqe *cqes;
	/* [한국어] 실제 완료 이벤트(CQE) 배열 — user_data/res/flags 포함.
	 * 설정자: 커널이 tail 위치에 채움. 읽는 자: fio_ioring_event()가 io_u 복원과 결과 해석.
	 * 값 범위: 길이 = cq_entries. 동기화: tail 업데이트 후 유저가 acquire로 안전 읽기. */
};

/*
 * mmap 매핑 정보 구조체
 * io_uring 셋업 시 커널과 공유하기 위해 mmap()한 영역의 정보를 보관
 * cleanup 시 munmap()에 필요한 포인터와 크기를 저장
 */
struct ioring_mmap {
	void *ptr;
	/* [한국어] mmap()으로 매핑된 영역의 시작 가상 주소.
	 * 설정자: fio_ioring_mmap()이 mmap 반환값 저장. 읽는 자: fio_ioring_unmap()이 munmap에 전달.
	 * 값 범위: 유효 주소 또는 MAP_FAILED 후 초기화 실패 경로. 동기화: 초기화/종료 시점만 접근. */

	size_t len;
	/* [한국어] 매핑 영역 바이트 길이(munmap에 필요).
	 * 설정자: fio_ioring_mmap()이 params의 ring_size/sqe_off 기반으로 계산.
	 * 읽는 자: unmap 경로. 값 범위: 양수. 동기화: 초기화 후 불변. */
};

/*
 * io_uring 엔진의 핵심 데이터 구조체
 * 각 fio 스레드(thread_data)마다 하나씩 생성되며,
 * io_uring 인스턴스의 모든 상태를 보관
 */
struct ioring_data {
	int ring_fd;
	/* [한국어] io_uring 인스턴스의 파일 디스크립터 (io_uring_setup() syscall 반환값).
	 * 설정자: fio_ioring_queue_init()에서 syscall(__NR_io_uring_setup)의 반환값으로 저장.
	 * 읽는 자: io_uring_enter()/io_uring_register()/mmap() 호출 시 첫 인자로 사용, cleanup에서 close().
	 * 값 범위: 성공 시 >= 0 (일반 fd), 실패 시 -1 후 errno.
	 * 동기화: 잡 스레드 1개만 소유하지만 SQPOLL 커널 스레드와 공유 메모리로 연결되는 핵심 핸들. */

	struct io_u **io_u_index;
	/* [한국어] 인덱스 → io_u 포인터 역참조 테이블. SQE/CQE의 user_data 매핑 보조용.
	 * 설정자: fio_ioring_io_u_init()이 io_u->index 위치에 기록.
	 * 읽는 자: fio_ioring_cmd_prep() 등에서 io_u->index로 접근.
	 * 값 범위: 길이 = td->o.iodepth, 각 원소는 유효한 io_u 또는 NULL.
	 * 동기화: 잡 스레드 단독 사용 — 별도 락 불필요. */

	char *md_buf;
	/* [한국어] PI(Protection Information) 사용 시 io_u 당 메타데이터 버퍼의 베이스 주소.
	 * 설정자: post_init()에서 calloc/posix_memalign으로 md_per_io_size * iodepth 크기 할당.
	 * 읽는 자: fio_ioring_prep_md()가 io_u->mmap_data에 오프셋을 저장하여 사용.
	 * 값 범위: NULL(옵션 미사용) 또는 유효 포인터.
	 * 동기화: 잡 스레드가 단독 할당/읽기, 커널은 DMA 시 디바이스 주소로 접근. */

	char *pi_attr;
	/* [한국어] io_u 당 struct io_uring_attr_pi 속성 배열의 베이스.
	 * 설정자: post_init()에서 iodepth 만큼 연속 할당.
	 * 읽는 자: fio_ioring_prep_md()가 sqe->attr_ptr로 커널에 전달.
	 * 값 범위: NULL 또는 유효 포인터.
	 * 동기화: 잡 스레드가 준비하고 커널이 I/O 수행 중 읽기 — io_u 단위 배타적. */

	int *fds;
	/* [한국어] registerfiles 옵션 활성화 시 커널에 등록한 fd들의 로컬 사본 배열.
	 * 설정자: fio_ioring_register_files()가 td->o.nr_files 크기로 채움.
	 * 읽는 자: IORING_REGISTER_FILES_UPDATE / 닫기 경로.
	 * 값 범위: NULL(미사용) 또는 길이 nr_files 배열, 각 원소는 유효 fd.
	 * 동기화: 잡 스레드 단독 소유. */

	struct io_sq_ring sq_ring;
	/* [한국어] 커널과 mmap으로 공유하는 SQ 링의 유저스페이스 포인터 모음.
	 * 설정자: fio_ioring_mmap()이 io_uring_params의 오프셋을 기반으로 각 필드 매핑.
	 * 읽는 자: queue(tail 업데이트), commit(SQ_NEED_WAKEUP 확인), getevents.
	 * 값 범위: head/tail 등 유효 포인터. 접근 시 smp_load_acquire/smp_store_release 의미 필요.
	 * 동기화: 커널 SQPOLL 스레드와 공유 — 원자적 메모리 장벽으로 순서 보장. */

	struct io_uring_sqe *sqes;
	/* [한국어] 실제 I/O 요청 기술자(SQE) 배열. mmap[1]에 매핑된 커널 공유 영역.
	 * 설정자: fio_ioring_mmap()이 IORING_OFF_SQES로 매핑.
	 * 읽는 자: fio_ioring_prep()/fio_ioring_cmd_prep()가 io_u->index 위치에 요청 기록.
	 * 값 범위: 길이 = depth. 각 엔트리는 opcode/fd/addr/len/flags 등 포함.
	 * 동기화: 커널이 SQ tail 사이 SQE를 읽으므로 tail 기록 전 SQE 완성 필수. */

	struct iovec *iovecs;
	/* [한국어] 벡터 I/O(READV/WRITEV) 시 SQE.addr이 가리키는 iovec 배열 (io_u 인덱스 병렬).
	 * 설정자: fio_ioring_prep()가 iov_base/iov_len을 io_u->xfer_buf로 채움.
	 * 읽는 자: 커널 io_uring 코드가 I/O 수행 중 dereference.
	 * 값 범위: 길이 = iodepth. nonvectored=1이면 SQE에서 사용되지 않음.
	 * 동기화: io_u 단위 배타적 — SQE 커널 소비 중엔 변경 금지. */

	unsigned sq_ring_mask;
	/* [한국어] *sq_ring.ring_mask의 로컬 캐시 (매번 포인터 역참조 피함).
	 * 설정자: fio_ioring_mmap() 직후 저장.
	 * 읽는 자: queue()에서 tail & mask로 SQ 배열 인덱스 계산.
	 * 값 범위: (entries - 1), 2^n - 1 형태.
	 * 동기화: 읽기 전용 상수 취급. */

	struct io_cq_ring cq_ring;
	/* [한국어] 커널과 mmap 공유하는 CQ 링의 유저스페이스 포인터 모음.
	 * 설정자: fio_ioring_mmap()이 IORING_OFF_CQ_RING 기반으로 매핑.
	 * 읽는 자: fio_ioring_getevents()/event()가 head 증가시키며 CQE 수확.
	 * 값 범위: 유효 포인터. head/tail 접근은 메모리 장벽 의미 필요.
	 * 동기화: 커널(tail 기록) ↔ 유저(head 기록) 싱글 프로듀서/컨슈머. */

	unsigned cq_ring_mask;
	/* [한국어] *cq_ring.ring_mask의 로컬 캐시.
	 * 설정자: fio_ioring_mmap() 직후 저장. 읽는 자: getevents에서 head & mask.
	 * 값 범위: (cq_entries - 1). 동기화: 읽기 전용 상수. */

	int async_trim_fail;
	/* [한국어] TRIM 경로가 비동기 실패(-EOPNOTSUPP 등)를 반환한 적이 있는지 표시.
	 * 설정자: getevents 경로에서 TRIM 실패 감지 시 1로 설정.
	 * 읽는 자: queue/commit 흐름에서 동기 모드 폴백 결정에 사용.
	 * 값 범위: 0(정상) / 1(실패 감지). 동기화: 잡 스레드 단독. */

	int queued;
	/* [한국어] queue()로 SQ에 기록됐지만 아직 io_uring_enter()로 커널에 알리지 않은 SQE 수.
	 * 설정자: fio_ioring_queue()가 1씩 증가, commit이 0으로 리셋.
	 * 읽는 자: commit()이 enter()에 to_submit 인자로 전달.
	 * 값 범위: 0 <= queued <= iodepth. 동기화: 잡 스레드 단독. */

	int cq_ring_off;
	/* [한국어] getevents가 직전에 확인한 CQ head 스냅샷(수확 시작 오프셋).
	 * 설정자: fio_ioring_getevents()가 완료 대기 후 저장.
	 * 읽는 자: fio_ioring_event()가 이 오프셋부터 CQE 접근.
	 * 값 범위: 현재 CQ head 값 (0..cq_entries-1 이후에도 모듈로 동작).
	 * 동기화: 잡 스레드 단독. */

	unsigned iodepth;
	/* [한국어] io_uring 내부에서 사용하는 실제 큐 깊이 (2의 거듭제곱으로 올림 가능).
	 * 설정자: fio_ioring_queue_init()에서 roundup_pow2(td->o.iodepth).
	 * 읽는 자: 링 할당 크기 계산, 루프 상한. 값 범위: >= td->o.iodepth 최소 2^n.
	 * 동기화: 초기화 후 불변 — 읽기 전용. */

	int prepped;
	/* [한국어] force_async 주기 카운터. 매 SQE 준비 시 증가, force_async 도달 시 IOSQE_ASYNC 부여.
	 * 설정자/읽는 자: fio_ioring_prep()/cmd_prep()가 증감.
	 * 값 범위: 0..force_async. 동기화: 잡 스레드 단독. */

	struct ioring_mmap mmap[3];
	/* [한국어] mmap 매핑 3종 정보: [0]=SQ Ring, [1]=SQEs, [2]=CQ Ring.
	 * 설정자: fio_ioring_mmap()이 각 영역 크기/주소 기록.
	 * 읽는 자: fio_ioring_unmap()이 munmap 호출 시 참조.
	 * 값 범위: 각 ptr은 유효 주소 또는 MAP_FAILED 이후 에러 처리됨.
	 * 동기화: 초기화/종료 시점에만 접근. */

	struct cmdprio cmdprio;
	/* [한국어] cmdprio_percentage/bssplit 등 I/O 우선순위 정책 상태.
	 * 설정자: fio_ioring_cmdprio_init()가 옵션 기반으로 채움.
	 * 읽는 자: fio_ioring_cmdprio_prep()가 SQE의 ioprio를 재작성.
	 * 값 범위: 정책 비활성 시 0 초기화. 동기화: 잡 스레드 단독. */

	struct nvme_dsm *dsm;
	/* [한국어] NVMe Dataset Management(TRIM) 커맨드용 range 배열의 베이스.
	 * 설정자: fio_ioring_cmd_post_init()가 iodepth 만큼 할당.
	 * 읽는 자: fio_ioring_cmd_prep()가 TRIM 방향일 때 해당 엔트리를 SQE에 연결.
	 * 값 범위: NULL(미할당) 또는 유효 포인터.
	 * 동기화: io_u 단위 배타적 사용. */

	uint32_t cdw12_flags[DDIR_RWDIR_CNT];
	/* [한국어] NVMe Command Dword12용 플래그(FUA, PRCHK 등)를 방향별로 캐시.
	 * 설정자: fio_ioring_cmd_post_init()가 readfua/writefua/prchk로부터 계산.
	 * 읽는 자: fio_ioring_cmd_prep()가 ddir에 맞는 값을 NVMe 커맨드에 기록.
	 * 값 범위: 비트 OR 조합. 동기화: 초기화 후 불변. */

	uint8_t write_opcode;
	/* [한국어] write_mode 옵션에 따라 결정되는 NVMe 쓰기 opcode (Write/Write Uncor/Zeroes/Verify).
	 * 설정자: fio_ioring_cmd_post_init()가 옵션 파싱 결과로 저장.
	 * 읽는 자: fio_ioring_cmd_prep()가 방향이 WRITE일 때 사용.
	 * 값 범위: NVMe 스펙 opcode(0x01/0x04/0x08/0x0C 등). 동기화: 불변. */

	bool is_uring_cmd_eng;
	/* [한국어] 현재 엔진이 io_uring_cmd(NVMe passthrough)인지 일반 io_uring인지 구분.
	 * 설정자: fio_ioring_init()가 ioengine 이름으로 판단.
	 * 읽는 자: 공통 경로에서 분기(prep/queue/cleanup)하기 위해 검사.
	 * 값 범위: true/false. 동기화: 초기화 후 불변. */

	struct nvme_cmd_ext_io_opts ext_opts;
	/* [한국어] NVMe passthrough 시 PI(apptag/apptag_mask/io_flags 등)용 확장 옵션 묶음.
	 * 설정자: fio_ioring_cmd_post_init()가 pi_act/apptag/prchk 옵션으로부터 채움.
	 * 읽는 자: fio_ioring_cmd_prep()가 nvme_cmd_prep() 호출 시 전달.
	 * 값 범위: 비트 플래그 + 정수. 동기화: 초기화 후 불변. */
};

/*
 * io_uring 엔진 옵션 구조체
 * fio 설정 파일에서 사용자가 지정할 수 있는 io_uring 관련 옵션들
 */
struct ioring_options {
	struct thread_data *td;
	/* [한국어] 이 옵션 블록이 소속된 fio 잡(thread_data) 역참조 포인터.
	 * 설정자: fio 옵션 파싱 프레임워크가 엔진 옵션 구조 할당 시 자동 저장.
	 * 읽는 자: 옵션 콜백(fio_ioring_sqpoll_cb 등)이 td 기반 로깅/경고에 사용.
	 * 값 범위: 반드시 유효한 td. 동기화: td와 동일 수명, 잡 스레드가 단독 접근. */

	unsigned int hipri;
	/* [한국어] IORING_SETUP_IOPOLL(폴링 완료) 사용 여부. O_DIRECT + 블록 디바이스에서만 유효.
	 * 설정자: "hipri" 옵션 파서. 읽는 자: fio_ioring_queue_init()가 params.flags에 반영.
	 * 값 범위: 0/1. 동기화: 초기화 시점 이후 불변. */

	unsigned int readfua;
	/* [한국어] 읽기 시 FUA(Force Unit Access) — 디바이스 캐시 우회하여 미디어에서 읽기.
	 * 설정자: "readfua" 옵션. 읽는 자: uring_cmd 경로의 cdw12_flags 초기화.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int writefua;
	/* [한국어] 쓰기 시 FUA — 비휘발 매체 도달 전까지 완료로 보고하지 않도록 강제.
	 * 설정자: "writefua" 옵션. 읽는 자: cdw12_flags[WRITE]에 반영.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int deac;
	/* [한국어] Write Zeroes 커맨드의 DEAC(Deallocate) 비트 — LBA 할당 해제 힌트.
	 * 설정자: "deac" 옵션. 읽는 자: write_opcode가 ZEROES일 때 cdw12에 OR.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int write_mode;
	/* [한국어] 쓰기 방향에서 사용할 NVMe 커맨드 종류 선택 (enum uring_cmd_write_mode).
	 * 설정자: "write_mode" 옵션 문자열 → enum 변환.
	 * 읽는 자: fio_ioring_cmd_post_init()가 write_opcode 결정.
	 * 값 범위: WRITE(1)/UNCOR/ZEROES/VERIFY. 동기화: 불변. */

	unsigned int verify_mode;
	/* [한국어] verify 단계에서 사용할 NVMe 커맨드 (Read vs Compare).
	 * 설정자: "verify_mode" 옵션. 읽는 자: cmd_prep()가 READ/COMPARE opcode 결정.
	 * 값 범위: VMODE_READ(1)/VMODE_COMPARE. 동기화: 불변. */

	struct cmdprio_options cmdprio_options;
	/* [한국어] cmdprio_percentage/bssplit/class/level 등 우선순위 정책 원천 옵션.
	 * 설정자: fio 옵션 파서. 읽는 자: cmdprio_init()가 파싱 후 런타임 상태로 변환.
	 * 값 범위: 비활성 시 전 필드 0. 동기화: 불변. */

	unsigned int fixedbufs;
	/* [한국어] IORING_REGISTER_BUFFERS 사용 여부 — 버퍼 사전 등록으로 pin 비용 제거.
	 * 설정자: "fixedbufs" 옵션. 읽는 자: post_init()이 등록 수행, prep()이 opcode 선택.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int registerfiles;
	/* [한국어] IORING_REGISTER_FILES 사용 여부 — fd 사전 등록으로 lookup 비용 제거.
	 * 설정자: "registerfiles" 옵션. 읽는 자: open_file/prep에서 SQE fd/플래그 결정.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int sqpoll_thread;
	/* [한국어] IORING_SETUP_SQPOLL — 커널 스레드가 SQ를 폴링해 enter() 시스콜 최소화.
	 * 설정자: "sqthread_poll" 옵션. 읽는 자: queue_init 시 params.flags.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int sqpoll_set;
	/* [한국어] 사용자가 sqthread_poll_cpu를 명시적으로 지정했는지 플래그.
	 * 설정자: fio_ioring_sqpoll_cb() 콜백에서 1로 설정.
	 * 읽는 자: queue_init이 params.sq_thread_cpu 설정 여부 판단.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int sqpoll_cpu;
	/* [한국어] SQPOLL 커널 스레드를 핀(pin)할 CPU 번호.
	 * 설정자: sqpoll_cb. 읽는 자: queue_init의 params.sq_thread_cpu.
	 * 값 범위: 0..N_CPU-1. 동기화: 불변. */

	unsigned int nonvectored;
	/* [한국어] 비벡터 opcode(READ/WRITE) 선호 (-1=자동, 0=벡터, 1=비벡터).
	 * 설정자: "nonvectored" 옵션. 읽는 자: fio_ioring_prep()의 opcode 테이블 인덱스.
	 * 값 범위: -1/0/1. 동기화: init 후 자동값 해소되어 불변. */

	unsigned int uncached;
	/* [한국어] 버퍼드 I/O에서 RWF_DONTCACHE 적용 여부 — 페이지 캐시 오염 방지.
	 * 설정자: "uncached" 옵션. 읽는 자: prep()이 sqe->rw_flags에 OR.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int nowait;
	/* [한국어] RWF_NOWAIT 사용 — 블로킹 불가시 즉시 -EAGAIN 반환.
	 * 설정자: "nowait" 옵션. 읽는 자: prep()이 rw_flags에 OR.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int force_async;
	/* [한국어] N번째 SQE마다 IOSQE_ASYNC 플래그 강제 — 비동기 워커 경로 테스트용.
	 * 설정자: "force_async" 옵션. 읽는 자: prep()가 ld->prepped와 비교.
	 * 값 범위: 0(비활성) 또는 양의 정수. 동기화: 불변. */

	unsigned int md_per_io_size;
	/* [한국어] PI 메타데이터 버퍼 크기(바이트) — 0이면 메타데이터 경로 비활성.
	 * 설정자: "md_per_io_size" 옵션. 읽는 자: post_init()이 md_buf 크기 계산, prep_md() 경로.
	 * 값 범위: 0 또는 양수. 동기화: 불변. */

	unsigned int pi_act;
	/* [한국어] PI Action 비트 — 1이면 컨트롤러가 PI 생성/검증, 0이면 호스트 책임.
	 * 설정자: "pi_act" 옵션(기본 1). 읽는 자: ext_opts.io_flags에 반영.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int apptag;
	/* [한국어] PI의 Application Tag 값 (기본 0x1234).
	 * 설정자: "apptag" 옵션. 읽는 자: ext_opts에 저장되어 NVMe 커맨드에 삽입.
	 * 값 범위: 16비트 정수. 동기화: 불변. */

	unsigned int apptag_mask;
	/* [한국어] Application Tag 검사 마스크 (기본 0xffff=완전일치).
	 * 설정자: "apptag_mask" 옵션. 읽는 자: ext_opts.
	 * 값 범위: 16비트 마스크. 동기화: 불변. */

	unsigned int prchk;
	/* [한국어] pi_chk 문자열 파싱 결과를 비트로 저장(GUARD|REFTAG|APPTAG).
	 * 설정자: post_init()가 pi_chk 문자열 토큰화로 비트 구성.
	 * 읽는 자: cdw12_flags/ext_opts 구성 시 OR. 값 범위: 3비트 조합. 동기화: 불변. */

	char *pi_chk;
	/* [한국어] "GUARD,REFTAG,APPTAG" 형식의 원문자열 (파싱 전).
	 * 설정자: "pi_chk" 옵션 파서가 strdup. 읽는 자: post_init의 파싱 루프.
	 * 값 범위: NULL 또는 유효 heap 문자열. 동기화: 잡 수명 내 불변(해제는 종료시). */

	enum uring_cmd_type cmd_type;
	/* [한국어] io_uring_cmd로 전달할 프로토콜 커맨드 종류(현재 NVMe만).
	 * 설정자: "cmd_type" 옵션. 읽는 자: cmd_prep/cmd_post_init이 NVMe 분기.
	 * 값 범위: FIO_URING_CMD_NVME(1). 동기화: 불변. */
};

/*
 * io_uring_enter() 호출 시 사용할 기본 플래그
 * IORING_ENTER_GETEVENTS: 완료 이벤트를 기다림
 * (이후 IORING_FEAT_NO_IOWAIT가 지원되면 IORING_ENTER_NO_IOWAIT도 추가됨)
 */
/* [한국어] 전역 정적 변수 — 모든 잡 스레드가 공유하는 io_uring_enter 플래그.
 * 설정자: 첫 fio_ioring_queue_init 또는 fio_ioring_cmd_queue_init이 커널 응답
 *   params.features & IORING_FEAT_NO_IOWAIT 를 확인하고 있으면
 *   IORING_ENTER_NO_IOWAIT 를 OR로 추가. IORING_ENTER_GETEVENTS는 기본값 —
 *   enter() 호출 후 완료 대기(min_complete 인자) 시 필요.
 * 읽는 자: io_uring_enter() 헬퍼가 flags 인자로 그대로 전달.
 * 값 범위: IORING_ENTER_GETEVENTS | IORING_ENTER_NO_IOWAIT(조건부).
 * 동기화: 여러 잡 스레드가 동시에 fio_ioring_*_queue_init 를 수행할 수 있으나
 *   write-once 패턴이고 OR 연산이라 race 있어도 결과가 수렴(set-only). 관측되는
 *   값은 일관적 — 별도 락 불필요. */
static unsigned int enter_flags = IORING_ENTER_GETEVENTS;

/*
 * I/O 방향(읽기/쓰기)과 벡터/비벡터 여부에 따른 opcode 매핑 테이블
 * [ddir][nonvectored]: ddir=0(READ), ddir=1(WRITE)
 *                      nonvectored=0(벡터), nonvectored=1(비벡터)
 */
/* [한국어] 컴파일 타임 const 룩업 테이블 — fio_ioring_prep의 opcode 결정을
 * 분기 없이 2차원 인덱싱으로 해결. [ddir][!!nonvectored].
 * 읽는 자: fio_ioring_prep (uring_cmd 경로는 별도 NVMe opcode 사용).
 * 값 범위: IORING_OP_READV(1)/WRITEV(2)/READ(22)/WRITE(23) 고정. */
static const int ddir_to_op[2][2] = {
	{ IORING_OP_READV, IORING_OP_READ },
	/* [한국어] READ: [0][0]=READV(벡터 scatter-gather), [0][1]=READ(단일 buf).
	 * READV는 iovec 포인터+개수 필요, READ는 buf 포인터+길이만. */
	{ IORING_OP_WRITEV, IORING_OP_WRITE }
	/* [한국어] WRITE: [1][0]=WRITEV(gather), [1][1]=WRITE(단일 buf). */
};

/*
 * 고정 버퍼(fixedbufs) 사용 시 I/O 방향별 opcode 매핑
 * 미리 등록된 버퍼를 사용하므로 별도의 opcode 필요
 */
/* [한국어] IORING_REGISTER_BUFFERS 로 버퍼가 사전 등록된 경우의 opcode.
 * 차이점: READ_FIXED/WRITE_FIXED는 sqe->addr 를 "버퍼 내 주소", buf_index 를
 * "등록된 버퍼 인덱스"로 받아 커널이 pin/translate 비용 없이 DMA 주소를 재사용.
 * 값 범위: IORING_OP_READ_FIXED(4)/IORING_OP_WRITE_FIXED(5). */
static const int fixed_ddir_to_op[2] = {
	IORING_OP_READ_FIXED,
	/* [한국어] ddir=READ — 사전 등록 버퍼로 읽기. sqe->buf_index 필수. */
	IORING_OP_WRITE_FIXED
	/* [한국어] ddir=WRITE — 사전 등록 버퍼로 쓰기. sqe->buf_index 필수. */
};

/*
 * [한국어]
 * fio_ioring_sqpoll_cb - "sqthread_poll_cpu" 옵션 파싱 콜백
 *
 * @data: fio 옵션 파서가 전달하는 ioring_options 구조체 포인터 (void* 캐스팅됨).
 * @val: 사용자가 옵션 인자로 지정한 CPU 번호 (unsigned long long로 전달됨).
 * @return: 항상 0 (파싱 실패 없음).
 *
 * 배경: fio 옵션 프레임워크는 특수 처리가 필요한 옵션에 대해 .cb 콜백을 등록할
 * 수 있다. sqthread_poll_cpu는 "단순히 값을 저장하는 것 외에 사용자가 명시적으로
 * 값을 설정했는지 추적"해야 하므로 (queue_init이 IORING_SETUP_SQ_AFF 를 세팅할지
 * 결정) 별도 sqpoll_set 플래그와 함께 다루기 위해 콜백을 사용한다.
 *
 * 동작: (1) *val 을 o->sqpoll_cpu 에 저장. (2) o->sqpoll_set = 1 로 사용자가
 *   명시적으로 설정했음을 표시. 이후 queue_init 이 sqpoll_set 을 보고
 *   params.flags |= IORING_SETUP_SQ_AFF 와 params.sq_thread_cpu = sqpoll_cpu 를
 *   설정할지 결정.
 *
 * 실행 컨텍스트: 옵션 파싱 단계(fio 시작 시 parse_options 경로) — 잡 스레드가
 *   생성되기 전 메인 스레드. 재진입 불필요.
 *
 * 호출 체인:
 *   parse_options → fio_option_cb (options[]의 .cb=fio_ioring_sqpoll_cb) → [이 함수]
 */
static int fio_ioring_sqpoll_cb(void *data, unsigned long long *val)
{
	struct ioring_options *o = data;
	/* [한국어] void* 인자를 실제 옵션 구조체 타입으로 복원 — fio 프레임워크 규약. */

	o->sqpoll_cpu = *val;
	/* [한국어] 사용자가 지정한 CPU 번호(val은 long long로 전달되지만 queue_init은
	 * unsigned int로 내려보낸다 — CPU 번호가 31비트 이상일 일이 없어 안전). */

	o->sqpoll_set = 1;
	/* [한국어] 명시적 설정 마크 — 이후 queue_init의 "if (o->sqpoll_set)" 분기로
	 * IORING_SETUP_SQ_AFF 세팅 여부가 결정된다. 명시하지 않으면 커널이 자동 배치. */

	return 0;
	/* [한국어] 콜백은 성공(0)/실패(<0)로 보고. CPU 번호 유효성은 커널 측이 검증. */
}

/*
 * fio 옵션 정의 배열
 * io_uring 엔진에서 사용 가능한 모든 설정 옵션을 정의
 * 각 옵션은 fio 설정 파일이나 커맨드라인에서 사용 가능
 */
/* [한국어] === options[] 공통 규약 ===
 * - .name: fio CLI/잡파일에서 쓰는 문자열 키(예: "sqthread_poll").
 * - .lname: 사람이 읽는 긴 이름(--help / 리포트 출력).
 * - .type: FIO_OPT_STR_SET(불린 스위치, 존재만으로 1), FIO_OPT_BOOL(0/1),
 *   FIO_OPT_INT(정수), FIO_OPT_STR(열거형+posval), FIO_OPT_STR_STORE(문자열 저장).
 * - .off1: ioring_options 내 오프셋 — offsetof로 지정해 프레임워크가 해당 필드에
 *   파싱 결과를 자동 저장.
 * - .def: 기본값 문자열(타입에 맞게 파싱됨).
 * - .help: --help 설명.
 * - .posval[]: FIO_OPT_STR 열거값 — {.ival 문자열, .oval 정수, .help 설명}.
 * - .cb: 특수 파싱 필요 시 콜백(값 저장 외 사이드이펙트). 본 파일에서는
 *   sqthread_poll_cpu만 사용.
 * - .category=FIO_OPT_C_ENGINE: "I/O 엔진" 카테고리. 다른 예: _C_IO / _C_GENERAL.
 * - .group=FIO_OPT_G_IOURING: io_uring 전용 그룹. 공유 옵션(CMDPRIO_OPTIONS 매크로)
 *   도 호출측 그룹(_G_IOURING)에 묶여 확장됨.
 * 마지막 원소는 .name=NULL sentinel — 프레임워크가 배열 끝을 이 값으로 판별. */
static struct fio_option options[] = {
	{
		/* [한국어] hipri: IORING_SETUP_IOPOLL 활성화. 커널이 디바이스를 폴링해
		 * 인터럽트 없이 완료 확인(block-layer polling). O_DIRECT + 블록 디바이스
		 * + 드라이버가 blk_mq poll 지원 시만 유효. libaio의 hipri(IOCB_FLAG_HIPRI)와 동치. */
		.name	= "hipri",
		/* [한국어] CLI/잡파일에서 "hipri" 토큰 하나로 활성화(FIO_OPT_STR_SET). */
		.lname	= "High Priority",
		/* [한국어] 리포트/도움말에 표시되는 긴 이름. */
		.type	= FIO_OPT_STR_SET,
		/* [한국어] 값 없는 스위치 — 존재=1, 없음=0. */
		.off1	= offsetof(struct ioring_options, hipri),
		/* [한국어] 파싱 결과가 저장될 필드 오프셋. */
		.help	= "Use polled IO completions",
		.category = FIO_OPT_C_ENGINE,
		/* [한국어] "I/O 엔진" 카테고리로 분류 — --help-group 출력 분할. */
		.group	= FIO_OPT_G_IOURING,
		/* [한국어] io_uring 전용 그룹 — 같은 그룹은 잡파일 파서가 묶어 표시. */
	},
	{
		/* [한국어] readfua: NVMe Read 시 CDW12 bit30(FUA) — 디바이스 캐시를 우회하고
		 * 미디어에서 직접 읽도록 강제. uring_cmd 경로에서만 의미(일반 io_uring 경로는
		 * 블록 레이어의 RWF_* 플래그를 사용). */
		.name	= "readfua",
		/* [한국어] CLI/잡파일 키. */
		.lname	= "Read fua flag support",
		/* [한국어] 리포트용 긴 이름. */
		.type	= FIO_OPT_BOOL,
		/* [한국어] 0/1 불린. */
		.off1	= offsetof(struct ioring_options, readfua),
		/* [한국어] 저장 위치. */
		.help	= "Set FUA flag (force unit access) for all Read operations",
		.def	= "0",
		/* [한국어] 기본 비활성 — 성능에 악영향 주므로. */
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] writefua: NVMe Write 시 CDW12 bit30(FUA) — 데이터가 비휘발성
		 * 매체에 도달할 때까지 완료 보고를 지연시켜 강한 내구성 보장. 동기화 테스트에 사용. */
		.name	= "writefua",
		.lname	= "Write fua flag support",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, writefua),
		.help	= "Set FUA flag (force unit access) for all Write operations",
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] write_mode: uring_cmd 경로에서 "ddir==WRITE일 때 실제로 어떤 NVMe
		 * opcode를 발행할지" 선택. 기본 write(0x01) 외 uncor/zeroes/verify 3종.
		 * 설정자: 옵션 파서. 읽는 자: fio_ioring_cmd_init → ld->write_opcode. */
		.name	= "write_mode",
		.lname	= "Additional Write commands support (Write Uncorrectable, Write Zeores)",
		.type	= FIO_OPT_STR,
		/* [한국어] 열거형 — posval[] 에서 문자열→정수 매핑. */
		.off1	= offsetof(struct ioring_options, write_mode),
		.help	= "Issue Write Uncorrectable or Zeroes command instead of Write command",
		.def	= "write",
		/* [한국어] 기본값 문자열 — posval의 ival과 매칭되어 oval이 저장. */
		.posval = {
			  { .ival = "write",
			    .oval = FIO_URING_CMD_WMODE_WRITE,
			    .help = "Issue Write commands for write operations"
			    /* [한국어] 일반 Write(0x01) — 기본. */
			  },
			  { .ival = "uncor",
			    .oval = FIO_URING_CMD_WMODE_UNCOR,
			    .help = "Issue Write Uncorrectable commands for write operations"
			    /* [한국어] Write Uncorrectable(0x04) — LBA를 "읽기 시 에러" 상태로. */
			  },
			  { .ival = "zeroes",
			    .oval = FIO_URING_CMD_WMODE_ZEROES,
			    .help = "Issue Write Zeroes commands for write operations"
			    /* [한국어] Write Zeroes(0x08) — 데이터 전송 없이 0으로 채움. deac=1 시 DEAC. */
			  },
			  { .ival = "verify",
			    .oval = FIO_URING_CMD_WMODE_VERIFY,
			    .help = "Issue Verify commands for write operations"
			    /* [한국어] Verify(0x0C) — 디바이스가 읽어 내부 검증만 수행. */
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] verify_mode: fio의 verify 단계에서 사용할 NVMe 커맨드 선택.
		 * read=호스트로 데이터 가져와 비교, compare=디바이스에서 비교만 수행. */
		.name	= "verify_mode",
		.lname	= "Do verify based on the configured command (e.g., Read or Compare command)",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct ioring_options, verify_mode),
		.help	= "Issue Read or Compare command in the verification phase",
		.def	= "read",
		.posval = {
			  { .ival = "read",
			    .oval = FIO_URING_CMD_VMODE_READ,
			    .help = "Issue Read commands in the verification phase"
			    /* [한국어] Read + 호스트측 memcmp — 대역폭 소모 있지만 범용. */
			  },
			  { .ival = "compare",
			    .oval = FIO_URING_CMD_VMODE_COMPARE,
			    .help = "Issue Compare commands in the verification phase"
			    /* [한국어] NVMe Compare(0x05) — 디바이스 측 비교, 대역폭 절감. */
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] fixedbufs: IORING_REGISTER_BUFFERS 활성화. 초기화 시 iovecs 배열
		 * 을 커널에 한 번 등록 — 이후 I/O는 OP_READ_FIXED/WRITE_FIXED로 pin/translate
		 * 비용 제거. libaio는 매 I/O마다 pin/unpin 필요(RLIMIT_MEMLOCK 영향). */
		.name	= "fixedbufs",
		.lname	= "Fixed (pre-mapped) IO buffers",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, fixedbufs),
		.help	= "Pre map IO buffers",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] registerfiles: IORING_REGISTER_FILES 활성화. nr_files 전체를
		 * 커널에 등록, SQE는 fd 대신 인덱스 + IOSQE_FIXED_FILE 플래그로 발행 →
		 * 매 I/O마다 fd→struct file lookup과 refcount atomic 제거. SQPOLL은 이
		 * 옵션이 필수(커널 스레드가 유저 fd 테이블에 접근 불가). */
		.name	= "registerfiles",
		.lname	= "Register file set",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, registerfiles),
		.help	= "Pre-open/register files",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] sqthread_poll: IORING_SETUP_SQPOLL 활성화. 커널이 SQ thread를
		 * 만들어 SQ 링을 지속 폴링 — 유저 측은 tail 쓰기만으로 제출, 시스콜 제거.
		 * 유휴가 길면 커널 스레드가 sleep하고 SQ flags 에 IORING_SQ_NEED_WAKEUP 를
		 * 세팅 → commit에서 이 비트가 있을 때만 enter(SQ_WAKEUP) 호출. */
		.name	= "sqthread_poll",
		.lname	= "Kernel SQ thread polling",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct ioring_options, sqpoll_thread),
		.help	= "Offload submission/completion to kernel thread",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] sqthread_poll_cpu: SQPOLL 커널 스레드를 핀할 CPU. fio_ioring_sqpoll_cb
		 * 콜백이 o->sqpoll_cpu/sqpoll_set 양쪽을 설정 → queue_init에서
		 * IORING_SETUP_SQ_AFF + params.sq_thread_cpu 지정. 미지정 시 커널 자동. */
		.name	= "sqthread_poll_cpu",
		.lname	= "SQ Thread Poll CPU",
		.type	= FIO_OPT_INT,
		.cb	= fio_ioring_sqpoll_cb,
		/* [한국어] 콜백 — 단순 저장 이상(sqpoll_set 플래그 동반)이 필요해 콜백 사용. */
		.help	= "What CPU to run SQ thread polling on",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] nonvectored: 일반 경로의 opcode 선택.
		 *  -1: 자동 — fio_ioring_probe가 REGISTER_PROBE로 커널 지원 확인 후 1로 승격.
		 *   0: 강제 벡터(READV/WRITEV).
		 *   1: 강제 비벡터(READ/WRITE) — iovec 오버헤드 제거, 단일 buf 경로. */
		.name	= "nonvectored",
		.lname	= "Non-vectored",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, nonvectored),
		.def	= "-1",
		/* [한국어] 기본 -1 — 런타임 probe로 자동 결정. */
		.help	= "Use non-vectored read/write commands",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] uncached: 버퍼드 I/O에서 RWF_DONTCACHE 플래그 사용. 페이지 캐시
		 * 에 데이터가 남지 않게 해 캐시 오염 방지(벤치마크 정확성). odirect이면
		 * 이미 캐시 우회이므로 이 옵션은 "buffered I/O + cache bypass" 조합용. */
		.name	= "uncached",
		.lname	= "Uncached",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, uncached),
		.help	= "Use RWF_DONTCACHE for buffered read/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] nowait: RWF_NOWAIT — 블로킹 필요 시 즉시 -EAGAIN 반환. 캐시 미스
		 * 등 디바이스 I/O가 필요한 경우 요청을 포기하는 "빠른 실패" 시맨틱. */
		.name	= "nowait",
		.lname	= "RWF_NOWAIT",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, nowait),
		.help	= "Use RWF_NOWAIT for reads/writes",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] force_async: N번째마다 IOSQE_ASYNC를 강제 — 커널 io_uring이
		 * 인라인 실행 가능하더라도 비동기 워커(io-wq)로 라우팅. 비동기 경로
		 * 테스트/디버깅 용도(정상 성능은 인라인이 유리). prepped 카운터로 순환. */
		.name	= "force_async",
		.lname	= "Force async",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, force_async),
		.help	= "Set IOSQE_ASYNC every N requests",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] cmd_type: io_uring_cmd 엔진에서만 의미 — SQE에 어떤 프로토콜
		 * 커맨드를 패킹할지. 현재 "nvme" 하나만(FIO_URING_CMD_NVME). */
		.name	= "cmd_type",
		.lname	= "Uring cmd type",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct ioring_options, cmd_type),
		.help	= "Specify uring-cmd type",
		.def	= "nvme",
		.posval = {
			  { .ival = "nvme",
			    .oval = FIO_URING_CMD_NVME,
			    .help = "Issue nvme-uring-cmd",
			    /* [한국어] NVMe 패스스루 — IORING_OP_URING_CMD + nvme_uring_cmd */
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	/* [한국어] cmdprio 공유 옵션 — 이 매크로는 여러 옵션 엔트리를
	 * (cmdprio_percentage, cmdprio_bssplit, cmdprio_class, cmdprio_hint, ...)
	 * 한꺼번에 확장한다. 두 번째 인자는 그룹(FIO_OPT_G_IOURING)으로 카테고리
	 * 설정 — libaio/sg 등 다른 엔진과 공유되는 공통 정책. */
	CMDPRIO_OPTIONS(struct ioring_options, FIO_OPT_G_IOURING),
	{
		/* [한국어] md_per_io_size: PI 사용 시 I/O당 메타데이터 버퍼 크기.
		 * 0(기본)=PI 미사용. >0이면 fio_ioring_init이 md_buf 할당, post_init에서
		 * io_u->mmap_data=슬라이스, prep에서 pi_attr 채움. uring_cmd=iomem 제약 있음. */
		.name	= "md_per_io_size",
		.lname	= "Separate Metadata Buffer Size per I/O",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, md_per_io_size),
		.def	= "0",
		.help	= "Size of separate metadata buffer per I/O (Default: 0)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] pi_act: NVMe PRINFO bit3 (Protection Information Action).
		 * 1=컨트롤러가 호스트 대신 PI 생성/검증(호스트 부담 없음, 기본).
		 * 0=호스트가 직접 가드/REFTAG 생성 및 검증(fio_nvme_generate_guard /
		 * fio_nvme_pi_verify 사용). */
		.name	= "pi_act",
		.lname	= "Protection Information Action",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, pi_act),
		.def	= "1",
		/* [한국어] 기본 1 — 컨트롤러 처리 권장. */
		.help	= "Protection Information Action bit (pi_act=1 or pi_act=0)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] pi_chk: 어떤 PI 필드를 검사할지 콤마 구분 문자열.
		 * "GUARD"/"REFTAG"/"APPTAG" 토큰 조합 — parse_prchk_flags가 strstr로
		 * 토큰을 찾아 NVME_IO_PRINFO_PRCHK_* 비트로 변환(o->prchk). */
		.name	= "pi_chk",
		.lname	= "Protection Information Check",
		.type	= FIO_OPT_STR_STORE,
		/* [한국어] 문자열 저장 — 포인터 필드에 strdup된 값 저장, 파싱은 post_init. */
		.off1	= offsetof(struct ioring_options, pi_chk),
		.def	= NULL,
		/* [한국어] 기본 NULL — 검사 비활성. */
		.help	= "Control of Protection Information Checking (pi_chk=GUARD,REFTAG,APPTAG)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] apptag: PI Application Tag 16비트 값. 기본 0x1234 — 임의 값(검증의
		 * 일관성만 유지되면 됨). */
		.name	= "apptag",
		.lname	= "Application Tag used in Protection Information",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, apptag),
		.def	= "0x1234",
		.help	= "Application Tag used in Protection Information field (Default: 0x1234)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] apptag_mask: Application Tag 검증 마스크. 디바이스가
		 * (device_apptag & mask) == (host_apptag & mask) 검사.
		 * 기본 0xffff=완전일치. 일반 io_uring 경로에서는 0xffff 외 값 금지. */
		.name	= "apptag_mask",
		.lname	= "Application Tag Mask",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct ioring_options, apptag_mask),
		.def	= "0xffff",
		.help	= "Application Tag Mask used with Application Tag (Default: 0xffff)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		/* [한국어] deac: Write Zeroes(0x08)의 CDW12 bit25 DEAC(Deallocate) — 해당
		 * LBA 범위를 할당 해제(TRIM 유사 힌트). write_mode=zeroes 조합 시만 의미. */
		.name	= "deac",
		.lname	= "Deallocate bit for write zeroes command",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct ioring_options, deac),
		.help	= "Set DEAC (deallocate) flag for write zeroes command",
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_IOURING,
	},
	{
		.name	= NULL,
		/* [한국어] sentinel — 옵션 배열 종료 표시(fio 파서가 NULL name을 끝 신호로 인식). */
	},
};

/*
 * [한국어]
 * io_uring_enter - io_uring_enter(2) 시스콜의 얇은 래퍼
 *
 * @ld: ioring_data. ld->ring_fd 를 첫 인자로 사용.
 * @to_submit: 제출할 SQE 수(커널이 SQ에서 읽어갈 개수). 0이면 제출 없음.
 * @min_complete: 최소 대기할 완료 수. 0이면 기다리지 않음(non-blocking pop).
 * @flags: 동작 플래그(IORING_ENTER_GETEVENTS/SQ_WAKEUP/NO_IOWAIT 등).
 * @return: 성공 시 제출된 SQE 수(>=0), 실패 시 -errno.
 *
 * 배경: io_uring의 3개 주요 시스콜 중 가장 빈번한 호출. libaio의 io_submit(2) +
 * io_getevents(2) 두 콜의 역할을 하나로 합침. SQPOLL 모드에서는 커널 스레드가 SQ
 * 를 폴링하므로 대부분 호출하지 않지만, 유휴 sleep 시 NEED_WAKEUP 비트가 켜지면
 * SQ_WAKEUP 플래그로 깨운다.
 *
 * 동작: FIO_ARCH_HAS_SYSCALL 매크로가 정의된 아키텍처(x86_64/arm64 등 fio가
 * 직접 syscall 스텁을 제공하는 경우)는 __do_syscall6으로 glibc 우회 — 약 10-20ns
 * 절약. 그 외에는 glibc syscall(3) 래퍼 사용. 두 경로 모두 결과는 동일(syscall
 * 번호 __NR_io_uring_enter=426).
 *
 * 실행 컨텍스트: 잡 스레드. fio_ioring_commit/_getevents에서 호출.
 * 재진입성: 동일 ring_fd에 대해 여러 스레드가 동시에 호출 가능하지만(SINGLE_ISSUER
 *   플래그 없으면), 본 파일은 잡 스레드 1개가 단독 사용.
 *
 * 호출 체인:
 *   fio_ioring_commit / fio_ioring_getevents → [io_uring_enter] → __NR_io_uring_enter(커널)
 */
static int io_uring_enter(struct ioring_data *ld, unsigned int to_submit,
			 unsigned int min_complete, unsigned int flags)
{
#ifdef FIO_ARCH_HAS_SYSCALL
	/* [한국어] 아키텍처별 직접 syscall 스텁 — glibc vDSO/errno 처리 없이 커널 진입.
	 * __do_syscall6: 6인자 syscall 어셈블리 래퍼. 마지막 2인자는 sigset_t*/sigsetsize(미사용 시 NULL/0). */
	return __do_syscall6(__NR_io_uring_enter, ld->ring_fd, to_submit,
				min_complete, flags, NULL, 0);
#else
	/* [한국어] 표준 glibc syscall(3) 래퍼 — 이식성 안전. errno 자동 설정. */
	return syscall(__NR_io_uring_enter, ld->ring_fd, to_submit,
			min_complete, flags, NULL, 0);
#endif
}

/* [한국어] 블록 디바이스 io_uring_cmd 의 DISCARD(TRIM) 커맨드 번호.
 * _IO(type=0x12, nr=0) — type 0x12는 BLOCK_IOCTL 그룹(<linux/fs.h>).
 * 시스템 헤더에 정의되지 않은 구버전 빌드를 위해 fallback 정의.
 * fio_ioring_prep의 DDIR_TRIM 분기에서 sqe->cmd_op로 사용. */
#ifndef BLOCK_URING_CMD_DISCARD
#define BLOCK_URING_CMD_DISCARD	_IO(0x12, 0)
#endif

/*
 * [한국어]
 * fio_ioring_prep_md - 일반 io_uring 경로 SQE에 PI(메타데이터) 속성 세팅
 *
 * @td: 잡 컨텍스트 — td->io_ops_data = ioring_data, td->eo = ioring_options.
 * @io_u: 대상 I/O 유닛(SQE index, offset, xfer_buflen 소유).
 * @return: 없음(void). 실패 없이 항상 채움(SQE는 미리 memset 되어있음).
 *
 * 배경: NVMe passthru 경로가 아닌 일반 io_uring(READ/WRITE/READV/WRITEV) 에서도
 *   커널 io_uring이 "별도 메타데이터 버퍼 + 가드/REFTAG/APPTAG 검사"를 지원하도록
 *   최근 UAPI에 io_uring_attr_pi가 추가되었다. SQE의 attr_type_mask 에
 *   IORING_RW_ATTR_FLAG_PI 비트를 세팅하고 attr_ptr 에 사전 할당된 pi_attr 배열
 *   슬라이스를 연결하면 커널이 I/O 처리 중 PI 필드를 함께 DMA한다.
 *
 * 동작 단계:
 *   (1) io_u->index 위치의 SQE를 얻고,
 *   (2) SQE의 attr_type_mask/attr_ptr 를 채우고,
 *   (3) pi_attr.addr 에 io_u->mmap_data(=md_buf 슬라이스) 주소 기록,
 *   (4) REFTAG 검사 활성화면 시작 LBA를 seed 로 저장(커널이 이 값부터 증가 검증).
 *
 * 실행 컨텍스트: 잡 스레드(queue 콜백 내부 호출). 재진입 불필요.
 *
 * 호출 체인:
 *   fio_ioring_queue → (일반 경로 md_per_io_size 세트 조건) →
 *   fio_ioring_setup_pi → [fio_ioring_prep_md] (실제로는 fio_ioring_prep 내부
 *   의 o->md_per_io_size 분기에서 직접 호출)
 */
static void fio_ioring_prep_md(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;        /* io_uring 엔진 데이터 */
	struct io_uring_attr_pi *pi_attr = io_u->pi_attr; /* 해당 io_u의 PI 속성 */
	struct nvme_data *data = FILE_ENG_DATA(io_u->file); /* 파일의 NVMe 데이터 */
	struct io_uring_sqe *sqe;

	/* 해당 io_u 인덱스에 대응하는 SQE를 가져옴 */
	sqe = &ld->sqes[io_u->index];

	/* SQE에 PI 속성 타입 마스크와 포인터를 설정 */
	sqe->attr_type_mask = IORING_RW_ATTR_FLAG_PI;
	sqe->attr_ptr = (__u64)(uintptr_t)pi_attr;
	/* 메타데이터 버퍼 주소 설정 (mmap_data는 io_u_init에서 할당된 메타데이터 영역) */
	pi_attr->addr = (__u64)(uintptr_t)io_u->mmap_data;

	/* 참조 태그(REFTAG) 검사가 활성화된 경우, seed를 시작 LBA로 설정 */
	if (pi_attr->flags & IO_INTEGRITY_CHK_REFTAG) {
		__u64 slba = get_slba(data, io_u->offset);  /* 오프셋을 시작 LBA로 변환 */
		pi_attr->seed = (__u32)slba;
	}
}

/*
 * [함수] fio_ioring_prep
 * [역할] 일반 io_uring 엔진(비 NVMe passthrough)에서 I/O 요청을 위한 SQE를 준비.
 *        fio의 io_u를 io_uring의 SQE로 변환하는 핵심 함수.
 *
 * [I/O 흐름에서의 위치]
 * fio core에서 io_u를 할당 → fio_ioring_prep()으로 SQE 준비
 * → fio_ioring_queue()로 SQ Ring에 추가 → fio_ioring_commit()으로 커널에 제출
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 준비할 I/O 유닛
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_prep(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;   /* io_uring 엔진 데이터 */
	struct ioring_options *o = td->eo;           /* 엔진 옵션 */
	struct fio_file *f = io_u->file;             /* 대상 파일 */
	struct io_uring_sqe *sqe;

	/* io_u 인덱스에 해당하는 SQE를 가져옴 */
	sqe = &ld->sqes[io_u->index];

	/* 등록된 파일 사용 여부에 따라 fd 설정 방식이 달라짐 */
	if (o->registerfiles) {
		sqe->fd = f->engine_pos;            /* 등록된 파일 인덱스 사용 */
		sqe->flags = IOSQE_FIXED_FILE;     /* 등록된 파일임을 표시 */
	} else {
		sqe->fd = f->fd;                    /* 일반 파일 디스크립터 사용 */
		sqe->flags = 0;
	}

	/* 읽기 또는 쓰기 I/O인 경우 */
	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {
		if (o->fixedbufs) {
			/* 고정(사전 등록) 버퍼 사용 경로
			 * 커널에 미리 등록된 버퍼를 사용하므로 pin/unpin 오버헤드 없음 */
			sqe->opcode = fixed_ddir_to_op[io_u->ddir]; /* READ_FIXED or WRITE_FIXED */
			sqe->addr = (unsigned long) io_u->xfer_buf;  /* 버퍼 주소 */
			sqe->len = io_u->xfer_buflen;                /* 전송 크기 */
			sqe->buf_index = io_u->index;                /* 등록된 버퍼의 인덱스 */
		} else {
			/* 일반 버퍼 사용 경로 */
			struct iovec *iov = &ld->iovecs[io_u->index];

			/*
			 * Update based on actual io_u, requeue could have
			 * adjusted these
			 */
			/* 실제 io_u 기반으로 업데이트 - 재큐잉 시 조정될 수 있음 */
			iov->iov_base = io_u->xfer_buf;      /* 전송 버퍼 주소 */
			iov->iov_len = io_u->xfer_buflen;    /* 전송 버퍼 크기 */

			/* 벡터/비벡터 여부에 따라 opcode 선택 */
			sqe->opcode = ddir_to_op[io_u->ddir][!!o->nonvectored];
			if (o->nonvectored) {
				/* 비벡터: 직접 주소/크기 전달 (iovec 오버헤드 없음) */
				sqe->addr = (unsigned long) iov->iov_base;
				sqe->len = iov->iov_len;
			} else {
				/* 벡터: iovec 포인터 전달 (scatter/gather I/O 가능) */
				sqe->addr = (unsigned long) iov;
				sqe->len = 1;  /* iovec 개수: 1개 */
			}
		}
		/* 메타데이터가 있으면 PI 속성 설정 */
		if (o->md_per_io_size)
			fio_ioring_prep_md(td, io_u);
		/* 읽기/쓰기 플래그 초기화 */
		sqe->rw_flags = 0;
		/* 버퍼드 I/O에서 캐시 미사용(RWF_DONTCACHE) 설정 */
		if (!td->o.odirect && o->uncached)
			sqe->rw_flags |= RWF_DONTCACHE;
		/* 논블로킹 I/O(RWF_NOWAIT) 설정 */
		if (o->nowait)
			sqe->rw_flags |= RWF_NOWAIT;
		/* 원자적 쓰기(RWF_ATOMIC) 설정 - 쓰기 방향일 때만 */
		if (td->o.oatomic && io_u->ddir == DDIR_WRITE)
			sqe->rw_flags |= RWF_ATOMIC;

		/*
		 * Since io_uring can have a submission context (sqthread_poll)
		 * that is different from the process context, we cannot rely on
		 * the IO priority set by ioprio_set() (options prio, prioclass,
		 * and priohint) to be inherited.
		 * td->ioprio will have the value of the "default prio", so set
		 * this unconditionally. This value might get overridden by
		 * fio_ioring_cmdprio_prep() if the option cmdprio_percentage or
		 * cmdprio_bssplit is used.
		 */
		/*
		 * io_uring은 제출 컨텍스트(SQPOLL 스레드)가 프로세스 컨텍스트와
		 * 다를 수 있으므로, ioprio_set()으로 설정한 I/O 우선순위가
		 * 상속되지 않을 수 있음.
		 * 따라서 SQE에 I/O 우선순위를 무조건 명시적으로 설정함.
		 * cmdprio_percentage/cmdprio_bssplit 옵션 사용 시
		 * fio_ioring_cmdprio_prep()에서 이 값이 재정의될 수 있음.
		 */
		sqe->ioprio = td->ioprio;
		sqe->off = io_u->offset;     /* 파일 내 오프셋 */
	} else if (ddir_sync(io_u->ddir)) {
		/* 동기화(sync) 계열 I/O인 경우 */
		sqe->ioprio = 0;
		if (io_u->ddir == DDIR_SYNC_FILE_RANGE) {
			/* sync_file_range: 특정 파일 범위만 동기화 */
			sqe->off = f->first_write;                  /* 동기화 시작 오프셋 */
			sqe->len = f->last_write - f->first_write;  /* 동기화 범위 길이 */
			sqe->sync_range_flags = td->o.sync_file_range; /* 동기화 플래그 */
			sqe->opcode = IORING_OP_SYNC_FILE_RANGE;
		} else {
			/* fsync / fdatasync */
			sqe->off = 0;
			sqe->addr = 0;
			sqe->len = 0;
			if (io_u->ddir == DDIR_DATASYNC)
				sqe->fsync_flags |= IORING_FSYNC_DATASYNC; /* fdatasync: 메타데이터 제외 */
			sqe->opcode = IORING_OP_FSYNC;
		}
	} else if (io_u->ddir == DDIR_TRIM) {
		/* TRIM(Discard) 요청 - 블록 디바이스의 io_uring_cmd DISCARD 사용 */
		sqe->opcode = IORING_OP_URING_CMD;
		sqe->addr = io_u->offset;           /* TRIM 시작 오프셋 */
		sqe->addr3 = io_u->xfer_buflen;     /* TRIM 길이 */
		sqe->rw_flags = 0;
		sqe->len = sqe->off = 0;
		sqe->ioprio = 0;
		sqe->cmd_op = BLOCK_URING_CMD_DISCARD;  /* 블록 DISCARD 커맨드 */
		sqe->__pad1 = 0;
		sqe->file_index = 0;
	}

	/* force_async: N번째 요청마다 IOSQE_ASYNC 플래그를 설정하여 비동기 처리 강제 */
	if (o->force_async && ++ld->prepped == o->force_async) {
		ld->prepped = 0;              /* 카운터 리셋 */
		sqe->flags |= IOSQE_ASYNC;   /* 이 SQE는 반드시 비동기 워커에서 처리 */
	}

	/* SQE의 user_data에 io_u 포인터를 저장
	 * → CQE에서 이 값으로 어떤 I/O가 완료되었는지 식별 */
	sqe->user_data = (unsigned long) io_u;
	return 0;
}

/*
 * [함수] fio_ioring_cmd_prep
 * [역할] io_uring_cmd 엔진(NVMe passthrough)에서 I/O 요청의 SQE를 준비.
 *        NVMe 커맨드를 SQE의 cmd 필드에 직접 채워넣음.
 *
 * io_uring_cmd는 io_uring을 통해 디바이스별 커맨드를 직접 전달하는 메커니즘.
 * NVMe passthrough에서는 NVMe I/O 커맨드(Read, Write, Compare 등)를
 * 커널의 블록 레이어를 우회하여 NVMe 드라이버에 직접 전달함.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 준비할 I/O 유닛
 * [반환값] 0 (성공), -EINVAL (지원하지 않는 cmd_type)
 */
static int fio_ioring_cmd_prep(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct fio_file *f = io_u->file;
	struct nvme_uring_cmd *cmd;        /* NVMe uring 커맨드 구조체 */
	struct io_uring_sqe *sqe;
	struct nvme_dsm *dsm;              /* NVMe Dataset Management 구조체 (TRIM용) */
	void *ptr = ld->dsm;
	unsigned int dsm_size;
	uint8_t read_opcode = nvme_cmd_read;  /* 기본 읽기 opcode */

	/* only supports nvme_uring_cmd */
	/* NVMe uring_cmd만 지원 - 다른 유형이면 에러 반환 */
	if (o->cmd_type != FIO_URING_CMD_NVME)
		return -EINVAL;

	/* TRIM 요청이 동기 TRIM 모드인 경우, prep 단계를 건너뜀 (동기 처리됨) */
	if (io_u->ddir == DDIR_TRIM && td->io_ops->flags & FIO_ASYNCIO_SYNC_TRIM)
		return 0;

	/* NVMe는 128바이트 SQE 사용 (IORING_SETUP_SQE128) → 인덱스를 2배로 */
	sqe = &ld->sqes[(io_u->index) << 1];

	/* 등록된 파일 사용 여부에 따라 fd 설정 */
	if (o->registerfiles) {
		sqe->fd = f->engine_pos;
		sqe->flags = IOSQE_FIXED_FILE;
	} else {
		sqe->fd = f->fd;
	}
	sqe->rw_flags = 0;
	/* 캐시 미사용 플래그 */
	if (!td->o.odirect && o->uncached)
		sqe->rw_flags |= RWF_DONTCACHE;
	/* 논블로킹 플래그 */
	if (o->nowait)
		sqe->rw_flags |= RWF_NOWAIT;

	/* io_uring_cmd opcode 설정 */
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->user_data = (unsigned long) io_u;  /* 완료 시 io_u 식별용 */
	/* 벡터/비벡터에 따라 NVMe uring cmd 유형 선택 */
	if (o->nonvectored)
		sqe->cmd_op = NVME_URING_CMD_IO;       /* 비벡터 NVMe I/O */
	else
		sqe->cmd_op = NVME_URING_CMD_IO_VEC;   /* 벡터 NVMe I/O */
	/* force_async 처리 */
	if (o->force_async && ++ld->prepped == o->force_async) {
		ld->prepped = 0;
		sqe->flags |= IOSQE_ASYNC;
	}
	/* 고정 버퍼 사용 시 uring_cmd 고정 버퍼 플래그 설정 */
	if (o->fixedbufs) {
		sqe->uring_cmd_flags = IORING_URING_CMD_FIXED;
		sqe->buf_index = io_u->index;
	}

	/* SQE의 cmd 필드에서 NVMe 커맨드 구조체를 가져옴 (128바이트 SQE의 확장 영역) */
	cmd = (struct nvme_uring_cmd *)sqe->cmd;
	/* DSM(Dataset Management) 구조체 크기 계산 및 해당 io_u의 DSM 포인터 계산 */
	dsm_size = sizeof(*ld->dsm) + td->o.num_range * sizeof(struct nvme_dsm_range);
	ptr += io_u->index * dsm_size;
	dsm = (struct nvme_dsm *)ptr;

	/*
	 * If READ command belongs to the verification phase and the
	 * verify_mode=compare, convert READ to COMPARE command.
	 */
	/*
	 * 검증 단계의 READ 커맨드이고 verify_mode=compare인 경우,
	 * READ를 NVMe COMPARE 커맨드로 변환.
	 * COMPARE는 호스트 데이터와 디바이스 데이터를 디바이스 측에서 직접 비교.
	 */
	if (io_u->flags & IO_U_F_VER_LIST && io_u->ddir == DDIR_READ &&
			o->verify_mode == FIO_URING_CMD_VMODE_COMPARE) {
		populate_verify_io_u(td, io_u);          /* 검증 데이터 채우기 */
		read_opcode = nvme_cmd_compare;          /* READ → COMPARE로 변경 */
		io_u_set(td, io_u, IO_U_F_VER_IN_DEV);  /* 디바이스 측 검증 표시 */
	}

	/* NVMe uring 커맨드를 채워넣음 (opcode, LBA, 길이, 플래그 등) */
	return fio_nvme_uring_cmd_prep(cmd, io_u,
			o->nonvectored ? NULL : &ld->iovecs[io_u->index],
			dsm, read_opcode, ld->write_opcode,
			ld->cdw12_flags[io_u->ddir]);
}

/*
 * [함수] fio_ioring_validate_md
 * [역할] 읽기 완료 후 메타데이터(PI)의 무결성을 검증.
 *        pi_act=0(호스트 측 PI 처리)이고 PI 타입이 설정된 경우에만 동작.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 검증할 I/O 유닛
 * [반환값] 없음 (에러 시 io_u->error에 설정)
 */
static void fio_ioring_validate_md(struct thread_data *td, struct io_u *io_u)
{
	struct nvme_data *data;
	struct ioring_options *o = td->eo;
	int ret;

	data = FILE_ENG_DATA(io_u->file);  /* 파일의 NVMe 데이터 */
	/* PI 타입이 있고, 읽기 방향이고, pi_act=0(호스트 측 검증)인 경우 */
	if (data->pi_type && (io_u->ddir == DDIR_READ) && !o->pi_act) {
		ret = fio_nvme_pi_verify(data, io_u);  /* PI 무결성 검증 */
		if (ret)
			io_u->error = -ret;  /* 검증 실패 시 에러 설정 */
	}

	return;
}

/*
 * [함수] fio_ioring_event
 * [역할] CQ Ring에서 완료된 이벤트(CQE) 하나를 가져와서 해당하는 io_u를 반환.
 *        일반 io_uring 엔진(비 NVMe passthrough)용.
 *
 * [CQE 처리 흐름]
 * 1. event 번호와 cq_ring_off로 CQE 인덱스 계산
 * 2. CQE의 user_data에서 io_u 포인터 복원
 * 3. CQE의 res(결과)를 확인하여 io_u->error 설정
 * 4. 메타데이터 검증 수행 (필요 시)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - event: 이벤트 인덱스 (0부터 시작, getevents가 반환한 수 범위 내)
 * [반환값] 완료된 io_u 포인터
 */
static struct io_u *fio_ioring_event(struct thread_data *td, int event)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_uring_cqe *cqe;
	struct io_u *io_u;
	unsigned index;

	/* CQ Ring에서 해당 이벤트의 인덱스 계산 (링 마스크로 래핑) */
	index = (event + ld->cq_ring_off) & ld->cq_ring_mask;

	/* 해당 인덱스의 CQE를 가져옴 */
	cqe = &ld->cq_ring.cqes[index];
	/* CQE의 user_data에서 io_u 포인터 복원 (fio_ioring_prep에서 설정한 값) */
	io_u = (struct io_u *) (uintptr_t) cqe->user_data;

	/* trim returns 0 on success */
	/* 성공 확인: 전송된 바이트 == 요청한 바이트, 또는 TRIM 성공 (결과 0) */
	if (cqe->res == io_u->xfer_buflen ||
	    (io_u->ddir == DDIR_TRIM && !cqe->res)) {
		io_u->error = 0;
		/* 읽기 완료 시 메타데이터 검증 (pi_act=0인 경우) */
		if (io_u->ddir == DDIR_READ && o->md_per_io_size && !o->pi_act)
			fio_ioring_validate_md(td, io_u);
		return io_u;
	}

	/* TRIM 실패 시 비동기 TRIM 실패 플래그를 설정하여 이후 동기 모드로 전환 */
	if (io_u->ddir == DDIR_TRIM) {
		ld->async_trim_fail = 1;
		cqe->res = 0;
	}
	/* 에러 처리: res가 요청 크기보다 크면 음수 에러코드로 해석 */
	if (cqe->res > io_u->xfer_buflen)
		io_u->error = -cqe->res;
	else
		/* 부분 완료: 남은 바이트 수(잔여량) 계산 */
		io_u->resid = io_u->xfer_buflen - cqe->res;

	return io_u;
}

/*
 * [함수] fio_ioring_cmd_event
 * [역할] CQ Ring에서 완료된 이벤트(CQE)를 가져와 해당 io_u를 반환.
 *        io_uring_cmd 엔진(NVMe passthrough)용.
 *
 * 일반 io_uring 이벤트와의 차이:
 * - NVMe는 128바이트 CQE 사용 (IORING_SETUP_CQE32) → 인덱스 2배
 * - 에러 코드가 NVMe 상태 코드(SCT/SC) 형식
 * - IO_U_F_DEVICE_ERROR 플래그로 errno와 디바이스 에러를 구분
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - event: 이벤트 인덱스
 * [반환값] 완료된 io_u 포인터
 */
static struct io_u *fio_ioring_cmd_event(struct thread_data *td, int event)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_uring_cqe *cqe;
	struct io_u *io_u;
	struct nvme_data *data;
	unsigned index;
	int ret;

	/* CQ Ring에서 이벤트 인덱스 계산 */
	index = (event + ld->cq_ring_off) & ld->cq_ring_mask;
	/* NVMe는 CQE32 사용 → 인덱스를 2배로 */
	if (o->cmd_type == FIO_URING_CMD_NVME)
		index <<= 1;

	cqe = &ld->cq_ring.cqes[index];
	io_u = (struct io_u *) (uintptr_t) cqe->user_data;

	/* CQE의 res를 에러 코드로 설정 (NVMe: 0=성공, 양수=NVMe 에러) */
	io_u->error = cqe->res;
	if (io_u->error != 0)
		goto ret;

	/* 성공한 NVMe 읽기 시 PI 검증 수행 (pi_act=0인 경우) */
	if (o->cmd_type == FIO_URING_CMD_NVME) {
		data = FILE_ENG_DATA(io_u->file);
		if (data->pi_type && (io_u->ddir == DDIR_READ) && !o->pi_act) {
			ret = fio_nvme_pi_verify(data, io_u);
			if (ret)
				io_u->error = ret;
		}
	}

ret:
	/*
	 * If IO_U_F_DEVICE_ERROR is not set, io_u->error will be parsed as an
	 * errno, otherwise device-specific error value (status value in CQE).
	 */
	/*
	 * IO_U_F_DEVICE_ERROR가 설정되지 않으면 io_u->error는 errno로 해석되고,
	 * 설정되면 디바이스별 에러 값(CQE의 상태 값, NVMe SCT/SC)으로 해석됨.
	 */
	if ((int)io_u->error > 0)
		io_u_set(td, io_u, IO_U_F_DEVICE_ERROR);     /* 양수 에러 = 디바이스 에러 */
	else
		io_u_clear(td, io_u, IO_U_F_DEVICE_ERROR);   /* 0 또는 음수 = errno */
	io_u->error = abs((int)io_u->error);               /* 에러를 절대값으로 변환 */
	return io_u;
}

/*
 * [함수] fio_ioring_cmd_errdetails
 * [역할] io_uring_cmd 에러의 상세 정보를 문자열로 생성.
 *        NVMe의 경우 CQE 상태 코드(SCT, SC)를 포맷하여 반환.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 에러가 발생한 I/O 유닛
 * [반환값] 에러 상세 문자열 (NULL이면 상세 정보 없음)
 */
static char *fio_ioring_cmd_errdetails(struct thread_data *td,
				       struct io_u *io_u)
{
	struct ioring_options *o = td->eo;
	/* NVMe 상태 코드 파싱: SCT(Status Code Type)와 SC(Status Code) */
	unsigned int sct = (io_u->error >> 8) & 0x7;   /* 상태 코드 유형 (3비트) */
	unsigned int sc = io_u->error & 0xff;           /* 상태 코드 (8비트) */
#define MAXERRDETAIL 1024
#define MAXMSGCHUNK 128
	char *msg, msgchunk[MAXMSGCHUNK];

	/* 디바이스 에러가 아니면 상세 정보 불필요 */
	if (!(io_u->flags & IO_U_F_DEVICE_ERROR))
		return NULL;

	msg = calloc(1, MAXERRDETAIL);
	strcpy(msg, "io_uring_cmd: ");

	/* 파일명 추가 */
	snprintf(msgchunk, MAXMSGCHUNK, "%s: ", io_u->file->file_name);
	strlcat(msg, msgchunk, MAXERRDETAIL);

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		/* NVMe 에러: SCT와 SC를 16진수로 출력 */
		strlcat(msg, "cq entry status (", MAXERRDETAIL);

		snprintf(msgchunk, MAXMSGCHUNK, "sct=0x%02x; ", sct);
		strlcat(msg, msgchunk, MAXERRDETAIL);

		snprintf(msgchunk, MAXMSGCHUNK, "sc=0x%02x)", sc);
		strlcat(msg, msgchunk, MAXERRDETAIL);
	} else {
		/* Print status code in generic */
		/* 기타 디바이스: 상태 코드를 일반 형식으로 출력 */
		snprintf(msgchunk, MAXMSGCHUNK, "status=0x%x", io_u->error);
		strlcat(msg, msgchunk, MAXERRDETAIL);
	}

	return msg;
}

/*
 * [함수] fio_ioring_cqring_reap
 * [역할] CQ Ring에서 완료된 이벤트(CQE)를 수확(reap)하여 사용 가능한 수를 반환.
 *
 * [CQ Ring 수확 메커니즘]
 * 1. CQ Ring의 tail(커널이 업데이트)과 head(유저가 업데이트)의 차이로 사용 가능한 CQE 수 계산
 * 2. head를 전진시켜 커널에 소비 완료를 알림
 * 3. 메모리 순서 보장: tail은 acquire로 읽고, head는 relaxed로 쓰기
 *
 * libaio와의 차이: libaio의 io_getevents()는 커널→유저 복사가 필요하지만,
 * io_uring은 공유 메모리의 포인터만 업데이트하므로 제로 카피.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - max: 최대 수확할 CQE 수
 * [반환값] 실제로 수확한 CQE 수
 */
static unsigned fio_ioring_cqring_reap(struct thread_data *td, unsigned int max)
{
	struct ioring_data *ld = td->io_ops_data;
	struct io_cq_ring *ring = &ld->cq_ring;
	/* CQ Ring의 현재 head 위치 */
	unsigned head = *ring->head;
	/* tail을 acquire 의미론으로 읽어 커널이 추가한 CQE가 보이도록 보장 */
	unsigned available = atomic_load_acquire(ring->tail) - head;

	/* 사용 가능한 CQE가 없으면 0 반환 */
	if (!available)
		return 0;

	/* 최대 max개까지만 수확 */
	available = min(available, max);
	/*
	 * The CQ consumer index is advanced before the CQEs are actually read.
	 * This is generally unsafe, as it lets the kernel reuse the CQE slots.
	 * However, the CQ is sized large enough for the maximum iodepth and a
	 * new SQE won't be submitted until the CQE is processed, so the CQE
	 * slot won't actually be reused until it has been processed.
	 */
	/*
	 * CQ 소비자 인덱스(head)를 CQE를 실제로 읽기 전에 전진시킴.
	 * 일반적으로는 안전하지 않지만 (커널이 CQE 슬롯을 재사용할 수 있으므로),
	 * CQ 크기가 최대 iodepth만큼 크고, CQE가 처리되기 전에는
	 * 새 SQE가 제출되지 않으므로 실제로 슬롯이 재사용되지 않음.
	 */
	atomic_store_relaxed(ring->head, head + available);
	return available;
}

/*
 * [함수] fio_ioring_getevents
 * [역할] 완료된 I/O 이벤트를 수집하는 함수.
 *        CQ Ring에서 CQE를 수확하고, 필요 시 io_uring_enter()로 완료를 기다림.
 *
 * [I/O 흐름에서의 위치]
 * fio_ioring_queue() → fio_ioring_commit() → [커널 I/O 처리]
 * → fio_ioring_getevents() (여기) → fio_ioring_event()로 개별 CQE 처리
 *
 * [동작 방식]
 * 1. fio_ioring_cqring_reap()으로 이미 완료된 CQE를 수확 시도
 * 2. min 개수만큼 모이면 즉시 반환
 * 3. 부족하면 io_uring_enter()로 커널에 완료를 기다리도록 요청
 *    (SQPOLL 모드에서는 io_uring_enter()를 호출하지 않고 계속 폴링)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - min: 최소 수집해야 할 이벤트 수
 *   - max: 최대 수집 가능한 이벤트 수
 *   - t: 타임아웃 (현재 미사용)
 * [반환값] 수집된 이벤트 수, 실패 시 음수 에러코드
 */
static int fio_ioring_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct ioring_data *ld = td->io_ops_data;
	/* 최소 완료 수: iodepth_batch_complete_min=0이면 non-blocking (0개도 OK) */
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct ioring_options *o = td->eo;
	struct io_cq_ring *ring = &ld->cq_ring;
	unsigned events = 0;
	int r;

	/* CQ Ring의 현재 head를 cq_ring_off에 저장 (이벤트 처리 시작점) */
	ld->cq_ring_off = *ring->head;
	/* 완료 이벤트 수집 루프 */
	for (;;) {
		/* CQ Ring에서 완료된 CQE 수확 시도 */
		r = fio_ioring_cqring_reap(td, max - events);
		if (r) {
			events += r;
			/* min 개수 이상 수집했으면 반환 */
			if (events >= min)
				return events;

			/* 아직 부족하면 남은 최소 필요 수 갱신 */
			if (actual_min != 0)
				actual_min -= r;
		}

		/* SQPOLL 모드가 아닌 경우에만 io_uring_enter()로 커널에 완료 대기 요청 */
		if (!o->sqpoll_thread) {
			r = io_uring_enter(ld, 0, actual_min, enter_flags);
			if (r < 0) {
				/* EAGAIN/EINTR은 재시도 */
				if (errno == EAGAIN || errno == EINTR)
					continue;
				r = -errno;
				td_verror(td, errno, "io_uring_enter");
				return r;
			}
		}
	}
}

/*
 * [함수] fio_ioring_cmd_nvme_pi
 * [역할] NVMe uring_cmd에서 PI(Protection Information) 데이터를 SQE에 채움.
 *        NVMe passthrough 경로에서 PI 관련 필드를 설정.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛
 * [반환값] 없음
 */
static inline void fio_ioring_cmd_nvme_pi(struct thread_data *td,
					  struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct nvme_uring_cmd *cmd;
	struct io_uring_sqe *sqe;

	/* TRIM은 PI와 무관하므로 건너뜀 */
	if (io_u->ddir == DDIR_TRIM)
		return;

	/* NVMe는 128바이트 SQE → 인덱스 2배 */
	sqe = &ld->sqes[(io_u->index) << 1];
	cmd = (struct nvme_uring_cmd *)sqe->cmd;

	/* NVMe 커맨드에 PI 필드 채움 (가드 태그, 참조 태그, 앱 태그) */
	fio_nvme_pi_fill(cmd, io_u, &ld->ext_opts);
}

/*
 * [함수] fio_ioring_setup_pi
 * [역할] 일반 io_uring 경로에서 PI 가드 태그(CRC)를 생성.
 *        비 NVMe passthrough에서 메타데이터가 있는 I/O의 가드 태그 계산.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛
 * [반환값] 없음
 */
static inline void fio_ioring_setup_pi(struct thread_data *td,
				      struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;

	/* TRIM은 PI와 무관 */
	if (io_u->ddir == DDIR_TRIM)
		return;

	/* 가드 태그(CRC16 또는 CRC64) 생성 */
	fio_nvme_generate_guard(io_u, &ld->ext_opts);
}

/*
 * [함수] fio_ioring_cmdprio_prep
 * [역할] 커맨드 우선순위(cmdprio) 옵션이 활성화된 경우,
 *        I/O의 우선순위를 SQE에 설정.
 *        cmdprio_percentage/cmdprio_bssplit에 따라 일부 I/O의 우선순위를 변경.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: I/O 유닛
 * [반환값] 없음
 */
static inline void fio_ioring_cmdprio_prep(struct thread_data *td,
					   struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct cmdprio *cmdprio = &ld->cmdprio;

	/* 우선순위가 변경되었으면 SQE의 ioprio 필드를 업데이트 */
	if (fio_cmdprio_set_ioprio(td, cmdprio, io_u))
		ld->sqes[io_u->index].ioprio = io_u->ioprio;
}

/*
 * [함수] fio_ioring_queue
 * [역할] I/O 요청을 SQ Ring에 추가하는 함수. fio의 queue 콜백.
 *
 * === 핵심 I/O 제출 흐름 (1단계) ===
 *
 * [흐름 개요]
 * fio_ioring_queue() (여기)
 *   → SQ Ring의 array[tail]에 SQE 인덱스를 기록하고 tail을 증가
 *   → queued 카운터 증가
 *   → FIO_Q_QUEUED 반환 (나중에 commit에서 일괄 제출됨)
 *
 * fio_ioring_commit()
 *   → io_uring_enter() 시스콜로 SQ에 쌓인 SQE들을 커널에 제출
 *   → (SQPOLL: 시스콜 없이 커널 스레드가 자동 처리)
 *
 * fio_ioring_getevents()
 *   → CQ Ring에서 완료된 CQE를 수확
 *
 * [SQ Ring 동작 상세]
 * 1. tail = *ring->tail (현재 tail 위치 읽기)
 * 2. ring->array[tail & mask] = io_u->index (SQE 인덱스 기록)
 * 3. atomic_store_release(ring->tail, tail + 1) (tail 전진, 메모리 배리어)
 * 4. 커널(또는 SQPOLL 스레드)은 head와 tail의 차이를 감지하여 새 SQE 처리
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 큐에 추가할 I/O 유닛
 * [반환값]
 *   - FIO_Q_QUEUED: 성공적으로 큐에 추가됨 (나중에 commit으로 제출)
 *   - FIO_Q_BUSY: 큐가 가득 참 (iodepth에 도달)
 *   - FIO_Q_COMPLETED: 동기적으로 즉시 완료됨 (syncfs, 동기 trim)
 */
static enum fio_q_status fio_ioring_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_sq_ring *ring = &ld->sq_ring;
	unsigned tail;

	/* 읽기 전용 모드 체크 */
	fio_ro_check(td, io_u);

	/* should not hit... */
	/* 큐 깊이가 최대에 도달하면 BUSY 반환 - 이 상황은 발생하지 않아야 함 */
	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/*
	 * If this is a syncfs request, or if async trim has been tried and
	 * failed, punt to sync.
	 * */
	/*
	 * syncfs 요청이거나, 비동기 TRIM이 이전에 실패한 경우
	 * 동기 처리로 전환 (io_uring을 우회)
	 */
	if (io_u->ddir == DDIR_SYNCFS ||
	    (io_u->ddir == DDIR_TRIM && ld->async_trim_fail)) {
		/* 큐에 대기 중인 요청이 있으면 BUSY 반환 (먼저 처리해야 함) */
		if (ld->queued)
			return FIO_Q_BUSY;

		/* 동기적으로 즉시 처리 */
		if (io_u->ddir == DDIR_TRIM)
			do_io_u_trim(td, io_u);    /* 동기 TRIM 실행 */
		else
			do_io_u_sync(td, io_u);    /* 동기 sync 실행 */

		io_u_mark_submit(td, 1);    /* 제출 통계 업데이트 */
		io_u_mark_complete(td, 1);  /* 완료 통계 업데이트 */
		return FIO_Q_COMPLETED;     /* 즉시 완료로 반환 */
	}

	/* 커맨드 우선순위가 설정된 경우 SQE의 ioprio를 조정 */
	if (ld->cmdprio.mode != CMDPRIO_MODE_NONE)
		fio_ioring_cmdprio_prep(td, io_u);

	/* PI(Protection Information) 설정
	 * NVMe passthrough → NVMe PI 채우기
	 * 일반 io_uring + 메타데이터 → 가드 태그 생성 */
	if (o->cmd_type == FIO_URING_CMD_NVME && ld->is_uring_cmd_eng)
		fio_ioring_cmd_nvme_pi(td, io_u);
	else if (o->md_per_io_size)
		fio_ioring_setup_pi(td, io_u);

	/* === SQ Ring에 SQE 인덱스 추가 === */
	tail = *ring->tail;                                    /* 현재 tail 위치 읽기 */
	ring->array[tail & ld->sq_ring_mask] = io_u->index;   /* SQE 인덱스를 array에 기록 */
	atomic_store_release(ring->tail, tail + 1);            /* tail 전진 (release 배리어: 데이터가 먼저 쓰여짐을 보장) */

	ld->queued++;           /* 대기 중인 I/O 수 증가 */
	return FIO_Q_QUEUED;    /* 큐에 추가됨 - commit에서 일괄 제출될 예정 */
}

/*
 * [함수] fio_ioring_queued
 * [역할] 커널에 제출된 I/O들의 발행 시간(issue_time)을 기록.
 *        제출 지연시간(slat) 측정과 iolog에 사용됨.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - start: SQ Ring에서 시작 인덱스
 *   - nr: 처리할 I/O 수
 * [반환값] 없음
 */
static void fio_ioring_queued(struct thread_data *td, int start, int nr)
{
	struct ioring_data *ld = td->io_ops_data;
	struct timespec now;

	/* 발행 시간 기록이 필요 없으면 건너뜀 */
	if (!fio_fill_issue_time(td))
		return;

	/* 현재 시간 취득 */
	fio_gettime(&now, NULL);

	/* 각 제출된 I/O에 대해 발행 시간 기록 */
	while (nr--) {
		struct io_sq_ring *ring = &ld->sq_ring;
		/* SQ Ring의 array에서 SQE 인덱스를 가져옴 */
		int index = ring->array[start & ld->sq_ring_mask];
		/* 인덱스로 io_u를 찾음 */
		struct io_u *io_u = ld->io_u_index[index];

		/* 발행 시간 기록 */
		memcpy(&io_u->issue_time, &now, sizeof(now));
		io_u_queued(td, io_u);  /* fio에 큐잉 완료 알림 */

		start++;
	}

	/*
	 * only used for iolog
	 */
	/* iolog 파일 사용 시 마지막 발행 시간 기록 */
	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &now, sizeof(now));
}

/*
 * [함수] fio_ioring_commit
 * [역할] SQ Ring에 쌓인 I/O 요청들을 커널에 실제로 제출하는 함수.
 *        fio의 commit 콜백.
 *
 * === 핵심 I/O 제출 흐름 (2단계) ===
 *
 * fio_ioring_queue()에서 SQ Ring에 SQE를 추가한 후,
 * fio_ioring_commit()이 호출되어 io_uring_enter() 시스콜로 커널에 알림.
 *
 * [SQPOLL 모드 vs 일반 모드]
 *
 * SQPOLL 모드 (sqpoll_thread=1):
 * - 커널 스레드가 SQ를 지속 감시하므로 io_uring_enter() 불필요
 * - 단, 커널 스레드가 sleep한 경우 (IORING_SQ_NEED_WAKEUP 플래그)
 *   io_uring_enter(IORING_ENTER_SQ_WAKEUP)으로 깨워야 함
 * - libaio 대비 장점: 시스콜 오버헤드 완전 제거 가능
 *
 * 일반 모드:
 * - io_uring_enter()로 커널에 제출할 SQE 수를 알림
 * - libaio의 io_submit()에 해당하지만, SQE가 이미 공유 메모리에 있으므로
 *   데이터 복사 없이 포인터만 전달
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 에러코드 (실패)
 */
static int fio_ioring_commit(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	int ret;

	/* 대기 중인 I/O가 없으면 할 일 없음 */
	if (!ld->queued)
		return 0;

	/*
	 * Kernel side does submission. just need to check if the ring is
	 * flagged as needing a kick, if so, call io_uring_enter(). This
	 * only happens if we've been idle too long.
	 */
	/*
	 * SQPOLL 모드: 커널 측에서 제출을 처리함.
	 * 링에 NEED_WAKEUP 플래그가 설정되었는지만 확인하고,
	 * 설정되어 있으면 io_uring_enter()로 커널 스레드를 깨움.
	 * (유휴 시간이 너무 길면 커널 스레드가 sleep하므로)
	 */
	if (o->sqpoll_thread) {
		struct io_sq_ring *ring = &ld->sq_ring;
		unsigned start = *ld->sq_ring.tail - ld->queued;  /* 이번에 추가된 SQE의 시작 위치 */
		unsigned flags;

		/* SQ Ring의 flags를 읽어 NEED_WAKEUP 확인 */
		flags = atomic_load_relaxed(ring->flags);
		if (flags & IORING_SQ_NEED_WAKEUP)
			/* 커널 SQPOLL 스레드를 깨움 */
			io_uring_enter(ld, ld->queued, 0,
					IORING_ENTER_SQ_WAKEUP);
		/* 발행 시간 기록 */
		fio_ioring_queued(td, start, ld->queued);
		/* 제출 통계 업데이트 */
		io_u_mark_submit(td, ld->queued);

		ld->queued = 0;  /* 대기 큐 비움 */
		return 0;
	}

	/* 일반 모드: io_uring_enter()로 커널에 SQE 제출 */
	do {
		unsigned start = *ld->sq_ring.head;  /* SQ Ring의 현재 head (커널의 소비 위치) */
		long nr = ld->queued;                /* 제출할 수 */

		/* io_uring_enter() 호출: nr개의 SQE 제출 */
		ret = io_uring_enter(ld, nr, 0, enter_flags);
		if (ret > 0) {
			/* 성공: ret개의 SQE가 제출됨 */
			fio_ioring_queued(td, start, ret);   /* 발행 시간 기록 */
			io_u_mark_submit(td, ret);           /* 제출 통계 */

			ld->queued -= ret;   /* 대기 수 감소 */
			ret = 0;
		} else if (!ret) {
			/* 0개 제출됨 - 재시도 */
			io_u_mark_submit(td, ret);
			continue;
		} else {
			/* 에러 처리 */
			if (errno == EAGAIN || errno == EINTR) {
				/* 리소스 부족 또는 인터럽트 - CQ에서 완료를 수확하여 공간 확보 시도 */
				ret = fio_ioring_cqring_reap(td, ld->queued);
				if (ret)
					continue;
				/* Shouldn't happen */
				/* 발생하지 않아야 하는 상황 - 짧은 대기 후 재시도 */
				usleep(1);
				continue;
			}
			ret = -errno;
			td_verror(td, errno, "io_uring_enter submit");
			break;
		}
	} while (ld->queued);  /* 모든 대기 I/O가 제출될 때까지 반복 */

	return ret;
}

/*
 * [함수] fio_ioring_unmap
 * [역할] io_uring 인스턴스의 mmap 매핑을 해제하고 ring fd를 닫음.
 *        cleanup 시 호출됨.
 *
 * [파라미터]
 *   - ld: io_uring 데이터
 * [반환값] 없음
 */
static void fio_ioring_unmap(struct ioring_data *ld)
{
	int i;

	/* 3개의 mmap 영역 해제: SQ Ring, SQEs, CQ Ring */
	for (i = 0; i < FIO_ARRAY_SIZE(ld->mmap); i++)
		munmap(ld->mmap[i].ptr, ld->mmap[i].len);
	/* io_uring 파일 디스크립터 닫기 */
	close(ld->ring_fd);
}

/*
 * [함수] fio_ioring_cleanup
 * [역할] io_uring 엔진의 모든 리소스를 정리하는 함수. fio의 cleanup 콜백.
 *        mmap 해제, 동적 할당 메모리 해제 등을 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 없음
 */
static void fio_ioring_cleanup(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;

	if (ld) {
		/* 자식 스레드가 아닌 경우에만 mmap 해제 (자식은 부모의 매핑을 공유) */
		if (!(td->flags & TD_F_CHILD))
			fio_ioring_unmap(ld);

		fio_cmdprio_cleanup(&ld->cmdprio);  /* 커맨드 우선순위 리소스 해제 */
		free(ld->io_u_index);   /* io_u 인덱스 배열 해제 */
		free(ld->md_buf);       /* 메타데이터 버퍼 해제 */
		free(ld->pi_attr);      /* PI 속성 버퍼 해제 */
		free(ld->iovecs);       /* iovec 배열 해제 */
		free(ld->fds);          /* 등록된 fd 배열 해제 */
		free(ld->dsm);          /* NVMe DSM 구조체 해제 */
		free(ld);               /* 엔진 데이터 구조체 자체 해제 */
	}
}

/*
 * [함수] fio_ioring_mmap
 * [역할] io_uring 인스턴스의 SQ Ring, SQE 배열, CQ Ring을 mmap()으로
 *        유저스페이스에 매핑하는 함수.
 *
 * io_uring의 핵심 특징 중 하나: 커널과 유저가 메모리를 공유하여
 * 시스템 콜 없이 데이터를 주고받을 수 있음.
 *
 * [매핑 영역]
 * mmap[0]: SQ Ring (head, tail, flags, array 등)
 *          - IORING_OFF_SQ_RING 오프셋으로 매핑
 * mmap[1]: SQE 배열 (실제 I/O 요청 기술자들)
 *          - IORING_OFF_SQES 오프셋으로 매핑
 *          - SQE128 모드에서는 2배 크기
 * mmap[2]: CQ Ring (head, tail, CQE 배열 등)
 *          - IORING_OFF_CQ_RING 오프셋으로 매핑
 *          - CQE32 모드에서는 CQE 2배 크기
 *
 * [파라미터]
 *   - ld: io_uring 데이터
 *   - p: io_uring_setup()에서 반환된 파라미터 (각 구성요소의 오프셋 정보 포함)
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_mmap(struct ioring_data *ld, struct io_uring_params *p)
{
	struct io_sq_ring *sring = &ld->sq_ring;
	struct io_cq_ring *cring = &ld->cq_ring;
	void *ptr;

	/* === SQ Ring mmap (mmap[0]) === */
	/* 매핑 크기: array의 오프셋 + entries * sizeof(__u32) */
	ld->mmap[0].len = p->sq_off.array + p->sq_entries * sizeof(__u32);
	ptr = mmap(0, ld->mmap[0].len, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ld->ring_fd,
			IORING_OFF_SQ_RING);
	ld->mmap[0].ptr = ptr;
	/* 커널이 알려준 오프셋을 사용하여 각 필드의 포인터를 설정 */
	sring->head = ptr + p->sq_off.head;              /* SQ head 포인터 */
	sring->tail = ptr + p->sq_off.tail;              /* SQ tail 포인터 */
	sring->ring_mask = ptr + p->sq_off.ring_mask;    /* 링 마스크 */
	sring->ring_entries = ptr + p->sq_off.ring_entries; /* 엔트리 수 */
	sring->flags = ptr + p->sq_off.flags;            /* 플래그 (NEED_WAKEUP 등) */
	sring->array = ptr + p->sq_off.array;            /* SQE 인덱스 배열 */
	ld->sq_ring_mask = *sring->ring_mask;            /* 마스크를 로컬에 캐시 */

	/* === SQE 배열 mmap (mmap[1]) === */
	/* SQE128 모드(NVMe)에서는 SQE 크기가 2배 */
	if (p->flags & IORING_SETUP_SQE128)
		ld->mmap[1].len = 2 * p->sq_entries * sizeof(struct io_uring_sqe);
	else
		ld->mmap[1].len = p->sq_entries * sizeof(struct io_uring_sqe);
	ld->sqes = mmap(0, ld->mmap[1].len, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE, ld->ring_fd,
				IORING_OFF_SQES);
	ld->mmap[1].ptr = ld->sqes;

	/* === CQ Ring mmap (mmap[2]) === */
	/* CQE32 모드(NVMe)에서는 CQE 크기가 2배 */
	if (p->flags & IORING_SETUP_CQE32) {
		ld->mmap[2].len = p->cq_off.cqes +
					2 * p->cq_entries * sizeof(struct io_uring_cqe);
	} else {
		ld->mmap[2].len = p->cq_off.cqes +
					p->cq_entries * sizeof(struct io_uring_cqe);
	}
	ptr = mmap(0, ld->mmap[2].len, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ld->ring_fd,
			IORING_OFF_CQ_RING);
	ld->mmap[2].ptr = ptr;
	/* CQ Ring의 각 필드 포인터 설정 */
	cring->head = ptr + p->cq_off.head;              /* CQ head 포인터 */
	cring->tail = ptr + p->cq_off.tail;              /* CQ tail 포인터 */
	cring->ring_mask = ptr + p->cq_off.ring_mask;    /* 링 마스크 */
	cring->ring_entries = ptr + p->cq_off.ring_entries; /* 엔트리 수 */
	cring->cqes = ptr + p->cq_off.cqes;              /* CQE 배열 포인터 */
	ld->cq_ring_mask = *cring->ring_mask;             /* 마스크를 로컬에 캐시 */
	return 0;
}

/*
 * [함수] fio_ioring_probe
 * [역할] io_uring 인스턴스가 비벡터 I/O(IORING_OP_READ/WRITE)를 지원하는지 탐지.
 *        nonvectored=-1(자동) 모드에서 호출됨.
 *
 * IORING_REGISTER_PROBE를 사용하여 커널이 지원하는 opcode 목록을 조회하고,
 * READ/WRITE가 지원되면 nonvectored=1로 설정.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 없음
 */
static void fio_ioring_probe(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_uring_probe *p;
	int ret;

	/* already set by user, don't touch */
	/* 사용자가 명시적으로 설정한 경우 건드리지 않음 */
	if (o->nonvectored != -1)
		return;

	/* default to off, as that's always safe */
	/* 기본값: 벡터 I/O 사용 (항상 안전) */
	o->nonvectored = 0;

	/* probe 구조체 할당 (최대 256개 opcode 조회) */
	p = calloc(1, sizeof(*p) + 256 * sizeof(struct io_uring_probe_op));
	if (!p)
		return;

	/* io_uring_register PROBE 호출 - 커널이 지원하는 opcode 목록 조회 */
	ret = syscall(__NR_io_uring_register, ld->ring_fd,
			IORING_REGISTER_PROBE, p, 256);
	if (ret < 0)
		goto out;

	/* IORING_OP_WRITE가 조회 범위 밖이면 지원 안 함 */
	if (IORING_OP_WRITE > p->ops_len)
		goto out;

	/* READ와 WRITE 모두 지원되면 비벡터 모드 활성화 */
	if ((p->ops[IORING_OP_READ].flags & IO_URING_OP_SUPPORTED) &&
	    (p->ops[IORING_OP_WRITE].flags & IO_URING_OP_SUPPORTED))
		o->nonvectored = 1;
out:
	free(p);
}

/*
 * [함수] fio_ioring_queue_init
 * [역할] 일반 io_uring 인스턴스를 생성하고 초기화하는 함수.
 *        io_uring_setup() 시스콜을 호출하여 커널에 io_uring 인스턴스를 생성.
 *
 * [초기화 과정]
 * 1. io_uring_params 설정 (IOPOLL, SQPOLL, CQSIZE 등 플래그)
 * 2. io_uring_setup() 시스콜로 인스턴스 생성
 * 3. 실패 시 최신 기능부터 제거하며 재시도 (하위 호환성)
 * 4. probe로 비벡터 I/O 지원 여부 확인
 * 5. fixedbufs 사용 시 버퍼 등록
 * 6. mmap()으로 SQ/CQ Ring 매핑
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_queue_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	int depth = ld->iodepth;         /* 큐 깊이 (2의 거듭제곱) */
	struct io_uring_params p;
	int ret;

	memset(&p, 0, sizeof(p));

	/* IOPOLL: 폴링 기반 완료 확인 (인터럽트 없이) */
	if (o->hipri)
		p.flags |= IORING_SETUP_IOPOLL;
	/* SQPOLL: 커널 스레드가 SQ를 지속 감시 */
	if (o->sqpoll_thread) {
		p.flags |= IORING_SETUP_SQPOLL;
		/* SQPOLL 스레드를 특정 CPU에 고정 */
		if (o->sqpoll_set) {
			p.flags |= IORING_SETUP_SQ_AFF;
			p.sq_thread_cpu = o->sqpoll_cpu;
		}

		/*
		 * Submission latency for sqpoll_thread is just the time it
		 * takes to fill in the SQ ring entries, and any syscall if
		 * IORING_SQ_NEED_WAKEUP is set, we don't need to log that time
		 * separately.
		 */
		/*
		 * SQPOLL 모드에서 제출 지연시간(slat)은 SQ Ring 엔트리를 채우는 시간 +
		 * NEED_WAKEUP 시 시스콜 시간뿐이므로, 별도로 기록할 필요 없음.
		 */
		td->o.disable_slat = 1;
	}

	/*
	 * Clamp CQ ring size at our SQ ring size, we don't need more entries
	 * than that.
	 */
	/* CQ Ring 크기를 SQ Ring과 동일하게 설정 - 그 이상은 불필요 */
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = depth;

	/*
	 * Setup COOP_TASKRUN as we don't need to get IPI interrupted for
	 * completing IO operations.
	 */
	/* COOP_TASKRUN: I/O 완료 시 IPI(프로세서간 인터럽트) 대신
	 * 협력적 태스크 실행 - 불필요한 인터럽트 감소 */
	p.flags |= IORING_SETUP_COOP_TASKRUN;

	/*
	 * io_uring is always a single issuer, and we can defer task_work
	 * runs until we reap events.
	 */
	/* SINGLE_ISSUER: 단일 스레드만 제출함을 보장하여 최적화
	 * DEFER_TASKRUN: 이벤트 수확 시점까지 태스크 작업을 지연 */
	p.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

retry:
	/* io_uring_setup 시스콜: 커널에 io_uring 인스턴스 생성 요청 */
	ret = syscall(__NR_io_uring_setup, depth, &p);
	if (ret < 0) {
		/* 최신 기능이 지원되지 않으면 하나씩 제거하며 재시도 (graceful degradation) */
		if (errno == EINVAL && p.flags & IORING_SETUP_DEFER_TASKRUN) {
			p.flags &= ~IORING_SETUP_DEFER_TASKRUN;
			p.flags &= ~IORING_SETUP_SINGLE_ISSUER;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_COOP_TASKRUN) {
			p.flags &= ~IORING_SETUP_COOP_TASKRUN;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_CQSIZE) {
			p.flags &= ~IORING_SETUP_CQSIZE;
			goto retry;
		}
		return ret;
	}

	/* NO_IOWAIT 기능 지원 시 enter_flags에 추가 (불필요한 I/O 대기 방지) */
	if (p.features & IORING_FEAT_NO_IOWAIT)
		enter_flags |= IORING_ENTER_NO_IOWAIT;
	ld->ring_fd = ret;   /* io_uring 파일 디스크립터 저장 */

	/* 비벡터 I/O 지원 여부 탐지 */
	fio_ioring_probe(td);

	/* fixedbufs: 버퍼를 커널에 사전 등록 */
	if (o->fixedbufs) {
		ret = syscall(__NR_io_uring_register, ld->ring_fd,
				IORING_REGISTER_BUFFERS, ld->iovecs, depth);
		if (ret < 0)
			return ret;
	}

	/* SQ Ring, SQEs, CQ Ring을 mmap()으로 매핑 */
	return fio_ioring_mmap(ld, &p);
}

/*
 * [함수] fio_ioring_cmd_queue_init
 * [역할] io_uring_cmd 엔진(NVMe passthrough)용 io_uring 인스턴스를 생성.
 *        일반 io_uring과 유사하지만 SQE128/CQE32 모드를 추가로 설정.
 *
 * NVMe passthrough에서는:
 * - IORING_SETUP_SQE128: SQE가 128바이트로 확장 (NVMe 커맨드 64바이트 포함)
 * - IORING_SETUP_CQE32: CQE가 32바이트로 확장 (NVMe 완료 데이터 포함)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_queue_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	int depth = ld->iodepth;
	struct io_uring_params p;
	int ret;

	memset(&p, 0, sizeof(p));

	/* IOPOLL 설정 */
	if (o->hipri)
		p.flags |= IORING_SETUP_IOPOLL;
	/* SQPOLL 설정 */
	if (o->sqpoll_thread) {
		p.flags |= IORING_SETUP_SQPOLL;
		if (o->sqpoll_set) {
			p.flags |= IORING_SETUP_SQ_AFF;
			p.sq_thread_cpu = o->sqpoll_cpu;
		}

		/*
		 * Submission latency for sqpoll_thread is just the time it
		 * takes to fill in the SQ ring entries, and any syscall if
		 * IORING_SQ_NEED_WAKEUP is set, we don't need to log that time
		 * separately.
		 */
		/* SQPOLL 모드에서는 제출 지연시간 측정 비활성화 */
		td->o.disable_slat = 1;
	}
	/* NVMe는 확장 SQE(128바이트)와 확장 CQE(32바이트) 필요 */
	if (o->cmd_type == FIO_URING_CMD_NVME) {
		p.flags |= IORING_SETUP_SQE128;  /* 128바이트 SQE (기본 64 + NVMe cmd 64) */
		p.flags |= IORING_SETUP_CQE32;   /* 32바이트 CQE (기본 16 + NVMe 결과 16) */
	}

	/*
	 * Clamp CQ ring size at our SQ ring size, we don't need more entries
	 * than that.
	 */
	/* CQ Ring 크기 = SQ Ring 크기 */
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = depth;

	/*
	 * Setup COOP_TASKRUN as we don't need to get IPI interrupted for
	 * completing IO operations.
	 */
	/* 협력적 태스크 실행 설정 */
	p.flags |= IORING_SETUP_COOP_TASKRUN;

	/*
	 * io_uring is always a single issuer, and we can defer task_work
	 * runs until we reap events.
	 */
	/* 단일 발행자 + 태스크 작업 지연 */
	p.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

retry:
	/* io_uring 인스턴스 생성 */
	ret = syscall(__NR_io_uring_setup, depth, &p);
	if (ret < 0) {
		/* 하위 호환성을 위한 graceful degradation */
		if (errno == EINVAL && p.flags & IORING_SETUP_DEFER_TASKRUN) {
			p.flags &= ~IORING_SETUP_DEFER_TASKRUN;
			p.flags &= ~IORING_SETUP_SINGLE_ISSUER;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_COOP_TASKRUN) {
			p.flags &= ~IORING_SETUP_COOP_TASKRUN;
			goto retry;
		}
		if (errno == EINVAL && p.flags & IORING_SETUP_CQSIZE) {
			p.flags &= ~IORING_SETUP_CQSIZE;
			goto retry;
		}
		return ret;
	}

	ld->ring_fd = ret;  /* io_uring 파일 디스크립터 저장 */

	/* 비벡터 I/O 지원 탐지 */
	fio_ioring_probe(td);

	/* 고정 버퍼 등록 */
	if (o->fixedbufs) {
		ret = syscall(__NR_io_uring_register, ld->ring_fd,
				IORING_REGISTER_BUFFERS, ld->iovecs, depth);
		if (ret < 0)
			return ret;
	}

	/* SQ/CQ Ring mmap 매핑 */
	return fio_ioring_mmap(ld, &p);
}

/*
 * [함수] fio_ioring_register_files
 * [역할] 파일 디스크립터를 io_uring에 사전 등록하는 함수.
 *
 * 등록된 파일의 장점:
 * - 매 I/O마다 fd → struct file lookup 비용 제거
 * - atomic refcount 증감 비용 제거
 * - SQPOLL 모드에서 필수 (커널 스레드가 유저 fd 테이블에 접근 불가)
 *
 * [동작 과정]
 * 1. 모든 파일을 generic_open_file()로 열기
 * 2. fd 배열을 io_uring_register(IORING_REGISTER_FILES)로 커널에 등록
 * 3. 등록 후 f->fd를 -1로 설정 (파일이 닫힌 것처럼 보이지만 커널 내부에서 참조 유지)
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_register_files(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct fio_file *f;
	unsigned int i;
	int ret;

	/* fd 배열 할당 */
	ld->fds = calloc(td->o.nr_files, sizeof(int));

	/* 모든 파일을 열고 fd를 배열에 저장 */
	for_each_file(td, f, i) {
		ret = generic_open_file(td, f);
		if (ret)
			goto err;
		ld->fds[i] = f->fd;        /* fd 저장 */
		f->engine_pos = i;          /* 등록 인덱스 저장 (SQE에서 사용) */
	}

	/* 커널에 fd 배열 등록 */
	ret = syscall(__NR_io_uring_register, ld->ring_fd,
			IORING_REGISTER_FILES, ld->fds, td->o.nr_files);
	if (ret) {
err:
		free(ld->fds);
		ld->fds = NULL;
	}

	/*
	 * Pretend the file is closed again, and really close it if we hit
	 * an error.
	 */
	/*
	 * 파일이 닫힌 것처럼 처리 (fd=-1).
	 * 에러 발생 시에는 실제로 파일을 닫음.
	 * (커널 내부에서는 등록된 참조가 유지됨)
	 */
	for_each_file(td, f, i) {
		if (ret) {
			int fio_unused ret2;
			ret2 = generic_close_file(td, f);   /* 에러 시 실제로 닫기 */
		} else
			f->fd = -1;   /* 성공 시 fd를 -1로 (커널 내부에서 참조 유지) */
	}

	return ret;
}

/*
 * [함수] fio_ioring_post_init
 * [역할] io_uring 엔진의 후기 초기화 함수.
 *        io_u가 할당된 후 호출되어 iovec 설정, 큐 초기화, 파일 등록 등을 수행.
 *
 * fio의 초기화 순서: init → io_u 할당 → post_init
 * post_init에서는 io_u가 이미 할당되어 있으므로 버퍼 주소를 iovec에 설정 가능.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 1 (실패)
 */
static int fio_ioring_post_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_u *io_u;
	int err, i;

	/* 각 io_u의 버퍼 정보를 iovec에 설정 */
	for (i = 0; i < td->o.iodepth; i++) {
		struct iovec *iov = &ld->iovecs[i];

		io_u = ld->io_u_index[i];
		iov->iov_base = io_u->buf;             /* 버퍼 시작 주소 */
		iov->iov_len = td_max_bs(td);           /* 최대 블록 크기 */
	}

	/* io_uring 인스턴스 생성 및 초기화 */
	err = fio_ioring_queue_init(td);
	if (err) {
		int init_err = errno;

		if (init_err == ENOSYS)
			log_err("fio: your kernel doesn't support io_uring\n");
		td_verror(td, init_err, "io_queue_init");
		return 1;
	}

	/* 모든 SQE를 0으로 초기화 */
	for (i = 0; i < ld->iodepth; i++) {
		struct io_uring_sqe *sqe;

		sqe = &ld->sqes[i];
		memset(sqe, 0, sizeof(*sqe));
	}

	/* 파일 등록 (registerfiles 옵션 사용 시) */
	if (o->registerfiles) {
		err = fio_ioring_register_files(td);
		if (err) {
			td_verror(td, errno, "ioring_register_files");
			return 1;
		}
	}

	return 0;
}

/*
 * [함수] fio_ioring_cmd_post_init
 * [역할] io_uring_cmd 엔진(NVMe passthrough)의 후기 초기화 함수.
 *        fio_ioring_post_init과 유사하지만 NVMe용 확장 SQE 초기화.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 1 (실패)
 */
static int fio_ioring_cmd_post_init(struct thread_data *td)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct io_u *io_u;
	int err, i;

	/* iovec 초기화 (각 io_u의 버퍼 정보 설정) */
	for (i = 0; i < td->o.iodepth; i++) {
		struct iovec *iov = &ld->iovecs[i];

		io_u = ld->io_u_index[i];
		iov->iov_base = io_u->buf;
		iov->iov_len = td_max_bs(td);
	}

	/* io_uring_cmd 인스턴스 생성 (SQE128/CQE32 포함) */
	err = fio_ioring_cmd_queue_init(td);
	if (err) {
		int init_err = errno;

		td_verror(td, init_err, "io_queue_init");
		return 1;
	}

	/* SQE 초기화 - NVMe는 128바이트 SQE이므로 2배 크기로 memset */
	for (i = 0; i < ld->iodepth; i++) {
		struct io_uring_sqe *sqe;

		if (o->cmd_type == FIO_URING_CMD_NVME) {
			sqe = &ld->sqes[i << 1];              /* 인덱스 2배 (SQE128) */
			memset(sqe, 0, 2 * sizeof(*sqe));      /* 128바이트 초기화 */
		} else {
			sqe = &ld->sqes[i];
			memset(sqe, 0, sizeof(*sqe));           /* 64바이트 초기화 */
		}
	}

	/* 파일 등록 */
	if (o->registerfiles) {
		err = fio_ioring_register_files(td);
		if (err) {
			td_verror(td, errno, "ioring_register_files");
			return 1;
		}
	}

	return 0;
}

/*
 * [함수] parse_prchk_flags
 * [역할] pi_chk 옵션 문자열을 파싱하여 prchk 비트마스크로 변환.
 *        "GUARD,REFTAG,APPTAG" 형식의 문자열을 NVMe PI 플래그로 변환.
 *
 * [파라미터]
 *   - o: io_uring 옵션 구조체
 * [반환값] 없음
 */
static void parse_prchk_flags(struct ioring_options *o)
{
	if (!o->pi_chk)
		return;

	/* 각 PI 검사 유형에 대해 문자열에서 키워드를 검색 */
	if (strstr(o->pi_chk, "GUARD") != NULL)
		o->prchk = NVME_IO_PRINFO_PRCHK_GUARD;     /* 가드 태그(CRC) 검사 */
	if (strstr(o->pi_chk, "REFTAG") != NULL)
		o->prchk |= NVME_IO_PRINFO_PRCHK_REF;      /* 참조 태그 검사 */
	if (strstr(o->pi_chk, "APPTAG") != NULL)
		o->prchk |= NVME_IO_PRINFO_PRCHK_APP;      /* 애플리케이션 태그 검사 */
}

/*
 * [함수] fio_ioring_cmd_init
 * [역할] io_uring_cmd 엔진(NVMe passthrough)의 NVMe 관련 초기화.
 *        쓰기 모드에 따른 opcode 설정, FUA 플래그 설정 등.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - ld: io_uring 데이터
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_cmd_init(struct thread_data *td, struct ioring_data *ld)
{
	struct ioring_options *o = td->eo;

	/* 쓰기가 포함된 워크로드인 경우, 쓰기 모드에 따라 NVMe opcode 결정 */
	if (td_write(td)) {
		switch (o->write_mode) {
		case FIO_URING_CMD_WMODE_UNCOR:
			ld->write_opcode = nvme_cmd_write_uncor;    /* Write Uncorrectable */
			break;
		case FIO_URING_CMD_WMODE_ZEROES:
			ld->write_opcode = nvme_cmd_write_zeroes;   /* Write Zeroes */
			/* DEAC(Deallocate) 비트 설정 시 CDW12 bit 25 */
			if (o->deac)
				ld->cdw12_flags[DDIR_WRITE] = 1 << 25;
			break;
		case FIO_URING_CMD_WMODE_VERIFY:
			ld->write_opcode = nvme_cmd_verify;         /* Verify */
			break;
		default:
			ld->write_opcode = nvme_cmd_write;          /* 일반 Write */
			break;
		}
	}

	/* FUA(Force Unit Access) 플래그 설정 - NVMe CDW12 bit 30 */
	if (o->readfua)
		ld->cdw12_flags[DDIR_READ] = 1 << 30;    /* 읽기 FUA */
	if (o->writefua)
		ld->cdw12_flags[DDIR_WRITE] = 1 << 30;   /* 쓰기 FUA */

	return 0;
}

/*
 * [함수] fio_ioring_init
 * [역할] io_uring 엔진의 메인 초기화 함수. fio의 init 콜백.
 *        엔진 데이터 구조체를 할당하고, 옵션을 검증하고,
 *        메타데이터/iovec/DSM 버퍼를 할당.
 *
 * io_uring과 io_uring_cmd 두 엔진 모두 이 함수를 공유.
 *
 * [초기화 순서]
 * 1. 옵션 검증 (sqpoll → registerfiles 필수, nr_files == open_files)
 * 2. ioring_data 구조체 할당
 * 3. iodepth를 2의 거듭제곱으로 반올림 (io_uring 요구사항)
 * 4. io_u_index, md_buf, pi_attr, iovecs 배열 할당
 * 5. cmdprio 초기화
 * 6. DSM(Dataset Management) 구조체 할당 (TRIM용)
 * 7. io_uring_cmd 엔진이면 NVMe 관련 추가 초기화
 *
 * [파라미터]
 *   - td: 스레드 데이터
 * [반환값] 0 (성공), 1 (실패)
 */
static int fio_ioring_init(struct thread_data *td)
{
	struct ioring_options *o = td->eo;
	struct ioring_data *ld;
	struct nvme_dsm *dsm;
	void *ptr;
	unsigned int dsm_size;
	unsigned long long md_size;
	int ret, i;
	struct nvme_cmd_ext_io_opts *ext_opts;

	/* sqthread submission requires registered files */
	/* SQPOLL 모드에서는 등록된 파일이 필수 (커널 스레드가 유저 fd 테이블 접근 불가) */
	if (o->sqpoll_thread)
		o->registerfiles = 1;

	/* 등록된 파일 사용 시 nr_files == open_files 이어야 함 (부분 등록 불가) */
	if (o->registerfiles && td->o.nr_files != td->o.open_files) {
		log_err("fio: io_uring registered files require nr_files to "
			"be identical to open_files\n");
		return 1;
	}

	/* 엔진 데이터 구조체 할당 */
	ld = calloc(1, sizeof(*ld));

	/* io_uring_cmd 엔진 여부 판별 (prep 함수 포인터로 구분) */
	ld->is_uring_cmd_eng = (td->io_ops->prep == fio_ioring_cmd_prep);

	/*
	 * The internal io_uring queue depth must be a power-of-2, as that's
	 * how the ring interface works. So round that up, in case the user
	 * set iodepth isn't a power-of-2. Leave the fio depth the same, as
	 * not to be driving too much of an iodepth, if we did round up.
	 */
	/*
	 * io_uring 내부 큐 깊이는 2의 거듭제곱이어야 함 (링 인터페이스 요구사항).
	 * 사용자 설정이 2의 거듭제곱이 아니면 올림.
	 * fio의 논리적 깊이는 그대로 유지하여 과도한 I/O를 방지.
	 */
	ld->iodepth = roundup_pow2(td->o.iodepth);

	/* io_u index */
	/* io_u 인덱스 배열 할당 - SQE 인덱스로 io_u를 빠르게 찾기 위함 */
	ld->io_u_index = calloc(td->o.iodepth, sizeof(struct io_u *));

	/* io_uring 경로에서 메타데이터 사용 시 apptag_mask 검증 */
	if (!ld->is_uring_cmd_eng && o->md_per_io_size) {
		if (o->apptag_mask != 0xffff) {
			log_err("fio: io_uring with metadata requires an apptag_mask of 0xffff\n");
			free(ld->io_u_index);
			free(ld);
			return 1;
		}
	}

	/*
	 * metadata buffer
	 * We are only supporting iomem=malloc / mem=malloc as of now.
	 */
	/*
	 * 메타데이터 버퍼 할당
	 * 현재 iomem=malloc / mem=malloc만 지원
	 */
	if (o->md_per_io_size && (!ld->is_uring_cmd_eng ||
	    (ld->is_uring_cmd_eng && o->cmd_type == FIO_URING_CMD_NVME))) {
		/* 전체 메타데이터 버퍼 크기 계산 (I/O당 크기 * iodepth + 정렬 여유) */
		md_size = (unsigned long long) o->md_per_io_size
				* (unsigned long long) td->o.iodepth;
		md_size += page_mask + td->o.mem_align;
		if (td->o.mem_align && td->o.mem_align > page_size)
			md_size += td->o.mem_align - page_size;
		ld->md_buf = malloc(md_size);
		if (!ld->md_buf) {
			free(ld->io_u_index);
			free(ld);
			return 1;
		}

		/* 비 NVMe passthrough 경로에서는 PI 속성 배열도 할당 */
		if (!ld->is_uring_cmd_eng) {
			ld->pi_attr = calloc(ld->iodepth, sizeof(struct io_uring_attr_pi));
			if (!ld->pi_attr) {
				free(ld->io_u_index);
				free(ld->md_buf);
				free(ld);
				return 1;
			}
		}

	}
	/* pi_chk 문자열을 비트마스크로 파싱 */
	parse_prchk_flags(o);
	/* 확장 I/O 옵션 설정 (PI 관련) */
	ext_opts = &ld->ext_opts;
	if (o->pi_act)
		ext_opts->io_flags |= NVME_IO_PRINFO_PRACT;   /* PI Action 비트 */
	ext_opts->io_flags |= o->prchk;                     /* PI 검사 플래그 */
	ext_opts->apptag = o->apptag;                        /* Application Tag */
	ext_opts->apptag_mask = o->apptag_mask;              /* Application Tag 마스크 */

	/* iovec 배열 할당 (scatter/gather I/O용) */
	ld->iovecs = calloc(ld->iodepth, sizeof(struct iovec));

	/* 엔진 데이터를 스레드 데이터에 연결 */
	td->io_ops_data = ld;

	/* 커맨드 우선순위 초기화 */
	ret = fio_cmdprio_init(td, &ld->cmdprio, &o->cmdprio_options);
	if (ret) {
		td_verror(td, EINVAL, "fio_ioring_init");
		return 1;
	}

	/*
	 * For io_uring_cmd, trims are async operations unless we are operating
	 * in zbd mode where trim means zone reset.
	 */
	/*
	 * io_uring_cmd에서 TRIM은 비동기 작업이지만,
	 * ZBD(존 블록 디바이스) 모드에서는 TRIM이 존 리셋을 의미하므로 동기 처리.
	 */
	if (td_trim(td) && td->o.zone_mode == ZONE_MODE_ZBD &&
	    ld->is_uring_cmd_eng) {
		td->io_ops->flags |= FIO_ASYNCIO_SYNC_TRIM;
	} else {
		/* DSM(Dataset Management) 구조체 할당 (TRIM/Deallocate 용) */
		dsm_size = sizeof(*ld->dsm);
		dsm_size += td->o.num_range * sizeof(struct nvme_dsm_range);
		ld->dsm = calloc(td->o.iodepth, dsm_size);
		ptr = ld->dsm;
		/* 각 I/O 슬롯에 대해 DSM 구조체의 범위 수 초기화 */
		for (i = 0; i < td->o.iodepth; i++) {
			dsm = (struct nvme_dsm *)ptr;
			dsm->nr_ranges = td->o.num_range;
			ptr += dsm_size;
		}
	}

	/* io_uring_cmd 엔진이면 NVMe 관련 추가 초기화 수행 */
	if (ld->is_uring_cmd_eng)
		return fio_ioring_cmd_init(td, ld);
	return 0;
}

/*
 * [함수] fio_ioring_io_u_init
 * [역할] 개별 io_u(I/O 유닛)의 초기화 함수. fio의 io_u_init 콜백.
 *        io_u를 io_u_index에 등록하고, 메타데이터/PI 버퍼를 연결.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 초기화할 I/O 유닛
 * [반환값] 0 (항상 성공)
 */
static int fio_ioring_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;
	struct nvme_pi_data *pi_data;
	char *p, *q;

	/* io_u를 인덱스 배열에 등록 (SQE 인덱스로 io_u를 찾을 수 있도록) */
	ld->io_u_index[io_u->index] = io_u;

	/* 메타데이터 버퍼의 해당 io_u 영역 계산 및 연결 */
	p = PTR_ALIGN(ld->md_buf, page_mask) + td->o.mem_align;
	p += o->md_per_io_size * io_u->index;
	io_u->mmap_data = p;   /* io_u의 메타데이터 버퍼 포인터 */

	/* PI 속성이 있으면 (비 NVMe passthrough 경로) 설정 */
	if (ld->pi_attr) {
		struct io_uring_attr_pi *pi_attr;

		/* 해당 io_u의 PI 속성 영역 계산 */
		q = ld->pi_attr;
		q += (sizeof(struct io_uring_attr_pi) * io_u->index);
		io_u->pi_attr = q;

		/* PI 속성 초기화 */
		pi_attr = io_u->pi_attr;
		pi_attr->len = o->md_per_io_size;    /* 메타데이터 크기 */
		pi_attr->app_tag = o->apptag;        /* Application Tag */
		pi_attr->flags = 0;
		/* PI 검사 플래그를 io_uring PI 속성 플래그로 변환 */
		if (o->prchk & NVME_IO_PRINFO_PRCHK_GUARD)
			pi_attr->flags |= IO_INTEGRITY_CHK_GUARD;
		if (o->prchk & NVME_IO_PRINFO_PRCHK_REF)
			pi_attr->flags |= IO_INTEGRITY_CHK_REFTAG;
		if (o->prchk & NVME_IO_PRINFO_PRCHK_APP)
			pi_attr->flags |= IO_INTEGRITY_CHK_APPTAG;
	}

	/* pi_act=0(호스트 측 PI 처리)인 경우 PI 데이터 구조체 할당 */
	if (!o->pi_act) {
		pi_data = calloc(1, sizeof(*pi_data));
		pi_data->io_flags |= o->prchk;          /* PI 검사 플래그 */
		pi_data->apptag_mask = o->apptag_mask;   /* App Tag 마스크 */
		pi_data->apptag = o->apptag;             /* App Tag 값 */
		io_u->engine_data = pi_data;              /* io_u에 엔진별 데이터 연결 */
	}

	return 0;
}

/*
 * [함수] fio_ioring_io_u_free
 * [역할] 개별 io_u의 리소스를 해제하는 함수. fio의 io_u_free 콜백.
 *        PI 데이터 구조체를 해제.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - io_u: 해제할 I/O 유닛
 * [반환값] 없음
 */
static void fio_ioring_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct nvme_pi *pi = io_u->engine_data;

	free(pi);                     /* PI 데이터 해제 */
	io_u->engine_data = NULL;     /* 포인터 초기화 */
}

/*
 * [함수] fio_get_pi_info
 * [역할] 파일(블록 디바이스)에서 Protection Information 기능 정보를 조회.
 *        FS_IOC_GETLBMD_CAP ioctl을 사용하여 메타데이터/PI 지원 여부와
 *        관련 속성(크기, 유형, 오프셋 등)을 가져옴.
 *
 * 비 NVMe passthrough 경로에서 메타데이터가 있는 I/O에 사용.
 *
 * [파라미터]
 *   - f: fio 파일 구조체
 *   - data: NVMe 데이터 구조체 (결과 저장)
 * [반환값] 0 (성공), 음수 에러코드 (실패)
 */
static int fio_get_pi_info(struct fio_file *f, struct nvme_data *data)
{
	struct logical_block_metadata_cap md_cap;
	int ret;
	int fd, err = 0;

	/* 파일을 읽기 전용으로 열기 */
	fd = open(f->file_name, O_RDONLY);
	if (fd < 0)
		return -errno;

	/* ioctl로 논리 블록 메타데이터 기능 조회 */
	ret = ioctl(fd, FS_IOC_GETLBMD_CAP, &md_cap);
	if (ret < 0) {
		err = -errno;
		log_err("%s: failed to query protection information capabilities; error %d\n", f->file_name, errno);
		goto out;
	}

	/* 무결성 보호가 지원되지 않으면 에러 */
	if (!(md_cap.lbmd_flags & LBMD_PI_CAP_INTEGRITY)) {
		log_err("%s: Protection information not supported\n", f->file_name);
		err = -ENOTSUP;
		goto out;
	}

	/* Currently we don't support storage tags */
	/* 스토리지 태그는 현재 미지원 */
	if (md_cap.lbmd_storage_tag_size) {
		log_err("%s: Storage tag not supported\n", f->file_name);
		err = -ENOTSUP;
		goto out;
	}

	/* 조회된 정보를 NVMe 데이터 구조체에 저장 */
	data->lba_size = md_cap.lbmd_interval;         /* LBA 데이터 크기 */
	data->lba_shift = ilog2(data->lba_size);       /* LBA 크기의 log2 */
	data->ms = md_cap.lbmd_size;                   /* 메타데이터 크기 */
	data->pi_size = md_cap.lbmd_pi_size;           /* PI 크기 */
	data->pi_loc = !(md_cap.lbmd_pi_offset);       /* PI 위치 (시작=1, 끝=0) */

	/* Assume Type 1 PI if reference tags supported */
	/* 참조 태그가 지원되면 Type 1 PI, 아니면 Type 3 PI로 판단 */
	if (md_cap.lbmd_flags & LBMD_PI_CAP_REFTAG)
		data->pi_type = NVME_NS_DPS_PI_TYPE1;
	else
		data->pi_type = NVME_NS_DPS_PI_TYPE3;

	/* 가드 태그 유형 매핑 */
	switch (md_cap.lbmd_guard_tag_type) {
	case LBMD_PI_CSUM_CRC16_T10DIF:
		data->guard_type = NVME_NVM_NS_16B_GUARD;   /* T10 DIF CRC16 */
		break;
	case LBMD_PI_CSUM_CRC64_NVME:
		data->guard_type = NVME_NVM_NS_64B_GUARD;   /* NVMe CRC64 */
		break;
	default:
		log_err("%s: unsupported checksum type %d\n", f->file_name,
				md_cap.lbmd_guard_tag_type);
		err = -ENOTSUP;
		goto out;
	}

out:
	close(fd);
	return err;
}

/*
 * [함수] fio_ioring_open_file_md
 * [역할] 메타데이터가 있는 파일 열기 시 PI 정보를 조회하여
 *        파일 엔진 데이터에 저장.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 열려는 파일
 * [반환값] 0 (성공), 음수 (실패)
 */
static inline int fio_ioring_open_file_md(struct thread_data *td, struct fio_file *f)
{
	int ret = 0;
	struct nvme_data *data = NULL;

	/* 이미 엔진 데이터가 설정되어 있으면 건너뜀 (중복 호출 방지) */
	data = FILE_ENG_DATA(f);
	if (data == NULL) {
		data = calloc(1, sizeof(struct nvme_data));
		/* PI 정보 조회 */
		ret = fio_get_pi_info(f, data);
		if (ret) {
			free(data);
			return ret;
		}

		/* 파일에 엔진 데이터 연결 */
		FILE_SET_ENG_DATA(f, data);
	}

	return ret;
}

/*
 * [함수] fio_ioring_open_file
 * [역할] io_uring 엔진의 파일 열기 함수. fio의 open_file 콜백.
 *        메타데이터가 있으면 PI 정보를 조회하고,
 *        등록된 파일 사용 시 fd를 복원.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 열려는 파일
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_open_file(struct thread_data *td, struct fio_file *f)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;

	if (o->md_per_io_size) {
		/*
		 * This will be a no-op when called by the io_uring_cmd
		 * ioengine because engine data has already been collected by
		 * the time this call is made
		 */
		/*
		 * io_uring_cmd 엔진에서 호출 시에는 no-op (이미 엔진 데이터가 수집됨).
		 * 일반 io_uring에서만 PI 정보를 실제로 조회.
		 */
		int ret = fio_ioring_open_file_md(td, f);
		if (ret)
			return ret;
	}

	/* 등록된 파일이 아니면 일반적인 파일 열기 */
	if (!ld || !o->registerfiles)
		return generic_open_file(td, f);

	/* 등록된 파일: 이전에 저장한 fd를 복원 */
	f->fd = ld->fds[f->engine_pos];
	return 0;
}

/*
 * [함수] verify_params
 * [역할] NVMe 디바이스의 블록 크기와 메타데이터 설정이 올바른지 검증.
 *        블록 크기가 LBA 크기의 배수인지, 메타데이터 버퍼가 충분한지 확인.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - data: NVMe 디바이스 데이터
 *   - f: 대상 파일
 *   - ddir: I/O 방향 (READ, WRITE, TRIM)
 * [반환값] 0 (유효), 1 (무효)
 */
static int verify_params(struct thread_data *td, struct nvme_data *data,
			 struct fio_file *f, enum fio_ddir ddir)
{
	struct ioring_options *o = td->eo;
	unsigned int lba_size;

	/* 확장 LBA가 있으면 그 크기 사용, 없으면 기본 LBA 크기 */
	lba_size = data->lba_ext ? data->lba_ext : data->lba_size;
	/* 블록 크기가 LBA 크기의 배수인지 확인 */
	if (td->o.min_bs[ddir] % lba_size || td->o.max_bs[ddir] % lba_size) {
		if (data->lba_ext) {
			log_err("%s: block size must be a multiple of %u "
				"(LBA data size + Metadata size)\n", f->file_name, lba_size);
			/* 고정 블록 크기가 LBA 데이터 크기의 배수인 경우, 올바른 값을 제안 */
			if (td->o.min_bs[ddir] == td->o.max_bs[ddir] &&
			    !(td->o.min_bs[ddir] % data->lba_size)) {
				/* fixed block size is actually a multiple of LBA data size */
				unsigned long long suggestion = lba_size *
					(td->o.min_bs[ddir] / data->lba_size);
				log_err("Did you mean to use a block size of %llu?\n", suggestion);
			}
		} else {
			log_err("%s: block size must be a multiple of LBA data size\n",
				f->file_name);
		}
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}
	/* 메타데이터가 있고 확장 LBA가 아닌 경우, md_per_io_size가 충분한지 확인 */
	if (data->ms && !data->lba_ext && ddir != DDIR_TRIM &&
	    (o->md_per_io_size < ((td->o.max_bs[ddir] / data->lba_size) * data->ms))) {
		log_err("%s: md_per_io_size should be at least %llu bytes\n",
			f->file_name,
			((td->o.max_bs[ddir] / data->lba_size) * data->ms));
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}

	return 0;
}

/*
 * [함수] fio_ioring_open_nvme
 * [역할] NVMe 디바이스를 열고 네임스페이스 정보를 조회.
 *        NVMe 관련 설정 검증(블록 크기, PI, 쓰기 모드 등) 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: NVMe 디바이스 파일
 * [반환값] 0 (성공), 음수/1 (실패)
 */
static int fio_ioring_open_nvme(struct thread_data *td, struct fio_file *f)
{
	struct ioring_options *o = td->eo;
	struct nvme_data *data = NULL;
	__u64 nlba = 0;    /* 네임스페이스의 총 LBA 수 */
	int ret;

	/* Store the namespace-id and lba size. */
	/* 네임스페이스 ID와 LBA 크기를 저장 */
	data = FILE_ENG_DATA(f);
	if (data == NULL) {
		data = calloc(1, sizeof(struct nvme_data));
		/* NVMe 디바이스 정보 조회 (네임스페이스 정보, LBA 크기 등) */
		ret = fio_nvme_get_info(f, &nlba, o->pi_act, data);
		if (ret) {
			free(data);
			return ret;
		}

		FILE_SET_ENG_DATA(f, data);
	}

	/* 읽기/쓰기/트림 방향별로 파라미터 검증 */
	for_each_rw_ddir(ddir) {
		ret = verify_params(td, data, f, ddir);
		if (ret)
			return ret;
	}

	/*
	 * For extended logical block sizes we cannot use verify when
	 * end to end data protection checks are enabled, as the PI
	 * section of data buffer conflicts with verify.
	 */
	/*
	 * 확장 논리 블록(메타데이터가 데이터에 포함됨) 크기에서는
	 * E2E 데이터 보호가 활성화된 경우 fio verify를 사용할 수 없음.
	 * (PI 영역과 verify 데이터가 충돌)
	 */
	if (data->ms && data->pi_type && data->lba_ext &&
	    td->o.verify != VERIFY_NONE) {
		log_err("%s: for extended LBA, verify cannot be used when E2E "
			"data protection is enabled\n", f->file_name);
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}

	/* 특수 쓰기 모드(uncor, zeroes, verify)인데 쓰기가 포함되지 않으면 에러 */
	if (o->write_mode != FIO_URING_CMD_WMODE_WRITE && !td_write(td)) {
		log_err("%s: 'readwrite=|rw=' has no write\n", f->file_name);
		td_verror(td, EINVAL, "fio_ioring_cmd_open_file");
		return 1;
	}

	return 0;
}

/*
 * [함수] fio_ioring_cmd_open_file
 * [역할] io_uring_cmd 엔진의 파일 열기 함수.
 *        NVMe인 경우 NVMe 관련 검증 후, 일반 파일 열기 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 열려는 파일
 * [반환값] 0 (성공), 음수/1 (실패)
 */
static int fio_ioring_cmd_open_file(struct thread_data *td, struct fio_file *f)
{
	struct ioring_options *o = td->eo;

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		int ret;

		/* NVMe 디바이스 정보 조회 및 검증 */
		ret = fio_ioring_open_nvme(td, f);
		if (ret)
			return ret;
	}

	/* 공통 파일 열기 (메타데이터 처리 + 등록된 파일/일반 열기) */
	return fio_ioring_open_file(td, f);
}

/*
 * [함수] fio_ioring_close_file
 * [역할] io_uring 엔진의 파일 닫기 함수.
 *        등록된 파일 사용 시 fd를 -1로 설정만 하고 (커널 내부 참조 유지),
 *        아니면 일반 닫기 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 닫으려는 파일
 * [반환값] 0 (성공)
 */
static int fio_ioring_close_file(struct thread_data *td, struct fio_file *f)
{
	struct ioring_data *ld = td->io_ops_data;
	struct ioring_options *o = td->eo;

	/* 등록된 파일이 아니면 일반 닫기 */
	if (!ld || !o->registerfiles)
		return generic_close_file(td, f);

	/* 등록된 파일: fd를 -1로만 설정 (커널 내부에서 참조 유지 중) */
	f->fd = -1;
	return 0;
}

/*
 * [함수] fio_ioring_cmd_close_file
 * [역할] io_uring_cmd 엔진의 파일 닫기 함수.
 *        NVMe인 경우 엔진 데이터(NVMe 정보)를 해제하고,
 *        일반 닫기 수행.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 닫으려는 파일
 * [반환값] 0 (성공)
 */
static int fio_ioring_cmd_close_file(struct thread_data *td,
				     struct fio_file *f)
{
	struct ioring_options *o = td->eo;

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		/* NVMe 데이터 해제 */
		struct nvme_data *data = FILE_ENG_DATA(f);

		FILE_SET_ENG_DATA(f, NULL);
		free(data);
	}

	return fio_ioring_close_file(td, f);
}

/*
 * [함수] fio_ioring_cmd_get_file_size
 * [역할] io_uring_cmd 엔진의 파일 크기 조회 함수.
 *        NVMe인 경우 네임스페이스 정보에서 크기를 계산.
 *
 * [파라미터]
 *   - td: 스레드 데이터
 *   - f: 대상 파일
 * [반환값] 0 (성공), 음수 (실패)
 */
static int fio_ioring_cmd_get_file_size(struct thread_data *td,
					struct fio_file *f)
{
	struct ioring_options *o = td->eo;

	/* 이미 크기를 알고 있으면 조기 반환 */
	if (fio_file_size_known(f))
		return 0;

	if (o->cmd_type == FIO_URING_CMD_NVME) {
		struct nvme_data *data = NULL;
		__u64 nlba = 0;   /* 총 LBA 수 */
		int ret;

		data = calloc(1, sizeof(struct nvme_data));
		/* NVMe 네임스페이스 정보 조회 */
		ret = fio_nvme_get_info(f, &nlba, o->pi_act, data);
		if (ret) {
			free(data);
			return ret;
		}

		/* 파일 크기 = LBA 크기 * LBA 수 */
		if (data->lba_ext)
			f->real_file_size = data->lba_ext * nlba;   /* 확장 LBA (데이터+메타데이터) */
		else
			f->real_file_size = data->lba_size * nlba;  /* 기본 LBA (데이터만) */
		fio_file_set_size_known(f);   /* 크기 확인 완료 표시 */

		FILE_SET_ENG_DATA(f, data);
		return 0;
	}
	/* 비 NVMe: 일반적인 파일 크기 조회 */
	return generic_get_file_size(td, f);
}

/*
 * [한국어]
 * fio_ioring_get_zoned_model - 블록 디바이스의 ZBD 호스트 관리/인식 모델 조회
 *
 * @td: 잡 컨텍스트.
 * @f: 대상 파일(블록 디바이스여야 의미있음).
 * @model: 출력 — ZBD_NONE/HOST_AWARE/HOST_MANAGED 중 하나.
 * @return: 0 성공, 음수 -errno.
 *
 * 배경: fio 코어의 ZBD 모듈(zbd.c) 이 td->io_ops->get_zoned_model 콜백으로
 *   디바이스의 존 특성을 조회. 본 엔진은 커널 블록 레이어가 sysfs에 노출한
 *   `/sys/block/<DEV>/queue/zoned` 문자열을 읽는 blkzoned 백엔드에 위임한다.
 *   NVMe ZNS 패스스루의 경우 cmd 엔진이 별도로 fio_nvme_get_zoned_model을 호출.
 *
 * 실행 컨텍스트: 잡 초기화 시점(do_io 루프 전) 메인/잡 스레드. 재진입 안전.
 *
 * 호출 체인:
 *   zbd_setup_files(zbd.c) → td_io_* → [fio_ioring_get_zoned_model] →
 *   blkzoned_get_zoned_model(sysfs 읽기)
 */
static int fio_ioring_get_zoned_model(struct thread_data *td,
				      struct fio_file *f,
				      enum zbd_zoned_model *model)
{
	return blkzoned_get_zoned_model(td, f, model);
	/* [한국어] 공용 blkzoned 헬퍼 — sysfs zoned 속성 파싱 + model 포인터에 저장.
	 * 에러 시 -errno. 본 엔진은 로직 없이 위임만 수행(fio의 "얇은 어댑터" 패턴). */
}

/*
 * [한국어]
 * fio_ioring_report_zones - BLKREPORTZONE ioctl로 존 메타데이터 배열 수집
 *
 * @td: 잡 컨텍스트. @f: 블록 디바이스. @offset: 첫 존의 시작 LBA 바이트 오프셋.
 * @zbdz: 출력 배열(호출자 할당). @nr_zones: 배열 길이(= 조회 원하는 존 수).
 * @return: 실제 채워진 존 수(>=0) 또는 -errno.
 *
 * 배경: ZBD 에뮬레이션/실 디바이스(SMR, nullblk zoned) 모두 BLKREPORTZONE
 *   ioctl로 struct blk_zone 배열을 반환. blkzoned 헬퍼가 바이트→섹터 변환과
 *   blk_zone→fio zbd_zone 필드 매핑을 수행.
 *
 * 호출 체인:
 *   zbd_report_zones(zbd.c) → [fio_ioring_report_zones] → blkzoned_report_zones
 *   → ioctl(BLKREPORTZONE)
 */
static int fio_ioring_report_zones(struct thread_data *td,
				   struct fio_file *f, uint64_t offset,
				   struct zbd_zone *zbdz,
				   unsigned int nr_zones)
{
	return blkzoned_report_zones(td, f, offset, zbdz, nr_zones);
	/* [한국어] blkzoned 위임 — 커널 블록 레이어의 존 정보 테이블 반환. */
}

/*
 * [한국어]
 * fio_ioring_reset_wp - BLKRESETZONE ioctl로 존 Write Pointer 리셋
 *
 * @td: 잡. @f: 블록 디바이스. @offset/@length: 바이트 단위 존 범위.
 * @return: 0/-errno.
 *
 * 배경: SMR/ZNS Sequential Write Required 존은 랜덤 쓰기가 불가하므로, 반복
 *   벤치마크 시 각 epoch 시작에 존을 EMPTY 상태로 리셋해야 한다. fio zbd.c 의
 *   zbd_reset_zones()가 이 콜백을 호출.
 *
 * 호출 체인:
 *   zbd_reset_zones → [fio_ioring_reset_wp] → blkzoned_reset_wp → ioctl(BLKRESETZONE)
 */
static int fio_ioring_reset_wp(struct thread_data *td, struct fio_file *f,
			       uint64_t offset, uint64_t length)
{
	return blkzoned_reset_wp(td, f, offset, length);
	/* [한국어] blkzoned 위임 — 섹터 변환 + ioctl. */
}

/*
 * [한국어]
 * fio_ioring_get_max_open_zones - `/sys/block/X/queue/max_open_zones` 조회
 *
 * @td: 잡. @f: 디바이스. @max_open_zones: 출력(0=무제한).
 * @return: 0/-errno.
 *
 * 배경: 호스트 관리형 존 디바이스는 "동시에 OPEN 상태일 수 있는 존 수"에 제한이
 *   있다. fio가 이 값을 알아야 테스트 시나리오가 불법 상태로 진입하지 않게
 *   IMP_OPEN/EXP_OPEN 카운터를 관리할 수 있다.
 *
 * 호출 체인:
 *   zbd_init_max_open_zones → [fio_ioring_get_max_open_zones] →
 *   blkzoned_get_max_open_zones → sysfs read
 */
static int fio_ioring_get_max_open_zones(struct thread_data *td,
					 struct fio_file *f,
					 unsigned int *max_open_zones)
{
	return blkzoned_get_max_open_zones(td, f, max_open_zones);
	/* [한국어] sysfs 문자열 읽어 uint로 변환. */
}

/*
 * [한국어]
 * fio_ioring_finish_zone - BLKFINISHZONE ioctl로 존을 FULL 상태로 강제 종료
 *
 * @td: 잡. @f: 디바이스. @offset/@length: 바이트 존 범위.
 * @return: 0/-errno.
 *
 * 배경: IMP_OPEN/EXP_OPEN 존을 강제로 FULL 상태로 전환 — 벤치마크 시나리오에서
 *   존 수명 테스트나 GC/WAF 재현에 사용. 쓰기 포인터를 존 끝으로 이동하지만
 *   데이터는 채우지 않음(존 상태 머신만 전이).
 *
 * 호출 체인:
 *   zbd_finish_zone → [fio_ioring_finish_zone] → blkzoned_finish_zone →
 *   ioctl(BLKFINISHZONE)
 */
static int fio_ioring_finish_zone(struct thread_data *td, struct fio_file *f,
				  uint64_t offset, uint64_t length)
{
	return blkzoned_finish_zone(td, f, offset, length);
	/* [한국어] blkzoned 위임. */
}

/*
 * [한국어]
 * fio_ioring_move_zone_wp - 존 Write Pointer를 주어진 오프셋까지 쓰기로 이동
 *
 * @td: 잡. @f: 디바이스. @z: 대상 존. @length: 이동 거리. @buf: 채움 버퍼.
 * @return: 0/-errno.
 *
 * 배경: fio zbd 코어가 io_log replay 또는 verify 시 존 WP를 특정 위치로
 *   정렬해야 하는 경우가 있다(랜덤 read 대상의 WP 이후 영역이 유효 데이터여야
 *   함). 본 헬퍼는 buf 내용을 WP→WP+length 구간에 쓰기로써 WP를 이동.
 *
 * 호출 체인:
 *   zbd_replay → [fio_ioring_move_zone_wp] → blkzoned_move_zone_wp →
 *   pwrite(2) 루프
 */
static int fio_ioring_move_zone_wp(struct thread_data *td, struct fio_file *f,
				   struct zbd_zone *z, uint64_t length,
				   const char *buf)
{
	return blkzoned_move_zone_wp(td, f, z, length, buf);
	/* [한국어] blkzoned 위임 — 존 Sequential Write 의미론 유지하며 WP 전진. */
}

/*
 * [한국어]
 * fio_ioring_cmd_get_zoned_model - NVMe ZNS 디바이스의 존 모델 조회
 *
 * @td: 잡. @f: /dev/nvmeXnY 또는 ng. @model: 출력.
 * @return: 0/-errno.
 *
 * 배경: NVMe ZNS (Zoned Namespace) 커맨드 셋은 CSI(Command Set Identifier) 로
 *   식별(CSI=0x02). fio_nvme_get_zoned_model이 Identify Controller/Namespace를
 *   받아 HOST_MANAGED/NONE을 판정. 일반 블록 경로(blkzoned)가 아닌 passthru
 *   전용 — 블록 레이어가 존재하지 않는 /dev/ng* 에도 동작.
 *
 * 호출 체인:
 *   zbd_setup_files → [fio_ioring_cmd_get_zoned_model] → fio_nvme_get_zoned_model →
 *   nvme_identify_ctrl/ns
 */
static int fio_ioring_cmd_get_zoned_model(struct thread_data *td,
					  struct fio_file *f,
					  enum zbd_zoned_model *model)
{
	return fio_nvme_get_zoned_model(td, f, model);
	/* [한국어] nvme.c 공유 헬퍼 — Identify 명령 2회(Controller + Namespace)로
	 * ZNS CSI 감지 후 ZBD_HOST_MANAGED 반환. */
}

/*
 * [한국어]
 * fio_ioring_cmd_report_zones - NVMe Zone Management Receive 로 존 정보 조회
 *
 * @td: 잡. @f: NVMe 파일. @offset: 시작 LBA 바이트. @zbdz: 출력 배열.
 * @nr_zones: 배열 길이. @return: 실제 반환 존 수 또는 -errno.
 *
 * 배경: NVMe ZNS 스펙의 Zone Management Receive(Zone Report) 커맨드 —
 *   Admin/IO 경로 둘 다 가능하나 fio는 IO 큐 경유(ioctl NVME_IOCTL_IO_CMD).
 *   fio_nvme_report_zones가 64B nvme_zone_desc 배열을 파싱해 zbd_zone으로 변환.
 *
 * 호출 체인:
 *   zbd_report_zones → [fio_ioring_cmd_report_zones] → fio_nvme_report_zones →
 *   ioctl(NVME_IOCTL_IO_CMD, ZONE_MGMT_RECV)
 */
static int fio_ioring_cmd_report_zones(struct thread_data *td,
				       struct fio_file *f, uint64_t offset,
				       struct zbd_zone *zbdz,
				       unsigned int nr_zones)
{
	return fio_nvme_report_zones(td, f, offset, zbdz, nr_zones);
	/* [한국어] NVMe passthru 헬퍼 위임. */
}

/*
 * [한국어]
 * fio_ioring_cmd_reset_wp - NVMe Zone Management Send (RESET) 로 WP 리셋
 *
 * @td: 잡. @f: NVMe 파일. @offset/@length: 바이트 범위.
 * @return: 0/-errno.
 *
 * 배경: NVMe ZNS의 Zone Management Send 커맨드 with Zone Send Action=0x04
 *   (Reset Zone). Select All(bit0) 비트로 전체 존 일괄 리셋 최적화 가능.
 *   fio_nvme_reset_wp가 length 기반으로 범위/전체 분기.
 *
 * 호출 체인:
 *   zbd_reset_zones → [fio_ioring_cmd_reset_wp] → fio_nvme_reset_wp →
 *   ioctl(NVME_IOCTL_IO_CMD, ZONE_MGMT_SEND RESET)
 */
static int fio_ioring_cmd_reset_wp(struct thread_data *td, struct fio_file *f,
				   uint64_t offset, uint64_t length)
{
	return fio_nvme_reset_wp(td, f, offset, length);
	/* [한국어] NVMe passthru 헬퍼 위임. */
}

/*
 * [한국어]
 * fio_ioring_cmd_get_max_open_zones - NVMe Identify Namespace ZNS mor 필드 조회
 *
 * @td: 잡. @f: NVMe 파일. @max_open_zones: 출력.
 * @return: 0/-errno.
 *
 * 배경: ZNS Identify Namespace 데이터 구조의 mor(Maximum Open Resources, 32b)
 *   필드가 "동시에 OPEN 상태 가능한 존 수"의 상한. fio_nvme_get_max_open_zones
 *   이 Identify CSI=ZNS 응답을 파싱.
 *
 * 호출 체인:
 *   zbd_init_max_open_zones → [fio_ioring_cmd_get_max_open_zones] →
 *   fio_nvme_get_max_open_zones → nvme_identify_ns(CSI=ZNS)
 */
static int fio_ioring_cmd_get_max_open_zones(struct thread_data *td,
					     struct fio_file *f,
					     unsigned int *max_open_zones)
{
	return fio_nvme_get_max_open_zones(td, f, max_open_zones);
	/* [한국어] NVMe ZNS Identify 위임. */
}

/*
 * [한국어]
 * fio_ioring_cmd_fetch_ruhs - FDP(Flexible Data Placement) RUH 상태 수집
 *
 * @td: 잡 컨텍스트.
 * @f: NVMe 파일(/dev/nvmeXnY 또는 /dev/ng*).
 * @fruhs_info: 입출력 — 입력: nr_ruhs(호출자 할당한 plis 배열 길이),
 *              출력: nr_ruhs(실 반환 개수), plis[i](Placement Identifier=PID).
 * @return: 0 성공, 음수 -errno(-ENOMEM/-EIO 등).
 *
 * 배경: FDP는 NVMe 2.0 선택적 기능으로, 호스트가 데이터 배치를 Placement
 *   Identifier(PID) 로 제어해 SSD의 GC/WAF(Write Amplification) 효율을 높이는
 *   스펙. RUH(Reclaim Unit Handle)는 디바이스 내부의 재할당 단위 핸들 — 현재
 *   어떤 RUH가 활성/가용 상태인지 알아야 fio가 배치 정책을 세울 수 있다.
 *   I/O Management Receive(Admin/IO 커맨드 셋) 커맨드 with management operation
 *   0x01(RUH Status)로 조회.
 *
 * 동작 단계:
 *   (1) nvme_fdp_ruh_status 헤더 + nr_ruhs 만큼의 ruhss[] 디스크립터 크기로
 *       임시 버퍼 calloc,
 *   (2) fio_nvme_iomgmt_ruhs 로 NVMe 커맨드 발행(ioctl 경로),
 *   (3) 응답의 nruhsd(실제 반환된 디스크립터 수, 리틀엔디안)를 fruhs_info->nr_ruhs
 *       에 복사,
 *   (4) ruhss[i].pid 를 fruhs_info->plis[i] 에 복사,
 *   (5) 임시 버퍼 free.
 *
 * 실행 컨텍스트: 잡 초기화 시점(FDP 사용 선언한 잡) 잡 스레드. 일반 I/O 루프
 *   전에만 호출되므로 동시성 없음.
 *
 * 호출 체인:
 *   dataplacement.c fdp_init → td_io_* → [fio_ioring_cmd_fetch_ruhs] →
 *   fio_nvme_iomgmt_ruhs → ioctl(NVME_IOCTL_IO_CMD, IO_MGMT_RECV)
 */
static int fio_ioring_cmd_fetch_ruhs(struct thread_data *td, struct fio_file *f,
				     struct fio_ruhs_info *fruhs_info)
{
	struct nvme_fdp_ruh_status *ruhs;
	int bytes, nr_ruhs, ret, i;

	nr_ruhs = fruhs_info->nr_ruhs;
	/* RUH 상태 구조체 크기 계산 및 할당 */
	bytes = sizeof(*ruhs) + fruhs_info->nr_ruhs * sizeof(struct nvme_fdp_ruh_status_desc);

	ruhs = calloc(1, bytes);
	if (!ruhs)
		return -ENOMEM;

	/* NVMe I/O Management Receive 커맨드로 RUH 상태 조회 */
	ret = fio_nvme_iomgmt_ruhs(td, f, ruhs, bytes);
	if (ret)
		goto free;

	/* 결과를 fio RUH 정보 구조체로 변환 */
	fruhs_info->nr_ruhs = le16_to_cpu(ruhs->nruhsd);
	for (i = 0; i < nr_ruhs; i++)
		fruhs_info->plis[i] = le16_to_cpu(ruhs->ruhss[i].pid);
free:
	free(ruhs);
	return ret;
}

/*
 * === io_uring 엔진 등록 구조체 ===
 *
 * 일반 io_uring 엔진 (블록 I/O)
 * 이름: "io_uring"
 *
 * [콜백 함수 매핑과 I/O 흐름]
 * init        → fio_ioring_init          (엔진 초기화, 메모리 할당)
 * post_init   → fio_ioring_post_init     (큐 초기화, mmap, 파일 등록)
 * io_u_init   → fio_ioring_io_u_init     (개별 io_u 초기화)
 * io_u_free   → fio_ioring_io_u_free     (개별 io_u 해제)
 * prep        → fio_ioring_prep          (SQE 준비 - opcode, fd, offset 등 설정)
 * queue       → fio_ioring_queue         (SQ Ring에 SQE 인덱스 추가, tail 증가)
 * commit      → fio_ioring_commit        (io_uring_enter()로 커널에 제출)
 * getevents   → fio_ioring_getevents     (CQ Ring에서 CQE 수확)
 * event       → fio_ioring_event         (개별 CQE → io_u 변환)
 * cleanup     → fio_ioring_cleanup       (전체 리소스 해제)
 *
 * [한국어] === ioengine_ops §4 필드 단위 계약 ===
 * 이 구조체는 fio 코어(ioengines.c의 td_io_* 디스패처 + backend.c의 do_io 루프)가
 * 본 엔진을 다루기 위한 vtable. 필드별 설정자/호출자/반환 규약은 개별 필드 주석을
 * 참고. 잡 스레드 1개가 엔진 인스턴스를 단독 소유하므로 각 콜백은 "그 잡 스레드"
 * 컨텍스트에서만 실행된다고 가정할 수 있다(예외: SQPOLL 커널 스레드는 fio 콜백을
 * 호출하지 않고 SQ/CQ 메모리만 공유한다).
 */
static struct ioengine_ops ioengine_uring = {
	.name			= "io_uring",
	/* [한국어] 엔진 이름 문자열. fio --ioengine=io_uring 선택자 키.
	 * 설정자: 정적 초기화(컴파일 타임). 읽는 자: fio 코어 load_ioengine()가
	 *   flist_for_each로 engine_list를 돌며 strcmp로 매칭.
	 * 값 범위: NUL-종결 문자열. 동기화: 읽기 전용 상수. */
	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine_ops ABI 버전. 현재 ioengines.c 헤더가 정의한 값과 동일해야
	 * load 거부되지 않음(외부 .so 플러그인 안전망).
	 * 설정자: 정적. 읽는 자: check_engine_ops()에서 version 불일치 시 에러.
	 * 값 범위: 단조 증가 상수. 동기화: 불변. */
	.flags			= FIO_NO_OFFLOAD | FIO_ASYNCIO_SETS_ISSUE_TIME |
				  FIO_ATOMICWRITES,
	/* [한국어] 엔진 특성 비트 OR. 이 엔진이 세트하는 비트:
	 *  - FIO_NO_OFFLOAD: offload_submit 경로(별도 submission thread) 우회 — io_uring
	 *    자체가 비동기이므로 offload 워커가 불필요하고, SQ 배치에 race가 생기면
	 *    안전하지 않아 금지.
	 *  - FIO_ASYNCIO_SETS_ISSUE_TIME: fio가 issue_time을 기록하지 않고 본 엔진이
	 *    fio_ioring_queued() 에서 직접 memcpy — SQPOLL 경로는 커널 측 실제 제출 시점
	 *    측정이 불가하므로 유저측 "큐잉 완료" 시점을 issue time으로 삼는 규약.
	 *  - FIO_ATOMICWRITES: RWF_ATOMIC (td->o.oatomic) 지원 — 선언 시 fio가 원자적
	 *    쓰기 크기/정렬 검증을 런타임에 수행.
	 * 미설정 비트 의미(중요한 것들):
	 *  - FIO_SYNCIO(미설정): 동기 엔진이 아님 — queue가 즉시 COMPLETED 반환하지 않음.
	 *  - FIO_RAWIO(미설정): 일반 io_uring은 파일시스템 위에서도 동작(raw/char dev
	 *    강제하지 않음). io_uring_cmd 쪽은 세트(하단 참조).
	 *  - FIO_DISKLESSIO(미설정): 실제 디스크 I/O 수행 — disk util 계측 대상.
	 *  - FIO_MEMALIGN(미설정): fixedbufs 아닐 때는 일반 버퍼 정렬만 요구.
	 *  - FIO_PIPEIO(미설정): 파이프 전용 엔진 아님.
	 *  - FIO_ASYNCIO_SYNC_TRIM(미설정): TRIM도 비동기(일반 경로) — ZBD 모드에서만
	 *    런타임에 세트(fio_ioring_init 참조).
	 *  - FIO_ASYNCIO_SYNC_SYNCFS(미설정): DDIR_SYNCFS는 queue()에서 동기 폴백으로
	 *    처리하지만 플래그는 미세트.
	 *  - FIO_NOEXTEND(미설정): fio가 파일 확장 가능.
	 *  - FIO_RO(미설정): 읽기 전용 엔진이 아님.
	 *  - FIO_NODISKUTIL(미설정): iostat 디스크 util 데이터 수집 가능.
	 *  - FIO_MULTI_RANGE_TRIM(미설정): 블록 경로는 단일 DISCARD만 — cmd 경로만 세트.
	 * 설정자: 정적 초기화. 동적으로 fio_ioring_init이 ZBD+TRIM 조합 시
	 *   FIO_ASYNCIO_SYNC_TRIM 을 OR로 추가(td->io_ops->flags).
	 * 읽는 자: fio 코어 여러 곳(trim 경로, slat 기록 분기 등).
	 * 동기화: 초기화 후 불변. */
	.init			= fio_ioring_init,
	/* [한국어] init 콜백 — fio 코어 td_io_init()에서 td_io_prep/queue 전에 1회 호출.
	 * 본 구현은 ioring_data 할당, iodepth round_pow2, io_u_index/md_buf/pi_attr/
	 * iovecs/dsm 준비까지 수행. 반환 0 성공, 0 아님 실패(로그 후 잡 실패). */
	.post_init		= fio_ioring_post_init,
	/* [한국어] post_init 콜백 — io_u 풀이 채워진 뒤(iomem_alloc 완료) 호출.
	 * 본 구현은 iovec 주소 연결, io_uring_setup + mmap + (선택) fixedbufs/
	 * registerfiles 등록. 반환 0 성공. */
	.io_u_init		= fio_ioring_io_u_init,
	/* [한국어] io_u_init 콜백 — iomem_alloc 후 각 io_u에 대해 1회 호출.
	 * 본 구현은 ld->io_u_index[index]=io_u, mmap_data=md_buf 슬라이스,
	 * pi_attr 슬라이스, pi_data(pi_act=0) 할당. */
	.io_u_free		= fio_ioring_io_u_free,
	/* [한국어] io_u_free 콜백 — 잡 종료 시 각 io_u에 대해 1회 호출.
	 * 본 구현은 engine_data(pi_data) free. md_buf/pi_attr는 cleanup에서 일괄 해제. */
	.prep			= fio_ioring_prep,
	/* [한국어] prep 콜백 — td_io_prep에서 호출, io_u→SQE 변환. nonvectored 분기,
	 * fixedbufs 분기, ddir_sync(FSYNC/DATASYNC/SYNC_FILE_RANGE) 분기, TRIM→
	 * BLOCK_URING_CMD_DISCARD. 반환 항상 0. */
	.queue			= fio_ioring_queue,
	/* [한국어] queue 콜백 — td_io_queue에서 호출. SQ array[tail&mask]=io_u->index,
	 * atomic_store_release(tail+1), ld->queued++. SYNCFS/async_trim 실패 시
	 * 동기 폴백 후 FIO_Q_COMPLETED. 반환 FIO_Q_QUEUED/COMPLETED/BUSY 3분기. */
	.commit			= fio_ioring_commit,
	/* [한국어] commit 콜백 — td_io_commit에서 호출. SQPOLL이면 NEED_WAKEUP 확인
	 * 후 enter(SQ_WAKEUP), 일반이면 io_uring_enter(to_submit, 0, enter_flags)
	 * 루프로 EAGAIN/EINTR 시 cqring_reap+usleep(1) 백오프. */
	.getevents		= fio_ioring_getevents,
	/* [한국어] getevents 콜백 — td_io_getevents에서 호출. cqring_reap 루프 +
	 * io_uring_enter(0, actual_min, GETEVENTS)로 min 개수 완료 대기.
	 * 반환: 수확한 이벤트 수(>=0) 또는 -errno. */
	.event			= fio_ioring_event,
	/* [한국어] event 콜백 — getevents 반환 이후 개별 이벤트 N개에 대해 반복 호출.
	 * 본 구현은 CQE.user_data → io_u 복원, res 검사, MD 있으면 PI verify. */
	.cleanup		= fio_ioring_cleanup,
	/* [한국어] cleanup 콜백 — 잡 종료 시 호출. munmap×3 + close(ring_fd) +
	 * free 7종. TD_F_CHILD 경로는 munmap 스킵(부모와 매핑 공유). */
	.open_file		= fio_ioring_open_file,
	/* [한국어] open_file 콜백 — for_each_file 루프에서 호출. md_per_io_size 옵션
	 * 시 FS_IOC_GETLBMD_CAP ioctl로 PI 능력 조회. registerfiles이면 ld->fds에서
	 * fd 복원, 아니면 generic_open_file. */
	.close_file		= fio_ioring_close_file,
	/* [한국어] close_file 콜백 — registerfiles면 f->fd=-1만 표시(커널 내부 참조 유지),
	 * 아니면 generic_close_file. */
	.get_file_size		= generic_get_file_size,
	/* [한국어] get_file_size 콜백 — 일반 블록 디바이스/파일은 fstat/BLKGETSIZE64로
	 * 충분하므로 공용 헬퍼 사용. NVMe passthru 전용인 cmd 엔진은 별도 구현. */
	.get_zoned_model	= fio_ioring_get_zoned_model,
	/* [한국어] ZBD get_zoned_model — blkzoned_get_zoned_model로 위임
	 * (sysfs /sys/block/X/queue/zoned 읽기). */
	.report_zones		= fio_ioring_report_zones,
	/* [한국어] ZBD report_zones — blkzoned_report_zones 위임
	 * (BLKREPORTZONE ioctl, struct blk_zone 배열 반환). */
	.reset_wp		= fio_ioring_reset_wp,
	/* [한국어] ZBD reset_wp — blkzoned_reset_wp 위임 (BLKRESETZONE ioctl). */
	.get_max_open_zones	= fio_ioring_get_max_open_zones,
	/* [한국어] ZBD get_max_open_zones — blkzoned_get_max_open_zones
	 * (sysfs /sys/block/X/queue/max_open_zones 읽기). */
	.finish_zone		= fio_ioring_finish_zone,
	/* [한국어] ZBD finish_zone — blkzoned_finish_zone (BLKFINISHZONE ioctl),
	 * 존을 FULL 상태로 강제 종료. */
	.move_zone_wp		= fio_ioring_move_zone_wp,
	/* [한국어] ZBD move_zone_wp — blkzoned_move_zone_wp, 존 WP를 주어진 오프셋까지
	 * 빈 데이터로 채워 이동(시뮬레이션/정합성 유지용). */
	.options		= options,
	/* [한국어] 엔진 옵션 배열 포인터. 파서가 .name/.type/.off1 참조. */
	.option_struct_size	= sizeof(struct ioring_options),
	/* [한국어] 옵션 구조체 바이트 크기 — 파서가 calloc/freeze 시 사용. */
};

/*
 * io_uring_cmd 엔진 등록 구조체
 * 이름: "io_uring_cmd"
 *
 * NVMe passthrough용 io_uring 엔진.
 * 일반 io_uring과의 차이점:
 * - prep: fio_ioring_cmd_prep (NVMe 커맨드를 SQE에 직접 채움)
 * - post_init: fio_ioring_cmd_post_init (SQE128/CQE32 초기화)
 * - event: fio_ioring_cmd_event (NVMe 상태 코드 처리)
 * - errdetails: NVMe SCT/SC 에러 상세 정보
 * - open_file: NVMe 네임스페이스 정보 조회 포함
 * - get_file_size: NVMe 네임스페이스에서 크기 계산
 * - fdp_fetch_ruhs: NVMe FDP RUH 상태 조회
 *
 * queue, commit, getevents는 일반 io_uring과 동일한 함수 사용
 * (SQ/CQ Ring 매커니즘은 동일, SQE 내용만 다름)
 */
static struct ioengine_ops ioengine_uring_cmd = {
	.name			= "io_uring_cmd",
	/* [한국어] 엔진 이름 — --ioengine=io_uring_cmd 로 선택되는 NVMe passthru 전용
	 * 엔진. fio_ioring_init은 이 name 매칭 결과에 따라 is_uring_cmd_eng 플래그를
	 * 세팅(prep 함수 포인터 비교로 판정). */
	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전 — 코어와 동일해야 load 성공. */
	.flags			= FIO_NO_OFFLOAD | FIO_MEMALIGN | FIO_RAWIO |
					FIO_ASYNCIO_SETS_ISSUE_TIME |
					FIO_MULTI_RANGE_TRIM |
					FIO_ASYNCIO_SYNC_SYNCFS,
	/* [한국어] passthru 경로 플래그:
	 *  - FIO_NO_OFFLOAD: 동일 이유(offload 워커 금지).
	 *  - FIO_MEMALIGN: 버퍼가 페이지 정렬되어야 함(NVMe PRP/SGL DMA 요구).
	 *  - FIO_RAWIO: raw 블록/문자 디바이스만 대상(/dev/nvmeXnY 또는 ng). 파일시스템
	 *    경로 금지 — fio 코어가 파일 확장/트렁케이트 등을 수행하지 않게 함.
	 *  - FIO_ASYNCIO_SETS_ISSUE_TIME: 동일(fio_ioring_queued 가 issue_time 기록).
	 *  - FIO_MULTI_RANGE_TRIM: NVMe DSM(Dataset Management)이 한 번에 N개의
	 *    (SLBA, length) 범위를 받을 수 있음 — fio가 td->o.num_range>1 허용.
	 *  - FIO_ASYNCIO_SYNC_SYNCFS: SYNCFS는 NVMe 스펙상 전 네임스페이스 단위 강제
	 *    동기화이므로 queue()에서 동기 폴백.
	 * 미설정 비트: FIO_SYNCIO/DISKLESSIO/PIPEIO/BARRIER/UNIDIR/NODISKUTIL/
	 *   ASYNCIO_SYNC_TRIM/FAKEIO/RO — 전부 본 엔진에 해당하지 않음. TRIM도 기본
	 *   비동기(DSM) — ZBD 모드에서만 런타임 세트. */
	.init			= fio_ioring_init,
	/* [한국어] 공용 init — is_uring_cmd_eng 판정 후 NVMe 전용 추가 작업
	 * (fio_ioring_cmd_init)으로 write_opcode/cdw12_flags 결정. */
	.post_init		= fio_ioring_cmd_post_init,
	/* [한국어] NVMe 전용 post_init — SQE128 크기(2x) memset, queue_init에서
	 * SQE128/CQE32 플래그 세팅. */
	.io_u_init		= fio_ioring_io_u_init,
	/* [한국어] 공용 io_u_init. */
	.io_u_free		= fio_ioring_io_u_free,
	/* [한국어] 공용 io_u_free. */
	.prep			= fio_ioring_cmd_prep,
	/* [한국어] NVMe 전용 prep — SQE128 인덱스(<<1), NVME_URING_CMD_IO[_VEC],
	 * verify_mode=compare 시 READ→COMPARE, fio_nvme_uring_cmd_prep로 CDW0~15
	 * 패킹. */
	.queue			= fio_ioring_queue,
	/* [한국어] 공용 queue — SQ array 경로 동일. */
	.commit			= fio_ioring_commit,
	/* [한국어] 공용 commit — io_uring_enter 경로 동일. */
	.getevents		= fio_ioring_getevents,
	/* [한국어] 공용 getevents — CQ reap 경로 동일. */
	.event			= fio_ioring_cmd_event,
	/* [한국어] NVMe 전용 event — CQE32 인덱스(<<1), 양수 에러=디바이스 에러
	 * (IO_U_F_DEVICE_ERROR), pi_act=0 시 fio_nvme_pi_verify. */
	.errdetails		= fio_ioring_cmd_errdetails,
	/* [한국어] NVMe 에러 상세 문자열 반환 — "sct=0xNN; sc=0xNN" 포맷. fio
	 * --showcmd 출력에 포함. 일반 io_uring에는 없음(errno로 충분). */
	.cleanup		= fio_ioring_cleanup,
	/* [한국어] 공용 cleanup. */
	.open_file		= fio_ioring_cmd_open_file,
	/* [한국어] NVMe 전용 open_file — NVMe Identify Namespace로 lba_size/pi_type/
	 * guard_type 수집, 블록 크기 검증. */
	.close_file		= fio_ioring_cmd_close_file,
	/* [한국어] NVMe 전용 close_file — engine_data(nvme_data) free 후 공용
	 * close_file. */
	.get_file_size		= fio_ioring_cmd_get_file_size,
	/* [한국어] NVMe 전용 get_file_size — NLBA * LBA 크기로 계산(일반 fstat/ioctl
	 * 불가 — /dev/ng*은 크기 속성 없음). */
	.get_zoned_model	= fio_ioring_cmd_get_zoned_model,
	/* [한국어] ZNS get_zoned_model — fio_nvme_get_zoned_model (Identify CSI=ZNS
	 * 조회). */
	.report_zones		= fio_ioring_cmd_report_zones,
	/* [한국어] ZNS report_zones — fio_nvme_report_zones (Zone Management Receive
	 * 커맨드). */
	.reset_wp		= fio_ioring_cmd_reset_wp,
	/* [한국어] ZNS reset_wp — fio_nvme_reset_wp (Zone Management Send, RESET WRITE
	 * POINTER action). */
	.get_max_open_zones	= fio_ioring_cmd_get_max_open_zones,
	/* [한국어] ZNS get_max_open_zones — Identify Namespace ZNS(mor) 필드 조회. */
	.options		= options,
	/* [한국어] 공용 options[] — 두 엔진이 동일 옵션 세트 공유. cmd_type/write_mode/
	 * verify_mode/pi_chk 등은 uring_cmd 전용이나 파싱 테이블은 통합. */
	.option_struct_size	= sizeof(struct ioring_options),
	/* [한국어] 공용 옵션 구조체 크기. */
	.fdp_fetch_ruhs		= fio_ioring_cmd_fetch_ruhs,
	/* [한국어] FDP(Flexible Data Placement) RUH 상태 조회 콜백 — NVMe I/O
	 * Management Receive 커맨드. 일반 io_uring에는 없음(드라이버 추상화 위).
	 * 설정자: 정적. 읽는 자: fio 코어 FDP 초기화 경로. */
};

/*
 * [한국어]
 * fio_ioring_register - fio 시작 시 io_uring / io_uring_cmd 엔진 자동 등록
 *
 * @return: 없음(void).
 *
 * 배경: 이 함수는 `fio_init` 매크로(= `__attribute__((constructor))`)가 붙어
 * ELF의 `.init_array` 섹션에 포인터가 등록된다. 프로세스 로드 시점(ld.so가
 * DT_INIT/DT_INIT_ARRAY 를 순회) main() 진입 전 자동 호출되므로, fio가
 * load_ioengine("io_uring") 시점에는 이미 engine_list에 두 ops 가 들어있다.
 * 외부 .so 엔진(dlopen)이 아닌 내장 엔진은 전부 이 경로로 등록된다.
 *
 * 동작: register_ioengine()는 ioengines.c에서 flist_add_tail(&ops->list,
 * &engine_list) 만 수행(경량 단방향 등록). 실패 조건 없음 — 전역 리스트 append.
 *
 * 실행 컨텍스트: 프로세스 초기화(메인 스레드 유일, TLS/heap 준비 전), 재진입
 *   불가지만 호출 시점이 싱글 스레드이므로 안전.
 *
 * 호출 체인:
 *   ld.so → .init_array[N] → [fio_ioring_register] → register_ioengine ×2 →
 *   flist_add_tail(&ops->list, &engine_list)
 */
static void fio_init fio_ioring_register(void)
{
	register_ioengine(&ioengine_uring);
	/* [한국어] 일반 블록 io_uring 엔진 등록 — engine_list 꼬리에 ops->list 추가.
	 * 이후 load_ioengine("io_uring") 이 strcmp 매칭으로 이 ops 포인터 반환. */

	register_ioengine(&ioengine_uring_cmd);
	/* [한국어] NVMe passthru io_uring_cmd 엔진 등록 — 동일 매커니즘.
	 * 두 엔진은 동일 options[]/option_struct_size 를 공유하지만 콜백 테이블(prep/
	 * event/post_init/open_file 등)은 다르다. */
}

/*
 * [한국어]
 * fio_ioring_unregister - 프로세스 종료 시 ioengine_ops 등록 해제
 *
 * @return: 없음(void).
 *
 * 배경: `fio_exit` 매크로(= `__attribute__((destructor))`)가 붙어 ELF의
 * `.fini_array` 에 등록. atexit(3) 체인 처리 후 ld.so 가 DT_FINI_ARRAY 를
 * 역순 호출한다. 일반 정적 실행에서는 꼭 필요하지 않으나(프로세스 종료 시
 * 커널이 메모리 회수), dlopen으로 동적 로드된 외부 엔진 플러그인과의
 * 일관성을 위해 쌍을 맞춘다. 또한 fio가 테스트 환경에서 dlopen/dlclose 로
 * 재적재되는 경우 engine_list에 stale 포인터가 남지 않도록 방어.
 *
 * 동작: unregister_ioengine()는 flist_del_init(&ops->list) — O(1), 락 없음.
 *   재적재 후에도 list 헤드 상태가 0으로 초기화되어 안전.
 *
 * 실행 컨텍스트: 프로세스 종료 경로(메인 스레드 유일). 시그널 핸들러에서
 *   호출되지 않음.
 *
 * 호출 체인:
 *   exit(3) / main return → atexit 체인 → ld.so fini_array →
 *   [fio_ioring_unregister] → unregister_ioengine ×2 → flist_del_init
 */
static void fio_exit fio_ioring_unregister(void)
{
	unregister_ioengine(&ioengine_uring);
	/* [한국어] io_uring ops를 engine_list에서 제거. flist_del_init로 stale 링크
	 * 방지 — 재적재 시 register_ioengine이 또다시 flist_add_tail 하더라도 안전. */

	unregister_ioengine(&ioengine_uring_cmd);
	/* [한국어] io_uring_cmd ops 제거. 두 ops는 별도 list 노드이므로 순서 무관. */
}
#endif /* ARCH_HAVE_IOURING */
/* [한국어] 상단 `#ifdef ARCH_HAVE_IOURING` 의 짝. 미지원 아키텍처에서는 본 파일의
 * 모든 코드가 제거되고 빈 object가 만들어져 링크 에러 없이 엔진만 빠진다. */
