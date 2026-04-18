/*
 * [한국어 설명] statx(2) 폴리필 헤더 (statx.h)
 *
 * === 파일의 역할 ===
 * Linux 4.11+ 에서 추가된 확장 stat 시스템 호출 statx(2) 를 fio 에서 사용할 수
 * 있도록 하는 폴리필 선언부이다. statx(2) 는 기존 stat(2) 와 달리 원하는 정보만
 * 선택적으로 조회(STATX_* 마스크), 생성 시간(btime), 마운트 ID, DIO 정렬 요구
 * 등 Linux 고유 확장 필드를 제공한다. 3 가지 빌드 경로를 처리한다:
 *   1) CONFIG_HAVE_STATX: glibc 2.28+ 가 statx() 래퍼를 제공 → 본 헤더 건너뜀.
 *   2) CONFIG_HAVE_STATX_SYSCALL: 커널 헤더(<linux/stat.h>) 에 struct statx 는
 *      있으나 glibc 래퍼가 없음 → syscall(2) 로 직접 호출하는 폴백 선언.
 *   3) 둘 다 없음: 빈 struct statx 와 항상 실패하는 스텁 선언(컴파일만 통과시킴).
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 파일 메타데이터 조회 경로:
 *   filesetup.c::get_file_size() / extend_file() → statx(fd, "", AT_EMPTY_PATH,
 *     STATX_SIZE|STATX_BLKSIZE, &stx)
 *   fileoperations 엔진(engines/fileoperations.c) 의 FS 메타 연산 측정에도 사용.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/statx.c: syscall() 직접 호출 구현(CONFIG_HAVE_STATX_SYSCALL 경로).
 * - filesetup.c, engines/fileoperations.c: 주 호출자.
 * - <linux/stat.h>: struct statx 정의(커널 UAPI 헤더, CONFIG_HAVE_STATX_SYSCALL 경로).
 * - <sys/stat.h>: 표준 stat/fstat 와 공존, 일부 플래그 공유.
 * - configure: CONFIG_HAVE_STATX 와 CONFIG_HAVE_STATX_SYSCALL 판별.
 *
 * === 주요 함수/구조체 요약 ===
 * - statx(dfd, pathname, flags, mask, buffer): 확장 stat 조회.
 * - struct statx: 커널 UAPI 구조체(커널 헤더에서 공급) 또는 빈 스텁.
 * - STATX_ALL 매크로: 스텁 경로에서 0 으로 정의해 컴파일 통과시킴.
 */
#ifndef CONFIG_HAVE_STATX
/* [한국어] CONFIG_HAVE_STATX 정의 시 glibc 가 래퍼를 제공하므로 본 헤더의 선언 불필요.
 * 미정의 시 아래 두 경로 중 하나로 폴백 준비. */

#ifdef CONFIG_HAVE_STATX_SYSCALL
/* [한국어] 커널 헤더에 struct statx 는 있지만 glibc 래퍼가 없는 경우.
 * <linux/stat.h> 에서 struct statx 정의와 STATX_* 마스크 상수 공급. */

#include <linux/stat.h>
/* [한국어] <linux/stat.h> 포함 이유: struct statx, STATX_SIZE/STATX_BTIME/
 * STATX_MODE 등 커널 UAPI 상수 공급. */

#include <sys/stat.h>
/* [한국어] <sys/stat.h> 포함 이유: 표준 st_* 플래그(AT_EMPTY_PATH, AT_SYMLINK_NOFOLLOW 등)
 * 와의 호환성을 위해 추가. fio 호출자는 두 헤더를 동시에 기대한다. */

#else
/* [한국어] syscall(2) 헤더도 없는 구형 시스템(예: 일부 임베디드) — 빈 스텁 제공.
 * 실제 호출 시 구현 파일(oslib/statx.c)이 errno=ENOSYS 를 설정하고 -1 반환하거나,
 * 컴파일 단위에서 조건부로 호출 자체를 회피한다. */

#define STATX_ALL 0
/* [한국어] 마스크 상수 기본값. 빈 구조체라 어차피 필드 없어 0 으로 무의미화. */

#undef statx
/* [한국어] 매크로 충돌 회피용 undef — 외부 헤더가 statx 라는 이름을 매크로로
 * 선언했을 경우 본 아래 struct 선언이 깨지지 않게 방어. */

struct statx
{
	/* [한국어] 빈 스텁 구조체 — 필드 없음. 컴파일만 통과시키는 것이 목적이며,
	 * 실제 사용 경로가 없거나 런타임에 미지원으로 에러 반환한다. */
};
#endif

/*
 * [한국어]
 * statx - 확장 파일 상태 조회(Linux 4.11+ 폴백)
 *
 * @dfd:      디렉토리 파일 디스크립터(AT_FDCWD 면 현재 작업 디렉토리 기준).
 * @pathname: 대상 경로(빈 문자열 + AT_EMPTY_PATH 플래그 조합 시 dfd 자체 조회).
 * @flags:    AT_EMPTY_PATH/AT_SYMLINK_NOFOLLOW/AT_STATX_FORCE_SYNC 등 동작 제어.
 * @mask:     STATX_SIZE|STATX_BLKSIZE|STATX_TYPE|STATX_MODE|STATX_BTIME 등 원하는 필드.
 *            요청하지 않은 필드는 커널이 채우지 않을 수 있어 비용 절감.
 * @buffer:   결과를 받을 struct statx 포인터.
 * @return:   성공 0, 실패 -1(errno 설정).
 *
 * CONFIG_HAVE_STATX_SYSCALL 경로에서는 syscall(__NR_statx, ...) 를 직접 호출.
 * 스텁 경로에서는 항상 -1 + ENOSYS.
 *
 * 호출 체인: filesetup.c::get_file_size() → statx() → f->real_file_size 설정.
 * 실행 컨텍스트: 잡 스레드 초기화 단계. 에러: errno 로 호출자가 분기.
 */
int statx(int dfd, const char *pathname, int flags, unsigned int mask,
	  struct statx *buffer);
#endif
