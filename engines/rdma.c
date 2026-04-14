/*
 * [한국어 설명] RDMA I/O 엔진 구현 (rdma.c)
 *
 * === 파일의 역할 ===
 * fio의 I/O 엔진 플러그인 중 하나로, InfiniBand verbs(libibverbs)와 RDMA Connection
 * Manager(librdmacm)를 기반으로 RDMA(Remote Direct Memory Access) I/O를 수행한다.
 * RDMA 메모리 시맨틱(RDMA WRITE/READ — 원격 메모리에 직접 read/write)과 채널 시맨틱
 * (SEND/RECV — 전통적 메시지 전달)을 모두 지원하며, InfiniBand/RoCE/iWARP 프로토콜 위에서
 * 동작한다. 클라이언트는 --ioengine=rdma + hostname 옵션으로 원격 fio 서버에 접속하고,
 * 서버는 같은 엔진을 read 모드로 실행하여 listen한다. 본 파일은 엔진 플러그인 계약
 * (struct ioengine_ops)에 정의된 콜백들을 구현하여 fio 코어(backend.c, io_u.c)와 연동된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 일반적인 잡 실행 흐름 (fio.c → backend.c → ioengines.c → td_io_queue/commit/getevents)
 * 중 I/O 수행 계층에 위치한다. 이 엔진은 디스크 I/O가 아닌 네트워크(InfiniBand HCA) 기반
 * I/O이므로 FIO_DISKLESSIO·FIO_PIPEIO 플래그로 등록된다. 호출 체인:
 *   backend.c(thread_main) → ioengines.c(td_io_queue/commit/getevents) → 본 파일의 콜백
 *     → libibverbs(ibv_post_send/recv, ibv_poll_cq) / librdmacm(rdma_connect/accept)
 *     → 커널 RDMA 서브시스템(uverbs) → HCA 드라이버(mlx5/mlx4 등) → NIC 하드웨어
 * 실행 컨텍스트는 fio 잡 스레드(호스트 유저스페이스) 및 별도 RDMA CM 이벤트 스레드
 * (pthread로 분리 가능)이며, 하드웨어 완료 통지는 CQ(Completion Queue) 이벤트 채널로 수신한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존: libibverbs(verbs API, struct ibv_qp/cq/pd/mr), librdmacm(rdma_cm_id, 이벤트 채널),
 * fio 내부 모듈 ../fio.h(thread_data, io_u, fio_file), ../hash.h, ../optgroup.h(옵션 그룹).
 * 본 파일에 의존: ioengines.c의 register_ioengine()를 통해 런타임에 동적으로 엔진 테이블에 등록된다.
 * 데이터 흐름: fio 코어가 할당한 io_u 버퍼 → ibv_reg_mr로 NIC이 DMA할 수 있는 MR(Memory Region)
 * 등록 → QP(Queue Pair)에 WR(Work Request) 포스트 → HCA가 DMA로 원격 메모리로 전송/수신
 * → CQ에 CQE(Completion Queue Entry) 기록 → ibv_poll_cq로 수확 → fio의 io_u 완료로 보고.
 * 공유 상태: struct rdmaio_data가 td->io_ops_data에 저장되어 잡 스레드 내부에서만 접근된다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct rdmaio_data: 엔진 인스턴스 상태(QP/CQ/PD/MR, 연결 상태, io_u 큐 3종 등)
 * - struct rdma_io_u_data: io_u별 WR 기술자(send/recv WR, SGE)
 * - struct rdma_info_blk: 클라이언트-서버 간 제어 메시지(mode, rkey, remote addr 등)
 * - fio_rdmaio_init/post_init: CM 이벤트 채널 준비, QP 생성, io_u 버퍼 MR 등록
 * - fio_rdmaio_prep/queue/commit: 각 I/O를 WR로 변환하고 ibv_post_send/recv로 제출
 * - fio_rdmaio_getevents/event: ibv_poll_cq로 완료 수집 후 fio에 io_u 단위로 반환
 * - cq_event_handler: CQE 1개를 파싱해 flight → completed 상태로 전이
 * - fio_rdmaio_connect/accept: 클라이언트·서버 측 핸드셰이크 및 원격 MR 정보 교환
 *
 * === fio에서의 사용법 ===
 * --ioengine=rdma 옵션으로 선택하며 hostname, port, verb(write/read/send/recv), bindname
 * 서브옵션을 받는다. verb=write/read는 RDMA 메모리 시맨틱, send/recv는 채널 시맨틱이다.
 *
 * === 구현하는 주요 콜백 ===
 * .setup, .init, .post_init, .prep, .queue, .commit,
 * .getevents, .event, .cleanup, .open_file, .close_file
 */

/*
 * RDMA I/O engine
 *
 * RDMA I/O engine based on the IB verbs and RDMA/CM user space libraries.
 * Supports both RDMA memory semantics and channel semantics
 *   for the InfiniBand, RoCE and iWARP protocols.
 *
 * You will need the Linux RDMA software installed, either
 * from your Linux distributor or directly from openfabrics.org:
 *
 * http://www.openfabrics.org/downloads/OFED/
 *
 * Exchanging steps of RDMA ioengine control messages:
 *	1. client side sends test mode (RDMA_WRITE/RDMA_READ/SEND)
 *	   to server side.
 *	2. server side parses test mode, and sends back confirmation
 *	   to client side. In RDMA WRITE/READ test, this confirmation
 *	   includes memory information, such as rkey, address.
 *	3. client side initiates test loop.
 *	4. In RDMA WRITE/READ test, client side sends a completion
 *	   notification to server side. Server side updates its
 *	   td->done as true.
 *
 */
/* [한국어] 표준 C 라이브러리 헤더들 — printf/log 출력, malloc/free, read/write/close,
 *         errno, assert 등 엔진 구현 전반에서 사용하는 기본 인프라 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
/* [한국어] BSD 소켓·주소 변환 헤더 — rdma_cm이 sockaddr_in을 사용하므로 필요.
 *         inet_aton/gethostbyname으로 호스트명을 IP 주소로 변환한다. */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
/* [한국어] poll() 시스템 콜 선언 — 이벤트 채널 대기에서 사용 가능(현 파일에서는
 *         rdma_get_cm_event 블로킹 호출로 처리하지만, CM 이벤트 소켓 폴링 맥락으로 포함) */
#include <poll.h>
/* [한국어] 소켓/시간/리소스 한계 — setrlimit(RLIMIT_MEMLOCK)로 pinning 가능 메모리
 *         한계를 조정하기 위해 sys/resource.h 필요 (check_set_rlimits 참조) */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>

/* [한국어] 현재 파일에서는 pthread_t 선언에만 쓰이지만, 향후 CM 이벤트 처리 스레드를
 *         분리할 수 있도록 포함한다 (rdmaio_data::cmthread 필드 참조) */
#include <pthread.h>
/* [한국어] PRIx64/PRId64 등 고정폭 정수 포맷 매크로 — dprint/log_err에서 사용 */
#include <inttypes.h>

/* [한국어] fio 코어 헤더 — struct thread_data, io_u, fio_file, ioengine_ops 정의 제공 */
#include "../fio.h"
/* [한국어] __rand 등 해시/난수 유틸 — 원격 MR 인덱스 무작위 선택(rand_state)에서 사용 */
#include "../hash.h"
/* [한국어] FIO_OPT_G_RDMA 옵션 그룹 상수 정의 — 서브옵션을 'RDMA engine' 그룹으로 묶기 위함 */
#include "../optgroup.h"

/* [한국어] RDMA Connection Manager API — rdma_create_id/connect/accept/bind_addr 등 제공.
 *         InfiniBand 수준의 주소 해석 및 QP 생성을 TCP 유사한 시맨틱으로 감싼다. */
#include <rdma/rdma_cma.h>

/* [한국어] 이 엔진이 지원하는 최대 io_depth 상한. send/recv WR의 wr_id가 이 값일 때는
 *         "제어 메시지용 특수 WR"로 해석되므로(예: wc.wr_id == FIO_RDMA_MAX_IO_DEPTH),
 *         일반 io_u의 wr_id(0..iodepth-1)와 구분되는 센티널로도 쓰인다. */
#define FIO_RDMA_MAX_IO_DEPTH    512

/*
 * [한국어] RDMA 동작 모드 열거형.
 * - FIO_RDMA_UNKNOWN: 초기화 전 미정 상태 (0으로 초기화되므로 기본값 방어용).
 * - FIO_RDMA_MEM_WRITE: 클라이언트가 RDMA WRITE로 서버의 원격 메모리에 직접 쓰기.
 * - FIO_RDMA_MEM_READ:  클라이언트가 RDMA READ로 서버의 원격 메모리를 직접 읽기.
 * - FIO_RDMA_CHA_SEND:  채널 시맨틱 송신(IBV_WR_SEND). 서버 측 post_recv 버퍼로 복사.
 * - FIO_RDMA_CHA_RECV:  채널 시맨틱 수신. 서버가 클라이언트의 SEND를 받는 쪽.
 * 설정 경로: rdmaio_options.verb(사용자 입력) → fio_rdmaio_init에서 rdmaio_data.rdma_protocol로 전파.
 */
enum rdma_io_mode {
	FIO_RDMA_UNKNOWN = 0,
	FIO_RDMA_MEM_WRITE,
	FIO_RDMA_MEM_READ,
	FIO_RDMA_CHA_SEND,
	FIO_RDMA_CHA_RECV
};

/* [한국어] RDMA 엔진 전용 옵션 구조체 — fio 옵션 파서가 사용자로부터 받은 값을 담는다.
 * 인스턴스는 td->eo에 저장되며, option_struct_size로 크기를 등록(맨 아래 ioengine 구조체 참조). */
struct rdmaio_options {
	struct thread_data *td;
	/* [한국어] 이 옵션 구조체가 속한 잡의 thread_data 백 포인터.
	 * 설정자: fio 옵션 파서가 옵션 구조체를 초기화할 때 자동으로 채움.
	 * 읽는 자: str_hostname_cb() — 콜백 내부에서 td->o.filename을 갱신하기 위해 사용.
	 * 값 범위: 항상 유효한 thread_data 포인터 (NULL 불가).
	 * 동기화: 잡 스레드 전용이라 별도 락 불필요. */

	unsigned int port;
	/* [한국어] RDMA 연결에 사용할 TCP 유사 포트 번호 (1~65535).
	 * 설정자: 사용자 --port 옵션, compat_options()에서 filename 파싱 시.
	 * 읽는 자: fio_rdmaio_setup_connect/setup_listen에서 htons 후 sockaddr_in에 반영.
	 * 값 범위: 1..65535 (options[]의 minval/maxval로 검증).
	 * 동기화: 잡 스레드 전용. */

	enum rdma_io_mode verb;
	/* [한국어] 사용자 선택 RDMA verb (write/read/send/recv).
	 * 설정자: --verb 옵션 또는 compat_options()의 filename 경로 파싱.
	 * 읽는 자: fio_rdmaio_init에서 rd->rdma_protocol에 복사.
	 * 값 범위: enum rdma_io_mode의 유효값 중 하나.
	 * 동기화: 잡 스레드 전용. */

	char *bindname;
	/* [한국어] 로컬 RDMA 장치 바인딩 주소(멀티 HCA 환경에서 특정 장치 선택용).
	 * 설정자: --bindname 옵션 (기본 ""). 비어 있으면 NULL 주소로 resolve.
	 * 읽는 자: fio_rdmaio_setup_connect/setup_listen에서 aton()을 통해 sockaddr로 변환.
	 * 값 범위: 유효한 IPv4 문자열 또는 빈 문자열.
	 * 동기화: 잡 스레드 전용. */
};

/*
 * [한국어]
 * str_hostname_cb - hostname 옵션 파싱 콜백.
 *
 * @data:  rdmaio_options 포인터 (fio가 옵션 구조체를 전달).
 * @input: 사용자가 입력한 호스트명 문자열.
 * @return: 0 고정 (성공).
 *
 * 동기: fio의 옵션은 엔진별로 정의되지만, hostname은 td->o.filename에도 반영되어야 한다
 * (fio는 파일명/네트워크 대상을 filename으로 통일해 다룸). 이 콜백은 입력을 복제해
 * td->o.filename에 저장함으로써 두 경로 모두에서 호스트명을 읽을 수 있게 한다.
 * 호출 컨텍스트: fio 옵션 파싱 단계(메인 프로세스, 잡 초기화 이전).
 * 호출 체인: parse_options(init.c) → fio_option.cb → [이 함수]
 */
static int str_hostname_cb(void *data, const char *input)
{
	/* [한국어] void* 를 실제 옵션 구조체 포인터로 캐스트 (옵션 콜백 규약) */
	struct rdmaio_options *o = data;

	/* [한국어] 기존에 filename이 이미 할당돼 있으면 중복 할당 방지를 위해 해제 */
	if (o->td->o.filename)
		free(o->td->o.filename);
	/* [한국어] 사용자 입력 문자열을 heap으로 복제하여 thread_data의 filename 필드에 저장.
	 *         — filename은 잡 전 생명주기 동안 유지되어야 하므로 strdup 사용. */
	o->td->o.filename = strdup(input);
	/* [한국어] 옵션 콜백 규약상 0=성공을 반환 (parse 에러 없음) */
	return 0;
}

/*
 * [한국어] 엔진 옵션 테이블 — fio가 --enghelp rdma 또는 잡 파일 파싱 시 이 배열을 순회한다.
 * 각 엔트리는 { .name, .type, .off1(옵션 구조체 내 오프셋), .cb, .help, .group } 형태.
 * 마지막은 .name=NULL 종단자. FIO_OPT_STR_STORE는 문자열 복제, FIO_OPT_INT는 정수, FIO_OPT_STR은 열거형.
 */
static struct fio_option options[] = {
	{
		.name	= "hostname",              /* [한국어] 원격 서버 호스트명/IP */
		.lname	= "rdma engine hostname",  /* [한국어] 긴 이름(도움말에 표시) */
		.type	= FIO_OPT_STR_STORE,       /* [한국어] 문자열로 저장하는 옵션 */
		.cb	= str_hostname_cb,         /* [한국어] filename에도 반영하기 위한 콜백 */
		.help	= "Hostname for RDMA IO engine",
		.category = FIO_OPT_C_ENGINE,      /* [한국어] 카테고리: 엔진 옵션 */
		.group	= FIO_OPT_G_RDMA,          /* [한국어] 그룹: RDMA */
	},
	{
		.name	= "bindname",
		.lname	= "rdma engine bindname",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct rdmaio_options, bindname), /* [한국어] 옵션 구조체 오프셋으로 직접 저장 */
		.help	= "Bind for RDMA IO engine",
		.def    = "",                      /* [한국어] 기본값: 빈 문자열 = 바인딩 없음 */
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_RDMA,
	},
	{
		.name	= "port",
		.lname	= "rdma engine port",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct rdmaio_options, port),
		.minval	= 1,                       /* [한국어] TCP 유효 포트 범위 최소 */
		.maxval	= 65535,                   /* [한국어] 16비트 포트 상한 */
		.help	= "Port to use for RDMA connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_RDMA,
	},
	{
		.name	= "verb",
		.lname	= "RDMA engine verb",
		.alias	= "proto",                 /* [한국어] proto로도 입력 허용 (하위호환) */
		.type	= FIO_OPT_STR,             /* [한국어] 열거형 문자열 → posval 매핑 */
		.off1	= offsetof(struct rdmaio_options, verb),
		.help	= "RDMA engine verb",
		.def	= "write",                 /* [한국어] 기본 동작은 RDMA WRITE */
		.posval = {
			  /* [한국어] 각 posval은 문자열 입력 → enum 값 매핑을 정의 */
			  { .ival = "write",
			    .oval = FIO_RDMA_MEM_WRITE,
			    .help = "Memory Write",
			  },
			  { .ival = "read",
			    .oval = FIO_RDMA_MEM_READ,
			    .help = "Memory Read",
			  },
			  { .ival = "send",
			    .oval = FIO_RDMA_CHA_SEND,
			    .help = "Posted Send",
			  },
			  { .ival = "recv",
			    .oval = FIO_RDMA_CHA_RECV,
			    .help = "Posted Receive",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_RDMA,
	},
	{
		.name	= NULL,                    /* [한국어] 옵션 배열 종단자 */
	},
};

/* [한국어] 원격 RDMA 메모리 영역 정보 — RDMA WRITE/READ에서 대상 서버가 노출한 MR을 기술.
 * 클라이언트는 서버로부터 이 정보를 제어 메시지(rdma_info_blk.rmt_us)로 수신 후, 매 I/O마다
 * 이 중 하나를 난수(rand_state)로 선택하여 WR의 remote_addr/rkey에 채운다. */
struct remote_u {
	uint64_t buf;
	/* [한국어] 서버가 등록한 원격 버퍼의 가상 주소 (서버 측 VA).
	 * 설정자: 서버 fio_rdmaio_post_init()이 cpu_to_be64로 네트워크 바이트순 저장.
	 * 읽는 자: 클라이언트 client_recv() __be64_to_cpu로 변환, fio_rdmaio_send()에서 사용.
	 * 값 범위: 서버 주소공간의 유효 VA. HCA가 MR 등록을 통해 DMA 가능하도록 pin한 영역.
	 * 동기화: 연결 수립 시 1회 교환 후 불변. */

	uint32_t rkey;
	/* [한국어] 원격 MR의 rkey — RDMA WRITE/READ WR에 포함되며 HCA가 접근 권한 검증에 사용.
	 * 설정자: 서버 ibv_reg_mr()이 반환한 mr->rkey를 htonl로 저장.
	 * 읽는 자: 클라이언트 ntohl로 복원 후 sq_wr.wr.rdma.rkey에 대입.
	 * 값 범위: HCA가 발급한 32비트 키. 연결 유지 중 유효.
	 * 동기화: 수신 후 불변. */

	uint32_t size;
	/* [한국어] 원격 버퍼 크기(바이트). 클라이언트가 WRITE/READ 길이 검증·제한에 사용 가능.
	 * 설정자: 서버 측 max_bs 값을 htonl로 저장.
	 * 읽는 자: 클라이언트 client_recv()가 dprint 로그로 확인.
	 * 값 범위: max_bs 이상. 동기화: 1회 교환 후 불변. */
};

/*
 * [한국어] 클라이언트-서버 간 제어 메시지(컨트롤 플레인 메타데이터) 포맷.
 * SEND/RECV로 교환되며 연결 초기·종료 시 1~2회만 주고받는다. 네트워크 바이트순(htonl/ntohl) 사용.
 */
struct rdma_info_blk {
	uint32_t mode;		/* channel semantic or memory semantic */
	/* [한국어] 클라이언트가 선택한 RDMA 모드(enum rdma_io_mode). 서버는 이 값을 받아
	 * 자신의 rd->rdma_protocol을 결정. 네트워크 바이트순. 1회 교환 후 불변. */

	uint32_t nr;		/* client: io depth
				   server: number of records for memory semantic
				 */
	/* [한국어] 의미가 송수신 측에 따라 다름:
	 *  - 클라이언트→서버: iodepth (서버가 준비할 recv 버퍼 수 결정에 참고).
	 *  - 서버→클라이언트: rmt_us[] 배열에서 유효한 엔트리 수 (클라이언트가 선택 가능한 원격 MR 개수).
	 * 네트워크 바이트순. 1회 교환 후 불변. */

	uint32_t max_bs;        /* maximum block size */
	/* [한국어] 블록 크기 상한. 서버는 자신의 max_bs를 알려 클라이언트의 max_bs가 더 크면
	 * 오류를 보고하도록 한다(client_recv의 검증 로직 참조). 네트워크 바이트순. */

	struct remote_u rmt_us[FIO_RDMA_MAX_IO_DEPTH];
	/* [한국어] 서버가 노출하는 원격 MR 리스트(최대 512개). MEM_WRITE/READ 모드에서만 유효.
	 * 설정자: 서버 fio_rdmaio_post_init()이 자신의 io_u 각각에 대해 채움.
	 * 읽는 자: 클라이언트 client_recv()가 복사 후 rd->rmt_us[]로 전개.
	 * 동기화: 1회 교환 후 불변. */
};

/* [한국어] RDMA I/O 유닛 확장 데이터 — io_u당 1개. io_u->engine_data에 매달려 WR 템플릿을 보관.
 * 인스턴스는 fio_rdmaio_post_init에서 calloc로 할당되며, 잡 전 생명주기 동안 재사용된다. */
struct rdma_io_u_data {
	uint64_t wr_id;
	/* [한국어] 이 io_u의 고유 식별자 (0..iodepth-1). sq_wr/rq_wr.wr_id에 복사되어
	 * 완료 시 CQE의 wc.wr_id로 역 추적된다.
	 * 설정자: fio_rdmaio_post_init에서 freelist 인덱스 i로 초기화.
	 * 읽는 자: fio_rdmaio_prep에서 sq/rq_wr.wr_id에 전파, cq_event_handler에서 매칭에 사용.
	 * 값 범위: 0..iodepth-1. FIO_RDMA_MAX_IO_DEPTH와 충돌하지 않음(그 값은 제어 메시지 전용).
	 * 동기화: io_u는 단일 잡 스레드에서만 다뤄지므로 락 불필요. */

	struct ibv_send_wr sq_wr;
	/* [한국어] send/WRITE/READ용 Work Request 템플릿. opcode·send_flags·sg_list·rdma.* 를
	 * 매 I/O마다 fio_rdmaio_prep/send가 채워 ibv_post_send로 제출.
	 * 설정자: fio_rdmaio_prep(공통 필드), fio_rdmaio_send(opcode·rkey·remote_addr·length).
	 * 읽는 자: libibverbs 내부 → HCA 드라이버. */

	struct ibv_recv_wr rq_wr;
	/* [한국어] recv용 Work Request 템플릿. CHA_RECV에서 사용.
	 * 설정자: fio_rdmaio_prep가 wr_id·sg_list·num_sge 설정.
	 * 읽는 자: ibv_post_recv 경로. */

	struct ibv_sge rdma_sgl;
	/* [한국어] Scatter/Gather Entry — 로컬 io_u->buf의 (addr, length, lkey) 정보.
	 * 설정자: fio_rdmaio_prep에서 io_u->buf, io_u->mr->lkey로 초기화. fio_rdmaio_send에서
	 * 매 I/O마다 length = io_us[i]->buflen으로 갱신(WRITE/READ는 prep에서 lkey만, length는 send에서).
	 * 읽는 자: HCA가 로컬 DMA 소스/목적지로 사용. */
};

/* [한국어] RDMA 엔진 인스턴스 상태 — td->io_ops_data에 매달려 잡 전역 상태를 보관.
 * 생성자: fio_rdmaio_setup(calloc). 해제자: fio_rdmaio_cleanup(free). 한 잡 스레드 전용. */
struct rdmaio_data {
	int is_client;
	/* [한국어] 역할 플래그: 1=클라이언트(WRITE 잡), 0=서버(READ 잡).
	 * 설정자: fio_rdmaio_init에서 td_read(td) 판정으로 결정.
	 * 읽는 자: 대부분의 콜백이 송·수신 분기에 사용. */

	enum rdma_io_mode rdma_protocol;
	/* [한국어] 최종 선택된 RDMA 모드. 서버는 클라이언트의 제어 메시지로 덮어쓸 수 있다
	 * (server_recv가 CHA_SEND → CHA_RECV로 자동 치환).
	 * 설정자: fio_rdmaio_init(사용자 옵션), server_recv(수신 후 조정).
	 * 읽는 자: prep/send/recv/getevents 등 거의 모든 콜백. */

	char host[64];
	/* [한국어] 원격 호스트명 저장 버퍼 — 현재 구현에서는 직접 채우지 않고 filename을 사용하지만
	 * 구조체에는 예약되어 있다(향후 확장 용). */

	struct sockaddr_in addr;
	/* [한국어] 연결 대상/바인딩 주소(IPv4). fio_rdmaio_setup_connect/setup_listen이 채움.
	 * 읽는 자: rdma_resolve_addr / rdma_bind_addr. */

	struct ibv_recv_wr rq_wr;
	/* [한국어] 제어 메시지 수신용 WR(엔진 레벨, io_u와 무관). wr_id=FIO_RDMA_MAX_IO_DEPTH로
	 * 센티널 표시되어 cq_event_handler가 일반 io_u와 구분. */

	struct ibv_sge recv_sgl;
	/* [한국어] recv WR의 SGE — recv_buf에 대한 (addr,len,lkey). */

	struct rdma_info_blk recv_buf;
	/* [한국어] 수신된 제어 메시지 버퍼. ibv_reg_mr로 HCA가 DMA할 수 있게 등록됨. */

	struct ibv_mr *recv_mr;
	/* [한국어] recv_buf에 대한 Memory Region. dereg 시점은 연결 종료(cleanup 경로). */

	struct ibv_send_wr sq_wr;
	/* [한국어] 제어 메시지 송신용 WR (io_u와 무관). wr_id=FIO_RDMA_MAX_IO_DEPTH. */

	struct ibv_sge send_sgl;
	/* [한국어] send WR의 SGE — send_buf에 대한 (addr,len,lkey). */

	struct rdma_info_blk send_buf;
	/* [한국어] 송신할 제어 메시지 버퍼(mode/nr/max_bs/rmt_us 채움). */

	struct ibv_mr *send_mr;
	/* [한국어] send_buf MR. */

	struct ibv_comp_channel *channel;
	/* [한국어] CQ 완료 이벤트 채널. ibv_get_cq_event로 완료 알림을 수신.
	 * 이벤트 드리븐 완료 처리의 핵심(폴링 대신 대기). */

	struct ibv_cq *cq;
	/* [한국어] 송·수신 공용 Completion Queue. QP의 send_cq/recv_cq 모두 여기로 통합.
	 * 깊이: iodepth*2 (최소 16). ibv_poll_cq로 CQE를 수확. */

	struct ibv_pd *pd;
	/* [한국어] Protection Domain — 이 도메인 내 모든 MR/QP가 상호 접근 허용.
	 * 설정자: fio_rdmaio_setup_qp의 ibv_alloc_pd. */

	struct ibv_qp *qp;
	/* [한국어] Queue Pair(send+recv) — 실제 WR을 포스트할 대상. rdma_create_qp로 생성 후
	 * cm_id 또는 child_cm_id에 바인딩. */

	pthread_t cmthread;
	/* [한국어] CM 이벤트 전용 pthread 핸들 — 현 코드에서는 실제로 생성하지 않으나
	 * 구조체에 예약되어 있음(비동기 CM 이벤트 처리 확장 대비). */

	struct rdma_event_channel *cm_channel;
	/* [한국어] RDMA CM 이벤트 채널 — rdma_get_cm_event로 CONNECT_REQUEST/ESTABLISHED/
	 * ADDR_RESOLVED 등 생명주기 이벤트를 수신. */

	struct rdma_cm_id *cm_id;
	/* [한국어] 기본 CM 식별자. 클라이언트는 이것으로 connect, 서버는 listen. */

	struct rdma_cm_id *child_cm_id;
	/* [한국어] 서버 측에서 accept로 파생된 자식 CM id — 실제 연결된 QP는 여기에 붙는다. */

	int cq_event_num;
	/* [한국어] 이미 수신한 CQE 중 아직 fio에 보고하지 않은 개수의 카운터.
	 * getevents/rdma_poll_wait가 "previous left"로 이 값을 먼저 소비해 중복 대기를 방지. */

	struct remote_u *rmt_us;
	/* [한국어] 클라이언트가 수신한 서버의 원격 MR 배열. calloc로 MAX_IO_DEPTH 크기 예약. */

	int rmt_nr;
	/* [한국어] rmt_us[]의 유효 엔트리 수. WRITE/READ 시 __rand % rmt_nr로 인덱스 선택. */

	struct io_u **io_us_queued;
	/* [한국어] fio_rdmaio_queue로 쌓인 io_u들의 배열(아직 WR 포스트 전). commit에서 일괄 제출. */

	int io_u_queued_nr;
	/* [한국어] io_us_queued 유효 개수. iodepth 도달 시 queue는 FIO_Q_BUSY 반환. */

	struct io_u **io_us_flight;
	/* [한국어] ibv_post_send/recv로 HCA에 전달되어 완료 대기 중인 io_u 배열. */

	int io_u_flight_nr;
	/* [한국어] io_us_flight 유효 개수. CQE 도착 시 감소. */

	struct io_u **io_us_completed;
	/* [한국어] 완료된 io_u 배열. fio_rdmaio_event가 head부터 1개씩 fio에 반환. */

	int io_u_completed_nr;
	/* [한국어] io_us_completed 유효 개수. */

	struct frand_state rand_state;
	/* [한국어] 원격 MR 인덱스 무작위 선택용 난수 상태(GOLDEN_RATIO_64 시드).
	 * 설정자: fio_rdmaio_setup의 init_rand_seed. 읽는 자: fio_rdmaio_send의 __rand(). */
};

/*
 * [한국어]
 * client_recv - 클라이언트가 서버로부터 받은 제어 메시지를 처리.
 *
 * @td: 이 잡의 thread_data.
 * @wc: CQE 하나(IBV_WC_RECV로 완료한 제어 메시지 수신).
 * @return: 0=성공, 1=오류(크기 검증 실패 또는 블록 크기 불일치).
 *
 * 동기: 서버는 클라이언트의 첫 SEND에 응답하여 자신의 원격 MR 정보(rkey, addr, size)를
 * rdma_info_blk에 담아 돌려준다. 이 함수는 그 메시지를 파싱해 rd->rmt_us[]에 전개한다.
 * 호출 체인: cq_event_handler → [이 함수] (is_client==1인 IBV_WC_RECV 분기).
 * 실행 컨텍스트: 잡 스레드. 에러 시 caller가 -1 전파.
 */
static int client_recv(struct thread_data *td, struct ibv_wc *wc)
{
	/* [한국어] 엔진 상태 접근 단축 포인터 */
	struct rdmaio_data *rd = td->io_ops_data;
	/* [한국어] 사용자가 설정한 최대 블록 크기(read/write 중 큰 값) */
	unsigned int max_bs;

	/* [한국어] 수신 바이트 길이가 정확히 rdma_info_blk 크기가 아니면 프로토콜 위반으로 간주 */
	if (wc->byte_len != sizeof(rd->recv_buf)) {
		log_err("Received bogus data, size %d\n", wc->byte_len);
		return 1;
	}

	/* [한국어] 클라이언트의 max_bs 계산(READ/WRITE 중 더 큰 값) */
	max_bs = max(td->o.max_bs[DDIR_READ], td->o.max_bs[DDIR_WRITE]);
	/* [한국어] 서버의 max_bs보다 클라이언트가 더 크면, 서버 측 pin 메모리 부족 위험 → 오류 */
	if (max_bs > ntohl(rd->recv_buf.max_bs)) {
		log_err("fio: Server's block size (%d) must be greater than or "
			"equal to the client's block size (%d)!\n",
			ntohl(rd->recv_buf.max_bs), max_bs);
		return 1;
	}

	/* store mr info for MEMORY semantic */
	/* [한국어] RDMA WRITE/READ에서만 원격 MR 정보가 의미 있음. SEND/RECV는 원격 주소 불필요. */
	if ((rd->rdma_protocol == FIO_RDMA_MEM_WRITE) ||
	    (rd->rdma_protocol == FIO_RDMA_MEM_READ)) {
		/* struct flist_head *entry; */
		/* [한국어] 루프 인덱스 — 원격 MR 엔트리 순회 */
		int i = 0;

		/* [한국어] 서버가 알려준 유효 MR 수를 호스트 바이트순으로 복원 */
		rd->rmt_nr = ntohl(rd->recv_buf.nr);

		for (i = 0; i < rd->rmt_nr; i++) {
			/* [한국어] 64비트 네트워크 → 호스트 순서 변환 (원격 VA) */
			rd->rmt_us[i].buf = __be64_to_cpu(
						rd->recv_buf.rmt_us[i].buf);
			/* [한국어] rkey/size는 32비트 ntohl */
			rd->rmt_us[i].rkey = ntohl(rd->recv_buf.rmt_us[i].rkey);
			rd->rmt_us[i].size = ntohl(rd->recv_buf.rmt_us[i].size);

			/* [한국어] FD_IO 채널 디버그 로그로 각 원격 MR 등록 기록 */
			dprint(FD_IO,
			       "fio: Received rkey %x addr %" PRIx64
			       " len %d from peer\n", rd->rmt_us[i].rkey,
			       rd->rmt_us[i].buf, rd->rmt_us[i].size);
		}
	}

	return 0;
}

/*
 * [한국어]
 * server_recv - 서버가 클라이언트로부터 받은 제어 메시지를 처리.
 *
 * @td: thread_data.
 * @wc: CQE 하나.
 * @return: 0 성공, 1 블록 크기 검증 실패.
 *
 * 동기: 서버는 클라이언트가 어떤 모드(WRITE/READ/SEND/RECV)로 동작하고자 하는지를
 * 첫 제어 메시지로 받고, 자신의 rdma_protocol을 거기에 맞춘다. 채널 시맨틱의 경우
 * 클라이언트가 SEND면 서버는 RECV 역할로 바뀐다.
 * 호출 체인: cq_event_handler → [이 함수] (is_client==0인 IBV_WC_RECV).
 */
static int server_recv(struct thread_data *td, struct ibv_wc *wc)
{
	struct rdmaio_data *rd = td->io_ops_data;
	unsigned int max_bs;

	/* [한국어] wr_id == MAX_IO_DEPTH는 제어 메시지 수신 WR(rd->rq_wr)임을 의미.
	 * 일반 io_u의 wr_id(0..iodepth-1)와 구분됨. */
	if (wc->wr_id == FIO_RDMA_MAX_IO_DEPTH) {
		/* [한국어] 클라이언트가 선택한 모드를 수신 */
		rd->rdma_protocol = ntohl(rd->recv_buf.mode);

		/* CHANNEL semantic, do nothing */
		/* [한국어] 클라이언트가 CHA_SEND면 서버는 RECV 측이 되어야 한다 */
		if (rd->rdma_protocol == FIO_RDMA_CHA_SEND)
			rd->rdma_protocol = FIO_RDMA_CHA_RECV;

		/* [한국어] 서버 측 max_bs 산출 */
		max_bs = max(td->o.max_bs[DDIR_READ], td->o.max_bs[DDIR_WRITE]);
		/* [한국어] 서버의 max_bs가 클라이언트보다 작으면 buffer overflow 위험 → 오류 */
		if (max_bs < ntohl(rd->recv_buf.max_bs)) {
			log_err("fio: Server's block size (%d) must be greater than or "
				"equal to the client's block size (%d)!\n",
				ntohl(rd->recv_buf.max_bs), max_bs);
			return 1;
		}

	}

	return 0;
}

/*
 * [한국어]
 * cq_event_handler - CQ에서 CQE를 폴링해 io_u 상태 전이를 수행.
 *
 * @td:     thread_data.
 * @opcode: 기대 opcode(검증용으로는 실제 사용되지 않음, 향후 확장 예약).
 * @return: 처리한 CQE 개수(>=0), 오류 시 -1/1.
 *
 * 동기: ibv_poll_cq는 CQ의 남은 모든 CQE를 non-blocking으로 수확한다. 각 CQE에 대해
 *  - IBV_WC_RECV(수신 완료): 클라이언트/서버 제어 메시지 처리 또는 일반 RECV io_u 완료 처리
 *  - IBV_WC_SEND/RDMA_WRITE/RDMA_READ(송신 완료): flight → completed 전이
 * wr_id == FIO_RDMA_MAX_IO_DEPTH는 제어 메시지 전용 WR이므로 io_u 추적에서 제외.
 * 호출 체인: rdma_poll_wait / fio_rdmaio_getevents → [이 함수].
 * 에러: 상태 비정상 또는 미지 opcode → -1, 호출자가 전파.
 */
static int cq_event_handler(struct thread_data *td, enum ibv_wc_opcode opcode)
{
	struct rdmaio_data *rd = td->io_ops_data;
	/* [한국어] 단일 CQE 수확용 스택 버퍼 */
	struct ibv_wc wc;
	struct rdma_io_u_data *r_io_u_d;
	int ret;
	/* [한국어] 이번 호출에서 처리한 완료 이벤트 개수 누적 */
	int compevnum = 0;
	int i;

	/* [한국어] ibv_poll_cq는 최대 n개까지 수확. 여기선 1개씩 루프. 반환 1=수확함, 0=없음. */
	while ((ret = ibv_poll_cq(rd->cq, 1, &wc)) == 1) {
		compevnum++;

		/* [한국어] CQE의 status가 0(IBV_WC_SUCCESS)이 아니면 전송 실패 */
		if (wc.status) {
			log_err("fio: cq completion status %d(%s)\n",
				wc.status, ibv_wc_status_str(wc.status));
			return -1;
		}

		/* [한국어] 완료 opcode로 분기 — 수신/송신/RDMA 읽기/쓰기 */
		switch (wc.opcode) {

		case IBV_WC_RECV:
			/* [한국어] 역할에 따라 제어 메시지 파서 선택 */
			if (rd->is_client == 1)
				ret = client_recv(td, &wc);
			else
				ret = server_recv(td, &wc);

			/* [한국어] 파싱 실패면 전체 포기 */
			if (ret)
				return -1;

			/* [한국어] 제어 메시지 WR이면 io_u 추적과 무관하므로 종료 */
			if (wc.wr_id == FIO_RDMA_MAX_IO_DEPTH)
				break;

			/* [한국어] 일반 RECV io_u 완료: flight 배열에서 wr_id로 대응 io_u 탐색 */
			for (i = 0; i < rd->io_u_flight_nr; i++) {
				r_io_u_d = rd->io_us_flight[i]->engine_data;

				if (wc.wr_id == r_io_u_d->rq_wr.wr_id) {
					/* [한국어] resid = 요청 길이 - 실제 수신 길이. 0이면 full fill */
					rd->io_us_flight[i]->resid =
					    rd->io_us_flight[i]->buflen
					    - wc.byte_len;

					rd->io_us_flight[i]->error = 0;

					/* [한국어] flight → completed 배열 끝에 추가 */
					rd->io_us_completed[rd->
							    io_u_completed_nr]
					    = rd->io_us_flight[i];
					rd->io_u_completed_nr++;
					break;
				}
			}
			/* [한국어] 매칭 실패는 프로토콜/로직 오류 */
			if (i == rd->io_u_flight_nr)
				log_err("fio: recv wr %" PRId64 " not found\n",
					wc.wr_id);
			else {
				/* put the last one into middle of the list */
				/* [한국어] flight 배열에서 해당 슬롯을 마지막 요소로 swap-remove */
				rd->io_us_flight[i] =
				    rd->io_us_flight[rd->io_u_flight_nr - 1];
				rd->io_u_flight_nr--;
			}

			break;

		case IBV_WC_SEND:
		case IBV_WC_RDMA_WRITE:
		case IBV_WC_RDMA_READ:
			/* [한국어] 제어 메시지 송신 완료는 io_u 추적 대상 아님 */
			if (wc.wr_id == FIO_RDMA_MAX_IO_DEPTH)
				break;

			/* [한국어] send 측 WR의 wr_id로 io_u 역추적 (위 RECV 경로와 대칭) */
			for (i = 0; i < rd->io_u_flight_nr; i++) {
				r_io_u_d = rd->io_us_flight[i]->engine_data;

				if (wc.wr_id == r_io_u_d->sq_wr.wr_id) {
					rd->io_us_completed[rd->
							    io_u_completed_nr]
					    = rd->io_us_flight[i];
					rd->io_u_completed_nr++;
					break;
				}
			}
			if (i == rd->io_u_flight_nr)
				log_err("fio: send wr %" PRId64 " not found\n",
					wc.wr_id);
			else {
				/* put the last one into middle of the list */
				/* [한국어] flight에서 swap-remove */
				rd->io_us_flight[i] =
				    rd->io_us_flight[rd->io_u_flight_nr - 1];
				rd->io_u_flight_nr--;
			}

			break;

		default:
			/* [한국어] 예상 밖 opcode — 버전 불일치 또는 새 verb 추가 */
			log_info("fio: unknown completion event %d\n",
				 wc.opcode);
			return -1;
		}
		/* [한국어] 처리 성공한 CQE를 전체 카운터에 반영 */
		rd->cq_event_num++;
	}

	/* [한국어] ibv_poll_cq가 음수 반환 = 오류 */
	if (ret) {
		log_err("fio: poll error %d\n", ret);
		return 1;
	}

	return compevnum;
}

/*
 * Return -1 for error and 'nr events' for a positive number
 * of events
 */
/*
 * [한국어]
 * rdma_poll_wait - CQ 이벤트 채널에서 알림을 기다렸다가 CQE를 처리.
 *
 * @td:     thread_data.
 * @opcode: 기대 opcode (cq_event_handler 전달용, 내부 검증에는 사용하지 않음).
 * @return: 처리한 이벤트 수(>0), 남은 이전 이벤트 재사용 시 0, 오류 시 -1.
 *
 * 동기: ibv_get_cq_event는 comp_channel에 완료 알림이 올 때까지 블로킹 대기한다.
 * 호출 직후 반드시 ibv_req_notify_cq로 다음 알림을 재등록해야 CQE 누락이 없다.
 * 내부적으로 cq_event_handler를 호출해 실제 CQE를 수확하고, 이벤트 0개면 다시 대기한다.
 * ibv_ack_cq_events는 libibverbs 내부 참조 카운트 관리용(누적된 ack를 한꺼번에 처리).
 */
static int rdma_poll_wait(struct thread_data *td, enum ibv_wc_opcode opcode)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;

	/* [한국어] 이전 getevents/poll_wait에서 남긴 이벤트가 있으면 그것부터 소비 */
	if (rd->cq_event_num > 0) {	/* previous left */
		rd->cq_event_num--;
		return 0;
	}

again:
	/* [한국어] 블로킹 대기 — comp_channel의 fd에서 알림 수신 */
	if (ibv_get_cq_event(rd->channel, &ev_cq, &ev_ctx) != 0) {
		log_err("fio: Failed to get cq event!\n");
		return -1;
	}
	/* [한국어] 알림이 온 CQ가 우리 CQ인지 검증(다중 CQ 환경 방어) */
	if (ev_cq != rd->cq) {
		log_err("fio: Unknown CQ!\n");
		return -1;
	}
	/* [한국어] 다음 CQE 알림을 다시 요청(edge-triggered 재등록) */
	if (ibv_req_notify_cq(rd->cq, 0) != 0) {
		log_err("fio: Failed to set notify!\n");
		return -1;
	}

	/* [한국어] 실제 CQE 수확 및 io_u 상태 전이 */
	ret = cq_event_handler(td, opcode);
	/* [한국어] 이벤트 0개면 spurious wakeup — 다시 대기 */
	if (ret == 0)
		goto again;

	/* [한국어] libibverbs에 수확한 이벤트 ack(참조카운트 감소) */
	ibv_ack_cq_events(rd->cq, ret);

	/* [한국어] 자체 카운터 — getevents에서 중복 계산 방지를 위해 1 감소 */
	rd->cq_event_num--;

	return ret;
}

/*
 * [한국어]
 * fio_rdmaio_setup_qp - Protection Domain / Completion Channel / CQ / QP를 생성.
 *
 * @td: thread_data.
 * @return: 0 성공, 1 실패(단계별 err 레이블로 부분 롤백).
 *
 * 동기: RDMA 데이터 전송의 기본 단위인 QP(Queue Pair)를 생성하기 위해 필요한
 * 모든 컨텍스트(PD, comp_channel, CQ)를 만든다. QP 깊이는 iodepth*2(최소 16)로 여유 있게 잡음.
 * QP 타입은 IBV_QPT_RC (Reliable Connection) — 순서 보장·무손실.
 * 서버는 child_cm_id의 verbs를, 클라이언트는 cm_id의 verbs를 사용한다(연결 맥락 차이).
 * 호출 체인: setup_connect / setup_listen → [이 함수].
 */
static int fio_rdmaio_setup_qp(struct thread_data *td)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct ibv_qp_init_attr init_attr;
	int qp_depth = td->o.iodepth * 2;	/* 2 times of io depth */
	/* [한국어] 송수신 WR이 iodepth만큼 쌓일 수 있으므로 여유 있게 2배로 지정 */

	/* [한국어] 서버면 child_cm_id(수락된 연결)의 verbs 디바이스를, 아니면 cm_id의 것을 사용 */
	if (rd->is_client == 0)
		rd->pd = ibv_alloc_pd(rd->child_cm_id->verbs);
	else
		rd->pd = ibv_alloc_pd(rd->cm_id->verbs);

	/* [한국어] PD 할당 실패 — 커널 RDMA 리소스 부족 등 */
	if (rd->pd == NULL) {
		log_err("fio: ibv_alloc_pd fail: %m\n");
		return 1;
	}

	/* [한국어] Completion Channel 생성 — CQ 알림을 fd 기반으로 받기 위한 구조 */
	if (rd->is_client == 0)
		rd->channel = ibv_create_comp_channel(rd->child_cm_id->verbs);
	else
		rd->channel = ibv_create_comp_channel(rd->cm_id->verbs);
	if (rd->channel == NULL) {
		log_err("fio: ibv_create_comp_channel fail: %m\n");
		goto err1;
	}

	/* [한국어] 최소 깊이 보장(iodepth<8인 작은 잡에서도 16 이상 확보) */
	if (qp_depth < 16)
		qp_depth = 16;

	/* [한국어] CQ 생성 — cqe 수 qp_depth, cq_context=rd, comp_channel=channel, comp_vector=0 */
	if (rd->is_client == 0)
		rd->cq = ibv_create_cq(rd->child_cm_id->verbs,
				       qp_depth, rd, rd->channel, 0);
	else
		rd->cq = ibv_create_cq(rd->cm_id->verbs,
				       qp_depth, rd, rd->channel, 0);
	if (rd->cq == NULL) {
		log_err("fio: ibv_create_cq failed: %m\n");
		goto err2;
	}

	/* [한국어] 첫 CQE 알림을 활성화 — 이후 poll 루프에서 매 이벤트마다 재등록 */
	if (ibv_req_notify_cq(rd->cq, 0) != 0) {
		log_err("fio: ibv_req_notify_cq failed: %m\n");
		goto err3;
	}

	/* create queue pair */
	/* [한국어] QP 초기 속성 구성 — 송수신 WR/SGE 상한, RC 타입, send_cq/recv_cq 지정 */
	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = qp_depth;
	init_attr.cap.max_recv_wr = qp_depth;
	init_attr.cap.max_recv_sge = 1;    /* [한국어] 이 엔진은 각 WR당 SGE 1개만 사용 */
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;    /* [한국어] Reliable Connection: 순서·무손실 보장 */
	init_attr.send_cq = rd->cq;
	init_attr.recv_cq = rd->cq;

	/* [한국어] rdma_cm에 위임하여 QP 생성 — CM이 적절한 state 전이를 대신 수행 */
	if (rd->is_client == 0) {
		if (rdma_create_qp(rd->child_cm_id, rd->pd, &init_attr) != 0) {
			log_err("fio: rdma_create_qp failed: %m\n");
			goto err3;
		}
		rd->qp = rd->child_cm_id->qp;
	} else {
		if (rdma_create_qp(rd->cm_id, rd->pd, &init_attr) != 0) {
			log_err("fio: rdma_create_qp failed: %m\n");
			goto err3;
		}
		rd->qp = rd->cm_id->qp;
	}

	return 0;

	/* [한국어] 단계별 역롤백 레이블 — 각 단계에서 실패 시 이전에 성공한 자원을 해제 */
err3:
	ibv_destroy_cq(rd->cq);
err2:
	ibv_destroy_comp_channel(rd->channel);
err1:
	ibv_dealloc_pd(rd->pd);

	return 1;
}

/*
 * [한국어]
 * fio_rdmaio_setup_control_msg_buffers - 제어 메시지용 송/수신 버퍼를 MR로 등록하고 WR 템플릿 초기화.
 *
 * @td: thread_data.
 * @return: 0 성공, 1 실패.
 *
 * 동기: 제어 메시지는 일반 io_u와 별도의 버퍼(recv_buf/send_buf)를 쓴다. HCA가 DMA하려면
 * 반드시 ibv_reg_mr로 pin·등록해야 한다. recv_mr은 로컬 쓰기 권한만, send_mr은 권한 없음(=로컬 읽기만).
 * WR의 wr_id = FIO_RDMA_MAX_IO_DEPTH로 센티널 지정(일반 io_u와 구분).
 */
static int fio_rdmaio_setup_control_msg_buffers(struct thread_data *td)
{
	struct rdmaio_data *rd = td->io_ops_data;

	/* [한국어] 수신 버퍼 MR — 원격 HCA의 SEND가 로컬 메모리에 DMA 쓰기하므로 LOCAL_WRITE 필요 */
	rd->recv_mr = ibv_reg_mr(rd->pd, &rd->recv_buf, sizeof(rd->recv_buf),
				 IBV_ACCESS_LOCAL_WRITE);
	if (rd->recv_mr == NULL) {
		log_err("fio: recv_buf reg_mr failed: %m\n");
		return 1;
	}

	/* [한국어] 송신 버퍼 MR — 로컬 HCA가 DMA 읽기만 하므로 권한 0 */
	rd->send_mr = ibv_reg_mr(rd->pd, &rd->send_buf, sizeof(rd->send_buf),
				 0);
	if (rd->send_mr == NULL) {
		log_err("fio: send_buf reg_mr failed: %m\n");
		ibv_dereg_mr(rd->recv_mr);  /* [한국어] 실패 시 이미 등록한 recv_mr 롤백 */
		return 1;
	}

	/* setup work request */
	/* recv wq */
	/* [한국어] 수신 WR의 SGE — recv_buf 시작 주소/크기/lkey 설정 */
	rd->recv_sgl.addr = (uint64_t) (unsigned long)&rd->recv_buf;
	rd->recv_sgl.length = sizeof(rd->recv_buf);
	rd->recv_sgl.lkey = rd->recv_mr->lkey;
	rd->rq_wr.sg_list = &rd->recv_sgl;
	rd->rq_wr.num_sge = 1;
	rd->rq_wr.wr_id = FIO_RDMA_MAX_IO_DEPTH;  /* [한국어] 제어 메시지 전용 센티널 wr_id */

	/* send wq */
	/* [한국어] 송신 WR의 SGE/WR 초기화 */
	rd->send_sgl.addr = (uint64_t) (unsigned long)&rd->send_buf;
	rd->send_sgl.length = sizeof(rd->send_buf);
	rd->send_sgl.lkey = rd->send_mr->lkey;

	rd->sq_wr.opcode = IBV_WR_SEND;       /* [한국어] 채널 SEND 연산 */
	rd->sq_wr.send_flags = IBV_SEND_SIGNALED; /* [한국어] 완료 시 CQE 생성 요청 */
	rd->sq_wr.sg_list = &rd->send_sgl;
	rd->sq_wr.num_sge = 1;
	rd->sq_wr.wr_id = FIO_RDMA_MAX_IO_DEPTH;

	return 0;
}

/*
 * [한국어]
 * get_next_channel_event - CM 이벤트 채널에서 특정 이벤트를 동기로 대기.
 *
 * @td:         thread_data.
 * @channel:    rdma_event_channel (rd->cm_channel).
 * @wait_event: 기대 이벤트 타입(RDMA_CM_EVENT_ESTABLISHED 등).
 * @return: 0 성공(기대 이벤트 수신), 1 실패.
 *
 * 동기: rdma_connect/accept/resolve_addr/resolve_route는 비동기이며 CM 이벤트로 완료를 통지한다.
 * 이 함수는 그 동기화 래퍼 — 해당 이벤트가 올 때까지 블로킹하고, 이벤트가 예상과 다르면 오류 반환.
 * CONNECT_REQUEST 수신 시 event->id를 child_cm_id에 저장해 이후 accept 경로에서 사용.
 */
static int get_next_channel_event(struct thread_data *td,
				  struct rdma_event_channel *channel,
				  enum rdma_cm_event_type wait_event)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdma_cm_event *event;
	int ret;

	/* [한국어] 이벤트 블로킹 수신 */
	ret = rdma_get_cm_event(channel, &event);
	if (ret) {
		log_err("fio: rdma_get_cm_event: %d\n", ret);
		return 1;
	}

	/* [한국어] 예상과 다른 이벤트면 상태 머신 불일치 — 오류 보고 */
	if (event->event != wait_event) {
		log_err("fio: event is %s instead of %s\n",
			rdma_event_str(event->event),
			rdma_event_str(wait_event));
		return 1;
	}

	/* [한국어] CONNECT_REQUEST는 서버 측에 전달되며 event->id가 새 자식 cm_id */
	switch (event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		rd->child_cm_id = event->id;
		break;
	default:
		break;
	}

	/* [한국어] 이벤트 처리 완료를 CM에 알림 — 반드시 호출해야 리소스 누수 없음 */
	rdma_ack_cm_event(event);

	return 0;
}

/*
 * [한국어]
 * fio_rdmaio_prep - io_u 하나에 대해 WR 템플릿의 공통 필드를 채움.
 *
 * @td:   thread_data.
 * @io_u: 준비할 I/O 유닛 (fio 코어가 할당).
 * @return: 0 고정(현재 구현은 실패 경로 없음).
 *
 * 동기: fio의 td_io_prep 콜백 — io_u를 queue에 넣기 전 엔진이 준비 작업을 수행.
 * 여기서는 WR의 sg_list(주소·lkey), wr_id, 공통 플래그를 초기화한다. opcode·length는
 * send/recv 시점에서 최종 확정(io_u->buflen이 fio에 의해 달라질 수 있기 때문).
 * 호출 체인: backend.c → td_io_prep → [이 함수].
 */
static int fio_rdmaio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdma_io_u_data *r_io_u_d;

	/* [한국어] io_u에 매달린 엔진 전용 확장 데이터 접근 */
	r_io_u_d = io_u->engine_data;

	/* [한국어] 모드별로 채워야 할 WR 필드가 다름(send 계열은 sq_wr, recv 계열은 rq_wr) */
	switch (rd->rdma_protocol) {
	case FIO_RDMA_MEM_WRITE:
	case FIO_RDMA_MEM_READ:
		/* [한국어] 로컬 버퍼 주소와 lkey 등록 — 원격 주소/rkey는 send 시점에 주입 */
		r_io_u_d->rdma_sgl.addr = (uint64_t) (unsigned long)io_u->buf;
		r_io_u_d->rdma_sgl.lkey = io_u->mr->lkey;
		r_io_u_d->sq_wr.wr_id = r_io_u_d->wr_id;
		r_io_u_d->sq_wr.send_flags = IBV_SEND_SIGNALED;
		r_io_u_d->sq_wr.sg_list = &r_io_u_d->rdma_sgl;
		r_io_u_d->sq_wr.num_sge = 1;
		break;
	case FIO_RDMA_CHA_SEND:
		/* [한국어] 채널 SEND — length도 여기서 확정(io_u->buflen은 prep 시점에 알려짐) */
		r_io_u_d->rdma_sgl.addr = (uint64_t) (unsigned long)io_u->buf;
		r_io_u_d->rdma_sgl.lkey = io_u->mr->lkey;
		r_io_u_d->rdma_sgl.length = io_u->buflen;
		r_io_u_d->sq_wr.wr_id = r_io_u_d->wr_id;
		r_io_u_d->sq_wr.opcode = IBV_WR_SEND;
		r_io_u_d->sq_wr.send_flags = IBV_SEND_SIGNALED;
		r_io_u_d->sq_wr.sg_list = &r_io_u_d->rdma_sgl;
		r_io_u_d->sq_wr.num_sge = 1;
		break;
	case FIO_RDMA_CHA_RECV:
		/* [한국어] 채널 RECV — recv WR 준비(수신용). send_flags·opcode 없음 */
		r_io_u_d->rdma_sgl.addr = (uint64_t) (unsigned long)io_u->buf;
		r_io_u_d->rdma_sgl.lkey = io_u->mr->lkey;
		r_io_u_d->rdma_sgl.length = io_u->buflen;
		r_io_u_d->rq_wr.wr_id = r_io_u_d->wr_id;
		r_io_u_d->rq_wr.sg_list = &r_io_u_d->rdma_sgl;
		r_io_u_d->rq_wr.num_sge = 1;
		break;
	default:
		log_err("fio: unknown rdma protocol - %d\n", rd->rdma_protocol);
		break;
	}

	return 0;
}

/*
 * [한국어]
 * fio_rdmaio_event - 완료된 io_u를 하나씩 꺼내 fio에 반환.
 *
 * @td:    thread_data.
 * @event: 이벤트 인덱스(미사용 — 이 엔진은 내부 FIFO 큐에서 앞에서 꺼냄).
 * @return: 완료된 io_u 포인터.
 *
 * 동기: fio_rdmaio_getevents가 완료 수를 반환한 뒤, fio 코어는 그 수만큼 이 콜백을 호출해
 * 완료된 io_u를 하나씩 받아간다. 내부적으로 io_us_completed[] 배열을 shift로 소비.
 * 호출 체인: backend.c → td_io_event → [이 함수].
 */
static struct io_u *fio_rdmaio_event(struct thread_data *td, int event)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct io_u *io_u;
	int i;

	/* [한국어] 배열 선두(가장 먼저 완료된 io_u) 추출 */
	io_u = rd->io_us_completed[0];
	/* [한국어] 뒤따르는 요소들을 한 칸씩 앞으로 shift (FIFO 유지) */
	for (i = 0; i < rd->io_u_completed_nr - 1; i++)
		rd->io_us_completed[i] = rd->io_us_completed[i + 1];

	rd->io_u_completed_nr--;

	/* [한국어] 디버그 로그에 완료 io_u 정보 출력 */
	dprint_io_u(io_u, "fio_rdmaio_event");

	return io_u;
}

/*
 * [한국어]
 * fio_rdmaio_getevents - 최소 min 개의 완료를 기다렸다가 개수를 반환.
 *
 * @td:  thread_data.
 * @min: 최소 대기 이벤트 수.
 * @max: 최대 이벤트 수(이 엔진은 엄밀하게 상한 적용하지 않음).
 * @t:   타임아웃(미사용).
 * @return: 수집한 이벤트 수(>=min), 오류 시 -1.
 *
 * 동기: rdma_protocol에 따라 기대 opcode 결정 → ibv_get_cq_event 블로킹 대기 →
 * cq_event_handler가 0 반환하면 다시 대기 → 누적이 min 이상이면 반환.
 * rd->cq_event_num으로 이미 수신된 잔여 완료를 먼저 소비하여 중복 대기 회피.
 * 호출 체인: backend.c → td_io_getevents → [이 함수].
 */
static int fio_rdmaio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct rdmaio_data *rd = td->io_ops_data;
	enum ibv_wc_opcode comp_opcode;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret, r = 0;
	comp_opcode = IBV_WC_RDMA_WRITE;  /* [한국어] 기본값 — 아래 switch로 덮어씀 */

	/* [한국어] 모드별 기대 opcode 결정(CQE 필터링용은 아니고 호출 의도 표시) */
	switch (rd->rdma_protocol) {
	case FIO_RDMA_MEM_WRITE:
		comp_opcode = IBV_WC_RDMA_WRITE;
		break;
	case FIO_RDMA_MEM_READ:
		comp_opcode = IBV_WC_RDMA_READ;
		break;
	case FIO_RDMA_CHA_SEND:
		comp_opcode = IBV_WC_SEND;
		break;
	case FIO_RDMA_CHA_RECV:
		comp_opcode = IBV_WC_RECV;
		break;
	default:
		log_err("fio: unknown rdma protocol - %d\n", rd->rdma_protocol);
		break;
	}

	/* [한국어] 이전에 남긴 CQE 잔량이 있으면 새 알림 대기 없이 0 반환 후 재시도 유도 */
	if (rd->cq_event_num > 0) {	/* previous left */
		rd->cq_event_num--;
		return 0;
	}

again:
	/* [한국어] CQ 이벤트 알림 대기 */
	if (ibv_get_cq_event(rd->channel, &ev_cq, &ev_ctx) != 0) {
		log_err("fio: Failed to get cq event!\n");
		return -1;
	}
	if (ev_cq != rd->cq) {
		log_err("fio: Unknown CQ!\n");
		return -1;
	}
	/* [한국어] 다음 알림 재등록 */
	if (ibv_req_notify_cq(rd->cq, 0) != 0) {
		log_err("fio: Failed to set notify!\n");
		return -1;
	}

	/* [한국어] CQE 수확 — 1 이상이면 처리, 아니면 다시 대기 */
	ret = cq_event_handler(td, comp_opcode);
	if (ret < 1)
		goto again;

	/* [한국어] 수확한 이벤트 수만큼 ack */
	ibv_ack_cq_events(rd->cq, ret);

	r += ret;
	/* [한국어] 최소 min개 누적될 때까지 반복 */
	if (r < min)
		goto again;

	/* [한국어] 반환할 만큼 cq_event_num에서 차감 */
	rd->cq_event_num -= r;

	return r;
}

/*
 * [한국어]
 * fio_rdmaio_send - 큐잉된 io_u들을 실제로 ibv_post_send로 제출.
 *
 * @td:    thread_data.
 * @io_us: 제출할 io_u 배열.
 * @nr:    개수.
 * @return: 제출한 개수(성공), -1 실패.
 *
 * 동기: WRITE/READ에서는 원격 MR 인덱스를 난수로 선택해 rkey/remote_addr 채움,
 * 매 I/O마다 length를 io_us[i]->buflen으로 갱신(실제 블록 크기가 매번 다를 수 있음).
 * SEND에서는 opcode/plag만 재확정. ibv_post_send는 QP의 send queue에 WR을 게시.
 * 호출 체인: fio_rdmaio_commit → [이 함수] (클라이언트 측만).
 */
static int fio_rdmaio_send(struct thread_data *td, struct io_u **io_us,
			   unsigned int nr)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct ibv_send_wr *bad_wr;  /* [한국어] 실패 시 처음 거부된 WR 포인터 반환 */
#if 0
	enum ibv_wc_opcode comp_opcode;
	comp_opcode = IBV_WC_RDMA_WRITE;
#endif
	int i;
	long index;
	struct rdma_io_u_data *r_io_u_d;

	r_io_u_d = NULL;

	for (i = 0; i < nr; i++) {
		/* RDMA_WRITE or RDMA_READ */
		switch (rd->rdma_protocol) {
		case FIO_RDMA_MEM_WRITE:
			/* compose work request */
			r_io_u_d = io_us[i]->engine_data;
			/* [한국어] 원격 MR 인덱스를 난수로 선택(부하 분산) */
			index = __rand(&rd->rand_state) % rd->rmt_nr;
			r_io_u_d->sq_wr.opcode = IBV_WR_RDMA_WRITE;
			r_io_u_d->sq_wr.wr.rdma.rkey = rd->rmt_us[index].rkey;
			r_io_u_d->sq_wr.wr.rdma.remote_addr = \
				rd->rmt_us[index].buf;
			r_io_u_d->sq_wr.sg_list->length = io_us[i]->buflen;
			break;
		case FIO_RDMA_MEM_READ:
			/* compose work request */
			r_io_u_d = io_us[i]->engine_data;
			index = __rand(&rd->rand_state) % rd->rmt_nr;
			r_io_u_d->sq_wr.opcode = IBV_WR_RDMA_READ;
			r_io_u_d->sq_wr.wr.rdma.rkey = rd->rmt_us[index].rkey;
			r_io_u_d->sq_wr.wr.rdma.remote_addr = \
				rd->rmt_us[index].buf;
			r_io_u_d->sq_wr.sg_list->length = io_us[i]->buflen;
			break;
		case FIO_RDMA_CHA_SEND:
			r_io_u_d = io_us[i]->engine_data;
			r_io_u_d->sq_wr.opcode = IBV_WR_SEND;
			r_io_u_d->sq_wr.send_flags = IBV_SEND_SIGNALED;
			break;
		default:
			log_err("fio: unknown rdma protocol - %d\n",
				rd->rdma_protocol);
			break;
		}

		/* [한국어] 실제 WR 게시 — 커널 RDMA 드라이버 경유 HCA doorbell ring */
		if (ibv_post_send(rd->qp, &r_io_u_d->sq_wr, &bad_wr) != 0) {
			log_err("fio: ibv_post_send fail: %m\n");
			return -1;
		}

		dprint_io_u(io_us[i], "fio_rdmaio_send");
	}

	/* wait for completion
	   rdma_poll_wait(td, comp_opcode); */
	/* [한국어] 완료 대기는 getevents/commit 경로에서 수행 — 여기선 비동기 제출만 */

	return i;
}

/*
 * [한국어]
 * fio_rdmaio_recv - 서버가 io_u를 recv queue에 post하거나, MEM 모드에서 완료 메시지 대기.
 *
 * @td:    thread_data.
 * @io_us: 대상 io_u 배열.
 * @nr:    개수.
 * @return: 처리 수(>=0), 1 오류, 0 MEM 모드에서 완료 메시지 수신 후 td->done=1 설정.
 *
 * 동기: CHA_RECV 서버는 클라이언트의 SEND를 받을 수 있도록 미리 recv buffer들을 post해야 한다
 * (RNR 방지). MEM_READ/WRITE 서버는 데이터 전송에 직접 관여하지 않고, 클라이언트가 보내는
 * 종료 통지(제어 메시지)만 대기한다 — 이 메시지를 받으면 td->done=1로 잡 종료.
 */
static int fio_rdmaio_recv(struct thread_data *td, struct io_u **io_us,
			   unsigned int nr)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct ibv_recv_wr *bad_wr;
	struct rdma_io_u_data *r_io_u_d;
	int i;

	i = 0;
	if (rd->rdma_protocol == FIO_RDMA_CHA_RECV) {
		/* post io_u into recv queue */
		/* [한국어] 각 io_u를 recv queue에 post — 향후 클라이언트 SEND를 받음 */
		for (i = 0; i < nr; i++) {
			r_io_u_d = io_us[i]->engine_data;
			if (ibv_post_recv(rd->qp, &r_io_u_d->rq_wr, &bad_wr) !=
			    0) {
				log_err("fio: ibv_post_recv fail: %m\n");
				return 1;
			}
		}
	} else if ((rd->rdma_protocol == FIO_RDMA_MEM_READ)
		   || (rd->rdma_protocol == FIO_RDMA_MEM_WRITE)) {
		/* re-post the rq_wr */
		/* [한국어] 제어 메시지 수신 WR 재포스트 — 클라이언트의 종료 통지 대기 */
		if (ibv_post_recv(rd->qp, &rd->rq_wr, &bad_wr) != 0) {
			log_err("fio: ibv_post_recv fail: %m\n");
			return 1;
		}

		/* [한국어] 수신 완료 블로킹 대기 */
		rdma_poll_wait(td, IBV_WC_RECV);

		dprint(FD_IO, "fio: recv FINISH message\n");
		/* [한국어] 클라이언트가 종료 통보했으므로 서버도 잡 종료 */
		td->done = 1;
		return 0;
	}

	return i;
}

/*
 * [한국어]
 * fio_rdmaio_queue - io_u 1개를 내부 큐에 적재(아직 제출 X).
 *
 * @td:   thread_data.
 * @io_u: 큐잉할 I/O.
 * @return: FIO_Q_QUEUED(큐 적재 성공) 또는 FIO_Q_BUSY(iodepth 초과).
 *
 * 동기: fio 엔진 플러그인 계약의 queue 콜백 — 즉시 제출하지 않고 모아서 commit으로 일괄 제출.
 * fio_ro_check는 이 엔진이 read-only 모드인지 검증.
 */
static enum fio_q_status fio_rdmaio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct rdmaio_data *rd = td->io_ops_data;

	/* [한국어] read-only 모드에서 write 요청이 오지 않았는지 검증 */
	fio_ro_check(td, io_u);

	/* [한국어] 큐가 가득 찼으면 BUSY — fio 코어가 재시도 루프 처리 */
	if (rd->io_u_queued_nr == (int)td->o.iodepth)
		return FIO_Q_BUSY;

	rd->io_us_queued[rd->io_u_queued_nr] = io_u;
	rd->io_u_queued_nr++;

	dprint_io_u(io_u, "fio_rdmaio_queue");

	return FIO_Q_QUEUED;
}

/*
 * [한국어]
 * fio_rdmaio_queued - queued 상태의 io_u들을 flight로 전이시키고 issue 시각 기록.
 *
 * @td:    thread_data.
 * @io_us: 방금 post된 io_u 배열.
 * @nr:    개수.
 *
 * 동기: commit이 실제 ibv_post_send/recv를 끝낸 뒤, 통계·상태 갱신을 묶어 처리.
 * fio_fill_issue_time이 true일 때만 시간 측정(일부 플래그에서 건너뛰기 가능).
 * iolog 모드면 td->last_issue도 함께 갱신하여 재생 로그에 반영.
 */
static void fio_rdmaio_queued(struct thread_data *td, struct io_u **io_us,
			      unsigned int nr)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct timespec now;
	unsigned int i;

	/* [한국어] FIO_ASYNCIO_SETS_ISSUE_TIME 플래그에 따라 건너뛸 수 있음 */
	if (!fio_fill_issue_time(td))
		return;

	/* [한국어] 현재 시각 한 번만 측정해 모든 io_u에 동일 적용(마이크로 최적화) */
	fio_gettime(&now, NULL);

	for (i = 0; i < nr; i++) {
		struct io_u *io_u = io_us[i];

		/* queued -> flight */
		/* [한국어] 상태 전이: queue 배열에서 빠져 flight 배열로 이동(여기서는 append만) */
		rd->io_us_flight[rd->io_u_flight_nr] = io_u;
		rd->io_u_flight_nr++;

		/* [한국어] issue_time 기록 — 지연시간(latency) 계산에 사용 */
		memcpy(&io_u->issue_time, &now, sizeof(now));
		/* [한국어] fio 코어 통계 훅 */
		io_u_queued(td, io_u);
	}

	/*
	 * only used for iolog
	 */
	/* [한국어] iolog 재생 모드에서 last_issue 갱신 */
	if (td->o.read_iolog_file)
		memcpy(&td->last_issue, &now, sizeof(now));
}

/*
 * [한국어]
 * fio_rdmaio_commit - 큐잉된 io_u 일괄 제출(send 또는 recv post).
 *
 * @td: thread_data.
 * @return: 0 성공, 비0 실패.
 *
 * 동기: fio_rdmaio_queue가 쌓아둔 io_us_queued를 is_client에 따라 send/recv 경로로 분기 제출.
 * fio_rdmaio_send/recv가 처리한 개수만큼 queued_nr 감소. io_us 포인터도 앞으로 이동시켜
 * 남은 항목을 재시도.
 * 호출 체인: backend.c → td_io_commit → [이 함수].
 */
static int fio_rdmaio_commit(struct thread_data *td)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct io_u **io_us;
	int ret;

	/* [한국어] 큐 자체가 미할당이면 즉시 성공 반환 */
	if (!rd->io_us_queued)
		return 0;

	io_us = rd->io_us_queued;
	do {
		/* RDMA_WRITE or RDMA_READ */
		/* [한국어] 역할 분기: 클라이언트는 send 계열, 서버는 recv 계열 */
		if (rd->is_client)
			ret = fio_rdmaio_send(td, io_us, rd->io_u_queued_nr);
		else if (!rd->is_client)
			ret = fio_rdmaio_recv(td, io_us, rd->io_u_queued_nr);

		if (ret > 0) {
			/* [한국어] 제출 성공 수만큼 상태 전이 및 통계 반영 */
			fio_rdmaio_queued(td, io_us, ret);
			io_u_mark_submit(td, ret);
			rd->io_u_queued_nr -= ret;
			io_us += ret;
			ret = 0;
		} else
			break;  /* [한국어] 0 또는 음수 반환 시 루프 종료 */
	} while (rd->io_u_queued_nr);

	return ret;
}

/*
 * [한국어]
 * fio_rdmaio_connect - 클라이언트 측 연결 수립 및 첫 제어 메시지 교환.
 *
 * @td: thread_data.
 * @f:  fio_file (네트워크 대상이라 실체 파일은 더미).
 * @return: 0 성공, 1 실패.
 *
 * 동기: RC QP는 명시적 연결이 필요 — rdma_connect → ESTABLISHED 이벤트 대기.
 * 이후 mode/iodepth를 담은 제어 메시지를 SEND하고, 서버의 응답(원격 MR 정보)을 RECV로 수신.
 * 마지막에 500ms sleep은 채널 시맨틱에서 RNR(Receiver Not Ready) 회피용 — 서버가 recv 버퍼를
 * 충분히 post할 시간을 확보하기 위함.
 * 호출 체인: fio_rdmaio_open_file(클라이언트 경로) → [이 함수].
 */
static int fio_rdmaio_connect(struct thread_data *td, struct fio_file *f)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdma_conn_param conn_param;
	struct ibv_send_wr *bad_wr;

	/* [한국어] 연결 파라미터 초기화(RC QP 설정) */
	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.responder_resources = 1;  /* [한국어] RDMA READ/atomic 동시 수행 가능 수 */
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;          /* [한국어] 연결 재시도 횟수 */

	/* [한국어] CM connect 요청 — 서버의 accept로 완료 */
	if (rdma_connect(rd->cm_id, &conn_param) != 0) {
		log_err("fio: rdma_connect fail: %m\n");
		return 1;
	}

	/* [한국어] ESTABLISHED 이벤트 대기 */
	if (get_next_channel_event
	    (td, rd->cm_channel, RDMA_CM_EVENT_ESTABLISHED) != 0) {
		log_err("fio: wait for RDMA_CM_EVENT_ESTABLISHED\n");
		return 1;
	}

	/* send task request */
	/* [한국어] 제어 메시지에 모드와 iodepth를 담아 서버로 전송 */
	rd->send_buf.mode = htonl(rd->rdma_protocol);
	rd->send_buf.nr = htonl(td->o.iodepth);

	if (ibv_post_send(rd->qp, &rd->sq_wr, &bad_wr) != 0) {
		log_err("fio: ibv_post_send fail: %m\n");
		return 1;
	}

	/* [한국어] SEND 완료 대기 */
	if (rdma_poll_wait(td, IBV_WC_SEND) < 0)
		return 1;

	/* wait for remote MR info from server side */
	/* [한국어] 서버로부터 원격 MR 정보 수신 대기 */
	if (rdma_poll_wait(td, IBV_WC_RECV) < 0)
		return 1;

	/* In SEND/RECV test, it's a good practice to setup the iodepth of
	 * of the RECV side deeper than that of the SEND side to
	 * avoid RNR (receiver not ready) error. The
	 * SEND side may send so many unsolicited message before
	 * RECV side commits sufficient recv buffers into recv queue.
	 * This may lead to RNR error. Here, SEND side pauses for a while
	 * during which RECV side commits sufficient recv buffers.
	 */
	/* [한국어] 500ms 대기 — 서버(RECV측)가 recv 버퍼 post를 충분히 끝내도록 유예 */
	usleep(500000);

	return 0;
}

/*
 * [한국어]
 * fio_rdmaio_accept - 서버 측 연결 수락 및 첫 제어 메시지 왕복.
 *
 * @td: thread_data.
 * @f:  fio_file (미사용).
 * @return: 0 성공, 1 실패.
 *
 * 동기: rdma_accept → ESTABLISHED 대기 → 클라이언트의 첫 SEND 수신 → 서버가 원격 MR 정보를
 * SEND로 회신. 호출 체인: fio_rdmaio_open_file(서버 경로) → [이 함수].
 */
static int fio_rdmaio_accept(struct thread_data *td, struct fio_file *f)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdma_conn_param conn_param;
	struct ibv_send_wr *bad_wr;
	int ret = 0;

	/* rdma_accept() - then wait for accept success */
	memset(&conn_param, 0, sizeof(conn_param));
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	/* [한국어] 자식 cm_id(CONNECT_REQUEST에서 받은)에 대해 accept */
	if (rdma_accept(rd->child_cm_id, &conn_param) != 0) {
		log_err("fio: rdma_accept: %m\n");
		return 1;
	}

	if (get_next_channel_event
	    (td, rd->cm_channel, RDMA_CM_EVENT_ESTABLISHED) != 0) {
		log_err("fio: wait for RDMA_CM_EVENT_ESTABLISHED\n");
		return 1;
	}

	/* wait for request */
	/* [한국어] 클라이언트의 첫 제어 메시지 수신 */
	ret = rdma_poll_wait(td, IBV_WC_RECV) < 0;

	/* [한국어] 원격 MR 정보 포함한 회신 전송 (send_buf는 post_init에서 채워짐) */
	if (ibv_post_send(rd->qp, &rd->sq_wr, &bad_wr) != 0) {
		log_err("fio: ibv_post_send fail: %m\n");
		return 1;
	}

	if (rdma_poll_wait(td, IBV_WC_SEND) < 0)
		return 1;

	return ret;
}

/*
 * [한국어]
 * fio_rdmaio_open_file - 파일(=연결) 오픈 콜백. 클라이언트면 connect, 서버면 accept.
 *
 * @td: thread_data.
 * @f:  fio_file.
 * @return: 0 성공, 비0 실패.
 *
 * 동기: fio 엔진 계약상 open_file은 실제 파일 open에 대응하지만, 네트워크 엔진에서는
 * 연결 수립으로 치환된다. td_read(td)가 true면 이 노드는 서버 역할로 동작.
 */
static int fio_rdmaio_open_file(struct thread_data *td, struct fio_file *f)
{
	if (td_read(td))
		return fio_rdmaio_accept(td, f);
	else
		return fio_rdmaio_connect(td, f);
}

/*
 * [한국어]
 * fio_rdmaio_close_file - 연결 종료 및 리소스 정리.
 *
 * @td: thread_data.
 * @f:  fio_file.
 * @return: 0 성공, 1 ibv_post_send 실패.
 *
 * 동기: 클라이언트가 MEM 모드인 경우 서버에 종료 통지(제어 메시지 SEND) 전송 → 서버 측
 * fio_rdmaio_recv가 td->done=1로 잡 종료. 이후 CM disconnect, CQ/QP/PD/comp_channel 파괴.
 * 호출 체인: backend.c → td_io_close_file → [이 함수].
 */
static int fio_rdmaio_close_file(struct thread_data *td, struct fio_file *f)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct ibv_send_wr *bad_wr;

	/* unregister rdma buffer */

	/*
	 * Client sends notification to the server side
	 */
	/* refer to: http://linux.die.net/man/7/rdma_cm */
	/* [한국어] 클라이언트 && MEM 모드면 종료 통지 제어 메시지 전송 */
	if ((rd->is_client == 1) && ((rd->rdma_protocol == FIO_RDMA_MEM_WRITE)
				     || (rd->rdma_protocol ==
					 FIO_RDMA_MEM_READ))) {
		if (ibv_post_send(rd->qp, &rd->sq_wr, &bad_wr) != 0) {
			log_err("fio: ibv_post_send fail: %m\n");
			return 1;
		}

		dprint(FD_IO, "fio: close information sent success\n");
		rdma_poll_wait(td, IBV_WC_SEND);  /* [한국어] 전송 완료 대기 */
	}

	/* [한국어] 역할별 CM disconnect — 서버는 자식 id 기준 */
	if (rd->is_client == 1)
		rdma_disconnect(rd->cm_id);
	else {
		rdma_disconnect(rd->child_cm_id);
#if 0
		rdma_disconnect(rd->cm_id);
#endif
	}

#if 0
	/* [한국어] DISCONNECTED 이벤트 대기는 선택 — 현재는 비활성 */
	if (get_next_channel_event(td, rd->cm_channel, RDMA_CM_EVENT_DISCONNECTED) != 0) {
		log_err("fio: wait for RDMA_CM_EVENT_DISCONNECTED\n");
		return 1;
	}
#endif

	/* [한국어] 리소스 역순 해제: CQ → QP → cm_id → comp_channel → PD */
	ibv_destroy_cq(rd->cq);
	ibv_destroy_qp(rd->qp);

	if (rd->is_client == 1)
		rdma_destroy_id(rd->cm_id);
	else {
		rdma_destroy_id(rd->child_cm_id);
		rdma_destroy_id(rd->cm_id);
	}

	ibv_destroy_comp_channel(rd->channel);
	ibv_dealloc_pd(rd->pd);

	return 0;
}

/*
 * [한국어]
 * aton - 호스트명 또는 IP 문자열을 sockaddr_in.sin_addr로 변환.
 *
 * @td:   thread_data(오류 보고용).
 * @host: 문자열(IP 또는 호스트명).
 * @addr: 결과 저장 sockaddr_in.
 * @return: 0 성공, 1 gethostbyname 실패.
 *
 * 동기: inet_aton으로 점 표기 IP를 먼저 시도, 실패 시 gethostbyname으로 DNS 조회.
 * IPv4 전용(h_addr의 첫 4바이트를 복사). 이름은 "addr to network"의 줄임.
 */
static int aton(struct thread_data *td, const char *host,
		     struct sockaddr_in *addr)
{
	/* [한국어] 문자 IP가 아니면 DNS 폴백 */
	if (inet_aton(host, &addr->sin_addr) != 1) {
		struct hostent *hent;

		hent = gethostbyname(host);
		if (!hent) {
			td_verror(td, errno, "gethostbyname");
			return 1;
		}

		/* [한국어] IPv4 A 레코드의 첫 4바이트를 복사 */
		memcpy(&addr->sin_addr, hent->h_addr, 4);
	}
	return 0;
}

/*
 * [한국어]
 * fio_rdmaio_setup_connect - 클라이언트 측 주소/라우트 해석, QP 생성, recv buffer post.
 *
 * @td:   thread_data.
 * @host: 원격 호스트(옵션 filename).
 * @port: 원격 포트.
 * @return: 0 성공, 1 실패.
 *
 * 동기: CM의 주소 해석 파이프라인 — resolve_addr → ADDR_RESOLVED → resolve_route →
 * ROUTE_RESOLVED. 이후 QP/버퍼를 세팅하고 제어 메시지 수신 WR을 미리 post해 서버 응답을
 * 받을 준비 완료. bindname이 설정되어 있으면 특정 로컬 인터페이스에 바인드.
 */
static int fio_rdmaio_setup_connect(struct thread_data *td, const char *host,
				    unsigned short port)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdmaio_options *o = td->eo;
	struct sockaddr_storage addrb;
	struct ibv_recv_wr *bad_wr;
	int err;

	/* [한국어] 원격 주소 구조체 채움 */
	rd->addr.sin_family = AF_INET;
	rd->addr.sin_port = htons(port);

	err = aton(td, host, &rd->addr);
	if (err)
		return err;

	/* resolve route */
	/* [한국어] 로컬 바인딩 주소 있으면 dual-address resolve */
	if (o->bindname && strlen(o->bindname)) {
		addrb.ss_family = AF_INET;
		err = aton(td, o->bindname, (struct sockaddr_in *)&addrb);
		if (err)
			return err;
		err = rdma_resolve_addr(rd->cm_id, (struct sockaddr *)&addrb,
					(struct sockaddr *)&rd->addr, 2000);

	} else {
		/* [한국어] 자동 로컬 선택 */
		err = rdma_resolve_addr(rd->cm_id, NULL,
					(struct sockaddr *)&rd->addr, 2000);
	}

	if (err != 0) {
		log_err("fio: rdma_resolve_addr: %d\n", err);
		return 1;
	}

	/* [한국어] ADDR_RESOLVED 이벤트 대기 */
	err = get_next_channel_event(td, rd->cm_channel, RDMA_CM_EVENT_ADDR_RESOLVED);
	if (err != 0) {
		log_err("fio: get_next_channel_event: %d\n", err);
		return 1;
	}

	/* resolve route */
	/* [한국어] 라우트 해석(InfiniBand path record 조회) */
	err = rdma_resolve_route(rd->cm_id, 2000);
	if (err != 0) {
		log_err("fio: rdma_resolve_route: %d\n", err);
		return 1;
	}

	err = get_next_channel_event(td, rd->cm_channel, RDMA_CM_EVENT_ROUTE_RESOLVED);
	if (err != 0) {
		log_err("fio: get_next_channel_event: %d\n", err);
		return 1;
	}

	/* create qp and buffer */
	if (fio_rdmaio_setup_qp(td) != 0)
		return 1;

	if (fio_rdmaio_setup_control_msg_buffers(td) != 0)
		return 1;

	/* post recv buf */
	/* [한국어] 서버 응답을 받을 수 있도록 recv WR을 미리 post */
	err = ibv_post_recv(rd->qp, &rd->rq_wr, &bad_wr);
	if (err != 0) {
		log_err("fio: ibv_post_recv fail: %d\n", err);
		return 1;
	}

	return 0;
}

/*
 * [한국어]
 * fio_rdmaio_setup_listen - 서버 측 bind/listen, CONNECT_REQUEST 대기, QP 생성.
 *
 * @td:   thread_data.
 * @port: listen 포트.
 * @return: 0 성공, 1 실패.
 *
 * 동기: rdma_bind_addr → rdma_listen → CONNECT_REQUEST 이벤트 대기(클라이언트의 connect).
 * 수신 후 setup_qp에서 child_cm_id 기준으로 QP 생성 → 제어 메시지 수신 WR post. 이후 accept는
 * 별도 open_file 경로에서 수행.
 */
static int fio_rdmaio_setup_listen(struct thread_data *td, short port)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdmaio_options *o = td->eo;
	struct ibv_recv_wr *bad_wr;
	int state = td->runstate;  /* [한국어] 이전 runstate 보존(복원용) */

	/* [한국어] setup 중임을 fio 코어에 알림 — 통계에서 setup 시간 제외 */
	td_set_runstate(td, TD_SETTING_UP);

	rd->addr.sin_family = AF_INET;
	rd->addr.sin_port = htons(port);

	/* [한국어] bindname 없으면 INADDR_ANY(모든 인터페이스), 있으면 첫 바이트값 사용(레거시 동작) */
	if (!o->bindname || !strlen(o->bindname))
		rd->addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
		rd->addr.sin_addr.s_addr = htonl(*o->bindname);

	/* rdma_listen */
	if (rdma_bind_addr(rd->cm_id, (struct sockaddr *)&rd->addr) != 0) {
		log_err("fio: rdma_bind_addr fail: %m\n");
		return 1;
	}

	/* [한국어] backlog=3 — 대기 큐 길이(소량의 동시 접속 허용) */
	if (rdma_listen(rd->cm_id, 3) != 0) {
		log_err("fio: rdma_listen fail: %m\n");
		return 1;
	}

	log_info("fio: waiting for connection\n");

	/* wait for CONNECT_REQUEST */
	/* [한국어] 클라이언트의 connect 대기. 수신 시 child_cm_id 저장(get_next_channel_event 내부) */
	if (get_next_channel_event
	    (td, rd->cm_channel, RDMA_CM_EVENT_CONNECT_REQUEST) != 0) {
		log_err("fio: wait for RDMA_CM_EVENT_CONNECT_REQUEST\n");
		return 1;
	}

	if (fio_rdmaio_setup_qp(td) != 0)
		return 1;

	if (fio_rdmaio_setup_control_msg_buffers(td) != 0)
		return 1;

	/* post recv buf */
	if (ibv_post_recv(rd->qp, &rd->rq_wr, &bad_wr) != 0) {
		log_err("fio: ibv_post_recv fail: %m\n");
		return 1;
	}

	/* [한국어] 이전 runstate 복원 */
	td_set_runstate(td, state);
	return 0;
}

/*
 * [한국어]
 * check_set_rlimits - RLIMIT_MEMLOCK(pinning 가능 메모리)을 io 버퍼 크기 이상으로 확대.
 *
 * @td: thread_data.
 * @return: 0 성공, 1 실패.
 *
 * 동기: ibv_reg_mr은 물리 메모리를 pin하므로 MEMLOCK 한계를 초과하면 실패.
 * orig_buffer_size가 soft limit보다 크면 hard limit로 재설정 시도. root 권한 부족 시 실패하면
 * 사용자에게 `ulimit -l unlimited` 안내 로그.
 * CONFIG_RLIMIT_MEMLOCK이 정의되지 않은 OS에서는 no-op(항상 0 반환).
 */
static int check_set_rlimits(struct thread_data *td)
{
#ifdef CONFIG_RLIMIT_MEMLOCK
	/* [한국어] Linux 등 RLIMIT_MEMLOCK을 지원하는 OS에서만 컴파일됨 */
	struct rlimit rl;

	/* check RLIMIT_MEMLOCK */
	if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
		log_err("fio: getrlimit fail: %d(%s)\n",
			errno, strerror(errno));
		return 1;
	}

	/* soft limit */
	/* [한국어] soft limit이 무한도 아니고 우리 버퍼보다 작으면 확대 시도 */
	if ((rl.rlim_cur != RLIM_INFINITY)
	    && (rl.rlim_cur < td->orig_buffer_size)) {
		log_err("fio: soft RLIMIT_MEMLOCK is: %" PRId64 "\n",
			rl.rlim_cur);
		log_err("fio: total block size is:    %zd\n",
			td->orig_buffer_size);
		/* try to set larger RLIMIT_MEMLOCK */
		/* [한국어] soft를 hard까지 끌어올림 */
		rl.rlim_cur = rl.rlim_max;
		if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
			log_err("fio: setrlimit fail: %d(%s)\n",
				errno, strerror(errno));
			log_err("fio: you may try enlarge MEMLOCK by root\n");
			log_err("# ulimit -l unlimited\n");
			return 1;
		}
	}
#endif

	return 0;
}

/*
 * [한국어]
 * compat_options - 구버전 filename 포맷(host/port/mode) 하위호환 파싱.
 *
 * @td: thread_data.
 * @return: 0 성공, 1 잘못된 형식.
 *
 * 동기: 구버전 RDMA 엔진은 filename을 "host/port/mode" 슬래시 구분자로 받았다. 이 함수는
 * 새 옵션(--port, --verb)이 지정되지 않았을 때 filename을 역파싱해 채운다.
 * bindname 옵션은 레거시 모드에서 지원하지 않음.
 */
static int compat_options(struct thread_data *td)
{
	// The original RDMA engine had an ugly / separator
	// on the filename for it's options. This function
	// retains backwards compatibility with it. Note we do not
	// support setting the bindname option is this legacy mode.

	struct rdmaio_options *o = td->eo;
	char *modep, *portp;
	char *filename = td->o.filename;

	/* [한국어] filename이 없으면 파싱할 것도 없음 */
	if (!filename)
		return 0;

	/* [한국어] 첫 '/' 위치 탐색 — port 구분자 */
	portp = strchr(filename, '/');
	if (portp == NULL)
		return 0;

	/* [한국어] '/'를 널로 잘라 host 부분을 filename이 가리키도록 유지 */
	*portp = '\0';
	portp++;

	/* [한국어] port 문자열을 10진수로 변환 */
	o->port = strtol(portp, NULL, 10);
	if (!o->port || o->port > 65535)
		goto bad_host;

	/* [한국어] 두 번째 '/' — mode 구분자(선택) */
	modep = strchr(portp, '/');
	if (modep != NULL) {
		*modep = '\0';
		modep++;
	}

	/* [한국어] mode 문자열을 enum으로 매핑 */
	if (modep) {
		if (!strncmp("rdma_write", modep, strlen(modep)) ||
		    !strncmp("RDMA_WRITE", modep, strlen(modep)))
			o->verb = FIO_RDMA_MEM_WRITE;
		else if (!strncmp("rdma_read", modep, strlen(modep)) ||
			 !strncmp("RDMA_READ", modep, strlen(modep)))
			o->verb = FIO_RDMA_MEM_READ;
		else if (!strncmp("send", modep, strlen(modep)) ||
			 !strncmp("SEND", modep, strlen(modep)))
			o->verb = FIO_RDMA_CHA_SEND;
		else
			goto bad_host;
	} else
		o->verb = FIO_RDMA_MEM_WRITE;  /* [한국어] mode 생략 시 기본 WRITE */


	return 0;

bad_host:
	log_err("fio: bad rdma host/port/protocol: %s\n", td->o.filename);
	return 1;
}

/*
 * [한국어]
 * fio_rdmaio_init - 엔진 초기화 엔트리. CM 채널/id 생성, 역할 결정, listen/connect까지 수행.
 *
 * @td: thread_data.
 * @return: 0 성공, 1 실패.
 *
 * 동기: fio 엔진 계약의 init 콜백. 옵션 유효성(rw 동시 금지, 랜덤 금지, port 지정), rlimit
 * 확장, CM 이벤트 채널·cm_id 생성, io_u 관리 배열 할당, 역할(서버=read/클라이언트=write) 결정.
 * MEM_WRITE/READ 모드에서만 rmt_us 배열 사전 할당(채널 모드는 원격 MR 정보 불필요).
 * 호출 체인: backend.c → td_io_init → [이 함수].
 */
static int fio_rdmaio_init(struct thread_data *td)
{
	struct rdmaio_data *rd = td->io_ops_data;
	struct rdmaio_options *o = td->eo;
	int ret;

	/* [한국어] RDMA 엔진 제약: 한 연결은 단일 방향(read OR write)만 허용 */
	if (td_rw(td)) {
		log_err("fio: rdma connections must be read OR write\n");
		return 1;
	}
	/* [한국어] 랜덤 I/O는 RDMA 네트워크 엔진에서 의미 없음 — 서버는 순차적으로 MR을 수신 */
	if (td_random(td)) {
		log_err("fio: RDMA network IO can't be random\n");
		return 1;
	}

	/* [한국어] 레거시 filename 포맷 파싱(있으면) */
	if (compat_options(td))
		return 1;

	/* [한국어] port는 필수 */
	if (!o->port) {
		log_err("fio: no port has been specified which is required "
			"for the rdma engine\n");
		return 1;
	}

	/* [한국어] MEMLOCK 한계 확장(ibv_reg_mr 대비) */
	if (check_set_rlimits(td))
		return 1;

	rd->rdma_protocol = o->verb;
	rd->cq_event_num = 0;

	/* [한국어] CM 이벤트 채널 생성(fd 기반) */
	rd->cm_channel = rdma_create_event_channel();
	if (!rd->cm_channel) {
		log_err("fio: rdma_create_event_channel fail: %m\n");
		return 1;
	}

	/* [한국어] CM id 생성. RDMA_PS_TCP = RC QP 기반 TCP 유사 포트스페이스 */
	ret = rdma_create_id(rd->cm_channel, &rd->cm_id, rd, RDMA_PS_TCP);
	if (ret) {
		log_err("fio: rdma_create_id fail: %m\n");
		return 1;
	}

	/* [한국어] 메모리 시맨틱에서만 원격 MR 배열 예약 */
	if ((rd->rdma_protocol == FIO_RDMA_MEM_WRITE) ||
	    (rd->rdma_protocol == FIO_RDMA_MEM_READ)) {
		rd->rmt_us = calloc(FIO_RDMA_MAX_IO_DEPTH,
				    sizeof(struct remote_u));
		rd->rmt_nr = 0;
	}

	/* [한국어] io_u 관리 큐 3종 할당(queued/flight/completed) */
	rd->io_us_queued = calloc(td->o.iodepth, sizeof(struct io_u *));
	rd->io_u_queued_nr = 0;

	rd->io_us_flight = calloc(td->o.iodepth, sizeof(struct io_u *));
	rd->io_u_flight_nr = 0;

	rd->io_us_completed = calloc(td->o.iodepth, sizeof(struct io_u *));
	rd->io_u_completed_nr = 0;

	if (td_read(td)) {	/* READ as the server */
		rd->is_client = 0;
		/* [한국어] 서버는 완료를 fio에 보고하지 않으므로 진행률 추적 비활성 */
		td->flags |= TD_F_NO_PROGRESS;
		/* server rd->rdma_buf_len will be setup after got request */
		ret = fio_rdmaio_setup_listen(td, o->port);
	} else {		/* WRITE as the client */
		rd->is_client = 1;
		ret = fio_rdmaio_setup_connect(td, td->o.filename, o->port);
	}
	return ret;
}
/*
 * [한국어]
 * fio_rdmaio_post_init - 엔진 post-init 콜백. 각 io_u 버퍼를 MR로 등록하고 send_buf에 rmt_us 기록.
 *
 * @td: thread_data.
 * @return: 0 성공, 1 ibv_reg_mr 실패.
 *
 * 동기: fio 코어가 io_u freelist를 할당한 뒤 이 콜백이 호출된다. 각 io_u->buf를 LOCAL_WRITE +
 * REMOTE_READ/WRITE 권한으로 MR 등록하고, 엔진별 확장 데이터를 calloc로 매달아 wr_id를 부여.
 * 서버는 동시에 send_buf.rmt_us[]에 자신의 rkey/addr/size를 기록하여 클라이언트에게 전달 가능하도록 함.
 * 호출 체인: backend.c → td_io_post_init → [이 함수].
 */
static int fio_rdmaio_post_init(struct thread_data *td)
{
	unsigned int max_bs;
	int i;
	struct rdmaio_data *rd = td->io_ops_data;

	max_bs = max(td->o.max_bs[DDIR_READ], td->o.max_bs[DDIR_WRITE]);
	/* [한국어] 제어 메시지에 max_bs 반영(클라이언트가 검증) */
	rd->send_buf.max_bs = htonl(max_bs);

	/* register each io_u in the free list */
	/* [한국어] 모든 프리리스트 io_u를 순회하며 MR 등록과 엔진 데이터 부착 */
	for (i = 0; i < td->io_u_freelist.nr; i++) {
		struct io_u *io_u = td->io_u_freelist.io_us[i];

		/* [한국어] io_u별 확장 구조체 할당 */
		io_u->engine_data = calloc(1, sizeof(struct rdma_io_u_data));
		((struct rdma_io_u_data *)io_u->engine_data)->wr_id = i;

		/* [한국어] io_u 버퍼 MR 등록: 로컬 쓰기 + 원격 읽기·쓰기 권한.
		 * 서버가 client로부터 RDMA_WRITE를 받으려면 REMOTE_WRITE 필수,
		 * RDMA_READ를 허용하려면 REMOTE_READ 필수. */
		io_u->mr = ibv_reg_mr(rd->pd, io_u->buf, max_bs,
				      IBV_ACCESS_LOCAL_WRITE |
				      IBV_ACCESS_REMOTE_READ |
				      IBV_ACCESS_REMOTE_WRITE);
		if (io_u->mr == NULL) {
			log_err("fio: ibv_reg_mr io_u failed: %m\n");
			return 1;
		}

		/* [한국어] 서버 측이 클라이언트에게 전달할 rmt_us 엔트리 구성(네트워크 바이트순) */
		rd->send_buf.rmt_us[i].buf =
		    cpu_to_be64((uint64_t) (unsigned long)io_u->buf);
		rd->send_buf.rmt_us[i].rkey = htonl(io_u->mr->rkey);
		rd->send_buf.rmt_us[i].size = htonl(max_bs);

#if 0
		log_info("fio: Send rkey %x addr %" PRIx64 " len %d to client\n", io_u->mr->rkey, io_u->buf, max_bs); */
#endif
	}

	/* [한국어] 유효 rmt_us 엔트리 수 기록 */
	rd->send_buf.nr = htonl(i);

	return 0;
}

/*
 * [한국어]
 * fio_rdmaio_cleanup - 엔진 리소스 해제.
 *
 * @td: thread_data.
 *
 * 동기: fio 엔진 계약의 cleanup 콜백. 현재 구현은 rdmaio_data 자체만 free하고,
 * MR/QP/CQ/PD는 close_file에서 이미 해제되었다고 가정(불완전하지만 원본 그대로 유지).
 */
static void fio_rdmaio_cleanup(struct thread_data *td)
{
	struct rdmaio_data *rd = td->io_ops_data;

	if (rd)
		free(rd);
}

/*
 * [한국어]
 * fio_rdmaio_setup - 엔진 setup 콜백. 더미 파일 추가 및 rdmaio_data 초기 할당.
 *
 * @td: thread_data.
 * @return: 0 고정.
 *
 * 동기: 네트워크 엔진이므로 실제 파일은 없지만 fio는 최소 1개의 fio_file을 요구 →
 * filename을 그대로 쓰거나 "rdma" 기본 이름으로 add_file 호출. 난수 상태는
 * GOLDEN_RATIO_64 시드로 초기화(원격 MR 인덱스 선택용).
 */
static int fio_rdmaio_setup(struct thread_data *td)
{
	struct rdmaio_data *rd;

	if (!td->files_index) {
		/* [한국어] 잡에 파일이 하나도 등록되지 않았다면 더미 파일 추가 */
		add_file(td, td->o.filename ?: "rdma", 0, 0);
		td->o.nr_files = td->o.nr_files ?: 1;
		td->o.open_files++;
	}

	if (!td->io_ops_data) {
		/* [한국어] rdmaio_data 최초 할당 및 난수 시드 설정 */
		rd = calloc(1, sizeof(*rd));
		init_rand_seed(&rd->rand_state, (unsigned int) GOLDEN_RATIO_64, 0);
		td->io_ops_data = rd;
	}

	return 0;
}

/*
 * [한국어] ioengine_ops 테이블 — fio 코어가 이 엔진을 식별하고 콜백을 호출할 디스패치 테이블.
 * FIO_STATIC은 외부로 노출되지 않게 하며, register_ioengine으로 런타임에 테이블 체인에 등록된다.
 * flags 의미:
 *   FIO_DISKLESSIO: 실 파일 I/O가 아님(네트워크 엔진)
 *   FIO_UNIDIR:     단일 방향(read 또는 write만; td_rw 금지)
 *   FIO_PIPEIO:     파이프성 I/O(sequential, 오프셋 무의미)
 *   FIO_ASYNCIO_SETS_ISSUE_TIME: 엔진이 직접 issue_time을 설정(fio 코어가 건너뜀)
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "rdma",                 /* [한국어] --ioengine=rdma 선택 키 */
	.version		= FIO_IOOPS_VERSION,      /* [한국어] ABI 버전 — fio 코어와 매치 필요 */
	.setup			= fio_rdmaio_setup,
	.init			= fio_rdmaio_init,
	.post_init		= fio_rdmaio_post_init,
	.prep			= fio_rdmaio_prep,
	.queue			= fio_rdmaio_queue,
	.commit			= fio_rdmaio_commit,
	.getevents		= fio_rdmaio_getevents,
	.event			= fio_rdmaio_event,
	.cleanup		= fio_rdmaio_cleanup,
	.open_file		= fio_rdmaio_open_file,
	.close_file		= fio_rdmaio_close_file,
	.flags			= FIO_DISKLESSIO | FIO_UNIDIR | FIO_PIPEIO |
					FIO_ASYNCIO_SETS_ISSUE_TIME,
	.options		= options,                /* [한국어] 위에서 정의한 엔진 옵션 테이블 */
	.option_struct_size	= sizeof(struct rdmaio_options),
};

/*
 * [한국어]
 * fio_rdmaio_register - 라이브러리 로드 시 ioengine을 fio 코어에 등록.
 *
 * fio_init 속성은 __attribute__((constructor))로 치환되어 main 이전에 자동 실행된다.
 * register_ioengine은 전역 엔진 리스트에 본 ioengine을 삽입.
 */
static void fio_init fio_rdmaio_register(void)
{
	register_ioengine(&ioengine);
}

/*
 * [한국어]
 * fio_rdmaio_unregister - 라이브러리 언로드 시 엔진을 리스트에서 제거.
 *
 * fio_exit 속성은 __attribute__((destructor))로 치환되어 프로세스 종료 시 자동 실행.
 */
static void fio_exit fio_rdmaio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
