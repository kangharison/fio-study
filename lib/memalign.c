/*
 * [한국어 설명] 정렬된 메모리 할당 래퍼 (memalign.c)
 *
 * === 파일의 역할 ===
 * 임의의 정렬 경계(2의 거듭제곱)에 맞춘 메모리 블록을 할당/해제하는 얇은 래퍼.
 * 백엔드 할당자(malloc 또는 fio 의 공유 SHM 할당자 smalloc)를 함수 포인터로 받아,
 * 요청 크기의 뒤에 footer 하나를 더 할당한 뒤 포인터를 정렬 경계로 올림 정렬한다.
 * 할당된 원본 포인터와 정렬된 포인터 사이의 바이트 차이(offset) 를 footer 에
 * 저장해 두어 해제 시 footer 로부터 원본 주소를 복원한다. posix_memalign(3) /
 * aligned_alloc(3) 을 직접 쓰지 못하는 이유는 (a) smalloc 은 SHM 힙에서 할당되므로
 * libc 가 제공하는 aligned alloc 과 무관하고, (b) 여러 플랫폼(특히 Windows/
 * 구식 BSD) 호환성을 백엔드 교체로 해결하기 위함이다.
 *
 * 대표 사용처:
 *   - O_DIRECT I/O 버퍼: 블록 디바이스가 요구하는 섹터 정렬(통상 512B/4 KiB).
 *   - DMA 용 버퍼: IOMMU/하드웨어 제약에 맞춘 페이지 정렬.
 *   - SIMD/NT-store 정렬(32B/64B) — 캐시라인 false sharing 회피.
 *
 * === 전체 아키텍처에서의 위치 ===
 * io_u 버퍼 초기화(io_u.c init_io_u_buffers), SHM 힙(smalloc.c) 정렬 할당, 여러
 * 엔진의 페이로드 버퍼 등에서 호출된다. 소유 추적 상태는 각 호출 단위의 footer
 * 이외에는 없으므로 스레드 안전은 백엔드 할당자(fn)의 속성을 그대로 물려받는다.
 *
 * === 타 모듈과의 연결 ===
 * - memalign.h:   fio_memalign/fio_memfree 의 1차 인터페이스(매크로로 __fio_memalign 에 연결).
 * - smalloc.h:    smalloc/sfree 프로토타입 — SHM 힙 할당 백엔드.
 * - io_u.c / verify.c / 여러 엔진: O_DIRECT 정렬 페이로드 할당 시 주 호출자.
 * - <assert.h>:   alignment 가 2 의 거듭제곱인지 검증.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct align_footer: 정렬 오프셋을 담는 1-필드 구조체. 할당 블록 말미에 배치.
 * - __fio_memalign(alignment, size, fn): 핵심 할당 로직.
 *     반환: 정렬된 포인터(ret). 실패 시 NULL.
 * - __fio_memfree(ptr, size, fn): footer 로부터 원본 주소 복원 후 fn(free 류) 호출.
 *
 * === 메모리 레이아웃 ===
 *   원본(malloc) → |.. padding ..|<- 정렬됨(ret) ->|.. size ..|<- footer ->|
 *                  ^                                           ^
 *                  ptr                                         ret + size
 *   offset = ret - ptr (footer.offset 저장)
 *   해제 시 fn(ret - offset) == fn(ptr)
 */
#include <assert.h>             /* [한국어] alignment 가 2 의 거듭제곱인지 런타임 검증 */
#include <stdlib.h>             /* [한국어] (간접) 표준 타입 — malloc_fn/free_fn 시그니처 호환 */

#include "memalign.h"           /* [한국어] 공개 API 프로토타입(__fio_memalign/__fio_memfree) 및 malloc_fn/free_fn typedef */
#include "smalloc.h"            /* [한국어] SHM 힙 smalloc/sfree — fio_memalign 래퍼가 백엔드로 선택 가능 */

/* [한국어] 포인터 ptr 을 mask+1 바이트 경계로 올림 정렬하는 매크로.
 * mask 는 alignment-1 (예: alignment=4096 → mask=0xFFF).
 * 식: (uintptr_t)(ptr + mask) & ~mask  → 다음 경계로 올림. char* 캐스트는 산술 단위 바이트 보장. */
#define PTR_ALIGN(ptr, mask)   \
	(char *)((uintptr_t)((ptr) + (mask)) & ~(mask))

/*
 * [한국어] 할당 블록 말미에 배치되는 메타데이터.
 * - offset: 정렬된 ret 포인터에서 원본 ptr 까지의 바이트 차이.
 *   설정자: __fio_memalign — 정렬 직후 ret + size 위치에 기록.
 *   읽는 자: __fio_memfree — ptr + size 위치에서 읽어 원본 포인터 복원.
 *   값 범위: [0, alignment-1] — PTR_ALIGN 의 특성상 이 범위.
 *   동기화: 각 할당 단위에 1개씩 존재(스레드간 공유 없음) — 동기화 불필요.
 */
struct align_footer {
	unsigned int offset;
	/* [한국어] 정렬된 포인터(ret)와 원래 할당 포인터(ptr) 간의 바이트 차이(ret - ptr).
	 * 설정자: __fio_memalign() 이 할당 성공 직후 계산해 기록.
	 * 읽는 자: __fio_memfree() 가 ptr + size 오프셋에서 읽어 원본 포인터 복원에 사용.
	 * 값 범위: 0 ~ alignment-1 (최대 alignment-1 만큼의 패딩 발생 가능).
	 * 동기화: 각 할당 블록마다 1개 — 공유 없음, 락 불필요. */
};

/*
 * [한국어]
 * __fio_memalign - 지정된 정렬 경계에 맞춰 메모리를 할당.
 *
 * @alignment: 정렬 경계(바이트). 반드시 2 의 거듭제곱(assert 로 확인). 예: 512, 4096.
 * @size:      사용자 요청 크기(바이트). footer 는 추가로 할당됨.
 * @fn:        백엔드 할당 함수(malloc 또는 smalloc). size+alignment+sizeof(footer)-1 을
 *             요청받아 원본 포인터 ptr 을 반환.
 * @return:    정렬된 사용 가능 포인터 ret, 또는 할당 실패 시 NULL(ret 초기값 유지).
 *
 * 동작 단계:
 *   1) alignment 가 2 의 거듭제곱인지 assert — (a & (a-1))==0 트릭.
 *   2) 백엔드에 (size + alignment + sizeof(footer) - 1) 바이트 요청.
 *      최악의 경우 alignment-1 만큼 패딩이 필요하므로 여유분 포함.
 *   3) PTR_ALIGN 으로 ptr 을 alignment 경계로 올림 → ret.
 *   4) footer 는 ret + size 위치에 배치. footer->offset = ret - ptr.
 *   5) ret 반환. size 와 alignment 의 합계가 헤더 overhead 한계를 넘지 않도록
 *      호출자가 주의(현재 검증 없음).
 *
 * 실행 컨텍스트: 어디서나 호출 가능. 백엔드 fn 의 스레드 안전성을 상속(glibc malloc
 *               은 MT-safe, smalloc 은 내부 락 사용).
 *
 * 호출 체인: fio_memalign 매크로(memalign.h) → [__fio_memalign] → fn (malloc/smalloc).
 *
 * 에러 처리: fn 이 NULL 반환하면 ret 도 NULL 로 유지되어 자연 전파.
 */
void *__fio_memalign(size_t alignment, size_t size, malloc_fn fn)
{
	/* [한국어] footer 배치 위치 계산용 포인터 */
	struct align_footer *f;
	/* [한국어] ptr: 백엔드가 돌려준 원본 포인터. ret: 정렬된 사용자 포인터(초기 NULL=실패) */
	void *ptr, *ret = NULL;

	/* [한국어] alignment 가 2 의 거듭제곱인지 검증 — (a & (a-1))==0 트릭.
	 * 2 의 거듭제곱이어야 PTR_ALIGN 의 ~mask 가 의미 있는 정렬 경계가 된다 */
	assert(!(alignment & (alignment - 1)));

	/* [한국어] 필요량 = size + 정렬 패딩 최대(alignment-1) + footer 크기.
	 * fn 은 호출자 결정에 따라 malloc 또는 smalloc 중 하나 */
	ptr = fn(size + alignment + sizeof(*f) - 1);
	/* [한국어] OOM 등으로 ptr==NULL 이면 ret 은 초기 NULL 그대로 반환 → 호출자가 실패 감지 */
	if (ptr) {
		/* [한국어] 정렬된 반환 포인터 = ptr 을 alignment 경계로 올림 */
		ret = PTR_ALIGN(ptr, alignment - 1);
		/* [한국어] footer 를 "사용자 데이터 끝" 에 배치. size 바이트는 호출자 영역 */
		f = ret + size;
		/* [한국어] 해제 시 복원할 오프셋 기록(단위: 바이트).
		 * (uintptr_t) 캐스트로 포인터 산술을 정수 차로 명확화 */
		f->offset = (uintptr_t) ret - (uintptr_t) ptr;
	}

	/* [한국어] 성공 경로면 정렬된 ret, 실패 경로면 NULL 반환 */
	return ret;
}

/*
 * [한국어]
 * __fio_memfree - __fio_memalign() 으로 할당된 블록을 해제.
 *
 * @ptr:  __fio_memalign() 이 반환한 정렬 포인터. NULL 금지.
 * @size: 최초 요청한 size 와 동일해야 함(footer 위치 계산의 기준).
 *        호출자가 이 값을 보관하는 책임 — 헤더에 크기를 저장하지 않음으로써
 *        alignment 요구가 깨지지 않도록 설계.
 * @fn:   백엔드 해제 함수(free 또는 sfree). __fio_memalign 의 fn 과 짝을 맞춰야 함.
 *
 * 동작 단계:
 *   1) ptr + size 에서 footer 읽어 offset 획득.
 *   2) 원본 할당 포인터 = ptr - offset 로 복원.
 *   3) fn(원본) 호출로 해제.
 *
 * 실행 컨텍스트: 어디서나. MT 안전성은 fn 을 따름.
 *
 * 호출 체인: fio_memfree 매크로(memalign.h) → [__fio_memfree] → fn (free/sfree).
 *
 * 에러 처리: footer 손상/size 불일치 시 잘못된 주소를 해제하게 되어 크래시.
 *           호출자 계약 의존 — 런타임 검증 없음(오버헤드 회피).
 */
void __fio_memfree(void *ptr, size_t size, free_fn fn)
{
	/* [한국어] footer 위치: ptr + size. __fio_memalign 이 기록한 오프셋을 읽는다 */
	struct align_footer *f = ptr + size;

	/* [한국어] 원본 할당 포인터(ptr - offset) 를 백엔드에 반환.
	 * fn 은 malloc/smalloc 짝의 free/sfree — 호출자 책임으로 일치 보장 */
	fn(ptr - f->offset);
}
