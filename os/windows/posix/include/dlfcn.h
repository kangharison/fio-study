/*
 * [한국어 설명] Windows용 dlfcn.h 호환 헤더
 * POSIX 동적 라이브러리 API(dlopen/dlclose/dlsym/dlerror)를 선언.
 * LoadLibrary/FreeLibrary/GetProcAddress로 에뮬레이션 (windows/posix.c).
 */
#ifndef DLFCN_H
#define DLFCN_H

#define RTLD_LAZY 1

void *dlopen(const char *file, int mode);
int dlclose(void *handle);
void *dlsym(void *restrict handle, const char *restrict name);
char *dlerror(void);

#endif /* DLFCN_H */
