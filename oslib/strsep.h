/*
 * [한국어 설명] strsep 폴리필 헤더 (strsep.h)
 *
 * === 파일의 역할 ===
 * BSD 유래 토큰 분리 함수 strsep(3) 가 시스템에 없을 때를 위한 폴백 선언부이다.
 * strsep 은 strtok 의 재진입 가능(thread-safe) 대안으로:
 *   - 구분자(delim)에서 첫 일치를 찾아 해당 위치에 NUL 을 넣고 토큰 시작을 반환.
 *   - *stringp 를 다음 토큰 시작(또는 NULL)으로 전진.
 *   - 빈 토큰(연속 구분자)도 빈 문자열로 반환 → 필드가 고정된 CSV/명세 기반 파싱에 유리.
 *   - 전역 정적 상태를 쓰지 않아 스레드/재귀 재진입 안전.
 * glibc/musl/BSD 는 기본 제공하지만 구형 시스템 일부에서는 없어 fio configure 의
 * CONFIG_STRSEP 로 판별해 본 헤더 선언을 활성화한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * oslib/ 이식성 레이어. fio 의 옵션 값 파싱(콜론·콤마 구분 리스트), CSV 로그
 * 파싱, URI 분할(engines/nbd.c, engines/http.c, engines/rdma.c), cpu_list 같은
 * 구조화된 사용자 입력 처리 등에서 사용.
 *
 * === 타 모듈과의 연결 ===
 * - oslib/strsep.c: 폴백 구현(strpbrk 이후 NUL 삽입 + 포인터 전진).
 * - configure: CONFIG_STRSEP 정의 여부 결정.
 * - <string.h>: CONFIG_STRSEP 경로에서 표준 선언 대신 사용됨.
 * - options.c, engines/*.c: 옵션·URI 파싱 주 소비자.
 *
 * === 주요 함수/구조체 요약 ===
 * - strsep(stringp, delim): 다음 토큰 시작 포인터 반환, *stringp 전진.
 */
#ifndef CONFIG_STRSEP
/* [한국어] 시스템 미제공 시에만 선언 활성화. */

#ifndef FIO_STRSEP_LIB_H
/* [한국어] 헤더 가드. */
#define FIO_STRSEP_LIB_H

/*
 * [한국어]
 * strsep - 문자열을 구분자 기준으로 토큰 분리(재진입 안전)
 *
 * @stringp: **이중 포인터**. 첫 호출 시 char *str 의 주소를 넘기면 함수가
 *           내부적으로 str 를 토큰 바이 토큰으로 전진시킨다. *stringp == NULL 이면
 *           더 이상 토큰 없음을 의미하고 NULL 반환.
 * @delim:   구분자 후보 바이트들(예: ":,"). 어느 하나라도 일치하면 토큰 경계.
 * @return:  현재 토큰의 시작 포인터(즉시 원본 문자열 내부를 가리키며 구분자
 *           위치에 NUL 이 박힌 상태). 빈 토큰("::" 가운데) 도 빈 문자열로 반환.
 *
 * 동작:
 *   1) *stringp 에서 시작해 delim 중 한 문자 첫 일치 위치 탐색.
 *   2) 일치 위치에 '\0' 삽입, *stringp 를 그 다음 위치로 갱신.
 *   3) 불일치(끝까지 없음) 시 *stringp 를 NULL 로 설정.
 *
 * 호출 체인:
 *   options.c / engines/*.c 의 옵션 콜백 — strsep(&p, ":") 루프로 세그먼트 순회.
 *   engines/libcufile.c::find_gpu_id() → gpu_dev_ids 를 콜론 분리.
 * 실행 컨텍스트: 호출자 따름 — 전역 상태 없어 동시 호출 안전.
 * 에러: 없음(순수 함수).
 */
char *strsep(char **, const char *);

#endif

#endif
