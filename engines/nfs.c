/*
 * [한국어 설명] NFS I/O 엔진 구현 (nfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 커널 NFS 클라이언트(/sys/fs/nfs, mount.nfs4, kernel rpc)에 의존하지 않고
 * 유저스페이스 라이브러리 libnfs가 제공하는 NFS v3/v4 프로토콜 구현으로 원격
 * NFS 서버에 직접 접속하여 pread/pwrite를 비동기로 발행하는 fio I/O 엔진이다.
 * libnfs는 내부적으로 TCP 소켓 위에 NFS RPC(PROC_NFS3_READ/PROC_NFS3_WRITE 등, RFC1813)
 * 를 직접 조립/파싱하고, 상태머신을 nfs_service()로 한 스텝씩 전진시키는 콜백 기반
 * 비동기 API를 노출한다. fio는 이 비동기 API를 poll(2) 이벤트 루프로 감싸 io_u
 * 생명주기(queue→getevents→event)와 연결한다.
 * 커널 VFS를 우회하므로 페이지 캐시, dcache, 커널 RPC 재시도 경로가 관여하지 않아
 * "순수한 NFS 서버 측 처리 + 네트워크 왕복" 성능만 측정하는 목적에 적합하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend() [backend.c] → load_ioengine("nfs") [ioengines.c]
 *   → .setup = fio_libnfs_setup (use_thread=0 프로세스 모드 강제 — libnfs 정리 훅이
 *                                 pthread 종료 시 hang을 유발하는 이슈 회피)
 *   → .open_file = fio_libnfs_open (최초 호출에서 do_mount로 컨텍스트/이벤트 큐 초기화,
 *                                    이후 nfs_open으로 파일 핸들 nfsfh 획득)
 *   → .queue = fio_libnfs_queue (ddir에 따라 nfs_pread_async / nfs_pwrite_async 발행 후
 *                                 FIO_Q_QUEUED 반환 — 비동기 계약)
 *   → .getevents = fio_libnfs_getevents → nfs_event_loop
 *                   (poll(2) ← nfs_get_fd + nfs_which_events → nfs_service → nfs_callback
 *                    가 완료를 원형 큐 events[]에 푸시)
 *   → .event = fio_libnfs_event (원형 큐에서 io_u 1개 pop하여 fio 코어에 보고)
 *   → .close_file = fio_libnfs_close (nfs_close) / .cleanup = fio_libnfs_cleanup
 *                   (nfs_umount + nfs_destroy_context + events[] free)
 *
 * 실행 컨텍스트는 잡 프로세스(use_thread=0 강제) 내 단일 실행 흐름이다. libnfs 컨텍스트
 * 는 스레드 안전이 아니며 fio 엔진이 단일 잡 문맥에서만 사용하므로 별도 락이 필요 없다.
 *
 * === 타 모듈과의 연결 ===
 * - ../fio.h: thread_data/io_u/ioengine_ops/fio_q_status 등 fio 코어 타입.
 * - ../optgroup.h: FIO_OPT_C_ENGINE, __FIO_OPT_G_NFS — 옵션 카테고리/그룹 상수.
 * - libnfs (nfsc/libnfs.h, nfsc/libnfs-raw.h, nfsc/libnfs-raw-mount.h):
 *     · nfs_init_context / nfs_destroy_context — NFS 컨텍스트(TCP 소켓 + RPC xid 상태머신) 생성/파괴.
 *     · nfs_parse_url_full — "nfs://server[/export[/path]]" URL 파싱(server/path/file 분해).
 *     · nfs_mount — MOUNT RPC 호출로 export 핸들(root filehandle) 획득.
 *     · nfs_umount — UMOUNT RPC.
 *     · nfs_open / nfs_close — OPEN/CLOSE/LOOKUP RPC 조합으로 nfsfh(파일 핸들) 획득·해제.
 *     · nfs_pread_async / nfs_pwrite_async — 비동기 READ/WRITE RPC 발행(콜백 등록).
 *     · nfs_service — 소켓 readable/writable 시 한 스텝 진행(RPC 응답 수신·콜백 호출).
 *     · nfs_which_events — 현재 상태에서 필요한 poll 이벤트 마스크(POLLIN/POLLOUT) 반환.
 *     · nfs_get_fd — libnfs가 관리하는 단일 TCP 소켓 파일 디스크립터 노출.
 *     · nfs_get_error — 마지막 에러 문자열.
 * - poll(2): libnfs 소켓의 readable/writable 이벤트 대기(단일 FD).
 * - 데이터 흐름: io_u.xfer_buf ↔ libnfs 내부 RPC 페이로드 ↔ TCP ↔ NFS 서버.
 *   READ 경로에서 libnfs v1 API는 수신 버퍼를 라이브러리가 소유한 상태로 콜백에 전달하므로
 *   nfs_callback 내부에서 사용자 버퍼로 memcpy가 필요하다. v2 API는 사용자 버퍼를 직접
 *   전달하여 복사를 생략한다(#ifdef LIBNFS_API_V2 분기).
 * - 공유 상태:
 *     · td->eo = fio_libnfs_options* (잡 전용, fio 코어가 option_struct_size로 calloc).
 *       → libnfs 컨텍스트, URL, 이벤트 원형 큐 상태를 모두 보관.
 *     · f->engine_data = nfs_data* (파일 핸들 + options 역참조).
 *
 * === 주요 함수/구조체 요약 ===
 * - enum nfs_op_type: READ/WRITE 외에 메타데이터 벤치(stat/mkdir/rmdir 등)를 위한 예약 enum.
 * - struct fio_libnfs_options: 잡 전용 옵션 + libnfs 컨텍스트 + 완료 원형 큐 상태.
 * - struct nfs_data:          파일당 nfsfh 핸들 + 잡 옵션 역참조.
 * - fio_libnfs_setup():       use_thread=0 강제(libnfs 종료 훅 hang 회피).
 * - fio_libnfs_open():        최초 호출 시 do_mount, 이후 nfs_open.
 * - fio_libnfs_queue():       ddir에 따라 async read/write 제출; FIO_Q_QUEUED 반환.
 * - nfs_callback():           libnfs 완료 콜백 — 결과를 io_u에 반영하고 원형 큐에 push.
 * - nfs_event_loop() / fio_libnfs_getevents() / fio_libnfs_event():
 *                             poll + nfs_service 루프와 event pop.
 * - fio_libnfs_cleanup():     nfs_umount + nfs_destroy_context + events[] 해제.
 */

/* [한국어] 표준 C 라이브러리 — malloc/calloc/free(원형 큐 및 마운트 경로 문자열 할당/해제),
 * strcpy/strlen(URL 파싱 결과의 path+file 결합)에 사용. */
#include <stdlib.h>

/* [한국어] POSIX poll(2) — libnfs가 노출한 단일 TCP 소켓 FD에 대해 POLLIN/POLLOUT 이벤트를
 * 블로킹/논블로킹으로 대기한다. pollfd/struct pollfd/POLLIN 매크로 공급. */
#include <poll.h>

/* [한국어] libnfs 상위(high-level) API 헤더 — nfs_context 타입과 nfs_init_context,
 * nfs_mount, nfs_open, nfs_close, nfs_umount, nfs_pread_async, nfs_pwrite_async,
 * nfs_get_error, nfs_parse_url_full, nfs_destroy_url, nfs_destroy_context 등의 선언. */
#include <nfsc/libnfs.h>

/* [한국어] libnfs raw(low-level) API 헤더 — 이벤트 루프 구동에 필요한 nfs_get_fd,
 * nfs_which_events, nfs_service 선언. 상위 API가 숨기는 상태머신 진행 원시 훅이다. */
#include <nfsc/libnfs-raw.h>

/* [한국어] NFS MOUNT 프로토콜(NFSv3 기반) 관련 타입 선언 — 일부 libnfs 버전에서 raw 헤더가
 * mount 타입을 간접 참조하므로 함께 포함해 빌드 호환성을 확보한다. */
#include <nfsc/libnfs-raw-mount.h>

/* [한국어] fio 코어 공개 헤더 — thread_data/io_u/fio_file/ioengine_ops/fio_q_status 등
 * 엔진 구현에 필수인 타입과 register_ioengine/unregister_ioengine 프로토타입 공급. */
#include "../fio.h"

/* [한국어] fio 옵션 시스템 헤더 — FIO_OPT_C_ENGINE(카테고리: 엔진 옵션),
 * __FIO_OPT_G_NFS(그룹: NFS 엔진) 등 options[] 테이블 선언에 필요한 상수 공급. */
#include "../optgroup.h"

/*
 * [한국어]
 * enum nfs_op_type - 메타데이터 벤치마크 확장을 위한 op 분류자.
 *
 * 현재 소스 코드는 실제로 NFS_READ_WRITE(일반 데이터 I/O) 경로만 구현한다. 나머지 값은
 * 미래에 NFS 메타데이터 연산(디렉터리 생성/삭제, 파일 stat/touch/rm 등)을 벤치마크할 때
 * 사용할 수 있도록 예약되어 있으며, 현 버전에서는 참조되지 않는다.
 */
enum nfs_op_type {
	NFS_READ_WRITE = 0,
	/* [한국어] 일반 pread/pwrite 데이터 I/O 경로 — 현재 구현의 기본값이자 유일한 활성 경로.
	 * 설정자: 없음(0이 기본). 읽는 자: 향후 디스패처 확장 시. */

	NFS_STAT_MKDIR_RMDIR,
	/* [한국어] 디렉터리 stat/mkdir/rmdir 메타벤치(예약).
	 * NFS의 MKDIR/RMDIR/GETATTR RPC 측정을 의도. 현재 미구현. */

	NFS_STAT_TOUCH_RM,
	/* [한국어] 파일 stat/touch/rm 메타벤치(예약).
	 * CREATE/REMOVE/GETATTR RPC 측정을 의도. 현재 미구현. */
};

/*
 * [한국어] NFS 엔진의 잡(thread_data) 전용 옵션 + 런타임 상태 구조체.
 *
 * 수명: fio 코어가 ioengine_ops.option_struct_size에 따라 잡 시작 시 calloc하여 td->eo에
 *       저장. 잡 종료 시 코어가 해제한다.
 * 역할: libnfs 컨텍스트와 URL 같은 옵션, 그리고 완료 이벤트를 저장하는 원형 큐의 생산자/
 *       소비자 포인터·카운터를 한 군데 모아둔다.
 * 동기화: 모든 필드가 동일한 잡 프로세스(use_thread=0) 안에서 단일 흐름으로 접근되므로
 *         락이 필요 없다. 원형 큐는 생산자=nfs_callback(libnfs 내부에서 nfs_service가
 *         불러줌) / 소비자=fio_libnfs_event이지만, 양쪽 모두 같은 스레드 컨텍스트에서
 *         호출되어 동시성이 없다.
 */
struct fio_libnfs_options {
	struct nfs_context *context;
	/* [한국어] libnfs의 NFS 컨텍스트 핸들 — TCP 소켓 + RPC xid 상태머신 + MOUNT/FH 정보 보유.
	 * 설정자: do_mount()에서 nfs_init_context/nfs_mount 성공 시 저장.
	 * 읽는 자: fio_libnfs_queue, nfs_event_loop, fio_libnfs_open, fio_libnfs_close,
	 *          fio_libnfs_cleanup 등 거의 모든 경로.
	 * 값 범위: 유효한 libnfs 컨텍스트 포인터(마운트 후) 또는 NULL(초기화 직후/cleanup 후).
	 * 동기화: libnfs 컨텍스트는 스레드 안전이 아니므로 단일 잡 스레드만 소유해야 한다. */

	char *nfs_url;
	/* [한국어] "nfs://server[:port]/export[/path][?arg=val[&arg=val]*]" 형식의 서버 URL.
	 * 설정자: fio 옵션 파서가 options[] 테이블의 off1=offsetof(...nfs_url)에 strdup된 문자열을 저장.
	 * 읽는 자: fio_libnfs_open → do_mount에 전달되어 nfs_parse_url_full로 server/path/file 분해.
	 * 값 범위: NULL(미지정)이거나 libnfs URL 문법을 따르는 문자열.
	 * 동기화: 잡 생성 후 불변. */

	unsigned int queue_depth;
	/* [한국어] 완료 이벤트 원형 큐 events[]의 크기(= td->o.iodepth).
	 * 설정자: do_mount()에서 td->o.iodepth 값을 복사.
	 * 읽는 자: nfs_callback이 생산자 인덱스를 queue_depth로 모듈로 연산.
	 * 값 범위: 1..UINT_MAX(fio가 iodepth를 보장).
	 * 동기화: 잡 시작 후 불변. */

	/* [한국어] 아래 필드들은 libnfs 완료 콜백(nfs_callback)이 쌓은 io_u를 fio 코어가
	 *         event()로 순서대로 꺼내가기 위한 원형 버퍼 상태이다. 단일 스레드이므로
	 *         원자적 연산은 불필요하다. */

	int outstanding_events;
	/* [한국어] libnfs에 제출되었으나 아직 nfs_callback이 호출되지 않은 비행(in-flight) 건 수.
	 * 설정자: fio_libnfs_queue()에서 async 제출 성공 시 ++; nfs_callback()에서 완료 수신 시 --.
	 * 읽는 자: SHOULD_WAIT 매크로 — iodepth 가득참 판정 또는 flush 대기 여부 결정.
	 * 값 범위: 0..iodepth.
	 * 동기화: 잡 전용. */

	int prev_requested_event_index;
	/* [한국어] fio 코어가 직전에 event() 콜백으로 꺼내 간 이벤트 인덱스.
	 * fio는 getevents()가 반환한 N 건을 event(0), event(1), ..., event(N-1) 순서로 순차
	 * 호출하는 규약이 있으며, 이 값은 그 규약 위반 여부를 assert로 검증하는 용도.
	 * 설정자: fio_libnfs_event가 event 인덱스를 저장(또는 모두 소진 시 -1로 리셋).
	 * 읽는 자: fio_libnfs_event 다음 호출에서 assert(prev+1 == event) 검증.
	 * 값 범위: -1(미수확) 또는 0..iodepth-1. */

	int next_buffered_event;
	/* [한국어] 원형 버퍼 events[]의 소비자(consumer) 포인터 — 다음 event() 호출이 pop할 슬롯.
	 * 설정자: fio_libnfs_event가 pop 후 +1 mod iodepth 전진.
	 * 값 범위: 0..iodepth-1. */

	int buffered_event_count;
	/* [한국어] libnfs 완료 콜백이 push했지만 fio가 아직 event()로 꺼내지 않은 대기 건수.
	 * 설정자: nfs_callback push 시 ++; fio_libnfs_event pop 시 --.
	 * 읽는 자: nfs_event_loop 반환값(fio 코어에 보고할 완료 수), fio_libnfs_event 검증.
	 * 값 범위: 0..iodepth. */

	int free_event_buffer_index;
	/* [한국어] 원형 버퍼 events[]의 생산자(producer) 포인터 — 다음 nfs_callback push 슬롯.
	 * 설정자: nfs_callback이 push 후 +1 mod queue_depth 전진.
	 * 값 범위: 0..iodepth-1. */

	struct io_u**events;
	/* [한국어] 완료된 io_u 포인터를 담는 원형 배열(크기 = queue_depth).
	 * 설정자: do_mount()에서 calloc 할당; nfs_callback이 생산자 인덱스 슬롯에 write.
	 * 읽는 자: fio_libnfs_event가 소비자 인덱스 슬롯에서 pop하고 NULL로 비움.
	 * 값 범위: 각 슬롯은 유효한 io_u* 또는 NULL(비어 있음).
	 * 동기화: 잡 전용 단일 스레드 — 생산/소비가 같은 실행 흐름에서 발생.
	 * 주의: 선언에 typo가 있어 "io_u**events"(공백 없음)처럼 보이지만 의미는 struct io_u**. */
};

/*
 * [한국어] 파일별(fio_file 단위) NFS 상태. f->engine_data가 이 포인터를 가짐.
 *
 * 수명: fio_libnfs_open()에서 calloc; fio_libnfs_close()에서 free.
 * 용도: 완료 콜백이 io_u->file->engine_data를 통해 잡 옵션(fio_libnfs_options)에 역참조
 *       접근할 수 있도록 포인터를 연결해 둔다.
 */
struct nfs_data {
	struct nfsfh *nfsfh;
	/* [한국어] libnfs의 파일 핸들 — nfs_open()이 반환하는 불투명 구조체 포인터.
	 * 내부적으로 NFS file handle(fh), access 모드, 현재 오프셋(lseek용 — async 경로는
	 * 오프셋을 인자로 받으므로 영향 없음) 등을 보관한다.
	 * 설정자: fio_libnfs_open의 nfs_open 호출.
	 * 읽는 자: queue_read/queue_write가 nfs_pread_async/nfs_pwrite_async에 전달.
	 *          fio_libnfs_close가 nfs_close에 전달하여 해제.
	 * 값 범위: 유효 핸들 또는 NULL(열기 실패 시).
	 * 동기화: 해당 파일은 잡 단독 소유이므로 스레드 간 공유 없음. */

	struct fio_libnfs_options *options;
	/* [한국어] 잡 전역 fio_libnfs_options 구조체로의 역링크.
	 * queue_write/queue_read/nfs_callback 등이 io_u->file→engine_data→options 경로로
	 * context와 원형 큐 상태에 접근하기 위해 사용한다.
	 * 설정자: fio_libnfs_open에서 td->eo 값으로 초기화.
	 * 읽는 자: queue/callback 경로 전반.
	 * 값 범위: 해당 잡의 td->eo와 동일한 유효 포인터.
	 * 동기화: 잡 생성 후 불변. */
};

/*
 * [한국어] fio 옵션 파서용 테이블 — nfs_url 하나만 노출한다.
 *
 * fio 코어는 엔진 옵션을 이 테이블로 발견하여 jobfile/CLI(--nfs_url=...)에서 파싱하고
 * option_struct_size만큼 할당된 td->eo 버퍼의 off1 오프셋에 결과 문자열(strdup)을 저장한다.
 */
static struct fio_option options[] = {
	{
		.name	= "nfs_url",
		/* [한국어] 잡 파일에서 사용할 옵션 키(짧은 이름). 예: nfs_url=nfs://host/export/foo */

		.lname	= "nfs_url",
		/* [한국어] 긴 이름(사람이 읽는 설명용). 짧은 이름과 동일. */

		.type	= FIO_OPT_STR_STORE,
		/* [한국어] 문자열 저장 타입 — 파서가 값을 strdup하여 off1 위치의 char*에 기록. */

		.help	= "URL in libnfs format, eg nfs://<server|ipv4|"
			  "ipv6>/path[?arg=val[&arg=val]*]",
		/* [한국어] --cmdhelp=nfs_url 및 --enghelp 출력에 사용되는 도움말.
		 * libnfs가 지원하는 전체 URL 문법(서버는 호스트명/IPv4/IPv6, 경로 뒤 쿼리 인자로
		 * 프로토콜 버전·보안 모드 지정 가능)을 안내한다. */

		.off1	= offsetof(struct fio_libnfs_options, nfs_url),
		/* [한국어] td->eo 버퍼 내 저장 오프셋 — nfs_url 필드로 파싱 결과를 투입. */

		.category = FIO_OPT_C_ENGINE,
		/* [한국어] 옵션 카테고리: 엔진 전용(도움말/그룹핑용). */

		.group	= __FIO_OPT_G_NFS,
		/* [한국어] 옵션 그룹: NFS 엔진 — fio --enghelp 출력이나 옵션 검증에 사용. */
	},
	{
		.name     = NULL,
		/* [한국어] 옵션 배열 종료 센티넬 — fio 파서는 name==NULL을 만나면 루프 종료. */
	},
};

/*
 * [한국어]
 * fio_libnfs_event - ioengine_ops.event 콜백. 원형 큐에서 io_u 하나를 pop.
 *
 * @td:    잡 컨텍스트.
 * @event: 이번 getevents 결과 내에서의 인덱스(0부터 시작, 순차 호출 규약).
 * @return: 완료된 io_u 포인터(NULL이 아님 — assert).
 *
 * fio 코어는 getevents()가 반환한 N개의 완료를 event(0), event(1), ..., event(N-1) 순서로
 * 순차 호출하여 각 io_u를 수확한다. 이 함수는 소비자 인덱스(next_buffered_event)에서
 * io_u를 꺼내고 슬롯을 NULL로 비운 뒤 인덱스를 원형으로 전진시킨다.
 *
 * 실행 컨텍스트: 잡 프로세스(단일 스레드). fio_libnfs_getevents 직후 fio 코어가 연속 호출.
 * 에러 경로: assert 실패 시 프로세스 abort(상태머신 깨짐 감지).
 *
 * 호출 체인: fio 코어(io_u_queued_complete 등) → [fio_libnfs_event] → io_u 반환.
 */
static struct io_u *fio_libnfs_event(struct thread_data *td, int event)
{
	struct fio_libnfs_options *o = td->eo;                /* [한국어] 잡 전용 옵션/큐 상태 획득. */
	struct io_u *io_u = o->events[o->next_buffered_event]; /* [한국어] 소비자 포인터 슬롯에서 io_u 꺼냄(pop 후보). */

	assert(o->events[o->next_buffered_event]);   /* [한국어] 슬롯이 비어 있지 않아야 함(논리 오류 방지). */
	o->events[o->next_buffered_event] = NULL;    /* [한국어] 슬롯 비워 재사용 준비 — 생산자가 이 슬롯에 push 가능. */
	/* [한국어] 소비자 포인터를 원형(mod iodepth)으로 한 칸 전진 — 다음 event()가 다음 슬롯을 읽도록. */
	o->next_buffered_event = (o->next_buffered_event + 1) % td->o.iodepth;

	/* validate our state machine */
	assert(o->buffered_event_count);   /* [한국어] 수확 가능 건수(>0)가 있어야 함 — getevents가 반환한 값과 일치 검증. */
	o->buffered_event_count--;          /* [한국어] 대기 버퍼 카운터 감소(fio 코어가 한 건 가져갔음). */
	assert(io_u);                        /* [한국어] NULL이면 심각한 상태머신 파손 — 즉시 abort. */

	/* assert that fio_libnfs_event is being called in sequential fashion */
	/* [한국어] fio 코어가 0..N-1 순서로 호출했는지 검증 — 첫 호출(event==0)이거나 직전+1 이어야 함. */
	assert(event == 0 || o->prev_requested_event_index + 1 == event);
	if (o->buffered_event_count == 0)
		o->prev_requested_event_index = -1;   /* [한국어] 버퍼 모두 소진 시 인덱스 트래킹 리셋(다음 라운드를 위해). */
	else
		o->prev_requested_event_index = event; /* [한국어] 마지막으로 반환한 인덱스 기록 — 다음 호출의 +1 검증용. */
	return io_u;   /* [한국어] 완료된 io_u를 fio 코어에 반환 — 코어는 이후 put_io_u/통계 처리. */
}

/*
 * fio core logic seems to stop calling this event-loop if we ever return with
 * 0 events
 */
/*
 * [한국어] SHOULD_WAIT - poll(2) timeout 선택 매크로.
 *
 * fio 코어 설계 상 getevents()가 0을 반환하면 이벤트 루프 호출을 중단할 수 있으므로,
 * 아래 두 조건 중 하나라도 참이면 반드시 진전(완료 발생)이 있을 때까지 블로킹한다.
 *   1) outstanding_events == iodepth : 비행 건수가 최대치 — 더 제출 못 함, 수확해야 진행 가능.
 *   2) flush && outstanding_events  : flush(잡 종료/드레인) 요청 중이면서 비행 건이 남음.
 * 그 외에는 timeout=0(논블로킹)으로 poll하여 이미 도착한 이벤트만 회수하고 빠르게 반환.
 */
#define SHOULD_WAIT(td, o, flush)			\
 	((o)->outstanding_events == (td)->o.iodepth ||	\
		(flush && (o)->outstanding_events))

/*
 * [한국어]
 * nfs_event_loop - poll(2) + nfs_service 조합으로 libnfs 상태머신을 진행시키고 완료 수집.
 *
 * @td:    잡 컨텍스트.
 * @flush: true면 모든 비행 I/O가 끝날 때까지 블로킹(cleanup 경로 등에서 사용).
 * @return: fio가 지금 당장 event()로 꺼낼 수 있는 완료 건수(buffered_event_count).
 *
 * libnfs의 비동기 계약:
 *   (a) nfs_get_fd(ctx)로 라이브러리가 관리하는 단일 TCP 소켓 FD를 얻고,
 *   (b) nfs_which_events(ctx)로 현재 상태머신이 관심 있는 POLLIN/POLLOUT 마스크를 얻어
 *   (c) poll(2)로 대기한 뒤,
 *   (d) nfs_service(ctx, revents)로 한 스텝 진행 — 내부적으로 RPC 응답을 파싱하고
 *       등록된 콜백(nfs_callback)을 호출하여 완료를 원형 큐에 푸시한다.
 * 이 루프를 SHOULD_WAIT가 거짓이 될 때까지 반복한다.
 *
 * 실행 컨텍스트: 잡 프로세스(단일 스레드). getevents 콜백의 본체.
 * 에러 경로: poll EINTR/EAGAIN은 재시도; 그 외 poll 실패 또는 nfs_service <0(소켓 복구 불가)
 *            시 루프 탈출하고 지금까지 수집된 buffered_event_count 반환(다음 라운드에서
 *            다시 시도하거나 잡이 중단됨).
 *
 * 호출 체인:
 *   fio_libnfs_getevents / fio_libnfs_cleanup → [nfs_event_loop]
 *     → poll(2) → nfs_service → libnfs RPC 파서 → nfs_callback → events[] push.
 */
static int nfs_event_loop(struct thread_data *td, bool flush)
{
	struct fio_libnfs_options *o = td->eo;   /* [한국어] 잡 전용 옵션/큐 상태. */
	struct pollfd pfds[1]; /* nfs:0 */        /* [한국어] libnfs는 단일 TCP 소켓 FD만 노출하므로 pollfd 1개면 충분. */

	/* we already have stuff queued for fio, no need to waste cpu on poll() */
	/* [한국어] 이미 완료 큐에 건이 있으면 poll 호출 생략 — fast path(이전 루프에서 남겨둔 건 우선 소비). */
	if (o->buffered_event_count)
		return o->buffered_event_count;

	do {
		int timeout = SHOULD_WAIT(td, o, flush) ? -1 : 0;   /* [한국어] 큐 만원/flush 중이면 -1(무한 대기), 아니면 0(즉시 반환). */
		int ret = 0;

		pfds[0].fd = nfs_get_fd(o->context);                 /* [한국어] libnfs 내부 TCP 소켓 FD 취득(매번 조회 — 컨텍스트가 재접속할 수 있음). */
		pfds[0].events = nfs_which_events(o->context);       /* [한국어] 현재 상태머신이 원하는 POLLIN/POLLOUT 마스크 — 요청/응답 단계에 따라 변동. */
		ret = poll(&pfds[0], 1, timeout);                     /* [한국어] 소켓 이벤트 대기 — ret=FD 수 또는 -1(에러)/0(타임아웃). */
		if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;                                      /* [한국어] 시그널 인터럽트/일시적 자원 부족은 재시도 — 치명적 에러 아님. */
			log_err("nfs: failed to poll events: %s\n", strerror(errno));
			break;                                             /* [한국어] 그 외 poll 실패는 회복 불가 — 루프 탈출. */
		}

		ret = nfs_service(o->context, pfds[0].revents);       /* [한국어] libnfs 상태머신 한 스텝 진행 — 내부에서 RPC 파싱 + 완료 콜백 실행.
		                                                         revents는 실제 발생한 이벤트 마스크. */
		if (ret < 0) {
			log_err("nfs: socket is in an unrecoverable error state.\n");
			break;                                             /* [한국어] TCP 소켓 프로토콜 에러 등 복구 불가 — 즉시 종료. */
		}
	} while (SHOULD_WAIT(td, o, flush));                        /* [한국어] 대기 조건이 유지되는 동안 계속 poll+service 반복. */

	return o->buffered_event_count;   /* [한국어] 이번 루프로 누적된 수확 가능 건수 반환 — fio 코어가 event() N회 호출 예정. */
}

/*
 * [한국어]
 * fio_libnfs_getevents - ioengine_ops.getevents 콜백. 비동기 완료 이벤트 수확.
 *
 * @td:  잡 컨텍스트.
 * @min: 최소 완료 대기 수(fio 코어가 요청 — 이 구현은 엄격히 보장하지 않고 SHOULD_WAIT로 결정).
 * @max: 최대 완료 수(이 구현은 무시 — 실제로는 원형 큐 크기로 자연 제한).
 * @t:   최대 대기 시간(이 구현은 무시 — 내부적으로 무한/즉시 분기만 사용).
 * @return: 이번 호출에서 준비된 완료 건수.
 *
 * 이 함수는 nfs_event_loop(flush=false)를 한 번 호출하여 poll+service를 한 차례 진행시킨다.
 * flush=false이므로 드레인 모드가 아니며, 큐 만원이 아니면 논블로킹으로 현재 상태만 반영.
 *
 * 호출 체인: fio 코어(td_io_getevents) → [fio_libnfs_getevents] → nfs_event_loop.
 */
static int fio_libnfs_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	return nfs_event_loop(td, false);   /* [한국어] 비 flush 모드 이벤트 루프 — 현재 가능한 완료만 수집하여 반환. */
}

/*
 * [한국어]
 * nfs_callback - libnfs 완료 콜백. 결과를 io_u에 반영하고 원형 큐에 push.
 *
 * @res:          libnfs 호출 결과. >=0이면 전송된 바이트 수, <0이면 음수 에러 코드(libnfs 규약).
 * @nfs:          libnfs 컨텍스트 — 여기서는 사용하지 않으나 시그니처 상 전달됨.
 * @data:         READ 완료의 경우 libnfs가 할당한 수신 데이터 버퍼 포인터(콜백 반환 후 내부 해제).
 *                WRITE의 경우 NULL 또는 무의미.
 * @private_data: 제출 시 nfs_pread_async/nfs_pwrite_async에 넘긴 io_u 포인터(이 파일의 규약).
 *
 * 이 함수는 nfs_service() 내부에서 RPC 응답을 파싱한 직후 호출된다. 따라서 실행 스레드는
 * nfs_event_loop를 돌리고 있는 잡 스레드이며, 별도 동기화 없이 o->events[] 원형 큐에 푸시 가능.
 *
 * READ 처리:
 *   libnfs v1 API의 nfs_pread_async는 사용자 버퍼를 받지 않고 내부 버퍼를 콜백 data 인자로
 *   전달하므로, 여기서 io_u->buf로 memcpy가 필수다(v2는 이미 사용자 버퍼에 직접 기록).
 *   res==0이면 EOF 도달이며 일반 벤치마크 시나리오에서는 비정상 상황이므로 경고.
 *
 * 에러 처리:
 *   res<0이면 io_u->error에 errno 양수 값 저장 후 res=0으로 보정하여 아래 resid 계산이
 *   음수가 되지 않게 한다. fio 코어가 io_u->error를 보고 잡 상태를 전파.
 *
 * 호출 체인: nfs_service → libnfs 내부 RPC 파서 → [nfs_callback] → events[] push.
 */
static void nfs_callback(int res, struct nfs_context *nfs, void *data,
			 void *private_data)
{
	struct io_u *io_u = private_data;                    /* [한국어] 제출 시 심어둔 io_u 포인터 복원. */
	struct nfs_data *nfs_data = io_u->file->engine_data; /* [한국어] 파일당 상태 — options 역참조에 사용. */
	struct fio_libnfs_options *o = nfs_data->options;    /* [한국어] 잡 전역 옵션/원형 큐 상태 획득. */
	if (res < 0) {
		log_err("Failed NFS operation(code:%d): %s\n", res,
						nfs_get_error(o->context));   /* [한국어] libnfs의 마지막 에러 문자열과 함께 오류 로그. */
		io_u->error = -res;                 /* [한국어] libnfs는 음수 errno 관례 — fio는 양수 errno 관례이므로 부호 반전 저장. */
		/* res is used for read math below, don't want to pass negative
		 * there
		 */
		res = 0;   /* [한국어] 아래 resid = xfer_buflen - res 계산이 음수 들어가지 않도록 0으로 보정. */
	} else if (io_u->ddir == DDIR_READ) {
		/* [한국어] READ 성공 경로 — libnfs v1 API가 내부 버퍼(data)에 응답을 쓰므로 사용자 버퍼로 복사. */
		memcpy(io_u->buf, data, res);
		if (res == 0)
			log_err("Got NFS EOF, this is probably not expected\n");   /* [한국어] 0바이트 읽기는 일반적으로 파일 끝 도달 — 벤치에서는 경고. */
	}
	/* fio uses resid to track remaining data */
	io_u->resid = io_u->xfer_buflen - res;   /* [한국어] 잔여(미완료) 바이트 — 부분 전송 시 fio 코어가 재시도 또는 통계 반영. */

	assert(!o->events[o->free_event_buffer_index]);   /* [한국어] 생산자 슬롯이 비어 있어야 함(소비자가 제때 꺼내갔음을 검증). */
	o->events[o->free_event_buffer_index] = io_u;     /* [한국어] 원형 큐 생산자 슬롯에 io_u push. */
	o->free_event_buffer_index = (o->free_event_buffer_index + 1) % o->queue_depth;   /* [한국어] 생산자 포인터 원형 전진. */
	o->outstanding_events--;                            /* [한국어] 비행 카운터 감소(서버 응답 수신 완료). */
	o->buffered_event_count++;                          /* [한국어] 수확 대기 카운터 증가 — 다음 event() 호출이 가져감. */
}

/*
 * [한국어]
 * queue_write - ddir=DDIR_WRITE 분기 헬퍼. libnfs API 버전별 시그니처 차이를 #ifdef로 분기.
 *
 * @o:    잡 옵션(libnfs 컨텍스트 보유).
 * @io_u: 제출할 I/O 유닛.
 * @return: 0 성공, 음수 실패(libnfs 반환값을 그대로 전달).
 *
 * libnfs는 v1 → v2로 오면서 pread/pwrite_async의 인자 순서가 바뀌었다.
 *   v1: (ctx, fh, off, len, buf, cb, priv)
 *   v2: (ctx, fh, buf, len, off, cb, priv)   ← POSIX pread/pwrite 스타일에 더 가까움.
 * 헤더에서 LIBNFS_API_V2 매크로 노출 여부로 빌드 시 자동 분기.
 *
 * 호출 체인: fio_libnfs_queue → [queue_write] → nfs_pwrite_async → libnfs RPC 전송 큐.
 */
static int queue_write(struct fio_libnfs_options *o, struct io_u *io_u)
{
	struct nfs_data *nfs_data = io_u->engine_data;   /* [한국어] fio_libnfs_queue가 심어둔 파일 상태 — nfsfh 포함. */

#ifdef LIBNFS_API_V2
	/* [한국어] libnfs v2 API 시그니처 — buf/len/off 순서로 POSIX pwrite에 더 가까운 형태.
	 *  nfs_callback, io_u는 완료 시 되돌려 받을 컨텍스트. */
	return nfs_pwrite_async(o->context, nfs_data->nfsfh,
				io_u->buf, io_u->buflen, io_u->offset,
				nfs_callback, io_u);
#else
	/* [한국어] libnfs v1 API 시그니처 — offset/len/buf 순서(레거시). */
	return nfs_pwrite_async(o->context, nfs_data->nfsfh, io_u->offset,
				io_u->buflen, io_u->buf, nfs_callback, io_u);
#endif
}

/*
 * [한국어]
 * queue_read - ddir=DDIR_READ 분기 헬퍼. libnfs API 버전별 시그니처 차이를 #ifdef로 분기.
 *
 * @o:    잡 옵션(libnfs 컨텍스트 보유).
 * @io_u: 제출할 I/O 유닛.
 * @return: 0 성공, 음수 실패.
 *
 * v1과 v2의 차이:
 *   v1: nfs_pread_async(ctx, fh, off, len, cb, priv)  ← buf가 없음!
 *       라이브러리가 내부 버퍼에 응답을 수신한 뒤 콜백의 data 인자로 전달 → nfs_callback에서 memcpy 필수.
 *   v2: nfs_pread_async(ctx, fh, buf, len, off, cb, priv) ← 사용자 버퍼 직접 전달 → 복사 생략.
 *
 * 호출 체인: fio_libnfs_queue → [queue_read] → nfs_pread_async.
 */
static int queue_read(struct fio_libnfs_options *o, struct io_u *io_u)
{
	struct nfs_data *nfs_data = io_u->engine_data;   /* [한국어] 파일별 상태(nfsfh) 획득. */

#ifdef LIBNFS_API_V2
	/* [한국어] v2: 사용자 버퍼 직접 전달 — 완료 콜백에서 memcpy 불필요. */
	return nfs_pread_async(o->context, nfs_data->nfsfh,
				io_u->buf, io_u->buflen, io_u->offset,
				nfs_callback, io_u);
#else
	/* [한국어] v1: 사용자 버퍼를 전달하지 않음 — libnfs가 내부 버퍼에 수신 후 콜백 data로 넘김 → memcpy 필요. */
	return nfs_pread_async(o->context, nfs_data->nfsfh, io_u->offset,
				io_u->buflen, nfs_callback, io_u);
#endif
}

/*
 * [한국어]
 * fio_libnfs_queue - ioengine_ops.queue 콜백. 비동기 NFS read/write 제출.
 *
 * @td:   잡 컨텍스트.
 * @io_u: 제출할 I/O 유닛.
 * @return:
 *   FIO_Q_QUEUED    — 정상 비동기 수락(기본 경로). 추후 getevents로 수확.
 *   FIO_Q_COMPLETED — 에러 발생 시(td->error=1 설정으로 잡 종료 신호).
 *
 * 지원 ddir:
 *   DDIR_WRITE → queue_write (nfs_pwrite_async).
 *   DDIR_READ  → queue_read  (nfs_pread_async).
 *   DDIR_TRIM  → 미지원(NFS 프로토콜에 discard 개념 없음) — 에러 반환.
 *
 * 실행 컨텍스트: 잡 스레드. libnfs 제출은 즉시 반환(소켓 write 버퍼에 enqueue)하므로
 *                실제 전송은 이후 nfs_service 시점에 소켓이 writable일 때 발생.
 *
 * 호출 체인: fio 코어(td_io_queue) → [fio_libnfs_queue] → queue_read|queue_write → nfs_*_async.
 */
static enum fio_q_status fio_libnfs_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct nfs_data *nfs_data = io_u->file->engine_data;   /* [한국어] 파일별 NFS 상태(핸들 + options 역링크). */
	struct fio_libnfs_options *o = nfs_data->options;      /* [한국어] 잡 전역 옵션/원형 큐. */
	struct nfs_context *nfs = o->context;                  /* [한국어] libnfs 컨텍스트(에러 메시지 조회용). */
	enum fio_q_status ret = FIO_Q_QUEUED;                  /* [한국어] 기본 반환값 — 비동기 수락. */
	int err;

	io_u->engine_data = nfs_data;   /* [한국어] queue_read/queue_write가 io_u->engine_data를 통해 nfsfh 접근하도록 심어둠. */
	switch (io_u->ddir) {
	case DDIR_WRITE:
		err = queue_write(o, io_u);   /* [한국어] 쓰기 제출 — nfs_pwrite_async 호출. */
		break;
	case DDIR_READ:
		err = queue_read(o, io_u);    /* [한국어] 읽기 제출 — nfs_pread_async 호출. */
		break;
	case DDIR_TRIM:
		log_err("nfs: trim is not supported");   /* [한국어] NFS 프로토콜에 디스크 discard 연산이 없음 — TRIM 요청은 거부. */
		err = -1;
		break;
	default:
		log_err("nfs: unhandled io %d\n", io_u->ddir);   /* [한국어] 알 수 없는 ddir(SYNC 등) — 미지원 보고. */
		err = -1;
	}
	if (err) {
		log_err("nfs: Failed to queue nfs op: %s\n", nfs_get_error(nfs));   /* [한국어] libnfs 실패 이유 문자열 출력. */
		td->error = 1;   /* [한국어] 잡 중단 플래그 — fio 코어가 다음 루프에서 종료 처리. */
		return FIO_Q_COMPLETED;   /* [한국어] 즉시 완료(에러)로 반환 — 코어가 io_u를 회수. */
	}
	o->outstanding_events++;   /* [한국어] 제출 성공 — 비행 카운터 증가. 완료 시 nfs_callback에서 감소. */
	return ret;                 /* [한국어] FIO_Q_QUEUED 반환 — 비동기 수확 대기. */
}

/*
 * Do a mount if one has not been done before
 */
/*
 * [한국어]
 * do_mount - 지정 URL로 NFS 컨텍스트를 생성·마운트하고 완료 이벤트 원형 큐를 초기화.
 *
 * @td:  잡 컨텍스트.
 * @url: "nfs://server[/export[/path]]" 형식.
 * @return: 0 성공, 음수 실패.
 *
 * 동작 단계:
 *   1) 이미 options->context가 있으면 no-op(다중 fio_file이 같은 URL에 대해 재호출해도 안전).
 *   2) nfs_init_context()로 새 libnfs 컨텍스트 생성(TCP 소켓은 아직 미생성 상태).
 *   3) events[] 원형 큐 calloc(크기 = iodepth).
 *   4) 원형 큐 상태 초기값 설정(prev_requested_event_index=-1).
 *   5) nfs_parse_url_full()로 URL을 server/path/file로 분해.
 *   6) mount 대상 디렉터리 문자열 = path + file(예: "/export/dir" + "subdir") 구성 → nfs_mount 호출.
 *      nfs_mount는 MOUNT RPC(NFSv3) 또는 동등한 NFSv4 PUTROOTFH/LOOKUP 체인을 통해
 *      export의 root file handle을 획득하고 TCP 연결을 유지한다.
 *   7) 임시 문자열과 파싱된 url 객체 해제.
 *
 * 실행 컨텍스트: 잡 스레드의 open_file 경로(fio_libnfs_open에서 호출).
 * 에러 경로: nfs_init_context 실패, mount RPC 실패 시 음수 반환 — 호출자가 잡 실패로 전파.
 *
 * 호출 체인: fio_libnfs_open → [do_mount] → libnfs::nfs_init_context/nfs_parse_url_full/nfs_mount.
 */
static int do_mount(struct thread_data *td, const char *url)
{
	size_t event_size = sizeof(struct io_u **) * td->o.iodepth;   /* [한국어] 원형 큐 바이트 크기 — (io_u* 배열) * iodepth. */
	struct fio_libnfs_options *options = td->eo;                    /* [한국어] 잡 전용 옵션 포인터. */
	struct nfs_url *nfs_url = NULL;                                 /* [한국어] 파싱된 URL 구조체(server/path/file 보유). */
	int ret = 0;
	int path_len = 0;
	char *mnt_dir = NULL;

	if (options->context)
		return 0;   /* [한국어] 이미 초기화됨 — 다중 파일이 동일 URL에 재호출 시 idempotent 처리. */

	options->context = nfs_init_context();   /* [한국어] libnfs 컨텍스트 생성 — 아직 TCP 연결 전. 실패 시 NULL. */
	if (!options->context) {
		log_err("nfs: failed to init nfs context\n");
		return -1;
	}

	options->events = calloc(1, event_size);   /* [한국어] 완료 원형 큐 할당(0으로 초기화 — 모든 슬롯 비어있음). */

	options->prev_requested_event_index = -1;   /* [한국어] 이벤트 순차 검증 카운터 초기화(-1=아직 수확 없음). */
	options->queue_depth = td->o.iodepth;        /* [한국어] nfs_callback 모듈로 연산용 iodepth 캐시. */

	nfs_url = nfs_parse_url_full(options->context, url);   /* [한국어] URL 문자열을 server/path/file 구조로 파싱 — libnfs가 소유권 유지. */
	path_len = strlen(nfs_url->path);                       /* [한국어] path 부분 길이 — 아래 결합 버퍼 크기 계산용. */
	/* [한국어] mount 대상 디렉터리 문자열 = path + file 결합(예: path="/export" + file="dir" → "/exportdir").
	 * 주의: 이 코드는 path와 file 사이에 '/' 삽입 없이 그대로 이어 붙이므로, URL 형식과 libnfs 파싱 규칙에
	 *       맞춰 export 경계가 올바르게 분할되어 있어야 한다. */
	mnt_dir = malloc(path_len + strlen(nfs_url->file) + 1);
	strcpy(mnt_dir, nfs_url->path);                           /* [한국어] path를 버퍼 앞에 복사. */
	strcpy(mnt_dir + strlen(nfs_url->path), nfs_url->file);   /* [한국어] file을 그 뒤에 이어 붙여 최종 경로 완성. */
	ret = nfs_mount(options->context, nfs_url->server, mnt_dir);   /* [한국어] 실제 NFS 마운트 — MOUNT RPC(또는 NFSv4 상당) 수행 후 TCP 연결 유지.
	                                                                 성공 시 0, 실패 시 음수 errno. */
	free(mnt_dir);               /* [한국어] 결합 경로 버퍼 해제(nfs_mount가 내부 복사 수행). */
	nfs_destroy_url(nfs_url);    /* [한국어] 파싱된 URL 객체 해제(libnfs 소유 메모리). */
	return ret;                   /* [한국어] mount 결과 반환. 호출자는 실패 시 컨텍스트 정리 후 잡 중단. */
}

/*
 * [한국어]
 * fio_libnfs_setup - ioengine_ops.setup 콜백. 잡 초기화 직전 1회 호출.
 *
 * @td: 잡 컨텍스트.
 * @return: 0 성공(실패 경로 없음).
 *
 * libnfs는 프로세스 종료 시 내부 스레드 cleanup 훅이 pthread 환경에서 hang을 일으키는
 * 것으로 알려져 있다(라이브러리 자체가 스레드 unsafe하며 fio가 pthread로 잡을 spawn할 때
 * 자원 회수 경로가 꼬인다). 따라서 잡 생성 방식을 fork(프로세스)로 강제한다 — use_thread=0.
 * 성능은 다소 희생되지만 안정성을 우선한다.
 *
 * 실행 컨텍스트: 메인 프로세스에서 잡 스레드 생성 전 한 번.
 * 호출 체인: fio 코어(td_io_init 경로) → [fio_libnfs_setup].
 */
static int fio_libnfs_setup(struct thread_data *td)
{
	/* Using threads with libnfs causes fio to hang on exit, lower
	 * performance
	 */
	td->o.use_thread = 0;   /* [한국어] 잡 실행 단위를 스레드가 아닌 프로세스(fork)로 강제 — libnfs의 스레드 정리 hang 회피. */
	return 0;                /* [한국어] 항상 성공 반환. */
}

/*
 * [한국어]
 * fio_libnfs_cleanup - ioengine_ops.cleanup 콜백. 잡 종료 시 자원 회수.
 *
 * @td: 잡 컨텍스트.
 *
 * 동작:
 *   1) nfs_umount — NFS 언마운트 RPC 전송(서버의 export_stat 카운터 감소 등).
 *   2) nfs_destroy_context — TCP 소켓 닫기, RPC 상태머신 해제, 컨텍스트 메모리 free.
 *   3) free(events) — 완료 원형 큐 배열 해제.
 *
 * 주의: 이 함수는 비행(in-flight) I/O를 드레인하지 않는다 — fio 코어가 이전 단계에서
 *       getevents로 모두 수확했다고 가정한다. 안전을 위해 nfs_event_loop(td, true)를
 *       먼저 호출할 수도 있지만 현 구현은 생략.
 *
 * 실행 컨텍스트: 잡 스레드 종료 직전.
 */
static void fio_libnfs_cleanup(struct thread_data *td)
{
	struct fio_libnfs_options *o = td->eo;   /* [한국어] 잡 전용 옵션 획득. */

	nfs_umount(o->context);            /* [한국어] NFS 언마운트 RPC — 서버에 mount 해제 통지. */
	nfs_destroy_context(o->context);   /* [한국어] libnfs 컨텍스트 내부 소켓/상태머신/버퍼 해제. */
	free(o->events);                    /* [한국어] 완료 원형 큐 해제(do_mount가 calloc한 메모리). */
}

/*
 * [한국어]
 * fio_libnfs_open - ioengine_ops.open_file 콜백. 대상 NFS 파일을 연다.
 *
 * @td: 잡 컨텍스트.
 * @f:  열 파일 정보(f->file_name이 NFS 경로).
 * @return: 0 성공, 그 외 실패(libnfs 에러 코드).
 *
 * 동작:
 *   1) nfs_url 옵션 필수 검증.
 *   2) do_mount 호출 — 최초 호출 시 컨텍스트 생성+마운트, 이후 no-op.
 *   3) nfs_data 구조체 할당 + options 역링크 저장.
 *   4) flags 결정: 쓰기 잡이면 O_CREAT|O_RDWR(필요 시 파일 생성), 아니면 O_RDWR.
 *   5) nfs_open으로 파일 핸들 획득 후 f->engine_data에 부착.
 *
 * 실행 컨텍스트: 잡 스레드.
 * 에러 경로: nfs_url 미지정/마운트 실패/open 실패 — 음수 반환으로 잡 실패 전파.
 *
 * 호출 체인: fio 코어(td_io_open_file) → [fio_libnfs_open] → do_mount → nfs_open.
 */
static int fio_libnfs_open(struct thread_data *td, struct fio_file *f)
{
	struct fio_libnfs_options *options = td->eo;   /* [한국어] 잡 전용 옵션. */
	struct nfs_data *nfs_data = NULL;
	int flags = 0;
	int ret;

	if (!options->nfs_url) {
		log_err("nfs: nfs_url is a required parameter\n");   /* [한국어] 옵션 누락 — 엔진 사용 불가. */
		return -1;
	}

	ret = do_mount(td, options->nfs_url);   /* [한국어] 최초 파일이면 컨텍스트 생성 + 마운트 수행; 재호출이면 no-op. */

	if (ret) {
		log_err("nfs: Failed to mount %s with code %d: %s\n",
			options->nfs_url, ret, nfs_get_error(options->context));   /* [한국어] 마운트 실패 — 서버/네트워크/권한 이슈 가능. */
		return ret;
	}
	nfs_data = calloc(1, sizeof(struct nfs_data));  /* [한국어] 파일별 상태 할당(모든 필드 0). */
	nfs_data->options = options;                      /* [한국어] 잡 옵션 역링크 저장 — nfs_callback이 이 경로로 원형 큐 접근. */

	/* [한국어] 쓰기 계열(WRITE/RANDWRITE 등) 잡이면 파일이 없을 때 생성하도록 O_CREAT, 그 외엔 단순 RDWR. */
	if (td_write(td))
		flags |= O_CREAT | O_RDWR;
	else
		flags |= O_RDWR;

	ret = nfs_open(options->context, f->file_name, flags, &nfs_data->nfsfh);   /* [한국어] NFS OPEN RPC(및 LOOKUP 체인) 수행 — nfsfh 획득. */

	if (ret)
		log_err("Failed to open %s: %s\n", f->file_name,
					nfs_get_error(options->context));   /* [한국어] 파일 열기 실패(권한/경로/서버 상태 등). */
	f->engine_data = nfs_data;   /* [한국어] fio 코어가 관리하는 fio_file에 엔진별 상태 부착 — 이후 queue/close 경로에서 재사용. */
	return ret;                   /* [한국어] nfs_open 결과 그대로 반환 — 0이 아니면 코어가 실패 처리. */
}

/*
 * [한국어]
 * fio_libnfs_close - ioengine_ops.close_file 콜백. 파일 핸들 해제.
 *
 * @td: 잡 컨텍스트.
 * @f:  닫을 파일 정보.
 * @return: 0 성공, 그 외 libnfs 에러 코드.
 *
 * 동작:
 *   1) nfsfh가 NULL이 아니면 nfs_close RPC 발행(libnfs가 내부에서 상태 관리).
 *   2) 파일별 nfs_data 구조체 해제.
 *   3) f->engine_data 포인터 NULL 클리어 — 코어가 다시 열지 않도록 명시.
 *
 * 주의: 잡 전역 컨텍스트(umount/destroy)는 fio_libnfs_cleanup에서 별도 처리.
 *
 * 호출 체인: fio 코어(td_io_close_file) → [fio_libnfs_close] → nfs_close.
 */
static int fio_libnfs_close(struct thread_data *td, struct fio_file *f)
{
	struct nfs_data *nfs_data = f->engine_data;        /* [한국어] fio_libnfs_open이 부착한 파일별 상태. */
	struct fio_libnfs_options *o = nfs_data->options;  /* [한국어] 잡 옵션 — nfs_close에 전달할 컨텍스트 확보. */
	int ret = 0;

	if (nfs_data->nfsfh)
		ret = nfs_close(o->context, nfs_data->nfsfh);   /* [한국어] NFS CLOSE RPC(NFSv4) 또는 내부 정리(NFSv3) 수행. */

	free(nfs_data);            /* [한국어] 파일별 상태 구조체 해제. */
	f->engine_data = NULL;     /* [한국어] 코어가 dangling 포인터 참조하지 않도록 클리어. */
	return ret;                 /* [한국어] nfs_close 결과 전달. */
}

/*
 * [한국어] ioengine_ops 등록 테이블 — fio 코어가 이 구조체 포인터를 레지스트리에 보관하고
 *          load_ioengine("nfs") 시 각 콜백을 디스패치한다.
 */
static struct ioengine_ops ioengine = {
	.name		= "nfs",
	/* [한국어] 엔진 식별자 — jobfile/CLI에서 ioengine=nfs 로 선택.
	 * 설정자: 이 구조체 리터럴. 읽는 자: load_ioengine 이름 매칭. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] 엔진 ABI 버전 — fio 코어와 엔진이 동일한 ioengine_ops 구조 레이아웃을 공유하는지 확인.
	 * 불일치 시 엔진 로드 거부. */

	.setup		= fio_libnfs_setup,
	/* [한국어] 잡 초기화 전 1회 호출 — use_thread=0 강제.
	 * 호출 시점: td_io_init 전.
	 * 반환값 계약: 0 성공, 음수 실패(잡 중단). */

	.queue		= fio_libnfs_queue,
	/* [한국어] 각 io_u 제출 콜백 — 비동기 nfs_pread/pwrite_async 발행.
	 * 호출 시점: io_u 준비 완료 후.
	 * 반환값 계약: FIO_Q_QUEUED(비동기 수락) / FIO_Q_COMPLETED(동기 완료·에러 포함) / FIO_Q_BUSY(큐 만원). */

	.getevents	= fio_libnfs_getevents,
	/* [한국어] 비동기 완료 수집 콜백 — nfs_event_loop로 poll+service 1회 수행.
	 * 호출 시점: queue 반환이 FIO_Q_QUEUED인 경우 완료를 수확하기 위해 코어가 호출.
	 * 반환값 계약: 준비된 완료 건수(0 이상). */

	.event		= fio_libnfs_event,
	/* [한국어] getevents 결과 내 개별 io_u 추출 콜백.
	 * 호출 시점: getevents 반환값 N에 대해 event(0..N-1) 순차 호출.
	 * 반환값 계약: io_u 포인터(완료 처리 완료된 객체). */

	.cleanup	= fio_libnfs_cleanup,
	/* [한국어] 잡 종료 시 자원 해제 — nfs_umount + nfs_destroy_context + events[] free.
	 * 호출 시점: 잡의 모든 I/O 완료 후 정리 단계. */

	.open_file	= fio_libnfs_open,
	/* [한국어] 각 fio_file 열기 콜백 — 최초 시 do_mount, 이후 nfs_open.
	 * 호출 시점: 잡이 타겟 파일을 사용하기 직전(레이지 마운트). */

	.close_file	= fio_libnfs_close,
	/* [한국어] fio_file 닫기 콜백 — nfs_close로 원격 핸들 해제 + nfs_data 메모리 해제.
	 * 호출 시점: open_file과 짝 — 잡 종료 또는 파일 전환 시. */

	.flags		= FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL,
	/* [한국어] 엔진 동작 힌트 비트마스크:
	 *  · FIO_DISKLESSIO — 로컬 블록 디바이스/파일이 필요 없음(원격 NFS 서버가 타겟).
	 *                     fio 코어가 로컬 파일 생성/사이즈 측정 로직을 건너뛰도록 지시.
	 *  · FIO_NOEXTEND   — 잡 실행 중 파일 크기를 확장하지 않음(NFS는 서버가 관리 — 사전 생성 가정).
	 *                     fio 코어가 쓰기 오프셋을 파일 끝을 넘겨 자동 확장하지 않음.
	 *  · FIO_NODISKUTIL — 로컬 디스크 사용률(/proc/diskstats 기반 util%) 통계를 수집하지 않음.
	 *                     원격 네트워크 스토리지라 로컬 디스크 통계가 무의미하므로 비활성. */

	.options	= options,
	/* [한국어] 엔진별 옵션 테이블 포인터 — fio 파서가 이 배열을 순회하며 jobfile/CLI 인식. */

	.option_struct_size	= sizeof(struct fio_libnfs_options),
	/* [한국어] td->eo 버퍼 크기 — fio 코어가 잡마다 이 크기로 calloc하여 옵션/런타임 상태 저장소 마련. */
};

/*
 * [한국어]
 * fio_nfs_register - GCC constructor(fio_init 매크로 = __attribute__((constructor))).
 *
 * 프로세스 로딩 시점(main 진입 전, .init_array 섹션 실행)에 자동 호출되어 nfs 엔진을
 * fio의 엔진 레지스트리에 등록한다. register_ioengine은 내부적으로 engine_list(전역 flist)에
 * 이 ioengine 구조체 포인터를 추가하며, 이후 load_ioengine("nfs") 호출이 이 레지스트리를
 * 조회해 콜백 포인터를 찾는다.
 *
 * 실행 컨텍스트: 메인 프로세스 초기화(다른 엔진 등록자들과 순서는 링커 결정적 — 상호 의존 없음).
 */
static void fio_init fio_nfs_register(void)
{
	register_ioengine(&ioengine);   /* [한국어] engine_list에 이 엔진 항목 삽입. */
}

/*
 * [한국어]
 * fio_nfs_unregister - GCC destructor(fio_exit 매크로 = __attribute__((destructor))).
 *
 * 프로세스 종료 시점(exit/main 리턴 후, .fini_array 섹션)에 자동 호출되어 등록을 해제한다.
 * fio가 동적으로 엔진을 dlclose하지 않더라도 프로세스 정리 단계에서 레지스트리를 비워
 * 메모리 누수/댕글링 참조를 방지한다.
 */
static void fio_exit fio_nfs_unregister(void)
{
	unregister_ioengine(&ioengine);   /* [한국어] engine_list에서 이 엔진 항목 제거. */
}
