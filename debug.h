/*
 * [한국어 설명] 디버그 출력 시스템 헤더 (debug.h)
 *
 * === 파일의 역할 ===
 * fio의 디버그 로깅 인프라를 정의한다. 디버그 카테고리별 비트마스크로 선택적
 * 디버그 출력을 활성화. --debug=io,verify 등으로 런타임에 카테고리 선택 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * debug.c와 짝을 이루는 헤더. fio.h를 통해 전체 코드에서 dprint() 사용 가능.
 *
 * === 타 모듈과의 연결 ===
 * - debug.c: __dprint() 함수 구현
 * - fio.h: 이 헤더를 포함하여 전역 접근 제공
 * - log.c: 실제 로그 출력 수행
 *
 * === 주요 함수/구조체 요약 ===
 * - dprint() 매크로: 비트마스크 기반 조건부 디버그 출력
 * - FD_PROCESS/FD_IO/FD_VERIFY 등: 디버그 카테고리 열거형
 * - fio_did_warn: 경고 중복 방지 비트마스크
 */
#ifndef FIO_DEBUG_H
#define FIO_DEBUG_H

#include "lib/types.h"  /* 기본 타입 정의 (bool 등) */

/* [한국어] 디버그 카테고리 열거형 - 각 카테고리는 비트 위치(shift)로 사용됨
 * fio_debug 비트마스크에서 (1 << FD_xxx)로 해당 카테고리 활성화 여부를 판별 */
enum {
	FD_PROCESS	= 0,   /* 프로세스/스레드 생성 및 관리 */
	FD_FILE,           /* 파일 열기/닫기/생성 */
	FD_IO,             /* I/O 제출 및 완료 */
	FD_MEM,            /* 메모리 할당/해제 */
	FD_BLKTRACE,       /* blktrace 처리 */
	FD_VERIFY,         /* 데이터 검증 */
	FD_RANDOM,         /* 난수 생성 */
	FD_PARSE,          /* 옵션 파싱 */
	FD_DISKUTIL,       /* 디스크 유틸리티 통계 */
	FD_JOB,            /* 작업(job) 관리 */
	FD_MUTEX,          /* 뮤텍스/세마포어 동기화 */
	FD_PROFILE,        /* 프로파일 */
	FD_TIME,           /* 시간 처리 */
	FD_NET,            /* 네트워크(서버/클라이언트 모드) */
	FD_RATE,           /* rate 제어 */
	FD_COMPRESS,       /* 데이터 압축 */
	FD_STEADYSTATE,    /* steady state 감지 */
	FD_HELPERTHREAD,   /* 헬퍼 스레드 */
	FD_ZBD,            /* Zoned Block Device */
	FD_SPRANDOM,       /* 작은 범위 랜덤 */
	FD_DEBUG_MAX,      /* 디버그 카테고리 최대값 (경계 검사용) */
};

/* [한국어] 디버그 작업 번호 및 경고 플래그 - 특정 작업에 대한 디버그 필터링 */
extern unsigned int fio_debug_jobno, *fio_debug_jobp, *fio_warned;

/* [한국어] 특정 경고가 이미 출력되었는지 확인
 * 같은 경고가 반복 출력되는 것을 방지하는 유틸리티 함수
 * 반환값: true = 이미 경고됨 (출력 불필요), false = 첫 경고 (플래그 설정 후 출력 필요) */
static inline bool fio_did_warn(unsigned int mask)
{
	if (*fio_warned & mask)
		return true;

	*fio_warned |= mask;
	return false;
}

/* [한국어] 경고 비트마스크 - fio_did_warn()에서 사용하는 경고 카테고리 */
enum {
	FIO_WARN_ROOT_FLUSH	= 1,   /* root 권한 없이 flush 시도 경고 */
	FIO_WARN_VERIFY_BUF	= 2,   /* 검증 버퍼 관련 경고 */
	FIO_WARN_ZONED_BUG	= 4,   /* Zoned 장치 버그 경고 */
	FIO_WARN_IOLOG_DROP	= 8,   /* I/O 로그 드롭 경고 */
	FIO_WARN_FADVISE	= 16,  /* fadvise 실패 경고 */
	FIO_WARN_BTRACE_ZERO	= 32,  /* blktrace 크기 0 경고 */
};

#ifdef FIO_INC_DEBUG
/* [한국어] 디버그 레벨 구조체 - 각 디버그 카테고리의 이름, 도움말, 비트 위치 */
struct debug_level {
	const char *name;        /* 카테고리 이름 (예: "process", "io") */
	const char *help;        /* 도움말 텍스트 */
	unsigned long shift;     /* 비트 위치 (fio_debug에서의 비트 인덱스) */
	unsigned int jobno;      /* 특정 작업 번호 필터 (0이면 모든 작업) */
};
extern const struct debug_level debug_levels[];

/* [한국어] 전역 디버그 비트마스크 - 활성화된 디버그 카테고리의 비트 조합 */
extern unsigned long fio_debug;

/* [한국어] 실제 디버그 출력 함수 - printf 형식의 포맷 지원 */
void __dprint(int type, const char *str, ...) __attribute__((format (printf, 2, 3)));

/* [한국어] 디버그 출력 매크로 - 해당 카테고리가 fio_debug에 활성화된 경우에만 출력
 * 비트마스크 검사를 먼저 수행하여 비활성 카테고리의 오버헤드를 최소화 */
#define dprint(type, str, args...)			\
	do {						\
		if (((1 << type) & fio_debug) == 0)	\
			break;				\
		__dprint((type), (str), ##args);	\
	} while (0)					\

#else

/* [한국어] FIO_INC_DEBUG 미정의 시 빈 함수 - 디버그 출력 비활성화 */
static inline void dprint(int type, const char *str, ...)
{
}
#endif

#endif
