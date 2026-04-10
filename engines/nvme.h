// SPDX-License-Identifier: GPL-2.0
/*
 * nvme structure declarations and helper functions for the
 * io_uring_cmd engine.
 */
/*
 * [한국어 설명]
 * nvme.h - io_uring_cmd 엔진을 위한 NVMe 구조체 선언 및 헬퍼 함수
 *
 * === 개요 ===
 * io_uring의 IORING_OP_URING_CMD 기능을 통해 NVMe 디바이스에
 * 직접 명령(passthrough)을 전달하기 위한 인터페이스를 정의.
 * 일반 블록 I/O 스택(VFS → 블록 레이어 → 드라이버)을 우회하여
 * NVMe 컨트롤러에 직접 접근함으로써 최소 지연시간을 달성할 수 있음.
 *
 * === 주요 기능 영역 ===
 * 1. NVMe I/O 명령: read, write, flush, trim(DSM), write_zeroes
 * 2. 보호 정보 (Protection Information, PI):
 *    - 데이터 무결성 검증을 위한 가드 태그(CRC), 참조 태그, 애플리케이션 태그
 *    - 16비트 가드 (T10 DIF CRC16) 및 64비트 가드 (NVMe CRC64) 지원
 * 3. ZNS (Zoned Namespaces): 존 블록 디바이스 관리
 *    - 존 리포트, 존 리셋, 최대 오픈 존 수 조회
 * 4. FDP (Flexible Data Placement): 데이터 배치 제어
 *    - Reclaim Unit Handle 상태 조회
 *
 * === NVMe 네임스페이스 식별 ===
 * - Identify Controller (CNS=0x01): 컨트롤러 정보 (mdts, ctratt 등)
 * - Identify Namespace (CNS=0x00): 네임스페이스 정보 (LBA 크기, PI, 포맷 등)
 * - Identify NVM NS (CNS=0x05): NVM 커맨드 세트별 네임스페이스 정보 (elbaf 등)
 * - Identify ZNS NS (CNS=0x05, CSI=ZNS): ZNS 존 크기, 존 리소스 정보
 *
 * === 주요 구조체 ===
 * - nvme_uring_cmd: io_uring passthrough 명령 구조체
 * - nvme_data: fio 내부에서 사용하는 NVMe 디바이스 메타데이터
 * - nvme_id_ns/nvme_id_ctrl: NVMe Identify 응답 구조체
 * - nvme_pi_data: 보호 정보 설정
 * - nvme_dsm_range: Dataset Management(Trim) 범위 디스크립터
 */

#ifndef FIO_NVME_H
#define FIO_NVME_H

#include <linux/nvme_ioctl.h>  /* NVMe ioctl 정의: NVME_IOCTL_ADMIN_CMD, NVME_IOCTL_IO_CMD 등 */
#include "../fio.h"            /* fio 코어 프레임워크 */

/*
 * [한국어] 시스템 uapi 헤더에 nvme uring 명령 지원이 없을 경우
 * 컴파일 오류 방지를 위해 로컬에서 구조체를 정의.
 * nvme_uring_cmd는 io_uring의 IORING_OP_URING_CMD에서 사용되는
 * NVMe passthrough 명령 구조체.
 */
#ifndef CONFIG_NVME_URING_CMD
struct nvme_uring_cmd {
	__u8	opcode;        /* NVMe 명령 opcode (read=0x02, write=0x01 등) */
	__u8	flags;         /* 명령 플래그 */
	__u16	rsvd1;         /* 예약 필드 */
	__u32	nsid;          /* 대상 네임스페이스 ID */
	__u32	cdw2;          /* Command Dword 2 (명령별 용도) */
	__u32	cdw3;          /* Command Dword 3 */
	__u64	metadata;      /* 메타데이터 버퍼 주소 (PI 등) */
	__u64	addr;          /* 데이터 버퍼 주소 또는 iov 포인터 */
	__u32	metadata_len;  /* 메타데이터 버퍼 길이 */
	__u32	data_len;      /* 데이터 버퍼 길이 또는 iov 개수 */
	__u32	cdw10;         /* Command Dword 10 (보통 시작 LBA 하위 32비트) */
	__u32	cdw11;         /* Command Dword 11 (보통 시작 LBA 상위 32비트) */
	__u32	cdw12;         /* Command Dword 12 (보통 NLB + 플래그) */
	__u32	cdw13;         /* Command Dword 13 (DSPEC 등) */
	__u32	cdw14;         /* Command Dword 14 (참조 태그 등) */
	__u32	cdw15;         /* Command Dword 15 (앱 태그 + 마스크) */
	__u32	timeout_ms;    /* 명령 타임아웃 (밀리초) */
	__u32   rsvd2;         /* 예약 필드 */
};

/* [한국어] NVMe uring 명령 ioctl 매직 넘버
 * NVME_URING_CMD_IO: 단일 버퍼 I/O 명령
 * NVME_URING_CMD_IO_VEC: scatter-gather(iovec) I/O 명령 */
#define NVME_URING_CMD_IO	_IOWR('N', 0x80, struct nvme_uring_cmd)
#define NVME_URING_CMD_IO_VEC	_IOWR('N', 0x81, struct nvme_uring_cmd)
#endif /* CONFIG_NVME_URING_CMD */

#define NVME_DEFAULT_IOCTL_TIMEOUT 0      /* 기본 ioctl 타임아웃: 0 = 무제한 */
#define NVME_IDENTIFY_DATA_SIZE 4096      /* Identify 명령 응답 데이터 크기 (4KB) */
#define NVME_IDENTIFY_CSI_SHIFT 24        /* CDW11에서 CSI 필드의 비트 시프트 위치 */
#define NVME_NQN_LENGTH	256                /* NVMe Qualified Name 최대 길이 */

/* [한국어] 보호 정보(PI) 비활성화 매직 값
 * 이 값이 설정되면 해당 태그 검사를 건너뜀 */
#define NVME_PI_APP_DISABLE 0xFFFF        /* 애플리케이션 태그 비활성화 */
#define NVME_PI_REF_DISABLE 0xFFFFFFFF    /* 참조 태그 비활성화 (16비트 가드용 32비트) */

/* [한국어] ZNS (Zoned Namespaces) 관련 상수 */
#define NVME_ZNS_ZRA_REPORT_ZONES 0         /* Zone Report Action: 존 리포트 조회 */
#define NVME_ZNS_ZRAS_FEAT_ERZ (1 << 16)    /* 빈 존도 포함하여 리포트 (Extended Report Zones) */
#define NVME_ZNS_ZSA_RESET 0x4              /* Zone Send Action: 존 리셋 */
#define NVME_ZONE_TYPE_SEQWRITE_REQ 0x2     /* 순차 쓰기 필수 존 타입 */

#define NVME_ATTRIBUTE_DEALLOCATE (1 << 2)  /* DSM(Dataset Management) 할당 해제 속성 = Trim */

/* [한국어] Identify 명령의 CNS (Controller or Namespace Structure) 값
 * 어떤 정보를 조회할지 지정 */
enum nvme_identify_cns {
	NVME_IDENTIFY_CNS_NS		= 0x00,  /* 네임스페이스 정보 */
	NVME_IDENTIFY_CNS_CTRL		= 0x01,  /* 컨트롤러 정보 */
	NVME_IDENTIFY_CNS_CSI_NS	= 0x05,  /* 커맨드 세트별 네임스페이스 정보 */
	NVME_IDENTIFY_CNS_CSI_CTRL	= 0x06,  /* 커맨드 세트별 컨트롤러 정보 */
};

/* [한국어] NVMe Command Set Identifier (커맨드 세트 식별자) */
enum nvme_csi {
	NVME_CSI_NVM			= 0,  /* 기본 NVM 커맨드 세트 */
	NVME_CSI_KV			= 1,  /* Key-Value 커맨드 세트 */
	NVME_CSI_ZNS			= 2,  /* Zoned Namespace 커맨드 세트 */
};

/* [한국어] NVMe 관리자(Admin) 명령 opcode */
enum nvme_admin_opcode {
	nvme_admin_identify		= 0x06,  /* Identify 명령 */
};

/* [한국어] NVMe I/O 명령 opcode
 * 디바이스에 대한 실제 I/O 작업을 수행하는 명령들 */
enum nvme_io_opcode {
	nvme_cmd_flush			= 0x00,  /* 캐시 플러시 */
	nvme_cmd_write			= 0x01,  /* 쓰기 */
	nvme_cmd_read			= 0x02,  /* 읽기 */
	nvme_cmd_write_uncor		= 0x04,  /* 비정상 쓰기 (uncorrectable) */
	nvme_cmd_compare		= 0x05,  /* 비교 */
	nvme_cmd_write_zeroes		= 0x08,  /* 제로 쓰기 */
	nvme_cmd_dsm			= 0x09,  /* Dataset Management (Trim/Deallocate) */
	nvme_cmd_verify			= 0x0c,  /* 검증 */
	nvme_cmd_io_mgmt_recv		= 0x12,  /* I/O Management Receive (FDP 등) */
	nvme_zns_cmd_mgmt_send		= 0x79,  /* ZNS 존 관리 전송 (리셋 등) */
	nvme_zns_cmd_mgmt_recv		= 0x7a,  /* ZNS 존 관리 수신 (리포트 등) */
};

/* [한국어] ZNS 존 상태 (Zone State) 열거형
 * 존의 라이프사이클: Empty → Impl/Expl Open → Closed → Full */
enum nvme_zns_zs {
	NVME_ZNS_ZS_EMPTY		= 0x1,   /* 빈 존 - 쓰기 가능 */
	NVME_ZNS_ZS_IMPL_OPEN		= 0x2,   /* 암묵적 오픈 - 쓰기로 자동 오픈됨 */
	NVME_ZNS_ZS_EXPL_OPEN		= 0x3,   /* 명시적 오픈 - 명령으로 오픈됨 */
	NVME_ZNS_ZS_CLOSED		= 0x4,   /* 닫힘 - 재오픈 필요 */
	NVME_ZNS_ZS_READ_ONLY		= 0xd,   /* 읽기 전용 */
	NVME_ZNS_ZS_FULL		= 0xe,   /* 가득 참 - 리셋 필요 */
	NVME_ZNS_ZS_OFFLINE		= 0xf,   /* 오프라인 - 사용 불가 */
};

/* [한국어] 컨트롤러 속성 플래그
 * ELBAS: Extended LBA Formats 지원 → nvm_id_ns의 elbaf 필드로 가드/스토리지 태그 정보 조회 가능 */
enum nvme_id_ctrl_ctratt {
	NVME_CTRL_CTRATT_ELBAS		= 1 << 15,
};

/* [한국어] NVM 커맨드 세트 네임스페이스의 elbaf 필드 파싱용 마스크/시프트
 * elbaf[format_idx]에서 Storage Tag Size와 Guard Type을 추출 */
enum {
	NVME_ID_NS_NVM_STS_MASK		= 0x7f,  /* Storage Tag Size 마스크 (비트 0~6) */
	NVME_ID_NS_NVM_GUARD_SHIFT	= 7,     /* Guard Type 시작 비트 위치 */
	NVME_ID_NS_NVM_GUARD_MASK	= 0x3,   /* Guard Type 마스크 (2비트) */
};

/* [한국어] 가드 태그 유형 (PI에서 데이터 무결성 검증에 사용되는 체크섬 크기) */
enum {
	NVME_NVM_NS_16B_GUARD		= 0,  /* 16바이트 가드: T10 DIF CRC16 (8바이트 PI 튜플) */
	NVME_NVM_NS_32B_GUARD		= 1,  /* 32바이트 가드: (스토리지 태그 필수, 현재 미지원) */
	NVME_NVM_NS_64B_GUARD		= 2,  /* 64바이트 가드: NVMe CRC64 (16바이트 PI 튜플) */
};

/*
 * [한국어] fio 내부에서 사용하는 NVMe 디바이스 메타데이터
 * fio_nvme_get_info()에서 Identify 명령으로 채워지며,
 * FILE_ENG_DATA(f)로 접근하여 I/O 명령 생성 시 참조됨.
 */
struct nvme_data {
	__u32 nsid;       /* 네임스페이스 ID */
	__u32 lba_shift;  /* LBA 크기의 log2 값 (예: 512B→9, 4KB→12). lba_ext=0일 때 사용 */
	__u32 lba_size;   /* LBA 데이터 크기 (바이트) */
	__u32 lba_ext;    /* 확장 LBA 크기 (데이터+메타데이터). 0이면 메타데이터 별도 전송 */
	__u16 ms;         /* 메타데이터 크기 (바이트). PRACT=1이고 ms==pi_size면 0으로 설정 */
	__u16 pi_size;    /* 보호 정보(PI) 크기: 16비트 가드=8B, 64비트 가드=16B */
	__u8 pi_type;     /* PI 유형: 0=없음, 1=Type1, 2=Type2, 3=Type3 */
	__u8 guard_type;  /* 가드 태그 유형: 0=16비트, 2=64비트 */
	__u8 pi_loc;      /* PI 위치: 1=메타데이터 시작(first), 0=메타데이터 끝 */
};

/* [한국어] 네임스페이스 DPS (Data Protection Settings)
 * ns.dps 필드에서 PI 유형과 위치 정보를 추출하기 위한 마스크/플래그 */
enum nvme_id_ns_dps {
	NVME_NS_DPS_PI_NONE		= 0,       /* PI 없음 */
	NVME_NS_DPS_PI_TYPE1		= 1,       /* Type 1: 가드+앱태그+참조태그 (LBA 기반) */
	NVME_NS_DPS_PI_TYPE2		= 2,       /* Type 2: 가드+앱태그+참조태그 (명령 지정) */
	NVME_NS_DPS_PI_TYPE3		= 3,       /* Type 3: 가드+앱태그 (참조태그 선택적) */
	NVME_NS_DPS_PI_MASK		= 7 << 0,  /* PI 유형 마스크 (비트 0~2) */
	NVME_NS_DPS_PI_FIRST		= 1 << 3,  /* PI가 메타데이터 처음에 위치 */
};

/* [한국어] NVMe I/O 명령의 Protection Information 제어 플래그
 * CDW12의 상위 비트에 설정되어 PI 동작을 제어함 */
enum nvme_io_control_flags {
	NVME_IO_PRINFO_PRCHK_REF	= 1U << 26,  /* 참조 태그 검사 수행 */
	NVME_IO_PRINFO_PRCHK_APP	= 1U << 27,  /* 애플리케이션 태그 검사 수행 */
	NVME_IO_PRINFO_PRCHK_GUARD	= 1U << 28,  /* 가드 태그(CRC) 검사 수행 */
	NVME_IO_PRINFO_PRACT		= 1U << 29,  /* PI Action: 1=컨트롤러가 PI 처리 */
};

/*
 * [한국어] 보호 정보(PI) 런타임 데이터
 * io_u->engine_data에 저장되어 PI 생성/검증 시 사용됨.
 */
struct nvme_pi_data {
	__u32 interval;      /* 데이터 영역에서 PI까지의 오프셋 (바이트) */
	__u32 io_flags;      /* PI 검사 플래그 (PRCHK_GUARD/APP/REF) */
	__u16 apptag;        /* 기대하는 애플리케이션 태그 값 */
	__u16 apptag_mask;   /* 애플리케이션 태그 비교 마스크 */
};

/* [한국어] LBA 포맷 디스크립터
 * Identify Namespace 응답의 lbaf[] 배열 항목. 네임스페이스의 LBA 크기와 메타데이터 크기를 정의 */
struct nvme_lbaf {
	__le16			ms;   /* 메타데이터 크기 (바이트) */
	__u8			ds;   /* LBA 데이터 크기의 log2 값 (예: 9=512B, 12=4KB) */
	__u8			rp;   /* 상대적 성능 지표 (0=최고, 3=최저) */
};

/* [한국어] 16비트 가드 보호 정보 포맷 (T10 DIF 호환)
 * 총 8바이트. CRC16으로 데이터 무결성 검증 */
struct nvme_16b_guard_pif {
	__be16 guard;    /* CRC16 가드 태그 - 데이터 블록의 체크섬 */
	__be16 apptag;   /* 애플리케이션 태그 - 사용자 정의 식별자 */
	__be32 srtag;    /* 스토리지 참조 태그 - 보통 LBA 번호 (32비트) */
};

/* [한국어] 64비트 가드 보호 정보 포맷 (NVMe CRC64)
 * 총 16바이트. CRC64로 더 강력한 데이터 무결성 검증 */
struct nvme_64b_guard_pif {
	__be64 guard;    /* CRC64 가드 태그 */
	__be16 apptag;   /* 애플리케이션 태그 */
	__u8 srtag[6];   /* 스토리지 참조 태그 (48비트) */
};

/*
 * [한국어] NVMe Identify Namespace 응답 구조체 (4096바이트)
 * nvme_admin_identify(CNS=0x00)로 조회하며, 네임스페이스의 크기, LBA 포맷,
 * 보호 정보 설정 등 핵심 속성을 포함. fio에서는 주로 nsze, flbas, dps, lbaf를 사용.
 */
struct nvme_id_ns {
	__le64			nsze;   /* 네임스페이스 크기 (LBA 단위) → 파일 크기 계산에 사용 */
	__le64			ncap;
	__le64			nuse;
	__u8			nsfeat;
	__u8			nlbaf;  /* LBA 포맷 개수 - 1 */
	__u8			flbas;  /* 포맷된 LBA 설정: 하위 4비트=포맷 인덱스, 비트4=확장LBA 여부 */
	__u8			mc;     /* 메타데이터 능력 */
	__u8			dpc;    /* 데이터 보호 능력 */
	__u8			dps;    /* 데이터 보호 설정: 하위3비트=PI유형, 비트3=PI위치 */
	__u8			nmic;
	__u8			rescap;
	__u8			fpi;
	__u8			dlfeat;
	__le16			nawun;
	__le16			nawupf;
	__le16			nacwu;
	__le16			nabsn;
	__le16			nabo;
	__le16			nabspf;
	__le16			noiob;
	__u8			nvmcap[16];
	__le16			npwg;
	__le16			npwa;
	__le16			npdg;
	__le16			npda;
	__le16			nows;
	__le16			mssrl;
	__le32			mcl;
	__u8			msrc;
	__u8			rsvd81[11];
	__le32			anagrpid;
	__u8			rsvd96[3];
	__u8			nsattr;
	__le16			nvmsetid;
	__le16			endgid;
	__u8			nguid[16];
	__u8			eui64[8];
	struct nvme_lbaf	lbaf[64]; /* LBA 포맷 디스크립터 배열 (최대 64개 포맷) */
	__u8			vs[3712]; /* 벤더 특화 영역 */
};

/* [한국어] NVMe 전력 상태 디스크립터 (Power State Descriptor)
 * 컨트롤러의 각 전력 상태별 특성을 기술 */
struct nvme_id_psd {
	__le16			mp;
	__u8			rsvd2;
	__u8			flags;
	__le32			enlat;
	__le32			exlat;
	__u8			rrt;
	__u8			rrl;
	__u8			rwt;
	__u8			rwl;
	__le16			idlp;
	__u8			ips;
	__u8			rsvd19;
	__le16			actp;
	__u8			apws;
	__u8			rsvd23[9];
};

/*
 * [한국어] NVMe Identify Controller 응답 구조체 (4096바이트)
 * nvme_admin_identify(CNS=0x01)로 조회. 컨트롤러의 기본 정보와 능력을 포함.
 * fio에서는 주로 ctratt(ELBAS 지원 여부)를 확인하는 데 사용.
 */
struct nvme_id_ctrl {
	__le16			vid;    /* PCI Vendor ID */
	__le16			ssvid;
	char			sn[20];
	char			mn[40];
	char			fr[8];
	__u8			rab;
	__u8			ieee[3];
	__u8			cmic;
	__u8			mdts;
	__le16			cntlid;
	__le32			ver;
	__le32			rtd3r;
	__le32			rtd3e;
	__le32			oaes;
	__le32			ctratt;
	__le16			rrls;
	__u8			rsvd102[9];
	__u8			cntrltype;
	__u8			fguid[16];
	__le16			crdt1;
	__le16			crdt2;
	__le16			crdt3;
	__u8			rsvd134[119];
	__u8			nvmsr;
	__u8			vwci;
	__u8			mec;
	__le16			oacs;
	__u8			acl;
	__u8			aerl;
	__u8			frmw;
	__u8			lpa;
	__u8			elpe;
	__u8			npss;
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
	__u8			sqes;
	__u8			cqes;
	__le16			maxcmd;
	__le32			nn;
	__le16			oncs;
	__le16			fuses;
	__u8			fna;
	__u8			vwc;
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
	__u8			rsvd1024[768];

	/* Fabrics Only */
	__le32			ioccsz;
	__le32			iorcsz;
	__le16			icdoff;
	__u8			fcatt;
	__u8			msdbd;
	__le16			ofcs;
	__u8			dctype;
	__u8			rsvd1807[241];

	struct nvme_id_psd	psd[32];
	__u8			vs[1024];
};

/*
 * [한국어] NVM 커맨드 세트별 네임스페이스 정보 (CNS=0x05, CSI=NVM)
 * ELBAS(Extended LBA Format)를 지원하는 컨트롤러에서 가드 태그 유형과
 * 스토리지 태그 크기를 조회하는 데 사용됨.
 */
struct nvme_nvm_id_ns {
	__le64			lbstm;       /* 논리 블록 스토리지 태그 마스크 */
	__u8			pic;         /* 보호 정보 능력 */
	__u8			rsvd9[3];
	__le32			elbaf[64];   /* 확장 LBA 포맷 배열: 가드/스토리지 태그 정보 */
	__u8			rsvd268[3828];
};

/* [한국어] 정수의 log2 계산 (비트 시프트 방식). LBA 크기 → 시프트 값 변환에 사용 */
static inline int ilog2(uint32_t i)
{
	int log = -1;

	while (i) {
		i >>= 1;
		log++;
	}
	return log;
}

/* [한국어] ZNS LBA 포맷 확장 - 존 크기 정보 */
struct nvme_zns_lbafe {
	__le64	zsze;       /* 존 크기 (LBA 단위) */
	__u8	zdes;       /* 존 디스크립터 확장 크기 */
	__u8	rsvd9[7];
};

/*
 * [한국어] ZNS 네임스페이스 정보 (CNS=0x05, CSI=ZNS)
 * 존 블록 디바이스의 속성: 최대 오픈/액티브 존 수, 존 크기 등
 */
struct nvme_zns_id_ns {
	__le16			zoc;
	__le16			ozcs;
	__le32			mar;   /* 최대 액티브 리소스 (Max Active Resources) */
	__le32			mor;   /* 최대 오픈 리소스 (Max Open Resources) → max_open_zones 계산에 사용 */
	__le32			rrl;
	__le32			frl;
	__le32			rrl1;
	__le32			rrl2;
	__le32			rrl3;
	__le32			frl1;
	__le32			frl2;
	__le32			frl3;
	__le32			numzrwa;
	__le16			zrwafg;
	__le16			zrwasz;
	__u8			zrwacap;
	__u8			rsvd53[2763];
	struct nvme_zns_lbafe	lbafe[64];
	__u8			vs[256];
};

/* [한국어] 존 디스크립터 - Zone Report 응답의 개별 존 정보 */
struct nvme_zns_desc {
	__u8	zt;          /* 존 타입 (하위 4비트): 0x2=순차 쓰기 필수 */
	__u8	zs;          /* 존 상태 (상위 4비트): Empty/Open/Closed/Full 등 */
	__u8	za;          /* 존 속성 */
	__u8	zai;         /* 존 속성 정보 */
	__u8	rsvd4[4];
	__le64	zcap;        /* 존 용량 (LBA 단위) - 쓰기 가능한 LBA 수 */
	__le64	zslba;       /* 존 시작 LBA */
	__le64	wp;          /* 쓰기 포인터 (Write Pointer) - 다음 쓰기 위치 */
	__u8	rsvd32[32];
};

/* [한국어] Zone Report 응답 헤더 + 존 디스크립터 배열 */
struct nvme_zone_report {
	__le64			nr_zones;     /* 리포트된 존 수 */
	__u8			rsvd8[56];
	struct nvme_zns_desc	entries[];    /* 가변 길이 존 디스크립터 배열 */
};

/* [한국어] FDP (Flexible Data Placement) Reclaim Unit Handle 상태 디스크립터
 * FDP는 호스트가 데이터 배치를 제어하여 WAF(Write Amplification Factor)를 줄이는 기능 */
struct nvme_fdp_ruh_status_desc {
	__u16 pid;       /* Placement Identifier */
	__u16 ruhid;     /* Reclaim Unit Handle ID */
	__u32 earutr;    /* Estimated Active Reclaim Unit Time Remaining */
	__u64 ruamw;     /* Reclaim Unit Available Media Writes */
	__u8  rsvd16[16];
};

/* [한국어] FDP RUH 상태 응답 헤더 */
struct nvme_fdp_ruh_status {
	__u8  rsvd0[14];
	__le16 nruhsd;                              /* RUH 상태 디스크립터 수 */
	struct nvme_fdp_ruh_status_desc ruhss[];    /* 가변 길이 디스크립터 배열 */
};

/* [한국어] Dataset Management (Trim/Deallocate) 범위 디스크립터
 * 하나의 연속 LBA 범위를 기술 */
struct nvme_dsm_range {
	__le32	cattr;   /* 컨텍스트 속성 */
	__le32	nlb;     /* 범위의 LBA 수 (1-based for deallocate) */
	__le64	slba;    /* 시작 LBA */
};

/* [한국어] DSM 명령 데이터 구조체
 * 여러 범위를 한 번에 trim 할 수 있도록 범위 배열을 포함 */
struct nvme_dsm {
	__u32 nr_ranges;              /* 범위 수 */
	struct nvme_dsm_range range[]; /* 가변 길이 범위 배열 */
};

/* [한국어] 확장 I/O 옵션 - PI 관련 플래그와 애플리케이션 태그 설정
 * io_uring_cmd 엔진에서 PI를 사용할 때 NVMe 명령에 설정할 값들 */
struct nvme_cmd_ext_io_opts {
	__u32 io_flags;      /* PI 제어 플래그 (PRCHK_GUARD/APP/REF, PRACT) */
	__u16 apptag;        /* 애플리케이션 태그 값 */
	__u16 apptag_mask;   /* 애플리케이션 태그 비교 마스크 */
};

/* === [한국어] 함수 선언 === */

/* FDP Reclaim Unit Handle 상태 조회 (io_mgmt_recv 명령) */
int fio_nvme_iomgmt_ruhs(struct thread_data *td, struct fio_file *f,
			 struct nvme_fdp_ruh_status *ruhs, __u32 bytes);

/* NVMe 디바이스 정보 조회: Identify로 LBA 크기, PI 설정 등을 nvme_data에 채움 */
int fio_nvme_get_info(struct fio_file *f, __u64 *nlba, __u32 pi_act,
		      struct nvme_data *data);

/* NVMe passthrough 명령(nvme_uring_cmd) 준비: read/write/trim/flush에 맞게 CDW 설정 */
int fio_nvme_uring_cmd_prep(struct nvme_uring_cmd *cmd, struct io_u *io_u,
			    struct iovec *iov, struct nvme_dsm *dsm,
			    uint8_t read_opcode, uint8_t write_opcode,
			    unsigned int cdw12_flags);

/* PI 관련 CDW14/CDW15 설정 및 가드 태그 생성 */
void fio_nvme_pi_fill(struct nvme_uring_cmd *cmd, struct io_u *io_u,
		      struct nvme_cmd_ext_io_opts *opts);

/* 가드 태그(CRC) 생성: 16비트 또는 64비트 가드 유형에 따라 분기 */
void fio_nvme_generate_guard(struct io_u *io_u, struct nvme_cmd_ext_io_opts *opts);

/* 읽은 데이터의 PI 검증: 가드/앱/참조 태그 비교 */
int fio_nvme_pi_verify(struct nvme_data *data, struct io_u *io_u);

/* ZNS 디바이스 모델 확인: ZBD_HOST_MANAGED 또는 ZBD_NONE 반환 */
int fio_nvme_get_zoned_model(struct thread_data *td, struct fio_file *f,
			     enum zbd_zoned_model *model);

/* NVMe ZNS 존 리포트 조회 → fio의 zbd_zone 배열로 변환 */
int fio_nvme_report_zones(struct thread_data *td, struct fio_file *f,
			  uint64_t offset, struct zbd_zone *zbdz,
			  unsigned int nr_zones);

/* ZNS 존 쓰기 포인터 리셋 (Zone Management Send - Reset) */
int fio_nvme_reset_wp(struct thread_data *td, struct fio_file *f,
		      uint64_t offset, uint64_t length);

/* ZNS 최대 오픈 존 수 조회 (mor + 1) */
int fio_nvme_get_max_open_zones(struct thread_data *td, struct fio_file *f,
				unsigned int *max_open_zones);

/* [한국어] 48비트 big-endian 값을 비정렬 버퍼에 저장 (64비트 가드의 참조 태그용) */
static inline void put_unaligned_be48(__u64 val, __u8 *p)
{
	*p++ = val >> 40;
	*p++ = val >> 32;
	*p++ = val >> 24;
	*p++ = val >> 16;
	*p++ = val >> 8;
	*p++ = val;
}

/* [한국어] 비정렬 버퍼에서 48비트 big-endian 값을 읽어옴 */
static inline __u64 get_unaligned_be48(__u8 *p)
{
	return (__u64)p[0] << 40 | (__u64)p[1] << 32 | (__u64)p[2] << 24 |
		p[3] << 16 | p[4] << 8 | p[5];
}

/* [한국어] 참조 태그 이스케이프 확인 (모든 비트가 1이면 검사 건너뜀) */
static inline bool fio_nvme_pi_ref_escape(__u8 *reftag)
{
	__u8 ref_esc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	return memcmp(reftag, ref_esc, sizeof(ref_esc)) == 0;
}

/* [한국어] 바이트 오프셋에서 시작 LBA 번호 계산
 * 확장 LBA(lba_ext > 0)이면 나눗셈, 아니면 비트 시프트 사용 */
static inline __u64 get_slba(struct nvme_data *data, __u64 offset)
{
	if (data->lba_ext)
		return offset / data->lba_ext;

	return offset >> data->lba_shift;
}

/* [한국어] 바이트 길이에서 NLB(Number of Logical Blocks) 계산
 * NVMe NLB는 0-based이므로 -1 (예: 1블록=0, 2블록=1) */
static inline __u32 get_nlb(struct nvme_data *data, __u64 len)
{
	if (data->lba_ext)
		return len / data->lba_ext - 1;

	return (len >> data->lba_shift) - 1;
}

#endif
