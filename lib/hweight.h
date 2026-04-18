/*
 * [한국어 설명] 해밍 가중치(popcount) 공개 헤더 (hweight.h)
 *
 * === 파일의 역할 ===
 * lib/hweight.c 가 SWAR (SIMD Within A Register) 3 단계 비트 카운팅 알고리즘
 * 으로 구현한 "설정된 1 비트의 개수 반환" 함수 세 가지(hweight8/32/64) 를
 * 노출한다. 본 헤더는 구조체/매크로를 두지 않고 단 세 개의 extern 함수
 * 선언만 제공하는 순수 API 헤더이며, 입력 폭(8/32/64) 에 따라 오버로드
 * 없이 정적 디스패치되도록 세 개의 독립 함수로 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "비트마스크/플래그 진단" 계층에 속한다. verify.c 는 CRC 가
 * 불일치할 때 `hweight8(expected ^ actual)` 로 몇 비트가 뒤집혔는지 계산해
 * 메모리/미디어 에러의 비트-단위 특성을 진단하고, backend.c / options.c 는
 * CPU 어피니티 마스크에서 선택된 CPU 수를 세는 용도로 hweight64 를 사용한다.
 * cpumask_next() 등 배정 루프에서도 남은 CPU 수 검증에 쓰인다.
 * 호출 체인:
 *   verify.c log_verify_failure() → hweight8(a ^ b)
 *   options.c str_cpus_allowed_cb → fio_cpus_split → hweight64
 *
 * === 타 모듈과의 연결 ===
 * - hweight.c : 세 함수의 정의. 0x55.., 0x33.., 0x0f.. 마스크로 2비트→니블
 *   →바이트 순 SWAR 합산 후 마지막 곱셈으로 최상위 바이트에 합을 모은다.
 * - verify.c : 체크섬 불일치 시 bit-flip 개수 진단.
 * - options.c / cpumask 유틸 : 허용 CPU 카운트.
 * - <inttypes.h> (본 헤더가 포함) : uint8_t/uint32_t/uint64_t 고정폭 타입 공급.
 * 데이터 흐름: unsigned 정수 입력 → 1 비트 개수(0..N) 반환 → 호출자가
 * 진단/검증/카운팅 용도로 사용.
 *
 * === 주요 함수/구조체 요약 ===
 * - hweight8(w)  : uint8_t 의 1 비트 개수(0..8).
 * - hweight32(w) : uint32_t 의 1 비트 개수(0..32).
 * - hweight64(w) : uint64_t 의 1 비트 개수(0..64). 32 비트 타겟에서는 상/하위
 *   32 비트를 각각 hweight32 로 처리하는 분기가 hweight.c 에 있다.
 */
#ifndef FIO_HWEIGHT_H
#define FIO_HWEIGHT_H
/* [한국어] 헤더 가드. verify.c 와 options.c 양쪽에서 포함되어도 중복 선언
 * 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint8_t / uint32_t / uint64_t 등 고정폭 부호 없는
 * 정수 타입 정의를 공급. 아키텍처 간 portable 한 비트폭 보장을 위해 raw
 * unsigned char / unsigned long long 대신 이 타입들을 사용한다. */

unsigned int hweight8(uint8_t w);
/* [한국어] hweight8 - 8 비트 정수 w 의 설정된 비트 수(0..8) 반환.
 * 주 사용처: verify.c 의 log_verify_failure() — 읽은 바이트와 기대 바이트의
 * XOR 결과에 대해 hweight8 을 적용해 "몇 비트가 다른가" 를 보고한다.
 * 구현 컨텍스트: 실행 중 수십억 회 호출되지 않으므로 branch-free SWAR 알고
 * 리즘을 사용(룩업 테이블 없이 상수 마스크만 사용). */

unsigned int hweight32(uint32_t w);
/* [한국어] hweight32 - 32 비트 정수 w 의 설정된 비트 수(0..32) 반환.
 * SWAR: x = x - ((x>>1) & 0x55..); x = (x & 0x33..) + ((x>>2) & 0x33..);
 *        x = (x + (x>>4)) & 0x0f..; x = (x * 0x01010101) >> 24. */

unsigned int hweight64(uint64_t w);
/* [한국어] hweight64 - 64 비트 정수 w 의 설정된 비트 수(0..64) 반환.
 * 32 비트 타겟에서는 hweight.c 가 상/하위 32 비트 각각 hweight32 를 호출해
 * 합치는 분기를 취한다(long 이 32bit 인 ILP32 빌드 배려). */

#endif
/* [한국어] FIO_HWEIGHT_H 가드 종료. */
