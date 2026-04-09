/*
 * [한국어] client.c - fio 클라이언트 모드 구현
 *
 * 이 파일은 fio의 클라이언트-서버 모드에서 클라이언트 측 로직을 구현한다.
 * fio --client=<host> 명령으로 원격 서버에 잡(job)을 전송하고 결과를 수집한다.
 *
 * 주요 함수:
 *   1) fio_client_add()          - 클라이언트 객체를 생성하고 리스트에 추가
 *   2) fio_client_connect()      - 서버에 TCP/소켓 연결 수립, probe 전송
 *   3) fio_clients_send_ini()    - 모든 클라이언트에 잡 설정 파일 전송
 *   4) fio_start_all_clients()   - 모든 클라이언트에 실행 명령 전송
 *   5) fio_handle_clients()      - 메인 이벤트 루프 (poll 기반), 결과 수신 및 처리
 *   6) fio_handle_client()       - 개별 클라이언트의 수신 명령을 opcode별로 분기 처리
 *
 * 통신 흐름:
 *   connect → probe → send_ini(잡 파일) → start(실행) → poll 루프(ETA/통계/로그 수신) → 종료
 *
 * 바이트 오더:
 *   네트워크에서 수신한 데이터는 리틀엔디안으로 인코딩되어 있으며,
 *   convert_ts(), convert_gs() 등의 함수로 호스트 바이트 오더로 변환한다.
 */

/* 표준 라이브러리 및 시스템 헤더 */
#include <stdio.h>        /* 표준 입출력 */
#include <stdlib.h>       /* 메모리 할당, 종료 등 */
#include <unistd.h>       /* POSIX 시스템 호출 (read, write, close 등) */
#include <errno.h>        /* 에러 번호 정의 */
#include <fcntl.h>        /* 파일 제어 (open 플래그) */
#include <poll.h>         /* poll() 기반 I/O 다중화 */
#include <sys/types.h>    /* 기본 시스템 데이터 타입 */
#include <sys/stat.h>     /* 파일 상태 (stat, fstat) */
#include <sys/socket.h>   /* 소켓 API */
#include <sys/un.h>       /* Unix 도메인 소켓 */
#include <netinet/in.h>   /* IPv4/IPv6 소켓 주소 */
#include <arpa/inet.h>    /* IP 주소 변환 (inet_ntop 등) */
#include <netdb.h>        /* 네트워크 데이터베이스 (호스트 해석) */
#include <signal.h>       /* 시그널 처리 */
#ifdef CONFIG_ZLIB
#include <zlib.h>         /* zlib 압축 해제 (서버가 압축한 I/O 로그용) */
#endif

/* fio 내부 헤더 파일들 */
#include "fio.h"          /* fio 핵심 구조체 및 매크로 */
#include "client.h"       /* 클라이언트 구조체 및 API 선언 */
#include "server.h"       /* 서버 프로토콜 (명령 opcode, 송수신 함수) */
#include "flist.h"        /* 이중 연결 리스트 */
#include "hash.h"         /* 해시 함수 */
#include "verify-state.h" /* 검증 상태 저장/복원 */

/* [한국어] 전방 선언 - 서버 명령 핸들러 함수들 */
static void handle_du(struct fio_client *client, struct fio_net_cmd *cmd);    /* 디스크 유틸리티 처리 */
static void handle_ts(struct fio_client *client, struct fio_net_cmd *cmd);    /* 스레드 통계 처리 */
static void handle_gs(struct fio_client *client, struct fio_net_cmd *cmd);    /* 그룹 통계 처리 */
static void handle_probe(struct fio_client *client, struct fio_net_cmd *cmd); /* probe 응답 처리 */
static void handle_text(struct fio_client *client, struct fio_net_cmd *cmd);  /* 텍스트 메시지 처리 */
static void handle_stop(struct fio_client *client);                           /* 잡 정지 처리 */
static void handle_start(struct fio_client *client, struct fio_net_cmd *cmd); /* 잡 시작 처리 */

static void convert_text(struct fio_net_cmd *cmd);           /* 텍스트 PDU 바이트 오더 변환 */
static void client_display_thread_status(struct jobs_eta *je); /* ETA 화면 출력 */

/* [한국어] CLI 모드 기본 콜백 테이블 - 터미널 출력용 핸들러들을 등록 */
struct client_ops const fio_client_ops = {
	.text		= handle_text,                  /* 서버 텍스트 메시지 → 터미널 출력 */
	.disk_util	= handle_du,                    /* 디스크 유틸리티 통계 처리 */
	.thread_status	= handle_ts,                /* 스레드별 I/O 통계 처리 */
	.group_stats	= handle_gs,                /* 그룹 실행 통계 처리 */
	.stop		= handle_stop,                  /* 잡 정지 알림 처리 */
	.start		= handle_start,                 /* 잡 시작 알림 처리 */
	.eta		= client_display_thread_status, /* ETA 화면 표시 */
	.probe		= handle_probe,                 /* 서버 정보(OS, 아키텍처 등) 표시 */
	.eta_msec	= FIO_CLIENT_DEF_ETA_MSEC,      /* ETA 요청 주기: 900ms */
	.client_type	= FIO_CLIENT_TYPE_CLI,          /* CLI 타입 */
};

/* [한국어] 전역 변수들 - 클라이언트 관리 및 통계 집계 */
static struct timespec eta_ts;             /* 마지막 ETA 요청 시각 */

static FLIST_HEAD(client_list);            /* 모든 클라이언트 객체의 전역 리스트 */
static FLIST_HEAD(eta_list);               /* ETA 응답 대기 중인 클라이언트 리스트 */

static FLIST_HEAD(arg_list);               /* 공유 인자 그룹 리스트 */

struct thread_stat client_ts;              /* 전체 클라이언트 합산 스레드 통계 */
struct group_run_stats client_gs;          /* 전체 클라이언트 합산 그룹 통계 */
int sum_stat_clients;                      /* 통계를 합산할 총 클라이언트 수 */

static int sum_stat_nr;                    /* 현재까지 통계를 수신한 클라이언트 수 */
static struct buf_output allclients;       /* "All clients" 합산 통계 출력 버퍼 */
static struct json_object *root = NULL;           /* JSON 출력 최상위 객체 */
static struct json_object *global_opt_object = NULL; /* 단일 서버 전역 옵션 JSON */
static struct json_array *global_opt_array = NULL;   /* 다중 서버 전역 옵션 JSON 배열 */
static struct json_array *clients_array = NULL;      /* 클라이언트 통계 JSON 배열 */
static struct json_array *du_array = NULL;           /* 디스크 유틸리티 JSON 배열 */

static int error_clients;                  /* 에러로 종료한 클라이언트 수 */

/* [한국어] fd 기반 해시 테이블 - 파일 디스크립터로 클라이언트를 빠르게 검색 */
#define FIO_CLIENT_HASH_BITS	7                              /* 해시 비트 수 */
#define FIO_CLIENT_HASH_SZ	(1 << FIO_CLIENT_HASH_BITS)        /* 해시 버킷 수: 128 */
#define FIO_CLIENT_HASH_MASK	(FIO_CLIENT_HASH_SZ - 1)      /* 해시 마스크 */
static struct flist_head client_hash[FIO_CLIENT_HASH_SZ];      /* 해시 버킷 배열 */

static struct cmd_iolog_pdu *convert_iolog(struct fio_net_cmd *, bool *); /* I/O 로그 바이트 오더 변환 */

/* [한국어] 해시 테이블에 클라이언트 추가 - fd를 키로 사용 */
static void fio_client_add_hash(struct fio_client *client)
{
	int bucket = hash_long(client->fd, FIO_CLIENT_HASH_BITS);

	bucket &= FIO_CLIENT_HASH_MASK;
	flist_add(&client->hash_list, &client_hash[bucket]);
}

/* [한국어] 해시 테이블에서 클라이언트 제거 */
static void fio_client_remove_hash(struct fio_client *client)
{
	if (!flist_empty(&client->hash_list))
		flist_del_init(&client->hash_list);
}

/* [한국어] 해시 테이블 초기화 - fio_init 속성으로 프로그램 시작 시 자동 호출 */
static void fio_init fio_client_hash_init(void)
{
	int i;

	for (i = 0; i < FIO_CLIENT_HASH_SZ; i++)
		INIT_FLIST_HEAD(&client_hash[i]);
}

/* [한국어] 소켓/파일에서 지정된 크기만큼 데이터를 완전히 읽는 함수 (부분 읽기 재시도) */
static int read_data(int fd, void *data, size_t size)
{
	ssize_t ret;

	while (size) {
		ret = read(fd, data, size);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		} else if (!ret)
			break;
		else {
			data += ret;
			size -= ret;
		}
	}

	if (size)
		return EAGAIN;

	return 0;
}

/* [한국어] 잡 설정 파일(ini)을 읽으면서 변수 치환($옵션)을 수행하는 함수 */
static int read_ini_data(int fd, void *data, size_t size)
{
	char *p = data;
	int ret = 0;
	FILE *fp;
	int dupfd;

	dupfd = dup(fd);
	if (dupfd < 0)
		return errno;

	fp = fdopen(dupfd, "r");
	if (!fp) {
		ret = errno;
		close(dupfd);
		goto out;
	}

	while (1) {
		ssize_t len;
		char buf[OPT_LEN_MAX+1], *sub;

		if (!fgets(buf, sizeof(buf), fp)) {
			if (ferror(fp)) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				ret = errno;
			}
			break;
		}

		sub = fio_option_dup_subs(buf);
		len = strlen(sub);
		if (len + 1 > size) {
			log_err("fio: no space left to read data\n");
			free(sub);
			ret = ENOSPC;
			break;
		}

		memcpy(p, sub, len);
		free(sub);
		p += len;
		*p = '\0';
		size -= len;
	}

	fclose(fp);
out:
	return ret;
}

/* [한국어] JSON 출력 초기화 - 루트 객체 생성, 타임스탬프/버전 정보 추가 */
static void fio_client_json_init(void)
{
	char time_buf[32];
	time_t time_p;

	if (!(output_format & FIO_OUTPUT_JSON))
		return;

	time(&time_p);
	os_ctime_r((const time_t *) &time_p, time_buf, sizeof(time_buf));
	time_buf[strlen(time_buf) - 1] = '\0';

	root = json_create_object();
	json_object_add_value_string(root, "fio version", fio_version_string);
	json_object_add_value_int(root, "timestamp", time_p);
	json_object_add_value_string(root, "time", time_buf);

	if (nr_clients == 1) {
		global_opt_object = json_create_object();
		json_object_add_value_object(root, "global options", global_opt_object);
	} else {
		global_opt_array = json_create_array();
		json_object_add_value_array(root, "global options", global_opt_array);
	}
	clients_array = json_create_array();
	json_object_add_value_array(root, "client_stats", clients_array);
	du_array = json_create_array();
	json_object_add_value_array(root, "disk_util", du_array);
}

/* [한국어] JSON 출력 완료 - JSON 트리를 문자열로 출력하고 메모리 해제 */
static void fio_client_json_fini(void)
{
	struct buf_output out;

	if (!root)
		return;

	buf_output_init(&out);

	__log_buf(&out, "\n");
	json_print_object(root, &out);
	__log_buf(&out, "\n");
	log_info_buf(out.buf, out.buflen);

	buf_output_free(&out);

	json_free_object(root);
	root = NULL;
	global_opt_object = NULL;
	global_opt_array = NULL;
	clients_array = NULL;
	du_array = NULL;
}

/* [한국어] 파일 디스크립터로 클라이언트 검색 - 해시 테이블에서 fd에 해당하는 클라이언트 반환 */
static struct fio_client *find_client_by_fd(int fd)
{
	int bucket = hash_long(fd, FIO_CLIENT_HASH_BITS) & FIO_CLIENT_HASH_MASK;
	struct fio_client *client;
	struct flist_head *entry;

	flist_for_each(entry, &client_hash[bucket]) {
		client = flist_entry(entry, struct fio_client, hash_list);

		if (client->fd == fd) {
			client->refs++;
			return client;
		}
	}

	return NULL;
}

/* [한국어] 클라이언트 참조 카운트 감소 - 0이 되면 모든 자원을 해제 */
void fio_put_client(struct fio_client *client)
{
	if (--client->refs)
		return;

	log_info_buf(client->buf.buf, client->buf.buflen);
	buf_output_free(&client->buf);

	free(client->hostname);
	if (client->argv)
		free(client->argv);
	if (client->name)
		free(client->name);
	while (client->nr_files) {
		struct client_file *cf = &client->files[--client->nr_files];

		free(cf->file);
	}
	if (client->files)
		free(client->files);
	if (client->opt_lists)
		free(client->opt_lists);

	if (!client->did_stat)
		sum_stat_clients--;

	if (client->error)
		error_clients++;

	free(client);
}

/* [한국어] ETA 대기 카운트 감소 - 모든 클라이언트가 응답하면 ETA를 표시하고 메모리 해제 */
static int fio_client_dec_jobs_eta(struct client_eta *eta, client_eta_op eta_fn)
{
	if (!--eta->pending) {
		eta_fn(&eta->eta);
		free(eta);
		return 0;
	}

	return 1;
}

/* [한국어] 클라이언트의 미처리 텍스트 메시지를 모두 소진(drain)하여 출력 */
static void fio_drain_client_text(struct fio_client *client)
{
	do {
		struct fio_net_cmd *cmd = NULL;

		if (fio_server_poll_fd(client->fd, POLLIN, 0))
			cmd = fio_net_recv_cmd(client->fd, false);
		if (!cmd)
			break;

		if (cmd->opcode == FIO_NET_CMD_TEXT) {
			convert_text(cmd);
			client->ops->text(client, cmd);
		}

		free(cmd);
	} while (1);
}

/* [한국어] 클라이언트 제거 - 텍스트 소진, 리스트/해시 제거, 소켓 닫기, 참조 해제 */
static void remove_client(struct fio_client *client)
{
	assert(client->refs);

	dprint(FD_NET, "client: removed <%s>\n", client->hostname);

	fio_drain_client_text(client);

	if (!flist_empty(&client->list))
		flist_del_init(&client->list);

	fio_client_remove_hash(client);

	if (!flist_empty(&client->eta_list)) {
		flist_del_init(&client->eta_list);
		fio_client_dec_jobs_eta(client->eta_in_flight, client->ops->eta);
	}

	close(client->fd);
	client->fd = -1;

	if (client->ops->removed)
		client->ops->removed(client);

	nr_clients--;
	fio_put_client(client);
}

/* [한국어] 클라이언트 참조 카운트 증가 - 참조를 유지하는 동안 해제 방지 */
struct fio_client *fio_get_client(struct fio_client *client)
{
	client->refs++;
	return client;
}

/* [한국어] 단일 클라이언트에 커맨드라인 옵션 추가 (argv 배열 확장) */
static void __fio_client_add_cmd_option(struct fio_client *client,
					const char *opt)
{
	int index;

	index = client->argc++;
	client->argv = realloc(client->argv, sizeof(char *) * client->argc);
	client->argv[index] = strdup(opt);
	dprint(FD_NET, "client: add cmd %d: %s\n", index, opt);
}

/* [한국어] 클라이언트와 공유 인자 그룹에 커맨드라인 옵션 추가 */
void fio_client_add_cmd_option(void *cookie, const char *opt)
{
	struct fio_client *client = cookie;
	struct flist_head *entry;

	if (!client || !opt)
		return;

	__fio_client_add_cmd_option(client, opt);

	/*
	 * Duplicate arguments to shared client group
	 */
	flist_for_each(entry, &arg_list) {
		client = flist_entry(entry, struct fio_client, arg_list);

		__fio_client_add_cmd_option(client, opt);
	}
}

/* [한국어] 새 클라이언트 객체 할당 및 리스트 헤드 초기화 */
static struct fio_client *get_new_client(void)
{
	struct fio_client *client;

	client = calloc(1, sizeof(*client));

	INIT_FLIST_HEAD(&client->list);
	INIT_FLIST_HEAD(&client->hash_list);
	INIT_FLIST_HEAD(&client->arg_list);
	INIT_FLIST_HEAD(&client->eta_list);
	INIT_FLIST_HEAD(&client->cmd_list);

	buf_output_init(&client->buf);

	return client;
}

/* [한국어] 명시적 클라이언트 추가 - 호스트명, 연결타입, 포트를 직접 지정 (gfio 등에서 사용) */
struct fio_client *fio_client_add_explicit(struct client_ops *ops,
					   const char *hostname, int type,
					   int port)
{
	struct fio_client *client;

	client = get_new_client();

	if (type == Fio_client_socket)
		client->is_sock = true;
	else {
		int ipv6;

		ipv6 = type == Fio_client_ipv6;
		if (fio_server_parse_host(hostname, ipv6,
						&client->addr.sin_addr,
						&client->addr6.sin6_addr))
			goto err;

		client->port = port;
	}

	client->fd = -1;
	client->ops = ops;
	client->refs = 1;
	client->type = ops->client_type;
	client->hostname = strdup(hostname);

	__fio_client_add_cmd_option(client, "fio");

	flist_add(&client->list, &client_list);
	nr_clients++;
	dprint(FD_NET, "client: added <%s>\n", client->hostname);
	return client;
err:
	free(client);
	return NULL;
}

/* [한국어] 클라이언트에 잡 설정 파일 추가 - 나중에 서버로 전송됨 */
int fio_client_add_ini_file(void *cookie, const char *ini_file, bool remote)
{
	struct fio_client *client = cookie;
	struct client_file *cf;
	size_t new_size;
	void *new_files;

	if (!client)
		return 1;

	dprint(FD_NET, "client <%s>: add ini %s\n", client->hostname, ini_file);

	new_size = (client->nr_files + 1) * sizeof(struct client_file);
	new_files = realloc(client->files, new_size);
	if (!new_files)
		return 1;

	client->files = new_files;
	cf = &client->files[client->nr_files];
	cf->file = strdup(ini_file);
	cf->remote = remote;
	client->nr_files++;
	return 0;
}

/* [한국어] 클라이언트 추가 - 호스트명 문자열을 파싱하여 IP/소켓/포트를 결정하고 리스트에 추가 */
int fio_client_add(struct client_ops const *ops, const char *hostname, void **cookie)
{
	struct fio_client *existing = *cookie;
	struct fio_client *client;

	if (existing) {
		/*
		 * We always add our "exec" name as the option, hence 1
		 * means empty.
		 */
		if (existing->argc == 1)
			flist_add_tail(&existing->arg_list, &arg_list);
		else {
			while (!flist_empty(&arg_list))
				flist_del_init(arg_list.next);
		}
	}

	client = get_new_client();

	if (fio_server_parse_string(hostname, &client->hostname,
					&client->is_sock, &client->port,
					&client->addr.sin_addr,
					&client->addr6.sin6_addr,
					&client->ipv6)) {
		fio_put_client(client);
		return -1;
	}

	client->fd = -1;
	client->ops = ops;
	client->refs = 1;
	client->type = ops->client_type;

	__fio_client_add_cmd_option(client, "fio");

	flist_add(&client->list, &client_list);
	nr_clients++;
	dprint(FD_NET, "client: added <%s>\n", client->hostname);
	*cookie = client;
	return 0;
}

/* [한국어] 서버 이름 문자열 반환 - IPv4/IPv6 주소를 문자열로 변환, 소켓이면 "sock" 반환 */
static const char *server_name(struct fio_client *client, char *buf,
			       size_t bufsize)
{
	const char *from;

	if (client->ipv6)
		from = inet_ntop(AF_INET6, (struct sockaddr *) &client->addr6.sin6_addr, buf, bufsize);
	else if (client->is_sock)
		from = "sock";
	else
		from = inet_ntop(AF_INET, (struct sockaddr *) &client->addr.sin_addr, buf, bufsize);

	return from;
}

/* [한국어] 서버에 probe 명령 전송 - 서버의 OS, 아키텍처, fio 버전 등을 질의 */
static void probe_client(struct fio_client *client)
{
	struct cmd_client_probe_pdu pdu;
	const char *sname;
	uint64_t tag;
	char buf[64];

	dprint(FD_NET, "client: send probe\n");

#ifdef CONFIG_ZLIB
	pdu.flags = __le64_to_cpu(FIO_PROBE_FLAG_ZLIB);
#else
	pdu.flags = 0;
#endif

	sname = server_name(client, buf, sizeof(buf));
	memset(pdu.server, 0, sizeof(pdu.server));
	snprintf((char *) pdu.server, sizeof(pdu.server), "%s", sname);

	fio_net_send_cmd(client->fd, FIO_NET_CMD_PROBE, &pdu, sizeof(pdu), &tag, &client->cmd_list);
}

/* [한국어] IP(IPv4/IPv6) 기반 TCP 연결 수립 - 소켓 생성 후 connect() */
static int fio_client_connect_ip(struct fio_client *client)
{
	struct sockaddr *addr;
	socklen_t socklen;
	int fd, domain;

	if (client->ipv6) {
		client->addr6.sin6_family = AF_INET6;
		client->addr6.sin6_port = htons(client->port);
		domain = AF_INET6;
		addr = (struct sockaddr *) &client->addr6;
		socklen = sizeof(client->addr6);
	} else {
		client->addr.sin_family = AF_INET;
		client->addr.sin_port = htons(client->port);
		domain = AF_INET;
		addr = (struct sockaddr *) &client->addr;
		socklen = sizeof(client->addr);
	}

	fd = socket(domain, SOCK_STREAM, 0);
	if (fd < 0) {
		int ret = -errno;

		log_err("fio: socket: %s\n", strerror(errno));
		return ret;
	}

	if (connect(fd, addr, socklen) < 0) {
		int ret = -errno;

		log_err("fio: connect: %s\n", strerror(errno));
		log_err("fio: failed to connect to %s:%u\n", client->hostname,
								client->port);
		close(fd);
		return ret;
	}

	return fd;
}

/* [한국어] Unix 도메인 소켓 연결 수립 - 로컬 서버와의 연결에 사용 */
static int fio_client_connect_sock(struct fio_client *client)
{
	struct sockaddr_un *addr = &client->addr_un;
	socklen_t len;
	int fd;

	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	snprintf(addr->sun_path, sizeof(addr->sun_path), "%s",
		 client->hostname);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		int ret = -errno;

		log_err("fio: socket: %s\n", strerror(errno));
		return ret;
	}

	len = sizeof(addr->sun_family) + strlen(addr->sun_path) + 1;
	if (connect(fd, (struct sockaddr *) addr, len) < 0) {
		int ret = -errno;

		log_err("fio: connect; %s\n", strerror(errno));
		close(fd);
		return ret;
	}

	return fd;
}

/* [한국어] 클라이언트 연결 - 소켓/IP 자동 선택, 해시 등록, 상태 변경, probe 전송 */
int fio_client_connect(struct fio_client *client)
{
	int fd;

	dprint(FD_NET, "client: connect to host %s\n", client->hostname);

	if (client->is_sock)
		fd = fio_client_connect_sock(client);
	else
		fd = fio_client_connect_ip(client);

	dprint(FD_NET, "client: %s connected %d\n", client->hostname, fd);

	if (fd < 0)
		return fd;

	client->fd = fd;
	fio_client_add_hash(client);
	client->state = Client_connected;

	probe_client(client);
	return 0;
}

/* [한국어] 단일 클라이언트에 종료 명령(QUIT) 전송 */
int fio_client_terminate(struct fio_client *client)
{
	return fio_net_send_quit(client->fd);
}

/* [한국어] 모든 클라이언트에 종료 명령 전송 - 시그널 핸들러에서 호출 */
static void fio_clients_terminate(void)
{
	struct flist_head *entry;
	struct fio_client *client;

	dprint(FD_NET, "client: terminate clients\n");

	flist_for_each(entry, &client_list) {
		client = flist_entry(entry, struct fio_client, list);
		fio_client_terminate(client);
	}
}

/* [한국어] SIGINT/SIGTERM 시그널 핸들러 - 모든 클라이언트에 종료 명령 전송 */
static void sig_int(int sig)
{
	dprint(FD_NET, "client: got signal %d\n", sig);
	fio_clients_terminate();
}

/* [한국어] 시그널 핸들러 등록 - SIGINT, SIGTERM, SIGUSR1 (상태 출력) 설정 */
static void client_signal_handler(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &act, NULL);

/* Windows uses SIGBREAK as a quit signal from other applications */
#ifdef WIN32
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGBREAK, &act, NULL);
#endif

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_show_status;
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, NULL);
}

/* [한국어] 커맨드라인 인자를 서버에 전송 - 각 인자를 PDU로 직렬화하여 전송 */
static int send_client_cmd_line(struct fio_client *client)
{
	struct cmd_single_line_pdu *cslp;
	struct cmd_line_pdu *clp;
	unsigned long offset;
	unsigned int *lens;
	void *pdu;
	size_t mem;
	int i, ret;

	dprint(FD_NET, "client: send cmdline %d\n", client->argc);

	lens = malloc(client->argc * sizeof(unsigned int));

	/*
	 * Find out how much mem we need
	 */
	for (i = 0, mem = 0; i < client->argc; i++) {
		lens[i] = strlen(client->argv[i]) + 1;
		mem += lens[i];
	}

	/*
	 * We need one cmd_line_pdu, and argc number of cmd_single_line_pdu
	 */
	mem += sizeof(*clp) + (client->argc * sizeof(*cslp));

	pdu = malloc(mem);
	clp = pdu;
	offset = sizeof(*clp);

	for (i = 0; i < client->argc; i++) {
		uint16_t arg_len = lens[i];

		cslp = pdu + offset;
		strcpy((char *) cslp->text, client->argv[i]);
		cslp->len = cpu_to_le16(arg_len);
		offset += sizeof(*cslp) + arg_len;
	}

	free(lens);
	clp->lines = cpu_to_le16(client->argc);
	clp->client_type = __cpu_to_le16(client->type);
	ret = fio_net_send_cmd(client->fd, FIO_NET_CMD_JOBLINE, pdu, mem, NULL, NULL);
	free(pdu);
	return ret;
}

/* [한국어] 모든 클라이언트 연결 수립 - 시그널 핸들러 설정 후 각 클라이언트에 connect + cmdline 전송 */
int fio_clients_connect(void)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;
	int ret;

#ifdef WIN32
	WSADATA wsd;
	WSAStartup(MAKEWORD(2, 2), &wsd);
#endif

	dprint(FD_NET, "client: connect all\n");

	client_signal_handler();

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		ret = fio_client_connect(client);
		if (ret) {
			remove_client(client);
			continue;
		}

		if (client->argc > 1)
			send_client_cmd_line(client);
	}

	return !nr_clients;
}

/* [한국어] 단일 클라이언트에 잡 실행(RUN) 명령 전송 */
int fio_start_client(struct fio_client *client)
{
	dprint(FD_NET, "client: start %s\n", client->hostname);
	return fio_net_send_simple_cmd(client->fd, FIO_NET_CMD_RUN, 0, NULL);
}

/* [한국어] 모든 클라이언트에 잡 실행 명령 전송 - JSON 초기화 후 각 클라이언트에 RUN 전송 */
int fio_start_all_clients(void)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;
	int ret;

	dprint(FD_NET, "client: start all\n");

	fio_client_json_init();

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		ret = fio_start_client(client);
		if (ret) {
			remove_client(client);
			continue;
		}
	}

	return flist_empty(&client_list);
}

/* [한국어] 서버에 원격 파일 로드 요청 - 파일명만 전송, 서버가 자체적으로 파일을 읽음 */
static int __fio_client_send_remote_ini(struct fio_client *client,
					const char *filename)
{
	struct cmd_load_file_pdu *pdu;
	size_t p_size;
	int ret;

	dprint(FD_NET, "send remote ini %s to %s\n", filename, client->hostname);

	p_size = sizeof(*pdu) + strlen(filename) + 1;
	pdu = calloc(1, p_size);
	pdu->name_len = strlen(filename);
	strcpy((char *) pdu->file, filename);
	pdu->client_type = cpu_to_le16((uint16_t) client->type);

	client->sent_job = true;
	ret = fio_net_send_cmd(client->fd, FIO_NET_CMD_LOAD_FILE, pdu, p_size,NULL, NULL);
	free(pdu);
	return ret;
}

/*
 * Send file contents to server backend. We could use sendfile(), but to remain
 * more portable lets just read/write the darn thing.
 */
/* [한국어] 로컬 잡 파일을 읽어서 서버에 전송 - 변수 치환 후 파일 내용을 네트워크로 전송 */
static int __fio_client_send_local_ini(struct fio_client *client,
				       const char *filename)
{
	struct cmd_job_pdu *pdu;
	size_t p_size;
	struct stat sb;
	char *p;
	void *buf;
	off_t len;
	int fd, ret;

	dprint(FD_NET, "send ini %s to %s\n", filename, client->hostname);

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		log_err("fio: job file <%s> open: %s\n", filename, strerror(errno));
		return ret;
	}

	if (fstat(fd, &sb) < 0) {
		ret = -errno;
		log_err("fio: job file stat: %s\n", strerror(errno));
		close(fd);
		return ret;
	}

	/*
	 * Add extra space for variable expansion, but doesn't guarantee.
	 */
	sb.st_size += OPT_LEN_MAX;
	p_size = sb.st_size + sizeof(*pdu);
	pdu = malloc(p_size);
	buf = pdu->buf;

	len = sb.st_size;
	p = buf;
	if (read_ini_data(fd, p, len)) {
		log_err("fio: failed reading job file %s\n", filename);
		close(fd);
		free(pdu);
		return 1;
	}

	pdu->buf_len = __cpu_to_le32(sb.st_size);
	pdu->client_type = cpu_to_le32(client->type);

	client->sent_job = true;
	ret = fio_net_send_cmd(client->fd, FIO_NET_CMD_JOB, pdu, p_size, NULL, NULL);
	free(pdu);
	close(fd);
	return ret;
}

/* [한국어] 잡 파일 전송 - remote 플래그에 따라 원격/로컬 전송 방식 선택 */
int fio_client_send_ini(struct fio_client *client, const char *filename,
			bool remote)
{
	int ret;

	if (!remote)
		ret = __fio_client_send_local_ini(client, filename);
	else
		ret = __fio_client_send_remote_ini(client, filename);

	if (!ret)
		client->sent_job = true;

	return ret;
}

/* [한국어] client_file 구조체를 통한 잡 파일 전송 래퍼 */
static int fio_client_send_cf(struct fio_client *client,
			      struct client_file *cf)
{
	return fio_client_send_ini(client, cf->file, cf->remote);
}

/* [한국어] 모든 클라이언트에 잡 파일 전송 - 각 클라이언트별 파일 또는 공통 파일 전송 */
int fio_clients_send_ini(const char *filename)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;

	flist_for_each_safe(entry, tmp, &client_list) {
		bool failed = false;

		client = flist_entry(entry, struct fio_client, list);

		if (client->nr_files) {
			int i;

			for (i = 0; i < client->nr_files; i++) {
				struct client_file *cf;

				cf = &client->files[i];

				if (fio_client_send_cf(client, cf)) {
					failed = true;
					remove_client(client);
					break;
				}
			}
		}
		if (client->sent_job || failed)
			continue;
		if (!filename || fio_client_send_ini(client, filename, 0))
			remove_client(client);
	}

	return !nr_clients;
}

/* [한국어] 서버에 잡 옵션 업데이트 전송 - thread_options를 네트워크 형식으로 변환하여 전송 */
int fio_client_update_options(struct fio_client *client,
			      struct thread_options *o, uint64_t *tag)
{
	size_t cmd_sz = offsetof(struct cmd_add_job_pdu, top) +
		thread_options_pack_size(o);
	struct cmd_add_job_pdu *pdu;
	int ret;

	pdu = malloc(cmd_sz);
	pdu->thread_number = cpu_to_le32(client->thread_number);
	pdu->groupid = cpu_to_le32(client->groupid);
	convert_thread_options_to_net(&pdu->top, o);

	ret = fio_net_send_cmd(client->fd, FIO_NET_CMD_UPDATE_JOB, pdu,
			       cmd_sz, tag, &client->cmd_list);
	free(pdu);
	return ret;
}

/* [한국어] I/O 통계 바이트 오더 변환 - 리틀엔디안 → 호스트 오더, IEEE 754 float 복원 */
static void convert_io_stat(struct io_stat *dst, struct io_stat *src)
{
	dst->max_val	= le64_to_cpu(src->max_val);
	dst->min_val	= le64_to_cpu(src->min_val);
	dst->samples	= le64_to_cpu(src->samples);

	/*
	 * Floats arrive as IEEE 754 encoded uint64_t, convert back to double
	 */
	dst->mean.u.f	= fio_uint64_to_double(le64_to_cpu(dst->mean.u.i));
	dst->S.u.f	= fio_uint64_to_double(le64_to_cpu(dst->S.u.i));
}

/*
 * [한국어] 스레드 통계 바이트 오더 변환 - thread_stat의 모든 필드를 호스트 오더로 변환
 * IOPS, 대역폭, 지연시간, 백분위수, steady-state 데이터 등 포함
 */
static void convert_ts(struct thread_stat *dst, struct thread_stat *src)
{
	int i, j, k;

	dst->error		= le32_to_cpu(src->error);
	dst->thread_number	= le32_to_cpu(src->thread_number);
	dst->groupid		= le32_to_cpu(src->groupid);
	dst->job_start		= le64_to_cpu(src->job_start);
	dst->pid		= le32_to_cpu(src->pid);
	dst->members		= le32_to_cpu(src->members);
	dst->unified_rw_rep	= le32_to_cpu(src->unified_rw_rep);
	dst->ioprio		= le32_to_cpu(src->ioprio);
	dst->disable_prio_stat	= le32_to_cpu(src->disable_prio_stat);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		convert_io_stat(&dst->clat_stat[i], &src->clat_stat[i]);
		convert_io_stat(&dst->slat_stat[i], &src->slat_stat[i]);
		convert_io_stat(&dst->lat_stat[i], &src->lat_stat[i]);
		convert_io_stat(&dst->bw_stat[i], &src->bw_stat[i]);
		convert_io_stat(&dst->iops_stat[i], &src->iops_stat[i]);
	}
	convert_io_stat(&dst->sync_stat, &src->sync_stat);

	dst->usr_time		= le64_to_cpu(src->usr_time);
	dst->sys_time		= le64_to_cpu(src->sys_time);
	dst->ctx		= le64_to_cpu(src->ctx);
	dst->minf		= le64_to_cpu(src->minf);
	dst->majf		= le64_to_cpu(src->majf);
	dst->clat_percentiles	= le32_to_cpu(src->clat_percentiles);
	dst->lat_percentiles	= le32_to_cpu(src->lat_percentiles);
	dst->slat_percentiles	= le32_to_cpu(src->slat_percentiles);
	dst->percentile_precision = le64_to_cpu(src->percentile_precision);

	for (i = 0; i < FIO_IO_U_LIST_MAX_LEN; i++) {
		fio_fp64_t *fps = &src->percentile_list[i];
		fio_fp64_t *fpd = &dst->percentile_list[i];

		fpd->u.f = fio_uint64_to_double(le64_to_cpu(fps->u.i));
	}

	for (i = 0; i < FIO_IO_U_MAP_NR; i++) {
		dst->io_u_map[i]	= le64_to_cpu(src->io_u_map[i]);
		dst->io_u_submit[i]	= le64_to_cpu(src->io_u_submit[i]);
		dst->io_u_complete[i]	= le64_to_cpu(src->io_u_complete[i]);
	}

	for (i = 0; i < FIO_IO_U_LAT_N_NR; i++)
		dst->io_u_lat_n[i]	= le64_to_cpu(src->io_u_lat_n[i]);
	for (i = 0; i < FIO_IO_U_LAT_U_NR; i++)
		dst->io_u_lat_u[i]	= le64_to_cpu(src->io_u_lat_u[i]);
	for (i = 0; i < FIO_IO_U_LAT_M_NR; i++)
		dst->io_u_lat_m[i]	= le64_to_cpu(src->io_u_lat_m[i]);

	for (i = 0; i < FIO_LAT_CNT; i++)
		for (j = 0; j < DDIR_RWDIR_CNT; j++)
			for (k = 0; k < FIO_IO_U_PLAT_NR; k++)
				dst->io_u_plat[i][j][k] = le64_to_cpu(src->io_u_plat[i][j][k]);

	for (j = 0; j < FIO_IO_U_PLAT_NR; j++)
		dst->io_u_sync_plat[j] = le64_to_cpu(src->io_u_sync_plat[j]);

	for (i = 0; i < DDIR_RWDIR_SYNC_CNT; i++)
		dst->total_io_u[i]	= le64_to_cpu(src->total_io_u[i]);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		dst->short_io_u[i]	= le64_to_cpu(src->short_io_u[i]);
		dst->drop_io_u[i]	= le64_to_cpu(src->drop_io_u[i]);
	}

	dst->total_submit	= le64_to_cpu(src->total_submit);
	dst->total_complete	= le64_to_cpu(src->total_complete);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		dst->io_bytes[i]	= le64_to_cpu(src->io_bytes[i]);
		dst->runtime[i]		= le64_to_cpu(src->runtime[i]);
	}

	dst->total_run_time	= le64_to_cpu(src->total_run_time);
	dst->continue_on_error	= le16_to_cpu(src->continue_on_error);
	dst->total_err_count	= le64_to_cpu(src->total_err_count);
	dst->first_error	= le32_to_cpu(src->first_error);
	dst->kb_base		= le32_to_cpu(src->kb_base);
	dst->unit_base		= le32_to_cpu(src->unit_base);

	dst->sig_figs		= le32_to_cpu(src->sig_figs);

	dst->nr_zone_resets	= le64_to_cpu(src->nr_zone_resets);
	dst->count_zone_resets	= le16_to_cpu(src->count_zone_resets);

	dst->latency_depth	= le32_to_cpu(src->latency_depth);
	dst->latency_target	= le64_to_cpu(src->latency_target);
	dst->latency_window	= le64_to_cpu(src->latency_window);
	dst->latency_percentile.u.f = fio_uint64_to_double(le64_to_cpu(src->latency_percentile.u.i));

	dst->nr_block_infos	= le64_to_cpu(src->nr_block_infos);
	for (i = 0; i < dst->nr_block_infos; i++)
		dst->block_infos[i] = le32_to_cpu(src->block_infos[i]);

	dst->ss_dur		= le64_to_cpu(src->ss_dur);
	dst->ss_state		= le32_to_cpu(src->ss_state);
	dst->ss_head		= le32_to_cpu(src->ss_head);
	dst->ss_limit.u.f 	= fio_uint64_to_double(le64_to_cpu(src->ss_limit.u.i));
	dst->ss_slope.u.f 	= fio_uint64_to_double(le64_to_cpu(src->ss_slope.u.i));
	dst->ss_deviation.u.f 	= fio_uint64_to_double(le64_to_cpu(src->ss_deviation.u.i));
	dst->ss_criterion.u.f 	= fio_uint64_to_double(le64_to_cpu(src->ss_criterion.u.i));

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		dst->nr_clat_prio[i] = le32_to_cpu(src->nr_clat_prio[i]);
		for (j = 0; j < dst->nr_clat_prio[i]; j++) {
			for (k = 0; k < FIO_IO_U_PLAT_NR; k++)
				dst->clat_prio[i][j].io_u_plat[k] =
					le64_to_cpu(src->clat_prio[i][j].io_u_plat[k]);
			convert_io_stat(&dst->clat_prio[i][j].clat_stat,
					&src->clat_prio[i][j].clat_stat);
			dst->clat_prio[i][j].ioprio =
				le32_to_cpu(dst->clat_prio[i][j].ioprio);
		}
	}

	if (dst->ss_state & FIO_SS_DATA) {
		for (i = 0; i < dst->ss_dur; i++ ) {
			dst->ss_iops_data[i] = le64_to_cpu(src->ss_iops_data[i]);
			dst->ss_bw_data[i] = le64_to_cpu(src->ss_bw_data[i]);
			dst->ss_lat_data[i] = le64_to_cpu(src->ss_lat_data[i]);
		}
	}

	dst->cachehit		= le64_to_cpu(src->cachehit);
	dst->cachemiss		= le64_to_cpu(src->cachemiss);
}

/* [한국어] 그룹 실행 통계 바이트 오더 변환 - 최대/최소 실행 시간, 대역폭, I/O 바이트 등 */
static void convert_gs(struct group_run_stats *dst, struct group_run_stats *src)
{
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		dst->max_run[i]		= le64_to_cpu(src->max_run[i]);
		dst->min_run[i]		= le64_to_cpu(src->min_run[i]);
		dst->max_bw[i]		= le64_to_cpu(src->max_bw[i]);
		dst->min_bw[i]		= le64_to_cpu(src->min_bw[i]);
		dst->iobytes[i]		= le64_to_cpu(src->iobytes[i]);
		dst->agg[i]		= le64_to_cpu(src->agg[i]);
	}

	dst->kb_base	= le32_to_cpu(src->kb_base);
	dst->unit_base	= le32_to_cpu(src->unit_base);
	dst->sig_figs	= le32_to_cpu(src->sig_figs);
	dst->groupid	= le32_to_cpu(src->groupid);
	dst->unified_rw_rep	= le32_to_cpu(src->unified_rw_rep);
}

/* [한국어] JSON 객체에 클라이언트 식별 정보(호스트명, 포트) 추가 */
static void json_object_add_client_info(struct json_object *obj,
					struct fio_client *client)
{
	const char *hostname = client->hostname ? client->hostname : "";

	json_object_add_value_string(obj, "hostname", hostname);
	json_object_add_value_int(obj, "port", client->port);
}

/*
 * [한국어] 스레드 통계 수신 처리 - 개별 통계 출력 및 다중 클라이언트 합산
 * 모든 클라이언트 통계가 수신되면 "All clients" 합산 통계도 출력
 */
static void handle_ts(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_ts_pdu *p = (struct cmd_ts_pdu *) cmd->payload;
	struct flist_head *opt_list = NULL;
	struct json_object *tsobj;

	if (client->opt_lists && p->ts.thread_number <= client->jobs)
		opt_list = &client->opt_lists[p->ts.thread_number - 1];

	tsobj = show_thread_status(&p->ts, &p->rs, opt_list, &client->buf);
	if (tsobj) {
		json_object_add_client_info(tsobj, client);
		json_array_add_value_object(clients_array, tsobj);
		if (!client->did_stat && client->global_opts)
			json_array_add_value_object(global_opt_array, client->global_opts);
	}
	client->did_stat = true;

	if (sum_stat_clients <= 1)
		return;

	sum_thread_stats(&client_ts, &p->ts);
	sum_group_stats(&client_gs, &p->rs);

	if (!client_ts.members) {
		/* Arbitrarily use the percentile toggles and percentile list
		 * from the first thread_stat that comes our way */
		client_ts.slat_percentiles = p->ts.slat_percentiles;
		client_ts.clat_percentiles = p->ts.clat_percentiles;
		client_ts.lat_percentiles = p->ts.lat_percentiles;

		for (int i = 0; i < FIO_IO_U_LIST_MAX_LEN; i++)
			client_ts.percentile_list[i] = p->ts.percentile_list[i];
	}
	client_ts.members++;
	client_ts.thread_number = p->ts.thread_number;
	client_ts.groupid = p->ts.groupid;
	client_ts.unified_rw_rep = p->ts.unified_rw_rep;
	client_ts.sig_figs = p->ts.sig_figs;

	if (++sum_stat_nr == sum_stat_clients) {
		strcpy(client_ts.name, "All clients");
		tsobj = show_thread_status(&client_ts, &client_gs, NULL, &allclients);
		if (tsobj) {
			json_object_add_client_info(tsobj, client);
			json_array_add_value_object(clients_array, tsobj);
		}
	}
}

/* [한국어] 그룹 통계 수신 처리 - 일반 출력 모드일 때 그룹 통계 표시 */
static void handle_gs(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct group_run_stats *gs = (struct group_run_stats *) cmd->payload;

	if (output_format & FIO_OUTPUT_NORMAL)
		show_group_stats(gs, &client->buf);
}

/* [한국어] 잡 옵션 수신 처리 - 전역/그룹별 옵션을 JSON 객체 또는 리스트에 저장 */
static void handle_job_opt(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_job_option *pdu = (struct cmd_job_option *) cmd->payload;

	pdu->global = le16_to_cpu(pdu->global);
	pdu->truncated = le16_to_cpu(pdu->truncated);
	pdu->groupid = le32_to_cpu(pdu->groupid);

	if (pdu->global) {
		struct json_object *global_opts;

		if (!global_opt_object && !global_opt_array)
			return;

		/*
		 * If we have only one server connection, add it to the single
		 * global option dictionary. When we have connections to
		 * multiple servers, add the global option to the
		 * server-specific dictionary.
		 */
		if (global_opt_object) {
			global_opts = global_opt_object;
		} else {
			if (!client->global_opts) {
				client->global_opts = json_create_object();
				json_object_add_client_info(client->global_opts, client);
			}
			global_opts = client->global_opts;
		}

		json_object_add_value_string(global_opts,
					     (const char *)pdu->name,
					     (const char *)pdu->value);
		return;
	} else if (client->opt_lists) {
		struct flist_head *opt_list = &client->opt_lists[pdu->groupid];
		struct print_option *p;

		p = malloc(sizeof(*p));
		p->name = strdup((const char *)pdu->name);
		p->value = pdu->value[0] ? strdup((const char *)pdu->value) :
			NULL;
		flist_add_tail(&p->list, opt_list);
	}
}

/* [한국어] 텍스트 메시지 수신 처리 - 서버가 보낸 로그/에러 메시지를 화면에 출력 */
static void handle_text(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_text_pdu *pdu = (struct cmd_text_pdu *) cmd->payload;
	const char *buf = (const char *) pdu->buf;
	const char *name;
	int fio_unused ret;
	struct buf_output out;

	buf_output_init(&out);

	name = client->name ? client->name : client->hostname;

	if (!client->skip_newline && !(output_format & FIO_OUTPUT_TERSE))
		__log_buf(&out, "<%s> ", name);
	__log_buf(&out, "%s", buf);
	log_info_buf(out.buf, out.buflen);
	buf_output_free(&out);
	client->skip_newline = strchr(buf, '\n') == NULL;
}

/* [한국어] 디스크 유틸리티 집계 통계 바이트 오더 변환 */
static void convert_agg(struct disk_util_agg *agg)
{
	int i;

	for (i = 0; i < 2; i++) {
		agg->ios[i]	= le64_to_cpu(agg->ios[i]);
		agg->merges[i]	= le64_to_cpu(agg->merges[i]);
		agg->sectors[i]	= le64_to_cpu(agg->sectors[i]);
		agg->ticks[i]	= le64_to_cpu(agg->ticks[i]);
	}

	agg->io_ticks		= le64_to_cpu(agg->io_ticks);
	agg->time_in_queue	= le64_to_cpu(agg->time_in_queue);
	agg->slavecount		= le32_to_cpu(agg->slavecount);
	agg->max_util.u.f	= fio_uint64_to_double(le64_to_cpu(agg->max_util.u.i));
}

/* [한국어] 디스크 유틸리티 통계 바이트 오더 변환 - I/O 횟수, 병합, 섹터, 틱 등 */
static void convert_dus(struct disk_util_stat *dus)
{
	int i;

	for (i = 0; i < 2; i++) {
		dus->s.ios[i]		= le64_to_cpu(dus->s.ios[i]);
		dus->s.merges[i]	= le64_to_cpu(dus->s.merges[i]);
		dus->s.sectors[i]	= le64_to_cpu(dus->s.sectors[i]);
		dus->s.ticks[i]		= le64_to_cpu(dus->s.ticks[i]);
	}

	dus->s.io_ticks		= le64_to_cpu(dus->s.io_ticks);
	dus->s.time_in_queue	= le64_to_cpu(dus->s.time_in_queue);
	dus->s.msec		= le64_to_cpu(dus->s.msec);
}

/* [한국어] 디스크 유틸리티 수신 처리 - JSON/일반/terse 형식으로 디스크 통계 출력 */
static void handle_du(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_du_pdu *du = (struct cmd_du_pdu *) cmd->payload;

	if (!client->disk_stats_shown)
		client->disk_stats_shown = true;

	if (output_format & FIO_OUTPUT_JSON) {
		struct json_object *duobj;

		json_array_add_disk_util(&du->dus, &du->agg, du_array);
		duobj = json_array_last_value_object(du_array);
		json_object_add_client_info(duobj, client);
	}
	if (output_format & FIO_OUTPUT_NORMAL) {
		__log_buf(&client->buf, "\nDisk stats (read/write):\n");
		print_disk_util(&du->dus, &du->agg, 0, &client->buf);
	}
	if (output_format & FIO_OUTPUT_TERSE && terse_version >= 3) {
		print_disk_util(&du->dus, &du->agg, 1, &client->buf);
		__log_buf(&client->buf, "\n");
	}
}

/* [한국어] ETA 데이터 바이트 오더 변환 - 실행 중/대기/설정 중 스레드 수, 속도, 경과 시간 등 */
static void convert_jobs_eta(struct jobs_eta *je)
{
	int i;

	je->nr_running		= le32_to_cpu(je->nr_running);
	je->nr_ramp		= le32_to_cpu(je->nr_ramp);
	je->nr_pending		= le32_to_cpu(je->nr_pending);
	je->nr_setting_up	= le32_to_cpu(je->nr_setting_up);
	je->files_open		= le32_to_cpu(je->files_open);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		je->m_rate[i]	= le64_to_cpu(je->m_rate[i]);
		je->t_rate[i]	= le64_to_cpu(je->t_rate[i]);
		je->m_iops[i]	= le32_to_cpu(je->m_iops[i]);
		je->t_iops[i]	= le32_to_cpu(je->t_iops[i]);
		je->rate[i]	= le64_to_cpu(je->rate[i]);
		je->iops[i]	= le32_to_cpu(je->iops[i]);
	}

	je->elapsed_sec		= le64_to_cpu(je->elapsed_sec);
	je->eta_sec		= le64_to_cpu(je->eta_sec);
	je->nr_threads		= le32_to_cpu(je->nr_threads);
	je->is_pow2		= le32_to_cpu(je->is_pow2);
	je->unit_base		= le32_to_cpu(je->unit_base);
	je->sig_figs		= le32_to_cpu(je->sig_figs);
}

/* [한국어] 여러 클라이언트의 ETA 합산 - 실행 중 스레드, 속도, IOPS 등을 누적 */
void fio_client_sum_jobs_eta(struct jobs_eta *dst, struct jobs_eta *je)
{
	int i;

	dst->nr_running		+= je->nr_running;
	dst->nr_ramp		+= je->nr_ramp;
	dst->nr_pending		+= je->nr_pending;
	dst->nr_setting_up	+= je->nr_setting_up;
	dst->files_open		+= je->files_open;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		dst->m_rate[i]	+= je->m_rate[i];
		dst->t_rate[i]	+= je->t_rate[i];
		dst->m_iops[i]	+= je->m_iops[i];
		dst->t_iops[i]	+= je->t_iops[i];
		dst->rate[i]	+= je->rate[i];
		dst->iops[i]	+= je->iops[i];
	}

	dst->elapsed_sec	+= je->elapsed_sec;

	if (je->eta_sec > dst->eta_sec)
		dst->eta_sec = je->eta_sec;

	dst->nr_threads		+= je->nr_threads;

	/*
	 * This wont be correct for multiple strings, but at least it
	 * works for the basic cases.
	 */
	strcpy((char *) dst->run_str, (char *) je->run_str);
}

/* [한국어] 응답 대기 리스트에서 태그가 일치하는 명령 제거 - 응답과 요청을 매칭 */
static bool remove_reply_cmd(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct fio_net_cmd_reply *reply = NULL;
	struct flist_head *entry;

	flist_for_each(entry, &client->cmd_list) {
		reply = flist_entry(entry, struct fio_net_cmd_reply, list);

		if (cmd->tag == (uintptr_t) reply)
			break;

		reply = NULL;
	}

	if (!reply) {
		log_err("fio: client: unable to find matching tag (%llx)\n", (unsigned long long) cmd->tag);
		return false;
	}

	flist_del(&reply->list);
	cmd->tag = reply->saved_tag;
	free(reply);
	return true;
}

/* [한국어] 특정 태그의 응답을 동기적으로 대기 - 1ms 간격으로 폴링 */
int fio_client_wait_for_reply(struct fio_client *client, uint64_t tag)
{
	do {
		struct fio_net_cmd_reply *reply = NULL;
		struct flist_head *entry;

		flist_for_each(entry, &client->cmd_list) {
			reply = flist_entry(entry, struct fio_net_cmd_reply, list);

			if (tag == (uintptr_t) reply)
				break;

			reply = NULL;
		}

		if (!reply)
			break;

		usleep(1000);
	} while (1);

	return 0;
}

/* [한국어] ETA 응답 수신 처리 - 클라이언트별 ETA를 집계 구조체에 합산 */
static void handle_eta(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct jobs_eta *je = (struct jobs_eta *) cmd->payload;
	struct client_eta *eta = (struct client_eta *) (uintptr_t) cmd->tag;

	dprint(FD_NET, "client: got eta tag %p, %d\n", eta, eta->pending);

	assert(client->eta_in_flight == eta);

	client->eta_in_flight = NULL;
	flist_del_init(&client->eta_list);
	client->eta_timeouts = 0;

	if (client->ops->jobs_eta)
		client->ops->jobs_eta(client, je);

	fio_client_sum_jobs_eta(&eta->eta, je);
	fio_client_dec_jobs_eta(eta, client->ops->eta);
}

/* [한국어] 히스토그램 I/O 샘플을 파일에 기록 - 지연시간 분포 히스토그램 데이터 출력 */
static void client_flush_hist_samples(FILE *f, int hist_coarseness, void *samples,
				      uint64_t sample_size)
{
	struct io_sample *s, *s_tmp;
	bool log_offset, log_issue_time;
	uint64_t i, j, nr_samples;
	struct io_u_plat_entry *entry;
	uint64_t *io_u_plat;

	int stride = 1 << hist_coarseness;

	if (!sample_size)
		return;

	s = __get_sample(samples, 0, 0, 0);
	log_offset = (s->__ddir & LOG_OFFSET_SAMPLE_BIT) != 0;
	log_issue_time = (s->__ddir & LOG_ISSUE_TIME_SAMPLE_BIT) != 0;

	nr_samples = sample_size / __log_entry_sz(log_offset, log_issue_time);

	for (i = 0; i < nr_samples; i++) {

		s_tmp = __get_sample(samples, log_offset, log_issue_time, i);
		s = (struct io_sample *)((char *)s_tmp +
					 i * sizeof(struct io_u_plat_entry));

		entry = s->data.plat_entry;
		io_u_plat = entry->io_u_plat;

		fprintf(f, "%lu, %u, %llu, ", (unsigned long) s->time,
						io_sample_ddir(s), (unsigned long long) s->bs);
		for (j = 0; j < FIO_IO_U_PLAT_NR - stride; j += stride) {
			fprintf(f, "%llu, ", (unsigned long long)hist_sum(j, stride, io_u_plat, NULL));
		}
		fprintf(f, "%llu\n", (unsigned long long)
			hist_sum(FIO_IO_U_PLAT_NR - stride, stride, io_u_plat, NULL));

	}
}

/*
 * [한국어] I/O 로그 수신 처리 - 서버에서 전송된 I/O 로그를 파일로 저장
 * 압축된 로그는 직접 저장, 비압축 로그는 디코딩 후 텍스트 형태로 저장
 * 호스트명을 붙여 고유한 로그 파일명을 생성 (예: jobname.log.hostname)
 */
static int fio_client_handle_iolog(struct fio_client *client,
				   struct fio_net_cmd *cmd)
{
	struct cmd_iolog_pdu *pdu = NULL;
	bool store_direct;
	char *log_pathname = NULL;
	int ret = 0;

	pdu = convert_iolog(cmd, &store_direct);
	if (!pdu) {
		log_err("fio: failed converting IO log\n");
		ret = 1;
		goto out;
	}

        /* allocate buffer big enough for next sprintf() call */
	log_pathname = malloc(10 + strlen((char *)pdu->name) +
			strlen(client->hostname));
	if (!log_pathname) {
		log_err("fio: memory allocation of unique pathname failed\n");
		ret = -1;
		goto out;
	}
	/* generate a unique pathname for the log file using hostname */
	sprintf(log_pathname, "%s.%s", pdu->name, client->hostname);

	if (store_direct) {
		ssize_t wrote;
		size_t sz;
		int fd, flags;

		if (pdu->per_job_logs)
			flags = O_WRONLY | O_CREAT | O_TRUNC;
		else
			flags = O_WRONLY | O_CREAT | O_APPEND;
		fd = open((const char *) log_pathname, flags, 0644);
		if (fd < 0) {
			log_err("fio: open log %s: %s\n",
				log_pathname, strerror(errno));
			ret = 1;
			goto out;
		}

		sz = cmd->pdu_len - sizeof(*pdu);
		wrote = write(fd, pdu->samples, sz);
		close(fd);

		if (wrote != sz) {
			log_err("fio: short write on compressed log\n");
			ret = 1;
			goto out;
		}

		ret = 0;
	} else {
		FILE *f;
		const char *mode;

		if (pdu->per_job_logs)
			mode = "w";
		else
			mode = "a";
		f = fopen((const char *) log_pathname, mode);
		if (!f) {
			log_err("fio: fopen log %s : %s\n",
				log_pathname, strerror(errno));
			ret = 1;
			goto out;
		}

		if (pdu->log_type == IO_LOG_TYPE_HIST) {
			client_flush_hist_samples(f, pdu->log_hist_coarseness, pdu->samples,
					   pdu->nr_samples * sizeof(struct io_sample));
		} else {
			flush_samples(f, pdu->samples,
					pdu->nr_samples * sizeof(struct io_sample));
		}
		fclose(f);
		ret = 0;
	}

out:
	if (pdu && pdu != (void *) cmd->payload)
		free(pdu);

	if (log_pathname)
		free(log_pathname);

	return ret;
}

/* [한국어] probe 응답 처리 - 서버의 OS, 아키텍처, fio 버전 등을 화면에 표시 */
static void handle_probe(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_probe_reply_pdu *probe = (struct cmd_probe_reply_pdu *) cmd->payload;
	const char *os, *arch;
	char bit[16];

	os = fio_get_os_string(probe->os);
	if (!os)
		os = "unknown";

	arch = fio_get_arch_string(probe->arch);
	if (!arch)
		os = "unknown";

	sprintf(bit, "%d-bit", probe->bpp * 8);
	probe->flags = le64_to_cpu(probe->flags);

	if (output_format & FIO_OUTPUT_NORMAL) {
		log_info("hostname=%s, be=%u, %s, os=%s, arch=%s, fio=%s, flags=%lx\n",
			probe->hostname, probe->bigendian, bit, os, arch,
			probe->fio_version, (unsigned long) probe->flags);
	}

	if (!client->name)
		client->name = strdup((char *) probe->hostname);
}

/* [한국어] 잡 시작 처리 - 잡 수와 통계 출력 수를 기록하고, 잡별 옵션 리스트 할당 */
static void handle_start(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_start_pdu *pdu = (struct cmd_start_pdu *) cmd->payload;

	client->state = Client_started;
	client->jobs = le32_to_cpu(pdu->jobs);
	client->nr_stat = le32_to_cpu(pdu->stat_outputs);

	if (client->jobs) {
		int i;

		if (client->opt_lists)
			free(client->opt_lists);

		client->opt_lists = malloc(client->jobs * sizeof(struct flist_head));
		for (i = 0; i < client->jobs; i++)
			INIT_FLIST_HEAD(&client->opt_lists[i]);
	}

	sum_stat_clients += client->nr_stat;
}

/* [한국어] 잡 정지 처리 - 에러가 있으면 에러 코드와 함께 로그 출력 */
static void handle_stop(struct fio_client *client)
{
	if (client->error)
		log_info("client <%s>: exited with error %d\n", client->hostname, client->error);
}

/* [한국어] 정지 PDU 바이트 오더 변환 */
static void convert_stop(struct fio_net_cmd *cmd)
{
	struct cmd_end_pdu *pdu = (struct cmd_end_pdu *) cmd->payload;

	pdu->error = le32_to_cpu(pdu->error);
}

/* [한국어] 텍스트 PDU 바이트 오더 변환 - 로그 레벨, 버퍼 길이, 타임스탬프 */
static void convert_text(struct fio_net_cmd *cmd)
{
	struct cmd_text_pdu *pdu = (struct cmd_text_pdu *) cmd->payload;

	pdu->level	= le32_to_cpu(pdu->level);
	pdu->buf_len	= le32_to_cpu(pdu->buf_len);
	pdu->log_sec	= le64_to_cpu(pdu->log_sec);
	pdu->log_usec	= le64_to_cpu(pdu->log_usec);
}

/* [한국어] zlib 압축된 I/O 로그 해제 - inflate로 압축 해제하여 원본 샘플 복원 */
static struct cmd_iolog_pdu *convert_iolog_gz(struct fio_net_cmd *cmd,
					      struct cmd_iolog_pdu *pdu)
{
#ifdef CONFIG_ZLIB
	struct cmd_iolog_pdu *ret;
	z_stream stream;
	uint64_t nr_samples;
	size_t total;
	char *p;
	size_t log_entry_size;

	stream.zalloc = Z_NULL;
	stream.zfree = Z_NULL;
	stream.opaque = Z_NULL;
	stream.avail_in = 0;
	stream.next_in = Z_NULL;

	if (inflateInit(&stream) != Z_OK)
		return NULL;

	/*
	 * Get header first, it's not compressed
	 */
	nr_samples = le64_to_cpu(pdu->nr_samples);

	log_entry_size = __log_entry_sz(le32_to_cpu(pdu->log_offset),
					le32_to_cpu(pdu->log_issue_time));
	if (pdu->log_type == IO_LOG_TYPE_HIST)
		total = nr_samples * (log_entry_size +
				      sizeof(struct io_u_plat_entry));
	else
		total = nr_samples * log_entry_size;
	ret = malloc(total + sizeof(*pdu));
	ret->nr_samples = nr_samples;

	memcpy(ret, pdu, sizeof(*pdu));

	p = (char *) ret + sizeof(*pdu);

	stream.avail_in = cmd->pdu_len - sizeof(*pdu);
	stream.next_in = (void *)((char *) pdu + sizeof(*pdu));
	while (stream.avail_in) {
		unsigned int this_chunk = 65536;
		unsigned int this_len;
		int err;

		if (this_chunk > total)
			this_chunk = total;

		stream.avail_out = this_chunk;
		stream.next_out = (void *)p;
		err = inflate(&stream, Z_NO_FLUSH);
		/* may be Z_OK, or Z_STREAM_END */
		if (err < 0) {
			/*
			 * Z_STREAM_ERROR and Z_BUF_ERROR can safely be
			 * ignored */
			if (err == Z_STREAM_ERROR || err == Z_BUF_ERROR)
				break;
			log_err("fio: inflate error %d\n", err);
			free(ret);
			ret = NULL;
			goto err;
		}

		this_len = this_chunk - stream.avail_out;
		p += this_len;
		total -= this_len;
	}

err:
	inflateEnd(&stream);
	return ret;
#else
	return NULL;
#endif
}

/*
 * This has been compressed on the server side, since it can be big.
 * Uncompress here.
 */
/*
 * [한국어] I/O 로그 변환 - 압축 해제(필요시) 및 바이트 오더 변환
 * 압축 방식: XMIT_COMPRESSED(zlib 해제), STORE_COMPRESSED(바이너리 직접 저장), 비압축
 * 각 샘플의 시간, 값, 방향, 블록 크기, 우선순위, 오프셋 등을 호스트 오더로 변환
 */
static struct cmd_iolog_pdu *convert_iolog(struct fio_net_cmd *cmd,
					   bool *store_direct)
{
	struct cmd_iolog_pdu *pdu = (struct cmd_iolog_pdu *) cmd->payload;
	struct cmd_iolog_pdu *ret;
	uint64_t i;
	int compressed;
	void *samples;

	*store_direct = false;

	/*
	 * Convert if compressed and we support it. If it's not
	 * compressed, we need not do anything.
	 */
	compressed = le32_to_cpu(pdu->compressed);
	if (compressed == XMIT_COMPRESSED) {
#ifndef CONFIG_ZLIB
		log_err("fio: server sent compressed data by mistake\n");
		return NULL;
#endif
		ret = convert_iolog_gz(cmd, pdu);
		if (!ret) {
			log_err("fio: failed decompressing log\n");
			return NULL;
		}
	} else if (compressed == STORE_COMPRESSED) {
		*store_direct = true;
		ret = pdu;
	} else
		ret = pdu;

	ret->nr_samples		= le64_to_cpu(ret->nr_samples);
	ret->thread_number	= le32_to_cpu(ret->thread_number);
	ret->log_type		= le32_to_cpu(ret->log_type);
	ret->compressed		= le32_to_cpu(ret->compressed);
	ret->log_offset		= le32_to_cpu(ret->log_offset);
	ret->log_prio		= le32_to_cpu(ret->log_prio);
	ret->log_issue_time	= le32_to_cpu(ret->log_issue_time);
	ret->log_hist_coarseness = le32_to_cpu(ret->log_hist_coarseness);
	ret->per_job_logs	= le32_to_cpu(ret->per_job_logs);

	if (*store_direct)
		return ret;

	samples = &ret->samples[0];
	for (i = 0; i < ret->nr_samples; i++) {
		struct io_sample *s;

		s = __get_sample(samples, ret->log_offset, ret->log_issue_time, i);
		if (ret->log_type == IO_LOG_TYPE_HIST)
			s = (struct io_sample *)((char *)s + sizeof(struct io_u_plat_entry) * i);

		s->time		= le64_to_cpu(s->time);
		if (ret->log_type != IO_LOG_TYPE_HIST) {
			s->data.val.val0	= le64_to_cpu(s->data.val.val0);
			s->data.val.val1	= le64_to_cpu(s->data.val.val1);
		}
		s->__ddir	= __le32_to_cpu(s->__ddir);
		s->bs		= le64_to_cpu(s->bs);
		s->priority	= le16_to_cpu(s->priority);

		if (ret->log_offset)
			s->aux[IOS_AUX_OFFSET_INDEX] =
				le64_to_cpu(s->aux[IOS_AUX_OFFSET_INDEX]);

		if (ret->log_issue_time)
			s->aux[IOS_AUX_ISSUE_TIME_INDEX] =
				le64_to_cpu(s->aux[IOS_AUX_ISSUE_TIME_INDEX]);

		if (ret->log_type == IO_LOG_TYPE_HIST) {
			s->data.plat_entry = (struct io_u_plat_entry *)(((char *)s) + sizeof(*s));
			s->data.plat_entry->list.next = NULL;
			s->data.plat_entry->list.prev = NULL;
		}
	}

	return ret;
}

/* [한국어] 파일 전송 응답을 서버에 전송 */
static void sendfile_reply(int fd, struct cmd_sendfile_reply *rep,
			   size_t size, uint64_t tag)
{
	rep->error = cpu_to_le32(rep->error);
	fio_net_send_cmd(fd, FIO_NET_CMD_SENDFILE, rep, size, &tag, NULL);
}

/* [한국어] 서버 요청에 따라 로컬 파일을 읽어서 전송 - 서버가 필요한 파일을 클라이언트에 요청 */
static int fio_send_file(struct fio_client *client, struct cmd_sendfile *pdu,
			 uint64_t tag)
{
	struct cmd_sendfile_reply *rep;
	struct stat sb;
	size_t size;
	int fd;

	size = sizeof(*rep);
	rep = malloc(size);

	if (stat((char *)pdu->path, &sb) < 0) {
fail:
		rep->error = errno;
		sendfile_reply(client->fd, rep, size, tag);
		free(rep);
		return 1;
	}

	size += sb.st_size;
	rep = realloc(rep, size);
	rep->size = cpu_to_le32((uint32_t) sb.st_size);

	fd = open((char *)pdu->path, O_RDONLY);
	if (fd == -1 )
		goto fail;

	rep->error = read_data(fd, &rep->data, sb.st_size);
	sendfile_reply(client->fd, rep, size, tag);
	free(rep);
	close(fd);
	return 0;
}

/*
 * [한국어] 단일 클라이언트의 수신 명령 처리 - opcode에 따라 적절한 핸들러로 분기
 *
 * 주요 opcode 처리:
 *   QUIT       → 클라이언트 제거
 *   TEXT       → 텍스트 메시지 출력
 *   DU         → 디스크 유틸리티 통계
 *   TS         → 스레드 통계 (steady-state 데이터 포함)
 *   GS         → 그룹 실행 통계
 *   ETA        → 예상 완료 시간
 *   PROBE      → 서버 정보 응답
 *   START/STOP → 잡 시작/정지
 *   IOLOG      → I/O 로그 데이터
 *   VTRIGGER   → 검증 트리거
 *   SENDFILE   → 파일 전송 요청
 *   JOB_OPT    → 잡 옵션 정보
 */
int fio_handle_client(struct fio_client *client)
{
	struct client_ops const *ops = client->ops;
	struct fio_net_cmd *cmd;

	dprint(FD_NET, "client: handle %s\n", client->hostname);

	cmd = fio_net_recv_cmd(client->fd, true);
	if (!cmd)
		return 0;

	dprint(FD_NET, "client: got cmd op %s from %s (pdu=%u)\n",
		fio_server_op(cmd->opcode), client->hostname, cmd->pdu_len);

	client->last_cmd = cmd->opcode;

	switch (cmd->opcode) {
	case FIO_NET_CMD_QUIT:
		if (ops->quit)
			ops->quit(client, cmd);
		remove_client(client);
		break;
	case FIO_NET_CMD_TEXT:
		convert_text(cmd);
		ops->text(client, cmd);
		break;
	case FIO_NET_CMD_DU: {
		struct cmd_du_pdu *du = (struct cmd_du_pdu *) cmd->payload;

		convert_dus(&du->dus);
		convert_agg(&du->agg);

		ops->disk_util(client, cmd);
		break;
		}
	case FIO_NET_CMD_TS: {
		struct cmd_ts_pdu *p = (struct cmd_ts_pdu *) cmd->payload;
		uint64_t offset;
		int i;

		for (i = 0; i < DDIR_RWDIR_CNT; i++) {
			if (le32_to_cpu(p->ts.nr_clat_prio[i])) {
				offset = le64_to_cpu(p->ts.clat_prio_offset[i]);
				p->ts.clat_prio[i] =
					(struct clat_prio_stat *)((char *)p + offset);
			}
		}

		dprint(FD_NET, "client: ts->ss_state = %u\n", (unsigned int) le32_to_cpu(p->ts.ss_state));
		if (le32_to_cpu(p->ts.ss_state) & FIO_SS_DATA) {
			dprint(FD_NET, "client: received steadystate ring buffers\n");

			offset = le64_to_cpu(p->ts.ss_iops_data_offset);
			p->ts.ss_iops_data = (uint64_t *)((char *)p + offset);

			offset = le64_to_cpu(p->ts.ss_bw_data_offset);
			p->ts.ss_bw_data = (uint64_t *)((char *)p + offset);

			offset = le64_to_cpu(p->ts.ss_lat_data_offset);
			p->ts.ss_lat_data = (uint64_t *)((char *)p + offset);
		}

		convert_ts(&p->ts, &p->ts);
		convert_gs(&p->rs, &p->rs);

		ops->thread_status(client, cmd);
		break;
		}
	case FIO_NET_CMD_GS: {
		struct group_run_stats *gs = (struct group_run_stats *) cmd->payload;

		convert_gs(gs, gs);

		ops->group_stats(client, cmd);
		break;
		}
	case FIO_NET_CMD_ETA: {
		struct jobs_eta *je = (struct jobs_eta *) cmd->payload;

		if (!remove_reply_cmd(client, cmd))
			break;
		convert_jobs_eta(je);
		handle_eta(client, cmd);
		break;
		}
	case FIO_NET_CMD_PROBE:
		remove_reply_cmd(client, cmd);
		ops->probe(client, cmd);
		break;
	case FIO_NET_CMD_SERVER_START:
		client->state = Client_running;
		if (ops->job_start)
			ops->job_start(client, cmd);
		break;
	case FIO_NET_CMD_START: {
		struct cmd_start_pdu *pdu = (struct cmd_start_pdu *) cmd->payload;

		pdu->jobs = le32_to_cpu(pdu->jobs);
		ops->start(client, cmd);
		break;
		}
	case FIO_NET_CMD_STOP: {
		struct cmd_end_pdu *pdu = (struct cmd_end_pdu *) cmd->payload;

		convert_stop(cmd);
		client->state = Client_stopped;
		client->error = le32_to_cpu(pdu->error);
		client->signal = le32_to_cpu(pdu->signal);
		ops->stop(client);
		break;
		}
	case FIO_NET_CMD_ADD_JOB: {
		struct cmd_add_job_pdu *pdu = (struct cmd_add_job_pdu *) cmd->payload;

		client->thread_number = le32_to_cpu(pdu->thread_number);
		client->groupid = le32_to_cpu(pdu->groupid);

		if (ops->add_job)
			ops->add_job(client, cmd);
		break;
		}
	case FIO_NET_CMD_IOLOG:
		fio_client_handle_iolog(client, cmd);
		break;
	case FIO_NET_CMD_UPDATE_JOB:
		ops->update_job(client, cmd);
		remove_reply_cmd(client, cmd);
		break;
	case FIO_NET_CMD_VTRIGGER: {
		struct all_io_list *pdu = (struct all_io_list *) cmd->payload;
		char buf[128];
		int off = 0;

		if (aux_path) {
			strcpy(buf, aux_path);
			off = strlen(buf);
		}

		__verify_save_state(pdu, server_name(client, &buf[off], sizeof(buf) - off));
		exec_trigger(trigger_cmd);
		break;
		}
	case FIO_NET_CMD_SENDFILE: {
		struct cmd_sendfile *pdu = (struct cmd_sendfile *) cmd->payload;
		fio_send_file(client, pdu, cmd->tag);
		break;
		}
	case FIO_NET_CMD_JOB_OPT: {
		handle_job_opt(client, cmd);
		break;
	}
	default:
		log_err("fio: unknown client op: %s\n", fio_server_op(cmd->opcode));
		break;
	}

	free(cmd);
	return 1;
}

/* [한국어] 모든 클라이언트에 검증 트리거 명령 전송 - verify_trigger 옵션과 연동 */
int fio_clients_send_trigger(const char *cmd)
{
	struct flist_head *entry;
	struct fio_client *client;
	size_t slen;

	dprint(FD_NET, "client: send vtrigger: %s\n", cmd);

	if (!cmd)
		slen = 0;
	else
		slen = strlen(cmd);

	flist_for_each(entry, &client_list) {
		struct cmd_vtrigger_pdu *pdu;

		client = flist_entry(entry, struct fio_client, list);

		pdu = malloc(sizeof(*pdu) + slen);
		pdu->len = cpu_to_le16((uint16_t) slen);
		if (slen)
			memcpy(pdu->cmd, cmd, slen);
		fio_net_send_cmd(client->fd, FIO_NET_CMD_VTRIGGER, pdu,
					sizeof(*pdu) + slen, NULL, NULL);
		free(pdu);
	}

	return 0;
}

/*
 * [한국어] 모든 실행 중인 클라이언트에 ETA 요청 전송
 * client_eta 구조체를 할당하고, 각 클라이언트에 SEND_ETA 명령을 보낸다.
 * 이미 ETA 요청 중인 클라이언트는 건너뛰고 pending 카운트를 감소시킨다.
 */
static void request_client_etas(struct client_ops const *ops)
{
	struct fio_client *client;
	struct flist_head *entry;
	struct client_eta *eta;
	int skipped = 0;

	if (eta_print == FIO_ETA_NEVER)
		return;

	dprint(FD_NET, "client: request eta (%d)\n", nr_clients);

	eta = calloc(1, sizeof(*eta) + __THREAD_RUNSTR_SZ(REAL_MAX_JOBS));
	eta->pending = nr_clients;

	flist_for_each(entry, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		if (!flist_empty(&client->eta_list)) {
			skipped++;
			continue;
		}
		if (client->state != Client_running)
			continue;

		assert(!client->eta_in_flight);
		flist_add_tail(&client->eta_list, &eta_list);
		client->eta_in_flight = eta;
		fio_net_send_simple_cmd(client->fd, FIO_NET_CMD_SEND_ETA,
					(uintptr_t) eta, &client->cmd_list);
	}

	while (skipped--) {
		if (!fio_client_dec_jobs_eta(eta, ops->eta))
			break;
	}

	dprint(FD_NET, "client: requested eta tag %p\n", eta);
}

/*
 * A single SEND_ETA timeout isn't fatal. Attempt to recover.
 */
/* [한국어] 명령 타임아웃 처리 - ETA 타임아웃은 5회까지 복구 시도, 그 외 명령은 즉시 실패 */
static int handle_cmd_timeout(struct fio_client *client,
			      struct fio_net_cmd_reply *reply)
{
	uint16_t reply_opcode = reply->opcode;

	flist_del(&reply->list);
	free(reply);

	if (reply_opcode != FIO_NET_CMD_SEND_ETA)
		return 1;

	log_info("client <%s>: timeout on SEND_ETA\n", client->hostname);

	flist_del_init(&client->eta_list);
	if (client->eta_in_flight) {
		fio_client_dec_jobs_eta(client->eta_in_flight, client->ops->eta);
		client->eta_in_flight = NULL;
	}

	/*
	 * If we fail 5 in a row, give up...
	 */
	if (client->eta_timeouts++ > 5)
		return 1;

	return 0;
}

/* [한국어] 클라이언트의 모든 대기 명령에 대해 타임아웃 검사 */
static int client_check_cmd_timeout(struct fio_client *client,
				    struct timespec *now)
{
	struct fio_net_cmd_reply *reply;
	struct flist_head *entry, *tmp;
	int ret = 0;

	flist_for_each_safe(entry, tmp, &client->cmd_list) {
		unsigned int op;

		reply = flist_entry(entry, struct fio_net_cmd_reply, list);

		if (mtime_since(&reply->ts, now) < FIO_NET_CLIENT_TIMEOUT)
			continue;

		op = reply->opcode;
		if (!handle_cmd_timeout(client, reply))
			continue;

		log_err("fio: client %s, timeout on cmd %s\n", client->hostname,
						fio_server_op(op));
		ret = 1;
	}

	return flist_empty(&client->cmd_list) && ret;
}

/* [한국어] 모든 클라이언트의 명령 타임아웃 검사 - 타임아웃된 클라이언트를 제거 */
static int fio_check_clients_timed_out(void)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;
	struct timespec ts;
	int ret = 0;

	fio_gettime(&ts, NULL);

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		if (flist_empty(&client->cmd_list))
			continue;

		if (!client_check_cmd_timeout(client, &ts))
			continue;

		if (client->ops->timed_out)
			client->ops->timed_out(client);
		else
			log_err("fio: client %s timed out\n", client->hostname);

		if (client->last_cmd != FIO_NET_CMD_VTRIGGER)
			client->error = ETIMEDOUT;
		else
			log_info("fio: ignoring timeout due to vtrigger\n");
		remove_client(client);
		ret = 1;
	}

	return ret;
}

/*
 * [한국어] 메인 클라이언트 이벤트 루프 - 모든 클라이언트가 종료될 때까지 poll()로 대기
 *
 * 동작 흐름:
 *   1) 잡 미전송 + 비연결 유지 클라이언트 제거
 *   2) poll()로 소켓 읽기 이벤트 대기 (타임아웃: min(100ms, eta_msec))
 *   3) ETA 주기 도래 시 request_client_etas() 호출
 *   4) 타임아웃 검사 및 타임아웃 클라이언트 제거
 *   5) 데이터 수신 시 fio_handle_client()로 명령 처리
 *   6) 모든 클라이언트 종료 후 합산 통계 출력 및 JSON 마무리
 */
int fio_handle_clients(struct client_ops const *ops)
{
	struct pollfd *pfds;
	int i, ret = 0, retval = 0;

	fio_gettime(&eta_ts, NULL);

	pfds = malloc(nr_clients * sizeof(struct pollfd));

	init_thread_stat(&client_ts);
	init_group_run_stat(&client_gs);

	while (!exit_backend && nr_clients) {
		struct flist_head *entry, *tmp;
		struct fio_client *client;

		i = 0;
		flist_for_each_safe(entry, tmp, &client_list) {
			client = flist_entry(entry, struct fio_client, list);

			if (!client->sent_job && !client->ops->stay_connected &&
			    flist_empty(&client->cmd_list)) {
				remove_client(client);
				continue;
			}

			pfds[i].fd = client->fd;
			pfds[i].events = POLLIN;
			i++;
		}

		if (!nr_clients)
			break;

		assert(i == nr_clients);

		do {
			struct timespec ts;
			int timeout;

			fio_gettime(&ts, NULL);
			if (eta_time_within_slack(mtime_since(&eta_ts, &ts))) {
				request_client_etas(ops);
				memcpy(&eta_ts, &ts, sizeof(ts));

				if (fio_check_clients_timed_out())
					break;
			}

			check_trigger_file();

			timeout = min(100u, ops->eta_msec);

			ret = poll(pfds, nr_clients, timeout);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				log_err("fio: poll clients: %s\n", strerror(errno));
				break;
			} else if (!ret)
				continue;
		} while (ret <= 0);

		for (i = 0; i < nr_clients; i++) {
			if (!(pfds[i].revents & POLLIN))
				continue;

			client = find_client_by_fd(pfds[i].fd);
			if (!client) {
				log_err("fio: unknown client fd %ld\n", (long) pfds[i].fd);
				continue;
			}
			if (!fio_handle_client(client)) {
				log_info("client: host=%s disconnected\n",
						client->hostname);
				remove_client(client);
				retval = 1;
			} else if (client->error)
				retval = 1;
			fio_put_client(client);
		}
	}

	log_info_buf(allclients.buf, allclients.buflen);
	buf_output_free(&allclients);

	fio_client_json_fini();

	free_clat_prio_stats(&client_ts);
	free(pfds);
	return retval || error_clients;
}

/* [한국어] ETA 화면 표시 - JSON 출력 모드가 아닐 때만 ETA 정보를 터미널에 출력 */
static void client_display_thread_status(struct jobs_eta *je)
{
	if (!(output_format & FIO_OUTPUT_JSON))
		display_thread_status(je);
}
