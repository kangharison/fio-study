/*
 * [한국어 설명] Windows CPU 마스크 정의 (os-windows-7.h)
 *
 * === 파일의 역할 ===
 * Windows용 os_cpu_mask_t 타입을 정의한다.
 * Windows의 프로세서 그룹 시스템에서 최대 512개 CPU를 지원하기 위해
 * 64비트 정수 8개(512비트)로 구성된 비트마스크를 사용.
 * Hyper-V 2016의 최대 논리 프로세서 수에 맞춤.
 *
 * === 타 모듈과의 연결 ===
 * - os-windows.h에서 포함
 * - windows/cpu-affinity.c에서 마스크 조작 함수 구현
 */
#define FIO_MAX_CPUS		512 /* From Hyper-V 2016's max logical processors */
#define FIO_CPU_MASK_STRIDE	64
#define FIO_CPU_MASK_ROWS	(FIO_MAX_CPUS / FIO_CPU_MASK_STRIDE)

/* [한국어] Windows CPU 마스크 - 8개 uint64_t로 512비트 비트마스크 구성 */
typedef struct {
	uint64_t row[FIO_CPU_MASK_ROWS];	/* [한국어] 각 row는 64개 CPU를 표현 */
} os_cpu_mask_t;
