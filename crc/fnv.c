/*
 * [한국어 설명] FNV (Fowler-Noll-Vo) 해시 구현 (fnv.c)
 *
 * === 파일의 역할 ===
 * FNV-1a 64비트 해시 함수(Fowler/Noll/Vo 알고리즘, IETF draft-eastlake-fnv 참조)를
 * 구현한다. 알고리즘은 매우 단순하다 — 각 바이트(또는 64비트 워드)에 대해
 * "누적값에 FNV_PRIME(=0x100000001B3)을 곱한 뒤 입력을 XOR" 하는 한 단계만 반복한다.
 * FNV-1은 "곱한 후 XOR" 순서가 바뀐 변형이며, 본 구현은 성능 최적화를 위해
 * 바이트 단위가 아니라 uint64_t 단위로 처리한다(표준 FNV-1a는 옥텟 단위라
 * 엄밀히는 다른 해시값이 되지만 fio 내부에서는 일관된 의사난수 해시로만 사용되므로
 * 표준 호환성은 목표가 아니다). 분산 특성은 다른 비-암호 해시에 비해 다소 떨어지지만
 * 구현이 지극히 단순하고 CPU 캐시 미스가 거의 없어 소형 버퍼에 대해 가장 빠르다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio verify 경로(verify.c)의 VERIFY_FNV 체크섬 공급자이다.
 * 쓰기 시 fill_fnv(verify.c) → fnv() [이 파일]가 불려 데이터의 64비트 해시를
 * verify_header.v_fnv 에 저장하고, 읽기 후 verify_io_u_fnv()가 동일 버퍼에 대해
 * fnv()를 재계산한 뒤 헤더 값과 비교하여 데이터 무결성을 검증한다.
 * 또한 crc/test.c(`fio --crctest fnv`) 벤치마크 러너에서 처리량 측정 대상이다.
 * 호출 체인:
 *   backend.c → io_verify_init → verify.c::fill_fnv / verify_io_u_fnv → [fnv] → FNV_PRIME 반복
 *   crc/test.c::t_fnv → [fnv]
 *
 * === 타 모듈과의 연결 ===
 * - fnv.h: 인터페이스(`uint64_t fnv(const void *, uint32_t, uint64_t)`)와 offset basis
 *   상수 FNV_PRIME / FNV_INIT(=0xcbf29ce484222325, 표준 offset basis) 선언.
 * - verify.c: fill_fnv()가 FNV_INIT로 시작해 fnv()를 호출, 결과를 verify_header에 저장.
 *   verify_io_u_fnv()가 같은 방식으로 재계산 후 비교.
 * - crc/test.c: --crctest 벤치마크에서 t_fnv() 래퍼가 NR_CHUNKS회 fnv()를 호출.
 * 데이터 흐름: 쓰기버퍼 → fnv(hval=FNV_INIT) → verify_header.v_fnv (8B). 읽기 시 역계산.
 * 동기화: 순수 계산 함수이며 전역 상태가 없어 재진입 안전(잡 스레드·verify 스레드 병렬
 *   호출 가능). 입력 버퍼는 호출자가 수명을 보장한다.
 *
 * === 주요 함수 요약 ===
 * - fnv(buf, len, hval): 64비트 FNV-1a 해시 계산. hval은 시드(표준이면 FNV_INIT,
 *   chunk 이어붙이기 시 직전 결과를 넘겨 증분 계산 가능). 내부 루프는 먼저
 *   hval *= FNV_PRIME 를 적용하고, 남은 길이가 8바이트 이상이면 한 번에 uint64_t를
 *   XOR하고, 8바이트 미만이면 잔여 바이트를 빅엔디안으로 쌓아 XOR한 뒤 종료.
 *   반환값은 호출자가 verify_header에 저장하거나 다음 chunk 시드로 재사용한다.
 */
#include "fnv.h"
/* [한국어] fnv.h: uint64_t/uint32_t 정수 타입 재노출 및 fnv() 프로토타입 제공.
 * verify.c·crc/test.c 가 동일 헤더를 포함하여 ABI 공유. */

/* [한국어] FNV-1a 64비트 소수(prime) — FNV 해시의 핵심 곱셈 상수.
 * 0x100000001B3 = 2^40 + 2^8 + 0xB3 형태의 저가중치 64비트 소수(IETF draft 지정).
 * 각 바이트에 대한 곱셈 결과가 하위 비트에까지 잘 퍼지도록 선택된 값으로,
 * 이 값을 바꾸면 해시 분포 특성이 무너진다. ULL 접미사로 unsigned long long 폭 보장. */
#define FNV_PRIME	0x100000001b3ULL

/*
 * [한국어]
 * fnv - 64비트 FNV-1a 해시 계산(워드 단위 최적화 버전)
 *
 * @buf:  해시를 계산할 데이터 버퍼(바이트 스트림 해석, 정렬 가정 있음 — uint64_t
 *        캐스팅이 안전하도록 호출자가 보장한다. fio 내부에서는 io_u->xfer_buf가
 *        io_u_init() 단계에서 페이지 정렬된다).
 * @len:  데이터 길이(바이트). 0이면 초기 hval을 그대로 반환(상단 while 진입 실패).
 * @hval: 해시 시드/증분 값. 표준 FNV-1a 의 offset basis 0xCBF29CE484222325 또는
 *        이전 chunk 에서 반환된 값을 넣으면 이어서 해싱할 수 있다.
 * @return: 64비트 FNV-1a 누적 해시. verify_header에 그대로 저장·비교에 사용.
 *
 * 동작 과정:
 *   1) 남은 길이 len 이 0이 될 때까지 반복.
 *   2) 각 반복마다 먼저 hval 을 FNV_PRIME 으로 곱한다(FNV-1a 순서).
 *   3) 길이가 sizeof(uint64_t)=8 이상이면 *ptr 로 8바이트를 한 번에 읽고 XOR 한 뒤
 *      ptr/len 을 8씩 진행 — 워드 단위 경로(본 함수의 성능 핵심).
 *   4) 길이가 8 미만이면 남은 바이트들을 uint8_t* 경로로 빅엔디안 순서로 쌓아
 *      하나의 uint64_t val 에 조립(val = (val<<8) | byte)한 뒤 마지막으로 XOR 하고
 *      break 로 루프 종료.
 *
 * 실행 컨텍스트: 잡 스레드의 verify 경로 또는 crc/test.c 벤치마크 루프. 전역 상태
 * 없음·재진입 안전. 하위 함수 호출 없음(순수 연산).
 *
 * 에러 경로: 없음. len==0 은 hval 그대로 반환, NULL buf 는 보호되지 않으므로
 * 호출자 책임(정상 경로에서는 io_u 버퍼로만 호출).
 *
 * 호출 체인:
 *   verify.c::fill_fnv/verify_io_u_fnv → [fnv] → (없음, 테이블/라이브러리 참조 없음)
 *   crc/test.c::t_fnv → [fnv]
 */
/*
 * 64-bit fnv, but don't require 64-bit multiples of data. Use bytes
 * for the last unaligned chunk.
 */
uint64_t fnv(const void *buf, uint32_t len, uint64_t hval)
{
	/* [한국어] 입력 void* 를 64비트 워드 포인터로 해석 — 8바이트 단위 XOR 경로의 기반.
	 * 호출자가 정렬(자연 정렬된 io_u 버퍼)을 보장해야 strict-alias/얼라인먼트 안전. */
	const uint64_t *ptr = buf;

	/* [한국어] len 만큼 남아있는 동안 반복 — 내부에서 8바이트/잔여 분기로 len 감소. */
	while (len) {
		/* [한국어] FNV-1a 단계 1: 누적 해시에 FNV_PRIME 곱셈.
		 * unsigned 64비트 곱셈은 자연스럽게 2^64 모듈러 연산이 되어 오버플로가 허용된다. */
		hval *= FNV_PRIME;
		/* [한국어] 워드(8B) 단위로 처리 가능한지 분기 — 대부분의 버퍼는 8의 배수 길이라
		 * 본 경로가 메인 성능 경로다. */
		if (len >= sizeof(uint64_t)) {
			/* [한국어] FNV-1a 단계 2(워드 경로): 8바이트 워드를 한 번에 읽어 XOR.
			 * *ptr++ 후위 증가로 포인터 전진, (uint64_t) 캐스팅은 타입 명시성(실제로는 no-op). */
			hval ^= (uint64_t) *ptr++;
			/* [한국어] 남은 길이에서 8바이트 차감 — 다음 반복 진입 여부 판정용. */
			len -= sizeof(uint64_t);
			/* [한국어] 다음 while 조건 평가로 루프 상단 재진입. break 아님에 주의. */
			continue;
		} else {
			/* [한국어] 잔여(<8B) 경로 진입. 바이트 단위 접근을 위해 uint8_t 재해석.
			 * 정렬 문제 없음(uint8_t* 는 모든 주소에 대해 안전). */
			const uint8_t *ptr8 = (const uint8_t *) ptr;
			/* [한국어] 잔여 바이트를 쌓을 임시 누적 워드 — 0으로 시작해 빅엔디안 순서로 조립. */
			uint64_t val = 0;
			/* [한국어] 루프 카운터. len < 8 이므로 비트가 작고 int 로 충분. */
			int i;

			/* [한국어] 잔여 바이트를 len 개만큼 순회 — 마지막 블록 하나만 처리하므로
			 * 종료 후 곧바로 break. */
			for (i = 0; i < len; i++) {
				/* [한국어] 기존 누적치를 한 바이트 공간만큼 좌로 밀어 상위에 자리 확보.
				 * 빅엔디안 조립(첫 바이트가 최상위) — 표준 FNV-1a 순서는 바이트 단위라
				 * 본 구현의 선택은 "내부 일관성" 확보용. */
				val <<= 8;
				/* [한국어] 다음 바이트를 최하위 8비트에 결합 후 포인터 전진. */
				val |= (uint8_t) *ptr8++;
			}
			/* [한국어] 조립된 잔여 워드를 hval 에 XOR — 본 함수의 최종 누적 단계. */
			hval ^= val;
			/* [한국어] 잔여 처리가 끝나면 남은 반복이 없으므로 루프 탈출. */
			break;
		}
	}

	/* [한국어] 누적된 64비트 해시 반환. 호출자는:
	 *   - verify.c 경로: verify_header.v_fnv 에 저장하거나 읽은 헤더와 비교.
	 *   - 벤치마크: t->output 에 누적(컴파일러 DCE 방지 용도). */
	return hval;
}
