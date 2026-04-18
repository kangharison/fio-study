/*
 * [한국어 설명] NVIDIA cuFile(GPUDirect Storage) I/O 엔진 (libcufile.c)
 *
 * === 파일의 역할 ===
 * NVIDIA GPUDirect Storage(GDS)를 위한 libcufile API를 사용하여, 스토리지(NVMe/NVMe-oF 등)와
 * GPU Device Memory 사이의 P2P DMA 전송을 fio로 측정할 수 있게 해주는 I/O 엔진 "libcufile"을
 * 구현한다. 일반 POSIX I/O는 스토리지→호스트 페이지 캐시→cudaMemcpy→GPU의 왕복 경로를 거치는
 * 반면, cuFile 경로는 커널 내 nvidia-fs 드라이버가 NVMe 컨트롤러의 PRP(Physical Region Page)
 * 엔트리를 호스트 물리 주소가 아닌 **GPU BAR1에 매핑된 물리 주소**로 채워 넣음으로써, CPU/호스트
 * 메모리를 우회하는 direct DMA를 수행한다. 이를 위해 버퍼는 cudaMalloc으로 할당된 GPU 메모리여야
 * 하며, cuFileBufRegister로 미리 DMA 매핑(pin)되어야 한다. 이 엔진은 cuda_io 옵션으로 cuFile 경로와
 * 비교용 POSIX 경로(pread/pwrite + cudaMemcpy)를 선택할 수 있게 하여, GDS 이득을 정량 비교한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=libcufile로 선택된다. 실행 흐름: backend.c의 잡 스레드가 td_io_init을 통해
 * fio_libcufile_init()를 호출하여 cuFileDriverOpen()으로 nvidia-fs 드라이버를 1회 열고 각 서브잡을
 * gpu_dev_ids 리스트에 라운드로빈으로 바인딩(cudaSetDevice)한다. 이어서 iomem_alloc 훅이
 * fio_libcufile_iomem_alloc()을 호출해 cudaMalloc으로 GPU 메모리를 확보하고 cuFileBufRegister로
 * DMA 매핑을 pin한다. open_file 훅의 fio_libcufile_open_file()이 파일 fd를 cuFileHandleRegister로
 * 등록해 CUfileHandle을 얻고, queue 훅의 fio_libcufile_queue()가 각 io_u에 대해 cuFileRead/
 * cuFileWrite(동기)를 호출하거나 POSIX 분기에서는 pread/pwrite + cudaMemcpy를 수행한다. 종료 시
 * close_file→cuFileHandleDeregister, iomem_free→cuFileBufDeregister+cudaFree, cleanup→마지막
 * 스레드가 cuFileDriverClose()를 호출한다. 실행 컨텍스트는 fio 잡 스레드(유저스페이스) + CUDA 런타임
 * 드라이버 + nvidia-fs 커널 모듈이며, 엔진 플래그는 FIO_SYNCIO(동기 계약 — queue가 즉시 완료 반환).
 *
 * === 타 모듈과의 연결 ===
 * 상단 의존: fio 코어(backend.c, ioengines.c — ioengine_ops 플러그인 계약; io_u.c — io_u 생명주기
 *           및 xfer_buf/xfer_buflen 제공; filesetup.c — generic_open_file/generic_close_file).
 * 하단 의존: libcufile(cuFileDriverOpen/Close, cuFileHandleRegister/Deregister, cuFileBufRegister/
 *           Deregister, cuFileRead/Write — nvidia-fs 커널 모듈로 ioctl), CUDA Runtime(cudaMalloc/
 *           cudaFree/cudaMemcpy/cudaMemset/cudaSetDevice/cudaGetErrorName), POSIX(pread/pwrite/
 *           fsync/fdatasync), pthread(드라이버 전역 상태 보호용 mutex).
 * 데이터 흐름 (cuFile 경로): 스토리지 블록 → NVMe DMA 엔진 → PCIe P2P → GPU Device Memory(cu_mem_ptr).
 *                            CPU 호스트 메모리와 페이지 캐시를 우회한다 (GDS의 핵심 이점).
 * 데이터 흐름 (POSIX 경로): 스토리지 → 커널 페이지 캐시 → xfer_buf(호스트) → cudaMemcpy → cu_mem_ptr(GPU).
 *                           baseline 비교용이며, cudaMemcpy의 오버헤드를 의도적으로 포함한다.
 * 공유 상태: 전역 running/cufile_initialized/running_lock (모든 잡 스레드 공유 — 드라이버를 첫 잡이
 *           열고 마지막 잡이 닫도록 reference count). libcufile_options는 잡 단위(td->eo),
 *           fio_libcufile_data는 파일 단위(fio_file->engine_data).
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_libcufile_init(): 첫 워커만 cuFileDriverOpen, 서브잡별 GPU 선택 + cudaSetDevice.
 * - fio_libcufile_cleanup(): 마지막 워커가 cuFileDriverClose.
 * - fio_libcufile_iomem_alloc()/_free(): cudaMalloc+cuFileBufRegister / 역순 해제.
 * - fio_libcufile_open_file()/_close_file(): fd→CUfileHandle 변환(Register/Deregister).
 * - fio_libcufile_queue(): 동기 I/O 경로 — cuFileRead/Write 또는 pread/pwrite+cudaMemcpy.
 * - fio_libcufile_pre_write()/_post_read(): verify 모드나 POSIX 경로에서 H↔D cudaMemcpy로 데이터 이동.
 * - fio_libcufile_find_gpu_id(): subjob_number % id_count 라운드로빈으로 GPU 할당.
 * - struct libcufile_options: 잡당 옵션(gpu_ids, cuda_io, cu_mem_ptr, junk_buf, my_gpu_id, total_mem).
 * - struct fio_libcufile_data: 파일당 cuFile 핸들 페어(CUfileDescr_t + CUfileHandle_t).
 */

/*
 * Copyright (c)2020 System Fabric Works, Inc. All Rights Reserved.
 * mailto:info@systemfabricworks.com
 *
 * License: GPLv2, see COPYING.
 *
 * libcufile engine
 *
 * fio I/O engine using the NVIDIA cuFile API.
 *
 */

#include <stdlib.h>          /* [한국어] calloc/free/atoi/strdup/strsep — 옵션 파싱과 버퍼 할당 */
#include <unistd.h>          /* [한국어] pread/pwrite/fsync/fdatasync — POSIX 분기용 동기 I/O */
#include <errno.h>           /* [한국어] errno 전역 — 시스템 호출 실패 원인 전파 */
#include <string.h>          /* [한국어] strchr/strsep — gpu_ids 콜론 구분 파싱 */
#include <sys/time.h>        /* [한국어] 시간 관련 매크로 (일부 fio 헤더 의존성 충족) */
#include <sys/resource.h>    /* [한국어] getrlimit 등 리소스 한계 (fio 헤더 간접 의존) */
#include <cufile.h>          /* [한국어] NVIDIA libcufile 공개 API — cuFileDriverOpen/Register/Read/Write 등 */
#include <cuda.h>            /* [한국어] CUDA Driver API 헤더 (일부 타입 정의 공유) */
#include <cuda_runtime.h>    /* [한국어] CUDA Runtime API — cudaMalloc/Free/Memcpy/SetDevice 등 */
#include <pthread.h>         /* [한국어] 모든 fio 잡 스레드가 공유하는 드라이버 refcount를 보호하는 mutex용 */

#include "../fio.h"          /* [한국어] fio 코어 타입: thread_data, io_u, fio_file, ioengine_ops, ddir 등 */
#include "../lib/pow2.h"     /* [한국어] 2의 거듭제곱 유틸 (일부 옵션 검증에 사용) */
#include "../optgroup.h"     /* [한국어] FIO_OPT_G_LIBCUFILE 등 옵션 그룹 enum */
#include "../lib/memalign.h" /* [한국어] 메모리 정렬 유틸 (cuFile은 4KB 정렬 필수) */

/* [한국어] 값 v가 4KB(0x1000) 경계에 정렬되어 있는지 확인.
 *          cuFileRead/Write는 xfer_buflen과 gpu_offset 모두 4KB 정렬을 요구하며,
 *          이는 NVMe LBA 블록 경계 + nvidia-fs 드라이버의 DMA 제약 때문이다. */
#define ALIGNED_4KB(v) (((v) & 0x0fff) == 0)

/* [한국어] 정렬 경고를 잡당 1회만 출력하기 위한 비트마스크 플래그 — 반복 로그 스팸 방지 */
#define LOGGED_BUFLEN_NOT_ALIGNED     0x01  /* [한국어] buflen 4KB 미정렬 경고 1회 출력 완료 */
#define LOGGED_GPU_OFFSET_NOT_ALIGNED 0x02  /* [한국어] gpu_offset 4KB 미정렬 경고 1회 출력 완료 */
#define GPU_ID_SEP ":"                      /* [한국어] gpu_dev_ids 옵션 구분자 — "0:1:2" 형식 파싱용 */

/* [한국어] cuda_io 옵션의 내부 정수 값 — cuFile 직접 P2P DMA vs 비교용 POSIX I/O */
enum {
	IO_CUFILE    = 1,   /* [한국어] cuFileRead/Write — nvidia-fs를 통한 GPU P2P DMA */
	IO_POSIX     = 2    /* [한국어] pread/pwrite + cudaMemcpy — 전통 경로 (baseline 측정용) */
};

/*
 * [한국어] cuFile 엔진 전용 잡 옵션 구조체.
 *          td->eo에 저장되며 잡(스레드) 단위로 유일하다. libcufile_options는 옵션 파싱 결과와
 *          잡 런타임 상태(현재 GPU, GPU 메모리 포인터, junk 버퍼)를 함께 보관한다.
 */
struct libcufile_options {
	struct thread_data *td;
	/* [한국어] 이 옵션 블록이 속한 fio 잡(스레드)의 thread_data 역포인터.
	 * 설정자: fio 옵션 파서(options.c)가 엔진별 option 구조를 할당할 때 자동으로 설정.
	 * 읽는 자: (현재 엔진에서는 직접 사용되지 않으나 옵션 시스템의 관례로 보존).
	 * 값 범위: 유효 thread_data 포인터, 잡 생명 주기 동안 고정.
	 * 동기화: 잡 단위 — 다른 잡이 접근하지 않음. */

	char *gpu_ids;
	/* [한국어] gpu_dev_ids 옵션에서 파싱한 원본 문자열 (예: "0:1:2:3").
	 * 설정자: FIO_OPT_STR_STORE로 옵션 파서가 strdup 결과를 저장.
	 * 읽는 자: fio_libcufile_find_gpu_id()가 strdup 후 strsep으로 파싱하여 서브잡에 라운드로빈 할당.
	 * 값 범위: NULL(기본값 = GPU 0) 또는 콜론 구분 정수 리스트.
	 * 동기화: 읽기 전용이므로 락 불필요. */

	void *cu_mem_ptr;
	/* [한국어] cudaMalloc으로 할당된 GPU Device Memory의 디바이스 포인터.
	 * 설정자: fio_libcufile_iomem_alloc()에서 cudaMalloc + cuFileBufRegister로 pin 완료 후 저장.
	 * 읽는 자: queue()에서 cuFileRead/Write의 devPtr_base 인자로, pre_write/post_read의 cudaMemcpy 대상으로.
	 * 값 범위: GPU 가상 주소 — CPU에서 역참조 절대 금지. total_mem 크기의 연속 영역.
	 * 동기화: 잡 단위 전용이나 GPU 내부에서는 CUDA 스트림(default stream)이 직렬화. */

	void *junk_buf;
	/* [한국어] IO_POSIX 경로에서 WRITE 시 GPU→호스트 cudaMemcpy의 목적지로 쓰이는 호스트 버퍼.
	 * 설정자: iomem_alloc에서 cuda_io==IO_POSIX인 경우에만 calloc으로 할당.
	 * 읽는 자: pre_write()가 cu_mem_ptr의 데이터를 D2H로 복사해 이 버퍼에 둠 (cudaMemcpy 오버헤드 재현용).
	 * 값 범위: total_mem 바이트 호스트 힙 영역 또는 NULL (cuFile 경로에서는 미사용).
	 * 동기화: 잡 단위 전용. */

	int my_gpu_id;
	/* [한국어] 이 서브잡이 cudaSetDevice로 바인딩한 GPU 인덱스.
	 * 설정자: fio_libcufile_init()이 fio_libcufile_find_gpu_id()로 라운드로빈 선택.
	 * 읽는 자: init의 cudaSetDevice 호출 + iomem_alloc의 dprint(FD_MEM) 로그.
	 * 값 범위: 0 이상의 시스템 CUDA 디바이스 인덱스, 음수는 에러.
	 * 동기화: 잡 단위 전용. */

	unsigned int cuda_io;
	/* [한국어] cuda_io 옵션의 내부 enum 값 (IO_CUFILE 또는 IO_POSIX).
	 * 설정자: FIO_OPT_STR 파서가 posval 매칭으로 설정, 기본값 "cufile".
	 * 읽는 자: init/queue/iomem_alloc/iomem_free/cleanup/open_file의 모든 분기 조건.
	 * 값 범위: {IO_CUFILE=1, IO_POSIX=2}.
	 * 동기화: 옵션 파싱 후 읽기 전용. */

	size_t total_mem;
	/* [한국어] iomem_alloc에 전달된 총 할당 크기 — cu_mem_ptr과 junk_buf 크기 기준.
	 * 설정자: fio_libcufile_iomem_alloc()이 인자로 받아 저장.
	 * 읽는 자: queue()의 assert(gpu_offset + xfer_buflen <= total_mem)로 OOB 방지.
	 * 값 범위: fio가 계산한 전체 io_u 버퍼 풀 크기 (io_u 개수 * max_bs).
	 * 동기화: 할당 후 불변. */

	int logged;
	/* [한국어] 정렬 경고 1회 출력을 위한 비트마스크 (LOGGED_BUFLEN_NOT_ALIGNED 등).
	 * 설정자: queue()가 정렬 위반을 발견하면 해당 비트를 set (로그 스팸 방지).
	 * 읽는 자: queue()가 경고 출력 전에 비트 체크.
	 * 값 범위: 0~0x03 비트 조합.
	 * 동기화: 단일 잡 스레드 내 — 다른 스레드가 읽지 않음. */
};

/*
 * [한국어] 파일별 cuFile 핸들 상태 구조체.
 *          fio_file->engine_data에 저장되며, 파일이 열려 있는 동안만 유효하다. cuFile API는
 *          일반 파일 fd를 CUfileHandle_t로 "등록"해야 내부적으로 nvidia-fs가 해당 파일의
 *          블록 매핑/FS 콜백을 캐시할 수 있다 (P2P DMA는 fd가 아닌 handle을 통해 수행).
 */
struct fio_libcufile_data {
	CUfileDescr_t cf_descr;
	/* [한국어] cuFileHandleRegister에 전달하는 디스크립터 — 파일 유형과 raw fd를 묶는다.
	 * 설정자: open_file()이 handle.fd=f->fd, type=OPAQUE_FD로 채움.
	 * 읽는 자: cuFileHandleRegister가 내부적으로 복제하므로 이후 참조는 libcufile 내부에서만.
	 * 값 범위: CU_FILE_HANDLE_TYPE_OPAQUE_FD (일반 POSIX fd) / NVME / NVMEOF / NVMEOF_SNAP 등.
	 * 동기화: 파일 오픈 시 1회 설정, 이후 불변. */

	CUfileHandle_t cf_handle;
	/* [한국어] cuFileHandleRegister가 반환한 불투명 핸들 — cuFileRead/Write에 전달.
	 * 설정자: open_file()의 cuFileHandleRegister.
	 * 읽는 자: queue()의 cuFileRead/cuFileWrite 첫 인자, close_file()의 Deregister.
	 * 값 범위: libcufile 내부 관리 포인터 — 직접 필드 접근 절대 금지.
	 * 동기화: nvidia-fs 드라이버가 내부적으로 핸들 상태를 관리 (fio측 락 불필요). */
};

/*
 * [한국어] fio 옵션 시스템에 등록되는 엔진 옵션 배열 — 잡 파일/CLI에서 파싱된다.
 */
static struct fio_option options[] = {
	{
		.name	  = "gpu_dev_ids",                                    /* [한국어] 옵션 키 이름 */
		.lname	  = "libcufile engine gpu dev ids",                  /* [한국어] 긴 이름 (help 출력용) */
		.type	  = FIO_OPT_STR_STORE,                                /* [한국어] 문자열 그대로 저장 */
		.off1	  = offsetof(struct libcufile_options, gpu_ids),      /* [한국어] gpu_ids 필드에 기록 */
		.help	  = "GPU IDs, one per subjob, separated by " GPU_ID_SEP, /* [한국어] 도움말 */
		.category = FIO_OPT_C_ENGINE,                                 /* [한국어] 엔진 카테고리 */
		.group	  = FIO_OPT_G_LIBCUFILE,                              /* [한국어] libcufile 그룹 */
	},
	{
		.name	  = "cuda_io",                                        /* [한국어] I/O 경로 선택 */
		.lname	  = "libcufile cuda io",
		.type	  = FIO_OPT_STR,                                      /* [한국어] 문자열→enum 매핑 */
		.off1	  = offsetof(struct libcufile_options, cuda_io),
		.help	  = "Type of I/O to use with CUDA",
		.def      = "cufile",                                         /* [한국어] 기본값: GDS 경로 */
		.posval   = {
			    { .ival = "cufile",                                   /* [한국어] 문자열 "cufile" */
			      .oval = IO_CUFILE,                                  /* [한국어] → 정수 1 */
			      .help = "libcufile nvidia-fs"
			    },
			    { .ival = "posix",                                    /* [한국어] 비교용 POSIX 경로 */
			      .oval = IO_POSIX,                                   /* [한국어] → 정수 2 */
			      .help = "POSIX I/O"
			    }
		},
		.category = FIO_OPT_C_ENGINE,
		.group	  = FIO_OPT_G_LIBCUFILE,
	},
	{
		.name	 = NULL,                                              /* [한국어] 옵션 배열 종료 센티넬 */
	},
};

/* [한국어] 전역 공유 상태 — 모든 libcufile 잡 스레드가 드라이버를 공유한다.
 *          첫 잡이 cuFileDriverOpen하고 마지막 잡이 cuFileDriverClose하도록 refcount 패턴을 구현. */
static int running = 0;                                      /* [한국어] 현재 활성 libcufile 잡 수 (refcount) */
static int cufile_initialized = 0;                           /* [한국어] 드라이버가 성공적으로 열려 있는가 */
static pthread_mutex_t running_lock = PTHREAD_MUTEX_INITIALIZER; /* [한국어] running/cufile_initialized 보호용 뮤텍스 */

/*
 * [한국어] CUDA Runtime API 호출 래퍼 매크로.
 *          호출 결과가 cudaSuccess가 아니면 함수명/라인/errno/오류명을 log_err로 출력하고 rc=-1.
 *          do-while(0) 이디엄으로 다중문 매크로 안전성 확보.
 */
#define check_cudaruntimecall(fn, rc)                                               \
	do {                                                                        \
		cudaError_t res = fn;                                               \
		if (res != cudaSuccess) {                                           \
			const char *str = cudaGetErrorName(res);                    \
			log_err("cuda runtime api call failed %s:%d : err=%d:%s\n", \
				#fn, __LINE__, res, str);                           \
			rc = -1;                                                    \
		} else                                                              \
			rc = 0;                                                     \
	} while(0)

/*
 * [한국어]
 * fio_libcufile_get_cuda_error - CUfileError_t를 사람이 읽을 수 있는 문자열로 변환.
 *
 * @st: cuFile API가 반환한 상태 구조체 (err, cu_err 필드).
 * @return: cufileop_status_error()로 변환된 오류 문자열 또는 "unknown".
 *
 * cuFile API는 CUDA 런타임 에러와 cuFile 자체 에러를 모두 담는 CUfileError_t를 반환한다.
 * IS_CUFILE_ERR 매크로로 cuFile 에러 여부를 판단하고, 맞다면 공식 변환 함수를 사용한다.
 * 실행 컨텍스트: 잡 스레드. 호출자: init/open_file/iomem_alloc의 에러 로깅.
 */
static const char *fio_libcufile_get_cuda_error(CUfileError_t st)
{
	if (IS_CUFILE_ERR(st.err))                          /* [한국어] cuFile 계열 에러인지 판정 */
		return cufileop_status_error(st.err);       /* [한국어] 공식 에러 문자열 테이블 조회 */
	return "unknown";                                   /* [한국어] 알 수 없는 코드면 placeholder */
}

/*
 * Assign GPU to subjob roundrobin, similar to how multiple
 * entries in 'directory' are handled by fio.
 */
/*
 * [한국어]
 * fio_libcufile_find_gpu_id - 서브잡 번호로 gpu_dev_ids 리스트에서 GPU를 라운드로빈 선택.
 *
 * @td: 이 서브잡의 thread_data — td->subjob_number와 td->eo(libcufile_options)를 사용.
 * @return: 선택된 GPU 인덱스 (기본 0) 또는 -1 (strdup 실패).
 *
 * gpu_dev_ids="0:1:2" + subjob_number=5 → "0"(5%3=2... 실제 2번째) 등으로 매핑.
 * 잡당 고유 GPU를 주어 멀티 GPU 환경에서 병렬 I/O를 측정할 수 있다.
 * 실행 컨텍스트: init() 호출 시 잡 스레드. 호출자: fio_libcufile_init.
 *
 * 호출 체인: fio_libcufile_init → [fio_libcufile_find_gpu_id] → strdup/strchr/strsep/atoi
 */
static int fio_libcufile_find_gpu_id(struct thread_data *td)
{
	struct libcufile_options *o = td->eo;   /* [한국어] 엔진 옵션 포인터 획득 */
	int gpu_id = 0;                         /* [한국어] 기본값 GPU 0 */

	if (o->gpu_ids != NULL) {               /* [한국어] 옵션이 지정된 경우에만 파싱 */
		char *gpu_ids, *pos, *cur;
		int i, id_count, gpu_idx;

		/* [한국어] 리스트 내 GPU ID 개수를 ':' 구분자로 센다 */
		for (id_count = 0, cur = o->gpu_ids; cur != NULL; id_count++) {
			cur = strchr(cur, GPU_ID_SEP[0]);  /* [한국어] 다음 ':' 위치 탐색 */
			if (cur != NULL)
				cur++;                      /* [한국어] ':' 다음 문자로 이동 */
		}

		gpu_idx = td->subjob_number % id_count; /* [한국어] 라운드로빈 인덱스 */

		pos = gpu_ids = strdup(o->gpu_ids);     /* [한국어] strsep가 파괴적이므로 복사 */
		if (gpu_ids == NULL) {
			log_err("strdup(gpu_ids): err=%d\n", errno);
			return -1;                           /* [한국어] 메모리 부족 */
		}

		i = 0;
		/* [한국어] strsep으로 gpu_idx번째 토큰까지 진행 */
		while (pos != NULL && i <= gpu_idx) {
			i++;
			cur = strsep(&pos, GPU_ID_SEP);     /* [한국어] ':' 기준 분리, pos 전진 */
		}

		if (cur)
			gpu_id = atoi(cur);                 /* [한국어] 문자열→정수 변환 */

		free(gpu_ids);                          /* [한국어] strdup한 사본 해제 */
	}

	return gpu_id;                              /* [한국어] 선택된 GPU ID 반환 */
}

/*
 * [한국어]
 * fio_libcufile_init - 엔진 초기화: 첫 워커만 드라이버 open, 서브잡별 GPU 바인딩.
 *
 * @td: 이 잡의 thread_data.
 * @return: 0 성공, 1 실패.
 *
 * running_lock 보호 하에 running==0이면 cuFileDriverOpen()을 1회 호출해 nvidia-fs를 연다.
 * 이후 라운드로빈으로 my_gpu_id를 정하고 cudaSetDevice로 현재 스레드의 CUDA 컨텍스트를 그 GPU에
 * 바인딩한다 — 이후 cudaMalloc/cuFileRead 등 모든 CUDA 호출이 해당 GPU에서 동작한다.
 * 실행 컨텍스트: 잡 스레드 시작 직후 (td_io_init에서 호출).
 *
 * 호출 체인: backend.c td_io_init → [fio_libcufile_init] → cuFileDriverOpen/cudaSetDevice
 */
static int fio_libcufile_init(struct thread_data *td)
{
	struct libcufile_options *o = td->eo;   /* [한국어] 엔진 옵션 */
	CUfileError_t status;
	int initialized;
	int rc;

	pthread_mutex_lock(&running_lock);      /* [한국어] 전역 refcount 보호 시작 */
	if (running == 0) {                      /* [한국어] 내가 첫 번째 워커 */
		assert(cufile_initialized == 0);     /* [한국어] 초기 상태 sanity */
		if (o->cuda_io == IO_CUFILE) {       /* [한국어] POSIX 경로는 드라이버 불필요 */
			/* only open the driver if this is the first worker thread */
			status = cuFileDriverOpen();    /* [한국어] nvidia-fs 커널 모듈에 IOCTL (드라이버 초기화) */
			if (status.err != CU_FILE_SUCCESS)
				log_err("cuFileDriverOpen: err=%d:%s\n", status.err,
					fio_libcufile_get_cuda_error(status));
			else
				cufile_initialized = 1;  /* [한국어] 성공 시 플래그 set */
		}
	}
	running++;                               /* [한국어] refcount 증가 (나의 참여 등록) */
	initialized = cufile_initialized;        /* [한국어] 락 해제 전 로컬 복사 (이후 사용) */
	pthread_mutex_unlock(&running_lock);     /* [한국어] 임계 구역 종료 */

	if (o->cuda_io == IO_CUFILE && !initialized) /* [한국어] cuFile 필요한데 드라이버 실패 */
		return 1;

	o->my_gpu_id = fio_libcufile_find_gpu_id(td); /* [한국어] 서브잡용 GPU 선택 */
	if (o->my_gpu_id < 0)                          /* [한국어] strdup 실패 등 */
		return 1;

	dprint(FD_MEM, "Subjob %d uses GPU %d\n", td->subjob_number, o->my_gpu_id);
	check_cudaruntimecall(cudaSetDevice(o->my_gpu_id), rc); /* [한국어] 현재 스레드 CUDA 컨텍스트를 해당 GPU로 */
	if (rc != 0)
		return 1;

	return 0;                                /* [한국어] 성공 */
}

/*
 * [한국어]
 * fio_libcufile_pre_write - WRITE 직전 GPU 버퍼 준비 (verify 또는 POSIX 경로 전용).
 *
 * @td: 잡 thread_data.
 * @o: 엔진 옵션 (cu_mem_ptr, junk_buf 포함).
 * @io_u: 현재 처리 중인 I/O 유닛 (xfer_buf = 호스트 데이터, xfer_buflen).
 * @gpu_offset: cu_mem_ptr 기준 이 io_u 슬롯의 GPU 메모리 오프셋.
 * @return: 0 성공, 그 외 에러.
 *
 * IO_CUFILE + verify: verify 데이터(호스트 xfer_buf)를 GPU로 H2D 복사해야 디스크에 쓸 내용이 맞다.
 *   (non-verify 경우 GPU 메모리에 이미 데이터가 있다고 가정 — 실제 GDS 앱 흐름 재현).
 * IO_POSIX: 실제로는 호스트 xfer_buf에서 pwrite하지만, 실제 CUDA 앱의 cudaMemcpy 오버헤드를
 *   재현하기 위해 cu_mem_ptr → junk_buf로 D2H 복사를 수행 (성능 비교 공정성).
 * 실행 컨텍스트: 잡 스레드, queue() 내부.
 *
 * 호출 체인: fio_libcufile_queue (DDIR_WRITE) → [fio_libcufile_pre_write] → cudaMemcpy
 */
static inline int fio_libcufile_pre_write(struct thread_data *td,
					  struct libcufile_options *o,
					  struct io_u *io_u,
					  size_t gpu_offset)
{
	int rc = 0;

	if (o->cuda_io == IO_CUFILE) {           /* [한국어] GDS 경로 */
		if (td->o.verify) {                   /* [한국어] verify 모드일 때만 데이터 이동 */
			/*
			  Data is being verified, copy the io_u buffer to GPU memory.
			  This isn't done in the non-verify case because the data would
			  already be in GPU memory in a normal cuFile application.
			*/
			/* [한국어] Host→Device DMA (verify 시드를 GPU로) */
			check_cudaruntimecall(cudaMemcpy(((char*) o->cu_mem_ptr) + gpu_offset,
							 io_u->xfer_buf,
							 io_u->xfer_buflen,
							 cudaMemcpyHostToDevice), rc);
			if (rc != 0) {
				log_err("DDIR_WRITE cudaMemcpy H2D failed\n");
				io_u->error = EIO;
			}
		}
	} else if (o->cuda_io == IO_POSIX) {

		/*
		  POSIX I/O is being used, the data has to be copied out of the
		  GPU into a CPU buffer. GPU memory doesn't contain the actual
		  data to write, copy the data to the junk buffer. The purpose
		  of this is to add the overhead of cudaMemcpy() that would be
		  present in a POSIX I/O CUDA application.
		*/
		/* [한국어] GPU→호스트 복사 — 실제 쓰기는 xfer_buf가 쓰이지만, 측정 공정성을 위해 D2H 오버헤드 재현 */
		check_cudaruntimecall(cudaMemcpy(o->junk_buf + gpu_offset,
						 ((char*) o->cu_mem_ptr) + gpu_offset,
						 io_u->xfer_buflen,
						 cudaMemcpyDeviceToHost), rc);
		if (rc != 0) {
			log_err("DDIR_WRITE cudaMemcpy D2H failed\n");
			io_u->error = EIO;
		}
	} else {
		log_err("Illegal CUDA IO type: %d\n", o->cuda_io);
		assert(0);                           /* [한국어] 불가능한 분기 */
		rc = EINVAL;
	}

	return rc;
}

/*
 * [한국어]
 * fio_libcufile_post_read - READ 직후 GPU 버퍼 후처리 (verify 또는 POSIX 경로 전용).
 *
 * @td: 잡 thread_data.
 * @o: 엔진 옵션.
 * @io_u: 방금 read 완료된 io_u.
 * @gpu_offset: cu_mem_ptr 기준 오프셋.
 * @return: 0 성공 또는 에러.
 *
 * IO_CUFILE + verify: GPU에 있는 읽은 데이터를 호스트 xfer_buf로 D2H — verify 엔진이 CRC 검증할 수 있도록.
 * IO_POSIX: pread가 xfer_buf에 채운 데이터를 cu_mem_ptr로 H2D — 실제 CUDA 앱이 GPU에서 소비할 수 있게 복사.
 * 실행 컨텍스트: 잡 스레드, queue() 내부 READ 완료 직후.
 *
 * 호출 체인: fio_libcufile_queue (DDIR_READ 성공) → [fio_libcufile_post_read] → cudaMemcpy
 */
static inline int fio_libcufile_post_read(struct thread_data *td,
					  struct libcufile_options *o,
					  struct io_u *io_u,
					  size_t gpu_offset)
{
	int rc = 0;

	if (o->cuda_io == IO_CUFILE) {
		if (td->o.verify) {
			/* Copy GPU memory to CPU buffer for verify */
			/* [한국어] GPU에 읽어 온 데이터를 호스트로 복사 → verify 엔진이 CRC 검증 */
			check_cudaruntimecall(cudaMemcpy(io_u->xfer_buf,
							 ((char*) o->cu_mem_ptr) + gpu_offset,
							 io_u->xfer_buflen,
							 cudaMemcpyDeviceToHost), rc);
			if (rc != 0) {
				log_err("DDIR_READ cudaMemcpy D2H failed\n");
				io_u->error = EIO;
			}
		}
	} else if (o->cuda_io == IO_POSIX) {
		/* POSIX I/O read, copy the CPU buffer to GPU memory */
		/* [한국어] pread가 호스트로 읽은 데이터를 GPU로 올림 — 실제 CUDA 앱이 GPU에서 처리할 수 있도록 */
		check_cudaruntimecall(cudaMemcpy(((char*) o->cu_mem_ptr) + gpu_offset,
						 io_u->xfer_buf,
						 io_u->xfer_buflen,
						 cudaMemcpyHostToDevice), rc);
		if (rc != 0) {
			log_err("DDIR_READ cudaMemcpy H2D failed\n");
			io_u->error = EIO;
		}
	} else {
		log_err("Illegal CUDA IO type: %d\n", o->cuda_io);
		assert(0);
		rc = EINVAL;
	}

	return rc;
}

/*
 * [한국어]
 * fio_libcufile_queue - 동기 I/O 제출 콜백 (FIO_SYNCIO 계약).
 *
 * @td: 잡 thread_data.
 * @io_u: 제출할 I/O 유닛 (offset, xfer_buf, xfer_buflen, ddir, file).
 * @return: FIO_Q_COMPLETED 항상 반환 — 동기 엔진이므로 호출 즉시 완료.
 *
 * ddir 분기: SYNC/DATASYNC는 fsync/fdatasync. READ/WRITE는 gpu_offset 계산 후 4KB 정렬
 * 경고 + pre_write(WRITE) + while(remaining) 루프에서 cuFileRead/Write 또는 pread/pwrite를
 * 부분 전송 지원과 함께 반복. POSIX 경로는 xfer_buf를, cuFile 경로는 cu_mem_ptr을 DMA 대상으로
 * 넘기며, cuFile은 devPtr_offset(gpu_offset+xfered)로 GPU 내부 오프셋을 전달한다.
 * 실행 컨텍스트: 잡 스레드, backend의 io 루프.
 *
 * 호출 체인: backend.c td_io_queue → ioengines.c td_io_queue → [fio_libcufile_queue]
 *            → cuFileRead/Write | pread/pwrite | fsync/fdatasync
 */
static enum fio_q_status fio_libcufile_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct libcufile_options *o = td->eo;                      /* [한국어] 엔진 옵션 */
	struct fio_libcufile_data *fcd = FILE_ENG_DATA(io_u->file);/* [한국어] 파일당 cuFile 핸들 */
	unsigned long long io_offset;
	ssize_t sz;
	ssize_t remaining;
	size_t xfered;
	size_t gpu_offset;
	int rc;

	/* [한국어] cuFile 경로인데 핸들이 등록 안 됐으면 즉시 EINVAL */
	if (o->cuda_io == IO_CUFILE && fcd == NULL) {
		io_u->error = EINVAL;
		td_verror(td, EINVAL, "xfer");
		return FIO_Q_COMPLETED;
	}

	fio_ro_check(td, io_u);                                    /* [한국어] read-only 위반 감지 */

	switch(io_u->ddir) {
	case DDIR_SYNC:                                             /* [한국어] fsync — 메타+데이터 플러시 */
		rc = fsync(io_u->file->fd);
		if (rc != 0) {
			io_u->error = errno;
			log_err("fsync: err=%d\n", errno);
		}
		break;

	case DDIR_DATASYNC:                                         /* [한국어] fdatasync — 데이터만 플러시 */
		rc = fdatasync(io_u->file->fd);
		if (rc != 0) {
			io_u->error = errno;
			log_err("fdatasync: err=%d\n", errno);
		}
		break;

	case DDIR_READ:
	case DDIR_WRITE:
		/*
		  There may be a better way to calculate gpu_offset. The intent is
		  that gpu_offset equals the the difference between io_u->xfer_buf and
		  the page-aligned base address for io_u buffers.
		*/
		/* [한국어] io_u->index로 GPU 메모리 내 슬롯 오프셋 계산 — 각 io_u는 고정 슬롯 점유 */
		gpu_offset = io_u->index * io_u->xfer_buflen;
		io_offset = io_u->offset;                                /* [한국어] 파일 내 시작 오프셋 */
		remaining = io_u->xfer_buflen;                           /* [한국어] 남은 바이트 */

		xfered = 0;                                              /* [한국어] 누적 전송 바이트 */
		sz = 0;

		assert(gpu_offset + io_u->xfer_buflen <= o->total_mem);  /* [한국어] OOB 방지 */

		if (o->cuda_io == IO_CUFILE) {
			/* [한국어] cuFile은 4KB 정렬 필수 — NVMe LBA + nvidia-fs DMA 제약 */
			if (!(ALIGNED_4KB(io_u->xfer_buflen) ||
			      (o->logged & LOGGED_BUFLEN_NOT_ALIGNED))) {
				log_err("buflen not 4KB-aligned: %llu\n", io_u->xfer_buflen);
				o->logged |= LOGGED_BUFLEN_NOT_ALIGNED; /* [한국어] 1회만 경고 */
			}

			if (!(ALIGNED_4KB(gpu_offset) ||
			      (o->logged & LOGGED_GPU_OFFSET_NOT_ALIGNED))) {
				log_err("gpu_offset not 4KB-aligned: %lu\n", gpu_offset);
				o->logged |= LOGGED_GPU_OFFSET_NOT_ALIGNED;
			}
		}

		if (io_u->ddir == DDIR_WRITE)                            /* [한국어] WRITE면 GPU 버퍼 준비 */
			rc = fio_libcufile_pre_write(td, o, io_u, gpu_offset);

		if (io_u->error != 0)
			break;

		while (remaining > 0) {                                  /* [한국어] 부분 전송 대비 루프 */
			assert(gpu_offset + xfered <= o->total_mem);
			if (io_u->ddir == DDIR_READ) {
				if (o->cuda_io == IO_CUFILE) {
					/* [한국어] cuFileRead: nvidia-fs가 NVMe PRP를 GPU 물리주소로 채워 P2P DMA 수행 */
					sz = cuFileRead(fcd->cf_handle, o->cu_mem_ptr, remaining,
							io_offset + xfered, gpu_offset + xfered);
					if (sz == -1) {                   /* [한국어] -1은 POSIX errno 스타일 */
						io_u->error = errno;
						log_err("cuFileRead: err=%d\n", errno);
					} else if (sz < 0) {              /* [한국어] 음수 cuFile 상태 코드 */
						io_u->error = EIO;
						log_err("cuFileRead: err=%ld:%s\n", sz,
							cufileop_status_error(-sz));
					}
				} else if (o->cuda_io == IO_POSIX) {
					/* [한국어] POSIX 비교 경로 — 호스트 버퍼로 pread */
					sz = pread(io_u->file->fd, ((char*) io_u->xfer_buf) + xfered,
						   remaining, io_offset + xfered);
					if (sz < 0) {
						io_u->error = errno;
						log_err("pread: err=%d\n", errno);
					}
				} else {
					log_err("Illegal CUDA IO type: %d\n", o->cuda_io);
					io_u->error = -1;
					assert(0);
				}
			} else if (io_u->ddir == DDIR_WRITE) {
				if (o->cuda_io == IO_CUFILE) {
					/* [한국어] cuFileWrite: GPU → NVMe P2P DMA (호스트 메모리 미경유) */
					sz = cuFileWrite(fcd->cf_handle, o->cu_mem_ptr, remaining,
							 io_offset + xfered, gpu_offset + xfered);
					if (sz == -1) {
						io_u->error = errno;
						log_err("cuFileWrite: err=%d\n", errno);
					} else if (sz < 0) {
						io_u->error = EIO;
						log_err("cuFileWrite: err=%ld:%s\n", sz,
							cufileop_status_error(-sz));
					}
				} else if (o->cuda_io == IO_POSIX) {
					/* [한국어] POSIX 비교 경로 — pwrite */
					sz = pwrite(io_u->file->fd,
						    ((char*) io_u->xfer_buf) + xfered,
						    remaining, io_offset + xfered);
					if (sz < 0) {
						io_u->error = errno;
						log_err("pwrite: err=%d\n", errno);
					}
				} else {
					log_err("Illegal CUDA IO type: %d\n", o->cuda_io);
					io_u->error = -1;
					assert(0);
				}
			} else {
				log_err("not DDIR_READ or DDIR_WRITE: %d\n", io_u->ddir);
				io_u->error = -1;
				assert(0);
				break;
			}

			if (io_u->error != 0)
				break;

			remaining -= sz;                         /* [한국어] 남은 바이트 감소 */
			xfered += sz;                            /* [한국어] 누적 전송 증가 */

			if (remaining != 0)                      /* [한국어] 부분 전송 발생 — 정보 로그 */
				log_info("Incomplete %s: %ld bytes remaining\n",
					 io_u->ddir == DDIR_READ? "read" : "write", remaining);
		}

		if (io_u->error != 0)
			break;

		if (io_u->ddir == DDIR_READ)                     /* [한국어] READ 성공 후 후처리 */
			rc = fio_libcufile_post_read(td, o, io_u, gpu_offset);
		break;

	default:
		io_u->error = EINVAL;                            /* [한국어] 지원 안 하는 ddir */
		break;
	}

	if (io_u->error != 0) {
		log_err("IO failed\n");
		td_verror(td, io_u->error, "xfer");              /* [한국어] fio에 잡 레벨 에러 기록 */
	}

	return FIO_Q_COMPLETED;                              /* [한국어] 동기 엔진 — 항상 즉시 완료 */
}

/*
 * [한국어]
 * fio_libcufile_open_file - 파일 열기 + cuFileHandleRegister.
 *
 * @td: 잡 thread_data.
 * @f: fio_file — fd, 파일명 등.
 * @return: 0 성공, 0이 아닌 에러 코드.
 *
 * generic_open_file로 fd를 얻은 뒤, IO_CUFILE이면 fcd를 calloc하고 cuFileHandleRegister로
 * fd→CUfileHandle 매핑을 등록해 nvidia-fs가 해당 파일의 블록 맵/FS 콜백을 준비하도록 한다.
 * 실행 컨텍스트: 잡 스레드 파일 셋업 단계.
 *
 * 호출 체인: backend.c setup → td_io_open_file → [fio_libcufile_open_file]
 *            → generic_open_file (open(2)) → cuFileHandleRegister
 */
static int fio_libcufile_open_file(struct thread_data *td, struct fio_file *f)
{
	struct libcufile_options *o = td->eo;
	struct fio_libcufile_data *fcd = NULL;
	int rc;
	CUfileError_t status;

	rc = generic_open_file(td, f);                  /* [한국어] 일반 open(2) + 카운터 */
	if (rc)
		return rc;

	if (o->cuda_io == IO_CUFILE) {
		fcd = calloc(1, sizeof(*fcd));              /* [한국어] 파일당 핸들 상태 할당 */
		if (fcd == NULL) {
			rc = ENOMEM;
			goto exit_err;
		}

		fcd->cf_descr.handle.fd = f->fd;            /* [한국어] POSIX fd를 디스크립터에 삽입 */
		fcd->cf_descr.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD; /* [한국어] 일반 파일 모드 */
		/* [한국어] nvidia-fs에 파일 등록 — 내부적으로 FS 블록맵/콜백을 준비 */
		status = cuFileHandleRegister(&fcd->cf_handle, &fcd->cf_descr);
		if (status.err != CU_FILE_SUCCESS) {
			log_err("cufile register: err=%d:%s\n", status.err,
				fio_libcufile_get_cuda_error(status));
			rc = EINVAL;
			goto exit_err;
		}
	}

	FILE_SET_ENG_DATA(f, fcd);                      /* [한국어] fio_file->engine_data에 저장 */
	return 0;

exit_err:
	if (fcd) {
		free(fcd);
		fcd = NULL;
	}
	if (f) {
		int rc2 = generic_close_file(td, f);        /* [한국어] 실패 시 open 되돌림 */
		if (rc2)
			log_err("generic_close_file: err=%d\n", rc2);
	}
	return rc;
}

/*
 * [한국어]
 * fio_libcufile_close_file - cuFileHandleDeregister + 일반 close.
 *
 * @td: 잡 thread_data.
 * @f: 닫을 파일.
 * @return: generic_close_file의 반환값.
 *
 * 등록된 cuFile 핸들을 먼저 해제하고 (nvidia-fs 내부 매핑 정리) 뒤이어 fd를 닫는다.
 * 실행 컨텍스트: 잡 종료 단계.
 *
 * 호출 체인: backend.c cleanup → td_io_close_file → [fio_libcufile_close_file]
 */
static int fio_libcufile_close_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_libcufile_data *fcd = FILE_ENG_DATA(f);
	int rc;

	if (fcd != NULL) {
		cuFileHandleDeregister(fcd->cf_handle); /* [한국어] nvidia-fs 매핑 해제 */
		FILE_SET_ENG_DATA(f, NULL);              /* [한국어] 참조 제거 */
		free(fcd);                                /* [한국어] 핸들 상태 구조체 해제 */
	}

	rc = generic_close_file(td, f);              /* [한국어] close(2) */

	return rc;
}

/*
 * [한국어]
 * fio_libcufile_iomem_alloc - GPU 메모리 할당 + cuFile DMA 매핑(pin).
 *
 * @td: 잡 thread_data.
 * @total_mem: 할당할 총 바이트 (fio 버퍼 풀 크기).
 * @return: 0 성공, 1 실패.
 *
 * 1) td->orig_buffer: fio 코어가 io_u->buf를 추적하는 더미 호스트 버퍼(필수 — fio 내부 계산에 쓰임).
 * 2) junk_buf: POSIX 경로 D2H 시뮬레이션용 호스트 버퍼.
 * 3) cu_mem_ptr: cudaMalloc으로 실제 GPU 메모리 할당 + cudaMemset(0xab)로 초기화.
 * 4) cuFileBufRegister: GPU 버퍼를 nvidia-fs에 DMA 매핑 등록 — 이후 cuFileRead/Write가 zero-copy P2P 가능.
 *    cuFileBufRegister 없이는 cuFile API가 내부 bounce 버퍼를 사용해 GDS 이점이 사라진다.
 * 실행 컨텍스트: 잡 스레드 초기화 (init 뒤, I/O 루프 앞).
 *
 * 호출 체인: backend.c allocate_buffers → td_io_iomem_alloc → [fio_libcufile_iomem_alloc]
 *            → calloc/cudaMalloc/cuFileBufRegister
 */
static int fio_libcufile_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	struct libcufile_options *o = td->eo;
	int rc;
	CUfileError_t status;

	o->total_mem = total_mem;               /* [한국어] 잡 옵션에 크기 저장 (queue의 assert 비교용) */
	o->logged = 0;                           /* [한국어] 정렬 경고 플래그 리셋 */
	o->cu_mem_ptr = NULL;
	o->junk_buf = NULL;
	td->orig_buffer = calloc(1, total_mem); /* [한국어] fio 코어가 io_u 베이스 포인터로 관리할 호스트 더미 */
	if (!td->orig_buffer) {
		log_err("orig_buffer calloc failed: err=%d\n", errno);
		goto exit_error;
	}

	if (o->cuda_io == IO_POSIX) {
		o->junk_buf = calloc(1, total_mem); /* [한국어] POSIX D2H 시뮬레이션용 */
		if (o->junk_buf == NULL) {
			log_err("junk_buf calloc failed: err=%d\n", errno);
			goto exit_error;
		}
	}

	dprint(FD_MEM, "Alloc %zu for GPU %d\n", total_mem, o->my_gpu_id);
	/* [한국어] GPU Device Memory 할당 (BAR1에 매핑된 VRAM) */
	check_cudaruntimecall(cudaMalloc(&o->cu_mem_ptr, total_mem), rc);
	if (rc != 0)
		goto exit_error;
	/* [한국어] 초기값 0xab로 채움 — 아직 쓰지 않은 영역의 read-before-write 검증 용이 */
	check_cudaruntimecall(cudaMemset(o->cu_mem_ptr, 0xab, total_mem), rc);
	if (rc != 0)
		goto exit_error;

	if (o->cuda_io == IO_CUFILE) {
		/* [한국어] GPU 버퍼를 nvidia-fs에 pin — DMA 매핑 등록.
		 *          이 호출이 GDS의 핵심: NVMe 컨트롤러의 PRP 엔트리를 GPU BAR1의 물리 주소로
		 *          채울 수 있게 해준다. 등록이 없으면 cuFile API가 bounce 버퍼를 거친다. */
		status = cuFileBufRegister(o->cu_mem_ptr, total_mem, 0);
		if (status.err != CU_FILE_SUCCESS) {
			log_err("cuFileBufRegister: err=%d:%s\n", status.err,
				fio_libcufile_get_cuda_error(status));
			goto exit_error;
		}
	}

	return 0;

exit_error:
	/* [한국어] 부분 할당 해제 — 역순 정리 */
	if (td->orig_buffer) {
		free(td->orig_buffer);
		td->orig_buffer = NULL;
	}
	if (o->junk_buf) {
		free(o->junk_buf);
		o->junk_buf = NULL;
	}
	if (o->cu_mem_ptr) {
		cudaFree(o->cu_mem_ptr);
		o->cu_mem_ptr = NULL;
	}
	return 1;
}

/*
 * [한국어]
 * fio_libcufile_iomem_free - iomem_alloc의 역동작.
 *
 * @td: 잡 thread_data.
 *
 * cuFileBufDeregister로 DMA 매핑 해제 → cudaFree로 GPU 메모리 반환 → 호스트 버퍼 해제.
 * 실행 컨텍스트: 잡 종료 단계.
 *
 * 호출 체인: backend.c free_buffers → td_io_iomem_free → [fio_libcufile_iomem_free]
 */
static void fio_libcufile_iomem_free(struct thread_data *td)
{
	struct libcufile_options *o = td->eo;

	if (o->junk_buf) {
		free(o->junk_buf);
		o->junk_buf = NULL;
	}
	if (o->cu_mem_ptr) {
		if (o->cuda_io == IO_CUFILE)
			cuFileBufDeregister(o->cu_mem_ptr); /* [한국어] DMA 매핑 해제 (먼저) */
		cudaFree(o->cu_mem_ptr);                 /* [한국어] GPU 메모리 반환 */
		o->cu_mem_ptr = NULL;
	}
	if (td->orig_buffer) {
		free(td->orig_buffer);                    /* [한국어] fio 더미 호스트 버퍼 해제 */
		td->orig_buffer = NULL;
	}
}

/*
 * [한국어]
 * fio_libcufile_cleanup - 엔진 종료: 마지막 잡이 드라이버를 닫는다.
 *
 * @td: 잡 thread_data.
 *
 * running_lock 하에 running을 감소시키고, 0이 되면 cuFileDriverClose()로 nvidia-fs 드라이버를 닫는다.
 * 실행 컨텍스트: 잡 스레드 종료 직전.
 *
 * 호출 체인: backend.c cleanup_td → td_io_cleanup → [fio_libcufile_cleanup]
 */
static void fio_libcufile_cleanup(struct thread_data *td)
{
	struct libcufile_options *o = td->eo;

	pthread_mutex_lock(&running_lock);      /* [한국어] refcount 보호 */
	running--;                                /* [한국어] 내 참여 해제 */
	assert(running >= 0);                    /* [한국어] 음수면 버그 */
	if (running == 0) {                      /* [한국어] 마지막 잡 */
		/* only close the driver if initialized and
		   this is the last worker thread */
		if (o->cuda_io == IO_CUFILE && cufile_initialized)
			cuFileDriverClose();         /* [한국어] nvidia-fs 드라이버 종료 (IOCTL) */
		cufile_initialized = 0;
	}
	pthread_mutex_unlock(&running_lock);
}

/*
 * [한국어] ioengine_ops — fio 코어가 엔진 기능을 호출하는 콜백 테이블.
 *                         FIO_STATIC으로 내부 연결만 허용 (외부 이름 충돌 방지).
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name                = "libcufile",                       /* [한국어] --ioengine=libcufile로 선택 */
	.version             = FIO_IOOPS_VERSION,                 /* [한국어] ABI 버전 호환 체크 */
	.init                = fio_libcufile_init,                /* [한국어] 잡 시작 시 */
	.queue               = fio_libcufile_queue,               /* [한국어] 각 io_u 제출 */
	.get_file_size       = generic_get_file_size,             /* [한국어] 기본 stat 기반 크기 획득 */
	.open_file           = fio_libcufile_open_file,           /* [한국어] 파일 + cuFile 핸들 등록 */
	.close_file          = fio_libcufile_close_file,          /* [한국어] 핸들 해제 + close */
	.iomem_alloc         = fio_libcufile_iomem_alloc,         /* [한국어] GPU 버퍼 할당 */
	.iomem_free          = fio_libcufile_iomem_free,          /* [한국어] GPU 버퍼 해제 */
	.cleanup             = fio_libcufile_cleanup,             /* [한국어] 잡 종료 시 드라이버 refcount-- */
	.flags               = FIO_SYNCIO,                         /* [한국어] 동기 — queue가 즉시 완료 반환 */
	.options             = options,                            /* [한국어] 엔진별 옵션 배열 */
	.option_struct_size  = sizeof(struct libcufile_options)   /* [한국어] td->eo 크기 */
};

/*
 * [한국어]
 * fio_libcufile_register - 프로세스 초기화 시 자동 호출되어 엔진을 fio 코어에 등록.
 *
 * fio_init 속성(=constructor)으로 main 실행 이전에 register_ioengine이 호출되어 engine_list에
 * 추가된다. 이후 --ioengine=libcufile 지정 시 load_ioengine가 이 엔트리를 찾는다.
 */
void fio_init fio_libcufile_register(void)
{
	register_ioengine(&ioengine);
}

/*
 * [한국어]
 * fio_libcufile_unregister - 프로세스 종료 시 자동 호출되어 엔진을 해제.
 *
 * fio_exit 속성(=destructor)으로 atexit 체인에서 호출된다.
 */
void fio_exit fio_libcufile_unregister(void)
{
	unregister_ioengine(&ioengine);
}
