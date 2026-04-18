/*
 * [한국어 설명] 유저스페이스 iSCSI initiator 기반 I/O 엔진 (libiscsi.c)
 *
 * === 파일의 역할 ===
 * libiscsi(유저스페이스 iSCSI initiator 라이브러리)를 사용해 원격 iSCSI 타겟/LUN에
 * SCSI READ16/WRITE16/SYNCHRONIZECACHE16 CDB를 직접 발행하여 I/O를 수행하는 fio
 * 엔진 "libiscsi"를 구현한다. 커널의 open-iscsi initiator나 tcm_loop, iSCSI HBA
 * 드라이버에 의존하지 않고 전적으로 유저스페이스에서 TCP 소켓 위에 iSCSI PDU를
 * 조립·해석한다. 비동기 I/O 계약을 사용하며 poll(2) 기반 이벤트 루프로 완료를
 * 수확한다. fio의 각 파일 이름은 iSCSI URL(iscsi://user%password@host[:port]/iqn/lun)
 * 로 해석되어 struct iscsi_lun 하나에 매핑된다. READ CAPACITY(16)로 LUN의 논리 블록
 * 크기와 총 블록 수를 얻어 fio에 파일 크기를 노출한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=libiscsi로 선택되는 플러그인. 실행 흐름은 다음과 같다:
 *   backend.c의 thread_main → td_io_init → fio_iscsi_setup(LUN 배열 할당 및 LUN별
 *   iscsi_create_context + iscsi_full_connect_sync + READ CAPACITY로 block_size/
 *   num_blocks 확보) → fio_iscsi_init(현재 noop) → I/O 루프: io_u.c의 get_io_u →
 *   ioengines.c td_io_queue → fio_iscsi_queue(DDIR_READ/WRITE/SYNC에 따라 CDB 조립 후
 *   iscsi_scsi_command_async로 비동기 제출) → td_io_getevents → fio_iscsi_getevents
 *   (iscsi_which_events로 POLLIN/POLLOUT 마스크 계산 → poll(2) → iscsi_service로 PDU
 *   처리 → iscsi_cb에서 complete_events[] 축적) → fio_iscsi_event로 io_u 반환 →
 *   io_u_sync_complete. 실행 컨텍스트는 fio 잡 스레드 1개(단일 스레드에서 queue/
 *   getevents/event/cleanup 콜백이 직렬화됨). cleanup 단계에서 LUN별로 logout_sync
 *   후 destroy_context.
 *
 * === 타 모듈과의 연결 ===
 * 상단: fio 코어 — backend.c(잡 스레드), ioengines.c(플러그인 디스패치), io_u.c(io_u
 *       생명주기), options.c(option 파싱), optgroup.h(FIO_OPT_G_ISCSI 그룹).
 * 하단: libiscsi (iscsi_create_context / iscsi_destroy_context / iscsi_set_targetname /
 *       iscsi_set_session_type / iscsi_set_header_digest / iscsi_full_connect_sync /
 *       iscsi_logout_sync / iscsi_readcapacity16_sync / iscsi_scsi_command_async /
 *       iscsi_get_fd / iscsi_which_events / iscsi_service), libiscsi/scsi-lowlevel
 *       (scsi_cdb_read16 / scsi_cdb_write16 / scsi_cdb_synchronizecache16 /
 *       scsi_task_add_data_in_buffer / scsi_task_add_data_out_buffer /
 *       scsi_datain_unmarshall / scsi_free_scsi_task), poll(2) 시스콜.
 * 데이터 흐름: io_u->xfer_buf ↔ scsi_task 내부 buffer 목록 ↔ iSCSI Data-In/Data-Out
 *       PDU ↔ TCP/IP ↔ iSCSI target의 블록 디바이스 LBA. READ CAPACITY로 얻은
 *       block_size·num_blocks로 fio의 real_file_size 계산.
 * 공유 상태: struct iscsi_info(td->io_ops_data), iscsi_lun 배열(luns[]), pollfd 배열
 *       (pfds[]), 완료 이벤트 배열(complete_events[]).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_iscsi_setup_lun(): 하나의 iSCSI URL을 파싱해 컨텍스트 생성, 연결, READ
 *   CAPACITY(16)로 용량 확보까지 수행한다.
 * - fio_iscsi_setup(): 모든 파일에 대해 setup_lun을 반복 호출해 luns[]/pfds[]/
 *   complete_events[]를 초기화한다.
 * - fio_iscsi_queue(): DDIR에 따라 READ16/WRITE16/SYNCHRONIZECACHE16 CDB를 만들고
 *   iscsi_scsi_command_async로 비동기 제출(FIO_Q_QUEUED 반환).
 * - fio_iscsi_getevents(): poll(2)로 소켓 이벤트를 기다려 iscsi_service를 돌리고
 *   iscsi_cb가 등록한 complete_events의 누적 개수를 반환.
 * - fio_iscsi_event(): 완료 이벤트 인덱스로부터 io_u를 복원해 fio에 넘긴다.
 * - struct iscsi_task: 한 건의 비동기 SCSI 요청(io_u ↔ scsi_task ↔ LUN 매핑).
 * - struct iscsi_lun: LUN 한 개의 연결 상태와 용량 정보.
 * - struct iscsi_info: 엔진 전역 상태(td->io_ops_data에 저장).
 */

/*
 * libiscsi engine
 *
 * this engine read/write iscsi lun with libiscsi.
 */


#include "../fio.h"          /* [한국어] fio 코어 API: thread_data/io_u/fio_file/ioengine_ops/ddir/log_err 등 */
#include "../optgroup.h"     /* [한국어] FIO_OPT_G_ISCSI 옵션 그룹 매크로 정의 */

#include <stdlib.h>          /* [한국어] malloc/calloc/free — iscsi_info/luns/pfds/tasks 동적 할당 */
#include <iscsi/iscsi.h>     /* [한국어] libiscsi 공개 API: iscsi_context/iscsi_url/connect/service/get_fd */
#include <iscsi/scsi-lowlevel.h> /* [한국어] SCSI CDB 빌더: READ16/WRITE16/SYNCHRONIZECACHE16, scsi_task, datain_unmarshall */
#include <poll.h>            /* [한국어] poll(2) + struct pollfd — libiscsi가 요구하는 이벤트 마스크(POLLIN|POLLOUT) 대기 */

struct iscsi_lun;   /* [한국어] 전방 선언 — iscsi_info가 iscsi_lun**을 보유하고 iscsi_lun이 iscsi_info*를 역참조하는 상호 참조 깨기 */
struct iscsi_info;  /* [한국어] 전방 선언 — 위와 동일한 이유 */

/*
 * [한국어] 개별 iSCSI 비동기 작업 상태 — queue 시점에 malloc되어 scsi_task의 private_data로
 * 묶이고, iscsi_cb에서 complete_events에 축적된 뒤 fio_iscsi_event에서 해제된다.
 */
struct iscsi_task {
	struct scsi_task	*scsi_task;
	/* [한국어] libiscsi가 관리하는 SCSI 작업 핸들(CDB + in/out 버퍼 + 센스 + 상태).
	 * 설정자: fio_iscsi_queue에서 scsi_cdb_read16/write16/synchronizecache16로 생성.
	 * 읽는 자: iscsi_scsi_command_async(제출), 완료 후 fio_iscsi_event가 scsi_free_scsi_task로 해제.
	 * 값 범위: 유효한 libiscsi scsi_task 포인터 — NULL 금지(불투명 구조체, 직접 필드 접근 지양).
	 * 동기화: 단일 잡 스레드만 접근(queue/event가 직렬화). */

	struct iscsi_lun	*iscsi_lun;
	/* [한국어] 이 작업이 대상으로 하는 LUN 컨텍스트 포인터.
	 * 설정자: fio_iscsi_queue에서 io_u->file->engine_data(= fio_iscsi_setup_lun이 저장한 iscsi_lun)로 초기화.
	 * 읽는 자: iscsi_cb가 iscsi_lun->iscsi_info->complete_events에 접근할 때 사용.
	 * 값 범위: fio_iscsi_setup_lun에서 할당된 유효 포인터(NULL 불가).
	 * 동기화: 읽기 전용 역참조이므로 별도 락 불필요. */

	struct io_u		*io_u;
	/* [한국어] 이 iSCSI 요청과 짝이 되는 fio io_u.
	 * 설정자: fio_iscsi_queue에서 초기 할당.
	 * 읽는 자: iscsi_cb에서 io_u->error/resid를 세팅, fio_iscsi_event에서 fio로 반환.
	 * 값 범위: 유효한 io_u 포인터(NULL 불가) — put_io_u 전까지 유효.
	 * 동기화: io_u는 단일 잡 스레드에서만 다뤄지므로 락 불필요. */
};

/*
 * [한국어] iSCSI LUN 한 개(= fio_file 한 개)의 연결 상태와 용량 정보.
 * fio_iscsi_setup_lun에서 생성되고 fio_iscsi_cleanup_lun에서 해제된다.
 */
struct iscsi_lun {
	struct iscsi_info	*iscsi_info;
	/* [한국어] 엔진 전역 상태로 올라가는 역참조 — iscsi_cb에서 complete_events 접근에 필요.
	 * 설정자: fio_iscsi_setup_lun 진입 직후.
	 * 읽는 자: iscsi_cb.
	 * 값 범위: td->io_ops_data가 가리키는 iscsi_info.
	 * 동기화: 단일 잡 스레드. */

	struct iscsi_context	*iscsi;
	/* [한국어] libiscsi의 iSCSI 세션/연결 컨텍스트 — TCP 소켓, CmdSN/StatSN, 세션 파라미터를 내포.
	 * 설정자: iscsi_create_context(initiator).
	 * 읽는 자: iscsi_set_*, iscsi_full_connect_sync, iscsi_scsi_command_async, iscsi_service,
	 *          iscsi_which_events, iscsi_get_fd, iscsi_destroy_context.
	 * 값 범위: 불투명 핸들 — 성공 시 non-NULL, 실패 시 NULL.
	 * 동기화: libiscsi 컨텍스트는 스레드 안전하지 않음. 단일 잡 스레드에서만 조작. */

	struct iscsi_url        *url;
	/* [한국어] iscsi_parse_full_url이 반환한 파싱된 URL(portal/target IQN/lun/user/password).
	 * 설정자: iscsi_parse_full_url(f->file_name).
	 * 읽는 자: iscsi_set_targetname(->target), iscsi_full_connect_sync(->portal, ->lun),
	 *          iscsi_scsi_command_async(->lun), iscsi_readcapacity16_sync(->lun).
	 * 값 범위: libiscsi가 소유/해제하는 구조체.
	 * 동기화: 읽기 전용으로만 사용. */

	int			 block_size;
	/* [한국어] LUN의 논리 블록 크기(바이트). READ CAPACITY(16)가 반환하는 block_length.
	 * 설정자: fio_iscsi_setup_lun에서 scsi_datain_unmarshall 후 rc16->block_length.
	 * 읽는 자: fio_iscsi_queue가 offset/xfer_buflen 정렬 검사 및 LBA 변환에 사용.
	 * 값 범위: 통상 512 또는 4096. 런타임 변경 없음.
	 * 동기화: 초기화 이후 read-only. */

	uint64_t		 num_blocks;
	/* [한국어] LUN의 총 논리 블록 수. READ CAPACITY(16)의 returned_lba + 1.
	 * 설정자: fio_iscsi_setup_lun.
	 * 읽는 자: real_file_size 계산, synchronizecache16 CDB의 길이 파라미터.
	 * 값 범위: 0 초과의 LBA 상한(포함).
	 * 동기화: 초기화 이후 read-only. */
};

/*
 * [한국어] iSCSI 엔진의 잡(thread_data) 단위 전역 상태 — td->io_ops_data에 저장.
 * fio_iscsi_setup에서 할당되고 fio_iscsi_cleanup에서 해제된다.
 */
struct iscsi_info {
	struct iscsi_lun	**luns;
	/* [한국어] LUN 포인터 배열 — 각 fio_file(i)이 luns[i]에 대응.
	 * 설정자: fio_iscsi_setup (calloc) + fio_iscsi_setup_lun(i).
	 * 읽는 자: fio_iscsi_getevents(iscsi_which_events/iscsi_service), fio_iscsi_cleanup.
	 * 값 범위: [0..nr_luns) 인덱스 유효, 실패한 LUN은 NULL일 수 있음.
	 * 동기화: 단일 잡 스레드. */

	int			  nr_luns;
	/* [한국어] luns/pfds 배열의 길이 — 열린 LUN 수(= td->o.nr_files).
	 * 설정자: fio_iscsi_setup.
	 * 읽는 자: getevents/cleanup의 for 루프 경계.
	 * 값 범위: > 0.
	 * 동기화: 초기화 이후 read-only. */

	struct pollfd		 *pfds;
	/* [한국어] poll(2)용 pollfd 배열 — fd는 iscsi_get_fd(), events는 iscsi_which_events()로 세팅.
	 * 설정자: fio_iscsi_setup(fd 고정), fio_iscsi_getevents(events 매 루프 갱신).
	 * 읽는 자: poll(2) / iscsi_service(revents).
	 * 값 범위: luns[]와 1:1 대응.
	 * 동기화: 단일 잡 스레드. */

	struct iscsi_task	**complete_events;
	/* [한국어] iscsi_cb가 완료된 iscsi_task를 축적하는 배열 — fio_iscsi_event가 여기서 꺼냄.
	 * 설정자: iscsi_cb (nr_events 인덱스에 추가).
	 * 읽는 자: fio_iscsi_event(event 인덱스로 조회 후 NULL clear).
	 * 값 범위: 길이 td->o.iodepth, 유효 구간은 [0..nr_events).
	 * 동기화: 단일 잡 스레드(iscsi_cb도 iscsi_service 경로에서 동일 스레드 호출). */

	int			  nr_events;
	/* [한국어] 현재 수확 가능한 완료 이벤트 수 — getevents에서 0으로 리셋되고 iscsi_cb가 증가시킴.
	 * 설정자: fio_iscsi_getevents(reset), iscsi_cb(++).
	 * 읽는 자: getevents 종료 조건, event 인덱스의 상한.
	 * 값 범위: [0..iodepth].
	 * 동기화: 단일 잡 스레드. */
};

/*
 * [한국어] fio --ioengine=libiscsi에서 허용하는 엔진 전용 옵션 컨테이너.
 * td->eo가 이 구조체를 가리키며, FIO_OPT_STR_STORE로 initiator 이름이 채워진다.
 */
struct iscsi_options {
	void	*pad;
	/* [한국어] fio 옵션 파서가 요구하는 패딩 — 실제 옵션 offset 계산을 안정화하기 위한 더미.
	 * 설정자/읽는 자: 없음(파서 내부 ABI 요구).
	 * 값 범위: 무의미.
	 * 동기화: N/A. */

	char	*initiator;
	/* [한국어] iSCSI Initiator Name(IQN 형식) — iscsi_create_context에 전달.
	 * 설정자: 옵션 파서(.def = "iqn.2019-04.org.fio:fio").
	 * 읽는 자: fio_iscsi_setup_lun → iscsi_create_context.
	 * 값 범위: 유효한 IQN 문자열.
	 * 동기화: 초기화 이후 read-only. */
};

/*
 * [한국어] fio 옵션 테이블 — --ioengine=libiscsi 선택 시 등록되는 엔진 전용 옵션들.
 * options.c의 옵션 파서가 이 배열을 훑어 iscsi_options 구조체를 채운다.
 * NULL name 엔트리가 종결자이다.
 */
static struct fio_option options[] = {
	{
		.name	  = "initiator",                              /* [한국어] CLI 옵션명: --initiator */
		.lname	  = "initiator",                              /* [한국어] 긴 이름(출력/도움말용) */
		.type	  = FIO_OPT_STR_STORE,                        /* [한국어] 문자열 복사 저장 옵션 타입 */
		.off1	  = offsetof(struct iscsi_options, initiator),/* [한국어] iscsi_options 내 저장 오프셋 */
		.def	  = "iqn.2019-04.org.fio:fio",                /* [한국어] 기본 IQN — RFC 3720 IQN 형식 */
		.help	  = "initiator name",                         /* [한국어] --cmdhelp 출력 문구 */
		.category = FIO_OPT_C_ENGINE,                         /* [한국어] 엔진 카테고리 */
		.group	  = FIO_OPT_G_ISCSI,                          /* [한국어] iSCSI 옵션 그룹(optgroup.h) */
	},

	{
		.name = NULL,   /* [한국어] 옵션 배열 종결자 */
	},
};

/*
 * [한국어]
 * fio_iscsi_setup_lun - 하나의 fio_file(= iSCSI URL)을 열어 LUN 하나를 준비한다.
 *
 * @iscsi_info: 잡 단위 공유 상태. luns[i]/pfds[i]가 이 함수에서 채워진다.
 * @initiator:  iSCSI Initiator Name(IQN). iscsi_options->initiator.
 * @f:          이번 LUN에 대응하는 fio_file. f->file_name이 iSCSI URL.
 * @i:          luns/pfds 인덱스.
 * @return:     0 성공, 그 외 errno/EINVAL/1 실패. 실패 경로는 iscsi_lun을 해제.
 *
 * URL을 파싱하고 libiscsi 컨텍스트를 만들어 targetname/session-type/header-digest를
 * 설정한 뒤 iscsi_full_connect_sync로 로그인한다. 이후 READ CAPACITY(16)로 블록 크기와
 * 총 블록 수를 받아 fio에 real_file_size로 노출한다. 실패 시 logout/destroy로 정리.
 * 실행 컨텍스트: 잡 스레드의 setup 단계(메인 I/O 루프 이전).
 *
 * 호출 체인: fio_iscsi_setup → [fio_iscsi_setup_lun] → libiscsi(connect/readcapacity16).
 */
static int fio_iscsi_setup_lun(struct iscsi_info *iscsi_info,
			       char *initiator, struct fio_file *f, int i)
{
	struct iscsi_lun		*iscsi_lun  = NULL;  /* [한국어] 이번 파일에 대응할 LUN 컨텍스트 — 실패 시 goto out에서 해제 */
	struct scsi_task		*task	    = NULL;  /* [한국어] READ CAPACITY(16) 결과를 담는 일회용 scsi_task */
	struct scsi_readcapacity16	*rc16	    = NULL;  /* [한국어] task에서 언마샬된 readcapacity16 페이로드 포인터(task 소유 메모리) */
	int				 ret	    = 0;     /* [한국어] 반환 코드 — 0=성공, 실패 시 errno/EINVAL/1 */

	iscsi_lun = calloc(1, sizeof(struct iscsi_lun)); /* [한국어] LUN 상태 0-초기화 할당(실패 처리는 아래 조건에서 포인터 NULL 검사로 간접 처리) */

	iscsi_lun->iscsi_info = iscsi_info;              /* [한국어] 역참조 포인터 저장 — iscsi_cb에서 complete_events 접근에 사용 */

	iscsi_lun->url = iscsi_parse_full_url(NULL, f->file_name); /* [한국어] "iscsi://[user%pw@]host[:port]/iqn/lun" 파싱 */
	if (iscsi_lun->url == NULL) {                     /* [한국어] URL 파싱 실패 → 잘못된 인자 처리 */
		log_err("iscsi: failed to parse url: %s\n", f->file_name); /* [한국어] 진단 메시지 — fio의 전역 로그로 직렬화 */
		ret = EINVAL;                             /* [한국어] 인자 오류로 분류 */
		goto out;                                 /* [한국어] 공통 정리 경로로 이동 */
	}

	iscsi_lun->iscsi = iscsi_create_context(initiator); /* [한국어] libiscsi 세션 컨텍스트 생성(Initiator Name 지정) */
	if (iscsi_lun->iscsi == NULL) {                   /* [한국어] 컨텍스트 생성 실패 */
		log_err("iscsi: failed to create iscsi context.\n");
		ret = 1;                                  /* [한국어] 일반 실패 */
		goto out;
	}

	if (iscsi_set_targetname(iscsi_lun->iscsi, iscsi_lun->url->target)) { /* [한국어] 타겟 IQN 설정 — 세션의 Target Name 파라미터 */
		log_err("iscsi: failed to set target name.\n");
		ret = EINVAL;
		goto out;
	}

	if (iscsi_set_session_type(iscsi_lun->iscsi, ISCSI_SESSION_NORMAL) != 0) { /* [한국어] Normal 세션(= 디스커버리 아닌 I/O 세션) */
		log_err("iscsi: failed to set session type.\n");
		ret = EINVAL;
		goto out;
	}

	if (iscsi_set_header_digest(iscsi_lun->iscsi,
				    ISCSI_HEADER_DIGEST_NONE_CRC32C) != 0) { /* [한국어] 헤더 다이제스트 None 또는 CRC32C 허용 — 타겟과 협상 */
		log_err("iscsi: failed to set header digest.\n");
		ret = EINVAL;
		goto out;
	}

	if (iscsi_full_connect_sync(iscsi_lun->iscsi,
				    iscsi_lun->url->portal,
				    iscsi_lun->url->lun)) {               /* [한국어] TCP 연결 + iSCSI Login PDU 교환 + LUN 바인딩(동기) */
		log_err("iscsi: failed to connect to LUN : %s\n",
			iscsi_get_error(iscsi_lun->iscsi));                  /* [한국어] libiscsi 내부 에러 문자열 추출 */
		ret = EINVAL;
		goto out;
	}

	task = iscsi_readcapacity16_sync(iscsi_lun->iscsi, iscsi_lun->url->lun); /* [한국어] SCSI READ CAPACITY(16) 동기 발행 — 블록 크기/마지막 LBA 획득 */
	if (task == NULL || task->status != SCSI_STATUS_GOOD) {               /* [한국어] 작업 자체 실패 또는 SCSI 응답 상태 비정상 */
		log_err("iscsi: failed to send readcapacity command: %s\n",
			iscsi_get_error(iscsi_lun->iscsi));
		ret = EINVAL;
		goto out;
	}

	rc16 = scsi_datain_unmarshall(task);              /* [한국어] 응답 Data-In 버퍼를 구조체로 역직렬화 */
	if (rc16 == NULL) {                               /* [한국어] 언마샬 실패 */
		log_err("iscsi: failed to unmarshal readcapacity16 data.\n");
		ret = EINVAL;
		goto out;
	}

	iscsi_lun->block_size = rc16->block_length;       /* [한국어] 논리 블록 크기 저장(512/4096 등) */
	iscsi_lun->num_blocks = rc16->returned_lba + 1;   /* [한국어] 마지막 LBA + 1 = 총 블록 수 */

	scsi_free_scsi_task(task);                        /* [한국어] 일회용 readcapacity task 해제 */
	task = NULL;                                      /* [한국어] 중복 해제 방지 */

	f->real_file_size = iscsi_lun->num_blocks * iscsi_lun->block_size; /* [한국어] fio에 노출할 LUN 용량(바이트) */
	f->engine_data	  = iscsi_lun;                    /* [한국어] queue에서 io_u->file->engine_data로 LUN을 즉시 얻기 위함 */

	iscsi_info->luns[i]    = iscsi_lun;               /* [한국어] 전역 LUN 배열에 등록 */
	iscsi_info->pfds[i].fd = iscsi_get_fd(iscsi_lun->iscsi); /* [한국어] poll(2)용 TCP 소켓 fd 고정 등록 */

out:
	if (task) {                                       /* [한국어] 실패 경로에서 task가 남아있으면 해제 */
		scsi_free_scsi_task(task);
	}

	if (ret && iscsi_lun) {                           /* [한국어] 실패했고 LUN이 부분 생성된 경우 역순 정리 */
		if (iscsi_lun->iscsi != NULL) {           /* [한국어] 컨텍스트까지 만들어진 경우 */
			if (iscsi_is_logged_in(iscsi_lun->iscsi)) { /* [한국어] 로그인 상태면 논리 로그아웃 먼저 */
				iscsi_logout_sync(iscsi_lun->iscsi);
			}
			iscsi_destroy_context(iscsi_lun->iscsi); /* [한국어] 컨텍스트 파기 */
		}
		free(iscsi_lun);                          /* [한국어] LUN 구조체 해제 */
	}

	return ret;                                       /* [한국어] 상위(fio_iscsi_setup)로 상태 반환 */
}

/*
 * [한국어]
 * fio_iscsi_setup - 엔진 초기화: iscsi_info 할당 후 LUN 배열을 채운다.
 *
 * @td: 잡 스레드 컨텍스트. td->eo는 iscsi_options, td->o.nr_files는 열 LUN 수.
 * @return: 0 성공, 음수 실패(첫 실패에서 중단).
 *
 * ioengine_ops.setup 계약으로, open_file보다 먼저 호출된다. 이 엔진은 파일마다
 * iSCSI URL 파싱/로그인/READ CAPACITY까지 setup에서 선수행하므로 open_file은 noop.
 * 실행 컨텍스트: 잡 스레드의 초기화 단계.
 *
 * 호출 체인: td_io_init → ops->setup=fio_iscsi_setup → fio_iscsi_setup_lun(×nr_files).
 */
static int fio_iscsi_setup(struct thread_data *td)
{
	struct iscsi_options	*options    = td->eo;       /* [한국어] 엔진 전용 옵션 블록(initiator 포함) */
	struct iscsi_info	*iscsi_info = NULL;         /* [한국어] 잡 전역 엔진 상태 */
	int			 ret	    = 0;            /* [한국어] 반환 코드 */
	struct fio_file		*f;                         /* [한국어] for_each_file 루프 변수 */
	int			 i;                         /* [한국어] 파일 인덱스 */

	iscsi_info	    = malloc(sizeof(struct iscsi_info));                /* [한국어] 엔진 전역 상태 할당(calloc 아님 — 이하 필드 모두 초기화) */
	iscsi_info->nr_luns = td->o.nr_files;                                   /* [한국어] LUN 수 = 잡이 지정한 파일 수 */
	iscsi_info->luns    = calloc(iscsi_info->nr_luns, sizeof(struct iscsi_lun*));   /* [한국어] LUN 포인터 배열 0-초기화 */
	iscsi_info->pfds    = calloc(iscsi_info->nr_luns, sizeof(struct pollfd));       /* [한국어] pollfd 배열 0-초기화 */

	iscsi_info->nr_events	    = 0;                                        /* [한국어] 수확 대기 중 완료 이벤트 수 초기화 */
	iscsi_info->complete_events = calloc(td->o.iodepth, sizeof(struct iscsi_task*));/* [한국어] iodepth만큼 완료 슬롯 준비(최대 inflight) */

	td->io_ops_data = iscsi_info;                                           /* [한국어] 엔진 전역 상태를 td에 바인딩 — 이후 콜백들이 여기서 복원 */

	for_each_file(td, f, i) {                                               /* [한국어] 잡의 모든 fio_file을 순회 — libaio/io_uring와 동일 패턴 */
		ret = fio_iscsi_setup_lun(iscsi_info, options->initiator, f, i); /* [한국어] 각 파일에 대해 연결/용량 확보 */
		if (ret < 0) break;                                             /* [한국어] 첫 실패에서 중단 — 이후 cleanup이 부분 상태를 해제 */
	}

	return ret;                                                             /* [한국어] 상위 td_io_init로 전달 */
}

/*
 * [한국어]
 * fio_iscsi_init - ioengine_ops.init 훅. 이 엔진은 setup에서 모두 끝내므로 noop.
 *
 * @td: 잡 스레드 컨텍스트.
 * @return: 0 성공(항상).
 *
 * 호출 체인: td_io_init → ops->init.
 */
static int fio_iscsi_init(struct thread_data *td) {
	return 0;   /* [한국어] 추가 초기화 불필요 — setup 단계에서 완결 */
}

/*
 * [한국어]
 * fio_iscsi_cleanup_lun - 한 LUN에 대해 로그아웃 후 컨텍스트를 파기한다.
 *
 * @iscsi_lun: 정리할 LUN.
 *
 * 호출 체인: fio_iscsi_cleanup → [fio_iscsi_cleanup_lun] → libiscsi(logout/destroy).
 * 실행 컨텍스트: 잡 스레드 종료 단계.
 */
static void fio_iscsi_cleanup_lun(struct iscsi_lun *iscsi_lun) {
	if (iscsi_lun->iscsi != NULL) {                          /* [한국어] 컨텍스트가 만들어졌다면 */
		if (iscsi_is_logged_in(iscsi_lun->iscsi)) {      /* [한국어] 아직 로그인 상태면 */
			iscsi_logout_sync(iscsi_lun->iscsi);     /* [한국어] iSCSI Logout PDU 교환(동기) */
		}
		iscsi_destroy_context(iscsi_lun->iscsi);         /* [한국어] 컨텍스트/소켓 파기 */
	}
	free(iscsi_lun);                                         /* [한국어] LUN 구조체 해제 */
}

/*
 * [한국어]
 * fio_iscsi_cleanup - 엔진 전역 상태를 해제한다(LUN들 + 배열들 + iscsi_info).
 *
 * @td: 잡 스레드 컨텍스트. td->io_ops_data = iscsi_info.
 *
 * 호출 체인: backend.c → td_io_cleanup → ops->cleanup=fio_iscsi_cleanup.
 */
static void fio_iscsi_cleanup(struct thread_data *td)
{
	struct iscsi_info *iscsi_info = td->io_ops_data;   /* [한국어] setup에서 저장한 전역 상태 복원 */

	for (int i = 0; i < iscsi_info->nr_luns; i++) {     /* [한국어] 모든 LUN 정리 */
		if (iscsi_info->luns[i]) {                  /* [한국어] setup이 실패했으면 NULL일 수 있음 */
			fio_iscsi_cleanup_lun(iscsi_info->luns[i]);
			iscsi_info->luns[i] = NULL;         /* [한국어] 이중 해제 방지 */
		}
	}

	free(iscsi_info->luns);                             /* [한국어] LUN 포인터 배열 해제 */
	free(iscsi_info->pfds);                             /* [한국어] pollfd 배열 해제 */
	free(iscsi_info->complete_events);                  /* [한국어] 완료 슬롯 해제 */
	free(iscsi_info);                                   /* [한국어] 엔진 전역 상태 해제 */
}

/*
 * [한국어]
 * fio_iscsi_prep - ioengine_ops.prep 훅. 이 엔진은 queue에서 CDB를 즉시 만들므로 noop.
 *
 * @td: 잡 스레드 컨텍스트.
 * @io_u: 준비할 I/O 유닛.
 * @return: 0(항상 성공).
 */
static int fio_iscsi_prep(struct thread_data *td, struct io_u *io_u)
{
	return 0;   /* [한국어] 사전 준비 없음 — queue()에서 모든 것을 처리 */
}

/*
 * [한국어]
 * fio_iscsi_open_file - ioengine_ops.open_file 훅. setup에서 이미 연결했으므로 noop.
 *
 * @td: 잡 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @return: 0(항상 성공).
 */
static int fio_iscsi_open_file(struct thread_data *td, struct fio_file *f)
{
	return 0;   /* [한국어] fio_iscsi_setup_lun에서 이미 연결 완료 */
}

/*
 * [한국어]
 * fio_iscsi_close_file - ioengine_ops.close_file 훅. cleanup에서 한꺼번에 파기하므로 noop.
 *
 * @td: 잡 스레드 컨텍스트.
 * @f: 대상 fio_file.
 * @return: 0(항상 성공).
 */
static int fio_iscsi_close_file(struct thread_data *td, struct fio_file *f)
{
	return 0;   /* [한국어] fio_iscsi_cleanup_lun에서 일괄 파기 */
}

/*
 * [한국어]
 * iscsi_cb - libiscsi SCSI 완료 콜백. iscsi_service가 응답 PDU를 처리할 때 호출된다.
 *
 * @iscsi:        libiscsi 컨텍스트(사용하지 않지만 API 시그니처 요구).
 * @status:       SCSI 상태 코드(SCSI_STATUS_GOOD 등).
 * @command_data: libiscsi가 전달하는 명령 데이터(여기선 미사용).
 * @private_data: iscsi_scsi_command_async에 넘긴 iscsi_task 포인터.
 *
 * io_u->error/resid를 기록하고 complete_events[nr_events++]에 iscsi_task를 축적한다.
 * 실행 컨텍스트: fio_iscsi_getevents 내부의 iscsi_service 경로(동일 잡 스레드).
 *
 * 호출 체인: getevents → poll(2) → iscsi_service → [iscsi_cb] → complete_events 적재.
 */
static void iscsi_cb(struct iscsi_context *iscsi, int status,
		     void *command_data, void *private_data)
{
	struct iscsi_task	*iscsi_task = (struct iscsi_task*)private_data; /* [한국어] 제출 시 연결해둔 task 복원 */
	struct iscsi_lun	*iscsi_lun  = iscsi_task->iscsi_lun;            /* [한국어] LUN 역참조 */
	struct iscsi_info       *iscsi_info = iscsi_lun->iscsi_info;            /* [한국어] complete_events 접근을 위한 전역 상태 */
	struct io_u             *io_u	    = iscsi_task->io_u;                 /* [한국어] fio에 돌려줄 I/O 유닛 */

	if (status == SCSI_STATUS_GOOD) {                                       /* [한국어] 정상 완료 */
		io_u->error = 0;                                                /* [한국어] 성공 표시 */
	} else {                                                                /* [한국어] SCSI 에러(CHECK CONDITION 등) */
		log_err("iscsi: request failed with error %s.\n",
			iscsi_get_error(iscsi_lun->iscsi));                     /* [한국어] libiscsi 에러 문자열 기록 */

		io_u->error = 1;                                                /* [한국어] 에러 표시 — io_u_sync_complete가 통계에 반영 */
		io_u->resid = io_u->xfer_buflen;                                /* [한국어] 전송 미완료 바이트 = 전체 — 부분 성공 미가정 */
	}

	iscsi_info->complete_events[iscsi_info->nr_events] = iscsi_task;        /* [한국어] 완료 슬롯에 task 추가 */
	iscsi_info->nr_events++;                                                /* [한국어] 완료 카운터 증가 — getevents 반환값 */
}

/*
 * [한국어]
 * fio_iscsi_queue - ioengine_ops.queue 훅. io_u의 방향(DDIR)에 따라 SCSI CDB를 만들고
 * iscsi_scsi_command_async로 비동기 제출한다.
 *
 * @td:   잡 스레드 컨텍스트.
 * @io_u: 제출할 I/O 유닛(offset/xfer_buflen/xfer_buf/ddir가 설정됨).
 * @return: FIO_Q_QUEUED(성공, 완료는 추후 getevents) 또는 FIO_Q_COMPLETED(즉시 에러 완료).
 *
 * READ/WRITE는 블록 크기 정렬을 검사하고 LBA로 변환해 CDB16을 조립, SYNC는
 * SYNCHRONIZECACHE16 CDB를 사용한다. 제출 실패 시 scsi_task/iscsi_task를 해제하고
 * io_u->error를 세워 FIO_Q_COMPLETED로 즉시 반환해 상위가 실패 처리를 하게 한다.
 * 실행 컨텍스트: 잡 스레드의 I/O 루프.
 *
 * 호출 체인: io_u.c get_io_u → td_io_queue → ops->queue=fio_iscsi_queue → libiscsi async 제출.
 */
static enum fio_q_status fio_iscsi_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	struct iscsi_lun	*iscsi_lun  = io_u->file->engine_data;             /* [한국어] setup에서 f->engine_data에 심어둔 LUN 복원 */
	struct scsi_task	*scsi_task  = NULL;                                /* [한국어] 이번 요청용 CDB 컨테이너 */
	struct iscsi_task	*iscsi_task = malloc(sizeof(struct iscsi_task));   /* [한국어] fio side 메타(완료 콜백에 전달) */
	int			 ret	    = -1;                                  /* [한국어] 반환/에러 코드(기본 실패) */

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {                  /* [한국어] 데이터 I/O만 정렬 검사 */
		if (io_u->offset % iscsi_lun->block_size != 0) {                    /* [한국어] offset이 블록 경계에 정렬돼야 함 — SCSI LBA 단위 */
			log_err("iscsi: offset is not align to block size.\n");
			ret = -1;
			goto out;
		}

		if (io_u->xfer_buflen % iscsi_lun->block_size != 0) {               /* [한국어] 길이도 블록 단위여야 함 */
			log_err("iscsi: buflen is not align to block size.\n");
			ret = -1;
			goto out;
		}
	}

	if (io_u->ddir == DDIR_READ) {                                              /* [한국어] READ16 CDB 조립 */
		scsi_task = scsi_cdb_read16(io_u->offset / iscsi_lun->block_size,   /* [한국어] 시작 LBA = 바이트 오프셋 / 블록 크기 */
					    io_u->xfer_buflen,                      /* [한국어] 전송 바이트 길이(libiscsi가 블록 수로 내부 변환) */
					    iscsi_lun->block_size,                  /* [한국어] 블록 크기 힌트 */
					    0, 0, 0, 0, 0);                         /* [한국어] rdprotect/dpo/fua/fua_nv/group_number 모두 0(기본 읽기) */
		ret = scsi_task_add_data_in_buffer(scsi_task, io_u->xfer_buflen,
						   io_u->xfer_buf);                 /* [한국어] Data-In 수신 버퍼 = io_u의 사용자 버퍼 — zero-copy 수신 */
		if (ret < 0) {
			log_err("iscsi: failed to add data in buffer.\n");
			goto out;
		}
	} else if (io_u->ddir == DDIR_WRITE) {                                      /* [한국어] WRITE16 CDB 조립 */
		scsi_task = scsi_cdb_write16(io_u->offset / iscsi_lun->block_size,
					     io_u->xfer_buflen,
					     iscsi_lun->block_size,
					     0, 0, 0, 0, 0);                        /* [한국어] wrprotect/dpo/fua/fua_nv/group_number 모두 0 */
		ret = scsi_task_add_data_out_buffer(scsi_task, io_u->xfer_buflen,
						    io_u->xfer_buf);                /* [한국어] Data-Out 송신 버퍼 = io_u 사용자 버퍼 */
		if (ret < 0) {
			log_err("iscsi: failed to add data out buffer.\n");
			goto out;
		}
	} else if (ddir_sync(io_u->ddir)) {                                         /* [한국어] DDIR_SYNC/DDIR_DATASYNC/... */
		scsi_task = scsi_cdb_synchronizecache16(
			0, iscsi_lun->num_blocks * iscsi_lun->block_size, 0, 0);    /* [한국어] LUN 전체 플러시(LBA=0, num_blocks=LUN 전체, sync_nv=0, immed=0) */
	} else {                                                                    /* [한국어] TRIM 등 미지원 방향 */
		log_err("iscsi: invalid I/O operation: %d\n", io_u->ddir);
		ret = EINVAL;
		goto out;
	}

	iscsi_task->scsi_task = scsi_task;   /* [한국어] 완료 수확 시 해제할 수 있도록 저장 */
	iscsi_task->iscsi_lun = iscsi_lun;   /* [한국어] 콜백에서 전역 상태로 올라가기 위한 역참조 */
	iscsi_task->io_u      = io_u;        /* [한국어] 완료 시 fio에 돌려줄 io_u */

	ret = iscsi_scsi_command_async(iscsi_lun->iscsi, iscsi_lun->url->lun,
				       scsi_task, iscsi_cb, NULL, iscsi_task);  /* [한국어] 비동기 제출 — 완료는 iscsi_cb에서 수집 */
	if (ret < 0) {
		log_err("iscsi: failed to send scsi command.\n");
		goto out;
	}

	return FIO_Q_QUEUED;                 /* [한국어] 성공적으로 비동기 큐잉 — 완료는 getevents가 수확 */

out:
	if (iscsi_task) {                    /* [한국어] 실패 경로 정리 */
		free(iscsi_task);
	}

	if (scsi_task) {
		scsi_free_scsi_task(scsi_task);
	}

	if (ret) {
		io_u->error = ret;           /* [한국어] 상위가 실패를 인지하도록 에러 코드 기록 */
	}
	return FIO_Q_COMPLETED;              /* [한국어] 즉시 완료(에러) — getevents를 거치지 않음 */
}

/*
 * [한국어]
 * fio_iscsi_getevents - ioengine_ops.getevents 훅. poll(2) + iscsi_service로 최소 min개
 * 완료를 수확한다.
 *
 * @td:  잡 스레드 컨텍스트.
 * @min: 최소 수확 개수.
 * @max: 최대 수확 개수(현 구현은 사용하지 않음 — iscsi_cb가 축적하는 대로 반환).
 * @t:   타임아웃(현 구현은 무한대 poll).
 * @return: 수확한 완료 개수(≥ min) 또는 음수 에러.
 *
 * 모든 LUN에 대해 iscsi_which_events로 관심 이벤트 마스크를 계산하고 poll(2)을
 * 블로킹 호출(-1 = 무한대)한다. 돌아오면 revents가 세팅된 LUN에 대해 iscsi_service를
 * 돌려 PDU 파싱을 진행시키고, 그 과정에서 iscsi_cb가 complete_events에 축적한다.
 * 누적 nr_events가 min 이상이 될 때까지 반복.
 *
 * 호출 체인: backend.c → io_u_queued_complete → td_io_getevents → [fio_iscsi_getevents].
 */
static int fio_iscsi_getevents(struct thread_data *td, unsigned int min,
			       unsigned int max, const struct timespec *t)
{
	struct iscsi_info	*iscsi_info = td->io_ops_data;   /* [한국어] 엔진 전역 상태 */
	int			 ret	    = 0;                 /* [한국어] poll/service 반환값 */

	iscsi_info->nr_events = 0;                               /* [한국어] 이 호출에서 수확한 이벤트만 카운트하도록 리셋 */

	while (iscsi_info->nr_events < min) {                    /* [한국어] 최소 min개 완료될 때까지 */
		for (int i = 0; i < iscsi_info->nr_luns; i++) {  /* [한국어] LUN별 관심 이벤트 마스크 갱신 */
			int events = iscsi_which_events(iscsi_info->luns[i]->iscsi); /* [한국어] libiscsi가 원하는 POLLIN/POLLOUT 조합 */
			iscsi_info->pfds[i].events = events;     /* [한국어] pollfd.events 갱신(fd는 setup에서 고정) */
		}

		ret = poll(iscsi_info->pfds, iscsi_info->nr_luns, -1); /* [한국어] 무한 블록 — 타겟 응답 또는 시그널까지 대기 */
		if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN) { /* [한국어] 시그널/일시적 실패는 재시도 */
				continue;
			}
			log_err("iscsi: failed to poll events: %s.\n",
				strerror(errno));                /* [한국어] 진짜 에러 — 루프 종료 */
			break;
		}

		for (int i = 0; i < iscsi_info->nr_luns; i++) {  /* [한국어] revents가 있는 LUN만 서비스 — libiscsi는 0 revents도 수용 */
			ret = iscsi_service(iscsi_info->luns[i]->iscsi,
					    iscsi_info->pfds[i].revents); /* [한국어] 소켓에서 PDU 읽어 파싱/iscsi_cb 호출 */
			assert(ret >= 0);                        /* [한국어] 서비스 실패 시 개발 중에 즉시 중단 */
		}
	}

	return ret < 0 ? ret : iscsi_info->nr_events;            /* [한국어] poll 에러면 음수 전파, 아니면 수확한 이벤트 수 */
}

/*
 * [한국어]
 * fio_iscsi_event - ioengine_ops.event 훅. getevents가 보고한 index번째 완료에서
 * io_u를 복원하고 관련 자원을 해제한다.
 *
 * @td:    잡 스레드 컨텍스트.
 * @event: [0..nr_events) 범위의 완료 인덱스.
 * @return: 완료된 fio io_u(상위가 io_u_sync_complete/put_io_u로 처리).
 *
 * 호출 체인: td_io_getevents → ops->event=fio_iscsi_event → put_io_u.
 */
static struct io_u *fio_iscsi_event(struct thread_data *td, int event)
{
	struct iscsi_info	*iscsi_info = (struct iscsi_info*)td->io_ops_data; /* [한국어] 전역 상태 */
	struct iscsi_task	*iscsi_task = iscsi_info->complete_events[event];  /* [한국어] iscsi_cb가 적재한 task */
	struct io_u		*io_u	    = iscsi_task->io_u;                    /* [한국어] 반환할 io_u */

	iscsi_info->complete_events[event] = NULL;   /* [한국어] 슬롯 비우기(다음 배치에서 재사용) */

	scsi_free_scsi_task(iscsi_task->scsi_task);  /* [한국어] libiscsi 측 scsi_task 해제 */
	free(iscsi_task);                            /* [한국어] fio 측 메타 해제 */

	return io_u;                                 /* [한국어] 상위로 완료된 io_u 반환 */
}

/*
 * [한국어] libiscsi 엔진의 ioengine_ops 테이블 — backend/ioengines가 이 콜백들로 엔진을 구동.
 *
 * flags 조합:
 *  - FIO_SYNCIO:    queue가 반환 시점에 완료가 확정되는 엔진이라고 주장하지는 않지만, fio가
 *                   이 엔진을 "큐잉형"으로 구분하기 위한 레거시 플래그. (queue는 실제로
 *                   FIO_Q_QUEUED를 반환하며 getevents로 수확한다.)
 *  - FIO_DISKLESSIO: 로컬 디스크/파일이 아닌 네트워크 타겟이므로 디스크 유틸리티 통계 비활성.
 *  - FIO_NODISKUTIL: 디스크 유틸라이제이션 계산 대상 아님.
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name               = "libiscsi",                                    /* [한국어] --ioengine=libiscsi로 선택되는 이름 */
	.version            = FIO_IOOPS_VERSION,                             /* [한국어] ABI 버전 — fio 코어와 매칭 검사 */
	.flags              = FIO_SYNCIO | FIO_DISKLESSIO | FIO_NODISKUTIL,  /* [한국어] 위 주석 참조 */
	.setup              = fio_iscsi_setup,                               /* [한국어] 파일별 iSCSI 로그인 + 용량 획득 */
	.init               = fio_iscsi_init,                                /* [한국어] noop */
	.prep               = fio_iscsi_prep,                                /* [한국어] noop(queue에서 CDB 생성) */
	.queue              = fio_iscsi_queue,                               /* [한국어] READ16/WRITE16/SYNC16 비동기 제출 */
	.getevents          = fio_iscsi_getevents,                           /* [한국어] poll(2) + iscsi_service 루프 */
	.event              = fio_iscsi_event,                               /* [한국어] 완료 슬롯에서 io_u 복원 */
	.cleanup            = fio_iscsi_cleanup,                             /* [한국어] LUN logout/destroy + 배열 해제 */
	.open_file          = fio_iscsi_open_file,                           /* [한국어] noop — setup에서 열림 */
	.close_file         = fio_iscsi_close_file,                          /* [한국어] noop — cleanup에서 닫힘 */
	.option_struct_size = sizeof(struct iscsi_options),                  /* [한국어] 옵션 블록 크기(파서가 할당) */
	.options	    = options,                                       /* [한국어] 옵션 테이블 */
};

/*
 * [한국어]
 * fio_iscsi_register - 공유 라이브러리 로드 시점의 생성자. ioengine 테이블을 fio에 등록.
 * fio_init 매크로는 GCC constructor 속성 — main() 이전에 자동 호출된다.
 */
static void fio_init fio_iscsi_register(void)
{
	register_ioengine(&ioengine);   /* [한국어] ioengines.c의 engine_list에 추가 */
}

/*
 * [한국어]
 * fio_iscsi_unregister - 종료 시점의 소멸자. 엔진 테이블 제거. fio_exit = GCC destructor.
 */
static void fio_exit fio_iscsi_unregister(void)
{
	unregister_ioengine(&ioengine); /* [한국어] engine_list에서 제거 */
}
