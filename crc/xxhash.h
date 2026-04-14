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
 * [한국어 설명] xxHash 고속 해시 알고리즘 헤더 (xxhash.h)
 *
 * === 파일의 역할 ===
 * xxHash 32비트 해시 함수의 인터페이스를 정의한다.
 * Yann Collet이 설계한 비암호학적 해시로, RAM 속도에 가까운 처리량(5.4 GB/s)을
 * 달성하면서 SMHasher 테스트를 완벽히 통과하는 고품질 해시이다.
 * 단일 블록 처리(XXH32)와 스트리밍 처리(init/update/digest) 두 가지 API를 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 호출 체인: verify.c → XXH32() 또는 XXH32_init/update/digest
 *
 * === 타 모듈과의 연결 ===
 * - verify.c: verify=xxhash 옵션 시 호출
 * - xxhash.c: 구현
 * - crc/test.c: 벤치마크 테스트
 *
 * === 주요 구조체 ===
 * - XXH_state32_t: 스트리밍 해시 상태 (4개의 누적자 v1~v4, 임시 버퍼)
 *
 * === 주요 함수 요약 ===
 * - XXH32(): 단일 블록 해시 계산 (가장 빠름)
 * - XXH32_init/update/digest(): 스트리밍 해시 (대용량 데이터)
 * - XXH32_intermediateDigest(): 상태를 유지하면서 중간 해시 획득
 */
#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

#include <inttypes.h>

/*
 * [한국어] xxHash 스트리밍 상태 구조체
 * init()으로 초기화하고, update()로 데이터를 반복 투입한 뒤,
 * digest()로 최종 해시를 추출한다.
 */
struct XXH_state32_t
{
    uint64_t total_len;
    /* 지금까지 처리한 총 바이트 수 - 16바이트 이상이면 누적자 사용 */
    uint32_t seed;
    /* 해시 시드값 - 같은 데이터라도 시드가 다르면 다른 해시 생성 */
    uint32_t v1;
    uint32_t v2;
    uint32_t v3;
    uint32_t v4;
    /* 4개의 병렬 누적자(accumulator)
     * 각각 PRIME 상수로 곱셈/회전을 수행하여 입력을 혼합
     * 최종적으로 rotl+합산으로 병합하여 32비트 해시 생성 */
    int memsize;
    /* 임시 버퍼에 저장된 바이트 수 (0~15) */
    char memory[16];
    /* 16바이트 미만 잔여 데이터 임시 저장 버퍼 */
};

//****************************
// Type
//****************************
typedef enum { XXH_OK=0, XXH_ERROR } XXH_errorcode;



//****************************
// Simple Hash Functions
//****************************

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

void*         XXH32_init   (uint32_t seed);
XXH_errorcode XXH32_update (void* state, const void* input, int len);
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


int           XXH32_sizeofState(void);
XXH_errorcode XXH32_resetState(void* state, uint32_t seed);

#define       XXH32_SIZEOFSTATE 48
typedef struct { long long ll[(XXH32_SIZEOFSTATE+(sizeof(long long)-1))/sizeof(long long)]; } XXH32_stateSpace_t;
/*
These functions allow user application to make its own allocation for state.

XXH32_sizeofState() is used to know how much space must be allocated for the xxHash 32-bits state.
Note that the state must be aligned to access 'long long' fields. Memory must be allocated and referenced by a pointer.
This pointer must then be provided as 'state' into XXH32_resetState(), which initializes the state.

For static allocation purposes (such as allocation on stack, or freestanding systems without malloc()),
use the structure XXH32_stateSpace_t, which will ensure that memory space is large enough and correctly aligned to access 'long long' fields.
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
#define XXH32_feed   XXH32_update
#define XXH32_result XXH32_digest
#define XXH32_getIntermediateResult XXH32_intermediateDigest



#if defined (__cplusplus)
}
#endif
