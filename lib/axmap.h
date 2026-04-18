/*
 * [한국어 설명] 계층적 비트맵(axmap) 공개 API 헤더 (axmap.h)
 *
 * === 파일의 역할 ===
 * lib/axmap.c 가 구현하는 다단계 계층 비트맵(axmap) 의 공개 API 를 노출한다.
 * 내부 구조(각 레벨별 uint64_t 배열, level 수, map_size 등) 는 axmap.c 에만
 * 알려진 불투명(opaque) 타입이며, 이 헤더는 전방 선언만 둔다. axmap 은
 * "N 개의 비트 중 어느 것이 이미 세팅되었나" 를 O(log64 N) 에 탐색 가능하게
 * 하는 자료구조로, 다음 미설정 비트 찾기가 log64 레벨 상위로 올라가며
 * ffz() 를 반복 적용하는 방식으로 이뤄진다. fio 에서는 랜덤 I/O 잡이
 * "모든 블록을 정확히 한 번 방문" (= `norandommap=0` 기본) 을 보장할 때 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 io_u.c 오프셋 선택 루프의 핵심 의존. 랜덤 분포로 후보 오프셋을
 * 뽑은 후 axmap_isset 으로 이미 방문했는지 확인하고, 방문했다면 axmap_next_free
 * 로 다음 미설정 비트를 찾아 새 오프셋으로 사용한다. 범위 소진 판정
 * (모든 블록 방문 완료 → 잡 종료) 도 axmap 의 완전 소진으로 감지.
 * 호출 체인:
 *   io_u.c get_next_rand_offset
 *     → axmap_isset(td->file->io_axmap, bit)
 *       if 세팅 → axmap_next_free(axmap, bit)  (wrap-around 지원)
 *     → axmap_set(axmap, bit)  (선택 확정 시)
 *   filesetup.c : file 생성 시 axmap_new(nr_blocks), 잡 종료 시 axmap_free.
 *
 * === 타 모듈과의 연결 ===
 * - axmap.c : 구현. 레벨별 64비트 워드, 하위→상위 전파 방식 set/unset,
 *   ffz()/fls() 사용.
 * - lib/ffz.h, lib/fls.h : axmap.c 가 사용하는 비트 탐색 유틸.
 * - io_u.c / filesetup.c : 사용자 측.
 * - lib/types.h : bool 타입.
 * - <inttypes.h> : uint64_t.
 * 데이터 흐름: 잡 초기화 시 nr_blocks → axmap_new → 런타임에 set/isset/
 * next_free 반복 → 잡 종료 시 axmap_free.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct axmap (opaque) : 계층 비트맵 인스턴스. 정의는 axmap.c.
 * - axmap_new(nr_bits) : nr_bits 비트 용량 확보. NULL=실패.
 * - axmap_free(bm) : 해제.
 * - axmap_set(bm, bit) : 단일 비트 세팅. 상위 레벨로 전파됨.
 * - axmap_set_nr(bm, bit, n) : [bit..bit+n-1] 연속 비트 세팅. 실제 세팅된
 *   비트 수 반환(이미 세팅된 비트는 카운트 제외).
 * - axmap_isset(bm, bit) : 세팅 여부 조회.
 * - axmap_next_free(bm, bit) : bit 이후 첫 미설정 비트 위치 반환. wrap-around.
 * - axmap_reset(bm) : 전체 비트맵 0 리셋.
 */

#ifndef FIO_BITMAP_H
#define FIO_BITMAP_H
/* [한국어] 헤더 가드. 이름이 FIO_BITMAP_H 인 것은 초기 이름이 bitmap 이었고
 * 이후 axmap 으로 바뀌면서 가드만 원명을 유지하는 역사적 이유. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t 고정폭 타입. axmap 의 비트 번호/용량이
 * 64 비트. 대형 디바이스(수백 TiB ÷ 최소 블록) 를 다루기 위해 uint32_t 로는
 * 부족. */

#include "types.h"
/* [한국어] "types.h" : bool 폴백. axmap_isset 의 반환 타입. */

struct axmap;
/* [한국어] struct axmap - 계층 비트맵 인스턴스의 불투명 타입.
 * 설정자: axmap_new 만 생성, axmap.c 에 완전 정의 존재.
 * 읽는 자: axmap_{set,set_nr,isset,next_free,reset,free} 만 내부 접근.
 * 값 범위: 유효 포인터 또는 NULL(할당 실패).
 * 동기화: 인스턴스는 한 td 당 한 개이고 해당 잡 스레드만 접근하므로 락 불필요. */

struct axmap *axmap_new(uint64_t nr_bits);
/* [한국어] axmap_new - nr_bits 개의 비트를 담는 계층 비트맵을 할당.
 * level = ceil(log64(nr_bits)) 만큼의 배열을 단계별로 만들고 모두 0 으로
 * 초기화. NULL 반환 시 calloc 실패. 호출 컨텍스트: filesetup.c 가 잡 초기화
 * 시 한 번. */

void axmap_free(struct axmap *bm);
/* [한국어] axmap_free - 모든 레벨 배열과 인스턴스를 해제. NULL 전달 허용. */

void axmap_set(struct axmap *axmap, uint64_t bit_nr);
/* [한국어] axmap_set - 레벨 0 의 비트 bit_nr 을 세팅. 해당 64비트 워드가
 * 모두 1 이 되면 상위 레벨의 대응 비트를 전파 세팅하여, 다음 탐색 시 해당
 * 구간을 건너뛸 수 있게 한다. 재진입 안전(잡 스레드 단일 소유). */

unsigned int axmap_set_nr(struct axmap *axmap, uint64_t bit_nr, unsigned int nr_bits);
/* [한국어] axmap_set_nr - [bit_nr..bit_nr+nr_bits-1] 범위를 연속 세팅.
 * 이미 세팅된 비트는 중복 카운트하지 않고, "이번 호출로 새로 세팅된 비트 수"
 * 를 반환. 블록 크기가 랜덤맵 단위보다 큰 경우(bs > min_bs) 하나의 I/O 가
 * 여러 비트를 덮는 상황에 사용. */

bool axmap_isset(struct axmap *axmap, uint64_t bit_nr);
/* [한국어] axmap_isset - 비트가 세팅되어 있으면 true. 범위 밖이면 true 로
 * 폴백(범위 종료 의미). 잡 진행도를 상위 레벨에서도 점검하여 "전부
 * 세팅됨" 빠른 판정 가능. */

uint64_t axmap_next_free(struct axmap *axmap, uint64_t bit_nr);
/* [한국어] axmap_next_free - bit_nr 이후의 첫 미설정 비트 위치를 반환.
 * 범위 끝까지 없으면 wrap-around 하여 0 부터 다시 탐색. 탐색은 상위
 * 레벨부터 ffz() 로 빈 슬롯을 찾아 내려와 log64 깊이에서 종료. 완전
 * 소진 시 구현 정의 특수값 반환(axmap.c 참조). */

void axmap_reset(struct axmap *axmap);
/* [한국어] axmap_reset - 모든 레벨의 비트를 0 으로 되돌려 "처음 상태" 로
 * 복원. 잡 루프가 재시작되어야 할 때 사용(예: --loops 옵션). */

#endif
/* [한국어] FIO_BITMAP_H 가드 종료. */
