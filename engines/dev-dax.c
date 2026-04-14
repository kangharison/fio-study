/*
 * device DAX engine
 *
 * IO engine that reads/writes from files by doing memcpy to/from
 * a memory mapped region of DAX enabled device.
 *
 * Copyright (C) 2016 Intel Corp
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation..
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

/*
 * [한국어 설명] dev-dax I/O 엔진 구현 (dev-dax.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Linux "Device DAX"(/dev/daxN.N) 캐릭터 디바이스를 통해 영속 메모리
 * (PMEM, Persistent Memory)에 바이트 단위로 직접 접근하는 fio I/O 엔진을 구현한다.
 * DAX(Direct Access)는 파일/디바이스 내용을 커널 페이지 캐시에 복사하지 않고,
 * NVDIMM/Optane 등 PMEM 물리 주소를 프로세스 주소 공간에 그대로 매핑하는 Linux
 * 서브시스템이다. 따라서 mmap(2)로 얻은 가상 주소에 대한 load/store는 곧바로 PMEM
 * 셀에 도달하며, VFS read/write 호출이나 buffer cache copy-up이 개입하지 않는다.
 * 읽기는 일반 memcpy()로 충분하지만, 쓰기는 CPU 캐시 계층(L1/L2/LLC)에 머물러
 * 전원 차단 시 손실될 수 있으므로 pmem_memcpy_persist()가 내부적으로 CLFLUSHOPT
 * 또는 CLWB(캐시라인 write-back) + SFENCE(store fence)를 발행하여 ADR(Asynchronous
 * DRAM Refresh) 도메인까지 내려보내 영속성(durability)을 보장한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 동기(synchronous) 엔진 경로에 속한다 (.flags에 FIO_SYNCIO). 엔진 콜백 순서는
 * 표준 플러그인 계약을 따른다:
 *   td_io_init()       → fio_devdax_init()       (블록 크기/정렬 검증)
 *   td_io_open_file()  → fio_devdax_open_file()  (fd open + 엔진별 상태 할당)
 *   td_io_get_file_size → fio_devdax_get_file_size (sysfs에서 DAX 크기 조회)
 *   td_io_prep()       → fio_devdax_prep()       (mmap + io_u->mmap_data 설정)
 *   td_io_queue()      → fio_devdax_queue()      (memcpy/pmem_memcpy_persist)
 *   td_io_close_file() → fio_devdax_close_file() (엔진 상태 해제 + fd close)
 * queue는 즉시 FIO_Q_COMPLETED를 반환하여 io_u 생명주기가
 * free → prepped → in_flight → completed → free 로 한 틱 안에 마감된다.
 * 실행 컨텍스트는 잡 스레드(유저스페이스)이며, 데이터 경로는 커널을 거의
 * 거치지 않고 CPU MMU 매핑을 통한 직접 로드/스토어로 진행된다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위(호출자): ioengines.c의 td_io_* 래퍼들 (backend.c의 do_io 루프가 구동).
 * - 하위(피호출): mmap(2)/munmap(2), libpmem(pmem_memcpy_persist),
 *   realpath(3)/fopen(3)/fscanf(3)/stat(2)로 sysfs 크기 조회, generic_open_file/
 *   generic_close_file(fio 공용 파일 open/close 래퍼).
 * - 옵션 그룹: FIO_OPT_C_ENGINE/FIO_OPT_G_DEV_DAX (이 파일에서는 별도 옵션 미정의).
 * - 공유 자료구조:
 *    * struct fio_file: fd, real_file_size, file_offset, io_size, filetype,
 *      engine_data(=FILE_ENG_DATA) 필드를 통해 코어와 창구 공유.
 *    * struct io_u: xfer_buf/xfer_buflen/offset/ddir/mmap_data를 통해 데이터 전송
 *      요청과 결과를 전달.
 *    * page_size/page_mask: 전역 변수 — fsync 경로에서 페이지 정렬 강제용.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_devdax_init(): rw_min_bs가 페이지 정렬되지 않았는데 fsync/fdatasync가
 *   요구되면 거절(mmap + msync 조합의 최소 단위가 페이지이기 때문).
 * - fio_devdax_file(): 실제 mmap 호출부. 잡 방향(read/write/rw/verify)에 따라
 *   PROT_READ/PROT_WRITE를 결정하고 MAP_SHARED로 매핑.
 * - fio_devdax_prep_full()/prep_limited(): 전체 매핑 시도 → 실패 시 1GiB 제한
 *   부분 매핑으로 폴백. MMAP_TOTAL_SZ로 상한을 둬 거대 DAX 장치에서도 VA 공간을
 *   전부 태우지 않는다.
 * - fio_devdax_prep(): 현재 io_u가 기존 매핑 범위 안이면 재사용, 아니면 munmap
 *   후 재매핑하고 io_u->mmap_data를 해당 오프셋으로 설정.
 * - fio_devdax_queue(): READ=memcpy, WRITE=pmem_memcpy_persist, SYNC=no-op.
 * - fio_devdax_get_file_size(): /sys/dev/char/<M>:<m>/subsystem 링크로 DAX임을
 *   확인하고 .../size에서 장치 크기를 파싱.
 * - fio_devdax_open_file()/close_file(): generic_open_file + fio_devdax_data 할당.
 * - struct fio_devdax_data: 현재 매핑된 (ptr, sz, off) 3-튜플을 보관해 재매핑
 *   필요 여부 판단에 사용.
 */

/*
 * device dax engine
 * IO engine that access a DAX device directly for read and write data
 *
 * To use:
 *   ioengine=dev-dax
 *
 *   Other relevant settings:
 *     iodepth=1
 *     direct=0	   REQUIRED
 *     filename=/dev/daxN.N
 *     bs=2m
 *
 *     direct should be left to 0. Using dev-dax implies that memory access
 *     is direct. However, dev-dax does not support O_DIRECT flag by design
 *     since it is not necessary.
 *
 *     bs should adhere to the device dax alignment at minimally.
 *
 * libpmem.so
 *   By default, the dev-dax engine will let the system find the libpmem.so
 *   that it uses. You can use an alternative libpmem by setting the
 *   FIO_PMEM_LIB environment variable to the full path to the desired
 *   libpmem.so.
 */

#include <stdio.h>           /* [한국어] FILE*, fopen/fscanf/fclose — sysfs 크기 파일 파싱에 사용. */
#include <limits.h>          /* [한국어] PATH_MAX — /sys 경로 버퍼 크기 상한. */
#include <stdlib.h>          /* [한국어] calloc/free — 엔진별 상태(fio_devdax_data) 할당. */
#include <unistd.h>          /* [한국어] 표준 유닉스 API(close 등). mmap 경로의 fd life cycle 용. */
#include <errno.h>           /* [한국어] errno 전역 — mmap/stat/realpath 실패 시 에러 코드 전달. */
#include <sys/mman.h>        /* [한국어] mmap(2)/munmap(2) 및 PROT_*/MAP_* 상수 — DAX 매핑의 핵심 API. */
#include <sys/stat.h>        /* [한국어] stat(2)과 struct stat.st_rdev — 캐릭터 디바이스 major/minor 추출. */
#include <sys/sysmacros.h>   /* [한국어] major()/minor() 매크로 — st_rdev에서 번호 디코딩. */
#include <libgen.h>          /* [한국어] basename(3) 계열(이 파일은 strrchr 사용) — 경로 파싱 호환 헤더. */
#include <libpmem.h>         /* [한국어] PMDK의 libpmem — pmem_memcpy_persist()가 CLWB+SFENCE로 영속 기록. */

#include "../fio.h"          /* [한국어] fio 코어 타입: thread_data, fio_file, io_u, ioengine_ops 등. */
#include "../verify.h"       /* [한국어] VERIFY_NONE 등 검증 모드 상수 — 쓰기 시 PROT_READ 추가 결정에 사용. */

/*
 * Limits us to 1GiB of mapped files in total to model after
 * mmap engine behavior
 */
/* [한국어] 한 번에 매핑할 수 있는 최대 크기(1 GiB). DAX 장치는 수백 GiB에 달할 수
 * 있어 전체 매핑 시 32비트 시스템이나 VA 공간이 빈약한 환경에서 실패할 수 있다.
 * mmap.c 엔진과 동일한 상한을 두어 동작 특성을 맞춘다. UL 접미사로 unsigned long
 * 승격해 32비트 곱셈 오버플로를 방지. */
#define MMAP_TOTAL_SZ	(1 * 1024 * 1024 * 1024UL)

/* [한국어] DAX 장치별 매핑 상태를 저장하는 구조체 (mmap.c의 fio_mmap_data와 유사).
 * 각 fio_file의 engine_data 슬롯(FILE_SET_ENG_DATA/FILE_ENG_DATA)을 통해 연결되며,
 * open_file에서 할당하고 close_file에서 해제한다. 한 잡 스레드당 단일 소유이므로
 * 락은 필요 없다. */
struct fio_devdax_data {
	void *devdax_ptr;
	/* [한국어] mmap()이 반환한 매핑 기준 가상 주소.
	 * 설정자: fio_devdax_file()의 mmap 호출.
	 * 읽는 자: fio_devdax_prep()가 io_u->mmap_data 계산에 사용, fio_devdax_queue의
	 *   memcpy/pmem_memcpy_persist가 최종 주소를 참조.
	 * 값 범위: 유효 매핑 주소 또는 NULL(매핑 해제 상태). MAP_FAILED는 저장되지 않음.
	 * 동기화: 잡 스레드 단독 사용, 별도 락 불요. */

	size_t devdax_sz;
	/* [한국어] 현재 매핑된 길이(바이트).
	 * 설정자: prep_full이면 f->io_size, prep_limited이면 min(MMAP_TOTAL_SZ, ...).
	 * 읽는 자: prep의 재사용 판정(io_u가 기존 매핑 범위 내인지), munmap 길이 인자.
	 * 값 범위: 0(매핑 없음) ~ MMAP_TOTAL_SZ. 페이지 단위로 round up될 수 있음. */

	off_t devdax_off;
	/* [한국어] DAX 장치 내에서 매핑 시작 오프셋(바이트).
	 * 설정자: prep 단계에서 io_u->offset 또는 0.
	 * 읽는 자: 재사용 판정과 io_u->mmap_data 계산식의 기준점.
	 * 값 범위: [0, real_file_size) 이내. 장치 얼라인먼트에 맞아야 mmap 성공. */
};

/*
 * [한국어]
 * fio_devdax_file - DAX 장치의 지정 영역을 mmap으로 잡 주소 공간에 매핑한다.
 *
 * @td:     현재 잡의 thread_data. td->o.verify, td_rw/td_write 판정에 사용.
 * @f:      대상 fio_file. f->fd는 이미 generic_open_file에서 열려 있어야 함.
 * @length: 매핑할 바이트 수. 보통 fdd->devdax_sz.
 * @off:    DAX 장치 내 시작 오프셋. 장치 얼라인먼트(보통 2MiB)에 맞아야 한다.
 * @return: 0=성공, 그 외=td->error로 설정된 errno 기반 에러.
 *
 * 왜 필요한가: DAX 장치는 read/write 시스템콜도 지원하지만, 영속 메모리의 본래
 * 강점(바이트 단위 직접 접근, 캐시라인 플러시로 영속성 보장)을 살리려면 mmap이
 * 필수다. MAP_SHARED 플래그는 쓰기가 즉시 장치에 반영되도록 한다(MAP_PRIVATE면
 * COW로 익명 페이지에 쓰여 영속성이 사라진다).
 *
 * 동작 단계:
 *   1. 잡 방향별 PROT 플래그 결정 (rw면 RW, verify 있는 write면 R+W).
 *   2. mmap(NULL, length, flags, MAP_SHARED, fd, off)로 커널에 매핑 요청.
 *      커널은 dax_iomap_* 경로를 통해 PTE에 PMEM 물리 주소를 직접 설치
 *      (page cache 미경유). 따라서 이후 load/store는 NVDIMM을 직접 접근.
 *   3. MAP_FAILED면 td_verror로 errno 기록 후 포인터를 NULL로 정돈.
 *   4. 이전 에러가 보존되어 있고 매핑은 성공했다면, 매핑을 즉시 되돌림(누수 방지).
 *
 * 실행 컨텍스트: 잡 스레드 유저스페이스. 재진입 없음(한 파일당 한 매핑).
 * 호출 체인: fio_devdax_prep_full/limited → [이 함수] → mmap(2) → 커널 DAX 드라이버.
 */
static int fio_devdax_file(struct thread_data *td, struct fio_file *f,
			   size_t length, off_t off)
{
	struct fio_devdax_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 파일에 붙은 엔진 상태 꺼내기(open_file에서 할당된 포인터). */
	int flags = 0;                                   /* [한국어] mmap의 PROT_* 누적 플래그 — 아래 분기로 결정. */

	if (td_rw(td))                                   /* [한국어] read+write 혼합 잡(rw/randrw)인가? — 양쪽 접근 모두 필요. */
		flags = PROT_READ | PROT_WRITE;          /* [한국어] 읽기+쓰기 모두 허용: load와 store 둘 다 수행 가능. */
	else if (td_write(td)) {                         /* [한국어] write 전용 잡인 경우. */
		flags = PROT_WRITE;                      /* [한국어] 기본은 쓰기 권한만. 일반 write-only는 load 불요. */

		if (td->o.verify != VERIFY_NONE)         /* [한국어] verify 옵션이 켜져 있나? 검증은 쓴 뒤 다시 읽어 CRC/해시 비교. */
			flags |= PROT_READ;              /* [한국어] 검증 때문에 readback이 필요하므로 READ 권한 추가. */
	} else                                            /* [한국어] read 전용(또는 trim 등 비쓰기) 잡. */
		flags = PROT_READ;                       /* [한국어] 읽기 권한만 부여 — SIGSEGV 발생으로 우발적 쓰기 탐지. */

	/* [한국어] 실제 매핑 호출. addr=NULL → 커널이 자유롭게 VA 할당. MAP_SHARED →
	 * 쓰기가 즉시 장치에 반영(MAP_PRIVATE는 COW라 영속성 X). fd는 /dev/daxN.N.
	 * 커널은 DAX-aware 경로(dax_iomap_fault 등)로 PFN을 PTE에 직접 매핑한다. */
	fdd->devdax_ptr = mmap(NULL, length, flags, MAP_SHARED, f->fd, off);
	if (fdd->devdax_ptr == MAP_FAILED) {             /* [한국어] mmap 실패 시 특수 반환값 체크 — ((void*)-1). */
		fdd->devdax_ptr = NULL;                  /* [한국어] MAP_FAILED를 그대로 두면 이후 NULL 비교 로직이 오동작하므로 정돈. */
		td_verror(td, errno, "mmap");            /* [한국어] td->error에 errno 기록 + 로그 출력(호출자가 종료 판정에 사용). */
	}

	if (td->error && fdd->devdax_ptr)                /* [한국어] 레이스: 에러가 이미 있는데 매핑은 성공한 경우. */
		munmap(fdd->devdax_ptr, length);         /* [한국어] 누수 방지로 즉시 해제. munmap은 커널 PTE 제거 + VMA 회수. */

	return td->error;                                /* [한국어] 0이면 성공, 아니면 에러 전파. */
}

/*
 * Just mmap an appropriate portion, we cannot mmap the full extent
 */
/*
 * [한국어]
 * fio_devdax_prep_limited - 전체 매핑이 불가능할 때 현재 io_u 영역만 제한 매핑.
 *
 * @td:   잡 컨텍스트.
 * @io_u: 준비 중인 I/O 유닛(offset/buflen 참조).
 * @return: 0=성공, EIO/기타=실패.
 *
 * prep_full이 EINVAL로 폴백되면 호출된다. MMAP_TOTAL_SZ(1GiB)와 파일 크기,
 * io_size의 최솟값으로 매핑 크기를 제한해 VA 공간 과소비를 방지한다.
 * 호출 체인: fio_devdax_prep → [이 함수] → fio_devdax_file → mmap.
 */
static int fio_devdax_prep_limited(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;                 /* [한국어] 이 io_u가 대상으로 하는 파일 포인터. */
	struct fio_devdax_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 엔진별 파일 상태. */

	if (io_u->buflen > f->real_file_size) {          /* [한국어] 요청 블록이 장치 크기보다 크면 물리적으로 불가. */
		log_err("dev-dax: bs too big for dev-dax engine\n");
		return EIO;                              /* [한국어] EIO로 상위에 보고 — I/O 에러 카테고리. */
	}

	/* [한국어] 매핑 크기 = min(1GiB 상한, 파일 실제 크기). 보수적 상한. */
	fdd->devdax_sz = min(MMAP_TOTAL_SZ, f->real_file_size);
	if (fdd->devdax_sz > f->io_size)                 /* [한국어] 잡이 실제로 사용할 io_size보다 클 필요 없음. */
		fdd->devdax_sz = f->io_size;             /* [한국어] io_size로 더 줄여 메모리 절약. */

	fdd->devdax_off = io_u->offset;                  /* [한국어] 현재 io_u 오프셋을 매핑 시작점으로 채택. */

	/* [한국어] 실제 mmap 실행. 성공 시 fdd->devdax_ptr이 유효 주소가 됨. */
	return fio_devdax_file(td, f, fdd->devdax_sz, fdd->devdax_off);
}

/*
 * Attempt to mmap the entire file
 */
/*
 * [한국어]
 * fio_devdax_prep_full - DAX 장치 전체를 한 번에 매핑 시도.
 *
 * @td:   잡 컨텍스트.
 * @io_u: 현재 io_u (offset/file_size 검증용).
 * @return: 0=성공, EINVAL=불가(부분 매핑으로 폴백 필요).
 *
 * size_t 캐스팅으로 off_t → size_t 절삭이 생기지 않는지 검사하여 64→32 축소
 * 오버플로 시 부분 매핑 경로로 폴백하도록 부분 매핑 플래그를 세운다.
 * 호출 체인: fio_devdax_prep → [이 함수] → fio_devdax_file.
 */
static int fio_devdax_prep_full(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;                 /* [한국어] 대상 파일. */
	struct fio_devdax_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 엔진 상태. */
	int ret;                                         /* [한국어] fio_devdax_file의 반환값 저장. */

	if (fio_file_partial_mmap(f))                    /* [한국어] 이전에 부분 매핑으로 전환되었는가? */
		return EINVAL;                           /* [한국어] 이미 부분 모드면 full 재시도 금지 — 바로 폴백. */

	/* [한국어] off_t/uint64 → size_t 변환이 비손실인지 확인. 32비트 size_t 환경에서
	 * 2GiB 초과 시 캐스팅 손실이 발생하므로 early out. */
	if (io_u->offset != (size_t) io_u->offset ||
	    f->io_size != (size_t) f->io_size) {
		fio_file_set_partial_mmap(f);            /* [한국어] 이후 항상 부분 매핑 경로를 쓰도록 플래그 설정. */
		return EINVAL;                           /* [한국어] 폴백 요청. */
	}

	fdd->devdax_sz = f->io_size;                     /* [한국어] 전체 io_size를 매핑 크기로. */
	fdd->devdax_off = 0;                             /* [한국어] 파일 시작(0 오프셋)부터 매핑. */

	ret = fio_devdax_file(td, f, fdd->devdax_sz, fdd->devdax_off);  /* [한국어] 실제 mmap. */
	if (ret)                                         /* [한국어] 전체 매핑이 실패했다면? */
		fio_file_set_partial_mmap(f);            /* [한국어] 다음부터는 부분 매핑만 시도하도록 마킹. */

	return ret;                                      /* [한국어] 성공 0, 실패 시 errno 기반 값. */
}

/*
 * [한국어]
 * fio_devdax_prep - 엔진 계약의 .prep 콜백 구현.
 *
 * @td:   잡 컨텍스트.
 * @io_u: 준비 대상 I/O 유닛.
 * @return: 0=성공, errno/EINVAL 등=실패.
 *
 * 현재 매핑이 io_u 범위를 포함하면 재사용하고, 아니면 기존 매핑을 풀고
 * 재매핑한다. 완료 후 io_u->mmap_data를 해당 바이트 오프셋으로 설정하여
 * queue 콜백이 곧바로 memcpy/pmem_memcpy_persist를 수행할 수 있게 한다.
 * 호출 체인: td_io_prep → [이 함수] → prep_full/limited → fio_devdax_file → mmap.
 */
static int fio_devdax_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;                 /* [한국어] 대상 파일. */
	struct fio_devdax_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 엔진 상태. */
	int ret;                                         /* [한국어] prep_limited 반환 보관. */

	/*
	 * It fits within existing mapping, use it
	 */
	/* [한국어] 재매핑 회피 최적화: 이번 io_u의 [offset, offset+buflen) 구간이
	 * 기존 매핑 [devdax_off, devdax_off+devdax_sz) 안에 완전히 들어가면
	 * 기존 포인터를 그대로 재사용 — mmap/munmap 시스템콜 비용 제거. */
	if (io_u->offset >= fdd->devdax_off &&
	    io_u->offset + io_u->buflen <= fdd->devdax_off + fdd->devdax_sz)
		goto done;

	/*
	 * unmap any existing mapping
	 */
	if (fdd->devdax_ptr) {                           /* [한국어] 유효 매핑이 남아 있으면 해제부터. */
		if (munmap(fdd->devdax_ptr, fdd->devdax_sz) < 0)  /* [한국어] 커널 VMA/PTE 제거. 실패 시 errno. */
			return errno;                    /* [한국어] 즉시 에러 반환 — 호출자가 td_verror 처리. */
		fdd->devdax_ptr = NULL;                  /* [한국어] 이중 해제 방지용 null 화. */
	}

	if (fio_devdax_prep_full(td, io_u)) {            /* [한국어] 우선 전체 매핑 시도. */
		td_clear_error(td);                      /* [한국어] prep_full이 남긴 td->error를 지움(폴백은 정상 시나리오). */
		ret = fio_devdax_prep_limited(td, io_u); /* [한국어] 실패 시 부분(1GiB 창) 매핑으로 폴백. */
		if (ret)                                 /* [한국어] 부분 매핑마저 실패? */
			return ret;                      /* [한국어] 더 이상 방법 없음 — 에러 반환. */
	}

done:
	/* [한국어] io_u가 DAX 메모리에서 실제 읽/쓸 바이트 주소를 계산:
	 *   base(devdax_ptr) + (io_u->offset - devdax_off) - file_offset.
	 * - devdax_off는 장치 내 매핑 시작, file_offset은 잡 옵션이 정한 파일 내 상대
	 *   시작점. io_u->offset은 file_offset 기준이 아닌 장치 절대값일 수 있어 이렇게
	 *   보정해야 올바른 캐시라인에 도달한다. */
	io_u->mmap_data = fdd->devdax_ptr + io_u->offset - fdd->devdax_off -
				f->file_offset;
	return 0;                                        /* [한국어] 성공. queue에서 이 mmap_data를 사용. */
}

/*
 * [한국어]
 * fio_devdax_queue - dev-dax 엔진의 .queue 콜백 (동기 I/O).
 *
 * @td:   잡 컨텍스트. fio_ro_check로 read-only 모드 위반 여부 확인.
 * @io_u: 실행할 I/O 유닛. ddir/xfer_buf/xfer_buflen/mmap_data 사용.
 * @return: 항상 FIO_Q_COMPLETED (동기 완료). io_u->error로 세부 에러 전달.
 *
 * READ:  memcpy(io_u->xfer_buf, io_u->mmap_data, len) — DAX 메모리에서 잡 버퍼로
 *        일반 load. 페이지 캐시가 없으므로 load는 곧바로 PMEM에서 반환된다.
 * WRITE: pmem_memcpy_persist(dst, src, len) — libpmem이 내부적으로
 *        1) 비임시(non-temporal) store(MOVNT) 또는 일반 store + CLWB(캐시라인
 *           write-back, 라인은 유효 상태로 남김) / CLFLUSHOPT(플러시+무효화)로
 *           CPU 캐시에서 DIMM 방향으로 내려보내고,
 *        2) SFENCE로 이전 store들의 순서를 고정해
 *        ADR(Asynchronous DRAM Refresh) 도메인까지 도달시킴으로써 전원 손실
 *        후에도 값이 보존되도록 보장한다.
 * SYNC/DATASYNC/SYNC_FILE_RANGE: DAX는 persist 단위가 이미 명령당 보장되므로
 *        추가 동기화 불요 — no-op.
 * 기타 ddir: EINVAL로 거절.
 *
 * 실행 컨텍스트: 잡 스레드. 동기 엔진이므로 즉시 완료 반환.
 * 호출 체인: td_io_queue() → [이 함수] → memcpy/pmem_memcpy_persist.
 */
static enum fio_q_status fio_devdax_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	fio_ro_check(td, io_u);                          /* [한국어] read-only 잡에서 WRITE 요청이면 assert — 안전성 보호. */
	io_u->error = 0;                                 /* [한국어] 이전 시도 잔여 에러 초기화. */

	switch (io_u->ddir) {                            /* [한국어] 데이터 방향(DDIR_*)에 따라 분기. */
	case DDIR_READ:
		/* [한국어] DAX 주소 → 잡 버퍼로 일반 memcpy. 페이지 캐시 미개입 →
		 * 실제 PMEM 매체에서 CPU로 직접 로드. libc가 제공하는 최적화된 memcpy는
		 * 큰 블록에 대해 AVX/MOVNTDQA 등을 활용할 수 있다. */
		memcpy(io_u->xfer_buf, io_u->mmap_data, io_u->xfer_buflen);
		break;
	case DDIR_WRITE:
		/* [한국어] PMDK의 영속 쓰기. 내부 구현(하드웨어 기능별):
		 * - CLWB 지원 CPU: store → CLWB(라인을 DIMM으로 write-back, L1에 유지)
		 *   → SFENCE(이전 store 순서 확정).
		 * - CLFLUSHOPT만 지원: store → CLFLUSHOPT(라인 무효화하며 write-back)
		 *   → SFENCE.
		 * - eADR 플랫폼: 캐시 자체가 전원 도메인이라 플러시 생략 가능.
		 * 호출이 반환하면 io_u->xfer_buflen 바이트가 ADR/eADR 경계를 넘어
		 * 영속 매체에 안착한 것으로 간주된다(NVMe/디스크의 fsync와 동일 보장). */
		pmem_memcpy_persist(io_u->mmap_data, io_u->xfer_buf,
				    io_u->xfer_buflen);
		break;
	case DDIR_SYNC:                                  /* [한국어] fsync 요청 — DAX는 매 store가 persist라 no-op. */
	case DDIR_DATASYNC:                              /* [한국어] fdatasync 요청 — 동일 이유로 no-op. */
	case DDIR_SYNC_FILE_RANGE:                       /* [한국어] 범위 sync — DAX는 의미 없음, no-op. */
		break;
	default:
		io_u->error = EINVAL;                    /* [한국어] TRIM 등 미지원 방향은 EINVAL로 거절. */
		break;
	}

	return FIO_Q_COMPLETED;                          /* [한국어] 동기 완료 — 상위는 즉시 결과 수집 가능. */
}

/*
 * [한국어]
 * fio_devdax_init - 엔진 .init 콜백. 블록 크기와 fsync 옵션의 정합성 검증.
 *
 * @td:     잡 컨텍스트.
 * @return: 0=성공, 1=구성 오류로 잡 거절.
 *
 * 왜 필요한가: fsync/fdatasync_blocks 옵션이 설정되면 주기적으로 페이지 단위
 * 동기화가 요구되는데, rw_min_bs가 페이지 크기에 정렬되지 않으면 mmap 기반
 * 동기화 단위(페이지)와 불일치하여 의미가 왜곡된다. 이를 사전에 막는다.
 * 실행 컨텍스트: 잡 스레드 초기화 단계(데이터 I/O 시작 전).
 * 호출 체인: td_io_init → [이 함수].
 */
static int fio_devdax_init(struct thread_data *td)
{
	struct thread_options *o = &td->o;               /* [한국어] 잡 옵션 꺼내기 — rw_min_bs 등 참조. */

	/* [한국어] 블록 크기가 페이지 얼라인되지 않았는데(fsync 단위 < bs) fsync 요구가
	 * 있으면 설정 오류. page_mask는 전역(예: 4095). */
	if ((o->rw_min_bs & page_mask) &&
	    (o->fsync_blocks || o->fdatasync_blocks)) {
		log_err("dev-dax: mmap options dictate a minimum block size of %llu bytes\n",
			(unsigned long long) page_size);
		return 1;                                /* [한국어] 1 반환 → 잡 거절. */
	}

	return 0;                                        /* [한국어] 검증 통과. */
}

/*
 * [한국어]
 * fio_devdax_open_file - .open_file 콜백. generic open 후 엔진 상태 할당.
 *
 * @td:     잡 컨텍스트.
 * @f:      열 대상 파일(보통 /dev/daxN.N).
 * @return: 0=성공, 1=실패(close 후 에러 전파).
 *
 * generic_open_file이 O_RDONLY/O_RDWR로 fd를 확보하고, 이어서 fio_devdax_data를
 * calloc으로 0 초기화해 파일에 붙인다. 할당 실패 시 fd까지 되돌린다.
 * 호출 체인: td_io_open_file → [이 함수] → generic_open_file(→ open(2)) + calloc.
 */
static int fio_devdax_open_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_devdax_data *fdd;                     /* [한국어] 새로 할당할 엔진 상태. */
	int ret;                                         /* [한국어] generic_open_file 결과 보관. */

	ret = generic_open_file(td, f);                  /* [한국어] fio 공용 open(호출 내부에서 open(2) 수행, f->fd 세팅). */
	if (ret)                                         /* [한국어] open 실패? */
		return ret;                              /* [한국어] 그대로 에러 전파 — 엔진 상태 할당 불요. */

	fdd = calloc(1, sizeof(*fdd));                   /* [한국어] 0-초기화 할당: devdax_ptr=NULL, sz=0, off=0. */
	if (!fdd) {                                      /* [한국어] 메모리 부족? */
		int fio_unused __ret;                    /* [한국어] 아래 호출 결과가 쓰이지 않음을 컴파일러에 알림. */
		__ret = generic_close_file(td, f);       /* [한국어] 앞서 연 fd를 되돌려 누수 방지. */
		return 1;                                /* [한국어] 1 반환 → 호출자 open 실패로 처리. */
	}

	FILE_SET_ENG_DATA(f, fdd);                       /* [한국어] f->engine_data에 상태 포인터 장착. */

	return 0;                                        /* [한국어] 성공. */
}

/*
 * [한국어]
 * fio_devdax_close_file - .close_file 콜백. 엔진 상태 해제 후 generic close.
 *
 * @td:     잡 컨텍스트.
 * @f:      닫을 파일.
 * @return: generic_close_file 결과(0=성공).
 *
 * 호출 체인: td_io_close_file → [이 함수] → free + generic_close_file(→ close(2)).
 * 주의: 매핑이 prep에서 살아있다면 이미 prep 교체 시점에 munmap되어 있어야
 * 하며, 그렇지 않은 경우 프로세스 종료 시 커널이 정리한다.
 */
static int fio_devdax_close_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_devdax_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 엔진 상태 획득. */

	FILE_SET_ENG_DATA(f, NULL);                      /* [한국어] 댕글링 방지로 먼저 슬롯 비움. */
	free(fdd);                                       /* [한국어] calloc 대응 해제. NULL 허용이므로 안전. */
	fio_file_clear_partial_mmap(f);                  /* [한국어] 부분 매핑 플래그 초기화 — 재open 시 full 매핑부터 재시도. */

	return generic_close_file(td, f);                /* [한국어] fd close 위임 — 커널은 VMA도 함께 정리. */
}

/*
 * [한국어]
 * fio_devdax_get_file_size - DAX 장치의 크기를 sysfs에서 조회 (.get_file_size 콜백).
 *
 * @td:     잡 컨텍스트.
 * @f:      대상 파일. filetype == FIO_TYPE_CHAR여야 함.
 * @return: 0=성공(f->real_file_size 세팅), 음수(errno) 또는 1=실패.
 *
 * 일반 파일과 달리 DAX 캐릭터 디바이스는 fstat의 st_size가 0이다. 대신
 * /sys/dev/char/<major>:<minor>/size에 바이트 크기가 적혀 있어 이를 파싱한다.
 * 동시에 /sys/dev/char/<M>:<m>/subsystem 심볼릭 링크의 타깃 basename이 "dax"인지
 * 확인해 실제 DAX 장치인지 검증한다(ext4 file 등 오용 방지).
 *
 * 호출 체인: td_io_get_file_size → [이 함수] → stat/realpath/fopen/fscanf.
 */
static int
fio_devdax_get_file_size(struct thread_data *td, struct fio_file *f)
{
	char spath[PATH_MAX];                            /* [한국어] "/sys/dev/char/M:m/..." 경로 생성 버퍼. */
	char npath[PATH_MAX];                            /* [한국어] realpath가 해석한 절대 경로 수신 버퍼. */
	char *rpath, *basename;                          /* [한국어] 각각 realpath 반환값과 경로의 마지막 구성요소. */
	FILE *sfile;                                     /* [한국어] size 파일 스트림. */
	uint64_t size;                                   /* [한국어] 파싱된 장치 크기(바이트). */
	struct stat st;                                  /* [한국어] /dev/daxN.N의 stat 결과 — st_rdev 추출 목적. */
	int rc;                                          /* [한국어] 시스템콜/라이브러리 반환 보관. */

	if (fio_file_size_known(f))                      /* [한국어] 이미 크기 확정된 파일이면 중복 조회 스킵. */
		return 0;

	if (f->filetype != FIO_TYPE_CHAR)                /* [한국어] 캐릭터 디바이스가 아니면 이 엔진 부적합. */
		return -EINVAL;

	rc = stat(f->file_name, &st);                    /* [한국어] 디바이스 노드 stat — st.st_rdev에 major:minor가 인코딩됨. */
	if (rc < 0) {                                    /* [한국어] 실패(경로 오류, 권한 등). */
		log_err("%s: failed to stat file %s (%s)\n",
			td->o.name, f->file_name, strerror(errno));
		return -errno;                           /* [한국어] 음수 errno로 실패 보고. */
	}

	/* [한국어] subsystem 링크 경로 구성: /sys/dev/char/<major>:<minor>/subsystem. */
	snprintf(spath, PATH_MAX, "/sys/dev/char/%d:%d/subsystem",
		 major(st.st_rdev), minor(st.st_rdev));

	rpath = realpath(spath, npath);                  /* [한국어] 심볼릭 링크를 절대 경로로 해석 — 실제 서브시스템 디렉토리 위치. */
	if (!rpath) {                                    /* [한국어] realpath 실패(링크 없음, 권한 등). */
		log_err("%s: realpath on %s failed (%s)\n",
			td->o.name, spath, strerror(errno));
		return -errno;
	}

	/* check if DAX device */
	/* [한국어] 해석된 경로의 마지막 구성요소가 "dax"인지 확인 — DAX 서브시스템 검증. */
	basename = strrchr(rpath, '/');                  /* [한국어] 마지막 '/' 위치. */
	if (!basename || strcmp("dax", basename+1)) {    /* [한국어] '/' 다음 토큰 비교. 불일치하면 경고. */
		log_err("%s: %s not a DAX device!\n",
			td->o.name, f->file_name);
		/* [한국어] 주의: 경고만 하고 리턴하지 않음 — 계속 size 읽기 시도. */
	}

	/* [한국어] 크기 파일 경로 재구성: /sys/dev/char/<M>:<m>/size. */
	snprintf(spath, PATH_MAX, "/sys/dev/char/%d:%d/size",
		 major(st.st_rdev), minor(st.st_rdev));

	sfile = fopen(spath, "r");                       /* [한국어] 텍스트로 읽기 모드 오픈. */
	if (!sfile) {
		log_err("%s: fopen on %s failed (%s)\n",
			td->o.name, spath, strerror(errno));
		return 1;                                /* [한국어] 양수 1 = 일반 실패. */
	}

	rc = fscanf(sfile, "%lu", &size);                /* [한국어] 10진수 unsigned long 파싱(바이트 수). */
	if (rc < 0) {                                    /* [한국어] 파싱 실패? */
		log_err("%s: fscanf on %s failed (%s)\n",
			td->o.name, spath, strerror(errno));
		fclose(sfile);                           /* [한국어] 에러 경로에서도 스트림 반드시 닫기. */
		return 1;
	}

	f->real_file_size = size;                        /* [한국어] fio 코어가 I/O 범위 계산에 쓸 크기 저장. */

	fclose(sfile);                                   /* [한국어] 정상 경로 스트림 해제. */

	if (f->file_offset > f->real_file_size) {        /* [한국어] 사용자 지정 offset이 장치 크기를 넘었는가? */
		log_err("%s: offset extends end (%llu > %llu)\n", td->o.name,
					(unsigned long long) f->file_offset,
					(unsigned long long) f->real_file_size);
		return 1;                                /* [한국어] 설정 오류로 거절. */
	}

	fio_file_set_size_known(f);                      /* [한국어] 다음 호출에서 중복 조회 피하도록 플래그 세팅. */
	return 0;                                        /* [한국어] 성공. */
}

/* [한국어] ioengine_ops: fio 엔진 플러그인 서술자. fio는 이 구조체의 콜백을
 * 통해 엔진을 호출한다. 필드 미정의(NULL)는 "해당 훅 없음"을 의미한다. */
FIO_STATIC struct ioengine_ops ioengine = {
	.name		= "dev-dax",
	/* [한국어] 잡 파일의 ioengine=dev-dax와 매칭되는 식별자. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ABI 버전 태그. fio 코어와 엔진의 구조체 레이아웃 불일치를 감지. */

	.init		= fio_devdax_init,
	/* [한국어] 잡 초기화 훅 — 옵션 정합성 검증. */

	.prep		= fio_devdax_prep,
	/* [한국어] io_u 준비 훅 — mmap 및 mmap_data 설정. */

	.queue		= fio_devdax_queue,
	/* [한국어] I/O 제출 훅 — memcpy/pmem_memcpy_persist 수행. */

	.open_file	= fio_devdax_open_file,
	/* [한국어] 파일 open 훅 — fd 확보 + 엔진 상태 할당. */

	.close_file	= fio_devdax_close_file,
	/* [한국어] 파일 close 훅 — 엔진 상태 해제 + fd 반납. */

	.get_file_size	= fio_devdax_get_file_size,
	/* [한국어] 파일 크기 조회 훅 — sysfs 기반. */

	.flags		= FIO_SYNCIO | FIO_DISKLESSIO | FIO_NOEXTEND | FIO_NODISKUTIL,
	/* [한국어] 엔진 특성 플래그 비트마스크.
	 * - FIO_SYNCIO: queue가 항상 FIO_Q_COMPLETED를 반환(동기 엔진). iodepth=1로 제한.
	 * - FIO_DISKLESSIO: 일반 블록 디바이스가 아니라 디스크 유틸 통계/경로가 불필요.
	 * - FIO_NOEXTEND: 파일 크기 확장(ftruncate) 불가 — DAX 장치는 고정 크기.
	 * - FIO_NODISKUTIL: /proc/diskstats 수집 비활성 — 캐릭터 디바이스라 불가. */
};

/*
 * [한국어]
 * fio_devdax_register - 공유 라이브러리 로드 시 자동 호출되는 등록자.
 *
 * fio_init 속성(__attribute__((constructor)))에 의해 main 전에 실행되어
 * dev-dax 엔진을 전역 엔진 리스트에 등록한다. 이후 잡 파일의 ioengine=dev-dax
 * 문자열이 이 이름과 매칭되면 본 ops가 선택된다.
 */
static void fio_init fio_devdax_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] 전역 엔진 테이블에 추가. */
}

/*
 * [한국어]
 * fio_devdax_unregister - 라이브러리 언로드 시 호출되는 해제자.
 *
 * fio_exit 속성(__attribute__((destructor)))으로 프로세스 종료 시 엔진을
 * 등록 해제한다. 누수 방지 및 반복 로드(dlopen/dlclose) 지원용.
 */
static void fio_exit fio_devdax_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] 전역 엔진 테이블에서 제거. */
}
