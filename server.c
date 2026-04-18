/*
 * [한국어 설명] fio 서버 모드 구현 (server.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 "서버(backend) 모드"를 구현한다. fio는 두 가지 원격 실행 모델을 지원한다:
 *   1) --server [args]          : TCP 또는 Unix 도메인 소켓으로 listen 하는 데몬.
 *                                  원격 잡 파일을 수신하여 로컬 I/O 벤치마크를 실행하고
 *                                  결과(통계/로그/ETA)를 역방향으로 전송한다.
 *   2) --client=<host> <file>   : 클라이언트 측(client.c)이 서버로 접속하여 잡 파일을
 *                                  전송·원격 실행·결과 수집한다. (client.c 와 대칭)
 * 본 파일은 서버 쪽(=즉 실제 I/O 를 실행하는 쪽) 의 프로토콜 엔진으로서 다음을 전담한다:
 *   (a) 소켓 초기화·바인드·listen·accept 루프.
 *   (b) 연결 당 프로세스 fork(POSIX) 또는 스레드(Windows) 생성.
 *   (c) fio_net_cmd 단위 프로토콜 수신/송신 (헤더 CRC16 + PDU CRC16 + 리틀엔디안 직렬화).
 *   (d) opcode 디스패치(handle_command) 및 하위 handle_*_cmd 처리.
 *   (e) fio_backend() 실행 중 발생하는 텍스트 로그/ETA/통계/IOLOG/디스크 유틸을 클라이언트로 스트리밍.
 *   (f) zlib 압축(use_zlib), verify 상태 파일 요청(SENDFILE), jobs_eta 수집, disk_util 전송.
 *   (g) 데몬화(setsid+syslog), pidfile 관리, 시그널 핸들링(SIGINT/SIGPIPE), Unix 소켓 정리.
 *
 * TLS 계층은 존재하지 않는다. fio 서버/클라이언트 프로토콜은 "신뢰할 수 있는 네트워크"를
 * 가정한 순수 TCP/Unix 스트림 위에서 CRC16 체크섬만으로 무결성을 검증하며, 인증이나 암호화는
 * 제공하지 않는다. 공개망에 노출 시 별도의 터널링(SSH/STunnel/VPN) 이 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   main() [fio.c]
 *     └─ is_backend 판정 시 → fio_start_server(pidfile)
 *             │
 *             ├─ check_existing_pidfile() / fork() / setsid() / syslog         [데몬화]
 *             │
 *             └─ fio_server()
 *                   ├─ fio_handle_server_arg()     인자 파싱(sock:/ip:/ip6:/:port)
 *                   ├─ set_sig_handlers()           SIGINT/SIGBREAK → sig_int
 *                   ├─ fio_init_server_connection() socket + setsockopt + bind + listen
 *                   └─ accept_loop(listen_sk)
 *                         │   (루프: poll(POLLIN) → accept → scalloc sk_out → fork)
 *                         │
 *                         │   [부모 프로세스]
 *                         │     └─ fio_server_add_conn_pid(conn_list, pid)
 *                         │
 *                         │   [자식 프로세스]
 *                         └───── handle_connection(sk_out)
 *                                   (루프: poll → handle_xmits → fio_net_recv_cmd → handle_command)
 *                                   │
 *                                   ├─ handle_job_cmd        FIO_NET_CMD_JOB / JOBLINE → parse_jobs_ini
 *                                   ├─ handle_probe_cmd      FIO_NET_CMD_PROBE       → zlib 네고
 *                                   ├─ handle_send_eta_cmd   FIO_NET_CMD_SEND_ETA    → jobs_eta
 *                                   ├─ handle_update_job_cmd FIO_NET_CMD_UPDATE_JOB  → 실행 중 옵션 변경
 *                                   ├─ handle_trigger_cmd    FIO_NET_CMD_VTRIGGER    → exec_trigger
 *                                   ├─ handle_load_file_cmd  FIO_NET_CMD_LOAD_FILE   → 서버 로컬 ini
 *                                   ├─ handle_run_cmd        FIO_NET_CMD_RUN
 *                                   │     └─ [자손 프로세스] fio_backend(sk_out) [backend.c]
 *                                   │              │
 *                                   │              ├─ 스레드 실행 중 ─→ log_err/info/log_ts_write 등 후크가
 *                                   │              │   fio_server_text_output()/fio_server_send_ts()/
 *                                   │              │   fio_server_send_gs()/fio_server_send_du()/
 *                                   │              │   fio_send_iolog()/fio_server_send_add_job() 호출
 *                                   │              │   → sk_out->list 로 큐잉 → handle_xmits 가 flush
 *                                   │              │
 *                                   │              └─ 종료 시 fio_server_fork_item_done → STOP + QUIT 큐잉
 *                                   │
 *                                   └─ FIO_NET_CMD_EXIT 수신 시 exit_backend=true → 루프 탈출
 *
 * 즉, 본 파일은 "소켓 <-> opcode 디스패치 <-> fio_backend()" 세 계층을 연결하는 프로토콜 허브이며,
 * 실제 I/O 는 fio_backend 아래의 잡 스레드가, 결과 스트리밍은 handle_xmits 가 직렬화한다.
 *
 * === fio_net_cmd 패킷 포맷 (server.h 정의) ===
 *
 *   고정 헤더 (sizeof(struct fio_net_cmd) = 24 바이트, 리틀엔디안):
 *     +----+----+----+----+--------+--------+----+----+----+----+----+----+----+----+
 *     | version | opcode  |  flags          | tag (8 bytes)                         |
 *     +----+----+----+----+--------+--------+----+----+----+----+----+----+----+----+
 *     | pdu_len (4 bytes)   | cmd_crc16 | pdu_crc16 |   payload ... (pdu_len bytes) |
 *     +---------------------+-----------+-----------+-------------------------------+
 *
 *   - version     : FIO_SERVER_VER (mismatch 시 연결 거부)
 *   - opcode      : FIO_NET_CMD_* (아래 opcode 표 참조)
 *   - flags       : FIO_NET_CMD_F_MORE(1<<0) — 프래그먼트 계속
 *   - tag         : 클라이언트가 전달한 요청 태그 (alloc_reply/free_reply 로 포인터 인코딩 가능)
 *   - pdu_len     : 가변 페이로드 길이 (최대 FIO_SERVER_MAX_FRAGMENT_PDU, 그 이상은 MORE 분할)
 *   - cmd_crc16   : 헤더만(payload 이전까지 FIO_NET_CMD_CRC_SZ 바이트) 의 CRC16
 *   - pdu_crc16   : 페이로드 전체의 CRC16 (pdu_len == 0 일 때도 필드 존재)
 *   - payload     : 가변 데이터(opcode 별 cmd_*_pdu 구조체 리틀엔디안 직렬화)
 *
 *   대형 PDU 처리: pdu_len 이 FIO_SERVER_MAX_FRAGMENT_PDU 초과 시 fio_net_send_cmd /
 *   fio_send_cmd_ext_pdu 가 청크로 나눠 각 조각에 FIO_NET_CMD_F_MORE 를 설정하고,
 *   수신 측 fio_net_recv_cmd 는 동일 opcode 로 들어오는 연속 조각을 realloc 로 이어붙인 뒤
 *   FIO_NET_CMD_TEXT / FIO_NET_CMD_JOB 의 경우 NUL 종결자를 덧붙여 반환한다.
 *
 * === FIO_NET_CMD_* opcode 전수 (server.h enum 순서) ===
 *
 *   QUIT(1)         : 현재 잡 종료 요청(fio_terminate_threads 호출).
 *   EXIT(2)         : 서버 완전 종료(exit_backend=true, accept_loop 종료).
 *   JOB(3)          : 클라이언트가 보낸 ini 잡 파일 버퍼 → parse_jobs_ini.
 *   JOBLINE(4)      : CLI argv 전송 → parse_cmd_line.
 *   TEXT(5)         : 서버→클라이언트 로그 출력(log_info/log_err 리다이렉트).
 *   TS(6)           : thread_stat + group_run_stats + (옵션) clat_prio/steadystate 링버퍼.
 *   GS(7)           : group_run_stats 단독 전송.
 *   SEND_ETA(8)     : 클라이언트의 ETA 요청.
 *   ETA(9)          : 서버의 jobs_eta 응답.
 *   PROBE(10)       : 서버 식별/특성(OS/arch/bpp/cpus/fio_version/hostname/flags) 교환.
 *   START(11)       : JOB/JOBLINE 파싱 완료 보고(thread_number/stat_number 반환).
 *   STOP(12)        : 잡 실행 종료 보고(exitval + signal).
 *   DU(13)          : disk_util_stat + disk_util_agg (디스크 I/O 카운터) 전송.
 *   SERVER_START(14): fio_backend() 가 잡 스레드 준비 완료 시 전송.
 *   ADD_JOB(15)     : thread_options_pack 으로 직렬화한 옵션 스냅샷.
 *   RUN(16)         : 잡 실행 시작 트리거 → fork → fio_backend.
 *   IOLOG(17)       : I/O 로그(BW/IOPS/LAT/CLAT) 샘플 배치. 압축 옵션 3가지.
 *   UPDATE_JOB(18)  : 실행 중 옵션 변경(convert_thread_options_to_cpu).
 *   LOAD_FILE(19)   : 서버 로컬 ini 경로 전달 → 서버가 파싱.
 *   VTRIGGER(20)    : verify 트리거 실행 + all_io_list 반환.
 *   SENDFILE(21)    : verify state 파일 요청/응답 (cmd_reply 세마포어 동기화).
 *   JOB_OPT(22)     : 파싱된 (key, value) 옵션 이름·값 쌍 전송(print_option).
 *   NR(23)          : opcode 총 개수 (배열 크기).
 *
 * === 전송 경로 (TCP vs Unix 도메인) ===
 *
 *   IP 모드  (fio_init_server_ip):
 *     socket(AF_INET or AF_INET6, SOCK_STREAM, 0)
 *       → setsockopt(SO_REUSEADDR, SO_REUSEPORT)
 *       → bind(saddr_in/saddr_in6)
 *   Unix 모드 (fio_init_server_sock):
 *     socket(AF_UNIX, SOCK_STREAM, 0)
 *       → umask(000) 로 권한 확장
 *       → bind(sun_path = bind_sock)
 *   listen(sk, 4) 로 백로그 4 설정 — 다수 클라 동시 접속은 드물다.
 *
 * === fork_server_loop 스레드/프로세스 모델 ===
 *
 *   POSIX:
 *     연결 수락마다 fork() — 메모리 공간 분리 → 자식이 _exit() 하면 부모 불변.
 *     자식은 다시 handle_run_cmd 시점에 한 번 더 fork() 해서 fio_backend 의 잡 프로세스를 만든다.
 *     conn_list: 연결 처리 프로세스 추적 (waitpid WNOHANG, STOP/QUIT 미전송).
 *     job_list : 잡 실행 프로세스 추적 (종료 시 STOP/QUIT 를 클라이언트로 전송).
 *   Windows:
 *     fork() 부재 → CreateProcess 를 windows_handle_connection 이 대신 수행 +
 *     WSASocket 으로 소켓 핸들을 명명된 파이프를 통해 자식으로 복제.
 *     fio_backend 는 pthread_create 로 별도 스레드에서 실행(Windows 는 process 격리가 비효율적).
 *
 *   두 경우 모두 fio_fork_item 구조체에 (pid or thread handle, exitval, signal, exited) 를 담아
 *   fio_server_check_fork_items → fio_server_fork_item_done 에서 회수한다.
 *
 * === CRC / 바이트 오더 / IEEE 754 ===
 *
 *   - CRC16 : crc/crc16.c 의 CCITT 다항식. 헤더는 pdu 필드 제외한 FIO_NET_CMD_CRC_SZ 만큼,
 *             페이로드는 pdu_len 전체. 검증 실패 시 verify_convert_cmd 가 즉시 연결 종료.
 *   - 바이트 오더 : cpu_to_le16/32/64 / le16/32/64_to_cpu (arch/arch-*.h 정의). htonll/ntohll 은
 *             server.h 에서 아키텍처별로 분기. 모든 페이로드 필드를 직렬화 시점에 LE 로 변환하고
 *             수신 측은 verify_convert_cmd 에서 역변환한다.
 *   - IEEE 754: fio_double_to_uint64() 로 double 의 비트 패턴을 uint64 로 보존(엔디안 독립적).
 *               수신 측은 fio_uint64_to_double() 로 복원 — mean/S/latency_percentile/ss_* 등.
 *
 * === 타 모듈과의 연결 ===
 *
 * - fio.c            : main() 이 is_backend 플래그에 따라 fio_start_server()를 호출하며 진입.
 * - backend.c        : handle_run_cmd 가 fork 후 fio_backend(sk_out) 을 호출해 실제 잡 실행.
 * - server.h         : fio_net_cmd 헤더 레이아웃, FIO_NET_CMD_* enum, cmd_*_pdu 페이로드 구조체,
 *                      FIO_SERVER_VER, FIO_SERVER_MAX_FRAGMENT_PDU, FIO_SERVER_MAX_CMD_MB 정의.
 * - client.c         : 이 서버와 대칭되는 클라이언트 — sk_out 쪽에서 fio_net_send_cmd → 서버,
 *                      handle_client_fd → fio_net_recv_cmd 로 결과 수신.
 * - stat.c           : get_jobs_eta / thread_stat / group_run_stats / disk_util_stat / io_log
 *                      원본 데이터 공급. 본 파일은 직렬화 + 전송만 담당.
 * - iolog.c          : io_log / io_logs / io_sample / iolog_compress / chunk_list 구조 정의.
 *                      fio_send_iolog() 가 이를 FIO_NET_CMD_IOLOG 로 스트리밍.
 * - smalloc.c        : SK_F_COPY 플래그 버퍼를 scalloc/sfree(공유 메모리) 로 다뤄, 잡 fork 자식이
 *                      동일 sk_out->list 에 큐잉 가능하게 한다.
 * - crc/crc16.c      : fio_crc16() — CMD/PDU CRC 계산.
 * - lib/ieee754.c    : fio_double_to_uint64 / fio_uint64_to_double — 부동소수점 직렬화.
 * - verify-state.h   : verify_state_hdr/thread_io_list 포맷 + verify_state_gen_name.
 * - options.c        : convert_thread_options_to_net/cpu — thread_options 직렬화(대부분 고정 레이아웃).
 *
 * === 공유 상태 ===
 *
 * - exit_backend     : 전역 플래그. 시그널 또는 EXIT 명령 수신 시 true. 모든 poll/send 루프에서 검사.
 * - sk_out_key       : pthread TLS 키. sk_out 구조체를 현재 스레드에 연결(sk_out_assign/drop).
 * - sk_out->lock     : 리스트 접근 직렬화 세마포어(뮤텍스 역할).
 * - sk_out->wait     : 잠든 전송 스레드 깨우기용(0→1 시그널).
 * - sk_out->xmit     : 소켓 송신 직렬화 세마포어(writev 중간에 다른 스레드 개입 차단).
 * - sk_out->refs     : 참조 카운트. 자식 스레드/자식 프로세스 완료 시 drop.
 * - me[128]          : PROBE 명령으로 클라이언트가 알려준 "서버가 자신을 인식하는 이름"
 *                      (verify state 파일명 조합에 사용).
 * - use_zlib         : PROBE 교환 결과 양측 모두 zlib 지원 시 1 → fio_send_iolog 가 IOLOG 를 압축.
 *
 * === 주요 함수/구조체 요약 ===
 *
 *   프로토콜 저수준:
 *     fio_send_data / fio_sendv_data / fio_recv_data : writev/recv 루프(부분 전송 재시도).
 *     verify_convert_cmd                              : 헤더 CRC 검증 + LE→CPU 변환.
 *     fio_net_recv_cmd / fio_net_send_cmd             : 프래그먼트 포함 완전 수신/송신.
 *     fio_net_cmd_crc / fio_net_cmd_crc_pdu           : CRC16 필드 채우기.
 *     fio_net_send_simple_cmd                         : 페이로드 없는 명령 스택 버퍼 전송.
 *   전송 큐:
 *     struct sk_entry / enum SK_F_*                   : list 큐잉 엔트리 + 메모리 관리 플래그.
 *     fio_net_prep_cmd / fio_net_queue_cmd / handle_xmits / handle_sk_entry : lock-guarded 큐.
 *   opcode 디스패치:
 *     handle_command                                  : 수신 cmd → opcode 별 handle_*_cmd.
 *     handle_job_cmd / handle_jobline_cmd / handle_run_cmd / handle_probe_cmd /
 *     handle_send_eta_cmd / handle_update_job_cmd / handle_trigger_cmd / handle_load_file_cmd.
 *   연결 수락 루프:
 *     accept_loop / handle_connection / fio_server_add_conn_pid / fio_server_check_fork_items.
 *   결과 스트리밍:
 *     fio_server_text_output / fio_server_send_ts / fio_server_send_gs /
 *     fio_server_send_du / fio_server_send_add_job / fio_server_send_start /
 *     fio_send_iolog (+ __fio_append_iolog_gz, fio_append_gz_chunks, fio_append_text_log).
 *   인자 파싱/바인드:
 *     fio_server_parse_string / fio_server_parse_host / fio_handle_server_arg /
 *     fio_init_server_ip / fio_init_server_sock / fio_init_server_connection.
 *   데몬화/TLS:
 *     fio_start_server / check_existing_pidfile / write_pid / sig_int / set_sig_handlers /
 *     fio_server_create_sk_key / fio_server_destroy_sk_key / fio_server_got_signal.
 *   verify 상태 동기 요청:
 *     struct cmd_reply + fio_server_get_verify_state  (SENDFILE 세마포어 핸드쉐이크).
 *
 * === 실행 컨텍스트 / 재진입성 ===
 *
 *   - accept_loop : 서버 프로세스 메인 스레드. fork 부모.
 *   - handle_connection : fork 자식 프로세스의 메인 스레드.
 *   - handle_run_cmd 가 다시 fork 한 프로세스의 main thread == fio_backend().
 *   - 잡 스레드(backend 가 pthread_create 로 만든 워커)는 fio_server_text_output 이나
 *     fio_server_send_ts 등을 호출. sk_out 은 pthread TLS 로 해당 스레드에 바인딩되며,
 *     sk_out->lock/xmit 으로 리스트 경쟁과 소켓 출력 경쟁을 모두 차단한다.
 *   - sig_int 는 async-signal-safety 가 필요한 맥락 — unlink 정도로 제한.
 *   - fio_server_got_signal 은 메인 스레드에서 호출되며 exit_backend 만 건드림.
 */

/*
 * [한국어] === 표준 라이브러리 및 시스템 헤더 ===
 * 각 헤더가 공급하는 핵심 심볼을 명시한다. 소켓/프로세스/파일 조작 관련이 대부분이다.
 */
#include <stdio.h>       /* [한국어] FILE, fopen/fread/fclose/fprintf/snprintf — pidfile 기록/디버그 출력 */
#include <stdlib.h>      /* [한국어] malloc/calloc/realloc/free/atoi/_exit/exit — 동적 할당과 프로세스 종료 */
#include <string.h>      /* [한국어] memcpy/memset/strcpy/strdup/strcmp/strncmp/strerror — 버퍼·문자열 조작 */
#include <unistd.h>      /* [한국어] close/read/write/fork/getpid/gethostname/unlink/setsid/_exit — POSIX API */
#include <errno.h>       /* [한국어] errno, EAGAIN/EINTR/ECHILD/ESRCH/ENOMEM/ETIMEDOUT/EILSEQ/ENODEV — 에러 코드 */
#include <poll.h>        /* [한국어] poll(2), struct pollfd, POLLIN/POLLHUP/POLLERR — I/O 멀티플렉싱
                          *         accept_loop·handle_connection 의 비블로킹 대기에 사용. */
#include <sys/types.h>   /* [한국어] pid_t/socklen_t/mode_t/off_t — 다른 헤더의 타입 선행 정의 */
#include <sys/wait.h>    /* [한국어] waitpid(2), WNOHANG, WIFSIGNALED/WIFEXITED/WTERMSIG/WEXITSTATUS
                          *         fio_server_check_fork_item 의 자식 상태 비블로킹 회수. */
#include <sys/socket.h>  /* [한국어] socket/bind/listen/accept/connect/setsockopt/send/recv/writev/recvmsg/
                          *         SOL_SOCKET/SO_REUSEADDR/SO_REUSEPORT/MSG_WAITALL — BSD 소켓 API. */
#include <sys/stat.h>    /* [한국어] stat(2) + struct stat — check_existing_pidfile 의 파일 존재 확인. */
#include <sys/un.h>      /* [한국어] struct sockaddr_un, sun_family, sun_path — Unix 도메인 소켓 주소. */
#include <sys/uio.h>     /* [한국어] struct iovec, writev(2) — scatter-gather 전송. fio_sendv_data 의 핵심. */
#include <netinet/in.h>  /* [한국어] struct sockaddr_in/in6, INADDR_ANY, IPPROTO_TCP, htons, htonl. */
#include <arpa/inet.h>   /* [한국어] inet_ntop/inet_pton — 주소 문자열 ↔ 바이너리 변환. */
#include <netdb.h>       /* [한국어] getaddrinfo/freeaddrinfo/gai_strerror/struct addrinfo — DNS 해석 폴백. */
#include <syslog.h>      /* [한국어] openlog/syslog/closelog, LOG_NDELAY/LOG_NOWAIT/LOG_PID/LOG_USER —
                          *         데몬 모드에서 stderr 대체. */
#include <signal.h>      /* [한국어] struct sigaction, sigaction(2), SIGINT/SIGPIPE/SIGBREAK/SIGCONT, SA_RESTART. */
#ifdef CONFIG_ZLIB
#include <zlib.h>        /* [한국어] z_stream, deflateInit/deflate/deflateEnd, Z_OK/Z_BLOCK/Z_FINISH/Z_STREAM_END —
                          *         IOLOG 압축 전송. configure 에서 zlib 발견 시 활성화. */
#endif

/*
 * [한국어] === fio 내부 헤더 ===
 * 본 파일이 사용하는 fio 고유 심볼의 소스.
 */
#include "fio.h"           /* [한국어] thread_data/thread_stat/group_run_stats/io_log/io_sample/
                            *         dprint/log_info/log_err/FIO_OS/FIO_ARCH/cpu_to_le*/
                            *         fio_gettime 등 코어 심볼 공급 허브. */
#include "options.h"       /* [한국어] convert_thread_options_to_cpu/to_net, thread_options_pack_size —
                            *         ADD_JOB/UPDATE_JOB 직렬화에 사용. */
#include "server.h"        /* [한국어] struct fio_net_cmd, FIO_NET_CMD_* enum, cmd_*_pdu 구조체,
                            *         FIO_SERVER_VER, FIO_SERVER_MAX_FRAGMENT_PDU, sk_out 정의. */
#include "crc/crc16.h"     /* [한국어] fio_crc16() — 명령 헤더 및 PDU 무결성 검증용 CCITT CRC16. */
#include "lib/ieee754.h"   /* [한국어] fio_double_to_uint64/fio_uint64_to_double — 부동소수점의
                            *         아키텍처 독립 비트패턴 직렬화. mean/S/percentile/ss_* 등에 사용. */
#include "verify-state.h"  /* [한국어] struct verify_state_hdr/thread_io_list, verify_state_hdr()/
                            *         verify_state_gen_name() — SENDFILE 로 회수한 state 검증. */
#include "smalloc.h"       /* [한국어] smalloc/scalloc/sfree — SHM 기반 공유 메모리 할당기.
                            *         fork 된 잡 프로세스와 연결 처리 프로세스가 동시에 sk_out 에
                            *         접근 가능하도록 SK_F_COPY 경로에서 활용. */

/*
 * [한국어] === 전역 변수 (external linkage) ===
 * 다른 translation unit 에서도 참조 가능한 서버 상태.
 */
int fio_net_port = FIO_NET_PORT;
/* [한국어] 서버가 listen() 할 TCP 포트. 기본값 FIO_NET_PORT(8765, server.h 정의).
 * 설정자: fio_handle_server_arg() 가 파싱된 포트로 덮어씀. 클라이언트도 동일 변수를
 *         참조해 연결 포트 결정(init.c 의 --client 옵션 처리).
 * 읽는 자: fio_init_server_ip() 의 saddr_in.sin_port, fio_init_server_connection 로그.
 * 값 범위: 1~65535. 0 이면 fio_server_parse_string 이 기본값 복귀.
 * 동기화: 서버 기동 시 1회 초기화 후 읽기 전용 — 멀티스레드 접근 안전. */

bool exit_backend = false;
/* [한국어] 서버/클라이언트 공용 종료 플래그. 모든 poll/recv/send 루프가 매 반복마다 검사.
 * 설정자: fio_server_got_signal() (SIGINT/SIGTERM 등) 또는 handle_command() 가
 *         FIO_NET_CMD_EXIT 수신 시 true 로 세트.
 * 읽는 자: accept_loop / handle_connection / fio_sendv_data / fio_recv_data 등 루프 가드.
 * 값 범위: false=정상, true=즉시 탈출 예약.
 * 동기화: 단순 bool 로 멀티프로세스 간 공유되지 않음. 각 프로세스가 자체 시그널로 세팅 후
 *         자기 루프만 탈출. fork 자식은 부모의 값 복사본을 가지고 시작(COW). */

/*
 * [한국어] === 소켓 전송 큐 엔트리 플래그 (enum SK_F_*) ===
 * sk_entry::flags 비트필드로 사용되며 메모리 소유권과 전송 방식을 결정한다.
 * 배타적이지 않고 중첩 가능(예: SK_F_VEC|SK_F_INLINE|SK_F_COPY 조합이 빈번).
 */
enum {
	SK_F_FREE	= 1,
	/* [한국어] buf 가 malloc() 으로 할당됨 → finish_entry() 에서 free(buf).
	 * 설정자: fio_net_queue_cmd(..., SK_F_FREE) — send_eta, iolog 압축 출력 등 힙 버퍼.
	 * 읽는 자: finish_entry() 가 free() 선택.
	 * 값: SK_F_COPY 와 상호 배타적(둘 다 세트 시 FREE 가 우선 처리). */

	SK_F_COPY	= 2,
	/* [한국어] 호출자 버퍼를 smalloc 힙에 복사 → finish_entry() 에서 sfree(buf).
	 * 설정자: 스택 변수나 호출자 소유 버퍼를 안전히 큐잉할 때 사용. ADD_JOB/TS/GS/DU/JOB_OPT 등 대부분 경로.
	 * 읽는 자: fio_net_prep_cmd 가 smalloc + memcpy, finish_entry 가 sfree.
	 * 값: SK_F_FREE 와 상호 배타적. smalloc 힙은 fork 자식에게 공유됨. */

	SK_F_SIMPLE	= 4,
	/* [한국어] 페이로드 없는 명령(QUIT/SERVER_START 등). handle_sk_entry 가 fio_net_send_simple_cmd 호출.
	 * 설정자: fio_net_queue_quit / fio_server_send_start.
	 * 값 의미: 0 크기 페이로드 + cmd_crc 만 계산(pdu_crc 는 빈 버퍼 CRC). */

	SK_F_VEC	= 8,
	/* [한국어] iovec 기반 scatter-gather 전송(first + first->next 체인).
	 * 설정자: fio_send_iolog 가 헤더+데이터 엔트리들을 하나의 벡터로 묶을 때 세트.
	 * 읽는 자: handle_sk_entry → send_vec_entry → fio_send_cmd_ext_pdu(writev iov[2]).
	 * 후속 엔트리 사이에 FIO_NET_CMD_F_MORE 플래그가 자동 삽입된다. */

	SK_F_INLINE	= 16,
	/* [한국어] 큐에 넣지 않고 즉시 전송(fio_net_queue_entry 진입 시 바로 handle_sk_entry).
	 * 설정자: VTRIGGER 응답처럼 지연 없이 전송해야 하는 경우, 또는 fio_send_iolog 의 헤더.
	 * 값 의미: 세마포어 경쟁을 우회하므로 호출 측이 이미 sk_out TLS 를 들고 있어야 함. */
};

/*
 * [한국어] === struct sk_entry ===
 * sk_out->list 에 연결되어 전송 대기하는 개별 패킷 디스크립터.
 * 잡 스레드/연결 스레드 어느 쪽이든 fio_net_prep_cmd → fio_net_queue_entry 로 적재한 뒤,
 * handle_xmits() 가 일괄 소비한다. SK_F_VEC 체인은 first 하나가 first->next FIFO 를 이끌며
 * writev 로 묶어 전송되므로 중간에 다른 엔트리가 끼어들지 않는다.
 */
struct sk_entry {
	struct flist_head list;		/* link on sk_out->list */
	/* [한국어] sk_out->list 에 연결하기 위한 intrusive flist 노드.
	 * 설정자: fio_net_queue_entry → flist_add_tail(&entry->list, &sk_out->list).
	 * 읽는 자: handle_xmits 가 flist_splice_init 로 로컬 리스트에 옮긴 뒤 순회.
	 * 동기화: sk_out->lock 세마포어 보호. handle_xmits / fio_net_queue_entry 모두 락 경유. */

	int flags;		/* SK_F_* */
	/* [한국어] SK_F_FREE/COPY/SIMPLE/VEC/INLINE 의 OR 조합.
	 * 설정자: fio_net_prep_cmd(..., flags) 에서 세트. 이후 읽기 전용.
	 * 읽는 자: fio_net_queue_entry(INLINE 검사) + handle_sk_entry(VEC/SIMPLE 분기) + finish_entry(FREE/COPY). */

	int opcode;		/* Actual command fields */
	/* [한국어] FIO_NET_CMD_* 중 하나. handle_sk_entry 가 실제 전송 시점에 fio_init_net_cmd 로 주입.
	 * 설정자: fio_net_prep_cmd(opcode, ...).
	 * 값 범위: 1..FIO_NET_CMD_NR-1. */

	void *buf;
	/* [한국어] 전송할 페이로드 버퍼 포인터.
	 * 설정자: SK_F_COPY 경로 → smalloc 힙 복사본, 아니면 호출자 소유 버퍼를 그대로 보관.
	 * 읽는 자: fio_net_send_cmd / fio_send_cmd_ext_pdu 가 iov_base 로 사용.
	 * 동기화: 소유권은 flags 가 결정(SK_F_FREE/COPY 에 따라 해제 방법 달라짐). */

	off_t size;
	/* [한국어] buf 의 바이트 크기. SK_F_SIMPLE 이면 0 가능.
	 * 값 범위: 0..FIO_SERVER_MAX_CMD_MB*1024. 초과 시 fio_net_send_cmd 가 프래그먼트 분할. */

	uint64_t tag;
	/* [한국어] 요청-응답 매칭 태그. 클라이언트 측에서는 fio_net_cmd_reply* 포인터의 비트 캐스트.
	 * 설정자: alloc_reply() 가 새 reply 구조체 포인터를 (uintptr_t) 캐스트해 세트.
	 * 읽는 자: 수신 측이 cmd->tag 를 읽어 해당 reply 를 찾음.
	 * 값 범위: 0=무시, 그 외=유효 포인터 값. */

	struct flist_head next;	/* Other sk_entry's, if linked command */
	/* [한국어] SK_F_VEC 모드에서 후속 엔트리 FIFO 헤드. INIT_FLIST_HEAD 로 초기화.
	 * 설정자: fio_send_iolog 등이 flist_add_tail(&next_entry->list, &first->next).
	 * 읽는 자: send_vec_entry 가 while(!flist_empty(&first->next)) 순회하며 연속 writev.
	 * 동기화: first 가 sk_out->list 에 들어가기 전에만 조작 — 큐잉 이후로는 단일 소비자. */
};

/*
 * [한국어] === 정적 전역 변수 (서버 상태) ===
 * translation unit 내부에서만 참조. 서버 프로세스 기동 시 1회 세팅되며 대부분 읽기 전용.
 */
static char *fio_server_arg;
/* [한국어] --server=<arg> 로 전달된 원본 문자열(예: "sock:/tmp/fio.sock", "ip6:::1,9000").
 * 설정자: fio_server_set_arg() — init.c 의 옵션 처리에서 호출.
 * 읽는 자: fio_handle_server_arg() — parse_string 으로 분해.
 * 해제: fio_server() 종료 시 free. */

static char *bind_sock;
/* [한국어] Unix 도메인 소켓 경로. "sock:" 접두어 파싱 성공 시만 non-NULL.
 * 설정자: fio_server_parse_string 이 strdup 으로 할당.
 * 읽는 자: fio_init_server_sock(bind sun_path), sig_int(종료 시 unlink).
 * 해제: fio_server() 말미 및 sig_int() 에서 unlink. */

static struct sockaddr_in saddr_in;
/* [한국어] IPv4 바인드 주소. fio_handle_server_arg 가 sin_addr/sin_port 채움.
 * 설정자: fio_server_parse_host() 가 inet_pton/getaddrinfo 로 sin_addr, fio_handle_server_arg 가 htons(port).
 * 읽는 자: fio_init_server_ip() 가 bind() 인자로 사용, accept_loop 에서 inet_ntop 로그. */

static struct sockaddr_in6 saddr_in6;
/* [한국어] IPv6 바인드 주소 (use_ipv6=1 일 때만). 구조와 라이프사이클은 saddr_in 과 동일. */

static int use_ipv6;
/* [한국어] "ip6:" 접두 또는 getaddrinfo 가 AF_INET6 반환 시 1.
 * 설정자: fio_server_parse_string.
 * 읽는 자: fio_init_server_ip / accept_loop / get_my_addr_str — v4/v6 소켓 호출 분기. */

#ifdef CONFIG_ZLIB
static unsigned int has_zlib = 1;
/* [한국어] configure 시 zlib 발견 → 컴파일 타임 상수. PROBE 응답 생성에 사용.
 * 읽는 자: handle_probe_cmd — 클라이언트도 zlib 지원 시 use_zlib=1. */
#else
static unsigned int has_zlib = 0;
/* [한국어] zlib 미지원 빌드 — IOLOG 는 항상 평문 전송. */
#endif
static unsigned int use_zlib;
/* [한국어] PROBE 핸드쉐이크 결과 현재 연결에서 IOLOG 압축 사용 여부.
 * 설정자: handle_probe_cmd 가 서버/클라 양측 has_zlib 확인 후 세트.
 * 읽는 자: fio_send_iolog — STORE_COMPRESSED/XMIT_COMPRESSED/평문 3분기. */

static char me[128];
/* [한국어] 클라이언트가 PROBE 요청에 실어보낸 "서버를 인식하는 이름"(자주 호스트명/별명).
 * 설정자: handle_probe_cmd 가 strcpy(me, pdu->server).
 * 읽는 자: fio_server_get_verify_state — verify state 파일명 조합
 *          (verify_state_gen_name(buf, buf_sz, name, me, threadnumber)). */

static pthread_key_t sk_out_key;
/* [한국어] pthread TLS(Thread-Local Storage) 키. 현재 스레드가 소유한 struct sk_out* 저장.
 * 생성: fio_server_create_sk_key() — main() 직후 1회.
 * 설정자: sk_out_assign(sk_out) → pthread_setspecific.
 * 읽는 자: fio_net_queue_entry / fio_server_text_output / fio_server_send_start 등
 *          "현재 스레드가 어느 소켓에 속하는지" 결정.
 * 삭제: fio_server_destroy_sk_key() — 프로세스 종료 직전. */

#ifdef WIN32
static char *fio_server_pipe_name  = NULL;
/* [한국어] Windows 내부에서 부모→자식 프로세스로 WSAPROTOCOL_INFO 를 전달하기 위한 명명된 파이프 이름.
 * 설정자: fio_server_internal_set() — 부모가 자식 실행 인자로 전달.
 * 읽는 자: handle_connection_process() — CreateFile(pipe_name) 으로 부모와 연결. */

static HANDLE hjob = INVALID_HANDLE_VALUE;
/* [한국어] Windows Job Object — 부모 종료 시 자식 프로세스들도 일괄 종료시키기 위한 컨테이너.
 * 설정자: fio_server() 가 windows_create_job() 호출.
 * 읽는 자: accept_loop 가 windows_handle_connection 에 인자로 전달. */

/* [한국어] Windows 포크 아이템 요소 — 스레드 또는 프로세스 핸들.
 * 본 Windows 포트는 POSIX fork 대신 pthread 또는 CreateProcess 를 사용하므로 둘을 구별해야 한다. */
struct ffi_element {
	union {
		pthread_t thread;
		/* [한국어] 잡 실행 pthread 핸들 (handle_run_cmd 가 pthread_create 생성).
		 * is_thread=true 일 때 유효. pthread_kill(thread, 0) 으로 생존 확인, pthread_join 으로 회수. */
		HANDLE hProcess;
		/* [한국어] 연결 처리 자식 프로세스 핸들 (windows_handle_connection 가 CreateProcess 반환).
		 * is_thread=false 일 때 유효. GetExitCodeProcess 로 종료 확인. */
	};
	bool is_thread;
	/* [한국어] 유니온 판별자. true=thread 멤버 사용, false=hProcess 멤버 사용. */
};
#endif

/*
 * [한국어] === struct fio_fork_item ===
 * 서버가 만든 자식(연결 처리용 또는 잡 실행용)의 생존·종료 상태를 추적하는 엔트리.
 * conn_list / job_list 두 리스트 각각에 연결된다. fio_server_check_fork_items 가 비블로킹으로
 * 회수하며, exited=true 가 되면 fio_server_fork_item_done 이 리스트에서 제거·해제한다.
 */
struct fio_fork_item {
	struct flist_head list;
	/* [한국어] conn_list 또는 job_list 의 intrusive 노드.
	 * 설정자: fio_server_add_fork_item → flist_add_tail.
	 * 읽는 자: fio_server_check_fork_items 의 flist_for_each_safe. */

	int exitval;
	/* [한국어] 정상 종료 시 exit status(0..255).
	 * 설정자: POSIX — waitpid+WEXITSTATUS; Windows — GetExitCodeProcess / pthread_join 반환값.
	 * 읽는 자: fio_server_fork_item_done → fio_net_queue_stop(exitval, signal). */

	int signal;
	/* [한국어] 시그널에 의해 종료됐을 때의 시그널 번호(POSIX 전용, Windows 는 항상 0).
	 * 설정자: waitpid 가 WIFSIGNALED 이면 WTERMSIG.
	 * 의미: 클라이언트가 "서버 측 잡이 어떤 시그널로 죽었는지" 표시하는 STOP 응답의 필드. */

	int exited;
	/* [한국어] 이번 라운드에서 종료를 확인했는가. 1=리스트에서 제거하고 해제 예정.
	 * 설정자: fio_server_check_fork_item 이 waitpid/GetExitCodeProcess 성공 시 세트.
	 * 읽는 자: fio_server_check_fork_items — if (ffi->exited) fio_server_fork_item_done. */

#ifdef WIN32
	struct ffi_element element;
	/* [한국어] Windows: thread 또는 hProcess 핸들(유니온). is_thread 로 판별. */
#else
	pid_t pid;
	/* [한국어] POSIX: 자식 프로세스 PID. waitpid(pid, ...) 대상. */
#endif
};

/*
 * [한국어] === struct cmd_reply ===
 * 동기 요청(SENDFILE 같이 응답을 기다리는 명령)에 사용하는 핸드쉐이크 구조.
 * 요청 측이 lock 을 LOCKED 로 초기화 → tag 에 자기 주소 인코딩 → 큐잉 → fio_sem_down_timeout 대기.
 * 응답 수신 측(handle_command 의 SENDFILE 분기)은 data/size/error 채우고 lock 을 up 하여 깨운다.
 */
struct cmd_reply {
	struct fio_sem lock;
	/* [한국어] 요청자-응답자 바인딩 세마포어.
	 * 초기값: FIO_SEM_LOCKED (fio_server_get_verify_state 가 __fio_sem_init 으로 세트).
	 * 설정자: 응답 도착 시 handle_command 가 fio_sem_up(&rep->lock) 호출.
	 * 읽는 자: 요청자 측 fio_sem_down_timeout(&rep->lock, 10000) — 10초 타임아웃.
	 * 동기화: 단일 생산자(응답 수신) / 단일 소비자(원 요청 스레드) — race 없음. */

	void *data;
	/* [한국어] 수신된 응답 페이로드 버퍼(smalloc 할당).
	 * 설정자: handle_command 가 smalloc(size) + memcpy.
	 * 읽는 자: fio_server_get_verify_state 가 verify_state_hdr 파싱. */

	size_t size;
	/* [한국어] data 버퍼 바이트 수. SENDFILE 응답의 cmd_sendfile_reply::size 값. */

	int error;
	/* [한국어] 응답에 포함된 에러 코드(errno 규격).
	 * 0=성공, 그 외=실패 사유(파일 없음/권한 에러 등).
	 * 설정자: handle_command 가 cmd_sendfile_reply::error 복사. */
};

/*
 * [한국어] === opcode → 문자열 매핑 테이블 ===
 * 디버그/로그 출력에서 숫자 opcode 대신 사람이 읽을 수 있는 이름 표기.
 * FIO_NET_CMD_NR(=23) 크기로 0 번 슬롯은 빈 문자열, 1..22 는 enum 순서와 일치.
 * fio_server_op() 가 range check 후 반환.
 */
static const char *fio_server_ops[FIO_NET_CMD_NR] = {
	"",
	"QUIT",
	"EXIT",
	"JOB",
	"JOBLINE",
	"TEXT",
	"TS",
	"GS",
	"SEND_ETA",
	"ETA",
	"PROBE",
	"START",
	"STOP",
	"DISK_UTIL",
	"SERVER_START",
	"ADD_JOB",
	"RUN",
	"IOLOG",
	"UPDATE_JOB",
	"LOAD_FILE",
	"VTRIGGER",
	"SENDFILE",
	"JOB_OPT",
};

/*
 * [한국어]
 * sk_lock - sk_out 의 리스트 보호 세마포어 획득 (뮤텍스 역할)
 *
 * @sk_out: 보호 대상 연결 상태 컨텍스트.
 * @return: 없음. 실패(데드락) 시 fio_sem_down 내부에서 abort.
 *
 * 왜 필요한가: sk_out->list 는 잡 스레드(결과 큐잉) 와 handle_xmits(flush)
 * 양쪽에서 접근하므로 리스트 조작을 직렬화해야 한다. FIO_SEM 은 내부적으로
 * pthread_mutex 와 동등 역할로 쓰인다.
 * 호출 체인: fio_net_queue_entry / handle_xmits / sk_out_assign / __sk_out_drop
 *           → sk_lock → fio_sem_down(&sk_out->lock).
 * 컨텍스트: 어떤 스레드/프로세스에서든 호출 가능 — sk_out 이 멀티 owner 이기 때문.
 */
static void sk_lock(struct sk_out *sk_out)
{
	fio_sem_down(&sk_out->lock);  /* [한국어] FIO_SEM_UNLOCKED → LOCKED; 이미 LOCKED 면 대기 */
}

/*
 * [한국어]
 * sk_unlock - sk_lock() 으로 획득한 세마포어를 해제
 *
 * @sk_out: 락 소유 컨텍스트.
 * @return: 없음.
 *
 * 호출 체인: 모든 sk_lock() 호출 사이트의 페어. 균형 맞지 않으면 다른 스레드가
 *           영원히 대기.
 */
static void sk_unlock(struct sk_out *sk_out)
{
	fio_sem_up(&sk_out->lock);  /* [한국어] LOCKED → UNLOCKED; 대기 중 스레드 1개 깨움 */
}

/*
 * [한국어]
 * sk_out_assign - 현재 스레드(잡/연결 처리)에 sk_out 을 바인딩하고 참조 카운트++
 *
 * @sk_out: 소유권을 얻을 소켓 상태 구조체 (NULL 가능 — 무시).
 * @return: 없음.
 *
 * 왜 필요한가: fio_server_text_output / fio_server_send_ts 등의 결과 스트리밍 API 는
 * pthread_getspecific(sk_out_key) 로 "현재 스레드가 어느 소켓에 속하는지" 찾는다.
 * 잡 스레드가 생성될 때 이 함수를 호출해 TLS 에 등록해야 한다.
 * 호출 체인: handle_run_cmd(fork 부모+자식) / handle_connection / fio_backend_thread
 *           → sk_out_assign → pthread_setspecific.
 * 컨텍스트: 새 잡/연결 스레드의 초기화 시점 — 부모와 자식이 각각 카운트를
 *           1 씩 증가시켜 race-free 해제를 보장한다.
 */
void sk_out_assign(struct sk_out *sk_out)
{
	if (!sk_out)
		return;  /* [한국어] sk_out 이 없는 모드(로컬 실행 등) 에서는 no-op */

	sk_lock(sk_out);                        /* [한국어] refs 원자적 증가 보호 */
	sk_out->refs++;                         /* [한국어] 참조 카운트 +1 */
	sk_unlock(sk_out);
	pthread_setspecific(sk_out_key, sk_out); /* [한국어] 현재 스레드 TLS 에 저장 — 이후 queue/send 함수가 조회 */
}

/*
 * [한국어]
 * sk_out_free - sk_out 내부 세마포어 3개를 해제하고 공유 메모리 회수
 *
 * @sk_out: 참조 카운트가 0 이 된 sk_out.
 * @return: 없음.
 *
 * 호출 체인: __sk_out_drop 이 refs==0 시점에 호출.
 * 컨텍스트: "마지막" 참조자만 호출 — 다른 스레드가 sk_out 을 만지지 않음이 보장됨.
 */
static void sk_out_free(struct sk_out *sk_out)
{
	__fio_sem_remove(&sk_out->lock);  /* [한국어] 리스트 보호 세마포어 파괴 */
	__fio_sem_remove(&sk_out->wait);  /* [한국어] 작업 대기 세마포어 파괴 */
	__fio_sem_remove(&sk_out->xmit);  /* [한국어] 전송 직렬화 세마포어 파괴 */
	sfree(sk_out);                    /* [한국어] smalloc 공유메모리 힙으로 반환 */
}

/*
 * [한국어]
 * __sk_out_drop - sk_out 참조 카운트 1 감소; 0 이 되면 해제
 *
 * @sk_out: 참조 해제 대상 (NULL 허용).
 * @return: 0 = 해제 완료(마지막 참조였음), 1 = 아직 남은 참조 존재 또는 NULL.
 *
 * 왜 필요한가: sk_out 은 부모-자식 프로세스 간 공유되므로 양측이 완료 시점을
 * 알 수 없다 → 참조 카운트로 안전 해제.
 * 호출 체인: handle_connection(_exit 직전) / sk_out_drop(잡 스레드 종료) → __sk_out_drop.
 * 에러 경로: assert(sk_out->refs != 0) — 언더플로 감지 시 abort.
 */
static int __sk_out_drop(struct sk_out *sk_out)
{
	if (sk_out) {
		int refs;

		sk_lock(sk_out);                 /* [한국어] refs 조작 직렬화 */
		assert(sk_out->refs != 0);       /* [한국어] 음수 방지 — drop 불균형 감지 */
		refs = --sk_out->refs;           /* [한국어] 참조 카운트 -1, 지역 복사 */
		sk_unlock(sk_out);

		if (!refs) {                     /* [한국어] 마지막 참조였으면 */
			sk_out_free(sk_out);     /* [한국어] 세마포어+힙 회수 */
			pthread_setspecific(sk_out_key, NULL);  /* [한국어] TLS 도 클리어 */
			return 0;
		}
	}

	return 1;  /* [한국어] 해제 안 됨(참조 남음) 또는 NULL 입력 */
}

/*
 * [한국어]
 * sk_out_drop - 현재 스레드의 sk_out 참조 해제 (TLS 에서 조회 → __sk_out_drop)
 *
 * @return: 없음.
 *
 * 호출 체인: fio_backend_thread / 잡 스레드 종료 직전 → sk_out_drop → __sk_out_drop.
 */
void sk_out_drop(void)
{
	struct sk_out *sk_out;

	sk_out = pthread_getspecific(sk_out_key);  /* [한국어] 현재 스레드 TLS 에서 조회 */
	__sk_out_drop(sk_out);
}

/*
 * [한국어]
 * __fio_init_net_cmd - fio_net_cmd 헤더를 0 으로 초기화하고 필수 필드를 LE 로 채움
 *
 * @cmd: 채울 헤더 구조체(호출자 제공 버퍼).
 * @opcode: FIO_NET_CMD_* 중 하나 (QUIT/EXIT/JOB/...).
 * @pdu_len: 페이로드 바이트 수 (payload 필드 제외).
 * @tag: 요청-응답 매칭 태그 (클라이언트 reply 포인터의 비트 캐스트 또는 0).
 * @return: 없음. cmd 의 version/opcode/tag/pdu_len 채움; flags/crc 는 후속 단계.
 *
 * 왜 LE 로 변환하는가: fio 서버/클라이언트는 이기종(x86 vs ARM vs PPC) 간에도 동작해야
 * 하므로 와이어 포맷을 리틀엔디안으로 고정하고 수신 측이 le*_to_cpu 로 역변환한다.
 * 호출 체인: fio_init_net_cmd / fio_send_cmd_ext_pdu 가 호출 → CRC 계산 전에 사용.
 */
static void __fio_init_net_cmd(struct fio_net_cmd *cmd, uint16_t opcode,
			       uint32_t pdu_len, uint64_t tag)
{
	memset(cmd, 0, sizeof(*cmd));  /* [한국어] 모든 필드 0 — flags/cmd_crc16/pdu_crc16 기본값 */

	cmd->version	= __cpu_to_le16(FIO_SERVER_VER);  /* [한국어] 프로토콜 버전 고정(수신 측 mismatch 거부) */
	cmd->opcode	= cpu_to_le16(opcode);             /* [한국어] FIO_NET_CMD_* enum을 LE 16비트로 직렬화 */
	cmd->tag	= cpu_to_le64(tag);                /* [한국어] 요청자 reply 포인터 또는 0 */
	cmd->pdu_len	= cpu_to_le32(pdu_len);            /* [한국어] 뒤따르는 페이로드 길이 */
}

/*
 * [한국어]
 * fio_init_net_cmd - 헤더 초기화 + 페이로드를 cmd->payload 뒤쪽에 복사
 *
 * @cmd: 헤더 + 페이로드 공간을 포함한 버퍼(sizeof(fio_net_cmd) + pdu_len 바이트 이상).
 * @opcode: FIO_NET_CMD_* opcode.
 * @pdu: 복사할 페이로드 소스 (NULL 가능 — SIMPLE 명령).
 * @pdu_len: 페이로드 바이트 수.
 * @tag: 요청 태그.
 * @return: 없음.
 *
 * 호출 체인: fio_net_send_cmd 가 매 프래그먼트마다 호출.
 * 주의: 호출자는 cmd 가 충분히 크도록 사전에 malloc 해야 한다(flexible array 성격).
 */
static void fio_init_net_cmd(struct fio_net_cmd *cmd, uint16_t opcode,
			     const void *pdu, uint32_t pdu_len, uint64_t tag)
{
	__fio_init_net_cmd(cmd, opcode, pdu_len, tag);  /* [한국어] 헤더 필드 세팅 */

	if (pdu)
		memcpy(&cmd->payload, pdu, pdu_len);  /* [한국어] payload[] 배열에 소스 복사 */
}

/*
 * [한국어]
 * fio_server_op - opcode 숫자를 사람이 읽는 문자열로 변환 (디버그/로그)
 *
 * @op: FIO_NET_CMD_* 값 (0..FIO_NET_CMD_NR-1 유효).
 * @return: 정적 문자열 포인터 (범위 밖이면 "UNKNOWN/<n>" 형태 정적 버퍼).
 *
 * 주의: "UNKNOWN" 경로는 정적 버퍼를 재사용하므로 멀티스레드 안전하지 않으나,
 *       디버그 경로라 실용상 문제 없음.
 */
const char *fio_server_op(unsigned int op)
{
	static char buf[32];  /* [한국어] "UNKNOWN/%d" 포매팅용 — 재진입 안전 아님 */

	if (op < FIO_NET_CMD_NR)
		return fio_server_ops[op];  /* [한국어] 알려진 opcode — 정적 문자열 반환 */

	sprintf(buf, "UNKNOWN/%d", op);   /* [한국어] 범위 밖 — 숫자 표시 */
	return buf;
}

/*
 * [한국어]
 * iov_total_len - iovec 배열의 총 바이트 수 합산
 *
 * @iov: iovec 배열 시작.
 * @count: 엔트리 수.
 * @return: 전체 합계(바이트).
 *
 * 호출 체인: fio_sendv_data 가 전송 성공 시 차감 대상 총량 계산에 사용.
 */
static ssize_t iov_total_len(const struct iovec *iov, int count)
{
	ssize_t ret = 0;

	while (count--) {            /* [한국어] count 개 엔트리 순회 */
		ret += iov->iov_len; /* [한국어] 각 iovec 의 바이트 누적 */
		iov++;
	}

	return ret;
}

/*
 * [한국어]
 * fio_sendv_data - scatter/gather 로 소켓 전송, 부분 전송 시 iov 전진
 *
 * @sk: 전송할 소켓 fd (TCP/Unix/IPv6 공통 — 스트림 소켓).
 * @iov: iovec 배열 (내부에서 전진시키므로 호출자 측 변경 주의).
 * @count: iovec 엔트리 수.
 * @return: 0 = 전체 전송 완료, 1 = 실패(연결 종료/errno).
 *
 * 왜 writev 인가: fio_net_cmd 헤더 24B + PDU 를 한 syscall 로 묶어 TCP_NODELAY 미사용
 * 환경에서 작은 패킷 분할을 방지한다. EAGAIN/EINTR 은 재시도(SA_RESTART 만으로 부족).
 * 호출 체인: fio_send_data / fio_send_cmd_ext_pdu → fio_sendv_data →
 *           writev(2) → 커널 TCP 송신 큐.
 * 에러 경로: writev 반환 0 = peer close(EOF), 음수 = errno (EAGAIN/EINTR 제외 시 즉시 탈출).
 * exit_backend: 시그널로 false→true 되면 루프 재진입 차단 (연결 강제 종료).
 */
static int fio_sendv_data(int sk, struct iovec *iov, int count)
{
	ssize_t total_len = iov_total_len(iov, count);  /* [한국어] 전체 전송해야 할 총 바이트 */
	ssize_t ret;

	do {
		ret = writev(sk, iov, count);  /* [한국어] scatter-gather 쓰기 — 부분/전체/실패 3-way */
		if (ret > 0) {
			total_len -= ret;  /* [한국어] 성공 바이트 만큼 잔여 감소 */
			if (!total_len)
				break;     /* [한국어] 모두 전송 완료 */

			/* [한국어] 부분 전송: iov 앞쪽 엔트리 소진 + 중간 엔트리는 오프셋 전진 */
			while (ret) {
				if (ret >= iov->iov_len) {
					ret -= iov->iov_len; /* [한국어] 이 엔트리 완전히 송신됨 */
					iov++;               /* [한국어] 다음 엔트리로 이동 */
					continue;
				}
				iov->iov_base += ret;   /* [한국어] 중간 엔트리 — 버퍼 내부 전진 */
				iov->iov_len -= ret;    /* [한국어] 남은 길이 감소 */
				ret = 0;                /* [한국어] 정산 종료 */
			}
		} else if (!ret)
			break;  /* [한국어] writev 0 = peer 가 연결 닫음(FIN) */
		else if (errno == EAGAIN || errno == EINTR)
			continue;  /* [한국어] 비치명 에러 — 재시도 */
		else
			break;     /* [한국어] 치명 에러 (ECONNRESET/EPIPE 등) */
	} while (!exit_backend);  /* [한국어] 시그널로 종료 요청 시 루프 탈출 */

	if (!total_len)
		return 0;  /* [한국어] 전체 완료 */

	return 1;  /* [한국어] 미완료 — 호출자가 연결 종료 또는 실패 처리 */
}

/*
 * [한국어]
 * fio_send_data - 단일 버퍼 전송 (내부적으로 iovec 1개로 fio_sendv_data 호출)
 *
 * @sk: 소켓 fd.
 * @p: 전송 버퍼.
 * @len: 바이트 수 (MAX_FRAGMENT_PDU + 헤더 이하여야 함).
 * @return: 0 성공, 1 실패.
 *
 * assert: 단일 프래그먼트 크기 이내를 보장 — 호출자는 이미 분할 처리를 했어야 함.
 */
static int fio_send_data(int sk, const void *p, unsigned int len)
{
	struct iovec iov = { .iov_base = (void *) p, .iov_len = len };

	assert(len <= sizeof(struct fio_net_cmd) + FIO_SERVER_MAX_FRAGMENT_PDU);
	/* [한국어] 단일 프래그먼트 상한 초과 방지 — 상위에서 분할 책임. */

	return fio_sendv_data(sk, &iov, 1);
}

/*
 * [한국어]
 * fio_server_poll_fd - 단일 fd 를 timeout ms 동안 이벤트 대기
 *
 * @fd: 감시할 파일 디스크립터.
 * @events: 감시 이벤트(POLLIN/POLLOUT/POLLERR 등의 OR).
 * @timeout: 밀리초 단위 대기 시간. -1 무한, 0 논블록 폴링.
 * @return: true = 요청한 이벤트 발생, false = 타임아웃/에러/EINTR.
 *
 * 호출 체인: 클라이언트 측 커넥션 상태 조회용 유틸 (client.c 에서 주로 사용).
 * 에러 경로: EINTR = 시그널 — false 반환으로 재시도는 호출자 몫.
 */
bool fio_server_poll_fd(int fd, short events, int timeout)
{
	struct pollfd pfd = {
		.fd	= fd,
		.events	= events,
	};
	int ret;

	ret = poll(&pfd, 1, timeout);  /* [한국어] nfds=1 단일 fd 폴. timeout=ms, -1 은 무한 */
	if (ret < 0) {
		if (errno == EINTR)
			return false;  /* [한국어] 시그널로 인한 조기 반환 — 재시도는 호출자 몫 */
		log_err("fio: poll: %s\n", strerror(errno));
		return false;
	} else if (!ret) {
		return false;  /* [한국어] 타임아웃 — 이벤트 없음 */
	}
	if (pfd.revents & events)
		return true;   /* [한국어] 요청한 비트가 revents 에 들어옴 — 정상 이벤트 */
	return false;          /* [한국어] POLLERR/POLLHUP 만 발생 — 요청한 이벤트는 아님 */
}

/*
 * [한국어]
 * fio_recv_data - 소켓에서 len 바이트 수신
 *
 * @sk: 스트림 소켓 fd.
 * @buf: 수신 버퍼(호출자 소유).
 * @len: 수신할 바이트 수.
 * @wait: true 면 MSG_WAITALL 로 정확히 len 바이트 대기(블로킹);
 *        false 면 OS_MSG_DONTWAIT 로 현재 가능한 만큼만 수신 후 재시도.
 * @return: 0 성공, -1 실패(연결 종료 또는 에러).
 *
 * MSG_WAITALL 주의: 시그널 EINTR 가 오면 부분 수신 상태로 반환될 수 있어
 *                 내부에서 EINTR/EAGAIN 을 재시도한다.
 * 호출 체인: fio_net_recv_cmd → fio_recv_data (헤더 1회 + PDU 1회) → recv(2).
 * 에러 경로: recv 0 = peer close (EOF). 단, 완전 수신 성공 직후 len=0 → 정상 return 0.
 */
static int fio_recv_data(int sk, void *buf, unsigned int len, bool wait)
{
	int flags;
	char *p = buf;

	if (wait)
		flags = MSG_WAITALL;  /* [한국어] 전체 len 도달까지 블로킹 — 짧은 수신 방지 */
	else
		flags = OS_MSG_DONTWAIT;  /* [한국어] 비블로킹 — 현재 가능한 만큼만 */

	do {
		int ret = recv(sk, p, len, flags);

		if (ret > 0) {
			len -= ret;    /* [한국어] 수신 바이트 만큼 남은 길이 감소 */
			if (!len)
				break; /* [한국어] 전체 수신 완료 */
			p += ret;      /* [한국어] 버퍼 포인터 전진 */
			continue;
		} else if (!ret)
			break;  /* [한국어] recv 0 = peer EOF */
		else if (errno == EAGAIN || errno == EINTR) {
			if (wait)
				continue;  /* [한국어] wait 모드 — 재시도 */
			break;             /* [한국어] 비동기 모드 — 포기 */
		} else
			break;  /* [한국어] 치명적 에러 */
	} while (!exit_backend);

	if (!len)
		return 0;  /* [한국어] 완전 수신 성공 */

	return -1;  /* [한국어] 부분/실패 — 호출자가 에러 처리 */
}

/*
 * [한국어]
 * verify_convert_cmd - 수신한 fio_net_cmd 헤더의 CRC16 를 검증하고 LE→CPU 변환
 *
 * @cmd: 수신한 헤더 버퍼(내용이 in-place 로 변환됨).
 * @return: 0 성공, 1 실패(CRC 불일치/버전 mismatch/PDU 크기 초과).
 *
 * CRC 범위: FIO_NET_CMD_CRC_SZ = offsetof(pdu_crc16) + sizeof(pdu_crc16) 이전 영역.
 *           즉 payload 제외한 헤더 전체. pdu_crc16/cmd_crc16 필드는 CRC 계산 시 0 취급.
 * 에러 경로:
 *   - CRC mismatch → log_err + fprintf(f_err) → 호출자(fio_net_recv_cmd) 가 연결 종료.
 *   - version != FIO_SERVER_VER → 동일 처리 + "client/server version mismatch".
 *   - pdu_len > FIO_SERVER_MAX_FRAGMENT_PDU → 단일 조각 상한 초과 거부 (재조립 필요).
 * 호출 체인: fio_net_recv_cmd → verify_convert_cmd → fio_crc16 (CCITT 다항식).
 */
static int verify_convert_cmd(struct fio_net_cmd *cmd)
{
	uint16_t crc;

	cmd->cmd_crc16 = le16_to_cpu(cmd->cmd_crc16);  /* [한국어] CRC 필드 먼저 호스트 바이트 오더로 복원 */
	cmd->pdu_crc16 = le16_to_cpu(cmd->pdu_crc16);  /* [한국어] 페이로드 CRC 비교용 */

	crc = fio_crc16(cmd, FIO_NET_CMD_CRC_SZ);  /* [한국어] 헤더만 CRC 재계산 — cmd_crc16 필드는 직전에 복원된 상태(LE 상 0이어도 보관값 기준 검증 대상 외) */
	if (crc != cmd->cmd_crc16) {
		/* [한국어] 헤더 CRC 불일치 — 회선 손상 또는 버전 mismatch 가능성 */
		log_err("fio: server bad crc on command (got %x, wanted %x)\n",
				cmd->cmd_crc16, crc);
		fprintf(f_err, "fio: server bad crc on command (got %x, wanted %x)\n",
				cmd->cmd_crc16, crc);
		return 1;
	}

	/* [한국어] 나머지 필드도 일괄 복원 — CRC 검증 통과했으므로 내용 신뢰 가능 */
	cmd->version	= le16_to_cpu(cmd->version);
	cmd->opcode	= le16_to_cpu(cmd->opcode);
	cmd->flags	= le32_to_cpu(cmd->flags);
	cmd->tag	= le64_to_cpu(cmd->tag);
	cmd->pdu_len	= le32_to_cpu(cmd->pdu_len);

	switch (cmd->version) {
	case FIO_SERVER_VER:
		break;  /* [한국어] 현재 버전 — 정상 */
	default:
		/* [한국어] 버전 불일치 — 클라/서버 fio 버전 호환 안 됨 */
		log_err("fio: bad server cmd version %d\n", cmd->version);
		fprintf(f_err, "fio: client/server version mismatch (%d != %d)\n",
				cmd->version, FIO_SERVER_VER);
		return 1;
	}

	if (cmd->pdu_len > FIO_SERVER_MAX_FRAGMENT_PDU) {
		/* [한국어] 단일 프래그먼트 상한 초과 — 악의적 또는 프로토콜 버그 */
		log_err("fio: command payload too large: %u\n", cmd->pdu_len);
		return 1;
	}

	return 0;  /* [한국어] 검증 통과 */
}

/*
 * Read (and defragment, if necessary) incoming commands
 */
/*
 * [한국어]
 * fio_net_recv_cmd - 소켓에서 1개의 완전한 fio_net_cmd 를 수신(필요 시 재조립)
 *
 * @sk: 수신 소켓 fd.
 * @wait: true 면 MSG_WAITALL 블로킹 수신.
 * @return: 성공 시 malloc 한 fio_net_cmd* (호출자가 free), 실패 시 NULL.
 *
 * 프래그먼트 처리: 첫 헤더 수신 후 FIO_NET_CMD_F_MORE 가 세트이면 동일 opcode 의
 * 조각이 계속 이어진다. 각 조각마다 (a) 헤더 수신+검증 (b) 버퍼 realloc (c) PDU 수신
 * (d) PDU CRC 검증 → pdu_len 누적. 마지막 조각(F_MORE 없음)에서 루프 탈출.
 *
 * NUL 종결: opcode 가 TEXT/JOB 이면 호출자가 strlen/문자열 처리하기 좋도록
 * malloc 시 +1 하여 끝에 '\0' 을 기록.
 *
 * 에러 경로:
 *   - fio_recv_data 실패 → free(cmdret) → NULL 반환.
 *   - CRC mismatch → 동일.
 *   - 조각 opcode 불일치(fragment opcode mismatch) → 동일.
 *   - cmd_size > FIO_SERVER_MAX_CMD_MB → 동일.
 *
 * 호출 체인: handle_connection(서버) / handle_client_fd(클라이언트) → fio_net_recv_cmd
 *           → fio_recv_data → recv(2).
 * 컨텍스트: 서버는 fork 자식 프로세스의 main thread, 클라이언트는 메인 스레드.
 */
struct fio_net_cmd *fio_net_recv_cmd(int sk, bool wait)
{
	struct fio_net_cmd cmd, *tmp, *cmdret = NULL;
	size_t cmd_size = 0, pdu_offset = 0;
	uint16_t crc;
	int ret, first = 1;
	void *pdu = NULL;

	do {
		ret = fio_recv_data(sk, &cmd, sizeof(cmd), wait);
		if (ret)
			break;

		/* We have a command, verify it and swap if need be */
		ret = verify_convert_cmd(&cmd);
		if (ret)
			break;

		if (first) {
			/* if this is text, add room for \0 at the end */
			cmd_size = sizeof(cmd) + cmd.pdu_len + 1;
			assert(!cmdret);
		} else
			cmd_size += cmd.pdu_len;

		if (cmd_size / 1024 > FIO_SERVER_MAX_CMD_MB * 1024) {
			log_err("fio: cmd+pdu too large (%llu)\n", (unsigned long long) cmd_size);
			ret = 1;
			break;
		}

		tmp = realloc(cmdret, cmd_size);
		if (!tmp) {
			log_err("fio: server failed allocating cmd\n");
			ret = 1;
			break;
		}
		cmdret = tmp;

		if (first)
			memcpy(cmdret, &cmd, sizeof(cmd));
		else if (cmdret->opcode != cmd.opcode) {
			log_err("fio: fragment opcode mismatch (%d != %d)\n",
					cmdret->opcode, cmd.opcode);
			ret = 1;
			break;
		}

		if (!cmd.pdu_len)
			break;

		/* There's payload, get it */
		pdu = (char *) cmdret->payload + pdu_offset;
		ret = fio_recv_data(sk, pdu, cmd.pdu_len, wait);
		if (ret)
			break;

		/* Verify payload crc */
		crc = fio_crc16(pdu, cmd.pdu_len);
		if (crc != cmd.pdu_crc16) {
			log_err("fio: server bad crc on payload ");
			log_err("(got %x, wanted %x)\n", cmd.pdu_crc16, crc);
			ret = 1;
			break;
		}

		pdu_offset += cmd.pdu_len;
		if (!first)
			cmdret->pdu_len += cmd.pdu_len;
		first = 0;
	} while (cmd.flags & FIO_NET_CMD_F_MORE);

	if (ret) {
		free(cmdret);
		cmdret = NULL;
	} else if (cmdret) {
		/* zero-terminate text input */
		if (cmdret->pdu_len) {
			if (cmdret->opcode == FIO_NET_CMD_TEXT) {
				struct cmd_text_pdu *__pdu = (struct cmd_text_pdu *) cmdret->payload;
				char *buf = (char *) __pdu->buf;
				int len = le32_to_cpu(__pdu->buf_len);

				buf[len] = '\0';
			} else if (cmdret->opcode == FIO_NET_CMD_JOB) {
				struct cmd_job_pdu *__pdu = (struct cmd_job_pdu *) cmdret->payload;
				char *buf = (char *) __pdu->buf;
				int len = le32_to_cpu(__pdu->buf_len);

				buf[len] = '\0';
			}
		}

		/* frag flag is internal */
		cmdret->flags &= ~FIO_NET_CMD_F_MORE;
	}

	return cmdret;
}

/*
 * [한국어]
 * add_reply - 전송한 명령에 대한 응답 추적 엔트리를 대기 리스트에 추가
 *
 * @tag: alloc_reply 가 반환한 포인터를 비트 캐스트한 값.
 * @list: 응답 대기 리스트(클라이언트 측 fio_client->cmd_list).
 * @return: 없음.
 *
 * 왜 포인터 인코딩인가: 프로토콜 tag 필드는 64비트 불투명 값 — 클라이언트는 포인터를
 * 그대로 심어두고 서버 응답이 돌아올 때 자기 reply 구조체를 찾는 용도로 사용.
 * 네트워크를 넘어도 변하지 않으므로 OK.
 * 호출 체인: fio_net_send_cmd / fio_net_send_simple_cmd → add_reply → flist_add_tail.
 */
static void add_reply(uint64_t tag, struct flist_head *list)
{
	struct fio_net_cmd_reply *reply;

	reply = (struct fio_net_cmd_reply *) (uintptr_t) tag;  /* [한국어] 비트 캐스트로 포인터 복원 */
	flist_add_tail(&reply->list, list);                    /* [한국어] 응답 대기 리스트에 연결 */
}

/*
 * [한국어]
 * alloc_reply - 응답 추적 구조체 할당 후 포인터를 비트 캐스트로 tag 로 반환
 *
 * @tag: 상위에서 전달된 원본 태그(saved_tag 로 보존 — 응답 식별에 활용).
 * @opcode: 이 요청의 FIO_NET_CMD_* — 응답 매칭 검증에 사용.
 * @return: (uintptr_t) reply 포인터를 uint64_t 로 비트 캐스트한 값.
 *
 * 생명주기: 대기 리스트(flist)에 들어간 채로 응답 도착을 기다림 → 응답 처리 후
 *          호출자가 free_reply 로 해제. 실패 경로에선 송신 실패 시 free_reply 로 회수.
 */
static uint64_t alloc_reply(uint64_t tag, uint16_t opcode)
{
	struct fio_net_cmd_reply *reply;

	reply = calloc(1, sizeof(*reply));        /* [한국어] 0 초기화된 reply 할당 */
	INIT_FLIST_HEAD(&reply->list);            /* [한국어] intrusive 리스트 노드 초기화 */
	fio_gettime(&reply->ts, NULL);            /* [한국어] 요청 시각 기록 — ETA/RTT 측정 */
	reply->saved_tag = tag;                   /* [한국어] 원본 태그 보존 */
	reply->opcode = opcode;                   /* [한국어] 응답 opcode 매칭용 */

	return (uintptr_t) reply;  /* [한국어] 포인터를 wire tag 로 전송 */
}

/*
 * [한국어]
 * free_reply - tag 에 인코딩된 reply 포인터를 디코딩하여 free
 *
 * @tag: alloc_reply 가 반환했던 값.
 * @return: 없음.
 *
 * 호출 경로: (a) 송신 실패 시 즉시 회수, (b) 응답 수신 후 클라이언트 측에서 회수.
 */
static void free_reply(uint64_t tag)
{
	struct fio_net_cmd_reply *reply;

	reply = (struct fio_net_cmd_reply *) (uintptr_t) tag;  /* [한국어] 비트 캐스트로 포인터 복원 */
	free(reply);
}

/*
 * [한국어]
 * fio_net_cmd_crc_pdu - 헤더 CRC 와 외부 PDU 버퍼에 대한 CRC 를 계산하여 헤더에 세트
 *
 * @cmd: 헤더 구조체 (cmd_crc16/pdu_crc16 필드가 LE 로 채워짐).
 * @pdu: 별도 할당된 페이로드 버퍼(cmd->payload 와 분리된 경우).
 * @return: 없음.
 *
 * 두 CRC 분리 계산 이유: 프래그먼트 전송 시 헤더는 매번 재계산되지만 페이로드 버퍼는
 * 위치가 다를 수 있다. writev iov[0]=cmd + iov[1]=buf 형태(fio_send_cmd_ext_pdu) 에서 사용.
 */
static void fio_net_cmd_crc_pdu(struct fio_net_cmd *cmd, const void *pdu)
{
	uint32_t pdu_len;

	cmd->cmd_crc16 = __cpu_to_le16(fio_crc16(cmd, FIO_NET_CMD_CRC_SZ));
	/* [한국어] 헤더 부분(FIO_NET_CMD_CRC_SZ = pdu_crc16 이전까지) CRC → LE 로 저장.
	 *         cmd_crc16 자체는 필드 값 0 기준으로 계산하도록 fio_crc16 설계.
	 */

	pdu_len = le32_to_cpu(cmd->pdu_len);  /* [한국어] LE 로 저장된 길이를 CPU 해석 */
	cmd->pdu_crc16 = __cpu_to_le16(fio_crc16(pdu, pdu_len));
	/* [한국어] 별도 버퍼 pdu 의 CRC — 수신 측 fio_net_recv_cmd 가 비교. */
}

/*
 * [한국어]
 * fio_net_cmd_crc - cmd->payload 인라인 페이로드에 대해 CRC16 계산(wrapper)
 *
 * @cmd: 헤더+인라인 페이로드가 연속된 버퍼.
 * @return: 없음.
 *
 * 인라인 페이로드 경로: fio_net_send_cmd 가 malloc(sizeof(cmd)+pdu_len) 한 후
 *                     payload[] flexible array 로 접근할 때 사용.
 */
static void fio_net_cmd_crc(struct fio_net_cmd *cmd)
{
	fio_net_cmd_crc_pdu(cmd, cmd->payload);  /* [한국어] cmd 연속 영역의 payload 시작점 사용 */
}

/*
 * [한국어]
 * fio_net_send_cmd - 인라인 페이로드 버전의 완전한 명령 전송 (프래그먼트 자동 분할)
 *
 * @fd: 전송 소켓 fd.
 * @opcode: FIO_NET_CMD_* opcode.
 * @buf: 페이로드 버퍼 (NULL 이면 SIMPLE 명령과 유사).
 * @size: 페이로드 바이트 수.
 * @tagptr: In/Out. NULL=태그 없음. list 가 있으면 이 위치에 alloc_reply 결과가 저장됨.
 * @list: non-NULL 이면 alloc_reply + add_reply 로 응답 대기 리스트에 등록.
 * @return: 0 성공, 1 실패.
 *
 * 프래그먼트 전송: size > FIO_SERVER_MAX_FRAGMENT_PDU 이면 조각으로 나눠
 * 각 조각에 FIO_NET_CMD_F_MORE 를 세트(마지막 조각 제외)하고 연속 전송.
 * 버퍼 재사용: cur_len 이 this_len 보다 작거나 cmd==NULL 이면 realloc 대신 free+malloc.
 * 호출 체인: fio_net_send_simple_cmd 등 상위 편의 함수에서 호출 → fio_init_net_cmd +
 *           fio_net_cmd_crc + fio_send_data → 커널 TCP 송신.
 */
int fio_net_send_cmd(int fd, uint16_t opcode, const void *buf, off_t size,
		     uint64_t *tagptr, struct flist_head *list)
{
	struct fio_net_cmd *cmd = NULL;
	size_t this_len, cur_len = 0;
	uint64_t tag;
	int ret;

	if (list) {
		assert(tagptr);                           /* [한국어] 응답 추적 요청 시 tagptr 필수 */
		tag = *tagptr = alloc_reply(*tagptr, opcode); /* [한국어] reply 할당 + tagptr 갱신 */
	} else
		tag = tagptr ? *tagptr : 0;   /* [한국어] 호출자 지정 태그 또는 0 */

	do {
		this_len = size;
		if (this_len > FIO_SERVER_MAX_FRAGMENT_PDU)
			this_len = FIO_SERVER_MAX_FRAGMENT_PDU;  /* [한국어] 한 조각의 상한으로 클램프 */

		/* [한국어] 버퍼 재사용: cur_len 이 부족하면 재할당 */
		if (!cmd || cur_len < sizeof(*cmd) + this_len) {
			if (cmd)
				free(cmd);  /* [한국어] 기존 작은 버퍼 해제 */

			cur_len = sizeof(*cmd) + this_len;
			cmd = malloc(cur_len);
		}

		fio_init_net_cmd(cmd, opcode, buf, this_len, tag);  /* [한국어] 헤더+페이로드 세팅 */

		if (this_len < size)
			cmd->flags = __cpu_to_le32(FIO_NET_CMD_F_MORE);
			/* [한국어] 뒤에 조각이 더 있음 — 수신 측이 재조립 */

		fio_net_cmd_crc(cmd);  /* [한국어] cmd_crc16 + pdu_crc16 계산·세트 */

		ret = fio_send_data(fd, cmd, sizeof(*cmd) + this_len);  /* [한국어] writev 단일 호출 */
		size -= this_len;  /* [한국어] 남은 크기 감소 */
		buf += this_len;   /* [한국어] 버퍼 포인터 전진 */
	} while (!ret && size);  /* [한국어] 성공 & 남은 데이터 있으면 계속 */

	/* [한국어] 응답 추적 — 실패 시 reply 회수, 성공 시 대기 리스트에 등록 */
	if (list) {
		if (ret)
			free_reply(tag);
		else
			add_reply(tag, list);
	}

	if (cmd)
		free(cmd);  /* [한국어] 임시 버퍼 해제 (응답 추적과 무관) */

	return ret;
}

/*
 * [한국어]
 * fio_net_prep_cmd - sk_entry 하나를 SHM 힙에 할당해 채움(큐에 넣기 직전 상태)
 *
 * @opcode: FIO_NET_CMD_*.
 * @buf: 페이로드 포인터. SK_F_COPY 면 smalloc 힙으로 복사, 아니면 그대로 저장.
 * @size: 페이로드 크기.
 * @tagptr: 요청-응답 매칭 태그 포인터(NULL 허용).
 * @flags: SK_F_* 비트 OR.
 * @return: 성공 시 sk_entry*, 실패(smalloc 실패) 시 NULL.
 *
 * 호출 체인: fio_net_queue_cmd / fio_send_iolog 등 → fio_net_prep_cmd →
 *           fio_net_queue_entry → handle_xmits.
 */
static struct sk_entry *fio_net_prep_cmd(uint16_t opcode, void *buf,
					 size_t size, uint64_t *tagptr,
					 int flags)
{
	struct sk_entry *entry;

	entry = smalloc(sizeof(*entry));  /* [한국어] SHM 힙에 엔트리 할당 — fork 자식도 접근 가능 */
	if (!entry)
		return NULL;

	INIT_FLIST_HEAD(&entry->next);  /* [한국어] VEC 체인 헤드 — 빈 리스트로 초기화 */
	entry->opcode = opcode;
	if (flags & SK_F_COPY) {
		entry->buf = smalloc(size);   /* [한국어] 호출자 버퍼를 SHM 복사 — 생명주기 독립 */
		memcpy(entry->buf, buf, size);
	} else
		entry->buf = buf;  /* [한국어] 포인터만 보관 — SK_F_FREE/기타 소유권 */

	entry->size = size;
	if (tagptr)
		entry->tag = *tagptr;
	else
		entry->tag = 0;
	entry->flags = flags;
	return entry;
}

static int handle_sk_entry(struct sk_out *sk_out, struct sk_entry *entry);

/*
 * [한국어]
 * fio_net_queue_entry - sk_entry 를 현재 스레드의 sk_out 전송 큐에 추가
 *
 * @entry: 이미 prep 된 sk_entry.
 * @return: 없음.
 *
 * SK_F_INLINE 분기: 큐잉 지연 없이 즉시 handle_sk_entry 로 전송 (VTRIGGER 등 시급한 응답).
 * 일반 경로: sk_out->lock 으로 리스트 보호하며 꼬리에 추가 → sk_out->wait 올려
 *           handle_xmits/handle_connection 을 깨움.
 */
static void fio_net_queue_entry(struct sk_entry *entry)
{
	struct sk_out *sk_out = pthread_getspecific(sk_out_key);  /* [한국어] 현재 스레드의 소켓 컨텍스트 */

	if (entry->flags & SK_F_INLINE)
		handle_sk_entry(sk_out, entry);  /* [한국어] 지연 없이 즉시 전송 */
	else {
		sk_lock(sk_out);                                     /* [한국어] 리스트 보호 */
		flist_add_tail(&entry->list, &sk_out->list);         /* [한국어] FIFO 꼬리에 추가 */
		sk_unlock(sk_out);

		fio_sem_up(&sk_out->wait);  /* [한국어] handle_connection poll 루프 깨움(절전 탈출) */
	}
}

/*
 * [한국어]
 * fio_net_queue_cmd - prep + queue 를 한 번에 (상위 편의 함수)
 *
 * @opcode/@buf/@size/@tagptr/@flags: fio_net_prep_cmd 참조.
 * @return: 0 성공, 1 실패(smalloc 실패).
 */
static int fio_net_queue_cmd(uint16_t opcode, void *buf, off_t size,
			     uint64_t *tagptr, int flags)
{
	struct sk_entry *entry;

	entry = fio_net_prep_cmd(opcode, buf, size, tagptr, flags);
	if (entry) {
		fio_net_queue_entry(entry);
		return 0;
	}

	return 1;
}

/*
 * [한국어]
 * fio_net_send_simple_stack_cmd - 페이로드 없는 명령을 스택 변수로 즉시 직송
 *
 * @sk: 소켓 fd.
 * @opcode: FIO_NET_CMD_*.
 * @tag: 태그.
 * @return: 0 성공, 1 실패.
 *
 * 왜 스택 변수인가: 페이로드 없는 24B 헤더만 전송하므로 heap 할당 불필요.
 * 호출 체인: fio_net_send_simple_cmd → fio_net_send_simple_stack_cmd → fio_send_data.
 */
static int fio_net_send_simple_stack_cmd(int sk, uint16_t opcode, uint64_t tag)
{
	struct fio_net_cmd cmd;  /* [한국어] 스택에 24B 헤더 */

	fio_init_net_cmd(&cmd, opcode, NULL, 0, tag);  /* [한국어] 페이로드 0 */
	fio_net_cmd_crc(&cmd);                         /* [한국어] 헤더 CRC 계산 */

	return fio_send_data(sk, &cmd, sizeof(cmd));   /* [한국어] writev 단일 호출 */
}

/*
 * If 'list' is non-NULL, then allocate and store the sent command for
 * later verification.
 */
/*
 * [한국어]
 * fio_net_send_simple_cmd - 페이로드 없는 명령의 외부용 API (응답 추적 옵션 포함)
 *
 * @sk: 소켓 fd.
 * @opcode: FIO_NET_CMD_*.
 * @tag: 원본 태그. list 있으면 내부적으로 alloc_reply 결과로 대체.
 * @list: non-NULL 이면 응답 대기 리스트 등록.
 * @return: 0 성공, 송신 실패 시 errno 또는 음수.
 *
 * 에러 경로: 송신 실패 시 alloc_reply 로 생성된 reply 를 free_reply 로 회수.
 */
int fio_net_send_simple_cmd(int sk, uint16_t opcode, uint64_t tag,
			    struct flist_head *list)
{
	int ret;

	if (list)
		tag = alloc_reply(tag, opcode);  /* [한국어] 응답 대기 엔트리 할당 */

	ret = fio_net_send_simple_stack_cmd(sk, opcode, tag);
	if (ret) {
		if (list)
			free_reply(tag);  /* [한국어] 송신 실패 — reply 회수 */

		return ret;
	}

	if (list)
		add_reply(tag, list);  /* [한국어] 송신 성공 — 대기 리스트에 등록 */

	return 0;
}

/*
 * [한국어]
 * fio_net_queue_quit - QUIT 명령을 현재 스레드 전송 큐에 추가
 *
 * @return: 0 성공, 1 실패.
 *
 * 의미: "현재 잡을 종료하시오" — 연결은 유지. handle_xmits 가 비동기 flush.
 */
static int fio_net_queue_quit(void)
{
	dprint(FD_NET, "server: sending quit\n");

	return fio_net_queue_cmd(FIO_NET_CMD_QUIT, NULL, 0, NULL, SK_F_SIMPLE);
	/* [한국어] SK_F_SIMPLE — 페이로드 없는 명령 */
}

/*
 * [한국어]
 * fio_net_send_quit - QUIT 을 소켓에 직접 전송 (큐 경유 X)
 *
 * @sk: 소켓 fd.
 * @return: 0 성공, 1 실패.
 *
 * 주로 클라이언트 측이 서버에 종료 요청을 보낼 때 사용.
 */
int fio_net_send_quit(int sk)
{
	dprint(FD_NET, "server: sending quit\n");

	return fio_net_send_simple_cmd(sk, FIO_NET_CMD_QUIT, 0, NULL);
}

/*
 * [한국어]
 * fio_net_send_ack - STOP 명령(잡 종료 상태 + 시그널)을 큐에 추가
 *
 * @cmd: 요청 측 cmd (NULL 가능 — 자발적 STOP).
 * @error: 잡 exit status.
 * @signal: 시그널로 죽었을 때 시그널 번호, 아니면 0.
 * @return: 0 성공, 1 실패.
 */
static int fio_net_send_ack(struct fio_net_cmd *cmd, int error, int signal)
{
	struct cmd_end_pdu epdu;
	uint64_t tag = 0;

	if (cmd)
		tag = cmd->tag;  /* [한국어] 요청 태그 보존 — 클라 측에서 원 요청과 매칭 */

	epdu.error = __cpu_to_le32(error);    /* [한국어] 32비트 LE 로 직렬화 */
	epdu.signal = __cpu_to_le32(signal);
	return fio_net_queue_cmd(FIO_NET_CMD_STOP, &epdu, sizeof(epdu), &tag, SK_F_COPY);
	/* [한국어] SK_F_COPY — 스택 변수 epdu 를 smalloc 복사 */
}

/*
 * [한국어]
 * fio_net_queue_stop - 잡 종료를 클라에 알리는 STOP 명령 큐잉(간편 래퍼)
 *
 * @error: exit status. @signal: 시그널 번호(없으면 0).
 * @return: 0 성공, 1 실패.
 *
 * 호출 체인: fio_server_fork_item_done(자식 잡 종료 감지 시) → fio_net_queue_stop →
 *           fio_net_send_ack → fio_net_queue_cmd.
 */
static int fio_net_queue_stop(int error, int signal)
{
	dprint(FD_NET, "server: sending stop (%d, %d)\n", error, signal);
	return fio_net_send_ack(NULL, error, signal);
}

/*
 * [한국어] === 포크 아이템 추적/종료 회수 함수군 ===
 * conn_list(연결 처리 프로세스) 와 job_list(잡 실행 프로세스/스레드) 각각에 엔트리
 * 추가, 비블로킹 waitpid/GetExitCodeProcess 로 종료 감지, 회수 시 필요에 따라
 * STOP/QUIT 클라 전송. POSIX 와 Windows 경로 분리.
 */
#ifdef WIN32
/*
 * [한국어]
 * fio_server_add_fork_item (Windows) - fio_fork_item 을 할당해 리스트에 꼬리 추가
 *
 * @element: thread/hProcess 유니온.
 * @list: conn_list 또는 job_list.
 * @return: 없음.
 */
static void fio_server_add_fork_item(struct ffi_element *element, struct flist_head *list)
{
	struct fio_fork_item *ffi;

	ffi = malloc(sizeof(*ffi));
	ffi->exitval = 0;
	ffi->signal = 0;
	ffi->exited = 0;
	ffi->element = *element;
	flist_add_tail(&ffi->list, list);
}

/*
 * [한국어]
 * fio_server_add_conn_pid (Windows) - 연결 처리 자식 프로세스 HANDLE 을 conn_list 에 등록
 *
 * @conn_list: 연결 처리 프로세스 리스트.
 * @hProcess: CreateProcess 반환 프로세스 핸들.
 * @return: 없음.
 */
static void fio_server_add_conn_pid(struct flist_head *conn_list, HANDLE hProcess)
{
	struct ffi_element element = {.hProcess = hProcess, .is_thread=FALSE};
	dprint(FD_NET, "server: forked off connection job (tid=%u)\n", (int) element.thread);

	fio_server_add_fork_item(&element, conn_list);
}

/*
 * [한국어]
 * fio_server_add_job_pid (Windows) - 잡 실행 pthread 핸들을 job_list 에 등록
 *
 * @job_list: 잡 실행 스레드 리스트.
 * @thread: pthread_create 반환 스레드 핸들.
 * @return: 없음.
 *
 * is_thread=TRUE 로 유니온 판별자 세트.
 */
static void fio_server_add_job_pid(struct flist_head *job_list, pthread_t thread)
{
	struct ffi_element element = {.thread = thread, .is_thread=TRUE};
	dprint(FD_NET, "server: forked off job job (tid=%u)\n", (int) element.thread);
	fio_server_add_fork_item(&element, job_list);
}

/*
 * [한국어]
 * fio_server_check_fork_item (Windows) - 스레드/프로세스 종료 상태 비블로킹 확인
 *
 * @ffi: 체크 대상.
 * @return: 없음. ffi->exited/exitval 업데이트.
 *
 * 분기:
 *   is_thread=true → pthread_kill(thread, 0) 로 존재 확인. 죽었으면 pthread_join 으로
 *                   exit code 수거(POSIX 스레드의 "status" = void* 반환값).
 *   is_thread=false → GetExitCodeProcess 로 STILL_ACTIVE 확인. STILL_ACTIVE 아니면 종료.
 */
static void fio_server_check_fork_item(struct fio_fork_item *ffi)
{
	int ret;

	if (ffi->element.is_thread) {

		ret = pthread_kill(ffi->element.thread, 0);
		if (ret) {
			int rev_val;
			pthread_join(ffi->element.thread, (void**) &rev_val); /*if the thread is dead, then join it to get status*/

			ffi->exitval = rev_val;
			if (ffi->exitval)
				log_err("thread (tid=%u) exited with %x\n", (int) ffi->element.thread, (int) ffi->exitval);
			dprint(FD_PROCESS, "thread (tid=%u) exited with %x\n", (int) ffi->element.thread, (int) ffi->exitval);
			ffi->exited = 1;
		}
	} else {
		DWORD exit_val;
		GetExitCodeProcess(ffi->element.hProcess, &exit_val);

		if (exit_val != STILL_ACTIVE) {
			dprint(FD_PROCESS, "process %u exited with %d\n", GetProcessId(ffi->element.hProcess), exit_val);
			ffi->exited = 1;
			ffi->exitval = exit_val;
		}
	}
}
#else
/*
 * [한국어]
 * fio_server_add_fork_item (POSIX) - fio_fork_item 할당 후 리스트 꼬리 추가
 *
 * @pid: 자식 PID (fork 반환값).
 * @list: conn_list 또는 job_list.
 * @return: 없음.
 */
static void fio_server_add_fork_item(pid_t pid, struct flist_head *list)
{
	struct fio_fork_item *ffi;

	ffi = malloc(sizeof(*ffi));
	ffi->exitval = 0;
	ffi->signal = 0;
	ffi->exited = 0;
	ffi->pid = pid;                        /* [한국어] waitpid 대상 */
	flist_add_tail(&ffi->list, list);
}

/*
 * [한국어]
 * fio_server_add_conn_pid (POSIX) - 연결 처리 자식 PID 를 conn_list 에 등록
 */
static void fio_server_add_conn_pid(struct flist_head *conn_list, pid_t pid)
{
	dprint(FD_NET, "server: forked off connection job (pid=%u)\n", (int) pid);
	fio_server_add_fork_item(pid, conn_list);
}

/*
 * [한국어]
 * fio_server_add_job_pid (POSIX) - 잡 실행 자식 PID 를 job_list 에 등록
 */
static void fio_server_add_job_pid(struct flist_head *job_list, pid_t pid)
{
	dprint(FD_NET, "server: forked off job job (pid=%u)\n", (int) pid);
	fio_server_add_fork_item(pid, job_list);
}

/*
 * [한국어]
 * fio_server_check_fork_item (POSIX) - waitpid(WNOHANG) 로 자식 종료 비블로킹 확인
 *
 * @ffi: 체크 대상 fork item.
 * @return: 없음. ffi->exited/signal/exitval 업데이트.
 *
 * waitpid 반환 규약:
 *   0 = 자식 아직 살아있음 (WNOHANG). ffi->exited 변경 없음.
 *   >0 = pid 반환 = 종료 감지. status 를 WIFSIGNALED/WIFEXITED 로 해석.
 *   <0 + ECHILD = 자식이 이미 회수됨 또는 존재하지 않음 → 분실 경고 + exited=1.
 *   기타 에러 = 로그만 남기고 다음 루프 대기.
 */
static void fio_server_check_fork_item(struct fio_fork_item *ffi)
{
	int ret, status;

	ret = waitpid(ffi->pid, &status, WNOHANG);  /* [한국어] 블로킹 없이 즉시 상태 조회 */
	if (ret < 0) {
		if (errno == ECHILD) {
			/* [한국어] 자식이 이미 reap 되었거나 존재하지 않음 */
			log_err("fio: connection pid %u disappeared\n", (int) ffi->pid);
			ffi->exited = 1;
		} else
			log_err("fio: waitpid: %s\n", strerror(errno));
	} else if (ret == ffi->pid) {
		/* [한국어] 자식이 종료됨 — status 해석 */
		if (WIFSIGNALED(status)) {
			ffi->signal = WTERMSIG(status);  /* [한국어] 시그널로 종료 */
			ffi->exited = 1;
		}
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status))
				ffi->exitval = WEXITSTATUS(status);  /* [한국어] exit(n) 의 n */
			ffi->exited = 1;
		}
	}
}
#endif

/*
 * [한국어]
 * fio_server_fork_item_done - 종료된 자식(잡/연결) 회수: 리스트 제거 + 필요시 STOP/QUIT
 *
 * @ffi: 종료 확인된 fork item.
 * @stop: true 면 잡 실행 자식 — STOP(exitval/signal)+QUIT 를 클라에 큐잉.
 *        false 면 연결 처리 자식 — STOP 보내지 않음 (연결 자체가 이미 닫힘).
 * @return: 없음.
 *
 * Windows: HANDLE 닫기. POSIX: 별도 리소스 없음(waitpid 가 이미 해제).
 * 호출 체인: fio_server_check_fork_items → fio_server_fork_item_done →
 *           fio_net_queue_stop+fio_net_queue_quit + flist_del + free.
 */
static void fio_server_fork_item_done(struct fio_fork_item *ffi, bool stop)
{
#ifdef WIN32
	if (ffi->element.is_thread)
		dprint(FD_NET, "tid %u exited, sig=%u, exitval=%d\n", (int) ffi->element.thread, ffi->signal, ffi->exitval);
	else {
		dprint(FD_NET, "pid %u exited, sig=%u, exitval=%d\n", (int)  GetProcessId(ffi->element.hProcess), ffi->signal, ffi->exitval);
		CloseHandle(ffi->element.hProcess);
		ffi->element.hProcess = INVALID_HANDLE_VALUE;
	}
#else
	dprint(FD_NET, "pid %u exited, sig=%u, exitval=%d\n", (int) ffi->pid, ffi->signal, ffi->exitval);
#endif

	/*
	 * Fold STOP and QUIT...
	 */
	if (stop) {
		fio_net_queue_stop(ffi->exitval, ffi->signal);
		fio_net_queue_quit();
	}

	flist_del(&ffi->list);
	free(ffi);
}

/*
 * [한국어]
 * fio_server_check_fork_items - 리스트 순회하며 각 자식 상태 점검 + 정리
 *
 * @list: conn_list 또는 job_list.
 * @stop: fio_server_fork_item_done 의 stop 인자.
 * @return: 없음.
 *
 * flist_for_each_safe 이디엄: 순회 중 리스트 원소 제거 가능(tmp 포인터로 안전 진행).
 */
static void fio_server_check_fork_items(struct flist_head *list, bool stop)
{
	struct flist_head *entry, *tmp;
	struct fio_fork_item *ffi;

	flist_for_each_safe(entry, tmp, list) {
		ffi = flist_entry(entry, struct fio_fork_item, list);

		fio_server_check_fork_item(ffi);

		if (ffi->exited)
			fio_server_fork_item_done(ffi, stop);
	}
}

/*
 * [한국어]
 * fio_server_check_jobs - 잡 프로세스 리스트의 종료 확인 (+ STOP/QUIT 송신)
 *
 * @job_list: handle_connection 보유 잡 프로세스 리스트.
 * @return: 없음.
 *
 * stop=true — 잡 종료 시 클라에 종료 상태 통보.
 */
static void fio_server_check_jobs(struct flist_head *job_list)
{
	fio_server_check_fork_items(job_list, true);
}

/*
 * [한국어]
 * fio_server_check_conns - 연결 프로세스 리스트 종료 확인 (STOP 송신 없음)
 *
 * @conn_list: accept_loop 의 연결 자식 리스트.
 * @return: 없음.
 *
 * stop=false — 연결 자식 종료는 연결 자체가 이미 끊긴 상태라 통보 불필요/불가.
 */
static void fio_server_check_conns(struct flist_head *conn_list)
{
	fio_server_check_fork_items(conn_list, false);
}

/*
 * [한국어]
 * handle_load_file_cmd - LOAD_FILE 명령 처리: 서버 로컬 ini 파일로부터 잡 로드
 *
 * @cmd: 수신 fio_net_cmd. payload = cmd_load_file_pdu{name_len, client_type, file[]}.
 * @return: 0 성공(START 응답 큐잉), -1 실패(QUIT 큐잉).
 *
 * 클라이언트가 "서버 측 로컬 경로에서 잡 파일을 읽어라" 요청할 때 사용.
 * parse_jobs_ini(name, is_buf=0, stdin=0, client_type) — is_buf=0 → 경로로 취급.
 * 성공 시 FIO_NET_CMD_START PDU(thread_number, stat_outputs) 로 응답.
 * 호출 체인: handle_command(LOAD_FILE case) → handle_load_file_cmd → parse_jobs_ini.
 */
static int handle_load_file_cmd(struct fio_net_cmd *cmd)
{
	struct cmd_load_file_pdu *pdu = (struct cmd_load_file_pdu *) cmd->payload;
	void *file_name = pdu->file;  /* [한국어] flexible array — payload 끝에 경로 문자열 */
	struct cmd_start_pdu spdu;

	dprint(FD_NET, "server: loading local file %s\n", (char *) file_name);

	pdu->name_len = le16_to_cpu(pdu->name_len);         /* [한국어] LE → CPU */
	pdu->client_type = le16_to_cpu(pdu->client_type);

	/* [한국어] parse_jobs_ini(name, is_buf=0, stdin=0, client_type) — 파일 경로로 열어 파싱 */
	if (parse_jobs_ini(file_name, 0, 0, pdu->client_type)) {
		fio_net_queue_quit();  /* [한국어] 파싱 실패 — 클라에 QUIT 통보 */
		return -1;
	}

	spdu.jobs = cpu_to_le32(thread_number);       /* [한국어] 파싱된 잡 수 */
	spdu.stat_outputs = cpu_to_le32(stat_number); /* [한국어] 통계 출력 엔트리 수 */
	fio_net_queue_cmd(FIO_NET_CMD_START, &spdu, sizeof(spdu), NULL, SK_F_COPY);
	return 0;
}

#ifdef WIN32
/*
 * [한국어]
 * fio_backend_thread (Windows) - pthread_create 엔트리 포인트 (fork 대체)
 *
 * @data: sk_out 포인터.
 * @return: pthread_exit 으로 반환 — ret 을 void* 로 인코딩.
 *
 * POSIX 는 fork 로 주소공간을 분리하지만 Windows 는 pthread_create 로 스레드만 분리.
 * sk_out_assign 을 이 스레드에서 다시 호출해 refs 를 2 로 유지 — 호출자(부모)와
 * 자식 스레드 양쪽이 완료해야 안전하게 해제.
 */
static void *fio_backend_thread(void *data)
{
	int ret;
	struct sk_out *sk_out = (struct sk_out *) data;

	sk_out_assign(sk_out);

	ret = fio_backend(sk_out);
	sk_out_drop();

	pthread_exit((void*) (intptr_t) ret);
	return NULL;
}
#endif

/*
 * [한국어]
 * handle_run_cmd - RUN 명령 처리: 실제 I/O 잡 실행 시작
 *
 * @sk_out: 연결 소켓 컨텍스트 (자식도 공유 — refs 2 증가).
 * @job_list: 잡 프로세스/스레드 추적 리스트.
 * @cmd: FIO_NET_CMD_RUN (페이로드 없음 — 태그만 사용 가능).
 * @return: 0 성공, 음수 실패(pthread_create 에러 등).
 *
 * POSIX: fork() 하여 자식에서 fio_backend(sk_out) 실행 → 자식이 STOP 큐잉 후 _exit.
 * Windows: fork 없음 → pthread_create(fio_backend_thread) 로 별도 스레드 시작.
 * 두 경우 모두 부모/자식이 sk_out_assign 을 각각 호출해 refs++ → 자식 종료 시에만
 * 자기 참조 드롭으로 안전한 해제.
 * fio_time_init + set_genesis_time 을 호출해 이 잡 실행 시작 시간 기준 세팅.
 * 호출 체인: handle_command(RUN case) → handle_run_cmd → fork → fio_backend.
 */
static int handle_run_cmd(struct sk_out *sk_out, struct flist_head *job_list,
			  struct fio_net_cmd *cmd)
{
	int ret;

	fio_time_init();      /* [한국어] 타임베이스 정렬 (TSC 또는 clock_gettime 백엔드) */
	set_genesis_time();   /* [한국어] 잡 실행 시작 기준 시각 저장 */

#ifdef WIN32
	{
		pthread_t thread;
		/* both this thread and backend_thread call sk_out_assign() to double increment
		 * the ref count.  This ensures struct is valid until both threads are done with it
		 */
		sk_out_assign(sk_out);  /* [한국어] 부모측 refs++ */
		ret = pthread_create(&thread, NULL,	fio_backend_thread, sk_out);
		if (ret) {
			log_err("pthread_create: %s\n", strerror(ret));
			return ret;
		}

		fio_server_add_job_pid(job_list, thread);  /* [한국어] 스레드 핸들 추적 */
		return ret;
	}
#else
    {
		pid_t pid;
		sk_out_assign(sk_out);  /* [한국어] 부모 측 refs++ (fork 전이라 자식도 상속) */
		pid = fork();
		if (pid) {
			/* [한국어] 부모 경로: 잡 PID 추적 리스트에 추가 */
			fio_server_add_job_pid(job_list, pid);
			return 0;
		}

		/* [한국어] 자식 경로: 실제 I/O 잡 실행 */
		ret = fio_backend(sk_out);  /* [한국어] 잡 스레드 생성 + I/O 루프 + 통계 수집 */
		free_threads_shm();         /* [한국어] SHM 에 잡 구조체 회수 */
		sk_out_drop();              /* [한국어] 자식측 refs-- */
		_exit(ret);                 /* [한국어] 자식 즉시 종료 — atexit/stdio 생략 */
	}
#endif
}

/*
 * [한국어]
 * handle_job_cmd - JOB 명령 처리: 클라가 보낸 ini 버퍼를 서버가 파싱
 *
 * @cmd: payload = cmd_job_pdu{buf_len, client_type, buf[]}.
 * @return: 0 성공(START 큐잉), -1 실패(QUIT 큐잉).
 *
 * LOAD_FILE 과 다른 점: buf 는 파일 경로가 아니라 ini 내용 자체. parse_jobs_ini 의
 * 두 번째 인자 is_buf=1 로 호출해 내부에서 경로 대신 버퍼로 취급.
 */
static int handle_job_cmd(struct fio_net_cmd *cmd)
{
	struct cmd_job_pdu *pdu = (struct cmd_job_pdu *) cmd->payload;
	void *buf = pdu->buf;       /* [한국어] flexible array — ini 문자열 */
	struct cmd_start_pdu spdu;

	pdu->buf_len = le32_to_cpu(pdu->buf_len);
	pdu->client_type = le32_to_cpu(pdu->client_type);

	/* [한국어] is_buf=1 → parse_jobs_ini 가 buf 를 메모리 버퍼로 해석 */
	if (parse_jobs_ini(buf, 1, 0, pdu->client_type)) {
		fio_net_queue_quit();
		return -1;
	}

	spdu.jobs = cpu_to_le32(thread_number);
	spdu.stat_outputs = cpu_to_le32(stat_number);

	fio_net_queue_cmd(FIO_NET_CMD_START, &spdu, sizeof(spdu), NULL, SK_F_COPY);
	return 0;
}

/*
 * [한국어]
 * handle_jobline_cmd - JOBLINE 명령 처리: 클라가 보낸 CLI argv 를 재구성해 parse_cmd_line
 *
 * @cmd: payload = cmd_line_pdu{lines, client_type} + 이어서 lines 개의
 *       cmd_single_line_pdu{len, text[]} 가 연속 배치.
 * @return: 0 성공(START 큐잉), -1 실패(QUIT 큐잉).
 *
 * 직렬화 형식: PDU 는 가변 길이 문자열 배열을 담으므로 오프셋 계산으로 각 인자를 분리.
 * argv[] 배열을 malloc 후 parse_cmd_line 호출.
 */
static int handle_jobline_cmd(struct fio_net_cmd *cmd)
{
	void *pdu = cmd->payload;
	struct cmd_single_line_pdu *cslp;
	struct cmd_line_pdu *clp;
	unsigned long offset;
	struct cmd_start_pdu spdu;
	char **argv;
	int i;

	clp = pdu;
	clp->lines = le16_to_cpu(clp->lines);
	clp->client_type = le16_to_cpu(clp->client_type);
	argv = malloc(clp->lines * sizeof(char *));
	offset = sizeof(*clp);

	dprint(FD_NET, "server: %d command line args\n", clp->lines);

	for (i = 0; i < clp->lines; i++) {
		cslp = pdu + offset;
		argv[i] = (char *) cslp->text;

		offset += sizeof(*cslp) + le16_to_cpu(cslp->len);
		dprint(FD_NET, "server: %d: %s\n", i, argv[i]);
	}

	if (parse_cmd_line(clp->lines, argv, clp->client_type)) {
		fio_net_queue_quit();
		free(argv);
		return -1;
	}

	free(argv);

	spdu.jobs = cpu_to_le32(thread_number);
	spdu.stat_outputs = cpu_to_le32(stat_number);

	fio_net_queue_cmd(FIO_NET_CMD_START, &spdu, sizeof(spdu), NULL, SK_F_COPY);
	return 0;
}

/*
 * [한국어]
 * handle_probe_cmd - PROBE 명령 처리: 서버 식별 정보 응답 + zlib 네고
 *
 * @cmd: payload = cmd_client_probe_pdu{flags, server[]}. 클라가 서버를 식별하는
 *       이름(me[]) 과 자신의 flags(FIO_PROBE_FLAG_ZLIB) 를 전송.
 * @return: 0 성공(PROBE 응답 큐잉), 1 실패.
 *
 * 응답 내용: hostname(gethostname), fio_version, OS, arch, bpp(포인터 크기),
 *          cpus_configured(_SC_NPROCESSORS), bigendian 플래그, zlib 지원 여부.
 * zlib 양측 지원 시 use_zlib=1 → fio_send_iolog 가 IOLOG 를 XMIT_COMPRESSED 로 전송.
 */
static int handle_probe_cmd(struct fio_net_cmd *cmd)
{
	struct cmd_client_probe_pdu *pdu = (struct cmd_client_probe_pdu *) cmd->payload;
	uint64_t tag = cmd->tag;
	struct cmd_probe_reply_pdu probe = {
#ifdef CONFIG_BIG_ENDIAN
		.bigendian	= 1,
#endif
		.os		= FIO_OS,
		.arch		= FIO_ARCH,
		.bpp		= sizeof(void *),
		.cpus		= __cpu_to_le32(cpus_configured()),
	};

	dprint(FD_NET, "server: sending probe reply\n");

	strcpy(me, (char *) pdu->server);

	gethostname((char *) probe.hostname, sizeof(probe.hostname));
	snprintf((char *) probe.fio_version, sizeof(probe.fio_version), "%s",
		 fio_version_string);

	/*
	 * If the client supports compression and we do too, then enable it
	 */
	if (has_zlib && le64_to_cpu(pdu->flags) & FIO_PROBE_FLAG_ZLIB) {
		probe.flags = __cpu_to_le64(FIO_PROBE_FLAG_ZLIB);
		use_zlib = 1;
	} else {
		probe.flags = 0;
		use_zlib = 0;
	}

	return fio_net_queue_cmd(FIO_NET_CMD_PROBE, &probe, sizeof(probe), &tag, SK_F_COPY);
}

/*
 * [한국어]
 * handle_send_eta_cmd - SEND_ETA 명령 처리: 현재 잡 진행률(ETA) 를 ETA 응답으로 전송
 *
 * @cmd: payload 없음. cmd->tag 를 응답 태그로 보존.
 * @return: 0 (항상 성공 취급 — 로컬 ETA 없으면 zero-filled 응답).
 *
 * 왜 zero-filled 응답인가: 클라가 ETA 요청 후 응답 타임아웃을 돌리고 있으므로
 * 서버 쪽에 잡이 없어도 반드시 응답해야 함.
 * get_jobs_eta(true, &size) 가 활성 잡 집계. 모든 필드를 LE 로 변환.
 * DDIR_RWDIR_CNT = 3 (READ/WRITE/TRIM) 방향별 rate/iops 4종 세트 변환.
 */
static int handle_send_eta_cmd(struct fio_net_cmd *cmd)
{
	struct jobs_eta *je;
	uint64_t tag = cmd->tag;
	size_t size;
	int i;

	dprint(FD_NET, "server sending status\n");

	/*
	 * Fake ETA return if we don't have a local one, otherwise the client
	 * will end up timing out waiting for a response to the ETA request
	 */
	je = get_jobs_eta(true, &size);
	if (!je) {
		size = sizeof(*je);
		je = calloc(1, size);
	} else {
		je->nr_running		= cpu_to_le32(je->nr_running);
		je->nr_ramp		= cpu_to_le32(je->nr_ramp);
		je->nr_pending		= cpu_to_le32(je->nr_pending);
		je->nr_setting_up	= cpu_to_le32(je->nr_setting_up);
		je->files_open		= cpu_to_le32(je->files_open);

		for (i = 0; i < DDIR_RWDIR_CNT; i++) {
			je->m_rate[i]	= cpu_to_le64(je->m_rate[i]);
			je->t_rate[i]	= cpu_to_le64(je->t_rate[i]);
			je->m_iops[i]	= cpu_to_le32(je->m_iops[i]);
			je->t_iops[i]	= cpu_to_le32(je->t_iops[i]);
			je->rate[i]	= cpu_to_le64(je->rate[i]);
			je->iops[i]	= cpu_to_le32(je->iops[i]);
		}

		je->elapsed_sec		= cpu_to_le64(je->elapsed_sec);
		je->eta_sec		= cpu_to_le64(je->eta_sec);
		je->nr_threads		= cpu_to_le32(je->nr_threads);
		je->is_pow2		= cpu_to_le32(je->is_pow2);
		je->unit_base		= cpu_to_le32(je->unit_base);
	}

	fio_net_queue_cmd(FIO_NET_CMD_ETA, je, size, &tag, SK_F_FREE);
	return 0;
}

/*
 * [한국어]
 * send_update_job_reply - UPDATE_JOB 응답 (성공/실패 에러 코드) 전송
 *
 * @__tag: 요청자의 태그(응답 매칭용).
 * @error: 0 성공 또는 errno.
 * @return: 0 성공, 1 실패.
 */
static int send_update_job_reply(uint64_t __tag, int error)
{
	uint64_t tag = __tag;
	uint32_t pdu_error;

	pdu_error = __cpu_to_le32(error);
	return fio_net_queue_cmd(FIO_NET_CMD_UPDATE_JOB, &pdu_error, sizeof(pdu_error), &tag, SK_F_COPY);
}

/*
 * [한국어]
 * handle_update_job_cmd - UPDATE_JOB 처리: 실행 중 잡의 옵션을 변경
 *
 * @cmd: payload = cmd_add_job_pdu{thread_number, top(직렬화 옵션)}.
 * @return: 0 (응답 송신까지 항상 성공 취급 — 실제 결과는 UPDATE_JOB 응답의 에러 필드).
 *
 * tnumber 가 유효 범위를 벗어나면 ENODEV 응답. 아니면 tnumber_to_td 로 td 조회 후
 * convert_thread_options_to_cpu 로 네트워크 직렬화된 옵션을 CPU 구조체에 복사.
 */
static int handle_update_job_cmd(struct fio_net_cmd *cmd)
{
	struct cmd_add_job_pdu *pdu = (struct cmd_add_job_pdu *) cmd->payload;
	struct thread_data *td;
	uint32_t tnumber;
	int ret;

	tnumber = le32_to_cpu(pdu->thread_number);

	dprint(FD_NET, "server: updating options for job %u\n", tnumber);

	if (!tnumber || tnumber > thread_number) {
		send_update_job_reply(cmd->tag, ENODEV);
		return 0;
	}

	td = tnumber_to_td(tnumber);
	ret = convert_thread_options_to_cpu(&td->o, &pdu->top,
			cmd->pdu_len - offsetof(struct cmd_add_job_pdu, top));
	send_update_job_reply(cmd->tag, ret);
	return 0;
}

/*
 * [한국어]
 * handle_trigger_cmd - VTRIGGER 처리: 현재 I/O 상태 수집 후 트리거 명령 실행
 *
 * @cmd: payload = cmd_vtrigger_pdu{len, cmd[]} — 실행할 셸 명령.
 * @job_list: 현재 연결의 잡 프로세스 추적 리스트.
 * @return: 0.
 *
 * 동작:
 *   1) get_all_io_list(IO_LIST_ALL, &sz) — 모든 잡의 io_history 수집.
 *   2) VTRIGGER 응답 큐잉(SK_F_INLINE — 지연 없이 즉시 전송). 실패 시 빈 응답.
 *   3) fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL) — 모든 잡 종료 예약.
 *   4) fio_server_check_jobs — STOP/QUIT 송신 기회 부여.
 *   5) exec_trigger(buf) — fork+execvp 로 셸 명령 실행.
 *
 * 사용 목적: 재부팅/전원 차단 전에 I/O 상태를 저장해 재기동 후 검증.
 */
static int handle_trigger_cmd(struct fio_net_cmd *cmd, struct flist_head *job_list)
{
	struct cmd_vtrigger_pdu *pdu = (struct cmd_vtrigger_pdu *) cmd->payload;
	char *buf = (char *) pdu->cmd;
	struct all_io_list *rep;
	size_t sz;

	pdu->len = le16_to_cpu(pdu->len);
	buf[pdu->len] = '\0';

	rep = get_all_io_list(IO_LIST_ALL, &sz);
	if (!rep) {
		struct all_io_list state;

		state.threads = cpu_to_le64((uint64_t) 0);
		fio_net_queue_cmd(FIO_NET_CMD_VTRIGGER, &state, sizeof(state), NULL, SK_F_COPY | SK_F_INLINE);
	} else
		fio_net_queue_cmd(FIO_NET_CMD_VTRIGGER, rep, sz, NULL, SK_F_FREE | SK_F_INLINE);

	fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
	fio_server_check_jobs(job_list);
	exec_trigger(buf);
	return 0;
}

/*
 * [한국어]
 * handle_command - 수신 fio_net_cmd 를 opcode 별로 분기하는 메인 디스패처
 *
 * @sk_out: 이 연결의 소켓 상태 컨텍스트.
 * @job_list: 현재 연결에서 실행 중인 잡 프로세스 추적 리스트.
 * @cmd: 수신/검증 완료된 명령 (verify_convert_cmd 통과).
 * @return: 0 = 정상 처리 후 다음 명령 수신, >0 = 처리 실패(연결 유지),
 *          -1 = EXIT 수신(루프 탈출).
 *
 * 각 case 별 의미:
 *   QUIT        → fio_terminate_threads(TERMINATE_ALL) — 현재 잡만 종료(연결은 유지).
 *   EXIT        → exit_backend=true + -1 반환 — 서버 데몬 완전 종료 예약.
 *   LOAD_FILE   → handle_load_file_cmd — 서버 로컬 ini 경로 수신 → parse_jobs_ini.
 *   JOB         → handle_job_cmd — 클라 전송 ini 버퍼 → parse_jobs_ini (buf 형태).
 *   JOBLINE     → handle_jobline_cmd — CLI argv 재구성 → parse_cmd_line.
 *   PROBE       → handle_probe_cmd — OS/arch/cpus/zlib 교환.
 *   SEND_ETA    → handle_send_eta_cmd — jobs_eta 구조체 응답.
 *   RUN         → handle_run_cmd — fork + fio_backend(실제 I/O 실행).
 *   UPDATE_JOB  → handle_update_job_cmd — 실행 중 옵션 변경.
 *   VTRIGGER    → handle_trigger_cmd — verify 트리거 + exec_trigger.
 *   SENDFILE    → 인라인 처리 — verify state 파일 응답을 cmd_reply 세마포어로 전달.
 *   default     → 알 수 없는 opcode — 에러 반환(연결 유지).
 *
 * 호출 체인: handle_connection → fio_net_recv_cmd → handle_command → handle_*_cmd.
 * 컨텍스트: 서버 fork 자식 프로세스의 main thread. 단, RUN case 는 또 한 번 fork
 *           하여 자손 프로세스에서 fio_backend 실행.
 */
static int handle_command(struct sk_out *sk_out, struct flist_head *job_list,
			  struct fio_net_cmd *cmd)
{
	int ret;

	dprint(FD_NET, "server: got op [%s], pdu=%u, tag=%llx\n",
			fio_server_op(cmd->opcode), cmd->pdu_len,
			(unsigned long long) cmd->tag);

	switch (cmd->opcode) {
	case FIO_NET_CMD_QUIT:
		/* [한국어] 현재 실행 중인 모든 잡을 중단 — 연결은 유지(다음 명령 수신 가능) */
		fio_terminate_threads(TERMINATE_ALL, TERMINATE_ALL);
		ret = 0;
		break;
	case FIO_NET_CMD_EXIT:
		/* [한국어] 서버 데몬 완전 종료 — 모든 accept/poll 루프 탈출. 응답 송신 없이 -1 반환 */
		exit_backend = true;
		return -1;
	case FIO_NET_CMD_LOAD_FILE:
		/* [한국어] 서버 로컬 파일명 — 서버가 자체 파일시스템에서 ini 읽음 */
		ret = handle_load_file_cmd(cmd);
		break;
	case FIO_NET_CMD_JOB:
		/* [한국어] 클라이언트가 보낸 ini 버퍼 — 서버가 buffer 파싱 */
		ret = handle_job_cmd(cmd);
		break;
	case FIO_NET_CMD_JOBLINE:
		/* [한국어] 클라가 보낸 CLI argv — 재구성 후 parse_cmd_line */
		ret = handle_jobline_cmd(cmd);
		break;
	case FIO_NET_CMD_PROBE:
		/* [한국어] 서버 식별/특성 교환 — zlib 네고, me[] 설정 */
		ret = handle_probe_cmd(cmd);
		break;
	case FIO_NET_CMD_SEND_ETA:
		/* [한국어] 클라가 ETA 요청 — 서버가 jobs_eta 응답 */
		ret = handle_send_eta_cmd(cmd);
		break;
	case FIO_NET_CMD_RUN:
		/* [한국어] 잡 실행 시작 — fork 자식에서 fio_backend 호출 */
		ret = handle_run_cmd(sk_out, job_list, cmd);
		break;
	case FIO_NET_CMD_UPDATE_JOB:
		/* [한국어] 실행 중 옵션 변경 — convert_thread_options_to_cpu 적용 */
		ret = handle_update_job_cmd(cmd);
		break;
	case FIO_NET_CMD_VTRIGGER:
		/* [한국어] verify trigger — all_io_list 반환 + 잡 종료 + exec_trigger */
		ret = handle_trigger_cmd(cmd, job_list);
		break;
	case FIO_NET_CMD_SENDFILE: {
		/* [한국어] verify state 파일 응답 — 원래 요청자가 cmd_reply 세마포어로 대기 중.
		 *         cmd->tag 에 cmd_reply* 포인터가 인코딩되어 있음. */
		struct cmd_sendfile_reply *in;
		struct cmd_reply *rep;

		rep = (struct cmd_reply *) (uintptr_t) cmd->tag;  /* [한국어] tag → 포인터 복원 */

		in = (struct cmd_sendfile_reply *) cmd->payload;
		in->size = le32_to_cpu(in->size);    /* [한국어] 페이로드 길이 LE→CPU */
		in->error = le32_to_cpu(in->error);  /* [한국어] 에러 코드 LE→CPU */
		if (in->error) {
			ret = 1;
			rep->error = in->error;  /* [한국어] 대기자에게 에러 전달 */
		} else {
			ret = 0;
			rep->data = smalloc(in->size);  /* [한국어] SHM 힙 할당 (대기자가 소비 후 sfree) */
			if (!rep->data) {
				ret = 1;
				rep->error = ENOMEM;  /* [한국어] 메모리 부족 */
			} else {
				rep->size = in->size;
				memcpy(rep->data, in->data, in->size);  /* [한국어] 파일 내용 복사 */
			}
		}
		fio_sem_up(&rep->lock);  /* [한국어] 세마포어 올려 대기자 깨움 → fio_server_get_verify_state 복귀 */
		break;
		}
	default:
		/* [한국어] 알 수 없는 opcode — 버전 호환 문제 또는 프로토콜 버그 */
		log_err("fio: unknown opcode: %s\n", fio_server_op(cmd->opcode));
		ret = 1;
	}

	return ret;
}

/*
 * Send a command with a separate PDU, not inlined in the command
 */
/*
 * [한국어]
 * fio_send_cmd_ext_pdu - 외부 PDU 버퍼 버전 (헤더 + 별도 버퍼를 writev 2-way)
 *
 * @sk: 소켓 fd.
 * @opcode: FIO_NET_CMD_*.
 * @buf: 외부 페이로드(fio_net_send_cmd 와 달리 헤더와 분리).
 * @size: 페이로드 바이트 수.
 * @tag: 요청 태그.
 * @flags: 추가 FIO_NET_CMD_F_* 비트(F_MORE 자동 부여됨).
 * @return: 0 성공, 1 실패.
 *
 * writev iov[2]: iov[0]=헤더(스택), iov[1]=외부 버퍼 → zero-copy 로 하나의 writev 호출.
 * VEC 체인 송신에 사용 — 헤더를 매번 복사하지 않아도 되는 이점.
 */
static int fio_send_cmd_ext_pdu(int sk, uint16_t opcode, const void *buf,
				off_t size, uint64_t tag, uint32_t flags)
{
	struct fio_net_cmd cmd;
	struct iovec iov[2];
	size_t this_len;
	int ret;

	iov[0].iov_base = (void *) &cmd;
	iov[0].iov_len = sizeof(cmd);

	do {
		uint32_t this_flags = flags;

		this_len = size;
		if (this_len > FIO_SERVER_MAX_FRAGMENT_PDU)
			this_len = FIO_SERVER_MAX_FRAGMENT_PDU;

		if (this_len < size)
			this_flags |= FIO_NET_CMD_F_MORE;

		__fio_init_net_cmd(&cmd, opcode, this_len, tag);
		cmd.flags = __cpu_to_le32(this_flags);
		fio_net_cmd_crc_pdu(&cmd, buf);

		iov[1].iov_base = (void *) buf;
		iov[1].iov_len = this_len;

		ret = fio_sendv_data(sk, iov, 2);
		size -= this_len;
		buf += this_len;
	} while (!ret && size);

	return ret;
}

/*
 * [한국어]
 * finish_entry - 전송 완료된 sk_entry 의 버퍼+엔트리 해제
 *
 * @entry: 완료된 엔트리.
 * @return: 없음.
 *
 * 해제 방식 분기:
 *   SK_F_FREE → 일반 free (malloc 한 버퍼).
 *   SK_F_COPY → sfree (smalloc 힙).
 *   둘 다 아님 → 버퍼는 호출자 소유이므로 건드리지 않음.
 * entry 자체는 smalloc 으로 할당되었으므로 sfree.
 */
static void finish_entry(struct sk_entry *entry)
{
	if (entry->flags & SK_F_FREE)
		free(entry->buf);       /* [한국어] malloc 버퍼 */
	else if (entry->flags & SK_F_COPY)
		sfree(entry->buf);      /* [한국어] smalloc 복사본 */

	sfree(entry);  /* [한국어] 엔트리 자체도 smalloc 으로 할당됨 */
}

/*
 * [한국어]
 * entry_set_flags - 체인에 다음 엔트리가 있으면 FIO_NET_CMD_F_MORE 비트 세트
 *
 * @entry: 현재 엔트리(unused, 시그니처 유지).
 * @list: 남은 엔트리 체크용.
 * @flags: Out — 0 또는 F_MORE.
 */
static void entry_set_flags(struct sk_entry *entry, struct flist_head *list,
			    unsigned int *flags)
{
	if (!flist_empty(list))
		*flags = FIO_NET_CMD_F_MORE;  /* [한국어] 뒤에 더 있음 */
	else
		*flags = 0;
}

/*
 * [한국어]
 * send_vec_entry - SK_F_VEC 체인(first + first->next) 순차 전송
 *
 * @sk_out: 전송 대상.
 * @first: 헤더 엔트리 — first->next FIFO 에 추가 엔트리 연결.
 * @return: 전송 실패 엔트리 수 합산.
 *
 * 각 엔트리 송신 후 finish_entry 호출. 중간 실패해도 계속 진행해 free 는 보장.
 */
static int send_vec_entry(struct sk_out *sk_out, struct sk_entry *first)
{
	unsigned int flags;
	int ret;

	entry_set_flags(first, &first->next, &flags);

	ret = fio_send_cmd_ext_pdu(sk_out->sk, first->opcode, first->buf,
					first->size, first->tag, flags);

	while (!flist_empty(&first->next)) {
		struct sk_entry *next;

		next = flist_first_entry(&first->next, struct sk_entry, list);
		flist_del_init(&next->list);

		entry_set_flags(next, &first->next, &flags);

		ret += fio_send_cmd_ext_pdu(sk_out->sk, next->opcode, next->buf,
						next->size, next->tag, flags);
		finish_entry(next);
	}

	return ret;
}

/*
 * [한국어]
 * handle_sk_entry - 단일 sk_entry 를 소켓으로 전송 (xmit 세마포어로 직렬화)
 *
 * @sk_out: 소켓 상태.
 * @entry: 전송할 엔트리 (호출자 소유 — 여기서 finish_entry 로 해제).
 * @return: 0 성공, 1 실패.
 *
 * SK_F_VEC → send_vec_entry (헤더 + 체인 writev).
 * SK_F_SIMPLE → fio_net_send_simple_cmd (페이로드 없는 24B 헤더만).
 * 그 외 → fio_net_send_cmd (인라인 페이로드 버전).
 *
 * xmit 세마포어: 잡 스레드 다수가 동시에 writev 를 호출하면 헤더/페이로드 교차 섞임이
 * 발생할 수 있음 → xmit 으로 소켓 출력 원자성 보장.
 */
static int handle_sk_entry(struct sk_out *sk_out, struct sk_entry *entry)
{
	int ret;

	fio_sem_down(&sk_out->xmit);  /* [한국어] 전송 직렬화 락 획득 */

	if (entry->flags & SK_F_VEC)
		ret = send_vec_entry(sk_out, entry);  /* [한국어] 헤더+체인 writev */
	else if (entry->flags & SK_F_SIMPLE) {
		ret = fio_net_send_simple_cmd(sk_out->sk, entry->opcode,
						entry->tag, NULL);  /* [한국어] 24B 헤더만 */
	} else {
		ret = fio_net_send_cmd(sk_out->sk, entry->opcode, entry->buf,
					entry->size, &entry->tag, NULL);  /* [한국어] 인라인 페이로드 */
	}

	fio_sem_up(&sk_out->xmit);  /* [한국어] 전송 직렬화 락 해제 */

	if (ret)
		log_err("fio: failed handling cmd %s\n", fio_server_op(entry->opcode));

	finish_entry(entry);  /* [한국어] SK_F_FREE/COPY 에 따라 버퍼 회수 + entry sfree */
	return ret;
}

/*
 * [한국어]
 * handle_xmits - 전송 큐의 모든 대기 엔트리를 일괄 flush
 *
 * @sk_out: 전송 대상.
 * @return: 누적 반환값 (각 엔트리 실패 개수의 합).
 *
 * flist_splice_init 이디엄: 리스트 헤드를 로컬로 이동시켜 락 최소 구간만 유지.
 * 이후 로컬 리스트를 락 없이 순회하며 전송 — 다른 스레드의 동시 큐잉을 차단하지 않음.
 * 호출 체인: handle_connection poll 루프 + fio_net_queue_entry(SK_F_INLINE 아닌 경우는
 *           큐잉만 하고 flush 는 handle_xmits 에 위임) → handle_xmits → handle_sk_entry.
 */
static int handle_xmits(struct sk_out *sk_out)
{
	struct sk_entry *entry;
	FLIST_HEAD(list);  /* [한국어] 로컬 리스트(락 밖에서 순회) */
	int ret = 0;

	sk_lock(sk_out);
	if (flist_empty(&sk_out->list)) {
		sk_unlock(sk_out);
		return 0;  /* [한국어] 빠른 경로 — 큐 비어 있음 */
	}

	flist_splice_init(&sk_out->list, &list);  /* [한국어] 헤드 교체 + 원 리스트 비우기 */
	sk_unlock(sk_out);

	while (!flist_empty(&list)) {
		entry = flist_first_entry(&list, struct sk_entry, list);
		flist_del(&entry->list);
		ret += handle_sk_entry(sk_out, entry);  /* [한국어] 개별 전송 */
	}

	return ret;
}

/*
 * [한국어]
 * handle_connection - 단일 클라이언트 연결의 메인 처리 루프 (fork 자식의 main)
 *
 * @sk_out: 이 연결에 바인딩된 소켓 상태 구조체 (accept_loop 가 scalloc 으로 준비).
 * @return: _exit 으로 프로세스 종료하므로 의미 없음(반환 안 됨).
 *
 * 동작 단계:
 *   1) reset_fio_state() — 부모로부터 상속받은 fio 전역 상태 초기화.
 *   2) while (!exit_backend): 외부 루프 — 각 반복이 "1건 수신-처리".
 *       a) 내부 do-while: poll(sk, POLLIN, 0)+fio_sem_down_timeout 의 2단 대기.
 *          - poll 비블로킹 체크 + 이벤트 없으면 세마포어로 절전.
 *          - handle_xmits() 를 매 반복마다 호출 → 잡 스레드가 큐잉한 결과 flush.
 *          - 잡이 실행 중(job_list 비어있지 않음)이면 timeout 100ms 로 단축
 *            → 잡 종료 감지 및 STOP/QUIT 전송 지연 최소화.
 *       b) POLLIN 이벤트 → fio_net_recv_cmd (blocking, MSG_WAITALL) 로 완전 수신.
 *       c) handle_command 로 dispatch → handle_*_cmd.
 *       d) EXIT 수신(ret=-1) 또는 에러(ret>0) 이면 탈출.
 *   3) 연결 종료 경로: 남은 xmit 플러시 + close(sk) + sk_out 참조 드롭 + _exit.
 *
 * POLLERR/POLLHUP: peer 가 TCP RST/FIN 보내거나 socket 에러 — 연결 종료 사유.
 * 호출 체인: accept_loop fork 자식 → handle_connection → handle_command → 백엔드 → _exit.
 * 컨텍스트: fork 자식 프로세스의 main thread — 부모 주소공간 COW 복사.
 */
static int handle_connection(struct sk_out *sk_out)
{
	struct fio_net_cmd *cmd = NULL;
	FLIST_HEAD(job_list);   /* [한국어] 이 연결에서 실행 중인 잡 프로세스 리스트 */
	int ret = 0;

	reset_fio_state();  /* [한국어] 부모에서 상속된 fio 전역 상태 클리어 */

	/* read forever */
	while (!exit_backend) {
		struct pollfd pfd = {
			.fd	= sk_out->sk,
			.events	= POLLIN,  /* [한국어] 읽기 이벤트 감시 — POLLERR/POLLHUP 는 항상 revents 로 올라옴 */
		};

		do {
			int timeout = 1000;  /* [한국어] 기본 1초 — 잡 없을 때 절전 */

			if (!flist_empty(&job_list))
				timeout = 100;  /* [한국어] 잡 실행 중 — STOP/QUIT 지연 최소화 */

			handle_xmits(sk_out);  /* [한국어] 잡 스레드가 큐잉한 결과 flush */

			ret = poll(&pfd, 1, 0);  /* [한국어] 논블록 폴 — 이벤트 즉시 확인 */
			if (ret < 0) {
				if (errno == EINTR)
					break;  /* [한국어] 시그널 수신 — 상위 while 재평가 */
				log_err("fio: poll: %s\n", strerror(errno));
				break;
			} else if (!ret) {
				/* [한국어] 이벤트 없음 — 잡 상태 확인 + 세마포어로 timeout 대기 */
				fio_server_check_jobs(&job_list);
				fio_sem_down_timeout(&sk_out->wait, timeout);
				continue;
			}

			if (pfd.revents & POLLIN)
				break;  /* [한국어] 읽기 가능 — 상위에서 fio_net_recv_cmd 호출 */
			if (pfd.revents & (POLLERR|POLLHUP)) {
				/* [한국어] POLLERR = 소켓 에러, POLLHUP = peer 종료. 연결 재개 불가. */
				ret = 1;
				break;
			}
		} while (!exit_backend);

		fio_server_check_jobs(&job_list);  /* [한국어] 잡 종료 수거 + STOP/QUIT 큐잉 */

		if (ret < 0)
			break;  /* [한국어] poll 에러 — 루프 탈출 */

		if (pfd.revents & POLLIN)
			cmd = fio_net_recv_cmd(sk_out->sk, true);  /* [한국어] MSG_WAITALL 완전 수신 */
		if (!cmd) {
			ret = -1;  /* [한국어] 수신 실패 — 연결 종료 */
			break;
		}

		ret = handle_command(sk_out, &job_list, cmd);  /* [한국어] opcode 디스패치 */
		if (ret)
			break;  /* [한국어] EXIT(-1) 또는 에러(>0) — 탈출 */

		free(cmd);  /* [한국어] fio_net_recv_cmd 가 malloc 했음 */
		cmd = NULL;
	}

	if (cmd)
		free(cmd);  /* [한국어] 에러 경로에서 남은 cmd 해제 */

	handle_xmits(sk_out);  /* [한국어] 마지막으로 큐에 남은 것 flush(베스트에포트) */

	close(sk_out->sk);      /* [한국어] 소켓 fd 반환 — TCP FIN 전송 */
	sk_out->sk = -1;        /* [한국어] 이후 송신 시도는 "이미 닫힘" 체크로 차단 */
	__sk_out_drop(sk_out);  /* [한국어] 자식 측 참조 해제 — refs 0 이면 해제 */
	_exit(ret);             /* [한국어] 자식 프로세스 즉시 종료 (atexit/stdio 플러시 생략) */
}

/* get the address on this host bound by the input socket,
 * whether it is ipv6 or ipv4 */
/*
 * [한국어]
 * get_my_addr_str - accept 된 소켓의 로컬 bind 주소를 문자열로 변환
 *
 * @sk: accept 로 받은 클라이언트 소켓.
 * @return: 0 성공(client_sockaddr_str 전역에 저장), -1 실패.
 *
 * getsockname(2): 소켓이 바인드된 로컬 주소 조회 — 서버가 어느 인터페이스로
 *                 연결을 받았는지 확인 (verify state gen_name 에 사용).
 */
static int get_my_addr_str(int sk)
{
	struct sockaddr_in6 myaddr6 = { 0, };
	struct sockaddr_in myaddr4 = { 0, };
	struct sockaddr *sockaddr_p;
	char *net_addr;
	socklen_t len;
	int ret;

	if (use_ipv6) {
		len = sizeof(myaddr6);
		sockaddr_p = (struct sockaddr * )&myaddr6;
		net_addr = (char * )&myaddr6.sin6_addr;
	} else {
		len = sizeof(myaddr4);
		sockaddr_p = (struct sockaddr * )&myaddr4;
		net_addr = (char * )&myaddr4.sin_addr;
	}

	ret = getsockname(sk, sockaddr_p, &len);
	if (ret) {
		log_err("fio: getsockname: %s\n", strerror(errno));
		return -1;
	}

	if (!inet_ntop(use_ipv6?AF_INET6:AF_INET, net_addr, client_sockaddr_str, INET6_ADDRSTRLEN - 1)) {
		log_err("inet_ntop: failed to convert addr to string\n");
		return -1;
	}

	dprint(FD_NET, "fio server bound to addr %s\n", client_sockaddr_str);
	return 0;
}

#ifdef WIN32
/*
 * [한국어]
 * handle_connection_process (Windows) - 자식 프로세스 엔트리: 파이프로 소켓 복제 받고 연결 처리
 *
 * @return: handle_connection 반환값 또는 -1 실패.
 *
 * 동작:
 *   1) 부모가 만든 명명된 파이프(fio_server_pipe_name) 에 CreateFile 로 연결.
 *   2) ReadFile 로 WSAPROTOCOL_INFO 수신.
 *   3) WSASocket 으로 부모 소켓 핸들을 자식에 복제.
 *   4) sk_out 생성 후 handle_connection 진입.
 *
 * Windows 에서 fork 가 없어서 socket fd 를 자식으로 전달할 수 없으므로
 * 명명된 파이프 + WSAPROTOCOL_INFO 조합으로 대체.
 */
static int handle_connection_process(void)
{
	WSAPROTOCOL_INFO protocol_info;
	DWORD bytes_read;
	HANDLE hpipe;
	int sk;
	struct sk_out *sk_out;
	int ret;
	char *msg = (char *) "connected";

	log_info("server enter accept loop.  ProcessID %d\n", GetCurrentProcessId());

	hpipe = CreateFile(
					fio_server_pipe_name,
					GENERIC_READ | GENERIC_WRITE,
					0, NULL,
					OPEN_EXISTING,
					0, NULL);

	if (hpipe == INVALID_HANDLE_VALUE) {
		log_err("couldnt open pipe %s error %lu\n",
				fio_server_pipe_name, GetLastError());
		return -1;
	}

	if (!ReadFile(hpipe, &protocol_info, sizeof(protocol_info), &bytes_read, NULL)) {
		log_err("couldnt read pi from pipe %s error %lu\n", fio_server_pipe_name,
				GetLastError());
	}

	if (use_ipv6) /* use protocol_info to create a duplicate of parents socket */
		sk = WSASocket(AF_INET6, SOCK_STREAM, 0, &protocol_info, 0, 0);
	else
		sk = WSASocket(AF_INET,  SOCK_STREAM, 0, &protocol_info, 0, 0);

	sk_out = scalloc(1, sizeof(*sk_out));
	if (!sk_out) {
		CloseHandle(hpipe);
		close(sk);
		return -1;
	}

	sk_out->sk = sk;
	sk_out->hProcess = INVALID_HANDLE_VALUE;
	INIT_FLIST_HEAD(&sk_out->list);
	__fio_sem_init(&sk_out->lock, FIO_SEM_UNLOCKED);
	__fio_sem_init(&sk_out->wait, FIO_SEM_LOCKED);
	__fio_sem_init(&sk_out->xmit, FIO_SEM_UNLOCKED);

	get_my_addr_str(sk);

	if (!WriteFile(hpipe, msg, strlen(msg), NULL, NULL)) {
		log_err("couldnt write pipe\n");
		close(sk);
		return -1;
	}
	CloseHandle(hpipe);

	sk_out_assign(sk_out);

	ret = handle_connection(sk_out);
	__sk_out_drop(sk_out);
	return ret;
}
#endif

/*
 * [한국어]
 * accept_loop - 서버 리슨 소켓에서 클라이언트 연결을 수락하고 처리 프로세스 생성
 *
 * @listen_sk: listen(2) 완료된 소켓 fd.
 * @return: 정상 종료 시 exitval (0 고정), 치명 에러 시 -1.
 *
 * 루프 구조:
 *   1) listen_sk 를 O_NONBLOCK 으로 설정 — poll 과 accept 가 블록되지 않도록.
 *   2) while (!exit_backend):
 *      a) poll(listen_sk, POLLIN, timeout=1000ms 또는 conn 있으면 100ms).
 *      b) 이벤트 없으면 fio_server_check_conns — 종료된 자식 프로세스 회수.
 *      c) accept(listen_sk, &addr) → 클라이언트 소켓 sk 획득.
 *      d) sk_out 구조체(scalloc — SHM 공유) 준비 + 세마포어 3개 초기화.
 *      e) POSIX: fork() — 부모는 sk 닫고 conn_list 에 자식 PID 추가, 자식은
 *         handle_connection(sk_out) 으로 진입(내부에서 _exit).
 *         Windows: windows_handle_connection + 명명된 파이프로 socket 핸들 복제.
 *
 * conn_list 추적: 매 연결마다 자식 PID 를 저장 → 주기적으로 waitpid(WNOHANG) 로
 *                회수해야 zombie 방지.
 * 호출 체인: fio_server → fio_init_server_connection → accept_loop → accept →
 *           fork → handle_connection.
 * 컨텍스트: 서버 메인 프로세스의 메인 스레드.
 * 에러 경로: accept 실패 시 즉시 -1 반환(서버 중단). sk_out 할당 실패 시
 *           연결만 거절.
 */
static int accept_loop(int listen_sk)
{
	struct sockaddr_in addr;
	struct sockaddr_in6 addr6;
	socklen_t len = use_ipv6 ? sizeof(addr6) : sizeof(addr);
	struct pollfd pfd;
	int ret = 0, sk, exitval = 0;
	FLIST_HEAD(conn_list);  /* [한국어] 연결 처리 자식 프로세스 추적 리스트 */

	dprint(FD_NET, "server enter accept loop\n");

	fio_set_fd_nonblocking(listen_sk, "server");
	/* [한국어] O_NONBLOCK — poll 이벤트 없을 때 accept 가 블록 안 됨 */

	while (!exit_backend) {
		struct sk_out *sk_out;
		const char *from;
		char buf[64];
#ifdef WIN32
		HANDLE hProcess;
#else
		pid_t pid;
#endif
		pfd.fd = listen_sk;
		pfd.events = POLLIN;  /* [한국어] 새 연결 요청 = 읽기 이벤트 */
		do {
			int timeout = 1000;

			if (!flist_empty(&conn_list))
				timeout = 100;  /* [한국어] 자식 프로세스 있으면 짧은 주기로 waitpid */

			ret = poll(&pfd, 1, timeout);
			if (ret < 0) {
				if (errno == EINTR)
					break;  /* [한국어] 시그널 — 상위 while 재평가 */
				log_err("fio: poll: %s\n", strerror(errno));
				break;
			} else if (!ret) {
				fio_server_check_conns(&conn_list);  /* [한국어] 타임아웃 — zombie 회수 */
				continue;
			}

			if (pfd.revents & POLLIN)
				break;  /* [한국어] 새 연결 대기 — accept 진행 */
		} while (!exit_backend);

		fio_server_check_conns(&conn_list);  /* [한국어] 연결 자식은 STOP/QUIT 안 보냄 */

		if (exit_backend || ret < 0)
			break;  /* [한국어] 종료 신호 또는 poll 에러 */

		/* [한국어] accept — 커널 3-way handshake 완료 소켓을 꺼냄 */
		if (use_ipv6)
			sk = accept(listen_sk, (struct sockaddr *) &addr6, &len);
		else
			sk = accept(listen_sk, (struct sockaddr *) &addr, &len);

		if (sk < 0) {
			log_err("fio: accept: %s\n", strerror(errno));
			return -1;
		}

		/* [한국어] 접속자 주소 문자열로 변환 — 디버그 로그용 */
		if (use_ipv6)
			from = inet_ntop(AF_INET6, (struct sockaddr *) &addr6.sin6_addr, buf, sizeof(buf));
		else
			from = inet_ntop(AF_INET, (struct sockaddr *) &addr.sin_addr, buf, sizeof(buf));

		dprint(FD_NET, "server: connect from %s\n", from);

		sk_out = scalloc(1, sizeof(*sk_out));
		/* [한국어] SHM 힙에 할당 — fork 자식과 공유 가능(참조 카운트 관리) */
		if (!sk_out) {
			close(sk);
			return -1;
		}

		sk_out->sk = sk;
		INIT_FLIST_HEAD(&sk_out->list);
		__fio_sem_init(&sk_out->lock, FIO_SEM_UNLOCKED);  /* [한국어] 리스트 보호 (뮤텍스 역할) */
		__fio_sem_init(&sk_out->wait, FIO_SEM_LOCKED);    /* [한국어] 대기 세마포어 — 잡업 도착 시 up */
		__fio_sem_init(&sk_out->xmit, FIO_SEM_UNLOCKED);  /* [한국어] 전송 직렬화 세마포어 */

#ifdef WIN32
		/* [한국어] Windows: fork 대체 — CreateProcess + 명명된 파이프로 WSAPROTOCOL_INFO 전달 */
		hProcess = windows_handle_connection(hjob, sk);
		if (hProcess == INVALID_HANDLE_VALUE)
			return -1;
		sk_out->hProcess = hProcess;
		fio_server_add_conn_pid(&conn_list, hProcess);
#else
		pid = fork();  /* [한국어] POSIX: 자식 프로세스 생성 — 주소공간 COW */
		if (pid) {
			/* [한국어] 부모 경로: 자식이 처리하므로 sk 닫고 추적 리스트에 PID 추가 */
			close(sk);
			fio_server_add_conn_pid(&conn_list, pid);
			continue;
		}

		/* [한국어] 자식 경로: sk_out 을 이 스레드에 바인딩 후 handle_connection 진입(_exit 로 종료) */
		/* if error, it's already logged, non-fatal */
		get_my_addr_str(sk);

		/*
		 * Assign sk_out here, it'll be dropped in handle_connection()
		 * since that function calls _exit() when done
		 */
		sk_out_assign(sk_out);
		handle_connection(sk_out);  /* [한국어] 내부에서 _exit(ret) — 반환 안 됨 */
#endif
	}

	return exitval;
}

/*
 * [한국어]
 * fio_server_text_output - 서버→클라 텍스트 로그 전송 (log_info/log_err 리다이렉트)
 *
 * @level: 로그 레벨 (0=info, 1=err 등).
 * @buf: 로그 문자열(널 종결 아닐 수 있음 — len 사용).
 * @len: 바이트 수.
 * @return: 성공 시 len, sk_out 없음/소켓 닫힘 시 -1.
 *
 * 호출 체인: 잡 스레드의 log_info/log_err 매크로 → fio_server_text_output →
 *           fio_net_queue_cmd(TEXT, pdu, SK_F_COPY) → handle_xmits → writev.
 * 실행 컨텍스트: 잡 스레드 — sk_out 이 pthread TLS 로 바인딩되어 있어야 함.
 * 타임스탬프: gettimeofday 로 log_sec/log_usec 기록 — 클라 출력 시 정렬.
 */
int fio_server_text_output(int level, const char *buf, size_t len)
{
	struct sk_out *sk_out = pthread_getspecific(sk_out_key);
	struct cmd_text_pdu *pdu;
	unsigned int tlen;
	struct timeval tv;

	if (!sk_out || sk_out->sk == -1)
		return -1;

	tlen = sizeof(*pdu) + len;
	pdu = malloc(tlen);

	pdu->level	= __cpu_to_le32(level);
	pdu->buf_len	= __cpu_to_le32(len);

	gettimeofday(&tv, NULL);
	pdu->log_sec	= __cpu_to_le64(tv.tv_sec);
	pdu->log_usec	= __cpu_to_le64(tv.tv_usec);

	memcpy(pdu->buf, buf, len);

	fio_net_queue_cmd(FIO_NET_CMD_TEXT, pdu, tlen, NULL, SK_F_COPY);
	free(pdu);
	return len;
}

/*
 * [한국어]
 * convert_io_stat - io_stat (max/min/samples/mean/S) 를 LE+IEEE754 로 직렬화
 *
 * @dst: 출력 — 네트워크 바이트 오더 결과.
 * @src: 입력 — CPU 바이트 오더 원본.
 * @return: 없음.
 *
 * mean/S 는 double 이므로 fio_double_to_uint64 로 비트 패턴 유지 후 LE64 변환.
 * 이 방식은 FPU 구현 차이에 영향받지 않고 비트 수준에서 정확히 복원 가능.
 */
static void convert_io_stat(struct io_stat *dst, struct io_stat *src)
{
	dst->max_val	= cpu_to_le64(src->max_val);
	dst->min_val	= cpu_to_le64(src->min_val);
	dst->samples	= cpu_to_le64(src->samples);

	/*
	 * Encode to IEEE 754 for network transfer
	 */
	dst->mean.u.i	= cpu_to_le64(fio_double_to_uint64(src->mean.u.f));
	dst->S.u.i	= cpu_to_le64(fio_double_to_uint64(src->S.u.f));
}

/*
 * [한국어]
 * convert_gs - group_run_stats 필드 전체를 LE 로 직렬화
 *
 * DDIR_RWDIR_CNT = 3 방향(READ/WRITE/TRIM) 각각의 max_run/min_run/max_bw/min_bw/
 * iobytes/agg 배열을 LE64 로 세트. 스칼라는 LE32.
 */
static void convert_gs(struct group_run_stats *dst, struct group_run_stats *src)
{
	int i;

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		dst->max_run[i]		= cpu_to_le64(src->max_run[i]);
		dst->min_run[i]		= cpu_to_le64(src->min_run[i]);
		dst->max_bw[i]		= cpu_to_le64(src->max_bw[i]);
		dst->min_bw[i]		= cpu_to_le64(src->min_bw[i]);
		dst->iobytes[i]		= cpu_to_le64(src->iobytes[i]);
		dst->agg[i]		= cpu_to_le64(src->agg[i]);
	}

	dst->kb_base	= cpu_to_le32(src->kb_base);
	dst->unit_base	= cpu_to_le32(src->unit_base);
	dst->groupid	= cpu_to_le32(src->groupid);
	dst->unified_rw_rep	= cpu_to_le32(src->unified_rw_rep);
	dst->sig_figs	= cpu_to_le32(src->sig_figs);
}

/*
 * Send a CMD_TS, which packs struct thread_stat and group_run_stats
 * into a single payload.
 */
/*
 * [한국어]
 * fio_server_send_ts - 잡 종료 통계를 FIO_NET_CMD_TS 로 클라에 전송
 *
 * @ts: thread_stat — lat/bw/iops 히스토그램 + 누적 카운터.
 * @rs: group_run_stats — 그룹 단위 집계.
 * @return: 없음.
 *
 * 구조: cmd_ts_pdu{ts, rs} 단일 구조체로 기본 패킹.
 *       + clat_prio 통계가 있으면 확장 버퍼 뒤쪽에 clat_prio_stat[] 배열 append.
 *       + steadystate 활성화 시 iops/bw/lat 링버퍼 3개 (각 ss_dur 길이) append.
 * 모든 정수 필드 LE 로 변환, double 은 fio_double_to_uint64 로 IEEE 754 비트 보존.
 * 호출 체인: stat.c show_run_stats → fio_server_send_ts → fio_net_queue_cmd.
 */
void fio_server_send_ts(struct thread_stat *ts, struct group_run_stats *rs)
{
	struct cmd_ts_pdu p;
	int i, j, k;
	size_t clat_prio_stats_extra_size = 0;
	size_t ss_extra_size = 0;
	size_t extended_buf_size = 0;
	void *extended_buf;
	void *extended_buf_wp;

	dprint(FD_NET, "server sending end stats\n");

	memset(&p, 0, sizeof(p));

	snprintf(p.ts.name, sizeof(p.ts.name), "%s", ts->name);
	snprintf(p.ts.verror, sizeof(p.ts.verror), "%s", ts->verror);
	snprintf(p.ts.description, sizeof(p.ts.description), "%s",
		 ts->description);

	p.ts.error		= cpu_to_le32(ts->error);
	p.ts.thread_number	= cpu_to_le32(ts->thread_number);
	p.ts.groupid		= cpu_to_le32(ts->groupid);
	p.ts.job_start		= cpu_to_le64(ts->job_start);
	p.ts.pid		= cpu_to_le32(ts->pid);
	p.ts.members		= cpu_to_le32(ts->members);
	p.ts.unified_rw_rep	= cpu_to_le32(ts->unified_rw_rep);
	p.ts.ioprio		= cpu_to_le32(ts->ioprio);
	p.ts.disable_prio_stat	= cpu_to_le32(ts->disable_prio_stat);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		convert_io_stat(&p.ts.clat_stat[i], &ts->clat_stat[i]);
		convert_io_stat(&p.ts.slat_stat[i], &ts->slat_stat[i]);
		convert_io_stat(&p.ts.lat_stat[i], &ts->lat_stat[i]);
		convert_io_stat(&p.ts.bw_stat[i], &ts->bw_stat[i]);
		convert_io_stat(&p.ts.iops_stat[i], &ts->iops_stat[i]);
	}
	convert_io_stat(&p.ts.sync_stat, &ts->sync_stat);

	p.ts.usr_time		= cpu_to_le64(ts->usr_time);
	p.ts.sys_time		= cpu_to_le64(ts->sys_time);
	p.ts.ctx		= cpu_to_le64(ts->ctx);
	p.ts.minf		= cpu_to_le64(ts->minf);
	p.ts.majf		= cpu_to_le64(ts->majf);
	p.ts.clat_percentiles	= cpu_to_le32(ts->clat_percentiles);
	p.ts.lat_percentiles	= cpu_to_le32(ts->lat_percentiles);
	p.ts.slat_percentiles	= cpu_to_le32(ts->slat_percentiles);
	p.ts.percentile_precision = cpu_to_le64(ts->percentile_precision);

	for (i = 0; i < FIO_IO_U_LIST_MAX_LEN; i++) {
		fio_fp64_t *src = &ts->percentile_list[i];
		fio_fp64_t *dst = &p.ts.percentile_list[i];

		dst->u.i = cpu_to_le64(fio_double_to_uint64(src->u.f));
	}

	for (i = 0; i < FIO_IO_U_MAP_NR; i++) {
		p.ts.io_u_map[i]	= cpu_to_le64(ts->io_u_map[i]);
		p.ts.io_u_submit[i]	= cpu_to_le64(ts->io_u_submit[i]);
		p.ts.io_u_complete[i]	= cpu_to_le64(ts->io_u_complete[i]);
	}

	for (i = 0; i < FIO_IO_U_LAT_N_NR; i++)
		p.ts.io_u_lat_n[i]	= cpu_to_le64(ts->io_u_lat_n[i]);
	for (i = 0; i < FIO_IO_U_LAT_U_NR; i++)
		p.ts.io_u_lat_u[i]	= cpu_to_le64(ts->io_u_lat_u[i]);
	for (i = 0; i < FIO_IO_U_LAT_M_NR; i++)
		p.ts.io_u_lat_m[i]	= cpu_to_le64(ts->io_u_lat_m[i]);

	for (i = 0; i < FIO_LAT_CNT; i++)
		for (j = 0; j < DDIR_RWDIR_CNT; j++)
			for (k = 0; k < FIO_IO_U_PLAT_NR; k++)
				p.ts.io_u_plat[i][j][k] = cpu_to_le64(ts->io_u_plat[i][j][k]);

	for (j = 0; j < FIO_IO_U_PLAT_NR; j++)
		p.ts.io_u_sync_plat[j] = cpu_to_le64(ts->io_u_sync_plat[j]);

	for (i = 0; i < DDIR_RWDIR_SYNC_CNT; i++)
		p.ts.total_io_u[i]	= cpu_to_le64(ts->total_io_u[i]);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		p.ts.short_io_u[i]	= cpu_to_le64(ts->short_io_u[i]);
		p.ts.drop_io_u[i]	= cpu_to_le64(ts->drop_io_u[i]);
	}

	p.ts.total_submit	= cpu_to_le64(ts->total_submit);
	p.ts.total_complete	= cpu_to_le64(ts->total_complete);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		p.ts.io_bytes[i]	= cpu_to_le64(ts->io_bytes[i]);
		p.ts.runtime[i]		= cpu_to_le64(ts->runtime[i]);
	}

	p.ts.total_run_time	= cpu_to_le64(ts->total_run_time);
	p.ts.continue_on_error	= cpu_to_le16(ts->continue_on_error);
	p.ts.total_err_count	= cpu_to_le64(ts->total_err_count);
	p.ts.first_error	= cpu_to_le32(ts->first_error);
	p.ts.kb_base		= cpu_to_le32(ts->kb_base);
	p.ts.unit_base		= cpu_to_le32(ts->unit_base);

	p.ts.nr_zone_resets	= cpu_to_le64(ts->nr_zone_resets);
	p.ts.count_zone_resets	= cpu_to_le16(ts->count_zone_resets);

	p.ts.latency_depth	= cpu_to_le32(ts->latency_depth);
	p.ts.latency_target	= cpu_to_le64(ts->latency_target);
	p.ts.latency_window	= cpu_to_le64(ts->latency_window);
	p.ts.latency_percentile.u.i = cpu_to_le64(fio_double_to_uint64(ts->latency_percentile.u.f));

	p.ts.sig_figs		= cpu_to_le32(ts->sig_figs);

	p.ts.nr_block_infos	= cpu_to_le64(ts->nr_block_infos);
	for (i = 0; i < p.ts.nr_block_infos; i++)
		p.ts.block_infos[i] = cpu_to_le32(ts->block_infos[i]);

	p.ts.ss_dur		= cpu_to_le64(ts->ss_dur);
	p.ts.ss_state		= cpu_to_le32(ts->ss_state);
	p.ts.ss_head		= cpu_to_le32(ts->ss_head);
	p.ts.ss_limit.u.i	= cpu_to_le64(fio_double_to_uint64(ts->ss_limit.u.f));
	p.ts.ss_slope.u.i	= cpu_to_le64(fio_double_to_uint64(ts->ss_slope.u.f));
	p.ts.ss_deviation.u.i	= cpu_to_le64(fio_double_to_uint64(ts->ss_deviation.u.f));
	p.ts.ss_criterion.u.i	= cpu_to_le64(fio_double_to_uint64(ts->ss_criterion.u.f));

	p.ts.cachehit		= cpu_to_le64(ts->cachehit);
	p.ts.cachemiss		= cpu_to_le64(ts->cachemiss);

	convert_gs(&p.rs, rs);

	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		if (ts->nr_clat_prio[i])
			clat_prio_stats_extra_size += ts->nr_clat_prio[i] * sizeof(*ts->clat_prio[i]);
	}
	extended_buf_size += clat_prio_stats_extra_size;

	dprint(FD_NET, "ts->ss_state = %d\n", ts->ss_state);
	if (ts->ss_state & FIO_SS_DATA)
		ss_extra_size = 3 * ts->ss_dur * sizeof(uint64_t);

	extended_buf_size += ss_extra_size;
	if (!extended_buf_size) {
		fio_net_queue_cmd(FIO_NET_CMD_TS, &p, sizeof(p), NULL, SK_F_COPY);
		return;
	}

	extended_buf_size += sizeof(p);
	extended_buf = calloc(1, extended_buf_size);
	if (!extended_buf) {
		log_err("fio: failed to allocate FIO_NET_CMD_TS buffer\n");
		return;
	}

	memcpy(extended_buf, &p, sizeof(p));
	extended_buf_wp = (struct cmd_ts_pdu *)extended_buf + 1;

	if (clat_prio_stats_extra_size) {
		for (i = 0; i < DDIR_RWDIR_CNT; i++) {
			struct clat_prio_stat *prio = (struct clat_prio_stat *) extended_buf_wp;

			for (j = 0; j < ts->nr_clat_prio[i]; j++) {
				for (k = 0; k < FIO_IO_U_PLAT_NR; k++)
					prio->io_u_plat[k] =
						cpu_to_le64(ts->clat_prio[i][j].io_u_plat[k]);
				convert_io_stat(&prio->clat_stat,
						&ts->clat_prio[i][j].clat_stat);
				prio->ioprio = cpu_to_le32(ts->clat_prio[i][j].ioprio);
				prio++;
			}

			if (ts->nr_clat_prio[i]) {
				uint64_t offset = (char *)extended_buf_wp - (char *)extended_buf;
				struct cmd_ts_pdu *ptr = extended_buf;

				ptr->ts.clat_prio_offset[i] = cpu_to_le64(offset);
				ptr->ts.nr_clat_prio[i] = cpu_to_le32(ts->nr_clat_prio[i]);
			}

			extended_buf_wp = prio;
		}
	}

	if (ss_extra_size) {
		uint64_t *ss_iops, *ss_bw, *ss_lat;
		uint64_t offset;
		struct cmd_ts_pdu *ptr = extended_buf;

		dprint(FD_NET, "server sending steadystate ring buffers\n");

		/* ss iops */
		ss_iops = (uint64_t *) extended_buf_wp;
		for (i = 0; i < ts->ss_dur; i++)
			ss_iops[i] = cpu_to_le64(ts->ss_iops_data[i]);

		offset = (char *)extended_buf_wp - (char *)extended_buf;
		ptr->ts.ss_iops_data_offset = cpu_to_le64(offset);
		extended_buf_wp = ss_iops + (int) ts->ss_dur;

		/* ss bw */
		ss_bw = extended_buf_wp;
		for (i = 0; i < ts->ss_dur; i++)
			ss_bw[i] = cpu_to_le64(ts->ss_bw_data[i]);

		offset = (char *)extended_buf_wp - (char *)extended_buf;
		ptr->ts.ss_bw_data_offset = cpu_to_le64(offset);
		extended_buf_wp = ss_bw + (int) ts->ss_dur;

		/* ss lat */
		ss_lat = extended_buf_wp;
		for (i = 0; i < ts->ss_dur; i++)
			ss_lat[i] = cpu_to_le64(ts->ss_lat_data[i]);

		offset = (char *)extended_buf_wp - (char *)extended_buf;
		ptr->ts.ss_lat_data_offset = cpu_to_le64(offset);
	}

	fio_net_queue_cmd(FIO_NET_CMD_TS, extended_buf, extended_buf_size, NULL, SK_F_COPY);
	free(extended_buf);
}

/*
 * [한국어]
 * fio_server_send_gs - 그룹 실행 통계(group_run_stats) 단독 전송
 *
 * @rs: 원본 통계 구조체.
 * @return: 없음.
 *
 * 용도: fio_server_send_ts 가 ts 에 rs 를 내장해 전송하는 경우 외에, 그룹 단위
 *       집계만 따로 전송해야 할 때 사용.
 * 호출 체인: stat.c show_group_stats → fio_server_send_gs.
 */
void fio_server_send_gs(struct group_run_stats *rs)
{
	struct group_run_stats gs;  /* [한국어] 스택 변환 버퍼 — LE 직렬화 전 임시 저장 */

	dprint(FD_NET, "server sending group run stats\n");

	convert_gs(&gs, rs);  /* [한국어] 각 필드를 LE 로 직렬화 (min/max/agg/bw) */
	fio_net_queue_cmd(FIO_NET_CMD_GS, &gs, sizeof(gs), NULL, SK_F_COPY);
}

/*
 * [한국어]
 * fio_server_send_job_options - print_option 리스트를 (key, value) 쌍으로 전송
 *
 * @opt_list: print_option entries 리스트.
 * @gid: 그룹 ID. -1U 면 전역 옵션, 그 외면 groupid.
 * @return: 없음.
 *
 * 각 옵션마다 FIO_NET_CMD_JOB_OPT 큐잉. 이름/값이 cmd_job_option 버퍼 크기를 초과하면
 * truncated 플래그로 표시 (클라가 사람이 볼 때만 사용하므로 허용).
 */
void fio_server_send_job_options(struct flist_head *opt_list,
				 unsigned int gid)
{
	struct cmd_job_option pdu;
	struct flist_head *entry;

	if (flist_empty(opt_list))
		return;

	flist_for_each(entry, opt_list) {
		struct print_option *p;
		size_t len;

		p = flist_entry(entry, struct print_option, list);
		memset(&pdu, 0, sizeof(pdu));

		if (gid == -1U) {
			pdu.global = __cpu_to_le16(1);
			pdu.groupid = 0;
		} else {
			pdu.global = 0;
			pdu.groupid = cpu_to_le32(gid);
		}
		len = strlen(p->name);
		if (len >= sizeof(pdu.name)) {
			len = sizeof(pdu.name) - 1;
			pdu.truncated = __cpu_to_le16(1);
		}
		memcpy(pdu.name, p->name, len);
		if (p->value) {
			len = strlen(p->value);
			if (len >= sizeof(pdu.value)) {
				len = sizeof(pdu.value) - 1;
				pdu.truncated = __cpu_to_le16(1);
			}
			memcpy(pdu.value, p->value, len);
		}
		fio_net_queue_cmd(FIO_NET_CMD_JOB_OPT, &pdu, sizeof(pdu), NULL, SK_F_COPY);
	}
}

/*
 * [한국어]
 * convert_agg - disk_util_agg 를 LE 로 직렬화 (슬레이브 디스크 집계)
 *
 * [0]=read, [1]=write 2 원소 배열들의 ios/merges/sectors/ticks 각 LE64 저장.
 * max_util(double) 은 IEEE 754 비트 패턴 유지.
 */
static void convert_agg(struct disk_util_agg *dst, struct disk_util_agg *src)
{
	int i;

	for (i = 0; i < 2; i++) {
		dst->ios[i]	= cpu_to_le64(src->ios[i]);
		dst->merges[i]	= cpu_to_le64(src->merges[i]);
		dst->sectors[i]	= cpu_to_le64(src->sectors[i]);
		dst->ticks[i]	= cpu_to_le64(src->ticks[i]);
	}

	dst->io_ticks		= cpu_to_le64(src->io_ticks);
	dst->time_in_queue	= cpu_to_le64(src->time_in_queue);
	dst->slavecount		= cpu_to_le32(src->slavecount);
	dst->max_util.u.i	= cpu_to_le64(fio_double_to_uint64(src->max_util.u.f));
}

/*
 * [한국어]
 * convert_dus - disk_util_stat(단일 디스크의 /proc/diskstats 기반 통계) LE 직렬화
 *
 * name 문자열 그대로 복사(문자열은 바이트 오더 무관). ios/merges/sectors/ticks/
 * io_ticks/time_in_queue/msec 전부 LE64.
 */
static void convert_dus(struct disk_util_stat *dst, struct disk_util_stat *src)
{
	int i;

	snprintf((char *) dst->name, sizeof(dst->name), "%s", src->name);

	for (i = 0; i < 2; i++) {
		dst->s.ios[i]		= cpu_to_le64(src->s.ios[i]);
		dst->s.merges[i]	= cpu_to_le64(src->s.merges[i]);
		dst->s.sectors[i]	= cpu_to_le64(src->s.sectors[i]);
		dst->s.ticks[i]		= cpu_to_le64(src->s.ticks[i]);
	}

	dst->s.io_ticks		= cpu_to_le64(src->s.io_ticks);
	dst->s.time_in_queue	= cpu_to_le64(src->s.time_in_queue);
	dst->s.msec		= cpu_to_le64(src->s.msec);
}

/*
 * [한국어]
 * fio_server_send_du - 모든 디스크의 유틸리티 통계(disk_util_stat+agg)를 클라로 전송
 *
 * @return: 없음.
 *
 * disk_list 전역 리스트(diskutil.c 관리) 를 순회하며 각 디스크 엔트리마다
 * cmd_du_pdu{dus(통계), agg(슬레이브 합)} 를 FIO_NET_CMD_DU 로 큐잉.
 * 호출 체인: stat.c show_disk_util → fio_server_send_du → flist_for_each(disk_list).
 */
void fio_server_send_du(void)
{
	struct disk_util *du;
	struct flist_head *entry;
	struct cmd_du_pdu pdu;

	dprint(FD_NET, "server: sending disk_util %d\n", !flist_empty(&disk_list));

	memset(&pdu, 0, sizeof(pdu));

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);

		convert_dus(&pdu.dus, &du->dus);
		convert_agg(&pdu.agg, &du->agg);

		fio_net_queue_cmd(FIO_NET_CMD_DU, &pdu, sizeof(pdu), NULL, SK_F_COPY);
	}
}

/* [한국어] === zlib 압축 전송 함수군 (CONFIG_ZLIB) ===
 * deflateInit → deflate(Z_BLOCK/Z_FINISH) 루프 → deflateEnd 의 zlib 3-단계 패턴.
 * 각 조각마다 FIO_NET_CMD_IOLOG sk_entry 를 생성해 first->next 체인에 append.
 */
#ifdef CONFIG_ZLIB

/*
 * [한국어]
 * __fio_net_prep_tail - 현재 압축 출력 버퍼(out_pdu)를 엔트리로 포장해 체인 끝에 추가
 *
 * @stream: z_stream — avail_out 으로 실제 압축 바이트 계산.
 * @out_pdu: malloc 된 FIO_SERVER_MAX_FRAGMENT_PDU 크기 버퍼.
 * @last_entry: Out — 생성된 sk_entry 포인터(NULL 이면 실패).
 * @first: 체인 헤더 엔트리 (first->next 에 연결).
 * @return: 없음.
 */
static inline void __fio_net_prep_tail(z_stream *stream, void *out_pdu,
					struct sk_entry **last_entry,
					struct sk_entry *first)
{
	unsigned int this_len = FIO_SERVER_MAX_FRAGMENT_PDU - stream->avail_out;

	*last_entry = fio_net_prep_cmd(FIO_NET_CMD_IOLOG, out_pdu, this_len,
				 NULL, SK_F_VEC | SK_F_INLINE | SK_F_FREE);
	if (*last_entry)
		flist_add_tail(&(*last_entry)->list, &first->next);
}

/*
 * Deflates the next input given, creating as many new packets in the
 * linked list as necessary.
 */
/*
 * [한국어]
 * __deflate_pdu_buffer - next_in 의 next_sz 바이트를 zlib 로 압축해 패킷 체인에 흘려보냄
 *
 * @next_in: 입력 데이터.
 * @next_sz: 입력 바이트 수.
 * @out_pdu: 현재 출력 버퍼(다 차면 새 버퍼 할당하여 치환).
 * @last_entry: 최근 생성된 sk_entry (실패 판정 시 사용).
 * @stream: z_stream (avail_in/out 유지).
 * @first: 체인 헤더.
 * @return: 0 성공, 1 실패.
 *
 * deflate(Z_BLOCK): 블록 단위 압축 — 스트림 전체 flush 하지 않고 다음 입력 허용.
 */
static int __deflate_pdu_buffer(void *next_in, unsigned int next_sz, void **out_pdu,
				struct sk_entry **last_entry, z_stream *stream,
				struct sk_entry *first)
{
	int ret;

	stream->next_in = next_in;
	stream->avail_in = next_sz;
	do {
		if (!stream->avail_out) {
			__fio_net_prep_tail(stream, *out_pdu, last_entry, first);
			if (*last_entry == NULL)
				return 1;

			*out_pdu = malloc(FIO_SERVER_MAX_FRAGMENT_PDU);

			stream->avail_out = FIO_SERVER_MAX_FRAGMENT_PDU;
			stream->next_out = *out_pdu;
		}

		ret = deflate(stream, Z_BLOCK);

		if (ret < 0) {
			free(*out_pdu);
			return 1;
		}
	} while (stream->avail_in);

	return 0;
}

/*
 * [한국어]
 * __fio_append_iolog_gz_hist - 히스토그램 I/O 로그 압축 (delta 계산 포함)
 *
 * @first: 체인 헤더.
 * @log: 원본 io_log.
 * @cur_log: 한 청크의 io_logs.
 * @stream: z_stream.
 * @return: 0 성공, 1 실패.
 *
 * 히스토그램 delta 최적화: 이전 샘플과의 차이만 저장하여 전송량 감소. 클라 측에서
 * 재구성 부담이 줄어들도록 서버가 직접 뺄셈 수행.
 */
static int __fio_append_iolog_gz_hist(struct sk_entry *first, struct io_log *log,
				      struct io_logs *cur_log, z_stream *stream)
{
	struct sk_entry *entry;
	void *out_pdu;
	int ret, i, j;
	int sample_sz = log_entry_sz(log);

	out_pdu = malloc(FIO_SERVER_MAX_FRAGMENT_PDU);
	stream->avail_out = FIO_SERVER_MAX_FRAGMENT_PDU;
	stream->next_out = out_pdu;

	for (i = 0; i < cur_log->nr_samples; i++) {
		struct io_sample *s;
		struct io_u_plat_entry *cur_plat_entry, *prev_plat_entry;
		uint64_t *cur_plat, *prev_plat;

		s = get_sample(log, cur_log, i);
		ret = __deflate_pdu_buffer(s, sample_sz, &out_pdu, &entry, stream, first);
		if (ret)
			return ret;

		/* Do the subtraction on server side so that client doesn't have to
		 * reconstruct our linked list from packets.
		 */
		cur_plat_entry  = s->data.plat_entry;
		prev_plat_entry = flist_first_entry(&cur_plat_entry->list, struct io_u_plat_entry, list);
		cur_plat  = cur_plat_entry->io_u_plat;
		prev_plat = prev_plat_entry->io_u_plat;

		for (j = 0; j < FIO_IO_U_PLAT_NR; j++) {
			cur_plat[j] -= prev_plat[j];
		}

		flist_del(&prev_plat_entry->list);
		free(prev_plat_entry);

		ret = __deflate_pdu_buffer(cur_plat_entry, sizeof(*cur_plat_entry),
					   &out_pdu, &entry, stream, first);

		if (ret)
			return ret;
	}

	__fio_net_prep_tail(stream, out_pdu, &entry, first);
	return entry == NULL;
}

/*
 * [한국어]
 * __fio_append_iolog_gz - 단일 io_logs 청크 압축 (히스토그램 분기 포함)
 *
 * @first: 체인 헤더.
 * @log: 원본 io_log.
 * @cur_log: 이 청크.
 * @stream: z_stream.
 * @return: 0 성공, 1 실패.
 *
 * log_type == IO_LOG_TYPE_HIST 이면 __fio_append_iolog_gz_hist 로 위임(delta 계산).
 * 일반 로그는 cur_log->log 배열을 FIO_SERVER_MAX_FRAGMENT_PDU 조각으로 압축.
 */
static int __fio_append_iolog_gz(struct sk_entry *first, struct io_log *log,
				 struct io_logs *cur_log, z_stream *stream)
{
	unsigned int this_len;
	void *out_pdu;
	int ret;

	if (log->log_type == IO_LOG_TYPE_HIST)
		return __fio_append_iolog_gz_hist(first, log, cur_log, stream);

	stream->next_in = (void *) cur_log->log;
	stream->avail_in = cur_log->nr_samples * log_entry_sz(log);

	do {
		struct sk_entry *entry;

		/*
		 * Dirty - since the log is potentially huge, compress it into
		 * FIO_SERVER_MAX_FRAGMENT_PDU chunks and let the receiving
		 * side defragment it.
		 */
		out_pdu = malloc(FIO_SERVER_MAX_FRAGMENT_PDU);

		stream->avail_out = FIO_SERVER_MAX_FRAGMENT_PDU;
		stream->next_out = out_pdu;
		ret = deflate(stream, Z_BLOCK);
		/* may be Z_OK, or Z_STREAM_END */
		if (ret < 0) {
			free(out_pdu);
			return 1;
		}

		this_len = FIO_SERVER_MAX_FRAGMENT_PDU - stream->avail_out;

		entry = fio_net_prep_cmd(FIO_NET_CMD_IOLOG, out_pdu, this_len,
					 NULL, SK_F_VEC | SK_F_INLINE | SK_F_FREE);
		if (!entry) {
			free(out_pdu);
			return 1;
		}
		flist_add_tail(&entry->list, &first->next);
	} while (stream->avail_in);

	return 0;
}

/*
 * [한국어]
 * fio_append_iolog_gz - io_log 전체 zlib 압축 전송 (top-level)
 *
 * @first: 헤더 sk_entry.
 * @log: 압축할 io_log.
 * @return: 0 성공, 1 실패.
 *
 * 단계:
 *   1) deflateInit(Z_DEFAULT_COMPRESSION) — zlib 스트림 초기화.
 *   2) 모든 io_logs 청크를 순회하며 __fio_append_iolog_gz 호출(압축 + 체인 추가).
 *   3) deflate(Z_FINISH) 루프 — 남은 출력 버퍼를 flush(Z_STREAM_END 까지).
 *   4) deflateEnd 로 자원 회수.
 */
static int fio_append_iolog_gz(struct sk_entry *first, struct io_log *log)
{
	z_stream stream = {
		.zalloc	= Z_NULL,
		.zfree	= Z_NULL,
		.opaque	= Z_NULL,
	};
	int ret = 0;

	if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK)
		return 1;

	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);

		ret = __fio_append_iolog_gz(first, log, cur_log, &stream);
		if (ret)
			break;
	}

	ret = deflate(&stream, Z_FINISH);

	while (ret != Z_STREAM_END) {
		struct sk_entry *entry;
		unsigned int this_len;
		void *out_pdu;

		out_pdu = malloc(FIO_SERVER_MAX_FRAGMENT_PDU);
		stream.avail_out = FIO_SERVER_MAX_FRAGMENT_PDU;
		stream.next_out = out_pdu;

		ret = deflate(&stream, Z_FINISH);
		/* may be Z_OK, or Z_STREAM_END */
		if (ret < 0) {
			free(out_pdu);
			break;
		}

		this_len = FIO_SERVER_MAX_FRAGMENT_PDU - stream.avail_out;

		entry = fio_net_prep_cmd(FIO_NET_CMD_IOLOG, out_pdu, this_len,
					 NULL, SK_F_VEC | SK_F_INLINE | SK_F_FREE);
		if (!entry) {
			free(out_pdu);
			break;
		}
		flist_add_tail(&entry->list, &first->next);
	}

	ret = deflateEnd(&stream);
	if (ret == Z_OK)
		return 0;

	return 1;
}
#else
/* [한국어] zlib 미지원 시 항상 실패 반환 */
static int fio_append_iolog_gz(struct sk_entry *first, struct io_log *log)
{
	return 1;
}
#endif

/*
 * [한국어]
 * fio_append_gz_chunks - 사전 압축된 io_log 청크를 그대로 체인에 추가 (STORE_COMPRESSED)
 *
 * @first: 헤더 엔트리.
 * @log: chunk_list 에 압축 청크 보유.
 * @return: 0 성공, 1 실패.
 *
 * chunk_lock 뮤텍스 — 잡 스레드가 동시에 청크를 append 하는 경우 보호.
 * 이미 압축된 데이터는 재압축하지 않음 (CPU 절약 + 클라 측 동일 포맷).
 */
static int fio_append_gz_chunks(struct sk_entry *first, struct io_log *log)
{
	struct sk_entry *entry;
	struct flist_head *node;
	int ret = 0;

	pthread_mutex_lock(&log->chunk_lock);
	flist_for_each(node, &log->chunk_list) {
		struct iolog_compress *c;

		c = flist_entry(node, struct iolog_compress, list);
		entry = fio_net_prep_cmd(FIO_NET_CMD_IOLOG, c->buf, c->len,
						NULL, SK_F_VEC | SK_F_INLINE);
		if (!entry) {
			ret = 1;
			break;
		}
		flist_add_tail(&entry->list, &first->next);
	}
	pthread_mutex_unlock(&log->chunk_lock);
	return ret;
}

/*
 * [한국어]
 * fio_append_text_log - 평문 I/O 로그 엔트리들을 체인에 추가 (압축 없음)
 *
 * @first: 헤더 엔트리.
 * @log: 원본 로그 (io_logs 체인 소비).
 * @return: 0 성공, 1 실패.
 *
 * use_zlib=0 이고 chunk_list 도 비었을 때 경로. 각 io_logs 청크 전체를 하나의 sk_entry 로
 * 묶어 체인에 추가. 실제 전송은 send_vec_entry 가 수행.
 */
static int fio_append_text_log(struct sk_entry *first, struct io_log *log)
{
	struct sk_entry *entry;
	int ret = 0;

	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;
		size_t size;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);

		size = cur_log->nr_samples * log_entry_sz(log);

		entry = fio_net_prep_cmd(FIO_NET_CMD_IOLOG, cur_log->log, size,
						NULL, SK_F_VEC | SK_F_INLINE);
		if (!entry) {
			ret = 1;
			break;
		}
		flist_add_tail(&entry->list, &first->next);
	}

	return ret;
}

/*
 * [한국어]
 * fio_send_iolog - I/O 로그(BW/IOPS/LAT/CLAT/히스토그램) 전체를 FIO_NET_CMD_IOLOG 로 전송
 *
 * @td: 이 로그를 생성한 잡의 thread_data.
 * @log: io_log — io_logs 체인 + (사전 압축 시) chunk_list.
 * @name: 로그 이름 (클라 측 파일명 조합에 사용).
 * @return: 0 성공, 1 실패.
 *
 * 4단계:
 *   1) cmd_iolog_pdu 헤더 채움 — nr_samples, thread_number, log_type,
 *      hist_coarseness, per_job_logs, compressed(STORE/XMIT/0).
 *   2) 모든 io_sample 의 정수 필드를 LE 로 직렬화 (time/val0/val1/__ddir/bs/aux).
 *   3) 헤더 엔트리(first, SK_F_VEC|SK_F_INLINE|SK_F_COPY) 준비.
 *   4) 압축 모드별 함수 호출:
 *      - chunk_list 비어있지 않음 → fio_append_gz_chunks (사전 압축 청크 그대로).
 *      - use_zlib=1 → fio_append_iolog_gz (런타임 zlib deflate).
 *      - 그 외 → fio_append_text_log (평문).
 *   5) fio_net_queue_entry(first) 로 체인 통째로 큐잉.
 *
 * 호출 체인: iolog.c write_iolog / stat.c → fio_send_iolog → fio_append_*.
 */
int fio_send_iolog(struct thread_data *td, struct io_log *log, const char *name)
{
	struct cmd_iolog_pdu pdu = {
		.nr_samples		= cpu_to_le64(iolog_nr_samples(log)),
		.thread_number		= cpu_to_le32(td->thread_number),
		.log_type		= cpu_to_le32(log->log_type),
		.log_hist_coarseness	= cpu_to_le32(log->hist_coarseness),
		.per_job_logs		= cpu_to_le32(td->o.per_job_logs),
	};
	struct sk_entry *first;
	struct flist_head *entry;
	int ret = 0;

	if (!flist_empty(&log->chunk_list))
		pdu.compressed = __cpu_to_le32(STORE_COMPRESSED);
	else if (use_zlib)
		pdu.compressed = __cpu_to_le32(XMIT_COMPRESSED);
	else
		pdu.compressed = 0;

	snprintf((char *) pdu.name, sizeof(pdu.name), "%s", name);

	/*
	 * We can't do this for a pre-compressed log, but for that case,
	 * log->nr_samples is zero anyway.
	 */
	flist_for_each(entry, &log->io_logs) {
		struct io_logs *cur_log;
		int i;

		cur_log = flist_entry(entry, struct io_logs, list);

		for (i = 0; i < cur_log->nr_samples; i++) {
			struct io_sample *s = get_sample(log, cur_log, i);

			s->time		= cpu_to_le64(s->time);
			if (log->log_type != IO_LOG_TYPE_HIST) {
				s->data.val.val0	= cpu_to_le64(s->data.val.val0);
				s->data.val.val1	= cpu_to_le64(s->data.val.val1);
			}
			s->__ddir	= __cpu_to_le32(s->__ddir);
			s->bs		= cpu_to_le64(s->bs);

			if (log->log_offset)
				s->aux[IOS_AUX_OFFSET_INDEX] =
					cpu_to_le64(s->aux[IOS_AUX_OFFSET_INDEX]);

			if (log->log_issue_time)
				s->aux[IOS_AUX_ISSUE_TIME_INDEX] =
					cpu_to_le64(s->aux[IOS_AUX_ISSUE_TIME_INDEX]);
		}
	}

	/*
	 * Assemble header entry first
	 */
	first = fio_net_prep_cmd(FIO_NET_CMD_IOLOG, &pdu, sizeof(pdu), NULL, SK_F_VEC | SK_F_INLINE | SK_F_COPY);
	if (!first)
		return 1;

	/*
	 * Now append actual log entries. If log compression was enabled on
	 * the job, just send out the compressed chunks directly. If we
	 * have a plain log, compress if we can, then send. Otherwise, send
	 * the plain text output.
	 */
	if (!flist_empty(&log->chunk_list))
		ret = fio_append_gz_chunks(first, log);
	else if (use_zlib)
		ret = fio_append_iolog_gz(first, log);
	else
		ret = fio_append_text_log(first, log);

	fio_net_queue_entry(first);
	return ret;
}

/*
 * [한국어]
 * fio_server_send_add_job - 잡이 추가되었음을 클라에 통보 (thread_options 스냅샷 포함)
 *
 * @td: 새로 추가된 잡의 thread_data.
 * @return: 없음.
 *
 * 동작: cmd_add_job_pdu 헤더에 thread_number/groupid 세트 후 top 뒤쪽에
 *       convert_thread_options_to_net 이 thread_options 를 네트워크 바이트 오더로 pack.
 * 호출 체인: init.c add_job → fio_server_send_add_job → fio_net_queue_cmd(ADD_JOB).
 */
void fio_server_send_add_job(struct thread_data *td)
{
	struct cmd_add_job_pdu *pdu;
	size_t cmd_sz = offsetof(struct cmd_add_job_pdu, top) +
		thread_options_pack_size(&td->o);

	pdu = malloc(cmd_sz);
	pdu->thread_number = cpu_to_le32(td->thread_number);
	pdu->groupid = cpu_to_le32(td->groupid);

	convert_thread_options_to_net(&pdu->top, &td->o);

	fio_net_queue_cmd(FIO_NET_CMD_ADD_JOB, pdu, cmd_sz, NULL, SK_F_COPY);
	free(pdu);
}

/*
 * [한국어]
 * fio_server_send_start - SERVER_START 통보 (fio_backend 준비 완료)
 *
 * @td: 신호 송신자 (컨벤션상 — 사용되지는 않음, 시그니처 일관성).
 * @return: 없음.
 *
 * 호출 체인: backend.c 잡 스레드가 준비 완료 시점에 호출 → 클라는 이 시점부터
 *           ETA 요청을 보내기 시작.
 * abort 경로: sk_out 이 없으면 (pthread_getspecific 실패) abort — 치명 버그.
 */
void fio_server_send_start(struct thread_data *td)
{
	struct sk_out *sk_out = pthread_getspecific(sk_out_key);

	if (sk_out->sk == -1) {
		log_err("pthread getting specific for key failed, sk_out %p, sk %i, err: %i:%s",
			sk_out, sk_out->sk, errno, strerror(errno));
		abort();
	}

	fio_net_queue_cmd(FIO_NET_CMD_SERVER_START, NULL, 0, NULL, SK_F_SIMPLE);
}

/*
 * [한국어]
 * fio_server_get_verify_state - 클라이언트에게 verify-state 파일 요청 + 세마포어 동기 대기
 *
 * @name: 잡/파일 식별자(verify_state_gen_name 의 입력).
 * @threadnumber: 잡 스레드 번호.
 * @datap: 출력 — 성공 시 malloc 한 thread_io_list 버퍼 포인터.
 * @return: 0 성공, errno (ETIMEDOUT/EILSEQ/ENOMEM 등) 실패.
 *
 * 동기 프로토콜:
 *   1) cmd_reply 구조체를 FIO_SEM_LOCKED 로 초기화 (smalloc — SHM 공유).
 *   2) FIO_NET_CMD_SENDFILE 을 큐잉 — tag 에 cmd_reply* 포인터 인코딩.
 *   3) fio_sem_down_timeout(&rep->lock, 10000) — 10초 블로킹 대기.
 *   4) handle_command(SENDFILE case) 가 응답 도착 시 rep->data/size/error 채우고
 *      fio_sem_up(&rep->lock) 으로 깨움.
 *   5) verify_state_hdr 체크섬 검증 → thread_io_list 만 추출해 반환.
 *
 * 에러 경로: 타임아웃/error/EILSEQ 모두 fail: 라벨로 갱신 후 QUIT 큐잉.
 */
int fio_server_get_verify_state(const char *name, int threadnumber,
				void **datap)
{
	struct thread_io_list *s;
	struct cmd_sendfile out;
	struct cmd_reply *rep;
	uint64_t tag;
	void *data;
	int ret;

	dprint(FD_NET, "server: request verify state\n");

	rep = smalloc(sizeof(*rep));
	if (!rep)
		return ENOMEM;

	__fio_sem_init(&rep->lock, FIO_SEM_LOCKED);
	rep->data = NULL;
	rep->error = 0;

	verify_state_gen_name((char *) out.path, sizeof(out.path), name, me,
				threadnumber);
	tag = (uint64_t) (uintptr_t) rep;
	fio_net_queue_cmd(FIO_NET_CMD_SENDFILE, &out, sizeof(out), &tag,
				SK_F_COPY);

	/*
	 * Wait for the backend to receive the reply
	 */
	if (fio_sem_down_timeout(&rep->lock, 10000)) {
		log_err("fio: timed out waiting for reply\n");
		ret = ETIMEDOUT;
		goto fail;
	}

	if (rep->error) {
		log_err("fio: failure on receiving state file %s: %s\n",
				out.path, strerror(rep->error));
		ret = rep->error;
fail:
		*datap = NULL;
		sfree(rep);
		fio_net_queue_quit();
		return ret;
	}

	/*
	 * The format is verify_state_hdr, then thread_io_list. Verify
	 * the header, and the thread_io_list checksum
	 */
	s = rep->data + sizeof(struct verify_state_hdr);
	if (verify_state_hdr(rep->data, s)) {
		ret = EILSEQ;
		goto fail;
	}

	/*
	 * Don't need the header from now, copy just the thread_io_list
	 */
	ret = 0;
	rep->size -= sizeof(struct verify_state_hdr);
	data = malloc(rep->size);
	memcpy(data, s, rep->size);
	*datap = data;

	sfree(rep->data);
	__fio_sem_remove(&rep->lock);
	sfree(rep);
	return ret;
}

/*
 * [한국어]
 * fio_init_server_ip - IPv4/IPv6 서버 소켓 생성 + SO_REUSEADDR/REUSEPORT + bind
 *
 * @return: 성공 시 소켓 fd, 실패 시 -1.
 *
 * socket(AF_INET or AF_INET6, SOCK_STREAM, 0): 스트림 TCP 소켓.
 * SO_REUSEADDR: TIME_WAIT 중인 주소 재사용 허용 (서버 재시작 용이).
 * SO_REUSEPORT: 여러 프로세스가 같은 포트에 bind 가능 — 실패해도 치명적 아님.
 * bind(saddr_in 또는 saddr_in6): fio_handle_server_arg 가 채운 주소로 바인드.
 */
static int fio_init_server_ip(void)
{
	struct sockaddr *addr;
	socklen_t socklen;
	char buf[80];
	const char *str;
	int sk, opt;

	if (use_ipv6)
		sk = socket(AF_INET6, SOCK_STREAM, 0);
	else
		sk = socket(AF_INET, SOCK_STREAM, 0);

	if (sk < 0) {
		log_err("fio: socket: %s\n", strerror(errno));  /* [한국어] 소켓 생성 실패(리소스 한계) */
		return -1;
	}

	opt = 1;
	if (setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt)) < 0) {
		/* [한국어] SO_REUSEADDR — 서버 재시작 시 "Address already in use" 회피 */
		log_err("fio: setsockopt(REUSEADDR): %s\n", strerror(errno));
		close(sk);
		return -1;
	}
#ifdef SO_REUSEPORT
	/*
	 * Not fatal if fails, so just ignore it if that happens
	 */
	if (setsockopt(sk, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
		/* [한국어] SO_REUSEPORT — 같은 포트에 여러 프로세스가 bind 가능(부하 분산).
		 *         구형 커널/다른 OS 에선 미지원 — 실패 무시. */
	}
#endif

	if (use_ipv6) {
		void *src = &saddr_in6.sin6_addr;

		addr = (struct sockaddr *) &saddr_in6;
		socklen = sizeof(saddr_in6);
		saddr_in6.sin6_family = AF_INET6;        /* [한국어] AF 계열 세트 */
		str = inet_ntop(AF_INET6, src, buf, sizeof(buf));  /* [한국어] 로그용 문자열화 */
	} else {
		void *src = &saddr_in.sin_addr;

		addr = (struct sockaddr *) &saddr_in;
		socklen = sizeof(saddr_in);
		saddr_in.sin_family = AF_INET;
		str = inet_ntop(AF_INET, src, buf, sizeof(buf));
	}

	if (bind(sk, addr, socklen) < 0) {
		/* [한국어] bind 실패 — 포트 점유 또는 권한 부족 */
		log_err("fio: bind: %s\n", strerror(errno));
		log_info("fio: failed with IPv%c %s\n", use_ipv6 ? '6' : '4', str);
		close(sk);
		return -1;
	}

	return sk;
}

/*
 * [한국어]
 * fio_init_server_sock - Unix 도메인 소켓 기반 서버 소켓 생성 + bind
 *
 * @return: 성공 시 소켓 fd, 실패 시 -1.
 *
 * AF_UNIX + SOCK_STREAM: 같은 호스트 내 파일 시스템 경로 기반 통신.
 * umask(000): sun_path 에 생성되는 소켓 파일의 권한을 666 으로 하여 접근 제한 해제
 *            (이후 원래 umask 복원).
 * sun_path 길이: 보통 108B 제한. snprintf 로 안전 복사.
 */
static int fio_init_server_sock(void)
{
	struct sockaddr_un addr;
	socklen_t len;
	mode_t mode;
	int sk;

	sk = socket(AF_UNIX, SOCK_STREAM, 0);  /* [한국어] Unix 도메인 스트림 소켓 */
	if (sk < 0) {
		log_err("fio: socket: %s\n", strerror(errno));
		return -1;
	}

	mode = umask(000);  /* [한국어] umask 0 으로 세트 — 소켓 파일 권한 제한 해제 */

	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", bind_sock);
	/* [한국어] 경로 복사 (길이 초과 시 자르고 NUL 종결) */

	len = sizeof(addr.sun_family) + strlen(bind_sock) + 1;
	/* [한국어] bind len 은 family + 경로(+ NUL) 합 */

	if (bind(sk, (struct sockaddr *) &addr, len) < 0) {
		/* [한국어] bind 실패 — 같은 경로 이미 존재 또는 권한 부족 */
		log_err("fio: bind: %s\n", strerror(errno));
		close(sk);
		return -1;
	}

	umask(mode);  /* [한국어] 원래 umask 복원 */
	return sk;
}

/*
 * [한국어]
 * fio_init_server_connection - 서버 소켓 초기화 + 주소 로그 출력 + listen 시작
 *
 * @return: 성공 시 listen 완료된 fd, 실패 시 -1.
 *
 * bind_sock 이 세트되어 있으면 Unix 도메인, 아니면 IP 소켓.
 * listen(sk, 4): backlog=4 — 다수 클라이언트가 동시 접속하는 드문 경우 대비.
 */
static int fio_init_server_connection(void)
{
	char bind_str[128];
	int sk;

	dprint(FD_NET, "starting server\n");

	if (!bind_sock)
		sk = fio_init_server_ip();
	else
		sk = fio_init_server_sock();

	if (sk < 0)
		return sk;

	memset(bind_str, 0, sizeof(bind_str));

	if (!bind_sock) {
		char *p, port[16];
		void *src;
		int af;

		if (use_ipv6) {
			af = AF_INET6;
			src = &saddr_in6.sin6_addr;
		} else {
			af = AF_INET;
			src = &saddr_in.sin_addr;
		}

		p = (char *) inet_ntop(af, src, bind_str, sizeof(bind_str));

		sprintf(port, ",%u", fio_net_port);
		if (p)
			strcat(p, port);
		else
			snprintf(bind_str, sizeof(bind_str), "%s", port);
	} else
		snprintf(bind_str, sizeof(bind_str), "%s", bind_sock);

	log_info("fio: server listening on %s\n", bind_str);

	if (listen(sk, 4) < 0) {
		/* [한국어] listen(backlog=4) — accept 대기 큐 크기. 동시 접속 드물어 소형. */
		log_err("fio: listen: %s\n", strerror(errno));
		close(sk);
		return -1;
	}

	return sk;
}

/*
 * [한국어]
 * fio_server_parse_host - 호스트명/IP 문자열을 binary 주소 구조체로 변환
 *
 * @host: "1.2.3.4" / "::1" / "example.com" 등.
 * @ipv6: 0=IPv4, 1=IPv6.
 * @inp: IPv4 결과 저장 위치.
 * @inp6: IPv6 결과 저장 위치.
 * @return: 0 성공, 1 실패.
 *
 * 2단계 접근: inet_pton 으로 numeric 시도 → 실패 시 getaddrinfo 로 DNS 해석.
 * DNS 실패 시 gai_strerror 로 에러 메시지 출력.
 */
int fio_server_parse_host(const char *host, int ipv6, struct in_addr *inp,
			  struct in6_addr *inp6)

{
	int ret = 0;

	if (ipv6)
		ret = inet_pton(AF_INET6, host, inp6);
	else
		ret = inet_pton(AF_INET, host, inp);

	if (ret != 1) {
		struct addrinfo *res, hints = {
			.ai_family = ipv6 ? AF_INET6 : AF_INET,
			.ai_socktype = SOCK_STREAM,
		};

		ret = getaddrinfo(host, NULL, &hints, &res);
		if (ret) {
			log_err("fio: failed to resolve <%s> (%s)\n", host,
					gai_strerror(ret));
			return 1;
		}

		if (ipv6)
			memcpy(inp6, &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr, sizeof(*inp6));
		else
			memcpy(inp, &((struct sockaddr_in *) res->ai_addr)->sin_addr, sizeof(*inp));

		ret = 1;
		freeaddrinfo(res);
	}

	return !(ret == 1);
}

/*
 * Parse a host/ip/port string. Reads from 'str'.
 *
 * Outputs:
 *
 * For IPv4:
 *	*ptr is the host, *port is the port, inp is the destination.
 * For IPv6:
 *	*ptr is the host, *port is the port, inp6 is the dest, and *ipv6 is 1.
 * For local domain sockets:
 *	*ptr is the filename, *is_sock is 1.
 */
/*
 * [한국어]
 * fio_server_parse_string - 서버 --server 인자 문자열을 각 구성 요소로 분리
 *
 * @str: 입력 문자열.
 * @ptr: Out — 호스트 문자열 strdup (is_sock 이면 Unix 경로).
 * @is_sock: Out — true 이면 "sock:" 접두.
 * @port: Out — 포트 번호.
 * @inp/@inp6: Out — IPv4/IPv6 바이너리 주소.
 * @ipv6: Out — 1 이면 IPv6 사용.
 * @return: 0 성공, 1 실패.
 *
 * 지원 형식:
 *   - "sock:/path"     → Unix 도메인 소켓.
 *   - "ip:1.2.3.4"     → IPv4 (ip: 접두 스트립).
 *   - "ip4:..."        → 동일.
 *   - "ip6:::1"        → IPv6.
 *   - "host,port"      → IP/호스트 + 포트 (',' 구분자).
 *   - ":8765"          → 포트만.
 *
 * 호출 체인: fio_handle_server_arg / client.c → fio_server_parse_string →
 *           fio_server_parse_host.
 */
int fio_server_parse_string(const char *str, char **ptr, bool *is_sock,
			    int *port, struct in_addr *inp,
			    struct in6_addr *inp6, int *ipv6)
{
	const char *host = str;
	char *portp;
	int lport = 0;

	*ptr = NULL;
	*is_sock = false;
	*port = fio_net_port;
	*ipv6 = 0;

	if (!strncmp(str, "sock:", 5)) {
		*ptr = strdup(str + 5);
		*is_sock = true;

		return 0;
	}

	/*
	 * Is it ip:<ip or host>:port
	 */
	if (!strncmp(host, "ip:", 3))
		host += 3;
	else if (!strncmp(host, "ip4:", 4))
		host += 4;
	else if (!strncmp(host, "ip6:", 4)) {
		host += 4;
		*ipv6 = 1;
	} else if (host[0] == ':') {
		/* String is :port */
		host++;
		lport = atoi(host);
		if (!lport || lport > 65535) {
			log_err("fio: bad server port %u\n", lport);
			return 1;
		}
		/* no hostname given, we are done */
		*port = lport;
		return 0;
	}

	/*
	 * If no port seen yet, check if there's a last ',' at the end
	 */
	if (!lport) {
		portp = strchr(host, ',');
		if (portp) {
			*portp = '\0';
			portp++;
			lport = atoi(portp);
			if (!lport || lport > 65535) {
				log_err("fio: bad server port %u\n", lport);
				return 1;
			}
		}
	}

	if (lport)
		*port = lport;

	if (!strlen(host))
		return 0;

	*ptr = strdup(host);

	if (fio_server_parse_host(*ptr, *ipv6, inp, inp6)) {
		free(*ptr);
		*ptr = NULL;
		return 1;
	}

	if (*port == 0)
		*port = fio_net_port;

	return 0;
}

/*
 * Server arg should be one of:
 *
 * sock:/path/to/socket
 *   ip:1.2.3.4
 *      1.2.3.4
 *
 * Where sock uses unix domain sockets, and ip binds the server to
 * a specific interface. If no arguments are given to the server, it
 * uses IP and binds to 0.0.0.0.
 *
 */
/*
 * [한국어]
 * fio_handle_server_arg - --server 인자 처리 → saddr_in/saddr_in6/bind_sock/use_ipv6 세팅
 *
 * @return: 0 성공, 1 실패.
 *
 * 인자 없음 → INADDR_ANY(0.0.0.0) + 기본 포트 FIO_NET_PORT(8765).
 * sock 문자열이 세트되었지만 is_sock 이 false 이면 bind_sock 해제.
 */
static int fio_handle_server_arg(void)
{
	int port = fio_net_port;
	bool is_sock;
	int ret = 0;

	saddr_in.sin_addr.s_addr = htonl(INADDR_ANY);

	if (!fio_server_arg)
		goto out;

	ret = fio_server_parse_string(fio_server_arg, &bind_sock, &is_sock,
					&port, &saddr_in.sin_addr,
					&saddr_in6.sin6_addr, &use_ipv6);

	if (!is_sock && bind_sock) {
		free(bind_sock);
		bind_sock = NULL;
	}

out:
	fio_net_port = port;
	saddr_in.sin_port = htons(port);
	saddr_in6.sin6_port = htons(port);
	return ret;
}

/*
 * [한국어]
 * sig_int - SIGINT(Ctrl+C) 시그널 핸들러 — Unix 소켓 파일 정리
 *
 * @sig: 받은 시그널 번호(SIGINT 또는 Windows SIGBREAK).
 * @return: 없음.
 *
 * async-signal-safe 함수만 사용 허용 — unlink(2) 는 safe.
 * exit_backend 세트는 상위 fio_server_got_signal 에서 처리하며, 여기는 소켓 파일만 제거.
 */
static void sig_int(int sig)
{
	if (bind_sock)
		unlink(bind_sock);  /* [한국어] Unix 도메인 소켓 파일 제거 — 다음 bind 가 "이미 존재" 에러를 피함 */
}

/*
 * [한국어]
 * set_sig_handlers - SIGINT/SIGBREAK 시그널 핸들러 등록
 *
 * @return: 없음.
 *
 * SA_RESTART: 시그널로 끊긴 syscall 을 자동 재시작(accept/poll 등이 EINTR 으로 깨지는 빈도 감소).
 * Windows SIGBREAK: Ctrl+Break (POSIX SIGINT 와 대응).
 */
static void set_sig_handlers(void)
{
	struct sigaction act = {
		.sa_handler = sig_int,
		.sa_flags = SA_RESTART,
	};

	sigaction(SIGINT, &act, NULL);  /* [한국어] Ctrl+C 처리 */

	/* Windows uses SIGBREAK as a quit signal from other applications */
#ifdef WIN32
	sigaction(SIGBREAK, &act, NULL);  /* [한국어] Windows Ctrl+Break — SIGINT 와 동일 처리 */
#endif
}

/*
 * [한국어]
 * fio_server_destroy_sk_key - pthread TLS 키 제거 (프로세스 종료 직전)
 *
 * @return: 없음.
 *
 * 호출 체인: main → deinitialize_fio → fio_server_destroy_sk_key.
 */
void fio_server_destroy_sk_key(void)
{
	pthread_key_delete(sk_out_key);
}

/*
 * [한국어]
 * fio_server_create_sk_key - pthread TLS 키 생성 (main 직후 1회)
 *
 * @return: 0 성공, 1 실패.
 *
 * sk_out_key 는 모듈 전역 — 이 키 하나로 여러 스레드가 각자의 sk_out* 을 TLS 에 저장.
 * 소멸자 NULL: 스레드 종료 시 TLS 값 자동 해제하지 않음(sk_out_drop 가 수동 처리).
 */
int fio_server_create_sk_key(void)
{
	if (pthread_key_create(&sk_out_key, NULL)) {
		log_err("fio: can't create sk_out backend key\n");
		return 1;
	}

	pthread_setspecific(sk_out_key, NULL);  /* [한국어] 메인 스레드 초기값 NULL */
	return 0;
}

/*
 * [한국어]
 * fio_server - 서버 메인 함수 (소켓 준비 + accept_loop 진입)
 *
 * @return: accept_loop 반환값 또는 -1 실패.
 *
 * 단계:
 *   1) fio_handle_server_arg — --server 인자 파싱 → saddr_in[6], bind_sock, use_ipv6.
 *   2) set_sig_handlers — SIGINT/SIGBREAK 등록.
 *   3) (Windows 자식) handle_connection_process 바로 호출 — 부모로부터 파이프 수신.
 *   4) fio_init_server_connection — socket+bind+listen.
 *   5) accept_loop — 연결 수락 루프.
 *   6) 종료 정리: close(listen_sk), fio_server_arg/bind_sock 해제.
 */
static int fio_server(void)
{
	int sk, ret;

	dprint(FD_NET, "starting server\n");

	if (fio_handle_server_arg())
		return -1;

	set_sig_handlers();

#ifdef WIN32
	/* if this is a child process, go handle the connection */
	if (fio_server_pipe_name != NULL) {
		ret = handle_connection_process();
		return ret;
	}

	/* job to link child processes so they terminate together */
	hjob = windows_create_job();
	if (hjob == INVALID_HANDLE_VALUE)
		return -1;
#endif

	sk = fio_init_server_connection();
	if (sk < 0)
		return -1;

	ret = accept_loop(sk);

	close(sk);

	if (fio_server_arg) {
		free(fio_server_arg);
		fio_server_arg = NULL;
	}
	if (bind_sock)
		free(bind_sock);

	return ret;
}

/*
 * [한국어]
 * fio_server_got_signal - 시그널 수신 시 서버 상태 플래그 갱신
 *
 * @signal: 받은 시그널 번호.
 * @return: 없음.
 *
 * SIGPIPE: writev/send 가 실패 반환 후 이 핸들러가 호출됨 → sk 를 -1 로 세트해
 *          이후 전송 시도를 모두 차단.
 * 기타: exit_backend=true → 모든 poll/accept 루프가 다음 반복에서 탈출.
 * 주의: 이 함수는 시그널 핸들러가 아니라 상위에서 호출되는 "시그널 이벤트 처리" 함수
 *       (main thread context) — async-signal-safety 제약 없음.
 */
void fio_server_got_signal(int signal)
{
	struct sk_out *sk_out = pthread_getspecific(sk_out_key);

	assert(sk_out);

	if (signal == SIGPIPE)
		sk_out->sk = -1;
	else {
		log_info("\nfio: terminating on signal %d\n", signal);
		exit_backend = true;
	}
}

/*
 * [한국어]
 * check_existing_pidfile - 기존 pidfile 검사 → 서버가 이미 돌고 있는지 판별
 *
 * @pidfile: 파일 경로.
 * @return: 0 = 파일 없음 또는 대상 프로세스 죽음(재시작 가능),
 *          1 = 살아있음 또는 파일 읽기 실패(중복 기동 차단).
 *
 * kill(pid, SIGCONT): SIGCONT=continue 는 "NOP"에 가까움 — 프로세스 존재 여부만 확인.
 * ESRCH 반환 = 해당 PID 없음 → 재기동 허용.
 */
static int check_existing_pidfile(const char *pidfile)
{
	struct stat sb;
	char buf[16];
	pid_t pid;
	FILE *f;

	if (stat(pidfile, &sb))
		return 0;

	f = fopen(pidfile, "r");
	if (!f)
		return 0;

	if (fread(buf, sb.st_size, 1, f) <= 0) {
		fclose(f);
		return 1;
	}
	fclose(f);

	pid = atoi(buf);
	if (kill(pid, SIGCONT) < 0)
		return errno != ESRCH;

	return 1;
}

/*
 * [한국어]
 * write_pid - 데몬 모드에서 자식 PID 를 pidfile 에 기록
 *
 * @pid: 기록할 PID.
 * @pidfile: 경로.
 * @return: 0 성공, 1 실패.
 */
static int write_pid(pid_t pid, const char *pidfile)
{
	FILE *fpid;

	fpid = fopen(pidfile, "w");
	if (!fpid) {
		log_err("fio: failed opening pid file %s\n", pidfile);
		return 1;
	}

	fprintf(fpid, "%u\n", (unsigned int) pid);
	fclose(fpid);
	return 0;
}

/*
 * If pidfile is specified, background us.
 */
/*
 * [한국어]
 * fio_start_server - 서버 기동 진입점 (main() 에서 is_backend 시 호출)
 *
 * @pidfile: 데몬화 요청 시 pidfile 경로, NULL 이면 포그라운드 실행.
 * @return: 서버 종료 코드.
 *
 * 데몬화 단계(pidfile != NULL):
 *   1) check_existing_pidfile — 기존 PID 살아있는지 검사 → 중복 기동 방지.
 *   2) fork() 로 데몬 자식 분리. 부모는 write_pid 후 _exit.
 *   3) 자식: setsid — 새 세션 리더 → 제어터미널 분리.
 *   4) openlog — stderr 대신 syslog 사용.
 *   5) stdin/stdout/stderr → /dev/null (파일 디스크립터 0/1/2 재활용 방지 + tty 연결 끊기).
 *   6) fio_server() 실행.
 *   7) 종료 시 closelog + unlink(pidfile).
 *
 * WSAStartup: Windows 소켓 라이브러리 초기화 (MAKEWORD(2,2) = WinSock 2.2).
 */
int fio_start_server(char *pidfile)
{
	FILE *file;
	pid_t pid;
	int ret;

#if defined(WIN32)
	WSADATA wsd;
	WSAStartup(MAKEWORD(2, 2), &wsd);
#endif

	if (!pidfile)
		return fio_server();

	if (check_existing_pidfile(pidfile)) {
		log_err("fio: pidfile %s exists and server appears alive\n",
								pidfile);
		free(pidfile);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		log_err("fio: failed server fork: %s\n", strerror(errno));
		free(pidfile);
		return -1;
	} else if (pid) {
		/* [한국어] 부모 경로: PID 기록 후 즉시 종료 (데몬 완료) */
		ret = write_pid(pid, pidfile);
		free(pidfile);
		_exit(ret);
	}

	setsid();  /* [한국어] 새 세션 리더 — tty 분리, SIGHUP 격리 */
	openlog("fio", LOG_NDELAY|LOG_NOWAIT|LOG_PID, LOG_USER);
	/* [한국어] LOG_NDELAY=즉시 연결, LOG_PID=로그에 PID 첨부, LOG_USER=일반 유저 시설 */
	log_syslog = true;  /* [한국어] stderr 대신 syslog 로 리다이렉트 지시 */

	file = freopen("/dev/null", "r", stdin);
	if (!file)
		perror("freopen");

	file = freopen("/dev/null", "w", stdout);
	if (!file)
		perror("freopen");

	file = freopen("/dev/null", "w", stderr);
	if (!file)
		perror("freopen");

	f_out = NULL;
	f_err = NULL;

	ret = fio_server();

	fclose(stdin);
	fclose(stdout);
	fclose(stderr);

	closelog();
	unlink(pidfile);
	free(pidfile);
	return ret;
}

/*
 * [한국어]
 * fio_server_set_arg - 외부(init.c)에서 --server=<arg> 원본 문자열 전달받아 저장
 *
 * @arg: 사용자 지정 서버 인자.
 * @return: 없음.
 *
 * strdup 복제 → fio_server() 말미에서 free.
 */
void fio_server_set_arg(const char *arg)
{
	fio_server_arg = strdup(arg);
}

#ifdef WIN32
/*
 * [한국어]
 * fio_server_internal_set (Windows) - 내부 명명된 파이프 이름 설정
 *
 * @arg: 부모가 생성한 파이프 이름 (CreateProcess 인자로 자식에 전달).
 * @return: 없음.
 *
 * 자식 프로세스가 handle_connection_process 에서 이 이름으로 파이프를 열어 부모와 통신.
 */
void fio_server_internal_set(const char *arg)
{
	fio_server_pipe_name = strdup(arg);
}
#endif
