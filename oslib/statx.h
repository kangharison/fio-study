/*
 * [한국어 설명] 확장 파일 상태 조회 함수 폴리필 헤더 (statx.h)
 *
 * === 파일의 역할 ===
 * statx() 시스템 호출이 C 라이브러리에서 직접 제공되지 않는 경우를 위한 선언부이다.
 * statx()는 Linux 4.11에서 추가된 확장 stat 시스템 호출로, 기존 stat()보다
 * 더 많은 파일 메타데이터(생성 시간, 마운트 ID 등)를 제공한다.
 * 3가지 경우를 처리한다:
 * 1) CONFIG_HAVE_STATX: 시스템이 직접 제공 → 이 파일 전체 건너뜀
 * 2) CONFIG_HAVE_STATX_SYSCALL: syscall() 통해 호출 가능 → 커널 헤더 사용
 * 3) 둘 다 없음 → 빈 구조체와 항상 실패하는 스텁 제공
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어로, fio에서 파일 메타데이터 조회에 사용된다.
 * 파일 크기, 블록 크기 등의 정보를 얻는 데 활용된다.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/statx.c: 실제 구현부
 * - fio 파일 관련 코드: 파일 상태 조회 시 사용
 *
 * === 주요 함수 요약 ===
 * - statx(): 확장 파일 상태 조회 (Linux 4.11+ syscall 래퍼)
 */
#ifndef CONFIG_HAVE_STATX
#ifdef CONFIG_HAVE_STATX_SYSCALL
/* [한국어] syscall()로 호출 가능한 경우: 커널 헤더에서 struct statx 정의를 가져옴 */
#include <linux/stat.h>
#include <sys/stat.h>
#else
/* [한국어] statx syscall도 없는 구형 시스템: 빈 구조체와 스텁 함수 제공 */
#define STATX_ALL 0
#undef statx
struct statx
{
};
#endif
int statx(int dfd, const char *pathname, int flags, unsigned int mask,
	  struct statx *buffer);
#endif
