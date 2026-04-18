/*
 * [한국어 설명] cpuio I/O 엔진 구현 (cpu.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 실제 디스크/네트워크 I/O를 전혀 수행하지 않고 CPU 사이클만 소모하는
 * "cpuio" 엔진을 구현한다. 목적은 두 가지이다. 첫째, CPU 바운드 워크로드를
 * 재현하여 순수 계산 부하를 벤치마크한다. 둘째, 실제 I/O 잡(libaio, io_uring,
 * sync 등)과 동일 파일/서버 내에서 병렬로 실행하여 "noisy neighbor"(이웃 잡의
 * CPU 점유로 인한 I/O 성능 저하) 상황을 재현한다. 두 가지 스트레스 모드 —
 * noop(빈 루프)과 qsort(대용량 배열 다중 정렬) — 를 지원하며, cpuload(0~100%)와
 * cpuchunks(ms)로 연소 시간 대 수면 시간 비율을 제어한다. 이 엔진은
 * FIO_DISKLESSIO(파일을 실제로 열지 않음) / FIO_NOIO(데이터 전송량 통계 제외) /
 * FIO_SYNCIO(큐에서 즉시 완료) 플래그를 모두 켜서 fio 상위 계층에 "I/O 아님"을 알린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 엔진 플러그인 계약(ioengine_ops: init/queue/cleanup/open_file/...) 위에
 * 존재하지만 I/O 관련 콜백은 의도적으로 no-op 또는 CPU 소비 루프로 구현한다.
 * 호출 경로: backend.c의 thread_main() → td_io_init() → fio_cpuio_init() →
 * 메인 루프에서 get_io_u()로 더미 io_u 발급 → td_io_queue() →
 * fio_cpuio_queue()(CPU 연소) → 즉시 FIO_Q_COMPLETED 반환 → put_io_u().
 * 실행 컨텍스트는 잡 스레드(유저스페이스). 커널 의존은 usleep(3)/nanosleep(2)
 * 계열이 호출될 수 있는 thinktime 경로뿐이며, 본 엔진 함수 내에서는 usec_spin()이
 * 비지-웨이트(busy spin)로 소모하므로 시스템콜 자체도 거의 일으키지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(fio 코어): ioengines.c의 td_io_init/td_io_queue/td_io_cleanup/
 *   td_io_open_file가 본 파일의 콜백을 호출. backend.c의 do_io()가 thinktime을
 *   적용하여 실제 cpuload% 점유를 달성.
 * - 하위(libc/유틸): qsort(3) — glibc 내부 quicksort, usec_spin() — fio 내부
 *   비지-루프 유틸(time.c), fio_get_mono_time() — clock_gettime(CLOCK_MONOTONIC)
 *   래퍼(oslib), log_info() — fio 로그.
 * - 옵션: optgroup.h의 FIO_OPT_C_ENGINE(엔진 카테고리), off1 = offsetof(...)로
 *   thread_data->eo에 할당된 struct cpu_options 내 필드 주소 지정.
 * - 공유 상태: thread_data::eo(엔진 옵션/상태 — cpu_options 인스턴스),
 *   thread_data::o(thread_options — thinktime 등), thread_data::runstate
 *   (TD_SETTING_UP로 전환하여 qsort 초기화 동안 다른 상위 로직 진입 방지).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_cpuio_queue(): 엔진의 핵심 진입점. td_io_queue → 본 함수에서 cpumode에
 *   따라 usec_spin()(noop) 또는 do_qsort()(qsort)를 호출해 CPU 시간을 태우고
 *   즉시 FIO_Q_COMPLETED를 반환.
 * - fio_cpuio_init(): cpuload 범위 검증, thinktime 계산(=cpucycle*(100-load)/load),
 *   모드별 서브 초기화(noop_init / qsort_init) 호출.
 * - do_qsort(): qsort 모드의 실제 CPU 소비 루틴 — 4회 정렬하고 cpucycle을
 *   실측으로 재교정해 thinktime을 갱신.
 * - fio_cpuio_cleanup(): qsort 모드에서 할당한 배열 해제.
 * - fio_cpuio_open(): 파일을 실제로 열지 않고 가짜 성공(0) 반환 — FIO_DISKLESSIO.
 * - struct cpu_options: cpuload/cpucycle/cpumode/exit_io_done/qsort_data.
 * - struct mwc: Multiply-With-Carry RNG 상태(정렬 데이터 무작위화).
 * - enum stress_mode: FIO_CPU_NOOP(0) / FIO_CPU_QSORT(1).
 */

/*
 * CPU engine
 *
 * Doesn't transfer any data, merely burns CPU cycles according to
 * the settings.
 *
 */
#include "../fio.h"          /* [한국어] fio 코어: thread_data/thread_options/ioengine_ops/io_u/td_vmsg/td_set_runstate/usec_spin 등 전체 API */
#include "../optgroup.h"     /* [한국어] FIO_OPT_C_ENGINE/FIO_OPT_G_INVALID 등 옵션 카테고리·그룹 상수 */

// number of 32 bit integers to sort
/* [한국어] qsort 모드에서 정렬할 int32 원소 개수 = 256 * 1024 = 262144개.
 * 배열 크기 = 262144 * 4B = 1MiB. static file-scope 전역으로, 모든 잡이
 * 공유하는 상수 파라미터(각 잡은 자기 버퍼를 개별 할당하므로 쓰기 충돌 없음).
 * 설정자: 초기화 시 1회 대입(컴파일타임 상수).
 * 읽는 자: qsort_init()(calloc 크기), do_qsort()(qsort nmemb 인자).
 * 동기화: 읽기 전용이므로 락 불필요. */
static size_t qsort_size = (256 * (1ULL << 10)); // 256KB

/* [한국어] Multiply-With-Carry (Marsaglia) 32비트 난수 생성기 상태.
 * qsort 입력 배열을 무작위 값으로 초기화할 때 사용 — glibc qsort의 피벗 선택이
 * 정렬 시간에 영향을 주므로, 난수 입력으로 평균적/재현 가능한 부하를 보장한다. */
struct mwc {
	uint32_t w;
	/* [한국어] MWC의 첫 번째 상태 변수 w (16비트 하위·16비트 상위 분리 연산 대상).
	 * 설정자: qsort_init()에서 초기 엔트로피(362436069UL)로 초기화.
	 * 읽는 자: mwc32()가 매 호출마다 w = 18000*(w & 0xFFFF) + (w >> 16)으로 갱신·반환 조합에 사용.
	 * 값 범위: uint32_t 전체. 주기는 약 2^59(Marsaglia 이론치).
	 * 동기화: 각 잡 스레드가 스택 지역 mwc 인스턴스를 쓰므로 스레드 간 공유 없음. */

	uint32_t z;
	/* [한국어] MWC의 두 번째 상태 변수 z (독립된 시퀀스 생성기).
	 * 설정자: qsort_init()에서 초기 엔트로피(521288629UL)로 초기화.
	 * 읽는 자: mwc32()가 z = 36969*(z & 0xFFFF) + (z >> 16)으로 갱신. 최종 난수 = (z << 16) + w.
	 * 값 범위: uint32_t 전체. w와 독립적으로 순환.
	 * 동기화: 스택 지역 — 공유 없음. */
};

/* [한국어] cpuio 엔진의 스트레스 모드 열거형.
 * cpumode 옵션("noop"/"qsort")의 posval.oval로 매핑되어 co->cpumode에 저장된다. */
enum stress_mode {
	FIO_CPU_NOOP = 0,
	/* [한국어] 빈 루프 — usec_spin(cpucycle)으로 지정 시간만큼 단순히 비지-웨이트.
	 * CPU 파이프라인/분기예측 부하 최소, 전력 소비 비교적 낮음. 기본값. */

	FIO_CPU_QSORT = 1,
	/* [한국어] 1MiB int32 배열을 4회(정순/역순/바이트단위/역순) 정렬.
	 * 캐시·분기예측기를 많이 괴롭히므로 실제 "바쁜 이웃" 시뮬레이션에 유용. */
};

/* [한국어] cpuio 엔진 전용 옵션 구조체. thread_data->eo가 이 구조체를 가리키도록
 * fio 코어가 io_ops->option_struct_size 크기로 할당·초기화한다. */
struct cpu_options {
	void *pad;
	/* [한국어] fio 옵션 파서 규약상 구조체 선두에 두는 불투명 패딩 포인터.
	 * 설정자: fio 코어(옵션 파서)가 내부 용도로 사용. 엔진 코드는 건드리지 않음.
	 * 읽는 자: fio 코어.
	 * 값 범위: 내부 사용. NULL일 수 있음.
	 * 동기화: fio 파서 단일 스레드 단계에서만 설정되므로 락 불필요. */

	unsigned int cpuload;
	/* [한국어] 이 잡이 점유할 목표 CPU 사용률(0~100 %). 0이면 초기화 실패(무의미).
	 * 설정자: 잡 파일/CLI의 cpuload= 옵션 파싱.
	 * 읽는 자: fio_cpuio_init()(검증·thinktime 계산), do_qsort()(thinktime 재교정).
	 * 값 범위: 1~100. 100 초과는 init에서 100으로 클램프.
	 * 동기화: 각 잡 단일 스레드에서 읽힘 — 락 불필요. */

	unsigned int cpucycle;
	/* [한국어] 한 연소 청크 길이(마이크로초). thinktime과 함께 cpuload 비율을 구성.
	 * 설정자: cpuchunks= 옵션(기본 50000us = 50ms). qsort 모드에서는 do_qsort() 실측값으로 동적 갱신.
	 * 읽는 자: usec_spin(co->cpucycle)(noop), thinktime 계산식에 피승수로 사용.
	 * 값 범위: 양의 정수(us). 너무 작으면 스핀 오버헤드가 지배적, 너무 크면 응답성 저하.
	 * 동기화: 본인 잡 스레드 내에서만 갱신/읽기 — 락 불필요. */

	enum stress_mode cpumode;
	/* [한국어] 스트레스 모드 선택 값.
	 * 설정자: cpumode= 옵션 파싱("noop"/"qsort" → 상수 매핑).
	 * 읽는 자: fio_cpuio_init()/queue()/cleanup()의 switch 분기.
	 * 값 범위: FIO_CPU_NOOP(0) / FIO_CPU_QSORT(1). 그 외는 init에서 에러.
	 * 동기화: 초기화 이후 불변 — 락 불필요. */

	unsigned int exit_io_done;
	/* [한국어] 다른 I/O 잡이 모두 끝나면 본 CPU 잡도 종료할지 여부(bool).
	 * 설정자: exit_on_io_done= 옵션(기본 0).
	 * 읽는 자: fio_cpuio_queue()에서 fio_running_or_pending_io_threads()와 함께 검사.
	 * 값 범위: 0/1.
	 * 동기화: 읽기 전용(잡 실행 중 불변) — 락 불필요. */

	int32_t *qsort_data;
	/* [한국어] qsort 모드에서 매 회차 정렬할 1MiB(=qsort_size*4B) 작업 버퍼.
	 * 설정자: qsort_init()의 calloc(). 해제자: qsort_cleanup()의 free().
	 * 읽는 자/쓰는 자: do_qsort()가 4차례 제자리 재정렬.
	 * 값 범위: 유효 힙 포인터 또는 NULL(할당 실패/해제 후). NULL이면 qsort_cleanup이 no-op.
	 * 동기화: 잡당 고유 버퍼 — 스레드 간 공유 없음. */
};

/* [한국어] 본 엔진의 잡 옵션 테이블. fio 코어가 옵션 파싱 시 순회하며 각 항목의
 * .off1 = offsetof(struct cpu_options, 필드)로 파싱 결과를 저장한다.
 * 마지막은 .name = NULL 센티넬. */
static struct fio_option options[] = {
	{
		.name	= "cpuload",                 /* [한국어] 명령줄/잡 파일에서 사용되는 짧은 키 */
		.lname	= "CPU load",                 /* [한국어] --help 등에서 보이는 긴 이름 */
		.type	= FIO_OPT_INT,                /* [한국어] 정수 파싱 */
		.off1	= offsetof(struct cpu_options, cpuload), /* [한국어] 파싱 결과 저장 오프셋 */
		.help	= "Use this percentage of CPU", /* [한국어] 도움말 문구 */
		.category = FIO_OPT_C_ENGINE,         /* [한국어] 엔진 카테고리 */
		.group	= FIO_OPT_G_INVALID,         /* [한국어] 세부 그룹 미지정 */
	},
	{
		.name     = "cpumode",                /* [한국어] 스트레스 모드 선택 */
		.lname    = "cpumode",
		.type     = FIO_OPT_STR,              /* [한국어] 문자열 → posval 상수 매핑 */
		.help     = "Stress mode",
		.off1     = offsetof(struct cpu_options, cpumode),
		.def      = "noop",                   /* [한국어] 기본값은 빈 루프 */
		.posval = {
			  { .ival = "noop",              /* [한국어] 사용자 입력 문자열 */
			    .oval = FIO_CPU_NOOP,        /* [한국어] 매핑되는 enum 값 */
			    .help = "NOOP instructions",
			  },
			  { .ival = "qsort",
			    .oval = FIO_CPU_QSORT,
			    .help = "QSORT computation",
			  },
		},
		.category = FIO_OPT_C_ENGINE,
		.group    = FIO_OPT_G_INVALID,
	},
	{
		.name	= "cpuchunks",                /* [한국어] 한 연소 청크 길이(us) */
		.lname	= "CPU chunk",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct cpu_options, cpucycle),
		.help	= "Length of the CPU burn cycles (usecs)",
		.def	= "50000",                    /* [한국어] 기본 50ms */
		.parent = "cpuload",                  /* [한국어] cpuload가 설정돼야 의미가 있음을 명시 */
		.hide	= 1,                          /* [한국어] cpuload 없을 땐 목록에서 숨김 */
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= "exit_on_io_done",          /* [한국어] 다른 I/O 잡 종료 시 본 잡도 종료 */
		.lname	= "Exit when IO threads are done",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct cpu_options, exit_io_done),
		.help	= "Exit when IO threads finish",
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_INVALID,
	},
	{
		.name	= NULL,                       /* [한국어] 테이블 종료 센티넬 */
	},
};

/*
 *  mwc32()
 *      Multiply-with-carry random numbers
 *      fast pseudo random number generator, see
 *      http://www.cse.yorku.ca/~oz/marsaglia-rng.html
 *
 * [한국어]
 * mwc32 - Marsaglia의 Multiply-With-Carry 32비트 난수 한 개 생성
 *
 * @mwc: 호출자가 보유한 MWC 상태(w, z). 함수는 두 변수를 모두 갱신한다.
 * @return: 생성된 32비트 난수 = (z<<16) + w (양측 시퀀스 합성).
 *
 * 왜 필요한가: qsort 입력 배열을 유의미하게 섞어 놓아야 qsort가 실제로 일을 하며
 * 매 실행마다 일관된 계산량을 가진다. rand()는 락/스레드-세이프 문제가 있고
 * glibc random()은 상태가 전역이라 이 간단한 Marsaglia RNG로 충분하다.
 * 실행 컨텍스트: 잡 스레드, qsort_init()의 초기화 루프에서만 호출.
 *
 * 호출 체인: qsort_init() → [mwc32] (반복)
 */
static uint32_t mwc32(struct mwc *mwc)
{
        mwc->z = 36969 * (mwc->z & 65535) + (mwc->z >> 16);
        /* [한국어] z 시퀀스 전진: 16비트 하위*36969 + 16비트 상위 캐리. Marsaglia가
         *  주기 >2^59로 검증한 곱셈 상수. 65535(=0xFFFF)로 하위 16비트 추출. */

        mwc->w = 18000 * (mwc->w & 65535) + (mwc->w >> 16);
        /* [한국어] w 시퀀스 전진: 동일 패턴, 곱셈 상수 18000. z와 독립적 주기. */

        return (mwc->z << 16) + mwc->w;
        /* [한국어] 두 16비트 상태의 상위 비트 결합으로 최종 32비트 난수 합성. */
}

/*
 *  stress_qsort_cmp_1()
 *	qsort comparison - sort on int32 values
 *
 * [한국어]
 * stress_qsort_cmp_1 - qsort() 표준 비교자: int32 오름차순.
 *
 * @p1, @p2: qsort가 내부적으로 전달하는 두 원소의 주소(void*).
 * @return: *p1 > *p2 → 1, *p1 < *p2 → -1, 같음 → 0 (C 표준 비교 규약).
 *
 * 왜 필요한가: qsort 모드 1차 정렬 패스. 정순(오름차순) 기준을 제공.
 * 실행 컨텍스트: glibc qsort 내부(잡 스레드)에서 다수 호출됨. 재진입 안전(순수 함수).
 * 주의: 큰 값 - 작은 값 방식은 int 오버플로 위험이라 비교 후 ±1을 반환한다.
 *
 * 호출 체인: qsort(stress_qsort_cmp_1) → [이 함수] (수천만 회)
 */
static int stress_qsort_cmp_1(const void *p1, const void *p2)
{
	const int32_t *i1 = (const int32_t *)p1;   /* [한국어] void*를 int32*로 타입 복원 */
	const int32_t *i2 = (const int32_t *)p2;   /* [한국어] 동일 */

	if (*i1 > *i2)                             /* [한국어] 크면 양의 값 반환(= 뒤로 보냄) */
		return 1;
	else if (*i1 < *i2)                        /* [한국어] 작으면 음의 값(= 앞으로) */
		return -1;
	else
		return 0;                          /* [한국어] 같으면 0(상대 순서 미정) */
}

/*
 *  stress_qsort_cmp_2()
 *	qsort comparison - reverse sort on int32 values
 *
 * [한국어]
 * stress_qsort_cmp_2 - 역순 비교자: stress_qsort_cmp_1의 인자 순서만 뒤집음.
 *
 * @p1, @p2: qsort가 전달하는 두 원소 주소.
 * @return: 내림차순 기준의 비교 결과.
 *
 * 왜 필요한가: 1차 정렬(오름차순) 결과를 그대로 둔 채 한 번 더 정렬해도 qsort가
 * 금방 끝나므로 CPU 부하가 적다. 인자를 뒤집어 역순으로 강제함으로써 최악에
 * 가까운 재정렬 작업량을 유도한다.
 * 실행 컨텍스트: qsort 내부 콜백(잡 스레드).
 *
 * 호출 체인: qsort(stress_qsort_cmp_2) → [이 함수] → stress_qsort_cmp_1
 */
static int stress_qsort_cmp_2(const void *p1, const void *p2)
{
	return stress_qsort_cmp_1(p2, p1);         /* [한국어] 인자 순서 뒤집어 부호 반전 */
}

/*
 *  stress_qsort_cmp_3()
 *	qsort comparison - sort on int8 values
 *
 * [한국어]
 * stress_qsort_cmp_3 - 바이트(int8) 비교자: 배열을 바이트열로 재해석해 정렬.
 *
 * @p1, @p2: 바이트 포인터로 캐스팅할 원소 주소.
 * @return: (int8)차. int8 범위(-128..127)이므로 int로 빼도 오버플로 없음.
 *
 * 왜 필요한가: int32를 뒤집어 바이트 단위로 재배열하면 원래 int32 정렬과는 전혀
 * 다른 순서가 나와 qsort에게 "거의 무작위" 입력이 된다. 최악 케이스에 가까운
 * CPU 소모를 유도.
 * 실행 컨텍스트: qsort 내부 콜백(잡 스레드).
 *
 * 호출 체인: qsort(stress_qsort_cmp_3) → [이 함수]
 */
static int stress_qsort_cmp_3(const void *p1, const void *p2)
{
	const int8_t *i1 = (const int8_t *)p1;     /* [한국어] void*를 int8*(=signed char*)로 캐스팅 */
	const int8_t *i2 = (const int8_t *)p2;     /* [한국어] 동일 */

	/* Force re-ordering on 8 bit value */
	return *i1 - *i2;                          /* [한국어] 8비트 차(오버플로 안전) 반환 */
}

/*
 * [한국어]
 * do_qsort - qsort 모드에서 한 번의 CPU 연소 청크 수행
 *
 * @td: fio 잡 컨텍스트. td->eo가 struct cpu_options, td->o가 thread_options.
 * @return: 항상 0(실패 경로 없음 — qsort는 항상 성공).
 *
 * 4단계 정렬(정순→역순→바이트단위→역순)로 CPU·캐시·분기예측기를 집중적으로
 * 괴롭힌다. 실행 시간을 실측하여 cpucycle을 갱신하고, cpuload 비율이 유지되도록
 * thinktime(연소 사이의 sleep 시간)을 재계산한다. 이는 전원관리·잡 경합·온도에
 * 따라 qsort 실행 시간이 변동하므로 정적 계산으로는 목표 cpuload를 맞출 수
 * 없기 때문이다.
 * 실행 컨텍스트: 잡 스레드. 단일 io_u 제출당 1회 호출되며 중간에 차단되지 않는다.
 *
 * 호출 체인: fio_cpuio_queue() → [do_qsort] → qsort(3)
 */
static int do_qsort(struct thread_data *td)
{
	struct thread_options *o = &td->o;         /* [한국어] thinktime 갱신 대상 */
	struct cpu_options *co = td->eo;           /* [한국어] 엔진 옵션/워크버퍼 접근 */
	struct timespec start, now;                /* [한국어] 청크 시작·종료 모노토닉 시각 */

	fio_get_mono_time(&start);                 /* [한국어] clock_gettime(CLOCK_MONOTONIC) 래퍼 — 벽시계 아님, 점프 없음 */

	/* Sort "random" data */
	qsort(co->qsort_data, qsort_size, sizeof(*(co->qsort_data)), stress_qsort_cmp_1);
	/* [한국어] 1차: 262144 × 4B int32 배열을 오름차순 정렬. glibc qsort(대개 intro-sort). */

	/* Reverse sort */
	qsort(co->qsort_data, qsort_size, sizeof(*(co->qsort_data)), stress_qsort_cmp_2);
	/* [한국어] 2차: 정렬된 배열을 역순으로 재정렬 — 최악에 가까운 워크로드. */

	/* And re-order by byte compare */
	qsort((uint8_t *)co->qsort_data, qsort_size * 4, sizeof(uint8_t), stress_qsort_cmp_3);
	/* [한국어] 3차: 동일 메모리를 바이트열(262144*4=1048576 바이트)로 재해석해 정렬.
	 * int32 정렬 결과가 바이트 단위로는 거의 무작위가 되어 다시 큰 작업량을 유발. */

	/* Reverse sort this again */
	qsort(co->qsort_data, qsort_size, sizeof(*(co->qsort_data)), stress_qsort_cmp_2);
	/* [한국어] 4차: 다시 int32 역순으로 정렬해 마무리. */

	fio_get_mono_time(&now);                   /* [한국어] 종료 시각 기록 */

	/* Adjusting cpucycle automatically to be as close as possible to the
	 * expected cpuload The time to execute do_qsort() may change over time
	 * as per : - the job concurrency - the cpu clock adjusted by the power
	 * management After every do_qsort() call, the next thinktime is
	 * adjusted regarding the last run performance
	 */
	co->cpucycle = utime_since(&start, &now);
	/* [한국어] 이번 청크의 실제 소요 시간(us)을 기록. 다음 thinktime 계산의 기준치가 된다. */

	o->thinktime = ((unsigned long long) co->cpucycle *
				(100 - co->cpuload)) / co->cpuload;
	/* [한국어] thinktime = cpucycle * (100-load)/load.
	 * 예: load=80, cpucycle=1000us → thinktime=250us → 비율 1000/(1000+250)=80%.
	 * 64비트 승산으로 오버플로 방지 후 정수 나눗셈. cpuload=0은 init에서 차단됨. */

	return 0;                                  /* [한국어] 실패 경로 없음 — 항상 성공 */
}

/*
 * [한국어]
 * fio_cpuio_queue - cpuio 엔진의 I/O 제출 콜백 (ioengine_ops.queue)
 *
 * @td: 현재 잡 컨텍스트.
 * @io_u: fio가 할당한 더미 I/O 유닛. cpuio는 데이터 전송을 하지 않으므로 fio_unused
 *        속성으로 경고를 억제하고 실제로는 사용하지 않는다.
 * @return: FIO_Q_COMPLETED — 즉시 완료(동기). exit_io_done 경로에서만 FIO_Q_BUSY
 *          (td->done=1로 종료 요청).
 *
 * 왜 필요한가: fio 백엔드의 do_io() 루프가 "무언가"를 큐잉해야 통계·thinktime
 * 계산이 돌아간다. 본 엔진은 그 한 스텝 동안 CPU를 연소시킨다.
 * 실행 컨텍스트: 잡 스레드. FIO_SYNCIO 엔진이므로 호출자가 commit/getevents를
 * 별도로 돌리지 않음.
 *
 * 호출 체인: backend.c do_io() → td_io_queue() → [fio_cpuio_queue]
 *   → usec_spin()(noop) / do_qsort()(qsort)
 */
static enum fio_q_status fio_cpuio_queue(struct thread_data *td,
					 struct io_u fio_unused *io_u)
{
	struct cpu_options *co = td->eo;           /* [한국어] 엔진 옵션/상태 접근 */

	if (co->exit_io_done && !fio_running_or_pending_io_threads()) {
		/* [한국어] exit_on_io_done=1이고 다른 실제 I/O 잡이 모두 끝났다면 본 잡도 종료.
		 * fio_running_or_pending_io_threads()는 NOIO/DISKLESSIO 잡을 제외한
		 * 실행/대기 중 I/O 스레드 유무를 bool로 반환(backend.c). */
		td->done = 1;                      /* [한국어] 백엔드 루프에 종료 신호 */
		return FIO_Q_BUSY;                 /* [한국어] 이 제출은 처리 않음 — 상위가 재큐 대신 종료 처리 */
	}

	switch (co->cpumode) {                      /* [한국어] 초기화 시 선택한 스트레스 모드에 따라 분기 */
	case FIO_CPU_NOOP:
		usec_spin(co->cpucycle);           /* [한국어] cpucycle us 동안 비지-웨이트 스핀(time.c). 시스템콜 없음 → sleep/wakeup 지터 제거 */
		break;
	case FIO_CPU_QSORT:
		do_qsort(td);                      /* [한국어] 4회 qsort로 CPU 소모 + thinktime 재교정 */
		break;
	}

	return FIO_Q_COMPLETED;                    /* [한국어] 동기 엔진 — 즉시 완료 보고 */
}

/*
 * [한국어]
 * noop_init - noop 모드 초기화 (로그만 출력)
 *
 * @td: 잡 컨텍스트.
 * @return: 항상 0.
 *
 * 왜 필요한가: noop 모드는 특별한 자원 할당이 없어 실체 초기화는 불필요하지만,
 * 운영자가 실제로 어떤 cpuload/cpucycle로 돌아가는지 확인할 수 있도록 로그만
 * 남긴다.
 * 실행 컨텍스트: fio_cpuio_init()에서 한 번(잡 시작 시).
 *
 * 호출 체인: fio_cpuio_init() → [noop_init] → log_info()
 */
static int noop_init(struct thread_data *td)
{
	struct cpu_options *co = td->eo;           /* [한국어] 로그에 쓸 파라미터 접근 */

	log_info("%s (noop): ioengine=%s, cpuload=%u, cpucycle=%u\n",
		td->o.name, td->io_ops->name, co->cpuload, co->cpucycle);
	/* [한국어] 잡명/엔진명/cpuload/cpucycle을 표준 로그로 출력(디버그·재현용). */
	return 0;                                  /* [한국어] 실패 경로 없음 */
}

/*
 * [한국어]
 * qsort_cleanup - qsort 모드 자원 해제
 *
 * @td: 잡 컨텍스트.
 * @return: 항상 0.
 *
 * qsort_init()에서 calloc으로 잡은 1MiB 버퍼를 free하고 포인터를 NULL로 리셋해
 * 이중 해제/이중 접근 위험을 방지한다.
 * 실행 컨텍스트: fio_cpuio_cleanup()(잡 종료 시) — 잡 스레드.
 *
 * 호출 체인: fio_cpuio_cleanup() → [qsort_cleanup] → free(3)
 */
static int qsort_cleanup(struct thread_data *td)
{
	struct cpu_options *co = td->eo;           /* [한국어] 해제 대상 포인터 위치 접근 */

	if (co->qsort_data) {                      /* [한국어] 할당 실패/미할당 상태면 건너뜀 — 안전 해제 관례 */
		free(co->qsort_data);              /* [한국어] glibc free(malloc/calloc으로 얻은 힙 반환) */
		co->qsort_data = NULL;             /* [한국어] 이중 해제 방지 */
	}

	return 0;                                  /* [한국어] 실패 없음 */
}

/*
 * [한국어]
 * qsort_init - qsort 모드 초기화 (작업 배열 할당 + 난수 채움 + 로그)
 *
 * @td: 잡 컨텍스트.
 * @return: 0 성공, 1 실패(메모리 부족 시 td_verror로 에러 기록).
 *
 * calloc으로 1MiB 버퍼를 확보한 뒤 MWC 난수로 전부 채운다. "무작위 초기화"는
 * qsort가 매 회차 공평하게 무거운 일을 하도록 만드는 핵심이다(정렬된 데이터를
 * 주면 qsort는 조기에 끝남). 초기화는 한 번만 수행(비용 큼).
 * 실행 컨텍스트: fio_cpuio_init() — 잡 시작 시 한 번. td->runstate = TD_SETTING_UP로
 * 전환된 상태에서 호출되므로 백엔드는 아직 실제 I/O 루프를 시작하지 않음.
 *
 * 호출 체인: fio_cpuio_init() → [qsort_init] → calloc(3)/mwc32()/log_info()
 */
static int qsort_init(struct thread_data *td)
{
	/* Setting up a default entropy */
	struct mwc mwc = { 521288629UL, 362436069UL };
	/* [한국어] MWC 초기 엔트로피 상수(Marsaglia 권장값). 결정론적 — 재현 가능한 부하.
	 * 스택 지역 — 잡 스레드당 개별 사본. */

	struct cpu_options *co = td->eo;           /* [한국어] qsort_data 필드가 있는 곳 */
	int32_t *ptr;                              /* [한국어] 초기화 루프용 이동 포인터 */
	int i;                                     /* [한국어] 루프 카운터 */

	co->qsort_data = calloc(qsort_size, sizeof(*co->qsort_data));
	/* [한국어] calloc(n, size): 0으로 초기화된 n*size 바이트 할당. 직후 mwc32로 덮어쓰므로
	 * 0 초기화가 필수는 아니나 malloc 대신 calloc을 쓴 것은 커밋된 페이지를 확보해
	 * 이후 qsort의 page fault 지연을 피하려는 의도로 해석 가능. */

	if (co->qsort_data == NULL) {              /* [한국어] OOM 시 NULL 반환 */
		td_verror(td, ENOMEM, "qsort_init"); /* [한국어] fio 에러 로그(errno=ENOMEM) 기록 */
		return 1;                          /* [한국어] init 실패 → 이 잡은 실행되지 않음 */
	}

	/* This is expensive, init the memory once */
	for (ptr = co->qsort_data, i = 0; i < qsort_size; i++)
		/* [한국어] 262144회 루프 — 각 원소를 MWC 난수로 초기화. 한 번만 수행되므로
		 * 잡 실행 중 반복 비용은 없음(do_qsort는 배열을 "재정렬"만 함). */
		*ptr++ = mwc32(&mwc);              /* [한국어] 포인터 후위증가로 연속 저장 */

	log_info("%s (qsort): ioengine=%s, cpuload=%u, cpucycle=%u\n",
		td->o.name, td->io_ops->name, co->cpuload, co->cpucycle);
	/* [한국어] 초기 설정값 로그(실제 cpucycle은 do_qsort 첫 실행 후 측정값으로 교체됨). */

	return 0;                                  /* [한국어] 성공 */
}

/*
 * [한국어]
 * fio_cpuio_init - cpuio 엔진 초기화 콜백 (ioengine_ops.init)
 *
 * @td: 잡 컨텍스트.
 * @return: 0 성공, 1 실패(설정 오류/OOM 등).
 *
 * 동작:
 *  1) cpuload 유효성 검사(0은 무의미, 100 초과는 100으로 클램프).
 *  2) runstate를 TD_SETTING_UP으로 바꿔 qsort 초기화 중 상태 관측 잡음을 방지.
 *  3) thinktime_* 파라미터 설정 — backend.c do_io()가 매 io_u마다 이 값을 사용.
 *     - thinktime_blocks=1: 매 1 I/O마다 thinktime 적용.
 *     - thinktime_blocks_type=COMPLETE: 완료 기반 트리거.
 *     - thinktime_spin=0: spin은 cpucycle에서 처리하므로 0.
 *     - thinktime = cpucycle*(100-load)/load.
 *  4) 가짜 파일 한 개 등록(nr_files=open_files=1) — FIO_DISKLESSIO이지만 fio
 *     공통 경로가 최소 1개 파일을 요구하므로.
 *  5) 모드별 서브 초기화 호출.
 *  6) runstate 원상복구.
 * 실행 컨텍스트: 잡 시작 시 한 번(잡 스레드).
 *
 * 호출 체인: backend.c thread_main() → td_io_init() → [fio_cpuio_init]
 *   → noop_init() / qsort_init()
 */
static int fio_cpuio_init(struct thread_data *td)
{
	struct thread_options *o = &td->o;         /* [한국어] thinktime/nr_files 설정 대상 */
	struct cpu_options *co = td->eo;           /* [한국어] 엔진 옵션 접근 */
	int td_previous_state;                     /* [한국어] runstate 백업용 */
	char *msg;                                 /* [한국어] 에러 메시지 동적 버퍼 */

	if (!co->cpuload) {                        /* [한국어] cpuload 미지정/0은 무의미(분모) */
		td_vmsg(td, EINVAL, "cpu thread needs rate (cpuload=)","cpuio");
		/* [한국어] fio 에러 로그에 EINVAL로 기록 — 잡 실행 차단. */
		return 1;                          /* [한국어] init 실패 */
	}

	if (co->cpuload > 100)                     /* [한국어] 상한 클램프 — 100% 초과는 의미 없음 */
		co->cpuload = 100;

	/* Saving the current thread state */
	td_previous_state = td->runstate;          /* [한국어] 복구를 위해 현재 상태 저장 */

	/* Reporting that we are preparing the engine
	 * This is useful as the qsort() calibration takes time
	 * This prevents the job from starting before init is completed
	 */
	td_set_runstate(td, TD_SETTING_UP);
	/* [한국어] runstate를 "설정 중"으로 변경 — helper 스레드/상태 출력(stat.c)이
	 * 이 상태의 잡을 실행 중으로 오인하지 않게 한다. TD_SETTING_UP는 fio.h 정의. */

	/*
	 * set thinktime_sleep and thinktime_spin appropriately
	 */
	o->thinktime_blocks = 1;
	/* [한국어] 매 1개 io_u 완료마다 thinktime 삽입 — cpucycle:thinktime 비율로 cpuload 달성. */

	o->thinktime_blocks_type = THINKTIME_BLOCKS_TYPE_COMPLETE;
	/* [한국어] 발급 기반(ISSUE)이 아닌 완료 기반(COMPLETE)으로 thinktime 카운트. */

	o->thinktime_spin = 0;
	/* [한국어] thinktime 전체를 sleep으로 — CPU 연소는 cpucycle 단계에서 이미 수행. */

	o->thinktime = ((unsigned long long) co->cpucycle *
				(100 - co->cpuload)) / co->cpuload;
	/* [한국어] 초기 thinktime. qsort 모드는 do_qsort가 매 회차 재계산. */

	o->nr_files = o->open_files = 1;
	/* [한국어] 가짜 파일 1개 — FIO_DISKLESSIO라도 공통 경로가 파일 객체 최소 1개를 가정. */

	switch (co->cpumode) {                     /* [한국어] 모드별 전용 초기화 */
	case FIO_CPU_NOOP:
		noop_init(td);                     /* [한국어] 로그 출력만 */
		break;
	case FIO_CPU_QSORT:
		qsort_init(td);                    /* [한국어] 1MiB 버퍼 할당 + 난수 채움 */
		break;
	default:                                   /* [한국어] 알 수 없는 모드 — 방어적 처리 */
		if (asprintf(&msg, "bad cpu engine mode: %d", co->cpumode) < 0)
			msg = NULL;                /* [한국어] asprintf 실패 시 NULL 처리 후 기본 문구로 대체 */
		td_vmsg(td, EINVAL, msg ? : "(?)", __func__);
		/* [한국어] 에러 로그 기록(GNU ?: 연산자로 NULL 안전). */
		free(msg);                         /* [한국어] asprintf로 할당된 버퍼 해제(NULL이면 no-op) */
		return 1;                          /* [한국어] init 실패 */
	}

	/* Let's restore the previous state. */
	td_set_runstate(td, td_previous_state);
	/* [한국어] runstate 원복 — 백엔드가 실제 실행 루프로 진입할 준비 완료. */
	return 0;                                  /* [한국어] 성공 */
}

/*
 * [한국어]
 * fio_cpuio_cleanup - cpuio 엔진 정리 콜백 (ioengine_ops.cleanup)
 *
 * @td: 잡 컨텍스트.
 *
 * qsort 모드에서만 자원 해제가 필요하며 noop 모드는 할 일 없음. 잡 종료 시
 * backend.c thread_main() 후반부에서 td_io_cleanup()을 통해 호출된다.
 * 실행 컨텍스트: 잡 스레드 종료 직전.
 *
 * 호출 체인: thread_main() → td_io_cleanup() → [fio_cpuio_cleanup] → qsort_cleanup()
 */
static void fio_cpuio_cleanup(struct thread_data *td)
{
	struct cpu_options *co = td->eo;           /* [한국어] 모드 판별 */

	switch (co->cpumode) {
	case FIO_CPU_NOOP:
		break;                             /* [한국어] 해제할 자원 없음 */
	case FIO_CPU_QSORT:
		qsort_cleanup(td);                 /* [한국어] 1MiB 버퍼 free */
		break;
	}
}

/*
 * [한국어]
 * fio_cpuio_open - 가짜 파일 open 콜백 (ioengine_ops.open_file)
 *
 * @td, @f: fio가 관리하는 잡/파일 메타 (사용하지 않음 — fio_unused로 경고 억제).
 * @return: 항상 0(성공).
 *
 * FIO_DISKLESSIO 플래그를 세워두었기 때문에 실제 파일을 여는 대신 성공만 보고하면
 * 된다. 상위 공통 경로(filesetup.c)는 이 반환값만 확인하고 이후 read/write 콜백을
 * 호출하지 않는다(FIO_NOIO).
 * 실행 컨텍스트: 잡 준비 단계(잡 스레드).
 *
 * 호출 체인: setup_files() → td_io_open_file() → [fio_cpuio_open]
 */
static int fio_cpuio_open(struct thread_data fio_unused *td,
			  struct fio_file fio_unused *f)
{
	return 0;                                  /* [한국어] 가짜 성공 — 실제로는 아무것도 안 함 */
}

/* [한국어] cpuio 엔진 플러그인 등록 기술자.
 * fio 코어가 .queue/.init/.cleanup/.open_file을 미리 정의된 순서로 호출하며
 * .flags로 엔진 능력을 광고한다.
 *   - FIO_SYNCIO: queue가 호출 즉시 완료 반환(getevents/commit 불필요).
 *   - FIO_DISKLESSIO: 실제 파일/디바이스 필요 없음 — open이 가짜라도 OK.
 *   - FIO_NOIO: 전송 바이트 통계 기록 대상 아님(대역폭 0으로 집계).
 * option_struct_size: fio 코어가 잡당 struct cpu_options만큼 힙을 할당해 td->eo에 넣음. */
static struct ioengine_ops ioengine = {
	.name			= "cpuio",              /* [한국어] ioengine=cpuio로 선택되는 이름 */
	.version		= FIO_IOOPS_VERSION,    /* [한국어] ABI 호환성 체크용 버전 상수 */
	.queue			= fio_cpuio_queue,      /* [한국어] I/O 제출 콜백 — CPU 연소 수행 */
	.init			= fio_cpuio_init,       /* [한국어] 잡 초기화 */
	.cleanup		= fio_cpuio_cleanup,    /* [한국어] 잡 종료 시 자원 해제 */
	.open_file		= fio_cpuio_open,       /* [한국어] 가짜 open */
	.flags			= FIO_SYNCIO | FIO_DISKLESSIO | FIO_NOIO,
	/* [한국어] 플래그 비트 조합 — 위 주석 참조. */
	.options		= options,              /* [한국어] 잡 옵션 테이블 주소 */
	.option_struct_size	= sizeof(struct cpu_options), /* [한국어] td->eo 할당 크기 */
};

/*
 * [한국어]
 * fio_cpuio_register - 엔진 등록 생성자
 *
 * fio_init 매크로는 gcc __attribute__((constructor))로 확장되어 main() 이전에
 * 자동 호출된다. 이로써 cpuio 엔진이 fio 내부 엔진 리스트에 등록되어 잡 파일에서
 * ioengine=cpuio로 참조 가능해진다.
 * 실행 컨텍스트: 프로세스 시작 직후, main() 진입 전. 단일 스레드.
 *
 * 호출 체인: (CRT startup) → [fio_cpuio_register] → register_ioengine()
 */
static void fio_init fio_cpuio_register(void)
{
	register_ioengine(&ioengine);              /* [한국어] ioengines.c의 전역 리스트에 &ioengine 삽입 */
}

/*
 * [한국어]
 * fio_cpuio_unregister - 엔진 해제 소멸자
 *
 * fio_exit 매크로는 __attribute__((destructor))로 확장되어 프로세스 종료 시
 * 자동 호출. 엔진 리스트에서 본 엔진을 제거해 누수 없는 종료를 보장.
 * 실행 컨텍스트: main() 반환 후(단일 스레드).
 *
 * 호출 체인: (CRT shutdown) → [fio_cpuio_unregister] → unregister_ioengine()
 */
static void fio_exit fio_cpuio_unregister(void)
{
	unregister_ioengine(&ioengine);            /* [한국어] 전역 리스트에서 &ioengine 제거 */
}
