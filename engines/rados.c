/*
 * [한국어 설명] Ceph RADOS 오브젝트 스토어 I/O 엔진 구현 (rados.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio가 Ceph 분산 스토리지의 최하위 계층인 RADOS(Reliable Autonomic
 * Distributed Object Store)에 직접 오브젝트 단위로 I/O를 발생시키게 하는 I/O 엔진이다.
 * RBD 엔진(rbd.c)이 "librados + librbd" 2계층으로 블록 디바이스 추상화(스냅샷/클론/
 * 스트라이프 단위 4MiB 오브젝트 자동 분해/캐시/이미지 락)를 테스트한다면, 이 엔진은
 * 그 아래 계층인 OSD(Object Storage Daemon)의 "한 오브젝트 단위 read/write/append/
 * zero(=TRIM)/remove" 경로만 고립해서 측정하기 위해 존재한다. 모든 I/O는 librados의
 * 비동기(aio) API(rados_aio_create_completion → rados_aio_write/read/write_op_operate
 * → 완료 콜백)로 제출되며, RADOS 라이브러리가 내부 메신저(Messenger) 스레드에서
 * 완료 콜백(complete_callback)을 호출해 fio로 완료를 통지한다. 엔진은 FIO_DISKLESSIO
 * 플래그로 fio 코어가 실제 파일을 생성하지 않게 하고, 파일 이름 문자열을 오브젝트
 * 이름으로 재사용한다(fio_file 하나 ↔ RADOS 오브젝트 하나). 또한 FIO_NODISKUTIL
 * 은 설정하지 않지만, diskless 경로이므로 블록 디바이스 utilization 통계는 비게 된다.
 *
 * RADOS vs RBD 핵심 차이:
 *   - RBD: 블록 디바이스 추상 — 하나의 "image" 가 4MiB 오브젝트 여러 개로 투명
 *          스트라이핑됨. snap/clone/resize/journal/exclusive-lock 등 블록 의미론.
 *          librbd 콜백이 librados 콜백 위에 한 겹 더 있음(2계층 비동기).
 *   - RADOS(본 엔진): "한 오브젝트 하나" 가 전체 단위. 오브젝트 크기 상한(기본
 *          osd_max_object_size ~ 128MiB, 실제로 4MiB 경계로 쓰는 것이 전형)을
 *          클라이언트가 직접 관리해야 함. TRIM 은 rados_write_op_zero(논리적 zero-fill)
 *          로 에뮬레이션. 스냅샷/클론 없음. 콜백 1계층.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 실행 흐름 main() → fio_backend() → load_ioengine("rados") 과정에서
 * fio_rados_register()(파일 끝, __attribute__((constructor))) 가 _start →
 * __libc_start_main 의 .init_array 단계에서 이 엔진 vtable 을 engine_list 에 등록한다.
 * 잡 스레드는 td_io_init → fio_rados_setup → _fio_setup_rados_data + _fio_rados_connect
 * 로 상태를 할당하고 Ceph 모니터(MON) 에 TCP 연결 + CephX 인증 + cluster map 수신 후
 * 지정 풀에 대한 ioctx 를 만든 뒤, 잡 루프에서 get_io_u → td_io_queue(=fio_rados_queue)
 * 로 오브젝트 I/O 를 librados 에 비동기 제출하고 td_io_getevents(=fio_rados_getevents)
 * 로 완료를 수확한다. 완료는 librados 내부 메신저 스레드가 complete_callback 을 호출해
 * 알려주므로, 실행 컨텍스트가 "잡 스레드(생산자 of submit, 소비자 of completion)"
 * 와 "librados 메신저 콜백 스레드(생산자 of completion)" 두 곳으로 나뉘며
 * completed_lock/completed_more_io 로 경합을 직렬화한다.
 *
 * CephX 인증/ceph.conf/keyring 경로:
 *   - rados_create(&cluster, id)          : 기본 클러스터명 "ceph", client id 지정.
 *   - rados_create2(&cluster, name, full, 0): 클러스터명(Zone)과 "type.id"(예: "client.admin")
 *     를 모두 지정. client_name 옵션이 '.' 없는 순수 ID 면 "client." 접두어 자동 부여.
 *   - rados_conf_read_file(cluster, path) : ceph.conf 에서 MON 주소/keyring 경로/auth 방식
 *     적용. 기본 "/etc/ceph/ceph.conf". keyring 내 CephX secret 으로 MON 과 rendezvous.
 *   - rados_connect(cluster)              : MON 포트(기본 3300 msgr2 / 6789 legacy)에
 *     TCP 접속, OSDMap/MonMap 수신. 이후 OSD 와 직접 통신.
 *   - rados_ioctx_create(cluster, pool, &ioctx): 특정 풀에 바인딩된 I/O 컨텍스트 생성.
 *
 * === 타 모듈과의 연결 ===
 * 상위: ioengines.c(td_io_* 래퍼) → 이 파일의 ioengine_ops 콜백(setup/queue/getevents/
 *        event/cleanup/open_file/invalidate/io_u_init/io_u_free).
 * 하위: librados (rados_create/create2/conf_read_file/connect/ioctx_create/aio_*
 *        /write/read/write_op_operate/create_write_op/write_op_zero/release_write_op/
 *        aio_release/aio_is_complete/remove/shutdown/ioctx_destroy).
 * 병렬: pthread(완료 리스트용 뮤텍스/조건변수).
 * 공유 상태: rados_data(클러스터·ioctx 핸들, 완료 리스트, 뮤텍스/조건변수, scheduled/
 *            completed 카운터) 를 td->io_ops_data 로 fio 코어와 공유하고, io_u 당
 *            fio_rados_iou 를 io_u->engine_data 로 매달아 queue→callback→getevents
 *            사이에서 완료 핸들/write_op 를 추적한다.
 * 데이터 흐름: fio 가 버퍼를 채워 io_u 로 넘겨주면 → queue 가 librados AIO 호출로
 *              primary OSD 에 오브젝트 I/O 를 보내고(OSD 는 replication/erasure coding
 *              으로 복제본에 전파 후 ACK) → 메신저 콜백 스레드가 완료 리스트에 push
 *              → getevents 가 pop 해서 aio_events[] 로 fio 에 반환 → fio_rados_event()
 *              가 인덱스로 io_u 를 코어에 돌려준다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_rados_setup: 잡 스레드 1회. 상태 구조체 할당 후 클러스터에 연결 + ioctx 생성
 *   + (touch_objects=1 이면) 대상 오브젝트를 0바이트 write 로 생성. use_thread=1 강제
 *   (librados 가 fork 후 안전하지 않아 pthread 모드를 요구).
 * - _fio_rados_connect: cluster_name/client_name 규칙("client.<id>")에 맞춰
 *   rados_create2/rados_create 선택, conf 읽고 풀 ioctx 생성, touch_objects 옵션일
 *   때 오브젝트 0바이트 write 로 "존재" 를 만든다(READ 시 ENOENT 방지).
 * - fio_rados_queue: ddir 별로 rados_aio_write / rados_aio_read / write_op+zero
 *   (TRIM) 분기. 각 호출마다 rados_completion_t 를 새로 만들고 콜백을 등록한다.
 *   FIO_Q_QUEUED(비동기 수락) 또는 FIO_Q_COMPLETED(즉시 에러) 반환.
 * - complete_callback: librados 메신저 콜백 스레드에서 실행. 완료 리스트에 링크하고
 *   ops_completed 증가 후 조건변수로 잡 스레드를 깨움. 자원 해제는 getevents 가 수행.
 * - fio_rados_getevents: min/max 를 만족할 때까지 대기하며 완료 리스트를 드레인.
 *   각 엔트리에서 rados_aio_release + rados_release_write_op(TRIM 한정) 로 자원 회수.
 * - fio_rados_cleanup: scheduled==completed 까지 대기 후 오브젝트 제거 + disconnect.
 * - struct rados_data: 엔진 전역 상태(잡 1개당 1개).
 * - struct fio_rados_iou: io_u 당 완료 핸들/write_op 보관.
 * - struct rados_options: CLI/잡파일 옵션(clustername/pool/clientname/conf/busy_poll/
 *   touch_objects).
 */

/*
 *  Ceph Rados engine
 *
 * IO engine using Ceph's RADOS interface to test low-level performance of
 * Ceph OSDs.
 *
 */

#include <rados/librados.h>     /* [한국어] librados C API (rados_t, rados_ioctx_t, rados_aio_*). Ceph 클러스터에 오브젝트 I/O 를 보내는 유일한 외부 의존성. */
#include <pthread.h>            /* [한국어] 완료 리스트를 보호하는 뮤텍스와 잡 스레드를 깨우는 조건변수를 사용하기 위해 필요. librados 콜백 스레드와의 공유가 있으므로 필수. */
#include "fio.h"                /* [한국어] fio 코어 자료구조(thread_data, io_u, fio_file, ioengine_ops 등) 선언. 모든 엔진의 공통 의존성. */
#include "../optgroup.h"        /* [한국어] FIO_OPT_C_ENGINE / FIO_OPT_G_RBD 옵션 그룹 상수 정의. options[] 의 category/group 에 필요. */

/*
 * [한국어] struct rados_data — RADOS 엔진이 잡(td) 당 보유하는 내부 상태.
 * td->io_ops_data 로 연결되며, _fio_setup_rados_data 에서 calloc 되고
 * fio_rados_cleanup 에서 해제된다. 완료 리스트는 잡 스레드(소비자)와
 * librados 콜백 스레드(생산자) 가 공유하므로 뮤텍스·조건변수로 보호한다.
 */
struct rados_data {
	rados_t cluster;
	/* [한국어] RADOS 클러스터 핸들(불투명 포인터).
	 * 설정자: _fio_rados_connect 의 rados_create/rados_create2.
	 * 읽는 자: rados_connect, rados_ioctx_create, rados_shutdown.
	 * 값 범위: 유효한 클러스터 핸들 또는 NULL(연결 전/해제 후).
	 * 동기화: 잡 스레드 안에서만 조작되므로 별도 락 불필요. */

	rados_ioctx_t io_ctx;
	/* [한국어] 특정 풀(pool)에 대한 I/O 컨텍스트. 오브젝트 수준 R/W/TRIM 에 사용.
	 * 설정자: rados_ioctx_create 성공 후 저장.
	 * 읽는 자: queue() 내 rados_aio_write/read/write_op_operate, cleanup 의 remove.
	 * 값 범위: 유효한 ioctx 또는 NULL.
	 * 동기화: librados 내부적으로 thread-safe. 우리는 덮어쓰지 않으므로 락 불필요. */

	struct io_u **aio_events;
	/* [한국어] getevents 가 채워 fio 에 돌려주는 완료된 io_u 포인터 배열.
	 * 설정자: fio_rados_getevents 가 인덱스 순으로 저장.
	 * 읽는 자: fio_rados_event(td, i) 가 i 번째 완료 io_u 를 반환할 때.
	 * 크기: td->o.iodepth. 동기화: getevents 와 event 는 같은 잡 스레드에서 순차 실행되므로 락 불필요. */

	bool connected;
	/* [한국어] 클러스터에 성공적으로 connect 했는지 여부. setup 에서 true 로 바뀐다.
	 * cleanup 경로가 반쯤 초기화된 상태를 만날 때 분기 지표로 사용될 수 있다. */

	pthread_mutex_t completed_lock;
	/* [한국어] completed_operations 리스트와 ops_completed 카운터를 보호.
	 * 생산자: librados 콜백 스레드(complete_callback).
	 * 소비자: 잡 스레드(fio_rados_getevents, fio_rados_cleanup). */

	pthread_cond_t completed_more_io;
	/* [한국어] 완료가 발생했음을 잡 스레드에 통지하는 조건변수.
	 * signal: complete_callback. wait: getevents(min 만족 전) 와 cleanup(drain). */

	struct flist_head completed_operations;
	/* [한국어] 완료된 fio_rados_iou 들의 연결 리스트(FIFO).
	 * flist_add_tail 로 push, flist_first_entry/flist_del 로 pop.
	 * completed_lock 필수. */

	uint64_t ops_scheduled;
	/* [한국어] queue() 가 librados 로 AIO 를 제출한 누적 수. cleanup 에서 drain 조건에 사용. */

	uint64_t ops_completed;
	/* [한국어] complete_callback 이 누적한 완료 수. scheduled==completed 이면 in-flight 0. */
};

/*
 * [한국어] struct fio_rados_iou — io_u 하나에 묶이는 엔진 고유 컨텍스트.
 * io_u_init 에서 할당되어 io_u->engine_data 에 매달리며 io_u_free 에서 해제된다.
 * queue→callback→getevents 사이에서 완료 핸들과 write_op 를 전달하는 매개체이다.
 */
struct fio_rados_iou {
	struct flist_head list;
	/* [한국어] rados_data->completed_operations 에 매다는 링크 필드.
	 * 설정자: complete_callback. 읽는 자: getevents. 동기화: completed_lock. */

	struct thread_data *td;
	/* [한국어] 이 io_u 가 소속된 잡의 thread_data 역참조.
	 * 콜백이 io_ops_data(=rados_data) 에 접근할 수 있게 하는 경로이다.
	 * io_u_init 에서 설정, io_u_free 에서 NULL 로 클리어. */

	struct io_u *io_u;
	/* [한국어] 이 엔진 컨텍스트에 대응하는 fio io_u.
	 * 설정자: io_u_init. 읽는 자: getevents 가 aio_events[] 에 저장할 때.
	 * 값 범위: 유효한 io_u 포인터(NULL 불가). */

	rados_completion_t completion;
	/* [한국어] librados 비동기 완료 핸들. rados_aio_create_completion 으로 queue 시 생성,
	 * complete_callback 등록. getevents 에서 rados_aio_release 로 해제 후 NULL.
	 * 동기화: librados 내부 상태는 thread-safe. */

	rados_write_op_t write_op;
	/* [한국어] TRIM(DDIR_TRIM) 처리용 write_op. rados_write_op_zero 로 영역 무효화.
	 * READ/WRITE 경로에서는 NULL 로 남는다.
	 * 해제: getevents 또는 io_u_free 에서 rados_release_write_op. */
};

/*
 * [한국어] struct rados_options — 잡 파일/CLI 에서 파싱된 엔진 전용 옵션.
 * pad 선행 필드는 fio option 파서가 offsetof 계산 시 기준점으로 사용하는 관례.
 */
/* fio configuration options read from the job file */
struct rados_options {
	void *pad;
	/* [한국어] fio 옵션 파싱 시 option_struct_size 와 함께 기준 주소로 사용되는 패딩.
	 * 코드상 의미 없는 값이지만 규약상 첫 필드로 둔다. */

	char *cluster_name;
	/* [한국어] Ceph 클러스터 이름. NULL 이면 rados_create(기본 "ceph") 경로로 fallback.
	 * 설정자: --clustername. 읽는 자: _fio_rados_connect. */

	char *pool_name;
	/* [한국어] 대상 풀 이름. 필수. NULL 이면 connect 에서 에러.
	 * 설정자: --pool. 읽는 자: rados_ioctx_create. */

	char *client_name;
	/* [한국어] RADOS 클라이언트 ID. cluster_name 지정 시 "client." 접두어를 자동 부여.
	 * 설정자: --clientname. */

	char *conf;
	/* [한국어] ceph.conf 파일 경로. 기본 /etc/ceph/ceph.conf. rados_conf_read_file 에서 사용. */

	int busy_poll;
	/* [한국어] 완료 수확 시 busy-poll 여부. 현 코드 상 조건변수 대기를 사용하므로
	 * 옵션은 파싱되지만 실제 분기 영향은 제한적(호환용 플래그). */

	int touch_objects;
	/* [한국어] setup 시 0바이트 write 로 오브젝트를 "생성"할지 여부. 기본 1.
	 * 미존재 오브젝트에 대한 read 가 ENOENT 가 되는 것을 방지. */
};

/*
 * [한국어] options[] — fio 옵션 파서에 등록할 엔진 옵션 테이블.
 *
 * 공통 규약:
 *   - .name: CLI 단축/잡 파일 키. `--clustername=ceph` 같은 형태로 노출.
 *   - .lname: --help/--ioengine=rados --enghelp 시 사람이 읽는 라벨.
 *   - .type: FIO_OPT_STR_STORE(strdup 후 포인터 저장) / FIO_OPT_BOOL(0|1 정수) 사용.
 *   - .off1: 파서가 결과를 기록할 rados_options 내 오프셋. FIO_OPT_C_ENGINE 가 있는
 *     엔진 옵션은 td->eo(엔진별 옵션 복제본, option_struct_size 바이트) 기준.
 *   - .def: 값 미지정 시 기본(문자열). 내부적으로 타입에 맞게 재파싱됨.
 *   - .help: --cmdhelp/--enghelp 출력용 한 줄 설명.
 *   - .category/.group: --cmdhelp 분류용. RADOS 는 RBD 와 한 그룹(FIO_OPT_G_RBD)을
 *     공유해 Ceph 관련 옵션이 --cmdhelp 에서 같이 묶여 표시되도록 한다.
 *   - 테이블 끝은 .name = NULL 센티널로 마감해 파서 순회 루프가 종료 조건으로 인식.
 */
static struct fio_option options[] = {
	{
		.name     = "clustername",
		/* [한국어] CLI/잡파일 키 — `clustername=<name>` 형태. rados_create2 의 첫 인자로
		 * 전달될 Ceph 클러스터 이름(일반적으로 "ceph"). 미지정이면 rados_create 로
		 * fallback 하여 기본 클러스터명을 librados 가 가정한다. */
		.lname    = "ceph cluster name",
		/* [한국어] --help/--enghelp 표시용 긴 이름. */
		.type     = FIO_OPT_STR_STORE,
		/* [한국어] 문자열을 strdup 해서 off1 위치에 포인터로 저장. cleanup 시 libc free
		 * 대상은 fio 공통 옵션 해제 경로가 처리. */
		.help     = "Cluster name for ceph",
		/* [한국어] --cmdhelp clustername 에 출력되는 설명. */
		.off1     = offsetof(struct rados_options, cluster_name),
		/* [한국어] rados_options.cluster_name 에 저장 — _fio_rados_connect 가 읽음. */
		.category = FIO_OPT_C_ENGINE,
		/* [한국어] 엔진 전용 카테고리(코어/로그/verify 와 구분). */
		.group    = FIO_OPT_G_RBD,
		/* [한국어] RBD/RADOS Ceph 공용 그룹 — 같은 --cmdhelp 섹션에 묶임. */
	},
	{
		.name     = "pool",
		/* [한국어] 필수 옵션. Ceph 풀 이름 — rados_ioctx_create 의 대상.
		 * 미지정 시 _fio_rados_connect 가 에러 로그 후 실패 반환. */
		.lname    = "pool name to use",
		/* [한국어] 도움말 표시용 긴 이름. */
		.type     = FIO_OPT_STR_STORE,
		/* [한국어] 풀 이름 문자열 저장. */
		.help     = "Ceph pool name to benchmark against",
		/* [한국어] --cmdhelp pool 출력. */
		.off1     = offsetof(struct rados_options, pool_name),
		/* [한국어] rados_options.pool_name 에 저장 — _fio_rados_connect 가 읽음. */
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_RBD,
	},
	{
		.name     = "clientname",
		/* [한국어] RADOS 클라이언트 ID. 값에 '.' 이 없으면 _fio_rados_connect 가
		 * "client." 접두어를 붙여 "client.<ID>" 형태로 rados_create2 에 전달(CephX
		 * principal 이름). 이미 "type.id" 형태면 그대로 사용. 기본 "client.admin". */
		.lname    = "rados engine clientname",
		.type     = FIO_OPT_STR_STORE,
		.help     = "Name of the ceph client to access RADOS engine",
		.off1     = offsetof(struct rados_options, client_name),
		/* [한국어] rados_options.client_name 에 저장. */
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_RBD,
	},
	{
		.name     = "conf",
		/* [한국어] ceph.conf 경로 — rados_conf_read_file 이 읽어 MON 주소, keyring
		 * 경로, auth 방식, 네트워크 옵션 등을 적용한다. */
		.lname    = "ceph configuration file path",
		.type     = FIO_OPT_STR_STORE,
		.help     = "Path of the ceph configuration file",
		.off1     = offsetof(struct rados_options, conf),
		/* [한국어] rados_options.conf 에 저장. */
		.def      = "/etc/ceph/ceph.conf",
		/* [한국어] 미지정 시 표준 Ceph 배포의 관례 경로를 기본값으로 사용. */
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_RBD,
	},
	{
		.name     = "busy_poll",
		/* [한국어] 완료 수확 정책 힌트 — 코드에 변수만 존재하고 실제 분기는 없음
		 * (조건변수 wait 경로 고정). 호환용/향후 튜닝용 예약 플래그로 보이며,
		 * RBD 엔진과 동일 옵션명을 맞춰 두어 사용자 학습 일관성을 제공한다. */
		.lname    = "busy poll mode",
		.type     = FIO_OPT_BOOL,
		/* [한국어] 0/1 부울 값. */
		.help     = "Busy poll for completions instead of sleeping",
		.off1     = offsetof(struct rados_options, busy_poll),
		.def	  = "0",
		/* [한국어] 기본 OFF — 조건변수 대기 모드로 CPU 를 쉬게 한다. */
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_RBD,
	},
	{
		.name     = "touch_objects",
		/* [한국어] setup 단계에서 대상 오브젝트를 0바이트 rados_write 로 "존재"시킬지.
		 * 켜두면 read-only 잡이나 읽기 우선 잡에서 ENOENT(-2) 가 나지 않는다. */
		.lname    = "touch objects on start",
		.type     = FIO_OPT_BOOL,
		.help     = "Touch (create) objects on start",
		.off1     = offsetof(struct rados_options, touch_objects),
		.def	  = "1",
		/* [한국어] 기본 ON — 안전한 기본값. 이미 존재하는 오브젝트 측정 시에도
		 * 0바이트 write 는 tail size 에 영향을 주지 않아 부작용이 작다. */
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_RBD,
	},
	{
		.name     = NULL,
		/* [한국어] 테이블 종단 센티널 — 파서가 .name==NULL 을 보면 루프 종료. */
	},
};

/*
 * [한국어]
 * _fio_setup_rados_data - 엔진 상태 구조체(rados_data)를 할당·초기화한다.
 *
 * @td: 이 잡의 thread_data. 이미 io_ops_data 가 있으면 재사용.
 * @rados_data_ptr: 출력 파라미터. 성공 시 할당된 rados_data 포인터가 기록된다.
 * @return: 0 성공, 1 실패(할당 실패 등). 호출자는 실패 시 cleanup 경로로 간다.
 *
 * 왜 필요한가: 클러스터 핸들, 완료 동기화 객체, 완료 리스트, aio_events 배열을
 *              한 덩어리로 묶어 관리하기 위해서이다.
 * 실행 컨텍스트: fio_rados_setup 에서 잡 스레드가 1회 호출한다.
 * 호출 체인: fio_rados_setup → _fio_setup_rados_data → calloc/pthread_*_init.
 */
static int _fio_setup_rados_data(struct thread_data *td,
				struct rados_data **rados_data_ptr)
{
	struct rados_data *rados;                                               /* [한국어] 할당할 엔진 상태 임시 포인터. */

	if (td->io_ops_data)                                                    /* [한국어] 이미 세팅되어 있으면 중복 할당 방지. */
		return 0;

	rados = calloc(1, sizeof(struct rados_data));                           /* [한국어] 0으로 초기화된 상태 구조체 할당(플래그·포인터 안전). */
	if (!rados)                                                             /* [한국어] OOM 시 failed 로. */
		goto failed;

	rados->connected = false;                                               /* [한국어] connect 성공 시점에만 true 로 바꾼다. */

	rados->aio_events = calloc(td->o.iodepth, sizeof(struct io_u *));       /* [한국어] 완료 반환 배열. 크기는 잡의 iodepth. */
	if (!rados->aio_events)                                                 /* [한국어] 배열 할당 실패 시 failed 로. */
		goto failed;
	pthread_mutex_init(&rados->completed_lock, NULL);                       /* [한국어] 완료 리스트용 뮤텍스 초기화. */
	pthread_cond_init(&rados->completed_more_io, NULL);                     /* [한국어] "완료 이벤트 왔음" 신호용 조건변수 초기화. */
	INIT_FLIST_HEAD(&rados->completed_operations);                          /* [한국어] flist 헤드 자기 참조 초기화. */
	rados->ops_scheduled = 0;                                               /* [한국어] 제출 카운터 0. */
	rados->ops_completed = 0;                                               /* [한국어] 완료 카운터 0. */
	*rados_data_ptr = rados;                                                /* [한국어] 호출자에게 상태 포인터 반환. */
	return 0;                                                               /* [한국어] 성공. */

failed:
	if (rados) {                                                            /* [한국어] 부분 할당된 상태를 풀어준다(누수 방지). */
		if (rados->aio_events)
			free(rados->aio_events);
		free(rados);
	}
	return 1;                                                               /* [한국어] 실패 코드. */
}

/*
 * [한국어]
 * _fio_rados_rm_objects - 잡이 사용한 오브젝트들을 풀에서 제거한다.
 *
 * @td: 잡의 thread_data. td->files[] 가 오브젝트 이름(file_name) 소스.
 * @rados: 유효한 ioctx 를 가진 엔진 상태.
 *
 * 왜 필요한가: 테스트 종료 시 남은 오브젝트를 청소해 반복 실행 간 상태를 깨끗이 유지.
 * 실행 컨텍스트: cleanup 또는 setup 실패 롤백 경로에서 잡 스레드가 호출.
 * 에러 처리: rados_remove 의 반환값은 무시한다(존재하지 않아도 문제없음).
 */
static void _fio_rados_rm_objects(struct thread_data *td, struct rados_data *rados)
{
	size_t i;                                                               /* [한국어] 파일 순회 인덱스. */
	for (i = 0; i < td->o.nr_files; i++) {                                  /* [한국어] 잡의 모든 파일(오브젝트) 순회. */
		struct fio_file *f = td->files[i];                              /* [한국어] 해당 fio_file. */
		rados_remove(rados->io_ctx, f->file_name);                      /* [한국어] 오브젝트 이름=파일 이름 으로 RADOS 에서 삭제 요청. */
	}
}

/*
 * [한국어]
 * _fio_rados_connect - Ceph 클러스터에 연결하고 ioctx 를 만든 뒤, 필요 시 오브젝트를 프리터치.
 *
 * @td: 잡 스레드의 thread_data.
 * @return: 0 성공, 1 실패(클라 생성/설정파일/connect/ioctx/오브젝트 생성 중 하나라도 실패).
 *
 * 왜 필요한가: 실제 I/O 를 내보내기 전에 클러스터·풀·오브젝트 존재를 보장해야 측정 노이즈를 줄일 수 있다.
 * 실행 컨텍스트: 잡 스레드(setup 단계)에서 1회 호출.
 * 호출 체인: fio_rados_setup → _fio_rados_connect → librados(rados_create*/rados_connect/rados_ioctx_create/rados_write).
 * 에러 경로: 단계별 failed_* 라벨을 통해 이미 생성된 자원을 역순으로 롤백한다.
 */
static int _fio_rados_connect(struct thread_data *td)
{
	struct rados_data *rados = td->io_ops_data;                             /* [한국어] setup 에서 할당된 엔진 상태. */
	struct rados_options *o = td->eo;                                       /* [한국어] 파싱된 엔진 옵션. */
	int r;                                                                  /* [한국어] librados 반환값 저장용. */
	const uint64_t file_size =
		td->o.size / (td->o.nr_files ? td->o.nr_files : 1u);            /* [한국어] 파일별 균등 분배 크기. 0 나눗셈 방지. */
	struct fio_file *f;                                                     /* [한국어] 순회 임시 포인터. */
	uint32_t i;                                                             /* [한국어] 파일 순회 인덱스. */

	if (o->cluster_name) {                                                  /* [한국어] 사용자가 클러스터 이름을 지정했을 때. */
		char *client_name = NULL;                                       /* [한국어] "client." 접두어를 합친 문자열을 담을 임시 포인터. */

		/*
		* If we specify cluster name, the rados_create2
		* will not assume 'client.'. name is considered
		* as a full type.id namestr
		*/
		if (o->client_name) {                                           /* [한국어] 클라이언트 이름이 주어졌다면 규칙을 보정한다. */
			if (!index(o->client_name, '.')) {                      /* [한국어] '.' 이 없으면 type prefix 가 없다는 뜻 → "client." 를 붙인다. */
				client_name = calloc(1, strlen("client.") +
					strlen(o->client_name) + 1);            /* [한국어] 접두어+ID+NUL 길이로 버퍼 확보. */
				strcat(client_name, "client.");                 /* [한국어] 접두어 복사. */
				strcat(client_name, o->client_name);            /* [한국어] 사용자 지정 ID 이어붙이기. */
			} else {
				client_name = o->client_name;                   /* [한국어] 이미 "type.id" 형태이면 그대로 사용. */
			}
		}

		r = rados_create2(&rados->cluster, o->cluster_name,
			client_name, 0);                                        /* [한국어] cluster/client 를 모두 명시하는 rados_create2 호출. */

		if (client_name && !index(o->client_name, '.'))                 /* [한국어] 우리가 새로 할당한 경우에만 해제(사용자 문자열 건드리지 않음). */
			free(client_name);
	} else
		r = rados_create(&rados->cluster, o->client_name);              /* [한국어] 클러스터 미지정 시 기본 클러스터("ceph") 가정. */

	if (o->pool_name == NULL) {                                             /* [한국어] 풀 이름은 필수 — 없으면 바로 실패. */
		log_err("rados pool name must be provided.\n");
		goto failed_early;
	}

	if (r < 0) {                                                            /* [한국어] rados_create* 실패. */
		log_err("rados_create failed.\n");
		goto failed_early;
	}

	r = rados_conf_read_file(rados->cluster, o->conf);                      /* [한국어] ceph.conf 를 읽어 클라이언트 설정 적용. */
	if (r < 0) {
		log_err("rados_conf_read_file failed.\n");
		goto failed_early;
	}

	r = rados_connect(rados->cluster);                                      /* [한국어] 모니터에 접속하여 클러스터 map 을 얻는다. */
	if (r < 0) {
		log_err("rados_connect failed.\n");
		goto failed_early;
	}

	r = rados_ioctx_create(rados->cluster, o->pool_name, &rados->io_ctx);   /* [한국어] 지정 풀의 I/O 컨텍스트 생성. */
	if (r < 0) {
		log_err("rados_ioctx_create failed.\n");
		goto failed_shutdown;                                           /* [한국어] 클러스터는 이미 연결됐으므로 shutdown 까지 롤백. */
	}

	for (i = 0; i < td->o.nr_files; i++) {                                  /* [한국어] 각 파일(오브젝트)에 대해. */
		f = td->files[i];                                               /* [한국어] fio_file 획득. */
		f->real_file_size = file_size;                                  /* [한국어] fio 코어에 파일의 "실제 크기" 를 알린다(offset 생성 기준). */
		if (o->touch_objects) {                                         /* [한국어] 프리터치 옵션이 켜져 있으면. */
			r = rados_write(rados->io_ctx, f->file_name, "", 0, 0); /* [한국어] 0바이트 write 로 오브젝트를 존재시킨다(read-before-write 의 ENOENT 방지). */
			if (r < 0) {
				goto failed_obj_create;                         /* [한국어] 일부만 생성된 오브젝트들을 롤백해야 함. */
			}
		}
	}
	return 0;                                                               /* [한국어] 모든 단계 성공. */

failed_obj_create:
	_fio_rados_rm_objects(td, rados);                                       /* [한국어] 부분 생성된 오브젝트 제거. */
	rados_ioctx_destroy(rados->io_ctx);                                     /* [한국어] ioctx 해제. */
	rados->io_ctx = NULL;                                                   /* [한국어] dangling 방지. */
failed_shutdown:
	rados_shutdown(rados->cluster);                                         /* [한국어] 클러스터 연결 종료. */
	rados->cluster = NULL;                                                  /* [한국어] dangling 방지. */
failed_early:
	return 1;                                                               /* [한국어] 실패 반환. */
}

/*
 * [한국어]
 * _fio_rados_disconnect - ioctx 와 클러스터 핸들을 안전하게 해제한다.
 *
 * @rados: 엔진 상태. NULL 안전.
 * 실행 컨텍스트: 잡 스레드(cleanup).
 */
static void _fio_rados_disconnect(struct rados_data *rados)
{
	if (!rados)                                                             /* [한국어] NULL 가드. */
		return;

	if (rados->io_ctx) {                                                    /* [한국어] ioctx 가 유효하면 파괴. */
		rados_ioctx_destroy(rados->io_ctx);
		rados->io_ctx = NULL;
	}

	if (rados->cluster) {                                                   /* [한국어] 클러스터 핸들이 유효하면 shutdown. */
		rados_shutdown(rados->cluster);
		rados->cluster = NULL;
	}
}

/*
 * [한국어]
 * fio_rados_cleanup - 엔진 종료 시 in-flight I/O 가 모두 완료될 때까지 기다린 뒤 자원 해제.
 *
 * @td: 잡 스레드의 thread_data.
 * 실행 컨텍스트: 잡 스레드가 잡 루프 종료 후 1회 호출.
 * 호출 체인: backend.c(do_io 끝) → td_io_cleanup → fio_rados_cleanup.
 * 주의: 조건변수 대기 중에는 반드시 mutex 를 쥔 상태여야 하며, scheduled==completed 이어야 drain 완료.
 */
static void fio_rados_cleanup(struct thread_data *td)
{
	struct rados_data *rados = td->io_ops_data;                             /* [한국어] 엔진 상태 확보. */
	if (rados) {                                                            /* [한국어] setup 실패로 NULL 일 수 있음. */
		pthread_mutex_lock(&rados->completed_lock);                     /* [한국어] 카운터 읽기·대기 전 락. */
		while (rados->ops_scheduled != rados->ops_completed)            /* [한국어] 아직 미완료 I/O 가 있으면 대기 반복. */
			pthread_cond_wait(&rados->completed_more_io, &rados->completed_lock); /* [한국어] 콜백이 signal 할 때까지 대기(mutex 자동 해제/재획득). */
		pthread_mutex_unlock(&rados->completed_lock);                   /* [한국어] 드레인 완료 후 락 해제. */
		_fio_rados_rm_objects(td, rados);                               /* [한국어] 테스트 오브젝트 청소. */
		_fio_rados_disconnect(rados);                                   /* [한국어] ioctx/클러스터 해제. */
		free(rados->aio_events);                                        /* [한국어] 완료 반환 배열 해제. */
		free(rados);                                                    /* [한국어] 엔진 상태 해제. */
	}
}

/*
 * [한국어]
 * complete_callback - librados 가 비동기 I/O 완료를 알리기 위해 호출하는 콜백.
 *
 * @cb: 완료된 rados_completion_t. (assert 이외에는 사용하지 않음, fri 에서 참조)
 * @arg: 우리가 rados_aio_create_completion 에 넘긴 사용자 포인터(=fio_rados_iou*).
 *
 * 실행 컨텍스트: **librados 내부 메신저 스레드**. 잡 스레드와 경쟁하므로 반드시 락으로 보호.
 * 동작: 완료 리스트에 tail 추가 → 완료 카운터 증가 → 조건변수 signal 로 잡 스레드 깨움.
 * 주의: 여기서는 rados_aio_release 등 자원 해제는 수행하지 않고 getevents 에서 일괄 처리.
 */
static void complete_callback(rados_completion_t cb, void *arg)
{
	struct fio_rados_iou *fri = (struct fio_rados_iou *)arg;                /* [한국어] 우리가 넘긴 사용자 포인터 복원. */
	struct rados_data *rados = fri->td->io_ops_data;                        /* [한국어] 해당 잡의 엔진 상태로 점프. */
	assert(fri->completion);                                                /* [한국어] 완료 핸들이 세팅되어 있어야 함. */
	assert(rados_aio_is_complete(fri->completion));                         /* [한국어] 실제로 완료 상태인지 sanity check. */
	pthread_mutex_lock(&rados->completed_lock);                             /* [한국어] 공유 상태 보호 시작. */
	flist_add_tail(&fri->list, &rados->completed_operations);               /* [한국어] 완료 FIFO 의 꼬리에 enqueue. */
	rados->ops_completed++;                                                 /* [한국어] 완료 카운터 증가. */
	pthread_mutex_unlock(&rados->completed_lock);                           /* [한국어] 락 해제. */
	pthread_cond_signal(&rados->completed_more_io);                         /* [한국어] 대기 중인 잡 스레드 1개 깨움. */
}

/*
 * [한국어]
 * fio_rados_queue - ioengine_ops.queue 콜백. io_u 1개를 librados 로 비동기 제출.
 *
 * @td: 잡 스레드의 thread_data.
 * @io_u: 제출할 I/O 유닛(버퍼/오프셋/방향 포함).
 * @return: FIO_Q_QUEUED(제출 성공, 완료는 나중에), FIO_Q_COMPLETED(즉시 실패).
 *
 * 왜 이렇게: fio 는 엔진에 "큐에 넣기만" 요청하고 완료는 getevents 에서 수확한다.
 * 실행 컨텍스트: 잡 스레드.
 * 호출 체인: td_io_queue → fio_rados_queue → rados_aio_create_completion + rados_aio_write/read/write_op_operate.
 * 에러 경로: failed_write_op → failed_comp → failed 순서로 이미 할당된 자원 역순 해제.
 */
static enum fio_q_status fio_rados_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	struct rados_data *rados = td->io_ops_data;                             /* [한국어] 엔진 상태(ioctx 접근용). */
	struct fio_rados_iou *fri = io_u->engine_data;                          /* [한국어] io_u 당 컨텍스트(완료 핸들 저장소). */
	char *object = io_u->file->file_name;                                   /* [한국어] 대상 오브젝트 이름 = fio 파일 이름. */
	int r = -1;                                                             /* [한국어] librados 반환값. 실패 기본값. */

	fio_ro_check(td, io_u);                                                 /* [한국어] readonly 잡에서 write 요청이 오면 에러 처리. */

	if (io_u->ddir == DDIR_WRITE) {                                         /* [한국어] 쓰기 분기. */
		 r = rados_aio_create_completion(fri, complete_callback,
			NULL, &fri->completion);                                /* [한국어] 완료 콜백 등록 핸들 생성. 1st cb=완료, 2nd cb=safe(미사용). */
		if (r < 0) {
			log_err("rados_aio_create_completion failed.\n");
			goto failed;                                            /* [한국어] 핸들 생성 실패 → 완료 처리 즉시 실패로. */
		}

		r = rados_aio_write(rados->io_ctx, object, fri->completion,
			io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);       /* [한국어] 비동기 write 제출. OSD 가 처리 후 콜백. */
		if (r < 0) {
			log_err("rados_write failed.\n");
			goto failed_comp;                                       /* [한국어] 이미 만든 completion 을 release 해야 함. */
		}
		rados->ops_scheduled++;                                         /* [한국어] 제출 카운터 증가. */
		return FIO_Q_QUEUED;                                            /* [한국어] fio 코어에 "큐됨" 통지. */
	} else if (io_u->ddir == DDIR_READ) {                                   /* [한국어] 읽기 분기. */
		r = rados_aio_create_completion(fri, complete_callback,
			NULL, &fri->completion);                                /* [한국어] 완료 핸들 생성. */
		if (r < 0) {
			log_err("rados_aio_create_completion failed.\n");
			goto failed;
		}
		r = rados_aio_read(rados->io_ctx, object, fri->completion,
			io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);       /* [한국어] 비동기 read 제출. xfer_buf 로 결과가 채워진다. */
		if (r < 0) {
			log_err("rados_aio_read failed.\n");
			goto failed_comp;
		}
		rados->ops_scheduled++;                                         /* [한국어] 제출 카운터 증가. */
		return FIO_Q_QUEUED;
	} else if (io_u->ddir == DDIR_TRIM) {                                   /* [한국어] TRIM 분기 - RADOS 는 직접 TRIM 이 없으므로 write_op_zero 로 대체. */
		r = rados_aio_create_completion(fri, complete_callback,
			NULL , &fri->completion);                               /* [한국어] 완료 핸들 생성. */
		if (r < 0) {
			log_err("rados_aio_create_completion failed.\n");
			goto failed;
		}
		fri->write_op = rados_create_write_op();                        /* [한국어] 복합 write 오퍼레이션 객체 생성(여러 op 를 한 트랜잭션으로 묶음). */
		if (fri->write_op == NULL) {
			log_err("rados_create_write_op failed.\n");
			goto failed_comp;
		}
		rados_write_op_zero(fri->write_op, io_u->offset,
			io_u->xfer_buflen);                                     /* [한국어] 해당 영역을 0으로 채우는 op 추가(논리적 TRIM). */
		r = rados_aio_write_op_operate(fri->write_op, rados->io_ctx,
			fri->completion, object, NULL, 0);                      /* [한국어] write_op 를 비동기로 실행(완료는 콜백). */
		if (r < 0) {
			log_err("rados_aio_write_op_operate failed.\n");
			goto failed_write_op;                                   /* [한국어] write_op 까지 만든 상태이므로 추가 해제 필요. */
		}
		rados->ops_scheduled++;                                         /* [한국어] 제출 카운터 증가. */
		return FIO_Q_QUEUED;
	 }

	log_err("WARNING: Only DDIR_READ, DDIR_WRITE and DDIR_TRIM are supported!"); /* [한국어] 지원하지 않는 방향(sync, flush 등)이면 경고 후 실패 처리. */

failed_write_op:
	rados_release_write_op(fri->write_op);                                  /* [한국어] 만들어진 write_op 해제. */
failed_comp:
	rados_aio_release(fri->completion);                                     /* [한국어] 완료 핸들 해제. */
failed:
	io_u->error = -r;                                                       /* [한국어] errno 양수로 저장(관례). */
	td_verror(td, io_u->error, "xfer");                                     /* [한국어] fio 에 에러 등록(verror=verbose error). */
	return FIO_Q_COMPLETED;                                                 /* [한국어] 즉시 실패 완료 — fio 는 getevents 없이 바로 처리. */
}

/*
 * [한국어]
 * fio_rados_event - ioengine_ops.event 콜백. getevents 가 채운 완료 배열에서 event 번째 io_u 반환.
 *
 * @td: 잡 스레드의 thread_data.
 * @event: 0 ≤ event < getevents 반환값 범위의 인덱스.
 * @return: 해당 완료된 io_u 포인터.
 *
 * 실행 컨텍스트: 잡 스레드. getevents 직후 연속 호출됨.
 */
static struct io_u *fio_rados_event(struct thread_data *td, int event)
{
	struct rados_data *rados = td->io_ops_data;                             /* [한국어] 엔진 상태 확보. */
	return rados->aio_events[event];                                        /* [한국어] getevents 에서 채워둔 배열에서 꺼내 반환. */
}

/*
 * [한국어]
 * fio_rados_getevents - ioengine_ops.getevents 콜백. 완료 리스트에서 min..max 건을 수확.
 *
 * @td: 잡 스레드의 thread_data.
 * @min: 최소 수확 개수(미만이면 대기).
 * @max: 최대 수확 개수(aio_events 배열 크기 이내).
 * @t: 타임아웃(현 구현에서는 사용하지 않음 — 무한 대기).
 * @return: 실제 수확한 이벤트 수.
 *
 * 실행 컨텍스트: 잡 스레드. 콜백 스레드가 생산자, 이 함수가 소비자.
 * 호출 체인: td_io_getevents → fio_rados_getevents → (이후 fio_rados_event 가 io_u 하나씩 꺼내감).
 */
int fio_rados_getevents(struct thread_data *td, unsigned int min,
	unsigned int max, const struct timespec *t)
{
	struct rados_data *rados = td->io_ops_data;                             /* [한국어] 엔진 상태 확보. */
	unsigned int events = 0;                                                /* [한국어] 이번 호출에서 수확한 개수. */
	struct fio_rados_iou *fri;                                              /* [한국어] 리스트에서 꺼낸 엔트리. */

	pthread_mutex_lock(&rados->completed_lock);                             /* [한국어] 공유 상태 보호 시작. */
	while (events < min) {                                                  /* [한국어] 최소 개수 만족할 때까지 반복. */
		while (flist_empty(&rados->completed_operations)) {             /* [한국어] 리스트가 비어있으면 콜백이 넣어줄 때까지 대기. */
			pthread_cond_wait(&rados->completed_more_io, &rados->completed_lock); /* [한국어] 대기(spurious wakeup 대비 while 안에서). */
		}
		assert(!flist_empty(&rados->completed_operations));             /* [한국어] 깨어난 시점에 반드시 하나 이상 있어야 함. */

		fri = flist_first_entry(&rados->completed_operations, struct fio_rados_iou, list); /* [한국어] FIFO 의 머리 엔트리 얻기. */
		assert(fri->completion);                                        /* [한국어] 완료 핸들이 유효해야 함. */
		assert(rados_aio_is_complete(fri->completion));                 /* [한국어] 실제 완료 상태 확인. */
		if (fri->write_op != NULL) {                                    /* [한국어] TRIM 경로였다면 write_op 도 함께 해제. */
			rados_release_write_op(fri->write_op);
			fri->write_op = NULL;
		}
		rados_aio_release(fri->completion);                             /* [한국어] 완료 핸들 해제. */
		fri->completion = NULL;                                         /* [한국어] dangling 방지. */

		rados->aio_events[events] = fri->io_u;                          /* [한국어] 반환 배열에 기록(나중에 event() 가 꺼냄). */
		events ++;                                                      /* [한국어] 수확 개수 증가. */
		flist_del(&fri->list);                                          /* [한국어] 완료 리스트에서 제거. */
		if (events >= max) break;                                       /* [한국어] 최대 개수 도달 시 종료. */
	}
	pthread_mutex_unlock(&rados->completed_lock);                           /* [한국어] 락 해제. */
	return events;                                                          /* [한국어] 수확 개수 반환. */
}

/*
 * [한국어]
 * fio_rados_setup - ioengine_ops.setup 콜백. 엔진 상태 생성 및 클러스터 연결.
 *
 * @td: 잡 스레드의 thread_data.
 * @return: 0 성공, 음수/양수 실패 코드(호출자는 잡을 중단).
 *
 * 실행 컨텍스트: 잡 스레드. 잡 루프 이전 초기화 단계.
 * 부가 효과: td->o.use_thread = 1 강제 — RADOS 클라이언트가 프로세스 fork 와 호환되지 않으므로.
 */
static int fio_rados_setup(struct thread_data *td)
{
	struct rados_data *rados = NULL;                                        /* [한국어] 엔진 상태 출력 포인터. */
	int r;                                                                  /* [한국어] 에러 코드. */
	/* allocate engine specific structure to deal with librados. */
	r = _fio_setup_rados_data(td, &rados);                                  /* [한국어] 상태 구조체 할당/초기화. */
	if (r) {
		log_err("fio_setup_rados_data failed.\n");
		goto cleanup;
	}
	td->io_ops_data = rados;                                                /* [한국어] fio 코어에 엔진 상태 등록(이후 모든 콜백에서 재사용). */

	/* Force single process mode.
	*/
	td->o.use_thread = 1;                                                   /* [한국어] 프로세스 fork 대신 pthread 사용 강제(librados 상태 보호). */

	/* connect in the main thread to determine to determine
	* the size of the given RADOS block device. And disconnect
	* later on.
	*/
	r = _fio_rados_connect(td);                                             /* [한국어] 클러스터 연결 + ioctx 생성 + 오브젝트 프리터치. */
	if (r) {
		log_err("fio_rados_connect failed.\n");
		goto cleanup;
	}
	rados->connected = true;                                                /* [한국어] 이후 cleanup 경로가 참조 가능하도록 표시. */

	return 0;                                                               /* [한국어] 성공. */
cleanup:
	fio_rados_cleanup(td);                                                  /* [한국어] 부분 초기화 상태를 정리. */
	return r;                                                               /* [한국어] 상위로 실패 전파. */
}

/* open/invalidate are noops. we set the FIO_DISKLESSIO flag in ioengine_ops to
   prevent fio from creating the files
*/
/*
 * [한국어]
 * fio_rados_open - ioengine_ops.open_file 콜백. RADOS 는 파일 대신 오브젝트 이름을 쓰므로 no-op.
 * FIO_DISKLESSIO 플래그 덕분에 fio 코어가 open(2) 를 호출하지 않아도 된다.
 * @return: 0 항상 성공.
 */
static int fio_rados_open(struct thread_data *td, struct fio_file *f)
{
	return 0;                                                               /* [한국어] 아무 것도 하지 않고 성공 반환. */
}

/*
 * [한국어]
 * fio_rados_invalidate - ioengine_ops.invalidate 콜백. 페이지 캐시 무효화가 의미 없으므로 no-op.
 * @return: 0 항상 성공.
 */
static int fio_rados_invalidate(struct thread_data *td, struct fio_file *f)
{
	return 0;                                                               /* [한국어] OSD 측 캐시는 클라이언트에서 제어 불가 → no-op. */
}

/*
 * [한국어]
 * fio_rados_io_u_free - ioengine_ops.io_u_free 콜백. io_u 당 fio_rados_iou 해제.
 *
 * @td: 잡 스레드 thread_data.
 * @io_u: 반환되는 I/O 유닛.
 *
 * 실행 컨텍스트: 잡 스레드(정리 단계). 잔존 completion/write_op 가 있으면 함께 해제.
 */
static void fio_rados_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct fio_rados_iou *fri = io_u->engine_data;                          /* [한국어] io_u 에 붙어 있던 엔진 컨텍스트. */

	if (fri) {                                                              /* [한국어] 할당되지 않은 경우 대비. */
		io_u->engine_data = NULL;                                       /* [한국어] 먼저 링크 끊어 double-free 방지. */
		fri->td = NULL;                                                 /* [한국어] 역참조 클리어(dangling 방지). */
		if (fri->completion)                                            /* [한국어] 혹시 남은 completion 해제. */
			rados_aio_release(fri->completion);
		if (fri->write_op)                                              /* [한국어] 혹시 남은 write_op 해제. */
			rados_release_write_op(fri->write_op);
		free(fri);                                                      /* [한국어] 컨텍스트 해제. */
	}
}

/*
 * [한국어]
 * fio_rados_io_u_init - ioengine_ops.io_u_init 콜백. io_u 당 fio_rados_iou 할당.
 *
 * @td: 잡 스레드 thread_data.
 * @io_u: 초기화 대상 I/O 유닛.
 * @return: 0 성공(이 구현은 실패 시에도 0 반환 — calloc 실패 체크 부재).
 *
 * 실행 컨텍스트: 잡 스레드(잡 초기화 단계, io_u 풀 생성 시).
 */
static int fio_rados_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct fio_rados_iou *fri;                                              /* [한국어] 새 컨텍스트. */
	fri = calloc(1, sizeof(*fri));                                          /* [한국어] 0 초기화 할당(포인터/리스트 헤드 안전). */
	fri->io_u = io_u;                                                       /* [한국어] 역참조 연결. */
	fri->td = td;                                                           /* [한국어] 콜백에서 io_ops_data 찾을 경로. */
	INIT_FLIST_HEAD(&fri->list);                                            /* [한국어] 완료 리스트 링크 노드 초기화. */
	io_u->engine_data = fri;                                                /* [한국어] io_u 에 엔진 컨텍스트 부착. */
	return 0;                                                               /* [한국어] 성공. */
}

/*
 * [한국어] ioengine — fio 코어가 엔진을 호출할 때 사용하는 콜백 vtable.
 * get_ioengine()(ioengines.c) 가 이름 "rados" 로 engine_list 에서 조회하며, 각 필드는
 * 잡 루프의 특정 훅에 매핑된다. FIO_STATIC 매크로는 이 심볼이 내장(static 링크) 에서는
 * extern 가시성, 외부 공유라이브러리 빌드(.so)에서는 static 가시성을 갖도록 전환한다.
 *
 * 이 vtable 의 반환 코드 계약 요약:
 *   - queue 의 FIO_Q_QUEUED: 비동기 수락(완료는 나중에 getevents 로 수확).
 *   - queue 의 FIO_Q_COMPLETED: 즉시 완료(동기 에러 경로). 코어가 put_io_u 까지 수행.
 *   - queue 의 FIO_Q_BUSY: 사용 안 함(이 엔진은 제출 포화로 인한 백프레셔 경로 없음).
 * 실행 컨텍스트는 모두 잡 스레드(단 complete_callback 만 librados 메신저 스레드).
 */
/* ioengine_ops for get_ioengine() */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "rados",
	/* [한국어] 엔진 이름 — `--ioengine=rados` 로 선택되는 키.
	 * 설정자: 이 정적 초기화. 읽는 자: register_ioengine → engine_list 링크 + 추후
	 *         load_ioengine 이 이름으로 탐색.
	 * 값 범위: 고정 상수 "rados". 다른 엔진과 충돌 금지.
	 * 동기화: read-only 전역. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine_ops ABI 버전(fio.h 정의). fio 코어와 버전 불일치 시 load 단계
	 * 에서 check_engine_ops 가 거부하여 잡 전체를 실패시킨다.
	 * 설정자: 컴파일 타임 매크로. 읽는 자: register_ioengine/check_engine_ops. */

	.flags			= FIO_DISKLESSIO,
	/* [한국어] 엔진 동작 힌트 비트마스크.
	 *   FIO_DISKLESSIO(1<<7 근방): "실제 파일 시스템 파일이 아님" — fio 코어가 파일
	 *     open/스캔/stat 으로 크기 계산을 시도하지 않고, 엔진이 real_file_size 를
	 *     직접 주입하도록 위임. 오브젝트 스토리지/네트워크 스토리지 엔진의 표준 플래그.
	 * 미설정 비트: FIO_SYNCIO(비동기 엔진이므로), FIO_RAWIO(블록디바이스 직접 아님),
	 *   FIO_NOEXTEND(오브젝트 크기 확장 허용), FIO_NODISKUTIL(설정 안 함 — 하지만
	 *   대상이 디스크가 아니라 utility 통계는 실질적으로 비어있음), FIO_MEMALIGN(정렬
	 *   요구 없음), FIO_BARRIER/FIO_UNIDIR 등. */

	.setup			= fio_rados_setup,
	/* [한국어] 잡 초기화 훅 — td_io_setup 에서 init 보다 먼저 호출되어 엔진 상태
	 * 할당과 Ceph 클러스터 연결을 수행한다.
	 * 설정자: 이 초기화. 읽는 자: backend.c/init.c 의 td_io_setup.
	 * 역할: use_thread=1 강제, 상태 구조체 할당, 클러스터 connect + ioctx create
	 *       + (touch_objects 이면) 오브젝트 프리터치.
	 * 동기화: 잡 스레드가 단독 실행. */

	.queue			= fio_rados_queue,
	/* [한국어] io_u 한 개를 비동기 제출. 반환값:
	 *   FIO_Q_QUEUED   - 정상 제출. 완료는 콜백을 통해 getevents 에서 수확.
	 *   FIO_Q_COMPLETED - 즉시 에러 완료(td_verror 세팅 후). 코어가 put_io_u 처리.
	 * 호출자: td_io_queue(ioengines.c). 호출 시점: get_io_u 이후 do_io 루프. */

	.getevents		= fio_rados_getevents,
	/* [한국어] 완료 이벤트 수집(min..max). completed_lock 을 잡고 completed_operations
	 * 리스트를 드레인하며, 최소 개수 미달 시 조건변수 wait.
	 * 반환: 실제 수집 이벤트 수(>=min 보장). aio_events[] 에 io_u 포인터를 기록한다. */

	.event			= fio_rados_event,
	/* [한국어] getevents 가 N 을 반환한 뒤, 인덱스 0..N-1 로 호출되어 io_u* 를 반환.
	 * rados->aio_events[event] 를 그대로 돌려주는 경량 조회 함수. */

	.cleanup		= fio_rados_cleanup,
	/* [한국어] 잡 종료 시 1회 호출 — scheduled==completed 까지 드레인 대기 → 오브젝트
	 * 제거(_fio_rados_rm_objects) → ioctx/cluster 해제(_fio_rados_disconnect) →
	 * aio_events/상태 구조체 free. io_u 모두 소멸(io_u_free) 이후에 호출됨을 코어가 보장. */

	.open_file		= fio_rados_open,
	/* [한국어] 파일 단위 open 콜백(no-op). RADOS 는 "파일" 이 없고 오브젝트 이름
	 * 문자열만 필요하므로 실제 open 작업이 없다. FIO_DISKLESSIO 덕분에 fio 코어가
	 * 이 훅을 건너뛸 수도 있으나 vtable 에 명시해 두면 모든 경로에서 안전하다. */

	.invalidate		= fio_rados_invalidate,
	/* [한국어] 로컬 호스트 캐시 무효화 훅(no-op). OSD 측 캐시는 클라이언트에서 제어
	 * 불가(풀 단위 cache tier/writeback 정책은 OSD 가 관리). 따라서 아무 작업도 없이
	 * 성공 반환한다. */

	.options		= options,
	/* [한국어] 엔진 전용 옵션 테이블 포인터. NULL 센티널로 끝나는 fio_option 배열.
	 * 파서가 이 배열을 순회하며 FIO_OPT_C_ENGINE/FIO_OPT_G_RBD 그룹으로 등록한다. */

	.io_u_init		= fio_rados_io_u_init,
	/* [한국어] io_u 최초 생성 시 호출 — per-io_u fio_rados_iou 를 calloc 하여 부착.
	 * 잡 초기화 중 io_u 풀(iodepth 개)을 만들 때 각 io_u 마다 한 번씩 호출된다.
	 * 동기화: 잡 스레드 단독 실행(io_u 풀 생성 단계). */

	.io_u_free		= fio_rados_io_u_free,
	/* [한국어] io_u 파괴 시 fri 해제. completion/write_op 가 남아 있으면 마저 해제.
	 * cleanup 이전에 코어가 모든 io_u 에 대해 호출함을 보장. */

	.option_struct_size	= sizeof(struct rados_options),
	/* [한국어] td->eo 로 할당될 옵션 구조체 크기. 파서가 잡마다 독립된 옵션 복제본을
	 * 만들 때 이 크기로 calloc 한다.
	 * 설정자: 컴파일 타임 sizeof. 읽는 자: fio 옵션 파싱 루틴. */
};

/*
 * [한국어]
 * fio_rados_register - fio_init 생성자 — 프로그램 시작 시 엔진을 등록한다.
 *
 * @return: 없음.
 *
 * 이 함수는 main() 진입 이전(libc 동적 로더가 .init_array 를 실행하는 시점) 에
 * 자동 호출되어 "rados" 이름의 엔진을 전역 engine_list 에 등록한다. fio_init 속성은
 * compiler/compiler.h 에서 __attribute__((constructor)) 로 확장되며, 같은 기법을
 * 쓰는 모든 엔진은 main 시작 전에 register_ioengine 을 완료하므로 load_ioengine 이
 * "rados" 키워드로 곧바로 이 vtable 을 찾을 수 있다.
 *
 * 실행 컨텍스트: 프로세스의 _start → __libc_start_main 의 초기화 단계. 아직 잡
 * 스레드는 생성되지 않았으며, ld.so 가 단일 스레드로 생성자를 순차 호출한다.
 *
 * 호출 체인: ld.so(.init_array) → [이 함수] → register_ioengine() → flist_add_tail
 */
static void fio_init fio_rados_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] fio 코어의 engine_list(링크드 리스트)에 이 ioengine vtable 을 등록.
	 * load_ioengine("rados") 가 이 리스트를 순회해 name == "rados" 를 찾는다. */
}

/*
 * [한국어]
 * fio_rados_unregister - fio_exit 소멸자 — 프로세스 종료 시 엔진 등록 해제.
 *
 * @return: 없음.
 *
 * fio_exit 속성은 __attribute__((destructor)) 로 확장되어 libc 의 atexit 체인을
 * 통해 main 종료 후 자동 호출된다. engine_list 에서 이 vtable 을 안전하게 분리해
 * 이후 접근 시 dangling 을 방지한다.
 *
 * 실행 컨텍스트: 모든 잡 스레드가 join 된 후의 단일 스레드. exit() 의 atexit 호출.
 *
 * 호출 체인: libc(atexit/.fini_array) → [이 함수] → unregister_ioengine() → flist_del
 */
static void fio_exit fio_rados_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 engine_list 에서 이 엔트리를 제거 — 이후 load_ioengine("rados")
	 * 는 NULL 을 반환한다. */
}
