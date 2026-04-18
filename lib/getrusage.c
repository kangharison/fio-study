/*
 * [한국어 설명] 리소스 사용량 래퍼 (getrusage.c)
 *
 * === 파일의 역할 ===
 * POSIX getrusage(2) 시스템 호출을 감싸는 얇은 호환 래퍼이다. 원래 getrusage(2)는
 * RUSAGE_SELF(프로세스 단위) 또는 RUSAGE_CHILDREN(자식 프로세스 단위)만 표준이지만,
 * Linux 2.6.26+ 는 RUSAGE_THREAD 를 확장으로 제공하여 호출 스레드 단독의
 * 사용자/시스템 CPU 시간, 컨텍스트 스위치, 페이지 폴트, 시그널 수신 횟수 등
 * 커널이 계측해 둔 값을 조회할 수 있다. fio 는 잡(스레드) 단위 통계를 선호하므로
 * RUSAGE_THREAD 를 먼저 시도하고, configure(CONFIG_RUSAGE_THREAD 미정의) 또는
 * 런타임(EINVAL 반환) 양쪽에서 지원되지 않으면 RUSAGE_SELF 로 투명하게 폴백한다.
 * 이 폴백은 "잡이 여러 개인 프로세스 모드(--thread=0 아닌 fork 모드)"에서는 정확도가
 * 문제되지 않고, 스레드 모드(--thread=1)에서는 thread_main 이 커널상 1:1 스레드로
 * 매핑되어 있다는 전제가 깨지면 모든 잡의 합이 보고되어 과대계상될 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * stat.c 의 clear_rusage_stat()/update_rusage_stat() 이 잡 시작(thread_main 초기화)
 * 시점과 러너 종료 직전(do_io 완료 후) 시점에 각 잡 스레드 문맥에서 이 함수를
 * 두 번 호출하여 차분(delta)을 구한다. 계산된 ru_utime/ru_stime 델타는
 * thread_stat::ru_start 와 thread_stat::ru_end 에 저장되어 결과 출력 시
 * "usr=xx.x%, sys=yy.y%, ctx=NNN, majf=MM, minf=PP" 형태로 터미널/JSON/terse
 * 출력에 나타난다. 호출 체인은 각 잡 스레드 내에서 닫혀 있으므로 전역 락을
 * 잡지 않고도 재진입 안전이다(커널이 per-task rusage 를 자체 관리).
 *
 * === 타 모듈과의 연결 ===
 * - stat.c: 유일한 직접 호출자. update_rusage_stat()/clear_rusage_stat() 에서
 *   각 잡의 thread_data::ts.ru_start/ru_end 를 갱신하기 위해 호출.
 * - getrusage.h: 본 파일이 제공하는 fio_getrusage() 의 함수 프로토타입 선언.
 * - configure/config-host.h: CONFIG_RUSAGE_THREAD 매크로 제공 여부에 따라
 *   Linux 전용 경로를 컴파일 타임에 포함/제외.
 * - 커널: getrusage(2)→sys_getrusage 경로에서 task_struct 또는 signal_struct 의
 *   누적 통계(utime, stime, maj_flt, min_flt, nvcsw, nivcsw, ...)를 복사.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_getrusage(struct rusage *ru): RUSAGE_THREAD→EINVAL 폴백→RUSAGE_SELF 의
 *   2단 시도 래퍼. 반환: 0=성공/-1=실패. 실패 시 errno 는 getrusage(2) 의 값.
 * - 본 파일은 자체 구조체를 정의하지 않으며 <sys/resource.h> 의 struct rusage 를 사용.
 *   ru_utime/ru_stime(시분/마이크로초 timeval)  사용자/시스템 CPU 시간
 *   ru_nvcsw/ru_nivcsw                          자발적/비자발적 컨텍스트 스위치
 *   ru_minflt/ru_majflt                         마이너/메이저 페이지 폴트
 *   ru_maxrss                                   최대 RSS(KB) — Linux 에선 피크 값
 *   ru_inblock/ru_oublock                       (과거 호환) 블록 I/O 수; Linux 최신은 0
 */
#include <errno.h>             /* [한국어] errno 와 EINVAL 매크로 공급 — RUSAGE_THREAD 를 알아보지 못하는 커널은 getrusage(2) 가 -1/EINVAL 로 회신 */
#include "getrusage.h"          /* [한국어] fio_getrusage() 프로토타입 및 struct rusage 간접 include (<sys/resource.h>) */

/*
 * [한국어]
 * fio_getrusage - 스레드 또는 프로세스의 누적 리소스 사용량을 조회한다.
 *
 * @ru: 호출자가 소유한 struct rusage 버퍼 포인터. 성공 시 커널이 채움. NULL 금지.
 * @return: 0=성공(ru 유효), -1=실패(errno 는 getrusage(2) 규약 — EFAULT/EINVAL 등).
 *
 * 동작 과정:
 *  1) CONFIG_RUSAGE_THREAD 가 정의된 빌드(주로 Linux 2.6.26+, glibc 2.7+)에서는
 *     먼저 RUSAGE_THREAD 로 "현재 스레드만" 집계된 값을 요청한다.
 *  2) 성공(0 반환)하면 즉시 0 반환.
 *  3) 실패이고 errno != EINVAL 이면(예: EFAULT — 잘못된 포인터) 복구 불가이므로 -1 반환.
 *  4) errno == EINVAL 이면 커널이 RUSAGE_THREAD 를 모르는 예전 버전으로 판단,
 *     RUSAGE_SELF(프로세스 전체)로 재시도한다 — 이것이 최종 반환값이 된다.
 *  5) 빌드 타임에 CONFIG_RUSAGE_THREAD 가 없으면(Windows/BSD 등) 곧바로 RUSAGE_SELF.
 *
 * 실행 컨텍스트: 잡 스레드(pthread_create 로 생성된 러너) 또는 fork 자식 프로세스.
 *               재진입 안전 — 커널이 per-task 데이터를 사용하며 로컬 상태 없음.
 *
 * 호출 체인: stat.c update_rusage_stat()/clear_rusage_stat() → [fio_getrusage] → getrusage(2).
 *           → 커널 sys_getrusage() → task_struct::{utime, stime, nvcsw, nivcsw, ...} 복사.
 *
 * 에러 처리: 드물게 발생. EFAULT 는 ru 포인터가 유저 공간에 매핑되지 않은 경우;
 *           호출자는 -1 반환 시 errno 를 확인하고 stat 기록을 건너뛰거나 0 으로 처리.
 */
int fio_getrusage(struct rusage *ru)
{
#ifdef CONFIG_RUSAGE_THREAD
	/* [한국어] Linux 확장 RUSAGE_THREAD=1 로 현재 스레드만의 usage 를 요청.
	 * 성공 시 ru 에 해당 스레드의 utime/stime/ctxsw 등이 누적 값으로 채워진다 */
	if (!getrusage(RUSAGE_THREAD, ru))
		return 0;
	/* [한국어] EINVAL 이외의 에러(EFAULT 등)는 의미 있는 폴백 전략이 없으므로 -1 전파 */
	if (errno != EINVAL)
		return -1;
	/* Fall through to RUSAGE_SELF */
	/* [한국어] EINVAL 인 경우만 RUSAGE_THREAD 미지원으로 간주하고 SELF 경로로 폴백 */
#endif
	/* [한국어] 폴백 경로: RUSAGE_SELF=0 — 호출 프로세스 전체(모든 스레드 포함)의 누적 값.
	 * fio 가 process-per-job 모드이면 이 값이 잡 단위와 1:1 매핑되므로 정확함 */
	return getrusage(RUSAGE_SELF, ru);
}
