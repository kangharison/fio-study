/*
 * [한국어 설명] 중복 제거(deduplication) 버퍼 관리 헤더 (dedupe.h)
 *
 * === 파일의 역할 ===
 * 중복 제거 워킹셋 시드 초기화 함수를 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * dedupe.c와 짝을 이루는 헤더. init.c에서 시드 초기화 시 참조.
 *
 * === 타 모듈과의 연결 ===
 * - dedupe.c: 이 헤더의 함수 구현
 * - init.c: add_job()에서 호출
 *
 * === 주요 함수/구조체 요약 ===
 * - init_dedupe_working_set_seeds(): 작업별 시드 초기화
 * - init_global_dedupe_working_set_seeds(): 전역 시드 초기화
 */
#ifndef DEDUPE_H
#define DEDUPE_H

/* [한국어] 작업별 중복 제거 워킹셋 시드 초기화 (global_dedupe: 전역 중복 제거 여부) */
int init_dedupe_working_set_seeds(struct thread_data *td, bool global_dedupe);
/* [한국어] 전역 중복 제거 워킹셋 시드 초기화 - 모든 작업의 시드 초기화 후 호출 */
int init_global_dedupe_working_set_seeds(void);

#endif
