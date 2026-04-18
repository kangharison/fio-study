/*
 * [한국어 설명] Linux /dev 재귀 탐색으로 major:minor → 블록 장치 경로 매핑 (linux-dev-lookup.c)
 *
 * === 파일의 역할 ===
 * blktrace 기반 I/O 재생(replay) 시, 원본 트레이스에 기록된 (major, minor)
 * 디바이스 번호로부터 실제 블록 장치 노드 경로(예: /dev/sda1)를 찾아낸다.
 * 동일 번호가 여러 마운트 경로에 나타날 수 있으므로, 보통은 /dev 디렉토리
 * 전체를 재귀 스캔하여 S_ISBLK 이면서 major/minor 가 일치하는 첫 노드를
 * 리턴한다. 사용자가 --replay_redirect=<path> 로 리다이렉트를 지정하면
 * 탐색 없이 해당 경로를 즉시 반환하여, 다른 하드웨어에서 트레이스를
 * 재생할 수 있게 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 blktrace 재생 경로:
 *   iolog.c::load_blktrace() → blktrace.c (파일 파싱) →
 *     [blktrace_lookup_device()] → /dev 재귀 스캔 → open() 후 I/O 재생
 * 탐색 결과는 iolog.c 가 struct io_piece 에 저장하고, 재생 중 각 io_u 가
 * 해당 fd 로 발행된다.
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: iolog.c 의 blktrace 재생 파서(주로 load_blktrace 내부).
 * - 의존: POSIX 파일시스템 API — opendir/readdir/closedir, lstat(2)(심볼릭
 *   링크를 따르지 않도록), <sys/sysmacros.h> 의 major()/minor() 매크로(st_rdev
 *   분해). /dev 는 일반적으로 devtmpfs 에 마운트되어 커널이 유지 관리.
 * - 공유 상태: 없음(순수 함수·재귀 호출 경로도 함수 인자만으로 상태 전달).
 *
 * === 주요 함수 요약 ===
 * - blktrace_lookup_device(): redirect 우선 처리 → /dev 재귀 탐색 →
 *   일치 노드의 경로를 path 버퍼에 기록.
 */
#include <sys/types.h>  /* [한국어] 기본 POSIX 타입(ino_t, off_t 등). */
#include <sys/stat.h>  /* [한국어] struct stat / lstat / S_ISBLK / S_ISDIR. */
#include <sys/sysmacros.h>  /* [한국어] major()/minor() 매크로 — dev_t 분해. glibc 2.25+ 는 별도 헤더. */
#include <dirent.h>  /* [한국어] DIR/readdir/opendir/closedir. */
#include <string.h>  /* [한국어] strcmp/strcpy 선언. */
#include <stdio.h>  /* [한국어] perror/sprintf. */
#include <unistd.h>  /* [한국어] 기본 유틸 — 여기선 간접 의존. */

#include "linux-dev-lookup.h"  /* [한국어] 공개 API 선언. */

/*
 * [한국어]
 * blktrace_lookup_device - /dev 재귀 스캔으로 (major, minor) 매칭 블록 장치 경로 탐색
 *
 * @redirect: --replay_redirect 의 치환 경로. non-NULL 이면 탐색을 건너뛰고
 *            path 에 복사한 뒤 1 리턴 — blktrace 가 가리키던 원래 장치가
 *            현재 시스템에 없을 때 사용자가 다른 장치로 매핑하는 기능.
 * @path: 탐색 시작 디렉토리(보통 "/dev"). 함수가 성공 시 결과 전체 경로를
 *        이 버퍼에 덮어쓰므로 호출자는 충분한 크기를 보장해야 한다.
 * @maj: 찾을 장치의 major 번호(커널 드라이버 식별).
 * @min: 찾을 장치의 minor 번호(같은 드라이버 내 인스턴스 식별).
 * @return: 일치 장치 발견 시 1, 없으면 0. 에러는 perror 로 stderr 출력 후
 *          부분 실패로 0 가능.
 *
 * 동작 단계:
 * 1) redirect != NULL → strcpy(path, redirect); return 1 (fast path).
 * 2) opendir(path) 로 디렉토리 open, 실패 시 0.
 * 3) readdir 로 항목 순회:
 *    - "." / ".." 스킵(무한 재귀 방지).
 *    - sprintf 로 "path/엔트리명" 조합(full_path).
 *    - lstat(full_path) — 심볼릭 링크는 따라가지 않아 장치 오인 방지.
 *    - S_ISDIR(중간 디렉토리, 예: /dev/mapper) 이면 재귀 호출;
 *      재귀 성공 시 path 에 하위 경로 복사 후 break.
 *    - S_ISBLK 이면서 major/minor 일치 → path 복사, found=1, break.
 * 4) closedir 후 found 반환.
 *
 * 주의: 재귀 호출로 스택 깊이는 /dev 의 디렉토리 중첩 깊이에 비례. 보통 얕아
 * 문제 없지만, 악의적/비정상 /dev 구조에서 스택 오버플로 가능성 이론상 존재.
 * full_path 가 257 바이트 고정이므로 지나치게 긴 이름에는 truncation 가능
 * (fio 의 /dev 사용 범위에선 실제 문제 없음).
 *
 * 호출 체인:
 *   iolog.c::load_blktrace → blktrace.c → [blktrace_lookup_device()]
 *     → opendir/readdir/lstat/재귀 self
 */
int blktrace_lookup_device(const char *redirect, char *path, unsigned int maj,
			   unsigned int min)
{
	struct dirent *dir;  /* [한국어] readdir 반환 엔트리. */
	struct stat st;  /* [한국어] lstat 결과 — st_mode(파일 종류), st_rdev(장치 번호) 사용. */
	int found = 0;  /* [한국어] 일치 장치 발견 플래그. */
	DIR *D;  /* [한국어] opendir 핸들. */

	/*
	 * If replay_redirect is set then always return this device
	 * upon lookup which overrides the device lookup based on
	 * major minor in the actual blktrace
	 */
	if (redirect) {
		strcpy(path, redirect);  /* [한국어] 사용자 지정 경로를 그대로 반환 경로에 복사. */
		return 1;  /* [한국어] redirect 는 항상 1 리턴(존재 여부 검증은 호출자 책임). */
	}

	D = opendir(path);  /* [한국어] 시작 디렉토리 오픈 — 실패면 즉시 0 반환(예: 권한 없음). */
	if (!D)
		return 0;

	while ((dir = readdir(D)) != NULL) {  /* [한국어] 엔트리 순회 — 종료 조건은 NULL 반환. */
		char full_path[257];  /* [한국어] "path/엔트리명" 조립용 스택 버퍼. dirent.d_name 최대 255 + '/' + NUL. */

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;  /* [한국어] 자기/부모 디렉토리는 건너뛴다 — 무한 재귀 방지 및 무의미한 스캔 회피. */

		sprintf(full_path, "%s/%s", path, dir->d_name);  /* [한국어] 경로 합성. sprintf 는 경로 합이 257-1 을 넘지 않는다는 가정. */
		if (lstat(full_path, &st) == -1) {  /* [한국어] 심볼릭 링크 따라가지 않음 — /dev/stdin 같은 링크가 장치로 오인되지 않도록. */
			perror("lstat");  /* [한국어] 에러 메시지 출력하되 루프 전체 중단. 부분 실패 허용. */
			break;
		}

		if (S_ISDIR(st.st_mode)) {  /* [한국어] 중간 디렉토리(예: /dev/mapper, /dev/disk). 재귀 탐색. */
			found = blktrace_lookup_device(redirect, full_path,
								maj, min);
			if (found) {
				strcpy(path, full_path);  /* [한국어] 재귀가 full_path 를 결과로 채웠음 — 상위 path 에도 반영. */
				break;
			}
		}

		if (!S_ISBLK(st.st_mode))
			continue;  /* [한국어] 블록 디바이스가 아니면 비교 스킵(문자 디바이스·정규 파일 등). */

		if (maj == major(st.st_rdev) && min == minor(st.st_rdev)) {  /* [한국어] major()/minor() 매크로로 dev_t 분해 비교. */
			strcpy(path, full_path);  /* [한국어] 결과를 path 에 기록. */
			found = 1;  /* [한국어] 완료 플래그 — 루프 종료. */
			break;
		}
	}

	closedir(D);  /* [한국어] 핸들 누수 방지. */
	return found;  /* [한국어] 0 = 미발견, 1 = 발견. */
}
