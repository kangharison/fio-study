/*
 * [한국어 설명] 블룸 필터 공개 API 헤더 (bloom.h)
 *
 * === 파일의 역할 ===
 * lib/bloom.c 가 구현하는 확률적 집합 자료구조 "블룸 필터" 의 공개 API 를
 * 노출한다. 본 헤더는 struct bloom 을 불투명(opaque) 전방 선언으로만 두어
 * 호출자가 내부 필드(비트맵 포인터, 해시 함수 배열, 엔트리 수) 에 접근할 수
 * 없게 한다 — bloom.c 가 완전한 정의를 독점한다. 노출되는 것은 생성/해제/
 * "32비트 워드 배열 기반 세트-인-쿼리" 와 "문자열 기반 세트-인-쿼리" 두
 * 연산 조합뿐이다. 5 개 독립 해시(jhash, XXH32, murmur3, crc32c, fnv) 를
 * 사용하여 N_HASHES=5 기준 false-positive 율 (1-e^(-kn/m))^k 를 최소화한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 dedupe(중복 제거) 진단 파이프라인에 속한다. 대규모 I/O 트레이스를
 * 분석하거나 `--dedupe_mode=working_set` 옵션으로 중복률을 추정할 때, 이미
 * 본 블록을 봤는지 O(1) 에 가깝게 질의하여 메모리 비용을 줄인다. t/dedupe.c
 * 벤치마크 툴과 io_u 버퍼 중복 판정에 쓰인다.
 * 호출 체인:
 *   t/dedupe.c main → bloom_new(N) → 블록 반복 → bloom_string(b, data, len, add)
 *     → (첫 호출에서만) bloom_set(b, hash_words, nwords) → 기존 존재 여부 반환
 *
 * === 타 모듈과의 연결 ===
 * - bloom.c : 본 헤더의 불투명 타입을 완전 정의. 5 개 해시 함수, 비트맵
 *   할당(calloc), 비트 세팅 시 이전 상태를 AND 집계.
 * - t/dedupe.c : 유일한 외부 사용자(실용적으로).
 * - lib/types.h : bool 폴백 정의.
 * - <inttypes.h> : uint32_t/uint64_t.
 * 데이터 흐름: 블록 데이터 → bloom_string/bloom_set → 5 해시 계산 → 비트맵
 * 의 5 비트 위치를 모두 세팅 → 이미 5 비트가 다 켜져 있었으면 "있음(true)"
 * 반환, 아니면 "신규(false)" 반환 + 세팅 완료.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct bloom (opaque) : 내부 비트맵 + 엔트리 수. 정의는 bloom.c 소속.
 * - bloom_new(entries) : 예상 엔트리 수로 적정 비트맵 크기를 정해 할당.
 * - bloom_free(b) : 비트맵 해제.
 * - bloom_set(b, data, nwords) : 32비트 워드 배열을 키로 하여 set/check.
 * - bloom_string(b, data, len, add) : 바이트 문자열 키. add=false 이면 조회만.
 */
#ifndef FIO_BLOOM_H
#define FIO_BLOOM_H
/* [한국어] 헤더 가드. t/dedupe.c 및 미래 확장에서의 중복 포함 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint32_t/uint64_t 고정폭 정수. bloom_set 의 data
 * 타입과 bloom_new 의 entries 타입이 고정폭을 요구하여 필수. */

#include "../lib/types.h"
/* [한국어] "../lib/types.h" : bool 타입 폴백. 본 헤더의 bloom_set/bloom_string
 * 반환 타입이 bool 이므로 반드시 포함. 경로가 "../lib/" 로 되어 있는 것은
 * 과거에 이 헤더가 t/ 또는 engines/ 에서 포함되던 관습을 반영한다. */

struct bloom;
/* [한국어] struct bloom - 블룸 필터의 불투명 타입 전방 선언.
 * 설정자: bloom_new() 만 새 인스턴스를 할당(bloom.c 에 완전 정의).
 * 읽는 자: bloom_set/bloom_string/bloom_free 만 내부 필드에 접근 가능.
 * 값 범위: bloom_new 의 반환 포인터(NULL = 할당 실패) 또는 NULL.
 * 동기화: 인스턴스는 단일 스레드 소유(t/dedupe 는 단일 스레드). 다중 스레드
 *   공유 시 호출자가 외부 락을 걸어야 한다 — 라이브러리는 락을 제공하지 않음. */

struct bloom *bloom_new(uint64_t entries);
/* [한국어] bloom_new - 예상 엔트리 수 entries 에 맞춘 블룸 필터 생성.
 * 내부적으로 bitmap 비트 수를 entries * N_HASHES 에 비례해 정하고 calloc 으로
 * 0 초기화된 비트맵을 할당. NULL 반환 시 calloc 실패. */

void bloom_free(struct bloom *b);
/* [한국어] bloom_free - bloom_new 로 받은 인스턴스 해제. NULL 전달 금지. */

bool bloom_set(struct bloom *b, uint32_t *data, unsigned int nwords);
/* [한국어] bloom_set - 32 비트 워드 배열(data, nwords 개)을 키로 5 개 해시를
 * 계산하여 비트맵 5 비트를 세팅한다. 반환 true = 5 비트가 모두 이미 세팅
 * 되어 있었음(= 중복 가능성 높음, false-positive 허용), false = 신규 엔트리. */

bool bloom_string(struct bloom *b, const char *data, unsigned int len, bool);
/* [한국어] bloom_string - 바이트 문자열 키로 set/query. 네 번째 bool 은
 * "add_when_missing" 플래그: true 이면 없으면 추가하고 false 반환, 있으면
 * true 반환. false 이면 조회만 수행하고 비트맵은 수정하지 않음. 호출자가
 * 중복 제거 통계를 누적하거나 단순 조회만 할 때 구분에 사용. */

#endif
/* [한국어] FIO_BLOOM_H 가드 종료. */
