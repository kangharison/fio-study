/*
 * [한국어 설명] 사용자 지정 할당자 기반 정렬 메모리 할당 헤더 (memalign.h)
 *
 * === 파일의 역할 ===
 * lib/memalign.c 가 제공하는 "사용자 지정 malloc/free 함수 포인터를 받아
 * 임의 alignment 바이트 경계로 정렬된 메모리를 할당/해제" 하는 두 함수
 * (`__fio_memalign`, `__fio_memfree`) 의 선언과 그에 필요한 두 함수 포인터
 * 타입(`malloc_fn`, `free_fn`) 을 정의한다. 리눅스 표준 posix_memalign(3) 이
 * 있음에도 별도로 이 래퍼가 존재하는 이유는 fio 가 일반 libc 힙 외에
 * smalloc(공유 메모리 SHM 기반 힙 — 서버/클라이언트 모드에서 잡 통계를
 * 공유하기 위해 사용) 이나 libnuma mmap 영역 위에도 정렬 할당을 해야 하기
 * 때문이다. 함수 포인터로 백엔드를 주입받는 설계로 SHM/NUMA/일반 힙을
 * 동일 API 로 다룰 수 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 의 I/O 버퍼 준비 계층. O_DIRECT / DMA / SIMD 로 수행되는 read/write
 * 는 페이지 경계 또는 디바이스-블록 경계 정렬 버퍼를 요구하고, 사용자가
 * `--iomem=malloc/shm/shmhuge/mmap/mmaphuge/cudamalloc` 중 어떤 백엔드를
 * 선택하든 정렬 요건은 공통이다. io_u.c / memory.c 가 백엔드에 맞는
 * malloc/free 함수 포인터를 쌍으로 전달하여 본 래퍼가 정렬 할당을 수행한다.
 * 호출 체인:
 *   memory.c alloc_mem_{malloc,shm,mmap,cuda} → __fio_memalign(align, sz, fn)
 *     → fn(sz + align + footer)  → 반환 포인터를 align 경계로 올림
 *   memory.c free_mem_* → __fio_memfree(ptr, sz, fn) → footer 로부터 원본 복원
 *
 * === 타 모듈과의 연결 ===
 * - memalign.c : 정렬 이디엄(입력 포인터 + align 으로 올림, footer 에 원본
 *   오프셋 저장) 구현.
 * - memory.c / io_u.c : 사용자 측. 잡별 I/O 버퍼 풀 할당.
 * - smalloc.c : __fio_memalign 의 fn 인자로 smalloc / sfree 를 넘겨
 *   SHM 영역에서 정렬 할당.
 * - <inttypes.h>, <stdbool.h> (본 헤더가 포함) : 고정폭 타입과 bool 정의.
 * 데이터 흐름: 사용자가 지정한 iomem 백엔드 → malloc_fn 포인터 →
 * __fio_memalign → 정렬된 포인터 → I/O 엔진(queue/getevents) 에서 사용 →
 * 잡 종료 시 __fio_memfree 로 반환.
 *
 * === 주요 함수/구조체 요약 ===
 * - malloc_fn : 사용자 정의 할당 함수 타입(size→void*). 예: malloc, smalloc,
 *   shm_malloc.
 * - free_fn : 사용자 정의 해제 함수 타입(void*→void). 예: free, sfree.
 * - __fio_memalign(align, size, fn) : size 바이트를 align 경계로 정렬하여 할당.
 *   내부적으로 fn(size + align + sizeof(offset)) 호출 후 정렬 올림 + footer
 *   에 오프셋 저장.
 * - __fio_memfree(ptr, size, fn) : footer 에서 원본 포인터를 복원하여 fn(원본).
 */
#ifndef FIO_MEMALIGN_H
#define FIO_MEMALIGN_H
/* [한국어] 헤더 가드. memory.c / io_u.c / smalloc 연동 소스에서 포함 중복
 * 방지. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : 여기서는 직접 쓰이지 않으나 호출자가 size_t /
 * uintptr_t 와 함께 사용할 때 공통 기반 제공. size_t 의 의미 있는 정의는
 * 실제로는 <stddef.h> 에 있으며 <inttypes.h> 가 대부분 간접 노출. */

#include <stdbool.h>
/* [한국어] <stdbool.h> : bool/true/false 매크로. 본 헤더는 bool 을 쓰지
 * 않지만 memory.c 나 smalloc 관련 호출부가 함께 bool 을 쓰는 경우가 많아
 * 공통 dependency 로 같이 공급. */

typedef void* (*malloc_fn)(size_t);
/* [한국어] malloc_fn - "size 바이트를 할당하고 포인터 반환" 함수 포인터 타입.
 * 설정자: memory.c 의 alloc_mem_* 류가 백엔드별 할당 함수를 넘김.
 * 읽는 자: __fio_memalign 내부에서 (size + alignment + footer) 인자로 호출.
 * 값 범위: 정상 포인터 또는 NULL(실패). NULL 시 __fio_memalign 도 NULL 반환.
 * 동기화: 호출 스레드 단위이며, 백엔드(예: smalloc) 내부 락은 백엔드가 관리. */

typedef void (*free_fn)(void*);
/* [한국어] free_fn - "포인터를 해제" 함수 포인터 타입. malloc_fn 으로 받은
 * 포인터를 해제하는 쌍이 되어야 한다(혼합 금지: malloc 으로 받아 sfree 로
 * 해제 금지).
 * 설정자/읽는 자: malloc_fn 과 동일한 짝으로 memory.c 가 쌍으로 등록·호출.
 * 값 범위: 유효 해제 함수 포인터. NULL 금지. */

extern void *__fio_memalign(size_t alignment, size_t size, malloc_fn fn);
/* [한국어] __fio_memalign - alignment 경계 정렬 메모리 size 바이트를 fn() 로
 * 할당하여 반환. 내부적으로 요청 크기에 alignment + sizeof(오프셋) 을 더해
 * fn 을 호출 → 반환 포인터를 alignment 로 올림 → 올림 직전 워드에 원본
 * 포인터의 오프셋을 저장(footer). 호출자는 __fio_memfree 로만 해제 가능.
 * 반환 NULL = 실패. 실행 컨텍스트: I/O 버퍼 초기화(잡 스레드). */

extern void __fio_memfree(void *ptr, size_t size, free_fn fn);
/* [한국어] __fio_memfree - __fio_memalign 이 반환한 ptr 을 해제. footer 에서
 * 원본 포인터 복원 후 fn(원본) 호출. size/fn 은 할당 시와 같아야 하며,
 * 호출자가 쌍을 관리해야 함. 실행 컨텍스트: 잡 종료/정리(잡 스레드). */

#endif
/* [한국어] FIO_MEMALIGN_H 가드 종료. */
