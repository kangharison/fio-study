/*
 * [한국어] err.h - 에러 처리 매크로 (Linux 커널 ERR_PTR 패턴)
 *
 * Linux 커널의 에러 포인터(error pointer) 패턴을 구현한다.
 * 핵심 아이디어: 포인터의 상위 주소 공간은 사용되지 않으므로,
 * 에러 코드(-1 ~ -4095)를 포인터로 인코딩하여 반환할 수 있다.
 *
 * 주요 함수:
 *   ERR_PTR(err)    : 에러 코드 -> 포인터 변환
 *   PTR_ERR(ptr)    : 포인터 -> 에러 코드 변환
 *   IS_ERR(ptr)     : 포인터가 에러인지 확인
 *   IS_ERR_OR_NULL(ptr) : 포인터가 NULL이거나 에러인지 확인
 *   PTR_ERR_OR_ZERO(ptr): 에러이면 에러 코드, 아니면 0 반환
 */
#ifndef FIO_ERR_H
#define FIO_ERR_H

/*
 * Kernel pointers have redundant information, so we can use a
 * scheme where we can return either an error code or a dentry
 * pointer with the same return value.
 *
 * This should be a per-architecture thing, to allow different
 * error and pointer decisions.
 */
/* [한국어] 최대 에러 번호 - 에러 포인터 범위: [(uintptr_t)-4095, (uintptr_t)-1] */
#define MAX_ERRNO	4095

/* [한국어] 값이 에러 포인터 범위에 있는지 확인하는 매크로 */
#define IS_ERR_VALUE(x) ((x) >= (uintptr_t)-MAX_ERRNO)

/* [한국어] 에러 코드를 에러 포인터로 변환 (예: ERR_PTR(-ENOMEM)) */
static inline void *ERR_PTR(uintptr_t error)
{
	return (void *) error;
}

/* [한국어] 에러 포인터에서 에러 코드를 추출 (예: PTR_ERR(ptr) -> -ENOMEM) */
static inline uintptr_t PTR_ERR(const void *ptr)
{
	return (uintptr_t) ptr;
}

/* [한국어] 포인터가 에러 포인터인지 확인 (참이면 0이 아닌 값 반환) */
static inline uintptr_t IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((uintptr_t)ptr);
}

/* [한국어] 포인터가 NULL이거나 에러 포인터인지 확인 */
static inline uintptr_t IS_ERR_OR_NULL(const void *ptr)
{
	return !ptr || IS_ERR_VALUE((uintptr_t)ptr);
}

/* [한국어] 에러 포인터이면 에러 코드를 반환하고, 아니면 0을 반환 */
static inline int PTR_ERR_OR_ZERO(const void *ptr)
{
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	else
		return 0;
}

#endif
