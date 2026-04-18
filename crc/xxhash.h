/*
   xxHash - Fast Hash algorithm
   Header File
   Copyright (C) 2012-2014, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - xxHash source repository : http://code.google.com/p/xxhash/
*/

/* Notice extracted from xxHash homepage :

xxHash is an extremely fast Hash algorithm, running at RAM speed limits.
It also successfully passes all tests from the SMHasher suite.

Comparison (single thread, Windows Seven 32 bits, using SMHasher on a Core 2 Duo @3GHz)

Name            Speed       Q.Score   Author
xxHash          5.4 GB/s     10
CrapWow         3.2 GB/s      2       Andrew
MumurHash 3a    2.7 GB/s     10       Austin Appleby
SpookyHash      2.0 GB/s     10       Bob Jenkins
SBox            1.4 GB/s      9       Bret Mulvey
Lookup3         1.2 GB/s      9       Bob Jenkins
SuperFastHash   1.2 GB/s      1       Paul Hsieh
CityHash64      1.05 GB/s    10       Pike & Alakuijala
FNV             0.55 GB/s     5       Fowler, Noll, Vo
CRC32           0.43 GB/s     9
MD5-32          0.33 GB/s    10       Ronald L. Rivest
SHA1-32         0.28 GB/s    10

Q.Score is a measure of quality of the hash function.
It depends on successfully passing SMHasher test set.
10 is a perfect score.
*/

/*
 * [한국어 설명] xxHash 고속 해시 알고리즘 공개 헤더 (xxhash.h)
 *
 * === 파일의 역할 ===
 * Yann Collet 가 2012 년 설계한 비암호학적 32비트 해시 xxHash(XXH32) 의 공개
 * API 를 정의한다. RAM 속도 한계(≈5 GiB/s)에 근접한 처리량을 보이면서도
 * SMHasher Q.Score 10 을 획득한 고품질 해시로, fio 에서는 verify=xxhash 모드로
 * 데이터 무결성 검증에 사용된다. 두 가지 사용법을 지원한다:
 *   (a) XXH32(ptr, len, seed) — 단일 블록 해시(간단 & 가장 빠름).
 *   (b) 스트리밍 API: XXH32_init → XXH32_update(여러 번) → XXH32_digest
 *       (대용량 데이터 또는 블록 단위 입력 상황).
 * 알고리즘 핵심 상수: PRIME32_1 = 2654435761, PRIME32_2 = 2246822519,
 * PRIME32_3 = 3266489917, PRIME32_4 = 668265263, PRIME32_5 = 374761393.
 * 16B 스트라이프 내 4개 병렬 누적자(v1~v4) 에 대해 "v = rotl13(v + data * P2) * P1"
 * 형태의 mix-round 반복, 최종 fmix(avalanche) 로 상위 비트까지 확산.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인(verify 경로):
 *   verify.c::fill_xxhash() / verify_io_u_xxhash()
 *     → XXH32_init(seed) → XXH32_update() → XXH32_digest()
 *       또는 단일 호출 XXH32(buf, len, seed)
 *   결과는 verify_header.v_xxhash 에 저장 후 재검증 시 비교.
 * 호출 체인(벤치마크):
 *   crc/test.c::do_test() → XXH32() → 256MB 반복 측정.
 *
 * === 타 모듈과의 연결 ===
 * - xxhash.c: 실제 구현(단일/스트리밍 공통 코드, avalanche finalize).
 * - verify.c: verify=xxhash 옵션 fill/verify 경로.
 * - crc/test.c: --crctest=xxhash 벤치마크 등록.
 * - inttypes.h: uint32_t/uint64_t 고정폭 타입 공급.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct XXH_state32_t: 스트리밍 해시 상태(총 바이트, seed, 4 누적자, 임시 버퍼).
 * - XXH32(): 단일 블록 해시(가장 빠름).
 * - XXH32_init/update/digest(): 스트리밍 해시 API(digest 가 메모리 해제까지 담당).
 * - XXH32_resetState()/sizeofState()/stateSpace_t: 정적 할당 버전 지원.
 * - XXH32_intermediateDigest(): 상태를 유지하며 중간 해시 얻기(digest 안 한 상태 연속).
 */
#pragma once
/* [한국어] #pragma once: 헤더 중복 포함 방지(GCC/Clang/MSVC 지원). 일반적인
 * #ifndef 가드 대신 원본 xxhash 가 이 관용구를 사용하므로 유지. */

#if defined (__cplusplus)
extern "C" {
/* [한국어] C++ 에서 포함 시 C 링커 네이밍(맹글링 없음) 유지 — 혼합 언어 프로젝트 대비. */
#endif

#include <inttypes.h>
/* [한국어] <inttypes.h> 포함 이유: uint32_t/uint64_t 고정폭 타입 공급.
 * xxHash 는 32비트 해시를 반환하고, 내부 total_len 은 64비트로 오버플로를 회피. */

/*
 * [한국어] xxHash 스트리밍 계산 컨텍스트
 * XXH32_init() → XXH32_update() → XXH32_digest() 순으로 사용.
 * 16B 미만 입력일 때는 단일 호출 XXH32() 가 본 구조체 없이 처리하는 빠른 경로 사용.
 */
struct XXH_state32_t
{
    uint64_t total_len;
    /* [한국어] 지금까지 투입한 총 바이트 수(64비트 — 2^64-1 까지 무제한).
     * 설정자: XXH32_update() 가 len 누적.
     * 읽는 자: XXH32_digest() 가 16 미만일 때 path A(tail-only), 이상이면 path B(accumulator 병합).
     * 값 범위: 0 ~ 2^64-1. 16B 스트라이프 누적 여부를 이 값으로 판정.
     * 동기화: 잡 스레드 단독 소유 — 락 불필요. */

    uint32_t seed;
    /* [한국어] 해시 시드 — XXH32_init(seed) 호출 시점에 고정.
     * 설정자: XXH32_init() / XXH32_resetState().
     * 읽는 자: 누적자 초기화(v1=seed+P1+P2, v2=seed+P2, v3=seed, v4=seed-P1) 및
     *         short-input path 에서 초기 해시 베이스로 사용.
     * 값 범위: 임의 32비트 값. 0 도 유효.
     * 동기화: 잡 스레드 단독 — 락 불필요. */

    uint32_t v1;
    uint32_t v2;
    uint32_t v3;
    uint32_t v4;
    /* [한국어] 4개의 병렬 누적자(accumulator).
     * 설정자: XXH32_init/resetState() 가 seed + PRIME 상수 조합으로 초기화.
     *        XXH32_update() 가 매 16B 스트라이프(stripe)마다 갱신 —
     *        v = rotl13(v + stripe * PRIME32_2) * PRIME32_1.
     * 읽는 자: digest 경로에서 rotl{1,7,12,18}(v1,v2,v3,v4) 로 합산해 h32 기본값 생성.
     * 값 범위: 32비트 부호 없는 정수.
     * 동기화: 잡 스레드 단독 — 락 불필요.
     * 병렬성 효과: 32비트 정수 연산 4개가 독립적으로 진행되어 현대 OoO CPU 의
     *            ILP(Instruction-Level Parallelism) 를 최대로 활용, RAM 대역폭
     *            한계에 근접한 처리량 달성. */

    int memsize;
    /* [한국어] 임시 버퍼 memory[] 에 저장된 잔여 바이트 수(0 ~ 15).
     * 설정자: XXH32_update() 가 16B 미달 잔여를 저장.
     * 읽는 자: 다음 update 호출이 기존 잔여 + 신규 입력을 합쳐 16B 가 되면 flush. */

    char memory[16];
    /* [한국어] 16B 스트라이프 미달 잔여 데이터 임시 저장 버퍼.
     * 크기는 정확히 스트라이프 크기 = 16바이트. */
};

//****************************
// Type
//****************************
/* [한국어] XXH 함수 반환 에러 코드.
 * XXH_OK(0): 성공. XXH_ERROR(1): 실패(현재 구현은 state NULL 체크 외엔 실패 경로 드묾). */
typedef enum { XXH_OK=0, XXH_ERROR } XXH_errorcode;



//****************************
// Simple Hash Functions
//****************************

/*
 * [한국어]
 * XXH32 - 단일 블록 xxHash-32 해시 계산 (가장 빠른 경로)
 *
 * @input: 해시할 데이터 시작 포인터. 유효한 len 바이트 영역을 가리켜야 함.
 * @len:   바이트 수(int — 최대 2^31-1). 대용량(2 GiB 초과) 은 스트리밍 API 사용.
 * @seed:  결과를 예측 가능한 방식으로 변형하는 시드. 같은 데이터/다른 seed → 독립 해시.
 * @return: 32비트 해시 결과. SMHasher 완전 통과.
 *
 * 성능: Core 2 Duo @3GHz 단일 스레드에서 5.4 GB/s (SMHasher 벤치).
 * 호출 체인: verify.c::fill_xxhash() / crc/test.c → XXH32() → 반환.
 * 실행 컨텍스트: 잡 스레드(verify) 또는 벤치마크 스레드. 에러: 없음(NULL 역참조는 호출자 책임).
 */
uint32_t XXH32 (const void* input, uint32_t len, uint32_t seed);

/*
XXH32() :
    Calculate the 32-bits hash of sequence of length "len" stored at memory address "input".
    The memory between input & input+len must be valid (allocated and read-accessible).
    "seed" can be used to alter the result predictably.
    This function successfully passes all SMHasher tests.
    Speed on Core 2 Duo @ 3 GHz (single thread, SMHasher benchmark) : 5.4 GB/s
    Note that "len" is type "int", which means it is limited to 2^31-1.
    If your data is larger, use the advanced functions below.
*/



//****************************
// Advanced Hash Functions
//****************************

/*
 * [한국어]
 * XXH32_init - 스트리밍 해시 상태 객체 할당 및 초기화
 * @seed: 해시 시드. 최초 호출 시 고정.
 * @return: 내부에서 malloc 으로 할당한 XXH_state32_t 포인터(불투명 void*).
 *          디폴트 구현은 malloc — digest/free 로 반드시 해제해야 누수 없음.
 * 호출 체인: verify.c::fill_xxhash() (데이터 분할 투입 시) → XXH32_init() → update 반복 → digest.
 */
void*         XXH32_init   (uint32_t seed);

/*
 * [한국어]
 * XXH32_update - 스트리밍 해시에 바이트 추가 투입
 * @state: XXH32_init / resetState 로 준비된 상태.
 * @input: 추가 입력 바이트.
 * @len:   바이트 수(int — 최대 2^31-1). 2 GiB 초과 단일 update 는 금지.
 * @return: XXH_OK / XXH_ERROR.
 * 동작: 16B 스트라이프 완성 시 v1~v4 갱신, 나머지는 memory[] 보관.
 */
XXH_errorcode XXH32_update (void* state, const void* input, int len);

/*
 * [한국어]
 * XXH32_digest - 스트리밍 해시 최종 확정 (상태 메모리도 해제)
 * @state: update 완료 후 상태 포인터.
 * @return: 32비트 해시 결과.
 * 주의: 본 함수는 state 메모리도 free 한다. 상태를 보존하면서 중간 해시만 얻고
 *      싶다면 XXH32_intermediateDigest 를 사용할 것.
 */
uint32_t XXH32_digest (void* state);

/*
These functions calculate the xxhash of an input provided in several small packets,
as opposed to an input provided as a single block.

It must be started with :
void* XXH32_init()
The function returns a pointer which holds the state of calculation.

This pointer must be provided as "void* state" parameter for XXH32_update().
XXH32_update() can be called as many times as necessary.
The user must provide a valid (allocated) input.
The function returns an error code, with 0 meaning OK, and any other value meaning there is an error.
Note that "len" is type "int", which means it is limited to 2^31-1.
If your data is larger, it is recommended to chunk your data into blocks
of size for example 2^30 (1GB) to avoid any "int" overflow issue.

Finally, you can end the calculation anytime, by using XXH32_digest().
This function returns the final 32-bits hash.
You must provide the same "void* state" parameter created by XXH32_init().
Memory will be freed by XXH32_digest().
*/


/*
 * [한국어]
 * XXH32_sizeofState - XXH_state32_t 의 sizeof 반환 (정적 할당 길이 계산용).
 * 호출자가 자체 힙/스택 버퍼를 준비해 XXH32_resetState 와 조합할 때 사용.
 */
int           XXH32_sizeofState(void);

/*
 * [한국어]
 * XXH32_resetState - 호출자 제공 버퍼에 상태 재초기화(할당 없이).
 * @state: 최소 XXH32_sizeofState() 바이트 이상의 정렬된 버퍼(long long 정렬 요구).
 * @seed:  새 시드.
 */
XXH_errorcode XXH32_resetState(void* state, uint32_t seed);

#define       XXH32_SIZEOFSTATE 48
/* [한국어] 정적 할당용 보장 크기 상수(48 바이트). 미래 확장 시 늘어날 수 있어
 * 이 값은 stateSpace_t 유니온의 비트 단위 정확 크기가 아닌 상한 용도. */

typedef struct { long long ll[(XXH32_SIZEOFSTATE+(sizeof(long long)-1))/sizeof(long long)]; } XXH32_stateSpace_t;
/* [한국어] 스택/정적 할당용 타입 — long long 정렬과 충분한 크기를 동시에 보장.
 * malloc 불가 환경(freestanding, 커널 모듈 등)에서 XXH32_stateSpace_t space;
 * 선언 후 XXH32_resetState(&space, seed) 로 사용. */
/*
These functions allow user application to make its own allocation for state.

XXH32_sizeofState() is used to know how much space must be allocated for the xxHash 32-bits state.
Note that the state must be aligned to access 'long long' fields. Memory must be allocated and referenced by a pointer.
This pointer must then be provided as 'state' into XXH32_resetState(), which initializes the state.

For static allocation purposes (such as allocation on stack, or freestanding systems without malloc()),
use the structure XXH32_stateSpace_t, which will ensure that memory space is large enough and correctly aligned to access 'long long' fields.
*/


/*
 * [한국어]
 * XXH32_intermediateDigest - 상태 보존 중간 해시 추출
 * @state: update 진행 중인 상태. digest 와 달리 메모리 해제하지 않음.
 * @return: 현재까지 투입된 데이터의 32비트 해시.
 * 용도: 로그 체크포인트마다 해시를 출력하면서 계속 데이터를 누적해야 할 때.
 * 해제 시에는 XXH32_digest() 를 최종 호출하거나 free() 로 직접 반환.
 */
uint32_t XXH32_intermediateDigest (void* state);
/*
This function does the same as XXH32_digest(), generating a 32-bit hash,
but preserve memory context.
This way, it becomes possible to generate intermediate hashes, and then continue feeding data with XXH32_update().
To free memory context, use XXH32_digest(), or free().
*/



//****************************
// Deprecated function names
//****************************
// The following translations are provided to ease code transition
// You are encouraged to no longer this function names
/* [한국어] 레거시 함수명과의 호환용 별칭 매크로 — 새 코드에서는 update/digest 사용 권장. */
#define XXH32_feed   XXH32_update
#define XXH32_result XXH32_digest
#define XXH32_getIntermediateResult XXH32_intermediateDigest



#if defined (__cplusplus)
}
/* [한국어] extern "C" 블록 종료. */
#endif
