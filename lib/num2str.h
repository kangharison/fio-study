/*
 * [한국어 설명] 숫자→사람이 읽기 쉬운 문자열 변환 공개 API 헤더 (num2str.h)
 *
 * === 파일의 역할 ===
 * lib/num2str.c 가 구현하는 두 공개 함수 `num2str` (SI/IEC 접두어 + 단위
 * 문자열 생성) 과 `bytes2str_simple` (바이트 수를 "1.23MiB" 류 간결 문자열로)
 * 의 프로토타입과, num2str 의 단위 열거 `enum n2s_unit` 을 노출한다. fio 의
 * 출력/통계/요약 코드 전반에서 raw 64 비트 숫자(바이트, 초당 바이트, 비트,
 * IOPS 등) 를 "1.23MiB/s" 처럼 사용자가 읽을 수 있는 포맷으로 바꾸는 허브
 * 역할을 한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 stat.c / diskutil.c / server.c 등 "출력" 계층의 필수 의존 헤더이다.
 * show_thread_status_normal 은 한 잡의 끝에서 bw/iops/slat/clat/lat 등을
 * num2str 로 변환하여 normal 포맷 출력 라인을 구성하고, fio_memcpy_test 의
 * MiB/sec 출력도 같은 경로를 쓴다.
 * 호출 체인:
 *   stat.c show_* → num2str(value, maxlen, base, pow2, N2S_BYTEPERSEC)
 *     → 반환 문자열(asprintf 으로 할당) → log_info 로 출력
 *
 * === 타 모듈과의 연결 ===
 * - num2str.c : 구현. SI(10^3) vs IEC(2^10) 분기, base 사전 정규화, 자리올림.
 * - stat.c / diskutil.c / memcpy.c / server.c : 사용자 측.
 * - <inttypes.h> (본 헤더가 포함) : uint64_t.
 * 데이터 흐름: 정수 값 → num2str → 힙에 asprintf 된 문자열 → 호출자가 출력
 * 후 free(). bytes2str_simple 은 호출자 제공 버퍼에 기록하여 할당을 피함.
 *
 * === 주요 함수/구조체 요약 ===
 * - enum n2s_unit : 단위 종류(NONE/PERSEC/BYTE/BIT/BYTEPERSEC/BITPERSEC).
 *   BIT 계열은 바이트 값을 8 배 곱해서 표시하는 정책(네트워크 표기 관습).
 * - num2str(val, maxlen, base, pow2, unit) : 힙에 접두어 포함 문자열 할당.
 *   base=0 은 자동, pow2=1 이면 IEC(KiB/MiB), pow2=0 이면 SI(KB/MB).
 * - bytes2str_simple(buf, bufsize, bytes) : 호출자 버퍼에 "1.23MiB" 포맷 기록.
 */
#ifndef FIO_NUM2STR_H
#define FIO_NUM2STR_H
/* [한국어] 헤더 가드. 출력 경로의 다수 소스에서 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t 고정폭 정수. num2str/bytes2str_simple 의
 * 입력 타입이 uint64_t 여야 64 비트 바이트 카운트(TiB 스케일) 를 오버플로
 * 없이 표현 가능. */

enum n2s_unit {
	N2S_NONE	= 0,
	/* [한국어] 단위 없음 — num2str 이 숫자/접두어만 출력하고 단위 문자는
	 * 붙이지 않음. 예: IOPS 처럼 단위 접미가 별도 관리되는 경우 사용.
	 * 설정자: 호출자. 읽는 자: num2str 내부. 값 범위: 0 고정. */

	N2S_PERSEC	= 1,
	/* [한국어] "/s" 접미만 붙음. 접두어(K/M/G) 는 base 에 맞춰 붙되 단위
	 * 이름이 없는 경우(IOPS/s 등). */

	N2S_BYTE	= 2,
	/* [한국어] "B" — 단순 바이트 수. pow2=1 이면 KiB/MiB/GiB/TiB, pow2=0
	 * 이면 KB/MB/GB/TB 접두어와 결합. */

	N2S_BIT		= 3,
	/* [한국어] "bit" — 값에 내부적으로 8 을 곱해 비트 단위로 표시. 네트워크
	 * 엔진의 링크 속도 표기처럼 비트 단위 리포트에 사용. */

	N2S_BYTEPERSEC	= 4,
	/* [한국어] "B/s" — 초당 바이트. bw 리포트의 기본 단위. */

	N2S_BITPERSEC	= 5,
	/* [한국어] "bit/s" — 초당 비트. N2S_BIT 처럼 값에 8 을 곱해 표시. */
};

extern char *num2str(uint64_t, int, int, int, enum n2s_unit);
/* [한국어]
 * num2str - 값을 접두어/단위가 붙은 사람-친화 문자열로 변환.
 *
 * @arg1 val : 변환할 64 비트 정수.
 * @arg2 maxlen : 소수점 이하 최대 자릿수(예: 4 → "1.2345").
 * @arg3 base : 입력 값의 단위 스케일(0=기본 1, 2 / 3 은 반올림/자리올림 정책).
 * @arg4 pow2 : 1 = IEC 접두어(2^10 스케일, KiB/MiB), 0 = SI(10^3 스케일, KB/MB).
 * @arg5 unit : N2S_* 중 하나로 접미 단위 문자열을 결정.
 * @return : asprintf 로 힙에 할당된 C 문자열. 호출자가 free() 해야 함. NULL
 *           반환 시 할당 실패.
 *
 * 실행 컨텍스트: 주로 잡 종료 시점의 stat 출력(메인 스레드). 할당을 동반
 * 하므로 tight loop 에서 호출 금지.
 */

extern const char *bytes2str_simple(char *buf, size_t bufsize, uint64_t bytes);
/* [한국어]
 * bytes2str_simple - bytes 를 호출자 버퍼에 "1.23MiB" 류로 기록.
 *
 * @buf : 출력용 호출자 버퍼.
 * @bufsize : buf 크기(snprintf 에 전달).
 * @bytes : 변환 대상 바이트 수.
 * @return : buf 포인터(편의상 같은 포인터를 돌려줌).
 *
 * 힙 할당이 없으므로 tight loop 에서도 안전. IEC 접두어(KiB/MiB/GiB/TiB) 고정.
 */

#endif
/* [한국어] FIO_NUM2STR_H 가드 종료. */
