/*
 * [한국어 설명] fio 공용 기본 타입/폴백 정의 헤더 (types.h)
 *
 * === 파일의 역할 ===
 * fio 가 지원하는 모든 컴파일러/플랫폼에서 공통으로 쓸 `bool/true/false` 와
 * Linux 커널이 제공하는 `__kernel_rwf_t` (io_uring/preadv2 의 RWF_* 플래그
 * 타입) 에 대한 폴백 정의를 제공한다. 본 헤더는 가장 낮은 계층의 이식성
 * 어댑터이며, fio 의 거의 모든 소스가 직간접적으로 포함한다. CONFIG_HAVE_BOOL
 * 과 CONFIG_HAVE_KERNEL_RWF_T 는 configure 스크립트가 컴파일 시점에 컴파일러
 * 지원 여부를 감지해 -D 로 주입하는 매크로이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 빌드 의존 그래프의 최하층. pow2.h, bloom.h, axmap.h, seqlock.h 등
 * bool 반환/매개변수를 쓰는 모든 헤더가 포함한다. __kernel_rwf_t 는 io_uring
 * 엔진(engines/io_uring.c) 과 preadv2/pwritev2 래퍼(os/) 가 RWF_NOWAIT,
 * RWF_DSYNC 등 플래그 타입 선언에 사용한다.
 * 호출 체인: 없음(선언 전용 헤더).
 *
 * === 타 모듈과의 연결 ===
 * - configure : CONFIG_HAVE_BOOL / CONFIG_HAVE_KERNEL_RWF_T 결정.
 * - <stdbool.h> (조건부 포함) : C99 bool 표준 정의.
 * - 거의 모든 fio 소스 : 직간접 포함.
 * 데이터 흐름: 없음(타입/매크로 선언만).
 *
 * === 주요 함수/구조체 요약 ===
 * - bool / true / false : C 표준 <stdbool.h> 가용 시 #include, 아니면 int/0/1
 *   로 폴백.
 * - __kernel_rwf_t : Linux uapi <linux/fs.h> 에서 오는 RWF_* 플래그 타입.
 *   커널 헤더가 없는 환경(Windows, macOS, BSD 일부) 에서는 int 로 폴백.
 */
#ifndef FIO_TYPES_H
#define FIO_TYPES_H
/* [한국어] 헤더 가드. 최하층 헤더이므로 모든 포함 경로에서 중복 방지가 필수. */

/* [한국어] C 컴파일러가 C99 bool 을 제공하지 않는 환경(일부 구형 MSVC, 특수
 * 임베디드 컴파일러) 을 위한 폴백. configure 가 CONFIG_HAVE_BOOL 을 정의
 * 하면 이 블록은 건너뛰고 <stdbool.h> 를 쓴다. __cplusplus 분기는 C++
 * 은 bool 이 키워드이므로 중복 정의 방지. */
#if !defined(CONFIG_HAVE_BOOL) && !defined(__cplusplus)
typedef int bool;
/* [한국어] bool 을 int 로 별칭. C99 _Bool(1 비트) 과 달리 int 크기(4B) 를
 * 사용하여 alignment 문제를 피하지만 메모리는 더 씀 — 폴백 환경에서의 절충. */

#ifndef false
#define false	0
/* [한국어] false 는 정수 0 으로 정의. if(x) 테스트 호환. */
#endif
#ifndef true
#define true	1
/* [한국어] true 는 정수 1 로 정의. 논리 연산 결과와 비교 가능. */
#endif
#else
#include <stdbool.h> /* IWYU pragma: export */
/* [한국어] C99 표준 <stdbool.h> 경로. bool/true/false 는 _Bool 의 별칭.
 * IWYU pragma: export 는 Include-What-You-Use 도구가 본 헤더를 포함한
 * 하위 헤더들도 bool 을 간접 노출받도록 허용 표시 — 본 파일을 인클루드
 * 하는 소스가 <stdbool.h> 를 따로 포함할 필요가 없게 한다. */
#endif

/* [한국어] __kernel_rwf_t : Linux 커널의 RWF_* 플래그 (io_uring/preadv2/
 * pwritev2) 타입. 커널 uapi 헤더 <linux/fs.h> 가 없는 플랫폼(Windows, macOS
 * 등) 에서는 int 로 폴백하여 fio 소스가 공통 타입으로 사용. */
#if !defined(CONFIG_HAVE_KERNEL_RWF_T)
typedef int __kernel_rwf_t;
/* [한국어] int 로 별칭. 커널이 정의하는 원본은 __u32 기반이지만 fio 의 폴백
 * 경로에서는 플래그 연산용이므로 int 로 충분. io_uring 엔진이 이 타입에
 * 저장되는 비트 패턴을 직접 다루지 않고 항상 커널 헤더 경유로만 접근. */
#endif

#endif
/* [한국어] FIO_TYPES_H 가드 종료. */
