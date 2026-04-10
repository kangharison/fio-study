/*
 * IO priority handling declarations and helper functions common to the
 * libaio and io_uring engines.
 */
/*
 * [한국어 설명]
 * cmdprio.h - libaio 및 io_uring 엔진에서 공통으로 사용하는 I/O 우선순위 처리 헤더
 *
 * === 개요 ===
 * fio에서 특정 비율의 I/O 요청에 높은 우선순위를 부여하여
 * 혼합 우선순위 워크로드(mixed priority workload)를 시뮬레이션할 수 있게 해주는 기능.
 * 이는 실제 프로덕션 환경에서 중요 I/O와 일반 I/O가 공존하는 상황을 재현하는 데 유용함.
 *
 * === 동작 모드 ===
 * 1. CMDPRIO_MODE_NONE: 우선순위 변경 없음 (기본값)
 * 2. CMDPRIO_MODE_PERC: 전체 I/O 중 일정 비율(%)에 높은 우선순위 적용
 *    - 예: cmdprio_percentage=20 → 20%의 I/O에 RT(Real-Time) 클래스 우선순위 부여
 * 3. CMDPRIO_MODE_BSSPLIT: 블록 크기별로 다른 우선순위 비율 적용
 *    - 예: cmdprio_bssplit=4k/10:64k/50 → 4K I/O는 10%, 64K I/O는 50%에 높은 우선순위
 *
 * === Linux I/O 우선순위 체계 ===
 * Linux ioprio는 32비트 값으로 구성:
 * - class (클래스): RT(실시간), BE(최선노력), IDLE(유휴) 3단계
 * - level (레벨): 클래스 내 세부 우선순위 (0~7, 낮을수록 높은 우선순위)
 * - hint (힌트): 스케줄러에 대한 추가 힌트 정보
 *
 * === 주요 구조체 ===
 * - cmdprio_options: fio 옵션에서 파싱된 사용자 설정값
 * - cmdprio_prio: 개별 우선순위 항목 (우선순위 값 + 비율 + 통계 인덱스)
 * - cmdprio_bsprio: 특정 블록 크기에 대한 우선순위 목록
 * - cmdprio: 엔진에서 사용하는 최종 우선순위 제어 구조체
 *
 * === 주요 함수 ===
 * - fio_cmdprio_init(): 옵션 파싱 후 우선순위 구조체 초기화
 * - fio_cmdprio_set_ioprio(): 개별 io_u에 대해 확률적으로 우선순위 설정
 * - fio_cmdprio_cleanup(): 자원 해제
 */

#ifndef FIO_CMDPRIO_H
#define FIO_CMDPRIO_H

#include "../fio.h"        /* fio 코어 구조체 (thread_data, io_u 등) */
#include "../optgroup.h"   /* 옵션 그룹 정의 매크로 */

/* [한국어] read와 write만 지원, trim은 우선순위 변경 미지원
 * DDIR_READ=0, DDIR_WRITE=1 이므로 인덱스로 사용 가능 */
#define CMDPRIO_RWDIR_CNT 2

/* [한국어] cmdprio 동작 모드 열거형 */
enum {
	CMDPRIO_MODE_NONE,    /* 우선순위 변경 없음 - cmdprio 미사용 */
	CMDPRIO_MODE_PERC,    /* 백분율 모드 - 전체 I/O 중 일정 비율에 우선순위 적용 */
	CMDPRIO_MODE_BSSPLIT, /* 블록크기별 분할 모드 - 블록 크기마다 다른 비율 적용 */
};

/*
 * [한국어] 개별 우선순위 항목
 * 하나의 우선순위 설정을 나타내며, 해당 우선순위가 적용될 확률과
 * 완료 지연시간(clat) 통계를 위한 인덱스를 포함함.
 */
struct cmdprio_prio {
	int32_t prio;              /* ioprio 값 (class + level + hint 조합) */
	uint32_t perc;             /* 이 우선순위가 적용될 확률 (0~100%) */
	uint16_t clat_prio_index;  /* 우선순위별 clat 통계 배열의 인덱스 */
};

/*
 * [한국어] 특정 블록 크기에 대한 우선순위 설정 집합
 * BSSPLIT 모드에서 사용되며, 하나의 블록 크기에 여러 우선순위를
 * 각각 다른 비율로 적용할 수 있음.
 * 예: 4K 블록에 대해 RT class 10%, BE class 30% 등
 */
struct cmdprio_bsprio {
	uint64_t bs;               /* 블록 크기 (바이트) */
	uint32_t tot_perc;         /* 모든 우선순위 비율의 합 (최대 100%) */
	unsigned int nr_prios;     /* prios 배열의 항목 수 */
	struct cmdprio_prio *prios; /* 우선순위 항목 배열 */
};

/*
 * [한국어] 블록 크기별 우선순위 설정 디스크립터
 * 하나의 ddir(read 또는 write)에 대한 모든 블록 크기별 우선순위 설정을 보관.
 */
struct cmdprio_bsprio_desc {
	struct cmdprio_bsprio *bsprios;  /* 블록 크기별 우선순위 배열 */
	unsigned int nr_bsprios;         /* bsprios 배열의 항목 수 */
};

/*
 * [한국어] 사용자가 fio 설정 파일에서 지정한 cmdprio 옵션들
 * 이 구조체는 td->eo (engine options)의 일부로 저장됨.
 * 각 배열은 [DDIR_READ]와 [DDIR_WRITE]에 대한 값을 개별 보관.
 */
struct cmdprio_options {
	unsigned int percentage[CMDPRIO_RWDIR_CNT]; /* cmdprio_percentage: I/O 중 고우선순위 비율 */
	unsigned int class[CMDPRIO_RWDIR_CNT];      /* cmdprio_class: ioprio 클래스 (RT/BE/IDLE) */
	unsigned int level[CMDPRIO_RWDIR_CNT];      /* cmdprio: ioprio 레벨 (0~7) */
	unsigned int hint[CMDPRIO_RWDIR_CNT];       /* cmdprio_hint: 스케줄러 힌트 */
	char *bssplit_str;                           /* cmdprio_bssplit: 블록크기별 분할 문자열 */
};

/*
 * [한국어] CMDPRIO_OPTIONS 매크로
 * libaio/io_uring 엔진의 옵션 배열에 포함시켜 cmdprio 관련 옵션을 등록하는 매크로.
 * FIO_HAVE_IOPRIO_CLASS가 정의된 경우 실제 옵션을, 아닌 경우 UNSUPPORTED를 등록.
 *
 * 옵션 목록:
 * - cmdprio_percentage: 고우선순위 I/O 비율 (0~100%)
 * - cmdprio_class: I/O 우선순위 클래스 (1=RT, 2=BE, 3=IDLE)
 * - cmdprio_hint: I/O 우선순위 힌트
 * - cmdprio: I/O 우선순위 레벨 (클래스 내 세부 순위)
 * - cmdprio_bssplit: 블록 크기별 우선순위 비율 분할 설정 문자열
 */
#ifdef FIO_HAVE_IOPRIO_CLASS
#define CMDPRIO_OPTIONS(opt_struct, opt_group)					\
	{									\
		.name	= "cmdprio_percentage",					\
		.lname	= "high priority percentage",				\
		.type	= FIO_OPT_INT,						\
		.off1	= offsetof(opt_struct,					\
				   cmdprio_options.percentage[DDIR_READ]),	\
		.off2	= offsetof(opt_struct,					\
				   cmdprio_options.percentage[DDIR_WRITE]),	\
		.minval	= 0,							\
		.maxval	= 100,							\
		.help	= "Send high priority I/O this percentage of the time",	\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	},									\
	{									\
		.name	= "cmdprio_class",					\
		.lname	= "Asynchronous I/O priority class",			\
		.type	= FIO_OPT_INT,						\
		.off1	= offsetof(opt_struct,					\
				   cmdprio_options.class[DDIR_READ]),		\
		.off2	= offsetof(opt_struct,					\
				   cmdprio_options.class[DDIR_WRITE]),		\
		.help	= "Set asynchronous IO priority class",			\
		.minval	= IOPRIO_MIN_PRIO_CLASS + 1,				\
		.maxval	= IOPRIO_MAX_PRIO_CLASS,				\
		.interval = 1,							\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	},									\
	{									\
		.name	= "cmdprio_hint",					\
		.lname	= "Asynchronous I/O priority hint",			\
		.type	= FIO_OPT_INT,						\
		.off1	= offsetof(opt_struct,					\
				   cmdprio_options.hint[DDIR_READ]),		\
		.off2	= offsetof(opt_struct,					\
				   cmdprio_options.hint[DDIR_WRITE]),		\
		.help	= "Set asynchronous IO priority hint",			\
		.minval	= IOPRIO_MIN_PRIO_HINT,					\
		.maxval	= IOPRIO_MAX_PRIO_HINT,					\
		.interval = 1,							\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	},									\
	{									\
		.name	= "cmdprio",						\
		.lname	= "Asynchronous I/O priority level",			\
		.type	= FIO_OPT_INT,						\
		.off1	= offsetof(opt_struct,					\
				   cmdprio_options.level[DDIR_READ]),		\
		.off2	= offsetof(opt_struct,					\
				   cmdprio_options.level[DDIR_WRITE]),		\
		.help	= "Set asynchronous IO priority level",			\
		.minval	= IOPRIO_MIN_PRIO,					\
		.maxval	= IOPRIO_MAX_PRIO,					\
		.interval = 1,							\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	},									\
	{									\
		.name   = "cmdprio_bssplit",					\
		.lname  = "Priority percentage block size split",		\
		.type   = FIO_OPT_STR_STORE,					\
		.off1   = offsetof(opt_struct, cmdprio_options.bssplit_str),	\
		.help   = "Set priority percentages for different block sizes",	\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	}
#else
#define CMDPRIO_OPTIONS(opt_struct, opt_group)					\
	{									\
		.name	= "cmdprio_percentage",					\
		.lname	= "high priority percentage",				\
		.type	= FIO_OPT_UNSUPPORTED,					\
		.help	= "Platform does not support I/O priority classes",	\
	},									\
	{									\
		.name	= "cmdprio_class",					\
		.lname	= "Asynchronous I/O priority class",			\
		.type	= FIO_OPT_UNSUPPORTED,					\
		.help	= "Platform does not support I/O priority classes",	\
	},									\
	{									\
		.name	= "cmdprio_hint",					\
		.lname	= "Asynchronous I/O priority hint",			\
		.type	= FIO_OPT_UNSUPPORTED,					\
		.help	= "Platform does not support I/O priority classes",	\
	},									\
	{									\
		.name	= "cmdprio",						\
		.lname	= "Asynchronous I/O priority level",			\
		.type	= FIO_OPT_UNSUPPORTED,					\
		.help	= "Platform does not support I/O priority classes",	\
	},									\
	{									\
		.name   = "cmdprio_bssplit",					\
		.lname  = "Priority percentage block size split",		\
		.type	= FIO_OPT_UNSUPPORTED,					\
		.help	= "Platform does not support I/O priority classes",	\
	}
#endif

/*
 * [한국어] cmdprio 메인 제어 구조체
 * I/O 엔진(libaio, io_uring)의 엔진 데이터에 포함되어
 * 런타임에 I/O 우선순위 결정 로직을 수행함.
 */
struct cmdprio {
	struct cmdprio_options *options;                       /* 사용자 옵션 포인터 (td->eo 내부) */
	struct cmdprio_prio perc_entry[CMDPRIO_RWDIR_CNT];    /* PERC 모드: read/write별 우선순위 항목 */
	struct cmdprio_bsprio_desc bsprio_desc[CMDPRIO_RWDIR_CNT]; /* BSSPLIT 모드: read/write별 블록크기별 설정 */
	unsigned int mode;                                     /* 현재 동작 모드 (NONE/PERC/BSSPLIT) */
};

/*
 * [한국어] 개별 io_u에 대해 확률적으로 우선순위를 설정
 * 난수를 생성하여 설정된 비율 내에 들면 io_u->ioprio를 변경.
 * 반환값: true면 우선순위가 변경됨, false면 기본 우선순위 유지.
 */
bool fio_cmdprio_set_ioprio(struct thread_data *td, struct cmdprio *cmdprio,
			    struct io_u *io_u);

/* [한국어] cmdprio 자원 해제. bsprio 배열과 prios 배열을 free함. */
void fio_cmdprio_cleanup(struct cmdprio *cmdprio);

/*
 * [한국어] cmdprio 초기화
 * 옵션을 분석하여 동작 모드를 결정하고, 해당 모드에 맞는
 * 우선순위 구조체와 clat 통계 배열을 생성함.
 * cmdprio_percentage와 cmdprio_bssplit은 상호 배타적 (동시 사용 불가).
 */
int fio_cmdprio_init(struct thread_data *td, struct cmdprio *cmdprio,
		     struct cmdprio_options *options);

#endif
