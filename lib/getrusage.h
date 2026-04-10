/*
 * [한국어 설명] 리소스 사용량 래퍼 헤더 (getrusage.h)
 *
 * === 파일의 역할 ===
 * fio_getrusage() 함수의 선언과 필요한 시스템 헤더(sys/time.h, sys/resource.h)
 * 포함을 제공한다.
 *
 * === fio에서의 사용 ===
 * fio의 CPU 사용량 통계 수집 코드에서 리소스 사용량을 조회하기 위해
 * 이 헤더를 포함한다.
 */
#ifndef FIO_GETRUSAGE_H
#define FIO_GETRUSAGE_H

#include <sys/time.h>
#include <sys/resource.h>

extern int fio_getrusage(struct rusage *ru);

#endif
