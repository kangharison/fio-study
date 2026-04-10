// SPDX-License-Identifier: GPL-2.0
/*
 * nvme structure declarations and helper functions for the
 * io_uring_cmd engine.
 */
/*
 * [한국어 설명]
 * nvme.c - io_uring_cmd 엔진을 위한 NVMe 헬퍼 함수 구현
 *
 * === 핵심 기능 영역 ===
 *
 * 1. 보호 정보(PI) 생성 및 검증:
 *    - fio_nvme_generate_pi_16b_guard(): T10 DIF CRC16 가드 태그 생성
 *    - fio_nvme_generate_pi_64b_guard(): NVMe CRC64 가드 태그 생성
 *    - fio_nvme_verify_pi_16b_guard(): 16비트 가드 PI 검증
 *    - fio_nvme_verify_pi_64b_guard(): 64비트 가드 PI 검증
 *    - fio_nvme_pi_fill(): NVMe 명령의 CDW12~15에 PI 관련 플래그 설정
 *
 * 2. NVMe 명령 준비:
 *    - fio_nvme_uring_cmd_prep(): read/write/trim/flush 명령의 CDW 필드 설정
 *    - fio_nvme_uring_cmd_trim_prep(): DSM(Deallocate) 명령 준비
 *
 * 3. 디바이스 정보 조회:
 *    - fio_nvme_get_info(): Identify 명령으로 LBA 크기, PI 설정 등 조회
 *    - nvme_identify(): Admin Identify 명령 발행 (ioctl 래퍼)
 *
 * 4. ZNS (Zoned Namespaces) 지원:
 *    - fio_nvme_get_zoned_model(): ZNS 지원 여부 확인
 *    - fio_nvme_report_zones(): 존 리포트 조회 → fio zbd_zone 변환
 *    - fio_nvme_reset_wp(): 존 쓰기 포인터 리셋
 *    - fio_nvme_get_max_open_zones(): 최대 오픈 존 수 조회
 *
 * 5. FDP (Flexible Data Placement):
 *    - fio_nvme_iomgmt_ruhs(): Reclaim Unit Handle 상태 조회
 *
 * === PI(Protection Information) 동작 원리 ===
 * 쓰기 시: 각 LBA 블록마다 데이터로부터 CRC를 계산하여 가드 태그에 저장
 * 읽기 시: 읽은 데이터로부터 CRC를 재계산하여 저장된 가드 태그와 비교
 * PI 위치(pi_loc)에 따라 메타데이터의 시작 또는 끝에 PI 튜플이 위치함
 * 확장 LBA(lba_ext > 0)이면 데이터+메타데이터가 연속, 아니면 별도 버퍼
 */

#include "nvme.h"
#include "../crc/crc-t10dif.h"  /* T10 DIF CRC16 계산 함수 */
#include "../crc/crc64.h"       /* NVMe CRC64 계산 함수 */

/*
 * [한국어] 16비트 가드(T10 DIF CRC16) 보호 정보 생성
 *
 * 쓰기 I/O의 각 LBA 블록에 대해:
 * 1. pi_loc에 따라 PI 튜플의 위치(interval)를 계산
 * 2. 데이터 영역에 대해 CRC16을 계산하여 guard에 저장
 * 3. 앱 태그와 참조 태그(LBA 번호)를 설정
 *
 * 확장 LBA(lba_ext > 0): 데이터와 메타데이터가 연속 → buf에서 직접 접근
 * 별도 메타데이터(lba_ext == 0): md_buf로 별도 접근, CRC는 데이터+메타데이터 누적
 */
static void fio_nvme_generate_pi_16b_guard(struct nvme_data *data,
					   struct io_u *io_u,
					   struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_pi_data *pi_data = io_u->engine_data;
	struct nvme_16b_guard_pif *pi;
	unsigned char *buf = io_u->xfer_buf;
	unsigned char *md_buf = io_u->mmap_data;
	__u64 slba = get_slba(data, io_u->offset);
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1;
	__u32 lba_num = 0;
	__u16 guard = 0;

	if (data->pi_loc) {
		if (data->lba_ext)
			pi_data->interval = data->lba_ext - data->ms;
		else
			pi_data->interval = 0;
	} else {
		if (data->lba_ext)
			pi_data->interval = data->lba_ext - sizeof(struct nvme_16b_guard_pif);
		else
			pi_data->interval = data->ms - sizeof(struct nvme_16b_guard_pif);
	}

	if (io_u->ddir != DDIR_WRITE)
		return;

	while (lba_num < nlb) {
		if (data->lba_ext)
			pi = (struct nvme_16b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_16b_guard_pif *)(md_buf + pi_data->interval);

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				guard = fio_crc_t10dif(0, buf, pi_data->interval);
			} else {
				guard = fio_crc_t10dif(0, buf, data->lba_size);
				guard = fio_crc_t10dif(guard, md_buf, pi_data->interval);
			}
			pi->guard = cpu_to_be16(guard);
		}

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			pi->apptag = cpu_to_be16(pi_data->apptag);

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:
			case NVME_NS_DPS_PI_TYPE2:
				pi->srtag = cpu_to_be32((__u32)slba + lba_num);
				break;
			case NVME_NS_DPS_PI_TYPE3:
				break;
			}
		}
		if (data->lba_ext) {
			buf += data->lba_ext;
		} else {
			buf += data->lba_size;
			md_buf += data->ms;
		}
		lba_num++;
	}
}

/*
 * [한국어] 16비트 가드 PI 검증
 *
 * 읽기 I/O의 각 LBA 블록에 대해:
 * 1. PI 비활성화 조건 확인 (apptag=0xFFFF이면 검사 건너뜀)
 * 2. PRCHK_GUARD: CRC16 재계산 → 저장된 guard와 비교
 * 3. PRCHK_APP: 앱 태그를 마스크 적용 후 비교
 * 4. PRCHK_REF: 참조 태그(LBA 번호)를 비교 (Type1/2만)
 * 불일치 시 -EIO 반환하고 에러 로그 출력
 */
static int fio_nvme_verify_pi_16b_guard(struct nvme_data *data,
					struct io_u *io_u)
{
	struct nvme_pi_data *pi_data = io_u->engine_data;
	struct nvme_16b_guard_pif *pi;
	struct fio_file *f = io_u->file;
	unsigned char *buf = io_u->xfer_buf;
	unsigned char *md_buf = io_u->mmap_data;
	__u64 slba = get_slba(data, io_u->offset);
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1;
	__u32 lba_num = 0;
	__u16 unmask_app, unmask_app_exp, guard = 0;

	while (lba_num < nlb) {
		if (data->lba_ext)
			pi = (struct nvme_16b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_16b_guard_pif *)(md_buf + pi_data->interval);

		if (data->pi_type == NVME_NS_DPS_PI_TYPE3) {
			if (pi->apptag == NVME_PI_APP_DISABLE &&
			    pi->srtag == NVME_PI_REF_DISABLE)
				goto next;
		} else if (data->pi_type == NVME_NS_DPS_PI_TYPE1 ||
			   data->pi_type == NVME_NS_DPS_PI_TYPE2) {
			if (pi->apptag == NVME_PI_APP_DISABLE)
				goto next;
		}

		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				guard = fio_crc_t10dif(0, buf, pi_data->interval);
			} else {
				guard = fio_crc_t10dif(0, buf, data->lba_size);
				guard = fio_crc_t10dif(guard, md_buf, pi_data->interval);
			}
			if (be16_to_cpu(pi->guard) != guard) {
				log_err("%s: Guard compare error: LBA: %llu Expected=%x, Actual=%x\n",
					f->file_name, (unsigned long long)slba,
					guard, be16_to_cpu(pi->guard));
				return -EIO;
			}
		}

		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_APP) {
			unmask_app = be16_to_cpu(pi->apptag) & pi_data->apptag_mask;
			unmask_app_exp = pi_data->apptag & pi_data->apptag_mask;
			if (unmask_app != unmask_app_exp) {
				log_err("%s: APPTAG compare error: LBA: %llu Expected=%x, Actual=%x\n",
					f->file_name, (unsigned long long)slba,
					unmask_app_exp, unmask_app);
				return -EIO;
			}
		}

		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:
			case NVME_NS_DPS_PI_TYPE2:
				if (be32_to_cpu(pi->srtag) !=
				    ((__u32)slba + lba_num)) {
					log_err("%s: REFTAG compare error: LBA: %llu Expected=%x, Actual=%x\n",
						f->file_name, (unsigned long long)slba,
						(__u32)slba + lba_num,
						be32_to_cpu(pi->srtag));
					return -EIO;
				}
				break;
			case NVME_NS_DPS_PI_TYPE3:
				break;
			}
		}
next:
		if (data->lba_ext) {
			buf += data->lba_ext;
		} else {
			buf += data->lba_size;
			md_buf += data->ms;
		}
		lba_num++;
	}

	return 0;
}

/*
 * [한국어] 64비트 가드(NVMe CRC64) 보호 정보 생성
 * 16비트 버전과 동일한 로직이지만 CRC64를 사용하여 더 강력한 무결성 보호 제공.
 * 참조 태그는 48비트(6바이트)로 확장되어 더 넓은 LBA 범위를 커버.
 */
static void fio_nvme_generate_pi_64b_guard(struct nvme_data *data,
					   struct io_u *io_u,
					   struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_pi_data *pi_data = io_u->engine_data;
	struct nvme_64b_guard_pif *pi;
	unsigned char *buf = io_u->xfer_buf;
	unsigned char *md_buf = io_u->mmap_data;
	uint64_t guard = 0;
	__u64 slba = get_slba(data, io_u->offset);
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1;
	__u32 lba_num = 0;

	if (data->pi_loc) {
		if (data->lba_ext)
			pi_data->interval = data->lba_ext - data->ms;
		else
			pi_data->interval = 0;
	} else {
		if (data->lba_ext)
			pi_data->interval = data->lba_ext - sizeof(struct nvme_64b_guard_pif);
		else
			pi_data->interval = data->ms - sizeof(struct nvme_64b_guard_pif);
	}

	if (io_u->ddir != DDIR_WRITE)
		return;

	while (lba_num < nlb) {
		if (data->lba_ext)
			pi = (struct nvme_64b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_64b_guard_pif *)(md_buf + pi_data->interval);

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				guard = fio_crc64_nvme(0, buf, pi_data->interval);
			} else {
				guard = fio_crc64_nvme(0, buf, data->lba_size);
				guard = fio_crc64_nvme(guard, md_buf, pi_data->interval);
			}
			pi->guard = cpu_to_be64(guard);
		}

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			pi->apptag = cpu_to_be16(pi_data->apptag);

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:
			case NVME_NS_DPS_PI_TYPE2:
				put_unaligned_be48(slba + lba_num, pi->srtag);
				break;
			case NVME_NS_DPS_PI_TYPE3:
				break;
			}
		}
		if (data->lba_ext) {
			buf += data->lba_ext;
		} else {
			buf += data->lba_size;
			md_buf += data->ms;
		}
		lba_num++;
	}
}

/*
 * [한국어] 64비트 가드 PI 검증
 * CRC64 재계산 후 저장된 가드 태그와 비교.
 * 참조 태그는 48비트 비정렬 big-endian으로 저장/비교.
 */
static int fio_nvme_verify_pi_64b_guard(struct nvme_data *data,
					struct io_u *io_u)
{
	struct nvme_pi_data *pi_data = io_u->engine_data;
	struct nvme_64b_guard_pif *pi;
	struct fio_file *f = io_u->file;
	unsigned char *buf = io_u->xfer_buf;
	unsigned char *md_buf = io_u->mmap_data;
	__u64 slba = get_slba(data, io_u->offset);
	__u64 ref, ref_exp, guard = 0;
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1;
	__u32 lba_num = 0;
	__u16 unmask_app, unmask_app_exp;

	while (lba_num < nlb) {
		if (data->lba_ext)
			pi = (struct nvme_64b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_64b_guard_pif *)(md_buf + pi_data->interval);

		if (data->pi_type == NVME_NS_DPS_PI_TYPE3) {
			if (pi->apptag == NVME_PI_APP_DISABLE &&
			    fio_nvme_pi_ref_escape(pi->srtag))
				goto next;
		} else if (data->pi_type == NVME_NS_DPS_PI_TYPE1 ||
			   data->pi_type == NVME_NS_DPS_PI_TYPE2) {
			if (pi->apptag == NVME_PI_APP_DISABLE)
				goto next;
		}

		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				guard = fio_crc64_nvme(0, buf, pi_data->interval);
			} else {
				guard = fio_crc64_nvme(0, buf, data->lba_size);
				guard = fio_crc64_nvme(guard, md_buf, pi_data->interval);
			}
			if (be64_to_cpu((uint64_t)pi->guard) != guard) {
				log_err("%s: Guard compare error: LBA: %llu Expected=%llx, Actual=%llx\n",
					f->file_name, (unsigned long long)slba,
					guard, be64_to_cpu((uint64_t)pi->guard));
				return -EIO;
			}
		}

		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_APP) {
			unmask_app = be16_to_cpu(pi->apptag) & pi_data->apptag_mask;
			unmask_app_exp = pi_data->apptag & pi_data->apptag_mask;
			if (unmask_app != unmask_app_exp) {
				log_err("%s: APPTAG compare error: LBA: %llu Expected=%x, Actual=%x\n",
					f->file_name, (unsigned long long)slba,
					unmask_app_exp, unmask_app);
				return -EIO;
			}
		}

		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:
			case NVME_NS_DPS_PI_TYPE2:
				ref = get_unaligned_be48(pi->srtag);
				ref_exp = (slba + lba_num) & ((1ULL << 48) - 1);
				if (ref != ref_exp) {
					log_err("%s: REFTAG compare error: LBA: %llu Expected=%llx, Actual=%llx\n",
						f->file_name, (unsigned long long)slba,
						ref_exp, ref);
					return -EIO;
				}
				break;
			case NVME_NS_DPS_PI_TYPE3:
				break;
			}
		}
next:
		if (data->lba_ext) {
			buf += data->lba_ext;
		} else {
			buf += data->lba_size;
			md_buf += data->ms;
		}
		lba_num++;
	}

	return 0;
}
/*
 * [한국어] NVMe Trim(Deallocate) 명령 준비
 * DSM(Dataset Management) 명령으로 NVMe 디바이스에 deallocate 요청.
 * 단일 범위이면 io_u의 offset/length에서 직접 변환,
 * 다중 범위이면 io_u->xfer_buf에 저장된 trim_range 배열을 변환.
 * CDW10: Number of Ranges - 1 (0-based)
 */
static void fio_nvme_uring_cmd_trim_prep(struct nvme_uring_cmd *cmd, struct io_u *io_u,
					 struct nvme_dsm *dsm)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file);
	struct trim_range *range;
	uint8_t *buf_point;
	int i;

	cmd->opcode = nvme_cmd_dsm;
	cmd->nsid = data->nsid;
	cmd->cdw11 = NVME_ATTRIBUTE_DEALLOCATE;
	cmd->addr = (__u64) (uintptr_t) (&dsm->range[0]);

	if (dsm->nr_ranges == 1) {
		dsm->range[0].slba = get_slba(data, io_u->offset);
		/* nlb is a 1-based value for deallocate */
		dsm->range[0].nlb = get_nlb(data, io_u->xfer_buflen) + 1;
		cmd->cdw10 = 0;
		cmd->data_len = sizeof(struct nvme_dsm_range);
	} else {
		buf_point = io_u->xfer_buf;
		for (i = 0; i < io_u->number_trim; i++) {
			range = (struct trim_range *)buf_point;
			dsm->range[i].slba = get_slba(data, range->start);
			/* nlb is a 1-based value for deallocate */
			dsm->range[i].nlb = get_nlb(data, range->len) + 1;
			buf_point += sizeof(struct trim_range);
		}
		cmd->cdw10 = io_u->number_trim - 1;
		cmd->data_len = io_u->number_trim * sizeof(struct nvme_dsm_range);
	}
}

/*
 * [한국어] NVMe passthrough 명령 준비 (메인 진입점)
 *
 * io_u의 ddir에 따라:
 * - READ: read_opcode (보통 0x02 또는 io_uring_cmd용 커스텀 opcode)
 * - WRITE: write_opcode
 * - TRIM: fio_nvme_uring_cmd_trim_prep()으로 위임
 * - SYNC/DATASYNC: flush 명령 (opcode=0x00)
 *
 * CDW10~11: 시작 LBA (64비트를 상/하위 32비트로 분할)
 * CDW12: NLB(0-based) | dtype(비트 20~23) | cdw12_flags(PI 플래그 등)
 * CDW13: DSPEC(비트 16~31) - FDP 데이터 배치 지정
 * iov가 NULL이 아니면 vectored I/O, 아니면 단일 버퍼
 * write_zeroes 명령은 데이터 버퍼 불필요
 */
int fio_nvme_uring_cmd_prep(struct nvme_uring_cmd *cmd, struct io_u *io_u,
			    struct iovec *iov, struct nvme_dsm *dsm,
			    uint8_t read_opcode, uint8_t write_opcode,
			    unsigned int cdw12_flags)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file);
	__u64 slba;
	__u32 nlb;

	memset(cmd, 0, sizeof(struct nvme_uring_cmd));

	switch (io_u->ddir) {
	case DDIR_READ:
		cmd->opcode = read_opcode;
		break;
	case DDIR_WRITE:
		cmd->opcode = write_opcode;
		break;
	case DDIR_TRIM:
		fio_nvme_uring_cmd_trim_prep(cmd, io_u, dsm);
		return 0;
	case DDIR_SYNC:
	case DDIR_DATASYNC:
		cmd->opcode = nvme_cmd_flush;
		cmd->nsid = data->nsid;
		return 0;
	default:
		return -ENOTSUP;
	}

	slba = get_slba(data, io_u->offset);
	nlb = get_nlb(data, io_u->xfer_buflen);

	/* cdw10 and cdw11 represent starting lba */
	cmd->cdw10 = slba & 0xffffffff;
	cmd->cdw11 = slba >> 32;
	/* cdw12 represent number of lba's for read/write */
	cmd->cdw12 = nlb | (io_u->dtype << 20) | cdw12_flags;
	cmd->cdw13 = io_u->dspec << 16;
	if (iov) {
		iov->iov_base = io_u->xfer_buf;
		iov->iov_len = io_u->xfer_buflen;
		cmd->addr = (__u64)(uintptr_t)iov;
		cmd->data_len = 1;
	} else {
		/* no buffer for write zeroes */
		if (cmd->opcode != nvme_cmd_write_zeroes)
			cmd->addr = (__u64)(uintptr_t)io_u->xfer_buf;
		else
			cmd->addr = (__u64)(uintptr_t)NULL;
		cmd->data_len = io_u->xfer_buflen;
	}
	if (data->lba_shift && data->ms) {
		cmd->metadata = (__u64)(uintptr_t)io_u->mmap_data;
		cmd->metadata_len = (nlb + 1) * data->ms;
	}
	cmd->nsid = data->nsid;
	return 0;
}

/*
 * [한국어] 가드 태그 생성 디스패처
 * PI가 활성화되고 PRACT=0(호스트가 PI 처리)일 때만 동작.
 * 가드 유형(16비트/64비트)에 따라 적절한 생성 함수를 호출.
 */
void fio_nvme_generate_guard(struct io_u *io_u, struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file);

	if (data->pi_type && !(opts->io_flags & NVME_IO_PRINFO_PRACT)) {
		if (data->guard_type == NVME_NVM_NS_16B_GUARD)
			fio_nvme_generate_pi_16b_guard(data, io_u, opts);
		else if (data->guard_type == NVME_NVM_NS_64B_GUARD)
			fio_nvme_generate_pi_64b_guard(data, io_u, opts);
	}
}

/*
 * [한국어] NVMe 명령에 PI 관련 필드 설정
 *
 * 1. CDW12에 io_flags(PRCHK_GUARD/APP/REF, PRACT)를 OR로 추가
 * 2. 가드 태그를 생성 (fio_nvme_generate_guard)
 * 3. PI 유형과 가드 유형에 따라:
 *    - CDW14: 초기 참조 태그 (시작 LBA)
 *    - CDW3: 64비트 가드의 경우 참조 태그 상위 16비트
 *    - CDW15: 앱 태그 마스크(상위 16비트) | 앱 태그(하위 16비트)
 */
void fio_nvme_pi_fill(struct nvme_uring_cmd *cmd, struct io_u *io_u,
		      struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file);
	__u64 slba;

	slba = get_slba(data, io_u->offset);
	cmd->cdw12 |= opts->io_flags;

	fio_nvme_generate_guard(io_u, opts);

	switch (data->pi_type) {
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		switch (data->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF)
				cmd->cdw14 = (__u32)slba;
			break;
		case NVME_NVM_NS_64B_GUARD:
			if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
				cmd->cdw14 = (__u32)slba;
				cmd->cdw3 = ((slba >> 32) & 0xffff);
			}
			break;
		default:
			break;
		}
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			cmd->cdw15 = (opts->apptag_mask << 16 | opts->apptag);
		break;
	case NVME_NS_DPS_PI_TYPE3:
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			cmd->cdw15 = (opts->apptag_mask << 16 | opts->apptag);
		break;
	case NVME_NS_DPS_PI_NONE:
		break;
	}
}

/*
 * [한국어] PI 검증 디스패처
 * 가드 유형(16비트/64비트)에 따라 적절한 검증 함수를 호출.
 * 반환값: 0=성공, -EIO=무결성 검사 실패
 */
int fio_nvme_pi_verify(struct nvme_data *data, struct io_u *io_u)
{
	int ret = 0;

	switch (data->guard_type) {
	case NVME_NVM_NS_16B_GUARD:
		ret = fio_nvme_verify_pi_16b_guard(data, io_u);
		break;
	case NVME_NVM_NS_64B_GUARD:
		ret = fio_nvme_verify_pi_64b_guard(data, io_u);
		break;
	default:
		break;
	}

	return ret;
}

/*
 * [한국어] NVMe Admin Identify 명령 발행
 * NVME_IOCTL_ADMIN_CMD ioctl을 통해 컨트롤러에 Identify 명령을 보냄.
 * CNS(Controller or Namespace Structure)와 CSI(Command Set Identifier)에 따라
 * 컨트롤러/네임스페이스/커맨드세트별 정보를 조회.
 */
static int nvme_identify(int fd, __u32 nsid, enum nvme_identify_cns cns,
			 enum nvme_csi csi, void *data)
{
	struct nvme_passthru_cmd cmd = {
		.opcode         = nvme_admin_identify,
		.nsid           = nsid,
		.addr           = (__u64)(uintptr_t)data,
		.data_len       = NVME_IDENTIFY_DATA_SIZE,
		.cdw10          = cns,
		.cdw11          = csi << NVME_IDENTIFY_CSI_SHIFT,
		.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,
	};

	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

/*
 * [한국어] NVMe 디바이스 정보 조회 (엔진 초기화 시 호출)
 *
 * 동작 과정:
 * 1. /dev/ngXnY 캐릭터 디바이스만 지원 (FIO_TYPE_CHAR 검사)
 * 2. NVME_IOCTL_ID로 네임스페이스 ID 획득
 * 3. Identify Controller로 ctratt(ELBAS 지원 여부) 확인
 * 4. Identify Namespace로 LBA 크기, 메타데이터 크기, PI 설정 조회
 * 5. ELBAS 지원 시 NVM Identify NS로 가드 유형(16b/64b) 결정
 * 6. PRACT=1이고 ms==pi_size이면 메타데이터 크기를 0으로 설정
 *    (컨트롤러가 PI를 자동 삽입/제거하므로 호스트에서 처리 불필요)
 * 7. flbas 비트4로 확장 LBA 여부 결정
 * 8. nsze를 nlba에 반환 (파일 크기 계산용)
 */
int fio_nvme_get_info(struct fio_file *f, __u64 *nlba, __u32 pi_act,
		      struct nvme_data *data)
{
	struct nvme_id_ns ns;
	struct nvme_id_ctrl ctrl;
	struct nvme_nvm_id_ns nvm_ns;
	int namespace_id;
	int fd, err;
	__u32 format_idx, elbaf;

	if (f->filetype != FIO_TYPE_CHAR) {
		log_err("ioengine io_uring_cmd only works with nvme ns "
			"generic char devices (/dev/ngXnY)\n");
		return 1;
	}

	fd = open(f->file_name, O_RDONLY);
	if (fd < 0)
		return -errno;

	namespace_id = ioctl(fd, NVME_IOCTL_ID);
	if (namespace_id < 0) {
		err = -errno;
		log_err("%s: failed to fetch namespace-id\n", f->file_name);
		goto out;
	}

	err = nvme_identify(fd, 0, NVME_IDENTIFY_CNS_CTRL, NVME_CSI_NVM, &ctrl);
	if (err) {
		log_err("%s: failed to fetch identify ctrl\n", f->file_name);
		goto out;
	}

	/*
	 * Identify namespace to get namespace-id, namespace size in LBA's
	 * and LBA data size.
	 */
	err = nvme_identify(fd, namespace_id, NVME_IDENTIFY_CNS_NS,
				NVME_CSI_NVM, &ns);
	if (err) {
		log_err("%s: failed to fetch identify namespace\n",
			f->file_name);
		goto out;
	}

	data->nsid = namespace_id;

	/*
	 * 16 or 64 as maximum number of supported LBA formats.
	 * From flbas bit 0-3 indicates lsb and bit 5-6 indicates msb
	 * of the format index used to format the namespace.
	 */
	if (ns.nlbaf < 16)
		format_idx = ns.flbas & 0xf;
	else
		format_idx = (ns.flbas & 0xf) + (((ns.flbas >> 5) & 0x3) << 4);

	data->lba_size = 1 << ns.lbaf[format_idx].ds;
	data->ms = le16_to_cpu(ns.lbaf[format_idx].ms);

	/* Check for end to end data protection support */
	if (data->ms && (ns.dps & NVME_NS_DPS_PI_MASK))
		data->pi_type = (ns.dps & NVME_NS_DPS_PI_MASK);

	if (!data->pi_type)
		goto check_elba;

	if (ctrl.ctratt & NVME_CTRL_CTRATT_ELBAS) {
		err = nvme_identify(fd, namespace_id, NVME_IDENTIFY_CNS_CSI_NS,
					NVME_CSI_NVM, &nvm_ns);
		if (err) {
			log_err("%s: failed to fetch identify nvm namespace\n",
				f->file_name);
			goto out;
		}

		elbaf = le32_to_cpu(nvm_ns.elbaf[format_idx]);

		/* Currently we don't support storage tags */
		if (elbaf & NVME_ID_NS_NVM_STS_MASK) {
			log_err("%s: Storage tag not supported\n",
				f->file_name);
			err = -ENOTSUP;
			goto out;
		}

		data->guard_type = (elbaf >> NVME_ID_NS_NVM_GUARD_SHIFT) &
				NVME_ID_NS_NVM_GUARD_MASK;

		/* No 32 bit guard, as storage tag is mandatory for it */
		switch (data->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			data->pi_size = sizeof(struct nvme_16b_guard_pif);
			break;
		case NVME_NVM_NS_64B_GUARD:
			data->pi_size = sizeof(struct nvme_64b_guard_pif);
			break;
		default:
			break;
		}
	} else {
		data->guard_type = NVME_NVM_NS_16B_GUARD;
		data->pi_size = sizeof(struct nvme_16b_guard_pif);
	}

	/*
	 * when PRACT bit is set to 1, and metadata size is equal to protection
	 * information size, controller inserts and removes PI for write and
	 * read commands respectively.
	 */
	if (pi_act && data->ms == data->pi_size)
		data->ms = 0;

	data->pi_loc = (ns.dps & NVME_NS_DPS_PI_FIRST);

check_elba:
	/*
	 * Bit 4 for flbas indicates if metadata is transferred at the end of
	 * logical block creating an extended LBA.
	 */
	if (data->ms && ((ns.flbas >> 4) & 0x1))
		data->lba_ext = data->lba_size + data->ms;
	else
		data->lba_shift = ilog2(data->lba_size);

	*nlba = ns.nsze;

out:
	close(fd);
	return err;
}

/*
 * [한국어] NVMe ZNS 디바이스 모델 확인
 * ZNS CSI로 Identify Controller/Namespace를 시도하여
 * 성공하면 ZBD_HOST_MANAGED, 실패하면 ZBD_NONE 반환.
 * fio의 ZBD(Zoned Block Device) 프레임워크에서 호출됨.
 */
int fio_nvme_get_zoned_model(struct thread_data *td, struct fio_file *f,
			     enum zbd_zoned_model *model)
{
	struct nvme_data *data = FILE_ENG_DATA(f);
	struct nvme_id_ns ns;
	struct nvme_passthru_cmd cmd;
	int fd, ret = 0;

	if (f->filetype != FIO_TYPE_CHAR)
		return -EINVAL;

	/* File is not yet opened */
	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	/* Using nvme_id_ns for data as sizes are same */
	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_CTRL,
				NVME_CSI_ZNS, &ns);
	if (ret) {
		*model = ZBD_NONE;
		goto out;
	}

	memset(&cmd, 0, sizeof(struct nvme_passthru_cmd));

	/* Using nvme_id_ns for data as sizes are same */
	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_NS,
				NVME_CSI_ZNS, &ns);
	if (ret) {
		*model = ZBD_NONE;
		goto out;
	}

	*model = ZBD_HOST_MANAGED;
out:
	close(fd);
	return 0;
}

/*
 * [한국어] NVMe ZNS Zone Management Receive (존 리포트) 명령 발행
 * slba부터 시작하는 존들의 상태를 data_len 크기만큼 조회.
 * zras_feat에 NVME_ZNS_ZRAS_FEAT_ERZ를 설정하면 빈 존도 포함.
 */
static int nvme_report_zones(int fd, __u32 nsid, __u64 slba, __u32 zras_feat,
			     __u32 data_len, void *data)
{
	struct nvme_passthru_cmd cmd = {
		.opcode         = nvme_zns_cmd_mgmt_recv,
		.nsid           = nsid,
		.addr           = (__u64)(uintptr_t)data,
		.data_len       = data_len,
		.cdw10          = slba & 0xffffffff,
		.cdw11          = slba >> 32,
		.cdw12		= (data_len >> 2) - 1,
		.cdw13		= NVME_ZNS_ZRA_REPORT_ZONES | zras_feat,
		.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,
	};

	return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
}

/*
 * [한국어] NVMe ZNS 존 리포트 조회 및 fio 형식 변환
 *
 * 동작 과정:
 * 1. ZNS Identify NS로 존 크기(zlen) 계산
 * 2. zones_chunks(기본 1024)씩 나누어 반복 조회 (대규모 존 수 처리)
 * 3. 각 nvme_zns_desc를 fio의 zbd_zone 구조체로 변환:
 *    - start, len, wp, capacity: LBA → 바이트 변환
 *    - zt(존 타입): SEQWRITE_REQ → ZBD_ZONE_TYPE_SWR
 *    - zs(존 상태): Empty/Open/Closed/Full → fio 존 상태로 매핑
 * 반환값: 조회된 존 수
 */
int fio_nvme_report_zones(struct thread_data *td, struct fio_file *f,
			  uint64_t offset, struct zbd_zone *zbdz,
			  unsigned int nr_zones)
{
	struct nvme_data *data = FILE_ENG_DATA(f);
	struct nvme_zone_report *zr;
	struct nvme_zns_id_ns zns_ns;
	struct nvme_id_ns ns;
	unsigned int i = 0, j, zones_fetched = 0;
	unsigned int max_zones, zones_chunks = 1024;
	int fd, ret = 0;
	__u32 zr_len;
	__u64 zlen;

	/* File is not yet opened */
	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	zones_fetched = 0;
	zr_len = sizeof(*zr) + (zones_chunks * sizeof(struct nvme_zns_desc));
	zr = calloc(1, zr_len);
	if (!zr) {
		close(fd);
		return -ENOMEM;
	}

	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_NS,
				NVME_CSI_NVM, &ns);
	if (ret) {
		log_err("%s: nvme_identify_ns failed, err=%d\n", f->file_name,
			ret);
		goto out;
	}

	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_NS,
				NVME_CSI_ZNS, &zns_ns);
	if (ret) {
		log_err("%s: nvme_zns_identify_ns failed, err=%d\n",
			f->file_name, ret);
		goto out;
	}
	zlen = zns_ns.lbafe[ns.flbas & 0x0f].zsze << data->lba_shift;

	max_zones = (f->real_file_size - offset) / zlen;
	if (max_zones < nr_zones)
		nr_zones = max_zones;

	if (nr_zones < zones_chunks)
		zones_chunks = nr_zones;

	while (zones_fetched < nr_zones) {
		if (zones_fetched + zones_chunks >= nr_zones) {
			zones_chunks = nr_zones - zones_fetched;
			zr_len = sizeof(*zr) + (zones_chunks * sizeof(struct nvme_zns_desc));
		}
		ret = nvme_report_zones(fd, data->nsid, offset >> data->lba_shift,
					NVME_ZNS_ZRAS_FEAT_ERZ, zr_len, (void *)zr);
		if (ret) {
			log_err("%s: nvme_zns_report_zones failed, err=%d\n",
				f->file_name, ret);
			goto out;
		}

		/* Transform the zone-report */
		for (j = 0; j < zr->nr_zones; j++, i++) {
			struct nvme_zns_desc *desc = (struct nvme_zns_desc *)&(zr->entries[j]);

			zbdz[i].start = desc->zslba << data->lba_shift;
			zbdz[i].len = zlen;
			zbdz[i].wp = desc->wp << data->lba_shift;
			zbdz[i].capacity = desc->zcap << data->lba_shift;

			/* Zone Type is stored in first 4 bits. */
			switch (desc->zt & 0x0f) {
			case NVME_ZONE_TYPE_SEQWRITE_REQ:
				zbdz[i].type = ZBD_ZONE_TYPE_SWR;
				break;
			default:
				log_err("%s: invalid type for zone at offset %llu.\n",
					f->file_name, (unsigned long long) desc->zslba);
				ret = -EIO;
				goto out;
			}

			/* Zone State is stored in last 4 bits. */
			switch (desc->zs >> 4) {
			case NVME_ZNS_ZS_EMPTY:
				zbdz[i].cond = ZBD_ZONE_COND_EMPTY;
				break;
			case NVME_ZNS_ZS_IMPL_OPEN:
				zbdz[i].cond = ZBD_ZONE_COND_IMP_OPEN;
				break;
			case NVME_ZNS_ZS_EXPL_OPEN:
				zbdz[i].cond = ZBD_ZONE_COND_EXP_OPEN;
				break;
			case NVME_ZNS_ZS_CLOSED:
				zbdz[i].cond = ZBD_ZONE_COND_CLOSED;
				break;
			case NVME_ZNS_ZS_FULL:
				zbdz[i].cond = ZBD_ZONE_COND_FULL;
				break;
			case NVME_ZNS_ZS_READ_ONLY:
			case NVME_ZNS_ZS_OFFLINE:
			default:
				/* Treat all these conditions as offline (don't use!) */
				zbdz[i].cond = ZBD_ZONE_COND_OFFLINE;
				zbdz[i].wp = zbdz[i].start;
			}
		}
		zones_fetched += zr->nr_zones;
		offset += zr->nr_zones * zlen;
	}

	ret = zones_fetched;
out:
	free(zr);
	close(fd);

	return ret;
}

/*
 * [한국어] ZNS 존 쓰기 포인터(Write Pointer) 리셋
 * offset~offset+length 범위의 모든 존에 대해 Zone Management Send(Reset) 명령 발행.
 * 리셋된 존은 Empty 상태로 돌아가 처음부터 다시 쓸 수 있게 됨.
 * 파일이 아직 열리지 않은 경우 임시로 open하고 완료 후 close.
 */
int fio_nvme_reset_wp(struct thread_data *td, struct fio_file *f,
		      uint64_t offset, uint64_t length)
{
	struct nvme_data *data = FILE_ENG_DATA(f);
	unsigned int nr_zones;
	unsigned long long zslba;
	int i, fd, ret = 0;

	/* If the file is not yet opened, open it for this function. */
	fd = f->fd;
	if (fd < 0) {
		fd = open(f->file_name, O_RDWR | O_LARGEFILE);
		if (fd < 0)
			return -errno;
	}

	zslba = offset >> data->lba_shift;
	nr_zones = (length + td->o.zone_size - 1) / td->o.zone_size;

	for (i = 0; i < nr_zones; i++, zslba += (td->o.zone_size >> data->lba_shift)) {
		struct nvme_passthru_cmd cmd = {
			.opcode         = nvme_zns_cmd_mgmt_send,
			.nsid           = data->nsid,
			.cdw10          = zslba & 0xffffffff,
			.cdw11          = zslba >> 32,
			.cdw13          = NVME_ZNS_ZSA_RESET,
			.addr           = (__u64)(uintptr_t)NULL,
			.data_len       = 0,
			.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,
		};

		ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	}

	if (f->fd < 0)
		close(fd);
	return -ret;
}

/*
 * [한국어] ZNS 최대 오픈 존 수 조회
 * ZNS Identify NS의 mor(Max Open Resources) + 1을 반환.
 * mor는 0-based이므로 +1하여 실제 최대 오픈 존 수를 계산.
 * fio의 ZBD 프레임워크에서 동시 오픈 존 수를 제한하는 데 사용.
 */
int fio_nvme_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				unsigned int *max_open_zones)
{
	struct nvme_data *data = FILE_ENG_DATA(f);
	struct nvme_zns_id_ns zns_ns;
	int fd, ret = 0;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_NS,
				NVME_CSI_ZNS, &zns_ns);
	if (ret) {
		log_err("%s: nvme_zns_identify_ns failed, err=%d\n",
			f->file_name, ret);
		goto out;
	}

	*max_open_zones = zns_ns.mor + 1;
out:
	close(fd);
	return ret;
}

/*
 * [한국어] FDP Reclaim Unit Handle 상태 조회
 * I/O Management Receive(opcode=0x12) 명령으로 RUH 상태를 조회.
 * CDW10=1은 Reclaim Unit Handle Status를 의미.
 * FDP는 호스트가 데이터 배치를 제어하여 GC(Garbage Collection)로 인한
 * Write Amplification을 줄이는 NVMe 기능.
 */
static inline int nvme_fdp_reclaim_unit_handle_status(int fd, __u32 nsid,
						      __u32 data_len, void *data)
{
	struct nvme_passthru_cmd cmd = {
		.opcode		= nvme_cmd_io_mgmt_recv,
		.nsid		= nsid,
		.addr		= (__u64)(uintptr_t)data,
		.data_len 	= data_len,
		.cdw10		= 1,
		.cdw11		= (data_len >> 2) - 1,
	};

	return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
}

/*
 * [한국어] FDP RUH 상태 조회 래퍼 함수
 * NVMe 캐릭터 디바이스를 열고 RUH 상태를 조회.
 * 실패 시 ENOTSUP으로 설정하여 FDP 미지원 디바이스를 처리.
 * dataplacement.c에서 호출되어 RUH ID 목록을 획득함.
 */
int fio_nvme_iomgmt_ruhs(struct thread_data *td, struct fio_file *f,
			 struct nvme_fdp_ruh_status *ruhs, __u32 bytes)
{
	struct nvme_data *data = FILE_ENG_DATA(f);
	int fd, ret;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	ret = nvme_fdp_reclaim_unit_handle_status(fd, data->nsid, bytes, ruhs);
	if (ret) {
		log_err("%s: nvme_fdp_reclaim_unit_handle_status failed, err=%d\n",
			f->file_name, ret);
		errno = ENOTSUP;
	} else
		errno = 0;

	ret = -errno;
	close(fd);
	return ret;
}
