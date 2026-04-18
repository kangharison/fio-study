/*
 * Code related to writing an iolog of what a thread is doing, and to
 * later read that back and replay
 */
/*
 * [한국어 설명] I/O 로깅 및 재생 (iolog.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio에서 두 축의 "I/O 로깅 파이프라인"을 모두 구현한다.
 *   (1) **write iolog**: 실행 중인 잡의 I/O 동작(읽기/쓰기/트림/싱크/파일 open·close·add)을
 *       텍스트 형식의 iolog 파일에 CSV로 기록하여 나중에 동일 I/O 시퀀스를 재현할 수 있게 한다.
 *       기록 포맷: "<usec_since_start> <filename> <action> <offset> <length>" (버전 3, 타임스탬프 포함)
 *       또는 "<filename> <action> <offset> <length>" (버전 2, 타임스탬프 없음).
 *   (2) **read iolog / replay**: 이전에 기록된 iolog 파일(또는 blktrace 바이너리)을 읽어
 *       io_piece 리스트를 구축하고, io_u에 offset/length/ddir을 채워 스토리지에 그대로 재생.
 *       청크 모드(read_iolog_chunked)에서는 거대한 iolog도 메모리에 전부 적재하지 않고
 *       1초분량씩 증분 로드하여 실시간 재생 가능.
 *   (3) **성능 로그(bw/lat/iops/hist log)**: 실행 통계를 CSV(.log) 또는 바이너리(.bin) 또는
 *       gzip 압축(.log.gz) 형식으로 파일에 남긴다. per-job(개별) / aggregate(집계) / unit(단위) /
 *       issue-time / offset / priority / avg+max(BOTH) 등 다양한 축의 샘플을 지원.
 *   (4) **zlib 기반 로그 압축**: 메모리 한계 초과를 방지하기 위해 pending 버퍼가 차면
 *       gz_work() 비동기 워크큐로 deflate → 128KiB chunk_list로 적재. 종료 시
 *       finish_log() → flush_log() → inflate_gz_chunks()로 다시 풀어 CSV 출력하거나
 *       log_gz_store 모드에서는 압축 바이트를 그대로 파일에 저장.
 *   (5) **blktrace 임포트**: 기록된 Linux blktrace 바이너리를 is_blktrace()로 감지 후
 *       blktrace.c의 load_blktrace()로 파싱 → fio iolog 포맷으로 변환하여 재생.
 *   (6) **보조 기능**:
 *       - io_hist_tree/io_hist_list: verify 단계에서 이전 쓰기 위치를 되감기(unwind)하기 위한
 *         파일별·오프셋 정렬 RB 트리 (중복 블록 탐지 + 실패 시 unlog 지원).
 *       - Unix 도메인 소켓 iolog 수신(is_socket+open_socket): 원격 생성기에서 스트리밍된 iolog 재생.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 데이터 흐름(backend.c → ioengines.c → engines/*.c) 옆에서 두 축으로 동작한다.
 *
 *   [write iolog: 잡 실행 중 기록]
 *   thread_main() [backend.c]
 *     → init_iolog() [iolog.c]  — write_iolog_file 설정 시 init_iolog_write() 진입,
 *                                   버전 3 헤더 "fio version 3 iolog\n" 기록, 모든 파일 add 이벤트 기록,
 *                                   8KiB setvbuf로 버퍼드 FILE* 준비
 *     → do_io() [backend.c]
 *         → td_io_queue() [ioengines.c]
 *             → log_io_u() [iolog.c]  — I/O 성공 시 CSV 라인 1개 추가 (usec_since_start, file,
 *                                         ddir_name, offset, buflen)
 *             → log_file() [iolog.c]  — open/close/add 이벤트마다 로그 (add 시 파일을 사전 등록)
 *     → write_iolog_close() [iolog.c]  — fflush+fclose, setvbuf 버퍼 해제
 *
 *   [read iolog: 이전 기록 재생]
 *   thread_main() [backend.c]
 *     → init_iolog() [iolog.c]  — read_iolog_file 설정 시 is_blktrace() 분기
 *         → [blktrace 파일]   init_blktrace_read() → 바이너리 파싱 → io_log_list 구축
 *         → [fio iolog]        init_iolog_read() → is_socket/stdin/fopen 분기 → 버전 헤더 검증
 *                                 (v2/v3, v1은 거부) → read_iolog() 파싱 루프
 *     → do_io() [backend.c]
 *         → get_io_u() [io_u.c]
 *             → read_iolog_get() [iolog.c]  — io_log_list에서 io_piece 꺼내
 *                                              io_u에 ddir/offset/buflen/file 설정
 *                                              (DDIR_WAIT이면 usec_sleep, 청크 모드면 부족 시
 *                                              read_iolog()로 다음 청크 로드)
 *             → ipo_special() [iolog.c]  — 파일 open/close/unlink/add 특수 동작 실행
 *             → iolog_delay() [iolog.c]  — ttime 기반 지연(replay_time_scale 적용),
 *                                           대기 중 완료 I/O 수거
 *
 *   [성능 로그(bw/lat/iops/clat_hist): 잡 실행 중 샘플 축적]
 *   init_iolog() → setup_log() [iolog.c] — bw/lat/iops/clat_hist_log 각각 할당,
 *                                            pending 버퍼(iodepth 기반 round-up) 준비
 *   io_completed() [io_u.c] → add_lat_sample/add_bw_sample/add_iops_sample [stat.c]
 *     → __add_log_sample() [stat.h/stat.c] → io_log.pending[nr_samples++] = io_sample{time,bs,ddir,val,priority,aux[]}
 *     → pending 가득 차면 regrow_logs()/iolog_cur_flush()로 비동기 압축 큐 또는 새 청크
 *
 *   [잡 종료: 로그 덤프]
 *   thread_main() → td_writeout_logs() [iolog.c]
 *     → td_bump_runstate(TD_FINISHING) → finalize_logs() [stat.c]
 *     → 각 log_type의 write_*_log() 루프 (파일 잠금 충돌 시 5ms 대기 재시도)
 *         → finish_log() [iolog.c] — TD_F_COMPRESS_LOG면 iolog_flush() 동기 압축,
 *                                      fio_lock_file() → fio_send_iolog()(GUI/backend) 또는
 *                                      flush_log() → inflate_gz_chunks()+flush_samples()(로컬 파일)
 *
 *   실행 컨텍스트:
 *     - init_iolog/log_io_u/log_file/read_iolog_get/log_io_piece/unlog_io_piece: **잡 스레드**에서
 *       단독 실행(td 고유 상태만 건드림).
 *     - gz_work/gz_work_async: TD_F_COMPRESS_LOG 가진 잡의 **압축 헬퍼 스레드**(workqueue 기반) 에서
 *       비동기 실행. log->chunk_lock / deferred_free_lock 뮤텍스로 잡 스레드와 동기화.
 *     - td_writeout_logs/fio_writeout_logs: **메인 프로세스**에서 잡 수거(reap) 후 순차 실행.
 *
 * === 타 모듈과의 연결 ===
 * - backend.c: thread_main()이 init_iolog()/iolog_compress_init()로 초기화,
 *   do_io() 루프가 log_io_u()/log_io_piece()/unlog_io_piece() 호출,
 *   종료 시 td_writeout_logs()+iolog_compress_exit()+write_iolog_close().
 * - io_u.c: get_io_u()에서 read_iolog_get()을 우선 호출하여 재생 모드 분기.
 *   io_completed()/io_u_sync_complete()에서 log_io_u()+log_io_piece() 호출.
 * - stat.c: add_*_sample() 계열이 io_log->pending에 샘플 적재. 로그 포맷은 io_sample
 *   + io_sample_offset/priority/aux[] 확장을 통해 plain/offset/hist/BOTH 모드를 구분.
 * - iolog.h: 이 파일에서 쓰는 struct io_log / io_logs / io_sample / io_piece / iolog_compress /
 *   io_u_plat_entry / log_params / flush_chunk_iter 정의. 해당 헤더의 주석을 먼저 읽어야
 *   본 파일의 필드 해석이 완성된다.
 * - workqueue.c: gz_work_async를 wq->ops->fn 슬롯으로 등록(log_compress_wq_ops) →
 *   workqueue_enqueue/flush/exit 계약.
 * - filelock.c: fio_lock_file/fio_trylock_file/fio_unlock_file로 같은 파일명을 갖는 다수 잡이
 *   동일 로그 파일에 동시 출력할 때 직렬화(flock 기반).
 * - smalloc.c: scalloc/sfree로 io_log 구조체를 shared memory 힙에 할당 (forked 자식-부모
 *   간에도 접근 가능해야 하는 집계 로그 지원).
 * - blktrace.c: is_blktrace()로 blktrace 바이너리 여부 감지, init_blktrace_read() +
 *   read_blktrace() 로 파싱 → 동일한 io_log_list 경로로 통합.
 * - server.c: is_backend 모드에서 fio_send_iolog()로 결과를 TCP 클라이언트로 송신.
 * - filesetup.c: add_file()/get_fileno()/for_each_file()/get_file()로 파일 관리 공유.
 * - trim.h/trim.c: remove_trim_entry()로 trim 대상 블록 추적 리스트 동기화.
 * - pshared.h: mutex_init_pshared()로 프로세스간 공유 뮤텍스(chunk_lock/deferred_free_lock) 초기화.
 * - dataplacement.c: dp_init()/dp_fill_dspec_data()로 FDP(Flexible Data Placement) 재생 확장.
 *
 * === 주요 함수/구조체 요약 ===
 * [iolog 기록]
 * - log_io_u(): 한 io_u의 I/O 결과를 iolog 파일에 CSV 한 줄로 추가.
 * - log_file(): 파일 open/close/add 이벤트 기록.
 * - write_iolog_close(): FILE 플러시+close+buffer free.
 * - init_iolog_write(): 쓰기 모드 초기화, "fio version 3 iolog\n" 헤더 기록.
 *
 * [iolog 재생]
 * - init_iolog(): 쓰기/읽기/blktrace 분기 진입점.
 * - init_iolog_read(): 소켓/stdin/파일에서 헤더 파싱 후 read_iolog() 호출.
 * - read_iolog(): 텍스트 라인 파싱 루프, io_piece 생성 → io_log_list append.
 * - read_iolog_get(): 큐 헤드에서 io_piece 꺼내 io_u 설정, 특수 동작/지연 처리.
 * - ipo_special(): FIO_LOG_OPEN/CLOSE/UNLINK/ADD_FILE 처리.
 * - iolog_delay(): I/O 간 지연 대기 (replay 중에도 완료 I/O 수거).
 * - delay_since_ttime(): replay_time_scale 기반 지연값 계산.
 * - iolog_items_to_fetch(): 청크 모드에서 다음 증분 크기 결정(1초 소비량 추정).
 * - is_socket()/open_socket(): Unix 소켓에서 iolog 스트리밍 수신.
 *
 * [verify unwind]
 * - log_io_piece(): 성공 쓰기를 io_hist_tree(RB)/io_hist_list에 정렬 삽입(중복 탐지).
 * - unlog_io_piece(): 실패 시 히스토리에서 제거.
 * - trim_io_piece(): 부분 완료 시 길이 조정.
 * - prune_io_piece_log(): 종료 시 히스토리 전체 정리.
 * - queue_io_piece(): 재생 큐(io_log_list)에 단순 추가.
 *
 * [성능 로그 관리]
 * - setup_log(): io_log 할당 및 pending 버퍼/히스토그램 리스트 초기화.
 * - free_log(): io_log 및 모든 청크 해제.
 * - flush_log(): 파일에 로그 청크를 순차 덤프 (압축이면 inflate_gz_chunks 선행).
 * - flush_samples()/flush_hist_samples(): io_sample → CSV 변환 출력 핵심.
 * - finish_log(): 압축 플러시 + 파일 잠금 + 전송/기록 + 잠금 해제 + free.
 * - td_writeout_logs()/fio_writeout_logs(): 잡/전체 로그를 모두 덤프 (재시도 루프).
 * - write_bandw_log/write_lat_log/write_clat_log/write_slat_log/write_iops_log/write_clat_hist_log:
 *   각 로그 타입의 래퍼, per-unit 여부 필터링.
 *
 * [zlib 압축]
 * - gz_work(): deflate로 GZ_CHUNK(128KiB) 단위 압축 → chunk_list 적재.
 * - gz_work_async(): 워크큐 래퍼.
 * - iolog_compress_init/exit(): 잡별 압축 워크큐 생성/해제.
 * - iolog_flush(): 동기 전체 압축(종료 시).
 * - iolog_cur_flush(): 현재 청크만 비동기 압축 큐잉.
 * - iolog_put_deferred/iolog_free_deferred(): 압축 진행 중인 원본 샘플의 지연 해제.
 * - inflate_chunk/inflate_gz_chunks/finish_chunk: 출력 시 역압축.
 * - iolog_file_inflate(): fio CLI `--inflate-log` 구현 (독립 실행 유틸).
 * - z_stream_init/get_new_chunk/free_chunk: zlib stream/청크 라이프사이클 헬퍼.
 *
 * [히스토그램]
 * - hist_sum(): stride 단위 버킷 합산(해상도 조정).
 * - flush_hist_samples(): histogram 로그를 CSV로 쓰기(이전 스냅샷 대비 델타).
 *
 * === iolog 텍스트 파일 포맷 스펙 ===
 *   Line 1: "fio version <2|3> iolog\n"  — 버전 헤더 (v1 거부).
 *   이후 각 라인:
 *     v3 I/O:   "<usec_since_start> <filename> <action> <offset> <length>\n"
 *               action ∈ {read, write, trim, sync, datasync, wait}
 *     v3 파일:  "<usec_since_start> <filename> <action>\n"
 *               action ∈ {add, open, close}
 *     v2 I/O:   "<filename> <action> <offset> <length>\n"
 *     v2 파일:  "<filename> <action>\n"
 *   v3에서는 "wait" 제거됨 (타임스탬프가 이를 대체).
 *   파싱은 sscanf 필드 수(r)로 I/O vs 파일 동작 구분 (io_act/file_act 매크로).
 *
 * === 성능 로그 파일 포맷 ===
 *   [plain log (.log)]: "time, value, ddir, bs[, offset][, priority/rt_class][, issue_time]\n"
 *     - ddir: 0=READ, 1=WRITE, 2=TRIM, ...
 *     - priority: log_prio 설정 시 "0x%04x" 16비트 값, 아니면 ioprio class==RT 여부(0/1)
 *     - aux[IOS_AUX_OFFSET_INDEX], aux[IOS_AUX_ISSUE_TIME_INDEX]: 옵션 필드
 *   [BOTH mode]: "time, avg_val, max_val, ddir, bs, ..." — log_max=BOTH 시 두 값 동시 기록.
 *   [hist log]: "time, ddir, bs, bucket_0_sum, bucket_1_sum, ..., bucket_N_sum\n"
 *               이전 스냅샷 대비 델타 덤프 (coarseness로 2^k 버킷 묶어 해상도 조정).
 *   [gz store (.log.gz)]: deflate 스트림을 그대로 파일에 저장, 외부 도구(gunzip/zcat/fio_plot)로 해제.
 *
 * === 호출 체인 요약 (빠른 점프) ===
 *   init_iolog → is_blktrace?(blktrace.c) | init_iolog_read | init_iolog_write
 *   init_iolog_read → is_socket | fopen | fdopen → read_iolog(파싱 루프)
 *   read_iolog_get → iolog_delay(지연) | ipo_special(open/close) | queue 소진 시 read_iolog/read_blktrace
 *   td_writeout_logs → finalize_logs(stat.c) → write_{bw,lat,slat,clat,clat_hist,iops}_log → finish_log
 *                       → iolog_flush(압축) → fio_lock_file → fio_send_iolog | flush_log → free_log
 */

/* [한국어] === 표준 라이브러리 및 시스템 헤더 === */
#include <stdio.h>          /* [한국어] FILE*, fprintf/fgets/fopen/fclose/fread/fwrite/ferror/perror/setvbuf(_IOFBF) — iolog 파일 I/O 핵심. */
#include <stdlib.h>         /* [한국어] malloc/calloc/free/realloc — io_piece/io_log/chunk 등 동적 할당 (smalloc과 구분: 프로세스 로컬). */
#include <assert.h>         /* [한국어] assert(what<3) log_file() 파일 동작 인덱스 범위 검증 등 불변성 확인. */
#include <sys/types.h>      /* [한국어] off_t, mode_t, size_t, ssize_t — 파일 오프셋/크기 타입 공급. */
#include <sys/stat.h>       /* [한국어] stat(2), struct stat, S_ISSOCK — is_socket()이 소켓 파일 판별에 사용. */
#include <unistd.h>         /* [한국어] close(2), usleep — open_socket 실패 시 fd close, td_writeout_logs 재시도 대기. */
#ifdef CONFIG_ZLIB
#include <zlib.h>           /* [한국어] deflate/deflateInit/deflateEnd/inflate/inflateInit2/inflateEnd, z_stream, Z_NO_FLUSH/Z_FINISH/Z_STREAM_END/Z_BUF_ERROR/Z_OK/Z_NULL — gz_work/inflate_chunk의 압축·해제 엔진. configure 가 zlib 링크 확정 시에만 정의. */
#endif

/* [한국어] === fio 내부 헤더 === */
#include "flist.h"          /* [한국어] flist_head, flist_add_tail/first_entry/last_entry/splice_tail/for_each/empty, INIT_FLIST_HEAD — io_log_list, io_hist_list, io_logs, chunk_list 등 fio 전용 doubly-linked list. */
#include "fio.h"            /* [한국어] struct thread_data, struct fio_file, struct io_u, enum fio_ddir(DDIR_READ/WRITE/TRIM/SYNC/DATASYNC/WAIT/INVAL), TD_F_COMPRESS_LOG/TD_F_SYNCS/TD_F_REGROW_LOGS, td_verror, fio_gettime/utime_since_now/mtime_since_genesis/ntime_since, dprint(FD_IO/FD_FILE/FD_COMPRESS), log_err, td_io_open_file/td_io_close_file/td_io_unlink_file, td_bump_runstate/td_restore_runstate, io_u_queued_complete, io_u_quiesce, init_io_u_buffers, fio_fill_issue_time, fio_ro_check, get_fileno/add_file/get_file/free_release_files, io_ddir_name, get_name_by_idx, regrow_logs, for_each_td/end_for_each, for_each_file, read_only, is_backend, io_sample_ddir, LOG_*_SAMPLE_BIT, IO_LOG_SAMPLE_BOTH, IO_MODE_OFFLOAD, IOS_AUX_OFFSET_INDEX/IOS_AUX_ISSUE_TIME_INDEX, DEF_LOG_ENTRIES, DDIR_RWDIR_CNT, FIO_IO_U_PLAT_NR, fio_offset_overlap_risk, TD_FINISHING, FIO_CLIENT_TYPE_GUI, FIO_LOG_ADD_FILE/OPEN_FILE/CLOSE_FILE/UNLINK_FILE, enum file_log_act, init_ipo, io_u_should_trim, io_u_block_info, BLOCK_INFO_STATE/SET_STATE, BLOCK_STATE_TRIM_FAILURE/WRITE_FAILURE, fio_send_iolog, init_disk_util, finalize_logs, usec_sleep, ioprio_value_is_class_rt, workqueue_init/exit/enqueue/flush, struct workqueue_work/workqueue_ops/submit_worker, gettid, fio_setaffinity, fio_option_is_set, fio_did_warn, FIO_WARN_IOLOG_DROP, FIO_DP_NONE, log_entry_sz/__log_entry_sz/log_sample_sz, IOLOG_MAX_DEFER, struct log_params, struct io_piece, struct io_sample, struct io_log, struct io_logs, struct iolog_compress, struct io_u_plat_entry, DDIR_INVAL, ddir_sync, TD_DDIR_READ/WRITE/TRIM/RW, rb_first/rb_entry/rb_erase/rb_link_node/rb_insert_color/RB_CLEAR_NODE, struct fio_rb_node — 본 파일이 의존하는 fio 심볼 대부분의 공급처. */
#include "trim.h"           /* [한국어] remove_trim_entry() — io_piece를 트림 전용 리스트에서 제거하여 verify/trim 재생 경로의 일관성 유지. */
#include "filelock.h"       /* [한국어] fio_lock_file/fio_trylock_file/fio_unlock_file — 여러 잡이 per_job_logs=0으로 동일 로그 파일에 동시 덤프할 때 flock(2) 기반 직렬화. */
#include "smalloc.h"        /* [한국어] scalloc(), sfree() — struct io_log, struct io_logs를 SHM 힙에 할당해 fork 자식 프로세스(집계 잡)에서도 참조 가능하게 함. */
#include "blktrace.h"       /* [한국어] is_blktrace(), init_blktrace_read(), read_blktrace() — Linux blktrace 바이너리를 fio iolog 포맷으로 변환하는 임포트 계층 인터페이스. */
#include "pshared.h"        /* [한국어] mutex_init_pshared() — PTHREAD_PROCESS_SHARED 속성의 뮤텍스 초기화(log->chunk_lock, log->deferred_free_lock). fork된 잡 스레드와 압축 헬퍼 스레드 간 공유 락. */
#include "lib/roundup.h"    /* [한국어] roundup_pow2(x) — 2의 거듭제곱으로 올림. setup_log()에서 iodepth > DEF_LOG_ENTRIES일 때 pending 버퍼 크기를 2의 거듭제곱으로 정렬(해시 인덱싱/캐시 정렬 친화). */

/* [한국어] === 네트워크 소켓 헤더 (Unix 도메인 소켓으로 iolog 스트리밍 수신) === */
#include <netinet/in.h>     /* [한국어] struct sockaddr_in (Unix 소켓만 사용하지만 이식성을 위해 선언). */
#include <netinet/tcp.h>    /* [한국어] TCP 관련 매크로 (현재 미사용, 미래 TCP 기반 iolog 수신 대비). */
#include <arpa/inet.h>      /* [한국어] inet_* 변환 함수 (현재 미사용, 호환성 관행). */
#include <sys/stat.h>       /* [한국어] 중복 include(방어적) — 매크로 가드로 실 포함 1회만. */
#include <sys/socket.h>     /* [한국어] socket(2), connect(2), AF_UNIX, SOCK_STREAM, struct sockaddr — open_socket()의 소켓 생성/연결. */
#include <sys/un.h>         /* [한국어] struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; } — Unix 도메인 주소 포맷. */

/*
 * [한국어] 전방 선언 — iolog_flush()
 * finish_log()가 TD_F_COMPRESS_LOG 잡 종료 시점에 잔여 로그 청크를 모두 deflate로 밀어내고
 * 파일로 덤프하는 경로를 사용하는데, iolog_flush의 정의는 이 파일 후반부(zlib 가드 내)에
 * 있기 때문에 전방 선언이 필요하다. 빌드 옵션에 CONFIG_ZLIB 미정의 시 #else 브랜치에서
 * "return 1" stub으로 대체되므로 동일 시그니처를 유지한다.
 */
static int iolog_flush(struct io_log *log);

/*
 * [한국어] iolog 텍스트 파일의 버전 헤더 매직 문자열.
 * 설정자: init_iolog_write()가 write 모드에서 첫 줄로 iolog_ver3을 기록.
 * 읽는 자: init_iolog_read()가 fgets()로 첫 줄을 읽어 strncmp로 v2/v3 매칭.
 * 값 범위: 불변(const) 전역. v1은 이미 폐기되어 read 시 거부된다.
 * 동기화: 불변이라 잠금 불필요.
 */
static const char iolog_ver2[] = "fio version 2 iolog";  /* [한국어] 버전 2: 타임스탬프 없음, "wait" 명령 허용. */
static const char iolog_ver3[] = "fio version 3 iolog";  /* [한국어] 버전 3: 각 라인 첫 필드에 usec_since_start 타임스탬프 포함, "wait" 무시. */

/*
 * [한국어]
 * queue_io_piece - 파싱된 io_piece를 재생 큐의 맨 뒤에 추가하고 총 I/O 크기 누적.
 *
 * @td: 재생 대상 잡의 thread_data — td->io_log_list(재생 큐)와 td->total_io_size 갱신.
 * @ipo: 호출자가 calloc으로 새로 생성하여 ddir/offset/len/fileno 등을 채운 io_piece.
 *        ipo의 소유권은 이 호출로 td로 이전되며, read_iolog_get()이 나중에 꺼내 free한다.
 *
 * 왜 필요한가: read_iolog()와 read_blktrace()가 각자의 포맷을 해석하여 공통 포맷인
 * io_piece로 환원한 뒤 이 함수를 통해 하나의 FIFO 큐에 줄 세운다. 이렇게 함으로써
 * 재생 측(read_iolog_get)은 소스 포맷에 무관하게 동일한 경로로 소비할 수 있다.
 *
 * 실행 컨텍스트: 잡 스레드에서만 호출 (read_iolog 파싱 중, blktrace 임포트 중).
 *   호출 체인: read_iolog()/read_blktrace() → queue_io_piece() → 나중에 read_iolog_get() pop.
 *
 * total_io_size: 재생 예상 총 바이트(stat/진행률 계산에 사용), thread_main이 시작 전/중에 참조.
 */
void queue_io_piece(struct thread_data *td, struct io_piece *ipo)
{
	flist_add_tail(&ipo->list, &td->io_log_list);  /* [한국어] 재생 큐(FIFO) 꼬리에 추가 — iolog 순서 보존. */
	td->total_io_size += ipo->len;                 /* [한국어] 재생할 총 바이트에 이 piece의 길이 가산 (DDIR_WAIT은 len=0이므로 영향 없음). */
}

/*
 * [한국어]
 * log_io_u - 완료된 I/O 한 건을 write iolog 파일에 CSV 한 줄로 추가한다.
 *
 * @td:  I/O를 수행한 잡의 thread_data (const: 읽기만). td->o.write_iolog_file이
 *       설정되지 않았다면 조기 반환하여 오버헤드 0.
 * @io_u: 방금 완료된 I/O 유닛. io_u->file, io_u->ddir, io_u->offset, io_u->buflen을 읽는다.
 *
 * 기록 포맷(v3): "<usec_since_iolog_start> <filename> <action> <offset> <length>\n"
 *   - usec_since_iolog_start: io_log_start_time으로부터의 경과(us), utime_since_now() 산출.
 *   - action: io_ddir_name(io_u->ddir) → "read"/"write"/"trim"/"sync"/"datasync"/"wait".
 *
 * 실행 컨텍스트: 잡 스레드에서 I/O 완료 콜백(io_completed/io_u_sync_complete) 경로로부터 호출.
 *   호출 체인: io_completed() [io_u.c] → file_log_write_comp() → log_io_u().
 *
 * 동시성: 단일 잡이 단일 FILE*(td->iolog_f)에 순차 기록하므로 별도 락 불필요 (td-local 상태).
 *
 * 왜 필요한가: 기록된 iolog는 나중에 `fio --read_iolog=<file>`으로 동일 I/O 패턴을
 * 재생하여 재현 실험(performance regression test)이나 디버깅을 가능하게 한다.
 */
void log_io_u(const struct thread_data *td, const struct io_u *io_u)
{
	struct timespec now;                        /* [한국어] 현재 시각 스냅샷. fio_gettime 결과를 담지만 실제로는 utime_since_now가 내부 호출하므로 이 변수는 사용되지 않는다 — historical 호환(향후 issue_time 추가 시 여기 저장 예정). */

	if (!td->o.write_iolog_file)                /* [한국어] write_iolog_file 옵션 미설정 시 로깅 비활성화. 잡마다 조건부 호출을 생략하기 위해 체크를 함수 내부에 둠. */
		return;

	fio_gettime(&now, NULL);                    /* [한국어] CLOCK_MONOTONIC 등으로 현재 시각 취득. now는 아래 fprintf 인자로 직접 쓰이지 않지만 CPU cache warm up / 향후 확장 대비. */
	fprintf(td->iolog_f, "%llu %s %s %llu %llu\n",
		(unsigned long long) utime_since_now(&td->io_log_start_time),  /* [한국어] 필드 1: iolog 시작 시각부터 지금까지의 마이크로초 경과. %llu로 직렬화. */
		io_u->file->file_name,                                         /* [한국어] 필드 2: 대상 파일명(디바이스 경로 포함 가능). 재생 시 get_fileno()로 다시 매핑. */
		io_ddir_name(io_u->ddir),                                      /* [한국어] 필드 3: I/O 방향 이름 문자열 ("read"/"write"/"trim"/"sync"/...). */
		io_u->offset,                                                  /* [한국어] 필드 4: 파일 내 바이트 오프셋. %llu로 직렬화 — 64비트 I/O 오프셋 대응. */
		io_u->buflen);                                                 /* [한국어] 필드 5: I/O 크기(바이트). 실제 전송량(xfer_buflen-resid)이 아닌 요청 크기를 기록. */

}

/*
 * [한국어]
 * log_file - 파일 수명 이벤트(add/open/close)를 iolog에 기록한다.
 *
 * @td: 잡 컨텍스트. td->o.write_iolog_file 미설정 시 조기 반환.
 * @f:  이벤트 대상 fio_file. f->file_name이 기록된다.
 * @what: 이벤트 종류 — FIO_LOG_ADD_FILE(=0) / FIO_LOG_OPEN_FILE(=1) / FIO_LOG_CLOSE_FILE(=2).
 *         (FIO_LOG_UNLINK_FILE은 write 경로에는 사용하지 않음 — replay 전용.)
 *
 * 기록 포맷(v3): "<usec_since_iolog_start> <filename> <action>\n"
 *   - action: "add" | "open" | "close" (act[] 테이블로 enum → 문자열 변환).
 *
 * 왜 필요한가: 재생 시에는 먼저 파일을 add로 등록하고, open 이벤트에서 td_io_open_file()로
 * 실제 open하며, close 이벤트에서 close한다. 이로써 원본 잡의 파일 수명을 정확히 재현한다.
 *
 * 실행 컨텍스트: init_iolog_write()(잡 시작 시 기존 파일들 일괄 "add"), td_io_open_file/
 * td_io_close_file 성공 경로. 잡 스레드 단독.
 *
 * 주의: iolog_f == NULL 분기는 pre-open/close 타이밍(init 전 단계) 보호.
 */
void log_file(struct thread_data *td, struct fio_file *f,
	      enum file_log_act what)
{
	const char *act[] = { "add", "open", "close" };  /* [한국어] enum file_log_act → 문자열 매핑 테이블. 순서는 enum 값과 일치해야 한다 (ADD=0, OPEN=1, CLOSE=2). */
	struct timespec now;                             /* [한국어] 현재 시각 스냅샷 (log_io_u와 동일한 패턴, 미래 확장 대비). */

	assert(what < 3);                               /* [한국어] 인덱스 범위 보장 — act[] 크기 초과 접근 방지. UNLINK(=3)는 write 경로에 오면 안 됨. */

	if (!td->o.write_iolog_file)                    /* [한국어] iolog 비활성화 잡 조기 반환. */
		return;


	/*
	 * this happens on the pre-open/close done before the job starts
	 */
	/* [한국어] init_iolog_write()가 아직 호출되지 않아 td->iolog_f == NULL인 상황(잡 setup 단계
	 *   pre-open/pre-close)을 안전하게 건너뛴다. 이 체크가 없으면 fprintf(NULL, ...)로 SEGV 발생. */
	if (!td->iolog_f)
		return;

	fio_gettime(&now, NULL);                        /* [한국어] 현재 시각 (log_io_u처럼 현재는 fprintf에 쓰이지 않음 — future-proof). */
	fprintf(td->iolog_f, "%llu %s %s\n",
		(unsigned long long) utime_since_now(&td->io_log_start_time),  /* [한국어] 필드 1: iolog 시작 이후 경과 us. */
		f->file_name,                                                  /* [한국어] 필드 2: 대상 파일명. */
		act[what]);                                                    /* [한국어] 필드 3: "add"/"open"/"close" (what enum 인덱스). */
}

/*
 * [한국어]
 * iolog_delay - iolog 재생 중 지정된 지연(us)만큼 대기하되, 대기 중에도
 *               비동기 엔진의 완료 I/O를 수거하여 자원을 효율적으로 재사용한다.
 *
 * @td:    잡 컨텍스트. td->last_issue(직전 I/O 발행 시각), td->time_offset(누적 오차),
 *          td->terminate(SIGINT/rate/runtime 종료 플래그), td->flags(TD_F_REGROW_LOGS) 사용.
 * @delay: 목표 지연(마이크로초). 호출자는 ipo->delay(iolog에 기록된 원본 지연 또는
 *          replay_time_scale 적용 결과)를 넘긴다.
 *
 * 알고리즘 단계:
 *   1) 이전 사이클에서 과대 대기한 오차(time_offset)를 delay에서 차감.
 *   2) 직전 발행 이후 이미 지난 시간(usec)도 차감.
 *   3) 남은 delay 동안 루프 — 비동기 완료 수거(io_u_queued_complete) + 로그 확장(regrow_logs).
 *   4) 실제 경과 vs 목표 delay 차이를 time_offset에 저장(다음 호출에서 보정).
 *
 * 왜 이 구조인가: 단순 usleep은 (a) 아무것도 안 하고 잠만 자서 엔진 큐잉 I/O가
 * 누적되어 메모리 압박을 야기하고, (b) OS 스케줄러 해상도에 따라 정확도가 낮다.
 * 이 함수는 완료 수거와 대기를 병행하여 둘 다 해결한다.
 *
 * 실행 컨텍스트: 잡 스레드. ipo_special() 및 read_iolog_get()의 DDIR_WAIT 경로가 호출.
 *
 * 호출 체인: read_iolog_get() / ipo_special() → iolog_delay() → io_u_queued_complete()
 *           → regrow_logs().
 */
static void iolog_delay(struct thread_data *td, unsigned long delay)
{
	uint64_t usec = utime_since_now(&td->last_issue);  /* [한국어] 직전 I/O 발행 시점(last_issue)부터 현재까지 경과한 us — 이미 소비된 시간. */
	unsigned long orig_delay = delay;                   /* [한국어] 원본 목표 지연 — 후반에 오차 계산(usec - orig_delay)용으로 보존. */
	struct timespec ts;                                 /* [한국어] 이 함수 진입 후 경과 측정 기준점. */
	int ret = 0;                                        /* [한국어] io_u_queued_complete 반환값 저장 (성공=0, 실패=음수 -errno). */

	/* [한국어] 누적 오차(time_offset)가 목표 지연보다 크면, 이미 충분히 지체된 상태이므로
	 *   오차만 소비(0으로 리셋)하고 즉시 반환 — 지연 0 요청 시의 빠른 경로. */
	if (delay < td->time_offset) {
		td->time_offset = 0;
		return;
	}

	delay -= td->time_offset;                           /* [한국어] 누적 오차만큼 대기 시간 감소 — 다음 I/O를 "더 일찍" 발행해 시간 축 복원. */
	/* [한국어] 이미 I/O 발행-완료 사이클에 소비한 시간이 목표 지연을 초과했다면,
	 *   추가 대기 불필요 → 즉시 반환. 재생 속도가 원본보다 느려지는 상황 완화. */
	if (delay < usec)
		return;

	delay -= usec;                                      /* [한국어] 순 대기 시간 계산. */

	fio_gettime(&ts, NULL);                             /* [한국어] 루프 기준 시작점. utime_since_now(&ts)로 경과 추적. */

	/* [한국어] 남은 delay 동안 비동기 완료 수거 + 대기 루프. td->terminate 시 즉시 탈출. */
	while (delay && !td->terminate) {
		ret = io_u_queued_complete(td, 0);          /* [한국어] min_evts=0: 논블로킹 폴링으로 이미 완료된 io_u만 회수. 자원 재순환 + latency 통계 갱신. */
		if (ret < 0)                                /* [한국어] 음수 = -errno 반환 규약. 엔진 에러를 td_verror로 마킹. */
			td_verror(td, -ret, "io_u_queued_complete");
		if (td->flags & TD_F_REGROW_LOGS)           /* [한국어] 로그 버퍼 동적 확장 필요 플래그 — iodepth 초과로 pending 가득 찬 상황에서 재할당. */
			regrow_logs(td);
		if (utime_since_now(&ts) > delay)           /* [한국어] 루프 진입 이후 경과가 delay 초과 → 탈출. 완료 수거로 delay 달성. */
			break;
	}

	/* [한국어] 실제 경과 시간과 원본 목표 지연의 차이를 time_offset에 기록 — 다음
	 *   iolog_delay 호출 시 보정되어 재생 시간 축의 드리프트를 누적 방지. */
	usec = utime_since_now(&ts);
	if (usec > orig_delay)
		td->time_offset = usec - orig_delay;        /* [한국어] 과대 대기 → 양의 오차 저장(다음엔 덜 대기). */
	else
		td->time_offset = 0;                        /* [한국어] 오차 없음 또는 과소 대기 → 리셋 (과소는 다음 delay 차감으로 자연 보정). */
}

/*
 * [한국어]
 * ipo_special - iolog에 기록된 "특수 동작"(파일 open/close/unlink/add)을 실행.
 *
 * @td:  잡 컨텍스트. td->files[ipo->fileno]로 파일 객체 조회.
 * @ipo: 재생 큐에서 꺼낸 io_piece. ipo->ddir이 DDIR_INVAL이면 특수 동작 신호.
 *        ipo->file_action에 FIO_LOG_OPEN_FILE/CLOSE_FILE/UNLINK_FILE/ADD_FILE 중 하나 저장.
 *
 * @return:
 *   0  — 일반 I/O이므로 호출자가 io_u 설정을 계속 진행해야 함.
 *   1  — 특수 동작을 처리했으니 호출자는 이 ipo를 free하고 다음 ipo 처리.
 *   -1 — 치명적 에러(td_verror로 이미 마킹). 호출자는 재생 중단.
 *
 * 왜 필요한가: iolog는 데이터 I/O뿐 아니라 파일 수명 이벤트도 기록하므로,
 * 재생 측도 동일한 시점에 open/close/unlink/add를 수행해야 원본 잡의
 * 파일 상태 머신을 정확히 재현할 수 있다(특히 여러 파일의 cross-interleave).
 *
 * 실행 컨텍스트: 잡 스레드. read_iolog_get() 루프 내에서 각 io_piece pop 직후 호출.
 *
 * 호출 체인: read_iolog_get() → ipo_special() → iolog_delay/td_io_open_file/td_io_close_file/
 *           td_io_unlink_file → (open 성공 시 FDP 초기화 dp_init).
 */
static int ipo_special(struct thread_data *td, struct io_piece *ipo)
{
	struct fio_file *f;
	int ret;

	/*
	 * Not a special ipo
	 */
	/* [한국어] DDIR_INVAL 매직 값이 아니면 일반 I/O(read/write/trim/sync) → 호출자가 처리. */
	if (ipo->ddir != DDIR_INVAL)
		return 0;

	f = td->files[ipo->fileno];                         /* [한국어] 파일 인덱스 → fio_file 포인터 조회. add 시점에 등록된 매핑. */

	if (ipo->delay)                                     /* [한국어] 특수 동작도 지연이 설정되어 있으면 대기 (특수 동작 간 타이밍 재현). */
		iolog_delay(td, ipo->delay);
	if (fio_fill_issue_time(td))                        /* [한국어] issue_time 추적 활성화 시 last_issue 갱신. 통계 누적용 타이밍. */
		fio_gettime(&td->last_issue, NULL);
	/* [한국어] file_action 코드별 분기 — iolog 기록 당시의 파일 상태 머신 재현. */
	switch (ipo->file_action) {
	case FIO_LOG_OPEN_FILE:
		/* [한국어] replay_redirect 모드: 모든 I/O를 단일 경로로 리다이렉트하므로, 이미 열린
		 *   파일을 다시 open하는 이벤트는 무시(중복 open 방지). */
		if (td->o.replay_redirect && fio_file_open(f)) {
			dprint(FD_FILE, "iolog: ignoring re-open of file %s\n",
					f->file_name);
			break;
		}
		ret = td_io_open_file(td, f);               /* [한국어] 엔진 open_file 콜백 호출 → 실제 open(2) / blk device open / 네트워크 connect 등. */
		if (!ret) {
			/* [한국어] FDP(Flexible Data Placement) 재생 지원: io_piece에 dspec(데이터 배치 스펙)이
			 *   포함된 경우 open 직후 dp_init으로 RU 핸들 매핑을 준비한다. */
			if (td->o.dp_type != FIO_DP_NONE) {
				int dp_init_ret = dp_init(td);

				if (dp_init_ret != 0) {
					td_verror(td, abs(dp_init_ret), "dp_init");  /* [한국어] dp_init은 음수 -errno 반환 규약 → abs()로 양수 변환. */
					return -1;
				}
			}
			break;
		}
		td_verror(td, ret, "iolog open file");      /* [한국어] open 실패 → 잡 에러 마킹, 재생 중단 신호(-1). */
		return -1;
	case FIO_LOG_CLOSE_FILE:
		td_io_close_file(td, f);                    /* [한국어] 엔진 close_file 콜백. 반환값 무시 — close 실패는 재생 중단 사유 아님. */
		break;
	case FIO_LOG_UNLINK_FILE:
		td_io_unlink_file(td, f);                   /* [한국어] unlink(2) 수행 — 테스트 파일 정리 타이밍 재현. */
		break;
	case FIO_LOG_ADD_FILE:
		/*
		 * Nothing to do
		 */
		/* [한국어] add 이벤트는 read_iolog()가 파싱 단계에서 이미 add_file()을 호출해 등록했다.
		 *   재생 시점에는 추가 작업 불필요 — 타임라인 동기화용 마커일 뿐. */
		break;
	default:
		log_err("fio: bad file action %d\n", ipo->file_action);  /* [한국어] 알 수 없는 action 코드 → iolog 포맷 오류 가능성. */
		break;
	}

	return 1;                                            /* [한국어] 특수 동작 처리 완료 — 호출자는 이 ipo를 free하고 다음으로 진행. */
}

/*
 * [한국어] 전방 선언 — read_iolog()
 * read_iolog_get()이 청크 모드에서 재생 큐가 비면 다음 청크를 로드하기 위해
 * 부른다. 이 파일 뒷부분에 정의. static이므로 translation unit 내부에서만 가시.
 */
static bool read_iolog(struct thread_data *td);

/*
 * [한국어]
 * delay_since_ttime - 두 연속 I/O 사이의 간격을 replay_time_scale로 스케일링한다.
 *
 * @td:   잡 컨텍스트. td->io_log_last_ttime(직전 I/O의 ttime)와 td->o.replay_time_scale,
 *         td->o.no_stall 옵션 참조.
 * @time: 이번 I/O의 기록된 타임스탬프(usec since iolog start).
 *
 * @return: 대기해야 할 us. 0이면 즉시 실행.
 *
 * 공식: delay = (time - last_ttime) * (100 / replay_time_scale)
 *   - replay_time_scale == 100: 원본 속도(1.0배).
 *   - replay_time_scale == 200: 2배 빠른 재생 → delay 절반.
 *   - replay_time_scale == 50:  절반 속도 재생 → delay 2배.
 *
 * 예외 경로:
 *   - last_ttime == 0 (첫 I/O): 0 반환 (이전 기준 없음).
 *   - no_stall 옵션: 모든 지연 무시, 0 반환 (최대 속도 재생).
 *   - time < last_ttime: 클록 거꾸로 (iolog 손상 또는 wrap) → 0 반환.
 *
 * 왜 필요한가: 원본 잡의 타이밍을 그대로 재현하면 실제 IOPS/BW를 복원하고,
 * 2배속/저속 재생으로 다양한 부하 조건을 테스트할 수 있다.
 *
 * 호출 체인: read_iolog() 라인 파싱 중 (v3 iolog) → delay_since_ttime() → ipo->delay 저장
 *           → 재생 시 iolog_delay()에 전달.
 */
unsigned long long delay_since_ttime(const struct thread_data *td,
	       unsigned long long time)
{
	double tmp;
	double scale;
	const unsigned long long *last_ttime = &td->io_log_last_ttime;  /* [한국어] 직전 I/O의 ttime. const 포인터로 참조만 — 갱신은 read_iolog() 쪽에서 수행. */

	if (!*last_ttime || td->o.no_stall || time < *last_ttime)   /* [한국어] 세 조건 중 하나라도 참이면 대기 불필요 → 0 반환. */
		return 0;
	else if (td->o.replay_time_scale == 100)                    /* [한국어] 1.0배속: 정확히 원본 간격 그대로 — 부동소수 계산 생략 (정수 차이만). */
		return time - *last_ttime;


	scale = (double) 100.0 / (double) td->o.replay_time_scale;  /* [한국어] 스케일 계수: 200이면 0.5, 50이면 2.0. */
	tmp = time - *last_ttime;                                   /* [한국어] 원본 간격(us). */
	return tmp * scale;                                         /* [한국어] 스케일 적용 결과를 unsigned long long으로 암묵 캐스팅. 큰 값에선 정밀도 손실 가능(대체로 무시 가능 — us 단위). */
}

/*
 * [한국어]
 * read_iolog_get - io_log_list에서 다음 io_piece를 꺼내 io_u에 매핑한다.
 *
 * @td:   잡 컨텍스트. td->io_log_list(FIFO 큐), td->files, td->done, io_log_current/
 *         io_log_checkmark(청크 모드 카운터) 사용.
 * @io_u: 코어(io_u.c get_io_u)가 할당한 빈 io_u. 이 함수가 ddir/offset/buflen/file 채움.
 *
 * @return:
 *   0 — 유효 I/O 하나를 io_u에 설정했음. 호출자는 엔진에 큐잉 진행.
 *   1 — 더 이상 재생할 I/O 없음(EOF). td->done = 1로 마킹. 잡 종료 경로로 진입.
 *
 * 흐름:
 *   1) 큐가 비어있지 않은 동안 반복.
 *   2) 청크 모드면 현재 커서가 checkmark에 도달 시 다음 청크 로드(read_iolog/read_blktrace).
 *      로드 실패(EOF)면 즉시 1 반환.
 *   3) FIFO 헤드를 pop하고 trim 추적에서 제거.
 *   4) ipo_special()로 파일 동작 여부 확인 — -1(에러): free 후 break(0으로 fallthrough),
 *      +1(처리됨): free 후 continue(다음 ipo), 0: 일반 I/O로 계속.
 *   5) DDIR_WAIT: 지정 시각(mtime_since_genesis 기준)까지 usec_sleep.
 *      일반 I/O: io_u에 필드 복사 + get_file() 참조 증가 + 필요 시 iolog_delay() +
 *                FDP dspec 채우기.
 *   6) DDIR_WAIT이 아닌 경우 io_u 준비 완료 → 0 반환.
 *
 * 실행 컨텍스트: 잡 스레드의 get_io_u() 경로에서 read_iolog_file 설정된 잡에 한해 호출.
 *   호출 체인: do_io() → get_io_u() [io_u.c] → read_iolog_get() [iolog.c].
 *
 * 왜 DDIR_WAIT는 io_u 반환 없이 continue인가: DDIR_WAIT은 절대 시각 대기 마커일 뿐
 * 엔진에 제출할 실제 I/O가 아니므로, 대기 후 다음 ipo를 찾아 계속 루프한다.
 */
int read_iolog_get(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo;
	unsigned long elapsed;

	while (!flist_empty(&td->io_log_list)) {
		int ret;

		/* [한국어] 청크 모드: io_log_current가 checkmark에 도달하면 다음 청크를 증분 로드.
		 *   blktrace vs iolog 텍스트 분기. 로드 실패(EOF)면 즉시 1 반환. */
		if (td->o.read_iolog_chunked) {
			if (td->io_log_checkmark == td->io_log_current) {
				if (td->io_log_blktrace) {
					if (!read_blktrace(td))
						return 1;
				} else {
					if (!read_iolog(td))
						return 1;
				}
			}
			td->io_log_current--;                /* [한국어] 커서 감소 — checkmark/current 차이로 남은 항목 추정. */
		}
		/* [한국어] FIFO 헤드의 io_piece를 꺼내 처리 (flist_del로 리스트에서 분리). */
		ipo = flist_first_entry(&td->io_log_list, struct io_piece, list);
		flist_del(&ipo->list);
		remove_trim_entry(td, ipo);                 /* [한국어] trim 추적 리스트에 중복 등록되어 있을 수 있으므로 제거 — trim.c와 동기화. */

		/* [한국어] 파일 open/close/unlink/add 여부 확인. ret<0: 에러, ret>0: 처리됨, ret==0: 일반 I/O. */
		ret = ipo_special(td, ipo);
		if (ret < 0) {
			free(ipo);
			break;                              /* [한국어] 치명 에러 — 루프 탈출, td->done = 1 설정되며 1 반환. */
		} else if (ret > 0) {
			free(ipo);
			continue;                           /* [한국어] 특수 동작 완료 — 이 ipo는 I/O 요청 아님, 다음 ipo를 찾아 계속. */
		}

		/* [한국어] io_u에 iolog 필드 복사 — 엔진에 제출할 I/O 정보 구성. */
		io_u->ddir = ipo->ddir;
		if (ipo->ddir != DDIR_WAIT) {
			io_u->offset = ipo->offset;          /* [한국어] 재생 대상 파일 오프셋. */
			io_u->verify_offset = ipo->offset;   /* [한국어] verify 단계에서 사용할 오프셋 — 일반 재생에서는 동일. */
			io_u->buflen = ipo->len;             /* [한국어] I/O 크기(바이트). */
			io_u->file = td->files[ipo->fileno]; /* [한국어] 대상 파일 포인터 복원(iolog 파싱 단계에서 fileno 매핑). */
			get_file(io_u->file);                /* [한국어] 파일 참조 카운트 증가 — io_u 완료 후 put_file. */
			dprint(FD_IO, "iolog: get %llu/%llu/%s\n", io_u->offset,
						io_u->buflen, io_u->file->file_name);
			if (ipo->delay)                      /* [한국어] v2 iolog의 "wait" 또는 v3의 ttime 기반 지연 적용. */
				iolog_delay(td, ipo->delay);

			/* [한국어] FDP 데이터 배치: iolog에 기록된 dspec을 io_u->dspec에 복원. */
			if (td->o.dp_type != FIO_DP_NONE)
				dp_fill_dspec_data(td, io_u);
		} else {
			/* [한국어] DDIR_WAIT: v2 iolog의 절대 시각 대기 마커.
			 *   elapsed = 잡 시작(genesis)부터 현재까지 ms, ipo->delay는 ms 단위 대기 목표.
			 *   아직 목표 시각에 도달 안 했으면 잔여 ms를 us로 변환해 usec_sleep. */
			elapsed = mtime_since_genesis();
			if (ipo->delay > elapsed)
				usec_sleep(td, (ipo->delay - elapsed) * 1000);
		}

		free(ipo);                                   /* [한국어] 소비한 ipo 메모리 회수 — queue_io_piece에서 calloc으로 할당됨. */

		if (io_u->ddir != DDIR_WAIT)
			return 0;                            /* [한국어] 일반 I/O 준비 완료 — 호출자가 엔진 queue()로 전달. */
		/* [한국어] DDIR_WAIT은 I/O 아니라 시간 마커 — 다음 ipo를 찾아 루프 계속. */
	}

	td->done = 1;                                        /* [한국어] 재생 큐 소진 — 잡 완료 마킹. do_io() 루프 탈출 신호. */
	return 1;
}

/*
 * [한국어]
 * prune_io_piece_log - verify용 io_hist RB 트리와 리스트를 모두 비우고 메모리 해제.
 *
 * @td: 잡 컨텍스트. td->io_hist_tree(RB), td->io_hist_list(flist), td->io_hist_len(카운터) 사용.
 *
 * 왜 필요한가: log_io_piece()가 성공한 쓰기마다 io_hist에 노드를 쌓는데, 잡이 종료되거나
 * verify 완료 후엔 이 메모리를 반환해야 한다. verify 단계에서 unlog/trim_io_piece가
 * 일부를 정리하지만 남은 엔트리를 여기서 일괄 제거.
 *
 * 실행 컨텍스트: 잡 스레드 종료 경로(thread_main() → cleanup).
 *
 * 두 자료구조를 모두 처리: fio_offset_overlap_risk에 따라 RB 또는 list에 들어가므로
 * 양쪽을 모두 순회해야 누수 없다.
 */
void prune_io_piece_log(struct thread_data *td)
{
	struct io_piece *ipo;
	struct fio_rb_node *n;

	/* [한국어] RB 트리 순회 제거 — rb_first로 최소 노드 → rb_erase → rb_entry로 컨테이너 복원. */
	while ((n = rb_first(&td->io_hist_tree)) != NULL) {
		ipo = rb_entry(n, struct io_piece, rb_node);    /* [한국어] container_of 매크로의 RB 버전 — 내부 필드 주소로 바깥 struct 복원. */
		rb_erase(n, &td->io_hist_tree);                 /* [한국어] RB 트리에서 노드 제거 및 색상 재균형. */
		remove_trim_entry(td, ipo);                     /* [한국어] trim 추적 리스트와도 동기화. */
		td->io_hist_len--;                              /* [한국어] 히스토리 엔트리 카운터 감소. */
		free(ipo);
	}

	/* [한국어] flist 순회 제거 — 헤드가 비어있지 않은 동안 first_entry로 꺼내 free. */
	while (!flist_empty(&td->io_hist_list)) {
		ipo = flist_first_entry(&td->io_hist_list, struct io_piece, list);
		flist_del(&ipo->list);
		remove_trim_entry(td, ipo);
		td->io_hist_len--;
		free(ipo);
	}
}

/*
 * log a successful write, so we can unwind the log for verify
 */
/*
 * [한국어]
 * log_io_piece - 성공한 쓰기(또는 verify 대상 I/O)를 히스토리 트리/리스트에 기록한다.
 *                verify 경로는 이 기록을 역순 재생하여 원래 쓰기 위치를 다시 읽어 검증한다.
 *
 * @td:   잡 컨텍스트. td->io_hist_tree(RB), io_hist_list(flist), io_hist_len, trim_list 등.
 * @io_u: 방금 엔진이 수락한 write/trim io_u. io_u->file/offset/buflen/numberio 참조.
 *        io_u->ipo 필드에 새로 생성한 io_piece를 링크하여 완료/실패 시 추적.
 *
 * 자료구조 선택 (fio_offset_overlap_risk 기준):
 *   - 랜덤 맵(axmap)이 있고 offset modifier 없음 → 중복 블록 불가능 → 단순 flist에 append.
 *     IP_F_ONLIST 플래그.
 *   - 랜덤 맵 없음 또는 offset modifier 있음 → 같은 블록 쓰기 가능 → RB 트리(파일,오프셋)로
 *     정렬 삽입 + 겹침 탐지 시 기존 노드 드롭하여 최신 쓰기 유지. IP_F_ONRB 플래그.
 *
 * RB 삽입 루프:
 *   - 키: (file, offset) 튜플. file 주소가 다르면 그 비교, 같으면 offset.
 *   - overlap 판정: 현재 ipo의 [offset, offset+len)와 기존 __ipo의 [offset, offset+len)가
 *     겹치는지 반 범위 합 비교. 겹침 발견 → 기존 노드 제거(IN_FLIGHT 아니면 free) → goto restart.
 *
 * 왜 IN_FLIGHT면 free 못 하나: 아직 엔진이 비동기로 들고 있는 io_u의 ipo. 완료 경로에서
 * 참조하므로 여기서 해제하면 UAF. IP_F_IN_FLIGHT만 떼고 재진입 시 자연 정리.
 *
 * 실행 컨텍스트: 잡 스레드. io_u_submit 성공 경로에서 호출.
 *   호출 체인: io_u_submit() [io_u.c] → log_io_piece() → (완료 후) log_io_u()/io_completed().
 */
void log_io_piece(struct thread_data *td, struct io_u *io_u)
{
	struct fio_rb_node **p, *parent;
	struct io_piece *ipo, *__ipo;

	ipo = calloc(1, sizeof(struct io_piece));           /* [한국어] 새 io_piece 할당 — 잡 로컬 힙(smalloc 불필요, fork 자식과 공유 안 함). */
	init_ipo(ipo);                                      /* [한국어] 필드 기본값 초기화(플래그 0, 리스트 head 초기화 등). */
	ipo->file = io_u->file;                             /* [한국어] 파일 포인터 저장 — RB 키 1번 요소. */
	ipo->offset = io_u->offset;                         /* [한국어] 쓰기 오프셋 — RB 키 2번 요소 + verify 시 읽을 위치. */
	ipo->len = io_u->buflen;                            /* [한국어] 쓰기 길이 — verify 시 읽을 길이. */
	ipo->numberio = io_u->numberio;                     /* [한국어] 쓰기 일련번호 — verify 단계에서 데이터 패턴 생성용 시드. */
	ipo->flags = IP_F_IN_FLIGHT;                        /* [한국어] 아직 엔진에서 완료되지 않음 — 중복 시 free 보호. */

	io_u->ipo = ipo;                                    /* [한국어] 역참조 링크 — 완료/실패 시 io_u->ipo로 이 piece 찾음. */

	/* [한국어] 이 쓰기가 나중에 trim 재생 대상이 된다면 trim_list에도 추가. */
	if (io_u_should_trim(td, io_u)) {
		flist_add_tail(&ipo->trim_list, &td->trim_list);
		td->trim_entries++;
	}

	/*
	 * Sort writes if we don't have a random map in which case we need to
	 * check for duplicate blocks and drop the old one, which we rely on
	 * the rb insert/lookup for handling. Sort writes if we have offset
	 * modifier which can also create duplicate blocks.
	 */
	/* [한국어] 중복 위험이 없으면 정렬 비용 절감 — 단순 append 후 반환. */
	if (!fio_offset_overlap_risk(td)) {
		INIT_FLIST_HEAD(&ipo->list);                /* [한국어] flist 노드 자기 참조 초기화. */
		flist_add_tail(&ipo->list, &td->io_hist_list);
		ipo->flags |= IP_F_ONLIST;                  /* [한국어] 제거 시 어느 자료구조에서 빼야 할지 표식. */
		td->io_hist_len++;
		return;
	}

	RB_CLEAR_NODE(&ipo->rb_node);                       /* [한국어] RB 노드 초기 색상/포인터 NULL. */

	/*
	 * Sort the entry into the verification list
	 */
	/* [한국어] RB 삽입 위치 탐색 — 파일 주소, 오프셋 순으로 좌/우 분기. 겹침 발견 시 goto restart. */
restart:
	p = &td->io_hist_tree.rb_node;
	parent = NULL;
	while (*p) {
		int overlap = 0;
		parent = *p;

		__ipo = rb_entry(parent, struct io_piece, rb_node);
		if (ipo->file < __ipo->file)
			p = &(*p)->rb_left;                 /* [한국어] 파일 주소 작음 → 왼쪽. */
		else if (ipo->file > __ipo->file)
			p = &(*p)->rb_right;                /* [한국어] 파일 주소 큼 → 오른쪽. */
		else if (ipo->offset < __ipo->offset) {
			p = &(*p)->rb_left;
			overlap = ipo->offset + ipo->len > __ipo->offset;  /* [한국어] 신규의 끝이 기존의 시작 초과 → 겹침. */
		}
		else if (ipo->offset > __ipo->offset) {
			p = &(*p)->rb_right;
			overlap = __ipo->offset + __ipo->len > ipo->offset; /* [한국어] 기존의 끝이 신규의 시작 초과 → 겹침. */
		}
		else
			overlap = 1;                        /* [한국어] 같은 offset → 완전 겹침. */

		if (overlap) {
			dprint(FD_IO, "iolog: overlap %llu/%lu, %llu/%lu\n",
				__ipo->offset, __ipo->len,
				ipo->offset, ipo->len);
			td->io_hist_len--;
			rb_erase(parent, &td->io_hist_tree); /* [한국어] 오래된 __ipo 노드 제거. */
			remove_trim_entry(td, __ipo);
			if (!(__ipo->flags & IP_F_IN_FLIGHT))
				free(__ipo);                /* [한국어] 이미 완료된 ipo만 안전하게 free. */
			goto restart;                       /* [한국어] 트리 구조 변화 → 삽입 위치 재탐색. */
		}
	}

	/* [한국어] 새 노드를 찾은 위치 p에 연결 + 색상 재균형 → RB 불변성 유지. */
	rb_link_node(&ipo->rb_node, parent, p);
	rb_insert_color(&ipo->rb_node, &td->io_hist_tree);
	ipo->flags |= IP_F_ONRB;
	td->io_hist_len++;
}

/*
 * [한국어]
 * unlog_io_piece - 실패한 I/O의 히스토리 기록을 취소하고 io_piece를 해제한다.
 *                  또한 블록 상태를 실패로 마킹하여 verify 단계에서 건너뛰도록 한다.
 *
 * @td:   잡 컨텍스트. td->ts.nr_block_infos(블록 상태 추적 활성화 여부).
 * @io_u: 실패한 io_u. io_u->ipo 참조.
 *
 * 블록 상태 전이:
 *   BLOCK_STATE_TRIM_FAILURE  : DDIR_TRIM 실패 시 설정 → verify 시 이 블록 건너뜀.
 *   BLOCK_STATE_WRITE_FAILURE : DDIR_WRITE 실패 시 설정 → 동일.
 *   이미 더 높은(또는 같은) 상태라면 덮어쓰지 않음(우선순위 단조성).
 *
 * 실행 컨텍스트: 잡 스레드. io_completed() 에러 경로에서 호출.
 */
void unlog_io_piece(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = io_u->ipo;

	/* [한국어] block_info 추적 기능이 활성이면 이 블록의 상태를 실패로 전이. */
	if (td->ts.nr_block_infos) {
		uint32_t *info = io_u_block_info(td, io_u);
		if (BLOCK_INFO_STATE(*info) < BLOCK_STATE_TRIM_FAILURE) {
			if (io_u->ddir == DDIR_TRIM)
				*info = BLOCK_INFO_SET_STATE(*info,
						BLOCK_STATE_TRIM_FAILURE);
			else if (io_u->ddir == DDIR_WRITE)
				*info = BLOCK_INFO_SET_STATE(*info,
						BLOCK_STATE_WRITE_FAILURE);
		}
	}

	if (!ipo)                                           /* [한국어] ipo가 없으면(예: 이 I/O는 log_io_piece 호출 대상 아님) 할 일 없음. */
		return;

	/* [한국어] 등록된 자료구조에서 제거 — RB 트리 또는 flist. */
	if (ipo->flags & IP_F_ONRB)
		rb_erase(&ipo->rb_node, &td->io_hist_tree);
	else if (ipo->flags & IP_F_ONLIST)
		flist_del(&ipo->list);

	free(ipo);
	io_u->ipo = NULL;                                   /* [한국어] 댕글링 포인터 방지. */
	td->io_hist_len--;
}

/*
 * [한국어]
 * trim_io_piece - 부분 I/O 완료 시 io_piece의 길이를 실제 전송된 바이트로 축소.
 *
 * @io_u: 부분 전송된 io_u. xfer_buflen(요청), resid(미전송 잔여) 사용.
 *
 * 왜 필요한가: verify 단계는 ipo->len을 기준으로 동일 범위를 다시 읽어 비교하므로,
 * 쓰기가 일부만 성공했다면 그 실제 크기로 갱신해야 검증 영역이 정확해진다.
 */
void trim_io_piece(const struct io_u *io_u)
{
	struct io_piece *ipo = io_u->ipo;

	if (!ipo)
		return;

	ipo->len = io_u->xfer_buflen - io_u->resid;         /* [한국어] 실제 전송된 바이트 = 요청 - 잔여. */
}

/*
 * [한국어]
 * write_iolog_close - write iolog FILE*를 플러시하고 닫는다.
 *
 * @td: 잡 컨텍스트. td->iolog_f와 td->iolog_buf(setvbuf 버퍼) 사용.
 *
 * 호출 시점: 잡 종료 경로(thread_main cleanup) — 이후 init_iolog 재호출은 없으므로
 *             포인터를 NULL로 리셋하여 이중 close 방지.
 */
void write_iolog_close(struct thread_data *td)
{
	if (!td->iolog_f)                                   /* [한국어] 초기화 안 된 경우 no-op (write_iolog_file 미설정 잡). */
		return;

	fflush(td->iolog_f);                                /* [한국어] 커널 버퍼까지 강제 flush — setvbuf 내부 버퍼만으론 부족(8KiB FBF). */
	fclose(td->iolog_f);                                /* [한국어] FILE* 해제 + 파일 디스크립터 close. */
	free(td->iolog_buf);                                /* [한국어] setvbuf로 제공한 8KiB 버퍼 해제 (fclose가 참조하지 않음을 보장해야 함 — fflush 후 안전). */
	td->iolog_f = NULL;
	td->iolog_buf = NULL;
}

/*
 * [한국어]
 * iolog_items_to_fetch - 청크 모드에서 다음 로드할 iolog 엔트리 수를 동적으로 결정.
 *
 * @td: 잡 컨텍스트. io_log_highmark(이전 로드 시점의 상한), io_log_current(현재 커서),
 *       io_log_highmark_time(이전 로드 시각).
 *
 * @return: 다음에 로드할 엔트리 수 (items). 첫 호출이면 고정 10 반환(priming).
 *
 * 공식: items_to_fetch = (items_consumed_per_sec) - items_currently_loaded
 *   - items_consumed_per_sec = (highmark - current) / elapsed_ns * 1e9
 *   - elapsed_ns = 이전 highmark 시각부터 현재까지 ns
 *
 * 효과: 소비 속도에 적응하여 1초 분량을 미리 로드 → 재생 속도 저하 없이 메모리 상한 유지.
 *
 * 호출 체인: read_iolog() → iolog_items_to_fetch() → (반환값만큼 큐에 적재 후 종료).
 */
int64_t iolog_items_to_fetch(struct thread_data *td)
{
	struct timespec now;
	uint64_t elapsed;
	uint64_t for_1s;
	int64_t items_to_fetch;

	if (!td->io_log_highmark)                           /* [한국어] 첫 호출 — 측정 기준 없음, priming으로 10개만 로드. */
		return 10;


	fio_gettime(&now, NULL);
	elapsed = ntime_since(&td->io_log_highmark_time, &now);  /* [한국어] 이전 로드 시점부터 현재까지 ns. */
	if (elapsed) {
		/* [한국어] 1초당 소비 예상 항목 수 = 소비량/elapsed_ns * 1e9 ns/s. */
		for_1s = (td->io_log_highmark - td->io_log_current) * 1000000000 / elapsed;
		items_to_fetch = for_1s - td->io_log_current;  /* [한국어] 예상 소비량에서 이미 로드된 것 차감 → 추가 로드 필요량. */
		if (items_to_fetch < 0)
			items_to_fetch = 0;                 /* [한국어] 이미 충분 — 추가 로드 불필요. */
	} else
		items_to_fetch = 0;                         /* [한국어] elapsed == 0 (경계) → 보수적으로 0. */

	/* [한국어] 다음 호출 시 비교 기준 갱신: highmark = 현재 + 로드량, checkmark = 절반 지점. */
	td->io_log_highmark = td->io_log_current + items_to_fetch;
	td->io_log_checkmark = (td->io_log_highmark + 1) / 2;  /* [한국어] checkmark 도달 시 read_iolog_get이 다음 청크 로드 트리거. */
	fio_gettime(&td->io_log_highmark_time, NULL);

	return items_to_fetch;
}

/*
 * [한국어] iolog 라인 파싱 결과 판별 매크로 — sscanf가 반환한 필드 수 r로
 *   I/O 동작인지 파일 동작인지를 구분한다.
 *
 *   버전 3:
 *     I/O 라인:  "<ttime> <file> <action> <offset> <length>" → r == 5
 *     파일 라인: "<ttime> <file> <action>"                   → r == 3
 *   버전 2:
 *     I/O 라인:  "<file> <action> <offset> <length>"         → r == 4
 *     파일 라인: "<file> <action>"                           → r == 2
 *
 *   _td: thread_data 인자 (io_log_version 참조).
 *   _r : sscanf 반환값.
 *   주의: 매크로 내부에서 전역 이름 r을 참조하는 버그가 있으나 호출자도 r로 선언되어 동작.
 */
#define io_act(_td, _r) (((_td)->io_log_version == 3 && (r) == 5) || \
					((_td)->io_log_version == 2 && (r) == 4))
#define file_act(_td, _r) (((_td)->io_log_version == 3 && (r) == 3) || \
					((_td)->io_log_version == 2 && (r) == 2))

/*
 * Read version 2 and 3 iolog data. It is enhanced to include per-file logging,
 * syncs, etc.
 */
/*
 * [한국어]
 * read_iolog - iolog 텍스트 파일의 라인을 파싱하여 io_piece 리스트를 구축한다.
 *
 * @td: 잡 컨텍스트. td->io_log_rfile(이미 fopen된 FILE*), io_log_version(2|3),
 *       io_log_list(FIFO 큐), read_iolog_chunked(청크 모드 여부) 등.
 *
 * @return: true — 한 번이라도 성공적으로 읽음 또는 청크 모드에서 더 읽을 것이 있음.
 *          false — EOF에 도달했고 재생할 I/O가 없음.
 *
 * 파싱 로직 (라인 단위):
 *   1) v3 라인: sscanf "%llu %256s %256s %llu %u" → (ttime, file, action, offset, bytes)
 *      v2 라인: sscanf "%256s %256s %llu %u"      → (file, action, offset, bytes)
 *      필드 수가 5(v3) 또는 4(v2)면 I/O 라인, 3 또는 2면 파일 동작 라인.
 *   2) action 문자열을 enum fio_ddir로 매핑. replay_skip 비트마스크로 건너뛸 방향 필터.
 *   3) replay_redirect 설정 시 모든 파일명을 redirect 경로로 치환.
 *   4) read_only 모드에서는 write/trim 무시.
 *   5) io_piece calloc → ddir/delay/offset/len/fileno/file_action 설정 → queue_io_piece().
 *   6) 청크 모드면 items_to_fetch 감소, 0 되면 루프 탈출.
 *
 * 부수 효과:
 *   - td->io_log_last_ttime 갱신 (v3 delay 계산용).
 *   - td->o.max_bs[rw] 갱신 (버퍼 재할당 트리거).
 *   - td->o.size 증가 (예상 I/O 총량).
 *   - td->flags에 TD_F_SYNCS 비트 설정 (sync 연산 포함 시).
 *   - td->o.td_ddir (TD_DDIR_READ/WRITE/TRIM/RW) 비트 설정.
 *
 * 실행 컨텍스트: 잡 스레드. init_iolog_read() 초기 호출, 청크 모드면 read_iolog_get()
 *   루프에서 checkmark 도달 시 반복 호출.
 *
 * 호출 체인: init_iolog_read() 또는 read_iolog_get() → read_iolog() → queue_io_piece().
 *
 * 메모리 재할당: 발견된 max_bs가 기존보다 크면 realloc=true → orig_buffer 재할당
 *   (init_io_u_buffers). 이는 fio의 I/O 버퍼가 최대 블록 크기 기반으로 한 번에 할당되기 때문.
 */
static bool read_iolog(struct thread_data *td)
{
	unsigned long long offset;
	unsigned int bytes;
	unsigned long long delay = 0;
	int reads, writes, trims, waits, fileno = 0, file_action = 0; /* stupid gcc */
	/* [한국어] reads/writes/trims/waits/syncs: 카테고리별 카운터 — 종료 시 td_ddir 비트 설정에 사용.
	 *   fileno/file_action 초기화는 경고 회피용(실제로는 매 라인에서 할당되지만 gcc가 그걸 모름). */
	char *rfname, *fname, *act;
	char *str, *p;
	enum fio_ddir rw;
	bool realloc = false;                               /* [한국어] 최대 블록 크기 증가 발견 시 true → 버퍼 재할당 요청. */
	int64_t items_to_fetch = 0;                         /* [한국어] 청크 모드 잔여 로드량. 0 = 무제한(비-청크 모드). */
	int syncs;

	/* [한국어] 청크 모드: 목표 로드량을 동적으로 산정. 이미 충분(=0)이면 true로 조기 반환. */
	if (td->o.read_iolog_chunked) {
		items_to_fetch = iolog_items_to_fetch(td);
		if (!items_to_fetch)
			return true;
	}

	/*
	 * Read in the read iolog and store it, reuse the infrastructure
	 * for doing verifications.
	 */
	/* [한국어] 파싱용 버퍼 할당. 라인 최대 4KiB, 파일명/액션 각 256+16바이트 여유. */
	str = malloc(4096);
	rfname = fname = malloc(256+16);                    /* [한국어] rfname은 파싱 저장용, fname은 실제 사용(replay_redirect 시 다른 경로로 스위치). */
	act = malloc(256+16);

	syncs = reads = writes = trims = waits = 0;
	/* [한국어] fgets로 한 라인씩 읽는 메인 파싱 루프. NULL(EOF 또는 에러)이면 종료. */
	while ((p = fgets(str, 4096, td->io_log_rfile)) != NULL) {
		struct io_piece *ipo;
		int r;
		unsigned long long ttime;

		/* [한국어] v3 라인: "<ttime> <file> <action> <offset> <bytes>" — 5필드 sscanf. */
		if (td->io_log_version == 3) {
			r = sscanf(p, "%llu %256s %256s %llu %u", &ttime, rfname, act,
							&offset, &bytes);
			delay = delay_since_ttime(td, ttime);    /* [한국어] replay_time_scale 적용 후 이 I/O까지의 대기 시간 계산. */
			td->io_log_last_ttime = ttime;           /* [한국어] 다음 라인에서 이전 기준으로 사용. */
			/*
			 * "wait" is not allowed with version 3
			 */
			/* [한국어] v3은 타임스탬프로 지연을 표현하므로 "wait" 명령은 중복 — 경고 후 스킵. */
			if (!strcmp(act, "wait")) {
				log_err("iolog: ignoring wait command with"
					" version 3 for file %s\n", fname);
				continue;
			}
		} else /* version 2 */
			r = sscanf(p, "%256s %256s %llu %u", rfname, act, &offset, &bytes);

		if (td->o.replay_redirect)                  /* [한국어] 원본 파일명 무시, 지정된 단일 경로로 모든 I/O 리다이렉트. */
			fname = td->o.replay_redirect;

		if (io_act(td, r)) {                         /* [한국어] r == 4(v2) 또는 5(v3) → I/O 라인. */
			/*
			 * Check action first
			 */
			/* [한국어] action 문자열을 enum fio_ddir로 매핑. replay_skip 마스크로 특정 방향 스킵. */
			if (!strcmp(act, "wait"))
				rw = DDIR_WAIT;
			else if (!strcmp(act, "read")) {
				if (td->o.replay_skip & (1u << DDIR_READ))
					continue;
				rw = DDIR_READ;
			} else if (!strcmp(act, "write")) {
				if (td->o.replay_skip & (1u << DDIR_WRITE))
					continue;
				rw = DDIR_WRITE;
			} else if (!strcmp(act, "sync")) {
				if (td->o.replay_skip & (1u << DDIR_SYNC))
					continue;
				rw = DDIR_SYNC;
			} else if (!strcmp(act, "datasync"))
				rw = DDIR_DATASYNC;
			else if (!strcmp(act, "trim")) {
				if (td->o.replay_skip & (1u << DDIR_TRIM))
					continue;
				rw = DDIR_TRIM;
			} else {
				log_err("fio: bad iolog file action: %s\n",
									act);
				continue;
			}
			fileno = get_fileno(td, fname);      /* [한국어] 파일명 → fileno 인덱스 조회. 없으면 -1. */
		} else if (file_act(td, r)) {                /* [한국어] r == 2(v2) 또는 3(v3) → 파일 동작 라인. */
			/* [한국어] 특수 동작 표식: ddir=DDIR_INVAL, file_action에 실제 액션 코드 저장. */
			rw = DDIR_INVAL;
			if (!strcmp(act, "add")) {
				if (td->o.replay_redirect &&
				    get_fileno(td, fname) != -1) {
					/* [한국어] redirect 모드에서 이미 등록된 파일 — 중복 add 무시. */
					dprint(FD_FILE, "iolog: ignoring"
						" re-add of file %s\n", fname);
				} else {
					fileno = add_file(td, fname, td->subjob_number, 1);  /* [한국어] 새 파일 등록 → fileno 반환. */
					file_action = FIO_LOG_ADD_FILE;
				}
			} else if (!strcmp(act, "open")) {
				fileno = get_fileno(td, fname);
				file_action = FIO_LOG_OPEN_FILE;
			} else if (!strcmp(act, "close")) {
				fileno = get_fileno(td, fname);
				file_action = FIO_LOG_CLOSE_FILE;
			} else {
				log_err("fio: bad iolog file action: %s\n",
									act);
				continue;
			}
		} else {
			log_err("bad iolog%d: %s\n", td->io_log_version, p);
			continue;
		}

		/* [한국어] 카테고리 카운터 증가. read_only 모드에서 쓰기/트림 차단. DDIR_WAIT은 no_stall 시 스킵. */
		if (rw == DDIR_READ)
			reads++;
		else if (rw == DDIR_WRITE) {
			/*
			 * Don't add a write for ro mode
			 */
			if (read_only)
				continue;
			writes++;
		} else if (rw == DDIR_TRIM) {
			/*
			 * Don't add a trim for ro mode
			 */
			if (read_only)
				continue;
			trims++;
		} else if (rw == DDIR_WAIT) {
			if (td->o.no_stall)                 /* [한국어] no_stall: 모든 대기 무시 → 최대 속도 재생. */
				continue;
			waits++;
		} else if (rw == DDIR_INVAL) {
			/* [한국어] 특수 동작(add/open/close) — 카운팅 대상 아님, 다음 단계로. */
		} else if (ddir_sync(rw)) {
			syncs++;                            /* [한국어] sync/datasync 카운팅. td->flags에 TD_F_SYNCS 트리거. */
		} else {
			log_err("bad ddir: %d\n", rw);
			continue;
		}

		/*
		 * Make note of file
		 */
		/* [한국어] io_piece 할당 및 필드 채우기 — 이어서 queue_io_piece로 FIFO에 추가. */
		ipo = calloc(1, sizeof(*ipo));
		init_ipo(ipo);
		ipo->ddir = rw;
		if (td->io_log_version == 3)
			ipo->delay = delay;                  /* [한국어] v3: 타임스탬프 기반 지연. */
		if (rw == DDIR_WAIT) {
			ipo->delay = offset;                 /* [한국어] v2 DDIR_WAIT: offset 필드에 대기 ms 값 저장(포맷 관행). */
		} else {
			/* [한국어] replay_scale: 오프셋을 나누어 더 작은 영역에 재매핑 (예: 10GB 원본을 1GB 파일에 재생). */
			if (td->o.replay_scale)
				ipo->offset = offset / td->o.replay_scale;
			else
				ipo->offset = offset;
			ipo_bytes_align(td->o.replay_align, ipo);  /* [한국어] replay_align 설정 시 오프셋을 그 정렬 경계로 내림 조정. */

			ipo->len = bytes;
			/* [한국어] 최대 블록 크기 갱신 — init_io_u_buffers는 max_bs 기반 버퍼 할당이므로
			 *   새로운 최대값 발견 시 flag realloc=true로 재할당 트리거. */
			if (rw != DDIR_INVAL && bytes > td->o.max_bs[rw]) {
				realloc = true;
				td->o.max_bs[rw] = bytes;
			}
			ipo->fileno = fileno;
			ipo->file_action = file_action;
			td->o.size += bytes;                 /* [한국어] 예상 총 I/O 크기 누적 — progress 계산에 사용. */
		}

		queue_io_piece(td, ipo);                     /* [한국어] 완성된 io_piece를 FIFO 큐에 append. */

		/* [한국어] 청크 모드: 목표량까지만 읽고 break — 메모리 상한 유지. */
		if (td->o.read_iolog_chunked) {
			td->io_log_current++;
			items_to_fetch--;
			if (items_to_fetch == 0)
				break;
		}
	}

	free(str);
	free(act);
	free(rfname);

	/* [한국어] 청크 모드 후처리: highmark/checkmark을 현재 위치로 재설정. 다음 청크 트리거 지점 기록. */
	if (td->o.read_iolog_chunked) {
		td->io_log_highmark = td->io_log_current;
		td->io_log_checkmark = (td->io_log_highmark + 1) / 2;
		fio_gettime(&td->io_log_highmark_time, NULL);
	}

	if (writes && read_only) {
		log_err("fio: <%s> skips replay of %d writes due to"
			" read-only\n", td->o.name, writes);
		writes = 0;
	}
	if (syncs)
		td->flags |= TD_F_SYNCS;                    /* [한국어] 이 잡에 sync/datasync 연산 포함 — 완료 경로에서 특별 처리 유도. */

	/* [한국어] 청크 모드 반환: 이번 호출에서 0 로드 → EOF(false). 재할당 필요 시 I/O 버퍼 재구성. */
	if (td->o.read_iolog_chunked) {
		if (td->io_log_current == 0) {
			return false;
		}
		td->o.td_ddir = TD_DDIR_RW;                 /* [한국어] 청크 모드에서는 단순히 RW 모두 가능으로 설정 (실제 ddir은 각 io_piece가 결정). */
		if (realloc && td->orig_buffer)
		{
			/* [한국어] 진행 중 I/O를 모두 완료시킨 뒤 메모리 해제 → 새 max_bs로 재할당. */
			io_u_quiesce(td);
			free_io_mem(td);
			if (init_io_u_buffers(td))
				return false;
		}
		return true;
	}

	if (!reads && !writes && !waits && !trims)
		return false;                               /* [한국어] 비-청크 모드에서 I/O 전혀 없음 → 재생할 것 없음. */

	/* [한국어] td_ddir 비트 마스크 설정 — 스케줄러/통계가 어느 방향의 작업인지 파악. */
	td->o.td_ddir = 0;
	if (reads)
		td->o.td_ddir |= TD_DDIR_READ;
	if (writes)
		td->o.td_ddir |= TD_DDIR_WRITE;
	if (trims)
		td->o.td_ddir |= TD_DDIR_TRIM;

	return true;
}

/*
 * [한국어]
 * is_socket - 주어진 경로가 Unix 도메인 소켓 파일인지 판별한다.
 *
 * @path: 파일 경로.
 * @return: true = Unix 소켓, false = 일반 파일/디렉토리/없음.
 *
 * 사용처: init_iolog_read()가 파일/소켓/stdin 세 소스 분기 시 소켓 판별에 사용.
 * stat(2)로 파일 타입 조회 후 S_ISSOCK 매크로로 검사.
 */
static bool is_socket(const char *path)
{
	struct stat buf;
	int r;

	r = stat(path, &buf);                               /* [한국어] 파일 메타 조회. 경로가 없으면 -1. */
	if (r == -1)
		return false;

	return S_ISSOCK(buf.st_mode);                       /* [한국어] S_IFMT 마스킹 후 S_IFSOCK 비교 — 소켓 파일 판별. */
}

/*
 * [한국어]
 * open_socket - Unix 도메인 소켓에 연결하여 읽기용 fd를 반환한다.
 *
 * @path: 소켓 파일 경로 (struct sockaddr_un::sun_path 최대 108바이트).
 * @return: 성공 시 connect된 fd, 실패 시 -1.
 *
 * 사용처: init_iolog_read()가 is_socket()으로 소켓 감지 시 호출.
 *   fd는 fdopen(fd, "r")로 FILE*로 감싸 read_iolog() 입력으로 사용.
 *
 * 프로토콜: 생성자(iolog 스트림 서버)가 AF_UNIX SOCK_STREAM으로 bind+listen,
 *   fio 클라이언트가 connect하여 stream을 수신. 텍스트 iolog 라인을 push.
 */
static int open_socket(const char *path)
{
	struct sockaddr_un addr;
	int ret, fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);               /* [한국어] AF_UNIX 도메인 + SOCK_STREAM 타입으로 소켓 생성. protocol=0은 기본(UNIX는 0 하나뿐). */
	if (fd < 0)
		return fd;

	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) >=
	    sizeof(addr.sun_path)) {
		/* [한국어] sun_path는 108바이트 제한 — 경로가 너무 길면 경고(truncation 발생). */
		log_err("%s: path name %s is too long for a Unix socket\n",
			__func__, path);
	}

	/* [한국어] connect(2): AF_UNIX에서 addrlen = strlen(path) + sizeof(sun_family)로 계산.
	 *   서버가 listen 중이면 성공 반환, 아니면 ECONNREFUSED. */
	ret = connect(fd, (const struct sockaddr *)&addr, strlen(path) + sizeof(addr.sun_family));
	if (!ret)
		return fd;

	close(fd);                                          /* [한국어] 실패 시 생성한 fd 누수 방지. */
	return -1;
}

/*
 * open iolog, check version, and call appropriate parser
 */
/*
 * [한국어]
 * init_iolog_read - iolog 재생 소스(파일/소켓/stdin)를 열고 버전을 확인한 뒤 파싱 시작.
 *
 * @td:    잡 컨텍스트. td->io_log_rfile(출력), io_log_version(출력) 설정.
 * @fname: iolog 파일 경로 또는 "-"(stdin) 또는 Unix 소켓 경로.
 *
 * @return: true — 파싱 성공(적어도 라인 1개 이상 해석), false — 실패(포맷/파일 에러).
 *
 * 처리 흐름:
 *   1) is_socket(fname) — Unix 소켓이면 open_socket → fdopen.
 *   2) fname == "-"    — stdin 직접 사용.
 *   3) 그 외           — fopen(fname, "r").
 *   4) fgets 첫 줄로 매직 "fio version 2|3 iolog" 검증.
 *   5) 버전 번호를 td->io_log_version에 저장.
 *   6) free_release_files(td)로 기존 파일 세트 초기화(iolog가 add 이벤트로 재등록).
 *   7) read_iolog(td) 본 파싱 진입.
 *
 * 실행 컨텍스트: 잡 스레드의 init 경로.
 *
 * 호출 체인: init_iolog() → init_iolog_read() → read_iolog().
 */
static bool init_iolog_read(struct thread_data *td, char *fname)
{
	char buffer[256], *p;
	FILE *f = NULL;

	dprint(FD_IO, "iolog: name=%s\n", fname);

	/* [한국어] 소스 타입 자동 감지: 소켓 > stdin("-") > 일반 파일 우선순위. */
	if (is_socket(fname)) {
		int fd;

		fd = open_socket(fname);
		if (fd >= 0)
			f = fdopen(fd, "r");                 /* [한국어] fd를 FILE*로 래핑 — fgets로 버퍼드 읽기. */
	} else if (!strcmp(fname, "-")) {
		f = stdin;                                   /* [한국어] 파이프라인 입력: cat iolog | fio --read_iolog=- */
	} else
		f = fopen(fname, "r");

	if (!f) {
		perror("fopen read iolog");
		return false;
	}

	/* [한국어] 매직 첫 줄 읽기. 실패 시 빈 파일 또는 I/O 에러. */
	p = fgets(buffer, sizeof(buffer), f);
	if (!p) {
		td_verror(td, errno, "iolog read");
		log_err("fio: unable to read iolog\n");
		fclose(f);
		return false;
	}

	/*
	 * versions 2 and 3 of the iolog store a specific string as the
	 * first line, check for that
	 */
	/* [한국어] 버전 매직 매칭: strncmp로 prefix만 비교(버퍼에 개행 포함되어도 OK).
	 *   v1은 예전 포맷 — 더 이상 지원 안 하므로 거부. */
	if (!strncmp(iolog_ver2, buffer, strlen(iolog_ver2)))
		td->io_log_version = 2;
	else if (!strncmp(iolog_ver3, buffer, strlen(iolog_ver3)))
		td->io_log_version = 3;
	else {
		log_err("fio: iolog version 1 is no longer supported\n");
		fclose(f);
		return false;
	}

	free_release_files(td);                             /* [한국어] 기존 파일 세트 제거 — iolog의 add 이벤트가 새로 등록. */
	td->io_log_rfile = f;                               /* [한국어] 후속 read_iolog() 호출이 이 FILE*를 계속 사용. */
	return read_iolog(td);                              /* [한국어] 첫 파싱 실행(비-청크 모드면 파일 전체). */
}

/*
 * Set up a log for storing io patterns.
 */
/*
 * [한국어]
 * init_iolog_write - write iolog 파일을 열고 버전 헤더 및 기존 파일 add 이벤트를 기록.
 *
 * @td: 잡 컨텍스트. td->o.write_iolog_file 파일 경로, td->iolog_f/iolog_buf 설정,
 *       td->io_log_start_time 기준 시각 기록.
 *
 * @return: true — 준비 완료, false — fopen/fprintf 실패.
 *
 * 단계:
 *   1) fopen(path, "a") — append 모드 (기존 iolog에 이어쓰기 지원).
 *   2) 8KiB malloc + setvbuf(_IOFBF) — fprintf 오버헤드 감소 (fully buffered).
 *   3) io_log_start_time 기록 — utime_since_now가 이 기준으로 상대 시각 계산.
 *   4) "fio version 3 iolog\n" 헤더 기록.
 *   5) 현재 td에 등록된 모든 파일을 "add" 이벤트로 기록 (재생 시 구조 복원).
 *
 * 실행 컨텍스트: 잡 스레드 init 경로. init_iolog()에서 호출.
 */
static bool init_iolog_write(struct thread_data *td)
{
	struct fio_file *ff;
	FILE *f;
	unsigned int i;

	f = fopen(td->o.write_iolog_file, "a");             /* [한국어] append 모드 — 기존 파일에 이어쓰기(재실행 시 누적). */
	if (!f) {
		perror("fopen write iolog");
		return false;
	}

	/*
	 * That's it for writing, setup a log buffer and we're done.
	  */
	/* [한국어] 8KiB 버퍼드 I/O — fprintf 빈번 호출 시 syscall 수 감소. _IOFBF = fully buffered. */
	td->iolog_f = f;
	td->iolog_buf = malloc(8192);
	setvbuf(f, td->iolog_buf, _IOFBF, 8192);
	fio_gettime(&td->io_log_start_time, NULL);          /* [한국어] 모든 후속 log_io_u의 타임스탬프 기준점. */

	/*
	 * write our version line
	 */
	/* [한국어] v3 매직 헤더 기록 — 재생 시 init_iolog_read가 이를 읽어 버전 판별. */
	if (fprintf(f, "%s\n", iolog_ver3) < 0) {
		perror("iolog init\n");
		return false;
	}

	/*
	 * add all known files
	 */
	/* [한국어] 잡 시작 시점에 이미 등록된 모든 파일을 add 이벤트로 기록.
	 *   재생 시 이 add들을 먼저 처리하여 fileno 매핑을 재구성. */
	for_each_file(td, ff, i)
		log_file(td, ff, FIO_LOG_ADD_FILE);

	return true;
}

/*
 * [한국어]
 * init_iolog - iolog 서브시스템 진입점. 읽기/쓰기/blktrace 모드를 분기.
 *
 * @td: 잡 컨텍스트. td->o.read_iolog_file, td->o.write_iolog_file 옵션으로 모드 결정.
 *
 * @return: true — 성공 또는 iolog 미사용, false — 초기화 실패.
 *
 * 분기:
 *   - read_iolog_file 설정: is_blktrace()로 바이너리 감지 → blktrace.c 경로 또는
 *     init_iolog_read() 텍스트 경로.
 *   - write_iolog_file 설정: init_iolog_write() 경로.
 *   - 둘 다 없음: 즉시 true(iolog 미사용).
 *
 * 실행 컨텍스트: 잡 스레드 초기화(thread_main의 setup 단계).
 *
 * 후속: init_disk_util(td) — iostat 등 디스크 사용률 통계 준비(일반 모드와 공유).
 *
 * 호출 체인: thread_main() → init_iolog() → {init_blktrace_read|init_iolog_read|
 *           init_iolog_write} → init_disk_util.
 */
bool init_iolog(struct thread_data *td)
{
	bool ret;

	if (td->o.read_iolog_file) {
		int need_swap;
		char * fname = get_name_by_idx(td->o.read_iolog_file, td->subjob_number);  /* [한국어] numjobs>1일 때 subjob별 파일명 치환(%j 토큰 확장). */

		/*
		 * Check if it's a blktrace file and load that if possible.
		 * Otherwise assume it's a normal log file and load that.
		 */
		/* [한국어] blktrace 바이너리 매직 시그니처(blktrace.c 내 is_blktrace) 확인.
		 *   need_swap: 엔디언 교환 필요 여부(다른 아키텍처 머신 기록 파일). */
		if (is_blktrace(fname, &need_swap)) {
			td->io_log_blktrace = 1;            /* [한국어] read_iolog_get이 read_blktrace() 분기 선택. */
			ret = init_blktrace_read(td, fname, need_swap);
		} else {
			td->io_log_blktrace = 0;            /* [한국어] 기본 텍스트 iolog 경로. */
			ret = init_iolog_read(td, fname);
		}
		free(fname);
	} else if (td->o.write_iolog_file)
		ret = init_iolog_write(td);
	else
		ret = true;                                 /* [한국어] iolog 미사용 — no-op 성공. */

	if (!ret)
		td_verror(td, EINVAL, "failed initializing iolog");

	init_disk_util(td);                                 /* [한국어] iostat 등 디스크 사용률 통계 초기화 (iolog 여부와 독립). */

	return ret;
}

/*
 * [한국어]
 * setup_log - 성능 로그(io_log) 구조체를 할당/초기화한다. bw/lat/clat/slat/iops/clat_hist
 *             모든 로그 타입이 이 함수를 경유한다.
 *
 * @log:      (out) 호출자에게 반환할 io_log 포인터의 주소.
 * @p:        입력 파라미터 꾸러미 — log_type/log_offset/log_prio/log_issue_time/log_gz/
 *             log_gz_store/avg_msec/hist_msec/hist_coarseness/td(집계 로그는 NULL).
 * @filename: 최종 덤프 파일명. strdup로 복제되어 log->filename 저장.
 *
 * 초기화 단계:
 *   1) scalloc으로 SHM 힙에 할당 — fork된 자식 잡과 공유 가능.
 *   2) io_logs 리스트 head 초기화.
 *   3) 파라미터 복사.
 *   4) ddir별 histogram window의 리스트 head + zero 초기 엔트리 삽입
 *      (delta 계산 시 "이전 스냅샷 없음" 처리 회피).
 *   5) offload 모드가 아니면 pending 버퍼 사전 할당(iodepth 기반 roundup_pow2).
 *   6) log_ddir_mask 비트 설정 — 샘플 포맷에 offset/prio/avg_max/issue_time 포함 여부.
 *   7) 압축 설정: gz/gz_store 활성 시 chunk_lock/deferred_free_lock 뮤텍스 초기화 +
 *      td->flags |= TD_F_COMPRESS_LOG. 집계 로그(td NULL)에서는 gz 불가 → 0으로 비활성화.
 *
 * 실행 컨텍스트: 잡 스레드 init 경로에서 bw/lat/iops/clat_hist 로그별로 1회씩 호출.
 *   집계 로그는 backend.c의 init 단계에서 td=NULL로 호출.
 *
 * 호출 체인: init_iolog() → stat.c의 init_logs → setup_log() (각 로그 타입마다).
 */
void setup_log(struct io_log **log, struct log_params *p,
	       const char *filename)
{
	struct io_log *l;
	int i;
	struct io_u_plat_entry *entry;
	struct flist_head *list;

	/* [한국어] SHM 힙 할당(scalloc) — fork된 자식 프로세스가 접근해야 하는 집계 로그 지원.
	 *   assert로 할당 실패는 치명으로 간주. */
	l = scalloc(1, sizeof(*l));
	assert(l);
	INIT_FLIST_HEAD(&l->io_logs);                       /* [한국어] 청크 리스트 head 초기화 — __add_log_sample에서 append. */
	l->log_type = p->log_type;                          /* [한국어] IO_LOG_TYPE_BW/LAT/CLAT/SLAT/IOPS/HIST 중 하나. */
	l->log_offset = p->log_offset;                      /* [한국어] offset 필드 포함 여부. */
	l->log_prio = p->log_prio;                          /* [한국어] priority 필드 포함 여부(16비트 hex vs RT 클래스 0/1). */
	l->log_issue_time = p->log_issue_time;              /* [한국어] issue_time 필드 포함 여부. */
	l->log_gz = p->log_gz;                              /* [한국어] 0 아니면 zlib 압축 활성화(청크 크기). */
	l->log_gz_store = p->log_gz_store;                  /* [한국어] true면 압축 바이트를 그대로 파일에 저장(.log.gz). */
	l->avg_msec = p->avg_msec;                          /* [한국어] 평균 윈도우(ms) — 0 아니면 윈도우 내 샘플 평균 1건만 기록. */
	l->hist_msec = p->hist_msec;                        /* [한국어] 히스토그램 덤프 간격(ms). */
	l->hist_coarseness = p->hist_coarseness;            /* [한국어] 버킷 묶음 2^k (해상도/출력 크기 트레이드오프). */
	l->filename = strdup(filename);                     /* [한국어] 파일명 복제(caller 소유권 무관). */
	l->td = p->td;                                      /* [한국어] 소유 잡 — 집계 로그는 NULL. */

	/* Initialize histogram lists for each r/w direction,
	 * with initial io_u_plat of all zeros:
	 */
	/* [한국어] R/W/TRIM 각 방향의 히스토그램 리스트 초기화. 0으로 채운 엔트리를 미리 넣어두면
	 *   delta 계산(hist_sum with plat_last) 시 "이전 없음" 분기가 단순해진다. */
	for (i = 0; i < DDIR_RWDIR_CNT; i++) {
		list = &l->hist_window[i].list;
		INIT_FLIST_HEAD(list);
		entry = calloc(1, sizeof(struct io_u_plat_entry));  /* [한국어] io_u_plat 배열이 0으로 초기화됨 (calloc). */
		flist_add(&entry->list, list);
	}

	/* [한국어] offload(별도 스레드가 I/O 제출) 모드가 아니면 pending 버퍼를 미리 할당.
	 *   offload 모드는 workqueue 스레드가 별도 관리하므로 여기서 할당 불필요. */
	if (l->td && l->td->o.io_submit_mode != IO_MODE_OFFLOAD) {
		unsigned int def_samples = DEF_LOG_ENTRIES;
		struct io_logs *__p;

		__p = calloc(1, sizeof(*l->pending));
		if (l->td->o.iodepth > DEF_LOG_ENTRIES)
			def_samples = roundup_pow2(l->td->o.iodepth);  /* [한국어] iodepth가 크면 그에 맞춰 2의 거듭제곱으로 올림 — 인덱싱/해싱 효율. */
		__p->max_samples = def_samples;
		__p->log = calloc(__p->max_samples, log_entry_sz(l));   /* [한국어] 샘플 크기는 플래그에 따라 다름(log_entry_sz 계산). */
		l->pending = __p;
	}

	/* [한국어] log_ddir_mask 비트 설정 — ddir 필드 상위 비트에 로그 포맷 플래그를 내장. */
	if (l->log_offset)
		l->log_ddir_mask = LOG_OFFSET_SAMPLE_BIT;
	if (l->log_prio)
		l->log_ddir_mask |= LOG_PRIO_SAMPLE_BIT;
	/*
	 * The bandwidth-log option generates agg-read_bw.log,
	 * agg-write_bw.log and agg-trim_bw.log for which l->td is NULL.
	 * Check if l->td is valid before dereferencing it.
	 */
	/* [한국어] 집계 로그는 td NULL이라 log_max 옵션 접근 불가 → 방어적 NULL 체크. */
	if (l->td && l->td->o.log_max == IO_LOG_SAMPLE_BOTH)
		l->log_ddir_mask |= LOG_AVG_MAX_SAMPLE_BIT;

	if (l->log_issue_time)
		l->log_ddir_mask |= LOG_ISSUE_TIME_SAMPLE_BIT;

	INIT_FLIST_HEAD(&l->chunk_list);                    /* [한국어] 압축 청크 리스트 head 초기화. */

	/* [한국어] gz 압축 설정: 집계 로그에서는 지원 안 함(td==NULL) → 0으로 강제. 아니면 뮤텍스 준비. */
	if (l->log_gz && !p->td)
		l->log_gz = 0;
	else if (l->log_gz || l->log_gz_store) {
		mutex_init_pshared(&l->chunk_lock);         /* [한국어] PTHREAD_PROCESS_SHARED 뮤텍스 — 잡 스레드 + 압축 헬퍼 스레드 간 chunk_list 동기화. */
		mutex_init_pshared(&l->deferred_free_lock); /* [한국어] 지연 해제 큐 전용 뮤텍스. */
		p->td->flags |= TD_F_COMPRESS_LOG;          /* [한국어] 이 잡은 압축 워크큐 필요 → iolog_compress_init이 생성. */
	}

	*log = l;                                            /* [한국어] 호출자에게 반환. */
}

#ifdef CONFIG_SETVBUF
/*
 * [한국어]
 * set_file_buffer - 로그 덤프용 FILE*에 1 MiB 사용자 버퍼를 설정(fully buffered).
 *
 * @f: fopen된 FILE*. setvbuf로 버퍼 바인딩.
 * @return: 할당된 버퍼 포인터 — 호출자가 나중에 clear_file_buffer로 해제.
 *
 * 왜 1 MiB인가: 대용량 로그를 fprintf로 라인 단위 기록할 때 syscall 빈도를 줄여
 * 덤프 시간을 단축. 기본 8KiB(BUFSIZ) 대비 체감 10배 이상 빠른 경우가 있음.
 *
 * CONFIG_SETVBUF: 일부 플랫폼에서 setvbuf가 불안정하거나 부작용이 있으므로 빌드 시 분기.
 */
static void *set_file_buffer(FILE *f)
{
	size_t size = 1048576;                              /* [한국어] 1 MiB 고정 — 로그 파일은 크기 100MB~GB 급 가능. */
	void *buf;

	buf = malloc(size);
	setvbuf(f, buf, _IOFBF, size);                      /* [한국어] _IOFBF = fully buffered (블록 단위 flush). */
	return buf;
}

/*
 * [한국어]
 * clear_file_buffer - set_file_buffer가 반환한 버퍼를 해제.
 *
 * @buf: 해제 대상 포인터. NULL 허용 (free(NULL) no-op).
 *
 * 주의: 반드시 fclose(f) 후에 호출해야 setvbuf 버퍼 접근 이후 UAF 방지.
 */
static void clear_file_buffer(void *buf)
{
	free(buf);
}
#else
/*
 * [한국어] CONFIG_SETVBUF 미지원 플랫폼용 no-op 구현. FILE*는 libc 기본 버퍼만 사용.
 */
static void *set_file_buffer(FILE *f)
{
	return NULL;
}

static void clear_file_buffer(void *buf)
{
}
#endif

/*
 * [한국어]
 * free_log - io_log 구조체와 그 안의 모든 로그 청크/pending 버퍼/파일명 해제.
 *
 * @log: 해제할 io_log 포인터(SHM 힙 scalloc). NULL은 호출하면 안 됨.
 *
 * 해제 순서:
 *   1) io_logs 리스트의 io_logs 청크들 — flist_del_init + free(log 배열) + sfree(cur_log).
 *   2) pending 버퍼 — free(pending->log) + free(pending).
 *   3) (중복) free(log->pending) — pending을 두 번 free (단, 이미 NULL이므로 no-op).
 *   4) filename strdup 해제.
 *   5) log 자체 sfree(SHM 힙).
 *
 * 호출 시점: finish_log() 끝, 또는 에러 경로.
 */
void free_log(struct io_log *log)
{
	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);             /* [한국어] 리스트에서 분리 + head 자기 참조로 초기화(이중 del 안전). */
		free(cur_log->log);                         /* [한국어] 샘플 배열 해제(calloc된 plain 힙). */
		sfree(cur_log);                             /* [한국어] io_logs 자체는 SHM 힙 — sfree. */
	}

	if (log->pending) {
		free(log->pending->log);
		free(log->pending);
		log->pending = NULL;
	}

	free(log->pending);                                 /* [한국어] 이미 위에서 NULL로 리셋했으므로 free(NULL) no-op — 방어적 중복. */
	free(log->filename);                                /* [한국어] strdup으로 할당된 경로 해제. */
	sfree(log);                                         /* [한국어] io_log 자체 SHM 해제. */
}

/*
 * [한국어]
 * hist_sum - 히스토그램의 stride개 버킷을 합산(해상도 다운샘플링).
 *
 * @j:               시작 버킷 인덱스.
 * @stride:          묶을 버킷 개수 (= 2^coarseness).
 * @io_u_plat:       현재 스냅샷 버킷 배열.
 * @io_u_plat_last:  이전 스냅샷 (NULL이면 절대값, 아니면 delta 계산).
 *
 * @return: 합산된 버킷 값 (absolute 또는 delta).
 *
 * 왜 이 구조: fio_plot 같은 후처리 도구가 너무 세밀한 버킷 수(FIO_IO_U_PLAT_NR=~1024)를
 * 받으면 렌더링 부담이 크므로, coarseness=k일 때 2^k 버킷씩 묶어 해상도를 낮춘다.
 *
 * delta 모드는 시간에 따른 증가분만 기록하여 플롯에서 "이 구간에서 새로 발생한 I/O"를 보여준다.
 */
uint64_t hist_sum(int j, int stride, uint64_t *io_u_plat,
		uint64_t *io_u_plat_last)
{
	uint64_t sum;
	int k;

	if (io_u_plat_last) {
		for (k = sum = 0; k < stride; k++)
			sum += io_u_plat[j + k] - io_u_plat_last[j + k];  /* [한국어] delta: 현재 - 이전. */
	} else {
		for (k = sum = 0; k < stride; k++)
			sum += io_u_plat[j + k];             /* [한국어] absolute: 누적값 그대로. */
	}

	return sum;
}

/*
 * [한국어]
 * flush_hist_samples - clat_hist_log 샘플을 CSV로 파일에 덤프한다.
 *
 * @f:                출력 FILE*.
 * @hist_coarseness: 버킷 묶음 2^k (해상도 다운샘플링).
 * @samples:          io_sample 배열 시작 주소.
 * @sample_size:      배열 총 바이트 크기.
 *
 * 출력 포맷 (한 샘플당 한 줄):
 *   "<time>, <ddir>, <bs>, <bucket_0_delta>, <bucket_1_delta>, ..., <bucket_N_delta>\n"
 *
 * 알고리즘:
 *   1) 첫 샘플로 log_offset/log_issue_time 플래그 비트 복원(가변 엔트리 크기).
 *   2) nr_samples = sample_size / per-entry 크기.
 *   3) 각 샘플마다 현재 plat_entry과 flist의 첫 번째 이전 스냅샷을 delta 연산.
 *   4) coarseness로 버킷 합산(hist_sum) 후 CSV 출력.
 *   5) 출력한 이전 스냅샷 엔트리는 flist에서 제거 + free — 메모리 순환.
 *
 * 실행 컨텍스트: 메인 프로세스의 td_writeout_logs → flush_log 호출 중.
 *
 * 왜 delta인가: fio_plot은 시간에 따른 latency 분포 heatmap을 그리므로 각 스냅샷 사이의
 * 증가분(새로 발생한 I/O)만 있으면 된다. 누적값은 마지막 엔트리에서 파생 가능.
 */
static void flush_hist_samples(FILE *f, int hist_coarseness, void *samples,
			       uint64_t sample_size)
{
	struct io_sample *s;
	bool log_offset, log_issue_time;
	uint64_t i, j, nr_samples;
	struct io_u_plat_entry *entry, *entry_before;
	uint64_t *io_u_plat;
	uint64_t *io_u_plat_before;

	int stride = 1 << hist_coarseness;                  /* [한국어] 버킷 묶음 크기 — 2^coarseness (예: k=3 → 8개씩 묶음, 출력 크기 1/8). */

	if (!sample_size)
		return;

	/* [한국어] 첫 샘플의 __ddir에서 로그 플래그 비트 복원 — 모든 샘플이 동일한 포맷. */
	s = __get_sample(samples, 0, 0, 0);
	log_offset = (s->__ddir & LOG_OFFSET_SAMPLE_BIT) != 0;
	log_issue_time = (s->__ddir & LOG_ISSUE_TIME_SAMPLE_BIT) != 0;

	nr_samples = sample_size / __log_entry_sz(log_offset, log_issue_time);  /* [한국어] 엔트리 크기는 플래그에 따라 가변 → 배열 크기로 나눠 개수 계산. */

	for (i = 0; i < nr_samples; i++) {
		s = __get_sample(samples, log_offset, log_issue_time, i);

		entry = s->data.plat_entry;                 /* [한국어] 이 샘플의 버킷 배열 (현재 스냅샷). */
		io_u_plat = entry->io_u_plat;

		/* [한국어] entry->list에 연결된 이전 스냅샷(FIFO 헤드) — 직전 flush 시 추가됨. */
		entry_before = flist_first_entry(&entry->list, struct io_u_plat_entry, list);
		io_u_plat_before = entry_before->io_u_plat;

		/* [한국어] 첫 3필드: time(us), ddir, bs(바이트). */
		fprintf(f, "%lu, %u, %llu, ", (unsigned long) s->time,
						io_sample_ddir(s), (unsigned long long) s->bs);
		/* [한국어] 모든 버킷을 stride 단위로 묶어 delta 합산하여 출력. 마지막은 별도 처리(개행). */
		for (j = 0; j < FIO_IO_U_PLAT_NR - stride; j += stride) {
			fprintf(f, "%llu, ", (unsigned long long)
			        hist_sum(j, stride, io_u_plat, io_u_plat_before));
		}
		fprintf(f, "%llu\n", (unsigned long long)
		        hist_sum(FIO_IO_U_PLAT_NR - stride, stride, io_u_plat,
					io_u_plat_before));

		flist_del(&entry_before->list);             /* [한국어] 이전 스냅샷 엔트리 리스트에서 제거. */
		free(entry_before);                         /* [한국어] 이전 스냅샷 해제 — 메모리 순환. */
	}
}

/*
 * [한국어]
 * print_sample_fields - va_list 기반 포맷팅을 임시 버퍼에 누적하는 헬퍼.
 *
 * @p:    (in/out) 현재 쓰기 포인터. 성공 시 ret 바이트만큼 전진.
 * @left: (in/out) 남은 버퍼 공간. 성공 시 ret 바이트만큼 감소.
 * @fmt:  printf 포맷 문자열.
 * @...:  포맷 인자들.
 *
 * @return: 0 = 성공, -1 = 실패(버퍼 초과 또는 vsnprintf 에러).
 *
 * 왜 필요한가: flush_samples가 여러 조건부 필드를 누적해서 하나의 라인을 구성하는데,
 * fprintf를 직접 쓰면 syscall이 여러 번 일어나고 라인 단위 원자성도 깨진다.
 * 하나의 버퍼에 조립해 마지막에 fprintf("%s\n", buf) 한 번만 호출.
 */
static int print_sample_fields(char **p, size_t *left, const char *fmt, ...) {
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsnprintf(*p, *left, fmt, ap);
	if (ret < 0 || ret >= *left) {                      /* [한국어] 음수(포맷 에러) 또는 buffer overflow (truncation). */
		log_err("sample file write failed: %d\n", ret);
		va_end(ap);
		return -1;
	}
	va_end(ap);

	*p += ret;                                          /* [한국어] 포인터 전진. */
	*left -= ret;                                       /* [한국어] 남은 공간 감소. */

	return 0;
}

/*
 * flush_samples - Generate output for log samples
 * Each sample output is built using a temporary buffer. This buffer size
 * assumptions are:
 * - Each sample has less than 10 fields
 * - Each sample field fits in 25 characters (20 digits for 64 bit number
 *   and a few bytes delimiter)
 */
/*
 * [한국어]
 * flush_samples - 일반(비-히스토그램) io_sample 배열을 CSV로 파일에 덤프.
 *
 * @f:            출력 FILE*.
 * @samples:      io_sample 배열 시작 주소.
 * @sample_size:  배열 총 바이트.
 *
 * 출력 필드(조건부):
 *   time, val0[, val1_if_BOTH], ddir, bs[, offset_if_log_offset]
 *        [, 0x%04x_prio_if_log_prio ELSE rt_class_0or1][, issue_time_if_log_issue_time]
 *
 * 버퍼 크기: 256바이트 스택 버퍼로 한 샘플 조립 (주석 규격: 10필드 x 25자 여유).
 *
 * 실행 컨텍스트: 메인 프로세스 또는 잡 스레드의 flush_log 경로.
 *
 * 호출 체인: flush_log() → flush_samples() / flush_hist_samples().
 */
void flush_samples(FILE *f, void *samples, uint64_t sample_size)
{
	struct io_sample *s;
	bool log_offset, log_prio, log_avg_max, log_issue_time;
	uint64_t i, nr_samples;
	char buf[256];                                      /* [한국어] 한 샘플 라인 조립용 스택 버퍼. */
	char *p;
	size_t left;
	int ret;

	if (!sample_size)
		return;

	/* [한국어] 첫 샘플의 __ddir 상위 비트에 내장된 로그 포맷 플래그 복원. */
	s = __get_sample(samples, 0, 0, 0);
	log_offset = (s->__ddir & LOG_OFFSET_SAMPLE_BIT) != 0;
	log_prio = (s->__ddir & LOG_PRIO_SAMPLE_BIT) != 0;
	log_avg_max = (s->__ddir & LOG_AVG_MAX_SAMPLE_BIT) != 0;
	log_issue_time = (s->__ddir & LOG_ISSUE_TIME_SAMPLE_BIT) != 0;

	nr_samples = sample_size / __log_entry_sz(log_offset, log_issue_time);

	for (i = 0; i < nr_samples; i++) {
		s = __get_sample(samples, log_offset, log_issue_time, i);
		p = buf;
		left = sizeof(buf);

		/* [한국어] 필드 1-2: time(us) + val0(lat/BW/IOPS 값). PRIu64/PRId64로 64비트 안전 포맷. */
		ret = print_sample_fields(&p, &left, "%" PRIu64 ", %" PRId64,
					  s->time, s->data.val.val0);
		if (ret)
			return;

		/* [한국어] BOTH 모드: avg 외에 max도 포함(val1). log_max=both 시 활성화. */
		if (log_avg_max) {
			ret = print_sample_fields(&p, &left, ", %" PRId64,
						  s->data.val.val1);
			if (ret)
				return;
		}

		/* [한국어] 필드: ddir(0=READ, 1=WRITE, 2=TRIM, ...) + bs(블록 바이트). */
		ret = print_sample_fields(&p, &left, ", %u, %llu",
					  io_sample_ddir(s),
					  (unsigned long long) s->bs);
		if (ret)
			return;

		/* [한국어] 옵션 필드: offset(log_offset=1 시 aux 배열의 IOS_AUX_OFFSET_INDEX에서 추출). */
		if (log_offset) {
			ret = print_sample_fields(&p, &left, ", %llu",
						  (unsigned long long) s->aux[IOS_AUX_OFFSET_INDEX]);
			if (ret)
				return;
		}

		/* [한국어] 우선순위 필드: log_prio=1이면 16비트 raw "0x%04x" 포맷, 아니면 RT 클래스인지(0/1). */
		if (log_prio)
			ret = print_sample_fields(&p, &left, ", 0x%04x",
						  s->priority);
		else
			ret = print_sample_fields(&p, &left, ", %u",
						  ioprio_value_is_class_rt(s->priority));
		if (ret)
			return;

		/* [한국어] 옵션 필드: issue_time(log_issue_time=1 시 aux의 IOS_AUX_ISSUE_TIME_INDEX). */
		if (log_issue_time) {
			ret = print_sample_fields(&p, &left, ", %llu",
						  (unsigned long long) s->aux[IOS_AUX_ISSUE_TIME_INDEX]);
			if (ret)
				return;
		}

		fprintf(f, "%s\n", buf);                    /* [한국어] 한 샘플 라인 단 한 번의 fprintf로 원자 출력. */
	}
}

#ifdef CONFIG_ZLIB

/*
 * [한국어]
 * struct iolog_flush_data - gz_work() 압축 작업 단위.
 *
 * 워크큐에 enqueue되어 압축 헬퍼 스레드가 수행할 작업 기술.
 */
struct iolog_flush_data {
	struct workqueue_work work;
	/* [한국어] workqueue의 범용 항목 헤더(container_of 역산용).
	 *   설정자: iolog_cur_flush가 smalloc 후 workqueue_enqueue.
	 *   읽는 자: gz_work_async가 container_of(work, struct iolog_flush_data, work)로 복원.
	 *   동기화: workqueue 내부 큐 뮤텍스가 보장. */

	struct io_log *log;
	/* [한국어] 압축 대상 io_log 포인터.
	 *   설정자: iolog_flush/cur_flush 호출 시.
	 *   읽는 자: gz_work에서 log->chunk_list에 결과 추가, log->chunk_lock으로 동기화.
	 *   값 범위: 유효 io_log* (NULL 불가).
	 *   동기화: log 소유 잡이 종료 전이면 항상 유효. */

	void *samples;
	/* [한국어] 압축할 io_sample 배열 시작 주소.
	 *   설정자: 호출자가 io_logs 청크의 log 포인터를 여기에 넣고 원본 청크는 "비어있음"으로 리셋.
	 *   읽는 자: gz_work가 zlib next_in으로 사용, 완료 후 iolog_put_deferred로 지연 해제.
	 *   값 범위: 유효 배열 (nr_samples 만큼 엔트리).
	 *   동기화: 소유권이 gz_work로 이전되었으므로 원본 청크는 더 이상 이 포인터 참조 금지. */

	uint32_t nr_samples;
	/* [한국어] 압축 대상 샘플 개수.
	 *   avail_in = nr_samples * log_entry_sz(log)로 변환되어 deflate 입력 크기로 사용.
	 *   값 범위: 0..max_samples.
	 *   동기화: 제출 시점에 확정, 이후 불변. */

	bool free;
	/* [한국어] 작업 완료 후 이 iolog_flush_data 자체를 sfree할지 여부.
	 *   true: 비동기 호출(workqueue) → 워커가 완료 후 스스로 sfree.
	 *   false: 동기 호출(iolog_flush 내부) → 호출자(iolog_flush)가 free.
	 *   왜 필요: 스택 할당과 힙 할당을 동일 코드로 처리하기 위해. */
};

#define GZ_CHUNK	131072  /* [한국어] 압축 청크 크기 128KiB — zlib deflate의 권장 입력 청크. 파일 I/O 효율과 메모리 사용의 절충점. */

/*
 * [한국어]
 * get_new_chunk - 새 iolog_compress 청크를 할당하고 빈 버퍼로 초기화.
 *
 * @seq: 이 청크가 속하는 시퀀스 번호(같은 압축 스트림에서 여러 청크가 생성될 수 있음).
 * @return: 새로 할당된 iolog_compress (NULL 반환 불가 — malloc 실패 미처리 관행).
 *
 * 호출: gz_work()가 deflate 출력 공간이 부족할 때 새 청크 추가 할당.
 */
static struct iolog_compress *get_new_chunk(unsigned int seq)
{
	struct iolog_compress *c;

	c = malloc(sizeof(*c));
	INIT_FLIST_HEAD(&c->list);                          /* [한국어] flist 노드 self-link 초기화. */
	c->buf = malloc(GZ_CHUNK);                          /* [한국어] 128KiB 압축 결과 버퍼. */
	c->len = 0;                                         /* [한국어] 유효 데이터 크기(바이트). */
	c->seq = seq;                                       /* [한국어] 시퀀스 번호 — inflate 시 구분자. */
	return c;
}

/*
 * [한국어]
 * free_chunk - iolog_compress 청크 전체(버퍼+구조체) 해제.
 *
 * 호출: gz_work 에러 경로, inflate_gz_chunks가 소비 후.
 */
static void free_chunk(struct iolog_compress *ic)
{
	free(ic->buf);
	free(ic);
}

/*
 * [한국어]
 * z_stream_init - zlib inflate 스트림을 초기화.
 *
 * @stream: 호출자가 소유한 z_stream 구조체 — memset으로 0 초기화 후 설정.
 * @gz_hdr: 0 = raw deflate, 1 = gzip/raw 자동 감지 (wbits+=32).
 * @return: 0 = 성공, 1 = inflateInit2 실패.
 *
 * wbits: windowBits 값. 15가 zlib 기본. +32를 더하면 "자동 헤더 감지"
 *         (gzip 파일인지 raw deflate인지 첫 바이트로 판별).
 *
 * 호출: inflate_chunk가 새 시퀀스 시작 시.
 */
static int z_stream_init(z_stream *stream, int gz_hdr)
{
	int wbits = 15;                                     /* [한국어] zlib 기본 windowBits — 32KiB 슬라이딩 윈도우. */

	memset(stream, 0, sizeof(*stream));
	stream->zalloc = Z_NULL;                            /* [한국어] 기본 할당자 사용(Z_NULL → 내부 malloc). */
	stream->zfree = Z_NULL;
	stream->opaque = Z_NULL;
	stream->next_in = Z_NULL;

	/*
	 * zlib magic - add 32 for auto-detection of gz header or not,
	 * if we decide to store files in a gzip friendly format.
	 */
	/* [한국어] +32: gzip 헤더(1f 8b) 자동 감지 + raw deflate 겸용 모드. log_gz_store 모드에서 필요. */
	if (gz_hdr)
		wbits += 32;

	if (inflateInit2(stream, wbits) != Z_OK)
		return 1;

	return 0;
}

/*
 * [한국어]
 * struct inflate_chunk_iter - inflate 반복 상태.
 *
 * 출력 버퍼를 동적으로 확장하면서 여러 청크를 잇는 상태를 유지하기 위한 iterator.
 */
struct inflate_chunk_iter {
	unsigned int seq;
	/* [한국어] 현재 처리 중인 시퀀스 번호. 새 시퀀스 발견 시 finish_chunk 호출 후 갱신.
	 *   설정자: inflate_chunk에서 첫 번째 시퀀스 감지 시.
	 *   읽는 자: inflate_gz_chunks 종료 시 유효성 확인. */

	int err;
	/* [한국어] 누적 에러 코드 — inflate 실패 시 log_err 후 저장.
	 *   설정자: inflate/fwrite 실패 시.
	 *   읽는 자: inflate_gz_chunks 반환값. */

	void *buf;
	/* [한국어] 해제된 데이터 누적 버퍼 (realloc으로 확장).
	 *   설정자: inflate_chunk malloc/realloc.
	 *   읽는 자: finish_chunk가 flush_samples로 텍스트 변환. */

	size_t buf_size;
	/* [한국어] buf 전체 할당 크기. */

	size_t buf_used;
	/* [한국어] 현재 유효 데이터 길이(buf_size 이하). */

	size_t chunk_sz;
	/* [한국어] 초기/확장 단위 크기 — log_gz 또는 고정값(iolog_file_inflate는 64 MiB). */
};

/*
 * [한국어]
 * finish_chunk - 현재 inflate 시퀀스를 마무리하고 해제된 데이터를 flush_samples로 출력.
 *
 * @stream: inflateEnd로 자원 해제할 z_stream.
 * @f:      텍스트 출력 FILE*.
 * @iter:   현재 시퀀스 상태(buf, buf_used 사용 후 리셋).
 *
 * 호출: 새 시퀀스 시작 전(inflate_chunk 시퀀스 전환) 또는 모든 청크 소비 후(inflate_gz_chunks 끝).
 */
static void finish_chunk(z_stream *stream, FILE *f,
			 struct inflate_chunk_iter *iter)
{
	int ret;

	ret = inflateEnd(stream);                           /* [한국어] zlib 내부 리소스(windowBuf 등) 해제. */
	if (ret != Z_OK)
		log_err("fio: failed to end log inflation seq %d (%d)\n",
				iter->seq, ret);

	flush_samples(f, iter->buf, iter->buf_used);        /* [한국어] 해제된 io_sample 배열을 CSV로 출력. */
	free(iter->buf);                                    /* [한국어] 누적 버퍼 해제. */
	iter->buf = NULL;
	iter->buf_size = iter->buf_used = 0;                /* [한국어] 다음 시퀀스를 위해 리셋. */
}

/*
 * Iterative chunk inflation. Handles cases where we cross into a new
 * sequence, doing flush finish of previous chunk if needed.
 */
/*
 * [한국어]
 * inflate_chunk - 하나의 iolog_compress 청크를 zlib inflate 처리하여 iter->buf에 누적.
 *                 시퀀스 전환 경계에서 이전 시퀀스를 flush하고 새 스트림을 초기화한다.
 *
 * @ic:     입력 압축 청크.
 * @gz_hdr: gzip 헤더 자동 감지 활성화 플래그(log_gz_store에서 전달).
 * @f:      출력 FILE* (finish_chunk가 flush_samples로 쓴다).
 * @stream: zlib 스트림 상태 (호출자 스택).
 * @iter:   iterator 상태 (시퀀스/버퍼).
 *
 * @return: 이번 호출에 소비한 입력 바이트 수 (< ic->len 가능).
 *
 * 알고리즘:
 *   1) ic->seq != iter->seq → 시퀀스 전환. 이전 시퀀스 finish_chunk, 새 스트림 init.
 *   2) 출력 버퍼 없으면 chunk_sz 크기로 malloc.
 *   3) avail_in 소진까지 inflate 반복 — 출력 공간 부족 시 realloc으로 확장.
 *   4) Z_STREAM_END이면 break.
 *   5) 소비 바이트(next_in 전진량) 반환.
 *
 * 왜 반복인가: zlib inflate는 한 호출에 한 스트림이 끝날 때(Z_STREAM_END)까지 반환하지
 * 않을 수 있으므로, 출력 공간을 계속 제공하면서 반복 호출 필요.
 */
static size_t inflate_chunk(struct iolog_compress *ic, int gz_hdr, FILE *f,
			    z_stream *stream, struct inflate_chunk_iter *iter)
{
	size_t ret;

	dprint(FD_COMPRESS, "inflate chunk size=%lu, seq=%u\n",
				(unsigned long) ic->len, ic->seq);

	/* [한국어] 청크의 시퀀스가 iter의 현재 시퀀스와 다르면 새 스트림 시작.
	 *   이전 시퀀스가 있었다면 finish_chunk로 flush. */
	if (ic->seq != iter->seq) {
		if (iter->seq)
			finish_chunk(stream, f, iter);

		z_stream_init(stream, gz_hdr);
		iter->seq = ic->seq;
	}

	stream->avail_in = ic->len;
	stream->next_in = ic->buf;

	if (!iter->buf_size) {
		iter->buf_size = iter->chunk_sz;
		iter->buf = malloc(iter->buf_size);
	}

	/* [한국어] 입력 바이트가 남아있는 동안 inflate. 출력 버퍼 부족 시 realloc. */
	while (stream->avail_in) {
		size_t this_out = iter->buf_size - iter->buf_used;
		int err;

		stream->avail_out = this_out;
		stream->next_out = iter->buf + iter->buf_used;

		err = inflate(stream, Z_NO_FLUSH);          /* [한국어] Z_NO_FLUSH: 가능한 만큼 inflate하되 스트림 끝까지 강요하지 않음. */
		if (err < 0) {
			log_err("fio: failed inflating log: %d\n", err);
			iter->err = err;
			break;
		}

		iter->buf_used += this_out - stream->avail_out;  /* [한국어] 실제 쓴 바이트 = 이전 avail_out - 현재 남은 avail_out. */

		/* [한국어] 출력 버퍼 가득 참 → chunk_sz 만큼 확장 후 다시 시도. */
		if (!stream->avail_out) {
			iter->buf_size += iter->chunk_sz;
			iter->buf = realloc(iter->buf, iter->buf_size);
			continue;
		}

		if (err == Z_STREAM_END)                    /* [한국어] 이 스트림 완료 — 루프 탈출. */
			break;
	}

	ret = (void *) stream->next_in - ic->buf;           /* [한국어] 소비된 입력 바이트 = 진행된 포인터 - 시작 포인터. */

	dprint(FD_COMPRESS, "inflated to size=%lu\n", (unsigned long) iter->buf_size);

	return ret;
}

/*
 * Inflate stored compressed chunks, or write them directly to the log
 * file if so instructed.
 */
/*
 * [한국어]
 * inflate_gz_chunks - io_log->chunk_list의 모든 압축 청크를 순회 처리.
 *
 * @log: 대상 io_log. log->log_gz_store=1이면 바이너리로 그대로 쓰고, 0이면 inflate 후 CSV.
 * @f:   출력 FILE* (바이너리 또는 텍스트).
 *
 * @return: 0 = 성공, 음수/양수 = inflate 또는 fwrite 에러.
 *
 * 두 모드:
 *   - log_gz_store: 사용자가 gzip 파일 원하는 경우 — 압축 바이트 그대로 fwrite(1회 호출).
 *                    외부에서 gunzip/zcat으로 풀어보면 CSV 복구.
 *   - 기본(텍스트 .log): inflate → flush_samples로 CSV 출력 → 대용량 로그도 사람이 읽을 수 있음.
 *
 * 호출: flush_log() — 잡 종료 시 로그 덤프.
 */
static int inflate_gz_chunks(struct io_log *log, FILE *f)
{
	struct inflate_chunk_iter iter = { .chunk_sz = log->log_gz, };  /* [한국어] 초기 확장 크기 = 사용자가 지정한 log_gz. */
	z_stream stream;

	while (!flist_empty(&log->chunk_list)) {
		struct iolog_compress *ic;

		ic = flist_first_entry(&log->chunk_list, struct iolog_compress, list);
		flist_del(&ic->list);

		if (log->log_gz_store) {
			size_t ret;

			dprint(FD_COMPRESS, "log write chunk size=%lu, "
				"seq=%u\n", (unsigned long) ic->len, ic->seq);

			ret = fwrite(ic->buf, ic->len, 1, f);  /* [한국어] 압축된 바이트를 파일에 직접 기록 — gzip 포맷 저장. */
			if (ret != 1 || ferror(f)) {
				iter.err = errno;
				log_err("fio: error writing compressed log\n");
			}
		} else
			inflate_chunk(ic, log->log_gz_store, f, &stream, &iter);  /* [한국어] 해제 후 CSV 출력(내부에서 flush_samples). */

		free_chunk(ic);                             /* [한국어] 청크 소비 후 해제. */
	}

	if (iter.seq) {                                     /* [한국어] 마지막 시퀀스 남아있으면 flush. */
		finish_chunk(&stream, f, &iter);
		free(iter.buf);
	}

	return iter.err;
}

/*
 * Open compressed log file and decompress the stored chunks and
 * write them to stdout. The chunks are stored sequentially in the
 * file, so we iterate over them and do them one-by-one.
 */
/*
 * [한국어]
 * iolog_file_inflate - `fio --inflate-log=<file>` CLI 구현. 저장된 .log.gz를 stdout에 CSV로 해제.
 *
 * @file: 압축 로그 파일 경로.
 * @return: 0 = 성공, 1 = 실패.
 *
 * 알고리즘:
 *   1) 파일 전체를 메모리에 fread.
 *   2) ic.seq=1로 시작, 전체 바이트가 소진될 때까지 반복 inflate_chunk.
 *   3) 각 반복에서 ic.buf/ic.len을 소비된 만큼 전진, seq 증가.
 *
 * 왜 시퀀스 번호 증가?: 파일에는 여러 압축 스트림이 순차 저장되어 있고 각각 Z_STREAM_END로
 *   끝나므로, inflate_chunk가 시퀀스 전환을 감지하도록 번호를 증가.
 *
 * 호출: fio CLI 옵션 파서 → iolog_file_inflate().
 */
int iolog_file_inflate(const char *file)
{
	struct inflate_chunk_iter iter = { .chunk_sz = 64 * 1024 * 1024, };  /* [한국어] 64 MiB 초기 버퍼 — 큰 로그 파일 해제 시 realloc 최소화. */
	struct iolog_compress ic;
	z_stream stream;
	struct stat sb;
	size_t ret;
	size_t total;
	void *buf;
	FILE *f;

	f = fopen(file, "rb");                              /* [한국어] 바이너리 모드 — Windows 라인엔딩 변환 방지. */
	if (!f) {
		perror("fopen");
		return 1;
	}

	if (stat(file, &sb) < 0) {                          /* [한국어] 파일 크기 조회. */
		fclose(f);
		perror("stat");
		return 1;
	}

	ic.buf = buf = malloc(sb.st_size);                  /* [한국어] 파일 전체 할당 — 매우 큰 로그에서는 메모리 사용 주의. */
	ic.len = sb.st_size;
	ic.seq = 1;

	ret = fread(ic.buf, ic.len, 1, f);                  /* [한국어] 전체 일괄 읽기 (nitems=1, size=파일크기). */
	if (ret == 0 && ferror(f)) {
		perror("fread");
		fclose(f);
		free(buf);
		return 1;
	} else if (ferror(f) || (!feof(f) && ret != 1)) {
		log_err("fio: short read on reading log\n");
		fclose(f);
		free(buf);
		return 1;
	}

	fclose(f);

	/*
	 * Each chunk will return Z_STREAM_END. We don't know how many
	 * chunks are in the file, so we just keep looping and incrementing
	 * the sequence number until we have consumed the whole compressed
	 * file.
	 */
	/* [한국어] 각 시퀀스는 Z_STREAM_END로 종료 → seq를 1씩 증가시키며 다음 스트림 처리. */
	total = ic.len;
	do {
		size_t iret;

		iret = inflate_chunk(&ic,  1, stdout, &stream, &iter);  /* [한국어] gz_hdr=1 — gzip 헤더 자동 감지. */
		total -= iret;
		if (!total)
			break;
		if (iter.err)
			break;

		ic.seq++;                                    /* [한국어] 다음 시퀀스로 인덱싱 증가. */
		ic.len -= iret;
		ic.buf += iret;                              /* [한국어] 포인터 전진 — 소비한 바이트만큼. */
	} while (1);

	if (iter.seq) {
		finish_chunk(&stream, stdout, &iter);
		free(iter.buf);
	}

	free(buf);
	return iter.err;
}

#else

/*
 * [한국어] CONFIG_ZLIB 미지원 플랫폼용 no-op 구현. inflate_gz_chunks는 성공으로 반환,
 * iolog_file_inflate는 에러로 반환(기능 비활성화).
 */
static int inflate_gz_chunks(struct io_log *log, FILE *f)
{
	return 0;
}

int iolog_file_inflate(const char *file)
{
	log_err("fio: log inflation not possible without zlib\n");
	return 1;
}

#endif

/*
 * [한국어]
 * flush_log - io_log의 모든 청크(압축된 것 + 평문)를 파일에 덤프.
 *
 * @log:        대상 io_log.
 * @do_append:  true = "a"/"ab" append 모드, false = "w"/"wb" 덮어쓰기.
 *
 * 처리 순서:
 *   1) 파일 모드 결정: log_gz_store이면 바이너리("wb"/"ab"), 아니면 텍스트("w"/"a").
 *   2) fopen → set_file_buffer(1 MiB).
 *   3) inflate_gz_chunks — 압축 청크 먼저 처리(log_gz_store면 바이너리 기록, 아니면 inflate+CSV).
 *   4) io_logs 리스트 순회 — 평문 청크들을 flush_samples 또는 flush_hist_samples로 출력.
 *   5) fclose + clear_file_buffer.
 *
 * 왜 압축 먼저인가: gz 청크는 기록 중 메모리 상한 초과로 생긴 것들이고, io_logs의 평문은
 *   가장 최근 기록이므로 타임라인 순서대로 ①gz ②plain 순으로 출력.
 *
 * 히스토그램 분기: log == td->clat_hist_log이면 flush_hist_samples(delta 버킷 출력),
 *   그 외는 flush_samples(필드 선택적 CSV).
 *
 * 실행 컨텍스트: finish_log()에서 호출. 잡 스레드 또는 메인(집계 로그).
 */
void flush_log(struct io_log *log, bool do_append)
{
	void *buf;
	FILE *f;

	/*
	 * If log_gz_store is true, we are writing a binary file.
	 * Set the mode appropriately (on all platforms) to avoid issues
	 * on windows (line-ending conversions, etc.)
	 */
	/* [한국어] 파일 오픈 모드 4가지 조합: (append|overwrite) × (binary|text). gz_store는 이진. */
	if (!do_append)
		if (log->log_gz_store)
			f = fopen(log->filename, "wb");
		else
			f = fopen(log->filename, "w");
	else
		if (log->log_gz_store)
			f = fopen(log->filename, "ab");
		else
			f = fopen(log->filename, "a");
	if (!f) {
		perror("fopen log");
		return;
	}

	buf = set_file_buffer(f);                           /* [한국어] 1 MiB setvbuf — 대용량 로그 덤프 속도 향상. */

	/* [한국어] 압축 청크를 먼저 파일에 씀(바이너리 모드면 그대로, 아니면 inflate 후 CSV). */
	inflate_gz_chunks(log, f);

	/* [한국어] 평문 청크 순회 — io_logs 리스트 헤드부터 꺼내 flush. */
	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);

		/* [한국어] 히스토그램 로그는 별도 포맷 — 버킷 배열. clat_hist_log 포인터로 식별. */
		if (log->td && log == log->td->clat_hist_log)
			flush_hist_samples(f, log->hist_coarseness, cur_log->log,
			                   log_sample_sz(log, cur_log));
		else
			flush_samples(f, cur_log->log, log_sample_sz(log, cur_log));

		sfree(cur_log);                              /* [한국어] 청크 구조체 해제(SHM 힙). log 배열은 cur_log->log에서 free되지 않고 유지 — 실수가 아닌가? flush_samples가 데이터를 이미 읽었으므로 memleak 가능, 하지만 기존 동작 유지. */
	}

	fclose(f);
	clear_file_buffer(buf);                             /* [한국어] fclose 후 안전하게 setvbuf 버퍼 해제. */
}

/*
 * [한국어]
 * finish_log - 잡의 특정 로그 하나를 최종 덤프 + 자원 해제.
 *
 * @td:      잡 컨텍스트. td->flags, td->client_type, td->o.per_job_logs 참조.
 * @log:     대상 io_log.
 * @trylock: true = fio_trylock_file (비차단, 실패 시 1 반환하여 재시도 유도),
 *           false = fio_lock_file (차단).
 *
 * @return: 0 = 완료, 1 = trylock 실패(호출자가 나중에 재시도).
 *
 * 단계:
 *   1) TD_F_COMPRESS_LOG 잡이면 iolog_flush()로 잔여 샘플 모두 deflate.
 *   2) fio_lock/trylock으로 같은 파일명의 다른 잡과 배타 접근 확보.
 *   3) client_type == GUI 또는 is_backend이면 TCP로 송신(fio_send_iolog),
 *      그 외는 로컬 파일에 flush_log(append 여부는 per_job_logs 반대).
 *   4) fio_unlock_file + free_log.
 *
 * 왜 per_job_logs에 !을 붙이나: per_job_logs=1이면 파일당 단일 잡 — 덮어쓰기(append=false),
 *   per_job_logs=0이면 여러 잡이 공유 파일 — append=true로 이어쓰기.
 *
 * 호출: write_bandw_log/write_lat_log/... → __write_log → finish_log.
 */
static int finish_log(struct thread_data *td, struct io_log *log, int trylock)
{
	if (td->flags & TD_F_COMPRESS_LOG)
		iolog_flush(log);                           /* [한국어] 잔여 샘플을 deflate하여 chunk_list로 밀어냄. */

	if (trylock) {
		if (fio_trylock_file(log->filename))
			return 1;                           /* [한국어] 잠금 실패 → 재시도 호출자에게 알림. */
	} else
		fio_lock_file(log->filename);

	/* [한국어] GUI/백엔드 모드면 네트워크 전송, 아니면 로컬 파일 덤프. */
	if (td->client_type == FIO_CLIENT_TYPE_GUI || is_backend)
		fio_send_iolog(td, log, log->filename);
	else
		flush_log(log, !td->o.per_job_logs);        /* [한국어] per_job_logs=0 → append=true (공유 파일). */

	fio_unlock_file(log->filename);
	free_log(log);
	return 0;
}

/*
 * [한국어]
 * log_chunk_sizes - 압축 청크 리스트의 총 바이트 수 반환(메모리 사용량 추적용).
 *
 * @log: 대상 io_log.
 * @return: chunk_list 내 모든 iolog_compress의 len 합산.
 *
 * 용도: regrow_logs()가 메모리 상한 초과 판단에 사용. chunk_lock으로 스레드 안전.
 */
size_t log_chunk_sizes(struct io_log *log)
{
	struct flist_head *entry;
	size_t ret;

	if (flist_empty(&log->chunk_list))
		return 0;

	ret = 0;
	pthread_mutex_lock(&log->chunk_lock);               /* [한국어] 압축 헬퍼 스레드가 chunk_list에 추가 중일 수 있으므로 락. */
	flist_for_each(entry, &log->chunk_list) {
		struct iolog_compress *c;

		c = flist_entry(entry, struct iolog_compress, list);
		ret += c->len;
	}
	pthread_mutex_unlock(&log->chunk_lock);
	return ret;
}

#ifdef CONFIG_ZLIB

/*
 * [한국어]
 * iolog_put_deferred - 지연 해제 큐에 포인터 추가. 압축 진행 중 UAF 방지용.
 *
 * @log: 대상 io_log (deferred_items 배열과 deferred_free_lock 소유).
 * @ptr: 해제 지연할 포인터 (원본 샘플 배열).
 *
 * 큐가 가득 차면 포인터를 드롭(메모리 누수) — 경고 1회만 출력(fio_did_warn).
 *
 * 왜 지연 해제: gz_work가 samples를 deflate 완료하기 전에 원본을 free하면 race condition.
 *   완료 후 다음 iolog_cur_flush 호출 시 iolog_free_deferred로 일괄 정리.
 */
static void iolog_put_deferred(struct io_log *log, void *ptr)
{
	if (!ptr)
		return;

	pthread_mutex_lock(&log->deferred_free_lock);
	if (log->deferred < IOLOG_MAX_DEFER) {
		log->deferred_items[log->deferred] = ptr;
		log->deferred++;
	} else if (!fio_did_warn(FIO_WARN_IOLOG_DROP))
		log_err("fio: had to drop log entry free\n");
	pthread_mutex_unlock(&log->deferred_free_lock);
}

/*
 * [한국어]
 * iolog_free_deferred - 지연 해제 큐의 모든 포인터를 실제 free 처리.
 *
 * 호출: iolog_cur_flush가 새 작업 enqueue 후. 이전 작업이 완료되었다면 원본 안전하게 해제.
 */
static void iolog_free_deferred(struct io_log *log)
{
	int i;

	if (!log->deferred)
		return;

	pthread_mutex_lock(&log->deferred_free_lock);

	for (i = 0; i < log->deferred; i++) {
		free(log->deferred_items[i]);
		log->deferred_items[i] = NULL;
	}

	log->deferred = 0;
	pthread_mutex_unlock(&log->deferred_free_lock);
}

/*
 * [한국어]
 * gz_work - zlib deflate로 샘플을 압축하는 핵심 함수.
 *
 * @data: iolog_flush_data — log/samples/nr_samples 입력, data->free로 sfree 여부 결정.
 *
 * @return: 0 = 성공, 1 = 치명 에러(에러 경로에서 생성된 청크 모두 해제 후 반환).
 *
 * 알고리즘:
 *   1) deflateInit(Z_DEFAULT_COMPRESSION)로 스트림 초기화.
 *   2) chunk_seq 증가(원자 아님 — 단일 압축 스레드 전제).
 *   3) avail_in = nr_samples * entry_size, next_in = samples.
 *   4) Z_NO_FLUSH 반복으로 청크 단위 deflate → get_new_chunk로 GZ_CHUNK 버퍼 할당.
 *   5) Z_FINISH로 마지막 flush — Z_BUF_ERROR면 추가 청크로 처리.
 *   6) Z_STREAM_END까지 반복.
 *   7) deflateEnd. 원본 samples를 iolog_put_deferred로 지연 해제.
 *   8) 생성된 청크 리스트를 log->chunk_list에 splice_tail (chunk_lock 보호).
 *   9) data->free면 sfree(data) — 호출자가 smalloc한 경우.
 *
 * 실행 컨텍스트: 압축 헬퍼 스레드(workqueue). log->chunk_lock으로 잡 스레드와 동기화.
 *
 * 호출 체인:
 *   비동기: workqueue → gz_work_async → gz_work (data->free=true).
 *   동기:   iolog_flush → gz_work (data->free=false).
 */
static int gz_work(struct iolog_flush_data *data)
{
	struct iolog_compress *c = NULL;
	struct flist_head list;
	unsigned int seq;
	z_stream stream;
	size_t total = 0;
	int ret;

	INIT_FLIST_HEAD(&list);                             /* [한국어] 이 작업에서 생성된 청크 임시 리스트 — 마지막에 log->chunk_list로 splice. */

	/* [한국어] deflate 스트림 초기화. Z_NULL allocator → zlib 내부 기본 사용. */
	memset(&stream, 0, sizeof(stream));
	stream.zalloc = Z_NULL;
	stream.zfree = Z_NULL;
	stream.opaque = Z_NULL;

	ret = deflateInit(&stream, Z_DEFAULT_COMPRESSION);  /* [한국어] 기본 압축 레벨(=6). 속도와 압축률의 절충. */
	if (ret != Z_OK) {
		log_err("fio: failed to init gz stream\n");
		goto err;
	}

	seq = ++data->log->chunk_seq;                       /* [한국어] 이 작업의 시퀀스 — inflate 시 경계 식별. */

	stream.next_in = (void *) data->samples;
	stream.avail_in = data->nr_samples * log_entry_sz(data->log);  /* [한국어] 입력 바이트 수 = 샘플 수 × 엔트리 크기. */

	dprint(FD_COMPRESS, "deflate input size=%lu, seq=%u, log=%s\n",
				(unsigned long) stream.avail_in, seq,
				data->log->filename);
	/* [한국어] 입력 소진까지 반복 — 청크 단위로 deflate. */
	do {
		if (c)
			dprint(FD_COMPRESS, "seq=%d, chunk=%lu\n", seq,
				(unsigned long) c->len);
		c = get_new_chunk(seq);                     /* [한국어] 새 128 KiB 버퍼 할당. */
		stream.avail_out = GZ_CHUNK;
		stream.next_out = c->buf;
		ret = deflate(&stream, Z_NO_FLUSH);         /* [한국어] 출력 버퍼 채울 때까지 압축 (스트림 종료 아님). */
		if (ret < 0) {
			log_err("fio: deflate log (%d)\n", ret);
			free_chunk(c);
			goto err;
		}

		c->len = GZ_CHUNK - stream.avail_out;       /* [한국어] 이번에 채워진 바이트 수. */
		flist_add_tail(&c->list, &list);
		total += c->len;
	} while (stream.avail_in);

	/* [한국어] Z_FINISH: 스트림 종료 요청. 마지막 청크에 잔여 압축 데이터 + stream end 마커 쓰기. */
	stream.next_out = c->buf + c->len;
	stream.avail_out = GZ_CHUNK - c->len;

	ret = deflate(&stream, Z_FINISH);
	if (ret < 0) {
		/*
		 * Z_BUF_ERROR is special, it just means we need more
		 * output space. We'll handle that below. Treat any other
		 * error as fatal.
		 */
		/* [한국어] Z_BUF_ERROR: 출력 공간 부족 — 치명 아님, 아래 루프에서 추가 청크로 처리. */
		if (ret != Z_BUF_ERROR) {
			log_err("fio: deflate log (%d)\n", ret);
			flist_del(&c->list);
			free_chunk(c);
			goto err;
		}
	}

	total -= c->len;                                    /* [한국어] 마지막 청크 len 업데이트(Z_FINISH 추가 출력 반영). */
	c->len = GZ_CHUNK - stream.avail_out;
	total += c->len;
	dprint(FD_COMPRESS, "seq=%d, chunk=%lu\n", seq, (unsigned long) c->len);

	/* [한국어] Z_STREAM_END 미도달 → 추가 청크를 할당하며 Z_FINISH 반복. */
	if (ret != Z_STREAM_END) {
		do {
			c = get_new_chunk(seq);
			stream.avail_out = GZ_CHUNK;
			stream.next_out = c->buf;
			ret = deflate(&stream, Z_FINISH);
			c->len = GZ_CHUNK - stream.avail_out;
			total += c->len;
			flist_add_tail(&c->list, &list);
			dprint(FD_COMPRESS, "seq=%d, chunk=%lu\n", seq,
				(unsigned long) c->len);
		} while (ret != Z_STREAM_END);
	}

	dprint(FD_COMPRESS, "deflated to size=%lu\n", (unsigned long) total);

	ret = deflateEnd(&stream);                          /* [한국어] zlib 내부 리소스 해제. */
	if (ret != Z_OK)
		log_err("fio: deflateEnd %d\n", ret);

	/* [한국어] 원본 샘플 배열 소유권 이전 — 즉시 free 금지(다른 스레드가 아직 참조 중일 수 있음).
	 *   iolog_free_deferred에서 일괄 해제. */
	iolog_put_deferred(data->log, data->samples);

	/* [한국어] 로컬 리스트를 log->chunk_list 꼬리에 일괄 병합. chunk_lock으로 잡 스레드(log_chunk_sizes)와 동기화. */
	if (!flist_empty(&list)) {
		pthread_mutex_lock(&data->log->chunk_lock);
		flist_splice_tail(&list, &data->log->chunk_list);
		pthread_mutex_unlock(&data->log->chunk_lock);
	}

	ret = 0;
done:
	if (data->free)
		sfree(data);                                /* [한국어] 비동기 호출자(iolog_cur_flush)는 smalloc → sfree. */
	return ret;
err:
	/* [한국어] 에러 경로: 생성한 로컬 청크 모두 해제하여 메모리 누수 방지. */
	while (!flist_empty(&list)) {
		c = flist_first_entry(list.next, struct iolog_compress, list);
		flist_del(&c->list);
		free_chunk(c);
	}
	ret = 1;
	goto done;
}

/*
 * Invoked from our compress helper thread, when logging would have exceeded
 * the specified memory limitation. Compresses the previously stored
 * entries.
 */
/*
 * [한국어]
 * gz_work_async - 워크큐 함수 포인터 시그니처에 맞춘 gz_work 래퍼.
 *
 * @sw:   submit_worker (현재 사용 안 함 — 시그니처만 맞춤).
 * @work: workqueue_work 베이스 — container_of로 iolog_flush_data 복원.
 * @return: gz_work 결과 (0 또는 1).
 *
 * 실행 컨텍스트: 압축 헬퍼 스레드. workqueue가 dequeue 후 이 함수 호출.
 */
static int gz_work_async(struct submit_worker *sw, struct workqueue_work *work)
{
	return gz_work(container_of(work, struct iolog_flush_data, work));
}

/*
 * [한국어]
 * gz_init_worker - 압축 워커 스레드 초기화(CPU 어피니티 설정).
 *
 * @sw: submit_worker. sw->wq->td로 잡 접근.
 * @return: 0 = 성공, 1 = 어피니티 실패.
 *
 * 옵션 log_gz_cpumask가 설정된 경우에만 적용 — 잡의 I/O 스레드와 다른 코어에 압축 스레드를
 * 고정하여 캐시 간섭 최소화.
 */
static int gz_init_worker(struct submit_worker *sw)
{
	struct thread_data *td = sw->wq->td;

	if (!fio_option_is_set(&td->o, log_gz_cpumask))
		return 0;

	if (fio_setaffinity(gettid(), td->o.log_gz_cpumask) == -1) {  /* [한국어] sched_setaffinity(2)로 현재 스레드를 지정 코어로 바인딩. */
		log_err("gz: failed to set CPU affinity\n");
		return 1;
	}

	return 0;
}

/*
 * [한국어] 로그 압축 워크큐 콜백 테이블.
 *   .fn: 실제 압축 함수.
 *   .init_worker_fn: 스레드 생성 후 1회 호출 (어피니티 설정).
 *   .nice: 스레드 nice 값 — 양수이면 낮은 우선순위(잡 스레드에 CPU 양보).
 */
static struct workqueue_ops log_compress_wq_ops = {
	.fn		= gz_work_async,
	.init_worker_fn	= gz_init_worker,
	.nice		= 1,
};

/*
 * [한국어]
 * iolog_compress_init - 잡에 TD_F_COMPRESS_LOG이 있으면 압축 워크큐 1개 스레드 생성.
 *
 * @td:     잡 컨텍스트.
 * @sk_out: 서버 모드 소켓 출력 — 압축 워커가 상태 보고 시 사용.
 * @return: 0 = 성공 또는 압축 불필요.
 *
 * 호출: thread_main() 초기화 단계.
 */
int iolog_compress_init(struct thread_data *td, struct sk_out *sk_out)
{
	if (!(td->flags & TD_F_COMPRESS_LOG))
		return 0;

	workqueue_init(td, &td->log_compress_wq, &log_compress_wq_ops, 1, sk_out);  /* [한국어] 스레드 수 = 1(순차 압축). */
	return 0;
}

/*
 * [한국어]
 * iolog_compress_exit - 압축 워크큐 종료 + 스레드 join.
 *
 * 호출: thread_main() 종료 단계.
 */
void iolog_compress_exit(struct thread_data *td)
{
	if (!(td->flags & TD_F_COMPRESS_LOG))
		return;

	workqueue_exit(&td->log_compress_wq);
}

/*
 * Queue work item to compress the existing log entries. We reset the
 * current log to a small size, and reference the existing log in the
 * data that we queue for compression. Once compression has been done,
 * this old log is freed. Will not return until the log compression
 * has completed, and will flush all previous logs too
 */
/*
 * [한국어]
 * iolog_flush - 잔여 로그 엔트리를 모두 동기적으로 압축(종료 시 호출).
 *
 * @log: 대상 io_log.
 * @return: 0 = 성공 또는 빈 로그, 1 = malloc 실패.
 *
 * 단계:
 *   1) workqueue_flush — 이미 enqueue된 비동기 작업 모두 완료 대기.
 *   2) 단일 iolog_flush_data를 스택 재사용(data->free=false) — 여러 청크를 순차 처리.
 *   3) io_logs 리스트에서 청크 하나씩 꺼내 gz_work로 직접(동기) 압축.
 *   4) 최종 free(data).
 *
 * 차이점 vs iolog_cur_flush: 이쪽은 블로킹 + 전체, 저쪽은 논블로킹 + 현재 청크만.
 *
 * 호출: finish_log()에서 잡 종료 시 모든 샘플을 chunk_list로 밀어내기 위해.
 */
static int iolog_flush(struct io_log *log)
{
	struct iolog_flush_data *data;

	workqueue_flush(&log->td->log_compress_wq);         /* [한국어] 비동기 큐를 먼저 모두 비움 — 순서 보장. */
	data = malloc(sizeof(*data));
	if (!data)
		return 1;

	data->log = log;
	data->free = false;                                 /* [한국어] 스택 재사용 — gz_work 내부에서 sfree 금지. */

	while (!flist_empty(&log->io_logs)) {
		struct io_logs *cur_log;

		cur_log = flist_first_entry(&log->io_logs, struct io_logs, list);
		flist_del_init(&cur_log->list);

		data->samples = cur_log->log;
		data->nr_samples = cur_log->nr_samples;

		sfree(cur_log);                              /* [한국어] io_logs 구조체만 해제 — log 배열은 data->samples로 이전. */

		gz_work(data);                               /* [한국어] 동기 호출 — 블로킹 대기 후 반환. */
	}

	free(data);
	return 0;
}

/*
 * [한국어]
 * iolog_cur_flush - 현재 로그 청크 하나를 비동기 압축 큐에 제출.
 *
 * @log:     대상 io_log.
 * @cur_log: 압축할 현재 청크 — 호출 후 샘플 포인터는 NULL로 리셋되어 재사용 가능.
 *
 * @return: 0 = 성공 enqueue, 1 = smalloc 실패.
 *
 * 소유권 이전:
 *   - cur_log->log(샘플 배열) → data->samples (gz_work가 완료 후 iolog_put_deferred).
 *   - cur_log는 "빈 청크"로 리셋 (호출자가 계속 샘플 적재 가능).
 *
 * 호출: __add_log_sample/regrow_logs — pending 버퍼가 찼을 때 새 버퍼를 받기 전 플러시.
 */
int iolog_cur_flush(struct io_log *log, struct io_logs *cur_log)
{
	struct iolog_flush_data *data;

	data = smalloc(sizeof(*data));                      /* [한국어] SHM 힙 — 압축 스레드와 공유. */
	if (!data)
		return 1;

	data->log = log;

	data->samples = cur_log->log;
	data->nr_samples = cur_log->nr_samples;
	data->free = true;                                  /* [한국어] 비동기 워커가 완료 후 스스로 sfree. */

	/* [한국어] 청크를 "빈 상태"로 리셋 — 호출자는 같은 cur_log 구조체에 다시 샘플을 쌓을 수 있다. */
	cur_log->nr_samples = cur_log->max_samples = 0;
	cur_log->log = NULL;

	workqueue_enqueue(&log->td->log_compress_wq, &data->work);  /* [한국어] 압축 워커 스레드가 이 작업을 dequeue하여 gz_work_async 실행. */

	iolog_free_deferred(log);                           /* [한국어] 이전 호출에서 put_deferred한 포인터들 해제 — 이 시점엔 압축 완료 가능성 큼. */

	return 0;
}
#else

/*
 * [한국어] CONFIG_ZLIB 미지원 플랫폼 — 압축 함수들은 항상 에러/no-op.
 */
static int iolog_flush(struct io_log *log)
{
	return 1;
}

int iolog_cur_flush(struct io_log *log, struct io_logs *cur_log)
{
	return 1;
}

int iolog_compress_init(struct thread_data *td, struct sk_out *sk_out)
{
	return 0;
}

void iolog_compress_exit(struct thread_data *td)
{
}

#endif

/*
 * [한국어]
 * iolog_cur_log - io_logs 리스트의 마지막(가장 최근) 청크 반환.
 *
 * @log: 대상 io_log.
 * @return: 마지막 io_logs 또는 NULL (빈 리스트).
 *
 * 용도: 새 샘플을 추가할 대상 청크를 stat.c가 질의할 때.
 */
struct io_logs *iolog_cur_log(struct io_log *log)
{
	if (flist_empty(&log->io_logs))
		return NULL;

	return flist_last_entry(&log->io_logs, struct io_logs, list);
}

/*
 * [한국어]
 * iolog_nr_samples - 전체 io_logs 리스트의 nr_samples 합산.
 *
 * 용도: 종료 로그 출력 시 "수집된 샘플 N개" 통계용.
 */
uint64_t iolog_nr_samples(struct io_log *iolog)
{
	struct flist_head *entry;
	uint64_t ret = 0;

	flist_for_each(entry, &iolog->io_logs) {
		struct io_logs *cur_log;

		cur_log = flist_entry(entry, struct io_logs, list);
		ret += cur_log->nr_samples;
	}

	return ret;
}

/*
 * [한국어]
 * __write_log - NULL 방어 래퍼. log이 존재하면 finish_log 호출.
 *
 * @try: 1 = trylock(논블로킹), 0 = lock(블로킹).
 * @return: finish_log 결과 (0 = 완료, 1 = 재시도).
 */
static int __write_log(struct thread_data *td, struct io_log *log, int try)
{
	if (log)
		return finish_log(td, log, try);

	return 0;
}

/*
 * [한국어]
 * write_iops_log - IOPS 로그 덤프 래퍼. per-unit 매칭 확인 후 finish_log.
 *
 * @unit_log: 외부 요청이 unit 로그 대상이면 true.
 * @return: 0 = 완료, 1 = trylock 실패 (td_writeout_logs에서 재시도).
 *
 * per_unit_log(log): 이 로그가 per-unit 유형이면 true. 요청과 맞으면 덤프 수행.
 */
static int write_iops_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (per_unit_log(td->iops_log) != unit_log)
		return 0;                                   /* [한국어] 이번 요청 대상 아님 — 완료로 처리. */

	ret = __write_log(td, td->iops_log, try);
	if (!ret)
		td->iops_log = NULL;                        /* [한국어] finish_log가 free까지 완료 — 포인터 리셋. */

	return ret;
}

/* [한국어] write_slat_log - submit latency 로그 덤프 래퍼. unit_log=true 요청에만 반응. */
static int write_slat_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->slat_log, try);
	if (!ret)
		td->slat_log = NULL;

	return ret;
}

/* [한국어] write_clat_log - completion latency 로그 덤프 래퍼. unit_log=true 요청에만 반응. */
static int write_clat_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->clat_log, try);
	if (!ret)
		td->clat_log = NULL;

	return ret;
}

/* [한국어] write_clat_hist_log - clat 히스토그램 로그 덤프 래퍼. unit_log=true 요청에만 반응.
 *   flush_log가 log == td->clat_hist_log 조건으로 flush_hist_samples 분기. */
static int write_clat_hist_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->clat_hist_log, try);
	if (!ret)
		td->clat_hist_log = NULL;

	return ret;
}

/* [한국어] write_lat_log - 총 latency(slat+clat) 로그 덤프 래퍼. */
static int write_lat_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (!unit_log)
		return 0;

	ret = __write_log(td, td->lat_log, try);
	if (!ret)
		td->lat_log = NULL;

	return ret;
}

/* [한국어] write_bandw_log - 대역폭(bandwidth) 로그 덤프 래퍼. per-unit 매칭 분기. */
static int write_bandw_log(struct thread_data *td, int try, bool unit_log)
{
	int ret;

	if (per_unit_log(td->bw_log) != unit_log)
		return 0;

	ret = __write_log(td, td->bw_log, try);
	if (!ret)
		td->bw_log = NULL;

	return ret;
}

/*
 * [한국어] 로그 타입별 비트 마스크 — td_writeout_logs의 진행 추적용.
 *   각 비트는 log_types 테이블의 한 엔트리에 대응.
 *   ALL_LOG_NR: 총 로그 수 = 비트 수 = 6.
 */
enum {
	BW_LOG_MASK	= 1,        /* [한국어] 대역폭 로그 완료 표식. */
	LAT_LOG_MASK	= 2,        /* [한국어] 총 latency 로그 완료 표식. */
	SLAT_LOG_MASK	= 4,        /* [한국어] submit latency 완료 표식. */
	CLAT_LOG_MASK	= 8,        /* [한국어] completion latency 완료 표식. */
	IOPS_LOG_MASK	= 16,       /* [한국어] IOPS 로그 완료 표식. */
	CLAT_HIST_LOG_MASK = 32,    /* [한국어] clat 히스토그램 로그 완료 표식. */

	ALL_LOG_NR	= 6,        /* [한국어] 총 로그 타입 수. log_types 배열 길이와 일치해야 함. */
};

/*
 * [한국어] struct log_type - 마스크와 덤프 함수의 매핑 테이블 엔트리.
 *   mask: 완료 비트.
 *   fn:   write_*_log 함수 포인터.
 */
struct log_type {
	unsigned int mask;
	int (*fn)(struct thread_data *, int, bool);
};

/*
 * [한국어] 모든 로그 타입의 매핑 테이블. td_writeout_logs가 이 테이블을 순회하며
 *   아직 완료되지 않은 로그만 시도하여 파일 잠금 충돌을 회피한다.
 */
static struct log_type log_types[] = {
	{
		.mask	= BW_LOG_MASK,
		.fn	= write_bandw_log,
	},
	{
		.mask	= LAT_LOG_MASK,
		.fn	= write_lat_log,
	},
	{
		.mask	= SLAT_LOG_MASK,
		.fn	= write_slat_log,
	},
	{
		.mask	= CLAT_LOG_MASK,
		.fn	= write_clat_log,
	},
	{
		.mask	= IOPS_LOG_MASK,
		.fn	= write_iops_log,
	},
	{
		.mask	= CLAT_HIST_LOG_MASK,
		.fn	= write_clat_hist_log,
	}
};

/*
 * [한국어]
 * td_writeout_logs - 한 잡의 모든 로그(bw/lat/slat/clat/clat_hist/iops)를 덤프.
 *
 * @td:        잡 컨텍스트.
 * @unit_logs: true = per-unit 로그만, false = 공유(집계용) 로그만.
 *
 * 알고리즘:
 *   1) 상태 TD_FINISHING으로 전환 (reap 시 식별용).
 *   2) finalize_logs — 마지막 펜딩 샘플을 io_logs로 밀어냄(stat.c).
 *   3) 루프: 아직 완료되지 않은 로그마다 write_*_log 호출.
 *      - try=1(trylock)을 log_left!=1일 때만(마지막은 블로킹 락).
 *      - 성공 시 log_mask에 비트 추가, log_left 감소.
 *   4) 진전 없으면 5ms usleep 후 재시도 (파일 잠금 충돌 해소 대기).
 *   5) 상태 복원.
 *
 * 왜 재시도 루프: per_job_logs=0일 때 다른 잡이 같은 파일명을 잠그고 있을 수 있으므로
 *   trylock 실패 시 다른 로그부터 시도하고 나중에 재방문. 모든 락이 경쟁 상태라면 5ms 대기.
 *
 * 실행 컨텍스트: 메인 프로세스가 잡 수거 후 순차 호출. 또는 잡 스레드 자체 종료 시.
 *
 * 호출 체인: fio_writeout_logs() → td_writeout_logs() → write_*_log → finish_log.
 */
void td_writeout_logs(struct thread_data *td, bool unit_logs)
{
	unsigned int log_mask = 0;                          /* [한국어] 완료된 로그 비트 누적. */
	unsigned int log_left = ALL_LOG_NR;                 /* [한국어] 남은 로그 수 — 0이 되면 루프 종료. */
	int old_state, i;

	/* [한국어] TD_FINISHING 상태로 전환하여 진행 단계 구분 — 외부 관찰자(클라이언트)에 신호. */
	old_state = td_bump_runstate(td, TD_FINISHING);

	finalize_logs(td, unit_logs);                       /* [한국어] stat.c의 finalize_logs — pending 샘플을 io_logs로 flush. */

	/* [한국어] 모든 로그 타입이 완료될 때까지 반복. */
	while (log_left) {
		int prev_log_left = log_left;

		for (i = 0; i < ALL_LOG_NR && log_left; i++) {
			struct log_type *lt = &log_types[i];
			int ret;

			if (!(log_mask & lt->mask)) {       /* [한국어] 아직 완료 안 된 로그만 시도. */
				ret = lt->fn(td, log_left != 1, unit_logs);  /* [한국어] log_left>1이면 trylock, 마지막이면 블로킹. */
				if (!ret) {
					log_left--;
					log_mask |= lt->mask;
				}
			}
		}

		/* [한국어] 이번 라운드에 아무 로그도 완료 못 함 → 5ms 대기 후 재시도. */
		if (prev_log_left == log_left)
			usleep(5000);
	}

	td_restore_runstate(td, old_state);                 /* [한국어] 원래 상태로 복원 (보통 EXITED). */
}

/*
 * [한국어]
 * fio_writeout_logs - 모든 잡의 로그를 덤프. 메인 프로세스가 잡 reap 후 호출.
 *
 * @unit_logs: 단위 로그(true) 또는 집계 로그(false) 선택.
 *
 * for_each_td 매크로는 전역 잡 테이블을 순회(대개 한 번에 하나씩, 병렬 락 없음).
 */
void fio_writeout_logs(bool unit_logs)
{
	for_each_td(td) {
		td_writeout_logs(td, unit_logs);
	} end_for_each();
}
