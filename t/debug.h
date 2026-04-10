/*
 * [한국어 설명] 테스트용 디버그 스텁 헤더 (debug.h)
 *
 * === 파일의 역할 ===
 * 테스트 프로그램에서 사용하는 debug_init() 함수의 선언을 제공하는 헤더 파일이다.
 * debug.c에 정의된 디버그 초기화 스텁 함수를 외부에서 호출할 수 있도록
 * 인터페이스를 노출한다.
 */
#ifndef FIO_DEBUG_INC_H
#define FIO_DEBUG_INC_H

extern void debug_init(void);

#endif
