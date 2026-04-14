/*
 * [한국어 설명] 네트워크 I/O 엔진 구현 (net.c — "net" 및 "netsplice" 두 엔진)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 두 개의 I/O 엔진을 동시에 구현한다: (1) "net" — 일반 소켓 I/O
 * (send/recv/sendto/recvfrom/sendmsg/recvmsg)로 TCP/UDP(IPv4·IPv6)/Unix 도메인 소켓/
 * VSOCK(게스트-호스트 VM 통신)을 커버, (2) "netsplice" — splice(2) + 파이프를 이용한
 * 제로카피 전송(FIO_PIPEIO 플래그, 커널 버퍼 간 페이지 이동). 클라이언트/서버 모드를
 * 모두 지원하며, UDP 모드에서는 각 패킷 앞에 udp_seq 헤더를 붙여 수신 측에서 시퀀스
 * 번호·블록 크기·매직 넘버로 순서·무결성을 검증한다. 연결 개방/종료 신호를 위해 UDP
 * 는 udp_close_msg(OPEN/CLOSE 매직)도 주고받는다. 옵션은 독자적인 optgroup
 * (FIO_OPT_G_NETIO)로 등록되며, proto=tcp/udp/unix/tcpv6/udpv6/vsock 및 pingpong,
 * nodelay, window_size, mss, ttl, 인터페이스 바인딩 등 상세 네트워크 튜닝을 제공.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend 잡 루프: load_ioengine → setup(fio_netio_setup 또는 _setup_splice)
 * → init(fio_netio_init: 소켓 create/bind·listen·accept 또는 connect) → prep
 * (전송 크기 보정) → queue(fio_netio_queue: 송수신, splice 사용 시 pipes[] 중계)
 * → cleanup/close_file/terminate. 이 엔진은 동기 엔진(FIO_SYNCIO)이라 queue()
 * 내부에서 blocking send/recv/poll까지 수행 후 FIO_Q_COMPLETED 반환. 실행 컨텍스트
 * 는 잡 스레드의 사용자 공간. splice 모드는 디스크리스(FIO_DISKLESSIO) + 파이프 기반.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h, optgroup.h, verify.h: 공용 타입/옵션/검증 프레임워크.
 * - <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>, <netdb.h>: POSIX 소켓 API.
 * - <sys/un.h>, <linux/vm_sockets.h>: Unix/VSOCK 주소 구조체.
 * - <poll.h>, <sys/stat.h>: poll(2) 및 Unix 소켓 경로 검증.
 * - splice(2): netsplice 엔진에서 zero-copy.
 * - 공유 상태:
 *   - td->io_ops_data = struct netio_data  (잡 스레드 소유).
 *   - td->eo           = struct netio_options (파싱된 옵션, 불변).
 *   - UDP의 경우 잡 양쪽(서버/클라이언트)이 udp_seq/udp_close_msg를 프로토콜로 공유.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct netio_data:    소켓 FD/listen FD/splice 파이프/주소 구조체들/UDP 시퀀스.
 * - struct netio_options: 포트·프로토콜·pingpong·NODELAY·WinSize·MSS·TTL·인터페이스.
 * - struct udp_seq:       UDP 패킷 매직·시퀀스·블록 크기 헤더.
 * - struct udp_close_msg: UDP 링크 OPEN/CLOSE 시그널 패킷.
 * - fio_netio_init():     proto에 따른 socket + bind + listen/accept 또는 connect.
 * - fio_netio_queue():    ddir + pingpong에 따른 send/recv/splice 경로 분기.
 * - fio_netio_send()/recv()/splice_*(): 각 전송 모드별 하위 구현.
 * - str2proto()/str2listen(): 문자열 옵션 파싱 헬퍼.
 *
 * === fio에서의 사용법 ===
 * --ioengine=net, --ioengine=netsplice. proto=tcp|udp|unix|tcpv6|udpv6|vsock,
 * listen=1(서버), hostname=..., port=...
 */

/*
 * net engine
 *
 * IO engine that reads/writes to/from sockets.
 *
 */
#include <stdio.h>            /* [한국어] 로그 포매팅 */
#include <stdlib.h>           /* [한국어] calloc/free/malloc/atoi */
#include <unistd.h>           /* [한국어] close/read/write/pipe 등 기본 POSIX */
#include <signal.h>           /* [한국어] 시그널 처리(서버 모드 중단 대응) */
#include <errno.h>            /* [한국어] errno 및 에러 코드 */
#include <netinet/in.h>       /* [한국어] sockaddr_in/sockaddr_in6, IPPROTO_* */
#include <netinet/tcp.h>      /* [한국어] TCP_NODELAY, TCP_MAXSEG 등 TCP 옵션 */
#include <arpa/inet.h>        /* [한국어] inet_pton/htons 등 주소 변환 */
#include <netdb.h>            /* [한국어] getaddrinfo — 호스트명 해석 */
#include <poll.h>             /* [한국어] poll(2) — 서버 accept 대기 등 */
#include <sys/stat.h>         /* [한국어] stat — Unix 소켓 경로 검증 */
#include <sys/socket.h>       /* [한국어] socket/bind/listen/accept/send/recv */
#include <sys/un.h>           /* [한국어] sockaddr_un — Unix 도메인 소켓 */

/* [한국어] VSOCK 지원은 커널 헤더 유무에 따라 조건부.
 * 미지원 빌드에서도 타입/매크로는 자리만 잡아 컴파일을 통과시킨다. */
#ifdef CONFIG_VSOCK
#include <linux/vm_sockets.h>
#else
struct sockaddr_vm {
};
#ifndef AF_VSOCK
#define AF_VSOCK	-1   /* [한국어] 런타임에 이 값으로 분기되면 에러 처리 */
#endif
#endif

#include "../fio.h"
#include "../verify.h"
#include "../optgroup.h"

/*
 * [한국어] net 엔진의 잡별 런타임 상태. td->io_ops_data가 가리킴.
 * 잡 스레드 단독 소유 — 동기화 불필요.
 */
struct netio_data {
	int listenfd;
	/* [한국어] 서버 모드에서 listen(2)까지 수행된 소켓 FD. -1이면 미사용(클라이언트).
	 * 설정자: fio_netio_accept 경로에서 socket/bind/listen 후 저장.
	 * 읽는 자: accept 루프/종료 시 close. 값 범위: 유효 FD 또는 -1. */

	int use_splice;
	/* [한국어] 1이면 splice(2) 기반 netsplice 경로, 0이면 일반 send/recv.
	 * 설정자: fio_netio_setup_splice 혹은 엔진 진입점에서 결정.
	 * 읽는 자: queue 분기. 불변(잡 런타임 중). */

	int seq_off;
	/* [한국어] UDP 모드에서 데이터 버퍼 선두에 붙는 udp_seq 헤더의 오프셋 보정(0 또는 sizeof).
	 * verify 비활성 등으로 시퀀스 검증을 끄는 경우 0으로 둔다. */

	int pipes[2];
	/* [한국어] splice 경로에서 파일↔소켓 사이 중계로 쓰는 익명 파이프 fd 쌍.
	 * pipes[0]=read, pipes[1]=write. 설정자: fio_netio_setup_splice의 pipe(). */

	struct sockaddr_in addr;      /* [한국어] IPv4 주소 구조체 — TCP/UDP v4 */
	struct sockaddr_in6 addr6;    /* [한국어] IPv6 주소 구조체 — TCP/UDP v6 */
	struct sockaddr_un addr_un;   /* [한국어] Unix 도메인 소켓 경로 주소 */
	struct sockaddr_vm addr_vm;   /* [한국어] VSOCK 주소(guest-host) */

	uint64_t udp_send_seq;
	/* [한국어] UDP 송신 시 패킷마다 증가시키는 시퀀스 카운터.
	 * 설정자: 매 송신마다 ++. 읽는 자: udp_seq.seq 기록용. */

	uint64_t udp_recv_seq;
	/* [한국어] UDP 수신에서 기대되는 다음 시퀀스 번호. 수신 패킷의 seq와 불일치 시
	 * 순서 뒤섞임/손실 감지. */
};

/*
 * [한국어] net 엔진 전용 옵션. 옵션 파서가 채우며 런타임 불변(잡 시작 후).
 * td->eo가 이 구조체를 가리킨다.
 */
struct netio_options {
	struct thread_data *td;
	/* [한국어] 옵션 파싱 콜백에서 td를 참조하기 위한 역링크.
	 * 설정자: 엔진 옵션 테이블 정의(offsetof 기반 자동 채움). */

	unsigned int port;
	/* [한국어] 접속/리슨 포트 번호(TCP/UDP). VSOCK에선 cid가 port로 재해석될 수 있음. */

	unsigned int proto;
	/* [한국어] 프로토콜 식별(FIO_TYPE_TCP/UDP/UNIX/TCP_V6/UDP_V6/VSOCK).
	 * 읽는 자: socket 호출의 family/type 결정, 분기 경로. */

	unsigned int listen;
	/* [한국어] 1이면 서버(수신) 모드. 0이면 클라이언트(송신). */

	unsigned int pingpong;
	/* [한국어] 1이면 각 I/O를 송신 후 수신(또는 그 역)으로 왕복 — 순수 RTT 측정에 사용. */

	unsigned int nodelay;
	/* [한국어] TCP_NODELAY — Nagle 알고리즘 끄기(저지연 필요 시). */

	unsigned int ttl;
	/* [한국어] 멀티캐스트 TTL 값(IP_MULTICAST_TTL). */

	unsigned int window_size;
	/* [한국어] SO_SNDBUF/SO_RCVBUF 크기(바이트). 0이면 커널 기본 사용. */

	unsigned int mss;
	/* [한국어] TCP_MAXSEG — 최대 세그먼트 크기 강제. */

	char *intfc;
	/* [한국어] SO_BINDTODEVICE로 소켓을 특정 네트워크 인터페이스에 바인딩할 때의 이름. */
};

/*
 * [한국어] UDP 연결 상태 신호 메시지(OPEN/CLOSE).
 * UDP는 연결 없는 프로토콜이므로 스트림 시작/종료를 애플리케이션 레벨 매직으로 알림.
 */
struct udp_close_msg {
	uint32_t magic;
	/* [한국어] 식별 매직(FIO_LINK_OPEN_CLOSE_MAGIC). 불일치 시 패킷 무시. */
	uint32_t cmd;
	/* [한국어] FIO_LINK_OPEN / FIO_LINK_CLOSE. 서버/클라이언트가 rendezvous/shutdown 신호로 사용. */
};

/*
 * [한국어] 각 UDP 패킷 앞에 붙이는 애플리케이션 레벨 헤더.
 * 수신 측이 순서/크기/무결성을 자체 검증.
 */
struct udp_seq {
	uint64_t magic;   /* [한국어] 매직(FIO_UDP_SEQ_MAGIC) */
	uint64_t seq;     /* [한국어] 송신 측이 단조 증가시키는 시퀀스 번호 */
	uint64_t bs;      /* [한국어] 본문 블록 크기(바이트) — 수신 검증용 */
};

enum {
	FIO_LINK_CLOSE = 0x89,
	FIO_LINK_OPEN_CLOSE_MAGIC = 0x6c696e6b,
	FIO_LINK_OPEN = 0x98,
	FIO_UDP_SEQ_MAGIC = 0x657375716e556563ULL,

	FIO_TYPE_TCP	= 1,
	FIO_TYPE_UDP	= 2,
	FIO_TYPE_UNIX	= 3,
	FIO_TYPE_TCP_V6	= 4,
	FIO_TYPE_UDP_V6	= 5,
	FIO_TYPE_VSOCK_STREAM   = 6,
};

/* [한국어] hostname= 옵션 파싱 콜백의 전방 선언. 아래 options[] 테이블에서 .cb로 참조되므로
 * 구현보다 먼저 이름이 필요하다. 실제 구현은 파일 하단(str_hostname_cb 정의)에 있음. */
static int str_hostname_cb(void *data, const char *input);
/*
 * [한국어] net/netsplice 엔진의 커맨드라인/잡파일 옵션 테이블.
 *  - fio의 옵션 파서(parse.c)가 이 배열을 순회하며 FIO_OPT_C_ENGINE/FIO_OPT_G_NETIO
 *    카테고리로 등록한다. .off1 은 struct netio_options 내부 필드 오프셋.
 *  - 각 옵션의 type(FIO_OPT_STR_STORE/INT/BOOL/STR/STR_SET)은 파서가 값을 해석하는 방식.
 *  - #ifdef CONFIG_* 로 플랫폼에서 지원 가능한 옵션만 노출된다.
 *  - 테이블은 반드시 .name = NULL 원소로 끝나야 파서가 종료를 인식한다. */
static struct fio_option options[] = {
	{
		.name	= "hostname",
		.lname	= "net engine hostname",
		.type	= FIO_OPT_STR_STORE,
		.cb	= str_hostname_cb,
		.help	= "Hostname for net IO engine",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "port",
		.lname	= "net engine port",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, port),
		.minval	= 1,
		.maxval	= 65535,
		.help	= "Port to use for TCP or UDP net connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "protocol",
		.lname	= "net engine protocol",
		.alias	= "proto",
		.type	= FIO_OPT_STR,
		.off1	= offsetof(struct netio_options, proto),
		.help	= "Network protocol to use",
		.def	= "tcp",
		.posval = {
			  { .ival = "tcp",
			    .oval = FIO_TYPE_TCP,
			    .help = "Transmission Control Protocol",
			  },
#ifdef CONFIG_IPV6
			  { .ival = "tcpv6",
			    .oval = FIO_TYPE_TCP_V6,
			    .help = "Transmission Control Protocol V6",
			  },
#endif
			  { .ival = "udp",
			    .oval = FIO_TYPE_UDP,
			    .help = "User Datagram Protocol",
			  },
#ifdef CONFIG_IPV6
			  { .ival = "udpv6",
			    .oval = FIO_TYPE_UDP_V6,
			    .help = "User Datagram Protocol V6",
			  },
#endif
			  { .ival = "unix",
			    .oval = FIO_TYPE_UNIX,
			    .help = "UNIX domain socket",
			  },
			  { .ival = "vsock",
			    .oval = FIO_TYPE_VSOCK_STREAM,
			    .help = "Virtual socket",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#ifdef CONFIG_TCP_NODELAY
	{
		.name	= "nodelay",
		.lname	= "No Delay",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct netio_options, nodelay),
		.help	= "Use TCP_NODELAY on TCP connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#endif
	{
		.name	= "listen",
		.lname	= "net engine listen",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct netio_options, listen),
		.help	= "Listen for incoming TCP connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "pingpong",
		.lname	= "Ping Pong",
		.type	= FIO_OPT_STR_SET,
		.off1	= offsetof(struct netio_options, pingpong),
		.help	= "Ping-pong IO requests",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "interface",
		.lname	= "net engine interface",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct netio_options, intfc),
		.help	= "Network interface to use",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "ttl",
		.lname	= "net engine multicast ttl",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, ttl),
		.def    = "1",
		.minval	= 0,
		.help	= "Time-to-live value for outgoing UDP multicast packets",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#ifdef CONFIG_NET_WINDOWSIZE
	{
		.name	= "window_size",
		.lname	= "Window Size",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, window_size),
		.minval	= 0,
		.help	= "Set socket buffer window size",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#endif
#ifdef CONFIG_NET_MSS
	{
		.name	= "mss",
		.lname	= "Maximum segment size",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, mss),
		.minval	= 0,
		.help	= "Set TCP maximum segment size",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#endif
	{
		.name	= NULL,
	},
};

/*
 * [한국어]
 * is_udp - 옵션의 proto가 UDP(v4/v6)인지 판별.
 * @o: 파싱된 netio_options.
 * @return: UDP면 1, 아니면 0.
 * caller: 엔진 전반(queue/recv/send/connect/accept/open_file 등)에서 분기용.
 * callee: 없음(단순 비교). 호출 체인: 엔진 콜백들 → [is_udp].
 */
static inline int is_udp(struct netio_options *o)
{
	/* [한국어] FIO_TYPE_UDP(v4) 또는 FIO_TYPE_UDP_V6 중 하나면 데이터그램 경로 사용. */
	return o->proto == FIO_TYPE_UDP || o->proto == FIO_TYPE_UDP_V6;
}

/*
 * [한국어]
 * is_tcp - 옵션의 proto가 TCP(v4/v6)인지 판별.
 * caller: nodelay/mss 적용 분기, connect 경로 등. callee: 없음.
 */
static inline int is_tcp(struct netio_options *o)
{
	/* [한국어] TCP v4 또는 v6 스트림 소켓 여부. */
	return o->proto == FIO_TYPE_TCP || o->proto == FIO_TYPE_TCP_V6;
}

/*
 * [한국어]
 * is_ipv6 - 프로토콜이 IPv6(TCP/UDP v6) 변종인지 판별.
 * caller: sockaddr_in6 경로 선택, AF_INET6 결정. callee: 없음.
 */
static inline int is_ipv6(struct netio_options *o)
{
	/* [한국어] v6 프로토콜이면 sockaddr_in6/AF_INET6 분기 사용. */
	return o->proto == FIO_TYPE_UDP_V6 || o->proto == FIO_TYPE_TCP_V6;
}

/*
 * [한국어]
 * is_vsock - 프로토콜이 VSOCK(VM 소켓)인지 판별.
 * caller: VSOCK 전용 주소 구조체/CID 처리 경로. callee: 없음.
 */
static inline int is_vsock(struct netio_options *o)
{
	/* [한국어] FIO_TYPE_VSOCK_STREAM 여부 — AF_VSOCK 사용 판단. */
	return o->proto == FIO_TYPE_VSOCK_STREAM;
}

/*
 * [한국어]
 * set_window_size - 소켓 송/수신 버퍼 크기(SO_SNDBUF/RCVBUF) 설정.
 * @td: 현재 잡 컨텍스트(에러 보고용 td_verror).
 * @fd: 설정 대상 소켓 FD.
 * @return: 0=성공/미적용, <0=에러.
 *
 * o->window_size 옵션이 설정된 경우에만 실제 setsockopt 수행. listen/pingpong 조합에 따라
 * 수신/송신 양쪽 모두 또는 한쪽만 설정한다(서버는 RCVBUF 중시, 클라는 SNDBUF 중시).
 * 컴파일 타임에 CONFIG_NET_WINDOWSIZE 미지원이면 에러 반환.
 *
 * 호출 체인: fio_netio_connect()/fio_netio_setup_listen_inet() → [set_window_size] → setsockopt(2).
 */
static int set_window_size(struct thread_data *td, int fd)
{
#ifdef CONFIG_NET_WINDOWSIZE
	struct netio_options *o = td->eo;            /* [한국어] 잡 옵션 포인터(window_size/listen/pingpong 사용). */
	unsigned int wss;                            /* [한국어] setsockopt에 넘길 버퍼 크기 값. */
	int snd, rcv, ret;                           /* [한국어] 송/수신 적용 플래그와 반환 코드. */

	if (!o->window_size)                         /* [한국어] 옵션 미지정이면 커널 기본값 유지. */
		return 0;

	rcv = o->listen || o->pingpong;              /* [한국어] 서버거나 pingpong이면 RCVBUF 설정 필요. */
	snd = !o->listen || o->pingpong;             /* [한국어] 클라거나 pingpong이면 SNDBUF 설정 필요. */
	wss = o->window_size;                        /* [한국어] 바이트 단위 버퍼 크기. */
	ret = 0;                                     /* [한국어] 기본 성공. */

	if (rcv) {                                   /* [한국어] 수신 버퍼 조정 경로. */
		ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *) &wss,
					sizeof(wss));    /* [한국어] 커널에 수신 큐 최대 크기 요청(실제는 커널이 2배로 증가시켜 저장). */
		if (ret < 0)                         /* [한국어] 실패 시 errno 기록. */
			td_verror(td, errno, "rcvbuf window size");
	}
	if (snd && !ret) {                           /* [한국어] 수신 설정 성공 후에만 송신 설정 진행. */
		ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void *) &wss,
					sizeof(wss));    /* [한국어] 송신 큐 최대 크기 요청. */
		if (ret < 0)
			td_verror(td, errno, "sndbuf window size");
	}

	return ret;                                  /* [한국어] 0/에러 전파. */
#else
	td_verror(td, -EINVAL, "setsockopt window size"); /* [한국어] 컴파일 타임 미지원 플랫폼. */
	return -1;
#endif
}

/*
 * [한국어]
 * set_mss - TCP 최대 세그먼트 크기(TCP_MAXSEG) 설정.
 * @td: 에러 보고 대상 잡 컨텍스트.
 * @fd: 대상 TCP 소켓 FD.
 * @return: 0=성공/비적용, <0=에러.
 *
 * o->mss 가 지정되고 프로토콜이 TCP일 때만 TCP_MAXSEG 를 설정한다. 이는 MTU 제약이 있는
 * 경로 튜닝 및 경계 케이스 재현용(fragmentation 실험 등). 호출 체인은 set_window_size 와 동일.
 */
static int set_mss(struct thread_data *td, int fd)
{
#ifdef CONFIG_NET_MSS
	struct netio_options *o = td->eo;            /* [한국어] 옵션. */
	unsigned int mss;                            /* [한국어] 설정할 MSS 값(바이트). */
	int ret;                                     /* [한국어] setsockopt 반환. */

	if (!o->mss || !is_tcp(o))                   /* [한국어] 미지정이거나 TCP가 아니면 무시. */
		return 0;

	mss = o->mss;                                /* [한국어] 옵션 값 복사(setsockopt이 포인터를 받기 때문에 로컬 저장 필요). */
	ret = setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, (void *) &mss,
				sizeof(mss));        /* [한국어] 커널에 TCP 최대 세그먼트 크기 강제 지정. */
	if (ret < 0)
		td_verror(td, errno, "setsockopt TCP_MAXSEG");

	return ret;
#else
	td_verror(td, -EINVAL, "setsockopt TCP_MAXSEG"); /* [한국어] 플랫폼 미지원 시 에러. */
	return -1;
#endif
}


/*
 * Return -1 for error and 'nr events' for a positive number
 * of events
 *
 * [한국어]
 * poll_wait - fd가 requested events 상태가 될 때까지 블로킹 대기.
 * @td: 잡 컨텍스트(종료 플래그 td->terminate 체크용).
 * @fd: 대기할 소켓/파이프 FD.
 * @events: 기다릴 이벤트 비트마스크(POLLIN/POLLOUT 등).
 * @return: 1 = 원하는 이벤트 발생, -1 = 에러 혹은 시그널로 중단.
 *
 * 동기 엔진(FIO_SYNCIO)이므로 fio_netio_send/recv/accept에서 블로킹 대기점으로 사용된다.
 * EINTR은 조용히 루프를 빠져나오고(시그널 재시도 상위에 위임), ret==0(타임아웃 없음, 사실상
 * 발생 불가)은 루프 재시도. td->terminate가 세트되면 즉시 탈출하여 잡 종료에 반응한다.
 *
 * 호출 체인: fio_netio_send/recv/accept → [poll_wait] → poll(2).
 */
static int poll_wait(struct thread_data *td, int fd, short events)
{
	struct pollfd pfd;                           /* [한국어] poll(2)에 전달할 단일 FD 엔트리. */
	int ret;                                     /* [한국어] poll 반환값(>0 이벤트 수, 0 타임아웃, <0 에러). */

	while (!td->terminate) {                     /* [한국어] 잡이 종료되지 않은 동안만 대기(fio 셧다운 경로 반응). */
		pfd.fd = fd;                         /* [한국어] 감시 대상 FD. */
		pfd.events = events;                 /* [한국어] 감시할 이벤트(POLLIN/POLLOUT 등). */
		ret = poll(&pfd, 1, -1);             /* [한국어] 무한 대기(-1) — 이벤트 또는 시그널이 올 때까지 블로킹. */
		if (ret < 0) {                       /* [한국어] 에러. */
			if (errno == EINTR)          /* [한국어] 시그널 인터럽트는 루프 탈출(상위에서 재시도/종료 판단). */
				break;

			td_verror(td, errno, "poll");/* [한국어] 그 외 에러는 기록. */
			return -1;
		} else if (!ret)                     /* [한국어] timeout=-1인데 0 반환은 이론상 없지만 방어적 재시도. */
			continue;

		break;                               /* [한국어] ret>0: 이벤트 도착. */
	}

	if (pfd.revents & events)                    /* [한국어] 요청한 이벤트가 revents에 실제로 올라왔는지 확인. */
		return 1;

	return -1;                                   /* [한국어] 종료 플래그 때문에 빠져나왔거나 이벤트 불일치 → 실패 간주. */
}

/*
 * [한국어]
 * fio_netio_is_multicast - 호스트 문자열이 IPv4 멀티캐스트(224.0.0.0/4) 주소인지 판별.
 * @mcaddr: 점-표기 IPv4 주소 문자열.
 * @return: 1=멀티캐스트 범위, 0=일반/무효.
 *
 * 멀티캐스트 주소이면 IP_ADD_MEMBERSHIP / IP_MULTICAST_IF 등의 추가 setsockopt 처리가 필요.
 * 호출 체인: fio_netio_connect/setup_listen_inet → [is_multicast] → inet_network(3).
 */
static int fio_netio_is_multicast(const char *mcaddr)
{
	in_addr_t addr = inet_network(mcaddr);       /* [한국어] 문자열을 호스트 바이트 순서 숫자로 변환. */
	if (addr == -1)                              /* [한국어] inet_network 실패 시 멀티캐스트 아님. */
		return 0;

	if (inet_network("224.0.0.0") <= addr &&     /* [한국어] IPv4 멀티캐스트 범위 하한 포함. */
	    inet_network("239.255.255.255") >= addr) /* [한국어] 상한(Class D 전체) 포함. */
		return 1;

	return 0;                                    /* [한국어] 범위를 벗어나면 일반 유니캐스트. */
}


/*
 * [한국어]
 * fio_netio_prep - net 엔진의 I/O 준비 콜백(ioengine_ops.prep).
 * @td: 잡 컨텍스트. @io_u: 곧 queue에 전달될 I/O 유닛.
 * @return: 0=정상, 1=방향 오류.
 *
 * UDP는 수신 서버(listen)가 WRITE를 내거나 송신 클라이언트(!listen)가 READ를 내는 경우를
 * 차단한다(데이터그램은 방향 고정). TCP는 전이중 스트림이므로 별도 검증 없이 통과.
 * 호출 체인: td_io_prep() → [fio_netio_prep].
 */
static int fio_netio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct netio_options *o = td->eo;            /* [한국어] 프로토콜/listen 등 옵션 참조. */

	/*
	 * Make sure we don't see spurious reads to a receiver, and vice versa
	 */
	if (is_tcp(o))                               /* [한국어] TCP는 양방향 스트림 — 방향 검사 불필요. */
		return 0;

	if ((o->listen && io_u->ddir == DDIR_WRITE) ||
	    (!o->listen && io_u->ddir == DDIR_READ)) {/* [한국어] 서버=수신, 클라=송신이라는 UDP 규약 위반 감지. */
		td_verror(td, EINVAL, "bad direction");
		return 1;
	}

	return 0;
}

#ifdef CONFIG_LINUX_SPLICE
/*
 * [한국어]
 * splice_io_u - len 바이트를 두 FD 사이에서 splice(2)로 제로카피 전송(루프).
 * @fdin: 입력 FD. @fdout: 출력 FD. @len: 총 전송 바이트.
 * @return: 전송된 바이트 수(>=0) 또는 splice가 한 번도 진행 못한 경우 splice의 음수 반환.
 *
 * splice(2)는 커널 파이프 버퍼를 거쳐 페이지 포인터만 이동시키므로 유저 공간 복사를 피한다.
 * 한 번의 호출이 len 전체를 처리하지 못할 수 있어 루프에서 잔여량을 반복 전송한다.
 * 호출 체인: splice_in/splice_out → [splice_io_u] → splice(2).
 */
static int splice_io_u(int fdin, int fdout, unsigned int len)
{
	int bytes = 0;                               /* [한국어] 누적 전송 바이트. */

	while (len) {                                /* [한국어] 잔여량이 있을 동안 반복. */
		int ret = splice(fdin, NULL, fdout, NULL, len, 0); /* [한국어] 커널 파이프 통한 페이지 이동; offset NULL=현재 위치 사용. */

		if (ret < 0) {                       /* [한국어] splice 에러. */
			if (!bytes)                  /* [한국어] 한 번도 진전이 없었다면 에러 코드 그대로 반환. */
				bytes = ret;

			break;
		} else if (!ret)                     /* [한국어] EOF/피어 종료 — 더 이상 이동 불가. */
			break;

		bytes += ret;                        /* [한국어] 진행량 누적. */
		len -= ret;                          /* [한국어] 남은 길이 감소. */
	}

	return bytes;                                /* [한국어] 총 전송량(혹은 에러). */
}

/*
 * Receive bytes from a socket and fill them into the internal pipe
 *
 * [한국어]
 * splice_in - 소켓에서 읽은 바이트를 내부 파이프(pipes[1])로 밀어넣음.
 * callee: splice_io_u. caller: fio_netio_splice_in.
 */
static int splice_in(struct thread_data *td, struct io_u *io_u)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 파이프 FD 얻기. */

	return splice_io_u(io_u->file->fd, nd->pipes[1], io_u->xfer_buflen); /* [한국어] 소켓→파이프 쓰기쪽. */
}

/*
 * Transmit 'len' bytes from the internal pipe
 *
 * [한국어]
 * splice_out - 파이프(pipes[0])에 들어 있는 데이터를 소켓으로 전송.
 * callee: splice_io_u. caller: fio_netio_splice_out.
 */
static int splice_out(struct thread_data *td, struct io_u *io_u,
		      unsigned int len)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 파이프 읽기 측 FD. */

	return splice_io_u(nd->pipes[0], io_u->file->fd, len); /* [한국어] 파이프 읽기쪽→소켓 쓰기. */
}

/*
 * [한국어]
 * vmsplice_io_u - 유저 메모리(io_u->xfer_buf)와 파이프 사이에서 vmsplice(2) 반복 수행.
 * @io_u: 버퍼 주인. @fd: 파이프 FD(read 또는 write 끝). @len: 총 바이트.
 * @return: 전송 바이트 수 또는 음수 에러.
 *
 * vmsplice는 유저 페이지를 파이프에 매핑하여 제로카피 이동을 수행한다(SPLICE_F_MOVE).
 * 호출 체인: vmsplice_io_u_in/out → [vmsplice_io_u] → vmsplice(2).
 */
static int vmsplice_io_u(struct io_u *io_u, int fd, unsigned int len)
{
	struct iovec iov = {                         /* [한국어] 유저 버퍼 세그먼트 기술. */
		.iov_base = io_u->xfer_buf,          /* [한국어] fio가 할당한 I/O 버퍼. */
		.iov_len = len,                      /* [한국어] 남은 이동 바이트. */
	};
	int bytes = 0;                               /* [한국어] 진행량 누적. */

	while (iov.iov_len) {                        /* [한국어] 모두 이동할 때까지 반복. */
		int ret = vmsplice(fd, &iov, 1, SPLICE_F_MOVE); /* [한국어] MOVE 플래그: 페이지 소유권 이전(복사 회피). */

		if (ret < 0) {                       /* [한국어] 시스템 호출 에러. */
			if (!bytes)
				bytes = ret;         /* [한국어] 완전 실패 시만 에러 값 반환. */
			break;
		} else if (!ret)                     /* [한국어] 더 이상 진행 불가. */
			break;

		iov.iov_len -= ret;                  /* [한국어] 남은 길이. */
		iov.iov_base += ret;                 /* [한국어] 버퍼 커서 전진. */
		bytes += ret;
	}

	return bytes;

}

/*
 * vmsplice() pipe to io_u buffer
 *
 * [한국어]
 * vmsplice_io_u_out - 파이프(pipes[0])의 데이터를 io_u 유저 버퍼로 가져옴(RX 마무리).
 */
static int vmsplice_io_u_out(struct thread_data *td, struct io_u *io_u,
			     unsigned int len)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 파이프 FD 배열. */

	return vmsplice_io_u(io_u, nd->pipes[0], len);/* [한국어] 파이프 읽기쪽에서 유저 버퍼로 매핑. */
}

/*
 * vmsplice() io_u to pipe
 *
 * [한국어]
 * vmsplice_io_u_in - io_u 유저 버퍼 → 파이프(pipes[1]) 쓰기쪽으로 주입(TX 준비).
 */
static int vmsplice_io_u_in(struct thread_data *td, struct io_u *io_u)
{
	struct netio_data *nd = td->io_ops_data;

	return vmsplice_io_u(io_u, nd->pipes[1], io_u->xfer_buflen); /* [한국어] 유저 버퍼→파이프 쓰기쪽. */
}

/*
 * splice receive - transfer socket data into a pipe using splice, then map
 * that pipe data into the io_u using vmsplice.
 *
 * [한국어]
 * fio_netio_splice_in - netsplice RX 경로: 소켓→파이프(splice) 이후 파이프→io_u(vmsplice).
 * @return: 수신 바이트 수 또는 에러.
 * 호출 체인: __fio_netio_queue(DDIR_READ) → [fio_netio_splice_in] → splice/vmsplice.
 */
static int fio_netio_splice_in(struct thread_data *td, struct io_u *io_u)
{
	int ret;

	ret = splice_in(td, io_u);                   /* [한국어] 1단계: 소켓 → 파이프. */
	if (ret > 0)
		return vmsplice_io_u_out(td, io_u, ret); /* [한국어] 2단계: 파이프 → 유저 버퍼(수신량만큼). */

	return ret;                                  /* [한국어] 0 또는 음수는 그대로 전파. */
}

/*
 * splice transmit - map data from the io_u into a pipe by using vmsplice,
 * then transfer that pipe to a socket using splice.
 *
 * [한국어]
 * fio_netio_splice_out - netsplice TX 경로: io_u→파이프(vmsplice) 이후 파이프→소켓(splice).
 * 호출 체인: __fio_netio_queue(DDIR_WRITE) → [fio_netio_splice_out].
 */
static int fio_netio_splice_out(struct thread_data *td, struct io_u *io_u)
{
	int ret;

	ret = vmsplice_io_u_in(td, io_u);            /* [한국어] 1단계: 유저 버퍼 → 파이프. */
	if (ret > 0)
		return splice_out(td, io_u, ret);    /* [한국어] 2단계: 파이프 → 소켓. */

	return ret;
}
#else
/*
 * [한국어]
 * fio_netio_splice_in (splice 미지원 빌드) - EOPNOTSUPP 리턴하는 스텁.
 */
static int fio_netio_splice_in(struct thread_data *td, struct io_u *io_u)
{
	errno = EOPNOTSUPP;                          /* [한국어] 상위에서 에러 경로로 분기. */
	return -1;
}

/*
 * [한국어]
 * fio_netio_splice_out (splice 미지원 빌드) - 동일하게 EOPNOTSUPP 스텁.
 */
static int fio_netio_splice_out(struct thread_data *td, struct io_u *io_u)
{
	errno = EOPNOTSUPP;
	return -1;
}
#endif

/*
 * [한국어]
 * store_udp_seq - UDP 송신 버퍼 끝에 udp_seq(magic/bs/seq) 헤더 삽입.
 * @nd: 송신 시퀀스 카운터를 보관하는 잡별 상태.
 * @io_u: 전송 대상(페이로드 끝에 헤더를 덮어씌움).
 *
 * 버퍼 끝(뒤쪽 sizeof(udp_seq))에 리틀엔디언 정렬로 기록하여 수신 측 verify_udp_seq에서 동일
 * 위치로 해석한다. 버퍼가 너무 작으면(헤더보다 작으면) skip. 매 호출마다 udp_send_seq 증가.
 * 호출 체인: fio_netio_send() → [store_udp_seq].
 */
static void store_udp_seq(struct netio_data *nd, struct io_u *io_u)
{
	struct udp_seq *us;                          /* [한국어] 버퍼 끝에 오버레이할 헤더 포인터. */

	if (io_u->xfer_buflen < sizeof(*us))         /* [한국어] 헤더가 들어갈 공간이 없으면 포기. */
		return;

	us = io_u->xfer_buf + io_u->xfer_buflen - sizeof(*us); /* [한국어] 버퍼 끝-헤더크기 지점. */
	us->magic = cpu_to_le64((uint64_t) FIO_UDP_SEQ_MAGIC); /* [한국어] 매직 값 LE로 기록. */
	us->bs = cpu_to_le64((uint64_t) io_u->xfer_buflen);   /* [한국어] 본 페이로드 길이 기록. */
	us->seq = cpu_to_le64(nd->udp_send_seq++);            /* [한국어] 현재 시퀀스 기록 후 증가. */
}

/*
 * [한국어]
 * verify_udp_seq - 수신 측에서 udp_seq 헤더를 검증하고 드롭 카운트를 누적.
 * @td: 통계 기록 대상. @nd: 수신 시퀀스 추적. @io_u: 방금 수신된 데이터그램.
 *
 * 매직/길이 불일치 시 검증을 건너뛰거나(seq_off=1로 영구 비활성화) 무시한다. seq가 기대값보다
 * 앞서 있으면 그 차이만큼 drop_io_u 통계에 누적(순서는 UDP 특성상 손실/재정렬 구분 X).
 * 호출 체인: fio_netio_recv() → [verify_udp_seq].
 */
static void verify_udp_seq(struct thread_data *td, struct netio_data *nd,
			   struct io_u *io_u)
{
	struct udp_seq *us;                          /* [한국어] 수신 버퍼 끝 헤더. */
	uint64_t seq;                                /* [한국어] 호스트 엔디언 시퀀스. */

	if (io_u->xfer_buflen < sizeof(*us))         /* [한국어] 헤더 크기보다 작으면 검증 불가. */
		return;

	if (nd->seq_off)                             /* [한국어] 한 번 틀린 블록 크기 감지된 잡은 이후 검증 끔. */
		return;

	us = io_u->xfer_buf + io_u->xfer_buflen - sizeof(*us); /* [한국어] 헤더 오프셋. */
	if (le64_to_cpu(us->magic) != FIO_UDP_SEQ_MAGIC) /* [한국어] 매직 불일치면 타 송신자/버전 — 무시. */
		return;
	if (le64_to_cpu(us->bs) != io_u->xfer_buflen) {  /* [한국어] 송신 측 블록 크기가 다르면 이후 전체 검증 비활성. */
		nd->seq_off = 1;
		return;
	}

	seq = le64_to_cpu(us->seq);                  /* [한국어] 수신한 시퀀스 번호 추출. */

	if (seq != nd->udp_recv_seq)                 /* [한국어] 기대값과 다르면 점프한 만큼 드롭으로 간주. */
		td->ts.drop_io_u[io_u->ddir] += seq - nd->udp_recv_seq;

	nd->udp_recv_seq = seq + 1;                  /* [한국어] 다음 기대 시퀀스 갱신. */
}

/*
 * [한국어]
 * fio_netio_send - UDP/TCP/UNIX에 대해 단일 io_u를 송신하는 저수준 루틴.
 * @td: 잡 컨텍스트. @io_u: 송신 버퍼(xfer_buf/xfer_buflen 사용).
 * @return: 송신 바이트 수(>0) 또는 에러.
 *
 * UDP는 sendto(2)로 저장해둔 peer 주소에 직접 전송하며, verify 미사용 시 udp_seq 헤더를 끼워
 * 넣는다. TCP/UNIX는 send(2)로 전송, 마지막 패킷이 아니면 MSG_MORE로 Nagle 병합 힌트. 송신이
 * 부분/실패이면 POLLOUT poll_wait 후 재시도.
 *
 * 호출 체인: __fio_netio_queue(DDIR_WRITE) → [fio_netio_send] → send(2)/sendto(2)/poll(2).
 */
static int fio_netio_send(struct thread_data *td, struct io_u *io_u)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 소켓 주소/상태 보관. */
	struct netio_options *o = td->eo;            /* [한국어] 프로토콜/pingpong 등 옵션. */
	int ret, flags = 0;                          /* [한국어] ret: send 반환, flags: send 플래그 누적. */

	do {
		if (is_udp(o)) {                     /* [한국어] UDP 경로: 데이터그램 목적지 필요. */
			const struct sockaddr *to;   /* [한국어] sendto에 전달할 peer 주소 포인터. */
			socklen_t len;               /* [한국어] 주소 구조체 길이. */

			if (is_ipv6(o)) {            /* [한국어] v6 주소 선택. */
				to = (struct sockaddr *) &nd->addr6;
				len = sizeof(nd->addr6);
			} else {
				to = (struct sockaddr *) &nd->addr; /* [한국어] v4 주소. */
				len = sizeof(nd->addr);
			}

			if (td->o.verify == VERIFY_NONE) /* [한국어] verify가 페이로드를 직접 검증하면 udp_seq 생략. */
				store_udp_seq(nd, io_u);

			ret = sendto(io_u->file->fd, io_u->xfer_buf,
					io_u->xfer_buflen, flags, to, len); /* [한국어] 커널 UDP 전송(IP 레이어에서 체크섬/프래그). */
		} else {
			/*
			 * if we are going to write more, set MSG_MORE
			 */
#ifdef MSG_MORE
			if ((td->this_io_bytes[DDIR_WRITE] + io_u->xfer_buflen <
			    td->o.size) && !o->pingpong) /* [한국어] 마지막 전송이 아니고 pingpong 아니면 다음 패킷 합치기 힌트. */
				flags |= MSG_MORE;
#endif
			ret = send(io_u->file->fd, io_u->xfer_buf,
					io_u->xfer_buflen, flags); /* [한국어] TCP/UNIX 스트림 송신(커널이 분절/합치기 관리). */
		}
		if (ret > 0)                         /* [한국어] 진전이 있었으면 루프 탈출. */
			break;

		ret = poll_wait(td, io_u->file->fd, POLLOUT); /* [한국어] 0/음수: 소켓이 write 가능해질 때까지 대기. */
		if (ret <= 0)                        /* [한국어] 대기 실패/종료면 포기. */
			break;
	} while (1);

	return ret;
}

/*
 * [한국어]
 * is_close_msg - 수신된 데이터그램이 udp_close_msg(LINK_CLOSE)인지 판정.
 * @io_u: 방금 수신된 버퍼. @len: 실제 수신 바이트.
 * @return: 1이면 CLOSE 메시지, 0이면 일반 페이로드.
 *
 * UDP는 연결 개념이 없어 스트림 종료를 애플리케이션 레벨에서 알려야 한다. 길이/매직/cmd가 모두
 * 일치해야 CLOSE로 인정. 호출 체인: fio_netio_recv() → [is_close_msg].
 */
static int is_close_msg(struct io_u *io_u, int len)
{
	struct udp_close_msg *msg;                   /* [한국어] 버퍼 선두를 CLOSE 메시지로 오버레이. */

	if (len != sizeof(struct udp_close_msg))     /* [한국어] 크기 자체가 다르면 일반 데이터. */
		return 0;

	msg = io_u->xfer_buf;                        /* [한국어] 페이로드 시작을 msg로 해석. */
	if (le32_to_cpu(msg->magic) != FIO_LINK_OPEN_CLOSE_MAGIC) /* [한국어] 매직 불일치. */
		return 0;
	if (le32_to_cpu(msg->cmd) != FIO_LINK_CLOSE) /* [한국어] OPEN/기타 cmd는 CLOSE가 아님. */
		return 0;

	return 1;
}

/*
 * [한국어]
 * fio_netio_recv - 단일 io_u 크기만큼 소켓에서 수신.
 * @return: 수신 바이트 수, 0(상대가 CLOSE 표명), 음수(에러).
 *
 * UDP는 listen 측에서 recvfrom으로 peer 주소 캡처(다음 응답 송신용), 그 외는 recv 사용. CLOSE
 * 메시지 수신 시 td->done=1로 잡 종료 신호. MSG_WAITALL 재시도로 짧은 수신을 메꾼다.
 * 호출 체인: __fio_netio_queue(DDIR_READ) → [fio_netio_recv] → recv(2)/recvfrom(2)/poll(2).
 */
static int fio_netio_recv(struct thread_data *td, struct io_u *io_u)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 주소/상태 공유 블록. */
	struct netio_options *o = td->eo;            /* [한국어] 프로토콜/listen 여부. */
	int ret, flags = 0;                          /* [한국어] flags는 초반 0, 재시도 시 MSG_WAITALL 추가. */

	do {
		if (is_udp(o)) {
			struct sockaddr *from;       /* [한국어] 서버 모드에서 peer 주소 수신. */
			socklen_t l, *len = &l;      /* [한국어] recvfrom inout 길이 포인터. */

			if (o->listen) {             /* [한국어] 서버: 응답을 위해 peer 주소 수집. */
				if (!is_ipv6(o)) {
					from = (struct sockaddr *) &nd->addr;
					*len = sizeof(nd->addr);
				} else {
					from = (struct sockaddr *) &nd->addr6;
					*len = sizeof(nd->addr6);
				}
			} else {
				from = NULL;         /* [한국어] 클라이언트는 주소 필요 없음. */
				len = NULL;
			}

			ret = recvfrom(io_u->file->fd, io_u->xfer_buf,
					io_u->xfer_buflen, flags, from, len); /* [한국어] UDP 수신. */

			if (is_close_msg(io_u, ret)) {/* [한국어] 상대가 링크 종료를 알렸는가? */
				td->done = 1;        /* [한국어] 잡 메인 루프에 완료 신호. */
				return 0;
			}
		} else {
			ret = recv(io_u->file->fd, io_u->xfer_buf,
					io_u->xfer_buflen, flags); /* [한국어] TCP/UNIX 스트림 수신. */

			if (is_close_msg(io_u, ret)) {
				td->done = 1;
				return 0;
			}
		}
		if (ret > 0)                         /* [한국어] 수신 진전 — 루프 탈출. */
			break;
		else if (!ret && (flags & MSG_WAITALL)) /* [한국어] WAITALL인데 0이면 상대가 정상 종료. */
			break;

		ret = poll_wait(td, io_u->file->fd, POLLIN); /* [한국어] 데이터 도착까지 블록. */
		if (ret <= 0)
			break;
		flags |= MSG_WAITALL;                /* [한국어] 재시도 시 전량 수신까지 커널이 누적하도록 요청. */
	} while (1);

	if (is_udp(o) && td->o.verify == VERIFY_NONE) /* [한국어] verify 없으면 자체 시퀀스 검증 수행. */
		verify_udp_seq(td, nd, io_u);

	return ret;
}

/*
 * [한국어]
 * __fio_netio_queue - 내부 방향별 I/O 제출 로직.
 * @td: 잡. @io_u: I/O 유닛. @ddir: 요청 방향(READ/WRITE/SYNC).
 * @return: fio_q_status (COMPLETED/BUSY).
 *
 * use_splice/UDP/UNIX 조건에 따라 send/recv 또는 splice_in/out 경로로 분기. 부분 전송 시
 * resid 기록(짧은 전송)으로 COMPLETED 반환. EMSGSIZE는 큰 UDP 데이터그램을 줄여서 재시도
 * 하도록 BUSY로 상위에 돌려준다.
 *
 * 호출 체인: fio_netio_queue() → [__fio_netio_queue] → fio_netio_{send,recv,splice_*}().
 */
static enum fio_q_status __fio_netio_queue(struct thread_data *td,
					   struct io_u *io_u,
					   enum fio_ddir ddir)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] splice 사용 여부. */
	struct netio_options *o = td->eo;            /* [한국어] proto 참조. */
	int ret;                                     /* [한국어] 저수준 전송량/에러. */

	if (ddir == DDIR_WRITE) {
		if (!nd->use_splice || is_udp(o) ||
		    o->proto == FIO_TYPE_UNIX)       /* [한국어] splice 미사용이거나 UDP/UNIX면 일반 send. */
			ret = fio_netio_send(td, io_u);
		else
			ret = fio_netio_splice_out(td, io_u); /* [한국어] TCP+splice 경로. */
	} else if (ddir == DDIR_READ) {
		if (!nd->use_splice || is_udp(o) ||
		    o->proto == FIO_TYPE_UNIX)
			ret = fio_netio_recv(td, io_u);
		else
			ret = fio_netio_splice_in(td, io_u);
	} else
		ret = 0;	/* must be a SYNC */ /* [한국어] 네트워크 엔진에서 SYNC는 no-op. */

	if (ret != (int) io_u->xfer_buflen) {        /* [한국어] 요청한 만큼 전송/수신되지 않았다면 후처리. */
		if (ret > 0) {                       /* [한국어] 부분 처리 — short transfer. */
			io_u->resid = io_u->xfer_buflen - ret; /* [한국어] 남은 바이트 통계용. */
			io_u->error = 0;
			return FIO_Q_COMPLETED;
		} else if (!ret)                     /* [한국어] 진전 전혀 없음 — 재시도 요청. */
			return FIO_Q_BUSY;
		else {
			int err = errno;             /* [한국어] 저수준에서 설정된 errno 스냅샷. */

			if (ddir == DDIR_WRITE && err == EMSGSIZE) /* [한국어] UDP 최대 데이터그램 초과 — 축소 후 재시도. */
				return FIO_Q_BUSY;

			io_u->error = err;
		}
	}

	if (io_u->error)                             /* [한국어] 오류가 있으면 상세 기록. */
		td_verror(td, io_u->error, "xfer");

	return FIO_Q_COMPLETED;                      /* [한국어] 단일 호출로 완료되는 동기 엔진. */
}

/*
 * [한국어]
 * fio_netio_queue - net 엔진의 I/O 제출 콜백
 *
 * 읽기는 recv/recvfrom/splice, 쓰기는 send/sendto/vmsplice로 전송한다.
 * pingpong 모드에서는 쓰기 후 읽기(또는 반대)를 자동으로 수행한다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → send(2)/recv(2)/splice(2)
 */
static enum fio_q_status fio_netio_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	struct netio_options *o = td->eo;            /* [한국어] pingpong 여부 확인. */
	int ret;                                     /* [한국어] 1차 queue 결과. */

	fio_ro_check(td, io_u);                      /* [한국어] readonly 잡에서 write io_u 차단 공통 가드. */

	ret = __fio_netio_queue(td, io_u, io_u->ddir);/* [한국어] 1차 방향 실행. */
	if (!o->pingpong || ret != FIO_Q_COMPLETED)  /* [한국어] pingpong 아니거나 실패면 여기서 종료. */
		return ret;

	/*
	 * For ping-pong mode, receive or send reply as needed
	 */
	if (td_read(td) && io_u->ddir == DDIR_READ)  /* [한국어] 수신 후에는 동일 버퍼를 돌려보냄(에코). */
		ret = __fio_netio_queue(td, io_u, DDIR_WRITE);
	else if (td_write(td) && io_u->ddir == DDIR_WRITE) /* [한국어] 송신 후에는 응답 수신으로 RTT 측정. */
		ret = __fio_netio_queue(td, io_u, DDIR_READ);

	return ret;
}

/*
 * [한국어]
 * fio_netio_connect - 클라이언트 모드에서 프로토콜별 소켓을 생성하고 서버에 연결.
 * @td: 잡. @f: 해당 연결을 표현할 fio_file(엔진 내부에서 f->fd 채움).
 * @return: 0=성공, 1=실패(에러 보고 후).
 *
 * TCP/TCP_V6/UDP/UDP_V6/UNIX/VSOCK 각각에 대해 domain/type을 결정 후 socket(2)을 호출.
 * TCP_NODELAY/window_size/MSS 옵션 반영. UDP는 연결 없이 bind만 하거나 멀티캐스트 멤버십
 * 설정. 그 외(UNIX/TCP/VSOCK)는 connect(2)로 서버 rendezvous.
 *
 * 호출 체인: fio_netio_open_file → [fio_netio_connect] → socket/connect/setsockopt.
 */
static int fio_netio_connect(struct thread_data *td, struct fio_file *f)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 목적지 주소 구조체 보관. */
	struct netio_options *o = td->eo;            /* [한국어] 옵션(proto/nodelay/...). */
	int type, domain;                            /* [한국어] socket(2) 인자. */

	if (o->proto == FIO_TYPE_TCP) {              /* [한국어] TCP v4: 스트림+AF_INET. */
		domain = AF_INET;
		type = SOCK_STREAM;
	} else if (o->proto == FIO_TYPE_TCP_V6) {    /* [한국어] TCP v6: 스트림+AF_INET6. */
		domain = AF_INET6;
		type = SOCK_STREAM;
	} else if (o->proto == FIO_TYPE_UDP) {       /* [한국어] UDP v4: 데이터그램. */
		domain = AF_INET;
		type = SOCK_DGRAM;
	} else if (o->proto == FIO_TYPE_UDP_V6) {    /* [한국어] UDP v6. */
		domain = AF_INET6;
		type = SOCK_DGRAM;
	} else if (o->proto == FIO_TYPE_UNIX) {      /* [한국어] Unix 도메인 스트림 소켓. */
		domain = AF_UNIX;
		type = SOCK_STREAM;
	} else if (is_vsock(o)) {                    /* [한국어] VM 소켓(guest/host 간). */
		domain = AF_VSOCK;
		type = SOCK_STREAM;
	} else {
		log_err("fio: bad network type %d\n", o->proto); /* [한국어] 잘못된 proto 값. */
		f->fd = -1;
		return 1;
	}

	f->fd = socket(domain, type, 0);             /* [한국어] 지정 family/type으로 소켓 생성(커널 fd 할당). */
	if (f->fd < 0) {
		td_verror(td, errno, "socket");
		return 1;
	}

#ifdef CONFIG_TCP_NODELAY
	if (o->nodelay && is_tcp(o)) {               /* [한국어] TCP인 경우에만 Nagle 비활성화 옵션 적용. */
		int optval = 1;

		if (setsockopt(f->fd, IPPROTO_TCP, TCP_NODELAY, (void *) &optval, sizeof(int)) < 0) {
			log_err("fio: cannot set TCP_NODELAY option on socket (%s), disable with 'nodelay=0'\n", strerror(errno));
			return 1;
		}
	}
#endif

	if (set_window_size(td, f->fd)) {            /* [한국어] SO_SNDBUF/RCVBUF 크기 적용. */
		close(f->fd);
		return 1;
	}
	if (set_mss(td, f->fd)) {                    /* [한국어] TCP MSS 적용. */
		close(f->fd);
		return 1;
	}

	if (is_udp(o)) {                             /* [한국어] UDP는 connect 없이 전송 — 멀티캐스트 확인 경로 분기. */
		if (!fio_netio_is_multicast(td->o.filename))
			return 0;                    /* [한국어] 일반 유니캐스트 UDP면 추가 설정 없이 완료. */
		if (is_ipv6(o)) {                    /* [한국어] 현재 구현은 IPv6 멀티캐스트 미지원. */
			log_err("fio: multicast not supported on IPv6\n");
			close(f->fd);
			return 1;
		}

		if (o->intfc) {                      /* [한국어] 특정 인터페이스로 송출 강제(intfc 옵션). */
			struct in_addr interface_addr;

			if (inet_aton(o->intfc, &interface_addr) == 0) { /* [한국어] 텍스트 IP → 이진 변환. */
				log_err("fio: interface not valid interface IP\n");
				close(f->fd);
				return 1;
			}
			if (setsockopt(f->fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&interface_addr, sizeof(interface_addr)) < 0) {
				td_verror(td, errno, "setsockopt IP_MULTICAST_IF"); /* [한국어] 멀티캐스트 아웃바운드 인터페이스 지정 실패. */
				close(f->fd);
				return 1;
			}
		}
		if (setsockopt(f->fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&o->ttl, sizeof(o->ttl)) < 0) {
			td_verror(td, errno, "setsockopt IP_MULTICAST_TTL"); /* [한국어] 멀티캐스트 TTL(라우터 홉 수) 제한 설정. */
			close(f->fd);
			return 1;
		}
		return 0;
	} else if (o->proto == FIO_TYPE_TCP) {
		socklen_t len = sizeof(nd->addr);

		if (connect(f->fd, (struct sockaddr *) &nd->addr, len) < 0) { /* [한국어] TCP 3-way handshake 수행. */
			td_verror(td, errno, "connect");
			close(f->fd);
			return 1;
		}
	} else if (o->proto == FIO_TYPE_TCP_V6) {
		socklen_t len = sizeof(nd->addr6);

		if (connect(f->fd, (struct sockaddr *) &nd->addr6, len) < 0) { /* [한국어] IPv6 TCP 연결. */
			td_verror(td, errno, "connect");
			close(f->fd);
			return 1;
		}
	} else if (is_vsock(o)) {
		socklen_t len = sizeof(nd->addr_vm);

		if (connect(f->fd, (struct sockaddr *) &nd->addr_vm, len) < 0) { /* [한국어] VSOCK cid/port로 연결. */
			td_verror(td, errno, "connect");
			close(f->fd);
			return 1;
		}
	} else {
		struct sockaddr_un *addr = &nd->addr_un; /* [한국어] Unix 도메인 소켓 경로. */
		socklen_t len;

		len = sizeof(addr->sun_family) + strlen(addr->sun_path) + 1; /* [한국어] family 필드 + 문자열 + NUL 길이. */

		if (connect(f->fd, (struct sockaddr *) addr, len) < 0) { /* [한국어] AF_UNIX 서버 소켓 파일에 연결. */
			td_verror(td, errno, "connect");
			close(f->fd);
			return 1;
		}
	}

	return 0;
}

/*
 * [한국어]
 * fio_netio_accept - 서버 모드에서 listen FD로부터 클라이언트 연결을 수락.
 * @td: 잡. @f: 수락한 연결을 담을 fio_file.
 * @return: 0=성공, 1=실패.
 *
 * UDP는 accept 없이 listenfd 자체를 f->fd로 사용. TCP/VSOCK/TCPv6는 poll_wait 후 accept(2).
 * TD_SETTING_UP 상태로 잠시 바꿔 런타임 통계에 대기 시간이 섞이지 않도록 하고, 연결 직후
 * reset_all_stats()로 깨끗한 시작점을 만든다.
 *
 * 호출 체인: fio_netio_open_file → [fio_netio_accept] → accept(2)/poll(2).
 */
static int fio_netio_accept(struct thread_data *td, struct fio_file *f)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] listenfd/주소. */
	struct netio_options *o = td->eo;            /* [한국어] 옵션. */
	socklen_t socklen;                           /* [한국어] accept inout 길이. */
	int state;                                   /* [한국어] 이전 runstate 백업. */

	if (is_udp(o)) {                             /* [한국어] UDP: connectionless — listenfd를 그대로 사용. */
		f->fd = nd->listenfd;
		return 0;
	}

	state = td->runstate;                        /* [한국어] 현재 런상태 저장(복원용). */
	td_set_runstate(td, TD_SETTING_UP);          /* [한국어] 통계상 "설정 중" 구간으로 표시. */

	log_info("fio: waiting for connection\n");   /* [한국어] 사용자에게 대기 알림. */

	if (poll_wait(td, nd->listenfd, POLLIN) < 0) /* [한국어] 연결 요청 도착까지 대기. */
		goto err;

	if (o->proto == FIO_TYPE_TCP) {              /* [한국어] IPv4 TCP accept. */
		socklen = sizeof(nd->addr);
		f->fd = accept(nd->listenfd, (struct sockaddr *) &nd->addr, &socklen);
	} else if (is_vsock(o)) {                    /* [한국어] VSOCK accept. */
		socklen = sizeof(nd->addr_vm);
		f->fd = accept(nd->listenfd, (struct sockaddr *) &nd->addr_vm, &socklen);
	} else {                                     /* [한국어] IPv6 TCP accept(기본 분기). */
		socklen = sizeof(nd->addr6);
		f->fd = accept(nd->listenfd, (struct sockaddr *) &nd->addr6, &socklen);
	}

	if (f->fd < 0) {                             /* [한국어] accept 실패. */
		td_verror(td, errno, "accept");
		goto err;
	}

#ifdef CONFIG_TCP_NODELAY
	if (o->nodelay && is_tcp(o)) {               /* [한국어] 수락한 연결에도 NODELAY 적용. */
		int optval = 1;

		if (setsockopt(f->fd, IPPROTO_TCP, TCP_NODELAY, (void *) &optval, sizeof(int)) < 0) {
			log_err("fio: cannot set TCP_NODELAY option on socket (%s), disable with 'nodelay=0'\n", strerror(errno));
			return 1;
		}
	}
#endif

	reset_all_stats(td);                         /* [한국어] 연결 대기 동안 누적된 통계 제거. */
	td_set_runstate(td, state);                  /* [한국어] 원래 runstate 복원. */
	return 0;
err:
	td_set_runstate(td, state);                  /* [한국어] 에러 경로에서도 runstate 복원. */
	return 1;
}

/*
 * [한국어]
 * fio_netio_send_close - UDP 링크 종료를 peer에 알리는 udp_close_msg 전송.
 * 연결 없는 UDP에서 상대가 read를 언제까지 반복할지 모르므로 애플리케이션 레벨 EOF 역할.
 * 호출 체인: fio_netio_close_file → [send_close] → sendto(2).
 */
static void fio_netio_send_close(struct thread_data *td, struct fio_file *f)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 저장된 peer 주소. */
	struct netio_options *o = td->eo;
	struct udp_close_msg msg;                    /* [한국어] CLOSE 시그널 메시지. */
	struct sockaddr *to;                         /* [한국어] 목적지 주소 포인터. */
	socklen_t len;                               /* [한국어] 주소 길이. */
	int ret;

	if (is_ipv6(o)) {                            /* [한국어] v6 목적지. */
		to = (struct sockaddr *) &nd->addr6;
		len = sizeof(nd->addr6);
	} else if (is_vsock(o)) {                    /* [한국어] VSOCK은 연결형(스트림)이라 별도 to 불필요. */
		to = NULL;
		len = 0;
	} else {                                     /* [한국어] v4 기본. */
		to = (struct sockaddr *) &nd->addr;
		len = sizeof(nd->addr);
	}

	msg.magic = cpu_to_le32((uint32_t) FIO_LINK_OPEN_CLOSE_MAGIC); /* [한국어] CLOSE 구분용 매직. */
	msg.cmd = cpu_to_le32((uint32_t) FIO_LINK_CLOSE);              /* [한국어] cmd=CLOSE. */

	ret = sendto(f->fd, (void *) &msg, sizeof(msg), MSG_WAITALL, to, len); /* [한국어] 마지막 바이트까지 송신. */
	if (ret < 0)
		td_verror(td, errno, "sendto udp link close");
}

/*
 * [한국어]
 * fio_netio_close_file - fio가 파일(=연결)을 닫을 때 호출되는 엔진 콜백.
 * UDP/TCP 모두 우선 CLOSE 시그널을 보낸 후, 공통 generic_close_file로 FD를 정리한다.
 * 호출 체인: td_io_close_file() → [close_file] → send_close + generic_close_file.
 */
static int fio_netio_close_file(struct thread_data *td, struct fio_file *f)
{
	/*
	 * Notify the receiver that we are closing down the link
	 */
	fio_netio_send_close(td, f);                 /* [한국어] 상대에게 종료 알림. */

	return generic_close_file(td, f);            /* [한국어] 공통 close + 통계 처리. */
}

/*
 * [한국어]
 * fio_netio_udp_recv_open - 서버 측 UDP 수신 잡에서 클라이언트의 LINK_OPEN 메시지를 기다림.
 * 첫 OPEN 메시지를 받은 순간 td->start 시점을 지정하여 정확한 시작 시간 측정.
 * 호출 체인: fio_netio_open_file(UDP, !td_write) → [udp_recv_open] → recvfrom(2).
 */
static int fio_netio_udp_recv_open(struct thread_data *td, struct fio_file *f)
{
	struct netio_data *nd = td->io_ops_data;
	struct netio_options *o = td->eo;
	struct udp_close_msg msg;                    /* [한국어] OPEN 메시지도 동일 구조체 재활용. */
	struct sockaddr *to;                         /* [한국어] recvfrom 결과로 채워질 송신자 주소. */
	socklen_t len;
	int ret;

	if (is_ipv6(o)) {                            /* [한국어] v6 주소 슬롯 사용. */
		len = sizeof(nd->addr6);
		to = (struct sockaddr *) &nd->addr6;
	} else {
		len = sizeof(nd->addr);
		to = (struct sockaddr *) &nd->addr;
	}

	ret = recvfrom(f->fd, (void *) &msg, sizeof(msg), MSG_WAITALL, to, &len); /* [한국어] OPEN 메시지 수신(차단). */
	if (ret < 0) {
		td_verror(td, errno, "recvfrom udp link open");
		return ret;
	}

	if (ntohl(msg.magic) != FIO_LINK_OPEN_CLOSE_MAGIC ||
	    ntohl(msg.cmd) != FIO_LINK_OPEN) {       /* [한국어] OPEN 매직/커맨드 검증(네트워크 바이트 순서). */
		log_err("fio: bad udp open magic %x/%x\n",
			(unsigned int) ntohl(msg.magic),
			(unsigned int) ntohl(msg.cmd));
		return -1;
	}

	fio_gettime(&td->start, NULL);               /* [한국어] 측정 시작 시각 고정. */
	return 0;
}

/*
 * [한국어]
 * fio_netio_send_open - 클라이언트 UDP 송신 잡에서 서버에 LINK_OPEN 핸드쉐이크 전송.
 * 호출 체인: fio_netio_open_file(UDP, td_write) → [send_open] → sendto(2).
 */
static int fio_netio_send_open(struct thread_data *td, struct fio_file *f)
{
	struct netio_data *nd = td->io_ops_data;
	struct netio_options *o = td->eo;
	struct udp_close_msg msg;
	struct sockaddr *to;
	socklen_t len;
	int ret;

	if (is_ipv6(o)) {                            /* [한국어] v6 주소 선택. */
		len = sizeof(nd->addr6);
		to = (struct sockaddr *) &nd->addr6;
	} else if (is_vsock(o)) {                    /* [한국어] VSOCK 주소. */
		len = sizeof(nd->addr_vm);
		to = (struct sockaddr *) &nd->addr_vm;
	} else {                                     /* [한국어] IPv4. */
		len = sizeof(nd->addr);
		to = (struct sockaddr *) &nd->addr;
	}

	msg.magic = htonl(FIO_LINK_OPEN_CLOSE_MAGIC);/* [한국어] OPEN 매직(네트워크 바이트 순서). */
	msg.cmd = htonl(FIO_LINK_OPEN);              /* [한국어] cmd=OPEN. */

	ret = sendto(f->fd, (void *) &msg, sizeof(msg), MSG_WAITALL, to, len); /* [한국어] 서버에 OPEN 송신. */
	if (ret < 0) {
		td_verror(td, errno, "sendto udp link open");
		return ret;
	}

	return 0;
}

/*
 * [한국어]
 * fio_netio_open_file - 잡 시작 시 실제 네트워크 연결/바인딩을 수행하는 엔진 콜백.
 * listen 옵션이면 accept, 아니면 connect. UDP의 경우 OPEN 핸드쉐이크까지 수행.
 * @return: 0=성공, 그 외=에러(자동으로 close 호출).
 * 호출 체인: td_io_open_file → [open_file] → accept/connect + UDP OPEN.
 */
static int fio_netio_open_file(struct thread_data *td, struct fio_file *f)
{
	int ret;
	struct netio_options *o = td->eo;

	if (o->listen)                               /* [한국어] 서버 모드 경로. */
		ret = fio_netio_accept(td, f);
	else                                         /* [한국어] 클라이언트 모드 경로. */
		ret = fio_netio_connect(td, f);

	if (ret) {                                   /* [한국어] 연결 실패 시 fd 무효화 후 반환. */
		f->fd = -1;
		return ret;
	}

	if (is_udp(o)) {                             /* [한국어] UDP OPEN 핸드쉐이크 처리. */
		if (td_write(td))                    /* [한국어] 송신 잡이면 OPEN 보냄. */
			ret = fio_netio_send_open(td, f);
		else {                               /* [한국어] 수신 잡이면 OPEN 수신 대기 — runstate 변경 구간. */
			int state;

			state = td->runstate;
			td_set_runstate(td, TD_SETTING_UP);
			ret = fio_netio_udp_recv_open(td, f);
			td_set_runstate(td, state);
		}
	}

	if (ret)                                     /* [한국어] 실패 시 자동 정리. */
		fio_netio_close_file(td, f);

	return ret;
}

/*
 * [한국어]
 * fio_fill_addr - 호스트 문자열을 sockaddr용 이진 주소로 변환(inet_pton 우선, getaddrinfo 폴백).
 * @td: 잡. @host: 호스트명 또는 숫자 IP. @af: AF_INET/AF_INET6/AF_VSOCK.
 * @dst: in_addr 또는 in6_addr 저장 위치. @res: getaddrinfo 결과(호출자가 freeaddrinfo 책임).
 * @return: 0=성공, 1=실패.
 * 호출 체인: fio_netio_setup_connect_inet → [fill_addr] → inet_pton/getaddrinfo.
 */
static int fio_fill_addr(struct thread_data *td, const char *host, int af,
			 void *dst, struct addrinfo **res)
{
	struct netio_options *o = td->eo;            /* [한국어] proto 분기용. */
	struct addrinfo hints;                       /* [한국어] getaddrinfo에 넘길 필터. */
	int ret;

	if (inet_pton(af, host, dst))                /* [한국어] 숫자형 주소는 DNS 없이 즉시 변환(성공 시 1). */
		return 0;

	memset(&hints, 0, sizeof(hints));            /* [한국어] hints 초기화. */

	if (is_tcp(o) || is_vsock(o))                /* [한국어] 스트림이면 SOCK_STREAM으로 필터. */
		hints.ai_socktype = SOCK_STREAM;
	else
		hints.ai_socktype = SOCK_DGRAM;      /* [한국어] UDP는 SOCK_DGRAM. */

	if (is_ipv6(o))                              /* [한국어] IPv6 전용 해석. */
		hints.ai_family = AF_INET6;
#ifdef CONFIG_VSOCK
	else if (is_vsock(o))                        /* [한국어] VSOCK family. */
		hints.ai_family = AF_VSOCK;
#endif
	else
		hints.ai_family = AF_INET;           /* [한국어] 기본 IPv4. */

	ret = getaddrinfo(host, NULL, &hints, res);  /* [한국어] DNS/서비스 해석(libc). */
	if (ret) {                                   /* [한국어] EAI_* 에러. */
		int e = EINVAL;
		char str[128];

		if (ret == EAI_SYSTEM)               /* [한국어] EAI_SYSTEM이면 errno가 실에러. */
			e = errno;

		snprintf(str, sizeof(str), "getaddrinfo: %s", gai_strerror(ret));
		td_verror(td, e, str);
		return 1;
	}

	return 0;
}

/*
 * [한국어]
 * fio_netio_setup_connect_inet - TCP/UDP v4/v6 클라이언트 측 목적지 주소 세팅.
 * @host: 호스트명. @port: 포트.
 * @return: 0=성공, 1=실패.
 * 호출 체인: fio_netio_setup_connect → [setup_connect_inet] → fio_fill_addr.
 */
static int fio_netio_setup_connect_inet(struct thread_data *td,
					const char *host, unsigned short port)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 주소 구조체 저장 대상. */
	struct netio_options *o = td->eo;            /* [한국어] IPv6 여부. */
	struct addrinfo *res = NULL;                 /* [한국어] getaddrinfo 결과 체인. */
	void *dst, *src;                             /* [한국어] 주소 복사 원본/목적지. */
	int af, len;                                 /* [한국어] address family 및 복사 길이. */

	if (!host) {                                 /* [한국어] hostname 미지정이면 가이드성 에러. */
		log_err("fio: connect with no host to connect to.\n");
		if (td_read(td))
			log_err("fio: did you forget to set 'listen'?\n");

		td_verror(td, EINVAL, "no hostname= set");
		return 1;
	}

	nd->addr.sin_family = AF_INET;               /* [한국어] v4 기본값 준비. */
	nd->addr.sin_port = htons(port);             /* [한국어] 네트워크 바이트 순서로 포트 저장. */
	nd->addr6.sin6_family = AF_INET6;            /* [한국어] v6 기본값 준비. */
	nd->addr6.sin6_port = htons(port);

	if (is_ipv6(o)) {                            /* [한국어] 채울 주소 선택. */
		af = AF_INET6;
		dst = &nd->addr6.sin6_addr;
	} else {
		af = AF_INET;
		dst = &nd->addr.sin_addr;
	}

	if (fio_fill_addr(td, host, af, dst, &res))  /* [한국어] 문자열 → 이진 주소 변환. */
		return 1;

	if (!res)                                    /* [한국어] inet_pton 경로라면 res가 없음 — 이미 dst에 기록됨. */
		return 0;

	if (is_ipv6(o)) {                            /* [한국어] getaddrinfo 결과에서 주소 추출. */
		len = sizeof(nd->addr6.sin6_addr);
		src = &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;
	} else {
		len = sizeof(nd->addr.sin_addr);
		src = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
	}

	memcpy(dst, src, len);                       /* [한국어] 해석 결과를 최종 목적지에 복사. */
	freeaddrinfo(res);                           /* [한국어] getaddrinfo 할당 메모리 반환. */
	return 0;
}

/*
 * [한국어]
 * fio_netio_setup_connect_unix - Unix 도메인 소켓 경로 설정(sockaddr_un.sun_path 채움).
 * 호출 체인: fio_netio_setup_connect → [setup_connect_unix].
 */
static int fio_netio_setup_connect_unix(struct thread_data *td,
					const char *path)
{
	struct netio_data *nd = td->io_ops_data;
	struct sockaddr_un *soun = &nd->addr_un;

	soun->sun_family = AF_UNIX;                  /* [한국어] Unix 도메인 family. */
	snprintf(soun->sun_path, sizeof(soun->sun_path), "%s", path); /* [한국어] 소켓 경로 복사(truncation 안전). */
	return 0;
}

/*
 * [한국어]
 * fio_netio_setup_connect_vsock - VSOCK 클라이언트 주소(cid/port) 설정.
 * host는 숫자 CID 문자열(예: "2"는 호스트). 호출 체인: setup_connect → [setup_connect_vsock].
 */
static int fio_netio_setup_connect_vsock(struct thread_data *td,
					const char *host, unsigned short port)
{
#ifdef CONFIG_VSOCK
	struct netio_data *nd = td->io_ops_data;
	struct sockaddr_vm *addr = &nd->addr_vm;
	int cid;

	if (!host) {                                 /* [한국어] CID 미지정. */
		log_err("fio: connect with no host to connect to.\n");
		if (td_read(td))
			log_err("fio: did you forget to set 'listen'?\n");

		td_verror(td, EINVAL, "no hostname= set");
		return 1;
	}

	addr->svm_family = AF_VSOCK;                 /* [한국어] 주소 family 고정. */
	addr->svm_port = port;                       /* [한국어] VSOCK 포트(네트워크 순서 변환 불필요). */

	if (host) {                                  /* [한국어] host(cid 문자열) 파싱. */
		cid = atoi(host);
		if (cid < 0 || cid > UINT32_MAX) {   /* [한국어] CID 범위 검증. */
			log_err("fio: invalid CID %d\n", cid);
			return 1;
		}
		addr->svm_cid = cid;
	}

	return 0;
#else
	td_verror(td, -EINVAL, "vsock not supported"); /* [한국어] 커널 헤더 미지원 빌드. */
	return 1;
#endif
}

/*
 * [한국어]
 * fio_netio_setup_connect - 프로토콜별 클라이언트 측 주소 세팅 디스패처.
 * 호출 체인: fio_netio_init(클라) → [setup_connect] → proto별 헬퍼.
 */
static int fio_netio_setup_connect(struct thread_data *td)
{
	struct netio_options *o = td->eo;

	if (is_udp(o) || is_tcp(o))                  /* [한국어] IPv4/IPv6 TCP/UDP. */
		return fio_netio_setup_connect_inet(td, td->o.filename,o->port);
	else if (is_vsock(o))                        /* [한국어] VSOCK. */
		return fio_netio_setup_connect_vsock(td, td->o.filename, o->port);
	else                                         /* [한국어] Unix 도메인. */
		return fio_netio_setup_connect_unix(td, td->o.filename);
}

/*
 * [한국어]
 * fio_netio_setup_listen_unix - Unix 도메인 소켓 서버 바인딩(path 생성/재사용).
 * 이미 존재하는 소켓 파일을 unlink 후 재생성, 모든 권한 허용(umask 000)으로 bind.
 * 호출 체인: fio_netio_setup_listen → [setup_listen_unix] → socket/bind.
 */
static int fio_netio_setup_listen_unix(struct thread_data *td, const char *path)
{
	struct netio_data *nd = td->io_ops_data;
	struct sockaddr_un *addr = &nd->addr_un;
	mode_t mode;                                 /* [한국어] 이전 umask 백업. */
	int len, fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);        /* [한국어] Unix 스트림 소켓 생성. */
	if (fd < 0) {
		log_err("fio: socket: %s\n", strerror(errno));
		return -1;
	}

	mode = umask(000);                           /* [한국어] 소켓 파일 생성 시 권한 제한 해제. */

	addr->sun_family = AF_UNIX;                  /* [한국어] Unix family. */
	snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path); /* [한국어] 경로 설정. */
	unlink(path);                                /* [한국어] 기존 소켓 파일 제거(재시작 재사용). */

	len = sizeof(addr->sun_family) + strlen(path) + 1; /* [한국어] SUN_LEN 대체 계산. */

	if (bind(fd, (struct sockaddr *) addr, len) < 0) { /* [한국어] 소켓 파일 경로에 바인딩. */
		log_err("fio: bind: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	umask(mode);                                 /* [한국어] 원래 umask 복원. */
	nd->listenfd = fd;                           /* [한국어] 이후 listen/accept에서 사용. */
	return 0;
}

/*
 * [한국어]
 * fio_netio_setup_listen_inet - IPv4/IPv6 TCP·UDP 서버 소켓 생성 및 bind.
 * SO_REUSEADDR/SO_REUSEPORT로 빠른 재시작 지원, 멀티캐스트(UDP v4)면 그룹 조인까지 수행.
 * 호출 체인: fio_netio_setup_listen → [setup_listen_inet].
 */
static int fio_netio_setup_listen_inet(struct thread_data *td, short port)
{
	struct netio_data *nd = td->io_ops_data;
	struct netio_options *o = td->eo;
	struct ip_mreq mr;                           /* [한국어] 멀티캐스트 그룹 가입 구조체. */
	struct sockaddr_in sin;                      /* [한국어] 멀티캐스트 대상 IPv4 주소 임시 저장. */
	struct sockaddr *saddr;                      /* [한국어] bind에 넘길 포인터. */
	int fd, opt, type, domain;                   /* [한국어] 소켓 생성 파라미터. */
	socklen_t len;

	memset(&sin, 0, sizeof(sin));                /* [한국어] 기본 0 초기화(비멀티캐스트 경로 구분용). */

	if (o->proto == FIO_TYPE_TCP) {              /* [한국어] TCP v4. */
		type = SOCK_STREAM;
		domain = AF_INET;
	} else if (o->proto == FIO_TYPE_TCP_V6) {    /* [한국어] TCP v6. */
		type = SOCK_STREAM;
		domain = AF_INET6;
	} else if (o->proto == FIO_TYPE_UDP) {       /* [한국어] UDP v4. */
		type = SOCK_DGRAM;
		domain = AF_INET;
	} else if (o->proto == FIO_TYPE_UDP_V6) {    /* [한국어] UDP v6. */
		type = SOCK_DGRAM;
		domain = AF_INET6;
	} else {
		log_err("fio: unknown proto %d\n", o->proto);
		return 1;
	}

	fd = socket(domain, type, 0);                /* [한국어] 서버 소켓 생성. */
	if (fd < 0) {
		td_verror(td, errno, "socket");
		return 1;
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &opt, sizeof(opt)) < 0) {
		td_verror(td, errno, "setsockopt");  /* [한국어] TIME_WAIT 중인 포트 재바인딩 허용. */
		close(fd);
		return 1;
	}
#ifdef SO_REUSEPORT
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (void *) &opt, sizeof(opt)) < 0) {
		td_verror(td, errno, "setsockopt");  /* [한국어] 멀티 프로세스/스레드 동일 포트 바인딩(로드 분산). */
		close(fd);
		return 1;
	}
#endif

	if (set_window_size(td, fd)) {               /* [한국어] 수신 버퍼 크기 튜닝. */
		close(fd);
		return 1;
	}
	if (set_mss(td, fd)) {                       /* [한국어] TCP MSS 튜닝. */
		close(fd);
		return 1;
	}

	if (td->o.filename) {                        /* [한국어] 서버인데 filename 지정 = 멀티캐스트 그룹 주소. */
		if (!is_udp(o) || !fio_netio_is_multicast(td->o.filename)) {
			log_err("fio: hostname not valid for non-multicast inbound network IO\n");
			close(fd);
			return 1;
		}
		if (is_ipv6(o)) {
			log_err("fio: IPv6 not supported for multicast network IO\n");
			close(fd);
			return 1;
		}

		inet_aton(td->o.filename, &sin.sin_addr); /* [한국어] 멀티캐스트 그룹 주소 이진화. */

		mr.imr_multiaddr = sin.sin_addr;     /* [한국어] 가입 대상 그룹. */
		if (o->intfc) {                      /* [한국어] 특정 인터페이스로 수신 강제. */
			if (inet_aton(o->intfc, &mr.imr_interface) == 0) {
				log_err("fio: interface not valid interface IP\n");
				close(fd);
				return 1;
			}
		} else {
			mr.imr_interface.s_addr = htonl(INADDR_ANY); /* [한국어] 기본: 모든 인터페이스. */
		}

		if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof(mr)) < 0) {
			td_verror(td, errno, "setsockopt IP_ADD_MEMBERSHIP"); /* [한국어] 커널에 멀티캐스트 그룹 가입 요청. */
			close(fd);
			return 1;
		}
	}

	if (!is_ipv6(o)) {                           /* [한국어] IPv4 바인드 주소 준비. */
		saddr = (struct sockaddr *) &nd->addr;
		len = sizeof(nd->addr);

		nd->addr.sin_family = AF_INET;
		nd->addr.sin_addr.s_addr = sin.sin_addr.s_addr ? sin.sin_addr.s_addr : htonl(INADDR_ANY); /* [한국어] 멀티캐스트 주소면 그 주소로, 아니면 전체 수신. */
		nd->addr.sin_port = htons(port);
	} else {                                     /* [한국어] IPv6 바인드. */
		saddr = (struct sockaddr *) &nd->addr6;
		len = sizeof(nd->addr6);

		nd->addr6.sin6_family = AF_INET6;
		nd->addr6.sin6_addr = in6addr_any;   /* [한국어] 모든 IPv6 인터페이스 수신. */
		nd->addr6.sin6_port = htons(port);
	}

	if (bind(fd, saddr, len) < 0) {              /* [한국어] 지정 주소/포트에 바인딩. */
		close(fd);
		td_verror(td, errno, "bind");
		return 1;
	}

	nd->listenfd = fd;                           /* [한국어] 이후 listen/accept 대상. */
	return 0;
}

/*
 * [한국어]
 * fio_netio_setup_listen_vsock - VSOCK 서버 소켓 생성/바인딩(VMADDR_CID_ANY 사용).
 * 호출 체인: fio_netio_setup_listen → [setup_listen_vsock].
 */
static int fio_netio_setup_listen_vsock(struct thread_data *td, short port, int type)
{
#ifdef CONFIG_VSOCK
	struct netio_data *nd = td->io_ops_data;
	struct sockaddr_vm *addr = &nd->addr_vm;
	int fd, opt;
	socklen_t len;

	fd = socket(AF_VSOCK, type, 0);              /* [한국어] VSOCK 소켓. */
	if (fd < 0) {
		td_verror(td, errno, "socket");
		return 1;
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &opt, sizeof(opt)) < 0) {
		td_verror(td, errno, "setsockopt"); /* [한국어] 재바인딩 허용. */
		close(fd);
		return 1;
	}

	len = sizeof(*addr);

	nd->addr_vm.svm_family = AF_VSOCK;
	nd->addr_vm.svm_cid = VMADDR_CID_ANY;        /* [한국어] 어떤 CID에서 오는 연결이든 허용. */
	nd->addr_vm.svm_port = port;

	if (bind(fd, (struct sockaddr *) addr, len) < 0) {
		td_verror(td, errno, "bind");
		close(fd);
		return 1;
	}

	nd->listenfd = fd;
	return 0;
#else
	td_verror(td, -EINVAL, "vsock not supported");
	return -1;
#endif
}

/*
 * [한국어]
 * fio_netio_setup_listen - 서버 모드 진입점. proto별 setup_listen_* 호출 후 listen(2).
 * UDP는 listen 불필요(연결 없음)므로 바인딩만 하고 반환.
 * 호출 체인: fio_netio_init(서버) → [setup_listen] → 프로토콜별 setup_listen_*.
 */
static int fio_netio_setup_listen(struct thread_data *td)
{
	struct netio_data *nd = td->io_ops_data;
	struct netio_options *o = td->eo;
	int ret;

	if (is_udp(o) || is_tcp(o))                  /* [한국어] IPv4/6 TCP/UDP. */
		ret = fio_netio_setup_listen_inet(td, o->port);
	else if (is_vsock(o))                        /* [한국어] VSOCK은 STREAM. */
		ret = fio_netio_setup_listen_vsock(td, o->port, SOCK_STREAM);
	else                                         /* [한국어] Unix 도메인. */
		ret = fio_netio_setup_listen_unix(td, td->o.filename);

	if (ret)
		return ret;
	if (is_udp(o))                               /* [한국어] UDP는 listen 단계 생략. */
		return 0;

	if (listen(nd->listenfd, 10) < 0) {          /* [한국어] 백로그 10 — 대기 중 미완료 연결 큐 한계. */
		td_verror(td, errno, "listen");
		nd->listenfd = -1;
		return 1;
	}

	return 0;
}

/*
 * [한국어]
 * fio_netio_init - net 엔진 초기화 콜백
 *
 * listen 모드이면 소켓을 생성하고 bind/listen하여 클라이언트 연결을 대기하고,
 * 그렇지 않으면 connect 주소를 설정한다. 프로토콜별(TCP/UDP/Unix/VSOCK)로
 * 적절한 소켓 생성 및 주소 구조체 초기화를 수행한다.
 *
 * 호출 체인: td_io_init() → [이 함수] → socket(2)/bind(2)/listen(2)
 */
static int fio_netio_init(struct thread_data *td)
{
	struct netio_options *o = td->eo;
	int ret;

#ifdef WIN32
	WSADATA wsd;                                 /* [한국어] Winsock 2.2 초기화(DLL 로드). */
	WSAStartup(MAKEWORD(2,2), &wsd);
#endif

	if (td_random(td)) {                         /* [한국어] 네트워크는 순차 I/O만 유효(오프셋 개념 없음). */
		log_err("fio: network IO can't be random\n");
		return 1;
	}

	if (o->proto == FIO_TYPE_UNIX && o->port) {  /* [한국어] UNIX 도메인은 포트 개념 없음. */
		log_err("fio: network IO port not valid with unix socket\n");
		return 1;
	} else if (is_vsock(o) && !o->port) {        /* [한국어] VSOCK은 포트 필수. */
		log_err("fio: network IO requires port for vsock\n");
		return 1;
	} else if (o->proto != FIO_TYPE_UNIX && !o->port) { /* [한국어] TCP/UDP도 포트 필수. */
		log_err("fio: network IO requires port for tcp or udp\n");
		return 1;
	}

	o->port += td->subjob_number;                /* [한국어] 서브잡 번호별로 포트 오프셋(다수 잡 분산용). */

	if (!is_tcp(o) && !is_vsock(o)) {            /* [한국어] UDP/UNIX만 해당되는 제약 검증 블록. */
		if (o->listen) {
			log_err("fio: listen only valid for TCP proto IO\n"); /* [한국어] listen= 옵션 오용. */
			return 1;
		}
		if (td_rw(td)) {                     /* [한국어] 데이터그램/UNIX는 읽기 또는 쓰기 중 하나만. */
			log_err("fio: datagram network connections must be"
				   " read OR write\n");
			return 1;
		}
		if (o->proto == FIO_TYPE_UNIX && !td->o.filename) {
			log_err("fio: UNIX sockets need host/filename\n"); /* [한국어] Unix 소켓 경로 필수. */
			return 1;
		}
		o->listen = td_read(td);             /* [한국어] 읽기면 서버, 쓰기면 클라이언트로 자동 지정. */
	}

	if (o->listen)
		ret = fio_netio_setup_listen(td);    /* [한국어] 서버 경로. */
	else
		ret = fio_netio_setup_connect(td);   /* [한국어] 클라이언트 경로. */

	return ret;
}

/*
 * [한국어]
 * fio_netio_cleanup - 엔진 종료 시 자원 반환(ioengine_ops.cleanup).
 * listenfd/파이프 FD를 닫고 netio_data 할당 해제. td->io_ops_data는 잡 스레드 전용이라 락 불필요.
 * 호출 체인: td_io_cleanup() → [fio_netio_cleanup].
 */
static void fio_netio_cleanup(struct thread_data *td)
{
	struct netio_data *nd = td->io_ops_data;

	if (nd) {
		if (nd->listenfd != -1)              /* [한국어] 서버 소켓 정리. */
			close(nd->listenfd);
		if (nd->pipes[0] != -1)              /* [한국어] splice 파이프 읽기쪽. */
			close(nd->pipes[0]);
		if (nd->pipes[1] != -1)              /* [한국어] splice 파이프 쓰기쪽. */
			close(nd->pipes[1]);

		free(nd);                            /* [한국어] 구조체 자체 해제. */
	}
}

/*
 * [한국어]
 * fio_netio_setup - 엔진 setup 콜백(init 이전에 호출). fio_file 등록 및 netio_data 할당.
 * 파일명이 제공되지 않으면 가상 이름 "net"으로 파일을 하나 추가해 fio의 I/O 경로와 호환시킴.
 * 호출 체인: load_ioengine → [fio_netio_setup] → add_file.
 */
static int fio_netio_setup(struct thread_data *td)
{
	struct netio_data *nd;

	if (!td->files_index) {                      /* [한국어] 아직 파일이 등록되지 않았다면 가상 파일 추가. */
		add_file(td, td->o.filename ?: "net", 0, 0);
		td->o.nr_files = td->o.nr_files ?: 1;
		td->o.open_files++;
	}

	if (!td->io_ops_data) {                      /* [한국어] 첫 호출에서만 netio_data 할당. */
		nd = calloc(1, sizeof(*nd));
		nd->listenfd = -1;                   /* [한국어] 초기 무효값. */
		nd->pipes[0] = nd->pipes[1] = -1;    /* [한국어] 파이프 아직 미생성. */
		td->io_ops_data = nd;
	}

	return 0;
}

/*
 * [한국어]
 * fio_netio_terminate - 외부 종료 요청 시 현재 잡 프로세스에 SIGTERM 송신.
 * 블로킹 poll/recv 중인 잡을 깨우기 위한 용도. 호출 체인: 종료 핸들러 → [terminate] → kill(2).
 */
static void fio_netio_terminate(struct thread_data *td)
{
	kill(td->pid, SIGTERM);                      /* [한국어] 잡 프로세스/스레드에 종료 시그널. */
}

#ifdef CONFIG_LINUX_SPLICE
/*
 * [한국어]
 * fio_netio_setup_splice - netsplice 엔진 전용 setup(공통 setup + 파이프 생성).
 * pipe(2)로 익명 파이프를 만들어 splice/vmsplice 경로 활성화.
 * 호출 체인: load_ioengine(netsplice) → [setup_splice] → fio_netio_setup + pipe(2).
 */
static int fio_netio_setup_splice(struct thread_data *td)
{
	struct netio_data *nd;

	fio_netio_setup(td);                         /* [한국어] 공통 setup 먼저 수행. */

	nd = td->io_ops_data;
	if (nd) {
		if (pipe(nd->pipes) < 0)             /* [한국어] read/write 파이프 쌍 생성. */
			return 1;

		nd->use_splice = 1;                  /* [한국어] queue 분기에서 splice 경로 선택. */
		return 0;
	}

	return 1;
}

/*
 * [한국어] netsplice 엔진 등록 메타(splice 기반 제로카피 엔진).
 *  - flags에 FIO_PIPEIO(파이프 중계), FIO_DISKLESSIO(디스크 없음) 포함.
 */
static struct ioengine_ops ioengine_splice = {
	.name			= "netsplice",
	.version		= FIO_IOOPS_VERSION,
	.prep			= fio_netio_prep,
	.queue			= fio_netio_queue,
	.setup			= fio_netio_setup_splice,
	.init			= fio_netio_init,
	.cleanup		= fio_netio_cleanup,
	.open_file		= fio_netio_open_file,
	.close_file		= fio_netio_close_file,
	.terminate		= fio_netio_terminate,
	.options		= options,
	.option_struct_size	= sizeof(struct netio_options),
	.flags			= FIO_SYNCIO | FIO_DISKLESSIO | FIO_UNIDIR |
				  FIO_PIPEIO,
};
#endif

/*
 * [한국어] 일반 "net" 엔진 등록 메타(send/recv 기반).
 *  - FIO_SYNCIO: 동기 I/O, FIO_DISKLESSIO: 디스크 의존 없음, FIO_UNIDIR: 한 방향 잡 지원,
 *    FIO_PIPEIO: 파이프 유사 성격, FIO_BIT_BASED: 비트 단위 크기 표기 허용.
 */
static struct ioengine_ops ioengine_rw = {
	.name			= "net",
	.version		= FIO_IOOPS_VERSION,
	.prep			= fio_netio_prep,
	.queue			= fio_netio_queue,
	.setup			= fio_netio_setup,
	.init			= fio_netio_init,
	.cleanup		= fio_netio_cleanup,
	.open_file		= fio_netio_open_file,
	.close_file		= fio_netio_close_file,
	.terminate		= fio_netio_terminate,
	.options		= options,
	.option_struct_size	= sizeof(struct netio_options),
	.flags			= FIO_SYNCIO | FIO_DISKLESSIO | FIO_UNIDIR |
				  FIO_PIPEIO | FIO_BIT_BASED,
};

/*
 * [한국어]
 * str_hostname_cb - hostname= 옵션 값이 파싱될 때 호출되는 콜백.
 * @data: netio_options 포인터(파서가 제공). @input: 사용자가 지정한 문자열.
 * @return: 0(항상 성공).
 *
 * 기존 td->o.filename이 있으면 해제 후 새 문자열 복제 저장. fio는 네트워크 호스트를 파일명
 * 슬롯으로 저장하여 공통 파일 경로와 동일하게 다룬다.
 * 호출 체인: 옵션 파서(parse.c) → [str_hostname_cb].
 */
static int str_hostname_cb(void *data, const char *input)
{
	struct netio_options *o = data;              /* [한국어] 옵션 컨텍스트에서 td 역참조. */

	if (o->td->o.filename)
		free(o->td->o.filename);             /* [한국어] 이전 filename 해제(중복 호출 안전). */
	o->td->o.filename = strdup(input);           /* [한국어] 사용자 입력 복제 저장. */
	return 0;
}

/*
 * [한국어]
 * fio_netio_register - 모듈 로드(init 생성자) 시 net/netsplice 엔진을 fio 코어에 등록.
 * fio_init 속성으로 인해 라이브러리/바이너리 로드 시 자동 호출.
 * 호출 체인: ELF constructor → [fio_netio_register] → register_ioengine().
 */
static void fio_init fio_netio_register(void)
{
	register_ioengine(&ioengine_rw);             /* [한국어] 일반 "net" 엔진 등록. */
#ifdef CONFIG_LINUX_SPLICE
	register_ioengine(&ioengine_splice);         /* [한국어] "netsplice" 엔진 등록(리눅스만). */
#endif
}

/*
 * [한국어]
 * fio_netio_unregister - 모듈 언로드(exit 소멸자) 시 엔진 등록 해제.
 * 호출 체인: ELF destructor → [fio_netio_unregister] → unregister_ioengine().
 */
static void fio_exit fio_netio_unregister(void)
{
	unregister_ioengine(&ioengine_rw);
#ifdef CONFIG_LINUX_SPLICE
	unregister_ioengine(&ioengine_splice);
#endif
}
