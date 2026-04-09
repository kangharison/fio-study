#ifndef FIO_ZONE_DIST_H
#define FIO_ZONE_DIST_H
/*
 * [한국어] zone-dist.h - 존(zone) 분배 인덱스 헤더
 *
 * zone_split 옵션에 따른 I/O 접근 분배 인덱스 테이블의
 * 생성 및 해제 함수를 선언한다.
 */

void td_zone_gen_index(struct thread_data *td);   /* 존 인덱스 생성 */
void td_zone_free_index(struct thread_data *td);  /* 존 인덱스 해제 */

#endif
