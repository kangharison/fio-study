#ifndef FIO_OPT_GROUP_H
#define FIO_OPT_GROUP_H
/*
 * [한국어] optgroup.h - fio 옵션 그룹/카테고리 정의 헤더
 *
 * fio 옵션들을 논리적 그룹으로 분류하기 위한 구조체와 열거형을 정의한다.
 * 각 옵션은 비트마스크를 통해 상위 카테고리(opt_category)와
 * 세부 카테고리(opt_category_group)에 매핑된다.
 */

/* [한국어] 옵션 그룹 구조체 - 이름과 비트마스크 쌍 */
struct opt_group {
	const char *name;    /* 그룹 이름 (예: "General", "I/O") */
	uint64_t mask;       /* 비트마스크 값 */
};

/* [한국어] 상위 옵션 카테고리 열거형
 * __FIO_OPT_C_* : 비트 위치 인덱스 (0, 1, 2, ...)
 * FIO_OPT_C_*   : 실제 비트마스크 값 (1<<0, 1<<1, 1<<2, ...) */
enum opt_category {
	__FIO_OPT_C_GENERAL	= 0,
	__FIO_OPT_C_IO,
	__FIO_OPT_C_FILE,
	__FIO_OPT_C_STAT,
	__FIO_OPT_C_LOG,
	__FIO_OPT_C_PROFILE,
	__FIO_OPT_C_ENGINE,
	__FIO_OPT_C_NR,         /* 카테고리 총 개수 */

	FIO_OPT_C_GENERAL	= (1ULL << __FIO_OPT_C_GENERAL),
	FIO_OPT_C_IO		= (1ULL << __FIO_OPT_C_IO),
	FIO_OPT_C_FILE		= (1ULL << __FIO_OPT_C_FILE),
	FIO_OPT_C_STAT		= (1ULL << __FIO_OPT_C_STAT),
	FIO_OPT_C_LOG		= (1ULL << __FIO_OPT_C_LOG),
	FIO_OPT_C_PROFILE	= (1ULL << __FIO_OPT_C_PROFILE),
	FIO_OPT_C_ENGINE	= (1ULL << __FIO_OPT_C_ENGINE),
	FIO_OPT_C_INVALID	= (1ULL << __FIO_OPT_C_NR),   /* 유효하지 않은 마스크 (경계값) */
};

/* [한국어] 세부 옵션 카테고리 그룹 열거형
 * 각 I/O 엔진, 기능 영역별 세부 그룹을 정의한다.
 * __FIO_OPT_G_* : 비트 위치 인덱스
 * FIO_OPT_G_*   : 실제 비트마스크 값 */
enum opt_category_group {
	__FIO_OPT_G_RATE	= 0,       /* 속도 제한 */
	__FIO_OPT_G_ZONE,                 /* 존(zone) */
	__FIO_OPT_G_RWMIX,                /* 읽기/쓰기 비율 */
	__FIO_OPT_G_VERIFY,               /* 검증 */
	__FIO_OPT_G_TRIM,                 /* TRIM */
	__FIO_OPT_G_IOLOG,                /* I/O 로그 */
	__FIO_OPT_G_IO_DEPTH,             /* I/O 깊이 */
	__FIO_OPT_G_IO_FLOW,              /* I/O 흐름 */
	__FIO_OPT_G_DESC,                 /* 설명 */
	__FIO_OPT_G_FILENAME,             /* 파일명 */
	__FIO_OPT_G_IO_BASIC,             /* 기본 I/O */
	__FIO_OPT_G_CGROUP,               /* cgroup */
	__FIO_OPT_G_RUNTIME,              /* 실행 시간 */
	__FIO_OPT_G_PROCESS,              /* 프로세스 */
	__FIO_OPT_G_CRED,                 /* 자격증명 */
	__FIO_OPT_G_CLOCK,                /* 클럭 */
	__FIO_OPT_G_IO_TYPE,              /* I/O 유형 */
	__FIO_OPT_G_THINKTIME,            /* 대기 시간 */
	__FIO_OPT_G_RANDOM,               /* 난수화 */
	__FIO_OPT_G_IO_BUF,               /* I/O 버퍼 */
	__FIO_OPT_G_TIOBENCH,             /* tiobench */
	__FIO_OPT_G_ERR,                  /* 에러 처리 */
	__FIO_OPT_G_E4DEFRAG,             /* ext4 조각모음 */
	__FIO_OPT_G_NETIO,                /* 네트워크 I/O */
	__FIO_OPT_G_RDMA,                 /* RDMA */
	__FIO_OPT_G_LIBAIO,               /* libaio */
	__FIO_OPT_G_ACT,                  /* ACT 벤치마크 */
	__FIO_OPT_G_LATPROF,              /* 지연시간 프로파일링 */
	__FIO_OPT_G_RBD,                  /* Ceph RBD */
	__FIO_OPT_G_HTTP,                 /* HTTP */
	__FIO_OPT_G_GFAPI,                /* GlusterFS */
	__FIO_OPT_G_MTD,                  /* MTD 플래시 */
	__FIO_OPT_G_HDFS,                 /* HDFS */
	__FIO_OPT_G_SG,                   /* SCSI Generic */
	__FIO_OPT_G_MMAP,                 /* mmap */
	__FIO_OPT_G_ISCSI,                /* iSCSI */
	__FIO_OPT_G_NBD,                  /* NBD */
	__FIO_OPT_G_IOURING,              /* io_uring */
	__FIO_OPT_G_FILESTAT,             /* 파일 상태 */
	__FIO_OPT_G_NR,                   /* 그룹 총 개수 (경계값) */
	__FIO_OPT_G_LIBCUFILE,            /* libcufile (GPU 직접 I/O) */
	__FIO_OPT_G_DFS,                  /* DAOS 파일시스템 */
	__FIO_OPT_G_NFS,                  /* NFS */
	__FIO_OPT_G_WINDOWSAIO,           /* Windows AIO */
	__FIO_OPT_G_XNVME,               /* xNVMe */
	__FIO_OPT_G_LIBBLKIO,             /* libblkio */

	FIO_OPT_G_RATE		= (1ULL << __FIO_OPT_G_RATE),
	FIO_OPT_G_ZONE		= (1ULL << __FIO_OPT_G_ZONE),
	FIO_OPT_G_RWMIX		= (1ULL << __FIO_OPT_G_RWMIX),
	FIO_OPT_G_VERIFY	= (1ULL << __FIO_OPT_G_VERIFY),
	FIO_OPT_G_TRIM		= (1ULL << __FIO_OPT_G_TRIM),
	FIO_OPT_G_IOLOG		= (1ULL << __FIO_OPT_G_IOLOG),
	FIO_OPT_G_IO_DEPTH	= (1ULL << __FIO_OPT_G_IO_DEPTH),
	FIO_OPT_G_IO_FLOW	= (1ULL << __FIO_OPT_G_IO_FLOW),
	FIO_OPT_G_DESC		= (1ULL << __FIO_OPT_G_DESC),
	FIO_OPT_G_FILENAME	= (1ULL << __FIO_OPT_G_FILENAME),
	FIO_OPT_G_IO_BASIC	= (1ULL << __FIO_OPT_G_IO_BASIC),
	FIO_OPT_G_CGROUP	= (1ULL << __FIO_OPT_G_CGROUP),
	FIO_OPT_G_RUNTIME	= (1ULL << __FIO_OPT_G_RUNTIME),
	FIO_OPT_G_PROCESS	= (1ULL << __FIO_OPT_G_PROCESS),
	FIO_OPT_G_CRED		= (1ULL << __FIO_OPT_G_CRED),
	FIO_OPT_G_CLOCK		= (1ULL << __FIO_OPT_G_CLOCK),
	FIO_OPT_G_IO_TYPE	= (1ULL << __FIO_OPT_G_IO_TYPE),
	FIO_OPT_G_THINKTIME	= (1ULL << __FIO_OPT_G_THINKTIME),
	FIO_OPT_G_RANDOM	= (1ULL << __FIO_OPT_G_RANDOM),
	FIO_OPT_G_IO_BUF	= (1ULL << __FIO_OPT_G_IO_BUF),
	FIO_OPT_G_TIOBENCH	= (1ULL << __FIO_OPT_G_TIOBENCH),
	FIO_OPT_G_ERR		= (1ULL << __FIO_OPT_G_ERR),
	FIO_OPT_G_E4DEFRAG	= (1ULL << __FIO_OPT_G_E4DEFRAG),
	FIO_OPT_G_NETIO		= (1ULL << __FIO_OPT_G_NETIO),
	FIO_OPT_G_RDMA		= (1ULL << __FIO_OPT_G_RDMA),
	FIO_OPT_G_LIBAIO	= (1ULL << __FIO_OPT_G_LIBAIO),
	FIO_OPT_G_ACT		= (1ULL << __FIO_OPT_G_ACT),
	FIO_OPT_G_LATPROF	= (1ULL << __FIO_OPT_G_LATPROF),
	FIO_OPT_G_RBD		= (1ULL << __FIO_OPT_G_RBD),
	FIO_OPT_G_HTTP		= (1ULL << __FIO_OPT_G_HTTP),
	FIO_OPT_G_GFAPI		= (1ULL << __FIO_OPT_G_GFAPI),
	FIO_OPT_G_MTD		= (1ULL << __FIO_OPT_G_MTD),
	FIO_OPT_G_HDFS		= (1ULL << __FIO_OPT_G_HDFS),
	FIO_OPT_G_SG		= (1ULL << __FIO_OPT_G_SG),
	FIO_OPT_G_MMAP		= (1ULL << __FIO_OPT_G_MMAP),
	FIO_OPT_G_INVALID	= (1ULL << __FIO_OPT_G_NR),
	FIO_OPT_G_ISCSI         = (1ULL << __FIO_OPT_G_ISCSI),
	FIO_OPT_G_NBD		= (1ULL << __FIO_OPT_G_NBD),
	FIO_OPT_G_NFS		= (1ULL << __FIO_OPT_G_NFS),
	FIO_OPT_G_IOURING	= (1ULL << __FIO_OPT_G_IOURING),
	FIO_OPT_G_FILESTAT	= (1ULL << __FIO_OPT_G_FILESTAT),
	FIO_OPT_G_LIBCUFILE	= (1ULL << __FIO_OPT_G_LIBCUFILE),
	FIO_OPT_G_DFS		= (1ULL << __FIO_OPT_G_DFS),
	FIO_OPT_G_WINDOWSAIO	= (1ULL << __FIO_OPT_G_WINDOWSAIO),
	FIO_OPT_G_XNVME         = (1ULL << __FIO_OPT_G_XNVME),
	FIO_OPT_G_LIBBLKIO	= (1ULL << __FIO_OPT_G_LIBBLKIO),
};

extern const struct opt_group *opt_group_from_mask(uint64_t *mask);
extern const struct opt_group *opt_group_cat_from_mask(uint64_t *mask);

#endif
