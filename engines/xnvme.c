/*
 * [한국어 설명] xNVMe 기반 교차 백엔드 NVMe I/O 엔진 구현 (xnvme.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio I/O 엔진 플러그인 중 "xnvme" 엔진의 전체 구현을 담는다. xNVMe
 * (github.com/OpenMPDK/xNVMe)는 다양한 NVMe 접근 백엔드 — Linux 커널 NVMe 블록
 * 드라이버, io_uring(일반 블록 SQE), io_uring_cmd(NVMe passthru SQE), libaio,
 * SPDK user-space NVMe 드라이버(UIO/VFIO 기반 폴 모드), FreeBSD NVMe, POSIX
 * pread/pwrite, VFIO, thrpool(스레드풀 에뮬레이션), nil(더미) 등을 단일 C API
 * 로 추상화한 라이브러리이다. 이 엔진은 libxnvme C API를 사용하여 NVMe
 * 네임스페이스에 대한 Read/Write 를 비동기로 제출·수확(submit/reap)하며, Zoned
 * Namespace(ZNS) 관리(Report Zones / Zone Mgmt Send RESET), Flexible Data
 * Placement(FDP) Reclaim Unit Handle Status 조회, NVMe Protection Information
 * (PI, Type1/2/3, 16B/64B GUARD) 생성·검증, Application Tag/Reference Tag
 * 관리, 확장 LBA(데이터+PI 결합) 지원, 분리 메타데이터 버퍼 경로까지 제공한다.
 * 즉, 하나의 `fio --ioengine=xnvme` 로 "어떤 NVMe 접근 경로를 쓸지"를 런타임
 * 옵션(--xnvme_async=io_uring_cmd, --xnvme_sync=nvme, --xnvme_be=spdk 등)으로
 * 선택하게 한다. 유사 엔진인 engines/nvme.c(io_uring_cmd 전용)나
 * engines/sg.c(SCSI generic ioctl), engines/io_uring.c(io_uring 전용)와 기능이
 * 일부 중첩되지만, 이 엔진의 고유 가치는 "하나의 코드 경로로 6~7종의 백엔드를
 * 스위치"하여 성능/기능 비교 및 이식성을 얻는 점이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 호출 체인에서 이 파일의 콜백들은 backend.c → ioengines.c 가 관리하는
 * struct ioengine_ops 를 통해 잡 스레드(job thread) 컨텍스트에서 호출된다.
 *   main() → fio_backend() → thread_main() 잡 스레드
 *     → td_io_init() → .init = xnvme_fioe_init()       (잡 시작 시 1회)
 *     → td_io_open_file() → .open_file = xnvme_fioe_open()
 *     → get_io_u() → .io_u_init = xnvme_fioe_io_u_init()
 *     → td_io_queue() → .queue = xnvme_fioe_queue()    (I/O 제출, 비동기)
 *     → td_io_commit() → (엔진이 commit 콜백 없음 — queue 가 즉시 커밋)
 *     → td_io_getevents() → .getevents = xnvme_fioe_getevents() (완료 수확)
 *     → .event = xnvme_fioe_event()                    (개별 완료 추출)
 *     → td_io_close_file() → .close_file = xnvme_fioe_close()
 *     → .cleanup = xnvme_fioe_cleanup()                (잡 종료 시 1회)
 * ZBD/FDP 경로는 엔진 init 이전에도 호출될 수 있으므로(.get_zoned_model,
 * .report_zones, .reset_wp, .fdp_fetch_ruhs, .get_max_open_zones,
 * .get_file_size) 각 콜백이 "io_ops_data 있으면 재사용 / 없으면 임시
 * xnvme_dev_open/close" 양쪽 경로를 모두 지원한다. 실행 컨텍스트는 항상 호스트
 * 유저스페이스의 fio 잡 스레드이며, 옵션 --thread=1 이 강제되어 동일 프로세스
 * 내 스레드 모델로만 동작한다 (멀티프로세스 fork 모델 금지: libxnvme 의
 * 백엔드 레지스트리·PCI enumeration·SPDK EAL 상태가 전역이라 fork 후 공유
 * 불가). 실제 I/O 시스템 콜은 선택된 백엔드가 결정 — 예: io_uring_cmd 는
 * io_uring_enter(2) + IORING_OP_URING_CMD + NVMe_URING_CMD_IO, libaio 는
 * io_submit(2)/io_getevents(2), SPDK 는 유저스페이스 폴링(시스템 콜 0회).
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): fio 코어(ioengines.c, backend.c, io_u.c, zbd.c, dataplacement.c).
 *   io_u 하나가 들어오면 xnvme_fioe_queue() 에서 xNVMe 명령 컨텍스트(xnvme_cmd_ctx)
 *   로 변환 후 xnvme_queue_*/xnvme_cmd_pass*() 로 제출한다. zbd.c 는
 *   get_zoned_model/report_zones/reset_wp 를 호출해 "Zoned Block Device" 추상을
 *   구현하며, dataplacement.c 는 fdp_fetch_ruhs 로 RUH(Reclaim Unit Handle) 리스트를
 *   얻어 FDP dtype/dspec 을 설정한다.
 * - 하위(피호출자): libxnvme API — 장치 관리(xnvme_dev_open/close/get_geo/get_ssw/
 *   get_nsid/get_ns_css), 큐(xnvme_queue_init/term/poke/put_cmd_ctx/get_cmd_ctx/
 *   set_cb), 버퍼(xnvme_buf_alloc/free, xnvme_buf_virt_free), NVM 명령
 *   (xnvme_cmd_pass/passv, xnvme_cmd_ctx_from_dev, xnvme_cmd_ctx_cpl_status,
 *   xnvme_cmd_ctx_pr), ZNS(xnvme_znd_report_from_dev, xnvme_znd_mgmt_send,
 *   xnvme_znd_dev_get_lbafe), FDP(xnvme_nvm_mgmt_recv), PI(xnvme_pi_ctx_init,
 *   xnvme_pi_generate, xnvme_pi_verify, xnvme_pi_size). libxnvme 는 내부에서
 *   선택된 백엔드의 시스템 콜/ioctl(NVME_IOCTL_IO_CMD, io_uring_enter,
 *   SPDK NVMe doorbell write 등)을 수행한다.
 * - 공유 자료구조: struct thread_data 의 io_ops_data 필드에 struct xnvme_fioe_data
 *   포인터를 저장한다. io_u 는 mmap_data 에 xd 포인터를(cb_pool 역참조용),
 *   engine_data 에 struct xnvme_fioe_request 포인터를(PI 컨텍스트/메타 버퍼 보관)
 *   연결한다. 완료 콜백 cb_pool 의 cb_arg 에는 io_u 자체 포인터가 저장되어,
 *   xnvme_queue_poke 가 완료 항목별로 cb_pool(ctx, io_u) 를 호출하면
 *   io_u->mmap_data 에서 xd 를, io_u->engine_data 에서 fio_req 를 복원한다.
 * - 데이터 흐름: fio io_u (offset, buflen, xfer_buf, ddir) → 이 엔진이 LBA(SLBA)
 *   /NLB(Number of Logical Blocks) 로 변환(2의 거듭제곱 LBA 는 >>ssw, 확장 LBA
 *   는 /lba_nbytes) → xnvme_cmd_ctx.cmd.nvm 필드(opcode, nsid, slba, nlb,
 *   prinfo, ilbrt, lbat, lbatm) 채움 → PI 필요 시 xnvme_pi_generate 로 데이터
 *   +메타 버퍼에 GUARD/REFTAG/APPTAG 삽입 → libxnvme 로 제출 → 백엔드가 커널/
 *   디바이스로 전달 → 완료 시 libxnvme 내부 완료 큐에 적재 → xnvme_queue_poke
 *   가 꺼내 cb_pool 호출 → cb_pool 이 cpl.status 검사/READ 시 PI verify/
 *   xd->iocq[] 로 적재 → getevents() 가 라운드로빈으로 여러 파일을 poke 후
 *   min 도달 시 반환 → fio 가 event(idx) 로 개별 io_u 회수 → put_io_u() 로 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - xnvme_fioe_init()     : 잡 스레드 시작 시 xd 할당(flexible array + iocq +
 *                            옵션별 iovec/md_iovec 풀), --thread=1 강제, 파일별
 *                            _dev_open() 호출.
 * - xnvme_fioe_cleanup()  : xd 와 iocq/iovec/md_iovec 해제, 파일별 _dev_close()
 *                            호출 (g_serialize 직렬화).
 * - xnvme_fioe_queue()    : io_u → NVMe Read/Write 명령 변환 후 비동기 제출.
 *                            FIO_Q_QUEUED/BUSY/COMPLETED 반환. PI/FDP 필드 세팅.
 * - xnvme_fioe_getevents(): 여러 파일의 xNVMe 큐를 라운드로빈으로 poke 하여 완료를
 *                            수확, xd->iocq 에 채움. min 도달 시 반환.
 * - cb_pool()             : xNVMe 큐의 명령 완료 콜백. cpl.status 검사 / PI 검증
 *                            (READ 시) / iocq 적재 / cmd_ctx 반환.
 * - xnvme_fioe_report_zones()/reset_wp(): ZNS Report Zones 로그 변환 / Zone Mgmt
 *                            Send RESET (zbd.c 연동).
 * - xnvme_fioe_fetch_ruhs(): NVMe IO Management Receive 로 FDP RUHS 로그 조회
 *                            (dataplacement.c 연동).
 * - xnvme_fioe_get_zoned_model/get_max_open_zones/get_file_size: init 이전 경로
 *                            (자체 open/close) + init 이후 경로 공존.
 * - xnvme_opts_from_fioe(): fio 옵션 → xnvme_opts 변환 공통 헬퍼.
 * - _dev_open()/_dev_close(): 파일 단위 xNVMe 장치·큐 초기화·종료.
 * - struct xnvme_fioe_fwrap : fio_file 1개당 xNVMe 장치 핸들/큐/지오메트리 래퍼
 *                              (64B 캐시라인 정렬 STATIC_ASSERT).
 * - struct xnvme_fioe_data  : 잡당 런타임 상태 (iocq, files[] flexible array,
 *                              iovec/md_iovec 풀, 라운드로빈 커서).
 * - struct xnvme_fioe_request : io_u 당 PI 컨텍스트/메타데이터 버퍼.
 * - struct xnvme_fioe_options : --xnvme_* 커맨드라인 옵션 값의 저장소
 *                                (td->eo 로 접근).
 */

/*
 * [한국어 설명] 아래 주석은 원본 fio 프로젝트에서 유지해야 하는 헤더 주석 및
 * SPDX 라이선스 식별자이다. 엔진의 출처(http://xnvme.io)와 Apache-2.0 라이선스를
 * 명시한다. 수정하지 않는다.
 */
/*
 * fio xNVMe IO Engine
 *
 * IO engine using the xNVMe C API.
 *
 * See: http://xnvme.io/
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* [한국어] calloc/free/abs/usleep(구현자에 따라) 등 표준 메모리·수치·유틸.
 * xd/iocq/iovec/md_iovec/fio_req 동적 할당 및 cb_pool/queue 에서 abs(err) 로
 * libxnvme 의 음수 에러코드를 양수 errno 로 변환하는 데 사용. */
#include <stdlib.h>
/* [한국어] assert() 매크로. 디버그 빌드에서 내부 불변식(event 인덱스 범위,
 * fwrap->dev/geo NULL 여부, 미지원 ddir 도달 금지)을 검증. NDEBUG 빌드에서는
 * 컴파일 시 제거되므로 성능 크리티컬 경로에도 안전하게 배치. */
#include <assert.h>
/* [한국어] xNVMe 공개 헤더. xnvme_dev/queue/cmd_ctx/znd/pi 등 모든 타입·함수
 * 선언을 한 파일에서 제공. libxnvme.so 와의 링크는 configure 단계의
 * CONFIG_LIBXNVME 매크로/Makefile 로 제어됨. 이 헤더는 백엔드별 내부 구현을
 * 노출하지 않고 불투명 포인터(xnvme_dev*, xnvme_queue*)만 제공한다. */
#include <libxnvme.h>
/* [한국어] fio 코어 헤더: thread_data, fio_file, io_u, DDIR_*, FIO_Q_*,
 * fio_ro_check, fio_file_* 등 엔진 계약 정의. register_ioengine/
 * unregister_ioengine 함수 프로토타입과 fio_init/fio_exit 매크로도 제공. */
#include "fio.h"
/* [한국어] VERIFY_NONE/VERIFY_MD5 등 verify 모드 상수 — 확장 LBA + PI +
 * verify 조합 충돌 검사(_verify_options)에 사용. fio 의 end-to-end 검증 경로와
 * NVMe PI 의 end-to-end 보호가 서로 "데이터 바디"를 덮어쓰려 해서 배타 관계. */
#include "verify.h"
/* [한국어] zbd_zone/zbd_zoned_model/ZBD_ZONE_COND_*/ZBD_ZONE_TYPE_SWR/ZBD_HOST_MANAGED
 * 등 존 블록 디바이스 추상 타입. NVMe ZNS 의 zs/zt 코드를 이 공용 타입으로 매핑하여
 * zbd.c 가 백엔드 독립적으로 존 상태를 다루게 한다. */
#include "zbd_types.h"
/* [한국어] fio_ruhs_info — FDP Reclaim Unit Handle Status 전달 구조체
 * (nr_ruhs, plis[]). dataplacement.c 가 잡 시작 시 이 콜백으로 RUH 목록을
 * 수집해서 WRITE 명령의 dtype/dspec 필드를 라운드로빈 할당한다. */
#include "dataplacement.h"
/* [한국어] FIO_OPT_G_XNVME 옵션 그룹 식별자 정의. `fio --enghelp=xnvme` 로 이
 * 엔진 전용 옵션만 필터링 출력하는 분류 태그로 사용. FIO_OPT_C_ENGINE 은
 * 엔진 카테고리(최상위), FIO_OPT_G_XNVME 는 이 엔진 서브그룹. */
#include "optgroup.h"

/*
 * [한국어] 전역 직렬화 뮤텍스.
 * 역할: libxnvme 의 xnvme_dev_open()/close() 와 일부 큐/핸들 조작은 프로세스 전역
 *       상태(백엔드 레지스트리, PCI enumeration 등)를 건드리므로, 여러 잡 스레드가
 *       동시에 진입하지 않도록 직렬화해야 한다.
 * 설정자: _dev_open/_dev_close, get_max_open_zones, get_zoned_model, report_zones,
 *        reset_wp, fetch_ruhs, get_file_size 등 장치 open/close 경로가 사용.
 * 값 범위: PTHREAD_MUTEX_INITIALIZER 정적 초기화 (lazy, recursive 아님).
 * 동기화: 모든 잡 스레드가 공유하는 프로세스 단일 뮤텍스. 경합 시 직렬화 비용 발생.
 */
static pthread_mutex_t g_serialize = PTHREAD_MUTEX_INITIALIZER;

/*
 * [한국어] 파일별 xNVMe 장치 상태 래퍼(per-fio_file wrapper).
 * fio 가 관리하는 fio_file 1개당 1개씩 xd->files[] 배열에 저장되어, 해당 파일에
 * 대한 xNVMe 장치 핸들, 큐, 지오메트리, LBA 계산 파라미터를 한 곳에 모아 둔다.
 * sizeof 는 XNVME_STATIC_ASSERT 로 64바이트에 정렬(캐시라인 크기와 일치)되어
 * 배열 접근 시 false sharing 완화 및 예측 가능한 레이아웃을 보장한다.
 */
struct xnvme_fioe_fwrap {
	struct fio_file *fio_file;
	/* [한국어] 이 래퍼가 대응하는 fio 파일 객체 포인터.
	 * 설정자: _dev_open() 이 성공 시 f 를 저장.
	 * 읽는 자: open()/close() 에서 "xd->files[fileno].fio_file == f" 검증에 사용.
	 * 값 범위: 유효한 fio_file* (잡 수명 동안 유지) 또는 NULL(미할당 슬롯).
	 * 동기화: 단일 잡 스레드에서만 접근하므로 별도 락 불필요. */

	struct xnvme_dev *dev;
	/* [한국어] xNVMe 장치 핸들(불투명 구조체 — libxnvme 내부 관리).
	 * 설정자: _dev_open() 의 xnvme_dev_open() 호출로 획득.
	 * 읽는 자: queue/report/reset 경로 등 거의 모든 콜백에서 nsid/geo 조회에 사용.
	 * 값 범위: 유효 포인터 또는 NULL(미오픈/실패 상태).
	 * 동기화: 핸들의 라이프사이클(open/close)은 g_serialize 뮤텍스로 보호. */

	const struct xnvme_geo *geo;
	/* [한국어] xNVMe 장치 지오메트리 (lba_nbytes, tbytes, nbytes_oob, pi_type,
	 * pi_format, lba_extended, nzone, nsect 등). const 포인터 = 라이브러리 소유.
	 * 설정자: _dev_open() 에서 xnvme_dev_get_geo() 로 획득.
	 * 읽는 자: 거의 모든 I/O 경로에서 LBA 변환, PI 설정, ZNS 계산에 참조.
	 * 값 범위: dev 수명과 동일. dev 가 유효한 동안만 접근 가능. */

	struct xnvme_queue *queue;
	/* [한국어] 이 장치에 대한 비동기 I/O 큐 핸들. iodepth 만큼의 동시 명령을 담는다.
	 * 설정자: _dev_open() 의 xnvme_queue_init() 로 생성.
	 * 읽는 자: queue() 가 cmd_ctx 를 얻어 제출, getevents() 가 poke 로 완료 수확.
	 * 값 범위: 유효 포인터 또는 NULL(초기화 전).
	 * 동기화: 잡 스레드 전용. 완료 콜백(cb_pool)도 동일 스레드에서 실행. */

	uint32_t ssw;
	/* [한국어] Sector Size Shift Width — log2(sector_size). 2의 거듭제곱 LBA 에서
	 * 오프셋→LBA 변환을 시프트 한 번으로 수행하기 위한 프리컴퓨트 값.
	 * 설정자: _dev_open() 의 xnvme_dev_get_ssw() 반환값.
	 * 읽는 자: queue()/reset_wp()/report_zones() 에서 offset>>ssw 계산. */

	uint32_t lba_nbytes;
	/* [한국어] LBA 한 개에 해당하는 데이터 영역 바이트 수(확장 LBA 의 경우 PI 포함).
	 * 설정자: _dev_open() 에서 geo->lba_nbytes 복사. PI 인서션 시 감산 보정됨.
	 * 읽는 자: 비 2의 거듭제곱 LBA 에서 offset/lba_nbytes 나눗셈에 사용.
	 * 값 범위: 일반적으로 512, 4096, 4104(확장), 4168 등. */

	uint32_t md_nbytes;
	/* [한국어] LBA 당 메타데이터(OOB) 바이트 수 (분리 버퍼 경로).
	 * 설정자: _dev_open() 에서 geo->nbytes_oob 복사. PI 인서션 시 0 으로 보정.
	 * 읽는 자: queue() 가 xnvme_cmd_pass/passv 의 md 인자 크기 계산에 사용. */

	uint32_t lba_pow2;
	/* [한국어] LBA 크기가 2의 거듭제곱인지 플래그(1) 또는 확장 LBA 인지(0).
	 * 설정자: _dev_open() 에서 geo->lba_extended 기준으로 판별.
	 * 읽는 자: queue() 에서 시프트 vs 나눗셈 분기, _verify_options 에서 검증. */

	uint8_t _pad[16];
	/* [한국어] 구조체 크기를 64바이트(캐시라인)로 맞추기 위한 패딩.
	 * 설정자/읽는 자: 없음(사용 금지). 단순 정렬용.
	 * 동기화: false sharing 완화 목적이므로 주변 필드와 함께 배치됨. */
};
/* [한국어] 구조체 크기가 정확히 64바이트(한 캐시라인)인지 컴파일 타임 검증. */
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fioe_fwrap) == 64, "Incorrect size")

/*
 * [한국어] 잡(thread_data) 당 한 개 할당되는 xNVMe 엔진 런타임 상태.
 * flexible array member(files[]) 로 파일 수만큼 xnvme_fioe_fwrap 을 뒤따라 잡는다.
 * td->io_ops_data 에 저장되어 모든 콜백에서 참조된다. 크기 64바이트 정렬.
 */
struct xnvme_fioe_data {
	/* I/O completion queue */
	struct io_u **iocq;
	/* [한국어] 완료된 io_u 포인터들의 배열(= completion queue).
	 * 설정자: cb_pool() 가 xd->iocq[xd->completed++] = io_u 로 채움.
	 * 읽는 자: xnvme_fioe_event() 가 인덱스로 꺼내 fio 에 반환.
	 * 값 범위: iodepth 크기의 배열. 각 슬롯은 유효 io_u* 또는 stale.
	 * 동기화: 단일 잡 스레드 내에서 완료 콜백이 실행되므로 락 불필요. */

	/* # of iocq entries; incremented via getevents()/cb_pool() */
	uint64_t completed;
	/* [한국어] 현재 getevents 라운드에서 iocq 에 적재된 완료 개수.
	 * 설정자: getevents() 진입 시 0 리셋, cb_pool 에서 ++.
	 * 읽는 자: event() 의 인덱스 범위 검증, getevents() 반환값.
	 * 동기화: 잡 스레드 전용. */

	/*
	 *  # of errors; incremented when observed on completion via
	 *  getevents()/cb_pool()
	 */
	uint64_t ecount;
	/* [한국어] 완료 경로에서 관찰된 에러 누적 카운터 (통계/디버그용).
	 * 설정자: cb_pool 에서 cpl status nonzero 또는 PI verify 실패 시 ++.
	 * 읽는 자: 현재 내부 통계용이며 fio 외부로 직접 노출되지 않음. */

	/* Controller which device/file to select */
	int32_t prev;
	/* [한국어] 이전 getevents 라운드에서 완료를 본 마지막 파일 인덱스(라운드로빈 힌트).
	 * 설정자/읽는 자: getevents() 가 공평성/지역성을 위해 prev+1 부터 재개. */
	int32_t cur;
	/* [한국어] 현재 getevents 라운드에서 탐색 중인 파일 인덱스.
	 * 설정자/읽는 자: getevents() 의 루프 커서. */

	/* Number of devices/files for which open() has been called */
	int64_t nopen;
	/* [한국어] .open_file 이 호출된 누적 수 — .close_file 에서 감소.
	 * 설정자: xnvme_fioe_open() ++, xnvme_fioe_close() --.
	 * 읽는 자: dprint 로 디버그 로그. */

	/* Number of devices/files allocated in files[] */
	uint64_t nallocated;
	/* [한국어] files[] 에 실제로 _dev_open() 이 성공한 파일 수.
	 * 설정자: init() 가 파일 루프를 돌며 증가.
	 * 읽는 자: cleanup/getevents 에서 순회 상한으로 사용. */

	struct iovec *iovec;
	/* [한국어] --xnvme_iovec 옵션 시, io_u 별 데이터 iovec 풀 (iodepth 크기).
	 * 설정자: init() 에서 calloc, queue() 에서 io_u->index 슬롯에 buf/len 채움.
	 * 읽는 자: xnvme_cmd_passv 호출에서 포인터로 전달. */

	struct iovec *md_iovec;
	/* [한국어] 메타데이터 분리 버퍼용 iovec 풀 (md_per_io_size+iovec 동시 지정 시).
	 * 설정자: init() 에서 조건부 calloc, queue() 에서 md_buf/md_nbytes*(nlb+1).
	 * 읽는 자: xnvme_cmd_passv 의 md iovec 인자. */

	struct xnvme_fioe_fwrap files[];
	/* [한국어] 파일별 래퍼 배열. flexible array member 로 calloc 시
	 * sizeof(xd) + nr_files*sizeof(fwrap) 크기로 한 덩어리 할당.
	 * 설정자: _dev_open() 이 files[f->fileno] 를 채움.
	 * 읽는 자: 모든 I/O 경로. */
};
/* [한국어] 헤더 부분(files[] 제외) 크기가 정확히 64바이트인지 컴파일 타임 검증. */
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fioe_data) == 64, "Incorrect size")

/*
 * [한국어] io_u 1개당 붙는 요청 상태. io_u->engine_data 로 연결.
 * io_u_init 에서 calloc 되고 io_u_free 에서 해제된다.
 */
struct xnvme_fioe_request {
	/* Context for NVMe PI */
	struct xnvme_pi_ctx pi_ctx;
	/* [한국어] NVMe Protection Information 생성/검증 컨텍스트.
	 * 설정자: queue() 의 xnvme_pi_ctx_init(), WRITE 이면 pi_generate.
	 * 읽는 자: cb_pool() 의 xnvme_pi_verify (READ 완료 시 pi_act=0).
	 * 값 범위: libxnvme 불투명 상태 — 직접 필드 접근 금지. */

	/* Separate metadata buffer pointer */
	void *md_buf;
	/* [한국어] md_per_io_size 옵션 사용 시 별도 할당된 메타데이터 버퍼.
	 * 설정자: io_u_init() 의 xnvme_buf_alloc() (DMA-능 메모리).
	 * 읽는 자: queue() 의 xnvme_cmd_pass/passv 의 mbuf 인자, PI verify/generate.
	 * 동기화: io_u 에 전속(1:1). */
};

/*
 * [한국어] --xnvme_* 커맨드라인 옵션을 매핑할 구조체.
 * fio 의 fio_option.off1 이 이 구조체의 필드 오프셋을 가리켜, 옵션 파서가 값을
 * 저장한다. thread_data 의 eo(engine options) 필드에 연결되며 이 엔진의 모든
 * 콜백에서 td->eo 로 접근한다. 라이프사이클: 파서가 init 이전에 채우고,
 * cleanup 이후까지 유지(문자열 포인터 포함). 필드 순서는 ABI 가 아니므로
 * 재배치 가능하지만 padding 은 반드시 선두여야 한다.
 */
struct xnvme_fioe_options {
	void *padding;
	/* [한국어] fio 옵션 프레임워크가 요구하는 선두 패딩(옵션 파서 호환성).
	 * 설정자: 없음(파서가 건드리지 않음).
	 * 읽는 자: 없음.
	 * 값 범위: 항상 NULL.
	 * 동기화: 불변. fio 옵션 파서의 포인터 정렬·구버전 호환 관례 충족용. */

	unsigned int hipri;
	/* [한국어] --hipri : polled completion(고우선순위) 사용 여부.
	 * 설정자: 옵션 파서가 FIO_OPT_STR_SET 로 0/1 저장.
	 * 읽는 자: xnvme_opts_from_fioe 가 opts.poll_io 에 매핑.
	 * 값 범위: 0(비활성)/1(활성). io_uring 백엔드에서 IORING_SETUP_IOPOLL 활성.
	 * 동기화: init 이전 설정, 이후 read-only. */

	unsigned int sqpoll_thread;
	/* [한국어] --sqthread_poll : io_uring 커널 SQ 폴링 스레드 사용 여부.
	 * 설정자: 옵션 파서.
	 * 읽는 자: xnvme_opts_from_fioe 가 opts.poll_sq 에 매핑.
	 * 값 범위: 0/1. 1 이면 IORING_SETUP_SQPOLL 로 커널이 SQ 를 폴링(도어벨
	 *          쓰기 절감, CPU 한 개 커널 스레드 점유).
	 * 동기화: init 이전 설정, 이후 read-only. */

	unsigned int xnvme_dev_nsid;
	/* [한국어] --xnvme_dev_nsid : user-space NVMe 드라이버(SPDK 등)에서 선택할
	 *         Namespace Identifier. 커널 백엔드(/dev/nvme0n1)에서는 장치 이름으로
	 *         NSID 가 이미 결정되어 불필요, SPDK 는 컨트롤러 당 여러 NS 중
	 *         선택해야 하므로 필요.
	 * 설정자: 옵션 파서(FIO_OPT_INT).
	 * 읽는 자: xnvme_opts_from_fioe → opts.nsid.
	 * 값 범위: 0=미지정(백엔드 기본), 1..N=유효 NSID. */

	unsigned int xnvme_iovec;
	/* [한국어] --xnvme_iovec : 벡터드 I/O(xnvme_cmd_passv) 경로 강제.
	 * 설정자: 옵션 파서(FIO_OPT_STR_SET).
	 * 읽는 자: xnvme_fioe_queue 에서 분기(passv vs pass), init 에서 iovec 풀 할당 조건.
	 * 값 범위: 0(단일 버퍼)/1(iovec). 1 이면 iodepth 크기의 iovec 배열 할당. */

	unsigned int md_per_io_size;
	/* [한국어] --md_per_io_size : io_u 당 별도 메타데이터 버퍼 크기(바이트).
	 *         확장 LBA 가 아닌 분리 메타 경로에서 사용.
	 * 설정자: 옵션 파서(FIO_OPT_INT).
	 * 읽는 자: io_u_init 에서 xnvme_buf_alloc 크기, _verify_options 에서 최소값 검증.
	 * 값 범위: 0=미사용, >0=바이트 수. 일반적으로 lba_count * lba_md_size 이상. */

	unsigned int pi_act;
	/* [한국어] --pi_act : PI Action 비트. NVMe 스펙 PRINFO bit3 에 매핑.
	 *         1 = 컨트롤러가 PI 삽입(WRITE)/제거(READ), 호스트는 데이터만 다룸.
	 *         0 = 호스트가 PI 생성/검증 (xnvme_pi_generate/verify 사용).
	 * 설정자: 옵션 파서(FIO_OPT_BOOL, 기본 1).
	 * 읽는 자: queue 에서 pi_ctx_init/generate 호출 조건, cb_pool 에서 verify 조건.
	 * 값 범위: 0/1. */

	unsigned int apptag;
	/* [한국어] --apptag : PI 의 Application Tag 필드 값(16비트).
	 *         PI Type1/2/3 모두에서 의미 있으며, NVMe 가 쓰기 시 저장/읽기 시 반환.
	 * 설정자: 옵션 파서(FIO_OPT_INT, 기본 0x1234).
	 * 읽는 자: queue 에서 ctx->cmd.nvm.lbat 필드에 복사, pi_ctx_init 에 전달.
	 * 값 범위: 0x0000..0xFFFF. */

	unsigned int apptag_mask;
	/* [한국어] --apptag_mask : Application Tag 체크용 마스크(16비트).
	 *         컨트롤러가 비교 시 mask 비트 1 인 위치만 확인.
	 * 설정자: 옵션 파서(FIO_OPT_INT, 기본 0xffff=전체 체크).
	 * 읽는 자: queue 에서 ctx->cmd.nvm.lbatm, pi_ctx_init.
	 * 값 범위: 0x0000..0xFFFF. */

	unsigned int prchk;
	/* [한국어] --pi_chk 콜백(str_pi_chk_cb)이 파싱하여 채우는 플래그 비트 OR.
	 *         GUARD/REFTAG/APPTAG 체크 중 활성화할 것들.
	 * 설정자: str_pi_chk_cb 가 XNVME_PI_FLAGS_*_CHECK 비트 조합으로 설정.
	 * 읽는 자: queue 에서 ctx->cmd.nvm.prinfo 하위 비트, pi_ctx_init 플래그.
	 * 값 범위: 0..(GUARD|REFTAG|APPTAG). off1 없이 .cb 로만 채워짐. */

	char *xnvme_be;
	/* [한국어] --xnvme_be : 백엔드 선택 문자열 (예: "spdk", "linux", "fbsd").
	 *         libxnvme 가 이 문자열로 xnvme_be_* 구조체 레지스트리에서 선택.
	 * 설정자: 옵션 파서(FIO_OPT_STR_STORE) — strdup 한 포인터.
	 * 읽는 자: xnvme_opts_from_fioe → opts.be.
	 * 값 범위: NULL(기본 자동선택) 또는 등록된 백엔드 이름. */

	char *xnvme_mem;
	/* [한국어] --xnvme_mem : DMA 메모리 할당자 선택 문자열 ("hugepage", "posix", ...).
	 *         백엔드별로 지원되는 할당자가 다르며, SPDK 는 DPDK hugepage 기본.
	 * 설정자: 옵션 파서.
	 * 읽는 자: xnvme_opts_from_fioe → opts.mem.
	 * 값 범위: NULL 또는 지원 이름. */

	char *xnvme_async;
	/* [한국어] --xnvme_async : 비동기 인터페이스 선택
	 *         (emu/thrpool/io_uring/io_uring_cmd/libaio/posix/vfio/nil).
	 *         io_uring_cmd 가 NVMe passthru(SQE opcode IORING_OP_URING_CMD) 로 NVMe
	 *         명령을 커널 블록 계층 우회하여 전송. nil 은 실제 I/O 없이 즉시 완료.
	 * 설정자: 옵션 파서.
	 * 읽는 자: xnvme_opts_from_fioe → opts.async.
	 * 값 범위: NULL(기본) 또는 지원 이름. */

	char *xnvme_sync;
	/* [한국어] --xnvme_sync : 동기 인터페이스 선택 (nvme/psync/block).
	 *         이 엔진 자체는 비동기 제출이지만, 백엔드에 따라 내부에서 동기
	 *         경로가 필요한 경우(admin 명령, 초기화 질의) 사용.
	 * 설정자: 옵션 파서.
	 * 읽는 자: xnvme_opts_from_fioe → opts.sync.
	 * 값 범위: NULL 또는 지원 이름. */

	char *xnvme_admin;
	/* [한국어] --xnvme_admin : admin 명령 인터페이스 선택 (nvme/block).
	 *         Identify/Log Page 등 admin 큐 명령을 어느 채널로 보낼지.
	 * 설정자: 옵션 파서.
	 * 읽는 자: xnvme_opts_from_fioe → opts.admin.
	 * 값 범위: NULL 또는 지원 이름. */

	char *xnvme_dev_subnqn;
	/* [한국어] --xnvme_dev_subnqn : NVMe-oF(Over Fabrics) subsystem NQN
	 *         (예: nqn.2014-08.org.nvmexpress:uuid:...). Fabrics 연결 시 필요.
	 * 설정자: 옵션 파서.
	 * 읽는 자: xnvme_opts_from_fioe → opts.subnqn.
	 * 값 범위: NULL(로컬 PCIe) 또는 유효 NQN 문자열. */
};

/*
 * [한국어]
 * str_pi_chk_cb - --pi_chk 옵션 파서 콜백.
 *
 * @data: thread_options 내부의 struct xnvme_fioe_options* (fio 옵션 파서가 전달).
 * @str:  사용자 입력 문자열. "GUARD,REFTAG,APPTAG" 조합.
 * @return: 항상 0 (성공). 실패 시그널 없음.
 *
 * PI(Protection Information) 체크 대상은 3가지 플래그의 OR 조합으로 표현되므로,
 * 문자열에서 부분 매칭 하여 해당 비트를 설정한다. queue() 의 prinfo 필드에 반영된다.
 * 호출 체인: fio 옵션 파서 → str_pi_chk_cb → o->prchk 비트 설정.
 */
static int str_pi_chk_cb(void *data, const char *str)
{
	/* [한국어] 옵션 구조체로 캐스팅 (파서는 void* 로만 전달). */
	struct xnvme_fioe_options *o = data;

	/* [한국어] "GUARD" 포함 시 GUARD 체크 비트 설정(= 필드 대입; OR 아님에 주의). */
	if (strstr(str, "GUARD") != NULL)
		o->prchk = XNVME_PI_FLAGS_GUARD_CHECK;
	/* [한국어] "REFTAG" 포함 시 REFTAG 체크 비트 OR 추가. */
	if (strstr(str, "REFTAG") != NULL)
		o->prchk |= XNVME_PI_FLAGS_REFTAG_CHECK;
	/* [한국어] "APPTAG" 포함 시 APPTAG 체크 비트 OR 추가. */
	if (strstr(str, "APPTAG") != NULL)
		o->prchk |= XNVME_PI_FLAGS_APPTAG_CHECK;

	/* [한국어] 성공 반환 — 파싱 실패는 발생하지 않음(부분 매칭 방식). */
	return 0;
}

/*
 * [한국어] xnvme 엔진이 노출하는 커맨드라인/잡파일 옵션 테이블.
 * 마지막 원소 .name = NULL 로 종결(fio 옵션 파서가 이 센티넬로 배열 끝 인식).
 * 모든 옵션이 FIO_OPT_C_ENGINE(엔진 카테고리) + FIO_OPT_G_XNVME(xnvme 서브그룹)
 * 에 속한다. `fio --enghelp=xnvme` 로 출력 필터링 가능.
 *
 * 각 엔트리 공통 필드 규약:
 *  .name       : 커맨드라인/잡파일에서 쓰는 옵션 이름 (--<name>=<val>).
 *  .lname      : long name — --help 설명 출력에 표시되는 사람이 읽는 이름.
 *  .type       : FIO_OPT_STR_SET(부울 플래그)/STR_STORE(문자열)/INT(정수)/BOOL.
 *  .off1       : offsetof(struct xnvme_fioe_options, <field>) — 파서가 저장할 위치.
 *  .def        : 기본값(문자열). NULL = 기본 없음.
 *  .help       : 한 줄 설명.
 *  .cb         : 사용자 정의 파서 콜백(필요 시). off1 대신 사용 가능.
 *  .category   : 최상위 카테고리. FIO_OPT_C_ENGINE=엔진 옵션 분류.
 *  .group      : 서브그룹. FIO_OPT_G_XNVME=xnvme 전용.
 */
static struct fio_option options[] = {
	{
		/* [한국어] --hipri : polled completion(io_uring polled 등) 사용 플래그. */
		.name = "hipri",                                          /* [한국어] 옵션 이름. */
		.lname = "High Priority",                                 /* [한국어] 설명용 long name. */
		.type = FIO_OPT_STR_SET,                                  /* [한국어] 존재만으로 1 설정(값 불필요). */
		.off1 = offsetof(struct xnvme_fioe_options, hipri),       /* [한국어] hipri 필드 오프셋. */
		.help = "Use polled IO completions",                      /* [한국어] --help 설명. */
		.category = FIO_OPT_C_ENGINE,                              /* [한국어] 엔진 카테고리. */
		.group = FIO_OPT_G_XNVME,                                 /* [한국어] xnvme 서브그룹. */
	},
	{
		/* [한국어] --sqthread_poll : io_uring SQ 폴링 커널 스레드 활성화. */
		.name = "sqthread_poll",                                  /* [한국어] 옵션 이름. */
		.lname = "Kernel SQ thread polling",                       /* [한국어] long name. */
		.type = FIO_OPT_STR_SET,                                  /* [한국어] 부울 플래그. */
		.off1 = offsetof(struct xnvme_fioe_options, sqpoll_thread), /* [한국어] sqpoll_thread 필드. */
		.help = "Offload submission/completion to kernel thread",  /* [한국어] 설명: CPU 1개 전용. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_be : 백엔드 문자열. libxnvme 가 런타임에 동적 선택. */
		.name = "xnvme_be",                                       /* [한국어] 옵션 이름. */
		.lname = "xNVMe Backend",                                 /* [한국어] long name. */
		.type = FIO_OPT_STR_STORE,                                /* [한국어] 문자열 저장(strdup). */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_be),    /* [한국어] xnvme_be 필드. */
		.help = "Select xNVMe backend [spdk,linux,fbsd]",         /* [한국어] 지원 값 힌트. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_mem : DMA 메모리 할당자 선택 (hugepage/posix 등). */
		.name = "xnvme_mem",
		.lname = "xNVMe Memory Backend",
		.type = FIO_OPT_STR_STORE,                                /* [한국어] 문자열 저장. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_mem),   /* [한국어] xnvme_mem 필드. */
		.help = "Select xNVMe memory backend",                    /* [한국어] 백엔드 의존. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_async : 비동기 인터페이스 (io_uring/io_uring_cmd/libaio/...). */
		.name = "xnvme_async",
		.lname = "xNVMe Asynchronous command-interface",
		.type = FIO_OPT_STR_STORE,                                /* [한국어] 문자열. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_async), /* [한국어] xnvme_async 필드. */
		.help = "Select xNVMe async. interface: "
			"[emu,thrpool,io_uring,io_uring_cmd,libaio,posix,vfio,nil]",
		/* [한국어] 8종 지원 값. io_uring_cmd=NVMe passthru, nil=더미(성능 상한 측정). */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_sync : 동기 인터페이스 (nvme/psync/block). */
		.name = "xnvme_sync",
		.lname = "xNVMe Synchronous. command-interface",
		.type = FIO_OPT_STR_STORE,                                /* [한국어] 문자열. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_sync),  /* [한국어] xnvme_sync 필드. */
		.help = "Select xNVMe sync. interface: [nvme,psync,block]", /* [한국어] 지원 값. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_admin : admin 명령 전송 채널 선택. */
		.name = "xnvme_admin",
		.lname = "xNVMe Admin command-interface",
		.type = FIO_OPT_STR_STORE,                                /* [한국어] 문자열. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_admin), /* [한국어] xnvme_admin 필드. */
		.help = "Select xNVMe admin. cmd-interface: [nvme,block]", /* [한국어] admin 채널. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_dev_nsid : user-space 드라이버용 NSID 명시. */
		.name = "xnvme_dev_nsid",
		.lname = "xNVMe Namespace-Identifier, for user-space NVMe driver",
		.type = FIO_OPT_INT,                                      /* [한국어] 정수. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_dev_nsid), /* [한국어] 필드. */
		.help = "xNVMe Namespace-Identifier, for user-space NVMe driver",
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_dev_subnqn : NVMe-oF Fabrics subsystem NQN. */
		.name = "xnvme_dev_subnqn",
		.lname = "Subsystem nqn for Fabrics",
		.type = FIO_OPT_STR_STORE,                                /* [한국어] 문자열. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_dev_subnqn), /* [한국어] 필드. */
		.help = "Subsystem NQN for Fabrics",                      /* [한국어] Fabrics 연결용. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --xnvme_iovec : 벡터드 I/O 경로(xnvme_cmd_passv) 강제. */
		.name = "xnvme_iovec",
		.lname = "Vectored IOs",
		.type = FIO_OPT_STR_SET,                                  /* [한국어] 부울 플래그. */
		.off1 = offsetof(struct xnvme_fioe_options, xnvme_iovec), /* [한국어] 필드. */
		.help = "Send vectored IOs",                              /* [한국어] iovec 사용. */
		.category = FIO_OPT_C_ENGINE,
		.group = FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --md_per_io_size : I/O 당 별도 메타데이터 버퍼 크기(바이트). */
		.name	= "md_per_io_size",
		.lname	= "Separate Metadata Buffer Size per I/O",
		.type	= FIO_OPT_INT,                                    /* [한국어] 정수 바이트. */
		.off1	= offsetof(struct xnvme_fioe_options, md_per_io_size), /* [한국어] 필드. */
		.def	= "0",                                            /* [한국어] 기본=0(미사용). */
		.help	= "Size of separate metadata buffer per I/O (Default: 0)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --pi_act : PI 삽입/제거를 컨트롤러가 수행할지 여부. */
		.name	= "pi_act",
		.lname	= "Protection Information Action",
		.type	= FIO_OPT_BOOL,                                   /* [한국어] 0/1. */
		.off1	= offsetof(struct xnvme_fioe_options, pi_act),    /* [한국어] 필드. */
		.def	= "1",                                            /* [한국어] 기본=1(컨트롤러). */
		.help	= "Protection Information Action bit (pi_act=1 or pi_act=0)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --pi_chk : PI 체크 대상 문자열. str_pi_chk_cb 로 파싱. */
		.name	= "pi_chk",
		.lname	= "Protection Information Check",
		.type	= FIO_OPT_STR_STORE,                              /* [한국어] 문자열 저장. */
		.def	= NULL,                                           /* [한국어] 기본 없음. */
		.help	= "Control of Protection Information Checking (pi_chk=GUARD,REFTAG,APPTAG)",
		.cb	= str_pi_chk_cb,                                  /* [한국어] 사용자 파서(off1 없음). */
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --apptag : PI Application Tag 16비트 값. */
		.name	= "apptag",
		.lname	= "Application Tag used in Protection Information",
		.type	= FIO_OPT_INT,                                    /* [한국어] 정수. */
		.off1	= offsetof(struct xnvme_fioe_options, apptag),    /* [한국어] 필드. */
		.def	= "0x1234",                                       /* [한국어] 기본값 리터럴. */
		.help	= "Application Tag used in Protection Information field (Default: 0x1234)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_XNVME,
	},
	{
		/* [한국어] --apptag_mask : Application Tag 체크 마스크 16비트. */
		.name	= "apptag_mask",
		.lname	= "Application Tag Mask",
		.type	= FIO_OPT_INT,                                    /* [한국어] 정수. */
		.off1	= offsetof(struct xnvme_fioe_options, apptag_mask), /* [한국어] 필드. */
		.def	= "0xffff",                                       /* [한국어] 기본=전체 체크. */
		.help	= "Application Tag Mask used with Application Tag (Default: 0xffff)",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_XNVME,
	},

	{
		/* [한국어] 옵션 배열 종결 마커 — fio 옵션 파서가 .name==NULL 감지. */
		.name = NULL,
	},
};

/*
 * [한국어]
 * cb_pool - xNVMe 큐 완료 콜백.
 *
 * @ctx:    완료된 NVMe 명령 컨텍스트 (라이브러리 소유, 이 함수 종료 시 큐로 반환).
 * @cb_arg: queue() 에서 ctx->async.cb_arg 로 저장했던 io_u 포인터.
 *
 * 역할: xnvme_queue_poke() 내부에서 각 완료 항목마다 호출된다. 완료 상태를
 * 검사하고, READ 인 경우 PI 검증을 수행한 뒤, xd->iocq 에 io_u 를 적재하여
 * 상위 getevents() 가 반환할 수 있도록 한다.
 * 실행 컨텍스트: 잡 스레드 내부. xnvme_queue_poke() 의 콜 트리 안에서 동기 호출됨.
 * 호출 체인: xnvme_fioe_getevents() → xnvme_queue_poke() → cb_pool().
 */
static void cb_pool(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	/* [한국어] cb_arg 를 io_u 로 복원 (queue() 에서 저장한 포인터). */
	struct io_u *io_u = cb_arg;
	/* [한국어] io_u 에 연결해 둔 xd 포인터 복원. 전역 상태 접근용. */
	struct xnvme_fioe_data *xd = io_u->mmap_data;
	/* [한국어] io_u 에 붙은 요청 상태(PI 컨텍스트/메타 버퍼) 복원. */
	struct xnvme_fioe_request *fio_req = io_u->engine_data;
	/* [한국어] 해당 파일의 래퍼(PI 타입/LBA 크기 조회용). */
	struct xnvme_fioe_fwrap *fwrap = &xd->files[io_u->file->fileno];
	/* [한국어] pi_flags 의 상위 비트(bit3) = pi_act 플래그.
	 * queue() 에서 (pi_act<<3 | prchk) 로 인코딩했으므로 역연산으로 추출. */
	bool pi_act = (fio_req->pi_ctx.pi_flags >> 3);
	int err;

	/* [한국어] 완료 상태 확인 — 0 이 아니면 NVMe 디바이스/커널에서 에러 반환. */
	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		/* [한국어] 디버깅을 위해 완료 상태를 사람이 읽을 수 있게 출력. */
		xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
		/* [한국어] 에러 카운터 누적 (통계용). */
		xd->ecount += 1;
		/* [한국어] io_u 에 EIO 기록 → fio 가 상위 단계에서 실패로 처리. */
		io_u->error = EIO;
	}

	/* [한국어] PI 타입이 활성화되어 있고, 호스트가 PI 를 검증해야 하며(pi_act=0),
	 * READ 완료인 경우에만 호스트 측 PI verify 수행 (WRITE 는 컨트롤러가 저장). */
	if (!io_u->error && fwrap->geo->pi_type && (io_u->ddir == DDIR_READ) && !pi_act) {
		/* [한국어] 데이터 버퍼 + 메타 버퍼에 대해 GUARD/REFTAG/APPTAG 검증. */
		err = xnvme_pi_verify(&fio_req->pi_ctx, io_u->xfer_buf,
				      fio_req->md_buf, io_u->xfer_buflen / fwrap->lba_nbytes);
		if (err) {
			/* [한국어] PI 불일치 → 무결성 오류로 기록. */
			xd->ecount += 1;
			io_u->error = EIO;
		}
	}

	/* [한국어] 완료된 io_u 를 iocq 에 적재 후 카운터 증가.
	 * event() 가 이 인덱스로 io_u 를 fio 에 반환한다. */
	xd->iocq[xd->completed++] = io_u;
	/* [한국어] 명령 컨텍스트를 큐에 반환(다음 제출에서 재사용). */
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

/*
 * [한국어]
 * xnvme_opts_from_fioe - fio 옵션을 libxnvme 의 xnvme_opts 구조체로 변환.
 *
 * @td: 현재 잡의 thread_data.
 * @return: 값 복사본 xnvme_opts (스택/레지스터 반환).
 *
 * _dev_open/get_zoned_model/report_zones/reset_wp 등 xnvme_dev_open 을 호출하는
 * 모든 경로에서 공통으로 사용되어 중복을 제거한다.
 */
static struct xnvme_opts xnvme_opts_from_fioe(struct thread_data *td)
{
	/* [한국어] 잡의 엔진 옵션 포인터. */
	struct xnvme_fioe_options *o = td->eo;
	/* [한국어] libxnvme 기본값으로 초기화된 opts 로 출발. */
	struct xnvme_opts opts = xnvme_opts_default();

	/* [한국어] 아래 블록은 fio 측 옵션을 libxnvme 필드로 1:1 매핑. */
	opts.nsid = o->xnvme_dev_nsid;       /* [한국어] Namespace ID 지정. */
	opts.subnqn = o->xnvme_dev_subnqn;   /* [한국어] Fabrics subsystem NQN. */
	opts.be = o->xnvme_be;               /* [한국어] 백엔드 (linux/spdk/fbsd). */
	opts.mem = o->xnvme_mem;             /* [한국어] 메모리 할당자. */
	opts.async = o->xnvme_async;         /* [한국어] 비동기 인터페이스. */
	opts.sync = o->xnvme_sync;           /* [한국어] 동기 인터페이스. */
	opts.admin = o->xnvme_admin;         /* [한국어] admin 명령 경로. */

	opts.poll_io = o->hipri;             /* [한국어] polled completion. */
	opts.poll_sq = o->sqpoll_thread;     /* [한국어] SQ 폴링 커널 스레드. */

	opts.direct = td->o.odirect;         /* [한국어] fio --direct 그대로 전달 (O_DIRECT). */

	/* [한국어] 값 반환 — 호출자가 xnvme_dev_open 에 주소를 넘긴다. */
	return opts;
}

/*
 * [한국어]
 * _dev_close - 파일 래퍼의 xNVMe 큐 종료 + 장치 닫기 + 구조체 0 초기화.
 *
 * @td:    잡 컨텍스트 (현 구현에서는 미사용이나 시그니처 통일성 유지).
 * @fwrap: 닫을 대상 래퍼.
 *
 * cleanup 경로 중 한 파일 단위 해제를 담당. g_serialize 는 상위에서 보유.
 */
static void _dev_close(struct thread_data *td, struct xnvme_fioe_fwrap *fwrap)
{
	/* [한국어] 장치가 열려 있을 때만 큐 종료 (진행 중 명령 드레인 포함). */
	if (fwrap->dev)
		xnvme_queue_term(fwrap->queue);

	/* [한국어] 장치 핸들 닫기. dev==NULL 시에도 안전(라이브러리 no-op). */
	xnvme_dev_close(fwrap->dev);

	/* [한국어] 래퍼 전체를 0으로 리셋(이후 cleanup 중복 호출 방어). */
	memset(fwrap, 0, sizeof(*fwrap));
}

/*
 * [한국어]
 * xnvme_fioe_cleanup - 엔진 정리 콜백 (.cleanup).
 *
 * @td: 잡 컨텍스트.
 *
 * init 에서 할당한 xd / iocq / iovec / md_iovec 해제 및 파일별 장치 닫기.
 * g_serialize 로 xnvme_dev_close 직렬화. 잡 종료 시 1회 호출.
 */
static void xnvme_fioe_cleanup(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = NULL;
	int err;

	/* [한국어] init 이 실패했거나 호출되지 않은 경우(데이터 없음) 조기 반환. */
	if (!td->io_ops_data)
		return;

	/* [한국어] 엔진 상태 포인터 복원. */
	xd = td->io_ops_data;

	/* [한국어] 장치 close 는 libxnvme 전역 상태를 만지므로 직렬화. */
	err = pthread_mutex_lock(&g_serialize);
	if (err)
		log_err("ioeng->cleanup(): pthread_mutex_lock(), err(%d)\n", err);
		/* NOTE: not returning here */
		/* [한국어] 락 실패해도 진행한다 — 자원 누수 방지가 더 중요. */

	/* [한국어] 모든 할당된 파일에 대해 _dev_close 호출. */
	for (uint64_t i = 0; i < xd->nallocated; ++i)
		_dev_close(td, &xd->files[i]);

	/* [한국어] 락을 실제 잡은 경우에만 해제 시도. */
	if (!err) {
		err = pthread_mutex_unlock(&g_serialize);
		if (err)
			log_err("ioeng->cleanup(): pthread_mutex_unlock(), err(%d)\n", err);
	}

	/* [한국어] 완료 큐 / iovec 풀 / md_iovec 풀 / xd 순으로 해제. */
	free(xd->iocq);
	free(xd->iovec);
	free(xd->md_iovec);
	free(xd);
	/* [한국어] dangling 방지 — fio 코어가 중복 cleanup 을 호출해도 안전. */
	td->io_ops_data = NULL;
}

/*
 * [한국어]
 * _verify_options - 파일 오픈 직후 블록 크기/메타데이터 크기 적합성 검증.
 *
 * @td:    잡 컨텍스트.
 * @f:     대상 파일.
 * @fwrap: 이미 open 된 장치 래퍼 (geo/lba_nbytes/md_nbytes 포함).
 * @return: 0=OK, 1=옵션 오류.
 *
 * - bs 가 LBA 배수가 아니면 실패.
 * - md_per_io_size 가 최대 bs/lba_nbytes * md_nbytes 보다 작으면 실패.
 * - 확장 LBA + PI + verify 조합은 금지(PI 영역이 verify 데이터와 충돌).
 */
static int _verify_options(struct thread_data *td, struct fio_file *f,
			   struct xnvme_fioe_fwrap *fwrap)
{
	struct xnvme_fioe_options *o = td->eo;
	unsigned int correct_md_size;

	/* [한국어] READ/WRITE 방향 각각에 대해 검증 (fio 매크로). */
	for_each_rw_ddir(ddir) {
		/* [한국어] 최소/최대 블록 크기가 LBA 크기의 배수인지. */
		if (td->o.min_bs[ddir] % fwrap->lba_nbytes || td->o.max_bs[ddir] % fwrap->lba_nbytes) {
			if (!fwrap->lba_pow2) {
				/* [한국어] 확장 LBA: 메타데이터 포함 LBA 크기 기준으로 안내. */
				log_err("ioeng->_verify_options(%s): block size must be a multiple of %u "
					"(LBA data size + Metadata size)\n", f->file_name, fwrap->lba_nbytes);
			} else {
				/* [한국어] 일반 LBA 크기 기준 안내. */
				log_err("ioeng->_verify_options(%s): block size must be a multiple of LBA data size\n",
					f->file_name);
			}
			return 1;
		}
		/* [한국어] TRIM 은 데이터 전송이 없으므로 메타 크기 검증 스킵. */
		if (ddir == DDIR_TRIM)
			continue;

		/* [한국어] 한 I/O 가 담는 LBA 수 × LBA 당 메타 크기 = 요구 버퍼 크기. */
		correct_md_size = (td->o.max_bs[ddir] / fwrap->lba_nbytes) * fwrap->md_nbytes;
		if (fwrap->md_nbytes && fwrap->lba_pow2 && (o->md_per_io_size < correct_md_size)) {
			log_err("ioeng->_verify_options(%s): md_per_io_size should be at least %u bytes\n",
				f->file_name, correct_md_size);
			return 1;
		}
	}

	/*
	 * For extended logical block sizes we cannot use verify when
	 * end to end data protection checks are enabled, as the PI
	 * section of data buffer conflicts with verify.
	 */
	/* [한국어] 확장 LBA(데이터+PI 통합) + PI 활성 + fio verify 는 상호 배타.
	 * verify 가 기대하는 패턴 영역을 PI 가 덮어쓰기 때문. */
	if (fwrap->md_nbytes && fwrap->geo->pi_type && !fwrap->lba_pow2 &&
	    td->o.verify != VERIFY_NONE) {
		log_err("ioeng->_verify_options(%s): for extended LBA, verify cannot be used when E2E data protection is enabled\n",
			f->file_name);
		return 1;
	}

	/* [한국어] 모든 검증 통과. */
	return 0;
}

/**
 * Helper function setting up device handles as addressed by the naming
 * convention of the given `fio_file` filename.
 *
 * Checks thread-options for explicit control of asynchronous implementation via
 * the ``--xnvme_async={thrpool,emu,posix,io_uring,libaio,nil}``.
 */
/*
 * [한국어]
 * _dev_open - 파일 하나에 대해 xNVMe 장치를 열고 큐를 초기화.
 *
 * @td: 잡 컨텍스트.
 * @f:  대상 fio 파일 (파일명이 xNVMe URI 역할).
 * @return: 0=성공, 1=실패.
 *
 * 동작: (1) g_serialize 획득 → (2) xnvme_dev_open → (3) geo 조회 → (4) queue_init →
 *       (5) 큐 완료 콜백 등록 → (6) LBA/메타 파라미터 계산 → (7) PI 인서션 보정 →
 *       (8) 옵션 검증 → (9) fio_file 메타 채우기 → (10) 락 해제.
 * 실패 경로: failure: 레이블에서 큐 종료/장치 닫기 후 락 해제하고 1 반환.
 */
static int _dev_open(struct thread_data *td, struct fio_file *f)
{
	/* [한국어] fio 옵션을 xnvme_opts 로 변환 (복사본). */
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_fioe_options *o = td->eo;
	/* [한국어] 엔진 런타임 상태 — 파일 슬롯 배열 접근용. */
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap;
	/* [한국어] 현재는 0 이지만 libxnvme 큐 플래그 확장 여지. */
	int flags = 0;
	int err;

	/* [한국어] fileno 가 할당 범위를 넘지 않는지 불변식 검사. */
	if (f->fileno > (int)xd->nallocated) {
		log_err("ioeng->_dev_open(%s): invalid assumption\n", f->file_name);
		return 1;
	}

	/* [한국어] 대상 래퍼 슬롯 선택. */
	fwrap = &xd->files[f->fileno];

	/* [한국어] 전역 직렬화: 여러 잡 스레드의 dev_open 충돌 방지. */
	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		log_err("ioeng->_dev_open(%s): pthread_mutex_lock(), err(%d)\n", f->file_name,
			err);
		return -err;
	}

	/* [한국어] 장치 열기 — URI 는 파일명(예: /dev/nvme0n1, pcie:...). */
	fwrap->dev = xnvme_dev_open(f->file_name, &opts);
	if (!fwrap->dev) {
		log_err("ioeng->_dev_open(%s): xnvme_dev_open(), err(%d)\n", f->file_name, errno);
		goto failure;
	}
	/* [한국어] 지오메트리 획득(라이브러리 소유 const). */
	fwrap->geo = xnvme_dev_get_geo(fwrap->dev);

	/* [한국어] iodepth 크기의 비동기 명령 큐 생성 및 콜백 등록. */
	if (xnvme_queue_init(fwrap->dev, td->o.iodepth, flags, &(fwrap->queue))) {
		log_err("ioeng->_dev_open(%s): xnvme_queue_init(), err(?)\n", f->file_name);
		goto failure;
	}
	/* [한국어] 완료 콜백을 cb_pool 로 통일. cb_arg 는 queue() 에서 io_u 별로 설정. */
	xnvme_queue_set_cb(fwrap->queue, cb_pool, NULL);

	/* [한국어] 섹터 시프트/LBA/메타 크기 프리컴퓨트. */
	fwrap->ssw = xnvme_dev_get_ssw(fwrap->dev);
	fwrap->lba_nbytes = fwrap->geo->lba_nbytes;
	fwrap->md_nbytes = fwrap->geo->nbytes_oob;

	/* [한국어] 확장 LBA 는 시프트 불가(데이터+PI 붙어 있음) → 나눗셈 경로 사용. */
	if (fwrap->geo->lba_extended)
		fwrap->lba_pow2 = 0;
	else
		fwrap->lba_pow2 = 1;

	/*
	 * When PI action is set and PI size is equal to metadata size, the
	 * controller inserts/removes PI. So update the LBA data and metadata
	 * sizes accordingly.
	 */
	/* [한국어] pi_act=1 이고 OOB 전체가 PI 라면 컨트롤러가 PI 삽입/제거를 담당.
	 * 호스트 입장에서는 데이터 크기만 다루면 되므로 lba/md 값을 보정한다. */
	if (o->pi_act && fwrap->geo->pi_type &&
	    fwrap->geo->nbytes_oob == xnvme_pi_size(fwrap->geo->pi_format)) {
		if (fwrap->geo->lba_extended) {
			/* [한국어] 확장 LBA 의 PI 부분을 데이터 크기에서 제외 → 다시 2의 거듭제곱. */
			fwrap->lba_nbytes -= fwrap->geo->nbytes_oob;
			fwrap->lba_pow2 = 1;
		}
		/* [한국어] 메타 버퍼 필요 없음 (컨트롤러가 자체 처리). */
		fwrap->md_nbytes = 0;
	}

	/* [한국어] bs / md_per_io_size / verify 조합 검증. */
	if (_verify_options(td, f, fwrap)) {
		td_verror(td, EINVAL, "_dev_open");
		goto failure;
	}

	/* [한국어] fio 가 이 파일을 블록 디바이스로 인식하도록 메타 설정. */
	fwrap->fio_file = f;
	fwrap->fio_file->filetype = FIO_TYPE_BLOCK;
	fwrap->fio_file->real_file_size = fwrap->geo->tbytes;
	fio_file_set_size_known(fwrap->fio_file);

	/* [한국어] 성공 경로 락 해제. */
	err = pthread_mutex_unlock(&g_serialize);
	if (err)
		log_err("ioeng->_dev_open(%s): pthread_mutex_unlock(), err(%d)\n", f->file_name,
			err);

	return 0;

failure:
	/* [한국어] 실패 시 부분적으로 생성된 자원을 정리 (queue/dev 모두 NULL-safe). */
	xnvme_queue_term(fwrap->queue);
	xnvme_dev_close(fwrap->dev);

	/* [한국어] 실패 경로 락 해제. */
	err = pthread_mutex_unlock(&g_serialize);
	if (err)
		log_err("ioeng->_dev_open(%s): pthread_mutex_unlock(), err(%d)\n", f->file_name,
			err);

	return 1;
}

/*
 * [한국어]
 * xnvme_fioe_init - 엔진 초기화 콜백 (.init).
 *
 * @td: 잡 컨텍스트.
 * @return: 0=성공, 1=실패 (fio 가 잡 중단).
 *
 * - --thread=1 강제 (libxnvme 전역 상태 공유 때문에 fork 불가).
 * - xd 와 iocq, 옵션에 따라 iovec/md_iovec 풀 할당.
 * - 모든 파일에 대해 _dev_open 호출.
 */
static int xnvme_fioe_init(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = NULL;
	struct xnvme_fioe_options *o = td->eo;
	struct fio_file *f;
	unsigned int i;

	/* [한국어] thread 모델 요구: process 모델은 libxnvme 공유 상태와 호환 불가. */
	if (!td->o.use_thread) {
		log_err("ioeng->init(): --thread=1 is required\n");
		return 1;
	}

	/* Allocate xd and iocq */
	/* [한국어] flexible array 로 files[nr_files] 를 뒤에 붙여 한 덩어리로 할당. */
	xd = calloc(1, sizeof(*xd) + sizeof(*xd->files) * td->o.nr_files);
	if (!xd) {
		log_err("ioeng->init(): !calloc(), err(%d)\n", errno);
		return 1;
	}

	/* [한국어] 완료 큐: iodepth 만큼의 io_u 포인터 저장 공간. */
	xd->iocq = calloc(td->o.iodepth, sizeof(struct io_u *));
	if (!xd->iocq) {
		free(xd);
		log_err("ioeng->init(): !calloc(xd->iocq), err(%d)\n", errno);
		return 1;
	}

	/* [한국어] 벡터드 I/O 모드 시 데이터 iovec 풀 할당. */
	if (o->xnvme_iovec) {
		xd->iovec = calloc(td->o.iodepth, sizeof(*xd->iovec));
		if (!xd->iovec) {
			free(xd->iocq);
			free(xd);
			log_err("ioeng->init(): !calloc(xd->iovec), err(%d)\n", errno);
			return 1;
		}
	}

	/* [한국어] 벡터드 I/O + 분리 메타 버퍼 동시 사용 시 메타 iovec 풀도 필요. */
	if (o->xnvme_iovec && o->md_per_io_size) {
		xd->md_iovec = calloc(td->o.iodepth, sizeof(*xd->md_iovec));
		if (!xd->md_iovec) {
			free(xd->iocq);
			free(xd->iovec);
			free(xd);
			log_err("ioeng->init(): !calloc(xd->md_iovec), err(%d)\n", errno);
			return 1;
		}
	}

	/* [한국어] getevents 라운드로빈 커서 초기값: "아직 진행 안 함" 표식 -1. */
	xd->prev = -1;
	/* [한국어] 엔진 상태를 td 에 연결 — 이후 모든 콜백에서 이 포인터를 사용. */
	td->io_ops_data = xd;

	/* [한국어] 잡에 지정된 모든 파일에 대해 장치 오픈 루프. */
	for_each_file(td, f, i)
	{
		if (_dev_open(td, f)) {
			/*
			 * Note: We are not freeing xd, iocq, iovec and md_iovec.
			 * This will be done as part of cleanup routine.
			 */
			/* [한국어] 실패해도 xd 는 유지 — cleanup 이 일괄 해제. */
			log_err("ioeng->init(): failed; _dev_open(%s)\n", f->file_name);
			return 1;
		}

		/* [한국어] 성공한 파일 수 카운트 (cleanup 의 상한). */
		++(xd->nallocated);
	}

	/* [한국어] 모든 파일이 열렸는지 불변식 검사. */
	if (xd->nallocated != td->o.nr_files) {
		log_err("ioeng->init(): failed; nallocated != td->o.nr_files\n");
		return 1;
	}

	return 0;
}

/* NOTE: using the first device for buffer-allocators) */
/*
 * [한국어]
 * xnvme_fioe_iomem_alloc - DMA 가능한 I/O 버퍼 할당 (.iomem_alloc).
 *
 * @td:        잡 컨텍스트.
 * @total_mem: 할당할 총 바이트 수.
 * @return: 0=성공, 1=실패.
 *
 * fio 는 모든 io_u 가 공유하는 큰 버퍼 하나를 td->orig_buffer 에 할당한다.
 * 여러 장치가 있어도 첫 번째 장치의 할당자를 사용 — 백엔드 간 DMA 호환 가정.
 */
static int xnvme_fioe_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	/* [한국어] 엔진 상태와 0번 파일 래퍼 선택. */
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[0];

	/* [한국어] 장치 핸들 유효성 확인. */
	if (!fwrap->dev) {
		log_err("ioeng->iomem_alloc(): failed; no dev-handle\n");
		return 1;
	}

	/* [한국어] xnvme_buf_alloc 은 백엔드에 맞는 DMA 버퍼 할당(hugepage/pinned 등). */
	td->orig_buffer = xnvme_buf_alloc(fwrap->dev, total_mem);

	/* [한국어] NULL 이면 실패 = 1 반환. */
	return td->orig_buffer == NULL;
}

/* NOTE: using the first device for buffer-allocators) */
/*
 * [한국어]
 * xnvme_fioe_iomem_free - I/O 버퍼 해제 (.iomem_free).
 *
 * @td: 잡 컨텍스트.
 * iomem_alloc 과 대응. 0번 장치의 할당자로 해제.
 */
static void xnvme_fioe_iomem_free(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = NULL;
	struct xnvme_fioe_fwrap *fwrap = NULL;

	/* [한국어] init 실패 등 경로에서 xd 가 NULL 일 수 있음. */
	if (!td->io_ops_data)
		return;

	xd = td->io_ops_data;
	fwrap = &xd->files[0];

	/* [한국어] 장치 핸들 없으면 해제 경로도 스킵. */
	if (!fwrap->dev) {
		log_err("ioeng->iomem_free(): failed no dev-handle\n");
		return;
	}

	/* [한국어] 할당 당시와 동일한 dev 핸들로 해제. */
	xnvme_buf_free(fwrap->dev, td->orig_buffer);
}

/*
 * [한국어]
 * xnvme_fioe_io_u_init - io_u 하나에 엔진 전용 상태 붙이기 (.io_u_init).
 *
 * @td:   잡 컨텍스트.
 * @io_u: 초기화 대상.
 * @return: 0=성공, 1=실패.
 *
 * 각 io_u 마다 xnvme_fioe_request 를 calloc 하고, md_per_io_size 지정 시
 * DMA 가능한 메타 버퍼를 별도로 할당하여 붙인다.
 */
static int xnvme_fioe_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct xnvme_fioe_request *fio_req;
	struct xnvme_fioe_options *o = td->eo;
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[0];

	/* [한국어] 0번 장치 핸들이 있어야 DMA 버퍼 할당 가능. */
	if (!fwrap->dev) {
		log_err("ioeng->io_u_init(): failed; no dev-handle\n");
		return 1;
	}

	/* [한국어] mmap_data 에 xd 포인터 저장 → cb_pool 에서 역참조. */
	io_u->mmap_data = td->io_ops_data;
	/* [한국어] engine_data 는 아래서 할당한 fio_req 로 덮어씀. */
	io_u->engine_data = NULL;

	/* [한국어] 요청 상태 구조체 할당 (PI 컨텍스트/메타 버퍼 포인터 보관). */
	fio_req = calloc(1, sizeof(*fio_req));
	if (!fio_req) {
		log_err("ioeng->io_u_init(): !calloc(fio_req), err(%d)\n", errno);
		return 1;
	}

	/* [한국어] 메타 버퍼 사용 옵션이면 DMA 가능 메모리로 할당. */
	if (o->md_per_io_size) {
		fio_req->md_buf = xnvme_buf_alloc(fwrap->dev, o->md_per_io_size);
		if (!fio_req->md_buf) {
			free(fio_req);
			return 1;
		}
	}

	/* [한국어] io_u 에 요청 상태 연결. */
	io_u->engine_data = fio_req;

	return 0;
}

/*
 * [한국어]
 * xnvme_fioe_io_u_free - io_u 엔진 상태 해제 (.io_u_free).
 *
 * @td:   잡 컨텍스트.
 * @io_u: 해제 대상.
 *
 * io_u_init 와 대응. 메타 버퍼/요청 구조체 해제.
 */
static void xnvme_fioe_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct xnvme_fioe_data *xd = NULL;
	struct xnvme_fioe_fwrap *fwrap = NULL;
	struct xnvme_fioe_request *fio_req = NULL;

	/* [한국어] 엔진 상태 없으면 아무 것도 붙여 둔 것이 없으므로 반환. */
	if (!td->io_ops_data)
		return;

	xd = td->io_ops_data;
	fwrap = &xd->files[0];

	/* [한국어] 장치 핸들 없으면 DMA 버퍼 해제 불가(라이브러리 요구). */
	if (!fwrap->dev) {
		log_err("ioeng->io_u_free(): failed no dev-handle\n");
		return;
	}

	/* [한국어] 메타 버퍼가 있으면 dev 할당자로 해제. */
	fio_req = io_u->engine_data;
	if (fio_req->md_buf)
		xnvme_buf_free(fwrap->dev, fio_req->md_buf);

	/* [한국어] 요청 구조체 자체 해제. */
	free(fio_req);

	/* [한국어] 역참조 안전성: xd 포인터 끊기. */
	io_u->mmap_data = NULL;
}

/*
 * [한국어]
 * xnvme_fioe_event - 완료 큐에서 인덱스로 io_u 반환 (.event).
 *
 * @td:    잡 컨텍스트.
 * @event: getevents 가 반환한 완료 수 범위 내의 인덱스.
 * @return: 완료된 io_u 포인터.
 *
 * fio 코어가 getevents() 반환값만큼 이 함수를 event=0..N-1 로 호출한다.
 */
static struct io_u *xnvme_fioe_event(struct thread_data *td, int event)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	/* [한국어] 인덱스 범위 불변식 검증 (디버그 빌드에서만 abort). */
	assert(event >= 0);
	assert((unsigned)event < xd->completed);

	/* [한국어] cb_pool 이 채워둔 슬롯에서 io_u 반환. */
	return xd->iocq[event];
}

/*
 * [한국어]
 * xnvme_fioe_getevents - 완료 수확 (.getevents).
 *
 * @td:  잡 컨텍스트.
 * @min: 최소 수확 개수 (이만큼 모이면 반환).
 * @max: 최대 수확 개수.
 * @t:   타임아웃(현 구현 미사용 — 무한 폴링).
 * @return: 실제 수확한 완료 수.
 *
 * 여러 파일(= 장치)의 큐를 라운드로빈으로 돌며 xnvme_queue_poke() 로 완료를
 * 끌어낸다. poke 가 EBUSY/EAGAIN 반환 시 1us 수면 후 재시도. 각 파일에서
 * 수확된 완료는 콜백 cb_pool 이 xd->iocq 에 적재.
 */
static int xnvme_fioe_getevents(struct thread_data *td, unsigned int min, unsigned int max,
				const struct timespec *t)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = NULL;
	int nfiles = xd->nallocated;
	int err = 0;

	/* [한국어] 이전 라운드에서 멈춘 지점 다음부터 공평하게 재개. */
	if (xd->prev != -1 && ++xd->prev < nfiles) {
		fwrap = &xd->files[xd->prev];
		xd->cur = xd->prev;
	}

	/* [한국어] 이번 라운드의 완료 카운터를 0으로 리셋. */
	xd->completed = 0;
	for (;;) {
		/* [한국어] fwrap 이 없거나 커서가 끝에 도달하면 처음부터 순회. */
		if (fwrap == NULL || xd->cur == nfiles) {
			fwrap = &xd->files[0];
			xd->cur = 0;
		}

		/* [한국어] 파일 단위로 poke 호출하며 완료를 수확. */
		while (fwrap != NULL && xd->cur < nfiles && err >= 0) {
			/* [한국어] 최대 (max-completed) 개까지 콜백 호출 유도. */
			err = xnvme_queue_poke(fwrap->queue, max - xd->completed);
			if (err < 0) {
				switch (err) {
				case -EBUSY:
				case -EAGAIN:
					/* [한국어] 아직 완료 없음 — 짧게 쉬고 재시도. */
					usleep(1);
					break;

				default:
					log_err("ioeng->getevents(): unhandled IO error\n");
					assert(false);
					return 0;
				}
			}
			/* [한국어] min 이상 모였으면 커서 저장 후 반환. */
			if (xd->completed >= min) {
				xd->prev = xd->cur;
				return xd->completed;
			}
			/* [한국어] 다음 파일로 이동. */
			xd->cur++;
			fwrap = &xd->files[xd->cur];

			/* [한국어] 재시도 경로 보조 — poke 에러 재진입 시 sleep. */
			if (err < 0) {
				switch (err) {
				case -EBUSY:
				case -EAGAIN:
					usleep(1);
					break;
				}
			}
		}
	}

	/* [한국어] 이론상 도달 불가 — 상단 for(;;) 무한루프. 방어적 코드. */
	xd->cur = 0;

	return xd->completed;
}

/*
 * [한국어]
 * xnvme_fioe_queue - io_u 를 NVMe 명령으로 변환 후 제출 (.queue).
 *
 * @td:   잡 컨텍스트.
 * @io_u: 제출할 I/O 유닛.
 * @return: FIO_Q_QUEUED(제출 성공)/FIO_Q_BUSY(재시도 요청)/FIO_Q_COMPLETED(즉시완료/실패).
 *
 * 단계:
 *  1) io_u->offset, xfer_buflen 을 SLBA/NLB 로 변환.
 *  2) xnvme_queue_get_cmd_ctx 로 명령 컨텍스트 획득.
 *  3) NSID, SLBA, NLB, dtype/dspec(FDP) 설정.
 *  4) ddir → opcode 매핑 (READ/WRITE).
 *  5) PI 필요 시 pi_ctx 초기화 및 WRITE 이면 PI 생성.
 *  6) PI 타입별 ILBRT/APPTAG 필드 채움.
 *  7) iovec 또는 단일 버퍼 경로로 xnvme_cmd_pass(v) 호출.
 *  8) 반환값을 FIO_Q_* 로 매핑.
 */
static enum fio_q_status xnvme_fioe_queue(struct thread_data *td, struct io_u *io_u)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_options *o = td->eo;
	struct xnvme_fioe_fwrap *fwrap;
	struct xnvme_cmd_ctx *ctx;
	struct xnvme_fioe_request *fio_req = io_u->engine_data;
	uint32_t nsid;
	uint64_t slba;
	uint16_t nlb;
	int err;
	/* [한국어] 벡터드 I/O 여부 (옵션 캐시). */
	bool vectored_io = ((struct xnvme_fioe_options *)td->eo)->xnvme_iovec;
	/* [한국어] FDP directive type (0 = 미사용). */
	uint32_t dir = io_u->dtype;

	/* [한국어] read-only 타겟에 WRITE 요청 금지 등 공통 사전 체크. */
	fio_ro_check(td, io_u);

	/* [한국어] io_u 가 속한 파일의 래퍼 선택 및 NSID 조회. */
	fwrap = &xd->files[io_u->file->fileno];
	nsid = xnvme_dev_get_nsid(fwrap->dev);

	/* [한국어] 2의 거듭제곱 LBA 는 시프트 한 번으로 빠르게 계산. */
	if (fwrap->lba_pow2) {
		slba = io_u->offset >> fwrap->ssw;
		nlb = (io_u->xfer_buflen >> fwrap->ssw) - 1;
	} else {
		/* [한국어] 확장 LBA 경로: 나눗셈 필요. */
		slba = io_u->offset / fwrap->lba_nbytes;
		nlb = (io_u->xfer_buflen / fwrap->lba_nbytes) - 1;
	}

	/* [한국어] 큐에서 사용 가능한 명령 컨텍스트 획득(풀에서 꺼냄). */
	ctx = xnvme_queue_get_cmd_ctx(fwrap->queue);
	/* [한국어] 완료 콜백 인자로 io_u 저장 — cb_pool 이 이 포인터로 복원. */
	ctx->async.cb_arg = io_u;

	/* [한국어] 공통 NVMe 필드 설정: Namespace ID. */
	ctx->cmd.common.nsid = nsid;
	/* [한국어] NVM 명령의 Starting LBA. */
	ctx->cmd.nvm.slba = slba;
	/* [한국어] Number of Logical Blocks (0-based, 즉 실제 블록 수 = nlb+1). */
	ctx->cmd.nvm.nlb = nlb;
	/* [한국어] Flexible Data Placement directive 지정(비 0 일 때만). */
	if (dir) {
		ctx->cmd.nvm.dtype = io_u->dtype;
		ctx->cmd.nvm.cdw13.dspec = io_u->dspec;
	}

	/* [한국어] fio 방향 → NVMe opcode 매핑. */
	switch (io_u->ddir) {
	case DDIR_READ:
		ctx->cmd.common.opcode = XNVME_SPEC_NVM_OPC_READ;
		break;

	case DDIR_WRITE:
		ctx->cmd.common.opcode = XNVME_SPEC_NVM_OPC_WRITE;
		break;

	default:
		/* [한국어] TRIM/SYNC 등 미지원 방향은 즉시 에러 완료. */
		log_err("ioeng->queue(): ENOSYS: %u\n", io_u->ddir);
		xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);

		io_u->error = ENOSYS;
		assert(false);
		return FIO_Q_COMPLETED;
	}

	/* [한국어] PI 타입 활성 + pi_act=0 이면 호스트가 PI 를 생성/검증. */
	if (fwrap->geo->pi_type && !o->pi_act) {
		/* [한국어] PI 컨텍스트 초기화 — 이후 verify/generate 의 기준값. */
		err = xnvme_pi_ctx_init(&fio_req->pi_ctx, fwrap->lba_nbytes,
					fwrap->geo->nbytes_oob, fwrap->geo->lba_extended,
					fwrap->geo->pi_loc, fwrap->geo->pi_type,
					(o->pi_act << 3 | o->prchk), slba, o->apptag_mask,
					o->apptag, fwrap->geo->pi_format);
		if (err) {
			log_err("ioeng->queue(): err: '%d'\n", err);

			xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);

			io_u->error = abs(err);
			return FIO_Q_COMPLETED;
		}

		/* [한국어] WRITE 는 디바이스로 보내기 전 데이터 버퍼에 PI 채움. */
		if (io_u->ddir == DDIR_WRITE)
			xnvme_pi_generate(&fio_req->pi_ctx, io_u->xfer_buf, fio_req->md_buf,
					  nlb + 1);
	}

	/* [한국어] PRINFO 필드 세팅 — pi_act 비트(bit3) + prchk 비트(GUARD/REF/APP). */
	if (fwrap->geo->pi_type)
		ctx->cmd.nvm.prinfo = (o->pi_act << 3 | o->prchk);

	/* [한국어] PI 타입별 추가 필드: ILBRT/CDW03/LBAT/LBATM. */
	switch (fwrap->geo->pi_type) {
	case XNVME_PI_TYPE1:
	case XNVME_PI_TYPE2:
		switch (fwrap->geo->pi_format) {
		case XNVME_SPEC_NVM_NS_16B_GUARD:
			/* [한국어] REFTAG 검사 시 Initial LBA Reference Tag = SLBA 하위 32비트. */
			if (o->prchk & XNVME_PI_FLAGS_REFTAG_CHECK)
				ctx->cmd.nvm.ilbrt = (uint32_t)slba;
			break;
		case XNVME_SPEC_NVM_NS_64B_GUARD:
			/* [한국어] 64B GUARD: SLBA 상위 48비트를 CDW03 에 분할 저장. */
			if (o->prchk & XNVME_PI_FLAGS_REFTAG_CHECK) {
				ctx->cmd.nvm.ilbrt = (uint32_t)slba;
				ctx->cmd.common.cdw03 = ((slba >> 32) & 0xffff);
			}
			break;
		default:
			break;
		}
		/* [한국어] APPTAG 검사 시 태그/마스크 채움. */
		if (o->prchk & XNVME_PI_FLAGS_APPTAG_CHECK) {
			ctx->cmd.nvm.lbat = o->apptag;
			ctx->cmd.nvm.lbatm = o->apptag_mask;
		}
		break;
	case XNVME_PI_TYPE3:
		/* [한국어] TYPE3 은 REFTAG 검사 없음, APPTAG 만 의미 있음. */
		if (o->prchk & XNVME_PI_FLAGS_APPTAG_CHECK) {
			ctx->cmd.nvm.lbat = o->apptag;
			ctx->cmd.nvm.lbatm = o->apptag_mask;
		}
		break;
	case XNVME_PI_DISABLE:
		/* [한국어] PI 비활성 — 추가 필드 없음. */
		break;
	}

	/* [한국어] 실제 명령 제출 경로 분기: 벡터드 vs 단일 버퍼. */
	if (vectored_io) {
		/* [한국어] io_u->index 슬롯에 iovec 구성 (길이 1). */
		xd->iovec[io_u->index].iov_base = io_u->xfer_buf;
		xd->iovec[io_u->index].iov_len = io_u->xfer_buflen;
		if (fwrap->md_nbytes && fwrap->lba_pow2) {
			/* [한국어] 메타 iovec 도 함께 제출. */
			xd->md_iovec[io_u->index].iov_base = fio_req->md_buf;
			xd->md_iovec[io_u->index].iov_len = fwrap->md_nbytes * (nlb + 1);
			err = xnvme_cmd_passv(ctx, &xd->iovec[io_u->index], 1, io_u->xfer_buflen,
					      &xd->md_iovec[io_u->index], 1,
					      fwrap->md_nbytes * (nlb + 1));
		} else {
			/* [한국어] 메타 없이 데이터 iovec 만 전송. */
			err = xnvme_cmd_passv(ctx, &xd->iovec[io_u->index], 1, io_u->xfer_buflen,
					      NULL, 0, 0);
		}
	} else {
		/* [한국어] 단일 버퍼 전송. 메타 버퍼 유무로 다시 분기. */
		if (fwrap->md_nbytes && fwrap->lba_pow2)
			err = xnvme_cmd_pass(ctx, io_u->xfer_buf, io_u->xfer_buflen,
					     fio_req->md_buf, fwrap->md_nbytes * (nlb + 1));
		else
			err = xnvme_cmd_pass(ctx, io_u->xfer_buf, io_u->xfer_buflen, NULL, 0);
	}
	/* [한국어] 제출 결과 → fio 큐 상태 변환. */
	switch (err) {
	case 0:
		return FIO_Q_QUEUED;

	case -EBUSY:
	case -EAGAIN:
		/* [한국어] 일시적 혼잡 — 컨텍스트 반환 후 fio 에게 재시도 요청. */
		xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
		return FIO_Q_BUSY;

	default:
		/* [한국어] 기타 에러 — 즉시 완료 처리. */
		log_err("ioeng->queue(): err: '%d'\n", err);

		xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);

		io_u->error = abs(err);
		assert(false);
		return FIO_Q_COMPLETED;
	}
}

/*
 * [한국어]
 * xnvme_fioe_close - 파일 close 콜백 (.close_file).
 *
 * @td: 잡 컨텍스트.
 * @f:  닫을 파일.
 * @return: 0 (항상 성공).
 *
 * 실제 장치 닫기는 cleanup 에서 일괄 처리. 여기서는 참조 카운터만 감소시킨다.
 */
static int xnvme_fioe_close(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	/* [한국어] 디버그 로그: 닫는 파일 이름과 현재 open 카운터. */
	dprint(FD_FILE, "xnvme close %s -- nopen: %ld\n", f->file_name, xd->nopen);

	/* [한국어] open 카운터 감소. */
	--(xd->nopen);

	return 0;
}

/*
 * [한국어]
 * xnvme_fioe_open - 파일 open 콜백 (.open_file).
 *
 * @td: 잡 컨텍스트.
 * @f:  열 파일.
 * @return: 0=성공, 1=실패.
 *
 * 실제 xnvme_dev_open 은 init() 에서 이미 했으므로, 여기서는 슬롯 매핑 검증과
 * open 카운터 증가만 수행한다.
 */
static int xnvme_fioe_open(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	/* [한국어] 디버그 로그. */
	dprint(FD_FILE, "xnvme open %s -- nopen: %ld\n", f->file_name, xd->nopen);

	/* [한국어] 슬롯 범위 불변식 확인. */
	if (f->fileno > (int)xd->nallocated) {
		log_err("ioeng->open(): f->fileno > xd->nallocated; invalid assumption\n");
		return 1;
	}
	/* [한국어] init 에서 저장해둔 fio_file 과 일치하는지 확인. */
	if (xd->files[f->fileno].fio_file != f) {
		log_err("ioeng->open(): fio_file != f; invalid assumption\n");
		return 1;
	}

	/* [한국어] open 카운터 증가. */
	++(xd->nopen);

	return 0;
}

/*
 * [한국어]
 * xnvme_fioe_invalidate - 페이지 캐시 무효화 (.invalidate).
 *
 * @td/@f: 표준 시그니처 (현 구현은 미사용).
 * @return: 0.
 *
 * 대부분의 xNVMe 백엔드는 커널 페이지 캐시를 우회(direct)하므로 no-op.
 * SPDK 백엔드만 필요하다면 추후 구현 예정(주석 TODO 참조).
 */
static int xnvme_fioe_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* Consider only doing this with be:spdk */
	/* [한국어] SPDK 사용자 공간 드라이버는 캐시 자체가 없음 → 무시. */
	return 0;
}

/*
 * [한국어]
 * xnvme_fioe_get_max_open_zones - ZNS Maximum Open Resources 조회 (.get_max_open_zones).
 *
 * @td/@f: 표준. @max_open_zones: 출력 버퍼.
 * @return: 0=성공(또는 non-ZNS 무시), 음수=에러.
 *
 * init 이전에도 호출될 수 있어 자체적으로 장치를 열었다 닫는다. g_serialize 로 직렬화.
 */
static int xnvme_fioe_get_max_open_zones(struct thread_data *td, struct fio_file *f,
					 unsigned int *max_open_zones)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	const struct xnvme_spec_znd_idfy_ns *zns;
	int err = 0, err_lock;

	/* [한국어] 파일 타입이 일반 파일/블록/캐릭터가 아니면 ZNS 대상 아님 → 0 반환. */
	if (f->filetype != FIO_TYPE_FILE && f->filetype != FIO_TYPE_BLOCK &&
	    f->filetype != FIO_TYPE_CHAR) {
		log_info("ioeng->get_max_open_zoned(): ignoring filetype: %d\n", f->filetype);
		return 0;
	}
	/* [한국어] 장치 open/close 직렬화. */
	err_lock = pthread_mutex_lock(&g_serialize);
	if (err_lock) {
		log_err("ioeng->get_max_open_zones(): pthread_mutex_lock(), err(%d)\n", err_lock);
		return -err_lock;
	}

	/* [한국어] 임시 장치 오픈. */
	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		log_err("ioeng->get_max_open_zones(): xnvme_dev_open(), err(%d)\n", err_lock);
		err = -errno;
		goto exit;
	}
	/* [한국어] ZNS 지오메트리 아니면 EINVAL. */
	if (xnvme_dev_get_geo(dev)->type != XNVME_GEO_ZONED) {
		errno = EINVAL;
		err = -errno;
		goto exit;
	}

	/* [한국어] ZNS Identify Namespace 데이터 포인터 획득. */
	zns = (void *)xnvme_dev_get_ns_css(dev);
	if (!zns) {
		log_err("ioeng->get_max_open_zones(): xnvme_dev_get_ns_css(), err(%d)\n", errno);
		err = -errno;
		goto exit;
	}

	/*
	 * intentional overflow as the value is zero-based and NVMe
	 * defines 0xFFFFFFFF as unlimited thus overflowing to 0 which
	 * is how fio indicates unlimited and otherwise just converting
	 * to one-based.
	 */
	/* [한국어] MOR(Maximum Open Resources) 은 0-기반이므로 +1. 0xFFFFFFFF(= unlimited)
	 * 는 +1 시 0으로 오버플로우 → fio 관례(0 = unlimited)와 일치. */
	*max_open_zones = zns->mor + 1;

exit:
	/* [한국어] 임시 장치 닫기 + 락 해제. */
	xnvme_dev_close(dev);
	err_lock = pthread_mutex_unlock(&g_serialize);
	if (err_lock)
		log_err("ioeng->get_max_open_zones(): pthread_mutex_unlock(), err(%d)\n",
			err_lock);

	return err;
}

/**
 * Currently, this function is called before of I/O engine initialization, so,
 * we cannot consult the file-wrapping done when 'fioe' initializes.
 * Instead we just open based on the given filename.
 *
 * TODO: unify the different setup methods, consider keeping the handle around,
 * and consider how to support the --be option in this usecase
 */
/*
 * [한국어]
 * xnvme_fioe_get_zoned_model - ZBD 모델(없음/관리형/호스트 관리형) 분류 (.get_zoned_model).
 *
 * @td/@f: 표준. @model: 출력.
 * @return: 0=성공, 음수=에러.
 *
 * 엔진 init 이전에 호출되므로 자체 장치 open/close. g_serialize 로 직렬화.
 */
static int xnvme_fioe_get_zoned_model(struct thread_data *td, struct fio_file *f,
				      enum zbd_zoned_model *model)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	int err = 0, err_lock;

	/* [한국어] ZBD 의미가 없는 파일 타입은 EINVAL. */
	if (f->filetype != FIO_TYPE_FILE && f->filetype != FIO_TYPE_BLOCK &&
	    f->filetype != FIO_TYPE_CHAR) {
		log_info("ioeng->get_zoned_model(): ignoring filetype: %d\n", f->filetype);
		return -EINVAL;
	}

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		log_err("ioeng->get_zoned_model(): pthread_mutex_lock(), err(%d)\n", err);
		return -err;
	}

	/* [한국어] 임시로 장치 오픈. */
	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		log_err("ioeng->get_zoned_model(): xnvme_dev_open(%s) failed, errno: %d\n",
			f->file_name, errno);
		err = -errno;
		goto exit;
	}

	/* [한국어] 지오메트리 타입을 fio zbd 모델로 매핑. */
	switch (xnvme_dev_get_geo(dev)->type) {
	case XNVME_GEO_UNKNOWN:
		dprint(FD_ZBD, "%s: got 'unknown', assigning ZBD_NONE\n", f->file_name);
		*model = ZBD_NONE;
		break;

	case XNVME_GEO_CONVENTIONAL:
		dprint(FD_ZBD, "%s: got 'conventional', assigning ZBD_NONE\n", f->file_name);
		*model = ZBD_NONE;
		break;

	case XNVME_GEO_ZONED:
		/* [한국어] 순수 ZNS = 호스트 관리형 (SMR HM 과 동일하게 취급). */
		dprint(FD_ZBD, "%s: got 'zoned', assigning ZBD_HOST_MANAGED\n", f->file_name);
		*model = ZBD_HOST_MANAGED;
		break;

	default:
		dprint(FD_ZBD, "%s: hit-default, assigning ZBD_NONE\n", f->file_name);
		*model = ZBD_NONE;
		errno = EINVAL;
		err = -errno;
		break;
	}

exit:
	/* [한국어] 임시 dev 닫기. */
	xnvme_dev_close(dev);

	err_lock = pthread_mutex_unlock(&g_serialize);
	if (err_lock)
		log_err("ioeng->get_zoned_model(): pthread_mutex_unlock(), err(%d)\n", err_lock);

	return err;
}

/**
 * Fills the given ``zbdz`` with at most ``nr_zones`` zone-descriptors.
 *
 * The implementation converts the NVMe Zoned Command Set log-pages for Zone
 * descriptors into the Linux Kernel Zoned Block Report format.
 *
 * NOTE: This function is called before I/O engine initialization, that is,
 * before ``_dev_open`` has been called and file-wrapping is setup. Thus is has
 * to do the ``_dev_open`` itself, and shut it down again once it is done
 * retrieving the log-pages and converting them to the report format.
 *
 * TODO: unify the different setup methods, consider keeping the handle around,
 * and consider how to support the --async option in this usecase
 */
/*
 * [한국어]
 * xnvme_fioe_report_zones - ZNS Zone Descriptor 를 fio zbd_zone[] 로 변환 (.report_zones).
 *
 * @td/@f: 표준. @offset: 리포트 시작 바이트 오프셋. @zbdz: 출력 배열.
 * @nr_zones: 배열 용량.
 * @return: 성공 시 실제 변환된 존 수(양수), 실패 시 음수/1.
 *
 * NVMe Zoned Command Set 의 Report Zones 로그페이지를 Linux 커널 ZBD 리포트 포맷으로
 * 매핑한다. zone state/type 코드를 ZBD_ZONE_COND_*/TYPE_SWR 로 변환.
 */
static int xnvme_fioe_report_zones(struct thread_data *td, struct fio_file *f, uint64_t offset,
				   struct zbd_zone *zbdz, unsigned int nr_zones)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	const struct xnvme_spec_znd_idfy_lbafe *lbafe = NULL;
	struct xnvme_dev *dev = NULL;
	const struct xnvme_geo *geo = NULL;
	struct xnvme_znd_report *rprt = NULL;
	uint32_t ssw;
	uint64_t slba;
	unsigned int limit = 0;
	int err = 0, err_lock;

	/* [한국어] 디버그 로그: 요청 범위. */
	dprint(FD_ZBD, "%s: report_zones() offset: %zu, nr_zones: %u\n", f->file_name, offset,
	       nr_zones);

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		log_err("ioeng->report_zones(%s): pthread_mutex_lock(), err(%d)\n", f->file_name,
			err);
		return -err;
	}

	/* [한국어] 임시 장치 오픈. */
	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		log_err("ioeng->report_zones(%s): xnvme_dev_open(), err(%d)\n", f->file_name,
			errno);
		goto exit;
	}

	/* [한국어] 지오메트리 / 섹터 시프트 / LBA Format Extension 조회. */
	geo = xnvme_dev_get_geo(dev);
	ssw = xnvme_dev_get_ssw(dev);
	lbafe = xnvme_znd_dev_get_lbafe(dev);

	/* [한국어] 요청된 존 수와 디바이스 존 수 중 작은 값. */
	limit = nr_zones > geo->nzone ? geo->nzone : nr_zones;

	dprint(FD_ZBD, "%s: limit: %u\n", f->file_name, limit);

	/* [한국어] offset → LBA → 존 경계 SLBA 로 올림 내림. */
	slba = ((offset >> ssw) / geo->nsect) * geo->nsect;

	/* [한국어] Report Zones 수행 — 힙에 rprt 할당되어 반환(exit 에서 해제). */
	rprt = xnvme_znd_report_from_dev(dev, slba, limit, 0);
	if (!rprt) {
		log_err("ioeng->report_zones(%s): xnvme_znd_report_from_dev(), err(%d)\n",
			f->file_name, errno);
		err = -errno;
		goto exit;
	}
	/* [한국어] 엔트리 수 일치 검증. */
	if (rprt->nentries != limit) {
		log_err("ioeng->report_zones(%s): nentries != nr_zones\n", f->file_name);
		err = 1;
		goto exit;
	}
	/* [한국어] offset 이 디바이스 범위를 벗어나면 중단. */
	if (offset > geo->tbytes) {
		log_err("ioeng->report_zones(%s): out-of-bounds\n", f->file_name);
		goto exit;
	}

	/* Transform the zone-report */
	/* [한국어] 각 존 디스크립터 → zbd_zone 구조체로 변환. */
	for (uint32_t idx = 0; idx < rprt->nentries; ++idx) {
		struct xnvme_spec_znd_descr *descr = XNVME_ZND_REPORT_DESCR(rprt, idx);

		/* [한국어] LBA 단위를 바이트로 환산하여 저장. */
		zbdz[idx].start = descr->zslba << ssw;
		zbdz[idx].len = lbafe->zsze << ssw;
		zbdz[idx].capacity = descr->zcap << ssw;
		zbdz[idx].wp = descr->wp << ssw;

		/* [한국어] 존 타입 매핑: 현재는 SEQWR(Sequential Write Required) 만 허용. */
		switch (descr->zt) {
		case XNVME_SPEC_ZND_TYPE_SEQWR:
			zbdz[idx].type = ZBD_ZONE_TYPE_SWR;
			break;

		default:
			log_err("ioeng->report_zones(%s): invalid type for zone at offset(%zu)\n",
				f->file_name, zbdz[idx].start);
			err = -EIO;
			goto exit;
		}

		/* [한국어] 존 상태 매핑. */
		switch (descr->zs) {
		case XNVME_SPEC_ZND_STATE_EMPTY:
			zbdz[idx].cond = ZBD_ZONE_COND_EMPTY;
			break;
		case XNVME_SPEC_ZND_STATE_IOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_IMP_OPEN;
			break;
		case XNVME_SPEC_ZND_STATE_EOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_EXP_OPEN;
			break;
		case XNVME_SPEC_ZND_STATE_CLOSED:
			zbdz[idx].cond = ZBD_ZONE_COND_CLOSED;
			break;
		case XNVME_SPEC_ZND_STATE_FULL:
			zbdz[idx].cond = ZBD_ZONE_COND_FULL;
			break;

		case XNVME_SPEC_ZND_STATE_RONLY:
		case XNVME_SPEC_ZND_STATE_OFFLINE:
		default:
			/* [한국어] 읽기 전용/오프라인/미지원 상태는 OFFLINE 으로 통합. */
			zbdz[idx].cond = ZBD_ZONE_COND_OFFLINE;
			break;
		}
	}

exit:
	/* [한국어] 리포트 버퍼 해제 (xnvme_buf_virt 계열). */
	xnvme_buf_virt_free(rprt);

	/* [한국어] 임시 장치 닫기. */
	xnvme_dev_close(dev);

	err_lock = pthread_mutex_unlock(&g_serialize);
	if (err_lock)
		log_err("ioeng->report_zones(): pthread_mutex_unlock(), err: %d\n", err_lock);

	dprint(FD_ZBD, "err: %d, nr_zones: %d\n", err, (int)nr_zones);

	/* [한국어] 에러면 err, 아니면 변환된 존 수 반환. */
	return err ? err : (int)limit;
}

/**
 * NOTE: This function may get called before I/O engine initialization, that is,
 * before ``_dev_open`` has been called and file-wrapping is setup. In such
 * case it has to do ``_dev_open`` itself, and shut it down again once it is
 * done resetting write pointer of zones.
 */
/*
 * [한국어]
 * xnvme_fioe_reset_wp - 지정 범위의 존 write pointer 리셋 (.reset_wp).
 *
 * @td/@f: 표준. @offset/@length: 리셋할 바이트 범위.
 * @return: 0=성공, 음수=에러.
 *
 * io_ops_data 가 있으면 기존 핸들 재사용, 없으면(pre-init 경로) 자체 open/close.
 * 각 존 SLBA 에 대해 Zone Management Send (RESET) 을 동기 호출.
 */
static int xnvme_fioe_reset_wp(struct thread_data *td, struct fio_file *f, uint64_t offset,
			       uint64_t length)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_fioe_data *xd = NULL;
	struct xnvme_fioe_fwrap *fwrap = NULL;
	struct xnvme_dev *dev = NULL;
	const struct xnvme_geo *geo = NULL;
	uint64_t first, last;
	uint32_t ssw;
	uint32_t nsid;
	int err = 0, err_lock;

	/* [한국어] init 후 경로: 이미 열린 fwrap 재사용. */
	if (td->io_ops_data) {
		xd = td->io_ops_data;
		fwrap = &xd->files[f->fileno];

		assert(fwrap->dev);
		assert(fwrap->geo);

		dev = fwrap->dev;
		geo = fwrap->geo;
		ssw = fwrap->ssw;
	} else {
		/* [한국어] pre-init 경로: 전역 락 + 임시 장치 오픈. */
		err = pthread_mutex_lock(&g_serialize);
		if (err) {
			log_err("ioeng->reset_wp(): pthread_mutex_lock(), err(%d)\n", err);
			return -err;
		}

		dev = xnvme_dev_open(f->file_name, &opts);
		if (!dev) {
			log_err("ioeng->reset_wp(): xnvme_dev_open(%s) failed, errno(%d)\n",
				f->file_name, errno);
			goto exit;
		}
		geo = xnvme_dev_get_geo(dev);
		ssw = xnvme_dev_get_ssw(dev);
	}

	/* [한국어] NSID 조회 (관리 명령에 필요). */
	nsid = xnvme_dev_get_nsid(dev);

	/* [한국어] offset/length 를 존 경계로 정렬 (floor 후 nsect 배수로). */
	first = ((offset >> ssw) / geo->nsect) * geo->nsect;
	last = (((offset + length) >> ssw) / geo->nsect) * geo->nsect;
	dprint(FD_ZBD, "first: 0x%lx, last: 0x%lx\n", first, last);

	/* [한국어] 각 존 SLBA 마다 RESET 관리 명령 제출 (동기). */
	for (uint64_t zslba = first; zslba < last; zslba += geo->nsect) {
		struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);

		/* [한국어] 디바이스 범위 초과 시 조용히 중단. */
		if (zslba >= (geo->nsect * geo->nzone)) {
			log_err("ioeng->reset_wp(): out-of-bounds\n");
			err = 0;
			break;
		}

		/* [한국어] Zone Mgmt Send: RESET (비선택). */
		err = xnvme_znd_mgmt_send(&ctx, nsid, zslba, false,
					  XNVME_SPEC_ZND_CMD_MGMT_SEND_RESET, 0x0, NULL);
		if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
			err = err ? err : -EIO;
			log_err("ioeng->reset_wp(): err(%d), sc(%d)", err, ctx.cpl.status.sc);
			goto exit;
		}
	}

exit:
	/* [한국어] pre-init 경로에서만 자체 close + 락 해제. */
	if (!td->io_ops_data) {
		xnvme_dev_close(dev);

		err_lock = pthread_mutex_unlock(&g_serialize);
		if (err_lock)
			log_err("ioeng->reset_wp(): pthread_mutex_unlock(), err(%d)\n", err_lock);
	}

	return err;
}

/*
 * [한국어]
 * xnvme_fioe_fetch_ruhs - FDP Reclaim Unit Handle Status 조회 (.fdp_fetch_ruhs).
 *
 * @td/@f: 표준. @fruhs_info: 입력(nr_ruhs 용량)/출력(nr_ruhs 실제 + plis[]).
 * @return: 0=성공, 음수=에러.
 *
 * NVMe IO Management Receive 명령으로 RUHS 로그를 가져와 fio 가 FDP placement
 * directive 할당에 쓸 Placement Identifier(pi) 리스트를 채운다.
 */
static int xnvme_fioe_fetch_ruhs(struct thread_data *td, struct fio_file *f,
				 struct fio_ruhs_info *fruhs_info)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	struct xnvme_spec_ruhs *ruhs;
	struct xnvme_cmd_ctx ctx;
	uint32_t ruhs_nbytes, nr_ruhs;
	uint32_t nsid;
	int err = 0, err_lock;

	/* [한국어] 캐릭터/일반 파일만 허용 (블록 디바이스는 FDP 미지원 경로). */
	if (f->filetype != FIO_TYPE_CHAR && f->filetype != FIO_TYPE_FILE) {
		log_err("ioeng->fdp_ruhs(): ignoring filetype: %d\n", f->filetype);
		return -EINVAL;
	}

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		log_err("ioeng->fdp_ruhs(): pthread_mutex_lock(), err(%d)\n", err);
		return -err;
	}

	/* [한국어] 임시 장치 오픈. */
	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		log_err("ioeng->fdp_ruhs(): xnvme_dev_open(%s) failed, errno: %d\n",
			f->file_name, errno);
		err = -errno;
		goto exit;
	}

	/* [한국어] RUHS 버퍼 크기 = 헤더 + nr_ruhs * descriptor. */
	nr_ruhs = fruhs_info->nr_ruhs;
	ruhs_nbytes = sizeof(*ruhs) + (fruhs_info->nr_ruhs * sizeof(struct xnvme_spec_ruhs_desc));
	ruhs = xnvme_buf_alloc(dev, ruhs_nbytes);
	if (!ruhs) {
		err = -errno;
		goto exit;
	}
	/* [한국어] 수신 버퍼 초기화. */
	memset(ruhs, 0, ruhs_nbytes);

	/* [한국어] 동기 admin 경로 컨텍스트 준비 + NSID. */
	ctx = xnvme_cmd_ctx_from_dev(dev);
	nsid = xnvme_dev_get_nsid(dev);

	/* [한국어] IO Management Receive — RUHS 서브오퍼코드로 로그 수신. */
	err = xnvme_nvm_mgmt_recv(&ctx, nsid, XNVME_SPEC_IO_MGMT_RECV_RUHS, 0, ruhs, ruhs_nbytes);

	if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
		err = err ? err : -EIO;
		log_err("ioeng->fdp_ruhs(): err(%d), sc(%d)", err, ctx.cpl.status.sc);
		goto free_buffer;
	}

	/* [한국어] 실제 RUH descriptor 수를 반환값에 반영. */
	fruhs_info->nr_ruhs = ruhs->nruhsd;
	/* [한국어] 각 descriptor 의 Placement Identifier 를 little-endian → host 변환 후 저장. */
	for (uint32_t idx = 0; idx < nr_ruhs; ++idx) {
		fruhs_info->plis[idx] = le16_to_cpu(ruhs->desc[idx].pi);
	}

free_buffer:
	/* [한국어] DMA 버퍼 해제. */
	xnvme_buf_free(dev, ruhs);
exit:
	/* [한국어] 임시 장치 닫기 + 락 해제. */
	xnvme_dev_close(dev);

	err_lock = pthread_mutex_unlock(&g_serialize);
	if (err_lock)
		log_err("ioeng->fdp_ruhs(): pthread_mutex_unlock(), err(%d)\n", err_lock);

	return err;
}

/*
 * [한국어]
 * xnvme_fioe_get_file_size - 파일/디바이스 크기 조회 (.get_file_size).
 *
 * @td/@f: 표준.
 * @return: 0=성공, 음수=에러.
 *
 * init 이전에도 fio 가 파일 크기를 알아내기 위해 호출. 자체 open/close 로 geo->tbytes 취득.
 */
static int xnvme_fioe_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	int ret = 0, err;

	/* [한국어] 이미 알려진 경우 조기 반환. */
	if (fio_file_size_known(f))
		return 0;

	ret = pthread_mutex_lock(&g_serialize);
	if (ret) {
		log_err("ioeng->reset_wp(): pthread_mutex_lock(), err(%d)\n", ret);
		return -ret;
	}

	/* [한국어] 임시 장치 오픈. */
	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		log_err("%s: failed retrieving device handle, errno: %d\n", f->file_name, errno);
		ret = -errno;
		goto exit;
	}

	/* [한국어] geo->tbytes 로 총 바이트 크기 설정 + known 플래그. */
	f->real_file_size = xnvme_dev_get_geo(dev)->tbytes;
	fio_file_set_size_known(f);

	/* [한국어] ZBD 모드면 filetype 을 BLOCK 으로 강제(ZBD 경로 활성). */
	if (td->o.zone_mode == ZONE_MODE_ZBD)
		f->filetype = FIO_TYPE_BLOCK;

exit:
	/* [한국어] 임시 dev 닫기 + 락 해제. */
	xnvme_dev_close(dev);
	err = pthread_mutex_unlock(&g_serialize);
	if (err)
		log_err("ioeng->reset_wp(): pthread_mutex_unlock(), err(%d)\n", err);

	return ret;
}

/*
 * [한국어] 이 엔진이 fio 에 등록하는 콜백 테이블(ioengine_ops vtable).
 * fio 코어(ioengines.c)는 struct ioengine_ops 포인터를 engine_list 에 연결해두고
 * `--ioengine=xnvme` 요청 시 이 테이블을 찾아 td_io_* 디스패처에서 각 콜백을 호출.
 * 필드별 계약은 아래 §4 주석 참조.
 *
 * flags 비트 의미 (ioengine_ops.flags 비트마스크):
 *  - FIO_DISKLESSIO : 실제 파일시스템 파일이 아닌 장치/URI 도 허용. fio 코어가
 *                     파일 크기/생성 등 POSIX 가정을 완화.
 *  - FIO_NODISKUTIL : diskutil 통계(/proc/diskstats) 수집 비활성 — xNVMe 가
 *                     직접 장치를 관리해서 fio 의 OS 통계가 부정확할 수 있음.
 *  - FIO_NOEXTEND   : 파일 자동 확장 금지 (블록/NVMe 디바이스는 크기 고정).
 *  - FIO_MEMALIGN   : 버퍼 정렬 요구 (DMA 요건 — 페이지 또는 LBA 정렬).
 *  - FIO_RAWIO      : raw I/O 엔진 (커널 페이지 캐시 우회, O_DIRECT 와 유사 의미).
 * 미설정 비트: FIO_SYNCIO(아님 — 비동기 계약), FIO_PIPEIO(아님), FIO_NOIO(아님),
 *              FIO_ASYNCIO_SYNC_TRIM(이 엔진은 TRIM 미지원), FIO_FAKEIO(아님).
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name = "xnvme",
	/* [한국어] 엔진 식별자. `--ioengine=xnvme` 매칭.
	 * 설정자: 초기화 리터럴.
	 * 읽는 자: ioengines.c::find_ioengine(name) 이 engine_list 순회하며 strcmp. */

	.version = FIO_IOOPS_VERSION,
	/* [한국어] 엔진 ABI 버전. fio 코어와 불일치 시 로드 거부.
	 * 설정자: 빌드 시 fio.h 매크로.
	 * 읽는 자: register_ioengine() 이 검사. */

	.options = options,
	/* [한국어] 위에 정의한 옵션 테이블 포인터.
	 * 설정자: 리터럴.
	 * 읽는 자: fio 옵션 파서가 init 이전에 순회해 td->eo 에 값 저장. */

	.option_struct_size = sizeof(struct xnvme_fioe_options),
	/* [한국어] 옵션 구조체 크기(td->eo 버퍼 할당 크기).
	 * 설정자: sizeof.
	 * 읽는 자: fio 코어가 td->eo = calloc(1, option_struct_size) 로 할당. */

	.flags = FIO_DISKLESSIO | FIO_NODISKUTIL | FIO_NOEXTEND | FIO_MEMALIGN | FIO_RAWIO,
	/* [한국어] 위 주석 참조. fio 코어가 엔진의 특성을 파악해 파일 처리/통계 수집/
	 * 버퍼 정렬 정책을 조정. 설정자: 리터럴. 읽는 자: ioengines.c, backend.c,
	 * io_u.c 의 다양한 조건문. */

	.cleanup = xnvme_fioe_cleanup,
	/* [한국어] 잡 종료 시 1회 호출되는 엔진 정리 콜백. xd/iocq/iovec 해제, 파일
	 * 장치 닫기. 설정자: 리터럴. 읽는 자: backend.c::close_ioengine. */

	.init = xnvme_fioe_init,
	/* [한국어] 잡 시작 시 1회 호출되는 초기화 콜백. --thread=1 강제, xd 할당,
	 * 파일별 _dev_open. 반환 0=성공, 1=실패(잡 중단). 설정자: 리터럴.
	 * 읽는 자: backend.c::td_io_init. */

	.iomem_free = xnvme_fioe_iomem_free,
	/* [한국어] DMA 버퍼 해제 콜백. iomem_alloc 과 대응. 설정자: 리터럴.
	 * 읽는 자: io_u.c::free_io_mem. */

	.iomem_alloc = xnvme_fioe_iomem_alloc,
	/* [한국어] DMA 가능한 I/O 버퍼 할당 콜백 — fio 의 총 버퍼(orig_buffer) 할당.
	 * 백엔드(SPDK/hugepage)에 맞는 특수 메모리 필요해서 이 콜백 존재.
	 * 설정자: 리터럴. 읽는 자: io_u.c::init_io_u_buffers. */

	.io_u_free = xnvme_fioe_io_u_free,
	/* [한국어] io_u 당 엔진 상태 해제 콜백. 설정자: 리터럴.
	 * 읽는 자: io_u.c::__free_io_u. */

	.io_u_init = xnvme_fioe_io_u_init,
	/* [한국어] io_u 1개당 엔진 전용 상태(xnvme_fioe_request) 할당·연결 콜백.
	 * 설정자: 리터럴. 읽는 자: io_u.c::init_io_u (get_io_u 전 사전 초기화). */

	.event = xnvme_fioe_event,
	/* [한국어] 완료 큐에서 인덱스로 io_u 반환. getevents 가 반환한 N 만큼 호출.
	 * 설정자: 리터럴. 읽는 자: backend.c::io_u_queued_complete 루프. */

	.getevents = xnvme_fioe_getevents,
	/* [한국어] 완료 수확 — xNVMe 큐 poke 하여 cb_pool 호출 유도, iocq 에 적재.
	 * 반환값 = 수확된 완료 수. 설정자: 리터럴. 읽는 자: td_io_getevents. */

	.queue = xnvme_fioe_queue,
	/* [한국어] io_u → NVMe 명령 변환·제출. 반환값 계약:
	 *   FIO_Q_QUEUED    : 제출 성공, 완료는 getevents 경유.
	 *   FIO_Q_BUSY      : 일시 혼잡 — fio 가 재시도.
	 *   FIO_Q_COMPLETED : 즉시 완료(성공/실패 io_u->error 로 구분).
	 * 설정자: 리터럴. 읽는 자: td_io_queue. */

	.close_file = xnvme_fioe_close,
	/* [한국어] 파일 close 콜백. 실제 dev_close 는 cleanup 일괄 처리, 여기서는
	 * nopen 감소만. 설정자: 리터럴. 읽는 자: td_io_close_file. */

	.open_file = xnvme_fioe_open,
	/* [한국어] 파일 open 콜백. _dev_open 은 이미 init 에서 수행 — 슬롯 매핑
	 * 검증과 nopen 증가만. 설정자: 리터럴. 읽는 자: td_io_open_file. */

	.get_file_size = xnvme_fioe_get_file_size,
	/* [한국어] 장치 총 크기 조회 콜백(geo->tbytes). init 이전 경로용 자체 open/
	 * close. 설정자: 리터럴. 읽는 자: filesetup.c::get_file_sizes. */

	.invalidate = xnvme_fioe_invalidate,
	/* [한국어] 페이지 캐시 무효화 콜백. 대부분 xNVMe 백엔드는 direct 이므로 no-op.
	 * 설정자: 리터럴. 읽는 자: filesetup.c 의 invalidate 경로. */

	.get_max_open_zones = xnvme_fioe_get_max_open_zones,
	/* [한국어] ZNS Maximum Open Resources 조회 (zbd.c 요청). 설정자: 리터럴.
	 * 읽는 자: zbd.c::zbd_get_max_open_zones. */

	.get_zoned_model = xnvme_fioe_get_zoned_model,
	/* [한국어] 장치가 ZBD_NONE/HOST_MANAGED/HOST_AWARE 중 어느 모델인지 분류.
	 * 설정자: 리터럴. 읽는 자: zbd.c::zbd_get_zoned_model. */

	.report_zones = xnvme_fioe_report_zones,
	/* [한국어] NVMe Report Zones → fio zbd_zone[] 변환. 설정자: 리터럴.
	 * 읽는 자: zbd.c::zbd_report_zones. */

	.reset_wp = xnvme_fioe_reset_wp,
	/* [한국어] Zone Mgmt Send RESET 으로 write pointer 초기화. 설정자: 리터럴.
	 * 읽는 자: zbd.c::zbd_reset_range. */

	.fdp_fetch_ruhs = xnvme_fioe_fetch_ruhs,
	/* [한국어] FDP Reclaim Unit Handle Status 조회. 설정자: 리터럴.
	 * 읽는 자: dataplacement.c::fdp_init. */
};

/*
 * [한국어]
 * fio_xnvme_register - 공유 라이브러리 로드 시 자동 호출되는 생성자(constructor).
 *
 * @void: 파라미터 없음.
 * @return: 없음.
 *
 * fio_init 은 fio.h 가 제공하는 매크로로 __attribute__((constructor)) 를 풀어
 * 정의된다. GCC 의 이 속성은 함수를 .init_array 섹션에 등록하여, ld.so 동적
 * 로더(또는 정적 링크 시 _start)가 main() 호출 전에 순회하며 실행한다. 따라서
 * 이 엔진이 fio 바이너리에 정적 링크되든 외부 .so 로 dlopen 되든, 적재 시점에
 * 자동으로 엔진 레지스트리에 자신을 등록하게 된다.
 *
 * 실행 컨텍스트: 메인 스레드, main() 이전(정적 링크) 또는 dlopen 콜러 스레드
 * (동적 링크). 시그널 핸들러 문맥이 아님 — async-signal-safety 제약 없음.
 *
 * 호출 체인:
 *   ld.so / dlopen → .init_array 순회 → fio_xnvme_register → register_ioengine.
 */
static void fio_init fio_xnvme_register(void)
{
	/* [한국어] ioengines.c 의 등록 API — flist_add_tail 로 전역 engine_list 에
	 * ioengine 포인터 추가. 이후 find_ioengine("xnvme") 가 성공하여
	 * --ioengine=xnvme 로 선택 가능해진다. 중복 등록은 register_ioengine 내부
	 * 에서 경고 후 무시. */
	register_ioengine(&ioengine);
}

/*
 * [한국어]
 * fio_xnvme_unregister - 프로세스 종료/.so 언로드 시 호출되는 소멸자(destructor).
 *
 * @void: 파라미터 없음.
 * @return: 없음.
 *
 * fio_exit 매크로는 __attribute__((destructor)) 로 풀린다. GCC 의 이 속성은
 * 함수를 .fini_array 섹션에 등록하여, exit(3) / main() return 후 atexit 체인의
 * 일부로 실행된다. dlclose(3) 를 통한 .so 언로드 시에도 호출된다.
 *
 * 실행 컨텍스트: 메인 스레드 종료 경로. 이미 잡 스레드들은 join 되었고 fio
 * 코어의 대부분 리소스가 정리된 이후. 시그널 핸들러 문맥 아님.
 *
 * 호출 체인:
 *   exit() / dlclose() → .fini_array 역순 순회 → fio_xnvme_unregister →
 *   unregister_ioengine.
 */
static void fio_exit fio_xnvme_unregister(void)
{
	/* [한국어] 등록 해제 — flist_del_init 로 engine_list 에서 제거. 중복 언로드
	 * (dlclose 후 재 dlopen 같은 시나리오)에서 깨끗한 상태 유지를 위해 쌍으로
	 * 존재. 이후 find_ioengine("xnvme") 이 NULL 반환. */
	unregister_ioengine(&ioengine);
}
