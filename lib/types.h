/*
 * [한국어 설명] 기본 타입 정의 (types.h)
 *
 * === 파일의 역할 ===
 * fio 전반에서 사용되는 기본 타입 정의를 제공한다.
 * C 컴파일러에 bool 타입이 없는 환경을 위해 bool/true/false를 정의하고,
 * __kernel_rwf_t 타입이 없는 환경을 위한 폴백 정의도 포함한다.
 *
 * === fio에서의 사용 ===
 * fio의 거의 모든 소스 파일에서 직간접적으로 포함되는 기본 헤더로,
 * 다양한 플랫폼과 컴파일러 간의 타입 호환성을 보장하는 역할을 한다.
 */
#ifndef FIO_TYPES_H
#define FIO_TYPES_H

#if !defined(CONFIG_HAVE_BOOL) && !defined(__cplusplus)
typedef int bool;
#ifndef false
#define false	0
#endif
#ifndef true
#define true	1
#endif
#else
#include <stdbool.h> /* IWYU pragma: export */
#endif

#if !defined(CONFIG_HAVE_KERNEL_RWF_T)
typedef int __kernel_rwf_t;
#endif

#endif
