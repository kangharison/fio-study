#ifndef FIO_ZONE_DIST_H
#define FIO_ZONE_DIST_H
/*
 * [한국어] zone-dist.h - 존(zone) 분배 인덱스 헤더
 *
 * zone_split 옵션에 따른 I/O 접근 분배 인덱스 테이블의
 * 생성 및 해제 함수를 선언한다.
 
 * === 파일의 역할 ===
 * zone_split I/O 분배 인덱스 테이블의 생성/해제 함수를 선언.
 *
 * === 전체 아키텍처에서의 위치 ===
 * zone-dist.c와 짝을 이루는 헤더.
 *
 * === 타 모듈과의 연결 ===
 * - zone-dist.c: 이 헤더의 함수 구현
 *
 * === 주요 함수/구조체 요약 ===
 * - td_zone_gen_index(): 존 인덱스 생성
 * - td_zone_free_index(): 존 인덱스 해제
 */

void td_zone_gen_index(struct thread_data *td);   /* 존 인덱스 생성 */
void td_zone_free_index(struct thread_data *td);  /* 존 인덱스 해제 */

#endif
