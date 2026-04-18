/*
 * [한국어 설명] 스레드/프로세스 리소스 사용량 조회 래퍼 헤더 (getrusage.h)
 *
 * === 파일의 역할 ===
 * lib/getrusage.c 가 제공하는 `fio_getrusage(ru)` 래퍼의 유일한 선언을 노출
 * 한다. 시스템 콜 getrusage(2) 는 who 인자로 RUSAGE_SELF(프로세스 누적),
 * RUSAGE_CHILDREN(자식 누적), (Linux 한정) RUSAGE_THREAD(호출 스레드만) 중
 * 하나를 받는데, fio 는 잡-스레드 단위로 CPU 시간/컨텍스트 스위치/페이지
 * 폴트를 정확히 수집해야 하므로 RUSAGE_THREAD 를 선호하지만, 플랫폼에 따라
 * 해당 매크로가 없을 수 있다. 본 래퍼는 CONFIG_RUSAGE_THREAD 빌드 매크로로
 * RUSAGE_THREAD 와 RUSAGE_SELF 를 분기해 호출해 주어, 상위 통계 코드
 * (stat.c 의 clear_rusage_stat/update_rusage_stat) 가 OS 차이를 신경 쓰지
 * 않도록 추상화한다. 본 헤더는 struct rusage 타입을 사용 가능하게 하기
 * 위해 <sys/time.h> 와 <sys/resource.h> 를 함께 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 통계 수집(stat.c) 계층의 하위 유틸이다. 잡 스레드 생애주기에서
 * backend.c 의 thread_main() 진입 시 clear_rusage_stat() 이 호출되어 초기
 * rusage 를 기록하고, 잡 종료 직전 update_rusage_stat() 이 다시 호출되어
 * 차이를 thread_stat 에 누적한다. 이 두 호출이 모두 본 헤더를 통해
 * fio_getrusage() 를 호출한다.
 * 호출 체인:
 *   thread_main() [backend.c]
 *     → clear_rusage_stat() [stat.c]  → fio_getrusage(&td->ts.ru_start)
 *     → ... 잡 실행 ...
 *     → update_rusage_stat() [stat.c] → fio_getrusage(&td->ru_end) → 차이 계산
 *
 * === 타 모듈과의 연결 ===
 * - getrusage.c : 본 헤더의 유일한 함수 구현. CONFIG_RUSAGE_THREAD 분기로
 *   RUSAGE_THREAD(Linux) vs RUSAGE_SELF(폴백) 선택.
 * - stat.c : ru_utime/ru_stime 을 msec/usec 단위로 환산하여 jobs usr/sys%
 *   출력에 사용. ru_nvcsw/nivcsw 로 자발/비자발 컨텍스트 스위치, ru_majflt/
 *   minflt 로 major/minor page fault 보고.
 * - <sys/time.h>, <sys/resource.h> (본 헤더가 포함) : struct rusage, struct
 *   timeval 정의 + RUSAGE_* 상수. 호출자가 struct rusage 를 스택에 올려
 *   출력 파라미터로 넘길 수 있게 하기 위해 타입을 반드시 가용해야 함.
 * 데이터 흐름: 커널 → getrusage(2) → struct rusage → stat.c 집계.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_getrusage(ru) : 호출 스레드(가능 시) 혹은 프로세스의 리소스 사용량을
 *   *ru 에 채워 반환. 반환 0=성공, -1=실패(errno 유지).
 */
#ifndef FIO_GETRUSAGE_H
#define FIO_GETRUSAGE_H
/* [한국어] 헤더 가드. stat.c 와 backend.c 에서 포함되어도 중복 선언 방지. */

#include <sys/time.h>
/* [한국어] <sys/time.h> : struct timeval(ru_utime/ru_stime 필드의 타입) 정의.
 * POSIX 구(舊) 표준이 사용자 CPU 시간을 timeval 로 표현하기 때문에 rusage
 * 사용 전 반드시 포함 필요. gettimeofday(2) 프로토타입도 같이 들어오지만
 * 본 헤더 범위에서는 의미 없음. */

#include <sys/resource.h>
/* [한국어] <sys/resource.h> : struct rusage 전체 정의 + RUSAGE_SELF /
 * RUSAGE_CHILDREN / (Linux 한정) RUSAGE_THREAD 상수. getrusage(2) 프로토타입
 * 도 함께 들어온다. 호출자 측에서 struct rusage 를 스택 변수로 선언할 수
 * 있게 하기 위해 반드시 이 헤더를 같이 공급해야 함. */

/*
 * [한국어]
 * fio_getrusage - RUSAGE_THREAD(선호) 혹은 RUSAGE_SELF 로 rusage 를 채운다.
 *
 * @ru: (출력) 호출자가 소유한 struct rusage 버퍼. 성공 시 ru_utime, ru_stime,
 *      ru_nvcsw, ru_nivcsw, ru_majflt, ru_minflt 등이 갱신됨.
 * @return: 0 성공, -1 실패(errno 는 getrusage(2) 규약 유지).
 *
 * CONFIG_RUSAGE_THREAD 가 정의된 Linux 빌드에서는 RUSAGE_THREAD 를 사용해
 * 호출 스레드 단위의 정확한 CPU 시간을 얻는다(fio 의 잡 스레드 통계는
 * 스레드 격리가 필수). 매크로가 없는 플랫폼에서는 RUSAGE_SELF 로 폴백하며,
 * 이 경우 같은 프로세스 내 다른 잡 스레드의 비용이 합쳐져 부정확해질 수
 * 있음을 상위 계층이 감안해야 한다.
 *
 * 실행 컨텍스트: 잡 스레드(thread_main). 재진입 안전(시스템 콜만 호출).
 * 호출 체인: stat.c clear_rusage_stat / update_rusage_stat → [이 함수] →
 * getrusage(2) syscall.
 */
extern int fio_getrusage(struct rusage *ru);
/* [한국어] extern : 정의는 getrusage.c. 컴파일 시 하나의 오브젝트로만 링크. */

#endif
/* [한국어] FIO_GETRUSAGE_H 가드 종료. */
