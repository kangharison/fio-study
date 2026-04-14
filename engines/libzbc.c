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
 * 이 엔진은 libzbc(Western Digital)를 이용해 SMR HDD 및 ZBC/ZAC 호환 호스트 관리 드라이브의
 * Zoned Block Device(ZBD) 영역으로 SCSI/ATA 패스스루 경로로 직접 I/O를 수행한다. 단순
 * pread/pwrite/flush 뿐 아니라 fio ZBD 프레임워크에서 요구하는 zoned model 질의, zone 리포트,
 * reset write pointer, finish zone, move zone WP, max open zones 보고 등의 콜백을 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend → load_ioengine("libzbc") → get_zoned_model(존 모델 인식) → get_file_size →
 * open_file(zbc_open) → queue(zbc_pread/pwrite/flush) → close_file/cleanup 순. zbd.c의 존
 * 관리 계층이 report_zones/reset_wp/finish_zone/move_zone_wp 콜백을 잡 실행 중간에 호출하여
 * 순차 쓰기 규칙(호스트 관리 존의 write pointer)에 부합하도록 I/O를 재배치한다. 동기 엔진
 * (FIO_SYNCIO)이므로 queue()가 곧 완료.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h, err.h: 공용 타입 및 ERR_PTR 매크로.
 * - zbd_types.h, zbd.h: fio ZBD 프레임워크 — 존 타입/상태 열거형(zbd_zone/zbd_zoned_model 등).
 * - libzbc/zbc.h: SMR/ZBC 디바이스 제어 API.
 * - 공유 상태: td->io_ops_data = struct libzbc_data (잡 스레드 단독 소유).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct libzbc_data: zbc 핸들 + 장치 정보(model/nr_sectors/max_open_seq_req).
 * - libzbc_open_dev():  패스스루 드라이버 플래그(ZBC_O_DRV_SCSI|ATA)로 디바이스 오픈 + 정보 획득.
 * - libzbc_get_zoned_model(): Host-Aware/Managed 모델을 fio ZBD enum으로 변환.
 * - libzbc_report_zones(): zbc_report_zones → zbd_zone 배열로 번역(섹터→바이트 <<9).
 * - libzbc_reset_wp()/finish_zone()/move_zone_wp(): 존 상태 머신 제어 콜백들.
 * - libzbc_rw()/libzbc_queue(): 실제 I/O 수행(ddir별 pread/pwrite/flush/trim).
 */

#include <stdlib.h>        /* [한국어] calloc/free */
#include <unistd.h>        /* [한국어] 표준 POSIX */
#include <errno.h>         /* [한국어] errno */
#include <libzbc/zbc.h>    /* [한국어] libzbc 공개 API */

#include "fio.h"
#include "err.h"
#include "zbd_types.h"     /* [한국어] ZBD 프레임워크 타입(zbd_zone, zbd_zoned_model, ZBD_ZONE_*) */
#include "zbd.h"           /* [한국어] zbd_do_io_u_trim 등 프레임워크 헬퍼 */

/*
 * [한국어] libzbc 엔진의 잡별 상태. td->io_ops_data에 저장.
 * 잡 스레드 단독 소유 — 동기화 불필요.
 */
struct libzbc_data {
	struct zbc_device	*zdev;
	/* [한국어] libzbc의 디바이스 핸들(불투명). 설정자: libzbc_open_dev의 zbc_open.
	 * 읽는 자: 모든 I/O/존 제어 콜백. 값 범위: 유효 포인터 또는 NULL(open 전/close 후). */

	enum zbc_dev_model	model;
	/* [한국어] ZBC 모델(HOST_AWARE / HOST_MANAGED). 설정자: libzbc_get_dev_info.
	 * 읽는 자: libzbc_get_zoned_model에서 fio ZBD enum으로 변환. */

	uint64_t		nr_sectors;
	/* [한국어] 디바이스 총 512B 섹터 수. 설정자: libzbc_get_dev_info.
	 * 읽는 자: get_file_size(<<9로 바이트 변환), report_zones 범위 체크. */

	uint32_t		max_open_seq_req;
	/* [한국어] 동시에 열 수 있는 순차 쓰기 존 수. ZBC_NO_LIMIT이면 무제한(→0 보고).
	 * 설정자: libzbc_get_dev_info. 읽는 자: libzbc_get_max_open_zones. */
};

/*
 * [한국어]
 * libzbc_get_dev_info - libzbc에서 디바이스 정보(모델/섹터 수/최대 오픈 존)를 읽어 ld에 캐시.
 * 호출 체인: libzbc_open_dev → [libzbc_get_dev_info] → libzbc::zbc_get_device_info.
 */
static int libzbc_get_dev_info(struct libzbc_data *ld, struct fio_file *f)
{
	struct zbc_device_info *zinfo;

	zinfo = calloc(1, sizeof(*zinfo));   /* [한국어] 스택이 아닌 힙에 — 큰 구조체 대응 */
	if (!zinfo)
		return -ENOMEM;

	zbc_get_device_info(ld->zdev, zinfo);             /* [한국어] libzbc 호출 */
	ld->model = zinfo->zbd_model;                      /* [한국어] HA/HM 모델 */
	ld->nr_sectors = zinfo->zbd_sectors;               /* [한국어] 전체 섹터 수 */
	ld->max_open_seq_req = zinfo->zbd_max_nr_open_seq_req;

	dprint(FD_ZBD, "%s: vendor_id:%s, type: %s, model: %s\n",
	       f->file_name, zinfo->zbd_vendor_id,
	       zbc_device_type_str(zinfo->zbd_type),
	       zbc_device_model_str(zinfo->zbd_model));

	free(zinfo);

	return 0;
}

/*
 * [한국어]
 * libzbc_open_dev - 디바이스를 SCSI/ATA 패스스루 드라이버로 오픈하고 ld 생성/부착.
 *                  이미 열려 있으면 기존 핸들 반환(report_zones/get_zoned_model 등이
 *                  open_file 이전에 호출되는 경로에 대비한 "lazy open" 패턴).
 * @p_ld: NULL이 아니면 호출자에게 ld를 돌려준다.
 * @return: 0 성공, 음수 에러.
 */
static int libzbc_open_dev(struct thread_data *td, struct fio_file *f,
			   struct libzbc_data **p_ld)
{
	struct libzbc_data *ld = td->io_ops_data;
	int ret, flags = OS_O_DIRECT;  /* [한국어] 기본 O_DIRECT — 페이지 캐시 우회 */

	if (ld) {
		/* Already open */
		assert(ld->zdev);
		goto out;
	}

	/* [한국어] ZBD는 블록/캐릭터 디바이스여야 함. 파일은 허용하지 않음 */
	if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR) {
		td_verror(td, EINVAL, "wrong file type");
		log_err("ioengine libzbc only works on block or character devices\n");
		return -EINVAL;
	}

	/* [한국어] 잡 방향에 따른 오픈 모드. read_only 전역이 켜져 있으면 RDWR 금지 */
	if (td_write(td) || td_trim(td)) {
		if (!read_only)
			flags |= O_RDWR;
	} else if (td_read(td)) {
			flags |= O_RDONLY;
	}

	ld = calloc(1, sizeof(*ld));
	if (!ld)
		return -ENOMEM;

	/* [한국어] libzbc에 SCSI+ATA 드라이버 둘 다 허용 — 장치에 맞는 걸 자동 선택 */
	ret = zbc_open(f->file_name,
		       flags | ZBC_O_DRV_SCSI | ZBC_O_DRV_ATA,
		       &ld->zdev);
	if (ret) {
		log_err("%s: zbc_open() failed, err=%d\n",
			f->file_name, ret);
		goto err;
	}

	ret = libzbc_get_dev_info(ld, f);
	if (ret)
		goto err_close;

	td->io_ops_data = ld;   /* [한국어] 성공 시 잡에 부착 */
out:
	if (p_ld)
		*p_ld = ld;

	return 0;

err_close:
	zbc_close(ld->zdev);
err:
	free(ld);
	return ret;
}

/*
 * [한국어]
 * libzbc_close_dev - 디바이스 닫고 ld 해제. td->io_ops_data를 먼저 NULL로 비워
 *                   중복 close 방지.
 */
static int libzbc_close_dev(struct thread_data *td)
{
	struct libzbc_data *ld = td->io_ops_data;
	int ret = 0;

	td->io_ops_data = NULL;
	if (ld) {
		if (ld->zdev)
			ret = zbc_close(ld->zdev);
		free(ld);
	}

	return ret;
}

/* [한국어] open_file 콜백 — 공통 로직 open_dev로 위임. */
static int libzbc_open_file(struct thread_data *td, struct fio_file *f)
{
	return libzbc_open_dev(td, f, NULL);
}

/* [한국어] close_file 콜백 — 디바이스 close 실패 시 로그 기록. */
static int libzbc_close_file(struct thread_data *td, struct fio_file *f)
{
	int ret;

	ret = libzbc_close_dev(td);
	if (ret)
		log_err("%s: close device failed err %d\n",
			f->file_name, ret);

	return ret;
}

/* [한국어] cleanup 콜백 — 디바이스가 아직 열려있으면 정리. */
static void libzbc_cleanup(struct thread_data *td)
{
	libzbc_close_dev(td);
}

/* [한국어] invalidate 콜백 — 패스스루라 호스트 캐시 없음. no-op. */
static int libzbc_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* Passthrough IO do not cache data. Nothing to do */
	return 0;
}

/*
 * [한국어]
 * libzbc_get_file_size - 파일 크기 콜백. 디바이스 섹터 수를 바이트로 환산해 저장.
 */
static int libzbc_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct libzbc_data *ld;
	int ret;

	if (fio_file_size_known(f))
		return 0;   /* [한국어] 이미 알려짐 */

	ret = libzbc_open_dev(td, f, &ld);   /* [한국어] 지연 오픈 */
	if (ret)
		return ret;

	f->real_file_size = ld->nr_sectors << 9;   /* [한국어] 섹터×512 = 바이트 */
	fio_file_set_size_known(f);

	return 0;
}

/*
 * [한국어]
 * libzbc_get_zoned_model - fio ZBD 코어에게 이 디바이스의 모델(HA/HM/NONE) 보고.
 * 호출 체인: zbd 초기화 → ioengine_ops.get_zoned_model → [libzbc_get_zoned_model].
 */
static int libzbc_get_zoned_model(struct thread_data *td, struct fio_file *f,
				  enum zbd_zoned_model *model)
{
	struct libzbc_data *ld;
	int ret;

	if (f->filetype != FIO_TYPE_BLOCK && f->filetype != FIO_TYPE_CHAR)
		return -EINVAL;

	ret = libzbc_open_dev(td, f, &ld);
	if (ret)
		return ret;

	switch (ld->model) {
	case ZBC_DM_HOST_AWARE:
		*model = ZBD_HOST_AWARE;    /* [한국어] 기존 블록 장치처럼 쓰되 zone 힌트 활용 */
		break;
	case ZBC_DM_HOST_MANAGED:
		*model = ZBD_HOST_MANAGED;  /* [한국어] 순차 쓰기 강제 — fio가 WP 추적 필수 */
		break;
	default:
		*model = ZBD_NONE;          /* [한국어] Drive-managed/regular */
		break;
	}

	return 0;
}

/*
 * [한국어]
 * libzbc_report_zones - zbc_report_zones의 결과를 fio ZBD의 zbd_zone 배열로 번역.
 * 섹터(LBA) 단위 → 바이트 단위로 <<9 변환하며, 타입/상태 열거형도 매핑한다.
 * 호출 체인: zbd_open → ioengine_ops.report_zones → [libzbc_report_zones].
 */
static int libzbc_report_zones(struct thread_data *td, struct fio_file *f,
			       uint64_t offset, struct zbd_zone *zbdz,
			       unsigned int nr_zones)
{
	struct libzbc_data *ld;
	uint64_t sector = offset >> 9;   /* [한국어] 바이트→섹터 변환(512B 가정) */
	struct zbc_zone *zones;
	unsigned int i;
	int ret;

	ret = libzbc_open_dev(td, f, &ld);
	if (ret)
		return ret;

	if (sector >= ld->nr_sectors)
		return 0;   /* [한국어] 장치 범위 밖 — 0개 리포트 */

	zones = calloc(nr_zones, sizeof(struct zbc_zone));   /* [한국어] libzbc 포맷 수신 버퍼 */
	if (!zones) {
		ret = -ENOMEM;
		goto out;
	}

	/* [한국어] ZBC_RO_ALL: 모든 존 상태 포함. &nr_zones는 in/out(실제 채워진 수) */
	ret = zbc_report_zones(ld->zdev, sector, ZBC_RO_ALL, zones, &nr_zones);
	if (ret < 0) {
		log_err("%s: zbc_report_zones failed, err=%d\n",
			f->file_name, ret);
		goto out;
	}

	for (i = 0; i < nr_zones; i++, zbdz++) {
		/* [한국어] libzbc는 LBA(섹터)를 주므로 <<9로 바이트 변환해 fio ZBD에 저장 */
		zbdz->start = zones[i].zbz_start << 9;
		zbdz->len = zones[i].zbz_length << 9;
		zbdz->wp = zones[i].zbz_write_pointer << 9;
		/*
		 * ZBC/ZAC do not define zone capacity, so use the zone size as
		 * the zone capacity.
		 */
		/* [한국어] ZBC/ZAC엔 zone capacity 개념이 없으므로 size=capacity로 간주
		 * (ZNS/NVMe와 달리) */
		zbdz->capacity = zbdz->len;

		switch (zones[i].zbz_type) {
		case ZBC_ZT_CONVENTIONAL:
			zbdz->type = ZBD_ZONE_TYPE_CNV;  /* [한국어] 일반 블록 영역 */
			break;
		case ZBC_ZT_SEQUENTIAL_REQ:
			zbdz->type = ZBD_ZONE_TYPE_SWR;  /* [한국어] 순차 쓰기 필수(HM) */
			break;
		case ZBC_ZT_SEQUENTIAL_PREF:
			zbdz->type = ZBD_ZONE_TYPE_SWP;  /* [한국어] 순차 선호(HA) */
			break;
		default:
			td_verror(td, errno, "invalid zone type");
			log_err("%s: invalid type for zone at sector %llu.\n",
				f->file_name, (unsigned long long)zbdz->start);
			ret = -EIO;
			goto out;
		}

		/* [한국어] ZBC 존 상태 → fio ZBD 상태로 1:1 매핑 */
		switch (zones[i].zbz_condition) {
		case ZBC_ZC_NOT_WP:     zbdz->cond = ZBD_ZONE_COND_NOT_WP;   break;  /* [한국어] WP 없음(conv) */
		case ZBC_ZC_EMPTY:      zbdz->cond = ZBD_ZONE_COND_EMPTY;    break;  /* [한국어] 비어 있음 */
		case ZBC_ZC_IMP_OPEN:   zbdz->cond = ZBD_ZONE_COND_IMP_OPEN; break;  /* [한국어] 암묵 오픈 */
		case ZBC_ZC_EXP_OPEN:   zbdz->cond = ZBD_ZONE_COND_EXP_OPEN; break;  /* [한국어] 명시 오픈 */
		case ZBC_ZC_CLOSED:     zbdz->cond = ZBD_ZONE_COND_CLOSED;   break;  /* [한국어] 닫힘 */
		case ZBC_ZC_FULL:       zbdz->cond = ZBD_ZONE_COND_FULL;     break;  /* [한국어] 가득 참 */
		case ZBC_ZC_RDONLY:
		case ZBC_ZC_OFFLINE:
		default:
			/* Treat all these conditions as offline (don't use!) */
			/* [한국어] RDONLY/OFFLINE/알 수 없음 → 접근 불가 간주, WP를 start로 되돌림 */
			zbdz->cond = ZBD_ZONE_COND_OFFLINE;
			zbdz->wp = zbdz->start;
		}
	}

	ret = nr_zones;
out:
	free(zones);
	return ret;
}

/*
 * [한국어]
 * libzbc_reset_wp - [offset, offset+length] 범위의 존 write pointer를 리셋.
 *                   전체 범위라면 ZBC_OP_ALL_ZONES 한 번에, 아니면 존별로 반복 호출.
 */
static int libzbc_reset_wp(struct thread_data *td, struct fio_file *f,
			   uint64_t offset, uint64_t length)
{
	struct libzbc_data *ld = td->io_ops_data;
	uint64_t sector = offset >> 9;                         /* [한국어] 시작 섹터 */
	uint64_t end_sector = (offset + length) >> 9;          /* [한국어] 끝 섹터 */
	unsigned int nr_zones;
	struct zbc_errno err;
	int i, ret;

	assert(ld);
	assert(ld->zdev);

	/* [한국어] 포함 존 수(올림 나눗셈) */
	nr_zones = (length + td->o.zone_size - 1) / td->o.zone_size;
	if (!sector && end_sector >= ld->nr_sectors) {
		/* Reset all zones */
		/* [한국어] 전 범위 요청 → ALL_ZONES 단일 호출로 효율화 */
		ret = zbc_reset_zone(ld->zdev, 0, ZBC_OP_ALL_ZONES);
		if (ret)
			goto err;

		return 0;
	}

	/* [한국어] 존별로 순회하며 개별 reset */
	for (i = 0; i < nr_zones; i++, sector += td->o.zone_size >> 9) {
		ret = zbc_reset_zone(ld->zdev, sector, 0);
		if (ret)
			goto err;
	}

	return 0;

err:
	zbc_errno(ld->zdev, &err);   /* [한국어] SCSI sense key/ASC/ASCQ 추출 */
	td_verror(td, errno, "zbc_reset_zone failed");
	if (err.sk)
		log_err("%s: reset wp failed %s:%s\n",
			f->file_name,
			zbc_sk_str(err.sk), zbc_asc_ascq_str(err.asc_ascq));
	return -ret;
}

/*
 * [한국어]
 * libzbc_move_zone_wp - 지정 존의 WP 위치까지 length만큼 buf로 채워 쓴다.
 *                      fio ZBD가 존 sparse fill 시뮬레이션에 사용.
 */
static int libzbc_move_zone_wp(struct thread_data *td, struct fio_file *f,
			       struct zbd_zone *z, uint64_t length,
			       const char *buf)
{
	struct libzbc_data *ld = td->io_ops_data;
	uint64_t sector = z->wp >> 9;     /* [한국어] 현재 WP(섹터) */
	size_t count = length >> 9;        /* [한국어] 쓸 섹터 수 */
	struct zbc_errno err;
	int ret;

	assert(ld);
	assert(ld->zdev);
	assert(buf);

	/* [한국어] WP부터 count 섹터를 buf로 쓰기. 부분 완료 시 아래에서 에러로 처리 */
	ret = zbc_pwrite(ld->zdev, buf, count, sector);
	if (ret == count)
		return 0;

	zbc_errno(ld->zdev, &err);
	td_verror(td, errno, "zbc_write for write pointer move failed");
	if (err.sk)
		log_err("%s: wp move failed %s:%s\n",
			f->file_name,
			zbc_sk_str(err.sk), zbc_asc_ascq_str(err.asc_ascq));
	return -ret;
}

/*
 * [한국어]
 * libzbc_finish_zone - 지정 범위의 존을 FINISH(가득 찬 상태로 전환).
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
	assert(nr_zones > 0);

	for (i = 0; i < nr_zones; i++, sector += td->o.zone_size >> 9) {
		ret = zbc_finish_zone(ld->zdev, sector, 0);
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
 * libzbc_get_max_open_zones - fio ZBD에 동시 오픈 가능 존 수 보고.
 *                             ZBC_NO_LIMIT이면 0(=무제한)을 반환.
 */
static int libzbc_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				     unsigned int *max_open_zones)
{
	struct libzbc_data *ld;
	int ret;

	ret = libzbc_open_dev(td, f, &ld);
	if (ret)
		return ret;

	if (ld->max_open_seq_req == ZBC_NO_LIMIT)
		*max_open_zones = 0;
	else
		*max_open_zones = ld->max_open_seq_req;

	return 0;
}

/*
 * [한국어]
 * libzbc_rw - DDIR_READ/WRITE 처리 헬퍼. bytes→sectors 변환 후 libzbc pread/pwrite 호출.
 *            short transfer나 에러를 -EIO로 정규화.
 * @return: 성공 시 전송 섹터 수(count), 실패 시 -EIO.
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
	else
		ret = zbc_pread(ld->zdev, io_u->xfer_buf, count, sector);
	if (ret == count)
		return ret;   /* [한국어] 전량 전송 성공 */

	if (ret > 0) {
		log_err("Short %s, len=%zu, ret=%zd\n",
			io_u->ddir == DDIR_READ ? "read" : "write",
			count << 9, ret << 9);
		return -EIO;
	}

	/* I/O error */
	zbc_errno(ld->zdev, &err);
	td_verror(td, errno, "libzbc i/o failed");
	if (err.sk) {
		log_err("%s: op %u offset %llu+%llu failed (%s:%s), err %zd\n",
			f->file_name, io_u->ddir,
			io_u->offset, io_u->xfer_buflen,
			zbc_sk_str(err.sk),
			zbc_asc_ascq_str(err.asc_ascq), ret);
	} else {
		log_err("%s: op %u offset %llu+%llu failed, err %zd\n",
			f->file_name, io_u->ddir,
			io_u->offset, io_u->xfer_buflen, ret);
	}

	return -EIO;
}

/*
 * [한국어]
 * libzbc_queue - 동기 엔진의 queue 콜백. ddir별로 RW/SYNC/TRIM 경로를 분기.
 *               호출 체인: td_io_queue → [libzbc_queue] → libzbc_rw/zbc_flush/zbd_do_io_u_trim.
 */
static enum fio_q_status libzbc_queue(struct thread_data *td, struct io_u *io_u)
{
	struct libzbc_data *ld = td->io_ops_data;
	struct fio_file *f = io_u->file;
	ssize_t ret = 0;

	fio_ro_check(td, io_u);

	dprint(FD_ZBD, "%p:%s: libzbc queue %llu\n",
	       td, f->file_name, io_u->offset);

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {
		ret = libzbc_rw(td, io_u);                /* [한국어] 표준 RW 경로 */
	} else if (ddir_sync(io_u->ddir)) {
		ret = zbc_flush(ld->zdev);                 /* [한국어] 캐시 flush(SYNCHRONIZE CACHE) */
		if (ret)
			log_err("zbc_flush error %zd\n", ret);
	} else if (io_u->ddir == DDIR_TRIM) {
		ret = zbd_do_io_u_trim(td, io_u);         /* [한국어] fio ZBD 공용 trim 헬퍼 */
		if (!ret)
			ret = EINVAL;                         /* [한국어] 프레임워크가 0을 반환하면 에러로 간주 */
	} else {
		log_err("Unsupported operation %u\n", io_u->ddir);
		ret = -EINVAL;
	}
	if (ret < 0)
		io_u->error = -ret;  /* [한국어] 음수 errno → 양수로 io_u에 기록 */

	return FIO_Q_COMPLETED;    /* [한국어] 동기 엔진: 즉시 완료 */
}

FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "libzbc",
	.version		= FIO_IOOPS_VERSION,
	.open_file		= libzbc_open_file,
	.close_file		= libzbc_close_file,
	.cleanup		= libzbc_cleanup,
	.invalidate		= libzbc_invalidate,
	.get_file_size		= libzbc_get_file_size,
	.get_zoned_model	= libzbc_get_zoned_model,
	.report_zones		= libzbc_report_zones,
	.reset_wp		= libzbc_reset_wp,
	.move_zone_wp		= libzbc_move_zone_wp,
	.get_max_open_zones	= libzbc_get_max_open_zones,
	.finish_zone		= libzbc_finish_zone,
	.queue			= libzbc_queue,
	.flags			= FIO_SYNCIO | FIO_NOEXTEND | FIO_RAWIO,
};

/* [한국어] 생성자/소멸자 — 엔진 레지스트리 등록/해제 */
static void fio_init fio_libzbc_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_libzbc_unregister(void)
{
	unregister_ioengine(&ioengine);
}
