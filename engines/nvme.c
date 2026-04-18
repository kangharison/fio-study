// SPDX-License-Identifier: GPL-2.0
/*
 * nvme structure declarations and helper functions for the
 * io_uring_cmd engine.
 */
/*
 * [한국어 설명] NVMe 디바이스용 공용 헬퍼 구현 (nvme.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 io_uring_cmd / xnvme / nvme-uring-cmd 계열 I/O 엔진들이 공통으로
 * 사용하는 NVMe 도메인 헬퍼 구현을 모아둔 라이브러리다. ioctl(NVME_IOCTL_ADMIN_CMD)
 * 래퍼, NVMe Identify(Controller/Namespace/NS Descriptor), Read/Write/Trim/Flush
 * Submission Queue Entry(uring_cmd용 nvme_uring_cmd 구조체)의 CDW0~15 세팅,
 * Protection Information(PI: T10 DIF CRC16 / NVMe CRC64) 생성·검증, ZNS(Zoned
 * Namespaces) 존 리포트/리셋/Open 수 질의, FDP(Flexible Data Placement)의 RUHS
 * 조회, 그리고 LBA 포맷(lba_size/ms/pi_loc/lba_ext) 파싱을 담당한다. 이들 헬퍼는
 * 엔진 파일들(engines/io_uring.c 등)에서 직접 호출된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * io_uring_cmd 엔진 플로우: backend → load_ioengine("io_uring_cmd") → init(io_uring
 * 초기화) → prep(여기서 fio_nvme_uring_cmd_prep으로 SQE에 NVMe 명령 빌드) →
 * queue/commit(io_uring_enter) → getevents/event(CQE 회수) → cleanup. 이 파일의
 * 함수들은 prep/open_file 경로에서 불리며, 디바이스 속성 질의는 ioctl 경유
 * Admin 큐, I/O는 io_uring_cmd를 통한 NVMe IOCTL passthrough이다. 실행 컨텍스트는
 * 잡 스레드의 사용자 공간이고, ioctl은 커널의 /dev/nvme* 드라이버로 넘어간다.
 *
 * === 타 모듈과의 연결 ===
 * - nvme.h: 이 파일이 노출하는 선언(struct nvme_data/pi_data/uring_cmd 등).
 * - crc/crc-t10dif.h, crc/crc64.h: PI 가드 태그 계산용 체크섬 엔진.
 * - fio 코어(fio.h): thread_data, io_u, fio_file, dprint(FD_ZBD/FD_FILE) 등.
 * - NVMe UAPI(linux/nvme_ioctl.h): NVME_IOCTL_ADMIN_CMD/IO_CMD 구조체와 opcode.
 * - io_uring_cmd 계열 엔진: io_u->engine_data에 struct nvme_pi_data 연결.
 * - 데이터 흐름: fio_file → NVME_IOCTL_ADMIN_CMD(Identify) → struct nvme_data 채움 →
 *   각 io_u prep 시 nvme_uring_cmd 구조체로 번역 → uring SQE(opcode IORING_OP_URING_CMD)
 *   → 커널 nvme-driver → 디바이스. 완료는 CQE → fio 이벤트 수확.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvme_data (nvme.h): namespace 단위 LBA 포맷 캐시(lba_size/ms/pi_*/lba_ext).
 * - struct nvme_pi_data (nvme.h): io_u당 PI 컨텍스트(interval/앱태그/참조태그).
 * - fio_nvme_generate_pi_16b/64b_guard(): 쓰기 전 PI 가드 태그 계산·기록.
 * - fio_nvme_verify_pi_16b/64b_guard(): 읽기 후 PI 재검증.
 * - fio_nvme_pi_fill(): SQE의 control/dspec/apptag/reftag 필드 구성.
 * - fio_nvme_uring_cmd_prep(): nvme_uring_cmd 구조체의 opcode/nsid/slba/nlb/cdw 세팅.
 * - fio_nvme_uring_cmd_trim_prep(): DSM(Dataset Management - Deallocate) 명령 조립.
 * - nvme_identify()/fio_nvme_get_info(): Admin Identify로 디바이스 속성 로드.
 * - fio_nvme_get_zoned_model/report_zones/reset_wp/get_max_open_zones(): ZNS 콜백 지원.
 * - fio_nvme_iomgmt_ruhs(): FDP의 Reclaim Unit Handle 상태 조회.
 *
 * === PI(Protection Information) 동작 원리 ===
 * 쓰기 시: 각 LBA 블록마다 데이터로부터 CRC를 계산하여 가드 태그에 저장.
 * 읽기 시: 읽은 데이터로부터 CRC를 재계산하여 저장된 가드 태그와 비교.
 * PI 위치(pi_loc)에 따라 메타데이터의 시작 또는 끝에 PI 튜플이 위치.
 * 확장 LBA(lba_ext > 0)이면 데이터+메타데이터가 연속, 아니면 별도 버퍼.
 *
 * === 본 파일의 비-엔진 성격 ===
 * nvme.c 는 ioengine_ops vtable 을 등록하지 않는다(즉, .name="nvme" 같은
 * 엔진이 따로 존재하지 않는다). 대신 io_uring_cmd / xnvme(io_uring_cmd 백엔드) /
 * sg(NVMe 경유) 같은 정식 엔진들이 자신의 prep/event/zbd 콜백 안에서
 * fio_nvme_* 함수를 호출하는 "공유 라이브러리" 역할만 한다. 그래서
 * fio_init/fio_exit 생성자도 없으며, register_ioengine 도 호출하지 않는다.
 * 빌드 시 같은 디렉토리의 다른 엔진 .o 와 함께 링크되어 fio 본체에 정적으로
 * 포함된다(외부 .so 가 아님).
 *
 * === ioctl(NVME_IOCTL_*) vs io_uring_cmd 경로 비교 ===
 * 본 파일이 사용하는 두 가지 커널 진입 경로:
 *  - ioctl(NVME_IOCTL_ADMIN_CMD, struct nvme_passthru_cmd*): 동기. nvme_identify()
 *    가 사용. Admin 큐로 전달되며 디바이스 응답까지 잡 스레드 블록.
 *  - ioctl(NVME_IOCTL_IO_CMD, struct nvme_passthru_cmd*): 동기. nvme_report_zones,
 *    fio_nvme_reset_wp, nvme_fdp_reclaim_unit_handle_status 가 사용. I/O 큐로
 *    전달되지만 호스트 시점에서는 동기.
 *  - io_uring_cmd 의 IORING_OP_URING_CMD(SQE.cmd_op = NVME_URING_CMD_IO[_VEC]):
 *    비동기. 본 파일은 prep 까지만 담당하고 실제 제출/완료 수확은 엔진 본체
 *    (engines/io_uring.c) 의 io_uring_enter / CQE 폴링 경로가 처리.
 */

#include "nvme.h"                /* [한국어] 이 파일이 구현하는 NVMe 헬퍼들의 공개 선언/구조체 */
#include "../crc/crc-t10dif.h"   /* [한국어] T10 DIF CRC16 — 16b 가드 PI 계산 */
#include "../crc/crc64.h"        /* [한국어] NVMe 표준 CRC64 다항식(Rocksoft) — 64b 가드 PI 계산 */

/*
 * [한국어] 16비트 가드(T10 DIF CRC16) 보호 정보 생성
 *
 * @param data: 이 네임스페이스의 LBA/메타데이터/PI 포맷 캐시 (fio_nvme_get_info가 채운 값).
 * @param io_u: 현재 prep 중인 fio I/O 유닛. xfer_buf(데이터), mmap_data(메타데이터),
 *              offset/xfer_buflen 등을 통해 PI 삽입 대상 영역을 식별한다.
 * @param opts: 이 호출의 io_flags(PRCHK_GUARD/APP/REF)와 apptag/apptag_mask.
 * @return: 없음. 부작용으로 io_u 버퍼에 PI 튜플(guard/apptag/srtag)을 기록한다.
 *
 * caller: fio_nvme_generate_guard() (PI 활성 + PRACT=0일 때만 진입).
 * callee: fio_crc_t10dif()(T10 DIF CRC16 엔진), cpu_to_be16/be32()(바이트 순서 변환).
 *
 * 쓰기 I/O의 각 LBA 블록에 대해:
 * 1. pi_loc에 따라 PI 튜플의 위치(interval)를 계산
 * 2. 데이터 영역에 대해 CRC16을 계산하여 guard에 저장
 * 3. 앱 태그와 참조 태그(LBA 번호)를 설정
 *
 * 확장 LBA(lba_ext > 0): 데이터와 메타데이터가 연속 → buf에서 직접 접근
 * 별도 메타데이터(lba_ext == 0): md_buf로 별도 접근, CRC는 데이터+메타데이터 누적
 *
 * 실행 컨텍스트: 잡 스레드(사용자 공간) — prep() 콜백 경로. I/O별 로컬 버퍼만 다루므로
 * 동시성 문제 없음.
 *
 * 호출 체인:
 *   td_io_prep → 엔진별 prep → fio_nvme_pi_fill → fio_nvme_generate_guard
 *     → [fio_nvme_generate_pi_16b_guard] → fio_crc_t10dif
 */
static void fio_nvme_generate_pi_16b_guard(struct nvme_data *data,
					   struct io_u *io_u,
					   struct nvme_cmd_ext_io_opts *opts)
{
	/* [한국어] io_u->engine_data에는 엔진 init 단계에서 할당한 nvme_pi_data가 연결돼 있다.
	 * 여기서 interval(이 io_u의 PI 튜플 오프셋)을 기록해 verify 경로가 재사용하게 한다. */
	struct nvme_pi_data *pi_data = io_u->engine_data;
	struct nvme_16b_guard_pif *pi;                /* [한국어] 16b 가드 PI 튜플(guard/apptag/srtag)을 가리킬 포인터 — 블록 루프에서 재배치. */
	unsigned char *buf = io_u->xfer_buf;          /* [한국어] 사용자 데이터 버퍼 시작점 — 한 블록씩 전진한다. */
	unsigned char *md_buf = io_u->mmap_data;      /* [한국어] 메타데이터 전용 버퍼(별도 메타일 때만 유효). 확장 LBA면 미사용. */
	__u64 slba = get_slba(data, io_u->offset);    /* [한국어] 시작 LBA = byte offset >> lba_shift (또는 lba_ext로 나눗셈). 참조 태그의 기준. */
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1; /* [한국어] 블록 수. NVMe NLB는 0-based라 +1 하여 1-based 루프 경계로 사용. */
	__u32 lba_num = 0;                            /* [한국어] 루프 인덱스(현재 블록 순번) — 참조 태그 = slba + lba_num. */
	__u16 guard = 0;                              /* [한국어] 계산된 CRC16 누적값. 매 블록 재계산되므로 0으로 초기화. */

	/* [한국어] pi_loc=1(NVME_NS_DPS_PI_FIRST): PI 튜플이 메타 영역의 '앞'에 배치된다.
	 * 이 경우 블록 내 PI 오프셋(interval)은 메타 영역의 시작 = 확장 LBA에서 데이터 크기만큼
	 * 건너뛴 지점(lba_ext - ms), 별도 메타면 메타 버퍼 시작(0). */
	if (data->pi_loc) {
		if (data->lba_ext)                            /* [한국어] 확장 LBA: 데이터+메타가 연속된 lba_ext 블록 한 덩어리. */
			pi_data->interval = data->lba_ext - data->ms; /* [한국어] 버퍼 시작부터 데이터(lba_size) 다음, 메타 시작점. */
		else
			pi_data->interval = 0;                /* [한국어] 별도 메타 버퍼: 메타 시작이 곧 PI 시작. */
	} else {
		/* [한국어] pi_loc=0(기본, PI가 메타 끝에 위치): PI 튜플은 메타 영역의 마지막 sizeof(pif) 바이트. */
		if (data->lba_ext)
			pi_data->interval = data->lba_ext - sizeof(struct nvme_16b_guard_pif); /* [한국어] 확장 LBA 끝에서 PI 크기만큼 뒤로. */
		else
			pi_data->interval = data->ms - sizeof(struct nvme_16b_guard_pif);     /* [한국어] 별도 메타 끝에서 PI 크기만큼 뒤로. */
	}

	/* [한국어] 읽기/트림/플러시에서는 호스트가 PI를 '생성'하지 않는다. 검증은 verify 경로에서 수행. */
	if (io_u->ddir != DDIR_WRITE)
		return;

	/* [한국어] 블록 단위 루프: 각 LBA 블록마다 PI 튜플을 재계산해 삽입. */
	while (lba_num < nlb) {
		/* [한국어] 현재 블록의 PI 튜플 위치 계산. 확장 LBA면 데이터 버퍼 내부, 아니면 메타 버퍼 내부. */
		if (data->lba_ext)
			pi = (struct nvme_16b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_16b_guard_pif *)(md_buf + pi_data->interval);

		/* [한국어] PRCHK_GUARD: T10 DIF CRC16을 데이터(+ 일부 메타)에 대해 계산해 guard 필드에 기록. */
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				/* [한국어] 확장 LBA: 데이터 영역(=interval 바이트, PI 직전까지)에 대해 CRC 계산. */
				guard = fio_crc_t10dif(0, buf, pi_data->interval);
			} else {
				/* [한국어] 별도 메타: 먼저 데이터 블록 전체(lba_size)에 대해 CRC를 돌리고,
				 * 이어서 메타 버퍼의 PI 이전 부분까지 누적해 연속된 CRC를 만든다. */
				guard = fio_crc_t10dif(0, buf, data->lba_size);
				guard = fio_crc_t10dif(guard, md_buf, pi_data->interval);
			}
			pi->guard = cpu_to_be16(guard);       /* [한국어] NVMe/T10 PI는 big-endian으로 저장해야 디바이스가 올바르게 검증. */
		}

		/* [한국어] PRCHK_APP: 사용자가 지정한 application tag(16b)를 be16으로 기록. */
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			pi->apptag = cpu_to_be16(pi_data->apptag);

		/* [한국어] PRCHK_REF: 참조 태그 = 블록별 초기값(TYPE1/2는 LBA 번호로 정해짐). TYPE3는 호스트가 쓰지 않음. */
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:            /* [한국어] Type1: 모든 블록에서 REFTAG 검사 필수, 초기값 = 시작 LBA 하위 32b. */
			case NVME_NS_DPS_PI_TYPE2:            /* [한국어] Type2: 호스트가 초기값 지정 가능하지만 여기서는 LBA 기반. */
				pi->srtag = cpu_to_be32((__u32)slba + lba_num); /* [한국어] srtag = (slba + 현재 블록 인덱스)의 하위 32비트, be32 저장. */
				break;
			case NVME_NS_DPS_PI_TYPE3:            /* [한국어] Type3: 참조 태그 미사용(디바이스가 0xFFFFFFFF로 취급). */
				break;
			}
		}
		/* [한국어] 다음 블록으로 포인터 전진 — 확장 LBA면 lba_ext, 아니면 데이터/메타를 각각 전진. */
		if (data->lba_ext) {
			buf += data->lba_ext;
		} else {
			buf += data->lba_size;
			md_buf += data->ms;
		}
		lba_num++;                                    /* [한국어] 참조 태그용 블록 카운트 증가. */
	}
}

/*
 * [한국어] 16비트 가드 PI 검증
 *
 * @param data: 네임스페이스 LBA/PI 포맷 캐시.
 * @param io_u: 완료된 읽기 io_u — engine_data에 저장된 pi_data(interval/io_flags/apptag) 사용.
 * @return: 0 성공, -EIO 검사 실패(guard/apptag/reftag 불일치). 호출자(엔진 event())는 이 값을
 *          io_u->error에 설정하거나 상위로 전파.
 *
 * caller: fio_nvme_pi_verify()가 guard_type에 따라 디스패치.
 * callee: fio_crc_t10dif(), be16_to_cpu/be32_to_cpu(), log_err().
 *
 * 읽기 I/O의 각 LBA 블록에 대해:
 * 1. PI 비활성화 조건 확인 (apptag=0xFFFF이면 검사 건너뜀)
 * 2. PRCHK_GUARD: CRC16 재계산 → 저장된 guard와 비교
 * 3. PRCHK_APP: 앱 태그를 마스크 적용 후 비교
 * 4. PRCHK_REF: 참조 태그(LBA 번호)를 비교 (Type1/2만)
 * 불일치 시 -EIO 반환하고 에러 로그 출력
 *
 * 호출 체인:
 *   엔진 event/commit 완료 경로 → fio_nvme_pi_verify → [fio_nvme_verify_pi_16b_guard]
 */
static int fio_nvme_verify_pi_16b_guard(struct nvme_data *data,
					struct io_u *io_u)
{
	struct nvme_pi_data *pi_data = io_u->engine_data; /* [한국어] generate 단계에서 기록된 interval/io_flags/apptag/mask를 그대로 사용. */
	struct nvme_16b_guard_pif *pi;                    /* [한국어] 현재 블록의 PI 튜플 포인터. */
	struct fio_file *f = io_u->file;                  /* [한국어] 에러 메시지에 파일명을 포함하기 위해 보관. */
	unsigned char *buf = io_u->xfer_buf;              /* [한국어] 방금 읽어온 데이터 버퍼 시작. */
	unsigned char *md_buf = io_u->mmap_data;          /* [한국어] 별도 메타 버퍼 시작(확장 LBA이면 미사용). */
	__u64 slba = get_slba(data, io_u->offset);        /* [한국어] 시작 LBA — REFTAG 기대값 계산용. */
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1; /* [한국어] 루프 상한(1-based). */
	__u32 lba_num = 0;                                /* [한국어] 블록 카운터. */
	__u16 unmask_app, unmask_app_exp, guard = 0;      /* [한국어] apptag 마스킹 결과와 재계산된 guard 임시 변수. */

	/* [한국어] 블록별로 PI 튜플 위치를 계산하고 guard/apptag/reftag를 재검증. */
	while (lba_num < nlb) {
		if (data->lba_ext)
			pi = (struct nvme_16b_guard_pif *)(buf + pi_data->interval);   /* [한국어] 확장 LBA: 데이터 버퍼 내 상대 오프셋. */
		else
			pi = (struct nvme_16b_guard_pif *)(md_buf + pi_data->interval); /* [한국어] 별도 메타: 메타 버퍼 내 상대 오프셋. */

		/* [한국어] PI Escape: 디바이스가 이 블록을 '비보호'로 표기한 경우 검증 생략 (스펙상 허용). */
		if (data->pi_type == NVME_NS_DPS_PI_TYPE3) {
			/* [한국어] Type3: apptag=0xFFFF && reftag=0xFFFFFFFF 이면 해당 블록 PI 검사 스킵. */
			if (pi->apptag == NVME_PI_APP_DISABLE &&
			    pi->srtag == NVME_PI_REF_DISABLE)
				goto next;
		} else if (data->pi_type == NVME_NS_DPS_PI_TYPE1 ||
			   data->pi_type == NVME_NS_DPS_PI_TYPE2) {
			/* [한국어] Type1/2: apptag=0xFFFF이면 해당 블록 비보호 표시 → 스킵. */
			if (pi->apptag == NVME_PI_APP_DISABLE)
				goto next;
		}

		/* [한국어] PRCHK_GUARD: 동일 알고리즘으로 CRC16을 재계산해 저장된 guard와 비교. */
		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				guard = fio_crc_t10dif(0, buf, pi_data->interval);        /* [한국어] 확장 LBA: 데이터 구간 CRC. */
			} else {
				guard = fio_crc_t10dif(0, buf, data->lba_size);           /* [한국어] 데이터 블록 CRC. */
				guard = fio_crc_t10dif(guard, md_buf, pi_data->interval); /* [한국어] 메타(PI 앞) 구간까지 누적 CRC. */
			}
			/* [한국어] 저장된 be16 guard를 호스트 엔디안으로 변환해 비교. 불일치 시 즉시 -EIO. */
			if (be16_to_cpu(pi->guard) != guard) {
				log_err("%s: Guard compare error: LBA: %llu Expected=%x, Actual=%x\n",
					f->file_name, (unsigned long long)slba,
					guard, be16_to_cpu(pi->guard));
				return -EIO;
			}
		}

		/* [한국어] PRCHK_APP: apptag_mask로 일부 비트만 비교(Type1/2/3 공통 — 0이면 전부 돈케어). */
		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_APP) {
			unmask_app = be16_to_cpu(pi->apptag) & pi_data->apptag_mask;    /* [한국어] 읽어온 apptag에 마스크 적용. */
			unmask_app_exp = pi_data->apptag & pi_data->apptag_mask;        /* [한국어] 기대값에도 동일 마스크 적용. */
			if (unmask_app != unmask_app_exp) {                             /* [한국어] 마스킹된 값이 다르면 손상. */
				log_err("%s: APPTAG compare error: LBA: %llu Expected=%x, Actual=%x\n",
					f->file_name, (unsigned long long)slba,
					unmask_app_exp, unmask_app);
				return -EIO;
			}
		}

		/* [한국어] PRCHK_REF: Type1/2에서 srtag == (slba + blockIndex) 하위 32비트인지 확인. */
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
			case NVME_NS_DPS_PI_TYPE3:            /* [한국어] Type3: 참조 태그 검사 스펙상 생략. */
				break;
			}
		}
next:
		/* [한국어] 다음 블록으로 전진. goto next 진입 시에도 동일 전진 로직을 태운다. */
		if (data->lba_ext) {
			buf += data->lba_ext;
		} else {
			buf += data->lba_size;
			md_buf += data->ms;
		}
		lba_num++;
	}

	return 0;                                         /* [한국어] 모든 블록 검증 통과 — 상위에서 io_u 완료 처리 계속. */
}

/*
 * [한국어] 64비트 가드(NVMe CRC64) 보호 정보 생성
 *
 * @param data: 네임스페이스 LBA/PI 포맷 캐시(guard_type=64B 가정).
 * @param io_u: 쓰기 prep 중인 fio I/O 유닛.
 * @param opts: io_flags 및 apptag/apptag_mask.
 * @return: 없음. 부작용으로 각 블록의 PI 튜플을 채움.
 *
 * caller: fio_nvme_generate_guard() (guard_type == NVME_NVM_NS_64B_GUARD).
 * callee: fio_crc64_nvme()(NVMe 표준 CRC64, Rocksoft 다항식), cpu_to_be64/be16(), put_unaligned_be48().
 *
 * 16비트 버전과 동일한 로직이지만 CRC64를 사용하여 더 강력한 무결성 보호 제공.
 * 참조 태그는 48비트(6바이트)로 확장되어 더 넓은 LBA 범위를 커버.
 *
 * 호출 체인:
 *   td_io_prep → 엔진 prep → fio_nvme_pi_fill → fio_nvme_generate_guard
 *     → [fio_nvme_generate_pi_64b_guard]
 */
static void fio_nvme_generate_pi_64b_guard(struct nvme_data *data,
					   struct io_u *io_u,
					   struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_pi_data *pi_data = io_u->engine_data; /* [한국어] 엔진이 이 io_u에 연결한 PI 컨텍스트. */
	struct nvme_64b_guard_pif *pi;                    /* [한국어] 64b PI 튜플(guard:u64, apptag:u16, srtag:48b). */
	unsigned char *buf = io_u->xfer_buf;              /* [한국어] 데이터 버퍼 포인터. */
	unsigned char *md_buf = io_u->mmap_data;          /* [한국어] 메타 버퍼 포인터(별도 메타일 때 사용). */
	uint64_t guard = 0;                               /* [한국어] CRC64 누적값(호스트 엔디안). */
	__u64 slba = get_slba(data, io_u->offset);        /* [한국어] 시작 LBA. Type1/2에서 reftag 초기값. */
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1; /* [한국어] 블록 수(1-based). */
	__u32 lba_num = 0;                                /* [한국어] 블록 인덱스. */

	/* [한국어] 16b 버전과 동일한 interval(블록 내 PI 오프셋) 계산 로직. pif 크기만 64b용으로 바뀜. */
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

	if (io_u->ddir != DDIR_WRITE)                     /* [한국어] 쓰기만 PI 생성. 그 외는 그냥 리턴. */
		return;

	while (lba_num < nlb) {
		/* [한국어] 블록 내 64b PI 튜플 포인터 획득. */
		if (data->lba_ext)
			pi = (struct nvme_64b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_64b_guard_pif *)(md_buf + pi_data->interval);

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_GUARD) {
			if (data->lba_ext) {
				guard = fio_crc64_nvme(0, buf, pi_data->interval);        /* [한국어] 확장 LBA: 데이터 구간 CRC64. */
			} else {
				guard = fio_crc64_nvme(0, buf, data->lba_size);           /* [한국어] 데이터 블록 CRC64. */
				guard = fio_crc64_nvme(guard, md_buf, pi_data->interval); /* [한국어] 메타 프리픽스까지 누적. */
			}
			pi->guard = cpu_to_be64(guard);           /* [한국어] be64로 저장 — 네트워크 바이트 순서. */
		}

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			pi->apptag = cpu_to_be16(pi_data->apptag);/* [한국어] apptag는 여전히 16비트. */

		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:
			case NVME_NS_DPS_PI_TYPE2:
				/* [한국어] 48비트 참조 태그: slba+lba_num의 하위 48b를 big-endian, 비정렬 6바이트에 저장. */
				put_unaligned_be48(slba + lba_num, pi->srtag);
				break;
			case NVME_NS_DPS_PI_TYPE3:
				break;
			}
		}
		/* [한국어] 다음 블록 전진 — 16b 버전과 동일. */
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
 *
 * @param data: 네임스페이스 포맷 캐시.
 * @param io_u: 완료된 읽기 io_u.
 * @return: 0 성공, -EIO 무결성 실패.
 *
 * caller: fio_nvme_pi_verify().
 * callee: fio_crc64_nvme(), be64_to_cpu(), be16_to_cpu(), get_unaligned_be48(),
 *         fio_nvme_pi_ref_escape() (48b srtag가 모두 1인지 확인).
 *
 * CRC64 재계산 후 저장된 가드 태그와 비교.
 * 참조 태그는 48비트 비정렬 big-endian으로 저장/비교.
 *
 * 호출 체인:
 *   엔진 완료 경로 → fio_nvme_pi_verify → [fio_nvme_verify_pi_64b_guard]
 */
static int fio_nvme_verify_pi_64b_guard(struct nvme_data *data,
					struct io_u *io_u)
{
	struct nvme_pi_data *pi_data = io_u->engine_data; /* [한국어] generate에서 기록한 PI 컨텍스트. */
	struct nvme_64b_guard_pif *pi;                    /* [한국어] 현재 블록의 64b PI 튜플. */
	struct fio_file *f = io_u->file;                  /* [한국어] 로그 출력용 파일 정보. */
	unsigned char *buf = io_u->xfer_buf;              /* [한국어] 데이터 버퍼. */
	unsigned char *md_buf = io_u->mmap_data;          /* [한국어] 메타 버퍼(별도 메타일 때만). */
	__u64 slba = get_slba(data, io_u->offset);        /* [한국어] 시작 LBA — reftag 기대값 산출. */
	__u64 ref, ref_exp, guard = 0;                    /* [한국어] 실제 srtag/기대 srtag/재계산 guard. */
	__u32 nlb = get_nlb(data, io_u->xfer_buflen) + 1; /* [한국어] 블록 수(1-based). */
	__u32 lba_num = 0;                                /* [한국어] 블록 카운터. */
	__u16 unmask_app, unmask_app_exp;                 /* [한국어] apptag 마스킹 비교 임시. */

	while (lba_num < nlb) {
		/* [한국어] 현재 블록의 64b PI 튜플 포인터. */
		if (data->lba_ext)
			pi = (struct nvme_64b_guard_pif *)(buf + pi_data->interval);
		else
			pi = (struct nvme_64b_guard_pif *)(md_buf + pi_data->interval);

		/* [한국어] PI Escape 처리: 64b에서는 srtag가 48비트 모두 1인지를 fio_nvme_pi_ref_escape()로 확인. */
		if (data->pi_type == NVME_NS_DPS_PI_TYPE3) {
			if (pi->apptag == NVME_PI_APP_DISABLE &&
			    fio_nvme_pi_ref_escape(pi->srtag))
				goto next;
		} else if (data->pi_type == NVME_NS_DPS_PI_TYPE1 ||
			   data->pi_type == NVME_NS_DPS_PI_TYPE2) {
			if (pi->apptag == NVME_PI_APP_DISABLE)
				goto next;
		}

		/* [한국어] PRCHK_GUARD: CRC64 재계산 후 be64 비교. */
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

		/* [한국어] PRCHK_REF: 48비트 srtag를 읽어와 기대값(slba+blk & 48비트 마스크)와 비교. */
		if (pi_data->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
			switch (data->pi_type) {
			case NVME_NS_DPS_PI_TYPE1:
			case NVME_NS_DPS_PI_TYPE2:
				ref = get_unaligned_be48(pi->srtag);              /* [한국어] 비정렬 6바이트 be48 읽기. */
				ref_exp = (slba + lba_num) & ((1ULL << 48) - 1);  /* [한국어] 48비트로 마스킹한 기대값. */
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
 *
 * @param cmd: uring_cmd SQE에 매핑되는 nvme_uring_cmd. 호출자가 memset으로 0-초기화한 상태.
 * @param io_u: TRIM ddir을 가진 fio I/O 유닛. 단일/다중 범위 판정에 사용.
 * @param dsm: nvme_dsm_range 배열을 감싸는 DSM 페이로드 버퍼(엔진 측이 io_u 단위로 보유).
 * @return: 없음. cmd 필드(opcode, nsid, cdw10/11, addr, data_len)를 채우고 dsm->range[]를 작성.
 *
 * caller: fio_nvme_uring_cmd_prep() — io_u->ddir == DDIR_TRIM 분기.
 * callee: get_slba(), get_nlb(), FILE_ENG_DATA().
 *
 * DSM(Dataset Management) 명령으로 NVMe 디바이스에 deallocate 요청.
 * 단일 범위이면 io_u의 offset/length에서 직접 변환,
 * 다중 범위이면 io_u->xfer_buf에 저장된 trim_range 배열을 변환.
 * CDW10: Number of Ranges - 1 (0-based), CDW11: Attributes(=DEALLOCATE).
 *
 * 호출 체인:
 *   엔진 prep → fio_nvme_uring_cmd_prep → [fio_nvme_uring_cmd_trim_prep]
 */
static void fio_nvme_uring_cmd_trim_prep(struct nvme_uring_cmd *cmd, struct io_u *io_u,
					 struct nvme_dsm *dsm)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file); /* [한국어] fio_file에 엔진이 붙인 네임스페이스 속성 캐시 획득. */
	struct trim_range *range;                           /* [한국어] 다중 범위 trim 시 xfer_buf 내부 배열 원소 포인터. */
	uint8_t *buf_point;                                 /* [한국어] xfer_buf 순회 커서. */
	int i;                                              /* [한국어] 범위 인덱스. */

	cmd->opcode = nvme_cmd_dsm;                         /* [한국어] NVMe I/O opcode 0x09 = Dataset Management. */
	cmd->nsid = data->nsid;                             /* [한국어] 타깃 네임스페이스 ID. */
	cmd->cdw11 = NVME_ATTRIBUTE_DEALLOCATE;             /* [한국어] Attributes: AD 비트 — Deallocate 요청. */
	cmd->addr = (__u64) (uintptr_t) (&dsm->range[0]);   /* [한국어] DSM range 배열의 사용자 공간 주소(커널이 카피). */

	if (dsm->nr_ranges == 1) {
		/* [한국어] 단일 범위: io_u의 offset/xfer_buflen을 그대로 DSM range로 환산. */
		dsm->range[0].slba = get_slba(data, io_u->offset);
		/* nlb is a 1-based value for deallocate */
		dsm->range[0].nlb = get_nlb(data, io_u->xfer_buflen) + 1; /* [한국어] DSM NLB는 1-based라 +1. */
		cmd->cdw10 = 0;                             /* [한국어] NR(Number of Ranges) - 1 = 0 → 1개 범위. */
		cmd->data_len = sizeof(struct nvme_dsm_range); /* [한국어] 페이로드 길이 = range 구조체 하나. */
	} else {
		/* [한국어] 다중 범위: io_u->xfer_buf가 trim_range[]로 채워져 있음(fio 상위가 준비). */
		buf_point = io_u->xfer_buf;
		for (i = 0; i < io_u->number_trim; i++) {
			range = (struct trim_range *)buf_point; /* [한국어] 현재 오프셋의 trim_range 해석. */
			dsm->range[i].slba = get_slba(data, range->start); /* [한국어] 시작 LBA 변환. */
			/* nlb is a 1-based value for deallocate */
			dsm->range[i].nlb = get_nlb(data, range->len) + 1; /* [한국어] 길이 → NLB(1-based). */
			buf_point += sizeof(struct trim_range); /* [한국어] 다음 원소로 이동. */
		}
		cmd->cdw10 = io_u->number_trim - 1;         /* [한국어] NR(0-based) = 범위 수 - 1. */
		cmd->data_len = io_u->number_trim * sizeof(struct nvme_dsm_range); /* [한국어] 배열 전체 바이트. */
	}
}

/*
 * [한국어] NVMe passthrough 명령 준비 (메인 진입점)
 *
 * @param cmd: io_uring SQE(opcode=IORING_OP_URING_CMD)에 실릴 nvme_uring_cmd 버퍼. 호출자는
 *             SQE 내부에 위치하는 버퍼 주소를 넘김. 이 함수가 전부 기록(mset+필드).
 * @param io_u: fio I/O 유닛(offset/xfer_buflen/xfer_buf/mmap_data/ddir/dtype/dspec 등).
 * @param iov: NULL이면 단일 버퍼(data_len=바이트), 비-NULL이면 vectored I/O(IORING_OP_URING_CMD_VEC).
 * @param dsm: TRIM 시 사용할 DSM range 풀.
 * @param read_opcode: NVMe READ opcode(보통 0x02 or custom — 엔진 옵션에 따름).
 * @param write_opcode: NVMe WRITE opcode(보통 0x01, write_zeroes는 0x08).
 * @param cdw12_flags: CDW12에 OR할 부가 비트(PRCHK 등 PI 플래그).
 * @return: 0 성공, -ENOTSUP 지원되지 않는 ddir.
 *
 * caller: engines/io_uring.c(io_uring_cmd 엔진)의 prep 콜백,
 *         xnvme/nvme-passthru 래퍼 엔진들.
 * callee: fio_nvme_uring_cmd_trim_prep(), get_slba(), get_nlb(), memset().
 *
 * io_u의 ddir에 따라:
 * - READ: read_opcode
 * - WRITE: write_opcode
 * - TRIM: fio_nvme_uring_cmd_trim_prep()으로 위임
 * - SYNC/DATASYNC: flush 명령 (opcode=0x00)
 *
 * CDW10~11: 시작 LBA (64비트를 상/하위 32비트로 분할)
 * CDW12: NLB(0-based) | dtype(비트 20~23) | cdw12_flags(PI 플래그 등)
 * CDW13: DSPEC(비트 16~31) - FDP 데이터 배치 지정
 * iov가 NULL이 아니면 vectored I/O, 아니면 단일 버퍼
 * write_zeroes 명령은 데이터 버퍼 불필요
 *
 * 호출 체인:
 *   td_io_prep → ioengine_ops->prep → [fio_nvme_uring_cmd_prep]
 *     → (TRIM) fio_nvme_uring_cmd_trim_prep
 */
int fio_nvme_uring_cmd_prep(struct nvme_uring_cmd *cmd, struct io_u *io_u,
			    struct iovec *iov, struct nvme_dsm *dsm,
			    uint8_t read_opcode, uint8_t write_opcode,
			    unsigned int cdw12_flags)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file); /* [한국어] 네임스페이스 포맷 캐시. */
	__u64 slba;                                         /* [한국어] 시작 LBA. */
	__u32 nlb;                                          /* [한국어] NLB(0-based 저장 형식). */

	memset(cmd, 0, sizeof(struct nvme_uring_cmd));      /* [한국어] SQE 내부 버퍼 0-초기화 — 남아있는 이전 명령 비트 제거. */

	switch (io_u->ddir) {
	case DDIR_READ:
		cmd->opcode = read_opcode;                  /* [한국어] 엔진이 지정한 READ opcode(기본 nvme_cmd_read=0x02). */
		break;
	case DDIR_WRITE:
		cmd->opcode = write_opcode;                 /* [한국어] WRITE opcode(write_zeroes=0x08일 수도). */
		break;
	case DDIR_TRIM:
		fio_nvme_uring_cmd_trim_prep(cmd, io_u, dsm); /* [한국어] DSM 경로로 위임하고 즉시 종료. */
		return 0;
	case DDIR_SYNC:
	case DDIR_DATASYNC:
		cmd->opcode = nvme_cmd_flush;               /* [한국어] NVMe Flush(0x00) — 휘발 쓰기 버퍼 flush. */
		cmd->nsid = data->nsid;
		return 0;
	default:
		return -ENOTSUP;                            /* [한국어] ddir 스펙 밖(VERIFY 등) — 엔진이 다른 경로로 처리. */
	}

	slba = get_slba(data, io_u->offset);                /* [한국어] byte offset → LBA. */
	nlb = get_nlb(data, io_u->xfer_buflen);             /* [한국어] 길이 → NLB(0-based 그대로 저장). */

	/* cdw10 and cdw11 represent starting lba */
	cmd->cdw10 = slba & 0xffffffff;                     /* [한국어] SLBA 하위 32b. */
	cmd->cdw11 = slba >> 32;                            /* [한국어] SLBA 상위 32b. */
	/* cdw12 represent number of lba's for read/write */
	/* [한국어] CDW12: [15:0] NLB(0-based), [23:20] DTYPE(FDP 등), 그 외 상위 비트는 PRINFO/PRCHK 플래그(cdw12_flags). */
	cmd->cdw12 = nlb | (io_u->dtype << 20) | cdw12_flags;
	cmd->cdw13 = io_u->dspec << 16;                     /* [한국어] CDW13 [31:16] DSPEC — FDP 배치 식별자(Placement Identifier). */
	if (iov) {
		/* [한국어] vectored I/O: SQE에 iovec을 지정. data_len은 iov 개수(여기서는 1). */
		iov->iov_base = io_u->xfer_buf;
		iov->iov_len = io_u->xfer_buflen;
		cmd->addr = (__u64)(uintptr_t)iov;
		cmd->data_len = 1;
	} else {
		/* no buffer for write zeroes */
		if (cmd->opcode != nvme_cmd_write_zeroes)
			cmd->addr = (__u64)(uintptr_t)io_u->xfer_buf; /* [한국어] 일반 read/write: 사용자 버퍼 주소. */
		else
			cmd->addr = (__u64)(uintptr_t)NULL;           /* [한국어] write_zeroes는 패턴 생성이라 데이터 버퍼 불필요. */
		cmd->data_len = io_u->xfer_buflen;          /* [한국어] 단일 버퍼 길이(바이트). */
	}
	if (data->lba_shift && data->ms) {
		/* [한국어] 별도 메타가 있는 포맷: 커널에 메타 버퍼/길이 전달. 확장 LBA면 lba_shift=0이라 진입하지 않음. */
		cmd->metadata = (__u64)(uintptr_t)io_u->mmap_data;
		cmd->metadata_len = (nlb + 1) * data->ms;   /* [한국어] 블록 수 × 메타 크기. nlb는 0-based이므로 +1. */
	}
	cmd->nsid = data->nsid;                             /* [한국어] 타깃 네임스페이스. */
	return 0;
}

/*
 * [한국어] 가드 태그 생성 디스패처
 *
 * @param io_u: PI 대상 I/O 유닛.
 * @param opts: io_flags(PRACT/PRCHK_*), apptag/apptag_mask.
 * @return: 없음.
 *
 * caller: fio_nvme_pi_fill() — prep 단계에서 CDW12에 PI 플래그를 OR한 뒤 호출.
 * callee: fio_nvme_generate_pi_16b_guard() 또는 fio_nvme_generate_pi_64b_guard().
 *
 * PI가 활성화되고 PRACT=0(호스트가 PI 처리)일 때만 동작.
 * 가드 유형(16비트/64비트)에 따라 적절한 생성 함수를 호출.
 * PRACT=1일 경우 컨트롤러가 PI를 직접 생성/제거하므로 호스트는 건드리지 않는다.
 *
 * 호출 체인:
 *   엔진 prep → fio_nvme_pi_fill → [fio_nvme_generate_guard]
 *     → fio_nvme_generate_pi_{16b,64b}_guard
 */
void fio_nvme_generate_guard(struct io_u *io_u, struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file); /* [한국어] 네임스페이스 PI 포맷 조회. */

	/* [한국어] pi_type!=0 (PI 포맷 사용)이고 PRACT 비트가 꺼져 있어야 호스트 PI 생성을 수행. */
	if (data->pi_type && !(opts->io_flags & NVME_IO_PRINFO_PRACT)) {
		if (data->guard_type == NVME_NVM_NS_16B_GUARD)
			fio_nvme_generate_pi_16b_guard(data, io_u, opts); /* [한국어] T10 DIF CRC16 경로. */
		else if (data->guard_type == NVME_NVM_NS_64B_GUARD)
			fio_nvme_generate_pi_64b_guard(data, io_u, opts); /* [한국어] NVMe CRC64 경로. */
	}
}

/*
 * [한국어] NVMe 명령에 PI 관련 필드 설정
 *
 * @param cmd: 이미 fio_nvme_uring_cmd_prep()으로 기본 필드가 채워진 nvme_uring_cmd.
 * @param io_u: 대상 I/O 유닛.
 * @param opts: io_flags(PRINFO 비트), apptag/apptag_mask.
 * @return: 없음. cdw3/cdw12/cdw14/cdw15를 PI 정보로 갱신.
 *
 * caller: io_uring_cmd 엔진의 prep(옵션에서 PI가 켜진 경우).
 * callee: fio_nvme_generate_guard(), get_slba().
 *
 * 1. CDW12에 io_flags(PRCHK_GUARD/APP/REF, PRACT)를 OR로 추가
 * 2. 가드 태그를 생성 (fio_nvme_generate_guard)
 * 3. PI 유형과 가드 유형에 따라:
 *    - CDW14: 초기 참조 태그 (시작 LBA)
 *    - CDW3: 64비트 가드의 경우 참조 태그 상위 16비트
 *    - CDW15: 앱 태그 마스크(상위 16비트) | 앱 태그(하위 16비트)
 *
 * 호출 체인:
 *   엔진 prep → fio_nvme_uring_cmd_prep → [fio_nvme_pi_fill] → fio_nvme_generate_guard
 */
void fio_nvme_pi_fill(struct nvme_uring_cmd *cmd, struct io_u *io_u,
		      struct nvme_cmd_ext_io_opts *opts)
{
	struct nvme_data *data = FILE_ENG_DATA(io_u->file); /* [한국어] 네임스페이스 PI 포맷 캐시. */
	__u64 slba;                                         /* [한국어] 시작 LBA — 초기 참조 태그. */

	slba = get_slba(data, io_u->offset);
	cmd->cdw12 |= opts->io_flags;                       /* [한국어] PRACT/PRCHK_* 비트를 CDW12에 합성(기존 NLB/DTYPE는 유지). */

	fio_nvme_generate_guard(io_u, opts);                /* [한국어] 호스트 측 PI 데이터(guard/apptag/srtag) 기록. */

	switch (data->pi_type) {
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		/* [한국어] Type1/2: 참조 태그 초기값을 CDW14(+64b 가드면 CDW3)에 지정 — 블록마다 +1 증가해 디바이스가 검증. */
		switch (data->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF)
				cmd->cdw14 = (__u32)slba;           /* [한국어] 16b 가드: 초기 reftag 32b. */
			break;
		case NVME_NVM_NS_64B_GUARD:
			if (opts->io_flags & NVME_IO_PRINFO_PRCHK_REF) {
				cmd->cdw14 = (__u32)slba;           /* [한국어] 64b 가드: 초기 reftag 하위 32b. */
				cmd->cdw3 = ((slba >> 32) & 0xffff); /* [한국어] 64b 가드: 상위 16b(총 48b 참조 태그). */
			}
			break;
		default:
			break;
		}
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			cmd->cdw15 = (opts->apptag_mask << 16 | opts->apptag); /* [한국어] [31:16] LBAT_M, [15:0] LBAT — apptag 검사 파라미터. */
		break;
	case NVME_NS_DPS_PI_TYPE3:
		/* [한국어] Type3: 참조 태그는 디바이스가 임의값 사용 — apptag만 호스트가 지정. */
		if (opts->io_flags & NVME_IO_PRINFO_PRCHK_APP)
			cmd->cdw15 = (opts->apptag_mask << 16 | opts->apptag);
		break;
	case NVME_NS_DPS_PI_NONE:                           /* [한국어] PI 미사용 포맷 — 아무것도 하지 않음. */
		break;
	}
}

/*
 * [한국어] PI 검증 디스패처
 *
 * @param data: 네임스페이스 PI 포맷 캐시(guard_type 확인).
 * @param io_u: 완료된 읽기 io_u.
 * @return: 0=성공, -EIO=무결성 검사 실패.
 *
 * caller: io_uring_cmd 엔진 event()/commit 완료 처리 — verify 옵션이 켜진 경우.
 * callee: fio_nvme_verify_pi_{16b,64b}_guard.
 *
 * 가드 유형(16비트/64비트)에 따라 적절한 검증 함수를 호출.
 *
 * 호출 체인:
 *   엔진 완료 경로 → [fio_nvme_pi_verify] → fio_nvme_verify_pi_{16b,64b}_guard
 */
int fio_nvme_pi_verify(struct nvme_data *data, struct io_u *io_u)
{
	int ret = 0;                                      /* [한국어] 기본 성공. 알 수 없는 guard_type이면 그냥 0 반환. */

	switch (data->guard_type) {
	case NVME_NVM_NS_16B_GUARD:
		ret = fio_nvme_verify_pi_16b_guard(data, io_u); /* [한국어] T10 DIF CRC16 검증. */
		break;
	case NVME_NVM_NS_64B_GUARD:
		ret = fio_nvme_verify_pi_64b_guard(data, io_u); /* [한국어] NVMe CRC64 검증. */
		break;
	default:
		break;                                      /* [한국어] 가드 유형 미지정(PI 없음) — 검증 스킵. */
	}

	return ret;
}

/*
 * [한국어] NVMe Admin Identify 명령 발행
 *
 * @param fd: /dev/ngXnY(또는 /dev/nvmeX) 파일 디스크립터.
 * @param nsid: 네임스페이스 ID(0=컨트롤러 글로벌).
 * @param cns: Identify CNS 필드 — 어떤 구조체를 원하는지(Controller/Namespace/NS Descriptor/…).
 * @param csi: Command Set Identifier(NVM/ZNS/KV 등).
 * @param data: 4096바이트(NVME_IDENTIFY_DATA_SIZE) 응답 버퍼.
 * @return: 0 성공, 음수 오류(ioctl 실패) — 호출자가 log_err 후 에러 경로 진입.
 *
 * caller: fio_nvme_get_info(), fio_nvme_get_zoned_model(), fio_nvme_report_zones(),
 *         fio_nvme_get_max_open_zones() 등 디바이스 속성 질의 경로 전반.
 * callee: ioctl(NVME_IOCTL_ADMIN_CMD) → 커널 nvme-driver의 Admin Submission Queue로 전달.
 *
 * NVME_IOCTL_ADMIN_CMD ioctl을 통해 컨트롤러에 Identify 명령을 보냄.
 * CNS(Controller or Namespace Structure)와 CSI(Command Set Identifier)에 따라
 * 컨트롤러/네임스페이스/커맨드세트별 정보를 조회.
 *
 * 실행 컨텍스트: 잡 스레드(또는 초기화 스레드)의 사용자 공간에서 동기 호출. ioctl이 돌아올
 * 때까지 블록. 드라이버가 Admin 큐에 제출 → 완료 → 응답 버퍼 복사.
 */
static int nvme_identify(int fd, __u32 nsid, enum nvme_identify_cns cns,
			 enum nvme_csi csi, void *data)
{
	/* [한국어] NVMe Admin Identify 커맨드 템플릿. 스택에 매번 새로 만든다(재사용 없음). */
	struct nvme_passthru_cmd cmd = {
		.opcode         = nvme_admin_identify,          /* [한국어] Admin opcode 0x06 = Identify. */
		.nsid           = nsid,                         /* [한국어] 대상 네임스페이스. 0이면 컨트롤러 수준. */
		.addr           = (__u64)(uintptr_t)data,       /* [한국어] 응답 DMA 대상 사용자 버퍼. */
		.data_len       = NVME_IDENTIFY_DATA_SIZE,      /* [한국어] 항상 4096바이트. */
		.cdw10          = cns,                          /* [한국어] CDW10 [7:0] CNS. */
		.cdw11          = csi << NVME_IDENTIFY_CSI_SHIFT,/* [한국어] CDW11 [31:24] CSI. */
		.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,   /* [한국어] 60초 기본 타임아웃(느린 디바이스 보호). */
	};

	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);           /* [한국어] 커널로 Admin 패스스루 제출 — 동기. */
}

/*
 * [한국어] NVMe 디바이스 정보 조회 (엔진 초기화 시 호출)
 *
 * @param f: fio_file — NVMe 캐릭터 디바이스를 가리켜야 함(FIO_TYPE_CHAR).
 * @param nlba: 네임스페이스 크기(LBA 단위)를 반환할 포인터. 호출자가 파일 크기로 환산.
 * @param pi_act: PRACT 활성 여부(1이면 ms==pi_size일 때 ms=0 처리).
 * @param data: 채워질 nvme_data(호출자가 fio_file의 engine_data로 보관).
 * @return: 0 성공, 1 비지원 파일타입, 음수 errno 또는 ioctl 오류.
 *
 * caller: io_uring_cmd/xnvme 엔진의 open_file/init 단계(엔진별 setup 경로).
 * callee: open(), close(), ioctl(NVME_IOCTL_ID), nvme_identify(), ilog2(), log_err().
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
 *
 * 호출 체인:
 *   ioengine_ops->init 또는 open_file → [fio_nvme_get_info] → nvme_identify
 */
int fio_nvme_get_info(struct fio_file *f, __u64 *nlba, __u32 pi_act,
		      struct nvme_data *data)
{
	struct nvme_id_ns ns;                              /* [한국어] Identify Namespace 응답 버퍼(4KB). */
	struct nvme_id_ctrl ctrl;                          /* [한국어] Identify Controller 응답(ctratt 등). */
	struct nvme_nvm_id_ns nvm_ns;                      /* [한국어] NVM 전용 Identify NS(ELBAS 지원 시). */
	int namespace_id;                                  /* [한국어] NVME_IOCTL_ID 결과로 획득한 NSID. */
	int fd, err;                                       /* [한국어] 디바이스 fd, 반환 에러 코드. */
	__u32 format_idx, elbaf;                           /* [한국어] LBA 포맷 인덱스와 extended LBA 포맷 비트필드. */

	/* [한국어] 이 엔진은 /dev/ngXnY(캐릭터 디바이스)만 지원. 블록 디바이스(/dev/nvmeXnY)는 불가. */
	if (f->filetype != FIO_TYPE_CHAR) {
		log_err("ioengine io_uring_cmd only works with nvme ns "
			"generic char devices (/dev/ngXnY)\n");
		return 1;
	}

	fd = open(f->file_name, O_RDONLY);                 /* [한국어] 읽기 전용 open — Identify만 할 거라 충분. */
	if (fd < 0)
		return -errno;                             /* [한국어] open 실패 시 호출자에게 errno 역부호 전달. */

	namespace_id = ioctl(fd, NVME_IOCTL_ID);           /* [한국어] 커널이 이 fd에 바인딩된 NSID를 반환. */
	if (namespace_id < 0) {
		err = -errno;
		log_err("%s: failed to fetch namespace-id\n", f->file_name);
		goto out;                                  /* [한국어] 아래 close 공통 cleanup. */
	}

	/* [한국어] Identify Controller: CNS=0x01, NSID=0. ctratt(컨트롤러 속성)에서 ELBAS 지원 여부 확인. */
	err = nvme_identify(fd, 0, NVME_IDENTIFY_CNS_CTRL, NVME_CSI_NVM, &ctrl);
	if (err) {
		log_err("%s: failed to fetch identify ctrl\n", f->file_name);
		goto out;
	}

	/*
	 * Identify namespace to get namespace-id, namespace size in LBA's
	 * and LBA data size.
	 */
	/* [한국어] Identify Namespace: CNS=0x00, NSID=나. nsze/lbaf[]/flbas/dps 등 포맷 정보 취득. */
	err = nvme_identify(fd, namespace_id, NVME_IDENTIFY_CNS_NS,
				NVME_CSI_NVM, &ns);
	if (err) {
		log_err("%s: failed to fetch identify namespace\n",
			f->file_name);
		goto out;
	}

	data->nsid = namespace_id;                         /* [한국어] 이후 모든 명령(CDW0의 NSID)에서 사용. */

	/*
	 * 16 or 64 as maximum number of supported LBA formats.
	 * From flbas bit 0-3 indicates lsb and bit 5-6 indicates msb
	 * of the format index used to format the namespace.
	 */
	/* [한국어] NVMe 1.4 이후 포맷 인덱스 확장(최대 64개). nlbaf<16이면 기존 4비트 필드만. */
	if (ns.nlbaf < 16)
		format_idx = ns.flbas & 0xf;
	else
		format_idx = (ns.flbas & 0xf) + (((ns.flbas >> 5) & 0x3) << 4); /* [한국어] MSB 2비트를 bit5-6에서 추출해 합성. */

	data->lba_size = 1 << ns.lbaf[format_idx].ds;      /* [한국어] ds는 LBA 크기의 log2. 예: 12 → 4096B. */
	data->ms = le16_to_cpu(ns.lbaf[format_idx].ms);    /* [한국어] 블록당 메타데이터 바이트 수(리틀엔디안 → 호스트). */

	/* Check for end to end data protection support */
	/* [한국어] dps(Data Protection Settings)의 PI_MASK(하위 3비트)로 PI 유형(Type1/2/3) 판정. */
	if (data->ms && (ns.dps & NVME_NS_DPS_PI_MASK))
		data->pi_type = (ns.dps & NVME_NS_DPS_PI_MASK);

	if (!data->pi_type)
		goto check_elba;                           /* [한국어] PI 없음 → 가드 조사 스킵하고 확장 LBA 판정만. */

	/* [한국어] ELBAS(Extended LBA Support): 64b 가드 지원 여부를 위한 NVM CSI Identify 필요. */
	if (ctrl.ctratt & NVME_CTRL_CTRATT_ELBAS) {
		err = nvme_identify(fd, namespace_id, NVME_IDENTIFY_CNS_CSI_NS,
					NVME_CSI_NVM, &nvm_ns);
		if (err) {
			log_err("%s: failed to fetch identify nvm namespace\n",
				f->file_name);
			goto out;
		}

		elbaf = le32_to_cpu(nvm_ns.elbaf[format_idx]); /* [한국어] Extended LBA Format: guard_type/storage_tag 등 포함. */

		/* Currently we don't support storage tags */
		if (elbaf & NVME_ID_NS_NVM_STS_MASK) {     /* [한국어] storage tag size != 0 → 미지원 에러. */
			log_err("%s: Storage tag not supported\n",
				f->file_name);
			err = -ENOTSUP;
			goto out;
		}

		data->guard_type = (elbaf >> NVME_ID_NS_NVM_GUARD_SHIFT) &
				NVME_ID_NS_NVM_GUARD_MASK;  /* [한국어] 가드 타입 추출(16b/32b/64b — 32b는 미지원). */

		/* No 32 bit guard, as storage tag is mandatory for it */
		switch (data->guard_type) {
		case NVME_NVM_NS_16B_GUARD:
			data->pi_size = sizeof(struct nvme_16b_guard_pif); /* [한국어] 8바이트. */
			break;
		case NVME_NVM_NS_64B_GUARD:
			data->pi_size = sizeof(struct nvme_64b_guard_pif); /* [한국어] 16바이트. */
			break;
		default:
			break;
		}
	} else {
		/* [한국어] ELBAS 미지원 레거시 컨트롤러: 무조건 16b T10 DIF 가드로 간주. */
		data->guard_type = NVME_NVM_NS_16B_GUARD;
		data->pi_size = sizeof(struct nvme_16b_guard_pif);
	}

	/*
	 * when PRACT bit is set to 1, and metadata size is equal to protection
	 * information size, controller inserts and removes PI for write and
	 * read commands respectively.
	 */
	/* [한국어] PRACT 모드: 컨트롤러가 PI만 다루는 'pure PI metadata' 포맷이면 호스트 입장에서는 메타 없음으로 취급. */
	if (pi_act && data->ms == data->pi_size)
		data->ms = 0;

	data->pi_loc = (ns.dps & NVME_NS_DPS_PI_FIRST);    /* [한국어] PI 위치(0=메타 끝, 1=메타 앞). */

check_elba:
	/*
	 * Bit 4 for flbas indicates if metadata is transferred at the end of
	 * logical block creating an extended LBA.
	 */
	/* [한국어] flbas 비트4=1이면 확장 LBA(데이터+메타 연속). 0이면 별도 메타 버퍼. */
	if (data->ms && ((ns.flbas >> 4) & 0x1))
		data->lba_ext = data->lba_size + data->ms; /* [한국어] 확장 LBA 1개 블록 크기. */
	else
		data->lba_shift = ilog2(data->lba_size);   /* [한국어] offset ↔ LBA 변환용 shift값. 확장 LBA면 0으로 남아 get_slba가 나눗셈 사용. */

	*nlba = ns.nsze;                                   /* [한국어] Namespace Size(LBA 수) 반환. fio가 파일 크기 계산. */

out:
	close(fd);                                         /* [한국어] 초기화용 fd 해제 — 실제 I/O는 엔진 open_file이 별도 open. */
	return err;
}

/*
 * [한국어] NVMe ZNS 디바이스 모델 확인
 *
 * @param td: thread_data — 현재 호출 스레드 컨텍스트(여기서 직접 사용하지는 않음).
 * @param f: 대상 fio_file(NVMe 캐릭터 디바이스).
 * @param model: 반환 슬롯 — ZBD_HOST_MANAGED 또는 ZBD_NONE.
 * @return: 0 성공, -EINVAL 비지원 파일 타입.
 *
 * caller: ZBD 프레임워크(zbd.c) — fio_zbd_get_zoned_model().
 * callee: open(), close(), nvme_identify()(CSI=ZNS).
 *
 * ZNS CSI로 Identify Controller/Namespace를 시도하여
 * 성공하면 ZBD_HOST_MANAGED, 실패하면 ZBD_NONE 반환.
 * fio의 ZBD(Zoned Block Device) 프레임워크에서 호출됨.
 *
 * 호출 체인:
 *   zbd_setup → fio_zbd_get_zoned_model → [fio_nvme_get_zoned_model] → nvme_identify
 */
int fio_nvme_get_zoned_model(struct thread_data *td, struct fio_file *f,
			     enum zbd_zoned_model *model)
{
	struct nvme_data *data = FILE_ENG_DATA(f);         /* [한국어] nsid를 얻기 위해 기존 캐시 사용. */
	struct nvme_id_ns ns;                              /* [한국어] 응답 버퍼(ZNS id도 같은 크기라 재사용). */
	struct nvme_passthru_cmd cmd;                      /* [한국어] 아래 memset만 하고 실제 사용되지는 않음(상위 스타일 유지용). */
	int fd, ret = 0;

	if (f->filetype != FIO_TYPE_CHAR)                  /* [한국어] 캐릭터 디바이스만 지원. */
		return -EINVAL;

	/* File is not yet opened */
	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);   /* [한국어] O_LARGEFILE: 대용량 디바이스 대응. */
	if (fd < 0)
		return -errno;

	/* Using nvme_id_ns for data as sizes are same */
	/* [한국어] ZNS Identify Controller(CSI=ZNS, CNS=CSI_CTRL). 실패=ZNS 미지원. */
	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_CTRL,
				NVME_CSI_ZNS, &ns);
	if (ret) {
		*model = ZBD_NONE;
		goto out;
	}

	memset(&cmd, 0, sizeof(struct nvme_passthru_cmd)); /* [한국어] 관례적 초기화(실제 미사용). */

	/* Using nvme_id_ns for data as sizes are same */
	/* [한국어] ZNS Identify Namespace도 성공해야 진짜 ZNS NS로 인정. */
	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_NS,
				NVME_CSI_ZNS, &ns);
	if (ret) {
		*model = ZBD_NONE;
		goto out;
	}

	*model = ZBD_HOST_MANAGED;                         /* [한국어] NVMe ZNS는 호스트 관리 모델. */
out:
	close(fd);
	return 0;                                          /* [한국어] 호출자는 *model로 판단. 실패도 0 반환(진단 정보는 *model). */
}

/*
 * [한국어] NVMe ZNS Zone Management Receive (존 리포트) 명령 발행
 *
 * @param fd: 디바이스 fd.
 * @param nsid: 네임스페이스 ID.
 * @param slba: 리포트 시작 LBA.
 * @param zras_feat: Zone Receive Action Specific Features(NVME_ZNS_ZRAS_FEAT_ERZ 등 비트).
 * @param data_len: 응답 버퍼 바이트. CDW12에는 (DWORD 단위 - 1)로 인코딩.
 * @param data: nvme_zone_report + entries[]를 담을 버퍼.
 * @return: ioctl 반환값(0 성공).
 *
 * caller: fio_nvme_report_zones() — chunk 단위 루프.
 * callee: ioctl(NVME_IOCTL_IO_CMD) → 커널 NVMe I/O 큐로 전달.
 *
 * slba부터 시작하는 존들의 상태를 data_len 크기만큼 조회.
 * zras_feat에 NVME_ZNS_ZRAS_FEAT_ERZ를 설정하면 빈 존도 포함.
 */
static int nvme_report_zones(int fd, __u32 nsid, __u64 slba, __u32 zras_feat,
			     __u32 data_len, void *data)
{
	/* [한국어] ZNS 스펙 — Zone Management Receive 커맨드(opcode 0x7A) 구성. */
	struct nvme_passthru_cmd cmd = {
		.opcode         = nvme_zns_cmd_mgmt_recv,  /* [한국어] ZNS I/O opcode. */
		.nsid           = nsid,
		.addr           = (__u64)(uintptr_t)data,  /* [한국어] 응답 DMA 버퍼. */
		.data_len       = data_len,
		.cdw10          = slba & 0xffffffff,       /* [한국어] SLBA 하위 32b. */
		.cdw11          = slba >> 32,              /* [한국어] SLBA 상위 32b. */
		.cdw12		= (data_len >> 2) - 1,     /* [한국어] NUMD = (바이트>>2) - 1 — DWORD 기반 0-based. */
		.cdw13		= NVME_ZNS_ZRA_REPORT_ZONES | zras_feat, /* [한국어] [7:0] ZRA(=Report Zones), [15:8] ZRAS feat, [16] PR(Partial Report). */
		.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,
	};

	return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);         /* [한국어] I/O 큐 경유 패스스루. */
}

/*
 * [한국어] NVMe ZNS 존 리포트 조회 및 fio 형식 변환
 *
 * @param td: 현재 스레드 컨텍스트(미사용 — 시그니처 호환).
 * @param f: 대상 파일(ZNS 캐릭터 디바이스).
 * @param offset: 바이트 오프셋 — 여기서부터 nr_zones만큼 리포트.
 * @param zbdz: fio zbd_zone 배열(호출자가 nr_zones 크기로 할당).
 * @param nr_zones: 요청 존 수. 실제 존재 개수가 더 작으면 축소된다.
 * @return: 성공 시 조회된 존 수(>0), 실패 시 음수 오류.
 *
 * caller: ZBD 프레임워크(zbd.c/zbd_get_zoned_model 이후의 zbd_report_zones).
 * callee: nvme_identify()(ZNS CSI NS/NVM NS), nvme_report_zones(), calloc(), free().
 *
 * 동작 과정:
 * 1. ZNS Identify NS로 존 크기(zlen) 계산
 * 2. zones_chunks(기본 1024)씩 나누어 반복 조회 (대규모 존 수 처리)
 * 3. 각 nvme_zns_desc를 fio의 zbd_zone 구조체로 변환:
 *    - start, len, wp, capacity: LBA → 바이트 변환
 *    - zt(존 타입): SEQWRITE_REQ → ZBD_ZONE_TYPE_SWR
 *    - zs(존 상태): Empty/Open/Closed/Full → fio 존 상태로 매핑
 *
 * 호출 체인:
 *   zbd_init → zbd_create_zone_info → [fio_nvme_report_zones] → nvme_report_zones
 */
int fio_nvme_report_zones(struct thread_data *td, struct fio_file *f,
			  uint64_t offset, struct zbd_zone *zbdz,
			  unsigned int nr_zones)
{
	struct nvme_data *data = FILE_ENG_DATA(f);         /* [한국어] 기존 get_info 결과 — nsid/lba_shift 재사용. */
	struct nvme_zone_report *zr;                       /* [한국어] 동적 할당 리포트 버퍼(헤더 + entries[]). */
	struct nvme_zns_id_ns zns_ns;                      /* [한국어] ZNS CSI NS — zsze(존 크기) 얻기 위함. */
	struct nvme_id_ns ns;                              /* [한국어] NVM NS — flbas의 하위 4b로 포맷 인덱스 확인. */
	unsigned int i = 0, j, zones_fetched = 0;          /* [한국어] 전역 출력 인덱스(i), 청크 내 인덱스(j), 누적 카운트. */
	unsigned int max_zones, zones_chunks = 1024;       /* [한국어] 파일 크기로 제한되는 최대 존 수, 한 번에 요청할 청크 크기. */
	int fd, ret = 0;
	__u32 zr_len;                                      /* [한국어] 현재 청크의 DMA 길이. */
	__u64 zlen;                                        /* [한국어] 존 하나의 바이트 크기. */

	/* File is not yet opened */
	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	zones_fetched = 0;
	zr_len = sizeof(*zr) + (zones_chunks * sizeof(struct nvme_zns_desc)); /* [한국어] 헤더 + zones_chunks개의 desc. */
	zr = calloc(1, zr_len);                            /* [한국어] 0-초기화 할당(필요 시 한 번만 할당해 루프 재사용). */
	if (!zr) {
		close(fd);
		return -ENOMEM;
	}

	/* [한국어] flbas의 하위 4비트를 사용하기 위해 NVM NS identify. */
	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_NS,
				NVME_CSI_NVM, &ns);
	if (ret) {
		log_err("%s: nvme_identify_ns failed, err=%d\n", f->file_name,
			ret);
		goto out;
	}

	/* [한국어] ZNS NS identify: lbafe[포맷].zsze(LBA 단위 존 크기) 획득. */
	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_NS,
				NVME_CSI_ZNS, &zns_ns);
	if (ret) {
		log_err("%s: nvme_zns_identify_ns failed, err=%d\n",
			f->file_name, ret);
		goto out;
	}
	zlen = zns_ns.lbafe[ns.flbas & 0x0f].zsze << data->lba_shift; /* [한국어] LBA → 바이트(존 크기). */

	max_zones = (f->real_file_size - offset) / zlen;   /* [한국어] 남은 바이트로 표현 가능한 최대 존 수. */
	if (max_zones < nr_zones)
		nr_zones = max_zones;                      /* [한국어] 요청값이 실제보다 크면 축소. */

	if (nr_zones < zones_chunks)
		zones_chunks = nr_zones;                   /* [한국어] 청크가 남은 존보다 크면 축소. */

	while (zones_fetched < nr_zones) {
		/* [한국어] 마지막 청크는 남은 수로 축소해 버퍼 낭비 없이 요청. */
		if (zones_fetched + zones_chunks >= nr_zones) {
			zones_chunks = nr_zones - zones_fetched;
			zr_len = sizeof(*zr) + (zones_chunks * sizeof(struct nvme_zns_desc));
		}
		/* [한국어] Report Zones with ERZ — 빈 존도 포함해 끊김 없이 리포트. offset을 LBA로 환산. */
		ret = nvme_report_zones(fd, data->nsid, offset >> data->lba_shift,
					NVME_ZNS_ZRAS_FEAT_ERZ, zr_len, (void *)zr);
		if (ret) {
			log_err("%s: nvme_zns_report_zones failed, err=%d\n",
				f->file_name, ret);
			goto out;
		}

		/* Transform the zone-report */
		/* [한국어] NVMe zns_desc → fio zbd_zone 구조체로 필드별 변환(LBA → byte, 타입/상태 코드 매핑). */
		for (j = 0; j < zr->nr_zones; j++, i++) {
			struct nvme_zns_desc *desc = (struct nvme_zns_desc *)&(zr->entries[j]);

			zbdz[i].start = desc->zslba << data->lba_shift;    /* [한국어] 존 시작 바이트. */
			zbdz[i].len = zlen;                                /* [한국어] 존 크기(모든 존 동일 가정 — 전형적 ZNS). */
			zbdz[i].wp = desc->wp << data->lba_shift;          /* [한국어] write pointer 바이트. */
			zbdz[i].capacity = desc->zcap << data->lba_shift;  /* [한국어] 유효 용량(zsze보다 작을 수 있음). */

			/* Zone Type is stored in first 4 bits. */
			switch (desc->zt & 0x0f) {
			case NVME_ZONE_TYPE_SEQWRITE_REQ:
				zbdz[i].type = ZBD_ZONE_TYPE_SWR;  /* [한국어] Sequential Write Required. */
				break;
			default:
				/* [한국어] 정의되지 않은 타입은 오류 — 현재 스펙은 SWR만. */
				log_err("%s: invalid type for zone at offset %llu.\n",
					f->file_name, (unsigned long long) desc->zslba);
				ret = -EIO;
				goto out;
			}

			/* Zone State is stored in last 4 bits. */
			switch (desc->zs >> 4) {
			case NVME_ZNS_ZS_EMPTY:
				zbdz[i].cond = ZBD_ZONE_COND_EMPTY;    /* [한국어] 비어있음(wp=start). */
				break;
			case NVME_ZNS_ZS_IMPL_OPEN:
				zbdz[i].cond = ZBD_ZONE_COND_IMP_OPEN; /* [한국어] 암묵적 오픈(write로 인해 자동 오픈). */
				break;
			case NVME_ZNS_ZS_EXPL_OPEN:
				zbdz[i].cond = ZBD_ZONE_COND_EXP_OPEN; /* [한국어] 명시적 Open Zone 명령으로 오픈. */
				break;
			case NVME_ZNS_ZS_CLOSED:
				zbdz[i].cond = ZBD_ZONE_COND_CLOSED;   /* [한국어] 닫힘(쓰기 중단/리소스 반납). */
				break;
			case NVME_ZNS_ZS_FULL:
				zbdz[i].cond = ZBD_ZONE_COND_FULL;     /* [한국어] 가득 참. */
				break;
			case NVME_ZNS_ZS_READ_ONLY:
			case NVME_ZNS_ZS_OFFLINE:
			default:
				/* Treat all these conditions as offline (don't use!) */
				/* [한국어] 읽기 전용/오프라인/미지정: fio에서 사용 불가로 처리. wp=start로 안전값. */
				zbdz[i].cond = ZBD_ZONE_COND_OFFLINE;
				zbdz[i].wp = zbdz[i].start;
			}
		}
		zones_fetched += zr->nr_zones;                     /* [한국어] 이번 청크 결과 누적. */
		offset += zr->nr_zones * zlen;                     /* [한국어] 다음 청크 시작 바이트 오프셋 전진. */
	}

	ret = zones_fetched;                                       /* [한국어] 성공 시 총 조회 수를 반환. */
out:
	free(zr);
	close(fd);

	return ret;
}

/*
 * [한국어] ZNS 존 쓰기 포인터(Write Pointer) 리셋
 *
 * @param td: thread_data — 잡 옵션(zone_size)을 사용.
 * @param f: 대상 fio_file. 열려있으면 기존 fd 재사용, 아니면 임시 open.
 * @param offset: 리셋 시작 바이트 오프셋(존 경계에 정렬되어야 함).
 * @param length: 리셋할 바이트 길이. zone_size 단위로 올림하여 존 수 계산.
 * @return: 0 성공, 음수 — 마지막 ioctl의 -errno(ret는 ioctl 반환값의 부호 반전).
 *
 * caller: ZBD 프레임워크의 zone reset 경로(zbd.c/zbd_reset_zone).
 * callee: open(), close(), ioctl(NVME_IOCTL_IO_CMD).
 *
 * offset~offset+length 범위의 모든 존에 대해 Zone Management Send(Reset) 명령 발행.
 * 리셋된 존은 Empty 상태로 돌아가 처음부터 다시 쓸 수 있게 됨.
 * 파일이 아직 열리지 않은 경우 임시로 open하고 완료 후 close.
 *
 * 호출 체인:
 *   zbd_reset_zone → [fio_nvme_reset_wp] → ioctl(NVME_IOCTL_IO_CMD, ZNS_MGMT_SEND)
 */
int fio_nvme_reset_wp(struct thread_data *td, struct fio_file *f,
		      uint64_t offset, uint64_t length)
{
	struct nvme_data *data = FILE_ENG_DATA(f);         /* [한국어] nsid/lba_shift. */
	unsigned int nr_zones;                             /* [한국어] 리셋할 존 수. */
	unsigned long long zslba;                          /* [한국어] 현재 존의 시작 LBA. */
	int i, fd, ret = 0;

	/* If the file is not yet opened, open it for this function. */
	fd = f->fd;                                        /* [한국어] 이미 엔진이 열어둔 fd가 있으면 그걸 사용(권한 O_RDWR 가정). */
	if (fd < 0) {
		fd = open(f->file_name, O_RDWR | O_LARGEFILE); /* [한국어] 없으면 이 함수 범위 내에서만 임시 open. */
		if (fd < 0)
			return -errno;
	}

	zslba = offset >> data->lba_shift;                 /* [한국어] byte offset → 시작 LBA. */
	nr_zones = (length + td->o.zone_size - 1) / td->o.zone_size; /* [한국어] 올림 나눗셈으로 존 개수 산출. */

	/* [한국어] 각 존마다 Reset Zone(ZSA=4) 명령을 순차 발행. 하나 실패해도 루프 계속(마지막 ret만 반환). */
	for (i = 0; i < nr_zones; i++, zslba += (td->o.zone_size >> data->lba_shift)) {
		struct nvme_passthru_cmd cmd = {
			.opcode         = nvme_zns_cmd_mgmt_send, /* [한국어] ZNS I/O opcode 0x79. */
			.nsid           = data->nsid,
			.cdw10          = zslba & 0xffffffff,     /* [한국어] SLBA 하위 32b. */
			.cdw11          = zslba >> 32,            /* [한국어] SLBA 상위 32b. */
			.cdw13          = NVME_ZNS_ZSA_RESET,     /* [한국어] ZSA(Zone Send Action) = Reset Zone. */
			.addr           = (__u64)(uintptr_t)NULL, /* [한국어] Reset은 데이터 전송 없음. */
			.data_len       = 0,
			.timeout_ms     = NVME_DEFAULT_IOCTL_TIMEOUT,
		};

		ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);   /* [한국어] 커널에 Zone Reset 제출. */
	}

	if (f->fd < 0)
		close(fd);                                  /* [한국어] 이 함수가 임시로 연 fd만 닫기. */
	return -ret;                                       /* [한국어] ioctl 양수 코드/음수 에러를 관례상 부호 반전해 반환. */
}

/*
 * [한국어] ZNS 최대 오픈 존 수 조회
 *
 * @param td: thread_data(미사용 — 시그니처 호환).
 * @param f: 대상 파일.
 * @param max_open_zones: 반환 포인터(mor+1).
 * @return: 0 성공, 음수 에러.
 *
 * caller: ZBD 프레임워크 — zbd_setup 단계에서 최대 오픈 존 제한값 획득.
 * callee: open(), close(), nvme_identify(ZNS CSI NS).
 *
 * ZNS Identify NS의 mor(Max Open Resources) + 1을 반환.
 * mor는 0-based이므로 +1하여 실제 최대 오픈 존 수를 계산.
 * fio의 ZBD 프레임워크에서 동시 오픈 존 수를 제한하는 데 사용.
 *
 * 호출 체인:
 *   zbd_init → [fio_nvme_get_max_open_zones] → nvme_identify
 */
int fio_nvme_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				unsigned int *max_open_zones)
{
	struct nvme_data *data = FILE_ENG_DATA(f);         /* [한국어] nsid 필요. */
	struct nvme_zns_id_ns zns_ns;                      /* [한국어] ZNS identify 응답. */
	int fd, ret = 0;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);   /* [한국어] 읽기 전용 임시 open. */
	if (fd < 0)
		return -errno;

	ret = nvme_identify(fd, data->nsid, NVME_IDENTIFY_CNS_CSI_NS,
				NVME_CSI_ZNS, &zns_ns);
	if (ret) {
		log_err("%s: nvme_zns_identify_ns failed, err=%d\n",
			f->file_name, ret);
		goto out;
	}

	*max_open_zones = zns_ns.mor + 1;                  /* [한국어] mor는 0-based → 실제 한계는 +1. */
out:
	close(fd);
	return ret;
}

/*
 * [한국어] FDP Reclaim Unit Handle 상태 조회
 *
 * @param fd: 디바이스 fd.
 * @param nsid: 네임스페이스 ID.
 * @param data_len: 응답 버퍼 바이트 수.
 * @param data: 응답 버퍼(nvme_fdp_ruh_status).
 * @return: ioctl 반환값.
 *
 * caller: fio_nvme_iomgmt_ruhs().
 * callee: ioctl(NVME_IOCTL_IO_CMD).
 *
 * I/O Management Receive(opcode=0x12) 명령으로 RUH 상태를 조회.
 * CDW10=1은 Reclaim Unit Handle Status를 의미.
 * FDP는 호스트가 데이터 배치를 제어하여 GC(Garbage Collection)로 인한
 * Write Amplification을 줄이는 NVMe 기능.
 */
static inline int nvme_fdp_reclaim_unit_handle_status(int fd, __u32 nsid,
						      __u32 data_len, void *data)
{
	/* [한국어] I/O Management Receive 커맨드 템플릿. MO(Management Operation)=1(RUHS). */
	struct nvme_passthru_cmd cmd = {
		.opcode		= nvme_cmd_io_mgmt_recv,   /* [한국어] NVMe I/O opcode 0x12. */
		.nsid		= nsid,
		.addr		= (__u64)(uintptr_t)data,  /* [한국어] RUHS 응답 DMA 버퍼. */
		.data_len 	= data_len,
		.cdw10		= 1,                       /* [한국어] [7:0] MO = 1 (RUH Status). */
		.cdw11		= (data_len >> 2) - 1,     /* [한국어] NUMD(DWORD - 1). */
	};

	return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);         /* [한국어] I/O 큐로 동기 제출. */
}

/*
 * [한국어] FDP RUH 상태 조회 래퍼 함수
 *
 * @param td: thread_data(미사용 — 시그니처 호환).
 * @param f: 대상 파일.
 * @param ruhs: 호출자가 제공한 RUH 상태 버퍼.
 * @param bytes: 버퍼 바이트 수.
 * @return: 0 성공, -ENOTSUP 미지원 또는 오류.
 *
 * caller: dataplacement.c — FDP RUH ID 목록 수집 시.
 * callee: open(), close(), nvme_fdp_reclaim_unit_handle_status().
 *
 * NVMe 캐릭터 디바이스를 열고 RUH 상태를 조회.
 * 실패 시 ENOTSUP으로 설정하여 FDP 미지원 디바이스를 처리.
 *
 * 호출 체인:
 *   fio_dp_init(또는 그 상위) → [fio_nvme_iomgmt_ruhs] → nvme_fdp_reclaim_unit_handle_status
 */
int fio_nvme_iomgmt_ruhs(struct thread_data *td, struct fio_file *f,
			 struct nvme_fdp_ruh_status *ruhs, __u32 bytes)
{
	struct nvme_data *data = FILE_ENG_DATA(f);         /* [한국어] nsid. */
	int fd, ret;

	fd = open(f->file_name, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -errno;

	ret = nvme_fdp_reclaim_unit_handle_status(fd, data->nsid, bytes, ruhs);
	if (ret) {
		/* [한국어] FDP 미지원 디바이스는 여기로 진입. 상위에 ENOTSUP으로 알려 fio가 FDP 관련 옵션을 무시하도록. */
		log_err("%s: nvme_fdp_reclaim_unit_handle_status failed, err=%d\n",
			f->file_name, ret);
		errno = ENOTSUP;
	} else
		errno = 0;                                 /* [한국어] 성공 케이스에서 이전 errno 잔재 제거. */

	ret = -errno;                                      /* [한국어] 최종 반환값은 -errno로 일원화(0 또는 -ENOTSUP). */
	close(fd);
	return ret;
}
