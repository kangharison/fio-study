/*
 * [한국어 설명] Windows용 asm/types.h 호환 헤더
 * Linux 커널 스타일 타입(__u16, __u32, __u64)을 Windows unsigned 타입으로 정의.
 * io_uring.h 등 커널 헤더 호환에 사용.
 */
#ifndef ASM_TYPES_H
#define ASM_TYPES_H

typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;

#endif /* ASM_TYPES_H */
