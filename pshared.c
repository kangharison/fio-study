/*
 * [한국어] pshared.c - 프로세스 간 공유 뮤텍스/조건변수 초기화 구현
 *
 * PTHREAD_PROCESS_SHARED 속성을 설정하여 fork된 프로세스 간에
 * 공유할 수 있는 뮤텍스와 조건변수를 초기화한다.
 *
 * 주요 기능:
 *   cond_init_pshared()              - 공유 조건변수 초기화 (MONOTONIC 클럭 지원)
 *   mutex_init_pshared_with_type()   - 지정 타입의 공유 뮤텍스 초기화
 *   mutex_init_pshared()             - 기본 타입 공유 뮤텍스 초기화
 *   mutex_cond_init_pshared()        - 뮤텍스 + 조건변수 한 번에 초기화
 *
 * 참고: CONFIG_PSHARED가 정의되지 않은 플랫폼(NetBSD/OpenBSD)에서는
 *       PTHREAD_PROCESS_SHARED 설정을 건너뛴다.
 
 * === 파일의 역할 ===
 * PTHREAD_PROCESS_SHARED 속성을 가진 mutex/cond 초기화 유틸리티를 구현.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전체에서 프로세스 간 공유 동기화 프리미티브 초기화에 사용.
 *
 * === 타 모듈과의 연결 ===
 * - pshared.h: API 선언
 * - fio_sem.c, workqueue.c 등: 공유 뮤텍스/조건변수 초기화 시 호출
 *
 * === 주요 함수/구조체 요약 ===
 * - mutex_init_pshared(): 공유 뮤텍스 초기화
 * - cond_init_pshared(): 공유 조건변수 초기화 (MONOTONIC 클럭)
 */
#include <string.h>

#include "log.h"
#include "pshared.h"

/* [한국어] 프로세스 간 공유 가능한 조건변수 초기화 */
int cond_init_pshared(pthread_cond_t *cond)
{
	pthread_condattr_t cattr;
	int ret;

	ret = pthread_condattr_init(&cattr);
	if (ret) {
		log_err("pthread_condattr_init: %s\n", strerror(ret));
		return ret;
	}

	/* [한국어] 프로세스 간 공유 속성 설정 (지원 플랫폼에서만) */
#ifdef CONFIG_PSHARED
	ret = pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
	if (ret) {
		log_err("pthread_condattr_setpshared: %s\n", strerror(ret));
		return ret;
	}
#endif

	/* [한국어] MONOTONIC 클럭 설정 - 시스템 시간 변경에 영향받지 않는 타이머 */
#ifdef CONFIG_PTHREAD_CONDATTR_SETCLOCK
	ret = pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	if (ret) {
		log_err("pthread_condattr_setclock: %s\n", strerror(ret));
		return ret;
	}
#endif

	ret = pthread_cond_init(cond, &cattr);
	if (ret) {
		log_err("pthread_cond_init: %s\n", strerror(ret));
		return ret;
	}

	return 0;
}

/*
 * 'type' must be a mutex type, e.g. PTHREAD_MUTEX_NORMAL,
 * PTHREAD_MUTEX_ERRORCHECK, PTHREAD_MUTEX_RECURSIVE or PTHREAD_MUTEX_DEFAULT.
 */
/* [한국어] 지정된 타입으로 프로세스 간 공유 뮤텍스 초기화 */
int mutex_init_pshared_with_type(pthread_mutex_t *mutex, int type)
{
	pthread_mutexattr_t mattr;
	int ret;

	ret = pthread_mutexattr_init(&mattr);
	if (ret) {
		log_err("pthread_mutexattr_init: %s\n", strerror(ret));
		return ret;
	}

	/*
	 * Not all platforms support process shared mutexes (NetBSD/OpenBSD)
	 */
	/* [한국어] 프로세스 간 공유 속성 설정 (NetBSD/OpenBSD 등 미지원 플랫폼은 건너뜀) */
#ifdef CONFIG_PSHARED
	ret = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
	if (ret) {
		log_err("pthread_mutexattr_setpshared: %s\n", strerror(ret));
		return ret;
	}
#endif
	/* [한국어] 뮤텍스 타입 설정 (NORMAL, ERRORCHECK, RECURSIVE, DEFAULT) */
	ret = pthread_mutexattr_settype(&mattr, type);
	if (ret) {
		log_err("pthread_mutexattr_settype: %s\n", strerror(ret));
		return ret;
	}
	ret = pthread_mutex_init(mutex, &mattr);
	if (ret) {
		log_err("pthread_mutex_init: %s\n", strerror(ret));
		return ret;
	}
	pthread_mutexattr_destroy(&mattr);

	return 0;
}

/* [한국어] 기본 타입(PTHREAD_MUTEX_DEFAULT)으로 프로세스 공유 뮤텍스 초기화 */
int mutex_init_pshared(pthread_mutex_t *mutex)
{
	return mutex_init_pshared_with_type(mutex, PTHREAD_MUTEX_DEFAULT);
}

/* [한국어] 프로세스 공유 뮤텍스와 조건변수를 한 번에 초기화하는 편의 함수 */
int mutex_cond_init_pshared(pthread_mutex_t *mutex, pthread_cond_t *cond)
{
	int ret;

	ret = mutex_init_pshared(mutex);
	if (ret)
		return ret;

	ret = cond_init_pshared(cond);
	if (ret)
		return ret;

	return 0;
}
