/*
 * [한국어 설명] Ceph RBD(RADOS Block Device) I/O 엔진 구현 (rbd.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Ceph 분산 스토리지 클러스터의 블록 디바이스 인터페이스인 RBD(RADOS
 * Block Device)에 대해 fio가 직접 I/O 벤치마크를 수행할 수 있도록 해 주는
 * I/O 엔진(ioengine) 플러그인이다. 커널 rbd 모듈을 경유하지 않고 사용자
 * 공간의 librbd 라이브러리를 직접 호출하여, read/write/trim/flush를 모두
 * 비동기(aio) 방식으로 Ceph OSD에 전달한다. 선택적으로 CONFIG_RBD_POLL이
 * 정의되어 있으면 eventfd + poll(2)을 이용하여 완료 알림을 명시적으로
 * 수신하는 저지연 경로를 활성화하며, 미지정 시 완료 콜백이 설정한 플래그를
 * busy loop/대기 조합으로 확인한다. 또한 CONFIG_RBD_ENCRYPTION이 정의된
 * 빌드에서는 LUKS1/LUKS2 이미지에 대한 passphrase 기반 복호화를 지원한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 백엔드 실행 루프(backend.c: thread_main → do_io)는 잡마다 동일한
 * ioengine_ops 인터페이스를 통해 I/O를 수행한다. 이 엔진은 engines/ 디렉토리에
 * 위치한 40여 개의 엔진 중 하나이며, 호스트 유저스페이스에서 잡 스레드
 * 컨텍스트로 실행된다. 호출 체인은 다음과 같다:
 *   backend.c:do_io() → td_io_queue() [ioengines.c] → fio_rbd_queue()
 *     → rbd_aio_write/read/discard/flush() [librbd]
 *     → (커널 또는 사용자 네트워크 스택을 통해 Ceph MON/OSD로 RADOS 메시지 송신)
 *   완료 경로:
 *     librbd 내부 callback thread → _fio_rbd_finish_aiocb()
 *     → 잡 스레드의 fio_rbd_getevents() → rbd_iter_events() → fri_check_complete()
 * 실행 컨텍스트: librbd는 자체 메시저/콜백 스레드를 가지므로, 완료 콜백은
 * 잡 스레드가 아닌 librbd 내부 스레드에서 실행된다. 따라서 fri->io_complete
 * 플래그는 작성자와 독자가 서로 다른 스레드이므로 메모리 가시성을 고려해야
 * 한다(librbd가 내부적으로 동기화 장벽을 제공).
 *
 * === 타 모듈과의 연결 ===
 * - 의존 모듈:
 *   * librbd (rbd_aio_*, rbd_open, rbd_stat, rbd_set_image_notification,
 *            rbd_encryption_load, rbd_poll_io_events)
 *   * librados (rados_create/rados_create2, rados_connect, rados_ioctx_create,
 *              rados_conf_read_file, rados_shutdown, rados_conf_set)
 *   * eventfd(2), poll(2) — CONFIG_RBD_POLL 경로
 *   * fio 코어(fio.h, optgroup.h): ioengine_ops 계약, io_u, thread_data,
 *     td_verror, log_err, fio_ro_check 등
 * - 이 파일에 의존하는 모듈: 직접적인 정적 의존자는 없으며, 런타임에
 *   register_ioengine()을 통해 fio 코어의 엔진 목록에 이름 "rbd"로 등록된다.
 * - 데이터 흐름: fio가 생성한 io_u(오프셋/길이/버퍼/방향)는 fio_rbd_queue()에서
 *   OVERLAPPED 성격의 rbd_completion_t와 엮인 뒤 Ceph 클러스터로 전달되며,
 *   완료 시 반환값(-errno 또는 0)만 io_u->error/resid로 옮겨져 fio 통계 계층에
 *   반영된다. 실제 데이터 버퍼는 사용자 공간 io_u 버퍼를 직접 공유한다.
 * - 공유 자료구조: td->io_ops_data = struct rbd_data*, io_u->engine_data =
 *   struct fio_rbd_iou*. 두 구조체는 잡 스레드가 유일한 소유자이며,
 *   aio_events/sort_events 배열은 getevents 경로 내에서만 읽고 쓰인다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_rbd_setup(): td->files[0]에 이미지 크기를 등록, use_thread=1 강제.
 * - fio_rbd_init(): 잡 스레드에서 Ceph 클러스터에 (재)연결.
 * - _fio_rbd_connect() / _fio_rbd_disconnect(): RADOS 핸들·IOCTX·image 초기화/해제.
 * - fio_rbd_queue(): io_u 한 개를 rbd_aio_{read,write,discard,flush}로 변환 제출.
 * - _fio_rbd_finish_aiocb(): librbd가 호출하는 완료 콜백(io_complete 플래그 set).
 * - rbd_iter_events() / fio_rbd_getevents() / fio_rbd_event(): 완료 수집·보고.
 * - struct rbd_data: 잡 단위 엔진 상태(클러스터·IOCTX·image·aio_events·fd).
 * - struct fio_rbd_iou: io_u 단위 상태(completion 핸들과 완료/관측 플래그).
 * - struct rbd_options: fio CLI/잡 파일에서 받는 pool/image/client/암호화 옵션.
 */

/*
 * rbd engine
 *
 * IO engine using Ceph's librbd to test RADOS Block Devices.
 *
 */

/* [한국어] librbd의 공식 퍼블릭 헤더 — rbd_image_t 타입과 rbd_aio_* API를 선언.
 * 이 엔진의 모든 I/O 경로는 이 헤더에 정의된 심볼에 의존한다. */
#include <rbd/librbd.h>

/* [한국어] fio 핵심 타입 선언(thread_data, io_u, ioengine_ops 등). 상대경로는
 * engines/ 디렉토리에서 상위 트리의 fio.h를 가리킨다. */
#include "../fio.h"
/* [한국어] fio 옵션 그룹(FIO_OPT_C_ENGINE, FIO_OPT_G_RBD) 매크로/enum 정의. */
#include "../optgroup.h"

/* [한국어] CONFIG_RBD_POLL: configure 스크립트가 librbd의
 * rbd_set_image_notification()/rbd_poll_io_events() API 가용성을 검출한 경우에만
 * 정의된다. 이 분기에서는 eventfd + poll(2) 기반의 명시적 완료 수신 경로를 사용해
 * 사용자 스레드가 직접 완료 이벤트를 수신할 수 있다. */
#ifdef CONFIG_RBD_POLL
/* add for poll */
/* [한국어] poll(2) 구조체 pollfd 및 POLLIN 매크로 제공. */
#include <poll.h>
/* [한국어] eventfd(2) 시스템 호출과 EFD_SEMAPHORE 플래그 정의. librbd가
 * 완료마다 이 fd를 +1 하고, 엔진이 read()로 카운터를 감소시킨다. */
#include <sys/eventfd.h>
#endif

/* [한국어] RBD I/O 유닛 확장 데이터 — 각 io_u의 engine_data 슬롯에 할당되어
 * 완료 콜백과 잡 스레드 사이의 통신 채널 역할을 수행한다. */
struct fio_rbd_iou {
	struct io_u *io_u;
	/* [한국어] 이 RBD 요청과 짝이 되는 fio io_u 포인터.
	 * 설정자: fio_rbd_io_u_init()가 io_u 배열 초기화 시 1회 설정.
	 * 읽는 자: _fio_rbd_finish_aiocb() 콜백과 fri_check_complete()에서 역참조.
	 * 값 범위: 유효한 io_u* (put_io_u 전까지 유효, NULL 금지).
	 * 동기화: 잡당 단일 스레드만 접근하지만, 완료 콜백은 librbd 스레드에서
	 *         실행되므로 io_u 자체는 수정하지 않고 플래그만 set한다. */

	rbd_completion_t completion;
	/* [한국어] librbd가 반환하는 비동기 I/O 완료 핸들(불투명 포인터).
	 * 설정자: fio_rbd_queue()에서 rbd_aio_create_completion()으로 생성.
	 * 읽는 자: 완료 콜백이 rbd_aio_get_return_value()로 결과 조회,
	 *          fri_check_complete()가 rbd_aio_release()로 해제.
	 * 값 범위: librbd 관리 불투명 포인터 — 직접 접근 금지.
	 * 동기화: 하나의 completion은 반드시 1회 release 되어야 함(release 후 재사용 금지). */

	int io_seen;
	/* [한국어] 잡 스레드가 이 io_u의 완료를 이미 aio_events에 수확했는지 표시.
	 * 설정자: rbd_iter_events()에서 완료 수확 직후 1로 set, queue 직전 0으로 reset.
	 * 읽는 자: rbd_io_u_seen() — 같은 I/O를 두 번 보고하지 않기 위한 가드.
	 * 값 범위: 0(미관측) | 1(관측 후 fio에 보고 완료).
	 * 동기화: 잡 스레드에서만 접근(콜백 스레드는 건드리지 않음). */

	int io_complete;
	/* [한국어] librbd 완료 콜백이 "I/O 완료됨"을 잡 스레드에 알리는 플래그.
	 * 설정자: _fio_rbd_finish_aiocb() — librbd 내부 콜백 스레드에서 1로 set.
	 * 읽는 자: fri_check_complete() — 잡 스레드가 polling 방식으로 확인.
	 * 값 범위: 0(진행 중) | 1(완료 — 에러 여부는 io_u->error로 별도 전달).
	 * 동기화: 콜백 스레드 writer ↔ 잡 스레드 reader. librbd의 콜백 디스패치
	 *         경계가 release semantics를 제공하므로 별도 배리어 불필요하나,
	 *         엄밀히는 atomic 접근이 안전하다. */
};

/* [한국어] 잡(스레드) 단위의 RBD 엔진 상태 — td->io_ops_data에 매달려 있다. */
struct rbd_data {
	rados_t cluster;
	/* [한국어] RADOS 클러스터 연결 핸들(불투명).
	 * 설정자: _fio_rbd_connect() — rados_create/rados_create2()가 반환.
	 * 읽는 자: rados_connect/ioctx_create/shutdown 계열 호출.
	 * 값 범위: NULL(미연결) 또는 유효 핸들.
	 * 동기화: 잡당 1개이므로 단일 스레드 소유. */

	rados_ioctx_t io_ctx;
	/* [한국어] 특정 pool에 대한 RADOS I/O 컨텍스트.
	 * 설정자: _fio_rbd_connect() — rados_ioctx_create()의 출력.
	 * 읽는 자: rbd_open() 및 cleanup 시 rados_ioctx_destroy().
	 * 값 범위: NULL 또는 유효 IOCTX.
	 * 동기화: 동일 잡 스레드 및 librbd 내부 스레드가 간접 사용 — librbd가 보호. */

	rbd_image_t image;
	/* [한국어] RBD 이미지(블록 디바이스) 핸들 — 모든 I/O의 대상.
	 * 설정자: rbd_open() 성공 시.
	 * 읽는 자: rbd_aio_read/write/discard/flush, rbd_stat, rbd_flush,
	 *          rbd_encryption_load, rbd_set_image_notification 등.
	 * 값 범위: NULL 또는 유효 핸들.
	 * 동기화: librbd가 내부적으로 image별 동시성 제어. */

	struct io_u **aio_events;
	/* [한국어] getevents()가 fio에 보고할 "완료된 io_u"들의 순서 배열.
	 * 설정자: fri_check_complete()가 events 카운터 위치에 기록.
	 * 읽는 자: fio_rbd_event()가 인덱스로 역참조하여 io_u를 반환.
	 * 값 범위: 길이 iodepth — 각 슬롯은 유효 io_u* 또는 미초기화.
	 * 동기화: 잡 스레드 전용 — 멀티스레드 접근 없음. */

	struct io_u **sort_events;
	/* [한국어] wait=1 경로에서 미완료 I/O를 발행 시간순으로 정렬하기 위한 스크래치 버퍼.
	 * 설정자: rbd_iter_events()가 IO_U_F_FLIGHT 중 아직 관측되지 않은 io_u를 모아 저장.
	 * 읽는 자: qsort + 후속 fri_check_complete/rbd_aio_wait_for_complete 루프.
	 * 값 범위: 길이 iodepth. 각 getevents 호출 내에서만 유효(세션 간 재사용).
	 * 동기화: 잡 스레드 전용. */

	int fd;
	/* [한국어] eventfd(EFD_SEMAPHORE) 파일 디스크립터 — CONFIG_RBD_POLL 전용.
	 * 설정자: _fio_rbd_setup_poll()에서 eventfd(0, EFD_SEMAPHORE)로 생성,
	 *          rbd_set_image_notification()으로 librbd에 바인딩.
	 * 읽는 자: rbd_iter_events()의 poll()/read().
	 * 값 범위: -1(미사용/미빌드) 또는 유효 fd.
	 * 동기화: librbd 내부 스레드가 eventfd에 write, 잡 스레드가 read —
	 *         eventfd 자체가 커널 수준에서 원자 카운터를 보장. */

	bool connected;
	/* [한국어] 클러스터 연결 상태 캐시.
	 * 설정자: fio_rbd_setup()/fio_rbd_init()에서 true로, disconnect 시 의미 상 false.
	 * 읽는 자: fio_rbd_init()가 setup 단계의 사전 연결을 재활용할지 판단.
	 * 값 범위: false(미연결) | true(연결됨).
	 * 동기화: 잡 스레드 전용. */
};

/* [한국어] RBD 엔진 전용 fio 옵션(클러스터/풀/이미지/클라이언트/암호화)을 담는 구조체.
 * td->eo 포인터로 잡 파일·CLI에서 파싱된 값이 주입된다. */
struct rbd_options {
	void *pad;
	/* [한국어] fio 옵션 파서가 요구하는 정렬용 패딩. 실제 사용 필드 아님.
	 * 설정자/읽는 자: 없음. 값 범위: 미사용. 동기화: 해당 없음. */

	char *cluster_name;
	/* [한국어] Ceph 클러스터 이름(기본 "ceph"). NULL이면 rados_create() 경로 사용.
	 * 설정자: fio 옵션 파서(FIO_OPT_STR_STORE).
	 * 읽는 자: _fio_rbd_connect()가 rados_create2() 선택 조건으로 사용.
	 * 값 범위: NULL | NUL-terminated 문자열.
	 * 동기화: 파싱 완료 후 read-only. */

	char *rbd_name;
	/* [한국어] 열어야 할 RBD 이미지 이름. 필수 옵션.
	 * 설정자: 옵션 파서. 읽는 자: rbd_open().
	 * 값 범위: NULL(오류) | 이미지 명.
	 * 동기화: read-only. */

	char *pool_name;
	/* [한국어] RBD 이미지가 속한 RADOS pool 이름. 필수.
	 * 설정자: 파서. 읽는 자: rados_ioctx_create(). 값 범위: NULL 금지. */

	char *client_name;
	/* [한국어] Ceph 인증 클라이언트 이름("client.admin" 형태).
	 * 'client.' 접두가 없으면 _fio_rbd_connect()에서 붙여 준다.
	 * 설정자: 파서. 읽는 자: rados_create/rados_create2. 동기화: read-only. */

	int busy_poll;
	/* [한국어] 완료 대기 시 sleep 대신 busy-poll 사용 여부.
	 * 설정자: 파서(bool). 읽는 자: fio_rbd_getevents().
	 * 값 범위: 0(wait=1로 진입) | 1(nop; CPU 소모형 폴링).
	 * 동기화: read-only. */

	char *encryption_format;
	/* [한국어] "luks1" 또는 "luks2" — RBD 이미지의 암호화 포맷.
	 * 설정자: 파서. 읽는 자: _fio_rbd_setup_encryption().
	 * 값 범위: NULL(암호화 미사용) | "luks1" | "luks2". */

	char *encryption_passphrase;
	/* [한국어] 암호화 이미지 해제용 passphrase. encryption_format 지정 시 필수.
	 * 설정자: 파서. 읽는 자: _fio_rbd_setup_encryption().
	 * 값 범위: NULL(암호화 미사용) | 문자열. 민감 정보: 메모리 상주 주의. */
};

/* [한국어] fio의 옵션 테이블 — 이름/유형/오프셋/카테고리 메타데이터를 선언해
 * 공통 옵션 파서가 rbd_options의 필드를 자동으로 채울 수 있게 한다. 마지막
 * 엔트리는 .name=NULL 센티널로 종료된다.
 *
 * 공통 규약:
 *   .name         CLI/잡파일에서 사용자가 입력하는 키(예: "pool=rbd").
 *   .lname        사람이 읽기 좋은 긴 이름(HTML 도움말·gfio 툴팁 등에 사용).
 *   .type         FIO_OPT_STR_STORE(문자열 복사), FIO_OPT_BOOL(0/1) 등 — 파서가
 *                 이 값으로 값 변환 및 off1 위치에 주입할 형식을 결정.
 *   .off1         struct rbd_options 내 타겟 필드 오프셋(offsetof로 계산).
 *                 파서가 td->eo + off1 위치에 파싱 결과를 기록.
 *   .help         --cmdhelp, --enghelp 출력에 표시되는 한 줄 설명.
 *   .def          값이 지정되지 않은 경우 기본값(문자열로 제공).
 *   .category     옵션 카탈로그 분류 — FIO_OPT_C_ENGINE = "엔진 고유".
 *   .group        세부 그룹 — FIO_OPT_G_RBD = "RBD 엔진". 그룹 단위로 help 출력을
 *                 묶어 보여 주는 데 사용. */
static struct fio_option options[] = {
        {
		.name		= "clustername",
		/* [한국어] CLI 키: "clustername=<ceph 클러스터명>". 미지정 시 rados_create 경로,
		 * 지정 시 rados_create2 경로로 분기. */
		.lname		= "ceph cluster name",
		/* [한국어] 도움말 용 긴 이름. */
		.type		= FIO_OPT_STR_STORE,
		/* [한국어] 문자열 복사 저장(strdup 성격) — cluster_name 포인터로 할당된 문자열 소유권 이전. */
		.help		= "Cluster name for ceph",
		/* [한국어] --enghelp rbd 시 표시. */
		.off1		= offsetof(struct rbd_options, cluster_name),
		/* [한국어] 파서가 cluster_name 필드 위치에 결과 포인터 저장. */
		.category	= FIO_OPT_C_ENGINE,
		/* [한국어] 엔진 전용 옵션 분류. */
		.group		= FIO_OPT_G_RBD,
		/* [한국어] RBD 엔진 하위 그룹 — help 그룹핑. */
        },
	{
		.name		= "rbdname",
		/* [한국어] CLI 키: "rbdname=<image 이름>". 필수(미지정 시 _fio_rbd_connect에서 실패). */
		.lname		= "rbd engine rbdname",
		.type		= FIO_OPT_STR_STORE,
		/* [한국어] RBD 이미지 이름을 rbd_name 필드에 문자열로 저장 — rbd_open 인자로 사용. */
		.help		= "RBD name for RBD engine",
		.off1		= offsetof(struct rbd_options, rbd_name),
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_RBD,
	},
	{
		.name		= "pool",
		/* [한국어] CLI 키: "pool=<RADOS pool 이름>". 필수 — 이미지가 속한 pool. */
		.lname		= "rbd engine pool",
		.type		= FIO_OPT_STR_STORE,
		/* [한국어] rados_ioctx_create의 pool 인자로 전달. */
		.help		= "Name of the pool hosting the RBD for the RBD engine",
		.off1		= offsetof(struct rbd_options, pool_name),
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_RBD,
	},
	{
		.name		= "clientname",
		/* [한국어] CLI 키: "clientname=admin" 또는 "clientname=client.admin".
		 * '.' 미포함 시 _fio_rbd_connect에서 "client." 접두 자동 부여. */
		.lname		= "rbd engine clientname",
		.type		= FIO_OPT_STR_STORE,
		/* [한국어] CephX 인증 주체 — keyring 파일 검색 키로도 사용됨. */
		.help		= "Name of the ceph client to access the RBD for the RBD engine",
		.off1		= offsetof(struct rbd_options, client_name),
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_RBD,
	},
	{
		.name		= "busy_poll",
		/* [한국어] CLI 키: "busy_poll=1" — 완료 대기 시 sleep 대신 CPU 폴링 사용. */
		.lname		= "Busy poll",
		.type		= FIO_OPT_BOOL,
		/* [한국어] 0/1 파서. */
		.help		= "Busy poll for completions instead of sleeping",
		.off1		= offsetof(struct rbd_options, busy_poll),
		.def		= "0",
		/* [한국어] 기본값 off — 지정되지 않으면 블로킹 대기로 진입. */
		.category	= FIO_OPT_C_ENGINE,
		.group		= FIO_OPT_G_RBD,
	},
	{
        .name     = "rbd_encryption_format",
        /* [한국어] CLI 키: "rbd_encryption_format=luks1|luks2" — 암호화 이미지 복호화 포맷.
         * CONFIG_RBD_ENCRYPTION 빌드에서만 의미가 있으며, 그 외에는 경고 후 실패. */
        .lname    = "RBD Encryption Format",
        .type     = FIO_OPT_STR_STORE,
        /* [한국어] 문자열로 저장하여 _fio_rbd_setup_encryption에서 strcmp로 분기. */
        .off1     = offsetof(struct rbd_options, encryption_format),
        .help     = "RBD Encryption Format (luks1, luks2)",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_RBD,
    },
    {
        .name     = "rbd_encryption_passphrase",
        /* [한국어] CLI 키: "rbd_encryption_passphrase=<LUKS passphrase>".
         * encryption_format이 있는데 이 값이 없으면 즉시 에러 반환. */
        .lname    = "RBD Encryption Passphrase",
        .type     = FIO_OPT_STR_STORE,
        /* [한국어] 민감 정보 — 프로세스 메모리에 상주. 테스트 목적으로만 사용 권고. */
        .off1     = offsetof(struct rbd_options, encryption_passphrase),
        .help     = "Passphrase for unlocking the RBD image",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_RBD,
    },
	{
		.name = NULL,
		/* [한국어] 센티널: 옵션 파서의 순회 종료 조건. 이 항목을 제거하면 UB. */
	},
};

/*
 * [한국어]
 * _fio_setup_rbd_data - 잡당 struct rbd_data와 이벤트 배열을 최초 1회 할당한다.
 *
 * @td: 잡 컨텍스트. td->io_ops_data가 이미 세팅돼 있으면 재활용(중복 할당 방지).
 * @rbd_data_ptr: 할당된 rbd_data 포인터의 출력 위치.
 * @return: 0=성공, 1=할당 실패(부분 할당은 내부에서 모두 해제).
 *
 * fio_rbd_setup() 초기 단계에서 호출되며, aio_events와 sort_events를
 * iodepth 크기로 한꺼번에 확보해 이후 getevents 경로가 배열 경계만 믿고
 * 빠르게 동작하도록 한다. 실패 경로에서는 부분적으로 할당된 자원을 모두
 * 해제해 leak을 방지한다.
 *
 * 호출 체인: fio_rbd_setup() → [이 함수] → calloc(3)
 */
static int _fio_setup_rbd_data(struct thread_data *td,
			       struct rbd_data **rbd_data_ptr)
{
	struct rbd_data *rbd;	/* [한국어] 새로 만들 엔진 상태 포인터 작업용 지역 변수. */

	if (td->io_ops_data)
		/* [한국어] setup 경로가 init에서도 재호출될 수 있으므로, 이미 생성된 경우엔 조기 성공 반환. */
		return 0;

	rbd = calloc(1, sizeof(struct rbd_data));
	/* [한국어] zero-초기화 할당 — fd=-1 같은 기본값은 아래에서 명시적으로 재설정. */
	if (!rbd)
		goto failed;

	rbd->connected = false;
	/* [한국어] 아직 rados_connect() 이전이므로 명시적으로 미연결 상태로 마크. */

	/* add for poll, init fd: -1 */
	rbd->fd = -1;
	/* [한국어] eventfd를 쓰지 않는 빌드/경로를 대비해 invalid fd 센티널로 초기화.
	 * disconnect 시 -1이면 close() 건너뛰는 가드로도 사용된다. */

	rbd->aio_events = calloc(td->o.iodepth, sizeof(struct io_u *));
	/* [한국어] getevents에서 완료된 io_u를 채워 반환할 배열. iodepth 슬롯이면 충분. */
	if (!rbd->aio_events)
		goto failed;

	rbd->sort_events = calloc(td->o.iodepth, sizeof(struct io_u *));
	/* [한국어] wait 경로에서 in-flight io_u를 정렬용으로 담는 보조 배열. */
	if (!rbd->sort_events)
		goto failed;

	*rbd_data_ptr = rbd;
	/* [한국어] 호출자에게 완성된 구조체를 전달. */
	return 0;

failed:
	if (rbd) {
		/* [한국어] 부분 할당된 자원을 개별 해제 — 순서는 할당 역순 필요 없음(독립 free). */
		if (rbd->aio_events)
			free(rbd->aio_events);
		if (rbd->sort_events)
			free(rbd->sort_events);
		free(rbd);
	}
	return 1;

}

/* [한국어] CONFIG_RBD_ENCRYPTION: configure가 rbd_encryption_load() 심볼의 존재를
 * 확인한 경우에만 정의. 이 분기에서는 실제 LUKS 로드를 수행한다. */
#ifdef CONFIG_RBD_ENCRYPTION
/*
 * [한국어]
 * _fio_rbd_setup_encryption - CONFIG_RBD_ENCRYPTION 빌드에서 LUKS 포맷의 암호화 이미지를 연다.
 *
 * @rbd: 이미지 핸들 소유 엔진 상태.
 * @options: encryption_format/passphrase를 담은 옵션 구조체.
 * @return: true=암호화 미사용이거나 로드 성공, false=옵션 불완전/알 수 없는 포맷/로드 실패.
 *
 * 호출 체인: _fio_rbd_connect() → [이 함수] → rbd_encryption_load()
 */
static bool _fio_rbd_setup_encryption(struct rbd_data *rbd, struct rbd_options *options)
{
	rbd_encryption_format_t fmt;	/* [한국어] librbd에 전달할 포맷 enum. */
	void *opts_ptr = NULL;		/* [한국어] luks1/luks2 옵션 구조체의 범용 포인터. */
	size_t opts_size = 0;		/* [한국어] 옵션 구조체 크기(librbd가 구조체 버전 판별에 사용). */
	int r;				/* [한국어] librbd 반환 코드(<0=errno). */

	rbd_encryption_luks1_format_options_t luks1_opts;	/* [한국어] LUKS1용 옵션 스택 인스턴스. */
	rbd_encryption_luks2_format_options_t luks2_opts;	/* [한국어] LUKS2용 옵션 스택 인스턴스. */

	if (!options->encryption_format)
		return true; // No encryption requested
	/* [한국어] 사용자가 암호화를 요청하지 않았으면 성공으로 조기 반환. */

	if (!options->encryption_passphrase) {
		/* [한국어] 포맷만 있고 passphrase가 없으면 복호화 불가 — 명확한 에러 로그 후 실패. */
		log_err("rbd_encryption_passphrase is required when a rbd_encryption_format is specified.\n");
		return false;
	}

	if (!strcmp(options->encryption_format, "luks2")) {
		/* [한국어] LUKS2 경로: 구조체 0-초기화 후 passphrase/크기만 설정. */
		fmt = RBD_ENCRYPTION_FORMAT_LUKS2;
		memset(&luks2_opts, 0, sizeof(luks2_opts));
		luks2_opts.passphrase = options->encryption_passphrase;
		luks2_opts.passphrase_size = strlen(options->encryption_passphrase);
		opts_ptr = &luks2_opts;
		opts_size = sizeof(luks2_opts);
	} else if (!strcmp(options->encryption_format, "luks1")) {
		/* [한국어] LUKS1 경로: 동일한 패턴으로 구조체 구성. */
		fmt = RBD_ENCRYPTION_FORMAT_LUKS1;
		memset(&luks1_opts, 0, sizeof(luks1_opts));
		luks1_opts.passphrase = options->encryption_passphrase;
		luks1_opts.passphrase_size = strlen(options->encryption_passphrase);
		opts_ptr = &luks1_opts;
		opts_size = sizeof(luks1_opts);
	} else {
		/* [한국어] 지원하지 않는 포맷명 — 오타나 미지원 버전일 가능성. */
		log_err("rbd_encryption_load failed. Unknown rbd_encryption_format: %s\n", options->encryption_format);
		return false;
	}
	r = rbd_encryption_load(rbd->image, fmt, opts_ptr, opts_size);
	/* [한국어] librbd에 복호화 키 주입 — 이 이후의 I/O는 자동으로 복호화된다. */
	if (r < 0) {
		log_err("rbd_encryption_load failed.\n");
		return false;
	}
	return true;
}
#else
/*
 * [한국어]
 * _fio_rbd_setup_encryption (stub) - 암호화 미지원 빌드용 스텁.
 * 사용자가 암호화 옵션을 지정한 경우에만 버전 경고와 함께 실패 반환.
 */
static bool _fio_rbd_setup_encryption(struct rbd_data *rbd, struct rbd_options *options)
{
	if (options->encryption_format) {
		int major, minor, extra;	/* [한국어] librbd 버전 3요소. */
        rbd_version(&major, &minor, &extra);
        /* [한국어] 설치된 librbd 버전을 사용자에게 알려 주기 위해 조회. */

        log_err("rbd encryption requested but not supported by this librbd version (%d.%d.%d).\n",
                major, minor, extra);
        return false;
    }
	return true;
}
#endif

/* [한국어] CONFIG_RBD_POLL 분기: eventfd+poll 완료 경로 빌드 여부. */
#ifdef CONFIG_RBD_POLL
/*
 * [한국어]
 * _fio_rbd_setup_poll - eventfd를 만들고 librbd 이미지에 완료 알림 채널로 등록.
 * @rbd: 대상 엔진 상태.
 * @return: true=성공, false=eventfd 생성 또는 알림 등록 실패.
 *
 * 호출 체인: _fio_rbd_connect() → [이 함수] → eventfd(2) / rbd_set_image_notification()
 */
static bool _fio_rbd_setup_poll(struct rbd_data *rbd)
{
	int r;	/* [한국어] rbd_set_image_notification 반환값. */

	/* add for rbd poll */
	rbd->fd = eventfd(0, EFD_SEMAPHORE);
	/* [한국어] SEMAPHORE 모드 eventfd: read 시마다 1씩 감소 → 완료 1건당 1회 수신 보장. */
	if (rbd->fd < 0) {
		log_err("eventfd failed.\n");
		return false;
	}

	r = rbd_set_image_notification(rbd->image, rbd->fd, EVENT_TYPE_EVENTFD);
	/* [한국어] librbd가 이 이미지의 완료 이벤트를 위 fd에 "+1 write" 하도록 등록. */
	if (r < 0) {
		log_err("rbd_set_image_notification failed.\n");
		close(rbd->fd);
		rbd->fd = -1;
		return false;
	}

	return true;
}
#else
/*
 * [한국어]
 * _fio_rbd_setup_poll (stub) - poll 경로가 빌드되지 않았을 때의 no-op.
 * 완료 수집은 fri->io_complete 플래그 polling으로 대체된다.
 */
static bool _fio_rbd_setup_poll(struct rbd_data *rbd)
{
	return true;
}
#endif

/*
 * [한국어]
 * _fio_rbd_connect - 클러스터 생성·연결, pool IOCTX 생성, 이미지 open까지 전체 초기화.
 *
 * @td: 잡 컨텍스트. td->io_ops_data에 미리 _fio_setup_rbd_data()로 할당된 rbd_data가 있어야 한다.
 * @return: 0=성공, 1=단계별 실패(실패 단계에 따라 goto 라벨로 부분 정리 수행).
 *
 * 호출 체인: fio_rbd_setup() / fio_rbd_init() → [이 함수]
 *   → rados_create(2) → rados_conf_read_file → rados_connect
 *   → rados_ioctx_create → rbd_open → _fio_rbd_setup_encryption → _fio_rbd_setup_poll
 *
 * 실패 시에도 cluster/ioctx/image 핸들이 부분적으로 살아있지 않도록,
 * 실패한 시점에 따라 failed_early/shutdown/open/post_open 라벨로 점진적
 * 롤백을 수행한다.
 */
static int _fio_rbd_connect(struct thread_data *td)
{
	struct rbd_data *rbd = td->io_ops_data;	/* [한국어] 엔진 상태 포인터(반드시 미리 할당되어 있어야 함). */
	struct rbd_options *o = td->eo;		/* [한국어] 옵션 구조체(cluster/pool/rbd/client 등). */
	int r;					/* [한국어] librbd/librados 반환값 수신. */

	if (o->cluster_name) {
		/* [한국어] 사용자가 클러스터명을 지정한 경우: rados_create2 경로로 분기. */
		char *client_name = NULL;

		/*
		 * If we specify cluster name, the rados_create2
		 * will not assume 'client.'. name is considered
		 * as a full type.id namestr
		 */
		if (o->client_name) {
			if (!index(o->client_name, '.')) {
				/* [한국어] '.'이 없으면 "client." 접두를 붙여 type.id 형태로 정규화. */
				client_name = calloc(1, strlen("client.") +
						    strlen(o->client_name) + 1);
				strcat(client_name, "client.");
				strcat(client_name, o->client_name);
			} else {
				/* [한국어] 이미 type.id 형태면 그대로 전달(소유권은 옵션 쪽). */
				client_name = o->client_name;
			}
		}

		r = rados_create2(&rbd->cluster, o->cluster_name,
				 client_name, 0);
		/* [한국어] 클러스터명·전체 클라이언트명·flags(0)로 RADOS 핸들 생성. */

		if (client_name && !index(o->client_name, '.'))
			free(client_name);
		/* [한국어] 우리가 새로 할당한 경우에 한해 해제. */
	} else
		r = rados_create(&rbd->cluster, o->client_name);
		/* [한국어] 클러스터명 미지정 시: 'client.' 접두를 내부에서 가정하는 단순 API 사용. */

	if (r < 0) {
		log_err("rados_create failed.\n");
		goto failed_early;
	}
	if (o->pool_name == NULL) {
		log_err("rbd pool name must be provided.\n");
		goto failed_early;
	}
	if (!o->rbd_name) {
		log_err("rbdname must be provided.\n");
		goto failed_early;
	}

	r = rados_conf_read_file(rbd->cluster, NULL);
	/* [한국어] 기본 ceph.conf 경로에서 설정 로드(NULL=라이브러리 기본). */
	if (r < 0) {
		log_err("rados_conf_read_file failed.\n");
		goto failed_early;
	}

	r = rados_connect(rbd->cluster);
	/* [한국어] MON에 실제 TCP 연결 수립 및 인증. 이후 IO 경로가 가능해진다. */
	if (r < 0) {
		log_err("rados_connect failed.\n");
		goto failed_shutdown;
	}

	r = rados_ioctx_create(rbd->cluster, o->pool_name, &rbd->io_ctx);
	/* [한국어] 특정 pool에 대한 IOCTX 확보 — RBD 이미지는 pool 안에 위치. */
	if (r < 0) {
		log_err("rados_ioctx_create failed.\n");
		goto failed_shutdown;
	}

        if (td->o.odirect) {
		/* [한국어] O_DIRECT 유사 의미: RBD 인메모리 캐시를 비활성화하여 디바이스 동작을 모사. */
		r = rados_conf_set(rbd->cluster, "rbd_cache", "false");
		if (r < 0) {
			log_info("failed to disable RBD in-memory cache\n");
		}
	}

	r = rbd_open(rbd->io_ctx, o->rbd_name, &rbd->image, NULL /*snap */ );
	/* [한국어] 스냅샷 지정 없이 읽기/쓰기 가능한 head image 열기. */
	if (r < 0) {
		log_err("rbd_open failed.\n");
		goto failed_open;
	}

	if (!td->o.odirect) {
		/*
		 * ensure cache enables writeback/around mode unless explicitly
		 * configured for writethrough mode
		 */
		r = rbd_flush(rbd->image);
		/* [한국어] 첫 flush로 캐시 모드(writeback/around) 확정을 유도. */
		if (r < 0) {
			log_info("rbd: failed to issue initial flush\n");
		}
	}

	if (!_fio_rbd_setup_encryption(rbd, o))
		goto failed_post_open;
	/* [한국어] 암호화 옵션이 있으면 LUKS 키 로드 — 이후 I/O가 투명 복호화된다. */

	if (!_fio_rbd_setup_poll(rbd))
		goto failed_post_open;
	/* [한국어] poll 빌드라면 eventfd 등록까지 완료. */

	return 0;

failed_post_open:
	/* [한국어] image까지 열렸다가 이후 단계에서 실패 — image 닫고 단계적 롤백. */
	rbd_close(rbd->image);
	rbd->image = NULL;
failed_open:
	rados_ioctx_destroy(rbd->io_ctx);
	rbd->io_ctx = NULL;
failed_shutdown:
	rados_shutdown(rbd->cluster);
	rbd->cluster = NULL;
failed_early:
	return 1;
}

/*
 * [한국어]
 * _fio_rbd_disconnect - 잡 종료 시 이미지/IOCTX/cluster/eventfd를 순차 해제.
 * @rbd: NULL 허용. NULL이면 즉시 반환.
 *
 * 호출 체인: fio_rbd_cleanup() → [이 함수] → rbd_close/ioctx_destroy/rados_shutdown/close
 */
static void _fio_rbd_disconnect(struct rbd_data *rbd)
{
	if (!rbd)
		return;

	/* close eventfd */
	if (rbd->fd != -1) {
		close(rbd->fd);
		rbd->fd = -1;
	}

	/* shutdown everything */
	if (rbd->image) {
		rbd_close(rbd->image);
		rbd->image = NULL;
	}

	if (rbd->io_ctx) {
		rados_ioctx_destroy(rbd->io_ctx);
		rbd->io_ctx = NULL;
	}

	if (rbd->cluster) {
		rados_shutdown(rbd->cluster);
		rbd->cluster = NULL;
	}
}

/*
 * [한국어]
 * _fio_rbd_finish_aiocb - librbd가 각 비동기 I/O 완료 시 호출하는 콜백.
 *
 * @comp: librbd 완료 핸들(여기서는 직접 사용하지 않음, fri->completion과 동일).
 * @data: rbd_aio_create_completion에 넘긴 fri 포인터.
 *
 * 실행 컨텍스트: librbd 내부 콜백 디스패처 스레드(잡 스레드 아님!).
 * 하는 일: 반환값을 읽어 io_u->error/resid를 세팅하고, fri->io_complete=1로
 *          잡 스레드에 완료를 시그널.
 * 주의: 여기서는 rbd_aio_release()를 호출하지 않는다 — 해제는 잡 스레드의
 *       fri_check_complete에서 수행해 race를 회피.
 *
 * 호출 체인: librbd callback thread → [이 함수] (→ 잡 스레드의 fri_check_complete)
 */
static void _fio_rbd_finish_aiocb(rbd_completion_t comp, void *data)
{
	struct fio_rbd_iou *fri = data;		/* [한국어] queue 시 인자로 넘긴 iou. */
	struct io_u *io_u = fri->io_u;		/* [한국어] 대응 io_u 복원. */
	ssize_t ret;				/* [한국어] librbd 결과(바이트 수 또는 -errno). */

	/*
	 * Looks like return value is 0 for success, or < 0 for
	 * a specific error. So we have to assume that it can't do
	 * partial completions.
	 */
	ret = rbd_aio_get_return_value(fri->completion);
	if (ret < 0) {
		/* [한국어] 오류: -errno를 양수로 변환해 io_u에 기록, 전체를 잔여로 표시. */
		io_u->error = -ret;
		io_u->resid = io_u->xfer_buflen;
	} else
		io_u->error = 0;
		/* [한국어] librbd는 부분 완료를 가정하지 않으므로 성공=전체 전송. */

	fri->io_complete = 1;
	/* [한국어] 잡 스레드가 polling으로 관측할 완료 플래그. 반드시 마지막에 set. */
}

/*
 * [한국어]
 * fio_rbd_event - getevents가 보고한 event 인덱스에 대응하는 io_u 반환.
 * @td: 잡. @event: 0..getevents 반환값-1 범위.
 * @return: aio_events[event] io_u 포인터.
 *
 * 호출 체인: td_io_getevents → [이 함수]
 */
static struct io_u *fio_rbd_event(struct thread_data *td, int event)
{
	struct rbd_data *rbd = td->io_ops_data;

	return rbd->aio_events[event];
}

/*
 * [한국어]
 * fri_check_complete - 하나의 io_u가 완료되었는지 확인하고 완료됐다면 aio_events에 등록.
 *
 * @rbd: 엔진 상태.
 * @io_u: 대상.
 * @events: 현재까지 수집된 이벤트 개수(완료 시 여기서 증가, in-out).
 * @return: 1=완료/등록됨, 0=아직 미완료.
 *
 * 완료 콜백이 set한 fri->io_complete 플래그를 관측하면 io_seen=1로 중복 보고를
 * 방지하고, completion 핸들을 release해 librbd 리소스를 회수한다.
 *
 * 호출 체인: rbd_iter_events → [이 함수] → rbd_aio_release
 */
static inline int fri_check_complete(struct rbd_data *rbd, struct io_u *io_u,
				     unsigned int *events)
{
	struct fio_rbd_iou *fri = io_u->engine_data;

	if (fri->io_complete) {
		fri->io_seen = 1;			/* [한국어] 중복 수확 방지. */
		rbd->aio_events[*events] = io_u;	/* [한국어] 현재 보고 슬롯에 등록. */
		(*events)++;				/* [한국어] 수집 개수 증가. */

		rbd_aio_release(fri->completion);	/* [한국어] completion 핸들 회수 — 재사용 금지. */
		return 1;
	}

	return 0;
}

/* [한국어] poll 빌드가 아닐 때만 필요한 헬퍼 — 이미 관측된 io_u를 건너뛰기 위해 사용. */
#ifndef CONFIG_RBD_POLL
/*
 * [한국어]
 * rbd_io_u_seen - fri->io_seen 플래그 조회(중복 수확 가드).
 */
static inline int rbd_io_u_seen(struct io_u *io_u)
{
	struct fio_rbd_iou *fri = io_u->engine_data;

	return fri->io_seen;
}
#endif

/*
 * [한국어]
 * rbd_io_u_wait_complete - 단일 io_u의 완료를 librbd에 동기 대기.
 * wait=1 경로에서 가장 오래된 I/O에 대해 블로킹 대기할 때 사용.
 */
static void rbd_io_u_wait_complete(struct io_u *io_u)
{
	struct fio_rbd_iou *fri = io_u->engine_data;

	rbd_aio_wait_for_complete(fri->completion);
	/* [한국어] 이 호출은 완료 콜백이 반드시 실행된 이후 반환됨을 librbd가 보장. */
}

/*
 * [한국어]
 * rbd_io_u_cmp - qsort 비교 함수 — 발행 시각이 오래된 io_u가 앞에 오도록 정렬.
 * @return: -1/0/1 표준 qsort 규약.
 */
static int rbd_io_u_cmp(const void *p1, const void *p2)
{
	const struct io_u **a = (const struct io_u **) p1;
	const struct io_u **b = (const struct io_u **) p2;
	uint64_t at, bt;	/* [한국어] 각 io_u의 "발행 이후 경과 μs". 클수록 오래된 I/O. */

	at = utime_since_now(&(*a)->start_time);
	bt = utime_since_now(&(*b)->start_time);

	if (at < bt)
		return -1;	/* [한국어] a가 더 최근이므로 뒤로. */
	else if (at == bt)
		return 0;
	else
		return 1;	/* [한국어] a가 더 오래됐으므로 앞으로. */
}

/*
 * [한국어]
 * rbd_iter_events - 한 번의 완료 수집 패스를 수행한다.
 *
 * @td: 잡. @events: in-out 완료 카운터. @min_evts: 최소 이벤트 수(wait 판단 참고).
 * @wait: 1이면 부족 시 대기 허용, 0이면 non-blocking.
 * @return: 이번 호출에서 새로 수확한 이벤트 수.
 *
 * CONFIG_RBD_POLL 분기에서는 eventfd에 대해 poll(2)로 대기한 뒤
 * rbd_poll_io_events()로 완료된 completion 배열을 얻어와 수확한다.
 * 비poll 분기에서는 in-flight io_u를 순회하며 fri->io_complete를 관측하고,
 * wait=1인 경우 오래된 것부터 rbd_aio_wait_for_complete로 동기 대기한다.
 *
 * 호출 체인: fio_rbd_getevents → [이 함수]
 *   → poll(2)/read(2)/rbd_poll_io_events (poll 빌드)
 *   → io_u_qiter + rbd_aio_wait_for_complete (비poll 빌드)
 */
static int rbd_iter_events(struct thread_data *td, unsigned int *events,
			   unsigned int min_evts, int wait)
{
	struct rbd_data *rbd = td->io_ops_data;
	unsigned int this_events = 0;	/* [한국어] 이번 호출에서 수확한 개수. */
	struct io_u *io_u;
	int i, sidx = 0;		/* [한국어] sort_events 채우는 인덱스. */

#ifdef CONFIG_RBD_POLL
	int ret = 0;
	int event_num = 0;
	struct fio_rbd_iou *fri = NULL;
	rbd_completion_t comps[min_evts];	/* [한국어] VLA: 최대 min_evts개 완료 핸들 수신용. */
	uint64_t counter;			/* [한국어] eventfd read 결과(세마포어 모드에서 1). */
	bool completed;

	struct pollfd pfd;
	pfd.fd = rbd->fd;
	pfd.events = POLLIN;			/* [한국어] eventfd에 write가 발생하면 readable로 관측. */

	ret = poll(&pfd, 1, wait ? -1 : 0);
	/* [한국어] wait=1이면 무한 대기(-1), 아니면 non-blocking(0). */
	if (ret <= 0)
		return 0;
	if (!(pfd.revents & POLLIN))
		return 0;

	event_num = rbd_poll_io_events(rbd->image, comps, min_evts);
	/* [한국어] 지금 완료된 것들을 최대 min_evts개까지 배치로 회수. */

	for (i = 0; i < event_num; i++) {
		fri = rbd_aio_get_arg(comps[i]);
		/* [한국어] completion에 바인딩된 fri 포인터 역획득. */
		io_u = fri->io_u;

		/* best effort to decrement the semaphore */
		ret = read(rbd->fd, &counter, sizeof(counter));
		/* [한국어] 세마포어 모드 eventfd: 이벤트 1건마다 read로 카운터 1씩 감소. */
		if (ret <= 0)
			log_err("rbd_iter_events failed to decrement semaphore.\n");

		completed = fri_check_complete(rbd, io_u, events);
		assert(completed);
		/* [한국어] librbd가 완료 배열을 줬으므로 반드시 io_complete=1이어야 함. */

		this_events++;
	}
#else
	io_u_qiter(&td->io_u_all, io_u, i) {
		/* [한국어] 잡에 할당된 모든 io_u를 순회 — FLIGHT인 것만 조사. */
		if (!(io_u->flags & IO_U_F_FLIGHT))
			continue;
		if (rbd_io_u_seen(io_u))
			continue;		/* [한국어] 이미 이전 getevents에서 수확됨. */

		if (fri_check_complete(rbd, io_u, events))
			this_events++;
		else if (wait)
			rbd->sort_events[sidx++] = io_u;
			/* [한국어] 완료 안 됐고 대기 허용이면 나중에 정렬해서 대기 대상에 추가. */
	}
#endif

	if (!wait || !sidx)
		return this_events;
	/* [한국어] 대기 불가거나 대기 대상 없으면 현재까지 결과만 반환. */

	/*
	 * Sort events, oldest issue first, then wait on as many as we
	 * need in order of age. If we have enough events, stop waiting,
	 * and just check if any of the older ones are done.
	 */
	if (sidx > 1)
		qsort(rbd->sort_events, sidx, sizeof(struct io_u *), rbd_io_u_cmp);
	/* [한국어] 오래된 I/O부터 처리해 latency tail을 제한. */

	for (i = 0; i < sidx; i++) {
		io_u = rbd->sort_events[i];

		if (fri_check_complete(rbd, io_u, events)) {
			this_events++;
			continue;	/* [한국어] 정렬 도중 완료됐을 수도 있으니 먼저 관측. */
		}

		/*
		 * Stop waiting when we have enough, but continue checking
		 * all pending IOs if they are complete.
		 */
		if (*events >= min_evts)
			continue;	/* [한국어] 이미 충분 — 이후는 블로킹하지 않고 관측만. */

		rbd_io_u_wait_complete(io_u);
		/* [한국어] 가장 오래된 미완료 I/O부터 동기 대기. */

		if (fri_check_complete(rbd, io_u, events))
			this_events++;
	}

	return this_events;
}

/*
 * [한국어]
 * fio_rbd_getevents - ioengine_ops.getevents 구현.
 *
 * @td: 잡. @min: 최소 요구 이벤트 수. @max: 상한(여기선 상한 구현 생략).
 * @t: 타임스펙(현재 엔진은 무시).
 * @return: 실제로 수집된 이벤트 수.
 *
 * rbd_iter_events를 반복 호출해 min이 충족될 때까지 진행한다. busy_poll=0
 * 이면 두 번째 반복부터 wait=1로 전환하여 블로킹 대기, busy_poll=1이면 CPU
 * 를 소모하며 폴링(nop은 pause/backoff 힌트).
 *
 * 호출 체인: td_io_getevents → [이 함수] → rbd_iter_events → poll/rbd_poll_io_events
 */
static int fio_rbd_getevents(struct thread_data *td, unsigned int min,
			     unsigned int max, const struct timespec *t)
{
	unsigned int this_events, events = 0;
	struct rbd_options *o = td->eo;
	int wait = 0;	/* [한국어] 첫 패스는 non-blocking으로 시작(최근 완료 먼저 수확). */

	do {
		this_events = rbd_iter_events(td, &events, min, wait);

		if (events >= min)
			break;
		if (this_events)
			continue;	/* [한국어] 진전이 있다면 또 non-blocking으로 한 번 더. */

		if (!o->busy_poll)
			wait = 1;	/* [한국어] 진전 없음 + busy_poll off → 블로킹 대기 모드로. */
		else
			nop;		/* [한국어] CPU 완화용 pause 힌트(아키텍처별). */
	} while (1);

	return events;
}

/*
 * [한국어]
 * fio_rbd_queue - 하나의 io_u를 librbd aio로 제출한다(ioengine_ops.queue).
 *
 * @td: 잡. @io_u: read/write/trim/sync 중 하나인 I/O 유닛.
 * @return: FIO_Q_QUEUED(비동기 제출 성공) 또는 FIO_Q_COMPLETED(즉시 완료/에러).
 *
 * 과정:
 *  1) fri 플래그(seen/complete) reset.
 *  2) rbd_aio_create_completion(fri, _fio_rbd_finish_aiocb) 로 완료 훅 등록.
 *  3) ddir에 따라 rbd_aio_{read,write,discard,flush} 분기 호출.
 *  4) 성공 시 FIO_Q_QUEUED, 실패 시 completion 해제 + 오류 기록 + COMPLETED.
 *
 * 호출 체인: td_io_queue → [이 함수] → rbd_aio_*
 *            → (완료 시) _fio_rbd_finish_aiocb
 */
static enum fio_q_status fio_rbd_queue(struct thread_data *td,
				       struct io_u *io_u)
{
	struct rbd_data *rbd = td->io_ops_data;
	struct fio_rbd_iou *fri = io_u->engine_data;
	int r = -1;

	fio_ro_check(td, io_u);
	/* [한국어] read-only 잡인데 쓰기 요청 들어왔는지 같은 안전성 검증. */

	fri->io_seen = 0;	/* [한국어] 새 제출이므로 관측 플래그 초기화. */
	fri->io_complete = 0;	/* [한국어] 완료 플래그도 초기화 — 콜백이 다시 set할 예정. */

	r = rbd_aio_create_completion(fri, _fio_rbd_finish_aiocb,
						&fri->completion);
	/* [한국어] (사용자 데이터=fri, 콜백=_fio_rbd_finish_aiocb)로 completion 핸들 생성. */
	if (r < 0) {
		log_err("rbd_aio_create_completion failed.\n");
		goto failed;
	}

	if (io_u->ddir == DDIR_WRITE) {
		r = rbd_aio_write(rbd->image, io_u->offset, io_u->xfer_buflen,
					 io_u->xfer_buf, fri->completion);
		/* [한국어] 오프셋/길이/버퍼를 Ceph OSD로 비동기 쓰기. */
		if (r < 0) {
			log_err("rbd_aio_write failed.\n");
			goto failed_comp;
		}

	} else if (io_u->ddir == DDIR_READ) {
		r = rbd_aio_read(rbd->image, io_u->offset, io_u->xfer_buflen,
					io_u->xfer_buf, fri->completion);
		/* [한국어] 비동기 읽기: 완료 시점에 버퍼가 채워진다. */

		if (r < 0) {
			log_err("rbd_aio_read failed.\n");
			goto failed_comp;
		}
	} else if (io_u->ddir == DDIR_TRIM) {
		r = rbd_aio_discard(rbd->image, io_u->offset,
					io_u->xfer_buflen, fri->completion);
		/* [한국어] RBD discard — 해당 영역을 sparse/thin-provisioned 상태로. */
		if (r < 0) {
			log_err("rbd_aio_discard failed.\n");
			goto failed_comp;
		}
	} else if (io_u->ddir == DDIR_SYNC) {
		r = rbd_aio_flush(rbd->image, fri->completion);
		/* [한국어] 캐시된 쓰기를 모두 내리는 flush. */
		if (r < 0) {
			log_err("rbd_flush failed.\n");
			goto failed_comp;
		}
	} else {
		dprint(FD_IO, "%s: Warning: unhandled ddir: %d\n", __func__,
		       io_u->ddir);
		r = -EINVAL;
		goto failed_comp;
	}

	return FIO_Q_QUEUED;
failed_comp:
	rbd_aio_release(fri->completion);	/* [한국어] 생성했던 completion 즉시 해제. */
failed:
	io_u->error = -r;			/* [한국어] -errno를 양수로 변환해 저장. */
	td_verror(td, io_u->error, "xfer");
	return FIO_Q_COMPLETED;
}

/*
 * [한국어]
 * fio_rbd_init - 잡 스레드 내에서 (setup에서 연결이 안 된 경우에 한해) RADOS/RBD에 연결.
 * @return: 0=성공 or 이미 연결됨, 1=실패.
 *
 * 호출 체인: td_io_init → [이 함수] → _fio_rbd_connect
 */
static int fio_rbd_init(struct thread_data *td)
{
	int r;
	struct rbd_data *rbd = td->io_ops_data;

	if (rbd->connected)
		return 0;

	r = _fio_rbd_connect(td);
	if (r) {
		log_err("fio_rbd_connect failed, return code: %d .\n", r);
		goto failed;
	}

	return 0;

failed:
	return 1;
}

/*
 * [한국어]
 * fio_rbd_cleanup - 잡 종료 시 disconnect 및 메모리 반환.
 * 호출 체인: td_io_cleanup → [이 함수] → _fio_rbd_disconnect + free
 */
static void fio_rbd_cleanup(struct thread_data *td)
{
	struct rbd_data *rbd = td->io_ops_data;

	if (rbd) {
		_fio_rbd_disconnect(rbd);
		free(rbd->aio_events);
		free(rbd->sort_events);
		free(rbd);
	}
}

/*
 * [한국어]
 * fio_rbd_setup - 이미지 크기 탐지와 가상 파일 등록을 위해 메인 컨텍스트에서 1회 호출.
 *
 * @td: 잡. 이 함수의 부수효과로 td->o.use_thread=1 강제, td->files[0] 등록.
 * @return: 0=성공, 오류 코드=실패(cleanup 경로 수행 후 반환).
 *
 * librbd는 fork 안전하지 않으므로 use_thread=1로 강제한다. 이후 연결하여
 * rbd_stat으로 이미지 크기를 얻고, 가상 파일의 real_file_size에 반영하여
 * fio가 io_u 오프셋을 올바르게 분배하게 한다.
 *
 * 호출 체인: fio 메인 초기화 → ioengine_ops.setup (= 이 함수)
 *   → _fio_setup_rbd_data → _fio_rbd_connect → rbd_stat → add_file
 */
static int fio_rbd_setup(struct thread_data *td)
{
	rbd_image_info_t info;		/* [한국어] 이미지 크기/order 등 메타데이터. */
	struct fio_file *f;		/* [한국어] 등록할 fio 가상 파일 포인터. */
	struct rbd_data *rbd = NULL;
	int r;

	/* allocate engine specific structure to deal with librbd. */
	r = _fio_setup_rbd_data(td, &rbd);
	if (r) {
		log_err("fio_setup_rbd_data failed.\n");
		goto cleanup;
	}
	td->io_ops_data = rbd;

	/* librbd does not allow us to run first in the main thread and later
	 * in a fork child. It needs to be the same process context all the
	 * time.
	 */
	td->o.use_thread = 1;
	/* [한국어] fork() 기반 잡 모델 금지 — pthread 모델로 강제. */

	/* connect in the main thread to determine to determine
	 * the size of the given RADOS block device. And disconnect
	 * later on.
	 */
	r = _fio_rbd_connect(td);
	if (r) {
		log_err("fio_rbd_connect failed.\n");
		goto cleanup;
	}
	rbd->connected = true;		/* [한국어] init에서 재연결하지 않도록 마크. */

	/* get size of the RADOS block device */
	r = rbd_stat(rbd->image, &info, sizeof(info));
	if (r < 0) {
		log_err("rbd_status failed.\n");
		goto cleanup;
	} else if (info.size == 0) {
		log_err("image size should be larger than zero.\n");
		r = -EINVAL;
		goto cleanup;
	}

	dprint(FD_IO, "rbd-engine: image size: %" PRIu64 "\n", info.size);

	/* taken from "net" engine. Pretend we deal with files,
	 * even if we do not have any ideas about files.
	 * The size of the RBD is set instead of an artificial file.
	 */
	if (!td->files_index) {
		add_file(td, td->o.filename ? : "rbd", 0, 0);
		/* [한국어] "rbd"라는 가짜 파일명으로 파일 인덱스 생성 — 실제 FS 접근은 없음. */
		td->o.nr_files = td->o.nr_files ? : 1;
		td->o.open_files++;
	}
	f = td->files[0];
	f->real_file_size = info.size;		/* [한국어] 이미지 실제 크기를 파일 크기로 주입. */

	return 0;

cleanup:
	fio_rbd_cleanup(td);
	return r;
}

/*
 * [한국어]
 * fio_rbd_open - 파일 단위 open 콜백(no-op).
 * RBD 이미지는 _fio_rbd_connect에서 이미 열려 있으므로 여기선 0만 반환.
 */
static int fio_rbd_open(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

/*
 * [한국어]
 * fio_rbd_invalidate - 호스트측 캐시 무효화(선택 빌드).
 * CONFIG_RBD_INVAL이 정의된 경우에만 rbd_invalidate_cache 호출.
 */
static int fio_rbd_invalidate(struct thread_data *td, struct fio_file *f)
{
#if defined(CONFIG_RBD_INVAL)
	struct rbd_data *rbd = td->io_ops_data;

	return rbd_invalidate_cache(rbd->image);
#else
	return 0;
#endif
}

/*
 * [한국어]
 * fio_rbd_io_u_free - io_u 파괴 시 engine_data(fri) 해제.
 * 호출 체인: io_u_free → ioengine_ops.io_u_free
 */
static void fio_rbd_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct fio_rbd_iou *fri = io_u->engine_data;

	if (fri) {
		io_u->engine_data = NULL;
		free(fri);
	}
}

/*
 * [한국어]
 * fio_rbd_io_u_init - io_u 생성 시 fri 구조체 할당 및 역참조 링크 설정.
 * 호출 체인: io_u_init → ioengine_ops.io_u_init
 */
static int fio_rbd_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct fio_rbd_iou *fri;

	fri = calloc(1, sizeof(*fri));		/* [한국어] zero-init로 seen/complete=0 자동. */
	fri->io_u = io_u;			/* [한국어] 콜백에서 io_u를 복원할 수 있도록 역링크. */
	io_u->engine_data = fri;
	return 0;
}

/* [한국어] 엔진 vtable — fio 코어(ioengines.c)가 각 콜백을 이 테이블에서 찾아 호출한다.
 * FIO_STATIC은 공유 라이브러리 빌드(.so로 빌드될 때)에서 심볼을 static으로, 내부
 * 빌드에서는 extern 가시성으로 전환하는 매크로로, 외부 로딩과 내장 로딩을 하나의
 * 소스로 지원하기 위한 장치이다.
 *
 * 이 vtable의 각 필드는 아래 단위 §4 주석에서 계약(언제 호출되는지, 반환값의
 * 의미, 스레드 컨텍스트)을 설명한다. FIO_Q_COMPLETED/FIO_Q_QUEUED/FIO_Q_BUSY의
 * 세 반환 코드 중 이 엔진은 QUEUED/COMPLETED만 사용한다(BUSY 없음). */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "rbd",
	/* [한국어] 엔진 이름 — fio CLI에서 `--ioengine=rbd` 로 선택되는 키.
	 * 설정자: 이 정적 초기화. 읽는 자: register_ioengine/load_ioengine가 dlsym
	 *         또는 engine_list 탐색 키로 사용.
	 * 값 범위: "rbd" 상수 — 다른 엔진과 충돌해선 안 됨.
	 * 동기화: read-only 전역. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine_ops ABI 버전. fio 코어와 버전 불일치 시 load 단계에서 거부.
	 * 설정자: fio.h의 현재 버전 매크로. 읽는 자: register_ioengine의 check_engine_ops.
	 * 동기화: 컴파일 타임 상수. */

	.setup			= fio_rbd_setup,
	/* [한국어] 잡 초기화 전(메인 프로세스 컨텍스트) 1회 호출되는 훅.
	 * 설정자: 이 초기화. 읽는 자: td_io_setup(backend.c 초기 단계).
	 * 역할: use_thread=1 강제, RBD 이미지 크기 조회 후 fio_file에 주입.
	 * 동기화: 메인 스레드 단독 실행 시점이므로 잡 스레드와 경쟁 없음. */

	.init			= fio_rbd_init,
	/* [한국어] 잡 스레드가 시작된 후 I/O 시작 전에 호출(스레드 컨텍스트 최초 훅).
	 * 역할: setup이 이미 connected=true이면 스킵, 아니면 _fio_rbd_connect 재실행.
	 * librbd는 fork 금지이므로 setup에서의 연결은 메인에만, 잡 스레드에서는
	 * use_thread=1 pthread이므로 동일 연결을 재활용할 수 있다. */

	.queue			= fio_rbd_queue,
	/* [한국어] io_u 한 개를 비동기 제출. 반환: FIO_Q_QUEUED(대부분) / FIO_Q_COMPLETED(에러).
	 * 호출자: td_io_queue → ioengines.c. 호출 시점: get_io_u 후 do_io 루프. */

	.getevents		= fio_rbd_getevents,
	/* [한국어] 제출된 I/O의 완료 이벤트를 수집(min..max). rbd_iter_events를 루프 호출.
	 * 반환: 실제 수집된 이벤트 수(>=min 보장 for wait 경로). */

	.event			= fio_rbd_event,
	/* [한국어] getevents가 N을 반환한 뒤, 인덱스 0..N-1로 호출되어 io_u*를 반환.
	 * aio_events[event]를 그대로 반환하는 경량 조회 함수. */

	.cleanup		= fio_rbd_cleanup,
	/* [한국어] 잡 종료 시 disconnect + free. getevents 완료 후에만 호출됨을 코어가 보장. */

	.open_file		= fio_rbd_open,
	/* [한국어] 파일 단위 open(기본 구현은 no-op). RBD 이미지는 _fio_rbd_connect에서 이미 열려 있음.
	 * fio 코어가 "파일 객체"를 관리하기 위해 형식적으로 요구하는 콜백. */

	.invalidate		= fio_rbd_invalidate,
	/* [한국어] 호스트 캐시 무효화(선택). CONFIG_RBD_INVAL 빌드에서만 rbd_invalidate_cache 호출. */

	.options		= options,
	/* [한국어] 엔진 전용 CLI 옵션 테이블 포인터. NULL 센티널 종료.
	 * 파서가 이 배열을 순회하며 FIO_OPT_C_ENGINE/FIO_OPT_G_RBD 그룹으로 등록. */

	.io_u_init		= fio_rbd_io_u_init,
	/* [한국어] io_u가 처음 생성될 때(io_u 배열 초기화 시) 호출 — per-io_u fri 할당.
	 * 동기화: 각 io_u는 단일 잡 스레드 소유. */

	.io_u_free		= fio_rbd_io_u_free,
	/* [한국어] io_u 파괴 시 fri 해제. cleanup 전에 코어가 전부 호출함을 보장. */

	.option_struct_size	= sizeof(struct rbd_options),
	/* [한국어] td->eo를 할당할 크기 힌트. 옵션 구조체를 잡별로 복제할 때 사용됨.
	 * 설정자: 컴파일 시 sizeof. 읽는 자: init 단계의 옵션 복제 루틴. */
};

/*
 * [한국어]
 * fio_rbd_register - fio_init 생성자 — 프로그램 시작 시 엔진을 등록한다.
 *
 * @return: 없음.
 *
 * 이 함수는 main() 진입 이전(libc 동적 로더가 .init_array를 실행하는 시점)에
 * 자동 호출되어 "rbd" 이름의 엔진을 전역 engine_list에 등록한다. fio_init 속성은
 * compiler/compiler.h에서 __attribute__((constructor))로 확장되며, 같은 기법을
 * 쓰는 모든 엔진은 main 시작 전에 register_ioengine을 완료하므로 load_ioengine이
 * "rbd" 키워드로 곧바로 이 vtable을 찾을 수 있다.
 *
 * 실행 컨텍스트: 프로세스의 _start → __libc_start_main의 초기화 단계. 아직 잡
 * 스레드는 생성되지 않았으며, ld.so가 단일 스레드로 생성자를 순차 호출한다.
 *
 * 호출 체인: ld.so(.init_array) → [이 함수] → register_ioengine() → flist_add_tail
 */
static void fio_init fio_rbd_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] fio 코어의 engine_list(링크드 리스트)에 이 ioengine vtable을 등록.
	 * load_ioengine("rbd")가 이 리스트를 순회해 name == "rbd"를 찾는다. */
}

/*
 * [한국어]
 * fio_rbd_unregister - fio_exit 소멸자 — 프로세스 종료 시 엔진 등록 해제.
 *
 * @return: 없음.
 *
 * fio_exit 속성은 __attribute__((destructor))로 확장되어 libc의 atexit 체인을
 * 통해 main 종료 후 자동 호출된다. engine_list에서 이 vtable을 안전하게 분리해
 * 이후 접근 시 dangling을 방지한다.
 *
 * 실행 컨텍스트: 모든 잡 스레드가 join된 후의 단일 스레드. exit()의 atexit 호출.
 *
 * 호출 체인: libc(atexit/.fini_array) → [이 함수] → unregister_ioengine() → flist_del
 */
static void fio_exit fio_rbd_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 engine_list에서 이 엔트리를 제거 — 이후 load_ioengine("rbd")는 실패. */
}
