/*
 * [한국어 설명] memcpy 성능 테스트 헤더 (memcpy.h)
 *
 * === 파일의 역할 ===
 * fio_memcpy_test() 함수의 선언을 제공한다. 이 함수는 테스트 유형 문자열을 받아
 * 해당하는 메모리 복사 벤치마크를 실행한다.
 *
 * === fio에서의 사용 ===
 * fio 메인 코드에서 메모리 복사 성능 테스트 기능을 호출하기 위해 이 헤더를 포함한다.
 */
#ifndef FIO_MEMCPY_H
#define FIO_MEMCPY_H

int fio_memcpy_test(const char *type);

#endif
