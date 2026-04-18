/*
 * [한국어 설명] SCSI Generic (SG) I/O 엔진 구현 (sg.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 I/O 엔진 플러그인 중 하나로, Linux 커널의 SCSI Generic(SG) v3
 * 인터페이스를 사용하여 SCSI 장치(HDD/SSD/테이프/광학장치/광디스크/iSCSI/SAS/FC/USB
 * Mass Storage 등 모든 SCSI Transport 위 디바이스)에 SCSI CDB(Command Descriptor
 * Block)를 사용자 공간에서 직접 발행하는 로우-레벨 I/O 엔진이다.
 *
 * 일반 read()/write() 경로(파일시스템 또는 블록 계층 위)가 아니라 다음 두 경로 중
 * 하나로 SCSI 명령을 직접 발행한다:
 *   (1) 동기 경로 — ioctl(fd, SG_IO, struct sg_io_hdr *hdr): 커널 SG 드라이버가
 *       hdr.cmdp(CDB)와 hdr.dxferp(데이터 버퍼)를 받아 SCSI mid-level → LLD(Low
 *       Level Driver, HBA 드라이버) → HBA 펌웨어로 전달, 완료까지 블로킹.
 *   (2) 비동기 경로 — write(fd, hdr, sizeof(*hdr))로 SG 드라이버 큐에 명령 적재
 *       후 즉시 반환, 추후 poll(fd, POLLIN) → read(fd, hdr, sizeof(*hdr))로
 *       완료 hdr 수신. 이 경로는 /dev/sgY 캐릭터 디바이스에서만 가능하며,
 *       direct=0 + sync=0 인 경우에만 활성화된다.
 *
 * 발행 가능한 SCSI 명령(SBC-3/4 / SPC-4):
 *   - READ(10) opcode 0x28 / READ(16) opcode 0x88 — LBA 32b/64b
 *   - WRITE(10) 0x2A / WRITE(16) 0x8A
 *   - WRITE AND VERIFY(10) 0x2E / (16) 0x8E — 쓰기 후 매체 검증 1패스
 *   - WRITE SAME(10) 0x41 / (16) 0x93 — 동일 1블록을 N개 LBA에 반복 기록
 *   - WRITE SAME(16) + NDOB(No Data-Out Buffer, byte1 bit0) — 0 패턴 채우기 트림 대체
 *   - WRITE STREAM(16) 0x9A — 스트림 ID로 host hint 전달 (SBC-4 streams)
 *   - VERIFY(10) 0x2F / (16) 0x8F — BYTCHK 비트로 매체-only/호스트 비교/반복 비교
 *   - UNMAP 0x42 — 디스카드(TRIM 등가). 여러 LBA range를 parameter list로 묶어 한 번에 발행
 *   - SYNCHRONIZE CACHE(10) 0x35 / (16) 0x91 — 휘발성 캐시 매체로 강제 플러시
 *   - READ CAPACITY(10) 0x25 / (16) 0x9E SA=0x10 — 디바이스 용량/블록 크기 조회
 *   - STREAM CONTROL(16) 0x9E SA=0x34/0x54 — 스트림 ID OPEN/CLOSE
 *
 * 엔진 고유 옵션:
 *   - hipri          : SGV4_FLAG_HIPRI(0x800) — 폴링 기반 완료(io_uring HIPRI 유사)
 *   - readfua        : 모든 READ에 FUA(Force Unit Access, CDB byte1 bit3) 비트 — 캐시 우회 매체 직접 읽기
 *   - writefua       : 모든 WRITE/STREAM에 FUA 비트 — 매체 영속성 보장 후 완료
 *   - sg_write_mode  : write/write_and_verify/write_same/write_same_ndob/verify_bytchk_*/write_stream
 *   - stream_id      : WRITE STREAM(16)에 사용할 스트림 ID(0=자동 할당)
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio는 main()[fio.c] → fio_backend()[backend.c] → 각 잡(thread_data) 스레드를
 * 생성하고, 각 잡은 등록된 ioengine_ops의 콜백(init/prep/queue/commit/getevents/
 * event/cleanup/open_file/close_file/get_file_size/errdetails)을 통해 I/O를 수행한다.
 * 이 파일은 정적 초기화 함수 fio_sgio_register()(fio_init 생성자, ELF .init_array)로
 * 자신의 ioengine_ops("sg") 인스턴스를 전역 ioengine 리스트(register_ioengine() →
 * flist_add_tail)에 등록하고, 언로드 시 fio_sgio_unregister()(fio_exit 소멸자)가
 * 해제한다.
 *
 * 콜백 호출 순서(잡 스레드 한 라이프사이클):
 *   1. ioengine_ops 등록(생성자) → init.c에서 잡 파일 파싱 시 .options/.option_struct_size
 *      참조 → CLI 옵션 파싱
 *   2. backend.c가 td 스레드 시작:
 *        td_io_init() → fio_sgio_init()              [iodepth/nr_files 기반 풀 할당]
 *        td_io_get_file_size() → fio_sgio_get_file_size() [RCAP으로 디바이스 크기 조회]
 *        td_io_open_file()  → fio_sgio_open()          [generic_open_file + type_check + STREAM open]
 *        td_io_prep()        → fio_sgio_prep()         [io_u → CDB 인코딩]
 *        td_io_queue()       → fio_sgio_queue()        [ioctl(SG_IO) 또는 write(2)]
 *        td_io_commit()      → fio_sgio_commit()       [TRIM 누적분 일괄 제출, 비동기 모드]
 *        td_io_getevents()   → fio_sgio_getevents()    [poll(2) + read(2)로 완료 수확]
 *        td_io_event(idx)    → fio_sgio_event()        [수확된 io_u 인덱스→포인터 변환]
 *        td_io_errdetails()  → fio_sgio_errdetails()   [SG_INFO_CHECK 시 sense/host/driver 디코드]
 *        td_io_close_file()  → fio_sgio_close()        [STREAM close + generic_close_file]
 *        td_io_cleanup()     → fio_sgio_cleanup()      [모든 풀 free]
 *
 * 동기 vs 비동기 모드 결정 매트릭스:
 *   /dev/sdX (FIO_TYPE_BLOCK)                    → 항상 ioctl(SG_IO) 동기
 *                                                  type_check가 commit/getevents/event=NULL로 무력화
 *   /dev/sgN (FIO_TYPE_CHAR) + direct=1|sync=1   → write+read 동기(do_sync=1) 경로
 *   /dev/sgN (FIO_TYPE_CHAR) + direct=0 sync=0   → write 비동기 → poll+read getevents
 *
 * 실행 컨텍스트는 호스트 유저스페이스이며, 각 잡 스레드에서 td_io_queue() →
 * queue() → ioctl(SG_IO)/write() 순으로 SCSI 명령이 커널 SG 드라이버 → SCSI mid-layer
 * → blk-mq → HBA LLD → HBA 펌웨어 → 디바이스로 전달된다. 완료는 인터럽트 → softirq →
 * SG 드라이버 큐 → poll() wakeup → read() 회수로 사용자 공간에 반환된다.
 *
 * 플래그 조합:
 *   - FIO_SYNCIO         : td_io_queue()가 queue() 호출 전 issue_time을 기록(SG는 본질적으로
 *                          큐잉이 즉시 발행이며 latency 측정의 기준점이 queue() 직전이어야 정확).
 *   - FIO_RAWIO          : 이 엔진이 원시 디바이스만 다루며 파일시스템 위에서 동작 불가
 *                          (filename=/dev/sdX 또는 /dev/sgN 만 허용).
 *   - FIO_RO_NEEDS_RW_OPEN: 읽기-전용 워크로드라도 /dev/sgN을 RW 모드로 open해야 함
 *                          (write(2) 시스템 호출로 명령을 제출해야 하므로 fd가 RW여야 함).
 *
 * === 타 모듈과의 연결 ===
 * 상위 의존:
 *   - ../fio.h        : thread_data, io_u(offset/xfer_buflen/xfer_buf/ddir/index/file/error/
 *                       resid/issue_time/hdr 등), fio_file(fd/file_name/filetype/real_file_size/
 *                       engine_pos), ioengine_ops, fio_q_status(FIO_Q_COMPLETED/QUEUED/BUSY),
 *                       FIO_SYNCIO/RAWIO/RO_NEEDS_RW_OPEN 플래그, ddir_sync, td_verror,
 *                       dprint(FD_IO/FD_FILE/FD_ZBD), io_u_mark_submit/complete, io_u_queued,
 *                       io_u_sync_complete, fio_ro_check, fio_fill_issue_time, fio_gettime,
 *                       fio_set_fd_nonblocking, generic_open_file/generic_close_file,
 *                       fio_file_size_known/set_size_known, register_ioengine/unregister_ioengine,
 *                       fio_init/fio_exit(GCC constructor/destructor 매크로), be16/32/64_to_cpu/
 *                       cpu_to_be*, strlcat, log_err.
 *   - ../optgroup.h   : FIO_OPT_C_ENGINE/FIO_OPT_G_SG (옵션 카테고리/그룹 상수).
 *   - <scsi/sg.h>     : 커널 UAPI — struct sg_io_hdr, SG_IO ioctl 매크로, SG_INFO_CHECK,
 *                       SG_DXFER_FROM_DEV/TO_DEV/NONE, SG_GET_VERSION_NUM. 본 파일에서
 *                       명시적으로 #include 하지 않지만 fio.h가 OS 헤더 체인을 통해 포함.
 *   - <linux/fs.h>    : BLKSSZGET (블록 디바이스 논리 섹터 크기 조회 ioctl).
 *   - <poll.h>/<unistd.h>/<errno.h>/<stdlib.h>/<stdio.h> : 표준 POSIX/C.
 *
 * 하위 의존: 없음(이 엔진은 플러그인 말단). fio 공통 유틸을 호출만 한다.
 *
 * 데이터 흐름:
 *   io_u (fio 논리 I/O 단위) ──prep──▶ io_u->hdr(sg_io_hdr) 구성
 *      offset/buflen/buf/ddir         hdr.cmdp = sgio_cmd.cdb (16B)
 *                                     hdr.sbp  = sgio_cmd.sb  (64B sense)
 *                                     hdr.usr_ptr = io_u   ◀ 완료 식별 핵심 (커널이 그대로 echo back)
 *   ──queue──▶ ioctl(SG_IO) 또는 write(/dev/sgN, hdr) ──▶ 커널 SG ──▶ blk-mq ──▶ HBA
 *   ──완료(인터럽트) ──▶ SG 드라이버 큐 ──poll/read──▶ getevents()의 sd->sgbuf
 *   ──hdr.usr_ptr 역참조 ──▶ events[] ──event(idx)──▶ td_io_event ──▶ fio 통계 누적
 *
 * TRIM(UNMAP)은 다대일 관계 — 여러 io_u의 (LBA, len) 쌍을 하나의 UNMAP parameter list로
 * 묶어 한 번의 SCSI 명령으로 발행한다. 비동기 모드에서는 prep()/queue()가 누적만 하고
 * commit()이 일괄 제출하며, 완료 수확 시 대표 io_u의 완료가 전체 누적 io_u의 완료를
 * 의미한다(getevents()에서 unmap_range_count만큼 events[]를 확장).
 *
 * 공유 상태: thread_data(td) 단위로 생성된 struct sgio_data가 td->io_ops_data에
 *   보관되어 모든 콜백에서 공유된다. 잡 스레드는 단일이므로 별도 락 불필요.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수(엔진 콜백):
 *   - fio_sgio_init()         : iodepth × sizeof(sgio_cmd/sg_io_hdr) 풀, nr_files × pollfd
 *                               할당. trim_queues[] 슬롯 iodepth개 초기화. type_checked=0.
 *   - fio_sgio_get_file_size(): RCAP(10/16)으로 blksz/max_lba 조회 → real_file_size 설정.
 *   - fio_sgio_open()         : generic_open_file + (최초 1회) type_check + WRITE STREAM 세팅.
 *   - fio_sgio_close()        : 자동 할당된 STREAM 해제 + generic_close_file.
 *   - fio_sgio_prep()         : ddir별 CDB 인코딩(READ/WRITE/UNMAP/SYNC + 모드 분기).
 *   - fio_sgio_queue()        : 동기/비동기/TRIM 분기 후 fio_sgio_doio()로 위임.
 *   - fio_sgio_commit()       : 비동기 TRIM 누적분을 한 UNMAP으로 일괄 발행.
 *   - fio_sgio_getevents()    : poll() + read()로 sg_io_hdr 회수, TRIM 완료 펼침.
 *   - fio_sgio_event()        : sd->events[idx] → io_u 반환.
 *   - fio_sgio_errdetails()   : host_status/driver_status/SCSI status/sense/CDB 디코드 문자열.
 *   - fio_sgio_cleanup()      : 모든 풀 free.
 *
 * 함수(내부 헬퍼):
 *   - sgio_get_be16/32/64, sgio_set_be16/32/64 : BE 직렬화 유틸.
 *   - sgio_unbuffered()        : O_DIRECT/sync_io 동기 모드 판정.
 *   - sgio_hdr_init()          : sg_io_hdr 공통 필드 채우기(interface_id='S', cmdp/sbp/usr_ptr).
 *   - pollin_events()/sg_fd_read(): poll/read 보조.
 *   - fio_sgio_ioctl_doio()/rw_doio()/doio(): 동기/비동기 발행 경로.
 *   - fio_sgio_rw_lba()         : LBA 크기에 따라 10B/16B CDB 레이아웃 인코딩.
 *   - fio_sgio_unmap_setup()    : UNMAP parameter list 헤더 길이 필드 확정.
 *   - fio_sgio_read_capacity()  : RCAP(10) → 32b 포화 시 RCAP(16) 폴백.
 *   - fio_sgio_type_check()     : BLOCK이면 BLKSSZGET, CHAR이면 SG_GET_VERSION_NUM + RCAP.
 *   - fio_sgio_stream_control() : OPEN/CLOSE STREAM(16) 발행, 스트림 ID 자동 할당/해제.
 *
 * 구조체:
 *   - struct sgio_cmd  : io_u당 CDB(16B)와 sense buffer(MAX_SB=64B) 저장소.
 *   - struct sgio_trim : 한 UNMAP에 묶인 TRIM io_u 집합과 parameter list 버퍼.
 *   - struct sgio_data : 잡 td 단위 상태(cmds/events/pfds/fd_flags/sgbuf/bs/trim_queues 등).
 *   - struct sg_options: 엔진 옵션(hipri/readfua/writefua/write_mode/stream_id).
 *
 * === SG v3 sg_io_hdr 핵심 필드 핸드북 ===
 * struct sg_io_hdr (커널 UAPI, scsi/sg.h):
 *   .interface_id   : 항상 'S' (SG v3 magic).
 *   .dxfer_direction: SG_DXFER_FROM_DEV(-3) / TO_DEV(-2) / NONE(-1) / TO_FROM_DEV(-4).
 *   .cmd_len        : CDB 길이(보통 6/10/12/16). UNMAP=10, READ/WRITE(10/16)=10/16, RCAP(10)=10/(16)=16.
 *   .mx_sb_len      : sense buffer 최대 수신 크기.
 *   .iovec_count    : 0=단일 버퍼(dxferp 사용), >0=scatter/gather.
 *   .dxfer_len      : 데이터 전송 바이트(CDB 자체와 별개).
 *   .dxferp         : 데이터 버퍼 포인터(또는 iovec 배열).
 *   .cmdp           : CDB 버퍼 포인터.
 *   .sbp            : sense data 수신 버퍼.
 *   .timeout        : ms 단위 타임아웃(이 엔진은 SCSI_TIMEOUT_MS=30000 고정).
 *   .flags          : SG_FLAG_DIRECT_IO(0x1=DMA 직접/페이지 핀), SGV4_FLAG_HIPRI(0x800=폴링).
 *   .pack_id        : 사용자 태그(이 엔진은 io_u->index 사용, 디버깅용).
 *   .usr_ptr        : 사용자 포인터(이 엔진은 io_u 주소 — 완료 hdr에서 io_u 복원의 핵심).
 *   .status         : SCSI status byte(0=GOOD, 0x02=CHECK CONDITION, 0x18=RESERVATION CONFLICT 등).
 *   .masked_status  : status >> 1 후 마스킹.
 *   .msg_status/sb_len_wr/host_status/driver_status : 각 계층 상태.
 *   .resid          : 미전송 바이트(부분 완료).
 *   .duration       : 명령 소요 ms.
 *   .info           : SG_INFO_CHECK(0x1=에러 상세 있음) / SG_INFO_DIRECT_IO_MASK 등.
 *
 * === SCSI sense data / 에러 처리 ===
 * SCSI 명령이 실패하면 디바이스가 sense data(최대 252B, 보통 18B fixed format 또는
 * descriptor format)를 응답한다. SG 드라이버는 이를 hdr.sbp에 복사하고 hdr.info에
 * SG_INFO_CHECK 비트를 세팅한다. sense data의 핵심 필드:
 *   - sense key (4 bits): NO_SENSE/RECOVERED/NOT_READY/MEDIUM_ERROR/HARDWARE_ERROR/
 *                          ILLEGAL_REQUEST/UNIT_ATTENTION/DATA_PROTECT/BLANK_CHECK/
 *                          ABORTED_COMMAND/MISCOMPARE 등.
 *   - ASC/ASCQ (Additional Sense Code/Qualifier): 에러의 구체 원인.
 *     예) 0x21/0x00 = LBA OUT OF RANGE, 0x27/0x00 = WRITE PROTECTED,
 *         0x29/0x00 = POWER ON RESET, 0x44/0x00 = INTERNAL TARGET FAILURE.
 * 본 엔진의 fio_sgio_errdetails()는 sense 바이트를 헥스 덤프하고 host/driver/SCSI
 * 상태 코드를 사람이 읽는 문자열로 변환한다(fio --debug=io 또는 verify 실패 시 출력).
 *
 * === ZBD/ZAC 통합 (참고) ===
 * SG 엔진 자체는 ZBD 콜백(get_zoned_model/report_zones/reset_wp/finish_zone/
 * get_max_open_zones/move_zone_wp)을 직접 구현하지 않는다 — 이는 libzbc 엔진의 역할이며,
 * libzbc가 내부적으로 같은 SG_IO ioctl을 통해 REPORT ZONES(0x95)/RESET WRITE POINTER
 * (0x94 SA=0x04)/CLOSE ZONE(SA=0x01)/FINISH ZONE(SA=0x02)/OPEN ZONE(SA=0x03) CDB를
 * 발행한다. 이 파일은 표준 R/W/TRIM/SYNC만 처리하고, 존 디바이스에서 사용하려면
 * libzbc 엔진을 사용해야 한다.
 *
 * === cmdprio 통합 (참고) ===
 * fio의 cmdprio 유틸(engines/cmdprio.c)은 libaio/io_uring 등에서 io_u별 ioprio를
 * IOPRIO_CLASS_RT/BE/IDLE로 분기 설정한다. SG 엔진은 현재 cmdprio_options를 등록하지
 * 않으며(opts[] 테이블에 CMDPRIO_OPTIONS 매크로 미사용), io_u별 우선순위 분기 기능이
 * 없다. SCSI 자체는 task attribute(SIMPLE/HEAD OF QUEUE/ORDERED/ACA)로 우선순위
 * 표현이 가능하지만 본 파일은 그 기능을 노출하지 않는다.
 *
 * === streamid (SBC-4 streams) ===
 * WRITE STREAM(16) opcode 0x9A는 host가 데이터의 "스트림 ID"를 디바이스에 전달해
 * 같은 스트림 데이터를 같은 NAND 블록(SSD) 또는 같은 zone(ZNS)에 묶도록 힌트한다.
 * 이는 GC(Garbage Collection) 효율을 높이고 WAF(Write Amplification Factor)를 낮춘다.
 * 사용 흐름:
 *   1. open_file() — STREAM CONTROL(16) SA=0x34(OPEN STREAM)으로 스트림 ID 자동 할당
 *      (stream_id=0 옵션) 또는 사용자가 명시한 ID 사용.
 *   2. prep() — WRITE STREAM(16) CDB[10..11]에 BE16 스트림 ID 인코딩.
 *   3. close_file() — STREAM CONTROL(16) SA=0x54(CLOSE STREAM)으로 자동 할당된 ID 해제.
 */

/*
 * sg engine
 *
 * IO engine that uses the Linux SG v3 interface to talk to SCSI devices
 *
 * This ioengine can operate in two modes:
 *	sync	with block devices (/dev/sdX) or
 *		with character devices (/dev/sgY) with direct=1 or sync=1
 *	async	with character devices with direct=0 and sync=0
 *
 * What value does queue() return for the different cases?
 *				queue() return value
 * In sync mode:
 *  /dev/sdX		RWT	FIO_Q_COMPLETED
 *  /dev/sgY		RWT	FIO_Q_COMPLETED
 *   with direct=1 or sync=1
 *
 * In async mode:
 *  /dev/sgY		RWT	FIO_Q_QUEUED
 *   direct=0 and sync=0
 *
 * Because FIO_SYNCIO is set for this ioengine td_io_queue() will fill in
 * issue_time *before* each IO is sent to queue()
 *
 * Where are the IO counting functions called for the different cases?
 *
 * In sync mode:
 *  /dev/sdX (commit==NULL)
 *   RWT
 *    io_u_mark_depth()			called in td_io_queue()
 *    io_u_mark_submit/complete()	called in td_io_queue()
 *    issue_time			set in td_io_queue()
 *
 *  /dev/sgY with direct=1 or sync=1 (commit does nothing)
 *   RWT
 *    io_u_mark_depth()			called in td_io_queue()
 *    io_u_mark_submit/complete()	called in queue()
 *    issue_time			set in td_io_queue()
 *
 * In async mode:
 *  /dev/sgY with direct=0 and sync=0
 *   RW: read and write operations are submitted in queue()
 *    io_u_mark_depth()			called in td_io_commit()
 *    io_u_mark_submit()		called in queue()
 *    issue_time			set in td_io_queue()
 *   T: trim operations are queued in queue() and submitted in commit()
 *    io_u_mark_depth()			called in td_io_commit()
 *    io_u_mark_submit()		called in commit()
 *    issue_time			set in commit()
 *
 */
#include <stdio.h>
/* [한국어] 표준 I/O 라이브러리 — snprintf(에러 메시지 포맷, fio_sgio_errdetails의
 * msgchunk 작성)에 사용. printf 계열은 이 파일에서 직접 호출하지 않으나 fio.h가
 * 내부적으로 의존하므로 명시 포함 관행. */

#include <stdlib.h>
/* [한국어] C 표준 — calloc(sgio_data/sgio_cmd 배열/sgio_trim/unmap_param/trim_io_us/
 * pollfd/fd_flags/sgbuf/events 등 모든 풀 할당, errdetails msg 1024B 할당), free
 * (cleanup에서 역순 해제). NULL/size_t 정의도 공급. */

#include <unistd.h>
/* [한국어] POSIX — read(2)/write(2)(/dev/sgN 비동기 경로의 명령 제출과 완료 회수),
 * close(2)(read_capacity의 독립 fd 닫기), usleep(2)(getevents 재시도 1ms 백오프).
 * fcntl 매크로 F_SETFL은 fio_set_fd_nonblocking 내부에서 사용. */

#include <errno.h>
/* [한국어] errno 전역 변수 + 오류 코드 매크로 공급:
 *   - EAGAIN  : sg_fd_read에서 비차단 모드의 일시적 데이터 부족 → 재시도
 *   - EINTR   : 시그널에 의한 시스템 호출 중단 → 재시도
 *   - EIO     : SG_INFO_CHECK 발생 또는 RCAP 응답 비정상 → io_u->error 표시
 *   - EINVAL  : prep에서 블록 크기 미정렬, type_check에서 지원하지 않는 파일 타입
 *   - ENOMEM  : (간접) calloc 실패 시 (단, 이 파일은 NULL 체크 없음 — fio 관행) */

#include <poll.h>
/* [한국어] poll(2) 시스템 호출 + struct pollfd + POLLIN/POLLERR 매크로.
 * fio_sgio_getevents()가 무한 timeout(-1)으로 호출하여 /dev/sgN의 완료 데이터가
 * 도착할 때까지 대기. min==0(논블로킹) 경로에서는 poll을 스킵하고 직접 read 시도. */

#include "../fio.h"
/* [한국어] fio 코어 헤더 — 본 엔진이 사용하는 거의 모든 심볼을 간접 공급:
 *   타입: struct thread_data(td), struct io_u, struct fio_file, struct ioengine_ops,
 *         enum fio_q_status (FIO_Q_COMPLETED/QUEUED/BUSY), enum fio_ddir (DDIR_READ/
 *         WRITE/TRIM/SYNC), fio_unused 매크로, OS_O_DIRECT 등.
 *   상수: FIO_SYNCIO, FIO_RAWIO, FIO_RO_NEEDS_RW_OPEN(ioengine_ops.flags 비트),
 *         FIO_TYPE_BLOCK, FIO_TYPE_CHAR, FIO_IOOPS_VERSION (엔진 ABI 버전).
 *   유틸: io_u_mark_submit/complete, io_u_queued, io_u_sync_complete, fio_ro_check,
 *         fio_fill_issue_time, fio_gettime, fio_set_fd_nonblocking, generic_open_file/
 *         generic_close_file, fio_file_size_known/set_size_known, register_ioengine/
 *         unregister_ioengine, fio_init/fio_exit (GCC constructor/destructor 매크로),
 *         td_verror, dprint, log_err, strlcat, ddir_sync, clear_io_u.
 *   엔디언: be16_to_cpu/be32_to_cpu/be64_to_cpu, cpu_to_be16/32/64.
 *   OS UAPI 체인: scsi/sg.h(struct sg_io_hdr, SG_IO, SG_INFO_CHECK, SG_DXFER_*,
 *         SG_GET_VERSION_NUM, SGV4_FLAG_HIPRI), linux/fs.h(BLKSSZGET). */

#include "../optgroup.h"
/* [한국어] 옵션 카테고리/그룹 상수:
 *   - FIO_OPT_C_ENGINE: 옵션 카테고리 = "엔진"
 *   - FIO_OPT_G_SG    : 옵션 그룹 = "SG 엔진"
 * options[] 테이블의 각 엔트리에 .category/.group으로 부여되어 --help 출력과
 * jobspec 검증에서 옵션이 어느 엔진/카테고리에 속하는지 분류된다. */

#ifdef FIO_HAVE_SGIO
/* [한국어] configure가 Linux scsi/sg.h 존재를 확인한 경우에만 실제 엔진을 빌드한다.
 * 그렇지 않으면 파일 하단의 #else 블록에서 "사용 불가" 스텁만 등록한다. */

#ifndef SGV4_FLAG_HIPRI
#define SGV4_FLAG_HIPRI 0x800
/* [한국어] 구버전 커널 헤더에는 HIPRI(폴링 기반 완료) 플래그 매크로가 없을 수 있으므로
 * fallback 정의. SG v4 인터페이스에서 고성능 폴링 경로를 요청하는 비트이다. */
#endif

/* [한국어] SCSI WRITE 계열 명령의 모드 선택용 enum.
 * sg_options.write_mode에 저장되며, fio_sgio_prep()에서 CDB opcode를 분기 결정한다. */
enum {
	FIO_SG_WRITE		= 1, /* [한국어] 표준 WRITE(10/16) */
	FIO_SG_WRITE_VERIFY,     /* [한국어] WRITE AND VERIFY(10/16) — 쓰기 후 매체 검증 */
	FIO_SG_WRITE_SAME,       /* [한국어] WRITE SAME — 동일 블록을 여러 LBA에 반복 기록 */
	FIO_SG_WRITE_SAME_NDOB,  /* [한국어] WRITE SAME(16) + NDOB(No Data-Out Buffer) — 0으로 채우기 */
	FIO_SG_WRITE_STREAM,     /* [한국어] WRITE STREAM(16) — 스트림 ID 기반 기록 */
	FIO_SG_VERIFY_BYTCHK_00, /* [한국어] VERIFY + BYTCHK=00 — 매체 검증만 */
	FIO_SG_VERIFY_BYTCHK_01, /* [한국어] VERIFY + BYTCHK=01 — 호스트 데이터와 비교(논리 블록 단위) */
	FIO_SG_VERIFY_BYTCHK_11, /* [한국어] VERIFY + BYTCHK=11 — 호스트 데이터 반복 비교 */
};

/* [한국어] SG 엔진 전용 옵션 구조체.
 * fio 옵션 파서가 명령줄/잡 파일의 옵션을 이 구조체에 채우고, td->eo로 접근 가능.
 * option_struct_size=sizeof(struct sg_options)로 ioengine_ops에 등록된다. */
struct sg_options {
	void *pad;
	/* [한국어] fio 옵션 구조체 정렬용 패딩.
	 * 설정자: fio 공통 옵션 파서가 레이아웃 호환성 유지를 위해 선두에 요구.
	 * 읽는 자: 사용되지 않음(placeholder).
	 * 값 범위: 항상 NULL 혹은 무시. 동기화 불필요. */

	unsigned int hipri;
	/* [한국어] HIPRI(폴링 기반 완료) 사용 여부.
	 * 설정자: --hipri / sg_options.hipri 옵션 파싱 시 1로 설정.
	 * 읽는 자: fio_sgio_prep()에서 READ/WRITE CDB 구성 시 hdr->flags에 SGV4_FLAG_HIPRI 추가.
	 * 값 범위: 0(비활성) / 1(활성). 동기화: td 스레드 로컬, 락 불필요. */

	unsigned int readfua;
	/* [한국어] READ CDB에 FUA(Force Unit Access) 비트를 설정할지 여부.
	 * 설정자: readfua 옵션(0/1).
	 * 읽는 자: fio_sgio_prep()의 READ 분기에서 cmdp[1] |= 0x08.
	 * 값 범위: 0/1. 설정 시 디바이스 캐시를 우회하여 매체로부터 직접 읽음.
	 * 동기화: td 로컬, 락 불필요. */

	unsigned int writefua;
	/* [한국어] WRITE CDB에 FUA 비트를 설정할지 여부 — 캐시 우회 직접 기록.
	 * 설정자: writefua 옵션(0/1).
	 * 읽는 자: fio_sgio_prep()의 WRITE/WRITE_STREAM 분기에서 cmdp[1] |= 0x08.
	 * 값 범위: 0/1. 동기화: td 로컬. */

	unsigned int write_mode;
	/* [한국어] WRITE 모드 선택(FIO_SG_WRITE, FIO_SG_WRITE_VERIFY, ... 중 하나).
	 * 설정자: sg_write_mode 옵션 파싱 시 posval 테이블에서 매핑.
	 * 읽는 자: fio_sgio_prep()의 DDIR_WRITE switch, fio_sgio_open/close 스트림 제어.
	 * 값 범위: FIO_SG_* enum 값. 동기화: td 로컬. */

	uint16_t stream_id;
	/* [한국어] WRITE STREAM(16) 명령에 사용할 스트림 ID.
	 * 설정자: stream_id 옵션(정수).
	 * 읽는 자: fio_sgio_open()/close()에서 명시적 스트림 사용 또는 자동 할당 결정.
	 * 값 범위: 0=자동 오픈(STREAM_CONTROL로 할당), >0=명시적 스트림. 동기화: td 로컬. */
};

/* [한국어] === fio 옵션 테이블 (sg 엔진 전용) ===
 * engine 초기화 시 fio 옵션 파서(options.c의 parse_options)에 .options로 전달되어
 * CLI(--hipri/--readfua=1 등)와 잡 파일(hipri=1/readfua=1)에서 해당 키워드를
 * sg_options 구조체의 대응 필드로 파싱하도록 한다.
 *
 * 각 엔트리 공통 규약(fio_option 스키마):
 *   - .name       : 짧은 키 이름(CLI/잡 파일에서 사용)
 *   - .lname      : 긴 설명 이름(--help 상세 표시)
 *   - .type       : FIO_OPT_STR_SET(값 없이 1로 토글) / FIO_OPT_BOOL / FIO_OPT_INT /
 *                   FIO_OPT_STR(posval 배열로 열거형)
 *   - .off1       : 대상 필드의 구조체 내 오프셋(offsetof) — 파서가 td->eo + off1에 기록
 *   - .help       : --help 한 줄 설명
 *   - .def        : 기본값(문자열) — 파서가 타입에 맞게 해석
 *   - .posval     : FIO_OPT_STR일 때 허용되는 문자열↔enum 매핑 테이블(최대 16개)
 *   - .category   : 옵션 분류(FIO_OPT_C_ENGINE = "엔진 옵션")
 *   - .group      : 엔진별 서브그룹(FIO_OPT_G_SG)
 * 배열 종단: .name=NULL 센티넬 필수. */
static struct fio_option options[] = {
        {
                .name   = "hipri",                      /* [한국어] 옵션 이름(짧음) */
                .lname  = "High Priority",              /* [한국어] 긴 설명(도움말 표기) */
                .type   = FIO_OPT_STR_SET,              /* [한국어] 값 없이 플래그만 세팅하는 타입 */
                .off1   = offsetof(struct sg_options, hipri), /* [한국어] 저장 위치 오프셋 */
                .help   = "Use polled IO completions",  /* [한국어] --help 표시 텍스트 */
                .category = FIO_OPT_C_ENGINE,           /* [한국어] 카테고리: 엔진 옵션 */
                .group  = FIO_OPT_G_SG,                 /* [한국어] 그룹: SG 엔진 */
        },
	{
		.name	= "readfua",                            /* [한국어] 옵션 이름: READ에 FUA 설정 */
		.lname	= "sg engine read fua flag support",    /* [한국어] 긴 설명 */
		.type	= FIO_OPT_BOOL,                         /* [한국어] 부울(0/1) 타입 */
		.off1	= offsetof(struct sg_options, readfua), /* [한국어] 저장 오프셋 */
		.help	= "Set FUA flag (force unit access) for all Read operations", /* [한국어] 도움말 */
		.def	= "0",                                  /* [한국어] 기본값 0(FUA 미사용) */
		.category = FIO_OPT_C_ENGINE,                   /* [한국어] 엔진 카테고리 */
		.group	= FIO_OPT_G_SG,                         /* [한국어] SG 그룹 */
	},
	{
		.name	= "writefua",                           /* [한국어] 옵션 이름: WRITE에 FUA 설정 */
		.lname	= "sg engine write fua flag support",   /* [한국어] 긴 설명 */
		.type	= FIO_OPT_BOOL,                         /* [한국어] 부울 타입 */
		.off1	= offsetof(struct sg_options, writefua),/* [한국어] 저장 오프셋 */
		.help	= "Set FUA flag (force unit access) for all Write operations", /* [한국어] 도움말 */
		.def	= "0",                                  /* [한국어] 기본값 0 */
		.category = FIO_OPT_C_ENGINE,                   /* [한국어] 엔진 카테고리 */
		.group	= FIO_OPT_G_SG,                         /* [한국어] SG 그룹 */
	},
	{
		.name	= "sg_write_mode",                      /* [한국어] 옵션 이름: WRITE 모드 선택 */
		.lname	= "specify sg write mode",              /* [한국어] 긴 설명 */
		.type	= FIO_OPT_STR,                          /* [한국어] 문자열 열거형(posval 매핑) */
		.off1	= offsetof(struct sg_options, write_mode), /* [한국어] 저장 오프셋 */
		.help	= "Specify SCSI WRITE mode",            /* [한국어] 도움말 */
		.def	= "write",                              /* [한국어] 기본값 "write" = FIO_SG_WRITE */
		.posval = {
			  /* [한국어] 허용 값 테이블: ival(CLI 문자열) → oval(enum). */
			  { .ival = "write",
			    .oval = FIO_SG_WRITE,
			    .help = "Issue standard SCSI WRITE commands",
			  },
			  { .ival = "write_and_verify",
			    .oval = FIO_SG_WRITE_VERIFY,
			    .help = "Issue SCSI WRITE AND VERIFY commands",
			  },
			  { .ival = "verify",
			    .oval = FIO_SG_WRITE_VERIFY,
			    .help = "Issue SCSI WRITE AND VERIFY commands. This "
				    "option is deprecated. Use write_and_verify instead.",
			  },
			  { .ival = "write_same",
			    .oval = FIO_SG_WRITE_SAME,
			    .help = "Issue SCSI WRITE SAME commands",
			  },
			  { .ival = "same",
			    .oval = FIO_SG_WRITE_SAME,
			    .help = "Issue SCSI WRITE SAME commands. This "
				    "option is deprecated. Use write_same instead.",
			  },
			  { .ival = "write_same_ndob",
			    .oval = FIO_SG_WRITE_SAME_NDOB,
			    .help = "Issue SCSI WRITE SAME(16) commands with NDOB flag set",
			  },
			  { .ival = "verify_bytchk_00",
			    .oval = FIO_SG_VERIFY_BYTCHK_00,
			    .help = "Issue SCSI VERIFY commands with BYTCHK set to 00",
			  },
			  { .ival = "verify_bytchk_01",
			    .oval = FIO_SG_VERIFY_BYTCHK_01,
			    .help = "Issue SCSI VERIFY commands with BYTCHK set to 01",
			  },
			  { .ival = "verify_bytchk_11",
			    .oval = FIO_SG_VERIFY_BYTCHK_11,
			    .help = "Issue SCSI VERIFY commands with BYTCHK set to 11",
			  },
			  { .ival = "write_stream",
			    .oval = FIO_SG_WRITE_STREAM,
			    .help = "Issue SCSI WRITE STREAM(16) commands",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_SG,
	},
	{
		.name	= "stream_id",                          /* [한국어] 옵션 이름: 스트림 ID */
		.lname	= "stream id for WRITE STREAM(16) commands", /* [한국어] 긴 설명 */
		.type	= FIO_OPT_INT,                          /* [한국어] 정수 타입 */
		.off1	= offsetof(struct sg_options, stream_id), /* [한국어] 저장 오프셋 (uint16_t) */
		.help	= "Stream ID for WRITE STREAM(16) commands", /* [한국어] 도움말 */
		.def	= "0",                                  /* [한국어] 0=자동 할당 모드(OPEN STREAM로 얻음) */
		.category = FIO_OPT_C_ENGINE,                   /* [한국어] 엔진 카테고리 */
		.group	= FIO_OPT_G_SG,                         /* [한국어] SG 그룹 */
	},
	{
		.name	= NULL,                                 /* [한국어] 센티넬 — 옵션 테이블 종료 표식(필수) */
	},
};

#define MAX_10B_LBA  0xFFFFFFFFULL
/* [한국어] READ/WRITE(10) CDB가 표현할 수 있는 최대 LBA(=2^32-1).
 * 이보다 크면 READ/WRITE(16)로 전환해야 한다. fio_sgio_prep/rw_lba/read_capacity에서 사용. */
#define SCSI_TIMEOUT_MS 30000   // 30 second timeout; currently no method to override
/* [한국어] sg_io_hdr.timeout(밀리초) — 30초. 현재 런타임 옵션으로 덮어쓸 수 없음. */
#define MAX_SB 64               // sense block maximum return size
/* [한국어] SCSI sense buffer 최대 크기(바이트). sgio_cmd.sb 크기와 hdr->mx_sb_len에 사용. */
/*
#define FIO_SGIO_DEBUG
*/
/* [한국어] 디버그 빌드 시 TRIM 큐 맵 추적 및 assert 출력을 활성화. 기본 비활성. */

/* [한국어] io_u 한 개당 전용으로 할당되는 SCSI 명령 슬롯.
 * sgio_data.cmds[]로 io_u->index 인덱싱되어 재사용되며, sg_io_hdr이 이 필드들을 참조한다. */
struct sgio_cmd {
	unsigned char cdb[16];      // enhanced from 10 to support 16 byte commands
	/* [한국어] SCSI CDB(최대 16B) 저장소.
	 * 설정자: fio_sgio_prep()가 opcode·LBA·블록 수·플래그를 채움.
	 * 읽는 자: 커널 SG 드라이버가 sg_io_hdr.cmdp를 통해 DMA로 HBA에 전달.
	 * 값 범위: SCSI CDB 인코딩(opcode 1B + 파라미터). 동기화: io_u 단위 독점. */

	unsigned char sb[MAX_SB];   // add sense block to commands
	/* [한국어] sense data 수신 버퍼(최대 64B).
	 * 설정자: SG 드라이버가 CHECK CONDITION 발생 시 디바이스 sense data를 복사.
	 * 읽는 자: fio_sgio_errdetails()가 에러 보고 문자열 생성 시 읽음.
	 * 값 범위: SCSI sense key/ASC/ASCQ 등. 동기화: io_u 단위 독점. */

	int nr;
	/* [한국어] 미사용(레거시). 향후 확장 여지로 남아 있으며 어떤 함수도 읽지 않음.
	 * 설정자/읽는 자 없음. 값 범위: 미정의(0). 동기화: 불필요. */
};

/* [한국어] 하나의 UNMAP(TRIM) SCSI 명령에 묶일 여러 io_u를 누적하는 큐.
 * sgio_data.trim_queues[io_u->index]로 접근되며, current_queue 필드가 현재 누적 중인 큐를 지정한다. */
struct sgio_trim {
	uint8_t *unmap_param;
	/* [한국어] UNMAP 명령의 parameter list 버퍼(헤더 8B + 범위별 16B × N).
	 * 설정자: fio_sgio_init()에서 calloc, fio_sgio_prep()에서 LBA/길이 쌍을 append.
	 * 읽는 자: fio_sgio_unmap_setup()에서 헤더 길이 필드 채우고 sg_io_hdr.dxferp로 커널에 전달.
	 * 값 범위: SCSI UNMAP parameter list 인코딩. 동기화: current_queue가 가리키는 동안 단일 잡 독점. */

	unsigned int unmap_range_count;
	/* [한국어] 현재 이 큐에 누적된 UNMAP 범위(LBA range) 수.
	 * 설정자: fio_sgio_prep()에서 +1, 완료/제출 후 0으로 리셋.
	 * 읽는 자: fio_sgio_unmap_setup(), commit(), getevents(), queue() 분기 결정.
	 * 값 범위: 0 ~ iodepth. 동기화: td 스레드 전용. */

	struct io_u **trim_io_us;
	/* [한국어] 이 UNMAP에 포함된 io_u 포인터들의 배열.
	 * 설정자: fio_sgio_prep()가 unmap_range_count 위치에 io_u 저장.
	 * 읽는 자: fio_sgio_getevents()가 완료 시 각 io_u를 events[]에 복사,
	 *   commit() 실패 시 전체 io_u에 에러 전파.
	 * 값 범위: 각 원소는 유효한 io_u 포인터. 동기화: td 전용. */
};

/* [한국어] SG 엔진의 잡(td)별 전역 상태.
 * td->io_ops_data에 저장되어 모든 ioengine_ops 콜백이 공유한다. 멀티스레드 공유 없음. */
struct sgio_data {
	struct sgio_cmd *cmds;
	/* [한국어] iodepth만큼 할당된 sgio_cmd 배열(CDB+sense buffer 풀).
	 * 설정자: fio_sgio_init()에서 calloc.
	 * 읽는 자: sgio_hdr_init()이 io_u->index로 인덱싱.
	 * 값 범위: 길이 == td->o.iodepth. 동기화: io_u.index 고유성으로 보장. */

	struct io_u **events;
	/* [한국어] 수확된 완료 io_u 포인터 배열. fio 코어가 event() 콜백으로 하나씩 가져감.
	 * 설정자: fio_sgio_getevents/ioctl_doio가 인덱스 순서대로 채움.
	 * 읽는 자: fio_sgio_event(td, idx) → sd->events[idx].
	 * 값 범위: 길이 == td->o.iodepth. 동기화: 동일 잡 스레드 내 순차 접근. */

	struct pollfd *pfds;
	/* [한국어] poll() 호출용 fd 배열. 파일 수만큼 할당.
	 * 설정자: fio_sgio_getevents()가 매 호출마다 fd/events를 재설정.
	 * 읽는 자: poll() 시스템 호출과 pollin_events().
	 * 값 범위: 길이 == td->o.nr_files. 동기화: td 전용. */

	int *fd_flags;
	/* [한국어] min==0(논블로킹 수확) 경로에서 원래 fcntl 플래그 백업용.
	 * 설정자: fio_sgio_getevents()가 fio_set_fd_nonblocking으로 교체하기 전 저장.
	 * 읽는 자: 수확 종료 후 fcntl(F_SETFL)로 복원.
	 * 값 범위: 원본 파일 플래그 또는 -1(복원 불필요). 동기화: td 전용. */

	void *sgbuf;
	/* [한국어] sg_io_hdr 수신 버퍼(iodepth × sizeof(sg_io_hdr)).
	 * 설정자: fio_sgio_init()에서 calloc.
	 * 읽는 자: fio_sgio_getevents()가 read()로 커널에서 hdr들을 이 버퍼에 받음.
	 * 값 범위: 길이 == iodepth × sizeof(sg_io_hdr). 동기화: td 전용. */

	unsigned int bs;
	/* [한국어] 디바이스의 논리 블록 크기(바이트). READ CAPACITY 또는 BLKSSZGET 결과.
	 * 설정자: fio_sgio_type_check()가 1회만 설정.
	 * 읽는 자: fio_sgio_prep()가 LBA/블록 수 계산 및 정렬 검증에 사용.
	 * 값 범위: 보통 512/4096 등. 동기화: init 이후 read-only. */

	int type_checked;
	/* [한국어] 파일 타입 검사 및 BS 조회 완료 여부(0/1).
	 * 설정자: fio_sgio_type_check()가 1로 세팅.
	 * 읽는 자: fio_sgio_open()에서 중복 호출 방지.
	 * 값 범위: 0/1. 동기화: 단일 잡 스레드 내 직렬 실행. */

	struct sgio_trim **trim_queues;
	/* [한국어] TRIM(UNMAP) 큐 배열(길이 iodepth). 각 원소는 sgio_trim* — 각 잡이 최대
	 * iodepth개의 독립 UNMAP 명령을 동시에 구성할 수 있도록 슬롯을 제공.
	 * 설정자: fio_sgio_init()가 할당, fio_sgio_prep()이 누적.
	 * 읽는 자: queue()/commit()/getevents()가 current_queue 또는 io_u->index로 접근.
	 * 값 범위: 각 원소는 유효 포인터. 동기화: td 전용. */

	int current_queue;
	/* [한국어] 현재 누적 중인 TRIM 큐의 인덱스(첫 번째 TRIM io_u의 index). -1이면 누적 없음.
	 * 설정자: fio_sgio_prep()가 새 TRIM 시작 시 설정, queue()/commit()이 제출 후 -1로 리셋.
	 * 읽는 자: queue()/commit() 분기.
	 * 값 범위: -1 또는 0 ~ iodepth-1. 동기화: td 전용. */
#ifdef FIO_SGIO_DEBUG
	unsigned int *trim_queue_map;
	/* [한국어] (디버그) io_u.index → 소속 TRIM 큐 index 매핑, assert용.
	 * 설정자: prep() 시 기록. 읽는 자: getevents()에서 assert. 값 범위: 큐 인덱스. */
#endif
};

/*
 * [한국어]
 * sgio_get_be16 - 빅엔디언 16비트 값을 호스트 엔디언으로 읽기
 * @buf: 2바이트 빅엔디언 데이터 포인터
 * @return: 호스트 엔디언 uint16_t
 * 용도: SCSI 응답(READ CAPACITY 등) 파싱. 정렬된 16b 로드 후 be16_to_cpu.
 * 호출 체인: fio_sgio_stream_control() → [이 함수]
 */
static inline uint16_t sgio_get_be16(uint8_t *buf)
{
	return be16_to_cpu(*((uint16_t *) buf)); /* [한국어] 포인터 캐스팅으로 16b 로드 후 엔디언 변환 */
}

/*
 * [한국어]
 * sgio_get_be32 - 빅엔디언 32비트 읽기
 * @buf: 4바이트 BE 데이터
 * @return: 호스트 엔디언 uint32_t
 * 호출 체인: fio_sgio_read_capacity() → [이 함수]
 */
static inline uint32_t sgio_get_be32(uint8_t *buf)
{
	return be32_to_cpu(*((uint32_t *) buf)); /* [한국어] 32b BE → CPU 엔디언 변환 */
}

/*
 * [한국어]
 * sgio_get_be64 - 빅엔디언 64비트 읽기
 * @buf: 8바이트 BE 데이터
 * @return: 호스트 엔디언 uint64_t
 * 용도: READ CAPACITY(16)에서 64b max LBA 추출.
 */
static inline uint64_t sgio_get_be64(uint8_t *buf)
{
	return be64_to_cpu(*((uint64_t *) buf)); /* [한국어] 64b BE → CPU 변환 */
}

/*
 * [한국어]
 * sgio_set_be16 - 호스트 값을 BE16으로 버퍼에 기록
 * @val: 기록할 값
 * @buf: 대상 버퍼(2B 이상)
 * SCSI CDB/파라미터 리스트의 다중 바이트 필드는 모두 BE이므로 필수.
 * 호출 체인: fio_sgio_rw_lba/unmap_setup/prep → [이 함수]
 */
static inline void sgio_set_be16(uint16_t val, uint8_t *buf)
{
	uint16_t t = cpu_to_be16(val);         /* [한국어] 호스트 → BE16 변환 */
	memcpy(buf, &t, sizeof(uint16_t));     /* [한국어] 정렬 문제 회피 위해 memcpy로 기록 */
}

/*
 * [한국어]
 * sgio_set_be32 - BE32 기록
 * @val: 값, @buf: 대상
 */
static inline void sgio_set_be32(uint32_t val, uint8_t *buf)
{
	uint32_t t = cpu_to_be32(val);         /* [한국어] 32b → BE */
	memcpy(buf, &t, sizeof(uint32_t));     /* [한국어] unaligned 안전 기록 */
}

/*
 * [한국어]
 * sgio_set_be64 - BE64 기록
 * 용도: 64b LBA를 CDB[2..9]에 기록.
 */
static inline void sgio_set_be64(uint64_t val, uint8_t *buf)
{
	uint64_t t = cpu_to_be64(val);         /* [한국어] 64b → BE */
	memcpy(buf, &t, sizeof(uint64_t));     /* [한국어] unaligned 안전 기록 */
}

/*
 * [한국어]
 * sgio_unbuffered - "동기(unbuffered) 모드" 판정
 * @td: 잡 컨텍스트
 * @return: true면 O_DIRECT 또는 sync_io 요청 — queue()는 동기 실행 경로를 선택
 * 호출 체인: fio_sgio_queue() → [이 함수]
 */
static inline bool sgio_unbuffered(struct thread_data *td)
{
	return (td->o.odirect || td->o.sync_io); /* [한국어] 둘 중 하나라도 참이면 동기 모드 */
}

/*
 * [한국어]
 * sgio_hdr_init - sg_io_hdr와 대응 sgio_cmd를 초기화
 * @sd: 잡 SG 상태 (cmds[] 접근용)
 * @hdr: 초기화할 sg_io_hdr (io_u->hdr 또는 로컬 hdr)
 * @io_u: 연결할 I/O 유닛 (usr_ptr/pack_id에 기록되어 완료 시 역참조)
 * @fs: 데이터 전송이 필요한 명령이면 1, 제어/데이터없음 명령이면 0
 * 커널 SG v3 프로토콜 필드를 채우는 공용 헬퍼.
 * 호출 체인: fio_sgio_prep() → [이 함수]
 */
static void sgio_hdr_init(struct sgio_data *sd, struct sg_io_hdr *hdr,
			  struct io_u *io_u, int fs)
{
	struct sgio_cmd *sc = &sd->cmds[io_u->index]; /* [한국어] io_u 전용 CDB/sense 슬롯 선택 */

	memset(hdr, 0, sizeof(*hdr));       /* [한국어] 헤더 전체 0 초기화 — 미사용 플래그 제거 */
	memset(sc->cdb, 0, sizeof(sc->cdb));/* [한국어] CDB 초기화 — 이전 명령 잔재 제거 */

	hdr->interface_id = 'S';            /* [한국어] SG v3 프로토콜 식별자(항상 'S') */
	hdr->cmdp = sc->cdb;                /* [한국어] CDB 버퍼 포인터 연결 */
	hdr->cmd_len = sizeof(sc->cdb);     /* [한국어] CDB 최대 길이(16B) — 필요 시 prep에서 축소 */
	hdr->sbp = sc->sb;                  /* [한국어] sense buffer 포인터 */
	hdr->mx_sb_len = sizeof(sc->sb);    /* [한국어] sense buffer 최대 크기 */
	hdr->pack_id = io_u->index;         /* [한국어] 디버깅용 태그(io_u 인덱스) */
	hdr->usr_ptr = io_u;                /* [한국어] 완료 수확 시 io_u 복원용 포인터 — 핵심 */
	hdr->timeout = SCSI_TIMEOUT_MS;     /* [한국어] 30초 타임아웃 */

	if (fs) {                           /* [한국어] 데이터 전송 있는 명령이면 */
		hdr->dxferp = io_u->xfer_buf;     /* [한국어] 데이터 버퍼 포인터 설정 */
		hdr->dxfer_len = io_u->xfer_buflen;/* [한국어] 전송 길이 */
	}
}

/*
 * [한국어]
 * pollin_events - poll() 결과에서 POLLIN이 세트된 fd가 하나라도 있는지 검사
 * @pfds: pollfd 배열
 * @fds: 배열 길이
 * @return: 1=있음, 0=없음
 * 호출 체인: fio_sgio_getevents() → [이 함수]
 */
static int pollin_events(struct pollfd *pfds, int fds)
{
	int i;                              /* [한국어] 루프 인덱스 */

	for (i = 0; i < fds; i++)           /* [한국어] 모든 파일 순회 */
		if (pfds[i].revents & POLLIN)     /* [한국어] 읽기 가능 이벤트가 세팅됐으면 */
			return 1;                       /* [한국어] 즉시 true 반환 */

	return 0;                           /* [한국어] 아무 fd도 준비되지 않음 */
}

/*
 * [한국어]
 * sg_fd_read - 지정된 fd에서 size 바이트를 빠짐없이 읽기(짧은 read 반복 처리)
 * @fd: /dev/sgN fd
 * @data: 대상 버퍼
 * @size: 요청 바이트 수
 * @return: 0 성공 / errno / EAGAIN(EOF 도달 전 미완)
 * EAGAIN/EINTR은 재시도, 0 반환(EOF)은 루프 종료 후 EAGAIN 반환.
 * 호출 체인: fio_sgio_getevents() → [이 함수] → read(2)
 */
static int sg_fd_read(int fd, void *data, size_t size)
{
	int err = 0;                        /* [한국어] 누적 오류 */

	while (size) {                      /* [한국어] 필요한 바이트가 남아 있는 동안 */
		ssize_t ret;                      /* [한국어] 매 read 반환값 */

		ret = read(fd, data, size);       /* [한국어] SG 드라이버에서 sg_io_hdr을 논블로킹 수신 */
		if (ret < 0) {                    /* [한국어] 오류 발생 */
			if (errno == EAGAIN || errno == EINTR) /* [한국어] 일시적이면 */
				continue;                              /* [한국어] 재시도 */
			err = errno;                    /* [한국어] 영구 오류 저장 */
			break;                          /* [한국어] 루프 탈출 */
		} else if (!ret)                  /* [한국어] EOF — 더 읽을 데이터 없음 */
			break;                          /* [한국어] 불완전하지만 탈출 */
		else {                            /* [한국어] 일부 읽음 */
			data += ret;                    /* [한국어] 다음 바이트 위치로 포인터 이동 */
			size -= ret;                    /* [한국어] 남은 바이트 수 감소 */
		}
	}

	if (err)                            /* [한국어] 영구 오류 있었으면 */
		return err;                       /* [한국어] 에러 반환 */
	if (size)                           /* [한국어] 완전히 채우지 못했으면 */
		return EAGAIN;                    /* [한국어] 재시도 요청 신호 */

	return 0;                           /* [한국어] 완전히 읽음 */
}

/*
 * [한국어]
 * fio_sgio_getevents - 비동기 모드 완료 이벤트 수확 콜백
 * @td: 잡 컨텍스트
 * @min: 최소 수확 개수 (0이면 블로킹 없이 즉시 반환)
 * @max: 최대 수확 개수
 * @t: 타임아웃(현재 미사용 — SG는 자체 timeout 사용)
 * @return: 수확한 이벤트 수 (io_u 기준, TRIM은 범위 수만큼 확장) / 음수 errno
 * 1) min>0이면 poll()로 블로킹 대기 → 2) /dev/sgN에서 sg_io_hdr 레코드 read() →
 * 3) TRIM의 경우 하나의 UNMAP 완료가 여러 io_u를 의미하므로 확장 처리.
 * 호출 체인: td_io_getevents() → [이 함수] → poll(2)/read(2)
 */
static int fio_sgio_getevents(struct thread_data *td, unsigned int min,
			      unsigned int max,
			      const struct timespec fio_unused *t)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] 이 td의 SG 상태 */
	int left = max, eventNum, ret, r = 0, trims = 0; /* [한국어] 남은수/루프i/ret/총수확/트림확장수 */
	void *buf = sd->sgbuf;                    /* [한국어] hdr 수신 공용 버퍼 */
	unsigned int i, j, events;                /* [한국어] 인덱스 및 이번 루프 이벤트 수 */
	struct fio_file *f;                       /* [한국어] 파일 반복자 */
	struct io_u *io_u;                        /* [한국어] 완료 io_u 포인터 */

	/*
	 * Fill in the file descriptors
	 */
	for_each_file(td, f, i) {                 /* [한국어] 이 잡의 모든 파일 순회 */
		/*
		 * don't block for min events == 0
		 */
		if (!min)                               /* [한국어] 논블로킹 수확 요청이면 */
			sd->fd_flags[i] = fio_set_fd_nonblocking(f->fd, "sg"); /* [한국어] O_NONBLOCK 설정, 원래 플래그 백업 */
		else
			sd->fd_flags[i] = -1;                 /* [한국어] 복원 불필요 표시 */

		sd->pfds[i].fd = f->fd;                 /* [한국어] poll 대상 fd */
		sd->pfds[i].events = POLLIN;            /* [한국어] 읽기 가능 이벤트 관심 */
	}

	/*
	** There are two counters here:
	**  - number of SCSI commands completed
	**  - number of io_us completed
	**
	** These are the same with reads and writes, but
	** could differ with trim/unmap commands because
	** a single unmap can include multiple io_us
	*/

	while (left > 0) {                        /* [한국어] 요청 최대치를 채울 때까지 반복 */
		char *p;                                /* [한국어] sgbuf 내부 기록 포인터 */

		dprint(FD_IO, "sgio_getevents: sd %p: min=%d, max=%d, left=%d\n", sd, min, max, left); /* [한국어] 디버그 로그 */

		do {
			if (!min)                             /* [한국어] 논블로킹이면 poll 스킵 */
				break;

			ret = poll(sd->pfds, td->o.nr_files, -1); /* [한국어] 무한 대기 poll */
			if (ret < 0) {                        /* [한국어] poll 오류 */
				if (!r)                             /* [한국어] 아직 수확 0이면 */
					r = -errno;                       /* [한국어] 에러 반환값으로 기록 */
				td_verror(td, errno, "poll");       /* [한국어] fio 에러 보고 */
				break;
			} else if (!ret)                      /* [한국어] 타임아웃(무한이므로 실질적으로 불가) */
				continue;

			if (pollin_events(sd->pfds, td->o.nr_files)) /* [한국어] 준비된 fd가 있다면 */
				break;                              /* [한국어] 수확 단계로 */
		} while (1);

		if (r < 0)                              /* [한국어] poll 오류 발생했다면 루프 종료 */
			break;

re_read:
		p = buf;                                /* [한국어] sgbuf 선두로 리셋 */
		events = 0;                             /* [한국어] 이번 라운드 이벤트 카운터 */
		for_each_file(td, f, i) {               /* [한국어] 각 파일에서 수확 */
			for (eventNum = 0; eventNum < left; eventNum++) { /* [한국어] 남은 한도까지 */
				ret = sg_fd_read(f->fd, p, sizeof(struct sg_io_hdr)); /* [한국어] 하나의 hdr 수신 */
				dprint(FD_IO, "sgio_getevents: sg_fd_read ret: %d\n", ret);
				if (ret) {                          /* [한국어] 실패(EAGAIN 포함) */
					r = -ret;                         /* [한국어] 부호 반전해서 반환값에 반영 */
					td_verror(td, r, "sg_read");
					break;
				}
				io_u = ((struct sg_io_hdr *)p)->usr_ptr; /* [한국어] hdr → io_u 복원 */
				if (io_u->ddir == DDIR_TRIM) {      /* [한국어] TRIM 완료는 여러 io_u를 대표 */
					events += sd->trim_queues[io_u->index]->unmap_range_count; /* [한국어] 범위 수만큼 */
					eventNum += sd->trim_queues[io_u->index]->unmap_range_count - 1; /* [한국어] 루프 카운터도 보정 */
				} else
					events++;                         /* [한국어] 일반 R/W는 1개 */

				p += sizeof(struct sg_io_hdr);      /* [한국어] 다음 hdr 슬롯 위치로 */
				dprint(FD_IO, "sgio_getevents: events: %d, eventNum: %d, left: %d\n", events, eventNum, left);
			}
		}

		if (r < 0 && !events)                   /* [한국어] 오류이고 아무것도 못 받았으면 탈출 */
			break;
		if (!events) {                          /* [한국어] 수확 0이면 */
			usleep(1000);                         /* [한국어] 1ms 백오프 후 */
			goto re_read;                         /* [한국어] 재시도 */
		}

		left -= events;                         /* [한국어] 남은 한도 감소 */
		r += events;                            /* [한국어] 누적 수확 수 */

		for (i = 0; i < events; i++) {          /* [한국어] 수확된 hdr를 events[]로 매핑 */
			struct sg_io_hdr *hdr = (struct sg_io_hdr *) buf + i; /* [한국어] i번째 hdr */
			sd->events[i + trims] = hdr->usr_ptr; /* [한국어] 외부에 노출될 io_u 포인터 저장 */
			io_u = (struct io_u *)(hdr->usr_ptr); /* [한국어] 편의 별칭 */

			if (hdr->info & SG_INFO_CHECK) {      /* [한국어] host/driver/scsi 에러 플래그 */
				/* record if an io error occurred, ignore resid */
				memcpy(&io_u->hdr, hdr, sizeof(struct sg_io_hdr)); /* [한국어] errdetails용 hdr 사본 저장 */
				sd->events[i + trims]->error = EIO;  /* [한국어] I/O 에러 표시 */
			}

			if (io_u->ddir == DDIR_TRIM) {        /* [한국어] TRIM 대표 io_u 처리 */
				struct sgio_trim *st = sd->trim_queues[io_u->index]; /* [한국어] 해당 TRIM 큐 */
#ifdef FIO_SGIO_DEBUG
				assert(st->trim_io_us[0] == io_u);  /* [한국어] 첫 원소가 대표 io_u여야 함 */
				assert(sd->trim_queue_map[io_u->index] == io_u->index);
				dprint(FD_IO, "sgio_getevents: reaping %d io_us from trim queue %d\n", st->unmap_range_count, io_u->index);
				dprint(FD_IO, "sgio_getevents: reaped io_u %d and stored in events[%d]\n", io_u->index, i+trims);
#endif
				for (j = 1; j < st->unmap_range_count; j++) { /* [한국어] 나머지 io_u들도 events[]에 추가 */
					++trims;                          /* [한국어] 추가 슬롯 카운트 */
					sd->events[i + trims] = st->trim_io_us[j]; /* [한국어] 동일 완료 결과로 등록 */
#ifdef FIO_SGIO_DEBUG
					dprint(FD_IO, "sgio_getevents: reaped io_u %d and stored in events[%d]\n", st->trim_io_us[j]->index, i+trims);
					assert(sd->trim_queue_map[st->trim_io_us[j]->index] == io_u->index);
#endif
					if (hdr->info & SG_INFO_CHECK) {  /* [한국어] 에러면 모든 io_u에 전파 */
						/* record if an io error occurred, ignore resid */
						memcpy(&st->trim_io_us[j]->hdr, hdr, sizeof(struct sg_io_hdr));
						sd->events[i + trims]->error = EIO;
					}
				}
				events -= st->unmap_range_count - 1; /* [한국어] events 카운터 보정(루프 상한) */
				st->unmap_range_count = 0;          /* [한국어] 큐 재사용 준비 */
			}
		}
	}

	if (!min) {                               /* [한국어] 논블로킹 경로 정리 */
		for_each_file(td, f, i) {
			if (sd->fd_flags[i] == -1)            /* [한국어] 원본 플래그 미백업 파일은 스킵 */
				continue;

			if (fcntl(f->fd, F_SETFL, sd->fd_flags[i]) < 0) /* [한국어] 원래 플래그 복원 */
				log_err("fio: sg failed to restore fcntl flags: %s\n", strerror(errno));
		}
	}

	return r;                                 /* [한국어] 총 수확 수(또는 음수 에러) */
}

/*
 * [한국어]
 * fio_sgio_ioctl_doio - ioctl(SG_IO)로 단일 SCSI 명령을 동기 실행
 * @td: 잡
 * @f: 대상 파일
 * @io_u: I/O 유닛 (io_u->hdr에 CDB/dxfer 구성되어 있어야 함)
 * @return: FIO_Q_COMPLETED / 음수(ioctl 실패)
 * /dev/sdX 블록 디바이스에서 주로 사용. 에러 시 io_u->error=EIO.
 * 호출 체인: fio_sgio_doio() → [이 함수] → ioctl(SG_IO)
 */
static enum fio_q_status fio_sgio_ioctl_doio(struct thread_data *td,
					     struct fio_file *f,
					     struct io_u *io_u)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] SG 상태 */
	struct sg_io_hdr *hdr = &io_u->hdr;       /* [한국어] 미리 prep된 hdr */
	int ret;                                  /* [한국어] ioctl 반환 */

	sd->events[0] = io_u;                     /* [한국어] 동기 완료이므로 단일 슬롯에 바로 등록 */

	ret = ioctl(f->fd, SG_IO, hdr);           /* [한국어] SG_IO ioctl — 커널이 SCSI 실행 후 완료까지 블로킹 */
	if (ret < 0)                              /* [한국어] ioctl 자체 실패 */
		return ret;

	/* record if an io error occurred */
	if (hdr->info & SG_INFO_CHECK)            /* [한국어] SCSI/host/driver 에러 플래그 */
		io_u->error = EIO;                      /* [한국어] I/O 오류 표기 */

	return FIO_Q_COMPLETED;                   /* [한국어] 동기 완료 반환 */
}

/*
 * [한국어]
 * fio_sgio_rw_doio - /dev/sgN에 write()로 SCSI 명령 제출 (선택적 동기 read)
 * @td: 잡
 * @f: 파일
 * @io_u: I/O 유닛
 * @do_sync: 1이면 즉시 완료 수확, 0이면 큐잉만
 * @return: FIO_Q_COMPLETED(동기) / FIO_Q_QUEUED(비동기) / 음수(write 실패)
 * do_sync 경로에서는 다른 in-flight 명령도 완료될 수 있으므로 usr_ptr 비교로 해당 io_u 도달까지 대기.
 * 호출 체인: fio_sgio_doio()/fio_sgio_commit() → [이 함수] → write(2)/read(2)
 */
static enum fio_q_status fio_sgio_rw_doio(struct thread_data *td,
					  struct fio_file *f,
					  struct io_u *io_u, int do_sync)
{
	struct sg_io_hdr *hdr = &io_u->hdr;       /* [한국어] 미리 prep된 hdr */
	int ret;                                  /* [한국어] syscall 결과 */

	ret = write(f->fd, hdr, sizeof(*hdr));    /* [한국어] /dev/sgN에 hdr 써서 SCSI 명령 제출 */
	if (ret < 0)                              /* [한국어] 제출 실패 */
		return ret;

	if (do_sync) {                            /* [한국어] 동기 모드면 즉시 수확 */
		/*
		 * We can't just read back the first command that completes
		 * and assume it's the one we need, it could be any command
		 * that is inflight.
		 */
		do {
			struct io_u *__io_u;                  /* [한국어] 수확된 완료 io_u */

			ret = read(f->fd, hdr, sizeof(*hdr)); /* [한국어] 완료 hdr 수신 */
			if (ret < 0)
				return ret;

			__io_u = hdr->usr_ptr;                /* [한국어] 완료 io_u 복원 */

			/* record if an io error occurred */
			if (hdr->info & SG_INFO_CHECK)
				__io_u->error = EIO;                /* [한국어] 에러 기록 */

			if (__io_u == io_u)                   /* [한국어] 우리가 기다리던 io_u면 */
				break;                              /* [한국어] 종료 */

			if (io_u_sync_complete(td, __io_u))   /* [한국어] 다른 io_u는 즉시 fio 코어에 완료 통보 */
				break;                              /* [한국어] 완료가 처리되지 못했으면 루프 종료 */

		} while (1);

		return FIO_Q_COMPLETED;                 /* [한국어] 목표 io_u 완료 확인 */
	}

	return FIO_Q_QUEUED;                      /* [한국어] 비동기: getevents()가 나중에 수확 */
}

/*
 * [한국어]
 * fio_sgio_doio - 파일 타입에 따라 ioctl/RW 경로 분기
 * @td: 잡
 * @io_u: 준비된 I/O 유닛
 * @do_sync: 동기 수확 여부
 * @return: 하위 doio가 반환한 상태
 * 블록 디바이스(/dev/sdX)는 항상 ioctl, 캐릭터(/dev/sgN)는 write 경로.
 * 호출 체인: fio_sgio_queue() → [이 함수] → ioctl_doio/rw_doio
 */
static enum fio_q_status fio_sgio_doio(struct thread_data *td,
				       struct io_u *io_u, int do_sync)
{
	struct fio_file *f = io_u->file;          /* [한국어] 대상 파일 */
	enum fio_q_status ret;                    /* [한국어] 반환 상태 */

	if (f->filetype == FIO_TYPE_BLOCK) {      /* [한국어] /dev/sdX 블록 디바이스 */
		ret = fio_sgio_ioctl_doio(td, f, io_u); /* [한국어] ioctl 경로(항상 동기) */
		if (io_u->error)                        /* [한국어] 에러 있으면 */
			td_verror(td, io_u->error, __func__); /* [한국어] fio 에러 기록 */
	} else {                                  /* [한국어] /dev/sgN 캐릭터 디바이스 */
		ret = fio_sgio_rw_doio(td, f, io_u, do_sync); /* [한국어] write/read 경로 */
		if (io_u->error && do_sync)             /* [한국어] 동기 경로 에러만 즉시 보고 */
			td_verror(td, io_u->error, __func__);
	}

	return ret;
}

/*
 * [한국어]
 * fio_sgio_rw_lba - READ/WRITE CDB에 LBA와 블록 수를 인코딩
 * @hdr: 대상 sg_io_hdr (CDB opcode가 이미 세팅됨)
 * @lba: 시작 LBA
 * @nr_blocks: 전송 블록 수
 * @override16: true면 LBA 크기와 무관하게 16바이트 CDB 레이아웃 사용
 * 10바이트: LBA(BE32)@[2..5], 블록수(BE16)@[7..8]; 16바이트: LBA(BE64)@[2..9], 블록수(BE32)@[10..13].
 * 호출 체인: fio_sgio_prep() → [이 함수]
 */
static void fio_sgio_rw_lba(struct sg_io_hdr *hdr, unsigned long long lba,
			    unsigned long long nr_blocks, bool override16)
{
	if (lba < MAX_10B_LBA && !override16) {   /* [한국어] 10B CDB로 표현 가능하고 강제 16B 아님 */
		sgio_set_be32((uint32_t) lba, &hdr->cmdp[2]); /* [한국어] 32b LBA 기록 */
		sgio_set_be16((uint16_t) nr_blocks, &hdr->cmdp[7]); /* [한국어] 16b 블록 수 */
	} else {                                  /* [한국어] 16B CDB */
		sgio_set_be64(lba, &hdr->cmdp[2]);      /* [한국어] 64b LBA */
		sgio_set_be32((uint32_t) nr_blocks, &hdr->cmdp[10]); /* [한국어] 32b 블록 수 */
	}

	return;
}

/*
 * [한국어]
 * fio_sgio_prep - SG 엔진의 I/O 준비 콜백
 * @td: 잡 컨텍스트
 * @io_u: 준비할 I/O 유닛 (offset/xfer_buflen/ddir 확정 상태)
 * @return: 0 성공 / EINVAL(미정렬) / (assert 실패 시 종료)
 *
 * io_u에 대응하는 sg_io_hdr을 구성한다:
 *  - READ: READ(10)/READ(16) + 선택적 FUA/HIPRI
 *  - WRITE: write_mode에 따라 WRITE/WRITE_VERIFY/WRITE_SAME/WRITE_STREAM/VERIFY
 *  - TRIM: UNMAP parameter list에 (LBA, len) 쌍을 누적(실제 제출은 queue/commit)
 *  - SYNC: SYNCHRONIZE CACHE(10/16)
 * 호출 체인: td_io_prep() → [이 함수]
 */
static int fio_sgio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct sg_io_hdr *hdr = &io_u->hdr;       /* [한국어] 이 io_u 전용 hdr */
	struct sg_options *o = td->eo;            /* [한국어] SG 엔진 옵션 */
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] SG 상태 */
	unsigned long long nr_blocks, lba;        /* [한국어] 블록 단위 크기/오프셋 */
	int offset;                               /* [한국어] UNMAP param 내 쓰기 오프셋 */

	if (io_u->xfer_buflen & (sd->bs - 1)) {   /* [한국어] 블록 크기 미정렬 체크(bs는 2의 거듭제곱 가정) */
		log_err("read/write not sector aligned\n");
		return EINVAL;                          /* [한국어] 정렬 위반 */
	}

	nr_blocks = io_u->xfer_buflen / sd->bs;   /* [한국어] 블록 수 계산 */
	lba = io_u->offset / sd->bs;              /* [한국어] 시작 LBA */

	if (io_u->ddir == DDIR_READ) {            /* [한국어] === READ === */
		sgio_hdr_init(sd, hdr, io_u, 1);        /* [한국어] 공용 hdr 초기화(데이터 전송 있음) */

		hdr->dxfer_direction = SG_DXFER_FROM_DEV; /* [한국어] 디바이스→호스트 전송 */
		if (lba < MAX_10B_LBA)                  /* [한국어] 32b LBA로 충분하면 */
			hdr->cmdp[0] = 0x28; // read(10)
		else
			hdr->cmdp[0] = 0x88; // read(16)

		if (o->hipri)                           /* [한국어] HIPRI 폴링 요청 */
			hdr->flags |= SGV4_FLAG_HIPRI;
		if (o->readfua)                         /* [한국어] FUA 요청 */
			hdr->cmdp[1] |= 0x08;                 /* [한국어] CDB byte1 bit3 = FUA */

		fio_sgio_rw_lba(hdr, lba, nr_blocks, false); /* [한국어] LBA/블록 수 기록 */

	} else if (io_u->ddir == DDIR_WRITE) {    /* [한국어] === WRITE 계열 === */
		sgio_hdr_init(sd, hdr, io_u, 1);

		hdr->dxfer_direction = SG_DXFER_TO_DEV; /* [한국어] 호스트→디바이스 */
		switch(o->write_mode) {                 /* [한국어] 모드별 opcode 분기 */
		case FIO_SG_WRITE:                      /* [한국어] 표준 WRITE */
			if (lba < MAX_10B_LBA)
				hdr->cmdp[0] = 0x2a; // write(10)
			else
				hdr->cmdp[0] = 0x8a; // write(16)
			if (o->hipri)
				hdr->flags |= SGV4_FLAG_HIPRI;
			if (o->writefua)
				hdr->cmdp[1] |= 0x08;               /* [한국어] FUA 비트 */
			break;
		case FIO_SG_WRITE_VERIFY:               /* [한국어] WRITE AND VERIFY */
			if (lba < MAX_10B_LBA)
				hdr->cmdp[0] = 0x2e; // write and verify(10)
			else
				hdr->cmdp[0] = 0x8e; // write and verify(16)
			break;
			// BYTCHK is disabled by virtue of the memset in sgio_hdr_init
		case FIO_SG_WRITE_SAME:                 /* [한국어] WRITE SAME — 1블록 반복 기록 */
			hdr->dxfer_len = sd->bs;              /* [한국어] 전송 길이는 1블록뿐 */
			if (lba < MAX_10B_LBA)
				hdr->cmdp[0] = 0x41; // write same(10)
			else
				hdr->cmdp[0] = 0x93; // write same(16)
			break;
		case FIO_SG_WRITE_SAME_NDOB:            /* [한국어] WRITE SAME(16) + NDOB — 0으로 채우기 */
			hdr->cmdp[0] = 0x93; // write same(16)
			hdr->cmdp[1] |= 0x1; // no data output buffer  /* [한국어] 데이터 버퍼 없음(0 패턴) */
			hdr->dxfer_len = 0;                   /* [한국어] 전송 데이터 0B */
			break;
		case FIO_SG_WRITE_STREAM:               /* [한국어] WRITE STREAM(16) */
			hdr->cmdp[0] = 0x9a; // write stream (16)
			if (o->writefua)
				hdr->cmdp[1] |= 0x08;
			sgio_set_be64(lba, &hdr->cmdp[2]);   /* [한국어] 64b LBA */
			sgio_set_be16((uint16_t) io_u->file->engine_pos, &hdr->cmdp[10]); /* [한국어] 스트림 ID */
			sgio_set_be16((uint16_t) nr_blocks, &hdr->cmdp[12]); /* [한국어] 전송 블록 수 */
			break;
		case FIO_SG_VERIFY_BYTCHK_00:           /* [한국어] VERIFY 매체 검증 */
			if (lba < MAX_10B_LBA)
				hdr->cmdp[0] = 0x2f; // VERIFY(10)
			else
				hdr->cmdp[0] = 0x8f; // VERIFY(16)
			hdr->dxfer_len = 0;                   /* [한국어] 데이터 전송 없음 */
			break;
		case FIO_SG_VERIFY_BYTCHK_01:           /* [한국어] VERIFY + 호스트 데이터 비교 */
			if (lba < MAX_10B_LBA)
				hdr->cmdp[0] = 0x2f; // VERIFY(10)
			else
				hdr->cmdp[0] = 0x8f; // VERIFY(16)
			hdr->cmdp[1] |= 0x02;		// BYTCHK = 01b
			break;
		case FIO_SG_VERIFY_BYTCHK_11:           /* [한국어] VERIFY + 1블록 반복 비교 */
			if (lba < MAX_10B_LBA)
				hdr->cmdp[0] = 0x2f; // VERIFY(10)
			else
				hdr->cmdp[0] = 0x8f; // VERIFY(16)
			hdr->cmdp[1] |= 0x06;		// BYTCHK = 11b
			hdr->dxfer_len = sd->bs;              /* [한국어] 1블록 참조 데이터 */
			break;
		};

		if (o->write_mode != FIO_SG_WRITE_STREAM) /* [한국어] STREAM은 위에서 직접 인코딩 완료 */
			fio_sgio_rw_lba(hdr, lba, nr_blocks,
				o->write_mode == FIO_SG_WRITE_SAME_NDOB); /* [한국어] NDOB는 16B CDB 강제 */

	} else if (io_u->ddir == DDIR_TRIM) {     /* [한국어] === TRIM/UNMAP === */
		struct sgio_trim *st;                   /* [한국어] 현재 UNMAP 큐 */

		if (sd->current_queue == -1) {          /* [한국어] 새 UNMAP 시작 */
			sgio_hdr_init(sd, hdr, io_u, 0);      /* [한국어] 데이터 전송 없음으로 초기화 */

			hdr->cmd_len = 10;                    /* [한국어] UNMAP CDB는 10B */
			hdr->dxfer_direction = SG_DXFER_TO_DEV; /* [한국어] 파라미터 리스트는 호스트→디바이스 */
			hdr->cmdp[0] = 0x42; // unmap
			sd->current_queue = io_u->index;      /* [한국어] 첫 TRIM io_u의 index로 큐 지정 */
			st = sd->trim_queues[sd->current_queue];
			hdr->dxferp = st->unmap_param;        /* [한국어] 파라미터 리스트 버퍼 연결 */
#ifdef FIO_SGIO_DEBUG
			assert(sd->trim_queues[io_u->index]->unmap_range_count == 0);
			dprint(FD_IO, "sg: creating new queue based on io_u %d\n", io_u->index);
#endif
		}
		else
			st = sd->trim_queues[sd->current_queue]; /* [한국어] 기존 큐에 추가 */

		dprint(FD_IO, "sg: adding io_u %d to trim queue %d\n", io_u->index, sd->current_queue);
		st->trim_io_us[st->unmap_range_count] = io_u; /* [한국어] io_u 추적용 저장 */
#ifdef FIO_SGIO_DEBUG
		sd->trim_queue_map[io_u->index] = sd->current_queue;
#endif

		offset = 8 + 16 * st->unmap_range_count; /* [한국어] UNMAP param 헤더 8B + 범위당 16B */
		sgio_set_be64(lba, &st->unmap_param[offset]);      /* [한국어] 시작 LBA(BE64) */
		sgio_set_be32((uint32_t) nr_blocks, &st->unmap_param[offset + 8]); /* [한국어] 길이(BE32) */

		st->unmap_range_count++;                /* [한국어] 범위 수 증가 */

	} else if (ddir_sync(io_u->ddir)) {       /* [한국어] === SYNC (SYNCHRONIZE CACHE) === */
		sgio_hdr_init(sd, hdr, io_u, 0);
		hdr->dxfer_direction = SG_DXFER_NONE;   /* [한국어] 데이터 전송 없음 */
		if (lba < MAX_10B_LBA)
			hdr->cmdp[0] = 0x35; // synccache(10)
		else
			hdr->cmdp[0] = 0x91; // synccache(16)
	} else
		assert(0);                              /* [한국어] 정의되지 않은 ddir은 버그 */

	return 0;
}

/*
 * [한국어]
 * fio_sgio_unmap_setup - 누적된 UNMAP 큐를 실제 CDB에 완성
 * @hdr: UNMAP용 sg_io_hdr (CDB[0]=0x42 세팅 상태)
 * @st: 누적된 범위들을 담은 sgio_trim
 *
 * UNMAP parameter list는 {data len BE16, block descriptor len BE16, reserved 4B, descriptors...} 구조.
 * 여기서 CDB[7..8](allocation length)와 param 헤더의 두 길이 필드를 채운다.
 * 호출 체인: queue()(동기 TRIM)/commit()(비동기 TRIM) → [이 함수]
 */
static void fio_sgio_unmap_setup(struct sg_io_hdr *hdr, struct sgio_trim *st)
{
	uint16_t cnt = st->unmap_range_count * 16; /* [한국어] 범위 × 16B = 디스크립터 총 길이 */

	hdr->dxfer_len = cnt + 8;                 /* [한국어] 실제 전송 길이 = 디스크립터 + 헤더 8B */
	sgio_set_be16(cnt + 8, &hdr->cmdp[7]);    /* [한국어] CDB[7..8]: param list allocation length */
	sgio_set_be16(cnt + 6, st->unmap_param);  /* [한국어] param[0..1]: data length(=list-2) */
	sgio_set_be16(cnt, &st->unmap_param[2]);  /* [한국어] param[2..3]: block descriptor data length */

	return;
}

/*
 * [한국어]
 * fio_sgio_queue - SG 엔진의 I/O 제출 콜백
 * @td: 잡
 * @io_u: prep 완료된 io_u
 * @return: FIO_Q_COMPLETED(동기 완료) / FIO_Q_QUEUED(비동기 또는 TRIM 누적)
 *
 * 동기 모드: ioctl(SG_IO)/write+read로 즉시 실행 (블록 디바이스 또는 direct/sync)
 * 비동기 모드: write()로 /dev/sgN에 제출 → getevents()가 나중에 수확
 * TRIM 동기: 단일 범위 UNMAP 즉시 실행
 * TRIM 비동기: 범위만 누적하고 FIO_Q_QUEUED — commit()에서 일괄 제출
 * 호출 체인: td_io_queue() → [이 함수] → fio_sgio_doio
 */
static enum fio_q_status fio_sgio_queue(struct thread_data *td,
					struct io_u *io_u)
{
	struct sg_io_hdr *hdr = &io_u->hdr;       /* [한국어] 준비된 hdr */
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] SG 상태 */
	int ret, do_sync = 0;                     /* [한국어] ret/동기실행 여부 */

	fio_ro_check(td, io_u);                   /* [한국어] read-only 모드에서 write가 오지 않는지 검증 */

	if (sgio_unbuffered(td) || ddir_sync(io_u->ddir)) /* [한국어] O_DIRECT/sync/SYNCCACHE면 동기 */
		do_sync = 1;

	if (io_u->ddir == DDIR_TRIM) {            /* [한국어] TRIM 분기 */
		if (do_sync || io_u->file->filetype == FIO_TYPE_BLOCK) { /* [한국어] 동기 TRIM 경로 */
			struct sgio_trim *st = sd->trim_queues[sd->current_queue];

			/* finish cdb setup for unmap because we are
			** doing unmap commands synchronously */
#ifdef FIO_SGIO_DEBUG
			assert(st->unmap_range_count == 1);
			assert(io_u == st->trim_io_us[0]);
#endif
			hdr = &io_u->hdr;                     /* [한국어] 대표 io_u의 hdr 사용 */

			fio_sgio_unmap_setup(hdr, st);        /* [한국어] 길이 필드 확정 */

			st->unmap_range_count = 0;            /* [한국어] 큐 비움 */
			sd->current_queue = -1;               /* [한국어] 다음 TRIM 라운드 준비 */
		} else
			/* queue up trim ranges and submit in commit() */
			return FIO_Q_QUEUED;                  /* [한국어] 누적만 하고 반환 — commit에서 일괄 */
	}

	ret = fio_sgio_doio(td, io_u, do_sync);   /* [한국어] 실제 제출 */

	if (ret < 0)                              /* [한국어] 시스템 호출 실패 */
		io_u->error = errno;
	else if (hdr->status) {                   /* [한국어] SCSI status != 0 */
		io_u->resid = hdr->resid;               /* [한국어] 미전송 바이트 기록 */
		io_u->error = EIO;
	} else if (td->io_ops->commit != NULL) {  /* [한국어] commit이 정의된(=비블록) 엔진이면 */
		if (do_sync && !ddir_sync(io_u->ddir)) { /* [한국어] 동기 R/W */
			io_u_mark_submit(td, 1);              /* [한국어] 제출 카운트 */
			io_u_mark_complete(td, 1);            /* [한국어] 완료 카운트 */
		} else if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) { /* [한국어] 비동기 R/W */
			io_u_mark_submit(td, 1);
			io_u_queued(td, io_u);                /* [한국어] 큐 시간 기록 */
		}
	}

	if (io_u->error) {                        /* [한국어] 에러 있으면 즉시 완료 반환 */
		td_verror(td, io_u->error, "xfer");
		return FIO_Q_COMPLETED;
	}

	return ret;                               /* [한국어] 상위 호출자에 상태 전달 */
}

/*
 * [한국어]
 * fio_sgio_commit - 비동기 TRIM 큐에 쌓인 범위들을 하나의 UNMAP으로 일괄 제출
 * @td: 잡
 * @return: 0 성공 / 음수 에러(EIO 등)
 * current_queue == -1 이면 아무것도 하지 않음.
 * 성공 시 누적 io_u 모두에 issue_time을 기록하고 submit 카운터 갱신.
 * 호출 체인: td_io_commit() → [이 함수] → fio_sgio_rw_doio(비동기 write)
 */
static int fio_sgio_commit(struct thread_data *td)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] SG 상태 */
	struct sgio_trim *st;                     /* [한국어] 제출할 TRIM 큐 */
	struct io_u *io_u;                        /* [한국어] 대표 io_u */
	struct sg_io_hdr *hdr;                    /* [한국어] 대표 hdr */
	struct timespec now;                      /* [한국어] issue_time 기록용 */
	unsigned int i;                           /* [한국어] 루프 인덱스 */
	int ret;                                  /* [한국어] 반환 */

	if (sd->current_queue == -1)              /* [한국어] 누적된 TRIM 없으면 */
		return 0;                               /* [한국어] NOP */

	st = sd->trim_queues[sd->current_queue];  /* [한국어] 현재 큐 */
	io_u = st->trim_io_us[0];                 /* [한국어] 대표(= 큐 식별자) io_u */
	hdr = &io_u->hdr;                         /* [한국어] 대표 hdr */

	fio_sgio_unmap_setup(hdr, st);            /* [한국어] 길이 필드 확정 */

	sd->current_queue = -1;                   /* [한국어] 새로운 누적 시작 허용 */

	ret = fio_sgio_rw_doio(td, io_u->file, io_u, 0); /* [한국어] 비동기 write(do_sync=0) */

	if (ret < 0 || hdr->status) {             /* [한국어] 실패 — 큐 내 모든 io_u에 에러 전파 */
		int error;

		if (ret < 0)
			error = errno;
		else {
			error = EIO;
			ret = -EIO;
		}

		for (i = 0; i < st->unmap_range_count; i++) {
			st->trim_io_us[i]->error = error;     /* [한국어] 각 io_u에 에러 표시 */
			clear_io_u(td, st->trim_io_us[i]);    /* [한국어] in-flight 해제 */
			if (hdr->status)
				st->trim_io_us[i]->resid = hdr->resid; /* [한국어] 미전송 바이트 전파 */
		}

		td_verror(td, error, "xfer");
		return ret;
	}

	if (fio_fill_issue_time(td)) {            /* [한국어] issue_time 채우기 요구 */
		fio_gettime(&now, NULL);                /* [한국어] 현재 시간 */
		for (i = 0; i < st->unmap_range_count; i++) {
			memcpy(&st->trim_io_us[i]->issue_time, &now, sizeof(now)); /* [한국어] 공통 issue_time */
			io_u_queued(td, io_u);                /* [한국어] 큐 지연 통계 업데이트 */
		}
	}
	io_u_mark_submit(td, st->unmap_range_count); /* [한국어] 제출 개수 마킹(범위 수) */

	return 0;
}

/*
 * [한국어]
 * fio_sgio_event - 수확된 이벤트 인덱스에 해당하는 io_u 반환
 * @td: 잡, @event: events[] 인덱스
 * @return: 완료된 io_u 포인터
 * 호출 체인: td_io_event() → [이 함수]
 */
static struct io_u *fio_sgio_event(struct thread_data *td, int event)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] SG 상태 */

	return sd->events[event];                 /* [한국어] 수확 시 저장된 io_u */
}

/*
 * [한국어]
 * fio_sgio_read_capacity - READ CAPACITY(10/16)로 블록 크기와 최대 LBA 조회
 * @td: 잡
 * @bs: [out] 논리 블록 크기
 * @max_lba: [out] 최대 LBA(0-기반 마지막 LBA)
 * @return: 0 성공 / errno 또는 EIO
 * get_file_size()에서도 호출되기 때문에 sgio_data가 아직 없을 수 있어 지역 변수로만 동작한다.
 * 10B 시도 후 LBA=0xFFFFFFFF이면 16B로 재시도.
 * 호출 체인: fio_sgio_type_check()/fio_sgio_get_file_size() → [이 함수] → ioctl(SG_IO)
 */
static int fio_sgio_read_capacity(struct thread_data *td, unsigned int *bs,
				  unsigned long long *max_lba)
{
	/*
	 * need to do read capacity operation w/o benefit of sd or
	 * io_u structures, which are not initialized until later.
	 */
	struct sg_io_hdr hdr;                     /* [한국어] 지역 hdr */
	unsigned long long hlba;                  /* [한국어] 임시 LBA */
	unsigned int blksz = 0;                   /* [한국어] 임시 블록 크기 */
	unsigned char cmd[16];                    /* [한국어] CDB */
	unsigned char sb[64];                     /* [한국어] sense buffer */
	unsigned char buf[32];  // read capacity return /* [한국어] RCAP 응답 */
	int ret;
	int fd = -1;

	struct fio_file *f = td->files[0];        /* [한국어] 첫 번째 파일만 대표로 사용 */

	/* open file independent of rest of application */
	fd = open(f->file_name, O_RDONLY);        /* [한국어] 독립 open — 아직 init 전일 수 있음 */
	if (fd < 0)
		return -errno;

	memset(&hdr, 0, sizeof(hdr));             /* [한국어] 필드 초기화 */
	memset(cmd, 0, sizeof(cmd));
	memset(sb, 0, sizeof(sb));
	memset(buf, 0, sizeof(buf));

	/* First let's try a 10 byte read capacity. */
	hdr.interface_id = 'S';                   /* [한국어] SG v3 마커 */
	hdr.cmdp = cmd;
	hdr.cmd_len = 10;                         /* [한국어] READ CAPACITY(10) */
	hdr.sbp = sb;
	hdr.mx_sb_len = sizeof(sb);
	hdr.timeout = SCSI_TIMEOUT_MS;
	hdr.cmdp[0] = 0x25;  // Read Capacity(10)
	hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	hdr.dxferp = buf;
	hdr.dxfer_len = sizeof(buf);

	ret = ioctl(fd, SG_IO, &hdr);             /* [한국어] RCAP(10) 실행 */
	if (ret < 0) {
		close(fd);
		return ret;
	}

	if (hdr.info & SG_INFO_CHECK) {
		/* RCAP(10) might be unsupported by device. Force RCAP(16) */
		hlba = MAX_10B_LBA;                     /* [한국어] 강제로 16B 재시도 플래그 */
	} else {
		blksz = sgio_get_be32(&buf[4]);         /* [한국어] block size @ buf[4..7] */
		hlba = sgio_get_be32(buf);              /* [한국어] max LBA @ buf[0..3] */
	}

	/*
	 * If max lba masked by MAX_10B_LBA equals MAX_10B_LBA,
	 * then need to retry with 16 byte Read Capacity command.
	 */
	if (hlba == MAX_10B_LBA) {                /* [한국어] 32b 표현 포화 → 16B 버전 필요 */
		hdr.cmd_len = 16;
		hdr.cmdp[0] = 0x9e; // service action
		hdr.cmdp[1] = 0x10; // Read Capacity(16) /* [한국어] service action = 10h */
		sgio_set_be32(sizeof(buf), &hdr.cmdp[10]); /* [한국어] allocation length */

		hdr.dxfer_direction = SG_DXFER_FROM_DEV;
		hdr.dxferp = buf;
		hdr.dxfer_len = sizeof(buf);

		ret = ioctl(fd, SG_IO, &hdr);
		if (ret < 0) {
			close(fd);
			return ret;
		}

		/* record if an io error occurred */
		if (hdr.info & SG_INFO_CHECK)
			td_verror(td, EIO, "fio_sgio_read_capacity");

		blksz = sgio_get_be32(&buf[8]);         /* [한국어] RCAP(16): bs @ buf[8..11] */
		hlba = sgio_get_be64(buf);              /* [한국어] RCAP(16): max LBA @ buf[0..7] */
	}

	if (blksz) {                              /* [한국어] 정상 응답 */
		*bs = blksz;
		*max_lba = hlba;
		ret = 0;
	} else {
		ret = EIO;                              /* [한국어] 블록 크기 0 = 비정상 */
	}

	close(fd);
	return ret;
}

/*
 * [한국어]
 * fio_sgio_cleanup - td별 SG 엔진 자원 해제 (.cleanup 콜백)
 * @td: 잡
 * 모든 trim_queues, 할당된 배열, 구조체를 역순으로 free.
 * 호출 체인: td_io_cleanup() → [이 함수]
 */
static void fio_sgio_cleanup(struct thread_data *td)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] 할당된 상태 */
	int i;

	if (sd) {
		free(sd->events);                       /* [한국어] 완료 이벤트 배열 */
		free(sd->cmds);                         /* [한국어] CDB/sense 풀 */
		free(sd->fd_flags);                     /* [한국어] fcntl 백업 */
		free(sd->pfds);                         /* [한국어] pollfd 배열 */
		free(sd->sgbuf);                        /* [한국어] hdr 수신 버퍼 */
#ifdef FIO_SGIO_DEBUG
		free(sd->trim_queue_map);
#endif

		for (i = 0; i < td->o.iodepth; i++) {   /* [한국어] 각 TRIM 슬롯 */
			free(sd->trim_queues[i]->unmap_param); /* [한국어] UNMAP 파라미터 버퍼 */
			free(sd->trim_queues[i]->trim_io_us);  /* [한국어] io_u 추적 배열 */
			free(sd->trim_queues[i]);              /* [한국어] 구조체 자체 */
		}

		free(sd->trim_queues);                  /* [한국어] 최상위 포인터 배열 */
		free(sd);                               /* [한국어] 컨테이너 */
	}
}

/*
 * [한국어]
 * fio_sgio_init - SG 엔진 초기화 (.init 콜백)
 * @td: 잡
 * @return: 0 성공
 * iodepth/nr_files 기반으로 모든 내부 버퍼를 calloc하고 td->io_ops_data에 저장.
 * override_sync=1로 설정하여 sync_io 옵션 유무와 관계없이 SG 엔진 자체 동기화 정책을 적용.
 * 호출 체인: td_io_init() → [이 함수]
 */
static int fio_sgio_init(struct thread_data *td)
{
	struct sgio_data *sd;                     /* [한국어] 새 엔진 상태 */
	struct sgio_trim *st;                     /* [한국어] TRIM 슬롯 초기화용 */
	struct sg_io_hdr *h3p;                    /* [한국어] sgbuf 내 hdr 초기화용 반복자 */
	int i;

	sd = calloc(1, sizeof(*sd));              /* [한국어] 상태 할당 */
	sd->cmds = calloc(td->o.iodepth, sizeof(struct sgio_cmd));        /* [한국어] 명령 풀 */
	sd->sgbuf = calloc(td->o.iodepth, sizeof(struct sg_io_hdr));      /* [한국어] hdr 수신 버퍼 */
	sd->events = calloc(td->o.iodepth, sizeof(struct io_u *));        /* [한국어] 완료 이벤트 */
	sd->pfds = calloc(td->o.nr_files, sizeof(struct pollfd));         /* [한국어] poll fd */
	sd->fd_flags = calloc(td->o.nr_files, sizeof(int));               /* [한국어] fcntl 백업 */
	sd->type_checked = 0;                     /* [한국어] 아직 타입 체크 미실행 */

	sd->trim_queues = calloc(td->o.iodepth, sizeof(struct sgio_trim *)); /* [한국어] TRIM 큐 포인터 배열 */
	sd->current_queue = -1;                   /* [한국어] 누적 중 큐 없음 */
#ifdef FIO_SGIO_DEBUG
	sd->trim_queue_map = calloc(td->o.iodepth, sizeof(int));
#endif
	for (i = 0, h3p = sd->sgbuf; i < td->o.iodepth; i++, ++h3p) { /* [한국어] 각 슬롯 초기화 */
		sd->trim_queues[i] = calloc(1, sizeof(struct sgio_trim));
		st = sd->trim_queues[i];
		st->unmap_param = calloc(td->o.iodepth + 1, sizeof(char[16])); /* [한국어] 헤더+범위 최대 iodepth */
		st->unmap_range_count = 0;
		st->trim_io_us = calloc(td->o.iodepth, sizeof(struct io_u *));
		h3p->interface_id = 'S';                /* [한국어] 수신 hdr 기본값 */
	}

	td->io_ops_data = sd;                     /* [한국어] 엔진 상태 포인터 저장 */

	/*
	 * we want to do it, regardless of whether odirect is set or not
	 */
	td->o.override_sync = 1;                  /* [한국어] fio가 자동으로 sync 조작하지 않도록 */
	return 0;
}

/*
 * [한국어]
 * fio_sgio_type_check - 파일 종류 검증 및 블록 크기/opcode 폭 결정
 * @td: 잡
 * @f: 첫 번째 열린 파일
 * @return: 0 성공 / 1 실패
 * 블록 디바이스는 BLKSSZGET으로 bs 조회하고 비동기 콜백들을 비활성화(commit/getevents/event=NULL).
 * 캐릭터 디바이스는 SG_GET_VERSION_NUM으로 SG 지원 확인 후 READ CAPACITY.
 * 호출 체인: fio_sgio_open() → [이 함수] → ioctl(BLKSSZGET)/ioctl(SG_GET_VERSION_NUM)/fio_sgio_read_capacity()
 */
static int fio_sgio_type_check(struct thread_data *td, struct fio_file *f)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] 엔진 상태 */
	unsigned int bs = 0;                      /* [한국어] 조회 결과 bs */
	unsigned long long max_lba = 0;           /* [한국어] 조회 결과 max LBA */

	if (f->filetype == FIO_TYPE_BLOCK) {      /* [한국어] /dev/sdX */
		if (ioctl(f->fd, BLKSSZGET, &bs) < 0) { /* [한국어] 블록 크기 조회 ioctl */
			td_verror(td, errno, "ioctl");
			return 1;
		}
	} else if (f->filetype == FIO_TYPE_CHAR) { /* [한국어] /dev/sgN */
		int version, ret;

		if (ioctl(f->fd, SG_GET_VERSION_NUM, &version) < 0) { /* [한국어] SG 버전 확인 */
			td_verror(td, errno, "ioctl");
			return 1;
		}

		ret = fio_sgio_read_capacity(td, &bs, &max_lba); /* [한국어] RCAP으로 bs + max_lba */
		if (ret) {
			td_verror(td, td->error, "fio_sgio_read_capacity");
			log_err("ioengine sg unable to read capacity successfully\n");
			return 1;
		}
	} else {
		td_verror(td, EINVAL, "wrong file type"); /* [한국어] 블록/캐릭터 외 지원 안 함 */
		log_err("ioengine sg only works on block or character devices\n");
		return 1;
	}

	sd->bs = bs;                              /* [한국어] 엔진 상태에 저장 */
	// Determine size of commands needed based on max_lba
	if (max_lba >= MAX_10B_LBA) {             /* [한국어] 32b LBA 초과 디바이스 */
		dprint(FD_IO, "sgio_type_check: using 16 byte read/write "
			"commands for lba above 0x%016llx/0x%016llx\n",
			MAX_10B_LBA, max_lba);
	}

	if (f->filetype == FIO_TYPE_BLOCK) {      /* [한국어] 블록 디바이스는 동기만 지원 */
		td->io_ops->getevents = NULL;
		td->io_ops->event = NULL;
		td->io_ops->commit = NULL;
		/*
		** Setting these functions to null may cause problems
		** with filename=/dev/sda:/dev/sg0 since we are only
		** considering a single file
		*/
	}
	sd->type_checked = 1;                     /* [한국어] 1회만 수행 */

	return 0;
}

/*
 * [한국어]
 * fio_sgio_stream_control - STREAM CONTROL(16)로 스트림 오픈/클로즈
 * @f: 파일
 * @open_stream: true=open(0x34), false=close(0x54)
 * @stream_id: [in/out] open 시 출력, close 시 입력
 * @return: 0 성공 / <0 ioctl 오류 / 1 SCSI 에러
 * 호출 체인: fio_sgio_open()/close() → [이 함수] → ioctl(SG_IO)
 */
static int fio_sgio_stream_control(struct fio_file *f, bool open_stream, uint16_t *stream_id)
{
	struct sg_io_hdr hdr;                     /* [한국어] 지역 hdr */
	unsigned char cmd[16];                    /* [한국어] CDB 16B */
	unsigned char sb[64];                     /* [한국어] sense */
	unsigned char buf[8];                     /* [한국어] 응답 버퍼 */
	int ret;

	memset(&hdr, 0, sizeof(hdr));             /* [한국어] 초기화 */
	memset(cmd, 0, sizeof(cmd));
	memset(sb, 0, sizeof(sb));
	memset(buf, 0, sizeof(buf));

	hdr.interface_id = 'S';                   /* [한국어] SG v3 */
	hdr.cmdp = cmd;
	hdr.cmd_len = 16;
	hdr.sbp = sb;
	hdr.mx_sb_len = sizeof(sb);
	hdr.timeout = SCSI_TIMEOUT_MS;
	hdr.cmdp[0] = 0x9e;                       /* [한국어] service action in */
	hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	hdr.dxferp = buf;
	hdr.dxfer_len = sizeof(buf);
	sgio_set_be32(sizeof(buf), &hdr.cmdp[10]);/* [한국어] allocation length */

	if (open_stream)
		hdr.cmdp[1] = 0x34;                     /* [한국어] OPEN STREAM service action */
	else {
		hdr.cmdp[1] = 0x54;                     /* [한국어] CLOSE STREAM service action */
		sgio_set_be16(*stream_id, &hdr.cmdp[4]);/* [한국어] 닫을 스트림 ID */
	}

	ret = ioctl(f->fd, SG_IO, &hdr);          /* [한국어] 동기 실행 */

	if (ret < 0)
		return ret;

	if (hdr.info & SG_INFO_CHECK)             /* [한국어] SCSI 에러 */
		return 1;

	if (open_stream) {
		*stream_id = sgio_get_be16(&buf[4]);    /* [한국어] 할당된 스트림 ID 추출 */
		dprint(FD_FILE, "sgio_stream_control: opened stream %u\n", (unsigned int) *stream_id);
		assert(*stream_id != 0);                /* [한국어] 0은 "자동 모드" 예약값 */
	} else
		dprint(FD_FILE, "sgio_stream_control: closed stream %u\n", (unsigned int) *stream_id);

	return 0;
}

/*
 * [한국어]
 * fio_sgio_open - .open_file 콜백
 * @td: 잡, @f: 파일
 * @return: 0 성공 / 비0 실패
 * generic_open_file로 표준 open 수행 → 첫 파일에 대해 type_check 실행 →
 * WRITE STREAM 모드면 스트림 ID 설정(기본 0이면 OPEN STREAM으로 자동 할당).
 * 호출 체인: td_io_open_file() → [이 함수] → generic_open_file/fio_sgio_type_check/fio_sgio_stream_control
 */
static int fio_sgio_open(struct thread_data *td, struct fio_file *f)
{
	struct sgio_data *sd = td->io_ops_data;   /* [한국어] 엔진 상태(type_checked 확인용) */
	struct sg_options *o = td->eo;            /* [한국어] 엔진 옵션 */
	int ret;

	ret = generic_open_file(td, f);           /* [한국어] 공통 open 로직(O_RDWR 등) */
	if (ret)
		return ret;

	if (sd && !sd->type_checked && fio_sgio_type_check(td, f)) { /* [한국어] 최초 1회 타입 검사 */
		ret = generic_close_file(td, f);        /* [한국어] 실패 시 롤백 close */
		return ret;
	}

	if (o->write_mode == FIO_SG_WRITE_STREAM) { /* [한국어] WRITE STREAM 모드만 스트림 처리 */
		if (o->stream_id)                       /* [한국어] 명시적 스트림 ID */
			f->engine_pos = o->stream_id;         /* [한국어] 엔진별 보조 저장소에 저장(prep에서 사용) */
		else {
			ret = fio_sgio_stream_control(f, true, (uint16_t *) &f->engine_pos); /* [한국어] 자동 할당 */
			if (ret)
				return ret;
		}
	}

	return 0;
}

/*
 * [한국어]
 * fio_sgio_close - .close_file 콜백
 * @td: 잡, @f: 파일
 * 자동 할당된 스트림이라면 CLOSE STREAM으로 반환 후 표준 close.
 * 호출 체인: td_io_close_file() → [이 함수] → fio_sgio_stream_control/generic_close_file
 */
static int fio_sgio_close(struct thread_data *td, struct fio_file *f)
{
	struct sg_options *o = td->eo;            /* [한국어] 옵션 */
	int ret;

	if (!o->stream_id && o->write_mode == FIO_SG_WRITE_STREAM) { /* [한국어] 자동 할당했던 경우만 */
		ret = fio_sgio_stream_control(f, false, (uint16_t *) &f->engine_pos); /* [한국어] 스트림 해제 */
		if (ret)
			return ret;
	}

	return generic_close_file(td, f);         /* [한국어] 공통 close */
}

/*
 * Build an error string with details about the driver, host or scsi
 * error contained in the sg header Caller will use as necessary.
 */
/*
 * [한국어]
 * fio_sgio_errdetails - sg_io_hdr 오류 상세 정보를 동적 할당 문자열로 포맷
 * @td: 잡 (미사용)
 * @io_u: 오류가 발생한 io_u (io_u->hdr에 저장된 sg_io_hdr 사용)
 * @return: 힙에 할당된 문자열 (호출자가 free 책임)
 *
 * host_status/driver_status/SCSI status/sense data/resid/CDB를 모두 사람이 읽을
 * 수 있는 형태로 변환. SG_INFO_CHECK가 없으면 "SG Driver did not report..." 메시지 반환.
 * 호출 체인: td_io_errdetails() → [이 함수]
 */
static char *fio_sgio_errdetails(struct thread_data *td, struct io_u *io_u)
{
	struct sg_io_hdr *hdr = &io_u->hdr;       /* [한국어] 완료 시 복사된 hdr */
#define MAXERRDETAIL 1024                     /* [한국어] 최종 메시지 버퍼 크기 */
#define MAXMSGCHUNK  128                      /* [한국어] snprintf 작업 버퍼 */
	char *msg, msgchunk[MAXMSGCHUNK];
	int i;

	msg = calloc(1, MAXERRDETAIL);            /* [한국어] 0으로 초기화된 결과 버퍼 */
	strcpy(msg, "");                          /* [한국어] 빈 문자열 시작(명시적) */

	/*
	 * can't seem to find sg_err.h, so I'll just echo the define values
	 * so others can search on internet to find clearer clues of meaning.
	 */
	if (hdr->info & SG_INFO_CHECK) {          /* [한국어] 오류 정보 있음 */
		if (hdr->host_status) {                 /* [한국어] HBA/전송 계층 오류 */
			snprintf(msgchunk, MAXMSGCHUNK, "SG Host Status: 0x%02x; ", hdr->host_status);
			strlcat(msg, msgchunk, MAXERRDETAIL);
			switch (hdr->host_status) {
			case 0x01: strlcat(msg, "SG_ERR_DID_NO_CONNECT", MAXERRDETAIL); break;    /* [한국어] 연결 실패 */
			case 0x02: strlcat(msg, "SG_ERR_DID_BUS_BUSY", MAXERRDETAIL); break;      /* [한국어] 버스 사용 중 */
			case 0x03: strlcat(msg, "SG_ERR_DID_TIME_OUT", MAXERRDETAIL); break;      /* [한국어] 타임아웃 */
			case 0x04: strlcat(msg, "SG_ERR_DID_BAD_TARGET", MAXERRDETAIL); break;    /* [한국어] 잘못된 타깃 */
			case 0x05: strlcat(msg, "SG_ERR_DID_ABORT", MAXERRDETAIL); break;         /* [한국어] 중단 */
			case 0x06: strlcat(msg, "SG_ERR_DID_PARITY", MAXERRDETAIL); break;        /* [한국어] 패리티 에러 */
			case 0x07: strlcat(msg, "SG_ERR_DID_ERROR (internal error)", MAXERRDETAIL); break; /* [한국어] 내부 에러 */
			case 0x08: strlcat(msg, "SG_ERR_DID_RESET", MAXERRDETAIL); break;         /* [한국어] 리셋 발생 */
			case 0x09: strlcat(msg, "SG_ERR_DID_BAD_INTR (unexpected)", MAXERRDETAIL); break; /* [한국어] 예기치 못한 인터럽트 */
			case 0x0a: strlcat(msg, "SG_ERR_DID_PASSTHROUGH", MAXERRDETAIL); break;   /* [한국어] 패스스루 에러 */
			case 0x0b: strlcat(msg, "SG_ERR_DID_SOFT_ERROR (driver retry?)", MAXERRDETAIL); break; /* [한국어] 소프트 에러 */
			case 0x0c: strlcat(msg, "SG_ERR_DID_IMM_RETRY", MAXERRDETAIL); break;     /* [한국어] 즉시 재시도 */
			case 0x0d: strlcat(msg, "SG_ERR_DID_REQUEUE", MAXERRDETAIL); break;       /* [한국어] 재큐잉 */
			case 0x0e: strlcat(msg, "SG_ERR_DID_TRANSPORT_DISRUPTED", MAXERRDETAIL); break; /* [한국어] 전송 중단 */
			case 0x0f: strlcat(msg, "SG_ERR_DID_TRANSPORT_FAILFAST", MAXERRDETAIL); break;  /* [한국어] 전송 실패(즉시) */
			case 0x10: strlcat(msg, "SG_ERR_DID_TARGET_FAILURE", MAXERRDETAIL); break;      /* [한국어] 타깃 실패 */
			case 0x11: strlcat(msg, "SG_ERR_DID_NEXUS_FAILURE", MAXERRDETAIL); break;       /* [한국어] 넥서스 실패 */
			case 0x12: strlcat(msg, "SG_ERR_DID_ALLOC_FAILURE", MAXERRDETAIL); break;       /* [한국어] 자원 할당 실패 */
			case 0x13: strlcat(msg, "SG_ERR_DID_MEDIUM_ERROR", MAXERRDETAIL); break;        /* [한국어] 매체 오류 */
			default:   strlcat(msg, "Unknown", MAXERRDETAIL); break;                        /* [한국어] 미정의 코드 */
			}
			strlcat(msg, ". ", MAXERRDETAIL);
		}
		if (hdr->driver_status) {               /* [한국어] 드라이버 계층 오류 */
			snprintf(msgchunk, MAXMSGCHUNK, "SG Driver Status: 0x%02x; ", hdr->driver_status);
			strlcat(msg, msgchunk, MAXERRDETAIL);
			switch (hdr->driver_status & 0x0F) { /* [한국어] 하위 니블: 실패 원인 */
			case 0x01: strlcat(msg, "SG_ERR_DRIVER_BUSY", MAXERRDETAIL); break;     /* [한국어] 드라이버 바쁨 */
			case 0x02: strlcat(msg, "SG_ERR_DRIVER_SOFT", MAXERRDETAIL); break;     /* [한국어] 소프트 에러 */
			case 0x03: strlcat(msg, "SG_ERR_DRIVER_MEDIA", MAXERRDETAIL); break;    /* [한국어] 매체 에러 */
			case 0x04: strlcat(msg, "SG_ERR_DRIVER_ERROR", MAXERRDETAIL); break;    /* [한국어] 일반 에러 */
			case 0x05: strlcat(msg, "SG_ERR_DRIVER_INVALID", MAXERRDETAIL); break;  /* [한국어] 잘못된 요청 */
			case 0x06: strlcat(msg, "SG_ERR_DRIVER_TIMEOUT", MAXERRDETAIL); break;  /* [한국어] 타임아웃 */
			case 0x07: strlcat(msg, "SG_ERR_DRIVER_HARD", MAXERRDETAIL); break;     /* [한국어] 하드 에러 */
			case 0x08: strlcat(msg, "SG_ERR_DRIVER_SENSE", MAXERRDETAIL); break;    /* [한국어] sense data 유효 */
			default:   strlcat(msg, "Unknown", MAXERRDETAIL); break;
			}
			strlcat(msg, "; ", MAXERRDETAIL);
			switch (hdr->driver_status & 0xF0) { /* [한국어] 상위 니블: 권고 동작 */
			case 0x10: strlcat(msg, "SG_ERR_SUGGEST_RETRY", MAXERRDETAIL); break;   /* [한국어] 재시도 권고 */
			case 0x20: strlcat(msg, "SG_ERR_SUGGEST_ABORT", MAXERRDETAIL); break;   /* [한국어] 중단 권고 */
			case 0x30: strlcat(msg, "SG_ERR_SUGGEST_REMAP", MAXERRDETAIL); break;   /* [한국어] 리맵 권고 */
			case 0x40: strlcat(msg, "SG_ERR_SUGGEST_DIE", MAXERRDETAIL); break;     /* [한국어] 중단 권고 */
			case 0x80: strlcat(msg, "SG_ERR_SUGGEST_SENSE", MAXERRDETAIL); break;   /* [한국어] sense 확인 권고 */
			}
			strlcat(msg, ". ", MAXERRDETAIL);
		}
		if (hdr->status) {                      /* [한국어] SCSI 상태 코드 */
			snprintf(msgchunk, MAXMSGCHUNK, "SG SCSI Status: 0x%02x; ", hdr->status);
			strlcat(msg, msgchunk, MAXERRDETAIL);
			// SCSI 3 status codes
			switch (hdr->status) {
			case 0x02: strlcat(msg, "CHECK_CONDITION", MAXERRDETAIL); break;            /* [한국어] sense data 있음 */
			case 0x04: strlcat(msg, "CONDITION_MET", MAXERRDETAIL); break;              /* [한국어] 조건 충족 */
			case 0x08: strlcat(msg, "BUSY", MAXERRDETAIL); break;                       /* [한국어] 바쁨 */
			case 0x10: strlcat(msg, "INTERMEDIATE", MAXERRDETAIL); break;               /* [한국어] 중간 상태 */
			case 0x14: strlcat(msg, "INTERMEDIATE_CONDITION_MET", MAXERRDETAIL); break; /* [한국어] 중간 조건 충족 */
			case 0x18: strlcat(msg, "RESERVATION_CONFLICT", MAXERRDETAIL); break;       /* [한국어] 예약 충돌 */
			case 0x22: strlcat(msg, "COMMAND_TERMINATED", MAXERRDETAIL); break;         /* [한국어] 명령 종료 */
			case 0x28: strlcat(msg, "TASK_SET_FULL", MAXERRDETAIL); break;              /* [한국어] 큐 가득참 */
			case 0x30: strlcat(msg, "ACA_ACTIVE", MAXERRDETAIL); break;                 /* [한국어] ACA 활성 */
			case 0x40: strlcat(msg, "TASK_ABORTED", MAXERRDETAIL); break;               /* [한국어] 태스크 중단 */
			default:   strlcat(msg, "Unknown", MAXERRDETAIL); break;
			}
			strlcat(msg, ". ", MAXERRDETAIL);
		}
		if (hdr->sb_len_wr) {                   /* [한국어] sense data 유효 */
			const uint8_t *const sbp = hdr->sbp;

			snprintf(msgchunk, MAXMSGCHUNK, "Sense Data (%d bytes):", hdr->sb_len_wr);
			strlcat(msg, msgchunk, MAXERRDETAIL);
			for (i = 0; i < hdr->sb_len_wr; i++) { /* [한국어] sense 바이트 헥스 덤프 */
				snprintf(msgchunk, MAXMSGCHUNK, " %02x", sbp[i]);
				strlcat(msg, msgchunk, MAXERRDETAIL);
			}
			strlcat(msg, ". ", MAXERRDETAIL);
		}
		if (hdr->resid != 0) {                  /* [한국어] 미전송 바이트 있음 */
			snprintf(msgchunk, MAXMSGCHUNK, "SG Driver: %d bytes out of %d not transferred. ", hdr->resid, hdr->dxfer_len);
			strlcat(msg, msgchunk, MAXERRDETAIL);
		}
		if (hdr->cmdp) {                        /* [한국어] CDB 덤프 */
			strlcat(msg, "cdb:", MAXERRDETAIL);
			for (i = 0; i < hdr->cmd_len; i++) {
				snprintf(msgchunk, MAXMSGCHUNK, " %02x", hdr->cmdp[i]);
				strlcat(msg, msgchunk, MAXERRDETAIL);
			}
			strlcat(msg, ". ", MAXERRDETAIL);
			if (io_u->ddir == DDIR_TRIM) {        /* [한국어] TRIM이면 UNMAP parameter list도 덤프 */
				unsigned char *param_list = hdr->dxferp;
				strlcat(msg, "dxferp:", MAXERRDETAIL);
				for (i = 0; i < hdr->dxfer_len; i++) {
					snprintf(msgchunk, MAXMSGCHUNK, " %02x", param_list[i]);
					strlcat(msg, msgchunk, MAXERRDETAIL);
				}
				strlcat(msg, ". ", MAXERRDETAIL);
			}
		}
	}

	if (!(hdr->info & SG_INFO_CHECK) && !strlen(msg)) /* [한국어] 오류 플래그도 없고 메시지도 없음 */
		snprintf(msg, MAXERRDETAIL, "%s",
			 "SG Driver did not report a Host, Driver or Device check");

	return msg;                               /* [한국어] 호출자가 free 책임 */
}

/*
 * get max file size from read capacity.
 */
/*
 * [한국어]
 * fio_sgio_get_file_size - .get_file_size 콜백
 * @td: 잡, @f: 파일
 * @return: 0 성공 / 1 실패
 * init 전에 호출될 수 있어 RCAP을 지역 변수로만 수행. 결과를 f->real_file_size에 저장.
 * 호출 체인: td_io_get_file_size() → [이 함수] → fio_sgio_read_capacity()
 */
static int fio_sgio_get_file_size(struct thread_data *td, struct fio_file *f)
{
	/*
	 * get_file_size is being called even before sgio_init is
	 * called, so none of the sg_io structures are
	 * initialized in the thread_data yet.  So we need to do the
	 * ReadCapacity without any of those helpers.  One of the effects
	 * is that ReadCapacity may get called 4 times on each open:
	 * readcap(10) followed by readcap(16) if needed - just to get
	 * the file size after the init occurs - it will be called
	 * again when "type_check" is called during structure
	 * initialization I'm not sure how to prevent this little
	 * inefficiency.
	 */
	unsigned int bs = 0;                      /* [한국어] 블록 크기 */
	unsigned long long max_lba = 0;           /* [한국어] 최대 LBA */
	int ret;

	if (fio_file_size_known(f))               /* [한국어] 이미 알고 있으면 */
		return 0;                               /* [한국어] 스킵 */

	if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR) { /* [한국어] 지원 타입 검사 */
		td_verror(td, EINVAL, "wrong file type");
		log_err("ioengine sg only works on block or character devices\n");
		return 1;
	}

	ret = fio_sgio_read_capacity(td, &bs, &max_lba); /* [한국어] RCAP 실행 */
	if (ret ) {
		td_verror(td, td->error, "fio_sgio_read_capacity");
		log_err("ioengine sg unable to successfully execute read capacity to get block size and maximum lba\n");
		return 1;
	}

	f->real_file_size = (max_lba + 1) * bs;   /* [한국어] 총 용량 = (max_lba+1) × bs */
	fio_file_set_size_known(f);               /* [한국어] 크기 확정 플래그 세팅 */
	return 0;
}


/* [한국어] === SG I/O 엔진 등록 테이블 (ioengine_ops, 엔진 vtable) ===
 * fio_sgio_register()(fio_init 생성자)가 이 구조체의 주소를 register_ioengine()에
 * 전달하여 전역 ioengine 리스트(flist)에 추가한다. 사용자가 --ioengine=sg 또는
 * jobspec에서 ioengine=sg를 지정하면 fio 코어(ioengines.c)가 .name으로 lookup하여
 * 본 콜백들로 td_io_* 매크로(td_io_init/td_io_prep/td_io_queue/td_io_commit/
 * td_io_getevents/td_io_event/td_io_open_file/td_io_close_file/td_io_get_file_size/
 * td_io_errdetails/td_io_cleanup)를 구동한다. */
static struct ioengine_ops ioengine = {
	.name		= "sg",
	/* [한국어] 엔진 식별자(고유 문자열).
	 * 설정자: 본 정적 초기화. 읽는 자: load_ioengine() lookup, --help 출력.
	 * 값 범위: NUL 종단 ASCII. 동기화: 전역 리스트 수정은 register_ioengine 진입점에서만. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] 엔진 ABI 버전 — fio 코어가 등록 시 자신의 빌드 버전과 일치하는지 확인.
	 * 설정자: 빌드 시 fio.h 매크로로 결정(fio v3.42 기준 특정 정수).
	 * 읽는 자: register_ioengine()가 mismatch 시 거부.
	 * 값 범위: fio 빌드 버전 정수. 동기화: 컴파일 타임 상수. */

	.init		= fio_sgio_init,
	/* [한국어] td별 엔진 초기화 콜백. iodepth × sgio_cmd, sgio_trim 큐 풀 할당.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_init()이 잡 시작 시 호출.
	 * 호출 시점: 잡 스레드 시작 직후, open_file 이전.
	 * 반환: 0=성공, !=0=잡 실패. */

	.prep		= fio_sgio_prep,
	/* [한국어] io_u → SCSI CDB 인코딩 콜백.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_prep()이 매 io_u 발행 직전 호출.
	 * 호출 시점: get_io_u() 후 td_io_queue() 호출 직전.
	 * 반환: 0=성공, EINVAL 등=실패. */

	.queue		= fio_sgio_queue,
	/* [한국어] I/O 제출 콜백. 동기/비동기/TRIM 분기 후 ioctl(SG_IO) 또는 write(2).
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_queue()가 매 io_u마다 호출.
	 * 호출 시점: prep() 직후. 반환: FIO_Q_COMPLETED(즉시 완료) / FIO_Q_QUEUED(비동기
	 * 큐잉, getevents 대기) / FIO_Q_BUSY(현재 미사용). */

	.commit		= fio_sgio_commit,
	/* [한국어] 누적된 비동기 TRIM 일괄 제출 콜백.
	 * 설정자: 본 정적 초기화. 단, type_check가 BLOCK 디바이스에서 NULL로 덮어쓴다.
	 * 읽는 자: td_io_commit()이 큐잉 페이즈 종료 시 호출.
	 * 호출 시점: queue()로 누적된 TRIM이 있을 때.
	 * 반환: 0=성공, <0=에러. */

	.getevents	= fio_sgio_getevents,
	/* [한국어] 비동기 완료 수확 콜백. poll(2) + read(2)로 sg_io_hdr 회수.
	 * 설정자: 본 정적 초기화. 단, BLOCK 디바이스는 type_check가 NULL로 덮어씀.
	 * 읽는 자: td_io_getevents()가 io_u가 in-flight일 때만 호출.
	 * 반환: 수확 개수(>=0) 또는 음수 errno. */

	.errdetails	= fio_sgio_errdetails,
	/* [한국어] 에러 상세 문자열 생성 콜백 — verify 실패/IO 에러 출력 시 사용.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_errdetails()가 io_u->error 발생 시 호출.
	 * 반환: 힙 할당 문자열(호출자 free 책임). */

	.event		= fio_sgio_event,
	/* [한국어] 수확된 이벤트 인덱스 → io_u 변환 콜백.
	 * 설정자: 본 정적 초기화. 단, BLOCK 디바이스는 type_check가 NULL로 덮어씀.
	 * 읽는 자: td_io_event(idx)가 getevents 반환 후 idx in [0, count)로 호출.
	 * 반환: 해당 io_u 포인터(NULL 불가). */

	.cleanup	= fio_sgio_cleanup,
	/* [한국어] td별 엔진 자원 해제 콜백. 모든 calloc 풀을 free.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_cleanup()이 잡 종료 시 호출.
	 * 호출 시점: close_file 모두 완료 후. */

	.open_file	= fio_sgio_open,
	/* [한국어] 파일 오픈 콜백. generic_open_file + 1회성 type_check + STREAM open.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_open_file()이 각 파일별로 호출.
	 * 반환: 0=성공. */

	.close_file	= fio_sgio_close,
	/* [한국어] 파일 클로즈 콜백. 자동 할당된 STREAM 해제 + generic_close_file.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_close_file()이 호출.
	 * 반환: 0=성공. */

	.get_file_size	= fio_sgio_get_file_size,
	/* [한국어] 디바이스 크기 조회 콜백. RCAP(10/16) → real_file_size.
	 * 설정자: 본 정적 초기화. 읽는 자: td_io_get_file_size()가 init 전에도 호출 가능.
	 * 반환: 0=성공. */

	.flags		= FIO_SYNCIO | FIO_RAWIO | FIO_RO_NEEDS_RW_OPEN,
	/* [한국어] 엔진 동작 특성 비트마스크:
	 *   - FIO_SYNCIO          : queue() 진입 전 td_io_queue()가 issue_time을 기록해야
	 *                           함을 의미. 본 엔진은 SG_IO ioctl이 본질적으로 동기적
	 *                           이고, /dev/sgN write 경로도 발행 직후 즉시 반환되므로
	 *                           "queue 호출 = 발행"이라는 가정이 유효.
	 *   - FIO_RAWIO           : 파일시스템 위가 아닌 원시 디바이스만 다룸을 명시.
	 *                           filename=/dev/sdX 또는 /dev/sgN만 허용되며, 일반 파일
	 *                           대상으로 사용하면 type_check가 거부.
	 *   - FIO_RO_NEEDS_RW_OPEN: rw=read 워크로드라도 fd를 RW(O_RDWR)로 open해야 함.
	 *                           /dev/sgN에 write(2)로 명령을 제출하기 위해 fd가 RW여야
	 *                           하기 때문. /dev/sdX(BLOCK)는 ioctl만 쓰므로 무관.
	 * 미설정 비트 의미:
	 *   - FIO_DISKLESSIO      : 미설정 — 실제 디바이스 fd가 필요.
	 *   - FIO_NOEXTEND        : 미설정 — 디바이스 크기 변경 불가는 별도 처리.
	 *   - FIO_MEMALIGN        : 미설정 — fio가 SG 엔진의 메모리 정렬을 강제하지 않음
	 *                           (단, O_DIRECT 시 사용자가 dma_alignment 만족해야 함).
	 *   - FIO_PIPEIO/UNIDIR/BARRIER/FAKEIO : 미설정 — 일반 R/W 디바이스 엔진. */

	.options	= options,
	/* [한국어] 엔진 전용 옵션 테이블 포인터. fio 옵션 파서에 등록됨.
	 * 설정자: 본 정적 초기화. 읽는 자: parse_options/show_engine_help.
	 * 값 범위: NULL 종단 fio_option 배열. */

	.option_struct_size	= sizeof(struct sg_options)
	/* [한국어] 옵션 구조체 크기 — fio 코어가 td->eo 영역을 이 크기로 calloc.
	 * 설정자: 본 정적 초기화. 읽는 자: 옵션 파서가 td->eo 할당 시 사용.
	 * 값 범위: sizeof(struct sg_options). */
};

#else /* FIO_HAVE_SGIO */

/*
 * When we have a proper configure system in place, we simply wont build
 * and install this io engine. For now install a crippled version that
 * just complains and fails to load.
 */
/*
 * [한국어]
 * fio_sgio_init (스텁) - SG 미지원 환경에서 init 시도 시 에러 출력 후 실패
 * @td: 잡 (미사용)
 * @return: 1 (항상 실패)
 * 호출 체인: td_io_init() → [이 함수]
 */
static int fio_sgio_init(struct thread_data fio_unused *td)
{
	log_err("fio: ioengine sg not available\n"); /* [한국어] 사용자에게 불가 알림 */
	return 1;
}

/* [한국어] SG 미지원 환경용 최소 엔진 정의 — init만 존재하고 즉시 실패.
 * 이 stub 엔진은 "sg" 이름을 차지하여 --ioengine=sg 지정 시 parse 단계를 통과하지만,
 * init 단계에서 "fio: ioengine sg not available" 메시지와 함께 잡을 실패시킨다. */
static struct ioengine_ops ioengine = {
	.name		= "sg",
	/* [한국어] 엔진 이름(FIO_HAVE_SGIO 분기와 동일해야 함 — 사용자 혼동 방지). */
	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전(동일 fio 빌드이므로 동일 상수). */
	.init		= fio_sgio_init,
	/* [한국어] 즉시 실패 stub. 다른 콜백(prep/queue/...)은 미정의이므로 fio가
	 * init 실패를 보고 잡을 종료하므로 호출되지 않는다. */
};

#endif

/*
 * [한국어]
 * fio_sgio_register - 전역 ioengine 리스트에 SG 엔진 등록 (fio_init 생성자)
 *
 * @return: 없음
 *
 * GCC __attribute__((constructor)) 매크로(fio_init)로 표기되어 ELF 로더가 .init_array
 * 섹션을 처리하는 시점(즉, main() 진입 이전)에 자동 실행된다. fio가 정적 링크된 모든
 * I/O 엔진은 이 패턴으로 자기 ioengine_ops를 전역 리스트에 추가하므로, 사용자가
 * --ioengine=sg를 지정하면 ioengines.c의 load_ioengine()이 lookup만으로 발견할 수 있다.
 *
 * 실행 컨텍스트: 메인 프로세스, 단일 스레드(다른 엔진 생성자와 직렬 실행 — 리스트는
 * flist_add_tail로 단방향 추가만 하므로 락 불필요).
 *
 * 호출 체인:
 *   ld.so/ld-linux의 .init_array 처리 → [이 함수] → register_ioengine() → flist_add_tail(&ioengine.list)
 */
static void fio_init fio_sgio_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] 전역 ioengine 리스트(ioengines.c의 engine_list)에 본 엔진을 추가.
	 * register_ioengine()은 단순히 ioengine_ops.list 노드를 flist_add_tail로 enqueue. */
}

/*
 * [한국어]
 * fio_sgio_unregister - 엔진 등록 해제 (fio_exit 소멸자)
 *
 * @return: 없음
 *
 * GCC __attribute__((destructor)) 매크로(fio_exit)로 표기되어 ELF 로더가 .fini_array
 * 섹션을 처리하는 시점(프로세스 종료 직전, exit() / _Exit() 흐름의 atexit 체인)에
 * 자동 실행된다. 정적 링크 환경에서는 사실상 부수효과가 거의 없지만, 동적 링크
 * (dlopen으로 외부 엔진 로딩) 환경에서 안전한 dlclose를 위해 필요하다.
 *
 * 실행 컨텍스트: 메인 프로세스 종료 단계, 단일 스레드.
 *
 * 호출 체인:
 *   exit()/_exit()의 atexit/.fini_array → [이 함수] → unregister_ioengine() → flist_del_init
 */
static void fio_exit fio_sgio_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 리스트에서 본 엔진을 제거(flist_del_init). 이후 다시 register
	 * 가능하도록 list 노드를 빈 상태로 초기화. */
}
