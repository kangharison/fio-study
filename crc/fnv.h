/*
 * [한국어 설명] FNV (Fowler-Noll-Vo) 해시 헤더 (fnv.h)
 *
 * === 파일의 역할 ===
 * FNV-1a 64비트 해시 함수의 공개 API 를 정의한다. FNV 는 1991년 Glenn Fowler,
 * Landon Noll, Phong Vo 가 설계한 비암호학적 해시로, 초기값(offset basis)과
 * 소수(FNV prime)를 곱셈·XOR 로 반복 적용한다. 구현은 단순하지만 분산성이
 * 우수하여 fio 에서는 verify=fnv 모드의 데이터 무결성 검증에 사용된다.
 * 알고리즘 수식(64비트): hash = offset; for each byte b: hash ^= b; hash *= PRIME
 * 상수: FNV_OFFSET64 = 0xCBF29CE484222325, FNV_PRIME64 = 0x100000001B3.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c::fill_fnv() / verify_io_u_fnv()
 *     → fnv(buf, len, seed)  // 본 헤더 선언, 구현은 fnv.c
 *   verify_header 의 v_fnv 필드 8바이트에 해시값이 저장/검증된다.
 *   crc/test.c 벤치마크에서도 직접 호출되어 256MB 처리 시간을 측정한다.
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=fnv 옵션 시 fill/verify 양쪽에서 호출(쓰기 후 검증).
 * - fnv.c: 실제 구현(64비트 워드 단위 최적화 + 잔여 바이트 빅엔디안 조립).
 * - crc/test.c: --crctest 벤치마크에서 fnv 를 struct test_type 으로 등록.
 * - inttypes.h: uint32_t, uint64_t 타입 정의 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - fnv(const void *buf, uint32_t len, uint64_t seed): 64비트 FNV-1a 해시 계산.
 *   seed 를 초기값으로 받아 스트리밍식 확장도 가능하나, fio 는 1회 호출만 사용.
 */
#ifndef FIO_FNV_H
/* [한국어] 헤더 중복 포함 방지 가드 — verify.c / fnv.c / test.c 동시 포함 대비. */
#define FIO_FNV_H

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: uint32_t(len), uint64_t(seed/반환값) 타입을
 * C99 표준 고정 폭 정수로 정의받기 위해. 이식성이 중요한 알고리즘 API 이므로
 * 플랫폼별 unsigned long 크기 차이(LP64 vs ILP32)를 피한다. */

/*
 * [한국어]
 * fnv - FNV-1a 64비트 해시 계산 (단발 호출용)
 *
 * @buf:  해시할 버퍼 시작 포인터(void* 로 캐스팅하여 바이트 스트림 취급).
 *        호출자 소유의 유효한 읽기 가능 영역이어야 하며, len 바이트를 읽는다.
 * @len:  해시할 바이트 수(uint32_t 로 최대 4 GiB-1). verify 블록은 대개 4KB~1MB.
 * @seed: 초기 해시값. 전형적으로 FNV_OFFSET64(0xCBF29CE484222325) 를 전달하되,
 *        스트리밍식 확장을 위해 이전 호출의 반환값을 재투입하는 것도 가능하다.
 * @return: 64비트 FNV-1a 해시. verify_header.v_fnv 에 저장되어 재검증 시 비교 대상.
 *
 * 호출 체인:
 *   verify.c::fill_fnv() → fnv() → (fnv.c 내부) 워드 단위 루프 → 반환
 *
 * 실행 컨텍스트: 잡 스레드(I/O 완료 후 verify 체크) 또는 벤치마크 스레드(test.c).
 * 에러 경로: 없음(순수 계산 함수, NULL 역참조 시 호출자 책임).
 */
uint64_t fnv(const void *, uint32_t, uint64_t);

#endif
