/*
 * [한국어 설명] 커널 호환성 타입 정의 (kcompat.h)
 *
 * === 파일의 역할 ===
 * Linux 커널 스타일 타입(u64, u32)을 유저 공간 타입(uint64_t, uint32_t)으로 매핑.
 * 커널 헤더(특히 io_uring.h)를 유저 공간에서 사용할 때 필요한 호환성 제공.
 *
 * === 타 모듈과의 연결 ===
 * - os/linux/io_uring.h에서 사용되는 커널 타입 호환
 */
#ifndef _KCOMPAT_H_
#define _KCOMPAT_H_

#include <stdint.h>

#define u64 uint64_t	/* [한국어] 커널 64비트 unsigned → POSIX uint64_t */
#define u32 uint32_t	/* [한국어] 커널 32비트 unsigned → POSIX uint32_t */

#endif
