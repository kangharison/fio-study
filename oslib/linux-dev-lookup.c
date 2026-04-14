/*
 * [한국어 설명] Linux 블록 장치 검색 구현 (linux-dev-lookup.c)
 *
 * === 파일의 역할 ===
 * blktrace 재생(replay) 시 major:minor 번호를 기반으로 /dev 디렉토리를
 * 재귀적으로 탐색하여 해당하는 블록 장치 노드를 찾는다.
 * replay_redirect 옵션이 설정되면 탐색 없이 지정된 장치를 바로 반환한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 blktrace 재생 체인:
 *   iolog.c → blktrace_lookup_device() → /dev 디렉토리 탐색
 *
 * === 타 모듈과의 연결 ===
 * - 호출자: iolog.c의 blktrace 재생 코드
 * - 의존: stat(), readdir() (POSIX 파일시스템 API)
 *
 * === 주요 함수 요약 ===
 * - blktrace_lookup_device(): /dev 재귀 탐색으로 major:minor 매칭 장치 검색
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "linux-dev-lookup.h"

/*
 * [한국어]
 * blktrace_lookup_device - major:minor 번호로 블록 장치 노드를 검색
 *
 * @redirect: replay_redirect 경로 (NULL이 아니면 이 경로를 바로 반환)
 * @path: 검색 시작 디렉토리 (보통 "/dev"), 결과 경로도 여기에 저장됨
 * @maj: 찾을 장치의 major 번호
 * @min: 찾을 장치의 minor 번호
 * @return: 찾으면 1, 못 찾으면 0
 *
 * 동작 과정:
 * 1) redirect가 설정되면 해당 경로를 바로 복사하고 1 반환
 * 2) path 디렉토리를 opendir()로 열고 모든 엔트리를 순회
 * 3) 하위 디렉토리면 재귀 호출
 * 4) 블록 장치(S_ISBLK)이고 major:minor가 일치하면 경로를 path에 복사
 *
 * 호출 체인:
 *   iolog.c → [blktrace_lookup_device()] → stat(), readdir(), 재귀 호출
 */
int blktrace_lookup_device(const char *redirect, char *path, unsigned int maj,
			   unsigned int min)
{
	struct dirent *dir;
	struct stat st;
	int found = 0;
	DIR *D;

	/*
	 * If replay_redirect is set then always return this device
	 * upon lookup which overrides the device lookup based on
	 * major minor in the actual blktrace
	 */
	if (redirect) {
		strcpy(path, redirect);
		return 1;
	}

	D = opendir(path);
	if (!D)
		return 0;

	while ((dir = readdir(D)) != NULL) {
		char full_path[257];

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;

		sprintf(full_path, "%s/%s", path, dir->d_name);
		if (lstat(full_path, &st) == -1) {
			perror("lstat");
			break;
		}

		if (S_ISDIR(st.st_mode)) {
			found = blktrace_lookup_device(redirect, full_path,
								maj, min);
			if (found) {
				strcpy(path, full_path);
				break;
			}
		}

		if (!S_ISBLK(st.st_mode))
			continue;

		if (maj == major(st.st_rdev) && min == minor(st.st_rdev)) {
			strcpy(path, full_path);
			found = 1;
			break;
		}
	}

	closedir(D);
	return found;
}
