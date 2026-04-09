/*
 * [한국어] optgroup.c - fio 옵션 그룹/카테고리 정의
 *
 * 이 파일은 fio 옵션들을 논리적 그룹과 카테고리로 분류하는 테이블을 정의한다.
 * --cmdhelp 등에서 옵션을 그룹별로 표시할 때 사용된다.
 *
 * 두 가지 그룹 체계:
 *   1) fio_opt_groups[]     - 상위 카테고리 (General, I/O, File, Statistics 등)
 *   2) fio_opt_cat_groups[] - 세부 카테고리 (Rate, Zone, Verify, 각 I/O 엔진별 등)
 *
 * 각 옵션은 비트마스크로 하나 이상의 그룹에 속할 수 있다.
 */
#include <stdio.h>
#include <inttypes.h>
#include "optgroup.h"
#include "compiler/compiler.h"

/*
 * Option grouping
 */
/* [한국어] 상위 옵션 카테고리 테이블 - 옵션의 큰 분류 */
static const struct opt_group fio_opt_groups[] = {
	{
		.name	= "General",        /* 일반 옵션 */
		.mask	= FIO_OPT_C_GENERAL,
	},
	{
		.name	= "I/O",            /* I/O 관련 옵션 */
		.mask	= FIO_OPT_C_IO,
	},
	{
		.name	= "File",           /* 파일 관련 옵션 */
		.mask	= FIO_OPT_C_FILE,
	},
	{
		.name	= "Statistics",     /* 통계 관련 옵션 */
		.mask	= FIO_OPT_C_STAT,
	},
	{
		.name	= "Logging",        /* 로깅 관련 옵션 */
		.mask	= FIO_OPT_C_LOG,
	},
	{
		.name	= "Profiles",       /* 프로파일 관련 옵션 */
		.mask	= FIO_OPT_C_PROFILE,
	},
	{
		.name	= "I/O engines",    /* I/O 엔진 관련 옵션 */
		.mask	= FIO_OPT_C_ENGINE,
	},
	{
		.name	= NULL,             /* 종료 표시 */
	},
};

/* [한국어] 세부 옵션 카테고리 테이블 - 옵션의 상세 분류
 * 각 I/O 엔진별 옵션 그룹도 여기에 포함된다. */
static const struct opt_group fio_opt_cat_groups[] = {
	{
		.name	= "Rate",                               /* 속도 제한 */
		.mask	= FIO_OPT_G_RATE,
	},
	{
		.name	= "Zone",                               /* 존(zone) 설정 */
		.mask	= FIO_OPT_G_ZONE,
	},
	{
		.name	= "Read/write mix",                     /* 읽기/쓰기 비율 */
		.mask	= FIO_OPT_G_RWMIX,
	},
	{
		.name	= "Verify",                             /* 데이터 검증 */
		.mask	= FIO_OPT_G_VERIFY,
	},
	{
		.name	= "Trim",                               /* TRIM/DISCARD */
		.mask	= FIO_OPT_G_TRIM,
	},
	{
		.name	= "I/O Logging",                        /* I/O 로깅 */
		.mask	= FIO_OPT_G_IOLOG,
	},
	{
		.name	= "I/O Depth",                          /* I/O 큐 깊이 */
		.mask	= FIO_OPT_G_IO_DEPTH,
	},
	{
		.name	= "I/O Flow",                           /* I/O 흐름 제어 */
		.mask	= FIO_OPT_G_IO_FLOW,
	},
	{
		.name	= "Description",                        /* 작업 설명 */
		.mask	= FIO_OPT_G_DESC,
	},
	{
		.name	= "Filename",                           /* 파일 이름 */
		.mask	= FIO_OPT_G_FILENAME,
	},
	{
		.name	= "General I/O",                        /* 기본 I/O 설정 */
		.mask	= FIO_OPT_G_IO_BASIC,
	},
	{
		.name	= "Cgroups",                            /* cgroup 설정 */
		.mask	= FIO_OPT_G_CGROUP,
	},
	{
		.name	= "Runtime",                            /* 실행 시간 */
		.mask	= FIO_OPT_G_RUNTIME,
	},
	{
		.name	= "Process",                            /* 프로세스/스레드 */
		.mask	= FIO_OPT_G_PROCESS,
	},
	{
		.name	= "Job credentials / priority",         /* 자격증명/우선순위 */
		.mask	= FIO_OPT_G_CRED,
	},
	{
		.name	= "Clock settings",                     /* 클럭 설정 */
		.mask	= FIO_OPT_G_CLOCK,
	},
	{
		.name	= "I/O Type",                           /* I/O 유형 */
		.mask	= FIO_OPT_G_IO_TYPE,
	},
	{
		.name	= "I/O Thinktime",                      /* I/O 간 대기 시간 */
		.mask	= FIO_OPT_G_THINKTIME,
	},
	{
		.name	= "Randomizations",                     /* 난수화 설정 */
		.mask	= FIO_OPT_G_RANDOM,
	},
	{
		.name	= "I/O buffers",                        /* I/O 버퍼 */
		.mask	= FIO_OPT_G_IO_BUF,
	},
	{
		.name	= "Tiobench profile",                   /* tiobench 프로파일 */
		.mask	= FIO_OPT_G_TIOBENCH,
	},
	{
		.name	= "Error handling",                     /* 에러 처리 */
		.mask	= FIO_OPT_G_ERR,
	},
	{
		.name	= "Ext4 defrag I/O engine", /* e4defrag */
		.mask	= FIO_OPT_G_E4DEFRAG,
	},
	{
		.name	= "Network I/O engine", /* net */
		.mask	= FIO_OPT_G_NETIO,
	},
	{
		.name	= "RDMA I/O engine", /* rdma */
		.mask	= FIO_OPT_G_RDMA,
	},
	{
		.name	= "libaio I/O engine", /* libaio */
		.mask	= FIO_OPT_G_LIBAIO,
	},
	{
		.name	= "ACT Aerospike like benchmark profile",
		.mask	= FIO_OPT_G_ACT,
	},
	{
		.name	= "Latency profiling",                  /* 지연시간 프로파일링 */
		.mask	= FIO_OPT_G_LATPROF,
	},
	{
		.name	= "RBD I/O engine", /* rbd */
		.mask	= FIO_OPT_G_RBD,
	},
	{
		.name	= "GlusterFS I/O engine", /* gfapi,gfapi_async */
		.mask	= FIO_OPT_G_GFAPI,
	},
	{
		.name	= "MTD I/O engine", /* mtd */
		.mask	= FIO_OPT_G_MTD,
	},
	{
		.name	= "libhdfs I/O engine", /* libhdfs */
		.mask	= FIO_OPT_G_HDFS,
	},
	{
		.name	= "NBD I/O engine", /* NBD */
		.mask	= FIO_OPT_G_NBD,
	},
	{
		.name	= "libcufile I/O engine", /* libcufile */
		.mask	= FIO_OPT_G_LIBCUFILE,
	},
	{
		.name	= "DAOS File System (dfs) I/O engine", /* dfs */
		.mask	= FIO_OPT_G_DFS,
	},
	{
		.name	= "NFS I/O engine", /* nfs */
		.mask	= FIO_OPT_G_NFS,
	},
	{
		.name	= NULL,             /* 종료 표시 */
	},
};

/* [한국어] 비트마스크에서 일치하는 옵션 그룹을 찾는 내부 함수
 * mask에서 해당 그룹의 비트를 제거하고 그룹을 반환한다.
 * 호출할 때마다 다음 그룹을 반환하므로, 반복 호출로 모든 그룹을 순회할 수 있다. */
static const struct opt_group *group_from_mask(const struct opt_group *ogs,
					       uint64_t *mask,
					       uint64_t inv_mask)
{
	int i;

	if (*mask == inv_mask || !*mask)
		return NULL;

	for (i = 0; ogs[i].name; i++) {
		const struct opt_group *og = &ogs[i];

		if (*mask & og->mask) {
			*mask &= ~(og->mask);
			return og;
		}
	}

	return NULL;
}

/* [한국어] 상위 카테고리 마스크에서 옵션 그룹을 찾는 외부 함수 */
const struct opt_group *opt_group_from_mask(uint64_t *mask)
{
	return group_from_mask(fio_opt_groups, mask, FIO_OPT_C_INVALID);
}

/* [한국어] 세부 카테고리 마스크에서 옵션 그룹을 찾는 외부 함수
 * 컴파일 타임에 카테고리 수가 uint64_t 비트 수를 초과하지 않는지 검증한다. */
const struct opt_group *opt_group_cat_from_mask(uint64_t *mask)
{
	compiletime_assert(__FIO_OPT_G_NR <= 8 * sizeof(uint64_t),
				"__FIO_OPT_G_NR");

	return group_from_mask(fio_opt_cat_groups, mask, FIO_OPT_G_INVALID);
}
