/*
 * [한국어 설명] 장치 마운트 확인 헤더 (mountcheck.h)
 *
 * === 파일의 역할 ===
 * device_is_mounted() 함수의 선언을 제공한다. 장치 경로 문자열을 받아
 * 마운트 여부를 정수(0 또는 1)로 반환한다.
 *
 * === fio에서의 사용 ===
 * fio의 장치 초기화 코드에서 블록 장치의 마운트 상태를 확인하기 위해
 * 이 헤더를 포함한다.
 */
#ifndef FIO_MOUNT_CHECK_H
#define FIO_MOUNT_CHECK_H

extern int device_is_mounted(const char *);

#endif
