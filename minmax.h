/*
 * [한국어] minmax.h - 타입 안전 min/max 매크로
 *
 * Linux 커널 스타일의 min/max 매크로를 제공한다.
 * 특징:
 *   - GCC 확장(__typeof__)을 사용한 타입 안전 비교
 *   - (void)(&_x == &_y) 트릭으로 타입 불일치 시 컴파일 경고 발생
 *   - 임시 변수를 사용하여 매크로 인자의 부작용(side effect) 방지
 *   - min_not_zero: 0이 아닌 값 중 최솟값 반환
 */
#ifndef FIO_MIN_MAX_H
#define FIO_MIN_MAX_H

/* [한국어] min - 두 값 중 작은 값을 반환 (타입 안전) */
#ifndef min
#define min(x,y) ({ \
	__typeof__(x) _x = (x);	\
	__typeof__(y) _y = (y);	\
	(void) (&_x == &_y);		\
	_x < _y ? _x : _y; })
#endif

/* [한국어] max - 두 값 중 큰 값을 반환 (타입 안전) */
#ifndef max
#define max(x,y) ({ \
	__typeof__(x) _x = (x);	\
	__typeof__(y) _y = (y);	\
	(void) (&_x == &_y);		\
	_x > _y ? _x : _y; })
#endif

/* [한국어] min_not_zero - 0이 아닌 값 중 최솟값을 반환
 *   둘 다 0이 아니면: min(x, y)
 *   하나가 0이면: 0이 아닌 쪽을 반환
 *   둘 다 0이면: 0을 반환
 */
#define min_not_zero(x, y) ({		\
	__typeof__(x) __x = (x);		\
	__typeof__(y) __y = (y);		\
	__x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })

#endif
