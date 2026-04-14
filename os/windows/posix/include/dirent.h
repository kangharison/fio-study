/*
 * [한국어 설명] Windows용 dirent.h 호환 헤더
 * POSIX 디렉토리 읽기 API(opendir/readdir/closedir)를 선언.
 * dirent_ctx 구조체는 FindFirstFile/FindNextFile 핸들을 래핑.
 * 실제 구현은 windows/posix.c.
 */
#ifndef DIRENT_H
#define DIRENT_H

#include <winsock2.h>

struct dirent
{
	ino_t  d_ino;     /*  File serial number */
	char   d_name[MAX_PATH];  /* Name of entry */
};

struct dirent_ctx
{
	HANDLE find_handle;
	char dirname[MAX_PATH];
};

typedef struct dirent_ctx DIR;

DIR *opendir(const char *dirname);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif /* DIRENT_H */
