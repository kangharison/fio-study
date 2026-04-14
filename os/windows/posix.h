/*
 * [한국어 설명] Windows POSIX 호환 함수 선언 헤더 (windows/posix.h)
 *
 * === 파일의 역할 ===
 * Windows에서 에뮬레이션하는 POSIX 함수의 선언과 유틸리티를 제공.
 * win_to_posix_error()로 Windows 에러 코드를 POSIX errno로 변환.
 *
 * === 타 모듈과의 연결 ===
 * - os-windows.h에서 포함
 * - windows/posix.c에서 구현
 */
#ifndef FIO_WINDOWS_POSIX_H
#define FIO_WINDOWS_POSIX_H

typedef int clockid_t;

extern int inet_aton(const char *, struct in_addr *);
extern int win_to_posix_error(DWORD winerr);

#endif
