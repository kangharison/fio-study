/*
 * [한국어 설명] fio 내장 memcpy 벤치마크 공개 엔트리 헤더 (memcpy.h)
 *
 * === 파일의 역할 ===
 * fio 서브커맨드 `--memcpy-test[=type]` 을 구현하는 lib/memcpy.c 의 유일한
 * 공개 API 인 `fio_memcpy_test(type)` 의 프로토타입만 노출하는 헤더이다. 본
 * 파일은 구조체/매크로/열거형을 일절 선언하지 않고, fio.c/main.c 등
 * 최상위 드라이버가 type 문자열(예: "help", "list", "memcpy", "memcpy,hybrid")
 * 을 받아 단 한 줄 호출로 벤치마크를 실행할 수 있게 해 주는 경계 역할만
 * 한다. 프로젝트 내 다른 모듈이 memcpy.c 의 내부 테스트 테이블이나 타이밍
 * 함수에 직접 접근하지 못하도록 캡슐화 경계를 이 헤더가 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 실행 경로 중 "본격 I/O 루프 진입 이전에 실행되는 사전 진단/벤치마크"
 * 계층에 속한다. main() → parse_cmd_line() 이 `--memcpy-test=<type>` 을
 * 발견하면 즉시 fio_memcpy_test() 를 호출하고, 반환값을 프로세스 종료 코드로
 * 삼는다(0=성공, 1=타입 미지원). 벤치마크가 끝나면 fio 는 일반 I/O 잡을
 * 시작하지 않고 즉시 종료된다 — 즉 이 헤더가 가리키는 함수는 "독립 서브커맨드"
 * 처럼 동작한다.
 * 호출 체인:
 *   main() [fio.c]
 *     → parse_cmd_line() [init.c]
 *         → fio_memcpy_test(type)  [본 파일이 선언, memcpy.c 가 정의]
 *             → setup_tests / usec_spin / 각 memcpy 구현 / free_tests
 *
 * === 타 모듈과의 연결 ===
 * - memcpy.c : 본 헤더가 선언하는 단일 함수의 구현체. 테스트 테이블, 버퍼
 *   할당, 타이밍 측정을 모두 포함한다.
 * - init.c / fio.c : 이 헤더를 포함하여 CLI 파싱 결과에 따라 호출.
 * - lib/rand.h, gettime.c : memcpy.c 가 내부적으로 사용(버퍼 초기화, 시간 측정).
 *   이 헤더는 해당 의존성을 외부로 노출하지 않는다(캡슐화 원칙).
 * 데이터 흐름: fio CLI 문자열 → fio_memcpy_test() → stdout 에 "type | block
 * size | MiB/sec" 테이블 출력 → 프로세스 종료. fio 의 I/O 엔진/검증 경로와는
 * 공유 상태를 가지지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_memcpy_test(const char *type) : 유일한 공개 API. type 이 NULL 이면
 *   기본 집합("memcpy,memmove,simple,hybrid") 전체를 수행하고, "help"/"list"
 *   이면 지원 타입 목록만 출력 후 종료한다. 반환 0=성공, 1=알 수 없는 타입.
 */
#ifndef FIO_MEMCPY_H
#define FIO_MEMCPY_H
/* [한국어] 중복 포함 가드. 다른 헤더가 여러 경로로 간접 포함하더라도 한 번만
 * 처리되도록 한다 — 함수 "선언" 중복은 허용되지만 혹시 있을 future 매크로/
 * 인라인 정의 충돌을 사전 차단하기 위한 표준 패턴. */

/*
 * [한국어]
 * fio_memcpy_test - 지정된 memcpy 구현들의 대역폭을 벤치마크한다.
 *
 * @type: CLI `--memcpy-test=` 에 전달된 문자열(NULL/"help"/"list"/타입 리스트).
 *        "memcpy,hybrid" 처럼 쉼표 구분 복수 지정 가능. 대소문자 구분.
 * @return: 0 성공(모든 지정 테스트가 인식되고 실행됨), 1 실패(미지원 타입 포함).
 *          main() 에서 exit code 로 그대로 사용 — 자동화 스크립트의 성공 판정.
 *
 * 벤치마크는 32MiB src/dst 버퍼를 8B ~ 512KiB 11 단계 블록 크기로 64회 반복
 * 복사하며 MiB/s 를 stdout 에 출력한다. fio 본 I/O 실행 전에 시스템의 순수
 * 메모리 복사 대역폭을 기준선으로 측정하는 진단용 경로이다. 호출자는 벤치마크
 * 후 프로세스를 즉시 종료해야 한다(fio_memcpy_test 자체는 종료하지 않음).
 *
 * 실행 컨텍스트: fio 메인 스레드(옵션 파싱 직후), I/O 엔진/검증 스레드와 무관.
 * 호출 체인: main() → parse_cmd_line() → [이 함수] → memcpy.c 내부 테이블 루프.
 */
int fio_memcpy_test(const char *type);
/* [한국어] const char * : 호출자 소유 문자열을 읽기 전용으로 넘김. NULL 허용.
 * 반환 int : exit code 와 호환되도록 0/1 양의 작은 정수만 사용. */

#endif
/* [한국어] FIO_MEMCPY_H 헤더 가드 종료. */
