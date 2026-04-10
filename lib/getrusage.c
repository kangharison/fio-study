/*
 * [한국어 설명] 리소스 사용량 래퍼 (getrusage.c)
 *
 * === 파일의 역할 ===
 * 시스템의 getrusage() 호출을 감싸는 래퍼 함수를 제공한다. 가능한 경우
 * RUSAGE_THREAD(스레드 단위)를 먼저 시도하고, 지원되지 않으면
 * RUSAGE_SELF(프로세스 단위)로 폴백한다.
 *
 * === fio에서의 사용 ===
 * fio가 벤치마크 실행 중 CPU 사용 시간(user/system time)을 측정하여
 * 통계에 포함시킬 때 이 함수를 호출한다.
 */
#include <errno.h>
#include "getrusage.h"

int fio_getrusage(struct rusage *ru)
{
#ifdef CONFIG_RUSAGE_THREAD
	if (!getrusage(RUSAGE_THREAD, ru))
		return 0;
	if (errno != EINVAL)
		return -1;
	/* Fall through to RUSAGE_SELF */
#endif
	return getrusage(RUSAGE_SELF, ru);
}
