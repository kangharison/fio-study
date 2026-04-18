/*
 * [한국어 설명] MurmurHash3 해시 헤더 (murmur3.h)
 *
 * === 파일의 역할 ===
 * MurmurHash3 32비트 해시 함수의 공개 API 를 정의한다. Austin Appleby 가
 * 2011년 설계한 비암호학적 해시로, SMHasher 테스트 스위트를 통과하는 높은
 * 분산성과 단순한 연산(곱셈·회전·XOR)으로 해시 테이블·체크섬·데이터 검증
 * 등에 널리 쓰인다. 32비트 블록 단위 처리 + 잔여 1~3 바이트 tail 처리 +
 * fmix32 finalize(x ^= x>>16; x *= 0x85ebca6b; x ^= x>>13; x *= 0xc2b2ae35;
 * x ^= x>>16) 로 avalanche 성질을 보장한다.
 * 핵심 상수: c1 = 0xcc9e2d51, c2 = 0x1b873593, rotl 회전량 r1=15, r2=13,
 * 스프레드 상수 0xe6546b64.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인:
 *   verify.c 경로(해시 자체는 fio 가 verify 목적으로는 사용하지 않으나
 *   lib/bloom.c 의 블룸 필터 구성에 사용되며, dedupe 엔트리의 해시로도 활용)
 *     → murmurhash3(buf, len, seed) — 본 헤더 선언, 구현은 murmur3.c
 *   crc/test.c 벤치마크에서도 호출된다.
 *
 * === 타 모듈과의 연결 ===
 * - murmur3.c: 실제 구현(본체 32비트 블록 루프 + tail + fmix32 finalize).
 * - lib/bloom.c: N_HASHES=5 중 하나로 MurmurHash3 를 사용(중복 감지 확률 제어).
 * - crc/test.c: --crctest=murmur3 벤치마크 등록.
 * - verify.c: 직접 호출은 현재 없음(레퍼런스만, 역사적으로 verify=murmur 고려).
 * - inttypes.h: uint32_t 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - murmurhash3(key, len, seed): 단일 호출 32비트 해시 계산.
 *   seed 가 바뀌면 완전히 다른 해시 — 블룸 필터에서 여러 독립 해시 생성에 이용.
 */
#ifndef FIO_MURMUR3_H
/* [한국어] 헤더 중복 포함 방지 가드. */
#define FIO_MURMUR3_H

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: uint32_t 타입을 C99 표준으로 정의받아
 * 32비트 확정 연산을 보장하기 위함. MurmurHash3 는 32비트 곱셈과 회전이
 * 알고리즘 정의의 핵심이므로 플랫폼 의존적 int 크기 차이를 배제해야 한다. */

/*
 * [한국어]
 * murmurhash3 - MurmurHash3 32비트 해시 계산 (단일 호출)
 *
 * @key:  해시할 데이터 버퍼 포인터(void* — 바이트 스트림으로 처리). NULL 금지.
 * @len:  데이터 길이(바이트). 32비트 블록으로 분할 후 tail 1~3 바이트 별도 처리.
 * @seed: 해시 시드. 같은 데이터라도 seed 가 다르면 서로 독립인 해시가 나옴
 *        (블룸 필터 다중 해시, 해시 테이블 재해싱 등에 유용).
 * @return: 32비트 MurmurHash3 해시값. SMHasher Q.Score 10 의 분산성 보장.
 *
 * 동작 요약:
 *   1) 입력을 uint32_t 블록으로 분할, 각 블록에 c1 곱셈 → rotl(15) → c2 곱셈 적용.
 *   2) h1 ^= block 후 rotl(13), h1 = h1*5 + 0xe6546b64 스프레드.
 *   3) tail 1~3 바이트를 하나의 k1 으로 모아 동일 연산(rotl(15) 한 번만).
 *   4) h1 ^= len, fmix32(h1) avalanche 적용 후 반환.
 *
 * 호출 체인:
 *   lib/bloom.c::bloom_{add,check}() → murmurhash3() → 32비트 결과를 비트맵 인덱스로 매핑
 *   crc/test.c::do_test() → murmurhash3() → 256MB 처리 시간 측정
 *
 * 실행 컨텍스트: 잡 스레드(블룸 필터 조회/삽입) 또는 벤치마크 스레드.
 * 에러 경로: 없음 — 순수 계산 함수.
 */
uint32_t murmurhash3(const void *key, uint32_t len, uint32_t seed);

#endif
