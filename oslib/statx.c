/*
 * [한국어 설명] 확장 파일 상태 조회 함수 폴리필 구현 (statx.c)
 *
 * === 파일의 역할 ===
 * statx() 시스템 호출의 폴리필 구현을 제공한다.
 * C 라이브러리가 statx()를 직접 제공하지 않지만 커널이 지원하는 경우,
 * syscall()을 통해 직접 호출한다. 커널도 지원하지 않으면 항상 실패하는 스텁을 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio의 파일 메타데이터 조회를 지원한다.
 * statx()는 Linux 4.11+에서 __NR_statx 시스콜 번호로 호출 가능하다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 파일 관련 코드
 * - 의존: syscall() (glibc), __NR_statx (커널 헤더)
 *
 * === 주요 함수 요약 ===
 * - statx(): syscall(__NR_statx, ...) 래퍼 또는 EINVAL 스텁
 */
#ifndef CONFIG_HAVE_STATX
#include "statx.h"

#ifdef CONFIG_HAVE_STATX_SYSCALL
#include <unistd.h>
#include <sys/syscall.h>

/*
 * [한국어]
 * statx - syscall()을 통한 확장 파일 상태 조회
 *
 * @dfd: 디렉토리 파일 디스크립터 (AT_FDCWD 가능)
 * @pathname: 조회할 파일 경로
 * @flags: AT_EMPTY_PATH, AT_SYMLINK_NOFOLLOW 등의 플래그
 * @mask: 조회할 정보 마스크 (STATX_ALL 등)
 * @buffer: 결과가 저장될 statx 구조체
 * @return: 성공 시 0, 실패 시 -1
 *
 * C 라이브러리에 statx() 래퍼가 없는 경우 syscall()로 직접 호출한다.
 * __NR_statx는 커널 헤더에서 정의된 시스콜 번호이다.
 *
 * 호출 체인:
 *   fio 파일 코드 → [statx()] → syscall(__NR_statx, ...)
 */
int statx(int dfd, const char *pathname, int flags, unsigned int mask,
	  struct statx *buffer)
{
	return syscall(__NR_statx, dfd, pathname, flags, mask, buffer);
}
#else
#include <errno.h>

/*
 * [한국어]
 * statx - statx를 지원하지 않는 시스템용 스텁
 *
 * 커널이 statx syscall을 지원하지 않는 구형 시스템에서는
 * 항상 EINVAL 에러로 실패한다. 호출자는 이 경우 기존 stat()으로 대체해야 한다.
 */
int statx(int dfd, const char *pathname, int flags, unsigned int mask,
	  struct statx *buffer)
{
	errno = EINVAL;
	return -1;
}
#endif
#endif
