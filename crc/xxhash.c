/*
xxHash - Fast Hash algorithm
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


//**************************************
// Tuning parameters
//**************************************
// Unaligned memory access is automatically enabled for "common" CPU, such as x86.
// For others CPU, the compiler will be more cautious, and insert extra code to ensure aligned access is respected.
// If you know your target CPU supports unaligned memory access, you want to force this option manually to improve performance.
// You can also enable this parameter if you know your input data will always be aligned (boundaries of 4, for uint32_t).
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__i386) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
#  define XXH_USE_UNALIGNED_ACCESS 1
#endif

// XXH_ACCEPT_NULL_INPUT_POINTER :
// If the input pointer is a null pointer, xxHash default behavior is to trigger a memory access error, since it is a bad pointer.
// When this option is enabled, xxHash output for null input pointers will be the same as a null-length input.
// This option has a very small performance cost (only measurable on small inputs).
// By default, this option is disabled. To enable it, uncomment below define :
//#define XXH_ACCEPT_NULL_INPUT_POINTER 1

// XXH_FORCE_NATIVE_FORMAT :
// By default, xxHash library provides endian-independent Hash values, based on little-endian convention.
// Results are therefore identical for little-endian and big-endian CPU.
// This comes at a performance cost for big-endian CPU, since some swapping is required to emulate little-endian format.
// Should endian-independence be of no importance for your application, you may set the #define below to 1.
// It will improve speed for Big-endian CPU.
// This option has no impact on Little_Endian CPU.
#define XXH_FORCE_NATIVE_FORMAT 0


//**************************************
// Includes & Memory related functions
//**************************************
/*
 * [한국어 설명] xxHash 32비트 (XXH32) 고속 비-암호 해시 구현 (xxhash.c)
 *
 * === 파일의 역할 ===
 * Yann Collet 의 xxHash(32비트 변형 XXH32)를 구현한다. 16바이트 스트라이프 당
 * 4개의 독립 누적자(v1~v4)를 병렬로 갱신하는 구조로, 각 누적자는 4바이트 워드를
 * 리틀엔디안으로 읽어 "v = rotl32(v + w*PRIME32_2, 13) * PRIME32_1" 업데이트를
 * 수행한다. 16B 에 못 미치는 잔여는 4B/1B 단위 꼬리 처리, 마지막은 avalanche
 * 단계(x^= x>>15; x*=P2; x^=x>>13; x*=P3; x^=x>>16) 로 비트를 섞는다.
 * PRIME32_1..5 는 작은 수의 곱셈으로 좋은 분산을 내도록 선택된 32비트 소수.
 * 엔디안 독립적(내부에서 필요 시 XXH_swap32)·비정렬 접근 가능.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 의 VERIFY_XXHASH 경로와 crc/test.c 벤치마크 대상.
 * 원샷 API: XXH32(buf, len, seed) — 상태 malloc 없이 직행.
 * 스트리밍 API: XXH32_init → XXH32_update*n → XXH32_digest (digest 가 free 포함).
 * 호출 체인:
 *   verify.c::fill_xxhash/verify_io_u_xxhash → XXH32 또는 init/update/digest
 *   crc/test.c::t_xxhash → XXH32_init/update/digest
 *
 * === 타 모듈과의 연결 ===
 * - xxhash.h: XXH32 프로토타입과 XXH_state32_t 구조체(v1..v4/seed/total_len/memsize/memory[16]) 선언.
 * - stdlib.h: malloc/free — 스트리밍 상태 구조체 동적 할당.
 * - string.h: memcpy — 부분 버퍼 카피.
 * - compiler.h 없음 — 매크로는 내부 정의.
 * 동기화: 상태는 호출자가 소유 — 병렬 호출자 간 락 불요. PRIME 상수·테이블 없음.
 *
 * === 주요 함수 요약 ===
 * - XXH_rotl32(x,r): 32비트 좌회전 매크로.
 * - XXH_swap32(x): 32비트 바이트스왑(컴파일러 빌트인 또는 수동).
 * - XXH_readLE32_align/XXH_readLE32: 엔디안/정렬 옵션으로 32비트 워드 읽기.
 * - XXH32_endian_align: 원샷 해시 코어(엔디안/정렬 파라미터).
 * - XXH32: 엔디안 자동감지 원샷 공용 진입.
 * - XXH32_sizeofState: struct 크기 정적 검증.
 * - XXH32_resetState: 기존 상태 재사용 초기화.
 * - XXH32_init: malloc + resetState 로 새 상태 반환.
 * - XXH32_update_endian/XXH32_update: 스트리밍 업데이트.
 * - XXH32_intermediateDigest_endian/XXH32_intermediateDigest: 중간 다이제스트 추출.
 * - XXH32_digest: 최종 다이제스트 + free.
 */
#include "xxhash.h"
/* [한국어] xxhash.h: XXH_state32_t 구조체·XXH32_* 공용 API 선언·XXH32_SIZEOFSTATE 크기 상수. */
#include <stdlib.h>
/* [한국어] malloc/free: 스트리밍 상태(sizeof(XXH_state32_t)) 동적 할당·해제. */
#include <string.h>
/* [한국어] memcpy: 부분 버퍼 카피. */


/* [한국어] GCC 에서 비정렬 접근 미지원 타깃이면 struct 에 packed 속성 적용 —
 * 이를 통해 __attribute__((packed)) wrapper 필드의 접근이 "바이트별" 시퀀스로
 * 컴파일러에 의해 자동 생성된다. XXH_USE_UNALIGNED_ACCESS 가 정의되면 그냥 생략. */
#if defined(__GNUC__)  && !defined(XXH_USE_UNALIGNED_ACCESS)
#  define _PACKED __attribute__ ((packed))
#else
#  define _PACKED
#endif

/* [한국어] 비-GCC 컴파일러에서 #pragma pack 으로 1바이트 정렬 강제. IBM XL / MSVC 분기. */
#if !defined(XXH_USE_UNALIGNED_ACCESS) && !defined(__GNUC__)
#  ifdef __IBMC__
#    pragma pack(1)
#  else
#    pragma pack(push, 1)
#  endif
#endif

/* [한국어] uint32_t 한 개를 감싼 packed 구조체 — "엔디안/정렬 안전"한 32비트 로드 trick.
 * 필드 v 가 struct 내부에 들어있어 컴파일러가 struct 단위 정렬을 무시하고 바이트 로드를 낸다. */
typedef struct _uint32_t_S { uint32_t v; } _PACKED uint32_t_S;

#if !defined(XXH_USE_UNALIGNED_ACCESS) && !defined(__GNUC__)
#  pragma pack(pop)
#endif

/* [한국어] A32(x) — 주소 x 를 uint32_t_S* 로 재해석해 내부 v 를 읽음(비정렬 안전 로드).
 * 정렬 지원 타깃(x86 등)에서는 struct 가 그냥 uint32_t 라 *x 와 동등. */
#define A32(x) (((uint32_t_S *)(x))->v)


//***************************************
// Compiler-specific Functions and Macros
//***************************************
/* [한국어] GCC 의 major.minor 를 한 정수로 인코딩해 버전 비교 — 4.3 이상에서 builtin bswap 지원. */
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

// Note : although _rotl exists for minGW (GCC under windows), performance seems poor
/* [한국어] 32비트 좌회전 매크로. MSVC 에서는 _rotl 인트린직, 기타는 C 표현식. */
#if defined(_MSC_VER)
#  define XXH_rotl32(x,r) _rotl(x,r)
#else
#  define XXH_rotl32(x,r) ((x << r) | (x >> (32 - r)))
#endif

/* [한국어] 32비트 byte-swap — 빅엔디안 CPU 에서 리틀엔디안 데이터 처리 시 필요.
 * MSVC: _byteswap_ulong, GCC>=4.3: __builtin_bswap32, 그 외: 수동 마스크+시프트. */
#if defined(_MSC_VER)     // Visual Studio
#  define XXH_swap32 _byteswap_ulong
#elif GCC_VERSION >= 403
#  define XXH_swap32 __builtin_bswap32
#else
static inline uint32_t XXH_swap32 (uint32_t x)
{
    /* [한국어] 수동 bswap — 최상위 바이트를 최하위로, 나머지도 거울 반사. */
    return  ((x << 24) & 0xff000000 ) |
        ((x <<  8) & 0x00ff0000 ) |
        ((x >>  8) & 0x0000ff00 ) |
        ((x >> 24) & 0x000000ff );
}
#endif


//**************************************
// Constants
//**************************************
/* [한국어] xxHash32 의 5개 소수 상수. 작은 정수라 곱셈 후 분산이 뛰어나도록 선택.
 * PRIME32_1,2 는 16바이트 스트라이프 처리에, 3,4 는 4바이트 꼬리, 5 는 1바이트 꼬리에 사용. */
#define PRIME32_1   2654435761U
#define PRIME32_2   2246822519U
#define PRIME32_3   3266489917U
#define PRIME32_4    668265263U
#define PRIME32_5    374761393U


//**************************************
// Architecture Macros
//**************************************
/* [한국어] 엔디안 열거자 — XXH 코어 함수가 둘을 동일 코드로 처리하게 하는 파라미터. */
typedef enum { XXH_bigEndian=0, XXH_littleEndian=1 } XXH_endianess;
#ifndef XXH_CPU_LITTLE_ENDIAN   // It is possible to define XXH_CPU_LITTLE_ENDIAN externally, for example using a compiler switch
    /* [한국어] 정적 int 변수 one=1 의 최하위 바이트를 char* 로 읽어 엔디안 감지.
     * 리틀엔디안이면 그 바이트가 1, 빅엔디안이면 0 — 매크로로 런타임 한 번 평가. */
    static const int one = 1;
#   define XXH_CPU_LITTLE_ENDIAN   (*(char*)(&one))
#endif


//**************************************
// Macros
//**************************************
/* [한국어] 정적 검증 매크로 — c 가 0 이면 "1/0" 으로 컴파일 에러 유발(변수 선언 뒤에만 사용).
 * C11 이전 static_assert 우회. */
#define XXH_STATIC_ASSERT(c)   { enum { XXH_static_assert = 1/(!!(c)) }; }    // use only *after* variable declarations


//****************************
// Memory reads
//****************************
/* [한국어] 정렬 옵션 열거자 — aligned 경로는 *ptr 직접, unaligned 는 A32() 매크로 사용. */
typedef enum { XXH_aligned, XXH_unaligned } XXH_alignment;

/*
 * [한국어]
 * XXH_readLE32_align - ptr 에서 32비트 워드를 리틀엔디안 순서로 읽기
 *
 * @ptr:    읽을 주소.
 * @endian: 호출자 CPU 의 실제 엔디안(리틀이면 변환 없음, 빅이면 swap32).
 * @align:  unaligned 이면 A32() 로 안전 로드, aligned 이면 *ptr 직접.
 * @return: 리틀엔디안 정순으로 해석된 32비트 값.
 */
static uint32_t XXH_readLE32_align(const uint32_t* ptr, XXH_endianess endian, XXH_alignment align)
{
    /* [한국어] 비정렬 접근 경로: packed struct 래퍼 사용. */
    if (align==XXH_unaligned)
        return endian==XXH_littleEndian ? A32(ptr) : XXH_swap32(A32(ptr));
    /* [한국어] 정렬 접근 경로: 직접 dereference. 빅엔디안 CPU 는 swap 필요. */
    else
        return endian==XXH_littleEndian ? *ptr : XXH_swap32(*ptr);
}

/*
 * [한국어] XXH_readLE32 - unaligned 경로 고정 래퍼. 입력 포인터 정렬 미보장 경로에서 사용. */
static uint32_t XXH_readLE32(const uint32_t* ptr, XXH_endianess endian) { return XXH_readLE32_align(ptr, endian, XXH_unaligned); }


//****************************
// Simple Hash Functions
//****************************
/*
 * [한국어]
 * XXH32_endian_align - XXH32 원샷 해시 코어(엔디안/정렬 파라미터화)
 *
 * @input:  버퍼 시작.
 * @len:    바이트 길이.
 * @seed:   해시 시드.
 * @endian: 런타임 감지된 CPU 엔디안.
 * @align:  포인터 정렬 상태(aligned/unaligned).
 * @return: 32비트 해시.
 *
 * 동작: 16B 스트라이프 루프 → 16B 미만이면 seed+PRIME32_5 로 초기화 → 꼬리 4B·1B 처리
 *      → avalanche 단계 적용 → 반환. 모든 분기를 한 함수에 두어 빌드 시 4가지
 *      (엔디안 × 정렬) 특화 인스턴스가 XXH32 에서 호출된다.
 *
 * 호출 체인: XXH32 → [XXH32_endian_align]
 */
static uint32_t XXH32_endian_align(const void* input, int len, uint32_t seed, XXH_endianess endian, XXH_alignment align)
{
    /* [한국어] 바이트 포인터(꼬리 및 증가 연산용). */
    const uint8_t *p = (const uint8_t *)input;
    /* [한국어] 버퍼 종료 주소(p+len). */
    const uint8_t * const bEnd = p + len;
    /* [한국어] 누적 해시. */
    uint32_t h32;

#ifdef XXH_ACCEPT_NULL_INPUT_POINTER
    /* [한국어] NULL 입력 수용 옵션: len=0 으로 축소하고 p 를 더미(정렬된 비-NULL) 로 변경. */
    if (p==NULL) { len=0; p=(const uint8_t *)(size_t)16; }
#endif

    /* [한국어] 16B 이상이면 4-way 누적자 사용하는 주 루프. */
    if (len>=16)
    {
        /* [한국어] 16B 스트라이프 루프 한계선. */
        const uint8_t * const limit = bEnd - 16;
        /* [한국어] 누적자 4개 초기화 — PRIME 상수로 seed 에서 분화. */
        uint32_t v1 = seed + PRIME32_1 + PRIME32_2;
        uint32_t v2 = seed + PRIME32_2;
        uint32_t v3 = seed + 0;
        uint32_t v4 = seed - PRIME32_1;

        /* [한국어] 매 반복에서 16B(=4 × 4B) 스트라이프 처리.
         * 각 누적자 v_k 업데이트: v += word * P2; v = rotl32(v, 13); v *= P1. */
        do
        {
            v1 += XXH_readLE32_align((const uint32_t*)p, endian, align) * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1; p+=4;
            v2 += XXH_readLE32_align((const uint32_t*)p, endian, align) * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1; p+=4;
            v3 += XXH_readLE32_align((const uint32_t*)p, endian, align) * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1; p+=4;
            v4 += XXH_readLE32_align((const uint32_t*)p, endian, align) * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1; p+=4;
        } while (p<=limit);

        /* [한국어] 4개 누적자를 서로 다른 회전값(1/7/12/18)으로 합쳐 단일 h32 생성. */
        h32 = XXH_rotl32(v1, 1) + XXH_rotl32(v2, 7) + XXH_rotl32(v3, 12) + XXH_rotl32(v4, 18);
    }
    else
    {
        /* [한국어] 16B 미만 입력: 작은 버퍼 초기화 상수. */
        h32  = seed + PRIME32_5;
    }

    /* [한국어] 길이를 XOR/덧셈에 섞어 길이 구별성 확보. */
    h32 += (uint32_t) len;

    /* [한국어] 4B 단위 꼬리 처리(p 가 끝에서 4B 이내가 될 때까지). */
    while (p<=bEnd-4)
    {
        /* [한국어] word*P3 을 누적, rotl17*P4 로 확산. */
        h32 += XXH_readLE32_align((const uint32_t*)p, endian, align) * PRIME32_3;
        h32  = XXH_rotl32(h32, 17) * PRIME32_4 ;
        p+=4;
    }

    /* [한국어] 남은 <4B 1바이트씩 꼬리 처리. */
    while (p<bEnd)
    {
        /* [한국어] byte*P5 누적, rotl11*P1 확산. */
        h32 += (*p) * PRIME32_5;
        h32 = XXH_rotl32(h32, 11) * PRIME32_1 ;
        p++;
    }

    /* [한국어] avalanche(finalization): 누적값의 상·하 비트를 섞어 입력 민감도 극대화. */
    h32 ^= h32 >> 15;
    h32 *= PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= PRIME32_3;
    h32 ^= h32 >> 16;

    /* [한국어] 최종 해시 반환. */
    return h32;
}


/*
 * [한국어]
 * XXH32 - 원샷 해시 공용 진입점(엔디안·정렬 자동 감지)
 *
 * @input: 버퍼. @len: 바이트 길이. @seed: 시드.
 * @return: 32비트 해시.
 *
 * 동작: CPU 엔디안 감지 → (UNALIGNED 빌드가 아니고 ptr 이 비정렬이면) aligned 호출,
 *       그렇지 않으면 unaligned 호출. 두 분기 모두 XXH32_endian_align 에 위임.
 *
 * 호출 체인: verify.c / crc/test.c → [XXH32] → XXH32_endian_align
 */
uint32_t XXH32(const void* input, uint32_t len, uint32_t seed)
{
#if 0
    // Simple version, good for code maintenance, but unfortunately slow for small inputs
    void* state = XXH32_init(seed);
    XXH32_update(state, input, len);
    return XXH32_digest(state);
#else
    /* [한국어] 런타임 엔디안 감지 — XXH_CPU_LITTLE_ENDIAN 매크로는 char 캐스팅으로 1 또는 0. */
    XXH_endianess endian_detected = (XXH_endianess)XXH_CPU_LITTLE_ENDIAN;

#  if !defined(XXH_USE_UNALIGNED_ACCESS)
    /* [한국어] 비정렬 접근 미지원 타깃: ptr 이 정렬되어 있으면 aligned 특화 경로 선택. */
    if ((((size_t)input) & 3))   // Input is aligned, let's leverage the speed advantage
    {
        if ((endian_detected==XXH_littleEndian) || XXH_FORCE_NATIVE_FORMAT)
            return XXH32_endian_align(input, len, seed, XXH_littleEndian, XXH_aligned);
        else
            return XXH32_endian_align(input, len, seed, XXH_bigEndian, XXH_aligned);
    }
#  endif

    /* [한국어] 기본 경로: unaligned 로 안전하게 호출(x86 등은 추가 비용 거의 없음). */
    if ((endian_detected==XXH_littleEndian) || XXH_FORCE_NATIVE_FORMAT)
        return XXH32_endian_align(input, len, seed, XXH_littleEndian, XXH_unaligned);
    else
        return XXH32_endian_align(input, len, seed, XXH_bigEndian, XXH_unaligned);
#endif
}


//****************************
// Advanced Hash Functions
//****************************

/*
 * [한국어]
 * XXH32_sizeofState - XXH_state32_t 크기 반환(컴파일타임 정적 검증 포함)
 *
 * XXH32_SIZEOFSTATE(헤더 상수) 가 실제 struct 크기 이상인지 검증 — 아니면 컴파일 에러.
 * 외부에서 상태 버퍼를 미리 할당하고 싶을 때 안전 최소 크기 제공.
 */
int XXH32_sizeofState(void)
{
    XXH_STATIC_ASSERT(XXH32_SIZEOFSTATE >= sizeof(struct XXH_state32_t));   // A compilation error here means XXH32_SIZEOFSTATE is not large enough
    return sizeof(struct XXH_state32_t);
}


/*
 * [한국어]
 * XXH32_resetState - 기존 상태 버퍼를 seed 로 재초기화(재사용 루프용)
 *
 * @state_in: 할당된 상태 버퍼. @seed: 새 시드. @return: XXH_OK.
 */
XXH_errorcode XXH32_resetState(void* state_in, uint32_t seed)
{
    /* [한국어] void* 를 구조체 포인터로 재해석. */
    struct XXH_state32_t * state = (struct XXH_state32_t *) state_in;
    /* [한국어] seed 저장(작은 버퍼 finalize 에서 사용). */
    state->seed = seed;
    /* [한국어] 4 누적자 seed 에서 분화 초기화. */
    state->v1 = seed + PRIME32_1 + PRIME32_2;
    state->v2 = seed + PRIME32_2;
    state->v3 = seed + 0;
    state->v4 = seed - PRIME32_1;
    /* [한국어] 총 길이·부분 버퍼 크기 0. */
    state->total_len = 0;
    state->memsize = 0;
    return XXH_OK;
}


/*
 * [한국어]
 * XXH32_init - 새 상태 구조체 malloc + seed 초기화
 *
 * @seed: 해시 시드. @return: 불투명 핸들 — digest 가 free 책임.
 */
void* XXH32_init (uint32_t seed)
{
    /* [한국어] 상태 버퍼 malloc. */
    void *state = malloc (sizeof(struct XXH_state32_t));
    /* [한국어] seed 로 초기화. */
    XXH32_resetState(state, seed);
    /* [한국어] 핸들 반환. */
    return state;
}


/*
 * [한국어]
 * XXH32_update_endian - 스트리밍 업데이트 코어(엔디안 파라미터)
 *
 * 동작: 기존 memory 잔여 채워 한 16B 블록 소화 → 16B 이상 본체 루프 → <16B 꼬리는 memory 에 저장.
 *
 * 호출 체인: XXH32_update → [XXH32_update_endian]
 */
static XXH_errorcode XXH32_update_endian (void* state_in, const void* input, int len, XXH_endianess endian)
{
    struct XXH_state32_t * state = (struct XXH_state32_t *) state_in;
    /* [한국어] 입력 포인터(바이트 단위)와 끝 주소. */
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t * const bEnd = p + len;

#ifdef XXH_ACCEPT_NULL_INPUT_POINTER
    /* [한국어] NULL 입력 거부(빌드 옵션). */
    if (input==NULL) return XXH_ERROR;
#endif

    /* [한국어] 총 누적 길이 갱신. */
    state->total_len += len;

    /* [한국어] 부분 버퍼에 아직 16B 미만이 될 것이면 누적만 하고 반환. */
    if (state->memsize + len < 16)   // fill in tmp buffer
    {
        memcpy(state->memory + state->memsize, input, len);
        state->memsize +=  len;
        return XXH_OK;
    }

    /* [한국어] 부분 버퍼 잔여 소진 — input 에서 (16-memsize) 바이트 보충해 16B 완성. */
    if (state->memsize)   // some data left from previous update
    {
        memcpy(state->memory + state->memsize, input, 16-state->memsize);
        {
            /* [한국어] 완성된 16B 를 4 × 4B 로 읽어 4 누적자 업데이트. */
            const uint32_t* p32 = (const uint32_t*)state->memory;
            state->v1 += XXH_readLE32(p32, endian) * PRIME32_2; state->v1 = XXH_rotl32(state->v1, 13); state->v1 *= PRIME32_1; p32++;
            state->v2 += XXH_readLE32(p32, endian) * PRIME32_2; state->v2 = XXH_rotl32(state->v2, 13); state->v2 *= PRIME32_1; p32++;
            state->v3 += XXH_readLE32(p32, endian) * PRIME32_2; state->v3 = XXH_rotl32(state->v3, 13); state->v3 *= PRIME32_1; p32++;
            state->v4 += XXH_readLE32(p32, endian) * PRIME32_2; state->v4 = XXH_rotl32(state->v4, 13); state->v4 *= PRIME32_1; p32++;
        }
        /* [한국어] input 포인터 전진. */
        p += 16-state->memsize;
        /* [한국어] 부분 버퍼 비움. */
        state->memsize = 0;
    }

    /* [한국어] 남은 입력 16B 이상이면 본체 루프. */
    if (p <= bEnd-16)
    {
        const uint8_t * const limit = bEnd - 16;
        /* [한국어] 누적자를 로컬 레지스터로 승격(성능). */
        uint32_t v1 = state->v1;
        uint32_t v2 = state->v2;
        uint32_t v3 = state->v3;
        uint32_t v4 = state->v4;

        /* [한국어] 4-way 16B 스트라이프 루프(원샷과 동일 시퀀스). */
        do
        {
            v1 += XXH_readLE32((const uint32_t*)p, endian) * PRIME32_2; v1 = XXH_rotl32(v1, 13); v1 *= PRIME32_1; p+=4;
            v2 += XXH_readLE32((const uint32_t*)p, endian) * PRIME32_2; v2 = XXH_rotl32(v2, 13); v2 *= PRIME32_1; p+=4;
            v3 += XXH_readLE32((const uint32_t*)p, endian) * PRIME32_2; v3 = XXH_rotl32(v3, 13); v3 *= PRIME32_1; p+=4;
            v4 += XXH_readLE32((const uint32_t*)p, endian) * PRIME32_2; v4 = XXH_rotl32(v4, 13); v4 *= PRIME32_1; p+=4;
        } while (p<=limit);

        /* [한국어] 누적자 상태 저장. */
        state->v1 = v1;
        state->v2 = v2;
        state->v3 = v3;
        state->v4 = v4;
    }

    /* [한국어] <16B 꼬리를 memory 에 보관 — 다음 update/digest 가 이어감. */
    if (p < bEnd)
    {
        memcpy(state->memory, p, bEnd-p);
        state->memsize = (int)(bEnd-p);
    }

    return XXH_OK;
}

/*
 * [한국어] XXH32_update - 엔디안 감지 후 _endian 위임. */
XXH_errorcode XXH32_update (void* state_in, const void* input, int len)
{
    XXH_endianess endian_detected = (XXH_endianess)XXH_CPU_LITTLE_ENDIAN;

    if ((endian_detected==XXH_littleEndian) || XXH_FORCE_NATIVE_FORMAT)
        return XXH32_update_endian(state_in, input, len, XXH_littleEndian);
    else
        return XXH32_update_endian(state_in, input, len, XXH_bigEndian);
}



/*
 * [한국어]
 * XXH32_intermediateDigest_endian - 상태 보존 형태로 중간 해시 추출(엔디안 파라미터)
 *
 * 동작: 현재 누적자 합성 + total_len XOR + 꼬리 4B/1B + avalanche.
 * 상태는 변경되지 않아 이후 update 를 이어갈 수 있다(checkpoint).
 */
static uint32_t XXH32_intermediateDigest_endian (void* state_in, XXH_endianess endian)
{
    struct XXH_state32_t * state = (struct XXH_state32_t *) state_in;
    /* [한국어] 꼬리 시작·끝 포인터(memory 기반). */
    const uint8_t *p = (const uint8_t *)state->memory;
    uint8_t * bEnd = (uint8_t *)state->memory + state->memsize;
    uint32_t h32;

    /* [한국어] 16B 이상 처리했으면 4 누적자 합성, 아니면 작은 버퍼 초기화. */
    if (state->total_len >= 16)
    {
        h32 = XXH_rotl32(state->v1, 1) + XXH_rotl32(state->v2, 7) + XXH_rotl32(state->v3, 12) + XXH_rotl32(state->v4, 18);
    }
    else
    {
        h32  = state->seed + PRIME32_5;
    }

    /* [한국어] 길이 혼합. */
    h32 += (uint32_t) state->total_len;

    /* [한국어] 4B 단위 꼬리. */
    while (p<=bEnd-4)
    {
        h32 += XXH_readLE32((const uint32_t*)p, endian) * PRIME32_3;
        h32  = XXH_rotl32(h32, 17) * PRIME32_4;
        p+=4;
    }

    /* [한국어] 1B 단위 꼬리. */
    while (p<bEnd)
    {
        h32 += (*p) * PRIME32_5;
        h32 = XXH_rotl32(h32, 11) * PRIME32_1;
        p++;
    }

    /* [한국어] avalanche finalization. */
    h32 ^= h32 >> 15;
    h32 *= PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= PRIME32_3;
    h32 ^= h32 >> 16;

    return h32;
}


/*
 * [한국어] XXH32_intermediateDigest - 엔디안 감지 후 _endian 위임. 상태 유지(free 없음). */
uint32_t XXH32_intermediateDigest (void* state_in)
{
    XXH_endianess endian_detected = (XXH_endianess)XXH_CPU_LITTLE_ENDIAN;

    if ((endian_detected==XXH_littleEndian) || XXH_FORCE_NATIVE_FORMAT)
        return XXH32_intermediateDigest_endian(state_in, XXH_littleEndian);
    else
        return XXH32_intermediateDigest_endian(state_in, XXH_bigEndian);
}


/*
 * [한국어] XXH32_digest - 최종 해시 추출 + free. XXH32_init 와 쌍.
 */
uint32_t XXH32_digest (void* state_in)
{
    /* [한국어] intermediate 로 해시 추출. */
    uint32_t h32 = XXH32_intermediateDigest(state_in);

    /* [한국어] 상태 메모리 해제. */
    free(state_in);

    return h32;
}
