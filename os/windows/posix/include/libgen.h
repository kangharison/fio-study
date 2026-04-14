/*
 * [한국어 설명] Windows용 libgen.h 호환 헤더
 * POSIX basename() 함수 선언. 실제 구현은 windows/posix.c.
 */
#ifndef LIBGEN_H
#define LIBGEN_H

char *basename(char *path);

#endif /* LIBGEN_H */
