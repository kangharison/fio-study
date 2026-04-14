/*
 * [한국어 설명] macOS posix_fadvise 에뮬레이션 헤더 (mac/posix.h)
 *
 * === 파일의 역할 ===
 * macOS는 posix_fadvise()를 지원하지 않으므로, fio가 사용하는
 * POSIX_FADV_* 상수를 정의하고 에뮬레이션 함수를 선언한다.
 * 실제 구현은 mac/posix.c에서 F_RDAHEAD fcntl과 mmap/msync를 사용.
 *
 * === 타 모듈과의 연결 ===
 * - os-mac.h에서 포함
 * - filesetup.c, io_u.c 등에서 posix_fadvise() 호출 시 이 에뮬레이션 사용
 */
#ifndef FIO_MAC_POSIX_H
#define FIO_MAC_POSIX_H

/* [한국어] posix_fadvise 어드바이스 상수 (macOS 에뮬레이션용) */
#define POSIX_FADV_NORMAL       (0)	/* [한국어] 기본 접근 패턴 */
#define POSIX_FADV_RANDOM       (1)	/* [한국어] 랜덤 접근 → readahead 비활성화 */
#define POSIX_FADV_SEQUENTIAL   (2)	/* [한국어] 순차 접근 → readahead 활성화 */
#define POSIX_FADV_DONTNEED     (4)	/* [한국어] 캐시 불필요 → mmap+msync로 페이지 해제 */

extern int posix_fadvise(int fd, off_t offset, off_t len, int advice);

#endif
