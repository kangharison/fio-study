/*
 * [한국어 설명] 네트워크 I/O 엔진 구현 (net.c — "net" 및 "netsplice" 두 엔진)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 두 개의 I/O 엔진을 단일 translation unit에서 동시에 구현한다:
 *
 *   (1) "net"      — 일반 소켓 I/O. read 경로는 recv(2)/recvfrom(2),
 *                     write 경로는 send(2)/sendto(2)를 호출하여 TCP/UDP(IPv4·IPv6)/
 *                     Unix 도메인 소켓/VSOCK(게스트-호스트 VM 통신)을 모두 커버한다.
 *                     동기 엔진(FIO_SYNCIO)이라 queue() 콜백 내부에서 blocking
 *                     send/recv + poll(2) 대기까지 모두 수행한 뒤 FIO_Q_COMPLETED
 *                     를 즉시 반환하는 1-단계 디스패치 모델이다.
 *
 *   (2) "netsplice" — Linux splice(2)+vmsplice(2) 기반 제로카피 변종.
 *                     소켓↔커널 파이프(pipes[2])↔유저 페이지 사이에서 페이지
 *                     포인터만 이동해 사용자공간 복사를 회피한다. FIO_PIPEIO 플래그
 *                     로 fio 코어에 "파이프 기반 엔진"임을 알리며 CONFIG_LINUX_SPLICE
 *                     로 빌드된 환경에서만 등록된다(BSD/macOS/Windows에선 비활성).
 *
 * 두 엔진 모두 클라이언트(initiator) / 서버(acceptor) 모드를 지원한다:
 *   - listen=1 또는 read 잡(td_read) → 서버: socket+bind+listen 후 accept(2) 대기.
 *   - 그 외 (write 잡 또는 명시적 hostname 지정) → 클라이언트: socket+connect(2).
 *   UDP는 connectionless라 listen/accept 단계가 생략되고, sendto/recvfrom으로 매
 *   호출마다 peer 주소를 명시한다. 또한 UDP 잡은 시작/종료를 애플리케이션 레벨 매직
 *   메시지(udp_close_msg, FIO_LINK_OPEN/CLOSE)로 핸드쉐이크하여 잡 종료 시점을
 *   양측이 합의한다.
 *
 * UDP 데이터 무결성/순서 검증을 위해 verify=none 잡에서는 매 패킷의 페이로드 끝
 * sizeof(struct udp_seq) 영역에 (magic, seq, bs) 헤더를 덮어 송신하고 수신 측이
 * 매칭한다. seq jump가 감지되면 ts.drop_io_u에 손실 카운트를 누적한다. verify가
 * 데이터 패턴을 직접 검증하는 모드에서는 udp_seq를 끼워넣지 않는다(중복 검증 회피).
 *
 * 옵션은 별도 optgroup(FIO_OPT_G_NETIO)으로 등록되며 호스트/포트/프로토콜
 * (proto=tcp|udp|unix|tcpv6|udpv6|vsock), listen, pingpong, nodelay(TCP_NODELAY),
 * window_size(SO_RCVBUF/SNDBUF), mss(TCP_MAXSEG), ttl(IP_MULTICAST_TTL),
 * interface(SO_BINDTODEVICE 의도지만 실제 구현은 IP_MULTICAST_IF) 등 상세
 * 네트워크 튜닝을 제공한다.
 *
 * === ioengine_ops 콜백 매핑 (null.c §1과 같은 양식) ===
 *   .setup       : fio_netio_setup / fio_netio_setup_splice — fio_file 등록 +
 *                  netio_data 할당. splice 변종은 pipe(2)로 익명 파이프 추가 생성.
 *   .init        : fio_netio_init — proto/listen 검증 후 setup_listen|setup_connect.
 *                  주소 구조체(addr/addr6/addr_un/addr_vm)를 채우고 listen 모드면
 *                  socket+bind+listen, 클라면 connect 대상 주소만 준비.
 *   .open_file   : fio_netio_open_file — 실제 accept(2)/connect(2) 수행. UDP면
 *                  LINK_OPEN 핸드쉐이크까지 한 번 더 주고받음.
 *   .prep        : fio_netio_prep — UDP 잡의 ddir 방향 검증(데이터그램은 단방향).
 *   .queue       : fio_netio_queue → __fio_netio_queue → send/recv/splice_*.
 *                  pingpong=1이면 송신 후 응답 수신(또는 그 역)까지 한 큐에서 처리.
 *                  반환값: FIO_Q_COMPLETED(부분 전송 포함) / FIO_Q_BUSY(EMSGSIZE 등).
 *   .commit/.getevents/.event: 미설정 — 동기 엔진이라 ioengines.c 코어가 즉시
 *                  put_io_u로 정리(FIO_SYNCIO 비트 의미).
 *   .close_file  : fio_netio_close_file — UDP면 LINK_CLOSE 송신 후 generic_close.
 *   .terminate   : fio_netio_terminate — 외부 종료 신호 수신 시 SIGTERM 자기송신
 *                  으로 blocking poll/recv를 깨움(SIGTERM 핸들러는 EINTR 발생).
 *   .cleanup     : fio_netio_cleanup — listenfd, pipes[], netio_data 모두 회수.
 *   .flags       : FIO_SYNCIO|DISKLESSIO|UNIDIR|PIPEIO (+ "net" 변종은 BIT_BASED).
 *                  splice 변종은 위에서 BIT_BASED 제외.
 *
 * === TCP 소켓 옵션 풀이 (set_window_size/set_mss + connect/accept 분기) ===
 *   - SO_REUSEADDR  : TIME_WAIT 상태인 포트 재바인딩 허용. 잡 재시작 즉시 가능케 함.
 *   - SO_REUSEPORT  : 다중 프로세스/스레드가 같은 포트에 동시 bind — 커널이 부하 분산.
 *   - SO_RCVBUF/SNDBUF (set_window_size): 수신/송신 큐 한계. 커널이 실제로는 2배로
 *                     기록(bufsize*2)하여 sk_rmem_alloc 누적과 비교.
 *   - TCP_NODELAY   : Nagle 알고리즘 비활성화. 작은 쓰기를 즉시 송출(저지연).
 *   - TCP_MAXSEG    : MSS 강제. MTU 제약 환경 재현 또는 단편화 실험용.
 *   - IP_MULTICAST_TTL : 멀티캐스트 패킷의 라우터 홉 한계.
 *   - IP_MULTICAST_IF  : 멀티캐스트 송출에 사용할 출력 인터페이스 선택.
 *   - IP_ADD_MEMBERSHIP: 수신 측이 멀티캐스트 그룹에 가입(struct ip_mreq).
 *   - SO_LINGER, SO_KEEPALIVE: 본 엔진은 직접 사용하지 않음(필요 시 sysctl 의존).
 *
 * === pingpong / Multicast / iodepth=1 사유 ===
 *   - pingpong=1: 클라이언트가 데이터를 송신하면 서버가 동일 버퍼를 그대로 회신해
 *     RTT(왕복지연시간)를 측정. fio_netio_queue가 1차 ddir 실행 후 자동으로 반대
 *     방향을 한 번 더 호출(td_read+DDIR_READ → DDIR_WRITE 응답, td_write+DDIR_WRITE
 *     → DDIR_READ 수신).
 *   - Multicast: hostname이 224.0.0.0/4 범위면 자동으로 IP_ADD_MEMBERSHIP/
 *     IP_MULTICAST_IF/IP_MULTICAST_TTL 분기로 진입. IPv6 멀티캐스트는 본 구현
 *     미지원(별도 setsockopt API 필요).
 *   - iodepth는 fio 옵션상 임의 설정 가능하나, 본 엔진은 동기 엔진이라 queue()
 *     안에서 모든 I/O가 즉시 완료되므로 사실상 직렬화된다(스트리밍 소켓의 직렬성).
 *
 * === short read/write 처리 ===
 *   send/recv가 요청량보다 적게 처리하면 ret>0 분기에서 io_u->resid에 잔여를
 *   기록한 뒤 FIO_Q_COMPLETED 반환(fio 코어가 통계에서 부분 전송으로 회계).
 *   재시도 루프는 send 내부에서 poll_wait(POLLOUT) → 재시도 1회만 수행하며 EINTR/
 *   EAGAIN은 poll에서 흡수한다. 큰 UDP 데이터그램(EMSGSIZE) 시 BUSY 반환으로
 *   상위가 bs를 줄여 재시도하도록 유도.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend 잡 루프 → load_ioengine("net"|"netsplice") → ioengine_ops 콜백 체인:
 *
 *   setup → init → open_file → [잡 루프: prep → queue (→ commit no-op)] → close_file
 *                                                                       → cleanup
 *
 *   load_ioengine
 *       └─ fio_netio_setup     : add_file("net" 가짜) + calloc(netio_data)
 *   td_io_init
 *       └─ fio_netio_init      : proto/listen 검증 → setup_listen | setup_connect
 *                                 ├─ setup_listen: socket(2)+SO_REUSEADDR/PORT+bind(2)
 *                                 │                +(TCP)listen(2)+(UDP)multicast join
 *                                 └─ setup_connect: getaddrinfo/inet_pton + addr 채움
 *   td_io_open_file
 *       └─ fio_netio_open_file : (server) accept(2)+poll(2) | (client) connect(2)
 *                                 └─ (UDP) LINK_OPEN/RECV 핸드쉐이크
 *   잡 루프(get_io_u → prep → queue → put_io_u)
 *       ├─ fio_netio_prep      : UDP ddir 방향 검증
 *       └─ fio_netio_queue → __fio_netio_queue
 *               ├─ DDIR_WRITE: send / sendto / splice_out
 *               ├─ DDIR_READ : recv / recvfrom / splice_in
 *               └─ pingpong : 1차 후 반대 ddir 한 번 더
 *   td_io_close_file
 *       └─ fio_netio_close_file: send_close(UDP CLOSE) + generic_close_file
 *   td_io_cleanup
 *       └─ fio_netio_cleanup   : close(listenfd/pipes) + free(nd)
 *
 * 실행 컨텍스트는 잡 스레드의 사용자 공간이며, blocking poll/recv가 일어나는
 * 동안 외부 SIGTERM 등으로 잡을 깨우는 경로는 fio_netio_terminate가 SIGTERM을
 * self-send해서 EINTR을 유도하는 방식.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h          : thread_data, io_u, ioengine_ops, FIO_Q_*, dprint, td_verror,
 *                    td_set_runstate, fio_ro_check, generic_close_file, fio_init/exit,
 *                    register_ioengine/unregister_ioengine, ddir/td_read/write 매크로.
 * - verify.h       : VERIFY_NONE — UDP 시퀀스 헤더 삽입 분기 조건.
 * - optgroup.h     : FIO_OPT_C_ENGINE/FIO_OPT_G_NETIO — `fio --enghelp=net` 필터링.
 * - <sys/socket.h> : socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/
 *                    setsockopt + SO_xxx / MSG_xxx / SOL_SOCKET 매크로.
 * - <netinet/in.h> : sockaddr_in/sockaddr_in6/in_addr/in6_addr/IPPROTO_TCP/UDP/IP.
 * - <netinet/tcp.h>: TCP_NODELAY/TCP_MAXSEG.
 * - <arpa/inet.h>  : inet_pton/inet_aton/inet_network/htonl/htons/ntohl.
 * - <netdb.h>      : getaddrinfo/freeaddrinfo/gai_strerror/EAI_xxx/struct addrinfo.
 * - <sys/un.h>     : sockaddr_un (Unix 도메인).
 * - <linux/vm_sockets.h> : sockaddr_vm/AF_VSOCK/VMADDR_CID_ANY (옵션).
 * - <poll.h>       : poll(2)/struct pollfd/POLLIN/POLLOUT.
 * - <sys/stat.h>   : Unix 소켓 경로 검증/모드 처리(umask).
 * - <signal.h>     : kill(SIGTERM) (terminate 콜백).
 * - splice(2)/vmsplice(2) (CONFIG_LINUX_SPLICE) : 제로카피 페이지 이동.
 *
 * - 공유 상태:
 *   * td->io_ops_data = struct netio_data  (잡 스레드 소유, 락 불필요).
 *   * td->eo           = struct netio_options (옵션 파서가 채움, 잡 시작 후 불변).
 *   * UDP의 경우 잡 양쪽(서버/클라이언트)이 udp_seq/udp_close_msg를 wire 레벨
 *     프로토콜로 공유 — 매직과 cmd 값을 매칭해 시작/종료/순서를 합의.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct netio_data    : 잡별 런타임 상태 — listenfd/use_splice/seq_off/pipes[2]/
 *                          addr/addr6/addr_un/addr_vm/udp_send_seq/udp_recv_seq.
 * - struct netio_options : 옵션 — port/proto/listen/pingpong/nodelay/ttl/window_size/
 *                          mss/intfc(인터페이스).
 * - struct udp_seq       : UDP 페이로드 끝에 덮어쓰는 무결성/순서 검증 헤더.
 * - struct udp_close_msg : UDP 링크 OPEN/CLOSE 시그널 메시지.
 * - enum FIO_TYPE_*      : proto 식별 — TCP/UDP/UNIX/TCP_V6/UDP_V6/VSOCK_STREAM.
 * - enum FIO_LINK_*      : OPEN/CLOSE cmd 및 매직 상수.
 * - is_udp/is_tcp/is_ipv6/is_vsock : proto 분기 헬퍼.
 * - set_window_size/set_mss/poll_wait : 공통 setsockopt + poll 헬퍼.
 * - fio_netio_send/recv/splice_in/splice_out : 저수준 전송/수신.
 * - __fio_netio_queue/fio_netio_queue : 방향 디스패치 + pingpong 처리.
 * - fio_netio_init/setup/cleanup : 잡 라이프사이클.
 * - fio_netio_setup_listen_xxx / setup_connect_xxx : proto별 주소/소켓 준비.
 * - fio_netio_open_file/close_file/accept/connect/terminate : 연결 관리.
 * - fio_netio_send_open/udp_recv_open/send_close : UDP 핸드쉐이크.
 * - str_hostname_cb : hostname= 옵션 파싱 콜백.
 * - fio_netio_register/unregister : ELF constructor/destructor 진입점.
 *
 * === fio에서의 사용법 ===
 * 클라이언트(write 잡):
 *   fio --name=cli --ioengine=net --proto=tcp --hostname=server --port=8765 \
 *       --rw=write --bs=64k --size=1g --iodepth=1
 * 서버(read 잡 또는 listen=1):
 *   fio --name=srv --ioengine=net --proto=tcp --listen --port=8765 \
 *       --rw=read --bs=64k --size=1g
 * UDP pingpong RTT:
 *   양측 모두 --proto=udp --pingpong, 한쪽은 listen=1.
 */

/*
 * net engine
 *
 * IO engine that reads/writes to/from sockets.
 *
 */
#include <stdio.h>            /* [한국어] log_err/snprintf — 에러 메시지 포매팅에 사용 */
#include <stdlib.h>           /* [한국어] calloc(netio_data 0초기화)/free(cleanup)/atoi(VSOCK CID 파싱)/strdup(hostname cb) */
#include <unistd.h>           /* [한국어] close(소켓/파이프 FD 정리)/pipe(2)(splice 변종에서 익명 파이프 생성)/read/write 기본 POSIX */
#include <signal.h>           /* [한국어] kill(SIGTERM, ...) — fio_netio_terminate가 blocking poll/recv를 EINTR로 깨우기 위해 사용 */
#include <errno.h>            /* [한국어] errno 전역 — send/recv 실패 시 EMSGSIZE/EAGAIN/EINTR/EOPNOTSUPP 분기에 사용 */
#include <netinet/in.h>       /* [한국어] sockaddr_in/sockaddr_in6/in_addr/in6_addr/IPPROTO_TCP·UDP·IP 매크로 — IP 레이어 주소 구조체 정의 공급 */
#include <netinet/tcp.h>      /* [한국어] TCP_NODELAY(Nagle off)/TCP_MAXSEG(MSS 강제) 등 TCP 레벨 setsockopt 상수 공급 */
#include <arpa/inet.h>        /* [한국어] inet_pton/inet_aton/inet_network/htons/htonl/ntohl — 텍스트↔이진 주소 변환 + 바이트 순서 변환 */
#include <netdb.h>            /* [한국어] getaddrinfo/freeaddrinfo/gai_strerror/EAI_xxx/struct addrinfo — 호스트명 해석(DNS/sysdb) */
#include <poll.h>             /* [한국어] poll(2)/struct pollfd/POLLIN/POLLOUT — 동기 엔진의 blocking 대기점에서 fd 이벤트 대기 */
#include <sys/stat.h>         /* [한국어] umask(소켓 파일 mode 제한 해제) — Unix 도메인 소켓 bind 시 권한 보존을 위해 사용 */
#include <sys/socket.h>       /* [한국어] socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/setsockopt + SOL_SOCKET/SO_xxx/MSG_xxx/AF_xxx 매크로 공급 */
#include <sys/un.h>           /* [한국어] sockaddr_un.sun_family/sun_path — Unix 도메인 소켓 주소 표현(파일시스템 경로 기반) */

/* [한국어] VSOCK(Virtual Socket — VM guest↔host 통신) 지원은 Linux 커널 헤더(linux/vm_sockets.h)
 * 유무에 따라 조건부 컴파일. CONFIG_VSOCK는 ./configure가 헤더 존재를 확인해 정의한다.
 * 미지원 빌드에서도 sockaddr_vm 빈 stub 구조체와 AF_VSOCK=-1 매크로를 자리만 잡아두어
 * 컴파일이 통과되도록 한다(런타임에 socket(AF_VSOCK,...)는 EAFNOSUPPORT로 실패함). */
#ifdef CONFIG_VSOCK
#include <linux/vm_sockets.h>  /* [한국어] sockaddr_vm/svm_family/svm_cid/svm_port/AF_VSOCK/VMADDR_CID_ANY 공급 */
#else
struct sockaddr_vm {
	/* [한국어] 빈 stub — 미지원 플랫폼에서 netio_data가 멤버를 들고 있어도 OK하도록 함.
	 * 실제 사용 경로(setup_listen_vsock 등)는 #ifdef CONFIG_VSOCK로 보호되어 무력화. */
};
#ifndef AF_VSOCK
#define AF_VSOCK	-1   /* [한국어] 런타임에 socket(-1, ...) 호출이 EAFNOSUPPORT를 반환하도록 — connect_vsock 경로의 자연스러운 차단 */
#endif
#endif

#include "../fio.h"        /* [한국어] fio 코어 타입/매크로 (thread_data, io_u, ioengine_ops, FIO_Q_*, td_verror, dprint, fio_init/exit 등) */
#include "../verify.h"     /* [한국어] VERIFY_NONE 매크로 — UDP udp_seq 헤더 삽입 분기 조건(verify가 페이로드를 직접 검증하면 udp_seq 비활성) */
#include "../optgroup.h"   /* [한국어] FIO_OPT_C_ENGINE/FIO_OPT_G_NETIO — `fio --enghelp=net` 카테고리 분류용 옵션 그룹 ID */

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

	struct sockaddr_in addr;
	/* [한국어] IPv4 주소 슬롯 — TCP/UDP v4의 connect 목적지 또는 bind/accept 결과.
	 * 설정자: setup_connect_inet(클라 목적지)/setup_listen_inet(서버 bind 주소)/
	 *         accept(서버 측 클라 주소 수신).
	 * 읽는 자: send/recv가 sendto/recvfrom 인자로 사용; udp_close/open 메시지의 to.
	 * 값 범위: sin_family=AF_INET 고정, sin_port=네트워크 순서, sin_addr=in_addr.
	 * 동기화: 잡 스레드 전용. */

	struct sockaddr_in6 addr6;
	/* [한국어] IPv6 주소 슬롯 — TCP/UDP v6 동일 용도.
	 * 설정자/읽는 자: addr와 동일하지만 is_ipv6(o)일 때 선택.
	 * 값 범위: sin6_family=AF_INET6, sin6_addr=in6_addr(128bit). 동기화: 잡 전용. */

	struct sockaddr_un addr_un;
	/* [한국어] Unix 도메인 소켓 주소(파일시스템 경로 sun_path).
	 * 설정자: setup_connect_unix(클라)/setup_listen_unix(서버, 기존 파일 unlink 후 bind).
	 * 읽는 자: connect/bind 호출 인자.
	 * 값 범위: sun_family=AF_UNIX, sun_path는 sizeof 한도 내 경로(보통 108바이트).
	 * 동기화: 잡 전용. */

	struct sockaddr_vm addr_vm;
	/* [한국어] VSOCK(guest-host VM) 주소(svm_cid + svm_port).
	 * 설정자: setup_connect_vsock(host에서 atoi(CID))/setup_listen_vsock(VMADDR_CID_ANY).
	 * 읽는 자: connect/bind/accept.
	 * 값 범위: svm_family=AF_VSOCK, svm_cid는 32비트 unsigned (예: 2=호스트).
	 * 동기화: 잡 전용. CONFIG_VSOCK 미정의 시 빈 stub. */

	uint64_t udp_send_seq;
	/* [한국어] UDP 송신 시 패킷마다 증가시키는 시퀀스 카운터.
	 * 설정자: 매 송신마다 ++. 읽는 자: udp_seq.seq 기록용. */

	uint64_t udp_recv_seq;
	/* [한국어] UDP 수신에서 기대되는 다음 시퀀스 번호. 수신 패킷의 seq와 불일치 시
	 * 순서 뒤섞임/손실 감지. */
};

/*
 * [한국어] net 엔진 전용 옵션. 옵션 파서(parse.c)가 잡 파일/CLI 인자 파싱 시 채우며
 * 잡 시작 후에는 런타임 내내 불변(read-only). td->eo가 이 구조체를 가리키며 옵션
 * 테이블 options[]의 .off1 오프셋이 이 구조체 멤버를 직접 가리키도록 정의된다.
 * 잡 스레드 단독 소유 — 동시 접근 없음.
 */
struct netio_options {
	struct thread_data *td;
	/* [한국어] 옵션 파싱 콜백(예: str_hostname_cb)에서 td를 역참조하기 위한 링크.
	 * 설정자: parse.c가 옵션 구조체 할당 직후 td를 채워줌(엔진별 ioengine_ops.options
	 *         테이블 등록 경로의 후처리).
	 * 읽는 자: str_hostname_cb가 o->td->o.filename에 hostname을 저장할 때 사용.
	 * 값 범위: 유효한 thread_data 포인터(NULL 아님).
	 * 동기화: 잡 스레드 전용 — 콜백도 동일 스레드에서 호출. */

	unsigned int port;
	/* [한국어] 접속/리슨 포트 번호(TCP/UDP). VSOCK에서는 svm_port에 직접 매핑되며
	 * UNIX 도메인 소켓에선 의미 없음(설정 시 init에서 에러).
	 * 설정자: parse.c가 옵션 "port=NNN"을 INT로 파싱(min=1 max=65535).
	 * 읽는 자: setup_listen/setup_connect 경로의 모든 헬퍼 — htons(o->port).
	 * 값 범위: 1..65535 (well-known 포트 0 거부). 동기화: 불변. */

	unsigned int proto;
	/* [한국어] 프로토콜 식별 enum (FIO_TYPE_TCP/TCP_V6/UDP/UDP_V6/UNIX/VSOCK_STREAM).
	 * 설정자: parse.c가 "protocol=tcp" 등 STR을 옵션 테이블의 .posval 매핑으로
	 *         FIO_TYPE_* 정수로 변환.
	 * 읽는 자: is_udp/is_tcp/is_ipv6/is_vsock 헬퍼와 모든 socket()/연결 분기.
	 * 값 범위: FIO_TYPE_TCP(1)..FIO_TYPE_VSOCK_STREAM(6). 동기화: 불변. */

	unsigned int listen;
	/* [한국어] 1이면 서버 모드(socket+bind+listen+accept), 0이면 클라이언트 모드(connect).
	 * 설정자: parse.c가 STR_SET(존재 자체로 1)으로 처리; UDP/UNIX 잡은 init이 td_read에
	 *         따라 자동 결정해 덮어씀(o->listen = td_read(td)).
	 * 읽는 자: open_file → accept|connect 분기, set_window_size의 RCVBUF/SNDBUF 분기.
	 * 값 범위: 0 또는 1. 동기화: init 직후 한 번 갱신될 수 있고 이후 불변. */

	unsigned int pingpong;
	/* [한국어] 1이면 매 I/O 후 반대 방향 I/O를 한 번 더 수행 — RTT 측정.
	 * 설정자: parse.c STR_SET. 읽는 자: fio_netio_queue가 1차 ddir 후 자동 반대 방향
	 *         호출 여부 판단; set_window_size가 양방향 버퍼 모두 설정 여부 판단.
	 * 값 범위: 0/1. 동기화: 불변. */

	unsigned int nodelay;
	/* [한국어] 1이면 TCP_NODELAY를 setsockopt — Nagle 알고리즘 비활성화로 작은
	 * 메시지를 즉시 전송(저지연 응용 측정).
	 * 설정자: parse.c BOOL. 읽는 자: connect/accept 직후 setsockopt 분기.
	 * 값 범위: 0/1. 동기화: 불변. CONFIG_TCP_NODELAY 빌드에서만 노출. */

	unsigned int ttl;
	/* [한국어] 멀티캐스트 패킷의 IP TTL(Time-To-Live, 라우터 홉 한계).
	 * 설정자: parse.c INT(def="1"). 읽는 자: connect 경로에서 멀티캐스트 IPv4면
	 *         IP_MULTICAST_TTL setsockopt 인자.
	 * 값 범위: 0..255 (0=같은 호스트만, 1=같은 서브넷, 32=region, 255=전세계).
	 * 동기화: 불변. */

	unsigned int window_size;
	/* [한국어] SO_SNDBUF/SO_RCVBUF로 설정할 소켓 큐 크기(바이트). 0=커널 기본.
	 * 설정자: parse.c INT. 읽는 자: set_window_size가 listen/pingpong 조합으로
	 *         RCVBUF만/SNDBUF만/양쪽 결정 후 setsockopt.
	 * 값 범위: 0..(net.core.rmem_max 이하 권장). 커널이 실제로는 sysctl 한계로 클램프.
	 * 동기화: 불변. CONFIG_NET_WINDOWSIZE 빌드에서만 노출. */

	unsigned int mss;
	/* [한국어] TCP 최대 세그먼트 크기(TCP_MAXSEG). MTU 제약 환경 재현/단편화 실험용.
	 * 설정자: parse.c INT. 읽는 자: set_mss가 TCP일 때만 setsockopt 적용.
	 * 값 범위: 0(미설정) 또는 보통 536..1460. 동기화: 불변. CONFIG_NET_MSS 빌드 한정. */

	char *intfc;
	/* [한국어] 멀티캐스트 송출/수신에 사용할 인터페이스 IP 텍스트(예: "192.168.1.10").
	 * 설정자: parse.c STR_STORE(strdup). 읽는 자: connect 멀티캐스트 분기에서
	 *         IP_MULTICAST_IF, setup_listen_inet에서 IP_ADD_MEMBERSHIP의 imr_interface.
	 * 값 범위: NULL 또는 점-표기 IPv4 문자열. 옵션 이름이 "interface"인 점 주의.
	 * 동기화: 불변(잡 시작 후 free되지 않음). */
};

/*
 * [한국어] UDP 연결 상태 신호 메시지(OPEN/CLOSE).
 * UDP는 연결 없는(connectionless) 프로토콜이므로 스트림의 시작·종료를 애플리케이션
 * 레벨 매직으로 알려야 한다(TCP의 SYN/FIN과 유사한 역할). 양측이 매직과 cmd를
 * 매칭해 핸드쉐이크 동기화.
 *
 * wire 표현: 8바이트 고정. 모든 정수는 네트워크 바이트 순서(big-endian)로 저장.
 *   send_open: htonl(MAGIC) + htonl(OPEN), recv는 ntohl로 복원.
 *   send_close: cpu_to_le32(MAGIC) + cpu_to_le32(CLOSE), is_close_msg는 le32_to_cpu.
 *   ★ 주의: OPEN과 CLOSE가 엔디언이 다름(역사적/구현 한계). magic/cmd가 양쪽
 *           엔디언에서 모두 잘못 디코딩되면 일반 데이터로 간주.
 */
struct udp_close_msg {
	uint32_t magic;
	/* [한국어] 식별 매직(FIO_LINK_OPEN_CLOSE_MAGIC = 0x6c696e6b = "link" ASCII).
	 * 설정자: send_open(htonl)/send_close(cpu_to_le32). 읽는 자: udp_recv_open(ntohl)/
	 *         is_close_msg(le32_to_cpu).
	 * 값 범위: 항상 0x6c696e6b. 다른 값이면 무시(다른 송신자/타 프로토콜).
	 * 동기화: wire 프로토콜 — 잡 인스턴스 간 합의로 의미 부여. */

	uint32_t cmd;
	/* [한국어] FIO_LINK_OPEN(0x98) / FIO_LINK_CLOSE(0x89) — 핸드쉐이크 종류.
	 * 설정자: 위와 같음. 읽는 자: udp_recv_open이 OPEN인지 검증, is_close_msg가
	 *         CLOSE인지 검증. 값 범위: 두 상수 중 하나(다른 값이면 무시).
	 * 동기화: wire 프로토콜 매칭. */
};

/*
 * [한국어] 각 UDP 패킷의 페이로드 끝에 덮어쓰는 애플리케이션 레벨 검증 헤더.
 * 수신 측이 시퀀스 점프(=손실/재정렬)와 블록 크기 일치 여부를 자체 검증한다.
 *
 * verify=none(데이터 패턴 검증 비활성) 잡에서만 끼워 넣어진다 — verify가 활성화면
 * fio 코어가 페이로드 전체를 검증하므로 끝부분에 헤더를 덮는 것이 충돌. wire 표현은
 * 24바이트(매직+seq+bs 각 8바이트), 모두 little-endian(cpu_to_le64).
 */
struct udp_seq {
	uint64_t magic;
	/* [한국어] 매직 상수 FIO_UDP_SEQ_MAGIC(="ceUnqUse" ASCII LE 해석).
	 * 설정자: store_udp_seq(cpu_to_le64). 읽는 자: verify_udp_seq(le64_to_cpu).
	 * 값 범위: 0x657375716e556563ULL. 불일치면 검증 스킵.
	 * 동기화: wire 프로토콜. */

	uint64_t seq;
	/* [한국어] 송신 측이 단조 증가시키는 시퀀스 번호(0부터 시작).
	 * 설정자: store_udp_seq가 nd->udp_send_seq++. 읽는 자: verify_udp_seq가
	 *         nd->udp_recv_seq와 비교, 차이만큼 td->ts.drop_io_u에 누적.
	 * 값 범위: 0..2^64-1 (단조 증가). 동기화: wire 프로토콜. */

	uint64_t bs;
	/* [한국어] 본 페이로드 블록 크기(바이트) — 수신/송신 측의 bs 일치 검증용.
	 * 불일치 시 nd->seq_off=1로 이후 모든 검증 비활성(잡 단위 한 번만 트립).
	 * 설정자: store_udp_seq=io_u->xfer_buflen. 읽는 자: verify_udp_seq.
	 * 값 범위: io_u 버퍼 크기와 동일. 동기화: wire 프로토콜. */
};

/*
 * [한국어] 네트워크 엔진 내부에서 사용하는 식별 상수 enum.
 * 두 그룹으로 나뉜다:
 *   (1) FIO_LINK_*  : udp_close_msg의 magic/cmd 필드에 들어가는 값.
 *   (2) FIO_UDP_SEQ_MAGIC : udp_seq.magic 필드에 들어가는 값(64비트라 ULL 접미사).
 *   (3) FIO_TYPE_*  : netio_options.proto의 정수 식별자(parse 시 .posval 매핑 결과).
 */
enum {
	FIO_LINK_CLOSE = 0x89,
	/* [한국어] udp_close_msg.cmd가 CLOSE 시그널일 때의 값.
	 * 설정자: send_close. 읽는 자: is_close_msg. 동기화: wire 매칭. */

	FIO_LINK_OPEN_CLOSE_MAGIC = 0x6c696e6b,
	/* [한국어] udp_close_msg.magic 값(="link" ASCII LE/BE 모두 동일 해석).
	 * 설정자: send_open/send_close. 읽는 자: udp_recv_open/is_close_msg. */

	FIO_LINK_OPEN = 0x98,
	/* [한국어] udp_close_msg.cmd가 OPEN(잡 시작 핸드쉐이크) 시그널일 때의 값.
	 * 설정자: send_open. 읽는 자: udp_recv_open. */

	FIO_UDP_SEQ_MAGIC = 0x657375716e556563ULL,
	/* [한국어] udp_seq.magic 64비트 값 — 송신 측이 udp_seq 헤더를 끼워 넣었음을
	 * 표시. 다른 값이면 verify_udp_seq에서 검증 스킵(타 송신자/구버전 호환).
	 * ULL 접미사는 64비트 리터럴 보장. */

	FIO_TYPE_TCP		= 1,
	/* [한국어] proto=tcp — IPv4 SOCK_STREAM. 읽는 자: is_tcp(o). */

	FIO_TYPE_UDP		= 2,
	/* [한국어] proto=udp — IPv4 SOCK_DGRAM. 읽는 자: is_udp(o). */

	FIO_TYPE_UNIX		= 3,
	/* [한국어] proto=unix — AF_UNIX SOCK_STREAM(filesystem path 기반). */

	FIO_TYPE_TCP_V6		= 4,
	/* [한국어] proto=tcpv6 — IPv6 SOCK_STREAM. CONFIG_IPV6 빌드에서만 옵션 노출. */

	FIO_TYPE_UDP_V6		= 5,
	/* [한국어] proto=udpv6 — IPv6 SOCK_DGRAM. CONFIG_IPV6 한정. */

	FIO_TYPE_VSOCK_STREAM	= 6,
	/* [한국어] proto=vsock — AF_VSOCK SOCK_STREAM (VM guest↔host 통신).
	 * CONFIG_VSOCK 빌드 환경에서만 실제 socket 호출이 성공. */
};

/* [한국어] hostname= 옵션 파싱 콜백의 전방 선언. 아래 options[] 테이블에서 .cb로 참조되므로
 * 구현보다 먼저 이름이 필요하다. 실제 구현은 파일 하단(str_hostname_cb 정의)에 있다.
 * 콜백은 파서(parse.c)가 옵션 값을 STR_STORE로 처리하기 전에 .cb가 있으면 우선 호출하여
 * 사용자 정의 처리를 하게 해주는 hook이다. 본 엔진에서는 hostname을 td->o.filename으로
 * 복제 저장하여 파일 경로처럼 다루기 위해 사용한다. */
static int str_hostname_cb(void *data, const char *input);

/*
 * [한국어] net/netsplice 엔진의 커맨드라인/잡파일 옵션 테이블.
 *
 * 공통 규약:
 *  - 파서: fio의 옵션 파서(parse.c)가 이 배열을 .name 기준으로 선형 탐색.
 *  - 엔진 등록 시 ioengine_ops.options 포인터로 공유되어 "ioengine=net" 또는
 *    "ioengine=netsplice" 두 엔진 모두 동일 옵션 셋 사용.
 *  - .name      : 잡 파일/CLI에서 사용자가 지정하는 키워드.
 *  - .lname     : long name — `--cmdhelp`/`--enghelp` 출력에 노출되는 가독성 라벨.
 *  - .type      : 값 파싱 방식 — FIO_OPT_STR_STORE(문자열 strdup),
 *                 FIO_OPT_INT(정수, minval/maxval 검증), FIO_OPT_BOOL(0/1),
 *                 FIO_OPT_STR(.posval 매핑으로 enum 정수화),
 *                 FIO_OPT_STR_SET(존재 자체로 1, 값 불필요).
 *  - .off1      : offsetof(struct netio_options, member) — 파서가 옵션 값을
 *                 td->eo + .off1 위치에 직접 기록. 잡 시작 후에는 read-only.
 *  - .cb        : 사용자 정의 후처리 콜백(예: hostname을 td->o.filename으로 복제).
 *  - .def       : 기본값 문자열(파싱 후 .off1 위치에 설정됨).
 *  - .minval/.maxval: INT 타입의 입력 범위 검증.
 *  - .alias     : 다른 이름으로도 동일 옵션 인식(예: protocol↔proto).
 *  - .posval[]  : STR 타입의 허용 값 매핑(.ival=문자열, .oval=정수, .help=설명).
 *  - .category  : FIO_OPT_C_ENGINE — 엔진 옵션 분류.
 *  - .group     : FIO_OPT_G_NETIO — 네트워크 그룹(--enghelp=net 필터링).
 *  - #ifdef CONFIG_*: 빌드 시 지원 여부에 따라 옵션 노출 여부 결정.
 *  - 마지막 항목 .name=NULL : 파서의 종료 센티널(필수).
 */
static struct fio_option options[] = {
	{
		.name	= "hostname",                                  /* [한국어] CLI/잡파일 키 — 클라이언트 목적지 호스트 또는 서버 멀티캐스트 그룹 IP */
		.lname	= "net engine hostname",                       /* [한국어] --cmdhelp 출력 라벨 */
		.type	= FIO_OPT_STR_STORE,                           /* [한국어] 자유 문자열 저장(strdup); off1 대신 cb로 처리 */
		.cb	= str_hostname_cb,                             /* [한국어] td->o.filename으로 복제(파일 경로 슬롯 재사용) */
		.help	= "Hostname for net IO engine",                /* [한국어] --cmdhelp/--enghelp 도움말 */
		.category = FIO_OPT_C_ENGINE,                          /* [한국어] 엔진 옵션 분류 */
		.group	= FIO_OPT_G_NETIO,                             /* [한국어] 네트워크 그룹 */
	},
	{
		.name	= "port",                                      /* [한국어] TCP/UDP/VSOCK 포트 — 양측이 합의한 동일 값 사용 */
		.lname	= "net engine port",
		.type	= FIO_OPT_INT,                                 /* [한국어] 정수, minval/maxval 검증 */
		.off1	= offsetof(struct netio_options, port),        /* [한국어] netio_options.port 위치에 기록 */
		.minval	= 1,                                           /* [한국어] 0 거부(예약) */
		.maxval	= 65535,                                       /* [한국어] 16비트 포트 한계 */
		.help	= "Port to use for TCP or UDP net connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "protocol",                                  /* [한국어] proto 옵션 본명 */
		.lname	= "net engine protocol",
		.alias	= "proto",                                     /* [한국어] 짧은 별칭(잡파일에서 더 흔함) */
		.type	= FIO_OPT_STR,                                 /* [한국어] .posval 매핑으로 enum 정수화 */
		.off1	= offsetof(struct netio_options, proto),       /* [한국어] netio_options.proto 위치에 정수 기록 */
		.help	= "Network protocol to use",
		.def	= "tcp",                                       /* [한국어] 기본 TCP — 가장 흔한 사용 사례 */
		.posval = {
			  { .ival = "tcp",                              /* [한국어] 사용자 입력 문자열 */
			    .oval = FIO_TYPE_TCP,                       /* [한국어] netio_options.proto에 저장될 정수 */
			    .help = "Transmission Control Protocol",    /* [한국어] --enghelp 출력 */
			  },
#ifdef CONFIG_IPV6
			  { .ival = "tcpv6",                            /* [한국어] IPv6 TCP — getaddrinfo가 IPv6 결과만 반환 */
			    .oval = FIO_TYPE_TCP_V6,
			    .help = "Transmission Control Protocol V6",
			  },
#endif
			  { .ival = "udp",                              /* [한국어] UDP v4 — connectionless, sendto/recvfrom */
			    .oval = FIO_TYPE_UDP,
			    .help = "User Datagram Protocol",
			  },
#ifdef CONFIG_IPV6
			  { .ival = "udpv6",                            /* [한국어] UDP v6 */
			    .oval = FIO_TYPE_UDP_V6,
			    .help = "User Datagram Protocol V6",
			  },
#endif
			  { .ival = "unix",                             /* [한국어] AF_UNIX 스트림 — 동일 호스트 IPC */
			    .oval = FIO_TYPE_UNIX,
			    .help = "UNIX domain socket",
			  },
			  { .ival = "vsock",                            /* [한국어] AF_VSOCK 스트림 — VM guest↔host */
			    .oval = FIO_TYPE_VSOCK_STREAM,
			    .help = "Virtual socket",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#ifdef CONFIG_TCP_NODELAY
	{
		.name	= "nodelay",                                   /* [한국어] TCP_NODELAY = Nagle off */
		.lname	= "No Delay",
		.type	= FIO_OPT_BOOL,                                /* [한국어] 0(켬)/1(끔=NODELAY 적용) */
		.off1	= offsetof(struct netio_options, nodelay),
		.help	= "Use TCP_NODELAY on TCP connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#endif
	{
		.name	= "listen",                                    /* [한국어] 서버 모드 표시 */
		.lname	= "net engine listen",
		.type	= FIO_OPT_STR_SET,                             /* [한국어] 존재 자체로 1(값 불필요) */
		.off1	= offsetof(struct netio_options, listen),
		.help	= "Listen for incoming TCP connections",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "pingpong",                                  /* [한국어] 매 I/O 후 반대 방향 1회 — RTT 측정 */
		.lname	= "Ping Pong",
		.type	= FIO_OPT_STR_SET,                             /* [한국어] 부울 토글 */
		.off1	= offsetof(struct netio_options, pingpong),
		.help	= "Ping-pong IO requests",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "interface",                                 /* [한국어] 멀티캐스트 송수신 인터페이스 IP(점-표기) */
		.lname	= "net engine interface",
		.type	= FIO_OPT_STR_STORE,                           /* [한국어] strdup(intfc) */
		.off1	= offsetof(struct netio_options, intfc),
		.help	= "Network interface to use",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "ttl",                                       /* [한국어] 멀티캐스트 IP_MULTICAST_TTL */
		.lname	= "net engine multicast ttl",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, ttl),
		.def    = "1",                                          /* [한국어] 기본 TTL=1 (같은 서브넷만) — 멀티캐스트 안전 기본값 */
		.minval	= 0,                                            /* [한국어] 0=같은 호스트만(루프백) */
		.help	= "Time-to-live value for outgoing UDP multicast packets",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#ifdef CONFIG_NET_WINDOWSIZE
	{
		.name	= "window_size",                               /* [한국어] SO_RCVBUF/SNDBUF 크기 */
		.lname	= "Window Size",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, window_size),
		.minval	= 0,                                            /* [한국어] 0=커널 기본 사용 */
		.help	= "Set socket buffer window size",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#endif
#ifdef CONFIG_NET_MSS
	{
		.name	= "mss",                                       /* [한국어] TCP_MAXSEG */
		.lname	= "Maximum segment size",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct netio_options, mss),
		.minval	= 0,                                            /* [한국어] 0=설정 안 함 */
		.help	= "Set TCP maximum segment size",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_NETIO,
	},
#endif
	{
		.name	= NULL,                                        /* [한국어] 종료 센티널 — parse.c가 .name==NULL을 보면 순회 종료 */
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
 *
 * @td: 현재 잡 컨텍스트(에러 보고용 td_verror).
 * @fd: 설정 대상 소켓 FD.
 * @return: 0=성공/미적용, <0=에러.
 *
 * 소켓 옵션 스펙(POSIX/Linux):
 *   - SO_RCVBUF: 수신 큐(커널 skb 누적) 최대 바이트. 커널은 내부 부하 계산용으로 요청값의
 *     2배를 sk->sk_rcvbuf에 저장하고, sysctl net.core.rmem_max를 상한으로 클램프한다.
 *     (SO_RCVBUFFORCE는 CAP_NET_ADMIN 필요, 본 엔진 미사용)
 *   - SO_SNDBUF: 송신 큐 최대 바이트. 마찬가지로 net.core.wmem_max로 클램프.
 *
 * 적용 규칙:
 *   - o->window_size=0이면 커널 기본값 사용(미적용 후 성공 반환).
 *   - o->listen || o->pingpong 시 RCVBUF 설정(서버는 수신 중심, pingpong은 양방향).
 *   - !o->listen || o->pingpong 시 SNDBUF 설정(클라는 송신 중심).
 *   - 한쪽 실패 시 반대쪽은 시도하지 않는다(ret 전파).
 *
 * 컴파일 타임에 CONFIG_NET_WINDOWSIZE 미지원이면 EINVAL 반환(옵션 자체가 사용자에게
 * 노출되지 않지만 엔진 내부 호출 방어).
 *
 * 호출 체인: fio_netio_connect()/fio_netio_setup_listen_inet() → [set_window_size]
 *            → setsockopt(2).
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
 *
 * @td: 에러 보고 대상 잡 컨텍스트.
 * @fd: 대상 TCP 소켓 FD.
 * @return: 0=성공/비적용, <0=에러.
 *
 * TCP_MAXSEG(level IPPROTO_TCP): TCP 세그먼트 최대 크기를 강제한다. 일반적으로 MSS는
 * TCP 핸드쉐이크 시 피어 간 MSS 옵션 교환으로 자동 결정(MTU 1500 → MSS 1460 기본)되지만,
 * 본 옵션은 fio가 의도적으로 작은 값을 설정해 fragmentation/다수 세그먼트화 경로를 재현할
 * 수 있게 한다. 커널은 IP/TCP 헤더(40바이트) 감산을 고려하므로 0=자동.
 *
 * 유효성:
 *   - o->mss=0 또는 is_tcp 아님 → no-op.
 *   - TCP면 setsockopt로 값 설정. 커널은 IP MTU 이하로만 허용(EINVAL 가능).
 *
 * 호출 체인: fio_netio_connect()/fio_netio_setup_listen_inet() → [set_mss]
 *            → setsockopt(2, IPPROTO_TCP, TCP_MAXSEG).
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
 *
 * @td: 잡 컨텍스트(종료 플래그 td->terminate 체크용).
 * @fd: 대기할 소켓/파이프 FD.
 * @events: 기다릴 이벤트 비트마스크(POLLIN=수신 가능/POLLOUT=송신 가능/POLLPRI 등).
 * @return: 1=원하는 이벤트 발생, -1=에러·시그널로 중단 또는 이벤트 불일치.
 *
 * 동기 엔진(FIO_SYNCIO)이므로 fio_netio_send/recv/accept에서 blocking 대기점으로 사용된다.
 * poll(2)은 timeout=-1로 무한 대기하며 다음 3개 경로에서 깨어난다:
 *   1) fd가 요청 이벤트 상태 도달 (ret>0, revents에 마스크 기록).
 *   2) 시그널 수신(terminate에서 SIGTERM self-send) → ret<0 && errno==EINTR → 루프 탈출.
 *   3) 에러(EBADF 등) → td_verror 기록 후 -1 반환.
 *
 * td->terminate가 설정되면 while 루프의 다음 반복에서 빠져나가 -1 반환하도록 설계(블로킹
 * 중이라도 SIGTERM이 poll을 깨워 다음 반복에서 terminate를 검사하게 됨). timeout=0(타임아웃
 * 반환)은 -1 지정 시 이론상 오지 않지만 방어적으로 continue 처리.
 *
 * 호출 체인: fio_netio_send(POLLOUT)/fio_netio_recv(POLLIN)/fio_netio_accept(POLLIN)
 *            → [poll_wait] → poll(2).
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
 *
 * @td:   잡 컨텍스트(옵션·fio_file 참조용).
 * @io_u: 곧 td_io_queue()로 넘겨질 I/O 유닛 — ddir/offset/xfer_buflen은 이미 코어가 결정.
 * @return: 0=정상(queue 진행), 1=방향 오류(잡 중단).
 *
 * fio 코어는 get_io_u() 직후 매 io_u마다 엔진의 .prep를 호출해 엔진별 전처리를 허용한다.
 * 파일시스템 엔진은 여기서 파일 포인터 조정 등을 하지만, 네트워크 엔진은 offset 개념이
 * 없으므로 UDP의 "잡 방향 ↔ listen 역할" 규약만 검증한다:
 *   - UDP listen=1 (서버·read 잡) 가 DDIR_WRITE io_u를 받으면 비정상(수신 전용)
 *   - UDP listen=0 (클라·write 잡) 가 DDIR_READ io_u를 받으면 비정상(송신 전용)
 * TCP는 양방향 스트림이라 pingpong/혼합 잡에서 양쪽 방향 io_u가 정상이므로 통과.
 *
 * 실행 컨텍스트: 잡 스레드 — queue 호출 직전. 실패는 td_verror로 통계에 기록 후 코어가
 * 잡을 EINVAL 종료하므로 복구 경로는 없다.
 *
 * 호출 체인: backend.c 잡 루프 → td_io_prep() → ioengines.c td->io_ops->prep()
 *            → [fio_netio_prep].
 */
static int fio_netio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct netio_options *o = td->eo;            /* [한국어] 프로토콜/listen 등 옵션 참조(불변). */

	/*
	 * Make sure we don't see spurious reads to a receiver, and vice versa
	 */
	if (is_tcp(o))                               /* [한국어] TCP는 전이중 스트림 — READ/WRITE 모두 허용하므로 조기 return 0. */
		return 0;

	if ((o->listen && io_u->ddir == DDIR_WRITE) ||
	    (!o->listen && io_u->ddir == DDIR_READ)) {/* [한국어] 서버=수신, 클라=송신이라는 UDP 규약 위반 — 잡 설정 오류로 간주. */
		td_verror(td, EINVAL, "bad direction"); /* [한국어] td->error=EINVAL 설정 + 통계 기록 — 코어가 잡 종료 판정. */
		return 1;
	}

	return 0;                                    /* [한국어] 방향 규약 일치 — queue 호출 허용. */
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
 *
 * @td: 잡 컨텍스트. @io_u: 송신 버퍼(xfer_buf/xfer_buflen 사용).
 * @return: 송신 바이트 수(>0) 또는 음수 에러.
 *
 * 송신 경로 선택:
 *   - UDP(v4/v6): sendto(2) — peer 주소를 매 호출마다 지정(connectionless).
 *     verify=none 잡에서는 store_udp_seq로 버퍼 끝에 udp_seq 헤더 삽입(시퀀스 검증용).
 *   - TCP/UNIX: send(2) — 이미 연결된 스트림이라 주소 불필요.
 *
 * send/sendto 플래그 해설(본 엔진이 사용하는 것):
 *   - MSG_MORE     : "곧 더 보낼 데이터가 있음" — 커널이 패킷을 즉시 내보내지 않고 축적.
 *                    Nagle + TCP_CORK와 유사한 효과를 호출별 힌트로 줌. 본 엔진은 마지막
 *                    패킷이 아니고 pingpong이 아닐 때만 설정(MSG_MORE를 set하면 그 send는
 *                    flush하지 않고, 다음 send에서 누적본과 함께 전송).
 *   - 미사용이지만 관련 플래그:
 *     MSG_NOSIGNAL: SIGPIPE 대신 EPIPE 반환(본 엔진은 SIGPIPE를 무시 설정 가정).
 *     MSG_DONTWAIT: 비블로킹 힌트 — 본 엔진은 blocking 모드라 불필요.
 *     MSG_OOB     : out-of-band 데이터(TCP URG) — 본 엔진 미지원.
 *
 * 재시도 루프: send가 0/음수를 반환하면 poll_wait(POLLOUT)으로 송신 가능 상태가 될 때까지
 * 블로킹 대기 후 재시도. poll_wait 실패(종료/에러) 시 루프 탈출.
 *
 * 호출 체인: __fio_netio_queue(DDIR_WRITE) → [fio_netio_send]
 *            → send(2)/sendto(2) + 필요 시 poll_wait → poll(2).
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
 *
 * @td: 잡 컨텍스트. @io_u: 수신 버퍼(xfer_buf/xfer_buflen 사용).
 * @return: 수신 바이트 수(>0), 0(상대가 CLOSE 표명 — td->done=1 후), 음수(에러).
 *
 * 수신 경로:
 *   - UDP listen 측: recvfrom으로 peer 주소 캡처(응답 echo에서 sendto 목적지로 사용).
 *   - UDP client 측: from=NULL (이미 connect 없이 sendto만 할 예정).
 *   - TCP/UNIX : recv(2) — 연결 단위라 주소 불필요.
 *
 * recv/recvfrom 플래그 해설:
 *   - MSG_WAITALL : 요청 바이트 수가 모두 채워질 때까지 커널 내부에서 반복 수신.
 *                   본 엔진은 초기 호출에 플래그 없이 사용하다가, 부분 수신 시
 *                   재시도 루프에서 flags|=MSG_WAITALL로 전환해 커널에 전량 대기 요청.
 *                   TCP는 메시지 경계가 없어 short receive 이슈를 감추는 가장 안전한 방법.
 *                   UDP는 데이터그램이라 MSG_WAITALL의 효과가 제한적(큰 영향 없음).
 *   - MSG_DONTWAIT: 비블로킹. 본 엔진은 blocking 모드라 사용 안 함.
 *   - MSG_PEEK    : 큐에서 꺼내지 않고 미리보기. 본 엔진 미사용.
 *   - MSG_TRUNC   : 데이터그램이 버퍼보다 클 때 진짜 길이 반환. UDP만 의미.
 *
 * CLOSE 감지: 수신 데이터가 sizeof(udp_close_msg) && 매직/cmd가 CLOSE면 애플리케이션 종료
 * 신호로 간주하고 td->done=1을 세워 잡 메인 루프 탈출을 유도, 0을 반환.
 *
 * 재시도 루프: recv가 0/음수면 poll_wait(POLLIN) 후 재시도(MSG_WAITALL 추가). 시그널/종료
 * 상황은 poll_wait가 -1 반환으로 전파.
 *
 * verify_udp_seq: UDP이고 verify=none이면 수신 직후 udp_seq 검증으로 시퀀스 점프(손실/재
 * 정렬)를 측정.
 *
 * 호출 체인: __fio_netio_queue(DDIR_READ) → [fio_netio_recv]
 *            → recv(2)/recvfrom(2) + poll_wait → poll(2).
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
 * __fio_netio_queue - 방향별(ddir) 저수준 I/O 디스패처.
 *
 * @td:   잡 컨텍스트(옵션·netio_data 참조).
 * @io_u: 전송/수신 대상 I/O 유닛(xfer_buf/xfer_buflen/file->fd 사용).
 * @ddir: 요청 방향(DDIR_READ / DDIR_WRITE / DDIR_SYNC).
 * @return: FIO_Q_COMPLETED(전량·부분 전송 성공 또는 에러) / FIO_Q_BUSY(재시도 요청).
 *
 * fio_netio_queue의 1차/2차(pingpong) 방향 모두에서 공용으로 사용되는 내부 dispatcher.
 * 분기 규칙:
 *   - use_splice=0(net 엔진)  → 일반 send/recv(TCP/UNIX 스트림) 또는 sendto/recvfrom(UDP).
 *   - use_splice=1 && TCP      → splice_in/out(TCP 소켓↔커널 파이프 페이지 이동).
 *   - use_splice=1 && UDP/UNIX → splice가 UDP/UNIX에는 의미 없어 일반 send/recv로 폴백.
 *
 * 반환 규약 해석:
 *   1) ret == xfer_buflen : 전량 전송 성공 → COMPLETED, resid=0.
 *   2) 0 < ret < xfer_buflen : short transfer — resid에 남은 바이트 기록 후 COMPLETED.
 *      fio 코어가 부분 전송으로 통계에 반영, 재시도는 발생하지 않음(네트워크 스트림은
 *      상대 버퍼 상황에 따라 자연스러운 partial 허용).
 *   3) ret == 0 : 전혀 진전 없음(poll 실패 등) → FIO_Q_BUSY로 코어에 재시도 요청.
 *   4) ret < 0  : errno 설정된 실패. DDIR_WRITE+EMSGSIZE(UDP 데이터그램 크기 초과)는
 *                 BUSY로 반환해 상위가 bs를 줄여 재시도하도록 유도. 그 외 errno는
 *                 io_u->error에 기록 후 COMPLETED(코어가 잡 종료 판정).
 *
 * 실행 컨텍스트: 잡 스레드 전용. 내부에서 blocking send/recv/poll이 일어날 수 있음.
 * 호출 체인: fio_netio_queue() → [__fio_netio_queue] → fio_netio_{send,recv,splice_in,
 *            splice_out}() → send(2)/recv(2)/sendto(2)/recvfrom(2)/splice(2)/vmsplice(2).
 */
static enum fio_q_status __fio_netio_queue(struct thread_data *td,
					   struct io_u *io_u,
					   enum fio_ddir ddir)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] use_splice 플래그와 주소/파이프 슬롯 보유. */
	struct netio_options *o = td->eo;            /* [한국어] proto 참조(is_udp/UNIX 판정용). */
	int ret;                                     /* [한국어] 저수준 send/recv/splice 반환(음수 에러/0 EOF/양수 바이트). */

	if (ddir == DDIR_WRITE) {
		if (!nd->use_splice || is_udp(o) ||
		    o->proto == FIO_TYPE_UNIX)       /* [한국어] net 엔진이거나 UDP/UNIX는 splice 의미 없음 — 일반 송신 경로. */
			ret = fio_netio_send(td, io_u);
		else
			ret = fio_netio_splice_out(td, io_u); /* [한국어] netsplice TCP 경로 — io_u→파이프→소켓 2단 이동. */
	} else if (ddir == DDIR_READ) {
		if (!nd->use_splice || is_udp(o) ||
		    o->proto == FIO_TYPE_UNIX)
			ret = fio_netio_recv(td, io_u); /* [한국어] 일반 수신 경로(recv/recvfrom). */
		else
			ret = fio_netio_splice_in(td, io_u); /* [한국어] netsplice TCP 경로 — 소켓→파이프→io_u. */
	} else
		ret = 0;	/* must be a SYNC */ /* [한국어] 네트워크 엔진은 DDIR_SYNC를 no-op로 처리(TCP은 상대 ACK로 "sync" 의미 없음). */

	if (ret != (int) io_u->xfer_buflen) {        /* [한국어] 요청한 크기와 다르면 에러/부분/EOF 경로 분기. */
		if (ret > 0) {                       /* [한국어] 부분 전송(short transfer) — 스트림 소켓에서 자연 발생 가능. */
			io_u->resid = io_u->xfer_buflen - ret; /* [한국어] 남은 바이트를 resid에 기록 — 코어가 부분 IO로 stat 누적. */
			io_u->error = 0;             /* [한국어] 에러 아님(short는 성공 변종). */
			return FIO_Q_COMPLETED;
		} else if (!ret)                     /* [한국어] ret==0: poll 타임아웃/EOF 등 진전 전혀 없음 — 코어 재시도 요청. */
			return FIO_Q_BUSY;
		else {
			int err = errno;             /* [한국어] send/recv가 설정한 errno 스냅샷(이후 td_verror가 덮어쓸 수 있음). */

			if (ddir == DDIR_WRITE && err == EMSGSIZE) /* [한국어] UDP 최대 데이터그램 크기 초과(IP MTU/64KiB 한계) — bs 축소해 재시도 유도. */
				return FIO_Q_BUSY;

			io_u->error = err;           /* [한국어] 그 외 에러는 io_u에 기록 — 코어가 잡 종료 결정. */
		}
	}

	if (io_u->error)                             /* [한국어] 에러가 있는 경우에만 통계 상세 기록("xfer" 라벨). */
		td_verror(td, io_u->error, "xfer");

	return FIO_Q_COMPLETED;                      /* [한국어] 동기 엔진 — 단일 호출로 완료 보고. 코어가 즉시 put_io_u. */
}

/*
 * [한국어]
 * fio_netio_queue - net 엔진의 I/O 제출 콜백 진입점(ioengine_ops.queue).
 *
 * @td:   잡 컨텍스트(ts/eo/io_ops_data 등 참조 소유자).
 * @io_u: 코어가 준비한 I/O 유닛 — ddir/xfer_buf/xfer_buflen/file/offset 필드 사용.
 * @return: FIO_Q_COMPLETED(성공·부분 전송 포함, 코어가 즉시 put_io_u) /
 *          FIO_Q_BUSY(진전 없음 — 코어가 잠시 후 재시도) /
 *          FIO_Q_QUEUED(본 엔진은 동기라 반환하지 않음).
 *
 * 본 엔진은 FIO_SYNCIO 플래그를 단 "동기 엔진"이므로 queue 호출 하나에서 send/recv
 * 와 필요한 경우 poll 대기까지 모두 수행한 뒤 반환한다. 비동기 엔진과 달리 .commit /
 * .getevents / .event 콜백은 정의하지 않으며 ioengines.c 코어가 COMPLETED 반환을
 * 보고 즉시 io_u_sync_complete → put_io_u 경로로 정리한다.
 *
 * pingpong 모드는 한 I/O 라운드 안에서 본 방향 완료 뒤 반대 방향을 연속 호출해
 * RTT(Round-Trip Time)를 단일 queue 호출에 포함시킨다. 이 때문에 실제 측정되는
 * clat는 "요청 송신 + 응답 수신 + 반대 경로 처리"의 누적이 된다.
 *
 * 실행 컨텍스트: 잡 스레드 전용(블로킹 구간 포함).
 * 호출 체인: backend.c 잡 루프 → td_io_queue() → ioengines.c
 *            td->io_ops->queue() → [fio_netio_queue] → __fio_netio_queue()
 *            → send(2)/sendto(2)/recv(2)/recvfrom(2)/splice(2)/vmsplice(2)
 *            → poll(2)(필요 시 대기).
 * 에러 경로: io_u->error에 errno 기록 후 COMPLETED 반환(코어가 에러 잡 종료 결정).
 */
static enum fio_q_status fio_netio_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	struct netio_options *o = td->eo;            /* [한국어] pingpong/proto 등 옵션 참조(불변 — init 이후 read-only). */
	int ret;                                     /* [한국어] 1차 방향 큐잉 결과(fio_q_status 값). */

	fio_ro_check(td, io_u);                      /* [한국어] readonly 잡(td->o.read_only)인데 write io_u가 오면 assert — I/O 엔진 공통 가드. */

	ret = __fio_netio_queue(td, io_u, io_u->ddir);/* [한국어] 1차 방향 실행(io_u->ddir이 그대로 — DDIR_READ/WRITE/SYNC). */
	if (!o->pingpong || ret != FIO_Q_COMPLETED)  /* [한국어] pingpong=0(일반 단방향)이거나 1차가 실패/BUSY면 반대 방향 생략. */
		return ret;

	/*
	 * For ping-pong mode, receive or send reply as needed
	 */
	if (td_read(td) && io_u->ddir == DDIR_READ)  /* [한국어] 서버 측(read 잡): 수신 완료 후 동일 버퍼를 클라에 echo(WRITE) — RTT 응답 생성. */
		ret = __fio_netio_queue(td, io_u, DDIR_WRITE);
	else if (td_write(td) && io_u->ddir == DDIR_WRITE) /* [한국어] 클라 측(write 잡): 송신 완료 후 서버 응답을 수신(READ) — 왕복 시간 포함. */
		ret = __fio_netio_queue(td, io_u, DDIR_READ);

	return ret;                                  /* [한국어] 반대 방향 결과를 그대로 반환 — 코어가 stat 누적에 반영. */
}

/*
 * [한국어]
 * fio_netio_connect - 클라이언트 모드에서 프로토콜별 소켓 생성 및 서버 연결.
 *
 * @td: 잡 컨텍스트(옵션·netio_data·에러 보고 채널).
 * @f:  이 연결을 대표할 fio_file — 성공 시 f->fd에 유효 소켓 FD 기록.
 * @return: 0=성공, 1=실패(에러는 td_verror/log_err로 보고됨).
 *
 * 동작 단계:
 *   1) proto → (domain, type) 매핑으로 socket(2) 호출(AF_INET·AF_INET6·AF_UNIX·AF_VSOCK).
 *   2) TCP인 경우 nodelay 옵션 시 TCP_NODELAY setsockopt(Nagle 알고리즘 비활성화 —
 *      작은 쓰기를 즉시 패킷화해 저지연 응용 재현).
 *   3) 모든 타입에 SO_SNDBUF/RCVBUF(window_size) 및 TCP_MAXSEG(mss) 옵션 적용.
 *   4) UDP 분기:
 *      - 유니캐스트 UDP   : connect 없이 바로 반환(sendto가 매 패킷마다 주소 지정).
 *      - 멀티캐스트 UDP v4: IP_MULTICAST_IF(인터페이스 강제) + IP_MULTICAST_TTL(홉 제한)
 *        설정. IPv6 멀티캐스트는 본 구현 미지원(에러).
 *   5) TCP/VSOCK/UNIX: connect(2) 호출 — 커널이 3-way handshake(TCP) 또는 서버 accept
 *      큐에 요청을 넣는다. 실패 시 FD close하고 1 반환.
 *
 * 실행 컨텍스트: 잡 스레드, open_file 경로에서 1회. connect(2)는 블로킹이므로 서버가
 * 없으면 ETIMEDOUT/ECONNREFUSED까지 대기.
 * 호출 체인: fio_netio_open_file → [fio_netio_connect] → socket(2)/setsockopt(2)/
 *            connect(2)/IP_MULTICAST_* setsockopt.
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
 *
 * @td: 잡 컨텍스트(listenfd가 io_ops_data에 있음).
 * @f:  수락한 연결을 담을 fio_file — 성공 시 f->fd에 피어 소켓 FD 기록.
 * @return: 0=성공, 1=실패.
 *
 * TCP/VSOCK 흐름:
 *   1) runstate를 TD_SETTING_UP으로 전환 — 대기 구간이 런타임 통계에서 분리됨.
 *   2) poll_wait(POLLIN)으로 listenfd에 연결 요청 도착 대기(td->terminate 반응).
 *   3) accept(2) — 커널이 완성된 연결(3-way handshake 끝난 TCP의 경우)을 accept 큐에서
 *      꺼내 새 소켓을 할당. 피어 주소 구조체(addr/addr6/addr_vm)에 기록.
 *   4) nodelay 옵션이면 새 피어 소켓에도 TCP_NODELAY 적용(listenfd가 아닌 피어 소켓에
 *      소켓 옵션이 상속되므로 재설정 필요).
 *   5) reset_all_stats(td) — 연결 대기 중 누적된 기간/IOPS 통계 제거.
 *   6) 원래 runstate 복원.
 *
 * UDP 흐름: connectionless라 accept 개념 자체가 없어 listenfd를 그대로 f->fd로 복사.
 * 이후 recvfrom에서 피어 주소를 동적으로 캡처(응답 송신 시 사용).
 *
 * 실행 컨텍스트: 잡 스레드, open_file 경로. poll_wait은 무한 블로킹 가능(외부 SIGTERM
 * 으로 깨어남).
 * 호출 체인: fio_netio_open_file → [fio_netio_accept] → poll_wait → poll(2) + accept(2).
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
 *
 * @td: 잡 컨텍스트. @f: 이 연결을 대표할 fio_file(f->fd에 소켓 FD 기록됨).
 * @return: 0=성공(잡 루프 진입 가능), 그 외=실패(fd=-1, 코어가 잡 중단).
 *
 * 코어(filesetup.c)가 잡 루프 시작 전 td_io_open_file()을 통해 호출. 본 엔진에서는
 * 이 단계에서 비로소 실제 커널 리소스(connected 소켓 또는 accepted 소켓)가 만들어진다.
 * init 단계에서는 단지 listenfd(서버) 또는 목적지 주소 구조체(클라)까지만 준비했다.
 *
 * 동작 흐름:
 *   1) listen=1 → fio_netio_accept: TCP/VSOCK면 poll+accept, UDP면 listenfd를 그대로 fd로.
 *   2) listen=0 → fio_netio_connect: socket(2)+setsockopt+connect(2).
 *   3) UDP 추가 핸드쉐이크:
 *      - td_write 잡(=클라) → send_open: LINK_OPEN 매직 sendto.
 *      - td_read 잡(=서버)  → udp_recv_open: recvfrom으로 OPEN 대기, runstate를
 *        TD_SETTING_UP으로 바꿔 통계에서 대기 시간을 분리한 뒤 복원.
 *   4) 1~3 중 하나라도 실패하면 close_file로 자동 정리(fd leak 방지).
 *
 * 실행 컨텍스트: 잡 스레드. 서버 측은 accept/recvfrom에서 블로킹(외부 SIGTERM이
 * fio_netio_terminate 경유로 깨움).
 * 호출 체인: backend.c → td_io_open_file() → filesetup.c get_file_or_default
 *            → [fio_netio_open_file] → {accept,connect,send_open,udp_recv_open,close_file}.
 */
static int fio_netio_open_file(struct thread_data *td, struct fio_file *f)
{
	int ret;                                     /* [한국어] 하위 accept/connect/OPEN 반환. */
	struct netio_options *o = td->eo;            /* [한국어] listen/proto 분기 판정용. */

	if (o->listen)                               /* [한국어] 서버 모드 경로(accept 또는 UDP listenfd 재사용). */
		ret = fio_netio_accept(td, f);
	else                                         /* [한국어] 클라이언트 모드 경로(socket+connect). */
		ret = fio_netio_connect(td, f);

	if (ret) {                                   /* [한국어] 연결 실패 시 fd 무효화 후 반환(코어가 잡 중단). */
		f->fd = -1;
		return ret;
	}

	if (is_udp(o)) {                             /* [한국어] UDP는 connectionless라 애플리케이션 레벨 OPEN 핸드쉐이크 필요. */
		if (td_write(td))                    /* [한국어] 송신 잡(클라이언트)이면 먼저 OPEN 메시지 송신. */
			ret = fio_netio_send_open(td, f);
		else {                               /* [한국어] 수신 잡(서버)이면 OPEN 대기 — runstate 분리로 통계 왜곡 방지. */
			int state;                   /* [한국어] 원래 runstate 백업. */

			state = td->runstate;        /* [한국어] 현재 상태 보존(TD_RUNNING 등). */
			td_set_runstate(td, TD_SETTING_UP); /* [한국어] 대기 시간은 "설정 구간" 통계로 분리. */
			ret = fio_netio_udp_recv_open(td, f); /* [한국어] OPEN 수신 시 td->start 시각 고정. */
			td_set_runstate(td, state);  /* [한국어] 원 상태 복원. */
		}
	}

	if (ret)                                     /* [한국어] 어느 단계라도 실패 → 소켓 자동 정리(fd leak 방지). */
		fio_netio_close_file(td, f);

	return ret;                                  /* [한국어] 0/에러 전파 — 0이면 잡 루프 진입 승인. */
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
 *
 * @td: 잡 컨텍스트(nd에 listenfd 기록).
 * @port: 바인딩할 포트(호스트 엔디언 — 내부에서 htons로 네트워크 엔디언 변환).
 * @return: 0=성공, 1=실패.
 *
 * 수행 순서:
 *   1) proto에서 (domain, type) 결정(TCP SOCK_STREAM / UDP SOCK_DGRAM, IPv4/IPv6).
 *   2) socket(2) — 서버 소켓 생성.
 *   3) SO_REUSEADDR — TIME_WAIT 상태 포트 재사용(잡 재시작 시 bind 거부 방지).
 *   4) SO_REUSEPORT(지원 시) — 다중 프로세스/스레드 동일 포트 bind 허용(커널이 accept/
 *      datagram을 해시 기반으로 분산 — fio numjobs와 결합 시 로드 밸런싱).
 *   5) set_window_size / set_mss — 사용자 지정 SO_RCVBUF/SO_SNDBUF/TCP_MAXSEG 적용.
 *   6) filename이 설정된 경우 반드시 멀티캐스트 주소(224.0.0.0/4)여야 하며:
 *      a) IP_ADD_MEMBERSHIP — 커널에 IGMPv2/v3 join 요청 전송(해당 그룹 멀티캐스트 수신).
 *      b) imr_interface = o->intfc 있으면 해당 IP 인터페이스 / 없으면 INADDR_ANY(전 인터페이스).
 *   7) 바인딩 주소 결정:
 *      - IPv4: sin_addr = 멀티캐스트 그룹이면 그 주소, 아니면 INADDR_ANY(모든 인터페이스).
 *      - IPv6: in6addr_any 고정(멀티캐스트 IPv6은 본 구현 미지원).
 *   8) bind(2) — 주소/포트를 소켓에 할당.
 *
 * 주의: 본 함수는 listen(2)을 호출하지 않음 — 상위 fio_netio_setup_listen이 TCP인 경우에만
 * listen(fd, 10)으로 accept 큐를 활성화한다. UDP는 연결 없이 bind만으로 수신 준비 완료.
 *
 * 호출 체인: fio_netio_setup_listen → [setup_listen_inet] → socket/bind/setsockopt +
 *            IP_ADD_MEMBERSHIP(멀티캐스트).
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

	fd = socket(AF_VSOCK, type, 0);              /* [한국어] AF_VSOCK 소켓 생성 — Linux transport(virtio-vsock/vhost-vsock)로 hypervisor↔guest 통신. */
	if (fd < 0) {                                /* [한국어] EAFNOSUPPORT 등 — 커널 모듈 미로드 또는 권한 문제. */
		td_verror(td, errno, "socket");
		return 1;
	}

	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &opt, sizeof(opt)) < 0) {
		td_verror(td, errno, "setsockopt"); /* [한국어] 재바인딩 허용(이전 잡의 소켓이 TIME_WAIT 상태인 경우 대비). */
		close(fd);                           /* [한국어] 실패 시 FD leak 방지. */
		return 1;
	}

	len = sizeof(*addr);                         /* [한국어] bind에 전달할 주소 길이. */

	nd->addr_vm.svm_family = AF_VSOCK;           /* [한국어] family 고정 — bind/accept가 family 매칭 검사. */
	nd->addr_vm.svm_cid = VMADDR_CID_ANY;        /* [한국어] 어떤 CID(Context ID)에서 오는 연결이든 허용. CID=0(hypervisor)/2(host)/3+(guest) 전부 수락. */
	nd->addr_vm.svm_port = port;                 /* [한국어] VSOCK 포트(네트워크 바이트 순서 변환 불필요 — 커널이 호스트 엔디언 사용). */

	if (bind(fd, (struct sockaddr *) addr, len) < 0) { /* [한국어] VSOCK transport에 (CID_ANY, port) 바인딩. */
		td_verror(td, errno, "bind");
		close(fd);
		return 1;
	}

	nd->listenfd = fd;                           /* [한국어] 바인딩된 소켓을 listenfd로 보존 — 이후 listen/accept 대상. */
	return 0;
#else
	td_verror(td, -EINVAL, "vsock not supported"); /* [한국어] CONFIG_VSOCK 미지원 빌드 — linux/vm_sockets.h 부재. */
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
	struct netio_data *nd = td->io_ops_data;     /* [한국어] listenfd 저장 대상. */
	struct netio_options *o = td->eo;            /* [한국어] proto/port 참조. */
	int ret;                                     /* [한국어] 하위 setup_listen_* 반환 코드. */

	if (is_udp(o) || is_tcp(o))                  /* [한국어] IPv4/IPv6 TCP·UDP — inet 공통 경로(SO_REUSEADDR/PORT+bind+멀티캐스트 처리). */
		ret = fio_netio_setup_listen_inet(td, o->port);
	else if (is_vsock(o))                        /* [한국어] VSOCK 스트림 — STREAM type 전달(DGRAM은 본 엔진 미사용). */
		ret = fio_netio_setup_listen_vsock(td, o->port, SOCK_STREAM);
	else                                         /* [한국어] Unix 도메인 — filename이 소켓 파일 경로. */
		ret = fio_netio_setup_listen_unix(td, td->o.filename);

	if (ret)                                     /* [한국어] 프로토콜별 bind 실패 시 그대로 전파. */
		return ret;
	if (is_udp(o))                               /* [한국어] UDP는 connectionless — listen(2)/accept(2) 단계 생략. */
		return 0;

	if (listen(nd->listenfd, 10) < 0) {          /* [한국어] TCP listen(2) — backlog=10 (3-way handshake 완료 후 accept 대기 큐 한계). 실제 한계는 min(10, net.core.somaxconn). fio 테스트 규모에서는 충분. SYN queue(SYN-ACK 대기)는 net.ipv4.tcp_max_syn_backlog로 별도 관리. */
		td_verror(td, errno, "listen");
		nd->listenfd = -1;                   /* [한국어] 실패 표시 — cleanup이 close(-1)를 건너뛰도록. */
		return 1;
	}

	return 0;                                    /* [한국어] listen 완료 — 이후 open_file에서 accept로 진입. */
}

/*
 * [한국어]
 * fio_netio_init - net/netsplice 엔진의 init 콜백(ioengine_ops.init).
 *
 * @td:     잡 컨텍스트(옵션/runstate 접근).
 * @return: 0=성공(다음 단계 open_file 진입 가능), 1=실패(잡 중단).
 *
 * 잡 시작 시 ioengines.c가 td_io_init()을 통해 호출하며, 이 시점에 setup 단계는
 * 이미 완료된 상태(netio_data 할당, fio_file 등록 완료). 본 함수는 다음을 수행:
 *   1) Windows 플랫폼이면 WSAStartup로 Winsock 2.2 DLL을 프로세스 당 1회 로드.
 *   2) td_random 방지 — 네트워크 소켓은 오프셋 개념이 없어 "랜덤 I/O"가 무의미.
 *   3) 프로토콜별 port 필수성 검증(UNIX는 포트 무관, VSOCK/TCP/UDP는 필수).
 *   4) 서브잡 번호만큼 port에 오프셋 추가 — numjobs>1 잡이 겹치지 않도록.
 *   5) TCP/VSOCK가 아닌 경우(UDP/UNIX): listen은 TCP만 허용, td_rw 금지,
 *      UNIX는 filename 필수, 그리고 listen을 td_read(td)로 자동 결정.
 *   6) listen이면 setup_listen(서버 경로), 아니면 setup_connect(클라 경로) 호출.
 *
 * 실행 컨텍스트: 잡 스레드, 단 1회. 실패 시 잡은 즉시 중단되고 cleanup으로 진입.
 * 호출 체인: td_io_init() → [fio_netio_init] → fio_netio_setup_listen |
 *                                                  fio_netio_setup_connect.
 */
static int fio_netio_init(struct thread_data *td)
{
	struct netio_options *o = td->eo;            /* [한국어] 파싱된 엔진 옵션(proto/port/listen/...) */
	int ret;                                     /* [한국어] 하위 setup_* 반환 코드 */

#ifdef WIN32
	WSADATA wsd;                                 /* [한국어] Winsock 2.2 초기화 결과 — DLL 버전/기능 집합 저장. */
	WSAStartup(MAKEWORD(2,2), &wsd);             /* [한국어] Windows 소켓 서브시스템 로드 — 반드시 socket(2) 이전에 호출 */
#endif

	if (td_random(td)) {                         /* [한국어] rw=randread/randwrite 잡 거부 — 네트워크 스트림은 offset 없음 */
		log_err("fio: network IO can't be random\n");
		return 1;
	}

	if (o->proto == FIO_TYPE_UNIX && o->port) {  /* [한국어] UNIX 도메인 소켓은 파일시스템 경로 기반 — port= 옵션 의미 없음 */
		log_err("fio: network IO port not valid with unix socket\n");
		return 1;
	} else if (is_vsock(o) && !o->port) {        /* [한국어] VSOCK은 svm_port 필수 — 0 포트는 예약 */
		log_err("fio: network IO requires port for vsock\n");
		return 1;
	} else if (o->proto != FIO_TYPE_UNIX && !o->port) { /* [한국어] TCP/UDP도 port 필수(well-known 0 불가) */
		log_err("fio: network IO requires port for tcp or udp\n");
		return 1;
	}

	o->port += td->subjob_number;                /* [한국어] numjobs>1 서브잡 포트 자동 분산(subjob_number=0..n-1) */

	if (!is_tcp(o) && !is_vsock(o)) {            /* [한국어] UDP/UNIX 전용 추가 제약 — 이 블록 안에서 listen/방향 규약 결정 */
		if (o->listen) {
			log_err("fio: listen only valid for TCP proto IO\n"); /* [한국어] UDP/UNIX에서는 listen= 명시적 설정 금지(자동 결정) */
			return 1;
		}
		if (td_rw(td)) {                     /* [한국어] rw=readwrite 잡 금지 — 데이터그램은 양방향 잡 규약 없음 */
			log_err("fio: datagram network connections must be"
				   " read OR write\n");
			return 1;
		}
		if (o->proto == FIO_TYPE_UNIX && !td->o.filename) {
			log_err("fio: UNIX sockets need host/filename\n"); /* [한국어] Unix 소켓은 filename= 또는 hostname=으로 경로 필수 */
			return 1;
		}
		o->listen = td_read(td);             /* [한국어] read 잡=서버(listen=1), write 잡=클라(listen=0) 자동 매핑 */
	}

	if (o->listen)
		ret = fio_netio_setup_listen(td);    /* [한국어] 서버 경로 — socket+bind+(TCP)listen */
	else
		ret = fio_netio_setup_connect(td);   /* [한국어] 클라이언트 경로 — getaddrinfo+주소 구조체 채움(socket/connect는 open_file에서) */

	return ret;                                  /* [한국어] 0/1 전파 — 1이면 잡 중단 */
}

/*
 * [한국어]
 * fio_netio_cleanup - ioengine_ops.cleanup 콜백. 엔진 종료 시 자원 반환.
 *
 * @td: 잡 컨텍스트(io_ops_data 접근).
 *
 * 잡 종료 시 ioengines.c가 td_io_cleanup으로 호출. init/setup에서 할당한 모든
 * 자원(서버 소켓 listenfd, splice 파이프 쌍, netio_data 자체)을 해제한다.
 * 클라이언트 소켓(f->fd)은 close_file에서 이미 generic_close_file로 닫혔으므로
 * 여기서는 건드리지 않는다. nd==NULL 가드로 setup 실패 경로에서도 안전하게 호출.
 *
 * 실행 컨텍스트: 잡 스레드, 단 1회. 잡 스레드 전용 데이터라 락 불필요.
 * 호출 체인: td_io_cleanup() → [fio_netio_cleanup] → close(2)/free(3).
 */
static void fio_netio_cleanup(struct thread_data *td)
{
	struct netio_data *nd = td->io_ops_data;     /* [한국어] 잡별 상태 포인터 — NULL 가능(setup 실패). */

	if (nd) {
		if (nd->listenfd != -1)              /* [한국어] 서버 소켓이 있으면 닫음(accept 완료 후에도 listenfd는 별도로 유지되었음). */
			close(nd->listenfd);
		if (nd->pipes[0] != -1)              /* [한국어] splice 변종의 읽기 파이프(커널 버퍼 소유) 해제. */
			close(nd->pipes[0]);
		if (nd->pipes[1] != -1)              /* [한국어] splice 변종의 쓰기 파이프 해제. */
			close(nd->pipes[1]);

		free(nd);                            /* [한국어] 구조체 본체(calloc) 해제. */
	}
}

/*
 * [한국어]
 * fio_netio_setup - 엔진 setup 콜백(ioengine_ops.setup).
 *
 * @td: 잡 컨텍스트(files/io_ops_data 초기화 대상).
 * @return: 0=성공(init 진행 허용), 그 외=실패(잡 중단).
 *
 * 코어(ioengines.c)가 load_ioengine으로 엔진을 td->io_ops에 연결한 직후 init 이전에
 * 한 번 호출한다. 본 엔진에서는 두 가지를 처리한다:
 *   1) 가상 파일 등록: 네트워크 엔진은 실제 파일시스템 파일이 없지만 fio의 I/O 경로가
 *      모두 fio_file 단위로 설계되어 있어, 사용자가 filename=을 지정하지 않아도 "net"이
 *      라는 더미 이름으로 add_file을 호출해 files_index/nr_files/open_files를 채운다.
 *      이는 생성된 더미 fio_file이 이후 open_file/queue/close_file의 f 파라미터로
 *      전달되어 f->fd에 진짜 소켓 FD가 들어가는 구조이기 때문이다.
 *   2) netio_data(잡별 런타임 상태) 할당: calloc으로 0 초기화 후 FD 슬롯 -1 초기값.
 *      재호출 안전(netsplice의 fio_netio_setup_splice가 본 함수를 감싼 뒤 pipe(2)로
 *      파이프를 추가 생성).
 *
 * 실행 컨텍스트: 잡 스레드, init 이전 단 1회.
 * 호출 체인: ioengines.c td_io_init 초기 단계 → td->io_ops->setup()
 *            → [fio_netio_setup] → add_file() + calloc(3).
 */
static int fio_netio_setup(struct thread_data *td)
{
	struct netio_data *nd;                       /* [한국어] 할당할 netio_data 임시 포인터. */

	if (!td->files_index) {                      /* [한국어] 아직 파일이 등록되지 않았다면(filename= 미지정 & nr_files=0) 가상 파일 추가. */
		add_file(td, td->o.filename ?: "net", 0, 0); /* [한국어] filename이 있으면 그 값을 파일명으로, 없으면 "net" 상수 — 로깅/표시용. */
		td->o.nr_files = td->o.nr_files ?: 1; /* [한국어] 최소 1개 파일 보장. */
		td->o.open_files++;                  /* [한국어] 열려 있는 파일 수 카운트 증가(잡 통계 meta). */
	}

	if (!td->io_ops_data) {                      /* [한국어] 중복 호출 시(예: setup_splice가 본 함수 호출 후 추가 작업) 재할당 방지. */
		nd = calloc(1, sizeof(*nd));         /* [한국어] 모든 필드 0 초기화 — 카운터/플래그의 안전한 기본값. */
		nd->listenfd = -1;                   /* [한국어] 초기 무효 FD — cleanup 시 close(-1) 방지. */
		nd->pipes[0] = nd->pipes[1] = -1;    /* [한국어] 파이프 미생성 상태 표시(netsplice만 나중에 채움). */
		td->io_ops_data = nd;                /* [한국어] 코어 스트럭에 endpoint 연결 — 이후 모든 콜백이 td->io_ops_data로 접근. */
	}

	return 0;                                    /* [한국어] 성공 — init 단계 진행 허용. */
}

/*
 * [한국어]
 * fio_netio_terminate - 외부 종료 요청 수신 시 잡 프로세스에 SIGTERM self-send.
 *
 * @td: 잡 컨텍스트(.pid가 잡 프로세스/스레드 식별자).
 *
 * fio 코어는 사용자 Ctrl-C, runtime 만료, 타 잡 에러 등으로 terminate_threads()를
 * 호출할 때 각 엔진의 .terminate 콜백을 시도한다. 본 동기 엔진은 queue 내부에서
 * blocking send/recv/poll에 갇혀 있을 수 있어, 단순히 td->terminate=1을 설정하는
 * 것만으로는 즉시 반응하지 못한다. 따라서 kill(2)로 SIGTERM을 자기 자신에게 전송해
 * blocking syscall이 EINTR로 풀려나오도록 강제한다. poll_wait / fio_netio_send /
 * fio_netio_recv 는 EINTR을 감지하면 td->terminate 를 재검사해 루프를 빠져나간다.
 *
 * 실행 컨텍스트: 다른 스레드(시그널/모니터 스레드) 또는 같은 잡 스레드에서 호출 가능.
 * kill(2)은 async-signal-safe 하므로 동시성 안전.
 * 호출 체인: backend.c terminate_threads() → td->io_ops->terminate()
 *            → [fio_netio_terminate] → kill(SIGTERM).
 */
static void fio_netio_terminate(struct thread_data *td)
{
	kill(td->pid, SIGTERM);                      /* [한국어] 잡 프로세스/스레드에 SIGTERM — blocking syscall을 EINTR로 깨움(POSIX 기본 핸들러=종료지만 fio가 사전에 핸들러 설치). */
}

#ifdef CONFIG_LINUX_SPLICE
/*
 * [한국어]
 * fio_netio_setup_splice - netsplice 엔진 전용 setup 콜백.
 *
 * @td: 잡 컨텍스트.
 * @return: 0=성공, 1=실패(잡 중단).
 *
 * netsplice 엔진에만 사용되는 .setup 콜백. 공통 fio_netio_setup()을 먼저 호출해
 * fio_file 등록과 netio_data 할당을 마친 뒤, pipe(2)로 익명 파이프를 한 쌍 더 만들어
 * 두 가지 제로카피 경로에 사용한다:
 *   - splice(2) 경로 : 소켓↔파이프 페이지 이동(커널이 페이지 참조만 옮김, copy X).
 *   - vmsplice(2) 경로: 유저 버퍼(io_u->xfer_buf)↔파이프 사이 페이지 매핑(GIFT 옵션
 *                         으로 페이지 소유권을 커널에 넘기면 copy 완전 회피).
 * 완성된 경로: READ = 소켓 →splice→ 파이프 →vmsplice→ 유저 버퍼.
 *              WRITE = 유저 버퍼 →vmsplice→ 파이프 →splice→ 소켓.
 *
 * nd->use_splice=1로 설정하면 __fio_netio_queue가 TCP 경로에 한해 splice 변종을
 * 선택한다(UDP/UNIX는 여전히 send/recv로 폴백 — splice는 스트림 소켓에서만 유용).
 *
 * CONFIG_LINUX_SPLICE가 정의되지 않은 플랫폼(BSD/macOS/Windows)에서는 아예 이 함수도
 * 존재하지 않고 엔진 자체도 등록되지 않는다(파일 하단 register 생성자의 #ifdef 분기).
 *
 * 실행 컨텍스트: 잡 스레드, init 이전 1회.
 * 호출 체인: ioengines.c td_io_init → ioengine_splice.setup → [setup_splice]
 *            → fio_netio_setup() + pipe(2).
 */
static int fio_netio_setup_splice(struct thread_data *td)
{
	struct netio_data *nd;                       /* [한국어] 공통 setup에서 할당한 구조체 포인터. */

	fio_netio_setup(td);                         /* [한국어] 공통 setup 먼저 수행(파일 등록 + calloc + FD 슬롯 -1 초기화). */

	nd = td->io_ops_data;                        /* [한국어] calloc 결과 — 이 시점엔 non-NULL 보장(setup이 성공했으면). */
	if (nd) {
		if (pipe(nd->pipes) < 0)             /* [한국어] 커널 익명 파이프 한 쌍 생성(pipes[0]=read, pipes[1]=write) — splice/vmsplice 중계 버퍼. */
			return 1;

		nd->use_splice = 1;                  /* [한국어] queue 분기에서 splice_in/out 경로를 선택하도록 마킹. */
		return 0;
	}

	return 1;                                    /* [한국어] 공통 setup이 calloc 실패한 이례적 경로. */
}

/*
 * [한국어] netsplice 엔진 등록 메타(splice 기반 제로카피 엔진) — ioengine_ops vtable.
 * fio 코어(ioengines.c)가 load_ioengine("netsplice") 시 이 구조체를 engine_list
 * 에서 찾아 td->io_ops로 링크하고, 이후 잡 수명 동안 각 .xxx 콜백을 호출한다.
 *
 * CONFIG_LINUX_SPLICE 빌드에서만 등록된다(constructor 내부 #ifdef). 플래그에
 * FIO_PIPEIO가 포함되어 fio 코어가 "파이프 중계 엔진"임을 인지하고 해당 최적화
 * 경로(예: 파이프 기반 통계 처리)를 활성화한다. BIT_BASED 비트는 netsplice에는
 * 부여하지 않음(bs= 단위가 "byte"만 허용 — splice 페이지 단위 요구).
 */
static struct ioengine_ops ioengine_splice = {
	.name			= "netsplice",
	/* [한국어] 엔진 식별 문자열. 잡 파일의 `ioengine=netsplice`와 매칭.
	 * 설정자: 이 초기화. 읽는 자: load_ioengine strcmp. 불변. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전. check_engine_ops가 불일치 시 로드 거부.
	 * 설정자: 이 초기화. 읽는 자: register_ioengine. 불변. */

	.prep			= fio_netio_prep,
	/* [한국어] io_u를 queue 전에 준비(UDP ddir 방향 검증).
	 * 반환: 0=정상, 1=잡 중단. 동기화: 잡 스레드 전용. */

	.queue			= fio_netio_queue,
	/* [한국어] io_u 제출 — 동기 엔진이라 내부에서 splice 완료까지 블로킹.
	 * 반환: FIO_Q_COMPLETED/BUSY. 동기화: 잡 스레드 전용. */

	.setup			= fio_netio_setup_splice,
	/* [한국어] fio_file 등록 + netio_data calloc + pipe(2) 생성으로 splice 경로 활성.
	 * 반환: 0=성공, 1=실패(잡 중단). 동기화: 잡 스레드 전용, init 전 1회. */

	.init			= fio_netio_init,
	/* [한국어] proto/listen/port 검증 후 setup_listen|setup_connect 디스패치.
	 * 반환: 0=성공, 1=실패. 동기화: 잡 스레드 전용, 1회. */

	.cleanup		= fio_netio_cleanup,
	/* [한국어] listenfd/pipes[0]/[1]/netio_data 해제. init/setup과 대칭.
	 * 동기화: 잡 스레드 전용, 1회. */

	.open_file		= fio_netio_open_file,
	/* [한국어] accept(2) 또는 connect(2) + UDP면 LINK_OPEN 핸드쉐이크.
	 * 반환: 0=성공, 음수=실패(코어가 정리). */

	.close_file		= fio_netio_close_file,
	/* [한국어] UDP면 LINK_CLOSE 송신 후 generic_close_file(fd 정리).
	 * 반환: 0=성공. */

	.terminate		= fio_netio_terminate,
	/* [한국어] 외부 종료 신호에 대응해 kill(SIGTERM)로 자신을 깨움 — blocking
	 * poll/recv를 EINTR로 빠져나오게 함. 동기화: 임의 시그널 스레드에서 호출 가능
	 * 하지만 kill(2)은 async-signal-safe. */

	.options		= options,
	/* [한국어] net/netsplice 공유 옵션 테이블 포인터. parse.c가 읽음. 불변. */

	.option_struct_size	= sizeof(struct netio_options),
	/* [한국어] td->eo에 할당할 옵션 구조체 크기. parse.c가 이 값으로 malloc. 불변. */

	.flags			= FIO_SYNCIO | FIO_DISKLESSIO | FIO_UNIDIR |
				  FIO_PIPEIO,
	/* [한국어] 엔진 특성 플래그.
	 *   FIO_SYNCIO    — 동기 엔진. queue 호출이 즉시 완료(실제로 내부 blocking 포함).
	 *                    코어가 commit/getevents/event를 호출하지 않음.
	 *   FIO_DISKLESSIO — 실제 파일/블록 디바이스 필요 없음. 코어가 파일 크기/존재
	 *                    검증 건너뜀. netsplice는 "가짜 net 파일"만 등록.
	 *   FIO_UNIDIR    — 잡을 read 또는 write 한 방향만 허용 (rw 불가).
	 *                    UDP가 단방향 데이터그램인 점을 반영.
	 *   FIO_PIPEIO    — 파이프 중계 엔진임을 공지 — splice/vmsplice의 파이프 의존성.
	 * 미설정 비트 의미:
	 *   FIO_RAWIO/NOEXTEND/MEMALIGN: 블록 디바이스 특성 — 네트워크 소켓에 무관.
	 *   FIO_BIT_BASED: 비트 단위 크기 표기 — splice는 페이지 단위 제약이라 제외.
	 *   FIO_ASYNCIO_*: 비동기 계약 — 본 엔진은 동기이므로 N/A. */
};
#endif

/*
 * [한국어] 일반 "net" 엔진 등록 메타(send/recv 기반) — ioengine_ops vtable.
 * splice 변종과 동일한 콜백 체인을 사용하되 .setup만 fio_netio_setup(파이프 생성 생략)
 * 이며, flags에 FIO_BIT_BASED가 추가되어 bs=단위로 "8b"(비트) 같은 표기가 허용된다
 * (유선 프로토콜 테스트에서 비트 단위 크기를 쓰는 경우 대비).
 */
static struct ioengine_ops ioengine_rw = {
	.name			= "net",
	/* [한국어] 엔진 식별자. 잡파일 `ioengine=net`과 매칭. 불변. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전 가드. 불변. */

	.prep			= fio_netio_prep,
	/* [한국어] UDP ddir 방향 검증(listen=1과 WRITE, !listen과 READ 충돌 체크).
	 * 반환: 0=정상, 1=잡 중단. */

	.queue			= fio_netio_queue,
	/* [한국어] io_u 제출. pingpong 시 1차 후 반대 방향 한 번 더.
	 * 반환: FIO_Q_COMPLETED(부분 전송 포함), FIO_Q_BUSY(EMSGSIZE/전혀 진전 없음). */

	.setup			= fio_netio_setup,
	/* [한국어] fio_file 등록("net" 가짜) + netio_data calloc. init보다 먼저.
	 * 반환: 0=성공. */

	.init			= fio_netio_init,
	/* [한국어] proto/listen/port 검증 후 setup_listen|setup_connect. 반환: 0/1. */

	.cleanup		= fio_netio_cleanup,
	/* [한국어] 잡 종료 시 소켓/구조체 해제. init/setup과 대칭. */

	.open_file		= fio_netio_open_file,
	/* [한국어] accept|connect + UDP LINK_OPEN 핸드쉐이크. */

	.close_file		= fio_netio_close_file,
	/* [한국어] UDP LINK_CLOSE + generic_close_file. */

	.terminate		= fio_netio_terminate,
	/* [한국어] SIGTERM self-send로 blocking poll 깨우기. */

	.options		= options,
	/* [한국어] 동일한 옵션 테이블을 netsplice와 공유. 불변. */

	.option_struct_size	= sizeof(struct netio_options),
	/* [한국어] td->eo 구조체 크기. 불변. */

	.flags			= FIO_SYNCIO | FIO_DISKLESSIO | FIO_UNIDIR |
				  FIO_PIPEIO | FIO_BIT_BASED,
	/* [한국어] 엔진 특성 플래그.
	 *   FIO_SYNCIO     — 동기 I/O. queue 완료 즉시 put_io_u.
	 *   FIO_DISKLESSIO — 디스크 의존 없음.
	 *   FIO_UNIDIR     — 한 방향 잡만 허용(UDP/UNIX에서 강제됨).
	 *   FIO_PIPEIO     — 네트워크/파이프 성격 공지(일부 코어 분기 영향).
	 *   FIO_BIT_BASED  — bs="8b" 같은 비트 단위 크기 표기 허용(네트워크 프로토콜
	 *                    테스트의 비트 단위 측정을 위함).
	 * 미설정 비트:
	 *   FIO_RAWIO/MEMALIGN/NOEXTEND: 블록 디바이스 전용 — 무관.
	 *   FIO_ASYNCIO_* : 비동기 계약 아님. */
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
 * fio_netio_register - 빌트인 엔진 등록 생성자.
 *
 * @return: 없음.
 *
 * `fio_init` 속성(= GCC __attribute__((constructor)))에 의해 ELF .init_array 섹션
 * 에 등록되어, libc 동적 로더(ld.so)가 main() 진입 전에 자동으로 호출한다.
 * register_ioengine()은 flist_add_tail로 전역 engine_list(ioengines.c)에 링크하여,
 * 이후 load_ioengine("net"|"netsplice") 호출 시 strcmp 매칭이 가능해진다.
 *
 * netsplice는 CONFIG_LINUX_SPLICE가 정의된 빌드에서만 등록된다 — splice(2)는 Linux
 * 전용 syscall이라 BSD/macOS/Windows에서는 조건부 컴파일로 제외.
 *
 * 실행 컨텍스트: 프로세스 메인 스레드, main() 진입 전, 단 1회.
 * 호출 체인: ELF loader(.init_array) → [fio_netio_register] → register_ioengine()
 *                                                             → flist_add_tail().
 */
static void fio_init fio_netio_register(void)
{
	register_ioengine(&ioengine_rw);             /* [한국어] "net" 엔진을 전역 engine_list에 링크 — load_ioengine("net") 매칭 가능. */
#ifdef CONFIG_LINUX_SPLICE
	register_ioengine(&ioengine_splice);         /* [한국어] "netsplice" 엔진 등록 — Linux splice/vmsplice 빌드에서만 */
#endif
}

/*
 * [한국어]
 * fio_netio_unregister - 빌트인 엔진 등록 해제 소멸자.
 *
 * @return: 없음.
 *
 * `fio_exit`(= __attribute__((destructor))) 속성에 의해 .fini_array 섹션에 등록되어
 * main() 복귀(또는 atexit 체인) 후 자동 호출. 정적 바이너리에서는 동작상 불필요하나
 * .so 빌드의 dlclose 대비 안전 장치 역할을 하며, 재적재(드물게 반복 dlopen)에도
 * engine_list에 중복 엔트리가 남지 않도록 flist_del_init으로 노드를 해제한다.
 *
 * 실행 컨텍스트: 프로세스 종료 직전 메인 스레드, 단 1회.
 * 호출 체인: ELF .fini_array / atexit → [fio_netio_unregister] → unregister_ioengine()
 *                                                               → flist_del_init().
 */
static void fio_exit fio_netio_unregister(void)
{
	unregister_ioengine(&ioengine_rw);           /* [한국어] "net" 엔진 링크 해제. */
#ifdef CONFIG_LINUX_SPLICE
	unregister_ioengine(&ioengine_splice);       /* [한국어] "netsplice" 엔진 해제(Linux 빌드만). */
#endif
}
