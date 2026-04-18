/*
 * [한국어 설명] IEEE 754 수동 패킹/언패킹 및 fio_fp64_t 공개 헤더 (ieee754.h)
 *
 * === 파일의 역할 ===
 * lib/ieee754.c 가 수동으로 구현한 IEEE 754 부동소수점 ↔ 정수 비트 패턴
 * 변환 함수(`pack754`/`unpack754`) 프로토타입과, 그 함수들을 특정 비트폭
 * (double = 64비트, 지수 11비트) 으로 바인딩한 두 편의 매크로
 * (`fio_double_to_uint64`/`fio_uint64_to_double`), 그리고 fio 의 서버/클라이언트
 * 프로토콜에서 double 값을 네트워크/디스크 직렬화할 때 쓰는 16 바이트
 * 공용체 `fio_fp64_t` 를 선언한다. 구현은 Beej's Guide 의 퍼블릭 도메인
 * 코드에서 유래했으며, 호스트의 hardware float 표현이나 엔디안에 의존하지
 * 않도록 수동으로 bias/significand/sign 을 조립한다. 이는 fio 가 서로 다른
 * 아키텍처(x86 / ARM / POWER / s390) 간에 부동소수점 통계를 무손실로
 * 주고받기 위한 핵심 이식성 계층이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "네트워크 프로토콜/통계 직렬화" 계층. server.c 의 convert_io_stat,
 * convert_gs, fio_server_send_ts 가 jobs 의 평균/표준편차/퍼센타일을
 * uint64_t 비트 패턴으로 변환하여 little-endian 으로 전송하고, client.c 가
 * 수신측에서 역변환한다. 디스크에 기록되는 히스토그램 로그도 같은 경로를
 * 쓴다.
 * 호출 체인:
 *   server.c convert_io_stat → fio_double_to_uint64(val) = pack754(val,64,11)
 *     → cpu_to_le64 → writev(sock)
 *   client.c → recvfrom → le64_to_cpu → fio_uint64_to_double = unpack754(..,64,11)
 *
 * === 타 모듈과의 연결 ===
 * - ieee754.c : 구현(비트 단위 수동 조립).
 * - server.c / client.c / stat.c : 사용자 측.
 * - <inttypes.h> (본 헤더가 포함) : uint8_t/uint64_t.
 * 데이터 흐름: double (CPU FP unit) → pack754 (순수 정수 비트 연산) → uint64_t
 *   → 네트워크/디스크 → uint64_t → unpack754 → double.
 *
 * === 주요 함수/구조체 요약 ===
 * - pack754(f, bits, expbits) : long double f 를 (bits 비트 전체, expbits 지수)
 *   포맷으로 패킹. 일반적으로 (64, 11) = double.
 * - unpack754(i, bits, expbits) : 반대 방향.
 * - fio_double_to_uint64 / fio_uint64_to_double : (64, 11) 매크로 바인딩.
 * - fio_fp64_t : 16 바이트 정렬 공용체. u.i(정수), u.f(double), u.filler(패딩).
 *   프로토콜 필드 정렬/버전 호환을 위해 16 바이트 공간을 예약.
 */
#ifndef FIO_IEEE754_H
#define FIO_IEEE754_H
/* [한국어] 헤더 가드. server.c / client.c / stat.c 에서 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t, uint8_t 고정폭 타입. filler[16] 과
 * pack754 반환/입력 타입에 필수. */

extern uint64_t pack754(long double f, unsigned bits, unsigned expbits);
/* [한국어] pack754 - 부동소수점 값 f 를 수동으로 IEEE 754 비트 패턴 정수로
 * 패킹. bits 는 전체 비트 폭(32/64/80), expbits 는 지수부 비트(8/11/15).
 * 특별 값 f==0.0 은 0 으로 빠르게 반환. 일반 값은 frexp 대신 수동 로그/
 * 시프트를 써서 표준 라이브러리 의존도를 낮춘다. 무한대/NaN 은 특별한
 * 처리 없이 bias 오버플로로 귀결되므로 호출자가 사전 체크 권장.
 * 반환 : 비트 폭이 64 미만이면 상위는 0 채움. */

extern long double unpack754(uint64_t i, unsigned bits, unsigned expbits);
/* [한국어] unpack754 - pack754 의 역연산. 정수 비트 패턴 i 를 long double 로
 * 복원. 0 패턴은 0.0 으로 빠르게 반환. bias 복원 → significand 조립 →
 * 2^(e-bias) * (1 + frac) 계산. */

#define fio_double_to_uint64(val)	pack754((val), 64, 11)
/* [한국어] (64, 11) = IEEE 754 double 포맷. bits=64 전체, expbits=11, mantissa
 * =52. fio 의 네트워크 프로토콜/히스토그램 로그의 double 필드를 직렬화할
 * 때 호출. 예: fp = 123.456 → 0x405EDD2F1A9FBE77 류 패턴. */

#define fio_uint64_to_double(val)	unpack754((val), 64, 11)
/* [한국어] 위의 역변환. 수신측/읽기측에서 uint64_t 패턴을 double 로 복원. */

typedef struct fio_fp64 {
	union {
		uint64_t i;
		/* [한국어] 정수 표현 — 네트워크/디스크 직렬화. cpu_to_le64/
		 * le64_to_cpu 를 적용 후 전송/수신. 읽는 자: 전송 함수. 설정자:
		 * convert_io_stat 등이 fio_double_to_uint64(double) 결과를 저장. */

		double f;
		/* [한국어] CPU 네이티브 부동소수점 표현. 잡 실행 중 평균/표준편차
		 * 누적 시 직접 접근. 설정자: stat.c 의 계산 함수. 읽는 자: 출력 경로. */

		uint8_t filler[16];
		/* [한국어] 16 바이트 정렬/예약 공간. struct 크기를 16 바이트로 고정
		 * 하여 프로토콜 필드의 향후 확장(long double 80/128 비트) 호환을
		 * 확보한다. 직접 읽지 않음. */
	} u;
} fio_fp64_t;
/* [한국어] struct fio_fp64 : 프로토콜/통계용 16 바이트 double 컨테이너.
 * 설정자/읽는 자: 위 각 필드 주석 참조.
 * 값 범위: double 정상 값. NaN/Inf 는 비트 패턴으로는 전달되지만 의미
 *   해석 책임은 호출자.
 * 동기화: 잡 스레드 내에서 누적되고, stat 수집 중 한 번에 읽혀 복사되므로
 *   별도 락이 필요 없다(stat_sem 스코프 내). */

#endif
/* [한국어] FIO_IEEE754_H 가드 종료. */
