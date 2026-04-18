/*
 * [한국어 설명] 2의 거듭제곱으로 올림하는 인라인 유틸 헤더 (roundup.h)
 *
 * === 파일의 역할 ===
 * 정수 값 depth 가 주어졌을 때 "depth 이상인 가장 작은 2 의 거듭제곱" 을
 * 반환하는 `roundup_pow2(depth)` 단일 인라인 함수를 노출한다. fio 내부에는
 * 링 버퍼(io_u_freelist, gettime 버퍼), 해시 테이블(filehash, file_lookup),
 * 큐 깊이(iodepth, iodepth_low) 등 2 의 거듭제곱 크기를 요구하는 다수의
 * 자료구조가 있고, 사용자가 임의 크기를 입력하더라도 구현이 2 의 거듭제곱
 * 경계로 맞출 수 있도록 하는 공통 매크로 성격의 함수이다. 구현은 fls.h 의
 * __fls() 에 의존하며, __fls(depth-1) 로 최상위 비트 위치를 얻어 1UL 을
 * 좌 시프트하는 한 줄 로직이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 "크기 정규화" 유틸 계층에 속한다. 호출자는 자료구조 초기화 시점
 * (대개 backend.c init_io_u, filehash.c init_file_hash, iolog.c 링버퍼 초기화,
 * helper_thread 의 큐 설정 등) 에 사용자가 준 iodepth 나 num_buckets 를
 * 2 의 거듭제곱으로 정렬해 내부 마스크 연산(val & (size-1)) 을 쓸 수 있게
 * 한다.
 * 호출 체인:
 *   backend.c init_io_u / iolog.c / filehash.c 등
 *     → roundup_pow2(user_value)  [본 파일 인라인]
 *         → __fls(user_value - 1) [fls.h 인라인]
 *
 * === 타 모듈과의 연결 ===
 * - lib/fls.h : __fls() 인라인의 정의 공급. 본 파일에서 #include 하여 인라인
 *   재사용.
 * - backend.c / iolog.c / filehash.c 등 : 사용자 측.
 * 데이터 흐름: 입력 정수 → __fls 로 MSB 위치 계산 → 1UL 시프트 → 출력.
 * 순수 함수(부작용 없음) — 동시성/락/공유 상태 없음.
 *
 * === 주요 함수/구조체 요약 ===
 * - roundup_pow2(depth) : depth ≤ 결과 인 최소 2^k 반환. 입력 0 은 정의되지
 *   않음(depth-1 이 언더플로로 0xFF.. → __fls=32 → 2^32 이 반환되지만 호출자
 *   측에서 사용 금지). 입력이 이미 2 의 거듭제곱이면 같은 값 반환.
 */
#ifndef FIO_ROUNDUP_H
#define FIO_ROUNDUP_H
/* [한국어] 헤더 가드. 본 헤더는 static inline 을 노출하므로 중복 포함 시
 * 재정의 위험은 없으나, fls.h 간접 포함 경로가 여럿이라 관례에 따라
 * 가드를 둔다. */

#include "lib/fls.h"
/* [한국어] "lib/fls.h" : __fls(int) 인라인 함수의 정의를 제공. 본 파일의
 * roundup_pow2 구현이 직접 의존. 포함 경로가 "lib/fls.h" 인 것은 fio 의
 * Makefile 이 최상위를 -I 경로로 삼기 때문(즉 lib/roundup.h 를 포함하는
 * 소스는 lib/ 와 ./ 양쪽에서 온다). */

/*
 * [한국어]
 * roundup_pow2 - depth 이상인 가장 작은 2 의 거듭제곱을 반환.
 *
 * @depth: 정규화 대상 값(unsigned). 1 이상 권장. 0 은 정의되지 않음.
 * @return: depth ≤ 결과 인 최소 2^k 값(unsigned long).
 *
 * 예시: roundup_pow2(1)=1, roundup_pow2(2)=2, roundup_pow2(3)=4,
 *      roundup_pow2(1024)=1024, roundup_pow2(1025)=2048.
 *
 * 사용처: iodepth, 해시 테이블 버킷 수, 링 버퍼 크기 등 2 의 거듭제곱을
 * 요구하는 자료구조의 초기 크기 결정. 결과가 2^k 이면 mod N 을
 * "val & (N-1)" 로 대체할 수 있어 분기 없이 인덱싱 가능.
 *
 * 실행 컨텍스트: 인라인으로 호출자 코드에 직접 삽입. 순수 함수로 스레드
 * 안전. 호출 체인: 호출자 → __fls(depth-1) → (1UL << r) 반환.
 */
static inline unsigned roundup_pow2(unsigned depth)
{
	/* [한국어] depth-1 의 최상위 비트 위치 r 을 구한 뒤 1UL << r 을 반환.
	 * __fls(n) 은 n 의 최상위 1 비트 "1 기반 위치"(__fls(1)=1, __fls(2)=2,
	 * __fls(8)=4) 를 돌려주므로 결과적으로 2^r = 2^__fls(depth-1) 가 되어
	 * "depth-1 의 MSB 위치 + 1" 의 2 의 거듭제곱 = depth 이상 최소 2^k. */
	return 1UL << __fls(depth - 1);
}

#endif
/* [한국어] FIO_ROUNDUP_H 가드 종료. */
