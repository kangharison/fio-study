/*
 * [한국어] client.h - fio 클라이언트 모드 헤더 파일
 *
 * 이 파일은 fio의 클라이언트-서버 모드에서 클라이언트 측 구조체와 API를 정의한다.
 * 주요 내용:
 *   1) fio_client 구조체    - 원격 fio 서버와의 연결 상태 및 통신 정보를 보관
 *   2) client_ops 구조체    - 서버로부터 수신한 명령에 대한 콜백 함수 테이블
 *   3) client_eta 구조체    - 여러 클라이언트의 ETA(예상 완료 시간) 집계
 *   4) 외부 함수 선언       - 연결, 작업 전송, 결과 수신 등 클라이언트 API
 *
 * 동작 원리:
 *   fio --client=<host> 로 실행하면 이 헤더에 정의된 구조체/함수를 통해
 *   원격 서버에 잡 파일을 전송하고, 실행 결과(통계, ETA, 디스크 유틸 등)를 수신한다.
 */
#ifndef CLIENT_H
#define CLIENT_H

/* 시스템 네트워크 헤더 */
#include <sys/un.h>        /* Unix 도메인 소켓 주소 (sockaddr_un) */
#include <netinet/in.h>    /* IPv4/IPv6 소켓 주소 (sockaddr_in, sockaddr_in6) */
#include <arpa/inet.h>     /* IP 주소 변환 함수 (inet_ntop 등) */

/* fio 내부 헤더 */
#include "lib/types.h"     /* fio 기본 타입 정의 */
#include "stat.h"          /* 통계 구조체 (thread_stat, group_run_stats 등) */

struct fio_net_cmd;        /* 네트워크 명령 구조체 전방 선언 */

/* [한국어] 클라이언트 상태 열거형 - 연결 생명주기를 추적 */
enum {
	Client_created		= 0,   /* 클라이언트 객체 생성됨 */
	Client_connected	= 1,   /* 서버에 TCP/소켓 연결 완료 */
	Client_started		= 2,   /* 잡 실행 시작됨 */
	Client_running		= 3,   /* I/O 실행 중 */
	Client_stopped		= 4,   /* 잡 실행 중단됨 */
	Client_exited		= 5,   /* 클라이언트 종료됨 */
};

/* [한국어] 클라이언트 파일 정보 - 서버에 전송할 잡 파일 */
struct client_file {
	char *file;     /* 잡 파일 경로 */
	bool remote;    /* true: 서버 측 파일, false: 로컬 파일을 전송 */
};

/*
 * [한국어] fio_client - 원격 fio 서버와의 연결을 나타내는 핵심 구조체
 *
 * 하나의 fio_client 인스턴스가 하나의 원격 서버 연결에 대응한다.
 * 연결 정보, 상태, 통계, 커맨드라인 인자, 콜백 등을 모두 포함한다.
 */
struct fio_client {
	struct flist_head list;        /* 전역 client_list에 연결되는 리스트 노드 */
	struct flist_head hash_list;   /* fd 기반 해시 테이블 연결 노드 */
	struct flist_head arg_list;    /* 공유 인자 그룹 리스트 노드 */
	union {
		struct sockaddr_in addr;     /* IPv4 소켓 주소 */
		struct sockaddr_in6 addr6;   /* IPv6 소켓 주소 */
		struct sockaddr_un addr_un;  /* Unix 도메인 소켓 주소 */
	};
	char *hostname;                /* 서버 호스트명 또는 소켓 경로 */
	int port;                      /* 서버 포트 번호 */
	int fd;                        /* 서버와의 소켓 파일 디스크립터 */
	unsigned int refs;             /* 참조 카운트 (0이 되면 메모리 해제) */
	unsigned int last_cmd;         /* 마지막으로 수신한 명령 opcode */

	char *name;                    /* 서버가 보내준 호스트명 (probe 응답) */

	struct flist_head *opt_lists;  /* 잡별 옵션 리스트 배열 */
	struct json_object *global_opts; /* JSON 출력용 전역 옵션 객체 */

	int state;                     /* 현재 클라이언트 상태 (Client_created ~ Client_exited) */

	bool skip_newline;             /* 텍스트 출력 시 줄바꿈 건너뛰기 여부 */
	bool is_sock;                  /* Unix 도메인 소켓 연결 여부 */
	bool disk_stats_shown;         /* 디스크 통계 출력 여부 플래그 */
	unsigned int jobs;             /* 서버에서 실행 중인 잡 개수 */
	unsigned int nr_stat;          /* 통계 출력 개수 */
	int error;                     /* 에러 코드 (0이면 정상) */
	int signal;                    /* 수신한 시그널 번호 */
	int ipv6;                      /* IPv6 사용 여부 */
	bool sent_job;                 /* 잡 전송 완료 여부 */
	bool did_stat;                 /* 통계 처리 완료 여부 */
	uint32_t type;                 /* 클라이언트 타입 (CLI/GUI) */

	uint32_t thread_number;        /* 서버 측 스레드 번호 */
	uint32_t groupid;              /* 서버 측 그룹 ID */

	struct flist_head eta_list;    /* ETA 요청 대기 리스트 노드 */
	struct client_eta *eta_in_flight; /* 현재 진행 중인 ETA 요청 */
	unsigned int eta_timeouts;     /* 연속 ETA 타임아웃 횟수 */

	struct flist_head cmd_list;    /* 응답 대기 중인 명령 리스트 */

	uint16_t argc;                 /* 커맨드라인 인자 개수 */
	char **argv;                   /* 커맨드라인 인자 배열 */

	struct client_ops const *ops;  /* 콜백 함수 테이블 포인터 */
	void *client_data;             /* 사용자 정의 데이터 (GUI 등에서 사용) */

	struct client_file *files;     /* 전송할 잡 파일 배열 */
	unsigned int nr_files;         /* 잡 파일 개수 */

	struct buf_output buf;         /* 출력 버퍼 (클라이언트별 로그 축적) */
};

/* [한국어] 콜백 함수 타입 정의 - 서버 명령 수신 시 호출되는 핸들러들 */
typedef void (client_cmd_op)(struct fio_client *, struct fio_net_cmd *);  /* 명령+페이로드 처리 */
typedef void (client_op)(struct fio_client *);                            /* 클라이언트만 전달 */
typedef void (client_eta_op)(struct jobs_eta *je);                        /* ETA 집계 결과 표시 */
typedef void (client_timed_out_op)(struct fio_client *);                  /* 타임아웃 처리 */
typedef void (client_jobs_eta_op)(struct fio_client *client, struct jobs_eta *je); /* 클라이언트별 ETA */

extern struct client_ops const fio_client_ops; /* 기본 CLI 모드 콜백 테이블 */

/*
 * [한국어] client_ops - 서버로부터 수신한 각종 명령에 대한 콜백 함수 테이블
 *
 * CLI 모드와 GUI 모드(gfio)가 각각 다른 client_ops를 등록하여
 * 동일한 프로토콜 위에서 서로 다른 방식으로 결과를 표시할 수 있다.
 */
struct client_ops {
	client_cmd_op		*text;         /* 텍스트 메시지 수신 콜백 */
	client_cmd_op		*disk_util;    /* 디스크 유틸리티 통계 수신 콜백 */
	client_cmd_op		*thread_status;/* 스레드 통계(thread_stat) 수신 콜백 */
	client_cmd_op		*group_stats;  /* 그룹 통계(group_run_stats) 수신 콜백 */
	client_jobs_eta_op	*jobs_eta;     /* 클라이언트별 ETA 수신 콜백 */
	client_eta_op		*eta;          /* 집계된 ETA 표시 콜백 */
	client_cmd_op		*probe;        /* probe 응답(서버 정보) 수신 콜백 */
	client_cmd_op		*quit;         /* 서버 종료 알림 수신 콜백 */
	client_cmd_op		*add_job;      /* 잡 추가 알림 수신 콜백 */
	client_cmd_op		*update_job;   /* 잡 업데이트 응답 수신 콜백 */
	client_timed_out_op	*timed_out;    /* 명령 타임아웃 콜백 */
	client_op		*stop;         /* 잡 정지 알림 수신 콜백 */
	client_cmd_op		*start;        /* 잡 시작 알림 수신 콜백 */
	client_cmd_op		*job_start;    /* 서버 실행 시작 알림 콜백 */
	client_timed_out_op	*removed;      /* 클라이언트 제거 시 콜백 */

	unsigned int eta_msec;  /* ETA 요청 주기 (밀리초) */
	int stay_connected;     /* 잡 완료 후에도 연결 유지 여부 */
	uint32_t client_type;   /* 클라이언트 타입 (FIO_CLIENT_TYPE_CLI/GUI) */
};

/* [한국어] client_eta - 여러 클라이언트의 ETA를 집계하기 위한 구조체 */
struct client_eta {
	unsigned int pending;   /* 아직 응답을 받지 못한 클라이언트 수 */
	struct jobs_eta eta;    /* 집계된 ETA 데이터 */
};

/* [한국어] 클라이언트 핸들링 및 ETA 집계 함수 */
extern int fio_handle_client(struct fio_client *);      /* 단일 클라이언트의 수신 명령 처리 */
extern void fio_client_sum_jobs_eta(struct jobs_eta *dst, struct jobs_eta *je); /* ETA 합산 */

/* [한국어] 클라이언트 연결 타입 열거형 */
enum {
	Fio_client_ipv4 = 1,    /* IPv4 TCP 연결 */
	Fio_client_ipv6,        /* IPv6 TCP 연결 */
	Fio_client_socket,      /* Unix 도메인 소켓 연결 */
};

/* [한국어] 클라이언트 API 함수 선언 */
extern int fio_client_connect(struct fio_client *);      /* 단일 클라이언트 연결 */
extern int fio_clients_connect(void);                    /* 모든 클라이언트 연결 */
extern int fio_start_client(struct fio_client *);        /* 단일 클라이언트 잡 실행 시작 */
extern int fio_start_all_clients(void);                  /* 모든 클라이언트 잡 실행 시작 */
extern int fio_clients_send_ini(const char *);           /* 모든 클라이언트에 잡 파일 전송 */
extern int fio_client_send_ini(struct fio_client *, const char *, bool); /* 단일 클라이언트에 잡 파일 전송 */
extern int fio_handle_clients(struct client_ops const*); /* 메인 이벤트 루프 - 모든 클라이언트 처리 */
extern int fio_client_add(struct client_ops const*, const char *, void **); /* 클라이언트 추가 (호스트명 파싱) */
extern struct fio_client *fio_client_add_explicit(struct client_ops *, const char *, int, int); /* 명시적 클라이언트 추가 */
extern void fio_client_add_cmd_option(void *, const char *);  /* 커맨드라인 옵션 추가 */
extern int fio_client_add_ini_file(void *, const char *, bool); /* 잡 설정 파일 추가 */
extern int fio_client_terminate(struct fio_client *);    /* 클라이언트에 종료 명령 전송 */
extern struct fio_client *fio_get_client(struct fio_client *); /* 참조 카운트 증가 */
extern void fio_put_client(struct fio_client *);         /* 참조 카운트 감소 (0이면 해제) */
extern int fio_client_update_options(struct fio_client *, struct thread_options *, uint64_t *); /* 잡 옵션 업데이트 */
extern int fio_client_wait_for_reply(struct fio_client *, uint64_t); /* 특정 태그의 응답 대기 */
extern int fio_clients_send_trigger(const char *);       /* 모든 클라이언트에 트리거 명령 전송 */

#define FIO_CLIENT_DEF_ETA_MSEC		900  /* 기본 ETA 요청 주기: 900ms */

/* [한국어] 클라이언트 타입 - CLI(터미널)와 GUI(gfio) 구분 */
enum {
	FIO_CLIENT_TYPE_CLI		= 1,   /* 커맨드라인 인터페이스 */
	FIO_CLIENT_TYPE_GUI		= 2,   /* 그래픽 인터페이스 (gfio) */
};

/* [한국어] 전역 통계 변수 - 여러 클라이언트의 결과를 합산 */
extern int sum_stat_clients;              /* 통계를 합산할 클라이언트 수 */
extern struct thread_stat client_ts;      /* 합산된 스레드 통계 */
extern struct group_run_stats client_gs;  /* 합산된 그룹 실행 통계 */

#endif

