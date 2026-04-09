/*
 * [한국어] dedupe.h - 중복 제거(deduplication) 버퍼 관리 헤더
 *
 * 중복 제거 워킹셋 시드 초기화 함수의 선언을 제공한다.
 * 중복 제거 모드에서 동일한 데이터 패턴을 생성하기 위한 시드 관리 인터페이스.
 */
#ifndef DEDUPE_H
#define DEDUPE_H

/* [한국어] 작업별 중복 제거 워킹셋 시드 초기화 (global_dedupe: 전역 중복 제거 여부) */
int init_dedupe_working_set_seeds(struct thread_data *td, bool global_dedupe);
/* [한국어] 전역 중복 제거 워킹셋 시드 초기화 - 모든 작업의 시드 초기화 후 호출 */
int init_global_dedupe_working_set_seeds(void);

#endif
