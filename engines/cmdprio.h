/*
 * IO priority handling declarations and helper functions common to the
 * libaio and io_uring engines.
 */
/*
 * [한국어 설명] I/O 우선순위 제어 유틸 헤더 (cmdprio.h)
 *
 * === 파일의 역할 ===
 * libaio / io_uring / sg 세 비동기·동기 엔진이 공유하는 "I/O 우선순위 믹스(priority mix)"
 * 기능의 선언 집합이다. 잡 하나 안에서 각 개별 io_u 마다 Linux의 ioprio(class + level + hint)
 * 를 동적으로 교체하여, "80% 일반 우선순위 + 20% RT 우선순위 같은 혼합 워크로드"를
 * 재현할 수 있게 한다. 두 가지 축(ddir × 블록 크기) 로 비율을 독립 제어할 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 엔진별 init() 콜백에서 fio_cmdprio_init() 를 불러 cmdprio 상태를 1회 구축하고,
 * queue() 콜백의 매 io_u 준비 시점에서 fio_cmdprio_set_ioprio() 를 호출하여
 * io_u->ioprio 를 교체한다. 그 값은 libaio 쪽에서는 iocb->aio_reqprio (libaio
 * >=0.3.110 의 IOCB_FLAG_IOPRIO ABI 비트)로, io_uring 쪽에서는 sqe->ioprio 로,
 * sg 쪽에서는 ioprio_set(2) syscall 로 각각 커널에 전달된다. 잡 종료 시
 * fio_cmdprio_cleanup() 이 bsprio/prios 동적 배열을 해제한다.
 *
 * === 타 모듈과의 연결 ===
 * - ../fio.h: thread_data, io_u, DDIR_READ/WRITE 등 fio 코어 타입.
 * - ../optgroup.h: FIO_OPT_C_ENGINE, 그룹 매크로(FIO_OPT_G_LIBAIO/IOURING/SG).
 * - ../lib/rand.h: BSSPLIT 모드에서 확률 추첨용 RNG 호출 경로 참조(구현 .c 에서 사용).
 * - 공유 상태: cmdprio 구조체는 엔진별 td->io_ops_data 안에 내장되며 잡 스레드 1명 소유.
 *   bsprio 배열은 잡 시작 후 불변(init 직후 구축, cleanup 전까지 읽기 전용).
 * - FIO_HAVE_IOPRIO_CLASS: configure 스크립트가 감지하는 기능 매크로 — 플랫폼이
 *   ioprio_set(2)/IOPRIO_CLASS_* 를 지원할 때만 진짜 옵션을 등록하고, 아니면
 *   UNSUPPORTED 자리표시를 등록한다(사용자에게 가시적 에러 메시지 제공).
 *
 * === 주요 함수/구조체 요약 ===
 * - enum CMDPRIO_MODE_{NONE,PERC,BSSPLIT}: 3가지 동작 모드.
 * - struct cmdprio_prio: 단일 ioprio 항목 + 적용 확률 + clat 통계 인덱스.
 * - struct cmdprio_bsprio: 특정 블록 크기 bucket에 속한 cmdprio_prio 배열.
 * - struct cmdprio_bsprio_desc: 한 ddir 에 대한 모든 블록크기 bucket 집합.
 * - struct cmdprio_options: 사용자 옵션 원본(cmdprio_percentage/class/level/hint/bssplit).
 * - struct cmdprio: 런타임 상태(mode + perc_entry + bsprio_desc + options 포인터).
 * - fio_cmdprio_init(): 옵션 파싱 결과를 런타임 구조로 변환. PERC/BSSPLIT 배타 검증.
 * - fio_cmdprio_set_ioprio(): 개별 io_u 에 확률적으로 ioprio 주입.
 * - fio_cmdprio_cleanup(): 동적 배열 해제.
 * - CMDPRIO_OPTIONS 매크로: libaio/io_uring/sg 엔진 options[] 배열에 일괄 삽입.
 *
 * === Linux I/O 우선순위 체계 요약 ===
 * ioprio 는 32비트 값 (class << 13) | level, 상위 비트에 hint(6.4+ 커널) 포함.
 * - class: IOPRIO_CLASS_RT(1, 실시간) / BE(2, 최선노력 기본) / IDLE(3, 유휴)
 * - level: 0~7 (낮을수록 높은 우선순위, CFQ/BFQ 스케줄러가 해석)
 * - hint: DEVICE DURATION LIMIT 등 최신 커널의 힌트 비트(선택)
 * syscall: ioprio_set(IOPRIO_WHO_PROCESS, tid, ioprio) / libaio: iocb->aio_reqprio
 * / io_uring: sqe->ioprio.
 */

#ifndef FIO_CMDPRIO_H
#define FIO_CMDPRIO_H

#include "../fio.h"        /* [한국어] thread_data, io_u, DDIR_READ/WRITE 등 fio 코어 구조체. */
#include "../optgroup.h"   /* [한국어] FIO_OPT_C_ENGINE / FIO_OPT_G_* 옵션 분류 매크로. */

/* [한국어] cmdprio 가 지원하는 ddir 종류 수.
 * read/write 만 지원(TRIM 은 ioprio 의미가 스케줄러마다 불명확하여 제외).
 * DDIR_READ=0, DDIR_WRITE=1 이 인덱스로 쓰인다. 배열 선언에 사용. */
#define CMDPRIO_RWDIR_CNT 2

/* [한국어] cmdprio 동작 모드 열거형.
 * 잡 시작 후 불변(init 에서 확정). queue() 시 모드별 브랜치로 확률 계산 경로가 갈린다. */
enum {
	CMDPRIO_MODE_NONE,
	/* [한국어] 우선순위 변경 없음 — 사용자가 cmdprio 관련 옵션 전무.
	 *  fio_cmdprio_set_ioprio()는 즉시 false 반환하여 io_u->ioprio 를 td 기본값 그대로 유지. */

	CMDPRIO_MODE_PERC,
	/* [한국어] 백분율 모드 — ddir 당 단일 "고우선순위" 항목 + 그 적용 비율(%) 지정.
	 *  예: cmdprio_percentage=20,cmdprio_class=1 → read/write 의 20% 가 RT-class 로 발사.
	 *  나머지 80% 는 td 기본 ioprio(혹은 커널 상속) 유지. cmdprio.perc_entry[ddir] 에 구축. */

	CMDPRIO_MODE_BSSPLIT,
	/* [한국어] 블록 크기별 분할 모드 — bssplit 문자열을 파싱해 각 블록 크기마다 독립 비율.
	 *  예: cmdprio_bssplit=4k/10:64k/50 → 4KB I/O 중 10%·64KB I/O 중 50% 를 고우선순위로.
	 *  ddir 당 cmdprio_bsprio_desc 에 bucket 배열로 구축. PERC 와 동시 사용 불가(init 검증). */
};

/*
 * [한국어] 개별 우선순위 항목 — "이 ioprio 값을 P% 확률로 사용" 을 표현.
 * 하나의 cmdprio_bsprio 안에 여러 개가 누적되며, 합은 최대 100%.
 */
struct cmdprio_prio {
	int32_t prio;
	/* [한국어] 실제로 io_u->ioprio 에 주입될 32비트 ioprio 값.
	 * 인코딩: ioprio_value(class, level, hint) 매크로(fio.h) 결과 — (class<<13)|level + hint 상위비트.
	 * 설정자: init 에서 사용자 options (class/level/hint) 조합으로 빌드.
	 * 읽는 자: fio_cmdprio_set_ioprio() 가 io_u->ioprio = prio 대입.
	 * 값 범위: 0 ~ 2^31-1 (class 3비트 + level 13비트 + hint 상위). 동기화: 불변. */

	uint32_t perc;
	/* [한국어] 이 항목이 "활성화"될 확률(0~100). 누적 CDF 추첨에 사용.
	 * 예: perc=20 → 무작위값 0~99 중 20 미만일 때 선택. perc=0 은 항목 무효.
	 * 설정자: init 에서 옵션 또는 bssplit 파싱값. 읽는 자: set_ioprio 추첨.
	 * 값 범위: 0~100. 동기화: 불변. */

	uint16_t clat_prio_index;
	/* [한국어] clat(완료 지연) 통계 분리용 우선순위 인덱스.
	 * stat.c가 우선순위별 lat 분포를 별도 버킷으로 누적할 때 이 인덱스로 배열 위치 결정.
	 * 설정자: init 에서 td->ts 의 clat_prio 배열을 할당하고 각 항목에 인덱스 부여.
	 * 읽는 자: set_ioprio 가 io_u->clat_prio_index = 이 값, getevents 완료 처리에서 사용.
	 * 값 범위: 0 ~ (할당된 clat_prio 개수 - 1). 동기화: 불변. */
};

/*
 * [한국어] "특정 블록 크기" 버킷의 cmdprio_prio 집합. BSSPLIT 모드 전용.
 * 예: 4K 블록에 RT-class 10% + BE-class-0 30% 조합을 표현하려면
 *      bs=4096, tot_perc=40, nr_prios=2, prios={RT 10%, BE0 30%} 가 된다.
 */
struct cmdprio_bsprio {
	uint64_t bs;
	/* [한국어] 대상 블록 크기(바이트). 사용자 bssplit 문자열의 왼쪽 토큰.
	 * 설정자: init 의 bssplit 파서가 단위 변환(4k→4096) 후 저장.
	 * 읽는 자: set_ioprio 가 io_u->buflen == bs 로 버킷 매칭.
	 * 값 범위: >0. 동기화: 불변. */

	uint32_t tot_perc;
	/* [한국어] prios[] 내 모든 perc 의 합(≤100). 합이 < 100 이면 나머지 확률은 기본(변경없음).
	 * 설정자: init 에서 누적. 읽는 자: set_ioprio 의 total-bound 검증 / 추첨 상한.
	 * 값 범위: 0~100. 동기화: 불변. */

	unsigned int nr_prios;
	/* [한국어] prios 배열 길이. 0 이면 해당 버킷은 비어있는 것(미사용). */

	struct cmdprio_prio *prios;
	/* [한국어] 우선순위 항목 동적 배열. malloc/free 수명은 fio_cmdprio_init/cleanup 에 귀속.
	 * 설정자: init. 읽는 자: set_ioprio 의 선형 탐색. 값 범위: nr_prios 만큼 유효.
	 * 동기화: 잡 스레드 1명 소유, 잡 시작 후 불변. */
};

/*
 * [한국어] 한 ddir (read 또는 write) 에 대한 모든 블록 크기 버킷 집합.
 * cmdprio.bsprio_desc[DDIR_READ]/[DDIR_WRITE] 로 ddir 별 독립 관리.
 */
struct cmdprio_bsprio_desc {
	struct cmdprio_bsprio *bsprios;
	/* [한국어] 블록 크기별 우선순위 버킷 배열. 길이 = nr_bsprios.
	 * 설정자: init (bssplit 파싱 후 오름차순 정렬). 읽는 자: set_ioprio.
	 * 값 범위: 각 원소는 고유 bs 값. 동기화: 불변. */

	unsigned int nr_bsprios;
	/* [한국어] bsprios 배열 길이. 0 이면 이 ddir 에 대한 BSSPLIT 설정 없음. */
};

/*
 * [한국어] 사용자가 CLI/잡파일에서 지정한 cmdprio 관련 옵션 원본.
 * 엔진별 옵션 구조체(예: libaio_options) 안에 내장(CMDPRIO_OPTIONS 매크로가 offsetof 로 접근).
 * 각 배열은 [DDIR_READ]/[DDIR_WRITE] 두 원소. 사용자 미지정 시 0 (기본) 유지.
 */
struct cmdprio_options {
	unsigned int percentage[CMDPRIO_RWDIR_CNT];
	/* [한국어] cmdprio_percentage=<R>,<W> — PERC 모드에서 각 ddir 의 고우선순위 적용 비율(%).
	 * 설정자: 옵션 파서. 읽는 자: fio_cmdprio_init (0이면 PERC 모드 진입 안 함).
	 * 값 범위: 0~100. 동기화: 불변. */

	unsigned int class[CMDPRIO_RWDIR_CNT];
	/* [한국어] cmdprio_class=<R>,<W> — ioprio 클래스 (1=RT, 2=BE, 3=IDLE).
	 * 설정자: 옵션 파서. 읽는 자: init 이 ioprio_value(class, level, hint) 로 prio 생성.
	 * 값 범위: IOPRIO_MIN_PRIO_CLASS+1 ~ IOPRIO_MAX_PRIO_CLASS (fio.h). 동기화: 불변. */

	unsigned int level[CMDPRIO_RWDIR_CNT];
	/* [한국어] cmdprio=<R>,<W> — ioprio 레벨 (0~7, 낮을수록 높음).
	 * 설정자: 옵션 파서. 읽는 자: init 의 ioprio 인코딩.
	 * 값 범위: IOPRIO_MIN_PRIO ~ IOPRIO_MAX_PRIO. 동기화: 불변. */

	unsigned int hint[CMDPRIO_RWDIR_CNT];
	/* [한국어] cmdprio_hint=<R>,<W> — 커널 6.4+ 의 IOPRIO_HINT_DEV_DURATION_LIMIT 등 힌트 비트.
	 * 설정자: 옵션 파서. 읽는 자: init 의 ioprio 인코딩.
	 * 값 범위: IOPRIO_MIN_PRIO_HINT ~ IOPRIO_MAX_PRIO_HINT. 동기화: 불변. */

	char *bssplit_str;
	/* [한국어] cmdprio_bssplit="4k/10:64k/50,...." — BSSPLIT 모드 입력 문자열.
	 * 설정자: FIO_OPT_STR_STORE 파서가 strdup 로 소유. 잡 시작 후 불변.
	 * 읽는 자: fio_cmdprio_init 이 토크나이즈하여 bsprio_desc 로 변환.
	 * 값 범위: 비-NULL(BSSPLIT 모드 시) 또는 NULL. 동기화: 불변. */
};

/*
 * [한국어] CMDPRIO_OPTIONS 매크로
 * libaio/io_uring/sg 엔진의 options[] 배열에 "..., CMDPRIO_OPTIONS(...), ..." 형태로
 * 삽입하여 cmdprio_percentage/class/hint/cmdprio/cmdprio_bssplit 다섯 엔트리를 일괄 등록.
 *
 * @opt_struct: 엔진별 옵션 구조체 타입(예: struct libaio_options). cmdprio_options 를 멤버로 포함.
 * @opt_group:  FIO_OPT_G_LIBAIO / FIO_OPT_G_IOURING / FIO_OPT_G_SG 등.
 *
 * FIO_HAVE_IOPRIO_CLASS 가 정의된 플랫폼(Linux)에서는 실제 옵션을, 아니면
 * FIO_OPT_UNSUPPORTED 로 대체하여 "해당 플랫폼에서 지원 안 됨" 에러를 제공.
 *
 * 옵션 목록(실제 지원 케이스):
 * - cmdprio_percentage: 고우선순위 I/O 비율 (0~100%, ddir 별 off1/off2).
 * - cmdprio_class:      I/O 우선순위 클래스 (RT=1/BE=2/IDLE=3).
 * - cmdprio_hint:       스케줄러 힌트(6.4+ 커널).
 * - cmdprio:            I/O 우선순위 레벨 (0~7).
 * - cmdprio_bssplit:    블록 크기별 비율 문자열 (FIO_OPT_STR_STORE).
 */
#ifdef FIO_HAVE_IOPRIO_CLASS
#define CMDPRIO_OPTIONS(opt_struct, opt_group)					\
	{									\
		.name	= "cmdprio_percentage",		/* [한국어] CLI/잡파일 키워드 */\
		.lname	= "high priority percentage",	/* [한국어] --showcmd-help 등 긴 라벨 */\
		.type	= FIO_OPT_INT,			/* [한국어] 정수 파서 사용 */\
		.off1	= offsetof(opt_struct,		/* [한국어] READ 방향 값 저장 오프셋 */\
				   cmdprio_options.percentage[DDIR_READ]),	\
		.off2	= offsetof(opt_struct,		/* [한국어] WRITE 방향 값 저장 오프셋 */\
				   cmdprio_options.percentage[DDIR_WRITE]),	\
		.minval	= 0,				/* [한국어] 하한 — 0 은 "사용 안 함" */\
		.maxval	= 100,				/* [한국어] 상한 — 100 은 "전부 고우선순위" */\
		.help	= "Send high priority I/O this percentage of the time",	\
		.category = FIO_OPT_C_ENGINE,		/* [한국어] 엔진 계열 분류 */\
		.group	= opt_group,			/* [한국어] 엔진별 서브그룹 */\
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
		.minval	= IOPRIO_MIN_PRIO_CLASS + 1, /* [한국어] 0(NONE)은 허용 안 함 */\
		.maxval	= IOPRIO_MAX_PRIO_CLASS,     /* [한국어] 상한 = IDLE(3) */\
		.interval = 1,                        /* [한국어] 정수 증분 1 */\
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
		.minval	= IOPRIO_MIN_PRIO_HINT, /* [한국어] 0 = hint 없음 */\
		.maxval	= IOPRIO_MAX_PRIO_HINT, /* [한국어] 커널 정의 상한 */\
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
		.minval	= IOPRIO_MIN_PRIO,   /* [한국어] 일반적으로 0(가장 높음) */\
		.maxval	= IOPRIO_MAX_PRIO,   /* [한국어] 일반적으로 7(가장 낮음) */\
		.interval = 1,							\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	},									\
	{									\
		.name   = "cmdprio_bssplit",					\
		.lname  = "Priority percentage block size split",		\
		.type   = FIO_OPT_STR_STORE, /* [한국어] 원본 문자열을 strdup 로 보관(파싱은 init) */\
		.off1   = offsetof(opt_struct, cmdprio_options.bssplit_str),	\
		.help   = "Set priority percentages for different block sizes",	\
		.category = FIO_OPT_C_ENGINE,					\
		.group	= opt_group,						\
	}
#else
/* [한국어] ioprio 미지원 플랫폼(예: Windows/Solaris) — UNSUPPORTED 자리표시 5엔트리.
 * 사용자가 이 옵션을 지정하면 옵션 파서가 명확한 에러 메시지와 함께 실패. */
#define CMDPRIO_OPTIONS(opt_struct, opt_group)					\
	{									\
		.name	= "cmdprio_percentage",					\
		.lname	= "high priority percentage",				\
		.type	= FIO_OPT_UNSUPPORTED,  /* [한국어] 파서가 즉시 거부 */\
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
 * [한국어] cmdprio 런타임 제어 구조체.
 * 엔진별 io_ops_data 안에 내장되어, init 시 구축되고 set_ioprio 에서 읽힌다.
 */
struct cmdprio {
	struct cmdprio_options *options;
	/* [한국어] 원본 사용자 옵션 포인터(td->eo 내부 cmdprio_options). init 이후 읽기 전용.
	 * 설정자: fio_cmdprio_init. 읽는 자: set_ioprio 가 bssplit_str/percentage 재확인.
	 * 값 범위: 비-NULL. 동기화: 불변. */

	struct cmdprio_prio perc_entry[CMDPRIO_RWDIR_CNT];
	/* [한국어] PERC 모드 전용 — ddir 별 단일 고우선순위 항목.
	 * 설정자: init 에서 class/level/hint/percentage 로 빌드.
	 * 읽는 자: set_ioprio 의 PERC 브랜치.
	 * 값 범위: perc=0 이면 해당 ddir 비활성. 동기화: 불변. */

	struct cmdprio_bsprio_desc bsprio_desc[CMDPRIO_RWDIR_CNT];
	/* [한국어] BSSPLIT 모드 전용 — ddir 별 블록 크기 버킷 집합.
	 * 설정자: init 에서 bssplit_str 파싱 결과. 읽는 자: set_ioprio 의 BSSPLIT 브랜치.
	 * 값 범위: nr_bsprios==0 이면 해당 ddir 비활성. 동기화: 불변. */

	unsigned int mode;
	/* [한국어] 현재 동작 모드 — CMDPRIO_MODE_{NONE,PERC,BSSPLIT} 중 하나.
	 * 설정자: init (options 를 분석해 확정, PERC 와 BSSPLIT 동시 사용 시 에러 반환).
	 * 읽는 자: set_ioprio 의 상단 스위치. 값 범위: enum 멤버. 동기화: 불변. */
};

/*
 * [한국어] fio_cmdprio_set_ioprio - 개별 io_u 에 확률적 ioprio 주입.
 *
 * @td:      실행 컨텍스트(RNG 상태 및 td->ts 통계 배열 소유).
 * @cmdprio: init 이 채워둔 런타임 상태.
 * @io_u:    대상 I/O. io_u->ddir 과 io_u->buflen 기반으로 확률 추첨.
 * @return:  true — ioprio 변경됨(clat_prio_index 도 갱신). false — 변경 없음(기본값 유지).
 *
 * 호출 컨텍스트: 엔진 queue() 내부에서 iocb/sqe 빌드 직전에 호출.
 * 호출 체인: td_io_queue() → 엔진 queue 콜백 → [이 함수] → (io_u 필드 갱신).
 */
bool fio_cmdprio_set_ioprio(struct thread_data *td, struct cmdprio *cmdprio,
			    struct io_u *io_u);

/*
 * [한국어] fio_cmdprio_cleanup - bsprio/prios 동적 배열 해제.
 *
 * init 에서 할당한 모든 버킷/항목을 free. 잡 종료 시(엔진 cleanup) 1회 호출.
 * 호출 체인: 엔진 cleanup 콜백 → [이 함수].
 */
void fio_cmdprio_cleanup(struct cmdprio *cmdprio);

/*
 * [한국어] fio_cmdprio_init - 옵션 → 런타임 구조 변환.
 *
 * @td:      잡 컨텍스트(clat_prio 통계 배열 할당 대상).
 * @cmdprio: 초기화할 대상.
 * @options: 사용자 옵션 원본(엔진별 옵션 구조체의 cmdprio_options 멤버).
 * @return:  0 성공, 음수 에러(PERC 와 BSSPLIT 동시 지정 등 배타 위반, 파싱 실패).
 *
 * 동작 단계:
 *  1) options 검사 → mode 결정(NONE/PERC/BSSPLIT).
 *  2) PERC 모드면 perc_entry[DDIR_*] 를 옵션 값으로 채움.
 *  3) BSSPLIT 모드면 bssplit_str 토크나이즈 → bsprio_desc 구축.
 *  4) 생성된 모든 cmdprio_prio 에 clat_prio_index 배정 + td->ts 의 clat_prio 배열 확장.
 *
 * 호출 체인: 엔진 init 콜백(예: fio_libaio_init) → [이 함수].
 */
int fio_cmdprio_init(struct thread_data *td, struct cmdprio *cmdprio,
		     struct cmdprio_options *options);

#endif
