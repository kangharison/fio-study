/*
 * [한국어] server.h - fio 서버/클라이언트 네트워크 프로토콜 정의
 *
 * 이 헤더는 fio의 서버 모드에서 사용되는 모든 프로토콜 구조체,
 * 명령 코드(opcode), 그리고 서버 API 함수 선언을 포함한다.
 *
 * 주요 내용:
 *   1) 네트워크 명령 구조체 (fio_net_cmd) - 와이어 프로토콜의 기본 단위
 *   2) 명령 코드 (FIO_NET_CMD_*) - 서버-클라이언트 간 통신 명령 열거
 *   3) PDU 구조체들 (cmd_*_pdu) - 각 명령의 페이로드 데이터 형식
 *   4) sk_out 구조체 - 소켓 출력 관리 (참조 카운팅, 전송 큐)
 *   5) 서버 API 함수 선언 - 서버 시작, 통계 전송, 명령 송수신 등
 *
 * 프로토콜 특징:
 *   - 와이어 인코딩은 리틀 엔디안(little endian)
 *   - 명령 헤더와 페이로드 각각 CRC16 체크섬 포함
 *   - 큰 페이로드는 FIO_SERVER_MAX_FRAGMENT_PDU 단위로 분할 전송
 *   - zlib 압축 지원 (CONFIG_ZLIB 설정 시)
 */
#ifndef FIO_SERVER_H
#define FIO_SERVER_H

#include <inttypes.h>    /* 고정 크기 정수 타입 (uint16_t, uint32_t 등) */
#include <string.h>      /* 문자열/메모리 조작 함수 */
#include <sys/time.h>    /* 시간 관련 구조체 (timespec 등) */
#include <netinet/in.h>  /* 네트워크 주소 구조체 (sockaddr_in, in_addr 등) */

#include "stat.h"        /* 스레드 통계 구조체 (thread_stat, group_run_stats) */
#include "diskutil.h"    /* 디스크 유틸리티 통계 구조체 */

#define FIO_NET_PORT 8765  /* [한국어] 기본 서버 포트 번호 */

/*
 * [한국어] 소켓 출력 관리 구조체
 * 각 클라이언트 연결마다 하나씩 생성되며, 참조 카운팅으로 수명 관리.
 * 워커 스레드들이 전송할 데이터를 list에 큐잉하면, 메인 루프가 꺼내서 전송한다.
 */
struct sk_out {
	unsigned int refs;	/* frees sk_out when it drops to zero.
				 * protected by below ->lock */
				/* [한국어] 참조 카운트. 0이 되면 해제. lock으로 보호 */

#ifdef WIN32
	HANDLE hProcess;		/* process handle of handle_connection_process*/
					/* [한국어] 연결 처리 프로세스의 핸들 (Windows 전용) */
#endif
	int sk;			/* socket fd to talk to client */
				/* [한국어] 클라이언트와 통신하는 소켓 파일 디스크립터 */
	struct fio_sem lock;	/* protects ref and below list */
				/* [한국어] refs와 list를 보호하는 세마포어 (뮤텍스 용도) */
	struct flist_head list;	/* list of pending transmit work */
				/* [한국어] 전송 대기 중인 sk_entry 연결 리스트 */
	struct fio_sem wait;	/* wake backend when items added to list */
				/* [한국어] list에 항목 추가 시 백엔드를 깨우는 세마포어 */
	struct fio_sem xmit;	/* held while sending data */
				/* [한국어] 데이터 전송 중 점유되는 세마포어 (전송 직렬화) */
};

/*
 * On-wire encoding is little endian
 */
/*
 * [한국어] 네트워크 명령 구조체 - 와이어 프로토콜의 기본 패킷 형식
 * 모든 서버-클라이언트 통신은 이 구조체를 헤더로 사용한다.
 * cmd_crc16/pdu_crc16 필드 앞까지가 체크섬 대상이다.
 */
struct fio_net_cmd {
	uint16_t version;	/* protocol version */
				/* [한국어] 프로토콜 버전 (FIO_SERVER_VER) */
	uint16_t opcode;	/* command opcode */
				/* [한국어] 명령 코드 (FIO_NET_CMD_* 열거값) */
	uint32_t flags;		/* modifier flags */
				/* [한국어] 수정자 플래그 (예: FIO_NET_CMD_F_MORE - 후속 프래그먼트 존재) */
	uint64_t tag;		/* passed back on reply */
				/* [한국어] 응답 시 돌려보내는 태그 (요청-응답 매칭용) */
	uint32_t pdu_len;	/* length of post-cmd layload */
				/* [한국어] 명령 뒤에 오는 페이로드(PDU)의 길이 */
	/*
	 * These must be immediately before the payload, anything before
	 * these fields are checksummed.
	 */
	uint16_t cmd_crc16;	/* cmd checksum */
				/* [한국어] 명령 헤더의 CRC16 체크섬 */
	uint16_t pdu_crc16;	/* payload checksum */
				/* [한국어] 페이로드의 CRC16 체크섬 */
	uint8_t payload[];	/* payload */
				/* [한국어] 가변 길이 페이로드 (유연한 배열 멤버) */
};

/* [한국어] 명령 응답 추적 구조체 - 보낸 명령의 응답을 기다리기 위해 사용 */
struct fio_net_cmd_reply {
	struct flist_head list;  /* [한국어] 응답 대기 리스트 연결 */
	struct timespec ts;      /* [한국어] 명령 전송 시각 (타임아웃 검사용) */
	uint64_t saved_tag;      /* [한국어] 원래 태그 값 (응답 수신 후 복원) */
	uint16_t opcode;         /* [한국어] 명령 코드 */
};

/*
 * [한국어] 서버 프로토콜 상수 열거
 * - 프로토콜 버전, 프래그먼트 크기, 명령 코드, 플래그 등
 */
enum {
	FIO_SERVER_VER			= 120,  /* [한국어] 현재 프로토콜 버전 */

	FIO_SERVER_MAX_FRAGMENT_PDU	= 1024,  /* [한국어] 프래그먼트당 최대 PDU 크기 (바이트) */
	FIO_SERVER_MAX_CMD_MB		= 2048,  /* [한국어] 명령 + PDU 최대 크기 (MB 단위로 계산 시 사용) */

	/* [한국어] 명령 코드 (opcode) 정의 */
	FIO_NET_CMD_QUIT		= 1,   /* [한국어] 현재 잡 종료 요청 */
	FIO_NET_CMD_EXIT		= 2,   /* [한국어] 서버 완전 종료 */
	FIO_NET_CMD_JOB			= 3,   /* [한국어] 잡 설정 파일 내용 전송 (ini 형식) */
	FIO_NET_CMD_JOBLINE		= 4,   /* [한국어] 커맨드라인 인자로 잡 전송 */
	FIO_NET_CMD_TEXT		= 5,   /* [한국어] 텍스트 출력 전송 (로그 메시지) */
	FIO_NET_CMD_TS			= 6,   /* [한국어] 스레드 통계(thread_stat) 전송 */
	FIO_NET_CMD_GS			= 7,   /* [한국어] 그룹 통계(group_run_stats) 전송 */
	FIO_NET_CMD_SEND_ETA		= 8,   /* [한국어] ETA(예상 완료 시간) 요청 */
	FIO_NET_CMD_ETA			= 9,   /* [한국어] ETA 응답 데이터 */
	FIO_NET_CMD_PROBE		= 10,  /* [한국어] 서버 정보 조회 (호스트명, OS, 아키텍처 등) */
	FIO_NET_CMD_START		= 11,  /* [한국어] 잡 파싱 완료, 실행 준비됨 알림 */
	FIO_NET_CMD_STOP		= 12,  /* [한국어] 잡 실행 완료/중단 알림 (종료 코드 포함) */
	FIO_NET_CMD_DU			= 13,  /* [한국어] 디스크 유틸리티 통계 전송 */
	FIO_NET_CMD_SERVER_START	= 14,  /* [한국어] 서버 시작됨 알림 */
	FIO_NET_CMD_ADD_JOB		= 15,  /* [한국어] 잡 추가 알림 (thread_options 포함) */
	FIO_NET_CMD_RUN			= 16,  /* [한국어] 잡 실행 시작 명령 */
	FIO_NET_CMD_IOLOG		= 17,  /* [한국어] I/O 로그 데이터 전송 */
	FIO_NET_CMD_UPDATE_JOB		= 18,  /* [한국어] 실행 중 잡 옵션 업데이트 */
	FIO_NET_CMD_LOAD_FILE		= 19,  /* [한국어] 서버 로컬 파일에서 잡 설정 로드 */
	FIO_NET_CMD_VTRIGGER		= 20,  /* [한국어] verify 트리거 명령 */
	FIO_NET_CMD_SENDFILE		= 21,  /* [한국어] 파일 전송 요청 (verify state 등) */
	FIO_NET_CMD_JOB_OPT		= 22,  /* [한국어] 잡 옵션 전송 */
	FIO_NET_CMD_NR			= 23,  /* [한국어] 명령 코드 총 개수 (배열 크기용) */

	FIO_NET_CMD_F_MORE		= 1UL << 0,  /* [한국어] 후속 프래그먼트가 있음을 나타내는 플래그 */

	/* crc does not include the crc fields */
	FIO_NET_CMD_CRC_SZ		= sizeof(struct fio_net_cmd) -
						2 * sizeof(uint16_t),
				/* [한국어] CRC 계산 대상 크기 (cmd_crc16, pdu_crc16 제외) */

	FIO_NET_NAME_MAX		= 256,  /* [한국어] 이름/경로 최대 길이 */

	FIO_NET_CLIENT_TIMEOUT		= 5000, /* [한국어] 클라이언트 타임아웃 (밀리초) */

	FIO_PROBE_FLAG_ZLIB		= 1UL << 0,  /* [한국어] zlib 압축 지원 플래그 */
};

/* [한국어] 파일 전송 요청 PDU - verify state 파일 등을 클라이언트에게 요청 */
struct cmd_sendfile {
	uint8_t path[FIO_NET_NAME_MAX];  /* [한국어] 요청할 파일 경로 */
};

/* [한국어] 파일 전송 응답 PDU - 요청된 파일의 데이터를 포함 */
struct cmd_sendfile_reply {
	uint32_t size;       /* [한국어] 파일 데이터 크기 */
	uint32_t error;      /* [한국어] 에러 코드 (0이면 성공) */
	uint8_t data[0];     /* [한국어] 파일 데이터 (가변 길이) */
};

/*
 * Client sends this to server on VTRIGGER, server sends back a full
 * all_io_list structure.
 */
/* [한국어] verify 트리거 PDU - 클라이언트가 서버에 verify 트리거 명령을 보낼 때 사용 */
struct cmd_vtrigger_pdu {
	uint16_t len;    /* [한국어] 명령 문자열 길이 */
	uint8_t cmd[];   /* [한국어] 실행할 트리거 명령 문자열 */
};

/* [한국어] 파일 로드 PDU - 서버 로컬 파일에서 잡 설정을 로드할 때 사용 */
struct cmd_load_file_pdu {
	uint16_t name_len;      /* [한국어] 파일 이름 길이 */
	uint16_t client_type;   /* [한국어] 클라이언트 타입 */
	uint8_t file[];         /* [한국어] 파일 이름 (가변 길이) */
};

/* [한국어] 스레드 통계 PDU - thread_stat과 group_run_stats를 하나의 패킷으로 전송 */
struct cmd_ts_pdu {
	struct thread_stat ts;       /* [한국어] 개별 스레드 I/O 통계 */
	struct group_run_stats rs;   /* [한국어] 그룹 실행 통계 */
};

/* [한국어] 디스크 유틸리티 PDU - 디스크별 I/O 통계를 전송 */
struct cmd_du_pdu {
	struct disk_util_stat dus;   /* [한국어] 디스크 유틸리티 통계 */
	struct disk_util_agg agg;    /* [한국어] 디스크 통계 집계 */
};

/* [한국어] 클라이언트 프로브 요청 PDU - 서버 정보를 요청할 때 사용 */
struct cmd_client_probe_pdu {
	uint64_t flags;          /* [한국어] 프로브 플래그 (예: zlib 지원 여부) */
	uint8_t server[128];     /* [한국어] 서버 식별 문자열 */
};

/* [한국어] 프로브 응답 PDU - 서버의 시스템 정보를 클라이언트에게 전송 */
struct cmd_probe_reply_pdu {
	uint8_t hostname[64];    /* [한국어] 서버 호스트명 */
	uint8_t bigendian;       /* [한국어] 빅 엔디안 여부 (1=빅엔디안) */
	uint8_t fio_version[32]; /* [한국어] fio 버전 문자열 */
	uint8_t os;              /* [한국어] 운영체제 타입 (FIO_OS) */
	uint8_t arch;            /* [한국어] CPU 아키텍처 (FIO_ARCH) */
	uint8_t bpp;             /* [한국어] 포인터 크기 (바이트, 4 또는 8) */
	uint32_t cpus;           /* [한국어] CPU 개수 */
	uint64_t flags;          /* [한국어] 서버 기능 플래그 (예: zlib 지원) */
};

/* [한국어] 커맨드라인 단일 라인 PDU - JOBLINE 명령의 개별 인자 */
struct cmd_single_line_pdu {
	uint16_t len;     /* [한국어] 텍스트 길이 */
	uint8_t text[];   /* [한국어] 커맨드라인 인자 텍스트 */
};

/* [한국어] 커맨드라인 PDU - 여러 커맨드라인 인자를 묶어서 전송 */
struct cmd_line_pdu {
	uint16_t lines;          /* [한국어] 커맨드라인 인자 개수 */
	uint16_t client_type;    /* [한국어] 클라이언트 타입 */
	struct cmd_single_line_pdu options[];  /* [한국어] 개별 인자 배열 */
};

/* [한국어] 잡 PDU - 잡 설정 파일 내용을 버퍼로 전송 (ini 형식) */
struct cmd_job_pdu {
	uint32_t buf_len;       /* [한국어] 버퍼 길이 */
	uint32_t client_type;   /* [한국어] 클라이언트 타입 */
	uint8_t buf[0];         /* [한국어] 잡 설정 데이터 (가변 길이) */
};

/* [한국어] 시작 알림 PDU - 잡 파싱 완료 후 잡 수와 통계 출력 수를 알림 */
struct cmd_start_pdu {
	uint32_t jobs;           /* [한국어] 생성된 잡(스레드) 수 */
	uint32_t stat_outputs;   /* [한국어] 통계 출력 수 */
};

/* [한국어] 종료 PDU - 잡 실행 완료/중단 시 에러 코드와 시그널 정보 전송 */
struct cmd_end_pdu {
	uint32_t error;   /* [한국어] 에러 코드 (0이면 정상 종료) */
	uint32_t signal;  /* [한국어] 시그널 번호 (시그널로 종료된 경우) */
};

/* [한국어] 잡 추가 PDU - 새 잡의 스레드 옵션을 포함하여 클라이언트에 알림 */
struct cmd_add_job_pdu {
	uint32_t thread_number;        /* [한국어] 스레드 번호 */
	uint32_t groupid;              /* [한국어] 그룹 ID */
	struct thread_options_pack top; /* [한국어] 직렬화된 스레드 옵션 */
};

/* [한국어] 텍스트 출력 PDU - 로그 메시지를 클라이언트로 전송 */
struct cmd_text_pdu {
	uint32_t level;     /* [한국어] 로그 레벨 */
	uint32_t buf_len;   /* [한국어] 텍스트 길이 */
	uint64_t log_sec;   /* [한국어] 로그 시각 - 초 */
	uint64_t log_usec;  /* [한국어] 로그 시각 - 마이크로초 */
	uint8_t buf[0];     /* [한국어] 텍스트 데이터 (가변 길이) */
};

/* [한국어] I/O 로그 압축 모드 */
enum {
	XMIT_COMPRESSED		= 1U,  /* [한국어] 전송 시 실시간 압축 (zlib deflate) */
	STORE_COMPRESSED	= 2U,  /* [한국어] 저장 시 이미 압축된 청크 직접 전송 */
};

/* [한국어] I/O 로그 PDU - I/O 로그 데이터의 헤더 구조체 */
struct cmd_iolog_pdu {
	uint64_t nr_samples;             /* [한국어] 총 샘플 수 */
	uint32_t thread_number;          /* [한국어] 스레드 번호 */
	uint32_t log_type;               /* [한국어] 로그 타입 (대역폭, IOPS, 지연시간 등) */
	uint32_t compressed;             /* [한국어] 압축 모드 (XMIT_COMPRESSED/STORE_COMPRESSED/0) */
	uint32_t log_offset;             /* [한국어] 오프셋 로깅 여부 */
	uint32_t log_prio;               /* [한국어] 우선순위 로깅 여부 */
	uint32_t log_issue_time;         /* [한국어] 발행 시간 로깅 여부 */
	uint32_t log_hist_coarseness;    /* [한국어] 히스토그램 해상도 */
	uint32_t per_job_logs;           /* [한국어] 잡별 개별 로그 여부 */
	uint8_t name[FIO_NET_NAME_MAX];  /* [한국어] 로그 이름 */
	struct io_sample samples[0];     /* [한국어] I/O 샘플 데이터 (가변 길이) */
};

/* [한국어] 잡 옵션 PDU - 개별 잡 옵션을 이름-값 쌍으로 전송 */
struct cmd_job_option {
	uint16_t global;       /* [한국어] 전역 옵션 여부 (1=전역) */
	uint16_t truncated;    /* [한국어] 이름/값이 잘렸는지 여부 */
	uint32_t groupid;      /* [한국어] 그룹 ID */
	uint8_t name[64];      /* [한국어] 옵션 이름 */
	uint8_t value[128];    /* [한국어] 옵션 값 */
};

/* [한국어] 서버 API 함수 선언 */

extern int fio_start_server(char *);          /* [한국어] 서버 시작 (pidfile 지정 시 데몬화) */
extern int fio_server_text_output(int, const char *, size_t);  /* [한국어] 텍스트 로그를 클라이언트로 전송 */
extern int fio_net_send_cmd(int, uint16_t, const void *, off_t, uint64_t *, struct flist_head *);  /* [한국어] PDU 포함 명령 전송 (분할 가능) */
extern int fio_net_send_simple_cmd(int, uint16_t, uint64_t, struct flist_head *);  /* [한국어] 페이로드 없는 단순 명령 전송 */
extern void fio_server_set_arg(const char *);            /* [한국어] 서버 주소/포트 인자 설정 */
extern void fio_server_internal_set(const char *);       /* [한국어] 내부 파이프 이름 설정 (Windows) */
extern int fio_server_parse_string(const char *, char **, bool *, int *, struct in_addr *, struct in6_addr *, int *);  /* [한국어] 서버 주소 문자열 파싱 */
extern int fio_server_parse_host(const char *, int, struct in_addr *, struct in6_addr *);  /* [한국어] 호스트명/IP 주소 파싱 */
extern const char *fio_server_op(unsigned int);          /* [한국어] opcode를 문자열로 변환 */
extern void fio_server_got_signal(int);                  /* [한국어] 시그널 수신 처리 */

extern void fio_server_send_ts(struct thread_stat *, struct group_run_stats *);  /* [한국어] 스레드 통계를 클라이언트로 전송 */
extern void fio_server_send_gs(struct group_run_stats *);    /* [한국어] 그룹 통계를 클라이언트로 전송 */
extern void fio_server_send_du(void);                        /* [한국어] 디스크 유틸리티 통계를 클라이언트로 전송 */
extern void fio_server_send_job_options(struct flist_head *, unsigned int);  /* [한국어] 잡 옵션을 클라이언트로 전송 */
extern int fio_server_get_verify_state(const char *, int, void **);  /* [한국어] verify state를 클라이언트에서 수신 */
extern bool fio_server_poll_fd(int fd, short events, int timeout);   /* [한국어] 파일 디스크립터 이벤트 폴링 */

extern struct fio_net_cmd *fio_net_recv_cmd(int sk, bool wait);  /* [한국어] 네트워크 명령 수신 (분할 패킷 재조립) */

extern int fio_send_iolog(struct thread_data *, struct io_log *, const char *);  /* [한국어] I/O 로그를 클라이언트로 전송 */
extern void fio_server_send_add_job(struct thread_data *);   /* [한국어] 잡 추가 알림을 클라이언트로 전송 */
extern void fio_server_send_start(struct thread_data *);     /* [한국어] 서버 시작 알림을 클라이언트로 전송 */
extern int fio_net_send_quit(int sk);                        /* [한국어] QUIT 명령 전송 */

extern int fio_server_create_sk_key(void);    /* [한국어] 스레드별 sk_out 키 생성 (pthread_key) */
extern void fio_server_destroy_sk_key(void);  /* [한국어] 스레드별 sk_out 키 삭제 */

extern bool exit_backend;  /* [한국어] 백엔드 종료 플래그 (true면 서버 루프 탈출) */
extern int fio_net_port;   /* [한국어] 서버 포트 번호 (기본값 FIO_NET_PORT) */

#endif
