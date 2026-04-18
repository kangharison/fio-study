// SPDX-License-Identifier: GPL-2.0
/*
 * nvme structure declarations and helper functions for the
 * io_uring_cmd engine.
 */
/*
 * [한국어 설명] NVMe passthru 공용 헤더 (nvme.h)
 *
 * === 파일의 역할 ===
 * fio의 NVMe passthru 계열 엔진들(io_uring_cmd, xnvme의 io_uring_cmd 백엔드,
 * sg/nvme-passthrough 래퍼 등)이 공유하는 UAPI/도메인 데이터형 모음. NVMe
 * 스펙의 명령 패킷(struct nvme_uring_cmd, struct nvme_passthru_cmd64),
 * Identify 응답(Controller / Namespace / NVM CSI NS / ZNS CSI NS), Protection
 * Information(PI) 튜플(16B T10 DIF / 64B NVMe CRC64), Dataset Management(DSM)
 * 범위, ZNS Zone 디스크립터/리포트, FDP RUH 상태 디스크립터를 한국어로 풀이된
 * C 구조체로 노출한다. 또한 LBA↔offset 변환, 비정렬 48비트 big-endian 접근,
 * PI ref-tag escape 검사 같은 자주 쓰이는 인라인 유틸을 제공한다. 이 헤더는
 * "선언만 모아둔 라이브러리"이며, 실제 구현은 같은 디렉토리의 nvme.c 가
 * 갖는다(엔진들은 fio_nvme_* 접두 함수만 호출하면 된다).
 *
 * === 전체 아키텍처에서의 위치 ===
 * - 호출 방향: engines/io_uring.c (io_uring_cmd 엔진), engines/xnvme.c
 *   (백엔드=io_uring_cmd 일 때), engines/sg.c 의 NVMe 경유 경로가 본 헤더를
 *   include 한다. 이들은 prep/queue/getevents/event 콜백 안에서
 *   fio_nvme_uring_cmd_prep / fio_nvme_pi_fill / fio_nvme_pi_verify /
 *   fio_nvme_get_info / fio_nvme_get_zoned_model / fio_nvme_report_zones /
 *   fio_nvme_reset_wp / fio_nvme_get_max_open_zones / fio_nvme_iomgmt_ruhs 를
 *   호출해 명령 빌드와 디바이스 속성 질의를 위임한다.
 * - 실행 컨텍스트: 모두 호스트 유저스페이스(잡 스레드). Identify/ZNS 질의는
 *   리눅스 nvme-driver 의 ioctl(NVME_IOCTL_ADMIN_CMD / NVME_IOCTL_IO_CMD) 로
 *   동기 호출되고, 데이터 I/O 는 io_uring_cmd 의 IORING_OP_URING_CMD SQE 에
 *   nvme_uring_cmd 를 실어 내보낸다(NVME_URING_CMD_IO / _IO_VEC ioctl 매직).
 * - 매핑 ASCII (engines/io_uring.c::fio_ioring_cmd_prep 시점):
 *     io_u(offset, xfer_buflen, ddir, dtype, dspec, xfer_buf, mmap_data)
 *       │ get_slba/get_nlb (이 헤더 inline)
 *       ▼
 *     nvme_uring_cmd { opcode, nsid, cdw10/11=SLBA, cdw12=NLB|DTYPE|PRINFO,
 *                      cdw13=DSPEC, cdw14/15=REFTAG/APPTAG, addr, data_len,
 *                      metadata, metadata_len }
 *       │ 커널 nvme-driver(IORING_OP_URING_CMD)
 *       ▼
 *     NVMe SQE → 디바이스 컨트롤러 → CQE → io_uring CQ
 *
 * === 타 모듈과의 연결 ===
 * - linux/nvme_ioctl.h (UAPI): struct nvme_passthru_cmd / nvme_passthru_cmd64,
 *   NVME_IOCTL_ADMIN_CMD/IO_CMD/ID 매크로의 출처. 시스템 헤더가 이 정의를
 *   제공하지 못해도 본 헤더가 #ifndef CONFIG_NVME_URING_CMD 분기로 nvme_uring_cmd
 *   와 NVME_URING_CMD_IO[_VEC] 매직 넘버를 자체 정의해 빌드를 지킨다.
 * - ../fio.h: thread_data, struct io_u, struct fio_file, dprint, log_err,
 *   FILE_ENG_DATA 매크로 등 fio 코어 심볼 제공. 본 헤더는 fio 의존이라 다른
 *   엔진 소스에서 include 하기 전에 fio.h 가 필요한 종속을 끌어온다.
 * - ../crc/crc-t10dif.h, ../crc/crc64.h (nvme.c 가 include): 16B/64B 가드 PI 의
 *   체크섬 엔진. 본 헤더의 nvme_16b_guard_pif/nvme_64b_guard_pif 가 그 결과를
 *   저장하는 매개체.
 * - ../zbd_types.h, ../zbd.h (nvme.c 가 include): NVMe ZNS Zone Type/State 코드를
 *   fio 공용 zbd_zone(ZBD_ZONE_TYPE_SWR / ZBD_ZONE_COND_*)으로 매핑하는 사전.
 * - 데이터 흐름:
 *     fio_nvme_get_info(이 헤더의 선언) → nvme.c 가 ioctl(NVME_IOCTL_ID + Identify)
 *     호출로 struct nvme_data{nsid, lba_size, lba_shift, lba_ext, ms, pi_size,
 *     pi_type, guard_type, pi_loc} 를 채움 → fio_file 의 engine_data 슬롯에
 *     보존 → 이후 모든 prep 가 FILE_ENG_DATA(f) 매크로로 이 캐시를 읽어
 *     nvme_uring_cmd 의 SLBA/NLB/PRINFO/PRACT/REFTAG/APPTAG 를 계산.
 * - 공유 자료구조: 본 헤더가 정의하는 struct nvme_data 는 fio_file 단위
 *   per-namespace 캐시이므로 잡 스레드 1개만 다루고 init 단계에서 한 번
 *   채워진 뒤 read-only. struct nvme_pi_data 는 io_u 단위라 동시성 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct nvme_uring_cmd: io_uring_cmd 의 sqe.cmd 영역에 그대로 카피되는
 *   NVMe 명령 패킷(64B 정렬, CDW0~15 + addr/data_len/metadata 등). Linux
 *   nvme_ioctl.h 의 nvme_passthru_cmd64 와 1:1 의미 대응.
 * - struct nvme_data: fio_nvme_get_info 가 채우는 NS 포맷 캐시(LBA/MS/PI/lba_ext).
 * - struct nvme_pi_data: io_u 당 PI 컨텍스트(interval, io_flags, apptag/mask).
 * - struct nvme_id_ns / nvme_id_ctrl / nvme_nvm_id_ns / nvme_zns_id_ns: NVMe
 *   Identify 응답 4096B 레이아웃. fio 가 사용하는 필드는 일부에 한정되며 나머지는
 *   reserved/지정 안 함.
 * - struct nvme_16b_guard_pif / nvme_64b_guard_pif: PI 튜플(8B / 16B). 디바이스에
 *   기록되는 big-endian 표현 그대로.
 * - struct nvme_dsm_range / nvme_dsm: DSM(Deallocate=Trim) 명령의 가변 길이 범위 배열.
 * - struct nvme_zns_desc / nvme_zone_report: ZNS Zone Management Receive 응답.
 * - struct nvme_fdp_ruh_status[_desc]: FDP RUHS 응답 디스크립터.
 * - struct nvme_cmd_ext_io_opts: PI 발행 시 호스트가 지정하는 PRINFO/REFTAG/APPTAG.
 * - inline ilog2 / get_slba / get_nlb / put_unaligned_be48 / get_unaligned_be48 /
 *   fio_nvme_pi_ref_escape: 자주 쓰이는 비트/바이트 변환. 헤더에 인라인으로 두어
 *   호출 오버헤드 제거.
 * - fio_nvme_uring_cmd_prep / fio_nvme_pi_fill / fio_nvme_generate_guard /
 *   fio_nvme_pi_verify: 명령 빌드/검증의 핵심 4함수(외부 링크 — nvme.c 구현).
 * - fio_nvme_get_info / fio_nvme_get_zoned_model / fio_nvme_report_zones /
 *   fio_nvme_reset_wp / fio_nvme_get_max_open_zones / fio_nvme_iomgmt_ruhs:
 *   디바이스 속성 질의 6종(외부 링크).
 *
 * === NVMe 명령 CDW 레이아웃 핸드북 ===
 * Read/Write 공통(opcode 0x02/0x01, NVM CSI):
 *   CDW0   = opcode | flags<<8 | cid<<16
 *   CDW1   = NSID
 *   CDW10  = SLBA[31:0]
 *   CDW11  = SLBA[63:32]
 *   CDW12  = NLB[15:0] | DTYPE[23:20] | STC[24] | PRINFO[29:26](PRACT[29] |
 *            PRCHK_GUARD[28] | PRCHK_APP[27] | PRCHK_REF[26]) | FUA[30] | LR[31]
 *   CDW13  = DSPEC[31:16] | DSM[7:0](FDP/streams)
 *   CDW14  = ELBST/EILBRT[31:0] (Initial Logical Block Reference Tag, 64b 가드면 하위 32b)
 *   CDW15  = LBAT_M[31:16] | LBAT[15:0] (Logical Block App Tag Mask | App Tag)
 *   CDW3   = (64b 가드만) ELBST[47:32] (REFTAG 상위 16b 추가 분할)
 * Flush(opcode 0x00): NSID 만 채우고 데이터 없음.
 * DSM(opcode 0x09, Trim/Deallocate):
 *   CDW10 = NR[7:0] (Number of Ranges - 1, 0-based)
 *   CDW11 = AD[2] | IDW[1] | IDR[0] (= NVME_ATTRIBUTE_DEALLOCATE 등)
 *   addr  = nvme_dsm_range[] 사용자 버퍼
 * Zone Mgmt Receive(opcode 0x7A, ZNS):
 *   CDW10/11 = SLBA, CDW12 = NUMD-1(DWORD), CDW13 = ZRA[7:0]|ZRAS[15:8]|PR[16]
 * Zone Mgmt Send(opcode 0x79, ZNS):
 *   CDW10/11 = SLBA, CDW13 = ZSA[7:0] (Reset=4, Open=3, Close=2, Finish=1)
 * I/O Management Receive(opcode 0x12, FDP RUHS):
 *   CDW10 = MO[7:0] (RUHS=1), CDW11 = NUMD-1(DWORD)
 * Identify(Admin opcode 0x06):
 *   CDW10 = CNS[7:0] | CNTID[31:16], CDW11 = CSI[31:24] | NVMSETID[15:0]
 *
 * === Protection Information(PI) 동작 원리 ===
 * - 포맷 종류: 16B GUARD = T10 DIF CRC16 (8B 튜플 = guard 2B + apptag 2B + reftag 4B),
 *   64B GUARD = NVMe CRC64 Rocksoft (16B 튜플 = guard 8B + apptag 2B + reftag 6B).
 *   32B GUARD 도 스펙은 정의하지만 storage tag 가 필수라 fio 미지원.
 * - 위치: pi_loc=0(기본) → 메타 영역의 끝, pi_loc=1(NVME_NS_DPS_PI_FIRST) → 메타 시작.
 *   확장 LBA(flbas bit4=1)면 데이터+메타가 단일 lba_ext 블록으로 묶여 같은 버퍼에
 *   연속 배치, 별도 메타(flbas bit4=0)면 metadata DPTR 로 별도 버퍼 전송.
 * - PRACT 비트(CDW12[29]): 1이면 컨트롤러가 PI 자동 삽입(WRITE)/제거(READ), 0이면
 *   호스트가 직접 작성. PRACT=1 + ms==pi_size 조합은 메타 전체가 PI 라 호스트 입장에
 *   서는 ms=0 으로 취급(fio_nvme_get_info 에서 처리).
 * - PRCHK_{GUARD,APP,REF} 비트(CDW12[28:26]): 컨트롤러가 어떤 필드를 검증할지 지시.
 *   호스트가 PRACT=0 으로 PI 를 직접 쓸 때, 일치하는 필드만 채우면 됨.
 * - REFTAG 초기값(CDW14, 64b 가드면 + CDW3 상위 16b): Type1/2 는 SLBA 의 하위
 *   32/48b 를 그대로 사용해 블록마다 +1 되며 디바이스가 자동 검증. Type3 는 미사용.
 * - APPTAG/MASK(CDW15): 호스트 의도 식별자. mask 비트가 1인 위치만 비교.
 * - PI Escape: apptag=0xFFFF (Type1/2/3 공통) 또는 reftag 가 모두 1 (Type3) 이면 해당
 *   블록의 PI 검사를 디바이스가 스킵 — 비초기화 블록 표시 용도.
 *
 * === ZNS / FDP / Streams 요약 ===
 * - ZNS(Zoned Namespace, CSI=2): 네임스페이스가 다수의 sequential-write-required
 *   존(SWR)으로 분할. write 는 wp(write pointer) 위치에만 가능, reset 으로 wp 초기화.
 *   Zone Mgmt Receive(report)/Send(reset/open/close/finish) 명령으로 제어. fio 는
 *   ZBD 프레임워크(zbd.c)로 감싸 SWR 일관성을 보장한다.
 * - FDP(Flexible Data Placement): 호스트가 RUH(Reclaim Unit Handle) 를 통해 데이터
 *   배치 힌트를 디바이스에 전달 → 디바이스 GC 시 동일 RUH 데이터끼리 묶어
 *   WAF(Write Amplification) 감소. 명령은 일반 Write 의 CDW13 DSPEC 에 PID
 *   (Placement Identifier)를 실어 보낸다.
 * - Streams Directive: FDP 의 전신. 같은 CDW13 DSPEC 슬롯을 stream id 로 사용.
 */

#ifndef FIO_NVME_H
#define FIO_NVME_H

/* [한국어] linux/nvme_ioctl.h: 리눅스 커널 UAPI. NVME_IOCTL_ADMIN_CMD/IO_CMD/ID
 * 매크로와 struct nvme_passthru_cmd[64] 를 공급한다. nvme.c 가 ioctl()의
 * 두 번째 인자로 이 매크로를 사용한다. 시스템 헤더 버전이 충분히 새로우면
 * struct nvme_uring_cmd 와 NVME_URING_CMD_IO[_VEC] 도 함께 정의해 주지만,
 * 그렇지 않은 환경을 위해 아래 #ifndef CONFIG_NVME_URING_CMD 분기에서
 * 본 헤더가 자체 정의를 백업으로 둔다. */
#include <linux/nvme_ioctl.h>
/* [한국어] ../fio.h: fio 코어 — thread_data, struct io_u(필드 ddir/offset/
 * xfer_buf/xfer_buflen/mmap_data/file/dtype/dspec/engine_data 등),
 * struct fio_file(file_name/filetype/real_file_size/fd/engine_data),
 * FILE_ENG_DATA(f) 매크로, log_err/dprint 매크로, 기본 타입(__u8/__u16/__u32/
 * __u64, __le16/__le32/__le64, __be16/__be32/__be64), be16_to_cpu/cpu_to_be16,
 * memcmp 등의 표준 라이브러리 brought-in 제공. 본 헤더가 fio 도메인 위에
 * 서기 때문에 필수. */
#include "../fio.h"

/*
 * [한국어] CONFIG_NVME_URING_CMD: configure 가 자동 검출하는 빌드 피처. 시스템
 * 의 linux/nvme_ioctl.h 가 struct nvme_uring_cmd 와 NVME_URING_CMD_IO[_VEC]
 * 매크로를 이미 정의하면 이 매크로가 set 되어 아래 블록을 통째로 스킵한다.
 * 구버전 커널 헤더(예: 5.19 미만 일부 배포판)는 이 정의가 없으므로 본 헤더가
 * UAPI 와 동일한 레이아웃·매직 넘버를 직접 선언해 빌드를 보장한다. nvme.c 와
 * engines/io_uring.c 가 이 구조체를 io_uring 의 sqe.cmd 영역(80B)에 그대로
 * 카피하므로 필드 순서/크기는 커널 UAPI 와 비트 단위로 동일해야 한다.
 */
#ifndef CONFIG_NVME_URING_CMD
struct nvme_uring_cmd {
	__u8	opcode;
	/* [한국어] NVMe 명령 opcode 1바이트. enum nvme_io_opcode (Read=0x02,
	 * Write=0x01, Flush=0x00, Write Zeroes=0x08, DSM=0x09, ZNS Mgmt
	 * Send=0x79, Recv=0x7A, IO Mgmt Recv=0x12) 또는 enum
	 * nvme_admin_opcode (Identify=0x06) 가 들어간다.
	 * 설정자: nvme.c::fio_nvme_uring_cmd_prep, fio_nvme_uring_cmd_trim_prep,
	 * nvme_identify, nvme_report_zones 등 SQE 빌더 전부.
	 * 읽는 자: 커널 nvme-driver. fio 호스트 측은 다시 읽지 않음.
	 * 동기화: io_u 단위 1회 기록 → io_uring SQE 로 카피되므로 추가 락 불필요. */

	__u8	flags;
	/* [한국어] NVMe Submission Queue Entry 의 fuse/PSDT 비트(상위 4b PSDT,
	 * 하위 2b FUSE). 본 엔진은 단일 명령(FUSE=0)·PRP 모드(PSDT=0)만 사용해
	 * 항상 0 으로 둔다. 향후 SGL 모드 확장 시 PSDT bit6 를 1 로 설정.
	 * 설정자: SQE 빌더(현재는 memset 으로만 0 유지).
	 * 읽는 자: 커널 driver. */

	__u16	rsvd1;
	/* [한국어] NVMe 스펙상 SQE CDW0 의 CID(Command Identifier) 자리. 커널
	 * io_uring driver 가 자체 cid 관리를 하므로 호스트는 0 유지(reserved).
	 * 설정자: memset(0).
	 * 읽는 자: 커널 driver(0 으로 받아서 자체 채움). */

	__u32	nsid;
	/* [한국어] CDW1: Namespace Identifier. fio_nvme_get_info 가 NVME_IOCTL_ID
	 * 로 채집해 struct nvme_data.nsid 에 저장 → SQE 빌더가 매번 복사.
	 * Admin 명령(Identify Controller) 시에는 0 으로 두기도 한다(글로벌).
	 * 설정자: fio_nvme_uring_cmd_prep, _trim_prep, nvme_identify 등.
	 * 읽는 자: 커널 driver — 어느 namespace 의 LBA 를 다룰지 결정.
	 * 값 범위: 0(글로벌/admin) 또는 컨트롤러가 알려준 활성 NSID 1..N. */

	__u32	cdw2;
	/* [한국어] CDW2: 일부 명령에서만 사용. 본 엔진의 Read/Write/Flush/DSM/ZNS
	 * 경로는 미사용이라 0 유지. Identify 의 NVMSETID 같은 경우에 활용 가능.
	 * 설정자: 현재 미사용(memset 0).
	 * 읽는 자: 커널 driver — 명령에 따라 의미 다름. */

	__u32	cdw3;
	/* [한국어] CDW3: 64비트 가드 PI 의 ELBST 상위 16b(Initial Logical Block
	 * Reference Tag 의 [47:32]) 저장 자리. 16b 가드에서는 사용하지 않는다.
	 * 설정자: nvme.c::fio_nvme_pi_fill — 64b 가드 + Type1/2 + PRCHK_REF 인 경우
	 *         (slba >> 32) & 0xffff 를 저장.
	 * 읽는 자: 커널 → 디바이스가 PI 검증에 사용.
	 * 값 범위: [15:0] 만 유효. */

	__u64	metadata;
	/* [한국어] 별도 메타데이터 버퍼(MPTR)의 사용자 공간 주소. 확장 LBA 가 아닌
	 * 포맷에서만 의미가 있다(데이터+메타가 같은 lba_ext 블록에 함께 있으면
	 * 0). PI 가 메타 안에 위치할 수도, 메타 = PI 자체일 수도 있다.
	 * 설정자: fio_nvme_uring_cmd_prep — data->lba_shift && data->ms 일 때 io_u->mmap_data 주소 저장.
	 * 읽는 자: 커널 driver → DMA 매핑 후 디바이스에 MPTR 로 전달.
	 * 값 범위: 유효 사용자 주소 또는 0. */

	__u64	addr;
	/* [한국어] 데이터 버퍼 주소(단일 모드) 또는 struct iovec* 포인터(vectored 모드).
	 * vectored 모드에서는 data_len 이 iovec 개수 의미로 바뀐다.
	 * 설정자: fio_nvme_uring_cmd_prep — io_u->xfer_buf(단일) 또는 iov 포인터.
	 *         DSM 경로에서는 nvme_dsm_range 배열 주소.
	 *         write_zeroes 명령은 0 으로 둠(데이터 없음).
	 * 읽는 자: 커널 driver → DMA 매핑 후 PRP/SGL 로 디바이스에 전달.
	 * 값 범위: 유효 사용자 주소 또는 0. */

	__u32	metadata_len;
	/* [한국어] 메타데이터 버퍼 길이(바이트). 보통 (NLB+1) * data->ms.
	 * 설정자: fio_nvme_uring_cmd_prep.
	 * 읽는 자: 커널 driver → 디바이스 메타 전송 길이. */

	__u32	data_len;
	/* [한국어] 데이터 버퍼 길이(바이트, 단일 모드) 또는 iov 항목 수(vectored 모드).
	 * 설정자: fio_nvme_uring_cmd_prep — io_u->xfer_buflen 또는 1.
	 * 읽는 자: 커널 driver → DMA 길이 또는 iovec 카운트. */

	__u32	cdw10;
	/* [한국어] CDW10: Read/Write 의 SLBA[31:0], DSM 의 NR(Number of Ranges - 1),
	 * Identify 의 CNS, Zone Mgmt Recv 의 SLBA[31:0], IO Mgmt Recv 의 MO 등
	 * 명령마다 의미가 달라진다. 본 헤더 상단의 "CDW 레이아웃 핸드북" 참조.
	 * 설정자: SQE 빌더들.
	 * 읽는 자: 커널 → 디바이스. */

	__u32	cdw11;
	/* [한국어] CDW11: Read/Write 의 SLBA[63:32], DSM 의 Attributes(=NVME_ATTRIBUTE_DEALLOCATE),
	 * Identify 의 CSI<<24, Zone Mgmt Recv 의 SLBA[63:32], IO Mgmt Recv 의 NUMD-1.
	 * 설정자/독자: 위와 동일. */

	__u32	cdw12;
	/* [한국어] CDW12: Read/Write 의 NLB[15:0] | DTYPE[23:20] | PRINFO[29:26] |
	 * FUA[30] | LR[31], Zone Mgmt Recv 의 NUMD-1.
	 * 설정자: fio_nvme_uring_cmd_prep 가 NLB|DTYPE|cdw12_flags 합성, 이후
	 *         fio_nvme_pi_fill 이 opts->io_flags(PRINFO 비트들)를 OR 로 추가.
	 * 읽는 자: 커널 → 디바이스 PI/배치/캐시 동작 결정. */

	__u32	cdw13;
	/* [한국어] CDW13: Read/Write 의 DSPEC[31:16](FDP/Streams 배치 식별자) |
	 * DSM[15:0]. Zone Mgmt Send 의 ZSA(Zone Send Action). Zone Mgmt Recv 의
	 * ZRA(Report Zones=0) | ZRAS_FEAT(ERZ) | PR.
	 * 설정자: SQE 빌더 — io_u->dspec << 16 또는 NVME_ZNS_ZSA_RESET 등.
	 * 읽는 자: 커널 → 디바이스. */

	__u32	cdw14;
	/* [한국어] CDW14: Read/Write 의 EILBRT(Initial Logical Block Reference Tag)
	 * 32b. 64b 가드면 SLBA 의 하위 32b, 16b 가드면 SLBA 하위 32b 그대로.
	 * 설정자: fio_nvme_pi_fill — Type1/2 + PRCHK_REF 활성일 때.
	 * 읽는 자: 커널 → 디바이스 REFTAG 검증 시작값. */

	__u32	cdw15;
	/* [한국어] CDW15: Read/Write 의 LBAT_M[31:16] | LBAT[15:0] (Application
	 * Tag Mask | Application Tag).
	 * 설정자: fio_nvme_pi_fill — PRCHK_APP 활성일 때 opts->apptag_mask<<16|opts->apptag.
	 * 읽는 자: 커널 → 디바이스 APPTAG 비교 파라미터. */

	__u32	timeout_ms;
	/* [한국어] 명령 타임아웃(밀리초). 0 이면 커널 기본값(통상 60초). 본 엔진은
	 * NVME_DEFAULT_IOCTL_TIMEOUT(0)을 일관 사용해 디바이스/드라이버 타임아웃 위임.
	 * 설정자: SQE 빌더 — 통상 NVME_DEFAULT_IOCTL_TIMEOUT.
	 * 읽는 자: 커널 nvme-driver — admin/io 명령 타임아웃 설정. */

	__u32   rsvd2;
	/* [한국어] 예약 필드 — 향후 확장 대비. 항상 0.
	 * 설정자: memset(0).
	 * 읽는 자: 커널(0 검증). */
};

/* [한국어] NVME_URING_CMD_IO: io_uring_cmd 의 sqe.cmd_op 슬롯에 들어가는 매직
 * 넘버. _IOWR('N', 0x80, struct nvme_uring_cmd) 로 정의되어 디렉션 비트(R+W)
 * + 매직('N') + 번호(0x80) + size(struct nvme_uring_cmd) 를 32비트 코드로
 * 인코딩한다. 단일 데이터 버퍼(addr=user buffer, data_len=byte length) 모드.
 * 설정자: engines/io_uring.c::fio_ioring_cmd_prep — sqe->cmd_op 에 저장.
 * 읽는 자: 커널 io_uring + nvme driver — 어떤 ioctl 시맨틱을 쓸지 분기. */
#define NVME_URING_CMD_IO	_IOWR('N', 0x80, struct nvme_uring_cmd)
/* [한국어] NVME_URING_CMD_IO_VEC: vectored I/O 변종(_IOWR('N', 0x81, ...)).
 * addr 는 struct iovec*, data_len 은 iovec 카운트로 해석된다. fio 의 io_uring_cmd
 * 엔진은 vectored=1 옵션일 때 이 매직을 사용. */
#define NVME_URING_CMD_IO_VEC	_IOWR('N', 0x81, struct nvme_uring_cmd)
#endif /* CONFIG_NVME_URING_CMD */

/* [한국어] NVME_DEFAULT_IOCTL_TIMEOUT: 0 = 커널 기본 타임아웃 사용 위임. 본 엔진은
 * 모든 admin/io 명령에 동일 값을 사용해 디바이스 응답성에 의존한다. 무제한이
 * 아니라 nvme-driver 의 io_timeout/admin_timeout 모듈 파라미터(기본 30/60초)를 따른다. */
#define NVME_DEFAULT_IOCTL_TIMEOUT 0
/* [한국어] NVME_IDENTIFY_DATA_SIZE: NVMe 스펙이 고정한 Identify 응답 길이(4096B).
 * Identify Controller/Namespace/CSI NS 모두 동일. nvme.c::nvme_identify 가
 * data_len 으로 사용. */
#define NVME_IDENTIFY_DATA_SIZE 4096
/* [한국어] NVME_IDENTIFY_CSI_SHIFT: Identify CDW11 의 CSI 필드 [31:24] 의 시프트량.
 * nvme_identify 가 cdw11 = csi << 24 로 합성. */
#define NVME_IDENTIFY_CSI_SHIFT 24
/* [한국어] NVME_NQN_LENGTH: NVMe Qualified Name(NQN) 의 스펙 최대 길이(256B).
 * struct nvme_id_ctrl.subnqn 필드 크기로 사용. */
#define NVME_NQN_LENGTH	256

/* [한국어] PI Escape 매직 — 디바이스가 이 블록을 "비보호/비초기화"로 표시할 때
 * 사용하는 reserved 값. 호스트 측 fio_nvme_verify_pi_*_guard 가 이 값을 만나면
 * 해당 블록의 PI 검사를 스킵한다(스펙 허용). */
#define NVME_PI_APP_DISABLE 0xFFFF
/* [한국어] NVME_PI_REF_DISABLE: 16b 가드 reftag 의 escape 값(32b 모두 1). 64b 가드는
 * 48b 모두 1 인지 fio_nvme_pi_ref_escape() 헬퍼로 검사한다. */
#define NVME_PI_REF_DISABLE 0xFFFFFFFF

/* [한국어] NVME_ZNS_ZRA_REPORT_ZONES: Zone Receive Action = 0 (Report Zones).
 * Zone Mgmt Receive(opcode 0x7A) CDW13 의 [7:0] 자리. */
#define NVME_ZNS_ZRA_REPORT_ZONES 0
/* [한국어] NVME_ZNS_ZRAS_FEAT_ERZ: Zone Receive Action Specific Features 에서
 * Extended Report Zones(빈 존 포함) 비트(CDW13 [16]). nvme_report_zones 가
 * 이 비트를 OR 해 비어있는 존도 누락 없이 한 번에 받기 위해 사용. */
#define NVME_ZNS_ZRAS_FEAT_ERZ (1 << 16)
/* [한국어] NVME_ZNS_ZSA_RESET: Zone Send Action = 4 (Reset Zone). Zone Mgmt
 * Send(opcode 0x79) CDW13 의 [7:0] 자리. fio_nvme_reset_wp 가 사용. */
#define NVME_ZNS_ZSA_RESET 0x4
/* [한국어] NVME_ZONE_TYPE_SEQWRITE_REQ: ZNS 존 타입 = 0x2 (Sequential Write
 * Required). 현재 NVMe ZNS 스펙은 사실상 이 한 가지 타입만 정의. zns_desc.zt
 * [3:0] 비교에 사용되어 fio 의 ZBD_ZONE_TYPE_SWR 로 매핑된다. */
#define NVME_ZONE_TYPE_SEQWRITE_REQ 0x2

/* [한국어] NVME_ATTRIBUTE_DEALLOCATE: DSM(Dataset Management) CDW11 의 AD(bit 2)
 * 비트. fio 의 TRIM(ddir=DDIR_TRIM) 은 NVMe 스펙상 Deallocate=DSM 으로 매핑되며,
 * 이 비트가 설정되어야 디바이스가 LBA 범위를 물리적으로 무효화한다. IDR(bit 0)
 * = Integral Dataset for Read, IDW(bit 1) = Integral Dataset for Write 는 본
 * 엔진 미사용. */
#define NVME_ATTRIBUTE_DEALLOCATE (1 << 2)

/*
 * [한국어] enum nvme_identify_cns: Identify 명령(Admin opcode 0x06) 의 CNS
 * (Controller or Namespace Structure) 필드 값. CDW10 [7:0] 에 들어가 디바이스
 * 가 어떤 구조체를 응답으로 돌려줄지 결정한다. nvme.c::nvme_identify 가 받는
 * 인자이며, fio_nvme_get_info / fio_nvme_get_zoned_model /
 * fio_nvme_report_zones 등이 호출 시 지정한다.
 */
enum nvme_identify_cns {
	NVME_IDENTIFY_CNS_NS		= 0x00,
	/* [한국어] CNS=0x00: Identify Namespace. 응답 = struct nvme_id_ns(4096B).
	 * 사용자: fio_nvme_get_info(NVMe NS 포맷·PI 정보 추출),
	 *        fio_nvme_report_zones(flbas 포맷 인덱스 결정용). */
	NVME_IDENTIFY_CNS_CTRL		= 0x01,
	/* [한국어] CNS=0x01: Identify Controller. 응답 = struct nvme_id_ctrl(4096B).
	 * 사용자: fio_nvme_get_info(ctratt 의 ELBAS 비트 확인). */
	NVME_IDENTIFY_CNS_CSI_NS	= 0x05,
	/* [한국어] CNS=0x05: I/O Command Set specific Identify Namespace. 함께
	 * 지정한 CSI(NVM/ZNS) 에 따라 응답 구조체가 달라진다.
	 *  - CSI=NVM → struct nvme_nvm_id_ns(elbaf[] 으로 가드 타입 조사).
	 *  - CSI=ZNS → struct nvme_zns_id_ns(zsze, mor 등). */
	NVME_IDENTIFY_CNS_CSI_CTRL	= 0x06,
	/* [한국어] CNS=0x06: I/O Command Set specific Identify Controller.
	 * fio_nvme_get_zoned_model 이 CSI=ZNS 와 함께 사용해 ZNS 지원 여부 탐지. */
};

/*
 * [한국어] enum nvme_csi: NVMe I/O Command Set Identifier. Identify CDW11
 * [31:24] 슬롯에 저장. nvme.c 가 NVME_IDENTIFY_CSI_SHIFT(=24) 만큼 시프트해
 * 합성한다. NVMe 2.x 스펙이 정의한 다중 커맨드세트 분기 키.
 */
enum nvme_csi {
	NVME_CSI_NVM			= 0,
	/* [한국어] CSI=0: 기본 NVM(블록) 커맨드 세트. Read/Write/Flush/DSM 등.
	 * 사용자: fio_nvme_get_info — NVM 포맷 정보 취득. */
	NVME_CSI_KV			= 1,
	/* [한국어] CSI=1: Key-Value 커맨드 세트(NVMe 2.0). 본 fio 엔진 미지원.
	 * 정의만 두어 추후 확장에 대비. */
	NVME_CSI_ZNS			= 2,
	/* [한국어] CSI=2: Zoned Namespace 커맨드 세트. Zone Mgmt Send/Recv,
	 * Zone Append 등을 정의.
	 * 사용자: fio_nvme_get_zoned_model, fio_nvme_report_zones,
	 *        fio_nvme_get_max_open_zones — ZNS 디바이스 속성 질의. */
};

/*
 * [한국어] enum nvme_admin_opcode: Admin Submission Queue 에 제출되는 명령
 * opcode. 본 헤더는 fio 가 사용하는 Identify 만 정의. Admin 명령은
 * NVME_IOCTL_ADMIN_CMD ioctl 경로(struct nvme_passthru_cmd) 로 보낸다.
 */
enum nvme_admin_opcode {
	nvme_admin_identify		= 0x06,
	/* [한국어] Identify(0x06): 컨트롤러/네임스페이스 메타데이터 조회.
	 * 사용자: nvme.c::nvme_identify(static) — fio_nvme_get_info,
	 * fio_nvme_get_zoned_model, fio_nvme_report_zones, fio_nvme_get_max_open_zones. */
};

/*
 * [한국어] enum nvme_io_opcode: I/O Submission Queue 에 제출되는 명령 opcode.
 * NVME_IOCTL_IO_CMD ioctl 또는 io_uring_cmd 의 IORING_OP_URING_CMD 로 디바이스
 * 에 전달된다. nvme_uring_cmd.opcode 에 1바이트로 저장된다.
 */
enum nvme_io_opcode {
	nvme_cmd_flush			= 0x00,
	/* [한국어] Flush(0x00): 휘발성 쓰기 캐시(VWC) 의 데이터를 매체에 플러시.
	 * 사용자: fio_nvme_uring_cmd_prep — DDIR_SYNC/DATASYNC 를 매핑. */
	nvme_cmd_write			= 0x01,
	/* [한국어] Write(0x01): LBA 데이터 쓰기. SLBA/NLB/PI 필드 사용.
	 * 사용자: fio_nvme_uring_cmd_prep — DDIR_WRITE 의 기본 opcode. */
	nvme_cmd_read			= 0x02,
	/* [한국어] Read(0x02): LBA 데이터 읽기.
	 * 사용자: fio_nvme_uring_cmd_prep — DDIR_READ 의 기본 opcode. */
	nvme_cmd_write_uncor		= 0x04,
	/* [한국어] Write Uncorrectable(0x04): 지정 LBA 를 의도적으로 unrecoverable
	 * 표시(에러 주입). 본 엔진은 직접 사용하지 않으나 사용자 정의 opcode 로
	 * 외부 엔진이 호출 가능. */
	nvme_cmd_compare		= 0x05,
	/* [한국어] Compare(0x05): 호스트 버퍼와 매체 데이터 비교(verify-by-compare).
	 * 본 엔진 미사용. */
	nvme_cmd_write_zeroes		= 0x08,
	/* [한국어] Write Zeroes(0x08): 데이터 버퍼 없이 지정 LBA 범위를 0 으로 초기화.
	 * 사용자: io_uring_cmd 엔진의 write_opcode 옵션으로 0x08 지정 시 활성. */
	nvme_cmd_dsm			= 0x09,
	/* [한국어] Dataset Management(0x09): Deallocate(=Trim) / IDR / IDW 힌트.
	 * 사용자: fio_nvme_uring_cmd_trim_prep — DDIR_TRIM 매핑.
	 * 페이로드: nvme_dsm_range[] 배열. */
	nvme_cmd_verify			= 0x0c,
	/* [한국어] Verify(0x0c): 디바이스 단독으로 LBA 범위의 PI/CRC 재검증.
	 * 본 엔진 미사용. */
	nvme_cmd_io_mgmt_recv		= 0x12,
	/* [한국어] I/O Management Receive(0x12): FDP 의 RUH 상태/통계 조회 등.
	 * 사용자: nvme.c::nvme_fdp_reclaim_unit_handle_status — RUHS(MO=1) 조회. */
	nvme_zns_cmd_mgmt_send		= 0x79,
	/* [한국어] ZNS Zone Management Send(0x79): Open/Close/Finish/Reset/Offline
	 * Zone 등 ZSA(Zone Send Action) 분기. CDW13 [7:0] 에 ZSA 값 저장.
	 * 사용자: fio_nvme_reset_wp — ZSA=NVME_ZNS_ZSA_RESET. */
	nvme_zns_cmd_mgmt_recv		= 0x7a,
	/* [한국어] ZNS Zone Management Receive(0x7A): Report Zones 등.
	 * 사용자: nvme.c::nvme_report_zones — ZRA=Report. */
};

/*
 * [한국어] enum nvme_zns_zs: ZNS 존 상태(Zone State). nvme_zns_desc.zs 의 [7:4]
 * 자리에 인코딩되어 디바이스가 보고. ZNS 라이프사이클은 Empty → Impl/Expl Open →
 * Closed → Full 순환이며 Reset 으로 Empty 로 복귀한다.
 */
enum nvme_zns_zs {
	NVME_ZNS_ZS_EMPTY		= 0x1,
	/* [한국어] Empty: 데이터 없음. wp = zslba. 새 쓰기 가능.
	 * fio 매핑: ZBD_ZONE_COND_EMPTY. */
	NVME_ZNS_ZS_IMPL_OPEN		= 0x2,
	/* [한국어] Implicitly Opened: 첫 Write 가 자동 오픈한 상태. mor 한도 안에서만 가능.
	 * fio 매핑: ZBD_ZONE_COND_IMP_OPEN. */
	NVME_ZNS_ZS_EXPL_OPEN		= 0x3,
	/* [한국어] Explicitly Opened: 호스트가 Open Zone 명령(ZSA=3) 으로 명시 오픈.
	 * fio 매핑: ZBD_ZONE_COND_EXP_OPEN. */
	NVME_ZNS_ZS_CLOSED		= 0x4,
	/* [한국어] Closed: Open 슬롯 회수. wp 유지. 다시 쓰려면 Implicit/Explicit Open.
	 * fio 매핑: ZBD_ZONE_COND_CLOSED. */
	NVME_ZNS_ZS_READ_ONLY		= 0xd,
	/* [한국어] Read Only: 매체 손상으로 읽기 전용. fio 는 OFFLINE 으로 취급. */
	NVME_ZNS_ZS_FULL		= 0xe,
	/* [한국어] Full: zcap 까지 다 채워짐. Reset 전엔 새 쓰기 불가.
	 * fio 매핑: ZBD_ZONE_COND_FULL. */
	NVME_ZNS_ZS_OFFLINE		= 0xf,
	/* [한국어] Offline: 디바이스가 사용 불가 표시. fio 매핑: ZBD_ZONE_COND_OFFLINE. */
};

/*
 * [한국어] enum nvme_id_ctrl_ctratt: Identify Controller 의 ctratt(Controller
 * Attributes) 32비트 비트필드. fio 가 관심 갖는 비트만 정의.
 */
enum nvme_id_ctrl_ctratt {
	NVME_CTRL_CTRATT_ELBAS		= 1 << 15,
	/* [한국어] ELBAS(Extended LBA Format Support, ctratt[15]): 1 이면 NVM CSI
	 * Identify NS(elbaf[]) 로 가드 타입(16b/32b/64b) 과 storage tag 정보를
	 * 조회 가능. 0 이면 16b T10 DIF 가드만 지원하는 구형 컨트롤러로 간주.
	 * 사용자: fio_nvme_get_info — 분기 결정. */
};

/*
 * [한국어] elbaf 비트 풀이 상수. nvme_nvm_id_ns.elbaf[format_idx] 32비트
 * 비트필드를 [6:0] STS(Storage Tag Size) + [8:7] Guard Type + [16:9] reserved 등
 * 으로 분할. fio_nvme_get_info 에서 사용.
 */
enum {
	NVME_ID_NS_NVM_STS_MASK		= 0x7f,
	/* [한국어] STS 비트 [6:0]. 0 이 아니면 storage tag 가 LBA 별 메타에 추가
	 * 되는데 fio 는 미지원이라 이 값이 0 이어야 한다(아니면 ENOTSUP). */
	NVME_ID_NS_NVM_GUARD_SHIFT	= 7,
	/* [한국어] Guard Type 의 시작 비트 위치(bit 7). elbaf >> 7 후 마스크. */
	NVME_ID_NS_NVM_GUARD_MASK	= 0x3,
	/* [한국어] Guard Type 의 비트 너비(2b). 결과는 enum {16B/32B/64B}_GUARD. */
};

/*
 * [한국어] 가드 타입 코드. nvme_data.guard_type 과 elbaf 의 [8:7] 추출값에
 * 사용된다. PI 튜플 크기와 CRC 알고리즘을 결정.
 */
enum {
	NVME_NVM_NS_16B_GUARD		= 0,
	/* [한국어] 16비트 가드(T10 DIF CRC16). PI 튜플 크기 = 8B(struct
	 * nvme_16b_guard_pif). 레거시 ELBAS 미지원 컨트롤러도 이 모드로 간주. */
	NVME_NVM_NS_32B_GUARD		= 1,
	/* [한국어] 32비트 가드. storage tag 가 필수라 fio 미지원. 정의만 둠. */
	NVME_NVM_NS_64B_GUARD		= 2,
	/* [한국어] 64비트 가드(NVMe CRC64 Rocksoft). PI 튜플 크기 = 16B(struct
	 * nvme_64b_guard_pif). REFTAG 가 48b 로 확장되어 cdw3 + cdw14 두 워드에
	 * 분할 저장. */
};

/*
 * [한국어] struct nvme_data: fio 내부 NVMe 네임스페이스 포맷 캐시. 엔진 init/
 * open_file 시점에 fio_nvme_get_info() 가 한 번 채우고, 이후 모든 prep 콜백이
 * FILE_ENG_DATA(f) 매크로로 읽기만 한다. fio_file 의 engine_data 슬롯에 저장.
 * 단일 잡 스레드만 다루며 init 이후 read-only 라 락 불필요.
 */
struct nvme_data {
	__u32 nsid;
	/* [한국어] Namespace Identifier. NVME_IOCTL_ID 결과를 그대로 보관.
	 * 설정자: fio_nvme_get_info — ioctl(fd, NVME_IOCTL_ID).
	 * 읽는 자: nvme_uring_cmd 빌더 전부(.nsid 필드로 복사).
	 * 값 범위: 1..N(컨트롤러가 알려준 활성 NSID). 0 은 사용하지 않음. */

	__u32 lba_shift;
	/* [한국어] LBA 크기의 log2 값(예: 512B → 9, 4KB → 12). 비확장 LBA(별도 메타)
	 * 포맷에서만 의미 있고, 확장 LBA(lba_ext>0) 면 0 으로 남는다. get_slba/get_nlb
	 * 가 lba_ext==0 분기일 때 시프트 연산자에 사용.
	 * 설정자: fio_nvme_get_info — !lba_ext 일 때 ilog2(lba_size).
	 * 읽는 자: get_slba, get_nlb, fio_nvme_report_zones, fio_nvme_reset_wp.
	 * 값 범위: 0(확장 LBA) 또는 9..16. */

	__u32 lba_size;
	/* [한국어] 한 LBA 의 데이터 크기(바이트). lbaf[format_idx].ds 의 1<<ds.
	 * 설정자: fio_nvme_get_info.
	 * 읽는 자: PI 가드 생성/검증 — fio_crc_*(buf, lba_size, ...) 에 길이로 사용.
	 * 값 범위: 512, 4096 등 2의 거듭제곱. */

	__u32 lba_ext;
	/* [한국어] 확장 LBA(데이터+메타가 하나로 묶인) 1블록의 바이트 크기. flbas
	 * bit4=1 이면 lba_size + ms, 아니면 0. get_slba/get_nlb 가 0 인지 확인해
	 * 분기한다.
	 * 설정자: fio_nvme_get_info — flbas bit4=1 일 때 lba_size+ms.
	 * 읽는 자: get_slba, get_nlb, PI 가드 코드의 buf 전진 폭 결정.
	 * 값 범위: 0 또는 lba_size+ms. */

	__u16 ms;
	/* [한국어] 블록 당 메타데이터 크기(바이트). 0 이면 PI/메타 없음. PRACT=1 +
	 * ms==pi_size 조합은 컨트롤러가 PI 만 다루므로 호스트는 0 으로 취급.
	 * 설정자: fio_nvme_get_info — lbaf[format_idx].ms.
	 * 읽는 자: PI 가드 코드, fio_nvme_uring_cmd_prep 의 metadata_len 계산.
	 * 값 범위: 0, 8, 16, 64, 128 등. */

	__u16 pi_size;
	/* [한국어] PI 튜플 크기(바이트). 16b 가드 = 8, 64b 가드 = 16. PRACT 모드
	 * 판정에 ms 와 비교한다.
	 * 설정자: fio_nvme_get_info — guard_type 에 따른 sizeof().
	 * 읽는 자: fio_nvme_get_info(PRACT 비교), interval 계산.
	 * 값 범위: 8 또는 16. */

	__u8 pi_type;
	/* [한국어] PI 보호 유형. ns.dps 의 [2:0]. 0=없음, 1/2/3 = Type1/2/3.
	 * Type1: REFTAG 필수+자동(LBA), Type2: REFTAG 호스트 지정 가능, Type3:
	 * REFTAG 미사용(GUARD+APPTAG 만).
	 * 설정자: fio_nvme_get_info — ns.dps & NVME_NS_DPS_PI_MASK.
	 * 읽는 자: PI 가드 생성/검증 분기, fio_nvme_pi_fill 분기. */

	__u8 guard_type;
	/* [한국어] 가드 알고리즘. 0=16b T10 DIF CRC16, 2=64b NVMe CRC64. ELBAS 비
	 * 지원 컨트롤러는 무조건 0.
	 * 설정자: fio_nvme_get_info — elbaf 추출 또는 레거시 0.
	 * 읽는 자: fio_nvme_generate_guard / fio_nvme_pi_verify 디스패치. */

	__u8 pi_loc;
	/* [한국어] PI 튜플의 메타 영역 내 위치. 0 = 메타 끝(기본), 1 = 메타 시작
	 * (NVME_NS_DPS_PI_FIRST). 가드 생성/검증 코드의 interval 계산 분기 키.
	 * 설정자: fio_nvme_get_info — ns.dps & NVME_NS_DPS_PI_FIRST.
	 * 읽는 자: PI 가드 코드. */
};

/*
 * [한국어] enum nvme_id_ns_dps: Identify Namespace 응답의 dps(Data Protection
 * Settings) 8비트 필드 풀이. nvme.c::fio_nvme_get_info 가 마스크/시프트로 분리해
 * struct nvme_data.pi_type / pi_loc 에 저장한다.
 */
enum nvme_id_ns_dps {
	NVME_NS_DPS_PI_NONE		= 0,
	/* [한국어] dps[2:0]=0: PI 미설정. 메타 데이터가 있어도 PI 로 해석하지 않음. */
	NVME_NS_DPS_PI_TYPE1		= 1,
	/* [한국어] PI Type1: 모든 블록의 REFTAG = 시작 LBA 의 하위 32/48b. 디바이스가
	 * 자동으로 +1 증가시키며 자동 검증. 디스크 셀프 손상 감지에 가장 강력. */
	NVME_NS_DPS_PI_TYPE2		= 2,
	/* [한국어] PI Type2: REFTAG 초기값을 호스트가 명시 지정 가능(EILBRT). 본 fio
	 * 는 SLBA 를 그대로 초기값으로 쓰므로 사용자 입장 동작은 Type1 과 유사. */
	NVME_NS_DPS_PI_TYPE3		= 3,
	/* [한국어] PI Type3: REFTAG 검증 없음(GUARD + APPTAG 만). escape 매직(reftag
	 * 모두 1 + apptag=0xFFFF) 으로 블록 단위 비보호 표시 가능. */
	NVME_NS_DPS_PI_MASK		= 7 << 0,
	/* [한국어] [2:0] PI Type 추출 마스크. */
	NVME_NS_DPS_PI_FIRST		= 1 << 3,
	/* [한국어] [3] PI 위치. 1 = PI 가 메타 영역의 처음, 0 = 끝. struct
	 * nvme_data.pi_loc 의 원본. */
};

/*
 * [한국어] enum nvme_io_control_flags: Read/Write CDW12 의 PRINFO 비트필드 풀이.
 * fio_nvme_pi_fill 이 cmd->cdw12 에 OR 하여 디바이스에게 PI 검사 정책 전달.
 * 본 헤더의 "PI 동작 원리" 섹션에 의미 요약.
 */
enum nvme_io_control_flags {
	NVME_IO_PRINFO_PRCHK_REF	= 1U << 26,
	/* [한국어] CDW12[26] PRCHK_REF: REFTAG 검사 수행 요청. Type1/2 와만 의미.
	 * 사용자: fio_nvme_pi_fill — opts->io_flags & 이 비트면 cdw14(+cdw3) 에
	 * 초기 REFTAG 기록. */
	NVME_IO_PRINFO_PRCHK_APP	= 1U << 27,
	/* [한국어] CDW12[27] PRCHK_APP: APPTAG 비교 요청. cdw15 에 (mask<<16|tag). */
	NVME_IO_PRINFO_PRCHK_GUARD	= 1U << 28,
	/* [한국어] CDW12[28] PRCHK_GUARD: CRC GUARD 검증 요청. */
	NVME_IO_PRINFO_PRACT		= 1U << 29,
	/* [한국어] CDW12[29] PRACT: 1 = 컨트롤러가 PI 자동 삽입(WRITE)/제거(READ).
	 * 호스트는 PI 튜플을 직접 작성/검증하지 않는다. fio_nvme_get_info 가 ms ==
	 * pi_size 인 경우 ms=0 으로 강제해 호스트 측이 메타를 다루지 않게 한다. */
};

/*
 * [한국어] struct nvme_pi_data: io_u 단위 PI 컨텍스트. io_u->engine_data 에
 * 보관되어 generate(prep) 가 기록한 interval/io_flags/apptag(_mask) 를
 * verify(event) 에서 그대로 재사용한다. 단일 io_u → 단일 잡 스레드라 락 없음.
 */
struct nvme_pi_data {
	__u32 interval;
	/* [한국어] 블록 내 PI 튜플의 시작 오프셋(바이트). pi_loc/lba_ext 조합으로
	 * generate 시점에 한 번 계산되며 verify 가 동일 값을 사용해 위치 일치 보장.
	 * 설정자: fio_nvme_generate_pi_{16b,64b}_guard.
	 * 읽는 자: fio_nvme_verify_pi_{16b,64b}_guard. */

	__u32 io_flags;
	/* [한국어] 이 io_u 발행 시 사용한 PRINFO 비트(PRCHK_GUARD/APP/REF, PRACT).
	 * verify 가 동일한 비트만 비교해 일관성 유지.
	 * 설정자: io_uring_cmd 엔진 prep 가 옵션 파싱 결과를 채움.
	 * 읽는 자: fio_nvme_verify_pi_*. */

	__u16 apptag;
	/* [한국어] 기대 APPTAG 값(호스트 지정). PRCHK_APP 활성 시 디바이스 비교값.
	 * 설정자: 엔진 옵션 → 엔진 prep.
	 * 읽는 자: fio_nvme_pi_fill, fio_nvme_verify_pi_*. */

	__u16 apptag_mask;
	/* [한국어] APPTAG 비교 마스크. 비트가 1 인 위치만 비교(0 이면 don't-care).
	 * 설정자: 엔진 옵션 → 엔진 prep.
	 * 읽는 자: fio_nvme_pi_fill(cdw15 합성), fio_nvme_verify_pi_*. */
};

/*
 * [한국어] struct nvme_lbaf: Identify Namespace 응답의 lbaf[64] 배열 항목 (4B).
 * 네임스페이스가 지원하는 LBA 포맷(데이터/메타 크기 + 성능 등급) 을 기술.
 * 활성 포맷은 ns.flbas 의 인덱스로 선택된다.
 */
struct nvme_lbaf {
	__le16			ms;
	/* [한국어] 메타데이터 크기(바이트). PI 만 사용 시 8/16, 추가 메타 포함 시
	 * 64/128 등. fio_nvme_get_info 가 le16_to_cpu 변환 후 nvme_data.ms 에 저장. */

	__u8			ds;
	/* [한국어] LBA 데이터 크기의 log2(예: 9=512B, 12=4KB). nvme_data.lba_size
	 * = 1 << ds. */

	__u8			rp;
	/* [한국어] Relative Performance(0=Best, 3=Worst). 디바이스가 권장 포맷을
	 * 알리는 힌트. fio 는 직접 사용하지 않는다. */
};

/*
 * [한국어] struct nvme_16b_guard_pif: 16바이트 가드(T10 DIF CRC16) 의 PI 튜플
 * 레이아웃(8B). 매체에 big-endian 으로 저장되며 호스트는 cpu_to_be16/be32 로
 * 변환해 기록·검증. fio_nvme_generate_pi_16b_guard / fio_nvme_verify_pi_16b_guard.
 */
struct nvme_16b_guard_pif {
	__be16 guard;
	/* [한국어] T10 DIF CRC16 결과. 데이터(+옵션 메타 prefix) 의 체크섬.
	 * 설정자: generate — cpu_to_be16(fio_crc_t10dif(...)).
	 * 읽는 자: verify — be16_to_cpu 후 재계산값과 비교. */

	__be16 apptag;
	/* [한국어] Application Tag(16b). 사용자 의도 식별자.
	 * 설정자: generate — cpu_to_be16(opts->apptag).
	 * 읽는 자: verify — apptag_mask 적용 후 비교. */

	__be32 srtag;
	/* [한국어] Storage/Reference Tag(32b). Type1/2 는 (SLBA + blockIndex) & 0xFFFFFFFF.
	 * Type3 는 의미 없음(읽기 escape 매직 NVME_PI_REF_DISABLE 와 비교에만 사용).
	 * 설정자: generate(Type1/2 만).
	 * 읽는 자: verify, fio_nvme_pi_ref_escape 류 분기. */
};

/*
 * [한국어] struct nvme_64b_guard_pif: 64바이트 가드(NVMe CRC64 Rocksoft) 의 PI
 * 튜플 레이아웃(16B). 매체 big-endian. fio_nvme_generate_pi_64b_guard /
 * fio_nvme_verify_pi_64b_guard 가 다룬다.
 */
struct nvme_64b_guard_pif {
	__be64 guard;
	/* [한국어] NVMe CRC64 Rocksoft 다항식 결과(64b). 16b 가드보다 충돌 확률 낮음.
	 * 설정자: generate — cpu_to_be64(fio_crc64_nvme(...)).
	 * 읽는 자: verify — be64_to_cpu 후 비교. */

	__be16 apptag;
	/* [한국어] Application Tag(16b). 16b 가드와 동일. */

	__u8 srtag[6];
	/* [한국어] 48비트 Reference Tag. 비정렬 6바이트라 put_unaligned_be48 /
	 * get_unaligned_be48 헬퍼로 접근. cdw3 + cdw14 두 워드(상위 16b + 하위 32b)
	 * 가 디바이스 측 초기값. */
};

/*
 * [한국어] struct nvme_id_ns: NVMe Identify Namespace(CNS=0x00) 응답 4096B 레이아웃.
 * nvme_admin_identify(CNS=0x00, CSI=NVM) 가 채운다. fio 가 직접 읽는 필드는
 * nsze, nlbaf, flbas, dps, lbaf[] 정도이며 나머지는 NVMe 스펙(NVM Express Base
 * Spec, Identify Namespace Data Structure 표) 의 reserved/벤더/관리 정보로,
 * 본 헤더는 메모리 레이아웃 일치만을 위해 동일 순서·크기로 선언해 둔다.
 * 설정자: nvme.c::nvme_identify(CNS_NS) → ioctl 응답 DMA.
 * 읽는 자: fio_nvme_get_info, fio_nvme_report_zones(flbas 만).
 */
struct nvme_id_ns {
	__le64			nsze;
	/* [한국어] Namespace Size(LBA 단위). 파일 크기로 환산되는 핵심 값.
	 * 설정자: 디바이스 → ioctl 응답.
	 * 읽는 자: fio_nvme_get_info — *nlba 출력으로 호출자에게 전달, 호출자가
	 *         f->real_file_size = nsze * lba_size 로 계산. */

	__le64			ncap;
	/* [한국어] Namespace Capacity(LBA). 실제 할당 가능 LBA 수(thin-provisioning).
	 * 본 fio 미사용. */

	__le64			nuse;
	/* [한국어] Namespace Utilization(LBA). 현재 사용 중인 LBA 수. fio 미사용. */

	__u8			nsfeat;
	/* [한국어] Namespace Features 비트필드(thin-prov, atomic write, 등). 미사용. */

	__u8			nlbaf;
	/* [한국어] Number of LBA Formats(0-based). NVMe 1.4 부터 16 이상도 가능해
	 * fio_nvme_get_info 가 nlbaf<16 분기로 flbas 의 MSB 2b 추가 여부 결정. */

	__u8			flbas;
	/* [한국어] Formatted LBA Size 비트필드. [3:0]=포맷 인덱스 LSB, [4]=Extended
	 * LBA(메타+데이터 결합) 사용 여부, [6:5]=인덱스 MSB(NVMe 1.4 +).
	 * 설정자: 디바이스(포맷 결과).
	 * 읽는 자: fio_nvme_get_info(인덱스 합성 + 확장 LBA 판정),
	 *         fio_nvme_report_zones(flbas & 0xf 로 ZNS lbafe 인덱스 결정). */

	__u8			mc;
	/* [한국어] Metadata Capabilities. 별도/확장 메타 지원 여부. 미사용. */

	__u8			dpc;
	/* [한국어] Data Protection Capabilities. 디바이스가 지원하는 PI Type 들.
	 * fio 는 사용 중인 dps 만 보고 dpc 는 무시. */

	__u8			dps;
	/* [한국어] Data Protection Settings 8b 필드. [2:0]=PI Type, [3]=PI 위치
	 * (NVME_NS_DPS_PI_FIRST). nvme_data.pi_type/pi_loc 의 원본.
	 * 설정자: 디바이스(포맷 시점에 결정).
	 * 읽는 자: fio_nvme_get_info — NVME_NS_DPS_PI_MASK / _FIRST 로 분리. */

	__u8			nmic;
	/* [한국어] Namespace Multi-path I/O and Namespace Sharing Capabilities. 미사용. */

	__u8			rescap;
	/* [한국어] Reservation Capabilities. 미사용. */

	__u8			fpi;
	/* [한국어] Format Progress Indicator. 미사용. */

	__u8			dlfeat;
	/* [한국어] Deallocate Logical Block Features. 디바이스가 deallocate 후 읽기 시
	 * 어떤 패턴을 반환하는지(0x00 / 0xFF / random). fio 미사용. */

	/* [한국어] 다음 16바이트(nawun~noiob) 는 atomic write 단위, atomic compare,
	 * boundary 정보. fio 는 NVMe atomic write 옵션 미지원이라 사용하지 않는다. */
	__le16			nawun;
	__le16			nawupf;
	__le16			nacwu;
	__le16			nabsn;
	__le16			nabo;
	__le16			nabspf;
	__le16			noiob;

	__u8			nvmcap[16];
	/* [한국어] NVM Capacity(128b 바이트 단위). 미사용. */

	/* [한국어] 다음 6워드(npwg~mssrl) 는 namespace preferred write granularity,
	 * deallocate granularity, MSSRL(Maximum Single Source Range Length) 등 dataset
	 * 힌트. fio 미사용. */
	__le16			npwg;
	__le16			npwa;
	__le16			npdg;
	__le16			npda;
	__le16			nows;
	__le16			mssrl;

	__le32			mcl;
	/* [한국어] Maximum Copy Length. Copy 명령 한도. 미사용. */

	__u8			msrc;
	/* [한국어] Maximum Source Range Count. Copy 명령 범위 수. 미사용. */

	__u8			rsvd81[11];
	/* [한국어] reserved(스펙 보존). */

	__le32			anagrpid;
	/* [한국어] ANA Group Identifier. 멀티패스. 미사용. */

	__u8			rsvd96[3];
	/* [한국어] reserved. */

	__u8			nsattr;
	/* [한국어] Namespace Attributes(write protected 등). 미사용. */

	__le16			nvmsetid;
	/* [한국어] NVM Set Identifier. 미사용. */

	__le16			endgid;
	/* [한국어] Endurance Group Identifier. 미사용. */

	__u8			nguid[16];
	/* [한국어] Namespace Globally Unique Identifier. 미사용. */

	__u8			eui64[8];
	/* [한국어] IEEE Extended Unique Identifier. 미사용. */

	struct nvme_lbaf	lbaf[64];
	/* [한국어] LBA Format Descriptors. nlbaf+1 개가 유효하며 flbas 인덱스가 활성
	 * 포맷을 가리킨다. 본 헤더는 최대 64 개분 공간을 모두 정의해 둔다.
	 * 읽는 자: fio_nvme_get_info — lbaf[format_idx].ds 와 .ms 사용,
	 *         fio_nvme_report_zones — lbaf 인덱스로 ZNS lbafe 매핑. */

	__u8			vs[3712];
	/* [한국어] Vendor Specific 영역. 4096 - 384 = 3712B. 미사용(레이아웃만 보존). */
};

/*
 * [한국어] struct nvme_id_psd: Power State Descriptor(32B). 컨트롤러의 각 전력
 * 상태(P0..P31) 별 최대전력/지연시간/RR/RW 특성을 기술. fio 는 사용하지 않으며
 * 본 헤더는 nvme_id_ctrl 의 psd[32] 슬롯 레이아웃 정합성만을 위해 정의.
 */
struct nvme_id_psd {
	__le16			mp;
	/* [한국어] Maximum Power(0.01W 단위). 미사용. */
	__u8			rsvd2;
	__u8			flags;
	/* [한국어] Max Power Scale, Non-Operational State 비트. 미사용. */
	__le32			enlat;
	/* [한국어] Entry Latency(us). 미사용. */
	__le32			exlat;
	/* [한국어] Exit Latency(us). 미사용. */
	__u8			rrt;
	/* [한국어] Relative Read Throughput. 미사용. */
	__u8			rrl;
	/* [한국어] Relative Read Latency. 미사용. */
	__u8			rwt;
	/* [한국어] Relative Write Throughput. 미사용. */
	__u8			rwl;
	/* [한국어] Relative Write Latency. 미사용. */
	__le16			idlp;
	/* [한국어] Idle Power. 미사용. */
	__u8			ips;
	/* [한국어] Idle Power Scale. 미사용. */
	__u8			rsvd19;
	__le16			actp;
	/* [한국어] Active Power. 미사용. */
	__u8			apws;
	/* [한국어] Active Power Scale + Workload. 미사용. */
	__u8			rsvd23[9];
};

/*
 * [한국어] NVMe Identify Controller 응답 구조체 (4096바이트)
 * nvme_admin_identify(CNS=0x01)로 조회. 컨트롤러의 기본 정보와 능력을 포함.
 * fio에서는 주로 ctratt(ELBAS 지원 여부)를 확인하는 데 사용.
 */
struct nvme_id_ctrl {
	/* [한국어] === 컨트롤러 식별 정보(0..71B) === */
	__le16			vid;
	/* [한국어] PCI Vendor ID. 미사용(레이아웃만). */
	__le16			ssvid;
	/* [한국어] PCI Subsystem Vendor ID. 미사용. */
	char			sn[20];
	/* [한국어] Serial Number(아스키, 공백 패딩). 미사용. */
	char			mn[40];
	/* [한국어] Model Number. 미사용. */
	char			fr[8];
	/* [한국어] Firmware Revision. 미사용. */

	__u8			rab;
	/* [한국어] Recommended Arbitration Burst. 미사용. */
	__u8			ieee[3];
	/* [한국어] IEEE OUI. 미사용. */
	__u8			cmic;
	/* [한국어] Controller Multi-path I/O. 미사용. */
	__u8			mdts;
	/* [한국어] Maximum Data Transfer Size(2^mdts * MPSMIN 페이지). 본 fio
	 * 미사용 — 사용자가 bs 옵션으로 직접 제어. */
	__le16			cntlid;
	/* [한국어] Controller Identifier. 미사용. */
	__le32			ver;
	/* [한국어] NVMe 스펙 버전(예: 0x10400 = 1.4). 미사용. */
	__le32			rtd3r;
	__le32			rtd3e;
	/* [한국어] RTD3 (Resume/Entry latency). 미사용. */
	__le32			oaes;
	/* [한국어] Optional Asynchronous Events Supported. 미사용. */

	__le32			ctratt;
	/* [한국어] Controller Attributes 32b 비트필드. fio 가 ELBAS 비트
	 * (NVME_CTRL_CTRATT_ELBAS = 1<<15) 만 검사해 NVM CSI Identify NS 호출 여부 결정.
	 * 설정자: 디바이스.
	 * 읽는 자: fio_nvme_get_info — `if (ctrl.ctratt & NVME_CTRL_CTRATT_ELBAS)`. */

	__le16			rrls;
	/* [한국어] Read Recovery Levels Supported. 미사용. */
	__u8			rsvd102[9];
	__u8			cntrltype;
	/* [한국어] Controller Type(I/O/Discovery/Admin). 미사용. */
	__u8			fguid[16];
	/* [한국어] FRU GUID. 미사용. */
	__le16			crdt1;
	__le16			crdt2;
	__le16			crdt3;
	/* [한국어] Command Retry Delay Timers. 미사용. */
	__u8			rsvd134[119];

	/* [한국어] === Admin/IO 명령 능력 === */
	__u8			nvmsr;
	__u8			vwci;
	__u8			mec;
	__le16			oacs;
	/* [한국어] Optional Admin Command Support. 미사용. */
	__u8			acl;
	/* [한국어] Abort Command Limit. 미사용. */
	__u8			aerl;
	/* [한국어] Asynchronous Event Request Limit. 미사용. */
	__u8			frmw;
	__u8			lpa;
	__u8			elpe;
	__u8			npss;
	/* [한국어] Number of Power States Supported(0-based). psd[] 배열 사용 한계. */
	__u8			avscc;
	__u8			apsta;
	__le16			wctemp;
	__le16			cctemp;
	__le16			mtfa;
	__le32			hmpre;
	__le32			hmmin;
	__u8			tnvmcap[16];
	__u8			unvmcap[16];
	__le32			rpmbs;
	__le16			edstt;
	__u8			dsto;
	__u8			fwug;
	__le16			kas;
	__le16			hctma;
	__le16			mntmt;
	__le16			mxtmt;
	__le32			sanicap;
	__le32			hmminds;
	__le16			hmmaxd;
	__le16			nsetidmax;
	__le16			endgidmax;
	__u8			anatt;
	__u8			anacap;
	__le32			anagrpmax;
	__le32			nanagrpid;
	__le32			pels;
	__le16			domainid;
	__u8			rsvd358[10];
	__u8			megcap[16];
	__u8			rsvd384[128];
	/* [한국어] 위 70여 바이트는 호스트메모리버퍼/Sanitize/ANA/도메인 등 fio 가
	 * 참조하지 않는 Admin 능력. 정의만 두어 4096B 레이아웃 정렬을 보장. */

	/* [한국어] === I/O 명령 정보 === */
	__u8			sqes;
	/* [한국어] Submission Queue Entry Size(req/min, 1바이트 안에 4b/4b). 미사용. */
	__u8			cqes;
	/* [한국어] Completion Queue Entry Size. 미사용. */
	__le16			maxcmd;
	/* [한국어] Maximum Outstanding Commands. 미사용. */
	__le32			nn;
	/* [한국어] Number of Namespaces. 미사용. */
	__le16			oncs;
	/* [한국어] Optional NVM Command Support(Compare/Write Uncor/DSM/Write Zeroes
	 * 지원 비트). fio 는 사용자 옵션을 신뢰하므로 이 필드를 직접 참조하지 않는다. */
	__le16			fuses;
	__u8			fna;
	__u8			vwc;
	/* [한국어] Volatile Write Cache 존재 여부. 미사용(fio 는 sync/dsync 옵션으로 결정). */
	__le16			awun;
	__le16			awupf;
	__u8			icsvscc;
	__u8			nwpc;
	__le16			acwu;
	__le16			ocfs;
	__le32			sgls;
	__le32			mnan;
	__u8			maxdna[16];
	__le32			maxcna;
	__u8			rsvd564[204];
	char			subnqn[NVME_NQN_LENGTH];
	/* [한국어] Subsystem NVMe Qualified Name(아스키, 256B 패딩). xnvme/fabrics
	 * 백엔드는 사용. 본 io_uring_cmd 경로 fio 는 미사용. */
	__u8			rsvd1024[768];

	/* Fabrics Only */
	/* [한국어] 다음 8필드(ioccsz~ofcs)는 NVMe-oF Fabrics 컨트롤러 전용
	 * 능력으로, 본 PCIe 기반 io_uring_cmd 엔진은 사용하지 않는다. */
	__le32			ioccsz;
	__le32			iorcsz;
	__le16			icdoff;
	__u8			fcatt;
	__u8			msdbd;
	__le16			ofcs;
	__u8			dctype;
	__u8			rsvd1807[241];

	struct nvme_id_psd	psd[32];
	/* [한국어] Power State Descriptors 32 개(npss+1 만 유효). 미사용. */
	__u8			vs[1024];
	/* [한국어] Vendor Specific 영역. 미사용. */
};

/*
 * [한국어] struct nvme_nvm_id_ns: NVM 커맨드 세트 전용 Identify Namespace
 * (CNS=0x05, CSI=NVM) 응답 4096B. ctratt 의 ELBAS(1<<15) 비트가 설정된
 * 컨트롤러에서만 의미가 있고, 가드 타입(16/32/64b) 과 storage tag 정보를
 * elbaf[format_idx] 32b 비트필드에서 추출한다.
 * 설정자: 디바이스(Identify 응답).
 * 읽는 자: fio_nvme_get_info — elbaf 와 STS 검사.
 */
struct nvme_nvm_id_ns {
	__le64			lbstm;
	/* [한국어] Logical Block Storage Tag Mask. storage tag 사용 시 어느 비트가
	 * 유효한지 표시. fio 는 storage tag 미지원이라 미사용. */
	__u8			pic;
	/* [한국어] Protection Information Capabilities(PI 위치/타입 능력). 미사용
	 * (ns.dps 의 활성 값을 신뢰). */
	__u8			rsvd9[3];
	__le32			elbaf[64];
	/* [한국어] Extended LBA Format Descriptors. 각 32b 항목 = [6:0] STS,
	 * [8:7] Guard Type, 그 외 reserved.
	 * 읽는 자: fio_nvme_get_info — elbaf[format_idx] 에서 STS=0 검증 후
	 *         (elbaf >> NVME_ID_NS_NVM_GUARD_SHIFT) & MASK 로 guard_type 추출. */
	__u8			rsvd268[3828];
	/* [한국어] reserved — 4096B 정합성. */
};

/*
 * [한국어] ilog2: 32비트 정수의 floor(log2) 계산. 0 입력 시 -1 반환(고정).
 * fio_nvme_get_info 가 lba_size(2의 거듭제곱) → lba_shift 변환에 사용. 헤더에
 * 인라인으로 두어 함수 호출 비용을 제거. lib/ 의 ilog2 와 충돌하지 않도록
 * 본 파일 스코프에서만 의미.
 */
static inline int ilog2(uint32_t i)
{
	int log = -1;
	/* [한국어] 시프트 카운터 — i 가 0 이 아닌 동안 1비트씩 오른쪽 시프트. */

	while (i) {                      /* [한국어] i 가 0 되면 종료. */
		i >>= 1;                 /* [한국어] 한 비트 제거(2 로 나누기). */
		log++;                   /* [한국어] log 증가 — 최종적으로 floor(log2(i_initial)). */
	}
	return log;                      /* [한국어] 입력 0 이면 -1, 1 이면 0, 4096 이면 12. */
}

/*
 * [한국어] struct nvme_zns_lbafe: ZNS LBA Format Extension. NVM 의 nvme_lbaf
 * 와 짝을 이루는 ZNS 전용 보조 정보. 같은 format_idx 위치에서 가져온다.
 */
struct nvme_zns_lbafe {
	__le64	zsze;
	/* [한국어] Zone Size(LBA 단위). 모든 존이 동일하다고 간주.
	 * 설정자: 디바이스.
	 * 읽는 자: fio_nvme_report_zones — zlen = zsze << lba_shift. */
	__u8	zdes;
	/* [한국어] Zone Descriptor Extension Size(64B 단위). 미사용. */
	__u8	rsvd9[7];
};

/*
 * [한국어] struct nvme_zns_id_ns: ZNS CSI Identify Namespace(CNS=0x05, CSI=ZNS)
 * 응답 4096B. ZNS 네임스페이스 속성 — 동시 오픈/액티브 존 한도, ZRWA 등.
 * 설정자: 디바이스.
 * 읽는 자: fio_nvme_get_max_open_zones, fio_nvme_report_zones.
 */
struct nvme_zns_id_ns {
	__le16			zoc;
	/* [한국어] Zone Operation Characteristics. 미사용. */
	__le16			ozcs;
	/* [한국어] Optional Zoned Command Support. 미사용. */
	__le32			mar;
	/* [한국어] Maximum Active Resources(0-based). 동시 active(open/closed) 존 한도.
	 * 미사용 — fio 는 mor 만 활용. */
	__le32			mor;
	/* [한국어] Maximum Open Resources(0-based). 동시 open 존 한도.
	 * 설정자: 디바이스.
	 * 읽는 자: fio_nvme_get_max_open_zones — *max_open_zones = mor + 1. */
	__le32			rrl;
	__le32			frl;
	__le32			rrl1;
	__le32			rrl2;
	__le32			rrl3;
	__le32			frl1;
	__le32			frl2;
	__le32			frl3;
	/* [한국어] Reset/Finish Recommended Limits. 미사용. */
	__le32			numzrwa;
	__le16			zrwafg;
	__le16			zrwasz;
	__u8			zrwacap;
	/* [한국어] Zone Random Write Area 능력. 미사용(fio 는 ZRWA 미지원). */
	__u8			rsvd53[2763];
	struct nvme_zns_lbafe	lbafe[64];
	/* [한국어] ZNS LBA Format Extensions. NVM lbaf 와 같은 인덱스로 매칭.
	 * 읽는 자: fio_nvme_report_zones — `lbafe[ns.flbas & 0x0f].zsze`. */
	__u8			vs[256];
	/* [한국어] Vendor Specific. 미사용. */
};

/*
 * [한국어] struct nvme_zns_desc: Zone Mgmt Receive 의 Report Zones 응답에서
 * 단일 존의 상태/위치/포인터를 기술하는 64B 디스크립터. ZNS 스펙의 Zone
 * Descriptor 표 1:1 대응.
 */
struct nvme_zns_desc {
	__u8	zt;
	/* [한국어] Zone Type([3:0]). 0x2 = SEQWRITE_REQ. 그 외는 fio 에러로 거부.
	 * 설정자: 디바이스.
	 * 읽는 자: fio_nvme_report_zones — switch(zt & 0x0f) → ZBD_ZONE_TYPE_SWR. */
	__u8	zs;
	/* [한국어] Zone State([7:4]). enum nvme_zns_zs 값.
	 * 읽는 자: fio_nvme_report_zones — switch(zs >> 4) → ZBD_ZONE_COND_*. */
	__u8	za;
	/* [한국어] Zone Attributes(Reset Recommended/Finish Recommended/ZRWA 활성 등).
	 * 미사용. */
	__u8	zai;
	/* [한국어] Zone Attributes Information. 미사용. */
	__u8	rsvd4[4];
	__le64	zcap;
	/* [한국어] Zone Capacity(LBA). 실제 쓰기 가능 용량(zsze 보다 작을 수 있음 —
	 * SLC zone 등). 읽는 자: fio_nvme_report_zones — capacity = zcap << lba_shift. */
	__le64	zslba;
	/* [한국어] Zone Start LBA. 읽는 자: report_zones — start = zslba << lba_shift. */
	__le64	wp;
	/* [한국어] Write Pointer(LBA). 다음 쓰기 위치. Empty 면 zslba, Full 면
	 * zslba+zcap. 읽는 자: report_zones — wp = wp << lba_shift. */
	__u8	rsvd32[32];
};

/*
 * [한국어] struct nvme_zone_report: Zone Mgmt Receive(Report Zones) 응답의
 * 페이로드 헤더(64B) + 가변 길이 nvme_zns_desc[] 배열. 호출자가 충분한 크기로
 * calloc 해 ioctl 의 addr 로 넘긴다.
 */
struct nvme_zone_report {
	__le64			nr_zones;
	/* [한국어] 응답에 실린 존 디스크립터 개수. 호출자가 요청한 크기보다 작을 수
	 * 있다(파셜 리포트). 읽는 자: fio_nvme_report_zones 의 j 루프 상한. */
	__u8			rsvd8[56];
	struct nvme_zns_desc	entries[];
	/* [한국어] flexible array — entries[0..nr_zones-1] 가 유효. C99 스타일. */
};

/*
 * [한국어] struct nvme_fdp_ruh_status_desc: FDP RUH(Reclaim Unit Handle) 상태
 * 디스크립터 32B. FDP 는 호스트가 PID(Placement ID) 를 통해 데이터 배치 힌트를
 * 디바이스에 전달하여 GC 로 인한 WAF 감소를 노린다. 이 구조체는 각 RUH 의
 * 잔존 capacity / 활성 시간을 디바이스가 보고하는 단위.
 */
struct nvme_fdp_ruh_status_desc {
	__u16 pid;
	/* [한국어] Placement Identifier. CDW13 [31:16] DSPEC 에 들어가는 값과 매칭. */
	__u16 ruhid;
	/* [한국어] Reclaim Unit Handle ID. PID 를 매핑하는 RUH 인덱스. */
	__u32 earutr;
	/* [한국어] Estimated Active Reclaim Unit Time Remaining(초). */
	__u64 ruamw;
	/* [한국어] Reclaim Unit Available Media Writes. */
	__u8  rsvd16[16];
};

/*
 * [한국어] struct nvme_fdp_ruh_status: I/O Mgmt Receive(RUHS, MO=1) 응답 헤더.
 * 호출자: dataplacement.c → fio_nvme_iomgmt_ruhs.
 */
struct nvme_fdp_ruh_status {
	__u8  rsvd0[14];
	__le16 nruhsd;
	/* [한국어] 응답에 실린 RUH 디스크립터 개수.
	 * 읽는 자: dataplacement.c 가 ruhss[0..nruhsd-1] 순회. */
	struct nvme_fdp_ruh_status_desc ruhss[];
	/* [한국어] flexible array — RUH 상태 디스크립터 배열. */
};

/*
 * [한국어] struct nvme_dsm_range: DSM(Deallocate=Trim) 명령의 단일 범위 디스크립터(16B).
 * 호스트가 page-aligned 버퍼를 만들어 디바이스 DMA 로 전송. 다중 범위는 배열로 연결.
 */
struct nvme_dsm_range {
	__le32	cattr;
	/* [한국어] Context Attributes. Access Frequency/Latency/RWHint 등 힌트.
	 * fio 는 0 으로만 사용. */
	__le32	nlb;
	/* [한국어] 범위의 LBA 수(1-based for deallocate — 1 = LBA 1개).
	 * 설정자: fio_nvme_uring_cmd_trim_prep — get_nlb(data, len) + 1. */
	__le64	slba;
	/* [한국어] 시작 LBA.
	 * 설정자: fio_nvme_uring_cmd_trim_prep — get_slba(data, offset). */
};

/*
 * [한국어] struct nvme_dsm: 엔진이 io_u 단위로 보유하는 DSM 페이로드 컨테이너.
 * NVMe 스펙 자체에는 없고 fio 가 nvme_dsm_range[] 와 nr_ranges 를 한 번에
 * 다루기 위해 도입.
 */
struct nvme_dsm {
	__u32 nr_ranges;
	/* [한국어] 이번 DSM 명령에 포함되는 범위 수. trim_prep 가 io_u->number_trim 으로 채움. */
	struct nvme_dsm_range range[];
	/* [한국어] flexible array — nr_ranges 개의 range 디스크립터.
	 * 읽는 자: 커널 driver — addr=&range[0] 로 DMA 전송. */
};

/*
 * [한국어] struct nvme_cmd_ext_io_opts: Read/Write 명령 발행 시 호스트가 지정
 * 하는 PI 관련 옵션 묶음. 엔진별 옵션 파싱 결과를 한 번에 전달하기 위한
 * 컨테이너로, fio_nvme_pi_fill / fio_nvme_generate_guard 인자로 사용된다.
 */
struct nvme_cmd_ext_io_opts {
	__u32 io_flags;
	/* [한국어] PRINFO 비트들(NVME_IO_PRINFO_PRACT/PRCHK_GUARD/APP/REF 의 OR).
	 * 설정자: 엔진 옵션(io_uring.c 의 cmd_pi_chk/cmd_pi_act 등). */
	__u16 apptag;
	/* [한국어] Application Tag 기대값. */
	__u16 apptag_mask;
	/* [한국어] Application Tag 비교 마스크. */
};

/*
 * [한국어] === 외부 링크 함수 선언 ===
 * 아래 선언된 함수의 본체는 모두 nvme.c 에 있다. 엔진(engines/io_uring.c 등)
 * 이 #include 후 직접 호출한다. 각 함수의 상세 §2 주석은 nvme.c 의 정의 위치
 * 에서 확인.
 */

/* [한국어] fio_nvme_iomgmt_ruhs: FDP Reclaim Unit Handle 상태 조회 래퍼.
 * I/O Mgmt Receive(opcode 0x12, MO=1) 명령으로 RUHS 를 가져온다.
 * caller: engines/io_uring.c, dataplacement.c.
 * return: 0 성공, -ENOTSUP FDP 미지원/오류. */
int fio_nvme_iomgmt_ruhs(struct thread_data *td, struct fio_file *f,
			 struct nvme_fdp_ruh_status *ruhs, __u32 bytes);

/* [한국어] fio_nvme_get_info: NVMe 캐릭터 디바이스의 Identify(Controller +
 * Namespace + 옵션 NVM CSI NS) 를 호출해 nvme_data 캐시를 채운다. nlba 출력
 * 으로 namespace 의 LBA 수도 반환.
 * caller: io_uring_cmd 엔진의 init/open_file. return: 0 성공, 1 비지원
 * 파일타입(블록 디바이스), 음수 errno 또는 ioctl 오류. */
int fio_nvme_get_info(struct fio_file *f, __u64 *nlba, __u32 pi_act,
		      struct nvme_data *data);

/* [한국어] fio_nvme_uring_cmd_prep: 메인 prep — io_u 의 ddir/offset/buf 를
 * nvme_uring_cmd 의 opcode/cdw10..15/addr/data_len/metadata 로 번역.
 * iov 가 NULL 이 아니면 vectored 모드(NVME_URING_CMD_IO_VEC).
 * return: 0 성공, -ENOTSUP 미지원 ddir. */
int fio_nvme_uring_cmd_prep(struct nvme_uring_cmd *cmd, struct io_u *io_u,
			    struct iovec *iov, struct nvme_dsm *dsm,
			    uint8_t read_opcode, uint8_t write_opcode,
			    unsigned int cdw12_flags);

/* [한국어] fio_nvme_pi_fill: 이미 빌드된 nvme_uring_cmd 에 PI 필드(cdw3/12/14/15)
 * 를 합성하고 호스트 PI(가드/apptag/reftag)를 사용자 버퍼에 기록. */
void fio_nvme_pi_fill(struct nvme_uring_cmd *cmd, struct io_u *io_u,
		      struct nvme_cmd_ext_io_opts *opts);

/* [한국어] fio_nvme_generate_guard: 16b/64b 가드 생성 디스패처. PI 활성 +
 * PRACT=0 일 때만 동작. fio_nvme_pi_fill 이 내부에서 호출. */
void fio_nvme_generate_guard(struct io_u *io_u, struct nvme_cmd_ext_io_opts *opts);

/* [한국어] fio_nvme_pi_verify: 읽은 데이터의 PI 검증 디스패처. 가드 타입에
 * 따라 16b/64b 검증 함수로 분기. return: 0 성공, -EIO 무결성 실패. */
int fio_nvme_pi_verify(struct nvme_data *data, struct io_u *io_u);

/* [한국어] fio_nvme_get_zoned_model: ZNS 디바이스 여부 판정. ZNS Identify 가
 * 성공하면 ZBD_HOST_MANAGED, 실패하면 ZBD_NONE. zbd.c 콜백. */
int fio_nvme_get_zoned_model(struct thread_data *td, struct fio_file *f,
			     enum zbd_zoned_model *model);

/* [한국어] fio_nvme_report_zones: ZNS Report Zones → fio zbd_zone[] 변환.
 * 청크 단위(기본 1024존) 반복으로 대규모 ZNS 도 처리. zbd.c 콜백. */
int fio_nvme_report_zones(struct thread_data *td, struct fio_file *f,
			  uint64_t offset, struct zbd_zone *zbdz,
			  unsigned int nr_zones);

/* [한국어] fio_nvme_reset_wp: ZNS Zone Mgmt Send(ZSA=Reset) 로 wp 를 zslba 로
 * 되돌린다. offset~offset+length 범위의 모든 존을 순차 리셋. */
int fio_nvme_reset_wp(struct thread_data *td, struct fio_file *f,
		      uint64_t offset, uint64_t length);

/* [한국어] fio_nvme_get_max_open_zones: ZNS Identify NS 의 mor + 1 반환.
 * zbd.c 가 동시 오픈 존 수 제한에 사용. */
int fio_nvme_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				unsigned int *max_open_zones);

/*
 * [한국어] put_unaligned_be48: 48비트 정수 val 의 상위→하위 6바이트를
 * big-endian 순서로 비정렬 버퍼에 기록. 64b 가드 PI 의 srtag(6B) 작성에 사용.
 *
 * @val: 저장할 64비트 값(상위 16비트는 무시되고 하위 48비트만 사용).
 * @p: 6바이트 이상 사용 가능한 출력 버퍼(정렬 요구 없음).
 *
 * caller: nvme.c::fio_nvme_generate_pi_64b_guard.
 * 헤더에 인라인으로 두어 호출 비용 제거 + 컴파일러가 64b 가드 hot loop 안에서
 * 인라인 전개해 6 회의 *p++ = byte 코드를 직접 생성한다.
 */
static inline void put_unaligned_be48(__u64 val, __u8 *p)
{
	*p++ = val >> 40;                /* [한국어] 비트 [47:40] = MSByte. */
	*p++ = val >> 32;                /* [한국어] 비트 [39:32]. */
	*p++ = val >> 24;                /* [한국어] 비트 [31:24]. */
	*p++ = val >> 16;                /* [한국어] 비트 [23:16]. */
	*p++ = val >> 8;                 /* [한국어] 비트 [15:8]. */
	*p++ = val;                      /* [한국어] 비트 [7:0] = LSByte. 암묵적 (__u8) 캐스트. */
}

/*
 * [한국어] get_unaligned_be48: put_unaligned_be48 의 역. 6 바이트 비정렬 big-
 * endian 데이터를 64비트 정수로 복원(상위 16b 는 0).
 *
 * @p: 6바이트 이상 읽을 수 있는 입력 버퍼(정렬 요구 없음).
 * @return: __u64 — 상위 16비트 0, 하위 48비트가 디코딩된 값.
 *
 * caller: nvme.c::fio_nvme_verify_pi_64b_guard 에서 srtag 비교 시.
 */
static inline __u64 get_unaligned_be48(__u8 *p)
{
	/* [한국어] 각 바이트를 (__u64) 로 캐스트해 시프트 — 32b 시프트 시 부호/오버플로
	 * 방지. p[3..5] 는 16/8/0 시프트라 32b 캐스트로도 안전하나 명시성을 위해
	 * 그대로 둔다. */
	return (__u64)p[0] << 40 | (__u64)p[1] << 32 | (__u64)p[2] << 24 |
		p[3] << 16 | p[4] << 8 | p[5];
}

/*
 * [한국어] fio_nvme_pi_ref_escape: 64b 가드의 srtag(6B) 가 모두 0xFF 인지
 * 확인 — Type3 PI escape 매직(이 블록을 비보호로 표시) 검사. 16b 가드는
 * NVME_PI_REF_DISABLE(0xFFFFFFFF) 매크로 직접 비교로 충분해 별도 헬퍼 없음.
 *
 * @reftag: 검사할 6바이트 srtag 포인터.
 * @return: bool — 모든 비트가 1 이면 true(검사 스킵), 아니면 false.
 *
 * caller: nvme.c::fio_nvme_verify_pi_64b_guard — Type3 분기.
 */
static inline bool fio_nvme_pi_ref_escape(__u8 *reftag)
{
	__u8 ref_esc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	/* [한국어] 비교 대상 상수 — 스택 6바이트. 컴파일러가 .rodata 로 옮길 수도. */

	return memcmp(reftag, ref_esc, sizeof(ref_esc)) == 0;
	/* [한국어] memcmp == 0 → 모든 비트가 1 → escape 매직. */
}

/*
 * [한국어] get_slba: byte offset 을 LBA 번호로 변환. 확장 LBA 포맷이면
 * lba_ext 로 나누고, 일반 포맷이면 lba_shift 비트 시프트(2의 거듭제곱 가속).
 *
 * @data: 네임스페이스 포맷 캐시(설정자 = fio_nvme_get_info).
 * @offset: io_u->offset 등의 바이트 오프셋.
 * @return: __u64 SLBA 번호.
 *
 * caller: fio_nvme_uring_cmd_prep, _trim_prep, fio_nvme_pi_fill, generate/verify
 *         _pi_*_guard 모든 PI 경로. 헤더 인라인이라 호출 비용 0.
 */
static inline __u64 get_slba(struct nvme_data *data, __u64 offset)
{
	if (data->lba_ext)               /* [한국어] 확장 LBA — 데이터+메타가 묶여 비-2거듭제곱 크기 가능. */
		return offset / data->lba_ext;

	return offset >> data->lba_shift; /* [한국어] 일반 포맷 — lba_size 가 2의 거듭제곱이라 시프트로 분할. */
}

/*
 * [한국어] get_nlb: byte length 를 NLB(0-based) 로 변환. NVMe Read/Write 의 NLB
 * 필드는 0-based(0=LBA 1개) 라 -1 이 들어간다.
 *
 * @data: 네임스페이스 포맷 캐시.
 * @len: io_u->xfer_buflen 등 바이트 길이.
 * @return: __u32 NLB(0-based) — cmd->cdw12 [15:0] 슬롯에 그대로 저장 가능.
 *
 * 주의: TRIM(DSM) 의 nlb 필드는 1-based 라 호출자가 +1 보정함(fio_nvme_uring_cmd_trim_prep).
 *
 * caller: fio_nvme_uring_cmd_prep, generate/verify_pi_*_guard(루프 상한 +1 환산).
 */
static inline __u32 get_nlb(struct nvme_data *data, __u64 len)
{
	if (data->lba_ext)
		return len / data->lba_ext - 1;     /* [한국어] 확장 LBA — 나눗셈 후 -1. */

	return (len >> data->lba_shift) - 1;        /* [한국어] 일반 포맷 — 시프트 후 -1. */
}

#endif
