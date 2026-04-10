/*
 * [한국어 설명] 계층적 비트맵(Axmap) 헤더 (axmap.h)
 *
 * === 파일의 역할 ===
 * 다단계 계층적 비트맵(axmap)의 공개 API를 정의한다.
 * 내부 구조체는 axmap.c에 숨겨져 있으며, 이 헤더는 불투명(opaque) 포인터로 접근한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct axmap: 불투명 포인터로 선언 (전방 선언)
 * - axmap_new/axmap_free: nr_bits 크기의 비트맵 생성/해제
 * - axmap_set/axmap_set_nr: 단일 또는 연속 비트 설정
 * - axmap_isset: 특정 비트의 설정 여부 확인
 * - axmap_next_free: 다음 미설정 비트를 탐색 (wrap-around 지원)
 * - axmap_reset: 전체 비트맵 초기화
 *
 * === fio에서의 사용 ===
 * io_u.c의 랜덤맵 관리에서 사용된다. 랜덤 I/O 워크로드 실행 시 각 블록의
 * 접근 여부를 추적하고, 모든 블록이 접근될 때까지 미사용 블록을 효율적으로 찾는다.
 */

#ifndef FIO_BITMAP_H
#define FIO_BITMAP_H

#include <inttypes.h>
#include "types.h"

struct axmap;
struct axmap *axmap_new(uint64_t nr_bits);
void axmap_free(struct axmap *bm);

void axmap_set(struct axmap *axmap, uint64_t bit_nr);
unsigned int axmap_set_nr(struct axmap *axmap, uint64_t bit_nr, unsigned int nr_bits);
bool axmap_isset(struct axmap *axmap, uint64_t bit_nr);
uint64_t axmap_next_free(struct axmap *axmap, uint64_t bit_nr);
void axmap_reset(struct axmap *axmap);

#endif
