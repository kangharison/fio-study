/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 * This file is released under the GPL.
 *
 * libzbc engine
 * IO engine using libzbc library to talk to SMR disks.
 */

/*
 * [한국어 설명] libzbc I/O 엔진 구현 (libzbc.c)
 *
 * === 파일의 역할 ===
 * 이 엔진은 Western Digital이 공개한 libzbc 유저스페이스 라이브러리를 통해 ZBC(Zoned
 * Block Command, SCSI T10)/ZAC(Zoned ATA Command) 표준을 따르는 "호스트 관리형(Host-
 * Managed)" 및 "호스트 인지형(Host-Aware)" SMR(Shingled Magnetic Recording) 드라이브로
 * I/O를 수행한다. 리눅스 블록 레이어의 /dev/sgX(scsi generic) 또는 /dev/sdX를 대상으로
 * zbc_open() → zbc_pread/pwrite/flush 경로를 통해 SCSI CDB(READ/WRITE/SYNCHRONIZE
 * CACHE)나 ATA 명령을 사실상 패스스루로 송신한다. 단순 RW 이외에도 fio ZBD(Zoned Block
 * Device) 프레임워크가 요구하는 메타 콜백 — 존 모델 질의, 존 리포트, 쓰기 포인터 리셋,
 * 존 FINISH, 존 WP 이동, 동시 오픈 존 수 한계 보고 — 를 모두 구현하여 존 규칙(순차 쓰기
 * 강제, open-zone limit)에 부합하도록 fio가 I/O 계획을 세울 수 있게 한다.
 *
 * ZBC/ZAC 존 모델 요약:
 *   - Conventional(CNV) : 기존 블록처럼 랜덤 RW 가능한 영역.
 *   - Sequential Write Required(SWR): 호스트 관리형 드라이브 전용. WP(Write Pointer)
 *     위치에만 쓰기 가능. 재사용하려면 zbc_reset_zone으로 WP를 zone start로 되돌려야 함.
 *   - Sequential Write Preferred(SWP): 호스트 인지형 드라이브. 순차 아닌 쓰기도 내부
 *     간접(indirect) 블록을 통해 허용되지만 성능 저하가 따름.
 * 존 상태(condition): EMPTY / IMP_OPEN / EXP_OPEN / CLOSED / FULL / READ_ONLY /
 * OFFLINE / NOT_WP. fio ZBD 코어는 report_zones로 이 상태 머신을 추적해 I/O를 재배치.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend → load_ioengine("libzbc") → td_io_init(구현 없음, 엔진 상태는 파일 오픈
 * 시 지연 생성) → get_zoned_model(ZBD 코어가 디바이스의 모델을 파악) → get_file_size
 * (디바이스 전체 크기 바이트) → open_file(zbc_open + 정보 취득) → report_zones(fio ZBD가
 * 존 맵을 구축) → [잡 루프 동안] queue(zbc_pread/pwrite/flush)와 reset_wp/finish_zone/
 * move_zone_wp를 혼용 → close_file → cleanup. FIO_SYNCIO 플래그이므로 queue()는 즉시
 * FIO_Q_COMPLETED를 돌려주며 fio 코어는 별도의 getevents/event 호출 없이 통계/레이턴시를
 * 바로 회계 처리한다. 실행 컨텍스트는 모두 해당 잡 스레드 단일 문맥이다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h         : thread_data, io_u, ioengine_ops, fio_ro_check, dprint, td_verror,
 *                   log_err, FIO_Q_COMPLETED, fio_q_status, FIO_TYPE_BLOCK/CHAR 등 공용.
 * - err.h         : ERR_PTR 계열(이 파일은 직접 사용하지 않으나 관용적으로 포함).
 * - zbd_types.h   : fio ZBD 프레임워크의 구조체(enum zbd_zoned_model, struct zbd_zone,
 *                   ZBD_ZONE_TYPE_CNV/SWR/SWP, ZBD_ZONE_COND_*). libzbc enum을 여기에
 *                   매핑하는 것이 report_zones 구현의 핵심.
 * - zbd.h         : zbd_do_io_u_trim 같은 fio ZBD 공용 헬퍼.
 * - libzbc/zbc.h  : zbc_open/zbc_close/zbc_get_device_info/zbc_report_zones/
 *                   zbc_reset_zone/zbc_finish_zone/zbc_open_zone/zbc_close_zone/
 *                   zbc_pread/zbc_pwrite/zbc_flush + zbc_errno/sense key 유틸.
 * - 공유 상태: td->io_ops_data = struct libzbc_data (잡 스레드 단독 소유, 동기화 없음).
 *   fio ZBD 프레임워크는 get_zoned_model/report_zones/reset_wp/finish_zone/move_zone_wp/
 *   get_max_open_zones를 ioengine_ops 콜백을 통해 이 엔진에 위임한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct libzbc_data        : zbc_device 핸들 + 장치 정보(model/nr_sectors/max_open).
 * - libzbc_get_dev_info()     : zbc_get_device_info 호출 결과를 ld에 캐시.
 * - libzbc_open_dev()         : O_DIRECT + ZBC_O_DRV_SCSI|ATA 플래그로 지연 오픈(이미
 *                               열려 있으면 기존 ld 반환; report_zones/get_zoned_model이
 *                               open_file 이전에 호출되는 경로 대비).
 * - libzbc_get_zoned_model()  : libzbc의 ZBC_DM_HOST_AWARE/HOST_MANAGED를 fio ZBD의
 *                               ZBD_HOST_AWARE/HOST_MANAGED/NONE enum으로 변환.
 * - libzbc_report_zones()     : zbc_report_zones → struct zbd_zone 배열(섹터→바이트
 *                               <<9 변환, 타입/상태 매핑, zone capacity=zone length).
 * - libzbc_reset_wp()/finish_zone()/move_zone_wp(): 존 상태 전이 제어 콜백.
 * - libzbc_rw()/libzbc_queue(): pread/pwrite/flush/trim ddir별 디스패치.
 * - libzbc_get_max_open_zones(): ZBC_NO_LIMIT → 0(무제한)로 변환해 fio에 보고.
 */

#include <stdlib.h>
/* [한국어] C 표준 라이브러리 — calloc/free/malloc: struct zbc_device_info·struct zbc_zone
 * 배열·struct libzbc_data 힙 할당·해제에 사용. 큰 구조체를 스택 프레임에 두지 않기 위함. */

#include <unistd.h>
/* [한국어] POSIX 표준 — 이 파일에서는 직접적인 POSIX syscall(read/write 등)을 쓰지 않지만
 * libzbc 내부가 pread/pwrite/lseek/close를 사용하므로 호환성 차원에서 포함. */

#include <errno.h>
/* [한국어] errno 및 E* 매크로 — zbc_* 호출 실패 시 libzbc가 설정하는 errno를
 * td_verror/log_err로 전파하기 위해 필요. EINVAL/EIO/ENOMEM 등 직접 반환값으로도 사용. */

#include <libzbc/zbc.h>
/* [한국어] libzbc 공개 API 헤더 — zbc_device/zbc_device_info/zbc_zone/zbc_errno 구조체,
 * ZBC_O_DRV_SCSI/ATA, ZBC_RO_ALL, ZBC_ZT_*/ZBC_ZC_*, ZBC_DM_HOST_AWARE/MANAGED,
 * ZBC_OP_ALL_ZONES, ZBC_NO_LIMIT 매크로, zbc_open/close/get_device_info/report_zones/
 * reset_zone/finish_zone/pread/pwrite/flush/errno/sk_str/asc_ascq_str 함수 원형 공급. */

#include "fio.h"
/* [한국어] fio 코어 헤더 — thread_data, io_u, fio_file, ioengine_ops, td_verror, log_err,
 * dprint(FD_ZBD,...), fio_ro_check, FIO_Q_COMPLETED, FIO_SYNCIO/NOEXTEND/RAWIO, FIO_TYPE_*,
 * fio_file_size_known/set_size_known, td_read/td_write/td_trim, ddir_sync, OS_O_DIRECT 등
 * 엔진 구현에 필요한 거의 모든 심볼을 간접 공급. */

#include "err.h"
/* [한국어] fio 공용 에러 포인터 매크로(ERR_PTR/IS_ERR/PTR_ERR) 공급. 본 파일에서
 * 직접 사용하지는 않지만 fio.h 공통 관행에 따라 포함. */

#include "zbd_types.h"
/* [한국어] fio ZBD 프레임워크의 타입 정의 — enum zbd_zoned_model(NONE/HOST_AWARE/HOST_MANAGED),
 * struct zbd_zone(start/len/capacity/wp/type/cond), ZBD_ZONE_TYPE_CNV/SWR/SWP,
 * ZBD_ZONE_COND_NOT_WP/EMPTY/IMP_OPEN/EXP_OPEN/CLOSED/FULL/OFFLINE 등. report_zones
 * 콜백이 libzbc 타입을 이 enum으로 번역하는 데 필수. */

#include "zbd.h"
/* [한국어] fio ZBD 공통 유틸 — zbd_do_io_u_trim(DDIR_TRIM 처리 시 존 reset으로 변환),
 * zbd 인프라가 ioengine_ops 콜백을 언제 어떻게 호출하는지의 계약을 정의한다. */

/*
 * [한국어] libzbc 엔진의 잡별(per-job) 상태. td->io_ops_data에 저장되어 queue/report_zones/
 * reset_wp/finish_zone/move_zone_wp/get_max_open_zones 등 모든 콜백이 공유한다.
 * 수명: 첫 콜백(open_file 또는 get_zoned_model 등 어느 것이 먼저 호출되든 libzbc_open_dev
 * 내부의 "지연 오픈" 경로)에서 calloc 생성, cleanup/close_file에서 해제.
 * 공유/동기화: 잡 스레드 단독 소유 — 락/atomic 불필요.
 */
struct libzbc_data {
	struct zbc_device	*zdev;
	/* [한국어] libzbc가 관리하는 디바이스 핸들(불투명 포인터 — 필드 직접 접근 금지).
	 * 설정자: libzbc_open_dev() 내부의 zbc_open() 성공 시 libzbc가 반환.
	 * 읽는 자: 모든 I/O(libzbc_rw), 모든 zone 제어(reset_wp/finish_zone/move_zone_wp),
	 *        정보 조회(libzbc_get_dev_info의 zbc_get_device_info), flush(zbc_flush).
	 * 값 범위: zbc_open 성공 시 유효 포인터, libzbc_close_dev 후 NULL. open 전 calloc(0)이
	 *        보장하는 NULL 상태.
	 * 동기화: 잡 스레드 전용. libzbc 내부는 핸들 단위로 상태를 가지므로 다른 스레드에서
	 *        같은 핸들을 공유해서는 안 된다. */

	enum zbc_dev_model	model;
	/* [한국어] libzbc가 보고하는 디바이스 모델 — 드라이브가 호스트 인지형(Host-Aware)인지
	 * 호스트 관리형(Host-Managed)인지 판별.
	 * 설정자: libzbc_get_dev_info()가 zbc_get_device_info 결과의 zbd_model 복사.
	 * 읽는 자: libzbc_get_zoned_model()이 fio ZBD enum(ZBD_HOST_AWARE/HOST_MANAGED/NONE)
	 *        으로 변환해 zbd 코어에 전달.
	 * 값 범위: ZBC_DM_HOST_AWARE, ZBC_DM_HOST_MANAGED, ZBC_DM_DRIVE_MANAGED, ZBC_DM_STANDARD.
	 *        후자 두 개는 zoning이 호스트에 노출되지 않으므로 ZBD_NONE으로 처리.
	 * 동기화: 오픈 직후 1회 설정 후 불변 — 잡 스레드 내에서만 읽기. */

	uint64_t		nr_sectors;
	/* [한국어] 디바이스 총 512B 논리 섹터 수. SCSI READ CAPACITY(16) 결과를 libzbc가 보관.
	 * 설정자: libzbc_get_dev_info()가 zbc_get_device_info의 zbd_sectors 복사.
	 * 읽는 자: libzbc_get_file_size()가 <<9로 바이트 환산해 f->real_file_size에 저장,
	 *        libzbc_report_zones()가 "offset이 디바이스 범위 밖이면 0개 리포트" 조기 종료
	 *        판정에 사용, libzbc_reset_wp()가 "전 범위 요청 감지 → ALL_ZONES 최적화".
	 * 값 범위: 0 < nr_sectors < 2^63. 2TiB 초과 드라이브를 위해 64비트.
	 * 동기화: 오픈 직후 1회 설정 후 불변. */

	uint32_t		max_open_seq_req;
	/* [한국어] 드라이브가 동시에 허용하는 오픈 상태(IMP_OPEN+EXP_OPEN) 순차 쓰기(SWR) 존의
	 * 최대 개수. ZBC/ZAC의 Open Zone Limit(OZL) 개념.
	 * 설정자: libzbc_get_dev_info()가 zbd_max_nr_open_seq_req 복사.
	 * 읽는 자: libzbc_get_max_open_zones()가 fio ZBD에 보고. fio는 이 값을 참고해 동시에
	 *        너무 많은 존을 열지 않도록 I/O 플래너를 제약한다.
	 * 값 범위: 0 ≤ N, 또는 ZBC_NO_LIMIT(무제한 표시값). ZBC_NO_LIMIT이면 0으로 변환해 보고.
	 * 동기화: 오픈 직후 1회 설정 후 불변. */
};

/*
 * [한국어]
 * libzbc_get_dev_info - libzbc에서 디바이스 정보(모델/섹터 수/최대 오픈 순차 존 수)를
 *                      조회해 libzbc_data에 캐시한다.
 *
 * @ld: 정보를 기록할 엔진 상태 구조체. zdev는 이미 열린 유효 핸들이어야 함.
 * @f:  로그 메시지에 표시할 파일명(file_name) 참조용. 필수 아님(로그 전용).
 * @return: 0 성공, -ENOMEM(zinfo 임시 버퍼 할당 실패). zbc_get_device_info 자체는 void
 *          계열이라 별도 실패 경로가 없다.
 *
 * 왜 필요한가: libzbc는 zbc_device_info를 힙 기반 별도 구조체로 반환하는데(해당 구조체가
 * 수백 바이트 규모), 엔진 상태에 필요한 세 필드만 뽑아내 간결한 struct libzbc_data에
 * 옮겨 담기 위해 중개자 역할을 한다. 또한 dprint(FD_ZBD)로 디바이스 식별 로그를 남겨
 * ZBD 디버깅을 돕는다.
 *
 * 실행 컨텍스트: 잡 스레드. libzbc_open_dev()가 zbc_open 직후 1회 호출. 재진입 없음.
 *
 * 호출 체인:
 *   libzbc_open_dev → [libzbc_get_dev_info] → zbc_get_device_info(libzbc)
 */
static int libzbc_get_dev_info(struct libzbc_data *ld, struct fio_file *f)
{
	struct zbc_device_info *zinfo;
	/* [한국어] libzbc가 제공하는 디바이스 정보 구조체 — vendor_id, 모델, 섹터 수,
	 * 블록 크기, 최대 오픈 존 수 등을 담는다. 크기가 커 스택이 아닌 힙에 할당. */

	zinfo = calloc(1, sizeof(*zinfo));
	/* [한국어] 힙에 0으로 초기화된 zbc_device_info 1개 할당. 스택 오버플로 방지 + 모든
	 * 필드를 기본 0으로 시작해 libzbc가 채우지 않는 필드도 안전. */
	if (!zinfo)
		return -ENOMEM;
		/* [한국어] 할당 실패 시 ENOMEM을 음수로 반환 — fio 관행: 음수 errno로 실패 전달. */

	zbc_get_device_info(ld->zdev, zinfo);
	/* [한국어] libzbc API: 이미 열린 zdev의 장치 정보(디바이스 타입, 모델, 벤더ID, 섹터
	 * 크기/수, 최대 오픈 존 수 등)를 zinfo에 복사. 반환값 없음(항상 성공). */
	ld->model = zinfo->zbd_model;
	/* [한국어] Host-Aware vs Host-Managed vs Drive-Managed 구분값을 ld에 캐시. */
	ld->nr_sectors = zinfo->zbd_sectors;
	/* [한국어] 512B 섹터 단위 총 크기. 파일 크기 계산(<<9)의 근거. */
	ld->max_open_seq_req = zinfo->zbd_max_nr_open_seq_req;
	/* [한국어] 동시에 오픈 가능한 SWR 존 수 한계 또는 ZBC_NO_LIMIT. */

	dprint(FD_ZBD, "%s: vendor_id:%s, type: %s, model: %s\n",
	       f->file_name, zinfo->zbd_vendor_id,
	       zbc_device_type_str(zinfo->zbd_type),
	       zbc_device_model_str(zinfo->zbd_model));
	/* [한국어] FD_ZBD 디버그 채널(zbd.c에서 활성화 시)로 장치 식별 로그 출력.
	 * zbc_device_type_str/model_str는 libzbc가 제공하는 enum→문자열 변환 유틸. */

	free(zinfo);
	/* [한국어] 정보 복사 완료 — 임시 버퍼 해제. */

	return 0;
	/* [한국어] 정상 경로 — 항상 0. */
}

/*
 * [한국어]
 * libzbc_open_dev - 디바이스를 SCSI/ATA 패스스루 드라이버로 오픈하고 libzbc_data를
 *                  생성/부착한다. 이미 오픈되어 있으면(즉 td->io_ops_data != NULL)
 *                  기존 핸들을 그대로 반환하는 "lazy open" 패턴을 사용한다.
 *
 * @td:   잡 실행 컨텍스트. td->io_ops_data가 상태 저장소.
 * @f:    대상 fio_file — 파일명과 파일 유형(블록/캐릭터) 검증.
 * @p_ld: NULL이 아니면 호출자가 ld를 재사용할 수 있도록 포인터 반환(get_zoned_model/
 *        get_file_size/report_zones가 open을 트리거하면서 동시에 ld가 필요한 경우).
 * @return: 0 성공, 음수 에러(-EINVAL 파일 유형 오류, -ENOMEM calloc 실패, libzbc 음수 에러).
 *
 * 왜 지연 오픈 패턴인가: fio ZBD 초기화 경로는 ioengine_ops.open_file 이전에 이미
 * get_zoned_model/get_file_size/report_zones를 호출할 수 있다(존 맵 구축이 파일 오픈
 * 전에 필요하기 때문). 각 콜백이 독립적으로 디바이스를 열어서는 안 되므로, 공통 진입점
 * libzbc_open_dev를 두고 최초 호출 시에만 실제 zbc_open을 수행한다.
 *
 * 실행 컨텍스트: 잡 스레드. 잡 기간 중 여러 콜백에서 호출되지만 최초 1회만 실제 오픈.
 *
 * 호출 체인:
 *   libzbc_open_file / libzbc_get_file_size / libzbc_get_zoned_model / libzbc_report_zones /
 *   libzbc_get_max_open_zones → [libzbc_open_dev] → zbc_open + libzbc_get_dev_info
 */
static int libzbc_open_dev(struct thread_data *td, struct fio_file *f,
			   struct libzbc_data **p_ld)
{
	struct libzbc_data *ld = td->io_ops_data;
	/* [한국어] 이미 이 잡에 엔진 상태가 부착되어 있는지 확인(지연 오픈 패턴의 캐시 키). */
	int ret, flags = OS_O_DIRECT;
	/* [한국어] 기본 open(2) 플래그 — OS_O_DIRECT: 리눅스에서 O_DIRECT. 페이지 캐시를
	 * 우회해 libzbc가 블록 디바이스로 직접 명령을 보낼 때 중복 캐싱/캐시 히트에 의한
	 * 지연 측정 왜곡을 막는다. ZBD 드라이브는 대부분 direct I/O 시나리오에서만 유효한
	 * 성능 수치를 제공한다. */

	if (ld) {
		/* Already open */
		/* [한국어] 이미 열린 상태(예: get_zoned_model → report_zones → open_file 순으로
		 * 여러 콜백이 동일 잡에서 호출됐고 첫 콜백에서 오픈된 경우) */
		assert(ld->zdev);
		/* [한국어] 방어적 확인 — ld가 있다면 zdev도 반드시 유효해야 함(invariant). */
		goto out;
		/* [한국어] 추가 작업 없이 p_ld 전달만 수행. */
	}

	/* [한국어] ZBD는 블록/캐릭터 디바이스여야 함. 파일은 허용하지 않음 */
	if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR) {
		/* [한국어] ZBC/ZAC는 원시 디바이스 명령이 필요하므로 일반 파일을 대상으로 할 수 없다.
		 * 블록(/dev/sdX) 또는 캐릭터(/dev/sgN) 노드만 허용. */
		td_verror(td, EINVAL, "wrong file type");
		/* [한국어] fio 표준 에러 전파 — errno 코드 + 간단 설명. */
		log_err("ioengine libzbc only works on block or character devices\n");
		/* [한국어] 사용자 대면 에러 메시지 — 사용법 안내. */
		return -EINVAL;
	}

	/* [한국어] 잡 방향에 따른 오픈 모드. read_only 전역이 켜져 있으면 RDWR 금지 */
	if (td_write(td) || td_trim(td)) {
		/* [한국어] 쓰기 또는 트림(= WP 리셋) 잡이면 RDWR 필요. */
		if (!read_only)
			flags |= O_RDWR;
		/* [한국어] read_only 전역(--readonly 옵션)이 켜져 있으면 안전 장치로 RDWR 금지 —
		 * 쓰기 시도는 잡 시작 후 fio_ro_check에서 잡힘. */
	} else if (td_read(td)) {
			flags |= O_RDONLY;
			/* [한국어] 읽기 전용 잡이면 RDONLY로 오픈 — 커널이 쓰기 경로의 권한
			 * 체크를 생략하므로 메타데이터 부담 경감. */
	}

	ld = calloc(1, sizeof(*ld));
	/* [한국어] 엔진 상태 할당. calloc으로 모든 필드를 0/NULL 초기화 — zdev NULL은
	 * 이후 에러 경로에서 close_dev의 NULL 체크가 안전하게 동작하게 함. */
	if (!ld)
		return -ENOMEM;

	/* [한국어] libzbc에 SCSI+ATA 드라이버 둘 다 허용 — 장치에 맞는 걸 자동 선택 */
	ret = zbc_open(f->file_name,
		       flags | ZBC_O_DRV_SCSI | ZBC_O_DRV_ATA,
		       &ld->zdev);
	/* [한국어] libzbc의 zbc_open: 표준 O_* 플래그에 libzbc 특수 플래그를 비트 OR로 결합.
	 *   ZBC_O_DRV_SCSI: SCSI(sg_io ioctl) 경로로 ZBC 명령 전송 허용.
	 *   ZBC_O_DRV_ATA:  ATA 경로(SAT — SCSI-ATA Translation)로 ZAC 명령 전송 허용.
	 * 둘 다 세트하면 libzbc가 드라이브 종류를 자동 탐지해 적절한 백엔드를 선택한다.
	 * 반환: 0 성공, 음수 libzbc 에러코드. ld->zdev에 디바이스 핸들 저장. */
	if (ret) {
		log_err("%s: zbc_open() failed, err=%d\n",
			f->file_name, ret);
		goto err;
	}

	ret = libzbc_get_dev_info(ld, f);
	/* [한국어] 디바이스 정보(모델/섹터 수/최대 오픈 순차 존) 캐시. 실패 시 지금까지 오픈된
	 * 핸들을 닫아야 하므로 err_close 경로로 이동. */
	if (ret)
		goto err_close;

	td->io_ops_data = ld;   /* [한국어] 성공 시 잡에 부착 */
	/* [한국어] 이후 모든 콜백이 이 포인터를 통해 엔진 상태를 공유한다. */
out:
	if (p_ld)
		*p_ld = ld;
		/* [한국어] 호출자가 ld를 요구했으면(get_zoned_model 등) 포인터 전달. */

	return 0;

err_close:
	zbc_close(ld->zdev);
	/* [한국어] dev_info 실패 시 방금 연 핸들을 닫음 — 부분 초기화 누수 방지. */
err:
	free(ld);
	/* [한국어] calloc으로 확보한 구조체 해제. td->io_ops_data에 아직 붙지 않았으므로 안전. */
	return ret;
}

/*
 * [한국어]
 * libzbc_close_dev - 디바이스를 닫고 libzbc_data를 해제한다.
 *                   td->io_ops_data를 먼저 NULL로 비워 이중 close 방지 안전 장치를
 *                   삽입한다.
 *
 * @td: 잡 실행 컨텍스트.
 * @return: 0 성공, 음수: zbc_close의 실패 코드(디바이스 특정 상황).
 *
 * 실행 컨텍스트: 잡 스레드. close_file 또는 cleanup에서 호출. 잡당 1회 이하.
 *
 * 호출 체인:
 *   libzbc_close_file / libzbc_cleanup → [libzbc_close_dev] → zbc_close
 */
static int libzbc_close_dev(struct thread_data *td)
{
	struct libzbc_data *ld = td->io_ops_data;
	/* [한국어] 현재 부착된 엔진 상태 스냅샷. */
	int ret = 0;

	td->io_ops_data = NULL;
	/* [한국어] 먼저 포인터를 끊어 다른 경로에서 재진입해도 NULL을 보게 함(중복 close 방어). */
	if (ld) {
		if (ld->zdev)
			ret = zbc_close(ld->zdev);
			/* [한국어] libzbc 핸들 닫기 — 내부적으로 close(2)와 상태 해제. */
		free(ld);
		/* [한국어] 엔진 상태 구조체 해제. */
	}

	return ret;
}

/*
 * [한국어]
 * libzbc_open_file - ioengine_ops.open_file 콜백 구현. 공통 로직은 libzbc_open_dev에
 *                   위임하며, fio 코어가 파일마다 이 콜백을 호출한다.
 * @td: 잡 실행 컨텍스트.
 * @f:  이번에 오픈할 파일(블록/캐릭터 디바이스).
 * @return: 0 성공, 음수 에러 — fio 코어는 이를 잡 중단 신호로 해석.
 *
 * 실행 컨텍스트: 잡 스레드. 잡당 nr_files회(보통 1회) 호출.
 *
 * 호출 체인:
 *   td_io_open_file → [libzbc_open_file] → libzbc_open_dev
 */
static int libzbc_open_file(struct thread_data *td, struct fio_file *f)
{
	return libzbc_open_dev(td, f, NULL);
	/* [한국어] p_ld=NULL — 여기서는 상태 포인터를 사용할 일이 없으므로 전달 생략. */
}

/*
 * [한국어]
 * libzbc_close_file - ioengine_ops.close_file 콜백 구현. libzbc_close_dev에 위임하고
 *                    실패 시 로그를 남긴다.
 * @return: 0 성공, 음수 에러(zbc_close 실패). fio 코어에 전파.
 *
 * 실행 컨텍스트: 잡 스레드. 잡 종료 직전 또는 파일 전환 시 호출.
 */
static int libzbc_close_file(struct thread_data *td, struct fio_file *f)
{
	int ret;

	ret = libzbc_close_dev(td);
	/* [한국어] 내부 핸들 닫기 + 상태 해제. */
	if (ret)
		log_err("%s: close device failed err %d\n",
			f->file_name, ret);
		/* [한국어] 실패 시에도 구조체는 이미 free됐음 — 사용자에게 경고만 출력. */

	return ret;
}

/*
 * [한국어]
 * libzbc_cleanup - ioengine_ops.cleanup 콜백. 잡 종료 시 열린 상태가 남아 있으면 정리.
 *                 close_file이 먼저 호출된 경우 io_ops_data가 이미 NULL이라 no-op로 수렴.
 * 실행 컨텍스트: 잡 스레드, 잡당 1회.
 */
static void libzbc_cleanup(struct thread_data *td)
{
	libzbc_close_dev(td);
	/* [한국어] 이중 호출 안전 — close_dev 내부에서 NULL 체크. */
}

/*
 * [한국어]
 * libzbc_invalidate - ioengine_ops.invalidate 콜백. 일반적으로 페이지 캐시를 비우는
 *                    용도이나 libzbc는 O_DIRECT 패스스루 경로이므로 호스트 캐시가 없어
 *                    아무 일도 하지 않는다.
 * @return: 항상 0.
 * 실행 컨텍스트: 잡 스레드, 파일별로 호출될 수 있음.
 */
static int libzbc_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* Passthrough IO do not cache data. Nothing to do */
	/* [한국어] ZBC_O_DRV_SCSI|ATA + O_DIRECT 조합은 페이지 캐시를 거치지 않으므로
	 * invalidate_fadvise(2) 같은 작업이 불필요. no-op. */
	return 0;
}

/*
 * [한국어]
 * libzbc_get_file_size - ioengine_ops.get_file_size 콜백. 디바이스의 전체 섹터 수를
 *                       바이트로 환산해 fio_file.real_file_size에 기록한다.
 *
 * @td: 잡 실행 컨텍스트.
 * @f:  크기를 확정할 대상 파일.
 * @return: 0 성공, 음수 에러(open_dev 실패 코드).
 *
 * 왜 필요한가: fio는 잡 시작 전에 대상 파일의 크기를 알아야 오프셋 분포, 블록 수 계산,
 * 존 맵 메모리 할당 등을 수행할 수 있다. ZBD는 디바이스 단위로 크기가 고정이므로
 * libzbc_get_dev_info에서 얻은 nr_sectors를 <<9로 바이트 변환해 돌려준다(512B 가정 —
 * ZBC/ZAC 표준은 논리 블록 크기 512B 또는 4KB이나 libzbc는 섹터 단위를 추상화 섹터
 * (=512B)로 정규화해 노출).
 *
 * 실행 컨텍스트: 잡 스레드. 잡 초기화 단계에서 파일별 1회.
 *
 * 호출 체인:
 *   td_io_get_file_size → [libzbc_get_file_size] → libzbc_open_dev(지연 오픈)
 */
static int libzbc_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct libzbc_data *ld;
	int ret;

	if (fio_file_size_known(f))
		return 0;
		/* [한국어] 이미 다른 경로(예: 사용자 size= 옵션, 이전 콜백)에서 크기가 확정됐으면
		 * 재계산 불필요. */

	ret = libzbc_open_dev(td, f, &ld);
	/* [한국어] 지연 오픈 — 이미 열려 있으면 기존 ld 반환, 아니면 새로 오픈 후 ld 반환. */
	if (ret)
		return ret;

	f->real_file_size = ld->nr_sectors << 9;
	/* [한국어] 섹터 수 × 512B = 전체 용량(바이트). 비트 시프트는 곱셈보다 명시적. */
	fio_file_set_size_known(f);
	/* [한국어] "크기 확정" 플래그 세트 — 다음 콜백 호출 시 재계산 방지. */

	return 0;
}

/*
 * [한국어]
 * libzbc_get_zoned_model - ioengine_ops.get_zoned_model 콜백. fio ZBD 코어에게
 *                         이 디바이스의 zoned model(NONE/HOST_AWARE/HOST_MANAGED)을
 *                         보고한다. fio는 이 값을 토대로 존 규칙을 적용할지 결정한다.
 *
 * @td:    잡 실행 컨텍스트.
 * @f:     대상 파일.
 * @model: 출력 파라미터. 이 함수가 채워 넣는다.
 * @return: 0 성공, -EINVAL(파일 타입 오류), 음수(open_dev 실패).
 *
 * 매핑 규칙:
 *   ZBC_DM_HOST_AWARE   → ZBD_HOST_AWARE   (순차 쓰기 선호, 랜덤 쓰기 허용)
 *   ZBC_DM_HOST_MANAGED → ZBD_HOST_MANAGED (순차 쓰기 강제, WP 추적 필수)
 *   그 외(DRIVE_MANAGED/STANDARD) → ZBD_NONE (기존 블록 디바이스로 취급)
 *
 * 실행 컨텍스트: 잡 스레드. fio ZBD 초기화 단계에서 파일별 1회 호출.
 *
 * 호출 체인:
 *   zbd_init(zbd.c) → ioengine_ops.get_zoned_model → [libzbc_get_zoned_model] → libzbc_open_dev
 */
static int libzbc_get_zoned_model(struct thread_data *td, struct fio_file *f,
				  enum zbd_zoned_model *model)
{
	struct libzbc_data *ld;
	int ret;

	if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR)
		return -EINVAL;
		/* [한국어] ZBC/ZAC는 원시 디바이스 전용. 일반 파일/파이프는 즉시 거절. */

	ret = libzbc_open_dev(td, f, &ld);
	/* [한국어] 지연 오픈 경로 — 필요 시 여기서 최초 zbc_open. */
	if (ret)
		return ret;

	switch (ld->model) {
	case ZBC_DM_HOST_AWARE:
		*model = ZBD_HOST_AWARE;
		/* [한국어] 호스트 인지형: 기존 블록 장치처럼 랜덤 RW 가능하지만 WP 힌트 활용 권장.
		 * fio는 순차 쓰기 정책을 "선호"로만 적용. */
		break;
	case ZBC_DM_HOST_MANAGED:
		*model = ZBD_HOST_MANAGED;
		/* [한국어] 호스트 관리형: SWR 존은 WP에서만 쓰기 허용. fio는 WP를 엄격히 추적하고
		 * reset_wp/finish_zone을 적극 사용해 존 상태를 직접 관리. */
		break;
	default:
		*model = ZBD_NONE;
		/* [한국어] Drive-managed SMR 또는 일반 디스크 — zoning이 호스트에 노출되지 않음.
		 * fio는 ZBD 존 제약 없이 일반 블록 엔진처럼 동작. */
		break;
	}

	return 0;
}

/*
 * [한국어]
 * libzbc_report_zones - ioengine_ops.report_zones 콜백. libzbc의 zbc_report_zones를
 *                      호출해 지정 오프셋부터 nr_zones개의 존 정보를 받아 fio ZBD의
 *                      struct zbd_zone 배열로 변환한다.
 *
 * @td:       잡 실행 컨텍스트.
 * @f:        대상 파일.
 * @offset:   보고 시작 바이트 오프셋. 내부적으로 섹터(>>9)로 변환.
 * @zbdz:    출력 배열. nr_zones개 만큼 채워짐.
 * @nr_zones: 요청 존 수(입력). 실제 채워진 개수가 반환값.
 * @return:   실제 채운 존 수(≥0) 또는 음수 에러.
 *
 * 왜 필요한가: fio ZBD 코어는 잡 시작 전 모든 존의 start/len/capacity/wp/type/cond를
 * 알아야 이후 I/O 분배 시 존 상태 머신을 유지할 수 있다. libzbc는 자체 포맷(struct
 * zbc_zone — 섹터 단위 필드)으로 보고하므로, 바이트 단위 fio ZBD 포맷으로 번역한다.
 *
 * 변환 규칙:
 *   - start/len/wp: 섹터→바이트(<<9)
 *   - capacity: ZBC/ZAC에 존 용량 개념이 없으므로 length와 동일로 설정(ZNS/NVMe와 차이).
 *   - type: CONVENTIONAL→CNV, SEQUENTIAL_REQ→SWR, SEQUENTIAL_PREF→SWP
 *   - cond: NOT_WP/EMPTY/IMP_OPEN/EXP_OPEN/CLOSED/FULL 1:1 매핑,
 *           RDONLY/OFFLINE/unknown은 OFFLINE 취급 + wp를 start로 리셋.
 *
 * 실행 컨텍스트: 잡 스레드. fio ZBD 초기화 시 호출(전체 디바이스 스캔은 여러 번 호출로
 * 분할될 수 있음). 오픈 전에 호출 가능 — libzbc_open_dev의 지연 오픈 패턴이 대응.
 *
 * 호출 체인:
 *   zbd_create_zone_info → ioengine_ops.report_zones → [libzbc_report_zones]
 *   → zbc_report_zones(libzbc)
 */
static int libzbc_report_zones(struct thread_data *td, struct fio_file *f,
			       uint64_t offset, struct zbd_zone *zbdz,
			       unsigned int nr_zones)
{
	struct libzbc_data *ld;
	uint64_t sector = offset >> 9;
	/* [한국어] 바이트 오프셋 → 512B 섹터 변환. libzbc API가 섹터 단위를 요구. */
	struct zbc_zone *zones;
	unsigned int i;
	int ret;

	ret = libzbc_open_dev(td, f, &ld);
	/* [한국어] 지연 오픈 — 이 콜백이 open_file보다 먼저 호출될 수 있음에 유의. */
	if (ret)
		return ret;

	if (sector >= ld->nr_sectors)
		return 0;
		/* [한국어] 요청 시작 섹터가 디바이스 용량 밖 → 유효한 존이 없으니 0개 보고
		 * (fio ZBD는 이를 "더 이상 보고할 존 없음"으로 해석해 스캔 종료). */

	zones = calloc(nr_zones, sizeof(struct zbc_zone));
	/* [한국어] libzbc가 채워 줄 수신 버퍼. 0 초기화로 미사용 슬롯 노이즈 방지. */
	if (!zones) {
		ret = -ENOMEM;
		goto out;
	}

	/* [한국어] ZBC_RO_ALL: 모든 존 상태 포함. &nr_zones는 in/out(실제 채워진 수) */
	ret = zbc_report_zones(ld->zdev, sector, ZBC_RO_ALL, zones, &nr_zones);
	/* [한국어] libzbc API: SCSI REPORT ZONES 명령을 발행해 sector 이후의 zones[] 배열을
	 * 채운다. ZBC_RO_ALL은 Reporting Option=0x00(모든 존 상태 포함) — ZBC/ZAC 스펙의
	 * Reporting Option 매치. nr_zones는 in(요청 수)/out(실제 채워진 수) 양방향 전달. */
	if (ret < 0) {
		log_err("%s: zbc_report_zones failed, err=%d\n",
			f->file_name, ret);
		goto out;
	}

	for (i = 0; i < nr_zones; i++, zbdz++) {
		/* [한국어] 각 존을 libzbc 포맷(zones[i])에서 fio ZBD 포맷(*zbdz)으로 번역. */
		/* [한국어] libzbc는 LBA(섹터)를 주므로 <<9로 바이트 변환해 fio ZBD에 저장 */
		zbdz->start = zones[i].zbz_start << 9;
		/* [한국어] 존의 시작 LBA를 바이트 오프셋으로 변환. */
		zbdz->len = zones[i].zbz_length << 9;
		/* [한국어] 존 길이(섹터 수) → 바이트. 보통 256MB 또는 1GB(드라이브별). */
		zbdz->wp = zones[i].zbz_write_pointer << 9;
		/* [한국어] 쓰기 포인터(다음 쓰기 위치). SWR 존은 여기에만 쓸 수 있다.
		 * Conventional/NOT_WP 존의 wp는 libzbc가 start와 같은 값으로 반환. */
		/*
		 * ZBC/ZAC do not define zone capacity, so use the zone size as
		 * the zone capacity.
		 */
		/* [한국어] ZBC/ZAC엔 zone capacity 개념이 없으므로 size=capacity로 간주
		 * (ZNS/NVMe와 달리 — NVMe ZNS는 zone size > zone capacity 허용하여 내부 메타
		 * 영역을 보정하지만 ZBC/ZAC는 전체 존이 사용자 영역) */
		zbdz->capacity = zbdz->len;

		switch (zones[i].zbz_type) {
		case ZBC_ZT_CONVENTIONAL:
			zbdz->type = ZBD_ZONE_TYPE_CNV;
			/* [한국어] Conventional 존 — 일반 블록처럼 랜덤 RW 가능. 랜덤 쓰기 대상
			 * 으로 fio가 자유롭게 사용. */
			break;
		case ZBC_ZT_SEQUENTIAL_REQ:
			zbdz->type = ZBD_ZONE_TYPE_SWR;
			/* [한국어] Sequential Write Required — 호스트 관리형 드라이브의 존. WP에서만
			 * 쓰기 가능. 재사용하려면 reset_wp로 WP를 zone start로 되돌려야 함. */
			break;
		case ZBC_ZT_SEQUENTIAL_PREF:
			zbdz->type = ZBD_ZONE_TYPE_SWP;
			/* [한국어] Sequential Write Preferred — 호스트 인지형 드라이브의 존. 순차
			 * 아닌 쓰기도 내부 간접으로 허용하지만 성능 저하. */
			break;
		default:
			td_verror(td, errno, "invalid zone type");
			log_err("%s: invalid type for zone at sector %llu.\n",
				f->file_name, (unsigned long long)zbdz->start);
			/* [한국어] 알 수 없는 존 타입 — libzbc 버전 불일치 또는 드라이브 벤더 확장.
			 * 안전을 위해 EIO로 중단. */
			ret = -EIO;
			goto out;
		}

		/* [한국어] ZBC 존 상태 → fio ZBD 상태로 1:1 매핑 */
		switch (zones[i].zbz_condition) {
		case ZBC_ZC_NOT_WP:     zbdz->cond = ZBD_ZONE_COND_NOT_WP;   break;  /* [한국어] WP 없음(Conventional 존) — WP 개념이 적용 안 됨 */
		case ZBC_ZC_EMPTY:      zbdz->cond = ZBD_ZONE_COND_EMPTY;    break;  /* [한국어] 비어 있음(WP=start) — 오픈 한계에 포함 안 됨 */
		case ZBC_ZC_IMP_OPEN:   zbdz->cond = ZBD_ZONE_COND_IMP_OPEN; break;  /* [한국어] 암묵 오픈 — 쓰기 시 드라이브가 자동 오픈. 오픈 한계 카운트 */
		case ZBC_ZC_EXP_OPEN:   zbdz->cond = ZBD_ZONE_COND_EXP_OPEN; break;  /* [한국어] 명시 오픈 — OPEN ZONE 명령으로 오픈. 오픈 한계 카운트 */
		case ZBC_ZC_CLOSED:     zbdz->cond = ZBD_ZONE_COND_CLOSED;   break;  /* [한국어] 닫힘 — 일부 쓰여졌지만 오픈 상태는 아님. 재오픈 가능 */
		case ZBC_ZC_FULL:       zbdz->cond = ZBD_ZONE_COND_FULL;     break;  /* [한국어] 가득 참(WP=start+len) — reset 전까지 추가 쓰기 불가 */
		case ZBC_ZC_RDONLY:
		case ZBC_ZC_OFFLINE:
		default:
			/* Treat all these conditions as offline (don't use!) */
			/* [한국어] RDONLY/OFFLINE/알 수 없음 → 접근 불가 간주, WP를 start로 되돌림
			 * 이유: fio ZBD 코어가 OFFLINE 존을 스킵하도록 유도, wp=start로 통일해
			 * 이후 회계 혼선 방지. */
			zbdz->cond = ZBD_ZONE_COND_OFFLINE;
			zbdz->wp = zbdz->start;
		}
	}

	ret = nr_zones;
	/* [한국어] 성공 시 실제 채운 존 수를 반환 — fio는 이 값으로 while 루프를 돌리며
	 * 디바이스 끝까지 스캔. */
out:
	free(zones);
	/* [한국어] 수신 버퍼 해제. 에러 경로에서도 반드시 호출되도록 goto out. */
	return ret;
}

/*
 * [한국어]
 * libzbc_reset_wp - ioengine_ops.reset_wp 콜백. [offset, offset+length) 범위에 포함된
 *                  존들의 Write Pointer를 zone start로 되돌려(= 쓰기 가능 상태 EMPTY로)
 *                  존을 재사용 가능하게 만든다.
 *
 * @td:     잡 실행 컨텍스트.
 * @f:      대상 파일(에러 로깅용).
 * @offset: 리셋 시작 바이트 오프셋.
 * @length: 리셋 범위 바이트 수.
 * @return: 0 성공, 음수 에러 — zbc_errno로 SCSI sense key 추출해 로그에 첨부.
 *
 * 최적화: [offset, offset+length) = [0, nr_sectors*512)이면 ZBC_OP_ALL_ZONES 플래그로
 * 단일 호출. 그 외엔 존 크기 단위로 반복 호출. ZBC/ZAC의 RESET WRITE POINTER 명령
 * 지연은 존별로 수십 ms~수백 ms이므로 전 범위 최적화가 의미 있다.
 *
 * 실행 컨텍스트: 잡 스레드. fio ZBD가 존 재활용이 필요할 때 호출(잡 시작 전 prep 단계
 * 또는 잡 중 randomize 정책에 따라).
 *
 * 호출 체인:
 *   zbd_reset_zone → ioengine_ops.reset_wp → [libzbc_reset_wp] → zbc_reset_zone(libzbc)
 */
static int libzbc_reset_wp(struct thread_data *td, struct fio_file *f,
			   uint64_t offset, uint64_t length)
{
	struct libzbc_data *ld = td->io_ops_data;
	/* [한국어] 이 단계에서는 open_file이 이미 완료됐으므로 ld가 반드시 존재. */
	uint64_t sector = offset >> 9;                         /* [한국어] 시작 섹터 — libzbc가 섹터 단위 LBA 요구 */
	uint64_t end_sector = (offset + length) >> 9;          /* [한국어] 끝 섹터(배타적) — 전 범위 판정용 */
	unsigned int nr_zones;
	struct zbc_errno err;
	int i, ret;

	assert(ld);
	assert(ld->zdev);
	/* [한국어] 이 콜백은 오픈 후에만 호출되어야 함(fio ZBD 계약). */

	/* [한국어] 포함 존 수(올림 나눗셈) */
	nr_zones = (length + td->o.zone_size - 1) / td->o.zone_size;
	/* [한국어] ceil(length / zone_size). td->o.zone_size는 잡 옵션으로 설정된 존 크기
	 * (fio --zonesize). 디바이스 실제 존 크기와 일치해야 하며 report_zones로 동일성 검증. */
	if (!sector && end_sector >= ld->nr_sectors) {
		/* Reset all zones */
		/* [한국어] 전 범위 요청 → ALL_ZONES 단일 호출로 효율화 */
		ret = zbc_reset_zone(ld->zdev, 0, ZBC_OP_ALL_ZONES);
		/* [한국어] libzbc API: SCSI RESET WRITE POINTER 명령(op=0x04) with ALL 플래그.
		 * sector 인자 무시, 모든 순차 존을 한 번에 EMPTY 상태로 전환. */
		if (ret)
			goto err;

		return 0;
	}

	/* [한국어] 존별로 순회하며 개별 reset */
	for (i = 0; i < nr_zones; i++, sector += td->o.zone_size >> 9) {
		/* [한국어] 섹터 포인터를 존 크기만큼 증가(zone_size는 바이트, >>9로 섹터 변환). */
		ret = zbc_reset_zone(ld->zdev, sector, 0);
		/* [한국어] 단일 존 RESET. 두 번째 인자는 존 시작 섹터, 세 번째는 ALL 플래그(0=single). */
		if (ret)
			goto err;
	}

	return 0;

err:
	zbc_errno(ld->zdev, &err);
	/* [한국어] libzbc가 가장 최근에 기록한 SCSI 에러 정보를 추출 — sense key, ASC/ASCQ
	 * (Additional Sense Code/Qualifier). ZBC 에러는 대부분 "Invalid Field in CDB" 또는
	 * "Zone Transition To Full"처럼 ASC로 세분화된다. */
	td_verror(td, errno, "zbc_reset_zone failed");
	if (err.sk)
		log_err("%s: reset wp failed %s:%s\n",
			f->file_name,
			zbc_sk_str(err.sk), zbc_asc_ascq_str(err.asc_ascq));
		/* [한국어] sense key가 있을 때만 사람이 읽을 수 있는 문자열로 출력. */
	return -ret;
	/* [한국어] libzbc는 양수 에러코드를 반환하므로 fio 관례(음수 errno)에 맞춰 부호 반전. */
}

/*
 * [한국어]
 * libzbc_move_zone_wp - ioengine_ops.move_zone_wp 콜백. 지정 존의 현재 WP 위치부터
 *                      length 바이트만큼 buf로 쓰기 수행해 WP를 "앞으로 이동"시킨다.
 *                      fio ZBD가 "이미 일부 쓰여진 존"을 시뮬레이션하거나, sparse fill로
 *                      테스트 시나리오를 구성할 때 사용.
 *
 * @td:    잡 실행 컨텍스트.
 * @f:     대상 파일.
 * @z:     이동 대상 존(zbd_zone — wp 필드가 현재 WP 바이트 위치).
 * @length: 쓸 바이트 수. 512B의 배수여야 함.
 * @buf:   쓸 데이터(보통 패턴 데이터).
 * @return: 0 성공, 음수 에러(부분 쓰기 또는 libzbc 실패).
 *
 * 실행 컨텍스트: 잡 스레드. 잡 prep 단계에서 존 WP 초기화 시 호출.
 *
 * 호출 체인:
 *   zbd_move_zone_wp → ioengine_ops.move_zone_wp → [libzbc_move_zone_wp] → zbc_pwrite
 */
static int libzbc_move_zone_wp(struct thread_data *td, struct fio_file *f,
			       struct zbd_zone *z, uint64_t length,
			       const char *buf)
{
	struct libzbc_data *ld = td->io_ops_data;
	uint64_t sector = z->wp >> 9;     /* [한국어] 현재 WP(바이트)를 섹터 오프셋으로 변환 */
	size_t count = length >> 9;        /* [한국어] 쓸 바이트 수를 섹터 수로 변환 */
	struct zbc_errno err;
	int ret;

	assert(ld);
	assert(ld->zdev);
	assert(buf);
	/* [한국어] 모든 선결 조건 확인 — buf는 호출자가 zone 크기 이상 확보한 패턴 버퍼. */

	/* [한국어] WP부터 count 섹터를 buf로 쓰기. 부분 완료 시 아래에서 에러로 처리 */
	ret = zbc_pwrite(ld->zdev, buf, count, sector);
	/* [한국어] libzbc API: SCSI WRITE(10/16) CDB를 조립해 sector부터 count 섹터를 쓴다.
	 * SWR 존에서는 sector == 현재 WP와 일치해야 하며 아니면 드라이브가 UNALIGNED WRITE
	 * 에러를 돌려준다. 반환값은 실제 쓴 섹터 수 또는 음수 에러. */
	if (ret == count)
		return 0;

	zbc_errno(ld->zdev, &err);
	td_verror(td, errno, "zbc_write for write pointer move failed");
	if (err.sk)
		log_err("%s: wp move failed %s:%s\n",
			f->file_name,
			zbc_sk_str(err.sk), zbc_asc_ascq_str(err.asc_ascq));
	return -ret;
	/* [한국어] 부분 성공(ret < count) 또는 음수 에러 모두 실패로 처리. */
}

/*
 * [한국어]
 * libzbc_finish_zone - ioengine_ops.finish_zone 콜백. 지정 범위의 존을 FINISH 상태
 *                     (=WP를 존 끝으로 강제 이동, FULL 상태로 전이)로 전환.
 *                     데이터는 쓰지 않고 메타데이터만 조작한다.
 *
 * @td:     잡 실행 컨텍스트.
 * @f:      대상 파일.
 * @offset: FINISH 시작 바이트 오프셋.
 * @length: FINISH 범위 바이트 수.
 * @return: 0 성공, 음수 에러.
 *
 * 왜 필요한가: 부분적으로 쓰여진 SWR 존이 "오픈 한계(max_open_seq_req)"를 소모하지
 * 않도록 fio가 명시적으로 FINISH해서 FULL 상태로 만들 때 사용. reset과 달리 데이터는
 * 그대로 두고 상태만 전이하는 것이 핵심.
 *
 * 실행 컨텍스트: 잡 스레드. fio ZBD가 오픈 한계 관리를 위해 호출.
 *
 * 호출 체인:
 *   zbd_finish_zone → ioengine_ops.finish_zone → [libzbc_finish_zone] → zbc_finish_zone(libzbc)
 */
static int libzbc_finish_zone(struct thread_data *td, struct fio_file *f,
			      uint64_t offset, uint64_t length)
{
	struct libzbc_data *ld = td->io_ops_data;
	uint64_t sector = offset >> 9;
	unsigned int nr_zones;
	struct zbc_errno err;
	int i, ret;

	assert(ld);
	assert(ld->zdev);

	nr_zones = (length + td->o.zone_size - 1) / td->o.zone_size;
	/* [한국어] 올림 나눗셈으로 포함 존 수 산출(reset_wp와 동일 공식). */
	assert(nr_zones > 0);
	/* [한국어] 범위가 비어있으면(length=0) 호출될 이유가 없음 — 방어적 검사. */

	for (i = 0; i < nr_zones; i++, sector += td->o.zone_size >> 9) {
		ret = zbc_finish_zone(ld->zdev, sector, 0);
		/* [한국어] libzbc API: SCSI FINISH ZONE 명령(op=0x02). 두 번째 인자는 존 시작
		 * 섹터, 세 번째는 ALL 플래그(0=single). 성공 시 해당 존이 FULL 상태로 전이. */
		if (ret)
			goto err;
	}

	return 0;

err:
	zbc_errno(ld->zdev, &err);
	td_verror(td, errno, "zbc_finish_zone failed");
	if (err.sk)
		log_err("%s: finish zone failed %s:%s\n",
			f->file_name,
			zbc_sk_str(err.sk), zbc_asc_ascq_str(err.asc_ascq));
	return -ret;
}

/*
 * [한국어]
 * libzbc_get_max_open_zones - ioengine_ops.get_max_open_zones 콜백. 드라이브의 동시
 *                            오픈 가능 순차 존 수 한계를 fio ZBD에 보고한다.
 *
 * @td:             잡 실행 컨텍스트.
 * @f:              대상 파일.
 * @max_open_zones: 출력 — 한계값. ZBC_NO_LIMIT은 0(무제한)으로 변환.
 * @return: 0 성공, 음수(open_dev 실패).
 *
 * 매핑 근거: fio ZBD는 0을 "무제한"으로 해석하며 양수는 "그만큼만 동시 오픈 가능"으로
 * 해석. libzbc의 ZBC_NO_LIMIT(플랫폼별 상수, 종종 0xffffffff)을 0으로 정규화해 전달.
 *
 * 실행 컨텍스트: 잡 스레드. fio ZBD 초기화 시 1회.
 *
 * 호출 체인:
 *   zbd_init → ioengine_ops.get_max_open_zones → [libzbc_get_max_open_zones]
 *   → libzbc_open_dev(지연 오픈 경유)
 */
static int libzbc_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				     unsigned int *max_open_zones)
{
	struct libzbc_data *ld;
	int ret;

	ret = libzbc_open_dev(td, f, &ld);
	/* [한국어] 지연 오픈 — 이 콜백도 open_file 이전에 호출될 수 있음. */
	if (ret)
		return ret;

	if (ld->max_open_seq_req == ZBC_NO_LIMIT)
		*max_open_zones = 0;
		/* [한국어] libzbc가 "무제한"을 돌려준 경우 fio 규약에 맞춰 0으로 보고. */
	else
		*max_open_zones = ld->max_open_seq_req;

	return 0;
}

/*
 * [한국어]
 * libzbc_rw - DDIR_READ/WRITE 공통 헬퍼. io_u의 바이트 오프셋/길이를 섹터로 환산해
 *            zbc_pread 또는 zbc_pwrite를 호출한다. short transfer(부분 전송) 또는
 *            libzbc 음수 에러를 모두 -EIO로 정규화한다.
 *
 * @td:   잡 실행 컨텍스트.
 * @io_u: 처리할 I/O 유닛(offset/xfer_buf/xfer_buflen/ddir 사용).
 * @return: 성공 시 전송된 섹터 수(count와 동일), 실패 시 -EIO.
 *
 * 주의: SWR 존에서 zbc_pwrite는 현재 WP와 sector가 일치해야 한다. 불일치 시 드라이브는
 * UNALIGNED WRITE COMMAND 에러를 반환하므로, 이 엔진은 fio ZBD가 io_u의 오프셋을
 * WP에 맞게 계산해 주는 것에 의존한다. 즉 fio --zonemode=zbd가 필수.
 *
 * 실행 컨텍스트: 잡 스레드. queue() 콜백에서 호출. 동기 엔진이므로 완료 후 반환.
 *
 * 호출 체인:
 *   libzbc_queue → [libzbc_rw] → zbc_pread / zbc_pwrite (libzbc → SCSI CDB)
 */
ssize_t libzbc_rw(struct thread_data *td, struct io_u *io_u)
{
	struct libzbc_data *ld = td->io_ops_data;
	struct fio_file *f = io_u->file;
	uint64_t sector = io_u->offset >> 9;      /* [한국어] 바이트 오프셋 → 512B 섹터 */
	size_t count = io_u->xfer_buflen >> 9;     /* [한국어] 바이트 길이 → 섹터 수 */
	struct zbc_errno err;
	ssize_t ret;

	if (io_u->ddir == DDIR_WRITE)
		ret = zbc_pwrite(ld->zdev, io_u->xfer_buf, count, sector);
		/* [한국어] libzbc API: SCSI WRITE(16) CDB로 count 섹터를 sector부터 쓴다.
		 * SWR 존: sector == WP 필수. Conventional/SWP: 임의 sector 허용. */
	else
		ret = zbc_pread(ld->zdev, io_u->xfer_buf, count, sector);
		/* [한국어] libzbc API: SCSI READ(16) CDB. 모든 존 타입에서 읽기 허용(단 FULL 존에
		 * 쓰여지지 않은 영역 읽기는 드라이브별로 0 또는 에러). */
	if (ret == count)
		return ret;   /* [한국어] 전량 전송 성공 */

	if (ret > 0) {
		log_err("Short %s, len=%zu, ret=%zd\n",
			io_u->ddir == DDIR_READ ? "read" : "write",
			count << 9, ret << 9);
		return -EIO;
		/* [한국어] 부분 전송은 ZBC/ZAC 표준상 드물지만 발생 가능(드라이브 캐시 이슈 등).
		 * fio의 io_u는 "전량 또는 에러" 의미론을 선호하므로 -EIO로 정규화. */
	}

	/* I/O error */
	zbc_errno(ld->zdev, &err);
	/* [한국어] libzbc가 기록한 최신 SCSI sense 정보 추출 — 디버깅에 필수적. */
	td_verror(td, errno, "libzbc i/o failed");
	if (err.sk) {
		log_err("%s: op %u offset %llu+%llu failed (%s:%s), err %zd\n",
			f->file_name, io_u->ddir,
			io_u->offset, io_u->xfer_buflen,
			zbc_sk_str(err.sk),
			zbc_asc_ascq_str(err.asc_ascq), ret);
		/* [한국어] sense key 있음 — SCSI 계층에서 에러 분류 확보(예: UNALIGNED WRITE,
		 * ZONE IS FULL, WRITE BOUNDARY VIOLATION). */
	} else {
		log_err("%s: op %u offset %llu+%llu failed, err %zd\n",
			f->file_name, io_u->ddir,
			io_u->offset, io_u->xfer_buflen, ret);
		/* [한국어] sense 없음 — I/O 이전 단계(파일 디스크립터 수준) 실패 가능성. */
	}

	return -EIO;
}

/*
 * [한국어]
 * libzbc_queue - ioengine_ops.queue 콜백. 동기 엔진이므로 io_u를 즉시 처리하고
 *               FIO_Q_COMPLETED를 반환한다. ddir에 따라 RW/SYNC/TRIM 경로로 분기.
 *
 * @td:   잡 실행 컨텍스트.
 * @io_u: 처리할 I/O 유닛.
 * @return: 항상 FIO_Q_COMPLETED. io_u->error에 errno(양수) 기록.
 *
 * 분기:
 *   DDIR_READ/WRITE         → libzbc_rw (zbc_pread/pwrite)
 *   DDIR_SYNC/DATASYNC      → zbc_flush (SCSI SYNCHRONIZE CACHE(16))
 *   DDIR_TRIM               → zbd_do_io_u_trim (fio ZBD 공용 헬퍼 — 존 reset으로 매핑)
 *   기타                    → -EINVAL
 *
 * 동기 엔진(FIO_SYNCIO) 계약:
 *   - queue()가 호출된 시점에 I/O가 이미 완료되어야 함.
 *   - commit/getevents/event는 호출되지 않음(코어가 FIO_SYNCIO 플래그로 건너뜀).
 *   - io_u->error에 양수 errno를 기록하면 fio 코어가 이를 해석해 통계에 반영.
 *   - 반환값은 FIO_Q_COMPLETED 고정(FIO_Q_QUEUED/BUSY는 비동기 전용).
 *
 * 실행 컨텍스트: 잡 스레드. io_u 1개당 1회 호출, 전량 직렬.
 *
 * 호출 체인:
 *   td_io_queue → [libzbc_queue] → libzbc_rw | zbc_flush | zbd_do_io_u_trim
 */
static enum fio_q_status libzbc_queue(struct thread_data *td, struct io_u *io_u)
{
	struct libzbc_data *ld = td->io_ops_data;
	struct fio_file *f = io_u->file;
	ssize_t ret = 0;

	fio_ro_check(td, io_u);
	/* [한국어] read-only 잡에 WRITE가 오지 않았는지 검증(디버그 빌드에서 assert).
	 * 잡 옵션과 io_u 방향의 불일치를 조기 포착. */

	dprint(FD_ZBD, "%p:%s: libzbc queue %llu\n",
	       td, f->file_name, io_u->offset);
	/* [한국어] FD_ZBD 디버그 채널로 queue 진입 로그 — 존별 I/O 순서 추적에 유용. */

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {
		ret = libzbc_rw(td, io_u);                /* [한국어] 표준 RW 경로 */
	} else if (ddir_sync(io_u->ddir)) {
		/* [한국어] ddir_sync는 DDIR_SYNC, DDIR_DATASYNC, DDIR_SYNC_FILE_RANGE 3종을 포괄. */
		ret = zbc_flush(ld->zdev);                 /* [한국어] 캐시 flush(SYNCHRONIZE CACHE) */
		/* [한국어] libzbc API: SCSI SYNCHRONIZE CACHE(16) CDB 전송. 드라이브 내부 write
		 * 캐시를 미디어에 플러시. O_DIRECT라도 드라이브 내부 캐시는 남기 때문에 내구성
		 * 보장이 필요한 시점(fdatasync 등)에 호출. */
		if (ret)
			log_err("zbc_flush error %zd\n", ret);
	} else if (io_u->ddir == DDIR_TRIM) {
		ret = zbd_do_io_u_trim(td, io_u);         /* [한국어] fio ZBD 공용 trim 헬퍼 */
		/* [한국어] ZBD에서 TRIM은 존 reset으로 매핑됨 — zbd.c의 zbd_do_io_u_trim이
		 * 이 엔진의 reset_wp 콜백을 역으로 호출해 해당 존을 EMPTY로 되돌린다. */
		if (!ret)
			ret = EINVAL;
			/* [한국어] zbd 헬퍼가 0(=트림이 무의미하거나 대상 없음)을 반환하면 에러로 승격.
			 * fio 코어는 io_u->error==0을 성공으로만 해석하므로 구별 필요. */
	} else {
		log_err("Unsupported operation %u\n", io_u->ddir);
		/* [한국어] DDIR_WAIT/DDIR_INVAL 등 알 수 없는 ddir. */
		ret = -EINVAL;
	}
	if (ret < 0)
		io_u->error = -ret;  /* [한국어] 음수 errno → 양수로 io_u에 기록 */
		/* [한국어] fio 코어 계약: io_u->error는 양수 errno. libzbc_rw 등이 음수를 돌려주므로
		 * 부호 반전. 0은 성공 의미라서 반전하지 않음. */

	return FIO_Q_COMPLETED;
	/* [한국어] 동기 엔진: I/O 완료 여부와 무관하게(에러도 io_u->error로 전파) 즉시 완료
	 * 신호를 돌려주면 코어가 submit_time/complete_time 회계 + 통계 갱신. */
}

/*
 * [한국어] ioengine_ops — libzbc 엔진 콜백 vtable.
 * register_ioengine()에 의해 전역 엔진 리스트에 링크되어 ioengine=libzbc 지정 시 선택.
 * 동기화: 잡 시작 시점에 고정된 후 불변(읽기 전용).
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "libzbc",
	/* [한국어] 엔진 식별 문자열 — 잡 파일의 `ioengine=libzbc`와 매칭.
	 * 설정자: 이 초기화. 읽는 자: load_ioengine의 strcmp. 동기화: 불변. */

	.version		= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전 — fio 코어와 구조체 레이아웃 호환성 런타임 검증.
	 * 설정자: 이 초기화. 읽는 자: register_ioengine. 동기화: 불변. */

	.open_file		= libzbc_open_file,
	/* [한국어] 파일 오픈 콜백. 각 fio_file마다 1회.
	 * 설정자: 이 초기화. 읽는 자: td_io_open_file.
	 * 반환: 0=성공, 음수=잡 중단. 동기화: 잡 스레드 전용. */

	.close_file		= libzbc_close_file,
	/* [한국어] 파일 클로즈 콜백 — open_file 대칭.
	 * 설정자: 이 초기화. 읽는 자: td_io_close_file.
	 * 반환: 0=성공, 음수=로그 경고(잡 종료 진행). 동기화: 잡 스레드 전용. */

	.cleanup		= libzbc_cleanup,
	/* [한국어] 잡 종료 시 엔진 상태 최종 정리 — close_file이 이미 정리했으면 no-op.
	 * 설정자: 이 초기화. 읽는 자: td_io_cleanup. 동기화: 잡 스레드 전용, 1회. */

	.invalidate		= libzbc_invalidate,
	/* [한국어] 페이지 캐시 무효화 훅. 본 엔진은 O_DIRECT 패스스루라 no-op.
	 * 설정자: 이 초기화. 읽는 자: invalidate_cache. 동기화: 잡 스레드 전용. */

	.get_file_size		= libzbc_get_file_size,
	/* [한국어] 파일/디바이스 크기 확정 콜백. real_file_size = nr_sectors*512.
	 * 설정자: 이 초기화. 읽는 자: td_io_get_file_size.
	 * 반환: 0=성공, 음수=에러. 동기화: 잡 스레드 전용, 파일별 1회. */

	.get_zoned_model	= libzbc_get_zoned_model,
	/* [한국어] ZBD 확장 콜백 — 디바이스의 zoned model을 ZBD_HOST_AWARE/MANAGED/NONE
	 * enum으로 변환 보고. 설정자: 이 초기화. 읽는 자: zbd_init.
	 * 반환: 0=성공, -EINVAL 파일 타입 오류. 동기화: 잡 스레드 전용. */

	.report_zones		= libzbc_report_zones,
	/* [한국어] ZBD 확장 콜백 — zbc_report_zones 호출해 zbd_zone 배열로 번역.
	 * 설정자: 이 초기화. 읽는 자: zbd_create_zone_info.
	 * 반환: 채운 존 수(≥0) 또는 음수. 동기화: 잡 스레드 전용, 여러 번 호출되어 전체 디바이스
	 * 스캔. */

	.reset_wp		= libzbc_reset_wp,
	/* [한국어] ZBD 확장 콜백 — 지정 범위 존의 WP 리셋(zone start로). 범위 전체면
	 * ZBC_OP_ALL_ZONES 최적화. 설정자: 이 초기화. 읽는 자: zbd_reset_zone.
	 * 반환: 0=성공, 음수 에러. 동기화: 잡 스레드 전용. */

	.move_zone_wp		= libzbc_move_zone_wp,
	/* [한국어] ZBD 확장 콜백 — 지정 존의 WP 위치에 buf로 쓰기해 WP를 앞으로 이동시킨다.
	 * 설정자: 이 초기화. 읽는 자: zbd_move_zone_wp.
	 * 반환: 0=성공, 음수 에러. 동기화: 잡 스레드 전용. */

	.get_max_open_zones	= libzbc_get_max_open_zones,
	/* [한국어] ZBD 확장 콜백 — 드라이브의 동시 오픈 순차 존 한계 보고. ZBC_NO_LIMIT→0.
	 * 설정자: 이 초기화. 읽는 자: zbd_init.
	 * 반환: 0=성공, 음수 에러. 동기화: 잡 스레드 전용. */

	.finish_zone		= libzbc_finish_zone,
	/* [한국어] ZBD 확장 콜백 — 지정 범위 존을 FULL 상태로 전이(데이터 유지, WP를 끝으로).
	 * 설정자: 이 초기화. 읽는 자: zbd_finish_zone.
	 * 반환: 0=성공, 음수 에러. 동기화: 잡 스레드 전용. */

	.queue			= libzbc_queue,
	/* [한국어] I/O 제출 콜백 — 동기 엔진이므로 즉시 완료(FIO_Q_COMPLETED). ddir별로
	 * RW/sync/trim 분기. 설정자: 이 초기화. 읽는 자: td_io_queue.
	 * 반환: FIO_Q_COMPLETED 고정. io_u->error에 양수 errno 기록. 동기화: 잡 스레드 전용. */

	.flags			= FIO_SYNCIO | FIO_NOEXTEND | FIO_RAWIO,
	/* [한국어] 엔진 특성 플래그 비트마스크 — 코어 동작 분기 힌트.
	 *   FIO_SYNCIO  : queue()가 호출 시점에 I/O 완료 보장. commit/getevents/event를
	 *                 호출하지 않고 코어가 즉시 submit_time/complete_time 회계 처리.
	 *                 비트 값 1 (ioengine_ops_flags enum 참조).
	 *   FIO_NOEXTEND: 잡 중 파일 크기 확장 금지 — 디바이스는 크기가 고정이므로 fio가
	 *                 write 시 자동 확장하려는 로직을 차단. 존 경계 위반 방지.
	 *   FIO_RAWIO   : 원시 디바이스(/dev/sg, /dev/sd) 대상 — fio가 파일 시스템 관련
	 *                 최적화(ftruncate, fallocate 등)를 시도하지 않게 함.
	 * 미설정 비트의 의미:
	 *   - FIO_DISKLESSIO 없음: 실제 디바이스 필요(파일명 필수).
	 *   - FIO_ASYNCIO_SETS_ISSUE_TIME 없음: 코어가 queue() 진입 시점을 issue_time으로 사용.
	 *   - FIO_NOIO 없음: 실제 I/O 수행.
	 *   - FIO_FAKEIO 없음: 데이터가 실제로 이동하므로 verify 유효.
	 *   - FIO_MEMALIGN 없음: 사용자 버퍼 정렬 강제 안 함(그러나 O_DIRECT이므로 실질적으로
	 *                      4KB 정렬 권장 — fio가 default로 정렬).
	 * 설정자: 이 초기화. 읽는 자: 잡 루프 전반(backend.c, io_u.c). 동기화: 불변. */
};

/*
 * [한국어]
 * fio_libzbc_register - 엔진 등록 생성자. __attribute__((constructor)) 속성으로
 *                      main() 진입 전 libc 동적 로더에 의해 호출되며, 전역 engine_list에
 *                      이 엔진의 ioengine_ops를 링크한다.
 *
 * 실행 컨텍스트: 프로세스 메인 스레드, 프로세스 수명 동안 1회. 정적 링크 빌드에서도
 * .init_array 섹션에 엔트리가 삽입되어 실행된다.
 *
 * 호출 체인:
 *   libc 동적 로더 / _init → [fio_libzbc_register] → register_ioengine → flist_add_tail(engine_list)
 */
static void fio_init fio_libzbc_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] ioengines.c의 전역 engine_list에 이 ioengine을 꼬리에 추가. 이후
	 * load_ioengine("libzbc")가 strcmp로 매칭해 td->io_ops에 연결한다. */
}

/*
 * [한국어]
 * fio_libzbc_unregister - 엔진 해제 소멸자. __attribute__((destructor))로 프로세스 종료
 *                        시 호출. 정적 바이너리에서는 실질적 의미가 적지만 .so 동적
 *                        로딩 빌드에서 dlclose 시 깨끗한 해제를 보장한다.
 *
 * 실행 컨텍스트: 프로세스 종료 단계(exit / dlclose). 1회.
 *
 * 호출 체인:
 *   _fini / libc exit → [fio_libzbc_unregister] → unregister_ioengine → flist_del
 */
static void fio_exit fio_libzbc_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 engine_list에서 이 ioengine을 언링크. */
}
