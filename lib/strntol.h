/*
 * [한국어 설명] 길이 제한 문자열→long 변환 strntol() 공개 헤더 (strntol.h)
 *
 * === 파일의 역할 ===
 * 표준 C 의 `strtol(str, endptr, base)` 는 NUL 종결 문자열을 가정하지만, fio 의
 * pattern 파싱(`--verify_pattern=`, `--buffer_pattern=`)처럼 "사용자 입력
 * 문자열의 일부 구간만" 을 숫자로 해석해야 하는 상황에서는 구간 끝을 길이로
 * 제한해야 한다. 본 헤더는 그 전용 API 인 `strntol(str, sz, end, base)` 의
 * 유일한 선언을 노출한다. 구조체/매크로는 두지 않고, stdint.h 를 포함해
 * size_t 정의만 간접 가용하게 한다(정확히는 stddef 소속이나 stdint 가
 * 간접 제공하는 구현이 일반적). 본 파일은 strntol.c 의 구현을 외부에 노출하는
 * 캡슐화 경계이며, NUL 이 보장되지 않는 임의 길이 하위 문자열을 안전하게
 * 숫자로 변환하는 책임의 계약(contract) 을 정의한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 옵션 파싱/패턴 파서 계층에 속한다. pattern.c 의 parse_number() 는
 * 사용자가 입력한 패턴 문자열(예: `-1024"abc"0xdeadbeef`)을 좌→우로 순회
 * 하면서 10진수 토큰 경계를 만나면 strntol() 을 호출해 해당 구간만 long 으로
 * 해석한다. 즉 이 헤더의 함수는 fio 의 "문자열 → 바이너리 패턴 바이트"
 * 파이프라인의 낮은 층 원시 유틸리티이다.
 * 호출 체인:
 *   options.c (str_verify_pattern_cb/str_buffer_pattern_cb)
 *     → parse_and_fill_pattern_alloc / parse_and_fill_pattern [pattern.c]
 *         → parse_number()
 *             → strntol(str, end - str, &endptr, 10)  [본 파일이 선언]
 *
 * === 타 모듈과의 연결 ===
 * - strntol.c : 본 헤더의 단일 함수 구현체. 24B 스택 버퍼에 복사 → NUL 종결
 *   → strtol() 호출 → 원 입력의 endptr 을 재매핑하는 3단계 이디엄 사용.
 * - pattern.c : 유일한 실사용자. 10진수 구간을 long 으로 해석.
 * - <stdint.h> (본 헤더가 포함) : size_t 및 고정폭 정수 타입 정의 공급.
 * 데이터 흐름: 입력 문자열 조각(str, sz) → strntol() → long 반환값 + endptr
 *   재매핑 → 호출자가 반환 long 을 패턴 버퍼에 little-endian 으로 바이트 기록.
 *
 * === 주요 함수/구조체 요약 ===
 * - strntol(str, sz, end, base) : NUL 이 없는 sz 바이트 문자열을 base 진수
 *   long 으로 변환. 24B 스택 복사본을 만든 뒤 strtol 호출 → 원본 endptr 위치
 *   로 재매핑. sz 가 23 을 초과하면 잘려 상위 자리가 반영되지 않을 수 있음 —
 *   LONG_MIN/MAX 의 10진 표기가 최대 20자이므로 부호/종결 포함 24B 로 충분.
 */
#ifndef FIO_STRNTOL_H
#define FIO_STRNTOL_H
/* [한국어] 헤더 가드. pattern.c 등에서 포함될 때 다중 포함 방지. */

#include <stdint.h>
/* [한국어] <stdint.h> : uint32_t, int64_t 등 고정폭 정수 타입 정의. 본 선언
 * 자체는 long/size_t/char** 만 쓰지만, 과거 구현에서 size_t 를 stdint 를
 * 통해 간접 제공받던 관례를 유지. size_t 는 엄밀히는 <stddef.h> 소속이나
 * 대부분의 libc 에서 stdint 가 재노출한다. */

/*
 * [한국어]
 * strntol - NUL 종결이 보장되지 않는 sz 바이트 문자열을 long 으로 변환.
 *
 * @str:  변환 대상 문자열의 시작 포인터(호출자 소유, 읽기 전용).
 * @sz:   변환 가능한 최대 바이트 수(문자열 내 해석 구간의 길이 한계).
 * @end:  (출력) 변환이 멈춘 위치. 호출 후 *end 는 원본 입력 str 기준의
 *        다음 문자 위치를 가리키도록 재매핑된다(내부 스택 버퍼가 아닌 원본).
 * @base: 2~36 진수 또는 0(자동 감지). strtol(3) 과 동일 의미.
 * @return: 해석된 long 값. 오버플로 시 LONG_MAX/MIN + errno=ERANGE 설정.
 *          해석 불가(첫 문자 무효) 시 0 반환 + end == str.
 *
 * 구현은 내부 24B 스택 버퍼에 min(sz, 23) 바이트를 복사 → 복사본에 NUL
 * 붙이기 → strtol(복사본, &tmp_end, base) 호출 → 오프셋 (tmp_end - 복사본)
 * 만큼을 원본 str 에 더해 *end 를 복귀시키는 3단계. 호출자는 end 가 반드시
 * 원본 입력 범위 내 포인터임을 가정할 수 있다.
 *
 * 실행 컨텍스트: fio 메인(파서) 스레드의 옵션 해석 중. 재진입 안전(스택만 사용).
 * 호출 체인: pattern.c parse_number → [이 함수] → libc strtol(3).
 * 에러 경로: 구현에 따라 errno 만 설정하고 부분 변환 결과를 반환. 호출자는
 * end==str 여부로 "전혀 변환 못 함" 을 판정.
 */
long strntol(const char *str, size_t sz, char **end, int base);
/* [한국어] 이 선언이 이 헤더의 유일한 공용 심볼. 함수 자체는 strntol.c 에
 * 정의되며, 링커가 pattern.c 등의 호출부와 연결한다. */

#endif
/* [한국어] FIO_STRNTOL_H 가드 종료. */
