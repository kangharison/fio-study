/*
 * [한국어 설명] 테스트용 디버그 스텁 (debug.c)
 *
 * === 파일의 역할 ===
 * 테스트 프로그램을 독립적으로 빌드할 때 fio 본체의 디버그 심볼을 대체하는
 * 스텁 파일이다. __dprint는 빈 함수로 구현되어 디버그 출력을 무시하며,
 * debug_init은 f_err를 stderr로 초기화한다.
 */
#include <stdio.h>

FILE *f_err;
void *fio_ts;
unsigned long fio_debug = 0;

void __dprint(int type, const char *str, ...)
{
}

void debug_init(void)
{
	f_err = stderr;
}
