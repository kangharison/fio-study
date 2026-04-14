/*
 * [한국어 설명] 스레드 에러 처리 헤더 (td_error.h)
 *
 * === 파일의 역할 ===
 * I/O 에러의 유형 분류 및 처리를 위한 열거형과 함수 선언을 제공한다.
 * continue_on_error/ignore_error 옵션에 의해 에러 처리 동작이 결정된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * td_error.c와 짝을 이루는 헤더. backend.c에서 에러 처리 시 참조.
 *
 * === 타 모듈과의 연결 ===
 * - td_error.c: 이 헤더의 함수 구현
 * - backend.c: I/O 에러 시 에러 분류/처리 호출
 *
 * === 주요 함수/구조체 요약 ===
 * - enum error_type_bit: 에러 유형 (ERROR_STR_READ/WRITE/VERIFY)
 * - td_non_fatal_error(): 비치명적 에러 판별
 * - update_error_count(): 에러 카운터 갱신
 */
#ifndef FIO_TD_ERROR_H
#define FIO_TD_ERROR_H

#include "io_ddir.h"  /* I/O 방향 정의 */

/*
 * What type of errors to continue on when continue_on_error is used,
 * and what type of errors to ignore when ignore_error is used.
 */
/* [한국어] 에러 유형 비트 위치 - 비트마스크 연산에 사용
 * continue_on_error 옵션에서 (1 << ERROR_TYPE_xxx_BIT)로 활성화 */
enum error_type_bit {
	ERROR_TYPE_READ_BIT = 0,    /* 읽기 에러 비트 위치 */
	ERROR_TYPE_WRITE_BIT = 1,   /* 쓰기 에러 비트 위치 */
	ERROR_TYPE_VERIFY_BIT = 2,  /* 검증 에러 비트 위치 */
	ERROR_TYPE_CNT = 3,         /* 에러 유형 총 개수 */
};

/* [한국어] 에러 유형 비트마스크 값 - 비트 위치를 실제 마스크 값으로 변환 */
enum error_type {
        ERROR_TYPE_NONE = 0,                            /* 에러 없음 */
        ERROR_TYPE_READ = 1 << ERROR_TYPE_READ_BIT,     /* 읽기 에러 (0x01) */
        ERROR_TYPE_WRITE = 1 << ERROR_TYPE_WRITE_BIT,   /* 쓰기 에러 (0x02) */
        ERROR_TYPE_VERIFY = 1 << ERROR_TYPE_VERIFY_BIT, /* 검증 에러 (0x04) */
        ERROR_TYPE_ANY = 0xffff,                        /* 모든 에러 유형 */
};

/* [한국어] I/O 방향과 에러 코드로 에러 유형 비트를 결정 */
enum error_type_bit td_error_type(enum fio_ddir ddir, int err);
/* [한국어] 에러가 비치명적(무시 가능)인지 판별 (1=비치명적, 0=치명적) */
int td_non_fatal_error(struct thread_data *td, enum error_type_bit etype,
		       int err);
/* [한국어] 에러 카운터 업데이트 및 첫 번째 에러 기록 */
void update_error_count(struct thread_data *td, int err);

#endif
