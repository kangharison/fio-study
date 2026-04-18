/*
 * [한국어 설명] 2의 거듭제곱 판별 인라인 유틸 헤더 (pow2.h)
 *
 * === 파일의 역할 ===
 * 입력된 64 비트 정수 val 이 2 의 거듭제곱(1, 2, 4, 8, ..., 2^63) 인지 bool
 * 로 반환하는 `is_power_of_2(val)` 단일 인라인 함수를 노출한다. 판별은
 * 비트 트릭 `val != 0 && (val & (val-1)) == 0` 을 사용해 분기 없이 O(1) 로
 * 수행된다. 0 은 2 의 거듭제곱이 아닌 것으로 취급한다(수학적으로는 2^k 가
 * 0 이 되는 정수 k 가 없으므로 타당). fio 의 옵션 검증 코드에서 블록 크기,
 * 정렬, iodepth 등 "2 의 거듭제곱만 허용" 조건을 일관되게 표현하는 데 사용.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 옵션 파싱(options.c) / 잡 초기화(fixup_options) / I/O 엔진(특히 sg, nvme,
 * io_uring passthru) 의 "입력 유효성 검사" 계층에 속한다. 예를 들어 NVMe
 * 디바이스의 LBA 크기는 512 / 1024 / 4096 등 2 의 거듭제곱만 유효하므로,
 * 엔진 init 경로에서 is_power_of_2(bs) 로 선행 검증 후 0 이면 에러 메시지
 * 와 함께 잡을 거부한다. stat.c 의 히스토그램 버킷 크기도 2 의 거듭제곱
 * 전제를 갖는다.
 * 호출 체인:
 *   options.c fixup_options → is_power_of_2(bs/align/iodepth)
 *   engines/sg.c / nvme.c / io_uring.c init → is_power_of_2(sector_size)
 *   stat.c plat_val_to_idx (PLAT_BITS=6 이므로 권/간접 확인)
 *
 * === 타 모듈과의 연결 ===
 * - lib/types.h : bool 타입 정의를 위해 포함.
 * - <inttypes.h> : uint64_t 고정폭 정수 타입.
 * 데이터 흐름: 입력 64비트 정수 → 비트 트릭 판별 → bool 반환. 순수 함수.
 *
 * === 주요 함수/구조체 요약 ===
 * - is_power_of_2(val) : val 이 양의 2 의 거듭제곱이면 true, 아니면 false.
 *   내부 연산: (val != 0) && ((val & (val-1)) == 0). 동작 원리: 2^k 는
 *   이진수로 단 하나의 1 비트만 가지고, 그 값에서 1 을 빼면 하위 모든 비트
 *   가 1 이 되어 원본과 AND 결과가 0.
 */
#ifndef FIO_POW2_H
#define FIO_POW2_H
/* [한국어] 헤더 가드. options.c, fixup, stat.c, 각종 엔진에서 포함 중복 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t 고정폭 정수 타입. 플랫폼의 long 크기와
 * 무관하게 64 비트를 명시적으로 쓰기 위해 필요. */

#include "types.h"
/* [한국어] "types.h" : fio 의 bool/true/false 폴백 정의. C89 환경이나
 * <stdbool.h> 없는 컴파일러에서도 is_power_of_2 의 반환 타입 bool 을
 * 사용할 수 있도록 보장. */

/*
 * [한국어]
 * is_power_of_2 - val 이 2 의 거듭제곱(1, 2, 4, ..., 2^63) 인지 판별.
 *
 * @val: 검사할 64 비트 부호 없는 정수(unsigned).
 * @return: val 이 양의 2 의 거듭제곱이면 true, 그 외(0 포함)는 false.
 *
 * 원리: 2^k 는 이진 표현에서 단 하나의 1 비트를 가진다. val-1 은 그
 * 비트 위치 아래로 모든 비트가 1 이 되고 원래 1 비트는 0 이 된다. 따라서
 * val & (val-1) 은 0 이 되어야 한다. 단 val=0 은 (0 & 0xFFFF...FF) = 0 이
 * 되어 거짓 양성이 되므로 val != 0 을 별도 조건으로 둔다.
 *
 * 실행 컨텍스트: 호출자 스레드에서 인라인 삽입. 순수 함수로 스레드 안전.
 * 호출 체인: options/engines init → [이 함수] → (결과에 따라 잡 수락/거부).
 */
static inline bool is_power_of_2(uint64_t val)
{
	/* [한국어] val 이 0 이 아니고, 최하위 1 비트 클리어 연산(val & (val-1))
	 * 결과가 0 이면 val 은 2^k. 분기 예측 친화적인 단일 논리 표현식. */
	return (val != 0 && ((val & (val - 1)) == 0));
}

#endif
/* [한국어] FIO_POW2_H 가드 종료. */
