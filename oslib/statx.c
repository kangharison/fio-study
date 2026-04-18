/*
 * [한국어 설명] 확장 파일 상태 조회 시스콜 래퍼 폴리필 구현 (statx.c)
 *
 * === 파일의 역할 ===
 * Linux 4.11 에서 도입된 statx(2) 시스콜에 대한 glibc 래퍼가 없는 환경을
 * 위한 폴리필을 제공한다. statx 는 stat(2) 의 확장으로, btime(생성 시각)·
 * STATX_ATTR_*(압축·암호화·불변·자동마운트 등) 같은 부가 속성을 제공하며,
 * 호출자가 mask 로 필요한 필드만 선별 요청해 비용을 줄일 수 있다.
 * 본 파일은 3단 분기:
 *   (a) libc 가 statx 래퍼 제공 → CONFIG_HAVE_STATX 정의, 파일 전체 스킵.
 *   (b) libc 래퍼 없음 + 커널 시스콜 지원 → CONFIG_HAVE_STATX_SYSCALL 로
 *       syscall(__NR_statx, ...) 직접 호출 경로 활성.
 *   (c) 커널도 지원 안 함 → EINVAL 스텁 — 호출자는 errno == EINVAL 감지
 *       시 stat(2) 로 폴백하도록 설계.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 oslib/ 이식성 계층. configure 스크립트가 빌드 시 glibc 버전·
 * 커널 헤더 존재 여부를 탐지하여 위 세 분기 중 하나를 선택한다. 호출자는
 * filesetup.c·iolog.c 등에서 파일 크기 측정, 생성 시각 조회 등을 수행할 때
 * statx 를 사용할 수 있다. 단 fio 는 보수적으로 fall back 경로를 많이 두어,
 * statx 가 없을 때도 일반 stat 으로 대체하도록 호출 사이트가 작성된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: fio 파일 메타데이터 조회 경로(filesetup.c 등).
 * - 의존: (a) <unistd.h> 의 syscall(2), <sys/syscall.h> 의 __NR_statx,
 *         (b) <errno.h> 의 errno/EINVAL(스텁 경로).
 * - 공유 상태: 없음 — 순수 시스콜 래퍼. 시그널 안전성은 syscall(2) 규약.
 *
 * === 주요 함수 요약 ===
 * - statx(): CONFIG_HAVE_STATX_SYSCALL 분기에서는 커널에 직접 위임,
 *   미지원 분기에서는 항상 EINVAL 실패.
 */
#ifndef CONFIG_HAVE_STATX  /* [한국어] libc 가 statx 래퍼를 제공하면 본 TU 전체 스킵. */
#include "statx.h"  /* [한국어] struct statx 선언·플래그 상수(AT_* / STATX_*) 임포트. */

#ifdef CONFIG_HAVE_STATX_SYSCALL  /* [한국어] 커널 __NR_statx 가 헤더에 존재하면 직접 시스콜 경로. */
#include <unistd.h>  /* [한국어] syscall(2) 선언. */
#include <sys/syscall.h>  /* [한국어] __NR_statx 시스콜 번호 매크로 — 아키텍처별 번호. */

/*
 * [한국어]
 * statx - Linux 커널에 statx 시스콜을 직접 발행 (libc 래퍼가 없는 환경용)
 *
 * @dfd: 상대 경로의 기준 디렉토리 fd. AT_FDCWD 면 cwd 기준.
 * @pathname: 조회 대상 경로. flags 에 AT_EMPTY_PATH 가 설정되어 있고
 *            dfd 가 열린 fd 이면 빈 문자열("")로 해당 fd 의 파일을 조회.
 * @flags: AT_SYMLINK_NOFOLLOW(심볼릭 링크 따라가지 않음)·AT_EMPTY_PATH·
 *         AT_NO_AUTOMOUNT·AT_STATX_SYNC_TYPE 등 at(2) 계열 플래그.
 * @mask: STATX_BASIC_STATS / STATX_BTIME / STATX_ALL 등. 커널에 "이 필드
 *        만 계산하라" 고 요청 — 파일시스템이 비싼 필드(btime 등)를 건너뛰
 *        도록 허용.
 * @buffer: 결과를 채울 out 파라미터. 커널이 stx_mask 로 실제 유효 필드를
 *          알린다 — 요청 mask 와 반환 mask 가 다를 수 있음에 주의.
 * @return: 성공 0, 실패 -1(errno 설정).
 *
 * 왜 필요한가: 구형 glibc(< 2.28)는 statx 래퍼가 없어 애플리케이션이
 * syscall() 로 직접 호출해야 한다. Linux 4.11+ 에서 시스콜 자체는 지원되므로,
 * 이 경로로 최신 기능(btime, 증분 mask)을 그대로 사용할 수 있다.
 *
 * 호출 체인:
 *   fio 파일 메타 조회 → [statx()] → syscall(__NR_statx, ...) → 커널 VFS
 */
int statx(int dfd, const char *pathname, int flags, unsigned int mask,
	  struct statx *buffer)
{
	/* [한국어] syscall(2) 로 statx(2) 번호 __NR_statx 를 지정해 커널에
	 * 인자 5개(dfd, pathname, flags, mask, buffer)를 그대로 전달.
	 * 반환값/에러코드 규약은 glibc 래퍼와 동일(성공 0, 실패 -1/errno). */
	return syscall(__NR_statx, dfd, pathname, flags, mask, buffer);
}
#else  /* [한국어] CONFIG_HAVE_STATX_SYSCALL 미정의 — 커널도 미지원인 환경. */
#include <errno.h>  /* [한국어] EINVAL / errno 설정용. */

/*
 * [한국어]
 * statx - 커널이 statx 를 지원하지 않는 시스템용 영구 실패 스텁
 *
 * @dfd, @pathname, @flags, @mask, @buffer: 래퍼 시그니처 유지용 — 모두 무시.
 * @return: 항상 -1, errno=EINVAL.
 *
 * 왜 이렇게 하는가: 호출 사이트가 "statx 미지원 시 stat(2) 폴백" 로직을
 * 갖도록 설계됐으므로, 본 스텁은 "ABI 는 유지하되 언제나 실패 반환" 계약을
 * 제공한다. EINVAL 은 "유효하지 않은 mask 또는 지원 안 됨"을 뜻하는
 * 일반적 에러로 호출자 분기에 적합하다.
 */
int statx(int dfd, const char *pathname, int flags, unsigned int mask,
	  struct statx *buffer)
{
	errno = EINVAL;  /* [한국어] 영구 실패 시 에러 코드 — 호출자 폴백 트리거용. */
	return -1;  /* [한국어] 표준 에러 반환. */
}
#endif  /* [한국어] CONFIG_HAVE_STATX_SYSCALL 분기 종료. */
#endif  /* [한국어] !CONFIG_HAVE_STATX 블록 종료. */
